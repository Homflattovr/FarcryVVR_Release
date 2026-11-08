#pragma once
// wristammo on|off (saved, default on; author 5 Nov 11:51): three numbers, "12/30/90", drawn into both
// eye pictures just under the weapon hand's wrist, facing the eyes (FarCry5WristAmmo.cpp, wrist_ammo_draw).
//  - First number: the rounds the gun fires before its next reload. With the manual reload on the weapon in hand (FarCry5Reload.cpp), the
//    count by hand, as the reload thread asked (5 Nov): the magazine seated (the game's clip, less the round reloadkeepone keeps there; 0 while
//    the magazine is out), the shells in the tube and the chamber, the live rounds in the cylinder. Without it: the game's clip.
//  - Second: what the gun holds when full (a magazine gun: the game's clip size, vtable +0x48, less that kept round; the tube's or the
//    cylinder's size by hand).
//  - Third: the rounds left to load: the game's reserve, plus those of the game's clip not in the gun by hand (a magazine in the hand, shells
//    its own reload put in ahead of the hand), so the total does not jump when a reload moves rounds between the two.
//  - Colours: the first number white; amber while the gun does not fire now (a handle or bolt to work, the cylinder open, the magazine out);
//    red at 0. The other two grey, the third red at 0.
//  - wristammopos right up back (m, the weapon controller's axes, mirrored left-handed; default 0 -0.05 0.10): where it sits;
//    wristammosize m (the text's height, default 0.018).
//  - wristammostyle smooth|pixel (saved, default smooth; author 5 Nov 13:06): smooth digits drawn from strokes on a see-through dark panel with an orange bar, the first number larger; pixel: the 8 x 14 font.
//  - Hidden with no weapon in hand (or none with a count), slung, in menus and cinematics. The game's clip, size and reserve are read every
//    100 ms on the script thread (FarCry5Lua.cpp, ammo_query), on while this is on or reloadmanual is.
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::wristammo
{
    struct Opts
    {
        bool  on{ true };
        float pos[3]{ 0.0f, -0.05f, 0.10f };   // right, up, back (m)
        float size{ 0.018f };                   // text height (m)
        bool  smooth{ true };                   // wristammostyle smooth|pixel
    };
    Opts opts();
    bool on();
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // F11 HUD and menus > Ammo on the wrist.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
