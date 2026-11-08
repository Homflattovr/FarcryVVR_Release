#pragma once
// spawncommand (FarCry5Spawn.cpp): any weapon in the hand, the held one dropped; any world entity on the ground. F11 Debug > Spawn command.
#include <cstdint>
#include <functional>
#include <string>

namespace fc5::spawn
{
    // spawnhand <item hex>, spawnground <archetype hex>, spawnyaw N (test). bx, by: the player's "back" axis in the world now (spawnfacing), to place things ahead of the player.
    bool     command(const std::string& cmd, const std::string& arg, float bx, float by);
    void     draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
    // vehiclespawn: spawnvehicle <archetype hex> (placed), spawnvehiclein <archetype hex> (placed, the player seated in it), spawnvehicleremove (all through command). F11 Debug > Vehicle spawn.
    void     draw_vehicle_menu(std::string& cmds, const std::function<void(const char*)>& hint);
    uint64_t pickup_of(uint64_t item);   // the world entity the game's data names for this item (0: none)
}
