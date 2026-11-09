#pragma once
// cullhead (FarCry5CullHead.cpp, always on since 9 Nov: the author headset check): the engine's own main-view culling camera posed from the headset.
// Camera-culling thread, author 8 Nov 08:57. Run 4
// (research\2026-11-08_cullcam\runtime4.md): on the render thread, with UpdateCullingCamera ([[FC+0x4EB73E8]+0x1A4]) and the view's byte [[rbx+0xa8]+0x111] set,
// FC+0x43a150(rdi+0x20, rsp+0x60) copies the frame camera into the view's culling camera (rdi+0x20, SceneCamera layout: View +0x30,
// InvView +0x70, World +0xB0, Proj +0xF0, ViewProj +0x170, position +0x1F0, near/far/fov +0x22C/+0x230/+0x234), then
// FC+0x43c3a0(rdi+0x20, rsp+0x60, r14) runs. Written there at yaw 90, the yaw-0 camera culled what yaw 0 culls (draws 1141 -> 857).
// This hooks FC+0x43c3a0 (only when called from that block) and puts the head's pose into the main view's culling camera.
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace fc5::cullhead
{
    // Once, with FC_m64's base (stores it; apply_mode() after the first command poll makes the hook and logs "[CullHead] hook at +0x43c3a0: ...").
    void install(uint8_t* base);
    // Commands (one per line in fc5vr_cmd.txt), none saved, for comparison and diagnosis only:
    //   cullhead off|on            off: the game's own culling camera until the next 'on' or a restart
    //   cullheadlog on|off         two "[CullHead]" lines a second: the views seen and driven (camera cuts followed), and the margin:
    //                              how far inside the culling side planes both eyes' edges are (worst, deg; below 0 an eye sees past
    //                              the culling), before and after the planes are opened where an eye reaches past them (3 deg spare)
    bool command(const std::string& cmd, const std::string& arg);
    void apply_mode();   // after the command poll: makes the hook (cullhead is on unless 'cullhead off' was typed)
    bool is_on();        // cullhead on (head mode)
    // Render thread, at the main world camera's first upload of a frame (on_viewport_constants, not the weapon view): the game's own
    // view matrix as it came (world to camera, glm column-major from load_rows) and the view the mod gives this eye.
    // eye_proj: that eye's projection (column vectors, as uploaded), for the margin and the widening; a zero matrix (the default)
    // leaves them off.
    void on_main_upload(const glm::mat4& game_view, const glm::mat4& eye_view, int frame, const glm::mat4& eye_proj = glm::mat4(0.0f));
    // Optional (default on): false leaves the game's culling camera alone. Where the mod draws the game's own view, Rel is the identity anyway.
    void set_active(bool on);
    std::string status();
}
