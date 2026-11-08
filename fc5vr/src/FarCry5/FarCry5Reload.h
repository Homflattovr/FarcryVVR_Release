#pragma once
// reloadmanual on|off (saved, default off, experimental; author 2 Nov 08:56). With a weapon of reloadweapons in the game's
// hands (default the AR-C, weapon id 57819, and since 2 Nov 14:01 the pistols with built-in parts: 1911 24417 and 26043, P226 22182, M9
// 29136, D50 28596, 6P9 26187, A99 32325; F11 Weapon > Manual reload ticks others), on foot with motion controls:
//  - a quick press of right B (released within reloadtap ms, default 250) drops the magazine: it leaves the gun and the game's own reload
//    (keyboard R, or pad X with reloadkey x) is tapped once, so the game refills the clip and counts the ammo itself while its animation
//    is kept out of the view (the gun stays in the hand, FarCry5GunStill.cpp). Held longer, B is the game's Y as before (heal); a quick
//    press is no longer "previous weapon" while such a weapon is in hand (the shoulders still switch weapons);
//  - a magazine is always shown in the pouch on the left hip (reloadpouch); the left grip pressed there takes one into the left hand;
//  - brought under the gun's magazine well (the well point, a ball shows it), it engages and follows the hand up into the well, a light
//    tick every centimetre, until it clicks home (reloadtravel, default 5 cm): a firmer buzz on both hands;
//  - when the clip had run dry since the last magazine went in, the charging handle must be racked too: the left grip pressed at the
//    handle (a ball shows it), pulled back reloadrack (default 6 cm) along the gun, ticks on the way, a click at the end;
//  - the gun does not fire while its magazine is out, while the handle needs racking, or until the game's own reload has refilled the clip.
// The state lives on the pad poll (FarCry5Gesture.cpp calls update()); the render thread reads view() for the drawn magazine
// (FarCry5GestureZones.cpp) and the game's own magazine model (FarCry5ReloadMag.cpp).
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <glm/glm.hpp>

namespace fc5::reload
{
    struct In
    {
        bool      listed{ false };      // reloadmanual on and a weapon of reloadweapons in the game's hands (else the state starts again)
        bool      active{ false };      // ... and on foot with motion controls, in the hand (not slung): else the state waits as it is
        uint32_t  sig{ 0 };             // the weapon id (m_weapon_sig)
        glm::vec3 gun_pos{ 0.0f };      // the weapon hand's controller (tracking space, m) and its axes (x right, y up, -z where it points)
        glm::mat3 gun_rot{ 1.0f };
        bool      gun_ok{ false };
        glm::vec3 hand_pos{ 0.0f };     // the other hand's controller and its axes
        glm::mat3 hand_rot{ 1.0f };
        bool      hand_ok{ false };
        bool      grip{ false };        // the other hand's grip now
        bool      pressed{ false };     // ... pressed this poll
        glm::vec3 pouch{ 0.0f };        // the pouch's centre (tracking space): where the left grip takes a magazine
        glm::vec3 pouch_fwd{ 0.0f, 0.0f, -1.0f };   // the body's heading there (level, unit)
        bool      pouch_ok{ false };
        bool      b_down{ false };      // the weapon hand's B (pad Y), a pop-up's Esc press excluded
        bool      b_mod{ false };       // the other hand's grip held raw: B is the throw (left grip + B), not the reload's
        bool      mirror{ false };      // left-handed
        bool      slung{ false };       // a listed weapon in the game's hands hangs on the sling (not active): a quick B is still held back
        bool      ammo_ok{ false };     // clip and reserve read from the game in the last second (FarCry5Lua.cpp)
        int       clip{ -1 };
        int       reserve{ -1 };
    };

    struct Out
    {
        bool     took_press{ false };   // the other hand's grip press went to the reload: no gesture zone gets it
        bool     grip_taken{ false };   // the other hand's grip belongs to the reload now (no two-handed hold, no modifier)
        bool     no_fire{ false };      // the right trigger must not fire
        bool     y_off{ false };        // the pad's Y (right B) held back this poll
        uint16_t key_scan{ 0 };         // a keyboard key (scan code) to tap once now (the game's reload, R)
        uint16_t buttons{ 0 };          // pad buttons to add this poll (the game's reload, X, with reloadkey x)
        float    buzz[2]{};             // by role (gesture::Hand): amplitude, and length (s)
        float    buzz_s[2]{};
    };

    void update(const In& in, Out& out);   // once per pad poll (FarCry5Gesture.cpp)

    bool on();                          // reloadmanual
    bool is_listed(uint32_t sig);       // that weapon id is in reloadweapons
    bool in_hand();                     // the last poll was active
    bool hand_hold_gun();               // reloadhandhold: a gun the game cycles after a shot in the hand (a pump, a lever, a bolt) ...
    bool hand_hold_now();               // ... and that cycle running: the weapon hand held where it was
    bool bolt_parts();                  // reloadboltparts: a bolt action's other bolt parts (BOLT_STOP, BOLT_COVER) kept at rest on the gun too
    // The game's own reload is under way (tapped at the drop, or its own reload after the clip ran dry), so its animation must be kept out
    // of the view: FarCry5GunStill.cpp holds the gun, FarCry5EmptyHands.cpp holds the free hand's turn. Also true while the magazine is out.
    bool hides_game_reload();
    bool game_busy();                   // the game's reload is running now (tapped, or the clip ran dry), not refilled yet
    bool game_pending();
    bool pump_held();                   // a pump shotgun's pump held and pulled by the left hand (manualreload31)                // the game's reload tapped and its clip not refilled yet (manualreload28; may outlast game_busy)
    bool mag_in_left_hand();            // a magazine, shell or round from the hip in the left hand (manualreload38: no two-handed hold then)
    bool button_reload();               // reloadbutton (manualreload48): the game's reload from its button, shown, followed now
    void note_reload_button(bool down); // reloadbutton: the reload button (left Y, the pad's X) as the player holds it (ModConfig.cpp, each poll)
    bool reload_button_pass(bool down); // reloadbuttonguard (manualreload51): left Y as the game gets it (held back during a manual step); notes it too
    bool hides_game_casings();
    bool relearn_armed(uint32_t sig, int item);   // manualreload52: a "Something wrong" button asked to learn this part again (1 the magazine)          // reloadcasinghide (manualreload51): the game's spent casing hidden now (FarCry5Stereo::casing_hide_tick)
    bool keep_one();                    // reloadkeepone (manualreload39, default on): a magazine gun runs dry with one round left in the game's clip
    bool b_heal_only();                 // bheal (manualreload38, default on): right B held heals, never switches weapon
    bool b_heal_y(bool b_down);         // bheal: whether right B is the pad's Y now (held back for reloadtap ms, then held at least bhealms ms)
    void b_heal_throw();                // bheal: this right B press is the throw (left grip + B): no Y
    bool log_on();                      // reloadlog

    // Per weapon (reloadgeom, saved): in the weapon hand's controller axes (x right, y up, z back, metres): the well point (the top of a
    // seated magazine), the well's axis (from the magazine up into the gun, unit) and the charging handle. Measured from the game's own
    // magazine once learnt (FarCry5ReloadMag.cpp), else the AR-C's defaults.
    struct Geom
    {
        glm::vec3 well{ 0.0f, 0.03f, -0.08f };
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
        glm::vec3 handle{ 0.0f, 0.09f, 0.05f };
        bool      measured{ false };    // well and axis from the game's magazine
        bool      handle_set{ false };  // the handle placed by hand (reloadhandlehere): not moved onto the game's handle part
        glm::vec3 back{ 0.0f, 0.0f, 1.0f };   // the gun's back (note_back; not saved)
    };
    Geom geom(uint32_t sig);
    // The render thread's measure of the well from the game's magazine: kept and the whole reloadgeom line returned to be saved.
    // Refused (empty) when the well is further than 0.30 m from the controller (2 Nov test: a wrong learn put it 1.4 m away).
    std::string set_measured(uint32_t sig, const glm::vec3& well, const glm::vec3& axis);
    // The charging handle's grab point onto the game's own handle part (render thread): kept and the reloadgeom line returned, unless set by
    // hand (empty then, or when it moved less than 1 cm).
    std::string set_handle_point(uint32_t sig, const glm::vec3& handle);
    // reloadrackzone (author 5 Nov 14:09: the A99's and MS16's): the handle's parts as a line
    // along the gun's back through their centre, front to rear (weapon controller's axes, as the grab point; render thread; not saved). While
    // the handle must be racked, the left grip takes it within kRackLineZone of that line too, so anywhere on the glowing part.
    void        set_handle_span(uint32_t sig, const glm::vec3& front, const glm::vec3& rear);
    // The gun's back (from the muzzle towards the stock) in the weapon hand's controller axes, from the weapon's calibration (render thread,
    // each frame): the charging handle is pulled along it. Until given: the controller's own back (+z).
    void note_back(uint32_t sig, const glm::vec3& back);
    float travel_m();                   // reloadtravel
    void  get_hand_mag(float v[3]);     // reloadhandmag: the top of a magazine held in the other hand, its controller's axes (m)
    void  get_pouch(float pos[3]);      // reloadpouch right down forward (m, from the eyes, the body's heading; mirrored left-handed)
    bool  pouch_anchor();               // reloadpouchanchor (manualreload49, default on): the hip's magazine goes with the eyes' shift (gun_anchor_apply)
    bool  pouch_neck();                 // reloadpouchneck (manualreload21): the hip's magazine from the neck, on its own heading (pouch_pos)
    float rock_deg(uint32_t sig);       // reloadrockin (manualreload23): an AK's magazine tilted this far at the start of its travel (0: straight in)
    float pouch_zone();                 // reloadpouchzone (m)

    // What is drawn (the last poll). The overlay composes these with the controllers' poses of the frame it draws.
    struct View
    {
        bool      active{ false };
        uint32_t  sig{ 0 };
        bool      pose_ok{ false };        // the controllers as the pad poll read them (tracking space; with testctrl the simulated ones), ...
        glm::vec3 gun_pos{ 0.0f }, hand_pos{ 0.0f };
        glm::mat3 gun_rot{ 1.0f }, hand_rot{ 1.0f };   // ... for drawing the zones where the reload sees them in tests (manualreload11)
        bool      pouch_mag{ false };      // a magazine in the pouch (none once the reserve is known empty)
        glm::vec3 pouch{ 0.0f };           // the pouch's centre (tracking space)
        glm::vec3 pouch_fwd{ 0.0f, 0.0f, -1.0f };
        float     pouch_r{ 0.0f };
        bool      pouch_near{ false };     // the free hand in the pouch zone
        bool      hand_mag{ false };       // a magazine in the other hand (not engaged)
        bool      mag_in_gun{ true };      // the gun's magazine seated
        bool      engaged{ false };        // the hand's magazine in the well, sliding (insert 0..1)
        float     insert{ 0.0f };
        bool      well_ball{ false };      // show the well point (a magazine in the hand)
        bool      well_near{ false };      // the magazine close enough to engage
        bool      rack_ball{ false };      // show the charging handle (it must be racked, it is held, or the free hand is near it)
        bool      racking{ false };        // held and pulled (rack 0..1)
        float     rack{ 0.0f };
        float     travel{ 0.05f };         // reloadtravel (m)
        float     rack_len{ 0.06f };       // reloadrack (m)
        Geom      g{};
        glm::vec3 hand_mag_off{ 0.0f };    // the held magazine's top in the other controller's axes
        glm::mat3 hand_mag_turn{ 1.0f };   // reloadhandgame: the held magazine turned from how it sits on the gun (the other controller's axes for the gun's)
        bool      hand_mag_game{ false };  // ... placed by its centre, hand_mag_c in the other controller's axes (m)
        glm::vec3 hand_mag_c{ 0.0f };
        float     slide_lock{ 0.0f };      // reloadhandgame: an empty handgun's slide held back this far until it is racked (m; 0: not)
        bool      tube{ false };           // a pump shotgun (tube_fed): hand_mag/engaged are a shell; the handle is the pump, held at rest
                                           // when not pulled (the game's own pump after a shot not shown: it is worked by hand)
        int       tube_n{ -1 };            // ... the shells loaded by hand in its tube (-1: not read yet)
        int       chamber{ 1 };            // ... its chamber: 0 empty, 1 a live shell, 2 a fired one
        float     lever_f{ 0.0f };         // ... a lever action's lever thrown (0 home .. 1 fully down; lever_parts; manualreload30)
        bool      bolt{ false };           // a bolt action (reloadbolt): its bolt (the handle part) drawn at rest unless the hand works it (reloadboltpin)
        float     bolt_up{ 0.0f };         // reloadboltturn: the bolt turned up (0 locked down .. 1 fully up) ...
        float     bolt_deg{ 60.0f };       // ... by this much at 1 (deg, about the gun's back through the bolt; mirrored left-handed)
        bool      bolt_open{ false };      // ... out of its lock (held, or let go turned up or back)
        bool      revolver{ false };       // a revolver (revolver_fed): hand_mag/engaged are a round going into chamber `target`
        float     open_f{ 0.0f };          // ... its cylinder swung out (0 closed .. 1 open)
        int       cyl[6]{};                // ... each chamber: 0 empty, 1 a live round, 2 a fired case
        int       target{ -1 };            // ... the chamber the next round goes into (the first empty one; -1: none)
        uint32_t  dumps{ 0 };              // ... the fired cases dropped out of the open cylinder (a count) ...
        uint32_t  dump_mask{ 0 };          // ... and which chambers the last drop emptied
        bool      belt{ false };           // a belt-fed gun (belt_fed, manualreload12): the magazine is its belt; open_f its feed cover lifted
        // reloadhighlight (manualreload40): the gun's part to work next glows (FarCry5GestureZones.cpp): the empty magazine (drop it), the
        // well (a magazine in the hand), the charging handle, slide or pump (rack it).
        bool      hl_mag{ false }, hl_well{ false }, hl_handle{ false };
        // reloadhintglow2 (author 5 Nov 12:47: no glow): reloadhintstyle glow only
        // (no box): a revolver's cylinder (run dry: open it; open: empty it, load it, close it; not while a round is in the hand), a lever
        // action's lever while its chamber needs it.
        bool      hl_cyl{ false }, hl_lever{ false };
        // reloadcoverpush (author 5 Nov 22:47: the M60's lid): a belt-fed gun's open feed cover over a new belt (push it
        // down to shut it), and whether the left hand holds it (brighter).
        bool      hl_cover{ false }, cover_held{ false };
        // reloadhlwhen (author 6 Nov 14:10): the highlights drawn at all (the hint glow, the boxes, the handle's blue ball): the gun run dry or a
        // reload begun, or an action needed before it fires (reloadhlaction: a bolt, pump or lever after a shot, a handle to rack); always: true.
        // The hl_ fields above stay as before.
        bool      hl_show{ true };
        // reloadhiploaded (author 6 Nov 12:40): a one-shot launcher's spare round shown at the hip with the launcher loaded too.
        bool      hip_loaded{ false };
        // wristammo (FarCry5WristAmmo.cpp; author 5 Nov 11:51): the rounds as this reload counts them (the count by hand, not the game's clip).
        int       rounds{ -1 };            // what the gun fires before its next reload: the magazine seated (the game's clip less rounds_keep;
                                           // 0 while it is out), the shells in the tube and the chamber, the live rounds in the cylinder (-1: not known)
        int       rounds_cap{ -1 };        // the tube's or the cylinder's size (-1: a magazine gun, whose size is the game's clip size)
        int       rounds_aside{ 0 };       // rounds of the game's clip not in the gun by hand (a magazine out, shells its reload put in ahead of the hand)
        int       rounds_keep{ 0 };        // the round reloadkeepone keeps in a magazine gun's clip (never fired, so not counted)
        bool      fire_held{ false };      // the gun does not fire now (Out::no_fire)
    };
    bool view(View& v);                 // false: nothing to draw
    // reloadhighlight on|off (manualreload40; saved, default on; author 3 Nov 12:39): a box round each part, measured on the game's model (FarCry5ReloadMag.cpp) once per weapon
    // and session, in the weapon controller's axes as Geom (m; x mirrored for the left hand as Geom is): its centre and half-axis vectors.
    struct HlBox
    {
        bool      ok{ false };
        glm::vec3 c{ 0.0f };
        glm::vec3 a[3]{};
    };
    enum { kHlMag = 0, kHlWell = 1, kHlHandle = 2, kHlCount = 3 };
    bool highlight_on();
    // reloadhintstyle glow|boxes (saved, default glow; author 5 Nov 08:28): with reloadhighlight on, the part glows on the game's own model (FarCry5ReloadHint.cpp); boxes: the boxes and balls as before. A part
    // the glow cannot draw keeps its box.
    bool hint_glow();
    bool hand_snap_on();                // reloadhandsnap (manualreload41, default on): the drawn free hand on the part it works
    // reloadrec on|off (reloadmatch; test, not saved, default off; F11 Manual reload > Sounds and tests; author 4 Nov): every frame of the game's own reload (G rows) and of the manual steps (V rows) in
    // bin\fc5vr_reloadrec.csv, in the gun controller's axes: the hand, the palm, the magazine, the slide, the finger bends; the game's sounds (S rows).
    bool rec_on();
    // reloadgamehand on|off (reloadmatch; test, not saved, default off): while the game's reload from its button runs (reloadforce button), its left hand is
    // left where the game's animation has it on the gun, with its own fingers, so the reference pictures show the flat game's hand.
    bool game_hand_now();
    bool hand_grab_on();                // reloadhandgrab (manualreload44, default on): ... at one place on the handle per weapon
    // reloadhandgame on|off (reloadmatch; saved, default on; author 4 Nov): a handgun's magazine in the hand and its hand on the slide as the game's own reload has them (FarCry5ReloadMatch.h, recorded with
    // reloadrec; tools\reloadmatch). False for a weapon with no row, or with the switch off.
    struct HandGame
    {
        bool      mag{ false };
        glm::vec3 mag_c{ 0.0f };       // the magazine's centre in the left controller's raw axes (m)
        glm::mat3 mag_turn{ 1.0f };    // its turn from how it sits on the gun, the left controller for the gun
        bool      grab{ false };
        glm::vec3 grab_c{ 0.0f };      // the left controller on the slide, in the gun controller's raw axes (m)
        glm::mat3 grab_turn{ 1.0f };   // ... and its turn against them
        float     slide_lock{ 0.0f };  // how far the slide stays back when the gun is empty (m)
    };
    bool hand_game(uint32_t sig, HandGame& h);
    bool mag_lower(uint32_t sig);       // reloadmaglower: this weapon's magazine held in the fist round its lower half, not as the game's hand holds it
    bool round_pinch_on();              // reloadroundpinch (manualreload46, default on): a round or shell pinched by the thumb and index
    // reloadhandgrabw (manualreload44): the left controller's place at rest on the charging handle, slide or pump (the gun controller's axes,
    // right-handed, m). set_grab_hold keeps a learnt one (within 12 cm of the handle point) and returns the whole line to save; grab_hold gives
    // the learnt (1), built-in (2) or made from the handle point (3) one.
    std::string set_grab_hold(uint32_t sig, const glm::vec3& c, const glm::vec3& handle);
    int         grab_hold(uint32_t sig, bool pump, const glm::vec3& handle, glm::vec3& c);
    void set_highlight(uint32_t sig, int which, const HlBox& b);
    bool highlight(uint32_t sig, int which, HlBox& b);
    // reloadmagmodel (FarCry5ReloadMag.cpp): the game's own magazine is drawn in the hand and the well instead of the drawn one.
    bool model_on();
    bool rack_needed();                 // the clip ran dry: the handle must be racked after the next magazine
    // The charging handle can always be taken and pulled (author 2 Nov 12:32): the pull moves the game's
    // handle; it chambers a round only after a fresh magazine went in once the clip had run dry.
    // reloaddrop on|off (saved, default on; author 2 Nov 11:58): the dropped magazine falls from the gun, tumbling, to the ground (reloaddropfloor m below the game's eyes, default
    // 1.6) and lies there 20 s (FarCry5ReloadMag.cpp).
    bool  drop_fall_on();
    float drop_floor();
    // A pull of the charging handle to its stop with a round in the chamber ejects it (author 2 Nov 13:53): counted here; FarCry5ReloadMag.cpp draws the weapon's round (round_part) flying out of the ejection port.
    uint32_t ejects();
    void     round_landed();   // an ejected round reached the ground (its sound)
    bool     round_part(uint32_t sig, uint32_t& count, uint32_t& bone, uint32_t& port_bone);
    // The casing port in the gun's model space (m: x right, y forward, z up), from the game's FX_CASING node (the engine internals thread's
    // research\weapondata\weapon_parts.csv): the ejected round starts there. False: beside the port bone instead.
    bool     casing_port(uint32_t sig, glm::vec3& p);   // the AR-C: draw 3036 bone 0, the port at the bolt (57819/6); a pistol: at its slide
    void set_model_shown(bool shown);   // the game's magazine stands for the drawn one in the hand now (render thread, each frame)
    bool model_shown();
    void set_pouch_shown(bool shown);   // the game's magazine is drawn at the hip too (a second draw): no drawn one there
    bool pouch_shown();
    void set_handle_shown(bool shown);  // the game's charging handle moves with the pull: no drawn bar
    bool handle_shown();
    // The game's magazine model per weapon (reloadmagmesh, saved): the draws (index counts) and, for a skinned one, the bones that are the
    // magazine; "p" a prop the game shows only during its reload (hidden). Learnt at the first drop with each weapon (FarCry5ReloadMag.cpp).
    struct MagPart
    {
        uint32_t count{ 0 };
        bool     rigid{ false };
        bool     prop{ false };
        uint32_t bones[16]{};   // up to 4 learnt or saved; a built-in belt's rounds up to 16 (manualreload12)
        int      nbones{ 0 };
    };
    // A weapon's parts have bone numbers up to this (its skeleton is small); a draw with higher ones is the player's arms or body, never the
    // magazine (2 Nov test: the arms' bones 19 and 31 were learnt as the AR-C's magazine).
    constexpr uint32_t kMaxWeaponBone = 24;
    constexpr uint32_t kRigidBone     = 0xFFFFu;   // a rigid draw's "bone" (handle_part)
    // The AR-C's magazine is known: bone 1 of its main mesh (57819 indices; 1400 vertices, as many as the game's spare magazine, draw 5106,
    // the prop its left hand brings in during its reload) and its top round (draw 3036, BULLET). The pistols' and some SMGs' and rifles' too,
    // from the bone map (kBuiltin in FarCry5Reload.cpp). Other weapons: learnt at their first game reload.
    bool        mag_parts(uint32_t sig, MagPart* out, int max, int& n);   // false: neither learnt nor known
    bool        mag_learnt(uint32_t sig);                                 // learnt (reloadmagmesh), not only the built-in default
    std::string learn_mag(uint32_t sig, const MagPart* parts, int n);     // kept; the whole reloadmagmesh line returned to be saved
    // reloadsoundlearn (manualreload33): each weapon's reload sounds learnt from the game's reload (FarCry5ReloadMag.cpp). ids: the magazine out, in, the
    // handle back, forward (a game sound id, or 0 or -1: unchanged). Kept; the whole reloadsoundlearnt line returned to be saved.
    bool        sound_learn_on();
    std::string learn_sounds(uint32_t sig, const int64_t ids[4]);
    int         sound_spread(uint32_t id);   // how many weapons of the arcade map's sound run play it in their reload
    // reloadgamehold (manualreload17; author 2 Nov 19:38):
    // where the game's own left hand holds each weapon's spare magazine in its reload, as reloadhandmag (the magazine's top in the left
    // controller's axes, right-handed, m), learnt by FarCry5ReloadMag.cpp. Kept and the whole reloadhandmagw line returned to be saved.
    std::string set_hold_mag(uint32_t sig, const glm::vec3& top);
    bool        hold_mag(uint32_t sig, glm::vec3& top);   // learnt for this weapon
    bool        game_hold();                              // reloadgamehold on: the learnt places used
    bool        hold_plausible(const glm::vec3& top);     // inside the box every good learnt hold fell in (manualreload28)
    // The game's charging handle part per weapon (reloadhandlepart, saved): a draw (index count) and its bone (kRigidBone: the whole draw).
    // Found by its motion in the game's reload from empty, else the weapon part nearest the handle point (FarCry5ReloadMag.cpp).
    bool        handle_part(uint32_t sig, uint32_t& count, uint32_t& bone, bool& learnt);
    // The parts that move with the pull and where the hand takes it (2 Nov 12:32, author; the bone map thread: the AR-C's handle is 57819/7 CHARGING_HANDLE, with its tabs
    // 8 and 9, the bolt 6 behind the chamber). A saved reloadhandlepart (one part, the grab point its centre), else built in: the AR-C's
    // handle 7, its tabs 8 and 9 and the bolt 6 move; the hand takes it at its tabs.
    struct HandleParts
    {
        uint32_t count{ 0 };
        uint32_t bones[4]{};
        int      nbones{ 0 };
        uint32_t grab[2]{};    // the parts whose centre is the grab point (the blue ball) ...
        int      ngrab{ 0 };
        bool     grab_rear{ false };   // ... or the rear 2.5 cm of them (a pistol's slide: where the fingers pinch it)
        bool     grab_right{ false };  // ... or their outermost 1.2 cm on the gun's right (an AK's bolt carrier: its knob; reloadhintglow2)
        uint32_t also_count{ 0 };      // another draw that moves with the pull (a pistol's slide sights; 0: none) ...
        uint32_t also_bone{ 0 };       // ... its bone
        bool     slide{ false };       // a pistol's slide
        bool     saved{ false };
    };
    bool        handle_parts(uint32_t sig, HandleParts& h);   // false: neither saved nor built in
    bool        mag_builtin(uint32_t sig);                    // the magazine is built in (the AR-C, the pistols...): not learnt from the game's reload
    // A pistol's magazine sits in its grip, tilted from the gun's up: its well's axis is measured along the magazine's own length (its
    // vertices' main axis), not taken as the gun's up (the AR-C's).
    bool        mag_own_axis(uint32_t sig);
    // A pump shotgun (manualreload9; author 2 Nov 15:15): the M133s (29259,
    // 34497, 20586, 35499, 29841) and the SPAS-12 (42549). No magazine and no drop (quick B does nothing): a shell at a time from the left
    // hip, pushed up into the loading port under the receiver (0.6 x reloadtravel), a click per shell. The pump is the handle: the left
    // grip takes it within 12 cm (the press stays the two-handed hold's too), pulled back 1.3 x reloadrack to its stop ejects the shell in
    // the chamber, pushed forward chambers the next from the tube. After a shot the pump must be worked (reloadpumpshot on, default) before
    // the gun fires again. The game's clip is refilled by its own reload, tapped and hidden when a shell goes in and the clip has fewer.
    bool        tube_fed(uint32_t sig);
    // A revolver (manualreload10): the M629 (.44, 18912 and its other model 9540). Quick right B swings the cylinder out (the crane turns
    // 80 deg to the left about its hinge) and, again, back in. Open, the muzzle tipped up (more than 37 deg) drops the fired cases out of it.
    // Rounds come one at a time from the left hip and slide 0.7 x reloadtravel into the next empty chamber, a click each. The gun fires
    // only closed, with a live round. As the shotguns, the game's clip is refilled by its own reload, tapped and hidden.
    struct Revolver
    {
        uint32_t  main{ 0 };          // the gun's main draw (its id)
        uint32_t  swing[6]{};         // its parts that swing out: CRANE, DRUM, EXTRACTOR (the M79: its barrel, extractor and leaf sight)
        int       nswing{ 0 };
        uint32_t  rounds{ 0 };        // the rounds' draw: a slot per chamber (BULLET_01..06) and CAP
        uint32_t  round_bones[6]{};
        uint32_t  cap_bone{ 0 };
        glm::vec3 pivot{ 0.0f };      // the crane's hinge (model space m: x right, y forward, z up), along the barrel (+y)
        float     open_deg{ -80.0f };
        // manualreload29: a break-action shotgun is a revolver of two chambers whose barrels tip down about the gun's right axis.
        int       nround{ 6 };                 // its chambers (round_bones used)
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };    // the hinge's axis (model space): the barrel (a revolver's crane), or x (a break action)
        bool      brk{ false };                // a break action: the fired shells thrown out by its ejector as it opens (no muzzle up)
        // allreload: one-shot launchers. The M79 is a break action of one chamber; the RPG-7 has nothing to open and takes its rocket at
        // the muzzle. Their round is a rigid draw (the 40 mm grenade, the rocket).
        bool      rigid_rounds{ false };       // the rounds' draw is rigid: one round, the whole draw (its parts' key is kRigidBone)
        bool      noopen{ false };             // nothing opens (the RPG-7): always ready to load, a quick right B does nothing
        bool      muzzle{ false };             // loaded at the muzzle: the round goes in backward, its tail first (the lead point is its tail)
        bool      spent_gone{ false };         // a shot leaves nothing in the chamber (a rocket flies off with its case)
        bool      door{ false };               // reloadratdoor: loaded from behind through a part that opens (the swing parts); the round itself does not move with them
        float     travel{ 0.0f };              // how far the round slides in (m; 0: a revolver's 0.7 x reloadtravel)
        float     busy_s{ 0.0f };              // the game's own reload at empty kept out of the view this long (s; 0: the usual 3.2)
    };
    // The round's lead point (its nose; a muzzle-loaded rocket's tail) from its centre, measured on the model (render thread, allreload):
    // the left hand holds a rocket by its middle, so the pad poll needs it to know where the tail is. 0: not measured.
    void        set_round_lead(uint32_t sig, float m);
    void        set_door_pivot(uint32_t sig, const glm::vec3& p);   // reloadratdoor: its hinge, from the model
    // reloadsamegun (author 6 Nov 12:40): the weapon id the reload goes by (the arcade map's RAT4 69646 is 80077, the same draws).
    uint32_t    reload_sig(uint32_t sig);
    bool        grenade_seat();                                     // reloadgrenadeseat: the M79's grenade moved onto its bore (ReloadMag)
    bool        shell_hip();                                        // reloadshellhip: a double barrel's hip shell centred on the pouch zone (ReloadMag)
    bool        bolt_fed(uint32_t sig);                             // reloadboltpin: worked by hand after every shot
    // reloadmuzzlezone (author 5 Nov 15:51: the RPG's zone): a muzzle-loaded launcher's depth from its muzzle to
    // the seated round's tail, measured on the seated rocket (render thread): its tail goes in at the muzzle, so this is its travel. 0: not measured.
    void        set_muzzle_depth(uint32_t sig, float m);
    // A belt-fed gun's feed cover (manualreload12): it lifts about its hinge (model space, x right, y forward, z up), turning about the gun's
    // right axis, while open (quick right B opens and closes it; the belt goes in or comes out only while it is open).
    struct Belt
    {
        uint32_t  main{ 0 };
        uint32_t  cover{ 0 };          // the cover's slot in the main draw (UPPER_SHELL)
        glm::vec3 pivot{ 0.0f };
        float     open_deg{ 0.0f };    // reloadcoverdeg (saved): its lift, the sign which way
    };
    bool        belt_fed(uint32_t sig);
    bool        belt_parts(uint32_t sig, Belt& b);
    bool        revolver_fed(uint32_t sig);
    bool        lever_parts(uint32_t sig, glm::vec3& pivot, float& deg);   // a lever action (manualreload30): its lever's hinge (model space) and throw
    bool        revolver_parts(uint32_t sig, Revolver& r);
    std::string set_handle_part(uint32_t sig, uint32_t count, uint32_t bone);   // kept; the whole reloadhandlepart line returned
    int         take_handle_next();     // F11 "Try the next part as the charging handle" presses since the last call (render thread)
    std::string mag_text(uint32_t sig);
    uint32_t    drops();            // magazines dropped this session (a new learning window starts at each)
    int64_t     drop_ms();          // the last drop (steady clock, ms)
    bool        scanning();         // reloadscan S (test): each weapon-pass draw logged once with its moving bones
    std::string take_save_line();   // once: a reloadgeom line from reloadwellhere / reloadhandlehere to run through the command queue (saved)

    // reloadmanual on|off, reloadweapons <id> ... | none, reloadkey r|x, reloadtap ms, reloadpouch x y z, reloadpouchzone m, reloadhandmag x y z,
    // reloadgeom <id>:<9 values> ... | none, reloadtravel m, reloadrack m, reloadmagmodel on|off, reloadmagmesh ... | none (saved);
    // reloadhandlepart <id>:<count>/<bone|r> ... | none, reloaddrop on|off, reloaddropfloor m, reloadsound on|off, reloadsoundvol F (saved);
    // reloadwellhere, reloadhandlehere (set from the hands now, saved through reloadgeom), reloadlog on|off, reloadscan S,
    // reloadforce dry|out|full|take|geom (tests: take = a magazine in the left hand, geom = a [Reload] test geometry line),
    // reloadpumpshot on|off (saved, default on: a pump shotgun's pump is worked after each shot).
    // reloadweaponsounds on|off (saved, default off): each weapon's own game reload sounds (built in from the arcade map's sound run);
    // reloadsoundpick <id>:<out>,<in>,<back>,<fwd> ... | none (saved; - built in, 0 the mod's own, or a game sound id);
    // reloadsoundtry <id>|mag_out|mag_in|rack_back|rack_fwd (test: play a game sound id, or the weapon in hand's sound for that event).
    // reloadgamehold on|off (saved, default off): a magazine in the left hand sits where the game's own hand holds that weapon's spare in its
    // reload (learnt; else reloadhandmag); reloadhandmagw <id>:x,y,z ... | none (saved, the whole list; written by the learning).
    // With a revolver, reloadforce out swings its cylinder out or back in (as a quick B), and dry makes every chamber hold a fired case.
    // allreload: reloadflame, reloadgl, reloadrocket, reloadlever, reloadbreak, reloadbelt, reloadbolt on|off (saved, default on): every
    // built-in weapon of that kind reloads by hand, ticked in reloadweapons or not (the flamethrower, the M79, the RPG-7, the lever actions,
    // the double barrels, the M60 and M249, the .308's bolt after each shot); manualreload47: reloadmag, reloadpump, reloadrevolver the same
    // for the magazine guns, pump shotguns and revolvers.
    // allreload2: reloadother on|off (saved, default on): every other gun (no built-in row: the M712, Mark IV, Lost on Mars weapons, the
    // arcade map's unnamed ids) reloads by hand as a magazine gun, its magazine learnt from the game's reload. The P08, MG42, classic RPG-7 and
    // RAT4 have rows (reloadmag, reloadbelt, reloadrocket).
    // reloadnostuck on|off (allreloadfire; saved, default on): a weapon held from firing as empty while the game's clip has rounds is loaded
    // from that clip, at once when nothing can go in by hand (its well never measured), else after 6 s with nothing done towards the reload.
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // F11 Weapon > Manual reload (experimental): its rows, in subsections; commands go into cmds. sig: the weapon in the game's hands now (0: none).
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, uint32_t sig);
}
