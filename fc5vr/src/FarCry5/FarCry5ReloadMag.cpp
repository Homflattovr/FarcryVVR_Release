// reloadmagmodel (FarCry5Reload.h; author 2 Nov 08:56; 2 Nov 11:09 after the
// first try: the hip magazine was not the gun's own, it did not always slide in, and racking did not move the gun's handle): the game's own magazine and charging handle follow the manual reload.
//
// Everything is measured against the gun's own body (bone 0 of its main mesh, the draw whose index count is the weapon id): the gun moves in
// the game's camera space while the game reloads (the first try measured in the steadied camera axes: the gun's body moved 24 cm there, the
// arms 2.7 m, and the arms' bones were taken for the magazine). A part's place on the gun is its transform relative to that body.
//  - The magazine: the AR-C's is known (bone 1 of its main mesh: 1400 vertices, as the game's spare magazine, draw 5106, that its left hand
//    brings in during its reload, hidden). Other weapons: learnt at their first game reload, from the parts of the weapon's own draws (bones up
//    to kMaxWeaponBone, fixed to the body while quiet) that move furthest from the body (at least 8 cm); draws seen only during the reload are
//    the game's prop. reloadmagmesh (saved).
//  - The well: measured on the magazine model while the gun is held quietly, in the weapon controller's axes: the top of its vertices along
//    the well's axis (the gun's up across its barrel, or the magazine's first motion out in a learnt reload). reloadgeom (saved), so the hand's
//    magazine engages and clicks exactly where the model sits.
//  - The charging handle: the AR-C's is built in (the bone map thread: 57819/7 CHARGING_HANDLE, its tabs 8 and 9, and the bolt 6 it pulls;
//    the grab point, the blue ball, measured on the tabs). Other weapons: the part that moves 2.5 to 15 cm back along the gun in the game's
//    reload from empty (reloadhandlepart, saved), else the part of the main mesh nearest the handle point; F11 "Try the next part as the
//    charging handle" steps through the parts nearest it.
// Then, with that weapon in hand:
//  - the magazine stays on the gun while the game reloads unseen, is not drawn while out of the gun, is drawn in the left hand once taken
//    (its top at reloadhandmag), slides along the well's axis while it goes in;
//  - the same magazine is drawn a second time at the left hip (a second draw of its mesh with only its bones placed) while one waits there;
//  - the charging handle part moves back with the pull.
#include "FarCry5Stereo.h"
#include "FarCry5Reload.h"
#include "FarCry5Record.h"
#include "FarCry5Fingers.h"
#include "FarCry5GameSound.h"
#include "FarCry5ReloadHint.h"
#include "FarCry5BoneMap.h"
#include "ModPath.h"

#include <array>
#include <functional>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace
{
    using clock = std::chrono::steady_clock;

    constexpr UINT     kPaletteBytes = 12288;
    constexpr uint32_t kRigid        = fc5::reload::kRigidBone;
    constexpr int      kMinVerts     = 20;        // a bone with fewer vertices is not looked at
    constexpr int      kSettleFrames = 45;        // reloadhandgame: the gun still against its controller this long before the well and the handle are measured
    constexpr float    kWindowSecs   = 3.2f;      // the learning window from the game's reload's start ...
    constexpr size_t   kOwnSteady    = 5;         // steady frames of the gun's own magazine part in the game's hand, for its hold
    constexpr float    kWindowMax    = 7.0f;      // ... longer while the game is still reloading (manualreload27: the .308's and HTI's bolt-action
                                                  // reloads were still running at 3.2 s in the reload5 run, their magazine not yet in the hand)
    constexpr float    kMinMove      = 0.08f;     // a magazine moves at least this far from the gun's body in a reload
    constexpr float    kMainMagMove  = 0.25f;     // a part of the gun's own mesh moving this far is the magazine before any separate draw (a bolt moves < 15 cm)
    constexpr int      kPropFrames   = 5;         // a draw present this many frames of the window, absent before it: the prop
    constexpr int      kQuietFrames  = 45;        // quiet frames before the well and the handle are measured
    constexpr float    kGrabNear     = 0.10f;     // manualreload53: a frame of the game's hand on the handle: within this of the handle point (m)
    // reloadhighlight (manualreload40): a box round points (weapon controller's axes) along the axes r, u, f (orthonormal), only the points
    // above u_min along u; 4 mm added on each side. Stored as Geom is (x mirrored for the left hand).
    fc5::reload::HlBox hl_box(const std::vector<glm::vec3>& pts, const glm::vec3& r, const glm::vec3& u, const glm::vec3& f, float u_min, bool lh)
    {
        fc5::reload::HlBox b{};
        glm::vec3          lo{ 1e9f }, hi{ -1e9f };
        int                n = 0;
        for (const auto& p : pts) {
            const glm::vec3 q{ glm::dot(p, r), glm::dot(p, u), glm::dot(p, f) };
            if (q.y < u_min) continue;
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            n++;
        }
        if (n < 8) return b;
        if (u_min > -1e8f) lo.y = std::max(lo.y, u_min);
        const glm::vec3 m = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f + glm::vec3{ 0.004f };
        auto mirv = [&](glm::vec3 v) { if (lh) v.x = -v.x; return v; };
        b.c    = mirv(r * m.x + u * m.y + f * m.z);
        b.a[0] = mirv(r * h.x);
        b.a[1] = mirv(u * h.y);
        b.a[2] = mirv(f * h.z);
        b.ok   = h.x < 0.25f && h.y < 0.25f && h.z < 0.4f;   // not the whole gun or an arm
        return b;
    }
    constexpr float    kAttached     = 0.003f;    // a part moving less than this against the body in a quiet second is fixed to the gun

    glm::mat4 from_rows(const float* f)
    {
        glm::mat4 m{ 1.0f };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[c][r] = f[r * 4 + c];
        return m;
    }
    void to_rows(const glm::mat4& m, float* f)
    {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) f[r * 4 + c] = m[c][r];
    }
    glm::mat4 affine(const glm::mat3& r, const glm::vec3& t)
    {
        glm::mat4 m{ r };
        m[3] = glm::vec4{ t, 1.0f };
        return m;
    }
    glm::mat4 shift(const glm::vec3& t) { return glm::translate(glm::mat4{ 1.0f }, t); }
    glm::vec3 point(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }
    glm::vec3 mir(glm::vec3 v, bool m)
    {
        if (m) v.x = -v.x;
        return v;
    }

    // A copy of FarCry5BowArrow.cpp's helper: a GPU buffer read back (a staging copy; immediate context only).
    bool read_gpu(ID3D11DeviceContext* ctx, ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        if (!b) return false;
        D3D11_BUFFER_DESC d{};
        b->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ID3D11Buffer* st = nullptr;
        if (dev) { dev->CreateBuffer(&d, nullptr, &st); dev->Release(); }
        if (!st) return false;
        ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
            ctx->Unmap(st, 0);
        }
        st->Release();
        return ok;
    }

    ID3D11Buffer* make_cb(ID3D11DeviceContext* ctx, UINT bytes)
    {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return nullptr;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* b = nullptr;
        dev->CreateBuffer(&d, nullptr, &b);
        dev->Release();
        return b;
    }

    using Key = std::pair<uint32_t, uint32_t>;   // (index count, bone or kRigid)

    struct Mesh   // one draw's vertices by bone, rest space (read once per weapon and draw)
    {
        bool                                       ok{ false };
        bool                                       skinned{ false };
        uint32_t                                   max_bone{ 0 };   // the highest bone any vertex hangs on (a weapon's are low)
        std::map<uint32_t, glm::vec3>              cent;   // bone -> its vertices' centroid
        std::map<uint32_t, int>                    n;      // bone -> how many
        std::map<uint32_t, std::vector<glm::vec3>> pts;    // bone -> up to 4000 of them (the magazine's top)
        std::vector<uint32_t>                      tris;   // reloadejectone: a small draw's indices, as drawn (no base) ...
        std::map<uint32_t, glm::vec3>              vpos;   // ... and their vertices
    };

    // reloadejectone (author 5 Nov: the first half of the indices was not one round): the triangles of one round of a draw holding several side by side: pieces joined by triangles or by a shared place,
    // the pieces split at the widest gap across the gun (x, z; along it a case and its bullet are apart), again until no gap of 3 mm is left. Empty: no such gap.
    std::vector<uint32_t> one_round(const Mesh& m, int& pieces, float& gap_out)
    {
        std::vector<uint32_t> t = m.tris;
        pieces  = 0;
        gap_out = 0.0f;
        bool any = false;
        for (int pass = 0; pass < 3; ++pass) {
            if (t.size() < 36 || t.size() % 3 != 0) break;
            std::map<uint32_t, uint32_t> par;
            std::function<uint32_t(uint32_t)> root = [&](uint32_t x) {
                auto it = par.find(x);
                if (it == par.end()) { par[x] = x; return x; }
                if (it->second == x) return x;
                const uint32_t r = root(it->second);
                par[x]           = r;
                return r;
            };
            auto join = [&](uint32_t a, uint32_t b) { par[root(a)] = root(b); };
            std::map<std::array<int, 3>, uint32_t> at;
            for (const uint32_t i : t) {
                const auto vp = m.vpos.find(i);
                if (vp == m.vpos.end()) return {};
                const std::array<int, 3> q{ int(std::lround(vp->second.x * 5000.0f)), int(std::lround(vp->second.y * 5000.0f)), int(std::lround(vp->second.z * 5000.0f)) };
                if (const auto a = at.find(q); a != at.end()) join(i, a->second);
                else at[q] = i;
            }
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                join(t[k], t[k + 1]);
                join(t[k], t[k + 2]);
            }
            struct Piece { glm::vec3 sum{ 0.0f }; int n{ 0 }; std::vector<uint32_t> idx; };
            std::map<uint32_t, Piece> ps;
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                Piece& p = ps[root(t[k])];
                for (int j = 0; j < 3; ++j) {
                    p.sum += m.vpos.at(t[k + j]);
                    p.n++;
                    p.idx.push_back(t[k + j]);
                }
            }
            if (pass == 0) pieces = int(ps.size());
            if (ps.size() < 2) break;
            float best = 0.0f, cut = 0.0f;
            int   axis = 0;
            for (const int ax : { 0, 2 }) {
                std::vector<float> c;
                for (const auto& [r, p] : ps) c.push_back(p.sum[ax] / float(p.n));
                std::sort(c.begin(), c.end());
                for (size_t i = 0; i + 1 < c.size(); ++i)
                    if (c[i + 1] - c[i] > best) {
                        best = c[i + 1] - c[i];
                        cut  = (c[i + 1] + c[i]) * 0.5f;
                        axis = ax;
                    }
            }
            if (best < 0.003f) break;
            std::vector<uint32_t> keep;
            for (const auto& [r, p] : ps)
                if (p.sum[axis] / float(p.n) < cut) keep.insert(keep.end(), p.idx.begin(), p.idx.end());
            if (keep.empty()) break;
            t       = std::move(keep);
            any     = true;
            gap_out = std::max(gap_out, best);
        }
        return any ? t : std::vector<uint32_t>{};
    }

    struct Rel   // a part on the gun, from a quiet frame
    {
        glm::mat4 r{ 1.0f };    // rest space -> the gun body's rest space (body^-1 part)
        glm::mat4 rc{ 1.0f };   // rest space -> the weapon controller's axes (x right, y up, z back; world units)
    };

    std::string v3(const glm::vec3& v) { return fmt::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z); }
    // reloadrec: the point at (rest space) in m, and M's turn as a quaternion: ",x,y,z,qw,qx,qy,qz".
    std::string rec_pose(const glm::mat4& M, const glm::vec3& at, float wsl)
    {
        glm::mat3 R{ M };
        for (int c = 0; c < 3; ++c) {
            const float l = glm::length(R[c]);
            if (l > 1e-6f) R[c] /= l;
        }
        const glm::quat q = glm::normalize(glm::quat_cast(R));
        const glm::vec3 p = point(M, at) / wsl;
        return fmt::format(",{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}", p.x, p.y, p.z, q.w, q.x, q.y, q.z);
    }
    std::string rec_turns(int side)   // reloadrec: the fingers' turns (w x y z, 15 of them), the G rows' last columns
    {
        glm::quat q[15]{};
        if (!fc5::fingers::game_finger_turns(side, q)) return {};
        std::string t;
        for (const auto& a : q) t += fmt::format(",{:.3f},{:.3f},{:.3f},{:.3f}", a.w, a.x, a.y, a.z);
        return t;
    }
    std::string rec_bends(int side, bool drawn)
    {
        float b[15]{};
        const bool ok = fc5::fingers::bends(side, drawn, b);
        std::string t;
        for (int i = 0; i < 15; ++i) t += ok ? fmt::format(",{:.0f}", b[i]) : std::string{ "," };
        return t;
    }
    void rec_flush(std::vector<std::string>& rows)
    {
        if (rows.empty()) return;
        std::ofstream f{ fc5::mod_file("fc5vr_reloadrec.csv"), std::ios::app };
        if (f.tellp() == std::streampos{ 0 })
            f << "kind,sig,n,t,state,insert,slide,hx,hy,hz,hqw,hqx,hqy,hqz,px,py,pz,pqw,pqx,pqy,pqz,mx,my,mz,mqw,mqx,mqy,mqz,seat,"
                 "b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14\n";
        for (const auto& r : rows) f << r << '\n';
        rows.clear();
    }
    std::string rec_clock()
    {
        const auto now = std::chrono::system_clock::now();
        const auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm    lt{};
        localtime_s(&lt, &t);
        return fmt::format("{:02d}:{:02d}:{:02d}.{:03d}", lt.tm_hour, lt.tm_min, lt.tm_sec, int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000));
    }
    std::string key_text(const Key& k) { return fmt::format("{}/{}", k.first, k.second == kRigid ? std::string{ "r" } : std::to_string(k.second)); }

    // manualreload33 (reloadsoundlearn): the moments of the game's reload, from the window's track (seconds, the magazine part from its seat, the
    // spare from that seat, the handle back; m, -1 unknown):
    // - the magazine out: its part 1.5 cm from its seat (it goes 5 cm or more);
    // - in: its part back within 8 mm of the seat after that; else (its part hidden or left away: a spare brought in) the spare, after being 10 cm
    //   or more away, within 6 cm of the seat and moving under 2 mm a frame;
    // - the handle back: 85 % of its furthest travel (1.5 cm or more); forward: under 15 % of it after that.
    // Each moment gets the game's handling sound nearest it (within 0.3 s, 0.15 s for a hand or pouch noise; manualreload36), the nearest pairs first, a sound to one moment only. manualreload34: not a
    // layer, an id heard 3 times or more in the reload, each (but one) within 20 ms of another id (reload7: 2361141864 came with every handling sound
    // of the 1911's and the AR-C's reloads, and was taken for the AR-C's magazine in). The line to save,
    // or empty when nothing matched; the log line says it all, with the track every 0.1 s, so a wrong match can be read.
    std::string learn_reload_sounds(uint32_t sig, const std::vector<std::array<float, 4>>& tr, const std::vector<fc5::gamesound::Heard>& heard)
    {
        static constexpr const char* kMoment[4]{ "magazine out", "magazine in", "handle back", "handle forward" };
        float       mt[4]{ -1.0f, -1.0f, -1.0f, -1.0f };
        const char* in_from = "";
        float       mx = 0.0f, hm = 0.0f;
        size_t      imx = 0;
        for (size_t i = 0; i < tr.size(); ++i) {
            if (tr[i][1] > mx) { mx = tr[i][1]; imx = i; }
            hm = std::max(hm, tr[i][3]);
        }
        if (mx >= 0.05f) {
            for (size_t i = 0; i < tr.size(); ++i)
                if (tr[i][1] >= 0.015f) { mt[0] = tr[i][0]; break; }
            for (size_t i = imx; i < tr.size(); ++i)
                if (tr[i][1] >= 0.0f && tr[i][1] <= 0.008f) { mt[1] = tr[i][0]; in_from = " (its part back in its seat)"; break; }
        }
        if (mt[1] < 0.0f && mt[0] >= 0.0f) {
            bool away = false;
            for (size_t i = 1; i < tr.size(); ++i) {
                if (tr[i][0] < mt[0]) continue;
                if (tr[i][2] >= 0.10f) away = true;
                if (away && tr[i][2] >= 0.0f && tr[i][2] <= 0.06f && tr[i - 1][2] >= 0.0f && std::abs(tr[i][2] - tr[i - 1][2]) < 0.002f) {
                    mt[1]   = tr[i][0];
                    in_from = " (the spare at the seat)";
                    break;
                }
            }
        }
        if (hm >= 0.015f) {
            size_t ib = tr.size();
            for (size_t i = 0; i < tr.size(); ++i)
                if (tr[i][3] >= 0.85f * hm) { mt[2] = tr[i][0]; ib = i; break; }
            for (size_t i = ib; i < tr.size(); ++i)
                if (tr[i][3] >= 0.0f && tr[i][3] <= 0.15f * hm) { mt[3] = tr[i][0]; break; }
        }
        std::map<uint32_t, std::pair<int, int>> occ;   // id: heard, with another id within 20 ms
        for (size_t j = 0; j < heard.size(); ++j) {
            bool with = false;
            for (size_t i = 0; i < heard.size() && !with; ++i) with = heard[i].id != heard[j].id && std::abs(heard[i].t - heard[j].t) <= 0.02f;
            auto& o = occ[heard[j].id];
            o.first++;
            if (with) o.second++;
        }
        // manualreload36 (the full walk of reload7): also left out, an id heard 3 times or more in the reload (a rattle, not one moment: the SVD's
        // 2551167301). A sound 7 or more weapons play (fc5::reload::sound_spread: the hands' and the pouch's, 4104195309 on 11) is taken only within
        // 0.15 s and after any other; another within 0.3 s (the magazine's seat is read late, under 8 mm: its click came 0.15-0.25 s before on the MP5,
        // the Skorpion, the AR-C). Ties within 20 ms: the sound fewer weapons play.
        std::set<uint32_t> layer;
        std::string        layer_txt;
        for (const auto& [id, o] : occ)
            if (o.first >= 3) {
                layer.insert(id);
                layer_txt += fmt::format(" {}x{}", id, o.first);
            }
        struct Pair { float dt; int m; size_t s; bool generic; int spread; };
        std::vector<Pair> pairs;
        for (int m = 0; m < 4; ++m)
            if (mt[m] >= 0.0f)
                for (size_t j = 0; j < heard.size(); ++j) {
                    if (layer.contains(heard[j].id)) continue;
                    const int  sp      = fc5::reload::sound_spread(heard[j].id);
                    const bool generic = sp >= 7;
                    if (std::abs(heard[j].t - mt[m]) <= (generic ? 0.15f : 0.3f)) pairs.push_back({ heard[j].t - mt[m], m, j, generic, sp });
                }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            if (a.generic != b.generic) return !a.generic;
            const int ba = int(std::abs(a.dt) / 0.02f), bb = int(std::abs(b.dt) / 0.02f);
            if (ba != bb) return ba < bb;
            return a.spread < b.spread;
        });
        int64_t           ids[4]{ -1, -1, -1, -1 };
        float             dts[4]{};
        std::vector<bool> used(heard.size(), false);
        for (const Pair& p : pairs) {
            if (ids[p.m] >= 0 || used[p.s]) continue;
            ids[p.m] = heard[p.s].id;
            dts[p.m] = p.dt;
            used[p.s] = true;
        }
        std::string got, snd, track;
        for (int m = 0; m < 4; ++m) {
            got += fmt::format("{}{} ", m ? "; " : "", kMoment[m]);
            if (mt[m] < 0.0f) got += "not seen";
            else if (ids[m] < 0) got += fmt::format("at {:.2f} s{}: no sound near", mt[m], m == 1 ? in_from : "");
            else got += fmt::format("at {:.2f} s{}: {} ({:+.2f} s)", mt[m], m == 1 ? in_from : "", ids[m], dts[m]);
        }
        for (const auto& h : heard) snd += fmt::format(" {}@{:.2f}", h.id, h.t);
        float next = 0.0f;
        auto  cm   = [](float v) { return v < 0.0f ? std::string{ "-" } : fmt::format("{:.0f}", v * 100.0f); };
        for (const auto& r : tr)
            if (r[0] >= next && track.size() < 1500) {
                track += fmt::format(" {:.1f}:{}/{}/{}", r[0], cm(r[1]), cm(r[2]), cm(r[3]));
                next = r[0] + 0.1f;
            }
        spdlog::info("[ReloadMag] weapon {}: reloadsoundlearn: {} | the game's handling sounds:{} | left out (layers, repeats):{} | track (s: magazine/spare from the seat/handle back, cm):{}",
                     sig, got, snd.empty() ? " none" : snd, layer_txt.empty() ? " none" : layer_txt, track.empty() ? " none" : track);
        if (ids[0] < 0 && ids[1] < 0 && ids[2] < 0 && ids[3] < 0) return {};
        return fc5::reload::learn_sounds(sig, ids);
    }
}

struct FarCry5Stereo::ReloadMagState
{
    int      frame{ -1 };
    uint32_t sig{ 0 };
    std::map<std::pair<uint32_t, uint32_t>, Mesh> mesh;   // (sig, count) -> its vertices
    std::set<std::pair<uint32_t, uint32_t>>       mesh_failed;
    int                                           scan_next{ 0 };

    // This frame: each part's transform (rest space -> game world), the draws seen, the body, and the placement of the pass.
    std::map<Key, glm::mat4> cur;
    std::set<uint32_t>       cur_counts;
    glm::mat4                body{ 1.0f };      // the gun's body (rest -> game world)
    int                      body_frame{ -100 };
    // manualreload31: each draw's object transform at its last draw, so a part drawn before the gun's main draw (a revolver's rounds) takes
    // the gun's body moved as its own object moved since (else the last frame's body: the rounds lagged out of the gun while it moved).
    std::map<uint32_t, std::pair<glm::mat4, int>> last_om;
    uint32_t                 rev_said_sig{ 0 };
    bool                     place_ok{ false };   // w2d, ctrl, back_c of this frame
    int                      place_frame{ -100 };
    glm::mat4                w2d{ 1.0f };       // game world -> drawn world (the weapon pass: w_pos + w_rot (rot (x - cam) - grip))
    glm::mat4                d2w{ 1.0f };
    glm::mat4                ctrl{ 1.0f };      // the weapon controller (its raw axes, without grippitch) in the drawn world
    glm::vec3                back_c{ 0.0f, 0.0f, 1.0f };   // the gun's back, controller axes

    // Quiet frames (the gun held, its magazine in, the game not reloading): each part on the gun.
    std::map<Key, Rel>       base;
    std::set<uint32_t>       base_counts;
    uint32_t                 base_sig{ 0 };
    int                      base_frames{ 0 };
    glm::mat3                b2c{ 1.0f }, c2b{ 1.0f };   // body rest space <-> controller axes (vectors)
    std::map<Key, glm::vec3> snap;              // each part's centre in body space, a second ago
    std::map<Key, float>     drift;             // ... how far it moved against the body in that second
    clock::time_point        snap_t{};

    // Per weapon, this session.
    std::set<uint32_t>            measured;     // the well measured on the model
    std::map<uint32_t, glm::vec3> well_c;       // the model's well (controller axes, this hand)
    std::map<uint32_t, glm::vec3> axis_c;       // a learnt well axis (controller axes, this hand)
    std::map<uint32_t, Key>       handle_guess; // the main mesh's part nearest the handle point (no saved part)
    std::map<uint32_t, std::vector<std::pair<Key, glm::vec3>>> handle_cands;   // parts by distance from the handle point
    std::map<uint32_t, int>       handle_idx;
    std::set<uint32_t>            grab_measured;   // the handle's grab point measured on its tabs (built in)
    // A pump shotgun's pump (and the SPAS-12's bolt) at rest on the gun's body, from the first quiet second with it in hand: drawn there
    // (plus the hand's pull), so the game's own pump after each shot is not seen (it is worked by hand; manualreload9).
    std::map<Key, glm::mat4>      pin_rest;
    bool                          bolt_said{ false };   // reloadboltturn: its pivot logged once
    uint32_t                      pin_sig{ 0 };
    // A revolver (manualreload10): the gun's body space -> the weapon controller's axes (a quiet frame's), and per weapon the rounds'
    // length ahead of their centres (their noses); the fired cases dropped out of its cylinder.
    glm::mat4                     b2c4{ 1.0f };
    // manualreload39 (author 3 Nov 12:35): the gun's body -> the weapon controller's axes held
    // steady (the first quiet frames of a weapon averaged, then followed over about 5 s). A part's place in the controller's axes (Rel::rc) was
    // read again at every quiet frame, so whatever the game's own animation moved the gun from the controller (its sway and lag as the hand
    // turns, a step) moved the hip's and the left hand's magazine with it; they now take the part on the body (Rel::r) through this.
    glm::mat4                     b2c_hold{ 1.0f };
    uint32_t                      b2c_hold_sig{ 0 };
    int                           b2c_hold_n{ 0 };
    std::map<uint32_t, float>     rev_nose;
    uint32_t                      shell_hip_said{ 0 };   // reloadshellhip: logged for this weapon
    std::map<uint32_t, glm::vec3> rev_seat;   // reloadgrenadeseat: the M79's grenade's move onto its bore (body space; measured once per weapon)
    bool                          rev_dump_init{ false };
    // reloadhandsnap (manualreload41): the free hand's drawn pose, in the gun controller's axes (raw, grippitch undone), eased in and out.
    bool                          snap_grab{ false };          // a pull under way: the hand's place on the handle taken at its start
    glm::mat3                     snap_grab_rot{ 1.0f };
    glm::vec3                     snap_grab_pos{ 0.0f };
    float                         snap_rack0{ 0.0f };
    glm::mat3                     snap_rel_rot{ 1.0f };        // the last target (kept to ease out from)
    glm::vec3                     snap_rel_pos{ 0.0f };
    float                         snap_w{ 0.0f };
    int                           snap_frame{ -1 };
    int                           snap_said{ -1 };
    clock::time_point             hsnap_t{};
    glm::mat3                     snap_out_rot{ 1.0f };        // this frame's answer
    glm::vec3                     snap_out_pos{ 0.0f };
    uint32_t                      rev_dump_sig{ 0 }, rev_dumps_seen{ 0 };

    // Diagnostics (2 Nov 12:32, author;
    // the log had no cause): every 5 s, when the hip's magazine was not drawn in some frames, or the gun's main draw was missing.
    clock::time_point diag_t{};
    int               diag_frames{ 0 }, diag_main{ 0 }, diag_none{ 0 }, diag_want{ 0 }, diag_drawn{ 0 }, diag_old{ 0 }, diag_swapped{ 0 };
    int               diag_old_frame{ -1 }, diag_swapped_frame{ -1 };
    uint64_t          diag_hidden0{ 0 }, diag_pinned0{ 0 }, diag_moved0{ 0 }, diag_racked0{ 0 };

    // Learning (from the game's reload's start).
    bool                     busy_was{ false };
    bool                     learning{ false };
    bool                     learn_mag{ false }, learn_handle{ false };
    bool                     spare_said{ false };    // manualreload22: the spare magazine drawn in the hand with the gun's in (logged once)
    bool                     learn_props{ false };   // a built-in magazine with no spare known: the draws the game's hand brings in, logged
    // reloadgamehold (manualreload17): the game's spare magazine in its left hand (fc5::fingers::game_palm), the free controller's axes (m,
    // right-handed) each frame it is within 25 cm; and the free controller's place on the charging handle (the gun controller's axes, the
    // handle at rest) each frame the handle is pulled back.
    std::vector<glm::vec3>   hold_pts;
    std::vector<glm::vec3>   grab_pts;
    int                      grab_far{ 0 };          // manualreload53: frames with the handle back but the free hand away from it
    float                    grab_near{ 1e9f };      // ... and the nearest the hand came to the handle point (m)
    uint32_t                 hold_count{ 0 };
    // manualreload19: the gun's own magazine part out of its seat, in the game's hand (most guns' reload moves the magazine itself, with no
    // separate prop draw: reload4, 2 Nov 21:22-22:00, "brought in the draws none" on every built-in gun without props); and why a hold was
    // not learnt: the game's palm read (ok, none, a frame old), the nearest candidate to it (m).
    std::vector<glm::vec3>   own_pts;
    int                      palm_ok{ 0 }, palm_none{ 0 }, palm_old{ 0 };
    float                    near_new{ 1e9f }, near_own{ 1e9f };
    uint32_t                 learn_sig{ 0 };
    clock::time_point        learn_t0{};
    int                      learn_frames{ 0 };
    std::map<Key, Rel>       learn_base;
    std::set<uint32_t>       learn_base_counts;
    std::map<Key, float>     maxd;
    std::map<Key, glm::vec3> first_dir;         // body space
    std::map<Key, float>     back_max, back_perp;   // the handle: the furthest back along the gun, and how far off that line then
    std::map<uint32_t, int>  appeared;
    // manualreload33 (reloadsoundlearn): each frame of the window, seconds from its start, the magazine part's distance from its seat, the nearest
    // draw new to the reload (the spare) from that seat, the handle's travel back (m; -1: not known).
    std::vector<std::array<float, 4>> snd_track;
    // manualreload35: each part within 15 cm of the free controller in the window (the game's left hand there): frames, nearest (m). reload7's
    // -GameAnim pictures of the AK show its new magazine in the left hand while the old one is still in the gun; the AK's magazine is a bone of the
    // gun's own mesh (45582/1), and the hold learnt from it in reload6 was the old magazine knocked out. Logged, to find the spare's part.
    std::map<Key, std::pair<int, float>> near_hand;
    // reloadrec: rows waiting for the file (written when a reload ends), this session's reload number, the manual steps under way and when they began; the
    // magazine as drawn this frame (rest space -> the gun controller's axes) and its part.
    std::vector<std::string> rec;
    int                      rec_n{ 0 };
    bool                     rec_vr{ false };
    clock::time_point        rec_vr_t0{};
    glm::mat4                rec_mag{ 1.0f };
    Key                      rec_mag_k{ 0, 0 };
    int                      rec_mag_frame{ -1000 };
    // reloadhandgame: the magazine's centre on the gun (the gun controller's axes, from its main part), this frame: what the hand's place on it is measured from.
    glm::vec3                mag_ref_c{ 0.0f };
    // reloadhandgame: frames in a row with the gun still against its controller. The well and the handle are measured only then: right after a draw the gun is still swinging in
    // the game's animation, and a well measured there sat 1-2 cm off for the session (4 Nov run: the P226's -0.025 against -0.041), the hand's magazine with it.
    int                      settle{ 0 };
    glm::vec3                settle_p{ 0.0f };

    // Drawing.
    ID3D11Buffer* standin[2]{};   // a rigid part's object transform (b1) and previous (b3)
    ID3D11Buffer* orig[3]{};      // VS b1..b3 as the game bound them
    ID3D11Buffer* pouch_pal{ nullptr };    // the hip's copy: a skinned palette ...
    ID3D11Buffer* pouch_obj{ nullptr };    // ... or a rigid object transform
    bool          shown_hand{ false }, shown_pouch{ false }, shown_handle{ false };   // this frame
    uint64_t      moved{ 0 }, hidden{ 0 }, pinned{ 0 }, pouched{ 0 }, racked{ 0 };
    int           last_mode{ -1 };
    bool          pouch_said{ false }, rack_said{ false };
    std::set<std::tuple<uint32_t, UINT, UINT, INT>> scan_seen;

    // reloaddrop: the dropped magazine falling (drawn world: the game world's axes, z up), then lying on the ground.
    bool                                   fall_init{ false };
    uint32_t                               fall_drops{ 0 };   // drops() last seen
    bool                                   falling{ false };
    uint32_t                               fall_sig{ 0 };
    clock::time_point                      fall_t0{};
    std::map<uint32_t, uint32_t>           fall_seen;         // per magazine draw (the AR-C's gun and its top round): drops() it last saw
    std::map<uint32_t, std::vector<std::pair<Key, glm::mat4>>> fall_x0;   // per draw: each part, rest space -> drawn world, at the drop
    glm::vec3                              fall_c0{ 0.0f };   // its centre then
    glm::vec3                              fall_axis{ 1.0f, 0.0f, 0.0f };   // it turns onto its side about the barrel's line as it falls
    float                                  fall_turn{ 1.5708f };
    float                                  fall_ground{ 0.0f };
    float                                  fall_t_land{ 0.5f };

    // The rounds a pull of the handle ejects (drawn world): each flies out of the ejection port, tumbling, and lies on the ground 10 s.
    struct Ejected
    {
        bool              half{ false };   // reloadejectone: its draw holds two rounds, only the first half of its indices drawn
        Key               k{};
        glm::mat4         x0{ 1.0f };      // rest space -> drawn world, at the port
        glm::vec3         c0{ 0.0f };      // its centre then
        glm::vec3         vel{ 0.0f };     // drawn world units per second
        glm::vec3         axis{ 0.0f, 0.0f, 1.0f };
        float             spin{ 20.0f };   // rad/s while in the air
        float             t_land{ 0.4f };
        clock::time_point t0{};
        bool              landed{ false };   // its sound played
    };
    bool                 eject_init{ false };
    uint32_t             eject_seen{ 0 };
    uint32_t             eject_sig{ 0 };
    uint32_t             eject_said{ 0 };
    std::map<uint32_t, std::pair<ID3D11Buffer*, UINT>> eject_ib;   // reloadejectone: (draw) -> one round's indices
    std::vector<Ejected> ejected;

    glm::vec3 centre(uint32_t sig_, const Key& k) const
    {
        const auto mi = mesh.find({ sig_, k.first });
        if (mi == mesh.end() || !mi->second.ok) return glm::vec3{ 0.0f };
        const auto ci = mi->second.cent.find(k.second == kRigid ? 0u : k.second);
        return ci != mi->second.cent.end() ? ci->second : glm::vec3{ 0.0f };
    }
    bool weapon_draw(uint32_t sig_, uint32_t count) const   // read, and its bones a weapon's
    {
        const auto mi = mesh.find({ sig_, count });
        return mi != mesh.end() && mi->second.ok && mi->second.max_bone <= fc5::reload::kMaxWeaponBone;
    }
};

// reload_mag_frame and reload_mag_draw: their long blocks are lambdas of their own, not inlined ([&]() __declspec(noinline) { ... }();, left at their old
// indent). As one function each they took the compiler 20 minutes (author 5 Nov: builds too long).
// The frame before this one is done: the parts on the gun kept from quiet frames, the well and handle measured, a learning window followed,
// the shown flags passed on.
void FarCry5Stereo::reload_mag_frame(uint32_t sig)
{
    auto&      s   = *m_rm;
    const auto now = clock::now();
    const bool lh  = m_left_handed.load();
    if (fc5::reload::in_hand()) {
        fc5::reload::View dv{};
        s.diag_frames++;
        if (s.body_frame == s.frame) s.diag_main++;
        s.diag_none += std::clamp(m_frame.load() - s.frame - 1, 0, 30);   // frames with no weapon-pass draw at all
        if (fc5::reload::view(dv) && dv.pouch_mag) {
            s.diag_want++;
            if (s.shown_pouch) s.diag_drawn++;
        }
    }
    if (now - s.diag_t > std::chrono::seconds(5)) {
        if (s.diag_frames > 0 && (s.diag_want - s.diag_drawn > 2 || s.diag_frames - s.diag_main > 2 || s.diag_none > 2))
            spdlog::info("[ReloadMag] last 5 s, weapon {} in hand: {} frames with weapon-pass draws, {} without any; the gun's main draw in {}; the hip's magazine wanted in {}, "
                         "drawn in {} (the hand map old in {}, another move's constants on the gun in {}); this reload's draws: {} hidden, {} kept on the gun, {} moved, {} racked",
                         sig, s.diag_frames, s.diag_none, s.diag_main, s.diag_want, s.diag_drawn, s.diag_old, s.diag_swapped, s.hidden - s.diag_hidden0, s.pinned - s.diag_pinned0,
                         s.moved - s.diag_moved0, s.racked - s.diag_racked0);
        s.diag_t      = now;
        s.diag_frames = s.diag_main = s.diag_none = s.diag_want = s.diag_drawn = s.diag_old = s.diag_swapped = 0;
        s.diag_hidden0 = s.hidden;
        s.diag_pinned0 = s.pinned;
        s.diag_moved0  = s.moved;
        s.diag_racked0 = s.racked;
    }
    fc5::reload::set_model_shown(s.shown_hand);
    fc5::reload::set_pouch_shown(s.shown_pouch);
    fc5::reload::set_handle_shown(s.shown_handle);
    s.shown_hand = s.shown_pouch = s.shown_handle = false;
    const bool place = s.place_ok && s.place_frame == s.frame;
    const bool body  = s.body_frame == s.frame;
    // The gun's back in the weapon controller's axes (the charging handle is pulled along it), kept as for the right hand.
    if (place && sig != 0) fc5::reload::note_back(sig, mir(s.back_c, lh));
    if (sig != s.sig) {
        s.sig = sig;
        s.base.clear();
        s.base_counts.clear();
        s.base_sig    = 0;
        s.base_frames = 0;
        s.snap.clear();
        s.drift.clear();
        s.pin_rest.clear();
        s.pin_sig = 0;
        if (s.learning) {
            s.learning = false;
            spdlog::info("[ReloadMag] learning dropped: another weapon");
        }
    }
    const bool      in_hand = fc5::reload::in_hand();
    const bool      quiet   = in_hand && !fc5::reload::hides_game_reload() && !fc5::reload::button_reload() && !s.cur.empty() && place && body;
    const glm::mat4 bi      = glm::inverse(s.body);
    if (quiet) {
        const glm::mat4 ci = glm::inverse(s.ctrl);
        for (const auto& [k, T] : s.cur) s.base[k] = Rel{ bi * T, ci * s.w2d * T };
        s.base_counts = s.cur_counts;
        s.base_sig    = sig;
        s.base_frames++;
        s.b2c  = glm::mat3{ ci * s.w2d * s.body };
        s.b2c4 = ci * s.w2d * s.body;
        {
            const glm::vec3 bp{ s.b2c4[3] };
            s.settle   = glm::length(bp - s.settle_p) < 0.002f * m_world_scale.load() ? s.settle + 1 : 0;
            s.settle_p = bp;
        }
        if (s.b2c_hold_sig != sig) {
            s.b2c_hold_sig = sig;
            s.b2c_hold_n   = 0;
        }
        if (s.b2c_hold_n < kQuietFrames) {
            s.b2c_hold = s.b2c_hold_n == 0 ? s.b2c4 : s.b2c_hold + (s.b2c4 - s.b2c_hold) * (1.0f / float(s.b2c_hold_n + 1));   // the mean so far
            s.b2c_hold_n++;
        } else {
            s.b2c_hold += (s.b2c4 - s.b2c_hold) * 0.002f;   // then a slow follow (a new grip calibration settles in a few seconds)
        }
        s.c2b = glm::inverse(s.b2c);
        if (now - s.snap_t > std::chrono::seconds(1)) {
            for (const auto& [k, r] : s.base) {
                const glm::vec3 c = point(r.r, s.centre(sig, k));
                if (const auto it = s.snap.find(k); it != s.snap.end()) s.drift[k] = glm::length(c - it->second);
                s.snap[k] = c;
            }
            s.snap_t = now;
        }
    }

    // A pump shotgun's pump at rest (the gun held still a moment after it came into the hand; the game's pump moves only after a shot).
    // A revolver's crane, drum and extractor and its rounds likewise (the cylinder closed and full: the game's own reload and its drum's
    // turn at each shot are not seen; the mod swings it and shows each chamber's round).
    [&]() __declspec(noinline) {
    if (fc5::reload::Revolver rp{}; quiet && sig != 0 && s.pin_sig != sig && s.base_frames >= kQuietFrames && fc5::reload::revolver_parts(sig, rp)) {
        int n = 0;
        auto take = [&](const Key& k) {
            if (const auto b = s.base.find(k); b != s.base.end()) {
                s.pin_rest[k] = b->second.r;
                n++;
            }
        };
        int nr = 0;
        for (int i = 0; i < rp.nswing; ++i) take({ rp.main, rp.swing[i] });
        // allreload: a rigid round's draw (the M79's grenade, the RPG-7's rocket) is one part, its key kRigid.
        for (int i = 0; i < rp.nround; ++i) { const int n0 = n; take({ rp.rounds, rp.rigid_rounds ? kRigid : rp.round_bones[i] }); nr += n - n0; }
        if (rp.cap_bone != 0xFFFFu) take({ rp.rounds, rp.cap_bone });
        // The rounds (six; a break action's two shells) and at least the crane and drum (the barrels): a part with fewer than 20 vertices is
        // not tracked. The RPG-7 has nothing that swings: its rocket alone.
        if (nr == rp.nround && n >= rp.nround + (rp.noopen ? 0 : rp.brk ? 1 : 2)) {
            s.pin_sig = sig;
            // reloadgrenadeseat: the grenade moved onto its bore as measured (the first time: below, with the well).
            if (const auto rs = s.rev_seat.find(sig); rs != s.rev_seat.end() && rp.rigid_rounds && fc5::reload::grenade_seat())
                if (const auto g = s.pin_rest.find(Key{ rp.rounds, kRigid }); g != s.pin_rest.end()) g->second = shift(rs->second) * g->second;
            spdlog::info("[ReloadMag] weapon {}: the {} ({} parts) kept on the gun, {} by the mod", sig,
                         rp.noopen ? "rocket" : rp.brk ? "break action's barrels and shells" : "revolver's cylinder (crane, drum, extractor, rounds)",
                         n, rp.noopen ? "loaded and shown" : rp.brk ? "tipped open" : "swung out");
        } else {
            s.pin_rest.clear();
        }
    }
    // A belt-fed gun's feed cover likewise (manualreload12): kept where it sits closed, lifted by the mod only.
    if (fc5::reload::Belt bp{}; quiet && sig != 0 && s.pin_sig != sig && s.base_frames >= kQuietFrames && fc5::reload::belt_parts(sig, bp)) {
        if (const auto b = s.base.find({ bp.main, bp.cover }); b != s.base.end()) {
            s.pin_rest[b->first] = b->second.r;
            s.pin_sig            = sig;
            spdlog::info("[ReloadMag] weapon {}: the feed cover (part {}/{}) kept closed on the gun, lifted by the mod ({:.0f} deg about its hinge)", sig, bp.main, bp.cover, bp.open_deg);
        }
    }
    // (reloadboltpin: a bolt too: the game's own cycle after a shot not seen, only the hand's.)
    // (reloadboltparts: a bolt action's rest taken once its bolt's draw has been read, so the other bolt parts are among the parts.)
    auto bolt_mesh_wait = [&](const fc5::reload::HandleParts& h) {
        const auto mk = std::make_pair(uint32_t(sig), h.count);
        return fc5::reload::bolt_fed(sig) && !s.mesh.contains(mk) && !s.mesh_failed.contains(mk);
    };
    if (fc5::reload::HandleParts pp{}; quiet && sig != 0 && s.pin_sig != sig && s.base_frames >= kQuietFrames && (fc5::reload::tube_fed(sig) || fc5::reload::bolt_fed(sig)) &&
                                       fc5::reload::handle_parts(sig, pp) && !bolt_mesh_wait(pp)) {
        std::string bl;
        for (int i = 0; i < pp.nbones; ++i)
            if (const auto b = s.base.find({ pp.count, pp.bones[i] }); b != s.base.end()) {
                s.pin_rest[b->first] = b->second.r;
                bl += fmt::format("{}{}", bl.empty() ? "" : "+", pp.bones[i]);
            }
        if (!bl.empty()) {
            s.pin_sig = sig;
            spdlog::info("[ReloadMag] weapon {}: the pump (part(s) {}/{}) kept at rest on the gun, moved by the hand only", sig, pp.count, bl);
            // reloadboltparts (author 6 Nov 13:55): a bolt action's other bolt parts in the handle's
            // draw (palette slots named BOLT_* in the bone map: the .308's BOLT_STOP, the HTI .50's BOLT_COVER), kept at rest as well; the game's
            // cycle after each shot moved them beside the kept bolt. Kept whatever the switch; drawn there only with it on (below).
            if (fc5::reload::bolt_fed(sig)) {
                const auto mm   = s.mesh.find(std::make_pair(uint32_t(sig), pp.count));
                int        alts = 0;
                const int  bd   = fc5::bonemap::find(pp.count, mm != s.mesh.end() && mm->second.ok ? int(mm->second.max_bone) : -1, false, alts);
                std::string cl;
                for (const auto& [k, r] : s.base) {
                    if (k.first != pp.count || k.second == kRigid || k.second == 0) continue;
                    bool handle = false;
                    for (int i = 0; i < pp.nbones; ++i) handle = handle || pp.bones[i] == k.second;
                    const char* nm = handle ? nullptr : fc5::bonemap::slot_name(bd, int(k.second));
                    if (!nm || std::strncmp(nm, "BOLT", 4) != 0) continue;
                    s.pin_rest[k] = r.r;
                    cl += fmt::format("{}{}/{} {}", cl.empty() ? "" : ", ", k.first, k.second, nm);
                }
                spdlog::info("[ReloadMag] weapon {}: reloadboltparts: {} (model {}{})", sig, cl.empty() ? std::string{ "no other bolt part in the bolt's draw" } : "kept at rest too: " + cl,
                             fc5::bonemap::model(bd), alts > 0 ? fmt::format(", {} other model(s) share its count", alts) : std::string{});
            }
        }
    }
    }();

    fc5::reload::MagPart parts[8];
    int                  np    = 0;
    const bool           known = sig != 0 && fc5::reload::mag_parts(sig, parts, 8, np);
    glm::vec3            up_c  = glm::vec3{ 0.0f, 1.0f, 0.0f } - s.back_c * s.back_c.y;   // the gun's up across its barrel
    up_c                       = glm::length(up_c) > 1e-4f ? glm::normalize(up_c) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    auto is_mag = [&](const Key& k) {
        for (int i = 0; i < np; ++i)
            if (!parts[i].prop && parts[i].count == k.first &&
                (parts[i].rigid || std::find(parts[i].bones, parts[i].bones + parts[i].nbones, k.second) != parts[i].bones + parts[i].nbones))
                return true;
        return false;
    };

    // The well, on the model (once per weapon and session, after a quiet moment): the top of the magazine's vertices along the well's axis.
    // A revolver's well: the noses of its rounds seated in the swung-out cylinder, at its centre; the axis along the barrel (a round goes
    // in from behind). Measured from the closed cylinder's rounds turned by the crane's swing.
    [&]() __declspec(noinline) {
    if (fc5::reload::Revolver rp{}; quiet && s.pin_sig == sig && sig != 0 && !s.measured.contains(sig) && fc5::reload::revolver_parts(sig, rp)) {
        const auto      mi  = s.mesh.find({ sig, rp.rounds });
        const glm::vec3 fwd{ 0.0f, 1.0f, 0.0f };   // the model's forward, along the hinge
        if (mi != s.mesh.end() && mi->second.ok) {
            // reloadgrenadeseat (author 6 Nov 12:40: the M79's grenade; final_m's M79_open: beside the open breech, not in
            // it): the game's grenade at rest sits off the bore. The barrel's back (its rearmost 2 cm along the model's y, forward): the middle of
            // its extent across (x, z) is the bore's centre, its rearmost point the breech face. The grenade (lying along y) is moved so the middle
            // of its extent across is there and its base (rearmost) at the breech face. Before the well is measured, so the well is the bore's.
            if (rp.brk && rp.rigid_rounds && !rp.door && rp.nswing > 0 && fc5::reload::grenade_seat() && !s.rev_seat.contains(sig)) {
                const auto dm = s.mesh.find({ sig, rp.main });
                if (dm == s.mesh.end() || !dm->second.ok) return;   // (the main draw's vertices first)
                const Key  gk{ rp.rounds, kRigid };
                const auto pb = s.pin_rest.find(Key{ rp.main, rp.swing[0] });
                const auto bv = dm->second.pts.find(rp.swing[0]);
                const auto pg = s.pin_rest.find(gk);
                const auto gv = mi->second.pts.find(rp.round_bones[0]);
                glm::vec3  seat{ 0.0f };
                bool       ok = false;
                if (pb != s.pin_rest.end() && bv != dm->second.pts.end() && pg != s.pin_rest.end() && gv != mi->second.pts.end() && !bv->second.empty() && !gv->second.empty()) {
                    float yb = 1e9f;
                    for (const auto& q : bv->second) yb = std::min(yb, point(pb->second, q).y);
                    glm::vec3 lo{ 1e9f }, hi{ -1e9f }, glo{ 1e9f }, ghi{ -1e9f };
                    for (const auto& q : bv->second)
                        if (const glm::vec3 p = point(pb->second, q); p.y < yb + 0.02f) {
                            lo = glm::min(lo, p);
                            hi = glm::max(hi, p);
                        }
                    for (const auto& q : gv->second) {
                        const glm::vec3 p = point(pg->second, q);
                        glo               = glm::min(glo, p);
                        ghi               = glm::max(ghi, p);
                    }
                    const glm::vec3 ge = ghi - glo;
                    seat = glm::vec3{ 0.5f * (lo.x + hi.x - glo.x - ghi.x), yb - glo.y, 0.5f * (lo.z + hi.z - glo.z - ghi.z) };
                    ok   = ge.y >= std::max(ge.x, ge.z) && glm::length(seat) < 0.30f;   // a grenade lying along the bore; less than 30 cm to move
                    spdlog::info("[ReloadMag] weapon {}: reloadgrenadeseat: the bore's centre at the breech ({:.3f} {:.3f}), the breech face at y {:.3f}; the grenade "
                                 "{:.1f} x {:.1f} x {:.1f} cm, its middle ({:.3f} {:.3f}), its base at y {:.3f} (the model's): {} ({:.1f} {:.1f} {:.1f} cm)",
                                 sig, 0.5f * (lo.x + hi.x), 0.5f * (lo.z + hi.z), yb, ge.x * 100.0f, ge.y * 100.0f, ge.z * 100.0f, 0.5f * (glo.x + ghi.x),
                                 0.5f * (glo.z + ghi.z), glo.y, ok ? "moved onto the bore by" : "not moved (not lying along the bore, or too far)",
                                 seat.x * 100.0f, seat.y * 100.0f, seat.z * 100.0f);
                }
                s.rev_seat[sig] = ok ? seat : glm::vec3{ 0.0f };
                if (ok) pg->second = shift(seat) * pg->second;
            }
            glm::vec3 cen{ 0.0f };
            int       nc = 0;
            float     nose = 0.0f, nose_sum = 0.0f;
            // allreload: a rigid round's part is keyed kRigid, its vertices kept under 0; a muzzle-loaded rocket's lead point is its
            // tail (it goes in backward), not its nose.
            const glm::vec3 lead_d = rp.muzzle ? -fwd : fwd;
            auto rk = [&](uint32_t b) { return Key{ rp.rounds, rp.rigid_rounds ? kRigid : b }; };
            for (int i = 0; i < rp.nround; ++i) {
                const uint32_t b  = rp.round_bones[i];
                const auto     pr = s.pin_rest.find(rk(b));
                const auto ce = mi->second.cent.find(b);
                if (pr == s.pin_rest.end() || ce == mi->second.cent.end()) continue;
                cen += point(pr->second, ce->second);
                nc++;
            }
            if (nc == rp.nround) {
                // reloadratdoor: its hinge: the opening parts' front top (the model's: y forward, z up), so their back lifts; the main mesh first.
                if (rp.door) {
                    const auto dm = s.mesh.find({ sig, rp.main });
                    if (dm == s.mesh.end() || !dm->second.ok) return;
                    float ymax = -1e9f, zmax = -1e9f;
                    for (int i = 0; i < rp.nswing; ++i) {
                        const auto pr = s.pin_rest.find(Key{ rp.main, rp.swing[i] });
                        const auto pi = dm->second.pts.find(rp.swing[i]);
                        if (pr == s.pin_rest.end() || pi == dm->second.pts.end()) continue;
                        for (const auto& q : pi->second) {
                            const glm::vec3 w = point(pr->second, q);
                            ymax              = std::max(ymax, w.y);
                            zmax              = std::max(zmax, w.z);
                        }
                    }
                    if (ymax < -1e8f) return;
                    fc5::reload::set_door_pivot(sig, glm::vec3{ 0.0f, ymax, zmax });
                    spdlog::info("[ReloadMag] weapon {}: reloadratdoor: the back opens about ({:.3f} {:.3f} {:.3f}) (the model's)", sig, 0.0f, ymax, zmax);
                }
                cen /= float(rp.nround);
                for (int i = 0; i < rp.nround; ++i) {
                    const uint32_t   b  = rp.round_bones[i];
                    const glm::mat4& r  = s.pin_rest.at(rk(b));
                    const glm::vec3  cb = point(r, mi->second.cent.at(b));
                    float            mx = 0.0f;
                    if (const auto pi = mi->second.pts.find(b); pi != mi->second.pts.end())
                        for (const auto& q : pi->second) {
                            mx   = std::max(mx, glm::dot(point(r, q) - cb, lead_d));
                            nose = std::max(nose, glm::dot(point(r, q) - cen, lead_d));
                        }
                    nose_sum += mx;
                }
                // (A break action: about the gun's right axis, so the chambers' axis tips down with the barrels; manualreload29.)
                const glm::mat4 Ro   = rp.door ? glm::mat4{ 1.0f } : shift(rp.pivot) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(rp.open_deg), rp.axis) * shift(-rp.pivot);   // (reloadratdoor: the round stays)
                const glm::vec3 well = point(s.b2c4, point(Ro, cen + lead_d * nose));
                glm::vec3       ax   = glm::mat3{ s.b2c4 } * (glm::mat3{ Ro } * lead_d);
                ax                   = glm::length(ax) > 1e-6f ? glm::normalize(ax) : (rp.muzzle ? s.back_c : -s.back_c);
                s.measured.insert(sig);
                s.rev_nose[sig]        = nose_sum / float(rp.nround);
                if (rp.muzzle || rp.door) fc5::reload::set_round_lead(sig, s.rev_nose[sig]);   // the pad poll's hold: the rocket's tail behind the hand
                // reloadmuzzlezone (author 5 Nov 15:51: the RPG's zone): the seated rocket's depth in the tube, from
                // its tail to the muzzle: the rearmost of its points well off its axis (the warhead, wider than the tube's bore; more than 0.65
                // of its widest), along its length. Its tail goes in there, so that is its travel (FarCry5Reload.cpp).
                if (rp.muzzle) {
                    const uint32_t   b0 = rp.round_bones[0];
                    const glm::mat4& r  = s.pin_rest.at(rk(b0));
                    const glm::vec3  cb = point(r, mi->second.cent.at(b0));
                    float tail_a = 1e9f, rmax = 0.0f, muz_a = 1e9f;
                    if (const auto pi = mi->second.pts.find(b0); pi != mi->second.pts.end()) {
                        for (const auto& q : pi->second) {
                            const glm::vec3 p = point(r, q) - cb;
                            const float     a = glm::dot(p, fwd);
                            tail_a = std::min(tail_a, a);
                            rmax   = std::max(rmax, glm::length(p - fwd * a));
                        }
                        for (const auto& q : pi->second) {
                            const glm::vec3 p = point(r, q) - cb;
                            const float     a = glm::dot(p, fwd);
                            if (glm::length(p - fwd * a) > 0.65f * rmax) muz_a = std::min(muz_a, a);
                        }
                    }
                    const float depth = muz_a - tail_a;
                    const bool  ok    = rmax > 0.01f && depth >= 0.10f && depth <= 0.70f;
                    if (ok) fc5::reload::set_muzzle_depth(sig, depth);
                    spdlog::info("[ReloadMag] weapon {}: reloadmuzzlezone: the seated rocket's depth in the tube {:.1f} cm (its tail to its widest part's rear; widest {:.1f} cm across){}",
                                 sig, depth * 100.0f, rmax * 200.0f, ok ? ": its way in starts at the muzzle" : ": not taken (the table's travel stays)");
                }
                s.well_c[sig]          = well;
                s.axis_c[sig]          = ax;
                const std::string line = fc5::reload::set_measured(sig, mir(well, lh), mir(ax, lh));
                if (!line.empty()) m_ui_cmds += line + "\n";
                spdlog::info("[ReloadMag] weapon {}: the {}'s well: {} at {} m, axis {} (weapon controller's axes; a round {:.1f} cm "
                             "from its centre to its {}){}", sig, rp.muzzle ? "rocket launcher" : rp.brk ? "break action" : "revolver",
                             rp.muzzle ? "the seated rocket's tail" : "the rounds' noses in the open chambers", v3(well), v3(ax), s.rev_nose[sig] * 100.0f,
                             rp.muzzle ? "tail" : "nose", line.empty() ? ": not taken" : "");
            }
        }
    }
    }();
    [&]() __declspec(noinline) {
    if (quiet && known && s.base_frames >= kQuietFrames && s.settle >= kSettleFrames && !s.measured.contains(sig) && !fc5::reload::revolver_fed(sig)) {
        // (A weapon whose magazine model is known always takes the model's top: a well set by hand kept instead put the AR-C's 87 cm away
        // on 2 Nov 11:53, the first try's bad well read back as set by hand.)
        glm::vec3              U = s.axis_c.contains(sig) ? s.axis_c[sig] : up_c;
        std::vector<glm::vec3> pts;
        // The magazine's body (its part of the main mesh, when it has one), not the top round above its lips (the AR-C's 3036).
        const bool in_main = std::any_of(parts, parts + np, [&](const fc5::reload::MagPart& q) { return !q.prop && q.count == sig_mesh(sig); });
        for (int i = 0; i < np; ++i) {
            if (parts[i].prop || (in_main && parts[i].count != sig_mesh(sig))) continue;
            const auto mi = s.mesh.find({ sig, parts[i].count });
            if (mi == s.mesh.end() || !mi->second.ok) continue;
            const int nb = parts[i].rigid ? 1 : parts[i].nbones;
            for (int b = 0; b < nb; ++b) {
                const Key  k{ parts[i].count, parts[i].rigid ? kRigid : parts[i].bones[b] };
                const auto bb = s.base.find(k);
                const auto pi = mi->second.pts.find(parts[i].rigid ? 0u : parts[i].bones[b]);
                if (bb == s.base.end() || pi == mi->second.pts.end()) continue;
                for (const auto& p : pi->second) pts.push_back(point(bb->second.rc, p));
            }
        }
        // A pistol's magazine stands in its grip, tilted back from the gun's up (the 1911's about 20 deg): the well's axis is then the
        // magazine's own length, the main axis of its vertices (power iteration on their covariance), pointing up the gun.
        std::string axis_txt;
        if (pts.size() >= 20 && fc5::reload::mag_own_axis(sig)) {
            glm::vec3 c{ 0.0f };
            for (const auto& p : pts) c += p;
            c /= float(pts.size());
            glm::mat3 C{ 0.0f };
            for (const auto& p : pts) C += glm::outerProduct(p - c, p - c);
            glm::vec3 a = up_c;
            for (int it = 0; it < 40; ++it) {
                const glm::vec3 na = C * a;
                if (glm::length(na) < 1e-9f) break;
                a = glm::normalize(na);
            }
            if (glm::dot(a, up_c) < 0.0f) a = -a;
            const float deg = glm::degrees(std::acos(std::clamp(glm::dot(a, up_c), -1.0f, 1.0f)));
            if (deg <= 45.0f) {
                U             = a;
                s.axis_c[sig] = a;
                axis_txt      = fmt::format(", along the magazine's length ({:.0f} deg from the gun's up)", deg);
            } else {
                axis_txt = fmt::format(", the gun's up (the magazine's length {:.0f} deg from it: not taken)", deg);
            }
        }
        if (!pts.empty()) {
            s.measured.insert(sig);
            float top_d = -1e9f;
            for (const auto& p : pts) top_d = std::max(top_d, glm::dot(p, U));
            glm::vec3 sum{ 0.0f };
            int       n = 0;
            for (const auto& p : pts)
                if (glm::dot(p, U) > top_d - 0.01f) { sum += p; n++; }
            const glm::vec3 well = sum / float(std::max(1, n));
            s.well_c[sig]        = well;
            const std::string line = fc5::reload::set_measured(sig, mir(well, lh), mir(U, lh));
            if (!line.empty()) m_ui_cmds += line + "\n";   // saved, as the F11 buttons do
            // reloadhighlight (manualreload40): the magazine's box (its length along the well's axis, front to back along the gun) and the
            // well's: the magazine's top 4 cm, where it goes into the gun.
            {
                glm::vec3 F = -s.back_c - U * glm::dot(-s.back_c, U);
                F           = glm::length(F) > 1e-4f ? glm::normalize(F) : glm::vec3{ 0.0f, 0.0f, -1.0f };
                const glm::vec3 Rt = glm::normalize(glm::cross(U, -F));
                fc5::reload::HlBox mb = hl_box(pts, Rt, U, F, -1e9f, lh), wb = hl_box(pts, Rt, U, F, top_d - 0.04f, lh);
                fc5::reload::set_highlight(sig, fc5::reload::kHlMag, mb);
                fc5::reload::set_highlight(sig, fc5::reload::kHlWell, wb);
            }
            spdlog::info("[ReloadMag] weapon {}: the well measured on the game's magazine: its top {} m, axis {}{} (weapon controller's axes; {} points){}", sig, v3(well),
                         v3(U), axis_txt, pts.size(), line.empty() ? ": not taken" : "");
        }
    }
    }();

    // The charging handle's grab point on the model (once per weapon and session, a built-in or saved handle): the centre of its tabs (a saved
    // part: its centre), in the weapon controller's axes, saved through reloadgeom unless set by hand. Until 2 Nov 12:32 the AR-C's came from the
    // bolt (learnt from the game's reload from empty), so the blue ball sat at the chamber.
    [&]() __declspec(noinline) {
    if (fc5::reload::HandleParts hp{}; quiet && sig != 0 && s.base_frames >= kQuietFrames && s.settle >= kSettleFrames && !s.grab_measured.contains(sig) && fc5::reload::handle_parts(sig, hp)) {
        glm::vec3 sum{ 0.0f };
        int       n = 0;
        for (int i = 0; i < hp.ngrab; ++i) {
            const Key  k{ hp.count, hp.grab[i] };
            const auto bb = s.base.find(k);
            if (bb == s.base.end()) continue;
            sum += point(bb->second.rc, s.centre(sig, k));
            n++;
        }
        // A pistol's slide is pinched at its rear (the serrations), not at its middle over the chamber: the centre of its vertices in its
        // rear 2.5 cm along the gun's back.
        std::string rear_txt;
        if (hp.grab_rear && n == hp.ngrab && n > 0) {
            const auto mi = s.mesh.find({ sig, hp.count });
            std::vector<glm::vec3> gp;
            if (mi != s.mesh.end() && mi->second.ok)
                for (int i = 0; i < hp.ngrab; ++i) {
                    const auto bb = s.base.find({ hp.count, hp.grab[i] });
                    const auto pi = mi->second.pts.find(hp.grab[i]);
                    if (bb == s.base.end() || pi == mi->second.pts.end()) continue;
                    for (const auto& q : pi->second) gp.push_back(point(bb->second.rc, q));
                }
            float back_d = -1e9f;
            for (const auto& q : gp) back_d = std::max(back_d, glm::dot(q, s.back_c));
            glm::vec3 rs{ 0.0f };
            int       rn = 0;
            for (const auto& q : gp)
                if (glm::dot(q, s.back_c) > back_d - 0.025f) { rs += q; rn++; }
            if (rn >= 4) {
                const glm::vec3 mid = sum / float(n);
                sum      = rs / float(rn) * float(n);
                rear_txt = fmt::format(", the rear 2.5 cm of the slide ({} points, {:.1f} cm behind its centre)", rn, glm::dot(rs / float(rn) - mid, s.back_c) * 100.0f);
            }
        }
        // reloadhintglow2 (author 5 Nov 12:47, the AK's blue ball in the middle of the receiver): an AK's handle part is its whole bolt
        // carrier, centred under the dust cover; the hand takes its knob, the vertices furthest out on the gun's right (their outer 1.2 cm).
        if (hp.grab_right && n == hp.ngrab && n > 0) {
            const auto mi = s.mesh.find({ sig, hp.count });
            std::vector<glm::vec3> gp;
            if (mi != s.mesh.end() && mi->second.ok)
                for (int i = 0; i < hp.ngrab; ++i) {
                    const auto bb = s.base.find({ hp.count, hp.grab[i] });
                    const auto pi = mi->second.pts.find(hp.grab[i]);
                    if (bb == s.base.end() || pi == mi->second.pts.end()) continue;
                    for (const auto& q : pi->second) gp.push_back(point(bb->second.rc, q));
                }
            glm::vec3 Ur = up_c - s.back_c * glm::dot(up_c, s.back_c);
            Ur           = glm::length(Ur) > 1e-4f ? glm::normalize(Ur) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            const glm::vec3 Rr = glm::normalize(glm::cross(Ur, s.back_c));   // the gun's right (x right, y up, z back)
            float right_d = -1e9f;
            for (const auto& q : gp) right_d = std::max(right_d, glm::dot(q, Rr));
            glm::vec3 rs{ 0.0f };
            int       rn = 0;
            for (const auto& q : gp)
                if (glm::dot(q, Rr) > right_d - 0.012f) { rs += q; rn++; }
            if (rn >= 4) {
                const glm::vec3 mid = sum / float(n);
                sum      = rs / float(rn) * float(n);
                rear_txt = fmt::format(", the knob: its outer 1.2 cm on the right ({} points, {:.1f} cm right, {:.1f} cm ahead of its centre)", rn,
                                       glm::dot(rs / float(rn) - mid, Rr) * 100.0f, -glm::dot(rs / float(rn) - mid, s.back_c) * 100.0f);
            }
        }
        if (n == hp.ngrab && n > 0 && s.mesh.contains({ sig, hp.count })) {
            // reloadhighlight (manualreload40): a box round the handle's (slide's, pump's) parts.
            if (const auto mi = s.mesh.find({ sig, hp.count }); mi != s.mesh.end() && mi->second.ok) {
                std::vector<glm::vec3> hpts;
                for (int i = 0; i < hp.nbones; ++i) {
                    const Key  k{ hp.count, hp.bones[i] };
                    const auto bb = s.base.find(k);
                    const auto pi = mi->second.pts.find(mi->second.skinned ? hp.bones[i] : 0u);
                    if (bb == s.base.end() || pi == mi->second.pts.end()) continue;
                    for (const auto& q : pi->second) hpts.push_back(point(bb->second.rc, q));
                }
                glm::vec3 Uh = up_c - s.back_c * glm::dot(up_c, s.back_c);
                Uh           = glm::length(Uh) > 1e-4f ? glm::normalize(Uh) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                const glm::vec3 F  = -s.back_c;
                const glm::vec3 Rt = glm::normalize(glm::cross(Uh, -F));
                if (hpts.size() >= 8) fc5::reload::set_highlight(sig, fc5::reload::kHlHandle, hl_box(hpts, Rt, Uh, F, -1e9f, lh));
                // reloadrackzone (author 5 Nov 14:09: the A99's and MS16's rack zones wrong, the glow right): the parts' line along the gun's
                // back through their centre, front to rear, so the grip takes the handle anywhere on what glows.
                if (hpts.size() >= 8) {
                    glm::vec3 c{ 0.0f };
                    for (const auto& q : hpts) c += q;
                    c /= float(hpts.size());
                    float lo = 1e9f, hi = -1e9f;
                    for (const auto& q : hpts) {
                        const float d = glm::dot(q - c, s.back_c);
                        lo = std::min(lo, d);
                        hi = std::max(hi, d);
                    }
                    fc5::reload::set_handle_span(sig, mir(c + s.back_c * lo, lh), mir(c + s.back_c * hi, lh));
                    spdlog::info("[ReloadMag] weapon {}: reloadrackzone: the handle's parts' line from {} to {} (weapon controller's axes, {:.1f} cm long, {} points)", sig,
                                 v3(c + s.back_c * lo), v3(c + s.back_c * hi), (hi - lo) * 100.0f, hpts.size());
                }
            }
            s.grab_measured.insert(sig);
            const glm::vec3   p    = sum / float(n);
            const std::string line = fc5::reload::set_handle_point(sig, mir(p, lh));
            if (!line.empty()) m_ui_cmds += line + "\n";
            std::string parts_txt;
            for (int i = 0; i < hp.nbones; ++i) parts_txt += fmt::format("{}{}", i ? "+" : "", hp.bones[i]);
            if (hp.also_count) parts_txt += fmt::format(" and {}/{}", hp.also_count, hp.also_bone);
            spdlog::info("[ReloadMag] weapon {}: {} {} part(s) {}/{} moving with the pull; taken at {} (weapon controller's axes, the centre of part(s) {}{}){}", sig,
                         hp.slide ? "slide" : "charging handle", hp.saved ? "saved" : "built in", hp.count, parts_txt, v3(p),
                         hp.ngrab == 2 ? fmt::format("{}+{}", hp.grab[0], hp.grab[1]) : std::to_string(hp.grab[0]), rear_txt,
                         line.empty() ? " (the grab point kept: set by hand, or already there)" : "");
        }
    }
    }();

    // The charging handle's candidates (once per weapon and session): the main mesh's parts, not its body nor the magazine, by distance from
    // the handle point. With none saved nor built in, the nearest (within 7 cm) moves with the pull.
    [&]() __declspec(noinline) {
    if (quiet && sig != 0 && s.base_frames >= kQuietFrames && !s.handle_cands.contains(sig) && s.weapon_draw(sig, sig_mesh(sig))) {
        const fc5::reload::Geom g  = fc5::reload::geom(sig);
        const glm::vec3         hp = mir(g.handle, lh);
        const auto&             mm = s.mesh.at({ sig, sig_mesh(sig) });
        std::vector<std::pair<Key, glm::vec3>> c;
        if (mm.skinned)
            for (const auto& [b, n] : mm.n) {
                const Key k{ sig_mesh(sig), b };
                if (b == 0 || n < kMinVerts || n > 2000 || is_mag(k)) continue;
                const auto bb = s.base.find(k);
                if (bb == s.base.end()) continue;
                c.push_back({ k, point(bb->second.rc, mm.cent.at(b)) });
            }
        std::sort(c.begin(), c.end(), [&](const auto& a, const auto& b) { return glm::length(a.second - hp) < glm::length(b.second - hp); });
        std::string list;
        for (const auto& [k, p] : c) list += fmt::format(" {}:{:.0f}cm", key_text(k), glm::length(p - hp) * 100.0f);
        s.handle_cands[sig] = c;
        fc5::reload::HandleParts hk{};
        const bool               hl = fc5::reload::handle_parts(sig, hk);
        if (!hl && !c.empty() && glm::length(c[0].second - hp) < 0.07f) s.handle_guess[sig] = c[0].first;
        std::string what = "not found (no part within 7 cm)";
        if (hl) what = fmt::format("part {} ({})", key_text({ hk.count, hk.bones[0] }), hk.saved ? "saved" : "built in");
        else if (s.handle_guess.contains(sig)) what = fmt::format("part {} (the nearest, until its motion is seen)", key_text(s.handle_guess[sig]));
        spdlog::info("[ReloadMag] weapon {}: charging handle {}; the main mesh's parts from the handle point {} (controller axes):{}", sig, what, v3(hp),
                     list.empty() ? " none" : list);
    }
    // F11 "Try the next part as the charging handle": the next candidate, saved, the grab point moved onto it.
    if (const int next = fc5::reload::take_handle_next(); next > 0 && sig != 0) {
        const auto it = s.handle_cands.find(sig);
        if (it == s.handle_cands.end() || it->second.empty()) {
            spdlog::info("[ReloadMag] weapon {}: no parts to try as the charging handle yet (hold the gun still a moment)", sig);
        } else {
            uint32_t hc = 0, hb = 0;
            bool     hl = false;
            fc5::reload::handle_part(sig, hc, hb, hl);
            // From the part moving now (saved or guessed) to the next one (the first when none is).
            const Key cur_k = hl ? Key{ hc, hb } : s.handle_guess.contains(sig) ? s.handle_guess[sig] : Key{ 0, 0 };
            int       i     = s.handle_idx.contains(sig) ? s.handle_idx[sig] : -1;
            for (size_t j = 0; j < it->second.size(); ++j)
                if (it->second[j].first == cur_k) i = int(j);
            i                  = (i + next) % int(it->second.size());
            s.handle_idx[sig]  = i;
            const auto& [k, p] = it->second[size_t(i)];
            m_ui_cmds += fc5::reload::set_handle_part(sig, k.first, k.second) + "\n";
            const std::string gl = fc5::reload::set_handle_point(sig, mir(p, lh));
            if (!gl.empty()) m_ui_cmds += gl + "\n";
            s.handle_guess.erase(sig);
            spdlog::info("[ReloadMag] weapon {}: charging handle part {} tried ({} of {}), at {} (controller axes)", sig, key_text(k), i + 1, it->second.size(), v3(p));
        }
    }
    }();

    // The game's reload starting: learn the magazine (not learnt yet) and, from empty, the charging handle (none saved).
    const bool busy = in_hand && (fc5::reload::game_busy() || (fc5::reload::rec_on() && fc5::reload::button_reload()));   // reloadrec: the reload from its button too
    [&]() __declspec(noinline) {
    if (busy && !s.busy_was && fc5::reload::model_on() && !s.learning && sig != 0 && s.base_sig == sig && !s.base.empty()) {
        fc5::reload::HandleParts hk{};
        s.learn_mag    = (!fc5::reload::mag_learnt(sig) && !fc5::reload::mag_builtin(sig)) || fc5::reload::relearn_armed(sig, 1);   // the AR-C's is built in (and its 2 Nov learns never held); manualreload52: or asked again
        s.learn_handle = !fc5::reload::handle_parts(sig, hk) && fc5::reload::rack_needed();
        // manualreload16: a built-in magazine with no spare-magazine prop known still has the window run, so the draws the game's hand brings
        // in are logged (to be built in: hidden in the game's reload, and fingergame learns the grip from them).
        bool has_prop = false;
        for (int i = 0; i < np; ++i) has_prop = has_prop || parts[i].prop;
        s.learn_props = !s.learn_mag && known && !has_prop;
        // manualreload17: every game reload is watched, for where the game's left hand holds the spare magazine (and the handle).
        {
            s.learning          = true;
            s.learn_sig         = sig;
            s.learn_t0          = now;
            s.learn_frames      = 0;
            s.learn_base        = s.base;
            s.learn_base_counts = s.base_counts;
            s.maxd.clear();
            s.first_dir.clear();
            s.back_max.clear();
            s.back_perp.clear();
            s.appeared.clear();
            s.snd_track.clear();
            s.near_hand.clear();
            s.hold_pts.clear();
            s.grab_pts.clear();
            s.grab_far  = 0;
            s.grab_near = 1e9f;
            s.hold_count = 0;
            s.own_pts.clear();
            s.palm_ok = s.palm_none = s.palm_old = 0;
            s.near_new = s.near_own = 1e9f;
            if (fc5::reload::rec_on()) {
                s.rec_n++;
                s.rec.push_back(fmt::format("W,{},{},{},G", sig, s.rec_n, rec_clock()));
            }
            spdlog::info("[ReloadMag] weapon {}: the game's reload started: learning its {} from {} parts of {} draws, against the gun's body", sig,
                         s.learn_mag && s.learn_handle ? "magazine and charging handle"
                         : s.learn_mag                 ? "magazine"
                         : s.learn_handle              ? (s.learn_props ? "charging handle and the props its hand brings in" : "charging handle")
                         : s.learn_props               ? "props its hand brings in (the magazine built in)"
                                                       : "hand's hold (the magazine and its props known)",
                         s.base.size(), s.base_counts.size());
        }
    }
    }();
    s.busy_was = busy;
    // reloadrec: the manual steps as the mod draws them, each frame from the magazine dropped to the slide let go: against the gun controller. (A pump's, lever's, revolver's and break action's too: their shell
    // or round in the hand, their pump held.)
    [&]() __declspec(noinline) {
    if (fc5::reload::rec_on() && !s.learning) {
        fc5::reload::View rv{};
        const bool act = in_hand && fc5::reload::view(rv) && (rv.hand_mag || rv.engaged || rv.racking || (!rv.revolver && !rv.tube && !rv.mag_in_gun));
        if (act && !s.rec_vr) {
            s.rec_vr    = true;
            s.rec_vr_t0 = now;
            s.rec_n++;
            s.rec.push_back(fmt::format("W,{},{},{},V", sig, s.rec_n, rec_clock()));
        }
        if (!act && s.rec_vr) {
            s.rec_vr = false;
            rec_flush(s.rec);
        }
        glm::mat4 pw{ 1.0f }, cfp{ 1.0f }, cu{ 1.0f };
        glm::vec3 rest{ 0.0f };
        int       pf = -1000;
        if (act && place && fc5::fingers::game_palm(lh ? 0 : 1, pw, cfp, pf) && std::abs(pf - s.frame) <= 1 && fc5::fingers::free_ctrl(lh ? 0 : 1, cu, rest) && s.rec.size() < 4000) {
            const float     wsl = std::max(m_world_scale.load(), 1e-3f);
            const glm::mat4 ci  = glm::inverse(s.ctrl);
            std::string     mp  = ",,,,,,,";
            if (s.rec_mag_frame >= s.frame - 1) mp = rec_pose(s.rec_mag, s.centre(sig, s.rec_mag_k), wsl);
            s.rec.push_back(fmt::format("V,{},{},{:.3f},{},{:.3f},{:.4f}", sig, s.rec_n, std::chrono::duration<float>(now - s.rec_vr_t0).count(),
                                        rv.racking ? "R" : rv.engaged ? "E" : rv.hand_mag ? "H" : "O", rv.insert, rv.racking ? rv.rack * rv.rack_len : 0.0f) +
                            rec_pose(ci * cu, glm::vec3{ 0.0f }, wsl) + rec_pose(ci * cu * cfp, rest, wsl) + mp + "," + rec_bends(lh ? 0 : 1, true));
        }
    }
    }();
    if (s.learning) {
        if (sig != s.learn_sig || !in_hand) {
            s.learning = false;
            spdlog::info("[ReloadMag] learning dropped: the weapon left the hands");
        } else {
            const glm::vec3 bb     = s.c2b * s.back_c;
            const glm::vec3 back_b = glm::length(bb) > 1e-6f ? glm::normalize(bb) : glm::vec3{ 0.0f, 0.0f, 1.0f };
            [&]() __declspec(noinline) {
            if (body) {
                s.learn_frames++;
                for (const auto& [k, T] : s.cur) {
                    const auto b = s.learn_base.find(k);
                    if (b == s.learn_base.end()) continue;
                    const glm::vec3 c  = s.centre(sig, k);
                    const glm::vec3 d  = point(bi * T, c) - point(b->second.r, c);   // body space
                    const float     dl = glm::length(d);
                    float&          mx = s.maxd[k];
                    mx                 = std::max(mx, dl);
                    if (dl > 0.03f && !s.first_dir.contains(k)) s.first_dir[k] = d / dl;
                    const float along = glm::dot(d, back_b);
                    if (along > s.back_max[k]) {
                        s.back_max[k]  = along;
                        s.back_perp[k] = glm::length(d - back_b * along);
                    }
                }
                if (fc5::reload::sound_learn_on() && s.snd_track.size() < 2000) {
                    std::array<float, 4> row{ std::chrono::duration<float>(now - s.learn_t0).count(), -1.0f, -1.0f, -1.0f };
                    for (int i = 0; i < np; ++i) {
                        if (parts[i].prop) continue;
                        const Key  k{ parts[i].count, parts[i].rigid ? kRigid : parts[i].bones[0] };
                        const auto b = s.learn_base.find(k);
                        if (b == s.learn_base.end()) break;
                        const glm::vec3 c    = s.centre(sig, k);
                        const glm::vec3 seat = point(b->second.r, c);
                        if (const auto T = s.cur.find(k); T != s.cur.end()) row[1] = glm::length(point(bi * T->second, c) - seat);
                        for (const auto& [k2, T2] : s.cur) {
                            bool is_prop = false;
                            for (int j = 0; j < np; ++j) is_prop = is_prop || (parts[j].prop && parts[j].count == k2.first);
                            if (!is_prop && (k2.first == sig_mesh(sig) || s.learn_base_counts.contains(k2.first) || !s.weapon_draw(sig, k2.first))) continue;
                            const float dd = glm::length(point(bi * T2, s.centre(sig, k2)) - seat);
                            if (row[2] < 0.0f || dd < row[2]) row[2] = dd;
                        }
                        break;
                    }
                    if (fc5::reload::HandleParts hk{}; fc5::reload::handle_parts(sig, hk) && hk.nbones > 0) {
                        const Key  k{ hk.count, hk.bones[0] };
                        const auto T = s.cur.find(k);
                        const auto b = s.learn_base.find(k);
                        if (T != s.cur.end() && b != s.learn_base.end()) {
                            const glm::vec3 c = s.centre(sig, k);
                            row[3]            = std::max(0.0f, glm::dot(point(bi * T->second, c) - point(b->second.r, c), back_b));
                        }
                    }
                    s.snd_track.push_back(row);
                }
            }
            }();
            for (const uint32_t c : s.cur_counts)
                if (!s.learn_base_counts.contains(c)) s.appeared[c]++;
            // manualreload17 (reloadgamehold): the game's left hand this frame (fc5::fingers::game_palm: its palm in the game world, and the
            // mod's palm -> free controller transform), so a part P the game's hand holds sits at ctrl_from_palm * palm_world^-1 * P.
            glm::mat4   palm_w{ 1.0f }, c_from_p{ 1.0f };
            int         pf  = -1000;
            const float wsl = std::max(m_world_scale.load(), 1e-3f);
            const bool  palm = body && fc5::fingers::game_palm(lh ? 0 : 1, palm_w, c_from_p, pf);
            if (body) (palm ? (std::abs(pf - s.frame) <= 1 ? s.palm_ok : s.palm_old) : s.palm_none)++;
            if (palm && std::abs(pf - s.frame) <= 1) {
                // reloadrec: this frame of the game's reload, against the gun's body (it moves in the animation) in the gun controller's axes.
                [&]() __declspec(noinline) {
                if (fc5::reload::rec_on()) {
                    const glm::mat4 gun = s.b2c4 * bi;
                    glm::mat4       cu{ 1.0f };
                    glm::vec3       rest{ 0.0f };
                    fc5::fingers::free_ctrl(lh ? 0 : 1, cu, rest);
                    float     seat  = -1.0f, slide = -1.0f;
                    std::string mp  = ",,,,,,,";
                    for (int i = 0; i < np; ++i) {
                        if (parts[i].prop) continue;
                        const Key  k{ parts[i].count, parts[i].rigid ? kRigid : parts[i].bones[0] };
                        const auto T = s.cur.find(k);
                        const auto b = s.learn_base.find(k);
                        if (T != s.cur.end() && b != s.learn_base.end()) {
                            const glm::vec3 c = s.centre(sig, k);
                            seat              = glm::length(point(bi * T->second, c) - point(b->second.r, c)) / wsl;
                            mp                = rec_pose(gun * T->second, c, wsl);
                        }
                        break;
                    }
                    if (fc5::reload::HandleParts hk{}; fc5::reload::handle_parts(sig, hk) && hk.nbones > 0) {
                        const Key  k{ hk.count, hk.bones[0] };
                        const auto T = s.cur.find(k);
                        const auto b = s.learn_base.find(k);
                        if (T != s.cur.end() && b != s.learn_base.end()) {
                            const glm::vec3 c = s.centre(sig, k);
                            slide             = std::max(0.0f, glm::dot(point(bi * T->second, c) - point(b->second.r, c), back_b)) / wsl;
                        }
                    }
                    // The spare magazine the game's hand brings in (a rifle's: a draw of its own, the gun's magazine part goes away): P rows, after their frame's G row is written below.
                    std::string prow;
                    {
                        int pbest = 0;
                        for (const auto& [k, T] : s.cur) {
                            bool is_prop = false;
                            for (int i = 0; i < np; ++i) is_prop = is_prop || (parts[i].prop && parts[i].count == k.first);
                            if (!is_prop) continue;
                            const auto mi = s.mesh.find({ sig, k.first });
                            if (mi == s.mesh.end() || !mi->second.ok) continue;
                            const auto ni = mi->second.n.find(k.second == kRigid ? 0u : k.second);
                            const int  nv = ni != mi->second.n.end() ? ni->second : 0;
                            if (nv > pbest) {
                                pbest = nv;
                                prow  = fmt::format("P,{},{},{:.3f},{},{}", sig, s.rec_n, std::chrono::duration<float>(now - s.learn_t0).count(), k.first, nv) + rec_pose(gun * T, s.centre(sig, k), wsl);
                            }
                        }
                    }
                    if (s.rec.size() < 8000)
                        s.rec.push_back(fmt::format("G,{},{},{:.3f},-,0,{:.4f}", sig, s.rec_n, std::chrono::duration<float>(now - s.learn_t0).count(), slide) +
                                        rec_pose(gun * palm_w * glm::inverse(c_from_p), glm::vec3{ 0.0f }, wsl) + rec_pose(gun * palm_w, rest, wsl) + mp +
                                        fmt::format(",{:.4f}", seat) + rec_bends(lh ? 0 : 1, false) + rec_turns(lh ? 0 : 1));
                    if (!prow.empty() && s.rec.size() < 8000) s.rec.push_back(prow);
                }
                }();
                [&]() __declspec(noinline) {
                const glm::mat4 to_c = c_from_p * glm::inverse(palm_w);   // game world -> the free controller's axes (drawn units)
                for (const auto& [k, T] : s.cur) {
                    const float d = glm::length(point(to_c * T, s.centre(sig, k)) / wsl);
                    if (d > 0.15f) continue;
                    auto& nh  = s.near_hand.try_emplace(k, 0, d).first->second;
                    nh.first++;
                    nh.second = std::min(nh.second, d);
                }
                // The spare magazine: the largest draw that was not on the gun before the reload (or a known prop: manualreload19 reads their
                // place while learning, hidden as before), within 25 cm of the controller.
                int       best_n = 0;
                glm::vec3 best_p{ 0.0f };
                uint32_t  best_c = 0;
                for (const auto& [k, T] : s.cur) {
                    if (k.first == sig_mesh(sig)) continue;
                    bool is_prop = false;
                    for (int i = 0; i < np; ++i) is_prop = is_prop || (parts[i].prop && parts[i].count == k.first);
                    if (!is_prop && s.learn_base_counts.contains(k.first)) continue;
                    const auto mi = s.mesh.find({ sig, k.first });
                    if (mi == s.mesh.end() || !mi->second.ok || (!is_prop && mi->second.max_bone > fc5::reload::kMaxWeaponBone)) continue;
                    const auto      ni = mi->second.n.find(k.second == kRigid ? 0u : k.second);
                    const int       nv = ni != mi->second.n.end() ? ni->second : 0;
                    const glm::vec3 p  = point(to_c * T, s.centre(sig, k)) / wsl;
                    s.near_new         = std::min(s.near_new, glm::length(p));
                    if (glm::length(p) < 0.25f && nv > best_n) {
                        best_n = nv;
                        best_p = p;
                        best_c = k.first;
                    }
                }
                if (best_n > 0 && (s.hold_count == 0 || s.hold_count == best_c)) {
                    s.hold_count = best_c;
                    s.hold_pts.push_back(mir(best_p, lh));
                }
                // The gun's own magazine part 5 cm or more out of its seat, within 25 cm of the controller: the game's hand has it.
                for (int i = 0; i < np; ++i) {
                    if (parts[i].prop) continue;
                    const Key  k{ parts[i].count, parts[i].rigid ? kRigid : parts[i].bones[0] };
                    const auto T = s.cur.find(k);
                    const auto b = s.learn_base.find(k);
                    if (T == s.cur.end() || b == s.learn_base.end()) continue;
                    const glm::vec3 c = s.centre(sig, k);
                    if (glm::length(point(bi * T->second, c) - point(b->second.r, c)) >= 0.05f) {
                        const glm::vec3 p = point(to_c * T->second, c) / wsl;
                        s.near_own        = std::min(s.near_own, glm::length(p));
                        if (glm::length(p) < 0.25f) s.own_pts.push_back(mir(p, lh));
                    }
                    break;
                }
                // The charging handle pulled back (a reload from empty): the fingers' handle grip (fc5::fingers::game_holds), and the free
                // controller's place on the handle as it sits at rest.
                if (fc5::reload::HandleParts hk{}; fc5::reload::handle_parts(sig, hk))
                    for (int i = 0; i < hk.nbones; ++i) {
                        const Key  k{ hk.count, hk.bones[i] };
                        const auto T = s.cur.find(k);
                        const auto b = s.learn_base.find(k);
                        if (T == s.cur.end() || b == s.learn_base.end()) continue;
                        const glm::vec3 c     = s.centre(sig, k);
                        const float     along = glm::dot(point(bi * T->second, c) - point(b->second.r, c), back_b);
                        if (along < 0.015f) continue;
                        const glm::vec3 cw = point(palm_w * glm::inverse(c_from_p), glm::vec3{ 0.0f });   // the free controller, game world
                        const glm::vec3 gp = mir(point(b->second.rc * glm::inverse(T->second), cw) / wsl, lh);
                        // manualreload53: only the frames with the hand at the handle. An empty pistol's slide stays locked back through the whole
                        // reload (the 4 Nov handgun run: 184-228 frames "pulled", most with the hand at the magazine, averaged 19-27 cm away and refused).
                        const float dh = glm::length(gp - fc5::reload::geom(sig).handle);
                        s.grab_near    = std::min(s.grab_near, dh);
                        if (dh > kGrabNear) {
                            s.grab_far++;
                            break;
                        }
                        fc5::fingers::game_holds(fc5::fingers::kHandle, s.frame);
                        s.grab_pts.push_back(gp);
                        break;
                    }
                }();
            }
            [&]() __declspec(noinline) {
            if (const float el = std::chrono::duration<float>(now - s.learn_t0).count(); el >= kWindowSecs && (!(busy || fc5::reload::game_pending()) || el >= kWindowMax)) {
                s.learning = false;
                if (fc5::reload::rec_on()) {
                    for (const auto& h : fc5::gamesound::heard(s.learn_t0)) s.rec.push_back(fmt::format("S,{},{},{:.3f},{}", sig, s.rec_n, h.t, h.id));
                    rec_flush(s.rec);
                }
                {
                    std::vector<std::pair<Key, std::pair<int, float>>> nh(s.near_hand.begin(), s.near_hand.end());
                    std::sort(nh.begin(), nh.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                    std::string t;
                    for (size_t i = 0; i < nh.size() && i < 10; ++i)
                        t += fmt::format(" {}{}:{}@{:.0f}cm", key_text(nh[i].first), s.learn_base.contains(nh[i].first) ? "" : "(new)", nh[i].second.first, nh[i].second.second * 100.0f);
                    spdlog::info("[ReloadMag] weapon {}: parts within 15 cm of the game's left hand in its reload ({} frames; part:frames@nearest):{}", sig, s.learn_frames,
                                 t.empty() ? " none" : t);
                }
                if (fc5::reload::sound_learn_on()) {
                    const std::string line = learn_reload_sounds(sig, s.snd_track, fc5::gamesound::heard(s.learn_t0));
                    if (!line.empty()) m_ui_cmds += line + "\n";
                }
                // Only the weapon's own parts (its draws' bones low), fixed to its body while quiet, and not the body itself.
                auto weapon_part = [&](const Key& k) {
                    if (!s.weapon_draw(sig, k.first)) return false;
                    if (k.first == sig_mesh(sig) && k.second == 0) return false;
                    const auto d = s.drift.find(k);
                    return d == s.drift.end() || d->second < kAttached;
                };
                std::string all;
                for (const auto& [k, d] : s.maxd)
                    if (d >= 0.02f) all += fmt::format(" {}:{:.0f}cm{}", key_text(k), d * 100.0f, weapon_part(k) ? "" : "(not the weapon's)");
                [&]() __declspec(noinline) {
                if (s.learn_mag) {
                    Key   best{ 0, 0 };
                    float bd = 0.0f;
                    // A part of the gun's own mesh that left by kMainMagMove first: a separate draw can fly further (the MP40's 9 mm round 1224
                    // went 129 cm, its magazine 40362/6 44 cm, reload3 run): then the furthest of any draw, as before (a magazine of its own).
                    for (const auto& [k, d] : s.maxd)
                        if (k.first == sig_mesh(sig) && d >= kMainMagMove && d > bd && s.first_dir.contains(k) && weapon_part(k)) { bd = d; best = k; }
                    if (bd == 0.0f)
                        for (const auto& [k, d] : s.maxd)
                            if (d > bd && s.first_dir.contains(k) && weapon_part(k)) { bd = d; best = k; }
                    std::string props;
                    for (const auto& [c, f] : s.appeared) props += fmt::format(" {}:{}", c, f);
                    if (bd < kMinMove) {
                        spdlog::info("[ReloadMag] weapon {}: no part of the weapon moved {:.0f} cm from its body in its reload ({} frames; moved:{}; appeared:{}): magazine not learnt{}", sig,
                                     kMinMove * 100.0f, s.learn_frames, all.empty() ? " none" : all, props.empty() ? " none" : props,
                                     np > 0 ? " (the built-in one stays)" : ", the drawn magazine stays");
                    } else {
                        const glm::vec3 dir = s.first_dir[best];
                        std::map<uint32_t, std::vector<uint32_t>> by_count;
                        for (const auto& [k, d] : s.maxd) {
                            if (d < std::max(kMinMove, 0.5f * bd) || !weapon_part(k)) continue;
                            const auto fd = s.first_dir.find(k);
                            if (fd == s.first_dir.end() || glm::dot(fd->second, dir) < 0.82f) continue;   // within 35 deg
                            by_count[k.first].push_back(k.second);
                        }
                        fc5::reload::MagPart lp[8];
                        int                  n = 0;
                        for (const auto& [c, bones] : by_count) {
                            if (n >= 6) break;
                            fc5::reload::MagPart p{};
                            p.count = c;
                            if (std::find(bones.begin(), bones.end(), kRigid) != bones.end()) p.rigid = true;
                            else for (const uint32_t b : bones) if (p.nbones < 4) p.bones[p.nbones++] = b;
                            lp[n++] = p;
                        }
                        for (const auto& [c, f] : s.appeared) {
                            if (n >= 8) break;
                            if (f < kPropFrames || c == sig_mesh(sig) || c == 65535u || by_count.contains(c)) continue;
                            fc5::reload::MagPart p{};
                            p.count = c;
                            p.prop  = true;
                            lp[n++] = p;
                        }
                        // A built-in magazine (the AR-C's bone 1) must be among them, else the built-in one stays.
                        bool keep = true;
                        for (int i = 0; i < np; ++i) {
                            if (parts[i].prop || parts[i].rigid) continue;
                            const auto bc = by_count.find(parts[i].count);
                            if (bc == by_count.end() || std::find(bc->second.begin(), bc->second.end(), parts[i].bones[0]) == bc->second.end()) keep = false;
                        }
                        if (!keep) {
                            spdlog::info("[ReloadMag] weapon {}: its reload moved {} furthest ({:.0f} cm; moved:{}), not the built-in magazine: the built-in one stays", sig, key_text(best),
                                         bd * 100.0f, all);
                        } else {
                            const std::string line = fc5::reload::learn_mag(sig, lp, n);
                            m_ui_cmds += line + "\n";
                            // The well's axis: the magazine's first motion out of the gun, reversed; taken if within 35 deg of the gun's up.
                            glm::vec3 U = s.b2c * -dir;
                            U           = glm::length(U) > 1e-6f ? glm::normalize(U) : up_c;
                            if (glm::dot(U, up_c) > 0.82f) {
                                s.axis_c[sig] = U;
                                s.measured.erase(sig);   // the well measured again along it
                            }
                            spdlog::info("[ReloadMag] weapon {}: its magazine learnt from the reload ({} frames; furthest {} {:.0f} cm from the body, first moving {} (controller axes){}; "
                                         "moved:{}; appeared:{}): {}", sig, s.learn_frames, key_text(best), bd * 100.0f, v3(-U),
                                         s.axis_c.contains(sig) ? ", taken as the well's axis" : ", not along the gun's up: the well's axis stays the gun's up", all,
                                         props.empty() ? " none" : props, line);
                        }
                    }
                }
                }();
                // manualreload17: the game's hold of the spare magazine, as reloadhandmag (the magazine's top in the left controller's axes).
                // Its steady frames (held: under 3 mm from the frame before, against the palm); their median centre, less the drawn magazine's
                // centre below its top (the gun's magazine part as it sits on the gun, against the measured well).
                [&]() __declspec(noinline) {
                {
                    auto steady = [](const std::vector<glm::vec3>& pts) {
                        std::vector<glm::vec3> r;
                        for (size_t i = 1; i < pts.size(); ++i)
                            if (glm::length(pts[i] - pts[i - 1]) < 0.003f) r.push_back(pts[i]);
                        return r;
                    };
                    // A separate draw (the spare magazine prop) first; else the gun's own magazine part in the hand (manualreload19).
                    // manualreload27: the gun's own part needs 5 steady frames (the AKs' hand has it 10 frames, 5 or 6 of them steady: reload5).
                    std::vector<glm::vec3> st      = steady(s.hold_pts);
                    const bool             use_own = st.size() < 8 && steady(s.own_pts).size() >= kOwnSteady;
                    if (use_own) {
                        st           = steady(s.own_pts);
                        s.hold_count = sig_mesh(sig);
                    }
                    glm::vec3 mag_c{ 0.0f };
                    bool      mag_ok = false;
                    for (int i = 0; i < np && !mag_ok; ++i) {
                        if (parts[i].prop) continue;
                        const Key  k{ parts[i].count, parts[i].rigid ? kRigid : parts[i].bones[0] };
                        const auto b = s.learn_base.find(k);
                        if (b == s.learn_base.end() || !s.well_c.contains(sig)) continue;
                        mag_c  = mir((point(b->second.rc, s.centre(sig, k)) - s.well_c[sig]) / std::max(m_world_scale.load(), 1e-3f), lh);
                        mag_ok = true;
                    }
                    if (st.size() >= (use_own ? kOwnSteady : size_t{ 8 }) && mag_ok) {
                        auto med = [&](int a) {
                            std::vector<float> vals;
                            for (const auto& q : st) vals.push_back(q[a]);
                            std::nth_element(vals.begin(), vals.begin() + long(vals.size() / 2), vals.end());
                            return vals[vals.size() / 2];
                        };
                        const glm::vec3   held{ med(0), med(1), med(2) };
                        const glm::vec3   top   = held - mag_c;
                        const bool        ok    = fc5::reload::hold_plausible(top);   // manualreload28: outside the hand's box, not kept
                        const std::string hline = ok ? fc5::reload::set_hold_mag(sig, top) : std::string{};
                        if (!hline.empty()) m_ui_cmds += hline + "\n";
                        spdlog::info("[ReloadMag] weapon {}: the game's hand holds its {} (draw {}) at {} (left controller's axes, m; {} steady frames of {}): "
                                     "the magazine's top at {}{}", sig, use_own ? "own magazine part" : "spare magazine", s.hold_count, v3(held), st.size(),
                                     use_own ? s.own_pts.size() : s.hold_pts.size(), v3(top),
                                     !ok                       ? " (outside the hand: not kept)"
                                     : fc5::reload::game_hold() ? " (reloadgamehold on: used)" : " (kept; used with reloadgamehold on)");
                    } else {
                        auto cm = [](float d) { return d < 1e8f ? fmt::format("{:.0f} cm", d * 100.0f) : std::string{ "none" }; };
                        spdlog::info("[ReloadMag] weapon {}: the game's hand hold not learnt ({} frames with a spare magazine in its hand, {} steady; {} with its own magazine "
                                     "part, {} steady{}; the game's palm read in {} frames, none in {}, a frame old in {}; the nearest new draw to the controller {}, the "
                                     "magazine part out of its seat {})", sig, s.hold_pts.size(), steady(s.hold_pts).size(), s.own_pts.size(), steady(s.own_pts).size(),
                                     mag_ok ? "" : "; the gun's magazine or its well not measured yet", s.palm_ok, s.palm_none, s.palm_old, cm(s.near_new), cm(s.near_own));
                    }
                    if (s.grab_pts.empty() && s.grab_far > 0)
                        spdlog::info("[ReloadMag] weapon {}: the handle back in {} frames, the game's hand never within {:.0f} cm of it (nearest {:.1f} cm): no grip learnt",
                                     sig, s.grab_far, kGrabNear * 100.0f, s.grab_near * 100.0f);
                    if (!s.grab_pts.empty()) {
                        glm::vec3 g{ 0.0f };
                        for (const auto& q : s.grab_pts) g += q;
                        g /= float(s.grab_pts.size());
                        const fc5::reload::Geom gm = fc5::reload::geom(sig);
                        // reloadhandgrab (manualreload44): kept and saved (reloadhandgrabw) from 10 frames, within 12 cm of the handle point.
                        const std::string gl = s.grab_pts.size() >= 10 ? fc5::reload::set_grab_hold(sig, g, gm.handle) : std::string{};
                        if (!gl.empty()) m_ui_cmds += gl + "\n";
                        spdlog::info("[ReloadMag] weapon {}: the game's hand on the charging handle ({} frames pulled): the left controller at {} (the gun controller's axes, "
                                     "the handle at rest); the grab point now {}, {:.1f} cm away{}", sig, s.grab_pts.size(), v3(g), v3(gm.handle),
                                     glm::length(g - gm.handle) * 100.0f, !gl.empty() ? " (kept: reloadhandgrabw)" : " (not kept: under 10 frames, over 12 cm, or the same)");
                    }
                }
                }();
                if (s.learn_props) {
                    std::string pl;
                    for (const auto& [c, f] : s.appeared)
                        if (f >= kPropFrames && c != sig_mesh(sig) && c != 65535u) pl += fmt::format(" {}:{}", c, f);
                    spdlog::info("[ReloadMag] weapon {}: its game reload brought in the draws{} (index count:frames of {}; built in, these are the props: hidden in the "
                                 "game's reload, the grip fingergame learns)", sig, pl.empty() ? " none" : pl, s.learn_frames);
                }
                [&]() __declspec(noinline) {
                if (s.learn_handle) {
                    // The charging handle: a weapon part (not the magazine) pulled back along the gun 2.5 to 15 cm, mostly along that line.
                    Key         best{ 0, 0 };
                    float       bd = 0.0f;
                    std::string list;
                    for (const auto& [k, a] : s.back_max) {
                        if (a < 0.01f || !weapon_part(k)) continue;
                        const bool  mag  = is_mag(k);
                        const float perp = s.back_perp[k];
                        list += fmt::format(" {}:{:.1f}cm back,{:.1f}cm off{}", key_text(k), a * 100.0f, perp * 100.0f, mag ? "(magazine)" : "");
                        if (mag || a < 0.025f || a > 0.15f || perp > 0.5f * a) continue;
                        if (a > bd) { bd = a; best = k; }
                    }
                    if (bd > 0.0f) {
                        m_ui_cmds += fc5::reload::set_handle_part(sig, best.first, best.second) + "\n";
                        if (const auto b = s.base.find(best); b != s.base.end()) {
                            const glm::vec3   p  = point(b->second.rc, s.centre(sig, best));
                            const std::string gl = fc5::reload::set_handle_point(sig, mir(p, lh));
                            if (!gl.empty()) m_ui_cmds += gl + "\n";
                        }
                        s.handle_guess.erase(sig);
                        spdlog::info("[ReloadMag] weapon {}: its charging handle learnt from the reload from empty: part {} ({:.1f} cm back); parts moving back:{}", sig, key_text(best),
                                     bd * 100.0f, list.empty() ? " none" : list);
                    } else {
                        spdlog::info("[ReloadMag] weapon {}: no part pulled back 2.5-15 cm along the gun in its reload from empty (parts moving back:{}): the charging handle stays {}", sig,
                                     list.empty() ? " none" : list, s.handle_guess.contains(sig) ? "the part nearest the handle point" : "the drawn bar");
                    }
                }
                }();
            }
            }();
        }
    }
    s.cur.clear();
    s.cur_counts.clear();
}

// A non-glove weapon-pass draw with a weapon of reloadweapons in hand (reloadmanual on). True: not to be drawn.
bool FarCry5Stereo::reload_mag_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_rm) m_rm = new ReloadMagState{};
    auto&          s     = *m_rm;
    const int      frame = m_frame.load();
    const uint32_t sig   = fc5::reload::reload_sig(m_weapon_sig.load());   // reloadsamegun
    if (frame != s.frame) {
        if (s.frame >= 0) reload_mag_frame(sig);
        s.frame = frame;
    }
    fc5::reloadhint::begin_draw(count, start, base, frame);   // reloadhintstyle glow: this draw's mask draws, if any, queued below
    m_hint_queued = false;
    if (!fc5::reload::in_hand() || in_vehicle() || flat_screen_on() || m_cine_on || m_mo.slung) return false;
    m_keep_bones      = true;   // the palettes' CPU copies (note_object_constants)
    const auto& hm    = m_hand_map;
    const bool  fresh = frame - hm.frame <= 1 && m_mo.valid && m_mo.ctrl_valid && !hm.empty;
    const bool  lh    = m_left_handed.load();
    s.cur_counts.insert(count);
    if (!fresh && s.diag_old_frame != frame) {
        s.diag_old_frame = frame;
        s.diag_old++;
    }
    if (!fresh || !fc5::reload::model_on()) return false;

    // This pass's placement: game world -> drawn world, and the weapon controller there (its raw axes: grippitch undone). Taken at each draw:
    // a frame has two weapon passes (one per game call of the pair), each with its own camera; until 2 Nov 13:52 the first pass's was kept
    // for the second, so whatever the mod placed in the world (the hip's and the dropped magazine) was off by the camera's move between
    // them in one eye, and shook.
    {
        const glm::mat3 L  = hm.w_rot * hm.rot;
        s.w2d              = affine(L, hm.w_pos - hm.w_rot * hm.grip - L * hm.cam);
        s.d2w              = glm::inverse(s.w2d);
        const glm::mat3 cr = m_mo.ctrl_rot_world * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-m_grip_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
        s.ctrl             = affine(cr, m_mo.ctrl_pos_world);
        const glm::vec3 b  = glm::transpose(cr) * (hm.w_rot * glm::transpose(hm.cr) * glm::vec3{ 0.0f, 0.0f, 1.0f });   // steadied q's z: the gun's back
        s.back_c           = glm::length(b) > 1e-4f ? glm::normalize(b) : glm::vec3{ 0.0f, 0.0f, 1.0f };
        s.place_ok         = true;
        s.place_frame      = frame;
    }

    fc5::reload::MagPart parts[8];
    int                  np    = 0;
    const bool           known = fc5::reload::mag_parts(sig, parts, 8, np);
    const fc5::reload::MagPart* part = nullptr;
    for (int i = 0; i < np; ++i) if (parts[i].count == count) part = &parts[i];
    // The game's prop magazine (its left hand's, during its reload): never shown while reloading by hand. manualreload19: while the game's
    // reload is learnt from, its place is read first (below), for where the game's hand holds it (reloadgamehold); it was never read before,
    // so no hold was learnt for the AKs, the AR-Cs or the .308 in reload4.
    const bool prop_draw = part && part->prop;
    if (prop_draw && !s.learning) {
        s.hidden++;
        return true;
    }
    const bool main_draw = count == sig_mesh(sig);
    // The charging handle's parts: saved, built in (the AR-C: 7, its tabs 8 and 9, the bolt 6), else the part nearest the handle point.
    fc5::reload::HandleParts hp{};
    const bool               hknown = fc5::reload::handle_parts(sig, hp);
    if (!hknown) {
        if (const auto hg = s.handle_guess.find(sig); hg != s.handle_guess.end()) {
            hp.count    = hg->second.first;
            hp.bones[0] = hg->second.second;
            hp.nbones   = 1;
        }
    }
    const bool handle_here = hp.count != 0 && hp.count == count && hp.nbones > 0;
    const bool also_here   = hp.also_count != 0 && hp.also_count == count && !handle_here;   // a pistol's slide sights (their own small draw)
    // A revolver's cylinder: the gun's main draw (crane, drum, extractor) and its rounds' draw.
    fc5::reload::Revolver rp{};
    const bool rev_here = fc5::reload::revolver_parts(sig, rp) && (count == rp.main || count == rp.rounds);
    fc5::reload::Belt blt{};
    const bool belt_here = fc5::reload::belt_parts(sig, blt) && count == blt.main;
    // Looked at: the main mesh always (the body), the magazine's and the handle's draws, and every draw while something is still to learn
    // (not for the AR-C, whose magazine and handle are built in: its other draws, muzzle flashes included, go by untouched).
    const bool want = main_draw || part != nullptr || handle_here || also_here || !(fc5::reload::mag_learnt(sig) || fc5::reload::mag_builtin(sig)) || !hknown || fc5::reload::scanning();
    if (!want) return false;

    ID3D11Buffer* cb[5]{};
    ctx->VSGetConstantBuffers(0, 5, cb);
    auto done = [&](bool r) {
        for (auto* b : cb) if (b) b->Release();
        return r;
    };
    float obj[32]{};
    bool  have = false;
    std::vector<float> pal;
    float scale   = 0.0f;
    bool  skinned = false;
    if (cb[3]) {
        D3D11_BUFFER_DESC d{};
        cb[3]->GetDesc(&d);
        skinned = d.ByteWidth == kPaletteBytes;
    }
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end(); it != m_obj_consts.end() && it->second.size == 128) {
            std::memcpy(obj, it->second.f, sizeof(obj));
            have = true;
        }
        if (skinned) if (auto it = m_bone_data.find(cb[3]); it != m_bone_data.end() && it->second.size() >= 3072) pal = it->second;
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
    }
    if (!have || (skinned && pal.empty())) {
        if (prop_draw) s.hidden++;
        return done(prop_draw);
    }
    const glm::mat4 Om = from_rows(obj);
    auto bone_m = [&](uint32_t b) {
        glm::mat4 S{ 1.0f };
        if (b != kRigid && (b + 1) * 12 <= pal.size()) S = from_rows(pal.data() + b * 12);
        return S;
    };
    auto world_of = [&](uint32_t b) { return skinned ? Om * bone_m(b) : Om; };   // rest space -> game world
    if (main_draw) {
        s.body       = world_of(0);
        s.body_frame = frame;
    }
    // This draw's estimate of the body now (manualreload31): this frame's, or the last one moved as this draw's own object moved since.
    glm::mat4 body_x = s.body;
    bool      body_moved = false;
    if (!main_draw && s.body_frame != frame)
        if (const auto lo = s.last_om.find(count); lo != s.last_om.end() && lo->second.second == s.body_frame) {
            body_x     = Om * glm::inverse(lo->second.first) * s.body;
            body_moved = true;
        }
    if (!main_draw) s.last_om[count] = { Om, frame };

    // This draw's vertices by bone, read once per weapon (one draw every 15 frames, the immediate context).
    const auto mk = std::make_pair(sig, uint32_t(count));
    auto       mi = s.mesh.find(mk);
    [&]() __declspec(noinline) {
    if (mi == s.mesh.end() && !s.mesh_failed.contains(mk) && frame >= s.scan_next && !fc5::rec::is_deferred(ctx) && scale > 0.0f) {
        s.scan_next = frame + 15;
        Mesh               m{};
        ID3D11InputLayout* il = nullptr;
        ctx->IAGetInputLayout(&il);
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        auto off = [&](const char* sem, DXGI_FORMAT fm) {
            for (const auto& e : layout) if (_stricmp(e.semantic.c_str(), sem) == 0 && e.index == 0 && (fm == DXGI_FORMAT_UNKNOWN || e.format == fm) && e.slot == 0) return int(e.offset);
            return -1;
        };
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ctx->IAGetPrimitiveTopology(&topo);
        ID3D11Buffer* vb = nullptr;
        UINT          stride = 0, voff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT   ib_fmt{};
        UINT          ib_off = 0;
        ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
        const int bi_off = off("BLENDINDICES", DXGI_FORMAT_UNKNOWN);
        if (off("POSITION", DXGI_FORMAT_R16G16B16A16_SINT) == 0 && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && vb && ib && stride >= 8 && stride <= 128 &&
            (!skinned || bi_off >= 0)) {
            std::vector<uint8_t> vbd, ibd;
            if (read_gpu(ctx, vb, vbd) && read_gpu(ctx, ib, ibd)) {
                const bool         i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
                std::set<uint32_t> seen;
                std::map<uint32_t, glm::vec3> sum;
                for (UINT k = 0; k < count; ++k) {
                    const size_t io = size_t(ib_off) + size_t(start + k) * (i32 ? 4 : 2);
                    if (io + (i32 ? 4 : 2) > ibd.size()) break;
                    uint32_t idx = 0;
                    std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
                    if (count <= 12000) m.tris.push_back(idx);   // reloadejectone
                    if (!seen.insert(idx).second) continue;   // each vertex once
                    const size_t vo = size_t(voff) + size_t(int64_t(idx) + base) * stride;
                    if (vo + stride > vbd.size()) continue;
                    const uint8_t* v = vbd.data() + vo;
                    int16_t        qv[3];
                    std::memcpy(qv, v, 6);
                    const glm::vec3 p{ float(qv[0]) * scale, float(qv[1]) * scale, float(qv[2]) * scale };
                    const uint32_t  b = skinned ? uint32_t(v[bi_off]) : 0u;
                    if (count <= 12000) m.vpos[idx] = p;
                    sum[b] += p;
                    m.n[b]++;
                    m.max_bone = std::max(m.max_bone, b);
                    auto& pv   = m.pts[b];
                    if (pv.size() < 4000) pv.push_back(p);   // all of a magazine (the AR-C's 1400; 600 until 2 Nov 12:40 could miss its top)
                }
                for (const auto& [b, sm] : sum) m.cent[b] = sm / float(m.n[b]);
                m.ok      = !m.n.empty();
                m.skinned = skinned;
            }
        }
        if (il) il->Release();
        if (vb) vb->Release();
        if (ib) ib->Release();
        if (m.ok) {
            std::string bs;
            for (const auto& [b, n] : m.n) if (n >= kMinVerts) bs += fmt::format(" {}:{}", b, n);
            spdlog::info("[ReloadMag] weapon {}: draw {} indices read ({}{}, bones with their vertices:{})", sig, count, skinned ? "skinned" : "rigid",
                         m.max_bone > fc5::reload::kMaxWeaponBone ? fmt::format(", bones up to {}: not the weapon's", m.max_bone) : std::string{}, bs);
            mi = s.mesh.emplace(mk, std::move(m)).first;
        } else {
            s.mesh_failed.insert(mk);
            spdlog::info("[ReloadMag] weapon {}: draw {} indices not readable (stride {}, topology {}, blend indices at {})", sig, count, stride, int(topo), bi_off);
        }
    }
    }();
    // This frame's parts: each bone with enough vertices (a rigid draw: itself), rest space -> game world.
    if (mi != s.mesh.end() && mi->second.ok) {
        for (const auto& [b, n] : mi->second.n)
            if (n >= kMinVerts) s.cur[{ count, skinned ? b : kRigid }] = world_of(skinned ? b : kRigid);
    } else if (part) {
        if (part->rigid) s.cur[{ count, kRigid }] = Om;
        else for (int i = 0; i < part->nbones; ++i) s.cur[{ count, part->bones[i] }] = world_of(part->bones[i]);
    }
    if (handle_here)
        for (int i = 0; i < hp.nbones; ++i) s.cur[{ count, hp.bones[i] }] = world_of(hp.bones[i]);
    if (prop_draw) {   // read for the learning (manualreload19), hidden as always
        s.hidden++;
        return done(true);
    }
    // reloadscan (test): each draw once, with its parts' places on the weapon controller (cm).
    [&]() __declspec(noinline) {
    if (fc5::reload::scanning() && s.scan_seen.insert({ sig, count, start, base }).second) {
        std::string     ps;
        const glm::mat4 ci = glm::inverse(s.ctrl) * s.w2d;
        for (const auto& [k, T] : s.cur)
            if (k.first == count) {
                const glm::vec3 p = point(ci * T, s.centre(sig, k));
                ps += fmt::format(" {}:({:.1f},{:.1f},{:.1f})cm", k.second == kRigid ? std::string{ "r" } : std::to_string(k.second), p.x * 100.0f, p.y * 100.0f, p.z * 100.0f);
            }
        spdlog::info("[ReloadMag] scan: weapon {} draw {} indices from {} base {}, {}{}{}; parts on the weapon controller (right, up, back):{}", sig, count, start, base,
                     skinned ? "skinned" : "rigid", part ? ", the magazine" : "", handle_here ? ", the charging handle" : "", ps.empty() ? " none read" : ps);
    }
    }();
    const bool mag_here = known && part != nullptr;
    if (!mag_here && !handle_here && !also_here && !rev_here && !belt_here) return done(false);

    // What the magazine and the handle do now.
    fc5::reload::View v{};
    if (!fc5::reload::view(v)) return done(false);
    enum Mode { kNone, kPin, kHide, kInsert, kHand };
    Mode mode = kNone;
    if (mag_here) {
        // A pump shotgun's shell: in the left hand, going into the loading port, else the game's own (not shown while its reload runs).
        // A revolver's rounds: drawn by its own block below.
        if (v.revolver) mode = kNone;
        else if (v.tube) mode = v.engaged ? kInsert : v.hand_mag ? kHand : fc5::reload::game_busy() ? kHide : kNone;
        else if (v.mag_in_gun) mode = fc5::reload::game_busy() ? kPin : kNone;
        else if (v.engaged) mode = kInsert;
        else if (v.hand_mag) mode = kHand;
        else mode = kHide;
        if (mode != s.last_mode && fc5::reload::log_on()) {
            static const char* const kName[]{ "the game's own", "kept on the gun while the game reloads", "not drawn (out of the gun)", "sliding into the well", "in the left hand" };
            spdlog::info("[ReloadMag] the game's magazine: {} (moved {}, hidden {}, kept {}, at the hip {} draws so far)", kName[mode], s.moved, s.hidden, s.pinned, s.pouched);
            s.last_mode = mode;
        }
    }
    // reloadhandgame: an empty handgun's slide stays back (as the game's locks open) until it is racked.
    const bool rack = (handle_here || also_here) && ((v.racking && v.rack > 0.0f) || v.slide_lock > 0.0f ||
                                                     (v.bolt && (v.rack > 0.0f || v.bolt_up > 0.0f)));   // any pull moves it (only a pull after an empty reload chambers a round); reloadboltturn: a bolt left open too
    // A pump shotgun's pump: always drawn at rest plus the hand's pull, never where the game's own pump animation puts it. reloadboltpin: a bolt
    // action's bolt too (author 6 Nov 12:38, the .308: the game's own bolt cycle
    // after each shot was drawn, its rest was kept but used only for pumps).
    const bool pin_pump = (v.tube || v.bolt) && handle_here && s.pin_sig == sig && !s.pin_rest.empty();
    if (handle_here && (v.rack_ball || v.racking)) s.shown_handle = true;   // the drawn bar stays away while the game's handle is there to pull
    if (m_hand_cb_swapped || m_th_cb_swapped) {   // another move owns this draw's constants: at least not shown out of the gun
        if (main_draw && s.diag_swapped_frame != frame) {
            s.diag_swapped_frame = frame;
            s.diag_swapped++;
        }
        return done(mode == kHide);
    }
    const bool body_now = s.body_frame >= frame - 1;
    const bool base_ok  = s.base_sig == sig && !s.base.empty();

    // The magazine's keys in this draw, with their place on the gun.
    std::vector<std::pair<Key, Rel>> mparts;
    if (mag_here && base_ok) {
        if (part->rigid) {
            if (const auto b = s.base.find({ count, kRigid }); b != s.base.end()) mparts.push_back(*b);
        } else {
            for (int i = 0; i < part->nbones; ++i)
                if (const auto b = s.base.find({ count, part->bones[i] }); b != s.base.end()) mparts.push_back(*b);
        }
    }
    const fc5::reload::Geom g      = fc5::reload::geom(sig);
    const glm::vec3         well_c = s.well_c.contains(sig) ? s.well_c[sig] : mir(g.well, lh);   // the model's top on the gun (measured on it)
    glm::vec3               axis_c = mir(g.axis, lh);
    axis_c                         = glm::length(axis_c) > 1e-4f ? glm::normalize(axis_c) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    const float             ws     = m_world_scale.load();
    // manualreload39: a part in the weapon controller's axes from its place on the gun's body and the held body -> controller (not this frame's).
    const bool hold_ok = s.b2c_hold_sig == sig && s.b2c_hold_n >= 10;
    auto rc_of = [&](const Rel& r) { return hold_ok ? s.b2c_hold * r.r : r.rc; };
    // reloadhandgame: the hand's magazine placed by its own centre (its main part's, for every draw of it), not through the measured well.
    glm::vec3 mag_from = well_c, mag_at = v.hand_mag_off;
    if (v.hand_mag_game) {
        fc5::reload::MagPart mp[8];
        int                  mn = 0;
        if (fc5::reload::mag_parts(sig, mp, 8, mn))
            for (int i = 0; i < mn; ++i) {
                if (mp[i].prop) continue;
                const Key  k0{ mp[i].count, mp[i].rigid ? kRigid : mp[i].bones[0] };
                const auto b0 = s.base.find(k0);
                if (b0 == s.base.end()) break;
                mag_from    = point(rc_of(b0->second), s.centre(sig, k0));
                mag_at      = v.hand_mag_c;
                s.mag_ref_c = mag_from;
                break;
            }
    }

    // A second draw of the magazine (the hip, the dropped one): this draw again with only the magazine's parts, placed by xd (rest space ->
    // drawn world for each part); every other bone at one point.
    UINT          xcount = count;     // reloadejectone: fewer indices of this draw
    ID3D11Buffer* xib    = nullptr;   // ... or these (one round's)
    auto xdraw = [&]() {
        if (!xib) {
            call_orig<void>(m_draw_idx_hook, 12, ctx, xcount, start, base);
            fp_eye_again(ctx, xcount, start, base);
            return;
        }
        ID3D11Buffer* pib = nullptr;
        DXGI_FORMAT   pf{};
        UINT          po = 0;
        ctx->IAGetIndexBuffer(&pib, &pf, &po);
        ctx->IASetIndexBuffer(xib, DXGI_FORMAT_R32_UINT, 0);
        call_orig<void>(m_draw_idx_hook, 12, ctx, xcount, 0u, base);
        fp_eye_again(ctx, xcount, 0u, base);
        ctx->IASetIndexBuffer(pib, pf, po);
        if (pib) pib->Release();
    };
    auto extra_draw = [&](const std::vector<std::pair<Key, glm::mat4>>& xd) __declspec(noinline) {
        if (xd.empty() || fc5::rec::is_deferred(ctx)) return false;
        if (part->rigid) {
            if (!s.pouch_obj) s.pouch_obj = make_cb(ctx, 128);
            if (!s.pouch_obj) return false;
            float o1[32];
            std::memcpy(o1, obj, sizeof(o1));
            to_rows(s.d2w * xd[0].second, o1);
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.pouch_obj), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
            ID3D11Buffer* was[3]{};
            ctx->VSGetConstantBuffers(1, 3, was);
            ID3D11Buffer* nb[3]{ s.pouch_obj, was[1], s.pouch_obj };
            call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(nb));
            xdraw();
            call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(was));
            for (auto* b : was) if (b) b->Release();
            return true;
        }
        if (!s.pouch_pal) s.pouch_pal = make_cb(ctx, kPaletteBytes);
        if (!s.pouch_pal) return false;
        const glm::mat4    Oi = glm::inverse(Om);
        std::vector<float> pb(kPaletteBytes / 4, 0.0f);
        // manualreload37 (author 3 Nov 11:32): the rest of the draw (the gun's body
        // and other bones) is hidden by collapsing its bones, but some of the magazine's vertices hang partly on them too. A zero bone pulled those
        // towards the draw's origin, the gun in the right hand, so the hip's magazine stretched towards it and moved with it. Every other bone now
        // collapses onto the magazine's own centre where it is drawn: those vertices stay with it.
        {
            const glm::vec3 c0 = s.centre(sig, xd[0].first);
            glm::mat4       C{ 0.0f };
            C[3] = Oi * s.d2w * xd[0].second * glm::vec4{ c0, 1.0f };
            C[3].w = 1.0f;
            for (size_t b = 0; (b + 1) * 12 <= pb.size(); ++b) to_rows(C, pb.data() + b * 12);
        }
        for (const auto& [k, X] : xd)
            if (k.second != kRigid && (k.second + 1) * 12 <= pb.size()) to_rows(Oi * s.d2w * X, pb.data() + k.second * 12);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.pouch_pal), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(pb.data()), 0u, 0u);
        ID3D11Buffer* was[2]{};
        ctx->VSGetConstantBuffers(3, 2, was);
        ID3D11Buffer* nb[2]{ s.pouch_pal, s.pouch_pal };
        call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(nb));
        xdraw();
        call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(was));
        for (auto* b : was) if (b) b->Release();
        return true;
    };

    // reloadhintstyle glow (FarCry5ReloadHint.cpp): the part to work next drawn again into the hint mask right after the game's draw of it (or of
    // its second draw), with only its bones placed (xs: rest space -> game world) and every other bone collapsed onto it, as extra_draw does.
    const bool hint_on = fc5::reload::highlight_on() && fc5::reload::hint_glow() && !fc5::rec::is_deferred(ctx) && !m_scope_call;
    // rigid: a rigid part (the whole draw: its object constants, as extra_draw places one).
    auto hint_job = [&](int group, bool seen, const std::vector<std::pair<Key, glm::mat4>>& xs, bool rigid) __declspec(noinline) {
        if (!hint_on || xs.empty()) return;
        fc5::reloadhint::Job j{};
        j.group = group;
        j.seen  = seen;
        if (rigid || !skinned) {
            j.rigid = true;
            j.data.assign(obj, obj + 32);
            to_rows(xs.back().second, j.data.data());
        } else {
            const glm::mat4 Oi = glm::inverse(Om);
            j.data.assign(kPaletteBytes / 4, 0.0f);
            glm::mat4 C{ 0.0f };
            C[3]   = Oi * xs[0].second * glm::vec4{ s.centre(sig, xs[0].first), 1.0f };
            C[3].w = 1.0f;
            for (size_t b = 0; (b + 1) * 12 <= j.data.size(); ++b) to_rows(C, j.data.data() + b * 12);
            for (const auto& [k, X] : xs)
                if (k.second != kRigid && (k.second + 1) * 12 <= j.data.size()) to_rows(Oi * X, j.data.data() + k.second * 12);
        }
        fc5::reloadhint::queue(std::move(j));
        m_hint_queued = true;
    };
    // The magazine at the hip glows while the gun has none in and the hand none either (take it).
    const bool hl_pouch = fc5::reload::highlight_on() && !v.revolver && !v.tube && !v.mag_in_gun && !v.hand_mag && !v.engaged && v.pouch_mag;

    // The hip: the magazine drawn a second time there, standing as it sits in a level gun (its top at the pouch's top), facing the body's way.
    // Placed in the drawn world (the game world's axes, z up) from this frame's pouch (m_mo, the same pose sample as the gun's controller).
    [&]() __declspec(noinline) {
    if (mag_here && !v.revolver && v.pouch_mag && !mparts.empty() && m_mo.pouch_valid) {
        glm::vec3 fwd = m_mo.pouch_fwd_world;
        fwd.z         = 0.0f;
        fwd           = glm::length(fwd) > 1e-4f ? glm::normalize(fwd) : glm::vec3{ 0.0f, 1.0f, 0.0f };
        {
            const glm::vec3 up{ 0.0f, 0.0f, 1.0f };
            const glm::vec3 zb    = -fwd;
            const glm::mat3 rw{ glm::normalize(glm::cross(up, zb)), up, zb };   // drawn world: the body's right, up, back
            const glm::vec3 top_w = m_mo.pouch_pos_world + up * (0.06f * ws);
            const glm::mat4 P     = affine(rw, top_w) * shift(-well_c);   // the gun controller's axes laid on the body's, the magazine's top at top_w
            std::vector<std::pair<Key, glm::mat4>> xd;
            for (const auto& [k, r] : mparts) xd.push_back({ k, P * rc_of(r) });
            if (extra_draw(xd)) {
                s.pouched++;
                s.shown_pouch = true;
                if (hl_pouch && hint_on) {
                    std::vector<std::pair<Key, glm::mat4>> xs;
                    for (const auto& [k, X] : xd) xs.push_back({ k, s.d2w * X });   // drawn world -> game world
                    hint_job(fc5::reloadhint::kMag, true, xs, part->rigid);
                    fc5::reloadhint::set_colour(fc5::reloadhint::kMag, 0.35f, 1.0f, 0.6f, 0.85f);
                }
                if (!s.pouch_said) {
                    s.pouch_said = true;
                    spdlog::info("[ReloadMag] weapon {}: the game's magazine drawn at the hip too (draw {}, {} part(s); its top {} from the gun controller, drawn world)", sig, count,
                                 mparts.size(), v3(top_w - glm::vec3{ s.ctrl[3] }));
                }
            }
        }
    }
    }();

    // manualreload22 (author 2 Nov 21:37): a magazine taken from the hip while the gun's is still in is this
    // magazine drawn a second time in the left hand, as kHand places it; the gun's stays in the gun, and it cannot go in (FarCry5Reload.cpp:
    // no engage while the gun holds one).
    [&]() __declspec(noinline) {
    if (mag_here && !v.revolver && !v.tube && v.mag_in_gun && v.hand_mag && !mparts.empty() && m_mo.other_valid) {
        const glm::mat3 Lr = m_mo.other_rot_world * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-m_grip_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
        const glm::mat4 P  = affine(Lr * v.hand_mag_turn, m_mo.other_pos_world + Lr * (mag_at * ws)) * shift(-mag_from);
        std::vector<std::pair<Key, glm::mat4>> xd;
        for (const auto& [k, r] : mparts) xd.push_back({ k, P * rc_of(r) });
        if (extra_draw(xd)) {
            s.shown_hand = true;
            if (!s.spare_said) {
                s.spare_said = true;
                spdlog::info("[ReloadMag] weapon {}: a spare magazine in the left hand with the gun's still in: the game's magazine drawn there too (draw {}, {} part(s))", sig, count,
                             mparts.size());
            }
        }
    }
    }();

    // reloaddrop: at a drop, the magazine leaves the gun where it sat and falls (in the game world, so it stays where it fell as you move),
    // turning onto its side about the barrel's line, and lies on the ground (reloaddropfloor below the game's eyes) for 20 s.
    // Each of the magazine's draws (the AR-C: the gun's, for its magazine bone, and its top round's) keeps its own parts; the first to see
    // the drop sets the fall.
    [&]() __declspec(noinline) {
    if (mag_here) {
        const uint32_t dn = fc5::reload::drops();
        if (!s.fall_init) {
            s.fall_init  = true;
            s.fall_drops = dn;
        }
        if (!s.fall_seen.contains(count)) s.fall_seen[count] = s.fall_drops;
        if (s.fall_seen[count] != dn) {
            s.fall_seen[count] = dn;
            auto& x0           = s.fall_x0[count];
            x0.clear();
            if (fc5::reload::drop_fall_on())
                for (const auto& [k, r] : mparts) x0.push_back({ k, body_now ? s.w2d * s.body * r.r : s.ctrl * r.rc });   // where it sat on the gun this frame
        }
        if (dn != s.fall_drops) {
            s.fall_drops = dn;
            const auto& x0 = s.fall_x0[count];
            if (fc5::reload::drop_fall_on() && !x0.empty()) {
                for (auto& [fc, fx] : s.fall_x0)
                    if (fc != count) fx.clear();   // the other draws record theirs as they come to it
                glm::vec3 c{ 0.0f };
                for (const auto& [k, X0] : x0) c += point(X0, s.centre(sig, k));
                s.fall_c0     = c / float(x0.size());
                glm::vec3 bar = glm::mat3{ s.ctrl } * -s.back_c;   // the barrel's line, drawn world
                bar.z         = 0.0f;
                s.fall_axis   = glm::length(bar) > 1e-3f ? glm::normalize(bar) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                s.fall_turn   = (dn & 1u) ? 1.5708f : -1.5708f;   // onto one side or the other
                s.fall_ground = hm.cam.z - fc5::reload::drop_floor() * ws;
                const float h = std::max(0.0f, s.fall_c0.z - (s.fall_ground + 0.015f * ws)) / std::max(ws, 1e-3f);   // metres; its centre comes to rest 1.5 cm above the ground
                const float v0 = 0.3f;                                                     // m/s down, as it is pulled out
                s.fall_t_land = (-v0 + std::sqrt(v0 * v0 + 2.0f * 9.81f * h)) / 9.81f;
                s.fall_t0     = clock::now();
                s.fall_sig    = sig;
                s.falling     = true;
                spdlog::info("[ReloadMag] weapon {}: the dropped magazine falls {:.2f} m to the ground ({:.2f} s)", sig, h, s.fall_t_land);
            }
        }
        if (s.falling) {
            const float t = std::chrono::duration<float>(clock::now() - s.fall_t0).count();
            if (s.fall_sig != sig || t > s.fall_t_land + 20.0f) {
                s.falling = false;
            } else {
                const float     tf = std::min(t, s.fall_t_land);
                const glm::vec3 c  = s.fall_c0 + glm::vec3{ 0.0f, 0.0f, (-0.3f * tf - 0.5f * 9.81f * tf * tf) * ws };
                const float     a  = s.fall_turn * (s.fall_t_land > 1e-3f ? tf / s.fall_t_land : 1.0f);
                const glm::mat4 F  = shift(c) * glm::rotate(glm::mat4{ 1.0f }, a, s.fall_axis) * shift(-s.fall_c0);
                std::vector<std::pair<Key, glm::mat4>> xd;
                if (const auto fx = s.fall_x0.find(count); fx != s.fall_x0.end())
                    for (const auto& [k, X0] : fx->second)
                        if (k.first == count) xd.push_back({ k, F * X0 });
                extra_draw(xd);
            }
        }
    }
    }();

    // The rounds and cases in the air or on the ground (each in its own draw of its round's draw).
    auto draw_ejected = [&]() __declspec(noinline) {
        const auto tn = clock::now();
        std::erase_if(s.ejected, [&](const ReloadMagState::Ejected& e) { return std::chrono::duration<float>(tn - e.t0).count() > e.t_land + 10.0f; });
        for (auto& e : s.ejected) {
            if (e.k.first != count) continue;
            const float     t  = std::chrono::duration<float>(tn - e.t0).count();
            if (!e.landed && t >= e.t_land) {
                e.landed = true;
                fc5::reload::round_landed();
            }
            const float     tf = std::min(t, e.t_land);
            const glm::vec3 c  = e.c0 + e.vel * tf + glm::vec3{ 0.0f, 0.0f, -0.5f * 9.81f * tf * tf * ws };
            const glm::mat4 F  = shift(c) * glm::rotate(glm::mat4{ 1.0f }, e.spin * tf, e.axis) * shift(-e.c0);
            xcount = e.half ? count / 2 : count;
            xib    = nullptr;
            if (const auto ei = s.eject_ib.find(count); e.half && ei != s.eject_ib.end() && ei->second.first) {
                xib    = ei->second.first;
                xcount = ei->second.second;
            }
            extra_draw({ { e.k, F * e.x0 } });
            xcount = count;
            xib    = nullptr;
        }
    };

    // A round ejected by a pull of the handle (FarCry5Reload.cpp counts them): the weapon's round (the AR-C's top round, draw 3036) drawn
    // again out of the ejection port (beside the bolt, on the gun's right), thrown right, up and a little back, tumbling, onto the ground
    // (reloaddropfloor below the game's eyes), where it lies 10 s. Drawn in the round's own draw, so only while the game draws that.
    [&]() __declspec(noinline) {
    if (uint32_t rcount = 0, rbone = 0, pbone = 0; mag_here && fc5::reload::round_part(sig, rcount, rbone, pbone) && count == rcount) {
        const uint32_t en = fc5::reload::ejects();
        if (!s.eject_init || s.eject_sig != sig) {
            s.eject_init = true;
            s.eject_sig  = sig;
            s.eject_seen = en;
            s.ejected.clear();
        }
        if (en != s.eject_seen) {
            s.eject_seen  = en;
            const auto rr = s.base.find({ count, rbone });
            const auto bp = s.base.find({ sig_mesh(sig), pbone });
            if (base_ok && body_now && rr != s.base.end() && bp != s.base.end()) {
                const glm::mat4 G  = s.w2d * s.body;   // the gun's body: rest space (model: x right, y forward, z up) -> drawn world
                const glm::mat3 ga{ G };
                const glm::vec3 xr = glm::normalize(ga[0]), yf = glm::normalize(ga[1]), zu = glm::normalize(ga[2]);
                ReloadMagState::Ejected e{};
                e.k                  = { count, rbone };
                // reloadejectone (author 5 Nov): a magazine's top draw can hold its two staggered rounds: two copies, one after the
                // other. The AR-C's 3036 is taken as that from his eyes (even in indices and in vertices, 858); others if the second half of the vertices is the first half moved.
                if (count % 2 == 0) {
                    bool twin = count == 3036;
                    if (const auto tm = s.mesh.find({ sig, uint32_t(count) }); !twin && tm != s.mesh.end())
                        if (const auto tp = tm->second.pts.find(rbone); tp != tm->second.pts.end() && tp->second.size() >= 40 && tp->second.size() < 4000 && tp->second.size() % 2 == 0) {
                            const auto&     pv = tp->second;
                            const size_t    hn = pv.size() / 2;
                            const glm::vec3 dv = pv[hn] - pv[0];
                            twin               = glm::length(dv) > 0.003f;
                            for (size_t i = 1; twin && i < hn; ++i) twin = glm::length(pv[hn + i] - pv[i] - dv) < 0.0015f;
                        }
                    e.half = twin;
                    if (twin && s.eject_said != count) {
                        s.eject_said = count;
                        int   pieces = 0;
                        float gap    = 0.0f;
                        std::vector<uint32_t> one;
                        if (const auto tm2 = s.mesh.find({ sig, uint32_t(count) }); tm2 != s.mesh.end()) one = one_round(tm2->second, pieces, gap);
                        ID3D11Buffer* ib = nullptr;
                        if (!one.empty()) {
                            ID3D11Device* dev = nullptr;
                            ctx->GetDevice(&dev);
                            D3D11_BUFFER_DESC d{};
                            d.ByteWidth = UINT(one.size() * sizeof(uint32_t));
                            d.Usage     = D3D11_USAGE_IMMUTABLE;
                            d.BindFlags = D3D11_BIND_INDEX_BUFFER;
                            D3D11_SUBRESOURCE_DATA sd{};
                            sd.pSysMem  = one.data();
                            if (dev) {
                                dev->CreateBuffer(&d, &sd, &ib);
                                dev->Release();
                            }
                        }
                        if (const auto old = s.eject_ib.find(count); old != s.eject_ib.end() && old->second.first) old->second.first->Release();
                        s.eject_ib[count] = { ib, UINT(one.size()) };
                        spdlog::info("[ReloadMag] weapon {}: reloadejectone: draw {} of {} indices, {} piece(s), the widest gap {:.1f} mm: {}", sig, count, count, pieces, gap * 1000.0f,
                                     ib ? fmt::format("one round of {} indices thrown out", one.size()) : std::string{ "no split: the first half of its indices" });
                    }
                }
                const glm::mat4 X0   = G * rr->second.r;   // the round as it sits on the gun
                const glm::vec3 cr   = point(X0, s.centre(sig, e.k));
                glm::vec3       port{ 0.0f };
                if (fc5::reload::casing_port(sig, port)) e.c0 = point(G, port) + xr * (0.004f * ws);   // the game's own casing port
                else e.c0 = point(G * bp->second.r, s.centre(sig, { sig_mesh(sig), pbone })) + xr * (0.015f * ws);
                e.x0                 = shift(e.c0 - cr) * X0;
                e.vel                = (xr * 2.2f + zu * 1.3f - yf * 0.5f) * ws;
                e.axis               = glm::normalize(zu * 0.85f + xr * 0.3f);
                e.spin               = (en & 1u) ? 22.0f : -18.0f;
                const float ground   = hm.cam.z - fc5::reload::drop_floor() * ws;
                const float h        = (e.c0.z - (ground + 0.006f * ws)) / std::max(ws, 1e-3f);   // metres above where it comes to rest
                const float vz       = e.vel.z / std::max(ws, 1e-3f);
                e.t_land             = h > 0.0f ? (vz + std::sqrt(vz * vz + 2.0f * 9.81f * h)) / 9.81f : 0.0f;
                e.t0                 = clock::now();
                if (s.ejected.size() >= 6) s.ejected.erase(s.ejected.begin());
                s.ejected.push_back(e);
                spdlog::info("[ReloadMag] weapon {}: a round ejected from {} (drawn world), lands {:.2f} m below in {:.2f} s", sig, v3(e.c0), h, e.t_land);
            } else {
                spdlog::info("[ReloadMag] weapon {}: a round to eject, but the gun's parts are not placed yet (no round drawn)", sig);
            }
        }
        draw_ejected();
    }
    }();

    // The new transforms (rest space -> game world) of the moved parts of this draw.
    std::vector<std::pair<Key, glm::mat4>> moved;
    bool                                   hide_all = false;
    if (mag_here && mode != kNone) {
        if (mode == kHide) {
            hide_all = true;
        } else if (mparts.empty() || (mode != kHand && !body_now)) {
            if (mode == kHand) return done(true);   // not seen on the gun yet: not placed (nor shown floating)
        } else {
            for (const auto& [k, r] : mparts) {
                glm::mat4 T{ 1.0f };
                if (mode == kPin) {
                    T = s.body * r.r;   // where it sits on the gun, wherever the game's reload moves the gun
                } else if (mode == kInsert) {
                    const glm::vec3 d = s.c2b * (-axis_c * (1.0f - std::clamp(v.insert, -2.0f, 1.0f)) * v.travel * ws);
                    T                 = s.body * shift(d) * r.r;
                    // reloadrockin (manualreload23): an AK's magazine tilted about its front lug (the well's front, 2 cm ahead of its centre),
                    // its bottom forward, rocked back into line as it goes home (the gun controller's axes, then the body's).
                    if (const float rk = fc5::reload::rock_deg(sig); rk > 0.0f) {
                        const glm::vec3 fwd_c = -s.back_c;
                        glm::vec3       rt    = glm::cross(axis_c, s.back_c);
                        rt                    = glm::length(rt) > 1e-4f ? glm::normalize(rt) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                        const float     th    = glm::radians(rk) * (1.0f - std::clamp(v.insert, 0.0f, 1.0f));
                        const glm::vec3 pv    = well_c + fwd_c * (0.02f * ws);
                        const glm::mat4 Rc    = shift(pv) * glm::rotate(glm::mat4{ 1.0f }, th, rt) * shift(-pv);
                        T                     = s.body * shift(d) * glm::inverse(s.b2c4) * Rc * s.b2c4 * r.r;
                    }
                } else {   // kHand: its top at reloadhandmag from the left controller, turned as it sits on the gun with the left controller for the gun
                    if (!m_mo.other_valid) return done(true);
                    // The left controller as the pad poll reads it: without the grip's tilt (grippitch) that the motion frame's other hand
                    // carries, so the magazine's top and axis drawn here are the ones the well is matched against (FarCry5Reload.cpp).
                    const glm::mat3 Lr = m_mo.other_rot_world * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-m_grip_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
                    const glm::mat4 X  = affine(Lr * v.hand_mag_turn, m_mo.other_pos_world + Lr * (mag_at * ws)) * shift(-mag_from) * rc_of(r);
                    T                  = s.d2w * X;
                }
                moved.push_back({ k, T });
                if (fc5::reload::rec_on() && (mode == kHand || mode == kInsert) && s.rec_mag_frame != frame) {   // reloadrec: as drawn
                    s.rec_mag       = glm::inverse(s.ctrl) * s.w2d * T;
                    s.rec_mag_k     = k;
                    s.rec_mag_frame = frame;
                }
            }
        }
    }
    [&]() __declspec(noinline) {
    if ((rack || pin_pump) && body_now) {
        // The charging handle (and what it pulls) back along the gun as far as the hand has pulled it (a pump: from its rest). A lever action
        // (manualreload30): its lever (the first part) turned down about its hinge, what it pulls (a bolt) back along the gun.
        glm::vec3       lpiv{ 0.0f };
        float           ldeg  = 0.0f;
        const bool      lev   = v.tube && fc5::reload::lever_parts(sig, lpiv, ldeg);
        const glm::mat4 Rl    = lev ? shift(lpiv) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(ldeg * v.lever_f), glm::vec3{ 1.0f, 0.0f, 0.0f }) * shift(-lpiv) : glm::mat4{ 1.0f };
        const float     pulled = rack ? std::max(v.racking || v.bolt ? v.rack : 0.0f, v.rack_len > 1e-4f ? v.slide_lock / v.rack_len : 0.0f) : lev ? v.lever_f * 0.6f : 0.0f;
        const glm::vec3 d      = s.c2b * (s.back_c * pulled * v.rack_len * ws);
        const glm::mat4 sh     = s.body * shift(d) * glm::inverse(s.body);
        // reloadboltturn: the bolt turned up about the gun's back through its axis (the handle point, moved across onto the well's centre line
        // once the well is measured), then pulled back. Built in the weapon controller's axes (as reloadrockin), applied in the body's.
        glm::mat4 Rb{ 1.0f };
        if (v.bolt && v.bolt_up > 0.0f && handle_here) {
            const glm::vec3 hc = mir(g.handle, lh);
            glm::vec3       rt = glm::cross(axis_c, s.back_c);
            rt                 = glm::length(rt) > 1e-4f ? glm::normalize(rt) : glm::vec3{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 pv = (g.measured ? hc - rt * glm::dot(hc - well_c, rt) : hc) * ws;
            const float     th = glm::radians((lh ? -1.0f : 1.0f) * v.bolt_deg * std::clamp(v.bolt_up, 0.0f, 1.0f));
            Rb                 = glm::inverse(s.b2c4) * shift(pv) * glm::rotate(glm::mat4{ 1.0f }, th, s.back_c) * shift(-pv) * s.b2c4;
            if (!s.bolt_said) {
                s.bolt_said = true;
                spdlog::info("[ReloadMag] weapon {}: reloadboltturn: the bolt (part(s) {}/{}) turned about the gun's back through ({:.3f}, {:.3f}, {:.3f}) (weapon controller's axes), up to {:.0f} deg", sig, count,
                             hp.bones[0], pv.x, pv.y, pv.z, v.bolt_deg);
            }
        }
        const glm::mat4 shb    = s.body * shift(d) * Rb * glm::inverse(s.body);
        if (handle_here)
            for (int i = 0; i < hp.nbones; ++i) {
                const Key  k{ count, hp.bones[i] };
                const auto pr = pin_pump ? s.pin_rest.find(k) : s.pin_rest.end();
                if (lev && i == 0 && pr != s.pin_rest.end()) moved.push_back({ k, s.body * Rl * pr->second });
                else moved.push_back({ k, pr != s.pin_rest.end() ? s.body * shift(d) * Rb * pr->second : shb * world_of(hp.bones[i]) });
            }
        else
            moved.push_back({ Key{ count, hp.also_bone }, sh * world_of(hp.also_bone) });
        // reloadboltparts: a bolt action's other bolt parts (kept with the bolt's rest above) where they sit at rest, whatever the game's cycle does.
        if (handle_here && pin_pump && v.bolt && fc5::reload::bolt_parts())
            for (const auto& [k, r] : s.pin_rest) {
                if (k.first != count) continue;
                bool handle = false;
                for (int i = 0; i < hp.nbones; ++i) handle = handle || hp.bones[i] == k.second;
                if (!handle) moved.push_back({ k, s.body * r });
            }
        if (handle_here && rack) s.racked++;
        if (handle_here && rack && !s.rack_said) {
            s.rack_said = true;
            std::string bl;
            for (int i = 0; i < hp.nbones; ++i) bl += fmt::format("{}{}", i ? "+" : "", hp.bones[i]);
            spdlog::info("[ReloadMag] weapon {}: the game's charging handle (part(s) {}/{}) moves with the pull", sig, count, bl);
        }
    }
    }();
    // A revolver (manualreload10): the crane, drum and extractor and the rounds kept where they sit on the gun (the game's own reload and the
    // drum's turn at each shot not seen), the cylinder swung out about the crane's hinge as far as open_f; each chamber's round shown when it
    // holds one (live or fired), the next round in the left hand or sliding into its chamber, one at the hip, the fired cases falling out.
    [&]() __declspec(noinline) {
    if (rev_here && body_now && s.pin_sig == sig && !s.pin_rest.empty()) {
        const glm::vec3 fwd{ 0.0f, 1.0f, 0.0f };
        if (count == rp.rounds && s.rev_said_sig != sig) {
            s.rev_said_sig = sig;
            spdlog::info("[ReloadMag] weapon {}: the {} rounds (draw {}) drawn by the mod, {}", sig, rp.brk ? "break action's" : "revolver's", count,
                         s.body_frame == frame ? "after the gun's main draw" : body_moved ? "before the gun's main draw (the body moved with the rounds' own object)"
                                                                                          : "before the gun's main draw (the last frame's body)");
        }
        const glm::mat4 Ro = shift(rp.pivot) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(rp.open_deg) * std::clamp(v.open_f, 0.0f, 1.0f), rp.axis) * shift(-rp.pivot);
        auto pinned = [&](const Key& k, glm::mat4& T) {
            const auto pr = s.pin_rest.find(k);
            if (pr == s.pin_rest.end()) return false;
            T = (rp.door && k.first == rp.rounds ? body_x : body_x * Ro) * pr->second;   // (reloadratdoor: the missile stays)
            return true;
        };
        if (count == rp.main) {
            for (int i = 0; i < rp.nswing; ++i)
                if (glm::mat4 T{}; pinned({ count, rp.swing[i] }, T)) moved.push_back({ Key{ count, rp.swing[i] }, T });
        } else {
            // The fired cases dropped (FarCry5Reload.cpp): each falls from its chamber of the open cylinder, out of its back, to the ground.
            const uint32_t dn = v.dumps;
            if (!s.rev_dump_init || s.rev_dump_sig != sig) {
                s.rev_dump_init  = true;
                s.rev_dump_sig   = sig;
                s.rev_dumps_seen = dn;
            }
            if (dn != s.rev_dumps_seen) {
                s.rev_dumps_seen = dn;
                const glm::mat4 G   = s.w2d * body_x;
                const glm::mat3 ga{ G };
                const glm::vec3 xr  = glm::normalize(ga[0]), yf = glm::normalize(ga[1]), zu = glm::normalize(ga[2]);
                const glm::mat4 Ro1 = shift(rp.pivot) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(rp.open_deg), rp.axis) * shift(-rp.pivot);
                for (int c = 0; c < rp.nround; ++c) {
                    if (!(v.dump_mask & (1u << c))) continue;
                    // reloadrigidspent (author 5 Nov 15:51): a rigid round is the
                    // whole of its model (the M79's grenade, warhead and all): its fired case is not drawn falling and lying on the ground.
                    if (rp.rigid_rounds) continue;
                    const Key  k{ count, rp.rigid_rounds ? kRigid : rp.round_bones[c] };
                    const auto pr = s.pin_rest.find(k);
                    if (pr == s.pin_rest.end()) continue;
                    ReloadMagState::Ejected e{};
                    e.k                = k;
                    e.x0               = G * Ro1 * pr->second;
                    e.c0               = point(e.x0, s.centre(sig, k));
                    e.vel              = rp.brk ? (-yf * 1.6f + zu * 1.1f + xr * (c ? 0.15f : -0.15f)) * ws   // thrown back and up by the ejector
                                                : (-yf * 0.6f - xr * (0.1f + 0.03f * float(c)) + zu * 0.05f) * ws;   // out of the back of the cylinder
                    e.axis             = glm::normalize(xr * 0.7f + zu * 0.3f);
                    e.spin             = (c & 1) ? 9.0f : -7.0f;
                    const float ground = hm.cam.z - fc5::reload::drop_floor() * ws;
                    const float h      = (e.c0.z - (ground + 0.006f * ws)) / std::max(ws, 1e-3f);
                    const float vz     = e.vel.z / std::max(ws, 1e-3f);
                    e.t_land           = h > 0.0f ? (vz + std::sqrt(vz * vz + 2.0f * 9.81f * h)) / 9.81f : 0.0f;
                    e.t0               = clock::now();
                    if (s.ejected.size() >= 8) s.ejected.erase(s.ejected.begin());
                    s.ejected.push_back(e);
                }
                spdlog::info("[ReloadMag] weapon {}: fired cases dropped out of the cylinder (mask {:#x})", sig, v.dump_mask);
            }
            draw_ejected();
            // The next round at the hip: its nose at the pouch's top, lying along the body's heading. allreload: a rigid round (the M79's
            // grenade, the RPG-7's rocket) is its draw's one part; a muzzle-loaded rocket's lead point is its tail, and it stands nose up at the hip.
            auto        rkey = [&](int c) { return Key{ count, rp.rigid_rounds ? kRigid : rp.round_bones[c] }; };
            const float nose = s.rev_nose.contains(sig) ? s.rev_nose[sig] : rp.muzzle || rp.door ? 0.25f : 0.017f;
            if ((rp.muzzle || rp.door) && s.rev_nose.contains(sig)) fc5::reload::set_round_lead(sig, nose);   // (again after a weapon switch)
            auto        nose_c = [&](const Rel& r, const Key& k) { return point(rc_of(r), s.centre(sig, k)) + s.back_c * ((rp.muzzle ? nose : -nose) * ws); };   // (controller axes)
            // reloadhiploaded (author 6 Nov 12:40: picked up loaded, the M79's grenade and the RAT4's rocket not at the hip): a one-shot
            // launcher's spare shown with no empty chamber too (loaded, or fired and not yet opened), while the reserve holds one.
            const int hip_slot = v.target >= 0 ? v.target : v.hip_loaded && rp.rigid_rounds && rp.nround == 1 ? 0 : -1;
            if (hip_slot >= 0 && v.pouch_mag && m_mo.pouch_valid && base_ok) {
                const Key k = rkey(hip_slot);
                if (const auto b = s.base.find(k); b != s.base.end()) {
                    glm::vec3 fw = m_mo.pouch_fwd_world;
                    fw.z         = 0.0f;
                    fw           = glm::length(fw) > 1e-4f ? glm::normalize(fw) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                    const glm::vec3 up{ 0.0f, 0.0f, 1.0f };
                    const glm::vec3 rt = glm::normalize(glm::cross(up, -fw));
                    const glm::mat3 rw = rp.muzzle || rp.door ? glm::mat3{ rt, -fw, -up } : glm::mat3{ rt, up, -fw };   // the rocket: its forward up, its top back
                    // (reloadrockethip, author 5 Nov: too big at the hip: about its tail, where the hand takes it. In the hand and on the gun: its own size.)
                    // (reloadratdoor: its lead point is its nose; at the hip it stands on its tail, as the RPG's.)
                    const glm::vec3 hip_c = nose_c(b->second, k) + (rp.door ? s.back_c * (2.0f * nose * ws) : glm::vec3{ 0.0f });
                    glm::mat4       P     = affine(rp.muzzle || rp.door ? rw * 0.55f : rw, m_mo.pouch_pos_world + up * (0.03f * ws)) * shift(-hip_c);
                    // reloadshellhip (author 6 Nov: the double barrel's shell): a break
                    // action's shell (not a rigid round) placed by its middle, the middle of its vertices' extent on the model (rest space), on the
                    // pouch's centre, which is the centre of the zone the left grip tests (FarCry5Reload.cpp, reloadpouchzone). Its turn as before.
                    if (rp.brk && !rp.rigid_rounds && fc5::reload::shell_hip()) {
                        const auto                    mi  = s.mesh.find({ sig, count });
                        const std::vector<glm::vec3>* pts = nullptr;
                        if (mi != s.mesh.end() && mi->second.ok)
                            if (const auto pi = mi->second.pts.find(k.second); pi != mi->second.pts.end() && !pi->second.empty()) pts = &pi->second;
                        if (pts) {
                            glm::vec3 lo{ 1e9f }, hi{ -1e9f };
                            for (const auto& q : *pts) {
                                lo = glm::min(lo, q);
                                hi = glm::max(hi, q);
                            }
                            const glm::vec3 mid_c = point(rc_of(b->second), 0.5f * (lo + hi));   // the shell's middle, controller axes
                            if (s.shell_hip_said != sig) {
                                s.shell_hip_said    = sig;
                                const glm::vec3 was = point(P, mid_c) - m_mo.pouch_pos_world;      // where its middle was (drawn world, z up)
                                spdlog::info("[ReloadMag] weapon {}: reloadshellhip: the hip's shell ({:.1f} cm long) placed by its middle on the hip zone's centre; "
                                             "its middle was {:.1f} cm from it ({:.1f} up, {:.1f} forward, {:.1f} right cm)",
                                             sig, std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z }) * 100.0f,
                                             glm::length(was) / std::max(ws, 1e-3f) * 100.0f, was.z / std::max(ws, 1e-3f) * 100.0f,
                                             glm::dot(was, fw) / std::max(ws, 1e-3f) * 100.0f, glm::dot(was, rt) / std::max(ws, 1e-3f) * 100.0f);
                            }
                            P = affine(rw, m_mo.pouch_pos_world) * shift(-mid_c);
                        }
                    }
                    if (extra_draw({ { k, P * rc_of(b->second) } })) s.shown_pouch = true;
                }
            }
            bool hide_round = false;   // a rigid round's draw holds only it: an empty chamber hides the draw (allreload)
            for (int c = 0; c < rp.nround; ++c) {
                const Key k = rkey(c);
                glm::mat4 T{};
                if (c == v.target && v.hand_mag && m_mo.other_valid && base_ok && s.base.contains(k)) {
                    // In the left hand: its nose at the hand's point, turned as it sits in the gun with the left controller for the gun.
                    const Rel&      r  = s.base.at(k);
                    const glm::mat3 Lr = m_mo.other_rot_world * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-m_grip_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
                    T                  = s.d2w * affine(Lr, m_mo.other_pos_world + Lr * (v.hand_mag_off * ws)) * shift(-nose_c(r, k)) * rc_of(r);
                    s.shown_hand       = true;
                } else if (c == v.target && v.engaged && pinned(k, T)) {
                    // Sliding into its chamber, from behind (the RPG-7's rocket: into the muzzle, from the front).
                    T            = (rp.door ? body_x : body_x * Ro) * shift((rp.muzzle ? fwd : -fwd) * (1.0f - std::clamp(v.insert, -2.0f, 1.0f)) * v.travel) * s.pin_rest.at(k);
                    s.shown_hand = true;
                } else if (v.cyl[c] == 0 || (rp.rigid_rounds && v.cyl[c] == 2) || !pinned(k, T)) {   // (reloadrigidspent: a fired rigid round, a whole grenade, not shown)
                    T = glm::mat4{ 0.0f };   // an empty chamber: not drawn
                    if (rp.rigid_rounds) hide_round = true;
                }
                if (!(rp.rigid_rounds && hide_round)) moved.push_back({ k, T });
            }
            if (rp.cap_bone != 0xFFFFu)
                if (glm::mat4 T{}; pinned({ count, rp.cap_bone }, T)) moved.push_back({ Key{ count, rp.cap_bone }, T });
            if (hide_round) hide_all = true;
        }
    }
    }();
    // A belt-fed gun's feed cover (manualreload12): where it sits closed, lifted about its hinge (the gun's right axis) as far as open_f.
    if (belt_here && body_now && s.pin_sig == sig) {
        if (const auto pr = s.pin_rest.find({ count, blt.cover }); pr != s.pin_rest.end()) {
            const glm::mat4 Ro = shift(blt.pivot) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(blt.open_deg) * std::clamp(v.open_f, 0.0f, 1.0f), glm::vec3{ 1.0f, 0.0f, 0.0f }) *
                                 shift(-blt.pivot);
            moved.push_back({ Key{ count, blt.cover }, s.body * Ro * pr->second });
        }
    }
    // reloadhintstyle glow: the empty magazine in the gun (orange, drop it), a ghost of the magazine seated in the gun while one is in the hand
    // (yellow, where it goes: drawn only into the see-through channel), the charging handle, slide or pump to work (blue), each as drawn now.
    [&]() __declspec(noinline) {
    if (hint_on && v.hl_show) {   // reloadhlwhen: only with the gun run dry or a reload begun
        auto now_of = [&](const Key& k) {
            for (const auto& [mk, T] : moved)
                if (mk == k) return T;
            return world_of(k.second);
        };
        if (v.hl_mag && mag_here && (mode == kNone || mode == kPin)) {
            std::vector<std::pair<Key, glm::mat4>> xs;
            if (part->rigid || !skinned) xs.push_back({ Key{ count, kRigid }, now_of(Key{ count, kRigid }) });
            else for (int i = 0; i < part->nbones; ++i) xs.push_back({ Key{ count, part->bones[i] }, now_of(Key{ count, part->bones[i] }) });
            hint_job(fc5::reloadhint::kMag, true, xs, part->rigid);
            fc5::reloadhint::set_colour(fc5::reloadhint::kMag, 1.0f, 0.45f, 0.1f, 1.0f);
        }
        if (v.hl_well && mag_here && !v.revolver && !mparts.empty() && body_now) {
            std::vector<std::pair<Key, glm::mat4>> xs;
            for (const auto& [k, r] : mparts) xs.push_back({ k, s.body * r.r });   // where it sits on the gun
            hint_job(fc5::reloadhint::kMag, false, xs, part->rigid);
            fc5::reloadhint::set_colour(fc5::reloadhint::kMag, 1.0f, 0.85f, 0.2f, v.well_near ? 1.7f : 1.0f);
        }
        // reloadhintglow2 (author 5 Nov 12:47: the handles' glow not seen): the handle also glows where its blue ball would show (the free
        // hand near it, or pulling it), dimmer than as the part to work next, so a glow always takes the ball's place; and brighter than
        // before (a handle is small next to a magazine). A lever action's lever (its handle part) while its chamber needs it.
        if ((v.hl_handle || v.hl_lever || v.rack_ball) && (handle_here || also_here)) {
            std::vector<std::pair<Key, glm::mat4>> xs;
            const bool rigid = !skinned || (handle_here ? hp.bones[0] == kRigid : hp.also_bone == kRigid);   // a whole draw (reloadhandlepart .../r)
            if (rigid) xs.push_back({ Key{ count, kRigid }, now_of(Key{ count, kRigid }) });
            else if (handle_here) for (int i = 0; i < hp.nbones; ++i) xs.push_back({ Key{ count, hp.bones[i] }, now_of(Key{ count, hp.bones[i] }) });
            else xs.push_back({ Key{ count, hp.also_bone }, now_of(Key{ count, hp.also_bone }) });
            hint_job(fc5::reloadhint::kHandle, true, xs, rigid);
            const float st = v.hl_handle || v.hl_lever ? 1.7f : v.racking ? 0.6f : 1.0f;
            fc5::reloadhint::set_colour(fc5::reloadhint::kHandle, 0.35f, 0.75f, 1.0f, st);
        }
        // reloadmuzzlezone (author 5 Nov 15:51): a one-piece round (the RPG-7's rocket, the
        // M79's grenade) in the hand: a ghost of it seated in the gun (yellow, seen through the gun: the loading tube's length and where it
        // ends), as the magazine's ghost in the well; the yellow ball is left out while it shows.
        if (v.well_ball && rev_here && count == rp.rounds && rp.rigid_rounds && body_now && s.pin_sig == sig && v.target >= 0) {
            if (const auto pr = s.pin_rest.find(Key{ count, kRigid }); pr != s.pin_rest.end()) {
                const glm::mat4 Rg = shift(rp.pivot) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(rp.open_deg) * std::clamp(v.open_f, 0.0f, 1.0f), rp.axis) * shift(-rp.pivot);
                hint_job(fc5::reloadhint::kMag, false, { { Key{ count, kRigid }, body_x * Rg * pr->second } }, true);
                fc5::reloadhint::set_colour(fc5::reloadhint::kMag, 1.0f, 0.85f, 0.2f, v.well_near ? 1.7f : 1.0f);
            }
        }
        // reloadcoverpush: a belt-fed gun's open feed cover over a new belt (push it down), as drawn now (lifted, or going down with the hand).
        if (v.hl_cover && belt_here && body_now && s.pin_sig == sig) {
            hint_job(fc5::reloadhint::kHandle, true, { { Key{ count, skinned ? blt.cover : kRigid }, now_of(Key{ count, skinned ? blt.cover : kRigid }) } }, !skinned);
            fc5::reloadhint::set_colour(fc5::reloadhint::kHandle, 0.35f, 0.75f, 1.0f, v.cover_held ? 2.2f : 1.7f);
        }
        // reloadhintglow2: a revolver's cylinder (its parts that swing out: crane, drum, extractor), as drawn now (swung out or not).
        if (v.hl_cyl && rev_here && count == rp.main && rp.nswing > 0) {
            std::vector<std::pair<Key, glm::mat4>> xs;
            if (!skinned) xs.push_back({ Key{ count, kRigid }, now_of(Key{ count, kRigid }) });
            else for (int i = 0; i < rp.nswing; ++i) xs.push_back({ Key{ count, rp.swing[i] }, now_of(Key{ count, rp.swing[i] }) });
            hint_job(fc5::reloadhint::kHandle, true, xs, !skinned);
            fc5::reloadhint::set_colour(fc5::reloadhint::kHandle, 0.35f, 0.75f, 1.0f, 1.7f);
        }
    }
    }();

    if (!hide_all && moved.empty()) return done(false);

    if (!skinned) {
        if (hide_all) {
            s.hidden++;
            if (m_hint_queued) reload_hint_after(ctx, count, start, base);   // the draw is left out: the hip's magazine glows from its second draw's state
            return done(true);
        }
        const glm::mat4 O2 = moved.back().second;
        float           o1[32];
        std::memcpy(o1, obj, sizeof(o1));
        to_rows(O2, o1);
        for (auto*& sb : s.standin)
            if (!sb) sb = make_cb(ctx, 128);
        if (!s.standin[0] || !s.standin[1]) return done(mode == kHand);
        ctx->VSGetConstantBuffers(1, 3, s.orig);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[0]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[1]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
        ID3D11Buffer* nb[3]{ s.standin[0], s.orig[1], s.standin[1] };
        call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(nb));
        m_rm_cb_swapped   = true;
        m_hand_cb_swapped = true;   // the draw hooks restore through hands_cb_restore
    } else {
        // Skinned: each moved bone's skinning matrix replaced (S2 = Om^-1 T), the palette swapped for this draw (as the gloves').
        std::vector<float> bones = pal;
        const glm::mat4    Oi    = glm::inverse(Om);
        int                set   = 0;
        if (hide_all)
            for (int i = 0; i < part->nbones; ++i) {
                const uint32_t bi = part->bones[i];
                if ((bi + 1) * 12 > bones.size()) continue;
                std::fill(bones.begin() + bi * 12, bones.begin() + bi * 12 + 12, 0.0f);   // every vertex of that bone at one point: not drawn
                set++;
            }
        for (const auto& [k, T] : moved) {
            if (k.second == kRigid || (k.second + 1) * 12 > bones.size()) continue;
            to_rows(Oi * T, bones.data() + k.second * 12);
            set++;
        }
        if (set == 0) return done(mode == kHand);
        if (!hands_cb_swap(ctx, bones)) return done(mode == kHand);
    }
    if (mode == kHide) s.hidden++;
    else if (mode == kPin) s.pinned++;
    else if (mode == kInsert || mode == kHand) {
        s.moved++;
        s.shown_hand = true;
    }
    return done(false);
}

void FarCry5Stereo::reload_mag_restore(ID3D11DeviceContext* ctx)
{
    m_rm_cb_swapped = false;
    if (!m_rm) return;
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(m_rm->orig));
    for (auto*& p : m_rm->orig) {
        if (p) p->Release();
        p = nullptr;
    }
}

// reloadhandsnap (manualreload41; author 3 Nov 12:37): the free controller's pose that the support hand is drawn at (drawn world), given its
// real one. While the hand's magazine slides into the well (drawn there, on the well's line: kInsert) the hand holds it as it held it before
// (the magazine's top at reloadhandmag from the controller, turned as on the gun); while the hand pulls the charging handle, slide or pump,
// it keeps the place on it it had when the pull began and moves with it along the gun's back. Eased in and out over 0.12 s, once per frame.
void FarCry5Stereo::reload_hand_snap(glm::mat3& rot, glm::vec3& pos)
{
    if (!m_rm) return;
    auto&     s     = *m_rm;
    const int frame = m_frame.load();
    if (s.snap_frame == frame) {
        if (s.snap_w > 0.0f) {
            rot = s.snap_out_rot;
            pos = s.snap_out_pos;
        }
        return;
    }
    s.snap_frame = frame;
    const auto  now = clock::now();
    const float dt  = std::clamp(std::chrono::duration<float>(now - s.hsnap_t).count(), 0.0f, 0.1f);
    s.hsnap_t       = now;
    fc5::reload::View v{};
    const bool  live = fc5::reload::hand_snap_on() && m_mo.ctrl_valid && fc5::reload::view(v) && v.active;
    const float ws   = m_world_scale.load();
    const bool  lh   = m_left_handed.load();
    const glm::mat3 gpr = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(m_grip_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
    const glm::mat3 cr  = m_mo.ctrl_rot_world * glm::transpose(gpr);   // the gun controller's raw axes (as ReloadMag's s.ctrl)
    const glm::vec3 cp  = m_mo.ctrl_pos_world;
    bool      want = false;
    glm::mat3 trel{ 1.0f };
    glm::vec3 tpos{ 0.0f };
    int       what = 0;
    if (live && v.engaged && !v.revolver && !v.tube && fc5::reload::model_shown()) {
        // The magazine as drawn going in (kInsert): its top on the well's line, (1 - insert) of the travel below the seat, turned as on the gun.
        // In the hand (kHand) its top is at reloadhandmag from the left controller (raw axes), turned so: the controller as the gun's.
        const glm::vec3 well_c = s.well_c.contains(v.sig) ? s.well_c[v.sig] : mir(v.g.well, lh);
        glm::vec3       axis_c = mir(v.g.axis, lh);
        axis_c                 = glm::length(axis_c) > 1e-4f ? glm::normalize(axis_c) : glm::vec3{ 0.0f, 1.0f, 0.0f };
        // reloadhandgame: the magazine is turned in the hand (hand_mag_turn), so the hand is turned back from the gun by as much, as the game's is at the well.
        const glm::mat3 hti    = glm::transpose(v.hand_mag_turn);
        trel                   = hti * gpr;   // raw left axes = the gun's raw axes: the controller's own = cr * gpr
        tpos                   = (v.hand_mag_game ? s.mag_ref_c : well_c) - axis_c * ((1.0f - std::clamp(v.insert, -2.0f, 1.0f)) * v.travel * ws) -
                                 hti * ((v.hand_mag_game ? v.hand_mag_c : v.hand_mag_off) * ws);
        want                   = true;
        what                   = 1;
    }
    if (live && v.racking) {
        if (!s.snap_grab) {
            s.snap_grab     = true;
            s.snap_grab_rot = glm::transpose(cr) * rot;
            s.snap_grab_pos = glm::transpose(cr) * (pos - cp);
            s.snap_rack0    = v.rack;
        }
        if (!want) {
            // reloadhandgrab (manualreload44): one place on the handle per weapon (the game's grip), held as the gun is, moving with the pull from
            // where it was taken (rack 0 at the take); else where the hand took it.
            glm::vec3 gc{ 0.0f };
            const int src = fc5::reload::hand_grab_on() ? fc5::reload::grab_hold(v.sig, v.tube, v.g.handle, gc) : 0;
            fc5::reload::HandGame hg{};
            if (!lh && !v.revolver && fc5::reload::hand_game(v.sig, hg) && hg.grab) {
                // reloadhandgame: as the game's hand on the slide, recorded with the slide at slide_lock: moving with it from there. (A pump: at rest on it, at home.)
                trel = hg.grab_turn;
                tpos = hg.grab_c * ws + s.back_c * ((std::max(v.rack * v.rack_len, hg.slide_lock) - hg.slide_lock) * ws);
                what = 7;
            } else if (src > 0) {
                trel = gpr;
                tpos = mir(gc, lh) * ws + s.back_c * (v.rack * v.rack_len * ws);
                what = 2 + src;
            } else {
                trel = s.snap_grab_rot;
                tpos = s.snap_grab_pos + s.back_c * ((v.rack - s.snap_rack0) * v.rack_len * ws);
                what = 2;
            }
            if (v.bolt && v.bolt_up > 0.0f) {
                // reloadboltturn: the hand turned up with the bolt, about the same axis as the drawn bolt (ReloadMag's bolt block).
                const glm::vec3 well_c = s.well_c.contains(v.sig) ? s.well_c[v.sig] : mir(v.g.well, lh);
                glm::vec3       axis_c = mir(v.g.axis, lh);
                axis_c                 = glm::length(axis_c) > 1e-4f ? glm::normalize(axis_c) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                const glm::vec3 hc     = mir(v.g.handle, lh);
                glm::vec3       rt     = glm::cross(axis_c, s.back_c);
                rt                     = glm::length(rt) > 1e-4f ? glm::normalize(rt) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                const glm::vec3 pv     = (v.g.measured ? hc - rt * glm::dot(hc - well_c, rt) : hc) * ws;
                const float     th     = glm::radians((lh ? -1.0f : 1.0f) * v.bolt_deg * std::clamp(v.bolt_up, 0.0f, 1.0f));
                const glm::mat3 R      = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, th, s.back_c) };
                tpos                   = pv + R * (tpos - pv);
                trel                   = R * trel;
            }
            want = true;
        }
    } else {
        s.snap_grab = false;
    }
    if (want) {
        s.snap_rel_rot = trel;
        s.snap_rel_pos = tpos;
    }
    s.snap_w = std::clamp(s.snap_w + (want ? dt : -dt) / 0.12f, 0.0f, 1.0f);
    if (what != s.snap_said && (what != 0 || s.snap_said > 0)) {
        s.snap_said = what;
        spdlog::info("[ReloadMag] reloadhandsnap: the free hand drawn {}", what == 1 ? "holding the magazine going into the well" : what == 2 ? "on the handle it pulls, moving with it"
                                                                         : what == 3 ? "on the handle it pulls at the game's learnt grip (reloadhandgrab), moving with it"
                                                                         : what == 4 ? "on the handle it pulls at the built-in grip (reloadhandgrab), moving with it"
                                                                         : what == 5 ? "on the handle it pulls at the grip made from the handle point (reloadhandgrab), moving with it"
                                                                         : what == 6 ? "on the slide it pulls at a handgun's slide grip (reloadslidegrab), moving with it"
                                                                         : what == 7 ? "on the slide it pulls as the game's hand is (reloadhandgame), moving with it"
                                                                                     : "at its controller again");
    }
    if (s.snap_w <= 0.0f) return;
    const float     w  = s.snap_w * s.snap_w * (3.0f - 2.0f * s.snap_w);   // smoothstep
    const glm::mat3 tr = cr * s.snap_rel_rot;
    const glm::vec3 tp = cp + cr * s.snap_rel_pos;
    s.snap_out_rot     = glm::mat3_cast(glm::slerp(glm::quat_cast(rot), glm::quat_cast(tr), w));
    s.snap_out_pos     = glm::mix(pos, tp, w);
    rot                = s.snap_out_rot;
    pos                = s.snap_out_pos;
}
