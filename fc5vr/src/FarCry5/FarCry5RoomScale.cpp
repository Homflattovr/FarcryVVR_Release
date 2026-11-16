// Room scale, first version: the rule (see FarCry5RoomScale.h). No game or framework code here; FarCry5RoomScaleMod.cpp feeds
// step() once per picture and applies its answer, the pad pollers call left_stick().
#include "FarCry5RoomScale.h"

#ifndef FC5_ROOMSCALE_HARNESS
#include <imgui.h>
#endif
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <istream>

namespace fc5::roomscale
{
    namespace
    {
        // Settings.
        std::atomic<bool>  g_on{ false };       // roomscale on|off (saved)
        std::atomic<float> g_start{ 0.12f };    // roomscalestart cm (saved): the neck this far from the body starts a walk
        std::atomic<float> g_stop{ 0.04f };     // roomscalestop cm (test): the walk ends under this
        std::atomic<float> g_lo{ 0.30f };       // roomscalestick LO HI (test): where a push starts (the game's dead zone is 0.24) ...
        std::atomic<float> g_hi{ 0.85f };       // ... and the most it grows to while the body answers
        std::atomic<int>   g_log{ 0 };          // roomscalelog N (test)
        std::atomic<bool>  g_turnhead{ false };  // turnhead on|off (saved)

        constexpr float kMaxDebt    = 1.00f;    // m: further than this is no step (tracking lost and found, a recentre due) ...
        constexpr float kMaxFollow  = 1.60f;    // ... but a walk that runs goes on up to this (the head walks faster than the body picks up speed)
        constexpr float kStillSpeed = 0.50f;    // m/s: a walk starts from a body slower than this ...
        constexpr int   kStillSteps = 3;        // ... for this many pictures
        constexpr float kMoving     = 0.12f;    // m/s: the body answers the push
        constexpr float kStallGrace = 0.30f;    // s without an answer before the push grows
        constexpr float kBoostRate  = 0.60f;    // stick per second, until the body first answers ...
        constexpr float kBoostFar   = 1.20f;    // ... when the head is far (kFar) and walking away meanwhile ...
        constexpr float kBoostRate2 = 2.00f;    // ... and when a body that walked stops answering (something in its way)
        constexpr float kFar        = 0.30f;    // m
        constexpr float kAnswerLagS = 0.13f;    // s the push went on growing between moving the body and the mod seeing it move
        constexpr float kFloorMax   = 0.85f;    // the learned start of a push, at most
        constexpr float kFloorTry   = 0.05f;    // a start learned in a hurry is tried this much lower each time
        constexpr float kBlockedS   = 0.40f;    // s at full push without an answer: blocked
        constexpr float kRetryS     = 3.00f;    // a blocked way is tried again after this
        constexpr float kSettleS    = 1.50f;    // the view is held this long at most for the body's last slide
        constexpr float kLoop       = 2.00f;    // 1/s: how fast the push is corrected towards the speed wanted (over the game's m/s per stick, below) ...
        constexpr float kLoopMin    = 0.80f;    // ... slower for a body that answers late (half its slide time, inverted)
        constexpr float kGain0      = 5.00f, kGainMin = 2.0f, kGainMax = 15.0f;   // m/s per stick: the game's answer, learned while it walks
        constexpr float kSpeedMax   = 3.00f;    // m/s: the body is never asked to go faster
        constexpr float kBrake0     = 0.20f;    // s: the body's slide after the push ends, as a time at its speed (learned from each stop)
        constexpr float kBrakeMin   = 0.08f, kBrakeMax = 0.60f;
        constexpr float kWrongS     = 0.40f;    // s the body may walk away from the head before the walk is given up
        constexpr int   kTurnsMax   = 2;        // pushes back after walking past the head, per walk
        constexpr float kRunaway    = 7.00f;    // m/s: the body moves by something else (a fall, a vehicle, a teleport)
        constexpr float kGapS       = 0.25f;    // a picture this late is a gap (a hitch, a load)
        constexpr float kUserOn     = 0.18f, kUserOff = 0.10f;   // the player's own stick, in and out
        constexpr int   kStaleMs    = 150;      // no step for this long: the pollers send nothing
        constexpr float kFloorHurry = 0.15f;    // a start learned in a hurry is kept at most this far above the last calm one (camjitter)
        constexpr float kUserFreeS  = 0.50f;    // s the player's own stick must have been let go before a walk starts (camjitter)
        constexpr float kLookDown   = 0.5236f;  // rad (30 deg): the head looking further down than this starts no walk (a bend, not a step)

        // To the pad pollers.
        std::atomic<float>   g_sx{ 0.0f }, g_sy{ 0.0f };
        std::atomic<bool>    g_inject{ false };
        std::atomic<int64_t> g_stamp_ms{ 0 };
        std::atomic<float>   g_user{ 0.0f };    // the player's own left stick at the last poll
        std::atomic<float>   g_sent{ 0.0f };    // what the last poll added (for the log)

        enum class St { Idle, Follow, Settle };
        const char* name(St s) { return s == St::Follow ? "follow" : s == St::Settle ? "settle" : "idle"; }

        // Render thread only.
        struct State
        {
            St        st{ St::Idle };
            glm::vec2 body{ 0.0f };
            bool      body_ok{ false };
            float     speed{ 0.0f };        // the body's, eased over ~0.1 s
            int       still{ 0 };           // pictures in a row under kStillSpeed
            bool      user{ false };
            float     user_free{ 10.0f };   // s since the player's own stick was let go (none yet: long ago)
            double    clock{ 0.0 };         // s, the steps' own
            // The view's hold: what the body moved since the walk began, what the centre was moved by.
            glm::vec2 moved{ 0.0f }, applied{ 0.0f };
            glm::vec2 db_prev{ 0.0f };      // rslead: the body's step in the last picture
            // The walk.
            double    t0{ 0.0 };
            float     debt0{ 0.0f }, stall{ 0.0f }, full{ 0.0f }, settle{ 0.0f }, mag_max{ 0.0f }, path{ 0.0f };
            float     m{ 0.0f };            // the push now
            float     m0{ 0.0f };           // ... and where this push began
            float     va{ 0.0f };           // the body's speed towards the head (eased, 0.05 s)
            float     close{ 0.0f };        // how fast the distance shrinks (eased, 0.06 s): the body's speed less the head's
            float     d_prev{ 0.0f };
            float     wrong{ 0.0f };        // s the body has walked away from the head
            float     ramp{ 0.0f };         // how fast the push grew while the body did not answer (stick per second)
            bool      answered{ false };
            int       turns{ 0 };
            glm::vec2 dir{ 0.0f };          // the way this push began
            // The stop being measured: the body's speed and path when the push ended.
            bool      rel{ false };
            float     rel_v{ 0.0f }, rel_path{ 0.0f };
            // Learned from the game's answers (session): the push that moves the body, its slide after a push.
            float     floor{ 0.0f };
            bool      floor_sure{ false };
            float     brake{ kBrake0 };
            float     gain{ kGain0 };
            // A blocked way.
            bool      blocked{ false };
            glm::vec2 blocked_dir{ 0.0f };
            double    blocked_t{ 0.0 };
            // Log.
            const char* why{ "" };
            uint64_t  walks{ 0 }, blocks{ 0 };
            double    sum_t{ 0.0 };
            uint64_t  sum_walks{ 0 }, sum_blocks{ 0 };
            float     sum_path{ 0.0f };
        } g;

        float yaw_of(const glm::vec2& f) { return std::atan2(-f.x, f.y); }   // as FarCry5Stereo.cpp: 0 = +y, counter-clockwise
        int64_t clock_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        void stop_push()
        {
            g_inject = false;
            g_sx = g_sy = 0.0f;
        }

        void end_walk(const char* how, float left, bool quiet = false)
        {
            auto& s = g;
            if (!quiet && (s.walks <= 200 || (s.walks % 20) == 0))
                spdlog::info("[Room] {} after {:.2f} s: the body walked {:.0f} cm with the view held, the head {:.0f} cm from it now (was {:.0f}); push up to {:.2f}", how,
                             s.clock - s.t0, s.path * 100.0f, left * 100.0f, s.debt0 * 100.0f, s.mag_max);
            s.sum_path += s.path;
            s.st = St::Idle;
            stop_push();
        }
    }

    bool on() { return g_on.load(); }
    bool turnhead_on() { return g_turnhead.load(); }

    glm::vec2 turn_fix(const glm::vec2& p, float yaw0, float yaw1)
    {
        // The head's offset across the ground in world axes (as roomscale's debt: right * x - forward * z), then read back in the turned frame.
        auto            fwd = [](float y) { return glm::vec2{ -std::sin(y), std::cos(y) }; };
        auto            rgt = [](float y) { return glm::vec2{ std::cos(y), std::sin(y) }; };
        const glm::vec2 w   = rgt(yaw0) * p.x - fwd(yaw0) * p.y;
        return p - glm::vec2{ glm::dot(w, rgt(yaw1)), -glm::dot(w, fwd(yaw1)) };
    }

    Out step(const In& in)
    {
        Out   out{};
        auto& s = g;
        const float start = g_start.load(), stop = std::min(g_stop.load(), 0.8f * start);
        const float d     = glm::length(in.debt);
        const bool  gap   = !(in.dt > 1e-4f) || in.dt > kGapS;
        s.clock += gap ? 0.0 : double(in.dt);
        g_stamp_ms = in.now_ms ? in.now_ms : clock_ms();

        // The player's own stick (the pollers'), with a little hold either way.
        const float user = g_user.load();
        s.user           = user > (s.user ? kUserOff : kUserOn);
        s.user_free      = s.user ? 0.0f : std::min(s.user_free + (gap ? 0.0f : in.dt), 10.0f);
        const bool look_down = in.head_pitch < -kLookDown;

        // The body's step in this picture.
        glm::vec2 db{ 0.0f };
        const bool have = in.body_ok && s.body_ok && !gap;
        if (have) db = in.body - s.body;
        s.body    = in.body;
        s.body_ok = in.body_ok;
        const float v = have ? glm::length(db) / in.dt : 0.0f;
        if (have) s.speed += (v - s.speed) * (1.0f - std::exp(-in.dt / 0.1f));
        else s.speed = 0.0f;
        s.still = (have && v < kStillSpeed) ? std::min(s.still + 1, 1000) : 0;

        const bool  was_walking = s.st != St::Idle;
        const bool  gate        = g_on.load() && in.ok && in.body_ok && !s.user;
        const char* why         = !g_on.load() ? "off" : !in.ok ? in.why : !in.body_ok ? "no body under the eyes in this picture" : s.user ? "the left stick is pushed" : "";

        // The view's hold: while a walk runs, everything the body moves across the ground.
        if (was_walking) {
            if (!have) end_walk("stopped (a gap in the pictures)", d);
            else if (v > kRunaway) end_walk("stopped (the body moved by something else)", d);
            else {
                s.moved += db;
                s.path  += glm::length(db);
            }
        }

        // A blocked way opens again: the head came back, went another way, or after a while.
        if (s.blocked) {
            const bool turned = d > 1e-3f && glm::dot(in.debt / d, s.blocked_dir) < 0.766f;   // 40 deg
            if (d < 0.75f * start || turned || s.clock - s.blocked_t > kRetryS) s.blocked = false;
        }

        // The body's speed towards the head, and how fast the distance shrinks (the head moves too).
        const glm::vec2 dir = d > 1e-4f ? in.debt / d : glm::vec2{ 0.0f };
        if (s.st != St::Idle && have) {
            s.va    += (glm::dot(db, dir) / in.dt - s.va) * (1.0f - std::exp(-in.dt / 0.05f));
            s.close += ((s.d_prev - d) / in.dt - s.close) * (1.0f - std::exp(-in.dt / 0.06f));
        }
        s.d_prev = d;

        const float lo = std::clamp(std::max(g_lo.load(), s.floor), 0.05f, 1.0f), hi = std::clamp(g_hi.load(), lo, 1.0f);
        auto push_begins = [&](bool walking) {   // towards the head as it is now; a body still walking keeps most of its push
            s.st       = St::Follow;
            s.m = s.m0 = walking ? std::max(lo, 0.8f * s.m) : s.floor_sure ? lo : std::max(std::min(lo, g_lo.load()), lo - kFloorTry);
            s.stall = s.full = s.wrong = 0.0f;
            s.answered = walking;
            s.ramp     = 0.0f;
            s.rel      = false;
            s.dir      = dir;
        };
        auto push_ends = [&] {   // the body slides on: the view is held, the slide measured
            s.st       = St::Settle;
            s.settle   = 0.0f;
            s.rel      = s.va > 0.25f;
            s.rel_v    = s.va;
            s.rel_path = s.path;
        };
        auto give_up = [&](const char* how) {   // this way is left alone until the head comes back, goes another way, or a while has passed
            s.blocked     = true;
            s.blocked_dir = dir;
            s.blocked_t   = s.clock;
            s.blocks++;
            s.sum_blocks++;
            end_walk(how, d);
        };

        // camjitter: a walk waits for the player's own stick to have been let go a while (not between two pushes of his own), and looking down
        // is a bend at the hips, not a step.
        const bool may_start = gate && have && d > start && d < kMaxDebt && !s.blocked && !look_down && s.user_free >= kUserFreeS;

        switch (s.st) {
        case St::Idle:
            if (may_start && s.still >= kStillSteps) {
                s.t0    = s.clock;
                s.debt0 = d;
                s.settle = s.mag_max = s.path = s.va = s.close = 0.0f;
                s.turns  = 0;
                s.moved = s.applied = glm::vec2{ 0.0f };
                s.walks++;
                s.sum_walks++;
                push_begins(false);
                if (s.walks <= 200 || (s.walks % 20) == 0)
                    spdlog::info("[Room] the head is {:.0f} cm from the body ({:+.0f} deg from the game's walk direction): the body walks to it (walk {})", d * 100.0f,
                                 glm::degrees(std::remainder(yaw_of(in.debt) - in.game_yaw, 6.2831853f)), s.walks);
            }
            break;
        case St::Follow:
            if (!gate) end_walk(why[0] ? why : "stopped", d, !g_on.load());
            else if (d > kMaxFollow) end_walk("stopped (the head too far from the body)", d);
            else if (d < stop) push_ends();
            else {
                const bool moving = s.speed > kMoving;
                if (moving) {
                    if (!s.answered && s.ramp > 0.0f) {
                        // The push that first moved it, less what it grew while the answer was on its way: pushes start there from now on.
                        // Learned in a hurry (the head walking away) it is too high: the next pushes try a little under it.
                        // camjitter: one learned in a hurry (the push growing fast) overshoots by the game's pick-up time: it may raise the start
                        // only a little above the last calm one (at 22:21:11 it put every later push at 0.85, nearly a run for a 12 cm step).
                        const float found = std::clamp(s.m - kAnswerLagS * s.ramp, s.m0, kFloorMax);
                        const bool  sure  = s.ramp <= kBoostRate + 1e-3f;
                        const float kept  = sure ? found : std::min(found, std::max(s.floor_sure ? s.floor : 0.0f, g_lo.load()) + kFloorHurry);
                        if (std::abs(kept - s.floor) > 0.01f)
                            spdlog::info("[Room] the game's body first moved at a push of {:.2f} (started at {:.2f}): pushes start at {:.2f} from now on{}", s.m, s.m0, kept,
                                         sure ? "" : kept < found - 0.005f ? " (learned in a hurry: kept near the last calm start, lower ones are tried)" : " (learned in a hurry: lower ones are tried)");
                        s.floor      = kept;
                        s.floor_sure = sure;
                        s.m          = found;
                    } else if (!s.answered && !s.floor_sure && s.floor > 0.0f) {
                        s.floor = s.m0 > g_lo.load() + 0.005f ? s.m0 : 0.0f;   // it moved at the lower start
                    }
                    s.answered = true;
                    s.stall = s.full = 0.0f;
                    // The speed the body can still stop from before the head: the distance left over its slide (a little more, for room).
                    // The push follows the speed, as fast as the game's answer to the stick allows (learned too: m/s per stick past the dead zone).
                    const float want = std::min(kSpeedMax, (d - stop) / (1.3f * s.brake + 0.05f));
                    if (s.va > 0.2f) s.gain += (std::clamp(s.va / std::max(0.05f, s.m - 0.20f), kGainMin, kGainMax) - s.gain) * (1.0f - std::exp(-in.dt / 0.25f));
                    s.m = std::clamp(s.m + std::clamp(0.5f / s.brake, kLoopMin, kLoop) / s.gain * (want - s.va) * in.dt, lo, d > 0.6f ? 1.0f : hi);
                } else {
                    // No answer: after a while the push grows; none at full push, the way is blocked.
                    // (Slowly the first time, to learn where the body starts to move; faster when the head is far and walking away.)
                    const bool is_far = d > kFar && s.close < -0.3f;
                    s.stall += in.dt;
                    if (s.stall > (is_far ? 0.5f : 1.0f) * kStallGrace) {
                        s.ramp = s.answered ? kBoostRate2 : std::max(s.ramp, is_far ? kBoostFar : kBoostRate);
                        s.m    = std::min(1.0f, s.m + s.ramp * in.dt);
                    }
                    if (s.m >= 0.999f) s.full += in.dt;
                }
                s.wrong = (s.answered && s.va < -0.15f) ? s.wrong + in.dt : 0.0f;
                if (s.full > kBlockedS) give_up("blocked (the body does not move at full push: a wall or an edge it cannot pass)");
                else if (s.wrong > kWrongS) give_up("stopped (the push walks the body away from the head)");
                else if (s.answered && d - 0.5f * stop <= std::max(s.close, 0.0f) * s.brake) {
                    // Its slide brings it there. A head that walks on is not stopped under: the push drops to the head's pace (what the
                    // game's answer says that takes) and the walk goes on.
                    const float head = s.va - s.close;   // the head's own speed away from the body
                    if (head > 0.3f && d > start) s.m = std::clamp(0.20f + head / s.gain, lo, s.m);
                    else push_ends();
                }
            }
            break;
        case St::Settle: {
            s.settle += in.dt;
            const bool rest = s.speed < 0.08f;
            if (s.rel && rest) {
                // The slide that followed the push, as a time at the speed it had: the next push ends that early.
                const float t = (s.path - s.rel_path) / s.rel_v;
                s.brake       = std::clamp(0.5f * s.brake + 0.5f * t, kBrakeMin, kBrakeMax);
                s.rel         = false;
            }
            if (!gate) end_walk(why[0] ? why : "stopped", d, !g_on.load());
            else if (d > start && !look_down && (rest || (s.va > 0.0f && d - std::max(s.close, 0.0f) * s.brake > start))) {
                // Not there: the head walks on (the body still on its way pushes again at once), or the body stopped short or went past it.
                if (rest && glm::dot(dir, s.dir) < 0.0f && ++s.turns > kTurnsMax) give_up("stopped (the body does not come to rest under the head)");
                else if (d > kMaxFollow) end_walk("stopped (the head too far from the body)", d);
                else push_begins(!rest);
            } else if (rest && s.settle > 0.1f) end_walk("arrived", d);
            else if (s.settle > kSettleS) end_walk("arrived (the body still sliding)", d);
            break;
        }
        }
        const float mag = s.st == St::Follow ? s.m : 0.0f;

        // The push, in the game's stick frame (x right, y forward): a direction a to the left of the game's walk direction is (-sin a, cos a).
        if (s.st == St::Follow && mag > 0.0f && d > 1e-4f) {
            const float a = yaw_of(in.debt) - in.game_yaw;
            out.inject    = true;
            out.sx        = -std::sin(a) * mag;
            out.sy        = std::cos(a) * mag;
            s.mag_max     = std::max(s.mag_max, mag);
            g_sx          = out.sx;
            g_sy          = out.sy;
            g_inject      = true;
        } else {
            stop_push();
        }

        // The centre: by what the body has moved, plus this picture's step once more for the next one (the body moves on before the
        // next step can count it; without it the view trails the body by a picture's walk, 2 cm at 1 m/s). Given back as the body slows.
        // rslead (simulator 14 Nov 01:16, rs_run C2-C4): the game's body ends each push by sliding on, then stepping back 0.7-1.8 cm as it
        // stops. This picture's step taken once more for the next one put the view 2-3.6 cm off for a picture at the end of a walk. In the
        // slide the step ahead is the next step as the slowing goes on (this step less what it lost since the last), never backwards and
        // never more than this step: a smooth slide is still led, an abrupt stop no longer overshoots.
        const bool hold = s.st != St::Idle;
        glm::vec2  lead = hold ? db : glm::vec2{ 0.0f };
        if (hold && s.st == St::Settle) {
            const float l = glm::length(db), lp = glm::length(s.db_prev);
            const float along = lp > 1e-6f ? glm::dot(db, s.db_prev) / lp : l;   // this step along the last one
            lead = l > 1e-6f ? db * std::clamp((2.0f * along - lp) / l, 0.0f, 1.0f) : glm::vec2{ 0.0f };
        }
        s.db_prev = have ? db : glm::vec2{ 0.0f };
        if (was_walking) {
            const glm::vec2 want = s.moved + lead;
            out.shift            = want - s.applied;
            if (glm::length(out.shift) > 0.30f) {   // never a jump
                out.shift = glm::vec2{ 0.0f };
                if (hold) end_walk("stopped (the body jumped)", d);
            }
            s.applied += out.shift;
        }
        out.following = s.st != St::Idle;

        if (why != s.why && std::string{ why } != s.why) {
            // One line per change of what holds room scale back (not the stick: it comes and goes with every walk by stick).
            static int said = 0;
            if (g_on.load() && !s.user && said++ < 200) spdlog::info("[Room] {}", why[0] ? fmt::format("waiting: {}", why) : std::string{ "ready (on foot, standing: the body follows the head)" });
            s.why = why;
        }
        if (g_on.load() && s.clock - s.sum_t > 30.0) {
            if (s.sum_walks > 0)
                spdlog::info("[Room] last 30 s of play: {} walks to the head ({:.1f} m with the view held), {} given up; pushes start at {:.2f}, the body's slide {:.0f} ms", s.sum_walks,
                             s.sum_path, s.sum_blocks, std::max(g_lo.load(), s.floor), s.brake * 1000.0f);
            s.sum_t     = s.clock;
            s.sum_walks = s.sum_blocks = 0;
            s.sum_path  = 0.0f;
        }
        if (g_log.load() > 0) {
            // roomscalelog: anchor is body + debt + what the centre still owes: the neck's place in the world, which a walk must not move.
            g_log--;
            const glm::vec2 anchor = in.body + in.debt;
            spdlog::info("[Room] log frame {} dt {:.1f} ms | debt {:+.4f} {:+.4f} ({:.1f} cm) | body {:.4f} {:.4f} speed {:.2f} | anchor {:.4f} {:.4f} | {} push {:+.2f} {:+.2f} (to the head {:+.2f} m/s, "
                         "closing {:+.2f}, sent {:.2f}) | shift {:+.4f} {:+.4f} | game yaw {:.1f} | own stick {:.2f} | {}",
                         in.frame, in.dt * 1000.0f, in.debt.x, in.debt.y, d * 100.0f, in.body.x, in.body.y, s.speed, anchor.x, anchor.y, name(s.st), out.sx, out.sy, s.va,
                         s.close, g_sent.load(), out.shift.x, out.shift.y, glm::degrees(in.game_yaw), user, why[0] ? why : "ok");
        }
        return out;
    }

    void left_stick_at(int16_t& lx, int16_t& ly, bool blocked, int64_t now_ms)
    {
        const float ux = float(lx) / 32767.0f, uy = float(ly) / 32767.0f;
        const float um = std::hypot(ux, uy);
        g_user         = blocked ? 0.0f : um;
        g_sent         = 0.0f;
        if (blocked || !g_on.load() || !g_inject.load() || um > kUserOff || now_ms - g_stamp_ms.load() > kStaleMs) return;
        const float x = g_sx.load(), y = g_sy.load();
        lx            = int16_t(std::clamp((ux + x) * 32767.0f, -32767.0f, 32767.0f));
        ly            = int16_t(std::clamp((uy + y) * 32767.0f, -32767.0f, 32767.0f));
        g_sent        = std::hypot(x, y);
    }

    void left_stick(int16_t& lx, int16_t& ly, bool blocked) { left_stick_at(lx, ly, blocked, clock_ms()); }
    bool pushing() { return g_on.load() && g_inject.load() && clock_ms() - g_stamp_ms.load() <= kStaleMs; }

    std::string status()
    {
        const auto& s = g;
        return fmt::format("roomscale {}: {}; {} walks, {} given up; a walk starts at {:.0f} cm and ends under {:.0f}; pushes {:.2f} to {:.2f}{}; the body's slide {:.0f} ms; {}",
                           g_on.load() ? "on" : "off", name(s.st), s.walks, s.blocks, g_start.load() * 100.0f, g_stop.load() * 100.0f, std::max(g_lo.load(), s.floor), g_hi.load(),
                           s.floor > 0.0f ? " (the start learned from the game's answer)" : "", s.brake * 1000.0f, s.why[0] ? s.why : "ready");
    }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        if (cmd == "turnhead") {   // turnhead on|off (saved)
            const bool on = arg.empty() || arg == "on" || arg == "1" || arg == "true";
            g_turnhead    = on;
            persist       = true;
            spdlog::info("[Turn] turnhead {}", on ? "on: snap and smooth turns on foot turn the view about the head (the head keeps its place)"
                                                  : "off: turns turn the view about the game's body, as before");
            return true;
        }
        if (cmd.rfind("roomscale", 0) != 0) return false;
        if (cmd == "roomscale") {   // roomscale on|off (saved)
            const bool on = arg.empty() || arg == "on" || arg == "1" || arg == "true";
            if (on != g_on.exchange(on)) g = State{};   // a fresh start either way (what the game's answer taught is learned again)
            persist       = true;
            stop_push();
            spdlog::info("[Room] roomscale {}", on ? "on: on foot, the game's body walks under the headset (the left stick pushed by the mod), the view stays with the head"
                                                   : "off: the head moves off the body as before (autorecenter past its distance)");
            return true;
        }
        if (cmd == "roomscalestart") { g_start = std::clamp(fv, 5.0f, 40.0f) * 0.01f; persist = true; return true; }   // roomscalestart CM (saved)
        if (cmd == "roomscalestop") { g_stop = std::clamp(fv, 1.0f, 20.0f) * 0.01f; return true; }                     // roomscalestop CM (test)
        if (cmd == "roomscalebrake") { g.brake = std::clamp(fv * 0.001f, kBrakeMin, kBrakeMax); return true; }         // roomscalebrake MS (test): the slide, learned on from there
        if (cmd == "roomscalestick") {   // roomscalestick LO HI (test): where a push starts and the most it grows to; what was learned is forgotten
            float hi = 0.0f;
            rest >> hi;
            g_lo = std::clamp(fv, 0.05f, 1.0f);
            if (hi > 0.0f) g_hi = std::clamp(hi, 0.05f, 1.0f);
            g.floor      = 0.0f;
            g.floor_sure = false;
            spdlog::info("[Room] {}", status());
            return true;
        }
        if (cmd == "roomscalelog") { g_log = arg.empty() ? 600 : std::max(0, int(fv)); return true; }   // roomscalelog N (test)
        if (cmd == "roomscalestatus") { spdlog::info("[Room] {}", status()); return true; }
        return false;
    }

#ifndef FC5_ROOMSCALE_HARNESS
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        bool th = g_turnhead.load();
        if (ImGui::Checkbox("Turns about your head, not your game body (test)##turnhead", &th)) cmds += th ? "turnhead on\n" : "turnhead off\n";
        hint("For testing: turning turns the view around your head instead of the game body.");
        bool on = g_on.load();
        if (ImGui::Checkbox("Room scale: walking in your room walks your body in the game (test)##roomscale", &on)) cmds += on ? "roomscale on\n" : "roomscale off\n";
        hint("For testing: walking in your room walks your character in the game.");
        if (on) {
            int cm = int(std::lround(g_start.load() * 100.0f));
            if (ImGui::SliderInt("The body follows once your head is this far from it##roomscalestart", &cm, 5, 40, "%d cm")) cmds += fmt::format("roomscalestart {}\n", cm);
            hint("How far your head must move before the body follows. Larger: you can lean further first.");
            ImGui::TextDisabled("%s", status().c_str());
        }
    }
#endif
}
