#pragma once
// gameweapon (FarCry5GameWeapon.cpp): which of the game's weapons each of the mod's weapon ids (the largest mesh of the weapon pass) is,
// from the item the game says the player holds (FarCry5Lua equipped_item) and the game's own weapon data (FarCry5WeaponTable.h).
#include "FarCry5WeaponTable.h"

#include <cstdint>
#include <string>

namespace fc5::gameweapon
{
    // Each frame from the holster's frame end: the frame's weapon id and whether its draws are a throwable's (then nothing is learned).
    void frame(uint32_t sig, bool quiet);
    // What the game's data says about that weapon id: nullptr when not learned yet or not in the table.
    const fc5::weapons::Info* info(uint32_t sig);
    // The game's item seen for that weapon id, 0 none yet (gamedrawgrab).
    uint64_t item(uint32_t sig);
    // weaponcache (FarCry5Holster.cpp): the item kept with a weapon's copy from an earlier start, until the game shows it held.
    void seed(uint32_t sig, uint64_t item);
    // fistsnotmain (7 Nov): the weapon class of the game's fists and brass knuckles (Weapons/Melee/Fist/*).
    inline constexpr uint8_t kFistClass = 20;
    // The bare hands: the game's fists (class 20), or an id drawn while the game held no item (in a car it lowers the sidearm to them).
    // Never the main weapon on the sling, nor the game's last weapon a sling or shoulder grab asks for.
    bool bare_hands(uint32_t sig);
    std::string status();
}
