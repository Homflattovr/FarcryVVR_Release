// drivehands hands|all|off (saved, default hands; F11 Layout > Vehicles; author 5 Nov 22:47). See FarCry5Drive.h.
//
// Why they were gone: while driving there is no first-person pass; the game draws the player's arms on the wheel, bars or stick in the world
// pass, at the player's own root, so hidebody (world_character_draw) dropped them with the rest of the body.
//
// What this does, for each such draw while driving or piloting (settled, motion controls, no menu, flat screen, cinematic or fall):
//  - The draw is looked up by its index count in the game's first-person models (FarCry5BoneNames.h, fc5::bonemap::find): bare arms,
//    gloves and hand wear, sleeves and outfits. Indirect draws (the gloves) get their count from their arguments (indirect_args).
//  - Indirect draws (in a vehicle: one bare arm each, 14400 indices with the padding; no bone-table model fits them) are folded from their
//    own mesh as barehands folds the bare arm on foot (glove_mesh, bare_hand_fold): the forearm onto the wrist; a pair of gloves stays whole.
//    Models that share a count (the left and right bare arms, 4110; the DragonOmega and ThinShort01 gloves, both 14400 once padded) are
//    told apart by the highest palette slot the draw's vertices use, read once from its vertex buffer for each candidate's own index count
//    (as bonemap does); until those reads are in, such a draw stays hidden. (Builder 6 Nov: the left arm read with the right arm's table,
//    then the ThinShort01 gloves with DragonOmega's: a thin strip ran from each hand.)
//  - Its palette (VS b3, the CPU copy note_object_constants keeps) is copied; every slot that is not the hand or a finger (group 2) becomes a
//    zero-size matrix at its side's hand joint (the "LeftHand" / "RightHand" slot, else that side's first hand slot), so the upper arm, the
//    shoulders and the body shrink to a point at the hand and the forearm's skin closes onto the wrist. The hand and fingers keep the game's
//    own matrices: they sit where the game animates them, on the vehicle's own wheel or stick. The copy is bound with hands_cb_swap (b3 and
//    b4, as the floating hands), restored after the draw by the draw hooks.
//  - Not in the table, no hand slot, or no palette copy: hidden as before. Each new draw is logged once per drive ([VehHands]).
//  - drivehands all: every such draw as the game draws it (to compare; the head and body may fill the view). off: as before.
//  - drivehandsgrab on (the default; veh_hands_place at the end of this file): each hand then moved as one piece onto its own controller, the
//    weapon hand holding the recorded sidearm, or kept on the wheel, bars or stick where its grip took it (author 6 Nov 12:46).
//  - drivehandsspace auto (the default; author 7 Nov 12:21): the bare arm (indirect
//    draw) was moved as if its palette were in the space of its VS b1, as the ordinary draws' are. In the builder's 6 Nov night picture
//    (01_Compact_Civilian_turn) its left hand floated in the sky, as the author saw one ahead of the windshield on 6 Nov 14:59, while the arm
//    hair (an ordinary draw) followed the controllers; since patch 7 the hair takes the skin's move, so neither showed. Each drive the skin's
//    palm is now set against the hair's hand (space_check) and the space that puts it there is used.
//    the author's 13:59-14:01 drives (D2DEA5E8) found none: the skin's palm was 43-63 cm from the hair's hand in every space, because it was taken
//    from slot 0, the root (patch 13: the palm from the hand's own slots, slot 0 moved with it; see the bare arm's grab below).
#include "FarCry5Stereo.h"
#include "FarCry5SteerTune.h"
#include "FarCry5BoneMap.h"
#include "FarCry5Drive.h"
#include "FarCry5Record.h"
#include "FarCry5VehWheelTable.h"
#include "ModPath.h"

#include <spdlog/spdlog.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using ScanKey = std::tuple<void*, void*, UINT, UINT, INT>;   // index buffer, vertex buffer, count, start, base

    // vehwheels (author 8 Nov 10:35):
    // each vehicle's learnt wheel (drivehandsmodel) kept by its model, so the drive's ring is on that vehicle's own wheel from the moment
    // you sit, before any turn. A vehicle is told by its body draw's index count and the driver's seat on it (vehseathold: the camera in
    // the model's axes; within 8 cm): many bodies share 65535 indices, their seats differ. The wheel is kept in the recentred reference
    // (game metres: the game camera at the head, the vehicle's front), which comes back the same each time one sits in that seat (the author's
    // four drives of 8 Nov 11:02-11:27 in the same pickup: centres within 4 cm). Built-in entries (FarCry5VehWheelTable.h, filled from a
    // simulator pass over every vehicle with tools\wheelglue\gen_vehwheels.py), then bin\fc5vr_vehwheels.txt (learnt in play; one line
    // per vehicle: wheel count seat.xyz centre.xyz axis.xyz radius votes), which wins over a built-in one for the same vehicle.
    struct VehWheel
    {
        UINT      count{ 0 };
        glm::vec3 seat{ 0.0f }, c{ 0.0f }, n{ 0.0f, 0.0f, 1.0f };
        float     r{ 0.0f };
        int       votes{ 0 };
        bool      builtin{ false };
        int       style{ 0 };   // 0 wheel, 1 handlebars (c: their centre, r: half width), 2 stick (c: the grip) (infer_table.py, author 9 Nov 07:57)
    };
    std::vector<VehWheel> g_vw;
    bool                  g_vw_loaded{ false };

    bool vw_sane(const VehWheel& w)
    {
        if (w.style != 0)   // handlebars / stick: the centre (and the bars' half width) only
            return w.count > 0 && std::isfinite(w.c.x + w.c.y + w.c.z + w.r + w.seat.x + w.seat.y + w.seat.z) && glm::length(w.c) < 1.2f &&
                   (w.style == 2 || (w.r > 0.08f && w.r < 0.6f));
        return w.count > 0 && std::isfinite(w.c.x + w.c.y + w.c.z + w.n.x + w.n.y + w.n.z + w.r + w.seat.x + w.seat.y + w.seat.z) && w.r > 0.08f && w.r < 0.4f &&
               glm::length(w.c) < 1.2f && std::abs(glm::length(w.n) - 1.0f) < 0.05f;
    }
    int vw_find(UINT count, const glm::vec3& seat, int style = 0)
    {
        int   best = -1;
        float bd   = 0.08f;
        for (int i = 0; i < int(g_vw.size()); ++i) {
            if (g_vw[i].count != count || g_vw[i].style != style) continue;
            const float d = glm::length(g_vw[i].seat - seat);
            if (d < bd || (d == bd && best >= 0 && !g_vw[i].builtin)) bd = d, best = i;
        }
        return best;
    }
    void vw_load()
    {
        if (g_vw_loaded) return;
        g_vw_loaded = true;
        for (const auto& e : fc5::vehwheel::kBuiltin) {
            VehWheel w;
            w.count   = e.count;
            w.seat    = { e.seat[0], e.seat[1], e.seat[2] };
            w.c       = { e.c[0], e.c[1], e.c[2] };
            w.n       = { e.n[0], e.n[1], e.n[2] };
            w.r       = e.r;
            w.votes   = 99;
            w.builtin = true;
            w.style   = e.style;
            if (vw_sane(w)) g_vw.push_back(w);
        }
        const size_t  nb = g_vw.size();
        std::ifstream f{ fc5::mod_file("fc5vr_vehwheels.txt") };
        std::string   line;
        int           n = 0;
        while (std::getline(f, line)) {
            if (line.rfind("wheel ", 0) != 0) continue;
            std::istringstream ls{ line.substr(6) };
            VehWheel           w;
            if (!(ls >> w.count >> w.seat.x >> w.seat.y >> w.seat.z >> w.c.x >> w.c.y >> w.c.z >> w.n.x >> w.n.y >> w.n.z >> w.r >> w.votes) || !vw_sane(w)) continue;
            const int i = vw_find(w.count, w.seat);
            if (i >= 0 && glm::length(g_vw[i].seat - w.seat) < 0.02f) g_vw[i] = w;   // the same vehicle: the learnt one wins
            else g_vw.push_back(w);
            ++n;
        }
        spdlog::info("[VehHands] vehwheels: {} vehicles' wheels built in, {} read from fc5vr_vehwheels.txt", nb, n);
    }
    void vw_save()
    {
        std::ofstream f{ fc5::mod_file("fc5vr_vehwheels.txt"), std::ios::trunc };
        f << "# Far Cry 5 VR: each vehicle's steering wheel, learnt in play (drivehandsmodel; FarCry5VehHands.cpp, vehwheels). One line per vehicle:\n"
             "# wheel <body index count> <seat x y z (m, the camera in the model's axes)> <centre x y z> <axis x y z> <grip radius> <turns seen>\n"
             "# (centre, axis, radius: the recentred reference in game metres: x right, y up, z back from the head). Delete a line to learn it again.\n";
        int n = 0;
        for (const auto& w : g_vw) {
            if (w.builtin || w.style != 0) continue;
            f << fmt::format("wheel {} {:.3f} {:.3f} {:.3f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {}\n", w.count, w.seat.x, w.seat.y, w.seat.z, w.c.x, w.c.y, w.c.z, w.n.x,
                             w.n.y, w.n.z, w.r, w.votes);
            ++n;
        }
        spdlog::info("[VehHands] vehwheels: {} vehicles' wheels saved to fc5vr_vehwheels.txt", n);
    }
}

struct FarCry5Stereo::VehHandsState
{
    int                frame_seen{ -1000 };   // the last frame a draw came here (a gap of 2 s starts a new drive's log)
    std::set<uint64_t> said;                  // (count, outcome) logged this drive
    uint64_t           shown{ 0 }, hidden{ 0 }, drives{ 0 };
    bool               keep_bones_was{ false };
    std::vector<float> pal;                   // the palette being built (render thread)
    struct Scan
    {
        int                      max_bone{ -1 };   // the highest palette slot any vertex uses (any of its 4 weights)
        std::map<int, glm::vec3> cent;             // slot (a vertex's heaviest) -> its vertices' centre, in the vertex buffer's own units
    };
    std::map<ScanKey, Scan> max_bone;         // each draw's read, this drive
    std::set<ScanKey>      failed;
    int                    scan_next{ 0 };
    // drivehandsgrab (veh_hands_place). Poses are 4x4 (a hand pose: the palm slot's skinning, rest pose -> space; or the rest-pose hand axes
    // at its centre), in the recentred reference (the controllers' space: it stays with the seat, so a pose kept there stays on the vehicle).
    struct Side
    {
        bool        on{ false };                  // stuck to the control the gesture drive holds with this hand
        glm::mat4   h0{ 1.0f };                   // its pose there at the take (reference)
        float       a0{ 0.0f };                   // the control's turn then (rad)
        glm::vec3   top0{ 0.0f };                 // stick: its top then (reference)
        glm::vec4   lc{ 0.0f, 0.0f, 0.0f, 1.0f }; // the hand's centre in the pose's own axes (a pose's centre: pose * lc)
        glm::mat4   t{ 1.0f };                    // path B's move of this side (world) at t_frame, for path A's meshes of that side
        bool        t_hide{ false };
        int         t_frame{ -1000 };
        uint64_t    grabs{ 0 };
        int         lines{ 0 };                   // log lines this drive (takes, let-gos, state changes)
        std::string state;                        // the last state logged
        bool        far_said{ false };            // the recorded hold left out (its palm too far from the controller), logged
        // drivehandsmodel: glued to the vehicle's own control. Bars and stick ride a game hand (ref: which side's; k: the hand's pose in that
        // game hand's frame; g0: that game hand at the start, for the log). Wheel: k is the hand's pose at the glue and th0 the wheel model's
        // turn then. h_last: the pose last drawn, kept while no game hand is seen.
        bool        glued{ false };
        int         ref{ -1 };
        float       th0{ 0.0f };                  // the wheel model's turn (VehHandsState::theta) at the glue
        glm::mat4   k{ 1.0f };
        glm::mat4   g0{ 1.0f };
        glm::mat4   h_last{ 1.0f };
        int         glue_log_frame{ -1000 };
    };
    Side        side[2];
    std::set<std::pair<void*, UINT>> live_said;   // drivehandsdirect: the arguments read live and logged this drive (buffer, offset)
    // The weapon hand's recorded hold this drive (its palm slot's skinning, rest pose -> the controller's axes) and that palm's rest-pose centre,
    // for the other hand's mirror image.
    glm::mat4 rel{ 1.0f };
    glm::vec3 rel_c{ 0.0f };
    bool      rel_ok{ false };
    bool      mirror_said{ false };
    // drivehandsspace (veh_hands_place, space_check): which space the bare arm's (indirect draw's) palette puts its hand in, found by setting its
    // palm against the arm hair's hand of the same side in the same frame (the hair is an ordinary draw, placed right by its own VS b1:
    // it followed the controllers in patch 6, when the skin did not). Kinds: 0 the skin draw's own VS b1 (what patches 6-10 assumed),
    // 1 the hair draw's VS b1, 2 world (no object transform), 3 world less the game camera, 4 the game camera's view axes.
    int       drive_frame{ 0 };                 // this drive's first frame
    glm::vec3 skin_p[2]{};                      // the skin's palm, palette space, before the move (path B)
    glm::mat4 skin_om[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    int       skin_frame[2]{ -1000, -1000 };
    glm::vec3 hair_hw[2]{};                     // the hair's hand, world, before the move (path A)
    int       hair_frame[2]{ -1000, -1000 };
    glm::mat4 hair_om{ 1.0f };                  // the hair draw's VS b1 (kind 1)
    int       checked_frame{ -1000 };
    int       space{ -1 };                      // the kind found this drive (-1 not yet)
    int       cand{ -1 }, cand_n{ 0 };          // the kind winning the last checks, how many times in a row
    int       space_lines{ 0 };
    bool      fallback_said{ false };

    // drivehandsmodel (veh_hands_place): the game's own hands this drive, each as a rigid frame (its palm's turn made orthonormal, at the
    // hand's centre) in the recentred reference (game metres), before the mod moves them; what they say of the vehicle's control.
    struct GameHand
    {
        glm::mat4 x{ 1.0f };
        int       frame{ -1000 };
    };
    static constexpr int kHist = 64;
    GameHand gh[2];
    GameHand hist[2][kHist];   // each side's last frames (by frame % kHist)
    bool fresh(int s, int frame) const { return frame - gh[s].frame <= 1; }

    // The car's wheel, kept in the reference itself (it is fixed in the car, as the eyes are). Round 2 (simulator run of 8 Nov 03:10): the
    // game's two hands do not both ride its wheel. In a full lock the left hand turned 68 / 93 deg with the wheel model while the right one
    // stayed where it was (the wheel slides under it), so a hand riding the game's right hand never turned, and a hand riding the left one
    // swung off the rim (it was 31 cm from it at the take). The wheel is learnt from a game hand's rigid turn of 30-150 deg (8-29 deg turns
    // gave centres 10-40 cm apart in that run), taken only if it looks like a steering wheel (in front of the eyes, its face to the driver,
    // 12-32 cm grip radius, no move along the axis), and adopted once a second such turn 10 frames later agrees (6 cm, 12 deg). Its turn
    // (theta) is then summed frame by frame from the game hand that holds it (on its rim, its palm turning about the axis as far as it goes
    // round; of the two, the one that has turned the most lately: the other slides or rests), and a held VR hand is turned by it about the
    // wheel's axis from where it took the rim.
    struct Wheel
    {
        glm::vec3 c{ 0.0f };
        glm::vec3 n{ 0.0f, 0.0f, 1.0f };   // towards the driver; a positive turn about it is to the left
        float     r{ 0.0f };               // the game hands' grip radius
        int       votes{ 0 };
    };
    Wheel     wl;                      // adopted at votes >= 2
    Wheel     wcand;                   // the last candidate (votes 1)
    int       wcand_frame{ -1000 };
    float     theta{ 0.0f };           // the wheel model's turn since the drive began (rad)
    float     dphi[2]{};               // each game hand's turn about the wheel's axis since the last frame, while it turns with the wheel
    int       dphi_frame[2]{ -1000, -1000 };
    float     score[2]{};              // how much it has turned with the wheel lately
    int       holder{ -1 };            // the game hand the wheel's turn was last taken from
    int       step_frame{ -1000 };
    int       stick_hand{ -1 };        // the game hand on the stick: the one nearer the drive's stick (helicopters: the left, at the cyclic)
    int       model_lines{ 0 };
    int       learn_lines{ 0 };
    int       turn_log_frame{ -1000 };
    // vehwheels: this vehicle (its model and seat, from vehseathold), whether its wheel came from the table, and a wheel to save.
    bool      key_tried{ false };
    bool      key_ok{ false };
    UINT      key_count{ 0 };
    glm::vec3 key_seat{ 0.0f };
    bool      from_table{ false };
    bool      dirty{ false };
    int       tbl_style{ -1 };                          // the vehicle's handlebars or stick from the table (drive units: recentred metres)
    glm::vec3 tbl_c{ 0.0f };
    float     tbl_r{ 0.0f };
    bool wheel_ok() const { return wl.votes >= 2; }

    void reset_model()
    {
        for (int s = 0; s < 2; ++s) {
            gh[s] = GameHand{};
            for (auto& h : hist[s]) h = GameHand{};
            dphi_frame[s] = -1000;
            score[s]      = 0.0f;
        }
        wl = wcand  = Wheel{};
        wcand_frame = step_frame = turn_log_frame = -1000;
        theta       = 0.0f;
        stick_hand  = -1;
        holder      = -1;
        key_tried = key_ok = from_table = dirty = false;
        tbl_style = -1;
        key_count = 0;
        model_lines = learn_lines = 0;
    }

    // The vehicle's control from the game's hands, handed to the drive (its recentred reference: game metres / world scale).
    //  - wheel: only once learnt (above): its centre, axis and the game hands' grip radius; before that the drive keeps its own place (the
    //    centre between the two hands, used in round 1, put the ring too low and too large: the game's hands are not at 9 and 3 o'clock);
    //  - bars: the centre between the hands, half width half the line (the jetski's and the tractor's bars: the hands rode them within 1 cm);
    //  - stick: the game hand nearer the drive's stick (stick_hint), if within 45 cm (in the helicopters the game's right hand is 82 cm under
    //    the eyes and 63 cm from the drive's stick: not on the cyclic).
    void send_model(int style, int frame, float ws, const glm::vec3& stick_hint)
    {
        fc5::drive::Model m{};
        m.style             = style;
        const bool      f[2]{ fresh(0, frame), fresh(1, frame) };
        const glm::vec3 c[2]{ glm::vec3{ gh[0].x[3] }, glm::vec3{ gh[1].x[3] } };
        std::string     note;
        if (style == fc5::drive::kStick) {
            int   g = -1;
            float d = 1e9f;
            for (int s = 0; s < 2; ++s) {
                if (!f[s]) continue;
                const float ds = glm::length(c[s] - stick_hint);
                if (ds < d) d = ds, g = s;
            }
            if (g < 0) return;
            if (d > 0.45f * ws) {
                if (model_lines < 3) {
                    model_lines++;
                    spdlog::info("[VehHands] drivehandsmodel: no game hand on the stick: the nearer ({}) is {:.0f} cm from the drive's stick (right hand ({:+.0f}, {:+.0f}, "
                                 "{:+.0f}) cm, left ({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes); the drive's stick stays where it is",
                                 g == 0 ? "right" : "left", d / ws * 100.0f, c[0].x / ws * 100.0f, c[0].y / ws * 100.0f, c[0].z / ws * 100.0f,
                                 c[1].x / ws * 100.0f, c[1].y / ws * 100.0f, c[1].z / ws * 100.0f);
                }
                stick_hand = -1;
                return;
            }
            stick_hand = g;
            m.centre   = c[g];
            note       = fmt::format(" (the game's {} hand, {:.0f} cm from the drive's stick; the other hand {})", g == 0 ? "right" : "left", d / ws * 100.0f,
                                     f[1 - g] ? fmt::format("{:.0f} cm", glm::length(c[1 - g] - stick_hint) / ws * 100.0f) : std::string{ "not seen" });
        } else if (style == fc5::drive::kBars) {
            if (!f[0] || !f[1]) return;
            m.centre = 0.5f * (c[0] + c[1]);
            m.radius = 0.5f * glm::length(c[0] - c[1]);
            if (m.radius < 0.12f * ws || m.radius > 0.6f * ws) return;
            note = fmt::format(", half width {:.0f} cm", m.radius / ws * 100.0f);
        } else if (style == fc5::drive::kWheel) {
            if (!wheel_ok()) return;
            m.centre      = wl.c;
            m.n           = wl.n;
            m.radius      = std::clamp(wl.r, 0.12f * ws, 0.30f * ws);
            m.axis_learnt = true;
            note          = fmt::format(", radius {:.0f} cm, learnt from the game hand's turn", m.radius / ws * 100.0f);
        } else {
            return;
        }
        if (!std::isfinite(m.centre.x + m.centre.y + m.centre.z + m.n.x + m.n.y + m.n.z) || glm::length(m.centre) > 1.5f * ws) return;
        if (model_lines < 3) {
            model_lines++;
            spdlog::info("[VehHands] drivehandsmodel: the vehicle's {} from the game's hands: ({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes (right, up, back){}",
                         style == fc5::drive::kWheel ? "wheel" : style == fc5::drive::kBars ? "handlebars" : "stick", m.centre.x / ws * 100.0f,
                         m.centre.y / ws * 100.0f, m.centre.z / ws * 100.0f, note);
        }
        m.centre /= ws;
        m.radius /= ws;
        fc5::drive::set_model(m);
    }

    void offer_wheel(const Wheel& k, int frame, float ws, int s, float phi)
    {
        auto agree = [&](const Wheel& a, float dc, float da) {
            return glm::length(a.c - k.c) < dc * ws && glm::dot(a.n, k.n) > std::cos(glm::radians(da));
        };
        if (wheel_ok()) {   // refined by later turns that agree (saved again at 10 and 40 of them)
            if (agree(wl, 0.08f, 15.0f)) {
                wl.c = glm::mix(wl.c, k.c, 0.2f);
                wl.n = glm::normalize(glm::mix(wl.n, k.n, 0.2f));
                wl.r = glm::mix(wl.r, k.r, 0.2f);
                wl.votes++;
                if (wl.votes == 10 || wl.votes == 40) dirty = true;
                return;
            }
            // A wheel from the table that two turns 10 frames apart put elsewhere (the vehicle told wrongly, or the table off): learnt again.
            if (!from_table) return;
            if (wcand.votes == 1 && agree(wcand, 0.06f, 12.0f)) {
                if (frame - wcand_frame < 10) return;
                const float off = glm::length(wl.c - 0.5f * (wcand.c + k.c)) / ws * 100.0f;
                wl.c       = 0.5f * (wcand.c + k.c);
                wl.n       = glm::normalize(wcand.n + k.n);
                wl.r       = 0.5f * (wcand.r + k.r);
                wl.votes   = 2;
                from_table = false;
                dirty      = true;
                spdlog::info("[VehHands] drivehandsmodel: the table's wheel for this vehicle was {:.0f} cm off the one the game's {} hand turned: learnt again, centre "
                             "({:+.0f}, {:+.0f}, {:+.0f}) cm, grip {:.0f} cm", off, s == 0 ? "right" : "left", wl.c.x / ws * 100.0f, wl.c.y / ws * 100.0f,
                             wl.c.z / ws * 100.0f, wl.r / ws * 100.0f);
                return;
            }
            wcand       = k;
            wcand.votes = 1;
            wcand_frame = frame;
            return;
        }
        if (wcand.votes == 1 && agree(wcand, 0.06f, 12.0f)) {
            if (frame - wcand_frame < 10) return;
            wl.c     = 0.5f * (wcand.c + k.c);
            wl.n     = glm::normalize(wcand.n + k.n);
            wl.r     = 0.5f * (wcand.r + k.r);
            wl.votes = 2;
            dirty    = true;
            if (learn_lines < 6) {
                learn_lines++;
                spdlog::info("[VehHands] drivehandsmodel: the wheel learnt from the game's {} hand turning {:.0f} deg (two turns {} frames apart agree): its centre "
                             "({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes (right, up, back), the hands grip it {:.0f} cm from the axis, its face tilted {:.0f} deg up",
                             s == 0 ? "right" : "left", glm::degrees(phi), frame - wcand_frame, wl.c.x / ws * 100.0f, wl.c.y / ws * 100.0f, wl.c.z / ws * 100.0f,
                             wl.r / ws * 100.0f, glm::degrees(std::asin(std::clamp(wl.n.y, -1.0f, 1.0f))));
            }
            return;
        }
        wcand       = k;
        wcand.votes = 1;
        wcand_frame = frame;
    }

    void game_hand(int s, const glm::mat4& x, int frame, float ws, int style)
    {
        gh[s].x     = x;
        gh[s].frame = frame;
        auto& h     = hist[s][frame % kHist];
        h.x         = x;
        h.frame     = frame;
        if (style != fc5::drive::kWheel) return;   // the wheel only (a helicopter's hands learnt "wheels" 76 cm under the eyes in round 1)
        const glm::mat3 Rn{ x };
        const glm::vec3 cn{ x[3] };
        // Learn the wheel from this hand's turn since an older frame (the oldest that turned enough).
        for (int back = kHist - 4; back >= 6; --back) {
            const auto& o = hist[s][(frame - back) % kHist];
            if (o.frame != frame - back) continue;
            const glm::mat3 Rr = Rn * glm::transpose(glm::mat3{ o.x });
            glm::quat       q  = glm::quat_cast(Rr);
            if (q.w < 0.0f) q = -q;
            const float phi = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
            if (phi < glm::radians(30.0f) || phi > glm::radians(150.0f)) continue;
            const glm::vec3 a  = glm::normalize(glm::vec3{ q.x, q.y, q.z });
            const glm::vec3 co{ o.x[3] };
            const glm::vec3 d  = cn - co;
            if (std::abs(glm::dot(d, a)) > 0.02f * ws) break;   // not a turn about a fixed axis (the hand moved along it)
            const glm::vec3 dp = d - a * glm::dot(d, a);
            const float     dl = glm::length(dp);
            if (dl < 1e-4f) break;
            const glm::vec3 w  = glm::normalize(glm::cross(a, dp));
            const glm::vec3 p  = 0.5f * (cn + co) + w * (0.5f * dl / std::tan(0.5f * phi));
            const glm::vec3 r  = (cn - p) - a * glm::dot(cn - p, a);
            const float     rr = glm::length(r);
            glm::vec3       aa = a;
            if (glm::dot(aa, -p) < 0.0f) aa = -aa;   // the axis towards the driver (the eyes are the reference's origin)
            const float pl = glm::length(p);
            // A steering wheel: 12-32 cm grip radius, 25-85 cm from the eyes and in front of them, its face to the driver (tilted under 60 deg).
            if (rr < 0.12f * ws || rr > 0.32f * ws || pl < 0.25f * ws || pl > 0.85f * ws || p.z > -0.15f * ws || aa.z < 0.5f) break;
            offer_wheel(Wheel{ p, aa, rr, 0 }, frame, ws, s, phi);
            break;
        }
        // On the learnt wheel's rim and turning rigidly with it over the last 10 frames (its palm turns about the wheel's axis as far as it
        // goes round the rim, with little turn about any other axis): its palm's turn about the axis since the last frame counts.
        if (!wheel_ok()) return;
        const auto& o1 = hist[s][(frame - 1) % kHist];
        const auto& ow = hist[s][(frame - 10) % kHist];
        if (o1.frame != frame - 1 || ow.frame != frame - 10) {
            score[s] *= 0.9f;
            return;
        }
        auto on_rim = [&](const glm::vec3& p, glm::vec3& rp) {
            const glm::vec3 rv = p - wl.c;
            const float     ax = glm::dot(rv, wl.n);
            rp                 = rv - wl.n * ax;
            return std::abs(ax) < 0.04f * ws && std::abs(glm::length(rp) - wl.r) < 0.05f * ws;
        };
        auto twist = [&](const glm::mat4& A, const glm::mat4& B, float& swing) {
            glm::quat q = glm::quat_cast(glm::mat3{ A } * glm::transpose(glm::mat3{ B }));
            if (q.w < 0.0f) q = -q;
            const float tw  = 2.0f * std::atan2(glm::dot(glm::vec3{ q.x, q.y, q.z }, wl.n), q.w);
            const float tot = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
            swing           = std::sqrt(std::max(0.0f, tot * tot - tw * tw));
            return tw;
        };
        glm::vec3   rp_now{ 0.0f }, rp_w{ 0.0f };
        float       sw_w = 0.0f, sw_1 = 0.0f;
        const float tw_w = twist(x, ow.x, sw_w);
        const float tw_1 = twist(x, o1.x, sw_1);
        bool        ok   = on_rim(cn, rp_now) && on_rim(glm::vec3{ ow.x[3] }, rp_w);
        if (ok) {
            const float pos_w = std::atan2(glm::dot(wl.n, glm::cross(rp_w, rp_now)), glm::dot(rp_w, rp_now));
            ok = sw_w < glm::radians(4.0f) + 0.15f * std::abs(tw_w) && std::abs(pos_w - tw_w) < glm::radians(4.0f) + 0.2f * std::abs(tw_w) &&
                 std::abs(tw_1) < glm::radians(20.0f);
        }
        if (ok) {
            dphi[s]       = tw_1;
            dphi_frame[s] = frame;
            score[s]      = 0.97f * score[s] + std::abs(tw_1);
        } else {
            score[s] *= 0.9f;
        }
    }

    // Once per frame, for the frame just ended: the wheel turned as far as the game hand that holds it (of those turning rigidly with it, the
    // one that has turned the most lately: the other slides or rests on the rim).
    void wheel_step(int f)
    {
        if (step_frame == f) return;
        step_frame = f;
        int g      = -1;
        for (int s = 0; s < 2; ++s)
            if (dphi_frame[s] == f && (g < 0 || score[s] > score[g])) g = s;
        if (g >= 0) theta += dphi[g];
        holder = g;
    }

    static glm::mat4 space_matrix(int k, const glm::mat4& om_skin, const glm::mat4& om_hair, const glm::vec3& cam, const glm::mat3& game_rot)
    {
        switch (k) {
        case 1: return om_hair;
        case 2: return glm::mat4{ 1.0f };
        case 3: return glm::translate(glm::mat4{ 1.0f }, cam);
        case 4: { glm::mat4 m{ glm::transpose(game_rot) }; m[3] = glm::vec4{ cam, 1.0f }; return m; }   // view -> world (game_rot: world -> view)
        default: return om_skin;
        }
    }
    // Both draws' hands of one side seen this frame: each kind's place for the skin's palm against the hair's hand. A kind is taken after it
    // wins 3 checks in a row: within 20 cm, and no kind that puts the palm elsewhere (over 5 cm from it) within 15 cm of it.
    void space_check(int frame, const glm::vec3& cam, const glm::mat3& game_rot)
    {
        if (checked_frame == frame) return;
        for (int s = 0; s < 2; ++s) {
            if (skin_frame[s] != frame || hair_frame[s] != frame) continue;
            checked_frame = frame;
            static const char* kName[5]{ "its own object transform (VS b1)", "the arm hair's object transform", "world", "world less the game camera", "the game camera's view" };
            glm::vec3 pred[5];
            float     d[5];
            int       best = 0;
            for (int k = 0; k < 5; ++k) {
                pred[k] = glm::vec3{ space_matrix(k, skin_om[s], hair_om, cam, game_rot) * glm::vec4{ skin_p[s], 1.0f } };
                d[k]    = glm::length(pred[k] - hair_hw[s]);
                if (d[k] < d[best]) best = k;
            }
            bool clear = d[best] < 0.20f;
            for (int k = 0; k < 5 && clear; ++k)
                if (k != best && glm::length(pred[k] - pred[best]) > 0.05f && d[k] < d[best] + 0.15f) clear = false;
            const int won = clear ? best : -1;
            cand_n        = won == cand ? cand_n + 1 : 1;
            cand          = won;
            const bool decide = won >= 0 && cand_n >= 3 && won != space;
            const bool none   = won < 0 && cand_n == 30 && space < 0;   // a second of checks with no clear kind: said once
            if ((decide || none) && space_lines < 8) {
                space_lines++;
                spdlog::info("[VehHands] drivehandsspace: the {} skin's palm from the arm hair's hand, for each kind: {:.0f} / {:.0f} / {:.0f} / {:.0f} / {:.0f} cm "
                             "(its own b1, the hair's b1, world, less the camera, view): {}",
                             s == 0 ? "right" : "left", d[0] * 100.0f, d[1] * 100.0f, d[2] * 100.0f, d[3] * 100.0f, d[4] * 100.0f,
                             decide ? fmt::format("taken: {}{}", kName[won], won == 0 ? " (expected since patch 13: the palm from the hand's own slots, not the root)" : "") : std::string{ "no clear kind: its own object transform kept" });
            }
            if (decide) space = won;
            return;
        }
    }
};

namespace
{
    int side_of(const char* name)   // 0 right, 1 left, -1 neither (bone names: LeftHand, RightInHandIndex, PHandw ... start with the side)
    {
        if (!name) return -1;
        if (std::strncmp(name, "Left", 4) == 0) return 1;
        if (std::strncmp(name, "Right", 5) == 0) return 0;
        return -1;
    }
    glm::mat4 from_rows12(const float* m);   // below
    bool is_main_hand(const char* name) { return name && (std::strcmp(name, "LeftHand") == 0 || std::strcmp(name, "RightHand") == 0); }

    // A copy of FarCry5BoneMap.cpp's helper: a GPU buffer read back (a staging copy; immediate context only).
    bool read_gpu(ID3D11DeviceContext* ctx, ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        if (!b) return false;
        D3D11_BUFFER_DESC d{};
        b->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ID3D11Buffer* st = nullptr;
        if (dev) { dev->CreateBuffer(&d, nullptr, &st); dev->Release(); }
        if (!st) return false;
        ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
            ctx->Unmap(st, 0);
        }
        st->Release();
        return ok;
    }
}

// The highest palette slot the draw's vertices use (any of their 4 weights), read once per draw and drive; -1 while not known.
int FarCry5Stereo::veh_hands_max_bone(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, const std::map<int, glm::vec3>** cent)
{
    if (cent) *cent = nullptr;
    auto&         st = *m_vh;
    ID3D11Buffer* vb = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ib_fmt{};
    UINT          ib_off = 0;
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    struct Rel { ID3D11Buffer* a; ID3D11Buffer* b; ~Rel() { if (a) a->Release(); if (b) b->Release(); } } rel{ vb, ib };
    const ScanKey key{ ib, vb, count, start, base };
    if (const auto it = st.max_bone.find(key); it != st.max_bone.end()) {
        if (cent) *cent = &it->second.cent;
        return it->second.max_bone;
    }
    const int frame = m_frame.load();
    if (!vb || !ib || st.failed.contains(key) || frame < st.scan_next || fc5::rec::is_deferred(ctx) || st.max_bone.size() + st.failed.size() > 64) return -1;
    st.scan_next = frame + 5;   // one read every 5 frames at most (each is a GPU read)
    ID3D11InputLayout* il = nullptr;
    ctx->IAGetInputLayout(&il);
    std::vector<LayoutElem> layout;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
    }
    if (il) il->Release();
    const LayoutElem *be = nullptr, *we = nullptr;
    for (const auto& e : layout) {
        if (e.index != 0 || e.slot != 0) continue;
        if (_stricmp(e.semantic.c_str(), "BLENDINDICES") == 0) be = &e;
        if (_stricmp(e.semantic.c_str(), "BLENDWEIGHT") == 0) we = &e;
    }
    const LayoutElem* pe = nullptr;
    for (const auto& e : layout)
        if (e.index == 0 && e.slot == 0 && e.offset == 0 && e.format == DXGI_FORMAT_R16G16B16A16_SINT && _stricmp(e.semantic.c_str(), "POSITION") == 0) pe = &e;
    std::vector<uint8_t>     vbd, ibd;
    int                      mb = -1, verts = 0;
    std::map<int, glm::vec3> sum;
    std::map<int, int>       num;
    if (be && be->offset + 4 <= stride && stride <= 128 && read_gpu(ctx, vb, vbd) && read_gpu(ctx, ib, ibd)) {
        const bool         i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
        std::set<uint32_t> seen;
        for (UINT k = 0; k < count; ++k) {
            const size_t io = size_t(ib_off) + size_t(start + k) * (i32 ? 4 : 2);
            if (io + (i32 ? 4 : 2) > ibd.size()) break;
            uint32_t idx = 0;
            std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
            if (!seen.insert(idx).second) continue;
            const size_t vo = size_t(voff) + size_t(int64_t(idx) + base) * stride;
            if (vo + stride > vbd.size()) continue;
            const uint8_t* v  = vbd.data() + vo;
            const uint8_t* bi = v + be->offset;
            verts++;
            int heavy = bi[0];
            if (we && we->offset + 4 <= stride) {
                const uint8_t* wp   = v + we->offset;
                const bool     bgra = we->format == DXGI_FORMAT_B8G8R8A8_UNORM;
                int            hw   = -1;
                for (int c = 0; c < 4; ++c) {
                    const int slot = bi[bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c];
                    if (wp[c] > 0) mb = std::max(mb, slot);
                    if (int(wp[c]) > hw) { hw = wp[c]; heavy = slot; }
                }
            } else {
                mb = std::max(mb, heavy);
            }
            if (pe) {
                int16_t q[3];
                std::memcpy(q, v, 6);
                sum[heavy] += glm::vec3{ float(q[0]), float(q[1]), float(q[2]) };
                num[heavy]++;
            }
        }
    }
    if (verts > 0 && mb >= 0) {
        auto& sc    = st.max_bone[key];
        sc.max_bone = mb;
        for (const auto& [b, v] : sum) sc.cent[b] = v / float(num[b]);
        if (cent) *cent = &sc.cent;
    } else {
        st.failed.insert(key);
    }
    return verts > 0 ? mb : -1;
}

bool FarCry5Stereo::veh_hands_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, ID3D11Buffer* palette_cb)
{
    const int mode = fc5::drive::hands_mode();
    if (mode == 0) return false;
    if (!m_motion.load() || !m_in_vehicle.load() || !m_veh_driving.load() || m_veh_settle.load() != 0 || m_chute_vehicle || m_cine_on || flat_screen_on()) return false;
    if (!m_vh) m_vh = new VehHandsState{};
    auto&     st    = *m_vh;
    const int frame = m_frame.load();
    if (frame - st.frame_seen > 180) {   // a new drive (or the first): log its draws again
        st.said.clear();
        st.max_bone.clear();   // the buffers may have been freed and their addresses reused since
        st.failed.clear();
        st.drives++;
        for (auto& sd : st.side) sd = VehHandsState::Side{};   // drivehandsgrab: every hand free again, its log anew
        st.live_said.clear();
        st.rel_ok      = false;
        st.mirror_said = false;
        // drivehandsspace: found again for each drive.
        st.drive_frame   = frame;
        st.skin_frame[0] = st.skin_frame[1] = st.hair_frame[0] = st.hair_frame[1] = -1000;
        st.checked_frame = -1000;
        st.space         = -1;
        st.cand          = -1;
        st.cand_n        = 0;
        st.space_lines   = 0;
        st.fallback_said = false;
        // drivehandsmodel: the game's hands and the wheel found again for each drive.
        st.reset_model();
        spdlog::info("[VehHands] driving: drivehands {} (the player's body draws below, each once)", mode == 2 ? "all (the whole body as the game draws it)" : "hands");
    }
    if (st.frame_seen != frame) st.wheel_step(st.frame_seen);   // drivehandsmodel: the wheel's turn in the frame just ended
    st.frame_seen = frame;
    // The palettes' CPU copies (note_object_constants) are needed from here on; the floating hands usually have them on already.
    if (!m_keep_bones.load()) {
        m_keep_bones = true;
        spdlog::info("[VehHands] keeping the skinning palettes' copies (keepbones) for the hands");
    }
    // Once per drive for each count, outcome and model (the two bare arms share a count).
    auto say = [&](uint32_t c, int outcome, const std::string& text, int model = -1) {
        if (st.said.size() < 64 && st.said.insert((uint64_t(model + 1) << 40) | (uint64_t(c) << 8) | uint64_t(outcome)).second) spdlog::info("[VehHands] draw {}: {}", c, text);
    };
    if (mode == 2) {
        st.shown++;
        say(count, 9, "drawn whole (drivehands all)");
        return true;
    }
    // An indirect draw (the gloves are one on foot) has no count here: its arguments give it (one read per arguments buffer and offset).
    UINT                   n        = count;
    const bool             indirect = count == 0;
    std::array<int32_t, 5> ia{};   // an indirect draw's arguments as read for it (cached: the first read)
    if (indirect) {
        if (m_hud_draw_kind == 5 && m_hud_draw_args && !fc5::rec::is_deferred(ctx) && indirect_args(ctx, m_hud_draw_args, m_hud_draw_args_off, ia) && ia[0] > 0) {
            n     = UINT(ia[0]);
            start = UINT(std::max(0, ia[2]));
            base  = INT(ia[3]);
        }
        if (n == 0) {
            st.hidden++;
            say(0, 1, "hidden: an indirect draw whose count could not be read");
            return false;
        }
    }
    // An indirect draw in a vehicle is one bare arm with its hand (builder 6 Nov, patch 4 run: two 14400-index draws, "[Hands] scan: one arm
    // with its hand (left)" / "(right)", 66-67 bones, eight influences each), a model none of the bone table's 14400 candidates fits (the
    // DragonOmega gloves' table was taken: its joint lay 126 cm from the hand, and the strips stayed). Such a draw is folded from its own
    // mesh, as barehands folds the bare arm on foot: the slots nearer the body than 0.76 of the arm's reach go to the wrist (bare_hand_fold).
    // A pair of gloves is drawn as the game draws it. Either way the slots no vertex of the mesh uses are zeroed (the padding's triangles).
    if (indirect) {
        const GloveMesh* gm = fc5::rec::is_deferred(ctx) ? nullptr : glove_mesh(ctx, n, start, base);
        if (!gm || !gm->ok || (!gm->gloves && gm->arm_side < 0)) {
            st.hidden++;
            say(n, 10, gm && gm->ok ? fmt::format("hidden: its mesh is neither one arm with its hand nor a pair of gloves ({} bones)", gm->bones)
                                    : std::string{ "hidden: its mesh could not be read" });
            return false;
        }
        {
            std::lock_guard _{ m_obj_mtx };
            auto            it = palette_cb ? m_bone_data.find(palette_cb) : m_bone_data.end();
            if (it == m_bone_data.end() || it->second.size() < 3072) {
                st.hidden++;
                say(n, 4, "hidden: no copy of its palette yet");
                return false;
            }
            st.pal = it->second;
        }
        float scale = 1.13f / 32767.0f;   // barehands' default; VS b2's when it holds one
        {
            ID3D11Buffer* cb2 = nullptr;
            ctx->VSGetConstantBuffers(2, 1, &cb2);
            if (cb2) {
                std::lock_guard _{ m_obj_mtx };
                if (auto it = m_obj_consts.find(cb2); it != m_obj_consts.end() && it->second.f[5] > 1e-6f && it->second.f[5] < 1e-3f) scale = it->second.f[5];
            }
            if (cb2) cb2->Release();
        }
        const uint64_t folded = m_bare_folded;
        if (gm->arm_side >= 0) bare_hand_fold(st.pal, *gm, scale);
        int unused = 0;
        for (int b = 0; b < 256 && size_t(b + 1) * 12 <= st.pal.size(); ++b) {
            if (b < int(gm->cent.size()) && gm->cent[b].x > 0.0f) continue;
            std::fill(st.pal.begin() + size_t(b) * 12, st.pal.begin() + size_t(b + 1) * 12, 0.0f);
            unused++;
        }
        // drivehandsgrab: the hand onto its controller or the wheel. One arm: every slot its vertices use is that side's; a pair of gloves: by the
        // rest pose's side. Each side's palm is its heaviest hand slot but slot 0 (rest-pose |x| from hand_from), its centre the hand slots'
        // vertices' centre, each posed by its own slot (as barehands' hand_c on foot); the forearm's direction (hand slots' centre from the
        // forearm's, |x| from 0.55 of the reach) checks the rest pose against the table's.
        // Slot 0 (patch 13; author 7 Nov 13:06 video, the bare hand up at the roof, palm up): the bare arm's heaviest slot (the author's right
        // arm, 2793 vertices: weight 679, its vertices' rest-pose centre in the hand at |x| 14239 of 15264) is the root, which barehands on foot
        // and fingergrip already leave out of the palm. Patches 6-11 took it as the palm: in a vehicle it poses the hand's rest-pose point where
        // the arm held straight out would put it, 43-63 cm from the arm hair's hand (drivehandsspace 13:59-14:01, the same in each of the five
        // spaces), so the move put that point on the controller and the real hand 45 cm off with the root's axes (palm up). Slot 0 now also
        // takes its side's palm matrix before the move, so the wrist vertices skinned to it go with the hand instead of stretching to that point.
        {
            int8_t    sides[256];
            int       hpalm[2]{ -1, -1 };
            float     pw[2]{}, hw[2]{}, fw[2]{}, cw[2]{};
            glm::vec3 hs[2]{}, fs[2]{}, piv[2]{}, fore[2]{}, cs[2]{};
            std::fill(std::begin(sides), std::end(sides), int8_t(-1));
            const float reach_raw = gm->hand_from > 0.0f ? gm->hand_from / 0.76f : 0.0f;
            for (int b = 0; b < 256 && b < int(gm->cent.size()); ++b) {
                const glm::vec4& c = gm->cent[b];
                if (c.x <= 0.0f) continue;
                const int sd = gm->arm_side >= 0 ? gm->arm_side : (c.y > 0.0f ? 0 : 1);
                sides[b]     = int8_t(sd);
                const glm::vec3 rp{ c.y, c.z, c.w };
                if (std::abs(c.y) >= gm->hand_from) {
                    if (b == 0) continue;   // the root (see above): not the palm, nor in the hand's centre
                    if (c.x > pw[sd]) {
                        pw[sd]    = c.x;
                        hpalm[sd] = b;
                    }
                    hs[sd] += c.x * rp;
                    hw[sd] += c.x;
                    if (c.x >= 5.0f && size_t(b + 1) * 12 <= st.pal.size()) {   // posed by its own slot (as hand_c on foot)
                        const float*    m = st.pal.data() + size_t(b) * 12;
                        const glm::vec3 r = rp * scale;
                        cs[sd] += c.x * glm::vec3{ m[0] * r.x + m[1] * r.y + m[2] * r.z + m[3], m[4] * r.x + m[5] * r.y + m[6] * r.z + m[7], m[8] * r.x + m[9] * r.y + m[10] * r.z + m[11] };
                        cw[sd] += c.x;
                    }
                } else if (std::abs(c.y) >= 0.55f * reach_raw) {
                    fs[sd] += c.x * rp;
                    fw[sd] += c.x;
                }
            }
            for (int sd = 0; sd < 2; ++sd) {
                if (hpalm[sd] < 0 || size_t(hpalm[sd] + 1) * 12 > st.pal.size()) {
                    hpalm[sd] = -1;
                    continue;
                }
                const glm::vec4& c  = gm->cent[hpalm[sd]];
                const glm::vec4  rp{ c.y * scale, c.z * scale, c.w * scale, 1.0f };
                const float*     m  = st.pal.data() + size_t(hpalm[sd]) * 12;
                piv[sd]             = cw[sd] > 0.0f ? cs[sd] / cw[sd]
                                                    : glm::vec3{ m[0] * rp.x + m[1] * rp.y + m[2] * rp.z + m[3], m[4] * rp.x + m[5] * rp.y + m[6] * rp.z + m[7], m[8] * rp.x + m[9] * rp.y + m[10] * rp.z + m[11] };
                if (hw[sd] > 0.0f && fw[sd] > 0.0f && glm::length(hs[sd] / hw[sd] - fs[sd] / fw[sd]) > 1e-3f) fore[sd] = glm::normalize(hs[sd] / hw[sd] - fs[sd] / fw[sd]);
            }
            // Slot 0 with its side's palm (the game's matrices, before the move, which then takes both alike).
            if (!gm->cent.empty() && gm->cent[0].x > 0.0f && sides[0] >= 0 && hpalm[sides[0]] > 0 && st.pal.size() >= size_t(hpalm[sides[0]] + 1) * 12) {
                const float* pm = st.pal.data() + size_t(hpalm[sides[0]]) * 12;
                glm::vec3    r0 = glm::vec3{ gm->cent[0].y, gm->cent[0].z, gm->cent[0].w } * scale, was{}, now{};
                const float* m0 = st.pal.data();
                was = glm::vec3{ m0[0] * r0.x + m0[1] * r0.y + m0[2] * r0.z + m0[3], m0[4] * r0.x + m0[5] * r0.y + m0[6] * r0.z + m0[7], m0[8] * r0.x + m0[9] * r0.y + m0[10] * r0.z + m0[11] };
                now = glm::vec3{ pm[0] * r0.x + pm[1] * r0.y + pm[2] * r0.z + pm[3], pm[4] * r0.x + pm[5] * r0.y + pm[6] * r0.z + pm[7], pm[8] * r0.x + pm[9] * r0.y + pm[10] * r0.z + pm[11] };
                say(n, 12,
                    fmt::format("the {} palm: slot {} (weight {:.0f}) and the hand's centre; slot 0 (the root, weight {:.0f}) takes the palm's matrix: its vertices' centre "
                                "was posed {:.0f} cm from where the palm puts it",
                                sides[0] == 0 ? "right" : "left", hpalm[sides[0]], pw[sides[0]], gm->cent[0].x, 100.0f * glm::length(now - was)),
                    300 + sides[0]);
                std::copy(pm, pm + 12, st.pal.begin());
            }
            veh_hands_place(ctx, st.pal, std::min(256, int(gm->cent.size())), sides, hpalm, piv, fore, n, start, base, gm, scale);
        }
        if (!hands_cb_swap(ctx, st.pal)) {
            st.hidden++;
            say(n, 5, "hidden: the palette could not be swapped");
            return false;
        }
        st.shown++;
        // drivehandsdirect (test, off by default): drawn directly (hk_draw_idx_inst_indirect), one instance, with the counts read for it. In a car
        // only the arm hair followed the controllers once (author 6 Nov 14:41); an instance count of 0 written by the game's GPU culling would do
        // that, as would the skin being placed out of sight (the next run showed it drawn, misplaced: author 6 Nov 14:59). The line below
        // says which.
        const auto hopts  = fc5::drive::hand_opts();
        const bool direct = hopts.direct;
        m_vh_ontop        = hopts.ontop;   // drivehandsontop (test): hk_draw_idx_inst_indirect draws it with its depth test always passing
        if (direct) {
            m_vh_direct       = true;
            m_vh_direct_n     = n;
            m_vh_direct_start = start;
            m_vh_direct_base  = base;
            m_vh_direct_inst  = UINT(std::max(0, ia[4]));
        }
        // Once per drive for each arguments buffer: what the game's arguments hold now, and the pass the draw is in (render targets and
        // viewport: the colour pass has targets, a shadow or depth pass none), so the log says which cause it was.
        if (m_hud_draw_kind == 5 && m_hud_draw_args && st.live_said.size() < 16 && st.live_said.insert({ (void*)m_hud_draw_args, m_hud_draw_args_off }).second) {
            std::array<int32_t, 5> live{};
            const bool             got = indirect_args(ctx, m_hud_draw_args, m_hud_draw_args_off, live, true);
            ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* dsv = nullptr;
            ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
            int nrt = 0;
            for (auto*& r : rtv)
                if (r) {
                    nrt++;
                    r->Release();
                }
            const bool depth = dsv != nullptr;
            if (dsv) dsv->Release();
            D3D11_VIEWPORT vp{};
            UINT           nvp = 1;
            ctx->RSGetViewports(&nvp, &vp);
            ID3D11PixelShader* ps = nullptr;
            ctx->PSGetShader(&ps, nullptr, nullptr);
            const bool has_ps = ps != nullptr;
            if (ps) ps->Release();
            // The depth and stencil test it is drawn with (author 7 Nov 08:53: the hands not showing in the car, those far ahead did): a test
            // other than less / less-equal (equal: a depth pass before it decides) or a stencil test would hide hands placed elsewhere.
            std::string dss = "depth state: the default (less, written)";
            {
                ID3D11DepthStencilState* ds  = nullptr;
                UINT                     ref = 0;
                ctx->OMGetDepthStencilState(&ds, &ref);
                if (ds) {
                    D3D11_DEPTH_STENCIL_DESC d{};
                    ds->GetDesc(&d);
                    ds->Release();
                    static const char* kf[]{ "?", "never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always" };
                    dss = fmt::format("depth test {} {}, {}; stencil {}", d.DepthEnable ? "on" : "off", kf[std::clamp(int(d.DepthFunc), 0, 8)],
                                      d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? "written" : "not written",
                                      d.StencilEnable ? fmt::format("on (ref {}, read 0x{:02X}, write 0x{:02X}, front {})", ref, d.StencilReadMask, d.StencilWriteMask,
                                                                    kf[std::clamp(int(d.FrontFace.StencilFunc), 0, 8)])
                                                      : std::string{ "off" });
                }
            }
            spdlog::info("[VehHands] draw {}: the game's arguments now: {}; pass: {} render targets{}, viewport {:.0f}x{:.0f}, pixel shader {}, {}; {}{}", n,
                         got ? fmt::format("{} indices x{} instances, start {}, base {} (read for it: x{})", live[0], live[1], live[2], live[3], ia[1])
                             : std::string{ "not read" },
                         nrt, depth ? " and depth" : "", nvp ? vp.Width : 0.0f, nvp ? vp.Height : 0.0f, has_ps ? "bound" : "none", dss,
                         direct ? "drawn directly, one instance (drivehandsdirect on)" : "drawn with the game's arguments (drivehandsdirect off)",
                         hopts.ontop ? ", over everything (drivehandsontop on)" : "");
        }
        say(n, 11,
            fmt::format("{} ({} bones, indirect): {}; {} unused slots zeroed (scale {:.3g})",
                        gm->gloves ? "a pair of gloves" : gm->arm_side == 0 ? "one arm with its hand (right)" : "one arm with its hand (left)", gm->bones,
                        gm->gloves ? "drawn as the game draws it" : m_bare_folded != folded ? fmt::format("the forearm folded onto the wrist (rest-pose |x| under {:.0f})", gm->hand_from)
                                                                                           : std::string{ "no wrist found: drawn whole" },
                        unused, scale),
            gm->gloves ? 100 : 101 + gm->arm_side);
        return true;
    }
    // Models sharing this count are told apart by the highest palette slot their vertices use, each candidate read over its own index count
    // (an indirect draw's count is rounded up to 192: the gloves' 14316 indices are drawn as 14400, and the 84 past the model belong to
    // another mesh, whose slots went up to 69 and picked the 126-slot DragonOmega gloves over the 46-slot ThinShort01 ones). The candidate
    // whose last slot the read reaches (or comes closest to without passing it) is the model; until every read is in, the draw stays hidden.
    int alts = 0;
    int d    = fc5::bonemap::find(n, -1, indirect, alts);
    int mb   = -1;
    if (d >= 0 && alts > 0) {
        int       cand[8]{};
        const int nc   = fc5::bonemap::candidates(n, indirect, cand, 8);
        int       best = -1, best_gap = 1 << 30, best_mb = -1, pending = 0;
        for (int i = 0; i < nc; ++i) {
            const int c   = cand[i];
            const int cmb = veh_hands_max_bone(ctx, fc5::bonemap::index_count(c), start, base);
            if (cmb < 0) {
                pending++;
                continue;
            }
            const int cnb = fc5::bonemap::nbones(c);
            if (cnb <= 0 || cmb >= cnb) continue;   // its vertices use a slot past this model's last
            // The nearest fit; on a tie the hand wear for an indirect draw (find's rule), else the first.
            const int gap = (cnb - 1 - cmb) * 2 + (indirect && fc5::bonemap::kind(c) == 3 ? 0 : 1);
            if (gap < best_gap) {
                best_gap = gap;
                best     = c;
                best_mb  = cmb;
            }
        }
        if (pending > 0) {
            st.hidden++;
            say(n, 6, fmt::format("hidden for now: {} models share this count; waiting for {} vertex reads", nc, pending));
            return false;
        }
        if (best < 0) {
            st.hidden++;
            say(n, 7, fmt::format("hidden: none of the {} models that share this count fits its vertices' palette slots", nc));
            return false;
        }
        d    = best;
        mb   = best_mb;
        alts = 0;
        for (int i = 0; i < nc; ++i)
            if (cand[i] != best && fc5::bonemap::nbones(cand[i]) == fc5::bonemap::nbones(best) && fc5::bonemap::index_count(cand[i]) == fc5::bonemap::index_count(best)) alts++;
    }
    const int kind = fc5::bonemap::kind(d);
    const int nb   = fc5::bonemap::nbones(d);
    if (d < 0 || kind < 2 || nb <= 0) {
        st.hidden++;
        say(n, 2, d < 0 ? "hidden: not a first-person arm, glove or outfit model (not in the bone table)"
                        : fmt::format("hidden: {} ({}, {} slots)", fc5::bonemap::model(d), kind == 1 ? "a weapon model" : "rigid", nb));
        return false;
    }
    // Which slots are the hand and fingers, each side's hand joint.
    int  keep_n = 0, anchor[2]{ -1, -1 }, any_side = -1;
    bool keep[256]{};
    for (int s = 0; s < nb && s < 256; ++s) {
        if (fc5::bonemap::slot_group(d, s) != fc5::bonemap::kHand) continue;
        keep[s] = true;
        keep_n++;
        const char* nm   = fc5::bonemap::slot_name(d, s);
        const int   side = side_of(nm);
        if (side < 0) continue;
        if (any_side < 0) any_side = side;
        if (anchor[side] < 0 || is_main_hand(nm)) anchor[side] = s;   // "LeftHand" / "RightHand", else the side's first hand slot
    }
    if (keep_n == 0) {
        st.hidden++;
        say(n, 3, fmt::format("hidden: {} has no hand bone ({} slots)", fc5::bonemap::model(d), nb));
        return false;
    }
    if (any_side < 0) {   // hand slots with no side in their names: the first one anchors everything
        for (int s = 0; s < nb && s < 256; ++s)
            if (keep[s]) { anchor[0] = s; break; }
        any_side = 0;
    }
    if (anchor[0] < 0) anchor[0] = anchor[1];
    if (anchor[1] < 0) anchor[1] = anchor[0];
    // The model's own vertices (read once): the fold target is the anchor's vertices' centre, posed (as barehands' fold), not the bone
    // table's joint (builder 6 Nov, patch 3: the strips stayed with the right tables). Hidden until the read is in.
    const std::map<int, glm::vec3>* cent = nullptr;
    if (veh_hands_max_bone(ctx, indirect ? fc5::bonemap::index_count(d) : n, start, base, &cent) < 0 || !cent) {
        st.hidden++;
        say(n, 8, fmt::format("hidden for now: {}: waiting for its vertices' read", fc5::bonemap::model(d)), d);
        return false;
    }
    // The vertex positions' scale (VS b2, as bonemap and the gloves read it); 0 if not known.
    float scale = 0.0f;
    {
        ID3D11Buffer* cb2 = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cb2);
        if (cb2) {
            std::lock_guard _{ m_obj_mtx };
            if (auto it = m_obj_consts.find(cb2); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
        }
        if (cb2) cb2->Release();
    }
    {
        std::lock_guard _{ m_obj_mtx };
        auto            it = palette_cb ? m_bone_data.find(palette_cb) : m_bone_data.end();
        if (it == m_bone_data.end() || it->second.size() < 3072) {
            st.hidden++;
            say(n, 4, fmt::format("hidden: {}: no copy of its palette yet", fc5::bonemap::model(d)));
            return false;
        }
        st.pal = it->second;
    }
    // Each side's fold target, posed (object space): the palette matrix (3x4 rows) times the anchor's vertices' centre (x the scale); the
    // bone table's joint if those are not known, else the matrix's translation. The log says how far the joint's point lies from it.
    float posed[2][3]{};
    std::string apart;
    for (int side = 0; side < 2; ++side) {
        const int    a = anchor[side];
        const float* m = st.pal.data() + size_t(a) * 12;
        float        j[3]{};
        const bool   jk = fc5::bonemap::slot_joint(d, a, j);
        float        pj[3]{};
        for (int r = 0; r < 3; ++r) pj[r] = jk ? m[r * 4 + 0] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] : m[r * 4 + 3];
        const auto ci = cent->find(a);
        if (scale > 0.0f && ci != cent->end()) {
            const glm::vec3 c = ci->second * scale;
            for (int r = 0; r < 3; ++r) posed[side][r] = m[r * 4 + 0] * c.x + m[r * 4 + 1] * c.y + m[r * 4 + 2] * c.z + m[r * 4 + 3];
            if (side == 0 || anchor[1] != anchor[0])
                apart += fmt::format("{}{} {:.0f} cm", apart.empty() ? "" : ", ", fc5::bonemap::slot_name(d, a) ? fc5::bonemap::slot_name(d, a) : "?",
                                     100.0f * std::sqrt((pj[0] - posed[side][0]) * (pj[0] - posed[side][0]) + (pj[1] - posed[side][1]) * (pj[1] - posed[side][1]) +
                                                        (pj[2] - posed[side][2]) * (pj[2] - posed[side][2])));
        } else {
            for (int r = 0; r < 3; ++r) posed[side][r] = pj[r];
        }
    }
    // Slots past the model's last (an indirect draw's padding draws a few triangles of the next mesh with them): no size, out of sight.
    for (size_t s = size_t(nb) * 12; s < st.pal.size() && s < 256 * 12; ++s) st.pal[s] = 0.0f;
    // Every other slot of the model: no size, at its side's hand (a slot with no side in its name: the side of the draw's first hand slot).
    for (int s = 0; s < nb && s < 256; ++s) {
        if (keep[s]) continue;
        int side = side_of(fc5::bonemap::slot_name(d, s));
        if (side < 0) side = any_side;
        float* m = st.pal.data() + size_t(s) * 12;
        for (int r = 0; r < 3; ++r) {
            m[r * 4 + 0] = m[r * 4 + 1] = m[r * 4 + 2] = 0.0f;
            m[r * 4 + 3] = posed[side][r];
        }
    }
    // drivehandsgrab: each side's hand onto its controller or the wheel (its palm the side's hand joint slot, its centre that slot's vertices').
    {
        int8_t    sides[256];
        int       hpalm[2]{ -1, -1 };
        glm::vec3 piv[2]{};
        std::fill(std::begin(sides), std::end(sides), int8_t(-1));
        for (int s = 0; s < nb && s < 256; ++s) {
            int side = side_of(fc5::bonemap::slot_name(d, s));
            if (side < 0) side = any_side;
            sides[s] = int8_t(side);
            if (keep[s]) hpalm[side] = anchor[side];   // a side with a hand slot in this draw
        }
        for (int sd = 0; sd < 2; ++sd) piv[sd] = glm::vec3{ posed[sd][0], posed[sd][1], posed[sd][2] };
        // drivehandsspace: this draw's hands in the world as the game poses them (its VS b1 times the hand's vertices, before any move), for
        // the skin draw's palette space (space_check).
        {
            ID3D11Buffer* cb1 = nullptr;
            ctx->VSGetConstantBuffers(1, 1, &cb1);
            glm::mat4 Oh{ 1.0f };
            bool      ok = false;
            if (cb1) {
                std::lock_guard _{ m_obj_mtx };
                if (auto it = m_obj_consts.find(cb1); it != m_obj_consts.end()) {
                    Oh = from_rows12(it->second.f);
                    ok = std::abs(glm::determinant(glm::mat3{ Oh })) > 1e-6f;
                }
                cb1->Release();
            }
            if (ok && m_mo.valid && frame - m_mo.frame <= 1) {
                st.hair_om = Oh;
                for (int sd = 0; sd < 2; ++sd) {
                    if (hpalm[sd] < 0) continue;
                    st.hair_hw[sd]    = glm::vec3{ Oh * glm::vec4{ piv[sd], 1.0f } };
                    st.hair_frame[sd] = frame;
                }
                st.space_check(frame, m_mo.cam, m_mo.game_rot);
            }
        }
        veh_hands_place(ctx, st.pal, std::min(nb, 256), sides, hpalm, piv, nullptr, n, start, base);
    }
    if (!hands_cb_swap(ctx, st.pal)) {
        st.hidden++;
        say(n, 5, "hidden: the palette could not be swapped");
        return false;
    }
    st.shown++;
    say(n, 0,
        fmt::format("{} ({}{}): the hands drawn, {} of {} slots kept{}; the rest folded onto the hand's vertices{}{}", fc5::bonemap::model(d),
                    kind == 2 ? "bare arms" : kind == 3 ? "hand wear" : "sleeves or outfit", indirect ? ", indirect" : "", keep_n, nb,
                    mb >= 0 ? fmt::format(" (its vertices use slots up to {})", mb) : std::string{},
                    apart.empty() ? fmt::format(" (not known, scale {}: at the bone table's joint)", scale) : fmt::format(" (the table's joint lies {} from them)", apart),
                    alts > 0 ? fmt::format(" ({} other models still share this count)", alts) : std::string{}),
        d);
    return true;
}


namespace
{
    // The rest pose's hand axes per side (columns: fingers, knuckles index -> little finger, palm out) and its forearm direction (elbow -> wrist),
    // from Body_FPSArms*'s joints in FarCry5BoneNames.h (RightForeArm, RightHand, RightHandMiddle1, RightHandIndex1, RightHandPinky1; the left
    // mirrored, so its axes are a mirror image: the same holds for its axes on the controller, so the turn between them is a true rotation).
    struct RestHand
    {
        glm::mat3 axes[2]{};
        glm::vec3 fore[2]{};
    };
    const RestHand& rest_hand()
    {
        static const RestHand h = [] {
            RestHand        o{};
            const glm::vec3 el{ 0.434f, -0.053f, 1.247f }, wr{ 0.593f, 0.100f, 1.126f }, mid{ 0.639f, 0.156f, 1.079f }, idx{ 0.625f, 0.172f, 1.090f },
                pnk{ 0.643f, 0.111f, 1.076f };
            const glm::vec3 F = glm::normalize(mid - wr);
            glm::vec3       K = pnk - idx;
            K                 = glm::normalize(K - glm::dot(K, F) * F);
            const glm::vec3 M{ -1.0f, 1.0f, 1.0f };
            o.axes[0] = glm::mat3{ F, K, glm::cross(F, K) };
            o.axes[1] = glm::mat3{ F * M, K * M, glm::cross(K * M, F * M) };
            o.fore[0] = glm::normalize(wr - el);
            o.fore[1] = o.fore[0] * M;
            return o;
        }();
        return h;
    }
    // The same axes held on a controller (x right, y up, -z forward): a handshake grip, fingers forward, the index knuckle on top, the palm
    // towards the body's middle (the right palm to the left, the left palm to the right).
    const glm::mat3 kOnCtrl[2]{ glm::mat3{ glm::vec3{ 0, 0, -1 }, glm::vec3{ 0, -1, 0 }, glm::vec3{ -1, 0, 0 } },
                                glm::mat3{ glm::vec3{ 0, 0, -1 }, glm::vec3{ 0, -1, 0 }, glm::vec3{ 1, 0, 0 } } };
    // The shortest turn taking unit a onto unit b.
    glm::mat3 turn_onto(const glm::vec3& a, const glm::vec3& b)
    {
        const glm::vec3 ax = glm::cross(a, b);
        const float     s  = glm::length(ax);
        const float     c  = std::clamp(glm::dot(a, b), -1.0f, 1.0f);
        if (s < 1e-5f) return glm::mat3{ 1.0f };
        return glm::mat3_cast(glm::angleAxis(std::atan2(s, c), ax / s));
    }
    glm::mat4 from_rows12(const float* m)
    {
        glm::mat4 M{ 1.0f };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) M[c][r] = m[r * 4 + c];
        return M;
    }
    void to_rows12(const glm::mat4& M, float* m)
    {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[r * 4 + c] = M[c][r];
    }
}

// drivehandsgrab (FarCry5Drive.h). Author 6 Nov 20:58.
//
// Each side's hand in this draw (the game's hand on the wheel, bars or stick, the forearm already folded onto the wrist) is moved as one piece
// by one rigid move T (world), then (path B) its fingers are posed as on foot (fingergrip: open, grip and trigger close them):
//  - free: onto its own controller, as on foot. The weapon hand (the right one; the left for the left-handed) as the game held the recorded
//    sidearm on foot (holsterglove: that arm mesh's bones as the game held the weapon, in the weapon's own space, put on the controller as the
//    weapon is), which is where the on-foot hand sits on its controller; the other hand that hold mirrored onto its own controller (its arm
//    mesh is the weapon arm's mirror image: checked on the palms' rest-pose centres, within 3 cm). Patch 6 guessed the hand's axes from the
//    bone table's rest pose instead, and the bare hand floated ahead, palm up (author 6 Nov 14:59). With no recording (or a palm that is not
//    the mirror image): a handshake grip from those rest-pose axes (kOnCtrl; a mesh whose rest-pose forearm points over 20 deg away from the
//    table's has them turned onto it first). drivehandturn and drivehandshift turn and move either in the controller's axes. No weapon is
//    drawn in the hand (author 6 Nov 20:57). While the game draws its own sidearm (the right trigger: the weapon pass back with its glove of
//    that side) the weapon hand is not drawn: the game's is in the same place.
//  - on the control: while the gesture drive (drive on) holds the wheel, bars or stick with this hand, the hand stays where it took it, its
//    centre put on the hold point (the rim, the bar's end, the stick's top), and turns with the wheel or bars about their axis (moves with the
//    stick's top), until the grip is let go. The control is the drive's (FarCry5Drive.cpp, fixed in the recentred reference where the
//    vehicle's own control is drawn), read in tracking space and put in the world as the controllers are (update_motion).
// Path A (the bone table's meshes: the arm hair over the bare arms) takes path B's move of the same side this frame, so the hair stays on the
// skin; on its own (no path B draw of that side this frame) it is placed as above.
// Only the VR hands show (author 6 Nov 21:06): a hand whose controller
// is not tracked, and every hand in a frame with no motion frame or no object transform, is not drawn.
void FarCry5Stereo::veh_hands_place(ID3D11DeviceContext* ctx, std::vector<float>& pal, int nslots, const int8_t* slot_side, const int palm[2], const glm::vec3 pivot[2],
                                    const glm::vec3* fore, UINT count, UINT start, INT base, const GloveMesh* gm, float scale)
{
    const auto ho = fc5::drive::hand_opts();
    if (!ho.grab || !m_vh) return;
    auto&     st    = *m_vh;
    const int frame = m_frame.load();
    // Nothing to place them by this frame: the hands not drawn (only the VR hands show, never the game's on the wheel).
    auto hide_all = [&] {
        for (int b = 0; b < nslots && size_t(b + 1) * 12 <= pal.size(); ++b)
            if (slot_side[b] >= 0) std::fill(pal.begin() + size_t(b) * 12, pal.begin() + size_t(b + 1) * 12, 0.0f);
    };
    if (!m_mo.valid || frame - m_mo.frame > 1) {
        hide_all();
        return;
    }
    // The draw's object transform (VS b1, CSceneTransform: rows, the translation in .w): object -> world.
    glm::mat4 Om{ 1.0f };
    {
        ID3D11Buffer* cb1 = nullptr;
        ctx->VSGetConstantBuffers(1, 1, &cb1);
        bool ok = false;
        if (cb1) {
            std::lock_guard _{ m_obj_mtx };
            if (auto it = m_obj_consts.find(cb1); it != m_obj_consts.end()) {
                Om = from_rows12(it->second.f);
                ok = true;
            }
            cb1->Release();
        }
        if (!ok || std::abs(glm::determinant(glm::mat3{ Om })) < 1e-6f) {
            hide_all();
            return;
        }
    }
    // drivehandsspace: the bare arm's (indirect draw's) palette space. Each frame its palm (before the move) is noted for space_check, which sets
    // it against the arm hair's hand; the kind found (or drivehandsspace's own) then turns its palette into the world instead of VS b1 alone.
    // While none is found, for the drive's first 45 frames, the skin is not drawn (the hair is then placed on its own, by its own b1); after
    // that its own b1, as patches 6-10.
    if (gm) {
        for (int s = 0; s < 2; ++s) {
            if (palm[s] < 0) continue;
            st.skin_p[s]     = pivot[s];
            st.skin_om[s]    = Om;
            st.skin_frame[s] = frame;
        }
        st.space_check(frame, m_mo.cam, m_mo.game_rot);
        int k = ho.space >= 0 ? ho.space : st.space;
        if (k < 0) {
            if (frame - st.drive_frame < 45) {
                hide_all();
                return;
            }
            k = 0;
            if (!st.fallback_said) {
                st.fallback_said = true;
                spdlog::info("[VehHands] drivehandsspace: no space found for the skin after 45 frames (no arm hair seen, or no clear kind yet): its own object transform (VS b1) until one is");
            }
        }
        if (k != 0) {
            const glm::mat4 M = VehHandsState::space_matrix(k, Om, st.hair_om, m_mo.cam, m_mo.game_rot);
            if (std::abs(glm::determinant(glm::mat3{ M })) > 1e-6f) Om = M;
        }
    }
    const glm::mat4 Om_inv = glm::inverse(Om);
    // Every slot of side s by the move T (world), in the draw's object space; or folded to nothing.
    auto apply = [&](int s, const glm::mat4& T, bool hide) {
        const glm::mat4 A = Om_inv * T * Om;
        for (int b = 0; b < nslots && size_t(b + 1) * 12 <= pal.size(); ++b) {
            if (slot_side[b] != s) continue;
            float* m = pal.data() + size_t(b) * 12;
            if (hide) std::fill(m, m + 12, 0.0f);
            else to_rows12(A * from_rows12(m), m);
        }
    };
    // The recentred reference, as update_motion places the controllers: world = camera + draw shift + body_rot^T ref.
    const glm::mat3 B  = m_mo.body_rot;
    const glm::mat3 Bt = glm::transpose(B);
    const glm::vec3 o  = m_mo.cam + m_mo.draw_shift;
    auto to_ref = [&](const glm::mat4& w) {
        glm::mat4 r{ B * glm::mat3{ w } };
        r[3] = glm::vec4{ B * (glm::vec3{ w[3] } - o), 1.0f };
        return r;
    };
    auto to_world = [&](const glm::mat4& r) {
        glm::mat4 w{ Bt * glm::mat3{ r } };
        w[3] = glm::vec4{ o + Bt * glm::vec3{ r[3] }, 1.0f };
        return w;
    };
    // The gesture drive's control (tracking space) in the reference above, placed as update_motion places the controllers: a tracking point p is
    // at cam + draw_shift + body_rot^T (ws ref_inv p) in the world (gun_anchor_apply moves the controllers onto the eyes' base with draw_shift),
    // so at ws ref_inv p here; a direction d at ref_inv d. Patch 7 took the point from the game camera (- body_rot draw_shift here): in the
    // car of the author's 7 Nov 09:48 drive the eyes' base was 1.35-1.39 m from it, and the hands that took the wheel went there, ahead of the
    // car, outside it), while the drive had taken the wheel 1-4 cm from them.
    fc5::drive::View cv{};
    bool             have_cv = fc5::drive::control(cv) && cv.style != fc5::drive::kNone;
    glm::mat4        ref_inv{ 1.0f };
    if (have_cv) {
        std::lock_guard _{ m_ref_mtx };
        have_cv = m_have_ref;
        ref_inv = m_ref_inv;
    }
    const float ws     = std::max(1e-3f, m_world_scale.load());
    auto        cv_pt  = [&](const glm::vec3& p) { return ws * glm::vec3{ ref_inv * glm::vec4{ p, 1.0f } }; };
    auto        cv_dir = [&](const glm::vec3& d) { return glm::normalize(glm::mat3{ ref_inv } * d); };
    const RestHand& rh    = rest_hand();
    const int       wside = left_handed() ? 1 : 0;   // the weapon hand
    // The game's own sidearm out: the weapon pass holds a weapon this frame or the last.
    const bool      game_gun = !m_hand_map.empty && frame - m_hand_map.frame <= 1;
    const char*     names[2]{ "right", "left" };
    const char*     ctl = !have_cv ? "control" : cv.style == fc5::drive::kWheel ? "wheel" : cv.style == fc5::drive::kBars ? "handlebars" : "stick";
    // vehwheels: once this vehicle's seat is known, its wheel from the table (the ring on the vehicle's own wheel before any turn); a wheel
    // learnt here (or learnt again) saved for it.
    // Handlebars and sticks (9 Nov): from the table too (built in only), kept while the game's hands are not on them.
    if (ho.model && have_cv) {
        if (!st.key_tried && m_vsh_valid && m_vsh_count != 0 && m_vsh_learn_frame >= st.drive_frame - 600) {
            st.key_tried = true;
            st.key_ok    = true;
            st.key_count = m_vsh_count;
            st.key_seat  = m_vsh_seat;
            vw_load();
            const int sty = cv.style == fc5::drive::kWheel ? 0 : cv.style == fc5::drive::kBars ? 1 : 2;
            const int i   = vw_find(st.key_count, st.key_seat, sty);
            if (sty != 0) {
                fc5::steertune::auto_turn(350);   // driveautoturn for handlebars and sticks too (author 9 Nov 11:20)
                if (i >= 0) {
                    st.tbl_style = cv.style;
                    st.tbl_c     = g_vw[i].c;
                    st.tbl_r     = g_vw[i].r;
                    spdlog::info("[VehHands] drivehandsmodel: this vehicle's {} from the built-in table (model {} indices, the seat at ({:.2f}, {:.2f}, {:.2f}) on it): ({:+.0f}, "
                                 "{:+.0f}, {:+.0f}) cm from the eyes{}", ctl, st.key_count, st.key_seat.x, st.key_seat.y, st.key_seat.z, st.tbl_c.x * 100.0f,
                                 st.tbl_c.y * 100.0f, st.tbl_c.z * 100.0f, sty == 1 ? fmt::format(", half width {:.0f} cm", st.tbl_r * 100.0f) : std::string{});
                } else {
                    spdlog::info("[VehHands] drivehandsmodel: no {} in the table for this vehicle (model {} indices, the seat at ({:.2f}, {:.2f}, {:.2f}) on it): placed from "
                                 "the game's hands when they hold it", ctl, st.key_count, st.key_seat.x, st.key_seat.y, st.key_seat.z);
                }
            } else if (i >= 0 && !st.wheel_ok()) {
                const VehWheel& w = g_vw[i];
                st.wl.c           = w.c;
                st.wl.n           = w.n;
                st.wl.r           = w.r;
                st.wl.votes       = 2;
                st.from_table     = true;
                spdlog::info("[VehHands] drivehandsmodel: this vehicle's wheel from {} (model {} indices, the seat at ({:.2f}, {:.2f}, {:.2f}) on it): centre ({:+.0f}, {:+.0f}, "
                             "{:+.0f}) cm from the eyes, grip {:.0f} cm, face tilted {:.0f} deg up",
                             w.builtin ? "the built-in table" : "fc5vr_vehwheels.txt (learnt in play)", st.key_count, st.key_seat.x, st.key_seat.y, st.key_seat.z,
                             w.c.x / ws * 100.0f, w.c.y / ws * 100.0f, w.c.z / ws * 100.0f, w.r / ws * 100.0f, glm::degrees(std::asin(std::clamp(w.n.y, -1.0f, 1.0f))));
            } else if (i < 0) {
                spdlog::info("[VehHands] drivehandsmodel: no wheel known yet for this vehicle (model {} indices, the seat at ({:.2f}, {:.2f}, {:.2f}) on it): learnt from its "
                             "first turn of 30 deg or more, then saved", st.key_count, st.key_seat.x, st.key_seat.y, st.key_seat.z);
                fc5::steertune::auto_turn(350);   // driveautoturn (author 9 Nov 11:09): a quick small turn now, so that first turn comes at once
            }
        }
        if (cv.style != fc5::drive::kWheel && st.tbl_style == cv.style) {   // the game's hands, when seen this frame, send theirs after this
            fc5::drive::Model m{};
            m.style  = cv.style;
            m.centre = st.tbl_c;
            m.radius = st.tbl_r;
            fc5::drive::set_model(m);
        }
        if (cv.style == fc5::drive::kWheel && st.dirty && st.key_ok && st.wheel_ok()) {
            st.dirty = false;
            vw_load();
            VehWheel w;
            w.count     = st.key_count;
            w.seat      = st.key_seat;
            w.c         = st.wl.c;
            w.n         = st.wl.n;
            w.r         = st.wl.r;
            w.votes     = st.wl.votes;
            const int i = vw_find(w.count, w.seat);
            if (vw_sane(w)) {
                if (i >= 0) g_vw[i] = w;
                else g_vw.push_back(w);
                vw_save();
            }
        }
    }
    const glm::mat4 Mx{ glm::vec4{ -1, 0, 0, 0 }, glm::vec4{ 0, 1, 0, 0 }, glm::vec4{ 0, 0, 1, 0 }, glm::vec4{ 0, 0, 0, 1 } };   // x mirrored
    for (int s = 0; s < 2; ++s) {
        if (palm[s] < 0 || size_t(palm[s] + 1) * 12 > pal.size()) continue;
        auto& sd = st.side[s];
        // Path A: path B's move of this side this frame.
        if (!gm && sd.t_frame == frame) {
            apply(s, sd.t, sd.t_hide);
            continue;
        }
        auto say_state = [&](const std::string& now, const std::string& extra = {}) {
            if (now == sd.state || sd.lines >= 40) return;
            sd.state = now;
            sd.lines++;
            spdlog::info("[VehHands] drivehandsgrab: the {} hand {} (draw {}){}", names[s], now, count, extra);
        };
        // The game's hand now. Its palm slot's skinning (rest pose -> posed, object), world; its centre (the palm slot's vertices').
        const glm::mat4 P  = from_rows12(pal.data() + size_t(palm[s]) * 12);
        const glm::mat4 Gb = Om * P;
        if (std::abs(glm::determinant(glm::mat3{ Gb })) < 1e-6f) {
            apply(s, glm::mat4{ 1.0f }, true);
            continue;
        }
        const glm::vec3 cw = glm::vec3{ Om * glm::vec4{ pivot[s], 1.0f } };
        // The recorded hold (rest pose -> the controller's axes), path B only.
        glm::mat4  Rel{ 1.0f };
        bool       rec       = false;
        const bool mesh_side = gm && (gm->gloves || gm->arm_side == s) && palm[s] < int(gm->cent.size());
        const glm::vec3 palm_rest = mesh_side ? glm::vec3{ gm->cent[palm[s]].y, gm->cent[palm[s]].z, gm->cent[palm[s]].w } * scale : glm::vec3{ 0.0f };
        if (mesh_side && s == wside) {
            std::vector<std::pair<int, glm::mat4>> rbones;
            glm::mat3                              rcorr{ 1.0f };
            glm::vec3                              rgrip{ 0.0f };
            if (holster_vehicle_hand(gm, count, start, base, rbones, rcorr, rgrip)) {
                const auto it = std::find_if(rbones.begin(), rbones.end(), [&](const auto& e) { return e.first == palm[s]; });
                if (it != rbones.end() && rbones.size() >= 8) {
                    glm::mat4 Q2C{ rcorr };
                    Q2C[3]    = glm::vec4{ -(rcorr * rgrip), 1.0f };
                    Rel       = Q2C * it->second;
                    rec       = std::abs(glm::determinant(glm::mat3{ Rel })) > 1e-6f;
                    st.rel    = Rel;
                    st.rel_c  = palm_rest;
                    st.rel_ok = rec;
                }
            }
        } else if (mesh_side && st.rel_ok) {
            // The other hand: the weapon hand's hold mirrored (its controller is the weapon one's mirror image, x right for both, and so is
            // its arm's rest pose).
            const float off = glm::length(palm_rest - glm::vec3{ -st.rel_c.x, st.rel_c.y, st.rel_c.z });
            rec             = off < 0.03f;
            if (rec) Rel = Mx * st.rel * Mx;
            if (!st.mirror_said) {
                st.mirror_said = true;
                spdlog::info("[VehHands] drivehandsgrab: the {} hand {} (its palm's rest-pose centre {:.1f} cm from the {} palm's mirror image)", names[s],
                             rec ? "takes the recorded hold mirrored" : "is not the mirror image: placed from the rest pose's axes", off * 100.0f, names[wside]);
            }
        }
        // The recorded hold must put the palm on the controller. The author's 7 Nov 11:41-11:52 drives (build 1605A19C): with it the palms sat
        // 87-136 cm from the eyes' base (in the helicopter, far from the stick), the left hand before it had the
        // recording (the rest pose's axes) 54 cm, the hands on the wheel or stick 51-55 cm. So a hold whose palm centre lies over 12 cm from its
        // controller is not this mesh's (another arm draw's recording under the same counts, or one taken in another space): the rest pose's
        // axes instead, which put the palm's centre on the controller.
        if (rec) {
            const glm::vec3 off = glm::vec3{ Rel * (glm::inverse(Gb) * glm::vec4{ cw, 1.0f }) };
            if (glm::length(off) > 0.12f) {
                rec = false;
                if (s == wside) st.rel_ok = false;   // nor mirrored onto the other hand
                if (!sd.far_said) {
                    sd.far_said = true;
                    spdlog::info("[VehHands] drivehandsgrab: the {} hand's recorded hold puts its palm {:.0f} cm from its controller (at {:+.0f} {:+.0f} {:+.0f} cm in its axes): "
                                 "left out, the hand placed from the rest pose's axes (its palm on the controller)",
                                 names[s], glm::length(off) * 100.0f, off.x * 100.0f, off.y * 100.0f, off.z * 100.0f);
                }
            }
        }
        // Without it: the hand's own axes from the rest pose's (the palm slot's turn, orthonormal, times the table's hand axes).
        glm::mat4 X = Gb;
        if (!rec) {
            const glm::mat3 Rw = glm::mat3{ Gb };
            const glm::vec3 x  = glm::normalize(Rw[0]);
            glm::vec3       y  = Rw[1] - glm::dot(Rw[1], x) * x;
            if (glm::length(y) < 1e-4f) {
                apply(s, glm::mat4{ 1.0f }, true);
                continue;
            }
            y              = glm::normalize(y);
            glm::mat3 rest = rh.axes[s];
            if (fore && glm::length(fore[s]) > 0.5f && glm::dot(fore[s], rh.fore[s]) < std::cos(glm::radians(20.0f))) rest = turn_onto(rh.fore[s], fore[s]) * rest;
            X    = glm::mat4{ glm::mat3{ x, y, glm::cross(x, y) } * rest };
            X[3] = glm::vec4{ cw, 1.0f };
        }
        sd.lc = glm::inverse(X) * glm::vec4{ cw, 1.0f };
        // drivehandsmodel: this game hand, where the game animates it on the wheel, bars or stick (before the move), kept as a rigid frame
        // in the reference (unless it holds the game's own sidearm), and the vehicle's control from both hands handed to the drive.
        if (ho.model && have_cv && !(s == wside && game_gun)) {
            const glm::mat3 Rw = glm::mat3{ Gb };
            const glm::vec3 gx = glm::normalize(Rw[0]);
            glm::vec3       gy = Rw[1] - glm::dot(Rw[1], gx) * gx;
            if (glm::length(gy) > 1e-6f && std::isfinite(gx.x + gy.x + cw.x)) {
                gy = glm::normalize(gy);
                glm::mat4 G{ glm::mat3{ gx, gy, glm::cross(gx, gy) } };
                G[3] = glm::vec4{ cw, 1.0f };
                st.game_hand(s, to_ref(G), frame, ws, cv.style);
                st.send_model(cv.style, frame, ws, cv_pt(cv.stick_top));
            }
        }
        // Its controller (by physical side: the weapon hand's is update_motion's controller, or its own while the weapon hangs elsewhere).
        glm::mat3 CR{ 1.0f };
        glm::vec3 CP{ 0.0f };
        bool      cvalid = false;
        if (s == wside) {
            if (m_mo.ctrl_valid) {
                const bool away = m_mo.slung || m_mo.bow || m_mo.kept;
                CR              = away ? m_mo.hand_rot_world : m_mo.ctrl_rot_world;
                CP              = away ? m_mo.hand_pos_world : m_mo.ctrl_pos_world;
                cvalid          = true;
            }
        } else if (m_mo.other_valid) {
            CR     = m_mo.other_rot_world;
            CP     = m_mo.other_pos_world;
            cvalid = true;
        }
        if (!cvalid) {   // (only the VR hands show: the game's own hand is not left on the wheel)
            sd.on = false;
            apply(s, glm::mat4{ 1.0f }, true);
            sd.t       = glm::mat4{ 1.0f };
            sd.t_hide  = true;
            sd.t_frame = frame;
            say_state("not drawn: its controller is not tracked");
            continue;
        }
        glm::mat4 C{ CR };
        C[3]                  = glm::vec4{ CP, 1.0f };
        const glm::mat4 C_ref = to_ref(C);
        // On its controller.
        const glm::vec3 tv = s == 1 ? ho.turn * glm::vec3{ 1.0f, -1.0f, -1.0f } : ho.turn;
        const glm::mat3 turn{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(tv.y), glm::vec3{ 0, 1, 0 }) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(tv.x), glm::vec3{ 1, 0, 0 }) *
                              glm::rotate(glm::mat4{ 1.0f }, glm::radians(tv.z), glm::vec3{ 0, 0, 1 }) };
        const glm::vec3 shift = s == 1 ? ho.shift * glm::vec3{ -1.0f, 1.0f, 1.0f } : ho.shift;
        glm::mat4       Tadj{ turn };   // drivehandturn, drivehandshift (controller axes)
        Tadj[3] = glm::vec4{ shift, 1.0f };
        glm::mat4 H_free{ 1.0f };
        if (rec) {
            H_free = C_ref * Tadj * Rel;
        } else {
            glm::mat4 Hc{ turn * kOnCtrl[s] };
            Hc[3]  = glm::vec4{ shift, 1.0f };
            H_free = C_ref * Hc;
        }
        // Stuck to the control while the gesture drive holds it with this hand (cv.held: the drive's own sticky hold, taken by this hand's grip).
        const bool held = have_cv && cv.held[s];
        glm::mat4  H    = H_free;
        if (held) {
            const glm::vec3 c    = cv_pt(cv.centre);
            const glm::vec3 axis = cv.style == fc5::drive::kWheel ? cv_dir(cv.n) : cv_dir(cv.v);   // wheel: its plane's normal; bars: the vertical
            const glm::vec3 top  = cv_pt(cv.style == fc5::drive::kStick ? cv.stick_top : cv.centre);
            if (!sd.on) {
                // Taken: its centre put on the hold point, the rest as the controller holds it.
                sd.on               = true;
                sd.a0               = cv.angle;
                sd.top0             = top;
                const glm::vec3 hc  = glm::vec3{ H_free * sd.lc };
                const glm::vec3 hp  = cv_pt(cv.hold[s]);
                sd.h0               = glm::translate(glm::mat4{ 1.0f }, hp - hc) * H_free;
                sd.grabs++;
                if (sd.lines < 40) {
                    sd.lines++;
                    spdlog::info("[VehHands] drivehandsgrab: the {} hand took the {} ({:.0f} cm from its controller to the hold point; the eyes' base {:.0f} cm from the "
                                 "game camera, the hand {:.0f} cm from the eyes' base): it stays there and turns with it until its grip is let go",
                                 names[s], ctl, glm::length(hp - hc) * 100.0f, glm::length(m_mo.draw_shift) * 100.0f, glm::length(hp) * 100.0f);
                }
            }
            glm::mat4 M{ 1.0f };   // the control's move since the take
            if (cv.style == fc5::drive::kStick) {
                M = glm::translate(glm::mat4{ 1.0f }, top - sd.top0);
            } else {
                M = glm::translate(glm::mat4{ 1.0f }, c) * glm::mat4_cast(glm::angleAxis(cv.angle - sd.a0, axis)) * glm::translate(glm::mat4{ 1.0f }, -c);
            }
            H = M * sd.h0;
            // drivehandsmodel: the hand goes where the game moves the vehicle's own control under it.
            //  - wheel: turned about the learnt wheel's axis as far as the game turned its wheel model since the take (theta, from the game hand
            //    that holds the rim), from where it took the rim; until the wheel is learnt (the first turn of 30 deg or more), with the drive's own;
            //  - bars: rides the game hand of the same side (or the other while this one holds the game's sidearm);
            //  - stick: rides the game hand on the stick (stick_hand: the one nearer the drive's stick); none within 45 cm: with the drive's own.
            // No game hand this frame: where it was.
            if (ho.model && cv.style == fc5::drive::kWheel) {
                if (st.wheel_ok()) {
                    if (!sd.glued) {
                        sd.glued          = true;
                        sd.k              = H;   // at the take, sd.h0; once learnt while held, where the drive's wheel had turned it
                        sd.th0            = st.theta;
                        sd.glue_log_frame = frame;
                        if (sd.lines < 40) {
                            sd.lines++;
                            const glm::vec3 r  = glm::vec3{ H * sd.lc } - st.wl.c;
                            const float     h  = glm::dot(r, st.wl.n);
                            const float     rp = glm::length(r - st.wl.n * h);
                            spdlog::info("[VehHands] drivehandsmodel: the {} hand turns with the vehicle's own wheel (as far as the game turns it; it took the wheel {:.0f} cm "
                                         "from its rim)", names[s], std::sqrt((rp - st.wl.r) * (rp - st.wl.r) + h * h) / ws * 100.0f);
                        }
                    }
                    const glm::mat4 R = glm::translate(glm::mat4{ 1.0f }, st.wl.c) * glm::mat4_cast(glm::angleAxis(st.theta - sd.th0, st.wl.n)) *
                                        glm::translate(glm::mat4{ 1.0f }, -st.wl.c);
                    H = R * sd.k;
                    // Every 2 s: the game's turn of its wheel under the hand against the drive's own.
                    if (frame - sd.glue_log_frame >= 180 && sd.lines < 40) {
                        sd.glue_log_frame = frame;
                        sd.lines++;
                        const glm::vec3 r  = glm::vec3{ H * sd.lc } - st.wl.c;
                        const float     h  = glm::dot(r, st.wl.n);
                        const float     rp = glm::length(r - st.wl.n * h);
                        spdlog::info("[VehHands] drivehandsmodel: the {} hand on the wheel: the game turned it {:+.0f} deg since the take (positive: to the left), the "
                                     "drive's wheel {:+.0f} deg; the hand {:.0f} cm from the wheel's rim", names[s], glm::degrees(st.theta - sd.th0),
                                     glm::degrees(cv.angle - sd.a0), std::sqrt((rp - st.wl.r) * (rp - st.wl.r) + h * h) / ws * 100.0f);
                    }
                }
            } else if (ho.model) {
                int g = -1;
                if (cv.style == fc5::drive::kStick)
                    g = st.stick_hand >= 0 && st.fresh(st.stick_hand, frame) ? st.stick_hand : -1;
                else
                    g = st.fresh(s, frame) ? s : st.fresh(1 - s, frame) ? 1 - s : -1;
                if (g >= 0) {
                    const glm::mat4& Gn = st.gh[g].x;
                    if (!sd.glued || sd.ref != g) {
                        const glm::mat4 from = sd.glued ? sd.h_last : sd.h0;
                        sd.k                 = glm::inverse(Gn) * from;
                        sd.g0                = Gn;
                        if (sd.lines < 40) {
                            sd.lines++;
                            spdlog::info("[VehHands] drivehandsmodel: the {} hand {} the game's {} hand on the {} ({:.0f} cm from it): it goes where the game moves the "
                                         "vehicle's own {}", names[s], sd.glued ? "now rides" : "rides", names[g], ctl,
                                         glm::length(glm::vec3{ from * sd.lc } - glm::vec3{ Gn[3] }) / ws * 100.0f, ctl);
                        }
                        sd.glued          = true;
                        sd.ref            = g;
                        sd.glue_log_frame = frame;
                    }
                    H = Gn * sd.k;
                    // Every 2 s: how far the game moved its control under the hand, against the drive's own.
                    if (frame - sd.glue_log_frame >= 180 && sd.lines < 40) {
                        sd.glue_log_frame = frame;
                        sd.lines++;
                        if (cv.style == fc5::drive::kStick) {
                            const glm::vec3 mv = (glm::vec3{ Gn[3] } - glm::vec3{ sd.g0[3] }) / ws;
                            spdlog::info("[VehHands] drivehandsmodel: the {} hand on the stick: the game's stick hand moved ({:+.1f}, {:+.1f}, {:+.1f}) cm since the take, the "
                                         "drive's stick top ({:+.1f}, {:+.1f}, {:+.1f}) cm", names[s], mv.x * 100.0f, mv.y * 100.0f, mv.z * 100.0f,
                                         (top.x - sd.top0.x) / ws * 100.0f, (top.y - sd.top0.y) / ws * 100.0f, (top.z - sd.top0.z) / ws * 100.0f);
                        } else {
                            glm::quat q = glm::quat_cast(glm::mat3{ Gn } * glm::transpose(glm::mat3{ sd.g0 }));
                            if (q.w < 0.0f) q = -q;
                            const float phi = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
                            const float sgn = glm::dot(glm::vec3{ q.x, q.y, q.z }, axis) < 0.0f ? -1.0f : 1.0f;
                            spdlog::info("[VehHands] drivehandsmodel: the {} hand on the {}: the game turned them {:+.0f} deg since the take (positive: to the left), the "
                                         "drive's {:+.0f} deg", names[s], ctl, glm::degrees(sgn * phi), glm::degrees(cv.angle - sd.a0));
                        }
                    }
                } else if (sd.glued) {
                    H = sd.h_last;
                }
            }
            if (!ho.model) sd.glued = false;   // switched off while held: taken afresh if switched on again
            sd.h_last = H;
        } else if (sd.on) {
            sd.on     = false;
            sd.glued  = false;
            sd.ref    = -1;
            if (sd.lines < 40) {
                sd.lines++;
                spdlog::info("[VehHands] drivehandsgrab: the {} hand let go of the {}: back on its controller", names[s], ctl);
            }
        }
        // The game's own sidearm out, its glove of this side drawn: this hand is not drawn (every slot of the side folded to nothing).
        if (s == wside && !sd.on && game_gun && frame - m_hand_pivot_frame[s] <= 1) {
            apply(s, glm::mat4{ 1.0f }, true);
            sd.t       = glm::mat4{ 1.0f };
            sd.t_hide  = true;
            sd.t_frame = frame;
            say_state("not drawn: the game's own sidearm and hand are out");
            continue;
        }
        // Every slot of this side by the one move T = H X^-1 (world), then its fingers as on foot (fingergrip).
        const glm::mat4 T = to_world(H) * glm::inverse(X);
        apply(s, T, false);
        sd.t       = T;
        sd.t_hide  = false;
        sd.t_frame = frame;
        const bool fingers = gm && m_finger_grip.load();
        if (fingers) {
            m_finger_count = count;   // fingernames: the hand draw being posed, by its index count
            hand_fingers(pal, gm->cent, s, gm->hand_from, scale);
        }
        const std::string how = rec ? (s == wside ? std::string{ " (as on foot: the recorded hold)" } : fmt::format(" (as on foot: the {} hand's hold, mirrored)", names[wside]))
                                    : std::string{ " (the rest pose's axes: no recorded hold)" };
        say_state(sd.on ? fmt::format("on the {}", ctl) : fmt::format("on its controller{}{}", how, fingers ? ", fingers by grip and trigger" : ""),
                  fmt::format(": its palm {:.0f} cm from the eyes' base, which is {:.0f} cm from the game camera", glm::length(glm::vec3{ H * sd.lc }) * 100.0f,
                              glm::length(m_mo.draw_shift) * 100.0f));
    }
}

// drivewheelscan S (test, not saved; FarCry5Drive.h): while it runs, each Vehicle_ draw within 1.5 m of the camera, at most every 0.5 s per
// index count: its origin and axes in the axes of the vehicle's body (vehicle_probe's model under the seat: x across, y along, z up), its
// scale, and its skinning palette's size (VS b3, 0: none). A rigid steering wheel shows as a small draw ahead of the seat whose axes turn
// about one axis with the steering (steerstep); a wheel skinned in the body shows no such draw, only a palette.
void FarCry5Stereo::veh_wheel_scan(ID3D11DeviceContext* ctx, const float* f, UINT count, const glm::vec3& rel)
{
    if (!fc5::drive::wheel_scan_active() || m_frame.load() - m_veh_mesh_frame > 2) return;
    static std::mutex                                                mtx;
    static std::map<UINT, std::chrono::steady_clock::time_point> last;
    const auto                                                       now = std::chrono::steady_clock::now();
    {
        std::lock_guard _{ mtx };
        if (last.size() > 256) last.clear();
        auto& t = last[count];
        if (now - t < std::chrono::milliseconds(500)) return;
        t = now;
    }
    const glm::vec3 bx = glm::normalize(m_veh_mesh_x);
    const glm::vec3 by = glm::normalize(m_veh_mesh_y - glm::dot(m_veh_mesh_y, bx) * bx);
    const glm::vec3 bz = glm::cross(bx, by);
    auto            in_body = [&](const glm::vec3& v) { return glm::vec3{ glm::dot(v, bx), glm::dot(v, by), glm::dot(v, bz) }; };
    const glm::vec3 dx{ f[0], f[4], f[8] }, dz{ f[2], f[6], f[10] };
    const float     sc = glm::length(dx);
    if (sc < 1e-6f || glm::length(dz) < 1e-6f) return;
    const glm::vec3 o  = in_body(glm::vec3{ f[3], f[7], f[11] } - m_veh_mesh_o);
    const glm::vec3 ax = in_body(dx / sc), az = in_body(glm::normalize(dz));
    UINT            b3 = 0;
    ID3D11Buffer*   cb = nullptr;
    ctx->VSGetConstantBuffers(3, 1, &cb);
    if (cb) {
        D3D11_BUFFER_DESC d{};
        cb->GetDesc(&d);
        b3 = d.ByteWidth;
        cb->Release();
    }
    spdlog::info("[VehHands] wheelscan: draw {} {:.2f} m from the camera, at ({:+.2f}, {:+.2f}, {:+.2f}) m in the vehicle's axes (across, along, up); its x axis "
                 "({:+.3f}, {:+.3f}, {:+.3f}), z axis ({:+.3f}, {:+.3f}, {:+.3f}); scale {:.4f}; VS b3 {} B; drive {}", count, glm::length(rel), o.x, o.y, o.z,
                 ax.x, ax.y, ax.z, az.x, az.y, az.z, sc, b3, fc5::drive::status());
}

// vehdump full|pal NAME (test, not saved; vehcontrol research, author 9 Nov 08:36): in the next frame, every draw whose object origin (VS b1) is within 3.5 m of
// the camera, world draws (kind w: the vehicle's parts, whatever their material) and the player's own hidden body draws (kind b: the arms on
// the wheel), written once each to VRMod_dev\dumps\vehdump_NAME\: its VS b1 matrix, the vertex scale (VS b2), its skinning palette (VS b3, the
// CPU copy) and, for "full", its vertices (position, palette slots and weights; round 2: the position's 4th component and the vertex's bytes). Read by tools\wheelglue\vehdump_fit.py. "pal": no vertices
// (cheap: the bones at another steering angle). A draw recorded on a deferred context has its vertex read done at the next immediate draw.
namespace
{
    struct VDumpJob
    {
        std::string   path, head;
        ID3D11Buffer* vb{ nullptr };
        ID3D11Buffer* ib{ nullptr };
        UINT          stride{ 0 }, voff{ 0 }, ib_off{ 0 }, count{ 0 }, start{ 0 };
        INT           base{ 0 };
        bool          i32{ false };
        int           pos_off{ -1 }, pos_fmt{ 0 }, bi_off{ -1 }, w_off{ -1 };
        bool          w_bgra{ false };
    };
    struct VDump
    {
        std::mutex                                     mtx;
        int                                            mode{ 0 };   // 0 off, 1 full, 2 pal
        int                                            from{ 0 }, until{ 0 };
        std::string                                    dir, name;
        std::set<std::tuple<char, UINT, UINT, INT, void*>> done;
        std::vector<VDumpJob>                          jobs;
        std::map<ID3D11Buffer*, std::vector<uint8_t>>  cache;   // buffers read in this dump
        int                                            files{ 0 };
        bool                                           frame_written{ false };
    };
    VDump g_vd;

    void vdump_write(ID3D11DeviceContext* ctx, VDumpJob& j)
    {
        std::ofstream f(j.path, std::ios::out | std::ios::trunc);
        if (!f) return;
        f << j.head;
        if (j.vb && j.ib && j.pos_off >= 0) {
            auto get = [&](ID3D11Buffer* b) -> const std::vector<uint8_t>* {
                auto it = g_vd.cache.find(b);
                if (it == g_vd.cache.end()) {
                    std::vector<uint8_t> d;
                    if (g_vd.cache.size() > 48 || !read_gpu(ctx, b, d)) return nullptr;
                    it = g_vd.cache.emplace(b, std::move(d)).first;
                }
                return &it->second;
            };
            const std::vector<uint8_t>* vbd = get(j.vb);
            const std::vector<uint8_t>* ibd = get(j.ib);
            if (vbd && ibd) {
                std::set<uint32_t>   seen;
                std::vector<int64_t> order;
                std::ostringstream   tri;
                for (UINT k = 0; k < j.count; ++k) {
                    const size_t io = size_t(j.ib_off) + size_t(j.start + k) * (j.i32 ? 4 : 2);
                    if (io + (j.i32 ? 4 : 2) > ibd->size()) break;
                    uint32_t idx = 0;
                    std::memcpy(&idx, ibd->data() + io, j.i32 ? 4 : 2);
                    if (seen.insert(idx).second) order.push_back(idx);
                }
                f << "verts " << order.size() << " (qx qy qz  slot0..3  weight0..3  qw  the vertex's bytes; position x the scale, then the slot's palette matrix)\n";
                for (const int64_t idx : order) {
                    const size_t vo = size_t(j.voff) + size_t(idx + j.base) * j.stride;
                    if (vo + j.stride > vbd->size()) continue;
                    const uint8_t* v = vbd->data() + vo;
                    float p[3]{};
                    int   qw = 0;
                    if (j.pos_fmt == DXGI_FORMAT_R16G16B16A16_SINT) {
                        int16_t q[4];
                        std::memcpy(q, v + j.pos_off, 8);
                        p[0] = q[0], p[1] = q[1], p[2] = q[2];
                        qw   = q[3];   // round 2: the 4th component (a vehicle's draws have no BLENDINDICES: its bone, perhaps)
                    } else {
                        std::memcpy(p, v + j.pos_off, 12);
                    }
                    int sl[4]{}, w[4]{ 255, 0, 0, 0 };
                    if (j.bi_off >= 0) {
                        const uint8_t* bi = v + j.bi_off;
                        for (int c = 0; c < 4; ++c) sl[c] = bi[j.w_bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c];
                    }
                    if (j.w_off >= 0) {
                        const uint8_t* wp = v + j.w_off;
                        for (int c = 0; c < 4; ++c) w[c] = wp[c];
                    }
                    f << p[0] << ' ' << p[1] << ' ' << p[2] << "  " << sl[0] << ' ' << sl[1] << ' ' << sl[2] << ' ' << sl[3] << "  " << w[0] << ' ' << w[1] << ' '
                      << w[2] << ' ' << w[3] << "  " << qw << ' ';
                    for (UINT b = 0; b < j.stride; ++b) f << fmt::format("{:02x}", v[b]);
                    f << '\n';
                }
            } else {
                f << "verts 0 (the vertex or index buffer could not be read)\n";
            }
        }
        g_vd.files++;
    }
}

void FarCry5Stereo::veh_dump_arm(const std::string& mode, const std::string& name)
{
    std::lock_guard _{ g_vd.mtx };
    for (auto& j : g_vd.jobs) { if (j.vb) j.vb->Release(); if (j.ib) j.ib->Release(); }
    g_vd.jobs.clear();
    g_vd.cache.clear();
    g_vd.done.clear();
    g_vd.files         = 0;
    g_vd.frame_written = false;
    g_vd.name          = name.empty() ? std::string{ "x" } : name;
    const std::string root = "fc5vr_dumps";
    CreateDirectoryA(root.c_str(), nullptr);
    g_vd.dir = root + "/vehdump_" + g_vd.name;
    CreateDirectoryA(g_vd.dir.c_str(), nullptr);
    g_vd.mode  = mode == "pal" ? 2 : 1;
    g_vd.from  = m_frame.load() + 1;
    g_vd.until = g_vd.from;   // round 2: one frame (each draw once: a palette moves between frames)
    m_keep_bones = true;   // the palettes' CPU copies (note_object_constants)
    spdlog::info("[VehHands] vehdump {} {}: the draws within 3.5 m of the camera in the next frame, to {}", g_vd.mode == 2 ? "pal" : "full", g_vd.name, g_vd.dir);
}

void FarCry5Stereo::veh_dump_draw(ID3D11DeviceContext* ctx, int kind, UINT count, UINT start, INT base)
{
    if (g_vd.mode == 0) return;
    const int  frame    = m_frame.load();
    const bool deferred = fc5::rec::is_deferred(ctx);
    std::lock_guard _{ g_vd.mtx };
    if (g_vd.mode == 0) return;
    // The vertex reads waiting for an immediate context.
    if (!deferred && !g_vd.jobs.empty()) {
        for (auto& j : g_vd.jobs) {
            vdump_write(ctx, j);
            if (j.vb) j.vb->Release();
            if (j.ib) j.ib->Release();
        }
        g_vd.jobs.clear();
    }
    if (frame < g_vd.from) return;
    if (frame > g_vd.until) {
        if (!g_vd.jobs.empty() && frame < g_vd.until + 30) return;   // still waiting for an immediate draw
        spdlog::info("[VehHands] vehdump {}: {} draws written to {}{}", g_vd.name, g_vd.files, g_vd.dir,
                     g_vd.jobs.empty() ? "" : fmt::format(" ({} deferred draws never read)", g_vd.jobs.size()));
        for (auto& j : g_vd.jobs) { if (j.vb) j.vb->Release(); if (j.ib) j.ib->Release(); }
        g_vd.jobs.clear();
        g_vd.cache.clear();
        g_vd.mode = 0;
        return;
    }
    if (!g_vd.frame_written) {
        g_vd.frame_written = true;
        std::ofstream f(g_vd.dir + "/frame.txt", std::ios::out | std::ios::trunc);
        f << "frame " << frame << "\ncam " << m_world_cam_pos.x << ' ' << m_world_cam_pos.y << ' ' << m_world_cam_pos.z << "\nvehmesh_o " << m_veh_mesh_o.x << ' '
          << m_veh_mesh_o.y << ' ' << m_veh_mesh_o.z << "\nvehmesh_x " << m_veh_mesh_x.x << ' ' << m_veh_mesh_x.y << ' ' << m_veh_mesh_x.z << "\nvehmesh_y "
          << m_veh_mesh_y.x << ' ' << m_veh_mesh_y.y << ' ' << m_veh_mesh_y.z << "\nvehmesh_frame " << m_veh_mesh_frame << "\nin_vehicle " << int(m_in_vehicle.load())
          << " driving " << int(m_veh_driving.load()) << "\ndrive " << fc5::drive::status() << '\n';
    }
    // b1: the object's world matrix (3x4 rows); only the draws near the camera.
    float b1[12]{};
    {
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(1, 1, &cb);
        if (!cb) return;
        std::lock_guard o{ m_obj_mtx };
        const auto      it = m_obj_consts.find(cb);
        cb->Release();
        if (it == m_obj_consts.end()) return;
        std::memcpy(b1, it->second.f, sizeof(b1));
    }
    const glm::vec3 rel = glm::vec3{ b1[3], b1[7], b1[11] } - m_world_cam_pos;
    if (glm::length(rel) > 3.5f || g_vd.done.size() > 400) return;
    ID3D11Buffer* vb = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ib_fmt{};
    UINT          ib_off = 0;
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    float scale = 0.0f;
    std::vector<float> pal;
    {
        ID3D11Buffer* cb2 = nullptr;
        ID3D11Buffer* cb3 = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cb2);
        ctx->VSGetConstantBuffers(3, 1, &cb3);
        std::lock_guard o{ m_obj_mtx };
        if (cb2) {
            if (auto it = m_obj_consts.find(cb2); it != m_obj_consts.end()) scale = it->second.f[5];
            cb2->Release();
        }
        if (cb3) {
            if (auto it = m_bone_data.find(cb3); it != m_bone_data.end()) pal = it->second;
            cb3->Release();
        }
    }
    // Round 2: one draw = its count, start, base, vertex buffer and palette (the two bare arms share a mesh and differ by their palettes).
    uint64_t ph = 1469598103934665603ull;
    for (size_t k = 0; k < pal.size() && k < 12 * 64; ++k) {
        uint32_t u = 0;
        std::memcpy(&u, &pal[k], 4);
        ph = (ph ^ u) * 1099511628211ull;
    }
    const auto key = std::make_tuple(kind == 1 ? 'b' : 'w', count, start, base, (void*)(uintptr_t(vb) ^ uintptr_t(ph)));
    if (!g_vd.done.insert(key).second) {
        if (vb) vb->Release();
        if (ib) ib->Release();
        return;
    }
    bool vehicle_ps = false;
    {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            std::lock_guard p{ m_ps_mtx };
            vehicle_ps = m_vehicle_shaders.contains(ps);
            ps->Release();
        }
    }
    VDumpJob j;
    // Round 2: the same count, start and base can be two draws (the left and right bare arms: 4110 each): a number for each other buffer.
    int nth = 0;
    for (const auto& k : g_vd.done)
        if (std::get<0>(k) == std::get<0>(key) && std::get<1>(k) == count && std::get<2>(k) == start && std::get<3>(k) == base && std::get<4>(k) != std::get<4>(key)) nth++;
    j.path = fmt::format("{}/{}_{}_{}_{}{}{}.txt", g_vd.dir, kind == 1 ? 'b' : 'w', count, start, base, nth ? fmt::format("_{}", nth) : std::string{}, g_vd.mode == 2 ? "_pal" : "");
    std::ostringstream h;
    h << "draw " << (kind == 1 ? "body" : "world") << " count " << count << " start " << start << " base " << base << " stride " << stride << " frame " << frame
      << " vehicle_ps " << int(vehicle_ps) << " deferred " << int(deferred) << '\n';
    h << "b1";
    for (float v : b1) h << ' ' << v;
    h << "\ncam " << m_world_cam_pos.x << ' ' << m_world_cam_pos.y << ' ' << m_world_cam_pos.z << "\nscale " << scale << '\n';
    // The palette's slots up to the last one that is not all zero.
    int last = -1;
    for (int s = 0; s < 256 && size_t(s + 1) * 12 <= pal.size(); ++s)
        for (int k = 0; k < 12; ++k)
            if (pal[size_t(s) * 12 + k] != 0.0f) { last = s; break; }
    h << "pal " << (last + 1) << '\n';
    for (int s = 0; s <= last; ++s) {
        for (int k = 0; k < 12; ++k) h << (k ? " " : "") << pal[size_t(s) * 12 + k];
        h << '\n';
    }
    j.head = h.str();
    if (g_vd.mode == 1 && vb && ib) {
        ID3D11InputLayout* il = nullptr;
        ctx->IAGetInputLayout(&il);
        std::vector<LayoutElem> layout;
        {
            std::lock_guard p{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        if (il) il->Release();
        std::string all;   // round 2: every element of the layout (semantic index, format, stream, offset)
        for (const auto& e : layout) all += fmt::format(" {}{}:{}:s{}:+{}", e.semantic, e.index, int(e.format), e.slot, e.offset);
        j.head += "layout_all" + all + "\n";
        for (const auto& e : layout) {
            if (e.index != 0 || e.slot != 0) continue;
            if (_stricmp(e.semantic.c_str(), "POSITION") == 0 && (e.format == DXGI_FORMAT_R16G16B16A16_SINT || e.format == DXGI_FORMAT_R32G32B32_FLOAT ||
                                                                 e.format == DXGI_FORMAT_R32G32B32A32_FLOAT)) {
                j.pos_off = int(e.offset);
                j.pos_fmt = int(e.format);
            }
            if (_stricmp(e.semantic.c_str(), "BLENDINDICES") == 0) j.bi_off = int(e.offset);
            if (_stricmp(e.semantic.c_str(), "BLENDWEIGHT") == 0) { j.w_off = int(e.offset); j.w_bgra = e.format == DXGI_FORMAT_B8G8R8A8_UNORM; }
        }
        j.head += fmt::format("layout pos {} fmt {} blendindices {} blendweight {}{}\n", j.pos_off, j.pos_fmt, j.bi_off, j.w_off, j.w_bgra ? " bgra" : "");
        if (stride == 0 || stride > 256) j.pos_off = -1;
        j.vb = vb; j.ib = ib; vb = nullptr; ib = nullptr;   // kept (one reference each) until written
        j.stride = stride; j.voff = voff; j.ib_off = ib_off; j.count = count; j.start = start; j.base = base; j.i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
    }
    if (vb) vb->Release();
    if (ib) ib->Release();
    if (deferred && j.vb) {
        g_vd.jobs.push_back(std::move(j));
    } else {
        vdump_write(ctx, j);
        if (j.vb) j.vb->Release();
        if (j.ib) j.ib->Release();
    }
}
