#pragma once
// armik (F11 Experimental > Arms IK (test); off by default, saved once the author turns it on; research\2026-11-08_fullbody_ik.md step 1):
// the game's first-person arms, sleeves and gloves drawn from the shoulder to the hand the mod places, for every outfit.
//  - Every first-person draw is known by bonemap (FarCry5BoneNames.h); each of its palette slots has a role (FarCry5BoneRoles.h, made by
//    tools\fullbodyik\gen_anchors.py): the arm bone it is or hangs from (a cuff helper moves with its forearm twist bone, a finger with its
//    hand, cloth with the spine). One skeleton for all 130 first-person models, so no skin needs its own data.
//  - The joints the game posed (shoulder, elbow, wrist) are read from the draw's own palette (the slot's rest joint through its matrix);
//    the hand is where the mod drew it (the gloves' final palette: moved to the controller, or the game's on the weapon).
//  - The shoulders hang from the head (neck point, body heading as gestures: fc5::gesture::body_yaw), the two-bone solve puts the elbow,
//    and each role gets one rigid move in the drawn world (D); a slot's new matrix is M^-1 * D * M * its game matrix (M: rest model ->
//    drawn world, the object transform then the weapon pass's placement).
//  - The sleeves and bare forearms the mod hides with hand models on are drawn again with the same moves (armiksleeves).
// This file is the math only (glm): FarCry5ArmIkDraw.cpp reads the draws and holds the commands; tools\fullbodyik\armik_test.cpp checks it.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::armik
{
    // Roles (FarCry5BoneRoles.h kRole): low 6 bits the role, 0x40 the slot is that standard bone itself, 0x80 the right side.
    enum Role : uint8_t
    {
        kOther = 0, kHips = 1, kSpine = 2, kSpine1 = 3, kSpine2 = 4, kNeck = 5, kHead = 6, kLegs = 7, kRoot = 8,
        kClav = 16, kUpper = 17, kUpTwA = 18, kUpTwB = 19, kElbow = 20, kFore = 21, kFoTwA = 22, kFoTwB = 23, kHand = 24,
    };
    constexpr uint8_t kPrimary = 0x40, kRightBit = 0x80;
    inline int  role_of(uint8_t r) { return r & 0x3f; }
    inline int  side_of(uint8_t r) { return (r & kRightBit) ? 0 : 1; }   // 0 right, 1 left (the gloves' side numbering)
    inline bool is_arm(int role) { return role >= kClav && role <= kHand; }
    inline bool is_body(int role) { return role >= kHips && role <= kRoot; }
    enum Joint : int { kJShoulder = 0, kJArm = 1, kJForeArm = 2, kJHand = 3 };   // FarCry5BoneRoles.h kRest[side][joint]

    struct Opts
    {
        bool  on{ false };
        bool  sleeves{ true };      // armiksleeves: the sleeves and bare forearms the mod hides drawn again, moved with the arm
        bool  body{ false };        // armikbody: draws made mostly of torso and leg bones too (off: they stay hidden)
        bool  torso{ true };        // armiktorso: the clavicles and spine bones turned upright to the body's heading (off: only moved)
        float down{ 0.22f };        // armshoulder down back (m): the shoulders' line below and behind the eyes, body frame
        float back{ 0.06f };
        float soft{ 0.97f };        // armiksoft: the reach where the arm starts to slow down before straight (fraction of full length)
        float stretch{ 1.10f };     // armikstretch: the most the arm may lengthen to reach the hand (1: never; past it the hand leaves the wrist)
        float pole_body{ 0.5f };    // armikpole body game hand: weights of the elbow's direction: down and out from the body,
        float pole_game{ 0.3f };    //   the game's own elbow (as it bent the arm), the hand's turn
        float pole_hand{ 0.2f };
        float elbow_ms{ 50.0f };    // armikelbow: the elbow direction eased over this time (ms; 0 none)
        float lag_frames{ 2.0f };   // a draw before this frame's solve uses the last solve this many frames old at most
    };

    // The pose of one side as the game drew it (drawn world): joints and frames read from the palette.
    struct GameArm
    {
        bool      s{ false }, e{ false }, w{ false }, h{ false };
        glm::vec3 S{ 0.0f }, E{ 0.0f }, W{ 0.0f };   // shoulder (Arm joint), elbow (ForeArm joint), wrist (Hand joint)
        glm::mat4 H{ 1.0f };                          // the Hand bone's frame (rest model -> drawn world)
    };

    // The body: the game's shoulder line (both Arm joints) and its up, and where it must be.
    struct BodyIn
    {
        glm::vec3 S0[2]{};            // the game's shoulder joints (side 0 right)
        glm::vec3 up0{ 0, 0, 1 };     // the game's torso up (drawn world)
        glm::vec3 mid{ 0.0f };        // target: the point between the shoulders (drawn world)
        glm::vec3 right{ 1, 0, 0 };   // target: the body's right, level
        glm::vec3 up{ 0, 0, 1 };      // target: up
        bool      mirrored{ false };  // the placement mirrors (left-handed): the model's right side is drawn on the left
    };
    // D_torso: the game's shoulder line onto the body's (rigid). upright=false: moved only (the game's tilt kept).
    glm::mat4 torso_move(const BodyIn& b, bool upright);

    struct SideOut
    {
        bool      ok{ false };
        glm::mat4 D[25]{};            // per role (kClav..kHand used): the move in the drawn world
        glm::vec3 S{ 0.0f }, E{ 0.0f }, W{ 0.0f };   // solved joints (diagnostics)
        float     reach{ 0.0f };      // |W - S| / full length
        float     stretch{ 1.0f };    // applied lengthening
        float     gap{ 0.0f };        // m left between the forearm's end and the wrist (beyond armikstretch)
        glm::vec3 pole{ 0.0f };       // the eased elbow direction (unit, drawn world), kept between frames
    };

    // Options and commands (FarCry5ArmIkDraw.cpp). armik on|off (saved), armiksleeves, armikbody, armiktorso on|off, armshoulder down back,
    // armiksoft f, armikstretch f, armikpole body game hand, armikelbow ms (saved with armik); armcensus (once: the next frame's player draws
    // logged, never saved).
    Opts        opts();
    bool        on();
    bool        take_census();   // true once after armcensus
    bool        command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    void        draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
    std::string status();
    void        note_status(int solved, int moved, int kept_hidden, int unknown, float reach_r, float reach_l, float gap_max);

    // One side: the game's arm (orig), the hand where it is drawn now (Hf: the Hand bone's final frame), the torso move, the target body
    // frame (out = the side's outward direction, down, back), the last eased pole (zero: none) and the frame time.
    // twist[k]: where along the forearm the forearm twist bone k (0 TwistA, 1 TwistB) sits, 0 elbow .. 1 wrist (from the rest joints).
    SideOut solve_side(const Opts& o, const GameArm& g, const glm::mat4& Hf, const glm::mat4& Dt, const glm::vec3& out, const glm::vec3& down,
                       const glm::vec3& back, const glm::vec3& last_pole, float dt_s, const float twist[2]);

    // A slot's new palette matrix: M^-1 * D * M * P (row-major 3x4 in and out, as the game's palette).
    void move_slot(const float* p_in, float* p_out, const glm::mat4& M, const glm::mat4& Minv, const glm::mat4& D);

    // Helpers (exposed for the test).
    glm::quat arc(const glm::vec3& a, const glm::vec3& b);   // the shortest turn of direction a onto b
    glm::mat4 rigid(const glm::vec3& to, const glm::mat3& R, const glm::vec3& from, const glm::vec3& axis, float k);   // T(to) S(axis,k) R T(-from)
    glm::mat4 from_rows(const float* f);
    void      to_rows(const glm::mat4& m, float* f);
    // The fraction along a segment (0 at a, 1 at b) of point p's projection.
    float along(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p);
}
