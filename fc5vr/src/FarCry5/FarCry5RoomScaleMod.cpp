// Room scale, first version: the mod's side (the rule is FarCry5RoomScale.cpp, see FarCry5RoomScale.h).
//  - roomscale_note_base: every camera upload on foot gives the body the eyes stand on (the eyes' base after viewroot, walkbob and
//    smoothmotion, less the offset turned with the head), so the view is held against exactly what the eyes ride on.
//  - roomscale_present: once per picture (the right eye's Present of a stereo pair; every Present in AFW), after the body frame has
//    turned: the neck's distance from the body, the game's walk direction, fc5::roomscale::step, and the centre (m_ref_inv) moved
//    by what the body walked. The pad pollers add the push themselves (fc5::roomscale::left_stick, ModConfig.cpp).
#include "FarCry5Stereo.h"
#include "FarCry5RoomScale.h"
#include "FarCry5BodyIk.h"
#include "FarCry5Gesture.h"
#include "FarCry5Lua.h"

#include <mods/VR.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

void FarCry5Stereo::roomscale_note_base(int frame, const glm::vec3& body)
{
    m_rs_body       = body;
    m_rs_body_frame = frame;
}

void FarCry5Stereo::roomscale_present()
{
    static auto vr    = VR::get();
    const int   frame = m_frame.load();
    const bool  every = afw_mode();                    // AFW: every Present is its own picture
    if (!every && (frame & 1) == 0) return;            // Stereo: the pair ends with the right eye (as pairbody)
    const auto  now = std::chrono::steady_clock::now();
    const float dt  = std::chrono::duration<float>(now - m_rs_t).count();
    m_rs_t          = now;
    if (!fc5::roomscale::on() && m_rs_follow_frame < frame - 3) return;   // off, and nothing of a walk left to end

    fc5::roomscale::In in{};
    in.ok    = true;
    in.dt    = dt;
    in.frame = frame;
    auto no  = [&in](const char* why) { if (in.ok) { in.ok = false; in.why = why; } };
    if (!vr->is_hmd_active()) no("no headset");
    if (!m_use_hmd_position.load()) no("the head's position is not used");
    if (m_menu_screen_on) no("the flat screen");
    if (m_in_vehicle.load() || m_chute_vehicle || m_air_blend > 0.0f) no("a vehicle, a fall or a parachute");
    if (m_cine_on) no("a cutscene");
    if (!m_mo.valid || m_mo.frame != frame || m_player_views_last_frame == 0) no("no player camera");
    if (mounted_now()) no("a mounted gun");
    if (fc5::lua::pawn_in_air() == 1 || m_falling) no("in the air");
    if (fc5::lua::pawn_down() == 1 || m_dv_active) no("knocked down");
    if (fc5::lua::pawn_qte() == 1) no("a struggle");
    if (fc5::bodyik::seated()) no("seated mode");   // seated play (F11 Motion control > Hands, body and arms): a lean is not a step

    // The body the eyes stand on, if this picture uploaded one.
    const auto pic = [every](int f) { return every ? f : (f & ~1); };
    in.body_ok     = pic(m_rs_body_frame) == pic(frame);
    in.body        = glm::vec2{ m_rs_body };

    // The body frame's axes across the ground (as body_rotation: forward at m_body_yaw, 0 = +y, counter-clockwise).
    const float     yaw = m_body_yaw;
    const glm::vec2 fwd{ -std::sin(yaw), std::cos(yaw) }, right{ std::cos(yaw), std::sin(yaw) };
    const float     ws = std::max(1e-3f, m_world_scale.load());

    // From the centre to the neck, in the reference frame (x right, y up, z back): the headset less the head's own turn about the
    // neck (looking down brings the headset 10 cm forward; the body under it has not moved). testheadpos moves it as it moves the eyes.
    const glm::mat4 hmd = vr->get_transform(0);
    if (!std::isfinite(hmd[3][0]) || (hmd[0][0] == 0.0f && hmd[1][1] == 0.0f && hmd[2][2] == 0.0f)) no("no head pose");
    if (in.ok) {
        glm::mat4 rel{ 1.0f };
        {
            std::lock_guard _{ m_ref_mtx };
            if (!m_have_ref) no("not recentred yet");
            else rel = m_ref_inv * hmd;
        }
        const glm::vec3 neck{ 0.0f, -fc5::gesture::kNeckPivotDown, fc5::gesture::kNeckPivotBack };
        const glm::vec3 p = (glm::vec3{ rel[3] } + glm::mat3{ rel } * neck - neck) * ws +
                            glm::vec3{ m_test_head_pos[0].load(), m_test_head_pos[1].load(), m_test_head_pos[2].load() };
        in.debt           = right * p.x - fwd * p.z;
    }

    // The heading the game walks along with the stick forward: its aim (the camera less the Extended View offset it carries) with the
    // mouse counts in flight, as walkbasis (walk_set_rot, FarCry5Walk.cpp).
    if (in.ok) {
        const glm::vec3 cam_fwd = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
        if (std::sqrt(cam_fwd.x * cam_fwd.x + cam_fwd.y * cam_fwd.y) < 0.25f * std::max(1e-6f, glm::length(cam_fwd))) no("the game's aim points straight up or down");
        float game_yaw = std::atan2(-cam_fwd.x, cam_fwd.y);
        if (m_mo.ev_ok) game_yaw -= m_mo.ev_oy;
        int fx = 0, fy = 0;
        inflight(m_mo.ev_seq, fx, fy);
        in.game_yaw = game_yaw + glm::radians(-float(fx) * m_deg_per_count);
    }
    // The head's look above or below the horizon (camjitter: looking down starts no walk).
    if (glm::length(m_mo.head_world) > 1e-4f) in.head_pitch = std::asin(std::clamp(glm::normalize(m_mo.head_world).z, -1.0f, 1.0f));

    const fc5::roomscale::Out out = fc5::roomscale::step(in);
    if (out.following) m_rs_follow_frame = frame;
    if (out.shift.x != 0.0f || out.shift.y != 0.0f) {
        // The centre moves with the body: the head's offset from it shrinks by what the body walked, so the eyes (base + offset) stay.
        const float dx = glm::dot(out.shift, right) / ws, dz = -glm::dot(out.shift, fwd) / ws;
        std::lock_guard _{ m_ref_mtx };
        if (m_have_ref) m_ref_inv = glm::translate(glm::mat4{ 1.0f }, glm::vec3{ -dx, 0.0f, -dz }) * m_ref_inv;
    }
}

// turnhead (FarCry5RoomScale.h): the body frame turns from yaw0 to yaw1 by the stick; the centre of the head tracking moves so the head keeps
// its place across the ground. Called where the turn lands: between pairs (motion_post_present) or, for a snap synced with the game camera,
// at the pair's first upload (update_motion), where this eye's offset was already read: turnhead_patch moves it too.
bool FarCry5Stereo::turnhead_fix(int frame, float yaw0, float yaw1, const char* how)
{
    static auto vr = VR::get();
    const float turn = std::remainder(yaw1 - yaw0, glm::two_pi<float>());
    if (!fc5::roomscale::turnhead_on() || std::abs(turn) < 1e-5f) return false;
    const char* no = !vr->is_hmd_active() ? "no headset" : !m_use_hmd_position.load() ? "the head's position is not used" : m_menu_screen_on ? "the flat screen"
                     : m_in_vehicle.load() ? "a vehicle" : m_cine_on ? "a cutscene" : m_dv_active ? "knocked down" : nullptr;
    const glm::mat4 hmd = vr->get_transform(0);
    if (!no && (!std::isfinite(hmd[3][0]) || (hmd[0][0] == 0.0f && hmd[1][1] == 0.0f && hmd[2][2] == 0.0f))) no = "no head pose";
    const float ws = std::max(1e-3f, m_world_scale.load());
    glm::vec2   fix{ 0.0f }, p{ 0.0f };
    if (!no) {
        std::lock_guard _{ m_ref_mtx };
        if (!m_have_ref) no = "not recentred yet";
        else {
            // The head's offset as the eyes are drawn with it (scaled, testheadpos added), turned back by the turn; the centre takes the difference.
            const glm::vec3 h = glm::vec3{ (m_ref_inv * hmd)[3] } * ws + glm::vec3{ m_test_head_pos[0].load(), m_test_head_pos[1].load(), m_test_head_pos[2].load() };
            p                 = glm::vec2{ h.x, h.z };
            if (glm::length(p) > 1.5f) no = "the head over 1.5 m from the centre";
            else {
                fix       = fc5::roomscale::turn_fix(p, yaw0, yaw1);
                m_ref_inv = glm::translate(glm::mat4{ 1.0f }, glm::vec3{ -fix.x, 0.0f, -fix.y } / ws) * m_ref_inv;
            }
        }
    }
    if (no) {
        if (m_th_skip++ < 20) spdlog::info("[Turn] a {} of {:+.0f} deg turned about the body, not the head: {}", how, glm::degrees(turn), no);
        return false;
    }
    m_th_fix       = fix;
    m_th_fix_frame = frame;
    m_th_count++;
    if (std::abs(turn) > glm::radians(2.0f) && (m_th_count < 100 || (m_th_count % 50) == 0))   // snaps (a smooth turn lands a little every pair)
        spdlog::info("[Turn] a {} of {:+.0f} deg about the head: the head {:.0f} cm from the centre, the centre moved {:.0f} cm (turn {})", how, glm::degrees(turn),
                     glm::length(p) * 100.0f, glm::length(fix) * 100.0f, m_th_count);
    return true;
}

void FarCry5Stereo::turnhead_patch(int frame, glm::mat4& rel)
{
    if (m_th_fix_frame != frame) return;
    m_th_fix_frame = -100;   // once: the eye uploaded with the turn; later uploads read the moved centre themselves
    rel[3] -= glm::vec4{ m_th_fix.x, 0.0f, m_th_fix.y, 0.0f };
}
