#pragma once
// vehcontrol: which part of the vehicle is its steering wheel, handlebars or stick, and its shape (author 11 Nov 09:09). Pure geometry, no game or D3D types: FarCry5VehHands.cpp feeds it the
// vehicle's vertices and runs it; research\2026-11-11_vehcontrol2\vcfit_harness.cpp runs the same code on vehdump folders.
//
// Everything is in the seat frame: the origin at the eyes, x right, y up, z back (the drive's recentred reference), game metres.
//   Cloud:  the vehicle's vertices around the driver, each tagged with its part (one palette slot of one draw: a bone).
//   Hands:  the game's own hands' palm centres, where the game animates them on the control (when seen).
//   Motion: a part seen turning with the steering (the animation: its axis and angle), once there is one.
// The three sources, per style:
//   geometry: wheel: a circle of 13-27 cm found among all the vertices (circles through three vertices, the one holding vertices all round);
//             bars: a part with two small ends level with each other on either side of the driver; stick: a thin long part standing up.
//   spatial:  it is ahead of the eyes, below them, within reach, centred on the driver, its face towards the eyes; the game's hands are on it.
//   animation: it is the part that turns with the steering: the shape is then searched about that axis, in the parts that turn.
// detect() gives the best candidate with each source's score and one confidence (0..1); ok says it may be used.
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fc5::vcfit
{
    enum Style : int { kWheel = 1, kBars = 2, kStick = 3 };   // = fc5::drive::Style

    struct Cloud
    {
        std::vector<glm::vec3> p;
        std::vector<uint16_t>  part;
        int                    nparts{ 0 };
        uint32_t               seed{ 0 };   // 0: the usual draws; else another series (the harness: how often a find repeats)
    };
    struct Hands
    {
        bool      ok[2]{};
        glm::vec3 p[2]{};   // 0 right, 1 left
    };
    struct Motion
    {
        std::vector<uint8_t> parts;            // per part: 1 = turns with the control
        glm::vec3            axis{ 0.0f, 0.0f, 1.0f }, point{ 0.0f };
        float                angle{ 0.0f };    // rad
    };
    struct Result
    {
        bool        ok{ false };
        int         style{ 0 };
        glm::vec3   c{ 0.0f };                 // wheel: the rim's centre; bars: their centre; stick: its top (the grip)
        glm::vec3   n{ 0.0f, 0.0f, 1.0f };     // wheel: the axis, towards the driver; bars: the steering axis (up); stick: its long axis, upwards
        float       r{ 0.0f };                 // wheel: the rim's radius; bars: half the width between the hands
        int         part{ -1 };                // the part that holds most of it (the bone to follow)
        float       conf{ 0.0f };              // 0..1
        float       geo{ 0.0f }, spatial{ 0.0f }, hands{ 0.5f }, anim{ 0.5f };   // each source's score (0.5: no data)
        int         nin{ 0 }, cov{ 0 };        // wheel: rim vertices, 15-degree sectors holding some (of 24)
        float       gap{ 0.0f };               // wheel: the widest arc of the rim with no vertex (deg)
        float       alone{ 0.0f };             // wheel: the share of the rim's sectors with nothing much beside it in its plane (a rim: near 1; a circle across a panel: near 0)
        float       hand_d[2]{ -1.0f, -1.0f }; // wheel: each game hand's distance from the rim (m; -1: not seen)
        const char* how{ "" };
    };

    namespace detail
    {
        inline float sat(float x) { return std::clamp(x, 0.0f, 1.0f); }
        // 1 inside [lo, hi], falling to 0 over `soft` outside it.
        inline float band(float x, float lo, float hi, float soft) { return x < lo ? sat(1.0f - (lo - x) / soft) : x > hi ? sat(1.0f - (x - hi) / soft) : 1.0f; }

        // Symmetric 3x3 eigenvectors (Jacobi): columns of V, values in d, ascending.
        inline void eig3(glm::mat3 A, glm::mat3& V, glm::vec3& d)
        {
            V = glm::mat3{ 1.0f };
            for (int sweep = 0; sweep < 24; ++sweep) {
                float off = std::abs(A[1][0]) + std::abs(A[2][0]) + std::abs(A[2][1]);
                if (off < 1e-12f) break;
                for (int p = 0; p < 2; ++p)
                    for (int q = p + 1; q < 3; ++q) {
                        if (std::abs(A[q][p]) < 1e-15f) continue;
                        const float th = (A[q][q] - A[p][p]) / (2.0f * A[q][p]);
                        const float t  = (th >= 0.0f ? 1.0f : -1.0f) / (std::abs(th) + std::sqrt(th * th + 1.0f));
                        const float c  = 1.0f / std::sqrt(t * t + 1.0f), s = t * c;
                        glm::mat3   J{ 1.0f };
                        J[p][p] = c; J[q][q] = c; J[q][p] = s; J[p][q] = -s;   // J[col][row]
                        A = glm::transpose(J) * A * J;
                        V = V * J;
                    }
            }
            d = glm::vec3{ A[0][0], A[1][1], A[2][2] };
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2 - i; ++j)
                    if (d[j] > d[j + 1]) {
                        std::swap(d[j], d[j + 1]);
                        std::swap(V[j], V[j + 1]);
                    }
        }
        inline void mean_cov(const std::vector<glm::vec3>& P, glm::vec3& m, glm::mat3& C)
        {
            m = glm::vec3{ 0.0f };
            for (const auto& p : P) m += p;
            m /= float(std::max<size_t>(1, P.size()));
            C = glm::mat3{ 0.0f };
            for (const auto& p : P) C += glm::outerProduct(p - m, p - m);
            C /= float(std::max<size_t>(1, P.size()));
        }
        inline float pct(std::vector<float> v, float p)
        {
            if (v.empty()) return 0.0f;
            const size_t k = std::min(v.size() - 1, size_t(p * float(v.size() - 1) + 0.5f));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        }
        inline void basis(const glm::vec3& n, glm::vec3& e1, glm::vec3& e2)
        {
            e1 = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9f ? glm::vec3{ 1, 0, 0 } : glm::vec3{ 0, 1, 0 }));
            e2 = glm::cross(n, e1);
        }
        struct Rng
        {
            uint32_t s{ 0x9E3779B9u };
            uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
            size_t   below(size_t n) { return size_t(next() % uint32_t(std::max<size_t>(1, n))); }
        };
        // The part holding the most of these points.
        inline int major_part(const Cloud& cl, const std::vector<uint32_t>& idx, float* share = nullptr)
        {
            std::vector<int> cnt(size_t(std::max(1, cl.nparts)), 0);
            for (uint32_t i : idx)
                if (cl.part[i] < cnt.size()) cnt[cl.part[i]]++;
            int b = -1;
            for (int k = 0; k < int(cnt.size()); ++k)
                if (cnt[size_t(k)] > 0 && (b < 0 || cnt[size_t(k)] > cnt[size_t(b)])) b = k;
            if (share) *share = b >= 0 && !idx.empty() ? float(cnt[size_t(b)]) / float(idx.size()) : 0.0f;
            return b;
        }
        inline bool part_in(const Motion* m, uint16_t part) { return !m || (part < m->parts.size() && m->parts[part]); }

        struct Circle
        {
            glm::vec3 c{ 0.0f }, n{ 0.0f, 0.0f, 1.0f };
            float     r{ 0.0f };
            int       cov{ 0 }, nin{ 0 };
            float     alone{ 0.0f };   // the share of the rim's 15-degree sectors where it stands alone: no more vertices in its plane 3.5 to 8.5 cm
                                       // inside or outside it than on it (a rim: near 1, less a spoke or two; a circle drawn across a panel: near 0)
            float     score{ -1.0f };
        };
        // The points within tol of the circle: how many, and the 15-degree sectors they hold.
        inline void circle_count(const std::vector<glm::vec3>& P, Circle& k, float tol, std::vector<uint32_t>* in = nullptr)
        {
            glm::vec3 e1, e2;
            basis(k.n, e1, e2);
            int on[24]{}, beside[24]{};
            k.nin = 0;
            if (in) in->clear();
            const float t2 = tol * tol;
            for (size_t i = 0; i < P.size(); ++i) {
                const glm::vec3 q  = P[i] - k.c;
                const float     h  = glm::dot(q, k.n);
                if (h > tol || h < -tol) continue;
                const glm::vec3 rv = q - k.n * h;
                const float     dr = glm::length(rv) - k.r;
                const bool rim = dr * dr + h * h <= t2;
                if (!rim && (std::abs(dr) <= 0.035f || std::abs(dr) >= 0.085f)) continue;
                const int sec = std::clamp(int((std::atan2(glm::dot(rv, e2), glm::dot(rv, e1)) + 3.14159265f) * (24.0f / 6.2831853f)), 0, 23);
                if (!rim) {
                    beside[sec]++;
                    continue;
                }
                k.nin++;
                on[sec]++;
                if (in) in->push_back(uint32_t(i));
            }
            k.cov     = 0;
            int clear = 0;
            for (int b = 0; b < 24; ++b) {
                if (on[b] == 0) continue;
                k.cov++;
                if (beside[b] <= on[b]) clear++;
            }
            k.alone = k.cov > 0 ? float(clear) / float(k.cov) : 0.0f;
        }
        // Least squares on the inliers: the plane (smallest spread), then the circle in it (Kasa).
        inline bool circle_refit(const std::vector<glm::vec3>& P, const std::vector<uint32_t>& in, Circle& k)
        {
            if (in.size() < 12) return false;
            std::vector<glm::vec3> Q;
            Q.reserve(in.size());
            for (uint32_t i : in) Q.push_back(P[i]);
            glm::vec3 m;
            glm::mat3 C, V;
            glm::vec3 d;
            mean_cov(Q, m, C);
            eig3(C, V, d);
            glm::vec3 n = glm::normalize(V[0]);
            if (glm::dot(n, k.n) < 0.0f) n = -n;
            glm::vec3 e1, e2;
            basis(n, e1, e2);
            // x^2 + y^2 + a x + b y + c = 0 over the inliers projected on the plane.
            double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxz = 0, syz = 0, sz = 0;
            for (const auto& q : Q) {
                const double x = glm::dot(q - m, e1), y = glm::dot(q - m, e2), z = x * x + y * y;
                sxx += x * x; sxy += x * y; syy += y * y; sx += x; sy += y; sxz += x * z; syz += y * z; sz += z;
            }
            const double N   = double(Q.size());
            const double a11 = sxx, a12 = sxy, a13 = sx, a22 = syy, a23 = sy, a33 = N;
            const double b1 = -sxz, b2 = -syz, b3 = -sz;
            const double det = a11 * (a22 * a33 - a23 * a23) - a12 * (a12 * a33 - a23 * a13) + a13 * (a12 * a23 - a22 * a13);
            if (std::abs(det) < 1e-18) return false;
            const double A  = (b1 * (a22 * a33 - a23 * a23) - a12 * (b2 * a33 - a23 * b3) + a13 * (b2 * a23 - a22 * b3)) / det;
            const double B  = (a11 * (b2 * a33 - a23 * b3) - b1 * (a12 * a33 - a23 * a13) + a13 * (a12 * b3 - b2 * a13)) / det;
            const double Cc = (a11 * (a22 * b3 - b2 * a23) - a12 * (a12 * b3 - b2 * a13) + b1 * (a12 * a23 - a22 * a13)) / det;
            const double cx = -0.5 * A, cy = -0.5 * B, r2 = cx * cx + cy * cy - Cc;
            if (!(r2 > 1e-6)) return false;
            k.c = m + e1 * float(cx) + e2 * float(cy);
            k.n = n;
            k.r = float(std::sqrt(r2));
            return std::isfinite(k.c.x + k.c.y + k.c.z + k.r);
        }
    }

    // (Place, from the real vehicles of 11 Nov, relative to the game camera: the wheels' centres 42-46 cm ahead and 30-35 cm under it, within
    // 5 cm of its line on six vehicles, 23 cm to the side on the heavy truck, whose rim also comes within 12 cm of the camera's plane; the
    // seat's frame itself was up to 18 cm off sideways in the simulator runs. So sideways and nearness weigh little.)
    // The wheel. Geometry: circles through three vertices of the cloud ahead of the driver, of a wheel's size, facing the eyes, the one whose
    // rim holds vertices all round (15-degree sectors); refitted on its rim's vertices. With a motion: only the parts that turn, about that axis.
    inline Result detect_wheel(const Cloud& cl, const Hands& h, const Motion* mo)
    {
        using namespace detail;
        Result res;
        res.style = kWheel;
        res.how   = "no circle of a wheel's size ahead of the driver";
        // The vertices where a wheel can be: ahead, below the eyes, within reach.
        std::vector<glm::vec3> P;
        std::vector<uint32_t>  src;
        for (size_t i = 0; i < cl.p.size(); ++i) {
            const glm::vec3& p = cl.p[i];
            if (p.x < -0.68f || p.x > 0.68f || p.y < -0.85f || p.y > 0.20f || p.z > -0.02f || p.z < -1.05f || !part_in(mo, cl.part[i])) continue;
            P.push_back(p);
            src.push_back(uint32_t(i));
        }
        if (P.size() < 40 && !mo) return res;   // (with a motion: the hub-and-hands wheel below needs no rim's vertices)
        // A thinned copy to weigh the candidates on.
        std::vector<glm::vec3> S;
        {
            const size_t step = std::max<size_t>(1, P.size() / 6000);
            for (size_t i = 0; i < P.size(); i += step) S.push_back(P[i]);
        }
        auto plausible = [&](const Circle& k, bool loose) {
            if (k.r < 0.13f || k.r > 0.27f) return false;
            if (std::abs(k.c.x) > (loose ? 0.48f : 0.42f) || k.c.y > 0.05f || k.c.y < -0.75f || k.c.z > -0.14f || k.c.z < -0.95f) return false;
            const float l = glm::length(k.c);
            if (l < 0.24f || l > 1.0f) return false;
            if (mo) {
                // About the turn's axis: the same direction (12 degrees), the centre on it (4 cm).
                if (std::abs(glm::dot(k.n, mo->axis)) < 0.978f) return false;
                const glm::vec3 d = k.c - mo->point;
                return glm::length(d - mo->axis * glm::dot(d, mo->axis)) < 0.04f;
            }
            return glm::dot(k.n, -k.c / l) > 0.30f;   // its face towards the eyes (within 72 degrees)
        };
        // Each part's vertices, to draw three from the same part (a rim is mostly one bone's).
        std::vector<std::vector<uint32_t>> of(size_t(std::max(1, cl.nparts)));
        for (size_t i = 0; i < P.size(); ++i)
            if (cl.part[src[i]] < of.size()) of[cl.part[src[i]]].push_back(uint32_t(i));
        // The vertices that lie along a thin line: those whose neighbours within 9 cm spread along one direction (a rim's tube, a spoke, a
        // bar; not a panel's or a hull's). The three vertices of a candidate circle are drawn among them: the fishing boat's rim is 256 of
        // the 8700 vertices near the driver, all on the hull's own bone, and three drawn at random among all were on it once in 37000 tries
        // (11 Nov, pass 5: found offline by luck, missed live).
        std::vector<uint32_t> thin;
        {
            const float cell = 0.09f;
            auto        key  = [&](const glm::vec3& p) {
                const int64_t x = int64_t(std::floor(p.x / cell)) + 512, y = int64_t(std::floor(p.y / cell)) + 512, z = int64_t(std::floor(p.z / cell)) + 512;
                return uint64_t((x << 40) | (y << 20) | z);
            };
            std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
            grid.reserve(P.size());
            for (size_t i = 0; i < P.size(); ++i) grid[key(P[i])].push_back(uint32_t(i));
            for (size_t i = 0; i < P.size(); ++i) {
                glm::vec3 m{ 0.0f };
                glm::mat3 C{ 0.0f };
                int       n = 0;
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dz = -1; dz <= 1; ++dz) {
                            const auto it = grid.find(key(P[i] + glm::vec3{ float(dx), float(dy), float(dz) } * cell));
                            if (it == grid.end()) continue;
                            for (uint32_t j : it->second) {
                                const glm::vec3 d = P[j] - P[i];
                                if (glm::dot(d, d) > cell * cell) continue;
                                m += d;
                                C += glm::outerProduct(d, d);
                                n++;
                            }
                        }
                if (n < 5 || n > 400) continue;   // (alone, or inside a dense mesh)
                m /= float(n);
                C = C / float(n) - glm::outerProduct(m, m);
                const float tr = C[0][0] + C[1][1] + C[2][2];
                glm::vec3   v{ 0.58f, 0.57f, 0.59f };
                for (int k = 0; k < 8; ++k) {
                    const glm::vec3 w = C * v;
                    const float     l = glm::length(w);
                    if (l < 1e-12f) break;
                    v = w / l;
                }
                const float l1 = glm::dot(v, C * v);
                if (l1 > 1e-8f && (tr - l1) < 0.45f * l1) thin.push_back(uint32_t(i));
            }
        }
        const bool by_thin = thin.size() >= 30;
        std::vector<std::vector<uint32_t>> thin_of(of.size());
        if (by_thin)
            for (uint32_t i : thin)
                if (cl.part[src[i]] < thin_of.size()) thin_of[cl.part[src[i]]].push_back(i);
        Rng rng;
        if (cl.seed) rng.s = cl.seed;
        std::vector<Circle> top;
        int                 weighed = 0;
        for (int it = 0; P.size() >= 40 && it < 30000 && weighed < 600; ++it) {
            const size_t    i0 = by_thin ? thin[rng.below(thin.size())] : rng.below(P.size());
            const glm::vec3 p0 = P[i0];
            size_t          i1 = by_thin ? thin[rng.below(thin.size())] : rng.below(P.size());
            size_t          i2 = by_thin ? thin[rng.below(thin.size())] : rng.below(P.size());
            // Two in three tries: all three from the same part.
            if (it % 3 != 0 && cl.part[src[i0]] < of.size()) {
                const auto& L = by_thin ? thin_of[cl.part[src[i0]]] : of[cl.part[src[i0]]];
                if (L.size() < 12) continue;
                i1 = L[rng.below(L.size())];
                i2 = L[rng.below(L.size())];
            }
            const glm::vec3 a = P[i1] - p0, b = P[i2] - p0;
            const float     la = glm::dot(a, a), lb = glm::dot(b, b);
            if (la < 0.0064f || lb < 0.0064f || la > 0.30f || lb > 0.30f) continue;   // 8 to 55 cm apart
            const glm::vec3 nn = glm::cross(a, b);
            const float     l2 = glm::dot(nn, nn);
            if (l2 < 1e-7f || l2 < 0.04f * la * lb) continue;   // nearly in line
            Circle k;
            k.c = p0 + (la * glm::cross(b, nn) + lb * glm::cross(nn, a)) / (2.0f * l2);
            k.r = glm::length(k.c - p0);
            k.n = nn / std::sqrt(l2);
            if (glm::dot(k.n, -k.c) < 0.0f) k.n = -k.n;
            if (!plausible(k, true)) continue;
            weighed++;
            circle_count(S, k, 0.022f);
            if (k.cov < 9 || k.nin < 9) continue;
            k.score = (float(k.cov) + std::min(1.0f, float(k.nin) / 400.0f)) * k.alone;
            // Kept if it is not a worse copy of one already kept.
            bool dup = false;
            for (auto& t : top)
                if (glm::length(t.c - k.c) < 0.04f && glm::dot(t.n, k.n) > 0.97f) {
                    dup = true;
                    if (k.score > t.score) t = k;
                }
            if (!dup) top.push_back(k);
            std::sort(top.begin(), top.end(), [](const Circle& x, const Circle& y) { return x.score > y.score; });
            if (top.size() > 8) top.resize(8);
        }
        if (top.empty() && !mo) return res;
        // Each kept circle refitted on all the vertices of its rim, then weighed with the other sources.
        Result best = res;
        for (Circle k : top) {
            std::vector<uint32_t> in;
            bool                  good = true;
            for (int round = 0; round < 3 && good; ++round) {
                circle_count(P, k, round == 0 ? 0.028f : 0.022f, &in);
                good = circle_refit(P, in, k);
                if (good && glm::dot(k.n, -k.c) < 0.0f) k.n = -k.n;
            }
            if (!good || !plausible(k, false)) continue;
            circle_count(P, k, 0.022f, &in);
            Result r;
            r.style = kWheel;
            r.c     = k.c;
            r.n     = k.n;
            r.r     = k.r;
            r.nin   = k.nin;
            r.cov   = k.cov;
            std::vector<uint32_t> gin;
            for (uint32_t i : in) gin.push_back(src[i]);
            float share = 0.0f;
            r.part      = major_part(cl, gin, &share);
            // Geometry: vertices all round, by the widest arc of the rim with none (a rim of 16 segments has one every 22.5 degrees; a wheel
            // open at the bottom leaves 60; the arc of a hood over the gauges over 180): 40 degrees or less: 1; 100: 0. And a wheel's radius.
            {
                glm::vec3 e1, e2;
                basis(k.n, e1, e2);
                std::vector<float> ang;
                ang.reserve(in.size());
                for (uint32_t i : in) ang.push_back(std::atan2(glm::dot(P[i] - k.c, e2), glm::dot(P[i] - k.c, e1)));
                std::sort(ang.begin(), ang.end());
                float g = ang.empty() ? 6.2831853f : ang.front() + 6.2831853f - ang.back();
                for (size_t i = 1; i < ang.size(); ++i) g = std::max(g, ang[i] - ang[i - 1]);
                r.gap = glm::degrees(g);
            }
            r.alone = k.alone;
            r.geo   = sat((100.0f - r.gap) / 60.0f) * (0.6f + 0.4f * band(k.r, 0.15f, 0.24f, 0.03f)) * sat((k.alone - 0.40f) / 0.35f);
            // Spatial: centred on the driver, at arm's length, below the eyes, facing them.
            const float l = glm::length(k.c);
            r.spatial     = std::max(0.3f, band(std::abs(k.c.x), 0.0f, 0.12f, 0.24f)) * band(l, 0.32f, 0.72f, 0.15f) * band(k.c.y, -0.60f, -0.08f, 0.12f) *
                            (mo ? 1.0f : sat((glm::dot(k.n, -k.c / l) - 0.30f) / 0.30f));
            // The game's hands: on the rim (12 cm from the palm's centre) or not. The author's first drive (11 Nov, the right wheel found, both
            // palms over 8 cm from its rim): hands off the rim count a little against it, not fully.
            int seen = 0, on = 0;
            for (int s = 0; s < 2; ++s) {
                if (!h.ok[s]) continue;
                seen++;
                const glm::vec3 q  = h.p[s] - k.c;
                const float     hh = glm::dot(q, k.n);
                const float     dr = glm::length(q - k.n * hh) - k.r;
                r.hand_d[s]        = std::sqrt(dr * dr + hh * hh);
                if (r.hand_d[s] < 0.12f) on++;
            }
            r.hands = seen ? 0.25f + 0.75f * float(on) / float(seen) : 0.5f;
            r.anim  = mo ? 1.0f : 0.5f;
            r.conf  = 0.40f * r.geo + 0.20f * r.spatial + 0.20f * r.hands + 0.20f * r.anim;
            // Sure enough to place the drive's wheel on it: 0.6 (the real wheels found on 11 Nov: 0.73-0.80; the van's false ring: 0.50).
            r.ok    = r.gap <= (mo ? 100.0f : 75.0f) && r.spatial > 0.15f && k.nin >= 12 && k.alone >= 0.50f && r.conf >= 0.60f;
            r.how   = r.ok ? (mo ? "the ring about the axis it turns on" : "a ring ahead of the driver") : k.alone < 0.50f ? "a circle on a surface, not a rim" : "its rim does not go all round";
            // The surest; of two about as sure, the one nearer the eyes (the wheel is in front of the dashboard, not in it).
            auto rank = [](const Result& x) { return x.conf - 0.3f * std::max(0.0f, glm::length(x.c) - 0.45f) + 0.5f * x.r; };
            if ((r.ok && !best.ok) || (r.ok == best.ok && rank(r) > rank(best))) best = r;
        }
        if (mo && !best.ok) {
            // The part that turns holds no rim (the van, 11 Nov: its turning bone is the hub, 29 vertices; the rim is drawn elsewhere), but
            // the game's hands are on one: the wheel is about the turn's axis, in the hands' plane, as wide as they hold.
            int   seen = 0;
            float rs = 0.0f, hs = 0.0f;
            for (int s = 0; s < 2; ++s) {
                if (!h.ok[s]) continue;
                const glm::vec3 q  = h.p[s] - mo->point;
                const float     hh = glm::dot(q, mo->axis);
                const float     rr = glm::length(q - mo->axis * hh);
                if (rr < 0.12f || rr > 0.28f) continue;
                seen++;
                rs += rr;
                hs += hh;
            }
            if (seen > 0) {
                Circle k;
                k.c = mo->point + mo->axis * (hs / float(seen));
                k.n = glm::dot(mo->axis, -k.c) < 0.0f ? -mo->axis : mo->axis;
                k.r = std::clamp(rs / float(seen), 0.15f, 0.25f);
                Result r;
                r.style   = kWheel;
                r.c       = k.c;
                r.n       = k.n;
                r.r       = k.r;
                r.part    = -1;   // the caller follows the part that turns
                r.geo     = 0.3f;
                const float l = glm::length(k.c);
                r.spatial = std::max(0.3f, band(std::abs(k.c.x), 0.0f, 0.12f, 0.24f)) * band(l, 0.32f, 0.72f, 0.15f) * band(k.c.y, -0.60f, -0.08f, 0.12f);
                r.hands   = 1.0f;
                r.anim    = 1.0f;
                r.conf    = 0.40f * r.geo + 0.20f * r.spatial + 0.20f * r.hands + 0.20f * r.anim;
                r.ok      = r.spatial > 0.3f && l > 0.24f;
                r.how     = "about the part that turns, where the game's hands hold";
                if (r.ok) return r;
            }
        }
        return best.part >= 0 || best.ok ? best : res;
    }

    // The handlebars. The game's two hands, when seen level on either side of the driver, are the answer: the bars' ends are where they hold
    // (the quad, 11 Nov: palms 33 cm either side, 16 cm under the eyes, 37 cm ahead), the bone is the part under them (none: the caller
    // takes the part that turns). Without them, geometry: a part with two small ends level with each other, 45 cm to 1.1 m apart, within
    // reach (the quad's front rack, 71 cm ahead and 48 cm down, was taken for the bars with a looser place).
    inline Result detect_bars(const Cloud& cl, const Hands& h, const Motion* mo)
    {
        using namespace detail;
        Result res;
        res.style = kBars;
        res.how   = "no handlebars found";
        res.n     = mo ? (mo->axis.y < 0.0f ? -mo->axis : mo->axis) : glm::vec3{ 0.0f, 1.0f, 0.0f };
        auto in_box = [](const glm::vec3& p) { return p.x > -0.65f && p.x < 0.65f && p.y > -0.75f && p.y < 0.10f && p.z < -0.10f && p.z > -0.95f; };
        auto place  = [](const glm::vec3& c) { return band(std::abs(c.x), 0.0f, 0.05f, 0.10f) * band(-c.z, 0.22f, 0.58f, 0.10f) * band(c.y, -0.42f, -0.05f, 0.10f); };
        // The game's hands.
        bool pair = false;   // the two hands as on handlebars: 50 to 110 cm apart, across, ahead of the driver
        if (h.ok[0] && h.ok[1]) {
            const glm::vec3 d = h.p[0] - h.p[1];
            const float     l = glm::length(d);
            if (l > 0.50f && l < 1.10f && d.x / l > 0.80f && in_box(h.p[0]) && in_box(h.p[1])) {   // (on a wheel the game's hands are 35-37 cm apart: 11 Nov, four vehicles)
                pair = true;
                std::vector<uint32_t> nearby[2];
                for (size_t i = 0; i < cl.p.size(); ++i) {
                    if (!part_in(mo, cl.part[i])) continue;
                    for (int s = 0; s < 2; ++s)
                        if (glm::dot(cl.p[i] - h.p[s], cl.p[i] - h.p[s]) < 0.07f * 0.07f) nearby[s].push_back(uint32_t(i));
                }
                const int pr = major_part(cl, nearby[0]), pl = major_part(cl, nearby[1]);
                Result    r  = res;
                r.c          = 0.5f * (h.p[0] + h.p[1]);
                r.r          = 0.5f * l;
                r.part       = pr >= 0 && pr == pl ? pr : -1;
                r.hands      = 1.0f;
                r.geo        = r.part >= 0 ? 1.0f : 0.5f;   // a part under both hands
                r.spatial    = place(r.c);
                r.anim       = mo ? 1.0f : 0.5f;
                r.conf       = 0.30f * r.geo + 0.20f * r.spatial + 0.30f * r.hands + 0.20f * r.anim;
                r.ok         = r.spatial > 0.15f;
                r.how        = r.part >= 0 ? "where the game's two hands hold, a part under them" : "where the game's two hands hold";
                if (r.ok) return r;
            }
        }
        // Geometry, per part.
        Result geo = res;
        std::vector<std::vector<glm::vec3>> by(size_t(std::max(1, cl.nparts)));
        for (size_t i = 0; i < cl.p.size(); ++i)
            if (in_box(cl.p[i]) && part_in(mo, cl.part[i]) && cl.part[i] < by.size()) by[cl.part[i]].push_back(cl.p[i]);
        for (int k = 0; k < int(by.size()); ++k) {
            const auto& Q = by[size_t(k)];
            if (Q.size() < 40) continue;
            std::vector<float> xs(Q.size());
            for (size_t i = 0; i < Q.size(); ++i) xs[i] = Q[i].x;
            const float lo = pct(xs, 0.02f), hi = pct(xs, 0.98f), w = hi - lo;
            if (w < 0.45f || w > 1.10f || std::abs(lo + hi) > 0.16f) continue;   // the width, centred on the driver
            glm::vec3 el{ 0.0f }, er{ 0.0f }, sl{ 0.0f }, sr{ 0.0f };
            int       nl = 0, nr = 0;
            for (const auto& q : Q) {
                if (q.x <= lo + 0.05f) el += q, nl++;
                if (q.x >= hi - 0.05f) er += q, nr++;
            }
            if (nl < 4 || nr < 4) continue;
            el /= float(nl);
            er /= float(nr);
            for (const auto& q : Q) {
                if (q.x <= lo + 0.05f) sl += (q - el) * (q - el);
                if (q.x >= hi - 0.05f) sr += (q - er) * (q - er);
            }
            sl = glm::sqrt(sl / float(nl));
            sr = glm::sqrt(sr / float(nr));
            // Each end a small blob (a grip, not a fender's edge), the two level with each other.
            const float blob  = std::max(std::max(sl.y, sl.z), std::max(sr.y, sr.z));
            const float level = std::max(std::abs(el.y - er.y), std::abs(el.z - er.z));
            if (blob > 0.05f || level > 0.08f) continue;
            Result r  = res;
            r.c       = 0.5f * (el + er);
            r.r       = 0.5f * glm::length(er - el) - 0.04f;   // the hands a little in from the ends
            r.part    = k;
            r.geo     = sat(1.0f - blob / 0.05f) * 0.5f + sat(1.0f - level / 0.08f) * 0.5f;
            r.spatial = place(r.c);
            // Both hands seen as on handlebars, and not on this part: against it. Hands seen any other way say nothing (pass 6, the jetski:
            // the hands given by the hand wear's draw are not where its arms' are; counted against the part, its bars missed 0.55 by 0.001).
            r.hands   = pair ? 0.0f : 0.5f;
            r.anim    = mo ? 1.0f : 0.5f;
            r.conf    = 0.30f * r.geo + 0.20f * r.spatial + 0.30f * r.hands + 0.20f * r.anim;
            r.ok      = r.r > 0.16f && r.spatial > 0.3f && r.conf >= 0.55f;
            r.how     = "a part with two grips either side of the driver";
            if (r.ok && (!geo.ok || r.conf > geo.conf)) geo = r;
        }
        return geo;
    }

    // The stick. The game's hand on it: the grip is where it holds, the bone the part under it (the helicopter, 11 Nov: the left hand on
    // the cyclic). Geometry: a thin part standing up ahead of the driver, 12 to 80 cm long, at least twice as long as wide (the cyclic:
    // 23 x 8 x 3 cm of spread, 0.9 upright; the hydroplane's yoke column: its top 20 cm left of the eyes, 17 cm under them); the grip is its
    // upper end. With a motion: the part that turns is the stick, whatever its shape, if it is big enough and its upper end within reach.
    inline Result detect_stick(const Cloud& cl, const Hands& h, const Motion* mo)
    {
        using namespace detail;
        Result res;
        res.style = kStick;
        res.how   = "no stick found";
        res.n     = glm::vec3{ 0.0f, 1.0f, 0.0f };
        auto in_box = [](const glm::vec3& p) { return p.x > -0.40f && p.x < 0.40f && p.y > -0.90f && p.y < -0.05f && p.z < -0.10f && p.z > -0.85f; };
        auto place  = [](const glm::vec3& t) { return band(std::abs(t.x), 0.0f, 0.12f, 0.25f) * band(glm::length(t), 0.25f, 0.75f, 0.15f) * band(t.y, -0.62f, -0.10f, 0.12f); };
        Result geo = res;
        {
            std::vector<std::vector<glm::vec3>> by(size_t(std::max(1, cl.nparts)));
            for (size_t i = 0; i < cl.p.size(); ++i)
                if (in_box(cl.p[i]) && part_in(mo, cl.part[i]) && cl.part[i] < by.size()) by[cl.part[i]].push_back(cl.p[i]);
            for (int k = 0; k < int(by.size()); ++k) {
                const auto& Q = by[size_t(k)];
                if (Q.size() < (mo ? 60u : 30u)) continue;
                glm::vec3 m, d;
                glm::mat3 C, V;
                mean_cov(Q, m, C);
                eig3(C, V, d);
                glm::vec3 a = glm::normalize(V[2]);   // the long axis
                if (a.y < 0.0f) a = -a;
                const float thick = std::sqrt(std::max(0.0f, d[1]));
                const float slim  = std::sqrt(d[2] / std::max(1e-8f, d[1]));
                std::vector<float> t(Q.size());
                for (size_t i = 0; i < Q.size(); ++i) t[i] = glm::dot(Q[i] - m, a);
                const float lo = pct(t, 0.02f), hi = pct(t, 0.98f), len = hi - lo;
                // Standing up, thin, over twice as long as wide; the part that turns: only long enough to hold.
                if (mo ? len < 0.10f || len > 0.90f : a.y < 0.55f || thick > 0.06f || slim < 2.2f || len < 0.12f || len > 0.80f) continue;
                glm::vec3 top{ 0.0f };
                int       nt = 0;
                for (size_t i = 0; i < Q.size(); ++i)
                    if (t[i] >= hi - 0.04f) top += Q[i], nt++;
                if (nt < 3) continue;
                top /= float(nt);
                // Where a hand holds it: on the stick's own line 4 to 10 cm under its upper end, not on top of its head (the helicopter's
                // cyclic, pass 6: the game's palm 9 cm under the top; the top was 8.5 cm from it, this is 5).
                if (!mo && len >= 0.16f) {
                    glm::vec3 hold{ 0.0f };
                    int       nh = 0;
                    for (size_t i = 0; i < Q.size(); ++i)
                        if (t[i] >= hi - 0.10f && t[i] <= hi - 0.04f) hold += Q[i], nh++;
                    if (nh >= 6) top = hold / float(nh);
                }
                Result r  = res;
                r.c       = top;
                r.n       = a;
                r.part    = k;
                r.geo     = sat((slim - 2.0f) / 2.0f) * 0.6f + 0.4f * sat(1.0f - thick / 0.06f);
                r.spatial = place(top);
                r.ok      = r.spatial > 0.3f;
                r.how     = mo ? "the part that turns with the stick" : "a thin part standing up ahead of the driver";
                if (r.ok && (!geo.ok || r.geo * r.spatial > geo.geo * geo.spatial)) geo = r;
            }
        }
        Result hr = res;
        {
            // The hand whose palm has the most of one part's vertices under it (7 cm): the hand on the stick.
            int    g = -1, gp = -1;
            size_t gn = 0;
            for (int s = 0; s < 2; ++s) {
                if (!h.ok[s] || !in_box(h.p[s])) continue;
                std::vector<uint32_t> nearby;
                for (size_t i = 0; i < cl.p.size(); ++i)
                    if (part_in(mo, cl.part[i]) && glm::dot(cl.p[i] - h.p[s], cl.p[i] - h.p[s]) < 0.07f * 0.07f) nearby.push_back(uint32_t(i));
                const int mp = major_part(cl, nearby);
                // (the geometry's stick under a hand wins over a bigger part under the other)
                const size_t n = mp >= 0 && geo.ok && mp == geo.part ? nearby.size() + 100000 : nearby.size();
                if (mp >= 0 && nearby.size() >= 5 && n > gn) g = s, gp = mp, gn = n;
            }
            if (g >= 0) {
                hr.c       = h.p[g];
                hr.part    = gp;
                hr.hands   = 1.0f;
                hr.spatial = place(hr.c);
                hr.ok      = hr.spatial > 0.3f;
                hr.how     = "where the game's hand holds";
            }
        }
        Result r = res;
        if (hr.ok && geo.ok && (hr.part == geo.part || glm::length(hr.c - geo.c) < 0.12f)) {
            r      = hr;   // the hand's own hold point on the geometry's stick
            r.part = geo.part;
            r.n    = geo.n;
            r.geo  = geo.geo;
            r.how  = "a stick ahead of the driver, the game's hand on it";
        } else if (geo.ok) {
            r       = geo;
            r.hands = h.ok[0] || h.ok[1] ? 0.25f : 0.5f;
        } else if (hr.ok) {
            r     = hr;
            r.geo = 0.3f;
        } else {
            return res;
        }
        r.anim = mo ? 1.0f : 0.5f;
        r.conf = 0.30f * r.geo + 0.20f * r.spatial + 0.30f * r.hands + 0.20f * r.anim;
        r.ok   = r.conf >= (mo ? 0.45f : 0.55f);
        return r;
    }

    inline Result detect(int style, const Cloud& cl, const Hands& h, const Motion* mo = nullptr)
    {
        return style == kWheel ? detect_wheel(cl, h, mo) : style == kBars ? detect_bars(cl, h, mo) : style == kStick ? detect_stick(cl, h, mo) : Result{};
    }
}
