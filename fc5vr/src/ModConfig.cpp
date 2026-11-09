#include "ModConfig.h"
#include "Mods.hpp"
#include <mods/VR.hpp>
#include <mods/VRConfig.hpp>
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif

#include "ModSettings.h"
#include "FarCry5/FarCry5Stereo.h"
#include "FarCry5/FarCry5Probe.h"
#include "FarCry5/FarCry5Lua.h"
#include "FarCry5/FarCry5LoadPopup.h"
#include "FarCry5/FarCry5Reload.h"
#include "FarCry5/FarCry5SteerTune.h"
#include <mutex>

namespace ModSettings {
    // HudScale g_hudScale;
    // CrosshairTranslate g_crosshairTranslate;
    // AW2InternalSettings g_aw2_settings;
    // AW2GameFlow g_game_state;
    // DebugAndCalibration g_debugAndCalibration;
}

Mods::Mods() {
    m_mods.emplace_back(VRConfig::get());
    // Before VR: its on_present prepares the AFW held eye that VR's submission then uses.
    m_mods.emplace_back(FarCry5Stereo::Get());
    m_mods.emplace_back(VR::get());
#ifdef _DEBUG
    m_mods.emplace_back(ShaderDebugOverlay::Get());
#endif
}




// The game's pad rumble on the controllers. A pad has a heavy left motor and a light right one, and the game puts its
// shots mostly on the heavy one: the framework's motor-by-side routing (rumble game) buzzed only the left hand when firing
// (tester, author 24 Oct). FarCry5Stereo::rumble_hands decides which hand gets what (rumble gun|both|game, rumblegain).
void VR::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    if (user_index != m_lowest_xinput_user_index || vibration == nullptr) {
        return;
    }
    if (!is_using_controllers()) {
        return;
    }
    float amp[2]{};   // the left controller, the right controller
    FarCry5Stereo::Get()->rumble_hands(vibration->wLeftMotorSpeed / 65535.0f, vibration->wRightMotorSpeed / 65535.0f, amp);
    if (amp[0] > 0.0f) trigger_haptic_vibration(0.0f, 0.1f, 1.0f, amp[0], get_left_joystick());
    if (amp[1] > 0.0f) trigger_haptic_vibration(0.0f, 0.1f, 1.0f, amp[1], get_right_joystick());
}

// drivegunthrottle (FarCry5Drive.cpp, patch 15): when the pad last sent Y or LB while driving (the mod's driveready tap, the right trigger's
// shot or the player's own), and when the game last put the sidearm away: right A sends no RT around them.
static std::atomic<int64_t> s_drive_draw_ms{ -100000 }, s_drive_gun_off_ms{ -100000 };
static std::atomic<bool>    s_drive_gun_was{ false };

void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
//    ZoneScopedN(__FUNCTION__);
    // The hooked XInputGetState hands over a whole XINPUT_STATE: the pad is after the packet number
    // (the Starfield code this comes from passed the pad itself; cast here, every write landed 4 bytes
    // early: stick X on the buttons, stick Y on the triggers).
    auto pXinputGamepad = &state->Gamepad;
    // The game only polls XInput while some pad is plugged in, so the mod plugs in a virtual one
    // (ViGEm); the state the game reads is still built here, in the hook: the controllers replace
    // whatever the polled pad reports and every other pad is hidden.
    const bool from_pad = FarCry5Stereo::Get()->in_pad_update();
    const uint32_t probe_ret_in = *retval;
    struct ProbeOnExit
    {
        uint32_t* r; uint32_t in; uint32_t idx; XINPUT_GAMEPAD* g;
        ~ProbeOnExit() { fc5::probe::note_xinput(FarCry5Stereo::Get()->in_pad_update(), false, idx, in, *r, g); }
    } probe_on_exit{ retval, probe_ret_in, user_index, pXinputGamepad };

    const auto now = std::chrono::steady_clock::now();

    if (!from_pad) {
    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }
        if (is_using_controllers()) {
            // Only the pad the VR controllers drive exists for the game (a headset runtime's own
            // gamepad emulation would add its own buttons and sticks).
            *retval = ERROR_DEVICE_NOT_CONNECTED;
            std::memset(state, 0, sizeof(*state));
        }
        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }

    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;
    }

    if (is_using_controllers_within(std::chrono::minutes(5)))
        if (is_using_controllers_within(std::chrono::minutes(5)))
        {
            *retval = ERROR_SUCCESS;

        }

    // Tests (gesturegrip, throwtest): the simulated hands count as controller use (the simulator's controllers send no input).
    if (fc5::gesture::test_input()) m_last_controller_update = now;
    // padsim (test command): grips and sticks of the physical left / right controller replaced.
    const auto pad_sim = FarCry5Stereo::Get()->pad_sim();
    if (pad_sim.on) m_last_controller_update = now;
    const auto axis_of = [&](vr::VRInputValueHandle_t h) {
        if (!pad_sim.on) return get_joystick_axis(h);
        const float* a = h == m_left_joystick ? pad_sim.l : pad_sim.r;
        return Vector2f{ a[0], a[1] };
    };
    const auto grip_of = [&](vr::VRInputValueHandle_t h) {
        if (!pad_sim.on) return is_action_active(m_action_grip, h);
        return h == m_left_joystick ? pad_sim.lgrip : pad_sim.rgrip;
    };
    if (!is_using_controllers()) {
        VR::s_fc5_menu_trigger = false;
        return;
    }
    // The VR controllers are the whole pad: whatever a physical or emulated pad in this slot reports
    // is dropped (the packet number must still change for the game to take the new state).
    {
        static DWORD packet = 0;
        std::memset(pXinputGamepad, 0, sizeof(*pXinputGamepad));
        state->dwPacketNumber = ++packet;
    }

//    // Clear button state for VR controllers
//    if (is_using_controllers_within(std::chrono::seconds(5))) {
//        state->Gamepad.wButtons = 0;
//        state->Gamepad.bLeftTrigger = 0;
//        state->Gamepad.bRightTrigger = 0;
//        state->Gamepad.sThumbLX = 0;
//        state->Gamepad.sThumbLY = 0;
//        state->Gamepad.sThumbRX = 0;
//        state->Gamepad.sThumbRY = 0;
//    }


    const auto wants_swap = m_swap_controllers->value();
    const auto left_joystick = !wants_swap ? get_left_joystick():get_right_joystick();
    const auto right_joystick = !wants_swap? get_right_joystick():get_left_joystick();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    static bool ui_toggle_active = false;
    bool both_clicks_down = is_left_joystick_click_down && is_right_joystick_click_down;

    if (both_clicks_down && !ui_toggle_active && !Framework::s_fc5_test_mode.load()) {
        ui_toggle_active = true;
        g_framework->set_draw_ui(!g_framework->is_drawing_ui());
    } else if (!is_left_joystick_click_down || !is_right_joystick_click_down) {
        ui_toggle_active = false;
    }

    if (g_framework->is_drawing_ui()) {
        // The F11 menu: right stick scrolls / nudges sliders, the right trigger clicks.
        const auto axis = axis_of(right_joystick);
        const bool trigger = is_action_active(m_action_trigger, right_joystick);
        FarCry5Stereo::Get()->set_menu_input(axis.x, axis.y, trigger);
        VR::s_fc5_menu_trigger = trigger;   // clicks through OverlayComponent, with A
        return;
    }
    FarCry5Stereo::Get()->set_menu_input(0.0f, 0.0f, false);
    VR::s_fc5_menu_trigger = false;
    // loadpopup (FarCry5LoadPopup.cpp): while "VR mod loading" shows, the player's grips, triggers, buttons and sticks do not reach the game;
    // the mod's own presses (the look at the sidearm slot) and the camera's steering onto the head still do.
    const bool load_block = fc5::loadpopup::blocking();
    // drivebuttons (FarCry5Drive.cpp; author 5 Nov 22:47): driving a car, quad, motorcycle or boat (vehscript type 0, 1, 3), right A
    // is the throttle (RT), left X brakes / reverses (LT) and a double left X is the handbrake (B); the triggers, grips and the right stick no
    // longer accelerate or brake there (below). driveplanestick off (author): in a plane
    // (type 5) the right stick sends nothing.
    // Patch 15 v2 (author 7 Nov 20:41): only the driver's seat (or the seat not known yet) gets the
    // A throttle. Riding (vehscript's driver 0: a passenger or a gun seat), the game's RT shoots, so right A is pad A there (change seat) as on
    // foot (pad A: jump), in the air and with drivebuttons off; pad A never fires a weapon.
    int veh_type_now = -1;
    int veh_drv_now  = -1;
    if (FarCry5Stereo::Get()->in_vehicle() && !FarCry5Stereo::Get()->flat_screen_on()) {
        int drv = -1, type = -1;
        if (fc5::lua::vehicle_state(drv, type) == 1) {
            veh_type_now = type;
            veh_drv_now  = drv;
        }
    }
    const bool btn_type    = fc5::drive::buttons() && (veh_type_now == 0 || veh_type_now == 1 || veh_type_now == 3);
    const bool btn_layout  = btn_type && veh_drv_now != 0;
    {
        static std::atomic<int> rd_said{ 0 };
        static std::atomic<bool> rd_was{ false };
        const bool riding = btn_type && veh_drv_now == 0;
        if (rd_was.exchange(riding) != riding && rd_said.fetch_add(1) < 20)
            spdlog::info(riding ? "[Drive] drivebuttons: riding (not the driver's seat): right A is pad A (change seat), never the throttle or RT"
                                : "[Drive] drivebuttons: no longer riding");
    }
    const auto btn         = fc5::drive::buttons_poll(btn_layout && !load_block, is_right_a_button_down, is_left_a_button_down, veh_type_now);
    const bool plane_quiet = veh_type_now == 5 && !fc5::drive::plane_stick();

    // One button layout (the F11 Layout tab; the "A jump, B reload, X interact, Y switch weapon" choice was removed 24 Oct, author).
    // drivebuttons: right A is the throttle and left X the brake there (no change of seat, no B but the double press's handbrake).
    if (is_right_a_button_down && !btn_layout) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_A;
    }

    if ((is_left_a_button_down && !btn_layout) || btn.handbrake) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_raw = !load_block && is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);
    // popupesc (FarCry5Stereo::popup_esc_now): a press that starts while a tutorial pop-up has paused the game sends Esc, the
    // pop-up's only key, and is not the pad's Y (nor RB with the left grip) until released.
    // (Atomics: the game's polls and the virtual pad's updates both come here; one of them takes the press.)
    static std::atomic<bool> right_b_was{ false }, right_b_esc{ false };
    if (is_right_b_raw) {
        if (!right_b_was.exchange(true)) {
            const bool esc = FarCry5Stereo::Get()->popup_esc_now();
            right_b_esc    = esc;
            if (esc) FarCry5Stereo::Get()->popup_esc_tap();
        }
    } else {
        right_b_was = false;
        right_b_esc = false;
    }
    const auto is_right_b_button_down = is_right_b_raw && !right_b_esc.load();

    // bheal (manualreload38, default on; author 3 Nov 12:00): right B is the pad's Y only
    // once held past a quick press, then for at least bhealms ms (heal, never the game's previous weapon).
    if (fc5::reload::b_heal_only() ? fc5::reload::b_heal_y(is_right_b_button_down) : is_right_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_Y;
    }

    // reloadbuttonguard (FarCry5Reload.cpp): held back during a manual reload step until held; reloadbutton: the game's reload, shown, followed.
    if (fc5::reload::reload_button_pass(is_left_b_button_down)) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_X;
    }


    {
        if (is_left_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }

        if (is_right_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }
    }


    const auto is_left_trigger_down = !load_block && is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = !load_block && is_action_active(m_action_trigger, right_joystick);
    // Body gestures (FarCry5Gesture.cpp): a grip press that starts in a gesture's zone (the weapon hand at its shoulder:
    // switch weapon) taps the gesture's pad buttons, and that press is not the grip's usual action until released.
    const auto is_left_grip_raw  = !load_block && grip_of(left_joystick);
    const auto is_right_grip_raw = !load_block && grip_of(right_joystick);
    // drivepadecho (FarCry5Drive.cpp, test): the presses the runtime reports, against what the virtual pad sent just before.
    fc5::drive::echo_read((is_right_a_button_down ? fc5::drive::kEchoRA : 0u) | (is_left_a_button_down ? fc5::drive::kEchoLX : 0u) |
                          (is_right_b_raw ? fc5::drive::kEchoRB : 0u) | (is_left_b_button_down ? fc5::drive::kEchoLY : 0u) |
                          (is_right_trigger_down ? fc5::drive::kEchoRT : 0u) | (is_left_trigger_down ? fc5::drive::kEchoLT : 0u) |
                          (is_right_grip_raw ? fc5::drive::kEchoRG : 0u) | (is_left_grip_raw ? fc5::drive::kEchoLG : 0u));
    {
        // fingergrip: the index and the thumb follow the real ones (trigger pressed / touched; the thumb on a face button,
        // the thumbrest, or moving or pressing the stick).
        auto thumb_on = [&](vr::VRActionHandle_t a, vr::VRActionHandle_t b, vr::VRActionHandle_t rest, vr::VRInputValueHandle_t j) {
            const auto s = axis_of(j);
            return is_action_active(a, j) || is_action_active(b, j) || is_action_active(rest, j) || is_action_active(m_action_joystick_click, j) ||
                   std::abs(s.x) > 0.2f || std::abs(s.y) > 0.2f;
        };
        FarCry5Stereo::Get()->finger_inputs(is_right_trigger_down, is_left_trigger_down, is_action_active(m_action_trigger_touch, right_joystick),
                                            is_action_active(m_action_trigger_touch, left_joystick),
                                            thumb_on(m_action_a_button_touch_right, m_action_b_button_touch_right, m_action_thumbrest_touch_right, right_joystick),
                                            thumb_on(m_action_a_button_touch_left, m_action_b_button_touch_left, m_action_thumbrest_touch_left, left_joystick));
    }
    const bool lh                = FarCry5Stereo::Get()->left_handed();
    // dpadear (FarCry5Stereo::left_grip_at_ear; author 7 Nov 14:08): the left grip is the modifier (a stick as
    // the D-pad, + right B, + menu, + left stick click, and B as the throw for reloadmanual) only for a press that starts with the left controller at
    // the left ear / eye, until it is let go. Elsewhere it grabs, holds, brakes or descends. padsim old: as before.
    const bool dpad_ear          = FarCry5Stereo::Get()->dpad_ear_on() && !pad_sim.old;
    const bool left_at_ear       = FarCry5Stereo::Get()->left_grip_at_ear(is_left_grip_raw);
    // reloadmanual (FarCry5Reload.cpp): right B and the left grip (the throw's modifier) go to the gestures too: a quick B press with a listed
    // weapon in hand drops its magazine and is not the pad's Y (gesture.y_off below).
    const auto gesture           = FarCry5Stereo::Get()->gesture_poll(lh ? is_left_grip_raw : is_right_grip_raw, lh ? is_right_grip_raw : is_left_grip_raw,
                                                                      is_right_b_button_down, lh ? is_right_grip_raw : is_left_grip_raw && (!dpad_ear || left_at_ear));
    if (gesture.y_off) pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_Y;
    const int  left_role         = lh ? fc5::gesture::kWeaponHand : fc5::gesture::kOtherHand;
    const int  right_role        = lh ? fc5::gesture::kOtherHand : fc5::gesture::kWeaponHand;
    if (gesture.buzz[left_role] > 0.0f) trigger_haptic_vibration(0.0f, gesture.buzz_s[left_role], 1.0f, gesture.buzz[left_role], left_joystick);
    if (gesture.buzz[right_role] > 0.0f) trigger_haptic_vibration(0.0f, gesture.buzz_s[right_role], 1.0f, gesture.buzz[right_role], right_joystick);
    // drive (FarCry5Drive.cpp; F11 Experimental > Gesture driving): while driving, a grip pressed at the wheel, the handlebars or the stick holds it
    // until let go; that grip is not its usual action meanwhile (the left grip does not brake nor act as the modifier).
    const auto drive = FarCry5Stereo::Get()->drive_poll(is_right_grip_raw, is_left_grip_raw);
    if (drive.buzz[fc5::drive::kRight] > 0.0f) trigger_haptic_vibration(0.0f, 0.08f, 1.0f, drive.buzz[fc5::drive::kRight], right_joystick);
    if (drive.buzz[fc5::drive::kLeft] > 0.0f) trigger_haptic_vibration(0.0f, 0.08f, 1.0f, drive.buzz[fc5::drive::kLeft], left_joystick);
    const auto is_left_grip_button_down = is_left_grip_raw && !gesture.grip_taken[left_role] && !drive.grip_taken[fc5::drive::kLeft];
    const auto is_right_grip_down       = is_right_grip_raw && !gesture.grip_taken[right_role] && !drive.grip_taken[fc5::drive::kRight];
    // Two-handed grip: the camera code grabs the weapon when the left grip is pressed at
    // the weapon. While the left hand holds it, the grip's other functions (left stick as
    // D-pad, grip combos) are suspended, so you can still walk and shoot.
    static std::chrono::steady_clock::time_point left_grip_pressed_at{};
    static bool                                  left_grip_was_down = false;
    const auto                                   input_now          = std::chrono::steady_clock::now();
    if (is_left_grip_button_down && !left_grip_was_down) {
        left_grip_pressed_at = input_now;
    }
    left_grip_was_down          = is_left_grip_button_down;
    // The two-handed hold (FarCry5Stereo) takes the off hand's grip; while it holds, the grip is not a modifier.
    FarCry5Stereo::Get()->set_other_grip(FarCry5Stereo::Get()->left_handed() ? is_right_grip_down : is_left_grip_button_down);
    // A press is no modifier until the two-handed hold has decided on it (one frame later in update_motion; 150 ms
    // when the motion code is not running): the first polls of a grab sent the D-pad (author 17 Oct 21:12, the
    // binoculars toggled at each grab while sprinting).
    const bool grab_attempt     = !pad_sim.old && !FarCry5Stereo::Get()->other_grip_decided() &&
                              input_now - left_grip_pressed_at < std::chrono::milliseconds(150);
    // gripnearweapon (FarCry5Stereo, author 5 Nov 16:45): a press of the left grip near the weapon in hand is the weapon's, held with two hands
    // or not (a gun with no second-hand grip too): no D-pad and no grip combos until it is released. Right-handed only (left-handed, the
    // other hand is the right one).
    const bool grip_near_weapon  = !lh && FarCry5Stereo::Get()->other_grip_near_weapon();
    // is_left_grip_free: not the weapon's (the brake / descend below); is_left_grip_down: the modifier (dpadear: only a press at the ear).
    const auto is_left_grip_free = is_left_grip_button_down && !FarCry5Stereo::Get()->two_hand_held() && !grip_near_weapon && !grab_attempt;
    const auto is_left_grip_down = is_left_grip_free && (!dpad_ear || left_at_ear);
    {
        // dpadear: a short buzz on the left hand when a press at the ear becomes the modifier (so you know the D-pad is on).
        static std::atomic<bool> mod_was{ false };
        if (is_left_grip_down != mod_was.exchange(is_left_grip_down) && is_left_grip_down && dpad_ear)
            trigger_haptic_vibration(0.0f, 0.05f, 1.0f, 0.35f, left_joystick);
    }
    // vehtriggers left (FarCry5VehTransition.cpp): driving, the left hand drives (left trigger throttle, left grip brake) and the right hand
    // shoots (right trigger = LB); the right grip sends nothing (it holds the weapon). The left grip stays the modifier for the right stick
    // (D-pad), right B (throw / rockets) and the menu button (map), but the left stick keeps steering while it brakes.
    const bool veh_left = FarCry5Stereo::Get()->veh_triggers_swapped();
    // A stick already pushed when the grip becomes the modifier (walking or turning while reaching for the gun, or the
    // hold ending while walking) keeps its own use until it comes back to the centre; then it is the D-pad.
    static bool grip_mod_was = false, grip_dpad_left = false, grip_dpad_right = false;
    {
        const auto la      = axis_of(m_left_joystick);
        const auto ra      = axis_of(right_joystick);
        const bool l_centre = std::abs(la.x) < 0.3f && std::abs(la.y) < 0.3f;
        const bool r_centre = std::abs(ra.x) < 0.3f && std::abs(ra.y) < 0.3f;
        if (!is_left_grip_down) {
            grip_dpad_left = grip_dpad_right = false;
        } else if (!grip_mod_was) {
            grip_dpad_left  = l_centre;
            grip_dpad_right = r_centre;
        } else {
            grip_dpad_left  = grip_dpad_left || l_centre;
            grip_dpad_right = grip_dpad_right || r_centre;
        }
        grip_mod_was = is_left_grip_down;
        if (pad_sim.old) grip_dpad_left = grip_dpad_right = is_left_grip_down;
        // gesturemisswalk: a grip pressed just outside a gesture zone (a missed grab) leaves the left stick walking.
        if (gesture.miss_press[left_role]) grip_dpad_left = false;
        if (veh_left) grip_dpad_left = false;   // vehtriggers left: braking with the left grip, the left stick still steers
    }


    if(!Framework::s_fc5_test_mode.load() && ((is_left_joystick_click_down && is_left_grip_down) || (GetAsyncKeyState(VK_F12) & 1))) {
        static std::chrono::steady_clock::time_point m_last_grip_click = std::chrono::steady_clock::now();
        if(std::chrono::steady_clock::now() - m_last_grip_click > std::chrono::milliseconds(500)) {
            m_last_grip_click                           = std::chrono::steady_clock::now();
            auto& flatScreen = ModSettings::g_internalSettings.forceFlatScreen;
            flatScreen = !flatScreen;
        }
    }

    const auto system_button_down = is_action_active_any_joystick(m_action_system_button);
    // drivemenu (FarCry5Drive.h; author 7 Nov 12:02): in a vehicle, any seat (drivemenu all: on foot too), the menu button alone is the map on a short
    // press (the pad's Back, tapped 120 ms when it is let go before drivemenuhold ms) and the pause menu on a long one (Start from drivemenuhold
    // ms on, while held). While the pause menu a long press opened is up, a short press closes it (Start). Left grip + menu stays the map at
    // once (Back while held). The map or a menu opened from a vehicle keeps the vehicle's rule until play is back in 3D. Elsewhere, or
    // drivemenu off: Start, and left grip + menu Back, while held, as before.
    // (A mutex: the game's polls and the virtual pad's updates both come here; each press is timed once for both.)
    WORD menu_buttons = 0;
    {
        using mclock = std::chrono::steady_clock;
        static std::mutex         menu_mx;
        std::scoped_lock          menu_lk{ menu_mx };
        static mclock::time_point menu_down_t{}, menu_tap_t{}, menu_last{}, menu_paused_t{};
        static bool               menu_was = false, menu_classic = false, menu_grip = false, menu_timed = false, menu_long = false;
        static bool               menu_veh = false, menu_paused = false, menu_paused_seen = false;
        static WORD               menu_tap = 0;
        const auto                mnow   = mclock::now();
        const int                 mmode  = fc5::drive::menu_mode();
        const bool                mflat  = FarCry5Stereo::Get()->flat_screen_on();
        if (!mflat) menu_veh = FarCry5Stereo::Get()->in_any_vehicle();   // the map or a menu opened from a vehicle keeps the vehicle's rule
        const bool                mtimed = mmode == 2 || (mmode == 1 && menu_veh);
        if (mnow - menu_last > std::chrono::milliseconds(500)) {   // no polls for a while (the F11 menu, a load): nothing pending; a button
            menu_was = menu_classic = menu_grip = menu_timed = menu_long = false;   // held now counts as a new press
            menu_tap     = 0;
        }
        menu_last = mnow;
        if (menu_paused) {   // the pause menu the long press opened: up on the flat screen, then gone (or never shown within 3 s)
            if (mflat) menu_paused_seen = true;
            else if (menu_paused_seen || mnow - menu_paused_t > std::chrono::seconds(3)) menu_paused = menu_paused_seen = false;
        }
        if (system_button_down && !menu_was) {   // a new press: its rule is decided now
            menu_classic = !mtimed;
            menu_grip    = mtimed && is_left_grip_down;
            menu_timed   = mtimed && !is_left_grip_down;
            menu_long    = false;
            menu_down_t  = mnow;
            menu_tap     = 0;
        }
        menu_was = system_button_down;
        if (!system_button_down) menu_classic = menu_grip = false;
        if (menu_classic) {
            menu_buttons = is_left_grip_down ? XINPUT_GAMEPAD_BACK : XINPUT_GAMEPAD_START;
        } else if (menu_grip) {
            menu_buttons = XINPUT_GAMEPAD_BACK;
        } else if (menu_timed) {
            const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(mnow - menu_down_t).count();
            if (system_button_down) {
                if (!menu_long && held >= fc5::drive::menu_hold_ms()) {
                    menu_long        = true;
                    menu_paused      = !menu_paused;   // Start opens the pause menu, or closes the one a long press opened
                    menu_paused_seen = false;
                    menu_paused_t    = mnow;
                    spdlog::info("[Pad] drivemenu: menu button held {} ms: {} (Start)", held, menu_paused ? "the pause menu" : "closes the pause menu");
                }
                if (menu_long) menu_buttons = XINPUT_GAMEPAD_START;
            } else {
                menu_timed = false;
                if (!menu_long) {
                    menu_tap   = menu_paused ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_BACK;
                    menu_tap_t = mnow;
                    spdlog::info("[Pad] drivemenu: menu button let go after {} ms: {}", held,
                                 menu_paused ? "closes the pause menu (Start)" : mflat ? "Back (on the flat screen: closes the map)" : "the map (Back)");
                    menu_paused = menu_paused_seen = false;
                }
            }
        }
        if (menu_tap != 0) {
            if (mnow - menu_tap_t < std::chrono::milliseconds(120)) menu_buttons |= menu_tap;
            else menu_tap = 0;
        }
    }
    pXinputGamepad->wButtons &= ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);
    pXinputGamepad->wButtons |= menu_buttons;

    // Right grip: LT in a vehicle (brake / reverse; with vehtriggers left the left grip, below) and on the flat screen. Right trigger: RT (fire, throttle).
    // On foot LT is aim down sights, which narrows the game camera the game culls with: the grip never sends it there
    // ("Right grip aims down sights", gripads, was removed 24 Oct, author).
    // scopelearn: a short aim-down-sights press to read a gun's scope zoom (FarCry5Scope.cpp).
    // vehtriggers left: driving, LT (brake / reverse, throttle down, descend) comes from the left grip instead; a press that is also used
    // for a combo (right B, the menu button, the right stick as D-pad) stops braking until let go, and one held when the layout changes
    // waits until let go.
    bool left_grip_lt = false;
    {
        static std::atomic<bool> was_left{ false }, lgrip_wait{ false }, lgrip_combo{ false };
        if (veh_left != was_left.exchange(veh_left)) lgrip_wait = is_left_grip_button_down;
        if (!is_left_grip_button_down) lgrip_wait = false;
        const auto ra    = axis_of(right_joystick);
        const bool combo = is_right_b_button_down || system_button_down || (grip_dpad_right && (std::abs(ra.x) >= 0.5f || std::abs(ra.y) >= 0.5f));
        if (!is_left_grip_down) lgrip_combo = false;
        else if (combo) lgrip_combo = true;
        // dpadear: any free press away from the ear brakes (it is never a combo); a press at the ear is the modifier and never brakes.
        left_grip_lt = veh_left && is_left_grip_free && !(dpad_ear && left_at_ear) && !lgrip_wait.load() && !lgrip_combo.load() && !btn_layout;   // drivebuttons: X brakes instead
    }
    // menutabs (FarCry5Stereo.h; author 7 Nov 13:56): on the flat menu screen (the game's menus, the map, a load; not F12's flat play) the
    // triggers are the game's tabs, the left trigger LB and the right trigger RB (below), and the grips take the LT / RT the right trigger and the
    // right grip sent there: the left grip LT (the map zooms out, a menu's page to the left), the right grip RT (the map zooms in, the page to the
    // right). The left grip sends LT only once held 150 ms alone: a press used for a combo (+ menu: the map; + right B: RB; + a stick: the D-pad;
    // + left stick click: flat screen) sends no LT until let go, and one held as the menu screen comes up waits until let go.
    // menutabs off: as before (right trigger RT, right grip LT, left grip + right B RB). Left grip + right B stays RB either way.
    const bool menu_tabs = FarCry5Stereo::Get()->menu_tabs();
    bool       menu_lt   = false;
    {
        static std::atomic<bool> mt_was{ false }, mt_wait{ false }, mt_combo{ false };
        if (menu_tabs != mt_was.exchange(menu_tabs)) mt_wait = is_left_grip_button_down;
        if (!is_left_grip_button_down) mt_wait = false;
        const auto ra    = axis_of(right_joystick);
        const auto la    = axis_of(m_left_joystick);
        const bool combo = is_right_b_button_down || system_button_down || is_left_joystick_click_down ||
                           (grip_dpad_right && (std::abs(ra.x) >= 0.5f || std::abs(ra.y) >= 0.5f)) || (grip_dpad_left && (std::abs(la.x) >= 0.5f || std::abs(la.y) >= 0.5f));
        // dpadear: the grip's own press (is_left_grip_free), so away from the ear it is still LT here; only a press that is the modifier (at the
        // ear, or any with dpadear off) makes a combo. Before 9 Nov the left grip sent LT in menus only with the hand at the ear.
        if (!is_left_grip_free) mt_combo = false;
        else if (combo && is_left_grip_down) mt_combo = true;
        menu_lt = menu_tabs && is_left_grip_free && !mt_wait.load() && !mt_combo.load() && input_now - left_grip_pressed_at >= std::chrono::milliseconds(150);
    }
    const bool menu_rt       = menu_tabs && is_right_grip_down;
    bool       menu_rb       = false;   // menutabs: RB sent from the right trigger this poll (not a throw: note_rb below)
    const bool right_grip_lt = is_right_grip_down && FarCry5Stereo::Get()->grip_sends_lt() && !veh_left && !btn_layout && !menu_tabs;
    if (right_grip_lt || left_grip_lt || menu_lt || FarCry5Stereo::Get()->debug_lt() || FarCry5Stereo::Get()->scope_probe_lt()) {
        pXinputGamepad->bLeftTrigger = 255;
    }
    FarCry5Stereo::Get()->note_lt(pXinputGamepad->bLeftTrigger > 128);
    FarCry5Stereo::Get()->note_grip(FarCry5Stereo::Get()->left_handed() ? is_left_grip_button_down : is_right_grip_down);

    // Right trigger: RT (fire; the throttle while driving with vehtriggers right). Left trigger: LB (hold: the weapon wheel on foot; shoot from a vehicle).
    // vehtriggers left: driving, the two swap: the right trigger is the game's vehicle fire (LB: the sidearm drawn
    // and fired, or the vehicle's guns) and the left trigger the throttle (RT). A trigger held when the layout changes (getting in or out, a seat
    // change) sends nothing until it is let go: a held throttle does not open the weapon wheel, a held fire does not accelerate.
    {
        static std::atomic<bool> was_swapped{ false }, right_wait{ false }, left_wait{ false }, shot_was{ false }, rbrake_was{ false }, btn_was{ false }, menu_was{ false };
        const bool swapped = veh_left;
        if (swapped != was_swapped.exchange(swapped)) {
            right_wait = is_right_trigger_down;
            left_wait  = is_left_trigger_down;
            FarCry5Stereo::Get()->note_veh_triggers(swapped, is_right_trigger_down, is_left_trigger_down);
        }
        // drivebuttons: a trigger held when the button layout starts or ends (the vehicle's type known a moment after the seat) waits as well.
        if (btn_layout != btn_was.exchange(btn_layout)) {
            right_wait = right_wait.load() || is_right_trigger_down;
            left_wait  = left_wait.load() || is_left_trigger_down;
        }
        // drive: the right hand on the wheel or handlebars brakes with its trigger (LT) instead of shooting (LB). A right trigger held when the hand
        // takes or lets go of the wheel sends nothing until let go (no shot from a brake, no brake from a shot). drivebuttons: no brake from it.
        const bool rbrake = swapped && drive.right_brake && !btn_layout;
        if (rbrake != rbrake_was.exchange(rbrake)) right_wait = right_wait.load() || is_right_trigger_down;
        // menutabs: a right trigger held as the menu screen comes up or goes sends nothing until let go (a held shot is no tab change, a press
        // held on a tab as play comes back is no shot).
        if (menu_tabs != menu_was.exchange(menu_tabs)) right_wait = right_wait.load() || is_right_trigger_down;
        if (!is_right_trigger_down) right_wait = false;
        if (!is_left_trigger_down) left_wait = false;
        const bool right = is_right_trigger_down && !right_wait.load(), left = is_left_trigger_down && !left_wait.load();
        // drivebuttons: whichever vehtriggers says, the right trigger shoots (LB) unless the right hand holds the gesture wheel or bars (then
        // nothing), and the left trigger sends nothing: neither accelerates nor brakes.
        const bool btn_shot = btn_layout && right && !drive.held[fc5::drive::kRight];
        if (menu_tabs) {
            if (right) {   // menutabs: the tab to the right
                pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                menu_rb = true;
            }
        } else if (!btn_layout && (swapped ? left : right)) {
            pXinputGamepad->bRightTrigger = 255;
        }
        if (menu_rt) pXinputGamepad->bRightTrigger = 255;   // menutabs: the right grip (the map zooms in)
        if (btn_layout) {
            if (btn_shot) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        } else if (rbrake && right) {
            pXinputGamepad->bLeftTrigger = 255;
        } else if (swapped ? right : left) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }
        const bool shot = (swapped && right && !rbrake && !btn_layout) || btn_shot;
        if (shot && !shot_was.exchange(true)) FarCry5Stereo::Get()->m_vt_shots++;
        if (!shot) shot_was = false;
        // vehdrivegun: when the right trigger last sent LB while driving (a weapon view soon after is the driver's sidearm).
        if (shot) FarCry5Stereo::Get()->m_vt_lb_t = std::chrono::steady_clock::now().time_since_epoch().count();
        FarCry5Stereo::Get()->m_vt_lb_held = shot;
    }


    // driveready (FarCry5Drive.h; author 7 Nov 08:54). The game's own pad
    // data (research\keybinding\input_mapping_db.fcb, the triggers vehicle layout) has one driver's input for the sidearm: LB, which draws it and
    // then fires. Its "change weapon" (Y) draws it without a shot (the author's 29 Oct 10:12 log). So, driving a car, quad, motorcycle or boat with
    // the right trigger as the shot (drivebuttons or vehtriggers left), a right grip the gesture drive does not take (away from the wheel or bars)
    // taps Y (driveready lb: LB, test) when no weapon is in the hands; held, it taps again once the game has put the sidearm away (about 3 s
    // after the last shot), at most every 2.5 s. The right trigger shoots (LB) as before. A grip at the wheel holds the wheel.
    {
        static std::atomic<bool>    rg_was{ false }, rd_wait{ false };
        static std::atomic<int64_t> tap_until{ 0 }, last_tap{ -100000 };
        static std::atomic<int>     rd_said{ 0 };
        const int     rmode    = fc5::drive::ready_mode();
        const bool    ground   = veh_type_now == 0 || veh_type_now == 1 || veh_type_now == 3;
        const bool    ready_on = rmode != 0 && !load_block && ground && (btn_layout || veh_left);
        const bool    rg       = ready_on && is_right_grip_down;
        const int64_t now_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool    gun_out  = FarCry5Stereo::Get()->game_gun_out();
        // drivereadyaway (FarCry5Drive.cpp, patch 15; author 7 Nov 20:36): only a press
        // that starts well away from the wheel or bars, and not just after letting go of them, draws; a press that starts nearer does nothing
        // until it is let go (grabbing at the wheel and missing it).
        static std::atomic<bool> rg_ok{ false };
        if (rg) {
            const bool press = !rg_was.exchange(true);
            if (press) {
                const float d  = drive.dist[fc5::drive::kRight], since = drive.since_let_go[fc5::drive::kRight];
                const float aw = fc5::drive::ready_away();
                rg_ok          = d > aw && since > 1.0f;
                if (!rg_ok.load() && rd_said.fetch_add(1) < 30)
                    spdlog::info("[Drive] driveready: the right grip pressed {:.0f} cm from the wheel or bars{}: no draw (drivereadyaway {:.0f} cm, or within 1 s "
                                 "of letting go)", d * 100.0f, since < 1e2f ? fmt::format(", {:.1f} s after the right hand let go of them", since) : std::string{},
                                 aw * 100.0f);
            }
            if (rg_ok.load() && !gun_out && now_ms - last_tap.load() >= (press ? 400 : 2500)) {
                last_tap  = now_ms;
                tap_until = now_ms + 120;
                rd_wait   = true;
                if (rd_said.fetch_add(1) < 30)
                    spdlog::info("[Drive] driveready: the right grip {} away from the wheel, no weapon in the hands: {} tapped ({})", press ? "pressed" : "still held",
                                 rmode == 2 ? "LB" : "Y", rmode == 2 ? "the game's shoot, which draws the sidearm first" : "the game's change weapon, which draws the sidearm");
            }
        } else {
            rg_was = false;
        }
        if (rd_wait.load() && gun_out) {
            rd_wait = false;
            if (rd_said.load() < 30) spdlog::info("[Drive] driveready: a weapon in the hands {} ms after the tap", now_ms - last_tap.load());
        } else if (rd_wait.load() && now_ms - last_tap.load() > 2500) {
            rd_wait = false;
            if (rd_said.load() < 30) spdlog::info("[Drive] driveready: no weapon in the hands 2.5 s after the tap");
        }
        if (now_ms < tap_until.load()) pXinputGamepad->wButtons |= rmode == 2 ? XINPUT_GAMEPAD_LEFT_SHOULDER : XINPUT_GAMEPAD_Y;

    }

    // Left grip (the modifier) + right B: RB (throw), without resting a thumb on the thumbrest.
    if (is_left_grip_down && is_right_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_Y;
        fc5::reload::b_heal_throw();   // bheal: no Y from this press once let go either
    }

    // Thumbrest touches are no buttons (author 16 Oct: a resting thumb threw or opened the wheel).

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = axis_of(left_joystick);
    const auto right_joystick_axis = axis_of(right_joystick);

    const auto true_left_joystick_axis = axis_of(m_left_joystick);
    const auto true_right_joystick_axis = axis_of(m_right_joystick);

    pXinputGamepad->sThumbLX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbLY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbRY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);


    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (is_left_grip_down) {

        float ty{0.0f};
        float tx{0.0f};
        //SHORT ThumbY{0};
        //SHORT ThumbX{0};
        // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.

        // grip_dpad_left / grip_dpad_right: that stick was centred since the grip became the modifier (above).
        const bool left_dpad  = grip_dpad_left;
        const bool right_dpad = grip_dpad_right;
        if (left_dpad) {
            ty = true_left_joystick_axis.y; // ? wants_swap
            tx = true_left_joystick_axis.x;
        }

        if (ty >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if (ty <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (tx >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if (tx <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        // The other stick is the D-pad too while the grip is held, so the left stick can keep walking:
        // grip + right stick = D-pad (the right stick neither turns nor looks meanwhile).
        const auto other = right_joystick_axis;
        if (right_dpad && (std::abs(other.x) >= 0.5f || std::abs(other.y) >= 0.5f)) {
            if (other.y >= 0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            if (other.y <= -0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (other.x >= 0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (other.x <= -0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }
        if (right_dpad) {   // the right stick is the D-pad: it does not turn meanwhile
            pXinputGamepad->sThumbRX = 0;
            pXinputGamepad->sThumbRY = 0;
        }
        if (std::abs(tx) >= 0.5f || std::abs(ty) >= 0.5f) {
            // The left stick used as the D-pad (as before): it does not walk meanwhile.
            pXinputGamepad->sThumbLY = 0;
            pXinputGamepad->sThumbLX = 0;
        }
    }


    // walkbasis (FarCry5Walk.cpp): the left stick turned so forward is where the head looks, also with a weapon in hand.
    if (load_block) std::memset(pXinputGamepad, 0, sizeof(*pXinputGamepad));   // loadpopup: nothing of the player's (the gesture's own presses next)
    FarCry5Stereo::Get()->walk_left_stick(pXinputGamepad->sThumbLX, pXinputGamepad->sThumbLY);
    // drive: the wheel's or handlebars' turn is the left stick's X, the stick's move the left stick (added to the thumbstick's).
    if (!load_block && (drive.steer_on || drive.stick_on)) {
        const float dx = drive.steer_on ? drive.steer : drive.sx, dy = drive.stick_on ? drive.sy : 0.0f;
        pXinputGamepad->sThumbLX = (int16_t)std::clamp((float)pXinputGamepad->sThumbLX + dx * 32767.0f, -32767.0f, 32767.0f);
        pXinputGamepad->sThumbLY = (int16_t)std::clamp((float)pXinputGamepad->sThumbLY + dy * 32767.0f, -32767.0f, 32767.0f);
    }
    {
        // steerstep (FarCry5SteerTune.cpp, test): a timed left stick X and RT over the controllers'; steerprobe: what the game's poll gets.
        float sx = 0.0f;
        int   srt = 0;
        if (fc5::steertune::step_value(sx, srt)) {
            pXinputGamepad->sThumbLX      = (int16_t)std::clamp(sx * 32767.0f, -32767.0f, 32767.0f);
            pXinputGamepad->bRightTrigger = (BYTE)std::max<int>(pXinputGamepad->bRightTrigger, srt);
        }
        if (!from_pad) fc5::steertune::note_poll(pXinputGamepad->sThumbLX / 32767.0f);
    }

    pXinputGamepad->wButtons |= gesture.buttons;
    {
        // Each D-pad press sent to the game, with what made it (the left grip as modifier, trigger touch, a gesture).
        static WORD dpad_was = 0;
        const WORD  dpad     = pXinputGamepad->wButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
        if (dpad != dpad_was) {
            spdlog::info("[Pad] D-pad {:x} (was {:x}): left grip modifier {} (dpadear {}, at the ear {}, two-handed hold {}, near the weapon {}, grip decided {}), gesture {:x}; sticks L ({:.2f},{:.2f}) R ({:.2f},{:.2f})",
                         dpad, dpad_was, is_left_grip_down, dpad_ear, left_at_ear, FarCry5Stereo::Get()->two_hand_held(), grip_near_weapon, FarCry5Stereo::Get()->other_grip_decided(),
                         gesture.buttons & 0xF, true_left_joystick_axis.x, true_left_joystick_axis.y, right_joystick_axis.x, right_joystick_axis.y);
            dpad_was = dpad;
        }
    }
    FarCry5Stereo::Get()->note_rb((pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0 && !menu_rb);   // menutabs: a tab change is no throw
    FarCry5Stereo::Get()->note_pad_buttons(pXinputGamepad->wButtons);   // gunstill: reload (X), sprint (left stick press)
    // mapsticky: the presses that close the map (B, Back) or open the pause menu (Start) end its hold on the flat screen.
    FarCry5Stereo::Get()->note_map_close((pXinputGamepad->wButtons & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) != 0);
    // binoczoom: D-pad up is the game's binocular toggle on foot.
    FarCry5Stereo::Get()->note_binoc_press((pXinputGamepad->wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0);

    // Motion controls: the right stick turns the mod's body frame instead of the game camera, which
    // follows the gun.
    // On the flat screen (menus, the world map) the controllers are a plain pad: sticks pan and zoom,
    // the trigger selects.
    // Driving: the throttle (RT, from the right or the left trigger, vehtriggers) is never held back for the aim.
    const bool in_vehicle = FarCry5Stereo::Get()->in_vehicle();
    // manualbow (FarCry5Bow.cpp): the string drawn by hand holds the game's fire, so its bow draws; the grip let go releases it (the shot).
    if (gesture.bow_rt) pXinputGamepad->bRightTrigger = 255;
    // Sling: a weapon hanging on the sling does not fire.
    if (gesture.slung) pXinputGamepad->bRightTrigger = 0;
    // Throwable: from the grab to the end of the throw the right trigger does not fire (the game is throwing). reloadmanual: nor with the
    // magazine out, the handle to rack or the game's reload not done (FarCry5Reload.cpp).
    if (gesture.no_fire) pXinputGamepad->bRightTrigger = 0;
    // The ammo ledger's one-shot hold (FarCry5Reload.cpp): its last real round has left, held at once, before the next clip read.
    if (fc5::reload::ledger_shot_hold()) pXinputGamepad->bRightTrigger = 0;
    const bool motion_view = FarCry5Stereo::Get()->motion_enabled() && !FarCry5Stereo::Get()->flat_screen_on();
    if (float wx = 0.0f, wy = 0.0f; FarCry5Stereo::Get()->wheel_test(wx, wy)) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        pXinputGamepad->sThumbRX = (int16_t)(wx * 32767.0f);
        pXinputGamepad->sThumbRY = (int16_t)(wy * 32767.0f);
    }
    // sidearm (sidearmpick wheel, FarCry5Gesture): the hip's pick of the wheel's rightmost slot, LB held with the right stick to the right.
    if (gesture.wheel) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    // Weapon wheel (wheelstick): LB held on foot opens it; the game picks the slot from the right stick.
    FarCry5Stereo::Get()->note_wheel(motion_view && !in_vehicle && (pXinputGamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    if (motion_view && FarCry5Stereo::Get()->wheel_open()) {
        FarCry5Stereo::Get()->set_trigger(false);
        pXinputGamepad->bRightTrigger = 0;
        FarCry5Stereo::Get()->set_turn_input(0.0f);
    } else if (motion_view) {
        // Fire waits for the game camera to reach the barrel line (see FarCry5Stereo::motion_post_present).
        FarCry5Stereo::Get()->set_trigger(!in_vehicle && pXinputGamepad->bRightTrigger > 128);
        if (!in_vehicle) pXinputGamepad->bRightTrigger = FarCry5Stereo::Get()->fire_open() ? 255 : 0;
        FarCry5Stereo::Get()->set_turn_input(float(pXinputGamepad->sThumbRX) / 32767.0f);
        // vehstick: piloting with head look the mouse no longer turns the aircraft: the right stick does (left / right only).
        bool veh_stick = FarCry5Stereo::Get()->vehicle_stick_pass();
        // stickview: in a vehicle the right stick reaches the game only in a helicopter or a plane (vehscript's type; unknown: flying).
        bool veh_air = true;
        if (in_vehicle && FarCry5Stereo::Get()->stick_view()) {
            int drv = -1, type = -1;
            veh_air = (fc5::lua::vehicle_state(drv, type) == 1 && type >= 0) ? (type == 4 || type == 5) : FarCry5Stereo::Get()->veh_flying();
            veh_stick = veh_stick && veh_air;
        }
        // scopezoomstick: on foot the right stick's up / down is not sent to the game; it zooms a scope held with two hands.
        FarCry5Stereo::Get()->note_zoom_stick(in_vehicle ? 0.0f : right_joystick_axis.x, in_vehicle ? 0.0f : right_joystick_axis.y);
        if (plane_quiet) veh_stick = false;   // driveplanestick off: nothing from the right stick in a plane
        if (!veh_stick) pXinputGamepad->sThumbRX = 0;
        // helipitch: piloting a helicopter the right stick's up / down goes to the game as its own right stick Y (its look / pitch),
        // pushed forward = nose down, past a 0.25 dead zone so a turn's sideways push does not pitch. A known other vehicle type
        // (vehscript) never gets it.
        const float ry         = float(pXinputGamepad->sThumbRY) / 32767.0f;   // 0 while the left grip makes it the D-pad
        bool        heli_pitch = FarCry5Stereo::Get()->heli_pitch_pass() && veh_air;
        if (heli_pitch) {
            int drv = -1, type = -1;
            if (fc5::lua::vehicle_state(drv, type) == 1 && type >= 0 && type != 4) heli_pitch = false;
        }
        {
            static bool pitch_was = false;
            if (heli_pitch != pitch_was) {
                spdlog::info("[Pad] helipitch: the right stick's up / down {}", heli_pitch ? "goes to the game (pushed forward = nose down)" : "not sent (not piloting a helicopter, or off)");
                pitch_was = heli_pitch;
            }
        }
        // stickspeed (default on, saved; F11 Layout > Vehicles): driving a car, a quad, a motorcycle or a boat, the right stick's up is the
        // throttle (RT) and down the brake / reverse (LT), past a 0.2 dead zone, the further the more; the triggers still work (the larger wins).
        // ry is 0 while the left grip makes the right stick the D-pad.
        {
            int        drv = -1, type = -1;
            const int  vs        = in_vehicle ? fc5::lua::vehicle_state(drv, type) : -1;
            const bool speed     = in_vehicle && !btn_layout && FarCry5Stereo::Get()->stick_speed_pass(vs == 1 ? type : -1);   // drivebuttons: A and X instead
            static bool speed_was = false;
            if (speed != speed_was) {
                spdlog::info("[Pad] stickspeed: the right stick's up / down {} (vehicle type {})", speed ? "is the throttle / brake" : "not the throttle / brake", vs == 1 ? type : -1);
                speed_was = speed;
            }
            if (speed && std::abs(ry) > 0.2f) {
                const BYTE v = (BYTE)std::clamp((std::abs(ry) - 0.2f) / 0.8f * 255.0f, 0.0f, 255.0f);
                if (ry > 0.0f) pXinputGamepad->bRightTrigger = std::max(pXinputGamepad->bRightTrigger, v);
                else pXinputGamepad->bLeftTrigger = std::max(pXinputGamepad->bLeftTrigger, v);
            }
        }
        pXinputGamepad->sThumbRY = 0;
        if (heli_pitch && std::abs(ry) > 0.25f) {
            const float p            = std::copysign((std::abs(ry) - 0.25f) / 0.75f, ry);
            pXinputGamepad->sThumbRY = (int16_t)std::clamp(-p * 32767.0f, -32767.0f, 32767.0f);
        }
        // driveheli (FarCry5Drive.h; author 7 Nov 10:59): piloting a helicopter, the right stick's turn (X) and pitch (helipitch's Y) shaped, scaled down and smoothed.
        {
            int        hdrv = -1, htype = -1;
            const bool heli_now = in_vehicle && fc5::lua::vehicle_state(hdrv, htype) == 1 && htype == 4;
            float      hx = float(pXinputGamepad->sThumbRX) / 32767.0f, hy = float(pXinputGamepad->sThumbRY) / 32767.0f;
            fc5::drive::heli_stick(heli_now, hx, hy, heli_pitch);
            if (heli_now) {
                pXinputGamepad->sThumbRX = (int16_t)std::lround(std::clamp(hx, -1.0f, 1.0f) * 32767.0f);
                pXinputGamepad->sThumbRY = (int16_t)std::lround(std::clamp(hy, -1.0f, 1.0f) * 32767.0f);
            }
        }
        // stickview: the game's camera turned by the stick is not the aircraft's attitude (planelook does not learn its seat meanwhile).
        if (in_vehicle && (std::abs(pXinputGamepad->sThumbRX) > 3000 || pXinputGamepad->sThumbRY != 0)) FarCry5Stereo::Get()->note_veh_rstick();
        if (FarCry5Stereo::Get()->stick_aim()) {
            // The game camera is steered onto the barrel / head direction through the right stick.
            pXinputGamepad->sThumbRX = (int16_t)std::clamp(FarCry5Stereo::Get()->stick_x() * 32767.0f, -32767.0f, 32767.0f);
            pXinputGamepad->sThumbRY = (int16_t)std::clamp(FarCry5Stereo::Get()->stick_y() * 32767.0f, -32767.0f, 32767.0f);
        }
    }
    // drive, stick (helicopter, plane): the hand turned about the vertical on the stick is the right stick's X (after the branches above, which zero it).
    if (!load_block && drive.twist_on && drive.twist != 0.0f && !plane_quiet) {   // driveplanestick off: not in a plane either
        pXinputGamepad->sThumbRX = (int16_t)std::clamp(drive.twist * 32767.0f, -32767.0f, 32767.0f);
        FarCry5Stereo::Get()->note_veh_rstick();
    }
    if (gesture.wheel) {   // the sidearm slot's pick: the stick set whatever the wheel setting (after the branches above, which zero it)
        pXinputGamepad->sThumbRX     = (int16_t)std::clamp(gesture.wheel_x * 32767.0f, -32767.0f, 32767.0f);
        pXinputGamepad->sThumbRY     = (int16_t)std::clamp(gesture.wheel_y * 32767.0f, -32767.0f, 32767.0f);
        pXinputGamepad->bRightTrigger = 0;
    }
    // wheelmouse (FarCry5Stereo::wheel_mouse_tick): the stick the open wheel gets, also pushed through the mouse for the game's keyboard wheel.
    if (motion_view && FarCry5Stereo::Get()->wheel_open())
        FarCry5Stereo::Get()->note_wheel_stick(float(pXinputGamepad->sThumbRX) / 32767.0f, float(pXinputGamepad->sThumbRY) / 32767.0f);
    // meleehit (FarCry5Melee.cpp): a hit presses the game's melee (after the fire gate, which would hold RT back) and pulses the weapon hand.
    {
        float melee_buzz = 0.0f;
        FarCry5Stereo::Get()->melee_pad(pXinputGamepad->wButtons, pXinputGamepad->bRightTrigger, melee_buzz);
        if (melee_buzz > 0.0f) trigger_haptic_vibration(0.0f, 0.06f, 1.0f, melee_buzz, lh ? left_joystick : right_joystick);
    }
    // drivebuttons: the throttle and brake from A and X (after the sling and throw gates above, which would hold RT back).
    // drivegunthrottle (FarCry5Drive.cpp, patch 15; author 7 Nov 20:36): with the sidearm
    // out, RT is the game's drive-and-shoot trigger, so right A sends none then, nor just after a Y or LB, nor for 1.5 s after the sidearm goes
    // away (the game drew it again on RT 1.1 s after putting it away, 14:00:57). drivegunthrottle stick (test): the left stick instead.
    {
        const int64_t now_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool    gun_now = FarCry5Stereo::Get()->game_gun_out();
        if (btn_layout && (pXinputGamepad->wButtons & (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER))) s_drive_draw_ms = now_ms;
        if (s_drive_gun_was.exchange(gun_now) && !gun_now) s_drive_gun_off_ms = now_ms;
        const bool gun_live = btn_layout && (gun_now || now_ms - s_drive_draw_ms.load() < 1000 || now_ms - s_drive_gun_off_ms.load() < 1500);
        static std::atomic<int> gt_said{ 0 };
        static std::atomic<bool> gt_was{ false };
        if (btn.rt && gun_live) {
            if (!gt_was.exchange(true) && gt_said.fetch_add(1) < 20)
                spdlog::info("[Drive] drivegunthrottle: right A held with the sidearm {}: no RT (it would shoot){}",
                             gun_now ? "out" : now_ms - s_drive_gun_off_ms.load() < 1500 ? "just put away" : "being drawn (Y or LB just sent)",
                             fc5::drive::gun_throttle() == 1 ? "; the left stick pushed forward instead (drivegunthrottle stick)" : "");
            if (fc5::drive::gun_throttle() == 1 && !load_block) pXinputGamepad->sThumbLY = 32767;
        } else {
            gt_was = false;
            if (btn.rt) pXinputGamepad->bRightTrigger = 255;
        }
    }
    if (btn.lt) pXinputGamepad->bLeftTrigger = 255;
    if (load_block) {
        // loadpopup: a swing's melee press is the player's too; the gesture's buttons and the wheel's LB stay.
        pXinputGamepad->wButtons &= gesture.buttons | (gesture.wheel ? XINPUT_GAMEPAD_LEFT_SHOULDER : 0);
        pXinputGamepad->bRightTrigger = 0;
        pXinputGamepad->bLeftTrigger  = 0;
    }
    if (from_pad) fc5::drive::echo_sent(pXinputGamepad->wButtons, pXinputGamepad->bLeftTrigger, pXinputGamepad->bRightTrigger);   // drivepadecho
    // drive (FarCry5Drive.cpp, drivelog): the pad values this poll hands over while a control is in use, once a second for each caller
    // (the virtual pad's update, the game's own poll): whether the wheel's turn reaches the game's left stick (builder 5 Nov: the car's wheel stayed straight).
    if (drive.log && drive.style != fc5::drive::kNone && (drive.held[0] || drive.held[1] || drive.steer_on || drive.stick_on || drive.twist_on)) {
        static std::atomic<int64_t> last_ms[2]{};
        const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (t - last_ms[from_pad ? 1 : 0].load() >= 1000) {
            last_ms[from_pad ? 1 : 0] = t;
            const auto& g = *pXinputGamepad;
            spdlog::info("[Drive] sent ({}): left stick ({:+.2f}, {:+.2f}), right stick X {:+.2f}, RT {} LT {} LB {} buttons {:04x}{}; from the control: {} {:+.2f}{}{}",
                         from_pad ? "virtual pad" : "game's poll", g.sThumbLX / 32767.0f, g.sThumbLY / 32767.0f, g.sThumbRX / 32767.0f, (int)g.bRightTrigger,
                         (int)g.bLeftTrigger, (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0, (unsigned)g.wButtons, load_block ? " (load block: sticks not added)" : "",
                         drive.stick_on ? "stick" : "steer", drive.stick_on ? drive.sx : drive.steer,
                         drive.stick_on ? fmt::format(", {:+.2f}", drive.sy) : std::string{}, drive.twist_on ? fmt::format(", twist {:+.2f}", drive.twist) : std::string{});
        }
    }
}
