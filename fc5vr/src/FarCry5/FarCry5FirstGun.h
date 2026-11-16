#pragma once
// firstguntip: the first time a gun is looted in VR, a popup that pauses the game and says where the weapons are taken from (FarCry5FirstGun.cpp).
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::firstgun
{
    // This frame's state (FarCry5Stereo.cpp, render thread, at the frame's end).
    struct Frame
    {
        bool watch{ false };      // the loot watch may run: headset on, motion controls, no flat screen (menu, map, load), not a scripted test run
        bool play{ false };       // it may show: also on foot, no cinematic or loading popup
        bool flat{ false };       // the flat screen is on (a menu, the map, a loading screen)
        bool menu_open{ false };  // the F11 menu is open
    };

    // firstguntip on|off|show|close (on|off saved; on (default): still to show, off once shown; show / close: now, never saved);
    // firstgunpause on|off (saved, default on): the game paused while it shows.
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Once per frame on the render thread: shows it once play allows after the first gun looted (FarCry5Lua loot watch), closes it, and asks the game's pause and resume.
    void frame(const Frame& f);
    // The popup was shown for the first time and has closed: true once (the caller saves "firstguntip off").
    bool take_shown();
    // The popup shows: none of the player's controller input reaches the game and the aim sends nothing (ModConfig.cpp, FarCry5Stereo.cpp).
    bool showing();
    // The pad's thread, every poll: right A or the right trigger down. True: the pad is held (all zero): while it shows, and after it until the
    // press that closed it is let go. A press that starts at least 2 s after it showed closes it.
    bool hold_input(bool down);
    // The framework's UI frame with the F11 menu closed: draws the popup; true while it shows.
    bool draw();
    // F11 HUD and menus: its checkboxes and button; commands go into cmds.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
