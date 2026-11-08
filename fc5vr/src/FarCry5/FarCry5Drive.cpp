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
#include "FarCry5Drive.h"
#include "FarCry5VehicleTable.h"

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
            float stick_range{ 0.08f };                                                             // m of hand move for full stick
            bool  twist{ true };
            float twist_deg{ 40.0f };
            float grab{ 0.10f };
            float centre_s{ 0.30f };
            bool  react{ true };          // drivereact (see the top)
            float react_gain{ 2.0f };     // drivereactgain: the wheel's steering times this
            float react_dead{ 0.20f };    // drivereactdead: the game's left-stick dead zone, skipped
            float react_lead{ 0.0f };     // drivereactlead: s of the steering's rate added (test)
            float react_boost{ 1.5f };    // drivereactboost: times the modelled game steering's lag added (0: off)
            float react_tau{ 0.25f };     // drivereacttau: s, the modelled game steering's time constant
            bool  right_brake{ true };
            bool  log{ true };
            // Vehicle controls (FarCry5Drive.h), not tied to drive on|off.
            bool  buttons{ true };
            int   dbl_ms{ 350 };
            bool  plane_stick{ false };
            int   hands{ 1 };
            HandOpts hand{};
            int   ready{ 1 };             // driveready: 0 off, 1 y, 2 lb
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
        clock::time_point g_last{}, g_log_t{}, g_off_since{}, g_unknown_since{};
        bool        g_was_driving = false;
        int         g_polls = 0, g_holds = 0;
        float       g_steer_max = 0.0f;
        // drivebuttons (under g_mtx; both pad pollers call buttons_poll: only edges change the state, so a second poll of the same press does nothing)
        bool              g_btn_active = false, g_btn_a_wait = false, g_btn_x_wait = false;
        bool              g_btn_x_was = false, g_btn_hb = false, g_btn_tap = false;
        clock::time_point g_btn_x_down{}, g_btn_x_up{};
        int               g_btn_hb_n = 0, g_btn_brake_n = 0, g_btn_type = -1;
        int               g_btn_test = 0, g_btn_test_ms = 0;   // drivebuttontest (the simulator's controllers have no A or X): 1 a, 2 x, 3 xx
        clock::time_point g_btn_test_t0{};
        // driveheli (under g_mtx): the smoothed right stick (game-relative, before the dead zone is added)
        float             g_hs[2]{};
        clock::time_point g_hs_t{};
        bool              g_hs_on = false;
        int               g_hs_said = 0;
        clock::time_point g_seat_wait{};   // drivequadseat: when a type 0 vehicle started waiting for its seat
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
            if (in.veh_type == 1) { why = "boat (the game's type)"; return kWheel; }
            if (in.veh_type == 0) {
                // drivequadseat: the game calls a quad wheeled like a car; its driver sits on the vehicle's centre line, a car's 0.4 m to the side.
                if (g_o.quad_seat > 0.0f) {
                    if (std::abs(in.seat_side) > 50.0f) {
                        if (g_style != kNone) { why = g_why; return g_style; }
                        if (g_seat_wait == clock::time_point{}) g_seat_wait = now;
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

        // Each hand's distance to the control (grab), and its angle about it (wheel, bars).
        auto reach = [&](int h, float& ang, bool& ang_ok) {
            const glm::vec3 p = hp[h] - C;
            ang_ok            = false;
            if (style == kWheel) {
                const float     hgt = glm::dot(p, n);
                const glm::vec3 q   = p - n * hgt;
                const float     r   = glm::length(q);
                ang                 = std::atan2(glm::dot(q, v), glm::dot(q, u));
                ang_ok              = r > 0.04f;
                return std::sqrt((r - o.wheel_r) * (r - o.wheel_r) + hgt * hgt);
            }
            if (style == kBars) {
                const glm::vec3 e{ o.bars_w * std::cos(g_angle), 0.0f, -o.bars_w * std::sin(g_angle) };   // the right end, the bar turned by g_angle about +y
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
        for (int h = 0; h < 2; ++h) {
            const bool grip  = in.hand[h].grip && in.hand[h].tracked;
            const bool press = grip && !g_grip_was[h];
            g_grip_was[h]    = in.hand[h].grip;
            float      ang   = 0.0f;
            bool       ang_ok = false;
            const float d    = in.hand[h].tracked ? reach(h, ang, ang_ok) : 1e3f;
            g_near[h]        = !g_held[h] && d <= o.grab;
            if (g_held[h] && !grip) {
                g_held[h] = false;
                out.buzz[h] = 0.25f;
                if (o.log) spdlog::info("[Drive] {} hand let go of the {}{}", side_name(h), style_name(style),
                                        style == kStick ? std::string{} : fmt::format(" (turned {:.0f} deg)", glm::degrees(g_angle)));
                continue;
            }
            if (!g_held[h] && press && d <= o.grab) {
                g_held[h]        = true;
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

        if (style == kWheel || style == kBars) {
            const float lock = glm::radians(std::max(5.0f, style == kWheel ? o.wheel_lock : o.bars_lock));
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
            if (o.react) {
                // drivereact: more steering per degree of wheel, a lead on its change, and the game's dead zone skipped.
                float s = std::clamp(-g_angle / lock * (style == kWheel ? o.react_gain : 1.0f), -1.0f, 1.0f);
                const float rate = dt > 1e-4f ? (s - g_react_prev) / dt : 0.0f;
                g_react_prev     = s;
                if (o.react_lead > 0.0f) s = std::clamp(s + o.react_lead * rate, -1.0f, 1.0f);
                // drivereactboost (see the top): the wheel's steering plus B times the modelled game steering's lag behind it.
                if (o.react_boost > 0.0f && o.react_tau > 0.01f) {
                    const float u = std::clamp(s + o.react_boost * (s - g_react_game), -1.0f, 1.0f);
                    g_react_game += (u - g_react_game) * std::min(1.0f, dt / o.react_tau);
                    s = u;
                } else {
                    g_react_game = s;
                }
                const float a = std::abs(s);
                out.steer     = a < 0.015f ? 0.0f : std::copysign(std::min(1.0f, o.react_dead + (1.0f - o.react_dead) * a), s);
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
            vw.radius = o.wheel_r;
            for (int h = 0; h < 2; ++h) {
                vw.held[h] = g_held[h];
                vw.in_reach[h] = g_near[h];
                if (g_held[h]) vw.hold[h] = T(C + o.wheel_r * (std::cos(g_hand_ang[h]) * u + std::sin(g_hand_ang[h]) * v));
            }
        } else if (style == kBars) {
            vw.radius = o.bars_w;
            const glm::vec3 e{ o.bars_w * std::cos(g_angle), 0.0f, -o.bars_w * std::sin(g_angle) };
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
            for (int h = 0; h < 2; ++h) {
                vw.held[h] = g_held[h];
                vw.in_reach[h] = g_near[h];
                if (g_held[h]) vw.hold[h] = vw.stick_top;
            }
        }
        g_view    = vw;
        g_view_ok = true;
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
            o.stick_range = std::clamp(num(0.08f), 0.03f, 0.25f);
            spdlog::info("[Drive] drivestickrange {:.3f} m", o.stick_range);
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
        } else if (cmd == "driveready") {
            o.ready = arg == "off" || arg == "0" ? 0 : arg == "lb" || arg == "2" ? 2 : 1;
            spdlog::info("[Drive] driveready {}", o.ready == 0 ? "off: the right grip does nothing while driving (away from the wheel)"
                                                  : o.ready == 2 ? "lb (test): driving, a right grip away from the wheel taps LB (the game's shoot, which draws the sidearm first)"
                                                                 : "y: driving, a right grip away from the wheel taps Y (the game's change weapon: the sidearm drawn, no shot); the right trigger shoots");
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
        hint("On (default): driving, right A is the throttle, left X brakes and reverses while held, and a quick second press of left X is the handbrake "
             "(held as long as you hold it). The triggers, grips and right stick no longer accelerate or brake; the right trigger shoots. Right A no longer "
             "changes seat while you drive. Helicopters and planes: unchanged. Off: as before. Command drivebuttons on/off.");
        if (bt) {
            int ms = o.dbl_ms;
            if (ImGui::SliderInt("Double press within (ms)##drivebuttonsdbl", &ms, 150, 800)) cmds += fmt::format("drivebuttonsdbl {}\n", ms);
            hint("The second X press counts as the handbrake when it comes this soon after a short first press. Default 350. Command drivebuttonsdbl ms.");
        }
        int mm = o.menu;
        if (ImGui::Combo("Menu button: short press map, long press pause menu##drivemenu", &mm, "Off\0In vehicles\0Everywhere\0")) {
            static const char* k[]{ "off", "vehicle", "all" };
            cmds += fmt::format("drivemenu {}\n", k[std::clamp(mm, 0, 2)]);
        }
        hint("Everywhere (default): on foot and in any vehicle seat, a short press of the menu button opens the map (when you let go) and holding it opens "
             "the pause menu (save, load, options); a short press closes that pause menu again. Left grip + menu still opens the map at once. In vehicles: "
             "only there, on foot as before (menu = pause menu, left grip + menu = map). Off: as before everywhere. Command drivemenu all/vehicle/off.");
        if (mm != 0) {
            int mh = o.menu_hold;
            if (ImGui::SliderInt("Long press from (ms)##drivemenuhold", &mh, 200, 1500)) cmds += fmt::format("drivemenuhold {}\n", mh);
            hint("A press held this long opens the pause menu; a shorter one opens the map. Default 500. Command drivemenuhold ms.");
        }
        bool hl = o.heli;
        if (ImGui::Checkbox("Helicopter: gentle right stick##driveheli", &hl)) cmds += hl ? "driveheli on\n" : "driveheli off\n";
        hint("On (default): piloting a helicopter, the right stick turns and pitches more slowly, small pushes do little, and it eases in and out. "
             "Off: as before. Command driveheli on/off.");
        if (hl) {
            float hg = o.heli_gain * 100.0f;
            if (ImGui::SliderFloat("Right stick at full push (% of the game's)##driveheligain", &hg, 5.0f, 100.0f, "%.0f")) cmds += fmt::format("driveheligain {:.2f}\n", hg * 0.01f);
            hint("Default 40 %. Lower: slower turns. Command driveheligain 0.05-1.");
            float hc = o.heli_curve;
            if (ImGui::SliderFloat("Curve##drivehelicurve", &hc, 1.0f, 4.0f, "%.1f")) cmds += fmt::format("drivehelicurve {:.1f}\n", hc);
            hint("Default 2: half a push gives a quarter of the rate. 1: straight. Higher: even gentler near the centre. Command drivehelicurve 1-4.");
            float hs = o.heli_smooth;
            if (ImGui::SliderFloat("Smoothing (s)##drivehelismooth", &hs, 0.0f, 1.5f, "%.2f")) cmds += fmt::format("drivehelismooth {:.2f}\n", hs);
            hint("Default 0.30 s: how long the stick takes to ease in and out. 0: none. Command drivehelismooth s.");
        }
        bool ps = o.plane_stick;
        if (ImGui::Checkbox("Planes: the right stick moves the camera##driveplanestick", &ps)) cmds += ps ? "driveplanestick on\n" : "driveplanestick off\n";
        hint("Off (default): in a plane the right stick (and turning the hand on the gesture stick) sends nothing to the game, so it never moves the camera. "
             "Helicopters keep the right stick. Command driveplanestick on/off.");
        int rd = o.ready;
        if (ImGui::Combo("Right grip while driving##driveready", &rd, "Nothing\0Readies the sidearm (change weapon)\0Readies the sidearm (shoot button, test)\0")) {
            static const char* k[]{ "off", "y", "lb" };
            cmds += fmt::format("driveready {}\n", k[std::clamp(rd, 0, 2)]);
        }
        hint("Readies the sidearm (default): in a car, quad, bike or boat, pressing the right grip away from the wheel draws the sidearm (the game's change "
             "weapon), and keeps it out while you hold the grip; the right trigger then shoots at once. A grip at the wheel still holds the wheel (gesture "
             "driving). The shoot-button choice (test) presses the game's shoot instead, which may fire once. Nothing: as before. Command driveready y/lb/off.");
        int hm = o.hands;
        if (ImGui::Combo("Hands while driving##drivehands", &hm, "Hidden\0The game's hands on the wheel or stick\0The whole body (test)\0")) {
            static const char* k[]{ "off", "hands", "all" };
            cmds += fmt::format("drivehands {}\n", k[std::clamp(hm, 0, 2)]);
        }
        hint("The game's hands (default): in the driver's or pilot's seat the hands the game animates on the wheel, handlebars or stick are drawn, the rest "
             "of your body stays hidden. The whole body: everything the game draws of you (to compare). Hidden: as before. Command drivehands hands/all/off.");
        if (hm != 1) return;
        bool dd = o.hand.direct;
        if (ImGui::Checkbox("Always draw the bare arms (test)##drivehandsdirect", &dd)) cmds += dd ? "drivehandsdirect on\n" : "drivehandsdirect off\n";
        hint("Off (default): the skin of the arms and hands is drawn as the game asks. On: in the driver's seat it is drawn once whatever the game's own "
             "culling decided. Turn it on only if the hands show as arm hair alone. Command drivehandsdirect on/off.");
        bool ot = o.hand.ontop;
        if (ImGui::Checkbox("Draw the bare arms over everything (test)##drivehandsontop", &ot)) cmds += ot ? "drivehandsontop on\n" : "drivehandsontop off\n";
        hint("Off (default). On: in the driver's seat the skin of your hands is drawn in front of everything, even what should hide it. A test: if your "
             "hands show only with it on, something in front of them covers them. Command drivehandsontop on/off.");
        int sp = o.hand.space + 1;
        if (ImGui::Combo("Where the bare arms are drawn from##drivehandsspace", &sp, "Found each drive\0Their own object transform (test)\0The arm hair's transform (test)\0World (test)\0World less the camera (test)\0The camera's view (test)\0")) {
            static const char* k[]{ "auto", "object", "hair", "world", "camera", "view" };
            cmds += fmt::format("drivehandsspace {}\n", k[std::clamp(sp, 0, 5)]);
        }
        hint("Found each drive (default): when you sit in, your hand's skin is set against the arm hair the game draws over it, and the way that puts "
             "it on the hair is used to place the hands on the controllers (the hands appear a moment after you sit). The others force one way, to "
             "compare. Command drivehandsspace auto/object/hair/world/camera/view.");
        bool hg = o.hand.grab;
        if (ImGui::Checkbox("Your VR hands, stuck to the wheel when you grip it##drivehandsgrab", &hg)) cmds += hg ? "drivehandsgrab on\n" : "drivehandsgrab off\n";
        hint("On (default): driving, your hands follow the controllers as on foot (grip and trigger close the fingers). With gesture driving on (F11 "
             "Experimental), a hand whose grip holds the wheel, handlebars or stick sticks to it and turns with it until you let go. Off: the game's own hands, "
             "where the game animates them. Command drivehandsgrab on/off.");
        if (!hg) return;
        if (ImGui::TreeNode("Fit the hands to the controllers##drivehandfit")) {
            float t[3]{ o.hand.turn.x, o.hand.turn.y, o.hand.turn.z };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Pitch (deg)##dhtp", &t[0], -180.0f, 180.0f, "%.0f");
            moved |= ImGui::SliderFloat("Yaw (deg)##dhty", &t[1], -180.0f, 180.0f, "%.0f");
            moved |= ImGui::SliderFloat("Roll (deg)##dhtr", &t[2], -180.0f, 180.0f, "%.0f");
            if (moved) cmds += fmt::format("drivehandturn {:.0f} {:.0f} {:.0f}\n", t[0], t[1], t[2]);
            hint("Turns the free hands in the controller's axes (the left hand's yaw and roll mirrored). Default 0 0 0. Command drivehandturn pitch yaw roll.");
            float d[3]{ o.hand.shift.x * 100.0f, o.hand.shift.y * 100.0f, o.hand.shift.z * 100.0f };
            moved = false;
            moved |= ImGui::SliderFloat("Right (cm)##dhsx", &d[0], -30.0f, 30.0f, "%.0f");
            moved |= ImGui::SliderFloat("Up (cm)##dhsy", &d[1], -30.0f, 30.0f, "%.0f");
            moved |= ImGui::SliderFloat("Back (cm)##dhsz", &d[2], -30.0f, 30.0f, "%.0f");
            if (moved) cmds += fmt::format("drivehandshift {:.3f} {:.3f} {:.3f}\n", d[0] * 0.01f, d[1] * 0.01f, d[2] * 0.01f);
            hint("Moves the free hands from the controllers (the left hand's right / left mirrored). With the sidearm drawn the right hand sits where the game "
                 "holds it on the sidearm instead. Default 0 0 0. Command drivehandshift x y z (m).");
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
        hint("While driving, grab the wheel (cars, trucks, boats), the handlebars (quads, jetskis) or the stick (helicopters, planes) with a grip and turn it. "
             "Either hand alone steers; both together share. A hand holds until you let go of its grip. The left trigger is the throttle; the right "
             "trigger shoots while the right hand is off the wheel, and brakes / reverses while it holds it (cars, quads, bikes and boats with \"A accelerates, X "
             "brakes\" on, F11 Layout > Vehicles: A and X instead, and the right trigger does nothing while it holds the wheel). The left stick still steers too. Command drive on/off.");
        if (!v) return;
        ImGui::TextWrapped("%s", status().c_str());
        int st = force < 0 ? 0 : force == kWheel ? 1 : force == kBars ? 2 : force == kStick ? 3 : 4;
        if (ImGui::Combo("This vehicle##drivestyle", &st, "Automatic\0Wheel\0Handlebars\0Stick\0Nothing to hold\0")) {
            static const char* k[]{ "auto", "wheel", "bars", "stick", "off" };
            cmds += fmt::format("drivestyle {}\n", k[st]);
        }
        hint("Automatic: helicopters and planes get the stick, motorcycles, quads and (spawned from F11) jetskis the handlebars, the rest the wheel. "
             "A choice here lasts until you leave the vehicle. Command drivestyle auto/wheel/bars/stick/off.");
        float qs = o.quad_seat * 100.0f;
        if (ImGui::SliderFloat("Quads: handlebars when you sit this near the middle (cm)##drivequadseat", &qs, 0.0f, 35.0f, "%.0f")) cmds += fmt::format("drivequadseat {:.2f}\n", qs * 0.01f);
        hint("The game calls a quad a car. A quad's driver sits on its middle line, a car's about 40 cm to the side, so a vehicle where you sit within "
             "this of the middle gets the handlebars. Default 15 cm. 0: off (the wheel, as before). Command drivequadseat m.");
        bool sh = o.show;
        if (ImGui::Checkbox("Show the wheel / handlebars / stick##driveshow", &sh)) cmds += sh ? "driveshow on\n" : "driveshow off\n";
        hint("Drawn faintly where you can grab it; brighter where a hand is near or holds it. Command driveshow on/off.");
        bool rb = o.right_brake;
        if (ImGui::Checkbox("Right hand on the wheel: its trigger brakes##driverightbrake", &rb)) cmds += rb ? "driverightbrake on\n" : "driverightbrake off\n";
        hint("On: while the right hand holds the wheel or handlebars, its trigger brakes and reverses; off the wheel it shoots. Off: it always shoots. Command driverightbrake on/off.");

        bool re = o.react;
        if (ImGui::Checkbox("Very reactive wheel##drivereact", &re)) cmds += re ? "drivereact on\n" : "drivereact off\n";
        hint("On (default): the car follows the wheel closely. Half the turn gives full steering, and the smallest turn already steers (the game's own dead "
             "zone on the stick is skipped). The handlebars get the dead zone part only. Off: as before 6 Nov. Command drivereact on/off.");
        if (re) {
            float rg = o.react_gain;
            if (ImGui::SliderFloat("Steering per wheel turn (x)##drivereactgain", &rg, 1.0f, 4.0f, "%.1f")) cmds += fmt::format("drivereactgain {:.2f}\n", rg);
            hint("2 (default): full steering at half the wheel's lock (45 deg). Higher: even less turn needed. Command drivereactgain x.");
            float rd = o.react_dead;
            if (ImGui::SliderFloat("Game dead zone skipped##drivereactdead", &rd, 0.0f, 0.45f, "%.2f")) cmds += fmt::format("drivereactdead {:.2f}\n", rd);
            hint("Default 0.20. If the car twitches when you barely touch the wheel, lower it; if small turns do nothing, raise it. Command drivereactdead value.");
            float rl = o.react_lead * 1000.0f;
            if (ImGui::SliderFloat("Lead (ms, test)##drivereactlead", &rl, 0.0f, 150.0f, "%.0f")) cmds += fmt::format("drivereactlead {:.3f}\n", rl * 0.001f);
            hint("0 (default): none. A little (30-60 ms) makes the car start turning sooner as you turn the wheel; too much makes it nervous. Command drivereactlead s.");
            float rb2 = o.react_boost;
            if (ImGui::SliderFloat("Steering boost##drivereactboost", &rb2, 0.0f, 4.0f, "%.1f")) cmds += fmt::format("drivereactboost {:.2f}\n", rb2);
            hint("1.5 (default): the game turns its wheels slowly towards what you steer; this pushes them past it while they catch up, so the car answers "
                 "the wheel sooner (and straightens sooner when you let go). Higher: quicker; if the car overshoots or wobbles, lower it. 0: off (as before "
                 "7 Nov). Command drivereactboost x.");
            float rt2 = o.react_tau * 1000.0f;
            if (ImGui::SliderFloat("Boost: the game's steering delay (ms)##drivereacttau", &rt2, 50.0f, 800.0f, "%.0f")) cmds += fmt::format("drivereacttau {:.3f}\n", rt2 * 0.001f);
            hint("250 (default): how slowly the boost takes the game's own steering to follow. Longer: a stronger, longer push; shorter: a lighter one. "
                 "Command drivereacttau s.");
        }
        if (ImGui::TreeNode("Wheel##drivewheelnode")) {
            float p[4]{ o.wheel_fwd * 100.0f, o.wheel_down * 100.0f, o.wheel_r * 100.0f, o.wheel_tilt };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dwf", &p[0], 10.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dwd", &p[1], -20.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Radius (cm)##dwr", &p[2], 8.0f, 35.0f, "%.0f");
            moved |= ImGui::SliderFloat("Tilt (deg)##dwt", &p[3], -30.0f, 80.0f, "%.0f");
            if (moved) cmds += fmt::format("drivewheel {:.3f} {:.3f} {:.3f} {:.0f}\n", p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f, p[3]);
            hint("From the recentred head (sit straight and recentre in the vehicle). Default 38 cm ahead, 32 cm down, radius 18 cm, tilted 25 deg. Command drivewheel ahead down radius tilt (m, deg).");
            float lk = o.wheel_lock;
            if (ImGui::SliderFloat("Turn for full lock (deg)##dwl", &lk, 20.0f, 360.0f, "%.0f")) cmds += fmt::format("drivewheellock {:.0f}\n", lk);
            hint("How far the wheel turns for the game's full steering. Default 90. Command drivewheellock deg.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Handlebars##drivebarsnode")) {
            float p[3]{ o.bars_fwd * 100.0f, o.bars_down * 100.0f, o.bars_w * 100.0f };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dbf", &p[0], 10.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dbd", &p[1], -20.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Half width (cm)##dbw", &p[2], 10.0f, 45.0f, "%.0f");
            if (moved) cmds += fmt::format("drivebars {:.3f} {:.3f} {:.3f}\n", p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f);
            hint("Default 42 cm ahead, 45 cm down, grips 28 cm each side. Command drivebars ahead down halfwidth (m).");
            float lk = o.bars_lock;
            if (ImGui::SliderFloat("Turn for full lock (deg)##dbl", &lk, 10.0f, 90.0f, "%.0f")) cmds += fmt::format("drivebarslock {:.0f}\n", lk);
            hint("Default 30. Command drivebarslock deg.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Stick (helicopter, plane)##drivesticknode")) {
            float p[2]{ o.stick_fwd * 100.0f, o.stick_down * 100.0f };
            bool  moved = false;
            moved |= ImGui::SliderFloat("Ahead of the eyes (cm)##dsf", &p[0], 5.0f, 90.0f, "%.0f");
            moved |= ImGui::SliderFloat("Below the eyes (cm)##dsd", &p[1], -20.0f, 100.0f, "%.0f");
            if (moved) cmds += fmt::format("drivestick {:.3f} {:.3f}\n", p[0] * 0.01f, p[1] * 0.01f);
            hint("Where the stick's top is. Default 30 cm ahead, 50 cm down. Command drivestick ahead down (m).");
            float rg = o.stick_range * 100.0f;
            if (ImGui::SliderFloat("Hand move for full stick (cm)##dsr", &rg, 3.0f, 25.0f, "%.0f")) cmds += fmt::format("drivestickrange {:.3f}\n", rg * 0.01f);
            hint("Measured from where you took the stick: forward / back and left / right are the left stick. Default 8 cm. Command drivestickrange m.");
            bool tw = o.twist;
            if (ImGui::Checkbox("Turn the hand to turn the aircraft##drivetwist", &tw)) cmds += tw ? "drivetwist on\n" : "drivetwist off\n";
            hint("Turning the hand about the vertical while holding the stick is the right stick's left / right (helicopter turn). Command drivetwist on/off.");
            float td = o.twist_deg;
            if (ImGui::SliderFloat("Hand turn for full (deg)##dstd", &td, 10.0f, 120.0f, "%.0f")) cmds += fmt::format("drivetwistdeg {:.0f}\n", td);
            ImGui::TreePop();
        }
        float g = o.grab * 100.0f;
        if (ImGui::SliderFloat("Reach to grab (cm)##drivegrab", &g, 3.0f, 30.0f, "%.0f")) cmds += fmt::format("drivegrab {:.3f}\n", g * 0.01f);
        hint("A grip pressed this close to the wheel's rim, the handlebars or the stick's top takes it. Default 10 cm. Command drivegrab m.");
        float c = o.centre_s;
        if (ImGui::SliderFloat("Back to straight when let go (s)##drivecentre", &c, 0.0f, 3.0f, "%.2f")) cmds += fmt::format("drivecentre {:.2f}\n", c);
        hint("0: at once. Default 0.3 s. Command drivecentre s.");
    }
}
