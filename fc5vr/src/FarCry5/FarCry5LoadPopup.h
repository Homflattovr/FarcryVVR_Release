#pragma once
// loadpopup: "VR mod loading" in the headset after a load, the controllers held back until the mod's load-time work is done (FarCry5LoadPopup.cpp).
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::loadpopup
{
    // This frame's state of what the mod does after a load (FarCry5Stereo.cpp, render thread).
    struct Steps
    {
        bool allowed{ false };          // on foot with motion controls, no menu, flat screen or cinematic (the gestures' own test)
        bool armed{ false };            // a weapon drawn in hand this frame (not bare hands)
        bool weapon_recorded{ false };  // the weapon in hand has been recorded (its copy can be drawn on the sling or the hip)
        bool side_recorded{ false };    // the sidearm has been recorded this session
        bool  flat{ false };            // the flat screen is on (a menu, the map, a loading screen, a flat cinematic)
        bool  cine{ false };            // a cinematic (in 3D or flat)
        bool  black{ false };           // an eye image the headset got was black (reported this frame)
        float cam[3]{};                 // the world camera (where play stands)
    };

    // loadpopup on|off (saved, default on; the launcher's "Skip the loading popup" writes off); loadpopupsettle S (saved, default 2, 0-10 s).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Once per frame on the render thread, menus included: decides when the popup shows (back in play somewhere else, once play has settled).
    void frame(const Steps& s);
    // The popup is up, or waits for play to settle: the player's controller input is not sent to the game (ModConfig.cpp, the pad's thread).
    bool blocking();
    // The framework's UI frame with the F11 menu closed: draws the popup; true while it shows (its layer is then in the headset).
    bool draw();
    // F11 HUD and menus: its checkbox; commands go into cmds.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
