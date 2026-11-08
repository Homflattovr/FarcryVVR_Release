// armik, armcensus (FarCry5ArmIk.h; research\2026-11-08_fullbody_ik.md steps 0 and 1): the first-person arms drawn from the shoulder to
// the hand the mod places, read and rewritten draw by draw in the weapon pass.
//  - weapon_draw_impl calls armik_draw for each player draw of the hide branch: the gloves and bare hands that go ahead (how 0, after the
//    hands' moves), the arms and sleeves the mod hides (how 1) and the clothing glovemesh hides (how 2).
//  - Each draw is known by bonemap (bonemap_identify: its index count, and the palette slots its vertices use where models share a count);
//    its slots' roles come from FarCry5BoneRoles.h. The game's shoulder, elbow and wrist are read from its palette (a slot that is that
//    bone: its rest joint through its matrix) and kept per side in the game camera's space (the arms hang from the game camera, so a joint
//    read in one draw serves the next ones and the next frames).
//  - The first hand draw of a side in a frame solves that side (FarCry5ArmIk.cpp): the shoulder moved under the head, the wrist where
//    the hand is drawn. Every draw then gets the moves of its slots' roles; the hands keep the mod's palette in their own draw.
//  - A hidden draw is drawn again only when every arm side it holds was solved (this frame, or the last two) and it holds an arm
//    bone (a shirt's torso stays hidden unless armikbody).
//  - In a vehicle nothing is changed (drivehands, FarCry5VehHands.cpp, places the arms on the wheel there).
//  - armcensus: one frame's player draws in the log: order, model, roles per side, the game's joints (cm, game camera space) and how far
//    they are from the same joints read by an earlier draw of that frame (mm), and what armik did with the draw.
#include "FarCry5Stereo.h"
#include "FarCry5ArmIk.h"
#include "FarCry5BoneMap.h"
#include "FarCry5BoneNames.h"
#include "FarCry5BoneRoles.h"

#include <d3d11_1.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <istream>
#include <mutex>
#include <set>
#include <vector>

namespace fc5::armik
{
    namespace
    {
        std::mutex         g_mtx;
        Opts               g_o{};
        std::atomic<bool>  g_on{ false };
        std::atomic<bool>  g_census{ false };
        std::atomic<int>   g_solved{ 0 }, g_moved{ 0 }, g_hidden{ 0 }, g_unknown{ 0 };
        std::atomic<float> g_reach[2]{ 0.0f, 0.0f }, g_gap{ 0.0f };
    }

    Opts opts()
    {
        std::lock_guard _{ g_mtx };
        return g_o;
    }
    bool on() { return g_on.load(); }
    bool take_census() { return g_census.exchange(false); }

    void note_status(int solved, int moved, int kept_hidden, int unknown, float reach_r, float reach_l, float gap_max)
    {
        g_solved   = solved;
        g_moved    = moved;
        g_hidden   = kept_hidden;
        g_unknown  = unknown;
        g_reach[0] = reach_r;
        g_reach[1] = reach_l;
        g_gap      = gap_max;
    }

    std::string status()
    {
        return fmt::format("last second: {} arm solves, {} draws moved, {} kept hidden, {} not known; reach right {:.0f}% left {:.0f}%; widest gap {:.1f} cm",
                           g_solved.load(), g_moved.load(), g_hidden.load(), g_unknown.load(), g_reach[0].load() * 100.0f, g_reach[1].load() * 100.0f,
                           g_gap.load() * 100.0f);
    }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        if (cmd.rfind("armik", 0) != 0 && cmd != "armshoulder" && cmd != "armcensus") return false;
        const bool                   on = arg == "on" || arg == "1" || arg == "true";
        std::unique_lock<std::mutex> lk{ g_mtx };
        persist   = true;
        auto said = [&](const std::string& what) { spdlog::info("[ArmIk] {} {}: {}", cmd, arg, what); };
        if (cmd == "armcensus") {
            persist  = false;
            g_census = true;
            lk.unlock();
            said("the next frame's player draws logged ([ArmIk] census lines)");
            return true;
        }
        if (cmd == "armik") {
            g_o.on = on;
            g_on   = on;
            lk.unlock();
            said(on ? "the arms drawn from the shoulder to the hands (needs hidearms on)" : "off: the mod's hands only, as before");
            return true;
        }
        if (cmd == "armiksleeves") {
            g_o.sleeves = on;
            lk.unlock();
            said(on ? "the sleeves and forearms the mod hides drawn again with the arms" : "the sleeves and forearms stay hidden (the gloves still move)");
            return true;
        }
        if (cmd == "armikbody") {
            g_o.body = on;
            lk.unlock();
            said(on ? "draws of the torso (shirt, vest) drawn too, under the head" : "the torso's draws stay hidden");
            return true;
        }
        if (cmd == "armiktorso") {
            g_o.torso = on;
            lk.unlock();
            said(on ? "the shoulders turned upright to the body's heading" : "the shoulders moved only (the game's tilt kept)");
            return true;
        }
        if (cmd == "armshoulder") {
            float back = g_o.back;
            rest >> back;
            g_o.down = std::clamp(arg.empty() ? 0.22f : fv, 0.05f, 0.5f);
            g_o.back = std::clamp(back, -0.2f, 0.3f);
            lk.unlock();
            said("the shoulders' line below and behind the eyes set");
            return true;
        }
        if (cmd == "armiksoft") {
            g_o.soft = std::clamp(arg.empty() ? 0.97f : fv, 0.5f, 1.0f);
            lk.unlock();
            said("soft reach set");
            return true;
        }
        if (cmd == "armikstretch") {
            g_o.stretch = std::clamp(arg.empty() ? 1.10f : fv, 1.0f, 1.5f);
            lk.unlock();
            said("most lengthening set");
            return true;
        }
        if (cmd == "armikpole") {
            float g = g_o.pole_game, h = g_o.pole_hand;
            rest >> g >> h;
            g_o.pole_body = std::clamp(arg.empty() ? 0.5f : fv, 0.0f, 1.0f);
            g_o.pole_game = std::clamp(g, 0.0f, 1.0f);
            g_o.pole_hand = std::clamp(h, 0.0f, 1.0f);
            lk.unlock();
            said("elbow direction weights (body, game, hand) set");
            return true;
        }
        if (cmd == "armikelbow") {
            g_o.elbow_ms = std::clamp(arg.empty() ? 50.0f : fv, 0.0f, 500.0f);
            lk.unlock();
            said("elbow easing set");
            return true;
        }
        persist = false;
        return false;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        const Opts o = opts();
        bool       v = o.on;
        if (ImGui::Checkbox("Arms from the shoulder to the hands (arms IK)##armik", &v)) cmds += v ? "armik on\n" : "armik off\n";
        hint("Test (off by default). The game's own arms, sleeves and gloves drawn from your shoulders (under the headset) to where the mod puts "
             "the hands, for every outfit: the bones are known by name from the game's model files, so no outfit needs its own setting. "
             "Needs Floating weapon (hide the game's arms) on, just above. Command armik on/off. research\\2026-11-08_fullbody_ik.md.");
        if (ImGui::Button("Log one frame's player draws##armcensus")) cmds += "armcensus\n";
        hint("armcensus: every arm, glove and sleeve draw of the next frame in the log ([ArmIk] census): its model, which arm bones it holds, "
             "the game's shoulder, elbow and wrist, and what arms IK did with it. Send the log after a capture in each outfit.");
        if (!v) return;
        ImGui::Text("%s", status().c_str());
        bool s = o.sleeves;
        if (ImGui::Checkbox("Sleeves and forearms drawn with the arms##armiks", &s)) cmds += s ? "armiksleeves on\n" : "armiksleeves off\n";
        bool t = o.torso;
        if (ImGui::Checkbox("Shoulders upright, turned with your body##armikt", &t)) cmds += t ? "armiktorso on\n" : "armiktorso off\n";
        bool b = o.body;
        if (ImGui::Checkbox("Also the torso's draws (shirt, vest)##armikb", &b)) cmds += b ? "armikbody on\n" : "armikbody off\n";
        hint("Off: a draw made only of torso bones stays hidden, as now.");
        float sh[2]{ o.down, o.back };
        if (ImGui::SliderFloat2("Shoulders below / behind the eyes (m)##armsh", sh, -0.1f, 0.4f, "%.2f")) cmds += fmt::format("armshoulder {:.3f} {:.3f}\n", sh[0], sh[1]);
        float st = o.stretch;
        if (ImGui::SliderFloat("Most arm lengthening##armikst", &st, 1.0f, 1.5f, "%.2f")) cmds += fmt::format("armikstretch {:.3f}\n", st);
        hint("Past your arm's length the arm lengthens up to this to stay on the hand; beyond it the hand leaves the wrist.");
        float p[3]{ o.pole_body, o.pole_game, o.pole_hand };
        if (ImGui::SliderFloat3("Elbow: body / game / hand##armikp", p, 0.0f, 1.0f, "%.2f")) cmds += fmt::format("armikpole {:.2f} {:.2f} {:.2f}\n", p[0], p[1], p[2]);
        hint("Where the elbow points: down and out from the body, as the game bent it, and turned with the hand.");
    }
}

namespace
{
    using namespace fc5::armik;
    glm::vec3 pt(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }
    glm::mat4 affine(const glm::mat3& r, const glm::vec3& t)
    {
        glm::mat4 m{ r };
        m[3] = glm::vec4{ t, 1.0f };
        return m;
    }
    glm::vec3 rest(int side, int j)
    {
        const float* f = fc5::bonenames::kRest[side][j];
        return glm::vec3{ f[0], f[1], f[2] };
    }
    struct PoolJ
    {
        glm::vec3 c{ 0.0f };   // game camera space (a direction for the torso's up)
        int       frame{ -1000 };
    };
    const char* role_name(int r)
    {
        switch (r) {
        case kClav: return "Sh";
        case kUpper: return "Arm";
        case kUpTwA: return "ATa";
        case kUpTwB: return "ATb";
        case kElbow: return "Elb";
        case kFore: return "Fa";
        case kFoTwA: return "FTa";
        case kFoTwB: return "FTb";
        case kHand: return "Hand";
        default: return "?";
        }
    }
    constexpr int kPoolAge = 30;   // frames a joint read earlier stays usable (the arms hang from the camera; only the game's animation moves them)
}

struct FarCry5Stereo::ArmIkState
{
    int                                   frame{ -1 };
    PoolJ                                 pool[2][3];   // per side: shoulder, elbow, wrist (game camera space)
    PoolJ                                 up;           // the torso's up (game camera space)
    bool                                  body_ok{ false };
    glm::mat4                             Dt{ 1.0f };
    SideOut                               side[2]{};
    int                                   side_frame[2]{ -1000, -1000 };
    glm::vec3                             pole[2]{};
    std::chrono::steady_clock::time_point pole_t[2]{};
    float                                 twist[2][2]{ { 0.75f, 0.45f }, { 0.75f, 0.45f } };   // forearm twist bones along the forearm (0 elbow, 1 wrist)
    int                                   census_frame{ -1 };
    int                                   order{ 0 };
    int                                   solved{ 0 }, moved{ 0 }, hidden{ 0 }, unknown{ 0 };
    float                                 reach[2]{}, gap{ 0.0f };
    int                                   stat_frame{ 0 };
    std::set<uint32_t>                    unknown_said;
};

bool FarCry5Stereo::armik_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, int how)
{
    const bool census_req = fc5::armik::take_census();
    const bool ik_on      = fc5::armik::on();
    if (!ik_on && !census_req && (!m_ak || m_ak->census_frame < 0)) return false;
    if (!m_ak) m_ak = new ArmIkState{};
    auto&      st    = *m_ak;
    const int  frame = m_frame.load();
    if (census_req) {
        st.census_frame = frame + 1;
        st.order        = 0;
    }
    if (st.census_frame >= 0 && frame > st.census_frame) {
        spdlog::info("[ArmIk] census of frame {} done: {} player draws in the weapon pass's hide branch", st.census_frame, st.order);
        st.census_frame = -1;
    }
    const bool census = st.census_frame == frame;
    if (frame != st.frame) {
        st.frame   = frame;
        st.body_ok = false;
        if (frame - st.stat_frame >= 90) {
            fc5::armik::note_status(st.solved, st.moved, st.hidden, st.unknown, st.reach[0], st.reach[1], st.gap);
            st.solved = st.moved = st.hidden = st.unknown = 0;
            st.gap        = 0.0f;
            st.stat_frame = frame;
        }
    }
    m_keep_bones = true;   // the palettes' CPU copies (note_object_constants)
    static const char* const kHow[3]{ "hand", "hidden arm", "hidden clothing" };
    const char*              how_s = kHow[std::clamp(how, 0, 2)];
    const int                order = census ? st.order++ : 0;
    auto say = [&](const std::string& what) {
        if (census) spdlog::info("[ArmIk] census #{} {} draw {} from {} base {}: {}", order, how_s, count, start, base, what);
    };
    if (count == 0) {
        say("an indirect draw whose counts were not read (not known)");
        return false;
    }
    if (!m_vs_cb0 || !is_weapon_cb(m_vs_cb0)) return false;
    const auto& hm = m_hand_map;
    if (frame - hm.frame > 1) {
        say("the weapon pass not placed this frame");
        return false;
    }
    if (in_vehicle()) {
        say("in a vehicle: drivehands places the arms there (left alone)");
        return false;
    }
    if (m_hand_cb_swapped && !m_hand_bones_swapped) {
        say("another move's own constants bound (left alone)");
        return false;
    }

    // The game's palette (the original buffer when the hands' stand-in is bound), the palette as it is drawn now, the object transform.
    ID3D11Buffer* cb[4]{};
    ctx->VSGetConstantBuffers(0, 4, cb);
    struct Rel { ID3D11Buffer** b; ~Rel() { for (int i = 0; i < 4; ++i) if (b[i]) b[i]->Release(); } } rel{ cb };
    ID3D11Buffer*      pal_buf = m_hand_bones_swapped ? m_hand_cb_orig[0] : cb[3];
    std::vector<float> orig, fin;
    float              obj[12]{};
    bool               have_obj = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (pal_buf) if (auto it = m_bone_data.find(pal_buf); it != m_bone_data.end()) orig = it->second;
        if (cb[1]) if (auto it = m_obj_consts.find(cb[1]); it != m_obj_consts.end() && it->second.size >= 48) { std::memcpy(obj, it->second.f, sizeof(obj)); have_obj = true; }
    }
    if (m_hand_bones_swapped) {
        if (m_bm_pal_frame == frame && m_bm_pal.size() >= 3072) fin = m_bm_pal;
    } else {
        fin = orig;
    }
    if (orig.size() < 3072 || fin.size() < 3072 || !have_obj) {
        say(fmt::format("no palette or object transform yet ({} {} {})", orig.size(), fin.size(), have_obj));
        return false;
    }
    int       alts = 0;
    const int d    = bonemap_identify(ctx, count, start, base, m_hud_draw_kind == 5, alts);
    if (d < 0) {
        st.unknown++;
        if (st.unknown_said.size() < 64 && st.unknown_said.insert(count).second) spdlog::info("[ArmIk] {} draw of {} indices not known yet (bonemap tables; or two models share the count and its vertices are not read yet): left as it was", how_s, count);
        say("not in the bonemap tables (or its vertices not read yet)");
        return false;
    }
    const int kind = fc5::bonemap::kind(d), nb = std::min(256, fc5::bonemap::nbones(d));
    if (kind == 1 || nb <= 0) {
        say(fmt::format("{}: not a skinned player model", fc5::bonemap::model(d)));
        return false;
    }
    const uint8_t* roles = fc5::bonenames::kRole + fc5::bonenames::kDraws[d].first;

    // Rest model -> game world (b1) -> drawn world (the weapon pass's placement, as bonemap and reloadmagmodel).
    const glm::mat3 L        = hm.w_rot * hm.rot;
    const glm::mat4 w2d      = affine(L, hm.w_pos - hm.w_rot * hm.grip - L * hm.cam);
    const glm::mat4 Om       = from_rows(obj);
    const glm::mat4 M        = w2d * Om;
    const glm::mat4 Minv     = glm::inverse(M);
    const bool      mirrored = glm::determinant(glm::mat3{ M }) < 0.0f;
    const glm::mat3 rotT     = glm::transpose(hm.rot);
    auto to_cam   = [&](const glm::vec3& world) { return hm.rot * (world - hm.cam); };
    auto from_cam = [&](const glm::vec3& c) { return pt(w2d, hm.cam + rotT * c); };

    // The game's joints in this draw (slots that are the standard bones themselves), by priority: the bone itself, then a bone the joint is
    // fixed in (its parent, or a twist bone: the joint is on its axis).
    GameArm   g[2]{};
    int       prio[2][3]{};
    int       hand_slot[2]{ -1, -1 };
    glm::vec3 jw[2][3]{};   // game world
    glm::vec3 up_w{ 0.0f };
    int       up_prio = 0;
    bool      cov[2][25]{};
    int       n_body = 0, n_arm = 0, n_hand = 0, n_other = 0;
    bool      has_limb = false;
    for (int s = 0; s < nb; ++s) {
        const uint8_t r = roles[s];
        const int     role = role_of(r), side = side_of(r);
        if (role == kOther) n_other++;
        else if (is_body(role)) n_body++;
        else if (role == kHand) n_hand++;
        else n_arm++;
        if (role >= kUpper && role <= kHand) has_limb = true;
        if (!(r & kPrimary)) continue;
        if (is_arm(role)) cov[side][role] = true;
        const glm::mat4 W = Om * from_rows(orig.data() + size_t(s) * 12);   // rest model -> game world
        auto put = [&](int j, int p, int rest_j) {
            if (p > prio[side][j]) {
                prio[side][j] = p;
                jw[side][j]   = pt(W, rest(side, rest_j));
            }
        };
        switch (role) {
        case kClav: put(0, 1, kJArm); break;
        case kUpper: put(0, 3, kJArm); put(1, 2, kJForeArm); break;
        case kUpTwA: case kUpTwB: put(0, 2, kJArm); put(1, 1, kJForeArm); break;
        case kElbow: put(1, 2, kJForeArm); break;
        case kFore: put(1, 3, kJForeArm); put(2, 2, kJHand); break;
        case kFoTwA: case kFoTwB: {
            put(1, 1, kJForeArm); put(2, 1, kJHand);
            float j[3]{};   // where this twist bone sits along the forearm (rest)
            if (fc5::bonemap::slot_joint(d, s, j))
                st.twist[side][role == kFoTwA ? 0 : 1] = std::clamp(along(rest(side, kJForeArm), rest(side, kJHand), glm::vec3{ j[0], j[1], j[2] }), 0.0f, 1.0f);
            break;
        }
        case kHand: put(2, 3, kJHand); hand_slot[side] = s; break;
        case kSpine2: case kNeck: case kSpine1: case kSpine: {
            const int p = role == kSpine2 ? 4 : role == kNeck ? 3 : role == kSpine1 ? 2 : 1;
            if (p > up_prio) { up_prio = p; up_w = glm::mat3{ W } * glm::vec3{ 0.0f, 0.0f, 1.0f }; }
            break;
        }
        default: break;
        }
    }
    // Into the pool (camera space); the census compares with what earlier draws of this frame read.
    std::string joints;
    for (int side = 0; side < 2; ++side)
        for (int j = 0; j < 3; ++j) {
            if (!prio[side][j]) continue;
            const glm::vec3 c = to_cam(jw[side][j]);
            if (census) {
                const PoolJ& p = st.pool[side][j];
                joints += fmt::format(" {}{}({:.1f},{:.1f},{:.1f}){}", side == 0 ? 'R' : 'L', "SEW"[j], c.x * 100.0f, c.y * 100.0f, c.z * 100.0f,
                                      p.frame == frame ? fmt::format("[{:.0f}mm]", glm::length(c - p.c) * 1000.0f) : std::string{});
            }
            st.pool[side][j] = { c, frame };
        }
    if (up_prio > 0 && glm::length(up_w) > 1e-4f) st.up = { glm::normalize(hm.rot * up_w), frame };

    auto fresh = [&](const PoolJ& p) { return frame - p.frame <= kPoolAge; };
    for (int side = 0; side < 2; ++side) {
        auto& ga = g[side];
        if (fresh(st.pool[side][0])) { ga.S = from_cam(st.pool[side][0].c); ga.s = true; }
        if (fresh(st.pool[side][1])) { ga.E = from_cam(st.pool[side][1].c); ga.e = true; }
        if (prio[side][2]) { ga.W = pt(w2d, jw[side][2]); ga.w = true; }   // the wrist of this very draw (its hand's frame goes with it)
        if (hand_slot[side] >= 0) { ga.H = M * from_rows(orig.data() + size_t(hand_slot[side]) * 12); ga.h = true; }
    }

    // The torso: the game's shoulder line onto the body under the head (once a frame).
    const fc5::armik::Opts o = fc5::armik::opts();
    if (!st.body_ok && ik_on && m_mo.arm_body_valid && fresh(st.pool[0][0]) && fresh(st.pool[1][0])) {
        BodyIn b{};
        b.S0[0]    = from_cam(st.pool[0][0].c);
        b.S0[1]    = from_cam(st.pool[1][0].c);
        b.up0      = fresh(st.up) ? glm::mat3{ w2d } * (rotT * st.up.c) : glm::mat3{ w2d } * glm::vec3{ 0.0f, 0.0f, 1.0f };
        b.mid      = m_mo.arm_mid_world;
        b.right    = m_mo.arm_right_world;
        b.up       = m_mo.arm_up_world;
        b.mirrored = mirrored;
        st.Dt      = torso_move(b, o.torso);
        st.body_ok = true;
    }

    // A hand draw solves its sides (once a frame each).
    std::string solved_s;
    if (ik_on && how == 0 && st.body_ok) {
        for (int side = 0; side < 2; ++side) {
            if (hand_slot[side] < 0 || st.side_frame[side] == frame) continue;
            const glm::mat4 Hf  = M * from_rows(fin.data() + size_t(hand_slot[side]) * 12);
            const float     sgn = (side == 0 ? 1.0f : -1.0f) * (mirrored ? -1.0f : 1.0f);
            const auto      now = std::chrono::steady_clock::now();
            const float     dt  = st.pole_t[side].time_since_epoch().count() ? std::chrono::duration<float>(now - st.pole_t[side]).count() : 0.0f;
            const glm::vec3 last = (frame - st.side_frame[side] <= 10) ? st.pole[side] : glm::vec3{ 0.0f };
            const SideOut   so   = solve_side(o, g[side], Hf, st.Dt, sgn * m_mo.arm_right_world, -m_mo.arm_up_world, m_mo.arm_back_world, last,
                                              std::min(dt, 0.1f), st.twist[side]);
            if (!so.ok) {
                solved_s += fmt::format(" {}: not solved (joints {}{}{}{})", side == 0 ? "right" : "left", g[side].s ? 'S' : '-', g[side].e ? 'E' : '-', g[side].w ? 'W' : '-',
                                        g[side].h ? 'H' : '-');
                continue;
            }
            st.side[side]       = so;
            st.side_frame[side] = frame;
            st.pole[side]       = so.pole;
            st.pole_t[side]     = now;
            st.solved++;
            st.reach[side] = so.reach;
            st.gap         = std::max(st.gap, so.gap);
            solved_s += fmt::format(" {} solved: reach {:.0f}%, stretch {:.2f}, gap {:.1f} cm", side == 0 ? "right" : "left", so.reach * 100.0f, so.stretch, so.gap * 100.0f);
        }
    }

    // The moves: every slot with a role from the game's palette; the hands of a hand draw and unknown slots keep the palette as drawn.
    bool        need[2]{ false, false };
    for (int s = 0; s < nb; ++s) {
        const int role = role_of(roles[s]);
        if (is_arm(role) && !(role == kHand && how == 0)) need[side_of(roles[s])] = true;
    }
    const int   lag   = int(o.lag_frames);
    const bool  ready = st.body_ok && (!need[0] || frame - st.side_frame[0] <= lag) && (!need[1] || frame - st.side_frame[1] <= lag);
    std::string cover;
    if (census) {
        for (int side = 0; side < 2; ++side) {
            cover += side == 0 ? " R:" : " L:";
            for (int r = kClav; r <= kHand; ++r) if (cov[side][r]) cover += fmt::format("{},", role_name(r));
        }
        cover += fmt::format(" slots: {} body, {} arm, {} hand and fingers, {} other", n_body, n_arm, n_hand, n_other);
    }
    std::string what;
    bool        moved = false;
    if (!ik_on) {
        what = "armik off";
    } else if (!ready) {
        what = fmt::format("not moved: {}{}{}", st.body_ok ? "" : "no body yet (both shoulders not read, or no head pose)",
                           need[0] && frame - st.side_frame[0] > lag ? " right arm not solved" : "", need[1] && frame - st.side_frame[1] > lag ? " left arm not solved" : "");
    } else if (how != 0 && (!o.sleeves || (!has_limb && !o.body))) {
        what = !o.sleeves ? "kept hidden (armiksleeves off)" : "kept hidden (torso only; armikbody off)";
    } else {
        std::vector<float> out = fin;
        int                changed = 0;
        for (int s = 0; s < nb; ++s) {
            const int role = role_of(roles[s]), side = side_of(roles[s]);
            const float* in = orig.data() + size_t(s) * 12;
            float*       po = out.data() + size_t(s) * 12;
            if (role == kOther) continue;
            if (is_body(role)) {
                if (role == kRoot && kind == 3) continue;   // a glove's cuff is skinned to the root and goes with the hand (the mod moved it)
                move_slot(in, po, M, Minv, st.Dt);
                changed++;
                continue;
            }
            if (role == kHand && how == 0) continue;   // the hand as the mod drew it
            move_slot(in, po, M, Minv, role == kClav ? st.Dt : st.side[side].D[role]);
            changed++;
        }
        moved = changed > 0 && hands_cb_update(ctx, out);
        what  = moved ? fmt::format("moved ({} slots)", changed) : changed ? "not moved (the stand-in palette could not be bound)" : "nothing to move (hands only)";
    }
    if (moved) st.moved++;
    else if (how != 0) st.hidden++;
    say(fmt::format("{} (kind {}, {} slots{}){}{};{}{}", fc5::bonemap::model(d), kind, nb, alts ? fmt::format(", {} models share it", alts) : std::string{},
                    m_hand_bones_swapped ? ", hands moved by the mod" : "", cover, joints.empty() ? " no joints" : joints, solved_s + "; " + what));
    return moved;
}
