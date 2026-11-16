#pragma once
// weaponrest hold (FarCry5Stereo.cpp smooth_weapon_step; research\2026-11-12_nativeweapon): the gun at rest held by play instead of by
// a slow glide. This header is the maths only (glm, no game, no log), shared with the offline harness (rest_harness.cpp).
//
// Why. The weapon smoothing at rest is a 1.6 Hz low-pass (weaponsteady): it calms tremor, but after every small move of the hand
// the gun keeps gliding to it for 0.2-0.3 s. In the author's play of 9 Nov the steadied gun moved more than the hand read still (the
// sight point 25 cm ahead: 0.50 mm per reading against 0.38, 3.5 times more in the worst tenth; the turn 1.8 times more).
// A hand held still does not shake fast and small, it drifts slowly: a low-pass follows a drift whole, and adds its own catching up.
//
// What. The gun follows the hand through a little play (as a peg in a slightly larger hole): while the hand stays within the play
// the gun does not move at all; once the hand pushes past it the gun moves with it, exactly as far. A gun held this way never
// travels further than the hand does, and it stops when the hand stops: nothing glides. The play closes as the hand speeds up
// (none left from a few cm/s and a few deg/s), so a real move is followed with no offset.
#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fc5::rest
{
    struct Params
    {
        float play_m{ 0.0003f };      // the place's play at rest (the pose's own units: metres times the world scale)
        float play_rad{ 0.00052f };   // the turn's play at rest (0.03 deg: 0.13 mm at a sight 25 cm ahead)
        float v_lo{ 0.010f }, v_hi{ 0.040f };   // m/s: the place's play is whole below v_lo, gone from v_hi
        float w_lo{ 0.035f }, w_hi{ 0.140f };   // rad/s (2 and 8 deg/s): the turn's
        float speed_hz{ 3.0f };       // the speeds are smoothed at this cutoff
    };

    struct State
    {
        bool      init{ false };
        glm::vec3 pos{ 0.0f };                        // the gun
        glm::quat rot{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 in_pos{ 0.0f };                     // the hand at the last step
        glm::quat in_rot{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 vel{ 0.0f }, wvel{ 0.0f };          // its smoothed speed and turn speed
    };

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
        st.init   = true;
        st.pos    = st.in_pos = p;
        st.rot    = st.in_rot = q;
        st.vel    = st.wvel = glm::vec3{ 0.0f };
    }

    // The hand's pose (p, q) in, the gun's out. dt: seconds since the last step. fresh: start over on the hand.
    inline void step(State& st, glm::vec3& p, glm::quat& q, float dt, bool fresh, const Params& pr)
    {
        q = glm::normalize(q);
        if (st.init && glm::dot(q, st.in_rot) < 0.0f) q = -q;
        if (!st.init || fresh || dt <= 0.0f || dt > 0.25f || glm::length(p - st.in_pos) > 0.25f) {
            reset(st, p, q);
            return;
        }
        const float a = 1.0f / (1.0f + (1.0f / (6.2831853f * pr.speed_hz)) / dt);
        st.vel        = glm::mix(st.vel, (p - st.in_pos) / dt, a);
        st.wvel       = glm::mix(st.wvel, rotvec(st.in_rot, q) / dt, a);
        st.in_pos     = p;
        st.in_rot     = q;
        auto share = [](float v, float lo, float hi) { return hi > lo ? 1.0f - std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f; };
        const float rp = pr.play_m * share(glm::length(st.vel), pr.v_lo, pr.v_hi);
        const float rr = pr.play_rad * share(glm::length(st.wvel), pr.w_lo, pr.w_hi);
        // The play: the gun moves only as far as the hand pushes it.
        const glm::vec3 e = p - st.pos;
        if (const float d = glm::length(e); d > rp) st.pos = p - (d > 1e-9f ? e * (rp / d) : glm::vec3{ 0.0f });
        const glm::vec3 er = rotvec(st.rot, q);
        if (const float d = glm::length(er); d > rr) st.rot = glm::normalize(glm::inverse(from_rotvec(d > 1e-9f ? er * (rr / d) : glm::vec3{ 0.0f })) * q);
        p = st.pos;
        q = st.rot;
    }
}
