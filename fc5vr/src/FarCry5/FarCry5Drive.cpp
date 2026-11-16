// drive (gesture driving, experimental): see FarCry5Drive.h. Author 5 Nov 17:06.
//
// Everything is worked out in the recentred reference (x right, y up, z back, origin at the head at the last recentre): the player's real body
// does not turn with the vehicle, and the game camera sits at that origin, so a control fixed there stays where the vehicle's own wheel is.
//  - A press that starts within drivegrab of the control holds it until the grip is let go (sticky), wherever the hand goes meanwhile.
//  - Wheel: each holding hand's angle round the wheel's centre, in the wheel's plane; the wheel turns by the holding hands' mean change per poll
//    (a hand within 4 cm of the centre adds nothing). Clamped to drivewheellock either way. No hand: back to straight in drivecentre seconds.
//  - Bars: the same about the vertical through the bar's centre (a hand within 6 cm of it horizontally adds nothing).
//  - Stick: each holding hand's move from where it took the stick (ref axes) and its turn about the vertical; with two hands, their mean.
// The pad poll runs on the game's polling thread and the virtual pad's: one mutex over the state.
//  - drivereact on|off (saved, default on; author 6 Nov 21:40). The wheel's
//    turn already follows the hands' angle directly (controller poses read at each pad poll, no filter); what made it feel slow is after it:
//    90 deg of wheel for full steering, and the game's own dead zone on the left stick (a small turn sent as a small stick value does nothing
//    until it passes it). On: the wheel's steering times drivereactgain (default 2: full steering at 45 deg of wheel; the drawn wheel still
//    turns as far as the hands, up to drivewheellock), and every steer past 0.015 starts at drivereactdead (default 0.20) instead of 0, so the
//    first degree of wheel already reaches the game (wheel and handlebars; the bars get the dead zone only, not the gain). drivereactlead S
//    (saved, default 0 = none, test): the steering's rate of change times S added, so the game's own steering starts sooner (try 0.03-0.06).
//    Off: as before.
//  - drivereactboost B, drivereacttau T (saved, defaults 1.5 and 0.25 s; author 7 Nov 13:54, build EE5402C6). His 14:35-14:36 drives: the steering sent follows the wheel in the same poll (turned
//    -55 deg -> +1.00, +46 -> -1.00, 17 -> -0.70; the game's poll reads it within 16-70 ms), so the delay left is the game's own: its steering
//    eases towards the stick's value instead of taking it. The mod now keeps a model of that (the game's steering following what it is sent
//    with time constant T) and sends the wheel's steering plus B times how far that model still lags it (clamped to full lock): the game's
//    steering is pushed past the target while it catches up, then sent the target itself, so a game that eases with T reaches the wheel about
//    1 + B times sooner (2.5 times with the defaults); the return to straight, letting go, too. Inferred from the code and the log, not
//    measured in the game: if the car overshoots or wobbles, lower B or T; if it is still slow, raise them. 0 turns it off (as before).
//  - drivesteercurve P|off (saved, default 1.7; steering thread, author 7 Nov 21:37, build 58CF36F2). Measured in the simulator (research\2026-11-07_steerlag, RESULTS_builder
//    and RESULTS2_builder: a compact car at 11 m/s, timed stick steps): the stick reaches the game's poll in 27-34 ms, but the car's turn rate
//    is far from proportional to the stick: 0.30 -> 0.8, 0.50 -> 10.7, 0.70 -> 26.6, 1.00 -> 64.2 deg/s, that is about
//    ((x - 0.24) / 0.76) ^ 1.7 of the full turn (the game's dead zone 0.24 = XInput's 7849, then a curve). With drivereact's 0.20 + 0.80 x a
//    tenth of the wheel's range turned the car 0.7 % of full and half of it 28 %: small turns did almost nothing, and the car only answered
//    big ones, which the game also builds up slowest (stick 0.70: t50 110 ms, t90 167 ms; 1.00: 221 and 749 ms). drivesteercurve sends
//    0.24 + 0.76 x a ^ (1 / P) instead (a = drivereact's steering after the gain, lead and boost), so the car's turn rate follows the wheel in
//    proportion: a tenth of the range turns it a tenth of full, at the game's quick small-stick response. P 1 = linear past the dead zone
//    (drivereactdead then unused); off = as before 8 Nov. Needs drivereact on.
//  - drivereadyaway M, drivegunthrottle off|stick (saved; defaults 0.30 m, off; author 7 Nov 20:36, build 58CF36F2). His 21:18-21:20 quad drive: each draw was
//    driveready's Y from a right grip pressed while both hands grabbed at the bars and the right one missed them (21:19:06, 28.5, 36.8,
//    53.4: the left grip pressed in the same 10-50 ms; 53.4 came 0.46 s after the right hand let go of the bars), and while that grip stayed
//    held Y was tapped again each time the game put the sidearm away. With the sidearm out the game's throttle (RT, right A) is its
//    drive-and-shoot trigger: the weapon pass drew 120 muzzle and particle draws in the world at 21:20:03-07, with the sidearm out and no
//    right-trigger shot. Now driveready draws only for a right grip whose press starts over M from the wheel or bars and over 1 s after
//    that hand let go of them (a press that starts nearer does nothing until let go), and right A sends no RT while the sidearm is out, while
//    a mod or pad Y or LB has just been sent (1 s), and for 1.5 s after the sidearm goes away (the game drew it again on RT 1.1 s after
//    putting it away, 14:00:57). drivegunthrottle stick (test) has A push the left stick forward then instead.
//    v2 (author 7 Nov 20:41): drivebuttons applies only in the driver's seat (vehscript's driver
//    1, or not known yet); riding (driver 0, a passenger or gun seat, where the game's RT shoots) right A is pad A (change seat). So right A
//    is RT only when driving with no sidearm about, and pad A everywhere else (jump on foot, change seat in a vehicle), which fires nothing.
//  - carshoot (14 Nov; author 07:41):
//    drivereadyaway default 0.18 m (was 0.30). Since drivehandsmodel (7 Nov 21:52, after his 12:01 "right grip readies the sidearm ...
//    correctly") the drive's wheel sits on the car's own, about 43 cm ahead of and 33 cm below the head, radius 20 cm, tilted 19 deg; a
//    30 cm keep-out from that rim covers nearly every place a seated right hand rests or points from (the lap about 14 cm from the rim, the
//    hand out to the right window about 20-25 cm, beside the thigh about 30 cm), so the right grip drew nothing. 18 cm still leaves the
//    10 cm grab plus 8 cm for a near miss.
//    drivereadykeep on|off (new, saved, default on): a right grip that draws keeps the sidearm out after it is let go (ModConfig.cpp: Y
//    tapped again 200 ms after the game puts it away), until the right hand takes the wheel or bars, the layout ends or 3 taps in a row
//    bring nothing; while it is kept, right A pushes the left stick forward (the game's throttle while shooting) instead of RT, which would
//    shoot. Off: as before (taps again only while the grip is held).
//    drivekeephold on|off (new, saved, default on; FarCry5DsHold.cpp; simulator runs 2-3 of 14 Nov: the game put a Y-drawn sidearm away
//    3.17 s after the draw whatever the hand did, so the keep re-drew it every 3.6 s): while the keep holds, the game's own drive-and-shoot
//    put-away times (fDriveAndShootAutoHolsterDuration / Classic, 1.3 s in its data) are written as 600 s; put back when it ends.
//  - drivehandsmodel (FarCry5Drive.h; author 7 Nov 21:30): set_model hands over the vehicle's own wheel, bars or stick, found from the game's
//    hands (FarCry5VehHands.cpp). While no hand holds the control and the model is fresh (0.5 s), the drive's control eases onto it (0.25 s);
//    while a hand holds it, it stays where it was (the steering is the hands' turn about it, and the car's wheel bobs with the camera). Not
//    seen for 2 s: back to the drivewheel / drivebars / drivestick place.
#include "FarCry5Drive.h"
#include "FarCry5VehicleTable.h"
#include "FarCry5DsHold.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <istream>
#include <mutex>
#include <string>

namespace fc5::drive
{
    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr float kPi = 3.14159265f;

        struct Opts
        {
            bool  on{ false };
            bool  show{ true };
            // Defaults from the builder's simulator pictures (5 Nov, 2_turned / 5_bars / 6_stick): the ring 0.32 down, 0.18 was a little low and
            // large on the compact car's wheel; the quad's grips sit about 0.22 down, 0.36 out at 0.45 ahead; the helicopter's stick top about as
            // far down as ahead (0.50 down was below the view).
            float wheel_fwd{ 0.38f }, wheel_down{ 0.29f }, wheel_r{ 0.16f }, wheel_tilt{ 25.0f };   // m, m, m, deg (the face's tilt up towards the driver)
            float wheel_lock{ 90.0f };                                                              // deg of wheel for full lock
            float bars_fwd{ 0.45f }, bars_down{ 0.22f }, bars_w{ 0.36f };                           // m (half width: centre to each grip)
            float bars_lock{ 30.0f };
            float stick_fwd{ 0.40f }, stick_down{ 0.38f };
            // m of hand move for full stick. 12 cm since 12 Nov (was 8): in the author's 20:53 helicopter flight his hand was 8 to 16 cm from where
            // it took the stick in 8 of the 14 readings, so the stick was at its end most of the time ("not very precise").
            float stick_range{ 0.12f };
            bool  twist{ true };
            float twist_deg{ 40.0f };
            // drivestickfine (author 12 Nov 19:57). The hand's move went to the left stick as it was: the game's own dead zone (an Xbox 360 pad's: 0.24 on
            // the left stick, 0.265 on the right, full at 0.94) then swallowed the first quarter of it, so with the drive's own 12 % the
            // first 2.6 of the 8 cm did nothing and the last 5.4 cm were the whole range; the hand's turn did nothing up to 16 of its 40 deg
            // and then turned at the game's full rate, not driveheli's. On: what the stick sends starts just past the game's dead zone and
            // is full at 0.94 (taken across the two axes together, so a small move aside while pushing forward does not jump); in a
            // helicopter the hand's turn has driveheli's rate and curve, smoothed over half its time. drivestickcurve: the left stick as
            // |x|^curve (1: in proportion).
            bool  stick_fine{ true };
            float stick_curve{ 1.0f };
            // drivestickdead: the game's dead zone the left stick starts past. 0.24 (a car's steering) was assumed for the helicopter too and
            // is wrong there: on CF345D2A (13 Nov 00:59, testruns\2026-11-13_vehround8) the helicopter's own stick leant 3 deg in the frame
            // a left stick X of 0.15 began, after 22 s still, and 6 and 9 deg in the next two seconds. So the game answers at 0.15 or less,
            // and a stick that starts at 0.24 starts with a jump. 0.05 until the dead zone is measured (a smaller step series); full at 1.
            float stick_dead{ 0.05f };
            float grab{ 0.10f };
            float centre_s{ 0.30f };
            bool  react{ true };          // drivereact (see the top)
            float react_gain{ 2.0f };     // drivereactgain: the wheel's steering times this
            float react_dead{ 0.20f };    // drivereactdead: the game's left-stick dead zone, skipped
            float react_lead{ 0.0f };     // drivereactlead: s of the steering's rate added (test)
            float react_boost{ 1.5f };    // drivereactboost: times the modelled game steering's lag added (0: off)
            float react_tau{ 0.25f };     // drivereacttau: s, the modelled game steering's time constant
            float steer_curve{ 1.7f };    // drivesteercurve: the game's measured stick-to-turn exponent undone (0: off)
            bool  right_brake{ true };
            bool  model_steer{ true };    // drivemodelsteer (FarCry5Drive.h)
            float wheel_ratio{ 1.0f };    // drivewheelratio
            bool  model_show{ true };     // drivemodelshow
            bool  log{ true };
            // Vehicle controls (FarCry5Drive.h), not tied to drive on|off.
            bool  buttons{ true };
            int   dbl_ms{ 350 };
            bool  plane_stick{ false };
            int   hands{ 1 };
            HandOpts hand{};
            int   ready{ 1 };             // driveready: 0 off, 1 y, 2 lb
            float ready_away{ 0.18f };    // drivereadyaway: m from the wheel or bars a right grip must start for driveready (0.30 until 14 Nov)
            bool  ready_keep{ true };     // drivereadykeep: a sidearm the right grip drew stays out until the right hand takes the wheel
            int   gun_throttle{ 2 };      // drivegunthrottle: 0 off (A sends nothing while the sidearm is out), 1 stick (test), 2 rt (gunthrottle2)
            float quad_seat{ 0.15f };     // drivequadseat: m from the centre line (0: off)
            bool  heli{ true };           // driveheli
            float heli_gain{ 0.40f };     // driveheligain: the game's full rate times this at full stick
            float heli_curve{ 2.0f };     // drivehelicurve: |x|^curve
            float heli_smooth{ 0.30f };   // drivehelismooth: s
            int   menu{ 2 };              // drivemenu: 0 off, 1 vehicle, 2 all (default; author 7 Nov 12:15)
            int   menu_hold{ 500 };       // drivemenuhold: ms
        };

        std::mutex g_mtx;
        Opts       g_o{};
        int        g_force{ -1 };   // drivestyle: -1 auto, else a Style for the vehicle you are in

        // State (under g_mtx).
        int         g_style = kNone;
        const char* g_why   = "off";
        uint64_t    g_veh   = 0;
        bool        g_held[2]{}, g_grip_was[2]{}, g_near[2]{};
        clock::time_point g_let_go[2]{};      // drivereadyaway: when each hand last let go of the control
        float       g_hand_ang[2]{};          // wheel, bars: each holding hand's angle at the last poll (rad)
        bool        g_hand_ang_ok[2]{};
        glm::vec3   g_grab_pos[2]{};          // stick: where each hand took it (ref)
        float       g_grab_yaw[2]{};          // stick: its turn then (rad)
        float       g_last_yaw[2]{};
        float       g_angle = 0.0f;           // wheel / bars: the control's turn (rad, positive = to the left)
        float       g_react_prev = 0.0f;      // drivereact: the last poll's steering before the lead and the dead zone
        float       g_react_game = 0.0f;      // drivereactboost: the modelled game steering (before the dead zone)
        glm::vec3   g_defl{ 0.0f };           // stick: the mean move (ref)
        float       g_twist = 0.0f;
        Output      g_out{};
        View        g_view{};
        bool        g_view_ok = false;
        glm::mat4   g_view_ref_inv{ 1.0f };   // model_point: tracking space -> the recentred reference, at the last view
        clock::time_point g_last{}, g_log_t{}, g_off_since{}, g_unknown_since{};
        bool        g_was_driving = false;
        int         g_polls = 0, g_holds = 0;
        float       g_steer_max = 0.0f;
        // drivebuttons (under g_mtx; both pad pollers call buttons_poll: only edges change the state, so a second poll of the same press does nothing)
        bool              g_btn_active = false, g_btn_a_wait = false, g_btn_x_wait = false;
        bool              g_btn_x_was = false, g_btn_hb = false, g_btn_tap = false;
        clock::time_point g_btn_x_down{}, g_btn_x_up{};
        int               g_btn_hb_n = 0, g_btn_brake_n = 0, g_btn_type = -1;
        std::atomic<int64_t> g_trig_test_until{ 0 };   // drivetrigtest (the simulator's controllers have no trigger): ms on the steady clock
        int               g_btn_test = 0, g_btn_test_ms = 0;   // drivebuttontest (the simulator's controllers have no A or X): 1 a, 2 x, 3 xx
        clock::time_point g_btn_test_t0{};
        // driveheli (under g_mtx): the smoothed right stick (game-relative, before the dead zone is added)
        float             g_hs[2]{};
        clock::time_point g_hs_t{};
        bool              g_hs_on = false;
        int               g_hs_said = 0;
        clock::time_point g_seat_wait{};   // drivequadseat: when a type 0 vehicle started waiting for its seat
        // drivehandsmodel (under g_mtx): the vehicle's own control from the game's hands (set_model), and the place the drive uses now.
        Model             g_model{};
        clock::time_point g_model_t{};
        struct Cur
        {
            bool      on{ false };
            int       style{ kNone };
            glm::vec3 centre{ 0.0f };
            glm::vec3 n{ 0.0f, 0.0f, 1.0f };
            float     radius{ 0.0f };
        };
        Cur               g_cur{};
        // Each holding hand's own control, as it was when the hand took it (ref): the hand's angle is read about it. The control drawn and
        // taken follows the vehicle's own model (Model::bone), which moves in the reference when the game's camera moves in the cabin (10 cm
        // and more in hard turns, 12 Nov); a hand's angle read about that moving centre would steer by itself (a hand at 2 o'clock, the
        // centre 10 cm aside: 26 deg).
        struct Anchor
        {
            glm::vec3 c{ 0.0f }, u{ 1, 0, 0 }, v{ 0, 1, 0 }, n{ 0, 0, 1 };
            float     r{ 0.0f };
        };
        Anchor            g_anchor[2]{};
        float             g_tw_s = 0.0f;       // drivestickfine: the hand's turn, shaped and smoothed (the game's rate, before its dead zone)
        bool              g_st_full = false, g_st_centre = true;   // drivestickfine: the stick at its end / at its centre (the hand's buzz at each)
        int               g_fine_lines = 0;
        clock::time_point g_fine_log_t{};
        float             g_stray_max = 0.0f;  // the farthest the followed control was from a holding hand's own (m), this drive
        clock::time_point g_stray_log_t{};
        int               g_cur_lines = 0;     // log lines this drive
        glm::vec3         g_cur_logged{ 0.0f };
        clock::time_point g_cur_log_t{};
        clock::time_point g_scan_until{};      // drivewheelscan
        // drivemodelsteer (under g_mtx): the vehicle's own wheel's / bars' turn (Model.turn) against the steering sent.
        float             g_mt_zero = 0.0f, g_mt_last = 0.0f, g_mt_sign = 1.0f, g_mt_lock = 0.0f, g_mt_cmd = 0.0f;
        bool              g_mt_zero_ok = false, g_mt_dead = false;
        clock::time_point g_mt_still{}, g_mt_cmd_t{}, g_mt_log_t{};
        int               g_mt_opp = 0, g_mt_none = 0, g_mt_obs = 0;
        bool              g_ms_on = false;     // drivemodelshow: the vehicle's own wheel drawn at g_ms_turn (Model.turn's units)
        float             g_ms_turn = 0.0f;
        clock::time_point g_ms_t{};
        int               g_hint = kNone;      // set_shape_hint
        clock::time_point g_hint_t{};
        uint64_t          g_hint_veh = 0;      // the vehicle it was given for (kept while that vehicle is driven)
        // padecho (own lock: the pad pollers only)
        std::mutex        g_echo_mtx;
        bool              g_echo_on = false;
        uint32_t          g_echo_was = 0;
        clock::time_point g_echo_last[18]{}, g_echo_start[18]{};   // the 16 buttons, LT, RT: last poll sent pressed, start of that press
        bool              g_echo_held[18]{};
        int               g_echo_presses = 0, g_echo_after = 0;
        const char*       echo_pad_name(int i)
        {
            static const char* n[18]{ "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "Back", "left stick press", "right stick press",
                                      "LB", "RB", "?", "?", "A", "B", "X", "Y", "LT", "RT" };
            return i >= 0 && i < 18 ? n[i] : "?";
        }

        float wrap(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }
        const char* style_name(int s) { return s == kWheel ? "wheel" : s == kBars ? "handlebars" : s == kStick ? "stick" : "none"; }
        const char* side_name(int h) { return h == kRight ? "right" : "left"; }

        // The control for this vehicle: the game's type first for aircraft and motorcycles, then a spawned vehicle's archetype (quads, jetskis), then the type.
        int auto_style(const Input& in, const char*& why, clock::time_point now)
        {
            // Type and archetype not known (the game's answer can lag a few seconds after a seat; builder 5 Nov: the quad showed the wheel
            // for 6 s before its bars): keep the control already chosen in this drive, else nothing for 8 s, then the wheel (vehscript off).
            if (in.veh_type < 0 && in.archetype == 0) {
                if (g_style != kNone) { why = g_why; return g_style; }
                if (g_unknown_since == clock::time_point{}) g_unknown_since = now;
                if (g_hint != kNone && now - g_hint_t < std::chrono::seconds(2)) { why = "the vehicle's own model shows it (vehcontrol), before the game's type"; return g_hint; }
                if (now - g_unknown_since < std::chrono::seconds(8)) { why = "type not known yet: waiting for the game's answer"; return kNone; }
                why = "type not known for 8 s: wheel";
                return kWheel;
            }
            g_unknown_since = clock::time_point{};
            if (in.veh_type == 4) { why = "helicopter (the game's type)"; return kStick; }
            if (in.veh_type == 5) { why = "plane (the game's type)"; return kStick; }
            if (in.veh_type == 3) { why = "motorcycle (the game's type)"; return kBars; }
            if (in.veh_type == 2 || in.veh_type == 6) { why = "paraglider or wingsuit: nothing to hold"; return kNone; }
            if (in.archetype != 0) {
                for (const auto& e : fc5::spawn::kVehicles) {
                    if (e.archetype != in.archetype) continue;
                    const std::string ent = e.entity;
                    if (ent.find("ATVs") != std::string::npos || ent.find("Quad") != std::string::npos) { why = "quad (spawned from F11)"; return kBars; }
                    if (ent.find("Jetski") != std::string::npos) { why = "jetski (spawned from F11)"; return kBars; }
                    if (ent.find("Vehicles:Air:Heli") != std::string::npos || std::strcmp(e.kind, "Helicopters") == 0) { why = "helicopter (spawned from F11)"; return kStick; }
                    if (std::strcmp(e.kind, "Planes") == 0) { why = "plane (spawned from F11)"; return kStick; }
                    break;
                }
            }
            if (in.veh_type == 1) {
                // jetskibars: the game calls a jetski a boat, and only one spawned from F11 has its archetype. vehcontrol finds the
                // handlebars in the vehicle's own model when no wheel is there and tells the drive (author 16 Nov; his 10:28 drive on a jetski found in the world: "wheel: boat (the game's type)", no wheel found in 3 tries).
                if (g_hint == kBars && g_hint_veh == in.veh_id) { why = "boat; handlebars in the vehicle's own model (vehcontrol): a jetski"; return kBars; }
                why = "boat (the game's type)";
                return kWheel;
            }
            if (in.veh_type == 0) {
                // drivequadseat: the game calls a quad wheeled like a car; its driver sits on the vehicle's centre line, a car's 0.4 m to the side.
                // vehcontrol: the vehicle's own model shows a wheel (the tractor: its driver on the centre line, as a quad's), or the game's
                // hands hold handlebars (a quad whose seat was not learnt).
                if (g_hint == kWheel && g_hint_veh == in.veh_id) { why = "wheeled; a wheel in the vehicle's own model (vehcontrol)"; return kWheel; }
                if (g_hint == kBars && g_hint_veh == in.veh_id) { why = "wheeled; the game's hands hold handlebars (vehcontrol)"; return kBars; }
                if (g_o.quad_seat > 0.0f) {
                    if (std::abs(in.seat_side) > 50.0f) {
                        if (g_style != kNone) { why = g_why; return g_style; }
                        if (g_seat_wait == clock::time_point{}) g_seat_wait = now;
                        if (g_hint != kNone && now - g_hint_t < std::chrono::seconds(2)) { why = "the vehicle's own model shows it (vehcontrol), before the seat is known"; return g_hint; }
                        if (now - g_seat_wait < std::chrono::seconds(3)) { why = "wheeled: waiting for the seat (a quad or a car)"; return kNone; }
                        why = "wheeled (the game's type; the seat not learnt in 3 s)";
                        return kWheel;
                    }
                    g_seat_wait = clock::time_point{};
                    if (std::abs(in.seat_side) <= g_o.quad_seat) { why = "quad (wheeled, the driver on the vehicle's centre line)"; return kBars; }
                    why = "wheeled (the game's type; the driver to the side: a car)";
                    return kWheel;
                }
                why = "wheeled (the game's type)";
                return kWheel;
            }
            why = "type not known: wheel";
            return kWheel;
        }

        void drop_all(const char* why_drop)
        {
            for (int h = 0; h < 2; ++h) {
                if (g_held[h] && g_o.log) spdlog::info("[Drive] {} hand off the {} ({})", side_name(h), style_name(g_style), why_drop);
                g_held[h]        = false;
                g_hand_ang_ok[h] = false;
            }
        }
    }

    Output update(const Input& in)
    {
        std::lock_guard _{ g_mtx };
        const auto now = clock::now();
        float      dt  = g_last.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - g_last).count();
        g_last         = now;
        dt             = std::clamp(dt, 0.0f, 0.1f);
        Output out{};
        ++g_polls;
        for (int h = 0; h < 2; ++h)
            if (g_let_go[h] != clock::time_point{}) out.since_let_go[h] = std::chrono::duration<float>(now - g_let_go[h]).count();

        // Leaving the vehicle (3 s off driving, so a seat change's settle keeps the choice) forgets drivestyle.
        if (in.driving) {
            g_off_since = now;
        } else if (g_force >= 0 && now - g_off_since > std::chrono::seconds(3)) {
            g_force = -1;
            spdlog::info("[Drive] drivestyle back to auto (out of the vehicle)");
        }
        if (in.driving && in.veh_id != 0 && g_veh != 0 && in.veh_id != g_veh && g_force >= 0) {
            g_force = -1;
            spdlog::info("[Drive] drivestyle back to auto (another vehicle)");
        }
        if (in.veh_id != 0) g_veh = in.veh_id;
        if (!in.driving && now - g_off_since > std::chrono::seconds(3)) g_hint = kNone;   // set_shape_hint: for the vehicle it was given in

        const char* why   = "off";
        int         style = kNone;
        if (!g_o.on) {
            why = "drive off";
        } else if (!in.driving) {
            why             = "not driving";
            g_unknown_since = clock::time_point{};
            g_seat_wait     = clock::time_point{};
        } else if (!in.ref_ok) {
            why = "no recentred reference yet";
        } else if (g_force >= 0) {
            style = g_force;
            why   = "drivestyle (F11)";
        } else {
            style = auto_style(in, why, now);
        }
        if (style != g_style || (g_why != why && style != kNone)) {
            if (style != g_style) {
                drop_all("the control changed");
                g_cur       = Cur{};   // drivehandsmodel: found again for this control
                g_cur_lines = 0;
                g_stray_max = 0.0f;
                g_mt_zero_ok = g_mt_dead = false;   // drivemodelsteer: learnt again for this control
                g_mt_sign    = 1.0f;
                g_mt_opp = g_mt_none = g_mt_obs = 0;
                g_angle = 0.0f;
                g_react_prev = 0.0f;
                g_react_game = 0.0f;
                g_defl  = glm::vec3{ 0.0f };
                g_twist = 0.0f;
                if (g_o.log) {
                    if (style != kNone)
                        spdlog::info("[Drive] {}: {}{}", style_name(style), why, g_style == kNone ? " (taken with a grip near it; the left trigger is the throttle)" : "");
                    else if (g_style != kNone)
                        spdlog::info("[Drive] no control ({}){}", why, g_holds ? fmt::format("; {} holds this time, steering up to {:.2f}", g_holds, g_steer_max) : std::string{});
                }
                if (style == kNone) {
                    g_holds     = 0;
                    g_steer_max = 0.0f;
                }
            }
            g_style = style;
            g_why   = why;
        }
        g_was_driving = in.driving;
        if (style == kNone) {
            for (int h = 0; h < 2; ++h) g_grip_was[h] = in.hand[h].grip;
            g_view_ok = false;
            g_out     = out;
            return out;
        }
        out.style = style;

        const glm::mat4 inv  = glm::inverse(in.ref);
        const glm::mat3 invr = glm::mat3{ inv };
        glm::vec3       hp[2];
        glm::mat3       hr[2];
        for (int h = 0; h < 2; ++h) {
            hp[h] = glm::vec3{ inv * glm::vec4{ in.hand[h].pos, 1.0f } };
            hr[h] = invr * in.hand[h].rot;
        }
        const Opts& o = g_o;
        View        vw{};
        vw.style = style;
        vw.grab  = o.grab;

        // The control's place and axes (ref).
        glm::vec3 C{ 0.0f }, u{ 1, 0, 0 }, v{ 0, 1, 0 }, n{ 0, 0, 1 };
        if (style == kWheel) {
            C               = glm::vec3{ 0.0f, -o.wheel_down, -o.wheel_fwd };
            const float t   = glm::radians(o.wheel_tilt);
            n               = glm::vec3{ 0.0f, std::sin(t), std::cos(t) };    // towards the driver, tilted up
            v               = glm::vec3{ 0.0f, std::cos(t), -std::sin(t) };   // the wheel's top leans forward
            u               = glm::vec3{ 1.0f, 0.0f, 0.0f };                  // u x v = n
        } else if (style == kBars) {
            C = glm::vec3{ 0.0f, -o.bars_down, -o.bars_fwd };
        } else {
            C = glm::vec3{ 0.0f, -o.stick_down, -o.stick_fwd };
        }
        float wheel_r = wheel_r, bars_w = bars_w;
        // drivehandsmodel: the vehicle's own control (set_model). From the game's hands: eased onto while no hand holds it, kept while one
        // does. From the vehicle's own bone (vehcontrol): on it at once, and with it while a hand holds it too (each holding hand's angle is
        // read about its own anchor, below).
        if (o.hand.model && o.hand.grab && o.hands == 1) {
            const bool held_now = g_held[0] || g_held[1];
            const bool fresh    = g_model.style == style && now - g_model_t < std::chrono::milliseconds(500);
            const bool live     = fresh && g_model.bone && style != kStick;   // (a stick's top moves with the hand that pushes it: kept while held)
            if (fresh && (!held_now || live)) {
                const bool first = !g_cur.on || g_cur.style != style;
                const float a    = first || g_model.bone ? 1.0f : std::min(1.0f, dt / 0.25f);
                g_cur.centre     = glm::mix(g_cur.centre, g_model.centre, a);
                g_cur.radius     = glm::mix(g_cur.radius, g_model.radius, a);
                // drivefacefix (15 Nov, tester's fast travel in a car): the wheel's face points towards the driver's head. Before, the sign was
                // kept from frame to frame only; after a map fast travel the wheel was first placed while the game's aim still pointed 176 deg
                // off the car's front (16:56:22: centre +46 cm behind the head), and when vehheadingkeep turned the aim back, the face kept the
                // reversed sign: tilted -20 deg instead of +20, the hand's angle read about the reversed axis, steering backwards for the rest
                // of the drive. A wheel's face always looks at the driver, so the sign is taken from the head's side of the wheel.
                glm::vec3 mn     = g_model.n;
                if (style == kWheel && glm::dot(mn, -g_model.centre) < 0.0f) mn = -mn;
                if (style == kWheel && !first && glm::dot(g_cur.n, mn) < 0.0f && o.log)
                    spdlog::info("[Drive] drivefacefix: the {}'s face turned back towards you (it had kept a reversed side, tilted {:.0f} deg; now {:.0f} deg)", style_name(style),
                                 glm::degrees(std::asin(std::clamp(g_cur.n.y, -1.0f, 1.0f))), glm::degrees(std::asin(std::clamp(mn.y, -1.0f, 1.0f))));
                glm::vec3 nn     = first || (style == kWheel && glm::dot(g_cur.n, mn) < 0.0f) ? mn
                                                                                              : glm::mix(g_cur.n, glm::dot(g_cur.n, mn) < 0.0f ? -mn : mn, a);
                g_cur.n          = glm::length(nn) > 1e-3f ? glm::normalize(nn) : glm::vec3{ 0.0f, 0.0f, 1.0f };
                g_cur.style      = style;
                // Logged when placed, then every 5 s while it is still moving over 3 cm (12 lines a drive at most).
                const bool moved = glm::length(g_cur.centre - g_cur_logged) > 0.03f;
                if (o.log && (first || (moved && now - g_cur_log_t > std::chrono::seconds(5) && g_cur_lines < 12))) {
                    g_cur_log_t  = now;
                    g_cur_logged = g_cur.centre;
                    g_cur_lines++;
                    std::string what = " (the game's hand on it)";
                    if (style == kWheel)
                        what = fmt::format(", radius {:.0f} cm, its face tilted {:.0f} deg up towards you (axis {})", g_cur.radius * 100.0f,
                                           glm::degrees(std::asin(std::clamp(g_cur.n.y, -1.0f, 1.0f))),
                                           g_model.axis_learnt ? "from the game hand's own turn" : "from the hands' line and your eyes");
                    else if (style == kBars)
                        what = fmt::format(", half width {:.0f} cm", g_cur.radius * 100.0f);
                    spdlog::info("[Drive] drivehandsmodel: the {} {} the vehicle's own, from the game's hands: at ({:+.0f}, {:+.0f}, {:+.0f}) cm from the recentred head "
                                 "(right, up, back), {:.0f} cm from the drive's default place{}",
                                 style_name(style), first ? "placed on" : "moved with", g_cur.centre.x * 100.0f, g_cur.centre.y * 100.0f, g_cur.centre.z * 100.0f,
                                 glm::length(g_cur.centre - C) * 100.0f, what);
                }
                g_cur.on = true;
            } else if (!held_now && g_cur.on && now - g_model_t > std::chrono::seconds(2)) {
                g_cur.on = false;
                if (o.log) spdlog::info("[Drive] drivehandsmodel: the game's hands not seen for 2 s: the {} back at its default place", style_name(style));
            }
            if (g_cur.on && g_cur.style == style) {
                C = g_cur.centre;
                if (style == kWheel) {
                    n                 = g_cur.n;
                    const glm::vec3 x = glm::vec3{ 1.0f, 0.0f, 0.0f } - n * n.x;
                    u                 = glm::length(x) > 1e-3f ? glm::normalize(x) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                    v                 = glm::cross(n, u);   // u x v = n
                    wheel_r           = g_cur.radius;
                } else if (style == kBars) {
                    bars_w = g_cur.radius;
                }
            }
        } else {
            g_cur.on = false;
        }

        // Each hand's distance to the control (grab), and its angle about it (wheel, bars). A holding hand: about the control as it was when
        // the hand took it (g_anchor); a free hand: about the control where it is now.
        auto reach = [&](int h, float& ang, bool& ang_ok) {
            const bool      own = g_held[h];
            const glm::vec3 p   = hp[h] - (own ? g_anchor[h].c : C);
            ang_ok              = false;
            if (style == kWheel) {
                const glm::vec3 &au = own ? g_anchor[h].u : u, &av = own ? g_anchor[h].v : v, &an = own ? g_anchor[h].n : n;
                const float     ar  = own ? g_anchor[h].r : wheel_r;
                const float     hgt = glm::dot(p, an);
                const glm::vec3 q   = p - an * hgt;
                const float     r   = glm::length(q);
                ang                 = std::atan2(glm::dot(q, av), glm::dot(q, au));
                ang_ok              = r > 0.04f;
                return std::sqrt((r - ar) * (r - ar) + hgt * hgt);
            }
            if (style == kBars) {
                const float     bw = own ? g_anchor[h].r : bars_w;
                const glm::vec3 e{ bw * std::cos(g_angle), 0.0f, -bw * std::sin(g_angle) };   // the right end, the bar turned by g_angle about +y
                const glm::vec3 d = e * 2.0f;
                const float     s = std::clamp(glm::dot(p + e, d) / glm::dot(d, d), 0.0f, 1.0f);
                const float     r = std::sqrt(p.x * p.x + p.z * p.z);
                ang               = std::atan2(-p.z, p.x);
                ang_ok            = r > 0.06f;
                return glm::length(p - (-e + d * s));
            }
            return glm::length(p);   // stick: its top
        };
        auto hand_yaw = [&](int h, float& yaw) {
            const glm::vec3 f = -hr[h][2];
            const float     l = std::sqrt(f.x * f.x + f.z * f.z);
            if (l < 0.3f) return false;   // pointing up or down: the turn about the vertical is not read
            yaw = std::atan2(-f.x, -f.z);
            return true;
        };

        float delta_sum = 0.0f;
        int   delta_n   = 0;
        bool  took_alone = false;   // drivemodelsteer: a hand took the control this poll and no other holds it
        for (int h = 0; h < 2; ++h) {
            const bool grip  = in.hand[h].grip && in.hand[h].tracked;
            const bool press = grip && !g_grip_was[h];
            g_grip_was[h]    = in.hand[h].grip;
            float      ang   = 0.0f;
            bool       ang_ok = false;
            const float d    = in.hand[h].tracked ? reach(h, ang, ang_ok) : 1e3f;
            out.dist[h]      = d;
            g_near[h]        = !g_held[h] && d <= o.grab;
            if (g_held[h] && !grip) {
                g_held[h] = false;
                g_let_go[h] = now;
                out.buzz[h] = 0.25f;
                if (o.log) spdlog::info("[Drive] {} hand let go of the {}{}", side_name(h), style_name(style),
                                        style == kStick ? std::string{} : fmt::format(" (turned {:.0f} deg)", glm::degrees(g_angle)));
                continue;
            }
            if (!g_held[h] && press && d <= o.grab) {
                g_held[h]        = true;
                g_anchor[h]      = Anchor{ C, u, v, n, style == kWheel ? wheel_r : bars_w };
                took_alone       = took_alone || !g_held[1 - h];
                g_hand_ang[h]    = ang;
                g_hand_ang_ok[h] = ang_ok;
                g_grab_pos[h]    = hp[h];
                float yaw        = 0.0f;
                g_grab_yaw[h]    = hand_yaw(h, yaw) ? yaw : 0.0f;
                g_last_yaw[h]    = g_grab_yaw[h];
                out.buzz[h]      = 0.6f;
                ++g_holds;
                if (o.log) spdlog::info("[Drive] {} hand took the {} ({:.0f} cm from it)", side_name(h), style_name(style), d * 100.0f);
                continue;
            }
            if (!g_held[h]) continue;
            if (style == kWheel || style == kBars) {
                if (ang_ok && g_hand_ang_ok[h]) {
                    delta_sum += wrap(ang - g_hand_ang[h]);
                    ++delta_n;
                }
                g_hand_ang[h]    = ang;
                g_hand_ang_ok[h] = ang_ok;
            }
        }
        for (int h = 0; h < 2; ++h) {
            out.held[h]       = g_held[h];
            out.grip_taken[h] = g_held[h];
        }
        const bool any = g_held[0] || g_held[1];
        // How far the followed control is from each holding hand's own (the cabin moving in the reference): a line when it passes 5 cm more
        // than the drive's farthest so far, at most every 5 s.
        for (int h = 0; h < 2 && o.log; ++h) {
            if (!g_held[h] || style == kStick) continue;
            const float dc = glm::length(C - g_anchor[h].c);
            if (dc > g_stray_max + 0.05f && now - g_stray_log_t > std::chrono::seconds(5)) {
                g_stray_max   = dc;
                g_stray_log_t = now;
                spdlog::info("[Drive] drivehandsmodel: the vehicle's own {} is {:.0f} cm ({:+.0f}, {:+.0f}, {:+.0f}; right, up, back) from where the {} hand took it, its face "
                             "turned {:.0f} deg: the {} drawn and taken follows it, the hand's angle stays read about where it took it",
                             style_name(style), dc * 100.0f, (C.x - g_anchor[h].c.x) * 100.0f, (C.y - g_anchor[h].c.y) * 100.0f, (C.z - g_anchor[h].c.z) * 100.0f, side_name(h),
                             glm::degrees(std::acos(std::clamp(glm::dot(n, g_anchor[h].n), -1.0f, 1.0f))), style_name(style));
            }
        }

        if (style == kWheel || style == kBars) {
            // drivemodelsteer: the vehicle's own wheel's turn from straight (theta, rad, positive: to the left), while vehcontrol follows it.
            bool  mt    = false;
            float theta = 0.0f;
            if (o.model_steer && !g_mt_dead && o.hand.model && o.hand.grab && o.hands == 1 && g_model.style == style && g_model.turn_ok &&
                now - g_model_t < std::chrono::milliseconds(500)) {
                const float t = g_model.turn;
                if (!g_mt_zero_ok) {
                    g_mt_zero_ok = true;
                    g_mt_zero = g_mt_last = t;
                    g_mt_still   = now;
                    g_mt_lock    = glm::radians(style == kWheel ? 143.0f : 25.0f);   // the cars' and the jetski's bones at full lock (9 Nov dumps), until seen
                    g_mt_cmd     = 0.0f;
                    g_mt_cmd_t   = now;
                    g_ms_turn    = t;
                }
                if (std::abs(t - g_mt_last) > glm::radians(0.5f)) {
                    g_mt_last  = t;
                    g_mt_still = now;
                }
                // Straight: nobody holds it, the drive's own wheel is back at the centre and the model's has not moved for 0.75 s.
                if (!any && g_angle == 0.0f && now - g_mt_still > std::chrono::milliseconds(750)) g_mt_zero = t;
                theta = g_mt_sign * (t - g_mt_zero);
                mt    = true;
            }
            const float ratio = style == kWheel ? std::clamp(o.wheel_ratio, 0.5f, 4.0f) : 1.0f;
            const float lock  = mt ? g_mt_lock / ratio : glm::radians(std::max(5.0f, style == kWheel ? o.wheel_lock : o.bars_lock));
            // Taken where the vehicle's own wheel is now (turned by the stick, or still coming back): no jump to straight.
            if (mt && took_alone) g_angle = std::clamp(theta / ratio, -lock, lock);
            if (any) {
                if (delta_n > 0) g_angle = std::clamp(g_angle + delta_sum / float(delta_n), -lock, lock);
            } else if (o.centre_s <= 0.0f) {
                g_angle = 0.0f;
            } else {
                g_angle -= g_angle * std::min(1.0f, dt * 3.0f / o.centre_s);   // ~95 % back in centre_s
                if (std::abs(g_angle) < glm::radians(0.3f)) g_angle = 0.0f;
            }
            out.steer_on    = any || g_angle != 0.0f;
            out.steer       = std::clamp(-g_angle / lock, -1.0f, 1.0f);   // turned to the left (counter-clockwise from the driver) = steer left
            // drivemodelshow: held, the vehicle's own wheel is drawn at the hand's angle; let go, it eases back onto the game's own.
            if (mt) {
                const float t = g_model.turn;
                if (now - g_ms_t > std::chrono::milliseconds(300)) g_ms_turn = t;
                if (any) g_ms_turn = g_mt_zero + g_mt_sign * std::clamp(g_angle * ratio, -g_mt_lock, g_mt_lock);
                else g_ms_turn = t + (g_ms_turn - t) * std::exp(-dt / 0.10f);
                g_ms_on = o.model_show && (any || std::abs(g_ms_turn - t) > glm::radians(0.3f));
                g_ms_t  = now;
            } else {
                g_ms_on = false;
            }
            if (o.react) {
                // drivereact: more steering per degree of wheel, a lead on its change, and the game's dead zone skipped.
                float s = std::clamp(-g_angle / lock * (style == kWheel && !mt ? o.react_gain : 1.0f), -1.0f, 1.0f);
                const float rate = dt > 1e-4f ? (s - g_react_prev) / dt : 0.0f;
                g_react_prev     = s;
                if (o.react_lead > 0.0f) s = std::clamp(s + o.react_lead * rate, -1.0f, 1.0f);
                // drivereactboost (see the top): the wheel's steering plus B times the modelled game steering's lag behind it.
                if (mt && any) {
                    // drivemodelsteer: the lag measured on the vehicle's own wheel instead (B at most 1.5: its turn comes a few frames late).
                    const float meas = std::clamp(-theta / g_mt_lock, -1.5f, 1.5f);
                    const float us    = std::clamp(s + std::min(o.react_boost, 1.5f) * (s - meas), -1.0f, 1.0f);
                    g_react_game     = std::clamp(meas, -1.0f, 1.0f);
                    // The model's full lock, from what it shows for a steady command (0.4 s, its wheel still for 0.3 s): its turn itself when
                    // the full stick is sent, else its turn per unit sent. Turning the other way twice: its sign is the other one. Not turning
                    // three times: not usable this drive.
                    if (std::abs(s - g_mt_cmd) > 0.05f) {
                        g_mt_cmd   = s;
                        g_mt_cmd_t = now;
                    } else if (std::abs(s) >= 0.35f && now - g_mt_cmd_t > std::chrono::milliseconds(400) && now - g_mt_still > std::chrono::milliseconds(300)) {
                        g_mt_cmd_t = now;
                        const float lo = glm::radians(style == kWheel ? 30.0f : 8.0f), hi = glm::radians(style == kWheel ? 240.0f : 70.0f);
                        if (std::abs(theta) < glm::radians(2.0f)) {
                            if (++g_mt_none >= 3) {
                                g_mt_dead = true;
                                spdlog::info("[Drive] drivemodelsteer: the vehicle's own {} does not turn with the steering sent ({:+.2f} sent, {:+.1f} deg seen, three times): "
                                             "the usual steering for this drive", style_name(style), s, glm::degrees(theta));
                            }
                        } else if ((-theta > 0.0f) != (s > 0.0f)) {
                            if (++g_mt_opp >= 2) {
                                g_mt_sign = -g_mt_sign;
                                g_mt_opp  = 0;
                                spdlog::info("[Drive] drivemodelsteer: the vehicle's own {} turns the other way round its axis: its turn's sign changed", style_name(style));
                            }
                        } else {
                            g_mt_none = g_mt_opp = 0;
                            const float seen = std::clamp(std::abs(us) >= 0.98f ? std::abs(theta) : std::abs(theta) / std::abs(s), lo, hi);
                            g_mt_lock += 0.25f * (seen - g_mt_lock);
                            if (o.log && g_mt_obs++ < 6)
                                spdlog::info("[Drive] drivemodelsteer: the vehicle's own {} shows {:+.0f} deg for {:+.2f} sent{}: its full lock taken as {:.0f} deg", style_name(style),
                                             glm::degrees(theta), us, std::abs(us) >= 0.98f ? " (full)" : "", glm::degrees(g_mt_lock));
                        }
                    }
                    if (o.log && now - g_mt_log_t > std::chrono::seconds(2)) {
                        g_mt_log_t = now;
                        spdlog::info("[Drive] drivemodelsteer: the hand turned the {} {:+.0f} deg, the vehicle's own shows {:+.0f} deg (full lock {:.0f} deg, x{:.2f}); sent {:+.2f}",
                                     style_name(style), glm::degrees(g_angle), glm::degrees(theta), glm::degrees(g_mt_lock), style == kWheel ? o.wheel_ratio : 1.0f, us);
                    }
                    s = us;
                } else if (o.react_boost > 0.0f && o.react_tau > 0.01f) {
                    const float u = std::clamp(s + o.react_boost * (s - g_react_game), -1.0f, 1.0f);
                    g_react_game += (u - g_react_game) * std::min(1.0f, dt / o.react_tau);
                    s = u;
                } else {
                    g_react_game = s;
                }
                const float a = std::abs(s);
                if (o.steer_curve >= 1.0f) {
                    // drivesteercurve (see the top): the game's dead zone (0.24) and its ~x^1.7 curve undone: turn rate in proportion to a.
                    constexpr float kGameDead = 0.24f;
                    out.steer = a < 0.015f ? 0.0f : std::copysign(std::min(1.0f, kGameDead + (1.0f - kGameDead) * std::pow(a, 1.0f / o.steer_curve)), s);
                } else {
                    out.steer = a < 0.015f ? 0.0f : std::copysign(std::min(1.0f, o.react_dead + (1.0f - o.react_dead) * a), s);
                }
            }
            out.right_brake = g_held[kRight] && o.right_brake;
            g_steer_max     = std::max(g_steer_max, std::abs(out.steer));
        } else {
            glm::vec3 d{ 0.0f };
            float     tw = 0.0f;
            int       k = 0, kt = 0;
            for (int h = 0; h < 2; ++h) {
                if (!g_held[h]) continue;
                d += hp[h] - g_grab_pos[h];
                ++k;
                float yaw = 0.0f;
                if (hand_yaw(h, yaw)) g_last_yaw[h] = yaw;
                tw += wrap(g_last_yaw[h] - g_grab_yaw[h]);
                ++kt;
            }
            if (k > 0) {
                g_defl  = d / float(k);
                g_twist = kt > 0 ? tw / float(kt) : 0.0f;
            } else {
                g_defl  = glm::vec3{ 0.0f };   // a stick let go springs back
                g_twist = 0.0f;
            }
            auto axis = [](float x, float full, float dead) {
                const float a = std::abs(x) / std::max(1e-3f, full);
                if (a <= dead) return 0.0f;
                return std::copysign(std::min(1.0f, (a - dead) / (1.0f - dead)), x);
            };
            out.stick_on = any;
            out.sx       = axis(g_defl.x, o.stick_range, 0.12f);
            out.sy       = axis(-g_defl.z, o.stick_range, 0.12f);   // pushed forward (-z) = stick up
            out.twist_on = any && o.twist;
            out.twist    = axis(-g_twist, glm::radians(std::max(5.0f, o.twist_deg)), glm::radians(8.0f) / glm::radians(std::max(5.0f, o.twist_deg)));   // turned right = right
            // drivestickfine: sent past the game's dead zone.
            if (o.stick_fine) {
                const float raw_x = out.sx, raw_y = out.sy, raw_t = out.twist;
                const float m     = std::sqrt(out.sx * out.sx + out.sy * out.sy);
                // The hand feels where the stick's centre and end are (nothing holds it back, and the hand drawn rides the game's own stick):
                // a short buzz when the stick reaches its end, a lighter one when it comes back to its centre.
                if (any) {
                    const bool full = m >= 0.999f, centre = m <= 1e-4f;
                    for (int h = 0; h < 2; ++h) {
                        if (!g_held[h]) continue;
                        if (full && !g_st_full) out.buzz[h] = std::max(out.buzz[h], 0.30f);
                        if (centre && !g_st_centre) out.buzz[h] = std::max(out.buzz[h], 0.12f);
                    }
                    g_st_full   = g_st_full ? m > 0.93f : full;
                    g_st_centre = g_st_centre ? m < 0.08f : centre;
                } else {
                    g_st_full   = false;
                    g_st_centre = true;
                }
                if (m > 1e-4f) {
                    const float gd = std::clamp(o.stick_dead, 0.0f, 0.30f);
                    const float g  = gd + (1.0f - gd) * std::pow(std::min(1.0f, m), std::clamp(o.stick_curve, 1.0f, 3.0f));
                    out.sx *= g / m;
                    out.sy *= g / m;
                } else {
                    out.sx = out.sy = 0.0f;
                }
                if (!any) {
                    g_tw_s = 0.0f;
                } else if (in.veh_type == 4 && o.heli) {
                    const float t = std::copysign(o.heli_gain * std::pow(std::abs(out.twist), o.heli_curve), out.twist);
                    const float s = 0.5f * o.heli_smooth;
                    g_tw_s += (t - g_tw_s) * (s <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / s));
                    if (std::abs(g_tw_s) < 0.005f && t == 0.0f) g_tw_s = 0.0f;
                } else {
                    g_tw_s = out.twist;
                }
                const float a = std::abs(g_tw_s);
                out.twist     = a < 0.005f ? 0.0f : std::copysign(std::min(1.0f, 0.27f + (0.94f - 0.27f) * a), g_tw_s);
                if (o.log && any && g_fine_lines < 40 && now - g_fine_log_t > std::chrono::seconds(2)) {
                    g_fine_log_t = now;
                    g_fine_lines++;
                    spdlog::info("[Drive] drivestickfine: the hand moved {:+.1f} cm right, {:+.1f} cm forward, turned {:+.0f} deg: the left stick ({:+.2f}, {:+.2f}) "
                                 "(as before: ({:+.2f}, {:+.2f})), the right stick's X {:+.2f} (as before: {:+.2f}){}",
                                 g_defl.x * 100.0f, -g_defl.z * 100.0f, glm::degrees(-g_twist), out.sx, out.sy, raw_x, raw_y, out.twist, raw_t,
                                 in.veh_type == 4 && o.heli ? "; the turn at driveheli's rate and curve" : "");
                }
            }
        }

        out.log = o.log;
        // While held, a line every 2 s.
        if (o.log && any && now - g_log_t > std::chrono::seconds(2)) {
            g_log_t = now;
            if (style == kStick)
                spdlog::info("[Drive] stick held by {}{}: moved {:+.0f} right {:+.0f} forward {:+.0f} up cm, turned {:+.0f} deg -> left stick ({:+.2f}, {:+.2f}){}", g_held[kRight] ? "right" : "",
                             g_held[kLeft] ? (g_held[kRight] ? " and left" : "left") : "", g_defl.x * 100.0f, -g_defl.z * 100.0f, g_defl.y * 100.0f, glm::degrees(g_twist), out.sx, out.sy,
                             out.twist_on ? fmt::format(", right stick X {:+.2f}", out.twist) : std::string{});
            else
                spdlog::info("[Drive] {} held by {}{}: turned {:+.0f} deg -> steer {:+.2f}{}", style_name(style), g_held[kRight] ? "right" : "",
                             g_held[kLeft] ? (g_held[kRight] ? " and left" : "left") : "", glm::degrees(g_angle), out.steer, out.right_brake ? "; the right trigger brakes" : "");
        }

        // The view (tracking space).
        const glm::mat3 R{ in.ref };
        auto            T = [&](const glm::vec3& p) { return glm::vec3{ in.ref * glm::vec4{ p, 1.0f } }; };
        vw.centre         = T(C);
        vw.u              = R * u;
        vw.v              = R * v;
        vw.n              = R * n;
        vw.angle          = g_angle;
        if (style == kWheel) {
            vw.radius = wheel_r;
            for (int h = 0; h < 2; ++h) {
                vw.held[h] = g_held[h];
                vw.in_reach[h] = g_near[h];
                if (g_held[h]) vw.hold[h] = T(C + wheel_r * (std::cos(g_hand_ang[h]) * u + std::sin(g_hand_ang[h]) * v));
            }
        } else if (style == kBars) {
            vw.radius = bars_w;
            const glm::vec3 e{ bars_w * std::cos(g_angle), 0.0f, -bars_w * std::sin(g_angle) };
            for (int h = 0; h < 2; ++h) {
                vw.held[h] = g_held[h];
                vw.in_reach[h] = g_near[h];
                if (g_held[h]) vw.hold[h] = T(C + (h == kRight ? e : -e));
            }
        } else {
            const float     r   = o.stick_range;
            glm::vec3       dd  = g_defl;
            const float     len = glm::length(glm::vec2{ dd.x, dd.z });
            if (len > r) { dd.x *= r / len; dd.z *= r / len; }
            vw.stick_top = T(C + glm::vec3{ dd.x, 0.0f, dd.z });
            vw.base      = T(C + glm::vec3{ 0.0f, -0.28f, 0.0f });
            // Held, and the vehicle's own stick followed by its bone: drawn on it (it moves with the cabin, and the game leans it with what
            // the hand sends), not where it was taken plus the hand's move.
            if (any && o.hand.model && o.hand.grab && o.hands == 1 && g_model.bone && g_model.style == style && now - g_model_t < std::chrono::milliseconds(500)) {
                vw.stick_top = T(g_model.centre);
                vw.base      = T(g_model.centre + glm::vec3{ 0.0f, -0.28f, 0.0f });
            }
            for (int h = 0; h < 2; ++h) {
                vw.held[h] = g_held[h];
                vw.in_reach[h] = g_near[h];
                if (g_held[h]) vw.hold[h] = vw.stick_top;
            }
        }
        g_view         = vw;
        g_view_ok      = true;
        g_view_ref_inv = inv;
        g_out     = out;
        return out;
    }

    void echo_sent(uint16_t buttons, uint8_t lt, uint8_t rt)
    {
        std::lock_guard _{ g_echo_mtx };
        if (!g_echo_on) return;
        const auto now = clock::now();
        for (int i = 0; i < 18; ++i) {
            const bool on = i < 16 ? (buttons & (1u << i)) != 0 : i == 16 ? lt > 128 : rt > 128;
            if (on && !g_echo_held[i]) g_echo_start[i] = now;
            if (on) g_echo_last[i] = now;
            g_echo_held[i] = on;
        }
    }

    void echo_read(uint32_t pressed)
    {
        std::lock_guard _{ g_echo_mtx };
        const uint32_t up = pressed & ~g_echo_was;
        g_echo_was        = pressed;
        if (!g_echo_on || up == 0) return;
        static const char* kName[8]{ "right A", "left X", "right B", "left Y", "right trigger", "left trigger", "right grip", "left grip" };
        const auto         now = clock::now();
        std::string        sent;
        for (int i = 0; i < 18; ++i) {
            if (g_echo_last[i] == clock::time_point{} || now - g_echo_last[i] > std::chrono::milliseconds(600)) continue;
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_echo_start[i]).count();
            sent += fmt::format("{}{} (from {} ms before{})", sent.empty() ? "" : ", ", echo_pad_name(i), since, g_echo_held[i] ? ", still on" : "");
        }
        for (int b = 0; b < 8; ++b) {
            if (!(up & (1u << b))) continue;
            g_echo_presses++;
            if (!sent.empty()) g_echo_after++;
            spdlog::info("[Pad] drivepadecho: {} pressed (the runtime says); the virtual pad sent in the 0.6 s before: {}", kName[b],
                         sent.empty() ? "nothing (a press of the controller)" : sent + " (an echo if nobody pressed it: the runtime reads the virtual pad)");
        }
    }

    Buttons buttons_poll(bool active, bool a, bool x, int veh_type)
    {
        std::lock_guard _{ g_mtx };
        const auto now = clock::now();
        Buttons    b{};
        if (g_btn_test != 0) {   // drivebuttontest: the press added to the real buttons (xx: a 120 ms tap, 120 ms up, then held)
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_btn_test_t0).count();
            if (el > g_btn_test_ms + 500) g_btn_test = 0;
            else if (g_btn_test == 1) a = a || el < g_btn_test_ms;
            else if (g_btn_test == 2) x = x || el < g_btn_test_ms;
            else x = x || el < 120 || (el >= 240 && el < g_btn_test_ms);
        }
        if (active != g_btn_active) {
            // A press held when the layout starts or ends waits until let go (no throttle from a seat change press, no B from a brake).
            g_btn_active = active;
            g_btn_a_wait = a;
            g_btn_x_wait = x;
            g_btn_hb     = false;
            g_btn_tap    = false;
            g_btn_x_was  = x;
            g_btn_x_down = clock::time_point{};   // a press held across the change is no first press of a double one
            if (active) {
                g_btn_type = veh_type;
                spdlog::info("[Drive] drivebuttons: right A is the throttle, left X brakes / reverses, a double left X is the handbrake (vehicle type {}); triggers, grips and the right "
                             "stick no longer accelerate or brake{}",
                             veh_type, a || x ? " (a button held now waits until let go)" : "");
            } else if (g_btn_type >= 0) {
                spdlog::info("[Drive] drivebuttons: back to the usual buttons; {} brake presses, {} handbrakes this drive", g_btn_brake_n, g_btn_hb_n);
                g_btn_type  = -1;
                g_btn_hb_n  = 0;
                g_btn_brake_n = 0;
            }
        }
        if (!active) return b;
        if (!a) g_btn_a_wait = false;
        if (!x) g_btn_x_wait = false;
        if (x && !g_btn_x_was) {   // left X pressed: a second press soon after a short one is the handbrake
            const auto dbl = std::chrono::milliseconds(g_o.dbl_ms);
            g_btn_hb       = g_btn_tap && now - g_btn_x_up < dbl;
            g_btn_x_down   = now;
            if (g_btn_hb) {
                g_btn_hb_n++;
                if (g_o.log)
                    spdlog::info("[Drive] drivebuttons: handbrake (double X, {} ms after the first press ended)", std::chrono::duration_cast<std::chrono::milliseconds>(now - g_btn_x_up).count());
            } else {
                g_btn_brake_n++;
            }
        } else if (!x && g_btn_x_was) {
            g_btn_x_up = now;
            g_btn_tap  = !g_btn_hb && now - g_btn_x_down < std::chrono::milliseconds(g_o.dbl_ms);   // a short brake press can start a double press
            g_btn_hb   = false;
        }
        g_btn_x_was = x;
        const bool as = a && !g_btn_a_wait;
        const bool xs = x && !g_btn_x_wait;
        b.handbrake   = xs && g_btn_hb;
        // Right A is always the throttle (RT), left X the brake / reverse (LT). Author 7 Nov 13:17 (drivethrottle, which could move A onto the left stick, removed: at 14:00:57 a sidearm the game drew again after
        // driveready's had moved A there for the rest of the session, and the stick does not drive: "A stopped working for driving").
        b.rt = as;
        b.lt = xs && !g_btn_hb;
        return b;
    }

    void heli_stick(bool heli, float& rx, float& ry, bool ry_live)
    {
        std::lock_guard _{ g_mtx };
        const auto now = clock::now();
        if (!heli || !g_o.heli) {
            if (g_hs_on && g_hs_said < 20) {
                g_hs_said++;
                spdlog::info("[Drive] driveheli: {}", g_o.heli ? "out of the helicopter" : "off");
            }
            g_hs_on = false;
            g_hs[0] = g_hs[1] = 0.0f;
            return;
        }
        float dt = g_hs_on ? std::chrono::duration<float>(now - g_hs_t).count() : 0.0f;
        dt       = std::clamp(dt, 0.0f, 0.1f);
        g_hs_t   = now;
        if (!g_hs_on && g_hs_said < 20) {
            g_hs_said++;
            spdlog::info("[Drive] driveheli on: the helicopter's right stick at {:.0f} % of the game's rate, curve {:.1f}, smoothed over {:.2f} s", g_o.heli_gain * 100.0f, g_o.heli_curve,
                         g_o.heli_smooth);
        }
        g_hs_on = true;
        // Shaped and scaled (game-relative, 0 = still, 1 = the game's full rate), the thumbstick's own drift (0.12) left out on X; Y comes past
        // helipitch's dead zone already.
        auto shape = [&](float x, float dead) {
            const float a = std::abs(x);
            if (a <= dead) return 0.0f;
            const float n = std::min(1.0f, (a - dead) / (1.0f - dead));
            return std::copysign(g_o.heli_gain * std::pow(n, g_o.heli_curve), x);
        };
        const float t[2]{ shape(rx, 0.12f), ry_live ? shape(ry, 0.0f) : 0.0f };
        const float k = g_o.heli_smooth <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / g_o.heli_smooth);
        float*      o[2]{ &rx, &ry };
        for (int i = 0; i < 2; ++i) {
            g_hs[i] += (t[i] - g_hs[i]) * k;
            if (std::abs(g_hs[i]) < 0.005f && t[i] == 0.0f) g_hs[i] = 0.0f;
            // Past the game's dead zone (the virtual pad is an Xbox 360 pad: 0.2652, full at 0.94), so the least push turns slowly.
            const float a = std::abs(g_hs[i]);
            *o[i]         = a < 0.005f ? 0.0f : std::copysign(std::min(1.0f, 0.27f + (0.94f - 0.27f) * a), g_hs[i]);
        }
    }

    bool buttons()
    {
        std::lock_guard _{ g_mtx };
        return g_o.buttons;
    }
    bool plane_stick()
    {
        std::lock_guard _{ g_mtx };
        return g_o.plane_stick;
    }
    int hands_mode()
    {
        std::lock_guard _{ g_mtx };
        return g_o.hands;
    }

    HandOpts hand_opts()
    {
        std::lock_guard _{ g_mtx };
        return g_o.hand;
    }

    int ready_mode()
    {
        std::lock_guard _{ g_mtx };
        return g_o.ready;
    }

    float ready_away()
    {
        std::lock_guard _{ g_mtx };
        return g_o.ready_away;
    }

    bool trig_test()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() < g_trig_test_until.load();
    }

    bool ready_keep()
    {
        std::lock_guard _{ g_mtx };
        return g_o.ready_keep;
    }

    int gun_throttle()
    {
        std::lock_guard _{ g_mtx };
        return g_o.gun_throttle;
    }

    int menu_mode()
    {
        std::lock_guard _{ g_mtx };
        return g_o.menu;
    }

    int menu_hold_ms()
    {
        std::lock_guard _{ g_mtx };
        return g_o.menu_hold;
    }

    void set_model(const Model& m)
    {
        std::lock_guard _{ g_mtx };
        g_model   = m;
        g_model_t = clock::now();
    }

    bool model_show(float& turn)
    {
        std::lock_guard _{ g_mtx };
        if (!g_ms_on || g_style == kNone || clock::now() - g_ms_t > std::chrono::milliseconds(300)) return false;
        turn = g_ms_turn;
        return true;
    }

    void set_shape_hint(int style)
    {
        std::lock_guard _{ g_mtx };
        g_hint     = style;
        g_hint_t   = clock::now();
        g_hint_veh = g_veh;
    }

    bool model_point(int side, glm::vec3& p)
    {
        std::lock_guard _{ g_mtx };
        if (!g_view_ok || g_style == kNone) return false;
        const View& v = g_view;
        if (g_style == kWheel) {
            // The rim at 3 o'clock (right) or 9 o'clock (left), from the tracking space back into the recentred reference.
            p = v.centre + (side == kRight ? 1.0f : -1.0f) * v.radius * v.u;
        } else if (g_style == kBars) {
            const glm::vec3 e{ v.radius * std::cos(g_angle), 0.0f, -v.radius * std::sin(g_angle) };
            p = v.centre + glm::mat3{ v.u, v.v, v.n } * (side == kRight ? e : -e);
        } else {
            p = v.stick_top;
        }
        p = glm::vec3{ g_view_ref_inv * glm::vec4{ p, 1.0f } };
        return true;
    }

    bool wheel_scan_active()
    {
        std::lock_guard _{ g_mtx };
        return clock::now() < g_scan_until;
    }

    bool view(View& v)
    {
        std::lock_guard _{ g_mtx };
        if (!g_view_ok || !g_o.on || !g_o.show) return false;
        v = g_view;
        return true;
    }

    bool control(View& v)
    {
        std::lock_guard _{ g_mtx };
        if (!g_view_ok || !g_o.on) return false;
        v = g_view;
        return true;
    }

    bool on()
    {
        std::lock_guard _{ g_mtx };
        return g_o.on;
    }
    bool show()
    {
        std::lock_guard _{ g_mtx };
        return g_o.show;
    }

    std::string status()
    {
        std::lock_guard _{ g_mtx };
        if (!g_o.on) return "Off.";
        if (g_style == kNone) return fmt::format("No control now ({}).", g_why);
        std::string s = fmt::format("{}: {}.", style_name(g_style), g_why);
        if (g_held[0] || g_held[1])
            s += fmt::format(" Held by the {}{}.", g_held[kRight] ? "right" : "", g_held[kLeft] ? (g_held[kRight] ? " and left hands" : "left hand") : " hand");
        else
            s += " Not held.";
        if (g_style == kStick)
            s += fmt::format(" Stick ({:+.2f}, {:+.2f}), turn {:+.2f}.", g_out.sx, g_out.sy, g_out.twist);
        else
            s += fmt::format(" Turned {:+.0f} deg, steer {:+.2f}.", glm::degrees(g_angle), g_out.steer);
        return s;
    }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        if (cmd.rfind("drive", 0) != 0) return false;
        const bool on = arg.empty() || arg == "on" || arg == "1" || arg == "true";
        auto       num = [&](float def) { return arg.empty() ? def : fv; };
        auto       more = [&](float& a, float& b, float& c) {
            std::string s1, s2, s3;
            rest >> s1 >> s2 >> s3;
            if (!s1.empty()) a = std::strtof(s1.c_str(), nullptr);
            if (!s2.empty()) b = std::strtof(s2.c_str(), nullptr);
            if (!s3.empty()) c = std::strtof(s3.c_str(), nullptr);
        };
        std::lock_guard _{ g_mtx };
        persist = true;
        Opts& o = g_o;
        if (cmd == "drive") {
            o.on = on;
            if (!on) drop_all("drive off");
            spdlog::info("[Drive] drive {}", on ? "on: hold the wheel, handlebars or stick with a grip while driving (F11 Experimental > Gesture driving)" : "off");
        } else if (cmd == "driveshow") {
            o.show = on;
            spdlog::info("[Drive] driveshow {}", on ? "on" : "off");
        } else if (cmd == "drivewheel") {   // drivewheel fwd down radius tilt
            float fwd = num(0.38f), down = o.wheel_down, r = o.wheel_r, t = o.wheel_tilt;
            more(down, r, t);
            o.wheel_fwd  = std::clamp(fwd, 0.1f, 0.9f);
            o.wheel_down = std::clamp(down, -0.2f, 0.9f);
            o.wheel_r    = std::clamp(r, 0.08f, 0.35f);
            o.wheel_tilt = std::clamp(t, -30.0f, 80.0f);
            spdlog::info("[Drive] drivewheel {:.3f} {:.3f} {:.3f} {:.0f} (m ahead, m down, radius, deg of tilt)", o.wheel_fwd, o.wheel_down, o.wheel_r, o.wheel_tilt);
        } else if (cmd == "drivewheellock") {
            o.wheel_lock = std::clamp(num(90.0f), 20.0f, 540.0f);
            spdlog::info("[Drive] drivewheellock {:.0f} deg", o.wheel_lock);
        } else if (cmd == "drivebars") {   // drivebars fwd down halfwidth
            float fwd = num(0.42f), down = o.bars_down, w = o.bars_w, unused = 0.0f;
            more(down, w, unused);
            o.bars_fwd  = std::clamp(fwd, 0.1f, 0.9f);
            o.bars_down = std::clamp(down, -0.2f, 0.9f);
            o.bars_w    = std::clamp(w, 0.10f, 0.45f);
            spdlog::info("[Drive] drivebars {:.3f} {:.3f} {:.3f} (m ahead, m down, half width)", o.bars_fwd, o.bars_down, o.bars_w);
        } else if (cmd == "drivebarslock") {
            o.bars_lock = std::clamp(num(30.0f), 10.0f, 90.0f);
            spdlog::info("[Drive] drivebarslock {:.0f} deg", o.bars_lock);
        } else if (cmd == "drivestick") {   // drivestick fwd down
            float fwd = num(0.30f), down = o.stick_down, unused1 = 0.0f, unused2 = 0.0f;
            more(down, unused1, unused2);
            o.stick_fwd  = std::clamp(fwd, 0.05f, 0.9f);
            o.stick_down = std::clamp(down, -0.2f, 1.0f);
            spdlog::info("[Drive] drivestick {:.3f} {:.3f} (m ahead, m down)", o.stick_fwd, o.stick_down);
        } else if (cmd == "drivestickrange") {
            o.stick_range = std::clamp(num(0.12f), 0.03f, 0.25f);
            spdlog::info("[Drive] drivestickrange {:.3f} m", o.stick_range);
        } else if (cmd == "drivestickfine") {
            o.stick_fine = on;
            spdlog::info("[Drive] drivestickfine {}", on ? "on: the stick's left stick and turn start just past the game's dead zone (the whole of the hand's move counts); in a "
                                                            "helicopter the hand's turn has driveheli's rate and curve"
                                                         : "off: the hand's move and turn sent as they are");
        } else if (cmd == "drivestickdead") {
            o.stick_dead = std::clamp(num(0.05f), 0.0f, 0.30f);
            spdlog::info("[Drive] drivestickdead {:.2f} (the left stick starts at this, just past the game's own dead zone)", o.stick_dead);
        } else if (cmd == "drivestickcurve") {
            o.stick_curve = std::clamp(num(1.0f), 1.0f, 3.0f);
            spdlog::info("[Drive] drivestickcurve {:.1f}", o.stick_curve);
        } else if (cmd == "drivetwist") {
            o.twist = on;
            spdlog::info("[Drive] drivetwist {}", on ? "on: turning the hand on the stick turns the aircraft (right stick X)" : "off");
        } else if (cmd == "drivetwistdeg") {
            o.twist_deg = std::clamp(num(40.0f), 10.0f, 120.0f);
            spdlog::info("[Drive] drivetwistdeg {:.0f}", o.twist_deg);
        } else if (cmd == "drivegrab") {
            o.grab = std::clamp(num(0.10f), 0.03f, 0.30f);
            spdlog::info("[Drive] drivegrab {:.3f} m", o.grab);
        } else if (cmd == "drivereact") {
            o.react = on;
            spdlog::info("[Drive] drivereact {}", on ? fmt::format("on: wheel steering x{:.1f}, the game's dead zone {:.2f} skipped, lead {:.3f} s", o.react_gain, o.react_dead, o.react_lead)
                                                     : std::string{ "off: as before 6 Nov" });
        } else if (cmd == "drivereactgain") {
            o.react_gain = std::clamp(num(2.0f), 1.0f, 4.0f);
            spdlog::info("[Drive] drivereactgain {:.2f} (full steering at {:.0f} deg of wheel)", o.react_gain, o.wheel_lock / o.react_gain);
        } else if (cmd == "drivereactdead") {
            o.react_dead = std::clamp(num(0.20f), 0.0f, 0.45f);
            spdlog::info("[Drive] drivereactdead {:.2f}", o.react_dead);
        } else if (cmd == "drivereactlead") {
            o.react_lead = std::clamp(num(0.0f), 0.0f, 0.15f);
            spdlog::info("[Drive] drivereactlead {:.3f} s", o.react_lead);
        } else if (cmd == "drivereactboost") {
            o.react_boost = std::clamp(num(1.5f), 0.0f, 4.0f);
            spdlog::info("[Drive] drivereactboost {:.2f}{}", o.react_boost,
                         o.react_boost > 0.0f ? fmt::format(" (a game steering easing over {:.2f} s reaches the wheel about {:.1f} times sooner)", o.react_tau, 1.0f + o.react_boost)
                                              : std::string{ " (off: the wheel's steering sent as it is)" });
        } else if (cmd == "drivesteercurve") {
            o.steer_curve = (arg == "off" || arg == "0") ? 0.0f : std::clamp(num(1.7f), 1.0f, 3.0f);
            spdlog::info("[Drive] drivesteercurve {}", o.steer_curve >= 1.0f
                         ? fmt::format("{:.2f}: steering sent 0.24 + 0.76 x a^(1/{:.2f}), the car's turn rate in proportion to the wheel (the game's dead zone and curve undone)", o.steer_curve, o.steer_curve)
                         : std::string{ "off: drivereactdead + the rest in proportion (as before 8 Nov)" });
        } else if (cmd == "drivereacttau") {
            o.react_tau = std::clamp(num(0.25f), 0.05f, 0.80f);
            spdlog::info("[Drive] drivereacttau {:.2f} s", o.react_tau);
        } else if (cmd == "drivecentre") {
            o.centre_s = std::clamp(num(0.30f), 0.0f, 3.0f);
            spdlog::info("[Drive] drivecentre {:.2f} s", o.centre_s);
        } else if (cmd == "driverightbrake") {
            o.right_brake = on;
            spdlog::info("[Drive] driverightbrake {}", on ? "on: the right hand on the wheel brakes with its trigger" : "off: the right trigger shoots, on the wheel or not");
        } else if (cmd == "drivestyle") {   // not saved: for the vehicle you are in
            persist = false;
            g_force = arg == "wheel" ? kWheel : arg == "bars" ? kBars : arg == "stick" ? kStick : arg == "off" ? kNone : -1;
            spdlog::info("[Drive] drivestyle {} (for this vehicle)", g_force < 0 ? "auto" : style_name(g_force));
        } else if (cmd == "drivebuttons") {
            o.buttons = on;
            spdlog::info("[Drive] drivebuttons {}", on ? "on: cars, quads, bikes and boats: right A throttle, left X brake / reverse, double left X handbrake" : "off: the triggers, grips and right stick drive as before");
        } else if (cmd == "drivebuttonsdbl") {
            o.dbl_ms = (int)std::clamp(num(350.0f), 150.0f, 800.0f);
            spdlog::info("[Drive] drivebuttonsdbl {} ms", o.dbl_ms);
        } else if (cmd == "drivetrigtest") {   // test, not saved: drivetrigtest [ms] (default 1500): the right trigger held, as pressed on the controller
            persist      = false;
            const int ms = std::clamp(int(num(1500.0f)), 10, 10000);
            g_trig_test_until = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() + ms;
            spdlog::info("[Drive] drivetrigtest: the right trigger held {} ms", ms);
        } else if (cmd == "drivebuttontest") {   // test, not saved: drivebuttontest a|x|xx [ms] (default 2000)
            persist = false;
            std::string ms;
            rest >> ms;
            g_btn_test    = arg == "a" ? 1 : arg == "x" ? 2 : arg == "xx" ? 3 : 0;
            g_btn_test_ms = ms.empty() ? 2000 : std::clamp(std::atoi(ms.c_str()), 300, 10000);
            g_btn_test_t0 = clock::now();
            spdlog::info("[Drive] drivebuttontest {} for {} ms{}", g_btn_test == 1 ? "right A" : g_btn_test == 2 ? "left X" : g_btn_test == 3 ? "left X twice (tap, then held)" : "off",
                         g_btn_test_ms, g_o.buttons ? "" : " (drivebuttons is off: nothing is sent)");
        } else if (cmd == "driveplanestick") {
            o.plane_stick = on;
            spdlog::info("[Drive] driveplanestick {}", on ? "on: the right stick reaches the game in a plane, as before" : "off: the right stick sends nothing in a plane");
        } else if (cmd == "drivehands") {
            o.hands = arg == "off" || arg == "0" ? 0 : arg == "all" || arg == "2" ? 2 : 1;
            spdlog::info("[Drive] drivehands {}", o.hands == 0 ? "off: the player's body hidden while driving, as before" : o.hands == 2 ? "all: the player's whole body as the game draws it while driving (test)" : "hands: the game's own hands on the wheel, bars or stick while driving");
        } else if (cmd == "drivehandsgrab") {
            o.hand.grab = on;
            spdlog::info("[Drive] drivehandsgrab {}", on ? "on: driving, the hands follow the controllers as on foot; a hand the gesture drive holds the control with sticks to it"
                                                         : "off: the game's hands where the game animates them");
        } else if (cmd == "drivehandsdirect") {
            o.hand.direct = on;
            spdlog::info("[Drive] drivehandsdirect {}", on ? "on: the bare arms drawn directly, one instance, while driving (the game's culling of them skipped)"
                                                           : "off: the bare arms drawn with the game's own arguments while driving");
        } else if (cmd == "drivehandsontop") {
            o.hand.ontop = on;
            spdlog::info("[Drive] drivehandsontop {}", on ? "on (test): the bare arms drawn over everything while driving (their depth test always passes)"
                                                          : "off: the bare arms depth-tested as the game draws them");
        } else if (cmd == "drivehandsspace") {
            static const char* k[]{ "object", "hair", "world", "camera", "view" };
            o.hand.space = -1;
            for (int i = 0; i < 5; ++i)
                if (arg == k[i] || arg == std::to_string(i)) o.hand.space = i;
            spdlog::info("[Drive] drivehandsspace {}", o.hand.space < 0 ? "auto: driving, the bare arms' palette space found against the arm hair each drive"
                                                                        : fmt::format("{} (test): the bare arms' palette taken as that space while driving", k[o.hand.space]));
        } else if (cmd == "drivemodelsteer") {
            o.model_steer = on;
            g_mt_dead     = false;
            spdlog::info("[Drive] drivemodelsteer {}", on ? "on: with the vehicle's own wheel or bars followed (vehcontrol), the steering makes it turn as far as the hand turned"
                                                          : "off: the steering from drivewheellock and drivereactgain, as before");
        } else if (cmd == "drivemodelshow") {
            o.model_show = on;
            spdlog::info("[Drive] drivemodelshow {}", on ? "on: while a hand holds it, the vehicle's own wheel or bars drawn at the hand's angle (the game eases its own behind)"
                                                         : "off: the vehicle's own wheel where the game turns it");
        } else if (cmd == "drivewheelratio") {
            o.wheel_ratio = std::clamp(num(1.0f), 0.5f, 4.0f);
            spdlog::info("[Drive] drivewheelratio {:.2f}: the vehicle's own wheel turns {:.2f} times the hand's turn (drivemodelsteer)", o.wheel_ratio, o.wheel_ratio);
        } else if (cmd == "drivehandsmodel") {
            o.hand.model = on;
            if (!on) g_cur.on = false;
            spdlog::info("[Drive] drivehandsmodel {}", on ? "on: the wheel, bars or stick placed on the vehicle's own (from the game's hands; the wheel once the car "
                                                            "has turned 30 deg), and a hand that holds it goes where the game moves it (the car's wheel model, "
                                                            "the in-game stick)"
                                                          : "off: a hand that holds the control turns with the drive's own wheel, as before");
        } else if (cmd == "drivegripfist") {
            o.hand.fist = on;
            spdlog::info("[Drive] drivegripfist {}", on ? "on: a hand holding the wheel, bars or stick closes all its fingers round it, index and thumb too, by the grip alone"
                                                        : "off: a hand holding the wheel, bars or stick closes its fingers by grip and trigger, as on foot");
        } else if (cmd == "drivewheelscan") {   // test, not saved: drivewheelscan S (default 6)
            persist      = false;
            const float t = std::clamp(num(6.0f), 0.0f, 60.0f);
            g_scan_until = clock::now() + std::chrono::milliseconds(int(t * 1000.0f));
            spdlog::info("[Drive] drivewheelscan {:.0f} s: the vehicle's draws near the driver logged with their turn in the vehicle's axes ([VehHands] wheelscan)", t);
        } else if (cmd == "driveready") {
            o.ready = arg == "off" || arg == "0" ? 0 : arg == "lb" || arg == "2" ? 2 : 1;
            spdlog::info("[Drive] driveready {}", o.ready == 0 ? "off: the right grip does nothing while driving (away from the wheel)"
                                                  : o.ready == 2 ? "lb (test): driving, a right grip away from the wheel taps LB (the game's shoot, which draws the sidearm first)"
                                                                 : "y: driving, a right grip away from the wheel taps Y (the game's change weapon: the sidearm drawn, no shot); the right trigger shoots");
        } else if (cmd == "drivereadyaway") {
            o.ready_away = std::clamp(num(0.18f), 0.0f, 1.0f);
            spdlog::info("[Drive] drivereadyaway {:.2f} m: driveready draws only for a right grip pressed this far from the wheel or bars, and over 1 s after "
                         "that hand let go of them", o.ready_away);
        } else if (cmd == "drivereadykeep") {
            o.ready_keep = !(arg == "off" || arg == "0");
            spdlog::info("[Drive] drivereadykeep {}", o.ready_keep ? "on: a sidearm the right grip drew stays out (drawn again when the game puts it away) until the right "
                                                                     "hand takes the wheel or bars; right A pushes the left stick meanwhile"
                                                                   : "off: the sidearm is drawn again only while the right grip stays held, as before");
        } else if (cmd.rfind("drivekeephold", 0) == 0) {   // drivekeephold on|off (saved), drivekeepholdscan / list / set (tests): FarCry5DsHold.cpp
            return fc5::dshold::command(cmd, arg, fv, rest, persist);
        } else if (cmd == "drivegunthrottle") {
            o.gun_throttle = arg == "stick" || arg == "1" ? 1 : arg == "rt" || arg == "2" ? 2 : 0;
            spdlog::info("[Drive] drivegunthrottle {}", o.gun_throttle == 2   ? "rt (default): while the sidearm is out, right A is still the throttle (RT); if the game ever shoots on it, A "
                                                                                "stops sending RT with the sidearm out for the rest of that drive"
                                                        : o.gun_throttle == 1 ? "stick (test): while the sidearm is out, right A pushes the left stick forward instead of RT"
                                                                              : "off: while the sidearm is out, right A sends nothing (RT would shoot it)");
        } else if (cmd == "drivemenu") {
            o.menu = arg == "off" || arg == "0" ? 0 : arg == "vehicle" || arg == "1" ? 1 : 2;
            spdlog::info("[Drive] drivemenu {}", o.menu == 0 ? "off: the menu button is the pause menu (Start), left grip + menu the map (Back), as before"
                                                 : o.menu == 2 ? "all: everywhere, a short press of the menu button opens the map (Back), a long one the pause menu (Start)"
                                                               : "vehicle: in a vehicle, a short press of the menu button opens the map (Back), a long one the pause menu (Start); on foot as before");
        } else if (cmd == "drivemenuhold") {
            o.menu_hold = (int)std::clamp(num(500.0f), 200.0f, 1500.0f);
            spdlog::info("[Drive] drivemenuhold {} ms (a menu button press this long opens the pause menu)", o.menu_hold);
        } else if (cmd == "drivequadseat") {
            o.quad_seat = std::clamp(num(0.15f), 0.0f, 0.35f);
            spdlog::info("[Drive] drivequadseat {:.2f} m{}", o.quad_seat, o.quad_seat > 0.0f ? ": a wheeled vehicle whose driver sits this close to its centre line gets the handlebars" : " (off)");
        } else if (cmd == "driveheli") {
            o.heli = on;
            spdlog::info("[Drive] driveheli {}", on ? fmt::format("on: the helicopter's right stick at {:.0f} %, curve {:.1f}, smoothed {:.2f} s", o.heli_gain * 100.0f, o.heli_curve, o.heli_smooth)
                                                    : std::string{ "off: the helicopter's right stick as before" });
        } else if (cmd == "driveheligain") {
            o.heli_gain = std::clamp(num(0.40f), 0.05f, 1.0f);
            spdlog::info("[Drive] driveheligain {:.2f}", o.heli_gain);
        } else if (cmd == "drivehelicurve") {
            o.heli_curve = std::clamp(num(2.0f), 1.0f, 4.0f);
            spdlog::info("[Drive] drivehelicurve {:.1f}", o.heli_curve);
        } else if (cmd == "drivehelismooth") {
            o.heli_smooth = std::clamp(num(0.30f), 0.0f, 1.5f);
            spdlog::info("[Drive] drivehelismooth {:.2f} s", o.heli_smooth);
        } else if (cmd == "drivehandgun" || cmd == "drivehandreach") {   // removed (author 6 Nov 20:57): dropped from the settings
            persist = false;
            spdlog::info("[Drive] {}: removed (the hands follow the controllers while driving, with no sidearm, and stick to the control the gesture drive holds)", cmd);
        } else if (cmd == "drivehandturn") {   // drivehandturn pitch yaw roll (deg)
            float p = num(0.0f), y = o.hand.turn.y, r = o.hand.turn.z, unused = 0.0f;
            more(y, r, unused);
            o.hand.turn = glm::clamp(glm::vec3{ p, y, r }, glm::vec3{ -180.0f }, glm::vec3{ 180.0f });
            spdlog::info("[Drive] drivehandturn {:.0f} {:.0f} {:.0f} (deg: pitch, yaw, roll in the controller's axes)", o.hand.turn.x, o.hand.turn.y, o.hand.turn.z);
        } else if (cmd == "drivehandshift") {   // drivehandshift x y z (m)
            float x = num(0.0f), y = o.hand.shift.y, z = o.hand.shift.z, unused = 0.0f;
            more(y, z, unused);
            o.hand.shift = glm::clamp(glm::vec3{ x, y, z }, glm::vec3{ -0.3f }, glm::vec3{ 0.3f });
            spdlog::info("[Drive] drivehandshift {:.3f} {:.3f} {:.3f} (m: right, up, back in the controller's axes; x mirrored for the left hand)", o.hand.shift.x, o.hand.shift.y,
                         o.hand.shift.z);
        } else if (cmd == "drivepadecho") {   // test, not saved: drivepadecho on|off
            persist = false;
            std::lock_guard _{ g_echo_mtx };
            if (g_echo_on && !on)
                spdlog::info("[Pad] drivepadecho off: {} presses from the runtime, {} of them within 0.6 s of something the virtual pad sent", g_echo_presses, g_echo_after);
            if (on && !g_echo_on) {
                g_echo_presses = g_echo_after = 0;
                for (auto& t : g_echo_last) t = clock::time_point{};
            }
            g_echo_on = on;
            if (on) spdlog::info("[Pad] drivepadecho on: each press the headset runtime reports is logged with what the virtual pad sent just before it");
        } else if (cmd == "drivelog") {
            persist = false;
            o.log   = on;
        } else {
            persist = false;
            return false;
        }
        return true;
    }

    void draw_vehicle_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        Opts o;
        {
            std::lock_guard _{ g_mtx };
            o = g_o;
        }
        bool bt = o.buttons;
        if (ImGui::Checkbox("Cars, quads, bikes and boats: A accelerates, X brakes, double X handbrake##drivebuttons", &bt)) cmds += bt ? "drivebuttons on\n" : "drivebuttons off\n";
        hint("Driving: right A accelerates, left X brakes and reverses, and a quick double X is the handbrake. The right trigger shoots. Aircraft are unchanged.");
        if (bt) {
            int ms = o.dbl_ms;
            if (ImGui::SliderInt("Double press within (ms)##drivebuttonsdbl", &ms, 150, 800)) cmds += fmt::format("drivebuttonsdbl {}\n", ms);
            hint("How quickly the second X press must come to count as the handbrake.");
        }
        int mm = o.menu;
        if (ImGui::Combo("Menu button: short press map, long press pause menu##drivemenu", &mm, "Off\0In vehicles\0Everywhere\0")) {
            static const char* k[]{ "off", "vehicle", "all" };
            cmds += fmt::format("drivemenu {}\n", k[std::clamp(mm, 0, 2)]);
        }
        hint("A short press of the menu button opens the map, a long press opens the pause menu. Choose everywhere, in vehicles only, or off.");
        if (mm != 0) {
            int mh = o.menu_hold;
            if (ImGui::SliderInt("Long press from (ms)##drivemenuhold", &mh, 200, 1500)) cmds += fmt::format("drivemenuhold {}\n", mh);
            hint("How long to hold the menu button to open the pause menu instead of the map.");
        }
        bool hl = o.heli;
        if (ImGui::Checkbox("Helicopter: gentle right stick##driveheli", &hl)) cmds += hl ? "driveheli on\n" : "driveheli off\n";
        hint("In a helicopter the right stick turns more slowly and smoothly.");
        if (hl) {
            float hg = o.heli_gain * 100.0f;
            if (ImGui::SliderFloat("Right stick at full push (% of the game's)##driveheligain", &hg, 5.0f, 100.0f, "%.0f")) cmds += fmt::format("driveheligain {:.2f}\n", hg * 0.01f);
            hint("How fast the helicopter turns with the stick pushed all the way. Lower: slower.");
            float hc = o.heli_curve;
            if (ImGui::SliderFloat("Curve##drivehelicurve", &hc, 1.0f, 4.0f, "%.1f")) cmds += fmt::format("drivehelicurve {:.1f}\n", hc);
            hint("Higher: small stick pushes do less. 1: straight.");
            float hs = o.heli_smooth;
            if (ImGui::SliderFloat("Smoothing (s)##drivehelismooth", &hs, 0.0f, 1.5f, "%.2f")) cmds += fmt::format("drivehelismooth {:.2f}\n", hs);
            hint("How long the stick takes to ease in and out. 0: none.");
        }
        bool ps = o.plane_stick;
        if (ImGui::Checkbox("Planes: the right stick moves the camera##driveplanestick", &ps)) cmds += ps ? "driveplanestick on\n" : "driveplanestick off\n";
        hint("In a plane, the right stick moves the camera. Off: it does nothing.");
        int rd = o.ready;
        if (ImGui::Combo("Right grip while driving##driveready", &rd, "Nothing\0Readies the sidearm (change weapon)\0Readies the sidearm (shoot button, test)\0")) {
            static const char* k[]{ "off", "y", "lb" };
            cmds += fmt::format("driveready {}\n", k[std::clamp(rd, 0, 2)]);
        }
        hint("Driving, a right grip away from the wheel draws your sidearm so you can shoot.");
        if (rd != 0) {
            float ra = o.ready_away * 100.0f;
            if (ImGui::SliderFloat("Right grip draws only this far from the wheel (cm)##drivereadyaway", &ra, 0.0f, 80.0f, "%.0f"))
                cmds += fmt::format("drivereadyaway {:.2f}\n", ra * 0.01f);
            hint("A grip closer than this to the wheel does not draw the sidearm, so grabbing the wheel never brings the gun out.");
            bool rk = o.ready_keep;
            if (ImGui::Checkbox("The sidearm stays out until the right hand takes the wheel##drivereadykeep", &rk)) cmds += rk ? "drivereadykeep on\n" : "drivereadykeep off\n";
            hint("Once drawn, the sidearm stays out until your right hand takes the wheel again.");
            if (rk) {
                bool kh = fc5::dshold::on();
                if (ImGui::Checkbox("Hold off the game's own put-away (no re-draw)##drivekeephold", &kh)) cmds += kh ? "drivekeephold on\n" : "drivekeephold off\n";
                hint("Stops the game from putting the sidearm away on its own while you keep it out.");
            }
        }
        int gt = o.gun_throttle;
        if (ImGui::Combo("A while the sidearm is out##drivegunthrottle", &gt, "Nothing (the vehicle rolls)\0Pushes the left stick (test)\0Still drives (default)\0"))
            cmds += gt == 2 ? "drivegunthrottle rt\n" : gt == 1 ? "drivegunthrottle stick\n" : "drivegunthrottle off\n";
        hint("What right A does while the sidearm is out: still accelerates, or does nothing.");
        int hm = o.hands;
        if (ImGui::Combo("Hands while driving##drivehands", &hm, "Hidden\0The game's hands on the wheel or stick\0The whole body (test)\0")) {
            static const char* k[]{ "off", "hands", "all" };
            cmds += fmt::format("drivehands {}\n", k[std::clamp(hm, 0, 2)]);
        }
        hint("Which hands are drawn while driving: the game's hands on the wheel, your whole body, or none.");
        if (hm != 1) return;
        bool dd = o.hand.direct;
        if (ImGui::Checkbox("Always draw the bare arms (test)##drivehandsdirect", &dd)) cmds += dd ? "drivehandsdirect on\n" : "drivehandsdirect off\n";
        hint("For testing: always draws the bare arms while driving. Use it only if the hands show as just arm hair.");
        bool ot = o.hand.ontop;
        if (ImGui::Checkbox("Draw the bare arms over everything (test)##drivehandsontop", &ot)) cmds += ot ? "drivehandsontop on\n" : "drivehandsontop off\n";
        hint("For testing: draws your hands in front of everything while driving.");
        int sp = o.hand.space + 1;
        if (ImGui::Combo("Where the bare arms are drawn from##drivehandsspace", &sp, "Found each drive\0Their own object transform (test)\0The arm hair's transform (test)\0World (test)\0World less the camera (test)\0The camera's view (test)\0")) {
            static const char* k[]{ "auto", "object", "hair", "world", "camera", "view" };
            cmds += fmt::format("drivehandsspace {}\n", k[std::clamp(sp, 0, 5)]);
        }
        hint("How the bare arms are placed while driving. Leave on automatic; the others are for testing.");
        bool hg = o.hand.grab;
        if (ImGui::Checkbox("Your VR hands, stuck to the wheel when you grip it##drivehandsgrab", &hg)) cmds += hg ? "drivehandsgrab on\n" : "drivehandsgrab off\n";
        hint("Driving, your hands follow the controllers, and a hand that grips the wheel sticks to it. Off: the game's own hands.");
        if (!hg) return;
        bool mdl = o.hand.model;
        if (ImGui::Checkbox("Hands glued to the vehicle's own wheel and stick##drivehandsmodel", &mdl)) cmds += mdl ? "drivehandsmodel on\n" : "drivehandsmodel off\n";
        hint("Your hands stay on the vehicle's own wheel or stick as it turns.");
        bool gf = o.hand.fist;
        if (ImGui::Checkbox("Whole hand closes on the wheel with the grip##drivegripfist", &gf)) cmds += gf ? "drivegripfist on\n" : "drivegripfist off\n";
        hint("Gripping the wheel closes your whole hand on it.");
        bool ms = o.model_steer;
        if (ImGui::Checkbox("Steering follows the vehicle's own wheel (hand and wheel turn together)##drivemodelsteer", &ms)) cmds += ms ? "drivemodelsteer on\n" : "drivemodelsteer off\n";
        hint("Turning your hand turns the car's wheel by the same angle, so your hand and the wheel move together.");
        if (ms) {
            bool sh = o.model_show;
            if (ImGui::Checkbox("The car's wheel turns with your hand at once##drivemodelshow", &sh)) cmds += sh ? "drivemodelshow on\n" : "drivemodelshow off\n";
            hint("The car's wheel turns with your hand straight away, instead of lagging behind.");
            float wr = o.wheel_ratio;
            if (ImGui::SliderFloat("Wheel turn per hand turn (x)##drivewheelratio", &wr, 0.5f, 4.0f, "%.2f")) cmds += fmt::format("drivewheelratio {:.2f}\n", wr);
            hint("How far the car's wheel turns per hand turn. 2: twice as far, for quicker steering.");
        }
        if (ImGui::TreeNode("Fit the hands to the controllers##drivehandfit")) {
            float t[3]{ o.hand.turn.x, o.hand.turn.y, o.hand.turn.z };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Pitch (deg)##dhtp", &t[0], -180.0f, 180.0f, "%.0f");
            moved |= ImGui::SliderFloat("Yaw (deg)##dhty", &t[1], -180.0f, 180.0f, "%.0f");
            moved |= ImGui::SliderFloat("Roll (deg)##dhtr", &t[2], -180.0f, 180.0f, "%.0f");
            if (moved) cmds += fmt::format("drivehandturn {:.0f} {:.0f} {:.0f}\n", t[0], t[1], t[2]);
            hint("Turns the hands while driving.");
            float d[3]{ o.hand.shift.x * 100.0f, o.hand.shift.y * 100.0f, o.hand.shift.z * 100.0f };
            moved = false;
            moved |= ImGui::SliderFloat("Right (cm)##dhsx", &d[0], -30.0f, 30.0f, "%.0f");
            moved |= ImGui::SliderFloat("Up (cm)##dhsy", &d[1], -30.0f, 30.0f, "%.0f");
            moved |= ImGui::SliderFloat("Back (cm)##dhsz", &d[2], -30.0f, 30.0f, "%.0f");
            if (moved) cmds += fmt::format("drivehandshift {:.3f} {:.3f} {:.3f}\n", d[0] * 0.01f, d[1] * 0.01f, d[2] * 0.01f);
            hint("Moves the hands while driving.");
            ImGui::TreePop();
        }
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        Opts o;
        int  force;
        {
            std::lock_guard _{ g_mtx };
            o     = g_o;
            force = g_force;
        }
        bool v = o.on;
        if (ImGui::Checkbox("Drive with your hands (wheel, handlebars, stick)##drive", &v)) cmds += v ? "drive on\n" : "drive off\n";
        hint("Grab the wheel, handlebars or stick with a grip and turn it to steer. The left trigger accelerates.");
        if (!v) return;
        ImGui::TextWrapped("%s", status().c_str());
        int st = force < 0 ? 0 : force == kWheel ? 1 : force == kBars ? 2 : force == kStick ? 3 : 4;
        if (ImGui::Combo("This vehicle##drivestyle", &st, "Automatic\0Wheel\0Handlebars\0Stick\0Nothing to hold\0")) {
            static const char* k[]{ "auto", "wheel", "bars", "stick", "off" };
            cmds += fmt::format("drivestyle {}\n", k[st]);
        }
        hint("Choose wheel, handlebars or stick for this vehicle. Automatic picks by vehicle type.");
        float qs = o.quad_seat * 100.0f;
        if (ImGui::SliderFloat("Quads: handlebars when you sit this near the middle (cm)##drivequadseat", &qs, 0.0f, 35.0f, "%.0f")) cmds += fmt::format("drivequadseat {:.2f}\n", qs * 0.01f);
        hint("Sitting this close to the middle of the vehicle gives handlebars (quads). 0: off.");
        bool sh = o.show;
        if (ImGui::Checkbox("Show the wheel / handlebars / stick##driveshow", &sh)) cmds += sh ? "driveshow on\n" : "driveshow off\n";
        hint("Shows the wheel, handlebars or stick faintly where you can grab it.");
        bool rb = o.right_brake;
        if (ImGui::Checkbox("Right hand on the wheel: its trigger brakes##driverightbrake", &rb)) cmds += rb ? "driverightbrake on\n" : "driverightbrake off\n";
        hint("With your right hand on the wheel, its trigger brakes and reverses. Off: it always shoots.");

        bool re = o.react;
        if (ImGui::Checkbox("Very reactive wheel##drivereact", &re)) cmds += re ? "drivereact on\n" : "drivereact off\n";
        hint("The car follows the wheel closely: half a turn gives full steering, and the smallest turn already steers.");
        if (re) {
            float rg = o.react_gain;
            if (ImGui::SliderFloat("Steering per wheel turn (x)##drivereactgain", &rg, 1.0f, 4.0f, "%.1f")) cmds += fmt::format("drivereactgain {:.2f}\n", rg);
            hint("How much steering you get per wheel turn. Higher: less turning needed.");
            bool sc = o.steer_curve >= 1.0f;
            if (ImGui::Checkbox("Car turns in proportion to the wheel##drivesteercurveon", &sc)) cmds += sc ? "drivesteercurve 1.7\n" : "drivesteercurve off\n";
            hint("Small wheel turns turn the car a little, large ones a lot, in proportion. Off: small turns do almost nothing.");
            if (sc) {
                float pc = o.steer_curve;
                if (ImGui::SliderFloat("Game's curve undone##drivesteercurve", &pc, 1.0f, 3.0f, "%.2f")) cmds += fmt::format("drivesteercurve {:.2f}\n", pc);
                hint("Higher: small wheel turns steer more. Lower it if the car is twitchy near straight.");
            }
            float rd = o.react_dead;
            if (ImGui::SliderFloat("Game dead zone skipped##drivereactdead", &rd, 0.0f, 0.45f, "%.2f")) cmds += fmt::format("drivereactdead {:.2f}\n", rd);
            hint("Lower it if the car twitches when you barely touch the wheel; raise it if small turns do nothing.");
            float rl = o.react_lead * 1000.0f;
            if (ImGui::SliderFloat("Lead (ms, test)##drivereactlead", &rl, 0.0f, 150.0f, "%.0f")) cmds += fmt::format("drivereactlead {:.3f}\n", rl * 0.001f);
            hint("Makes the car start turning a little sooner. Too much makes it nervous. 0: none.");
            float rb2 = o.react_boost;
            if (ImGui::SliderFloat("Steering boost##drivereactboost", &rb2, 0.0f, 4.0f, "%.1f")) cmds += fmt::format("drivereactboost {:.2f}\n", rb2);
            hint("Makes the car answer the wheel sooner. Lower it if the car overshoots or wobbles. 0: off.");
            float rt2 = o.react_tau * 1000.0f;
            if (ImGui::SliderFloat("Boost: the game's steering delay (ms)##drivereacttau", &rt2, 50.0f, 800.0f, "%.0f")) cmds += fmt::format("drivereacttau {:.3f}\n", rt2 * 0.001f);
            hint("How long the steering boost lasts. Longer: a stronger push.");
        }
        if (ImGui::TreeNode("Wheel##drivewheelnode")) {
            float p[4]{ o.wheel_fwd * 100.0f, o.wheel_down * 100.0f, o.wheel_r * 100.0f, o.wheel_tilt };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dwf", &p[0], 10.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dwd", &p[1], -20.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Radius (cm)##dwr", &p[2], 8.0f, 35.0f, "%.0f");
            moved |= ImGui::SliderFloat("Tilt (deg)##dwt", &p[3], -30.0f, 80.0f, "%.0f");
            if (moved) cmds += fmt::format("drivewheel {:.3f} {:.3f} {:.3f} {:.0f}\n", p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f, p[3]);
            hint("Where the wheel sits in front of you, and its size and tilt. Sit straight and recentre first.");
            float lk = o.wheel_lock;
            if (ImGui::SliderFloat("Turn for full lock (deg)##dwl", &lk, 20.0f, 360.0f, "%.0f")) cmds += fmt::format("drivewheellock {:.0f}\n", lk);
            hint("How far you turn the wheel for full steering.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Handlebars##drivebarsnode")) {
            float p[3]{ o.bars_fwd * 100.0f, o.bars_down * 100.0f, o.bars_w * 100.0f };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dbf", &p[0], 10.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dbd", &p[1], -20.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Half width (cm)##dbw", &p[2], 10.0f, 45.0f, "%.0f");
            if (moved) cmds += fmt::format("drivebars {:.3f} {:.3f} {:.3f}\n", p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f);
            hint("Where the handlebars sit and how wide they are.");
            float lk = o.bars_lock;
            if (ImGui::SliderFloat("Turn for full lock (deg)##dbl", &lk, 10.0f, 90.0f, "%.0f")) cmds += fmt::format("drivebarslock {:.0f}\n", lk);
            hint("How far you turn the handlebars for full steering.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Stick (helicopter, plane)##drivesticknode")) {
            float p[2]{ o.stick_fwd * 100.0f, o.stick_down * 100.0f };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dsf", &p[0], 5.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dsd", &p[1], -20.0f, 100.0f, "%.0f");
            if (moved) cmds += fmt::format("drivestick {:.3f} {:.3f}\n", p[0] * 0.01f, p[1] * 0.01f);
            hint("Where the top of the flight stick sits.");
            float rg = o.stick_range * 100.0f;
            if (ImGui::SliderFloat("Hand move for full stick (cm)##dsr", &rg, 3.0f, 25.0f, "%.0f")) cmds += fmt::format("drivestickrange {:.3f}\n", rg * 0.01f);
            hint("How far you move your hand for full stick.");
            bool sf = o.stick_fine;
            if (ImGui::Checkbox("Precise stick: the whole of the hand's move counts##drivestickfine", &sf)) cmds += sf ? "drivestickfine on\n" : "drivestickfine off\n";
            hint("The aircraft answers from the first small stick move, and the hand buzzes at the stick's end and centre.");
            if (sf) {
                float sc = o.stick_curve;
                if (ImGui::SliderFloat("Stick curve##drivestickcurve", &sc, 1.0f, 3.0f, "%.1f")) cmds += fmt::format("drivestickcurve {:.1f}\n", sc);
                float sdz = o.stick_dead;
                if (ImGui::SliderFloat("Stick start##drivestickdead", &sdz, 0.0f, 0.30f, "%.2f")) cmds += fmt::format("drivestickdead {:.2f}\n", sdz);
                hint("Raise it if the aircraft ignores the first small move; lower it if it starts with a jump.");
                hint("1: in proportion to your hand. Higher: finer near the centre.");
            }
            bool tw = o.twist;
            if (ImGui::Checkbox("Turn the hand to turn the aircraft##drivetwist", &tw)) cmds += tw ? "drivetwist on\n" : "drivetwist off\n";
            hint("Twisting your hand on the stick turns the aircraft left and right.");
            float td = o.twist_deg;
            if (ImGui::SliderFloat("Hand turn for full (deg)##dstd", &td, 10.0f, 120.0f, "%.0f")) cmds += fmt::format("drivetwistdeg {:.0f}\n", td);
            ImGui::TreePop();
        }
        float g = o.grab * 100.0f;
        if (ImGui::SliderFloat("Reach to grab (cm)##drivegrab", &g, 3.0f, 30.0f, "%.0f")) cmds += fmt::format("drivegrab {:.3f}\n", g * 0.01f);
        hint("How close your hand must be to the wheel, handlebars or stick to grab it.");
        float c = o.centre_s;
        if (ImGui::SliderFloat("Back to straight when let go (s)##drivecentre", &c, 0.0f, 3.0f, "%.2f")) cmds += fmt::format("drivecentre {:.2f}\n", c);
        hint("How quickly the wheel goes back to straight when you let go. 0: at once.");
    }
}
