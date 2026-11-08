#pragma once
// fingernames on|off (saved, default on; F11 Experimental > Hands (test); author 2 Nov 14:37): the hands that follow their controllers (fingergrip) posed by their bones' names.
//  - The bone map (FarCry5BoneNames.h) names every palette slot of the game's hand draws (gloves, bare arms, outfits): the palm
//    (RightHand), the metacarpals (RightInHandIndex ..), each finger's three joints (RightHandIndex1-3, RightHandThumb1-3) and the
//    bare arms' skin helpers (Index_R_01-06 along the bones, Indexknuckles_R_01-03 on the joints, RightHandThumbHelper).
//  - Each finger is bent joint by joint about its own axis (Chasm's VR framework, chasmlol/chasms-vr-framework, MIT: per-joint full
//    curls, the thumb folding across the palm, fingers closing together into a fist), and every bone of that finger follows its
//    joint: a helper along a bone rides it, a helper on a joint takes half of that joint's bend.
//  - The shape-based pose (hand_fingers, 19-20 Oct) stays for draws the bone map does not know and with fingernames off.
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace fc5::fingers
{
    // The controller of one hand: grip held, trigger pressed, index on the trigger (touch), thumb on a button, the rest or the stick;
    // test: fingertest (-1 none, 0 open, 1 pointing, 2 fist), which wins over hold.
    // fingerholds on|off (saved, default on; author 2 Nov 14:50): what the hand holds, set by the caller from the bow and reload states; the fingers take that shape
    // (kHold in FarCry5Fingers.cpp) instead of the controller's. kString: the bow's string pinched (nocked and gripped, or drawing);
    // kNock: an arrow carried in the hand; kMagazine / kMagSmall: a rifle's / a pistol's magazine held or going into the well;
    // kHandle: the charging handle racked; kSlide: a pistol's slide racked; kRound: a revolver round or a shotgun shell pinched between
    // the thumb and index tips (manualreload46).
    enum Hold : int { kFree = 0, kString, kNock, kMagazine, kMagSmall, kHandle, kSlide, kRound, kHoldN };
    struct Input
    {
        bool grip{ false }, trig{ false }, ttouch{ false }, thumb{ false };
        int  test{ -1 };
        int  hold{ kFree };
        uint32_t sig{ 0 };   // the weapon whose magazine the hand holds (fingergame)
    };
    bool on();
    // fingergame on|off (saved, default off; author 2 Nov 20:38): the game's own grip on each weapon's spare magazine, learnt while its
    // reload animation brings it in (the prop drawn), is the fingers' shape when the free hand holds that weapon's magazine.
    bool game_on();
    // Each frame a hand draw is seen with the game's pose (before the mod moves it): side, the weapon, and whether the game's prop
    // magazine is drawn (this frame or the last). A stretch of 8 frames or more holding it (or the handle, game_holds) is learnt when it ends.
    void learn(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, uint32_t sig, bool prop, int frame);
    // From the manual reload's learning (FarCry5ReloadMag.cpp): the game's hand pulls the charging handle (or a pistol's slide) in
    // that frame (kHandle; its reload from empty, the handle part moving back). That stretch is learnt as the weapon's handle grip,
    // used while the free hand racks it (kHandle, kSlide). A note counts for its frame and the next two (it comes a frame late).
    void game_holds(int hold, int frame);
    // The game's own palm of the free hand (side) in the weapon pass, for the manual reload's learning (reloadhandmag per weapon), each
    // frame the mod moves that hand to its controller (whatever fingergame says):
    //  - palm_world: the palm bone (RightHand / LeftHand) as the game placed it, rest pose space -> game world (the space of
    //    FarCry5ReloadMag.cpp's parts: the draw's object transform (VS b1) times its palette row);
    //  - ctrl_from_palm: rest pose space -> that hand's controller axes (m_mo.other_rot_world / other_pos_world, drawn world units),
    //    as the mod's move places it this frame. So a part P (rest -> game world) held as the game holds it sits in the controller's
    //    axes at ctrl_from_palm * inverse(palm_world) * P.
    // frame: the frame it was taken (the last weapon pass that moved the hand). False before any.
    void note_game_palm(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, const glm::mat4& obj,
                        const glm::mat4& ctrl_from_world, int frame);
    bool game_palm(int side, glm::mat4& palm_world, glm::mat4& ctrl_from_palm, int& frame);
    // reloadrec (FarCry5Reload.h): the free hand's controller as the mod drew it, the palm's middle in rest pose space, and each finger joint's
    // bend (deg; thumb, index, middle, ring, little; 3 joints each): the game's before the mod moves the hand, or as drawn after it posed the fingers.
    void note_free_ctrl(int side, const glm::mat4& ctrl);
    bool free_ctrl(int side, glm::mat4& ctrl, glm::vec3& palm_rest);
    void track_bends(bool on);
    void note_drawn_bends(const std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale);
    bool bends(int side, bool drawn, float* out15);
    bool game_finger_turns(int side, glm::quat* out15);   // ... the game's turns themselves, as in fc5vr_fingerposes.txt
    // reloadhandgame (FarCry5ReloadFingers.h): the game's grips on a handgun's magazine and slide, recorded, built in: they come before fingergame's learnt ones.
    void use_builtin_grips(bool on);
    // One hand (side 0: the right glove, rest-pose x > 0) of a hand draw: its palette (256 rows of 3x4, as VS b3), its slots' rest-pose
    // vertex centres (x: vertices, yzw: position in vertex units), its index count and the vertex scale (m per unit). The fingers of
    // that hand are posed on its palm (RightHand / LeftHand). False when the draw is not known by name: the caller poses it.
    bool pose(std::vector<float>& bones, const std::vector<glm::vec4>& cent, uint32_t count, int side, float scale, const Input& in, int frame);
    // fingernames on|off, fingerholds on|off, fingergame on|off, fingerfist S (saved); fingerlog, fingergameforget (tests).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // F11 Experimental > Hands (test): its rows; commands go into cmds.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
