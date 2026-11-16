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
    //   cullheadpass2 off|on       off: the culling planes as the 9-11 Nov builds made them (the eye as Rel * V' alone, no snap turn,
    //                              head turn or flat screen handling), until the next 'on' or a restart; the true margin lines stay
    // Always logged (no switch): "[CullHead] true margin, last 1 s: ..." in any second where a drawn eye was outside the planes the game
    // culled with (checked at each upload against the hook's planes of that frame), and "[CullHead] last 30 s: ..." as a summary.
    //   cullheadlog on|off         two more "[CullHead]" lines a second: the views seen and driven (camera cuts followed), and the margin:
    //                              how far inside the culling side planes both eyes' edges are (worst, deg; below 0 an eye sees past
    //                              the culling), before and after the planes are opened where an eye reaches past them (3 deg spare)
    bool command(const std::string& cmd, const std::string& arg);
    void apply_mode();   // after the command poll: makes the hook (cullhead is on unless 'cullhead off' was typed)
    bool is_on();        // cullhead on (head mode)
    // cullnarrow on|off (saved; F11 Debug > Head tracking and view, default off): the side planes of the main view's culling fitted to both eyes
    // (every way they may stand at the draw) plus 3 deg, also where the game's own lens is wider; off: the game's lens, only opened.
    bool narrow_on();
    // Render thread, at the main world camera's first upload of a frame (on_viewport_constants, not the weapon view): the game's own
    // view matrix as it came (world to camera, glm column-major from load_rows) and the view the mod gives this eye.
    // eye_proj: that eye's projection (column vectors, as uploaded), for the margin and the widening; a zero matrix (the default)
    // leaves them off.
    // own_rotation: the eye's rotation is the mod's own (motion controls: the body frame and the headset), not the game camera's.
    void on_main_upload(const glm::mat4& game_view, const glm::mat4& eye_view, int frame, const glm::mat4& eye_proj = glm::mat4(0.0f), bool own_rotation = false);
    // The mod has turned the view by yaw_rad (about world Z, positive to the left) between two frames: a view cut (the snaps that follow
    // a vehicle's entry or exit animation, the settle cut onto the vehicle's front, a snap turn without snapsync), the vehicle's own
    // turn handed to the body frame, a smooth stick turn, or a recentre (less the head's yaw it takes away). Called when the turn is
    // made, before the next frame is culled; each eye carries it until its next upload.
    // body_frame: a turn of the body frame, which turns the eye only with motion controls (own_rotation); false for a recentre.
    void view_turned(float yaw_rad, bool body_frame = true);
    // The flat screen (menus, the map) or no eye pose: the picture is the game's own camera, so its own culling is right from this frame
    // on (before, the last eye was still applied for 250 ms). The next on_main_upload ends it.
    void no_eye();
    // The switch to the flat screen (true: no_eye at once, before that frame's culling) and back (false: the first frame back is culled
    // before its eye is known, so the game's culling camera keeps its pose with its side planes opened as far as they go, for 250 ms
    // at most or until the first on_main_upload).
    void flat_screen(bool on);
    // A snap turn sent to the game camera that the eyes have not made yet (snapsync): yaw_rad the body yaw to come (about world Z,
    // positive to the left), game_yaw0 the game camera's yaw at the send (atan2(-fwd.x, fwd.y)), frame0 the frame of the send.
    // yaw_rad 0 when the eyes have turned or the turn was given up. While the camera's turn lands (or the eyes are about to stop
    // waiting for it, 12 frames after the send), the culling holds the eye both before and after the turn.
    void pending_turn(float yaw_rad, float game_yaw0, int frame0);
    // Optional (default on): false leaves the game's culling camera alone. Where the mod draws the game's own view, Rel is the identity anyway.
    void set_active(bool on);
    std::string status();
}
