#pragma once
// recoil (FarCry5Recoil.cpp; author 7 Nov 20:13): each of the
// player's shots kicks the drawn gun (and the hand on it) back toward you and its muzzle up, with a little random sideways turn and roll,
// then it settles back onto the controller. With recoilaim (default on; author 8 Nov 08:55) the shots'
// line is turned by the same kick, so the shots follow the kicked gun; off, only the picture moves. The size and timing follow the weapon's
// class in the game's data; heavy guns kick harder.
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

#include <glm/glm.hpp>

namespace fc5::recoil
{
    // recoil on|off, recoilaim on|off, recoilscale K (0-2), recoiltwohand K (0-1), recoilscope K (0-1), recoilbuzz on|off (saved); recoillog on|off,
    // recoiltest N MS (not saved).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // The game thread's shot code, for each of the player's shots (fc5::bullet's player shot callback, the same call as shotbuzz).
    void note_shot();
    // The kick for the weapon pass being placed, in the gun's own axes as the controller holds it (x right, y up, z back, metres), as a
    // turn R about the grip point and a move T: a gun point x (relative to the grip) is drawn at R x + T. The same kick for every pass
    // of one game update (key: the stereo pair, or the AFW frame). False when nothing kicks (off, no shot in the last 0.6 s, a weapon
    // with no recoil). two_hand: held with both hands; scope: the eye is behind a magnifying scope.
    bool kick(int update_key, uint32_t weapon_sig, bool two_hand, bool scope, glm::mat3& R, glm::vec3& T);
    // recoilbuzz: the shot pulse's strength and length scaled by the weapon (a light SMG buzzes softer and shorter than a shotgun).
    // False when recoil or recoilbuzz is off (the pulse as set under Feel).
    bool buzz(uint32_t weapon_sig, float& amp_mul, float& len_mul);
    // recoilaim: recoil and recoilaim both on (the shots' line takes the kick: FarCry5Stereo.cpp update_motion).
    bool aim_follows();
    // F11 Weapon > Recoil: the current settings for the menu, and what the gun in hand gets (its class's name, 0 = no recoil).
    struct Settings { bool on{ false }, aim{ true }, buzz{ true }; float scale{ 1.0f }, two_hand{ 0.6f }, scope{ 0.4f }; };
    Settings settings();
    const char* weapon_kind(uint32_t weapon_sig, float& size);
}
