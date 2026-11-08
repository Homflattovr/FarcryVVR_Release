#pragma once
// manualbow on|off (saved, default off, experimental; author 1 Nov; the idea from NotLooky's BannerlordVR). With a bow in the game's hands (a weapon id in bowweapons,
// ticked in F11 Experimental > Manual bow), on foot with motion controls:
//  - the bow is in the other hand (the left one; the right one when left-handed): the weapon pass is placed on that controller,
//    on the game's own left hand on the bow, and the weapon hand's glove follows its own controller (as with the sling);
//  - the weapon hand's shoulder zone (shoulderswitch's place and size) is a quiver: the grip pressed there takes an arrow;
//  - the arrow brought to the nock point (just above the bow hand, bownock) while the grip is held sticks to the bow, and that grip
//    holds the string (nocked within bownockzone, 0.22 m); let go before drawing, the arrow stays on the bow and a grip press at the nock takes the string again;
//  - pulling the string back: bowstart (m) past where the arrow was nocked the game's fire (pad RT) is held, so the game draws its bow; at bowfull a
//    short buzz on both hands says the bow is fully drawn;
//  - letting go of the grip releases RT (after bowholdms at least), and the game looses the arrow. While drawn, the bow (and the shot)
//    points along the arrow, from the string hand through the nock point.
// The state lives on the pad poll (FarCry5Gesture.cpp calls update()); update_motion reads phase() / drawn() for the bow's pose.
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace fc5::bow
{
    enum class Phase : int { Off = 0, Empty, Arrow, Nocked, Drawing, Loose };

    struct In
    {
        bool      active{ false };       // manualbow on, a bow in the game's hands, on foot with motion controls
        glm::vec3 hand{ 0.0f };          // the string (weapon) hand, tracking space (m)
        bool      hand_ok{ false };
        glm::mat3 hand_rot{ 1.0f };      // its controller's axes (the arrow in the hand points along -z)
        bool      hand_rot_ok{ false };
        bool      grip{ false };         // its grip now
        bool      pressed{ false };      // its grip pressed this poll
        glm::vec3 bow_pos{ 0.0f };       // the bow (other) hand's controller, tracking space
        glm::mat3 bow_rot{ 1.0f };       // its axes (x right, y up, -z where it points)
        bool      bow_ok{ false };
        float     quiver_dist{ -1.0f };  // the string hand to the quiver (the weapon hand's shoulder zone), m; < 0 unknown
        float     quiver_radius{ 0.24f };
        bool      mirror{ false };       // left-handed (bownock's x is mirrored)
    };

    struct Out
    {
        bool  took_press{ false };   // the grip press of this poll went to the bow: no gesture zone gets it
        bool  grip_taken{ false };   // the weapon hand's grip belongs to the bow now (not its usual action)
        bool  rt{ false };           // the game's fire (pad RT) held: the game draws its bow
        float buzz[2]{};             // by role (gesture::Hand): amplitude, and length (s)
        float buzz_s[2]{};
        uint16_t key_scan{ 0 };      // bowmindraw: the game's cancel-shot key to tap now (scan code)
        uint16_t buttons{ 0 };       // bowmindraw with bowcancelkey 0: pad buttons held this poll (X)
    };

    void update(const In& in, Out& out);   // once per pad poll (FarCry5Gesture.cpp)

    bool  on();                       // manualbow
    bool  is_bow(uint32_t sig);       // that weapon id is in bowweapons, or the game's data calls it a bow (bowgame)
    bool  game_bow(uint32_t sig);     // bowgame: the game's weapon data says that weapon id is a bow (class 15)
    bool  game_on();                  // bowgame on
    bool  in_hand();                  // the last poll was active: the bow is in the other hand
    Phase phase();
    float draw_m();                   // the string hand's distance from the nock point at the last poll (m), < 0 none
    float full_m();                   // bowfull
    float start_m();                  // bowstart
    // The nock point (where the arrow sits on the bow) from the bow hand's controller, tracking space.
    glm::vec3 nock_point(const glm::vec3& bow_pos, const glm::mat3& bow_rot, bool mirror);
    // bownock right up back (m, controller axes; default 0 0.05 0): the nock point from the bow hand's controller.
    void get_nock(float v[3]);

    // bowarrow on|off (saved, default on; author 1 Nov 21:24): the arrow drawn over both eyes
    // (FarCry5GestureZones.cpp) from the last poll, tracking space: in the hand along the controller, on the bow along it, on the string
    // from the hand through the nock. No marker at the nock zone (author 6 Nov 14:02; ArrowView's nock fields stay unset).
    struct ArrowView
    {
        bool      arrow{ false };
        glm::vec3 tail{ 0.0f }, tip{ 0.0f };   // the nock end and the point
        bool      nock{ false };
        glm::vec3 nock_pos{ 0.0f };
        float     nock_r{ 0.0f };
        bool      nock_near{ false };          // the arrow is within the nock zone (brighter)
    };
    bool arrow_view(ArrowView& v);   // false: nothing to draw (no arrow, bowarrow off, the bow not in hand)
    bool arrow_shown();              // bowarrow

    // bowarrowmodel on|off (saved, default on; author 1 Nov 21:43): the game's own arrow (FarCry5BowArrow.cpp). Its draws among the bow's weapon-pass
    // draws are learnt at the first shot with each bow (the draws that go away when the arrow leaves the bow; bowarrowmesh, saved). Then:
    // no arrow on the bow until one is nocked (hidden while the quiver hand is empty), the game's arrow carried in the string hand while it
    // is held (its nock end at bowarrowhand from the controller, turned as it sits on the bow), and on the bow the game's own arrow, where the
    // game puts it. The drawn arrow (bowarrow) is left out once the game's is in use.
    bool  arrow_model_on();
    bool  arrow_part(uint32_t sig, uint32_t count);        // that draw (index count) is the game's arrow on that bow
    bool  arrow_known(uint32_t sig);                       // its arrow's draws are known
    std::string arrow_parts_text(uint32_t sig);            // "1234, 567" or ""
    // A learnt arrow (render thread): kept, and the command line that saves the whole list returned (run through the command queue).
    std::string learn_arrow(uint32_t sig, const std::vector<uint32_t>& counts);
    void  set_arrow_model_shown(bool shown);               // the game's arrow stands for the drawn one now (render thread, each frame)
    void  get_arrow_hand(float v[3]);                      // bowarrowhand right up back (m, string hand's controller axes)
    Phase arrow_phase();                                   // phase(), or bowforce's (test)
    uint32_t shots();                                      // arrows loosed this session (Loose -> Empty)
    bool  scanning();                                      // bowscan S (test): each weapon-pass draw of the bow logged once
    bool  arrow_log();                                     // bowlog
    // bowhandstring on|off (saved, default on): while RT is held for the draw the right glove stays where the game holds its string (not moved
    // to the controller); FarCry5Stereo's gloves move reads this.
    bool  hand_on_string();
    // bowpull on|off (saved, default on; author 2 Nov 09:51): the string and the game's arrow follow the string hand, so a pull can stop
    // partway. FarCry5BowArrow.cpp learns each bow's rest and full draw from the game (the bow's skinned draws' bone palettes and the arrow's
    // transform, relative to the bow's main mesh; the full draw at the first one per session) and poses them in between, the arrow's nock end
    // at the hand. While it does, the right hand stays on its controller (hand_on_string is false) and the full-draw buzz comes at the
    // string's full draw.
    bool  pull_on();
    float pull_frac();                          // the hand's pull from where it took the string to bowfull (0..1), while drawing
    bool  rt_held();                            // the game's fire held for the draw
    void  set_pull(bool active, float t);       // render thread, each frame: the string follows the hand; its draw now (0..1, < 0 unknown)
    // The learnt draws are kept in bin\fc5vr_bowpull.txt (written when a bow is first learnt) over bin\fc5vr_bowpull_default.txt (shipped
    // with the mod), so the string follows the hand from the first draw. bowpullforget (not saved): drops yours.
    bool  pull_forget_take();                   // render thread: bowpullforget asked (cleared)
    // bowpin on|off (saved, default on; author 2 Nov 12:26): a small red dot drawn into each eye on
    // the bow's sight pin (its reticle draw's middle; FarCry5BowArrow.cpp bow_pin_note, FarCry5RedDot.cpp reddot_draw).
    // bowpin aim|pin|off (author 2 Nov 13:40):
    // aim (default; "on" in older saves) puts the dot where the arrow lands (the shot line's hit), in each eye; pin keeps it on the pin.
    bool  pin_on();
    int   pin_mode();                           // 0 off, 1 pin, 2 aim
    // bowsightaim on|off (saved, default on; author 2 Nov 15:47 video: drawn, the dot where the arrow lands sat about 10 deg above the bow's
    // sight ring): while drawn the shots go from the head through the bow's sight pin (FarCry5Stereo.cpp update_motion).
    bool  sight_aim();
    // bowsighteye on|off (saved, default on; author 3 Nov 13:34, simulator run research\bowcal_2026-11-03): that line starts at the string
    // hand's eye (right eye for a right-handed archer) instead of between the eyes, so the dot is in the ring's middle for that eye.
    bool  sight_eye();
    // bowdoteye on|off (saved, default on; author 3 Nov 14:38): the bow's red dot drawn in
    // the aiming eye only (FarCry5RedDot.cpp reddot_draw).
    bool  dot_one_eye();
    // bowsteady on|off (saved, default on; author 5 Nov 16:51 video): while the string is drawn the bow's pose (and with it the sight, the dot
    // and the shots) goes through its own steadier, much stronger at rest than the weapon smoothing: bowsteadyhz (saved, default 0.6 Hz, the
    // cutoff while the aim is held), opening only for a deliberate move (past 8 deg/s or 3 cm/s, or once the bow is 2 deg off the hands).
    // Both eyes of a pair get the same bow. After a shot the steadied aim is held bowsteadyhold ms (saved, default 150) so the arrow leaves
    // where the sight was, then the weapon smoothing carries on from there. One [Bow] bowsteady line per draw (steady_stats).
    // Render thread (update_motion), each frame with the bow in the other hand: k the bow's pose as placed (room space, world scale ws),
    // drawn the string drawn (Drawing / Loose), from the weapon smoothing's last pose (where the steadier starts). True: k was steadied (the
    // weapon smoothing is skipped this frame).
    // bowsteadyaim on|off (saved, default on; author 6 Nov video 1:18-1:25 holding the drawn bow): only the aim's own
    // swing (roll left out, averaged at 1.5 Hz, 10 deg/s; catch-up past 3 deg) opens the steadier, the roll follows at 2 Hz at least, and the
    // position is steadied against head (the head in k's space, ref space times world scale; null: in the room as before).
    bool  steady_pose(glm::mat4& k, bool drawn, const glm::mat4& from, float ws, const glm::vec3* head = nullptr);
    bool  aim_held();                 // within bowsteadyhold after the last shot (the shots keep going through the sight)
    // bowsteadytest D (test, not saved, deg): the drawn bow shaken that much before bowsteady (or the weapon smoothing), 30 new readings a
    // second, for the simulator whose controllers do not move. Render thread, before steady_pose.
    void  test_shake(glm::mat4& k, bool drawn);
    // One [Bow] bowsteady line per draw (on or off): new readings, the aim's movement per reading as placed and as drawn, the two eyes of a
    // pair, and with bowsteady the cutoff and how far the aim sat off the hands. Render thread, after the bow's smoothing, each frame with
    // the bow in hand: raw as placed (after test_shake), out as drawn, steadied what steady_pose returned.
    // head (as for steady_pose, may be null) and ws: the bow's position against the head per reading, the sight line's share.
    void  steady_stats(const glm::mat4& raw, const glm::mat4& out, bool drawn, bool steadied, const glm::vec3* head = nullptr, float ws = 1.0f);
    // bownoscope on|off (saved, default on; same video: "There were also random pop up of scopes"): with the manual bow in hand its sight
    // never starts the scope picture (the bow's x3.3 scope entry, learnt from the game's aim down sights, counts as 1x; FarCry5Scope.cpp).
    bool  no_scope();
    // Author 5 Nov 22:47 autonomy list (bow):
    // bowsteadyarrow on|off (saved, default on; the arrow steadied the same way as the bow):
    // with bowpull, how far the string is drawn (the arrow's nock end at the string hand) goes through a steadier like bowsteady's (bowsteadyhz at
    // rest, opening for a deliberate pull), one step per stereo pair (FarCry5BowArrow.cpp bow_pull_draw). Only with bowsteady on.
    bool  steady_arrow();
    float steady_hz();                // bowsteadyhz, clamped
    // bowdotarrow on|off (saved, default on; no red dot with no arrow on the bow): the bow's red dot and the
    // aim marker only while an arrow is on the bow (Nocked, Drawing, Loose, or the aim held after a shot).
    bool  dot_wanted();               // false: no dot or marker with the bow now
    // bowpinsteady on|off (saved, default on; the bow's red dot moved around while drawn,
    // probably the walk animation): bowsightaim's line was the last eye picture's eye through the last drawn pin, one from the pair's other frame: walking,
    // the two came from different game updates (a few cm apart over a 0.7 m line). Now both from this frame: the eye from the head now (plus the
    // aiming eye's offset from it), the pin from the bow now (kept in the bow's own axes, eased at 0.25 Hz so the game's walk sway is left out).
    bool  pin_steady();
    // bowsling on|off (saved, default on; the bow on the sling like the other weapons, taken with the left
    // hand, or with the right hand into the left hand until the left grip is pressed and let go): with the sling on
    // (FarCry5Gesture.cpp) the bow hangs there, its grip at the sling's place. The left grip pressed in the sling zone holds it in the left hand
    // while held; the right grip pressed there (or a shoulder switch to the bow with the grip held) puts it in the left hand until a left grip
    // press is let go. bowslinghold yaw pitch roll (saved, deg, default 0 0 90: rolled so its limbs lie across
    // (about as the game holds it); upright, 0 0 0, its top stood in front of the face, 6 Nov sweep): the bow's further turn on the sling.
    bool  sling_on();
    // bowslinggrab on|off (saved, default on) and bowslinggrabzone m (saved, default 0.12; author 6 Nov 15:59, build AE016CEC): the slung bow lies across (bowslinghold 0 0 90), 97 cm tip to tip, so the part
    // in view is a limb or its cam, 30-45 cm from its grip, outside the 18 cm sling zone round the grip, and the left hand had no tick there.
    // On: a left grip press within the zone of the bow's line (from its grip along its limbs, 45 cm each way, as the weapon pass turns it on the
    // sling) takes it as one in the sling zone does, unless that hand is inside another of its zones; the free left hand ticks coming onto it.
    // The right grip keeps the sling zone only (its chest throwable lies 13 cm from the bow's line).
    bool  sling_grab();
    float sling_grab_zone();
    // bowquivergrip on|off (saved, default on; author 6 Nov 08:23): the weapon hand's shoulder is
    // the quiver only with the bow in hand and the other hand's grip held (FarCry5Gesture.cpp); otherwise it is the shoulder's weapon slot.
    bool  quiver_grip();
    // bowpullkeep on|off (saved, default on; author 6 Nov 14:02 video bow_jitter.mp4, 0:07-0:09):
    // bowpull kept taking the bow's full draw from the game at any still sample 15 frames after RT. Right after RT the game's own draw had not
    // started (or paused), so the full taken was the rest: the arrow's part then moved under 3 mm and was no longer posed, and the game's own
    // animation showed through (the arrow crept back, overshot the hand to the game's full draw) until a still sample at the game's full draw
    // put it back on the hand (snapping forward). On: once a part is known, its full is taken again only while drawing with RT held 90 frames
    // (the game's draw done), and a rigid part's (the arrow's) rest or full only if at least 80 % of its known draw away from the other end.
    bool  pull_keep();
    void  get_sling_hold(float v[3]);
    // bowmindraw F (saved, default 0.80; author 2 Nov 12:03): a string let go short of that share of full draw (the string's own draw with bowpull, else the hand's pull to bowfull) does
    // not shoot: the game's cancel-shot key is tapped (bowcancelkey scan code, saved, default 0x13 = R as his prompt shows; 0 = pad X)
    // with RT still held, RT let go 300 ms later, and the arrow stays on the bow (Nocked).
    // bowstringhand right up back (m, saved, default 0 0 -0.05): the string while drawing, from the string hand's controller.
    void  get_string_hand(float v[3]);
    // bowupright on|off (saved, default on; the author's 2 Nov 08:47 video: the game holds its bow canted onto its side, so with the gun's mapping
    // the bow lay flat across the left fist): the bow turned so its arrow points where the bow hand's controller points and its limbs stand
    // along the controller's up (FarCry5BowArrow.cpp measures both from the game's own bow and arrow). bowhold yaw pitch roll (deg, saved,
    // default 0 0 0): a further turn of the bow in the hand (right, up, roll to the right).
    bool  upright_on();
    // bowuprightkeep on|off (saved, default on; bowsling, 6 Nov builder run: the bow hangs on the sling from the load, before it has ever been in
    // the hand, and the upright turn was only measured in the hand, each session): each bow's measured turn kept in bin\fc5vr_bowupright.txt
    // (one line per bow: "upright <id> <9 numbers>"), read at the first bow frame, so the bow is upright on the sling and in the hand from the
    // load. A measurement this session replaces it (and the file's line once it differs).
    bool  upright_keep();
    // bowuprightwait on|off (saved, default on; author 7 Nov 08:53; his log of 09:50:56: the AR-C ran dry with no reserve, the game switched to bow 51543 by itself and the mod measured the game's
    // arrow 0.9 s later, still in the switch animation: its nock end (8.4, 3.1, 59.0) cm from the grip and 11 deg off straight ahead, against
    // (-3.8, -6.9, 23.7) cm and 0.2 deg at rest, so the up from the arrow to the grip came out reversed (canted 112 deg, not -69) and
    // bowuprightkeep wrote that upside-down turn to fc5vr_bowupright.txt): the arrow's nock end, the limbs and so the upright turn are measured
    // only once the bow has been out 1.5 s (its weapon-pass draws coming without a break), and an arrow pointing more than 4 deg from straight
    // ahead is not taken (looked at again 30 frames later; not counted as one of the 3 tries). Off: as before.
    bool  upright_wait();
    // bowflip on|off (saved, default off): the upright bow turned a further 180 deg about its arrow (a fallback if a bow still shows upside down).
    bool  flip_on();
    void  get_hold(float v[3]);
    // bowhandpos right up back (m, saved, default 0 -0.02 0): the bow and the game's left hand on it moved from the bow hand's controller.
    void  get_hand_pos(float v[3]);

    // bowweapons <id> <id> ... | none (saved): the weapon ids that are bows. manualbow, bowstart m, bowfull m, bownockzone m, bowholdms ms,
    // bowbuzz amplitude, bownock x y z, bowarrow on|off, bowarrowmodel on|off, bowarrowmesh <id>:<count>,<count> ... | none,
    // bowarrowhand x y z, bowhandstring on|off, bowupright on|off, bowuprightkeep on|off, bowuprightwait on|off, bowflip on|off, bowhold y p r, bowpull on|off (saved); bowlog on|off, bowscan S, bowforce empty|arrow|nocked|drawing|off, bowshoot [ms] (tests, not saved).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // F11 Experimental > Manual bow (test): its rows; commands go into cmds. sig: the weapon in the game's hands now (0: none).
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, uint32_t sig);
}
