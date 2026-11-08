#pragma once
// scriptwatch (test, log only; FarCry5ScriptWatch.cpp): watch any of the game's Lua script functions by name.
#include <string>

namespace fc5::scriptwatch
{
    // scriptwatch list <part> | scriptwatch <Name> | scriptwatch off | scriptwatch status. Returns a line for the log.
    std::string command(const std::string& arg);
    // hookprobe <rva hex> [label] | list | off: any function of the plain code logged by address. Returns a line for the log.
    std::string probe_command(const std::string& arg);
}
