// switchfast (author 28 Oct;
// F11 Experimental > Weapon switch (test), saved, default on).
//
// The game times each weapon's switch from its own data. Every weapon's common properties (the reflected class whose fields
// FC_m64.dll registers at +0x1df0394.. with their descriptions) hold, among others:
//   +0x1b4 fHolsterTime   "Time of the holstering process, playing animation is scaled on defined time when m_holsterTime is not equal to 0"
//   +0x1d8 fDrawTime      "Time of the weapon drawing process, playing animation is scaled on defined time when m_drawTime is not equal to 0"
// (the offsets are the registration's own: mov edx, offset before each name). The game data (common.dat's item database,
// 468 weapons) gives fDrawTime 0-1.5 s (0.7, 0.5, 0.9 the most common) and fHolsterTime 0-0.7 s (0.3, 0.2), which is the
// 0.6 s (1.2 s from the sling) a switch takes in the author's logs. 0 plays the animation at its own length.
// With switchfast on, every weapon's block found in the game's memory gets switchdraw (0.15 s) and switchholster (0.10 s)
// where its own time is longer or 0, so the game's draw and holster animations play that fast and the weapon can fire sooner.
//
// Finding the blocks: the code that reads them is in the protected part of FC_m64.dll, so they are found by their values. A
// worker thread reads the committed, writable private memory (ReadProcessMemory: a page freed meanwhile is skipped, never a
// fault) for the run of times +0x1b4..+0x1e8 (14 floats, every one 0-4 s) with fLadderHolsterTime +0x1bc exactly 0.15 or 0.125 s
// (the data's two values), fDrawTime at most 1.5 s and fHolsterTime at most 0.7 s (the data's longest), fSwitchToMacheteTime +0x1e0
// 0.05-1.5 s, fThrowableHolsterTime +0x1d0 at most 0.6 s and fFireTriggerPressThreshold +0xb8 0.01-0.6 (data: 0.05-0.5), at an 8-byte aligned address with fNearFovDegrees +0x1d5c 55-90 (without these two, ~10000
// short-lived places matched in the simulator). Each block found keeps its own times, so switchfast off puts them back.
// Every second each block is read again: one that no longer matches (its memory reused) is dropped, one the game set back is
// written again. A new scan runs when switchfast is turned on, every 15 s while nothing is found, after a switch slower than
// the times written (a weapon loaded since) and after 20 or more blocks went at once (a load); at most every 10 s for those two.
// switchrescan S adds a scan every S s (default 0 = none since 30 Oct: the author sat in the 300 s rescan, as did every big drop of his 13:25 play: 14-16 GB read in ~14 s, the GPU slowed throughout).
// Write-combined and uncached pages (the graphics driver's CPU-visible GPU memory, video memory itself with Resizable BAR)
// are never read: a CPU read there is uncached over PCIe and stalls the GPU's own traffic; game data is never there.
// Writes use WriteProcessMemory on the block's own two floats, only after its whole pattern was read again.
//
// reloadfullmag (FarCry5Reload.cpp; author 8 Nov 10:45 for the AR-C's 29 of 30): the same block holds bAutoReload at
// +0x1ed (the registration: mov edx, 0x1ed before its name at +0x1df06b7, as fDrawTime's 0x1d8 at +0x1df03c0), the game's own reload
// when the clip runs dry (1 on the AR-C: research 2026-11-02_manualreload.md). Written 0 on the gun in hand's block while the manual
// reload has it, so a magazine fires every round and the game still starts no reload of its own (reloadkeepone kept one instead).
// The gun in hand's block: from its weapon object (FarCry5Lua.cpp, inventory vtable +0x308): a copy inside it, or up to three steps of
// pointers into (or just ahead of) a block the scan holds, with the gun's magazine size (find_block); every pointer is checked with
// VirtualQuery (committed private read-write, no guard page) and read with ReadProcessMemory, as the scan. Its own value is kept and put back when reloadfullmag or the manual reload
// goes off, or after the game was seen reloading by itself anyway (autoreload_failed: not used again this session). A weapon put away
// keeps its 0: an empty gun drawn again must not reload by itself before the block is found again. Blocks are the weapon's data, shared
// with anyone carrying the same weapon template.
#include "FarCry5SwitchSpeed.h"
#include "FarCry5Lua.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <istream>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::switchspeed
{
namespace
{
    using clock = std::chrono::steady_clock;

    // Offsets in a weapon's common properties block (FC_m64.dll's registration, see above).
    constexpr size_t kTrigger  = 0xb8;    // fFireTriggerPressThreshold
    constexpr size_t kHolster  = 0x1b4;   // fHolsterTime
    constexpr size_t kLadder   = 0x1bc;   // fLadderHolsterTime
    constexpr size_t kThrow    = 0x1d0;   // fThrowableHolsterTime
    constexpr size_t kDraw     = 0x1d8;   // fDrawTime
    constexpr size_t kMachete  = 0x1e0;   // fSwitchToMacheteTime
    constexpr size_t kLast     = 0x1e8;   // fSprintToFireTime, the run's last float
    constexpr size_t kNearFov  = 0x1d5c;  // fNearFovDegrees (logged only: 55-90 in the data)
    constexpr size_t kSpan     = kLast + 4;

    std::atomic<bool>  g_on{ true };
    std::atomic<float> g_draw{ 0.15f }, g_holster{ 0.10f };
    std::atomic<int>   g_rescan_s{ 0 };     // switchrescan: 0 = no timed rescan (30 Oct)
    std::atomic<bool>  g_started{ false };
    std::atomic<bool>  g_scan_req{ false };
    std::atomic<bool>  g_restore_req{ false };
    std::atomic<int>   g_list_req{ 0 };
    std::atomic<int64_t> g_slow_ms{ 0 };     // the latest slow switch (clock ms), for a rescan
    std::atomic<int>   g_count{ 0 };         // blocks held (for the menu)
    std::atomic<int>   g_written{ 0 };       // of them, sped up
    int                g_scans = 0;          // scans this run (worker thread only; logged)
    // For the load popup (scan_state): a scan running, when the latest started (clock ms) and how long the latest took.
    std::atomic<bool>    g_scanning{ false };
    std::atomic<int64_t> g_scan_at{ 0 };
    std::atomic<float>   g_scan_secs{ 0.0f };

    struct Block
    {
        uintptr_t a;
        float     draw0, holster0;   // the game's own times
        float     draw1, holster1;   // what was written (= the own time when it is already short enough)
        bool      written;
    };
    std::mutex         g_mtx;
    std::vector<Block> g_blocks;

    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count(); }

    // Write-combined or uncached: the driver's GPU-visible memory (see the top), never read.
    bool gpu_mapped(const MEMORY_BASIC_INFORMATION& m) { return (m.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0; }

    bool readable(const MEMORY_BASIC_INFORMATION& m)
    {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
        if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (gpu_mapped(m)) return false;
        return (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    }
    bool read(uintptr_t a, void* out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
    }
    bool write_f(uintptr_t a, float v)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), &v, 4, &put) && put == 4;
    }
    float f_at(const uint8_t* p, size_t off)
    {
        float f;
        std::memcpy(&f, p + off, 4);
        return f;
    }
    bool in(float f, float lo, float hi) { return std::isfinite(f) && f >= lo && f <= hi; }

    // p: a candidate block's first byte, with at least kSpan bytes readable.
    bool matches(const uint8_t* p)
    {
        // The ladder time exactly 0.15 or 0.125 as in the data (simulator 29 Oct: 587 of 589 blocks; the other two, ladder 0.148 and
        // 0.149 with odd times and floats for a name, were not weapons).
        const float ladder = f_at(p, kLadder);
        if (std::abs(ladder - 0.15f) > 1e-5f && std::abs(ladder - 0.125f) > 1e-5f) return false;
        for (size_t o = kHolster; o <= kLast; o += 4)
            if (!in(f_at(p, o), 0.0f, 4.0f)) return false;
        return in(f_at(p, kMachete), 0.05f, 1.5f) && f_at(p, kThrow) <= 0.6f && f_at(p, kDraw) <= 1.5f && f_at(p, kHolster) <= 0.7f &&
               in(f_at(p, kTrigger), 0.01f, 0.6f);
    }
    // The run alone matched ~10000 places in the simulator (29 Oct), most of them short-lived (rescans every 10 s as they went):
    // a block is also 8-byte aligned and holds its near FOV 55-90 at +0x1d5c (627 of them did, all with real weapon times).
    bool is_block(uintptr_t a, const uint8_t* p)
    {
        if ((a & 7) != 0 || !matches(p)) return false;
        float fov = 0.0f;
        return read(a + kNearFov, &fov, 4) && in(fov, 55.0f, 90.0f);
    }
    bool still_matches(uintptr_t a)
    {
        uint8_t buf[kSpan];
        return read(a, buf, sizeof(buf)) && is_block(a, buf);
    }

    float target(float own, float want) { return (own <= 0.0f || own > want) ? want : own; }

    // reloadfullmag (see the top).
    constexpr size_t       kAutoReload = 0x1ed;   // bAutoReload
    std::atomic<bool>      g_ar_on{ false };      // reloadfullmag on and the manual reload on
    std::atomic<uintptr_t> g_ar_want{ 0 };        // the weapon object to have the game's reload at empty off (0: none in hand)
    std::atomic<uintptr_t> g_ar_ok{ 0 };          // the weapon object whose block holds 0 (checked by reading back)
    std::atomic<bool>      g_ar_failed{ false };  // autoreload_failed: put back and not used again this session
    struct ArBlock
    {
        uintptr_t a;
        uint8_t   own;
    };
    std::vector<ArBlock> g_ar_blocks;              // worker thread only: the blocks written, with their own value
    uintptr_t            g_ar_obj = 0, g_ar_blk = 0;   // worker thread only: the last weapon object and its block
    int64_t              g_ar_miss_t = 0;          // worker thread only: when the last object's block was not found (tried again after 3 s)

    // A place that may be read: committed private read-write memory with n bytes before its region's end (no guard page touched).
    bool may_read(uintptr_t a, size_t n)
    {
        if (a < 0x10000 || a >= 0x7FFFFFFF0000ull || (a & 7) != 0) return false;
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m)) || !readable(m)) return false;
        return a + n <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
    }
    bool is_block_at(uintptr_t a) { return may_read(a, kNearFov + 4) && still_matches(a); }

    // reloadfullmag 2 (the builder's run, 8 Nov 12:12: no block within two steps of the weapon object, every gun): the search widened.
    //  - A copy inside the object: the block's pattern anywhere in its first 0x4000 bytes (the weapon's own copy of its data: then only
    //    this weapon is changed).
    //  - Three steps of pointers (0x800 bytes of the object, 0x400 one step down, 0x200 two steps down), each one tested against the blocks
    //    the scan holds: a pointer into one (b <= p < b + 0x1d60) or up to 0x4000 bytes before one (an object the block is part of).
    //  - The block of the gun in hand holds its magazine size: iAmmoInClip at +0x328 (the registration: mov edx, 0x328 before its name at
    //    +0x1df11c4) equal to the game's clip size (weapon vtable +0x48, lua::player_ammo cap). Only such a block is ever written.
    // Every candidate is logged (8 at most), with the object's vtable and its +0x40 / +0x48 slots as FC_m64.dll offsets, for the next step.
    // The path found is kept and tried first for the next guns (the same class for every weapon).
    constexpr size_t kAmmoInClip = 0x328;   // iAmmoInClip
    constexpr size_t kBlockLen   = kNearFov + 4;
    struct ArPath
    {
        int     n{ -1 };         // pointer steps (0: a copy inside the object; -1: none)
        int     off[3]{};        // the slot read at each step
        int64_t delta{ 0 };      // block = the last pointer (or the object) + delta
    };
    ArPath g_ar_path;            // worker thread only: the path found

    std::vector<uintptr_t> held_blocks()
    {
        std::lock_guard _{ g_mtx };
        std::vector<uintptr_t> v;
        v.reserve(g_blocks.size());
        for (const Block& b : g_blocks) v.push_back(b.a);
        std::sort(v.begin(), v.end());
        return v;
    }
    // The held block p points into, or up to 0x4000 bytes ahead of; 0 for none.
    uintptr_t near_block(const std::vector<uintptr_t>& bl, uint64_t p)
    {
        auto it = std::upper_bound(bl.begin(), bl.end(), uintptr_t(p));
        if (it != bl.begin() && p < *(it - 1) + kBlockLen) return *(it - 1);
        if (it != bl.end() && *it - p <= 0x4000) return *it;
        return 0;
    }
    int block_clip(uintptr_t b)
    {
        int v = -1;
        return read(b + kAmmoInClip, &v, 4) ? v : -1;
    }
    uintptr_t resolve(uintptr_t obj, const ArPath& pa)
    {
        if (pa.n < 0) return 0;
        uint64_t p = obj;
        for (int i = 0; i < pa.n; ++i) {
            if (!may_read(p + pa.off[i], 8) || !read(p + pa.off[i], &p, 8)) return 0;
        }
        return uintptr_t(int64_t(p) + pa.delta);
    }

    struct ArCand
    {
        ArPath    pa;
        uintptr_t b;
    };
    uintptr_t find_block(uintptr_t obj, int cap)
    {
        // The path found before.
        if (const uintptr_t b = resolve(obj, g_ar_path); b != 0 && is_block_at(b) && (cap <= 0 || block_clip(b) == cap)) return b;
        const auto t0 = clock::now();
        const std::vector<uintptr_t> bl = held_blocks();
        std::vector<ArCand> cands;
        // A copy inside the object.
        {
            constexpr size_t kIn = 0x4000;
            std::vector<uint8_t> buf(kIn + kBlockLen);
            if (may_read(obj, buf.size()) && read(obj, buf.data(), buf.size()))
                for (size_t k = 0; k < kIn && cands.size() < 64; k += 8) {
                    float fov;
                    std::memcpy(&fov, buf.data() + k + kNearFov, 4);
                    if (matches(buf.data() + k) && in(fov, 55.0f, 90.0f)) cands.push_back({ { 0, {}, int64_t(k) }, obj + k });
                }
        }
        // Three steps of pointers.
        if (!bl.empty()) {
            constexpr size_t kSpanAt[3]{ 0x800, 0x400, 0x200 };
            struct Node
            {
                uint64_t a;
                int      n;
                int      off[3];
            };
            std::vector<Node> front{ { obj, 0, {} } };
            std::unordered_set<uint64_t> seen{ obj };
            std::vector<uint64_t> q;
            for (int depth = 0; depth < 3 && !front.empty(); ++depth) {
                std::vector<Node> next;
                for (const Node& nd : front) {
                    const size_t span = kSpanAt[depth];
                    q.assign(span / 8, 0);
                    if (!may_read(nd.a, span) || !read(nd.a, q.data(), span)) continue;
                    for (size_t i = 0; i < q.size(); ++i) {
                        const uint64_t p = q[i];
                        if (p < 0x10000 || p >= 0x7FFFFFFF0000ull) continue;
                        Node ch{ p, nd.n + 1, { nd.off[0], nd.off[1], nd.off[2] } };
                        ch.off[nd.n] = int(i * 8);
                        if (const uintptr_t b = near_block(bl, p); b != 0 && cands.size() < 64) {
                            ArPath pa{ ch.n, { ch.off[0], ch.off[1], ch.off[2] }, int64_t(b) - int64_t(p) };
                            cands.push_back({ pa, b });
                        }
                        if (depth < 2 && (p & 7) == 0 && seen.insert(p).second && seen.size() < 40000) next.push_back(ch);
                    }
                }
                front.swap(next);
            }
        }
        // The one with the magazine size, the fewest steps, then pointing at the block itself.
        auto score = [&](const ArCand& c) {
            const bool clip_ok = cap > 0 && block_clip(c.b) == cap;
            return std::tuple{ clip_ok ? 0 : 1, c.pa.n == 0 ? 0 : c.pa.n, c.pa.delta == 0 ? 0 : 1, std::abs(c.pa.delta) };
        };
        std::stable_sort(cands.begin(), cands.end(), [&](const ArCand& x, const ArCand& y) { return score(x) < score(y); });
        const float ms = std::chrono::duration<float, std::milli>(clock::now() - t0).count();
        // The object's vtable and its clip (+0x40) and clip size (+0x48) slots, as FC_m64.dll offsets (for reading the game's code next).
        std::string vt = "?";
        {
            const uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleA("FC_m64.dll"));
            uint64_t v = 0, s40 = 0, s48 = 0;
            if (read(obj, &v, 8)) {
                auto rel = [mod](uint64_t x) { return mod && x >= mod && x < mod + 0x10000000 ? fmt::format("FC_m64.dll+{:#x}", x - mod) : fmt::format("{:#x}", x); };
                vt = rel(v);
                if (read(v + 0x40, &s40, 8) && read(v + 0x48, &s48, 8)) vt += fmt::format(" (+0x40 {}, +0x48 {})", rel(s40), rel(s48));
            }
        }
        spdlog::info("[FullMag] weapon object {:#x} (vtable {}), clip size {}: {} candidate block(s) in {:.0f} ms ({} blocks held)", obj, vt, cap, cands.size(), ms,
                     bl.size());
        for (size_t i = 0; i < cands.size() && i < 8; ++i) {
            const ArCand& c = cands[i];
            std::string path = "object";
            for (int k = 0; k < c.pa.n; ++k) path = fmt::format("[{} + {:#x}]", path, c.pa.off[k]);
            uint8_t ar  = 0xFF;
            uint32_t nm = 0;
            float   dr  = -1.0f;
            read(c.b + kAutoReload, &ar, 1);
            read(c.b + 0x1d80, &nm, 4);
            read(c.b + kDraw, &dr, 4);
            spdlog::info("[FullMag]   {} {:+#x} = block {:#x}: iAmmoInClip {}, bAutoReload {}, draw {:.3f} s, name id {:#x}", path, c.pa.delta, c.b, block_clip(c.b), int(ar), dr, nm);
        }
        if (cands.empty() || !(cap > 0 && block_clip(cands[0].b) == cap)) return 0;   // never a block that is not this gun's size
        g_ar_path = cands[0].pa;
        return cands[0].b;
    }
    bool write_b(uintptr_t a, uint8_t v)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), &v, 1, &put) && put == 1;
    }
    void ar_put_back(const char* why)
    {
        int n = 0;
        for (const ArBlock& b : g_ar_blocks) {
            uint8_t v = 0xFF;
            if (is_block_at(b.a) && read(b.a + kAutoReload, &v, 1) && v == 0 && b.own != 0 && write_b(b.a + kAutoReload, b.own)) n++;
        }
        if (!g_ar_blocks.empty()) spdlog::info("[FullMag] the game's own reload at empty put back on {} of {} weapon blocks ({})", n, g_ar_blocks.size(), why);
        g_ar_blocks.clear();
        g_ar_obj = g_ar_blk = 0;
        g_ar_ok  = 0;
    }
    void ar_tick()
    {
        if (!g_ar_on.load() || g_ar_failed.load()) {
            if (!g_ar_blocks.empty() || g_ar_ok.load()) ar_put_back(g_ar_failed.load() ? "the game reloaded by itself anyway: reloadkeepone's kept round from now" : "reloadfullmag or the manual reload off");
            return;
        }
        const uintptr_t obj = g_ar_want.load();
        if (obj == 0) {   // none in hand, or not a magazine gun: blocks written stay so (see the top)
            g_ar_ok = 0;
            return;
        }
        const int64_t t = now_ms();
        if (obj != g_ar_obj) {
            g_ar_obj    = obj;
            g_ar_blk    = 0;
            g_ar_ok     = 0;
            g_ar_miss_t = 0;
        }
        if (g_ar_blk != 0 && !is_block_at(g_ar_blk)) {
            g_ar_blk = 0;
            g_ar_ok  = 0;
        }
        if (g_ar_blk == 0) {
            if (g_ar_miss_t != 0 && t - g_ar_miss_t < 10'000) return;   // (a search again after 10 s: a scan may have found more blocks)
            int clip = -1, reserve = -1, cap = -1;
            fc5::lua::player_ammo(clip, reserve, &cap);
            g_ar_blk = find_block(obj, cap);
            if (g_ar_blk == 0) {
                if (g_ar_miss_t == 0) spdlog::info("[FullMag] weapon object {:#x}: no block of its magazine size ({}) found from it: the round kept in the clip as before", obj, cap);
                g_ar_miss_t = t;
                return;
            }
        }
        uint8_t v = 0xFF;
        if (!read(g_ar_blk + kAutoReload, &v, 1) || v > 1) {
            if (g_ar_miss_t == 0) spdlog::info("[FullMag] weapon block {:#x}: bAutoReload reads {}: left alone", g_ar_blk, int(v));
            g_ar_miss_t = t;
            g_ar_blk    = 0;
            return;
        }
        if (v == 1) {
            if (!write_b(g_ar_blk + kAutoReload, 0) || !read(g_ar_blk + kAutoReload, &v, 1) || v != 0) {
                spdlog::info("[FullMag] weapon block {:#x}: bAutoReload not written: the round kept in the clip", g_ar_blk);
                g_ar_miss_t = t;
                g_ar_blk    = 0;
                return;
            }
            if (std::none_of(g_ar_blocks.begin(), g_ar_blocks.end(), [](const ArBlock& b) { return b.a == g_ar_blk; })) g_ar_blocks.push_back({ g_ar_blk, 1 });
            spdlog::info("[FullMag] weapon block {:#x} (object {:#x}): the game's own reload at empty off: the whole magazine fires ({} block(s) written)", g_ar_blk, obj, g_ar_blocks.size());
        }
        g_ar_ok = obj;   // 0 already (written by us, or the weapon's own data)
    }

    // Sets one block to the wanted times (or back to its own with restore); false: the block no longer matches.
    bool apply(Block& b, bool restore)
    {
        if (!still_matches(b.a)) return false;
        const float d = restore ? b.draw0 : target(b.draw0, g_draw.load());
        const float h = restore ? b.holster0 : target(b.holster0, g_holster.load());
        bool ok = true;
        if (d != b.draw0 || b.written) ok &= write_f(b.a + kDraw, d);
        if (h != b.holster0 || b.written) ok &= write_f(b.a + kHolster, h);
        b.draw1    = d;
        b.holster1 = h;
        b.written  = !restore && (d != b.draw0 || h != b.holster0);
        return ok;
    }

    std::vector<uintptr_t> scan(size_t& bytes, int& regions, size_t& skipped)
    {
        std::vector<uintptr_t> found;
        std::vector<uint8_t>   buf((4u << 20) + kSpan);
        bytes   = 0;
        regions = 0;
        skipped = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            a = end;
            if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && gpu_mapped(m)) skipped += m.RegionSize;
            if (!readable(m) || m.RegionSize < kSpan) continue;
            regions++;
            // Chunks overlap by kSpan so a block across a chunk's end is seen in the next one.
            for (uintptr_t c = base; c + kSpan <= end;) {
                const size_t n = std::min<size_t>(buf.size(), end - c);
                if (!read(c, buf.data(), n)) { c += n; continue; }
                bytes += n;
                // The ladder time first (0.1-0.2 s as bits 0x3DCCCCCD-0x3E4CCCCD), then the whole pattern.
                for (size_t o = kLadder; o + (kSpan - kLadder) <= n; o += 4) {
                    uint32_t u;
                    std::memcpy(&u, buf.data() + o, 4);
                    if (u < 0x3DCCCCCDu || u > 0x3E4CCCCDu) continue;
                    if (is_block(c + o - kLadder, buf.data() + o - kLadder)) found.push_back(c + o - kLadder);
                }
                if (c + n >= end) break;
                c += n - kSpan;
            }
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        return found;
    }

    void do_scan(const char* why)
    {
        spdlog::info("[SwitchFast] scan {} ({}) started", g_scans + 1, why);   // its end line gives the time: a hitch in between is the scan's
        const auto t0 = clock::now();
        size_t bytes = 0, skipped = 0;
        int    regions = 0;
        const auto found = scan(bytes, regions, skipped);
        const float secs = std::chrono::duration<float>(clock::now() - t0).count();
        g_scan_secs = secs;
        std::lock_guard _{ g_mtx };
        int added = 0, fov_ok = 0, aligned16 = 0;
        for (uintptr_t a : found) {
            if (std::any_of(g_blocks.begin(), g_blocks.end(), [a](const Block& b) { return b.a == a; })) continue;
            uint8_t buf[kSpan];
            if (!read(a, buf, sizeof(buf))) continue;
            float fov = 0.0f;
            read(a + kNearFov, &fov, 4);
            if (in(fov, 40.0f, 100.0f)) fov_ok++;
            if ((a & 15) == 0) aligned16++;
            Block b{ a, f_at(buf, kDraw), f_at(buf, kHolster), 0.0f, 0.0f, false };
            if (g_on.load()) apply(b, false);
            g_blocks.push_back(b);
            added++;
        }
        g_count = int(g_blocks.size());
        g_written = int(std::count_if(g_blocks.begin(), g_blocks.end(), [](const Block& b) { return b.written; }));
        spdlog::info("[SwitchFast] scan {} ({}): {} weapon blocks found in {:.2f} s ({} MB in {} regions; {} MB write-combined or uncached left out), {} new "
                     "({} with a near FOV 40-100 at +0x1d5c, {} 16-byte aligned); {} held, {} sped up (draw {:.2f} s, holster {:.2f} s){}",
                     ++g_scans, why, found.size(), secs, bytes >> 20, regions, skipped >> 20, added, fov_ok, aligned16, g_blocks.size(), g_written.load(), g_draw.load(),
                     g_holster.load(), g_on.load() ? "" : " (switchfast off: only found)");
    }

    // Every block read again: dropped when its memory no longer holds the pattern, written again when the game changed it.
    void upkeep()
    {
        std::lock_guard _{ g_mtx };
        int dropped = 0, again = 0;
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            uint8_t buf[kSpan];
            if (!read(it->a, buf, sizeof(buf)) || !is_block(it->a, buf)) { it = g_blocks.erase(it); dropped++; continue; }
            if (g_on.load()) {
                const float d = f_at(buf, kDraw), h = f_at(buf, kHolster);
                const float wd = target(it->draw0, g_draw.load()), wh = target(it->holster0, g_holster.load());
                if (d != wd || h != wh) {
                    // Set back (or to something new) by the game: that is its own time now.
                    if (d != it->draw1) it->draw0 = d;
                    if (h != it->holster1) it->holster0 = h;
                    apply(*it, false);
                    again++;
                }
            }
            ++it;
        }
        g_count = int(g_blocks.size());
        g_written = int(std::count_if(g_blocks.begin(), g_blocks.end(), [](const Block& b) { return b.written; }));
        if (dropped >= 20) g_slow_ms = now_ms();   // a load freed weapons: look for the new ones (not for one or two, so no rescan loop)
        if (dropped || again) spdlog::info("[SwitchFast] {} weapon blocks gone (memory reused), {} written again; {} held", dropped, again, g_blocks.size());
    }

    void restore_all()
    {
        std::lock_guard _{ g_mtx };
        int n = 0;
        for (auto& b : g_blocks)
            if (b.written && apply(b, true)) n++;
        g_written = 0;
        spdlog::info("[SwitchFast] the game's own draw and holster times put back on {} weapon blocks", n);
    }

    void list(int n)
    {
        std::lock_guard _{ g_mtx };
        spdlog::info("[SwitchFast] {} weapon blocks:", g_blocks.size());
        for (const auto& b : g_blocks) {
            if (n-- <= 0) break;
            uint8_t buf[kSpan];
            float   fov = 0.0f;
            uint32_t name = 0;
            read(b.a + kNearFov, &fov, 4);
            read(b.a + 0x1d80, &name, 4);   // sUniquePickupName (a name id)
            if (!read(b.a, buf, sizeof(buf))) continue;
            spdlog::info("[SwitchFast]   {:#x}: draw {:.3f} (own {:.3f}), holster {:.3f} (own {:.3f}), throwable holster {:.3f}, ladder {:.3f}, machete {:.3f}, "
                         "trigger {:.2f}, near FOV {:.0f}, name id {:#x}",
                         b.a, f_at(buf, kDraw), b.draw0, f_at(buf, kHolster), b.holster0, f_at(buf, kThrow), f_at(buf, kLadder), f_at(buf, kMachete),
                         f_at(buf, kTrigger), fov, name);
        }
    }

    void worker()
    {
        // A whole scan reads several GB: kept off the game's and the render's cores' way.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        int64_t last_scan = 0, last_upkeep = 0;
        bool    was_on = g_on.load();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const int64_t t  = now_ms();
            const bool    on = g_on.load();
            if (g_restore_req.exchange(false) || (was_on && !on)) restore_all();
            const bool just_on = on && !was_on;
            was_on = on;
            const int  every = g_rescan_s.load();
            const bool empty = g_count.load() == 0;
            const bool slow  = g_slow_ms.load() > last_scan && t - last_scan > 10'000;
            const char* why  = g_scan_req.exchange(false) ? "asked" : last_scan == 0 ? "first" : just_on ? "switchfast on" : slow ? "a slow switch"
                             : (on && every > 0 && t - last_scan > int64_t(every) * 1000) ? "periodic" : (on && empty && t - last_scan > 15'000) ? "none found yet" : nullptr;
            if (why) {
                if (just_on) upkeep();   // the blocks held written at once, not after the scan's ~7 s (simulator 29 Oct)
                g_scan_at  = t;
                g_scanning = true;
                do_scan(why);
                g_scanning = false;
                last_scan = t;
            } else if (t - last_upkeep > 1000) {
                upkeep();
                last_upkeep = t;
            }
            if (const int n = g_list_req.exchange(0); n > 0) list(n);
            ar_tick();   // reloadfullmag
        }
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    (void)rest;
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "switchfast") {
        g_on    = on;
        persist = true;
        spdlog::info("[SwitchFast] switchfast {}: {}", on ? "on" : "off",
                     on ? "the game's weapon draw and holster animations shortened to switchdraw / switchholster" : "the game's own draw and holster times");
        return true;
    }
    if (cmd == "switchdraw" || cmd == "switchholster") {
        (cmd == "switchdraw" ? g_draw : g_holster) = std::clamp(arg.empty() ? (cmd == "switchdraw" ? 0.15f : 0.10f) : fv, 0.05f, 2.0f);
        persist = true;   // taken by the next upkeep (every block then differs from the new time)
        return true;
    }
    if (cmd == "switchrescan") { g_rescan_s = std::clamp(arg.empty() ? 300 : int(fv), 0, 3600); persist = true; return true; }
    if (cmd == "switchscan") { g_scan_req = true; return true; }                           // test: scan now
    if (cmd == "switchlist") { g_list_req = arg.empty() ? 64 : std::max(1, int(fv)); return true; }   // test: log the blocks held
    return false;
}

void autoreload_want(bool on, uintptr_t obj)
{
    g_ar_on   = on;
    g_ar_want = on ? obj : 0;
}

bool autoreload_off(uintptr_t obj) { return obj != 0 && !g_ar_failed.load() && g_ar_ok.load() == obj; }

void autoreload_failed()
{
    if (g_ar_failed.exchange(true)) return;
    g_ar_ok = 0;
    spdlog::info("[FullMag] the game reloaded by itself with its auto reload written off: put back, the round kept in the clip again for this session");
}

void frame()
{
    if (!g_started.exchange(true)) std::thread(worker).detach();
}

int scan_state(float& secs, float& took)
{
    // As the worker decides (a scan due after a slow switch or a load waits 10 s from the latest; none found: every 15 s).
    const int64_t t = now_ms(), at = g_scan_at.load();
    took = g_scan_secs.load();
    if (g_scanning.load()) { secs = float(t - at) / 1000.0f; return 2; }
    if (g_scan_req.load() || at == 0) { secs = 0.0f; return 1; }
    if (g_slow_ms.load() > at) { secs = std::max(0.0f, float(10'000 - (t - at)) / 1000.0f); return 1; }
    if (g_on.load() && g_count.load() == 0) { secs = std::max(0.0f, float(15'000 - (t - at)) / 1000.0f); return 3; }
    secs = 0.0f;
    return 0;
}

void note_switch(float secs)
{
    if (!g_on.load()) return;
    const float expect = g_draw.load() + g_holster.load() + 0.8f;
    spdlog::info("[SwitchFast] switch: new weapon drawn {:.2f} s after the gesture ({} weapon blocks held, {} sped up){}", secs, g_count.load(), g_written.load(),
                 secs > expect ? fmt::format("; slower than {:.2f} s: a rescan is due", expect) : std::string{});
    if (secs > expect) g_slow_ms = now_ms();
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("Faster weapon switch: the game's own draw and holster sped up##switchfast", &v)) cmds += v ? "switchfast on\n" : "switchfast off\n";
    hint("On (default): every weapon's draw and holster animations in the game play in the times below instead of the game's own "
         "(about 0.3 s to put a weapon away and 0.5-0.9 s to draw one), so a switch is quicker and the new weapon can fire sooner. "
         "Works for the shoulder and hip grabs and for the game's own weapon wheel and buttons.");
    if (!v) return;
    ImGui::Text("Weapons found in the game's memory: %d (%d sped up)", g_count.load(), g_written.load());
    float d = g_draw.load(), h = g_holster.load();
    if (ImGui::SliderFloat("Draw time (s)##switchdraw", &d, 0.05f, 1.0f, "%.2f")) cmds += fmt::format("switchdraw {:.2f}\n", d);
    if (ImGui::SliderFloat("Holster time (s)##switchholster", &h, 0.05f, 1.0f, "%.2f")) cmds += fmt::format("switchholster {:.2f}\n", h);
    hint("A weapon whose own time is already shorter keeps it.");
}
}
