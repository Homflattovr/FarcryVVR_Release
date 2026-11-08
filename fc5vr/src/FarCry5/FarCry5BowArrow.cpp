// bowarrowmodel (FarCry5Bow.h; author 1 Nov 21:43): manualbow's arrow is the game's own.
//
// The game draws its bow in the weapon pass as several draws (his 1 Nov 22:40 log: weapon 51543, 9 draws, 5 of them skinned), the arrow
// resting on it among them. Which draws are the arrow is learnt at the first shot with each bow: the draws present while the string was
// drawn that go away for a few frames once the arrow is loosed (the arrow leaves the bow, the game puts a new one on). Kept per bow in
// bowarrowmesh (saved). Then, while manualbow's bow is in hand:
//  - Empty (no arrow taken): the arrow's draws are not drawn, so the bow shows no arrow;
//  - Arrow (in the string hand): its draws are moved by their object transform (VS b1, as the throwable in the hand) so the arrow's nock
//    end sits at the string hand's controller (bowarrowhand) and the arrow is turned with that controller as it sits on the bow with the
//    bow hand's: it points where the string hand points, as on the bow it points where the bow does;
//  - Nocked, Drawing, Loose: the game's own arrow on its bow, as the game draws it (exactly aligned with the bow by definition).
// bowpull (author 2 Nov 09:51): while the string is held (Drawing) the bow's moving parts are posed between the game's own rest and full
// draw, the arrow's nock end at the string hand, so the string follows the hand and a pull can stop partway (his 09:49 picture: at the end
// of the game's own draw animation its right hand sat well above the arrow's nock). The rest and the full draw are each draw's pose relative
// to the bow's main mesh, taken from the game while held still: the rest with RT up (a second after a shot), the full draw with RT held
// (the first full draw per bow and session shows the game's own animation). Skinned draws get a stand-in bone palette (VS b3, b4), rigid
// ones (the arrow) a stand-in object transform (b1, b3). Let go (Loose), the string is at rest and the arrow gone.
// The arrow's nock end is measured once per bow from its vertices (the line through them, its rear end), as the holster reads the sidearm.
#include "FarCry5Stereo.h"
#include "FarCry5Bow.h"
#include "FarCry5BoneNames.h"
#include "FarCry5Record.h"
#include "ModPath.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace
{
    constexpr UINT kPaletteBytes = 12288;   // a skinned draw's bone palette at VS b3 (a rigid draw has its previous transform there)
    constexpr int  kWatchFrames  = 60;      // frames looked at after a shot
    constexpr int  kMissFrames   = 3;       // a draw missing in this many of them is the arrow
    constexpr int  kMaxParts     = 4;
    // bowuprightwait (FarCry5Bow.h): the bow out this long before its arrow and limbs are measured, and the arrow at most this far from
    // straight ahead (at rest his bows' arrows point 0.2-0.6 deg off; 11 deg in the switch animation of 7 Nov 09:50:56).
    constexpr float kSettleSecs     = 1.5f;
    constexpr float kRestArrowMaxDeg = 4.0f;
    constexpr int   kSettleGapFrames = 10;   // weapon-pass draws of the bow missing this many frames: out again from then

    // bowpull poses only the bow's own draws. A draw whose index count the bone map knows only as the game's arms, gloves or sleeves
    // (kind 2..4, e.g. 4110 = Body_FPSArmsLeft/Right) is never sampled, posed or kept; a count it does not know, or knows as a weapon, is.
    bool bow_count(UINT count)
    {
        using namespace fc5::bonenames;
        const auto* end = kDraws + kDrawN;
        auto        it  = std::lower_bound(kDraws, end, count, [](const Draw& d, UINT c) { return d.count < c; });
        if (it == end || it->count != count) return true;
        for (; it != end && it->count == count; ++it) if (it->kind == 1) return true;
        return false;
    }

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

    // A copy of FarCry5Holster.cpp's helper: a GPU buffer read back (a staging copy; immediate context only).
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

    // The line through a mesh's points (its main axis, q): oriented forward (q -z), its extent along it, and how far the points stray
    // from it (95 % of them within thick).
    struct Line { bool ok{ false }; glm::vec3 mean{ 0.0f }, ax{ 0.0f, 0.0f, -1.0f }; float lo{ 0.0f }, hi{ 0.0f }, thick{ 0.0f }; };
    Line fit_line(const std::vector<glm::vec3>& pts)
    {
        Line l;
        if (pts.size() < 12) return l;
        for (const auto& p : pts) l.mean += p;
        l.mean /= float(pts.size());
        glm::mat3 cov{ 0.0f };
        for (const auto& p : pts) {
            const glm::vec3 d = p - l.mean;
            cov += glm::outerProduct(d, d);
        }
        for (int i = 0; i < 32; ++i) {
            const glm::vec3 n = cov * l.ax;
            if (glm::length(n) < 1e-12f) return l;
            l.ax = glm::normalize(n);
        }
        if (l.ax.z > 0.0f) l.ax = -l.ax;   // forward (q -z): the point
        l.lo = 1e9f;
        l.hi = -1e9f;
        std::vector<float> off;
        off.reserve(pts.size());
        for (const auto& p : pts) {
            const glm::vec3 d = p - l.mean;
            const float     t = glm::dot(d, l.ax);
            l.lo              = std::min(l.lo, t);
            l.hi              = std::max(l.hi, t);
            off.push_back(glm::length(d - l.ax * t));
        }
        std::nth_element(off.begin(), off.begin() + off.size() * 95 / 100, off.end());
        l.thick = off[off.size() * 95 / 100];
        l.ok    = true;
        return l;
    }

    // bowpull: a bone's (or a part's) pose between two, its turn slerped and its place and scale mixed.
    glm::mat4 blend_m(const glm::mat4& a, const glm::mat4& b, float t)
    {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        const glm::mat3 A{ a }, B{ b };
        const glm::vec3 sa{ glm::length(A[0]), glm::length(A[1]), glm::length(A[2]) }, sb{ glm::length(B[0]), glm::length(B[1]), glm::length(B[2]) };
        glm::mat4       m{ 1.0f };
        bool            lin = std::min({ sa.x, sa.y, sa.z, sb.x, sb.y, sb.z }) < 1e-6f;
        glm::mat3       Ra{ 1.0f }, Rb{ 1.0f };
        if (!lin) {
            Ra  = glm::mat3{ A[0] / sa.x, A[1] / sa.y, A[2] / sa.z };
            Rb  = glm::mat3{ B[0] / sb.x, B[1] / sb.y, B[2] / sb.z };
            lin = glm::determinant(Ra) < 0.5f || glm::determinant(Rb) < 0.5f;   // mirrored or sheared: mixed as they are
        }
        if (lin) {
            for (int c = 0; c < 4; ++c) m[c] = glm::mix(a[c], b[c], t);
            return m;
        }
        const glm::mat3 R = glm::mat3_cast(glm::slerp(glm::quat_cast(Ra), glm::quat_cast(Rb), t));
        const glm::vec3 s = glm::mix(sa, sb, t);
        m                 = glm::mat4{ glm::mat3{ R[0] * s.x, R[1] * s.y, R[2] * s.z } };
        m[3]              = glm::mix(a[3], b[3], t);
        return m;
    }
    // A palette entry no vertex uses can hold anything (his 11:54 log: draws "moving" inf cm).
    bool sane(const glm::mat4& m)
    {
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 3; ++r) if (!std::isfinite(m[c][r]) || std::abs(m[c][r]) > 1000.0f) return false;
        return true;
    }
    // How far two sets of poses are apart (m; a turn's matrix entries count a tenth).
    float pose_diff(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b)
    {
        if (a.size() != b.size()) return 1e9f;
        float d = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!sane(a[i]) || !sane(b[i])) continue;
            d = std::max(d, glm::length(glm::vec3{ a[i][3] - b[i][3] }));
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) d = std::max(d, 0.1f * std::abs(a[i][c][r] - b[i][c][r]));
        }
        return d;
    }

    std::string counts_text(const std::set<UINT>& s)
    {
        std::string t;
        for (const UINT c : s) t += (t.empty() ? "" : " ") + std::to_string(c);
        return t.empty() ? std::string{ "none" } : t;
    }
}

struct FarCry5Stereo::BowArrowState
{
    int            frame{ -1 };
    uint32_t       sig{ 0 };     // the bow the per-bow state below is for
    std::set<UINT> cur;      // this frame's weapon-pass draws with the bow in hand (index counts)
    std::set<UINT> drawn;    // the last frame's while the string was drawn (RT held)

    // Learning at a shot.
    uint32_t           shots_seen{ 0 };
    bool               watching{ false };
    uint32_t           watch_sig{ 0 };
    int                watch_frames{ 0 };
    std::set<UINT>     before;
    std::map<UINT, int> miss;

    // The arrow's nock end per part (index count), in the part's own object space; measured once per bow.
    uint32_t                 tail_sig{ 0 };
    std::map<UINT, glm::vec3> tail;
    float                    tail_len{ 0.0f };
    int                      tail_tries{ 0 };
    bool                     tail_given_up{ false };
    int                      meas_frame{ -1 };
    std::vector<glm::vec3>   meas_pts;   // that frame's arrow points, q (game camera axes)
    std::map<UINT, glm::mat4> meas_obj;  // each part's object transform that frame
    glm::mat3                meas_rot{ 1.0f };
    glm::vec3                meas_cam{ 0.0f };
    glm::mat3                meas_cr{ 1.0f };   // gunstill's steadying that frame (q -> cr q + ct) and the bow's grip there
    glm::vec3                meas_ct{ 0.0f }, meas_grip{ 0.0f };
    // bowuprightwait: since when this bow's weapon-pass draws have come without a break (the game's switch animation plays in the first second).
    uint32_t                              out_sig{ 0 };
    int                                   out_last{ -100 };
    std::chrono::steady_clock::time_point out_t{};
    bool                                  settled{ false };
    std::map<uint32_t, int>               rest_refused;   // arrows refused as not at rest, per bow (said at most 3 times)

    // bowupright: the arrow's line and the bow's limbs in the steadied axes (the bow as held still), and the turn made from them.
    uint32_t  line_sig{ 0 };
    glm::vec3 line_mean{ 0.0f }, line_ax{ 0.0f, 0.0f, -1.0f }, line_grip{ 0.0f };
    uint32_t  limb_sig{ 0 };
    glm::vec3 limb{ 0.0f, 1.0f, 0.0f };
    int       limb_tries{ 0 };
    uint32_t  fix_sig{ 0 };
    glm::mat3 fix_base{ 1.0f };
    // bowuprightkeep (FarCry5Bow.h): each bow's fix_base kept across sessions in bin\fc5vr_bowupright.txt ("upright <id> <9 numbers, by column>").
    // fix_from_kept: fix_base is the kept one (a measurement this session still replaces it).
    std::map<uint32_t, glm::mat3> fix_kept;
    std::set<uint32_t>            fix_kept_said;
    // And each bow's grip on the sling (m_bow_sling_hand: the game's left hand on it, q), "hand <id> x y z": measured only with the bow in the
    // hand, so a bow slung from the load had none and was not placed as the slung bow at all (pickup4, 6 Nov: no [BowArrow] line after the
    // relaunch). Kept once per session, with the turn.
    std::map<uint32_t, glm::vec3> hand_kept;
    std::set<uint32_t>            hand_saved, hand_kept_said;
    bool                          fix_from_kept{ false };
    BowArrowState() { fix_load(); }
    void fix_load()
    {
        std::ifstream f{ fc5::mod_file("fc5vr_bowupright.txt") };
        std::string   line;
        while (std::getline(f, line)) {
            std::istringstream ls{ line };
            std::string        tag;
            uint32_t           sig = 0;
            glm::mat3          m{ 1.0f };
            ls >> tag >> sig;
            if (tag == "hand" && sig != 0) {
                glm::vec3 h{ 0.0f };
                if (ls >> h.x >> h.y >> h.z && std::isfinite(h.x + h.y + h.z) && glm::length(h) < 3.0f) hand_kept[sig] = h;
                continue;
            }
            bool               ok  = tag == "upright" && sig != 0;
            for (int c = 0; c < 3 && ok; ++c) for (int r = 0; r < 3 && ok; ++r) ok = bool(ls >> m[c][r]) && std::isfinite(m[c][r]) && std::abs(m[c][r]) <= 1.01f;
            if (ok && std::abs(glm::determinant(m) - 1.0f) < 0.05f) fix_kept[sig] = m;
        }
        if (!fix_kept.empty()) {
            std::string b;
            for (const auto& [w, m] : fix_kept) b += (b.empty() ? "" : ", ") + std::to_string(w);
            spdlog::info("[BowArrow] bowuprightkeep: the upright turn of bow(s) {} read from fc5vr_bowupright.txt", b);
        }
    }
    void fix_save()
    {
        std::ofstream f{ fc5::mod_file("fc5vr_bowupright.txt"), std::ios::trunc };
        for (const auto& [w, m] : fix_kept) {
            f << "upright " << w;
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) f << ' ' << m[c][r];
            f << '\n';
        }
        for (const auto& [w, h] : hand_kept) f << "hand " << w << ' ' << h.x << ' ' << h.y << ' ' << h.z << '\n';
    }
    // The kept turn put in place for a bow not measured this session (none kept, or bowuprightkeep off: left as it is).
    void fix_use_kept(uint32_t sig)
    {
        if (sig == 0 || fix_sig == sig || !fc5::bow::upright_keep()) return;
        const auto it = fix_kept.find(sig);
        if (it == fix_kept.end()) return;
        fix_base      = it->second;
        fix_sig       = sig;
        fix_from_kept = true;
        if (fix_kept_said.insert(sig).second)
            spdlog::info("[BowArrow] bow {}: bowuprightkeep: upright as measured in an earlier session (fc5vr_bowupright.txt) until measured again here", sig);
    }
    // The bow's turn for the weapon pass (m_bow_fix): bowupright's measured turn, bowflip, bowhold; none when not known for this bow.
    glm::mat3 fix_now(uint32_t sig) const
    {
        if (!fc5::bow::upright_on() || fix_sig != sig || sig == 0 || (fix_from_kept && !fc5::bow::upright_keep())) return glm::mat3{ 1.0f };
        float h[3];
        fc5::bow::get_hold(h);
        const glm::mat3 adj{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-h[0]), glm::vec3{ 0, 1, 0 }) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(h[1]), glm::vec3{ 1, 0, 0 }) *
                             glm::rotate(glm::mat4{ 1.0f }, glm::radians(h[2]), glm::vec3{ 0, 0, -1 }) };
        const glm::mat3 flip = fc5::bow::flip_on() ? glm::mat3{ -1, 0, 0, 0, -1, 0, 0, 0, 1 } : glm::mat3{ 1.0f };   // bowflip: 180 deg about the arrow
        return adj * flip * fix_base;
    }
    std::set<ID3D11InputLayout*> unreadable;

    // bowpull: per bow, per draw (index count), its pose relative to the bow's main mesh at rest and at full draw (a skinned draw's 256
    // bones, a rigid draw's own transform), and the last sample (every 5 frames: two alike are a pose held still).
    struct PullPart
    {
        bool                   skinned{ false };
        bool                   has_rest{ false }, has_full{ false };
        std::vector<glm::mat4> rest, full, last;
        int                    last_frame{ -100 };
        float                  moves{ 0.0f };   // how far it moves from rest to full (m)
    };
    std::map<uint32_t, std::map<UINT, PullPart>> pull;
    // bowpin: each bow's sight-pin draw's middle in its object space (its vertices read once), and the tries.
    std::map<std::pair<uint32_t, UINT>, glm::vec3> pin_obj;
    std::map<std::pair<uint32_t, UINT>, int>       pin_tries;
    struct PullDiag { bool skinned{ false }; uint32_t b1{ 0 }; uint64_t seen{ 0 }, samples{ 0 }; float diff{ 0.0f }; std::string pal{ "-" }, why; };
    std::map<uint32_t, std::map<UINT, PullDiag>> pull_diag;
    std::map<uint32_t, int>                      pull_diag_said;   // shots logged per bow (at most 3)
    std::set<uint32_t> pull_said;
    bool               rt_was{ false };
    int                rt_frame{ -1000 }, rt_off_frame{ -1000 };   // RT held / let go (frames)
    int                ref_frame{ -1 };                            // the bow's main mesh this frame: its transform (world, and q)
    bool               ref_ok{ false };
    glm::mat4          ref_world{ 1.0f }, ref_q{ 1.0f };
    int                t_frame{ -1 };
    float              t{ 0.0f };
    bool               t_by_hand{ false };
    // bowsteadyarrow (FarCry5Bow.h): the string's draw (t) steadied, one step per stereo pair; per-draw sums for its log line.
    bool               ts_ok{ false };
    int                ts_frame{ -100 };
    float              ts{ 0.0f }, ts_v{ 0.0f }, ts_raw{ 0.0f };
    std::chrono::steady_clock::time_point ts_t{};
    int                ts_n{ 0 }, ts_open{ 0 };
    double             ts_raw_sum{ 0.0 }, ts_out_sum{ 0.0 };
    ID3D11Buffer*      pal_standin{};
    uint64_t           pulled{ 0 };
    // bowpullkeep (FarCry5Bow.h): samples of the game's own pose not taken as a rest or full, and the frame of the last log line about them.
    int                keep_refused{ 0 }, keep_early{ 0 };
    int                keep_said_frame{ -100000 };
    // Kept across sessions: bin\fc5vr_bowpull_default.txt (shipped with the mod), then bin\fc5vr_bowpull.txt (learnt here) over it. One line
    // per draw: "part <bow> <draw indices> <skinned 0|1> <n>", then n rest and n full poses (3x4, rows).
    std::set<uint32_t> pull_fresh;   // bows whose full draw was taken this session (saved once)
    std::set<uint32_t> pull_saved;
    std::set<uint32_t> pull_user;    // bows read from fc5vr_bowpull.txt
    void pull_load(bool user)
    {
        for (const char* name : { "fc5vr_bowpull_default.txt", "fc5vr_bowpull.txt" }) {
            const bool is_user = std::string{ name } == "fc5vr_bowpull.txt";
            if (is_user && !user) continue;
            std::ifstream    f{ fc5::mod_file(name) };
            std::string      line;
            std::set<uint32_t> bows;
            while (std::getline(f, line)) {
                std::istringstream ls{ line };
                std::string        tag;
                uint32_t           sig = 0, count = 0, n = 0;
                int                sk  = 0;
                if (!(ls >> tag >> sig >> count >> sk >> n) || tag != "part" || sig == 0 || n == 0 || n > 256) continue;
                std::vector<glm::mat4> pose[2];
                bool                   ok = true;
                for (auto& v : pose) {
                    v.resize(n);
                    for (auto& m : v) {
                        float rows[12];
                        for (float& x : rows) if (!(ls >> x)) ok = false;
                        if (!ok) break;
                        m = from_rows(rows);
                    }
                    if (!ok) break;
                }
                if (!ok) continue;
                if (!bow_count(count)) {
                    spdlog::info("[BowArrow] bowpull: draw {} of bow {} in {} dropped (the game's arms, not the bow's)", count, sig, name);
                    continue;
                }
                auto& pp    = pull[sig][count];
                pp.skinned  = sk != 0;
                pp.rest     = std::move(pose[0]);
                pp.full     = std::move(pose[1]);
                pp.has_rest = pp.has_full = true;
                pp.moves    = pose_diff(pp.rest, pp.full);
                bows.insert(sig);
                if (is_user) pull_user.insert(sig);
            }
            if (!bows.empty()) {
                std::string b;
                for (const uint32_t w : bows) b += (b.empty() ? "" : ", ") + std::to_string(w);
                spdlog::info("[BowArrow] bowpull: the rest and full draw of bow(s) {} read from {}", b, name);
            }
        }
    }
    void pull_save()
    {
        std::ofstream f{ fc5::mod_file("fc5vr_bowpull.txt"), std::ios::trunc };
        int           n = 0;
        for (const auto& [sig, parts] : pull) {
            if (!learnt(sig) || !(pull_fresh.contains(sig) || pull_user.contains(sig))) continue;
            for (const auto& [count, pp] : parts) {
                if (!bow_count(count)) continue;
                if (!pp.has_rest || !pp.has_full || pp.rest.size() != pp.full.size() || pp.rest.empty()) continue;
                f << "part " << sig << ' ' << count << ' ' << (pp.skinned ? 1 : 0) << ' ' << pp.rest.size();
                for (const auto* v : { &pp.rest, &pp.full })
                    for (const auto& m : *v) {
                        float rows[12];
                        to_rows(sane(m) ? m : glm::mat4{ 1.0f }, rows);
                        for (const float x : rows) f << ' ' << x;
                    }
                f << '\n';
                ++n;
            }
        }
        spdlog::info("[BowArrow] bowpull: {} bow draws' rest and full draw written to fc5vr_bowpull.txt", n);
    }

    bool learnt(uint32_t sig) const   // a skinned part (the string) moves between the two
    {
        const auto it = pull.find(sig);
        if (it == pull.end()) return false;
        for (const auto& [c, p] : it->second) if (bow_count(c) && p.skinned && p.has_rest && p.has_full && p.moves > 0.05f) return true;
        return false;
    }

    // Drawing.
    int           seen_frame{ -100 };   // the last frame an arrow draw came
    ID3D11Buffer* standin[2]{};         // object transform (b1) and previous (b3) for the arrow in the hand
    ID3D11Buffer* orig[4]{};            // VS b1..b4 as the game bound them
    uint64_t      moved{ 0 }, hidden{ 0 };
    int           last_shown_phase{ -1 };
    bool          origin_said{ false };

    std::set<std::tuple<UINT, UINT, INT>> scan_seen;

    // Before any shot: the bow's draws looked at one at a time for an arrow's shape (once each per bow).
    std::set<std::pair<uint32_t, UINT>> probed;
    int                                 probe_next{ 0 };
};

// The frame before this one is done: its draws judged (a shot's learning, the nock end's measurement), the drawn arrow's stand-in set.
void FarCry5Stereo::bow_arrow_frame(uint32_t sig)
{
    auto&      s  = *m_ba;
    const auto ph = fc5::bow::phase();
    if (sig != s.sig) {   // another bow (or none): its nock end measured again, at most 3 tries
        s.sig           = sig;
        s.limb_tries    = 0;
        s.tail_tries    = 0;
        s.tail_given_up = false;
        s.origin_said   = false;
    }
    // bowpull: RT's changes (frames), and whether the string follows the hand from now on.
    if (const bool rt = fc5::bow::rt_held(); rt != s.rt_was) {
        s.rt_was = rt;
        (rt ? s.rt_frame : s.rt_off_frame) = s.frame;
        // RT let go with nothing learnt: what each of the bow's draws gave (at most 3 shots per bow).
        if (!rt && fc5::bow::pull_on() && sig != 0 && !s.learnt(sig) && s.pull_diag_said[sig]++ < 3) {
            std::string txt;
            const auto& parts = s.pull[sig];
            for (const auto& [c, d] : s.pull_diag[sig]) {
                const auto  it = parts.find(c);
                const bool  pr = it != parts.end() && it->second.has_rest, pf = it != parts.end() && it->second.has_full;
                txt += fmt::format(" {}: {}, b1 {} B, palette {}, {} draws, {} samples, last change {}, rest {}, full {}, moves {:.1f} cm{};", c, d.skinned ? "skinned" : "rigid", d.b1, d.pal,
                                   d.seen, d.samples, d.diff < 1e8f ? fmt::format("{:.1f} mm", d.diff * 1000.0f) : std::string{ "-" }, pr ? "yes" : "no", pf ? "yes" : "no",
                                   it != parts.end() ? it->second.moves * 100.0f : 0.0f, d.why.empty() ? "" : ", " + d.why);
            }
            spdlog::info("[BowArrow] bow {}: bowpull: not learnt yet after RT held {} frames (main mesh {}):{}", sig, s.frame - s.rt_frame, s.ref_ok ? "seen" : "not seen",
                         txt.empty() ? " no draw of the bow came here" : txt);
        }
    }
    if (fc5::bow::pull_forget_take()) {
        s.pull.clear();
        s.pull_fresh.clear();
        s.pull_saved.clear();
        s.pull_user.clear();
        s.pull_said.clear();
        { std::ofstream f{ fc5::mod_file("fc5vr_bowpull.txt"), std::ios::trunc }; }
        s.pull_load(false);   // the shipped ones stay
    }
    if (sig != 0 && s.learnt(sig) && s.pull_fresh.contains(sig) && s.pull_saved.insert(sig).second) s.pull_save();
    {
        const bool learnt = fc5::bow::pull_on() && s.learnt(sig) && fc5::bow::in_hand();
        fc5::bow::set_pull(learnt, learnt && ph == fc5::bow::Phase::Drawing && s.t_frame == s.frame ? s.t : -1.0f);
        if (learnt && s.pull_said.insert(sig).second && fc5::bow::arrow_log()) {
            std::string parts;
            for (const auto& [c, p] : s.pull[sig])
                if (p.has_rest && p.has_full && p.moves > 0.003f) parts += fmt::format(" {}{} {:.0f} cm;", c, p.skinned ? " (skinned)" : "", p.moves * 100.0f);
            spdlog::info("[BowArrow] bow {}: bowpull: its rest and full draw learnt, the string follows the hand from now on (draws that move:{})", sig, parts);
        }
    }
    // Learning: the draws of the last frame with the string drawn, compared with the frames after the shot.
    if (s.watching) {
        if (sig != s.watch_sig || !fc5::bow::in_hand()) {
            s.watching = false;
            spdlog::info("[BowArrow] learning dropped: the bow left the hands {} frames after the shot", s.watch_frames);
        } else {
            for (const UINT c : s.before) if (!s.cur.contains(c)) s.miss[c]++;
            if (++s.watch_frames >= kWatchFrames) {
                s.watching = false;
                std::vector<uint32_t> parts;
                std::string           all;
                for (const UINT c : s.before) {
                    const int m = s.miss.contains(c) ? s.miss[c] : 0;
                    all += fmt::format(" {}:{}", c, m);
                    if (m >= kMissFrames && c != sig_mesh(sig) && c != 65535u) parts.push_back(c);
                }
                if (!parts.empty() && (int)parts.size() <= kMaxParts) {
                    const std::string line = fc5::bow::learn_arrow(sig, parts);
                    m_ui_cmds += line + "\n";   // saved, as the F11 buttons do
                    s.tail_sig      = 0;        // measured again
                    s.tail_tries    = 0;
                    s.tail_given_up = false;
                    std::string p;
                    for (const uint32_t c : parts) p += (p.empty() ? "" : ", ") + std::to_string(c);
                    spdlog::info("[BowArrow] bow {}: its arrow is the draw(s) of {} indices (gone after the shot; frames missing of {}:{}): {}", sig, p, kWatchFrames, all, line);
                } else {
                    spdlog::info("[BowArrow] bow {}: arrow not learnt at this shot: {} draws went away (frames missing of {}:{}); the drawn arrow stays. "
                                 "bowscan and bowarrowmesh {}:<indices> set it by hand",
                                 sig, parts.size(), kWatchFrames, all, sig);
                }
            }
        }
    }
    if (ph == fc5::bow::Phase::Drawing || ph == fc5::bow::Phase::Loose) {
        if (!s.cur.empty()) s.drawn = s.cur;
    }
    if (const uint32_t n = fc5::bow::shots(); n != s.shots_seen) {
        s.shots_seen = n;
        if (fc5::bow::arrow_model_on() && !fc5::bow::arrow_known(sig) && !s.drawn.empty() && !s.watching) {
            s.watching     = true;
            s.watch_sig    = sig;
            s.watch_frames = 0;
            s.before       = s.drawn;
            s.miss.clear();
            spdlog::info("[BowArrow] bow {}: shot {}, learning its arrow from the {} draws drawn with the string pulled ({})", sig, n, s.before.size(), counts_text(s.before));
        }
        s.drawn.clear();
    }
    // The nock end: the arrow's points of the measured frame, along their main line, its rear end (q: z back).
    if (s.meas_frame == s.frame && s.meas_frame >= 0) {
        s.meas_frame = -1;
        bool       ok = false, refused = false;
        const Line l  = fit_line(s.meas_pts);
        if (l.ok) {
            const float len = l.hi - l.lo;
            // bowuprightwait: an arrow not pointing straight ahead is not resting on the idle bow (still in the game's switch animation).
            const float off_deg = glm::degrees(std::acos(std::clamp(std::abs(l.ax.z), 0.0f, 1.0f)));
            if (len > 0.25f && len < 1.6f && fc5::bow::upright_wait() && off_deg > kRestArrowMaxDeg) {
                refused = true;
                if (s.rest_refused[sig]++ < 3) {
                    const glm::vec3 g = l.mean + l.ax * l.lo - m_hand_map.grip;
                    spdlog::info("[BowArrow] bow {}: bowuprightwait: the game's arrow {:.1f} deg from straight ahead (at most {:.0f}), its nock end ({:.1f}, {:.1f}, {:.1f}) cm "
                                 "from the bow's grip: not at rest on the bow (the game still moving it), measured again later",
                                 sig, off_deg, kRestArrowMaxDeg, g.x * 100.0f, g.y * 100.0f, g.z * 100.0f);
                }
            } else if (len > 0.25f && len < 1.6f) {
                const glm::vec3 tail_q = l.mean + l.ax * l.lo;
                const glm::vec3 tail_w = s.meas_cam + glm::transpose(s.meas_rot) * tail_q;
                s.tail.clear();
                for (const auto& [c, O] : s.meas_obj) s.tail[c] = glm::vec3{ glm::inverse(O) * glm::vec4{ tail_w, 1.0f } };
                s.tail_sig = sig;
                s.tail_len = len;
                ok         = true;
                s.line_sig  = sig;   // bowupright
                s.line_mean = s.meas_cr * l.mean + s.meas_ct;
                s.line_ax   = glm::normalize(s.meas_cr * l.ax);
                s.line_grip = s.meas_grip;
                s.fix_sig   = 0;
                const glm::vec3 g = tail_q - m_hand_map.grip;
                spdlog::info("[BowArrow] bow {}: the game's arrow measured: {} draws, {} points, {:.0f} cm long, {:.1f} cm thick, its nock end ({:.1f}, {:.1f}, {:.1f}) cm from the bow's grip "
                             "(right, up, back), pointing {:.1f} deg right {:.1f} deg up of straight ahead",
                             sig, s.meas_obj.size(), s.meas_pts.size(), len * 100.0f, l.thick * 100.0f, g.x * 100.0f, g.y * 100.0f, g.z * 100.0f,
                             glm::degrees(std::atan2(l.ax.x, -l.ax.z)), glm::degrees(std::asin(std::clamp(l.ax.y, -1.0f, 1.0f))));
            } else {
                spdlog::info("[BowArrow] bow {}: the arrow's {} points span {:.2f} m along their line: not an arrow's length, not used", sig, s.meas_pts.size(), len);
            }
        }
        if (!ok && !refused && ++s.tail_tries >= 3 && !s.tail_given_up) {
            s.tail_given_up = true;
            spdlog::info("[BowArrow] bow {}: the arrow's nock end not measured (3 tries): its draw's origin is put at the hand instead (bowarrowhand moves it)", sig);
        }
        s.meas_pts.clear();
        s.meas_obj.clear();
    }
    // bowupright: the bow's turn in the hand from its arrow's line (forward) and its limbs (up; signed from the arrow towards the grip). His 51543
    // in his 08:47 run: the arrow's nock end 7 cm to the left of the grip and level with it, so the game holds the bow canted about 90 deg (his
    // video: the bow flat across his fist). The grip is the hand bones' middle, and the game's arrow passes below it (09:20 run: 8 cm), so the
    // first rule (up towards the arrow) stood the bow upside down (author, 2 Nov 08:23 relay). bowflip turns it a further 180 deg if a bow needs it.
    s.fix_use_kept(sig);   // bowuprightkeep: until measured here (and while a new measurement waits for the limbs), the kept turn
    if (s.line_sig == sig && (s.limb_sig == sig || s.limb_tries >= 3) && (s.fix_sig != sig || s.fix_from_kept)) {
        const glm::vec3 a  = glm::normalize(s.line_ax);
        const glm::vec3 p  = s.line_mean + a * glm::dot(s.line_grip - s.line_mean, a);   // the arrow's point nearest the grip
        glm::vec3       v  = p - s.line_grip;
        v                 -= a * glm::dot(v, a);
        glm::vec3       up = s.limb_sig == sig ? s.limb - a * glm::dot(s.limb, a) : v;
        if (glm::length(up) < 1e-3f) up = v;
        if (glm::length(up) < 1e-3f) up = glm::vec3{ 0.0f, 1.0f, 0.0f } - a * a.y;
        up = glm::normalize(up);
        if (glm::dot(up, v) > 0.0f) up = -up;   // the limbs' up points away from the arrow, past the hand
        const glm::vec3 z = -a, x = glm::normalize(glm::cross(up, z));
        s.fix_base        = glm::transpose(glm::mat3{ x, glm::cross(z, x), z });
        s.fix_sig         = sig;
        s.fix_from_kept   = false;
        spdlog::info("[BowArrow] bow {}: bowupright: the game holds it canted {:.0f} deg (its up {:.0f} deg from the camera's), its arrow {:.1f} deg from straight ahead, "
                     "resting {:.1f} cm from the grip; turned upright in the hand{}",
                     sig, glm::degrees(std::atan2(-up.x, up.y)), glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f))),
                     glm::degrees(std::acos(std::clamp(-a.z, -1.0f, 1.0f))), glm::length(v) * 100.0f, s.limb_sig == sig ? " (limbs measured)" : " (up from the arrow to the grip)");
        // bowuprightkeep: kept for the next sessions (written when new or changed: an entry more than 0.02 apart).
        if (fc5::bow::upright_keep()) {
            const auto it    = s.fix_kept.find(sig);
            const bool fresh = it == s.fix_kept.end();
            float      diff  = fresh ? 1.0f : 0.0f;
            if (!fresh) for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) diff = std::max(diff, std::abs(it->second[c][r] - s.fix_base[c][r]));
            if (diff > 0.02f) {
                s.fix_kept[sig] = s.fix_base;
                s.fix_save();
                spdlog::info("[BowArrow] bow {}: bowuprightkeep: its upright turn {} fc5vr_bowupright.txt", sig, fresh ? "written to" : fmt::format("changed ({:.2f} apart) in", diff));
            }
        }
    }
    m_bow_fix     = s.fix_now(sig);
    m_bow_fix_sig = sig;
    // bowuprightkeep: the bow's grip on the sling (the game's left hand on it, recorded in the hand), kept once per session once its turn is measured.
    if (fc5::bow::upright_keep() && sig != 0 && s.fix_sig == sig && !s.fix_from_kept && m_bow_sling_hand_sig == sig && !s.hand_saved.contains(sig)) {
        s.hand_saved.insert(sig);
        const auto it = s.hand_kept.find(sig);
        if (it == s.hand_kept.end() || glm::length(it->second - m_bow_sling_hand) > 0.01f) {
            s.hand_kept[sig] = m_bow_sling_hand;
            s.fix_save();
            spdlog::info("[BowArrow] bow {}: bowuprightkeep: its grip on the sling ({:.1f}, {:.1f}, {:.1f}) cm kept in fc5vr_bowupright.txt", sig, m_bow_sling_hand.x * 100.0f,
                         m_bow_sling_hand.y * 100.0f, m_bow_sling_hand.z * 100.0f);
        }
    }
    // The drawn arrow left out once the game's stands for it: its draws known, and in the hand only while the game draws them.
    const auto ap    = fc5::bow::arrow_phase();
    const bool known = fc5::bow::arrow_model_on() && fc5::bow::arrow_known(sig);
    fc5::bow::set_arrow_model_shown(known && (ap != fc5::bow::Phase::Arrow || s.frame - s.seen_frame <= 5));
    if (known && fc5::bow::arrow_log() && int(ap) != s.last_shown_phase) {
        s.last_shown_phase = int(ap);
        const char* what   = ap == fc5::bow::Phase::Empty ? "not drawn (no arrow taken)"
                             : ap == fc5::bow::Phase::Arrow ? "in the string hand"
                                                            : "the game's own, on its bow";
        spdlog::info("[BowArrow] the game's arrow: {} (moved {} draws, hidden {} so far)", what, s.moved, s.hidden);
    }
    s.cur.clear();
}

// bowsling + bowuprightkeep: the bow on the sling (its weapon view, each pass). bow_arrow_frame runs only with the bow in hand, so a bow slung from
// the load had no upright turn (it hung as the game holds it, canted onto its side) until first held: the kept turn is used meanwhile.
void FarCry5Stereo::bow_sling_fix(uint32_t sig)
{
    if (!m_ba) {
        m_ba = new BowArrowState{};
        m_ba->pull_load(true);
    }
    auto& s = *m_ba;
    s.fix_use_kept(sig);
    m_bow_fix     = s.fix_now(sig);
    m_bow_fix_sig = sig;
    // The bow's grip on the sling: the kept one until the bow has been in the hand this session.
    if (sig != 0 && m_bow_sling_hand_sig != sig && fc5::bow::upright_keep()) {
        if (const auto it = s.hand_kept.find(sig); it != s.hand_kept.end()) {
            m_bow_sling_hand     = it->second;
            m_bow_sling_hand_sig = sig;
            if (s.hand_kept_said.insert(sig).second)
                spdlog::info("[BowArrow] bow {}: bowuprightkeep: hung on the sling by its grip as measured in an earlier session ({:.1f}, {:.1f}, {:.1f}) cm", sig,
                             it->second.x * 100.0f, it->second.y * 100.0f, it->second.z * 100.0f);
        }
    }
}

// A non-glove weapon-pass draw with manualbow's bow in hand. True: not to be drawn.
bool FarCry5Stereo::bow_arrow_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_ba) {
        m_ba = new BowArrowState{};
        m_ba->pull_load(true);   // bowpull: the draws kept from earlier sessions
    }
    auto&          s     = *m_ba;
    const int      frame = m_frame.load();
    const uint32_t sig   = m_weapon_sig.load();
    if (frame != s.frame) {
        if (s.frame >= 0) bow_arrow_frame(sig);
        s.frame = frame;
    }
    // bowuprightwait: how long this bow has been out (its draws coming each frame), so nothing is measured in the game's switch animation.
    {
        const auto now = std::chrono::steady_clock::now();
        if (sig != s.out_sig || frame - s.out_last > kSettleGapFrames) {
            s.out_sig = sig;
            s.out_t   = now;
        }
        s.out_last = frame;
        s.settled  = !fc5::bow::upright_wait() || std::chrono::duration<float>(now - s.out_t).count() >= kSettleSecs;
    }
    if (!fc5::bow::in_hand() || in_vehicle() || flat_screen_on() || m_cine_on) return false;
    const bool again = s.cur.contains(count);   // the same draw again this frame (a second pass)
    s.cur.insert(count);
    const auto& hm    = m_hand_map;
    const bool  fresh = frame - hm.frame <= 1 && m_mo.valid;
    // The object transform (b1) and whether a palette is bound (skinned).
    ID3D11Buffer* cb[5]{};
    ctx->VSGetConstantBuffers(0, 5, cb);
    float    obj[32]{};
    bool     have     = false;
    uint32_t obj_size = 0;   // bowpull: b1's size whatever it is (a skinned part's may not be 128 bytes)
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end(); it != m_obj_consts.end()) {
            obj_size = it->second.size;
            std::memcpy(obj, it->second.f, std::min<size_t>(sizeof(obj), it->second.size));
            have = it->second.size == 128;
        }
    }
    bool skinned = false;
    if (cb[3]) {
        D3D11_BUFFER_DESC d{};
        cb[3]->GetDesc(&d);
        skinned = d.ByteWidth == kPaletteBytes;
    }
    const bool part = fc5::bow::arrow_model_on() && fc5::bow::arrow_part(sig, count);
    // bowscan (test): each draw once.
    if (fc5::bow::scanning() && s.scan_seen.insert({ count, start, base }).second) {
        std::string at = "no rigid transform at b1";
        if (have && fresh) {
            const glm::mat4 O = from_rows(obj);
            const glm::vec3 q = hm.rot * (glm::vec3{ O[3] } - hm.cam) - hm.grip;
            at                = fmt::format("origin ({:.1f}, {:.1f}, {:.1f}) cm from the bow's grip", q.x * 100.0f, q.y * 100.0f, q.z * 100.0f);
        }
        spdlog::info("[BowArrow] scan: bow {} draw {} indices from {} base {}, {}, {}{}{}, phase {}", sig, count, start, base, skinned ? "skinned" : "rigid", at,
                     count == sig_mesh(sig) ? ", the bow's main mesh" : "", part ? ", the arrow (learnt)" : "", fc5::bow::arrow_known(sig) ? "" : " (no arrow learnt yet)",
                     int(fc5::bow::phase()));
    }
    auto done = [&](bool r) {
        for (auto* b : cb) if (b) b->Release();
        return r;
    };
    // This draw's points (its vertices, skinned by its palette, through its object transform), q of this pass. Immediate context only.
    std::string why_not;
    auto read_points = [&](std::vector<glm::vec3>& out) -> bool {
        bool               read = false;
        ID3D11InputLayout* il   = nullptr;
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
        if (off("POSITION", DXGI_FORMAT_R16G16B16A16_SINT) == 0 && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && vb && ib && stride >= 8 && stride <= 128) {
            std::vector<uint8_t> vbd, ibd, cb2, pal;
            if (read_gpu(ctx, vb, vbd) && read_gpu(ctx, ib, ibd) && read_gpu(ctx, cb[2], cb2) && cb2.size() >= 24) {
                float scale = 0.0f;
                std::memcpy(&scale, cb2.data() + 20, 4);
                if (scale > 1e-7f && scale < 1e-2f) {
                    if (skinned) read_gpu(ctx, cb[3], pal);
                    const int       bi_off = off("BLENDINDICES", DXGI_FORMAT_UNKNOWN);
                    const bool      i32    = ib_fmt == DXGI_FORMAT_R32_UINT;
                    const glm::mat4 O      = from_rows(obj);
                    for (UINT k = 0; k < count; ++k) {
                        const size_t io = size_t(ib_off) + size_t(start + k) * (i32 ? 4 : 2);
                        if (io + (i32 ? 4 : 2) > ibd.size()) break;
                        uint32_t idx = 0;
                        std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
                        const size_t vo = size_t(voff) + size_t(int64_t(idx) + base) * stride;
                        if (vo + stride > vbd.size()) continue;
                        const uint8_t* v = vbd.data() + vo;
                        int16_t        qv[3];
                        std::memcpy(qv, v, 6);
                        glm::vec4 p{ float(qv[0]) * scale, float(qv[1]) * scale, float(qv[2]) * scale, 1.0f };
                        if (!pal.empty() && bi_off >= 0) {
                            const int b = v[bi_off];
                            if ((b + 1) * 48 <= (int)pal.size()) p = glm::vec4{ glm::vec3{ from_rows(reinterpret_cast<const float*>(pal.data() + b * 48)) * p }, 1.0f };
                        }
                        out.push_back(hm.rot * (glm::vec3{ O * p } - hm.cam));
                    }
                    read = true;
                }
            }
        }
        if (!read) {
            why_not.clear();
            for (const auto& e : layout) why_not += fmt::format(" {}{}@{}:{}", e.semantic, e.index, e.offset, int(e.format));
            why_not = fmt::format("stride {}, topology {}:{}", stride, int(topo), why_not);
            if (!s.unreadable.insert(il).second) why_not.clear();   // said once per layout
        }
        if (il) il->Release();
        if (vb) vb->Release();
        if (ib) ib->Release();
        return read;
    };
    // bowupright: the bow's limbs, from its main mesh's points (their longest line), once per bow while it is held idle.
    if (count == sig_mesh(sig) && s.limb_sig != sig && s.limb_tries < 3 && s.settled && fresh && have && !again && !fc5::rec::is_deferred(ctx) && frame % 30 == 15 &&
        fc5::bow::phase() != fc5::bow::Phase::Drawing && fc5::bow::phase() != fc5::bow::Phase::Loose) {
        std::vector<glm::vec3> pts;
        bool                   ok = false;
        if (read_points(pts)) {
            for (auto& p : pts) p = hm.cr * p + hm.ct;   // steadied
            const Line l = fit_line(pts);
            if (l.ok && l.hi - l.lo > 0.4f) {
                s.limb     = l.ax;
                s.limb_sig = sig;
                s.fix_sig  = 0;
                ok         = true;
                spdlog::info("[BowArrow] bow {}: its limbs measured: {:.0f} cm tip to tip, along ({:.2f}, {:.2f}, {:.2f}) of the game camera's axes (right, up, back)", sig,
                             (l.hi - l.lo) * 100.0f, l.ax.x, l.ax.y, l.ax.z);
            }
        } else if (!why_not.empty()) {
            spdlog::info("[BowArrow] bow {}: its main mesh not readable for its limbs ({})", sig, why_not);
        }
        if (!ok && ++s.limb_tries >= 3) spdlog::info("[BowArrow] bow {}: its limbs not measured: the bow's up taken from its grip to the arrow", sig);
    }
    // Before any shot with this bow (author 2 Nov 07:51): its draws looked at one every 20 frames
    // for an arrow's shape: a straight thin mesh 0.5-1.1 m long (95 % of its points within 3.5 cm of its line) pointing within 20 deg of ahead.
    // His bow 51543's arrow (3693 indices): 80 cm, 1.7 deg right, 7.3 deg down. A bow's string or cables stand up, its limbs are curved.
    {
        const auto rp = fc5::bow::phase();
        if (!part && fc5::bow::arrow_model_on() && !fc5::bow::arrow_known(sig) && !s.watching && fresh && have && !again && !fc5::rec::is_deferred(ctx) &&
            count >= 60 && count != sig_mesh(sig) && count != 65535u && frame >= s.probe_next && rp != fc5::bow::Phase::Drawing && rp != fc5::bow::Phase::Loose &&
            s.probed.insert({ sig, count }).second) {
            s.probe_next = frame + 20;
            std::vector<glm::vec3> pts;
            if (read_points(pts)) {
                const Line  l     = fit_line(pts);
                const float len   = l.hi - l.lo;
                const bool  arrow = l.ok && len > 0.5f && len < 1.1f && l.ax.z < -0.94f && l.thick < 0.035f;
                spdlog::info("[BowArrow] bow {}: draw {} indices looked at for an arrow's shape: {:.0f} cm long, {:.1f} cm thick, pointing {:.0f} deg from ahead: {}", sig, count,
                             len * 100.0f, l.thick * 100.0f, glm::degrees(std::acos(std::clamp(-l.ax.z, -1.0f, 1.0f))), arrow ? "the arrow" : "not the arrow");
                if (arrow) {
                    const std::string line = fc5::bow::learn_arrow(sig, { count });
                    m_ui_cmds += line + "\n";   // saved, as the F11 buttons do
                    s.tail_sig      = 0;
                    s.tail_tries    = 0;
                    s.tail_given_up = false;
                    spdlog::info("[BowArrow] bow {}: its arrow is the draw of {} indices (by its shape, before any shot): {}", sig, count, line);
                }
            } else if (!why_not.empty()) {
                spdlog::info("[BowArrow] bow {}: draw {} indices not readable for its shape ({})", sig, count, why_not);
            }
        }
    }
    if (const int r = bow_pull_draw(ctx, count, skinned, part, obj_size, fresh, again, obj, cb); r >= 0) return done(r != 0);
    if (!part) return done(false);
    const auto ap = fc5::bow::arrow_phase();
    if (ap != fc5::bow::Phase::Arrow) s.seen_frame = frame;
    // The nock end, measured from this draw's vertices while the arrow rests on the idle bow (once per bow).
    const bool idle = ap == fc5::bow::Phase::Empty || ap == fc5::bow::Phase::Arrow || ap == fc5::bow::Phase::Nocked;
    const bool can_measure = s.tail_sig != sig && !s.tail_given_up && idle && s.settled && fresh && have && !again && !fc5::rec::is_deferred(ctx);   // (bowuprightwait)
    if (can_measure && (s.meas_frame == frame || (s.meas_frame < 0 && frame % 30 == 0))) {
        if (s.meas_frame != frame) {
            s.meas_frame = frame;
            s.meas_pts.clear();
            s.meas_obj.clear();
            s.meas_rot  = hm.rot;
            s.meas_cam  = hm.cam;
            s.meas_cr   = hm.cr;
            s.meas_ct   = hm.ct;
            s.meas_grip = hm.grip0;
        }
        if (read_points(s.meas_pts)) {
            s.meas_obj[count] = from_rows(obj);
        } else {
            if (!why_not.empty()) spdlog::info("[BowArrow] bow {}: the arrow's draw ({} indices) not readable ({})", sig, count, why_not);
            if (s.meas_obj.empty()) s.meas_frame = -1;
            if (s.meas_obj.empty() && ++s.tail_tries >= 3 && !s.tail_given_up) {
                s.tail_given_up = true;
                spdlog::info("[BowArrow] bow {}: the arrow's nock end not measured (3 tries): its draw's origin is put at the hand instead (bowarrowhand moves it)", sig);
            }
        }
    }
    if (ap == fc5::bow::Phase::Empty) {
        s.hidden++;
        return done(true);   // no arrow taken: none on the bow
    }
    if (ap != fc5::bow::Phase::Arrow) return done(false);   // on the bow: the game's own
    // In the string hand: the arrow moved so its nock end sits at the hand, turned with the string hand's controller as it sits on the bow
    // with the bow hand's (the weapon's axes: world = w_pos + w_rot (q - grip), w_rot = controller * corr * Cr).
    if (!have || !fresh || m_hand_cb_swapped || m_th_cb_swapped || !m_mo.bow) return done(true);   // cannot be placed: not drawn on the bow either
    const glm::mat4 O = from_rows(obj);
    glm::vec3       F;   // the nock end, q
    if (const auto it = s.tail.find(count); s.tail_sig == sig && it != s.tail.end()) {
        F = hm.rot * (glm::vec3{ O * glm::vec4{ it->second, 1.0f } } - hm.cam);
    } else {
        F = hm.rot * (glm::vec3{ O[3] } - hm.cam);
        if (!s.origin_said) {
            s.origin_said = true;
            spdlog::info("[BowArrow] bow {}: the arrow in the hand placed by its draw's origin (its nock end not measured yet)", sig);
        }
    }
    float off[3];
    fc5::bow::get_arrow_hand(off);
    if (m_left_handed.load()) off[0] = -off[0];
    const glm::mat3 Hc    = m_mo.hand_rot_world;
    const glm::vec3 place = m_mo.hand_pos_world + Hc * glm::vec3{ off[0], off[1], off[2] };
    const glm::mat3 LR    = Hc * hm.corr * hm.fix * hm.cr;   // as the bow's own axes (bowupright's turn included)
    const glm::mat3 rT    = glm::transpose(hm.rot);
    const glm::mat3 WT    = glm::transpose(hm.w_rot);
    const glm::mat3 G     = rT * WT * LR * hm.rot;
    const glm::vec3 c     = hm.cam + rT * (hm.grip + WT * (place - hm.w_pos)) - rT * WT * LR * F;
    glm::mat4       A{ G };
    A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
    float o1[32];
    std::memcpy(o1, obj, sizeof(o1));
    to_rows(A * O, o1);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    for (auto*& b : s.standin) {
        if (b || !dev) continue;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 128;
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &b);
    }
    if (dev) dev->Release();
    if (!s.standin[0] || !s.standin[1]) return done(true);
    ctx->VSGetConstantBuffers(1, 4, s.orig);
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[0]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
    // A rigid draw's previous transform (b3) the same: no motion of its own (as the moved gloves); a skinned draw keeps its palettes.
    if (!skinned) call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[1]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
    ID3D11Buffer* b[3]{ s.standin[0], s.orig[1], skinned ? s.orig[2] : s.standin[1] };
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(b));
    m_ba_cb_swapped   = true;
    m_hand_cb_swapped = true;   // the draw hooks restore through hands_cb_restore
    s.seen_frame      = frame;
    if (s.moved++ % 300 == 0)
        spdlog::info("[BowArrow] the game's arrow ({} indices{}) in the string hand at ({:.2f}, {:.2f}, {:.2f}){}", count, skinned ? ", skinned" : "", place.x, place.y, place.z,
                     s.tail_sig == sig ? fmt::format(", its nock end there ({:.0f} cm arrow)", s.tail_len * 100.0f) : std::string{ ", by its origin" });
    return done(false);
}

// bowpull: a weapon-pass draw of the bow (from bow_arrow_draw): sampled for the rest and full draw, and while the string is held posed between
// them. -1: not concerned (carry on), 0: to be drawn (its constants swapped if posed), 1: not to be drawn.
int FarCry5Stereo::bow_pull_draw(ID3D11DeviceContext* ctx, UINT count, bool skinned, bool part, uint32_t obj_size, bool fresh, bool again, const float* obj, ID3D11Buffer* const* cb)
{
    auto&          s     = *m_ba;
    const int      frame = m_frame.load();
    const uint32_t sig   = m_weapon_sig.load();
    if (!fc5::bow::pull_on() || sig == 0) return -1;
    auto& dg   = s.pull_diag[sig][count];   // for the log at a shot while nothing is learnt
    dg.skinned = skinned;
    dg.b1      = obj_size;
    dg.seen++;
    if (!bow_count(count) && count != sig_mesh(sig)) { dg.why = "not the bow's (the game's arms)"; return -1; }
    if (obj_size < 48) { dg.why = "no object transform at b1"; return -1; }
    if (!fresh) { dg.why = "the hands' placement not fresh"; return -1; }
    const auto&     hm = m_hand_map;
    const auto      ph = fc5::bow::phase();
    glm::mat4       V{ hm.rot };   // game world -> q (the game camera's axes)
    V[3]               = glm::vec4{ -(hm.rot * hm.cam), 1.0f };
    const glm::mat4 O  = from_rows(obj);
    if (count == sig_mesh(sig)) {
        s.ref_world = O;
        s.ref_q     = V * O;
        s.ref_frame = frame;
        s.ref_ok    = true;
    }
    if (!s.ref_ok) { dg.why = "the bow's main mesh not seen yet"; return -1; }
    // The bow's main mesh now: this frame's, else last frame's where it sat from the game camera.
    const glm::mat4 Oref  = s.ref_frame == frame ? s.ref_world : glm::inverse(V) * s.ref_q;
    const glm::mat4 K     = glm::inverse(Oref) * O;   // this draw's transform from the main mesh's
    auto&           parts = s.pull[sig];
    // A sample every 5 frames; two alike in a row: held still.
    if (!again && frame % 5 == 0) {
        std::vector<glm::mat4> cur;
        if (skinned) {
            // The palette as the game wrote it (kept on the CPU), else read back from the GPU (immediate context only).
            m_keep_bones = true;
            {
                std::lock_guard _{ m_obj_mtx };
                if (auto it = cb[3] ? m_bone_data.find(cb[3]) : m_bone_data.end(); it != m_bone_data.end() && it->second.size() >= 3072) {
                    cur.resize(256);
                    for (int b = 0; b < 256; ++b) cur[b] = K * from_rows(it->second.data() + b * 12);
                    dg.pal = "kept";
                }
            }
            std::vector<uint8_t> raw;
            if (cur.empty() && !fc5::rec::is_deferred(ctx) && read_gpu(ctx, cb[3], raw) && raw.size() >= kPaletteBytes) {
                cur.resize(256);
                for (int b = 0; b < 256; ++b) cur[b] = K * from_rows(reinterpret_cast<const float*>(raw.data() + b * 48));
                dg.pal = "read back";
            }
            if (cur.empty()) dg.pal = "not readable";
        } else {
            cur.push_back(K);
        }
        if (!cur.empty()) {
            auto&       pp     = parts[count];
            const float diff   = pp.last_frame == frame - 5 && pp.skinned == skinned ? pose_diff(pp.last, cur) : 1e9f;
            const bool  stable = diff < 0.005f;
            dg.diff            = diff;
            dg.samples++;
            pp.skinned         = skinned;
            pp.last_frame      = frame;
            const bool rt      = fc5::bow::rt_held();
            // Rest: RT up a while (a new arrow on the bow), held still, or the first such sample at all. Full: RT held, held still,
            // or the first sample once RT has been held 90 frames (the game's draw done).
            // bowpullkeep (FarCry5Bow.h; author 6 Nov 14:02 video, 0:07-0:09, his 14:44:48 draw): "stable 15 frames after RT" also caught the
            // game's bow before its own draw had started, so the full became the rest; the arrow's part then moved under 3 mm, was not posed,
            // and the game's own draw animation showed (the arrow crept back past the hand to the game's full draw, then snapped forward to the
            // hand when a still sample at the game's full draw put the full right again). On: a known full is taken again only while drawing
            // with RT held 90 frames, and a rigid part's (the arrow's) rest or full only at least 80 % of its known draw from the other end.
            const bool keep    = fc5::bow::pull_keep();
            const bool rigid_k = keep && !skinned && pp.has_rest && pp.has_full && pp.moves > 0.003f && pp.rest.size() == cur.size() && pp.full.size() == cur.size();
            const bool full_at = keep ? frame - s.rt_frame > 90 && (ph == fc5::bow::Phase::Drawing || !pp.has_full) && (stable || !pp.has_full)
                                      : (stable && frame - s.rt_frame > 15) || (!pp.has_full && frame - s.rt_frame > 90);
            if (!rt && frame - s.rt_off_frame > 120 && ph != fc5::bow::Phase::Loose && ph != fc5::bow::Phase::Off && (stable || !pp.has_rest)) {
                if (rigid_k && pose_diff(cur, pp.full) < 0.8f * pp.moves) {
                    s.keep_refused++;
                } else {
                    pp.rest     = cur;
                    pp.has_rest = true;
                }
            } else if (rt && full_at) {
                if (rigid_k && pose_diff(cur, pp.rest) < 0.8f * pp.moves) {
                    s.keep_refused++;
                } else {
                    pp.full     = cur;
                    pp.has_full = true;
                    s.pull_fresh.insert(sig);   // saved once it is learnt
                }
            } else if (rt && keep && stable && pp.has_full && frame - s.rt_frame > 15 && frame - s.rt_frame <= 90) {
                s.keep_early++;   // the old rule would have taken this one as the full
            }
            if ((s.keep_refused > 0 || s.keep_early > 0) && frame - s.keep_said_frame > 450 && fc5::bow::arrow_log()) {
                s.keep_said_frame = frame;
                spdlog::info("[BowArrow] bow {}: bowpullkeep: {} still samples of the game's own draw within 90 frames of RT not taken as the full draw, {} "
                             "arrow samples refused (under 80 % of its known draw from the other end)", sig, s.keep_early, s.keep_refused);
                s.keep_refused = s.keep_early = 0;
            }
            if (pp.has_rest && pp.has_full) pp.moves = pose_diff(pp.rest, pp.full);
            pp.last = std::move(cur);
        }
    }
    dg.why = "";
    if (!(ph == fc5::bow::Phase::Drawing || ph == fc5::bow::Phase::Loose) || !s.learnt(sig) || !m_mo.bow || m_hand_cb_swapped || m_th_cb_swapped) return -1;
    if (part && ph == fc5::bow::Phase::Loose) return 1;   // let go: the arrow is gone from the bow
    const auto it = parts.find(count);
    if (it == parts.end()) return -1;
    const auto& pp = it->second;
    if (!pp.has_rest || !pp.has_full || pp.skinned != skinned || pp.moves < 0.003f || pp.rest.size() != pp.full.size()) return -1;
    // How far the string is drawn: the arrow's nock end at the string hand (its nock end's path from rest to full draw, the hand put on
    // it), else the hand's pull from where it took the string to bowfull.
    if (s.t_frame != frame) {
        float t     = fc5::bow::pull_frac();
        s.t_by_hand = false;
        if (s.tail_sig == sig) {
            for (const auto& [c, tl] : s.tail) {
                const auto at = parts.find(c);
                if (at == parts.end() || at->second.skinned || !at->second.has_rest || !at->second.has_full) continue;
                const glm::vec3 nr = glm::vec3{ s.ref_q * at->second.rest[0] * glm::vec4{ tl, 1.0f } };
                const glm::vec3 nf = glm::vec3{ s.ref_q * at->second.full[0] * glm::vec4{ tl, 1.0f } };
                float off[3];
                fc5::bow::get_string_hand(off);   // bowstringhand: at the fingers
                if (m_left_handed.load()) off[0] = -off[0];
                const glm::vec3 place = m_mo.hand_pos_world + m_mo.hand_rot_world * glm::vec3{ off[0], off[1], off[2] };
                const glm::vec3 h     = hm.grip + glm::transpose(hm.w_rot) * (place - hm.w_pos);   // the hand in q
                const float     L2    = glm::dot(nf - nr, nf - nr);
                if (L2 > 0.05f * 0.05f) {
                    t           = std::clamp(glm::dot(h - nr, nf - nr) / L2, 0.0f, 1.0f);
                    s.t_by_hand = true;
                }
                break;
            }
        }
        // bowsteadyarrow (author 5 Nov 22:47):
        // the bow is steadied (bowsteady) but the arrow's nock end followed the string hand as read, so the hand's shake (and the bow hand's,
        // now steadied away from the bow) slid the arrow and the string back and forth every frame. The draw goes through a steadier like the
        // bow's: bowsteadyhz at rest, opened by a deliberate pull or let-down (past 0.15 of a full draw per second, averaged at 3 Hz so shake
        // cancels) or once it trails by more than 3 % of the draw; one step per stereo pair, so both eyes see one arrow.
        const bool tsteady = fc5::bow::steady_arrow() && ph == fc5::bow::Phase::Drawing;
        if (tsteady) {
            const auto now   = std::chrono::steady_clock::now();
            const bool pairs = m_pair_body.load() && m_frame_double.load() && m_stereo2.load();
            if (!s.ts_ok || frame - s.ts_frame > 6) {
                s.ts_ok      = true;
                s.ts         = t;
                s.ts_v       = 0.0f;
                s.ts_raw     = t;
                s.ts_t       = now;
                s.ts_n       = 0;
                s.ts_open    = 0;
                s.ts_raw_sum = s.ts_out_sum = 0.0;
            } else if (!pairs || (frame & 1) == 0) {
                const float dt   = std::clamp(std::chrono::duration<float>(now - s.ts_t).count(), 0.001f, 0.1f);
                s.ts_v          += (1.0f - std::exp(-6.2831853f * 3.0f * dt)) * ((t - s.ts_raw) / dt - s.ts_v);
                const float hz   = std::min(30.0f, fc5::bow::steady_hz() + 6.0f * std::max(0.0f, std::abs(s.ts_v) - 0.15f) + 30.0f * std::max(0.0f, std::abs(t - s.ts) - 0.03f));
                const float prev = s.ts;
                s.ts            += (1.0f - std::exp(-6.2831853f * hz * dt)) * (t - s.ts);
                s.ts_raw_sum    += std::abs(t - s.ts_raw);
                s.ts_out_sum    += std::abs(s.ts - prev);
                s.ts_n++;
                if (hz > fc5::bow::steady_hz() + 1.0f) s.ts_open++;
                s.ts_raw = t;
                s.ts_t   = now;
            }
            s.ts_frame = frame;
            t          = s.ts;
        } else if (s.ts_ok && ph != fc5::bow::Phase::Drawing) {
            s.ts_ok = false;
            if (s.ts_n > 0 && fc5::bow::arrow_log())
                spdlog::info("[BowArrow] bow {}: bowsteadyarrow: a draw of {} steps: the string's draw moved {:.2f} % of a full draw per step as read, {:.2f} % steadied; "
                             "opened for a pull in {:.0f} % of steps", sig, s.ts_n, 100.0 * s.ts_raw_sum / s.ts_n, 100.0 * s.ts_out_sum / s.ts_n, 100.0 * s.ts_open / s.ts_n);
        }
        if (ph == fc5::bow::Phase::Loose) t = 0.0f;   // let go: the string back at rest
        s.t       = t;
        s.t_frame = frame;
    }
    const float   t   = s.t;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        D3D11_BUFFER_DESC d{};
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!s.pal_standin) { d.ByteWidth = kPaletteBytes; dev->CreateBuffer(&d, nullptr, &s.pal_standin); }
        d.ByteWidth = 128;
        for (auto*& b : s.standin) if (!b) dev->CreateBuffer(&d, nullptr, &b);
        dev->Release();
    }
    if (skinned) {
        if (!s.pal_standin) return -1;
        // Each bone: its pose from the main mesh, blended, back into this draw's object space.
        const glm::mat4    Ki = glm::inverse(K);
        std::vector<float> pal(kPaletteBytes / 4);
        for (size_t b = 0; b < pp.rest.size() && b < 256; ++b) to_rows(Ki * blend_m(pp.rest[b], pp.full[b], t), pal.data() + b * 12);
        ctx->VSGetConstantBuffers(1, 4, s.orig);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.pal_standin), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(pal.data()), 0u, 0u);
        ID3D11Buffer* b[2]{ s.pal_standin, s.pal_standin };   // its palette and its previous one (no motion of its own)
        call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(b));
    } else {
        if (!s.standin[0] || !s.standin[1] || obj_size != 128) return -1;
        float o1[32];
        std::memcpy(o1, obj, sizeof(o1));
        to_rows(Oref * blend_m(pp.rest[0], pp.full[0], t), o1);
        ctx->VSGetConstantBuffers(1, 4, s.orig);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[0]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(s.standin[1]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
        ID3D11Buffer* b[3]{ s.standin[0], s.orig[1], s.standin[1] };
        call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(b));
    }
    m_ba_cb_swapped   = true;
    m_hand_cb_swapped = true;   // the draw hooks restore through hands_cb_restore
    if (s.pulled++ % 600 == 0 && fc5::bow::arrow_log())
        spdlog::info("[BowArrow] bow {}: bowpull: draw {} posed {:.0f} % of the way to full draw ({})", sig, count, t * 100.0f,
                     s.t_by_hand ? "the arrow's nock end at the string hand" : "from the hand's pull to bowfull");
    return 0;
}

// bowpin: a reticle draw (the bow's sight pin glow) of the weapon pass with manualbow's bow in hand: its middle (its vertices' mean,
// object space, read once per bow and draw), this frame in the game camera's axes for reddot_draw, which draws a dot there in each eye.
void FarCry5Stereo::bow_pin_note(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!fc5::bow::pin_on() && !fc5::bow::sight_aim()) return;
    if (!m_ba) {
        m_ba = new BowArrowState{};
        m_ba->pull_load(true);
    }
    auto&          s   = *m_ba;
    const uint32_t sig = m_weapon_sig.load();
    const auto&    hm  = m_hand_map;
    if (sig == 0 || m_frame.load() - hm.frame > 1) return;
    ID3D11Buffer* cb[4]{};
    ctx->VSGetConstantBuffers(0, 4, cb);
    auto done = [&] { for (auto* b : cb) if (b) b->Release(); };
    float obj[12]{};
    bool  have = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end(); it != m_obj_consts.end() && it->second.size >= 48) {
            std::memcpy(obj, it->second.f, sizeof(obj));
            have = true;
        }
    }
    if (!have) return done();
    const auto key = std::make_pair(sig, count);
    auto       pit = s.pin_obj.find(key);
    if (pit == s.pin_obj.end()) {
        if (fc5::rec::is_deferred(ctx) || s.pin_tries[key] >= 3) return done();
        s.pin_tries[key]++;
        // Its vertices (POSITION R16G16B16A16_SINT at 0, scaled by cb2's float 5; skinned by the palette at b3 if one is bound).
        ID3D11InputLayout* il = nullptr;
        ctx->IAGetInputLayout(&il);
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        if (il) il->Release();
        int pos_off = -1, bi_off = -1;
        for (const auto& e : layout) {
            if (_stricmp(e.semantic.c_str(), "POSITION") == 0 && e.index == 0 && e.slot == 0 && e.format == DXGI_FORMAT_R16G16B16A16_SINT) pos_off = int(e.offset);
            if (_stricmp(e.semantic.c_str(), "BLENDINDICES") == 0 && e.index == 0 && e.slot == 0) bi_off = int(e.offset);
        }
        ID3D11Buffer* vb = nullptr;
        UINT          stride = 0, voff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT   ib_fmt{};
        UINT          ib_off = 0;
        ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
        glm::vec3 sum{ 0.0f };
        int       n = 0;
        std::vector<uint8_t> vbd, ibd, cb2, pal;
        if (pos_off == 0 && vb && ib && stride >= 8 && stride <= 128 && read_gpu(ctx, vb, vbd) && read_gpu(ctx, ib, ibd) && read_gpu(ctx, cb[2], cb2) && cb2.size() >= 24) {
            float scale = 0.0f;
            std::memcpy(&scale, cb2.data() + 20, 4);
            D3D11_BUFFER_DESC d3{};
            if (cb[3]) cb[3]->GetDesc(&d3);
            if (d3.ByteWidth == kPaletteBytes) read_gpu(ctx, cb[3], pal);
            const bool i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
            for (UINT k = 0; k < count && scale > 1e-7f && scale < 1e-2f; ++k) {
                const size_t io = size_t(ib_off) + size_t(start + k) * (i32 ? 4 : 2);
                if (io + (i32 ? 4 : 2) > ibd.size()) break;
                uint32_t idx = 0;
                std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
                const size_t vo = size_t(voff) + size_t(int64_t(idx) + base) * stride;
                if (vo + stride > vbd.size()) continue;
                int16_t qv[3];
                std::memcpy(qv, vbd.data() + vo, 6);
                glm::vec4 p{ float(qv[0]) * scale, float(qv[1]) * scale, float(qv[2]) * scale, 1.0f };
                if (!pal.empty() && bi_off >= 0) {
                    const int b = vbd[vo + bi_off];
                    if ((b + 1) * 48 <= (int)pal.size()) p = glm::vec4{ glm::vec3{ from_rows(reinterpret_cast<const float*>(pal.data() + b * 48)) * p }, 1.0f };
                }
                sum += glm::vec3{ p };
                ++n;
            }
        }
        if (vb) vb->Release();
        if (ib) ib->Release();
        if (n == 0) {
            if (s.pin_tries[key] >= 3) spdlog::info("[BowArrow] bow {}: bowpin: its sight's draw ({} indices) not readable: no pin dot", sig, count);
            return done();
        }
        pit = s.pin_obj.emplace(key, sum / float(n)).first;
        const glm::vec3 q = hm.rot * (glm::vec3{ from_rows(obj) * glm::vec4{ pit->second, 1.0f } } - hm.cam) - hm.grip;
        std::string     ps;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_ps_names.find(m_cur_ps); it != m_ps_names.end()) ps = it->second.substr(0, 80);
        }
        spdlog::info("[BowArrow] bow {}: bowpin: its sight's reticle draw {} indices ({} points, ps {}) at ({:.1f}, {:.1f}, {:.1f}) cm from the bow's grip (right, up, back); "
                     "the dot goes on the smallest such draw (the pin's glow)", sig, count, n, ps, q.x * 100.0f, q.y * 100.0f, q.z * 100.0f);
    }
    // The dot follows the smallest reticle draw seen lately (author 2 Nov 12:52: the dot): the pegs
    // sight draws its pin's glow (12 indices) and its pegs (1680, about 7 mm off), and the dot had followed whichever came last.
    const int fr = m_frame.load();
    if (m_bow_pin_count != 0 && count > m_bow_pin_count && fr - m_bow_pin_count_frame <= 30) return done();
    // Not this bow's: a reticle more than half a metre from its grip (another weapon's, seen while switching: 6 indices 82 cm off at 14:36).
    const glm::vec3 pq = hm.rot * (glm::vec3{ from_rows(obj) * glm::vec4{ pit->second, 1.0f } } - hm.cam);
    if (glm::length(pq - hm.grip) > 0.5f) return done();
    m_bow_pin_count       = count;
    m_bow_pin_count_frame = fr;
    m_bow_pin_q           = pq;
    m_bow_pin_w           = hm.w_pos + hm.w_rot * (m_bow_pin_q - hm.grip);   // with the placement this draw was moved by
    m_bow_pin_frame       = fr;
    // bowpinsteady (FarCry5Bow.h): the drawn bow's pin in its own axes (those it was placed with, m_mo.ctrl_rot_world, steadied), for bowsightaim's
    // line from this frame's bow. Kept while drawn only (the game's drawn hold may sit otherwise in its hand than its rest), eased at 0.25 Hz so the
    // game's walk sway and the left hand's one-update lag (the bow placed by last frame's hand) are left out; 2 Hz while it is more than 1 cm
    // off (a real change: another hold, bowhold), at once past 5 cm or for another bow.
    {
        const auto ph = fc5::bow::phase();
        if ((ph == fc5::bow::Phase::Drawing || ph == fc5::bow::Phase::Loose) && hm.frame == fr && m_mo.ctrl_valid) {
            const float     ws  = std::max(1e-3f, m_world_scale.load());
            const glm::vec3 L   = glm::transpose(m_mo.ctrl_rot_world) * (m_bow_pin_w - hm.w_pos);
            const auto      now = std::chrono::steady_clock::now();
            const float     off = glm::length(L - m_bow_pin_l);
            if (!m_bow_pin_l_ok || m_bow_pin_l_sig != sig || off > 0.05f * ws) {
                if (m_bow_pin_l_ok && fc5::bow::arrow_log())
                    spdlog::info("[Bow] bowpinsteady: bow {}: the drawn pin kept again ({}, {:.1f} cm from the last)", sig, m_bow_pin_l_sig != sig ? "another bow" : "moved", off / ws * 100.0f);
                m_bow_pin_l     = L;
                m_bow_pin_l_ok  = true;
                m_bow_pin_l_sig = sig;
            } else {
                const float dt = std::clamp(std::chrono::duration<float>(now - m_bow_pin_l_t).count(), 0.0f, 0.1f);
                const float hz = off > 0.01f * ws ? 2.0f : 0.25f;
                m_bow_pin_l += (1.0f - std::exp(-6.2831853f * hz * dt)) * (L - m_bow_pin_l);
            }
            m_bow_pin_l_t = now;
        }
    }
    done();
}

void FarCry5Stereo::bow_arrow_restore(ID3D11DeviceContext* ctx)
{
    m_ba_cb_swapped = false;
    if (!m_ba) return;
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 4u, static_cast<ID3D11Buffer* const*>(m_ba->orig));
    for (auto*& p : m_ba->orig) {
        if (p) p->Release();
        p = nullptr;
    }
}
