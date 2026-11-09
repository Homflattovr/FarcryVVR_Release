// cullhead (always on since 9 Nov, the author's headset check): the engine's main-view culling camera posed from the headset. Notes in FarCry5CullHead.h.
//
// How the pose is made: at the main world camera's upload the mod knows the game's own view Vg and the view it gives the eye Ve, so
// Rel = Ve * inverse(Vg) is the head relative to the game camera (head offset and turn, Extended View included). When the game has
// copied this frame's camera into the culling camera (view V'), the head's view for culling is Rel * V': the player's own motion this
// frame is the game's (exact), only the head's motion since the last upload lags (one render call).
//
// Writing: View (+0x30), InvView (+0x70), World (+0xB0), ViewProj (+0x170, the multiplication order learnt from the game's own
// values), position (+0x1F0), and the two sets of six frustum planes (+0x23C and +0x2AC, run 5's cullheadslots) moved with the camera.
// The SceneCamera's matrices are stored for row vectors (x * View, ViewProj = View * Proj: run 5), i.e. transposed against the mod's
// constant-buffer matrices (load_rows, column vectors): load_cam / store_cam turn them into the same convention (run 5's head mode found
// no main view because of this: the camera position read as 0). Proj, near/far and fov stay the
// game's (LOD and size culling see the game's own lens); the side planes are only opened where an eye would see past them (widen).
// Written after the game's FC+0x43c3a0 (run 5: written before it, nothing changed).
#include "FarCry5CullHead.h"

#include <Windows.h>
#include <intrin.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <glm/gtc/matrix_transform.hpp>

namespace fc5::cullhead
{
namespace
{
    // SceneCamera offsets (runtime1.md / runtime4.md)
    constexpr size_t kView = 0x30, kInvView = 0x70, kWorld = 0xB0, kProj = 0xF0, kViewProj = 0x170, kPos = 0x1F0;
    constexpr size_t kNear = 0x22C, kFar = 0x230, kFov = 0x234;
    // Run 5 (testruns\2026-11-08_cullhead5\cullhead_lines.txt): six planes (n, d), near / far / four sides, at +0x23C, 16 bytes
    // apart; the same six with d negated at +0x2AC. Run 5b's dump reads as outward normals (the camera at +0.1 from the near plane,
    // inside n . x + d <= 0); the sign is read from the near plane at run time anyway (g_sign).
    constexpr size_t kPlanesA = 0x23C, kPlanesB = 0x2AC;
    constexpr int    kPlanes  = 6;
    // The block that copies the frame camera into the culling camera calls FC+0x43c3a0 from about FC+0x4ec6a5 (runtime4.md).
    constexpr uintptr_t kDeriveRva = 0x43c3a0, kCallerLo = 0x4ec000, kCallerHi = 0x4ed000;

    uint8_t*              g_base = nullptr;
    safetyhook::InlineHook g_hook{};

    std::atomic<int>   g_mode{ 1 };          // 1 head (always, author 9 Nov 08:27), 0 off (cullhead off, unsaved, for comparison)
    std::atomic<bool>  g_log{ false };
    std::atomic<bool>  g_active{ true };   // the eye's own view equal to the game's (flat screen, menus) gives Rel = identity anyway
    constexpr float    kMarginDeg = 3.0f;  // side planes opened to hold both eyes' frusta with this much to spare
    // The main view's culling camera (one address, rdi+0x20 of the copy block): once driven, it stays driven through a camera cut (more
    // than 20 deg or 3 m from the last upload, e.g. getting out of a vehicle), where the distance test alone handed one frame back to
    // the game's own culling camera (testruns\2026-11-09_cullhead_exit 00:11:53: 1 call at 13.7 deg / 4.82 m; author 9 Nov 08:27
    //).
    void*              g_main_cam = nullptr;

    struct Upload {
        glm::mat4 rel{ 1.0f };
        glm::mat4 game_view{ 1.0f };
        int       frame{ -1 };
        std::chrono::steady_clock::time_point t{};
        bool      valid{ false };
        // Per eye (frame & 1: AFW draws one eye a frame), for the margin metric and the widening: that eye's Rel and its
        // view-space edge directions (corners and edge midpoints of its projection).
        struct Eye {
            glm::mat4 rel{ 1.0f };
            glm::vec3 dirs[8]{};
            int       ndirs{ 0 };
            std::chrono::steady_clock::time_point t{};
        } eye[2];
    };
    std::mutex g_mtx;
    Upload     g_up{};

    // Per-second stats (render thread only)
    struct Stats {
        int calls = 0, from_block = 0, views = 0, driven = 0, skipped_stale = 0, skipped_far = 0, planes = 0, cuts = 0;
        float max_turn = 0.0f, max_move = 0.0f;
        float near_deg = -1.0f, near_m = 0.0f;   // the closest view left alone as "not the main view" (diagnosis)
        // Margin: the smallest angle (deg) by which the eyes' edge directions sit inside the culling camera's four side planes,
        // per main-view call; below 0 an eye sees past the culling. raw: before widening; final: what the game culls with.
        int   m_frames = 0, m_below_raw = 0, m_below = 0, widened = 0, eyes_seen = 0;
        float m_raw = 1e9f, m_final = 1e9f, widen_max = 0.0f;
        void* cams[8]{};
        int   cam_calls[8]{};
        uintptr_t rets[4]{};
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    } g_st;
    int g_vp_order = -1;   // 0: VP = P * V, 1: VP = V * P (column-vector matrices from load_cam); -1 not learnt yet
    int g_checked  = 0;    // 1: View's camera position agrees with the position field (+0x1F0); -1: it does not, nothing is written

    glm::mat4 load_rows(const float* p)
    {
        glm::mat4 m{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[c][r] = p[r * 4 + c];
        return m;
    }
    void store_rows(const glm::mat4& m, float* p)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) p[r * 4 + c] = m[c][r];
    }
    // The SceneCamera's own matrices (row vectors) as column-vector matrices like the mod's, and back.
    glm::mat4 load_cam(const float* p) { return glm::transpose(load_rows(p)); }
    void      store_cam(const glm::mat4& m, float* p) { store_rows(glm::transpose(m), p); }
    bool finite(const glm::mat4& m)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r])) return false;
        return true;
    }
    float diff(const glm::mat4& a, const glm::mat4& b)
    {
        float d = 0.0f;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) d = std::max(d, std::fabs(a[c][r] - b[c][r]));
        return d;
    }
    // Angle between two views' rotations (deg) and between their camera positions (m).
    void compare(const glm::mat4& va, const glm::mat4& vb, float& deg, float& m)
    {
        const glm::mat3 ra{ va }, rb{ vb };
        const glm::mat3 r  = ra * glm::transpose(rb);
        const float     tr = std::clamp((r[0][0] + r[1][1] + r[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
        deg = glm::degrees(std::acos(tr));
        const glm::vec3 pa = glm::vec3(glm::inverse(va)[3]), pb = glm::vec3(glm::inverse(vb)[3]);
        m = glm::length(pa - pb);
    }
    // The game's own ViewProj tells the multiplication order (learnt once).
    void learn_vp(uint8_t* cam)
    {
        if (g_vp_order >= 0) return;
        const glm::mat4 v  = load_cam(reinterpret_cast<float*>(cam + kView));
        const glm::mat4 p  = load_cam(reinterpret_cast<float*>(cam + kProj));
        const glm::mat4 vp = load_cam(reinterpret_cast<float*>(cam + kViewProj));
        const float     a = diff(vp, p * v), b = diff(vp, v * p);
        if (std::min(a, b) < 1.0e-3f * std::max(1.0f, std::fabs(vp[0][0]) + std::fabs(vp[1][1]))) {
            g_vp_order = a <= b ? 0 : 1;
            spdlog::info("[CullHead] ViewProj = {} (column vectors; differences {:.2g} / {:.2g})", g_vp_order == 0 ? "Proj * View" : "View * Proj", a, b);
        }
    }

    // Once: the View matrix read as column vectors must put the camera where the position field says, and the planes at +0x23C must
    // have the camera on the near plane's inside within near (and on the side planes); otherwise the layout is not what this
    // code assumes and nothing is written (logged).
    bool check_layout(uint8_t* cam)
    {
        if (g_checked != 0) return g_checked > 0;
        const float*    f   = reinterpret_cast<float*>(cam);
        const glm::vec3 pv  = glm::vec3(glm::inverse(load_cam(f + kView / 4))[3]);
        const glm::vec3 pf{ f[kPos / 4], f[kPos / 4 + 1], f[kPos / 4 + 2] };
        float side = 0.0f;
        for (int i = 2; i < kPlanes; ++i) {
            const float* q = f + (kPlanesA + 16 * i) / 4;
            side = std::max(side, std::fabs(q[0] * pf.x + q[1] * pf.y + q[2] * pf.z + q[3]));
        }
        bool b_mirror = true;
        for (int i = 0; i < kPlanes; ++i) {
            const float* a = f + (kPlanesA + 16 * i) / 4;
            const float* b = f + (kPlanesB + 16 * i) / 4;
            b_mirror = b_mirror && std::fabs(a[0] - b[0]) < 1e-4f && std::fabs(a[1] - b[1]) < 1e-4f && std::fabs(a[2] - b[2]) < 1e-4f &&
                       std::fabs(a[3] + b[3]) < 1e-2f * std::max(1.0f, std::fabs(a[3]));
        }
        const float off = glm::length(pv - pf);
        g_checked = (off < 0.5f && side < 0.5f) ? 1 : -1;
        spdlog::info("[CullHead] layout check {}: View's camera ({:.2f} {:.2f} {:.2f}) vs position field ({:.2f} {:.2f} {:.2f}), {:.2f} m; "
                     "side planes up to {:.2f} m from the camera; planes at +0x2AC {} the +0x23C set with d negated",
                     g_checked > 0 ? "passed" : "FAILED (nothing will be written)", pv.x, pv.y, pv.z, pf.x, pf.y, pf.z, off, side,
                     b_mirror ? "are" : "are NOT");
        return g_checked > 0;
    }

    // Puts the view vnew into the culling camera. Pose fields always; with move_planes the two plane sets are moved too.
    int repose(uint8_t* cam, const glm::mat4& vnew, bool move_planes)
    {
        float* f = reinterpret_cast<float*>(cam);
        const glm::mat4 vold = load_cam(f + kView / 4);
        const glm::mat4 inew = glm::inverse(vnew);
        const glm::mat4 M    = inew * vold;   // world to world: what sat at a place relative to the old camera, relative to the new one
        store_cam(vnew, f + kView / 4);
        store_cam(inew, f + kInvView / 4);
        store_cam(M * load_cam(f + kWorld / 4), f + kWorld / 4);
        if (g_vp_order >= 0) {
            const glm::mat4 p = load_cam(f + kProj / 4);
            store_cam(g_vp_order == 0 ? p * vnew : vnew * p, f + kViewProj / 4);
        }
        const glm::vec3 pos = glm::vec3(M * glm::vec4(f[kPos / 4], f[kPos / 4 + 1], f[kPos / 4 + 2], 1.0f));
        f[kPos / 4] = pos.x; f[kPos / 4 + 1] = pos.y; f[kPos / 4 + 2] = pos.z;
        int planes = 0;
        if (move_planes) {
            // A plane p (row) with p . (x, 1) = 0 for the points on it: under x' = M x it becomes p' = p * inverse(M).
            const glm::mat4 MiT = glm::transpose(glm::inverse(M));
            for (int i = 0; i < kPlanes; ++i) {
                float* a = f + (kPlanesA + 16 * i) / 4;
                float* b = f + (kPlanesB + 16 * i) / 4;
                const glm::vec4 q = MiT * glm::vec4{ a[0], a[1], a[2], a[3] };
                a[0] = q.x; a[1] = q.y; a[2] = q.z; a[3] = q.w;
                // the second set: the same plane with d negated (n . x - d' form)
                const glm::vec4 r = MiT * glm::vec4{ b[0], b[1], b[2], -b[3] };
                b[0] = r.x; b[1] = r.y; b[2] = r.z; b[3] = -r.w;
                planes += 2;
            }
        }
        return planes;
    }

    // The frustum's sign and forward, from the near plane: the camera sits outside it (near ahead of the camera).
    int g_sign = 0;   // +1 outward normals (inside n . x + d <= 0), -1 inward; 0 not read yet
    struct Frame {
        float*    f;
        glm::vec3 c, fwd;
        int       s;
    };
    bool read_frame(uint8_t* cam, Frame& fr)
    {
        fr.f = reinterpret_cast<float*>(cam);
        fr.c = glm::vec3(fr.f[kPos / 4], fr.f[kPos / 4 + 1], fr.f[kPos / 4 + 2]);
        const float* n0 = fr.f + kPlanesA / 4;
        const float  e0 = n0[0] * fr.c.x + n0[1] * fr.c.y + n0[2] * fr.c.z + n0[3];
        if (!std::isfinite(e0) || std::fabs(e0) < 1e-4f) return false;
        fr.s   = e0 > 0.0f ? 1 : -1;
        fr.fwd = -float(fr.s) * glm::normalize(glm::vec3(n0[0], n0[1], n0[2]));
        if (g_sign == 0) {
            g_sign = fr.s;
            const float* n1 = fr.f + (kPlanesA + 16) / 4;
            const float  ff = glm::dot(-float(fr.s) * glm::vec3(n1[0], n1[1], n1[2]), fr.fwd);
            spdlog::info("[CullHead] planes: {} normals (camera at {:+.3f} from the near plane); far plane's inward normal . forward {:+.3f} (want -1)",
                         fr.s > 0 ? "outward" : "inward", e0, ff);
        }
        return std::isfinite(fr.fwd.x);
    }
    glm::vec3 inward(const Frame& fr, int i)
    {
        const float* q = fr.f + (kPlanesA + 16 * i) / 4;
        return -float(fr.s) * glm::normalize(glm::vec3(q[0], q[1], q[2]));
    }
    // World edge directions of the fresh eyes, for this culling camera's game view vg (the eye's view is rel_k * vg).
    int eye_dirs(const Upload& up, const glm::mat4& vg, glm::vec3* out)
    {
        const auto now = std::chrono::steady_clock::now();
        int        n   = 0;
        for (int k = 0; k < 2; ++k) {
            const auto& e = up.eye[k];
            if (e.ndirs == 0 || now - e.t > std::chrono::milliseconds(250)) continue;
            const glm::mat3 r = glm::transpose(glm::mat3(e.rel * vg));   // eye to world rotation
            for (int j = 0; j < e.ndirs; ++j) out[n++] = r * e.dirs[j];
        }
        return n;
    }
    // Smallest angle (deg) of the directions inside the four side planes.
    float margin(const Frame& fr, const glm::vec3* d, int n)
    {
        float m = 1e9f;
        for (int i = 2; i < kPlanes; ++i) {
            const glm::vec3 w = inward(fr, i);
            for (int j = 0; j < n; ++j) m = std::min(m, glm::degrees(std::asin(std::clamp(glm::dot(w, d[j]), -1.0f, 1.0f))));
        }
        return m;
    }
    // Opens each side plane about the camera, turning it away from the forward direction, just enough to hold every direction
    // with margin_deg to spare (never narrower than the game's). Both sets, the camera's distance from each plane kept.
    int widen(const Frame& fr, const glm::vec3* d, int n, float margin_deg, float& opened)
    {
        int count = 0;
        for (int i = 2; i < kPlanes; ++i) {
            float*          a  = fr.f + (kPlanesA + 16 * i) / 4;
            float*          b  = fr.f + (kPlanesB + 16 * i) / 4;
            const glm::vec3 m  = inward(fr, i);
            const float     sa = std::clamp(glm::dot(m, fr.fwd), -1.0f, 1.0f);
            const float     al = std::asin(sa);
            glm::vec3       p  = m - sa * fr.fwd;
            if (glm::length(p) < 1e-4f) continue;
            p = glm::normalize(p);   // in the plane's normal, the part across the forward direction (towards the inside)
            float need = -1e9f;
            for (int j = 0; j < n; ++j) need = std::max(need, std::atan2(-glm::dot(p, d[j]), glm::dot(fr.fwd, d[j])));
            const float target = std::min(need + glm::radians(margin_deg), glm::radians(88.0f));
            if (!(target > al)) continue;
            const glm::vec3 m2 = p * std::cos(target) + fr.fwd * std::sin(target);
            const glm::vec3 n2 = -float(fr.s) * m2;
            const float     ec = a[0] * fr.c.x + a[1] * fr.c.y + a[2] * fr.c.z + a[3];   // the camera's distance from the plane, kept
            const float     d2 = ec - glm::dot(n2, fr.c);
            a[0] = n2.x; a[1] = n2.y; a[2] = n2.z; a[3] = d2;
            b[0] = n2.x; b[1] = n2.y; b[2] = n2.z; b[3] = -d2;
            opened = std::max(opened, glm::degrees(target - al));
            count++;
        }
        return count;
    }

    void tick_log()
    {
        auto&      s   = g_st;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.t0 < std::chrono::seconds(1)) return;
        if (g_log.load()) {
            std::string cams;
            for (int i = 0; i < 8 && s.cams[i]; ++i) cams += fmt::format(" {}x{}", s.cams[i], s.cam_calls[i]);
            std::string rets;
            for (int i = 0; i < 4 && s.rets[i]; ++i) rets += fmt::format(" +{:x}", s.rets[i] - reinterpret_cast<uintptr_t>(g_base));
            spdlog::info("[CullHead] last 1 s: mode {}{}, {} calls ({} from the copy block; callers{}), culling cameras{}; driven {}, stale {}, "
                         "not the main view {} (closest {:.1f} deg / {:.2f} m from the last main upload); head vs game culling camera up to {:.1f} deg / {:.2f} m; planes moved {}",
                         g_mode.load() == 1 ? "head" : "off", fmt::format(", {} camera cuts followed", s.cuts), s.calls, s.from_block, rets, cams, s.driven, s.skipped_stale, s.skipped_far,
                         s.near_deg, s.near_m, s.max_turn, s.max_move, s.planes);
            if (s.m_frames > 0)
                spdlog::info("[CullHead] margin (eyes inside the culling camera's side planes): {} main-view calls, {} eye frusta; worst {:+.1f} deg "
                             "({:+.1f} before widening), {} calls with an eye past the culling ({} before widening); planes opened {} times, up to {:.1f} deg",
                             s.m_frames, s.eyes_seen, s.m_final, s.m_raw, s.m_below, s.m_below_raw, s.widened, s.widen_max);
            else
                spdlog::info("[CullHead] margin: no main-view call with fresh eye frusta (on_main_upload without the eye projection?)");
        }
        s = Stats{};
        s.t0 = now;
    }

    uintptr_t hk_derive(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
    {
        const uintptr_t ret  = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
        const bool      ours = ret >= base + kCallerLo && ret < base + kCallerHi;
        const int       mode = g_mode.load();
        auto&           s    = g_st;
        s.calls++;
        if (!ours || (mode == 0 && !g_log.load()) || !g_active.load()) {
            const uintptr_t r = g_hook.call<uintptr_t>(a, b, c, d);
            if (ours) s.from_block++;
            tick_log();
            return r;
        }
        s.from_block++;
        for (int i = 0; i < 4; ++i) {
            if (s.rets[i] == ret) break;
            if (!s.rets[i]) { s.rets[i] = ret; break; }
        }
        uint8_t* cam = reinterpret_cast<uint8_t*>(a);
        for (int i = 0; i < 8; ++i) {
            if (s.cams[i] == cam) { s.cam_calls[i]++; break; }
            if (!s.cams[i]) { s.cams[i] = cam; s.cam_calls[i] = 1; s.views++; break; }
        }
        learn_vp(cam);
        if (!check_layout(cam)) {
            const uintptr_t r = g_hook.call<uintptr_t>(a, b, c, d);
            tick_log();
            return r;
        }

        // The pose wanted for this culling camera, from the copy the game just made (its View).
        const glm::mat4 vg = load_cam(reinterpret_cast<float*>(cam + kView));
        Upload          up;
        {
            std::lock_guard _{ g_mtx };
            up = g_up;
        }
        bool       ok = finite(vg);
        glm::mat4  vnew{ 1.0f };
        if (mode == 0) {
            // off, with cullheadlog on: only the margin of the eyes against the game's own culling planes (the main view only).
            const uintptr_t r = g_hook.call<uintptr_t>(a, b, c, d);
            float deg = 0.0f, m = 0.0f;
            const bool fresh = up.valid && std::chrono::steady_clock::now() - up.t < std::chrono::milliseconds(250);
            if (ok && fresh) compare(vg, up.game_view, deg, m);
            Frame     fr{};
            glm::vec3 dirs[16];
            const int nd = (ok && fresh && deg <= 20.0f && m <= 3.0f && read_frame(cam, fr)) ? eye_dirs(up, vg, dirs) : 0;
            if (nd > 0) {
                const float mg = margin(fr, dirs, nd);
                s.m_frames++;
                s.eyes_seen += nd / 8;
                s.m_raw   = std::min(s.m_raw, mg);
                s.m_final = std::min(s.m_final, mg);
                if (mg < 0.0f) { s.m_below_raw++; s.m_below++; }
            }
            tick_log();
            return r;
        }
        if (ok && mode == 1) {
            const bool fresh = up.valid && std::chrono::steady_clock::now() - up.t < std::chrono::milliseconds(250);
            float      deg = 0.0f, m = 0.0f;
            if (fresh) compare(vg, up.game_view, deg, m);
            if (!fresh) { s.skipped_stale++; ok = false; }
            else if ((deg > 20.0f || m > 3.0f) && cam != g_main_cam) {   // another view (shadow, reflection, secondary pass)
                s.skipped_far++;
                ok = false;
                if (s.near_deg < 0.0f || deg < s.near_deg) { s.near_deg = deg; s.near_m = m; }
            } else {
                // The main view; across a camera cut the eye keeps its place relative to the game camera (Rel), so Rel * V' is still the eye.
                if (deg > 20.0f || m > 3.0f) s.cuts++;
                g_main_cam = cam;
                vnew       = up.rel * vg;
            }
        }
        ok = ok && finite(vnew);
        uintptr_t r = 0;
        {
            r = g_hook.call<uintptr_t>(a, b, c, d);
            if (ok) {
                // After the game's own pass the copy may have moved on: re-read and move from there.
                s.planes += repose(cam, vnew, true);
                Frame     fr{};
                glm::vec3 dirs[16];
                const int nd = (mode == 1 && read_frame(cam, fr)) ? eye_dirs(up, vg, dirs) : 0;
                if (nd > 0) {
                    const float mg = margin(fr, dirs, nd);
                    float       mf = mg;
                    const int   w  = widen(fr, dirs, nd, kMarginDeg, s.widen_max);
                    if (w > 0) { s.widened += w; mf = margin(fr, dirs, nd); }
                    s.m_frames++;
                    s.eyes_seen += nd / 8;
                    s.m_raw   = std::min(s.m_raw, mg);
                    s.m_final = std::min(s.m_final, mf);
                    if (mg < 0.0f) s.m_below_raw++;
                    if (mf < 0.0f) s.m_below++;
                }
            }
        }
        if (ok) {
            s.driven++;
            float deg = 0.0f, m = 0.0f;
            compare(vnew, vg, deg, m);
            s.max_turn = std::max(s.max_turn, deg);
            s.max_move = std::max(s.max_move, m);
        }
        tick_log();
        return r;
    }
}

void install(uint8_t* base)
{
    g_base = base;   // the hook itself is made by the first cullhead command (nothing runs through it before)
}

namespace
{
    void ensure_hook()
    {
        if (g_hook || !g_base) return;
        // FC+0x43c3a0 is plain code (below +0x4108c00), called from the copy block. Its first bytes are logged so another game build shows.
        const uint8_t* fn = g_base + kDeriveRva;
        spdlog::info("[CullHead] FC+0x43c3a0 starts {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7]);
        g_hook = safetyhook::create_inline(g_base + kDeriveRva, reinterpret_cast<void*>(&hk_derive));
        spdlog::info("[CullHead] hook at +0x43c3a0: {}", (bool)g_hook);
    }
}

bool command(const std::string& cmd, const std::string& arg)
{
    // Only this module's own commands make the hook ("cullheads", the GPU culling switch, is another command).
    // Always on since 9 Nov (no F11 switch, nothing saved). Left for comparison and diagnosis: cullhead on|off and cullheadlog on|off.
    // A 'cullhead on' line still in an older settings file is read like the default.
    if (cmd == "cullhead") {
        g_mode = arg == "off" ? 0 : 1;
        spdlog::info("[CullHead] cullhead {}", g_mode.load() == 1 ? "on (the headset's pose)" : "off (the game's own culling camera, until cullhead on or a restart)");
        return true;
    }
    if (cmd == "cullheadlog") { ensure_hook(); g_log = arg != "off"; return true; }
    return false;
}

void apply_mode()
{
    if (g_mode.load() != 0) ensure_hook();
}

bool is_on() { return g_mode.load() == 1; }

void on_main_upload(const glm::mat4& game_view, const glm::mat4& eye_view, int frame, const glm::mat4& eye_proj)
{
    if (!finite(game_view) || !finite(eye_view)) return;
    // This eye's edge directions in its own view space: the corners and edge midpoints of its projection, unprojected at z 0.5.
    Upload::Eye e{};
    e.rel = eye_view * glm::inverse(game_view);
    e.t   = std::chrono::steady_clock::now();
    if (finite(eye_proj) && std::fabs(glm::determinant(eye_proj)) > 1e-12f) {
        const glm::mat4 ip = glm::inverse(eye_proj);
        static const float k[8][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 }, { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        for (const auto& q : k) {
            const glm::vec4 v = ip * glm::vec4(q[0], q[1], 0.5f, 1.0f);
            if (!(std::fabs(v.w) > 1e-12f)) continue;
            const glm::vec3 pt = glm::vec3(v) / v.w;
            const float     l  = glm::length(pt);
            if (std::isfinite(l) && l > 1e-6f) e.dirs[e.ndirs++] = pt / l;
        }
        if (e.ndirs != 8) e.ndirs = 0;
        static bool logged = false;
        if (!logged && e.ndirs == 8) {
            logged = true;
            const float h = glm::degrees(std::acos(std::clamp(glm::dot(e.dirs[7], e.dirs[5]), -1.0f, 1.0f)));
            const float v = glm::degrees(std::acos(std::clamp(glm::dot(e.dirs[4], e.dirs[6]), -1.0f, 1.0f)));
            spdlog::info("[CullHead] eye frustum (frame {}): {:.1f} deg across, {:.1f} deg high", frame, h, v);
        }
    }
    std::lock_guard _{ g_mtx };
    g_up.game_view    = game_view;
    g_up.rel          = e.rel;
    g_up.frame        = frame;
    g_up.t            = e.t;
    g_up.valid        = true;
    g_up.eye[frame & 1] = e;
}

void set_active(bool on) { g_active = on; }

std::string status()
{
    return fmt::format("cullhead {}, hook {}", g_mode.load() == 1 ? "on" : "off", (bool)g_hook);
}
}
