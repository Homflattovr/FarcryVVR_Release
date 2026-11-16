#pragma once
// ctrlrec (FarCry5CtrlRec.cpp; research\2026-11-12_nativeweapon): a recorder of the weapon controller's readings, to tune the weapon
// smoothing on a real hand instead of a made-up one. It writes a CSV next to the game's exe and changes nothing else.
//   ctrlrec [S]        (test, not saved) S seconds from now (default 20, 1 to 300) to fc5vr_ctrlrec.csv; "ctrlrec off" ends it now.
//   ctrlrecauto on|off (saved, default off) once per session, 20 s from the moment a gun has been in hand for 5 s, to
//                      fc5vr_ctrlrec_auto.csv (the file of the session before is replaced).
// One row per controller reading (a stereo pair's second eye, the same reading, is left out): time (ms), frame, weapon id, two-handed
// hold, the weapon controller as read (place in metres, turn as a quaternion; the recentred space, grip tilt in), the other
// controller, the gun the hold asked for, and the gun as drawn after the smoothing and the weight.
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace fc5::ctrlrec
{
    bool command(const std::string& cmd, const std::string& arg, float fv, bool& persist);
    // Render thread, once per update_motion with a weapon controller: this frame's poses (update_motion's space; world_scale divides
    // the places back to metres). gun_drawn: a gun mesh was drawn in the last frames.
    void note(int frame, const glm::mat4& ctrl, bool have_other, const glm::vec3& other_pos, const glm::mat3& other_rot, const glm::mat4& held, const glm::mat4& out,
              uint32_t weapon_sig, bool two_hand, bool gun_drawn, float world_scale);
}
