#pragma once
// gamesound (FarCry5GameSound.cpp): the game's own sounds (its Wwise events), played by ID and watched.
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace fc5::gamesound
{
    // Play this event (the numeric Wwise id, as in the game's data) after delay_s, on the game's script thread. type: the game's sound type for it (the second value
    // soundwatch logs; a type the game has no entry for plays nothing; a 1911's reload is 15). False: the game's sound call is not usable.
    bool post(uint32_t id, int type = 15, float delay_s = 0.0f);
    // From FarCry5Lua's game tick, on the script thread: work is waiting (due), then do it (tick).
    bool due();
    void mute_handling(int ms);   // the game's own handling sounds (type 15) not played for this long (0: from now on played): a shot's own pump, lever or bolt cycle, which the hand plays again
    void tick();
    // soundwatch on|off (F11, saved): log the sound ids the game plays. soundplay <id> [param] (test). soundstatus. True: the command was one of these.
    bool command(const std::string& cmd, const std::string& arg, std::string& reply);
    bool watch_on();
    // reloadsoundlearn (manualreload33): keep the handling sounds (type 15) the game plays, with their time; heard: those since a moment (from 0.1 s before
    // it; t in seconds from it).
    struct Heard { float t; uint32_t id; };
    void               record(bool on);
    std::vector<Heard> heard(std::chrono::steady_clock::time_point since);
}
