// drivekeephold (author 14 Nov 07:41;
// research\2026-11-14_carshoot\README.md, runs 2 and 3).
//
// Driving, the game puts the sidearm away by itself: in the simulator a sidearm drawn with Y (or the game's own DrawWeapon) went away
// 3.17 s after the draw, and 3-4 s after the last shot, whatever the hand, the right stick or LT did. drivereadykeep drew it again
// each time (about 0.45 s without it every 3.6 s). The timer is the game's data, not an input:
//  - FC_m64.dll registers a reflected class (registration at rva 0xaf3e0ac..; mov dword [rbx+30h], offset after each name) with
//    +0x0 fDriveAndShootFireRateModifier, +0x4 fDriveAndShootReticleAlpha, +0x8 fDriveAndShootAutoHolsterDuration ("Time before auto
//    holster when not firing in drive and shoot mode. (in seconds)"), +0xc fDriveAndShootAutoHolsterDurationClassic ("Classic control
//    scheme: ..."; the layout the mod uses: LB shoots), +0x10 curveDriveAndShootAutoHolsterVehicleSpeedMultiplier.
//  - common.dat (entry FCFA35BB9B788D77, as Claude_dev\fc5_weapondata\common) holds them as fire rate 1.0, alpha 1.0, both durations 1.3
//    (the 3.17 s seen is 1.3 s times the speed curve, plus the put-away animation).
// So the block is found in the game's memory by its four floats, in a row and 4-byte aligned: 1.0, 1.0, 1.3, 1.3. While drivereadykeep
// keeps the sidearm out, both durations are written as kHold (600 s); when the keep ends (the right hand on the wheel, getting out,
// drivereadykeep or drivekeephold off) the data's own values are written back. drivereadykeep's re-draw stays as the fallback.
//
// The scan reads the committed, writable private memory once (as switchfast's, FarCry5SwitchSpeed.cpp: ReadProcessMemory, write-combined
// and uncached pages never read) on a low-priority thread, the first time the player drives with driveready (prepare), so the block is
// known before the first draw. Only places whose +0x10 points into the game's code image (the speed curve object's vtable) count: the v3 run found 10 with
// the four floats and that one alone held the sidearm when written by itself (v4). Before that filter the v2 simulator run found 10 places (the class is instanced more than once); all of them are written
// (up to kMaxBlocks: more, none, since the pattern would no longer be telling), each one's 32 bytes either side logged once after the scan,
// and drivekeepholdonly N limits the writes to one of them (a test, to learn which the game reads). Each place is read again (every 20 ms, at most 4 places) before every write: one that no longer holds 1.0, 1.0 and either the data's or the written times
// is dropped; with none left while a hold is wanted, a new scan runs (at most every 30 s).
//
//   drivekeephold on|off      (saved, default on; F11 Layout > Vehicles under drivereadykeep)
//   drivekeepholdscan         (test) scan now
//   drivekeepholdlist         (test) log the places held and their floats
//   drivekeepholdset S|off    (test) write S seconds now whatever the keep (off: the keep decides again)
//   drivekeepholdonly N|all   (test) write to place N only (drivekeepholdlist numbers them), the others put back
#include "FarCry5DsHold.h"

#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <istream>
#include <string>
#include <thread>
#include <vector>

namespace fc5::dshold
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr uint32_t kOne      = 0x3F800000u;   // 1.0f
    constexpr uint32_t kOwn      = 0x3FA66666u;   // 1.3f, the data's two durations
    constexpr size_t   kSpan     = 24;            // the four floats and the curve's 8 bytes (logged only)
    constexpr size_t   kMaxBlocks = 16;   // the v2 run (14 Nov 10:44) found 10, none written with 4 as the cap
    constexpr float    kHold     = 600.0f;

    struct Block
    {
        uintptr_t a = 0;
        uint32_t  own8 = kOwn, ownc = kOwn;   // as found
    };

    std::atomic<bool>  g_on{ true };          // drivekeephold
    std::atomic<bool>  g_want{ false };       // hold(): drivereadykeep keeps the sidearm out
    std::atomic<float> g_test{ 0.0f };        // drivekeepholdset (0: off)
    std::atomic<int>   g_only{ -1 };          // drivekeepholdonly (-1: all)
    std::atomic<bool>  g_prep{ false };       // prepare() asked for the first scan
    std::atomic<bool>  g_scan_req{ false }, g_list_req{ false };
    std::atomic<bool>  g_started{ false }, g_holding{ false };
    std::vector<Block> g_blocks;              // worker thread only
    bool               g_too_many = false;    // worker thread only

    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count(); }

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
    bool write_u(uintptr_t a, uint32_t v)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), &v, 4, &put) && put == 4;
    }
    uint32_t bits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    float flt(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }

    void scan(const char* why)
    {
        const int64_t        t0 = now_ms();
        std::vector<Block>   found;
        // The real block's +0x10 (the speed curve) starts with a pointer into the game's code image (the curve object's vtable,
        // 0x7fff4f588fc8 in the v3 run, 14 Nov 10:59): that run found 10 places with the four floats, and only that one held the sidearm out
        // when written alone (the other nine: +0x10 0x100000001, 1.3 / 1.3 or other floats). So a place counts only if +0x10 is an address
        // in a loaded module (VirtualQuery: MEM_IMAGE).
        auto in_image = [](uint64_t v) {
            if (v < 0x10000 || v >= 0x7FFFFFFF0000ull || (v & 7) != 0) return false;
            MEMORY_BASIC_INFORMATION q{};
            return VirtualQuery(reinterpret_cast<LPCVOID>(v), &q, sizeof(q)) != 0 && q.State == MEM_COMMIT && q.Type == MEM_IMAGE;
        };
        int others = 0;
        std::vector<uint8_t> buf((4u << 20) + kSpan);
        size_t               bytes = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            a = end;
            if (!readable(m) || m.RegionSize < kSpan) continue;
            for (uintptr_t c = base; c + kSpan <= end;) {
                const size_t n = std::min<size_t>(buf.size(), end - c);
                if (!read(c, buf.data(), n)) { c += n; continue; }
                bytes += n;
                const uint8_t* p = buf.data();
                for (size_t o = 0; o + kSpan <= n; o += 4) {
                    uint32_t u[4];
                    std::memcpy(u, p + o, 16);
                    if (u[0] != kOne || u[1] != kOne || u[2] != kOwn || u[3] != kOwn) continue;
                    uint64_t curve;
                    std::memcpy(&curve, p + o + 16, 8);
                    if (!in_image(curve)) { ++others; continue; }   // not the class (no curve object after the floats)
                    found.push_back(Block{ c + o, kOwn, kOwn });
                }
                if (c + n >= end) break;
                c += n - kSpan;
            }
        }
        std::sort(found.begin(), found.end(), [](const Block& x, const Block& y) { return x.a < y.a; });
        found.erase(std::unique(found.begin(), found.end(), [](const Block& x, const Block& y) { return x.a == y.a; }), found.end());
        g_too_many = found.size() > kMaxBlocks;
        std::string where;
        for (size_t i = 0; i < found.size() && i < 8; ++i) where += fmt::format(" {:#x}", found[i].a);
        spdlog::info("[DsHold] scan ({}): {} place(s) with the game's drive-and-shoot times (fire rate 1.0, reticle 1.0, put-away 1.3 / 1.3 s and the curve "
                     "object after them; {} other place(s) with the four floats only) in {:.1f} GB, {:.1f} s:{}{}", why, found.size(), others, double(bytes) / (1u << 30), double(now_ms() - t0) / 1000.0, where.empty() ? " none" : where,
                     g_too_many ? fmt::format("; more than {}: none written (the pattern is not telling)", kMaxBlocks) : std::string{});
        g_blocks = g_too_many ? std::vector<Block>{} : std::move(found);
        for (size_t i = 0; i < g_blocks.size(); ++i) {   // each place's surroundings, once: what tells the real one apart (the class's other fields, the curve)
            uint8_t ctx[64 + kSpan + 32]{};
            if (!read(g_blocks[i].a - 64, ctx, sizeof(ctx))) continue;
            std::string hx;
            for (size_t k = 0; k < sizeof(ctx); ++k) hx += fmt::format("{:02x}{}", ctx[k], k == 63 || k == 63 + kSpan ? "|" : (k % 8 == 7 ? " " : ""));
            spdlog::info("[DsHold]   place {} {:#x}: -64..+56: {}", i, g_blocks[i].a, hx);
        }
    }

    // Writes v (bits) at +0x8 and +0xc of every place still holding the block; drops the rest. Returns the places written.
    int apply(bool hold, uint32_t v)
    {
        int       n = 0, i = -1;
        const int only = g_only.load();
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            ++i;
            uint32_t   u[4];
            const bool mine = hold && (only < 0 || only == i);
            const uint32_t w8 = mine ? v : it->own8, wc = mine ? v : it->ownc;
            const bool ok = read(it->a, u, 16) && u[0] == kOne && std::isfinite(flt(u[1])) && flt(u[1]) >= 0.0f && flt(u[1]) <= 1.0f &&
                            std::isfinite(flt(u[2])) && std::isfinite(flt(u[3])) && flt(u[2]) > 0.0f && flt(u[3]) > 0.0f && flt(u[2]) <= 100000.0f &&
                            flt(u[3]) <= 100000.0f;
            if (!ok) {
                spdlog::info("[DsHold] {:#x} no longer holds the drive-and-shoot times: dropped", it->a);
                it = g_blocks.erase(it);
                continue;
            }
            if (u[2] != w8) write_u(it->a + 0x8, w8);
            if (u[3] != wc) write_u(it->a + 0xc, wc);
            if (mine) ++n;
            ++it;
        }
        return n;
    }

    void list()
    {
        spdlog::info("[DsHold] {} place(s) held{}; drivekeephold {}, a hold {}, written now {}, writes to {}", g_blocks.size(), g_too_many ? " (the last scan found too many)" : "",
                     g_on.load() ? "on" : "off", g_want.load() ? "wanted" : "not wanted", g_holding.load() ? "yes" : "no",
                     g_only.load() < 0 ? std::string("all") : fmt::format("place {} only", g_only.load()));
        int i = 0;
        for (const auto& b : g_blocks) {
            const int k = i++;
            uint8_t buf[kSpan]{};
            if (!read(b.a, buf, kSpan)) { spdlog::info("[DsHold]   place {} {:#x}: unreadable", k, b.a); continue; }
            float f[4];
            uint64_t curve;
            std::memcpy(f, buf, 16);
            std::memcpy(&curve, buf + 16, 8);
            spdlog::info("[DsHold]   place {} {:#x}: fire rate {:.3f}, reticle alpha {:.3f}, put-away {:.2f} s, classic {:.2f} s (own {:.2f} / {:.2f}), curve {:#x}", k, b.a, f[0],
                         f[1], f[2], f[3], flt(b.own8), flt(b.ownc), curve);
        }
    }

    void worker()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        bool    scanned = false, was = false;
        int64_t last_scan = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a keep starts with the Y tap: written before the draw lands (~115 ms)
            const int64_t t    = now_ms();
            const float   test = g_test.load();
            const bool    hold = test > 0.0f || (g_on.load() && g_want.load());
            const char*   why  = g_scan_req.exchange(false) ? "asked"
                               : (!scanned && (g_prep.load() || hold)) ? "the first drive"
                               : (hold && g_blocks.empty() && !g_too_many && t - last_scan > 30'000) ? "none held while a hold is wanted"
                                                                                                       : nullptr;
            if (why) {
                scan(why);
                scanned   = true;
                last_scan = now_ms();
            }
            const uint32_t v = bits(test > 0.0f ? test : kHold);
            const int      n = g_blocks.empty() ? 0 : apply(hold, v);
            const bool     h = hold && n > 0;
            if (h != was)
                spdlog::info("[DsHold] {}", h ? fmt::format("the game's drive-and-shoot put-away times held at {:.0f} s in {} place(s): the sidearm stays out", flt(v), n)
                                              : "the game's own put-away times back (1.3 s, the speed curve, then the put-away)");
            was = h;
            g_holding = h;
            if (g_list_req.exchange(false)) list();
        }
    }

    void start()
    {
        if (!g_started.exchange(true)) std::thread(worker).detach();
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    (void)rest;
    if (cmd == "drivekeephold") {
        g_on = !(arg == "off" || arg == "0" || arg == "false");
        spdlog::info("[DsHold] drivekeephold {}", g_on.load() ? "on: while drivereadykeep keeps the sidearm out, the game's own put-away timer is held off (no re-draw)"
                                                               : "off: the game puts the sidearm away after its own time and drivereadykeep draws it again");
        if (g_on.load()) start();
        return true;
    }
    if (cmd == "drivekeepholdscan") { persist = false; g_scan_req = true; start(); return true; }
    if (cmd == "drivekeepholdlist") { persist = false; g_list_req = true; start(); return true; }
    if (cmd == "drivekeepholdonly") {   // test: drivekeepholdonly N|all: writes only to place N (as drivekeepholdlist numbers them); the others get their own times back
        persist = false;
        g_only  = (arg.empty() || arg == "all") ? -1 : std::max(0, int(fv));
        spdlog::info("[DsHold] drivekeepholdonly {}", g_only.load() < 0 ? std::string("all: every place found is written") : fmt::format("{}: only place {} is written (test)", g_only.load(), g_only.load()));
        start();
        return true;
    }
    if (cmd == "drivekeepholdset") {
        persist = false;
        g_test  = (arg.empty() || arg == "off") ? 0.0f : std::clamp(fv, 0.1f, 100000.0f);
        spdlog::info("[DsHold] drivekeepholdset {}", g_test.load() > 0.0f ? fmt::format("{:.1f} s written now whatever the keep (test)", g_test.load()) : std::string("off: the keep decides"));
        start();
        return true;
    }
    return false;
}

bool on() { return g_on.load(); }

void prepare()
{
    if (!g_on.load() || g_prep.load()) return;
    g_prep = true;
    start();
}

void hold(bool want)
{
    g_want = want;
    if (want && g_on.load()) start();
}

bool holding() { return g_holding.load(); }
}
