// memwatch: a memory line a minute, to tell a leak of the mod's from the game's own streaming (see FarCry5MemWatch.h).
#include "FarCry5MemWatch.h"
#include "FarCry5SessionLogs.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <malloc.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#pragma intrinsic(_ReturnAddress)

// ---------------------------------------------------------------------------------------------------------------------------------
// The mod's own C++ heap: every new/delete of dxgi.dll's code (std containers, strings, the mod's objects) goes through these, which
// add the block's size to two counters. MSVC routes the array, sized and nothrow forms through these two; aligned new is left alone
// (it uses _aligned_malloc, not counted). Cost: two relaxed atomic adds per allocation and per free.
namespace fc5::memwatch
{
    std::atomic<int64_t> g_heap_bytes{ 0 }, g_heap_blocks{ 0 };
}

void* operator new(size_t n)
{
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc{};
    fc5::memwatch::g_heap_bytes.fetch_add(int64_t(_msize(p)), std::memory_order_relaxed);
    fc5::memwatch::g_heap_blocks.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void operator delete(void* p) noexcept
{
    if (!p) return;
    fc5::memwatch::g_heap_bytes.fetch_sub(int64_t(_msize(p)), std::memory_order_relaxed);
    fc5::memwatch::g_heap_blocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(p);
}

void* operator new[](size_t n) { return ::operator new(n); }
void  operator delete[](void* p) noexcept { ::operator delete(p); }
void  operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void  operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

namespace fc5::memwatch
{
    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr double kMB = 1024.0 * 1024.0;

        // ---- who made a resource ------------------------------------------------------------------------------------------------
        // A module that calls the device: the mod (index 0), the game's engine (not counted) or another one (DLSS, overlays).
        struct Module
        {
            uintptr_t            lo = 0, len = 0;
            int                  kind = -1;   // 0 the mod, 1 another module (counted), -1 the game (not counted)
            char                 name[40]{};
            std::atomic<int64_t> live{ 0 }, bytes{ 0 }, staging{ 0 }, made{ 0 };
            int64_t              made_seen = 0;   // made at the last line (log thread)
        };
        constexpr int     kMods = 24;
        Module            g_mods[kMods];
        std::atomic<int>  g_nmods{ 0 };
        std::mutex        g_mod_mtx;
        uintptr_t         g_mod_lo = 0, g_mod_len = 0;          // dxgi.dll (the mod)
        uintptr_t         g_engine_lo = 0, g_engine_len = 0;    // FC_m64.dll: the game's calls leave at the first compare

        Module* add_module(uintptr_t a)
        {
            std::lock_guard _{ g_mod_mtx };
            const int n = g_nmods.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                if (a - g_mods[i].lo < g_mods[i].len) return &g_mods[i];
            if (n >= kMods) return nullptr;
            HMODULE h = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a), &h) || !h)
                return nullptr;
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return nullptr;
            char path[MAX_PATH]{};
            GetModuleFileNameA(h, path, MAX_PATH);
            const char* base = std::strrchr(path, '\\');
            base             = base ? base + 1 : path;
            Module& m        = g_mods[n];
            m.lo             = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            m.len            = mi.SizeOfImage;
            strncpy_s(m.name, base, _TRUNCATE);
            if (m.lo == g_mod_lo) m.kind = 0;
            else if (_stricmp(base, "FC_m64.dll") == 0 || _stricmp(base, "FarCry5.exe") == 0) m.kind = -1;
            else m.kind = 1;
            g_nmods.store(n + 1, std::memory_order_release);
            return &m;
        }

        // The module of a return address; nullptr for the game's own calls (not counted).
        Module* owner(const void* ra)
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(ra);
            if (a - g_engine_lo < g_engine_len) return nullptr;
            const int n = g_nmods.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                if (a - g_mods[i].lo < g_mods[i].len) return g_mods[i].kind >= 0 ? &g_mods[i] : nullptr;
            Module* m = add_module(a);
            return m && m->kind >= 0 ? m : nullptr;
        }

        // A call site of the mod's: the call and its caller's return addresses.
        struct Site
        {
            uintptr_t            ret[2]{};
            std::atomic<int64_t> live{ 0 }, bytes{ 0 }, made{ 0 };
            int64_t              live_seen = 0;   // live at the last line (log thread)
            int64_t              made_seen = 0;   // made at the last line (log thread)
            char                 last[56]{};      // the last resource made there (under g_site_mtx)
        };
        std::mutex                              g_site_mtx;
        std::unordered_map<uint64_t, Site*>     g_sites;   // never freed (a few hundred at most)

        // The tag on a counted resource: D3D holds it as private data and releases it when the resource goes.
        // {6A3C2E51-9B7D-4F0A-8C1E-5D2B7F40C3A9}
        constexpr GUID kTagGuid{ 0x6a3c2e51, 0x9b7d, 0x4f0a, { 0x8c, 0x1e, 0x5d, 0x2b, 0x7f, 0x40, 0xc3, 0xa9 } };
        class Tag final : public IUnknown
        {
        public:
            Tag(Module* m, Site* s, int64_t b, bool st) : m_mod(m), m_site(s), m_bytes(b), m_staging(st) {}
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
            {
                if (!pp) return E_POINTER;
                if (riid == __uuidof(IUnknown)) { *pp = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
                *pp = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG r = --m_ref;
                if (r == 0) {
                    if (m_armed) {
                        m_mod->live.fetch_sub(1, std::memory_order_relaxed);
                        m_mod->bytes.fetch_sub(m_bytes, std::memory_order_relaxed);
                        if (m_staging) m_mod->staging.fetch_sub(m_bytes, std::memory_order_relaxed);
                        if (m_site) {
                            m_site->live.fetch_sub(1, std::memory_order_relaxed);
                            m_site->bytes.fetch_sub(m_bytes, std::memory_order_relaxed);
                        }
                    }
                    delete this;
                }
                return r;
            }
            void arm()
            {
                m_armed = true;
                m_mod->live.fetch_add(1, std::memory_order_relaxed);
                m_mod->bytes.fetch_add(m_bytes, std::memory_order_relaxed);
                m_mod->made.fetch_add(1, std::memory_order_relaxed);
                if (m_staging) m_mod->staging.fetch_add(m_bytes, std::memory_order_relaxed);
                if (m_site) {
                    m_site->live.fetch_add(1, std::memory_order_relaxed);
                    m_site->bytes.fetch_add(m_bytes, std::memory_order_relaxed);
                    m_site->made.fetch_add(1, std::memory_order_relaxed);
                }
            }

        private:
            std::atomic<ULONG> m_ref{ 1 };
            Module*            m_mod;
            Site*              m_site;
            int64_t            m_bytes;
            bool               m_staging;
            bool               m_armed{ false };
        };

        // Bytes per 4x4 block for block-compressed formats, else bits per pixel (approximate for odd formats).
        void fmt_size(DXGI_FORMAT f, int& bits, bool& block)
        {
            block = false;
            const int v = int(f);
            if ((v >= 70 && v <= 72) || (v >= 79 && v <= 81)) { block = true; bits = 8; return; }    // BC1, BC4: 8 bytes a block
            if ((v >= 73 && v <= 78) || (v >= 82 && v <= 84) || (v >= 94 && v <= 99)) { block = true; bits = 16; return; }   // BC2/3/5/6H/7
            if (v >= 1 && v <= 4) bits = 128;
            else if (v >= 5 && v <= 8) bits = 96;
            else if (v >= 9 && v <= 22) bits = 64;
            else if (v >= 48 && v <= 59) bits = 16;
            else if (v >= 60 && v <= 65) bits = 8;
            else if (v == 66) bits = 1;
            else if (v == 85 || v == 86 || v == 115) bits = 16;
            else bits = 32;
        }

        int64_t tex_bytes(DXGI_FORMAT f, UINT w, UINT h, UINT d, UINT mips, UINT array, UINT samples)
        {
            int  bits  = 32;
            bool block = false;
            fmt_size(f, bits, block);
            if (mips == 0) {   // a full chain
                UINT m = std::max({ w, h, d }), n = 1;
                while (m > 1) { m >>= 1; ++n; }
                mips = n;
            }
            int64_t total = 0;
            for (UINT i = 0; i < mips; ++i) {
                const int64_t mw = std::max<UINT>(1u, w >> i), mh = std::max<UINT>(1u, h >> i), md = std::max<UINT>(1u, d >> i);
                total += block ? ((mw + 3) / 4) * ((mh + 3) / 4) * md * bits : (mw * mh * md * bits + 7) / 8;
            }
            return total * std::max<UINT>(1u, array) * std::max<UINT>(1u, samples);
        }

        // A resource made by a counted module: its tag, its call site (the mod's only).
        __declspec(noinline) void note(ID3D11DeviceChild* r, Module* m, const void* ra, int64_t bytes, bool staging, const char* what)
        {
            Site* site = nullptr;
            if (m->kind == 0) {
                // [0] here, [1] the hook, [2] the mod's call (ra), [3] its caller
                void*   fr[4]{};
                const USHORT n = RtlCaptureStackBackTrace(0, 4, fr, nullptr);
                uintptr_t     k0 = reinterpret_cast<uintptr_t>(ra), k1 = 0;
                if (n >= 4 && fr[2] == ra) k1 = reinterpret_cast<uintptr_t>(fr[3]);
                const uint64_t key = uint64_t(k0) ^ (uint64_t(k1) * 0x9E3779B97F4A7C15ull);
                std::lock_guard _{ g_site_mtx };
                Site*& s = g_sites[key];
                if (!s) {
                    s         = new Site{};
                    s->ret[0] = k0;
                    s->ret[1] = k1;
                }
                strncpy_s(s->last, what, _TRUNCATE);
                site = s;
            }
            Tag* t = new (std::nothrow) Tag(m, site, bytes, staging);
            if (!t) return;
            if (SUCCEEDED(r->SetPrivateDataInterface(kTagGuid, t))) t->arm();
            t->Release();   // D3D holds it now (or it goes at once)
        }

        // ---- the device's creation calls (vtable slots) -------------------------------------------------------------------------
        using PfnBuf = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
        using PfnT1  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
        using PfnT2  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
        using PfnT3  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
        PfnBuf g_o_buf = nullptr;
        PfnT1  g_o_t1  = nullptr;
        PfnT2  g_o_t2  = nullptr;
        PfnT3  g_o_t3  = nullptr;

        HRESULT STDMETHODCALLTYPE hk_buf(ID3D11Device* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out)
        {
            const HRESULT hr = g_o_buf(d, desc, init, out);
            if (SUCCEEDED(hr) && out && *out && desc)
                if (Module* m = owner(_ReturnAddress())) {
                    char what[56];
                    sprintf_s(what, "buffer %u B usage %d bind 0x%x", desc->ByteWidth, int(desc->Usage), desc->BindFlags);
                    note(*out, m, _ReturnAddress(), desc->ByteWidth, desc->Usage == D3D11_USAGE_STAGING, what);
                }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE hk_t1(ID3D11Device* d, const D3D11_TEXTURE1D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture1D** out)
        {
            const HRESULT hr = g_o_t1(d, desc, init, out);
            if (SUCCEEDED(hr) && out && *out && desc)
                if (Module* m = owner(_ReturnAddress())) {
                    char what[56];
                    sprintf_s(what, "1D %u x%u fmt %d", desc->Width, desc->ArraySize, int(desc->Format));
                    note(*out, m, _ReturnAddress(), tex_bytes(desc->Format, desc->Width, 1, 1, desc->MipLevels, desc->ArraySize, 1), desc->Usage == D3D11_USAGE_STAGING, what);
                }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE hk_t2(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
        {
            const HRESULT hr = g_o_t2(d, desc, init, out);
            if (SUCCEEDED(hr) && out && *out && desc)
                if (Module* m = owner(_ReturnAddress())) {
                    char what[56];
                    sprintf_s(what, "2D %ux%u x%u fmt %d mips %u%s", desc->Width, desc->Height, desc->ArraySize, int(desc->Format), desc->MipLevels,
                              desc->Usage == D3D11_USAGE_STAGING ? " staging" : "");
                    note(*out, m, _ReturnAddress(), tex_bytes(desc->Format, desc->Width, desc->Height, 1, desc->MipLevels, desc->ArraySize, desc->SampleDesc.Count),
                         desc->Usage == D3D11_USAGE_STAGING, what);
                }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE hk_t3(ID3D11Device* d, const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D** out)
        {
            const HRESULT hr = g_o_t3(d, desc, init, out);
            if (SUCCEEDED(hr) && out && *out && desc)
                if (Module* m = owner(_ReturnAddress())) {
                    char what[56];
                    sprintf_s(what, "3D %ux%ux%u fmt %d", desc->Width, desc->Height, desc->Depth, int(desc->Format));
                    note(*out, m, _ReturnAddress(), tex_bytes(desc->Format, desc->Width, desc->Height, desc->Depth, desc->MipLevels, 1, 1), desc->Usage == D3D11_USAGE_STAGING,
                         what);
                }
            return hr;
        }

        template <class F> bool patch(void** vt, int i, F hook, F& orig)
        {
            DWORD old = 0;
            if (!VirtualProtect(&vt[i], sizeof(void*), PAGE_READWRITE, &old)) return false;
            orig = reinterpret_cast<F>(vt[i]);
            InterlockedExchangePointer(&vt[i], reinterpret_cast<void*>(hook));
            VirtualProtect(&vt[i], sizeof(void*), old, &old);
            return true;
        }

        // ---- the log thread ---------------------------------------------------------------------------------------------------
        std::atomic<bool>       g_started{ false };
        std::atomic<int>        g_interval{ 60 };   // seconds; 0 off
        std::atomic<int>        g_req{ 0 };         // 1 a line now, 2 the call sites too
        std::mutex              g_cv_mtx;
        std::condition_variable g_cv;
        IDXGIAdapter3*          g_adapter = nullptr;
        clock::time_point       g_t0;

        struct Snap
        {
            int64_t priv = 0, ws = 0, vram = 0, shared = 0, heap = 0, gpu = 0;
            bool    ok = false;
        };
        Snap g_first, g_prev;

        std::string mb_delta(int64_t now, int64_t then) { return fmt::format("{:+.0f}", double(now - then) / kMB); }

        std::string rva(uintptr_t a)
        {
            if (!a) return "-";
            if (a - g_mod_lo < g_mod_len) return fmt::format("+{:x}", a - g_mod_lo);
            const int n = g_nmods.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                if (a - g_mods[i].lo < g_mods[i].len) return fmt::format("{}+{:x}", g_mods[i].name, a - g_mods[i].lo);
            return fmt::format("{:x}", a);
        }

        // RAM detail (v2, after the 22:19 log: private bytes +4.6 GB in 15 min while the mod's own heap moved +20 MB): the process's
        // committed private memory split into the heaps' (HeapSummary: CRT malloc/new of every module, the game's included) and the rest
        // (VirtualAlloc'd blocks: the game's own allocators, the driver).
        // v3 (14 Nov, simulator runs 01:09-01:47): the full walk (VirtualQuery over the address space, ~1500 blocks of 1 MB+) took 290-460 ms
        // and the 5 s window holding it had its longest pair ~75 ms longer than the others (126 vs 51 ms): the game's own allocations wait
        // on the address-space lock the walk holds. So: the heaps alone (timed) on every 10th line, the walk with the large blocks only
        // on demand (memwatch detail).
        std::atomic<int> g_detail_every{ 10 };   // memheaps N (lines; 0 never)
        int              g_line_n = 0;

        std::string heaps_only()
        {
            const auto          t0 = clock::now();
            int64_t             heaps = 0, proc_heap = 0, top_heap = 0;
            std::vector<HANDLE> hs(64);
            DWORD               nh = GetProcessHeaps(DWORD(hs.size()), hs.data());
            if (nh > hs.size()) { hs.resize(nh + 16); nh = GetProcessHeaps(DWORD(hs.size()), hs.data()); }
            const HANDLE ph = GetProcessHeap();
            for (DWORD i = 0; i < nh && i < hs.size(); ++i) {
                HEAP_SUMMARY sum{};
                sum.cb = sizeof(sum);
                if (!HeapSummary(hs[i], 0, &sum)) continue;
                heaps += int64_t(sum.cbCommitted);
                if (hs[i] == ph) proc_heap = int64_t(sum.cbCommitted);
                else top_heap = std::max<int64_t>(top_heap, int64_t(sum.cbCommitted));
            }
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            static int64_t heaps_first = -1, rest_first = 0;
            const int64_t  rest = int64_t(pmc.PrivateUsage) - heaps;
            if (heaps_first < 0) { heaps_first = heaps; rest_first = rest; }
            const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            return fmt::format("heaps {:.0f} MB ({} heaps; process heap {:.0f}, largest other {:.0f}; {:+.0f} since the first heaps line) + the rest of private "
                               "(VirtualAlloc: the game's allocators, the driver) {:.0f} MB ({:+.0f}) | {:.1f} ms",
                               double(heaps) / kMB, nh, double(proc_heap) / kMB, double(top_heap) / kMB, double(heaps - heaps_first) / kMB, double(rest) / kMB,
                               double(rest - rest_first) / kMB, ms);
        }

        std::unordered_map<uintptr_t, int64_t> g_big_prev;   // allocation base -> committed bytes (blocks of 1 MB or more), last line
        int64_t                                g_heaps_first = -1, g_other_first = -1;

        std::string ram_detail()
        {
            const auto t0 = clock::now();
            int64_t    priv = 0, mapped = 0;
            std::vector<std::pair<uintptr_t, int64_t>> blocks;   // allocation base -> committed private bytes
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi);) {
                const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= a) break;
                if (mbi.State == MEM_COMMIT) {
                    if (mbi.Type == MEM_PRIVATE) {
                        priv += int64_t(mbi.RegionSize);
                        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                        if (!blocks.empty() && blocks.back().first == base) blocks.back().second += int64_t(mbi.RegionSize);
                        else blocks.push_back({ base, int64_t(mbi.RegionSize) });
                    } else if (mbi.Type == MEM_MAPPED) {
                        mapped += int64_t(mbi.RegionSize);
                    }
                }
                a = next;
            }
            // The heaps.
            int64_t             heaps = 0, proc_heap = 0, top_heap = 0;
            std::vector<HANDLE> hs(64);
            DWORD               nh = GetProcessHeaps(DWORD(hs.size()), hs.data());
            if (nh > hs.size()) { hs.resize(nh + 16); nh = GetProcessHeaps(DWORD(hs.size()), hs.data()); }
            const HANDLE ph = GetProcessHeap();
            for (DWORD i = 0; i < nh && i < hs.size(); ++i) {
                HEAP_SUMMARY sum{};
                sum.cb = sizeof(sum);
                if (!HeapSummary(hs[i], 0, &sum)) continue;
                heaps += int64_t(sum.cbCommitted);
                if (hs[i] == ph) proc_heap = int64_t(sum.cbCommitted);
                else top_heap = std::max<int64_t>(top_heap, int64_t(sum.cbCommitted));
            }
            const int64_t other = priv - heaps;
            if (g_heaps_first < 0) { g_heaps_first = heaps; g_other_first = other; }
            // Large blocks: count, new and gone since the last line, the five largest.
            std::unordered_map<uintptr_t, int64_t> big;
            int64_t big_sum = 0;
            for (const auto& [b, n] : blocks)
                if (n >= (1 << 20)) { big[b] = n; big_sum += n; }
            int     n_new = 0, n_gone = 0;
            int64_t b_new = 0, b_gone = 0, b_grew = 0;
            for (const auto& [b, n] : big) {
                const auto it = g_big_prev.find(b);
                if (it == g_big_prev.end()) { ++n_new; b_new += n; }
                else if (n > it->second) b_grew += n - it->second;
            }
            for (const auto& [b, n] : g_big_prev)
                if (!big.contains(b)) { ++n_gone; b_gone += n; }
            const bool first = g_big_prev.empty();
            g_big_prev       = std::move(big);
            std::sort(blocks.begin(), blocks.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            std::string top;
            for (size_t i = 0; i < blocks.size() && i < 5; ++i) top += fmt::format("{}{:x} {:.0f} MB", i ? ", " : "", blocks[i].first, double(blocks[i].second) / kMB);
            const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            return fmt::format("committed private {:.0f} MB = heaps {:.0f} MB ({} heaps; process heap {:.0f}, largest other {:.0f}; {:+.0f} since first line) + other "
                               "(VirtualAlloc) {:.0f} MB ({:+.0f}); mapped {:.0f} MB | blocks of 1 MB+: {} ({:.0f} MB), {}{} new ({:.0f} MB), {} gone ({:.0f} MB), "
                               "{:.0f} MB grown in place | largest: {} | walk {:.1f} ms",
                               double(priv) / kMB, double(heaps) / kMB, nh, double(proc_heap) / kMB, double(top_heap) / kMB, double(heaps - g_heaps_first) / kMB,
                               double(other) / kMB, double(other - g_other_first) / kMB, double(mapped) / kMB, g_big_prev.size(), double(big_sum) / kMB,
                               first ? "first line: " : "", n_new, double(b_new) / kMB, n_gone, double(b_gone) / kMB, double(b_grew) / kMB, top, ms);
        }

        void write_lines(int req)
        {
            const bool all_sites = req == 2;
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            DWORD handles = 0;
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            int threads = 0;
            if (HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); sn != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{};
                te.dwSize       = sizeof(te);
                const DWORD pid = GetCurrentProcessId();
                for (BOOL ok = Thread32First(sn, &te); ok; ok = Thread32Next(sn, &te))
                    if (te.th32OwnerProcessID == pid) ++threads;
                CloseHandle(sn);
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO loc{}, nonloc{};
            const bool vm_ok = g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc)) &&
                               SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonloc));

            const Module& mod = g_mods[0];
            Snap          s;
            s.ok     = true;
            s.priv   = int64_t(pmc.PrivateUsage);
            s.ws     = int64_t(pmc.WorkingSetSize);
            s.vram   = vm_ok ? int64_t(loc.CurrentUsage) : 0;
            s.shared = vm_ok ? int64_t(nonloc.CurrentUsage) : 0;
            s.heap   = g_heap_bytes.load(std::memory_order_relaxed);
            s.gpu    = mod.bytes.load(std::memory_order_relaxed);
            if (!g_first.ok) g_first = s;
            const Snap& p = g_prev.ok ? g_prev : s;
            const Snap& f = g_first;

            const int64_t made     = mod.made.load(std::memory_order_relaxed);
            const int64_t made_new = made - g_mods[0].made_seen;
            g_mods[0].made_seen    = made;
            std::string others;
            const int   n = g_nmods.load(std::memory_order_acquire);
            for (int i = 1; i < n; ++i) {
                Module& m = g_mods[i];
                if (m.kind != 1) continue;
                const int64_t mm = m.made.load(std::memory_order_relaxed);
                if (mm == 0) continue;
                others += fmt::format("{}{} {} live {:.0f} MB ({} made since last line)", others.empty() ? "" : ", ", m.name, m.live.load(), double(m.bytes.load()) / kMB,
                                      mm - m.made_seen);
                m.made_seen = mm;
            }
            const double mins = std::chrono::duration<double>(clock::now() - g_t0).count() / 60.0;
            spdlog::info("[Mem] t={:.0f} min | RAM private {:.0f} MB ({} since first line, {} since last), working set {:.0f} MB ({}), handles {}, threads {} | "
                         "VRAM {:.0f} of {:.0f} MB budget ({}, {}), shared {:.0f} MB ({}) | mod heap {:.1f} MB in {} blocks ({:+.1f}, {:+.1f}) | "
                         "mod GPU {} resources {:.1f} MB, staging {:.1f} MB ({:+.1f}, {:+.1f}), {} made since last line | other modules: {}",
                         mins, double(s.priv) / kMB, mb_delta(s.priv, f.priv), mb_delta(s.priv, p.priv), double(s.ws) / kMB, mb_delta(s.ws, f.ws), handles, threads,
                         double(s.vram) / kMB, vm_ok ? double(loc.Budget) / kMB : -1.0, mb_delta(s.vram, f.vram), mb_delta(s.vram, p.vram), double(s.shared) / kMB,
                         mb_delta(s.shared, f.shared), double(s.heap) / kMB, g_heap_blocks.load(std::memory_order_relaxed), double(s.heap - f.heap) / kMB,
                         double(s.heap - p.heap) / kMB, mod.live.load(), double(s.gpu) / kMB, double(mod.staging.load()) / kMB, double(s.gpu - f.gpu) / kMB,
                         double(s.gpu - p.gpu) / kMB, made_new, others.empty() ? "none" : others);
            g_prev = s;

            // Call sites: the biggest, and the ones whose live count grew.
            struct Row
            {
                Site*       s;
                int64_t     live, bytes, made, made_new;
                std::string last;
            };
            std::vector<Row> rows;
            {
                std::lock_guard _{ g_site_mtx };
                rows.reserve(g_sites.size());
                for (auto& [k, st] : g_sites) rows.push_back({ st, st->live.load(), st->bytes.load(), st->made.load(), st->made.load() - st->made_seen, st->last });
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
            auto one = [](const Row& r) {
                return fmt::format("{}<{} {} live {:.1f} MB ({} made; last {})", rva(r.s->ret[0]), rva(r.s->ret[1]), r.live, double(r.bytes) / kMB, r.made, r.last);
            };
            int live_sites = 0;
            for (const auto& r : rows) live_sites += r.live > 0;
            std::string top, grew;
            int         k = 0;
            for (const auto& r : rows) {
                if (r.live <= 0 || k >= 6) break;
                top += fmt::format("{}{}", k++ ? " | " : "", one(r));
            }
            int g = 0;
            for (const auto& r : rows) {
                if (r.live > r.s->live_seen && g < 8) grew += fmt::format("{}{} {} -> {}", g++ ? " | " : "", one(r), r.s->live_seen, r.live);
            }
            // Churn: the sites that made the most since the last line (a resource made and freed every frame costs the driver each time).
            std::vector<const Row*> churn;
            for (const auto& r : rows)
                if (r.made_new > 0) churn.push_back(&r);
            std::sort(churn.begin(), churn.end(), [](const Row* a, const Row* b) { return a->made_new > b->made_new; });
            std::string ch;
            for (size_t i = 0; i < churn.size() && i < 4; ++i) ch += fmt::format("{}{}: {} made", i ? " | " : "", one(*churn[i]), churn[i]->made_new);
            spdlog::info("[Mem] mod GPU by call site (top {} of {} live sites): {} || grew since last line: {} || made most since last line: {}", k, live_sites,
                         top.empty() ? "none" : top, grew.empty() ? "none" : grew, ch.empty() ? "none" : ch);
            if (req == 3) spdlog::info("[Mem] RAM detail (full walk): {}", ram_detail());
            else if (const int every = g_detail_every.load(); every > 0 && ++g_line_n % every == 0) spdlog::info("[Mem] RAM heaps: {}", heaps_only());
            if (all_sites) {
                std::string line;
                int         c = 0;
                for (const auto& r : rows) {
                    if (r.live <= 0) continue;
                    line += fmt::format("{}{}", line.empty() ? "" : " | ", one(r));
                    if (++c % 8 == 0) { spdlog::info("[Mem] sites: {}", line); line.clear(); }
                }
                if (!line.empty()) spdlog::info("[Mem] sites: {}", line);
                spdlog::info("[Mem] sites: {} live call sites listed (dxgi.dll base {:x}; +offsets are from it)", c, g_mod_lo);
            }
            for (auto& r : rows) {
                r.s->live_seen = r.live;
                r.s->made_seen = r.made;
            }
        }

        void worker()
        {
            auto last = clock::now();
            for (;;) {
                const int  iv       = g_interval.load();
                const auto deadline = iv > 0 ? last + std::chrono::seconds(iv) : clock::now() + std::chrono::hours(1);
                {
                    std::unique_lock lk{ g_cv_mtx };
                    g_cv.wait_until(lk, deadline, [] { return g_req.load() != 0; });
                }
                const int req = g_req.exchange(0);
                const int iv2 = g_interval.load();
                if (req == 0 && (iv2 <= 0 || clock::now() < last + std::chrono::seconds(iv2))) continue;   // the interval changed: wait again
                last = clock::now();
                if (req == 0 && !fc5::sessionlogs::on()) continue;   // sessionlogs off: no minute line (a typed memwatch still asks for one)
                write_lines(req);
            }
        }
    }

    void start(ID3D11Device* dev)
    {
        if (!dev || g_started.exchange(true)) return;
        g_t0 = clock::now();
        // The mod's own module first (kind 0), then the game's engine for the fast path.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&start), &self);
        MODULEINFO mi{};
        if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
            g_mod_lo  = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            g_mod_len = mi.SizeOfImage;
            add_module(g_mod_lo);
        }
        if (g_nmods.load() != 1 || g_mods[0].kind != 0) {
            spdlog::error("[Mem] memwatch: the mod's own module not found: GPU resources not counted");
        } else if (HMODULE eng = GetModuleHandleA("FC_m64.dll"); eng && GetModuleInformation(GetCurrentProcess(), eng, &mi, sizeof(mi))) {
            add_module(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll));
            g_engine_lo  = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            g_engine_len = mi.SizeOfImage;
        }
        bool hooked = false;
        if (g_nmods.load() >= 1 && g_mods[0].kind == 0) {
            // ID3D11Device: IUnknown (0-2), CreateBuffer 3, CreateTexture1D 4, CreateTexture2D 5, CreateTexture3D 6.
            void** vt = *reinterpret_cast<void***>(dev);
            hooked    = patch(vt, 3, &hk_buf, g_o_buf) && patch(vt, 4, &hk_t1, g_o_t1) && patch(vt, 5, &hk_t2, g_o_t2) && patch(vt, 6, &hk_t3, g_o_t3);
        }
        IDXGIDevice*  dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd))) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad)
            ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter));
        if (ad) ad->Release();
        if (dd) dd->Release();
        spdlog::info("[Mem] memwatch: a memory line every {} s (memwatch S to change, memwatch for one now, memwatch sites for every call site); "
                     "mod GPU resources counted from now: {} (dxgi.dll at {:x}, FC_m64.dll {})",
                     g_interval.load(), hooked ? "yes" : "NO (hook failed)", g_mod_lo, g_engine_len ? "found" : "not found");
        std::thread(worker).detach();
    }

    bool command(const std::string& cmd, const std::string& arg, float fv)
    {
        if (cmd == "memheaps") {   // memheaps N: the heaps line every N memory lines (default 10; 0 never)
            g_detail_every = std::max(0, int(fv));
            spdlog::info("[Mem] memheaps: the heaps line every {} memory lines (0 never)", g_detail_every.load());
            return true;
        }
        if (cmd != "memwatch") return false;
        if (arg.empty()) {
            g_req = 1;
        } else if (arg == "sites") {
            g_req = 2;
        } else if (arg == "detail") {
            g_req = 3;   // the full walk: a hitch of up to ~100 ms in the headset

        } else if (arg == "off" || fv <= 0.0f) {
            g_interval = 0;
            spdlog::info("[Mem] memwatch off (memwatch S to turn it on again)");
        } else {
            g_interval = std::max(10, int(fv));
            spdlog::info("[Mem] memwatch: a memory line every {} s", g_interval.load());
        }
        g_cv.notify_all();
        return true;
    }
}
