// armik math (FarCry5ArmIk.h): the torso move, the two-bone solve of one arm and the per-role moves. No D3D here (tools\fullbodyik\armik_test.cpp).
#include "FarCry5ArmIk.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fc5::armik
{
    glm::mat4 from_rows(const float* f)
    {
        glm::mat4 m{ 1.0f };
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) m[c][r] = f[r * 4 + c];
        return m;
    }

    void to_rows(const glm::mat4& m, float* f)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) f[r * 4 + c] = m[c][r];
    }

    glm::quat arc(const glm::vec3& a_in, const glm::vec3& b_in)
    {
        const float la = glm::length(a_in), lb = glm::length(b_in);
        if (la < 1e-8f || lb < 1e-8f) return glm::quat{ 1, 0, 0, 0 };
        const glm::vec3 a = a_in / la, b = b_in / lb;
        const float     c = glm::dot(a, b);
        if (c < -0.99999f) {
            // Opposite: half a turn about any axis square to a.
            glm::vec3 ax = glm::cross(a, glm::vec3{ 1, 0, 0 });
            if (glm::length(ax) < 1e-3f) ax = glm::cross(a, glm::vec3{ 0, 1, 0 });
            return glm::angleAxis(3.14159265f, glm::normalize(ax));
        }
        const glm::vec3 x = glm::cross(a, b);
        return glm::normalize(glm::quat{ 1.0f + c, x.x, x.y, x.z });
    }

    glm::mat4 rigid(const glm::vec3& to, const glm::mat3& R, const glm::vec3& from, const glm::vec3& axis, float k)
    {
        glm::mat3 S{ 1.0f };
        if (std::abs(k - 1.0f) > 1e-5f && glm::length(axis) > 1e-6f) {
            const glm::vec3 u = glm::normalize(axis);
            S += (k - 1.0f) * glm::outerProduct(u, u);
        }
        const glm::mat3 L = S * R;
        glm::mat4       m{ L };
        m[3] = glm::vec4{ to - L * from, 1.0f };
        return m;
    }

    float along(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p)
    {
        const glm::vec3 ab = b - a;
        const float     l2 = glm::dot(ab, ab);
        return l2 > 1e-10f ? glm::dot(p - a, ab) / l2 : 0.0f;
    }

    namespace
    {
        glm::mat3 rot_of(const glm::mat4& m)   // the rotation of a rigid move (rounding taken out)
        {
            return glm::mat3_cast(glm::normalize(glm::quat_cast(glm::mat3{ m })));
        }
        glm::mat3 frame(const glm::vec3& x_in, const glm::vec3& up_in)   // columns x, y = z cross x, z (up made square to x)
        {
            const glm::vec3 x = glm::normalize(x_in);
            glm::vec3       z = up_in - glm::dot(up_in, x) * x;
            if (glm::length(z) < 1e-4f) z = glm::abs(x.z) < 0.9f ? glm::vec3{ 0, 0, 1 } - x.z * x : glm::vec3{ 0, 1, 0 } - x.y * x;
            z = glm::normalize(z);
            return glm::mat3{ x, glm::cross(z, x), z };
        }
        glm::vec3 point(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }
    }

    glm::mat4 torso_move(const BodyIn& b, bool upright)
    {
        const glm::vec3 m0 = 0.5f * (b.S0[0] + b.S0[1]);
        glm::mat3       R{ 1.0f };
        if (upright && glm::length(b.S0[0] - b.S0[1]) > 1e-3f) {
            const glm::mat3 F0 = frame(b.S0[0] - b.S0[1], b.up0);
            const glm::mat3 Ft = frame(b.mirrored ? -b.right : b.right, b.up);
            R                  = Ft * glm::transpose(F0);
        }
        return rigid(b.mid, R, m0, glm::vec3{ 0.0f }, 1.0f);
    }

    SideOut solve_side(const Opts& o, const GameArm& g, const glm::mat4& Hf, const glm::mat4& Dt, const glm::vec3& out, const glm::vec3& down,
                       const glm::vec3& back, const glm::vec3& last_pole, float dt_s, const float twist[2])
    {
        SideOut r{};
        if (!g.s || !g.e || !g.w || !g.h) return r;
        const float a0 = glm::length(g.E - g.S), b0 = glm::length(g.W - g.E);
        if (a0 < 0.05f || b0 < 0.05f || a0 > 1.0f || b0 > 1.0f) return r;

        // Targets: the shoulder moved with the torso, the wrist where the hand is drawn (its frame's move takes the game's wrist there).
        const glm::mat4 Dh = Hf * glm::inverse(g.H);
        const glm::mat3 Rh = rot_of(Dh);
        const glm::mat3 Rt = rot_of(Dt);
        const glm::vec3 S  = point(Dt, g.S);
        const glm::vec3 W  = point(Dh, g.W);
        const glm::vec3 SW = W - S;
        const float     d  = glm::length(SW);
        if (d < 1e-4f) return r;
        const glm::vec3 u = SW / d;

        // Reach: the arm slows down before straight (soft), and lengthens up to armikstretch to stay on the hand.
        const float L  = a0 + b0;
        const float ds = std::clamp(o.soft, 0.5f, 1.0f) * L;
        float       k  = 1.0f;
        if (d > ds && L - ds > 1e-4f) {
            const float dsoft = ds + (L - ds) * (1.0f - std::exp(-(d - ds) / (L - ds)));
            k                 = d / dsoft;
        }
        k               = std::clamp(k, 1.0f, std::max(1.0f, o.stretch));
        const float a   = a0 * k, b = b0 * k;
        const float dc  = std::clamp(d, std::abs(a - b) + 1e-3f, a + b);   // the reach the triangle can make
        r.reach         = d / L;
        r.stretch       = k;
        r.gap           = std::max(0.0f, d - (a + b));

        // The elbow's direction: down and out from the body, the game's own bend (moved with the torso), and the hand's turn of it.
        const glm::vec3 body_pole = glm::normalize(down + 0.6f * out + 0.3f * back);
        glm::vec3       game_dir{ 0.0f };
        {
            const glm::vec3 u0 = glm::normalize(g.W - g.S);
            const glm::vec3 e  = (g.E - g.S) - glm::dot(g.E - g.S, u0) * u0;
            if (glm::length(e) > 1e-3f) game_dir = glm::normalize(e);
        }
        glm::vec3 raw = o.pole_body * body_pole;
        if (glm::length(game_dir) > 0.5f) raw += o.pole_game * (Rt * game_dir) + o.pole_hand * (Rh * game_dir);
        raw = glm::length(raw) > 1e-4f ? glm::normalize(raw) : body_pole;
        glm::vec3 pole = raw;
        if (glm::length(last_pole) > 0.5f && o.elbow_ms > 0.0f) {
            // Part of the turn from the last direction to this one (a turn, not a mix: opposite directions still meet).
            const float t = 1.0f - std::exp(-std::max(0.0f, dt_s) / (o.elbow_ms / 1000.0f));
            pole          = glm::normalize(glm::slerp(glm::quat{ 1, 0, 0, 0 }, arc(last_pole, raw), t) * glm::normalize(last_pole));
        }
        r.pole      = pole;
        glm::vec3 v = pole - glm::dot(pole, u) * u;
        if (glm::length(v) < 1e-3f) v = body_pole - glm::dot(body_pole, u) * u;
        if (glm::length(v) < 1e-3f) v = glm::cross(u, glm::abs(u.z) < 0.9f ? glm::vec3{ 0, 0, 1 } : glm::vec3{ 1, 0, 0 });
        v = glm::normalize(v);

        const float     ca = std::clamp((a * a + dc * dc - b * b) / (2.0f * a * dc), -1.0f, 1.0f);
        const float     sa = std::sqrt(std::max(0.0f, 1.0f - ca * ca));
        const glm::vec3 E  = S + a * (ca * u + sa * v);
        const glm::vec3 Wc = S + dc * u;   // the forearm's end (W itself unless the arm cannot reach)
        r.S = S;
        r.E = E;
        r.W = Wc;

        // Turns: the upper arm from the torso-moved one (least swing), the forearm from the upper arm's (no hand roll: at the elbow)
        // and from the hand's (with the hand's roll: at the wrist); the twist bones between, the elbow helper half way.
        const glm::quat qt  = glm::quat_cast(Rt);
        const glm::quat qup = glm::normalize(arc(Rt * (g.E - g.S), E - S) * qt);
        const glm::quat qfb = glm::normalize(arc(glm::mat3_cast(qup) * (g.W - g.E), Wc - E) * qup);
        const glm::quat qh  = glm::quat_cast(Rh);
        const glm::quat qf  = glm::normalize(arc(Rh * (g.W - g.E), Wc - E) * qh);
        const glm::vec3 ua  = E - S, fa = Wc - E;

        for (auto& m : r.D) m = glm::mat4{ 1.0f };
        r.D[kClav]   = Dt;
        r.D[kUpper]  = rigid(S, glm::mat3_cast(qup), g.S, ua, k);
        r.D[kUpTwA]  = r.D[kUpper];
        r.D[kUpTwB]  = r.D[kUpper];
        r.D[kElbow]  = rigid(E, glm::mat3_cast(glm::slerp(qup, qfb, 0.5f)), g.E, fa, 1.0f);
        r.D[kFore]   = rigid(E, glm::mat3_cast(qfb), g.E, fa, k);
        r.D[kFoTwA]  = rigid(E, glm::mat3_cast(glm::slerp(qfb, qf, std::clamp(twist[0], 0.0f, 1.0f))), g.E, fa, k);
        r.D[kFoTwB]  = rigid(E, glm::mat3_cast(glm::slerp(qfb, qf, std::clamp(twist[1], 0.0f, 1.0f))), g.E, fa, k);
        r.D[kHand]   = Dh;
        r.ok         = true;
        return r;
    }

    void move_slot(const float* p_in, float* p_out, const glm::mat4& M, const glm::mat4& Minv, const glm::mat4& D)
    {
        to_rows(Minv * D * M * from_rows(p_in), p_out);
    }
}
