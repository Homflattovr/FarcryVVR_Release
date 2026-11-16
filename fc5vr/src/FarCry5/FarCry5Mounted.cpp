// Mounted weapons (turret thread 5 Nov; simulator run testruns\2026-11-05_turret, research\2026-11-05_turret\README.md). Using a mounted
// mortar (pad X) equips weapon 29346, the game's item Weapons/Mounted/GROUND_Mounted_Mortar (class 10, two-handed, no holster). The mod took
// it for a gun in the hand: its first-person pass was put on the right controller (the tube followed the controller and looked carried), the
// game camera only turned within the mount's limits (24 deg of a 40 deg controller turn left, 0 right, 19 down, 0 up), the barrel ray found
// nothing (-1 m) and every shot waited for firewaitmax (250 ms) with 1.3-3.7 deg of aim error.
//  - mountedweapon on|off (default on, saved; F11 Weapon > Mounted weapons): while the weapon in hand is a mounted one (decided once per
//    weapon change in mounted_frame_end, logged "[Weapon] mounted weapon N: ..."), its first-person pass is drawn where the game draws it,
//    fixed to the game camera as the world is (FarCry5Stereo.cpp weapon_view: not put on the controller), so the gun or mortar stays on its
//    mount and turns with it, seen from the eyes. Also off for it: the body gestures (sling, hip and shoulder switches, throwables, manual
//    reload: gesture_poll's allowed), the two-handed hold and gripnearweapon (update_motion), the holster's copies and its main weapon /
//    sidearm learning (held_still, FarCry5Holster.cpp), sightaim and sightlearn's automatic aim down sights (FarCry5Sight.cpp), the scope
//    picture and scopelearn (FarCry5Scope.cpp), melee by hitting a body (FarCry5Melee.cpp). Hand-held weapons are unchanged. Off: as before.
//  - On foot on a mount (mounted_aim_now), the aim loop (motion_post_present): the game's aim follows the right controller's pointing
//    (its forward axis, grippitch included, from the controller itself: no gun model's barrel or sight line; mounted_motion), always, not
//    only while the trigger is held. At the mount's limit the camera stops where the game holds it: that is not counted as the game ignoring
//    the mouse (no 1 s pause, no switch to stick aim, no popupesc), and the mouse scale is not learnt from it. The trigger goes through at
//    once (no aim gate, no firewaitmax). firesplit still turns the drawn and culled camera onto the head through Extended View.
//  - mountedaim hand|head (default hand, saved): head = the game's aim follows the head on a mount instead (as with the trigger up
//    on foot); the trigger still fires at once.
//  - mountedinvert on|off (default on, saved; author 6 Nov 13:53): with mountedaim hand, the controller's pointing is mirrored, as the handles of a gun on a
//    pivot: tilting it up aims down, turning it right aims left. Pitch is mirrored about level (controller level = gun level); yaw about
//    the head's heading when the mount began (taken in the body frame, so snap turns keep it), so pointing the controller where you faced
//    when you got on aims there. A new mount (another weapon id, or 30 frames with no mounted aim) takes a new heading. Off: as before.
//  - mountedhands on|off (default on, saved; F11 Weapon > Mounted weapons; author 13 Nov 16:32). On foot on a mount the game draws its own hands on the handles, fixed to the gun;
//    the mod left them there and bodyik left the whole pass alone (no hand map: the pass is not on a controller). Now the mounted pass has
//    its own hand map, the identity (m_mt_hand_map, set at the weapon pass's camera upload), so weapon_draw_impl moves each of the game's
//    hands onto its own controller (as on the sling: the hand's centre on the controller, the palm at the empty hand's grip pose,
//    emptyhandgrip; the fingers close with the grip, fingergrip) and bodyik draws the arms and body to wherever the hands are drawn. A grip
//    pressed with that hand within mountedgripreach of its handle (the game's own hand there, read from the same draws) puts the hand on the
//    handle: it is left where the game draws it, holding it, until the grip is let go (a short buzz when it takes). On a mount the grips do
//    nothing else (ModConfig: no D-pad modifier, brake or LT). Off: as before (the game's hands on the handles, no arms).
//  - mountedtwohand on|off (default on, saved): with a hand on a handle the gun turns as one on a pivot does: the pivot sits mountedpivot
//    (default 45 cm) ahead of the hands on the line the gun pointed when they took the handles, and the aim is the line from the hands'
//    centre (both hands, or the one on a handle) to the pivot: hands to the right turn the gun left, hands up tilt it down, hands forward
//    or back change nothing. With no hand on a handle the gun stays where it was left. The hands are read from the game camera in the
//    body's axes (the controller's offset from it), so the camera moving with the gun does not feed back into the aim; a snap turn or a
//    change of hands takes the aim then as the new start. At the mount's turn limit (the game's aim held 2.5 deg or more off for 12
//    frames) the start is moved so the aim is where the game holds it: pulling back turns the gun at once. It replaces mountedaim and
//    mountedinvert while on; off: those as before. Log: "[Weapon] mountedtwohand:" a line a second while a hand holds a handle.
//  - mountedgain x (default 2.5, saved; author 13 Nov 20:44, the first headset try): the pivot's turn (yaw and pitch from the
//    start) multiplied. His 21:32-21:37 log at 1x: 48 cm of hand move for 57 deg of turn, 31 cm for 37 deg; at 2.5x about 19 cm and
//    12 cm. On the game's side the camera swings round the pivot with the gun (it moved 58-97 cm while the hands moved 24-43 cm), so
//    from the player the handles moved the other way (22-32 cm): the hands drawn on them stay on them, the controllers part from them
//    by the hands' move, which the gain shortens.
//  - mountedvehicle on|off (default on, saved; F11 Weapon > Mounted weapons; author 14 Nov 20:57).
//    His 21:53 log: the truck's back seat (seat 2, passenger) equips Weapons/Mounted/GROUND_Mounted_M2.50 (weapon 44589), seen as a
//    mounted weapon and kept on its mount, but mounted_aim_now left every vehicle to the vehicle code: no hands moved, no handles, the aim
//    on the head (his saved mountedaim head). Now in a vehicle seat that is not the driver's (mounted_vehicle_seat), with mountedhands on,
//    the mount is handled as on foot: hands, arms and body, grips on the handles, the two-hand aim with mountedgain, the trigger at once.
//    The two-hand aim is worked in the vehicle's frame there (the body frame's yaw, which the vehicle code turns with the vehicle), so a
//    hand still on the truck keeps the gun still on the truck while it turns, and a gun let go stays pointing the same way on the truck.
//    A step of the vehicle frame over 10 deg in one frame (the vehicle code correcting the front) keeps the world aim instead. No
//    Extended View split in a vehicle (as with a gun in a passenger seat): the game camera follows the gun, the eyes stay on the head and
//    cullhead culls from the headset. The per-second "[Weapon] mountedtwohand:" line adds how far the vehicle frame turned since the grip.
//    Off, or driving: as before (the aim loop left to the vehicle code).
//  - mountedring on|off (default on, saved), mountedringgain x (default 1.5, saved), mountedstick on|off (default on, saved); F11 Weapon >
//    Mounted weapons. Author 15 Nov 22:24 (his 21:37 ride on a truck's back M60, weapon 56664): "a turret in the back of a vehicle with 360
//    rotation ... was not working like a static LMG (hands etc...) and it was not rotating (have right stick left/right rotating it if
//    possible)". The log: the hands took the handles and the gun followed them (up to 78 deg), but the game turns that turret about its own
//    camera (the camera moved 1-8 cm while the gun turned 11-78 deg, the handles about 1 m ahead swinging 20-33 cm the way the gun turned),
//    so the pivot-ahead aim (hands right = muzzle left) swung the drawn handles against the hands: the hands on them flew off the real hands,
//    and the gun went the opposite way to them. With mountedring, in a vehicle seat the gun turns about the player: its yaw follows the hands'
//    bearing around the game camera and its pitch their height (hands left = gun left, hands up = gun up, as the handles go), times
//    mountedringgain, the limit re-bases as before. Turning the body all the way round turns the gun all the way round. Off: the pivot ahead
//    (mountedpivot, mountedgain) as on foot. mountedstick: on a vehicle's turret (mountedtwohand on) the right stick turns the view as on
//    foot (snap or smooth turn, its settings), not done in vehicles otherwise; the gun's aim is held in the body frame there, so it turns
//    with the view and the handles stay in front. Never synced through the mouse (snapsync): the turret's mouse scale is not the on-foot one.
//    The vehicle frame's step test (a front correction, above) is not made for 10 frames after a stick turn.
// A weapon is a mounted one when the game's data names it under Weapons/Mounted/ (or Weapons_DLCV/Mounted/) or gives it class 10 (the
// mortars); before the game's data is learnt for the weapon id (gameweapon: the same id and item for 45 frames), by the id list below or
// the item the game says the player holds now (FarCry5Lua equipped_item, asked every 150 ms), then again each frame until the data says.
// Vehicles: the first-person pass stays off the controller for a mounted weapon there too; the aim loop is left to the vehicle code unless
// mountedvehicle takes the seat (above).
//
// spawnfacing (F11 Debug > Spawn command and Vehicle spawn): the spawns go where the player faces now (the head's heading, the body frame's
// yaw included so snap turns count; the game camera's when no head is known), not the chest copy's axis, which kept the first heading
// after snap turns. Ground entities are turned to that heading (FarCry5Lua.cpp sp_place), so a mounted gun's handles face the player.
#include "FarCry5Stereo.h"
#include "FarCry5GameWeapon.h"
#include "FarCry5Lua.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // Weapon ids (the mod's: the weapon pass's largest mesh) seen for mounted weapons, for the half second before the game's data is learnt.
    constexpr uint32_t kMountedIds[]{
        29346,   // Weapons/Mounted/GROUND_Mounted_Mortar (turret_ground360_mortar, simulator 5 Nov)
    };

    bool mounted_info(const fc5::weapons::Info* w)
    {
        return w && ((w->name && std::strstr(w->name, "/Mounted/")) || w->cls == 10);
    }

    // mortaraim: the mortars (GROUND_Mounted_Mortar, _Cluster, DLCV_Turret_Mortar: class 10)
    bool mounted_mortar_info(const fc5::weapons::Info* w)
    {
        return w && (w->cls == 10 || (w->name && std::strstr(w->name, "Mortar")));
    }

    // A turn about the world's up axis (Z) by a: a direction's yaw (atan2(-x, y), positive turning left) grows by a.
    glm::vec3 rot_z(float a, const glm::vec3& v)
    {
        const float c = std::cos(a), s = std::sin(a);
        return glm::vec3{ c * v.x - s * v.y, s * v.x + c * v.y, v.z };
    }
}

// mortarview on|off (default on, saved; F11 Weapon > Mounted weapons). Author 15 Nov 22:40, after mortaraim. The game has a mortar camera of its own
// (CCameraMortarComponent, research6-10-30_engine_natives.md), reached with the pad's aim button (LT) as the plane's bomber view is.
// On a mount the grips were the handles' and nothing else, and on foot no button sends LT (aim down sights is the mod's own there), so his
// 20:30 mortar never left the first-person camera (the same camera vtable +0x44f3680 all along): no aiming view, the shells flew where the
// tube happened to point. Now on a mortar on foot:
//  - either grip held sends LT (ModConfig), so the game shows its mortar view; the grips no longer take the mortar's handles (they were
//    35-70 cm out of reach; the aim is mortaraim's head / controller aim, or the mortar view's below).
//  - while LT is sent (and until the game camera is back within 0.5 m of where it was, or 0.5 s after LT goes up), as bomberview: the eyes
//    look around the game camera's own heading and pitch from its place (FarCry5Stereo.cpp, the eye base), head look and firesplit off, the
//    aim loop sends no mouse (it would chase a camera that is not the aim), the right stick goes to the game as its own right stick (the
//    mortar view's target; no snap turn, no stick jump / crouch), and the right trigger fires at once.
// Log: "[Weapon] mortarview:" at the start and end (how long, how far and how pitched the game camera went). Off: as before (no LT on a mount).
void FarCry5Stereo::mortar_view_update()
{
    using namespace std::chrono_literals;
    const auto      now = std::chrono::steady_clock::now();
    const bool      can = mortar_view_grip();
    const glm::vec3 pos{ m_game_cam_pos[0], m_game_cam_pos[1], m_game_cam_pos[2] };
    const glm::vec3 gf  = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
    const float     gp  = glm::degrees(std::asin(std::clamp(gf.z / std::max(1e-6f, glm::length(gf)), -1.0f, 1.0f)));
    bool on = m_mortar_view_on.load();
    if (!can) {
        on = false;
    } else if (m_lt_sent.load()) {
        on         = true;
        m_mv_up_t  = {};
    } else if (on) {
        if (m_mv_up_t == std::chrono::steady_clock::time_point{}) m_mv_up_t = now;
        if (glm::length(pos - m_mv_cam0) < 0.5f || now - m_mv_up_t > 500ms) on = false;
    }
    if (on && m_mortar_view_on.load()) {
        m_mv_frames++;
        m_mv_far      = std::max(m_mv_far, glm::length(pos - m_mv_cam0));
        m_mv_pitch[0] = std::min(m_mv_pitch[0], gp);
        m_mv_pitch[1] = std::max(m_mv_pitch[1], gp);
    }
    if (on == m_mortar_view_on.load()) return;
    if (on) {
        m_mv_cam0     = pos;
        m_mv_t0       = now;
        m_mv_frames   = 0;
        m_mv_far      = 0.0f;
        m_mv_pitch[0] = m_mv_pitch[1] = gp;
    }
    m_mortar_view_on = on;
    request_jump_reset();   // the view cuts to or from the mortar camera: each eye's DLSS history starts over
    if (on)
        spdlog::info("[Weapon] mortarview: a grip held on the mortar, LT sent (the game's mortar view) at frame {}: the eyes follow the game camera's heading and "
                     "pitch from its place (now {:.0f} deg), no mouse aim, the right stick is the game's",
                     m_frame.load(), gp);
    else
        spdlog::info("[Weapon] mortarview: over after {:.1f} s ({} updates; {}): the game camera went at most {:.1f} m from where it was, pitch {:.0f} to {:.0f} deg",
                     std::chrono::duration<float>(now - m_mv_t0).count(), m_mv_frames,
                     !can ? "off the mortar, a menu or mortarview off" : glm::length(pos - m_mv_cam0) < 0.5f ? "LT let go, the camera back" : "LT let go 0.5 s ago",
                     m_mv_far, m_mv_pitch[0], m_mv_pitch[1]);
}

void FarCry5Stereo::mounted_frame_end()
{
    mortar_view_update();   // mortarview: each frame, before the decision below (which returns early once made)
    const uint32_t sig = m_weapon_sig.load();   // the weapon in hand (kept through a frame that draws none)
    if (sig == m_mounted_sig && m_mounted_final) return;
    const bool new_sig = sig != m_mounted_sig;
    m_mounted_sig      = sig;
    bool        held   = false;
    bool        mortar = false;   // mortaraim: a mortar (class 10)
    bool        final  = true;
    std::string why;
    if (sig == 0) {
        why = "no weapon";
    } else if (const fc5::weapons::Info* w = fc5::gameweapon::info(sig)) {
        held   = mounted_info(w);
        mortar = mounted_mortar_info(w);
        why  = fmt::format("the game's data: {} (class {})", w->name ? w->name : "?", w->cls);
    } else if (std::find(std::begin(kMountedIds), std::end(kMountedIds), sig) != std::end(kMountedIds)) {
        held   = true;
        mortar = true;   // the list holds only the mortar (29346)
        why  = "the mod's list of mounted weapon ids";
    } else {
        final         = false;   // asked again each frame until the game's data is learnt
        uint64_t item = 0;
        if (fc5::lua::equipped_item(item) && item != 0) {
            const fc5::weapons::Info* wi = fc5::weapons::find(item);
            held   = mounted_info(wi);
            mortar = mounted_mortar_info(wi);
            why  = fmt::format("the item the game says is held now: {:x} ({})", item, wi && wi->name ? wi->name : "not in the weapon table");
        } else {
            why = "not known yet";
        }
    }
    m_mounted_mortar = held && mortar;
    const bool was = m_mounted_held.exchange(held);
    m_mounted_final = final;
    {
        std::lock_guard _{ m_mounted_mtx };
        m_mounted_why = why;
    }
    if (held && (!was || new_sig))
        spdlog::info("[Weapon] mounted weapon {}: {}; {}", sig, why,
                     !m_mounted_on.load()                            ? std::string{ "mountedweapon off: held like a gun" }
                     : m_in_vehicle.load() && !mounted_vehicle_seat() ? std::string{ "kept on its mount; in a vehicle the aim is the vehicle code's (mountedvehicle off, or driving)" }
                                                                      : fmt::format("kept on its mount{}, the aim follows the {} (mountedweapon)",
                                                                                    m_in_vehicle.load() ? " in a vehicle seat (mountedvehicle)" : "",
                                                                                    (m_mt_hands_on.load() && m_mt_twohand_on.load() && m_mounted_mortar.load() && m_mortar_aim_on.load())
                                                                                        ? (m_mounted_aim.load() == 1 ? "head while no hand holds a handle, else the hands on its handles (mortaraim)"
                                                                                                                     : "right controller's pointing while no hand holds a handle, else the hands on its handles (mortaraim)")
                                                                                    : (m_mt_hands_on.load() && m_mt_twohand_on.load()) ? "hands on its handles (mountedtwohand)"
                                                                                    : m_mounted_aim.load() == 1                        ? "head"
                                                                                                                                       : "right controller's pointing"));
    else if (!held && was)
        spdlog::info("[Weapon] mounted weapon no longer held: weapon {} ({})", sig, why);
}

// update_motion, at its end (the head known): on a mount the aim is the right controller's pointing (mirrored with mountedinvert), from
// the game camera (no muzzle offset, no sight line, no convergence: the direction itself), or the head's (mountedaim head). The barrel ray
// starts at the controller; the red dot is drawn along the aim.
//
// mortaraim on|off (default on, saved; F11 Weapon > Mounted weapons). Author 15 Nov 22:24. His
// 20:30:45-20:31:13 log (build 138B7300) on Weapons/Mounted/GROUND_Mounted_Mortar (weapon 29346): mountedtwohand (on since 13 Nov, made for
// the LMG) left the mortar's aim to the hands on its handles, but the game's hands on the mortar were 35-70 cm from the controllers
// ("right grip pressed 37 cm from its handle: too far", 35 cm, left 70 cm; mountedgripreach 25), so no hand ever held one and the mortar
// "stays where it was left": it never turned, his head was ignored (his saved mountedaim head), and his snap turns (body yaw -608 to -728 deg)
// did not turn it either; the eyes turned away from it (a shot's barrel 21 screen widths off the view). Now on a mortar, while no hand holds
// a handle, the aim is mountedaim's again (the head for him; else the right controller's pointing, mountedinvert), as on 5 Nov: look (or
// point) and fire, snap turns included. A hand put on a handle still turns it with the hands (mountedtwohand), from where the game holds it
// then. The machine guns are unchanged. Off: as since 13 Nov. Log: "[Weapon] mortaraim:" when the aim changes hands.
void FarCry5Stereo::mounted_motion(const glm::vec3& cam)
{
    if (mounted_hands_now()) mounted_hands_frame(cam);   // mountedhands: which hand holds its handle (and mountedtwohand's aim, below)
    glm::vec3 dir{ 0.0f };
    {
        // mortaraim: on a mortar with no hand on a handle the aim below is mountedaim's; the two-hand aim starts again from the game's aim
        // when a hand takes a handle.
        const bool look = mounted_twohand_now() && !mounted_twohand_aim();
        if (look) m_mt_hold_ok = false;
        if (look != m_mortar_look_was) {
            m_mortar_look_was = look;
            if (look)
                spdlog::info("[Weapon] mortaraim: on the mortar with no hand on a handle: the aim follows the {}", m_mounted_aim.load() == 1 ? "head (mountedaim head)" : "right controller's pointing (mountedaim hand)");
            else if (mounted_twohand_now())
                spdlog::info("[Weapon] mortaraim: a hand on the mortar's handle: it turns with the hands (mountedtwohand)");
        }
    }
    if (mounted_twohand_aim()) {
        // mountedtwohand: the aim mounted_hands_frame worked out (the hands about the pivot, or held), shot from the game camera (the gun's
        // own sight line on a mount, not the controller's). mountedvehicle: m_mt_hold is in the vehicle's frame, turned back to the world here.
        dir                 = glm::normalize(rot_z(m_mt_by, m_mt_hold));
        m_mo.barrel_origin  = cam;
        m_mo.aim_origin     = cam;
        m_mo.barrel_dir     = dir;
        m_mo.aim_world      = dir;
        m_mo.sight_aim      = false;
        return;
    }
    if (m_mounted_aim.load() == 1) {
        dir = glm::normalize(m_mo.head_world);
    } else {
        dir = glm::normalize(m_mo.ctrl_rot_world * glm::vec3{ 0.0f, 0.0f, -1.0f });
        if (m_mounted_invert.load()) dir = mounted_inverted(dir);
    }
    m_mo.barrel_origin  = m_mo.ctrl_pos_world;
    m_mo.aim_origin     = cam;
    m_mo.barrel_dir     = dir;
    m_mo.aim_world      = dir;
    m_mo.sight_aim      = false;
}

// mountedinvert: the controller's pointing (world, Z up) mirrored: pitch about level, yaw about the heading kept for this mount.
glm::vec3 FarCry5Stereo::mounted_inverted(const glm::vec3& c)
{
    const int frame = m_frame.load();
    if (m_mounted_ref_sig != m_mounted_sig || frame - m_mounted_ref_frame > 30 || frame < m_mounted_ref_frame) {
        m_mounted_ref_yaw = m_head_rel_yaw;   // the head's yaw in the body frame (update_motion, just before)
        m_mounted_ref_sig = m_mounted_sig;
        spdlog::info("[Weapon] mountedinvert: controller pointing mirrored on this mount (up aims down, right aims left) about the heading {:.1f} deg (head, body frame {:.1f} deg)",
                     glm::degrees(std::remainder(m_body_yaw + m_mounted_ref_yaw, glm::two_pi<float>())), glm::degrees(m_mounted_ref_yaw));
    }
    m_mounted_ref_frame = frame;
    const float ref = m_body_yaw + m_mounted_ref_yaw;
    const float cy  = std::atan2(-c.x, c.y);   // as yaw_of: 0 = +Y, positive turning left
    const float cp  = std::asin(std::clamp(c.z, -1.0f, 1.0f));
    const float y   = ref - std::remainder(cy - ref, glm::two_pi<float>());
    const float p   = -cp;
    return glm::vec3{ -std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p) };
}

namespace
{
    glm::vec3 dir_of(float yaw, float pitch) { return glm::vec3{ -std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch) }; }
    float     yaw_dir(const glm::vec3& f) { return std::atan2(-f.x, f.y); }   // 0 = +Y, positive turning left (as yaw_of)
    float     pitch_dir(const glm::vec3& f) { return std::asin(std::clamp(f.z / std::max(1e-6f, glm::length(f)), -1.0f, 1.0f)); }
}

// mountedhands: the game's hands on the handles (weapon_draw_impl's gloves draws, before any move; hand_c in the pass's camera axes).
void FarCry5Stereo::mounted_note_handles(const glm::vec3* hand_c, const float* hand_w, const HandMap& hm)
{
    for (int s = 0; s < 2; ++s) {
        if (!(hand_w[s] > 0.0f)) continue;
        const glm::vec3 w = hm.cam + glm::transpose(hm.rot) * hand_c[s];
        if (!std::isfinite(w.x + w.y + w.z)) continue;
        m_mt_handle[s]       = w;
        m_mt_handle_frame[s] = m_frame.load();
    }
}

// mountedhands: a free hand's turn (game camera axes) that puts its palm (palm, the game's) at the empty hand's grip pose on its controller
// (emptyhandgrip, the left mirrored): the mounted map has no weapon calibration, so drawn on the controller the palm is turn x palm. Once
// per frame and side, so every mesh of that hand (a glove piece and the bare skin under it) turns the same.
glm::quat FarCry5Stereo::mounted_free_turn(int side, const glm::quat& palm)
{
    const glm::quat id{ 1.0f, 0.0f, 0.0f, 0.0f };
    if (side < 0 || side > 1) return id;
    const int fr = m_frame.load();
    if (m_mt_turn_frame[side] == fr) return m_mt_turn[side];
    glm::quat t = glm::normalize(empty_hand_target(side) * glm::inverse(palm));
    if (t.w < 0.0f) t = -t;
    if (!std::isfinite(t.w + t.x + t.y + t.z)) t = id;
    m_mt_turn[side]       = t;
    m_mt_turn_frame[side] = fr;
    return t;
}

float FarCry5Stereo::mounted_buzz(int phys_side)
{
    if (phys_side < 0 || phys_side > 1) return 0.0f;
    return float(m_mt_buzz[phys_side].exchange(0)) / 100.0f;
}

// mountedhands and mountedtwohand, once per update_motion on foot on a mount (the controllers known).
void FarCry5Stereo::mounted_hands_frame(const glm::vec3& cam)
{
    const int  frame = m_frame.load();
    const int  ws    = left_handed() ? 1 : 0;   // the weapon hand's glove side; glove side 0 is the right hand, as the physical side
    const bool grip[2]{ m_dh_grip[0].load(), m_dh_grip[1].load() };   // the raw grips by physical side (drive_poll)
    auto       ctrl_ok  = [&](int s) { return s == ws ? m_mo.ctrl_valid : m_mo.other_valid; };
    auto       ctrl_pos = [&](int s) { return s == ws ? m_mo.ctrl_pos_world : m_mo.other_pos_world; };
    const char* name[2]{ "right", "left" };
    const float reach = m_mt_reach.load();
    const bool veh = m_in_vehicle.load();   // mountedvehicle: a vehicle seat (mounted_aim_now let it through)
    if (m_mt_sig != m_mounted_sig || frame - m_mt_frame > 30 || frame < m_mt_frame || veh != m_mt_veh) {
        // A new mount: no hand on a handle yet, and a grip already held is not a press.
        m_mt_sig      = m_mounted_sig;
        m_mt_veh      = veh;
        m_mt_ref_mask = 0;
        m_mt_hold_ok  = false;
        for (int s = 0; s < 2; ++s) {
            m_mt_snap[s]     = false;
            m_mt_grip_was[s] = grip[s];
        }
        spdlog::info("[Weapon] mountedhands: on mount {}{}: each hand on its own controller, a grip {} takes its handle; the gun {}", m_mt_sig,
                     veh ? " in a vehicle seat (mountedvehicle: as on foot, the aim held in the vehicle's frame)" : "",
                     reach > 0.0f ? fmt::format("within {:.0f} cm of it", reach * 100.0f) : std::string{ "anywhere" },
                     m_mt_twohand_on.load() ? fmt::format("turns with the hands on the handles (mountedtwohand, pivot {:.0f} cm ahead)", m_mt_pivot.load() * 100.0f)
                                            : std::string{ "aims as mountedaim says (mountedtwohand off)" });
    }
    m_mt_frame = frame;
    for (int s = 0; s < 2; ++s) {
        const bool g = grip[s];
        if (g && !m_mt_grip_was[s]) {
            const bool  known = frame - m_mt_handle_frame[s] <= 4 && frame >= m_mt_handle_frame[s];
            const float d     = known ? glm::length(ctrl_pos(s) - m_mt_handle[s]) : -1.0f;
            const bool  view  = mortar_view_grip();   // mortarview: on a mortar the grips are its aiming view (LT), not its handles
            const bool  take  = !view && ctrl_ok(s) && known && (reach <= 0.0f || d < reach);
            m_mt_snap[s]      = take;
            if (take) m_mt_buzz[s] = 60;
            spdlog::info("[Weapon] mountedhands: {} grip pressed {}: {}", name[s], known ? fmt::format("{:.0f} cm from its handle", d * 100.0f) : std::string{ "(the handle's place not read yet)" },
                         view ? "the mortar's grip: its aiming view (mortarview)" : take ? "the hand holds the handle" : !ctrl_ok(s) ? "no controller" : "too far, the hand stays on its controller");
        } else if (!g && m_mt_snap[s]) {
            m_mt_snap[s] = false;
            spdlog::info("[Weapon] mountedhands: {} hand let go of its handle", name[s]);
        }
        m_mt_grip_was[s] = g;
    }
    if (!m_mt_twohand_on.load()) return;

    // mountedvehicle: in a vehicle the aim is worked in the vehicle's frame, the body frame's yaw (from the same body axes the controllers
    // were placed with, so the hands and the frame agree this frame); on foot in the world (snap turns re-start below, as before).
    float by = 0.0f;
    if (veh) {
        const glm::vec3 bf = glm::transpose(m_mo.body_rot) * glm::vec3{ 0.0f, 0.0f, -1.0f };   // the body's forward (body view: -z ahead)
        if (std::hypot(bf.x, bf.y) > 0.1f) by = yaw_dir(bf);
        else by = m_mt_by;
    }
    if (m_mt_hold_ok && veh) {
        const float step = std::remainder(by - m_mt_by_last, glm::two_pi<float>());
        if (std::abs(step) > glm::radians(10.0f) && (frame - m_mt_stick_frame > 10 || frame < m_mt_stick_frame)) {
            // A step of the frame in one frame is the vehicle code correcting the front, not the vehicle turning: the world aim kept, a new start.
            // (Not after the right stick's turn, mountedstick: that one turns the gun with the view.)
            m_mt_hold     = rot_z(-step, m_mt_hold);
            m_mt_ref_mask = -1;
            m_mt_by_jumps++;
        }
    }
    m_mt_by_last = by;
    m_mt_by      = by;
    const bool ring = veh && m_mt_ring_on.load();   // mountedring: a vehicle's turret turns about the player

    // mountedtwohand. The game's aim now: its camera less the Extended View offset the camera carries (in the frame above).
    const glm::vec3 fc = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
    float           ya = std::remainder(yaw_dir(fc) - by, glm::two_pi<float>()), pa = pitch_dir(fc);
    if (m_mo.ev_ok) {
        ya -= m_mo.ev_oy;
        pa -= m_mo.ev_op;
    }
    int       mask = 0, n = 0;
    glm::vec3 mid{ 0.0f };
    for (int s = 0; s < 2; ++s) {
        if (!m_mt_snap[s] || !ctrl_ok(s)) continue;
        mask |= 1 << s;
        mid += rot_z(-by, ctrl_pos(s) - cam);   // the controller from the game camera: the body's axes only, not where the camera is
        ++n;
    }
    if (!m_mt_hold_ok) {
        m_mt_hold    = dir_of(ya, pa);
        m_mt_hold_ok = true;
    }
    if (n == 0) {   // no hand on a handle: the gun stays where it was left
        if (m_mt_ref_mask > 0) spdlog::info("[Weapon] mountedtwohand: no hand on a handle, the gun stays at yaw {:.1f} pitch {:.1f} deg",
                                             glm::degrees(yaw_dir(m_mt_hold)), glm::degrees(pitch_dir(m_mt_hold)));
        m_mt_ref_mask = 0;
        return;
    }
    mid /= float(n);
    if (mask != m_mt_ref_mask || (!veh && std::abs(std::remainder(m_body_yaw - m_mt_ref_body, glm::two_pi<float>())) > 1e-4f)) {
        // The start: the aim given last (or where the game holds it, if that is more than 3 deg away: a limit), the hands' centre now.
        float y0 = yaw_dir(m_mt_hold), p0 = pitch_dir(m_mt_hold);
        if (std::abs(std::remainder(y0 - ya, glm::two_pi<float>())) > glm::radians(3.0f) || std::abs(p0 - pa) > glm::radians(3.0f)) {
            y0 = ya;
            p0 = pa;
        }
        m_mt_yaw0     = y0;
        m_mt_pitch0   = p0;
        m_mt_m0       = mid;
        m_mt_ref_body = m_body_yaw;
        m_mt_by0      = by;
        m_mt_ring_acc  = 0.0f;
        m_mt_ring_last = yaw_dir(mid);
        spdlog::info("[Weapon] mountedtwohand: {} on the handle{}: the gun turns {}, from yaw {:.1f} pitch {:.1f} deg (the game's aim {:.1f} / {:.1f})",
                     mask == 3 ? "both hands" : (mask & 1) ? "the right hand" : "the left hand", mask == 3 ? "s" : "",
                     ring ? fmt::format("about you with the hands' bearing (mountedring, x{:.2f}; the hands {:.0f} cm out)", m_mt_ring_gain.load(), std::hypot(mid.x, mid.y) * 100.0f)
                          : fmt::format("about a pivot {:.0f} cm ahead (x{:.2f})", m_mt_pivot.load() * 100.0f, m_mt_gain.load()),
                     glm::degrees(y0), glm::degrees(p0), glm::degrees(ya), glm::degrees(pa));
        m_mt_ref_mask = mask;
        m_mt_off[0] = m_mt_off[1] = 0.0f;
        m_mt_stuck[0] = m_mt_stuck[1] = 0;
        m_mt_act_last[0] = ya;
        m_mt_act_last[1] = pa;
        // diagnostics: the handles' centre from the game camera, and the camera, now
        glm::vec3 hc{ 0.0f };
        int       hn = 0;
        for (int s = 0; s < 2; ++s) if (frame - m_mt_handle_frame[s] <= 4) { hc += rot_z(-by, m_mt_handle[s] - cam); ++hn; }
        m_mt_handles0 = hn ? hc / float(hn) : glm::vec3{ 0.0f };
        m_mt_cam0     = cam;
        m_mt_log_t    = std::chrono::steady_clock::now();
    }
    float yaw = 0.0f, pitch = 0.0f;
    if (ring) {
        // mountedring: the gun turns about the player (the game camera): the hands' bearing and height around it from the start, times
        // mountedringgain, plus the limit re-bases.
        const float hd = std::hypot(mid.x, mid.y), hd0 = std::hypot(m_mt_m0.x, m_mt_m0.y);
        if (hd < 0.08f || hd0 < 0.08f) return;   // the hands over the head: no bearing, the aim kept
        const float gain = std::clamp(m_mt_ring_gain.load(), 0.25f, 8.0f);
        // unwrapped frame to frame, so a turn past behind the start goes on (no flip at 180 deg)
        const float b = yaw_dir(mid);
        m_mt_ring_acc += std::remainder(b - m_mt_ring_last, glm::two_pi<float>());
        m_mt_ring_last = b;
        yaw   = m_mt_yaw0 + gain * m_mt_ring_acc + m_mt_off[0];
        pitch = m_mt_pitch0 + gain * (std::atan2(mid.z, hd) - std::atan2(m_mt_m0.z, hd0)) + m_mt_off[1];
    } else {
        // The pivot ahead of the hands' start on the gun's heading then; the aim is the line from the hands to it.
        const float     L  = std::max(0.10f, m_mt_pivot.load());
        const glm::vec3 P  = m_mt_m0 + L * glm::vec3{ -std::sin(m_mt_yaw0), std::cos(m_mt_yaw0), 0.0f };
        const glm::vec3 v  = P - mid;
        const float     hv = std::hypot(v.x, v.y);
        if (hv < 0.05f) return;   // the hands at the pivot: no line, the aim kept
        // the line's turn from the start (yaw: its heading against the gun's then; pitch: its tilt from level), times mountedgain, plus the
        // limit re-bases
        const float gain = std::clamp(m_mt_gain.load(), 0.25f, 8.0f);
        yaw   = m_mt_yaw0 + gain * std::remainder(yaw_dir(v) - m_mt_yaw0, glm::two_pi<float>()) + m_mt_off[0];
        pitch = m_mt_pitch0 + gain * std::atan2(v.z, hv) + m_mt_off[1];
    }
    // The mount's limit: the game's aim held off the hands' for 12 frames, not moving: the start moved so the hands' aim is the game's.
    const float act[2]{ ya, pa };
    float       tgt[2]{ yaw, pitch };
    for (int a = 0; a < 2; ++a) {
        const float err   = a == 0 ? std::remainder(tgt[a] - act[a], glm::two_pi<float>()) : tgt[a] - act[a];
        const float moved = std::abs(a == 0 ? std::remainder(act[a] - m_mt_act_last[a], glm::two_pi<float>()) : act[a] - m_mt_act_last[a]);
        m_mt_act_last[a]  = act[a];
        m_mt_stuck[a]     = (std::abs(err) > glm::radians(2.5f) && moved < glm::radians(0.05f)) ? m_mt_stuck[a] + 1 : 0;
        if (m_mt_stuck[a] >= 12) {
            m_mt_stuck[a] = 0;
            m_mt_rebased[a]++;
            m_mt_off[a] -= err;   // the same hands now give where the game holds the gun
            tgt[a] -= err;
        }
    }
    yaw   = tgt[0];
    pitch = std::clamp(tgt[1], glm::radians(-75.0f), glm::radians(75.0f));
    m_mt_hold = dir_of(yaw, pitch);
    // A line a second while a hand holds a handle: the hands' move, the aim given and the game's, the handles' and the camera's moves.
    const auto now = std::chrono::steady_clock::now();
    if (now - m_mt_log_t >= std::chrono::seconds(1)) {
        m_mt_log_t = now;
        glm::vec3 hc{ 0.0f };
        int       hn = 0;
        for (int s = 0; s < 2; ++s) if (frame - m_mt_handle_frame[s] <= 4) { hc += rot_z(-by, m_mt_handle[s] - cam); ++hn; }
        const glm::vec3 dh = (mid - m_mt_m0) * 100.0f;
        const glm::vec3 dg = hn ? (hc / float(hn) - m_mt_handles0) * 100.0f : glm::vec3{ 0.0f };
        const glm::vec3 dc = (cam - m_mt_cam0) * 100.0f;
        spdlog::info("[Weapon] mountedtwohand: hands moved ({:+.0f}, {:+.0f}, {:+.0f}) cm since the grip; aim given yaw {:.1f} pitch {:.1f}, the game's {:.1f} / {:.1f} deg; "
                     "the handles moved ({:+.0f}, {:+.0f}, {:+.0f}) cm from the game camera{}, the camera ({:+.0f}, {:+.0f}, {:+.0f}) cm; limit re-bases yaw {} pitch {}{}",
                     dh.x, dh.y, dh.z, glm::degrees(yaw), glm::degrees(pitch), glm::degrees(ya), glm::degrees(pa), dg.x, dg.y, dg.z, hn ? "" : " (not read)", dc.x, dc.y, dc.z,
                     m_mt_rebased[0], m_mt_rebased[1],
                     veh ? fmt::format("; in a vehicle: angles in its frame, which turned {:+.1f} deg since the grip ({} steps kept in the world)",
                                       glm::degrees(std::remainder(by - m_mt_by0, glm::two_pi<float>())), m_mt_by_jumps)
                         : std::string{});
        m_mt_rebased[0] = m_mt_rebased[1] = 0;
        m_mt_by_jumps   = 0;
    }
}

bool FarCry5Stereo::mounted_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    if (cmd == "mountedhands") {   // mountedhands on|off (saved)
        const bool on  = arg == "on" || arg == "1" || arg == "true";
        m_mt_hands_on  = on;
        spdlog::info("[Weapon] mountedhands {}", on ? "on (on a mount the hands follow the controllers, a grip near a handle holds it; arms and body drawn to them)"
                                                    : "off (on a mount the game's hands stay on the handles, as before 13 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "mountedtwohand") {   // mountedtwohand on|off (saved)
        const bool on    = arg == "on" || arg == "1" || arg == "true";
        m_mt_twohand_on  = on;
        m_mt_ref_mask    = 0;
        m_mt_hold_ok     = false;
        spdlog::info("[Weapon] mountedtwohand {}", on ? "on (on a mount the gun turns with the hands on its handles, as on a pivot)" : "off (on a mount the aim follows mountedaim)");
        persist = true;
        return true;
    }
    if (cmd == "mountedvehicle") {   // mountedvehicle on|off (saved)
        const bool on = arg == "on" || arg == "1" || arg == "true";
        m_mt_veh_on   = on;
        spdlog::info("[Weapon] mountedvehicle {}", on ? "on (a mounted gun in a vehicle's gun or passenger seat: hands, handles and the two-hand aim as on foot)"
                                                      : "off (a mounted gun in a vehicle: the aim left to the vehicle code, as before 14 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "mountedring" || cmd == "mountedstick") {   // mountedring on|off, mountedstick on|off (saved)
        const bool on = arg == "on" || arg == "1" || arg == "true";
        if (cmd == "mountedring") {
            m_mt_ring_on  = on;
            m_mt_ref_mask = 0;
            spdlog::info("[Weapon] mountedring {}", on ? "on (a vehicle's turret turns about you with your hands' bearing and height)"
                                                       : "off (a vehicle's turret turns about a pivot ahead of the hands, as on foot)");
        } else {
            m_mt_stick_on = on;
            spdlog::info("[Weapon] mountedstick {}", on ? "on (on a vehicle's turret the right stick turns the view and the gun with it)" : "off (on a vehicle's turret the right stick does nothing)");
        }
        persist = true;
        return true;
    }
    if (cmd == "mountedringgain") {   // mountedringgain x (saved)
        char*       end = nullptr;
        const float g   = std::strtof(arg.c_str(), &end);
        if (end == arg.c_str() || !std::isfinite(g)) {
            spdlog::info("[Weapon] mountedringgain '{}': a factor (1 = the gun points where the hands are, seen from you)", arg);
            return true;
        }
        m_mt_ring_gain = std::clamp(g, 0.25f, 8.0f);
        m_mt_ref_mask  = 0;
        spdlog::info("[Weapon] mountedringgain {:.2f}: on a vehicle's turret the gun turns {:.2f} times the hands' turn around you", m_mt_ring_gain.load(), m_mt_ring_gain.load());
        persist = true;
        return true;
    }
    if (cmd == "mountedgain") {   // mountedgain x (saved)
        char*       end = nullptr;
        const float g   = std::strtof(arg.c_str(), &end);
        if (end == arg.c_str() || !std::isfinite(g)) {
            spdlog::info("[Weapon] mountedgain '{}': a factor (1 = the pivot's own turn)", arg);
            return true;
        }
        m_mt_gain     = std::clamp(g, 0.25f, 8.0f);
        m_mt_ref_mask = 0;   // the next frame takes a new start
        spdlog::info("[Weapon] mountedgain {:.2f}: on a mount the gun turns {:.2f} times as far as a gun on its pivot would for the same hand move", m_mt_gain.load(), m_mt_gain.load());
        persist = true;
        return true;
    }
    if (cmd == "mountedpivot" || cmd == "mountedgripreach") {   // mountedpivot cm, mountedgripreach cm (saved)
        char*       end = nullptr;
        const float cm  = std::strtof(arg.c_str(), &end);
        if (end == arg.c_str() || !std::isfinite(cm)) {
            spdlog::info("[Weapon] {} '{}': a distance in cm", cmd, arg);
            return true;
        }
        if (cmd == "mountedpivot") {
            m_mt_pivot    = std::clamp(cm, 10.0f, 200.0f) / 100.0f;
            m_mt_ref_mask = 0;   // the next frame takes a new start
            spdlog::info("[Weapon] mountedpivot {:.0f} cm: on a mount the gun turns about a point that far ahead of the hands", m_mt_pivot.load() * 100.0f);
        } else {
            m_mt_reach = std::clamp(cm, 0.0f, 100.0f) / 100.0f;
            spdlog::info("[Weapon] mountedgripreach {:.0f} cm: {}", m_mt_reach.load() * 100.0f,
                         m_mt_reach.load() > 0.0f ? "a grip that close to a handle holds it" : "a grip anywhere holds its handle");
        }
        persist = true;
        return true;
    }
    if (cmd == "mountedweapon") {   // mountedweapon on|off (saved)
        const bool on = arg == "on" || arg == "1" || arg == "true";
        m_mounted_on  = on;
        spdlog::info("[Weapon] mountedweapon {}", on ? "on (a mounted weapon stays on its mount, the aim follows the controller or the head)" : "off (held like a gun, as before 5 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "mountedinvert") {   // mountedinvert on|off (saved)
        const bool on    = arg == "on" || arg == "1" || arg == "true";
        m_mounted_invert = on;
        m_mounted_ref_frame = -1000;   // a new heading at the next mounted frame
        spdlog::info("[Weapon] mountedinvert {}", on ? "on (on a mount, controller up aims down and right aims left)" : "off (on a mount the aim follows the controller's pointing as it is)");
        persist = true;
        return true;
    }
    if (cmd == "mortarview") {   // mortarview on|off (saved)
        const bool on    = arg == "on" || arg == "1" || arg == "true";
        m_mortar_view_cfg = on;
        spdlog::info("[Weapon] mortarview {}", on ? "on (on a mortar a grip held is the game's aiming view, the eyes follow its camera, the right stick moves its target)"
                                                  : "off (on a mortar the grips do nothing but its handles, as before)");
        persist = true;
        return true;
    }
    if (cmd == "mortaraim") {   // mortaraim on|off (saved)
        const bool on    = arg == "on" || arg == "1" || arg == "true";
        m_mortar_aim_on  = on;
        spdlog::info("[Weapon] mortaraim {}", on ? "on (on a mortar with no hand on a handle the aim follows the head or the controller, as the aim choice says)"
                                                 : "off (on a mortar the aim follows the hands on its handles only, as the machine guns)");
        persist = true;
        return true;
    }
    if (cmd == "mountedaim") {   // mountedaim hand|head (saved)
        if (arg != "hand" && arg != "head") {
            spdlog::info("[Weapon] mountedaim '{}': hand or head", arg);
            return true;
        }
        m_mounted_aim = arg == "head" ? 1 : 0;
        spdlog::info("[Weapon] mountedaim {}: on a mount the game's aim follows the {}", arg, arg == "head" ? "head" : "right controller's pointing");
        persist = true;
        return true;
    }
    return false;
}

void FarCry5Stereo::draw_mounted_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_mounted_on.load();
    if (ImGui::Checkbox("Mounted guns and mortars stay on their mount", &v)) {
        m_mounted_on = v;
        keep("mountedweapon", v ? "mountedweapon on" : "mountedweapon off");
    }
    hint("Mounted guns and mortars stay on their mount and aim where you point. Off: held in your hand like a gun.");
    int a = std::clamp(m_mounted_aim.load(), 0, 1);
    ImGui::TextUnformatted("On a mount, aim with");
    ImGui::SameLine();
    bool ch = ImGui::RadioButton("the right controller##mountedaim", &a, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("the head##mountedaim", &a, 1);
    if (ch) {
        m_mounted_aim = a;
        keep("mountedaim", a == 1 ? "mountedaim head" : "mountedaim hand");
    }
    hint("Aim the mounted gun with your right controller, or with your head.");
    bool inv = m_mounted_invert.load();
    if (ImGui::Checkbox("Inverted, like the handles of a gun on a pivot", &inv)) {
        m_mounted_invert    = inv;
        m_mounted_ref_frame = -1000;
        keep("mountedinvert", inv ? "mountedinvert on" : "mountedinvert off");
    }
    hint("Aims like real pivot handles: tilt up to aim down, turn right to aim left.");
    bool mh = m_mt_hands_on.load();
    if (ImGui::Checkbox("Your hands, arms and body on a mounted gun##mountedhands", &mh)) {
        m_mt_hands_on = mh;
        keep("mountedhands", mh ? "mountedhands on" : "mountedhands off");
    }
    hint("On mounted guns your own hands are drawn. Grip near a handle to hold it.");
    bool th = m_mt_twohand_on.load();
    if (ImGui::Checkbox("Turn the gun with your hands on its handles##mountedtwohand", &th)) {
        m_mt_twohand_on = th;
        m_mt_ref_mask   = 0;
        m_mt_hold_ok    = false;
        keep("mountedtwohand", th ? "mountedtwohand on" : "mountedtwohand off");
    }
    hint("Hold the handles and move your hands to swing the gun, like a real one.");
    bool ma = m_mortar_aim_on.load();
    if (ImGui::Checkbox("Mortar: aim with the head or controller (above) when no hand holds a handle##mortaraim", &ma)) {
        m_mortar_aim_on = ma;
        keep("mortaraim", ma ? "mortaraim on" : "mortaraim off");
    }
    hint("On a mortar, aim with your head or controller when no hand holds a handle.");
    bool mvw = m_mortar_view_cfg.load();
    if (ImGui::Checkbox("Mortar: hold a grip for its aiming view##mortarview", &mvw)) {
        m_mortar_view_cfg = mvw;
        keep("mortarview", mvw ? "mortarview on" : "mortarview off");
    }
    hint("On a mortar, hold a grip for its aiming view.");
    bool mv = m_mt_veh_on.load();
    if (ImGui::Checkbox("Also on a vehicle's turret (gun or passenger seat)##mountedvehicle", &mv)) {
        m_mt_veh_on = mv;
        keep("mountedvehicle", mv ? "mountedvehicle on" : "mountedvehicle off");
    }
    hint("Vehicle turret guns work like mounted guns on foot.");
    bool rg = m_mt_ring_on.load();
    if (ImGui::Checkbox("Vehicle turret turns about you##mountedring", &rg)) {
        m_mt_ring_on  = rg;
        m_mt_ref_mask = 0;
        keep("mountedring", rg ? "mountedring on" : "mountedring off");
    }
    hint("A vehicle's turret points the way your hands go around you.");
    float rgn = m_mt_ring_gain.load();
    if (ImGui::SliderFloat("Vehicle turret sensitivity##mountedringgain", &rgn, 0.5f, 4.0f, "%.1fx")) {
        m_mt_ring_gain = rgn;
        m_mt_ref_mask  = 0;
        keep("mountedringgain", fmt::format("mountedringgain {:.2f}", rgn));
    }
    hint("How much the turret turns for your hands. 1: it points where your hands are.");
    bool sk = m_mt_stick_on.load();
    if (ImGui::Checkbox("Right stick turns a vehicle's turret##mountedstick", &sk)) {
        m_mt_stick_on = sk;
        keep("mountedstick", sk ? "mountedstick on" : "mountedstick off");
    }
    hint("On a vehicle's turret, the right stick turns you and the gun together.");
    float pv = m_mt_pivot.load() * 100.0f;
    if (ImGui::SliderFloat("Pivot ahead of the hands (cm)##mountedpivot", &pv, 10.0f, 120.0f, "%.0f")) {
        m_mt_pivot    = pv / 100.0f;
        m_mt_ref_mask = 0;
        keep("mountedpivot", fmt::format("mountedpivot {:.0f}", pv));
    }
    hint("How far ahead of your hands the gun pivots. Shorter: it turns more.");
    float gn = m_mt_gain.load();
    if (ImGui::SliderFloat("Turn sensitivity##mountedgain", &gn, 0.5f, 6.0f, "%.1fx")) {
        m_mt_gain     = gn;
        m_mt_ref_mask = 0;
        keep("mountedgain", fmt::format("mountedgain {:.2f}", gn));
    }
    hint("How much the gun turns for a hand move. Higher: less hand movement needed.");
    float gr = m_mt_reach.load() * 100.0f;
    if (ImGui::SliderFloat("Grip reach to a handle (cm)##mountedgripreach", &gr, 0.0f, 60.0f, "%.0f")) {
        m_mt_reach = gr / 100.0f;
        keep("mountedgripreach", fmt::format("mountedgripreach {:.0f}", gr));
    }
    hint("How close your hand must be to a handle to grab it. 0: anywhere.");
    if (mounted_hands_now())
        ImGui::TextDisabled("Hands on handles: right %s, left %s", m_mt_snap[0] ? "yes" : "no", m_mt_snap[1] ? "yes" : "no");
    std::string why;
    {
        std::lock_guard _{ m_mounted_mtx };
        why = m_mounted_why;
    }
    ImGui::TextDisabled("Weapon %u: %s (%s)", m_mounted_sig, m_mounted_held.load() ? "a mounted weapon" : "not a mounted weapon", why.empty() ? "not decided yet" : why.c_str());
}

// spawnfacing: the player's back axis (x, y; the spawn commands go against it), from where the head faces now (snap turns included), else
// the game camera's heading, else north.
void FarCry5Stereo::spawn_back_axis(float& bx, float& by) const
{
    glm::vec3 f{ 0.0f };
    if (m_motion.load() && m_mo.valid) f = m_mo.head_world;
    if (std::hypot(f.x, f.y) < 0.2f) f = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
    if (std::hypot(f.x, f.y) < 0.2f) f = glm::vec3{ 0.0f, 1.0f, 0.0f };
    bx = -f.x;
    by = -f.y;
}
