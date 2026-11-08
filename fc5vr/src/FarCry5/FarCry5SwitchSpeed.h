#pragma once
// switchfast: the game's own weapon draw and holster animations made shorter (FarCry5SwitchSpeed.cpp).
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::switchspeed
{
    // switchfast on|off, switchdraw S, switchholster S, switchrescan S (saved); switchscan, switchlist N (tests).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Once per frame on the render thread: starts the first scan, keeps the written values and rescans when due (no memory work here).
    void frame();
    // A weapon switch's time from the gesture to the new weapon drawn (FarCry5WeaponSwitch.cpp): a slow one asks for a rescan.
    void note_switch(float secs);
    // The load popup (FarCry5LoadPopup.cpp): 2 = a scan running (secs since it started), 1 = one due after a load, a slow switch or
    // a request (secs until it may start), 3 = one due only because no weapon was found yet, 0 = none; took = the latest scan's time.
    int scan_state(float& secs, float& took);
    // reloadfullmag (FarCry5Reload.cpp): on: reloadfullmag and the manual reload on; obj: the weapon object of a magazine gun in hand
    // (FarCry5Lua.cpp weapon_object), 0 for none. The worker finds its block and writes bAutoReload 0 within about 250 ms.
    void autoreload_want(bool on, uintptr_t obj);
    // That weapon object's block holds bAutoReload 0 (read back after the write): its clip may run to 0 with no reload of the game's.
    bool autoreload_off(uintptr_t obj);
    // The game reloaded by itself at an empty clip anyway: every block put back, not used again this session.
    void autoreload_failed();
    // F11 Experimental > Weapon switch (test): its rows; commands go into cmds.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
