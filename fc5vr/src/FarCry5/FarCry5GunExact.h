#pragma once
// gunstillexact (FarCry5GunExact.cpp; research\2026-11-12_nativeweapon set 4): the gun's pose of this update read from the game's own
// memory at the weapon pass's first draw, so the left eye's weapon camera needs no guess.
//
// Why. In stereo the left eye's weapon pass is a new game update, and the gun mesh's transform reaches the mod only at the gun mesh's
// draw, late in the pass: the camera is first placed with a guess (3.8 mm / 0.65 deg off on average in the author's play of 9 Nov) and
// placed again at the first known part. What is drawn before that (the two arms, every pass) is drawn with the guess.
// The game keeps copies of the gun's world transform that its animation jobs write before the render (research
// fc2_arms_body_2026-10-24.md). The 12 Nov gunscan (testruns\2026-11-12_nativeweapon2_scene): four copies already held this update's
// value at the weapon pass's first draw in 301 of 301 passes whose gun had moved.
//
// How a copy is found, with no fixed address. A worker thread at the lowest priority reads the game's private read-write memory
// and keeps every 16-byte aligned place whose three floats are one of the gun mesh's last transforms' first column (the render
// thread publishes it at each gun mesh draw). Each such place is then put to the proof, every pass: read at the pass's first draw,
// compared at the gun mesh's draw with the mesh's own transform. A place that gave the mesh's transform exactly, before the mesh was
// drawn, in 10 passes in a row whose gun had moved is taken as the copy; three misses in a row and it is dropped and looked for
// again. A level load, a weapon switch or the game moving its memory therefore only cost a new search (the guess meanwhile, as
// before); each weapon's last copy is tried first when the weapon comes back.
#include <cstdint>
#include <string>

namespace fc5::gunexact
{
    // gunexactscan (test), and the render thread when the switch is on and no copy is known: one search of the memory.
    void request_scan();
    // Render thread, the weapon pass's first draw. Every place under proof is read. True and the gun's transform (object rows, as
    // the gun mesh's object constants hold it) when want is set and a proven copy reads as a rigid transform near the gun.
    // frac, frac_ok (camfine): the gun's WorldPositionFractions of this update, once found beside the copy (three floats, or the
    // place kept as doubles) and read right before the mesh's draw in 10 passes in a row; or zero when the mesh's has been zero for
    // 30 passes.
    bool first_draw(int frame, uint32_t sig, bool want, float obj[12], float frac[3], bool& frac_ok);
    // Render thread, the gun mesh's draw: its own transform. used: this pass's camera was placed from the copy at its first draw.
    // frac: the mesh's WorldPositionFractions (null: its object constants hold none).
    void gun_mesh(int frame, uint32_t sig, const float obj[12], const float* frac, bool used);
    // A pass that wanted the copy and did not get it: 0 no proven copy, 1 the copy did not read as a rigid transform near the gun,
    // 2 the weapon camera could not be placed again at the first draw.
    void fell_back(int why);
    // gunstillstat's line; its counters start over.
    std::string stat_line(bool on);
}
