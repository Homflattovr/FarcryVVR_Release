// gamesound (2 Nov, author 13:29; research/2026-11-02_game_sounds.md): the game's own sounds, so manual reload can use the
// real magazine and bolt sounds. The Lua function PlaySoundByID's native (+0x2FAE250: (ndString* id, int param)) turns the text into a number
// (+0x7A5B50, GetSoundID: "0x..." hex or decimal: the Wwise event id, as the weapon data's sndSingleBulletShot = '1270045856'), gets the
// sound service (+0x7B0770, a jmp into protected code) and calls its vtable +0x280 (+0x7BA0E0): (service, void* out, uint32 id, int type, nullptr, nullptr). type is looked
// up in the service's table of sound types (32 bytes each: +0x10 the type, +0x1C enabled): a type with no enabled entry, or -1, plays nothing. The author's 2 Nov 15:46 log: 15 for a
// 1911's reload, 14 for the bow's, 11, 13, 21, 27 for others.
// PlaySoundAtPosition's native (+0x2FAE150) is the same with vtable +0x2A0 and a position. All read statically from this build, not run yet: the checks
// below refuse anything but these exact bytes, and a fault turns it off.
//
// post: the call made directly on the script thread, from the game tick (FarCry5Lua.cpp), never from the pad or render threads.
// soundwatch: the reload's sound ids are not in the weapon data (they are on the animations), so it logs what the game plays: a mid hook at the entry of
// the service's vtable slots +0x240 to +0x2E0 (the two calls above and their neighbours), logging the third and fourth arguments of each slot. Each distinct id
// is logged the first 3 times, at most 40 lines a second. Bare stubs the engine shares are not hooked. Nothing is changed in the calls.
#include "FarCry5GameSound.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

namespace fc5::gamesound
{
    namespace
    {
        using clock    = std::chrono::steady_clock;
        using GetSvcFn = void*(__fastcall*)();
        using Play2DFn = void*(__fastcall*)(void* svc, void* out, uint32_t id, int param, void* a, void* b);

        uint8_t*          g_base  = nullptr;
        GetSvcFn          g_get   = nullptr;
        bool              g_tried = false, g_ok = false;
        std::atomic<bool> g_failed{ false };

        struct Queued { uint32_t id; int param; clock::time_point when; };
        std::mutex            g_mx;
        std::vector<Queued>   g_queue;
        std::atomic<bool>     g_pending{ false };
        std::atomic<uint64_t> g_played{ 0 };

        // soundwatch
        constexpr int kFirst = 0x240 / 8, kSlots = (0x2E0 - 0x240) / 8 + 1;
        std::atomic<bool>     g_watch{ false };
        bool                  g_hooked = false;
        safetyhook::MidHook   g_hooks[kSlots]{};
        std::atomic<uint32_t> g_slot_calls[kSlots]{};
        std::mutex            g_seen_mx;
        std::map<uint64_t, int> g_seen;   // slot << 32 | id: times seen
        std::atomic<int64_t>  g_line_sec{ 0 };
        std::atomic<int>      g_line_n{ 0 };
        std::atomic<bool>     g_all{ false };   // soundwatch all: every call of slot +0x280 logged, with who called (not only each id's first 3)
        // manualreload33 (reloadsoundlearn): the handling sounds (type 15) the game played, with their time, the last 256 (the hooks installed for it as for
        // soundwatch; the same id again within 60 ms, the positional call's, once).
        struct Heard_ { clock::time_point when; uint32_t id; };
        std::atomic<bool>     g_record{ false };
        thread_local bool     t_own = false;   // manualreload36: inside the mod's own play(): not heard (reload7 took the drop's sound, 3863307842, for the game's)
        std::mutex            g_heard_mx;
        std::vector<Heard_>   g_heard;

        // The natives' bytes as read on 2 Nov: their calls to the service and to vtable +0x280 / +0x2A0.
        bool resolve()
        {
            g_tried = true;
            g_base  = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return false;
            static const uint8_t kCall280[]{ 0x41, 0xFF, 0x92, 0x80, 0x02, 0x00, 0x00 };   // call [r10+280h]
            static const uint8_t kCall2A0[]{ 0x41, 0xFF, 0x92, 0xA0, 0x02, 0x00, 0x00 };   // call [r10+2A0h]
            const bool ok = g_base[0x7B0770] == 0xE9 && std::memcmp(g_base + 0x2FAE2E5, kCall280, sizeof(kCall280)) == 0 && std::memcmp(g_base + 0x2FAE1FD, kCall2A0, sizeof(kCall2A0)) == 0 &&
                            g_base[0x2FAE2C5] == 0xE8 && g_base + 0x2FAE2CA + *reinterpret_cast<int32_t*>(g_base + 0x2FAE2C6) == g_base + 0x7B0770;
            if (!ok) {
                spdlog::error("[Sound] gamesound: unexpected code at the game's sound calls (FC_m64.dll+0x2fae250): not used");
                return false;
            }
            g_get = reinterpret_cast<GetSvcFn>(g_base + 0x7B0770);
            spdlog::info("[Sound] gamesound: the game's sound service (FC_m64.dll+0x7b0770, vtable +0x280) usable");
            return true;
        }

        void* service()
        {
            __try {
                return g_get();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        bool play_raw(void* svc, uint32_t id, int param, uint32_t& handle)
        {
            __try {
                uint64_t out[4]{};
                auto**   vt = *reinterpret_cast<void***>(svc);
                reinterpret_cast<Play2DFn>(vt[0x280 / 8])(svc, out, id, param, nullptr, nullptr);
                handle = uint32_t(out[0]);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool play(void* svc, uint32_t id, int param, uint32_t& handle)
        {
            t_own         = true;   // the hooks see this call: not the game's
            const bool ok = play_raw(svc, id, param, handle);
            t_own         = false;
            return ok;
        }

        void note(int slot, uint32_t a, uint32_t b)
        {
            g_slot_calls[slot].fetch_add(1, std::memory_order_relaxed);
            if (!t_own && g_record.load(std::memory_order_relaxed) && int32_t(b) == 15 && a != 0 && a != 0xFFFFFFFFu) {
                const auto      now = clock::now();
                std::lock_guard _{ g_heard_mx };
                bool            dup = false;
                for (auto it = g_heard.rbegin(); it != g_heard.rend() && now - it->when < std::chrono::milliseconds(60); ++it) dup = dup || it->id == a;
                if (!dup) {
                    g_heard.push_back({ now, a });
                    if (g_heard.size() > 256) g_heard.erase(g_heard.begin());
                }
            }
            if (!g_watch.load(std::memory_order_relaxed) || a == 0 || a == 0xFFFFFFFFu) return;
            int times = 0;
            {
                std::lock_guard _{ g_seen_mx };
                times = ++g_seen[(uint64_t(slot) << 32) | a];
            }
            if (times > 3 && !(g_all.load(std::memory_order_relaxed) && (kFirst + slot) * 8 == 0x280)) return;
            const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(clock::now().time_since_epoch()).count();
            if (g_line_sec.exchange(s) != s) g_line_n = 0;
            if (++g_line_n > 40) return;
            spdlog::info("[Sound] soundwatch: slot +{:#x}: id {} (next argument {}), time {} of this id", (kFirst + slot) * 8, a, int32_t(b), times);
        }

        // mute_handling (author 5 Nov, the lever): the game plays its pump, lever or bolt cycle by itself after a shot (1887: +0.33 s and +0.47 s), and the hand
        // plays it again. Id 0 at the entry of +0x280: the call's own check turns it away (lea eax,[r8-1]; ... ja to the way out, read statically). The mod's own sounds (t_own) go through.
        std::atomic<int64_t> g_mute_until{ 0 };
        std::atomic<bool>    g_mute_want{ false };
        template <int N> void hk(safetyhook::Context& c)
        {
            note(N, uint32_t(c.r8), uint32_t(c.r9));
            // (Type 12 too: the fired shell's landing, 1345025944 and 2411789246, +0.2 to +1.0 s: the casing is hidden (reloadcasinghide) and the hand throws its own: run final_b's recording; the author's second shell, by ear at least.)
            if ((kFirst + N) * 8 != 0x280 || t_own || (int32_t(c.r9) != 15 && int32_t(c.r9) != 12)) return;
            const int64_t until = g_mute_until.load(std::memory_order_relaxed);
            if (until == 0) return;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() < until) c.r8 = 0;
        }
        template <int... I> constexpr std::array<void (*)(safetyhook::Context&), sizeof...(I)> make_fns(std::integer_sequence<int, I...>) { return { &hk<I>... }; }
        const auto kFns = make_fns(std::make_integer_sequence<int, kSlots>{});

        void install(void* svc)
        {
            g_hooked = true;
            auto** vt = *reinterpret_cast<void***>(svc);
            int    n = 0, stubs = 0, failed = 0;
            for (int i = 0; i < kSlots; ++i) {
                auto* f = reinterpret_cast<uint8_t*>(vt[kFirst + i]);
                // Bare stubs are shared with the engine's other classes (+0xb7bbf0 is called 90,000 times a second).
                if (f[0] == 0xC3 || (f[0] == 0x33 && f[1] == 0xC0 && f[2] == 0xC3) || (f[0] == 0xB0 && f[2] == 0xC3) || (f[0] == 0x32 && f[1] == 0xC0 && f[2] == 0xC3)) {
                    ++stubs;
                    continue;
                }
                g_hooks[i] = safetyhook::create_mid(f, kFns[i]);
                if (g_hooks[i]) ++n;
                else ++failed;
            }
            spdlog::info("[Sound] soundwatch: {} of the sound service's calls watched (vtable FC_m64.dll+{:#x}; {} bare stubs left out, {} could not be hooked)", n,
                         reinterpret_cast<uint8_t*>(vt) - g_base, stubs, failed);
        }
    }

    bool post(uint32_t id, int type, float delay_s)
    {
        if (g_failed.load() || id == 0) return false;
        if (!g_tried) g_ok = resolve();
        if (!g_ok) return false;
        std::lock_guard _{ g_mx };
        if (g_queue.size() >= 16) return false;
        g_queue.push_back({ id, type, clock::now() + std::chrono::microseconds(int64_t(std::max(0.0f, delay_s) * 1e6f)) });
        g_pending = true;
        return true;
    }

    bool due() { return !g_failed.load() && (g_pending.load() || ((g_watch.load() || g_record.load() || g_mute_want.load()) && !g_hooked)); }

    void mute_handling(int ms)
    {
        g_mute_want  = true;
        g_mute_until = ms <= 0 ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() + ms;
    }

    void tick()
    {
        if (!g_tried) g_ok = resolve();
        if (!g_ok) return;
        void* svc = service();
        if (!svc) return;
        if ((g_watch.load() || g_record.load() || g_mute_want.load()) && !g_hooked) install(svc);
        std::vector<Queued> now_list;
        {
            std::lock_guard _{ g_mx };
            const auto      now = clock::now();
            for (size_t i = 0; i < g_queue.size();) {
                if (g_queue[i].when <= now) {
                    now_list.push_back(g_queue[i]);
                    g_queue.erase(g_queue.begin() + i);
                } else {
                    ++i;
                }
            }
            g_pending = !g_queue.empty();
        }
        for (const auto& q : now_list) {
            uint32_t handle = 0;
            if (!play(svc, q.id, q.param, handle)) {
                g_failed = true;
                spdlog::error("[Sound] gamesound: a fault playing id {}: turned off (the mod's own sounds are used)", q.id);
                return;
            }
            g_played++;
            // The call writes the playing sound's handle; 0: refused (a sound type with no entry, the sound system not ready).
            if (g_played.load() <= 40 || handle == 0) spdlog::info("[Sound] gamesound: id {} type {}: {}", q.id, q.param, handle ? fmt::format("playing (handle {:#x})", handle) : std::string{ "refused (no handle)" });
        }
    }

    bool watch_on() { return g_watch.load(); }

    void record(bool on)
    {
        g_record = on;
        if (!on) {
            std::lock_guard _{ g_heard_mx };
            g_heard.clear();
        }
    }

    std::vector<Heard> heard(std::chrono::steady_clock::time_point since)
    {
        std::vector<Heard> r;
        std::lock_guard    _{ g_heard_mx };
        for (const auto& h : g_heard)
            if (h.when >= since - std::chrono::milliseconds(100)) r.push_back({ std::chrono::duration<float>(h.when - since).count(), h.id });
        return r;
    }

    bool command(const std::string& cmd, const std::string& arg, std::string& reply)
    {
        if (cmd == "soundwatch") {
            g_watch = arg == "on" || arg == "1" || arg == "true" || arg == "all";
            g_all   = arg == "all";
            {
                std::lock_guard _{ g_seen_mx };
                g_seen.clear();
            }
            reply = g_watch.load() ? "the game's sound ids logged" : "off";
            return true;
        }
        if (cmd == "soundplay") {   // soundplay <id> [param] (test)
            const uint32_t id    = uint32_t(std::strtoul(arg.c_str(), nullptr, 0));
            const size_t   sp    = arg.find(' ');
            const int      type  = sp == std::string::npos ? 15 : std::atoi(arg.c_str() + sp + 1);
            reply = post(id, type) ? fmt::format("id {} queued (sound type {})", id, type) : "the game's sound call is not usable";
            return true;
        }
        if (cmd == "soundstatus") {
            std::string calls;
            for (int i = 0; i < kSlots; ++i)
                if (g_hooks[i]) calls += fmt::format(" +{:#x}:{}", (kFirst + i) * 8, g_slot_calls[i].load());
            std::lock_guard _{ g_seen_mx };
            reply = fmt::format("usable {}, failed {}, {} played; watch {}, {} distinct ids; calls per slot:{}", g_ok, g_failed.load(), g_played.load(), g_watch.load(), g_seen.size(),
                                calls.empty() ? " none" : calls);
            return true;
        }
        return false;
    }
}
