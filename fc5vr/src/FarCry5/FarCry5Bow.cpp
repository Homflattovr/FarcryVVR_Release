// manualbow (see FarCry5Bow.h): the bow drawn by hand. Arrows from the quiver at the weapon hand's shoulder, nocked at the bow hand,
// the string pulled back with the weapon hand's grip held; the game's fire (pad RT) is held while the string is drawn and let go with
// the grip, so the game's own bow draws and looses the arrow. Phases:
//   Empty   -> Arrow    the grip pressed in the quiver (an arrow in the hand while the grip lasts)
//   Arrow   -> Drawing  the arrow brought within bownockzone of the nock point, grip still held (nocked: that grip holds the string;
//                        the distance then is the string's rest, d0)
//   Arrow   -> Empty    the grip let go away from the nock (the arrow dropped)
//   Drawing -> Nocked   the grip let go before the string was pulled bowstart past its rest (the arrow stays on the bow)
//   Nocked  -> Drawing  the grip pressed at the nock again (1.5 x bownockzone)
//   Drawing -> Loose    the grip let go once RT is held (pulled bowstart past the rest): RT kept until it has been held bowholdms, then let go
//   Loose   -> Empty    RT let go: the game looses the arrow
#include "FarCry5Bow.h"
#include "FarCry5Gesture.h"
#include "FarCry5GameWeapon.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace fc5::bow
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr int kString = gesture::kWeaponHand;   // the hand on the string (takes the arrows)
    constexpr int kBow    = gesture::kOtherHand;    // the hand holding the bow

    // Defaults: an adult's draw puts the string hand ~0.55-0.65 m behind the bow hand; the controllers' grip points sit a few cm
    // inside that, so 0.50 m reads as a full draw for most people and bowfull moves it. RT goes once the string is pulled 0.10 m past
    // where the arrow was nocked (d0): a string merely touched does not start the game's draw.
    // Nock zone 0.22 m (0.12 until 1 Nov 21:13, the author's first headset try: 11 arrows taken from the quiver, none nocked; let go
    // 0.23-0.61 m from the nock. Two controllers held fist to fist keep their tracked points ~0.15 m apart, so 0.12 m was out of reach).
    constexpr float kStartDefault = 0.10f, kFullDefault = 0.50f, kNockZoneDefault = 0.22f, kBuzzDefault = 0.7f;
    constexpr int   kHoldDefault  = 300;   // ms: the game's draw needs a moment before a release looses an arrow (not measured yet)
    constexpr float kNockDefault[3]{ 0.0f, 0.05f, 0.0f };   // right, up, back of the bow hand's controller (m): just above the fist

    std::atomic<bool>  g_on{ false };
    std::atomic<float> g_start{ kStartDefault };
    std::atomic<float> g_full{ kFullDefault };
    std::atomic<float> g_nock_zone{ kNockZoneDefault };
    std::atomic<int>   g_hold_ms{ kHoldDefault };
    std::atomic<float> g_buzz{ kBuzzDefault };
    std::atomic<float> g_nock[3]{ kNockDefault[0], kNockDefault[1], kNockDefault[2] };
    std::atomic<bool>  g_log{ true };   // bowlog: one line per phase change (a few per shot)
    std::atomic<bool>  g_arrow_on{ true };   // bowarrow: the arrow drawn over the eyes
    constexpr float    kArrowLen = 0.75f;    // a hunting arrow, nock to point (m)
    constexpr float    kNockBack = 0.17f;    // a nocked arrow's tail behind the nock point: on the string at rest (brace height, m)
    std::mutex         g_view_mtx;
    ArrowView          g_view{};
    // bowarrowmodel: the game's own arrow (FarCry5BowArrow.cpp).
    std::atomic<bool>  g_model_on{ true };
    std::atomic<bool>  g_model_shown{ false };   // the game's arrow stands for the drawn one now (set each frame by the render thread)
    constexpr float    kArrowHandDefault[3]{ 0.0f, 0.0f, 0.03f };   // the arrow's nock end from the string hand's controller: 3 cm behind its grip point
    std::atomic<float> g_arrow_hand[3]{ kArrowHandDefault[0], kArrowHandDefault[1], kArrowHandDefault[2] };
    std::atomic<int>   g_force{ -1 };            // bowforce (test): the arrow's phase as shown, -1 the real one
    std::atomic<int64_t> g_scan_until{ 0 };      // bowscan (test): steady clock ticks
    std::atomic<int>   g_test_hold{ 0 };         // bowshoot (test): RT held this long (ms) for one shot
    // bowdrawtest MS (test, the simulator; 14 Nov, the bowfine check): bowshoot goes straight to Loose, so no simulator run ever held the
    // string by hand (Drawing), the only phase in which FarCry5BowArrow.cpp poses the limbs, the string and the arrow. One manual shot with
    // the string hand's inputs made up: an arrow in the hand for 1 s (half a metre from the nock), the hand on the nock (nocked, the grip
    // holds the string), pulled back along the bow's own axis to 5 cm past full draw in 1 s, held, the grip let go MS ms after the nock.
    // The phases, RT and the loose are the real ones; the string's draw is the hand's pull (test_draw()).
    std::atomic<int>   g_test_draw{ 0 };         // the string held this long (ms), 0 none asked
    std::atomic<bool>  g_test_draw_on{ false };  // under way
    clock::time_point  g_test_draw_t{};          // when the hand reaches the nock (under g_mtx)
    std::atomic<uint32_t> g_shots{ 0 };
    // bowhandstring (author 2 Nov 07:51): while the game draws its bow (RT held by the
    // string pull) the right glove is left where the game holds it, on its string, instead of on the controller.
    std::atomic<bool>  g_hand_string{ true };
    std::atomic<bool>  g_rt_now{ false };   // RT held for the draw (g_rt, for the render thread)
    std::atomic<bool>  g_upright{ true };   // bowupright
    std::atomic<bool>  g_flip{ false };     // bowflip
    std::atomic<bool>  g_upright_keep{ true };   // bowuprightkeep: each bow's upright turn kept across sessions (bin\fc5vr_bowupright.txt)
    std::atomic<bool>  g_upright_wait{ true };   // bowuprightwait: the upright turn measured only once the bow is settled in the hand (FarCry5Bow.h)
    // bowpull (author 2 Nov 09:51):
    // the string and the arrow follow the string hand (FarCry5BowArrow.cpp poses them between the game's rest and full draw).
    std::atomic<bool>  g_pull{ true };
    std::atomic<bool>  g_pull_active{ false };   // the bow's rest and full draw are known: the string follows the hand (render thread)
    std::atomic<float> g_pull_t{ -1.0f };        // how far the string is drawn now (0 rest .. 1 full), < 0 unknown (render thread)
    std::atomic<float> g_pull_frac{ 0.0f };      // the hand's pull from where it took the string to bowfull (0..1), this poll
    // bowstringhand (author 2 Nov 11:55 video): where the string (the arrow's nock end) sits from the
    // string hand's controller while drawing. bowarrowhand's 3 cm behind the grip point put it across the back of the fist; the fingers
    // that hook a string are in front of it.
    constexpr float    kStringHandDefault[3]{ 0.0f, 0.0f, -0.05f };
    std::atomic<float> g_string_hand[3]{ kStringHandDefault[0], kStringHandDefault[1], kStringHandDefault[2] };
    std::atomic<bool>  g_pull_forget{ false };
    constexpr float    kMinDrawDefault = 0.80f;
    std::atomic<float> g_min_draw{ kMinDrawDefault };   // bowmindraw
    std::atomic<int>   g_pin{ 2 };                      // bowpin: 0 off, 1 on the sight's pin, 2 where the arrow lands
    std::atomic<bool>  g_sight_aim{ true };             // bowsightaim
    std::atomic<bool>  g_sight_eye{ true };             // bowsighteye
    std::atomic<bool>  g_dot_one_eye{ true };           // bowdoteye
    constexpr float    kSteadyHzDefault   = 0.6f;       // bowsteadyhz
    constexpr int      kSteadyHoldDefault = 150;        // bowsteadyhold (ms)
    std::atomic<bool>  g_steady{ true };                // bowsteady
    std::atomic<float> g_steady_hz{ kSteadyHzDefault };
    std::atomic<int>   g_steady_hold{ kSteadyHoldDefault };
    // bowsteadyaim (author 6 Nov video 1:18-1:25 holding the drawn bow on a target): in the headset bowsteady was 1.0-1.1
    // times steadier (fc5vr_log 6 Nov 13:19-14:44, 19 draws: its filter opened for a "move" in 22-100 % of readings, the steadied bow 0.75-1.9 deg
    // off the hands; the simulator's test shake, aim only, gave 10 times). Its move test counted the bow hand's roll and its sway with the body
    // (vector speed averaged at only 3 Hz, the 2 deg catch-up on the whole turn, 3 cm/s on the bow's position): a real hand's wrist roll and body
    // sway kept it open. On: the aim's own swing decides (roll left out, speed averaged at 1.5 Hz, 10 deg/s, catch-up past 3 deg of aim), the roll
    // follows at 2 Hz at least, and the bow's position is steadied against the head (the sight line is eye -> pin: body sway moves both and is
    // no reason to open, and a pin steadied in the room while the eye sways turned the line).
    std::atomic<bool>  g_steady_aim{ true };
    constexpr float    kAimSpeedHz   = 1.5f;    // the swing and position speeds averaged at this cutoff (tremor cancels, a real move adds up)
    constexpr float    kAimMoveDps   = 10.0f;   // a swing faster than this opens the aim: 0.25 Hz per deg/s past it
    constexpr float    kAimBehindDeg = 3.0f;    // the steadied aim behind the hands' by more than this opens it: 2 Hz per deg past it
    constexpr float    kRollMinHz    = 2.0f;    // the bow's roll (no effect on the aim) follows the bow hand at least this fast
    constexpr float    kPosMoveCms   = 4.0f;    // the bow against the head moving faster than this opens the position: 0.5 Hz per cm/s past it
    constexpr float    kPosBehindCm  = 2.0f;    // the steadied position behind the hands' by more than this opens it: 1 Hz per cm past it
    std::atomic<bool>  g_no_scope{ true };              // bownoscope
    // Author 5 Nov 22:47 (the arrow still shaking, a red dot with no arrow nocked, the dot moving while drawn, the bow on the sling):
    std::atomic<bool>  g_steady_arrow{ true };          // bowsteadyarrow: the drawn arrow and string steadied like the bow
    std::atomic<bool>  g_dot_arrow{ true };             // bowdotarrow: the bow's dot (and laser) only with an arrow on the bow
    std::atomic<bool>  g_pin_steady{ true };            // bowpinsteady: the sight line from this frame's bow and eye, the pin kept in the bow's axes
    std::atomic<bool>  g_sling{ true };                 // bowsling: the bow on the sling, taken with the left hand
    std::atomic<bool>  g_quiver_grip{ true };           // bowquivergrip: the right shoulder is the quiver only while the left grip is held
    std::atomic<bool>  g_pull_keep{ true };             // bowpullkeep: the learnt rest and full draw not taken again from the game mid-draw
    // bowslinggrab (author 6 Nov 15:59, build AE016CEC): the slung
    // bow taken with the left grip anywhere along it (within bowslinggrabzone of the line through its grip along its limbs), not only within the
    // sling zone round its grip; a light tick on the left controller when the free left hand comes onto it. The right grip: the sling zone only.
    std::atomic<bool>  g_sling_grab{ true };
    std::atomic<bool>  g_right_hand{ true };   // bowrighthand (FarCry5Bow.h): the bow taken with the right grip stays in the right hand
    std::atomic<bool>  g_fine{ true };         // bowfine (FarCry5Bow.h): the drawn bow's parts placed without map-size single floats
    std::atomic<bool>  g_pin_mid{ true };      // bowpinmid (FarCry5Bow.h): a ring sight's dot on the ring's middle
    std::atomic<float> g_sling_grab_zone{ 0.12f };
    // bowslinghold yaw pitch roll (deg): the bow's turn on the sling. Default 0 0 90 (6 Nov sweep, testruns\2026-11-06_bowslinghold): upright
    // (0 0 0) its upper limb and cam stood in front of the face looking ahead; rolled 90 deg (its limbs across, about as the game holds it)
    // only a cam's edge shows at the bottom right, as with the slung guns (0 0 -45 hid it entirely, 0 -60 0 too).
    constexpr float    kSlingHoldDefault[3]{ 0.0f, 0.0f, 90.0f };
    std::atomic<float> g_sling_hold[3]{ kSlingHoldDefault[0], kSlingHoldDefault[1], kSlingHoldDefault[2] };
    std::atomic<bool>  g_sling_body{ true };       // bowslingbody (FarCry5Bow.h)
    std::atomic<float> g_sling_body_deg{ 35.0f };  // bowslingbodyangle
    std::atomic<int64_t> g_loose_ms{ 0 };               // the last shot (Loose -> Empty), steady clock ms; 0 none
    std::atomic<int>   g_cancel_key{ 0x13 };            // bowcancelkey: R
    bool               g_cancel = false;                // a short draw being cancelled: RT still held until g_cancel_t + 300 ms
    clock::time_point  g_cancel_t{};   // bowpullforget: the learnt rest and full draws dropped (render thread takes it)
    std::atomic<float> g_hold[3]{ 0.0f, 0.0f, 0.0f };   // bowhold yaw pitch roll (deg)
    // bowhandpos (author 2 Nov 12:33): the bow and the left hand on it 2 cm lower by default.
    constexpr float    kHandPosDefault[3]{ 0.0f, -0.02f, 0.0f };
    std::atomic<float> g_hand_pos[3]{ kHandPosDefault[0], kHandPosDefault[1], kHandPosDefault[2] };

    // bowgame on|off (1 Nov, saved, default on): a weapon the game's own data calls a bow (class 15: FarCry5GameWeapon.cpp, research
    // 2026-11-01_weapon_data.md) is a bow without being ticked in bowweapons.
    std::atomic<bool>  g_game{ true };
    std::mutex         g_list_mtx;      // bowweapons (the pad poll reads it, F11 and the commands write it), bowarrowmesh
    std::set<uint32_t> g_bows;
    // bowarrowmesh: per bow, the index counts of its arrow's draws. The author's compound bow 51543 known from his 2 Nov 08:47 headset run (learnt at
    // its first shot: 3693 indices, 80 cm) and the builder's 1 Nov simulator run; other bows are found by shape or at their first shot.
    std::map<uint32_t, std::vector<uint32_t>> g_arrow_parts{ { 51543u, { 3693u } } };

    // The state: written on the pad poll (under g_mtx), read by update_motion and F11 through the atomics.
    std::mutex          g_mtx;
    std::atomic<int>    g_phase{ int(Phase::Off) };
    std::atomic<float>  g_draw{ -1.0f };
    std::atomic<float>  g_quiver{ -1.0f };
    std::atomic<bool>   g_in_hand{ false };
    bool                g_rt = false;          // RT held for the draw
    clock::time_point   g_rt_t{};              // since when
    bool                g_full_said = false;   // the full-draw buzz given (again once the string comes 5 cm forward)
    bool                g_quiver_in = false;   // the string hand in the quiver (entry buzz)
    float               g_draw_max  = 0.0f;    // the longest draw of this string pull (log)
    float               g_d0        = 0.0f;    // the string hand's distance from the nock when the string was taken (its rest)
    float               g_arrow_min = 1e9f;    // the arrow's closest approach to the nock while in the hand (log)
    clock::time_point   g_phase_t{};

    const char* phase_name(Phase p)
    {
        static const char* const kName[]{ "off", "empty", "arrow in hand", "nocked", "drawing", "loose" };
        const int i = int(p);
        return i >= 0 && i < 6 ? kName[i] : "?";
    }

    void set_phase(Phase p, const clock::time_point now, const std::string& why)
    {
        if (int(p) == g_phase.load()) return;
        if (g_log.load())
            spdlog::info("[Bow] {} -> {} after {} ms: {}", phase_name(Phase(g_phase.load())), phase_name(p),
                         g_phase_t == clock::time_point{} ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(now - g_phase_t).count(), why);
        g_phase   = int(p);
        g_phase_t = now;
    }

    void buzz(Out& o, int hand, float amp, float sec)
    {
        if (amp > o.buzz[hand]) {
            o.buzz[hand]   = amp;
            o.buzz_s[hand] = sec;
        }
    }

    std::string parts_line()   // bowarrowmesh's saved form (call with g_list_mtx held)
    {
        std::string s = "bowarrowmesh";
        for (const auto& [w, v] : g_arrow_parts) {
            if (v.empty()) continue;
            s += fmt::format(" {}:", w);
            for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
        }
        if (s == "bowarrowmesh") s += " none";
        return s;
    }

    std::string list_text()
    {
        std::lock_guard _{ g_list_mtx };
        std::string s;
        for (const uint32_t w : g_bows) s += (s.empty() ? "" : " ") + std::to_string(w);
        return s;
    }
}

glm::vec3 nock_point(const glm::vec3& bow_pos, const glm::mat3& bow_rot, bool mirror)
{
    const glm::vec3 o{ (mirror ? -1.0f : 1.0f) * g_nock[0].load(), g_nock[1].load(), g_nock[2].load() };
    return bow_pos + bow_rot * o;
}

void get_nock(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_nock[i].load();
}

bool  on() { return g_on.load(); }
bool  in_hand() { return g_in_hand.load(); }
Phase phase() { return Phase(g_phase.load()); }
bool  test_draw() { return g_test_draw_on.load(); }
float draw_m() { return g_draw.load(); }
float full_m() { return g_full.load(); }
float start_m() { return g_start.load(); }

bool is_bow(uint32_t sig)
{
    if (sig == 0) return false;
    {
        std::lock_guard _{ g_list_mtx };
        if (g_bows.contains(sig)) return true;
    }
    return game_bow(sig);
}

bool game_on() { return g_game.load(); }

bool game_bow(uint32_t sig)
{
    if (!g_game.load()) return false;
    const auto* w = fc5::gameweapon::info(sig);
    return w && w->cls == 15;
}

void update(const In& in_now, Out& out)
{
    std::lock_guard _{ g_mtx };
    In         in  = in_now;   // (bowdrawtest makes up the string hand's part of it)
    const auto now = clock::now();
    Phase      ph  = Phase(g_phase.load());
    if (!(in.active && g_on.load())) {
        g_test_draw    = 0;
        g_test_draw_on = false;
        // Not a bow any more (a weapon switch), a vehicle, a menu, the flat screen: the string let go and the arrow gone.
        if (ph != Phase::Off) set_phase(Phase::Off, now, g_rt ? "the bow is no longer in hand: RT let go" : "the bow is no longer in hand");
        g_rt       = false;
        g_rt_now   = false;
        g_cancel   = false;
        g_quiver_in = false;
        g_in_hand  = false;
        g_draw     = -1.0f;
        g_quiver   = -1.0f;
        std::lock_guard v{ g_view_mtx };
        g_view = ArrowView{};
        return;
    }
    g_in_hand = true;
    if (ph == Phase::Off) {
        set_phase(Phase::Empty, now, "a bow in hand: the weapon hand's shoulder is the quiver");
        ph = Phase::Empty;
    }
    // bowdrawtest: the string hand's inputs made up for one manual shot (see g_test_draw).
    if (const int td = g_test_draw.load(); td > 0 && in.bow_ok) {
        bool press = false;
        if (!g_test_draw_on.load() && (ph == Phase::Empty || ph == Phase::Nocked) && !g_rt && !g_cancel) {
            g_test_draw_on = true;
            if (ph == Phase::Empty) {
                g_test_draw_t = now + std::chrono::milliseconds(1000);
                g_arrow_min   = 1e9f;
                set_phase(Phase::Arrow, now, fmt::format("bowdrawtest: an arrow in the hand for 1 s, then nocked, drawn in 1 s and let go after {} ms", td));
                ph = Phase::Arrow;
            } else {
                g_test_draw_t = now;   // an arrow already on the bow: the grip pressed on the nock
                press         = true;
            }
        }
        if (g_test_draw_on.load()) {
            const float     ms   = std::chrono::duration<float, std::milli>(now - g_test_draw_t).count();
            const float     full = std::max(g_full.load(), g_start.load() + 0.05f) + 0.05f;
            const glm::vec3 n0   = nock_point(in.bow_pos, in.bow_rot, in.mirror);
            in.hand    = ms < 0.0f ? n0 + glm::normalize(in.bow_rot[0]) * 0.5f : n0 + glm::normalize(in.bow_rot[2]) * (full * std::clamp(ms / 1000.0f, 0.0f, 1.0f));
            in.hand_ok = true;
            in.grip    = ms < float(td);
            in.pressed = press;
            if (!in.grip || (ph != Phase::Arrow && ph != Phase::Nocked && ph != Phase::Drawing)) {
                g_test_draw    = 0;
                g_test_draw_on = false;
            }
        }
    }
    const bool      track = in.hand_ok && in.bow_ok;
    const glm::vec3 nock  = nock_point(in.bow_pos, in.bow_rot, in.mirror);
    const float     d     = track ? glm::length(in.hand - nock) : -1.0f;
    g_draw   = d;
    g_quiver = in.quiver_dist;
    // The quiver's entry tick (the shoulder zone's own one is off while the bow is in hand): a free hand coming in.
    const bool qin = in.quiver_dist >= 0.0f && in.quiver_dist < in.quiver_radius * (g_quiver_in ? 1.15f : 1.0f);
    if (qin && !g_quiver_in && !in.grip && (ph == Phase::Empty || ph == Phase::Nocked)) buzz(out, kString, 0.25f, 0.03f);
    g_quiver_in = qin;

    // bowshoot (test, the simulator): one shot without the gestures, RT held bowshoot ms from now.
    if (const int th = g_test_hold.load(); th > 0 && !g_rt && (ph == Phase::Empty || ph == Phase::Nocked || ph == Phase::Arrow)) {
        g_rt   = true;
        g_rt_t = now;
        set_phase(Phase::Loose, now, fmt::format("bowshoot: RT held {} ms for a test shot", th));
        ph = Phase::Loose;
    }
    // bowmindraw: a short draw's cancel under way: the cancel key went at the release; pad X held its first 150 ms; RT let go after 300 ms.
    if (g_cancel) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_cancel_t).count();
        if (g_cancel_key.load() == 0 && ms < 150) out.buttons |= 0x4000;   // XINPUT_GAMEPAD_X
        if (ms >= 300) {
            g_cancel = false;
            g_rt     = false;
            if (g_log.load()) spdlog::info("[Bow] short draw cancelled: RT let go {} ms after the cancel, the arrow stays on the bow", ms);
        }
    }
    switch (ph) {
    case Phase::Empty:
    case Phase::Nocked:
        if (!in.pressed || !track || g_cancel) break;
        if (ph == Phase::Nocked && d < 1.5f * g_nock_zone.load()) {
            out.took_press = true;
            g_full_said    = false;
            g_draw_max     = d;
            g_d0           = d;
            buzz(out, kString, 0.3f, 0.03f);
            set_phase(Phase::Drawing, now, fmt::format("grip pressed {:.2f} m from the nock: the string held", d));
            ph = Phase::Drawing;
        } else if (qin) {
            out.took_press = true;
            if (ph == Phase::Nocked) {
                if (g_log.load()) spdlog::info("[Bow] grip pressed in the quiver ({:.2f} m) with an arrow already on the bow: nothing taken", in.quiver_dist);
                break;
            }
            buzz(out, kString, 0.6f, 0.06f);
            g_arrow_min = 1e9f;
            set_phase(Phase::Arrow, now, fmt::format("grip pressed in the quiver ({:.2f} m from its centre, zone {:.2f} m): an arrow in the hand", in.quiver_dist, in.quiver_radius));
            ph = Phase::Arrow;
        }
        break;
    case Phase::Arrow:
        out.grip_taken = true;
        if (track) g_arrow_min = std::min(g_arrow_min, d);
        if (!in.grip) {
            set_phase(Phase::Empty, now, fmt::format("grip let go {:.2f} m from the nock (closest {:.2f} m, nock zone {:.2f} m): the arrow dropped", d,
                                                     g_arrow_min < 1e8f ? g_arrow_min : -1.0f, g_nock_zone.load()));
            ph = Phase::Empty;
        } else if (track && d < g_nock_zone.load()) {
            g_full_said = false;
            g_draw_max  = d;
            g_d0        = d;
            buzz(out, kString, 0.45f, 0.05f);
            buzz(out, kBow, 0.45f, 0.05f);
            set_phase(Phase::Drawing, now, fmt::format("the arrow {:.2f} m from the nock: nocked, the grip holds the string", d));
            ph = Phase::Drawing;
        }
        break;
    case Phase::Drawing:
        out.grip_taken = true;
        if (in.grip) {
            if (track) {
                g_draw_max = std::max(g_draw_max, d);
                if (!g_rt && d >= g_d0 + g_start.load()) {
                    g_rt   = true;
                    g_rt_t = now;
                    if (g_log.load()) spdlog::info("[Bow] string {:.2f} m behind the nock, pulled {:.2f} m from where it was taken (bowstart {:.2f}): RT held, the game draws its bow", d,
                                                   d - g_d0, g_start.load());
                }
                const float full = std::max(g_full.load(), g_d0 + g_start.load() + 0.05f);
                g_pull_frac      = std::clamp((d - g_d0) / std::max(full - g_d0, 0.05f), 0.0f, 1.0f);
                // bowpull: the buzz when the string itself reaches full draw (it follows the hand), else at bowfull.
                const float pt      = g_pull_active.load() ? g_pull_t.load() : -1.0f;
                const bool  at_full = pt >= 0.0f ? pt >= 0.99f : d >= full;
                const bool  eased   = pt >= 0.0f ? pt < 0.90f : d < full - 0.05f;
                if (!g_full_said && at_full) {
                    // Author.
                    g_full_said  = true;
                    const float a = std::clamp(g_buzz.load(), 0.1f, 1.0f);
                    buzz(out, kString, a, 0.07f);
                    buzz(out, kBow, 0.6f * a, 0.07f);
                    if (g_log.load()) spdlog::info("[Bow] full draw: string {:.2f} m behind the nock (bowfull {:.2f}), {:.0f} ms after RT", d, full,
                                                   g_rt ? double(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rt_t).count()) : 0.0);
                } else if (g_full_said && eased) {
                    g_full_said = false;   // eased forward: the buzz again at the next full draw
                }
            }
        } else if (g_rt && [&] {
                       // bowmindraw: how far the string was drawn when let go (the string's own draw with bowpull, else the hand's pull).
                       const float pt   = g_pull_active.load() ? g_pull_t.load() : -1.0f;
                       const float frac = pt >= 0.0f ? pt : g_pull_frac.load();
                       if (frac >= std::clamp(g_min_draw.load(), 0.0f, 1.0f)) return false;
                       g_cancel   = true;
                       g_cancel_t = now;
                       if (g_cancel_key.load() > 0) out.key_scan = uint16_t(g_cancel_key.load());
                       else out.buttons |= 0x4000;
                       set_phase(Phase::Nocked, now, fmt::format("grip let go at {:.2f} m, drawn {:.0f} % (bowmindraw {:.0f} %): no shot, the game's draw cancelled ({})", d, frac * 100.0f,
                                                                 g_min_draw.load() * 100.0f, g_cancel_key.load() > 0 ? fmt::format("key 0x{:02x}", g_cancel_key.load()) : std::string{ "pad X" }));
                       return true;
                   }()) {
            ph = Phase::Nocked;
        } else if (g_rt) {
            set_phase(Phase::Loose, now, fmt::format("grip let go at {:.2f} m (longest {:.2f}, full {}): the string released", d, g_draw_max, g_draw_max >= g_full.load() ? "yes" : "no"));
            ph = Phase::Loose;
        } else {
            set_phase(Phase::Nocked, now, fmt::format("grip let go at {:.2f} m, pulled {:.2f} m (bowstart {:.2f}): the arrow stays on the bow", d, d - g_d0, g_start.load()));
            ph = Phase::Nocked;
        }
        break;
    case Phase::Loose: {
        const int  hold = std::max(std::clamp(g_hold_ms.load(), 0, 1500), g_test_hold.load());
        const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rt_t).count();
        if (held >= hold) {
            g_rt = false;
            g_shots++;
            g_test_hold = 0;
            g_loose_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();   // bowsteady: the aim held a moment
            set_phase(Phase::Empty, now, fmt::format("RT let go after {} ms held (bowholdms {}): the game looses the arrow (shot {})", held, hold, g_shots.load()));
            ph = Phase::Empty;
        }
        break;
    }
    default: break;
    }
    out.rt   = g_rt;
    g_rt_now = g_rt;
    if (ph != Phase::Drawing) g_pull_frac = 0.0f;
    // bowarrow: where the arrow is drawn, from this poll.
    ArrowView v{};
    if (track) {
        const glm::vec3 bow_fwd = glm::normalize(-in.bow_rot[2]);
        if (ph == Phase::Arrow) {
            // In the hand, held at its nock end, along where the controller points (or out towards the bow when that is unknown).
            glm::vec3 f = in.hand_rot_ok ? -in.hand_rot[2] : nock - in.hand;
            f           = glm::length(f) > 1e-4f ? glm::normalize(f) : bow_fwd;
            v.arrow     = true;
            v.tail      = in.hand - f * 0.03f;
            v.tip       = v.tail + f * kArrowLen;
            // No ball at the nock zone any more (author 6 Nov 14:02).
        } else if (ph == Phase::Nocked) {
            v.arrow = true;
            v.tail  = nock - bow_fwd * kNockBack;
            v.tip   = v.tail + bow_fwd * kArrowLen;
        } else if (ph == Phase::Drawing || ph == Phase::Loose) {
            const glm::vec3 s = nock - in.hand;
            const glm::vec3 f = glm::length(s) > 0.02f ? glm::normalize(s) : bow_fwd;
            v.arrow           = true;
            v.tail            = in.hand;
            v.tip             = in.hand + f * std::max(kArrowLen, glm::length(s) + 0.1f);
        }
    }
    std::lock_guard vl{ g_view_mtx };
    g_view = v;
}

bool arrow_shown() { return g_arrow_on.load(); }

bool arrow_view(ArrowView& v)
{
    if (!g_arrow_on.load() || !g_in_hand.load()) return false;
    std::lock_guard _{ g_view_mtx };
    v = g_view;
    if (g_model_shown.load()) v.arrow = false;   // bowarrowmodel: the game's own arrow is there
    return v.arrow || v.nock;
}

bool arrow_model_on() { return g_model_on.load(); }

bool arrow_part(uint32_t sig, uint32_t count)
{
    std::lock_guard _{ g_list_mtx };
    const auto it = g_arrow_parts.find(sig);
    return it != g_arrow_parts.end() && std::find(it->second.begin(), it->second.end(), count) != it->second.end();
}

bool arrow_known(uint32_t sig)
{
    std::lock_guard _{ g_list_mtx };
    const auto it = g_arrow_parts.find(sig);
    return it != g_arrow_parts.end() && !it->second.empty();
}

std::string arrow_parts_text(uint32_t sig)
{
    std::lock_guard _{ g_list_mtx };
    std::string s;
    if (const auto it = g_arrow_parts.find(sig); it != g_arrow_parts.end())
        for (const uint32_t c : it->second) s += (s.empty() ? "" : ", ") + std::to_string(c);
    return s;
}

std::string learn_arrow(uint32_t sig, const std::vector<uint32_t>& counts)
{
    std::lock_guard _{ g_list_mtx };
    g_arrow_parts[sig] = counts;
    return parts_line();
}

void set_arrow_model_shown(bool shown) { g_model_shown = shown; }

void get_arrow_hand(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_arrow_hand[i].load();
}

Phase arrow_phase()
{
    const int f = g_force.load();
    return f >= 0 ? Phase(f) : phase();
}

uint32_t shots() { return g_shots.load(); }

bool scanning() { return clock::now().time_since_epoch().count() < g_scan_until.load(); }

bool arrow_log() { return g_log.load(); }

bool hand_on_string() { return g_hand_string.load() && g_in_hand.load() && g_rt_now.load() && !g_pull_active.load(); }

bool pull_on() { return g_pull.load(); }

float pull_frac() { return g_pull_frac.load(); }

bool rt_held() { return g_rt_now.load(); }

void get_string_hand(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_string_hand[i].load();
}

bool pull_forget_take() { return g_pull_forget.exchange(false); }

bool pin_on() { return g_pin.load() != 0; }
int  pin_mode() { return g_pin.load(); }
bool sight_aim() { return g_sight_aim.load(); }
bool sight_eye() { return g_sight_eye.load(); }
bool dot_one_eye() { return g_dot_one_eye.load(); }
bool no_scope() { return g_no_scope.load(); }
bool steady_arrow() { return g_steady_arrow.load() && g_steady.load(); }
float steady_hz() { return std::clamp(g_steady_hz.load(), 0.1f, 5.0f); }
bool pin_steady() { return g_pin_steady.load(); }
bool sling_on() { return g_sling.load(); }
bool quiver_grip() { return g_quiver_grip.load(); }
bool pull_keep() { return g_pull_keep.load(); }
bool sling_grab() { return g_sling_grab.load(); }
bool right_hand() { return g_right_hand.load(); }

bool fine() { return g_fine.load(); }

bool pin_mid() { return g_pin_mid.load(); }
float sling_grab_zone() { return std::clamp(g_sling_grab_zone.load(), 0.04f, 0.30f); }
bool  sling_body() { return g_sling_body.load(); }
float sling_body_deg() { return std::clamp(g_sling_body_deg.load(), 0.0f, 90.0f); }
glm::mat3 sling_body_turn(bool mirror)
{
    // Limbs (y) up and to the other hand's side, the arrow (-z) square to them in the body's front plane, the bow's thin axis (x) into the body.
    const float     a  = glm::radians(sling_body_deg());
    const float     m  = mirror ? -1.0f : 1.0f;
    const glm::vec3 y{ -m * std::sin(a), std::cos(a), 0.0f };
    const glm::vec3 z{ -m * std::cos(a), -std::sin(a), 0.0f };
    return glm::mat3{ glm::cross(y, z), y, z };
}
void get_sling_hold(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_sling_hold[i].load();
}
bool dot_wanted()
{
    if (!g_dot_arrow.load()) return true;
    const Phase p = Phase(g_phase.load());
    return p == Phase::Nocked || p == Phase::Drawing || p == Phase::Loose || aim_held();
}

namespace
{
    // bowsteady's state (render thread only).
    struct Steady
    {
        bool              active{ false };
        glm::quat         rot{ 1.0f, 0.0f, 0.0f, 0.0f }, raw_prev{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3         pos{ 0.0f }, raw_pos_prev{ 0.0f }, vel{ 0.0f }, wvel{ 0.0f };
        glm::mat4         in{ 0.0f }, out{ 1.0f };
        clock::time_point last{};
        float             last_cut{ 0.0f }, last_off{ 0.0f };   // the last new reading's cutoff (Hz) and the bow off the hands (deg)
        // bowsteadyaim: the mode this draw's state was set up for, the aim's last direction and its swing speed (rad/s), the bow against the
        // head (steadied, last as read, its speed), whether the head was known, the last reading's position cutoff and what opened it.
        bool              aim_mode{ false }, head_known{ false };
        glm::vec3         fwd_prev{ 0.0f, 0.0f, -1.0f }, fwvel{ 0.0f };
        glm::vec3         rel{ 0.0f }, rel_raw_prev{ 0.0f }, rvel{ 0.0f };
        float             last_pcut{ 0.0f };
        int               last_open{ 0 };   // 1 the swing speed, 2 the aim behind, 4 the position (its cutoff past bowsteadyhz + 2.5 Hz)
    };
    Steady g_sty;

    // bowsteadyaim: every speed and the head-relative position started again from this reading (a draw starting, the mode switched, the head
    // found or lost), so nothing reads as a jump.
    void steady_rates_reset(Steady& s, const glm::quat& q, const glm::vec3& p, const glm::vec3* head)
    {
        s.raw_prev     = q;
        s.raw_pos_prev = p;
        s.vel = s.wvel = glm::vec3{ 0.0f };
        s.fwd_prev     = glm::mat3_cast(q) * glm::vec3{ 0.0f, 0.0f, -1.0f };
        s.fwvel        = glm::vec3{ 0.0f };
        s.head_known   = head != nullptr;
        const glm::vec3 hd = head ? *head : glm::vec3{ 0.0f };
        s.rel          = s.pos - hd;
        s.rel_raw_prev = p - hd;
        s.rvel         = glm::vec3{ 0.0f };
        s.aim_mode     = g_steady_aim.load();
    }

    // bowsteady's log (render thread): one [Bow] bowsteady line per draw, whether bowsteady is on or off.
    struct DrawStat
    {
        bool              on{ false }, have{ false }, steadied{ false };
        clock::time_point t0{};
        uint64_t          frames{ 0 }, readings{ 0 }, repeats{ 0 }, open{ 0 }, jumps{ 0 };
        uint64_t          open_speed{ 0 }, open_behind{ 0 }, open_pos{ 0 }, rel_n{ 0 };   // bowsteadyaim: what opened it; readings with the head known
        double            rel_raw_mm{ 0.0 }, rel_out_mm{ 0.0 }, pcut{ 0.0 };             // the bow against the head per reading (as placed, as drawn)
        glm::vec3         rel_raw_prev{ 0.0f }, rel_out_prev{ 0.0f };
        bool              have_rel{ false };
        double            raw_deg{ 0.0 }, out_deg{ 0.0 }, cut{ 0.0 }, off{ 0.0 }, eyes{ 0.0 };
        float             off_max{ 0.0f }, eyes_max{ 0.0f };
        glm::mat4         raw_prev{ 0.0f };
        glm::vec3         raw_fwd{ 0.0f }, out_fwd{ 0.0f }, out_fwd_frame{ 0.0f };
        uint32_t          shots0{ 0 };
    };
    DrawStat g_dst;
    std::atomic<float> g_test_shake{ 0.0f };   // bowsteadytest (test, not saved): deg

    float turn_deg(const glm::quat& a, const glm::quat& b)   // asin of the vector part (acos near 1 rounds small turns away)
    {
        const glm::quat d = glm::inverse(a) * b;
        return glm::degrees(2.0f * std::asin(std::clamp(glm::length(glm::vec3{ d.x, d.y, d.z }), 0.0f, 1.0f)));
    }
    float dir_deg(const glm::vec3& a, const glm::vec3& b)
    {
        return glm::degrees(std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b)));
    }
    int64_t ms_now() { return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count(); }
}

bool aim_held()
{
    const int64_t t = g_loose_ms.load();
    return t != 0 && g_steady.load() && ms_now() - t < std::clamp(g_steady_hold.load(), 0, 1000);
}

bool steady_pose(glm::mat4& k, bool drawn, const glm::mat4& from, float ws, const glm::vec3* head)
{
    auto& s = g_sty;
    if (!g_steady.load()) {
        s.active = false;
        return false;
    }
    if (!drawn) {
        // A shot: the steadied aim held a moment, so the arrow leaves where the sight was (the bow's line jumps back to the bow hand's
        // controller as soon as the string is let go).
        if (s.active && aim_held()) {
            k = s.out;
            return true;
        }
        s.active = false;
        return false;
    }
    ws               = std::max(1e-3f, ws);
    const auto  now  = clock::now();
    const glm::mat4 raw = k;
    glm::quat   q    = glm::normalize(glm::quat_cast(glm::mat3{ raw }));
    const glm::vec3 p{ raw[3] };
    if (!s.active) {
        // The draw starts: from where the weapon smoothing had the bow (no jump), unless the bow's line itself jumped (it points along the
        // arrow as soon as the string is taken more than 20 cm from the nock): then at once, as the weapon smoothing did (its speed estimate
        // opens it fully). Gliding there instead took several readings with the bow swinging (the simulator run of 5 Nov: 77 deg).
        const glm::quat fq = glm::normalize(glm::quat_cast(glm::mat3{ from }));
        const bool      start_at = turn_deg(fq, q) < 3.0f && glm::length(glm::vec3{ from[3] } - p) < 0.03f * ws;
        s.active       = true;
        s.rot          = start_at ? fq : q;
        s.pos          = start_at ? glm::vec3{ from[3] } : p;
        steady_rates_reset(s, s.rot, p, head);
        s.fwd_prev     = glm::mat3_cast(q) * glm::vec3{ 0.0f, 0.0f, -1.0f };   // the speeds from the hands' own reading
        s.raw_prev     = q;
        s.rel_raw_prev = p - (head ? *head : glm::vec3{ 0.0f });
        s.last         = now;
        s.in           = raw;
        s.out          = glm::mat4_cast(s.rot);
        s.out[3]       = glm::vec4{ s.pos, 1.0f };
        k = s.out;
        return true;
    }
    if (std::memcmp(&raw, &s.in, sizeof(raw)) == 0) {   // the pair's second eye: the same reading, the same bow in both eyes
        k = s.out;
        return true;
    }
    const float dt = std::clamp(std::chrono::duration<float>(now - s.last).count(), 1e-4f, 0.1f);
    s.last = now;
    s.in   = raw;
    auto alpha = [dt](float hz) { return 1.0f / (1.0f + 1.0f / (glm::two_pi<float>() * hz * dt)); };
    if (glm::dot(q, s.rot) < 0.0f) q = -q;
    if (s.aim_mode != g_steady_aim.load() || s.head_known != (head != nullptr)) steady_rates_reset(s, q, p, head);
    if (s.aim_mode) {
        // bowsteadyaim. The aim: only the forward direction's swing decides; roll (the bow hand's wrist) has no say.
        const float     rest = std::clamp(g_steady_hz.load(), 0.1f, 5.0f);
        const glm::mat3 rm{ raw };
        const glm::vec3 f  = glm::normalize(-rm[2]);
        const glm::vec3 u  = glm::normalize(rm[1]);
        auto turn_toward = [](const glm::vec3& from_dir, const glm::vec3& to_dir, float frac) {
            const glm::vec3 ax = glm::cross(from_dir, to_dir);
            const float     sn = glm::length(ax);
            if (sn < 1e-7f) return from_dir;
            return glm::normalize(glm::angleAxis(std::atan2(sn, glm::dot(from_dir, to_dir)) * frac, ax / sn) * from_dir);
        };
        {
            const glm::vec3 ax = glm::cross(s.fwd_prev, f);
            const float     sn = glm::length(ax);
            const glm::vec3 w  = sn > 1e-7f ? ax / sn * (std::atan2(sn, glm::dot(s.fwd_prev, f)) / dt) : glm::vec3{ 0.0f };
            s.fwvel            = glm::mix(s.fwvel, w, alpha(kAimSpeedHz));
            s.fwd_prev         = f;
        }
        const glm::mat3 sm{ glm::mat3_cast(s.rot) };
        const glm::vec3 fs0  = glm::normalize(-sm[2]);
        const float     wdeg = glm::degrees(glm::length(s.fwvel));   // deg/s
        const float     off  = dir_deg(fs0, f);                      // the steadied aim behind the hands' (deg)
        const float     c_sp = 0.25f * std::max(0.0f, wdeg - kAimMoveDps);
        const float     c_bh = 2.0f * std::max(0.0f, off - kAimBehindDeg);
        const float     cut  = std::min(30.0f, rest + c_sp + c_bh);
        const glm::vec3 fs   = turn_toward(fs0, f, alpha(cut));
        // The roll: the steadied up eased toward the hands' at kRollMinHz at least, square to the aim.
        glm::vec3 us = glm::mix(sm[1], u, alpha(std::max(cut, kRollMinHz)));
        us -= glm::dot(us, fs) * fs;
        if (glm::length(us) < 1e-4f) us = u - glm::dot(u, fs) * fs;
        if (glm::length(us) < 1e-4f) us = sm[1] - glm::dot(sm[1], fs) * fs;
        us = glm::normalize(us);
        s.rot = glm::normalize(glm::quat_cast(glm::mat3{ glm::normalize(glm::cross(fs, us)), us, -fs }));
        // The position, against the head (the sight line runs from the eye to the pin on the bow).
        const glm::vec3 hd   = head ? *head : glm::vec3{ 0.0f };
        const glm::vec3 rel  = p - hd;
        s.rvel               = glm::mix(s.rvel, (rel - s.rel_raw_prev) / dt, alpha(kAimSpeedHz));
        s.rel_raw_prev       = rel;
        s.raw_prev           = q;
        s.raw_pos_prev       = p;
        const float vcm      = 100.0f * glm::length(s.rvel) / ws;      // cm/s
        const float poff     = 100.0f * glm::length(rel - s.rel) / ws;  // cm
        const float pcut     = std::min(30.0f, rest + 0.5f * std::max(0.0f, vcm - kPosMoveCms) + 1.0f * std::max(0.0f, poff - kPosBehindCm));
        s.rel                = glm::mix(s.rel, rel, alpha(pcut));
        s.pos                = hd + s.rel;
        s.out                = glm::mat4_cast(s.rot);
        s.out[3]             = glm::vec4{ s.pos, 1.0f };
        k                    = s.out;
        s.last_cut           = cut;
        s.last_off           = off;
        s.last_pcut          = pcut;
        s.last_open          = (c_sp > 0.0f ? 1 : 0) | (c_bh > 0.0f ? 2 : 0) | (pcut > rest + 2.5f ? 4 : 0);
        return true;
    }
    // The hands' speed, averaged as vectors at 3 Hz: tremor cancels out, a real move adds up.
    glm::quat dq = q * glm::inverse(s.raw_prev);
    if (dq.w < 0.0f) dq = -dq;
    const glm::vec3 axis_sin{ dq.x, dq.y, dq.z };
    const float     sn = glm::length(axis_sin);
    const glm::vec3 w  = sn > 1e-7f ? axis_sin / sn * (2.0f * std::asin(std::min(sn, 1.0f)) / dt) : glm::vec3{ 0.0f };
    s.wvel             = glm::mix(s.wvel, w, alpha(3.0f));
    s.vel              = glm::mix(s.vel, (p - s.raw_pos_prev) / dt, alpha(3.0f));
    s.raw_prev         = q;
    s.raw_pos_prev     = p;
    const float wdeg   = glm::degrees(glm::length(s.wvel));        // deg/s
    const float vcm    = 100.0f * glm::length(s.vel) / ws;          // cm/s
    const float off    = turn_deg(s.rot, q);                        // the steadied bow against the hands now (deg)
    const float poff   = 100.0f * glm::length(p - s.pos) / ws;      // cm
    // At rest bowsteadyhz; a deliberate move opens it: 0.25 Hz per deg/s past 8 deg/s, 0.5 Hz per cm/s past 3 cm/s, and 2 Hz per deg
    // (1 Hz per cm) once the bow trails the hands by more than 2 deg (2 cm), so it never lags far.
    const float rest   = std::clamp(g_steady_hz.load(), 0.1f, 5.0f);
    const float cut    = std::min(30.0f, rest + 0.25f * std::max(0.0f, wdeg - 8.0f) + 0.5f * std::max(0.0f, vcm - 3.0f) + 2.0f * std::max(0.0f, off - 2.0f) +
                                         1.0f * std::max(0.0f, poff - 2.0f));
    const float a      = alpha(cut);
    s.rot              = glm::normalize(glm::slerp(s.rot, q, a));
    s.pos              = glm::mix(s.pos, p, a);
    s.out              = glm::mat4_cast(s.rot);
    s.out[3]           = glm::vec4{ s.pos, 1.0f };
    k                  = s.out;
    s.last_cut         = cut;
    s.last_off         = off;
    s.last_pcut        = cut;
    s.last_open        = 0;
    return true;
}

void test_shake(glm::mat4& k, bool drawn)
{
    const float amp = g_test_shake.load();
    if (!(amp > 0.0f) || !drawn) return;
    // A new "reading" 30 times a second (frames within one see the same, as a pair's two eyes do): tremor at 8-9 Hz, sway at about
    // 1 Hz and noise, turning the bow in its own axes.
    const double  t    = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const int64_t slot = int64_t(t * 30.0);
    const double  ts   = double(slot) / 30.0;
    auto noise = [slot](uint32_t salt) {
        uint64_t x = uint64_t(slot) * 0x9E3779B97F4A7C15ull + salt;
        x ^= x >> 31;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 29;
        return float(double(x & 0xFFFFFF) / double(0xFFFFFF) * 2.0 - 1.0);
    };
    const double tp  = glm::two_pi<double>();
    const float  yaw = amp * float(0.6 * std::sin(tp * 9.0 * ts) + 0.8 * std::sin(tp * 1.1 * ts) + 0.4 * noise(1));
    const float  pit = amp * float(0.6 * std::sin(tp * 8.0 * ts + 0.7) + 0.6 * std::sin(tp * 0.9 * ts + 1.0) + 0.4 * noise(2));
    const glm::quat r = glm::angleAxis(glm::radians(yaw), glm::vec3{ 0.0f, 1.0f, 0.0f }) * glm::angleAxis(glm::radians(pit), glm::vec3{ 1.0f, 0.0f, 0.0f });
    const glm::vec4 p = k[3];
    k                 = k * glm::mat4_cast(r);
    k[3]              = p;
}

void steady_stats(const glm::mat4& raw, const glm::mat4& out, bool drawn, bool steadied, const glm::vec3* head, float ws)
{
    auto&      d   = g_dst;
    const auto now = clock::now();
    if (!drawn && !steadied) {   // the draw (and the aim held after it) is over: its line
        if (d.on) {
            const double secs = std::chrono::duration<double>(now - d.t0).count();
            const uint32_t shots = g_shots.load() - d.shots0;
            if (d.readings == 0) {
                spdlog::info("[Bow] bowsteady {}: a draw of {:.1f} s ({} frames): no new controller reading during it (the controllers did not move), "
                             "nothing to steady; bowsteadytest D shakes the drawn bow for a test", d.steadied ? "on" : "off", secs, d.frames);
            } else {
                const double n = double(d.readings);
                spdlog::info("[Bow] bowsteady {}: a draw of {:.1f} s ({} frames): {} new readings ({} more left out: the bow's line jumping or the aim held "
                             "after the shot), {} frames repeating one (a pair's second eye); the bow's aim moved {:.3f} deg per reading as placed, {:.3f} deg "
                             "as drawn ({:.1f} times steadier); a pair's two eyes {:.4f} deg apart on average, {:.4f} max{}; {}{}{}",
                             d.steadied ? "on" : "off", secs, d.frames, d.readings, d.jumps, d.repeats, d.raw_deg / n, d.out_deg / n, d.out_deg > 1e-9 ? d.raw_deg / d.out_deg : 0.0,
                             d.repeats ? d.eyes / double(d.repeats) : 0.0, d.eyes_max,
                             d.steadied ? fmt::format("; cutoff {:.2f} Hz on average (bowsteadyhz {:.2f}), opened for a move in {:.0f} % of readings; the steadied "
                                                      "aim off the hands {:.2f} deg on average, {:.2f} max", d.cut / n, g_steady_hz.load(), 100.0 * double(d.open) / n,
                                                      d.off / n, d.off_max)
                                        : std::string{},
                             shots ? fmt::format("shot loosed (aim held {} ms after it)", g_steady.load() ? g_steady_hold.load() : 0) : std::string{ "no shot" },
                             g_test_shake.load() > 0.0f ? fmt::format("; bowsteadytest {:.2f} deg", g_test_shake.load()) : std::string{},
                             !d.steadied ? std::string{}
                             : fmt::format("; bowsteadyaim {}: the aim opened by its swing in {:.0f} % of readings, by trailing in {:.0f} %; the position's cutoff "
                                           "{:.2f} Hz on average, opened in {:.0f} %{}",
                                           g_steady_aim.load() ? "on" : "off", 100.0 * double(d.open_speed) / n, 100.0 * double(d.open_behind) / n, d.pcut / n,
                                           100.0 * double(d.open_pos) / n,
                                           d.rel_n ? fmt::format("; the bow against the head moved {:.2f} mm per reading as placed, {:.2f} mm as drawn ({:.1f} times "
                                                                 "steadier: the sight line's share, 1 mm about 0.08 deg)",
                                                                 d.rel_raw_mm / double(d.rel_n), d.rel_out_mm / double(d.rel_n),
                                                                 d.rel_out_mm > 1e-9 ? d.rel_raw_mm / d.rel_out_mm : 0.0)
                                                   : std::string{ "; the head not known" }));
            }
        }
        d = DrawStat{};
        return;
    }
    if (!d.on) {
        d.on     = true;
        d.t0     = now;
        d.shots0 = g_shots.load();
    }
    d.frames++;
    d.steadied     = d.steadied || steadied;
    const glm::vec3 rf = -glm::vec3{ raw[2] }, of = -glm::vec3{ out[2] };
    if (d.have && std::memcmp(&raw, &d.raw_prev, sizeof(raw)) == 0) {
        // The same reading again: the pair's second eye, against the frame before it.
        const float e = dir_deg(d.out_fwd_frame, of);
        d.eyes += e;
        d.eyes_max = std::max(d.eyes_max, e);
        d.repeats++;
    } else {
        const float jump = d.have ? dir_deg(d.raw_fwd, rf) : 0.0f;
        if (d.have && (!drawn || jump > 3.0f)) {
            // Not shake: the bow's line jumping (the string taken or let go) or the aim held after the shot (the bow as placed is back on the
            // bow hand's controller). Counted apart, left out of the numbers per reading.
            d.jumps++;
        } else if (d.have) {
            d.raw_deg += jump;
            d.out_deg += dir_deg(d.out_fwd, of);
            d.readings++;
            if (steadied && drawn) {
                const auto& s = g_sty;
                d.cut += s.last_cut;
                d.off += s.last_off;
                d.off_max = std::max(d.off_max, s.last_off);
                if (s.last_cut > std::clamp(g_steady_hz.load(), 0.1f, 5.0f) + 2.5f) d.open++;
                d.pcut += s.last_pcut;
                if (s.last_open & 1) d.open_speed++;
                if (s.last_open & 2) d.open_behind++;
                if (s.last_open & 4) d.open_pos++;
            }
            if (head && d.have_rel) {   // the bow's position against the head, as placed and as drawn (mm per reading)
                const float wsc = std::max(1e-3f, ws);
                d.rel_raw_mm += 1000.0 * glm::length((glm::vec3{ raw[3] } - *head) - d.rel_raw_prev) / wsc;
                d.rel_out_mm += 1000.0 * glm::length((glm::vec3{ out[3] } - *head) - d.rel_out_prev) / wsc;
                d.rel_n++;
            }
        }
        d.raw_fwd  = rf;
        d.out_fwd  = of;
        d.raw_prev = raw;
        d.have     = true;
        d.have_rel = head != nullptr;
        if (head) {
            d.rel_raw_prev = glm::vec3{ raw[3] } - *head;
            d.rel_out_prev = glm::vec3{ out[3] } - *head;
        }
    }
    d.out_fwd_frame = of;
}

void set_pull(bool active, float t)
{
    g_pull_active = active;
    g_pull_t      = t;
}

bool upright_on() { return g_upright.load(); }

bool upright_keep() { return g_upright_keep.load(); }

bool upright_wait() { return g_upright_wait.load(); }

bool flip_on() { return g_flip.load(); }

void get_hand_pos(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_hand_pos[i].load();
}

void get_hold(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_hold[i].load();
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "manualbow") {
        g_on    = on;
        persist = true;
        spdlog::info("[Bow] manualbow {}: {}", on ? "on" : "off",
                     on ? fmt::format("with a bow in hand (bowweapons: {}) the right shoulder is a quiver and the string is drawn by hand", list_text().empty() ? "none yet, tick it in F11" : list_text())
                        : std::string{ "the bow is a weapon like the others" });
        return true;
    }
    if (cmd == "bowweapons") {
        // The whole list each time (saved as it is): bowweapons 12345 67890, or bowweapons none.
        std::set<uint32_t> s;
        auto add = [&](const std::string& t) {
            if (t.empty() || t == "none") return;
            char*               e = nullptr;
            const unsigned long v = std::strtoul(t.c_str(), &e, 10);
            if (e && *e == 0 && v != 0) s.insert(uint32_t(v));
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        {
            std::lock_guard _{ g_list_mtx };
            g_bows = s;
        }
        persist = true;
        spdlog::info("[Bow] bowweapons: {}", list_text().empty() ? "none" : list_text());
        return true;
    }
    if (cmd == "bowgame") {
        g_game  = on;
        persist = true;
        spdlog::info("[Bow] bowgame {}: {}", on ? "on" : "off", on ? "a weapon the game's data calls a bow is a bow (besides bowweapons)" : "only bowweapons");
        return true;
    }
    if (cmd == "bowstart") { g_start = std::clamp(arg.empty() ? kStartDefault : fv, 0.05f, 0.60f); persist = true; return true; }
    if (cmd == "bowfull") { g_full = std::clamp(arg.empty() ? kFullDefault : fv, 0.15f, 0.90f); persist = true; return true; }
    if (cmd == "bownockzone") { g_nock_zone = std::clamp(arg.empty() ? kNockZoneDefault : fv, 0.04f, 0.40f); persist = true; return true; }
    if (cmd == "bowholdms") { g_hold_ms = std::clamp(arg.empty() ? kHoldDefault : int(fv), 0, 1500); persist = true; return true; }
    if (cmd == "bowbuzz") { g_buzz = std::clamp(arg.empty() ? kBuzzDefault : fv, 0.1f, 1.0f); persist = true; return true; }
    if (cmd == "bownock") {
        float v[3]{ kNockDefault[0], kNockDefault[1], kNockDefault[2] };
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_nock[i] = std::clamp(v[i], -0.3f, 0.3f);
        persist = true;
        spdlog::info("[Bow] bownock {:.2f} {:.2f} {:.2f} (right, up, back of the bow hand's controller)", g_nock[0].load(), g_nock[1].load(), g_nock[2].load());
        return true;
    }
    if (cmd == "bowlog") { g_log = on; return true; }
    if (cmd == "bowarrow") { g_arrow_on = on; persist = true; return true; }
    if (cmd == "bowarrowmodel") {
        g_model_on = arg.empty() || on;
        if (!g_model_on.load()) g_model_shown = false;
        persist = true;
        spdlog::info("[Bow] bowarrowmodel {}", g_model_on.load() ? "on: the game's own arrow (learnt at the first shot with each bow)" : "off: the drawn arrow");
        return true;
    }
    if (cmd == "bowarrowmesh") {
        // The whole list each time (saved as it is): bowarrowmesh 51543:1234,567 60000:890, or bowarrowmesh none (learnt again at the next shot).
        std::map<uint32_t, std::vector<uint32_t>> m;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            std::vector<uint32_t> v;
            std::stringstream     ss{ t.substr(c + 1) };
            std::string           n;
            while (std::getline(ss, n, ',')) {
                const unsigned long k = std::strtoul(n.c_str(), nullptr, 10);
                if (k > 0 && v.size() < 8) v.push_back(uint32_t(k));
            }
            if (w != 0 && !v.empty()) m[uint32_t(w)] = v;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_list_mtx };
            g_arrow_parts = m;
            line          = parts_line();
        }
        persist = true;
        spdlog::info("[Bow] {}", line);
        return true;
    }
    if (cmd == "bowarrowhand") {
        float v[3]{ kArrowHandDefault[0], kArrowHandDefault[1], kArrowHandDefault[2] };
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_arrow_hand[i] = std::clamp(v[i], -0.4f, 0.4f);
        persist = true;
        spdlog::info("[Bow] bowarrowhand {:.2f} {:.2f} {:.2f} (the arrow's nock end from the string hand's controller: right, up, back)", g_arrow_hand[0].load(),
                     g_arrow_hand[1].load(), g_arrow_hand[2].load());
        return true;
    }
    if (cmd == "bowupright") {
        g_upright = arg.empty() || on;
        persist   = true;
        spdlog::info("[Bow] bowupright {}", g_upright.load() ? "on: the bow stands upright in the hand, its arrow where the controller points" : "off: the bow as the game holds it");
        return true;
    }
    if (cmd == "bowuprightkeep") {
        g_upright_keep = arg.empty() || on;
        persist        = true;
        spdlog::info("[Bow] bowuprightkeep {}", g_upright_keep.load() ? "on: each bow's upright turn, once measured, kept in fc5vr_bowupright.txt: upright in the hand and on the sling from the load"
                                                                      : "off: each bow measured again in the hand each session (on the sling until then: as the game holds it)");
        return true;
    }
    if (cmd == "bowuprightwait") {
        g_upright_wait = arg.empty() || on;
        persist        = true;
        spdlog::info("[Bow] bowuprightwait {}", g_upright_wait.load()
                                                    ? "on: the bow's arrow and upright turn measured only once the bow has been out 1.5 s and the game's arrow points straight ahead (not during the game's switch animation)"
                                                    : "off: measured at the first chance after the bow comes out, as before (a switch animation can stand it upside down)");
        return true;
    }
    if (cmd == "bowpull") {
        g_pull  = arg.empty() || on;
        persist = true;
        spdlog::info("[Bow] bowpull {}", g_pull.load() ? "on: the string and the arrow follow the string hand (from the first full draw with each bow)"
                                                       : "off: the game's own draw animation, the right hand on its string");
        return true;
    }
    if (cmd == "bowstringhand") {
        float v[3]{ kStringHandDefault[0], kStringHandDefault[1], kStringHandDefault[2] };
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_string_hand[i] = std::clamp(v[i], -0.2f, 0.2f);
        persist = true;
        spdlog::info("[Bow] bowstringhand {:.2f} {:.2f} {:.2f} (the string while drawing, from the string hand's controller: right, up, back)", g_string_hand[0].load(),
                     g_string_hand[1].load(), g_string_hand[2].load());
        return true;
    }
    if (cmd == "bowsteady") {
        g_steady = arg.empty() || on;
        persist  = true;
        spdlog::info("[Bow] bowsteady {}", g_steady.load() ? fmt::format("on: drawn, the bow held steady ({:.2f} Hz at rest), the aim held {} ms after a shot", g_steady_hz.load(), g_steady_hold.load())
                                                           : std::string{ "off: drawn, the bow follows the weapon smoothing as the other weapons" });
        return true;
    }
    if (cmd == "bowsteadyaim") {
        g_steady_aim = arg.empty() || on;
        persist      = true;
        spdlog::info("[Bow] bowsteadyaim {}", g_steady_aim.load()
                                                  ? "on: drawn, the bow's steadier opens only for the aim's own swing (roll left out) and steadies the bow against the head"
                                                  : "off: drawn, the bow's steadier opens for any turn or move of the bow hand (as before 6 Nov)");
        return true;
    }
    if (cmd == "bowsteadytest") {
        g_test_shake = std::clamp(arg.empty() ? 0.0f : fv, 0.0f, 2.0f);
        spdlog::info("[Bow] bowsteadytest {:.2f} deg (test, not saved): {}", g_test_shake.load(),
                     g_test_shake.load() > 0.0f ? "the drawn bow is shaken that much (tremor, sway and noise, 30 new readings a second) before it is steadied"
                                                : "no test shake");
        return true;
    }
    if (cmd == "bowsteadyhz") {
        g_steady_hz = std::clamp(arg.empty() ? kSteadyHzDefault : fv, 0.1f, 5.0f);
        persist     = true;
        spdlog::info("[Bow] bowsteadyhz {:.2f}: the drawn bow's cutoff while the aim is held (lower is steadier and slower to follow small moves)", g_steady_hz.load());
        return true;
    }
    if (cmd == "bowsteadyhold") {
        g_steady_hold = std::clamp(arg.empty() ? kSteadyHoldDefault : int(fv), 0, 1000);
        persist       = true;
        spdlog::info("[Bow] bowsteadyhold {} ms: the steadied aim kept that long after a shot", g_steady_hold.load());
        return true;
    }
    if (cmd == "bownoscope") {
        g_no_scope = arg.empty() || on;
        persist    = true;
        spdlog::info("[Bow] bownoscope {}", g_no_scope.load() ? "on: the manual bow's sight never starts the scope picture" : "off: the bow's sight can start the scope picture (its scope entry)");
        return true;
    }
    if (cmd == "bowsteadyarrow") {
        g_steady_arrow = arg.empty() || on;
        persist        = true;
        spdlog::info("[Bow] bowsteadyarrow {}", g_steady_arrow.load() ? "on: drawn, the arrow and the string follow the string hand through a steadier (with bowsteady on)"
                                                                     : "off: the arrow and the string follow the string hand as read");
        return true;
    }
    if (cmd == "bowdotarrow") {
        g_dot_arrow = arg.empty() || on;
        persist     = true;
        spdlog::info("[Bow] bowdotarrow {}", g_dot_arrow.load() ? "on: the bow's red dot (and laser) only while an arrow is on the bow" : "off: the bow's red dot whatever the arrow");
        return true;
    }
    if (cmd == "bowpinsteady") {
        g_pin_steady = arg.empty() || on;
        persist      = true;
        spdlog::info("[Bow] bowpinsteady {}", g_pin_steady.load() ? "on: drawn, the sight line from this frame's eye through the sight pin kept on this frame's bow"
                                                                 : "off: the sight line from the last eye picture through the last drawn pin");
        return true;
    }
    if (cmd == "bowpullkeep") {
        g_pull_keep = arg.empty() || on;
        persist     = true;
        spdlog::info("[Bow] bowpullkeep {}", g_pull_keep.load() ? "on: a bow's full draw taken from the game only after RT held 90 frames while drawing, an arrow's never shorter than known"
                                                               : "off: the rest and full draw taken from the game at any still moment (the full 15 frames after RT)");
        return true;
    }
    if (cmd == "bowquivergrip") {
        g_quiver_grip = arg.empty() || on;
        persist       = true;
        spdlog::info("[Bow] bowquivergrip {}", g_quiver_grip.load() ? "on: the right shoulder is the quiver only with the bow in hand and the left grip held; otherwise it gives its weapon"
                                                                   : "off: with the bow in hand the right shoulder is always the quiver");
        return true;
    }
    if (cmd == "bowsling") {
        g_sling = arg.empty() || on;
        persist = true;
        spdlog::info("[Bow] bowsling {}", g_sling.load() ? (g_right_hand.load() ? "on: with the sling on, the bow hangs on it; the left or the right grip there holds it in that hand "
                                                                                   "(bowrighthand), back on the sling when it is let go"
                                                                                 : "on: with the sling on, the bow hangs on it; the left grip there holds it, the right grip there puts it in the "
                                                                                   "left hand until the left grip is pressed and let go")
                                                         : "off: the bow is always in the left hand");
        return true;
    }
    if (cmd == "bowrighthand") {
        g_right_hand = arg.empty() || on;
        persist      = true;
        spdlog::info("[Bow] bowrighthand {}", g_right_hand.load() ? "on: the slung bow taken with the right grip stays in the right hand, upright as in the left, until that grip is let go "
                                                                    "(RT draws and looses it as the game does)"
                                                                  : "off: the slung bow taken with the right grip goes into the left hand until the left grip is pressed and let go");
        return true;
    }
    if (cmd == "bowfine") {   // (test: never saved)
        g_fine = arg.empty() || on;
        spdlog::info("[Bow] bowfine {} (not saved)", g_fine.load() ? "on: the drawn bow's limbs, string and arrow placed on the bow from small numbers in doubles, with their position fractions"
                                                                : "off: the drawn bow's parts placed with single floats at map size (as before 13 Nov)");
        return true;
    }
    if (cmd == "bowpinmid") {   // (test: never saved)
        g_pin_mid = arg.empty() || on;
        spdlog::info("[Bow] bowpinmid {} (not saved)", g_pin_mid.load() ? "on: the dot's point on a ring sight is the ring's middle (its reticle draws that agree), not a lone smaller piece"
                                                                      : "off: the dot's point is the bow's smallest reticle draw (as before 13 Nov)");
        return true;
    }
    if (cmd == "bowslinggrab") {
        g_sling_grab = arg.empty() || on;
        persist      = true;
        spdlog::info("[Bow] bowslinggrab {}", g_sling_grab.load() ? fmt::format("on: the slung bow taken with the left grip anywhere along it (within {:.2f} m of its limbs' line), the left hand ticked when it comes onto it", sling_grab_zone())
                                                                : std::string{ "off: the slung bow taken only in the sling zone round its grip" });
        return true;
    }
    if (cmd == "bowslinggrabzone") {
        if (!arg.empty()) g_sling_grab_zone = std::clamp(fv, 0.04f, 0.30f);
        persist = true;
        spdlog::info("[Bow] bowslinggrabzone {:.2f} m: how close to the slung bow's limbs a grip takes it (bowslinggrab)", sling_grab_zone());
        return true;
    }
    if (cmd == "bowslingbody") {
        g_sling_body = arg.empty() || on;
        persist      = true;
        spdlog::info("[Bow] bowslingbody {}", g_sling_body.load() ? fmt::format("on: the slung bow across the front of the body, its limbs from the right hip towards the left shoulder, {:.0f} deg from upright", sling_body_deg())
                                                                : std::string{ "off: the slung bow turned with the guns' sling (slingrot), then by bowslinghold" });
        return true;
    }
    if (cmd == "bowslingbodyangle") {
        g_sling_body_deg = arg.empty() ? 35.0f : std::clamp(fv, 0.0f, 90.0f);
        persist          = true;
        spdlog::info("[Bow] bowslingbodyangle {:.0f} deg: the slung bow's limbs this far from upright across the body (bowslingbody)", sling_body_deg());
        return true;
    }
    if (cmd == "bowslinghold") {
        float v[3]{ kSlingHoldDefault[0], kSlingHoldDefault[1], kSlingHoldDefault[2] };   // no numbers: the default
        if (!arg.empty()) {
            v[0] = fv;
            v[1] = v[2] = 0.0f;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_sling_hold[i] = std::clamp(v[i], -180.0f, 180.0f);
        persist = true;
        spdlog::info("[Bow] bowslinghold {:.0f} {:.0f} {:.0f} (deg: the bow on the sling turned right, up, roll to the right)", g_sling_hold[0].load(), g_sling_hold[1].load(),
                     g_sling_hold[2].load());
        return true;
    }
    if (cmd == "bowdoteye") {
        g_dot_one_eye = arg.empty() || on;
        persist       = true;
        spdlog::info("[Bow] bowdoteye {}", g_dot_one_eye.load() ? "on: the bow's red dot in the aiming eye only" : "off: the bow's red dot in both eyes");
        return true;
    }
    if (cmd == "bowsighteye") {
        g_sight_eye = arg.empty() || on;
        persist     = true;
        spdlog::info("[Bow] bowsighteye {}", g_sight_eye.load() ? "on: drawn, the arrow goes from the string hand's eye through the sight (the dot centred for that eye)"
                                                                : "off: from between the eyes through the sight");
        return true;
    }
    if (cmd == "bowsightaim") {
        g_sight_aim = arg.empty() || on;
        persist     = true;
        spdlog::info("[Bow] bowsightaim {}", g_sight_aim.load() ? "on: drawn, the arrow goes from your head through the bow's sight"
                                                                : "off: the arrow goes along the drawn arrow (string hand to nock)");
        return true;
    }
    if (cmd == "bowpin") {
        g_pin   = (arg == "off" || arg == "0" || arg == "false") ? 0 : arg == "pin" ? 1 : 2;   // on (older saves): aim
        persist = true;
        spdlog::info("[Bow] bowpin {}", g_pin.load() == 2 ? "aim: a dot where the arrow lands, in both eyes"
                                        : g_pin.load() == 1 ? "pin: a dot on the bow's sight pin in both eyes"
                                                            : "off: the game's own pin glow only");
        return true;
    }
    if (cmd == "bowmindraw") {
        g_min_draw = std::clamp(arg.empty() ? kMinDrawDefault : fv, 0.0f, 1.0f);
        persist    = true;
        spdlog::info("[Bow] bowmindraw {:.2f}: a string let go short of {:.0f} % of full draw does not shoot", g_min_draw.load(), g_min_draw.load() * 100.0f);
        return true;
    }
    if (cmd == "bowcancelkey") {
        g_cancel_key = arg.empty() ? 0x13 : std::clamp(int(std::strtol(arg.c_str(), nullptr, 0)), 0, 0xff);
        persist      = true;
        spdlog::info("[Bow] bowcancelkey {}: a short draw is cancelled with {}", g_cancel_key.load(), g_cancel_key.load() > 0 ? fmt::format("the key of scan code 0x{:02x}", g_cancel_key.load()) : std::string{ "pad X" });
        return true;
    }
    if (cmd == "bowpullforget") {
        g_pull_forget = true;
        spdlog::info("[Bow] bowpullforget: every bow's learnt rest and full draw dropped (fc5vr_bowpull.txt emptied); each is learnt again at its next full draw");
        return true;
    }
    if (cmd == "bowflip") {
        g_flip  = arg.empty() || on;
        persist = true;
        spdlog::info("[Bow] bowflip {}", g_flip.load() ? "on: the upright bow turned a further 180 deg about its arrow" : "off: the upright bow as measured");
        return true;
    }
    if (cmd == "bowhandpos") {
        float v[3]{ kHandPosDefault[0], kHandPosDefault[1], kHandPosDefault[2] };
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_hand_pos[i] = std::clamp(v[i], -0.15f, 0.15f);
        persist = true;
        spdlog::info("[Bow] bowhandpos {:.2f} {:.2f} {:.2f} (the bow and the left hand from the bow hand's controller: right, up, back)", g_hand_pos[0].load(),
                     g_hand_pos[1].load(), g_hand_pos[2].load());
        return true;
    }
    if (cmd == "bowhold") {
        float v[3]{};
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) g_hold[i] = std::clamp(v[i], -180.0f, 180.0f);
        persist = true;
        spdlog::info("[Bow] bowhold {:.0f} {:.0f} {:.0f} (deg: right, up, roll to the right)", g_hold[0].load(), g_hold[1].load(), g_hold[2].load());
        return true;
    }
    if (cmd == "bowhandstring") {
        g_hand_string = arg.empty() || on;
        persist       = true;
        spdlog::info("[Bow] bowhandstring {}", g_hand_string.load() ? "on: while the game draws its bow the right hand stays on its string" : "off: the right hand follows its controller");
        return true;
    }
    if (cmd == "bowscan") {
        const float sec = std::clamp(arg.empty() ? 10.0f : fv, 0.0f, 120.0f);
        g_scan_until    = (clock::now() + std::chrono::milliseconds(int(sec * 1000.0f))).time_since_epoch().count();
        spdlog::info("[Bow] bowscan {:.0f} s: each weapon-pass draw with the bow in hand logged once ([BowArrow] scan lines)", sec);
        return true;
    }
    if (cmd == "bowforce") {
        static const char* const kName[]{ "off", "empty", "arrow", "nocked", "drawing", "loose" };
        int f = -1;
        for (int i = 1; i < 6; ++i) if (arg == kName[i]) f = i;
        g_force = f;
        spdlog::info("[Bow] bowforce {}: the arrow shown as {}", f < 0 ? "off" : kName[f], f < 0 ? "the real phase says" : kName[f]);
        return true;
    }
    if (cmd == "bowdrawtest") {   // (test: one manual shot with the string hand's inputs made up; g_test_draw)
        g_test_draw = std::clamp(arg.empty() ? 3000 : int(fv), 1200, 10000);
        spdlog::info("[Bow] bowdrawtest: one manual shot made up (needs the bow in hand, manualbow on): an arrow in the hand 1 s, nocked, the string drawn in 1 s and let go {} ms after the nock",
                     g_test_draw.load());
        return true;
    }
    if (cmd == "bowshoot") {
        g_test_hold = std::clamp(arg.empty() ? 1500 : int(fv), 100, 5000);
        spdlog::info("[Bow] bowshoot: one test shot with RT held {} ms (needs the bow in hand, manualbow on)", g_test_hold.load());
        return true;
    }
    return false;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, uint32_t sig)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("Manual bow: arrows from the right shoulder, draw the string with your hand##manualbow", &v)) cmds += v ? "manualbow on\n" : "manualbow off\n";
    hint("With a bow, your right shoulder becomes a quiver: take an arrow there, place it on the bow, pull the string back with the grip held, and let go to shoot.");
    if (!v) return;
    // Holster, bow and throwables clean-up (author 13 Nov 19:27Z): always on, no longer settings: bowgame, bowsteady, bowsteadyaim, bownoscope, bowsteadyarrow, bowdotarrow, bowpinsteady, bowquivergrip,
    // bowsling, bowrighthand, bowslinggrab, bowdoteye, bowsighteye, bowsightaim, bowupright, bowuprightkeep, bowuprightwait,
    // bowpull, bowpullkeep, bowhandstring, bowarrowmodel, bowarrow (their commands still act for a session, never saved).
    if (sig != 0) {
        bool b = is_bow(sig);
        if (ImGui::Checkbox(fmt::format("The weapon in your hands (id {}) is a bow###bowhere", sig).c_str(), &b)) {
            std::set<uint32_t> s;
            {
                std::lock_guard _{ g_list_mtx };
                s = g_bows;
            }
            if (b) s.insert(sig);
            else s.erase(sig);
            std::string line = "bowweapons";
            for (const uint32_t w : s) line += " " + std::to_string(w);
            if (s.empty()) line += " none";
            cmds += line + "\n";
        }
        hint("Tick this once for each bow you use, so the mod treats it as a bow.");
        if (game_bow(sig)) ImGui::TextDisabled("The game says this weapon is a bow.");
    } else {
        ImGui::TextDisabled("No weapon in your hands now: take out a bow to tick it.");
    }
    const std::string bows = list_text();
    ImGui::Text("Bows: %s", bows.empty() ? "none ticked yet" : bows.c_str());
    {
        const float d = g_draw.load(), q = g_quiver.load();
        const std::string now = !g_in_hand.load() ? std::string{ "no bow in hand" }
                                                  : fmt::format("{}; string hand {} from the nock (full draw at {:.0f} cm), {} from the quiver", phase_name(phase()),
                                                                d >= 0.0f ? fmt::format("{:.0f} cm", d * 100.0f) : std::string{ "?" }, g_full.load() * 100.0f,
                                                                q >= 0.0f ? fmt::format("{:.0f} cm", q * 100.0f) : std::string{ "?" });
        ImGui::Text("Now: %s", now.c_str());
    }
    float f = g_full.load(), s = g_start.load(), z = g_nock_zone.load(), a = g_buzz.load();
    int   h = g_hold_ms.load();
    if (ImGui::SliderFloat("Full draw: string hand this far from the nock (m)##bowfull", &f, 0.25f, 0.80f, "%.2f")) cmds += fmt::format("bowfull {:.2f}\n", f);
    hint("How far back your hand must pull for a full draw (the buzz). Pull to your usual full draw and read 'Now' above.");
    if (ImGui::SliderFloat("The game starts drawing once pulled (m)##bowstart", &s, 0.03f, 0.30f, "%.2f")) cmds += fmt::format("bowstart {:.2f}\n", s);
    hint("How far you pull back before the game starts drawing the bow.");
    if (ImGui::SliderFloat("Nock: how close the arrow must come (m)##bownockzone", &z, 0.10f, 0.35f, "%.2f")) cmds += fmt::format("bownockzone {:.2f}\n", z);
    hint("How close the arrow must come to the bow before it sticks to it.");
    if (ImGui::SliderInt("RT held at least (ms)##bowholdms", &h, 0, 1000)) cmds += fmt::format("bowholdms {}\n", h);
    hint("Shortest time the shot is held, so a quick pull still fires.");
    if (ImGui::SliderFloat("Full-draw buzz strength##bowbuzz", &a, 0.1f, 1.0f, "%.2f")) cmds += fmt::format("bowbuzz {:.2f}\n", a);
    int pn = std::clamp(g_pin.load(), 0, 2);
    const char* pin_items[]{ "off", "on the bow's sight pin", "where the arrow lands" };
    float shz = g_steady_hz.load();
    if (ImGui::SliderFloat("Steadiness while drawn (Hz, lower is steadier)##bowsteadyhz", &shz, 0.2f, 3.0f, "%.2f")) cmds += fmt::format("bowsteadyhz {:.2f}\n", shz);
    hint("How still the drawn bow holds. Lower: steadier, but slower to follow small corrections.");
    float sgz = sling_grab_zone();
    if (ImGui::SliderFloat("How close to the slung bow (m)##bowslinggrabzone", &sgz, 0.04f, 0.30f, "%.2f")) cmds += fmt::format("bowslinggrabzone {:.2f}\n", sgz);
    hint("How close your left hand must be to the slung bow to take it.");
    bool sbd = g_sling_body.load();
    if (ImGui::Checkbox("Slung bow across the body##bowslingbody", &sbd)) cmds += sbd ? "bowslingbody on\n" : "bowslingbody off\n";
    hint("The slung bow hangs flat across the front of your body. Off: it hangs like the rifle.");
    float sba = sling_body_deg();
    if (ImGui::SliderFloat("Slung bow's angle from upright (deg)##bowslingbodyangle", &sba, 0.0f, 90.0f, "%.0f")) cmds += fmt::format("bowslingbodyangle {:.0f}\n", sba);
    hint("0: the slung bow stands upright. 90: it lies flat across the belly.");
    float slh[3]{ g_sling_hold[0].load(), g_sling_hold[1].load(), g_sling_hold[2].load() };
    if (ImGui::SliderFloat3("Bow turn on the sling: right, up, roll (deg)##bowslinghold", slh, -180.0f, 180.0f, "%.0f"))
        cmds += fmt::format("bowslinghold {:.0f} {:.0f} {:.0f}\n", slh[0], slh[1], slh[2]);
    hint("An extra turn of the slung bow where it hangs.");
    if (ImGui::Combo("Sight dot##bowpin", &pn, pin_items, 3)) cmds += pn == 2 ? "bowpin aim\n" : pn == 1 ? "bowpin pin\n" : "bowpin off\n";
    hint("Where the arrow lands: a red dot on the target. On the bow's pin: the dot sits on the pin itself.");
    float mind = g_min_draw.load() * 100.0f;
    if (ImGui::SliderFloat("Shoots only drawn at least (%)##bowmindraw", &mind, 0.0f, 100.0f, "%.0f")) cmds += fmt::format("bowmindraw {:.2f}\n", mind / 100.0f);
    hint("Let go before this much of a full draw and the arrow stays on the bow instead of flying. 0: every release shoots.");
    const bool up = g_upright.load();
    if (up) {
        bool fl = g_flip.load();
        if (ImGui::Checkbox("Flip the bow (upside down)##bowflip", &fl)) cmds += fl ? "bowflip on\n" : "bowflip off\n";
        hint("Turns the bow upside down, for a bow that shows the wrong way up in your hand.");
        float hp[3]{ g_hand_pos[0].load(), g_hand_pos[1].load(), g_hand_pos[2].load() };
        if (ImGui::SliderFloat3("Bow in the hand: right, up, back (m)##bowhandpos", hp, -0.10f, 0.10f, "%.2f"))
            cmds += fmt::format("bowhandpos {:.2f} {:.2f} {:.2f}\n", hp[0], hp[1], hp[2]);
        hint("Moves the bow and your left hand on it.");
        float h[3]{ g_hold[0].load(), g_hold[1].load(), g_hold[2].load() };
        if (ImGui::SliderFloat3("Bow turn in the hand: right, up, roll (deg)##bowhold", h, -45.0f, 45.0f, "%.0f"))
            cmds += fmt::format("bowhold {:.0f} {:.0f} {:.0f}\n", h[0], h[1], h[2]);
        hint("Turns the bow in your hand if it doesn't sit right.");
    }
    const bool pl = g_pull.load();
    if (pl) {
        ImGui::Text("String: %s", g_pull_active.load() ? "follows the hand" : "waits for a first full draw with this bow");
        float sh[3]{ g_string_hand[0].load(), g_string_hand[1].load(), g_string_hand[2].load() };
        if (ImGui::SliderFloat3("String at the hand: right, up, back (m)##bowstringhand", sh, -0.15f, 0.15f, "%.2f"))
            cmds += fmt::format("bowstringhand {:.2f} {:.2f} {:.2f}\n", sh[0], sh[1], sh[2]);
        hint("Moves where the string sits from your right hand while drawing.");
        if (ImGui::Button("Learn the string again##bowpullforget")) cmds += "bowpullforget\n";
        hint("Forgets your saved draw lengths. The next full draw with each bow learns them again.");
    }
    const bool md = g_model_on.load();
    if (md && sig != 0 && is_bow(sig)) {
        const std::string parts = arrow_parts_text(sig);
        ImGui::Text("This bow's arrow: %s", parts.empty() ? "not learnt yet (shoot one arrow)" : fmt::format("learnt (draws of {} indices)", parts).c_str());
        if (!parts.empty() && ImGui::Button("Learn this bow's arrow again at the next shot##bowarrowforget")) {
            std::map<uint32_t, std::vector<uint32_t>> m;
            {
                std::lock_guard _{ g_list_mtx };
                m = g_arrow_parts;
            }
            m.erase(sig);
            std::string line = "bowarrowmesh";
            for (const auto& [w, v] : m) {
                line += fmt::format(" {}:", w);
                for (size_t i = 0; i < v.size(); ++i) line += (i ? "," : "") + std::to_string(v[i]);
            }
            if (m.empty()) line += " none";
            cmds += line + "\n";
        }
    }
}
}
