#pragma once
// gameray (FarCry5GameRay.cpp): the game's own physics ray cast, on the script thread, for melee by contact.
#include <cstdint>
#include <string>

namespace fc5::gameray
{
    struct Seg { float x0, y0, z0, x1, y1, z1; bool ahead; };   // world, a segment to look along; ahead: where the swing is going, not where it is
    struct Hit { bool any; uint64_t entity; float x, y, z, nx, ny, nz, health; int part; float along; };   // part: the body part's number on the finer shapes (below), -1 the outer capsule or a wall
    // (along: metres from the segment's start)
    struct Result { Hit body, wall, body_ahead, blade; };   // blade: the nearest wall on the first segment (meleecollide)
                 // body: an entity with health, not the player; wall: anything else, not the player

    // Any thread: these segments are cast at the next game tick (the last ones sent win).
    void submit(const Seg* segs, int n);
    // Any thread: the result of the latest cast, once. False: none since the last call.
    bool take(Result& out);
    // From FarCry5Lua's game tick, on the script thread: work is waiting (due), then do it (tick).
    bool due();
    void tick();
    // Script thread only (spawncommand): the height of the nearest surface below (x, y, z), not the player. False: none within 8 m, or no ray cast.
    bool ground_z(float x, float y, float z, float& out);
    // Any thread, a test: one cast at the next tick, every hit logged. mask and flags < 0: the set ones.
    void test(const float start[3], const float delta[3], int64_t mask, int flags);
    void set_mask(uint32_t mask);
    void set_flags(int flags);
    void set_fine(bool on);   // meleerayfine
    std::string status();
}
