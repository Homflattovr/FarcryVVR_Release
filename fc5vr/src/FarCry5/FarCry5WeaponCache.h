#pragma once
// weaponcache (FarCry5Holster.cpp): the recorded weapon copies kept on disk between game starts (bin\fc5vr_weaponcopies).
#include <cstddef>
#include <cstdint>

namespace fc5::weaponcache
{
    // weaponcache on|off (saved, default on).
    bool on();
    // The device's shader creation (FarCry5Stereo.cpp: hk_create_vs, note_pixel_shader), any thread: the bytecode of a shader a kept copy
    // draws with is written next to the copies, so a later start can make that shader itself if the game has not made it yet.
    void note_shader(bool ps, uint32_t crc, const void* code, size_t size);
}
