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
    // meleereact (12 Nov): on a segment a wall nearer than the body hides that body (nobody is hit through a wall).
    void set_wall_blocks(bool on);
    // weaponcollide (12 Nov): one more segment, cast at the next game tick beside the melee ones (its own slot: neither replaces the other).
    // The answer is the nearest thing on it that is no character and not the player (any = false: nothing); a hit under 3 cm from the start
    // is a start inside something and is left out.
    void submit_probe(const Seg& seg);
    bool take_probe(Hit& out);   // once per cast; false: none since the last call
    std::string status();
    // melee fix 13 (author 15 Nov: the damage seems to land only after the swing): when the game's blow lands. Any thread, at a press of the game's
    // melee: the entity's health is read (GetPawnHealth's native, read only, no stim) at each game tick for 1.5 s after the press, and the first change
    // is logged as "[Melee] damage landed: ... N ms after the press (M ms after the contact)", or "... no change" at the end. entity 0: not known yet
    // (a bones hit); watch_entity names it from the ray's next answer on the body within 1 s. health_at_contact < 0: the first read is the start.
    // melee fix 14b: entity 0 with root_ok: the recorded entity nearest the body's root (world x, y, z; within 0.7 m across the ground, 1.2 m up or
    // down, a living pawn) is looked up at the next game tick (fc5::lua::entity_at_root); none: the reason is logged and nothing is watched. A press
    // does not replace a watch on an entity still within its 3 s (nor while meleedmgwatch's write watch waits for its write). src: where the entity came from.
    void watch_damage(uint64_t entity, float health_at_contact, int64_t contact_to_press_ms, bool root_ok = false, float rx = 0.0f, float ry = 0.0f, float rz = 0.0f,
                      const std::string& src = {});
    void watch_entity(uint64_t entity, float health);
}
