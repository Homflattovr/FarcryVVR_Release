#pragma once
// bodyik (F11 Experimental > Body and arms (test); off by default; research\2026-11-11_bodyik\README.md): the player's own outfit drawn as a body
// under the headset: the torso hangs from the neck, the arms go from its shoulders to the hands the mod places, the legs stand under it.
// It replaces armik (8 Nov), which knew each draw by its index count in tables and broke on the draws the tables did not hold.
//  - No tables. Every character model of the game is skinned on one rest skeleton (667 models read, tools\fullbodyik\fbscan.py), so a
//    palette slot's body part is read from the mesh itself: the rest-pose centre of the vertices it moves (the scan the hands already
//    use) against that skeleton's segments (classify). Any outfit, any draw, direct or indirect.
//  - The body is placed, not corrected: a torso slot's matrix is the rest pose put under the head (body_pose, torso_frame), an arm slot's
//    is the rest pose put on the solved arm (solve_arm, arm_frame). The game's own animation of them is not read, so nothing depends on
//    reading its joints right. Only the hands come from the game and the mod: the wrist is where the hand is drawn.
//  - The legs keep the game's animation (walking, crouching) and are turned and moved as one piece under the body (legs_frame).
// This file is the math only (glm): FarCry5BodyIkDraw.cpp reads the draws and holds the commands; tools\fullbodyik\bench checks it on the
// game's real meshes.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc5::bodyik
{
    // Body parts of the shared rest skeleton (model space: metres, x to the model's right, y forward, z up).
    enum Region : uint8_t { kNone = 0, kTorso, kNeck, kClav, kUpper, kFore, kHand, kUpLeg, kLeg, kFoot, kRegions };
    const char* region_name(int region);
    enum Joint : int { kJHips = 0, kJNeck, kJHeadEnd, kJEye, kJClav, kJShoulder, kJElbow, kJWrist, kJKnuckle, kJHip, kJKnee, kJAnkle, kJToe, kJoints };
    glm::vec3 rest_joint(int joint, int side);   // side 0 the right (x > 0), 1 the left
    float     torso_length();                    // hips to neck (m)

    struct Role
    {
        uint8_t region{ kNone };
        uint8_t side{ 0 };     // 0 right, 1 left (0 for the torso and neck)
        float   t{ 0.0f };     // along the part's segment: 0 at its start (the body's end), 1 at its far end
        float   w{ 0.0f };     // the weight it carries
        float   fit{ 0.0f };   // distance from the segment over the part's radius (under 1: inside)
    };
    struct Roles
    {
        Role  slot[256]{};
        int   n{ 0 };
        float weight[2][kRegions]{};   // by side and part
        float total{ 0.0f };
        float part(int region) const { return weight[0][region] + weight[1][region]; }
        float arm(int side) const { return weight[side][kUpper] + weight[side][kFore]; }
        float legs() const { return part(kUpLeg) + part(kLeg) + part(kFoot); }
    };
    // cent[i] = (weight, rest-pose centre x, y, z in metres) of the vertices slot i moves (weight 0: unused).
    Roles classify(const glm::vec4* cent, int n);

    struct Opts
    {
        bool  on{ false };
        bool  legs{ true };          // bodyiklegs: the legs and feet drawn under the body
        bool  legs_upright{ true };  // bodyiklegsupright: the game's legs posed upside down (ankles above the hips) stay hidden
        bool  collar{ true };        // bodyikcollar: collars and hoods drawn (off: neck parts shrunk onto the neck)
        float neck_down{ 0.146f };   // bodyikneck down back (m): the neck joint below and behind the eyes, head level (the model's own: 0.146 0.047)
        float neck_back{ 0.047f };
        float back{ 0.05f };         // bodyikback (m): the whole body further back than that
        float soft{ 0.97f };         // bodyiksoft: where the arm starts to slow down before straight (fraction of its length)
        float stretch{ 1.25f };      // bodyikstretch: the most the arm lengthens to stay on the hand (past it the hand leaves the wrist)
        float pole_hand{ 0.35f };    // bodyikelbow hand ms: how much the hand's turn steers the elbow (0: the body only) ...
        float elbow_ms{ 60.0f };     // ... and the elbow's easing time
        float leash{ 0.10f };        // bodyiklean leash seconds: how far the neck leads the hips before they follow (m) ...
        float follow_s{ 0.5f };      // ... and how fast the hips come back under the neck
        float bow_from{ 0.70f };     // bodyikbow from gain: past this head pitch down (radians; 40 deg) the torso bows with the head,
        float bow_gain{ 1.0f };      //   this much of the pitch beyond it (the hips go back, the neck stays under the head); 0: never
        int   lag{ 2 };              // a draw before its hand's draw uses the last hand this many frames old at most
        bool  veh{ true };           // bodyikveh: in a vehicle the game's seated body drawn, its arms to the hands (with bodyik on)
        bool  seat_swing{ true };    // bodyikseatswing: seated, a hand on the wheel, bars or stick: the elbow swings so the forearm rolls less, the roll at
                                     //   most kSeatRollMax (off: as before 13 Nov). Off by default in D0D0104D (it swung a free hand's elbow 36-45 deg all
                                     //   the time); on again since 14 Nov 12:03Z for held hands only (a free hand: seatfollow)
        bool  seat_follow{ true };   // bodyikseatfollow: seated, every drawn hand (on its controller, the wheel, the bars or the stick): the two-bone arm with
                                     //   the elbow as standing (down, out and a little back from the body, pole_hand of it by the hand's turn), the forearm
                                     //   rolling at most kSeatRollMax, no seatswing (off: the elbow on the game's side, seatswing as set, as before 14 Nov)
        bool  worn{ true };          // bodyikworn: the slung weapon, the sidearm's holster, the throwables' pouch and the magazine worn on the drawn body (worn_place),
                                     //   their grab zones with them (with bodyik on, on foot); off: placed from the eyes (slingpos, sidearmpos, throwpos, reloadpouch)
        glm::vec3 worn_adj[4]{};     // bodyikwornsling / holster / pouch / mag x y z: each one's move from its place on the body (model metres: right, forward, up)
        float feet_max{ 0.70f };     // bodyikfeet deg to pelvis: the legs turned (about the hips' middle) until the game's feet point where it walks, this
                                     //   much at most (radians; 40 deg; 0: the game's hip line alone sets the legs' heading, as before 13 Nov) ...
        int   feet_to{ 1 };          //   ... to: 1 the feet on the game's own heading (where it walks), 2 the legs' root (the steps) on it, 0 the feet on the drawn body's heading ...
        float feet_pelvis{ 0.5f };   //   ... pelvis: the share of that turn the waistband takes too (the rest twists at the hip joints)
        bool  walk_turn{ true };     // bodyikwalkturn: while the game walks, the drawn body turns onto the game's own heading (standing: the gestures'
                                     //   body yaw, which follows the head past a dead zone and so can stay some degrees off where the game walks)
        // bodyiksnaphold (author 14 Nov 21:58Z, turning with the head's direction): the legs' turn (bodyikfeet) and the walk turn (bodyikwalkturn) are eased onto the game's own heading. A snap turns the
        // game camera some frames before the eyes (snapsync; after them without it), so for those frames the game's heading is the snap's angle
        // off the drawn body: both started to chase it and then eased back over 0.25-0.3 s. Now, from the snap's send until the game's heading is
        // back where it was against the drawn body (within snap_hold_deg; snap_hold_s at most), both hold still. Off: they chase it, as before.
        bool  snap_hold{ true };
        float snap_hold_s{ 0.6f };
        float snap_hold_deg{ 8.0f };
        // bodyikturnease FEET WALK (s, test): how fast the legs' turn and the walk turn follow the game's heading (0.25 and 0.3 until 14 Nov).
        float feet_tau{ 0.15f };
        float walk_tau{ 0.2f };
        // bodyikwalkstick (author 15 Nov 19:35Z): the walk turn counted the game as walking from its legs' speed eased over 0.2 s past 0.4 m/s, so it came on some 0.1 s after
        // the stick was pushed and stayed on about 0.4 s after it was let go (the eased speed falling from a run). Now the player's own left stick
        // says walking at once (pushed past stick_on); room scale's push, with the stick let go, still goes by the speed. Off: the speed alone.
        bool  walk_stick{ true };
        float stick_on{ 0.25f };     //   the stick's length that counts as pushed (the game's own dead zone is about 0.24)
        // bodyikcarrydrawn (author 16 Nov, headset video 13:03: the intro outfit's coat sleeves shake in the left eye only): a frame's draws
        // before its hand draw (the coat's sleeves come first) solve the arm on the last hand carried by each controller's move. That move was
        // taken in update_motion, before gun_anchor_apply moves the controllers by draw_shift (the eyes' base less the game camera, some 10 cm),
        // while the hand it carried was drawn after it: the carried hand missed the change of the shift and the shift turned by the hand's turn.
        // In Stereo only the left eye's game update has a new shift and a new controller sample, so only the left eye's coat left the sleeve and
        // glove drawn after the hand (the exact hand) by up to a few cm, every frame anew. Now the move is taken from the controllers as drawn, at
        // the frame's first arm draw. Off: as before.
        bool  carry_drawn{ true };
        // bodyikbody (author 13 Nov 21:42Z):
        // the player's own measures from the T-pose calibration (bodyikcalibrate, F11 Motion control > Hands, body and arms), saved. The eyes
        // stay at the game camera whatever the player's height (the game's world is the game's), so the body is not made taller or shorter:
        // what changes is the arms' length (straight in a T-pose, as the player's own) and the shoulders' height under the eyes.
        bool  cal{ false };          //   a calibration is known
        float cal_span{ 0.0f };      //   from one controller to the other, arms straight out (real metres)
        float cal_drop{ 0.0f };      //   the controllers below the eyes then: the shoulders' height under the eyes (real metres)
        float cal_grip{ 0.08f };     //   the drawn wrist's distance from the controller along the arm (real metres; measured by the solve when the
                                     //   body was on during the calibration, else this)
        // bodyikseated / seated (author 13 Nov 21:42Z): the drawn hips stay where they are (the chair)
        // and the torso leans from them towards the head; the head going down never crouches the legs; room scale does not walk (RoomScaleMod).
        bool  seated{ false };
        // bodyikhunch (author 14 Nov 15:07Z, the sling, holster and throwables): looking down past bow_from the
        // back curls (the upper torso, from hunch_at of the way up, bows forward; the lower torso leans back as much as keeps the neck over the
        // hips) instead of the whole torso swinging back about the neck. The hips, the legs and what is worn at the hips stay where you stand
        // (the swing took them 20 cm back looking 60 deg down, 33 cm at 75, under the chest and out of reach).
        bool  hunch{ true };
        float hunch_at{ 0.65f };     //   where the back bends (fraction of the torso from the hips)
        float hunch_gain{ 0.5f };    // bodyikhunch gain: how much of the bow the upper back takes (author 14 Nov 16:49Z, on foot with all of it, 1.0: 35 deg looking 75 deg down; 0: the torso stays upright)
    };
    // The model's own measures the calibration is set against (the rest skeleton): the shoulder joint's distance from the body's middle, the arm
    // from the shoulder joint to the wrist, and the eyes above the shoulder joints (neck_down 0.146 + the neck 6.2 cm above the shoulders).
    inline constexpr float kRestShoulderHalf = 0.1708f, kRestArm = 0.5820f, kRestEyeToShoulder = 0.2082f;
    // The calibrated arm's length against the model's (1: none), and the shoulders' move down under the eyes (reference metres), for a world scale.
    float cal_arm_k(const Opts& o, float scale);
    float cal_shoulder_dy(const Opts& o, float scale);

    // bodyikcalibrate: one sample per frame of the T-pose window (reference space: metres, x right, y up, z back; the head's pose and both
    // controllers'), and the result.
    struct CalSample
    {
        glm::mat4 head{ 1.0f };
        glm::vec3 c[2]{};             // the controllers (grip poses)
        float     reach[2]{ 0.0f, 0.0f };   // with the body on: the solve's drawn shoulder-to-wrist distance over the model's arm (0: none)
        float     scale{ 1.0f };      // world units per metre then
    };
    struct CalResult
    {
        bool        ok{ false };
        std::string why;              // not ok: what was missing
        float       span{ 0.0f }, drop{ 0.0f }, back{ 0.0f }, grip{ 0.08f }, level{ 0.0f };
        bool        grip_measured{ false };
        int         used{ 0 }, total{ 0 };
        float       height{ 0.0f };   // the height the arm span suggests (display only)
        float       arm{ 0.0f };      // shoulder joint to wrist (real metres, the model's shoulder width)
    };
    CalResult cal_solve(const std::vector<CalSample>& s);

    // The head, once per frame, in the mod's reference space (m_ref_inv * tracking: metres, x right, y up, z back).
    struct HeadIn
    {
        glm::mat4 head{ 1.0f };          // the headset's pose (midway between the eyes)
        glm::vec3 pivot{ 0.0f };         // the neck's pivot: the point that stays put when the head turns (gestures: 10 cm below, 8 cm behind the eyes)
        float     pivot_down{ 0.10f }, pivot_back{ 0.08f };   // that pivot's place from the eyes (m; the head's own down and back)
        glm::vec3 fwd{ 0.0f, 0.0f, -1.0f };   // the body's heading (level; gestures' body yaw)
        glm::mat3 to_world{ 1.0f };      // reference space -> world axes (the mod's body_to_world * world scale)
        glm::vec3 origin{ 0.0f };        // world position of the reference space's origin (the game camera)
        float     scale{ 1.0f };         // world units per metre
        float     dt{ 0.011f };          // seconds since the last frame
    };
    struct Follow   // kept between frames
    {
        bool      init{ false };
        glm::vec3 hips{ 0.0f };   // reference space, level: where the hips stand
        glm::vec3 last{ 0.0f };   // seated: the last frame's place under the neck (a recentre's jump carries the hips along)
    };
    // The body this frame (world: z up).
    struct BodyPose
    {
        bool      ok{ false };
        glm::vec3 right{ 1, 0, 0 }, fwd{ 0, 1, 0 }, up{ 0, 0, 1 };   // the body's level axes
        glm::mat3 yaw{ 1.0f };          // model axes -> world (upright, heading)
        glm::quat lean{ 1, 0, 0, 0 };   // the torso's lean (world), about the hips
        glm::vec3 neck{ 0.0f }, hips{ 0.0f };   // where the rest skeleton's neck and hips joints are
        glm::vec3 eye{ 0.0f };
        float     drop{ 0.0f };         // how far the hips are below their place with the head level at its reference height (world units; a real crouch)
        glm::vec3 lag{ 0.0f };          // the hips' lag behind the neck (reference space, m, level; the lean's leash): bodyikcarry moves the carried things by it
        float     arm_k{ 1.0f };        // bodyikbody: the arms' length against the model's (cal_arm_k)
        float     hunch_up{ 0.0f };     // bodyikhunch: the upper torso's bow forward (radians, about the body's right; 0: no hunch) ...
        float     hunch_low{ 0.0f };    //   ... the lower torso's lean back (radians), so the neck stays over the hips ...
        float     hunch_at{ 0.65f };    //   ... and where the back bends (fraction of the torso from the hips)
    };
    BodyPose body_pose(const Opts& o, const HeadIn& in, Follow& f);
    // Rest model -> world for a torso part t of the way from the hips (0) to the neck (1).
    glm::mat4 torso_frame(const BodyPose& b, float t);
    // bodyikworn: the carried things worn on the body. Each has a place on the rest skeleton (model metres: x right, y forward, z up; the
    // right-handed player's, mirrored in x for a left-handed one) and is carried by the torso there, as the mesh at that height is:
    // worn_place gives its frame in the world, its axes the gestures' (x right, y up, z back: the carried things' own frame before their
    // angles: slingrot, sidearmrot): the sling, the holster and the magazine upright on the body's heading (they hang), the throwables'
    // pouch turned with the chest it is sewn on.
    enum WornPart : int { kWornSling = 0, kWornHolster = 1, kWornPouch = 2, kWornMag = 3, kWornParts };
    const char* worn_name(int part);
    glm::vec3   worn_rest(int part);   // the default place (no worn_adj, the right-handed player's)
    glm::mat4   worn_place(const BodyPose& b, const Opts& o, int part, bool mirror);
    // The game's legs: object space -> world, in place of the draw's own object transform (floor_z: its height, the game's root).
    glm::mat4 legs_frame(const BodyPose& b, float floor_z);
    // One leg, in the legs' object space (the game's pose there, brought under the body's hips: Frame::stance): the hip moved to where the
    // body's hip is, the foot left where the game put it, the knee bent to fit (a real crouch bends the knees; the game's steps stay).
    // Moves to apply before the legs' frame.
    struct LegSolve
    {
        bool      ok{ false };
        glm::vec3 H{ 0.0f }, K{ 0.0f }, A{ 0.0f };      // the game's hip, knee, ankle
        glm::vec3 Ht{ 0.0f }, Kn{ 0.0f }, An{ 0.0f };   // the new ones
        glm::quat q_up{ 1, 0, 0, 0 }, q_low{ 1, 0, 0, 0 };
        float     lift{ 0.0f };                          // m the foot had to leave its place (the hips too high for the leg)
    };
    LegSolve  solve_leg(const glm::vec3& H, const glm::vec3& K, const glm::vec3& A, const glm::vec3& hip_target);
    glm::mat4 leg_move(const LegSolve& l, int region, float t);

    struct ArmSolve
    {
        bool      ok{ false };
        int       side{ 0 };
        glm::vec3 S{ 0.0f }, E{ 0.0f }, W{ 0.0f };   // shoulder, elbow, forearm's end (world)
        glm::quat q_chest{ 1, 0, 0, 0 };  // rest -> world rotations: the chest,
        glm::quat q_us{ 1, 0, 0, 0 };     //   the upper arm at the shoulder (least swing from the chest),
        glm::quat q_u{ 1, 0, 0, 0 };      //   the upper arm at the elbow (the hinge's own),
        glm::quat q_fe{ 1, 0, 0, 0 };     //   the forearm at the elbow,
        float     tw_u{ 0.0f };           // the upper arm's twist from q_us to q_u (radians about the arm),
        float     tw_f{ 0.0f };           // the forearm's twist from the elbow to the wrist (the hand's roll)
        float     k{ 1.0f };              // lengthening applied
        float     reach{ 0.0f };          // |wrist - shoulder| / the model's arm length (not the calibrated one: bodyikcalibrate reads it)
        float     gap{ 0.0f };            // m the hand is past the arm's end
        float     bend{ 0.0f };           // the elbow's angle (radians, 0 straight)
        glm::vec3 pole{ 0.0f };           // the elbow's direction (world, unit), eased
        glm::mat4 hand{ 1.0f };           // the hand's frame (rest model -> world)
    };
    // hand: the hand bone's frame as drawn (rest model -> world; its wrist is the target). last_pole: the eased direction of the last
    // frame (zero: none), last_twist: the forearm twist of the last frame (for continuity; pass 0 with last_pole zero).
    ArmSolve solve_arm(const Opts& o, const BodyPose& b, int side, const glm::mat4& hand, const glm::vec3& last_pole, float last_twist, float dt);
    // Rest model -> world for an arm part (kUpper, kFore) at t along it.
    glm::mat4 arm_frame(const ArmSolve& a, int region, float t);

    // One draw. The caller owns the per-frame state (Frame) and calls draw() for each player draw.
    struct Side
    {
        // the game's wrist this frame (world as drawn, the game's own pose), from forearm slots
        bool      wg_ok{ false };
        glm::vec3 wg{ 0.0f };
        // the hand as drawn: exact (from a hand draw of this frame) or carried from the last frames
        int       hand_frame{ -1000 };
        bool      exact{ false };
        glm::mat4 hand{ 1.0f };
        glm::mat4 move{ 1.0f };           // the mod's move of the hand this frame (world; with exact)
        glm::mat4 carry{ 1.0f };          // this frame's controller move since the last frame (world): carries an older hand forward
        ArmSolve  arm{};
        int       arm_frame{ -1000 };
        bool      arm_exact{ false };
        glm::vec3 pole{ 0.0f };           // eased elbow direction at the end of the last frame it was solved
        int       pole_frame{ -1000 };
        float     twist{ 0.0f };
        glm::vec3 pole_now{ 0.0f };       // this frame's (eased once)
        int       pole_now_frame{ -1000 };
    };
    // The same arm bone in every draw. A bone of the game's skeleton has the same matrix in every mesh it moves, so arm slots of different
    // meshes whose matrices are equal are one bone: they share one place along the arm, read from all their vertices together, and so get
    // the very same new matrix. Without this a skin under a sleeve, or a sleeve under a jacket, would slide against it (each mesh's own
    // vertices give a slightly different place) and show through. Two slots of one mesh are never one bone; where the game moves several
    // bones as one, a slot goes with the nearest. Kept between frames; a slot whose matrix stops matching leaves its bone and does not go
    // back to it. (The chest needs none of this: every chest, neck and clavicle slot gets the one chest matrix.)
    struct Bones
    {
        struct Member { uint64_t mesh{ 0 }; int slot{ 0 }; float w{ 0.0f }; glm::vec3 c{ 0.0f }; int seen{ 0 }; int left{ -1 }; };   // left: the bone it was wrongly in
        struct Entry { std::vector<Member> m; glm::mat4 now{ 1.0f }; int now_frame{ -1 }; };
        std::vector<Entry>                             e;
        std::unordered_map<uint64_t, std::vector<int>> of;   // mesh -> each slot's entry (-1: none)
        int                                            joins{ 0 }, leaves{ 0 };
    };
    struct Frame
    {
        Bones    bones{};
        LegSolve leg[2]{};
        int      leg_frame[2]{ -1000, -1000 };
        glm::mat4 stance{ 1.0f };        // the game's legs brought under the body's hips (legs' object space: a turn about the vertical and a level move)
        bool     cal_ok{ false };
        float    cal{ 0.0f };            // the game's standing hips above the body's own (m, 6 cm at most): the legs keep the game's stance at the reference height
        float    feet{ 0.0f };           // bodyikfeet: the legs' extra turn now (radians, eased), so the game's feet point along the body
        float    feet_raw{ 0.0f };       //   the turn the game's feet asked for this frame (before the limit and the easing)
        int      feet_frame{ -1 };
        int      inverted_frame{ -1000 };   // bodyiklegsupright: the last frame the game's legs were seen upside down
        bool     turn_hold{ false };     // bodyiksnaphold: a snap turn in flight, the legs' turn holds where it is (set once a frame by the caller)
        float    root_ang{ 0.0f }, hip_ang{ 0.0f }, feet_body{ 0.0f };   // bodyikfeet's log (radians, + left of the body's heading): the game's heading,
                                                                         //   the legs' turn to square the hip line, the feet's mean so squared
        glm::vec3 root_pos{ 0.0f };      // the legs draw's object origin (world) this frame: the game's own walk
        bool     feet_read{ false };     // both feet's headings were read lately
        glm::vec2 toe_dir[2]{ glm::vec2{ 0.0f, 1.0f }, glm::vec2{ 0.0f, 1.0f } };   // each foot's level heading as the game poses it (legs' object space) ...
        int      toe_frame[2]{ -1000, -1000 };                                   // ... and the frame it was read
        int      frame{ -1 };
        float    dt{ 0.011f };
        BodyPose body{};
        Side     side[2]{};
    };
    // Start of a frame: the body, and each side's controller move since the last frame (identity if not known).
    void begin_frame(Frame& f, int frame, float dt, const BodyPose& body, const glm::mat4& carry_right, const glm::mat4& carry_left);
    // bodyikcarrydrawn: each side's hand not yet drawn this frame carried by that controller's move as drawn (begin_frame then given identities).
    void carry_hands(Frame& f, const glm::mat4& carry_right, const glm::mat4& carry_left);

    enum Pass : int { kHandDraw = 0, kHiddenArm = 1, kHiddenCloth = 2, kWorld = 3 };
    struct DrawIn
    {
        int              pass{ kHiddenArm };
        uint64_t         mesh{ 0 };          // the mesh's identity (0: none: its slots are not matched with other draws')
        const float*     game{ nullptr };    // the game's palette (256 x 12, rows)
        const float*     fin{ nullptr };     // the palette as it would be drawn now (the mod's hand moves in it); == game if untouched
        int              nslots{ 256 };
        const glm::vec4* cent{ nullptr };    // per slot (weight, rest centre in metres)
        glm::mat4        M{ 1.0f };          // object space -> world as drawn (weapon pass: its placement times the object transform)
        int              fold_side{ -1 };    // hand draw of one bare arm (barehands): its side, and
        float            fold_from{ 0.0f };  //   the |x| (m, rest) below which the mod folded its slots onto the wrist
        const uint8_t*   folded{ nullptr };  // handroles: per slot, the mod folded it onto the wrist (replaces the two above)
    };
    enum Result : int
    {
        kMoved = 0,       // out holds the palette to draw with
        kNoBody,          // no head pose this frame
        kNoHand,          // an arm of it has no hand to go to (yet): left as it was
        kNotLegs,         // world pass: not the legs
        kLegsOff,
        kHeadPiece,       // mostly neck and head: stays hidden
        kNothing,         // no slot to place (hands only): left as it was
        kBadInput,
        kLegsInverted,    // world pass: the game's legs upside down (a cutscene hangs the player by the feet): stay hidden
    };
    const char* result_name(int r);
    struct DrawInfo
    {
        int   placed{ 0 }, kept{ 0 }, unused{ 0 };
        bool  solved[2]{ false, false };    // this draw gave the side's hand
        bool  predicted[2]{ false, false };
        float wrist_fit[2]{ -1.0f, -1.0f };   // m between the hand bone's wrist and the forearm's, game pose (-1: not looked at)
        int   hand_slot[2]{ -1, -1 };
        Roles roles{};                      // as used: after the same bones of other draws were taken into account
        int   shared{ 0 };                  // slots that are a bone another draw has too
        bool  legs_read{ false };           // world pass: this draw gave the legs' joints
        float knee[2]{ 0.0f, 0.0f };        // the knees' bend after the solve (radians)
    };
    // out: 256 x 12 floats (starts as a copy of fin). Returns a Result.
    int draw(Frame& f, const Opts& o, const DrawIn& in, float* out, DrawInfo* info);

    // ---- bodyikveh: the seated body in a vehicle (FarCry5BodyIkDraw.cpp bodyik_veh) ----
    // Driving or riding, the game draws the player seated in the world pass: torso, legs and arms on the wheel, bars or stick. The body is
    // drawn as the game poses it, moved by shift (onto the eyes' base, where the hands are drawn from), but for the arms: each side whose
    // hand the mod moved (drivehands: onto its controller or kept on the wheel) has its upper arm and forearm turned so that the wrist meets
    // the moved hand, the elbow on the game's side of the shoulder-wrist line, the forearm rolling with the hand towards the wrist.
    struct SeatArm
    {
        bool      ok{ false };
        glm::mat4 upper{ 1.0f };          // world, the game's pose (shift included) -> drawn: the upper arm
        glm::mat4 fore{ 1.0f };           // the same for the forearm, before its roll
        glm::vec3 axis{ 0.0f }, pivot{ 0.0f };   // the forearm's roll: about axis through pivot (the elbow), twist x its slot's t
        float     twist{ 0.0f };
        float     reach{ 0.0f };          // |target - shoulder| / the arm's length
        float     k{ 1.0f };              // lengthening applied
        float     gap{ 0.0f };            // m the hand was past the arm's end once lengthened: the forearm is pulled that far onto the hand
        glm::vec3 lunge{ 0.0f };          // the shoulder's move towards the hand (m, world; at most kSeatLunge)
        glm::vec3 E{ 0.0f }, W{ 0.0f };   // the elbow and wrist as drawn
        float     swing{ 0.0f };          // seatswing: the elbow turned about the shoulder-wrist line from the game's side (radians)
        float     roll_asked{ 0.0f };     // the forearm's roll the hand asked for (twist is that, at most kSeatRollMax with seatswing or seatfollow)
        float     bend{ 0.0f };           // the wrist's bend: the drawn forearm against the forearm the hand carries (radians)
    };
    // A hand further than this share of the arm's length takes the shoulder with it, up to kSeatLunge (the shoulder blade sliding forward).
    inline constexpr float kSeatLungeFrom = 0.92f, kSeatLunge = 0.08f;
    // seatswing: the elbow may swing up to kSeatSwingMax either way (in kSeatSwingSteps steps a side) so the forearm rolls less; a roll up to
    // kSeatRollEasy costs nothing, and the forearm never rolls more than kSeatRollMax (the hand's own slots carry the rest). The elbow moves
    // towards the place chosen by at most kSeatSwingRate a frame.
    inline constexpr float kSeatSwingMax = 0.7853982f /* 45 deg */, kSeatRollEasy = 0.7853982f /* 45 deg */, kSeatRollMax = 1.7453293f /* 100 deg */;
    inline constexpr int   kSeatSwingSteps = 5;
    inline constexpr float kSeatSwingRate  = 0.0698132f;   // 4 deg a frame (about 380 deg/s at 95 eye images a second)
    // S, E, W: the game's shoulder, elbow and wrist (world, shift included); target: where the wrist goes; hand: the hand's turn (game -> drawn).
    // The wrist always ends on the target: past the shoulder's reach and the lengthening, the forearm is moved onto the hand (the elbow opens a
    // little) instead of the wrist's skin being stretched to it (author 12 Nov 20:51, headset: a thin forearm many times its length).
    // prev_swing: the last frame's swing (radians); swing: seatswing on (off: the game's elbow side and the whole roll, as before 13 Nov).
    // follow (seatfollow, before swing): the forearm moved as one with the hand (the game's wrist bend and roll kept), the upper arm from the
    // shoulder to that elbow turned and lengthened or shortened (k is then the upper arm's); no lunge, no gap, no search, no state.
    // follow (seatfollow) and pole (world, unit): the elbow towards pole instead of the game's side (zero: the game's side).
    SeatArm seat_arm(const glm::vec3& S, const glm::vec3& E, const glm::vec3& W, const glm::vec3& target, const glm::mat3& hand, float stretch,
                     float prev_swing = 0.0f, bool swing = false, bool follow = false, const glm::vec3& pole = glm::vec3{ 0.0f });
    struct SeatState   // kept between draws: the game's joints of each side (in the draw's object space, before the shift), as last seen
    {
        glm::vec3 j[2][3]{};   // shoulder, elbow, wrist
        int       jf[2][3]{ { -1000, -1000, -1000 }, { -1000, -1000, -1000 } };
        glm::mat3 palm[2]{ glm::mat3{ 1.0f }, glm::mat3{ 1.0f } };   // seatfollow: the game's palm turn (rest -> object space), for every draw of the frame
        int       palmf[2]{ -1000, -1000 };
        float     swing[2]{ 0.0f, 0.0f }, swing_prev[2]{ 0.0f, 0.0f };   // seatswing: this frame's and the last frame's elbow swing
        int       swing_frame[2]{ -1000, -1000 };
    };
    struct SeatIn
    {
        int              frame{ 0 };
        const float*     game{ nullptr };      // the game's palette (256 x 12, rows)
        const float*     fin{ nullptr };       // the palette as drivehands made it (its hands placed); == game if it did not take the draw
        int              nslots{ 0 };
        const glm::vec4* cent{ nullptr };      // per slot: weight, rest centre (m)
        glm::mat4        M{ 1.0f };            // palette space -> world (the draw's object transform)
        glm::vec3        shift{ 0.0f };        // the body's move (world)
        glm::mat4        hand[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };   // each side's hand move (world: the game's pose -> drawn)
        bool             moved[2]{ false, false };    // the side's hand move is known
        bool             hidden[2]{ false, false };   // drivehands draws no hand on that side (the game's own sidearm is out): the arm stays the game's
        bool             held[2]{ false, false };     // drivehandsgrab: the hand on the wheel, bars or stick (seatfollow: only a free hand's forearm follows it)
        bool             fin_hands{ false };   // the hand slots are drivehands' (fin): kept
        float            stretch{ 1.25f };
        bool             swing{ false };      // seatswing
        bool             follow{ true };      // seatfollow (before swing)
        float            pole_hand{ 0.35f };  // seatfollow: the hand's share of the elbow's direction (bodyikelbow, as standing)
    };
    struct SeatInfo
    {
        Roles roles{};
        int   placed{ 0 };
        bool  solved[2]{ false, false };
        float reach[2]{ 0.0f, 0.0f };
        float gap[2]{ 0.0f, 0.0f };
        float lunge[2]{ 0.0f, 0.0f };   // m the shoulder went towards the hand
        float k[2]{ 1.0f, 1.0f };
        bool  unsolved[2]{ false, false };   // the hand was moved but its arm could not be solved (its joints not seen yet): drivehands' fold kept
        float swing[2]{ 0.0f, 0.0f };   // seatswing: the elbow's swing (radians)
        float roll[2]{ 0.0f, 0.0f };    // the forearm's roll the hand asked for (radians, before the kSeatRollMax limit)
        float bend[2]{ 0.0f, 0.0f };    // the wrist's bend (radians): the drawn forearm against the drawn palm's own (straight-wrist) line
        glm::vec3 S[2]{}, E[2]{}, W[2]{}, T[2]{};   // world, as drawn: the shoulder (lunge included), elbow, wrist; the wrist's target (the hand)
        glm::vec3 elbow[2]{};   // the drawn elbow from the drawn shoulder in the player's axes (m: out from the body's middle, forward, up)
    };
    // out: 256 x 12 floats. Returns kMoved, kHeadPiece (a hat, hair, the face: not drawn), kNothing (no bones) or kBadInput.
    int seat(SeatState& st, const SeatIn& in, float* out, SeatInfo* info);

    // handroles: what each palette slot is to its hand, read from the mesh alone (as classify: the slot's rest-pose centre against the shared
    // rest skeleton, here the wrist and the finger bones). Measured against the bone names on the game's first-person models
    // (tools\fullbodyik\handstudy): 99% of the finger bones get their finger, 98% of their weight the right joint.
    //  - A hand slot: its centre past the wrist (along the forearm) and within 8 cm of a finger bone.
    //  - On a finger: the nearest finger bone; near one of its ends (15%) the slot sits on that joint (it takes half of its bend).
    //  - The palm: the heaviest slot at the wrist end of the hand; the other slots short of the knuckles ride the palm.
    //  - The cuff: the forearm's last third on a side that has a hand (it goes with the hand, as a glove's cuff does today).
    // A mesh holds a hand on a side when at least 60 vertices' weight is on it and it has three finger slots or is mostly hand (a ring set).
    enum HandKind : uint8_t { kHandNone = 0, kHandPalm, kHandOnPalm, kHandOnBone, kHandOnJoint, kHandCuff };
    struct HandSlot
    {
        uint8_t kind{ kHandNone };
        uint8_t side{ 0 };      // 0 right (x > 0), 1 left
        uint8_t finger{ 0 };    // 0 thumb .. 4 little
        uint8_t seg{ 0 };       // kHandOnBone: the bone (1..3); kHandOnJoint: the joint (1..3); kHandOnPalm: 0
        bool    main{ false };  // the heaviest slot on that finger bone (the bone itself, most likely)
    };
    struct HandRoles
    {
        HandSlot slot[256]{};
        int      palm[2]{ -1, -1 };
        int      fingers[2]{ 0, 0 };        // slots on finger bones and joints
        float    weight[2]{ 0.0f, 0.0f };   // on the hand
        float    side_weight[2]{ 0.0f, 0.0f };   // everything on that side of the body
        bool     hand[2]{ false, false };
        float    centre_off[2]{ 0.0f, 0.0f };    // a hand: its slots' centre (hand and cuff, weight 5 and up, slot 1 and up) from hand_centre (m)
        bool     uneven[2]{ false, false };      // further than 1.5 cm: the mesh covers the hand unevenly (a bandage, a strap, long or fingerless gloves)
        bool     any() const { return hand[0] || hand[1]; }
    };
    HandRoles hand_roles(const glm::vec4* cent, int n);
    glm::vec3 rest_finger(int side, int finger, int k);   // k: 0 the metacarpal's root (the thumb: the wrist), 1..3 the joints, 4 the tip
    glm::vec3 hand_centre(int side);   // the full hand's centre at rest (m, model space): where the bare arms' and the full gloves' hand slots have theirs
    bool      hand_roles_on();                            // handroles on|off (FarCry5BodyIkDraw.cpp)

    // Options, commands and the F11 section (FarCry5BodyIkDraw.cpp).
    Opts        opts();
    bool        on();
    bool        seated();            // bodyikseated / seated (the body, and room scale)
    // bodyikcalibrate (FarCry5BodyIkDraw.cpp; the Stereo side feeds it, FarCry5Stereo::bodyik_cal_tick): 0 idle, 1 get into the T-pose
    // (kCalWait s), 2 sampling (kCalSample s). cal_take_start: true once after a start (the start buzz). cal_add: a sample while sampling.
    // cal_tick: the time; true once when the window has just ended (then cal_finish solves it, applies and returns the settings line).
    inline constexpr float kCalWait = 3.0f, kCalSample = 3.0f;
    int         cal_state();
    bool        cal_take_start();
    void        cal_add(const CalSample& s);
    bool        cal_tick();
    std::string cal_finish(CalResult* out);
    std::string cal_text();
    // The headset popup (author 14 Nov 11:24Z). On the framework's notice layer
    // (drawn while F11 is closed, in front of the head, as loadpopup): the calibration's countdown, the measure and its result for kCalShow s;
    // and once (bodyikintro, saved, default on, then off), after the first kIntroAfter s of play, the welcome with where the button is, for
    // kIntroShow s or until F11 opens. true while something is drawn.
    inline constexpr float kCalShow = 6.0f, kIntroAfter = 4.0f, kIntroShow = 25.0f;
    bool        popup_draw();
    void        intro_play(bool in_play);   // once per frame (present): in play on foot, nothing else on screen; arms the welcome
    bool        intro_take_shown();         // true once, when the welcome was first drawn: save bodyikintro off
    void        intro_dismiss();            // F11 opened (or a calibration started): the welcome closes
    int         dump_frames();       // bodydump: frames still to write
    int         snap_log_frames();   // bodyiksnaplog: per-frame lines still to write
    void        snap_log_done();
    void        dump_frame_done();
    int         seat_dump_frames();   // bodyikseatdump: frames still to write
    void        seat_dump_frame_done();
    void        note_status(int placed, int left, int hands, int carried, int legs, float reach_r, float reach_l, float gap, float fit, float cal);
    std::string status();
    bool        command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    void        draw_mode(std::string& cmds, const std::function<void(const char*)>& hint, bool seated_row = true);   // Hands only / Full body and arms (bodyik); F11 Main: no seated row
    void        draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, bool debug);

    // Helpers.
    glm::mat4 from_rows(const float* f);
    void      to_rows(const glm::mat4& m, float* f);
    glm::quat arc(const glm::vec3& a, const glm::vec3& b);
    glm::quat rot_of(const glm::mat4& m);   // the rotation of a matrix whose axes may be scaled
}
