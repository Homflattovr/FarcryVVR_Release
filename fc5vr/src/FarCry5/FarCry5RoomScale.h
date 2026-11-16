#pragma once

// Room scale, first version (author 12 Nov 2026).
// research/2026-11-13_roomscale/README.md has the plan; this is its step 1, "the body walks under the headset".
//
// Until now the head moved freely off the game's body: the eyes are the game camera plus the headset's offset from a centre
// taken at the last recentre, the body (what the game collides, what NPCs see and shoot, where the holsters and the legs hang)
// stayed behind, and past 50 cm for half a second autorecenter moved the centre under the head, which threw the view back onto
// the body. Walking in the room was a 50 cm leash with a jump at its end.
//
// roomscale on|off (saved, default off; F11 Motion control > "Room scale (test)"): on foot, when the neck is further than
// roomscalestart (12 cm) from the body across the ground and the player is standing with the left stick let go, the mod pushes
// the game's left stick towards the head, so the game itself walks the body there (its own collisions, steps, slopes, sounds
// and legs), and while it does, the centre moves by exactly what the body moved: the view stays where the headset is, the body
// arrives under it. Walk on in the room and the body keeps pace behind the head; the push ends when the body's slide will
// bring it under the head (roomscalestop, 4 cm), and the view is held for that slide. Nothing is written into the game: only
// the pad's left stick and the mod's own centre.
//  - The distance is the neck's (the headset less the head's own turn about the neck, as the gestures' body), so looking down
//    or around moves nobody.
//  - camjitter (author 13 Nov 22:34): a walk starts only once the player's own stick has been let go for 0.5 s (walks began
//    70 ms after it was let go, between two pushes of his own, and were cut 0.2 to 1 s later with the body 26 to 75 cm on); none starts
//    or pushes again with the head looking down more than 30 deg (people bend at the hips to look at the ground: four walks in a row at
//    22:33:36-39, 40 to 84 cm each, the head never closer); and a push's start learned in a hurry is kept at most 0.15 above the last
//    calm one (one put every later push at 0.85, nearly a run for a 12 cm step, from 22:21:11 to the end of the session).
//  - The game's answer to the stick is not known to the mod, so it is learned while walking (for the session, logged):
//    where the body starts to move (a push that moves nobody after 0.3 s grows until it does), how fast it goes per stick
//    (the push is steered to the speed the body can still stop from in the distance left), and how far it slides after a
//    push (the next push ends that early). A body that walked past the head is pushed back, twice at most.
//  - A body that does not move at full push (a wall, a rail, a table), or that walks away from the head, is given up: the push
//    stops, tried again when the head has come back, gone another way, or after 3 s. autorecenter stays the last resort (the
//    head 50 cm into a wall: the view goes back to the body); it waits while a walk runs.
//  - Never in a vehicle, on the flat screen, in a cutscene, at a mounted gun, in the air, knocked down, in a struggle, during a
//    load, or while the player's own left stick is pushed (the stick walks as before; the body catches up once it is let go).
//  - turnhead on|off (saved, default off): snap and smooth turns about the head (see turnhead_on below).
//  - Test commands (not saved): roomscalestop CM, roomscalestick LO HI (where a push starts, and the most it grows to while
//    the body answers), roomscalebrake MS (the slide, learned on from there), roomscalelog N (one [Room] log line per
//    picture for N pictures), roomscalestatus.
//
// This file has no game or framework code: step() is the whole rule, tested offline (roomscale_harness.cpp in
// research/2026-11-13_roomscale). FarCry5RoomScaleMod.cpp feeds it and applies its answer.

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::roomscale
{
    bool on();   // roomscale on

    struct In
    {
        bool        ok{ false };        // on foot in play, tracked, nothing else holding the player (why: what, when not)
        const char* why{ "" };
        bool        body_ok{ false };   // body is this picture's
        float       dt{ 0.0f };         // seconds since the last step
        int         frame{ 0 };
        glm::vec2   debt{ 0.0f };       // from the body to the neck across the ground, world x y, game metres
        glm::vec2   body{ 0.0f };       // the body the eyes stand on (their base less the offset turned with the head), world x y
        float       game_yaw{ 0.0f };   // the heading the game walks along with the stick forward (rad, 0 = +y, counter-clockwise)
        float       head_pitch{ 0.0f }; // the head's look above (+) or below (-) the horizon, rad: looking down is a bend, not a step (no walk starts)
        int64_t     now_ms{ 0 };        // tests: the clock (0: the steady clock, as left_stick's)
    };
    struct Out
    {
        glm::vec2 shift{ 0.0f };       // the centre moves by this across the ground now (world x y): the view stays, the body has moved
        bool      following{ false };  // the body is on its way (autorecenter waits)
        bool      inject{ false };     // a push is sent (stick below; the pad pollers read their own copy)
        float     sx{ 0.0f }, sy{ 0.0f };
    };
    // Once per picture (the end of a stereo pair), render thread.
    Out step(const In& in);

    // Every pad poll, after walkbasis turned the player's own stick (input thread, and the virtual pad's): the push added. blocked: a load.
    void left_stick(int16_t& lx, int16_t& ly, bool blocked);
    void left_stick_at(int16_t& lx, int16_t& ly, bool blocked, int64_t now_ms);   // the same with the clock given (tests)
    bool pushing();   // a push is on its way (the pad hook counts it as controller use, so it reaches the game)

    // turnhead on|off (saved, default off; F11 Motion control > "Room scale (test)"): a turn of the right stick (snap or smooth) on foot turns
    // the view about the head, not about the game's body. Until now the body frame turned about the body and carried the head's offset
    // round with it: with the head 30 cm off the body a 45 deg snap also threw the view some 23 cm sideways. Now the centre of the head
    // tracking is moved by the turn too, so the head keeps its place in the world and only the view's heading changes (the body still
    // turns where it stands; with roomscale on it then walks under the head as after any step).
    bool turnhead_on();
    // The centre's move (reference frame of the head tracking: x right, z back; the units of p) that keeps the head's place across the
    // ground when the body frame turns from yaw0 to yaw1 (rad, 0 = +y, counter-clockwise): p is the head's offset from the centre.
    glm::vec2 turn_fix(const glm::vec2& p, float yaw0, float yaw1);

    bool        command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    std::string status();
    // F11 Motion control, under Walking.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
