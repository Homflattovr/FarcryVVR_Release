// bodyik math (FarCry5BodyIk.h): each palette slot's body part from its vertices' rest-pose centre, the body under the head, the arm's
// two-bone solve, and the palette of one draw. No D3D here (tools\fullbodyik\bench builds it on Linux and runs it on the game's meshes).
#include "FarCry5BodyIk.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace fc5::bodyik
{
    namespace
    {
        // The rest skeleton every character model of the game is skinned on (tools\fullbodyik\fbscan.py: 667 models, the same joints
        // within a millimetre for Aver_M, Aver_F, Aver_MF, Fat and Slim). The right side; the left is its mirror in x.
        const glm::vec3 kRest[kJoints] = {
            { 0.0f, -0.0187f, 0.9332f },      // Hips
            { 0.0f, -0.0158f, 1.5088f },      // Neck
            { 0.0f, 0.0037f, 1.7891f },       // HeadEnd
            { 0.0f, 0.0311f, 1.6550f },       // Camera
            { 0.0600f, -0.0322f, 1.4482f },   // RightShoulder (the clavicle's root)
            { 0.1708f, -0.0532f, 1.4466f },   // RightArm (the shoulder joint)
            { 0.4336f, -0.0528f, 1.2469f },   // RightForeArm (the elbow)
            { 0.5934f, 0.1001f, 1.1262f },    // RightHand (the wrist)
            { 0.6391f, 0.1564f, 1.0791f },    // RightHandMiddle1 (the knuckles)
            { 0.1055f, -0.0002f, 0.9342f },   // RightUpLeg
            { 0.1421f, 0.0942f, 0.5111f },    // RightLeg (the knee)
            { 0.1777f, 0.0012f, 0.0993f },    // RightFoot (the ankle)
            { 0.1893f, 0.1621f, 0.0367f },    // RightToe
        };
        constexpr float kHandLen   = 0.19f;   // wrist to fingertips along the knuckles' direction
        constexpr float kZone      = 0.04f;   // m each side of the elbow where the two bones' turns are mixed
        constexpr float kZoneSh    = 0.08f;   // the same at the shoulder (the deltoid is wide)
        constexpr float kMinWeight = 1e-4f;   // every slot a vertex uses gets a part (a slot left as the game posed it tears its vertices away)
        constexpr float kPi        = 3.14159265f;
        constexpr float kSeatedFollow = 20.0f;   // seated: the hips' slow drift towards under the neck (s), for a player who moves on the seat
        constexpr float kSeatedLeash  = 0.35f;   //   and how far the neck leans from them at most (m)

        glm::vec3 pt(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }
        bool      finite3(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        glm::mat3 frame(const glm::vec3& x_in, const glm::vec3& y_in)   // columns x, y made square to x, x cross y
        {
            const glm::vec3 x = glm::normalize(x_in);
            glm::vec3       y = y_in - glm::dot(y_in, x) * x;
            if (glm::length(y) < 1e-5f) y = glm::cross(std::abs(x.z) < 0.9f ? glm::vec3{ 0, 0, 1 } : glm::vec3{ 1, 0, 0 }, x);
            y = glm::normalize(y);
            return glm::mat3{ x, y, glm::cross(x, y) };
        }
        // pivot_rest -> pivot_world, turned by q, lengthened k times along axis_rest.
        glm::mat4 rigid(const glm::vec3& pivot_world, const glm::quat& q, const glm::vec3& pivot_rest, const glm::vec3& axis_rest, float k)
        {
            glm::mat3 L = glm::mat3_cast(q);
            if (std::abs(k - 1.0f) > 1e-5f) L = L * (glm::mat3{ 1.0f } + (k - 1.0f) * glm::outerProduct(axis_rest, axis_rest));
            glm::mat4 m{ L };
            m[3] = glm::vec4{ pivot_world - L * pivot_rest, 1.0f };
            return m;
        }
        // The angle of q about axis (q is a turn about that axis), in (-pi, pi].
        float twist_angle(const glm::quat& q, const glm::vec3& axis)
        {
            float a = 2.0f * std::atan2(glm::dot(glm::vec3{ q.x, q.y, q.z }, axis), q.w);
            while (a > kPi) a -= 2.0f * kPi;
            while (a <= -kPi) a += 2.0f * kPi;
            return a;
        }
        std::string fmt_why(int good, int low, int uneven, int off_centre, int untracked)
        {
            std::string w = "not enough T-pose (" + std::to_string(good) + " good samples, 30 needed)";
            if (low >= uneven && low >= off_centre && low >= untracked && low > 0) w += ": the hands were less than 1 m apart; hold both arms straight out to the sides";
            else if (uneven >= off_centre && uneven >= untracked && uneven > 0) w += ": one hand was higher than the other; both at shoulder height";
            else if (off_centre >= untracked && off_centre > 0) w += ": the head was not between the hands; stand or sit straight, looking ahead";
            else if (untracked > 0) w += ": the controllers or the headset were not tracked";
            return w;
        }
        float seg_fit(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float& t)
        {
            const glm::vec3 ab = b - a;
            t                  = glm::dot(p - a, ab) / glm::dot(ab, ab);
            return glm::length(p - (a + std::clamp(t, 0.0f, 1.0f) * ab));
        }
    }

    const char* region_name(int r)
    {
        static const char* const k[kRegions]{ "none", "torso", "neck", "clavicle", "upper arm", "forearm", "hand", "thigh", "shin", "foot" };
        return r >= 0 && r < kRegions ? k[r] : "?";
    }
    const char* result_name(int r)
    {
        static const char* const k[]{ "placed", "no head pose", "no hand for its arm", "not the legs", "legs off", "a head piece (stays hidden)", "nothing to place", "no data",
                                      "the game's legs upside down (stay hidden)" };
        return r >= 0 && r <= kLegsInverted ? k[r] : "?";
    }

    glm::vec3 rest_joint(int joint, int side)
    {
        glm::vec3 v = kRest[std::clamp(joint, 0, int(kJoints) - 1)];
        if (side == 1) v.x = -v.x;
        return v;
    }
    float torso_length() { return glm::length(kRest[kJNeck] - kRest[kJHips]); }

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
        if (c < -0.99999f) {   // opposite: half a turn about any axis square to a
            glm::vec3 ax = glm::cross(a, glm::vec3{ 1, 0, 0 });
            if (glm::length(ax) < 1e-3f) ax = glm::cross(a, glm::vec3{ 0, 1, 0 });
            return glm::angleAxis(kPi, glm::normalize(ax));
        }
        const glm::vec3 x = glm::cross(a, b);
        return glm::normalize(glm::quat{ 1.0f + c, x.x, x.y, x.z });
    }
    glm::quat rot_of(const glm::mat4& m)
    {
        glm::vec3 x{ m[0] }, y{ m[1] }, z{ m[2] };
        if (glm::length(x) < 1e-8f || glm::length(y) < 1e-8f) return glm::quat{ 1, 0, 0, 0 };
        x = glm::normalize(x);
        y = y - glm::dot(y, x) * x;
        if (glm::length(y) < 1e-8f) return glm::quat{ 1, 0, 0, 0 };
        y = glm::normalize(y);
        glm::vec3 zc = glm::cross(x, y);
        if (glm::dot(zc, z) < 0.0f) zc = -zc;   // a mirror: not a turn; the nearest frame is kept (callers treat mirrors themselves)
        return glm::normalize(glm::quat_cast(glm::mat3{ x, y, glm::cross(x, y) }));
    }

    // ---------------------------------------------------------------------------------------------------------------- roles

    namespace
    {
        Role classify_one(float w, const glm::vec3& c)
        {
            static const glm::vec3 tip = kRest[kJWrist] + glm::normalize(kRest[kJKnuckle] - kRest[kJWrist]) * kHandLen;
            struct Seg { uint8_t region; glm::vec3 a, b; float rad; bool sided; };
            static const Seg segs[] = {
                { kClav, kRest[kJClav], kRest[kJShoulder], 0.08f, true }, { kUpper, kRest[kJShoulder], kRest[kJElbow], 0.07f, true },
                { kFore, kRest[kJElbow], kRest[kJWrist], 0.06f, true },   { kHand, kRest[kJWrist], tip, 0.06f, true },
                { kUpLeg, kRest[kJHip], kRest[kJKnee], 0.10f, true },     { kLeg, kRest[kJKnee], kRest[kJAnkle], 0.08f, true },
                { kFoot, kRest[kJAnkle], kRest[kJToe], 0.08f, true },     { kTorso, kRest[kJHips], kRest[kJNeck], 0.20f, false },
                { kNeck, kRest[kJNeck], kRest[kJHeadEnd], 0.12f, false },
            };
            Role best{};
            if (!(w > kMinWeight) || !finite3(c)) return best;
            const glm::vec3 m{ std::abs(c.x), c.y, c.z };
            best.fit = 1e9f;
            for (const Seg& g : segs) {
                float       t   = 0.0f;
                const float fit = seg_fit(g.sided ? m : c, g.a, g.b, t) / g.rad;
                if (fit < best.fit) {
                    best.fit    = fit;
                    best.region = g.region;
                    best.t      = t;
                    best.side   = g.sided ? (c.x >= 0.0f ? 0 : 1) : 0;
                }
            }
            best.w = w;
            best.t = std::clamp(best.t, -1.0f, 2.0f);
            return best;
        }
        void tally(Roles& r)
        {
            std::memset(r.weight, 0, sizeof(r.weight));
            r.total = 0.0f;
            for (int s = 0; s < r.n; ++s) {
                const Role& ro = r.slot[s];
                if (ro.region == kNone) continue;
                r.weight[ro.side][ro.region] += ro.w;
                r.total += ro.w;
            }
        }
    }

    Roles classify(const glm::vec4* cent, int n)
    {
        Roles r{};
        r.n = std::clamp(n, 0, 256);
        if (!cent) return r;
        for (int s = 0; s < r.n; ++s) r.slot[s] = classify_one(cent[s].x, glm::vec3{ cent[s].y, cent[s].z, cent[s].w });
        tally(r);
        return r;
    }

    namespace
    {
        bool close(const glm::mat4& a, const glm::mat4& b)
        {
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r)
                    if (std::abs(a[c][r] - b[c][r]) > 3e-3f) return false;
            return std::abs(a[3][0] - b[3][0]) < 2.5e-3f && std::abs(a[3][1] - b[3][1]) < 2.5e-3f && std::abs(a[3][2] - b[3][2]) < 2.5e-3f;
        }
        // One draw's arm slots matched with the arm bones known (Bones in FarCry5BodyIk.h); a matched slot's place becomes its bone's.
        int share_bones(Bones& B, int frame, uint64_t mesh, int n, const glm::vec4* cent, const float* game, const glm::mat4& M, Roles& R)
        {
            if (B.e.size() > 4000) {   // outfits and areas come and go
                B.e.clear();
                B.of.clear();
            }
            auto& of = B.of[mesh];
            if (of.size() != 256) of.assign(256, -1);
            int  shared = 0;
            auto centre = [](const Bones::Entry& e, int frame_now, float& w) {
                glm::vec3 c{ 0.0f };
                w = 0.0f;
                for (const auto& m : e.m)
                    if (frame_now - m.seen <= 300) {
                        c += m.w * m.c;
                        w += m.w;
                    }
                return w > 0.0f ? c / w : c;
            };
            auto is_arm = [](int region) { return region == kUpper || region == kFore || region == kHand; };
            // 1. Each arm slot's bone from the frames before: kept while its matrix still matches.
            int       left[256];
            glm::mat4 Wm[256];
            for (int s = 0; s < n; ++s) {
                left[s] = -1;
                if (!is_arm(R.slot[s].region)) continue;
                Wm[s]   = M * from_rows(game + size_t(s) * 12);
                int idx = of[s];
                if (idx >= 0 && idx < int(B.e.size())) {
                    auto& e  = B.e[idx];
                    auto  it = std::find_if(e.m.begin(), e.m.end(), [&](const Bones::Member& m) { return m.mesh == mesh && m.slot == s; });
                    if (it == e.m.end()) idx = -1;
                    else if (e.now_frame == frame && !close(e.now, Wm[s])) {   // not that bone after all
                        e.m.erase(it);
                        B.leaves++;
                        left[s] = idx;
                        idx     = -1;
                    } else left[s] = it->left;
                } else idx = -1;
                of[s] = idx;
            }
            // 2. Slots with no bone, or alone in theirs: the bones drawn this frame with the same matrix, on the same side, holding no slot
            //    of this mesh. The nearest pairs first, one slot of this mesh per bone.
            struct Pair { float d; int s, k; };
            std::vector<Pair> pairs;
            for (int s = 0; s < n; ++s) {
                if (!is_arm(R.slot[s].region)) continue;
                const int idx = of[s];
                if (idx >= 0 && B.e[idx].m.size() != 1) continue;
                const glm::vec3 c{ cent[s].y, cent[s].z, cent[s].w };
                for (int k = 0; k < int(B.e.size()); ++k) {
                    const auto& e = B.e[k];
                    if (k == idx || k == left[s] || e.now_frame != frame || e.m.empty() || !close(e.now, Wm[s])) continue;
                    if (std::any_of(e.m.begin(), e.m.end(), [&](const Bones::Member& m) { return m.mesh == mesh; })) continue;
                    float           w  = 0.0f;
                    const glm::vec3 ec = centre(e, frame, w);
                    if ((ec.x >= 0.0f) != (c.x >= 0.0f)) continue;
                    const float d = glm::length(ec - c);
                    if (d < 0.15f) pairs.push_back({ d, s, k });
                }
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
            bool taken_s[256]{};
            for (const Pair& p : pairs) {
                if (taken_s[p.s]) continue;
                auto& e = B.e[p.k];
                if (std::any_of(e.m.begin(), e.m.end(), [&](const Bones::Member& m) { return m.mesh == mesh; })) continue;
                if (of[p.s] >= 0) B.e[of[p.s]].m.clear();
                e.m.push_back({ mesh, p.s, cent[p.s].x, glm::vec3{ cent[p.s].y, cent[p.s].z, cent[p.s].w }, frame, left[p.s] });
                of[p.s]      = p.k;
                taken_s[p.s] = true;
                B.joins++;
            }
            // 3. The rest start their own bone; every slot's place becomes its bone's (all its meshes' vertices together).
            for (int s = 0; s < n; ++s) {
                if (!is_arm(R.slot[s].region)) continue;
                const glm::vec3 c{ cent[s].y, cent[s].z, cent[s].w };
                if (of[s] < 0) {
                    B.e.emplace_back();
                    B.e.back().m.push_back({ mesh, s, cent[s].x, c, frame, left[s] });
                    of[s] = int(B.e.size()) - 1;
                }
                auto& e = B.e[of[s]];
                for (auto& m : e.m)
                    if (m.mesh == mesh && m.slot == s) {
                        m.seen = frame;
                        m.w    = cent[s].x;
                        m.c    = c;
                    }
                if (e.now_frame != frame) {
                    e.now       = Wm[s];
                    e.now_frame = frame;
                }
                if (e.m.size() > 1) {
                    float           w  = 0.0f;
                    const glm::vec3 cc = centre(e, frame, w);
                    Role            ro = classify_one(w, cc);
                    if (is_arm(ro.region) && ro.side == R.slot[s].side) {
                        ro.w      = R.slot[s].w;
                        R.slot[s] = ro;
                        shared++;
                    }
                }
            }
            tally(R);
            return shared;
        }
    }

    // ----------------------------------------------------------------------------------------------------------------- body

    BodyPose body_pose(const Opts& o, const HeadIn& in, Follow& f)
    {
        BodyPose        b{};
        const glm::vec3 up_r{ 0.0f, 1.0f, 0.0f };
        glm::vec3       fw = in.fwd - glm::dot(in.fwd, up_r) * up_r;
        const glm::vec3 eye{ in.head[3] };
        if (glm::length(fw) < 1e-4f || !finite3(eye) || !finite3(in.pivot) || !finite3(in.origin)) return b;
        fw                   = glm::normalize(fw);
        const glm::vec3 back = -fw;
        // bodyikbody: the shoulders as far under the eyes as the player's own (the whole body moves down or up by the difference).
        const float nd = o.neck_down + cal_shoulder_dy(o, in.scale);
        // The model's neck joint: from the pivot (it stays put when the head turns), in the body's frame; with the head level and along
        // the heading it is neck_down below and neck_back + back behind the eyes.
        const glm::vec3 neck_r = in.pivot - up_r * (nd - in.pivot_down) + back * (o.neck_back + o.back - in.pivot_back);
        // The hips stand under the neck: they follow it with a short lag and never further than the leash, so a quick lean tilts the torso
        // and a step brings the hips along. Seated: they stay in the chair (a long leash, a slow drift for a player who moves on the seat).
        const glm::vec3 target{ neck_r.x, 0.0f, neck_r.z };
        const float     follow_s = o.seated ? kSeatedFollow : o.follow_s;
        if (!f.init) {
            f.init = true;
            f.hips = target;
        } else {
            // Seated, a jump of the place under the neck (a recentre moves the reference space; nobody leans 15 cm in a frame) takes the
            // hips along, so they do not lean the torso until the slow drift brings them back.
            if (o.seated && glm::length(target - f.last) > 0.15f) f.hips += target - f.last;
            const float a = follow_s > 1e-3f ? 1.0f - std::exp(-std::max(0.0f, in.dt) / follow_s) : 1.0f;
            f.hips += (target - f.hips) * a;
            const glm::vec3 d     = target - f.hips;
            const float     l     = glm::length(d);
            const float     leash = o.seated ? kSeatedLeash : std::clamp(o.leash, 0.0f, 0.3f);
            if (l > leash && l > 1e-6f) f.hips = target - d * (leash / l);
        }
        f.last = target;
        b.lag      = f.hips - target;
        auto world = [&](const glm::vec3& p) { return in.origin + in.to_world * (p * in.scale); };
        b.up       = glm::normalize(in.to_world * up_r);
        b.fwd      = glm::normalize(in.to_world * fw);
        b.right    = glm::normalize(glm::cross(b.fwd, b.up));
        b.fwd      = glm::cross(b.up, b.right);
        b.yaw      = glm::mat3{ b.right, b.fwd, b.up };
        b.neck     = world(neck_r);
        b.eye      = world(eye);
        b.arm_k    = cal_arm_k(o, in.scale);
        glm::vec3   off  = in.to_world * ((f.hips - target) * in.scale);   // the hips from under the neck (level)
        const float L    = torso_length();
        const float omax = (o.seated ? 0.7f : 0.5f) * L;
        float       ol   = glm::length(off);
        if (ol > omax) {
            off *= omax / ol;
            ol = omax;
        }
        // The bow: a neck bends about 40 deg; looking further down the back bends too. Without it the head, turning on about the neck's
        // pivot, ends over the collar and looks into it (the model has no head or neck to hide it). The torso leans by the pitch beyond
        // that: the hips go back, the neck stays under the head, and the chest is seen as at 40 deg at any pitch.
        const glm::vec3 look = glm::mat3{ in.head } * glm::vec3{ 0.0f, 0.0f, -1.0f };
        const float     ll   = glm::length(look);
        float           bow  = 0.0f;
        if (ll > 1e-4f && o.bow_gain > 0.0f) {
            const float down = std::asin(std::clamp(-look.y / ll, -1.0f, 1.0f));
            bow              = std::clamp((down - o.bow_from) * o.bow_gain, 0.0f, 0.9f);
        }
        if (bow > 0.0f && !o.hunch) {   // bodyikhunch off: the whole torso swings back about the neck (the hips go back), as before 14 Nov
            off -= b.fwd * (L * std::sin(bow));
            ol = glm::length(off);
            if (ol > 0.85f * L) {
                off *= 0.85f * L / ol;
                ol = 0.85f * L;
            }
        }
        b.hips = b.neck + off - b.up * std::sqrt(std::max(0.0f, L * L - ol * ol));
        b.drop = glm::dot(world(glm::vec3{ 0.0f, -(nd + L), 0.0f }) - b.hips, b.up);
        if (o.seated) {
            // Seated: the hips on the chair at their standing place's height (the head going down is a lean, never a crouch), the torso from
            // them towards the head's neck, its own length (the neck joint may sit a little off the head's pivot, which nothing shows).
            b.hips += b.up * b.drop;
            b.drop = 0.0f;
            glm::vec3 dn = b.neck - b.hips;
            if (glm::dot(dn, b.up) < 0.2f * L) dn += b.up * (0.2f * L - glm::dot(dn, b.up));   // never folded below the hips' level
            b.neck = b.hips + glm::normalize(dn) * L;
        }
        b.lean = arc(b.yaw * (kRest[kJNeck] - kRest[kJHips]), b.neck - b.hips);
        // bodyikhunch: the bow as a curled back. The upper part (from hunch_at of the torso to the neck) bows forward by the bow, the lower part
        // leans back so the neck stays over the hips; the curled spine is shorter than the straight one, so the neck sits that much lower under
        // the head's pivot (4 cm looking 75 deg down; nothing shows it: no head or neck is drawn). The hips, and the legs under them, stay.
        if (bow > 0.0f && o.hunch && o.hunch_gain > 0.0f) {
            const float     at = std::clamp(o.hunch_at, 0.3f, 0.85f);
            const float     a = at * L, c = (1.0f - at) * L;
            const glm::vec3 ax = b.neck - b.hips;
            const float     up = bow * std::clamp(o.hunch_gain, 0.0f, 1.0f);
            b.hunch_up  = up;
            b.hunch_low = std::asin(std::clamp(c / a * std::sin(up), 0.0f, 1.0f));
            b.hunch_at  = at;
            if (glm::length(ax) > 1e-4f) b.neck = b.hips + glm::normalize(ax) * (a * std::cos(b.hunch_low) + c * std::cos(b.hunch_up));
        }
        // The hips never stand above their standing place. The torso is rigid, so a bow (or a lean) under a head that did not come down
        // lifts them, and the feet with them (12 cm off the floor looking 85 deg down in the 11 Nov fix 3 run). The whole body goes down by
        // that much instead: its neck joint then sits below the head's pivot, which nothing shows (no head or neck is drawn).
        if (b.drop < 0.0f) {
            const glm::vec3 d = b.up * b.drop;
            b.neck += d;
            b.hips += d;
            b.drop = 0.0f;
        }
        b.ok   = finite3(b.neck) && finite3(b.hips);
        return b;
    }

    glm::mat4 torso_frame(const BodyPose& b, float t)
    {
        // The hips upright; from the first spine bone up the lean comes in, whole from a third of the way.
        const float s = std::clamp((t - 0.05f) / 0.30f, 0.0f, 1.0f);
        if (b.hunch_up <= 0.0f) {
            const glm::mat3 R = glm::mat3_cast(glm::slerp(glm::quat{ 1, 0, 0, 0 }, b.lean, s)) * b.yaw;
            glm::mat4       m{ R };
            m[3] = glm::vec4{ b.hips - R * kRest[kJHips], 1.0f };
            return m;
        }
        // bodyikhunch: below the bend the lower torso's turn (back by hunch_low, then the lean) about the hips, eased in from the first spine bone
        // as the lean is; above the bend the upper torso's (forward by hunch_up, then the lean) about the bend; over 15% of the torso either side
        // of it the turn eases from one to the other about the bend (the bend's point maps to the same place from either side).
        const glm::quat ql = b.lean * glm::angleAxis(b.hunch_low, b.right);
        const glm::quat qu = b.lean * glm::angleAxis(-b.hunch_up, b.right);
        const glm::vec3 rH = kRest[kJHips], rJ = kRest[kJHips] + b.hunch_at * (kRest[kJNeck] - kRest[kJHips]);
        float           w  = std::clamp((t - (b.hunch_at - 0.15f)) / 0.30f, 0.0f, 1.0f);
        w                  = w * w * (3.0f - 2.0f * w);
        glm::mat4 m{ 1.0f };
        if (w <= 0.0f) {
            const glm::mat3 R = glm::mat3_cast(glm::slerp(glm::quat{ 1, 0, 0, 0 }, ql, s)) * b.yaw;
            m    = glm::mat4{ R };
            m[3] = glm::vec4{ b.hips - R * rH, 1.0f };
        } else {
            const glm::vec3 J = b.hips + glm::mat3_cast(ql) * (b.yaw * (rJ - rH));
            const glm::mat3 R = glm::mat3_cast(glm::slerp(ql, qu, w)) * b.yaw;
            m    = glm::mat4{ R };
            m[3] = glm::vec4{ J - R * rJ, 1.0f };
        }
        return m;
    }

    // ------------------------------------------------------------------------------------------------------------ worn things

    namespace
    {
        // bodyikworn's places on the rest skeleton (model metres: x right, y forward, z up), read off the game's outfits in their rest pose
        // (tools\fullbodyik\samples: shirts, jackets, a hoodie, cargo pants): the hips' sides are 19-24 cm out at the belt (z 0.90-0.97), the
        // belly's front 15-19 cm forward at z 0.97-1.05, the chest's front 10-13 cm forward 11 cm left at z 1.30.
        //  - the slung weapon: its grip on the front of the belly, right of the middle (the gun across the body as slingrot turns it);
        //  - the sidearm: its grip at the belt on the right hip's outside (the gun down the thigh as sidearmrot turns it);
        //  - the throwables' pouch (11 x 9 x 5 cm): its middle on the left chest, its back on the thickest jacket's front;
        //  - the magazine: at the belt, front left, standing off the thickest jacket.
        const glm::vec3 kWornRest[kWornParts]{ { 0.10f, 0.23f, 1.08f }, { 0.25f, 0.03f, 0.97f }, { -0.11f, 0.165f, 1.27f }, { -0.135f, 0.205f, 0.99f } };
    }
    const char* worn_name(int part)
    {
        static const char* const n[kWornParts]{ "sling", "holster", "pouch", "mag" };
        return part >= 0 && part < kWornParts ? n[part] : "?";
    }
    glm::vec3 worn_rest(int part) { return kWornRest[std::clamp(part, 0, int(kWornParts) - 1)]; }

    glm::mat4 worn_place(const BodyPose& b, const Opts& o, int part, bool mirror)
    {
        part        = std::clamp(part, 0, int(kWornParts) - 1);
        glm::vec3 p = kWornRest[part] + o.worn_adj[part];
        if (mirror) p.x = -p.x;
        // The torso carries a point at its height as the mesh there is carried (torso_frame's t: 0 the hips joint, 1 the neck joint).
        const float     t  = std::clamp((p.z - kRest[kJHips].z) / std::max(1e-3f, kRest[kJNeck].z - kRest[kJHips].z), 0.0f, 1.0f);
        const glm::mat4 T  = torso_frame(b, t);
        const glm::vec3 pw = glm::vec3{ T * glm::vec4{ p, 1.0f } };
        // The gestures' axes (x right, y up, z back) in the model's (x right, y forward, z up), then into the world: the pouch with the chest,
        // the others on the body's upright heading.
        const glm::mat3 K{ glm::vec3{ 1, 0, 0 }, glm::vec3{ 0, 0, 1 }, glm::vec3{ 0, -1, 0 } };
        const glm::mat3 R = (part == kWornPouch ? glm::mat3{ T } : b.yaw) * K;
        glm::mat4       m{ R };
        m[3] = glm::vec4{ pw, 1.0f };
        return m;
    }

    glm::mat4 legs_frame(const BodyPose& b, float floor_z)
    {
        glm::mat4 m{ b.yaw };
        glm::vec3 t = b.hips - b.yaw * kRest[kJHips];
        t += b.up * (floor_z - glm::dot(t, b.up));   // on the game's floor
        m[3] = glm::vec4{ t, 1.0f };
        return m;
    }

    LegSolve solve_leg(const glm::vec3& H, const glm::vec3& K, const glm::vec3& A, const glm::vec3& hip_target)
    {
        LegSolve l{};
        l.H = H; l.K = K; l.A = A; l.Ht = hip_target; l.Kn = K; l.An = A;
        const float a = glm::length(K - H), b = glm::length(A - K);
        if (!(a > 0.25f && a < 0.65f && b > 0.25f && b < 0.65f) || !finite3(hip_target)) return l;   // not a leg as the skeleton has it
        glm::vec3   HA = A - hip_target;
        float       d  = glm::length(HA);
        const float dmax = 0.999f * (a + b), dmin = std::abs(a - b) + 0.05f;
        if (d < 1e-4f) return l;
        const glm::vec3 u = HA / d;
        if (d > dmax) {   // the hips too high for the leg: the foot comes up
            l.lift = d - dmax;
            d      = dmax;
        }
        d    = std::max(d, dmin);
        l.An = hip_target + u * d;
        // The knee keeps the side the game bent it to (forward if the game's leg is straight).
        const glm::vec3 u0 = glm::normalize(A - H);
        glm::vec3       e  = (K - H) - glm::dot(K - H, u0) * u0;
        if (glm::length(e) < 0.01f) e = glm::vec3{ 0.0f, 1.0f, 0.0f };
        glm::vec3 v = e - glm::dot(e, u) * u;
        if (glm::length(v) < 1e-4f) v = glm::vec3{ 0.0f, 1.0f, 0.0f } - u.y * u;
        v = glm::normalize(v);
        const float ca = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
        l.Kn    = hip_target + a * (ca * u + std::sqrt(std::max(0.0f, 1.0f - ca * ca)) * v);
        l.q_up  = arc(K - H, l.Kn - hip_target);
        l.q_low = arc(A - K, l.An - l.Kn);
        l.ok    = finite3(l.Kn);
        return l;
    }

    glm::mat4 leg_move(const LegSolve& l, int region, float t)
    {
        const float     a = glm::length(l.K - l.H), b = glm::length(l.A - l.K);
        const glm::vec3 none{ 1.0f, 0.0f, 0.0f };
        auto knee_zone = [&](float m_after) {
            const float f = 0.5f + 0.5f * std::clamp(m_after / 0.05f, -1.0f, 1.0f);
            return rigid(l.Kn, glm::slerp(l.q_up, l.q_low, f), l.K, none, 1.0f);
        };
        switch (region) {
        case kUpLeg: return (1.0f - t) * a < 0.05f ? knee_zone(-(1.0f - t) * a) : rigid(l.Ht, l.q_up, l.H, none, 1.0f);
        case kLeg: return t * b < 0.05f ? knee_zone(t * b) : rigid(l.Kn, l.q_low, l.K, none, 1.0f);
        default: {
            glm::mat4 m{ 1.0f };
            m[3] = glm::vec4{ l.An - l.A, 1.0f };
            return m;
        }
        }
    }

    // ------------------------------------------------------------------------------------------------------------------ arm

    ArmSolve solve_arm(const Opts& o, const BodyPose& b, int side, const glm::mat4& hand, const glm::vec3& last_pole, float last_twist, float dt)
    {
        ArmSolve r{};
        r.side = side;
        r.hand = hand;
        if (!b.ok) return r;
        const glm::vec3 Sr = rest_joint(kJShoulder, side), Er = rest_joint(kJElbow, side), Wr = rest_joint(kJWrist, side);
        const float     a0 = glm::length(Er - Sr), b0 = glm::length(Wr - Er);
        const glm::vec3 ar = (Er - Sr) / a0, fr = (Wr - Er) / b0;
        const glm::vec3 hr = glm::normalize(glm::cross(ar, fr));   // the elbow's hinge, rest
        const glm::mat4 Bc = torso_frame(b, 1.0f);
        const glm::quat qc = rot_of(Bc);
        const glm::vec3 S0 = pt(Bc, Sr);
        const glm::vec3 Wt = pt(hand, Wr);
        const glm::quat qh = rot_of(hand);
        if (!finite3(Wt)) return r;

        // bodyikbody: the arm as long as the player's own (both bones by the same share); 1 without a calibration.
        const float     kb = std::clamp(b.arm_k, 0.75f, 1.3f);
        const float     L = (a0 + b0) * kb;
        const glm::vec3 S = S0;   // the shoulders stay square on the chest (every chest and clavicle slot gets the one chest matrix)
        const glm::vec3 SW = Wt - S;
        const float     d  = glm::length(SW);
        if (d < 1e-4f) return r;
        const glm::vec3 u = SW / d;

        // Reach: the arm slows down before straight (soft), and lengthens up to the stretch limit to stay on the hand.
        const float ds = std::clamp(o.soft, 0.5f, 1.0f) * L;
        float       k  = 1.0f;
        if (d > ds && L - ds > 1e-4f) k = d / (ds + (L - ds) * (1.0f - std::exp(-(d - ds) / (L - ds))));
        k              = kb * std::clamp(k, 1.0f, std::max(1.0f, o.stretch));
        const float a  = a0 * k, bb = b0 * k;
        const float dc = std::clamp(d, std::abs(a - bb) + 1e-3f, a + bb);
        r.reach   = d / (a0 + b0);
        r.k       = k;
        r.gap     = std::max(0.0f, d - (a + bb));

        // The elbow's direction: down, out and a little back from the body, turned by the hand's own turn of the rest elbow.
        const glm::vec3 out       = (side == 0 ? 1.0f : -1.0f) * b.right;
        const glm::vec3 body_pole = glm::normalize(-b.up + 0.6f * out - 0.35f * b.fwd);
        const glm::vec3 ur        = glm::normalize(Wr - Sr);
        const glm::vec3 vr        = glm::normalize((Er - Sr) - glm::dot(Er - Sr, ur) * ur);
        const float     ph        = std::clamp(o.pole_hand, 0.0f, 1.0f);
        glm::vec3       raw       = (1.0f - ph) * body_pole + ph * (qh * vr);
        raw                       = glm::length(raw) > 1e-3f ? glm::normalize(raw) : body_pole;
        glm::vec3 pole            = raw;
        if (glm::length(last_pole) > 0.5f && o.elbow_ms > 0.0f) {
            const float t = 1.0f - std::exp(-std::max(0.0f, dt) / (o.elbow_ms / 1000.0f));
            pole          = glm::normalize(glm::slerp(glm::quat{ 1, 0, 0, 0 }, arc(last_pole, raw), t) * glm::normalize(last_pole));
        }
        r.pole      = pole;
        glm::vec3 v = pole - glm::dot(pole, u) * u;
        if (glm::length(v) < 1e-3f) v = body_pole - glm::dot(body_pole, u) * u;
        if (glm::length(v) < 1e-3f) v = glm::cross(u, std::abs(u.z) < 0.9f ? glm::vec3{ 0, 0, 1 } : glm::vec3{ 1, 0, 0 });
        v = glm::normalize(v);

        const float     ca = std::clamp((a * a + dc * dc - bb * bb) / (2.0f * a * dc), -1.0f, 1.0f);
        const float     sa = std::sqrt(std::max(0.0f, 1.0f - ca * ca));
        const glm::vec3 E  = S + a * (ca * u + sa * v);
        const glm::vec3 Wc = S + dc * u;
        const glm::vec3 ap = glm::normalize(E - S), fp = glm::normalize(Wc - E);
        const glm::vec3 hp = glm::normalize(glm::cross(v, u));   // the hinge now (the same sense as hr: both are the elbow's side crossed with the reach)
        r.S    = S;
        r.E    = E;
        r.W    = Wc;
        r.bend = std::acos(std::clamp(glm::dot(ap, fp), -1.0f, 1.0f));

        // Turns (rest -> world). At the elbow both bones carry the hinge; the shoulder end of the upper arm swings the least from the
        // chest, the wrist end of the forearm has the hand's roll; between, the twist is spread along the bone (arm_frame).
        r.q_chest = qc;
        r.q_u    = glm::normalize(glm::quat_cast(frame(ap, hp) * glm::transpose(frame(ar, hr))));
        r.q_fe   = glm::normalize(glm::quat_cast(frame(fp, hp) * glm::transpose(frame(fr, hr))));
        r.q_us   = glm::normalize(arc(qc * ar, ap) * qc);
        r.tw_u   = twist_angle(r.q_u * glm::conjugate(r.q_us), ap);
        const glm::quat q_fw = glm::normalize(arc(qh * fr, fp) * qh);
        float           tw   = twist_angle(q_fw * glm::conjugate(r.q_fe), fp);
        if (glm::length(last_pole) > 0.5f) {   // continuity: the nearest turn to the last frame's, unless that winds the forearm up
            while (tw - last_twist > kPi) tw -= 2.0f * kPi;
            while (tw - last_twist < -kPi) tw += 2.0f * kPi;
            if (std::abs(tw) > 1.25f * kPi) tw -= std::copysign(2.0f * kPi, tw);
        }
        r.tw_f = std::clamp(tw, -2.6f, 2.6f);
        r.ok   = finite3(E) && finite3(Wc);
        return r;
    }

    glm::mat4 arm_frame(const ArmSolve& a, int region, float t)
    {
        const int       side = a.side;
        const glm::vec3 Sr = rest_joint(kJShoulder, side), Er = rest_joint(kJElbow, side), Wr = rest_joint(kJWrist, side);
        const float     a0 = glm::length(Er - Sr), b0 = glm::length(Wr - Er);
        const glm::vec3 ar = (Er - Sr) / a0, fr = (Wr - Er) / b0;
        const glm::vec3 ap = glm::normalize(a.E - a.S), fp = glm::normalize(a.W - a.E);
        // The upper arm's twist along it: none at the shoulder (it only swings there), whole from three quarters of the way.
        auto up_at = [&](float tt) {
            const float tau = std::clamp((tt - 0.1f) / 0.65f, 0.0f, 1.0f);
            return glm::normalize(glm::angleAxis((tau - 1.0f) * a.tw_u, ap) * a.q_u);
        };
        // Just past the shoulder joint: from half way between the chest's turn and the upper arm's, to the upper arm's.
        auto shoulder_zone = [&](float m_after) {
            const float f = 0.5f + 0.5f * std::clamp(m_after / kZoneSh, -1.0f, 1.0f);
            return rigid(a.S, glm::slerp(a.q_chest, up_at(0.0f), f), Sr, ar, 1.0f);
        };
        // Around the elbow: from the upper arm's turn to the forearm's.
        auto elbow_zone = [&](float m_after) {
            const float f = 0.5f + 0.5f * std::clamp(m_after / kZone, -1.0f, 1.0f);
            return rigid(a.E, glm::slerp(a.q_u, a.q_fe, f), Er, fr, 1.0f);
        };
        switch (region) {
        case kUpper: {
            const float after_s = t * a0, before_e = (1.0f - t) * a0;
            if (after_s < kZoneSh) return shoulder_zone(after_s);
            if (before_e < kZone) return elbow_zone(-before_e);
            return rigid(a.S, up_at(t), Sr, ar, a.k);
        }
        case kFore: {
            const float after_e = t * b0;
            if (after_e < kZone) return elbow_zone(after_e);
            const float t0  = kZone / b0;
            const float tau = std::clamp((t - t0) / (0.95f - t0), 0.0f, 1.0f);
            return rigid(a.E, glm::normalize(glm::angleAxis(tau * a.tw_f, fp) * a.q_fe), Er, fr, a.k);
        }
        default: return a.hand;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------- draws

    void begin_frame(Frame& f, int frame, float dt, const BodyPose& body, const glm::mat4& carry_right, const glm::mat4& carry_left)
    {
        for (int s = 0; s < 2; ++s) {
            Side& sd = f.side[s];
            if (sd.arm_frame == f.frame && sd.arm.ok) {   // the last frame's solve: where its easing goes on from
                sd.pole       = sd.arm.pole;
                sd.twist      = sd.arm.tw_f;
                sd.pole_frame = sd.arm_frame;
            }
            sd.wg_ok = false;
            sd.carry = s == 0 ? carry_right : carry_left;
            if (frame != f.frame) {
                // An older hand goes on with its controller until this frame's own hand draw comes.
                const glm::mat4 h = sd.carry * sd.hand;
                if (finite3(glm::vec3{ h[3] })) sd.hand = h;
                sd.exact = false;
            }
        }
        f.frame = frame;
        f.dt    = dt;
        f.body  = body;
    }

    void carry_hands(Frame& f, const glm::mat4& carry_right, const glm::mat4& carry_left)
    {
        for (int s = 0; s < 2; ++s) {
            Side& sd = f.side[s];
            if (sd.exact && sd.hand_frame == f.frame) continue;   // already drawn this frame
            sd.carry        = s == 0 ? carry_right : carry_left;
            const glm::mat4 h = sd.carry * sd.hand;
            if (finite3(glm::vec3{ h[3] })) sd.hand = h;
            if (sd.arm_frame == f.frame) sd.arm_frame = -1000;   // an arm solved this frame on the uncarried hand is solved again
        }
    }

    namespace
    {
        // With no hand drawn for a side, its arm hangs by the body.
        glm::mat4 hanging_hand(const BodyPose& b, int side)
        {
            const glm::vec3 Sr = rest_joint(kJShoulder, side), Wr = rest_joint(kJWrist, side);
            const glm::mat4 Bc = torso_frame(b, 1.0f);
            const glm::vec3 S  = pt(Bc, Sr);
            const glm::vec3 W  = S - b.up * 0.50f + (side == 0 ? 1.0f : -1.0f) * b.right * 0.07f + b.fwd * 0.06f;
            const glm::quat q  = glm::normalize(arc(glm::mat3_cast(rot_of(Bc)) * (Wr - Sr), W - S) * rot_of(Bc));
            glm::mat4       m{ glm::mat3_cast(q) };
            m[3] = glm::vec4{ W - glm::mat3_cast(q) * Wr, 1.0f };
            return m;
        }
        const ArmSolve& arm_of(Frame& f, const Opts& o, int side, bool* predicted)
        {
            Side&      sd   = f.side[side];
            const bool have = f.frame - sd.hand_frame <= std::max(0, o.lag);
            const bool exact = have && sd.exact && sd.hand_frame == f.frame;
            if (predicted) *predicted = have && !exact;
            if (sd.arm_frame == f.frame && sd.arm.ok && sd.arm_exact == exact) return sd.arm;
            const bool      eased = f.frame - sd.pole_frame <= 10;
            const glm::mat4 hand  = have ? sd.hand : hanging_hand(f.body, side);
            sd.arm       = solve_arm(o, f.body, side, hand, eased ? sd.pole : glm::vec3{ 0.0f }, eased ? sd.twist : 0.0f, f.dt);
            sd.arm_frame = f.frame;
            sd.arm_exact = exact;
            return sd.arm;
        }
    }

    int draw(Frame& f, const Opts& o, const DrawIn& in, float* out, DrawInfo* info)
    {
        DrawInfo di{};
        auto     done = [&](int r) {
            if (info) *info = di;
            return r;
        };
        if (!in.game || !in.fin || !in.cent || !out) return done(kBadInput);
        const int n = std::clamp(in.nslots, 0, 256);
        std::memcpy(out, in.fin, 256 * 12 * sizeof(float));
        di.roles = classify(in.cent, n);
        if (in.mesh != 0 && in.pass != kWorld && f.body.ok) di.shared = share_bones(f.bones, f.frame, in.mesh, n, in.cent, in.game, in.M, di.roles);
        const Roles& R = di.roles;
        if (!f.body.ok) return done(kNoBody);
        const glm::mat4 Minv = glm::inverse(in.M);
        if (!finite3(glm::vec3{ Minv[3] })) return done(kBadInput);
        auto game = [&](int s) { return from_rows(in.game + size_t(s) * 12); };

        if (in.pass == kWorld) {
            // The legs: the game's own animation, as one piece under the body.
            if (!o.legs) return done(kLegsOff);
            if (R.total <= 0.0f || R.legs() < 0.5f * R.total) return done(kNotLegs);
            const glm::mat4 Bl = legs_frame(f.body, glm::dot(glm::vec3{ in.M[3] }, f.body.up));
            const glm::mat4 X  = Minv * Bl;
            // The game's hip, knee and ankle of each leg (object space): where this draw's thigh and shin slots put the rest joints (the
            // one nearest the others, as the wrist). A draw with both gives the legs' solve; the boots after it use the same.
            auto medoid = [&](int side, int joint, auto&& want, glm::vec3& outp) {
                glm::vec3 cand[12];
                float     cw[12];
                int       nc = 0;
                const glm::vec3 jr = rest_joint(joint, side);
                for (int s = 0; s < n && nc < 12; ++s) {
                    const Role& ro = R.slot[s];
                    if (ro.side != side || ro.w < 1.0f || !want(ro)) continue;
                    const glm::vec3 p = pt(game(s), jr);
                    if (finite3(p)) { cand[nc] = p; cw[nc++] = ro.w; }
                }
                int   best = -1;
                float bs   = 1e30f;
                for (int i = 0; i < nc; ++i) {
                    float sum = 0.0f;
                    for (int j = 0; j < nc; ++j) sum += cw[j] * std::min(0.05f, glm::length(cand[i] - cand[j]));
                    if (sum < bs) { bs = sum; best = i; }
                }
                if (best >= 0) outp = cand[best];
                return best >= 0;
            };
            glm::vec3 H[2], K[2], A[2];
            bool      got[2]{ false, false };
            for (int side = 0; side < 2; ++side)
                got[side] = medoid(side, kJHip, [](const Role& r) { return r.region == kUpLeg && r.t <= 0.75f; }, H[side]) &&
                            medoid(side, kJKnee, [](const Role& r) { return r.region == kUpLeg || r.region == kLeg; }, K[side]) &&
                            medoid(side, kJAnkle, [](const Role& r) { return (r.region == kLeg && r.t >= 0.25f) || r.region == kFoot; }, A[side]);
            // bodyiklegsupright (16 Nov, the author on 5AD4D339, the crashed helicopter cutscene): the
            // game hangs the player head down in the wreck (its root 0.67 m above the camera, the feet "squared" -170 deg), and the legs,
            // moved as one piece under the upright body, were drawn feet up in front of the face. The game's ankles above its hips in the
            // legs' object space (its +z, which the legs keep as the body's up when moved under it; 10 cm or more on the mean of the legs
            // read) is no stance the body under the headset can take: the legs stay hidden, as before bodyik. The boots' own draw after the trousers' (no hip read) follows the trousers' verdict for that frame and the next.
            if (o.legs_upright) {
                float hip = 0.0f, ank = 0.0f;
                int   nl  = 0;
                for (int side = 0; side < 2; ++side) {
                    if (!got[side]) continue;
                    hip += H[side].z;
                    ank += A[side].z;
                    ++nl;
                }
                // this draw's own legs decide; a draw with none (the boots) takes the last verdict, this frame or the one before
                const bool inverted = nl > 0 ? ank - hip >= 0.10f * float(nl) : f.frame - f.inverted_frame <= 1;
                if (inverted) {
                    if (nl > 0) f.inverted_frame = f.frame;
                    return done(kLegsInverted);
                }
            }
            // bodyikfeet: each foot's heading as the game poses it (ankle to toe, the legs' object space), from whichever draw has the feet
            // (the boots are often their own draw, after the trousers').
            for (int side = 0; side < 2; ++side) {
                glm::vec3 fa, ft;
                auto      foot = [](const Role& r) { return r.region == kFoot; };
                if (medoid(side, kJAnkle, foot, fa) && medoid(side, kJToe, foot, ft) && glm::length(glm::vec2{ ft - fa }) > 0.05f) {
                    f.toe_dir[side]   = glm::normalize(glm::vec2{ ft - fa });
                    f.toe_frame[side] = f.frame;
                }
            }
            const glm::mat4 Bli = glm::inverse(Bl);
            if (got[0] && got[1]) {
                // The game's hips above the body's own, taken with the head level at the reference height (drop: how far the body's hips
                // are below that place now): the legs keep the game's stance there, and only what the hips go down bends the knees.
                const float game_hips = glm::dot(pt(Bl, 0.5f * (H[0] + H[1])), f.body.up);
                const float sample    = game_hips - (glm::dot(f.body.hips, f.body.up) + f.body.drop);
                // A few centimetres only: more is not the stance (the head held higher or lower than the legs reach), and the legs' hips
                // would leave the torso's by it. The rest is the knees' (lower) or the feet's (higher: they leave the floor).
                const float sc = std::clamp(sample, -0.06f, 0.06f);
                if (!f.cal_ok) { f.cal = sc; f.cal_ok = true; }
                else f.cal += (sc - f.cal) * (1.0f - std::exp(-std::max(0.0f, f.dt) / 1.0f));
                di.legs_read = true;
            }
            // The legs' own hips: under the body's, the game's stance higher or lower (cal).
            glm::mat4 Bp = torso_frame(f.body, 0.0f);
            Bp[3] += glm::vec4{ f.body.up * (f.cal_ok ? f.cal : 0.0f), 0.0f };
            const glm::vec3 Ht[2]{ pt(Bli * Bp, rest_joint(kJHip, 0)), pt(Bli * Bp, rest_joint(kJHip, 1)) };
            // The game's stance brought under the body: holding a long gun the game stands the hips 20 to 30 cm behind its root and turned
            // (11 Nov capture: 43 deg), so with the feet left where they were the legs trailed behind the body. The legs as one piece: the
            // game's hips' middle onto the body's and their line onto the body's (a turn about the vertical, a level move); the feet keep
            // their place from the hips, and the solve below only has the height left.
            if (got[0] && got[1]) {
                const glm::vec3 hl = H[0] - H[1], tl = Ht[0] - Ht[1];
                float           a  = std::atan2(tl.y, tl.x) - std::atan2(hl.y, hl.x);
                while (a > kPi) a -= 2.0f * kPi;
                while (a < -kPi) a += 2.0f * kPi;
                if (glm::length(glm::vec2{ hl }) <= 0.05f) a = 0.0f;
                // bodyikfeet (author 13 Nov, roomscale on): the game turns its hips off its feet (a long gun held: the hips turned right while the feet walk straight),
                // so squaring the hip line under the body turned the feet left by that twist. The legs now also turn until the mean of the two
                // feet (ankle to toe, level) points along the body, up to feet_max, eased over a quarter second (a step's swing averages out).
                // The target is the game's own heading (feet_to 1: where it walks; the drawn body can be a few degrees off it, the gestures' body
                // yaw following the head past a dead zone) or the drawn body's (0). The game's heading: its object +y, in the body's frame.
                auto wrap = [](float x) { while (x > kPi) x -= 2.0f * kPi; while (x < -kPi) x += 2.0f * kPi; return x; };
                if (f.feet_frame != f.frame) {   // once a frame (the legs can come in more than one draw)
                    f.feet_frame = f.frame;
                    const glm::vec3 ry = glm::mat3{ Bli * in.M } * glm::vec3{ 0.0f, 1.0f, 0.0f };
                    f.root_ang         = glm::length(glm::vec2{ ry }) > 1e-4f ? wrap(std::atan2(ry.y, ry.x) - 0.5f * kPi) : 0.0f;
                    f.hip_ang          = a;
                    f.root_pos         = glm::vec3{ in.M[3] };
                    float     want = 0.0f;
                    glm::vec2 fs{ 0.0f };
                    int       nf = 0;
                    for (int side = 0; side < 2; ++side) {
                        const glm::vec2 d = f.toe_dir[side];
                        if (f.frame - f.toe_frame[side] <= 2) { fs += glm::vec2{ std::cos(a) * d.x - std::sin(a) * d.y, std::sin(a) * d.x + std::cos(a) * d.y }; ++nf; }
                    }
                    f.feet_read = nf == 2 && glm::length(fs) > 1.0f;   // both feet read lately, and not opposed
                    if (f.feet_read) {
                        f.feet_body = wrap(std::atan2(fs.y, fs.x) - 0.5f * kPi);   // the model's forward is +y
                        f.feet_raw  = o.feet_to == 2 ? wrap(f.root_ang - a) : wrap((o.feet_to == 1 ? f.root_ang : 0.0f) - f.feet_body);
                        if (o.feet_max > 0.0f) want = std::clamp(f.feet_raw, -o.feet_max, o.feet_max);
                    }
                    // bodyiksnaphold: through a snap the turn holds (the game's heading is the snap's angle off the body for a few frames);
                    // bodyikturnease: eased over feet_tau (0.25 s until 14 Nov).
                    if (!f.turn_hold) f.feet += (want - f.feet) * (1.0f - std::exp(-std::max(0.0f, f.dt) / std::max(0.02f, o.feet_tau)));
                }
                glm::mat4       T  = glm::rotate(glm::mat4{ 1.0f }, a + f.feet, glm::vec3{ 0.0f, 0.0f, 1.0f });
                const glm::vec3 d  = 0.5f * (Ht[0] + Ht[1]) - pt(T, 0.5f * (H[0] + H[1]));
                T[3]     = glm::vec4{ d.x, d.y, 0.0f, 1.0f };
                f.stance = T;
            }
            for (int side = 0; side < 2; ++side) {
                if (!got[side]) continue;
                f.leg[side]       = solve_leg(pt(f.stance, H[side]), pt(f.stance, K[side]), pt(f.stance, A[side]), Ht[side]);
                f.leg_frame[side] = f.frame;
            }
            for (int s = 0; s < n; ++s) {
                const Role& ro = R.slot[s];
                float*      po = out + size_t(s) * 12;
                const bool  leg_part = ro.region == kUpLeg || ro.region == kLeg || ro.region == kFoot;
                const bool  solved   = leg_part && f.leg[ro.side].ok && f.frame - f.leg_frame[ro.side] <= std::max(0, o.lag);
                if (solved) {
                    to_rows(X * leg_move(f.leg[ro.side], ro.region, ro.t) * f.stance * game(s), po);
                    di.knee[ro.side] = std::acos(std::clamp(glm::dot(glm::normalize(f.leg[ro.side].Kn - f.leg[ro.side].Ht), glm::normalize(f.leg[ro.side].An - f.leg[ro.side].Kn)), -1.0f, 1.0f));
                } else if (!leg_part && ro.region != kNone && f.leg[0].ok && f.leg[1].ok && f.frame - f.leg_frame[0] <= o.lag && f.frame - f.leg_frame[1] <= o.lag) {
                    // the waistband: on the legs' hips, upright (rest pose); bodyikfeet: turned with the legs by feet_pelvis of their extra turn
                    const glm::vec3 c = 0.5f * (Ht[0] + Ht[1]);
                    const glm::mat4 P = glm::translate(glm::mat4{ 1.0f }, c) * glm::rotate(glm::mat4{ 1.0f }, o.feet_pelvis * f.feet, glm::vec3{ 0.0f, 0.0f, 1.0f }) *
                                        glm::translate(glm::mat4{ 1.0f }, -c);
                    to_rows(Minv * Bl * P * Bli * Bp, po);
                } else {
                    to_rows(X * game(s), po);   // the game's own pose, moved as one piece
                }
                di.placed++;
            }
            return done(kMoved);
        }
        if (R.total <= 0.0f) return done(kNothing);
        if (R.part(kNeck) > 0.5f * R.total) return done(kHeadPiece);
        const bool hand_draw = in.pass == kHandDraw;

        // The game's wrist of each side (world as drawn, the game's pose): where this draw's forearm slots put the rest wrist. They agree
        // (the wrist is on every forearm bone's axis); the one nearest the others is taken.
        for (int side = 0; side < 2; ++side) {
            Side& sd = f.side[side];
            if (sd.wg_ok) continue;
            const glm::vec3 Wr = rest_joint(kJWrist, side);
            glm::vec3       cand[16];
            float           cw[16];
            int             nc = 0;
            for (int s = 0; s < n && nc < 16; ++s) {
                const Role& ro = R.slot[s];
                if (ro.region != kFore || ro.side != side || ro.t < 0.35f || ro.w < 1.0f) continue;
                const glm::vec3 p = pt(in.M * game(s), Wr);
                if (!finite3(p)) continue;
                cand[nc] = p;
                cw[nc++] = ro.w;
            }
            int   best = -1;
            float bs   = 1e30f;
            for (int i = 0; i < nc; ++i) {
                float sum = 0.0f;
                for (int j = 0; j < nc; ++j) sum += cw[j] * std::min(0.05f, glm::length(cand[i] - cand[j]));
                if (sum < bs) {
                    bs   = sum;
                    best = i;
                }
            }
            if (best >= 0) {
                sd.wg    = cand[best];
                sd.wg_ok = true;
            }
        }

        // A hand draw gives its sides' hands: the slot of the hand part that keeps the game's wrist (the hand bone or one fixed in it; a
        // finger's does not once it curls). Without a forearm to compare with: the heaviest-enough slot nearest the wrist.
        bool leave[2]{ false, false };   // the side's slots stay as the mod made them
        for (int side = 0; side < 2 && hand_draw; ++side) {
            Side&           sd = f.side[side];
            const glm::vec3 Wr = rest_joint(kJWrist, side);
            float           wmax = 0.0f;
            for (int s = 0; s < n; ++s)
                if (R.slot[s].region == kHand && R.slot[s].side == side) wmax = std::max(wmax, R.slot[s].w);
            if (wmax <= 0.0f) continue;
            int   h   = -1;
            float hs  = 1e30f, fit = -1.0f;
            for (int s = 0; s < n; ++s) {
                const Role& ro = R.slot[s];
                if (ro.region != kHand || ro.side != side || ro.t > 0.65f) continue;
                float score;
                if (sd.wg_ok) {
                    if (ro.w < 1.0f) continue;
                    score = glm::length(pt(in.M * game(s), Wr) - sd.wg) + 0.002f * ro.t;   // ties: the nearer the wrist
                } else {
                    if (ro.w < 0.15f * wmax) continue;
                    score = ro.t + 0.5f * ro.fit;
                }
                if (score < hs) {
                    hs = score;
                    h  = s;
                }
            }
            if (h >= 0 && sd.wg_ok) {
                fit = glm::length(pt(in.M * game(h), Wr) - sd.wg);
                if (fit > 0.02f) h = -1;   // no slot keeps the wrist: not a hand as the skeleton has it
            }
            di.wrist_fit[side] = fit;
            di.hand_slot[side] = h;
            if (h < 0) {
                leave[side] = true;
                continue;
            }
            const glm::mat4 Hf = in.M * from_rows(in.fin + size_t(h) * 12);
            const glm::mat4 Hg = in.M * game(h);
            const float     sc = glm::length(glm::vec3{ Hf[0] });
            if (!finite3(glm::vec3{ Hf[3] }) || !(sc > 0.3f && sc < 3.0f) || std::abs(glm::determinant(glm::mat3{ Hg })) < 1e-6f) {
                leave[side] = true;
                continue;
            }
            sd.hand         = Hf;
            sd.move         = Hf * glm::inverse(Hg);
            sd.exact        = true;
            sd.hand_frame   = f.frame;
            di.solved[side] = true;
        }

        // The slots.
        const ArmSolve* arm[2]{ nullptr, nullptr };
        auto            arm_for = [&](int side) -> const ArmSolve& {
            if (!arm[side]) arm[side] = &arm_of(f, o, side, &di.predicted[side]);
            return *arm[side];
        };
        for (int s = 0; s < n; ++s) {
            const Role& ro = R.slot[s];
            float*      po = out + size_t(s) * 12;
            switch (ro.region) {
            case kNone: di.unused++; continue;
            case kTorso: to_rows(Minv * torso_frame(f.body, ro.t), po); break;
            case kClav: to_rows(Minv * torso_frame(f.body, 1.0f), po); break;   // the shoulders square on the chest
            case kNeck:
                if (o.collar) to_rows(Minv * torso_frame(f.body, 1.0f), po);
                else {   // shrunk onto the neck joint
                    glm::mat4 z{ 0.0f };
                    z[3] = glm::vec4{ pt(Minv, f.body.neck), 1.0f };
                    to_rows(z, po);
                }
                break;
            case kUpLeg: case kLeg: case kFoot: to_rows(Minv * torso_frame(f.body, 0.0f), po); break;   // a coat's skirt: with the hips
            case kUpper: case kFore: {
                if (leave[ro.side]) { di.kept++; continue; }
                const ArmSolve& a = arm_for(ro.side);
                if (!a.ok) { di.kept++; continue; }
                to_rows(Minv * arm_frame(a, ro.region, ro.t), po);
                break;
            }
            case kHand: {
                const Side& sd = f.side[ro.side];
                if (hand_draw) {
                    // The mod's own hand and fingers; a slot it folded onto the wrist (a bare arm's last forearm bones) goes with the hand.
                    const bool folded = in.folded ? in.folded[s] != 0 : (in.fold_side == ro.side && std::abs(in.cent[s].y) < in.fold_from);
                    if (!leave[ro.side] && folded && sd.exact && sd.hand_frame == f.frame) {
                        to_rows(Minv * sd.hand, po);
                        break;
                    }
                    di.kept++;
                    continue;
                }
                const ArmSolve& a = arm_for(ro.side);
                if (!a.ok) { di.kept++; continue; }
                if (sd.exact && sd.hand_frame == f.frame) to_rows(Minv * sd.move * in.M * game(s), po);   // moved as the mod moved the hand
                else to_rows(Minv * a.hand, po);                                                           // with the hand, rest fingers
                break;
            }
            default: di.unused++; continue;
            }
            di.placed++;
        }
        return done(di.placed > 0 ? kMoved : kNothing);
    }
    // ---- handroles ----
    namespace
    {
        // The right hand's finger joints (rest, m): root, three joints, tip; thumb, index, middle, ring, little. The left hand: x mirrored.
        constexpr float kFinger[5][5][3] = {
            { { 0.5934f, 0.1001f, 1.1262f }, { 0.5819f, 0.1220f, 1.1179f }, { 0.5775f, 0.1672f, 1.0965f }, { 0.5748f, 0.1943f, 1.0837f }, { 0.5715f, 0.2281f, 1.0678f } },
            { { 0.5961f, 0.1235f, 1.1156f }, { 0.6253f, 0.1717f, 1.0905f }, { 0.6436f, 0.2031f, 1.0747f }, { 0.6556f, 0.2215f, 1.0645f }, { 0.6697f, 0.2434f, 1.0524f } },
            { { 0.6003f, 0.1152f, 1.1121f }, { 0.6391f, 0.1564f, 1.0791f }, { 0.6633f, 0.1830f, 1.0586f }, { 0.6782f, 0.1993f, 1.0460f }, { 0.6997f, 0.2229f, 1.0277f } },
            { { 0.6057f, 0.1062f, 1.1078f }, { 0.6453f, 0.1321f, 1.0781f }, { 0.6712f, 0.1495f, 1.0544f }, { 0.6890f, 0.1615f, 1.0382f }, { 0.7086f, 0.1746f, 1.0203f } },
            { { 0.6094f, 0.0957f, 1.1048f }, { 0.6432f, 0.1113f, 1.0764f }, { 0.6644f, 0.1179f, 1.0578f }, { 0.6788f, 0.1224f, 1.0452f }, { 0.6964f, 0.1279f, 1.0298f } },
        };
        constexpr float kHandReach   = 0.08f;   // a hand slot is within this of a finger bone
        constexpr float kJointEnd    = 0.15f;   // this near a finger bone's end: on the joint
        constexpr float kPalmEnd     = 0.30f;   // the palm: this near the wrist end of the hand
        constexpr float kCuffFrom    = 0.66f;   // the cuff: the forearm from here to the wrist (the bare arms' fold starts there today)
        constexpr float kCuffRadius  = 0.07f;
        constexpr float kHandWeight  = 60.0f;
        constexpr float kFingerWeight = 3.0f;
        constexpr float kCentreOff   = 0.015f;  // the hand's centre this far from the full hand's: the mesh covers the hand unevenly
    }

    glm::vec3 rest_finger(int side, int finger, int k)
    {
        finger = std::clamp(finger, 0, 4);
        k      = std::clamp(k, 0, 4);
        return glm::vec3{ side == 1 ? -kFinger[finger][k][0] : kFinger[finger][k][0], kFinger[finger][k][1], kFinger[finger][k][2] };
    }

    // The weighted centre of the hand's and cuff's slots (weight 5 and up) on the bare arms (Body_FPSArms 16632) and the full gloves
    // (GlovesThinFull01), from the model files (tools\fullbodyik\handstudy\hs_centre.py): 4.7 cm out, 6.0 forward, 5.6 below the wrist joint.
    glm::vec3 hand_centre(int side)
    {
        const glm::vec3 w = rest_joint(kJWrist, 0);
        return glm::vec3{ (side == 1 ? -1.0f : 1.0f) * (w.x + 0.047f), w.y + 0.060f, w.z - 0.056f };
    }

    HandRoles hand_roles(const glm::vec4* cent, int n)
    {
        HandRoles r{};
        if (!cent) return r;
        n = std::clamp(n, 0, 256);
        const glm::vec3 E = rest_joint(kJElbow, 0), W = rest_joint(kJWrist, 0);
        const glm::vec3 fa = W - E;
        const float     fl = glm::dot(fa, fa);
        float           t_at[256]{};
        for (int s = 0; s < n; ++s) {
            const float w = cent[s].x;
            glm::vec3   c{ cent[s].y, cent[s].z, cent[s].w };
            if (!(w > kMinWeight) || !finite3(c)) continue;
            const int side = c.x > 0.0f ? 0 : 1;
            c.x            = std::abs(c.x);
            r.side_weight[side] += w;
            const float te = glm::dot(c - E, fa) / fl;   // along the forearm: 1 at the wrist
            // The nearest finger bone.
            float best = 1e9f, bt = 0.0f;
            int   bf = 0, bs = 0;
            for (int f = 0; f < 5; ++f)
                for (int k = 0; k < 4; ++k) {
                    const glm::vec3 a{ kFinger[f][k][0], kFinger[f][k][1], kFinger[f][k][2] }, b{ kFinger[f][k + 1][0], kFinger[f][k + 1][1], kFinger[f][k + 1][2] };
                    float           t = 0.0f;
                    const float     d = seg_fit(c, a, b, t);
                    if (d < best) { best = d; bf = f; bs = k; bt = t; }
                }
            HandSlot& hs = r.slot[s];
            hs.side      = uint8_t(side);
            if (te >= 1.0f && best < kHandReach) {
                hs.finger = uint8_t(bf);
                if (bs == 0) {
                    if (bt >= 1.0f - kJointEnd) { hs.kind = kHandOnJoint; hs.seg = 1; }
                    else hs.kind = kHandOnPalm;
                } else if (bt <= kJointEnd) { hs.kind = kHandOnJoint; hs.seg = uint8_t(bs); }
                else if (bt >= 1.0f - kJointEnd && bs <= 2) { hs.kind = kHandOnJoint; hs.seg = uint8_t(bs + 1); }
                else { hs.kind = kHandOnBone; hs.seg = uint8_t(bs); }
                t_at[s] = bs == 0 ? bt : 1.0f;
                r.weight[side] += w;
                if (hs.kind != kHandOnPalm && w >= kFingerWeight) r.fingers[side]++;
            } else if (te >= kCuffFrom && te < 1.0f + 0.25f) {
                float       t = 0.0f;
                const float d = seg_fit(c, E, W, t);
                if (d < kCuffRadius) hs.kind = kHandCuff;
            }
        }
        for (int side = 0; side < 2; ++side) {
            // The palm: the heaviest slot riding the palm at the wrist end; else the heaviest riding the palm.
            int   palm = -1, any = -1;
            float pw = 0.0f, aw = 0.0f;
            for (int s = 0; s < n; ++s) {
                const HandSlot& hs = r.slot[s];
                if (hs.kind != kHandOnPalm || hs.side != side) continue;
                if (cent[s].x > aw) { aw = cent[s].x; any = s; }
                if (t_at[s] < kPalmEnd && cent[s].x > pw) { pw = cent[s].x; palm = s; }
            }
            if (palm < 0) palm = any;
            r.palm[side] = palm;
            if (palm >= 0) r.slot[palm].kind = kHandPalm;
            // A hand: enough of it, with a palm to carry it (a set of rings has none and stays hidden), and fingers or most of this side's weight.
            r.hand[side] = palm >= 0 && r.weight[side] >= kHandWeight && (r.fingers[side] >= 3 || r.weight[side] >= 0.5f * r.side_weight[side]);
            // The heaviest slot on each finger bone.
            for (int f = 0; f < 5; ++f)
                for (int k = 1; k <= 3; ++k) {
                    int   m = -1;
                    float mw = 0.0f;
                    for (int s = 0; s < n; ++s) {
                        const HandSlot& hs = r.slot[s];
                        if (hs.kind == kHandOnBone && hs.side == side && hs.finger == f && hs.seg == k && cent[s].x > mw) { mw = cent[s].x; m = s; }
                    }
                    if (m >= 0) r.slot[m].main = true;
                }
        }
        // A mesh with a hand on one side and a palm piece on the other (the Bayek suit's right strap, 48% of its side): both are hands, the
        // piece rides its palm (the slots of a side without a hand are folded away, so nothing is lost by taking it).
        for (int side = 0; side < 2; ++side)
            if (!r.hand[side] && r.hand[1 - side] && r.fingers[1 - side] >= 3 && r.palm[side] >= 0 && r.weight[side] >= kHandWeight) { r.hand[side] = true; break; }
        // No hand on a side: no cuff either.
        for (int side = 0; side < 2; ++side)
            if (!r.hand[side])
                for (int s = 0; s < n; ++s) if (r.slot[s].kind == kHandCuff && r.slot[s].side == side) r.slot[s].kind = kHandNone;
        // Each hand's centre as the hands' code takes it (its slots of weight 5 and up, slot 1 and up), against the full hand's.
        for (int side = 0; side < 2; ++side) {
            if (!r.hand[side]) continue;
            glm::vec3 c{ 0.0f };
            float     cw = 0.0f;
            for (int s = 1; s < n; ++s) {
                if (r.slot[s].kind == kHandNone || r.slot[s].side != side || !(cent[s].x >= 5.0f)) continue;
                c += cent[s].x * glm::vec3{ cent[s].y, cent[s].z, cent[s].w };
                cw += cent[s].x;
            }
            r.centre_off[side] = cw > 0.0f ? glm::length(c / cw - hand_centre(side)) : 1.0f;
            r.uneven[side]     = r.centre_off[side] > kCentreOff;
        }
        return r;
    }

    // ---- bodyikveh ----
    namespace
    {
        // The plane of an arm (unit, from the upper arm u and the forearm f); a straight arm: from the elbow's side p, as a bent one tends to.
        glm::vec3 arm_normal(const glm::vec3& u, const glm::vec3& f, const glm::vec3& line, const glm::vec3& p)
        {
            const glm::vec3 n = glm::cross(u, f);
            if (glm::length(n) > 1e-3f) return glm::normalize(n);
            const glm::vec3 m = glm::cross(p, line);
            if (glm::length(m) > 1e-6f) return glm::normalize(m);
            const glm::vec3 a = std::abs(u.z) < 0.9f ? glm::vec3{ 0.0f, 0.0f, 1.0f } : glm::vec3{ 1.0f, 0.0f, 0.0f };
            return glm::normalize(glm::cross(u, a));
        }
        glm::mat3 basis(const glm::vec3& a, const glm::vec3& n) { return glm::mat3{ a, n, glm::cross(a, n) }; }
        // Lengthening by k along unit a.
        glm::mat3 along(const glm::vec3& a, float k) { return glm::mat3{ 1.0f } + (k - 1.0f) * glm::outerProduct(a, a); }
        glm::mat4 about(const glm::vec3& p, const glm::mat3& r)
        {
            glm::mat4 m{ r };
            m[3] = glm::vec4{ p - r * p, 1.0f };
            return m;
        }
    }

    SeatArm seat_arm(const glm::vec3& S, const glm::vec3& E, const glm::vec3& W, const glm::vec3& target, const glm::mat3& hand, float stretch, float prev_swing, bool swing,
                     bool follow, const glm::vec3& pole)
    {
        SeatArm     r{};
        float       a = glm::length(E - S), b = glm::length(W - E);
        const float L0 = glm::length(target - S);
        if (!(a > 0.05f && b > 0.05f && L0 > 1e-4f) || !finite3(target - S)) return r;
        r.reach = L0 / (a + b);
        // The shoulder goes towards a far hand (up to kSeatLunge), then the arm lengthens (up to stretch), then the forearm is moved onto it.
        r.lunge          = (target - S) / L0 * std::clamp(L0 - kSeatLungeFrom * (a + b), 0.0f, kSeatLunge);
        const glm::vec3 S2 = S + r.lunge;
        const glm::vec3 d  = target - S2;
        const float     L  = glm::length(d);
        const glm::vec3 dn = d / L;
        r.k = std::clamp(L / (a + b), 1.0f, std::max(1.0f, stretch));
        a *= r.k;
        b *= r.k;
        r.gap = std::max(0.0f, L - (a + b));
        const float Lc = std::clamp(L, std::abs(a - b) + 1e-4f, a + b);
        // The elbow's side: where the game has it from the new shoulder-wrist line (else from the old line, else down).
        glm::vec3 p = (E - S) - glm::dot(E - S, dn) * dn;
        if (glm::length(p) < 1e-3f) {
            const glm::vec3 ow = glm::normalize(W - S);
            p                  = (E - S) - glm::dot(E - S, ow) * ow;
            p                  = p - glm::dot(p, dn) * dn;
        }
        // seatfollow: the elbow towards pole (the standing arms' rule: down, out and a little back from the body, turned a third by the hand),
        // not the game's side. The game's side is the elbow of the game's own hands on the wheel; with the hands where the player holds them
        // it bent the wrists 40-100 deg (the author's 14 Nov 16:50 video and log on 0D4830F6), and every way of moving the forearm with the hand
        // instead put the elbows low by the lap, across the chest or up in the face (carfollow 1 and 2, the bench of 14 Nov).
        if (follow && glm::length(pole) > 0.5f && finite3(pole)) {
            const glm::vec3 q = pole - glm::dot(pole, dn) * dn;
            if (glm::length(q) > 0.05f) p = q;
        }
        if (glm::length(p) < 1e-3f) p = glm::vec3{ 0.0f, 0.0f, -1.0f } - dn.z * dn;   // world down (z up)
        if (glm::length(p) < 1e-3f) p = glm::vec3{ 1.0f, 0.0f, 0.0f } - dn.x * dn;
        const glm::vec3 pn0  = glm::normalize(p);
        const float     cosA = std::clamp((a * a + Lc * Lc - b * b) / (2.0f * a * Lc), -1.0f, 1.0f);
        const float     sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
        // The game's arm frames: the bone along its axis, the elbow's hinge kept.
        const glm::vec3 u  = glm::normalize(E - S), f = glm::normalize(W - E);
        const glm::vec3 n  = arm_normal(u, f, glm::normalize(W - S), (E - S) - glm::dot(E - S, glm::normalize(W - S)) * glm::normalize(W - S));
        const glm::vec3 nu = glm::normalize(n - glm::dot(n, u) * u), nf = glm::normalize(n - glm::dot(n, f) * f);
        // One elbow place: the game's side turned by phi about the shoulder-wrist line. Gives the new elbow, the two swings and the forearm's roll
        // left for the hand (its twist about the new forearm).
        struct Try
        {
            glm::vec3 E2{ 0.0f }, f2{ 0.0f };
            glm::mat3 Ru{ 1.0f }, Rf{ 1.0f };
            float     tw{ 0.0f };
        };
        auto attempt = [&](float phi) {
            Try             t{};
            const glm::vec3 pn = glm::angleAxis(phi, dn) * pn0;
            t.E2               = S2 + a * (cosA * dn + sinA * pn);
            const glm::vec3 W2 = S2 + Lc * dn;
            const glm::vec3 u2 = glm::normalize(t.E2 - S2);
            t.f2               = glm::normalize(W2 - t.E2);
            const glm::vec3 n2  = arm_normal(u2, t.f2, dn, pn);
            const glm::vec3 nu2 = glm::normalize(n2 - glm::dot(n2, u2) * u2), nf2 = glm::normalize(n2 - glm::dot(n2, t.f2) * t.f2);
            t.Ru                = basis(u2, nu2) * glm::transpose(basis(u, nu));
            t.Rf                = basis(t.f2, nf2) * glm::transpose(basis(f, nf));
            // The forearm's roll towards the hand's: the hand's turn less the forearm's, its twist about the forearm.
            const glm::quat q  = glm::quat_cast(hand * glm::transpose(t.Rf));
            const float     pr = glm::dot(glm::vec3{ q.x, q.y, q.z }, t.f2);
            float           tw = 2.0f * std::atan2(pr, q.w);
            while (tw > kPi) tw -= 2.0f * kPi;
            while (tw < -kPi) tw += 2.0f * kPi;
            t.tw = tw;
            return t;
        };
        // seatswing: a hand turned far from the game's (a controller held palm in, thumb up, where the game grips a lever palm down) left the
        // whole turn to the forearm, which then wrung like a towel (author 13 Nov 22:31, helicopter). As a
        // person does, the elbow swings about the shoulder-wrist line to take part of it: the place with the least roll past kSeatRollEasy,
        // each degree of swing counted, a swing away from the last frame's counted again, an elbow raised over the shoulder counted heavily.
        auto cost = [&](const Try& t, float phi) {
            const float up = std::max(0.0f, t.E2.z - std::max(S2.z, target.z) - 0.03f);   // m the elbow stands over the shoulder and the hand (z up)
            return std::max(0.0f, std::abs(t.tw) - kSeatRollEasy) + 0.6f * std::abs(phi) + 0.3f * std::abs(phi - prev_swing) + 15.0f * up;
        };
        // The place aimed at (a straight arm has no elbow to swing: back to the game's side), then the elbow eased towards it by at most
        // kSeatSwingRate a frame, so a hand turning across a step does not make the elbow jump.
        float bphi = 0.0f;
        if (swing && !follow && sinA > 0.05f) {
            float bc = cost(attempt(0.0f), 0.0f);
            for (int i = -kSeatSwingSteps; i <= kSeatSwingSteps; ++i) {
                if (i == 0) continue;
                const float phi = kSeatSwingMax * float(i) / float(kSeatSwingSteps);
                const float c   = cost(attempt(phi), phi);
                if (c < bc - 1e-4f) { bphi = phi; bc = c; }
            }
        }
        const float phi  = swing && !follow ? prev_swing + std::clamp(bphi - prev_swing, -kSeatSwingRate, kSeatSwingRate) : 0.0f;
        const Try   best = attempt(phi);
        r.swing          = phi;
        r.E     = best.E2;
        r.W     = S2 + Lc * dn;
        r.upper = glm::translate(glm::mat4{ 1.0f }, r.lunge) * about(S, best.Ru * along(u, r.k));
        // What is left past the reach: the forearm moved onto the hand (the elbow opens by it), so the wrist is always on the hand.
        const glm::vec3 pull = target - r.W;
        r.fore  = glm::translate(glm::mat4{ 1.0f }, r.E - E + pull) * about(E, best.Rf * along(f, r.k));
        r.W     = target;
        // The roll the forearm takes: at most kSeatRollMax (a forearm turns about 90 deg either way); past it the hand's own slots carry the rest.
        r.roll_asked = best.tw;
        r.twist      = swing || follow ? std::clamp(best.tw, -kSeatRollMax, kSeatRollMax) : best.tw;
        const glm::vec3 fh = hand * (W - E);
        r.bend       = glm::length(fh) > 1e-4f ? std::acos(std::clamp(glm::dot(best.f2, glm::normalize(fh)), -1.0f, 1.0f)) : 0.0f;
        r.axis       = best.f2;
        r.pivot      = r.E + pull;
        r.ok         = finite3(r.E) && finite3(r.W) && std::isfinite(r.twist);
        return r;
    }

    int seat(SeatState& st, const SeatIn& in, float* out, SeatInfo* info)
    {
        SeatInfo si{};
        auto     done = [&](int r) {
            if (info) *info = si;
            return r;
        };
        if (!in.game || !in.fin || !in.cent || !out) return done(kBadInput);
        const int n = std::clamp(in.nslots, 0, 256);
        std::memcpy(out, in.fin, 256 * 12 * sizeof(float));
        si.roles      = classify(in.cent, n);
        const Roles& R = si.roles;
        if (R.total <= 0.0f) return done(kNothing);
        if (R.part(kNeck) > 0.5f * R.total) return done(kHeadPiece);
        const glm::mat4 Mi = glm::inverse(in.M);
        if (!finite3(glm::vec3{ Mi[3] })) return done(kBadInput);
        auto game = [&](int s) { return in.M * from_rows(in.game + size_t(s) * 12); };   // world, the game's pose

        // The game's shoulder, elbow and wrist of each side this draw has (the one of its slots nearest the others, as draw's wrist).
        for (int side = 0; side < 2; ++side) {
            for (int j = 0; j < 3; ++j) {
                const int       joint = j == 0 ? kJShoulder : j == 1 ? kJElbow : kJWrist;
                const glm::vec3 jr    = rest_joint(joint, side);
                glm::vec3       cand[16];
                float           cw[16];
                int             nc = 0;
                for (int s = 0; s < n && nc < 16; ++s) {
                    const Role& ro = R.slot[s];
                    if (ro.side != side || ro.w < 1.0f) continue;
                    const bool want = j == 0 ? (ro.region == kUpper && ro.t <= 0.6f)
                                    : j == 1 ? ((ro.region == kUpper && ro.t >= 0.4f) || (ro.region == kFore && ro.t <= 0.6f))
                                             : (ro.region == kFore && ro.t >= 0.35f);
                    if (!want) continue;
                    const glm::vec3 pj = pt(game(s), jr);
                    if (!finite3(pj)) continue;
                    cand[nc] = pj;
                    cw[nc++] = ro.w;
                }
                int   best = -1;
                float bs   = 1e30f;
                for (int i = 0; i < nc; ++i) {
                    float sum = 0.0f;
                    for (int k = 0; k < nc; ++k) sum += cw[k] * std::min(0.05f, glm::length(cand[i] - cand[k]));
                    if (sum < bs) { bs = sum; best = i; }
                }
                // Kept in the draw's object space (the player's own transform, the same for every draw of the seated body), not the world:
                // the hand draws come before the shirt that gives the shoulder, so they took last frame's shoulder, left behind by the
                // vehicle's travel (at 15-19 m/s, 16-20 cm a frame: the author's car of 13 Nov 22:30, the arms straight at 100-108% of their length).
                if (best >= 0) { st.j[side][j] = pt(Mi, cand[best]); st.jf[side][j] = in.frame; }
            }
        }

        // seatfollow: the game's palm turn (rest -> game, the heaviest hand slot near the wrist) of each side whose hand this draw has, kept as the joints.
        for (int side = 0; side < 2; ++side) {
            int   ps = -1;
            float pw = 0.0f;
            for (int s = 0; s < n; ++s) {
                const Role& ro = R.slot[s];
                if (ro.side != side || ro.region != kHand || ro.t > 0.2f || ro.w < 1.0f) continue;
                if (ro.w > pw) { pw = ro.w; ps = s; }
            }
            if (ps < 0) continue;
            const glm::mat3 pr = glm::mat3{ Mi * game(ps) };
            if (finite3(pr[0]) && finite3(pr[1]) && finite3(pr[2])) { st.palm[side] = pr; st.palmf[side] = in.frame; }
        }

        // Each side's arm: solved when its hand was moved and the three joints are this frame's or the last's.
        SeatArm arm[2]{};
        for (int side = 0; side < 2; ++side) {
            if (!in.moved[side] || in.hidden[side]) continue;
            if (R.arm(side) <= 0.0f && R.weight[side][kHand] <= 0.0f) continue;   // nothing of that arm here
            bool fresh = true;
            for (int j = 0; j < 3; ++j) fresh = fresh && in.frame - st.jf[side][j] <= 1;
            if (!fresh) continue;
            const glm::vec3 J[3]{ pt(in.M, st.j[side][0]), pt(in.M, st.j[side][1]), pt(in.M, st.j[side][2]) };   // world, this draw's transform
            const glm::vec3 target = pt(in.hand[side], J[2]);
            // The elbow's swing chosen once a frame from the last frame's (every draw of the frame starts from the same).
            if (st.swing_frame[side] != in.frame) {
                st.swing_prev[side]  = in.frame - st.swing_frame[side] <= 2 ? st.swing[side] : 0.0f;
                st.swing_frame[side] = in.frame;
            }
            // seatfollow: the elbow's direction as the standing arms have it (solve_arm): down, out and a little back from the body (the
            // player's own axes: the draw's object transform), a share pole_hand of it the rest elbow turned as the drawn hand is.
            glm::vec3 pole{ 0.0f };
            if (in.follow) {
                const glm::mat3 Mr{ in.M };
                const glm::vec3 bx = glm::normalize(Mr[0]), by = glm::normalize(Mr[1]), bz = glm::normalize(Mr[2]);   // right, forward, up
                const glm::vec3 out       = (side == 0 ? 1.0f : -1.0f) * bx;
                const glm::vec3 body_pole = glm::normalize(-bz + 0.6f * out - 0.35f * by);
                glm::vec3       raw       = body_pole;
                if (in.frame - st.palmf[side] <= 1) {
                    const glm::vec3 Sr = rest_joint(kJShoulder, side), Er = rest_joint(kJElbow, side), Wr = rest_joint(kJWrist, side);
                    const glm::vec3 ur = glm::normalize(Wr - Sr);
                    const glm::vec3 vr = (Er - Sr) - glm::dot(Er - Sr, ur) * ur;
                    const glm::mat3 Q  = glm::mat3{ in.hand[side] } * Mr * st.palm[side];   // rest -> drawn, the palm
                    if (glm::length(vr) > 1e-4f) {
                        const glm::vec3 hv = Q * glm::normalize(vr);
                        if (glm::length(hv) > 1e-3f) raw = (1.0f - in.pole_hand) * body_pole + in.pole_hand * glm::normalize(hv);
                    }
                }
                if (glm::length(raw) > 1e-3f) pole = glm::normalize(raw);
            }
            arm[side] = seat_arm(J[0] + in.shift, J[1] + in.shift, J[2] + in.shift, target, glm::mat3{ in.hand[side] }, in.stretch,
                                 in.swing && !in.follow ? st.swing_prev[side] : 0.0f, in.swing && !in.follow, in.follow, pole);
            if (arm[side].ok) {
                st.swing[side]  = arm[side].swing;
                si.swing[side]  = arm[side].swing;
                si.roll[side]   = arm[side].roll_asked;
                si.bend[side]   = arm[side].bend;
                if (in.frame - st.palmf[side] <= 1) {   // against the drawn palm's straight-wrist line (the rest forearm turned as the palm is)
                    const glm::vec3 fr = glm::normalize(rest_joint(kJWrist, side) - rest_joint(kJElbow, side));
                    const glm::vec3 nf = glm::mat3{ in.hand[side] } * glm::mat3{ in.M } * st.palm[side] * fr;
                    const glm::vec3 fd = arm[side].W - arm[side].pivot;
                    if (glm::length(nf) > 1e-3f && glm::length(fd) > 1e-3f)
                        si.bend[side] = std::acos(std::clamp(glm::dot(glm::normalize(nf), glm::normalize(fd)), -1.0f, 1.0f));
                }
                si.solved[side] = true;
                si.reach[side]  = arm[side].reach;
                si.gap[side]    = arm[side].gap;
                si.lunge[side]  = glm::length(arm[side].lunge);
                si.k[side]      = arm[side].k;
                si.S[side]      = J[0] + in.shift + arm[side].lunge;
                si.E[side]      = arm[side].E;
                si.W[side]      = arm[side].W;
                si.T[side]      = target;
                const glm::mat3 Mr{ in.M };
                const glm::vec3 d = si.E[side] - si.S[side];
                si.elbow[side]    = { (side == 0 ? 1.0f : -1.0f) * glm::dot(d, glm::normalize(Mr[0])), glm::dot(d, glm::normalize(Mr[1])), glm::dot(d, glm::normalize(Mr[2])) };
            }
        }
        for (int side = 0; side < 2; ++side) si.unsolved[side] = in.moved[side] && !in.hidden[side] && !arm[side].ok && R.arm(side) > 0.0f;

        const glm::mat4 Sh = glm::translate(glm::mat4{ 1.0f }, in.shift);
        for (int s = 0; s < n; ++s) {
            const Role& ro = R.slot[s];
            float*      po = out + size_t(s) * 12;
            if (ro.region == kNone) continue;   // no vertex: as drivehands left it
            const SeatArm& a = arm[ro.side];
            glm::mat4      X = Sh;
            switch (ro.region) {
            case kHand:
                if (in.fin_hands) continue;   // drivehands' hand (its fingers posed)
                if (in.hidden[ro.side]) {   // no hand drawn on that side: shrunk onto the game's wrist
                    glm::mat4 z{ 0.0f };
                    z[3] = glm::vec4{ pt(Mi, pt(in.M, st.j[ro.side][2]) + in.shift), 1.0f };
                    to_rows(z, po);
                    si.placed++;
                    continue;
                }
                if (in.moved[ro.side]) X = in.hand[ro.side];
                break;
            case kClav:
                if (a.ok) X = glm::translate(glm::mat4{ 1.0f }, 0.6f * a.lunge) * Sh;   // the shoulder blade goes with the shoulder (most of it)
                break;
            case kUpper:
            case kFore:
                // A moved hand whose arm has no solve: drivehands' arm (folded onto the wrist), not the game's pose stretched to the hand.
                if (!a.ok && in.fin_hands && si.unsolved[ro.side]) continue;
                if (ro.region == kUpper) {
                    if (a.ok) X = a.upper * Sh;
                    break;
                }
                if (a.ok) {
                    const glm::mat4 roll = glm::translate(glm::mat4{ 1.0f }, a.pivot) * glm::mat4_cast(glm::angleAxis(a.twist * std::clamp(ro.t, 0.0f, 1.0f), a.axis)) *
                                           glm::translate(glm::mat4{ 1.0f }, -a.pivot);
                    X = roll * a.fore * Sh;
                }
                break;
            default: break;   // torso, collar, shoulders, legs: the game's seat
            }
            to_rows(Mi * X * game(s), po);
            si.placed++;
        }
        return done(kMoved);
    }

    // ------------------------------------------------------------------------------------------------------------ calibration

    float cal_arm_k(const Opts& o, float scale)
    {
        if (!o.cal || !(o.cal_span > 0.8f) || !(scale > 1e-3f)) return 1.0f;
        // In the T-pose the drawn wrist is span / 2 - grip from the body's middle (world: times the scale), the model's shoulder joint
        // kRestShoulderHalf: the arm between them, straight, is the player's arm as drawn.
        const float arm = (0.5f * o.cal_span - o.cal_grip) * scale - kRestShoulderHalf;
        const float k   = arm / kRestArm;
        return std::isfinite(k) ? std::clamp(k, 0.75f, 1.3f) : 1.0f;
    }

    float cal_shoulder_dy(const Opts& o, float scale)
    {
        if (!o.cal || !(o.cal_drop > 0.0f) || !(scale > 1e-3f)) return 0.0f;
        const float dy = o.cal_drop - kRestEyeToShoulder / scale;   // reference metres: + the shoulders lower than the model has them
        return std::isfinite(dy) ? std::clamp(dy, -0.06f, 0.06f) : 0.0f;
    }

    CalResult cal_solve(const std::vector<CalSample>& in)
    {
        CalResult r{};
        r.total = int(in.size());
        struct V { float span, drop, back, level, reach, scale; };
        std::vector<V> v;
        int            low = 0, uneven = 0, off_centre = 0, untracked = 0;
        for (const CalSample& s : in) {
            const glm::vec3 H{ s.head[3] };
            const glm::vec3 a = s.c[0], b = s.c[1];
            if (!finite3(H) || !finite3(a) || !finite3(b) || glm::length(a - b) < 1e-3f) { untracked++; continue; }
            glm::vec3 fw = -glm::vec3{ s.head[2] };
            fw.y         = 0.0f;
            if (glm::length(fw) < 1e-3f) { untracked++; continue; }
            fw                  = glm::normalize(fw);
            const glm::vec3 mid = 0.5f * (a + b);
            const float     span = glm::length(a - b);
            const float     dyab = std::abs(a.y - b.y);
            glm::vec2       hm{ H.x - mid.x, H.z - mid.z };
            if (span < 1.0f) { low++; continue; }                  // the arms not out
            if (dyab > 0.20f) { uneven++; continue; }              // one arm up, one down
            if (glm::length(hm) > 0.25f) { off_centre++; continue; }   // the head not between the hands
            V x{};
            x.span  = span;
            x.drop  = H.y - mid.y;
            x.back  = glm::dot(H - mid, fw);
            x.level = dyab;
            x.reach = s.reach[0] > 0.2f && s.reach[1] > 0.2f ? 0.5f * (s.reach[0] + s.reach[1]) : 0.0f;
            x.scale = s.scale > 1e-3f ? s.scale : 1.0f;
            v.push_back(x);
        }
        if (v.size() < 30) {
            r.why = fmt_why(int(v.size()), low, uneven, off_centre, untracked);
            return r;
        }
        // The arms straightest: the widest spans (the 95th percentile, and everything within 3% of it).
        std::vector<float> spans;
        for (const V& x : v) spans.push_back(x.span);
        std::sort(spans.begin(), spans.end());
        const float top = spans[std::min(spans.size() - 1, size_t(0.95f * float(spans.size())))];
        std::vector<float> sp, dr, bk, lv, rc, sc;
        for (const V& x : v)
            if (x.span >= 0.97f * top) {
                sp.push_back(x.span);
                dr.push_back(x.drop);
                bk.push_back(x.back);
                lv.push_back(x.level);
                sc.push_back(x.scale);
                if (x.reach > 0.0f) rc.push_back(x.reach);
            }
        auto median = [](std::vector<float> a) {
            if (a.empty()) return 0.0f;
            std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
            return a[a.size() / 2];
        };
        r.used  = int(sp.size());
        r.span  = median(sp);
        r.drop  = median(dr);
        r.back  = median(bk);
        r.level = median(lv);
        const float scale = median(sc);
        if (!(r.span >= 1.1f && r.span <= 2.3f) || !(r.drop >= 0.05f && r.drop <= 0.45f)) {
            r.why = "the measures are not a person's (arm span " + std::to_string(int(std::lround(r.span * 100.0f))) + " cm, hands " +
                    std::to_string(int(std::lround(r.drop * 100.0f))) + " cm below the eyes): stand or sit straight, arms straight out to the sides at shoulder height";
            return r;
        }
        // The drawn wrist's distance from the controller, when the body was drawn: the solve's straight arm (its drawn shoulder to the drawn
        // wrist) against the measured span. Kept only if it is a hand's (0 to 18 cm); else the usual 8 cm.
        r.grip = 0.08f;
        if (rc.size() * 2 >= sp.size()) {
            const float d = median(rc) * kRestArm;   // world units
            const float g = 0.5f * r.span - (d + kRestShoulderHalf) / scale;
            if (std::isfinite(g) && g >= 0.0f && g <= 0.18f) {
                r.grip          = g;
                r.grip_measured = true;
            }
        }
        r.height = (r.span + 2.0f * 0.11f) / 1.02f;   // fingertip to fingertip (the grip about 11 cm short of the fingertips) is about 1.02 x the height
        r.arm    = 0.5f * r.span - r.grip - kRestShoulderHalf * r.height / 1.80f;   // with shoulders as wide as the height suggests
        r.ok     = true;
        return r;
    }
}
