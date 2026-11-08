#pragma once
// drive on|off (saved, default off; F11 Experimental > Gesture driving (test); author 5 Nov 17:06). FarCry5Drive.cpp.
//
// While driving (the mod's "driving", settled, motion controls, no menu, flat screen or cinematic) a control sits in front of the player, fixed
// in the recentred reference (the head at the last recentre, level, facing the vehicle's front: the game camera sits there, so a control fixed
// there stays where the vehicle's own wheel is drawn):
//  - wheel (cars, vans, trucks, buggies, boats): a ring drivewheel fwd down radius tilt ahead of and below the head. A grip pressed within
//    drivegrab of the rim holds it; the wheel turns by how far the holding hands go round its centre (both hands: their mean), drivewheellock
//    degrees is full lock (left stick X). Let go, it turns back to straight in drivecentre seconds.
//  - bars (quads, jetskis, motorcycles): a bar drivebars fwd down halfwidth turning about the vertical; a grip near either end (or the bar)
//    holds it, the bar's turn about its centre steers, drivebarslock degrees is full lock.
//  - stick (helicopters, planes): a joystick top drivestick fwd down; a grip near it holds it; the hand's move from where it took the stick is
//    the left stick (forward = stick up), drivestickrange metres full; drivetwist: turning the hand about the vertical is the right stick X
//    (helicopter turn / plane rudder), drivetwistdeg full.
// Either hand alone controls; both together share. A hand holds until its grip is let go, wherever it moves meanwhile (sticky), and that
// grip is not its usual action meanwhile (the left grip does not brake, nor act as the modifier). The left stick still steers too (added).
// The style follows the game's vehicle type (vehscript: 1 boat, 3 motorcycle, 4 helicopter, 5 plane) and, for a vehicle spawned from F11 Debug >
// Vehicle spawn, its archetype (quads and jetskis: bars); drivestyle auto|wheel|bars|stick|off changes it for the vehicle you are in (not saved).
// Triggers (vehtriggers left, the default): the left trigger is the throttle as before; the right trigger shoots (LB) while the right hand is
// free; while the right hand holds a wheel or bars its trigger brakes / reverses (LT; driverightbrake on|off). A right trigger held when the
// hand takes the wheel sends nothing until let go. On the stick the right trigger stays the aircraft's guns.
// driveshow on|off (saved, default on): the control drawn faintly in both eyes (FarCry5GestureZones.cpp), brighter where it is held.
//
// Vehicle controls (author 5 Nov 22:47), F11 Layout > Vehicles, all saved, independent of drive on|off:
//  - drivebuttons on|off (default on): "Change A to accelerate and X to brake and double X press for handbrake for cars and boats. Remove
//    other controls that could accelerate or brake." Driving a car, truck, quad, motorcycle or boat (vehscript type 0, 1, 3): right A is the
//    throttle (RT), left X brakes and reverses while held (LT), a second left X press within drivebuttonsdbl ms of a short first one is the
//    handbrake (the pad's B) while held. The left trigger, the left grip (vehtriggers left), the right grip (vehtriggers right), the right
//    trigger on the wheel (driverightbrake) and the right stick (stickspeed) no longer accelerate or brake there; the right trigger shoots
//    (LB). Right A no longer changes seat while driving those (the pad's A). A button held when the layout starts or ends waits until let go.
//    Helicopters and planes, passengers and type not known yet: as before.
//  - driveplanestick on|off (default off): "Remove right stick camera movement for planes". Off: in a plane the right stick (and the hand's
//    turn on the gesture stick, drivetwist) sends nothing to the game. Helicopters keep theirs (vehstick, helipitch).
//  - drivehands hands|all|off (default hands): "now I want the hands to be visible and grab the joystick/steering wheels (use in game hand
//    position and animations)". Driving or piloting (settled), the game draws the player's arms on the wheel, bars or stick in the world
//    pass, where hidebody hid them with the rest of the body. hands: those draws known from the game's first-person models
//    (FarCry5BoneNames.h: bare arms, gloves and hand wear, outfits) are drawn with every bone that is not the hand or a finger collapsed onto
//    its side's hand, so only the hands show, where and as the game animates them (FarCry5VehHands.cpp). all: the player's whole body as the
//    game draws it (a test). off: hidden, as before.
//  - drivehandsgrab on|off (default on; author 6 Nov 12:46).
//    author 6 Nov 20:58.
//    With drivehands hands, each of those hands is moved as one piece (FarCry5VehHands.cpp) onto its own controller, as on foot: the weapon
//    hand as the game held the recorded sidearm on foot (the holster's recording of that arm), the other as its mirror image; the fingers
//    open, closed by grip and trigger (fingergrip). No weapon is drawn in it (author 6 Nov 20:57; drivehandgun and drivehandreach are gone). With gesture driving on, a hand the drive holds the wheel, bars or stick
//    with sticks to it where it took it and turns (moves) with it until its grip is let go. drivehandturn pitch yaw roll (deg) and
//    drivehandshift x y z (m, the controller's axes) fit the hands to the controllers.
//  - drivehandsdirect on|off (default off, test): the bare arms' indirect draws drawn directly, one instance, while driving.
//  - drivehandsontop on|off (default off, test; author 7 Nov 08:53): the bare arms drawn over
//    everything while driving (their depth test always passes; their depth is still written). If the hands show with it, something nearer
//    than them (drawn or depth only) covers them; if not, they are cut by distance (the log's "[VehHands] ... pass" line says which state).
//  - drivehandsspace auto|object|hair|world|camera|view (default auto, saved; author 7 Nov 12:21): the space the bare arm's palette (an indirect draw) puts its hand in. Patches 6-10 took it to be the draw's own VS b1, and the
//    skin landed far off (the builder's 6 Nov night picture: the left hand in the sky; author 6 Nov 14:59: ahead of the windshield) while the
//    arm hair, an ordinary draw, followed the controllers; since patch 7 the hair takes the skin's move, so neither showed. auto: each drive the
//    skin's palm is set against the hair's hand of the same side in the same frame for each kind, and the kind that puts it there is used
//    (FarCry5VehHands.cpp, space_check); the skin is not drawn for up to 45 frames while it is found. The others force a kind (test).
//  - drivehandsmodel on|off (default on, saved; F11 Layout > Vehicles under the hands; author 7 Nov 21:30). The game still animates its own hands on the vehicle's wheel, bars or stick (drivehands hands) before the
//    mod moves them onto the controllers, and the car's wheel model turns as the game steers (eased, later than the stick:
//    research\2026-11-07_steerlag). On: (1) the drive's wheel, bars or stick is placed where the game's hands say the vehicle's own is (the wheel:
//    its centre, axis and grip radius from a game hand's rigid turn of 30 deg or more, confirmed by a second one; until then the drive's own
//    place), so a grip on the car's wheel takes it; (2) a hand that holds the wheel is turned about the wheel's axis as far as the game turns
//    its wheel model (read from the game hand that holds the rim: in the simulator run of 8 Nov the game's left hand turned with the wheel
//    and the right one stayed put, round 2); a hand on the bars rides the game hand of its side; a hand on the stick rides the game hand on
//    it (the one nearer the drive's stick, within 45 cm). With the game's sidearm out, the other game hand carries the bars. Off: the hand
//    turns with the drive's own wheel, as before.
//    vehwheels (FarCry5VehHands.cpp; author 8 Nov 10:35): each vehicle's learnt wheel kept by its model and seat (bin\fc5vr_vehwheels.txt, and the built-in
//    FarCry5VehWheelTable.h from a simulator pass), so the ring is on that vehicle's own wheel from the moment you sit.
//  - drivequadseat M (default 0.15, saved; 0 = off; author 7 Nov 10:58): the game
//    calls a quad wheeled (type 0) like a car, and only a quad spawned from F11 had its archetype. vehseathold learns where the camera sits
//    on the vehicle's model: the quad's driver on its centre line (11:41: 0.00 m), a car's 0.39-0.46 m to the side. Type 0 with the seat
//    within M of the centre line: handlebars. The seat is waited for up to 3 s.
//  - driveheli on|off, driveheligain G, drivehelicurve C, drivehelismooth S (saved; defaults on, 0.40, 2.0, 0.30 s; F11 Layout > Vehicles):
//    piloting a helicopter, the right stick (turn, and helipitch's pitch) is shaped |x|^C (small pushes do little), scaled to G of the
//    game's full rate, smoothed over S seconds, and sent past the game's own dead zone (0.27 of the virtual pad's travel) so the smallest
//    push already turns slowly. Off: as before.
//  - driveready off|y|lb (default y, saved; author 7 Nov 08:54). Driving a car, quad, motorcycle or boat with the right trigger as the shot (drivebuttons or vehtriggers left), a right grip
//    the gesture drive does not take (pressed away from the wheel or bars, or gesture driving off) readies the sidearm: y taps the pad's Y,
//    the game's "change weapon" in a vehicle, which draws the sidearm without a shot (the author's 29 Oct 10:12 log); while the grip stays held
//    and no weapon is in the hands (the game puts it away about 3 s after the last shot), Y is tapped again at most every 1.5 s. The right
//    trigger shoots (LB) as before, at once with the weapon out. lb taps LB instead (test: the game's shoot, which draws first; it may fire
//    once). A grip at the wheel still holds the wheel. off: the right grip does nothing there, as before.
//  - drivemenu all|vehicle|off and drivemenuhold MS (saved; defaults all, 500; F11 Layout > Vehicles; author 7 Nov 12:02). Everywhere (vehicle: only in a vehicle, any seat), the menu button alone opens the map with a short press (the pad's Back, tapped 120 ms when it is let go before MS)
//    and the pause menu with a long one (the pad's Start from MS on, while held). While the pause menu a long press opened is up, a short
//    press closes it (Start). Left grip + menu stays the map at once (Back while held). The map or a menu opened from a vehicle keeps the
//    vehicle's rule until play is back in 3D. Elsewhere, or off: the menu button is Start, left grip + menu Back, as before. ModConfig.cpp
//    sends it (log "[Pad] drivemenu: ..." per press). (The 29 Oct press / double press, menudouble, was removed at the author's request.)
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <glm/glm.hpp>

namespace fc5::drive
{
    enum Style : int { kNone = 0, kWheel = 1, kBars = 2, kStick = 3 };
    enum Side : int { kRight = 0, kLeft = 1 };   // physical controllers (the pad's triggers are mapped by side, ModConfig.cpp)

    struct Hand
    {
        glm::vec3 pos{ 0.0f };      // tracking space (m, y up)
        glm::mat3 rot{ 1.0f };      // the controller's axes (x right, y up, -z where it points)
        bool      tracked{ false };
        bool      grip{ false };    // raw grip (gestures and the two-handed hold do not apply in a vehicle)
    };

    struct Input
    {
        bool      driving{ false };   // driving, settled, motion controls, no menu, flat screen or cinematic
        glm::mat4 ref{ 1.0f };        // recentred reference -> tracking space (yaw only; origin: the head at the last recentre)
        bool      ref_ok{ false };
        Hand      hand[2];            // kRight, kLeft
        int       veh_type{ -1 };     // vehscript: 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle, 4 helicopter, 5 plane, 6 wingsuit; -1 unknown
        uint64_t  veh_id{ 0 };        // the game's vehicle entity (0 unknown)
        uint64_t  archetype{ 0 };     // spawned from F11 Debug > Vehicle spawn: its archetype (0: not one of those)
        float     seat_side{ 99.0f }; // drivequadseat: vehseathold's seat, the camera's sideways place on the vehicle's model (m; over 50: not learnt yet)
    };

    struct Output
    {
        int   style{ kNone };          // the control in use now (kNone: drive off, not driving, or none for this vehicle)
        bool  held[2]{};               // that hand holds the control
        bool  grip_taken[2]{};         // that hand's grip is the hold's: not its usual action (brake, modifier)
        bool  steer_on{ false };       // wheel, bars: the left stick's X from the control
        float steer{ 0.0f };           // -1 left .. 1 right
        bool  stick_on{ false };       // stick: the left stick from the control
        float sx{ 0.0f }, sy{ 0.0f };  // -1..1 (sy > 0: pushed forward)
        bool  twist_on{ false };       // stick, drivetwist: the right stick's X
        float twist{ 0.0f };
        bool  right_brake{ false };    // the right hand holds a wheel or bars: its trigger brakes (LT) instead of shooting (LB)
        float buzz[2]{};               // haptic pulse on that hand this poll (taking or letting go)
        float dist[2]{ 1e3f, 1e3f };   // drivereadyaway: each hand's distance from the control this poll (m; 1e3: no control, or not tracked)
        float since_let_go[2]{ 1e3f, 1e3f };   // drivereadyaway: s since that hand last let go of the control (1e3: never)
        bool  log{ false };            // drivelog: ModConfig.cpp logs the pad values sent (once a second while in use)
    };

    // Once per pad poll (VR::on_xinput_get_state, both the game's polls and the virtual pad's).
    Output update(const Input& in);

    // What FarCry5GestureZones.cpp draws (tracking space, from the last poll).
    struct View
    {
        int       style{ kNone };
        glm::vec3 centre{ 0.0f };      // wheel: the ring's centre; bars: the bar's centre; stick: its top
        glm::vec3 u{ 1, 0, 0 }, v{ 0, 1, 0 }, n{ 0, 0, 1 };   // wheel: the ring's plane (u right, v up), n towards the driver; bars, stick: ref axes
        float     radius{ 0.0f };      // wheel radius; bars: half width
        float     angle{ 0.0f };       // the wheel's / bar's turn (rad, positive = to the left); stick: unused
        glm::vec3 stick_top{ 0.0f };   // stick: the drawn top (moved with the held hand)
        glm::vec3 base{ 0.0f };        // stick: its base
        bool      held[2]{};
        glm::vec3 hold[2]{};           // where each holding hand is drawn on the control
        float     grab{ 0.1f };
        bool      in_reach[2]{};       // a free hand within reach of the control
    };
    bool view(View& v);
    bool control(View& v);   // the same, whatever driveshow says (drivehandsgrab: the hands stuck to the control)

    bool        on();
    bool        show();

    // drivebuttons: once per pad poll. active: driving a car, quad, motorcycle or boat with drivebuttons on. a: right A, x: left X.
    struct Buttons
    {
        bool  rt{ false };          // right A held: throttle (always RT; author 7 Nov 13:17)
        bool  lt{ false };          // left X held: brake / reverse (the same)
        bool  handbrake{ false };   // left X, second press of a double press, held: the pad's B
    };
    Buttons buttons_poll(bool active, bool a, bool x, int veh_type);
    // driveheli (author 7 Nov 10:59): piloting
    // a helicopter, the right stick's X (turn) and Y (helipitch, already past its dead zone) shaped, scaled and smoothed. rx, ry: -1..1 in and out
    // (out: what the pad sends, the game's own dead zone included). heli false: the smoothing state is dropped, rx and ry untouched.
    void    heli_stick(bool heli, float& rx, float& ry, bool ry_live);
    bool    buttons();             // drivebuttons on
    bool    plane_stick();         // driveplanestick on: the right stick still reaches the game in a plane
    int     hands_mode();          // drivehands: 0 off, 1 hands, 2 all
    struct HandOpts
    {
        bool      grab{ true };        // drivehandsgrab
        glm::vec3 turn{ 0.0f };        // drivehandturn: pitch, yaw, roll (deg) in the controller's axes
        glm::vec3 shift{ 0.0f };       // drivehandshift: x, y, z (m) in the controller's axes (x mirrored for the left hand)
        bool      direct{ false };     // drivehandsdirect: the bare arms' indirect draws drawn directly, one instance (the game's culling skipped; test)
        bool      ontop{ false };      // drivehandsontop: the bare arms drawn over everything (depth test always passes, depth still written; test)
        int       space{ -1 };         // drivehandsspace: -1 auto, 0 object (VS b1), 1 the arm hair's b1, 2 world, 3 camera-relative, 4 view
        bool      model{ true };       // drivehandsmodel: a held hand rides the game's own hand on the vehicle's wheel, bars or stick
    };
    HandOpts hand_opts();
    // drivehandsmodel (FarCry5VehHands.cpp -> here): the vehicle's own control, found from the game's hands on it, in the recentred reference
    // (the drive's own space: m, x right, y up, z back, origin at the head at the last recentre). The drive's wheel, bars or stick is moved
    // there while no hand holds it, so a grip on the car's own wheel takes it.
    struct Model
    {
        int       style{ kNone };              // kWheel: centre, n (towards the driver), radius; kBars: centre, radius = half width; kStick: centre = the grip
        glm::vec3 centre{ 0.0f };
        glm::vec3 n{ 0.0f, 0.0f, 1.0f };
        float     radius{ 0.0f };
        bool      axis_learnt{ false };        // wheel: its axis from the game hand's own turn (always, since round 2)
    };
    void set_model(const Model& m);
    // drivemodelctrl (test): the point on the drive's control (the vehicle's own once found) a testctrl hand of that side takes it at (ref, m).
    bool model_point(int side, glm::vec3& p);
    // drivewheelscan S (test, not saved): for S seconds, the vehicle's draws near the driver logged with their turn in the vehicle's axes
    // (FarCry5VehHands.cpp, veh_wheel_scan), to find the steering wheel's own draw or bone.
    bool wheel_scan_active();
    // driveready (author 7 Nov 08:54): 0 off, 1 the right grip taps the pad's Y (the game's change weapon in a vehicle: the sidearm drawn, no shot),
    // 2 it taps LB instead (test: the game's shoot, which draws first). ModConfig.cpp sends it.
    int      ready_mode();
    // drivereadyaway (patch 15): m from the wheel or bars a right grip's press must start for driveready to draw (and over 1 s after that hand
    // let go of them). drivegunthrottle: 0 off (right A sends no RT while the sidearm is out), 1 stick (test: it pushes the left stick instead).
    float    ready_away();
    int      gun_throttle();
    // drivemenu (author 7 Nov 12:02, 12:15): 0 off, 1 in vehicles, 2 everywhere (default); drivemenuhold: ms that make a press long. ModConfig.cpp sends it.
    int      menu_mode();
    int      menu_hold_ms();
    void    draw_vehicle_menu(std::string& cmds, const std::function<void(const char*)>& hint);   // F11 Layout > Vehicles

    // drivepadecho (test, not saved; builder 5 Nov: during the handbrake the log showed right B and left Y pressed, which nobody pressed): each press
    // the headset runtime reports is logged with what the virtual pad (ViGEm) sent in the 0.6 s before it. A runtime that reads the virtual
    // pad back as its own controllers (the Meta XR Simulator with an Xbox controller as its input) shows its echoes there.
    enum EchoBit : uint32_t { kEchoRA = 1, kEchoLX = 2, kEchoRB = 4, kEchoLY = 8, kEchoRT = 16, kEchoLT = 32, kEchoRG = 64, kEchoLG = 128 };
    void echo_sent(uint16_t buttons, uint8_t lt, uint8_t rt);   // the virtual pad's update: what it hands to ViGEm
    void echo_read(uint32_t pressed);                           // the controllers' presses this poll (EchoBit), from the runtime
    bool        command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    std::string status();   // one line for F11
    void        draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
