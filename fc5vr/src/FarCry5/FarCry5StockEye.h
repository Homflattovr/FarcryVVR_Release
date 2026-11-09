#pragma once
// stockeye (author 8 Nov 21:52: they play with a physical gunstock; research\2026-11-08_stockeye): the maths of the physical-stock hold,
// header only so the offline harness (research\2026-11-08_stockeye\stockeye_harness.cpp) runs exactly this code.
//
// On a stock both controllers are fixed to it, so the line between them steers nothing new, and what has to be right is the gun's SIGHTS
// against the EYE with the cheek on the stock. One calibration (stockeyelearn: 5 s with the cheek on the stock, aiming) gives the dominant
// eye's place and its look in the weapon controller's axes, kept as a Pose (angles and x y z, editable in F11 "Calibrate gunstock", for all
// long guns or per gun). Every long gun is then turned so its own sight line lies along that look and moved so the line passes through that
// point: each gun's own sights, whatever their height above its grip, sit before the eye. The gun is moved square to the look (along it
// nothing changes for the eye, so the grip stays as near the hand as its sight line lets it), then by the pose's z along it.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace fc5::stockeye {

// A direction from its yaw (right +) and pitch (up +) against -Z, deg (onehandmatch's convention), and back.
inline glm::vec3 dir_of(float yaw_deg, float pitch_deg)
{
    const float y = glm::radians(yaw_deg), p = glm::radians(pitch_deg);
    return glm::vec3{ std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p) };
}
inline void angles_of(const glm::vec3& d, float& yaw_deg, float& pitch_deg)
{
    yaw_deg   = glm::degrees(std::atan2(d.x, -d.z));
    pitch_deg = glm::degrees(std::asin(std::clamp(d.y / std::max(1e-6f, glm::length(d)), -1.0f, 1.0f)));
}
inline float deg_between(const glm::vec3& a, const glm::vec3& b)
{
    return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
}

// The grip pitch's turn: the weapon controller's axes as update_motion holds them (k_ctrl, grip pitch in) are the raw controller's turned by
// G about x, so a vector v in k_ctrl's axes is G v in the raw controller's. The calibration is kept in the raw axes (a later grippitch change
// does not move it).
inline glm::mat3 grip_turn(float grip_pitch_deg)
{
    return glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(grip_pitch_deg), glm::vec3{ 1.0f, 0.0f, 0.0f }) };
}

// The gun's frame against the weapon controller's: k = k_ctrl * [q | t] (k units, k_ctrl's axes).
struct Rel
{
    glm::quat q{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 t{ 0.0f };
};

// gunstockpose (F11 "Calibrate gunstock", author 9 Nov 08:32): the sights' line against the weapon controller, raw axes. up, right: its look
// (deg, as onehandmatch's angles); x, y: the point it passes, right and up of the controller's point square to the look (cm); z: the gun moved
// forward along it from where its grip is nearest the hand (cm).
struct Pose
{
    float up{ -40.0f }, right{ 0.0f }, x{ 0.0f }, y{ 6.0f }, z{ 0.0f };
};

// The look's right and up (the up made square to the look; straight up or down: the controller's -Z as up).
inline void look_frame(const glm::vec3& f, glm::vec3& r, glm::vec3& u)
{
    u = glm::vec3{ 0.0f, 1.0f, 0.0f } - f * f.y;
    u = glm::length(u) > 1e-4f ? glm::normalize(u) : glm::vec3{ 0.0f, 0.0f, -1.0f };
    r = glm::normalize(glm::cross(f, u));
}

// A pose as a line: a (m) and f, raw axes.
inline void pose_line(const Pose& p, glm::vec3& a, glm::vec3& f)
{
    f = dir_of(p.right, std::clamp(p.up, -89.0f, 89.0f));
    glm::vec3 r, u;
    look_frame(f, r, u);
    a = (r * p.x + u * p.y) * 0.01f;
}

// The calibration: the eye e (m) and its look f (raw axes) as a pose (z 0: the grip nearest the hand).
inline Pose line_pose(const glm::vec3& e, const glm::vec3& f)
{
    Pose p;
    angles_of(f, p.right, p.up);
    glm::vec3 r, u;
    look_frame(dir_of(p.right, p.up), r, u);
    p.x = 100.0f * glm::dot(e, r);
    p.y = 100.0f * glm::dot(e, u);
    p.z = 0.0f;
    return p;
}

// o, d: a point on the gun's sight line and its direction in the gun frame's axes (FarCry5Stereo::sight_line). e, f: a point of the wanted
// line and its look, k_ctrl's axes. q: the shortest turn of d onto f (the controller's roll kept, as onehandmatch); t: the sight line on the
// wanted one, moved square to f only (the least move), then z along f.
inline Rel place(const glm::vec3& o, const glm::vec3& d, const glm::vec3& e, const glm::vec3& f, float z = 0.0f)
{
    Rel             r;
    const glm::vec3 dn = glm::normalize(d), fn = glm::normalize(f);
    r.q                = glm::normalize(glm::quat{ 1.0f + glm::dot(dn, fn), glm::cross(dn, fn) });   // (d and f within 60 deg: the caller checks)
    const glm::vec3 v  = e - r.q * o;
    r.t                = v - fn * glm::dot(v, fn) + fn * z;
    return r;
}

inline glm::mat4 apply(const glm::mat4& k_ctrl, const Rel& r)
{
    glm::mat4       k = k_ctrl;
    const glm::mat3 K = glm::mat3{ k_ctrl } * glm::mat3_cast(r.q);
    k[0]              = glm::vec4{ K[0], 0.0f };
    k[1]              = glm::vec4{ K[1], 0.0f };
    k[2]              = glm::vec4{ K[2], 0.0f };
    k[3]              = glm::vec4{ glm::vec3{ k_ctrl[3] } + glm::mat3{ k_ctrl } * r.t, 1.0f };
    return k;
}

// The learn's sample: the dominant eye (side +1 right, -1 left, half: half the eyes' distance) and the head's look, in k_ctrl's axes.
// head, k_ctrl: the recentred reference space, positions in k units (scaled as update_motion's k).
inline void sample(const glm::mat4& head, const glm::mat4& k_ctrl, float side, float half, glm::vec3& e, glm::vec3& f)
{
    const glm::mat3 H{ head };
    const glm::mat3 Kt  = glm::transpose(glm::mat3{ k_ctrl });
    const glm::vec3 eye = glm::vec3{ head[3] } + H * glm::vec3{ side * half, 0.0f, 0.0f };
    e                   = Kt * (eye - glm::vec3{ k_ctrl[3] });
    f                   = glm::normalize(Kt * (H * glm::vec3{ 0.0f, 0.0f, -1.0f }));
}

// reloadzonesfollow: the drawn gun's offset from its controller, the raw controller's axes in metres (the pad poll's), from k and k_ctrl.
inline glm::vec3 raw_offset(const glm::mat4& k, const glm::mat4& k_ctrl, float grip_pitch_deg, float world_scale)
{
    const glm::vec3 dp = glm::vec3{ k[3] } - glm::vec3{ k_ctrl[3] };
    return grip_turn(grip_pitch_deg) * (glm::transpose(glm::mat3{ k_ctrl }) * dp) / std::max(1e-6f, world_scale);
}

}   // namespace fc5::stockeye
