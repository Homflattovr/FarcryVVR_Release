#pragma once
// meleedmgwatch (FarCry5MeleeDmgWatch.cpp; melee fix 14 probe, test only, never saved): where the game writes an NPC's health when its own melee
// blow lands. After a press of the game's melee at a contact (fix 13's press), the hit NPC's health object is found by the same calls
// GetPawnHealth's native (+0x2FA2900) makes, the float equal to its health is looked for in it, and a hardware write watch (DR0-DR3, every
// thread) is set on it for at most 3 s. The first write is logged once: the writing instruction (FC_m64+offset), the thread, old and new value,
// ms since the contact and the press, 12 return addresses. Then the watch is cleared. One watch at a time; a fault turns the probe off.
#include <cstdint>
#include <string>

namespace fc5::meleedmgwatch
{
    void        set_on(bool on);
    bool        on();
    std::string status();
    // Script thread, from gameray's tick (the game's camera update): marks the frame point for the watch's log.
    void tick_begin();
    void tick_end();
    // Script thread, from gameray's damage watch at its first read after a press: entity and its health now; press and contact times (steady ms).
    void arm(uint64_t entity, float health, int64_t press_ms, int64_t contact_ms);
    // Script thread, from gameray's damage watch: its health changed (a -> b) or not (changed false, 1.5 s after the press).
    void landed(uint64_t entity, float a, float b, bool changed);
    // melee fix 14b: a write watch is set (or being set) and has not caught its write yet (a new press must not replace it).
    bool waiting();
}
