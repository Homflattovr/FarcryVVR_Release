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
#include "FarCry5Lua.h"
#include "FarCry5Record.h"
#include "FarCry5VehWheelTable.h"
#include "FarCry5VehControlFit.h"
#include "ModPath.h"

#include <spdlog/spdlog.h>

#include <glm/gtc/matrix_access.hpp>
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

    // vehcontrol (veh_control_draw at the end of this file; FarCry5VehControlFit.h): the vehicle's own wheel, bars or stick, found when you
    // sit. A vehicle's draws are skinned at its origin with a palette of 256 bones; they have no BLENDINDICES: each vertex's bone is its
    // position's 4th component (the 9 Nov round 2 dumps: the Compact's steering bone holds 1706 vertices over 8 slots of two draws, turning
    // 143 deg at full lock; the rim from its own vertices is 16-20 cm on the four cars read, with the game's hands on it).
    struct VcEntry   // fc5vr_vehcontrol.txt: one vehicle model's control, in its bone's bind pose (game metres)
    {
        int       style{ 0 };   // fc5::drive::kWheel, kBars, kStick
        UINT      count{ 0 };   // the draw's index count
        int       slot{ -1 };   // the bone's palette slot in that draw (-1: no bone found: kept where it was found, in the seat's frame)
        int       nverts{ 0 };  // that draw's vertices on that slot (the model's fingerprint with the count)
        glm::vec3 c{ 0.0f };    // wheel: the rim's centre; bars: their centre; stick: its top
        glm::vec3 n{ 0.0f, 0.0f, 1.0f };   // wheel: the axis towards the driver; bars: the steering axis (up); stick: its long axis
        float     r{ 0.0f };    // wheel: the rim's radius; bars: the half width between the hands
        int       sure{ 0 };    // 1: its turn with the steering was seen (the animation agreed)
    };
    struct VcDraw   // a vehicle draw near the driver this drive (count, start, base, vertex buffer)
    {
        UINT               count{ 0 }, start{ 0 };
        INT                base{ 0 };
        void*              vb{ nullptr };
        std::vector<float> pal_ref, pal_now;   // the palette at the first sight (the reference for the first steer), and this frame's
        glm::mat4          b1{ 1.0f };
        glm::vec3          eye{ 0.0f };        // the eyes (world) when it was last seen (the same frame as b1 and pal_now)
        bool               seat{ false };
        float              scale{ 0.0f };      // VS b2: the vertex positions' scale
        int                frame{ -1000 };
        bool               has_verts{ false }, queued{ false };
        int                nslot{ 256 };       // the slots its vertices use (the highest + 1), once read
        // drivemodelshow (veh_control_show): its slots that belong to the control's bone, each with its pose relative to the draw's slot 0
        // when taken, and the bone's axis in slot 0's frame (a point on it, its direction: a positive turn about it is the bone's own).
        bool                                    ov{ false };
        std::vector<std::pair<int, glm::mat4>>  ov_slots;
        glm::vec3                               ov_ax{ 0.0f, 0.0f, 1.0f }, ov_piv{ 0.0f };
        std::vector<std::array<int16_t, 4>> verts;   // each vertex once: x y z, w = its bone
    };
    struct VcJob   // a vertex read waiting for an immediate context (the draw was recorded on a deferred one)
    {
        int           d{ -1 };
        ID3D11Buffer* vb{ nullptr };
        ID3D11Buffer* ib{ nullptr };
        UINT          stride{ 0 }, voff{ 0 }, ib_off{ 0 }, count{ 0 }, start{ 0 };
        INT           base{ 0 };
        bool          i32{ false };
        UINT          pos_off{ 0 };
        int           bi_off{ -1 }, w_off{ -1 };   // BLENDINDICES / BLENDWEIGHT (the boats' draws): the vertex's bone is its heaviest
        bool          w_bgra{ false };
    };
    struct VcState
    {
        std::mutex mtx;
        int        cur_frame{ -1000 }, last_frame{ -1000 };
        int        style{ 0 };
        int        state{ 0 };   // 0 looking (at the sit, then at the first steer), 2 following, 3 not found this drive
        std::vector<VcDraw> draws;
        std::vector<VcJob>  jobs;
        std::map<ID3D11Buffer*, std::vector<uint8_t>> cache;   // buffers read back while the draws are collected
        int        reads{ 0 }, read_frame{ -1000 }, reads_in_frame{ 0 };
        int        settled_frame{ -1000 };    // the first frame seated and settled this drive
        int        tries{ 0 }, try_frame{ -1000 };
        bool       drive_on{ false };         // gesture driving (drive on), read once a frame
        float      anim_ang{ 0.0f };          // the turn the animation was last weighed at
        bool       tried_hands{ false }, hinted{ false }, nudged{ false };
        // The game's entry animation (vehscript: in a vehicle, its seat not yet given): nothing is looked for during it, and what was looked
        // for before it ended is looked for again. The author's plane, 12 Nov 20:52:09: the view settled 0.7 s into a 4.4 s entry, the three
        // tries and the four turning-part tries were spent in the next half second on the 1700 vertices seen from the door, and the stick
        // was "not found in this vehicle's model" 3 s before he sat; his van a minute earlier lost two tries the same way.
        bool       entering{ false };
        int        enter_frame{ -100000 };
        int        hand_arm_frame[2]{ -100000, -100000 };   // vc_note_hand: when each hand's palm was last read from its own arm's mesh
        int        hold_tries{ 0 }, hold_frame{ 0 };   // handlebars: the looks for the game's two-hand hold after a find without it
        glm::vec3  hand_w[2]{};               // the game's hands (veh_hands_place: the palms' centres, world, before the mod moves them)
        // The detection's frame: the eyes, with the driven vehicle's own axes (x across to the right, y up, z back). Not the view's: at the
        // sit the view can be far off the vehicle's front (pass 5, 11 Nov: 58 deg in the heavy truck, 23 in the Offroad, whose wheels then lay
        // to the side or half behind the eyes' plane and were missed or found late). Sd: world -> that frame, as of the last cloud built;
        // home: a draw of the driven vehicle (its VS b1 gives the axes).
        glm::mat4  Sd{ 1.0f };
        int        home{ -1 };
        int        hand_frame[2]{ -1000, -1000 };
        // The animation: the slot that turned the most since the first sight, the same two frames in a row.
        struct Best
        {
            float     ang{ 0.0f };
            int       d{ -1 }, slot{ -1 }, frame{ -1000 };
            glm::vec3 ax_w{ 0.0f }, piv_w{ 0.0f };   // the axis and its point nearest the eyes (world)
        };
        std::vector<Best> cands, prev_cands;   // this frame's turning slots (the 8 largest turns), and the frame before's
        int        cand_frame{ -1000 };
        bool       by_hands{ false };         // bars: placed where the game's two hands hold
        bool       told{ false };             // the drive was told the other control once this drive (wheel <-> handlebars)
        std::map<std::pair<void*, UINT>, std::array<int32_t, 5>> args;   // indirect draws' arguments read this drive (buffer, offset)
        int        anim_tries{ 0 };
        // Following: the key and the control in the key slot's bind pose (slot -1: in the seat's frame).
        VcEntry    key;
        int        key_draw{ -1 };
        int        adopt_frame{ -1000 };
        const char* source{ "" };
        float      conf{ 0.0f };
        float      turn{ 0.0f }, turn_raw{ 0.0f };   // the bone's turn about its axis (rad, unwrapped; positive: to the left)
        bool       turn_ok{ false }, turn_any{ false };
        // This frame's control in the reference (game metres; the controllers' space), for veh_hands_place.
        int        out_frame{ -1000 };
        float      out_d{ 1e9f };
        glm::mat4  G_ref{ 1.0f };
        glm::vec3  c_ref{ 0.0f }, n_ref{ 0.0f, 0.0f, 1.0f };
        bool       said_follow{ false };
        // drivemodelshow: the control drawn at the hand's angle. ov_ok: every slot of the bone is known (its turn was seen this drive);
        // before that, a wheel's own slot only. ov_turn_c: the bone's turn (turn) when the slots' poses were taken.
        bool       ov_ok{ false }, ov_said{ false };
        // The followed bone's centre in the vehicle's own space when first followed, and the farthest from it said so far (a log line).
        glm::vec3  piv0{ 0.0f };
        int        piv_draw{ -2 }, piv_slot{ -2 };
        float      piv_said{ 0.0f };
        // A stick's bone: its lean from where it was first followed (a log line: what the game does with the left stick it is sent).
        glm::mat3  lean_R0{ 1.0f };
        int        lean_draw{ -2 }, lean_slot{ -2 }, lean_frame{ 0 }, lean_lines{ 0 };
        float      lean_said{ 0.0f };
        int        fine_until{ 0 }, fine_frame{ 0 };   // the fine lines while a steerstep runs and 3 s after it
        float      ov_turn_c{ 0.0f };
        std::vector<UINT> ov_counts;
        glm::vec3  key_b1{ 0.0f };            // the followed vehicle's origin (world), to leave a parked twin's wheel alone
        bool       show_on{ false };          // fc5::drive::model_show, read once a stereo pair
        float      show_turn{ 0.0f };
        int        show_frame{ -1000 };
        bool       table_loaded{ false };
        std::vector<VcEntry> table;
    };
    VcState g_vc;

    // veh_hands_place: the control bone's frame in the reference this frame (its rotation, at the rim's centre / the bars' centre / the stick's
    // top), and whether its search failed this drive (then the wheel is learnt from the game's hands as before).
    bool vc_pose(int frame, int style, glm::mat4& G, bool& failed)
    {
        std::lock_guard _{ g_vc.mtx };
        failed = g_vc.style == style && g_vc.state == 3;
        if (g_vc.style != style || g_vc.state != 2 || frame - g_vc.out_frame > 1) return false;
        G = g_vc.G_ref;
        return true;
    }
    // veh_hands_place: a game hand's palm centre (world), where the game animates it on the control: one of the detection's sources.
    // from_arm: read from a one-arm mesh (that arm's own palm slot); else from a pair of gloves. An arm's palm read in the last 3 frames is
    // kept over the gloves': the gloves' right palm is not on the control in the passes (jetski, 12 Nov: (-30, -25, +19) cm, behind the
    // eyes, its mesh's "RightHand joint 57 cm" from its vertices; the JacobSeed plane, 13 Nov: on the stick in the dump, not in the live run).
    void vc_note_hand(int s, const glm::vec3& p, int frame, bool from_arm)
    {
        std::lock_guard _{ g_vc.mtx };
        if (from_arm) g_vc.hand_arm_frame[s] = frame;
        else if (frame - g_vc.hand_arm_frame[s] <= 3) return;
        g_vc.hand_w[s]     = p;
        g_vc.hand_frame[s] = frame;
    }
}

struct FarCry5Stereo::VehHandsState
{
    int                frame_seen{ -1000 };   // the last frame a draw came here (a gap of 2 s starts a new drive's log)
    std::set<uint64_t> said;                  // (count, outcome) logged this drive
    uint64_t           shown{ 0 }, hidden{ 0 }, drives{ 0 };
    bool               keep_bones_was{ false };
    std::vector<float> pal;                   // the palette being built (render thread)
    std::vector<float> game;                  // the game's palette of that draw (bodyikveh)
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
        // drivehandhold (15 Nov, tester's first launch: the left hand on the wheel there or not): once this side's recorded hold (its own, or
        // the other hand's mirrored) has put the palm over 12 cm from the controller, it stays left out for the drive. Before, the left
        // hand's mirrored hold was tried again every frame and sat right at the limit (12 cm, at +5 -8 +8 cm): the hand switched between
        // the two poses (121 switches in the tester's first log), its pose on the wheel jumping between them with it.
        bool        hold_far{ false };   // (not "far": a Windows macro)
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
            spdlog::info("[VehHands] drivehandsmodel: the vehicle's {} from the game's hands: ({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes (right, up, back, the recentred view's axes){}",
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

    void game_hand(int s, const glm::mat4& x, int frame, float ws, int style, bool learn = true)
    {
        gh[s].x     = x;
        gh[s].frame = frame;
        auto& h     = hist[s][frame % kHist];
        h.x         = x;
        h.frame     = frame;
        if (style != fc5::drive::kWheel) return;   // the wheel only (a helicopter's hands learnt "wheels" 76 cm under the eyes in round 1)
        if (!learn) return;                        // vehcontrol: the wheel from the vehicle's own bone instead
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

bool FarCry5Stereo::veh_hands_active() const
{
    return fc5::drive::hands_mode() == 1 && m_motion.load() && m_in_vehicle.load() && m_veh_driving.load() && m_veh_settle.load() == 0 && !m_chute_vehicle && !m_cine_on &&
           !flat_screen_on();
}

// bodyikveh: the move drivehandsgrab gave that side's hand (path B, world), this frame or the last; hidden: it draws no hand there.
bool FarCry5Stereo::veh_hand_move(int side, glm::mat4& t, bool& hidden, bool& held) const
{
    if (!m_vh || side < 0 || side > 1) return false;
    const auto& sd = m_vh->side[side];
    if (m_frame.load() - sd.t_frame > 1) return false;
    t      = sd.t;
    hidden = sd.t_hide;
    held   = sd.on;   // drivehandsgrab: on the wheel, the bars or the stick (else on its controller)
    return true;
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
        // vehbodyindirect (default on since 23 Nov 14:05Z, author): with the game's
        // sidearm out while driving the seated body comes only as indirect draws (7725 and 3210 indices; the direct 8988/13545 that gave the
        // shoulders stop), so the left hand's arm had no fresh shoulder ("[BodyIk] seated: the left hand moved but its arm has no solve yet:
        // drivehands' fold kept" every 2 s, 13:58-15:04) and only the hand showed. Such a draw is read for its joints (bodyik_veh_joints) and
        // stays hidden: the hand draws' arms are solved from it, and no sleeve shows in the game's own pose. Off: hidden unread, as before.
        if (gm && gm->ok && !gm->gloves && gm->arm_side < 0 && m_veh_body_indirect.load() && bodyik_veh_joints(ctx, n, start, base, palette_cb)) {
            st.hidden++;
            say(n, 13, fmt::format("hidden, its shoulders read for the arms ({} bones, the seated body drawn indirect; vehbodyindirect)", gm->bones));
            return false;
        }
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
            st.pal  = it->second;
            st.game = it->second;   // bodyikveh: the game's pose of the rest of the body
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
        bodyik_veh(ctx, st.pal, st.game, n, start, base, true);   // bodyikveh: the arm from the seated shoulder to the hand (else as above)
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
        st.pal  = it->second;
        st.game = it->second;   // bodyikveh
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
    bodyik_veh(ctx, st.pal, st.game, n, start, base, true);   // bodyikveh
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
    // vehcontrol (veh_control_draw): the vehicle's own control bone this frame, in the reference. While on, the vehwheels table, driveautoturn
    // and the wheel learnt from the game's hands are left out (that wheel is learnt again only in a drive where the bone's rim was not found).
    const bool vc_on     = m_vehcontrol.load() && have_cv;
    glm::mat4  vc_G{ 1.0f };
    bool       vc_failed = false;
    const bool vc_ok     = vc_on && vc_pose(frame, cv.style, vc_G, vc_failed);
    if (ho.model && have_cv && !vc_on) {
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
        if (mesh_side && s == wside && !sd.hold_far) {
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
        } else if (mesh_side && s != wside && st.rel_ok && !sd.hold_far) {
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
                rec    = false;
                sd.hold_far = true;   // drivehandhold: left out for the rest of the drive
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
                st.game_hand(s, to_ref(G), frame, ws, cv.style, !vc_on || vc_failed);
                if (vc_on) vc_note_hand(s, cw, frame, gm && !gm->gloves && gm->arm_side == s);   // vehcontrol: where the game holds the control, one of its sources
                if (!vc_ok) st.send_model(cv.style, frame, ws, cv_pt(cv.stick_top));   // vehcontrol: the bone's control sent instead
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
            // vehcontrol: rides the vehicle's own control bone (wheel, bars or stick) from where the hand took it; that bone not drawn this
            // frame: where it was.
            if (ho.model && vc_on && (vc_ok || (sd.glued && sd.ref == 2))) {
                if (vc_ok) {
                    if (!sd.glued || sd.ref != 2) {
                        const glm::mat4 from = sd.glued ? sd.h_last : sd.h0;
                        sd.k                 = glm::inverse(vc_G) * from;
                        sd.g0                = vc_G;
                        sd.glued             = true;
                        sd.ref               = 2;
                        sd.glue_log_frame    = frame;
                        if (sd.lines < 40) {
                            sd.lines++;
                            spdlog::info("[VehHands] vehcontrol: the {} hand rides the vehicle's own {} bone ({:.0f} cm from its {} when taken)", names[s], ctl,
                                         glm::length(glm::vec3{ from * sd.lc } - glm::vec3{ vc_G[3] }) / ws * 100.0f,
                                         cv.style == fc5::drive::kWheel ? "rim's centre" : cv.style == fc5::drive::kBars ? "centre" : "top");
                        }
                    }
                    H = vc_G * sd.k;
                    if (frame - sd.glue_log_frame >= 180 && sd.lines < 40) {   // every 2 s: the game's turn of its control against the drive's own
                        sd.glue_log_frame = frame;
                        sd.lines++;
                        glm::quat q = glm::quat_cast(glm::mat3{ vc_G } * glm::transpose(glm::mat3{ sd.g0 }));
                        if (q.w < 0.0f) q = -q;
                        const float phi = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
                        const float sgn = glm::dot(glm::vec3{ q.x, q.y, q.z }, axis) < 0.0f ? -1.0f : 1.0f;
                        spdlog::info("[VehHands] vehcontrol: the {} hand on the {}: the game turned its bone {:+.0f} deg since the take (positive: to the left), the "
                                     "drive's {:+.0f} deg", names[s], ctl, glm::degrees(sgn * phi), glm::degrees(cv.angle - sd.a0));
                    }
                } else {
                    H = sd.h_last;
                }
            } else if (ho.model && cv.style == fc5::drive::kWheel) {
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
            // drivegripfist (default on; author 15 Nov): on the wheel, bars or stick the whole hand closes round it by the grip alone.
            hand_fingers(pal, gm->cent, s, gm->hand_from, scale, false, sd.on && ho.fist);
        }
        const std::string how = rec ? (s == wside ? std::string{ " (as on foot: the recorded hold)" } : fmt::format(" (as on foot: the {} hand's hold, mirrored)", names[wside]))
                                    : std::string{ " (the rest pose's axes: no recorded hold)" };
        say_state(sd.on ? fmt::format("on the {}{}", ctl, fingers && ho.fist ? ", the whole hand closed round it (drivegripfist)" : "") : fmt::format("on its controller{}{}", how, fingers ? ", fingers by grip and trigger" : ""),
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
// the wheel), written once each to fc5vr_dumps\vehdump_NAME\: its VS b1 matrix, the vertex scale (VS b2), its skinning palette (VS b3, the
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
    // An indirect draw (no count here; the van's rim, the boats' parts): its index count, start and base from its arguments.
    if (count == 0 && !deferred && m_hud_draw_kind == 5 && m_hud_draw_args) {
        std::array<int32_t, 5> ia{};
        if (indirect_args(ctx, m_hud_draw_args, m_hud_draw_args_off, ia) && ia[0] > 0) {
            count = UINT(ia[0]);
            start = UINT(std::max(0, ia[2]));
            base  = INT(ia[3]);
        }
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

// vehcontrol on|off (saved, default on; F11 Layout > Vehicles; author 11 Nov 09:09). The detection itself is FarCry5VehControlFit.h
// (pure geometry, also run offline by research\2026-11-11_vehcontrol2\vcfit_harness.cpp); this feeds it and follows the result:
//  0. A table first (bin\fc5vr_vehcontrol.txt): a vehicle model found before (its draw's index count, the bone's slot, that draw's vertex
//     count on the slot) is taken at once.
//  1. Collecting, from the moment the seat is taken (the camera still settling): every vehicle draw (a Vehicle_ material) within 3.5 m of
//     the camera has its vertices read once (position, and its 4th component: the vertex's bone) and its palette copied each frame.
//  2. At the sit (the first settled frame with the reads in): the vertices around the driver, placed by their bones, in the seat's frame
//     (the drive's recentred reference: the eyes, x right, y up, z back), go to fc5::vcfit::detect with the game's hands' palms when seen
//     (veh_hands_place): geometry (a ring, two grips, a thin upright part), spatial (ahead, below the eyes, centred, the hands on it).
//     Tried again when the game's hands are first seen, and 120 frames on. With no control known for the vehicle yet (the game's type not
//     answered), a wheel or bars found sure enough is handed to the drive as its style (fc5::drive::set_shape_hint).
//  3. The animation: the slot that turns with the steering (5 deg or more, sticks 3 to 45, relative to its draw's slot 0, about an axis near
//     the eyes, the same two frames in a row: not a rotor) confirms the control, corrects it (the shape searched about that axis in the
//     parts that turn), or finds it when step 2 could not (then one quick small turn is sent, driveautoturn, so it comes at once).
//  4. Every frame: VS b1 x palette[slot] places the control; the drive's wheel, bars or stick is set there with the bone's own turn about its
//     axis (fc5::drive::set_model: the steering follows the model's wheel), and veh_hands_place lets a held hand ride the bone.
namespace
{
    const char* vc_style_name(int s) { return s == fc5::drive::kWheel ? "wheel" : s == fc5::drive::kBars ? "handlebars" : s == fc5::drive::kStick ? "stick" : "control"; }
    int         vc_style_of(const std::string& s) { return s == "wheel" ? fc5::drive::kWheel : s == "bars" ? fc5::drive::kBars : s == "stick" ? fc5::drive::kStick : 0; }

    void vc_load()
    {
        if (g_vc.table_loaded) return;
        g_vc.table_loaded = true;
        std::ifstream f{ fc5::mod_file("fc5vr_vehcontrol.txt") };
        std::string   line;
        while (std::getline(f, line)) {
            // (control2, builds before 11 Nov round 4: dropped; the quad's front rack was saved as its bars. control3, before 12 Nov round 6:
            // dropped; the jetski's 42 cm part was saved as its bars and kept the round 5 build from looking again)
            if (line.rfind("control4 ", 0) != 0) continue;
            std::istringstream ls{ line.substr(9) };
            std::string        st;
            VcEntry            e;
            if (!(ls >> st >> e.count >> e.slot >> e.nverts >> e.c.x >> e.c.y >> e.c.z >> e.n.x >> e.n.y >> e.n.z >> e.r >> e.sure)) continue;
            e.style = vc_style_of(st);
            if (e.style == 0 || e.nverts <= 0 || e.slot < 0 || e.slot > 255 || !std::isfinite(e.c.x + e.c.y + e.c.z + e.n.x + e.n.y + e.n.z + e.r)) continue;
            g_vc.table.push_back(e);
        }
        spdlog::info("[VehHands] vehcontrol: {} vehicles' controls read from fc5vr_vehcontrol.txt", g_vc.table.size());
    }
    void vc_save(const VcEntry& e)
    {
        vc_load();
        bool replaced = false;
        for (auto& t : g_vc.table)
            if (t.style == e.style && t.count == e.count && t.slot == e.slot && t.nverts == e.nverts) t = e, replaced = true;
        if (!replaced) g_vc.table.push_back(e);
        std::ofstream f{ fc5::mod_file("fc5vr_vehcontrol.txt"), std::ios::trunc };
        f << "# Far Cry 5 VR: each vehicle's own wheel, handlebars or stick, found in its model (vehcontrol; FarCry5VehHands.cpp). One line per vehicle:\n"
             "# control4 wheel|bars|stick <draw index count> <bone slot> <that draw's vertices on the slot> <centre x y z> <axis x y z> <radius or half width> <1: seen turning>\n"
             "# (the bone's bind pose, game metres; wheel: the rim's centre, the axis towards the driver; bars: their centre, the steering axis; stick: its top).\n"
             "# Delete a line to find that vehicle's control again the next time you sit in it.\n";
        for (const auto& t : g_vc.table)
            f << fmt::format("control4 {} {} {} {} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {}\n",
                             t.style == fc5::drive::kWheel ? "wheel" : t.style == fc5::drive::kBars ? "bars" : "stick", t.count, t.slot, t.nverts, t.c.x, t.c.y, t.c.z, t.n.x,
                             t.n.y, t.n.z, t.r, t.sure);
    }
    // The control looked for again (another style, or lost): the draws and their vertices are kept.
    void vc_reset_find(int frame)
    {
        g_vc.state       = 0;
        g_vc.cands.clear();
        g_vc.prev_cands.clear();
        g_vc.by_hands    = false;
        g_vc.anim_tries  = 0;
        g_vc.anim_ang    = 0.0f;
        g_vc.tries       = 0;
        g_vc.try_frame   = -1000;
        g_vc.tried_hands = false;
        g_vc.hold_tries  = 0;
        g_vc.hold_frame  = 0;
        g_vc.nudged      = false;
        g_vc.out_frame   = -1000;
        g_vc.adopt_frame = frame;
        g_vc.settled_frame = -1000;   // the draws seen again (two frames) before the next try
        g_vc.key         = VcEntry{};
        g_vc.key_draw    = -1;
        g_vc.turn_ok = g_vc.turn_any = false;
        g_vc.turn        = 0.0f;
        g_vc.said_follow = false;
        g_vc.piv_draw = g_vc.piv_slot = -2;
        g_vc.lean_draw = g_vc.lean_slot = -2;
        g_vc.ov_ok = g_vc.ov_said = false;
        g_vc.ov_counts.clear();
        for (auto& D : g_vc.draws) {
            D.pal_ref = D.pal_now;
            D.ov      = false;
            D.ov_slots.clear();
        }
    }
    // A new drive: everything.
    void vc_reset_drive(int frame)
    {
        for (auto& j : g_vc.jobs) {
            if (j.vb) j.vb->Release();
            if (j.ib) j.ib->Release();
        }
        g_vc.jobs.clear();
        g_vc.draws.clear();
        g_vc.cache.clear();
        g_vc.args.clear();
        g_vc.reads         = 0;
        g_vc.hinted        = false;
        g_vc.told          = false;
        g_vc.hand_frame[0] = g_vc.hand_frame[1] = -1000;
        g_vc.style         = 0;
        vc_reset_find(frame);
    }
    bool vc_rigid(const glm::mat4& m)
    {
        const float d = glm::determinant(glm::mat3{ m });
        return std::isfinite(d) && std::abs(d - 1.0f) < 0.05f;
    }
    glm::mat4 vc_slot(const std::vector<float>& p, int s) { return from_rows12(p.data() + size_t(s) * 12); }
    // The rigid move A as a turn: its angle (rad, 0..pi), axis and the point on that axis nearest the origin.
    bool vc_motion(const glm::mat4& A, float& ang, glm::vec3& ax, glm::vec3& piv)
    {
        glm::quat q = glm::quat_cast(glm::mat3{ A });
        if (q.w < 0.0f) q = -q;
        ang                = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
        const glm::vec3 xv = glm::vec3{ q.x, q.y, q.z };
        if (ang < 1e-3f || glm::length(xv) < 1e-6f) return false;
        ax                 = glm::normalize(xv);
        const glm::vec3 t  = glm::vec3{ A[3] };
        const glm::vec3 tp = t - ax * glm::dot(t, ax);
        piv                = 0.5f * (tp + glm::cross(ax, tp) / std::tan(0.5f * ang));   // t = (I - R) c for c across the axis
        return std::isfinite(piv.x + piv.y + piv.z);
    }
    // The draw's vertices (each once: position x y z and its 4th component, the bone), read back on an immediate context. Buffers read
    // once each while the draws are collected (a vehicle's draws share them).
    void vc_run_job(ID3D11DeviceContext* ctx, const VcJob& j, VcDraw& D)
    {
        D.verts.clear();
        D.has_verts = true;
        D.queued    = false;
        auto get = [&](ID3D11Buffer* b) -> const std::vector<uint8_t>* {
            auto it = g_vc.cache.find(b);
            if (it == g_vc.cache.end()) {
                std::vector<uint8_t> d;
                if (g_vc.cache.size() > 64 || !read_gpu(ctx, b, d)) return nullptr;
                it = g_vc.cache.emplace(b, std::move(d)).first;
            }
            return &it->second;
        };
        const std::vector<uint8_t>* vbd = get(j.vb);
        const std::vector<uint8_t>* ibd = get(j.ib);
        if (!vbd || !ibd) return;
        std::vector<bool> seen;
        int               top = 0;
        for (UINT k = 0; k < j.count; ++k) {
            const size_t io = size_t(j.ib_off) + size_t(j.start + k) * (j.i32 ? 4 : 2);
            if (io + (j.i32 ? 4 : 2) > ibd->size()) break;
            uint32_t idx = 0;
            std::memcpy(&idx, ibd->data() + io, j.i32 ? 4 : 2);
            if (idx >= 4u << 20) continue;
            if (idx >= seen.size()) seen.resize(size_t(idx) + 1, false);
            if (seen[idx]) continue;
            seen[idx]       = true;
            const int64_t v = int64_t(idx) + j.base;
            if (v < 0) continue;
            const size_t vs = size_t(j.voff) + size_t(v) * j.stride;
            const size_t vo = vs + j.pos_off;
            if (vs + j.stride > vbd->size() || vo + 8 > vbd->size()) continue;
            std::array<int16_t, 4> q{};
            std::memcpy(q.data(), vbd->data() + vo, 8);
            if (j.bi_off >= 0) {
                const uint8_t* bi = vbd->data() + vs + j.bi_off;
                int            hv = bi[0], hw = -1;
                if (j.w_off >= 0) {
                    const uint8_t* wp = vbd->data() + vs + j.w_off;
                    for (int c = 0; c < 4; ++c)
                        if (int(wp[c]) > hw) hw = wp[c], hv = bi[j.w_bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c];
                }
                q[3] = int16_t(hv);
            }
            if (q[3] < 0 || q[3] > 255) continue;
            top = std::max(top, int(q[3]));
            D.verts.push_back(q);
        }
        D.nslot = top + 1;
    }
}

void FarCry5Stereo::veh_control_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_vehcontrol.load() || !m_motion.load() || !m_in_vehicle.load() || !m_veh_driving.load() || m_chute_vehicle || m_cine_on || flat_screen_on()) return;
    if (!m_keep_bones.load()) m_keep_bones = true;   // the palettes' CPU copies (note_object_constants)
    const int       frame    = m_frame.load();
    const bool      deferred = fc5::rec::is_deferred(ctx);
    const bool      settled  = m_veh_settle.load() == 0;
    const glm::vec3 cam      = m_world_cam_pos;
    auto&           vc       = g_vc;
    std::lock_guard _{ vc.mtx };

    // The seat's frame: the drive's recentred reference, as veh_hands_place: ref = body_rot (world - (camera + draw shift)); game metres.
    const bool      seat_ok = m_mo.valid && frame - m_mo.frame <= 1;
    const glm::mat3 B       = m_mo.body_rot;
    const glm::vec3 o       = m_mo.cam + m_mo.draw_shift;
    const float     ws      = std::max(1e-3f, m_world_scale.load());
    glm::mat4       S4{ B };
    S4[3] = glm::vec4{ -(B * o), 1.0f };

    // The detection's frame (VcState::Sd), from the draws seen since frame f: the driven vehicle is the origin (VS b1's translation, within
    // 15 cm) holding the most vertices among them; its b1 gives the axes (x across, y to the front, z up: the same on the 11 vehicles of the
    // 11 Nov passes), the eyes the origin.
    auto det_frame = [&](int f) {
        int    best = -1;
        size_t bn   = 0;
        for (int di = 0; di < int(vc.draws.size()); ++di) {
            const VcDraw& D = vc.draws[size_t(di)];
            if (!D.has_verts || D.verts.empty() || D.frame < f || !D.seat) continue;
            size_t n = 0;
            for (const VcDraw& E : vc.draws)
                if (E.has_verts && E.frame >= f && glm::length(glm::vec3{ E.b1[3] } - glm::vec3{ D.b1[3] }) < 0.15f) n += E.verts.size();
            if (n > bn) bn = n, best = di;
        }
        vc.home = best;
        if (best < 0) return false;
        const VcDraw&   H   = vc.draws[size_t(best)];
        const glm::vec3 fwd = glm::normalize(glm::vec3{ H.b1[1] }), up = glm::normalize(glm::vec3{ H.b1[2] });
        glm::vec3       rt  = glm::cross(fwd, up);
        if (glm::length(rt) < 1e-4f) return false;
        rt = glm::normalize(rt) * (glm::determinant(glm::mat3{ H.b1 }) < 0.0f ? -1.0f : 1.0f);
        const glm::mat3 R = glm::transpose(glm::mat3{ rt, up, -fwd });   // rows: right, up, back
        vc.Sd             = glm::mat4{ R };
        vc.Sd[3]          = glm::vec4{ -(R * H.eye), 1.0f };
        return true;
    };
    // The vehicle's vertices around the driver, in the detection's frame, each with its part (a draw's slot), from the draws seen since frame f.
    using Parts = std::vector<std::pair<int, int>>;
    auto build_cloud = [&](int f, fc5::vcfit::Cloud& cl, Parts& parts) {
        cl = fc5::vcfit::Cloud{};
        parts.clear();
        if (!det_frame(f)) return;
        for (int di = 0; di < int(vc.draws.size()); ++di) {
            const VcDraw& D = vc.draws[size_t(di)];
            if (!D.has_verts || D.verts.empty() || D.frame < f || D.scale <= 0.0f || !D.seat) continue;
            const int              ns = std::min(D.nslot, int(D.pal_now.size() / 12));
            std::vector<glm::mat4> M(size_t(std::max(0, ns)));
            std::vector<int>       pid(size_t(std::max(0, ns)), -2);   // -2 not looked at, -1 no use, else the part
            for (const auto& q : D.verts) {
                const int s = q[3];
                if (s >= ns) continue;
                if (pid[size_t(s)] == -2) {
                    const glm::mat4 P = vc_slot(D.pal_now, s);
                    const float     d = glm::determinant(glm::mat3{ P });
                    pid[size_t(s)]    = std::isfinite(d) && std::abs(d) > 1e-6f ? -3 : -1;   // -3: usable, no vertex kept yet
                    M[size_t(s)]      = vc.Sd * D.b1 * P;
                }
                if (pid[size_t(s)] == -1) continue;
                const glm::vec3 p = glm::vec3{ M[size_t(s)] * glm::vec4{ float(q[0]) * D.scale, float(q[1]) * D.scale, float(q[2]) * D.scale, 1.0f } };
                if (!(p.x > -0.7f && p.x < 0.7f && p.y > -0.9f && p.y < 0.25f && p.z < 0.0f && p.z > -1.1f)) continue;
                if (pid[size_t(s)] == -3) {
                    if (parts.size() >= 65000) continue;
                    pid[size_t(s)] = int(parts.size());
                    parts.push_back({ di, s });
                }
                cl.p.push_back(p);
                cl.part.push_back(uint16_t(pid[size_t(s)]));
            }
        }
        cl.nparts = int(parts.size());
    };
    // The game's hands in the detection's frame (after build_cloud: its frame).
    auto hands_now = [&](fc5::vcfit::Hands& h) {
        for (int s = 0; s < 2; ++s) {
            h.ok[s] = frame - vc.hand_frame[s] <= 3;
            h.p[s]  = glm::vec3{ vc.Sd * glm::vec4{ vc.hand_w[s], 1.0f } };
        }
        return h.ok[0] || h.ok[1];
    };
    // The followed control now, in the detection's frame (after build_cloud): from its bone, or (no bone) from the vehicle's own space.
    auto key_now = [&](glm::vec3& c, glm::vec3& n) {
        glm::mat4 M{ 1.0f };
        if (vc.key.slot >= 0) {
            if (vc.key_draw < 0 || vc.key_draw >= int(vc.draws.size())) return false;
            const VcDraw& K = vc.draws[size_t(vc.key_draw)];
            if (int(K.pal_now.size()) < (vc.key.slot + 1) * 12) return false;
            M = vc.Sd * K.b1 * vc_slot(K.pal_now, vc.key.slot);
        } else {
            if (vc.home < 0) return false;
            M = vc.Sd * vc.draws[size_t(vc.home)].b1;
        }
        c = glm::vec3{ M * glm::vec4{ vc.key.c, 1.0f } };
        n = glm::normalize(glm::mat3{ M } * vc.key.n);
        return std::isfinite(c.x + c.y + c.z + n.x + n.y + n.z);
    };
    auto say_result = [&](const char* what, const fc5::vcfit::Result& r) {
        return fmt::format("{}: {}; sure {:.2f} (geometry {:.2f}, where it is {:.2f}, the game's hands {:.2f}, its turn {:.2f}); ({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes "
                           "(right, up, back, the vehicle's axes){}",
                           what, r.how, r.conf, r.geo, r.spatial, r.hands, r.anim, r.c.x / ws * 100.0f, r.c.y / ws * 100.0f, r.c.z / ws * 100.0f,
                           r.style == fc5::drive::kWheel  ? fmt::format(", rim radius {:.1f} cm, {} rim vertices, widest empty arc {:.0f} deg", r.r / ws * 100.0f, r.nin, r.gap)
                           : r.style == fc5::drive::kBars ? fmt::format(", half width {:.0f} cm", r.r / ws * 100.0f)
                                                          : std::string{});
    };
    // A shape in the seat's frame taken as the control, kept in the bind pose of the bone that holds it (part: a draw's slot; none: in the
    // seat's frame, where it was found).
    auto adopt = [&](const fc5::vcfit::Result& r, int di, int slot, const char* source, int sure) {
        VcEntry e;
        e.style = vc.style;
        e.r     = r.r;
        e.sure  = sure;
        e.c     = r.c;
        e.n     = r.n;
        if (di >= 0 && slot >= 0) {
            const VcDraw&   D = vc.draws[size_t(di)];
            const glm::mat4 X = glm::inverse(vc.Sd * D.b1 * vc_slot(D.pal_now, slot));
            e.count           = D.count;
            e.slot            = slot;
            for (const auto& q : D.verts) e.nverts += q[3] == slot;
            e.c = glm::vec3{ X * glm::vec4{ r.c, 1.0f } };
            e.n = glm::normalize(glm::mat3{ X } * r.n);
        } else if (vc.home >= 0) {
            // No bone under it (the hands' hold with no part): kept in the vehicle's own space.
            const glm::mat4 X = glm::inverse(vc.Sd * vc.draws[size_t(vc.home)].b1);
            e.c               = glm::vec3{ X * glm::vec4{ r.c, 1.0f } };
            e.n               = glm::normalize(glm::mat3{ X } * r.n);
        }
        vc.key         = e;
        vc.key_draw    = e.slot >= 0 ? di : -1;
        vc.state       = 2;
        vc.adopt_frame = frame;
        vc.source      = source;
        vc.conf        = r.conf;
        vc.by_hands    = (r.style == fc5::drive::kBars && r.hands >= 0.99f && r.how[0] == 'w') || (r.style == fc5::drive::kStick && r.hands >= 0.99f) || (sure && vc.by_hands);   // ("where the game's two hands hold")
        vc.turn_ok = vc.turn_any = false;
        vc.said_follow = false;
        vc.out_frame   = -1000;
        if (e.slot >= 0 && e.nverts > 0 && (sure || r.conf >= 0.75f)) vc_save(e);
        spdlog::info("[VehHands] vehcontrol: {} ({}){}", say_result(vc_style_name(vc.style), r), source,
                     e.slot >= 0 ? fmt::format("; its bone: draw {} slot {} ({} vertices){}", e.count, e.slot, e.nverts, sure || r.conf >= 0.75f ? ", saved for this model" : "")
                                 : std::string{ "; no bone under it: kept where it was found" });
    };

    // A wheel expected, but the game's two hands hold handlebars (level, 50 cm apart or more) and are not on the wheel found, if any: the
    // drive is told it is handlebars (a quad the game calls wheeled, its seat not learnt: the drive shows a wheel, and the quad's model
    // holds a ring that passes for one).
    auto bars_not_wheel = [&](const fc5::vcfit::Cloud& cl, const fc5::vcfit::Hands& h, const fc5::vcfit::Result& rw) {
        if (vc.style != fc5::drive::kWheel || vc.told || !h.ok[0] || !h.ok[1] || (rw.ok && rw.hands >= 0.6f)) return false;
        const auto rb = fc5::vcfit::detect(fc5::drive::kBars, cl, h);
        if (!rb.ok || rb.hands < 0.99f) return false;
        vc.told = true;
        fc5::drive::set_shape_hint(fc5::drive::kBars);
        spdlog::info("[VehHands] vehcontrol: a wheel expected, but {}: the drive told it is handlebars", say_result("handlebars", rb));
        return true;
    };

    // Once per frame.
    if (vc.cur_frame != frame) {
        const int        prev = vc.cur_frame;
        fc5::drive::View cv{};
        const int        style = fc5::drive::control(cv) ? cv.style : fc5::drive::kNone;
        vc.cur_frame           = frame;
        vc.drive_on            = fc5::drive::on();
        if ((frame & 1) == 0 || frame - vc.show_frame >= 2) {   // drivemodelshow: the same angle for both eyes
            vc.show_on    = fc5::drive::model_show(vc.show_turn);
            vc.show_frame = frame;
        }
        if (frame - vc.last_frame > 180) vc_reset_drive(frame);   // a new drive
        if (style != vc.style) {
            vc_reset_find(frame);
            vc.style = style;
            if (style != fc5::drive::kNone) vc_load();
        }
        vc.last_frame = frame;
        {
            int        drv = -1, vtype = -1;
            const bool in_anim = fc5::lua::vehicle_state(drv, vtype) == 1 && drv < 0;
            if (in_anim && !vc.entering) vc.enter_frame = frame;
            // (at most 900 frames: a seat the game never gives is not waited for)
            const bool entering = in_anim && frame - vc.enter_frame < 900;
            if (vc.entering && !entering && vc.state != 2 && (vc.tries > 0 || vc.anim_tries > 0 || vc.state == 3)) {
                spdlog::info("[VehHands] vehcontrol: the game's entry animation is over ({} frames): the {} looked for again from the seat ({} tries and {} turning-part "
                             "tries were made during it)", frame - vc.enter_frame, vc_style_name(vc.style), vc.tries, vc.anim_tries);
                vc_reset_find(frame);
            }
            vc.entering = entering;
        }
        if (settled && vc.settled_frame == -1000) vc.settled_frame = frame;
        bool pending = !vc.jobs.empty();
        for (const auto& D : vc.draws)
            if (D.frame >= prev - 1 && !D.has_verts) pending = true;
        if (!pending && !vc.cache.empty()) vc.cache.clear();
        const bool ready = vc.drive_on && settled && seat_ok && !vc.entering && vc.settled_frame != -1000 && frame - vc.settled_frame >= 2 &&
                           (!pending || frame - vc.settled_frame > 45);

        // No control known for this vehicle yet (the game has not said its type): a wheel or bars found sure enough is the drive's style.
        if (ready && vc.style == fc5::drive::kNone && !vc.hinted) {
            vc.hinted = true;
            fc5::vcfit::Cloud cl;
            Parts             parts;
            fc5::vcfit::Hands h;
            build_cloud(prev - 1, cl, parts);
            hands_now(h);
            const auto rw = fc5::vcfit::detect(fc5::drive::kWheel, cl, h);
            // (a wheel: its rim whole and clean; the quad's model holds a 22 cm ring too: geometry 0.86, sure 0.69 on the 11 Nov dump)
            const bool sw = rw.ok && rw.geo >= 0.95f && rw.conf >= 0.75f;
            const auto rb = sw ? fc5::vcfit::Result{} : fc5::vcfit::detect(fc5::drive::kBars, cl, h);
            const int  hs = sw ? fc5::drive::kWheel : rb.ok && rb.conf >= 0.7f ? fc5::drive::kBars : fc5::drive::kNone;
            if (hs != fc5::drive::kNone) fc5::drive::set_shape_hint(hs);
            spdlog::info("[VehHands] vehcontrol: no control chosen for this vehicle yet; its model shows {} ({} vertices near the driver in {} parts)",
                         hs == fc5::drive::kNone ? std::string{ "neither a wheel nor handlebars for sure" } : say_result(vc_style_name(hs), hs == fc5::drive::kWheel ? rw : rb),
                         cl.p.size(), cl.nparts);
        }

        // 2. At the sit (and again when the game's hands are first seen, and 120 frames on).
        if (ready && vc.style != fc5::drive::kNone && vc.state == 0) {
            fc5::vcfit::Hands h;
            const bool        hands = frame - vc.hand_frame[0] <= 3 || frame - vc.hand_frame[1] <= 3;
            if (vc.tries == 0 || (vc.tries < 3 && ((hands && !vc.tried_hands) || frame - vc.try_frame > 120))) {
                vc.tries++;
                vc.try_frame = frame;
                if (hands) vc.tried_hands = true;
                // 0. The table: a draw whose count and vertex count on the slot are a known model's.
                bool got = false;
                for (int di = 0; di < int(vc.draws.size()) && !got; ++di) {
                    const VcDraw& D = vc.draws[size_t(di)];
                    if (!D.has_verts || D.frame < prev - 1 || !D.seat) continue;
                    bool any = false;
                    for (const auto& t : vc.table)
                        if (t.style == vc.style && t.count == D.count) any = true;
                    if (!any) continue;
                    std::map<int, int> hist;
                    for (const auto& q : D.verts) hist[q[3]]++;
                    for (const auto& t : vc.table) {
                        if (t.style != vc.style || t.count != D.count || int(D.pal_now.size()) < (t.slot + 1) * 12 || hist[t.slot] != t.nverts) continue;
                        // Near the driver's seat (not a parked twin's).
                        const glm::vec3 cs = glm::vec3{ D.b1 * vc_slot(D.pal_now, t.slot) * glm::vec4{ t.c, 1.0f } } - D.eye;
                        if (glm::length(cs) > 1.2f) continue;
                        vc.key         = t;
                        vc.key_draw    = di;
                        vc.state       = 2;
                        vc.adopt_frame = frame;
                        vc.source      = "the table";
                        vc.conf        = 1.0f;
                        vc.turn_ok = vc.turn_any = false;
                        got = true;
                        spdlog::info("[VehHands] vehcontrol: this vehicle's {} from fc5vr_vehcontrol.txt (draw {} slot {}, {} vertices on it{})", vc_style_name(vc.style),
                                     t.count, t.slot, t.nverts, t.sure ? ", seen turning before" : "");
                        break;
                    }
                }
                if (!got) {
                    fc5::vcfit::Cloud cl;
                    Parts             parts;
                    build_cloud(prev - 1, cl, parts);
                    hands_now(h);
                    const auto r = fc5::vcfit::detect(vc.style, cl, h);
                    if (bars_not_wheel(cl, h, r)) {
                        // (the drive changes its control: looked for again as handlebars)
                    } else if (r.ok) {
                        adopt(r, r.part >= 0 ? parts[size_t(r.part)].first : -1, r.part >= 0 ? parts[size_t(r.part)].second : -1,
                              hands ? "at the sit, with the game's hands" : "at the sit", 0);
                    } else {
                        spdlog::info("[VehHands] vehcontrol: try {} at the sit, {} vertices near the driver in {} parts, the game's hands {}: not found ({})", vc.tries,
                                     cl.p.size(), cl.nparts, hands ? "seen" : "not seen", say_result(vc_style_name(vc.style), r));
                        // Handlebars expected (a driver on the vehicle's centre line is taken for a quad's) and none, but a wheel for sure:
                        // the drive is told (the tractor, 11 Nov: a 16.4 cm wheel, and "no handlebars" in three tries).
                        // Not before the game's hands are seen or the second try: on a quad they are on the bars, and its model holds a ring too.
                        if (vc.style == fc5::drive::kBars && (hands || vc.tries >= 2) && !vc.told) {
                            const auto rw = fc5::vcfit::detect(fc5::drive::kWheel, cl, h);
                            if (rw.ok && rw.geo >= 0.95f && rw.conf >= 0.75f) {
                                vc.told = true;
                                fc5::drive::set_shape_hint(fc5::drive::kWheel);
                                spdlog::info("[VehHands] vehcontrol: no handlebars but {}: the drive told it is a wheel", say_result("a wheel", rw));
                            }
                        }
                        // jetskibars: a wheel expected on a boat (the game's type 1) and none, but handlebars in its model: a jetski found
                        // in the world (only one spawned from F11 has its archetype, which gives the drive the bars at once). Author 16 Nov:
                        //; his 10:28 drive: "rim does not go all round" in three tries, the bars never
                        // looked for. The 12 Nov lake pass found the same jetski's bars at the sit at geometry 0.83, sure 0.70. The game's
                        // hands are not required: on the jetski its right hand is off the bars (12 Nov: the right palm 25-72 cm away).
                        if (vc.style == fc5::drive::kWheel && !vc.told) {
                            int        jdrv = -1, jtype = -1;
                            const bool boat = fc5::lua::vehicle_state(jdrv, jtype) == 1 && jtype == 1;
                            if (boat) {
                                const auto rb = fc5::vcfit::detect(fc5::drive::kBars, cl, h);
                                if (rb.ok && rb.geo >= 0.8f && rb.conf >= 0.65f) {
                                    vc.told = true;
                                    fc5::drive::set_shape_hint(fc5::drive::kBars);
                                    spdlog::info("[VehHands] vehcontrol: a boat with no wheel but {}: the drive told it is handlebars (a jetski)", say_result("handlebars", rb));
                                } else if (vc.tries == 1) {
                                    spdlog::info("[VehHands] vehcontrol: a boat with no wheel, and no handlebars for sure either ({})", say_result("handlebars", rb));
                                }
                            }
                        }
                        if (vc.tries >= 2 && !vc.nudged) {
                            // The animation instead: one quick small turn (driveautoturn), so the part that turns shows at once.
                            vc.nudged = true;
                            fc5::steertune::auto_turn(350);
                            spdlog::info("[VehHands] vehcontrol: a quick small turn sent, to see which part turns");
                        }
                    }
                }
            }
        }
        // The game's hands seen after a find without them: weighed again with them, once. Handlebars also once their turn was seen: the
        // hands' hold is the answer for them (the jetski, pass 5: a 42 cm part that turns was taken before the hands were seen; they hold
        // 72 cm apart): looked for with both hands seen, up to 3 times, 45 frames apart.
        const bool h_r = frame - vc.hand_frame[0] <= 3, h_l = frame - vc.hand_frame[1] <= 3;
        const bool again_any  = !vc.tried_hands && (h_r || h_l) && vc.key.sure == 0 && vc.source[0] == 'a';
        const bool again_bars = vc.style == fc5::drive::kBars && !vc.by_hands && h_r && h_l && vc.hold_tries < 3 && frame - vc.hold_frame > 45;
        // A stick found by its shape: its hold point is the game's own palm once the game's hand is on it, which can be a second or two
        // after the sit (the JacobSeed plane, 13 Nov: "the hands' palms score 0.25" 150 ms after the sit, on the stick in the dump taken
        // later; the shape's point there is the bare tube's end, 10 cm under the grip). Looked at up to 4 times, 60 frames apart.
        const bool again_stick = vc.style == fc5::drive::kStick && !vc.by_hands && (h_r || h_l) && vc.hold_tries < 4 && frame - vc.hold_frame > 60 &&
                                 !cv.held[0] && !cv.held[1];   // (not under a hand that holds it: the hand would jump)
        if (ready && vc.state == 2 && frame - vc.out_frame <= 1 && (again_any || again_bars || again_stick)) {
            if (again_any) vc.tried_hands = true;
            if (again_bars || again_stick) vc.hold_tries++, vc.hold_frame = frame;
            fc5::vcfit::Cloud cl;
            Parts             parts;
            fc5::vcfit::Hands h;
            build_cloud(prev - 1, cl, parts);
            hands_now(h);
            glm::vec3  kc{ 0.0f }, kn{ 0.0f, 0.0f, 1.0f };
            const bool have = key_now(kc, kn);
            const auto r    = fc5::vcfit::detect(vc.style, cl, h);
            const bool hold = (vc.style == fc5::drive::kBars && r.ok && r.hands >= 0.99f && r.how[0] == 'w') ||   // "where the game's two hands hold"
                              (vc.style == fc5::drive::kStick && r.ok && r.hands >= 0.99f);                      // the game's hand on the stick
            if (bars_not_wheel(cl, h, r)) {
                // (the drive changes its control: looked for again as handlebars)
            } else if (have && r.ok && (hold ? glm::length(r.c - kc) > 0.03f || std::abs(r.r - vc.key.r) > 0.03f : glm::length(r.c - kc) > 0.04f && r.conf > vc.conf && vc.key.sure == 0)) {
                spdlog::info("[VehHands] vehcontrol: the game's hands are {:.0f} cm from the {} found without them: taken again with them", glm::length(r.c - kc) / ws * 100.0f,
                             vc_style_name(vc.style));
                // Its bone: the part under the hands; none: the bone followed so far.
                const int sure = vc.key.sure == 1 ? 1 : 0;
                adopt(r, r.part >= 0 ? parts[size_t(r.part)].first : vc.key_draw, r.part >= 0 ? parts[size_t(r.part)].second : vc.key.slot, "at the sit, with the game's hands", sure);
            } else if (r.ok) {
                vc.conf = std::max(vc.conf, r.conf);
                if (hold) vc.by_hands = true;
                spdlog::info("[VehHands] vehcontrol: with the game's hands the same {} is found (sure {:.2f}); the hands' palms {} from it", vc_style_name(vc.style), r.conf,
                             vc.style == fc5::drive::kWheel ? fmt::format("{:.0f} cm (right) and {:.0f} cm (left; -100: not seen)", r.hand_d[0] / ws * 100.0f, r.hand_d[1] / ws * 100.0f)
                                                            : fmt::format("score {:.2f}", r.hands));
            }
            if (again_stick && !hold)
                spdlog::info("[VehHands] vehcontrol: look {} for the game's hand on the stick: the palms at ({:+.0f}, {:+.0f}, {:+.0f}) cm (right{}) and ({:+.0f}, {:+.0f}, "
                             "{:+.0f}) cm (left{}) from the eyes (right, up, back, the vehicle's axes): not on it ({})",
                             vc.hold_tries, h.p[0].x / ws * 100.0f, h.p[0].y / ws * 100.0f, h.p[0].z / ws * 100.0f, h.ok[0] ? "" : ", not seen", h.p[1].x / ws * 100.0f,
                             h.p[1].y / ws * 100.0f, h.p[1].z / ws * 100.0f, h.ok[1] ? "" : ", not seen", r.how);
            // Handlebars and no two-hand hold: where the game's hands are, so a pass's log tells why.
            if (again_bars && !hold)
                spdlog::info("[VehHands] vehcontrol: look {} for the handlebars' hold: the game's palms at ({:+.0f}, {:+.0f}, {:+.0f}) cm (right) and ({:+.0f}, {:+.0f}, {:+.0f}) cm "
                             "(left) from the eyes (right, up, back, the vehicle's axes), {:.0f} cm apart: not a hold on handlebars ({}); the handlebars followed {}",
                             vc.hold_tries, h.p[0].x / ws * 100.0f, h.p[0].y / ws * 100.0f, h.p[0].z / ws * 100.0f, h.p[1].x / ws * 100.0f, h.p[1].y / ws * 100.0f,
                             h.p[1].z / ws * 100.0f, glm::length(h.p[0] - h.p[1]) / ws * 100.0f, r.how,
                             have ? fmt::format("at ({:+.0f}, {:+.0f}, {:+.0f}) cm, half width {:.0f} cm", kc.x / ws * 100.0f, kc.y / ws * 100.0f, kc.z / ws * 100.0f, vc.key.r / ws * 100.0f)
                                  : std::string{ "not placed" });
        }

        // 3. The animation: the parts that turned in the frame just ended, each seen turning alike the frame before (a rotor never turns twice
        // alike), the largest turn first. Turns are taken in the vehicle's own space (its draws share one transform, VS b1, and its body's bones
        // stay put there), not against a draw's slot 0: slot 0 is the first bone the draw's mesh uses, and in the Compact's wheel draw that is
        // the wheel itself (11 Nov dumps: its rim's 882 vertices on slot 0, the column's on slots 1-3).
        if (seat_ok && !vc.cands.empty() && vc.cand_frame == prev && vc.style != fc5::drive::kNone &&
            (vc.state == 0 || (vc.state == 2 && (vc.key.sure == 0 || (vc.key.sure == 1 && !vc.ov_ok && vc.style != fc5::drive::kStick))))) {
            std::vector<VcState::Best> stable;
            for (const auto& c : vc.cands)
                for (const auto& q : vc.prev_cands)
                    if (q.d == c.d && q.slot == c.slot && q.frame >= prev - 4 && std::abs(q.ang - c.ang) < glm::radians(8.0f) && glm::dot(q.ax_w, c.ax_w) > 0.98f) {
                        stable.push_back(c);
                        break;
                    }
            vc.prev_cands = vc.cands;
            vc.cands.clear();
            std::sort(stable.begin(), stable.end(), [](const VcState::Best& x, const VcState::Best& y) { return x.ang > y.ang; });
            if (!stable.empty() && (vc.anim_tries == 0 || stable[0].ang > vc.anim_ang + glm::radians(10.0f))) {
                vc.anim_tries++;
                vc.anim_ang = stable[0].ang;
                fc5::vcfit::Cloud cl;
                Parts             parts;
                fc5::vcfit::Hands h;
                build_cloud(prev, cl, parts);
                hands_now(h);
                glm::vec3  kc{ 0.0f }, kn{ 0.0f, 0.0f, 1.0f };
                const bool have_key = vc.state == 2 && key_now(kc, kn);
                std::vector<int> pn(parts.size(), 0);
                for (uint16_t k : cl.part) pn[k]++;
                auto part_draw = [&](int k) { return k >= 0 ? parts[size_t(k)].first : -1; };
                auto part_slot = [&](int k) { return k >= 0 ? parts[size_t(k)].second : -1; };
                bool                       done = false;
                std::string                failed;
                std::vector<VcState::Best> weighed;
                for (const auto& b : stable) {
                    if (done || weighed.size() >= 4 || b.d >= int(vc.draws.size())) break;
                    bool dup = false;   // turning with a part already weighed
                    for (const auto& w : weighed)
                        if (std::abs(w.ang - b.ang) < glm::radians(4.0f) && glm::dot(w.ax_w, b.ax_w) > std::cos(glm::radians(5.0f))) dup = true;
                    if (dup) continue;
                    weighed.push_back(b);
                    // Every slot of the vehicle's draws seen that frame turning about the same axis by the same angle: the same bone.
                    std::set<std::pair<int, int>> moving;
                    std::map<int, std::tuple<glm::vec3, glm::vec3, glm::vec3>> axis_of;   // per draw: the turn's axis and a point on it (the vehicle's space), the axis in the world
                    for (int di = 0; di < int(vc.draws.size()); ++di) {
                        const VcDraw& E = vc.draws[size_t(di)];
                        if (E.frame != prev || E.pal_ref.size() < 12 || E.pal_now.size() < 12) continue;
                        const glm::mat3 Wb{ E.b1 };
                        const int       ns = int(std::min(E.pal_now.size(), E.pal_ref.size()) / 12);
                        for (int s = 0; s < ns; ++s) {
                            const glm::mat4 Psn = vc_slot(E.pal_now, s), Psr = vc_slot(E.pal_ref, s);
                            if (!vc_rigid(Psn) || !vc_rigid(Psr)) continue;
                            float     ang = 0.0f;
                            glm::vec3 ax{ 0.0f }, piv{ 0.0f };
                            if (!vc_motion(Psn * glm::inverse(Psr), ang, ax, piv)) continue;
                            if (std::abs(ang - b.ang) > glm::radians(4.0f) || glm::dot(glm::normalize(Wb * ax), b.ax_w) < std::cos(glm::radians(5.0f))) continue;
                            moving.insert({ di, s });
                            if (!axis_of.contains(di)) axis_of[di] = { ax, piv, glm::normalize(Wb * ax) };
                        }
                    }
                    // drivemodelshow: the bone's slots and each draw's axis (a positive turn about it: the bone's own, as about n, seat frame),
                    // to draw the control at the hand's angle (veh_control_show).
                    auto capture_show = [&](const glm::vec3& n_seat) {
                        if (vc.style == fc5::drive::kStick) return;
                        const glm::vec3 n_w = glm::transpose(glm::mat3{ vc.Sd }) * n_seat;
                        vc.ov_counts.clear();
                        for (auto& E : vc.draws) {
                            E.ov = false;
                            E.ov_slots.clear();
                        }
                        for (const auto& [mi, ms] : moving) {
                            VcDraw&    E  = vc.draws[size_t(mi)];
                            const auto it = axis_of.find(mi);
                            if (it == axis_of.end()) continue;
                            if (!E.ov) {
                                E.ov     = true;
                                E.ov_ax  = glm::dot(std::get<2>(it->second), n_w) < 0.0f ? -std::get<0>(it->second) : std::get<0>(it->second);
                                E.ov_piv = std::get<1>(it->second);
                                vc.ov_counts.push_back(E.count);
                            }
                            E.ov_slots.push_back({ ms, vc_slot(E.pal_now, ms) });
                        }
                        vc.ov_turn_c = vc.turn;
                        vc.ov_ok     = !vc.ov_counts.empty();
                        spdlog::info("[VehHands] vehcontrol: the {}'s bone is {} slots of {} draws: drawn at the hand's angle while held (drivemodelshow)", vc_style_name(vc.style),
                                     moving.size(), vc.ov_counts.size());
                    };
                    fc5::vcfit::Motion mo;
                    mo.axis  = glm::normalize(glm::mat3{ vc.Sd } * b.ax_w);
                    mo.point = glm::vec3{ vc.Sd * glm::vec4{ b.piv_w, 1.0f } };
                    mo.angle = b.ang;
                    mo.parts.assign(parts.size(), 0);
                    int big = -1;   // the turning part with the most vertices near the driver
                    for (int k = 0; k < int(parts.size()); ++k)
                        if (moving.contains(parts[size_t(k)])) {
                            mo.parts[size_t(k)] = 1;
                            if (big < 0 || pn[size_t(k)] > pn[size_t(big)]) big = k;
                        }
                    const auto        r2     = fc5::vcfit::detect(vc.style, cl, h, &mo);
                    const std::string turned = fmt::format("draw {} slot {} turned {:.0f} deg, {} slots with it", vc.draws[size_t(b.d)].count, b.slot, glm::degrees(b.ang), moving.size());
                    // A find with no part of its own (the hands' hold, the hub-and-hands wheel): the turning part with the most vertices near
                    // the driver, else the part that turned.
                    auto own_draw = [&](const fc5::vcfit::Result& r) { return r.part >= 0 ? part_draw(r.part) : big >= 0 ? part_draw(big) : b.d; };
                    auto own_slot = [&](const fc5::vcfit::Result& r) { return r.part >= 0 ? part_slot(r.part) : big >= 0 ? part_slot(big) : b.slot; };
                    if (vc.state == 2) {
                        done = true;   // (while a control is followed the search only keeps what turns about it: the first is the one)
                        if (frame - vc.out_frame > 2 || !have_key) break;
                        // Found at the sit: does its turn agree?
                        fc5::vcfit::Result cur;
                        cur.style = vc.style;
                        cur.c     = kc;
                        cur.n     = kn;
                        cur.r     = vc.key.r;
                        cur.conf  = 1.0f;
                        cur.anim  = 1.0f;
                        cur.how   = "found at the sit, its turn agrees";
                        const glm::vec3 dc     = kc - mo.point;
                        const float     off    = glm::length(dc - mo.axis * glm::dot(dc, mo.axis));
                        const bool      key_in = vc.key.slot >= 0 && moving.contains({ vc.key_draw, vc.key.slot });
                        const bool      coax   = vc.style == fc5::drive::kWheel ? std::abs(glm::dot(kn, mo.axis)) > 0.97f && off < 0.04f
                                                 : vc.style == fc5::drive::kBars ? key_in || (vc.by_hands && off < 0.20f)
                                                                                 : key_in;
                        const bool      agree  = r2.ok && glm::length(r2.c - kc) < (vc.style == fc5::drive::kWheel ? 0.03f : 0.08f);
                        if ((agree || coax) && vc.key.sure == 1) {
                            capture_show(kn);   // known from the table: only its slots were still to see
                        } else if (agree || coax) {
                            if (vc.style == fc5::drive::kBars) cur.n = mo.axis.y < 0.0f ? -mo.axis : mo.axis;   // the steering axis, now known
                            // Its bone: the one found at the sit if it turns; else the turning part that holds the shape, or the most vertices,
                            // or the part that turned.
                            const int cand = agree && r2.part >= 0 ? r2.part : big;
                            const int kd   = key_in ? vc.key_draw : cand >= 0 ? part_draw(cand) : b.d;
                            const int ks   = key_in ? vc.key.slot : cand >= 0 ? part_slot(cand) : b.slot;
                            spdlog::info("[VehHands] vehcontrol: the {} found at the sit turns with the steering ({}; its centre {:.1f} cm from the turn's axis){}",
                                         vc_style_name(vc.style), turned, off / ws * 100.0f, key_in ? "" : ": it was on no bone that turns, the one that turns is followed");
                            adopt(cur, kd, ks, "at the sit, seen turning", 1);
                            capture_show(cur.n);
                        } else if (vc.key.sure == 1) {
                            // (known from the table and another part turned: nothing to do)
                        } else if (r2.ok) {
                            spdlog::info("[VehHands] vehcontrol: the {} found at the sit ({:.0f} cm from the part that turns) is not what turns ({})", vc_style_name(vc.style),
                                         glm::length(r2.c - kc) / ws * 100.0f, turned);
                            adopt(r2, own_draw(r2), own_slot(r2), "from its turn", 1);
                            capture_show(r2.n);
                        } else if (vc.anim_tries <= 3) {
                            spdlog::info("[VehHands] vehcontrol: a part turned ({}), not the {} found at the sit ({:.0f} cm from its axis) and no {} in it: kept", turned,
                                         vc_style_name(vc.style), off / ws * 100.0f, vc_style_name(vc.style));
                            if (vc.anim_tries == 3) vc.key.sure = -1;   // stop looking: kept as found
                        }
                    } else if (r2.ok) {
                        adopt(r2, own_draw(r2), own_slot(r2), "from its turn", 1);
                        capture_show(r2.n);
                        done = true;
                    } else if (failed.empty()) {
                        failed = fmt::format("{}: {}", turned, say_result("no shape in it", r2));
                    }
                }
                if (!done && vc.state == 0) {
                    spdlog::info("[VehHands] vehcontrol: {} parts turned and none is the {} (the first: {})", weighed.size(), vc_style_name(vc.style), failed);
                    if (vc.anim_tries >= 4) {
                        vc.state = 3;
                        spdlog::info("[VehHands] vehcontrol: the {} not found in this vehicle's model: this drive as before (the game's hands)", vc_style_name(vc.style));
                    }
                }
            }
        }
        // A control with no bone: where it was found, in the vehicle's own space (the driven vehicle's draw, VS b1).
        if (vc.state == 2 && vc.key.slot < 0 && seat_ok && vc.home >= 0 && vc.home < int(vc.draws.size()) && vc.draws[size_t(vc.home)].frame >= prev - 1) {
            const glm::mat4& Hb = vc.draws[size_t(vc.home)].b1;
            vc.c_ref     = B * (glm::vec3{ Hb * glm::vec4{ vc.key.c, 1.0f } } - o);
            vc.n_ref     = B * glm::normalize(glm::mat3{ Hb } * vc.key.n);
            vc.G_ref     = glm::translate(glm::mat4{ 1.0f }, vc.c_ref);
            vc.out_frame = frame;
            fc5::drive::Model m{};
            m.style       = vc.style;
            m.centre      = vc.c_ref / ws;
            m.n           = vc.n_ref;
            m.radius      = vc.key.r / ws;
            m.axis_learnt = true;
            m.bone        = true;
            fc5::drive::set_model(m);
        }
        // Lost: the followed bone not drawn near the eyes for 4 s: looked for again (a table entry that was another vehicle's: not taken again).
        if (vc.state == 2 && vc.key.slot >= 0 && frame - std::max(vc.out_frame, vc.adopt_frame) > 360) {
            spdlog::info("[VehHands] vehcontrol: the {} (draw {} slot {}, {}) not seen near the eyes for 4 s: looked for again", vc_style_name(vc.style), vc.key.count,
                         vc.key.slot, vc.source);
            if (vc.source[0] == 't') std::erase_if(vc.table, [&](const VcEntry& t) { return t.style == vc.key.style && t.count == vc.key.count && t.slot == vc.key.slot && t.nverts == vc.key.nverts; });
            vc_reset_find(frame);
        }
    }

    if (!vc.drive_on) return;   // gesture driving off: no control to place
    // The vertex reads left by draws on a deferred context.
    if (!deferred && !vc.jobs.empty()) {
        for (auto& j : vc.jobs) {
            if (j.d >= 0 && j.d < int(vc.draws.size())) vc_run_job(ctx, j, vc.draws[size_t(j.d)]);
            if (j.vb) j.vb->Release();
            if (j.ib) j.ib->Release();
        }
        vc.jobs.clear();
    }

    // This draw: near the eyes, a vehicle material, with a palette copy.
    const bool searching = settled && seat_ok && vc.style != fc5::drive::kNone &&
                           (vc.state == 0 || (vc.state == 2 && (vc.key.sure == 0 || (vc.key.sure == 1 && !vc.ov_ok && vc.style != fc5::drive::kStick))));
    float      b1f[12]{};
    {
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(1, 1, &cb);
        if (!cb) return;
        std::lock_guard ol{ m_obj_mtx };
        const auto      it = m_obj_consts.find(cb);
        cb->Release();
        if (it == m_obj_consts.end()) return;
        std::memcpy(b1f, it->second.f, sizeof(b1f));
    }
    if (glm::length(glm::vec3{ b1f[3], b1f[7], b1f[11] } - cam) > 3.5f) return;
    {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (!ps) return;
        bool veh = false;
        {
            std::lock_guard p{ m_ps_mtx };
            veh = m_vehicle_shaders.contains(ps);
        }
        ps->Release();
        if (!veh) return;
    }
    // An indirect draw (no count here): its index count, start and base from its arguments, read once a drive for each (buffer, offset).
    // The van's rim, the heavy truck's and the boats' parts are drawn so (11 Nov: the van's four vehicle draws with no count held what
    // the detection missed).
    if (count == 0) {
        if (m_hud_draw_kind != 5 || !m_hud_draw_args) return;
        const auto ak = std::make_pair(static_cast<void*>(m_hud_draw_args), m_hud_draw_args_off);
        auto       it = vc.args.find(ak);
        if (it == vc.args.end()) {
            std::array<int32_t, 5> ia{};
            if (deferred || vc.args.size() >= 256 || !indirect_args(ctx, m_hud_draw_args, m_hud_draw_args_off, ia, true)) return;
            it = vc.args.emplace(ak, ia).first;
        }
        if (it->second[0] <= 0) return;
        count = UINT(it->second[0]);
        start = UINT(std::max(0, it->second[2]));
        base  = INT(it->second[3]);
    }
    ID3D11Buffer* vb     = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    if (vb) vb->Release();   // a key only here (a read takes its own reference)
    int di = -1;
    for (int i = 0; i < int(vc.draws.size()); ++i) {
        const auto& D = vc.draws[size_t(i)];
        if (D.count == count && D.start == start && D.base == base && D.vb == vb) {
            di = i;
            break;
        }
    }
    const bool key_draw = vc.state == 2 && vc.key.slot >= 0 && count == vc.key.count;
    if (di >= 0) {
        const VcDraw& D = vc.draws[size_t(di)];
        // Known and read, and nothing to look for now: only the followed bone's draw goes on.
        if (!key_draw && D.has_verts && !searching && vc.state == 2) return;
        if (!key_draw && D.frame == frame && (D.has_verts || D.queued)) return;   // seen this frame
    }
    std::vector<float> pal;
    float              scale = 0.0f;
    {
        ID3D11Buffer* cb2 = nullptr;
        ID3D11Buffer* cb3 = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cb2);
        ctx->VSGetConstantBuffers(3, 1, &cb3);
        std::lock_guard ol{ m_obj_mtx };
        if (cb2) {
            if (auto it = m_obj_consts.find(cb2); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
            cb2->Release();
        }
        if (cb3) {
            if (auto it = m_bone_data.find(cb3); it != m_bone_data.end() && it->second.size() >= 24) {
                // The slots its vertices use only (once read): a palette is 256 slots, a vehicle's draw uses a few dozen.
                const size_t n = di >= 0 && vc.draws[size_t(di)].has_verts ? std::min(it->second.size(), size_t(std::max(2, vc.draws[size_t(di)].nslot)) * 12) : it->second.size();
                pal.assign(it->second.begin(), it->second.begin() + n);
            }
            cb3->Release();
        }
    }
    if (pal.empty()) return;

    // 4. Following: the bone's palette matrix places the control. Every drawn instance of the key's draw is weighed: the nearest within 1.2 m
    // of the eyes (not a parked twin).
    if (key_draw && seat_ok && int(pal.size()) >= (vc.key.slot + 1) * 12) {
        const glm::mat4 Ps = vc_slot(pal, vc.key.slot);
        const glm::mat4 W  = from_rows12(b1f) * Ps;
        const glm::vec3 cw = glm::vec3{ W * glm::vec4{ vc.key.c, 1.0f } };
        const float     dc = glm::length(cw - cam);
        const glm::mat3 R{ W };
        const glm::vec3 gx = glm::normalize(R[0]);
        glm::vec3       gy = R[1] - gx * glm::dot(R[1], gx);
        if (dc <= 1.2f && !(vc.out_frame == frame && dc >= vc.out_d) && glm::length(gy) > 1e-6f && std::isfinite(cw.x + gx.x + gy.x)) {
            gy                 = glm::normalize(gy);
            const glm::vec3 nw = glm::normalize(R * vc.key.n);
            vc.G_ref           = glm::mat4{ B * glm::mat3{ gx, gy, glm::cross(gx, gy) } };
            vc.c_ref           = B * (cw - o);
            vc.G_ref[3]        = glm::vec4{ vc.c_ref, 1.0f };
            vc.n_ref           = B * nw;
            // The bone's turn about its own axis, in the vehicle's space: summed frame to frame (rad, positive: to the left).
            if (vc.style != fc5::drive::kStick && vc_rigid(Ps)) {
                const glm::quat q  = glm::quat_cast(glm::mat3{ Ps });
                const float     tw = 2.0f * std::atan2(glm::dot(glm::vec3{ q.x, q.y, q.z }, vc.key.n), q.w);
                if (vc.turn_any && vc.out_frame != frame) {
                    float d = tw - vc.turn_raw;
                    while (d > 3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    vc.turn += d;
                }
                vc.turn_raw = tw;
                vc.turn_any = true;
                vc.turn_ok  = true;
            } else if (vc.out_frame != frame) {
                vc.turn_ok = false;
            }
            vc.out_frame = frame;
            vc.out_d     = dc;
            vc.key_b1    = glm::vec3{ b1f[3], b1f[7], b1f[11] };
            fc5::drive::Model m{};
            m.style       = vc.style;
            m.centre      = vc.c_ref / ws;
            m.n           = vc.n_ref;
            m.radius      = vc.key.r / ws;
            m.axis_learnt = true;
            m.bone        = true;
            m.turn        = vc.turn;
            m.turn_ok     = vc.turn_ok;
            fc5::drive::set_model(m);
            // Whether the bone itself moves in the vehicle's own space apart from its turn (a body that rolls on its springs under the
            // vehicle's transform): drivemodelshow draws the bone at the pose it was taken in, turned; a line when its centre is 2 cm more
            // from where it was first followed than said so far.
            {
                const glm::vec3 pv = glm::vec3{ Ps * glm::vec4{ vc.key.c, 1.0f } };
                if (vc.piv_draw != vc.key_draw || vc.piv_slot != vc.key.slot) {
                    vc.piv_draw = vc.key_draw;
                    vc.piv_slot = vc.key.slot;
                    vc.piv0     = pv;
                    vc.piv_said = 0.0f;
                }
                const float dv = glm::length(pv - vc.piv0);
                if (vc.style != fc5::drive::kStick && dv > vc.piv_said + 0.02f * ws) {
                    vc.piv_said = dv;
                    spdlog::info("[VehHands] vehcontrol: the {}'s bone is {:.1f} cm from where it was first followed, in the vehicle's own space (not its turn: the "
                                 "vehicle's body moves under its transform)", vc_style_name(vc.style), dv / ws * 100.0f);
                }
            }
            // A stick: how far the game leans its bone from where it was first followed (each change of 1 deg, at most every 10 frames, 150
            // lines a drive): with a timed left stick (steerstep) it shows where the game's dead zone ends.
            if (vc.style == fc5::drive::kStick && vc_rigid(Ps)) {
                if (vc.lean_draw != vc.key_draw || vc.lean_slot != vc.key.slot) {
                    vc.lean_draw  = vc.key_draw;
                    vc.lean_slot  = vc.key.slot;
                    vc.lean_R0    = glm::mat3{ Ps };
                    vc.lean_said  = 0.0f;
                    vc.lean_lines = 0;
                    vc.lean_frame = frame;
                }
                const glm::quat q    = glm::quat_cast(glm::mat3{ Ps } * glm::transpose(vc.lean_R0));
                const float     lean = glm::degrees(2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)));
                if (std::abs(lean - vc.lean_said) >= 1.0f && frame - vc.lean_frame >= 10 && vc.lean_lines < 150) {
                    vc.lean_said  = lean;
                    vc.lean_frame = frame;
                    vc.lean_lines++;
                    spdlog::info("[VehHands] vehcontrol: the stick's bone leans {:.1f} deg from where it was first followed", lean);
                }
                // While a steerstep runs, and for 3 s after it: every 9 frames the stick's lean to 0.01 deg, about the vehicle's front and
                // across axes, and the vehicle's own roll and pitch. On the ground the stick's bone leans about 1 deg for 0.60 of left
                // stick and sways 1 deg by itself (13 Nov, helidead2b), so 1 deg steps cannot tell where the game starts to answer; the
                // mean of these lines over a step can, and in a hover the vehicle's roll shows it directly.
                {
                    float      sx  = 0.0f;
                    int        srt = 0;
                    const bool on  = fc5::steertune::step_value(sx, srt);
                    if (on) vc.fine_until = frame + 270;
                    if (frame < vc.fine_until && frame - vc.fine_frame >= 9) {
                        vc.fine_frame = frame;
                        glm::vec3 qv{ q.x, q.y, q.z };
                        if (q.w < 0.0f) qv = -qv;
                        const float     ql = glm::length(qv);
                        const glm::vec3 rv = ql > 1e-9f ? qv / ql * glm::degrees(2.0f * std::asin(std::min(1.0f, ql))) : glm::vec3{ 0.0f };
                        const glm::mat4 Wv = from_rows12(b1f);
                        const glm::vec3 fw = glm::normalize(glm::vec3{ Wv[1] }), up = glm::normalize(glm::vec3{ Wv[2] });
                        const glm::vec3 ac = glm::cross(fw, glm::vec3{ 0.0f, 0.0f, 1.0f });   // level, across the vehicle (the world's up is z)
                        const float     roll  = glm::length(ac) > 1e-4f ? glm::degrees(std::asin(std::clamp(glm::dot(up, glm::normalize(ac)), -1.0f, 1.0f))) : 0.0f;
                        const float     pitch = glm::degrees(std::asin(std::clamp(fw.z, -1.0f, 1.0f)));
                        spdlog::info("[VehHands] vehcontrol: steerstep {}: left stick X {:+.2f}, RT {}; the stick's bone leans {:+.2f} deg about the vehicle's front axis, "
                                     "{:+.2f} about its across axis; the vehicle rolls {:+.2f} deg, pitches {:+.2f} deg",
                                     on ? "on" : "over", on ? sx : 0.0f, on ? srt : 0, rv.y, rv.x, roll, pitch);
                    }
                }
            }
            // drivemodelshow. Until the bone's turn has shown all its slots: a wheel's own slot, about the rim's axis.
            if (!vc.ov_ok && vc.ov_counts.empty() && vc.style == fc5::drive::kWheel && di >= 0 && vc.turn_ok) {
                VcDraw& E  = vc.draws[size_t(di)];
                E.ov       = true;
                E.ov_slots = { { vc.key.slot, Ps } };
                E.ov_ax    = glm::normalize(glm::mat3{ Ps } * vc.key.n);
                E.ov_piv   = glm::vec3{ Ps * glm::vec4{ vc.key.c, 1.0f } };
                vc.ov_turn_c        = vc.turn;
                vc.ov_counts        = { count };
            }
            // The hand rides the bone as it is drawn: at the hand's angle, not the game's.
            if (vc.show_on && vc.turn_ok && di >= 0 && vc.draws[size_t(di)].ov) {
                const VcDraw&   E  = vc.draws[size_t(di)];
                const glm::mat4 Wb = S4 * from_rows12(b1f);
                const glm::mat4 Rb = glm::translate(glm::mat4{ 1.0f }, E.ov_piv) * glm::mat4_cast(glm::angleAxis(vc.show_turn - vc.turn, E.ov_ax)) *
                                     glm::translate(glm::mat4{ 1.0f }, -E.ov_piv);
                vc.G_ref           = Wb * Rb * glm::inverse(Wb) * vc.G_ref;
            }
            if (!vc.said_follow) {
                vc.said_follow = true;
                spdlog::info("[VehHands] vehcontrol: following the vehicle's own {} ({}): {} ({:+.0f}, {:+.0f}, {:+.0f}) cm from the eyes (right, up, back, the recentred view's axes){}",
                             vc_style_name(vc.style), vc.source, vc.style == fc5::drive::kStick ? "its top" : "its centre", vc.c_ref.x / ws * 100.0f,
                             vc.c_ref.y / ws * 100.0f, vc.c_ref.z / ws * 100.0f,
                             vc.style == fc5::drive::kWheel  ? fmt::format(", rim radius {:.1f} cm, its face tilted {:.0f} deg up", vc.key.r / ws * 100.0f,
                                                                           glm::degrees(std::asin(std::clamp(vc.n_ref.y, -1.0f, 1.0f))))
                             : vc.style == fc5::drive::kBars ? fmt::format(", half width {:.0f} cm", vc.key.r / ws * 100.0f)
                                                             : std::string{});
            }
        }
    }
    if (di >= 0 && vc.draws[size_t(di)].frame == frame && (vc.draws[size_t(di)].has_verts || vc.draws[size_t(di)].queued)) return;   // seen this frame
    if (di < 0) {
        if (vc.draws.size() >= 128) return;
        VcDraw D;
        D.count   = count;
        D.start   = start;
        D.base    = base;
        D.vb      = vb;
        D.pal_ref = pal;
        vc.draws.push_back(std::move(D));
        di = int(vc.draws.size()) - 1;
    }
    VcDraw& D = vc.draws[size_t(di)];
    if (D.frame != frame) {
        D.pal_now = std::move(pal);
        D.b1      = from_rows12(b1f);
        D.eye     = o;
        D.seat    = seat_ok;
        D.scale   = scale;
        D.frame   = frame;
        if (D.pal_ref.size() > D.pal_now.size()) D.pal_ref.resize(D.pal_now.size());
    }

    // 1. Its vertices, once (6 reads a frame, 200 a drive): now on an immediate context, else at the next immediate draw.
    if (!D.has_verts && !D.queued && vc.reads < 200) {
        if (vc.read_frame != frame) vc.read_frame = frame, vc.reads_in_frame = 0;
        if (vc.reads_in_frame < 6) {
            ID3D11InputLayout* il = nullptr;
            ctx->IAGetInputLayout(&il);
            std::vector<LayoutElem> layout;
            {
                std::lock_guard p{ m_ps_mtx };
                if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
            }
            if (il) il->Release();
            int  pos_off = -1, bi_off = -1, w_off = -1;
            bool w_bgra  = false;
            for (const auto& e : layout) {
                if (e.slot != 0 || e.index != 0) continue;
                if (_stricmp(e.semantic.c_str(), "POSITION") == 0 && e.format == DXGI_FORMAT_R16G16B16A16_SINT) pos_off = int(e.offset);
                if (_stricmp(e.semantic.c_str(), "BLENDINDICES") == 0) bi_off = int(e.offset);
                if (_stricmp(e.semantic.c_str(), "BLENDWEIGHT") == 0) w_off = int(e.offset), w_bgra = e.format == DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            if (pos_off < 0 || !vb || stride == 0 || stride > 256 || bi_off + 4 > int(stride) || w_off + 4 > int(stride)) {
                D.has_verts = true;   // not the vehicles' packed position: no vertices to take
                D.verts.clear();
            } else {
                VcJob j;
                j.d = di;
                ctx->IAGetVertexBuffers(0, 1, &j.vb, &j.stride, &j.voff);
                DXGI_FORMAT ib_fmt{};
                ctx->IAGetIndexBuffer(&j.ib, &ib_fmt, &j.ib_off);
                j.i32     = ib_fmt == DXGI_FORMAT_R32_UINT;
                j.count   = count;
                j.start   = start;
                j.base    = base;
                j.pos_off = UINT(pos_off);
                j.bi_off  = bi_off;
                j.w_off   = w_off;
                j.w_bgra  = w_bgra;
                vc.reads++;
                vc.reads_in_frame++;
                if (!j.vb || !j.ib) {
                    if (j.vb) j.vb->Release();
                    if (j.ib) j.ib->Release();
                    D.has_verts = true;
                } else if (deferred) {
                    D.queued = true;
                    vc.jobs.push_back(j);
                } else {
                    vc_run_job(ctx, j, D);
                    j.vb->Release();
                    j.ib->Release();
                }
            }
        }
    }

    // 3. The animation's search: each slot's turn since the first sight, in the vehicle's own space, about an axis passing near the eyes.
    if (searching && D.pal_ref.size() >= 12 && D.pal_now.size() >= 12) {
        const bool      stick = vc.style == fc5::drive::kStick;
        const float     thr   = glm::radians(stick ? 3.0f : 5.0f);
        const float     reach = stick ? 1.2f : 0.6f;   // (the helicopter's cyclic turns about its foot, a metre under the eyes)
        const glm::mat3 Wb{ D.b1 };
        const int       ns = int(std::min(D.pal_now.size(), D.pal_ref.size()) / 12);
        if (vc.cand_frame != frame) {
            vc.cands.clear();
            vc.cand_frame = frame;
        }
        for (int s = 0; s < ns; ++s) {
            const glm::mat4 Psn = vc_slot(D.pal_now, s), Psr = vc_slot(D.pal_ref, s);
            if (!vc_rigid(Psn) || !vc_rigid(Psr)) continue;
            float     ang = 0.0f;
            glm::vec3 ax{ 0.0f }, piv{ 0.0f };
            if (!vc_motion(Psn * glm::inverse(Psr), ang, ax, piv)) continue;
            if (ang < thr || (stick && ang > glm::radians(45.0f))) continue;
            const glm::vec3 ax_w  = glm::normalize(Wb * ax);
            const glm::vec3 piv_w = glm::vec3{ D.b1 * glm::vec4{ piv, 1.0f } };
            const glm::vec3 dv    = cam - piv_w;
            const glm::vec3 foot  = dv - ax_w * glm::dot(dv, ax_w);
            if (glm::length(foot) > reach) continue;
            if (vc.state == 2) {
                // A control is followed: only what turns about it counts (the author's first drive, 11 Nov: another part 11 cm from the wheel's
                // axis turned 18 deg at each sit and hid the wheel's own first turns). Stick: its own bone.
                if (frame - vc.out_frame > 2) continue;
                if (stick) {
                    if (di != vc.key_draw || s != vc.key.slot) continue;
                } else {
                    const glm::vec3 as = glm::normalize(B * ax_w);
                    const glm::vec3 dc = vc.c_ref - B * (cam - foot - o);
                    const float     of = glm::length(dc - as * glm::dot(dc, as));
                    if (vc.style == fc5::drive::kWheel ? std::abs(glm::dot(as, vc.n_ref)) < 0.97f || of > 0.04f : of > 0.20f) continue;
                }
            }
            const VcState::Best c{ ang, di, s, frame, ax_w, cam - foot };
            // The 8 largest turns of the frame.
            if (vc.cands.size() < 8) {
                vc.cands.push_back(c);
            } else {
                auto lo = std::min_element(vc.cands.begin(), vc.cands.end(), [](const VcState::Best& x, const VcState::Best& y) { return x.ang < y.ang; });
                if (lo->ang < ang) *lo = c;
            }
        }
    }
}

// drivemodelshow on|off (saved, default on; FarCry5Drive.h; author 11 Nov 10:00). The game eases its own wheel behind the stick (his drive: 20-30 deg behind the hand while it moved; the
// game's steering takes 220 ms to half and 750 ms to 90 % at full stick, research\2026-11-07_steerlag). While a hand holds the wheel or bars,
// every draw of the control's bone is drawn with that bone turned to the hand's angle: its slots' matrices in the draw's palette are
// replaced by a turn about the bone's axis x the slot's pose when it was taken (veh_control_draw; the vehicle's own space), and the copy bound for this
// draw (hands_cb_swap; the draw hooks put the game's back). The game's own palette is not changed: the detection and the steering still
// read the game's turn. Called after world_character_draw, for a draw that is drawn and has no other palette swapped in.
void FarCry5Stereo::veh_control_show(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_vehcontrol.load() || !m_in_vehicle.load() || !m_veh_driving.load()) return;
    auto&           vc = g_vc;
    std::lock_guard _{ vc.mtx };
    if (!vc.show_on || vc.state != 2 || vc.ov_counts.empty() || m_frame.load() - vc.out_frame > 2) return;
    if (count == 0) {   // an indirect draw: its arguments as veh_control_draw read them
        if (m_hud_draw_kind != 5 || !m_hud_draw_args) return;
        const auto it = vc.args.find(std::make_pair(static_cast<void*>(m_hud_draw_args), m_hud_draw_args_off));
        if (it == vc.args.end() || it->second[0] <= 0) return;
        count = UINT(it->second[0]);
        start = UINT(std::max(0, it->second[2]));
        base  = INT(it->second[3]);
    }
    if (std::find(vc.ov_counts.begin(), vc.ov_counts.end(), count) == vc.ov_counts.end()) return;
    ID3D11Buffer* vb     = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    if (vb) vb->Release();
    for (const VcDraw& D : vc.draws) {
        if (!D.ov || D.count != count || D.start != start || D.base != base || D.vb != vb) continue;
        std::vector<float> pal;
        {
            ID3D11Buffer* cb1 = nullptr;
            ID3D11Buffer* cb3 = nullptr;
            ctx->VSGetConstantBuffers(1, 1, &cb1);
            ctx->VSGetConstantBuffers(3, 1, &cb3);
            std::lock_guard ol{ m_obj_mtx };
            bool            mine = false;
            if (cb1) {
                // This vehicle's (a parked twin's wheel is left alone).
                if (auto it = m_obj_consts.find(cb1); it != m_obj_consts.end())
                    mine = glm::length(glm::vec3{ it->second.f[3], it->second.f[7], it->second.f[11] } - vc.key_b1) < 1.0f;
                cb1->Release();
            }
            if (cb3) {
                if (auto it = m_bone_data.find(cb3); mine && it != m_bone_data.end() && it->second.size() >= 3072) pal = it->second;
                cb3->Release();
            }
        }
        if (pal.size() < 3072) return;
        const glm::mat4 R = glm::translate(glm::mat4{ 1.0f }, D.ov_piv) * glm::mat4_cast(glm::angleAxis(vc.show_turn - vc.ov_turn_c, D.ov_ax)) *
                            glm::translate(glm::mat4{ 1.0f }, -D.ov_piv);
        for (const auto& [sl, rel] : D.ov_slots)
            if (sl >= 0 && size_t(sl + 1) * 12 <= pal.size()) to_rows12(R * rel, pal.data() + size_t(sl) * 12);
        if (hands_cb_swap(ctx, pal, false) && !vc.ov_said) {
            vc.ov_said = true;
            spdlog::info("[VehHands] vehcontrol: the vehicle's own {} drawn at the hand's angle (drivemodelshow): draw {}, {} slots, {:+.0f} deg from where they were taken{}",
                         vc_style_name(vc.style), count, D.ov_slots.size(), glm::degrees(vc.show_turn - vc.ov_turn_c),
                         vc.ov_ok ? "" : " (the rim's own slot until its turn has shown the others)");
        }
        return;
    }
}
