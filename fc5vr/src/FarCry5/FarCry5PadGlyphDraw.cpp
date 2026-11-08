// padglyphs: the VR controllers' button icons, drawn from shapes and stroke letters (antialiased by distance), and their BC3 compression.
// See FarCry5PadGlyphDraw.h. Design units: the game's 64 x 64 icon; a larger or wider texture scales by its smaller side and centres.
#include "FarCry5PadGlyphDraw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace fc5::padglyph
{
    namespace
    {
        struct V2
        {
            float x, y;
        };
        struct Seg
        {
            V2 a, b;
        };
        struct Col
        {
            float r, g, b;
        };
        constexpr Col kBlack{ 0.0f, 0.0f, 0.0f };
        constexpr Col kBody{ 61.0f, 61.0f, 61.0f };      // the dark grey of the game's icons
        constexpr Col kCap{ 84.0f, 84.0f, 84.0f };       // a stick's thumb cap
        constexpr Col kRing{ 200.0f, 200.0f, 200.0f };   // a face button's light rim (Touch buttons have no colours)
        constexpr Col kWhite{ 255.0f, 255.0f, 255.0f };
        constexpr Col kOrange{ 245.0f, 166.0f, 35.0f };  // Far Cry 5's UI orange (the game's own mouse icons): "left grip held" badges, stick arrows

        float len(V2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

        float seg_dist(V2 p, const Seg& s)
        {
            const V2    ab{ s.b.x - s.a.x, s.b.y - s.a.y }, ap{ p.x - s.a.x, p.y - s.a.y };
            const float l2 = ab.x * ab.x + ab.y * ab.y;
            const float t  = l2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
            return len(V2{ ap.x - ab.x * t, ap.y - ab.y * t });
        }

        // Stroke letters, in a box 1 high (y up, baseline 0), width given per letter.
        struct Letter
        {
            char             c;
            float            w;
            std::vector<Seg> s;
        };

        void arc(std::vector<Seg>& out, V2 c, float r, float a0_deg, float a1_deg, int n = 10)
        {
            V2 prev{ c.x + r * std::cos(a0_deg * 0.0174533f), c.y + r * std::sin(a0_deg * 0.0174533f) };
            for (int i = 1; i <= n; ++i) {
                const float a = (a0_deg + (a1_deg - a0_deg) * float(i) / float(n)) * 0.0174533f;
                const V2    p{ c.x + r * std::cos(a), c.y + r * std::sin(a) };
                out.push_back({ prev, p });
                prev = p;
            }
        }

        const Letter& letter(char c)
        {
            static const std::vector<Letter> kLetters = [] {
                std::vector<Letter> v;
                v.push_back({ 'A', 0.72f, { { { 0.0f, 0.0f }, { 0.36f, 1.0f } }, { { 0.36f, 1.0f }, { 0.72f, 0.0f } }, { { 0.14f, 0.36f }, { 0.58f, 0.36f } } } });
                {
                    Letter b{ 'B', 0.62f, { { { 0.0f, 0.0f }, { 0.0f, 1.0f } }, { { 0.0f, 1.0f }, { 0.36f, 1.0f } }, { { 0.0f, 0.53f }, { 0.38f, 0.53f } }, { { 0.0f, 0.0f }, { 0.38f, 0.0f } } } };
                    arc(b.s, { 0.36f, 0.765f }, 0.235f, 90.0f, -90.0f);
                    arc(b.s, { 0.38f, 0.265f }, 0.265f, 90.0f, -90.0f);
                    v.push_back(b);
                }
                v.push_back({ 'X', 0.68f, { { { 0.0f, 0.0f }, { 0.68f, 1.0f } }, { { 0.0f, 1.0f }, { 0.68f, 0.0f } } } });
                v.push_back({ 'Y', 0.70f, { { { 0.0f, 1.0f }, { 0.35f, 0.48f } }, { { 0.70f, 1.0f }, { 0.35f, 0.48f } }, { { 0.35f, 0.48f }, { 0.35f, 0.0f } } } });
                v.push_back({ 'L', 0.52f, { { { 0.0f, 1.0f }, { 0.0f, 0.0f } }, { { 0.0f, 0.0f }, { 0.52f, 0.0f } } } });
                {
                    Letter r{ 'R', 0.62f, { { { 0.0f, 0.0f }, { 0.0f, 1.0f } }, { { 0.0f, 1.0f }, { 0.34f, 1.0f } }, { { 0.0f, 0.46f }, { 0.34f, 0.46f } }, { { 0.30f, 0.46f }, { 0.62f, 0.0f } } } };
                    arc(r.s, { 0.34f, 0.73f }, 0.27f, 90.0f, -90.0f);
                    v.push_back(r);
                }
                {
                    Letter g{ 'G', 0.74f, { { { 0.74f, 0.20f }, { 0.74f, 0.44f } }, { { 0.74f, 0.44f }, { 0.42f, 0.44f } } } };
                    arc(g.s, { 0.40f, 0.5f }, 0.40f, 40.0f, 335.0f, 16);
                    v.push_back(g);
                }
                v.push_back({ 'T', 0.66f, { { { 0.0f, 1.0f }, { 0.66f, 1.0f } }, { { 0.33f, 1.0f }, { 0.33f, 0.0f } } } });
                {
                    Letter two{ '2', 0.62f, { { { 0.58f, 0.0f }, { 0.0f, 0.0f } }, { { 0.0f, 0.0f }, { 0.50f, 0.50f } } } };
                    arc(two.s, { 0.31f, 0.70f }, 0.28f, 165.0f, -42.0f, 12);
                    v.push_back(two);
                }
                return v;
            }();
            for (const auto& l : kLetters)
                if (l.c == c) return l;
            return kLetters[0];
        }

        // One icon being drawn: premultiplied float colour, painted over in order.
        struct Canvas
        {
            int                w, h;
            float              k;        // pixels per design unit
            float              ox, oy;   // pixel position of design (0, 0)
            std::vector<float> px;       // r g b a, premultiplied

            Canvas(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4, 0.0f)
            {
                k  = float(std::min(w, h)) / 64.0f;
                ox = (float(w) - 64.0f * k) * 0.5f;
                oy = (float(h) - 64.0f * k) * 0.5f;
            }

            // Paint colour where dist (design units, negative inside) says so, with one pixel of smoothing.
            template <typename F> void paint(F dist, Col c, float alpha = 1.0f)
            {
                const float pxu = 1.0f / k;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const V2    p{ (float(x) + 0.5f - ox) / k, (float(y) + 0.5f - oy) / k };
                        const float d   = dist(p);
                        const float cov = std::clamp(0.5f - d / pxu, 0.0f, 1.0f) * alpha;
                        if (cov <= 0.0f) continue;
                        float* q = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
                        q[0]     = c.r * cov + q[0] * (1.0f - cov);
                        q[1]     = c.g * cov + q[1] * (1.0f - cov);
                        q[2]     = c.b * cov + q[2] * (1.0f - cov);
                        q[3]     = cov + q[3] * (1.0f - cov);
                    }
            }

            void circle(V2 c, float r, Col col)
            {
                paint([&](V2 p) { return len(V2{ p.x - c.x, p.y - c.y }) - r; }, col);
            }

            void rrect(float x0, float y0, float x1, float y1, float r, Col col)
            {
                paint(
                    [&](V2 p) {
                        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
                        const float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
                        const float qx = std::abs(p.x - cx) - hx, qy = std::abs(p.y - cy) - hy;
                        return len(V2{ std::max(qx, 0.0f), std::max(qy, 0.0f) }) + std::min(std::max(qx, qy), 0.0f) - r;
                    },
                    col);
            }

            void strokes(const std::vector<Seg>& segs, float half_width, Col col)
            {
                paint(
                    [&](V2 p) {
                        float d = 1e9f;
                        for (const auto& s : segs) d = std::min(d, seg_dist(p, s));
                        return d - half_width;
                    },
                    col);
            }

            // A filled triangle (any winding), its edge pushed out by grow.
            void triangle(V2 a, V2 b, V2 c, Col col, float grow = 0.0f)
            {
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float sgn  = area < 0.0f ? -1.0f : 1.0f;
                paint(
                    [&](V2 p) {
                        auto edge = [&](V2 u, V2 v) {
                            const V2    e{ v.x - u.x, v.y - u.y };
                            const float l = len(e);
                            return -sgn * ((e.x * (p.y - u.y) - e.y * (p.x - u.x)) / l);
                        };
                        return std::max({ edge(a, b), edge(b, c), edge(c, a) }) - grow;
                    },
                    col);
            }

            // Text centred on (cx, cy), cap height ht (design units), stroke width sw.
            void text(const char* s, float cx, float cy, float ht, Col col, float sw = -1.0f)
            {
                if (sw < 0.0f) sw = ht * 0.19f;
                const float gap   = ht * 0.22f;
                float       total = 0.0f;
                int         n     = 0;
                for (const char* q = s; *q; ++q, ++n) total += letter(*q).w * ht + (n ? gap : 0.0f);
                std::vector<Seg> segs;
                float            x = cx - total * 0.5f;
                for (const char* q = s; *q; ++q) {
                    const Letter& l = letter(*q);
                    for (const auto& g : l.s)
                        segs.push_back({ { x + g.a.x * ht, cy + ht * 0.5f - g.a.y * ht }, { x + g.b.x * ht, cy + ht * 0.5f - g.b.y * ht } });
                    x += l.w * ht + gap;
                }
                strokes(segs, sw * 0.5f, col);
            }
        };

        // A round button: black edge, light rim, dark face.
        void face_disc(Canvas& cv, V2 c, float r)
        {
            cv.circle(c, r, kBlack);
            cv.circle(c, r - 3.0f, kRing);
            cv.circle(c, r - 5.5f, kBody);
        }

        // A face button's letter: Touch letters, or Index's A / B with the hand.
        void face_letter(Canvas& cv, Icon i, Letters l, V2 c, float r)
        {
            char        ch   = 'A';
            const char* hand = "R";
            switch (i) {
            case Icon::FaceA: ch = 'A'; hand = "R"; break;
            case Icon::FaceB:
            case Icon::LGripB: ch = 'B'; hand = "R"; break;
            case Icon::FaceX:
            case Icon::FaceX2: ch = l == Letters::Index ? 'A' : 'X'; hand = "L"; break;
            case Icon::FaceY: ch = l == Letters::Index ? 'B' : 'Y'; hand = "L"; break;
            default: break;
            }
            const char s[2]{ ch, 0 };
            if (l == Letters::Index) {
                cv.text(s, c.x - r * 0.12f, c.y, r * 0.88f, kWhite);
                cv.text(hand, c.x + r * 0.52f, c.y + r * 0.40f, r * 0.36f, kRing);
            } else {
                cv.text(s, c.x, c.y, r * 0.95f, kWhite);
            }
        }

        // "LG": the left grip held, a small orange tag in a corner.
        void lg_badge(Canvas& cv, bool bottom)
        {
            const float y0 = bottom ? 40.0f : 0.5f;
            cv.rrect(0.5f, y0, 34.0f, y0 + 23.5f, 7.0f, kBlack);
            cv.rrect(2.5f, y0 + 2.0f, 32.0f, y0 + 21.5f, 5.0f, kOrange);
            cv.text("LG", 17.25f, y0 + 11.75f, 12.5f, kBlack, 2.9f);
        }

        // A stick: black edge, dark base, a lighter thumb cap; letter on the cap (none for "either stick").
        void stick(Canvas& cv, V2 c, float r, const char* label)
        {
            cv.circle(c, r, kBlack);
            cv.circle(c, r - 3.0f, kBody);
            cv.circle(c, r * 0.68f, kRing);
            cv.circle(c, r * 0.68f - 2.5f, kCap);
            if (label && *label) cv.text(label, c.x, c.y, r * 0.62f, kWhite);
        }

        // An orange arrow pointing out of the stick (dir 0 up, 1 down, 2 left, 3 right) at the icon's edge.
        void arrow(Canvas& cv, int dir, V2 c, float reach, float size)
        {
            const V2 d = dir == 0 ? V2{ 0.0f, -1.0f } : dir == 1 ? V2{ 0.0f, 1.0f } : dir == 2 ? V2{ -1.0f, 0.0f } : V2{ 1.0f, 0.0f };
            const V2 n{ -d.y, d.x };
            const V2 tip{ c.x + d.x * reach, c.y + d.y * reach };
            const V2 base{ tip.x - d.x * size, tip.y - d.y * size };
            const V2 a{ base.x + n.x * size * 0.75f, base.y + n.y * size * 0.75f }, b{ base.x - n.x * size * 0.75f, base.y - n.y * size * 0.75f };
            cv.triangle(tip, a, b, kBlack, 2.0f);
            cv.triangle(tip, a, b, kOrange);
        }

        void menu_disc(Canvas& cv, V2 c, float r)
        {
            face_disc(cv, c, r);
            std::vector<Seg> bars;
            const float      hw = r * 0.42f, dy = r * 0.30f;
            for (int i = -1; i <= 1; ++i) bars.push_back({ { c.x - hw, c.y + dy * float(i) }, { c.x + hw, c.y + dy * float(i) } });
            cv.strokes(bars, r * 0.085f, kWhite);
        }
    }

    const char* icon_name(Icon i)
    {
        static const char* const kNames[]{ "none", "A", "B", "X", "Y", "X twice", "left trigger", "right trigger", "left grip", "right grip",
                                           "left grip + B", "menu", "left grip + menu", "left stick", "right stick", "left stick press",
                                           "right stick press", "left stick up", "left stick down", "left stick left", "left stick right",
                                           "left grip + stick", "left grip + stick up", "left grip + stick down", "left grip + stick left",
                                           "left grip + stick right" };
        const int k = int(i);
        return k >= 0 && k < int(Icon::Count) ? kNames[k] : "?";
    }

    void render(Icon icon, Letters letters, int w, int h, uint8_t* rgba)
    {
        Canvas cv(w, h);
        switch (icon) {
        case Icon::FaceA:
        case Icon::FaceB:
        case Icon::FaceX:
        case Icon::FaceY:
            face_disc(cv, { 32.0f, 32.0f }, 28.0f);
            face_letter(cv, icon, letters, { 32.0f, 32.0f }, 28.0f);
            break;
        case Icon::FaceX2:
            face_disc(cv, { 27.0f, 35.0f }, 26.0f);
            face_letter(cv, icon, letters, { 27.0f, 35.0f }, 26.0f);
            cv.circle({ 50.0f, 13.5f }, 13.0f, kBlack);
            cv.circle({ 50.0f, 13.5f }, 10.5f, kOrange);
            cv.text("2", 50.0f, 13.5f, 12.0f, kBlack, 2.9f);
            break;
        case Icon::TrigL:
        case Icon::TrigR:
            // Tall: the index finger's trigger.
            cv.rrect(11.0f, 2.5f, 53.0f, 61.5f, 13.0f, kBlack);
            cv.rrect(14.0f, 5.5f, 50.0f, 58.5f, 10.0f, kBody);
            cv.text(icon == Icon::TrigL ? "LT" : "RT", 32.0f, 32.0f, 18.0f, kWhite);
            break;
        case Icon::GripL:
        case Icon::GripR:
            // Wide, with a ridge: the grip under the middle finger.
            cv.rrect(2.5f, 12.0f, 61.5f, 52.0f, 11.0f, kBlack);
            cv.rrect(5.5f, 15.0f, 58.5f, 49.0f, 8.0f, kBody);
            cv.strokes({ { { 12.0f, 44.0f }, { 52.0f, 44.0f } } }, 1.2f, kRing);
            cv.text(icon == Icon::GripL ? "LG" : "RG", 32.0f, 29.0f, 18.0f, kWhite);
            break;
        case Icon::LGripB:
            face_disc(cv, { 37.0f, 37.0f }, 26.0f);
            face_letter(cv, Icon::FaceB, letters, { 37.0f, 37.0f }, 26.0f);
            lg_badge(cv, false);
            break;
        case Icon::Menu: menu_disc(cv, { 32.0f, 32.0f }, 28.0f); break;
        case Icon::LGripMenu:
            menu_disc(cv, { 37.0f, 37.0f }, 26.0f);
            lg_badge(cv, false);
            break;
        case Icon::StickL:
        case Icon::StickR: stick(cv, { 32.0f, 32.0f }, 28.0f, icon == Icon::StickL ? "L" : "R"); break;
        case Icon::StickLPress:
        case Icon::StickRPress:
            stick(cv, { 32.0f, 35.0f }, 26.5f, icon == Icon::StickLPress ? "L" : "R");
            // Pressed: the game's own sign, a triangle pointing down onto the stick.
            cv.triangle({ 32.0f, 12.0f }, { 22.0f, 1.0f }, { 42.0f, 1.0f }, kBlack, 1.5f);
            cv.triangle({ 32.0f, 12.0f }, { 22.0f, 1.0f }, { 42.0f, 1.0f }, kWhite);
            break;
        case Icon::StickLUp:
        case Icon::StickLDown:
        case Icon::StickLLeft:
        case Icon::StickLRight: {
            const int dir = int(icon) - int(Icon::StickLUp);
            stick(cv, { 32.0f, 32.0f }, 23.0f, "L");
            arrow(cv, dir, { 32.0f, 32.0f }, 31.0f, 10.0f);
            break;
        }
        case Icon::LGripStick: {
            stick(cv, { 38.0f, 38.0f }, 19.0f, "");
            for (int d = 0; d < 4; ++d) arrow(cv, d, { 38.0f, 38.0f }, 25.5f, 7.0f);
            lg_badge(cv, false);
            break;
        }
        case Icon::LGripUp:
        case Icon::LGripDown:
        case Icon::LGripLeft:
        case Icon::LGripRight: {
            const int  dir    = int(icon) - int(Icon::LGripUp);
            const bool bottom = dir == 0;   // the up arrow takes the top: the tag goes under
            const V2   c{ 38.0f, bottom ? 27.0f : 38.0f };
            stick(cv, c, 19.0f, "");
            arrow(cv, dir, c, 25.5f, 8.5f);
            lg_badge(cv, bottom);
            break;
        }
        default: break;
        }
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
            const float* q = &cv.px[i * 4];
            const float  a = q[3];
            uint8_t*     o = rgba + i * 4;
            if (a <= 0.0f) {
                o[0] = o[1] = o[2] = 0;
                o[3]               = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(q[c] / a + 0.5f, 0.0f, 255.0f));
            o[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    namespace
    {
        uint16_t to565(const float c[3])
        {
            const int r = std::clamp(int(c[0] * 31.0f / 255.0f + 0.5f), 0, 31);
            const int g = std::clamp(int(c[1] * 63.0f / 255.0f + 0.5f), 0, 63);
            const int b = std::clamp(int(c[2] * 31.0f / 255.0f + 0.5f), 0, 31);
            return uint16_t((r << 11) | (g << 5) | b);
        }
        void from565(uint16_t v, float c[3])
        {
            const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            c[0]        = float((r << 3) | (r >> 2));
            c[1]        = float((g << 2) | (g >> 4));
            c[2]        = float((b << 3) | (b >> 2));
        }

        void alpha_block(const uint8_t a[16], uint8_t* out)
        {
            uint8_t lo = 255, hi = 0;
            for (int i = 0; i < 16; ++i) {
                lo = std::min(lo, a[i]);
                hi = std::max(hi, a[i]);
            }
            out[0] = hi;
            out[1] = lo;
            uint64_t bits = 0;
            if (hi != lo) {
                // 8 values: hi, lo, then six steps from hi to lo.
                int pal[8]{ hi, lo };
                for (int k = 1; k <= 6; ++k) pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
                for (int i = 0; i < 16; ++i) {
                    int best = 0, bd = 1 << 30;
                    for (int k = 0; k < 8; ++k) {
                        const int d = std::abs(pal[k] - a[i]);
                        if (d < bd) { bd = d; best = k; }
                    }
                    bits |= uint64_t(best) << (3 * i);
                }
            }
            for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
        }

        void color_block(const float c[16][3], const float w[16], uint8_t* out)
        {
            // The colours' main direction (weighted by alpha), the block's two extremes along it as the end points.
            float mean[3]{}, wsum = 0.0f;
            for (int i = 0; i < 16; ++i) {
                for (int k = 0; k < 3; ++k) mean[k] += c[i][k] * w[i];
                wsum += w[i];
            }
            if (wsum <= 0.0f) {
                std::memset(out, 0, 8);
                return;
            }
            for (float& m : mean) m /= wsum;
            float cov[6]{};
            for (int i = 0; i < 16; ++i) {
                const float d[3]{ c[i][0] - mean[0], c[i][1] - mean[1], c[i][2] - mean[2] };
                cov[0] += w[i] * d[0] * d[0];
                cov[1] += w[i] * d[0] * d[1];
                cov[2] += w[i] * d[0] * d[2];
                cov[3] += w[i] * d[1] * d[1];
                cov[4] += w[i] * d[1] * d[2];
                cov[5] += w[i] * d[2] * d[2];
            }
            float axis[3]{ 0.577f, 0.577f, 0.577f };
            for (int it = 0; it < 8; ++it) {
                const float n[3]{ cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                                  cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l < 1e-6f) break;
                for (int k = 0; k < 3; ++k) axis[k] = n[k] / l;
            }
            float tmin = 1e9f, tmax = -1e9f;
            for (int i = 0; i < 16; ++i) {
                if (w[i] <= 0.0f) continue;
                const float t = (c[i][0] - mean[0]) * axis[0] + (c[i][1] - mean[1]) * axis[1] + (c[i][2] - mean[2]) * axis[2];
                tmin          = std::min(tmin, t);
                tmax          = std::max(tmax, t);
            }
            float e0[3], e1[3];
            for (int k = 0; k < 3; ++k) {
                e0[k] = mean[k] + axis[k] * tmax;
                e1[k] = mean[k] + axis[k] * tmin;
            }
            uint16_t c0 = to565(e0), c1 = to565(e1);
            if (c0 < c1) std::swap(c0, c1);
            float p[4][3];
            from565(c0, p[0]);
            from565(c1, p[1]);
            for (int k = 0; k < 3; ++k) {
                p[2][k] = (2.0f * p[0][k] + p[1][k]) / 3.0f;
                p[3][k] = (p[0][k] + 2.0f * p[1][k]) / 3.0f;
            }
            uint32_t idx = 0;
            if (c0 != c1) {
                for (int i = 0; i < 16; ++i) {
                    int   best = 0;
                    float bd   = 1e30f;
                    for (int k = 0; k < 4; ++k) {
                        const float d = (p[k][0] - c[i][0]) * (p[k][0] - c[i][0]) + (p[k][1] - c[i][1]) * (p[k][1] - c[i][1]) + (p[k][2] - c[i][2]) * (p[k][2] - c[i][2]);
                        if (d < bd) { bd = d; best = k; }
                    }
                    idx |= uint32_t(best) << (2 * i);
                }
            }
            out[0] = uint8_t(c0);
            out[1] = uint8_t(c0 >> 8);
            out[2] = uint8_t(c1);
            out[3] = uint8_t(c1 >> 8);
            for (int k = 0; k < 4; ++k) out[4 + k] = uint8_t(idx >> (8 * k));
        }
    }

    void encode_bc3(const uint8_t* rgba, int w, int h, uint8_t* out)
    {
        for (int by = 0; by < h / 4; ++by)
            for (int bx = 0; bx < w / 4; ++bx) {
                uint8_t a[16];
                float   c[16][3], wt[16];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const uint8_t* p = rgba + (size_t(by * 4 + y) * size_t(w) + size_t(bx * 4 + x)) * 4;
                        const int      i = y * 4 + x;
                        a[i]             = p[3];
                        for (int k = 0; k < 3; ++k) c[i][k] = float(p[k]);
                        wt[i] = float(p[3]) / 255.0f;
                    }
                uint8_t* blk = out + (size_t(by) * size_t(w / 4) + size_t(bx)) * 16;
                alpha_block(a, blk);
                color_block(c, wt, blk + 8);
            }
    }

    uint64_t hash_rows(const void* data, int row_bytes, int rows, int row_pitch)
    {
        uint64_t       hsh = 1469598103934665603ull;
        const uint8_t* p   = static_cast<const uint8_t*>(data);
        for (int r = 0; r < rows; ++r) {
            const uint8_t* q = p + size_t(r) * size_t(row_pitch);
            for (int i = 0; i < row_bytes; ++i) {
                hsh ^= q[i];
                hsh *= 1099511628211ull;
            }
        }
        return hsh;
    }
}
