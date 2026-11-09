// Steering lab (test, never saved; steering thread, author 7 Nov 21:37). research\2026-11-07_steerlag\README.md.
//
// What the game does with the left stick's X in a wheeled vehicle (cars, quads, motorcycles, trucks), from its data and FC_m64.dll:
//  - Every wheeled vehicle's properties (patch.dat entry FCFA35BB9B788D77, the "VehicleProperties" library, 241 templates) hold a
//    WheeledPhysParams > Steering object: bUseTimedSteering (1 in all 241), fMaxSpeed, fLowMaxAngle / fHighMaxAngle (steer angle at zero /
//    max speed), fLowDirectMaxAngle / fHighDirectMaxAngle, fLowSteerSpeed / fHighSteerSpeed, fTimerSteerThreshold ("the threshold to switch
//    to timer based steering"). The game's own description of TimedSteerMaxSpeed: "Controls max speed for the change of steer angle. Means
//    it won't use the input directly for fast movements": the timed steering eases the wheels at a capped rate.
//    Cars and pick-ups: 12 or 10, 40 / 40 deg, direct 0 / 5 deg, steer speed 10 / 10, threshold 0.95. Quad 10, 45 / 45, 0 / 5, 10 / 10,
//    0.95. Trucks and trailers: steer speed 3 / 3. The old cars (FCO vintage, taxi, tow truck, Scavenger 4D, TukTuk large): steer speed
//    1 / 2.5, threshold 0.7. Motorcycles: 42 / 42 and 10 / 10, or 32 / 10 with direct 20 / 5, steer speed 1 / 0.5, threshold 0.7.
//  - The registration (FC_m64.dll rva 0x70e3720.., protected part; lea edx, [r9+off] with r9 = 0 before each name) places them in the
//    reflected object: +0x5c fMaxSteer, +0x60 fMaxSpeedFullSteeringAngle (the "Normal" steering: Havok's hkpVehicleDefaultSteering),
//    +0x65 bUseTimedSteering (byte), +0x68 fLowDirectMaxAngle, +0x6c fHighDirectMaxAngle, +0x70 fLowMaxAngle, +0x74 fHighMaxAngle,
//    +0x78 fMaxSpeed, +0x7c fLowSteerSpeed, +0x80 fHighSteerSpeed, +0x84 fTimerSteerThreshold.
//  - The code that applies them is in the protected part (no readable function reads +0x84 with its neighbours), so what each value does
//    is measured here, not read: the blocks are found by their values (as switchfast's weapon blocks), each field can be set or scaled
//    live on all of them (steerset), and steerprobe logs the car's yaw rate every frame against the stick X the game's poll read.
//
//   steerscan                 find the blocks now (one pass over the writable private memory on a worker thread, several GB: a hitch)
//   steerlist [N]             log the first N blocks held (16) with their values
//   steerset FIELD V          FIELD on every block = V (timed 0|1, lodir, hidir, lomax, himax, rate, losteer, histeer, thr, maxsteer, fullspeed)
//   steerset FIELD xK         FIELD = the game's own value times K
//   steerset FIELD off        the game's own value again;  steerset off: every field
//   steerprobe [S]            S seconds (15) of one line per frame while driving: stick, yaw rate, speed;  steerprobe off
//   steerstep RT X1 MS1 [X2 MS2 ...]   hands the game RT (0-255) and a left stick X of X1 for MS1 ms, then X2 for MS2 ms ... (up to 8),
//                             over whatever the controllers send (both pad paths), and runs steerprobe through it and 2 s after.
//                             Example: steerstep 255 0 2000 1 1500 0 1500 (2 s straight, 1.5 s full right, 1.5 s straight).
//   steerfind auto [wide]     round 2 (the 23:29 run: no steerset change moved the car's answer, so the blocks above are not what the car
//                             reads): the car's LIVE steering angle, found by what it does with the car standing (RT 0): the stick held
//                             right, left, centred, right again, half right (about 15 s, four memory passes on the worker thread: hitches).
//                             The values at +-40 deg (rad or deg) that flip sign, go to zero and come back are kept; the first 4 are watched
//                             (steerprobe adds them to its line). wide: any value of 6-69 deg instead of 40 (more memory, slower).
//   steerfind list | off      log the survivors and the floats around them (the runtime steering values should be near) | forget them
//   steersweep                with an angle watched and the car standing: the stick stepped from centre to 0.05, 0.10 ... 1.00 and back,
//                             the angle's final value, t50 and t90 logged for each (about 45 s)
//   steerpeek K OFF [N]       log N floats (16) at watched angle K + OFF (hex, may be negative)
//   steerpoke K OFF V         write the float V at watched angle K + OFF once (test; the game may write it back)
// Nothing is written to the game until steerset or steerpoke is used; steerset off and a restart put the game's values back.
#include "FarCry5SteerTune.h"

#include <windows.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <istream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fc5::steertune
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr size_t kFirst = 0x5c;            // the first byte read: fMaxSteer
    constexpr size_t kEnd   = 0x88;            // past fTimerSteerThreshold
    constexpr size_t kSpan  = kEnd - kFirst;   // 44 bytes
    constexpr size_t kThr   = 0x84;

    struct Field
    {
        const char* name;
        size_t      off;    // from the reflected object's start (the registration's offset)
        bool        byte;
        const char* what;
    };
    constexpr std::array<Field, 11> kFields{ {
        { "maxsteer", 0x5c, false, "fMaxSteer (default steering)" },
        { "fullspeed", 0x60, false, "fMaxSpeedFullSteeringAngle (default steering)" },
        { "timed", 0x65, true, "bUseTimedSteering" },
        { "lodir", 0x68, false, "fLowDirectMaxAngle" },
        { "hidir", 0x6c, false, "fHighDirectMaxAngle" },
        { "lomax", 0x70, false, "fLowMaxAngle" },
        { "himax", 0x74, false, "fHighMaxAngle" },
        { "rate", 0x78, false, "fMaxSpeed (TimedSteerMaxSpeed)" },
        { "losteer", 0x7c, false, "fLowSteerSpeed" },
        { "histeer", 0x80, false, "fHighSteerSpeed" },
        { "thr", 0x84, false, "fTimerSteerThreshold" },
    } };

    struct Ovr
    {
        int   mode{ 0 };   // 0 the game's, 1 set, 2 times
        float v{ 0.0f };
    };

    struct Block
    {
        uintptr_t a;                          // the reflected object's start (a + kFirst is read)
        uint8_t   orig[kSpan];                // the game's own bytes
        uint8_t   want[kSpan];                // what is meant to be there now (orig with the overrides)
        uint8_t   last[kSpan];                // what was last written (or read back): not a sign of reused memory
    };

    std::mutex                          g_mtx;
    std::vector<Block>                  g_blocks;
    std::array<Ovr, kFields.size()>     g_ovr{};
    std::atomic<bool>                   g_started{ false }, g_scan_req{ false }, g_apply_req{ false }, g_scanning{ false };
    std::atomic<int>                    g_list_req{ 0 }, g_count{ 0 }, g_scans{ 0 };

    float f_at(const uint8_t* p, size_t off) { float f; std::memcpy(&f, p + off - kFirst, 4); return f; }
    void  f_put(uint8_t* p, size_t off, float f) { std::memcpy(p + off - kFirst, &f, 4); }
    bool  in(float f, float lo, float hi) { return std::isfinite(f) && f >= lo && f <= hi; }
    uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

    bool gpu_mapped(const MEMORY_BASIC_INFORMATION& m) { return (m.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0; }
    bool readable(const MEMORY_BASIC_INFORMATION& m)
    {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
        if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (gpu_mapped(m)) return false;   // the driver's GPU-visible memory: never read (FarCry5SwitchSpeed.cpp)
        return (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    }
    bool read(uintptr_t a, void* out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
    }
    bool write(uintptr_t a, const void* p, size_t n)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), p, n, &put) && put == n;
    }

    // p: the 44 bytes at a block's +0x5c. The threshold exactly one of the data's three values, the rest in the data's ranges (degrees or
    // radians alike).
    bool matches(const uint8_t* p)
    {
        const uint32_t t = bits(f_at(p, kThr));
        if (t != bits(0.95f) && t != bits(0.7f) && t != bits(0.98f)) return false;
        if (p[0x65 - kFirst] > 1) return false;
        return in(f_at(p, 0x68), 0.0f, 30.0f) && in(f_at(p, 0x6c), 0.0f, 30.0f) && in(f_at(p, 0x70), 0.05f, 60.0f) && in(f_at(p, 0x74), 0.05f, 60.0f) &&
               in(f_at(p, 0x78), 0.5f, 40.0f) && in(f_at(p, 0x7c), 0.1f, 40.0f) && in(f_at(p, 0x80), 0.1f, 40.0f);
    }

    void make_want(Block& b)
    {
        std::memcpy(b.want, b.orig, kSpan);
        for (size_t i = 0; i < kFields.size(); ++i) {
            const auto& f = kFields[i];
            const Ovr   o = g_ovr[i];
            if (o.mode == 0) continue;
            if (f.byte) {
                const float own = float(b.orig[f.off - kFirst]);
                b.want[f.off - kFirst] = uint8_t(std::clamp(o.mode == 1 ? o.v : own * o.v, 0.0f, 1.0f) >= 0.5f ? 1 : 0);
            } else {
                f_put(b.want, f.off, o.mode == 1 ? o.v : f_at(b.orig, f.off) * o.v);
            }
        }
    }
    // Writes what differs from want (field by field); false if the block's memory no longer holds it.
    bool put(Block& b)
    {
        uint8_t cur[kSpan];
        if (!read(b.a + kFirst, cur, kSpan)) return false;
        if (std::memcmp(cur, b.want, kSpan) == 0) { std::memcpy(b.last, cur, kSpan); return true; }
        // Neither the game's values nor what was written last: the game changed them, or the memory holds something else now. (Before
        // 8 Nov the block's own override counted as reused memory: steerset off after steerset thr 2 dropped all 480 blocks, 23:32:38.)
        if (std::memcmp(cur, b.orig, kSpan) != 0 && std::memcmp(cur, b.last, kSpan) != 0) {
            if (!matches(cur)) return false;   // reused memory
            std::memcpy(b.orig, cur, kSpan);   // the game changed its own values: those are its own now
            make_want(b);
        }
        for (const auto& f : kFields) {
            const size_t o = f.off - kFirst, n = f.byte ? 1 : 4;
            if (std::memcmp(cur + o, b.want + o, n) != 0 && !write(b.a + f.off, b.want + o, n)) return false;
        }
        std::memcpy(b.last, b.want, kSpan);
        return true;
    }

    std::string values(const uint8_t* p)
    {
        return fmt::format("timed {} rate {:g} angle {:g}/{:g} direct {:g}/{:g} steer speed {:g}/{:g} thr {:g} (default steering: max {:g}, full-angle speed {:g})",
                           p[0x65 - kFirst], f_at(p, 0x78), f_at(p, 0x70), f_at(p, 0x74), f_at(p, 0x68), f_at(p, 0x6c), f_at(p, 0x7c), f_at(p, 0x80), f_at(p, 0x84),
                           f_at(p, 0x5c), f_at(p, 0x60));
    }

    void do_scan()
    {
        const auto t0 = clock::now();
        std::vector<uintptr_t> found;
        std::vector<uint8_t>   buf((4u << 20) + kEnd);
        size_t                 bytes = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            a = end;
            if (!readable(m) || m.RegionSize < kEnd) continue;
            for (uintptr_t c = base; c + kEnd <= end;) {
                const size_t n = std::min<size_t>(buf.size(), end - c);
                if (!read(c, buf.data(), n)) { c += n; continue; }
                bytes += n;
                // The threshold first (its exact bits), then the whole run; the object 8-byte aligned.
                for (size_t o = kThr; o + 4 <= n; o += 4) {
                    uint32_t u;
                    std::memcpy(&u, buf.data() + o, 4);
                    if (u != 0x3F733333u && u != 0x3F333333u && u != 0x3F7AE148u) continue;
                    const uintptr_t obj = c + o - kThr;
                    if ((obj & 7) == 0 && matches(buf.data() + o - kThr + kFirst)) found.push_back(obj);
                }
                if (c + n >= end) break;
                c += n - kEnd;
            }
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        const float secs = std::chrono::duration<float>(clock::now() - t0).count();
        std::lock_guard _{ g_mtx };
        int added = 0;
        for (uintptr_t a : found) {
            if (std::any_of(g_blocks.begin(), g_blocks.end(), [a](const Block& b) { return b.a == a; })) continue;
            Block b{};
            b.a = a;
            if (!read(a + kFirst, b.orig, kSpan) || !matches(b.orig)) continue;
            std::memcpy(b.last, b.orig, kSpan);
            make_want(b);
            put(b);
            g_blocks.push_back(b);
            added++;
        }
        g_count = int(g_blocks.size());
        // The distinct value sets, as in the data (README): a set not in the data is a false find.
        std::vector<std::pair<std::string, int>> sets;
        for (const auto& b : g_blocks) {
            const std::string v = values(b.orig);
            auto it = std::find_if(sets.begin(), sets.end(), [&](const auto& s) { return s.first == v; });
            if (it == sets.end()) sets.push_back({ v, 1 }); else it->second++;
        }
        spdlog::info("[Steer] scan {}: {} steering blocks found in {:.2f} s ({} MB read), {} new, {} held; {} value sets:", ++g_scans, found.size(), secs, bytes >> 20, added,
                     g_blocks.size(), sets.size());
        for (const auto& s : sets) spdlog::info("[Steer]   x{} {}", s.second, s.first);
    }

    void upkeep()
    {
        std::lock_guard _{ g_mtx };
        int dropped = 0;
        const bool remake = g_apply_req.exchange(false);
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            if (remake) make_want(*it);
            if (!put(*it)) { it = g_blocks.erase(it); dropped++; continue; }
            ++it;
        }
        g_count = int(g_blocks.size());
        if (dropped) spdlog::info("[Steer] {} steering blocks gone (memory reused); {} held", dropped, g_blocks.size());
        if (remake) {
            std::string s;
            for (size_t i = 0; i < kFields.size(); ++i)
                if (g_ovr[i].mode) s += fmt::format(" {}{}{:g}", kFields[i].name, g_ovr[i].mode == 1 ? "=" : "=x", g_ovr[i].v);
            spdlog::info("[Steer] {} steering blocks now:{}", g_blocks.size(), s.empty() ? " the game's own values" : s);
        }
    }

    void list(int n)
    {
        std::lock_guard _{ g_mtx };
        spdlog::info("[Steer] {} steering blocks held:", g_blocks.size());
        for (const auto& b : g_blocks) {
            if (n-- <= 0) break;
            uint8_t cur[kSpan];
            if (!read(b.a + kFirst, cur, kSpan)) continue;
            spdlog::info("[Steer]   {:#x}: {}{}", b.a, values(cur), std::memcmp(cur, b.orig, kSpan) ? fmt::format(" (own: {})", values(b.orig)) : std::string{});
        }
    }

    // steerfind / steersweep (worker thread; the stick it holds is read by step_value).
    struct Cand { uintptr_t a; float ref; };
    std::vector<Cand>      g_cand;
    std::atomic<int>       g_find_req{ 0 };              // 1 auto, 2 auto wide
    std::atomic<bool>      g_sweep_req{ false }, g_cand_list_req{ false }, g_cand_clear_req{ false };
    std::atomic<bool>      g_hold_on{ false };
    std::atomic<float>     g_hold_x{ 0.0f };
    std::atomic<bool>      g_finding{ false };
    constexpr int          kWatch = 4;
    std::atomic<uintptr_t> g_watch[kWatch]{};
    std::atomic<float>     g_watch_ref[kWatch]{};
    std::atomic<int>       g_watch_n{ 0 };

    void hold(float x, int ms)
    {
        g_hold_x  = x;
        g_hold_on = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    float to_deg(float v, float ref) { return std::fabs(ref) < 3.2f ? v * 57.29578f : v; }   // radians if the 40 deg value was ~0.7
    // By the bits: the build's /fp:fast lets infinities through float tests (8 Nov run: every survivor had -inf at pass 1, and -inf passes
    // |v + r| <= 0.03 |r| for any v).
    bool  finite_b(float v) { return (bits(v) & 0x7F800000u) != 0x7F800000u; }
    bool  read_f(uintptr_t a, float& v) { return read(a, &v, 4) && finite_b(v); }

    // Pass 1, the stick held full right: every 4-aligned float near +-40 deg (0.69813 rad or 40.0), or (wide) of 0.1-1.2 rad.
    void find_first(bool wide)
    {
        g_cand.clear();
        g_cand.shrink_to_fit();
        const size_t cap = wide ? 12'000'000u : 4'000'000u;   // 16 bytes each
        g_cand.reserve(wide ? 4'000'000u : 1'000'000u);
        std::vector<uint8_t> buf(4u << 20);
        const auto t0 = clock::now();
        size_t bytes = 0;
        bool   full = false;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && !full;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            a = end;
            if (!readable(m)) continue;
            for (uintptr_t c = base; c < end && !full; c += buf.size()) {
                const size_t n = std::min<size_t>(buf.size(), end - c) & ~size_t(3);
                if (!n || !read(c, buf.data(), n)) continue;
                bytes += n;
                for (size_t o = 0; o + 4 <= n; o += 4) {
                    float v;
                    std::memcpy(&v, buf.data() + o, 4);
                    if (!finite_b(v)) continue;
                    const float f = std::fabs(v);
                    const bool  ok = wide ? (f >= 0.1f && f <= 1.2f) : (std::fabs(f - 0.6981317f) <= 0.004f || std::fabs(f - 40.0f) <= 0.25f);
                    if (!ok) continue;
                    // Not the list itself (its refs are such values) nor the read buffer.
                    const uintptr_t at = c + o, lv = reinterpret_cast<uintptr_t>(g_cand.data()), bv = reinterpret_cast<uintptr_t>(buf.data());
                    if ((at >= lv && at < lv + g_cand.capacity() * sizeof(Cand)) || (at >= bv && at < bv + buf.size())) continue;
                    if (g_cand.size() >= cap) { full = true; break; }
                    g_cand.push_back({ c + o, v });
                }
            }
        }
        spdlog::info("[Steer] steerfind pass 1 (held right): {} values{} in {:.2f} s ({} MB read){}", g_cand.size(), wide ? " of 6-69 deg" : " at +-40 deg",
                     std::chrono::duration<float>(clock::now() - t0).count(), bytes >> 20, full ? " (the cap: the rest of memory not read)" : "");
    }

    // Later passes: keep the candidates whose value now passes keep(value, value at pass 1). Read in 64 KB pieces (the list is sorted).
    template <class Keep>
    void narrow(const char* what, Keep keep)
    {
        std::vector<uint8_t> buf(64 * 1024);
        uintptr_t lo = 0, hi = 0;
        size_t    kept = 0;
        for (size_t i = 0; i < g_cand.size(); ++i) {
            const Cand c = g_cand[i];
            if (c.a < lo || c.a + 4 > hi) {
                lo = c.a;
                hi = 0;
                size_t n = buf.size();
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(c.a), &m, sizeof(m)) || !readable(m)) continue;
                n = std::min<size_t>(n, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize - c.a);
                if (n < 4 || !read(c.a, buf.data(), n)) continue;
                hi = c.a + n;
            }
            float v;
            std::memcpy(&v, buf.data() + (c.a - lo), 4);
            if (finite_b(v) && keep(v, c.ref)) g_cand[kept++] = c;
        }
        g_cand.resize(kept);
        spdlog::info("[Steer] steerfind {}: {} left", what, kept);
    }

    void cand_list(size_t n)
    {
        spdlog::info("[Steer] steerfind: {} candidates{}", g_cand.size(), g_cand.size() > n ? fmt::format(" (the first {})", n) : std::string{});
        for (size_t i = 0; i < g_cand.size() && i < n; ++i) {
            const Cand& c = g_cand[i];
            float v = 0.0f;
            read_f(c.a, v);
            spdlog::info("[Steer]   {}: {:#x} now {:+.4f} ({:+.2f} deg), held right {:+.4f}", i, c.a, v, to_deg(v, c.ref), c.ref);
            if (i >= 6) continue;
            // The floats around it (the runtime steering values should be near: 40 deg, 0.95, 10, 12, 5 deg ...), plausible ones only.
            uint8_t near_[0x200];
            if (!read(c.a - 0x100, near_, sizeof(near_))) continue;
            std::string s;
            for (int o = 0; o < 0x200; o += 4) {
                float f;
                std::memcpy(&f, near_ + o, 4);
                const float af = std::fabs(f);
                if (!finite_b(f) || af < 1e-3f || af > 1e4f) continue;
                s += fmt::format(" {}{:#x}:{:g}{}", o < 0x100 ? "-" : "+", o < 0x100 ? 0x100 - o : o - 0x100, f, bits(f) == 0x3F733333u ? "(0.95!)" : "");
            }
            spdlog::info("[Steer]     around:{}", s);
        }
    }

    void find_auto(bool wide)
    {
        g_finding = true;
        spdlog::info("[Steer] steerfind auto{}: the car standing, RT 0; the stick held right, left, centred, right, half right (about 15 s; hitches)", wide ? " wide" : "");
        hold(1.0f, 2000);
        find_first(wide);
        hold(-1.0f, 2500);
        narrow("pass 2 (held left: the opposite)", [](float v, float r) { return std::fabs(v + r) <= 0.03f * std::fabs(r); });
        hold(0.0f, 2000);
        narrow("pass 3 (centred: zero)", [](float v, float r) { return std::fabs(v) <= 0.01f * std::fabs(r); });
        hold(1.0f, 2500);
        narrow("pass 4 (held right again: the same)", [](float v, float r) { return std::fabs(v - r) <= 0.03f * std::fabs(r); });
        hold(0.5f, 2000);
        // Logged, not filtered: the angle at half stick, as a share of the full one (the response curve's middle).
        for (size_t i = 0; i < g_cand.size() && i < 12; ++i) {
            float v = 0.0f;
            if (read_f(g_cand[i].a, v)) spdlog::info("[Steer] steerfind half right: {}: {:+.2f} deg = {:.3f} of the full angle", i, to_deg(v, g_cand[i].ref), v / g_cand[i].ref);
        }
        g_hold_on = false;
        const int n = int(std::min<size_t>(g_cand.size(), kWatch));
        for (int i = 0; i < n; ++i) { g_watch[i] = g_cand[i].a; g_watch_ref[i] = g_cand[i].ref; }
        g_watch_n = n;
        cand_list(24);
        if (g_cand.empty()) spdlog::info("[Steer] steerfind: nothing followed the stick (standing still may freeze the wheels: try it rolling slowly, or steerfind auto wide)");
        else spdlog::info("[Steer] steerfind: {} watched (steerprobe lines add them as 'angle')", n);
        g_finding = false;
    }

    // With an angle watched and the car standing: centre, then x for 1.2 s; the final angle, t50 and t90 of each watched angle.
    void sweep_step(float from, float to, int ms)
    {
        hold(from, 900);
        float a0[kWatch]{}, a1[kWatch]{};
        const int n = g_watch_n.load();
        for (int k = 0; k < n; ++k) read_f(g_watch[k].load(), a0[k]);
        struct S { int64_t us; float v[kWatch]; };
        std::vector<S> tr;
        g_hold_x = to;
        const auto t0 = clock::now();
        while (clock::now() - t0 < std::chrono::milliseconds(ms)) {
            S s{};
            s.us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
            for (int k = 0; k < n; ++k) read_f(g_watch[k].load(), s.v[k]);
            tr.push_back(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int k = 0; k < n; ++k) a1[k] = tr.empty() ? 0.0f : tr.back().v[k];
        std::string out;
        for (int k = 0; k < n; ++k) {
            const float d = a1[k] - a0[k], ref = g_watch_ref[k].load();
            int64_t t10 = -1, t50 = -1, t90 = -1;
            for (const auto& s : tr) {
                const float f = std::fabs(d) > 1e-6f ? (s.v[k] - a0[k]) / d : 0.0f;
                if (t10 < 0 && f >= 0.1f) t10 = s.us;
                if (t50 < 0 && f >= 0.5f) t50 = s.us;
                if (t90 < 0 && f >= 0.9f) { t90 = s.us; break; }
            }
            out += fmt::format(" | {}: {:+.2f} -> {:+.2f} deg (x{:.3f} of full) t10 {} t50 {} t90 {} ms", k, to_deg(a0[k], ref), to_deg(a1[k], ref), ref != 0.0f ? a1[k] / ref : 0.0f,
                               t10 < 0 ? -1 : t10 / 1000, t50 < 0 ? -1 : t50 / 1000, t90 < 0 ? -1 : t90 / 1000);
        }
        spdlog::info("[Steer] sweep {:+.2f} -> {:+.2f} ({} samples){}", from, to, tr.size(), out);
    }
    void sweep()
    {
        if (g_watch_n.load() == 0) { spdlog::info("[Steer] steersweep: no angle watched (steerfind auto first)"); return; }
        g_finding = true;
        spdlog::info("[Steer] steersweep: the car standing, RT 0; centre to x for 1.2 s, x = 0.05 ... 1.00; then releases and reversals (about 45 s)");
        for (int i = 1; i <= 20; ++i) sweep_step(0.0f, 0.05f * float(i), 1200);
        sweep_step(1.0f, 0.0f, 1200);
        sweep_step(0.5f, 0.0f, 1200);
        sweep_step(0.5f, 1.0f, 1200);
        sweep_step(1.0f, 0.5f, 1200);
        sweep_step(1.0f, -1.0f, 1500);
        sweep_step(0.94f, 0.96f, 1200);
        sweep_step(0.96f, 0.94f, 1200);
        g_hold_on = false;
        spdlog::info("[Steer] steersweep over");
        g_finding = false;
    }

    void worker()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        auto last = clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_scan_req.exchange(false)) {
                g_scanning = true;
                do_scan();
                g_scanning = false;
            }
            if (g_apply_req.load() || clock::now() - last > std::chrono::seconds(1)) {
                upkeep();
                last = clock::now();
            }
            if (const int n = g_list_req.exchange(0); n > 0) list(n);
            if (const int f = g_find_req.exchange(0); f > 0) find_auto(f == 2);
            if (g_sweep_req.exchange(false)) sweep();
            if (g_cand_list_req.exchange(false)) cand_list(24);
            if (g_cand_clear_req.exchange(false)) { g_cand.clear(); g_cand.shrink_to_fit(); g_watch_n = 0; spdlog::info("[Steer] steerfind off: nothing watched"); }
        }
    }
    void start() { if (!g_started.exchange(true)) std::thread(worker).detach(); }

    // The game's poll (note_poll): the stick X it was handed and when; poll intervals for the probe's summary.
    std::atomic<float>   g_poll_x{ 0.0f };
    std::atomic<int64_t> g_poll_us{ 0 };
    std::atomic<int>     g_polls{ 0 };
    std::atomic<int64_t> g_poll_gap_max{ 0 }, g_poll_gap_sum{ 0 };
    int64_t us_now() { return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count(); }

    // steerstep (under g_step_mtx).
    std::mutex g_step_mtx;
    struct Seg { float x; int ms; };
    std::vector<Seg>   g_steps;
    int                g_step_rt = 0;
    clock::time_point  g_step_t0{};
    bool               g_step_on = false;

    // steerprobe (render thread only, but started from commands: atomics).
    std::atomic<int64_t> g_probe_until_us{ 0 };
    std::atomic<bool>    g_probe_new{ false };
    int64_t              g_probe_t0 = 0, g_last_us = 0;
    float                g_last_heading = 0.0f;
    bool                 g_last_ok = false;
    int                  g_probe_lines = 0;

    int step_seg(clock::time_point now, float& x)
    {
        // The segment running now (-1: none or over); caller holds g_step_mtx.
        if (!g_step_on) return -1;
        int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_step_t0).count();
        for (size_t i = 0; i < g_steps.size(); ++i) {
            if (t < g_steps[i].ms) { x = g_steps[i].x; return int(i); }
            t -= g_steps[i].ms;
        }
        g_step_on = false;
        return -1;
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    persist = false;   // all tests
    if (cmd == "steerscan") { start(); g_scan_req = true; spdlog::info("[Steer] steerscan: a scan of the game's memory for the steering blocks (a few seconds)"); return true; }
    if (cmd == "steerlist") { start(); g_list_req = arg.empty() ? 16 : std::max(1, int(fv)); return true; }
    if (cmd == "steerset") {
        start();
        std::string v;
        rest >> v;
        std::lock_guard _{ g_mtx };
        if (arg == "off" || arg.empty()) {
            for (auto& o : g_ovr) o = Ovr{};
        } else {
            const auto it = std::find_if(kFields.begin(), kFields.end(), [&](const Field& f) { return arg == f.name; });
            if (it == kFields.end()) {
                spdlog::info("[Steer] steerset: no field {} (timed lodir hidir lomax himax rate losteer histeer thr maxsteer fullspeed)", arg);
                return true;
            }
            Ovr& o = g_ovr[size_t(it - kFields.begin())];
            if (v == "off" || v.empty()) o = Ovr{};
            else if (v[0] == 'x') o = Ovr{ 2, std::strtof(v.c_str() + 1, nullptr) };
            else o = Ovr{ 1, std::strtof(v.c_str(), nullptr) };
            spdlog::info("[Steer] steerset {} ({}) {}", it->name, it->what, o.mode == 0 ? "the game's own" : fmt::format("{}{:g}", o.mode == 2 ? "x" : "= ", o.v));
        }
        if (g_blocks.empty() && !g_scanning.load()) g_scan_req = true;   // nothing held yet: find them first (the overrides go in as found)
        g_apply_req = true;
        return true;
    }
    if (cmd == "steerfind") {
        start();
        std::string w;
        rest >> w;
        if (arg == "list") g_cand_list_req = true;
        else if (arg == "off") g_cand_clear_req = true;
        else if (g_finding.load()) spdlog::info("[Steer] steerfind: a find or sweep is running");
        else { g_find_req = (w == "wide") ? 2 : 1; g_probe_until_us = 0; }
        return true;
    }
    if (cmd == "steersweep") {
        start();
        if (g_finding.load()) spdlog::info("[Steer] steersweep: a find or sweep is running");
        else g_sweep_req = true;
        return true;
    }
    if (cmd == "steerpeek" || cmd == "steerpoke") {
        const int k = std::clamp(int(fv), 0, kWatch - 1);
        std::string os, vs;
        rest >> os >> vs;
        if (k >= g_watch_n.load() || os.empty()) { spdlog::info("[Steer] {}: no watched angle {} (or no offset)", cmd, k); return true; }
        const long long off = std::strtoll(os.c_str(), nullptr, 0);
        const uintptr_t a   = g_watch[k].load() + uintptr_t(off);
        if (cmd == "steerpeek") {
            const int n = vs.empty() ? 16 : std::clamp(std::atoi(vs.c_str()), 1, 64);
            std::string s;
            for (int i = 0; i < n; ++i) {
                float f = 0.0f;
                s += read(a + 4 * i, &f, 4) ? fmt::format(" {:g}", f) : std::string{ " ?" };
            }
            spdlog::info("[Steer] steerpeek {} {:+#x} ({:#x}):{}", k, off, a, s);
        } else {
            float old = 0.0f, v = std::strtof(vs.c_str(), nullptr);
            const bool okr = read(a, &old, 4), okw = !vs.empty() && write(a, &v, 4);
            spdlog::info("[Steer] steerpoke {} {:+#x} ({:#x}): {} -> {:g}{}", k, off, a, okr ? fmt::format("{:g}", old) : std::string{ "?" }, v, okw ? "" : " (not written)");
        }
        return true;
    }
    if (cmd == "steerprobe") {
        if (arg == "off") { g_probe_until_us = 0; return true; }
        g_probe_until_us = us_now() + int64_t((arg.empty() ? 15.0f : std::clamp(fv, 1.0f, 120.0f)) * 1e6f);
        g_probe_new      = true;
        return true;
    }
    if (cmd == "steerstep") {
        std::vector<Seg> segs;
        float x = 0.0f;
        int   ms = 0;
        while (segs.size() < 8 && (rest >> x >> ms)) segs.push_back({ std::clamp(x, -1.0f, 1.0f), std::clamp(ms, 1, 20000) });
        int total = 0;
        for (const auto& s : segs) total += s.ms;
        {
            std::lock_guard _{ g_step_mtx };
            g_steps   = segs;
            g_step_rt = std::clamp(int(fv), 0, 255);
            g_step_t0 = clock::now();
            g_step_on = !segs.empty();
        }
        g_probe_until_us = us_now() + int64_t(total + 2000) * 1000;
        g_probe_new      = true;
        std::string s;
        for (const auto& g : segs) s += fmt::format(" {:+.2f} for {} ms,", g.x, g.ms);
        spdlog::info("[Steer] steerstep: RT {} and the left stick X{} then the controllers again", g_step_rt, s.empty() ? std::string{ " (none)" } : s);
        return true;
    }
    return false;
}

void auto_turn(int ms)
{
    ms = std::clamp(ms, 50, 1000);
    {
        std::lock_guard _{ g_step_mtx };
        g_steps   = { { -1.0f, ms }, { 1.0f, ms }, { 0.0f, 100 } };
        g_step_rt = 0;
        g_step_t0 = clock::now();
        g_step_on = true;
    }
    spdlog::info("[Steer] driveautoturn: the wheel turned full left {} ms, full right {} ms, then the controllers again (to find the car's wheel)", ms, ms);
}

void note_poll(float lx)
{
    const int64_t t = us_now(), was = g_poll_us.exchange(t);
    g_poll_x = lx;
    if (was > 0 && t - was < 1'000'000) {
        g_polls++;
        g_poll_gap_sum += t - was;
        int64_t m = g_poll_gap_max.load();
        while (t - was > m && !g_poll_gap_max.compare_exchange_weak(m, t - was)) {}
    }
}

bool step_value(float& x, int& rt)
{
    if (g_hold_on.load()) { x = g_hold_x.load(); rt = 0; return true; }   // steerfind / steersweep: the car standing
    std::lock_guard _{ g_step_mtx };
    if (step_seg(clock::now(), x) < 0) return false;
    rt = g_step_rt;
    return true;
}

void frame(float heading, float speed, bool driving)
{
    const int64_t t = us_now();
    if (g_probe_new.exchange(false)) {
        g_probe_t0    = t;
        g_probe_lines = 0;
        g_polls       = 0;
        g_poll_gap_sum = 0;
        g_poll_gap_max = 0;
        g_last_ok     = false;
        spdlog::info("[Steer] probe: one line a frame while driving: ms since the probe started, the stick X the game's last poll read (and how long ago), "
                     "the step segment, the car's heading, yaw rate (deg/s, from the heading's change this frame) and speed");
    }
    const int64_t until = g_probe_until_us.load();
    if (until == 0) return;
    if (t > until) {
        g_probe_until_us = 0;
        const int n = g_polls.load();
        spdlog::info("[Steer] probe over: {} lines; the game polled the pad {} times, every {:.1f} ms on average, at most {:.1f} ms apart", g_probe_lines, n,
                     n ? double(g_poll_gap_sum.load()) / n / 1000.0 : 0.0, double(g_poll_gap_max.load()) / 1000.0);
        return;
    }
    if (!driving) { g_last_ok = false; return; }
    const float dt   = g_last_ok ? float(t - g_last_us) * 1e-6f : 0.0f;
    const float rate = dt > 1e-4f ? std::remainder(heading - g_last_heading, 6.2831853f) / dt : 0.0f;
    g_last_heading   = heading;
    g_last_us        = t;
    g_last_ok        = true;
    float sx  = 0.0f;
    int   seg = -1;
    {
        std::lock_guard _{ g_step_mtx };
        seg = step_seg(clock::now(), sx);
    }
    const int64_t pu = g_poll_us.load();
    std::string ang;
    for (int k = 0, n = g_watch_n.load(); k < n; ++k) {
        float v = 0.0f;
        ang += read_f(g_watch[k].load(), v) ? fmt::format("{}{:+.2f}", k ? "/" : " angle ", to_deg(v, g_watch_ref[k].load())) : std::string{ k ? "/?" : " angle ?" };
    }
    if (!ang.empty()) ang += " deg";
    spdlog::info("[Steer] {:6.0f} ms stick {:+.2f} ({:.0f} ms ago){} heading {:7.2f} deg yaw {:+7.1f} deg/s speed {:5.2f} m/s{}", double(t - g_probe_t0) / 1000.0,
                 g_poll_x.load(), pu ? double(t - pu) / 1000.0 : -1.0, seg >= 0 ? fmt::format(" step {} {:+.2f}", seg, sx) : std::string{}, heading * 57.29578f,
                 rate * 57.29578f, speed, ang);
    g_probe_lines++;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    ImGui::Text("Steering blocks found in the game's memory: %d%s", g_count.load(), g_scanning.load() ? " (scanning)" : "");
    if (ImGui::Button("Find them##steerscan")) cmds += "steerscan\n";
    ImGui::SameLine();
    if (ImGui::Button("Log them##steerlist")) cmds += "steerlist 16\n";
    ImGui::SameLine();
    if (ImGui::Button("Game's own values##steersetoff")) cmds += "steerset off\n";
    hint("Test. The game's own steering of cars, quads and bikes: it turns the wheels towards the stick at a capped rate. Find them reads several GB "
         "(a hitch of a few seconds). Commands steerset FIELD V|xK|off, steerprobe S, steerstep RT X MS ...");
    float r = 1.0f;
    {
        std::lock_guard _{ g_mtx };
        if (g_ovr[7].mode == 2) r = g_ovr[7].v;
    }
    if (ImGui::SliderFloat("Steering rate (x the game's)##steerrate", &r, 0.25f, 8.0f, "%.2f")) cmds += fmt::format("steerset rate x{:.2f}\n", r);
    if (ImGui::Button("Probe 15 s##steerprobe")) cmds += "steerprobe 15\n";
    ImGui::Text("Live steering angles watched: %d%s", g_watch_n.load(), g_finding.load() ? " (finding or sweeping: the stick is held)" : "");
    if (ImGui::Button("Find the live angle##steerfind")) cmds += "steerfind auto\n";
    ImGui::SameLine();
    if (ImGui::Button("Sweep##steersweep")) cmds += "steersweep\n";
    ImGui::SameLine();
    if (ImGui::Button("Log##steerfindlist")) cmds += "steerfind list\n";
    hint("Test, with the car standing still: the stick is held right, left, centred and right by itself for about 15 s while the game's memory "
         "is read (hitches), to find the angle the car's wheels are at. Sweep then steps the stick and logs how far and how fast the wheels turn.");
}
}
