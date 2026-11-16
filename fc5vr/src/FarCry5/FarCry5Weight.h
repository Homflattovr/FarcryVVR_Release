#pragma once
// weaponweight (FarCry5Weight.cpp; author 11 Nov; research\2026-11-12_nativeweapon): how heavy the gun in hand feels. This header is the maths only (glm, no game, no log), so
// the same code runs in the offline harness (research\2026-11-12_nativeweapon\weight_harness.cpp).
//
// The model. The drawn gun is a mass held to the hand by a sprung, damped wrist: the hand (the weapon controller after the weapon
// smoothing) is the base, the gun follows it as
//     x'' = w^2 (y - x) + 2 z w (y' - x')        y the hand, x the gun, w = 1 / inertia time, z the drag (1 = no ringing)
// for the turn (about the grip, so the muzzle trails and the grip stays in the hand) and, more stiffly, for the place. The damper sits
// between the gun and the hand, so a steady move or turn is followed with no lag at all: the gun trails only while the hand speeds up
// (lag = acceleration / w^2), runs a little on when the hand stops, and settles. Slow aiming has almost no acceleration, so it stays
// exact. A hard limit (degrees, centimetres) keeps the gun from ever trailing far; a jump of the hand (a grab, a recentre) starts over.
// Heavier guns have a longer inertia time; two hands and the eye behind a sight shorten it. Everything that follows the gun (the drawn
// weapon, the hands on it, the barrel and the shots) takes this one pose, so the shots leave the gun as it is drawn.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fc5::weight
{
    // One step's settings, already scaled for the weapon, the hands and the sight.
    struct Params
    {
        float tau_turn{ 0.0f };   // s: the turn's inertia time (0 = the gun is the hand)
        float tau_move{ 0.0f };   // s: the place's
        float drag{ 1.0f };       // damping ratio, 0.5-2
        float lim_rad{ 0.07f };   // the gun never turns further than this from the hand
        float lim_m{ 0.01f };     // nor stands further than this from it (in the pose's own units: metres times the world scale)
    };

    struct State
    {
        bool      init{ false };
        glm::vec3 pos{ 0.0f }, vel{ 0.0f };                 // the gun's grip point and its speed
        glm::quat rot{ 1.0f, 0.0f, 0.0f, 0.0f };            // the gun's axes
        glm::vec3 wvel{ 0.0f };                             // its turn speed (rad/s, the pose's space)
        glm::vec3 t_pos{ 0.0f };                            // the hand at the last step
        glm::quat t_rot{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 off_pos{ 0.0f };                          // the gun against the hand after the last step (for a pair's second eye)
        glm::quat off_rot{ 1.0f, 0.0f, 0.0f, 0.0f };
        float     carry_dt{ 0.0f };                         // time of the calls that did not step (the pair's second eye)
    };

    struct Step
    {
        float lag_rad{ 0.0f }, lag_m{ 0.0f };   // the gun against the hand after this step
        bool  limited{ false }, reset{ false };
        int   substeps{ 0 };
    };

    // The turn from a to b as a vector (axis times angle, rad), the short way.
    inline glm::vec3 rotvec(const glm::quat& from, const glm::quat& to)
    {
        glm::quat d = to * glm::inverse(from);
        if (d.w < 0.0f) d = -d;
        const glm::vec3 v{ d.x, d.y, d.z };
        const float     s = glm::length(v);
        if (s < 1e-8f) return 2.0f * v;
        return v * (2.0f * std::atan2(s, d.w) / s);
    }
    inline glm::quat from_rotvec(const glm::vec3& r)
    {
        const float a = glm::length(r);
        if (a < 1e-8f) return glm::normalize(glm::quat{ 1.0f, 0.5f * r.x, 0.5f * r.y, 0.5f * r.z });
        const float s = std::sin(0.5f * a) / a;
        return glm::quat{ std::cos(0.5f * a), r.x * s, r.y * s, r.z * s };
    }

    inline void reset(State& st, const glm::vec3& p, const glm::quat& q)
    {
        st.init     = true;
        st.pos      = st.t_pos = p;
        st.rot      = st.t_rot = q;
        st.vel      = st.wvel = glm::vec3{ 0.0f };
        st.off_pos  = glm::vec3{ 0.0f };
        st.off_rot  = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        st.carry_dt = 0.0f;
    }

    // The hand's pose (p, q) in, the gun's out. same: the pair's second eye (the same controller sample): no step, the gun keeps its
    // place against the hand. dt: seconds since the last call.
    inline Step step(State& st, glm::vec3& p, glm::quat& q, float dt, bool same, const Params& pr)
    {
        Step out{};
        const bool off = pr.tau_turn < 0.002f && pr.tau_move < 0.002f;
        if (off) {   // weight 0: the pose is not touched, bit for bit
            st.init = false;
            return out;
        }
        q = glm::normalize(q);
        if (st.init && glm::dot(q, st.t_rot) < 0.0f) q = -q;
        // A jump of the hand (a grab from the sling, a recentre, a stall): start over on the hand.
        const bool jump = st.init && (glm::length(p - st.t_pos) > 0.25f || glm::length(rotvec(st.t_rot, q)) > 0.8f);
        if (!st.init || jump || dt <= 0.0f || dt + st.carry_dt > 0.25f) {
            out.reset = st.init;
            reset(st, p, q);
            return out;
        }
        if (same) {
            st.carry_dt += dt;
            const glm::vec3 np = p + st.off_pos;
            const glm::quat nq = glm::normalize(st.off_rot * q);
            out.lag_m   = glm::length(st.off_pos);
            out.lag_rad = glm::length(rotvec(q, nq));
            p = np;
            q = nq;
            return out;
        }
        const float T = dt + st.carry_dt;
        st.carry_dt   = 0.0f;
        // The hand moves evenly over the step: its speed and turn speed.
        const glm::vec3 tv = (p - st.t_pos) / T;
        const glm::vec3 tw = rotvec(st.t_rot, q) / T;
        const float     z  = std::clamp(pr.drag, 0.5f, 2.0f);
        const float     wt = pr.tau_turn >= 0.002f ? 1.0f / pr.tau_turn : 0.0f;
        const float     wm = pr.tau_move >= 0.002f ? 1.0f / pr.tau_move : 0.0f;
        const int       n  = std::clamp(int(std::ceil(std::max(wt, wm) * T / 0.15f)), 1, 64);
        const float     h  = T / float(n);
        out.substeps       = n;
        for (int i = 0; i < n; ++i) {
            // The spring pulls toward the hand as it stands at the start of the sub-step: a gun already moving with the hand (no error,
            // the same speed) then lands exactly on it at the end, so a steady turn has no lag whatever the step length.
            const float     f  = float(i) / float(n);
            const glm::vec3 yp = glm::mix(st.t_pos, p, f);
            const glm::quat yq = glm::normalize(from_rotvec(tw * (T * f)) * st.t_rot);
            if (wt > 0.0f) {
                const glm::vec3 e = rotvec(st.rot, yq);
                st.wvel += (wt * wt * e + 2.0f * z * wt * (tw - st.wvel)) * h;
                st.rot   = glm::normalize(from_rotvec(st.wvel * h) * st.rot);
            } else {
                st.rot  = glm::normalize(from_rotvec(tw * (T * float(i + 1) / float(n))) * st.t_rot);
                st.wvel = tw;
            }
            if (wm > 0.0f) {
                st.vel += (wm * wm * (yp - st.pos) + 2.0f * z * wm * (tv - st.vel)) * h;
                st.pos += st.vel * h;
            } else {
                st.pos = glm::mix(st.t_pos, p, float(i + 1) / float(n));
                st.vel = tv;
            }
        }
        // The hard limit: never further from the hand than this; the speed away from the hand is dropped.
        glm::vec3 e = rotvec(st.rot, q);
        if (const float a = glm::length(e); a > pr.lim_rad && a > 1e-6f) {
            const glm::vec3 d = e / a;
            st.rot            = glm::normalize(glm::inverse(from_rotvec(d * pr.lim_rad)) * q);
            const float away  = glm::dot(tw - st.wvel, d);
            if (away > 0.0f) st.wvel += d * away;
            out.limited = true;
        }
        glm::vec3 ep = p - st.pos;
        if (const float a = glm::length(ep); a > pr.lim_m && a > 1e-7f) {
            const glm::vec3 d = ep / a;
            st.pos            = p - d * pr.lim_m;
            const float away  = glm::dot(tv - st.vel, d);
            if (away > 0.0f) st.vel += d * away;
            out.limited = true;
        }
        st.t_pos    = p;
        st.t_rot    = q;
        st.off_pos  = st.pos - p;
        st.off_rot  = glm::normalize(st.rot * glm::inverse(q));
        out.lag_m   = glm::length(st.off_pos);
        out.lag_rad = glm::length(rotvec(q, st.rot));
        p = st.pos;
        q = st.rot;
        return out;
    }

    // The settings for one step from the weapon and how it is held. weight 0-1 (weaponweight / 100), heft: 1 = a rifle (FarCry5Weight.cpp
    // class table), two hands and a sighted eye lighten it. Lag grows with the square of the inertia time, so the time goes with the
    // square root of the weight: twice the weight, twice the lag.
    struct Tuning
    {
        float inertia_ms{ 38.0f };    // a rifle's turn inertia time at weight 100
        float move_share{ 0.45f };    // the place's inertia time against the turn's
        float drag{ 1.0f };
        float lim_deg{ 5.0f };        // a rifle's limits at weight 100
        float lim_cm{ 1.2f };
        float two_hand{ 0.6f };       // the inertia time's share with both hands on the gun
        float sighted{ 0.75f };       // and with the eye behind the sight
    };
    inline Params params(float weight, float heft, bool two_hand, bool sighted, float world_scale, const Tuning& tn)
    {
        Params p{};
        weight = std::clamp(weight, 0.0f, 1.0f);
        heft   = std::clamp(heft, 0.0f, 3.0f);
        if (weight <= 0.0f || heft <= 0.0f) return p;   // tau 0: the gun is the hand
        float tau = 0.001f * tn.inertia_ms * std::sqrt(weight * heft);
        if (two_hand) tau *= std::clamp(tn.two_hand, 0.2f, 1.0f);
        if (sighted) tau *= std::clamp(tn.sighted, 0.2f, 1.0f);
        p.tau_turn = tau;
        p.tau_move = tau * std::clamp(tn.move_share, 0.0f, 1.0f);
        p.drag     = tn.drag;
        const float h = 0.5f + 0.5f * std::min(heft, 2.0f);   // a handgun's limits are tighter than a machine gun's
        p.lim_rad  = glm::radians(std::clamp(tn.lim_deg, 0.5f, 15.0f) * h);
        p.lim_m    = 0.01f * std::clamp(tn.lim_cm, 0.1f, 5.0f) * h * std::max(1e-3f, world_scale);
        return p;
    }

    // --- The mod's side (FarCry5Weight.cpp) ---
    // weaponweight 0-100 (saved, default 0 = off), weaponweightinertia MS, weaponweightmove K, weaponweightdrag K, weaponweightlimit DEG CM,
    // weaponweighttwohand K, weaponweightsighted K (saved); weaponweightlog on|off, weaponweightheft H|off, handlinglog on|off (not saved).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Render thread, right after the weapon smoothing (FarCry5Stereo.cpp smooth_weapon): k the smoothed weapon pose in, the gun's pose out.
    // same: the pair's second eye (the same controller sample). fresh: the smoothing started over on this call (sling, bow, twohandkeep,
    // a stall), so the weight does too. With weaponweight 0, or a weapon with no weight, k is not touched.
    void apply(glm::mat4& k, bool same, bool fresh, uint32_t weapon_sig, bool two_hand, bool sighted, float world_scale);
    // handlinglog: this sample's weapon controller (ctrl), the gun the hold asked for (held: after two hands, the one-hand line, the
    // stock) and the gun as drawn (out: after the smoothing and the weight), all in update_motion's space. One [Handling] line per 10 s,
    // one after each weapon switch (the new gun's first 1.5 s) and one at the end of each manual reload (reload_now: a step of it is
    // under way), with the worst figures of that event.
    void handling_note(const glm::mat4& ctrl, const glm::mat4& held, const glm::mat4& out, bool hold_active, bool reload_now, uint32_t weapon_sig, float world_scale);
    // F11 Weapon > Weight: the settings, and what the gun in hand gets (its class's name and heft; nullptr = no weight).
    struct Settings { float weight{ 0.0f }, inertia_ms{ 38.0f }, move_share{ 0.45f }, drag{ 1.0f }, lim_deg{ 5.0f }, lim_cm{ 1.2f }, two_hand{ 0.6f }, sighted{ 0.75f }; };
    Settings settings();
    const char* weapon_kind(uint32_t weapon_sig, float& heft);
}
