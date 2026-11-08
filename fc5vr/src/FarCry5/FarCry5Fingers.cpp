// fingernames (see FarCry5Fingers.h): the hands' fingers posed by their bones' names.
//
// Why (author 2 Nov 14:37 pictures, F11 Debug >, grip fully closed): the shape-based pose (hand_fingers,
// 19-20 Oct) finds the fingers from the mesh's shape. On the author's bare arms (Body_FPSArms*, 69/70 slots, about 30 of them skin
// helpers) his 2 Nov log has "37 fingers, 14 index, 3 thumb" per hand: the index has 13 bones, the thumb 9. So six thumb bones
// bent with the other fingers, one or more middle-finger bones stayed straight with the index, and the closed hand was the game's
// weapon grip relative to the palm (turned up to 1.4 times further on the less closed hand): fingers half straight, skin pulled
// between bones of one finger that bent differently.
//
// Now: the bone map names every slot, so each bone has its finger and joint. The rest pose of the game's hands is a flat hand
// (every joint within 5 deg of straight, the fingers fanned 18 deg (index) to -20 deg (little)). Each finger is bent at its three
// joints about its own axis (across the finger, towards the palm), by its curl times a full bend per joint; the bend replaces the
// rest pose's. The full bends and the curls from the controller are Chasm's VR framework's (crates/avatar/src/rig.rs CURL,
// crates/ik/src/fingers.rs; MIT, Copyright (c) 2026 chasmlol): index 1.22, 1.66, 1.05 rad .. little 1.54, 1.66, 1.05; the
// fingers close together as they curl (the rest pose's fan taken out). The thumb was fitted here on the game's skeleton so that in
// a fist it lies across the middle of the index and middle fingers without passing through them: its base folds 0.5 rad across
// the palm (between the palm and the little finger) and turns 0.3 rad out first, its two knuckles bend 0.3 and 1.05 rad across.
// Helpers: one along a bone rides that bone; one on a joint (the knuckles helpers, the thumb helper) takes half its bend; the
// metacarpals stay on the palm. The same bone gets the same pose in every draw (gloves, bare arms, arm hair), so they cannot part.
//
// Curls (0 straight .. 1 a fist; the thumb below 0 stands up): the grip closes the middle, ring and little fingers (relaxed
// 0.12 open); the index points with the grip, rests a little bent on the trigger, closes with the trigger (grip + trigger: a fist,
// as fingergrip has since 19 Oct); the thumb on a button, the thumbrest or the stick lies relaxed and folds over the fingers with
// the grip, lifted off it stands up (thumbs up). fingerfist scales the four fingers' full bends (default 1).
#include "FarCry5Fingers.h"
#include "FarCry5BoneNames.h"
#include "ModPath.h"
#include "FarCry5ReloadFingers.h"
#include "FarCry5Reload.h"

#include <imgui.h>
#include <spdlog/spdlog.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fc5::fingers
{
namespace
{
    std::atomic<bool>  g_on{ true };
    std::atomic<bool>  g_holds{ true };   // fingerholds
    std::atomic<bool>  g_game{ false };   // fingergame
    std::atomic<float> g_fist{ 1.0f };
    std::atomic<bool>  g_log{ false };
    std::atomic<bool>  g_reset{ false };
    std::atomic<int>   g_known{ 0 }, g_unknown{ 0 };

    constexpr const char* kFinger[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
    // Full bend (rad at curl 1) of each finger's three joints (Chasm's CURL); the thumb's are fitted below.
    constexpr float kCurl[5][3] = { { 0.0f, 0.0f, 0.0f }, { 1.22f, 1.66f, 1.05f }, { 1.36f, 1.75f, 1.13f }, { 1.43f, 1.75f, 1.08f }, { 1.54f, 1.66f, 1.05f } };
    constexpr float kThumbFold = 0.5f, kThumbOut = 0.3f, kThumbK1 = 0.3f, kThumbK2 = 1.05f;
    // How much of the rest pose's fan the fingers lose in a fist.
    constexpr float kClose = 0.8f;

    glm::vec3 nrm(const glm::vec3& v)
    {
        const float l = glm::length(v);
        return l > 1e-9f ? v / l : glm::vec3{ 0.0f, 0.0f, 1.0f };
    }
    // Signed angle from u to v about axis (both measured across the axis).
    float sangle(glm::vec3 u, glm::vec3 v, const glm::vec3& ax)
    {
        u -= ax * glm::dot(u, ax);
        v -= ax * glm::dot(v, ax);
        return std::atan2(glm::dot(ax, glm::cross(u, v)), glm::dot(u, v));
    }
    float seg_dist(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
    {
        const glm::vec3 d  = b - a;
        const float     dd = glm::dot(d, d);
        const float     t  = dd > 1e-12f ? std::clamp(glm::dot(p - a, d) / dd, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + t * d));
    }

    // A joint's rest position (m) by bone name, from the first draw that skins to it (every first-person hand shares the skeleton).
    bool joint_by_name(const std::string& name, glm::vec3& out)
    {
        using namespace fc5::bonenames;
        for (int d = 0; d < kDrawN; ++d) {
            for (int s = 0; s < kDraws[d].nbones; ++s) {
                const uint32_t i = kDraws[d].first + uint32_t(s);
                if (name != kName[kSlot[i]]) continue;
                out = glm::vec3{ float(kJoint[i][0]), float(kJoint[i][1]), float(kJoint[i][2]) } * 0.001f;
                return true;
            }
        }
        return false;
    }

    // One hand's skeleton (rest pose, m) and its axes.
    struct Finger
    {
        glm::vec3 base{};       // the metacarpal's root (InHand*; the thumb: the palm's joint)
        glm::vec3 P[4]{};       // the three joints and the tip (the last bone's length x 0.8 beyond the third)
        glm::vec3 A{};          // bend axis: positive bends towards the palm (the thumb: across it)
        glm::vec3 A2{};         // the thumb's knuckles' axis
        float     bind[3]{};    // the rest pose's bend at each joint (about A, or A2)
        float     fan{ 0.0f };  // the rest pose's spread from the hand's forward (about k, + towards the thumb)
    };
    struct Hand
    {
        bool      ok{ false };
        glm::vec3 wrist{}, fwd{}, a{}, k{}, palm{};
        Finger    f[5];
    };
    Hand g_hand[2];
    bool g_hand_tried[2]{};

    const Hand& hand(int side)
    {
        Hand& h = g_hand[side];
        if (g_hand_tried[side]) return h;
        g_hand_tried[side] = true;
        const std::string S = side == 0 ? "Right" : "Left";
        bool              ok = joint_by_name(S + "Hand", h.wrist);
        glm::vec3         mcp{ 0.0f };
        for (int f = 0; f < 5 && ok; ++f) {
            Finger& F = h.f[f];
            for (int j = 0; j < 3 && ok; ++j) ok = joint_by_name(S + "Hand" + kFinger[f] + std::to_string(j + 1), F.P[j]);
            if (f > 0 && ok) ok = joint_by_name(S + "InHand" + kFinger[f], F.base);
            if (f == 0) F.base = h.wrist;
            if (!ok) break;
            F.P[3] = F.P[2] + 0.8f * (F.P[2] - F.P[1]);
            if (f > 0) mcp += F.P[0];
        }
        if (!ok) {
            spdlog::info("[Hands] fingernames: the {} hand's joints are not in the bone tables: the shape-based pose is used", S);
            return h;
        }
        h.fwd   = nrm(mcp * 0.25f - h.wrist);
        glm::vec3 a = h.f[1].P[0] - h.f[4].P[0];   // towards the thumb: the index's knuckle from the little finger's
        h.a     = nrm(a - h.fwd * glm::dot(a, h.fwd));
        h.k     = nrm(glm::cross(h.fwd, h.a));      // a turn about k moves the forward towards the thumb
        // The palm: the side the thumb hangs on in the rest pose (right hand: -k).
        h.palm  = glm::dot(h.f[0].P[2] - h.wrist, h.k) > 0.0f ? h.k : -h.k;
        for (int f = 0; f < 5; ++f) {
            Finger&         F = h.f[f];
            const glm::vec3 d[3]{ nrm(F.P[1] - F.P[0]), nrm(F.P[2] - F.P[1]), nrm(F.P[3] - F.P[2]) };
            if (f == 0) {
                F.A  = nrm(glm::cross(d[0], nrm(0.5f * h.palm - h.a)));   // the base folds across the palm, towards the little finger
                F.A2 = nrm(glm::cross(d[0], -h.a));                        // the knuckles across
                F.bind[0] = 0.0f;                                          // the base's bend is added to the rest pose's
                F.bind[1] = sangle(d[0], d[1], F.A2);
                F.bind[2] = sangle(d[1], d[2], F.A2);
            } else {
                F.A  = nrm(glm::cross(d[0], h.palm));
                F.A2 = F.A;
                F.bind[0] = sangle(nrm(F.P[0] - F.base), d[0], F.A);
                F.bind[1] = sangle(d[0], d[1], F.A);
                F.bind[2] = sangle(d[1], d[2], F.A);
                F.fan     = sangle(h.fwd, d[0], h.k);
            }
        }
        h.ok = true;
        spdlog::info("[Hands] fingernames: the {} hand's skeleton from the bone tables: rest bends index {:.0f}/{:.0f}/{:.0f}, little {:.0f}/{:.0f}/{:.0f} deg, "
                     "fan index {:.0f}, little {:.0f} deg", S, glm::degrees(h.f[1].bind[0]), glm::degrees(h.f[1].bind[1]), glm::degrees(h.f[1].bind[2]),
                     glm::degrees(h.f[4].bind[0]), glm::degrees(h.f[4].bind[1]), glm::degrees(h.f[4].bind[2]), glm::degrees(h.f[1].fan), glm::degrees(h.f[4].fan));
        return h;
    }

    // What a palette slot is to its hand.
    enum Kind : uint8_t { kNone = 0, kPalm, kOnPalm, kOnBone, kOnJoint };
    struct Slot
    {
        int     slot{ 0 };
        uint8_t kind{ kNone };
        uint8_t finger{ 0 };
        uint8_t seg{ 0 };   // kOnBone: the bone it rides (0 the palm .. 3 the last); kOnJoint: the joint (1..3, as P[0..2])
        bool    main{ false };   // the bone itself (RightHandIndex1..3), not a skin helper: fingergame reads its turn
    };
    struct Side
    {
        bool              ok{ false };
        int               palm{ -1 };
        int               helpers{ 0 };
        std::vector<Slot> slots;
    };
    struct Match
    {
        int   draw{ -1 };
        float cm{ 0.0f };
        Side  side[2];
    };
    std::map<std::tuple<uint32_t, int, int64_t>, Match> g_match;

    // A slot's role from its name ("RightHandIndex2", "RightInHandRing", "Index_R_04", "Ringknuckles_R_02", "RightHandThumbHelper").
    bool role(const char* name, int side, const glm::vec3& joint, Slot& out)
    {
        if (!name) return false;
        const std::string n = name;
        const std::string S = side == 0 ? "Right" : "Left";
        const std::string s = side == 0 ? "_R_" : "_L_";
        const Hand&       h = hand(side);
        if (n == S + "Hand") { out.kind = kPalm; return true; }
        if (n == S + "HandThumbHelper") { out.kind = kOnJoint; out.finger = 0; out.seg = 1; return true; }
        for (int f = 0; f < 5; ++f) {
            const std::string F = kFinger[f];
            if (f > 0 && n == S + "InHand" + F) { out.kind = kOnPalm; out.finger = uint8_t(f); return true; }
            if (n.size() == S.size() + 5 + F.size() && n.rfind(S + "Hand" + F, 0) == 0) {
                const char c = n.back();
                if (c < '1' || c > '4') return false;
                out.kind   = kOnBone;
                out.finger = uint8_t(f);
                out.seg    = uint8_t(std::min(3, c - '0'));
                out.main   = c <= '3';
                return true;
            }
            const bool knuckle = n.rfind(F + "knuckles" + s, 0) == 0;
            if (!knuckle && n.rfind(F + s, 0) != 0) continue;
            const Finger& G = h.f[f];
            out.finger      = uint8_t(f);
            if (knuckle) {   // on a joint: the nearest one
                int   best = 0;
                float bd   = 1e9f;
                for (int j = 0; j < 3; ++j) if (const float d = glm::length(joint - G.P[j]); d < bd) { bd = d; best = j; }
                out.kind = kOnJoint;
                out.seg  = uint8_t(best + 1);
            } else {         // along a bone: the nearest one (0: the metacarpal, on the palm)
                const glm::vec3 pts[5]{ G.base, G.P[0], G.P[1], G.P[2], G.P[3] };
                int             best = 0;
                float           bd   = 1e9f;
                for (int b = 0; b < 4; ++b) if (const float d = seg_dist(joint, pts[b], pts[b + 1]); d < bd) { bd = d; best = b; }
                out.kind = best == 0 ? kOnPalm : kOnBone;
                out.seg  = uint8_t(best);
            }
            return true;
        }
        return false;
    }

    // The draw by its index count (an indirect draw's count is rounded up to 192), else by shape alone, checked against the mesh: the candidate whose
    // joints lie nearest the slots' vertex centres (median, slots with 3 vertices or more) within 5 cm.
    const Match& match(uint32_t count, const std::vector<glm::vec4>& cent, float scale)
    {
        int64_t key_w = 0;
        int     used  = 0;
        for (int b = 0; b < (int)cent.size() && b < 256; ++b)
            if (cent[b].x > 0.0f) { key_w += int64_t(b) * 7919 + int64_t(cent[b].x * 10.0f); ++used; }
        const auto key = std::make_tuple(count, used, key_w);
        if (auto it = g_match.find(key); it != g_match.end()) return it->second;
        if (g_match.size() > 64) g_match.clear();
        Match& m = g_match[key];
        using namespace fc5::bonenames;
        // The highest slot the mesh uses: a candidate must have that many.
        int top = -1;
        for (int b = 0; b < (int)cent.size() && b < 256; ++b) if (cent[b].x > 0.0f) top = b;
        // Median distance (cm) between a draw's joints and this mesh's bone centres (slots with 3 vertices or more).
        auto fit = [&](int d) -> float {
            const Draw& D = kDraws[d];
            if (D.kind < 2 || D.kind > 4 || D.nbones < 4 || D.nbones <= top) return 1e9f;
            std::vector<float> dist;
            for (int s = 0; s < D.nbones && s < (int)cent.size(); ++s) {
                if (cent[s].x < 3.0f) continue;
                const int16_t*  j = kJoint[D.first + uint32_t(s)];
                const glm::vec3 jp{ float(j[0]) * 0.001f, float(j[1]) * 0.001f, float(j[2]) * 0.001f };
                dist.push_back(glm::length(jp - glm::vec3{ cent[s].y, cent[s].z, cent[s].w } * scale));
            }
            if (dist.size() < 4) return 1e9f;
            std::nth_element(dist.begin(), dist.begin() + dist.size() / 2, dist.end());
            return dist[dist.size() / 2] * 100.0f;
        };
        // First the draws of this index count (an indirect draw's rounded up to 192), within 5 cm.
        const uint32_t from    = count > 191 ? count - 191 : count;
        float          best_cm = 1e9f;
        const char*    best_n  = "none";
        for (int d = 0; d < kDrawN; ++d) {
            const Draw& D = kDraws[d];
            if (D.count < from || D.count > count) continue;
            if (D.count != count && (D.count + 191) / 192 * 192 != count) continue;
            if (const float cm = fit(d); cm < best_cm) { best_cm = cm; m.draw = d; best_n = kMesh[D.mesh]; }
        }
        if (m.draw >= 0 && best_cm > 5.0f) m.draw = -1;
        // Else by shape alone: every hand, arm and outfit draw in the tables, the nearest within 4 cm. The author's bare arms (2 Nov
        // 15:17) come as indirect draws of 14400 indices with 66 and 67 slots used: the count is not the model's (16632).
        bool by_shape = false;
        if (m.draw < 0) {
            float cm2 = 1e9f;
            int   d2  = -1;
            for (int d = 0; d < kDrawN; ++d) if (const float cm = fit(d); cm < cm2) { cm2 = cm; d2 = d; }
            if (d2 >= 0) {
                spdlog::info("[Hands] fingernames: draw {} ({} slots used): no model of that count fits (nearest {} at {:.1f} cm); by shape the nearest is {} ({} slots) at {:.1f} cm{}",
                             count, top + 1, best_n, best_cm, kMesh[kDraws[d2].mesh], kDraws[d2].nbones, cm2, cm2 <= 4.0f ? ": taken" : ": too far");
                best_n = kMesh[kDraws[d2].mesh];
                best_cm = cm2;
                if (cm2 <= 4.0f) { m.draw = d2; by_shape = true; }
            }
        }
        m.cm = best_cm;
        if (m.draw >= 0) {
            const Draw& D = kDraws[m.draw];
            for (int side = 0; side < 2; ++side) {
                Side& sd = m.side[side];
                if (!hand(side).ok) continue;
                int named = 0;
                for (int s = 0; s < D.nbones && s < 256; ++s) {
                    const uint32_t  i = D.first + uint32_t(s);
                    const glm::vec3 jp{ float(kJoint[i][0]) * 0.001f, float(kJoint[i][1]) * 0.001f, float(kJoint[i][2]) * 0.001f };
                    Slot            sl{};
                    sl.slot = s;
                    if (!role(kName[kSlot[i]], side, jp, sl)) continue;
                    if (sl.kind == kPalm) { sd.palm = s; continue; }
                    const char* nm = kName[kSlot[i]];
                    if (std::strchr(nm, '_')) sd.helpers++;
                    sd.slots.push_back(sl);
                    if (sl.kind == kOnBone) named++;
                }
                sd.ok = sd.palm >= 0 && named >= 8;
                if (sd.ok)
                    spdlog::info("[Hands] fingernames: draw {} is {} ({}; its joints {:.1f} cm from the mesh's bone centres): {} hand, palm slot {}, {} finger slots ({} skin helpers)",
                                 count, kMesh[D.mesh], by_shape ? "by shape" : "by index count", best_cm, side == 0 ? "right" : "left", sd.palm, sd.slots.size(), sd.helpers);
            }
            if (m.side[0].ok || m.side[1].ok) g_known++;
            else { m.draw = -1; g_unknown++; }
        } else {
            g_unknown++;
        }
        if (m.draw < 0)
            spdlog::info("[Hands] fingernames: draw {} ({} slots used) not known by name (nearest {} at {:.0f} cm): the shape-based pose is used", count, used, best_n, best_cm);
        return m;
    }

    // Each hand's curls (thumb .. little, each finger's three joints), eased once per frame.
    float g_curl[2][5][3]{};
    int   g_curl_frame[2]{ -1, -1 };
    int   g_hold_was[2]{};
    // What a hand holds (Input::hold): per joint curls, thumb .. little (the thumb's first value turns its base, the other two bend its
    // knuckles). Sized on the bare arms' skeleton (2 Nov research, 2026-11-02_fingerholds.md): a curl of 1 is a tight fist.
    //  - String: the string (or the nock) pinched, the thumb pad on the side of the curled index's middle bone (~1.8 cm between the
    //    bones' centres, finger on finger), the other fingers curled out of the way. Nock: the same pinch on an arrow carried in the
    //    hand, the other fingers looser.
    //  - Magazine: the fingers round a ~4 cm body (a rifle's box gripped across), each finger's bones ~1 cm off it, the thumb along
    //    its side. Small: a ~3 cm one (a pistol's magazine). Chasm's Magazine and Small shapes (0.55-0.62, 0.68-0.78) rescaled to
    //    this skeleton, on which they leave a 2-3 cm gap.
    //  - Handle: the AR-C's charging handle hooked by the index and middle fingers' middle joints, the ring and little fingers curled.
    //    Slide: a pistol's slide gripped from above, the fingers round ~5 cm.
    //  - Round (manualreload46; author 3 Nov 16:31): a
    //    round or a shell (1.2-2 cm across) pinched tip to tip: the index half curled, the thumb turned across to meet it with its knuckles
    //    nearly straight, the middle finger curled under, the ring and little fingers closed into the palm.
    constexpr const char* kHoldName[kHoldN] = { "free", "the bow string (pinch)", "an arrow (pinch)", "a magazine", "a pistol magazine", "the charging handle (hook)", "the slide",
                                                "a round or a shell (pinch)" };
    constexpr float kHold[kHoldN][5][3] = {
        {},
        { { 0.50f, 1.00f, 1.00f }, { 1.00f, 1.00f, 0.50f }, { 0.80f, 0.80f, 0.60f }, { 0.85f, 0.85f, 0.60f }, { 0.90f, 0.85f, 0.60f } },
        { { 0.50f, 1.00f, 1.00f }, { 1.00f, 1.00f, 0.50f }, { 0.55f, 0.60f, 0.50f }, { 0.60f, 0.65f, 0.50f }, { 0.65f, 0.70f, 0.50f } },
        { { 0.50f, 0.60f, 0.60f }, { 0.75f, 0.65f, 0.85f }, { 0.70f, 0.65f, 0.80f }, { 0.65f, 0.65f, 0.80f }, { 0.55f, 0.60f, 0.70f } },
        { { 0.60f, 0.70f, 0.70f }, { 0.90f, 0.80f, 0.95f }, { 0.85f, 0.80f, 0.90f }, { 0.80f, 0.80f, 0.95f }, { 0.60f, 0.65f, 0.80f } },
        { { 0.40f, 0.50f, 0.50f }, { 0.45f, 0.95f, 0.70f }, { 0.45f, 0.95f, 0.70f }, { 0.85f, 0.90f, 0.70f }, { 0.90f, 0.90f, 0.70f } },
        { { 0.40f, 0.40f, 0.40f }, { 0.60f, 0.55f, 0.70f }, { 0.60f, 0.55f, 0.70f }, { 0.60f, 0.50f, 0.80f }, { 0.50f, 0.50f, 0.60f } },
        { { 0.75f, 0.35f, 0.30f }, { 0.55f, 0.60f, 0.45f }, { 0.80f, 0.85f, 0.65f }, { 0.95f, 0.95f, 0.75f }, { 1.00f, 0.95f, 0.75f } },
    };

    void update_curls(int side, const Input& in, int frame)
    {
        if (g_curl_frame[side] == frame) return;
        g_curl_frame[side] = frame;
        float t[5];
        const int hold = g_holds.load() ? std::clamp(in.hold, 0, kHoldN - 1) : 0;
        if (hold != g_hold_was[side]) {
            spdlog::info("[Hands] fingernames: the {} hand {}", side == 0 ? "right" : "left", hold ? fmt::format("holds {}", kHoldName[hold]) : std::string{ "is free (grip, trigger and thumb)" });
            g_hold_was[side] = hold;
        }
        if (in.test >= 0) {
            static constexpr float kTest[3][5] = { { 0.0f, 0.1f, 0.12f, 0.12f, 0.12f }, { 0.75f, 0.05f, 1.0f, 1.0f, 1.0f }, { 0.85f, 1.0f, 1.0f, 1.0f, 1.0f } };
            for (int f = 0; f < 5; ++f) t[f] = kTest[std::clamp(in.test, 0, 2)][f];
        } else if (hold > 0) {
            const float k = 0.3f;
            for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) g_curl[side][f][j] += (kHold[hold][f][j] - g_curl[side][f][j]) * k;
            return;
        } else {
            const float g = in.grip ? 1.0f : 0.0f;
            t[0] = in.thumb ? 0.15f + 0.6f * g + ((in.grip && in.trig) ? 0.1f : 0.0f) : -0.4f;
            t[1] = in.trig ? (in.grip ? 1.0f : 0.7f) : in.ttouch ? (in.grip ? 0.35f : 0.25f) : (in.grip ? 0.05f : 0.1f);
            t[2] = t[3] = t[4] = in.grip ? 1.0f : 0.12f;
        }
        const float k = in.test >= 0 ? 1.0f : 0.3f;   // ~60 ms at 72-90 frames a second; a test pose at once
        for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) g_curl[side][f][j] += (t[f] - g_curl[side][f][j]) * k;
    }

    glm::mat4 from_rows(const float* m)
    {
        glm::mat4 M{ 1.0f };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) M[c][r] = m[r * 4 + c];
        return M;
    }
    // x -> q (x - pivot) + to, as a matrix.
    glm::mat4 turn_about(const glm::quat& q, const glm::vec3& pivot, const glm::vec3& to)
    {
        glm::mat4 T = glm::mat4_cast(q);
        T[3]        = glm::vec4{ to - glm::mat3_cast(q) * pivot, 1.0f };
        return T;
    }
}

namespace
{
    // fingergame: the game's own grip on its spare magazine (and its charging handle), per weapon. Each finger bone's turn relative to
    // the palm (rest pose space), read from the game's hand while its reload brings the magazine in (its prop drawn), or pulls the
    // handle (game_holds, from the manual reload's learning), the frame most like all the others of that stretch (the grip held, not the
    // reach to it). Kept in bin\fc5vr_fingerposes.txt: "<weapon id> <side> <hold> <15 x w x y z>" (hold 3 the magazine, 5 the handle;
    // a line without it: the magazine).
    struct Learnt
    {
        glm::quat q[5][3];
        int       side{ 1 };
        int       frames{ 0 };
    };
    std::mutex                   g_learn_mtx;
    std::map<std::pair<uint32_t, int>, Learnt> g_learnt;   // (weapon, kMagazine or kHandle)
    bool                         g_learnt_read{ false };
    float                        g_game_w[2]{};      // the learnt grip's share, eased (pose)
    int                          g_game_frame[2]{ -1, -1 };
    bool                         g_game_used[2]{};   // last frame's, for the log
    struct Window
    {
        uint32_t                                sig{ 0 };
        int                                     side{ -1 };
        int                                     hold{ kFree };
        int                                     last{ -1000 };   // the last frame it held the prop
        int                                     seen{ -1 };      // the last frame looked at
        std::vector<std::array<glm::quat, 15>>  poses;
    };
    Window g_win;
    // game_holds: what the manual reload's learning sees the game's hand pull (the handle), and when.
    std::atomic<int> g_note_hold{ kFree }, g_note_frame{ -1000 };
    // note_game_palm / game_palm.
    struct Palm
    {
        glm::mat4 world{ 1.0f }, to_ctrl{ 1.0f };
        int       frame{ -1000 };
        glm::mat4 ctrl{ 1.0f };         // reloadrec: note_free_ctrl
        glm::vec3 rest{ 0.0f };         // ... the palm's middle in rest pose space
        float     bend[2][15]{};        // ... the game's, as drawn
        bool      bend_ok[2]{};
        glm::quat turn[15]{};           // ... the game's
    };
    Palm              g_palm[2];
    std::atomic<bool> g_track_bends{ false };
    std::atomic<bool> g_builtin_grips{ true };
    // reloadhandgame: a recorded grip: this weapon's on its magazine (kMagazine) or slide (kHandle), for the left hand.
    bool builtin_grip(uint32_t sig, int kind, int side, Learnt& L)
    {
        if (!g_builtin_grips.load() || side != 1) return false;
        for (const auto& r : fc5::reloadfingers::kRows) {
            if (r.sig != sig || r.kind != (kind == kHandle ? 1 : 0)) continue;
            L.side = side;
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j) {
                    const float* a = r.q[f * 3 + j];
                    L.q[f][j]      = glm::normalize(glm::quat{ a[0], a[1], a[2], a[3] });
                }
            return true;
        }
        return false;
    }
    const char* grip_of(int hold) { return hold == kHandle ? "charging handle" : "magazine"; }

    void read_learnt()   // g_learn_mtx held
    {
        if (g_learnt_read) return;
        g_learnt_read = true;
        std::ifstream f{ fc5::mod_file("fc5vr_fingerposes.txt") };
        std::string   line;
        int           n = 0;
        while (std::getline(f, line)) {
            std::istringstream ls{ line };
            std::vector<float> v;
            for (float x; ls >> x;) v.push_back(x);
            if (v.size() != 62 && v.size() != 63) continue;
            const uint32_t sig  = uint32_t(v[0]);
            const int      hold = v.size() == 63 ? int(v[2]) : int(kMagazine);
            Learnt         L{};
            L.side = int(v[1]);
            if (L.side < 0 || L.side > 1 || (hold != kMagazine && hold != kHandle)) continue;
            size_t at = v.size() - 60;
            bool ok = true;
            for (int f5 = 0; f5 < 5 && ok; ++f5)
                for (int j = 0; j < 3 && ok; ++j) {
                    L.q[f5][j] = glm::normalize(glm::quat{ v[at], v[at + 1], v[at + 2], v[at + 3] });
                    at += 4;
                }
            if (ok) { g_learnt[{ sig, hold }] = L; ++n; }
        }
        if (n) spdlog::info("[Hands] fingergame: {} learnt grips (magazines, charging handles) read from fc5vr_fingerposes.txt", n);
    }
    void write_learnt()   // g_learn_mtx held
    {
        std::ofstream f{ fc5::mod_file("fc5vr_fingerposes.txt"), std::ios::trunc };
        for (const auto& [key, L] : g_learnt) {
            f << key.first << ' ' << L.side << ' ' << key.second;
            for (int f5 = 0; f5 < 5; ++f5) for (int j = 0; j < 3; ++j) f << fmt::format(" {:.5f} {:.5f} {:.5f} {:.5f}", L.q[f5][j].w, L.q[f5][j].x, L.q[f5][j].y, L.q[f5][j].z);
            f << '\n';
        }
    }
    // Each finger bone's turn from its rest pose relative to the palm, in this draw's palette (the game's pose, before the mod moves it).
    bool game_turns(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, std::array<glm::quat, 15>& out)
    {
        const Match& m = match(count, cent, scale);
        if (m.draw < 0 || !m.side[side].ok) return false;
        const Side& sd = m.side[side];
        if (sd.palm < 0 || (sd.palm + 1) * 12 > (int)bones.size()) return false;
        const glm::mat4 Mpi = glm::inverse(from_rows(bones.data() + sd.palm * 12));
        bool got[15]{};
        for (const Slot& s : sd.slots) {
            if (!s.main || s.kind != kOnBone || s.seg < 1 || s.seg > 3 || (s.slot + 1) * 12 > (int)bones.size()) continue;
            glm::mat3 R{ Mpi * from_rows(bones.data() + s.slot * 12) };
            for (int c = 0; c < 3; ++c) {
                const float l = glm::length(R[c]);
                if (l < 0.5f || l > 2.0f) return false;   // not a bone's turn (scaled, or an empty slot)
                R[c] /= l;
            }
            out[s.finger * 3 + s.seg - 1] = glm::normalize(glm::quat_cast(R));
            got[s.finger * 3 + s.seg - 1] = true;
        }
        return std::all_of(std::begin(got), std::end(got), [](bool b) { return b; });
    }
    float qdist(const glm::quat& a, const glm::quat& b) { return 1.0f - std::fabs(glm::dot(a, b)); }
    // A joint's bend in degrees: the turn of bone j against the bone before it.
    float bend_deg(const glm::quat* q, int j)
    {
        const glm::quat r = j == 0 ? q[0] : glm::inverse(q[j - 1]) * q[j];
        return glm::degrees(2.0f * std::acos(std::clamp(std::fabs(r.w), 0.0f, 1.0f)));
    }
    void finish_window()
    {
        Window& w = g_win;
        const size_t n = w.poses.size();
        if (n >= 8 && w.sig != 0) {
            size_t best = 0;
            float  bd   = 1e30f;
            for (size_t i = 0; i < n; ++i) {
                float d = 0.0f;
                for (size_t k = 0; k < n; ++k) for (int j = 0; j < 15; ++j) d += qdist(w.poses[i][j], w.poses[k][j]);
                if (d < bd) { bd = d; best = i; }
            }
            float spread = 0.0f;   // the mean turn of the other frames' bones from it (deg)
            for (size_t k = 0; k < n; ++k)
                for (int j = 0; j < 15; ++j) spread += glm::degrees(2.0f * std::acos(std::clamp(std::fabs(glm::dot(w.poses[best][j], w.poses[k][j])), 0.0f, 1.0f)));
            spread /= float(n * 15);
            Learnt L{};
            L.side   = w.side;
            L.frames = int(n);
            for (int f5 = 0; f5 < 5; ++f5) for (int j = 0; j < 3; ++j) L.q[f5][j] = w.poses[best][f5 * 3 + j];
            {
                std::lock_guard _{ g_learn_mtx };
                read_learnt();
                g_learnt[{ w.sig, w.hold }] = L;
                write_learnt();
            }
            spdlog::info("[Hands] fingergame: weapon {}: the {} hand's grip on its {} learnt from the game's reload (frame {} of {} holding it, "
                         "the others' bones {:.1f} deg from it on average); bends (deg, joints 1/2/3) thumb {:.0f}/{:.0f}/{:.0f}, index {:.0f}/{:.0f}/{:.0f}, "
                         "middle {:.0f}/{:.0f}/{:.0f}, little {:.0f}/{:.0f}/{:.0f}; kept in fc5vr_fingerposes.txt",
                         w.sig, w.side == 0 ? "right" : "left", grip_of(w.hold), best + 1, n, spread,
                         bend_deg(L.q[0], 0), bend_deg(L.q[0], 1), bend_deg(L.q[0], 2), bend_deg(L.q[1], 0), bend_deg(L.q[1], 1), bend_deg(L.q[1], 2),
                         bend_deg(L.q[2], 0), bend_deg(L.q[2], 1), bend_deg(L.q[2], 2), bend_deg(L.q[4], 0), bend_deg(L.q[4], 1), bend_deg(L.q[4], 2));
        } else if (n > 0) {
            spdlog::info("[Hands] fingergame: weapon {}: the game's hand held its {} {} frames only: nothing learnt", w.sig, grip_of(w.hold), n);
        }
        w.poses.clear();
    }
}

bool on() { return g_on.load(); }
bool game_on() { return g_game.load(); }

void game_holds(int hold, int frame)
{
    if (hold != kHandle) return;
    g_note_hold  = hold;
    g_note_frame = frame;
}

void note_game_palm(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, const glm::mat4& obj,
                    const glm::mat4& ctrl_from_world, int frame)
{
    if (!g_on.load() || side < 0 || side > 1 || bones.size() < 3072 || count == 0 || scale <= 0.0f) return;
    const Match& m = match(count, cent, scale);
    if (m.draw < 0 || !m.side[side].ok) return;
    const int palm = m.side[side].palm;
    if (palm < 0 || palm >= (int)cent.size() || cent[palm].x <= 0.0f) return;
    glm::mat4 world = obj * from_rows(bones.data() + palm * 12);
    std::array<glm::quat, 15> turns{};
    const bool                track = g_track_bends.load() && game_turns(bones, cent, count, side, scale, turns);
    std::lock_guard _{ g_learn_mtx };
    Palm& p   = g_palm[side];
    p.world   = world;
    p.to_ctrl = ctrl_from_world * world;
    p.frame   = frame;
    p.rest    = glm::vec3{ cent[palm].y, cent[palm].z, cent[palm].w } * scale;
    p.bend_ok[0] = track;
    if (track)
        for (int i = 0; i < 15; ++i) {
            p.bend[0][i] = bend_deg(turns.data() + i / 3 * 3, i % 3);
            p.turn[i]    = turns[size_t(i)];
        }
}

void note_free_ctrl(int side, const glm::mat4& ctrl)
{
    if (side < 0 || side > 1) return;
    std::lock_guard _{ g_learn_mtx };
    g_palm[side].ctrl = ctrl;
}

bool free_ctrl(int side, glm::mat4& ctrl, glm::vec3& palm_rest)
{
    if (side < 0 || side > 1) return false;
    std::lock_guard _{ g_learn_mtx };
    if (g_palm[side].frame < 0) return false;
    ctrl      = g_palm[side].ctrl;
    palm_rest = g_palm[side].rest;
    return true;
}

void track_bends(bool on) { g_track_bends = on; }
void use_builtin_grips(bool on) { g_builtin_grips = on; }

void note_drawn_bends(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale)
{
    if (!g_track_bends.load() || side < 0 || side > 1 || bones.size() < 3072 || count == 0 || scale <= 0.0f) return;
    std::array<glm::quat, 15> turns{};
    const bool                ok = game_turns(bones, cent, count, side, scale, turns);
    std::lock_guard _{ g_learn_mtx };
    Palm& p = g_palm[side];
    p.bend_ok[1] = ok;
    if (ok)
        for (int i = 0; i < 15; ++i) p.bend[1][i] = bend_deg(turns.data() + i / 3 * 3, i % 3);
}

bool game_finger_turns(int side, glm::quat* out15)
{
    if (side < 0 || side > 1) return false;
    std::lock_guard _{ g_learn_mtx };
    if (!g_palm[side].bend_ok[0]) return false;
    std::copy_n(g_palm[side].turn, 15, out15);
    return true;
}

bool bends(int side, bool drawn, float* out15)
{
    if (side < 0 || side > 1) return false;
    std::lock_guard _{ g_learn_mtx };
    const Palm& p = g_palm[side];
    if (!p.bend_ok[drawn ? 1 : 0]) return false;
    std::copy_n(p.bend[drawn ? 1 : 0], 15, out15);
    return true;
}

bool game_palm(int side, glm::mat4& palm_world, glm::mat4& ctrl_from_palm, int& frame)
{
    if (side < 0 || side > 1) return false;
    std::lock_guard _{ g_learn_mtx };
    const Palm& p = g_palm[side];
    if (p.frame < 0) return false;
    palm_world     = p.world;
    ctrl_from_palm = p.to_ctrl;
    frame          = p.frame;
    return true;
}

void learn(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, uint32_t sig, bool prop, int frame)
{
    if (!g_game.load() || !g_on.load() || side < 0 || side > 1 || sig == 0 || bones.size() < 3072 || count == 0 || scale <= 0.0f) return;
    Window& w = g_win;
    if (w.seen == frame) return;   // a frame's second weapon pass
    w.seen = frame;
    // The magazine while its prop is drawn; else the handle while the manual reload's learning sees it pulled (a frame late: 2 frames' slack).
    const int nf   = g_note_frame.load();
    const int hold = prop ? int(kMagazine) : (frame - nf >= 0 && frame - nf <= 2) ? g_note_hold.load() : int(kFree);
    if (w.sig != sig || w.side != side || (hold != kFree && hold != w.hold)) {
        finish_window();
        w.sig  = sig;
        w.side = side;
        w.hold = hold != kFree ? hold : w.hold;
    }
    if (!w.poses.empty() && frame - w.last > 3) finish_window();   // the magazine left the hand (seated), the handle was let go, or the reload was cut
    if (hold == kFree) return;
    std::array<glm::quat, 15> t;
    if (!game_turns(bones, cent, count, side, scale, t)) return;
    if (w.poses.size() < 400) w.poses.push_back(t);
    w.last = frame;
}

bool pose(std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, const Input& in, int frame)
{
    if (!g_on.load() || side < 0 || side > 1 || bones.size() < 3072 || count == 0 || scale <= 0.0f) return false;
    if (g_reset.exchange(false)) g_match.clear();
    const Match& m = match(count, cent, scale);
    if (m.draw < 0 || !m.side[side].ok) return false;
    const Hand& h = hand(side);
    if (!h.ok) return false;
    const Side& sd = m.side[side];
    // The palm must have skin in this draw: the hand's move takes only the bones that do (the fingers go where the palm is).
    if (sd.palm < 0 || sd.palm >= (int)cent.size() || cent[sd.palm].x <= 0.0f) return false;
    update_curls(side, in, frame);
    const float fist = g_fist.load();
    // Each finger's bones: q[b] the turn of bone b (0 the palm), at[b] where its root joint goes (rest pose space, m).
    glm::quat q[5][4];
    glm::vec3 at[5][4];
    for (int f = 0; f < 5; ++f) {
        const Finger& F = h.f[f];
        const float*  c = g_curl[side][f];
        const float   fs = g_hold_was[side] > 0 ? 1.0f : fist;   // fingerfist: the free hand's fist only
        q[f][0]  = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        at[f][0] = F.base;
        glm::quat cur{ 1.0f, 0.0f, 0.0f, 0.0f };
        for (int j = 0; j < 3; ++j) {
            glm::quat d;
            if (f == 0) {
                d = j == 0 ? glm::angleAxis(kThumbOut * c[0], h.k) * glm::angleAxis(kThumbFold * c[0], F.A)
                           : glm::angleAxis(std::clamp(c[j], 0.0f, 1.0f) * (j == 1 ? kThumbK1 : kThumbK2) - F.bind[j], F.A2);
            } else {
                const float cp = std::clamp(c[j], 0.0f, 1.0f);
                d = glm::angleAxis(cp * fs * kCurl[f][j] - F.bind[j], F.A);
                if (j == 0) d = glm::angleAxis(-F.fan * kClose * std::clamp(c[1], 0.0f, 1.0f), h.k) * d;
            }
            cur         = cur * d;
            q[f][j + 1] = cur;
        }
    }
    // fingergame: holding a magazine, the game's own grip on that weapon's magazine (learnt from its reload) instead of the shape above.
    {
        const Learnt* L    = nullptr;
        const int     kind = (in.hold == kMagazine || in.hold == kMagSmall) ? int(kMagazine) : (in.hold == kHandle || in.hold == kSlide) ? int(kHandle) : int(kFree);
        // reloadmaglower: the shape above, closed round the magazine, not the game's grip from under. A shell keeps the SPAS-12's (kMagSmall on a tube; kMagazine only on magazine guns once mag_lower says so).
        const bool    lower = kind == int(kMagazine) && in.sig != 0 && fc5::reload::mag_lower(in.sig);
        const bool    want = g_game.load() && in.sig != 0 && g_hold_was[side] > 0 && kind != kFree && !lower;
        Learnt        copy{};
        // (reloadmaglower: the SVD's grip, the game's closest round a magazine's body, on every magazine: the shape above was left half open: author 5 Nov, the Luger.)
        if (in.sig != 0 && g_hold_was[side] > 0 && kind != kFree && builtin_grip(lower ? 42555u : in.sig, kind, side, copy)) {
            L = &copy;
        } else if (want) {
            std::lock_guard _{ g_learn_mtx };
            read_learnt();
            if (auto it = g_learnt.find({ in.sig, kind }); it != g_learnt.end() && it->second.side == side) { copy = it->second; L = &copy; }
        }
        if (g_game_frame[side] != frame) {
            g_game_frame[side] = frame;
            g_game_w[side] += ((L ? 1.0f : 0.0f) - g_game_w[side]) * 0.3f;
            if (g_game_used[side] != (L != nullptr))
                spdlog::info("[Hands] fingergame: the {} hand {}", side == 0 ? "right" : "left",
                             L ? fmt::format("takes the game's own grip on weapon {}'s {}", in.sig, grip_of(kind)) : std::string{ "leaves the game's own grip" });
            g_game_used[side] = L != nullptr;
            if (!L && g_game_w[side] < 0.01f) g_game_w[side] = 0.0f;
        }
        static Learnt last[2]{};
        if (L) last[side] = *L;
        if (g_game_w[side] > 0.0f)
            for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) q[f][j + 1] = glm::slerp(q[f][j + 1], last[side].q[f][j], g_game_w[side]);
    }
    for (int f = 0; f < 5; ++f) {
        const Finger& F = h.f[f];
        at[f][1] = F.P[0];
        for (int j = 1; j < 3; ++j) at[f][j + 1] = at[f][j] + q[f][j] * (F.P[j] - F.P[j - 1]);
    }
    const glm::mat4 Mp = from_rows(bones.data() + sd.palm * 12);
    for (const Slot& s : sd.slots) {
        if (s.slot <= 0 || s.slot >= 256) continue;
        const Finger& F = h.f[s.finger];
        glm::mat4     T{ 1.0f };
        if (s.kind == kOnBone && s.seg >= 1) T = turn_about(q[s.finger][s.seg], F.P[s.seg - 1], at[s.finger][s.seg]);
        else if (s.kind == kOnJoint) T = turn_about(glm::slerp(q[s.finger][s.seg - 1], q[s.finger][s.seg], 0.5f), F.P[s.seg - 1], at[s.finger][s.seg]);
        const glm::mat4 M = Mp * T;
        float*          o = bones.data() + s.slot * 12;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) o[r * 4 + c] = M[c][r];
    }
    if (g_log.exchange(false))
        spdlog::info("[Hands] fingerlog: {} hand, draw {} ({} slots): curls thumb {:.2f}, index {:.2f}, middle {:.2f}, ring {:.2f}, little {:.2f}; "
                     "grip {}, trigger {}, on the trigger {}, thumb down {}; holds {}{}", side == 0 ? "right" : "left", count, sd.slots.size(), g_curl[side][0][0],
                     g_curl[side][1][1], g_curl[side][2][1], g_curl[side][3][1], g_curl[side][4][1], in.grip, in.trig, in.ttouch, in.thumb, kHoldName[std::clamp(in.hold, 0, kHoldN - 1)],
                     in.test >= 0 ? fmt::format(" (fingertest {})", in.test) : g_game_w[side] > 0.0f ? fmt::format(" (the game's grip {:.0f}%)", g_game_w[side] * 100.0f) : std::string{});
    return true;
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream&, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "fingernames") { g_on = on; g_reset = true; persist = true; return true; }
    if (cmd == "fingerholds") { g_holds = on; persist = true; return true; }
    if (cmd == "fingergame") { g_game = on; persist = true; return true; }
    if (cmd == "fingergameforget") {   // test: drops every learnt grip (fc5vr_fingerposes.txt emptied)
        std::lock_guard _{ g_learn_mtx };
        g_learnt_read = true;
        g_learnt.clear();
        write_learnt();
        spdlog::info("[Hands] fingergame: every learnt magazine grip dropped");
        return true;
    }
    if (cmd == "fingerfist") { g_fist = std::clamp(arg.empty() ? 1.0f : fv, 0.5f, 1.2f); persist = true; return true; }
    if (cmd == "fingerlog") { g_log = true; return true; }   // test: the next pose's curls in the log
    return false;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("Fingers posed by bone name (fist, pointing, thumb)##fingernames", &v)) cmds += v ? "fingernames on\n" : "fingernames off\n";
    hint("On (default, 2 Nov): with the hand open, pointing or closed, each finger bends at its own three joints like a real hand, "
         "the thumb folds across the fingers, and the fingers close together in a fist. The bones are known by the names in the game's "
         "models, so no finger takes another's bones. Off: the 19-20 Oct pose (the fingers found from the hand's shape, closed as the "
         "game holds the weapon).");
    if (!v) return;
    float f = g_fist.load();
    if (ImGui::SliderFloat("Fist: how far the fingers close##fingerfist", &f, 0.5f, 1.2f, "%.2f")) cmds += fmt::format("fingerfist {:.2f}\n", f);
    hint("1 (default): a full fist. Lower if the fingertips go into the palm, higher if the fist looks loose.");
    bool hv = g_holds.load();
    if (ImGui::Checkbox("Fingers round what the hand holds (bow string, arrow, magazine, charging handle)##fingerholds", &hv))
        cmds += hv ? "fingerholds on\n" : "fingerholds off\n";
    hint("On (default, 2 Nov): the hand on the bow's string pinches it between the thumb and the side of the curled index, as it does "
         "an arrow taken from the quiver; the free hand's fingers wrap round the magazine it holds or slides into the well (closer "
         "round a pistol's), hook the charging handle while racking, or grip a pistol's slide. Off: the controller's grip, trigger "
         "and thumb pose the fingers as when the hand is empty.");
    bool gv = g_game.load();
    if (ImGui::Checkbox("Magazine grip learnt from the game's own reload (test)##fingergame", &gv)) cmds += gv ? "fingergame on\n" : "fingergame off\n";
    hint("Off (default). On: at the game's own reload of each weapon (the clip run dry, or tapped by the manual reload), the fingers of "
         "the game's hand round its spare magazine (and on its charging handle, when the reload's learning sees it pulled) are learnt, and "
         "your free hand takes that grip when it holds that weapon's magazine or racks it (instead of the fixed shapes). Learnt grips are "
         "kept in fc5vr_fingerposes.txt.");
    if (gv) {
        size_t n = 0;
        {
            std::lock_guard _{ g_learn_mtx };
            read_learnt();
            n = g_learnt.size();
        }
        ImGui::TextDisabled("Grips learnt (magazines, handles): %d", int(n));
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget them##fingergameforget")) cmds += "fingergameforget\n";
    }
    ImGui::TextDisabled("Hand draws posed by name: %d, by shape: %d", g_known.load(), g_unknown.load());
}
}
