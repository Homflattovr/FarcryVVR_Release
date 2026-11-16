// gunstillexact: see FarCry5GunExact.h.
//
// A copy is a 4x4 transform by columns: three axes (x y z 0 each), then, somewhere near, the gun's place. The axes' offset is known
// from gunscan (the first column is what is searched for); the place's offset is found at the gun mesh's draw, when the copy and the
// mesh's own transform both hold this update: the three floats equal to the mesh's place, within 16 floats before and 32 after.
#include "FarCry5GunExact.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace fc5::gunexact
{
namespace
{
    using clock = std::chrono::steady_clock;

    // The gun mesh's last first columns (bit patterns), published by the render thread, read by the search.
    constexpr int         kRing = 32;
    std::atomic<uint32_t> g_ring[kRing][3]{};
    std::atomic<uint32_t> g_ring_n{ 0 };

    std::atomic<bool> g_scan_req{ false }, g_scanning{ false }, g_worker{ false };
    // Where copies were proven before (this session): the search reads 256 MB either side of them first, and the whole memory
    // only when that finds nothing (12 Nov: the proven copies of four weapons lay within 7 MB of each other).
    constexpr int          kHints = 8;
    std::atomic<uintptr_t> g_hint[kHints]{};
    std::atomic<uint32_t>  g_hint_n{ 0 };
    std::atomic<bool>      g_full_next{ false };   // the next search reads the whole memory (the near one found nothing proven)
    std::atomic<int>       g_last_kind{ 0 };       // 1 near the known copies, 2 the whole memory
    std::atomic<int>  g_scans{ 0 };
    std::mutex        g_found_mtx;
    std::vector<uintptr_t> g_found;   // places the last search kept (under g_found_mtx)
    std::atomic<float> g_scan_secs{ 0.0f };
    std::atomic<int>   g_scan_mb{ 0 };

    void scan_once()
    {
        const auto t0 = clock::now();
        std::vector<uintptr_t> found;
        std::vector<uint8_t>   buf(4u << 20);
        uint64_t               bytes = 0;
        uint32_t               ring[kRing][3]{};
        uint8_t                bloom[512]{};
        auto refresh = [&] {
            std::memset(bloom, 0, sizeof(bloom));
            for (int i = 0; i < kRing; ++i) {
                for (int c = 0; c < 3; ++c) ring[i][c] = g_ring[i][c].load(std::memory_order_relaxed);
                const uint32_t h = (ring[i][0] * 2654435761u) >> 20;   // 12 bits
                bloom[h >> 3] |= uint8_t(1u << (h & 7));
            }
        };
        MEMORY_BASIC_INFORMATION m{};
        const HANDLE             self = GetCurrentProcess();
        // Near the known copies first; the whole memory when there is none, or when asked (the near search proved nothing).
        uintptr_t hint[kHints]{};
        int       hints = 0;
        for (int i = 0; i < kHints; ++i)
            if (const uintptr_t h = g_hint[i].load()) hint[hints++] = h;
        const bool      near_only = hints > 0 && !g_full_next.exchange(false);
        const uintptr_t kNear     = 256ull << 20;
        auto is_near = [&](uintptr_t lo, uintptr_t hi) {
            for (int i = 0; i < hints; ++i)
                if (hi + kNear > hint[i] && lo < hint[i] + kNear) return true;
            return false;
        };
        g_last_kind    = near_only ? 1 : 2;
        uint64_t since = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && found.size() < 256;) {
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && m.Protect == PAGE_READWRITE && (!near_only || is_near(base, end))) {
                for (uintptr_t p = base; p < end && found.size() < 256; p += buf.size()) {
                    if (near_only && !is_near(p, p + buf.size())) continue;
                    // The whole-memory search gives way: 1 ms of rest for every 32 MB read (12 GB then take about 2.5 s instead of 2).
                    if (since >= (32ull << 20)) {
                        since = 0;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const size_t n   = size_t(std::min<uintptr_t>(buf.size(), end - p));
                    SIZE_T       got = 0;
                    if (!ReadProcessMemory(self, reinterpret_cast<LPCVOID>(p), buf.data(), n, &got) || got < 64) continue;
                    refresh();
                    bytes += got;
                    since += got;
                    for (size_t o = (16 - (p & 15)) & 15; o + 16 <= got; o += 16) {
                        uint32_t v[4];
                        std::memcpy(v, buf.data() + o, 16);
                        if (v[3] != 0 || v[0] == 0) continue;   // x y z 0, and not an empty place
                        const uint32_t h = (v[0] * 2654435761u) >> 20;
                        if (!(bloom[h >> 3] & (1u << (h & 7)))) continue;
                        for (int i = 0; i < kRing; ++i)
                            if (ring[i][0] == v[0] && ring[i][1] == v[1] && ring[i][2] == v[2]) { found.push_back(p + o); break; }
                    }
                }
            }
            a = end;
        }
        const float secs = std::chrono::duration<float>(clock::now() - t0).count();
        g_scan_secs      = secs;
        g_scan_mb        = int(bytes >> 20);
        g_scans++;
        spdlog::info("[GunExact] search {} ({}): {} places hold one of the gun mesh's last transforms ({} MB read in {:.2f} s); each is now put to the proof at every pass", g_scans.load(),
                     near_only ? "near the copies known" : "the whole memory", found.size(), bytes >> 20, secs);
        std::lock_guard _{ g_found_mtx };
        g_found = std::move(found);
    }

    void worker()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!g_scan_req.exchange(false)) continue;
            g_scanning = true;
            scan_once();
            g_scanning = false;
        }
    }

    // A read of the game's memory that may be gone: no C++ objects in here.
    bool read_floats(const float* p, float* out, int n)
    {
        __try {
            for (int i = 0; i < n; ++i) out[i] = p[i];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool read_doubles(const double* p, double* out, int n)
    {
        __try {
            for (int i = 0; i < n; ++i) out[i] = p[i];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Render thread only.
    struct Cand
    {
        // camfine: the gun's position fraction beside the copy. 0 not found, 1 three floats frac_off floats from a, 2 the place
        // kept as three doubles frac_off doubles from a (the fraction is what the float leaves of each).
        int       frac_kind{ 0 }, frac_off{ 0 };
        int       frac_okn{ 0 }, frac_bad{ 0 }, frac_tries{ 0 };
        bool      snap_frac_ok{ false };
        float     snap_frac[3]{};
        uintptr_t a{ 0 };
        int       pos_off{ INT_MIN };   // the place: floats from a (INT_MIN: not found yet)
        int       ok{ 0 }, bad{ 0 };    // passes in a row (gun moved) with the mesh's transform before its draw; misses in a row
        int       snap_frame{ -100 };
        bool      snap_ok{ false };
        float     snap[16]{};           // axes (12 floats) and place (3) as read at the pass's first draw
    };
    std::vector<Cand> s_cands;
    uintptr_t         s_adopted{ 0 };
    uint32_t          s_sig{ 0 };
    float             s_prev_obj[12]{};
    bool              s_have_prev{ false };
    clock::time_point s_sig_since{}, s_last_req{};
    std::unordered_map<uint32_t, std::pair<uintptr_t, int>> s_known;   // each weapon's last proven copy and its place offset
    int               s_diag{ 0 };       // log lines left for "the axes are right but the place was not found near them" (a test aid: none by default)
    bool              s_near_tried{ false };   // this weapon's first search (near the known copies) has been asked for
    int               s_frac_diag{ 2 };   // log lines left for "the fraction was not found near the copy"
    int               s_zero_run{ 0 };    // gun mesh draws in a row whose fraction was zero
    struct Stat
    {
        uint64_t passes{ 0 }, moved{ 0 }, agree{ 0 }, over{ 0 }, placed{ 0 }, fb[3]{}, adopted{ 0 }, dropped{ 0 };
        uint64_t frac_read{ 0 }, frac_same{ 0 }, frac_given{ 0 }, frac_zero{ 0 };
        double   mm{ 0.0 }, deg{ 0.0 };
        float    max_mm{ 0.0f }, max_deg{ 0.0f };
    } s_st;

    void add_cand(uintptr_t a, int pos_off = INT_MIN)
    {
        for (const auto& c : s_cands)
            if (c.a == a) return;
        if (s_cands.size() >= 96) return;
        Cand c{};
        c.a       = a;
        c.pos_off = pos_off;
        s_cands.push_back(c);
    }

    // The copy's axes and place as the gun mesh's object rows (f[r * 4 + c]).
    void to_rows(const float* s, float* obj)
    {
        for (int r = 0; r < 3; ++r) {
            obj[r * 4 + 0] = s[0 + r];
            obj[r * 4 + 1] = s[4 + r];
            obj[r * 4 + 2] = s[8 + r];
            obj[r * 4 + 3] = s[12 + r];
        }
    }
    // camfine: where, beside a copy that holds this update's transform, the mesh's fraction is.
    void find_frac(Cand& c, const float* obj, const float* frac)
    {
        float w[96]{};
        if (!read_floats(reinterpret_cast<const float*>(c.a) - 32, w, 96)) return;
        for (int o = 0; o + 2 < 96; ++o) {
            if (w[o] == frac[0] && w[o + 1] == frac[1] && w[o + 2] == frac[2]) {
                c.frac_kind = 1;
                c.frac_off  = o - 32;
                return;
            }
        }
        double dd[48]{};
        std::memcpy(dd, w, sizeof(dd));
        const float pos[3]{ obj[3], obj[7], obj[11] };
        for (int o = 0; o + 2 < 48; ++o) {
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) {
                const double v = dd[o + i];
                const float  f = float(v);
                ok = std::isfinite(v) && f == pos[i] && float(v - double(f)) == frac[i];
            }
            if (ok) {
                c.frac_kind = 2;
                c.frac_off  = o - 16;
                return;
            }
        }
        if (c.frac_tries == 20 && s_frac_diag > 0) {
            s_frac_diag--;
            std::string line;
            for (int o = 0; o < 96; ++o) line += fmt::format("{}{:.9g}", o % 4 == 0 ? " | " : " ", w[o]);
            spdlog::info("[GunExact] the gun's position fraction ({:.9g} {:.9g} {:.9g}; its place {:.4f} {:.4f} {:.4f}) is not beside the copy at {:#x}; floats -32..+63:{}", frac[0], frac[1],
                         frac[2], obj[3], obj[7], obj[11], c.a, line);
        }
    }
    bool rigid_near(const float* obj, const float* near_obj)
    {
        float len[3]{}, dot01 = 0.0f, dot02 = 0.0f, dot12 = 0.0f;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(obj[r * 4 + c])) return false;
            len[0] += obj[r * 4 + 0] * obj[r * 4 + 0];
            len[1] += obj[r * 4 + 1] * obj[r * 4 + 1];
            len[2] += obj[r * 4 + 2] * obj[r * 4 + 2];
            dot01 += obj[r * 4 + 0] * obj[r * 4 + 1];
            dot02 += obj[r * 4 + 0] * obj[r * 4 + 2];
            dot12 += obj[r * 4 + 1] * obj[r * 4 + 2];
        }
        const float l0 = std::sqrt(len[0]);
        if (!(l0 > 0.5f && l0 < 2.0f) || std::abs(std::sqrt(len[1]) - l0) > 0.01f || std::abs(std::sqrt(len[2]) - l0) > 0.01f) return false;
        if (std::abs(dot01) > 0.01f || std::abs(dot02) > 0.01f || std::abs(dot12) > 0.01f) return false;
        const float dx = obj[3] - near_obj[3], dy = obj[7] - near_obj[7], dz = obj[11] - near_obj[11];
        return dx * dx + dy * dy + dz * dz < 2.0f * 2.0f;   // within 2 m of where the gun mesh was last drawn
    }
}

void request_scan()
{
    if (!g_worker.exchange(true)) std::thread(worker).detach();
    g_scan_req = true;
}

bool first_draw(int frame, uint32_t sig, bool want, float obj[12], float frac[3], bool& frac_ok)
{
    frac_ok = false;
    const auto now = clock::now();
    {
        std::lock_guard _{ g_found_mtx };
        for (uintptr_t a : g_found) add_cand(a);
        g_found.clear();
    }
    if (sig != s_sig) {
        // Another weapon: nothing is taken for granted. Its last copy, if it had one, is put to the proof first.
        s_sig       = sig;
        s_adopted   = 0;
        s_have_prev = false;
        s_sig_since = now;
        for (auto& c : s_cands) c.ok = 0;
        // Every copy proven this session is put to the proof for the new weapon (12 Nov: one place served three long guns in turn, another
        // the handgun), this weapon's own first: most switches then need no search at all.
        if (auto it = s_known.find(sig); it != s_known.end()) add_cand(it->second.first, it->second.second);
        for (const auto& [ksig, kv] : s_known) add_cand(kv.first, kv.second);
        s_near_tried = false;
    }
    for (auto& c : s_cands) {
        c.snap_frame = frame;
        c.snap_ok    = read_floats(reinterpret_cast<const float*>(c.a), c.snap, 12);
        if (c.snap_ok && c.pos_off != INT_MIN) c.snap_ok = read_floats(reinterpret_cast<const float*>(c.a) + c.pos_off, c.snap + 12, 3);
        c.snap_frac_ok = false;
        if (c.snap_ok && c.frac_kind == 1) {
            c.snap_frac_ok = read_floats(reinterpret_cast<const float*>(c.a) + c.frac_off, c.snap_frac, 3);
        } else if (c.snap_ok && c.frac_kind == 2) {
            double dd[3]{};
            if (read_doubles(reinterpret_cast<const double*>(c.a) + c.frac_off, dd, 3)) {
                for (int i = 0; i < 3; ++i) c.snap_frac[i] = float(dd[i] - double(float(dd[i])));
                c.snap_frac_ok = true;
            }
        }
    }
    if (sig == 0) return false;
    // With the switch on and no copy: a search, once the weapon has been in hand for 1.5 s, at most every 15 s.
    if (want && !s_adopted && !g_scanning.load() && now - s_sig_since > std::chrono::milliseconds(1500) &&
        now - s_last_req > (s_near_tried ? std::chrono::seconds(15) : std::chrono::seconds(3))) {
        // The first search for a weapon reads near the known copies (if any); if nothing is proven 3 s later, the whole memory.
        if (s_near_tried) g_full_next = true;
        s_near_tried = true;
        s_last_req   = now;
        request_scan();
    }
    if (!want) return false;
    if (!s_adopted || !s_have_prev) { s_st.fb[0]++; return false; }
    for (const auto& c : s_cands) {
        if (c.a != s_adopted) continue;
        if (!c.snap_ok || c.pos_off == INT_MIN) break;
        to_rows(c.snap, obj);
        if (!rigid_near(obj, s_prev_obj)) break;
        if (c.snap_frac_ok && c.frac_okn >= 10) {
            for (int i = 0; i < 3; ++i) frac[i] = c.snap_frac[i];
            frac_ok = std::abs(frac[0]) < 1.0f && std::abs(frac[1]) < 1.0f && std::abs(frac[2]) < 1.0f;
        } else if (s_zero_run >= 30) {
            frac[0] = frac[1] = frac[2] = 0.0f;
            frac_ok = true;
        }
        if (frac_ok) s_st.frac_given++;
        return true;
    }
    s_st.fb[1]++;
    return false;
}

void gun_mesh(int frame, uint32_t sig, const float obj[12], const float* frac, bool used)
{
    const bool frac_zero = frac && frac[0] == 0.0f && frac[1] == 0.0f && frac[2] == 0.0f;
    s_zero_run           = frac_zero ? s_zero_run + 1 : 0;
    if (frac_zero) s_st.frac_zero++;
    // The first column for the search.
    {
        const uint32_t n = g_ring_n.load(std::memory_order_relaxed);
        const float    col[3]{ obj[0], obj[4], obj[8] };
        uint32_t       b[3];
        std::memcpy(b, col, sizeof(b));
        for (int c = 0; c < 3; ++c) g_ring[n % kRing][c].store(b[c], std::memory_order_relaxed);
        g_ring_n.store(n + 1, std::memory_order_relaxed);
    }
    if (sig != s_sig) {   // (first_draw sees the change at the next pass)
        std::memcpy(s_prev_obj, obj, sizeof(s_prev_obj));
        return;
    }
    const bool moved = s_have_prev && std::memcmp(s_prev_obj, obj, sizeof(s_prev_obj)) != 0;
    s_st.passes++;
    if (moved) s_st.moved++;
    if (used) s_st.placed++;
    for (auto it = s_cands.begin(); it != s_cands.end();) {
        Cand& c = *it;
        if (c.snap_frame != frame || !c.snap_ok) {
            if (c.snap_frame == frame && ++c.bad > 30) {   // unreadable: the memory is gone
                if (c.a == s_adopted) { s_adopted = 0; s_st.dropped++; }
                it = s_cands.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (c.pos_off == INT_MIN) {
            // The place's offset: where the copy, now holding this update for certain, has the mesh's own place.
            float w[48]{};
            if (read_floats(reinterpret_cast<const float*>(c.a) - 16, w, 48)) {
                for (int o = 0; o + 2 < 48; o += 4) {
                    if (o >= 16 && o < 28) continue;   // the axes themselves
                    if (w[o] == obj[3] && w[o + 1] == obj[7] && w[o + 2] == obj[11]) { c.pos_off = o - 16; break; }
                }
                // The axes are the mesh's but its place is not among the 48 floats: logged (a few times) so the layout can be read.
                if (c.pos_off == INT_MIN && s_diag > 0 && w[16] == obj[0] && w[17] == obj[4] && w[18] == obj[8] && w[20] == obj[1] && w[21] == obj[5]) {
                    s_diag--;
                    std::string line;
                    for (int o = 0; o < 48; ++o) line += fmt::format("{}{:.6g}", o % 4 == 0 ? " | " : " ", w[o]);
                    spdlog::info("[GunExact] place not found near the copy at {:#x} (the gun mesh's place {:.4f} {:.4f} {:.4f}); floats -16..+31:{}", c.a, obj[3], obj[7], obj[11], line);
                }
            }
            ++it;
            continue;   // proved from the next pass on
        }
        float rows[12]{};
        to_rows(c.snap, rows);
        float d_axes = 0.0f, d_pos = 0.0f;
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) d_axes = std::max(d_axes, std::abs(rows[r * 4 + k] - obj[r * 4 + k]));
            d_pos += (rows[r * 4 + 3] - obj[r * 4 + 3]) * (rows[r * 4 + 3] - obj[r * 4 + 3]);
        }
        d_pos            = std::sqrt(d_pos);
        const bool same  = d_axes < 2e-6f && d_pos < 5e-5f;   // the mesh's transform, to the float
        // camfine: the adopted copy's fraction, found beside it and then proven as the copy is (read at the first draw, the mesh's own).
        if (c.a == s_adopted && frac && !frac_zero) {
            if (c.frac_kind == 0) {
                if (same && c.frac_tries < 200) {
                    c.frac_tries++;
                    find_frac(c, obj, frac);
                }
            } else if (c.snap_frac_ok) {
                s_st.frac_read++;
                if (c.snap_frac[0] == frac[0] && c.snap_frac[1] == frac[1] && c.snap_frac[2] == frac[2]) {
                    s_st.frac_same++;
                    c.frac_okn++;
                    c.frac_bad = 0;
                } else {
                    c.frac_okn = 0;
                    if (++c.frac_bad >= 3) {   // not the fraction after all: looked for again
                        c.frac_kind  = 0;
                        c.frac_bad   = 0;
                        c.frac_tries = 0;
                    }
                }
            }
        }
        if (c.a == s_adopted && moved) {
            const float mm = 1000.0f * d_pos, deg = 57.29578f * d_axes;
            s_st.agree++;
            s_st.mm += mm;
            s_st.deg += deg;
            s_st.max_mm  = std::max(s_st.max_mm, mm);
            s_st.max_deg = std::max(s_st.max_deg, deg);
            if (mm > 0.1f) s_st.over++;
        }
        if (moved) {
            if (same) {
                c.ok++;
                c.bad = 0;
            } else {
                c.ok = 0;
                c.bad++;
            }
        }
        if (c.a == s_adopted && c.bad >= 3) {
            s_adopted = 0;
            s_st.dropped++;
            spdlog::info("[GunExact] the copy at {:#x} no longer gives the gun mesh's transform before its draw (weapon {}): dropped, the guess again until another is proven", c.a, sig);
        }
        if (c.bad > 30 && c.a != s_adopted) {
            it = s_cands.erase(it);
            continue;
        }
        ++it;
    }
    if (!s_adopted) {
        for (const auto& c : s_cands) {
            if (c.ok < 10 || c.pos_off == INT_MIN) continue;
            s_adopted    = c.a;
            s_known[sig] = { c.a, c.pos_off };
            {
                bool had = false;
                for (int i = 0; i < kHints; ++i) had = had || g_hint[i].load() == c.a;
                if (!had) g_hint[g_hint_n.fetch_add(1) % kHints] = c.a;
            }
            s_st.adopted++;
            spdlog::info("[GunExact] weapon {}: a copy of the gun's transform at {:#x} (its place {} floats on) held the gun mesh's own, before the mesh was drawn, in 10 moved passes in a "
                         "row: taken ({} places under proof)", sig, c.a, c.pos_off, s_cands.size());
            break;
        }
    }
    std::memcpy(s_prev_obj, obj, sizeof(s_prev_obj));
    s_have_prev = true;
}

void fell_back(int why)
{
    if (why >= 0 && why < 3) s_st.fb[why]++;
}

std::string stat_line(bool on)
{
    const auto&  s = s_st;
    const double n = double(std::max<uint64_t>(1, s.agree));
    std::string  out = fmt::format("[GunStill] gunstillexact {}: {} | the copy read at the pass's first draw against the gun mesh's own transform at its draw: {} moved passes, {:.4f} mm / "
                                   "{:.5f} deg mean, {:.3f} mm / {:.4f} deg max, {} over 0.1 mm | passes placed from the copy {}, fallen back {} (no proven copy {}, not a rigid transform "
                                   "near the gun {}, camera not placed {}) | copies taken {}, dropped {}, places under proof {}, searches {} (last {:.1f} s, {} MB)",
                                   on ? "on" : "off", s_adopted ? fmt::format("copy at {:#x}", s_adopted) : std::string{ "no proven copy" }, s.agree, s.mm / n, s.deg / n, s.max_mm, s.max_deg, s.over,
                                   s.placed, s.fb[0] + s.fb[1] + s.fb[2], s.fb[0], s.fb[1], s.fb[2], s.adopted, s.dropped, s_cands.size(), g_scans.load(), g_scan_secs.load(), g_scan_mb.load());
    {
        std::string where = "no copy";
        for (const auto& c : s_cands) {
            if (c.a != s_adopted) continue;
            where = c.frac_kind == 1 ? fmt::format("three floats {} floats from the copy, {}", c.frac_off, c.frac_okn >= 10 ? "proven" : "under proof")
                  : c.frac_kind == 2 ? fmt::format("in the place kept as doubles {} doubles from the copy, {}", c.frac_off, c.frac_okn >= 10 ? "proven" : "under proof")
                                     : fmt::format("not found beside the copy ({} tries)", c.frac_tries);
        }
        out += fmt::format(" | the gun's position fraction: {}; read at the first draw in {} passes, the mesh's own in {}; given with the pose {} times; the mesh's was zero in {} draws", where,
                           s.frac_read, s.frac_same, s.frac_given, s.frac_zero);
    }
    s_st = Stat{};
    return out;
}
}
