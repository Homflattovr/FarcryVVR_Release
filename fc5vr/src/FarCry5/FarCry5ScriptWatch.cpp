// scriptwatch (test, log only; pass 3 of the systematic reverse engineering, author 1 Nov 22:05): watch any of the game's Lua script functions by name, to see which ones the
// game's own scripts call while something happens in play (a cutscene, a parachute, a weapon switch, a mission handing a weapon).
//
// The names come from the game's own registration code, found in memory at the first command as research\2026-10-30_engine_natives.md
// found them on disk (1,182 in this build): each registration is `lea rdx, fn; call +0x3303D0 (pushcclosure); lea r8, name; mov edx,
// idx; call +0x308BC0 (setfield)`. A watched function's binding (int(lua_State*)) gets an inline hook that counts the call, logs it
// (at most 5 lines a second per function) and passes it on unchanged.
//   scriptwatch list <part>   the names containing part (case-insensitive), with their binding address
//   scriptwatch <Name>        watch every binding registered under Name (a name registered twice, e.g. SkipCinematicSequence, gets both)
//   scriptwatch all           every binding counted, without a line per call (2 Nov: to find which ones a moment of play uses)
//   scriptwatch mark          the counts now kept;   scriptwatch top [N]   the N (20) most called since the mark (or since all)
//   scriptwatch off           every hook removed;   scriptwatch status   what is watched by name, with the call counts
//
// hookprobe (test, log only; 2 Nov, pass 3 going deeper, author 15:15): any function of the game's plain code by address, for what scripts never call (tools\codemap finds the address):
//   hookprobe <rva hex> [label]   a mid hook at that function's first instruction: at most 5 lines a second, each with who called it and its first four arguments (integers
//                                 and floats). Refused unless the exception table says a function starts exactly there, or it is a jmp stub into the protected code.
//   hookprobe list | hookprobe off
#include "FarCry5ScriptWatch.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

namespace fc5::scriptwatch
{
    namespace
    {
        struct Binding { std::string name; uint32_t rva; };
        std::vector<Binding> g_bind;
        bool                 g_scanned = false;
        uint8_t*             g_base    = nullptr;
        std::mutex           g_mx;

        constexpr int kSlots = 1280;   // all 1,182 bindings of this build fit
        struct Slot
        {
            safetyhook::InlineHook hook{};
            std::string            name;
            uint32_t               rva = 0;
            bool                   loud = true;   // a line per call (watched by name); false: counted only (scriptwatch all)
            uint64_t               mark = 0;      // calls at the last scriptwatch mark
            std::atomic<uint64_t>  calls{ 0 };
            std::atomic<int64_t>   sec{ 0 };
            std::atomic<int>       in_sec{ 0 };
        };
        Slot g_slot[kSlots];

        int64_t now_s() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        template <int N> int __fastcall hk(void* L)
        {
            Slot&      s = g_slot[N];
            const auto n = ++s.calls;
            if (!s.loud) return s.hook.call<int>(L);
            const auto t = now_s();
            if (s.sec.exchange(t) != t) s.in_sec = 0;
            if (++s.in_sec <= 5) spdlog::info("[ScriptWatch] {} called (call {}, binding FC_m64.dll+{:#x})", s.name, n, s.rva);
            return s.hook.call<int>(L);
        }

        using HookFn = void*;
        template <int... I> constexpr std::array<HookFn, sizeof...(I)> make_fns(std::integer_sequence<int, I...>) { return { reinterpret_cast<HookFn>(&hk<I>)... }; }
        const auto kFns = make_fns(std::make_integer_sequence<int, kSlots>{});

        const char* cstr_at(uint8_t* p, uint8_t* lo, uint8_t* hi)
        {
            if (p < lo || p >= hi) return nullptr;
            for (int i = 0; i < 80 && p + i < hi; ++i) {
                const uint8_t c = p[i];
                if (c == 0) return i >= 2 ? reinterpret_cast<const char*>(p) : nullptr;
                if (c < 0x20 || c > 0x7E) return nullptr;
            }
            return nullptr;
        }

        // The registrations in the plain code (the first section), as tools\codemap and the 30 Oct binders scan.
        void scan()
        {
            g_scanned = true;
            g_base    = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return;
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
            auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            uint8_t* code = g_base + sec[0].VirtualAddress;
            const size_t n = sec[0].Misc.VirtualSize;
            uint8_t* img_end = g_base + nt->OptionalHeader.SizeOfImage;
            uint8_t* setfield = g_base + 0x308BC0;
            uint8_t* pushcl   = g_base + 0x3303D0;
            // setfield is plain code (48 89 5C 24 08: mov [rsp+8],rbx), pushcclosure a jmp into the protected code.
            if (setfield[0] != 0x48 || setfield[1] != 0x89 || *pushcl != 0xE9) {
                spdlog::error("[ScriptWatch] the Lua helpers are not where this build has them (+0x308bc0, +0x3303d0): no names");
                return;
            }
            auto rel = [](uint8_t* at, int len) { return at + len + *reinterpret_cast<int32_t*>(at + len - 4); };
            for (size_t i = 0; i + 5 < n; ++i) {
                uint8_t* c = code + i;
                if (*c != 0xE8 || rel(c, 5) != setfield) continue;
                const char* name = nullptr;
                for (int j = 7; j < 48 && !name && i >= size_t(j); ++j) {
                    uint8_t* q = c - j;
                    if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05) name = cstr_at(rel(q, 7), g_base, img_end);
                }
                uint8_t* fn = nullptr;
                for (int j = 5; j < 64 && !fn && i >= size_t(j); ++j) {
                    uint8_t* q = c - j;
                    if (q[0] == 0xE8 && rel(q, 5) == pushcl) {
                        for (int k = 7; k < 40 && !fn; ++k) {
                            uint8_t* r = q - k;
                            if (r[0] == 0x48 && r[1] == 0x8D && r[2] == 0x15) fn = rel(r, 7);
                        }
                        break;
                    }
                }
                if (name && fn && fn > code && fn < code + n) g_bind.push_back({ name, uint32_t(fn - g_base) });
            }
            spdlog::info("[ScriptWatch] {} script functions found in the game's registration code", g_bind.size());
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }
    }

    namespace
    {
        constexpr int kProbes = 32;
        struct ProbeSlot
        {
            safetyhook::MidHook   hook{};
            std::string           label;
            uint32_t              rva = 0;
            std::atomic<uint64_t> calls{ 0 };
            std::atomic<int64_t>  sec{ 0 };
            std::atomic<int>      in_sec{ 0 };
        };
        ProbeSlot g_probe[kProbes];

        uintptr_t read_ptr(uintptr_t a)
        {
            __try {
                return *reinterpret_cast<uintptr_t*>(a);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        template <int N> void hk_probe(safetyhook::Context& c)
        {
            ProbeSlot& s = g_probe[N];
            const auto n = ++s.calls;
            const auto t = now_s();
            if (s.sec.exchange(t) != t) s.in_sec = 0;
            if (++s.in_sec > 5) return;
            const uintptr_t ret  = read_ptr(c.rsp);
            const auto      here = reinterpret_cast<uintptr_t>(g_base);
            spdlog::info("[Probe] {} +{:#x} call {} from {} rcx={:#x} rdx={:#x} r8={:#x} r9={:#x} xmm0={:.4f} xmm1={:.4f} xmm2={:.4f} xmm3={:.4f}", s.label, s.rva, n,
                         ret > here ? fmt::format("+{:#x}", ret - here) : std::string{ "?" }, c.rcx, c.rdx, c.r8, c.r9, c.xmm0.f32[0], c.xmm1.f32[0], c.xmm2.f32[0], c.xmm3.f32[0]);
        }
        template <int... I> constexpr std::array<void (*)(safetyhook::Context&), sizeof...(I)> make_probes(std::integer_sequence<int, I...>) { return { &hk_probe<I>... }; }
        const auto kProbeFns = make_probes(std::make_integer_sequence<int, kProbes>{});
    }

    std::string probe_command(const std::string& arg_in)
    {
        std::lock_guard lk(g_mx);
        if (!g_base) g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        if (!g_base) return "FC_m64.dll not loaded";
        std::string arg = arg_in;
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
        if (arg == "off") {
            int n = 0;
            for (auto& s : g_probe)
                if (s.hook) {
                    s.hook = {};
                    ++n;
                }
            return fmt::format("{} probes removed", n);
        }
        if (arg == "list" || arg.empty()) {
            std::string out;
            for (auto& s : g_probe)
                if (s.hook) out += fmt::format(" {} +{:#x} ({} calls)", s.label, s.rva, s.calls.load());
            return out.empty() ? std::string{ "none" } : out;
        }
        char*          e   = nullptr;
        const uint32_t rva = uint32_t(std::strtoul(arg.c_str(), &e, 16));
        std::string    label = e && *e ? std::string{ e + 1 } : fmt::format("{:x}", rva);
        auto*          dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        auto*          nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        auto*          sec = IMAGE_FIRST_SECTION(nt);
        if (rva < sec[0].VirtualAddress || rva >= sec[0].VirtualAddress + sec[0].Misc.VirtualSize) return fmt::format("+{:#x} is not in the plain code: refused", rva);
        DWORD64 img = 0;
        auto*   fe  = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(g_base) + rva, &img, nullptr);
        if (!(fe && fe->BeginAddress == rva) && g_base[rva] != 0xE9) return fmt::format("+{:#x} is not the start of a function: refused", rva);
        for (auto& s : g_probe)
            if (s.hook && s.rva == rva) return fmt::format("+{:#x} already probed", rva);
        int k = 0;
        while (k < kProbes && g_probe[k].hook) ++k;
        if (k == kProbes) return fmt::format("all {} probes in use", kProbes);
        g_probe[k].label = label;
        g_probe[k].rva   = rva;
        g_probe[k].calls = 0;
        g_probe[k].hook  = safetyhook::create_mid(g_base + rva, kProbeFns[k]);
        return g_probe[k].hook ? fmt::format("probing {} +{:#x}", label, rva) : fmt::format("+{:#x} could not be hooked", rva);
    }

    std::string command(const std::string& arg_in)
    {
        std::lock_guard lk(g_mx);
        if (!g_scanned) scan();
        std::string arg = arg_in;
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
        if (arg.rfind("list", 0) == 0) {
            const std::string part = lower(arg.size() > 5 ? arg.substr(5) : "");
            std::string out;
            int shown = 0, total = 0;
            for (const auto& b : g_bind) {
                if (lower(b.name).find(part) == std::string::npos) continue;
                ++total;
                if (shown < 60) {
                    out += fmt::format(" {} (+{:#x})", b.name, b.rva);
                    ++shown;
                }
            }
            return fmt::format("{} of {} names contain '{}':{}", total, g_bind.size(), part, out);
        }
        if (arg == "off") {
            int n = 0;
            for (auto& s : g_slot)
                if (s.hook) {
                    s.hook = {};
                    ++n;
                }
            return fmt::format("{} hooks removed", n);
        }
        if (arg == "status" || arg.empty()) {
            std::string out;
            int quiet = 0;
            for (auto& s : g_slot)
                if (s.hook && s.loud) out += fmt::format(" {} ({} calls)", s.name, s.calls.load());
                else if (s.hook) ++quiet;
            return fmt::format("{} names known; watched:{}; {} counted only", g_bind.size(), out.empty() ? " none" : out, quiet);
        }
        if (arg == "all") {
            const auto t0 = std::chrono::steady_clock::now();
            int added = 0, failed = 0, full = 0;
            int stubs = 0;
            for (const auto& b : g_bind) {
                // A binding that is a bare return (xor eax,eax / mov al,1 ... ret) is a stub the engine's own code shares (2 Nov:
                // CoopEnableInitialSpawn's +0xb7bbf0 is called ~90,000 times a second, also from vtables): not a script call, not counted.
                const uint8_t* f = g_base + b.rva;
                if (f[0] == 0xC3 || (f[0] == 0x33 && f[1] == 0xC0 && f[2] == 0xC3) || (f[0] == 0xB0 && f[2] == 0xC3) || (f[0] == 0x32 && f[1] == 0xC0 && f[2] == 0xC3)) {
                    ++stubs;
                    continue;
                }
                bool have = false;
                for (auto& s : g_slot) have = have || (s.hook && s.rva == b.rva);
                if (have) continue;
                int k = 0;
                while (k < kSlots && g_slot[k].hook) ++k;
                if (k == kSlots) {
                    ++full;
                    continue;
                }
                g_slot[k].name  = b.name;
                g_slot[k].rva   = b.rva;
                g_slot[k].loud  = false;
                g_slot[k].calls = 0;
                g_slot[k].mark  = 0;
                g_slot[k].hook  = safetyhook::create_inline(reinterpret_cast<void*>(g_base + b.rva), kFns[k]);
                if (g_slot[k].hook) ++added;
                else ++failed;
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            return fmt::format("counting {} bindings ({} could not be hooked, {} over the slots, {} bare stubs left out) in {} ms", added, failed, full, stubs, ms);
        }
        if (arg == "mark") {
            int n = 0;
            for (auto& s : g_slot)
                if (s.hook) {
                    s.mark = s.calls.load();
                    ++n;
                }
            return fmt::format("{} counts kept", n);
        }
        if (arg.rfind("top", 0) == 0) {
            const int n = std::clamp(arg.size() > 4 ? std::atoi(arg.c_str() + 4) : 20, 1, 200);
            std::vector<std::pair<uint64_t, const Slot*>> v;
            for (auto& s : g_slot)
                if (s.hook && s.calls.load() > s.mark) v.push_back({ s.calls.load() - s.mark, &s });
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string out;
            for (size_t i = 0; i < v.size() && int(i) < n; ++i) out += fmt::format(" {} {}", v[i].second->name, v[i].first);
            return fmt::format("{} bindings called since the mark; the most:{}", v.size(), out.empty() ? " none" : out);
        }
        int added = 0;
        std::string missing;
        for (const auto& b : g_bind) {
            if (b.name != arg) continue;
            bool have = false;
            for (auto& s : g_slot)
                if (s.hook && s.rva == b.rva) {
                    have = true;
                    if (!s.loud) {
                        s.loud = true;
                        ++added;
                    }
                }
            if (have) continue;
            int k = 0;
            while (k < kSlots && g_slot[k].hook) ++k;
            if (k == kSlots) return fmt::format("all {} slots in use ('scriptwatch off' first)", kSlots);
            g_slot[k].name  = b.name;
            g_slot[k].rva   = b.rva;
            g_slot[k].loud  = true;
            g_slot[k].calls = 0;
            g_slot[k].mark  = 0;
            g_slot[k].hook  = safetyhook::create_inline(reinterpret_cast<void*>(g_base + b.rva), kFns[k]);
            if (g_slot[k].hook) ++added;
            else missing += fmt::format(" +{:#x}", b.rva);
        }
        return added ? fmt::format("watching {} ({} binding{})", arg, added, added > 1 ? "s" : "")
                     : fmt::format("no binding named '{}' could be watched{}", arg, missing.empty() ? " (unknown name: try scriptwatch list)" : " (hook failed at" + missing + ")");
    }
}
