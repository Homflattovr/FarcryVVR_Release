#pragma once
// cinetravel (default on, saved; F11 Cinematics; author 16 Nov 12:04, his
// helicopter-ride video and log of build EA39C89B).
//
// 3D cutscenes (cinematics 3d with cinesteer on) place the eyes at a smoothed copy of the scene camera's position (m_cine_cam). The old
// smoothing eased that copy 9.75% of the way to the camera each stereo pair and took the camera's position at once when it was over
// 1.5 m away, calling it a cut. A camera riding a vehicle moves that far every pair (the helicopter: 1.5-1.8 m a pair, about 60 m/s),
// so the eyes moved in a sawtooth. One pair jumped 1.5 m onto the camera (logged as a "cut", and the heading snapped too). The next pair
// stood still while the camera flew on another 1.5 m ([ViewJump]: base 60 m/s, then 1.5 m/s, 944 lines in one minute). Then it jumped
// again. Every other pair the eyes were 1.4 m behind the scene camera (behind the seat, through the cabin roof), and DLSS and the
// headset's reprojection smeared between the two places: the doubled faces and melting roof in his video. Any cutscene camera moving
// faster than about 6 m/s did this (cars, boats, planes, helicopters).
//
// Now the copy carries the camera's own travel: its velocity, low-passed, moves the copy each update, and the same easing removes what
// is left (shake). A camera moving steadily is followed with no lag. A still or slow camera (under 0.6 m/s) is smoothed exactly as
// before. A cut is a jump the camera's own motion does not explain (over 1.5 m from where its velocity puts it), not any 1.5 m step.
// The copy is never more than kMaxOff from the scene camera.
// Off (cinetravel off): as before.
//
// Round 2 (author 16 Nov 12:33 on F1F4455D). His 12:32-12:33 log: no more false
// cuts in the ride (0 in most 6 s lines), but (1) the copy still trailed the camera, up to the 25 cm limit 5-15 times every 6 s, so
// the cabin, under a metre away, moved 12-30 cm against the eyes on most pairs ([ViewJump], 54 of 179 pairs over 4 cm in 5 s): the
// game frames came 20-95 ms apart and the camera's step per frame does not follow the wall clock, so a carried velocity overshoots and
// undershoots; (2) the held heading was steered 20-45 deg round in 0.5-0.8 s every few seconds as the helicopter turned, and two
// "cuts" of 1.6 and 2.2 m off the travel (95 ms pairs) snapped it 5 and 32 deg at once.
// Now, riding (the camera's travel over kFast): the eyes stand exactly on the scene camera (the cabin and everything moving with it
// stay still around you; its shake is the cabin's shake), and the heading follows the scene camera's continuously (kRideYawTau), level,
// as a vehicle's turn does while playing. A cut while riding is a jump over kCut plus kCutRel of the step expected. A still or slow
// camera (under kSlow) keeps the old easing and the held heading with cinesteer.
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace fc5::cinetravel {

constexpr float kCut     = 1.5f;    // m: a jump beyond the camera's own motion is a new shot
constexpr float kFirstV  = 100.0f;  // m/s: the first step after a start or a cut is itself a cut when faster than this (plus kCut)
constexpr float kVelGain = 0.3f;    // the travel velocity's low-pass per update
constexpr float kSlow    = 0.6f;    // m/s: under this the copy is eased exactly as before (handheld sway is not carried)
constexpr float kFast    = 2.0f;    // m/s: over this the whole travel is carried
constexpr float kMaxOff  = 0.25f;   // m: the eyes' base at most this far from the scene camera
constexpr float kCutRel  = 0.75f;   // a cut while moving: off the expected step by kCut plus this share of the expected step
constexpr float kRideYawTau = 0.2f; // s: riding, the heading follows the scene camera's with this time constant

struct Filter {
    glm::vec3 pos{ 0.0f }, vel{ 0.0f }, raw_prev{ 0.0f };
    bool      ok = false, vel_ok = false;
    float     last_jump = 0.0f;   // m: the last update's distance from where the camera's motion put it (its step, the first time)
    float     last_limit = kCut;  // m: the cut limit the last update used
    float     dt_prev = 0.0f;     // s: the update before's dt
    float     w = 0.0f;           // riding: 0 still (old easing), 1 the camera's travel over kFast (the eyes on the camera)
    // since take_stats()
    int   updates = 0, cuts = 0, clamped = 0, carried = 0;
    float worst_off = 0.0f, worst_speed = 0.0f;

    void reset() { ok = false; vel_ok = false; }
    void take_stats() { updates = cuts = clamped = carried = 0; worst_off = worst_speed = 0.0f; }

    // One update per stereo pair (or per eye frame without pairs). dt: seconds since the last update; ease: the old easing per update
    // (0.0975 a pair, 0.05 an eye frame). Returns true on a cut (the copy then takes the camera's position at once).
    bool update(const glm::vec3& cam, float dt, bool fresh, float ease) {
        dt = std::clamp(dt, 0.004f, 0.1f);
        const float dt_last = std::exchange(dt_prev, dt);
        updates++;
        if (fresh || !ok) {
            pos = cam; vel = glm::vec3{ 0.0f }; raw_prev = cam; ok = true; vel_ok = false; last_jump = 0.0f; w = 0.0f;
            return false;
        }
        const glm::vec3 step = cam - raw_prev;
        raw_prev             = cam;
        if (!vel_ok) {
            // The first step after a start or a cut: the camera's velocity is not known yet; a plausible step starts it.
            last_jump = glm::length(step);
            pos       = cam;
            if (last_jump > kCut + kFirstV * dt) { vel = glm::vec3{ 0.0f }; cuts++; return true; }
            vel    = step / dt;
            vel_ok = true;
            return false;
        }
        // A cut: the camera far from where its own motion would have taken it. The game's frames are not as far apart as the wall clock
        // says (20-95 ms pairs; a frame's step can be the length of the pair before), so the limit grows with the step expected.
        last_jump  = glm::length(step - vel * dt);
        last_limit = kCut + kCutRel * glm::length(vel) * std::max(dt, dt_last);
        if (last_jump > last_limit) {
            pos = cam; vel = glm::vec3{ 0.0f }; vel_ok = false; cuts++;
            return true;
        }
        vel += (step / dt - vel) * kVelGain;
        const float sp = glm::length(vel);
        const float w0 = std::clamp((sp - kSlow) / (kFast - kSlow), 0.0f, 1.0f);
        w              = w0 * w0 * (3.0f - 2.0f * w0);
        if (w > 0.0f) carried++;
        pos += vel * (w * dt);                         // the camera's steady travel, carried
        pos += (cam - pos) * (ease + (1.0f - ease) * w);   // the rest eased; riding (w 1) the eyes on the camera itself
        const glm::vec3 off = pos - cam;
        const float     d   = glm::length(off);
        if (d > kMaxOff) { pos = cam + off * (kMaxOff / d); clamped++; }
        worst_off   = std::max(worst_off, std::min(d, kMaxOff));
        worst_speed = std::max(worst_speed, sp);
        return false;
    }
};

}   // namespace fc5::cinetravel
