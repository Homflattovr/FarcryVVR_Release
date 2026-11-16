#pragma once

#include <functional>
#include <iosfwd>
#include <string>

// On-foot buttons (author 11 Nov 2026, then "put the weapon wheel on X, and d-pad on
// left trigger"). Pad input only (VR::on_xinput_get_state in ModConfig.cpp); vehicles, aircraft, the game's menus and the F11 menu unchanged.
//
// stickjump on|off (saved, default on): on foot the right stick pushed up is the game's jump (pad A, as right A) and pulled down its crouch / slide
//   (pad B, what left X sent), held while the stick stays out. A push counts past 0.7 and mostly up or down (|x| under half of |y|); it ends below
//   0.3. A push that first went sideways past 0.5 (a turn, snap or smooth) stays a turn until the stick comes back to the centre, and a jump or
//   crouch push does not turn meanwhile. Never in a vehicle (any seat), the weapon wheel (or LB held this poll), with a stick as the D-pad (left
//   grip or, footbuttons, left trigger held), while the stick zooms a scope (scopezoomstick), on the flat screen, a tutorial pop-up or a load,
//   and a push already out when any of those ends waits for the centre.
// footbuttons on|off (saved, default on): on foot left X held is the weapon wheel (pad LB, what the left trigger sent; X no longer crouches) and the
//   left trigger held is the modifier, as the left grip held: + either stick the D-pad (the right stick then neither turns nor jumps) and, from
//   12 Nov (author), + right B the throw (RB) and + the menu button the map (Back), with the weapon
//   wheel open too. The left grip's combos stay (pressed at the ear, dpadear); the flat screen's toggle (+ left stick click) is the grip's only.
//   A left X or left trigger held when on foot starts or ends sends nothing until let go.
namespace fc5::footpad
{
    bool stick_jump();     // stickjump on
    bool foot_buttons();   // footbuttons on

    struct StickOut
    {
        bool a{ false };            // pad A (jump)
        bool b{ false };            // pad B (crouch / slide)
        bool turn_block{ false };   // a jump or crouch push: the stick's X does not turn
    };
    // Every pad poll (both pollers; same stick, same answer). x, y: the weapon hand's stick (-1..1, up positive); allowed: the gates above.
    StickOut stick_poll(float x, float y, bool allowed);

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // F11 Layout, above the controls tables.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
