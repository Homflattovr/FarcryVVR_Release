// cullhead (always on since 9 Nov, the author's headset check): the engine's main-view culling camera posed from the headset. Notes in FarCry5CullHead.h.
//
// How the pose is made: at the main world camera's upload the mod knows the game's own view Vg and the view it gives the eye Ve, so
// Rel = Ve * inverse(Vg) is the head relative to the game camera (head offset and turn, Extended View included). When the game has
// copied this frame's camera into the culling camera (view V'), the head's view for culling is Rel * V': the player's own motion this
// frame is the game's (exact), only the head's motion since the last upload lags (one render call).
//
// Pass 2 (11 Nov, author): with motion controls the eye's rotation is the mod's own
// (body frame and headset), not the game camera's, and the game camera swings by itself (its aim goes to the barrel when a weapon is
// taken or fired, comes back, recoils). Rel * V' then turns the culling with that swing for one frame though the eye did not turn.
// Which of the two holds this frame is not known at culling time (a snap turn and a vehicle do turn the eye with the game camera), so
// the side planes are opened to hold the eyes both ways: as Rel * V' says, and as the eye's own world rotation at its last upload says,
// and that rotation carried on by the head's measured turn (each eye is drawn every other frame, so its last upload is two frames old).
// A snap turn (snapsync) turns the eyes at the first upload whose game camera holds half the turn, with pairbody only on a pair's
// first frame: when the camera landed on a pair's second frame, Rel * V' turned the culling a whole snap ahead of the eyes for one
// frame and left it a whole snap behind them on the next. While the snap's camera turn is landing, both the eye before the turn and
// after it are held, from a culling camera posed half way.
// Pass 3 (11 Nov, author): the mod itself turns the view in
// one step there (the snaps that follow the entry and exit animations, the settle cut onto the vehicle's front, the recentre once
// seated), between two frames, after the eye's last upload. No prediction from the uploads can know such a step, and the culling of the
// first frame after it was a whole step off (vehicle dump runs of 11 Nov with pass 2: 1 to 4 frames an entry, 10 to 85 deg outside).
// The mod now says so when it makes the step (view_turned): every way the eye may stand is turned by it, and the culling camera too.
// With motion controls the eye's rotation is the mod's own, so when the game camera stands over 70 deg from where the eye stood
// (a vehicle's look-behind flipping sides), the culling camera takes the eye's own rotation, not the half way pose.
// At each upload the eye really drawn is checked against the planes the game culled with ("true margin"); a line is logged in any
// second where an eye was outside them, and a summary every 30 s.
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

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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
    std::atomic<bool>  g_pass2{ true };      // cullheadpass2 off (unsaved, for comparison): the planes as the 9-11 Nov builds made them (Rel * V' alone)
    // cullnarrow on|off (saved, F11, default off; author 16 Nov): the side
    // planes fitted to every way both eyes may stand (the same predictions that open them) plus kMarginDeg, also where that is narrower
    // than the game's own lens (116-120 deg across on foot), instead of only opened past it. Proj, LOD and size culling stay the game's.
    std::atomic<bool>  g_narrow{ false };
    std::atomic<bool>  g_active{ true };   // the eye's own view equal to the game's (flat screen, menus) gives Rel = identity anyway
    constexpr float    kMarginDeg  = 3.0f;   // side planes opened to hold both eyes' frusta with this much to spare
    constexpr float    kLeadOver   = 1.5f;   // the head's measured turn is carried on to the drawn frame and this much further (it may speed up)
    constexpr float    kJumpDeg    = 12.0f;  // a turn over this much a frame is a jump (snap turn, recentre), not a head turn: not carried on
    constexpr float    kSnapPart   = 0.4f;   // a snap turn is landing once the game camera holds this part of it (the eyes turn at half)
    constexpr float    kMidDeg     = 15.0f;  // the game camera swung this far against the eye: the culling camera stands half way between
    constexpr float    kFarDeg     = 70.0f;  // past this no frustum holds both: with the eye's rotation the mod's own, the eye as it stood
    constexpr auto     kOtherViewAge = std::chrono::milliseconds(250);   // an upload this old says nothing about an unknown view
    constexpr auto     kMainViewAge  = std::chrono::milliseconds(1000);  // the main view keeps the last eye through a hitch
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
        bool      own{ false };   // the eye's rotation is the mod's own (motion controls: body frame and headset), not the game camera's
        // Per eye (frame & 1: AFW draws one eye a frame), for the margin metric and the widening: that eye's Rel and its
        // view-space edge directions (corners and edge midpoints of its projection).
        struct Eye {
            glm::mat4 rel{ 1.0f };
            glm::mat3 world{ 1.0f };     // eye to world rotation at this upload (the mod's own, whatever the game camera does)
            float     rate_deg{ 0.0f };  // how far that rotation turned per frame since this eye's upload before (head and body),
            float     turn_deg{ 0.0f };  // in all,
            glm::vec3 turn_axis{ 0.0f, 0.0f, 1.0f };   // about this world axis (the head is one rigid thing: the same turn for both eyes)
            bool      mirrored{ false }; // not this eye's own upload yet: the other eye's pose with its edges mirrored (the first frame)
            int       frame{ -1 };
            glm::vec3 dirs[8]{};
            int       ndirs{ 0 };
            std::chrono::steady_clock::time_point t{};
        } eye[2];
    };
    // A snap turn sent to the game and not yet on the eyes (pending_turn): the body yaw to come (rad, about world Z, positive to
    // the left), the game camera's yaw at the send and the frame of the send.
    struct Snap {
        float pend{ 0.0f }, game0{ 0.0f };
        int   frame0{ 0 };
    };
    std::mutex g_mtx;
    Upload     g_up{};
    Snap       g_snap{};
    // The turns the mod made of the view since each eye's last upload (view_turned; rad about world Z, positive to the left).
    float      g_turned[2]{};

    // The side planes the game culls the main view with this frame (inward normals, world), as the hook left them, with how they were
    // made; and the check of each drawn eye against them at its upload. Both under g_mtx.
    enum { kDriven = 0, kStale = 1, kNoEye = 2, kOff = 3, kBlind = 4 };
    // The flat screen has just gone (flat_screen(false)): until the first eye upload (one frame) nothing is known of the head, so the
    // game's own culling camera keeps its pose and its side planes are opened as far as they go.
    std::chrono::steady_clock::time_point g_blind_until{};   // under g_mtx
    struct Planes {
        glm::vec3 n[4]{};
        bool      valid{ false };
        int       how{ kDriven };
        float     disagree{ 0.0f }, lead{ 0.0f }, opened{ 0.0f }, age_ms{ 0.0f };
        bool      snap{ false };
        float     turned{ 0.0f };   // the mod's own turn of the view carried into these planes (deg)
        float     narrowed{ 0.0f }; // cullnarrow: the most a side plane was closed in from the game's lens (deg)
        int       eye{ -1 };        // the eye expected at this frame's draw ((last upload + 1) & 1), for the parity count
        std::chrono::steady_clock::time_point t{};
    } g_planes;
    struct True {
        int   frames = 0, below = 0, below2 = 0, not_driven = 0, widened = 0, snaps = 0, turns = 0;
        float worst = 1e9f, disagree_max = 0.0f, lead_max = 0.0f, opened_max = 0.0f;
        // cullnarrow: frames narrowed and by how much; the culling frustum's width across and high (sum, deg, driven frames); which eye was
        // drawn against the one expected (a later per-eye fit needs that right every frame)
        int   narrowed = 0, w_n = 0, eye_next = 0, eye_other = 0;
        float narrowed_max = 0.0f, across_sum = 0.0f, high_sum = 0.0f;
        // the worst frame's own facts
        int   w_how = kDriven, w_frame = -1;
        bool  w_snap = false;
        float w_turned = 0.0f;
        float w_disagree = 0.0f, w_lead = 0.0f, w_opened = 0.0f, w_age = 0.0f, w_rate = 0.0f;
        void add(const True& o)
        {
            frames += o.frames; below += o.below; below2 += o.below2; not_driven += o.not_driven; widened += o.widened; snaps += o.snaps; turns += o.turns;
            narrowed += o.narrowed; w_n += o.w_n; eye_next += o.eye_next; eye_other += o.eye_other;
            narrowed_max = std::max(narrowed_max, o.narrowed_max); across_sum += o.across_sum; high_sum += o.high_sum;
            disagree_max = std::max(disagree_max, o.disagree_max); lead_max = std::max(lead_max, o.lead_max); opened_max = std::max(opened_max, o.opened_max);
            if (o.worst < worst) { worst = o.worst; w_how = o.w_how; w_snap = o.w_snap; w_turned = o.w_turned; w_frame = o.w_frame; w_disagree = o.w_disagree; w_lead = o.w_lead; w_opened = o.w_opened; w_age = o.w_age; w_rate = o.w_rate; }
        }
    } g_true;

    // Per-second stats (render thread only)
    struct Stats {
        int calls = 0, from_block = 0, views = 0, driven = 0, skipped_stale = 0, skipped_far = 0, planes = 0, cuts = 0, no_eye = 0, old_used = 0, snaps = 0, blind = 0, mids = 0, turns = 0;
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
    constexpr int kMaxDirs = 64;   // two eyes, eight directions, four ways
    // World edge directions of the fresh eyes for this culling camera's game view vg, each way the eye may stand at this frame's draw:
    //  - as the eye's view rel_k * vg (it turned with the game camera: n_rel directions);
    //  - as its own world rotation at its last upload (it did not);
    //  - that rotation carried on by the head's measured turn, for the frames since that eye's upload and kLeadOver more;
    //  - with snap_rad not 0 (a snap turn landing), the last two turned by the snap about world Z.
    // disagree: the largest angle between the first two (deg); lead: the largest turn carried on (deg).
    // turned: the mod's own turns of the view since each eye's upload (view_turned), applied to every way of that eye.
    int eye_dirs(const Upload& up, const glm::mat4& vg, glm::vec3* out, int& n_rel, float& disagree, float& lead, float snap_rad = 0.0f, const float* turned = nullptr)
    {
        static const float kNone[2]{};
        if (!turned) turned = kNone;
        const auto rz = [](float rad) { return glm::mat3(glm::rotate(glm::mat4(1.0f), rad, glm::vec3(0.0f, 0.0f, 1.0f))); };
        glm::mat3  world[2]{ up.eye[0].world, up.eye[1].world };
        const auto now = std::chrono::steady_clock::now();
        int        n   = 0;
        disagree = lead = 0.0f;
        glm::mat3 ra[2]{};
        bool      have[2]{};
        for (int k = 0; k < 2; ++k) {
            const auto& e = up.eye[k];
            if (e.ndirs == 0 || now - e.t > kMainViewAge) continue;
            have[k] = true;
            ra[k]   = glm::transpose(glm::mat3(e.rel * vg));   // eye to world rotation
            if (turned[k] != 0.0f) {
                ra[k]    = rz(turned[k]) * ra[k];
                world[k] = rz(turned[k]) * world[k];
            }
            for (int j = 0; j < e.ndirs; ++j) out[n++] = ra[k] * e.dirs[j];
        }
        n_rel = n;
        // The head's turn, as the latest upload measured it against that eye's upload before (world: both eyes ride the same head).
        const int   lk      = up.frame & 1;
        const auto& last    = up.eye[lk];
        const bool  turning = have[lk] && last.rate_deg > 0.02f && last.rate_deg <= kJumpDeg;
        const bool  jumped  = have[lk] && last.rate_deg > kJumpDeg;
        const int   n_plain = n;
        for (int k = 0; k < 2; ++k) {
            if (!have[k]) continue;
            const auto&     e  = up.eye[k];
            const glm::mat3 d  = glm::transpose(ra[k]) * world[k];
            const float     tr = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
            disagree = std::max(disagree, glm::degrees(std::acos(tr)));
            for (int j = 0; j < e.ndirs; ++j) out[n++] = world[k] * e.dirs[j];
            float deg = 0.0f;
            if (turning) {
                // This eye is drawn next at frame up.frame + 1 at the earliest: that many frames after its own upload.
                deg  = std::min(last.rate_deg * float(std::clamp(up.frame + 1 - e.frame, 1, 4)) * kLeadOver, 3.0f * kJumpDeg);
                lead = std::max(lead, deg);
            } else if (jumped && k != lk && e.frame < last.frame && e.rate_deg <= kJumpDeg && turned[k] == 0.0f) {
                // The view jumped (a snap turn, a recentre) at the latest upload and this eye's last upload was before it.
                deg = last.turn_deg;
            }
            if (deg != 0.0f) {
                const glm::mat3 w = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(deg), last.turn_axis)) * world[k];
                for (int j = 0; j < e.ndirs; ++j) out[n++] = w * e.dirs[j];
            }
        }
        if (snap_rad != 0.0f) {
            const glm::mat3 rs = rz(snap_rad);
            const int       n0 = n;
            for (int j = n_plain; j < n0 && n < kMaxDirs; ++j) out[n++] = rs * out[j];
        }
        return n;
    }
    // The planes the game is left with for the main view, for the check at the next upload.
    void note_planes(uint8_t* cam, int how, float disagree, float lead, float opened, float age_ms, bool snap = false, float turned = 0.0f, float narrowed = 0.0f, int eye = -1)
    {
        Frame  fr{};
        Planes p{};
        if (read_frame(cam, fr)) {
            for (int i = 0; i < 4; ++i) p.n[i] = inward(fr, 2 + i);
            p.valid = true;
        }
        p.how = how; p.disagree = disagree; p.lead = lead; p.opened = opened; p.age_ms = age_ms; p.snap = snap; p.turned = turned; p.narrowed = narrowed; p.eye = eye;
        p.t = std::chrono::steady_clock::now();
        std::lock_guard _{ g_mtx };
        g_planes = p;
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
    // all: every side plane as far open as it goes (88 deg from the forward direction), whatever the directions.
    // fit (cullnarrow): each side plane set to hold the directions with margin_deg to spare, also where that is narrower than the
    // game's; narrowed: the most a plane was closed in (deg).
    int widen(const Frame& fr, const glm::vec3* d, int n, float margin_deg, float& opened, bool all = false, bool fit = false, float* narrowed = nullptr)
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
            const float target = all ? glm::radians(88.0f) : std::min(need + glm::radians(margin_deg), glm::radians(88.0f));
            if (fit && !all && n > 0 && std::isfinite(target) && target < al - 1e-4f) {
                if (narrowed) *narrowed = std::max(*narrowed, glm::degrees(al - target));
            } else if (!(target > al)) {
                continue;
            }
            const glm::vec3 m2 = p * std::cos(target) + fr.fwd * std::sin(target);
            const glm::vec3 n2 = -float(fr.s) * m2;
            const float     ec = a[0] * fr.c.x + a[1] * fr.c.y + a[2] * fr.c.z + a[3];   // the camera's distance from the plane, kept
            const float     d2 = ec - glm::dot(n2, fr.c);
            a[0] = n2.x; a[1] = n2.y; a[2] = n2.z; a[3] = d2;
            b[0] = n2.x; b[1] = n2.y; b[2] = n2.z; b[3] = -d2;
            if (target > al) opened = std::max(opened, glm::degrees(target - al));
            count++;
        }
        return count;
    }

    const char* how_name(int how)
    {
        return how == kDriven ? "posed from the eye" : how == kStale ? "the game's own (no eye upload for over 1 s)" : how == kNoEye ? "the game's own (no eye pose)" : how == kBlind ? "the game's own, opened wide (the first frame after the flat screen)" : "the game's own (cullhead off)";
    }
    True g_sum{};                                    // the last 30 s (render thread only)
    std::chrono::steady_clock::time_point g_sum_t0 = std::chrono::steady_clock::now();
    void tick_log()
    {
        auto&      s   = g_st;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.t0 < std::chrono::seconds(1)) return;
        True tr;
        {
            std::lock_guard _{ g_mtx };
            tr     = g_true;
            g_true = True{};
        }
        g_sum.add(tr);
        // Always (no switch): any second in which a drawn eye reached past the planes the game culled with.
        if (tr.below > 0 || (g_log.load() && tr.frames > 0))
            spdlog::info("[CullHead] true margin, last 1 s: an eye was outside the culling planes in {} of {} frames ({} by over 2 deg), worst {:+.1f} deg at frame {}: planes {}, "
                         "{}{}the game camera had swung {:.1f} deg against the eye, the eye turned {:.1f} deg a frame ({:.1f} deg carried on), planes opened {:.1f} deg, eye upload {:.0f} ms old; "
                         "this second: swings up to {:.1f} deg, planes opened in {} frames (up to {:.1f} deg), {} frames with a snap turn landing, {} after a turn of the view by the mod, {} frames left to the game",
                         tr.below, tr.frames, tr.below2, tr.worst, tr.w_frame, how_name(tr.w_how), tr.w_snap ? "a snap turn landing, " : "",
                         tr.w_turned != 0.0f ? fmt::format("the mod had turned the view {:+.1f} deg, ", tr.w_turned) : std::string{}, tr.w_disagree, tr.w_rate, tr.w_lead, tr.w_opened, tr.w_age,
                         tr.disagree_max, tr.widened, tr.opened_max, tr.snaps, tr.turns, tr.not_driven);
        if (now - g_sum_t0 >= std::chrono::seconds(30)) {
            if (g_sum.frames > 0)
                spdlog::info("[CullHead] last 30 s: {} drawn eyes checked against the culling planes, {} outside ({} by over 2 deg), worst {:+.1f} deg; "
                             "the game camera swung up to {:.1f} deg against the eye, planes opened in {} frames (up to {:.1f} deg; head turn carried on up to {:.1f} deg), "
                             "{} frames with a snap turn landing, {} after a turn of the view by the mod; {} frames left to the game's own culling; "
                             "culling frustum on average {:.1f} deg across, {:.1f} deg high; cullnarrow {}{}; the eye drawn was the one expected in {} frames, the other in {}",
                             g_sum.frames, g_sum.below, g_sum.below2, g_sum.worst, g_sum.disagree_max, g_sum.widened, g_sum.opened_max, g_sum.lead_max, g_sum.snaps, g_sum.turns, g_sum.not_driven,
                             g_sum.w_n ? g_sum.across_sum / g_sum.w_n : 0.0f, g_sum.w_n ? g_sum.high_sum / g_sum.w_n : 0.0f, g_narrow.load() ? "on" : "off",
                             g_narrow.load() ? fmt::format(" (planes closed in from the game's lens in {} frames, up to {:.1f} deg)", g_sum.narrowed, g_sum.narrowed_max) : std::string{},
                             g_sum.eye_next, g_sum.eye_other);
            g_sum    = True{};
            g_sum_t0 = now;
        }
        if (g_log.load()) {
            std::string cams;
            for (int i = 0; i < 8 && s.cams[i]; ++i) cams += fmt::format(" {}x{}", s.cams[i], s.cam_calls[i]);
            std::string rets;
            for (int i = 0; i < 4 && s.rets[i]; ++i) rets += fmt::format(" +{:x}", s.rets[i] - reinterpret_cast<uintptr_t>(g_base));
            spdlog::info("[CullHead] last 1 s: mode {}{}, {} calls ({} from the copy block; callers{}), culling cameras{}; driven {} ({} on an eye upload over 250 ms old), stale {}, "
                         "flat screen {} ({} opened wide after it), snap turn landing {}, posed half way on a camera swing {}, turned with the mod's own turn of the view {}, not the main view {} (closest {:.1f} deg / {:.2f} m from the last main upload); head vs game culling camera up to {:.1f} deg / {:.2f} m; planes moved {}",
                         g_mode.load() == 1 ? "head" : "off", fmt::format(", {} camera cuts followed", s.cuts), s.calls, s.from_block, rets, cams, s.driven, s.old_used, s.skipped_stale,
                         s.no_eye, s.blind, s.snaps, s.mids, s.turns, s.skipped_far, s.near_deg, s.near_m, s.max_turn, s.max_move, s.planes);
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
        const bool      pass2 = g_pass2.load();
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
        Snap            snap;
        bool            blind = false;
        float           turned[2]{};
        {
            std::lock_guard _{ g_mtx };
            up    = g_up;
            snap  = g_snap;
            if (pass2) { turned[0] = g_turned[0]; turned[1] = g_turned[1]; }
            blind = !up.valid && std::chrono::steady_clock::now() < g_blind_until;
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
            glm::vec3 dirs[kMaxDirs];
            int       nrel = 0;
            float     dis = 0.0f, lead0 = 0.0f;
            const bool main0 = ok && fresh && ((deg <= 20.0f && m <= 3.0f) || cam == g_main_cam);
            const int  nd    = (main0 && read_frame(cam, fr)) ? eye_dirs(up, vg, dirs, nrel, dis, lead0) : 0;
            if (main0) note_planes(cam, kOff, dis, 0.0f, 0.0f, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - up.t).count());
            if (nd > 0) {
                const float mg = margin(fr, dirs, nrel);
                s.m_frames++;
                s.eyes_seen += nrel / 8;
                s.m_raw   = std::min(s.m_raw, mg);
                s.m_final = std::min(s.m_final, mg);
                if (mg < 0.0f) { s.m_below_raw++; s.m_below++; }
            }
            tick_log();
            return r;
        }
        const auto  age    = std::chrono::steady_clock::now() - up.t;
        const float age_ms = std::chrono::duration<float, std::milli>(age).count();
        const bool  main_v = cam == g_main_cam;
        int         how    = kDriven;
        float       snap_rad = 0.0f;
        if (ok && mode == 1) {
            float deg = 0.0f, m = 0.0f;
            if (up.valid) compare(vg, up.game_view, deg, m);
            if (!up.valid) {   // the flat screen (menus, the map): the game's own camera is what is shown (no_eye)
                s.no_eye++;
                ok  = false;
                how = kNoEye;
            } else if (age > (main_v && pass2 ? kMainViewAge : kOtherViewAge)) {
                s.skipped_stale++;
                ok  = false;
                how = kStale;
            } else if ((deg > 20.0f || m > 3.0f) && !main_v) {   // another view (shadow, reflection, secondary pass)
                s.skipped_far++;
                ok = false;
                if (s.near_deg < 0.0f || deg < s.near_deg) { s.near_deg = deg; s.near_m = m; }
            } else {
                // The main view; across a camera cut the eye keeps its place relative to the game camera (Rel), so Rel * V' is still the eye.
                if (deg > 20.0f || m > 3.0f) s.cuts++;
                if (age > kOtherViewAge) s.old_used++;   // a hitch: the last eye is still the best there is
                g_main_cam = cam;
                vnew       = up.rel * vg;
                // A snap turn whose camera turn is landing (the game camera, row 2 of its view = back, holds kSnapPart of it): the
                // eyes turn by the whole snap at this upload or the next. The culling camera stands half way between the eye before
                // and after (its place kept), so that the planes can hold both.
                if (pass2 && snap.pend != 0.0f && std::fabs(snap.pend) < glm::radians(120.0f)) {
                    const glm::vec3 fwd{ -vg[0][2], -vg[1][2], -vg[2][2] };
                    const float     moved = std::remainder(std::atan2(-fwd.x, fwd.y) - snap.game0, glm::two_pi<float>());
                    const auto&     le    = up.eye[up.frame & 1];
                    const int       waited = up.frame + 1 - snap.frame0;   // at this frame's upload; over 12 the eyes turn without the camera
                    if ((moved * (snap.pend > 0.0f ? 1.0f : -1.0f) >= kSnapPart * std::fabs(snap.pend) || waited > 10 || waited < 0) && le.ndirs == 8) {
                        snap_rad = snap.pend;
                        const glm::mat3 w   = glm::mat3(glm::rotate(glm::mat4(1.0f), 0.5f * snap.pend, glm::vec3(0.0f, 0.0f, 1.0f))) * le.world;   // eye to world, half turned
                        const glm::vec3 pos = glm::vec3(glm::inverse(vnew)[3]);
                        glm::mat4       v{ glm::transpose(w) };
                        v[3] = glm::vec4(-(glm::transpose(w) * pos), 1.0f);
                        vnew = v;
                        s.snaps++;
                    }
                }
                // The game camera swung far against the eye: a side plane opens to 88 deg from the culling camera's forward at most, so
                // from the Rel * V' pose the eye where it stood can't be held past about 35 deg (11 Nov follow-up run, 20:12:00: a
                // 39.7 deg swing at a weapon switch, planes opened 30.0 deg, the eye 5.7 deg outside). The culling camera stands half
                // way between the eye as Rel * V' has it and as its last upload had it (its place kept); the planes hold both.
                if (const auto& le = up.eye[up.frame & 1]; pass2 && snap_rad == 0.0f && le.ndirs == 8 && !le.mirrored) {
                    const glm::quat qa = glm::quat_cast(glm::transpose(glm::mat3(vnew))), qb = glm::quat_cast(le.world);   // eye to world, both ways
                    const float     apart = glm::degrees(2.0f * std::acos(std::clamp(std::fabs(glm::dot(qa, qb)), 0.0f, 1.0f)));
                    if (apart > kFarDeg && !up.own) {
                        // Past kFarDeg no frustum holds both. The eye's rotation is the game camera's: Rel * V' as it is.
                    } else if (apart > kMidDeg) {
                        // Past kFarDeg, with the eye's rotation the mod's own: the eye as it stood (the game camera's own swing is not the
                        // eye's: vehicle dump run, 17:05:05, a plane's look-behind flipping sides 120 deg a frame for 21 frames).
                        const glm::mat3 w   = apart > kFarDeg ? le.world : glm::mat3_cast(glm::normalize(glm::slerp(qa, qb, 0.5f)));
                        const glm::vec3 pos = glm::vec3(glm::inverse(vnew)[3]);
                        glm::mat4       v{ glm::transpose(w) };
                        v[3] = glm::vec4(-(glm::transpose(w) * pos), 1.0f);
                        vnew = v;
                        s.mids++;
                    }
                }
                // The mod turned the view since the latest eye's upload (view_turned): the culling camera turns with it, about its own place.
                if (const float t = turned[up.frame & 1]; t != 0.0f) {
                    const glm::mat3 w   = glm::mat3(glm::rotate(glm::mat4(1.0f), t, glm::vec3(0.0f, 0.0f, 1.0f))) * glm::transpose(glm::mat3(vnew));
                    const glm::vec3 pos = glm::vec3(glm::inverse(vnew)[3]);
                    glm::mat4       v{ glm::transpose(w) };
                    v[3] = glm::vec4(-(glm::transpose(w) * pos), 1.0f);
                    vnew = v;
                    s.turns++;
                }
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
                glm::vec3 dirs[kMaxDirs];
                int       nrel = 0;
                float     dis = 0.0f, opened = 0.0f, lead = 0.0f, narrowed = 0.0f;
                int       nd = (mode == 1 && read_frame(cam, fr)) ? eye_dirs(up, vg, dirs, nrel, dis, lead, snap_rad, turned) : 0;
                if (!pass2) { nd = nrel; lead = 0.0f; }   // comparison: only the eyes as Rel * V' has them
                // cullnarrow: only with both eyes known (eight directions each), never on one eye alone.
                const bool narrow = pass2 && g_narrow.load() && nrel >= 16;
                if (nd > 0) {
                    // Before: the planes as moved with the camera (what the 8-10 Nov builds opened for Rel * V' alone). After: every way held.
                    const float mg = margin(fr, dirs, nd);
                    float       mf = mg;
                    const int   w  = widen(fr, dirs, nd, kMarginDeg, opened, false, narrow, &narrowed);
                    if (w > 0) { s.widened += w; mf = margin(fr, dirs, nd); }
                    s.widen_max = std::max(s.widen_max, opened);
                    s.m_frames++;
                    s.eyes_seen += nrel / 8;
                    s.m_raw   = std::min(s.m_raw, mg);
                    s.m_final = std::min(s.m_final, mf);
                    if (mg < 0.0f) s.m_below_raw++;
                    if (mf < 0.0f) s.m_below++;
                }
                note_planes(cam, kDriven, dis, lead, opened, age_ms, snap_rad != 0.0f, glm::degrees(turned[(up.frame + 1) & 1]), narrowed, up.valid ? ((up.frame + 1) & 1) : -1);
            } else if (mode == 1 && main_v && how != kDriven) {
                float opened = 0.0f;
                Frame fr{};
                if (how == kNoEye && blind && pass2 && read_frame(cam, fr)) {
                    widen(fr, nullptr, 0, 0.0f, opened, true);
                    how = kBlind;
                    s.blind++;
                }
                note_planes(cam, how, 0.0f, 0.0f, opened, age_ms);   // the main view left to the game this frame
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
    // Always on since 9 Nov (no F11 switch, nothing saved). Left for comparison and diagnosis: cullhead on|off, cullheadpass2 on|off and cullheadlog on|off.
    // A 'cullhead on' line still in an older settings file is read like the default.
    if (cmd == "cullhead") {
        g_mode = arg == "off" ? 0 : 1;
        spdlog::info("[CullHead] cullhead {}", g_mode.load() == 1 ? "on (the headset's pose)" : "off (the game's own culling camera, until cullhead on or a restart)");
        return true;
    }
    if (cmd == "cullheadlog") { ensure_hook(); g_log = arg != "off"; return true; }
    if (cmd == "cullnarrow") {
        g_narrow = arg == "on" || arg == "1" || arg == "true";
        spdlog::info("[CullHead] cullnarrow {}", g_narrow.load() ? "on (the side planes fitted to both eyes plus 3 deg, narrower than the game's lens where it is wider)" : "off (the game's lens, opened where an eye reaches past it)");
        return true;
    }
    if (cmd == "cullheadpass2") {
        g_pass2 = arg != "off";
        spdlog::info("[CullHead] cullheadpass2 {}", g_pass2.load() ? "on (the default)" : "off (the planes as the 9-11 Nov builds made them, until cullheadpass2 on or a restart)");
        return true;
    }
    return false;
}

void apply_mode()
{
    if (g_mode.load() != 0) ensure_hook();
}

bool is_on() { return g_mode.load() == 1; }

bool narrow_on() { return g_narrow.load(); }

void on_main_upload(const glm::mat4& game_view, const glm::mat4& eye_view, int frame, const glm::mat4& eye_proj, bool own_rotation)
{
    if (!finite(game_view) || !finite(eye_view)) return;
    // This eye's edge directions in its own view space: the corners and edge midpoints of its projection, unprojected at z 0.5.
    Upload::Eye e{};
    e.rel   = eye_view * glm::inverse(game_view);
    e.world = glm::transpose(glm::mat3(eye_view));
    e.frame = frame;
    e.t     = std::chrono::steady_clock::now();
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
    // How far this eye's own rotation turned per frame since its upload before (two frames back in AFW): the head's and the body's turn.
    if (const auto& o = g_up.eye[frame & 1]; o.ndirs == 8 && !o.mirrored && frame > o.frame && frame - o.frame <= 4 && e.t - o.t < kOtherViewAge) {
        // e.world = d * o.world: the turn in world space, less the turns the mod made itself in between (view_turned): the head's own.
        const glm::mat3 d  = glm::mat3(glm::rotate(glm::mat4(1.0f), -g_turned[frame & 1], glm::vec3(0.0f, 0.0f, 1.0f))) * e.world * glm::transpose(o.world);
        const float     tr = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
        const glm::vec3 ax{ d[1][2] - d[2][1], d[2][0] - d[0][2], d[0][1] - d[1][0] };   // 2 sin(angle) * axis
        const float     l  = glm::length(ax);
        if (l > 1e-6f) {
            e.turn_deg  = glm::degrees(std::acos(tr));
            e.rate_deg  = e.turn_deg / float(frame - o.frame);
            e.turn_axis = ax / l;
        }
    }
    // The true margin: this eye, as it is drawn now, against the side planes the game culled this frame with (the hook's last call for
    // the main view, since the upload before).
    if (g_planes.valid && e.ndirs == 8 && g_planes.t > g_up.t && e.t - g_planes.t < kOtherViewAge) {
        float tm = 1e9f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) tm = std::min(tm, glm::degrees(std::asin(std::clamp(glm::dot(g_planes.n[i], e.world * e.dirs[j]), -1.0f, 1.0f))));
        auto& t = g_true;
        t.frames++;
        if (tm < 0.0f) t.below++;
        if (tm < -2.0f) t.below2++;
        if (g_planes.how != kDriven) t.not_driven++;
        if (g_planes.opened > 0.0f) t.widened++;
        if (g_planes.snap) t.snaps++;
        if (g_planes.turned != 0.0f) t.turns++;
        if (g_planes.narrowed > 0.0f) t.narrowed++;
        t.narrowed_max = std::max(t.narrowed_max, g_planes.narrowed);
        if (g_planes.eye >= 0) (g_planes.eye == (frame & 1) ? t.eye_next : t.eye_other)++;
        if (g_planes.how == kDriven) {
            // The culling frustum's width: 180 deg less the angle between the inward normals of each pair of side planes.
            auto apart = [](const glm::vec3& a, const glm::vec3& b) { return 180.0f - glm::degrees(std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f))); };
            const float w01 = apart(g_planes.n[0], g_planes.n[1]), w23 = apart(g_planes.n[2], g_planes.n[3]);
            // The pair nearer the horizontal is "across" (the game's lens is wider than high).
            const bool first_across = std::fabs(g_planes.n[0].z) + std::fabs(g_planes.n[1].z) < std::fabs(g_planes.n[2].z) + std::fabs(g_planes.n[3].z);
            t.across_sum += first_across ? w01 : w23;
            t.high_sum += first_across ? w23 : w01;
            t.w_n++;
        }
        t.disagree_max = std::max(t.disagree_max, g_planes.disagree);
        t.lead_max     = std::max(t.lead_max, g_planes.lead);
        t.opened_max   = std::max(t.opened_max, g_planes.opened);
        if (tm < t.worst) {
            t.worst = tm; t.w_how = g_planes.how; t.w_snap = g_planes.snap; t.w_turned = g_planes.turned; t.w_frame = frame; t.w_disagree = g_planes.disagree; t.w_lead = g_planes.lead; t.w_opened = g_planes.opened;
            t.w_age = g_planes.age_ms; t.w_rate = e.rate_deg;
        }
    }
    g_up.game_view    = game_view;
    g_up.rel          = e.rel;
    g_up.frame        = frame;
    g_up.t            = e.t;
    g_up.valid        = true;
    g_up.own          = own_rotation;
    g_up.eye[frame & 1] = e;
    g_turned[frame & 1] = 0.0f;   // this upload holds every turn so far
    // The other eye not seen yet (the first frame, or the first after the flat screen): this eye's pose with its edges mirrored
    // left to right, until its own upload a frame later.
    if (auto& other = g_up.eye[(frame & 1) ^ 1]; g_pass2.load() && e.ndirs == 8 && (other.ndirs == 0 || other.mirrored)) {
        other          = e;
        other.mirrored = true;
        g_turned[(frame & 1) ^ 1] = 0.0f;
        other.rate_deg = other.turn_deg = 0.0f;
        for (auto& v : other.dirs) v.x = -v.x;
    }
}

void no_eye()
{
    if (!g_pass2.load()) return;
    std::lock_guard _{ g_mtx };
    g_turned[0] = g_turned[1] = 0.0f;
    if (!g_up.valid) return;
    g_up.valid = false;
    g_up.eye[0].ndirs = g_up.eye[1].ndirs = 0;
}

void flat_screen(bool on)
{
    if (on) { no_eye(); return; }
    std::lock_guard _{ g_mtx };
    g_blind_until = std::chrono::steady_clock::now() + kOtherViewAge;
}

void view_turned(float yaw_rad, bool body_frame)
{
    if (!std::isfinite(yaw_rad) || yaw_rad == 0.0f || !g_pass2.load()) return;
    std::lock_guard _{ g_mtx };
    if (!g_up.valid) return;   // the flat screen: the next upload starts afresh
    if (body_frame && !g_up.own) return;   // without motion controls the eye hangs on the game camera, not on the body frame
    for (float& t : g_turned) t = std::remainder(t + yaw_rad, glm::two_pi<float>());
}

void pending_turn(float yaw_rad, float game_yaw0, int frame0)
{
    std::lock_guard _{ g_mtx };
    g_snap.pend   = std::isfinite(yaw_rad) && std::isfinite(game_yaw0) ? yaw_rad : 0.0f;
    g_snap.game0  = game_yaw0;
    g_snap.frame0 = frame0;
}

void set_active(bool on) { g_active = on; }

std::string status()
{
    return fmt::format("cullhead {}, hook {}", g_mode.load() == 1 ? "on" : "off", (bool)g_hook);
}
}
