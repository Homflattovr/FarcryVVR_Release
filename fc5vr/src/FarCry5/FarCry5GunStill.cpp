// gunstill (author 16 Oct evening; F11 Comfort, default on).
//
// The motion-controlled gun is the game's weapon pass put on the controller: the game's own animation of the gun relative
// to its camera (running bob, the sprint pose, recoil, aiming down sights, vehicle bumps) moved the gun in the hand.
// Here the gun's pose in the game camera's axes (q = A (p - cam), the space the controller places) is held at this
// weapon's standing pose, learned while standing still with nothing going on, at all times except:
//  - while the game reloads: started by the pad's X (reload / interact) or by the game's left hand leaving its place on
//    the gun within 1.5 s of shooting (the game's own reloads after the last round), ended once the hand is back and the
//    pose has settled;
//  - while firing, only with gunstillfire game (gunstillrecoil, author 21:29: the correction
//    frozen at its value when firing began, so the game's kick shows on top of the held pose; off, the default since 20 Oct: the
//    game's pose less its 0.12 s average shows). gunstillfire hold (default since 24 Oct) keeps the gun held while firing too,
//    gunstillfire kick shows only a recoil kick taken from the game's (kick_of in gun_still);
//  - while a throw is under way (the game hides the gun and draws the throwable: FarCry5Throwable);
//  - before this weapon's pose is known (its first draw animation after it is taken out the first time).
// Parts that move on the gun (magazine, bolt, the right hand on the grip) keep their motion: only the whole gun is held.
// The pose is measured on the last weapon pass; in stereo the left eye's pass is the next game update (in AFW every
// frame is), so the pose is moved on by the last update's change (the right eye's pass is the same update: exact). The
// game writes the gun's transform after the pass's camera upload (simulator: 1059 of 1059 reads there a frame old), so
// it cannot be read for the pass itself.
// In a vehicle, gun_anchor_apply also puts the gun and the hands on the eyes' smoothed position (vehsmooth), so the
// vehicle's bob and vibration the view filters out are not left on the gun.
#include "FarCry5Stereo.h"
#include "FarCry5GameWeapon.h"
#include "FarCry5Gesture.h"
#include "FarCry5Reload.h"
#include "FarCry5Dlss.h"
#include "FarCry5GunPoseTable.h"
#include "ModPath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <set>
#include <unordered_map>

#include <Windows.h>
#include <Xinput.h>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

namespace
{
    using clock = std::chrono::steady_clock;

    struct Pose
    {
        glm::quat r{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 t{ 0.0f };
    };
    Pose to_pose(const glm::mat4& m) { return Pose{ glm::normalize(glm::quat_cast(glm::mat3{ m })), glm::vec3{ m[3] } }; }
    glm::mat4 to_mat(const Pose& p)
    {
        glm::mat4 m = glm::mat4_cast(p.r);
        m[3]        = glm::vec4{ p.t, 1.0f };
        return m;
    }
    float angle_deg(const glm::quat& a, const glm::quat& b) { return glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f))); }
    // A change of pose taken r times (its turn about the same axis, its move along the same line).
    glm::mat4 pose_pow(const Pose& p, float r)
    {
        glm::quat q = p.r;
        if (q.w < 0.0f) q = -q;
        const float a = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
        const glm::vec3 v{ q.x, q.y, q.z };
        Pose out{};
        if (a > 1e-6f && glm::length(v) > 1e-9f) out.r = glm::angleAxis(a * r, glm::normalize(v));
        out.t = p.t * r;
        return to_mat(out);
    }

    // gunstillshot on|off (default on): the game's left hand leaving the gun is a reload only within 1.5 s of shooting.
    // Off: as before 19 Oct (any time on foot, except sprinting or faster than 3 m/s).
    std::atomic<bool> g_hand_needs_shot{ true };
    // gunstillpad on|off (default on): a press of X shows the game's animation only once the game's left hand leaves the gun
    // within 1 s of it (a reload); interact, a full magazine and the press itself keep the gun held. Off: from the press on.
    std::atomic<bool> g_pad_waits_hand{ true };

    // gunposecheck: how far a pose holds a gun off its own axes, in the game camera's axes: its barrel (the mesh's +Y) from the aim's axis (-Z),
    // in deg, and (roll) its top (+Z) turned from up about the barrel, in deg.
    float axes_off_deg(const glm::quat& r, float& roll)
    {
        const glm::vec3 b = glm::mat3_cast(r) * glm::vec3{ 0.0f, 1.0f, 0.0f }, u = glm::mat3_cast(r) * glm::vec3{ 0.0f, 0.0f, 1.0f };
        roll              = glm::degrees(std::atan2(u.x, u.y));
        return glm::degrees(std::acos(std::clamp(-b.z, -1.0f, 1.0f)));
    }
    // gunposecheck applies only to a gun by the game's own weapon data (pistol, rifle, SMG, shotgun, sniper, LMG): not a bow (25110 and 51543
    // have saved poses rolled 46 and 69 deg), a launcher, a melee weapon or the flamethrower, nor a weapon the data does not know yet (the
    // first half second in hand) or at all.
    bool data_gun(uint32_t sig)
    {
        const auto* w = fc5::gameweapon::info(sig);
        return w && w->cls >= 1 && w->cls <= 6;
    }
    // gunorigin: a handgun by the game's data (a pistol, or a one-handed SMG: a machine pistol), which the game holds at arm's length.
    bool data_handgun(uint32_t sig)
    {
        const auto* w = fc5::gameweapon::info(sig);
        return w && (w->cls == 1 || (w->cls == 3 && w->one_handed));
    }
    // A learned pose well off the gun's axes (no gun is held standing so: the game holds its guns 0-13 deg off, rolled up to 16).
    bool pose_off_axes(const glm::quat& r)
    {
        float      roll = 0.0f;
        const float off = axes_off_deg(r, roll);
        return off > 20.0f || std::abs(roll) > 30.0f;
    }
    // twohandlevel: a long gun by the game's data (rifle, SMG, shotgun, marksman rifle, LMG; not one-handed): not a handgun, a machine
    // pistol, a bow, a launcher, the flamethrower or a melee weapon.
    bool data_long_gun(uint32_t sig)
    {
        const auto* w = fc5::gameweapon::info(sig);
        return w && w->cls >= 2 && w->cls <= 6 && !w->one_handed;
    }
    // A direction in a gun's axes (x right, y along the barrel, z up) from its angles right of and up from the barrel (deg), and back.
    glm::vec3 line_of(float right_deg, float up_deg)
    {
        const float r = glm::radians(right_deg), u = glm::radians(up_deg);
        return glm::vec3{ std::cos(u) * std::sin(r), std::cos(u) * std::cos(r), std::sin(u) };
    }
    void line_angles(const glm::vec3& l, float& right_deg, float& up_deg)
    {
        right_deg = glm::degrees(std::atan2(l.x, l.y));
        up_deg    = glm::degrees(std::atan2(l.z, std::sqrt(l.x * l.x + l.y * l.y)));
    }
    // weaponaudit: a direction's yaw (right +) and pitch (up +) against -Z, deg (the game camera's or a controller's axes).
    void dir_angles(const glm::vec3& d, float& yaw_deg, float& pitch_deg)
    {
        yaw_deg   = glm::degrees(std::atan2(d.x, -d.z));
        pitch_deg = glm::degrees(std::asin(std::clamp(d.y / std::max(1e-6f, glm::length(d)), -1.0f, 1.0f)));
    }
}

struct FarCry5Stereo::GunStillState
{
    struct Ref
    {
        Pose      pose{};
        bool      ok{ false }, settled{ false };
        glm::vec3 hand{ 0.0f };   // the game's left hand in the gun's own axes, standing
        bool      hand_ok{ false };
        // The first pose learned (in hand, standing): a dropped pose goes back to it instead of being learned anew from
        // whatever the game shows then (author 20 Oct 14:09: the AR-C relearned in the game's relaxed pose, 75 deg off).
        Pose      first{};
        glm::vec3 first_hand{ 0.0f };
        bool      first_ok{ false }, first_hand_ok{ false };
        bool      big_said{ false };
        glm::vec3 line{ 0.0f, 1.0f, 0.0f };   // twohandline: the game's weapon hand -> left hand direction, the gun's own axes
        bool      line_ok{ false };
        bool      temp{ false };   // shoulderdraw: a pose taken for one switch only (a weapon held out of the learning's reach), dropped after it
        int       ahead{ 0 }, beside{ 0 };    // sidearm: measures with the left hand ahead of the weapon hand along the barrel / not
        bool      builtin{ false };          // gunposebuiltin: the pose came from FarCry5GunPoseTable.h (not saved while so)
    };
    // A dropped pose: back to the weapon's first learned one if it has one, else learned again.
    static void drop(Ref& r)
    {
        if (r.first_ok) {
            r.pose    = r.first;
            r.hand    = r.first_hand;
            r.hand_ok = r.first_hand_ok;
            r.ok = r.settled = true;
        } else {
            r = Ref{};
        }
    }
    std::unordered_map<uint32_t, Ref> refs{};   // by weapon signature (m_weapon_sig)
    std::set<uint32_t>                unlearned_said{};   // weapons logged as out 5 s and not learned
    clock::time_point                 unlearned_t{};      // gunstillwhy: the last "still not learned" line
    bool                              loaded{ false };   // fc5vr_gunposes.txt read

    // The player's horizontal speed from the game camera.
    glm::vec3         spd_cam{ 0.0f };
    clock::time_point spd_t{};
    bool              spd_have{ false };
    float             speed{ 0.0f };

    // The measured pose, by game update (the measured pass's game camera tells updates apart).
    int       meas_frame{ -100 };
    glm::mat4 q_last{ 1.0f }, q_prev{ 1.0f };
    bool      have_prev{ false };

    // The game's left hand (gloves draw), in the game camera's axes, and its frame.
    glm::vec3 hand{ 0.0f };
    glm::vec3 hand_w{ 0.0f };   // the game's weapon (right) hand, same draw
    int       hand_frame{ -100 };

    uint32_t          sig{ 0 };
    clock::time_point sig_t{};
    clock::time_point last_fire{}, last_ads{}, last_throw{};
    clock::time_point last_slow{};   // the last time the player was slower than 1 m/s (a sprint press after it = sprinting)
    bool              sprinting{ false };
    uint64_t          hand_skips{ 0 };   // the game's left hand off the gun with no shot just before: not taken for a reload
    uint64_t          switch_skips{ 0 };   // gunstillswitch: X presses that changed weapon, not taken for a reload of the next one

    // Reload.
    bool              reload{ false };
    const char*       reload_why{ "" };
    clock::time_point reload_t0{}, active_t{};
    int64_t           pad_seen{ 0 };
    bool              pad_init{ false };
    clock::time_point pad_t{};   // the last press of X (gunstillpad: a reload once the hand leaves within 1 s)
    bool              away_on{ false }, away_said{ false };   // the game's standing gun clearly off the held pose, since away_t
    clock::time_point away_t{}, last_moving{};                 // last_moving: faster than 1 m/s or sprinting
    clock::time_point hand_off_t{};                            // since when the hand has been off its place, standing, no reload
    int               hand_off_n{ 0 };
    float             hand_off_cm{ 0.0f }, dev_cm{ 0.0f }, dev_deg{ 0.0f };

    float             alpha{ 0.0f };   // 1 = held at the standing pose, 0 = the game's pose
    // gunstillfire hold / kick: the kick's share (1 while firing, easing out over 0.15 s after); gunstillstat's firing line.
    float             kick_a{ 0.0f };
    uint64_t          fs_n{ 0 }, fs_gated{ 0 };
    float             fs_game_cm{ 0.0f }, fs_game_max_cm{ 0.0f }, fs_game_max_deg{ 0.0f }, fs_shown_max_cm{ 0.0f }, fs_shown_max_deg{ 0.0f };
    int               fs_frame{ -100 };
    bool              fire_on{ false };
    float             fire_beta{ 1.0f };   // gunstillrecoil: 0 = the correction frozen while firing, 1 = the full hold
    glm::mat4         fire_C{ 1.0f };
    clock::time_point alpha_t{};
    uint64_t          held_updates{ 0 }, reloads{ 0 };

    // gunstillstat: the correction given to each frame's pass, checked once that pass's pose is measured.
    int       applied_frame{ -100 };
    bool      applied_full{ false };
    glm::mat4 applied_C{ 1.0f };
    int       applied_C_frame{ -100 };   // the call that gave a correction (not the identity)
    uint64_t  pair_shared{ 0 };           // gunstillpair: right-eye passes given the left eye's hold
    int       pair_frame{ -100 };         // the right-eye frame that was given it (a second weapon view of that frame too)

    // The next update's guess (the Stereo left eye, every AFW frame): the times of the last three game updates (each one's
    // first call), the update q_prev was measured in, and each way's guess for the predicted pass, checked once it is measured.
    int               upd_u[3]{ -100, -100, -100 };
    clock::time_point upd_t[3]{};
    int               q_prev_upd{ -100 };
    int               pred_frame{ -100 };
    glm::mat4         pred_q[4]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };   // hold, linear, timed, half
    uint64_t          pr_n{ 0 }, pr_timed{ 0 };
    float             pr_cm[4]{}, pr_max_cm[4]{}, pr_deg[4]{}, pr_max_deg[4]{};
    float             pr_r_min{ 1e9f }, pr_r_max{ 0.0f };
    // gunstillstat per eye: the drawn gun off its standing pose, its change from that eye's last pass, and in Stereo the two
    // eyes of one update against each other (what one eye sees moving and the other not).
    uint64_t          ey_n[2]{}, ey_steps[2]{}, pd_n{ 0 };
    float             ey_cm[2]{}, ey_max_cm[2]{}, ey_max_deg[2]{}, ey_step_cm[2]{}, ey_step_max_cm[2]{}, ey_step_max_deg[2]{};
    float             pd_cm{ 0.0f }, pd_max_cm{ 0.0f }, pd_max_deg{ 0.0f };
    Pose              ey_prev[2]{};
    int               ey_prev_frame[2]{ -100, -100 };
    uint64_t  st_n{ 0 };
    float     st_drawn_cm{ 0.0f }, st_drawn_max_cm{ 0.0f }, st_drawn_max_deg{ 0.0f };
    float     last_drawn_cm{ 0.0f }, last_drawn_deg{ 0.0f };   // the last checked pass (gunstilllog)
    int       last_drawn_frame{ -100 };
    float     st_game_cm{ 0.0f }, st_game_max_cm{ 0.0f }, st_game_max_deg{ 0.0f };

    // While firing (gunstillrecoil off): the game's pose averaged.
    glm::mat4         q_lp{ 1.0f };
    int               q_lp_frame{ -100 };
    clock::time_point q_lp_t{};
    // gunstillstat: the drawn gun's change from one checked pass to the next (how much it shakes).
    Pose              step_prev{};
    int               step_prev_frame{ -100 };
    uint64_t          st_steps{ 0 };
    float             st_step_cm{ 0.0f }, st_step_max_cm{ 0.0f }, st_step_max_deg{ 0.0f };

    // Probe: the gun's object buffer read at the weapon pass's first draw (gun_still_first_draw).
    ID3D11Buffer* gun_cb{ nullptr };   // the gun mesh's object buffer of the last pass (a key only)
    int           first_frame{ -100 };
    bool          first_have{ false };
    float         first_obj[12]{}, prev_obj[12]{};
    uint64_t      first_current{ 0 }, first_stale{ 0 }, first_other{ 0 };

    // meleestill (FarCry5Melee.cpp): a melee weapon's game hand in the weapon's own axes (smoothed), per weapon.
    struct MeleeHand { glm::vec3 h{ 0.0f }; bool ok{ false }, said{ false }; };
    std::unordered_map<uint32_t, MeleeHand> melee{};

    // gunaxes: per weapon, the game's right hand in the gun's own axes (smoothed; "rhand" lines in fc5vr_gunposes.txt) and, standing,
    // how often the game held the mesh's +Y within 50 deg of the aim's axis (fwd) or not (side: not a gun's barrel).
    struct Axes { glm::vec3 h{ 0.0f }; int n{ 0 }; uint32_t fwd{ 0 }, side{ 0 }; bool said{ false }, side_said{ false }, prov_said{ false }, origin_said{ false }, refoff_said{ false }; };
    std::unordered_map<uint32_t, Axes> axes{};
    bool      axes_save{ false };
    // gunposecheck: since when the game has held the gun in hand along its axes while its learned pose is off them (relearned after 1 s).
    bool              offaxes_on{ false };
    clock::time_point offaxes_t{};
    // The pose the gun in hand is drawn at (gunaxes' made one, else the learned one), for sightaim and the two-handed hold.
    Pose      held{};
    uint32_t  held_sig{ 0 };
    bool      held_axes{ false };
};

void FarCry5Stereo::gun_still_first_draw()
{
    if (!m_gst) return;
    auto&     s  = *m_gst;
    const int fr = m_frame.load();
    if (s.first_frame == fr) return;
    s.first_frame = fr;
    s.first_have  = false;
    if (!s.gun_cb) return;
    std::lock_guard _{ m_obj_mtx };
    if (auto it = m_obj_consts.find(s.gun_cb); it != m_obj_consts.end() && it->second.size >= 48) {
        std::memcpy(s.first_obj, it->second.f, sizeof(s.first_obj));
        s.first_have = true;
    }
}

void FarCry5Stereo::gun_still_gun_cb(ID3D11Buffer* cb)
{
    if (m_gst) m_gst->gun_cb = cb;
}

void FarCry5Stereo::gun_still_note_support(const glm::vec3& F, const glm::vec3& W)
{
    if (!m_gst) m_gst = new GunStillState{};
    m_gst->hand       = F;
    m_gst->hand_w     = W;
    m_gst->hand_frame = m_frame.load();
}

// sidearm (FarCry5Holster.cpp): is the gun a handgun, by where the game holds its left hand standing (measured above): 1 a handgun (the
// left hand beside or behind the weapon hand in most measures), 0 not (ahead, along the barrel: a rifle, a shotgun, the bow), -1 not
// measured yet (fewer than 20).
int FarCry5Stereo::gun_still_handgun(uint32_t sig) const
{
    if (!m_gst || sig == 0) return -1;
    const auto it = m_gst->refs.find(sig);
    if (it == m_gst->refs.end() || it->second.ahead + it->second.beside < 20) return -1;
    return it->second.beside > 3 * it->second.ahead ? 1 : 0;
}

// twohandline (author 21 Oct 14:02): the two-handed hold points the gun
// along the line from the weapon hand to the left hand. The game holds the Uzi's left hand 15 deg off its barrel's line (the
// rifles about 4), so that gun tilted by as much in a two-handed hold. This is the game's own hand line for the gun in hand,
// in the game camera's axes of its held pose; the hold lines it up with the player's hands instead of the barrel.
bool FarCry5Stereo::gun_still_hand_line(glm::vec3& g)
{
    if (!m_gst || !m_two_hand_line.load()) return false;
    const uint32_t sig  = m_weapon_sig.load();
    const auto     it   = m_gst->refs.find(sig);
    const bool     have = it != m_gst->refs.end() && it->second.ok;
    const bool     made = m_gst->held_axes && m_gst->held_sig == sig;   // gunaxes: the gun drawn at the made pose
    // twohandlevel (Stereo.h): a long gun lines up the common hand line, at once (no need to have stood still with it first).
    if (m_two_hand_level.load() && (made || have) && data_long_gun(sig)) {
        g = glm::normalize(glm::mat3_cast(made ? m_gst->held.r : it->second.pose.r) * line_of(m_two_hand_level_ref[0].load(), m_two_hand_level_ref[1].load()));
        return g.z < -0.5f;
    }
    if (!have || !it->second.line_ok) return false;
    g = glm::normalize(glm::mat3_cast(made ? m_gst->held.r : it->second.pose.r) * it->second.line);
    return g.z < -0.5f;   // forward-ish only (a bad measure is ignored)
}

// onehandmatch (Stereo.h): called from update_motion right after the two-handed hold has set k (the weapon's frame: weapon controller with
// grip pitch, world scale in the position). k_ctrl: the weapon controller's own frame. can: a weapon in hand on its controller (not slung,
// bow, mounted, vehicle). two: the two-handed hold is on. Nothing is learnt: the one-hand line is the fixed onehandmatch P Y.
void FarCry5Stereo::one_hand_match(glm::mat4& k, const glm::mat4& k_ctrl, bool can, bool two)
{
    using clock    = std::chrono::steady_clock;
    const auto now = clock::now();
    float      dt  = m_ohm_t == clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - m_ohm_t).count();
    m_ohm_t        = now;
    dt             = std::clamp(dt, 0.0f, 0.1f);
    if (m_gsl_on.load() && std::chrono::duration<float>(now - m_gsl_t0).count() >= 8.0f) gunstock_learn_finish();
    const uint32_t sig = m_weapon_sig.load();
    // The gun's own barrel in the weapon controller's axes as its one-hand hold draws it (gun_still's pose this frame, axes or pose).
    glm::vec3 b{ 0.0f };
    if (m_gst && sig != 0 && m_gst->held_sig == sig) b = glm::mat3_cast(m_gst->held.r) * glm::vec3{ 0.0f, 1.0f, 0.0f };
    const bool b_ok = glm::length(b) > 0.5f;
    // gunstocklearn: from 1.5 s into the window, the drawn barrel of a long gun in two hands (weapon controller's axes; k is the hold's here),
    // not while firing or reloading. Sampled with the switch off too.
    if (m_gsl_on.load() && two && can && b_ok && data_long_gun(sig) && !fc5::gesture::kept() && std::chrono::duration<float>(now - m_gsl_t0).count() > 1.5f &&
        !m_trigger.load() && !fc5::reload::hides_game_reload() && !(m_gst && m_gst->reload)) {
        std::lock_guard _{ m_gsl_mtx };
        m_gsl_samples.push_back(glm::normalize(glm::transpose(glm::mat3{ k_ctrl }) * glm::mat3{ k } * glm::normalize(b)));
        m_gsl_good_s += dt;
    }
    if (!m_ohm_on.load() || !can || !b_ok || !data_long_gun(sig) || fc5::gesture::kept()) {
        m_ohm_cur_ok = m_ohm_was_two = false;
        m_ohm_sig                    = sig;
        m_ohm_last_turn              = 0.0f;
        return;
    }
    b = glm::normalize(b);
    if (sig != m_ohm_sig) { m_ohm_cur_ok = false; m_ohm_sig = sig; }   // another gun: no easing from the last one's turn
    const glm::mat3 Kc{ k_ctrl };
    auto            put = [&](const glm::quat& q) {
        const glm::mat3 K = Kc * glm::mat3_cast(q);   // turned about the controller's point: the grip point, where the weapon hand is
        k[0]              = glm::vec4{ K[0], 0.0f };
        k[1]              = glm::vec4{ K[1], 0.0f };
        k[2]              = glm::vec4{ K[2], 0.0f };
    };
    const float ease = 1.0f - std::exp(-dt / 0.05f);   // ~95 % in 0.15 s
    if (two) {
        // The grab: eased from the one-hand turn into the two-handed hold's (the hold itself is not changed).
        if (!m_ohm_was_two) m_ohm_two_t0 = now;
        const float     age = std::chrono::duration<float>(now - m_ohm_two_t0).count();
        const glm::quat qn  = glm::normalize(glm::quat_cast(glm::transpose(Kc) * glm::mat3{ k }));
        if (m_ohm_cur_ok && age < 0.15f) {
            m_ohm_cur = glm::normalize(glm::slerp(m_ohm_cur, qn, ease));
            put(m_ohm_cur);
        } else {
            m_ohm_cur = qn;
        }
        m_ohm_cur_ok    = true;
        m_ohm_was_two   = true;
        m_ohm_last_turn = 0.0f;
        return;
    }
    const bool let_go = m_ohm_was_two;
    m_ohm_was_two     = false;
    const float     pt = glm::radians(std::clamp(m_ohm_p.load(), -80.0f, 80.0f)), yt = glm::radians(std::clamp(m_ohm_y.load(), -80.0f, 80.0f));
    const glm::vec3 d{ std::sin(yt) * std::cos(pt), std::sin(pt), -std::cos(yt) * std::cos(pt) };
    glm::quat       r   = glm::normalize(glm::quat{ 1.0f + glm::dot(b, d), glm::cross(b, d) });   // shortest turn: the barrel onto the line (the roll kept)
    const float     ang = glm::degrees(2.0f * std::acos(std::clamp(std::abs(r.w), 0.0f, 1.0f)));
    if (ang > 45.0f) r = glm::normalize(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, r, 45.0f / ang));
    m_ohm_cur       = m_ohm_cur_ok ? glm::normalize(glm::slerp(m_ohm_cur, r, ease)) : r;   // the let-go eased from the hold; a new draw at once
    m_ohm_cur_ok    = true;
    m_ohm_last_turn = std::min(ang, 45.0f);
    put(m_ohm_cur);
    if (let_go) {   // information only: nothing is learnt from the hold
        float by = 0.0f, bp = 0.0f, dy = 0.0f, dp = 0.0f;
        dir_angles(b, by, bp);
        dir_angles(d, dy, dp);
        m_ohm_last_x = bp - dp;
        spdlog::info("[Motion] Angle correction for two handed weapons (gunstock calibration): weapon {} let go, 1H barrel turned {:.1f} {:.1f} deg onto the two-handed line (raw 1H would be {:.1f} deg above){}", sig, dp - bp,
                     dy - by, bp - dp, ang > 45.0f ? fmt::format("; the turn {:.0f} deg held to 45", ang) : std::string{});
    }
}

// reloadzonesfollow (Stereo.h): the drawn gun's turn against the weapon controller's raw axes (set in update_motion), false when none.
bool FarCry5Stereo::draw_turn(glm::mat3& E)
{
    std::lock_guard _{ m_draw_turn_mtx };
    E = m_draw_turn;
    return m_draw_turn_on;
}

// reloadzonesfollow: k with the one-hand turn last applied (onehandmatch), for the press tests made before one_hand_match runs this frame.
// k itself when none is applied (onehandmatch off, two hands, not a long gun): bit for bit as before.
glm::mat4 FarCry5Stereo::one_hand_drawn(const glm::mat4& k) const
{
    if (!m_ohm_on.load() || !m_ohm_cur_ok || m_ohm_was_two || m_ohm_last_turn.load() <= 0.0f) return k;
    glm::mat4       r = k;
    const glm::mat3 K = glm::mat3{ k } * glm::mat3_cast(m_ohm_cur);
    r[0]              = glm::vec4{ K[0], 0.0f };
    r[1]              = glm::vec4{ K[1], 0.0f };
    r[2]              = glm::vec4{ K[2], 0.0f };
    return r;
}

void FarCry5Stereo::gunstock_learn_start()
{
    {
        std::lock_guard _{ m_gsl_mtx };
        m_gsl_samples.clear();
        m_gsl_good_s = 0.0f;
        m_gsl_msg    = "learning...";
    }
    m_gsl_t0 = std::chrono::steady_clock::now();
    m_gsl_on = true;
    gunstock_tick();
    spdlog::info("[Motion] gunstock calibration: learning for 8 s: hold a two-handed weapon and aim with it");
}

void FarCry5Stereo::gunstock_learn_finish()
{
    m_gsl_on = false;
    std::vector<glm::vec3> s;
    float                  good = 0.0f;
    {
        std::lock_guard _{ m_gsl_mtx };
        s    = std::move(m_gsl_samples);
        good = m_gsl_good_s;
        m_gsl_samples.clear();
    }
    gunstock_tick();
    if (good < 2.0f || s.empty()) {
        std::lock_guard _{ m_gsl_mtx };
        m_gsl_msg = fmt::format("not learnt: hold a two-handed weapon and aim with it ({:.1f} s of two-handed aim, 2 s needed); the angle kept", good);
        spdlog::info("[Motion] gunstock calibration not learnt: hold a two-handed weapon and aim with it ({:.1f} s of samples, 2 s needed); kept {:.1f} up {:.1f} right", good,
                     m_ohm_p.load(), m_ohm_y.load());
        return;
    }
    glm::vec3 m{ 0.0f };
    for (const auto& v : s) m += v;
    m = glm::normalize(m);
    float sq = 0.0f;
    for (const auto& v : s) {
        const float a = glm::degrees(std::acos(std::clamp(glm::dot(v, m), -1.0f, 1.0f)));
        sq += a * a;
    }
    const float spread = std::sqrt(sq / float(s.size()));
    float       y = 0.0f, p = 0.0f;
    dir_angles(m, y, p);
    m_ohm_p                   = std::clamp(p, -45.0f, 30.0f);
    m_ohm_y                   = std::clamp(y, -20.0f, 20.0f);
    m_ohm_cur_ok              = false;
    m_persist["onehandmatch"] = fmt::format("onehandmatch {} {:.1f} {:.1f}", m_ohm_on.load() ? "on" : "off", m_ohm_p.load(), m_ohm_y.load());
    save_settings();
    {
        std::lock_guard _{ m_gsl_mtx };
        m_gsl_msg = fmt::format("learnt: {:.1f} deg up, {:.1f} right (from {} samples, spread {:.1f} deg)", m_ohm_p.load(), m_ohm_y.load(), s.size(), spread);
    }
    spdlog::info("[Motion] gunstock calibration learnt: {:.1f} up {:.1f} right from {} samples (spread {:.1f} deg){}", m_ohm_p.load(), m_ohm_y.load(), s.size(), spread,
                 (p != m_ohm_p.load() || y != m_ohm_y.load()) ? fmt::format(" (measured {:.1f} {:.1f}, held to the sliders' range)", p, y) : std::string{});
}

std::string FarCry5Stereo::gunstock_learn_text()
{
    if (m_gsl_on.load()) {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_gsl_t0).count();
        float       good;
        {
            std::lock_guard _{ m_gsl_mtx };
            good = m_gsl_good_s;
        }
        return fmt::format("Learning: hold a two-handed weapon and aim with it: {:.0f} s left ({:.1f} s of two-handed aim so far)", std::max(0.0f, std::ceil(8.0f - t)), good);
    }
    std::lock_guard _{ m_gsl_mtx };
    return m_gsl_msg;
}

std::string FarCry5Stereo::one_hand_match_text()
{
    return fmt::format("every rifle, SMG, shotgun and machine gun held {:.1f} deg up, {:.1f} right of your weapon hand's controller (default {:.0f} {:.0f}); turn now {:.1f} deg{}",
                       m_ohm_p.load(), m_ohm_y.load(), kOhmDefaultP, kOhmDefaultY, m_ohm_last_turn.load(),
                       m_ohm_last_x.load() != 0.0f ? fmt::format("; at the last let-go the raw one-hand hold was {:.1f} deg above", m_ohm_last_x.load()) : std::string{});
}
std::string FarCry5Stereo::gun_still_hand_line_text()
{
    if (!m_gst) return "its own hand line";
    const uint32_t sig = m_weapon_sig.load();
    const auto     it  = m_gst->refs.find(sig);
    std::string    own = "its own line not measured yet";
    if (it != m_gst->refs.end() && it->second.line_ok) {
        float r = 0.0f, u = 0.0f;
        line_angles(it->second.line, r, u);
        own = fmt::format("its own {:.1f} right, {:.1f} up", r, u);
    }
    if (m_two_hand_level.load() && data_long_gun(sig))
        return fmt::format("the common hand line (twohandlevel: {:.1f} deg right, {:.1f} up of the barrel; {})", m_two_hand_level_ref[0].load(), m_two_hand_level_ref[1].load(), own);
    return fmt::format("its own hand line ({})", own);
}

// weaponaudit (test; all-weapons pass 7 Nov): one [Audit] line for the gun in hand, from gun_still's frame (s.held just set). Points in the
// weapon controller's axes (grip pitch in: the axes the gun is placed in, game metres), then in the raw controller's axes and tracking metres
// (as the test commands ctrl / lctrl take them: "track"). With two hands: the turn the hold gave the weapon's frame against the one-hand
// frame, and the barrel it leaves.
void FarCry5Stereo::gun_still_audit(uint32_t sig, bool axes_hold, bool melee_hold)
{
    if (!m_gst) return;
    auto&       s    = *m_gst;
    const auto* w    = fc5::gameweapon::info(sig);
    const Pose& H    = s.held;
    const auto  it   = s.refs.find(sig);
    const bool  have = it != s.refs.end() && it->second.ok;
    const auto  ait  = s.axes.find(sig);
    const bool  rh_ok = ait != s.axes.end() && ait->second.n >= 3;
    const float gp   = m_grip_pitch.load();
    const float ws   = std::max(1e-3f, m_world_scale.load());
    const glm::mat3 Rg = glm::mat3_cast(glm::angleAxis(glm::radians(gp), glm::vec3{ 1.0f, 0.0f, 0.0f }));   // k axes -> the raw controller's
    auto cam = [&](const glm::vec3& p) { return glm::mat3_cast(H.r) * p + H.t; };   // gun axes -> the weapon controller's axes
    auto v3  = [](const glm::vec3& v) { return fmt::format("{:.3f} {:.3f} {:.3f}", v.x, v.y, v.z); };
    const glm::vec3 b = glm::mat3_cast(H.r) * glm::vec3{ 0.0f, 1.0f, 0.0f }, u = glm::mat3_cast(H.r) * glm::vec3{ 0.0f, 0.0f, 1.0f };
    float by = 0.0f, bp = 0.0f;
    dir_angles(b, by, bp);
    const float roll = glm::degrees(std::atan2(u.x, u.y));
    std::string sight = "none";
    if (glm::vec3 sd{ 0.0f }, su{ 0.0f }; sight_axes(sig, sd, su) && glm::length(sd) > 0.5f) {
        float r = 0.0f, uu = 0.0f;
        line_angles(glm::normalize(sd), r, uu);
        sight = fmt::format("{:.2f} {:.2f}", r, uu);
    }
    glm::vec3 c{ m_grip_hand_ref[0].load(), m_grip_hand_ref[1].load(), m_grip_hand_ref[2].load() };
    bool      grip_ok = false;
    {
        std::lock_guard _{ m_weapon_cal_mtx };
        if (auto g = m_grip_hand.find(sig); g != m_grip_hand.end() && g->second.ok) { c = g->second.c; grip_ok = true; }
    }
    const char* hold = melee_hold ? "melee" : axes_hold ? "axes" : (s.alpha > 0.5f ? "pose" : "game");
    std::string rh = "rhand none", lh = "lhand none", line = "line none";
    if (rh_ok) rh = fmt::format("rhand {} n {} rhctrl {}", v3(ait->second.h), ait->second.n, v3(cam(ait->second.h)));
    if (have && it->second.hand_ok) {
        const glm::vec3 lc = cam(it->second.hand);
        // The game's camera-space points are put on the controller about the grip point O (Stereo.cpp's weapon placement: world' = W_pos +
        // W_rot (q - O), O = gripoff + this gun's grip - the reference grip): the left hand as drawn, against the weapon controller, is lc - O.
        const glm::vec3 O = glm::vec3{ m_grip_off[0].load(), m_grip_off[1].load(), m_grip_off[2].load() } +
                            (m_grip_by_hand.load() ? c - glm::vec3{ m_grip_hand_ref[0].load(), m_grip_hand_ref[1].load(), m_grip_hand_ref[2].load() } : glm::vec3{ 0.0f });
        const glm::vec3 ld = lc - O;
        lh = fmt::format("lhand {} lhctrl {} lhtrack {} lhdrawn {} lhdtrack {} fore {:.2f}", v3(it->second.hand), v3(lc), v3(Rg * lc / ws), v3(ld), v3(Rg * ld / ws),
                         glm::length(ld - glm::vec3{ 0.0f, 0.0f, -m_fore_grip.load() }));
    }
    if (have && it->second.line_ok) {
        float r = 0.0f, uu = 0.0f;
        line_angles(it->second.line, r, uu);
        line = fmt::format("line {:.2f} {:.2f} ahead {} beside {}", r, uu, it->second.ahead, it->second.beside);
    } else if (have) line = fmt::format("line none ahead {} beside {}", it->second.ahead, it->second.beside);
    const bool  level = m_two_hand_level.load() && data_long_gun(sig);
    std::string two   = "two off";
    if (m_frame.load() - m_aud_frame <= 10) {
        const glm::mat3 kc{ m_aud_kc }, k{ m_aud_k };
        const glm::mat3 rel = glm::transpose(kc) * k;   // the weapon's frame in the one-hand frame's axes
        float ty = 0.0f, tp = 0.0f, b2y = 0.0f, b2p = 0.0f;
        dir_angles(rel * glm::vec3{ 0.0f, 0.0f, -1.0f }, ty, tp);
        dir_angles(rel * b, b2y, b2p);
        const glm::vec3 up2 = rel * glm::vec3{ 0.0f, 1.0f, 0.0f };
        const glm::vec3 o   = m_aud_other_ok ? glm::transpose(kc) * (m_aud_other - glm::vec3{ m_aud_kc[3] }) : glm::vec3{ 0.0f };
        two = fmt::format("two {} turn {:.2f} {:.2f} {:.2f} barrel2 {:.2f} {:.2f} other {} othertrack {}", m_aud_two ? "on" : "off", ty, tp, glm::degrees(std::atan2(up2.x, up2.y)), b2y, b2p,
                          m_aud_other_ok ? v3(o) : std::string{ "- - -" }, m_aud_other_ok ? v3(Rg * o / ws) : std::string{ "- - -" });
    }
    spdlog::info("[Audit] weapon {} item {} cls {} oneh {} | hold {} sight {} | barrel {:.2f} {:.2f} roll {:.2f} grippitch {:.1f} ws {:.3f} | grip {} {} gripraw {} | {} | {} | {} level {} ref {:.2f} {:.2f} | {} | handgun {} pose {} builtin {}",
                 sig, w ? w->name : "-", w ? int(w->cls) : -1, w ? int(w->one_handed) : -1, hold, sight, by, bp, roll, gp, ws, grip_ok ? "measured" : "ref", v3(c), v3(Rg * c / ws), rh, lh, line,
                 level ? "on" : "off", m_two_hand_level_ref[0].load(), m_two_hand_level_ref[1].load(), two, gun_still_handgun(sig), have ? v3(it->second.pose.t) : std::string{ "none" },
                 have && it->second.builtin ? "yes" : "no");
}

void FarCry5Stereo::note_pad_buttons(uint16_t buttons)
{
    // The time of the last press of X (reload / interact, rising edge) and the last poll with the left stick pressed (sprint).
    static uint16_t was = 0;
    const int64_t   t   = clock::now().time_since_epoch().count();
    if ((buttons & XINPUT_GAMEPAD_X) && !(was & XINPUT_GAMEPAD_X)) m_pad_x_press = t;
    if (buttons & XINPUT_GAMEPAD_LEFT_THUMB) m_pad_sprint_press = t;   // held (stick fully forward in the new layout) counts all along
    was = buttons;
}

// offhandstill (FarCry5EmptyHands.cpp): a reload under way (the game's animation shown), or (not reload_only) a shot in the last 0.8 s.
bool FarCry5Stereo::gun_still_busy(bool reload_only) const
{
    return m_gst && (m_gst->reload || (!reload_only && clock::now() - m_gst->last_fire < std::chrono::milliseconds(800)));
}

// sprinthead (FarCry5Walk.cpp): sprinting as gunstill sees it (the left stick pressed since the player was last slower than 1 m/s).
bool FarCry5Stereo::player_sprinting() const { return m_gst && m_gst->sprinting; }

// The player's speed (world Z up, smoothed; frames sharing a camera, stereo pairs, are skipped) and sprint: every weapon pass,
// with a gun or not (emptyhandstill holds the empty hands while moving), gunstill on or off.
void FarCry5Stereo::gun_still_pace(const glm::vec3& cam)
{
    if (!m_gst) m_gst = new GunStillState{};
    auto&      s   = *m_gst;
    const auto now = clock::now();
    {
        const float     dt = s.spd_have ? std::chrono::duration<float>(now - s.spd_t).count() : 0.0f;
        const glm::vec2 d{ cam.x - s.spd_cam.x, cam.y - s.spd_cam.y };
        if (!s.spd_have || dt > 0.25f) {
            s.spd_cam = cam; s.spd_t = now; s.spd_have = true;
        } else if (glm::length(d) > 1e-5f && dt > 1e-3f) {
            s.speed += (std::min(glm::length(d) / dt, 20.0f) - s.speed) * std::min(1.0f, dt / 0.15f);
            s.spd_cam = cam; s.spd_t = now;
        } else if (dt > 0.1f) {
            s.speed *= 0.5f;
        }
    }
    // Sprinting: the sprint input (the left stick pressed, or pushed fully forward in the new layout) since the player was last
    // below 1 m/s.
    if (s.speed < 1.0f) s.last_slow = now;
    const bool was_sprinting = s.sprinting;
    s.sprinting              = clock::time_point{ clock::duration{ m_pad_sprint_press.load() } } > s.last_slow;
    if (s.sprinting != was_sprinting) spdlog::info("[Motion] gunstill: {} (speed {:.1f} m/s)", s.sprinting ? "sprinting" : "no longer sprinting", s.speed);
    if (s.sprinting || s.speed >= 1.0f) s.last_moving = now;
}

float FarCry5Stereo::gun_still_speed(bool& sprinting, float& since_moving_s) const
{
    if (!m_gst) { sprinting = false; since_moving_s = 1e6f; return 0.0f; }
    sprinting      = m_gst->sprinting;
    since_moving_s = m_gst->last_moving == clock::time_point{} ? 1e6f : std::chrono::duration<float>(clock::now() - m_gst->last_moving).count();
    return m_gst->speed;
}

glm::mat4 FarCry5Stereo::gun_still(int frame, const glm::vec3& cam)
{
    if (!m_gst) m_gst = new GunStillState{};
    auto&      s   = *m_gst;
    if (!s.loaded) {
        s.loaded = true;
        gun_still_load();
    }
    if (m_grip_save_pending.exchange(false)) gun_still_save();   // a grip point just measured (gripbyhand)
    const auto now = clock::now();
    const float da = std::clamp(std::chrono::duration<float>(now - s.alpha_t).count(), 0.0f, 0.1f);
    s.alpha_t      = now;

    // Speed and sprint: gun_still_pace, called before this for every weapon pass.
    if (m_trigger.load() || m_dbg_trigger.load()) s.last_fire = now;
    if (m_lt_sent.load()) s.last_ads = now;
    const bool throwing = m_rb_down.load() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle;
    if (throwing) s.last_throw = now;
    // reloadmanual (FarCry5Reload.cpp): the magazine changed by hand while the game's own reload runs unseen: the gun stays held (no
    // "reload" shown), and nothing is learnt from the game's pose meanwhile (its reload animation moves the gun and its left hand).
    const bool manual_reload = fc5::reload::hides_game_reload();

    const auto& hm = m_hand_map;   // the pass the gun was measured in
    // gunstilllate: this pass's gun pose estimated from a part drawn before the gun mesh (weapon_draw_impl), this pass's camera.
    const bool est  = m_gst_est_frame == frame;
    const int  seen = est ? frame : m_gun_obj_seen;
    if (!m_motion.load() || seen < frame - 3 || hm.frame != seen) {
        s.alpha = 0.0f;
        return glm::mat4{ 1.0f };
    }
    glm::mat4 Om{ 1.0f };
    const float* obj = est ? m_gst_est_obj : m_gun_obj_now;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) Om[c][r] = obj[r * 4 + c];
    glm::mat4 A{ hm.rot };
    A[3]              = glm::vec4{ -(hm.rot * hm.cam), 1.0f };
    const glm::mat4 Q = A * Om;   // the gun's pose in the game camera's axes, as the game animates it

    // One step per game update (a stereo pair, or each frame in AFW): learning, the reload state, the update-to-update
    // change for the prediction.
    auto       upd        = [&](int f) { return afw_mode() ? f : (f & ~1); };
    if (upd(frame) != s.upd_u[0]) {
        for (int i = 2; i > 0; --i) { s.upd_u[i] = s.upd_u[i - 1]; s.upd_t[i] = s.upd_t[i - 1]; }
        s.upd_u[0] = upd(frame);
        s.upd_t[0] = now;
    }
    // (gunstilllate's guess from a part is not a measurement: the gun mesh's own pose is measured when it is drawn.)
    const bool new_update = !est && upd(m_gun_obj_seen) != upd(s.meas_frame);
    if (!est && s.meas_frame != m_gun_obj_seen) {
        if (new_update) {
            s.have_prev  = upd(m_gun_obj_seen) - upd(s.meas_frame) <= 2;
            s.q_prev     = s.q_last;
            s.q_prev_upd = upd(s.meas_frame);
        }
        // gunstillstat: each way's guess for a predicted pass against the pose the game then drew it with.
        if (s.pred_frame == m_gun_obj_seen) {
            float e_cm[4], e_deg[4];
            bool  sane = true;
            for (int k = 0; k < 4; ++k) {
                const Pose e = to_pose(glm::inverse(s.pred_q[k]) * Q);
                e_cm[k]      = 100.0f * glm::length(e.t);
                e_deg[k]     = angle_deg(e.r, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f });
                sane         = sane && e_cm[k] < 30.0f;   // not a weapon switch or a teleport
            }
            if (sane) {
                s.pr_n++;
                for (int k = 0; k < 4; ++k) {
                    s.pr_cm[k] += e_cm[k];
                    s.pr_deg[k] += e_deg[k];
                    s.pr_max_cm[k]  = std::max(s.pr_max_cm[k], e_cm[k]);
                    s.pr_max_deg[k] = std::max(s.pr_max_deg[k], e_deg[k]);
                }
            }
            s.pred_frame = -100;
        }
        // Probe (20 Oct, firing jolts in AFW): was the gun's transform already this pass's at the pass's first draw? Judged
        // only when the gun moved since the last measurement.
        if (s.first_have && s.first_frame == m_gun_obj_seen && s.meas_frame >= 0) {
            auto same = [](const float* a, const float* b) { for (int i = 0; i < 12; ++i) if (std::abs(a[i] - b[i]) > 1e-6f) return false; return true; };
            if (!same(m_gun_obj_now, s.prev_obj)) {
                if (same(s.first_obj, m_gun_obj_now)) s.first_current++;
                else if (same(s.first_obj, s.prev_obj)) s.first_stale++;
                else s.first_other++;
            }
        }
        std::memcpy(s.prev_obj, m_gun_obj_now, sizeof(s.prev_obj));
        s.q_last     = Q;
        s.meas_frame = m_gun_obj_seen;
        // gunstillstat: the gun as drawn in that pass (the correction given to it applied to its measured pose) against its
        // standing pose, and the game's own pose against it (what was taken out).
        if (s.applied_frame == m_gun_obj_seen && s.applied_full) {
            if (auto it = s.refs.find(m_weapon_sig.load()); it != s.refs.end() && it->second.ok) {
                const Pose drawn = to_pose(s.applied_C * Q), game = to_pose(Q);
                const float dc = 100.0f * glm::length(drawn.t - it->second.pose.t), dd = angle_deg(drawn.r, it->second.pose.r);
                const float gc = 100.0f * glm::length(game.t - it->second.pose.t), gd = angle_deg(game.r, it->second.pose.r);
                s.st_n++;
                s.st_drawn_cm += dc; s.st_drawn_max_cm = std::max(s.st_drawn_max_cm, dc); s.st_drawn_max_deg = std::max(s.st_drawn_max_deg, dd);
                s.last_drawn_cm = dc; s.last_drawn_deg = dd; s.last_drawn_frame = m_gun_obj_seen;
                if (m_gun_obj_seen - s.step_prev_frame <= 2) {
                    const float sc = 100.0f * glm::length(drawn.t - s.step_prev.t), sd = angle_deg(drawn.r, s.step_prev.r);
                    s.st_steps++;
                    s.st_step_cm += sc; s.st_step_max_cm = std::max(s.st_step_max_cm, sc); s.st_step_max_deg = std::max(s.st_step_max_deg, sd);
                }
                s.step_prev       = drawn;
                s.step_prev_frame = m_gun_obj_seen;
                const int e = m_gun_obj_seen & 1;
                s.ey_n[e]++;
                s.ey_cm[e] += dc;
                s.ey_max_cm[e]  = std::max(s.ey_max_cm[e], dc);
                s.ey_max_deg[e] = std::max(s.ey_max_deg[e], dd);
                if (m_gun_obj_seen - s.ey_prev_frame[e] <= 4) {
                    const float sc = 100.0f * glm::length(drawn.t - s.ey_prev[e].t), sd = angle_deg(drawn.r, s.ey_prev[e].r);
                    s.ey_steps[e]++;
                    s.ey_step_cm[e] += sc;
                    s.ey_step_max_cm[e]  = std::max(s.ey_step_max_cm[e], sc);
                    s.ey_step_max_deg[e] = std::max(s.ey_step_max_deg[e], sd);
                }
                if (!afw_mode() && e == 1 && s.ey_prev_frame[0] == m_gun_obj_seen - 1) {
                    const float pc = 100.0f * glm::length(drawn.t - s.ey_prev[0].t), pd = angle_deg(drawn.r, s.ey_prev[0].r);
                    s.pd_n++;
                    s.pd_cm += pc;
                    s.pd_max_cm  = std::max(s.pd_max_cm, pc);
                    s.pd_max_deg = std::max(s.pd_max_deg, pd);
                }
                s.ey_prev[e]       = drawn;
                s.ey_prev_frame[e] = m_gun_obj_seen;
                s.st_game_cm += gc;  s.st_game_max_cm = std::max(s.st_game_max_cm, gc);   s.st_game_max_deg = std::max(s.st_game_max_deg, gd);
            }
        }
    }

    const uint32_t sig = m_weapon_sig.load();
    if (sig != s.sig) {
        if (s.sig != 0 && sig != 0) {
            bool grip = false;
            {
                std::lock_guard _{ m_weapon_cal_mtx };
                auto it = m_grip_hand.find(sig);
                grip    = it != m_grip_hand.end() && it->second.ok;
            }
            const auto r = s.refs.find(sig);
            spdlog::info("[Motion] gunstill: weapon {} -> {} (pose {}, grip point {})", s.sig, sig, r != s.refs.end() && r->second.settled ? "known" : "not known yet",
                         grip ? "known" : "not measured yet");
        }
        // A weapon change ends a "reload" (the author's 14:26 and 14:27 log: shots, then a switch within 1.5 s, its holster
        // animation shown as a reload for 0.5 s): the new weapon is held at its own pose.
        if (s.reload && s.sig != 0) {
            s.reload = false;
            spdlog::info("[Motion] gunstill: reload over (weapon {} -> {}): the new weapon held at its standing pose", s.sig, sig);
        }
        // gunstillswitch (author 5 Nov 13:08; his 13:59-14:00 log): the reload button held changes weapon (from
        // 350 ms, reload_button_pass), and its press, armed for 1 s to start a reload once the gun leaves its standing pose (gunstillpad), started
        // one on the weapon it brought, still in its draw: "reload (pad X; hand 0 cm from its place, gun 19 cm / 28 deg from its standing pose)"
        // on the 1887 Modern 32472 at 13:59:18.998, on 18912 at 13:59:15.725. The game's animation was shown instead of the held gun (0.5 s on
        // 18912, 6 s on 32472). A press from before this weapon came out is not its reload.
        if (m_gst_switch_hold.load() && s.pad_t != clock::time_point{} && now - s.pad_t < std::chrono::seconds(1) && s.sig != 0 && sig != 0) {
            s.pad_t = clock::time_point{};
            s.switch_skips++;
            spdlog::info("[Motion] gunstillswitch: weapon {} -> {}: the X press of the change is no reload of the new weapon (held at its pose from the start)", s.sig, sig);
        }
        s.sig   = sig;
        s.sig_t = now;
    }
    auto&       ref     = s.refs[sig];
    const float out_s   = std::chrono::duration<float>(now - s.sig_t).count();
    const bool  vehicle = m_in_vehicle.load();
    const Pose  qp      = to_pose(Q);
    // sightaim (FarCry5Sight.cpp): the game's pose while it aims down sights gives the weapon's sight line.
    sight_note_pose(sig, Q, new_update, !s.reload && !throwing && !manual_reload);

    if (new_update) {
        // The game's left hand in the gun's own axes (same pass as the gun), against where it sits standing.
        const bool      hand_fresh = s.hand_frame == m_gun_obj_seen;
        const glm::vec3 hand_g     = hand_fresh ? glm::vec3{ glm::inverse(Q) * glm::vec4{ s.hand, 1.0f } } : glm::vec3{ 0.0f };
        s.hand_off_cm              = (hand_fresh && ref.hand_ok) ? 100.0f * glm::length(hand_g - ref.hand) : 0.0f;
        // gunstillswitch: a gun held by its axes (gunaxes, gunposecheck) whose learned pose is off them is not held where that pose says the
        // game holds it standing, and the game no longer holds it there: the 1887 Modern 32472's pose, saved 31 deg off its barrel, rolled 8,
        // while the game holds the gun 23 cm / 30 deg from it (close to the camera, z -0.04, where no pose is learned again). Measured from it,
        // the gun was always "away", so any press of X was a reload that lasted until its 6 s limit, the game's animation shown all along,
        // then the pose was "dropped" back to the same first pose. Such a gun is judged as one with no pose: X counts once the game's left
        // hand leaves the gun (manual reload guns hide the game's reload anyway).
        const bool ref_off_axes    = m_gst_switch_hold.load() && ref.ok && !ref.temp && s.held_axes && s.held_sig == sig && pose_off_axes(ref.pose.r) && data_gun(sig);
        if (ref_off_axes && !s.axes[sig].refoff_said) {
            s.axes[sig].refoff_said = true;
            float       rl = 0.0f;
            const float ol = axes_off_deg(ref.pose.r, rl);
            spdlog::info("[Motion] gunstillswitch: weapon {} held by its axes, its learned pose {:.0f} deg off them, rolled {:.0f} deg, the game's gun {:.0f} cm / {:.0f} deg "
                         "from it: that pose no longer tells a reload (the X press, the gun's distance from it)",
                         sig, ol, rl, 100.0f * glm::length(qp.t - ref.pose.t), angle_deg(qp.r, ref.pose.r));
        }
        s.dev_cm                   = ref.ok && !ref_off_axes ? 100.0f * glm::length(qp.t - ref.pose.t) : 0.0f;
        s.dev_deg                  = ref.ok && !ref_off_axes ? angle_deg(qp.r, ref.pose.r) : 0.0f;
        const bool hand_off        = s.hand_off_cm > m_gst_hand_cm.load();
        s.hand_off_n               = hand_off ? s.hand_off_n + 1 : 0;

        // Reload: the pad's X (reload / interact), or the game's left hand off the gun on foot (the game's own reloads).
        const int64_t press = m_pad_x_press.load();
        if (!s.pad_init) s.pad_seen = press;   // a press from before the first held gun (menus) is no reload
        s.pad_init          = true;
        const bool    pad   = press != s.pad_seen && press != 0;
        s.pad_seen          = press;
        // Sprinting (17 Oct, author): the game's sprint pose takes its
        // left hand 20-30 cm off the gun, which read as a reload (his 11:52-11:55 log: 1 s "reloads" over and over), so the
        // sprint pose was shown. While sprinting (the left stick pressed since last below 1 m/s) or faster than 3 m/s, the
        // hand counts only right after shooting (the game's own reload after the last round); pad X always counts.
        // (The sprint flag and last_moving: gun_still_pace.)
        const bool fast_move = s.sprinting || s.speed > 3.0f;
        const bool fired     = now - s.last_fire < std::chrono::milliseconds(1500);
        // gunstillshot (author 19 Oct): standing still, the game also takes its
        // left hand 23-43 cm off the gun for about a second with no shot and no reload press (his 12:49-13:08 log: 41 of the 45
        // hand-off "reloads", most with the weapon on the sling, speed 0; it lowers the gun 7-10 cm / 12-22 deg), and each
        // showed the game's animation for 1 s. Now the hand counts only right after shooting, at any speed; pad X always counts.
        const bool need_shot = g_hand_needs_shot.load() || fast_move;
        // Not in the first second of a weapon (its draw animation after a switch right after shooting).
        const bool by_hand   = !vehicle && s.hand_off_n >= 2 && (!need_shot || fired) && out_s > 1.0f;
        if (!vehicle && s.hand_off_n == 2 && need_shot && !fired && !s.reload && !manual_reload) {
            s.hand_skips++;
            spdlog::info("[Motion] gunstill: the game's left hand left the gun with no shot in the last 1.5 s (hand {:.0f} cm from its place, gun {:.0f} cm / {:.0f} deg "
                         "from its standing pose; speed {:.1f} m/s{}): no reload, the gun stays held",
                         s.hand_off_cm, s.dev_cm, s.dev_deg, s.speed, s.sprinting ? ", sprinting" : "");
        }
        // gunstillpad (author 19 Oct 14:22-14:33 log: X pressed to interact or right after a sprint showed the game's pose of that
        // moment, 15-59 deg off, for 0.8 s with the hand still on the gun): X counts once the hand leaves within 1 s; with no
        // hand to watch for this weapon, once the gun leaves its standing pose (4 cm / 8 deg).
        if (pad) s.pad_t = now;
        const bool hand_seen_now = hand_fresh && ref.hand_ok;
        const bool pad_go        = !g_pad_waits_hand.load() ? pad
                                   : (now - s.pad_t < std::chrono::seconds(1) && s.pad_t != clock::time_point{} &&
                                      (hand_seen_now ? s.hand_off_n >= 2 : (s.dev_cm > 4.0f || s.dev_deg > 8.0f)));
        if (manual_reload && s.reload) {
            s.reload = false;
            spdlog::info("[Motion] gunstill: reload over: reloadmanual holds the gun while the game reloads unseen");
        }
        if (!s.reload && !manual_reload && (pad_go || by_hand)) {
            if (pad_go) s.pad_t = clock::time_point{};
            s.reload     = true;
            s.reload_why = pad_go ? "pad X" : "the game's left hand left the gun";
            s.reload_t0  = now;
            s.active_t   = now;
            s.reloads++;
            spdlog::info("[Motion] gunstill: reload ({}; hand {:.0f} cm from its place, gun {:.0f} cm / {:.0f} deg from its standing pose; speed {:.1f} m/s{}): the game's animation shown",
                         s.reload_why, s.hand_off_cm, s.dev_cm, s.dev_deg, s.speed, s.sprinting ? ", sprinting" : "");
        }
        if (s.reload) {
            // Under way while the hand is off the gun (or, with no hand to watch, the gun away from its standing pose).
            const bool hand_seen = hand_fresh && ref.hand_ok;
            const bool busy      = hand_seen ? hand_off : (s.dev_cm > 4.0f || s.dev_deg > 8.0f);
            if (busy) s.active_t = now;
            const float t     = std::chrono::duration<float>(now - s.reload_t0).count();
            const float quiet = std::chrono::duration<float>(now - s.active_t).count();
            // A press with nothing following (interact, a full magazine) ends after 0.8 s; a reload once quiet for 0.3 s.
            const bool  ended = t > 6.0f || (s.active_t == s.reload_t0 ? t > 0.8f : (quiet > 0.3f && t > 0.5f));
            if (ended) {
                s.reload = false;
                spdlog::info("[Motion] gunstill: reload over after {:.1f} s: the gun held at its standing pose again", t);
                if (t > 6.0f) {
                    // No reload lasts 6 s: the held pose is wrong for this weapon (the gun stayed away from it). Learned again.
                    GunStillState::drop(ref);
                    spdlog::info("[Motion] gunstill: weapon {}'s standing pose dropped (the game's gun stayed {:.0f} cm / {:.0f} deg from it): {}", sig, s.dev_cm, s.dev_deg, ref.first_ok ? "back to its first learned pose" : "learned again");
                }
            }
        }
        // The hand off its place for 6 s on foot, below a walk, with no reload: this weapon's learned pose or hand place is wrong
        // (until 19 Oct such a hand started a "reload" that reached 6 s and dropped it). Learned again.
        if (!hand_off || vehicle || s.reload || manual_reload || fast_move || s.speed >= 1.0f) s.hand_off_t = now;
        else if (ref.ok && now - s.hand_off_t > std::chrono::seconds(6)) {
            spdlog::info("[Motion] gunstill: weapon {}'s standing pose dropped (the game's left hand stayed {:.0f} cm from its place for 6 s): {}", sig, s.hand_off_cm, ref.first_ok ? "back to its first learned pose" : "learned again");
            GunStillState::drop(ref);
            s.hand_off_t = now;
        }

        // Learning this weapon's standing pose: on foot, still (and 1.5 s after moving or sprinting: the game ends its sprint pose
        // after the player stops), nothing going on, the pose not changing.
        const bool quiet_now = !vehicle && s.speed < 0.4f && now - s.last_moving > std::chrono::milliseconds(1500) && !s.reload && !manual_reload && !throwing && out_s > 1.0f &&
                               now - s.last_fire > std::chrono::milliseconds(800) &&
                               now - s.last_ads > std::chrono::milliseconds(800) && now - s.last_throw > std::chrono::milliseconds(1500) &&
                               now - clock::time_point{ clock::duration{ m_pad_x_press.load() } } > std::chrono::milliseconds(2500);
        const Pose prev   = to_pose(s.q_prev);
        const bool steady = s.have_prev && glm::length(qp.t - prev.t) < 0.015f && angle_deg(qp.r, prev.r) < 2.0f;
        // Only a gun held in front, both of the game's hands drawn: at load the game shows the weapon still in its spawn
        // animation, 55-60 cm below the camera and beside it (author 17 Oct 15:43: learned there, the gun then flipped between
        // that pose and the game's every 6 s). Held guns sit 0.15-0.45 m ahead and 0.1-0.3 m below (rifle, SMG).
        // And not before the weapon has been out 3 s (the draw or spawn animation over; the simulator learned the rifle 13 and
        // 45 cm off its standing pose 1 s after loading).
        const bool in_hands = hand_fresh && qp.t.z < -0.10f && qp.t.y > -0.45f && out_s > 3.0f;
        // gunposecheck (author 3 Nov 08:17): a learned pose well off
        // the gun's own axes (its barrel over 20 deg from the aim's axis, or rolled over 30 deg) while the game, standing still, holds the gun
        // along them (within 12 deg, rolled under 20) for 1 s: that pose was taken in another stance, and is learned again from this one,
        // its first pose (fc5vr_gunposes.txt) and the left hand's place included. The MP40 40362 was saved 36 deg off, rolled -48 deg, where
        // the game holds it 4-5 deg off; "big" below kept it for good, as a learned pose follows corrections up to 15 deg only. Not a mesh
        // whose +Y the game does not hold forward (gunaxes' vote: a launcher, the 1888 32472), nor anything but a gun by the game's data
        // (data_gun: never a bow). The game's left hand need not be drawn (the simulator's walk never drew it: "game's left hand not drawn"),
        // the gun only in front, out 3 s: the pose is dropped, and learned again once the left hand is seen standing (until then gunaxes holds it).
        {
            const auto& axv   = s.axes[sig];
            const bool  front = qp.t.z < -0.10f && qp.t.y > -0.45f && out_s > 3.0f;
            const bool  check = m_gun_pose_check.load() && ref.ok && !ref.temp && !melee_weapon(sig) && !m_mo.bow && data_gun(sig) && quiet_now && steady && front &&
                               !m_mo.slung && axv.side <= axv.fwd;
            float       rr = 0.0f, rq = 0.0f;
            const float off_r = check ? axes_off_deg(ref.pose.r, rr) : 0.0f;
            const float off_q = check ? axes_off_deg(qp.r, rq) : 0.0f;
            const bool  wrong = check && pose_off_axes(ref.pose.r) && off_q < 12.0f && std::abs(rq) < 20.0f;
            if (!wrong) s.offaxes_on = false;
            else if (!s.offaxes_on) {
                s.offaxes_on = true;
                s.offaxes_t  = now;
            } else if (now - s.offaxes_t > std::chrono::seconds(1)) {
                s.offaxes_on = false;
                spdlog::info("[Motion] gunposecheck: weapon {}'s learned pose held it {:.0f} deg off its barrel's line, rolled {:.0f} deg; the game holds it {:.0f} deg off, "
                             "rolled {:.0f} deg, standing: dropped, learned again from this stance (saved as its first pose)", sig, off_r, rr, off_q, rq);
                s.axes_save = true;   // the bad line leaves fc5vr_gunposes.txt now, whether or not the new pose is learned this session
                ref = GunStillState::Ref{};
            }
        }
        // A pose clearly not the held one (4 cm / 6 deg) counts only once it has lasted 1 s: right after a sprint the game still
        // held the gun turned 76-85 deg for a moment, which was taken for the standing pose (author 17 Oct 21:18, 21:22).
        const bool away = ref.ok && (glm::length(qp.t - ref.pose.t) > 0.04f || angle_deg(qp.r, ref.pose.r) > 6.0f);
        // And not while the game's left hand is off its place: the game lowering the gun for a moment is not where it is held.
        // Nor with the weapon on the sling (the game then shows it relaxed, author 20 Oct 14:09: the AR-C learned 75 deg off), and a
        // learned pose follows only corrections up to 10 cm / 15 deg: larger ones are another stance of the game's, not a better pose.
        const bool big = ref.settled && (glm::length(qp.t - ref.pose.t) > 0.10f || angle_deg(qp.r, ref.pose.r) > 15.0f);
        if (big && quiet_now && steady && in_hands && !hand_off && !m_mo.slung && !ref.big_said) {
            ref.big_said = true;
            spdlog::info("[Motion] gunstill: weapon {}: the game holds it {:.0f} cm / {:.0f} deg from its learned pose standing; not followed (another stance)", sig,
                         100.0f * glm::length(qp.t - ref.pose.t), angle_deg(qp.r, ref.pose.r));
        }
        if (!big) ref.big_said = false;
        const bool calm = quiet_now && steady && in_hands && !hand_off && !m_mo.slung && !big;
        // A weapon out 5 s and still not learned: why, once per weapon (author 27 Oct: 5694 never learned).
        if (!ref.ok && out_s > 5.0f && s.unlearned_said.insert(sig).second)
            spdlog::info("[Motion] gunstill: weapon {} not learned after 5 s: gun at ({:.2f}, {:.2f}, {:.2f}) in the camera's axes (in the hands needs z < -0.10, y > -0.45), "
                         "game's left hand {}, quiet {}, steady {}, sling {}, speed {:.1f} m/s", sig, qp.t.x, qp.t.y, qp.t.z, hand_fresh ? "drawn" : "not drawn", quiet_now, steady,
                         m_mo.slung, s.speed);
        // gunstillwhy (author 7 Nov 20:49: the M133 29259; in his log it was the one gun of
        // the session with no learned pose, out about 1.5 min and logged once only, during its shell loading): every 10 s while the weapon in
        // hand is still not learned, what stopped the learning on this update.
        if (!ref.ok && out_s > 5.0f && now - s.unlearned_t > std::chrono::seconds(10)) {
            s.unlearned_t = now;
            std::string why;
            auto add = [&why](bool bad, const std::string& t) { if (bad) why += (why.empty() ? "" : ", ") + t; };
            add(vehicle, "in a vehicle");
            add(s.speed >= 0.4f, fmt::format("moving {:.1f} m/s (needs under 0.4)", s.speed));
            add(now - s.last_moving <= std::chrono::milliseconds(1500), "moved or sprinted in the last 1.5 s");
            add(s.reload, "the game's reload shown");
            add(manual_reload, "a manual reload step under way");
            add(throwing || now - s.last_throw <= std::chrono::milliseconds(1500), "throwing (or in the last 1.5 s)");
            add(now - s.last_fire <= std::chrono::milliseconds(800), "fired in the last 0.8 s");
            add(now - s.last_ads <= std::chrono::milliseconds(800), "aiming down sights (LT) in the last 0.8 s");
            add(now - clock::time_point{ clock::duration{ m_pad_x_press.load() } } <= std::chrono::milliseconds(2500), "pad X in the last 2.5 s");
            add(!steady, "the game's gun moving");
            add(!hand_fresh, "the game's left hand not drawn");
            add(!(qp.t.z < -0.10f), fmt::format("the gun {:.0f} cm ahead of the camera (needs over 10)", -qp.t.z * 100.0f));
            add(!(qp.t.y > -0.45f), fmt::format("the gun {:.0f} cm below the camera (needs under 45)", -qp.t.y * 100.0f));
            add(hand_off, fmt::format("the game's left hand {:.0f} cm off its place", s.hand_off_cm));
            add(m_mo.slung, "on the sling");
            spdlog::info("[Motion] gunstillwhy: weapon {} still not learned after {:.0f} s ({}held by its axes): {}", sig, out_s, s.axes[sig].n >= 3 ? "" : "not ",
                         why.empty() ? "nothing stopped it on this update (it learns once that lasts)" : why);
        }
        if (!(calm && away)) s.away_on = false;
        else if (!s.away_on) {
            s.away_on   = true;
            s.away_t    = now;
            s.away_said = false;
        }
        const bool away_wait = calm && away && ref.settled && now - s.away_t < std::chrono::seconds(1);
        if (calm && !away_wait) {
            // ~1 s at 45 updates/s; from a first guess, or when the held pose has been clearly off for 1 s, ~0.15 s.
            const float k = (ref.settled && !away) ? 0.03f : (ref.ok ? 0.15f : 1.0f);
            if (away && ref.settled && !s.away_said) {
                s.away_said = true;
                spdlog::info("[Motion] gunstill: weapon {} held {:.0f} cm / {:.0f} deg from where the game holds it standing: moved there", sig,
                             100.0f * glm::length(qp.t - ref.pose.t), angle_deg(qp.r, ref.pose.r));
            }
            if (!ref.ok) ref.pose = qp;
            ref.pose.t = glm::mix(ref.pose.t, qp.t, k);
            ref.pose.r = glm::normalize(glm::slerp(ref.pose.r, qp.r, k));
            if (hand_fresh) {
                ref.hand    = ref.hand_ok ? glm::mix(ref.hand, hand_g, k) : hand_g;
                ref.hand_ok = true;
                // twohandline: the weapon hand -> left hand direction in the gun's axes.
                const glm::vec3 d = hand_g - glm::vec3{ glm::inverse(Q) * glm::vec4{ s.hand_w, 1.0f } };
                // Only a left hand ahead of the weapon hand (a handgun's support hand beside or behind it gives no barrel-like line:
                // weapon 18912 measured 138 deg off in the simulator).
                const bool ahead = glm::length(d) > 0.08f && (glm::mat3_cast(ref.pose.r) * glm::normalize(d)).z < -0.7f;
                (ahead ? ref.ahead : ref.beside)++;   // sidearm: a handgun's support hand is beside or behind its weapon hand
                if (ahead) {
                    const bool first = !ref.line_ok;
                    ref.line         = first ? glm::normalize(d) : glm::normalize(glm::mix(ref.line, glm::normalize(d), k));
                    ref.line_ok      = true;
                    if (first) {
                        const glm::vec3 g = glm::normalize(glm::mat3_cast(ref.pose.r) * ref.line);
                        spdlog::info("[Motion] gunstill: weapon {}: the game's hands line {:.1f} deg right, {:.1f} deg up of its barrel's line (two-handed hold lines it up with your hands)",
                                     sig, glm::degrees(std::atan2(g.x, -g.z)), glm::degrees(std::atan2(g.y, std::sqrt(g.x * g.x + g.z * g.z))));
                    }
                }
            }
            // Settled once it has reached the game's standing pose: from a first guess taken while moving it eases over a few
            // updates. Marked settled after the first step, it stopped 15 % of the way and moved the rest only after 1 s more
            // standing still (author 14:28: weapon 42000 learned at 14:28:09, moved 4 cm / 10 deg at 14:28:36).
            const bool reached = glm::length(qp.t - ref.pose.t) < 0.01f && angle_deg(qp.r, ref.pose.r) < 1.5f;
            if (!ref.settled && reached)
                spdlog::info("[Motion] gunstill: weapon {}'s standing pose learned (gun at ({:.2f}, {:.2f}, {:.2f}) in the camera's axes{})", sig, qp.t.x, qp.t.y,
                             qp.t.z, hand_fresh ? ", left hand on it" : ", no left hand seen");
            if (!ref.settled && reached && !ref.first_ok) {
                ref.first         = ref.pose;
                ref.first_hand    = ref.hand;
                ref.first_hand_ok = ref.hand_ok;
                ref.first_ok      = true;
                gun_still_save();   // the next session starts with it
            }
            ref.ok      = true;
            ref.settled = ref.settled || reached;
        } else if (!ref.ok && out_s > 1.2f && !s.reload && !manual_reload && !throwing && !vehicle && in_hands && !m_mo.slung && !fast_move && s.speed < 1.5f && steady &&
                   now - s.last_moving > std::chrono::milliseconds(500)) {
            // Never still yet: a first guess once the draw animation is over (taken again when standing still). Not while
            // sprinting, on the sling or with the gun moving (author 20 Oct 16:11 / 16:19: the AR-15's guess was taken while
            // sprinting with it on the sling and held it at a wrong angle for the rest of the session).
            ref.pose = qp;
            ref.ok   = true;
            spdlog::info("[Motion] gunstill: weapon {}'s pose taken while moving (learned again when you stand still)", sig);
        } else if (!ref.ok && weapon_switch_needs_pose(sig) && out_s > 0.25f && steady && !s.sprinting && !throwing && !vehicle && qp.t.z < 0.0f) {
            // shoulderdraw (FarCry5WeaponSwitch.cpp): a weapon a switch brings whose pose is not known (author 27 Oct 21:41-21:42: weapon 19308,
            // never held standing still, showed the end of the game's draw 3 times out of 9): taken once the game's draw has settled, so the
            // switch holds it from there (learned again when you stand still).
            // A weapon the game holds out of the learning's reach (a melee weapon, 55-60 cm below the camera: the author's 5694, a wooden one,
            // at (0.34, -0.59, -0.31) in the simulator 28 Oct, never learned, so 6 of 6 switches to it showed the draw): held so for this
            // switch only (temp), then the game's own animation as before (meleestill off).
            const bool low = melee_weapon(sig) || !(qp.t.z < -0.10f && qp.t.y > -0.45f);
            ref.pose = qp;
            ref.ok   = true;
            ref.temp = low;
            spdlog::info("[Motion] gunstill: weapon {}'s pose taken at the end of a switch's draw, {:.2f} s after it showed ({})", sig, out_s,
                         low ? "held low, a melee weapon: for this switch only" : "learned again when you stand still");
        }
        if (s.alpha > 0.0f) s.held_updates++;
        if (m_gun_still_log.load() > 0) {
            m_gun_still_log--;
            spdlog::info("[GunStill] frame {} weapon {} | speed {:.1f} m/s{} | gun {:.1f} cm / {:.1f} deg from its standing pose ({}) | left hand {:.1f} cm off ({}) | {} | held {:.2f} | drawn gun {:.1f} cm / {:.1f} deg from it ({})", frame, sig,
                         s.speed, vehicle ? ", vehicle" : "", s.dev_cm, s.dev_deg, ref.ok ? (ref.settled ? "learned" : "first guess") : "none", s.hand_off_cm,
                         hand_fresh ? (ref.hand_ok ? "seen" : "no place yet") : "not drawn", s.reload ? "reload" : (throwing ? "throw" : "steady"), s.alpha, s.last_drawn_cm, s.last_drawn_deg, s.last_drawn_frame == m_gun_obj_seen ? (s.applied_full ? "held" : "easing") : "not checked");
        }
    }

    // meleestill (FarCry5Melee.cpp, author 25 Oct 10:22): a melee weapon is never learned (the game holds it 55-60 cm
    // below the camera, out of the learning's reach), so the game's idle, run and swing animations all showed. Its held pose is
    // made instead: the weapon turned so its length (the model's +Y, the barrel's axis on guns) points along meleepitch / meleeyaw /
    // meleeroll from the aim's axis, about the game's hand on it, which stays at its grip point; held at all times but a throw.
    bool melee_hold = false;
    if (melee_still_now() && melee_weapon(sig) && !m_mo.slung) {
        auto&     mh = s.melee[sig];
        glm::vec3 c{ 0.0f };
        bool      c_ok = false;
        {
            std::lock_guard _{ m_weapon_cal_mtx };
            if (auto it = m_grip_hand.find(sig); it != m_grip_hand.end() && it->second.ok) { c = it->second.c; c_ok = true; }
        }
        const bool hand_now = m_frame.load() - m_wpn_hand_frame <= 3;
        if (!c_ok && hand_now) { c = m_wpn_hand_cam; c_ok = true; }
        // The game's hand on it (else its grip point, where the game holds that hand on average) in the weapon's axes.
        if (new_update && (hand_now || c_ok)) {
            const glm::vec3 h = glm::vec3{ glm::inverse(Q) * glm::vec4{ hand_now ? m_wpn_hand_cam : c, 1.0f } };
            if (glm::length(h) < 0.6f) {
                mh.h  = mh.ok ? glm::mix(mh.h, h, 0.05f) : h;
                mh.ok = true;
            }
        }
        if (mh.ok && c_ok) {
            const glm::quat R = glm::normalize(melee_collide_rot() * melee_hold_rot());
            ref.pose          = Pose{ R, c - R * mh.h };
            ref.ok            = true;
            melee_hold        = true;
            if (!mh.said) {
                mh.said             = true;
                const glm::vec3 len = glm::mat3_cast(qp.r) * glm::vec3{ 0.0f, 1.0f, 0.0f };
                spdlog::info("[Melee] meleestill: weapon {} held as a melee weapon (the game held its length toward ({:.2f}, {:.2f}, {:.2f}) in the camera's axes, its hand at "
                             "({:.2f}, {:.2f}, {:.2f}) in the weapon's; now meleepitch {:.0f}, meleeyaw {:.0f}, meleeroll {:.0f})",
                             sig, len.x, len.y, len.z, mh.h.x, mh.h.y, mh.h.z, m_mel_pitch.load(), m_mel_yaw.load(), m_mel_roll.load());
            }
        }
    }
    // A pose taken for one switch only (temp) is dropped once that switch's hold is over.
    if (ref.temp && !weapon_switch_needs_pose(sig) && !weapon_switch_holds(sig)) {
        ref = GunStillState::Ref{};
        spdlog::info("[Motion] gunstill: weapon {}'s pose for the switch dropped (the switch is over: the game's own animation again)", sig);
    }
    // gunaxes (author 29 Oct 13:48): the gun's held pose
    // made from its own axes instead of the game's: its barrel (the mesh's +Y) along the aim's axis (-Z), its top (+Z) up, turned
    // about the game's right hand on it, which is put at the gun's grip point (so that hand sits on the controller as on every gun).
    // Known from the first updates a gun is in hand, standing or not; the game's own pose is still what tells a reload.
    Pose hpose     = ref.pose;
    bool axes_hold = false;
    if (!melee_hold && m_gun_axes.load() && sig != 0 && !melee_weapon(sig) && !gun_axes_game(sig)) {
        auto& ax = s.axes[sig];
        if (new_update && !m_mo.slung && !s.reload && !manual_reload && !throwing && out_s > 0.3f && m_wpn_grip_frame == m_gun_obj_seen) {
            const glm::vec3 h = glm::vec3{ glm::inverse(Q) * glm::vec4{ m_wpn_grip_cam, 1.0f } };
            if (glm::length(h) < 0.35f) {
                ax.h = ax.n == 0 ? h : glm::mix(ax.h, h, 0.05f);   // ~0.5 s at 45 updates a second
                if (++ax.n == 90) s.axes_save = true;
                ax.n = std::min(ax.n, 1 << 20);
            }
            // Standing, out of the draw and a while after moving, shooting or aiming down sights: is the mesh's +Y held forward (a gun's
            // barrel)? A bow's or a launcher's mesh may run another way: then the game's own pose stays.
            if (out_s > 1.0f && !s.sprinting && s.speed < 1.5f && now - s.last_moving > std::chrono::milliseconds(1500) &&
                now - s.last_fire > std::chrono::milliseconds(800) && now - s.last_ads > std::chrono::milliseconds(800)) {
                const glm::vec3 b = glm::mat3_cast(qp.r) * glm::vec3{ 0.0f, 1.0f, 0.0f };
                (b.z < -0.64f ? ax.fwd : ax.side)++;
            }
        }
        if (ax.side > ax.fwd && !ax.side_said) {
            ax.side_said = true;
            const glm::vec3 b = glm::mat3_cast(qp.r) * glm::vec3{ 0.0f, 1.0f, 0.0f };
            spdlog::info("[Motion] gunaxes: weapon {}: the game holds its mesh's +Y {:.0f} deg from the aim's axis standing ({} of {} updates): not a barrel, the game's pose kept",
                         sig, glm::degrees(std::acos(std::clamp(-b.z, -1.0f, 1.0f))), ax.side, ax.side + ax.fwd);
        }
        // gunposecheck: before gunaxes has measured the game's right hand on the gun (it measures with the left hand off the gun only, so a
        // gun taken in two hands at once, or while the manual reload hides the game's, is not measured), a learned pose well off the gun's
        // axes is not used: the gun is held by its axes about its own origin, which is where the game attaches it to the right hand
        // (righthand_rifles / righthand_pistols: the measured hand sits within 2.5 cm of it on 18 of the 21 guns of the 2 Nov arcade log, the
        // MP40's 1 cm; 23475, 69646 and 80077 about 15 cm), until measured.
        // Only a gun by the game's data (data_gun: never a bow, nor before the data knows the weapon), not with the manual bow, and only when
        // its +Y is known to be its barrel: a sight line read along it (sightcal), or gunaxes' vote (the 1888 32472's +Y is 86 deg off it).
        bool prov = false;
        if (ax.n < 3 && ax.fwd >= ax.side && m_gun_pose_check.load() && ref.ok && !ref.temp && !m_mo.bow && pose_off_axes(ref.pose.r) && data_gun(sig)) {
            glm::vec3  d{ 0.0f }, u{ 0.0f };
            const bool along = sight_axes(sig, d, u) && glm::length(d) > 0.5f && glm::normalize(d).y > 0.996f;
            prov             = along || ax.fwd > ax.side;
        }
        if ((ax.n >= 3 || prov) && ax.fwd >= ax.side) {
            glm::vec3 c{ m_grip_hand_ref[0].load(), m_grip_hand_ref[1].load(), m_grip_hand_ref[2].load() };
            if (m_grip_by_hand.load()) {
                std::lock_guard _{ m_weapon_cal_mtx };
                if (auto it = m_grip_hand.find(sig); it != m_grip_hand.end() && it->second.ok) c = it->second.c;
            }
            // The gun's own sight line, once learned from the game's aim down sights, sets the angle (a canted sight: 25110 and 32574
            // are rolled 10-12 deg in it); else the mesh's axes.
            const glm::quat R0 = glm::angleAxis(glm::radians(-90.0f), glm::vec3{ 1.0f, 0.0f, 0.0f });   // +Y -> -Z, +Z -> +Y
            glm::quat       R  = R0;
            glm::vec3       sd{ 0.0f }, su{ 0.0f };
            bool            by_sight = false;
            if (sight_axes(sig, sd, su) && glm::length(sd) > 0.5f) {
                sd                 = glm::normalize(sd);
                const glm::vec3 up = su - sd * glm::dot(su, sd);
                if (sd.y > 0.996f && glm::length(up) > 0.5f && glm::normalize(up).z > 0.9f) {   // within 5 deg of +Y, 25 deg of +Z
                    const glm::vec3 uz = glm::normalize(up);
                    const glm::mat3 B{ glm::cross(sd, uz), sd, uz };   // the sight's axes as the mesh's X, Y, Z
                    R                  = glm::normalize(R0 * glm::quat_cast(glm::transpose(B)));
                    by_sight           = true;
                }
            }
            const glm::vec3 hh = prov && ax.n == 0 ? glm::vec3{ 0.0f } : ax.h;
            hpose = Pose{ R, c - R * hh };
            axes_hold         = true;
            if (prov && !ax.prov_said) {
                ax.prov_said = true;
                float       rl = 0.0f;
                const float ol = axes_off_deg(ref.pose.r, rl);
                spdlog::info("[Motion] gunposecheck: weapon {}'s learned pose is {:.0f} deg off its barrel's line, rolled {:.0f} deg: held by its own axes about its origin "
                             "until gunaxes has measured the game's right hand on it", sig, ol, rl);
            }
            if (!prov && !ax.said) {
                ax.said = true;
                auto off = [](const glm::quat& r, float& roll) {
                    const glm::vec3 b = glm::mat3_cast(r) * glm::vec3{ 0.0f, 1.0f, 0.0f }, u = glm::mat3_cast(r) * glm::vec3{ 0.0f, 0.0f, 1.0f };
                    roll              = glm::degrees(std::atan2(u.x, u.y));
                    return glm::degrees(std::acos(std::clamp(-b.z, -1.0f, 1.0f)));
                };
                float      rg = 0.0f, rl = 0.0f;
                const float og = off(qp.r, rg), ol = ref.ok ? off(ref.pose.r, rl) : 0.0f;
                spdlog::info("[Motion] gunaxes: weapon {} held by its own axes ({}), about the game's right hand at ({:.3f}, {:.3f}, {:.3f}) "
                             "in its axes; the game holds it {:.0f} deg off the aim's axis, rolled {:.0f} deg{}",
                             sig, by_sight ? "its sight line along the controller, the sight's top up" : "barrel along the controller, top up", ax.h.x, ax.h.y, ax.h.z, og, rg, ref.ok ? fmt::format("; its learned pose was {:.0f} deg off, rolled {:.0f} deg", ol, rl) : std::string{ "; no learned pose" });
            }
        }
    }
    // gunorigin (author 5 Nov 11:46): the point put on the
    // controller is the gun's grip point: the game's own right hand on it once gripbyhand has measured it, else the one-for-all grip of the
    // reference SMG (m_grip_hand_ref, 0.06 -0.19 -0.215 in the game camera's axes). gunaxes puts the gun's measured hand there. A gun neither
    // has measured (gripbyhand needs both gloves in one draw, gunaxes the left hand off the gun: a pistol held in two hands is never measured)
    // was drawn where the game holds it: a handgun at arm's length, 0.37-0.50 m ahead of the camera. The 6P9 26187 (the author's log 5 Nov 12:45,
    // learned pose at (0.05, -0.17, -0.49)) so sat 28 cm ahead of the hand: its magazine well at -0.235 m in the controller's axes (the M9's
    // +0.041, the D50's +0.049) and its slide at -0.197 (+0.083 on the M9), and turning the controller swung it round that point. Now such a
    // handgun is held with its origin at the grip point, at its learned angle: the origin is where the game attaches the gun to the right
    // hand (righthand_pistols; the measured hand sits within 2.5 cm of it on 18 of the 21 guns of the 2 Nov arcade log, the M9 24417's 0.5 cm).
    // Until gunaxes or gripbyhand has measured the hand; not a gun held along its axes already (gunposecheck), nor one whose origin is within
    // 8 cm of the grip point anyway.
    if (!melee_hold && !axes_hold && m_gun_origin_hold.load() && sig != 0 && ref.ok && !ref.temp && !m_mo.bow && !melee_weapon(sig) && data_handgun(sig)) {
        auto&     ax = s.axes[sig];
        bool      grip_ok = false;
        if (m_grip_by_hand.load()) {
            std::lock_guard _{ m_weapon_cal_mtx };
            const auto it = m_grip_hand.find(sig);
            grip_ok       = it != m_grip_hand.end() && it->second.ok;
        }
        const glm::vec3 c{ m_grip_hand_ref[0].load(), m_grip_hand_ref[1].load(), m_grip_hand_ref[2].load() };
        const float     away_cm = 100.0f * glm::length(ref.pose.t - c);
        if (!grip_ok && (!m_gun_axes.load() || ax.n < 3) && ax.fwd >= ax.side && away_cm > 8.0f) {
            hpose = Pose{ ref.pose.r, c };
            if (!ax.origin_said) {
                ax.origin_said = true;
                spdlog::info("[Motion] gunorigin: weapon {} held with its origin at your hand, at its learned angle: the game's right hand on it not measured yet "
                             "(gripbyhand none, gunaxes {} of 3 measures{}); where the game holds it, at ({:.2f}, {:.2f}, {:.2f}), it sat {:.0f} cm from the grip point",
                             sig, ax.n, m_two_hand.load() ? ", the gun in two hands now: not measured while so" : "", ref.pose.t.x, ref.pose.t.y, ref.pose.t.z, away_cm);
            }
        }
    }
    m_gun_axes_state = (melee_hold || !m_gun_axes.load() || sig == 0 || melee_weapon(sig)) ? 0
                     : gun_axes_game(sig)                                                 ? 4
                     : axes_hold                                                          ? 2
                     : s.axes[sig].side > s.axes[sig].fwd                                 ? 3
                                                                                          : 1;
    if (s.axes_save) {
        s.axes_save = false;
        gun_still_save();
    }
    s.held      = hpose;
    s.held_sig  = sig;
    s.held_axes = axes_hold;
    if (m_weapon_audit.exchange(false)) gun_still_audit(sig, axes_hold, melee_hold);
    const bool known = ref.ok || axes_hold;
    // Held unless a reload or a throw is under way, the pose is unknown, or the gun is far from it (another mesh).
    // shoulderdraw (FarCry5WeaponSwitch.cpp): a weapon just taken by a switch gesture is held at once and fully, wherever the game's
    // draw animation has it, and that animation is no reload.
    const bool switch_hold = !melee_hold && known && !throwing && weapon_switch_holds(sig);
    if (switch_hold) s.reload = false;
    const bool hold = melee_hold ? !throwing
                                 : switch_hold || (m_gun_still.load() && known && !s.reload && !throwing && glm::length(qp.t - hpose.t) < 0.6f);
    s.alpha         = switch_hold ? 1.0f : std::clamp(s.alpha + (hold ? 1.0f : -1.0f) * da / std::max(0.02f, m_gst_ease.load()), 0.0f, 1.0f);
    const int  prev_applied = s.applied_frame;
    const bool prev_full    = s.applied_full;
    s.applied_frame = frame;
    s.applied_full  = false;
    if (s.alpha <= 0.0f) {
        s.fire_on   = false;
        s.fire_beta = 1.0f;
        return glm::mat4{ 1.0f };
    }
    // gunstillpair: in Stereo the right eye's pass (the same game update as the left eye's) takes the left eye's hold, so both
    // eyes show the same gun (the left one's is guessed from the last update; the right eye's alone was exact, and the gun
    // shook in one eye only).
    const bool shared_again = s.pair_frame == frame && prev_applied == frame && s.applied_C_frame == frame;
    if (m_gst_pair.load() && !afw_mode() && (frame & 1) && ((prev_applied == frame - 1 && s.applied_C_frame == frame - 1) || shared_again) && upd(frame) == upd(frame - 1)) {
        s.applied_full    = prev_full;
        s.applied_C_frame = frame;
        if (!shared_again) s.pair_shared++;
        s.pair_frame = frame;
        m_gs_frames_held++;
        return s.applied_C;
    }

    // This pass is the next game update when its camera is not the measured one: the gun guessed from the last change
    // (gunstillpredict; by default moved on by the whole change).
    glm::mat4 Qn = Q;
    if (upd(frame) != upd(m_gun_obj_seen)) m_gst_stale_frame = frame;   // gunstilllate: placed again at the gun mesh's draw
    if (!est && upd(frame) != upd(m_gun_obj_seen) && s.have_prev) {
        const glm::mat4 D     = Q * glm::inverse(s.q_prev);
        const Pose      dp    = to_pose(D);
        const bool      small_d = glm::length(dp.t) < 0.05f && angle_deg(dp.r, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }) < 10.0f;
        // timed: the change over the span it took, scaled to this update's span (the game animates by its frame time).
        const int step = afw_mode() ? 1 : 2;
        float     r    = 1.0f;
        if (s.upd_u[0] == upd(frame) && s.upd_u[1] == upd(m_gun_obj_seen) && s.upd_u[2] == s.q_prev_upd && s.upd_u[0] - s.upd_u[1] == step &&
            s.upd_u[1] - s.upd_u[2] == step) {
            const float dt0 = std::chrono::duration<float>(s.upd_t[0] - s.upd_t[1]).count(), dt1 = std::chrono::duration<float>(s.upd_t[1] - s.upd_t[2]).count();
            if (dt1 > 1e-3f && dt0 > 0.0f) {
                r = std::clamp(dt0 / dt1, 0.25f, 3.0f);
                s.pr_timed++;
                s.pr_r_min = std::min(s.pr_r_min, r);
                s.pr_r_max = std::max(s.pr_r_max, r);
            }
        }
        s.pred_q[0]  = Q;
        s.pred_q[1]  = small_d ? D * Q : Q;
        s.pred_q[2]  = small_d ? pose_pow(dp, r) * Q : Q;
        s.pred_q[3]  = small_d ? pose_pow(dp, 0.5f) * Q : Q;
        s.pred_frame = frame;
        Qn           = s.pred_q[std::clamp(m_gst_predict.load(), 0, 3)];
    }
    glm::mat4 Cf = to_mat(hpose) * glm::inverse(Qn);
    // gunstillfire hold (default) / kick (author 20 Oct; 23 Oct 22:17: the gun
    // still moved by itself when sprinting and shooting): the gun stays held while firing. The smoothed way below showed the
    // game's pose less its 0.12 s average from the trigger to up to 3 s after it: besides the kick, the fast part of everything the
    // game does with the gun then (into and out of its firing stance, out of the sprint pose when firing from a sprint, the aim's
    // swing from the head to the barrel). A pass held from a guess (Stereo's left eye without gunstilllate, every AFW frame) is
    // held from the last pose while firing, not moved on by the last change: a kick moved on overshoots (10-13 cm in AFW).
    const int  fire_mode = m_gst_fire.load();
    const bool fire_now  = now - s.last_fire < std::chrono::milliseconds(400);
    if (fire_mode != 0) {
        s.fire_on   = false;
        s.fire_beta = 1.0f;
        if (fire_now && upd(frame) != upd(m_gun_obj_seen)) {
            Qn = Q;
            Cf = to_mat(hpose) * glm::inverse(Qn);
        }
        // kick: the game's gun averaged over 0.12 s while firing and while the kick eases out, from the pose firing began with.
        const bool track = fire_now || s.kick_a > 0.0f;
        bool       fresh = false;
        if (!track) {
            s.q_lp       = Q;
            s.q_lp_frame = m_gun_obj_seen;
            s.q_lp_t     = now;
        } else if (s.q_lp_frame != m_gun_obj_seen) {
            const float k = std::clamp(std::chrono::duration<float>(now - s.q_lp_t).count() / 0.12f, 0.0f, 1.0f);
            const Pose  a = to_pose(s.q_lp), b = to_pose(Q);
            s.q_lp        = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, k)), glm::mix(a.t, b.t, k) });
            s.q_lp_frame  = m_gun_obj_seen;
            s.q_lp_t      = now;
            fresh         = true;
        }
        s.kick_a = fire_now ? 1.0f : std::max(0.0f, s.kick_a - da / 0.15f);
        // gunstillfire kick: the game's gun off that average (the fast part of its firing animation) in the game camera's axes, cut
        // down to a recoil kick on the held pose: back toward you, up, and the muzzle turned up, each clamped (gunstillkick back lift
        // deg). None while the game holds its gun far round from the standing pose (its sprint pose turns it 76-85 deg) or moves it a
        // lot at once (out of the sprint pose, into aiming down sights): those are no kick.
        auto kick_of = [&](const Pose& held, const glm::mat4& Qx, float a, bool log) {
            const Pose lp = to_pose(s.q_lp);
            const Pose dp = to_pose(glm::inverse(s.q_lp) * Qx);   // the gun's own axes
            const glm::vec3 t  = lp.r * dp.t;                      // the game camera's axes (x right, y up, z back)
            glm::quat       rc = glm::normalize(lp.r * dp.r * glm::inverse(lp.r));
            if (rc.w < 0.0f) rc = -rc;
            const float     ang = 2.0f * std::acos(std::clamp(rc.w, -1.0f, 1.0f));
            const glm::vec3 ax{ rc.x, rc.y, rc.z };
            const glm::vec3 rv  = (ang > 1e-6f && glm::length(ax) > 1e-9f) ? glm::normalize(ax) * ang : glm::vec3{ 0.0f };
            const float size_cm = 100.0f * glm::length(t), size_deg = glm::degrees(ang), stance_deg = angle_deg(lp.r, held.r);
            const float g = std::clamp((12.0f - size_cm) / 6.0f, 0.0f, 1.0f) * std::clamp((25.0f - size_deg) / 13.0f, 0.0f, 1.0f) *
                            std::clamp((30.0f - stance_deg) / 10.0f, 0.0f, 1.0f);
            if (log && g < 1.0f) s.fs_gated++;
            const float back = std::clamp(t.z, 0.0f, m_gst_kick_back.load()) * g * a;
            const float lift = std::clamp(t.y, 0.0f, m_gst_kick_lift.load()) * g * a;
            const float up   = std::clamp(rv.x, 0.0f, glm::radians(m_gst_kick_deg.load())) * g * a;
            Pose out = held;
            out.t += glm::vec3{ 0.0f, lift, back };
            out.r = glm::normalize(glm::angleAxis(up, glm::vec3{ 1.0f, 0.0f, 0.0f }) * held.r);
            if (log && m_gst_kick_log.load() > 0) {
                m_gst_kick_log--;
                spdlog::info("[GunKick] frame {} | the game's gun off its 0.12 s average: ({:+.1f}, {:+.1f}, {:+.1f}) cm, turn ({:+.1f}, {:+.1f}, {:+.1f}) deg (x right, y up, "
                             "z back) | its average {:.0f} deg from the standing pose | kept {:.2f} x {:.2f} | shown: back {:.1f} cm, up {:.1f} cm, muzzle up {:.1f} deg",
                             frame, 100.0f * t.x, 100.0f * t.y, 100.0f * t.z, glm::degrees(rv.x), glm::degrees(rv.y), glm::degrees(rv.z), stance_deg, g, a, 100.0f * back,
                             100.0f * lift, glm::degrees(up));
            }
            return out;
        };
        Pose target = hpose;
        if (fire_mode == 2 && s.kick_a > 0.0f) target = kick_of(hpose, Qn, s.kick_a, fresh);
        if (fire_mode == 2) Cf = to_mat(target) * glm::inverse(Qn);
    }
    // gunstillrecoil (author 16 Oct 21:29): while firing (and 0.4 s after) the correction is
    // frozen at its value when firing began, so the game's kick of the gun shows on top of the held pose; afterwards it eases
    // back to the full hold over 0.25 s.
    // While firing (gunstillrecoil off, author 20 Oct 09:11):
    // the correction follows a 0.12 s average of the game's pose, without extrapolation. The game writes the gun's pose only
    // right before drawing it, so the hold works from the previous frame's pose: the kick (2-7 cm per shot) then shook the
    // held gun by up to 10-13 cm / 21-25 deg from one frame to the next in AFW (simulator, rifle bursts). Around the average
    // the kick shows as the game animates it, small and smooth: 2-5.5 cm / 3-7 deg from frame to frame at most.
    const bool smooth = fire_mode == 0 && !m_gst_recoil.load();
    // Smoothed firing lasts while the game still holds the gun off its standing pose (2 cm / 3 deg): its shots and its way out of
    // the firing stance outlast the trigger (simulator: kicks 0.75 s after release, then an 8 cm / 15 deg one-frame exit).
    const Pose& stand = ref.ok ? ref.pose : hpose;   // where the game holds it standing (gunaxes before it is learned: the made pose)
    const bool stance = smooth && s.fire_on && (glm::length(qp.t - stand.t) > 0.02f || angle_deg(qp.r, stand.r) > 3.0f) && now - s.last_fire < std::chrono::seconds(3);
    const bool firing = fire_mode == 0 && (m_gst_recoil.load() || smooth) && (now - s.last_fire < std::chrono::milliseconds(400) || stance);
    if (smooth) {
        if (!s.fire_on) {
            s.q_lp       = Q;
            s.q_lp_frame = m_gun_obj_seen;
            s.q_lp_t     = now;
        } else if (s.q_lp_frame != m_gun_obj_seen) {
            const float k = std::clamp(std::chrono::duration<float>(now - s.q_lp_t).count() / 0.12f, 0.0f, 1.0f);
            const Pose  a = to_pose(s.q_lp), b = to_pose(Q);
            s.q_lp        = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, k)), glm::mix(a.t, b.t, k) });
            s.q_lp_frame  = m_gun_obj_seen;
            s.q_lp_t      = now;
        }
        if (firing) s.fire_C = to_mat(hpose) * glm::inverse(s.q_lp);
    } else if (firing && !s.fire_on) s.fire_C = Cf;
    if (fire_mode == 0) {
        s.fire_on   = firing;
        s.fire_beta = firing ? 0.0f : std::min(1.0f, s.fire_beta + da / 0.25f);
    }
    if (s.fire_beta < 1.0f) {
        const Pose a = to_pose(s.fire_C), b = to_pose(Cf);
        Cf           = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, s.fire_beta)), glm::mix(a.t, b.t, s.fire_beta) });
    }
    // gunstillstat's firing line: in the passes placed with their own gun pose (gunstilllate's, Stereo's right eye without
    // gunstillpair) while firing, what the game did with its gun from the standing pose and how far the drawn gun was from it.
    if ((fire_now || s.kick_a > 0.0f || s.fire_on) && m_gun_obj_seen == frame && s.fs_frame != frame) {
        s.fs_frame       = frame;
        s.fs_n++;
        const Pose  shown = to_pose(Cf * Q);
        const float gc = 100.0f * glm::length(qp.t - ref.pose.t), gd = angle_deg(qp.r, ref.pose.r);
        const float sc = 100.0f * glm::length(shown.t - ref.pose.t), sd = angle_deg(shown.r, ref.pose.r);
        s.fs_game_cm += gc;
        s.fs_game_max_cm   = std::max(s.fs_game_max_cm, gc);
        s.fs_game_max_deg  = std::max(s.fs_game_max_deg, gd);
        s.fs_shown_max_cm  = std::max(s.fs_shown_max_cm, sc);
        s.fs_shown_max_deg = std::max(s.fs_shown_max_deg, sd);
    }
    const glm::quat cq = glm::normalize(glm::quat_cast(glm::mat3{ Cf }));
    glm::mat4       C  = glm::mat4_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, cq, s.alpha));
    C[3]               = glm::vec4{ glm::vec3{ Cf[3] } * s.alpha, 1.0f };
    s.applied_C        = C;
    s.applied_C_frame  = frame;
    s.applied_full     = s.alpha >= 1.0f;
    m_gs_frames_held++;
    return C;
}

// The weapon's held (standing) pose in the game camera's axes, once learned: where gunstill draws the gun (sightaim).
bool FarCry5Stereo::gun_still_held(uint32_t sig, glm::mat4& pose)
{
    if (!m_gst || !m_gun_still.load()) return false;
    if (m_gst->held_axes && m_gst->held_sig == sig) {   // gunaxes: the made pose
        pose = to_mat(m_gst->held);
        return true;
    }
    const auto it = m_gst->refs.find(sig);
    if (it == m_gst->refs.end() || !it->second.ok) return false;
    pose = to_mat(it->second.pose);
    return true;
}

// partwatch (log only, default on; see FarCry5Stereo.h): each eye's weapon-pass draws for the gun in hand, frame by frame. Render thread only.
struct FarCry5Stereo::PartWatch
{
    using clock = std::chrono::steady_clock;
    struct Draw
    {
        uint32_t  count{ 0 };
        bool      dropped{ false }, pos_ok{ false }, skinned{ false };
        glm::vec3 pos{ 0.0f };
    };
    struct Known
    {
        int       streak{ 0 }, miss{ 0 }, rel_n{ 0 }, order{ -1 }, moved_n{ 0 };
        bool      moved{ false };
        glm::vec3 rel{ 0.0f };
    };
    struct Tally
    {
        uint32_t                     frames{ 0 }, missing{ 0 }, dropped{ 0 }, moved{ 0 }, guessed{ 0 }, episodes{ 0 };
        std::map<uint32_t, uint32_t> by_count;
    };
    int                                 fr{ -1000 };
    uint32_t                            sig{ 0 };
    std::vector<Draw>                   cur;
    bool                                guessed{ false };
    int                                 placed{ 0 };   // 0 not placed again, 1 at a part, 2 at the gun mesh
    std::unordered_map<uint32_t, Known> known[2];
    Tally                               tally[2];
    clock::time_point                   t_sum{ clock::now() }, t_line{};
    int                                 lines{ 0 };    // detailed lines in this 10 s period (at most 24)
};

void FarCry5Stereo::part_watch_draw(ID3D11DeviceContext* ctx, UINT count, bool dropped)
{
    // The gloves or bare hands (weapon_draw_impl's afwhandfresh flag) are not parts of the gun: they move on it all the time (final_l, 6 Nov:
    // 4110 and 14400 "moved" on every gun, 791 lines).
    if (m_fp_draw_glove) return;
    if (!m_pw) m_pw = new PartWatch{};
    auto&     w  = *m_pw;
    const int fr = m_frame.load();
    if (fr != w.fr) {
        if (w.fr >= 0) part_watch_frame(w.fr);
        w.fr = fr;
        w.cur.clear();
    }
    PartWatch::Draw d{};
    d.count   = count;
    d.dropped = dropped;
    ID3D11Buffer* cbs[4]{};
    ctx->VSGetConstantBuffers(0, 4, cbs);
    if (cbs[3]) {   // a skinned draw (a 12288-byte palette at VS b3): its object is the skeleton's, which the animation moves on the gun
        D3D11_BUFFER_DESC bd{};
        cbs[3]->GetDesc(&bd);
        d.skinned = bd.ByteWidth == 12288;
    }
    if (cbs[1]) {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = m_obj_consts.find(cbs[1]); it != m_obj_consts.end() && it->second.size >= 48) {
            d.pos    = glm::vec3{ it->second.f[3], it->second.f[7], it->second.f[11] };
            d.pos_ok = std::isfinite(d.pos.x) && std::isfinite(d.pos.y) && std::isfinite(d.pos.z);
        }
    }
    for (auto* c : cbs) if (c) c->Release();
    if (w.cur.size() < 64) w.cur.push_back(d);
    w.guessed = m_gst_stale_frame == fr;
    w.placed  = m_gst_est_done_frame == fr ? 1 : (m_gst_late_cam.frame == fr && m_gst_late_cam.redone) ? 2 : 0;
}

void FarCry5Stereo::part_watch_frame(int fr)
{
    auto&          w   = *m_pw;
    const int      e   = fr & 1;
    const uint32_t sig = m_weapon_sig.load();
    using clock        = PartWatch::clock;
    const auto now     = clock::now();
    if (sig != w.sig) {   // a new weapon (or none): start over
        w.sig = sig;
        w.known[0].clear();
        w.known[1].clear();
        return;
    }
    if (sig == 0 || w.cur.empty()) return;
    static const char* kEye[2]{ "left", "right" };
    static const char* kPlaced[3]{ "not placed again", "placed again at a part", "placed again at the gun mesh" };
    const uint32_t main_count = sig >= 0x10000u ? 65535u : sig;   // weaponidsplit's ids: the main piece is the 65535-index one
    glm::vec3      main_pos{ 0.0f };
    bool           main_ok = false;
    int            main_at = -1;
    for (size_t i = 0; i < w.cur.size(); ++i) {
        if (w.cur[i].count == main_count && !w.cur[i].dropped) {
            main_at = int(i);
            if (w.cur[i].pos_ok) { main_pos = w.cur[i].pos; main_ok = true; }
            break;
        }
    }
    auto& kn = w.known[e];
    auto& ta = w.tally[e];
    ta.frames++;
    if (w.guessed) ta.guessed++;
    const float gun_deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(m_mo.aim_world), glm::normalize(m_mo.head_world)), -1.0f, 1.0f)));
    auto order_line = [&] {
        std::string s;
        for (const auto& c : w.cur) s += fmt::format(" {}{}", c.count, c.dropped ? "(DROPPED)" : "");
        return s;
    };
    auto other = [&](uint32_t c) -> const char* {
        const auto& o = w.known[1 - e];
        auto        it = o.find(c);
        if (it == o.end()) return "never drawn there";
        return it->second.miss > 0 ? "missing there too" : "drawn there";
    };
    auto may_line = [&] {
        if (w.lines >= 24) return false;   // the 10 s summary counts the rest
        w.lines++;
        w.t_line = now;
        return true;
    };
    std::set<uint32_t> drawn, dropped;
    for (size_t i = 0; i < w.cur.size(); ++i) {
        const auto& d = w.cur[i];
        if (d.dropped) { dropped.insert(d.count); continue; }
        if (!drawn.insert(d.count).second) continue;   // a second draw of the same count: the first one is followed
        auto& k = kn[d.count];
        if (k.miss > 0) {
            if (k.miss >= 2 && may_line())
                spdlog::info("[PartWatch] frame {} {} eye: weapon {}: draw {} back after {} of this eye's frames (now {} of {} in the pass, the gun mesh {})", fr, kEye[e], sig, d.count,
                             k.miss, i + 1, w.cur.size(), main_at + 1);
            k.miss = 0;
        }
        k.streak = std::min(k.streak + 1, 100000);
        k.order  = int(i);
        if (main_ok && d.pos_ok && !d.skinned && d.count != main_count) {   // moved: rigid parts only (a scope, a barrel, a magazine)
            const glm::vec3 rel = d.pos - main_pos;
            const float     off = glm::length(rel - k.rel);
            if (k.rel_n >= 10 && off > 0.03f) {
                ta.moved++;
                ta.by_count[d.count]++;
                if (!k.moved && may_line())
                    spdlog::info("[PartWatch] frame {} {} eye: weapon {}: draw {} moved {:.1f} cm on the gun (from the gun mesh ({:.3f} {:.3f} {:.3f}), usually ({:.3f} {:.3f} {:.3f})); "
                                 "pass {}, {}; gun {:.0f} deg from where the head looks | pass:{}",
                                 fr, kEye[e], sig, d.count, 100.0f * off, rel.x, rel.y, rel.z, k.rel.x, k.rel.y, k.rel.z, w.guessed ? "guessed" : "its own pose", kPlaced[w.placed], gun_deg,
                                 order_line());
                k.moved = true;
                if (++k.moved_n > 60) { k.rel = rel; k.rel_n = 1; k.moved = false; k.moved_n = 0; }   // a lasting new place: learned again
            } else {
                k.moved   = false;
                k.moved_n = 0;
                k.rel   = k.rel_n == 0 ? rel : 0.9f * k.rel + 0.1f * rel;
                k.rel_n++;
            }
        }
    }
    for (auto it = kn.begin(); it != kn.end();) {
        auto& [c, k] = *it;
        if (drawn.contains(c)) { ++it; continue; }
        if (k.streak < 10) { k.streak = 0; k.miss = 0; ++it; continue; }   // not a steady part of this gun yet
        const bool drop = dropped.contains(c);
        k.miss++;
        if (drop) ta.dropped++; else ta.missing++;
        ta.by_count[c]++;
        if (k.miss == 1) {
            ta.episodes++;
            if (may_line())
                spdlog::info("[PartWatch] frame {} {} eye: weapon {}: draw {} {} (drawn the last {} frames of this eye, {} in the pass then; {}); pass {}, {}; gun {:.0f} deg from where the "
                             "head looks, trigger {} | pass:{}",
                             fr, kEye[e], sig, c, drop ? "DROPPED by the mod" : "missing", k.streak, k.order + 1, other(c), w.guessed ? "guessed" : "its own pose", kPlaced[w.placed], gun_deg,
                             m_trigger.load() ? "held" : "up", order_line());
        }
        if (k.miss > 300) {   // gone for good (a magazine out, an attachment changed): no longer followed
            it = kn.erase(it);
            continue;
        }
        ++it;
    }
    if (now - w.t_sum >= std::chrono::seconds(10)) {
        if (w.tally[0].missing + w.tally[0].dropped + w.tally[0].moved + w.tally[1].missing + w.tally[1].dropped + w.tally[1].moved > 0) {
            auto side = [&](int s) {
                const auto& t = w.tally[s];
                std::string by;
                for (const auto& [c, n] : t.by_count) by += fmt::format(" {}:{}", c, n);
                return fmt::format("{} eye {} frames ({} guessed): {} missing, {} dropped, {} moved in {} episodes{}{}", kEye[s], t.frames, t.guessed, t.missing, t.dropped, t.moved, t.episodes,
                                   by.empty() ? "" : " | by draw", by);
            };
            spdlog::info("[PartWatch] weapon {}, last 10 s: {}; {}", sig, side(0), side(1));
        }
        w.tally[0] = {};
        w.tally[1] = {};
        w.t_sum    = now;
        w.lines    = 0;
    }
}

// slingmv (FarCry5Stereo.h): at the gun mesh's draw, this pass's weapon camera final (gunstilllate placed it again just before): the
// slung gun's motion between this eye's two images, for DLSS's camera-motion pass (FarCry5Dlss.cpp): a point of the gun drawn at clip c
// now was drawn at S c in this eye's previous image. Exact for the gun and its rigid parts, whatever the game's own vectors hold.
void FarCry5Stereo::sling_mv_note(int fr)
{
    const int       e   = fr & 1;
    const auto&     c   = m_slmv_cam;
    const auto&     gw  = m_gun_world[fr & 3];
    auto&           h   = m_slmv_hist[e];
    if (h.frame == fr) return;   // this pass's gun mesh drawn again: noted at its first draw
    const uint32_t  sig = m_weapon_sig.load();
    bool            given = false;
    if (c.frame != fr || gw.frame != fr) {
        m_slmv_nocam++;   // no weapon camera of this pass placed by the mod, or the gun mesh's transform not read
    } else {
        glm::mat4 view{ c.view_rot };
        view[3]              = glm::vec4{ -(c.view_rot * c.cam), 1.0f };
        const glm::mat4 full = c.proj * view;   // the game pass's world -> this eye's clip, as the gun mesh is drawn
        // slingthrowhide: not while the game's gun is hidden (it is being put away or raised; the copy on the sling is what shows).
        // gunmv (author 6 Nov 21:53 video, OFXR Hybrid with DLSS vectors: the whole gun in hand flickered in OFXR's frames): the gun in
        // hand is placed by the mod at the controller too, so the same motion holds for it. Not the bow (its limbs leave the barrel line).
        const bool slung = m_sling_mv.load() && m_mo.slung;
        const bool held  = m_gun_mv.load() && !m_mo.slung && !m_mo.bow && m_hand_map.frame == c.frame && !m_hand_map.empty;
        if ((slung || held) && m_mo.valid && m_motion.load() && !afw_mode() && !holster_throw_cover()) {
            if (h.frame != fr - 2 || h.sig != sig) {
                m_slmv_nohist++;   // this eye's last image had no gun mesh of this weapon (just switched, or the first frame)
            } else {
                const glm::vec3 e0{ view * gw.Om[3] }, e1{ h.view * h.O[3] };   // the gun's origin in this eye's view, now and then
                if (glm::length(e0 - e1) > 0.25f || glm::length(e0) < 0.05f || glm::length(e1) < 0.05f) {
                    m_slmv_jump++;   // a teleport, a weapon swap under the same id: no motion
                } else {
                    const glm::mat4 S  = h.full * h.O * glm::inverse(gw.Om) * glm::inverse(full);
                    const glm::mat4 IP = glm::inverse(c.proj);
                    float           s_rows[16], ip_rows[16];
                    for (int r = 0; r < 4; ++r) {
                        for (int k = 0; k < 4; ++k) {
                            s_rows[r * 4 + k]  = S[k][r];
                            ip_rows[r * 4 + k] = IP[k][r];
                        }
                    }
                    // The hands (the right one at its own controller while the weapon hangs, the left at its controller) keep their own
                    // vectors: 15 cm about each, in this eye's view. gunmv: the hand holding the gun moves with it (not kept out); the
                    // free hand is (the other hand, or the weapon hand while the other hand alone holds the gun: twohandkeep).
                    float hands[6]{}, hand_r[2]{};
                    if (slung || m_mo.kept) {
                        const glm::vec3 hr = c.eye_rot * (m_mo.hand_pos_world - c.eye_cam);
                        hands[0] = hr.x; hands[1] = hr.y; hands[2] = hr.z;
                        hand_r[0] = 0.15f;
                    }
                    if (m_mo.other_valid && !(held && m_mo.kept)) {
                        const glm::vec3 hl = c.eye_rot * (m_mo.other_pos_world - c.eye_cam);
                        hands[3] = hl.x; hands[4] = hl.y; hands[5] = hl.z;
                        hand_r[1] = 0.15f;
                    }
                    // slingmv2: the gun as a capsule along its barrel line, from 60 cm behind the gun mesh's origin (as drawn) to 1 m ahead of
                    // it (stock to muzzle of the longest guns), in this eye's view: the barrel is the gun's -Z as placed on the sling (the
                    // hand map of this pass, the steadying folded in).
                    const glm::mat3 Wg  = m_hand_map.frame == c.frame ? m_hand_map.w_rot : m_mo.ctrl_rot_world;
                    const glm::vec3 fwd = glm::normalize(c.eye_rot * (Wg * glm::vec3{ 0.0f, 0.0f, -1.0f }));
                    const glm::vec3 pa  = e0 - 0.6f * fwd, pb = e0 + 1.0f * fwd;
                    const float     a[3]{ pa.x, pa.y, pa.z }, b[3]{ pb.x, pb.y, pb.z };
                    fc5::dlss::set_sling_motion(s_rows, ip_rows, a, b, std::clamp(m_sling_mv_radius.load(), 0.05f, 0.6f), hands, hand_r);
                    given = true;
                    (slung ? m_slmv_set : m_gunmv_set)++;
                    const float deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(e0), glm::normalize(e1)), -1.0f, 1.0f)));
                    m_slmv_sum_deg += deg;
                    m_slmv_max_deg = std::max(m_slmv_max_deg, deg);
                }
            }
        }
        h = { fr, sig, full, view, gw.Om };
    }
    if (!given) fc5::dlss::clear_sling_motion();
    const auto now = clock::now();
    if (now - m_slmv_t >= std::chrono::seconds(10)) {
        if (m_slmv_set + m_gunmv_set + m_slmv_nohist + m_slmv_jump > 0) {
            uint64_t sten = 0, shape = 0;
            fc5::dlss::sling_motion_stats(sten, shape);
            // The weapon pass's depth row (reversed Z: z = near / depth): the capsule test reads every pixel's depth with it, so it should
            // be the world's (near 0.1) for the ground behind the gun to stay out of the capsule.
            const float near_w = std::abs(m_slmv_cam.proj[2][3]) > 0.5f ? m_slmv_cam.proj[3][2] : 0.0f;
            const uint64_t n = m_slmv_set + m_gunmv_set;
            spdlog::info("[SlingMV] slingmv {} gunmv {} radius {:.2f} m, last 10 s: {} eye images gave DLSS the slung gun's own motion, {} the gun in hand's "
                         "({} DLSS evaluations with the stencil view, {} by the gun's place alone), {} without this eye's last image of the gun, {} jumps "
                         "left out, {} without the pass's camera | the gun's origin moved {:.2f} deg mean, {:.2f} deg max in view between an eye's two "
                         "images | weapon pass depth row {:.4f} {:.4f} (near {:.3f})",
                         m_sling_mv.load() ? "on" : "off", m_gun_mv.load() ? "on" : "off", m_sling_mv_radius.load(), m_slmv_set, m_gunmv_set, sten, shape,
                         m_slmv_nohist, m_slmv_jump, m_slmv_nocam, n ? m_slmv_sum_deg / double(n) : 0.0, m_slmv_max_deg, m_slmv_cam.proj[2][2],
                         m_slmv_cam.proj[3][2], near_w);
        }
        m_slmv_set = m_gunmv_set = m_slmv_nohist = m_slmv_jump = m_slmv_nocam = 0;
        m_slmv_sum_deg = 0.0;
        m_slmv_max_deg = 0.0f;
        m_slmv_t       = now;
    }
}

bool FarCry5Stereo::gun_still_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "partwatch") {   // on (default) | off, not saved: [PartWatch] lines, log only
        m_part_watch = on;
        spdlog::info("[PartWatch] partwatch {}", on ? "on: each eye's weapon-pass draws followed for the gun in hand (log only)" : "off");
        return true;
    }
    if (cmd == "gunstill") { m_gun_still = on; persist = true; return true; }
    if (cmd == "gunstilllog") { m_gun_still_log = arg.empty() ? 90 : (int)fv; return true; }
    if (cmd == "gunstillhand") { m_gst_hand_cm = std::clamp(fv, 2.0f, 40.0f); return true; }   // cm the left hand leaves its place for a reload
    if (cmd == "gunstillease") { m_gst_ease = std::clamp(fv, 0.02f, 1.0f); return true; }      // s to the game's pose and back
    if (cmd == "gunstillrecoil") { m_gst_recoil = on; persist = true; return true; }   // with gunstillfire game only
    if (cmd == "gunstillfire") {   // hold (default) | kick | game: the gun while firing (game = gunstillrecoil's way, before 24 Oct)
        m_gst_fire = arg == "game" ? 0 : arg == "kick" ? 2 : 1;
        persist    = true;
        spdlog::info("[Motion] gunstillfire {}: {}", arg == "game" ? "game" : arg == "kick" ? "kick" : "hold",
                     arg == "game" ? "the game's firing animation as gunstillrecoil sets it (before 24 Oct)" : arg == "kick" ? "only a recoil kick shows on the held gun" : "the gun stays held while firing");
        return true;
    }
    if (cmd == "gunstillkick") {   // test: the kick's limits, back and up in m, the muzzle's turn in deg
        float b = fv, l = m_gst_kick_lift.load(), d = m_gst_kick_deg.load();
        rest >> l >> d;
        m_gst_kick_back = std::clamp(b, 0.0f, 0.15f);
        m_gst_kick_lift = std::clamp(l, 0.0f, 0.10f);
        m_gst_kick_deg  = std::clamp(d, 0.0f, 30.0f);
        return true;
    }
    if (cmd == "gunkicklog") { m_gst_kick_log = arg.empty() ? 120 : (int)fv; return true; }   // test: one [GunKick] line per update while firing (kick)
    if (cmd == "weaponsteady") { m_ws_steady = on; persist = true; return true; }   // gunwobble: the weapon smoothed harder at rest, opening faster with speed
    if (cmd == "steadylog") { m_steady_log = on; return true; }                     // gunwobble: [Steady] and gunstillstat every 10 s (not saved)
    if (cmd == "slingmv") {   // on (default) | off, saved: DLSS gets the slung gun's own motion (FarCry5Stereo.h)
        m_sling_mv = on;
        persist    = true;
        spdlog::info("[SlingMV] slingmv {}", on ? "on: the slung gun's pixels move with the gun for DLSS" : "off: the game's motion vectors for the slung gun");
        return true;
    }
    if (cmd == "gunmv") {   // on (default) | off, saved: DLSS (and OFXR with DLSS vectors) gets the gun in hand's own motion (FarCry5Stereo.h)
        m_gun_mv = on;
        persist  = true;
        spdlog::info("[SlingMV] gunmv {}", on ? "on: the gun in hand's pixels move with the gun for DLSS" : "off: the game's motion vectors for the gun in hand");
        return true;
    }
    if (cmd == "slingmvradius") {   // test, not saved: the capsule's radius about the slung gun's barrel line, m (default 0.22)
        m_sling_mv_radius = std::clamp(arg.empty() ? 0.22f : fv, 0.05f, 0.6f);
        spdlog::info("[SlingMV] slingmvradius {:.2f} m", m_sling_mv_radius.load());
        return true;
    }
    if (cmd == "gunstillpair") { m_gst_pair = on; persist = true; return true; }   // Stereo: the right eye takes the left eye's hold (both eyes the same gun)
    if (cmd == "gunstilllate") { m_gst_late = on; persist = true; return true; }   // the guessed pass's weapon camera placed again at the gun mesh's draw (both eyes exact)
    if (cmd == "gunstillpartcheck") { m_gst_part_check = on; persist = true; return true; }   // gunstilllate parts: no broken or resized part (muzzle flash) places the gun
    if (cmd == "gunstillpredict") {   // test: the next update's gun guessed by hold | linear | timed | half
        m_gst_predict = arg == "hold" ? 0 : arg == "timed" ? 2 : arg == "half" ? 3 : 1;
        return true;
    }   // the game's recoil kick kept whole while firing (off: smoothed)
    if (cmd == "twohandgame") { m_two_hand_game = on; persist = true; return true; }   // machine pistols by the game's data: never with two hands
    if (cmd == "twohandblock") {   // twohandblock <sig> <sig> ... | none: the weapons never held with two hands
        std::set<uint32_t> s;
        auto               add = [&](const std::string& w) { if (const unsigned long v = std::strtoul(w.c_str(), nullptr, 10); v != 0) s.insert(uint32_t(v)); };
        add(arg);
        for (std::string w; rest >> w;) add(w);
        {
            std::lock_guard _{ m_weapon_cal_mtx };
            m_two_hand_block = std::move(s);
        }
        persist = true;
        return true;
    }
    if (cmd == "gunaxes") {   // on (default) | off: each gun held by its own model axes (off: the pose learned from the game, before 29 Oct)
        m_gun_axes = on;
        persist    = true;
        spdlog::info("[Motion] gunaxes {}: {}", on ? "on" : "off",
                     on ? "each gun's barrel along the controller, its top up, about the game's right hand on it" : "each gun at the pose learned from the game standing still (before 29 Oct)");
        return true;
    }
    if (cmd == "gunposecheck") {   // on (default) | off: a learned pose well off the gun's own axes is learned again, and not held before gunaxes measures the gun
        m_gun_pose_check = on;
        persist          = true;
        spdlog::info("[Motion] gunposecheck {}: {}", on ? "on" : "off",
                     on ? "a learned pose over 20 deg off a gun's barrel (or rolled over 30) is learned again once the game holds the gun straight standing, "
                          "and the gun is held by its axes until gunaxes has measured it"
                        : "learned poses kept as they are (before 3 Nov)");
        return true;
    }
    if (cmd == "gunstillswitch") {   // on (default) | off: no reload from the X press that changed weapon, nor from a learned pose off a gun held by its axes
        m_gst_switch_hold = on;
        persist           = true;
        spdlog::info("[Motion] gunstillswitch {}: {} ({} presses not taken for the weapon they brought so far)", on ? "on" : "off",
                     on ? "the X press that changed weapon is no reload of the next one, and a gun held by its axes is not judged from a learned pose off them"
                        : "both start the game's animation as before 5 Nov",
                     m_gst ? m_gst->switch_skips : 0);
        return true;
    }
    if (cmd == "gunorigin") {   // on (default) | off: a handgun whose right hand is not measured yet is held with its origin at the grip point
        m_gun_origin_hold = on;
        persist           = true;
        spdlog::info("[Motion] gunorigin {}: {}", on ? "on" : "off",
                     on ? "a handgun whose right hand is not measured yet is held with its origin (the game's right-hand attach point) at your hand"
                        : "such a handgun is drawn where the game holds it, ahead of your hand (before 5 Nov)");
        return true;
    }
    if (cmd == "gunaxesgame") {   // gunaxesgame <sig> <sig> ... | none: guns kept on the game's pose with gunaxes on
        std::set<uint32_t> s;
        auto               add = [&](const std::string& w) { if (const unsigned long v = std::strtoul(w.c_str(), nullptr, 10); v != 0) s.insert(uint32_t(v)); };
        add(arg);
        for (std::string w; rest >> w;) add(w);
        {
            std::lock_guard _{ m_weapon_cal_mtx };
            m_gun_axes_game = std::move(s);
        }
        persist = true;
        return true;
    }
    if (cmd == "adsprobe") {   // off | optic (default) | all: which guns sightlearn and scopelearn read by pressing aim down sights
        m_ads_probe = arg == "off" ? 0 : arg == "all" ? 2 : 1;
        persist     = true;
        spdlog::info("[Sight] adsprobe {}: {}", arg == "off" ? "off" : arg == "all" ? "all" : "optic",
                     arg == "off" ? "the game's aim down sights is never pressed for you" : arg == "all" ? "every gun is read once near the eye (before 29 Oct)" : "only a gun drawing sight glass (a scope, a red dot) is read near the eye");
        return true;
    }
    if (cmd == "weaponidsplit") { m_weapon_id_split = on; persist = true; return true; }   // a gun split in 65535-index pieces told apart by its next largest piece
    if (cmd == "twohandline") { m_two_hand_line = on; persist = true; return true; }   // the two-handed hold lines the gun's hand line up with your hands
    if (cmd == "twohandlevel") {   // on (default) | off, saved: every long gun lines up one common hand line in two hands (Stereo.h)
        m_two_hand_level = on;
        persist          = true;
        spdlog::info("[Motion] twohandlevel {}", on ? fmt::format("on: every long gun in two hands lines up the common hand line, {:.1f} deg right, {:.1f} up of its barrel",
                                                                  m_two_hand_level_ref[0].load(), m_two_hand_level_ref[1].load())
                                                     : std::string{ "off: each gun lines up its own hand line (as before 7 Nov)" });
        return true;
    }
    if (cmd == "twohandlevelref") {   // twohandlevelref right up (deg off the barrel; saved): the common hand line of twohandlevel
        float up = m_two_hand_level_ref[1].load();
        rest >> up;
        if (!arg.empty()) {
            m_two_hand_level_ref[0] = std::clamp(fv, -20.0f, 20.0f);
            m_two_hand_level_ref[1] = std::clamp(up, -20.0f, 25.0f);
        }
        persist = true;
        spdlog::info("[Motion] twohandlevelref {:.2f} {:.2f}: the common hand line {:.2f} deg right, {:.2f} up of the barrel", m_two_hand_level_ref[0].load(),
                     m_two_hand_level_ref[1].load(), m_two_hand_level_ref[0].load(), m_two_hand_level_ref[1].load());
        return true;
    }
    if (cmd == "gunstocklearn") { gunstock_learn_start(); return true; }   // test / F11 button, not saved: the 8 s gunstock calibration window (Stereo.h)
    if (cmd == "onehandmatch") {   // on (default) | off [P Y], saved: every long gun in one hand along one fixed line, P up and Y right (deg) of the weapon controller
        m_ohm_on = on;
        if (!on) m_ohm_cur_ok = false;
        float p = 0.0f, y = 0.0f;
        if (rest >> p >> y; std::isfinite(p) && std::isfinite(y) && !rest.fail()) {
            m_ohm_p = std::clamp(p, -45.0f, 30.0f);
            m_ohm_y = std::clamp(y, -20.0f, 20.0f);
        }
        persist = true;
        spdlog::info("[Motion] Angle correction for two handed weapons (gunstock calibration), onehandmatch {}", on ? one_hand_match_text() : std::string{ "off: the one-hand hold as before 8 Nov" });
        return true;
    }
    if (cmd == "weaponaudit") { m_weapon_audit = true; return true; }   // test: one [Audit] line for the gun in hand (gun_still_audit)
    if (cmd == "gunposebuiltin") {   // on (default) | off, saved, read at start: guns not in fc5vr_gunposes.txt use the built-in poses (FarCry5GunPoseTable.h)
        m_gun_builtin = on;
        persist       = true;
        spdlog::info("[Motion] gunposebuiltin {} (from the next launch)", on ? "on: a gun not learnt yet starts from its built-in pose, grip point and right hand" : "off: each gun learnt in play only");
        return true;
    }
    if (cmd == "gripnearweapon") {   // on (default) | off: a press of the left grip near the weapon in hand is the weapon's, never the D-pad modifier
        m_grip_near_weapon = on;
        if (!on) m_other_near = false;
        persist = true;
        spdlog::info("[Motion] gripnearweapon {}: {}", on ? "on" : "off",
                     on ? fmt::format("a left grip press within {:.2f} m of the weapon in hand is not the D-pad modifier until released", m_grip_near_dist.load())
                        : std::string{ "a left grip press that does not grab the weapon is the D-pad modifier (before 5 Nov)" });
        return true;
    }
    if (cmd == "gripneardist") { m_grip_near_dist = std::clamp(fv, 0.05f, 0.50f); persist = true; return true; }   // m from the weapon for gripnearweapon
    if (cmd == "dpadear") {   // on (default) | off: the left grip is the modifier (D-pad, throw, map, flat screen) only when pressed at the left ear / eye
        m_dpad_ear = on;
        persist    = true;
        spdlog::info("[Pad] dpadear {}: {}", on ? "on" : "off",
                     on ? fmt::format("a left grip press is the modifier only with the left controller within {:.2f} m of the left ear / eye; elsewhere it "
                                      "grabs, holds, brakes or descends", m_dpad_ear_dist.load())
                        : std::string{ "any left grip press that is not the weapon's is the modifier (before 7 Nov)" });
        return true;
    }
    if (cmd == "dpadeardist") { m_dpad_ear_dist = std::clamp(fv, 0.05f, 0.40f); persist = true; return true; }   // m from the eye-to-ear line for dpadear
    if (cmd == "fireready") { m_fire_ready = on; persist = true; return true; }   // the game's aim kept on the barrel with the weapon in hand (motion_post_present)
    if (cmd == "snapready") { m_snap_ready = on; persist = true; return true; }   // snap turns synced with the culled camera with the weapon in hand too
    if (cmd == "vehpitchguard") { m_veh_pitch_guard = on; return true; }   // a car's pitch read against the head look is kept (vehicle follow)
    if (cmd == "settlepitch") { m_settle_pitch = on; return true; }   // the culled camera looks up / down with the head while getting in
    if (cmd == "wheelforce") { m_wheel_force = on; return true; }   // test: the aim loop as with the weapon wheel open
    if (cmd == "wheellook") { m_wheel_look = on; return true; }   // the culled camera on the head while the weapon wheel is open
    if (cmd == "bodystarthead") { m_body_start_head = on; return true; }   // the body frame starts from the head (else the controller)
    if (cmd == "aimbacklimit") { m_aim_back_limit = arg == "off" ? 0.0f : std::clamp(fv, 0.0f, 180.0f); return true; }   // the game's aim at most N deg round from the head (0/off: no limit)
    if (cmd == "splitevpitch") { m_split_ev_pitch = std::clamp(fv, 69.0f, 160.0f); return true; }   // firing offset pitch limit (handed deg)
    if (cmd == "splitevmax") { m_split_ev_max = std::clamp(fv, 0.0f, 360.0f); return true; }   // firing offset limit (handed deg; x0.75 applied)
    if (cmd == "gunstillshot") { g_hand_needs_shot = on; return true; }
    if (cmd == "gunstillpad") { g_pad_waits_hand = on; return true; }                  // X shows the game's animation once the hand leaves the gun                // the left hand off the gun is a reload only right after shooting
    if (cmd == "gunstillwrong") {
        // Test: the current weapon's held pose moved D m down and D/2 m back, and its left hand's place 12 cm (author 17 Oct
        // 15:43: the pose learned 36 cm low and 23 cm back, the hand's place with it), to show how the hold recovers.
        if (!m_gst) return true;
        auto it = m_gst->refs.find(m_gst->sig);
        if (it == m_gst->refs.end() || !it->second.ok) {
            spdlog::info("[Motion] gunstillwrong: no held pose for this weapon yet");
            return true;
        }
        const float d = arg.empty() ? 0.36f : fv;
        it->second.pose.t += glm::vec3{ 0.0f, -d, 0.5f * d };
        it->second.hand += glm::vec3{ 0.0f, 0.0f, 0.12f };
        spdlog::info("[Motion] gunstillwrong: weapon {}'s held pose moved {:.2f} m down and {:.2f} m back, its left hand's place 12 cm", m_gst->sig, d, 0.5f * d);
        return true;
    }
    if (cmd == "gunstillreset") { gun_still_forget(); return true; }               // learn every weapon's pose again (F11 Comfort button)
    if (cmd == "gunstillstat") {
        // Since the last gunstillstat: held passes, the drawn gun's and the game's gun's distance from the standing pose.
        if (!m_gst) m_gst = new GunStillState{};
        auto& s = *m_gst;
        const float n = float(std::max<uint64_t>(1, s.st_n));
        spdlog::info("[GunStill] stat: {} held passes | drawn gun from its standing pose: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg | the game's gun: mean {:.2f} cm, "
                     "max {:.2f} cm / {:.2f} deg | reloads {} | speed {:.1f} m/s{} | hand-offs not taken for a reload (no shot just before) {} | gun transform at the pass's "
                     "first draw: {} current, {} a frame old, {} other | drawn gun's change pass to pass: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg",
                     s.st_n, s.st_drawn_cm / n, s.st_drawn_max_cm, s.st_drawn_max_deg, s.st_game_cm / n, s.st_game_max_cm, s.st_game_max_deg, s.reloads, s.speed, s.sprinting ? " (sprinting)" : "", s.hand_skips,
                     s.first_current, s.first_stale, s.first_other, s.st_step_cm / float(std::max<uint64_t>(1, s.st_steps)), s.st_step_max_cm, s.st_step_max_deg);
        // Per eye (L = the game call's pass, R = the extra call's in Stereo): mean / max off the standing pose, the change from that
        // eye's own last pass, and in Stereo the two eyes of one update against each other; then each way of guessing the next
        // update's gun against the pose the game drew it with (the predicted passes: Stereo left eye, every AFW frame).
        auto u   = [](uint64_t v) { return float(std::max<uint64_t>(1, v)); };
        auto eye = [&](int e) {
            return fmt::format("{} passes, off {:.3f} cm mean, {:.2f} cm / {:.2f} deg max, change from its last pass {:.3f} cm mean, {:.2f} cm / {:.2f} deg max", s.ey_n[e], s.ey_cm[e] / u(s.ey_n[e]),
                               s.ey_max_cm[e], s.ey_max_deg[e], s.ey_step_cm[e] / u(s.ey_steps[e]), s.ey_step_max_cm[e], s.ey_step_max_deg[e]);
        };
        static const char* kWay[]{ "hold", "linear", "timed", "half" };
        std::string ways;
        for (int k = 0; k < 4; ++k)
            ways += fmt::format(" | {}{} {:.3f} cm / {:.3f} deg mean, {:.2f} cm / {:.2f} deg max", kWay[k], k == m_gst_predict.load() ? " (used)" : "", s.pr_cm[k] / u(s.pr_n),
                                s.pr_deg[k] / u(s.pr_n), s.pr_max_cm[k], s.pr_max_deg[k]);
        spdlog::info("[GunStill] per eye ({}, gunstillpair {}, {} right passes shared): L {} | R {} | L vs R of one update: {} pairs, {:.3f} cm mean, {:.2f} cm / {:.2f} deg max",
                     afw_mode() ? "AFW" : "Stereo", m_gst_pair.load() ? "on" : "off", s.pair_shared, eye(0), eye(1), s.pd_n, s.pd_cm / u(s.pd_n), s.pd_max_cm, s.pd_max_deg);
        spdlog::info("[GunStill] guesses of the next update's gun, {} predicted passes ({} timed, span ratio {:.2f}..{:.2f}){}", s.pr_n, s.pr_timed, s.pr_timed ? s.pr_r_min : 0.0f,
                     s.pr_r_max, ways);
        // gunstillfire: the firing passes placed with their own gun pose (from the trigger to 0.4 s after it, the kick's ease out,
        // gunstillfire game's stance time).
        spdlog::info("[GunStill] firing (gunstillfire {}): {} exact passes | the game's gun from the standing pose: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg | drawn gun "
                     "from it: max {:.2f} cm / {:.2f} deg | kick cut down (sprint pose, large moves) in {} updates",
                     m_gst_fire.load() == 0 ? "game" : m_gst_fire.load() == 2 ? "kick" : "hold", s.fs_n, s.fs_game_cm / float(std::max<uint64_t>(1, s.fs_n)), s.fs_game_max_cm,
                     s.fs_game_max_deg, s.fs_shown_max_cm, s.fs_shown_max_deg, s.fs_gated);
        s.fs_n = s.fs_gated = 0;
        s.fs_game_cm = s.fs_game_max_cm = s.fs_game_max_deg = s.fs_shown_max_cm = s.fs_shown_max_deg = 0.0f;
        // gunstilllate: the per-eye numbers above are measured with the camera each pass was first placed with (its draws before
        // the gun mesh); with gunstilllate on, the gun mesh and the draws after it use the camera placed again (the pass's own pose).
        const uint64_t lt = m_gst_late_n + m_gst_late_miss + m_gst_late_fail;
        spdlog::info("[GunStill] gunstilllate parts: {} passes placed at their first part (the gun's pose from a part's, learned the update before), {} "
                     "without a known part | that guess against the gun mesh's own pose: {:.3f} cm / {:.3f} deg mean, {:.2f} cm / {:.2f} deg max, {} over 0.5 mm (or the coordinates' float rounding) / 0.1 deg",
                     m_gst_est_n, m_gst_est_none, m_gst_est_cm / double(std::max<uint64_t>(1, m_gst_est_chk)), m_gst_est_deg / double(std::max<uint64_t>(1, m_gst_est_chk)),
                     m_gst_est_max_cm, m_gst_est_max_deg, m_gst_est_bad);
        spdlog::info("[GunStill] gunstilllate parts (test): part draws {} without kept object constants (last b1 {} B), {} with; {} looked up "
                     "in a guessed pass; {} gun meshes reached the learning, {} parts learned there | gunstillpartcheck {}: {} part draws or places broken "
                     "(not finite or collapsed: a muzzle flash), {} guesses refused (the part resized since it was learned, or the guess not finite)",
                     m_gst_dbg_noobj, m_gst_dbg_noobj_size, m_gst_dbg_seen, m_gst_dbg_tries, m_gst_dbg_mesh, m_gst_dbg_learn, m_gst_part_check.load() ? "on" : "off",
                     m_gst_part_unfit, m_gst_part_refused);
        m_gst_dbg_noobj = m_gst_dbg_seen = m_gst_dbg_tries = m_gst_dbg_mesh = m_gst_dbg_learn = 0;
        m_gst_part_unfit = m_gst_part_refused = 0;
        m_gst_est_n = m_gst_est_none = m_gst_est_chk = m_gst_est_bad = 0;
        m_gst_est_cm = m_gst_est_deg = 0.0;
        m_gst_est_max_cm = m_gst_est_max_deg = 0.0f;
        spdlog::info("[GunStill] gunstilllate {}: {} passes placed again at the gun mesh's draw, {} not (another camera buffer bound or a stereo recording), {} failed "
                     "maps | draws before the gun mesh: {:.1f} mean ({:.1f} gloves), {} max",
                     m_gst_late.load() ? "on" : "off", m_gst_late_n, m_gst_late_miss, m_gst_late_fail, double(m_gst_late_pre_sum) / double(std::max<uint64_t>(1, lt)),
                     double(m_gst_late_pre_gloves_sum) / double(std::max<uint64_t>(1, lt)), m_gst_late_pre_max);
        m_gst_late_n = m_gst_late_miss = m_gst_late_fail = m_gst_late_pre_sum = m_gst_late_pre_gloves_sum = m_gst_late_pre_max = 0;
        for (int e = 0; e < 2; ++e) {
            s.ey_n[e] = s.ey_steps[e] = 0;
            s.ey_cm[e] = s.ey_max_cm[e] = s.ey_max_deg[e] = s.ey_step_cm[e] = s.ey_step_max_cm[e] = s.ey_step_max_deg[e] = 0.0f;
        }
        s.pd_n = s.pair_shared = s.pr_n = s.pr_timed = 0;
        s.pd_cm = s.pd_max_cm = s.pd_max_deg = 0.0f;
        for (int k = 0; k < 4; ++k) s.pr_cm[k] = s.pr_max_cm[k] = s.pr_deg[k] = s.pr_max_deg[k] = 0.0f;
        s.pr_r_min = 1e9f;
        s.pr_r_max = 0.0f;
        s.st_steps = 0;
        s.st_step_cm = s.st_step_max_cm = s.st_step_max_deg = 0.0f;
        s.st_n = 0;
        s.st_drawn_cm = s.st_drawn_max_cm = s.st_drawn_max_deg = s.st_game_cm = s.st_game_max_cm = s.st_game_max_deg = 0.0f;
        return true;
    }
    return false;
}

// Every weapon's learned pose forgotten (cmd gunstillreset, F11 Comfort button): each is learned again once it is in hand,
// standing still.
void FarCry5Stereo::gun_still_forget()
{
    if (!m_gst) return;
    m_gst->refs.clear();
    gun_still_save();   // the file emptied too
    spdlog::info("[Motion] gunstill: every weapon's pose forgotten; each is learned again in hand, standing still");
}

// Each weapon's first learned pose, kept across sessions (bin\fc5vr_gunposes.txt, one line per weapon: signature, position,
// rotation quaternion x y z w, the left hand's place, whether it was seen). Author 20 Oct 16:19: a session that never stood
// still with a weapon only had a first guess for it.
void FarCry5Stereo::gun_still_load()
{
    if (!m_gst) return;
    std::ifstream f{ fc5::mod_file("fc5vr_gunposes.txt") };
    std::string   line;
    int           n = 0;
    int           grips = 0;
    while (std::getline(f, line)) {
        std::istringstream ls{ line };
        if (line.rfind("rhand ", 0) == 0) {   // gunaxes: the game's right hand in the gun's axes
            std::string tag;
            uint32_t    hsig = 0;
            glm::vec3   h{ 0.0f };
            if ((ls >> tag >> hsig >> h.x >> h.y >> h.z) && hsig != 0 && glm::length(h) < 0.35f) {
                auto& ax = m_gst->axes[hsig];
                ax.h     = h;
                ax.n     = 90;
            }
            continue;
        }
        if (line.rfind("grip ", 0) == 0) {
            std::string tag;
            uint32_t    gsig = 0;
            glm::vec3   c{ 0.0f };
            if ((ls >> tag >> gsig >> c.x >> c.y >> c.z) && gsig != 0 && glm::length(c) < 1.0f) {
                std::lock_guard _{ m_weapon_cal_mtx };
                auto& gh = m_grip_hand[gsig];
                gh.c     = c;
                gh.n     = 90;
                gh.ok    = true;
                ++grips;
            }
            continue;
        }
        uint32_t           sig = 0;
        Pose               p{};
        glm::vec3          h{ 0.0f };
        int                hok = 0;
        if (!(ls >> sig >> p.t.x >> p.t.y >> p.t.z >> p.r.x >> p.r.y >> p.r.z >> p.r.w >> h.x >> h.y >> h.z >> hok) || sig == 0) continue;
        if (glm::length(p.r) < 0.5f || !(p.t.z < -0.10f && p.t.y > -0.45f)) continue;   // the same checks as learning in hand
        auto& r         = m_gst->refs[sig];
        r.pose          = Pose{ glm::normalize(p.r), p.t };
        r.hand          = h;
        r.hand_ok       = hok != 0;
        r.first         = r.pose;
        r.first_hand    = h;
        r.first_hand_ok = r.hand_ok;
        r.ok = r.settled = r.first_ok = true;
        ++n;
    }
    if (n || grips) spdlog::info("[Motion] gunstill: {} weapons' poses and {} grip points read from fc5vr_gunposes.txt", n, grips);
    // gunposebuiltin (Stereo.h): what the file does not have, from the built-in table (measured for every weapon in the simulator).
    if (m_gun_builtin.load()) {
        int bp = 0, bg = 0, bh = 0;
        for (const auto& e : fc5::gunposes::kTable) {
            if (e.has_pose && !m_gst->refs.contains(e.sig)) {
                auto& r         = m_gst->refs[e.sig];
                r.pose          = Pose{ glm::normalize(glm::quat{ e.q[3], e.q[0], e.q[1], e.q[2] }), glm::vec3{ e.t[0], e.t[1], e.t[2] } };
                r.hand          = glm::vec3{ e.lh[0], e.lh[1], e.lh[2] };
                r.hand_ok       = e.lh_ok;
                r.first         = r.pose;
                r.first_hand    = r.hand;
                r.first_hand_ok = r.hand_ok;
                r.ok = r.settled = r.first_ok = true;
                r.builtin       = true;
                ++bp;
            }
            if (e.has_grip) {
                std::lock_guard _{ m_weapon_cal_mtx };
                if (auto& gh = m_grip_hand[e.sig]; !gh.ok) {
                    gh.c  = glm::vec3{ e.grip[0], e.grip[1], e.grip[2] };
                    gh.n  = 90;
                    gh.ok = true;
                    ++bg;
                }
            }
            if (e.has_rhand) {
                if (auto& ax = m_gst->axes[e.sig]; ax.n < 90) {
                    ax.h = glm::vec3{ e.rh[0], e.rh[1], e.rh[2] };
                    ax.n = 90;
                    ++bh;
                }
            }
        }
        if (bp || bg || bh) spdlog::info("[Motion] gunposebuiltin: {} poses, {} grip points and {} right hands from the built-in table (guns not in fc5vr_gunposes.txt)", bp, bg, bh);
    }
}

void FarCry5Stereo::gun_still_save()
{
    if (!m_gst) return;
    std::ofstream f{ fc5::mod_file("fc5vr_gunposes.txt"), std::ios::trunc };
    int           n = 0;
    for (const auto& [sig, r] : m_gst->refs) {
        if (!r.first_ok || r.builtin) continue;   // gunposebuiltin: a built-in pose stays the table's (a later build's table reaches it)
        f << sig << ' ' << r.first.t.x << ' ' << r.first.t.y << ' ' << r.first.t.z << ' ' << r.first.r.x << ' ' << r.first.r.y << ' ' << r.first.r.z << ' ' << r.first.r.w << ' '
          << r.first_hand.x << ' ' << r.first_hand.y << ' ' << r.first_hand.z << ' ' << (r.first_hand_ok ? 1 : 0) << '\n';
        ++n;
    }
    // gripbyhand's measured grip points ("grip" lines): measured only once a weapon has been out 120 frames plus 90 draws, so
    // each first draw of a session sat 19-25 cm off the hand for about 2 s, then jumped (author 21 Oct 12:49).
    int g = 0;
    {
        std::lock_guard _{ m_weapon_cal_mtx };
        for (const auto& [sig, gh] : m_grip_hand) {
            if (!gh.ok) continue;
            f << "grip " << sig << ' ' << gh.c.x << ' ' << gh.c.y << ' ' << gh.c.z << '\n';
            ++g;
        }
    }
    // gunaxes: each gun's right hand in its own axes, once measured over 90 updates.
    for (const auto& [sig, ax] : m_gst->axes)
        if (ax.n >= 90) f << "rhand " << sig << ' ' << ax.h.x << ' ' << ax.h.y << ' ' << ax.h.z << '\n';
    spdlog::info("[Motion] gunstill: {} weapons' poses and {} grip points saved to fc5vr_gunposes.txt", n, g);
}

// gunaxesgame: guns kept on the game's pose with gunaxes on (F11, the weapon in hand). Saved as "gunaxesgame <sig> <sig> ...".
bool FarCry5Stereo::gun_axes_game(uint32_t sig)
{
    std::lock_guard _{ m_weapon_cal_mtx };
    return m_gun_axes_game.contains(sig);
}

std::string FarCry5Stereo::gun_axes_game_line()
{
    std::lock_guard _{ m_weapon_cal_mtx };
    std::string     s = "gunaxesgame";
    if (m_gun_axes_game.empty()) s += " none";
    for (uint32_t v : m_gun_axes_game) s += " " + std::to_string(v);
    return s;
}

const char* FarCry5Stereo::gun_axes_status() const
{
    switch (m_gun_axes_state.load()) {
    case 1: return "measuring the game's hand on it (about a second in hand)";
    case 2: return "held by its own axes";
    case 3: return "the game's pose (its model does not point forward: a bow or a launcher?)";
    case 4: return "the game's pose (unticked below)";
    default: return "not a gun (or gunaxes off)";
    }
}

// twohandblock: weapons never held with two hands (author 21 Oct 17:52, the Uzi: its two-handed hold stayed wrong with
// twohandline). Saved as "twohandblock <sig> <sig> ..." in fc5vr_settings.txt.
bool FarCry5Stereo::two_hand_blocked(uint32_t sig)
{
    if (two_hand_game_blocked(sig)) return true;
    std::lock_guard _{ m_weapon_cal_mtx };
    return m_two_hand_block.contains(sig);
}

// twohandgame: a machine pistol by the game's weapon data (not known for the first half second a weapon is held: a hold begun then ends when it is).
bool FarCry5Stereo::two_hand_game_blocked(uint32_t sig) const
{
    if (!m_two_hand_game.load()) return false;
    const auto* w = fc5::gameweapon::info(sig);
    return w && w->one_handed && w->cls == 3;
}

std::string FarCry5Stereo::two_hand_block_line()
{
    std::lock_guard _{ m_weapon_cal_mtx };
    std::string     s = "twohandblock";
    if (m_two_hand_block.empty()) s += " none";
    for (uint32_t v : m_two_hand_block) s += " " + std::to_string(v);
    return s;
}
