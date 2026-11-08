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
// A weapon is a mounted one when the game's data names it under Weapons/Mounted/ (or Weapons_DLCV/Mounted/) or gives it class 10 (the
// mortars); before the game's data is learnt for the weapon id (gameweapon: the same id and item for 45 frames), by the id list below or
// the item the game says the player holds now (FarCry5Lua equipped_item, asked every 150 ms), then again each frame until the data says.
// Vehicles: the first-person pass stays off the controller for a mounted weapon there too, but the aim loop is left to the vehicle code.
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
}

void FarCry5Stereo::mounted_frame_end()
{
    const uint32_t sig = m_weapon_sig.load();   // the weapon in hand (kept through a frame that draws none)
    if (sig == m_mounted_sig && m_mounted_final) return;
    const bool new_sig = sig != m_mounted_sig;
    m_mounted_sig      = sig;
    bool        held   = false;
    bool        final  = true;
    std::string why;
    if (sig == 0) {
        why = "no weapon";
    } else if (const fc5::weapons::Info* w = fc5::gameweapon::info(sig)) {
        held = mounted_info(w);
        why  = fmt::format("the game's data: {} (class {})", w->name ? w->name : "?", w->cls);
    } else if (std::find(std::begin(kMountedIds), std::end(kMountedIds), sig) != std::end(kMountedIds)) {
        held = true;
        why  = "the mod's list of mounted weapon ids";
    } else {
        final         = false;   // asked again each frame until the game's data is learnt
        uint64_t item = 0;
        if (fc5::lua::equipped_item(item) && item != 0) {
            const fc5::weapons::Info* wi = fc5::weapons::find(item);
            held = mounted_info(wi);
            why  = fmt::format("the item the game says is held now: {:x} ({})", item, wi && wi->name ? wi->name : "not in the weapon table");
        } else {
            why = "not known yet";
        }
    }
    const bool was = m_mounted_held.exchange(held);
    m_mounted_final = final;
    {
        std::lock_guard _{ m_mounted_mtx };
        m_mounted_why = why;
    }
    if (held && (!was || new_sig))
        spdlog::info("[Weapon] mounted weapon {}: {}; {}", sig, why,
                     m_mounted_on.load() ? fmt::format("kept on its mount, the aim follows the {} (mountedweapon)", m_mounted_aim.load() == 1 ? "head" : "right controller's pointing")
                                         : std::string{ "mountedweapon off: held like a gun" });
    else if (!held && was)
        spdlog::info("[Weapon] mounted weapon no longer held: weapon {} ({})", sig, why);
}

// update_motion, at its end (the head known): on a mount the aim is the right controller's pointing (mirrored with mountedinvert), from
// the game camera (no muzzle offset, no sight line, no convergence: the direction itself), or the head's (mountedaim head). The barrel ray
// starts at the controller; the red dot is drawn along the aim.
void FarCry5Stereo::mounted_motion(const glm::vec3& cam)
{
    glm::vec3 dir{ 0.0f };
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

bool FarCry5Stereo::mounted_command(const std::string& cmd, const std::string& arg, bool& persist)
{
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
    hint("On (default): using a mounted machine gun or mortar, it stays on its mount and turns with it, as the game draws it; the game's aim follows "
         "where your right controller points (or your head, below), and the trigger fires at once. Gestures, two-handed hold, sights and the "
         "holster leave it alone. Off: held on the controller like a gun (as before 5 Nov). mountedweapon");
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
    hint("The right controller (default): point it where the gun should aim, within the mount's limits. The head: the gun aims where you look. mountedaim");
    bool inv = m_mounted_invert.load();
    if (ImGui::Checkbox("Inverted, like the handles of a gun on a pivot", &inv)) {
        m_mounted_invert    = inv;
        m_mounted_ref_frame = -1000;
        keep("mountedinvert", inv ? "mountedinvert on" : "mountedinvert off");
    }
    hint("On (default): with the right controller, tilting it up lowers the aim and the red dot, turning it right moves them left. Straight ahead "
         "(where you faced when you got on) and level aims straight ahead and level. Off: the aim follows the controller's pointing as it is. mountedinvert");
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
