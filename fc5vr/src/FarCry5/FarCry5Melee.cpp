// meleehit (25 Oct, author 24 Oct 21:59): melee by hitting a body.
//  - The bodies: every skinned character draw of the world pass (a Character_* material with a bone palette) gives its object's
//    origin, the character's root at its feet; the draws of one frame are grouped by place (0.45 m across the ground, the lowest kept),
//    the player's own (right under the game camera) left out. Each body is a standing capsule from 0.2 to 1.8 m above its root,
//    meleeradius (0.30 m) round.
//  - The weapon: a segment from 0.25 m behind the weapon hand (a rifle's stock) to meleereach (0.55 m) ahead of it along the gun
//    (a machete, a bat, a rifle's barrel), or the bare hand (a fist, 8 cm wider) with no weapon in hand or the weapon on the sling.
//  - A hit: the segment enters a body's capsule while its faster end moves over meleespeed (2.5 m/s, relative to the game camera,
//    so walking into someone is no hit); then at most one every meleecool ms. meleehit 1 logs it ([Melee] hit ...) and pulses the
//    weapon hand (meleebuzz); meleehit 2 also turns the game's aim onto the point hit (meleeaim, like a throw: the eyes stay on the head
//    through Extended View; steps of up to meleeslew deg, 60, against firesplitslew's 12: a body 54 deg aside aimed in 66 ms, 230 ms at 12), and presses the game's melee (meleebutton r3 = the stick click, the game's melee / takedown; rt = the
//    fire trigger, a melee weapon's swing; both) for meleepressms once the aim is within meleegate deg, or after meleewait ms.
//  - meleestill on|off (default off since release 0.12.1, author 25 Oct 12:40; author 25 Oct 10:22): a melee weapon (its game hand farther than meleeauto 0.62 m from the camera:
//    the game holds a machete or a bat low at its side; guns sit within 0.55 m; or set by hand: meleeweapons +sig -sig) is held at a
//    made pose in FarCry5GunStill.cpp: its length (the model's +Y, a gun's barrel axis) along the aim's axis turned up by meleepitch
//    (40 deg), right by meleeyaw, rolled by meleeroll, about the game's hand on it, so none of the game's idle, run or swing
//    animation shows. With it meleehit's weapon is the blade: 8 cm behind the hand to meleeblade (0.70 m) along it, and meleebutton
//    auto (the default until release 0.12.1, r3 since) sends RT (the weapon's own swing) instead of the stick click.
//    Since 3 Nov (author) also whenever meleehit is on: a swing must be the player's hand alone (melee_still_now).
//  - meleeearly ms (melee fix 12: no effect now, the game's melee is pressed only at a contact; it was: default 100, 0 = off; author 26 Oct 12:44): with meleehit 2
//    the game's melee is pressed once the swing, moving on as it moves, will reach a body within that time (its blow lands ~0.2 s after the
//    press); the pulse waits for the contact.
//  - Damage (melee fix 10b, author 13 Nov 18:32Z): the mod sends none.
//    meleedamage, meleedamagehp, meleestim, meleeheadx (and the meleedamageself/probe/status tests) are gone, read and ignored from old
//    settings; the game's own melee, pressed at the contact (meleehit 2, meleebutton, meleeearly, the aim on the point hit), deals it.
//  - meleeray on|off (new, saved, default off; 2 Nov, author 15:15; FarCry5GameRay.cpp): instead of the capsules the game's own
//    physics ray cast along the weapon and along the path of its tip and middle: a hit is on the real shape of whatever has health, wherever it stands, crouches or lies, the NPC
//    known from the hit itself (logged), the point hit its aim. meleewallbuzz (0.4, 0 = none): the pulse when a swing meets anything else (a wall, a car).
//    meleeearly: a ray from the tip on along its speed. The test body stays a capsule. meleeraytest ox oy oz dx dy dz [mask] [flags] (x right, y ahead, z up from the camera's heading;
//    default 5 m ahead): one cast, every hit logged. meleeraymask, meleerayflags (0x2DBF as IsEntityInRay, 6 every hit). meleeraystatus.
//  - meleebones on|off (default on; 2 Nov, author 15:15): each character near the
//    player (4 m) is the shape of its own skinned draws instead of one standing capsule: every palette slot (bone) of a character draw is
//    a capsule fitted once to the vertices it moves most (rest pose: their centre, main axis, 5-95 % of their length, 60 % of their
//    distance from the axis; the draw's index and vertex ranges read back once, one draw every 10 frames), posed each frame by the
//    slot's palette matrix (VS b3) and the object transform (VS b1), as the GPU skins it. So a crouching, lying or turning body, an
//    arm or a head is hit where it is drawn; the weapon (meleepad 0.04 m thick, the bare hand 3 cm more) touching any capsule is the
//    contact, aimed at that point. The capsule's rest height names the part (head above 1.45 m, legs below 0.85 m, arms out to the
//    sides, else the torso), written in the hit's log line. A draw that cannot be read (no palette copy, an
//    unknown vertex format) leaves its body the standing capsule.
//  - The swing's speed is measured in the body's axes (the controller as the player moves it): a snap turn, walking or the game
//    turning the body no longer count as a swing (author 27 Oct 21:11 log at 35 and 53 m/s, one 0.9 m behind him), and a
//    jump over 30 m/s (tracking lost and found) is no swing either. The swept test starts from where the weapon was in the body's
//    axes, so a snap turn does not sweep the gun through someone either.
//  - With meleebones the contact is also judged as in Chasm's VR framework (crates/runtime/src/combat.rs, MIT): the weapon followed
//    at 8 points along it, each swept from its place at this eye's last present; the speed is that point's, and it must be moving
//    into the body (towards the bone's axis): a weapon pulled out or slid along someone is no blow. meleestrength on|off (default
//    on): the damage at contact and the pulse grow with that speed (a quarter just over meleespeed, full at 7 m/s, the fist 6, at
//    most 1.5 times).
//  - meleeshow on|off (test, not saved): the capsules near the player (white torso, red head, yellow arms, green legs) and the weapon
//    (cyan, red for 0.4 s after a contact) drawn as dots over the eye pictures; meleebonedump [self]: the next frame's bodies and
//    their capsules in the log (self: with the player's own body, which is never hit).
//    meleeraysound on|off (saved, default on, 3 Nov): the game's own melee hit sound at a contact it would not make itself (a wall, or a body with no game melee pressed). From the weapon data's sndMeleeHitSoundID, type 15
//    (research\weapondata\reload_data.csv): 2040638 on the weapons carried on the pistol holster, 2276490191 on the others; melee weapons have none in the data, so the second, not heard yet.
//    meleecollide on|off (saved, default on, only with meleeray, 3 Nov; author 13:19; needs meleeray): a melee weapon's drawn blade lies against a wall or object
//    the game's ray finds on it, turned about the hand, which stays on the controller (melee_collide_rot, used by the made pose in FarCry5GunStill.cpp). Hits and speeds still use the blade's undeflected line.
//  - 12 Nov (author;
//    research\2026-11-12_melee_hold_react_collide.md), three switches:
//    meleegrip on|off (saved, default on; meleefist 60, meleeedge 0, meleegripweapons sig:fist:edge ...): the made pose from what the models are (the grip
//    at the origin, the length along +Z, tools\bonemap): the handle through the fist, the striking end out of its top; the blade's hit line and rays along
//    that length (they ran along +Y, across the weapon: research\2026-11-03_melee_blade_axis.md); in two hands the length along the hands. Off: as before.
//    meleereact on|off (saved, default on): a melee weapon at the made pose is the controller itself (the guns' steadying filter and weaponweight left
//    out: update_motion); a new hit 150 ms after the weapon left the body (not meleecool's 650 ms); with meleeray a swing that meets the world's own
//    shapes ends there (pulse by its speed) and a body behind a wall on a ray is not hit.
//    weaponcollide on|off (saved, default off until seen in the simulator; weaponcollidemax 0.35, weaponcollidebuzz 0.35, weaponcollidelen and
//    weaponcollidestatus tests): one more game ray a frame along the weapon in hand (weapon_collide_frame): a gun or bow drawn pushed back along its
//    barrel to rest its muzzle on what it meets, a melee weapon turned about the hand (meleecollide's turn, which this replaces while on).
//    melee fix 3 (author 12 Nov 13:36): no push-back any more: a gun or bow is turned about the hand like a melee weapon (rest_dir), the smallest
//    turn that rests its muzzle on the surface; head on (within 10 deg of square) its muzzle rises; the side is kept for the whole contact; at
//    most weaponcollideturn deg (default 75; weaponcollidemax is read and ignored). The ray, the shots, the aim, the laser are not touched.
//  - meleegripfist on|off (saved, default on; 15 Nov, author 21:19Z): a melee weapon in the hand is held by the empty hand's fist: the hand's palm at the grip pose on its controller
//    (emptyhandalign's), its centre on the controller's point (where the empty hand's centre goes), the weapon where the game holds it in that
//    hand. Off: melee fix 8's gun hold (the palm as on the last gun, its centre 12 cm off). The weapon's place in the game's hand is kept in
//    fc5vr_gunposes.txt ("mhold" lines) either way, so a session's first grab no longer shows the game's pose for 2 s.
//  - meleefist on|off (saved, default on; 23 Nov, author 07:39Z): the bare hand (no weapon,
//    or the weapon on the sling) presses the game's melee only while the weapon hand's grip is held, a closed fist; an open hand that meets
//    someone still pulses and logs "not pressed: the bare hand was open", and the game does no blow. Off: as before.
//  - Tests: meleetest x y z [s] (a body standing at x right, y ahead, z up from the game camera, for s seconds, default 20), meleepress
//    (the press now, straight ahead), meleelog N (N seconds of the bodies seen, one line a second).
#include "FarCry5Stereo.h"
#include "FarCry5GameRay.h"
#include "FarCry5MeleeDmgWatch.h"
#include "FarCry5GameSound.h"
#include "FarCry5GameWeapon.h"
#include "FarCry5Lua.h"
#include "FarCry5Record.h"
#include "FarCry5Bow.h"
#include "FarCry5Gesture.h"
#include "FarCry5Reload.h"

#include <d3d11_1.h>

#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace
{
// The closest points of segments p0-p1 and q0-q1: their distance, and where on each (s on p, t on q, 0-1).
float seg_seg(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1, float& s, float& t)
{
    const glm::vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float     a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    if (a < 1e-8f && e < 1e-8f) { s = t = 0.0f; return glm::length(r); }
    if (a < 1e-8f) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e < 1e-8f) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2), den = a * e - b * b;
            s = den > 1e-8f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    return glm::length((p0 + d1 * s) - (q0 + d2 * t));
}
constexpr float kCapLow = 0.2f, kCapHigh = 1.8f;   // the body capsule's axis above its root
// meleecollide / weaponcollide: a blade from the hand along dir (world) with a surface (normal n) on it s metres from the hand: the turn, in the axes W
// the made pose is built in, that lays it where its tip just rests on that plane (its height above the plane kept, the rest along the surface).
// melee fix 3: the one rule both use. A weapon of length L from the hand, its line dir meeting a surface (normal n, dn = dir.n < 0) s metres
// along: the hand's height over the surface is h = s (-dn); the direction that puts the tip on the surface with the least turn is
// d' = sqrt(1 - (h/L)^2) t + (-(h/L)) n, t the unit of dir along the surface (its part off n), here given (the caller picks it head on).
glm::vec3 rest_dir(const glm::vec3& n, float s, float dn, float L, const glm::vec3& t)
{
    const float h = std::min(s * -dn, L);
    return glm::normalize(t * std::sqrt(std::max(L * L - h * h, 0.0f)) - n * h);
}
glm::quat turn_in(const glm::vec3& dir, const glm::vec3& want, const glm::mat3& W)
{
    const glm::mat3 WT   = glm::transpose(W);
    const glm::vec3 from = glm::normalize(WT * dir), to = glm::normalize(WT * want), ax = glm::cross(from, to);
    return glm::normalize(glm::quat{ 1.0f + glm::dot(from, to), ax.x, ax.y, ax.z });
}
float turn_deg(const glm::quat& q) { return glm::degrees(2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f))); }
glm::quat blade_turn(const glm::vec3& dir, float s, const glm::vec3& n, float reach, const glm::mat3& W)
{
    const float dn = glm::dot(dir, n);
    if (!(s > 0.02f && s < reach && dn < -0.01f)) return glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3   t  = dir - n * dn;
    const float tl = glm::length(t);
    // Head on: up the surface
    t = tl > 0.05f ? t / tl : glm::normalize(glm::vec3{ 0.0f, 0.0f, 1.0f } - n * n.z + glm::vec3{ 1e-3f, 0.0f, 0.0f });
    const glm::vec3 want = rest_dir(n, s, dn, reach, t);
    const glm::mat3 WT   = glm::transpose(W);
    const glm::vec3 from = glm::normalize(WT * dir), to = glm::normalize(WT * want), ax = glm::cross(from, to);
    return glm::normalize(glm::quat{ 1.0f + glm::dot(from, to), ax.x, ax.y, ax.z });
}
// melee fix 3 (weaponcollide, a gun or a bow): the turn about the hand, in the axes W, that rests its muzzle (length L along dir) on the surface met s
// metres along it (normal n), at most max_deg (past that it stays at the limit). The side along the surface: dir's own while it leans 30 deg or more
// off square, or 10 deg or more and on the side kept (side, side_ok: kept for the whole contact, so no flip left and right); else the side kept;
// with none yet (head on, within 10 deg of square) the weapon's own up along the surface: the muzzle rises, as a person raises it. deg: the turn,
// how: 0 dir's side, 1 the side kept, 2 the muzzle raised.
glm::quat gun_turn(const glm::vec3& dir, float s, const glm::vec3& n, float L, const glm::mat3& W, float max_deg, glm::vec3& side, bool& side_ok, int& how, float& deg)
{
    deg               = 0.0f;
    const float dn    = glm::dot(dir, n);
    if (!(s > 0.02f && s < L && dn < -0.01f)) return glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    const glm::vec3 t  = dir - n * dn;
    const float     tl = glm::length(t);   // sin of the lean off square
    const glm::vec3 tu = tl > 1e-4f ? t / tl : glm::vec3{ 0.0f };
    glm::vec3       kept{ 0.0f };
    bool            kept_ok = false;
    if (side_ok) {
        kept = side - n * glm::dot(side, n);
        if (glm::length(kept) > 0.2f) {
            kept    = glm::normalize(kept);
            kept_ok = true;
        }
    }
    constexpr float kSquare = 0.1736f, kOver = 0.5f;   // sin 10 deg, sin 30 deg
    glm::vec3       use{ 0.0f };
    if (tl > kOver || (tl > kSquare && (!kept_ok || glm::dot(tu, kept) > 0.0f))) {
        use = tu;
        how = 0;
    } else if (kept_ok) {
        use = kept;
        how = 1;
    } else {
        glm::vec3 up = W * glm::vec3{ 0.0f, 1.0f, 0.0f };
        up -= n * glm::dot(up, n);
        if (glm::length(up) < 0.2f) {   // the weapon's up along n (rolled flat on the surface): its right
            up = W * glm::vec3{ 1.0f, 0.0f, 0.0f };
            up -= n * glm::dot(up, n);
        }
        use = glm::normalize(up);
        how = 2;
    }
    side             = use;
    side_ok          = true;
    glm::vec3   want = rest_dir(n, s, dn, L, use);
    const float a    = std::acos(std::clamp(glm::dot(dir, want), -1.0f, 1.0f));
    const float lim  = glm::radians(std::clamp(max_deg, 10.0f, 90.0f));
    if (a > lim) {
        const glm::vec3 ax = glm::cross(dir, want);
        if (glm::length(ax) > 1e-5f) want = glm::angleAxis(lim, glm::normalize(ax)) * dir;
    }
    deg = glm::degrees(std::min(a, lim));
    return turn_in(dir, want, W);
}
constexpr float kProbeBack = 0.10f;   // weaponcollide: the ray starts this far behind the weapon hand
// melee fix 2 (weaponcollide): a contact ends once the tip is this far clear of the surface for this many answers running, or after this long
// without any answer while the frames run.
constexpr float kClearMargin  = 0.03f;
constexpr int   kClearAnswers = 3;
constexpr int   kHoldMs       = 500;
int64_t ticks_now() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
// melee fix 12: a melee weapon in the hand always gets RT, its own swing, whatever meleebutton says (see melee_pad).
constexpr bool kEarlyPress = false;   // melee fix 12: no press before a contact (meleeearly read and kept, no effect)
// melee fix 13 (author 15 Nov 21:43Z). Kill switches for the builder (false: as fix 12).
// kPressAtContact: the game's melee is pressed in the update of the contact itself, not once the game's aim has reached the point hit (meleegate,
// meleewait: 11-130 ms, median 70, in his 21:35-22:17 log); the aim still turns onto the point hit and is kept there kAimHoldMs after the press, through
// the game's swing to its blow. kSoundAtContact: the game's own melee hit sound at a body contact too (it was only for walls and log-only hits).
// kEquipBareNoBlock: the 0.5 s after-equip block does not apply when the game itself has just put the bare hands in (its weapon put away after a hit:
// 2 of his 11 hits dropped so); the sling grab's 0.5 s stays.
constexpr bool kPressAtContact   = true;
constexpr bool kSoundAtContact   = true;
constexpr bool kEquipBareNoBlock = true;
constexpr int  kAimHoldMs        = 400;
// melee fix 16 (author 16 Nov 10:18-10:43 headset log).
// kRearmReverse: fix 6's "the tip turned back" now compares the tip's way with the way it had when its stroke began (kept through slow frames), not
// with the frame before: a real back-swing turns over several frames and passes under 0.6 meleespeed at the turn, so fix 6 never saw it and
// "this body was already hit in this swing" refused follow-ups 0.6-1.3 s after a hit (7 contacts in that log). The swing still ends only once the
// weapon has been off every body 80 ms after the turn; fix 14b's "leave the body by 10 cm" and the 150 ms meleecool stay.
// kQueuePress: a contact that comes while the button is still held, or before the game takes a new press (kPressGapMs after the last one: his
// 10:22:29.754/10:22:30.227 presses 473 ms apart both landed, 661 ms between the blows), waits as the one queued press and goes at the first
// moment allowed, the button released at least kReleaseMs before it (a press inside the last one's 120 ms merged into it: no new edge for the game).
constexpr bool kRearmReverse = true;
constexpr bool kQueuePress   = true;
constexpr int  kPressGapMs   = 450;
constexpr int  kReleaseMs    = 60;
const char* button_name(int b, bool melee_weapon)
{
    if (melee_weapon) b = 1;
    else if (b == 3) b = 0;
    return b == 1 ? "RT" : b == 2 ? "the stick click and RT" : "the stick click";
}

// meleebones
constexpr float kBoneRange = 4.0f;    // characters this near (across the ground) get their bones' capsules
constexpr float kJumpSpeed = 30.0f;   // m/s: the hand faster than any swing (tracking lost and found). The tip whips round much faster than the hand in a hard swing (author 3 Nov 13:31: tip 32.6, hand 10.8, a real swing), so it gets twice this
const char*     region_name(int r) { return r == 1 ? "head" : r == 2 ? "arm" : r == 3 ? "leg" : "torso"; }
// melee fix 6: the part at a point of the rest pose (it stands on its root, z up, x across the shoulders): the head above the neck and near the
// middle, the legs under the hips, the arms out to the sides, else the torso. Used for the contact point itself (a hit); a capsule's own region
// (its colour, the dump) stays the rule of its centre in fit_capsule.
int             region_at(const glm::vec3& p) { return p.z > 1.50f && std::abs(p.x) < 0.25f ? 1 : p.z < 0.85f ? 3 : std::abs(p.x) > 0.22f ? 2 : 0; }
// melee fix 7: a rest-pose vertex above kHeadZ and within kHeadX of the middle is the head's (13 Nov's lodge character: the face skinned to
// dozens of face bones, 88 to 127 capsules of r 0.02 at 1.55-1.61 m, the cap's slot at 1.73 m, no single skull slot).
constexpr float kHeadZ = 1.50f, kHeadX = 0.25f;
constexpr float kRayNear = 0.35f;     // melee fix 6: a ray's hit counts as a drawn character's within this of its capsules (fix 4: 0.15)
glm::mat4      from_rows(const float* f)   // a 3x4 row-major constant (CSceneTransform, a palette matrix)
{
    glm::mat4 m{ 1.0f };
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[c][r] = f[r * 4 + c];
    return m;
}
glm::vec3 xform(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }

// Part of a GPU buffer read back (a staging copy of [off, off + bytes); immediate context only).
bool read_range(ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT off, UINT bytes, std::vector<uint8_t>& out)
{
    if (!b || bytes == 0) return false;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    if (off >= d.ByteWidth) return false;
    bytes = std::min(bytes, d.ByteWidth - off);
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth      = (bytes + 15u) & ~15u;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Buffer* st = nullptr;
    if (dev) { dev->CreateBuffer(&sd, nullptr, &st); dev->Release(); }
    if (!st) return false;
    const D3D11_BOX box{ off, 0, 0, off + bytes, 1, 1 };
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + bytes);
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}

// One character draw's bones, fitted once in its rest pose: per palette slot a capsule (ends a, b and radius r, rest space).
struct BoneCap { int slot{ 0 }; glm::vec3 a{ 0.0f }, b{ 0.0f }; float r{ 0.1f }; int region{ 0 }; int n{ 0 }; glm::vec3 m{ 0.0f }, ax{ 0.0f, 0.0f, 1.0f }; float t0{ 0.0f }, t1{ 0.0f }; };
struct DrawScan
{
    bool                 ok{ false };
    std::vector<BoneCap> caps;
    glm::vec3            lo{ 1e9f }, hi{ -1e9f };   // its vertices' extent (rest space)
    int                  verts{ 0 };
    std::string          why;                       // not ok: why
    // melee fix 7: the head-region vertices of every slot (rest space, at most 600 kept evenly), how many, and the slot owning most of them
    std::vector<glm::vec3> head_pts;
    int                    head_n{ 0 }, head_slot{ -1 }, head_slot_n{ 0 }, merged{ 0 };
};
using ScanKey = std::tuple<void*, void*, UINT, UINT, INT>;   // index buffer, vertex buffer, count, start, base

float pct(std::vector<float>& v, float q)   // the q quantile (0-1); v is reordered
{
    if (v.empty()) return 0.0f;
    const size_t k = std::min(v.size() - 1, size_t(q * float(v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
    return v[k];
}

// A slot's capsule from the vertices it moves most: the centre, the main axis (power iteration on the spread), 5-95 % of the
// vertices' extent along it, 60 % of their distance from it as the radius.
bool fit_capsule(const std::vector<glm::vec3>& pts, BoneCap& c)
{
    if (pts.size() < 10) return false;
    glm::vec3 m{ 0.0f };
    for (const auto& p : pts) m += p;
    m /= float(pts.size());
    glm::mat3 cov{ 0.0f };
    for (const auto& p : pts) {
        const glm::vec3 d = p - m;
        cov += glm::outerProduct(d, d);
    }
    glm::vec3 ax{ 0.3f, 0.2f, 0.93f };
    for (int i = 0; i < 16; ++i) {
        const glm::vec3 n = cov * ax;
        if (glm::length(n) < 1e-12f) break;
        ax = glm::normalize(n);
    }
    std::vector<float> t, d;
    t.reserve(pts.size());
    d.reserve(pts.size());
    for (const auto& p : pts) {
        const glm::vec3 q  = p - m;
        const float     tt = glm::dot(q, ax);
        t.push_back(tt);
        d.push_back(glm::length(q - ax * tt));
    }
    const float t0 = pct(t, 0.05f), t1 = pct(t, 0.95f);
    const float r  = std::clamp(pct(d, 0.60f), 0.02f, 0.25f);
    float       a  = t0 + 0.5f * r, b = t1 - 0.5f * r;
    if (b < a) a = b = 0.5f * (t0 + t1);
    c.a = m + ax * a;
    c.b = m + ax * b;
    c.r  = r;
    c.n  = int(pts.size());
    c.m  = m;
    c.ax = ax;
    c.t0 = t0;
    c.t1 = t1;
    // The part by its rest place (the rest pose stands on its root, z up): head, legs, arms out to the sides, else the torso.
    // melee fix 7: the head line at 1.50 m as region_at (the head's own vertices now make one capsule, head_fit; a neck piece is the torso's).
    c.region = m.z > kHeadZ && std::abs(m.x) < kHeadX ? 1 : m.z < 0.85f ? 3 : std::abs(m.x) > 0.22f ? 2 : 0;
    return true;
}

// melee fix 6: one slot's capsules. A slot whose vertices spread over more than 0.6 m along its axis (13 Nov: a "leg" slot from 0.14 to 0.93 m
// above the root, r 0.17, reaching the hips) is cut along it into pieces of at most 0.4 m; one whose vertices lie in two lumps across its axis
// (both thighs or both shoes on one slot: 12 Nov's shoe draw 0.50 m wide, one bone) is cut in two; each piece is fitted again on its own
// vertices (at most 3 cuts deep). Fewer than 10 vertices make no capsule (fit_capsule).
void fit_slot(const std::vector<glm::vec3>& pts, int slot, std::vector<BoneCap>& out, int depth = 0)
{
    BoneCap c{};
    if (!fit_capsule(pts, c)) return;
    c.slot = slot;
    if (depth < 3 && pts.size() >= 20) {
        const float span = c.t1 - c.t0;
        if (span > 0.6f) {
            const int                           k = std::clamp(int(std::ceil(span / 0.4f)), 2, 4);
            std::vector<std::vector<glm::vec3>> piece(static_cast<size_t>(k));
            for (const auto& p : pts) {
                const float u = (glm::dot(p - c.m, c.ax) - c.t0) / std::max(span, 1e-3f);
                piece[size_t(std::clamp(int(u * float(k)), 0, k - 1))].push_back(p);
            }
            const size_t before = out.size();
            for (const auto& q : piece) fit_slot(q, slot, out, depth + 1);
            if (out.size() > before) return;
        } else if (c.r >= 0.10f) {
            // Two lumps across the axis: 2-means on the offsets from it, seeded with the farthest vertex and the one farthest from that.
            std::vector<glm::vec3> off;
            off.reserve(pts.size());
            for (const auto& p : pts) { const glm::vec3 q = p - c.m; off.push_back(q - c.ax * glm::dot(q, c.ax)); }
            size_t i0 = 0, i1 = 0;
            for (size_t i = 1; i < off.size(); ++i) if (glm::length(off[i]) > glm::length(off[i0])) i0 = i;
            for (size_t i = 1; i < off.size(); ++i) if (glm::length(off[i] - off[i0]) > glm::length(off[i1] - off[i0])) i1 = i;
            glm::vec3         c0 = off[i0], c1 = off[i1];
            std::vector<char> side(off.size(), 0);
            for (int it = 0; it < 8; ++it) {
                glm::vec3 s0{ 0.0f }, s1{ 0.0f };
                int       n0 = 0, n1 = 0;
                for (size_t i = 0; i < off.size(); ++i) {
                    side[i] = glm::length(off[i] - c1) < glm::length(off[i] - c0) ? 1 : 0;
                    if (side[i]) { s1 += off[i]; n1++; } else { s0 += off[i]; n0++; }
                }
                if (n0 == 0 || n1 == 0) break;
                c0 = s0 / float(n0);
                c1 = s1 / float(n1);
            }
            std::vector<glm::vec3> a, b;
            for (size_t i = 0; i < pts.size(); ++i) (side[i] ? b : a).push_back(pts[i]);
            BoneCap ca{}, cb{};
            // Split when each lump is a capsule of its own and the lumps stand apart (their centres further apart than their radii together).
            if (a.size() >= 10 && b.size() >= 10 && fit_capsule(a, ca) && fit_capsule(b, cb) && glm::length(c1 - c0) > ca.r + cb.r) {
                fit_slot(a, slot, out, depth + 1);
                fit_slot(b, slot, out, depth + 1);
                return;
            }
        }
    }
    out.push_back(c);
}
}

struct FarCry5Stereo::MeleeScanState
{
    std::mutex                  mtx;
    std::map<ScanKey, DrawScan> scans;
    int                         scan_next{ 0 };
    int                         scanned{ 0 }, failed{ 0 }, posed{ 0 }, no_palette{ 0 };
    std::set<uint32_t>          said;   // draws logged once (their scan)
    // meleeshow
    ID3D11View*                 view{ nullptr };
    ID3D11Texture2D*            tex{ nullptr };
    bool                        said_no_view{ false };
    uint64_t                    drawn{ 0 };
};

bool FarCry5Stereo::melee_command(const std::string& cmd, const std::string& arg, std::istream& rest, bool& persist)
{
    auto num = [&](float lo, float hi, float def) { return arg.empty() ? def : std::clamp(float(std::atof(arg.c_str())), lo, hi); };
    if (cmd == "meleehit") {
        const int m = arg == "log" ? 1 : arg == "on" || arg == "hit" ? 2 : arg == "off" ? 0 : std::clamp(std::atoi(arg.c_str()), 0, 2);
        m_mel_mode = m;
        persist    = true;
        spdlog::info("[Melee] meleehit {} ({})", m, m == 0 ? "off" : m == 1 ? "log only" : "the game's melee pressed at a hit");
        return true;
    }
    if (cmd == "ctrlglide") {
        // melee fix 5 (test, not saved): ctrlglide X Y Z MS: the test controller (testctrl's ctrl) moves evenly to (X, Y, Z) over MS ms (default 200).
        float y = 0.0f, z = 0.0f, ms = 200.0f, m2 = 0.0f;
        rest >> y >> z;
        if (rest >> m2 && m2 > 0.0f) ms = m2;
        std::lock_guard _{ m_dbg_glide_mtx };
        m_dbg_glide_from  = glm::vec3{ m_dbg_ctrl[0].load(), m_dbg_ctrl[1].load(), m_dbg_ctrl[2].load() };
        m_dbg_glide_to    = glm::vec3{ float(std::atof(arg.c_str())), y, z };
        m_dbg_glide_ms    = std::clamp(int(ms), 20, 3000);
        m_dbg_glide_t0    = std::chrono::steady_clock::now();
        m_dbg_glide_steps = 0;
        m_dbg_glide_on    = true;
        return true;
    }
    if (cmd == "meleebones") {
        m_mel_bones = arg != "off" && arg != "0";
        persist     = true;
        spdlog::info("[Melee] meleebones {}", m_mel_bones.load() ? "on (bodies from their own bones)" : "off (standing capsules)");
        return true;
    }
    if (cmd == "meleestrength") { m_mel_strength = arg != "off" && arg != "0"; persist = true; return true; }   // melee fix 10b: the pulse only (no damage is sent)
    if (cmd == "meleepad") { m_mel_pad = num(0.0f, 0.3f, 0.04f); persist = true; return true; }
    if (cmd == "meleeshow") {   // test, not saved
        m_mel_show = arg.empty() ? !m_mel_show.load() : arg != "off" && arg != "0";
        spdlog::info("[Melee] meleeshow {}{}", m_mel_show.load() ? "on" : "off", m_mel_show.load() && m_mel_mode.load() == 0 ? " (nothing to show while meleehit is off)" : "");
        return true;
    }
    if (cmd == "meleebonedump") {   // test: the next frame's bodies and their capsules (self: the player's own too)
        m_mel_dump_at = m_frame.load() + 2;
        m_mel_dump    = arg == "self" ? 2 : 1;
        if (m_mel_mode.load() == 0) spdlog::info("[Melee] meleebonedump: meleehit is off, so no bodies are noted (turn it to log only)");
        return true;
    }
    if (cmd == "meleereach") { m_mel_reach = num(0.0f, 1.5f, 0.55f); persist = true; return true; }
    if (cmd == "meleeradius") { m_mel_radius = num(0.1f, 0.8f, 0.30f); persist = true; return true; }
    if (cmd == "meleebutton") { m_mel_button = arg == "rt" ? 1 : arg == "both" ? 2 : arg == "r3" ? 0 : 3; persist = true; return true; }
    // melee fix 4 (author 12 Nov 20:08): the made pose and
    // every melee-only angle are deleted; a melee weapon in the hand is held by the same code as a gun. The old commands, found in saved
    // settings, are read and ignored (one line for all of them), and are no longer saved.
    if (cmd == "meleestill" || cmd == "meleepitch" || cmd == "meleeyaw" || cmd == "meleeroll" || cmd == "meleegrip" || cmd == "meleefist" || cmd == "meleeedge" ||
        cmd == "meleegripweapons") {
        static bool said = false;
        if (!said) {
            said = true;
            spdlog::info("[Melee] {} {}: removed in melee fix 4 (meleestill, meleepitch, meleeyaw, meleeroll, meleegrip, meleefist, meleeedge, meleegripweapons): "
                         "ignored; a melee weapon is held like a gun", cmd, arg);
        }
        return true;
    }
    if (cmd == "meleeblade") { m_mel_blade = num(0.1f, 1.5f, 0.70f); persist = true; return true; }
    if (cmd == "meleereact") {
        m_mel_react = arg != "off" && arg != "0";
        persist     = true;
        spdlog::info("[Melee] meleereact {}", m_mel_react.load() ? "on (a new hit 150 ms after the weapon left the body; a swing ends on a wall)" : "off (meleecool between hits)");
        return true;
    }
    if (cmd == "weaponcollide") {
        m_wc_on = arg == "on" || arg == "1";
        persist = true;
        spdlog::info("[Collide] weaponcollide {}", m_wc_on.load() ? "on" : "off");
        return true;
    }
    if (cmd == "weaponcollidemax") {   // melee fix 3: read (saved settings stay readable) and not used: guns turn now (weaponcollideturn)
        m_wc_max = num(0.05f, 0.60f, 0.35f);
        spdlog::info("[Collide] weaponcollidemax {:.2f} m: no longer used (a gun or bow is turned away from a surface now, at most weaponcollideturn {:.0f} deg)", m_wc_max.load(), m_wc_turn_max.load());
        return true;
    }
    if (cmd == "weaponcollideturn") {   // melee fix 3: deg (10 to 90, default 75), saved
        m_wc_turn_max = num(10.0f, 90.0f, 75.0f);
        persist       = true;
        spdlog::info("[Collide] weaponcollideturn {:.0f} deg: a gun or bow turned away from a surface at most this far (past it, it stays at the limit)", m_wc_turn_max.load());
        return true;
    }
    if (cmd == "weaponcollidebuzz") { m_wc_buzz = num(0.0f, 1.0f, 0.35f); persist = true; return true; }
    if (cmd == "weaponcollidelen") { m_wc_len = num(0.0f, 2.0f, 0.0f); return true; }   // test, not saved: the length looked along, whatever the weapon (0: its own)
    if (cmd == "weaponcollidestatus") {
        // (melee fix 1: one ray per game update; the last answer's surface, so a push that finds nothing says how far the nearest thing was)
        // (melee fix 2: the contact under way with the push's range since it settled or since the last status line, which starts the range again)
        float now_deg = 0.0f;
        {
            std::lock_guard _{ m_mel_mtx };
            now_deg = turn_deg(m_wc_gun_q);
        }
        spdlog::info("[Collide] weaponcollide {}: {} rays sent, {} answered, {} contacts; now turned {:.1f} deg (wanted {:.1f}), the length looked along {:.2f} m; the game's ray: {}; {} old answers dropped; the last answer: {}; {}",
                     m_wc_on.load() ? "on" : "off", m_wc_probes, m_wc_answers, m_wc_contacts, now_deg, m_wc_target, m_wc_len_now, fc5::gameray::status(), m_wc_stale,
                     m_wc_last_s >= 0.0f ? fmt::format("a surface {:.2f} m from the hand", m_wc_last_s) : std::string{ "nothing on the ray (it ends 3 cm past the length looked along)" },
                     m_wc_in ? "in a contact: " + weapon_collide_rest() : std::string{ "no contact now" });
        m_wc_range_new = true;
        return true;
    }
    if (cmd == "meleeauto") { m_mel_auto_m = num(0.3f, 2.0f, 0.62f); persist = true; return true; }
    if (cmd == "meleeweapons") {
        // meleeweapons +sig -sig ...: weapons set by hand as melee weapons (+) or not (-); meleeweapons auto: none set.
        std::lock_guard _{ m_mel_mtx };
        m_mel_force_on.clear();
        m_mel_force_off.clear();
        for (std::string t = arg; !t.empty(); t.clear(), rest >> t) {
            if (t.size() < 2 || (t[0] != '+' && t[0] != '-')) continue;
            const uint32_t sig = uint32_t(std::strtoul(t.c_str() + 1, nullptr, 10));
            if (sig != 0) (t[0] == '+' ? m_mel_force_on : m_mel_force_off).insert(sig);
        }
        persist = true;
        spdlog::info("[Melee] meleeweapons: {} set as melee weapons, {} as not", m_mel_force_on.size(), m_mel_force_off.size());
        return true;
    }
    if (cmd == "meleepressms") { m_mel_press_ms = int(num(30.0f, 600.0f, 120.0f)); persist = true; return true; }
    if (cmd == "meleewait") { m_mel_wait_ms = int(num(0.0f, 500.0f, 120.0f)); persist = true; return true; }
    if (cmd == "meleegate") { m_mel_gate = num(1.0f, 90.0f, 15.0f); persist = true; return true; }
    if (cmd == "meleecool") { m_mel_cool_ms = int(num(150.0f, 3000.0f, 650.0f)); persist = true; return true; }   // (with meleereact on: 150 ms at most)
    if (cmd == "meleebuzz") { m_mel_buzz_amp = num(0.0f, 1.0f, 1.0f); persist = true; return true; }
    if (cmd == "meleeaim") { m_mel_aim_on = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleeslew") { m_mel_slew = num(1.0f, 90.0f, 60.0f); persist = true; return true; }
    if (cmd == "meleeearly") { m_mel_early_ms = int(num(0.0f, 400.0f, 100.0f)); persist = true; return true; }
    // melee fix 10b (author 13 Nov 18:32Z): the mod sends no damage at all; the
    // game's own melee, pressed at the contact, deals every blow's damage. The old damage commands, found in saved settings, are read and ignored
    // (one line for all of them), and are no longer saved.
    if (cmd == "meleedamage" || cmd == "meleedamagehp" || cmd == "meleestim" || cmd == "meleeheadx" || cmd == "meleedamageself" || cmd == "meleedamageprobe" ||
        cmd == "meleedamagestatus") {
        static bool said = false;
        if (!said) {
            said = true;
            spdlog::info("[Melee] {} {}: removed in melee fix 10b (meleedamage, meleedamagehp, meleestim, meleeheadx, meleedamageself, meleedamageprobe, meleedamagestatus): "
                         "ignored; the game's own melee, pressed at the contact, does all the damage", cmd, arg);
        }
        return true;
    }
    if (cmd == "meleeray") { m_mel_ray = arg != "off" && arg != "0"; persist = true; spdlog::info("[Melee] meleeray {}", m_mel_ray.load() ? "on" : "off"); return true; }
    if (cmd == "meleegripfist") {
        m_mel_grip_fist = arg != "off" && arg != "0";
        persist         = true;
        spdlog::info("[Melee] meleegripfist {}", m_mel_grip_fist.load() ? "on (a melee weapon held by the empty hand's fist on the controller)" : "off (held by the gun hold)");
        return true;
    }
    if (cmd == "meleefist") {
        m_mel_fist = arg != "off" && arg != "0";
        persist    = true;
        spdlog::info("[Melee] meleefist {}", m_mel_fist.load() ? "on (the bare hand hits only as a fist: the weapon hand's grip held)" : "off (the open hand hits too, as before 23 Nov)");
        return true;
    }
    if (cmd == "meleecollide") { m_mel_collide = arg != "off" && arg != "0"; persist = true; spdlog::info("[Melee] meleecollide {}", m_mel_collide.load() ? "on" : "off"); return true; }
    if (cmd == "meleeraysound") { m_mel_ray_sound = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleewallbuzz") { m_mel_wall_buzz = num(0.0f, 1.0f, 0.4f); persist = true; return true; }
    if (cmd == "meleeraymask") { fc5::gameray::set_mask(uint32_t(std::strtoul(arg.c_str(), nullptr, 0))); return true; }
    if (cmd == "meleerayfine") { fc5::gameray::set_fine(arg != "off" && arg != "0"); return true; }   // test (default on): a ray must meet a body part, not only the outer capsule
    if (cmd == "meleerayflags") { fc5::gameray::set_flags(std::atoi(arg.c_str())); return true; }
    // meleedmgwatch on|off|status (melee fix 14 probe, test only, never saved, off at start): a hardware write watch on the hit NPC's health
    // after each press of the game's melee at a contact (FarCry5MeleeDmgWatch.cpp); "[MeleeDmgWatch]" lines.
    if (cmd == "meleedmgwatch") {
        if (arg == "status") spdlog::info("[MeleeDmgWatch] {}", fc5::meleedmgwatch::status());
        else {
            const bool on = arg != "off" && arg != "0";
            fc5::meleedmgwatch::set_on(on);
            // melee fix 14b: the game's entity lookups recorded while it is on, so a bones hit with no ray entity finds the body's entity at its root
            if (on) spdlog::info("[MeleeDmgWatch] entities recorded for bones hits (the game's entity lookup watched): {}", fc5::lua::damage_watch(true) ? "yes" : "no, not available");
            else fc5::lua::damage_watch(false);
        }
        return true;
    }
    if (cmd == "meleeraystatus") { spdlog::info("[Melee] meleeray {}: {}; {} walls met", m_mel_ray.load() ? "on" : "off", fc5::gameray::status(), m_mel_walls); return true; }
    if (cmd == "meleeraytest") {
        float v[6]{ 0.0f, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f };
        std::string mk = "-1";
        int         fl = -1;
        if (!arg.empty()) {
            v[0] = float(std::atof(arg.c_str()));
            rest >> v[1] >> v[2] >> v[3] >> v[4] >> v[5];
            rest >> mk >> fl;
        }
        const glm::vec3 f   = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
        const float     yaw = std::atan2(-f.x, f.y);
        const glm::vec3 ex{ std::cos(yaw), std::sin(yaw), 0.0f }, ey{ -std::sin(yaw), std::cos(yaw), 0.0f }, ez{ 0.0f, 0.0f, 1.0f };
        const glm::vec3 s = m_world_cam_pos + ex * v[0] + ey * v[1] + ez * v[2], d = ex * v[3] + ey * v[4] + ez * v[5];
        const float     ss[3]{ s.x, s.y, s.z }, dd[3]{ d.x, d.y, d.z };
        fc5::gameray::test(ss, dd, mk == "-1" ? -1 : int64_t(std::strtoul(mk.c_str(), nullptr, 0)), fl);
        return true;
    }
    if (cmd == "meleelog") { m_mel_log = int(num(0.0f, 600.0f, 10.0f)); return true; }
    if (cmd == "meleetest") {
        // meleetest x y z [s]: a test body; meleetest off: none.
        float x = 0.0f, y = 0.0f, z = 0.0f, s = 20.0f;
        int   n = 0;
        if (arg != "off" && !arg.empty()) {
            x = float(std::atof(arg.c_str()));
            if (rest >> y >> z) n = 3;
            if (n == 3 && !(rest >> s)) s = 20.0f;
        }
        std::lock_guard _{ m_mel_mtx };
        if (n >= 3) {
            // x right, y ahead, z up: turned with the game camera's heading now (world axes from then on).
            const glm::vec3 f   = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
            const float     yaw = std::atan2(-f.x, f.y);
            m_mel_test_rel      = glm::vec3{ std::cos(yaw), std::sin(yaw), 0.0f } * x + glm::vec3{ -std::sin(yaw), std::cos(yaw), 0.0f } * y + glm::vec3{ 0.0f, 0.0f, z };
            m_mel_test_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(std::clamp(s, 0.5f, 600.0f) * 1000.0f));
            spdlog::info("[Melee] test body at ({:.2f}, {:.2f}, {:.2f}) from the game camera for {:.0f} s", x, y, z, s);
        } else {
            m_mel_test_until = {};
            spdlog::info("[Melee] test body removed");
        }
        return true;
    }
    if (cmd == "meleepress") {
        // The press now, straight ahead of the game camera (no aim): the game's melee as the pad would send it.
        m_mel_press_until = ticks_now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(m_mel_press_ms.load())).count();
        m_mel_presses++;
        spdlog::info("[Melee] meleepress: {} held {} ms", button_name(m_mel_button.load(), m_mel_held_melee.load()), m_mel_press_ms.load());
        return true;
    }
    return false;
}

// melee fix 5 (test): ctrlglide's step, once per motion update with testctrl on: the test controller on the line from where the glide started
// to its end, by the time gone. One line when it arrives (how many updates it took), so a script can tell a swing from a one-frame jump.
void FarCry5Stereo::test_ctrl_glide_step()
{
    if (!m_dbg_glide_on.load()) return;
    std::lock_guard _{ m_dbg_glide_mtx };
    if (!m_dbg_glide_on.load()) return;
    const float     ms = float(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_dbg_glide_t0).count()) / 1000.0f;
    const float     u  = std::clamp(ms / float(std::max(1, m_dbg_glide_ms)), 0.0f, 1.0f);
    const glm::vec3 p  = m_dbg_glide_from + (m_dbg_glide_to - m_dbg_glide_from) * u;
    m_dbg_ctrl[0]      = p.x;
    m_dbg_ctrl[1]      = p.y;
    m_dbg_ctrl[2]      = p.z;
    m_dbg_glide_steps++;
    if (u >= 1.0f) {
        m_dbg_glide_on = false;
        const float len = glm::length(m_dbg_glide_to - m_dbg_glide_from);
        spdlog::info("[Test] ctrlglide: at ({:.2f}, {:.2f}, {:.2f}) after {:.0f} ms in {} updates, {:.2f} m moved ({:.1f} m/s)", p.x, p.y, p.z, ms, m_dbg_glide_steps, len,
                     ms > 0.0f ? 1000.0f * len / ms : 0.0f);
    }
}

// meleestill: a melee weapon by hand (meleeweapons), else by where the game holds its hand (its grip point, gripbyhand).
bool FarCry5Stereo::melee_weapon(uint32_t sig)
{
    if (sig == 0) return false;
    {
        std::lock_guard _{ m_mel_mtx };
        if (m_mel_force_on.contains(sig)) return true;
        if (m_mel_force_off.contains(sig)) return false;
    }
    // The game's own data first (3 Nov; class 0 and carried on a melee holster): a machete or pipe is one from its first frame; otherwise where the game holds its hand.
    if (const auto* w = fc5::gameweapon::info(sig)) {
        if (w->cls == 0 && w->holster == 5) return true;
    }
    std::lock_guard _{ m_weapon_cal_mtx };
    const auto it = m_grip_hand.find(sig);
    return it != m_grip_hand.end() && it->second.ok && glm::length(it->second.c) > m_mel_auto_m.load();
}

// melee fix 3: a melee weapon's length from the fist for the sling: a long tool (TwoHanded, a name with "_Long", a spear) 1.30 m, else 0.85 m.
float FarCry5Stereo::melee_sling_len(uint32_t sig) const
{
    if (const auto* w = fc5::gameweapon::info(sig); w && w->name) {
        const std::string n{ w->name };
        if (n.find("TwoHanded") != std::string::npos || n.find("_Long") != std::string::npos || n.find("Spear") != std::string::npos) return 1.30f;
    }
    return 0.85f;
}

// melee fix 3 (author 12 Nov 13:31): a melee weapon on the sling, in the sling's axes (the controller's place there; x right, y up, z back as the
// sling is turned by slingrot): its length (the model's +Z, from the grip at its origin to the striking end) hangs from the fist, handle up and heavy
// end down, leaning out to the weapon hand's side and back just enough that the tip stays kSlungDrop under the fist (the fist is about 1.1 m above
// the ground standing): a pipe or bat about 20 deg, the long shovel or hoe about 52 deg, at least 10 deg. Its +Y toward the weapon hand's side.
// melee fix 11 (author 13 Nov 20:41Z): the
// fist (the handle, the grab zone's centre) stays on the sling's place, but the length now leans across the back toward the other side: its tip
// kSlungDrop under the fist, kSlungAcross toward the body's centre line (past the spine: the fist is 0.17 m right of the head on his sling) and
// the rest of the length back, behind the legs. His 0.85 m machete: tip from (0.30 right, -1.35 up, -0.28 forward of the head) to (-0.13, -1.27,
// -0.36); a 1.30 m tool from (0.63, -1.35, -0.94) to (-0.13, -1.27, -1.06).
glm::quat FarCry5Stereo::melee_sling_rot(uint32_t sig)
{
    constexpr float kSlungDrop   = 0.72f;   // (0.80 before fix 11) the tip still about 0.38 m above the ground standing
    constexpr float kSlungAcross = 0.30f;
    float           pos[3], rot[3];
    fc5::gesture::get_sling_place(pos, rot);
    const float     m = m_left_handed.load() ? -1.0f : 1.0f;
    const glm::mat3 S = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * rot[0]), glm::vec3{ 0, 1, 0 }) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(rot[1]), glm::vec3{ 1, 0, 0 }) *
                                   glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * rot[2]), glm::vec3{ 0, 0, -1 }) };   // as sling_pose (FarCry5Gesture.cpp)
    const float     len    = melee_sling_len(sig);
    const float     drop   = std::min(kSlungDrop, len * std::cos(glm::radians(10.0f)));
    const float     h      = std::sqrt(std::max(0.0f, len * len - drop * drop));   // the length's part off the vertical
    const float     across = std::min(h, kSlungAcross);
    const float     back   = std::sqrt(std::max(0.0f, h * h - across * across));
    const glm::vec3 F      = glm::normalize(glm::vec3{ -m * across, -drop, back });   // body axes: down, across toward the centre, back
    const glm::vec3 side{ m, 0.0f, 0.0f };
    const glm::vec3 y    = glm::normalize(side - F * glm::dot(side, F));
    const glm::vec3 x    = glm::cross(y, F);
    return glm::normalize(glm::quat_cast(glm::transpose(S) * glm::mat3{ x, y, F }));
}

// melee fix 3: a point of the slung weapon (c_rel: it less the point the weapon pass puts on the sling, game camera axes) right / up / forward of the
// head, m, from the motion update's sling frame (the weapon pass's model correction for this weapon included).
bool FarCry5Stereo::sling_grip_rel(uint32_t sig, const glm::vec3& c_rel, glm::vec3& rel)
{
    float cy = m_model_yaw.load(), cp = m_model_pitch.load();
    {
        std::lock_guard _{ m_weapon_cal_mtx };
        if (auto it = m_weapon_cal.find(sig); it != m_weapon_cal.end()) {
            cy += it->second.first;
            cp += it->second.second;
        }
    }
    const glm::mat3 corr = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(cy), glm::vec3{ 0, 1, 0 }) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(cp), glm::vec3{ 1, 0, 0 }) };
    std::lock_guard _{ m_slg_mtx };
    if (m_frame.load() - m_slg_frame > 10) return false;
    const glm::vec3 g = glm::vec3{ m_slg_k[3] } + glm::mat3{ m_slg_k } * (corr * c_rel);
    const glm::vec3 b = glm::transpose(m_slg_body) * (g - m_slg_head);
    rel               = glm::vec3{ b.x, b.y, -b.z };
    return true;
}

// weaponcollide (Stereo.h): once per present. One ray of the game's physics from 10 cm behind the weapon hand along the weapon to its tip, sent for
// the next game tick; the answer to the last one sets what is drawn: a gun or bow pushed back along its barrel (m_wc_push, read by the weapon
// pass once per game update so both eyes agree), a melee weapon turned about the hand (m_mel_col_q, read by the made pose). Only what is drawn
// moves: the controller's pose, the aim, the shots' line, the reload's zones and the melee's hit line are untouched.
// melee fix 1: a weapon counts for meleegrip, meleereact and weaponcollide once it has been the weapon in hand for 0.25 s, and not while a
// spawnhand's draw is under way (Stereo.h).
bool FarCry5Stereo::weapon_settled(uint32_t sig) const
{
    return sig != 0 && sig == m_weapon_sig.load() && std::chrono::steady_clock::now() - m_wsig_t >= std::chrono::milliseconds(250) && !fc5::lua::spawn_drawing();
}

// melee fix 1: weaponcollide's state starts over (nothing pushed, nothing turned, the waiting answer dropped). The let-go line names the weapon
// the contact was with, in that contact's own unit.
// melee fix 2: the contact under way (or just ended) for the log: how long, the push's range while resting, what was held through.
std::string FarCry5Stereo::weapon_collide_rest() const
{
    const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_wc_met_t).count();
    std::string r    = fmt::format("{:.1f} s on it", secs);
    if (!m_wc_in_melee && m_wc_hi >= m_wc_lo) r += fmt::format(", turned {:.1f} to {:.1f} deg while resting", m_wc_lo, m_wc_hi);   // (melee fix 3: was the push in cm)
    return r + fmt::format(", {} gaps in the frames and {} answers without a surface held through", m_wc_gaps, m_wc_held);
}

void FarCry5Stereo::weapon_collide_reset(const char* why, uint32_t sig)
{
    if (m_wc_in) spdlog::info("[Collide] weapon {} let go of the surface ({}): it was turned up to {:.1f} deg; {}", m_wc_sig != 0 ? m_wc_sig : sig, why, m_wc_peak, weapon_collide_rest());
    m_wc_clear_n  = 0;
    m_wc_in       = false;
    m_wc_in_melee = false;
    m_wc_peak     = 0.0f;
    m_wc_push     = 0.0f;   // at once: a reload's zones and the sling's place are the controller's
    m_wc_target   = 0.0f;
    m_wc_col_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    m_wc_gun_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    m_wc_side_ok  = false;
    m_wc_wait     = false;
    m_wc_have_P   = false;
    m_wc_last_s   = -1.0f;
    fc5::gameray::Hit drop{};
    fc5::gameray::take_probe(drop);
    std::lock_guard _{ m_mel_mtx };
    m_wc_gun_q = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };   // at once (melee fix 3)
    if (m_wc_on.load()) m_mel_col_q = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
}

void FarCry5Stereo::weapon_collide_frame()
{
    // melee fix 1: once per game update. In stereo the game's script thread answers one ray per pair of presents, so the ray sent at one
    // eye's present was overwritten by the other's before the game took it (12 Nov run: 3319 sent, 1659 answered), and the hand's world
    // place differs by a few cm between the two presents: only the pair's first present sends and reads.
    if (!afw_mode() && (m_frame.load() & 1)) return;
    const auto  now    = std::chrono::steady_clock::now();
    const float dt_raw = std::chrono::duration<float>(now - m_wc_t).count();
    const float dt     = std::clamp(dt_raw, 0.0f, 0.1f);
    m_wc_t             = now;
    const bool on      = m_wc_on.load();
    const uint32_t sig   = m_weapon_sig.load();
    const bool     armed = !m_unarmed && sig != 0 && !m_mo.slung && !m_mo.kept && weapon_settled(sig);
    // melee fix 4: a melee weapon's ray runs along its length as drawn this frame (melee_drawn_line); it turns at a surface as a gun does (gun_turn),
    // and only with weaponcollide on (the melee hit's own meleecollide turn is deleted).
    const bool     melee = armed && melee_weapon(sig);
    // Nothing during a reload step (manual or the game's), a throw, a bow being drawn, on the sling, in a seat, on a mounted gun, in menus or cutscenes.
    const bool reload = fc5::reload::hides_game_reload() || fc5::reload::mag_in_left_hand() || fc5::reload::pump_held() || fc5::reload::hand_hold_now() || fc5::reload::game_busy() ||
                        fc5::reload::button_reload();
    const bool throwing = m_rb_down.load() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle;
    const bool bow_busy = m_mo.bow && int(fc5::bow::phase()) >= int(fc5::bow::Phase::Nocked);
    const bool can = on && armed && m_motion.load() && m_mo.valid && m_mo.ctrl_valid && !reload && !throwing && !bow_busy && !m_bow_slung_now && !m_menu_screen_on && !m_cine_on && !in_vehicle() &&
                     !m_in_vehicle.load() && !mounted_now();
    auto leave = [&](const char* why) {
        if (m_wc_in) spdlog::info("[Collide] weapon {} let go of the surface ({}): it was turned up to {:.1f} deg; {}", m_wc_sig != 0 ? m_wc_sig : sig, why, m_wc_peak, weapon_collide_rest());
        m_wc_in      = false;
        m_wc_peak    = 0.0f;
        m_wc_clear_n = 0;
    };
    if (!can) {
        if (on || m_wc_push.load() != 0.0f || m_wc_in)
            weapon_collide_reset(!on ? "weaponcollide off" : reload ? "a reload" : !armed ? "no weapon held for 0.25 s in the hand" : "not on foot with the weapon held", sig);
        m_wc_sig = 0;
        return;
    }
    // melee fix 1: another weapon, or the hand somewhere else at once (over 1.5 m between two updates: a teleport, a load) or after a gap
    // (no update for 0.5 s): what was held against is gone.
    const glm::vec3 Pn = m_mo.ctrl_pos_world;
    if (sig != m_wc_sig) weapon_collide_reset("another weapon", sig);
    else if (m_wc_have_P && glm::length(Pn - m_wc_P) > 1.5f) weapon_collide_reset("the hand jumped: a teleport or a load", sig);
    else if (dt_raw > 0.25f) {
        // melee fix 2: a gap in the frames is not the surface gone. On 12 Nov (testruns\2026-11-12_melee2) each "no update for 0.5 s" let-go came
        // right after a picture capture, which holds the presents 0.6 to 0.7 s ("[VR] pacing: step max 666.67 ms"); a loading hitch does the same.
        // The hand is where it was (a jump is caught above), so the contact and its push are kept; the answer waiting is from before the gap
        // (dropped below as old), and the wait for a fresh one starts now.
        m_wc_in_t = now;
        if (m_wc_in) m_wc_gaps++;
    }
    m_wc_sig    = sig;
    m_wc_P      = Pn;
    m_wc_have_P = true;
    // The weapon's line from the hand, world: a melee weapon's length as drawn, else the barrel; its length: the blade (meleeblade), a gun's muzzle
    // when it lies 10 cm to 1.5 m ahead of the hand along the barrel, else by its kind (handgun 0.22, bow 0.45, long gun 0.70 m).
    const glm::vec3 P = m_mo.ctrl_pos_world;
    const glm::mat3 W = m_mo.ctrl_rot_world * (m_frame.load() - m_hand_map.frame <= 4 ? m_hand_map.corr : glm::mat3{ 1.0f });
    glm::vec3       dir{ 0.0f };
    float           len = 0.0f;
    if (melee) {
        glm::vec3 o_w{ 0.0f };
        if (!melee_drawn_line(sig, o_w, dir)) return;   // no drawn pose this frame: no ray
        len = m_mel_blade.load();
    } else {
        if (glm::length(m_mo.barrel_dir) < 0.5f) return;
        dir             = glm::normalize(m_mo.barrel_dir);
        const float muz = glm::dot(m_mo.barrel_origin - P, dir);
        const auto* w   = fc5::gameweapon::info(sig);
        // melee fix 1: the muzzle here is the mod's one barrel offset for every gun (m_barrel_off, 0.30 m ahead of the grip), not this
        // gun's own: right or short for a long gun, 8 to 10 cm too long for a handgun (the P226 read 0.29 to 0.32 m on 12 Nov). A handgun
        // by the game's data (carried on the pistol holster, held in one hand) is looked along 0.22 m.
        const bool handgun = w && w->holster == 1 && w->one_handed;
        len                = m_mo.bow ? 0.45f : handgun ? 0.22f : muz > 0.10f && muz < 1.5f ? muz : (w && (w->holster == 1 || w->one_handed)) ? 0.22f : 0.70f;
    }
    if (m_wc_len.load() > 0.0f) len = m_wc_len.load();
    // The answer to the last ray (a game tick old at most; none for 150 ms: nothing is held against).
    // melee fix 3: a gun's m_wc_target is its turn wanted, deg (it was the push, m).
    const float max_deg = std::clamp(m_wc_turn_max.load(), 10.0f, 90.0f);
    fc5::gameray::Hit hit{};
    bool              got = fc5::gameray::take_probe(hit);
    // melee fix 1: an answer to a ray sent more than 6 frames or 150 ms ago is of another place: dropped.
    if (got && (!m_wc_wait || m_frame.load() - m_wc_sent_frame > 6 || now - m_wc_sent_t > std::chrono::milliseconds(150))) {
        got = false;
        m_wc_stale++;
    }
    if (got) {
        m_wc_wait = false;
        m_wc_answers++;
        m_wc_in_t          = now;
        const float before = m_wc_target;
        float       s      = 0.0f;
        m_wc_last_s        = hit.any ? hit.along - kProbeBack : -1.0f;
        // melee fix 2: a contact is kept until the tip is clear of the surface by kClearMargin (the ray looks that far past the tip) for
        // kClearAnswers answers running. A surface within the margin keeps the contact with nothing pushed or turned (the weapon lies just
        // off it); an answer with nothing on the ray keeps the last push or turn until the third one in a row.
        if (!m_wc_in) m_wc_side_ok = false;   // melee fix 3: a new contact picks its side afresh
        if (hit.any) {
            s             = hit.along - kProbeBack;   // from the hand
            m_wc_clear_n  = 0;
            const glm::vec3 nrm{ hit.nx, hit.ny, hit.nz };
            // melee fix 4: a surface nearer than 5 cm to the hand is the hand's own (the body, a held item): never a turn.
            if (s < 0.05f) {
                if (!m_wc_near_said) {
                    m_wc_near_said = true;
                    spdlog::info("[Collide] weapon {}: a surface {:.2f} m from the hand (entity {:x}) left out: nearer than 5 cm, the hand's own (logged once)", sig, s, hit.entity);
                }
                m_wc_target   = 0.0f;
                m_wc_gun_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                m_wc_col_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
            } else {
                float deg     = 0.0f;
                m_wc_gun_want = gun_turn(dir, s, nrm, m_wc_len_now, W, max_deg, m_wc_side, m_wc_side_ok, m_wc_side_how, deg);
                m_wc_target   = deg;
                m_wc_col_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
            }
        } else if (m_wc_in && ++m_wc_clear_n < kClearAnswers) {
            m_wc_held++;   // (the last push or turn stays)
        } else {
            m_wc_target   = 0.0f;
            m_wc_col_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
            m_wc_gun_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        }
        const bool in = m_wc_in ? (hit.any || m_wc_clear_n < kClearAnswers) : m_wc_target > 0.5f;
        if (in && !m_wc_in) {
            m_wc_met_t   = now;
            m_wc_clear_n = 0;
            m_wc_lo      = 1e9f;
            m_wc_hi      = 0.0f;
            m_wc_gaps    = 0;
            m_wc_held    = 0;
            m_wc_contacts++;
            // The pulse on meeting it, stronger the faster the weapon goes in (the push's growth, to full at 2 m/s); a melee weapon's own wall pulse
            // (meleeray) is not doubled.
            // (melee fix 3: a gun's target is in deg now: full at 200 deg/s)
            const float v = dt > 1e-3f ? std::max(0.0f, m_wc_target - before) / dt / 100.0f : 0.0f;
            const float a = m_wc_buzz.load() * (0.5f + 0.5f * std::min(1.0f, v / 2.0f));
            if (a > 0.0f && !(melee && m_mel_mode.load() > 0 && m_mel_ray.load()) && a > m_mel_buzz.load()) m_mel_buzz = a;
            if (now - m_wc_said > std::chrono::milliseconds(300)) {
                m_wc_said = now;
                // melee fix 3: a gun's turn, its axis in the controller's axes (x right, y up, z back) and how its side was chosen.
                const glm::quat& gq = m_wc_gun_want;
                const glm::vec3  ga = glm::length(glm::vec3{ gq.x, gq.y, gq.z }) > 1e-6f ? glm::normalize(glm::vec3{ gq.x, gq.y, gq.z }) : glm::vec3{ 0.0f };
                spdlog::info("[Collide] weapon {} ({}) met a surface {:.2f} m from the hand along its {:.2f} m (entity {:x}, normal ({:.2f}, {:.2f}, {:.2f})): {}", sig, melee ? "melee" : m_mo.bow ? "bow" : "gun", s,
                             m_wc_len_now, hit.entity, hit.nx, hit.ny, hit.nz,
                             fmt::format("turned {:.1f} deg about the hand (at most {:.0f}), axis ({:.2f}, {:.2f}, {:.2f}) in the controller's axes (x right, y up, z back), {}", m_wc_target, max_deg, ga.x, ga.y, ga.z,
                                                 m_wc_side_how == 2 ? "head on: the muzzle raised" : m_wc_side_how == 1 ? "toward the side kept" : "toward the side it leans"));
            }
        }
        if (in && !m_wc_in) {
            m_wc_in_melee = melee;   // the peak's unit: deg for a melee weapon, m for a gun
            m_wc_peak     = 0.0f;
        }
        if (in) m_wc_peak = std::max(m_wc_peak, m_wc_target);   // deg (melee fix 3; fix 4: a melee weapon turns as a gun)
        if (!in && m_wc_in) leave("clear of it");
        m_wc_in = in;
    } else if (now - m_wc_in_t > std::chrono::milliseconds(m_wc_in ? kHoldMs : 150)) {
        // (melee fix 2: in a contact the last push or turn is held through 0.5 s without an answer, with the frames running)
        m_wc_target   = 0.0f;
        m_wc_col_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        m_wc_gun_want = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        if (m_wc_in) leave("no answer from the game's ray for 0.5 s");
    }
    // In at once, out eased (about 80 ms).
    const float ease = 1.0f - std::exp(-dt / 0.03f);
    (void)ease;
    {
        // melee fix 3: a gun or bow turned, eased in over about 40 ms and out over about 120 ms (95 % of the way), no push.
        m_wc_push = 0.0f;
        std::lock_guard _{ m_mel_mtx };
        const float cur  = turn_deg(m_wc_gun_q), want = turn_deg(m_wc_gun_want);
        const float k    = 1.0f - std::exp(-dt / (want > cur ? 0.013f : 0.040f));
        m_wc_gun_q       = (want < 0.05f && cur < 0.2f) ? glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f } : glm::normalize(glm::slerp(m_wc_gun_q, m_wc_gun_want, k));
        // melee fix 2: the range while the weapon rests (from 0.3 s after it met the surface, or since the last weaponcollidestatus): the turn, deg.
        if (m_wc_range_new.exchange(false)) {
            m_wc_lo = 1e9f;
            m_wc_hi = 0.0f;
        }
        if (m_wc_in && now - m_wc_met_t > std::chrono::milliseconds(300)) {
            const float d = turn_deg(m_wc_gun_q);
            m_wc_lo       = std::min(m_wc_lo, d);
            m_wc_hi       = std::max(m_wc_hi, d);
        }
        m_mel_col_q = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    }
    // The next ray: from the hand's own place along the weapon's own line (the controller's: m_mo, never the pushed-back picture), to
    // kClearMargin past the tip (melee fix 2).
    const glm::vec3 a = P - dir * kProbeBack, b = P + dir * (len + kClearMargin);
    fc5::gameray::submit_probe(fc5::gameray::Seg{ a.x, a.y, a.z, b.x, b.y, b.z, false });
    m_wc_len_now    = len;
    m_wc_wait       = true;
    m_wc_sent_frame = m_frame.load();
    m_wc_sent_t     = now;
    m_wc_probes++;
}

// The weapon pass: this game update's turn of a gun or bow, controller's axes (latched once per update: the two eyes of a stereo pair draw the
// same gun). melee fix 3: it replaces the push along the barrel.
bool FarCry5Stereo::weapon_collide_turn(int frame, glm::quat& q)
{
    const int key = afw_mode() ? frame : (frame & ~1);
    if (key != m_wc_applied_key) {
        m_wc_applied_key = key;
        std::lock_guard _{ m_mel_mtx };
        m_wc_applied_q = m_wc_on.load() ? m_wc_gun_q : glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    }
    q = m_wc_applied_q;
    return std::abs(q.w) < 0.99999f;
}

void FarCry5Stereo::draw_weapon_collide_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool on = m_wc_on.load();
    // Author 13 Nov 19:37Z ("make it just one parameters"): the "Walls and objects (test)" section is this one row; the turn (75 deg) and
    // the pulse (0.35) are fixed (weaponcollideturn, weaponcollidebuzz: no longer settings, typed they act for a session).
    if (ImGui::Checkbox("Weapons collide with the environment##weaponcollide", &on)) {
        m_wc_on = on;
        keep("weaponcollide", on ? "weaponcollide on" : "weaponcollide off");
    }
    hint("Weapons pushed into a wall or the ground rest against it instead of going through. Your aim and shots don't change.");
}

// world_character_draw: a skinned character draw of the world pass; obj = its object transform (VS b1, 3x4 rows, world), cbs = its VS
// constant buffers 0-4 (b2: the vertex scale, b3: the palette).
void FarCry5Stereo::melee_note_character(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, const float* obj, ID3D11Buffer* const* cbs)
{
    const glm::vec3 origin{ obj[3], obj[7], obj[11] };
    const glm::vec3 rel = origin - m_world_cam_pos;
    const float     horiz = std::sqrt(rel.x * rel.x + rel.y * rel.y);
    const bool self = horiz < 0.5f && rel.z < 0.3f;   // the player's own (its root 4 cm from the camera, 1.6 m below; author 25 Oct 11:18 log: a 0.33 m away hit by his own hand)
    // melee fix 2 (meleeshow on, test): the nearest character drawn this frame, however far (the bodies below stop at 20 m, so the 12 Nov
    // runs on lodge-night could not tell where the scene's NPC stood).
    if (!self && horiz < 400.0f && m_mel_show.load()) {
        const int       f = m_frame.load();
        std::lock_guard _{ m_mel_mtx };
        if (f != m_mel_far_frame) {
            if (m_mel_far_frame >= 0 && m_mel_far_n > 0) {
                m_mel_far_last       = m_mel_far_cur;
                m_mel_far_last_n     = m_mel_far_n;
                m_mel_far_last_frame = m_mel_far_frame;
            }
            m_mel_far_n     = 0;
            m_mel_far_frame = f;
        }
        if (m_mel_far_n++ == 0 || horiz < glm::length(glm::vec2{ m_mel_far_cur })) m_mel_far_cur = rel;
    }
    if (horiz > 20.0f || std::abs(rel.z) > 6.0f) return;   // (20 m: meleelog shows who is around)
    const int  dump = m_mel_dump.load();
    if (self && dump < 2) return;
    const int frame = m_frame.load();

    // meleebones: this draw's bones as capsules (its rest-pose fit, posed by its palette now).
    std::vector<MeleeCap> caps;
    MeleeHeadSrc          head{};          // melee fix 7: this draw's head-region vertices, posed by their main slot
    bool                  waiting = false; // melee fix 7: a draw of this body not read yet
    if (m_mel_bones.load() && horiz < kBoneRange && count > 0 && ctx && cbs && cbs[3]) {
        m_keep_bones = true;   // the palettes' CPU copies (note_object_constants)
        if (!m_mel_scan) m_mel_scan = new MeleeScanState{};
        auto&              st = *m_mel_scan;
        std::vector<float> pal;
        float              scale = 0.0f;
        {
            std::lock_guard _{ m_obj_mtx };
            if (auto it = m_bone_data.find(cbs[3]); it != m_bone_data.end() && it->second.size() >= 3072) pal = it->second;
            if (cbs[2]) if (auto it = m_obj_consts.find(cbs[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
        }
        ID3D11Buffer* vb = nullptr;
        UINT          stride = 0, voff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT   ib_fmt{};
        UINT          ib_off = 0;
        ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
        const ScanKey key{ ib, vb, count, start, base };
        std::lock_guard sl{ st.mtx };
        auto            si = st.scans.find(key);
        if (si == st.scans.end() && frame >= st.scan_next && !fc5::rec::is_deferred(ctx) && vb && ib && st.scans.size() < 800) {
            // Once per draw (one every 10 frames: each is a GPU read): its indices, then the vertices they use.
            st.scan_next = frame + 10;
            DrawScan sc{};
            ID3D11InputLayout* il = nullptr;
            ctx->IAGetInputLayout(&il);
            std::vector<LayoutElem> layout;
            {
                std::lock_guard _{ m_ps_mtx };
                if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
            }
            if (il) il->Release();
            auto find_el = [&](const char* sem) -> const LayoutElem* {
                for (const auto& e : layout) if (_stricmp(e.semantic.c_str(), sem) == 0 && e.index == 0 && e.slot == 0) return &e;
                return nullptr;
            };
            const LayoutElem* pe = find_el("POSITION");
            const LayoutElem* be = find_el("BLENDINDICES");
            const LayoutElem* we = find_el("BLENDWEIGHT");
            const bool        p16 = pe && pe->format == DXGI_FORMAT_R16G16B16A16_SINT && scale > 0.0f;
            const bool        p32 = pe && (pe->format == DXGI_FORMAT_R32G32B32_FLOAT || pe->format == DXGI_FORMAT_R32G32B32A32_FLOAT);
            D3D11_PRIMITIVE_TOPOLOGY topo{};
            ctx->IAGetPrimitiveTopology(&topo);
            const bool i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
            std::vector<uint8_t> ibd, vbd;
            if (!pe || !(p16 || p32)) sc.why = pe ? fmt::format("position format {}{}", int(pe->format), scale > 0.0f ? "" : " (no vertex scale)") : "no position in its layout";
            else if (!be) sc.why = "no blend indices";
            else if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || stride < 8 || stride > 128 || count > 600000) sc.why = fmt::format("topology {}, stride {}, {} indices", int(topo), stride, count);
            else if (!read_range(ctx, ib, ib_off + start * (i32 ? 4u : 2u), count * (i32 ? 4u : 2u), ibd)) sc.why = "index read failed";
            else {
                uint32_t lo = 0xffffffffu, hi = 0;
                const size_t nidx = ibd.size() / (i32 ? 4 : 2);
                for (size_t k = 0; k < nidx; ++k) {
                    uint32_t idx = 0;
                    std::memcpy(&idx, ibd.data() + k * (i32 ? 4 : 2), i32 ? 4 : 2);
                    lo = std::min(lo, idx);
                    hi = std::max(hi, idx);
                }
                const int64_t first = int64_t(lo) + base;
                const uint64_t bytes = (uint64_t(hi) - lo + 1) * stride;
                if (nidx == 0 || first < 0 || bytes > (16u << 20)) sc.why = fmt::format("vertex range {}-{} ({} B)", lo, hi, bytes);
                else if (!read_range(ctx, vb, voff + UINT(first) * stride, UINT(bytes), vbd)) sc.why = "vertex read failed";
                else {
                    std::map<int, std::vector<glm::vec3>> per;
                    std::set<uint32_t>                    seen;
                    std::vector<glm::vec3>                head_all;   // melee fix 7
                    std::map<int, int>                    head_per;
                    for (size_t k = 0; k < nidx; ++k) {
                        uint32_t idx = 0;
                        std::memcpy(&idx, ibd.data() + k * (i32 ? 4 : 2), i32 ? 4 : 2);
                        if (!seen.insert(idx).second) continue;
                        const size_t vo = size_t(idx - lo) * stride;
                        if (vo + stride > vbd.size()) continue;
                        const uint8_t* v = vbd.data() + vo;
                        glm::vec3      p{ 0.0f };
                        if (p16) {
                            int16_t q[3];
                            std::memcpy(q, v + pe->offset, 6);
                            p = glm::vec3{ float(q[0]), float(q[1]), float(q[2]) } * scale;
                        } else {
                            std::memcpy(&p, v + pe->offset, 12);
                        }
                        const uint8_t* bi    = v + be->offset;
                        int            heavy = bi[0];
                        if (we && we->offset + 4 <= stride) {
                            float          w[4]{};
                            const uint8_t* wp = v + we->offset;
                            if (we->format == DXGI_FORMAT_B8G8R8A8_UNORM) { w[0] = wp[2]; w[1] = wp[1]; w[2] = wp[0]; w[3] = wp[3]; }
                            else for (int c = 0; c < 4; ++c) w[c] = wp[c];
                            int hk = 0;
                            for (int c = 1; c < 4; ++c) if (w[c] > w[hk]) hk = c;
                            heavy = bi[hk];
                        }
                        // melee fix 7: a head-region vertex goes to the body's one head (head_fit), whichever face bone it rides on
                        if (heavy < 256 && p.z > kHeadZ && std::abs(p.x) < kHeadX) { head_all.push_back(p); head_per[heavy]++; }
                        else per[heavy].push_back(p);
                        sc.lo = glm::min(sc.lo, p);
                        sc.hi = glm::max(sc.hi, p);
                        sc.verts++;
                    }
                    // melee fix 7: a slot with too few vertices to stand alone (under 10) joins the nearest slot of the same region that has
                    // enough (by their centres), else the nearest at all: no vertex is dropped from a body.
                    auto centre = [](const std::vector<glm::vec3>& v) { glm::vec3 m{ 0.0f }; for (const auto& p : v) m += p; return v.empty() ? m : m / float(v.size()); };
                    auto reg    = [](const glm::vec3& m) { return m.z > kHeadZ && std::abs(m.x) < kHeadX ? 1 : m.z < 0.85f ? 3 : std::abs(m.x) > 0.22f ? 2 : 0; };
                    std::vector<int> few;
                    for (const auto& [slot, pts] : per) if (pts.size() < 10) few.push_back(slot);
                    for (const int s : few) {
                        const glm::vec3 ms = centre(per[s]);
                        int             best = -1, best_any = -1;
                        float           bd = 1e9f, bd_any = 1e9f;
                        for (const auto& [slot, pts] : per) {
                            if (slot == s || pts.size() < 10 || slot >= 256) continue;
                            const float d = glm::length(centre(pts) - ms);
                            if (d < bd_any) { bd_any = d; best_any = slot; }
                            if (reg(centre(pts)) == reg(ms) && d < bd) { bd = d; best = slot; }
                        }
                        const int to = best >= 0 ? best : best_any;
                        if (to < 0) continue;
                        auto& dst = per[to];
                        dst.insert(dst.end(), per[s].begin(), per[s].end());
                        per[s].clear();
                        sc.merged++;
                    }
                    for (const auto& [slot, pts] : per)
                        if (slot < 256 && !pts.empty()) fit_slot(pts, slot, sc.caps);   // melee fix 6 (was one capsule a slot, however spread)
                    sc.head_n = int(head_all.size());
                    for (const auto& [slot, n] : head_per) if (n > sc.head_slot_n) { sc.head_slot_n = n; sc.head_slot = slot; }
                    const size_t step = std::max<size_t>(1, head_all.size() / 600);
                    for (size_t k = 0; k < head_all.size(); k += step) sc.head_pts.push_back(head_all[k]);
                    sc.ok = !sc.caps.empty() || sc.head_n >= 10;
                    if (!sc.ok) sc.why = fmt::format("{} vertices, no slot with 10 or more", sc.verts);
                }
            }
            sc.ok ? st.scanned++ : st.failed++;
            if (st.said.size() < 64 && st.said.insert(count).second) {
                if (sc.ok) {
                    int nr[4]{};
                    for (const auto& c : sc.caps) nr[c.region]++;
                    spdlog::info("[Melee] meleebones: draw {} ({} vertices, rest {:.2f}..{:.2f} m high, {:.2f} m wide): {} bones ({} head, {} torso, {} arm, {} leg); {} head-region vertices "
                                 "(slot {} owns {}) for the body's one head; {} small slots merged",
                                 count, sc.verts, sc.lo.z, sc.hi.z, sc.hi.x - sc.lo.x, sc.caps.size(), nr[1], nr[0], nr[2], nr[3], sc.head_n, sc.head_slot, sc.head_slot_n, sc.merged);
                } else {
                    spdlog::info("[Melee] meleebones: draw {} not read ({}): its body stays the standing capsule", count, sc.why);
                }
            }
            si = st.scans.emplace(key, std::move(sc)).first;
        } else if (si == st.scans.end() && !fc5::rec::is_deferred(ctx) && vb && ib && st.scans.size() < 800) {
            waiting = true;   // melee fix 7: it will be read in turn (one draw every 10 frames)
        }
        if (vb) vb->Release();
        if (ib) ib->Release();
        if (si != st.scans.end() && si->second.ok) {
            if (pal.size() < 3072) st.no_palette++;
            else {
                st.posed++;
                const glm::mat4 Om = from_rows(obj);
                for (const auto& c : si->second.caps) {
                    if (size_t(c.slot) * 12 + 12 > pal.size()) continue;
                    const glm::mat4 M = Om * from_rows(pal.data() + size_t(c.slot) * 12);
                    // melee fix 6: with its rest ends and the bone's 3x3 undone (a hit's part from the contact point in the rest pose).
                    const glm::mat3 L{ M };
                    const float     det = glm::determinant(L);
                    caps.push_back(MeleeCap{ xform(M, c.a) - m_world_cam_pos, xform(M, c.b) - m_world_cam_pos, c.r, c.region, c.a, c.b,
                                             std::abs(det) > 1e-9f ? glm::inverse(L) : glm::mat3{ 1.0f }, std::abs(det) > 1e-9f, c.slot, c.n });
                }
                const auto& sc = si->second;
                if (sc.head_n >= 10 && sc.head_slot >= 0 && size_t(sc.head_slot) * 12 + 12 <= pal.size())
                    head = MeleeHeadSrc{ &sc, glm::mat4{ glm::vec4{ 1, 0, 0, 0 }, glm::vec4{ 0, 1, 0, 0 }, glm::vec4{ 0, 0, 1, 0 }, glm::vec4{ -m_world_cam_pos, 1.0f } } * Om * from_rows(pal.data() + size_t(sc.head_slot) * 12) };
            }
        }
    }

    std::lock_guard _{ m_mel_mtx };
    if (frame != m_mel_cur_frame) {
        if (m_mel_cur_frame >= 0 && frame > m_mel_cur_frame) {
            melee_finish_bodies(m_mel_cur);   // melee fix 7
            m_mel_bodies       = m_mel_cur;
            m_mel_bodies_frame = m_mel_cur_frame;
            m_mel_self         = m_mel_self_cur;
            m_mel_self_root    = m_mel_self_root_cur;
            m_mel_self_frame   = m_mel_cur_frame;
        }
        m_mel_cur.clear();
        m_mel_self_cur.clear();
        m_mel_self_root_cur = glm::vec3{ 0.0f, 0.0f, 1e9f };
        m_mel_cur_frame = frame;
    }
    if (self) {   // meleebonedump self: the player's own capsules, logged only (from its root: the lowest of its draws' origins)
        m_mel_self_cur.insert(m_mel_self_cur.end(), caps.begin(), caps.end());
        if (rel.z < m_mel_self_root_cur.z) m_mel_self_root_cur = rel;
        return;
    }
    for (auto& b : m_mel_cur) {
        if (glm::length(glm::vec2{ b.rel.x - rel.x, b.rel.y - rel.y }) < 0.45f && std::abs(b.rel.z - rel.z) < 2.0f) {
            b.rel.z = std::min(b.rel.z, rel.z);   // pieces hung higher on the body (a hat, a holster) stand over the root
            if (b.caps.size() < 256) b.caps.insert(b.caps.end(), caps.begin(), caps.end());
            if (head.scan && b.heads.size() < 16) b.heads.push_back(head);
            b.draws++;
            b.waiting += waiting ? 1 : 0;
            return;
        }
    }
    if (m_mel_cur.size() < 48) {
        m_mel_cur.push_back(MeleeBody{ rel, false, std::move(caps) });
        if (head.scan) m_mel_cur.back().heads.push_back(head);
        m_mel_cur.back().draws   = 1;
        m_mel_cur.back().waiting = waiting ? 1 : 0;
    }
}

// melee fix 7: a body's one head and its readiness, once its frame is whole. The head: all the head-region vertices of all its draws (the tiny
// face slots included), one capsule fitted to them (its radius 0.08 to 0.14 m), posed by the draw slot owning most of them (the face bones
// ride on the head); fitted once per set of draws. A body with a draw not read yet keeps its standing capsule (its capsules held back).
void FarCry5Stereo::melee_finish_bodies(std::vector<MeleeBody>& bodies)
{
    struct HeadFit { bool ok{ false }; BoneCap c{}; };
    static std::mutex                                     mtx;
    static std::map<std::vector<const void*>, HeadFit>   cache;
    for (auto& b : bodies) {
        if (b.test) continue;
        if (b.waiting > 0) {
            b.held = int(b.caps.size());
            b.caps.clear();
            continue;
        }
        if (b.heads.empty()) continue;
        std::vector<const void*> key;
        for (const auto& h : b.heads) key.push_back(h.scan);
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        const MeleeHeadSrc* anchor = nullptr;
        int                 most   = -1;
        for (const auto& h : b.heads) {
            const auto* sc = static_cast<const DrawScan*>(h.scan);
            if (sc->head_slot_n > most) { most = sc->head_slot_n; anchor = &h; }
        }
        HeadFit f{};
        {
            std::lock_guard _{ mtx };
            if (auto it = cache.find(key); it != cache.end()) f = it->second;
            else {
                std::vector<glm::vec3> pts;
                for (const void* k : key) {
                    const auto* sc = static_cast<const DrawScan*>(k);
                    pts.insert(pts.end(), sc->head_pts.begin(), sc->head_pts.end());
                }
                int n = 0;
                for (const void* k : key) n += static_cast<const DrawScan*>(k)->head_n;
                if (fit_capsule(pts, f.c)) {
                    f.c.r      = std::clamp(f.c.r, 0.08f, 0.14f);
                    f.c.region = 1;
                    f.c.n      = n;
                    f.c.slot   = static_cast<const DrawScan*>(anchor->scan)->head_slot;
                    f.ok       = true;
                    spdlog::info("[Melee] head of a body: {} head-region vertices from {} draws, one capsule r {:.2f}, rest ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f}), posed by slot {} "
                                 "({} of them)", n, key.size(), f.c.r, f.c.a.x, f.c.a.y, f.c.a.z, f.c.b.x, f.c.b.y, f.c.b.z, f.c.slot, most);
                }
                if (cache.size() < 256) cache.emplace(key, f);
            }
        }
        if (!f.ok || !anchor) continue;
        const glm::mat3 L{ anchor->M };
        const float     det = glm::determinant(L);
        b.caps.push_back(MeleeCap{ xform(anchor->M, f.c.a), xform(anchor->M, f.c.b), f.c.r, 1, f.c.a, f.c.b, std::abs(det) > 1e-9f ? glm::inverse(L) : glm::mat3{ 1.0f },
                                   std::abs(det) > 1e-9f, f.c.slot, f.c.n });
    }
}

// The game's aim for the press: the point hit, while aiming and pressing.
bool FarCry5Stereo::melee_aim(glm::vec3& want) const
{
    if (m_mel_state == 0 || !m_mel_aim_on.load() || m_mel_mode.load() < 2) return false;
    const glm::vec3 d = m_mel_target - m_world_cam_pos;
    if (glm::length(d) < 0.05f) return false;
    want = glm::normalize(d);
    return true;
}

void FarCry5Stereo::melee_hit_frame()
{
    const auto now  = std::chrono::steady_clock::now();
    const int  mode = m_mel_mode.load();
    // melee fix 12: a grab from the sling (or the end of twohandkeep): the weapon came into the hand now. A press still waiting from before it is dropped.
    {
        const bool away = m_mo.slung || m_mo.kept;
        if (m_mel_was_slung && !away) {
            m_mel_grab_t = now;
            if (m_mel_state == 1) {
                m_mel_state  = 0;
                m_mel_queued = false;
                spdlog::info("[Melee] the game's melee not pressed: the weapon was taken from the sling before the press");
            }
        }
        m_mel_was_slung = away;
    }
    // The press: once the aim is on the point hit (or after meleewait), held meleepressms, the aim kept 150 ms more.
    // melee fix 13 (kPressAtContact): pressed at the contact itself (press_go from the hit below), the aim kept kAimHoldMs after the press.
    auto press_go = [&](bool at_contact) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_state_t).count();
        m_mel_press_until = ticks_now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(m_mel_press_ms.load())).count();
        m_mel_presses++;
        const auto last = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_press_t).count();
        spdlog::info("[Melee] pressed {} ms after the hit ({}; the game's aim {:.1f} deg from the point hit): {} for {} ms{}", since,
                     m_mel_queued ? "the queued press, at the first moment the game takes a new one (melee fix 16)"
                     : at_contact ? "at the contact, the aim turning onto the point hit meanwhile" : m_mel_aimed ? "the game's aim on the point hit" : m_mel_aim_on.load() ? "meleewait: the aim not there yet" : "meleeaim off",
                     m_mel_aim_err, button_name(m_mel_button.load(), m_mel_held_melee.load()), m_mel_press_ms.load(),
                     last < 3000 ? fmt::format("; {} ms after the last press", last) : std::string{});
        m_mel_press_t = now;   // melee fix 16
        m_mel_queued  = false;
        // melee fix 13: when the game's blow lands (FarCry5GameRay.cpp: the hit entity's health read each game update for 1.5 s, the first change logged)
        // melee fix 14b: with the body's root, so a bones hit without the ray's entity finds it there (never a watch on entity 0)
        fc5::gameray::watch_damage(m_mel_hit_ent, m_mel_hit_hp, std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_state_t).count(), m_mel_hit_root_ok,
                                   m_mel_hit_root.x, m_mel_hit_root.y, m_mel_hit_root.z, m_mel_hit_src);
        m_mel_state   = 2;
        m_mel_state_t = now;
    };
    // melee fix 16: whether the game takes a new press now: kPressGapMs after the last press began, the button released kReleaseMs.
    auto press_ready = [&]() {
        if (!kQueuePress) return true;
        const int64_t rel = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(kReleaseMs)).count();
        return now - m_mel_press_t >= std::chrono::milliseconds(kPressGapMs) && ticks_now() >= m_mel_press_until.load() + rel;
    };
    // melee fix 16: pressed now, or queued (one: a later contact replaces the waiting one's target) to the first moment the game takes it.
    auto press_or_queue = [&]() {
        if (press_ready()) { press_go(true); return; }
        m_mel_queued = true;
        m_mel_state  = 1;
        if (now - m_mel_queue_said > std::chrono::milliseconds(300)) {
            m_mel_queue_said = now;
            spdlog::info("[Melee] press queued: the contact came {} ms after the last press (the button {}); it goes {} ms after that press, the button released {} ms first (melee fix 16)",
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_press_t).count(), ticks_now() < m_mel_press_until.load() ? "still held" : "released",
                         kPressGapMs, kReleaseMs);
        }
    };
    if (m_mel_state == 1) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_state_t).count();
        if (m_mel_queued) {
            if (press_ready()) press_go(true);
        } else if ((m_mel_aimed || !m_mel_aim_on.load() || since >= m_mel_wait_ms.load()) && press_ready()) press_go(false);
    } else if (m_mel_state == 2 && now - m_mel_state_t > std::chrono::milliseconds(kPressAtContact ? std::max(kAimHoldMs, m_mel_press_ms.load() + 150) : m_mel_press_ms.load() + 150)) {
        m_mel_state = 0;
    }
    const int e = m_frame.load() & 1;   // this present's eye
    // mountedweapon (FarCry5Mounted.cpp): no melee from swinging the controller while a mounted weapon is used.
    if (mode == 0 || !m_motion.load() || !m_mo.valid || !m_mo.ctrl_valid || m_menu_screen_on || m_cine_on || in_vehicle() || mounted_now()) {
        m_mel_have[0] = m_mel_have[1] = false;
        if (mode == 0) m_mel_state = 0;
        return;
    }
    // The bodies of the last whole frame (or the frame being drawn, if newer), and the test body.
    std::vector<MeleeBody> bodies;
    std::vector<MeleeCap>  self_caps;
    glm::vec3              self_root{ 0.0f, 0.0f, -1.6f };
    int                    bodies_frame = -100;
    {
        std::lock_guard _{ m_mel_mtx };
        const int frame = m_frame.load();
        if (frame - m_mel_bodies_frame <= 6) bodies = m_mel_bodies;
        bodies_frame = m_mel_bodies_frame;
        if (m_mel_dump.load() >= 2) {
            self_caps = m_mel_self;
            if (m_mel_self_root.z < 1e8f) self_root = m_mel_self_root;
        }
        if (now < m_mel_test_until) bodies.push_back(MeleeBody{ m_mel_test_rel, true });
        // melee fix 2 (meleeshow on, test): once a second, the nearest character drawn: how far, and how many degrees to the right of the
        // game camera's heading (the way the left stick walks; meleetest's axes), for a script to turn and walk by.
        if (m_mel_show.load() && now - m_mel_far_said >= std::chrono::seconds(1)) {
            m_mel_far_said = now;
            const bool cur = m_mel_far_frame < frame && frame - m_mel_far_frame <= 6 && m_mel_far_n > 0;
            if (cur || frame - m_mel_far_last_frame <= 6) {
                const glm::vec3 r   = cur ? m_mel_far_cur : m_mel_far_last;
                const glm::vec3 f   = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
                const float     yaw = std::atan2(-f.x, f.y);
                const float     rt = r.x * std::cos(yaw) + r.y * std::sin(yaw), ah = -r.x * std::sin(yaw) + r.y * std::cos(yaw);
                spdlog::info("[Melee] nearest character: {:.1f} m, {:.0f} deg to the right of the game camera's heading, its root {:.1f} m {} the camera (world offset ({:.2f}, {:.2f}, {:.2f})); {} character draws in the frame",
                             glm::length(glm::vec2{ r }), glm::degrees(std::atan2(rt, ah)), std::abs(r.z), r.z < 0.0f ? "below" : "above", r.x, r.y, r.z, cur ? m_mel_far_n : m_mel_far_last_n);
            } else {
                spdlog::info("[Melee] nearest character: none drawn within 400 m (only characters in the view are drawn: turn)");
            }
        }
    }
    // meleebonedump: the bodies of a frame noted after the command, with their capsules.
    if (const int dump = m_mel_dump.load(); dump > 0 && bodies_frame >= m_mel_dump_at) {
        m_mel_dump = 0;
        spdlog::info("[Melee] meleebonedump: frame {}, {} bodies within 20 m (meleebones {}; {} draws read, {} not, {} posed, {} without a palette copy)", bodies_frame, bodies.size(),
                     m_mel_bones.load() ? "on" : "off", m_mel_scan ? m_mel_scan->scanned : 0, m_mel_scan ? m_mel_scan->failed : 0, m_mel_scan ? m_mel_scan->posed : 0,
                     m_mel_scan ? m_mel_scan->no_palette : 0);
        auto list = [&](const char* who, const glm::vec3& root, const std::vector<MeleeCap>& caps, const std::string& draws = {}) {
            float top = -1e9f, bottom = 1e9f;
            int   nr[4]{};
            for (const auto& c : caps) {
                top    = std::max({ top, c.a.z - root.z + c.r, c.b.z - root.z + c.r });
                bottom = std::min({ bottom, c.a.z - root.z - c.r, c.b.z - root.z - c.r });
                nr[std::clamp(c.region, 0, 3)]++;
            }
            spdlog::info("[Melee]   {} root ({:.2f}, {:.2f}, {:.2f}) from the camera: {} capsules ({} head, {} torso, {} arm, {} leg){}", who, root.x, root.y, root.z, caps.size(), nr[1], nr[0], nr[2], nr[3],
                         (caps.empty() ? std::string{ ": the standing capsule" } : fmt::format(", {:.2f} to {:.2f} m above the root", bottom, top)) + draws);
            for (size_t k = 0; k < caps.size() && k < 200; ++k) {   // melee fix 5: all (was 40: 13 Nov's 123 capsules, 88 of them the head's)
                const auto& c = caps[k];
                // melee fix 6: and its slot, its vertices and its ends in the rest pose (x across, z up from the root)
                spdlog::info("[Melee]     {:<5} ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f}) above the root, r {:.2f}; slot {}, {} vertices, rest ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f})",
                             region_name(c.region), c.a.x - root.x, c.a.y - root.y, c.a.z - root.z, c.b.x - root.x, c.b.y - root.y, c.b.z - root.z, c.r, c.slot, c.n, c.ra.x, c.ra.y, c.ra.z,
                             c.rb.x, c.rb.y, c.rb.z);
            }
        };
        // melee fix 7: and how many of its draws were read, how many still wait (their capsules held back: the standing capsule meanwhile)
        for (const auto& b : bodies)
            if (!b.test)
                list("body", b.rel, b.caps, fmt::format("; its draws: {}, {} read, {} still waiting{}", b.draws, b.draws - b.waiting, b.waiting,
                                                      b.waiting ? fmt::format(" ({} capsules held back until all are read)", b.held) : std::string{}));
        if (dump >= 2) list("the player's own body (never hit)", self_root, self_caps);
    }
    const bool use_ray = m_mel_ray.load();
    // melee fix 4 (the 21:06 crash: the ray's entity 1d39cb8a65c08661 had no drawn character at the hit, part -1): the drawn characters (not the
    // test body) kept for the ray's guard: a ray's entity counts only with its hit point near one of them (fix 6: near_drawn, kRayNear).
    std::vector<MeleeBody> drawn_chars;
    const bool             geo = m_mel_show.load();   // melee fix 5: meleeshow's hit-line line needs them too
    if (use_ray || geo)
        for (const auto& b : bodies)
            if (!b.test) drawn_chars.push_back(b);
    const float char_r = m_mel_radius.load();
    // melee fix 6: a contact's part from the contact point itself: the point on the capsule's surface nearest p (on_axis: the axis point at t),
    // taken back into the rest pose (rest ends ra, rb; the bone's 3x3 undone), named by its rest height and sideways place (region_at).
    // melee fix 7: the capsule's own part first (no names for a character's palette slots; its region from its rest centre stands for its bone
    // chain): the body's one head capsule is the head, an arm capsule the arm (fix 6's rule named the side of the chest "arm": the rest pose
    // is an A pose, upper arms from (0.21, 1.33) down to (0.53, 1.18) m, the chest out to |x| 0.29); only between torso and leg capsules
    // does the contact point's rest height decide (a slot across the hips): under 0.85 m the leg, else the torso.
    auto contact_part = [](const MeleeCap& c, float t, const glm::vec3& on_axis, const glm::vec3& p) {
        if (!c.rest || c.region == 1 || c.region == 2) return c.region;
        {
            glm::vec3   off = p - on_axis;
            const float l   = glm::length(off);
            if (l > c.r && l > 1e-6f) off *= c.r / l;
            return (c.ra + (c.rb - c.ra) * t + c.inv * off).z < 0.85f ? 3 : 0;
        }
        glm::vec3   off = p - on_axis;
        const float l   = glm::length(off);
        if (l > c.r && l > 1e-6f) off *= c.r / l;
        return region_at(c.ra + (c.rb - c.ra) * t + c.inv * off);
    };
    // melee fix 6 (was fix 4's 15 cm): a ray's hit point belongs to a drawn character when it lies within kRayNear (0.35 m) of one of its
    // capsules, or inside its standing capsule (meleeradius round its root's axis, kCapLow to kCapHigh up). 13 Nov: the game's own shapes of a
    // real character were hit 0.16 to 0.22 m off the mod's capsules (refused 9 times of 11); the 12 Nov crash's hit was a map object 0.50 m
    // from the camera with no character's capsule logged near it. region: the part of the nearest capsule at the hit (contact_part), -1 none.
    struct Near { float d{ 1e9f }; int region{ -1 }; bool standing{ false }; bool ok() const { return standing || d <= kRayNear; } };
    auto near_drawn = [&](const glm::vec3& at) {   // at: from the game camera
        Near n{};
        for (const auto& b : drawn_chars) {
            const glm::vec3 s0 = b.rel + glm::vec3{ 0.0f, 0.0f, kCapLow }, s1 = b.rel + glm::vec3{ 0.0f, 0.0f, kCapHigh };
            const float     ts = std::clamp(glm::dot(at - s0, s1 - s0) / std::max(1e-6f, glm::dot(s1 - s0, s1 - s0)), 0.0f, 1.0f);
            const float     sd = glm::length(at - (s0 + (s1 - s0) * ts)) - char_r;
            if (sd <= 0.0f) n.standing = true;
            if (b.caps.size() >= 3) {
                for (const auto& cp : b.caps) {
                    const glm::vec3 ab = cp.b - cp.a;
                    const float     t  = std::clamp(glm::dot(at - cp.a, ab) / std::max(1e-6f, glm::dot(ab, ab)), 0.0f, 1.0f);
                    const glm::vec3 on = cp.a + ab * t;
                    const float     d  = glm::length(at - on) - cp.r;
                    if (d < n.d) { n.d = d; n.region = contact_part(cp, t, on, at); }
                }
            } else if (sd < n.d) {
                n.d      = sd;
                n.region = -1;
            }
        }
        return n;
    };
    // melee fix 9 (the builder's per-part run testruns\2026-11-13_melee_fix7d: with meleeray on no leg hit at all and 2 of 4 head hits, while the
    // bones found them): the drawn bodies stay with the ray on. A bones contact counts when this pass's ray answer has no character and no wall
    // nearer than the contact; the two paths are paired by the body nearest the ray's hit, one hit per body per swing (m_mel_swing_roots); each
    // hit line says its source: ray, bones or both. (Before: with meleeray on the bones were dropped and only the test body stayed.)
    // A body's root in the world, and whether this swing has already hit it (within 0.6 m across the ground of a root hit).
    auto swing_has = [&](const glm::vec3& root_w) {
        for (const auto& r0 : m_mel_swing_roots)
            if (glm::length(glm::vec2{ root_w - r0 }) < 0.6f) return true;
        return false;
    };
    // The drawn body nearest a point (from the game camera) across the ground, within 1 m; -1 none.
    auto body_at = [&](const glm::vec3& at) {
        int   nb = -1;
        float nd = 1.0f;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].test) continue;
            const float d = glm::length(glm::vec2{ at - bodies[i].rel });
            if (d < nd) { nd = d; nb = int(i); }
        }
        return nb;
    };
    // The weapon's segment from the game camera: the stock end behind the hand to the tip ahead (the bare hand: a fist).
    const bool      armed = !m_unarmed && !m_mo.slung && !m_mo.kept;   // twohandkeep: the right hand holds nothing
    // melee fix 4: a melee weapon's blade is its length as drawn this frame: from the drawn model's origin (the grip) along its +Z, both from the
    // weapon pass's own transform of the game's pose (melee_drawn_line), so the hit line, the swept points and the game's rays follow the picture.
    glm::vec3       blade_o{ 0.0f }, blade_d{ 0.0f };
    const bool      blade = armed && melee_weapon(m_weapon_sig.load()) && weapon_settled(m_weapon_sig.load()) && melee_drawn_line(m_weapon_sig.load(), blade_o, blade_d);
    m_mel_held_melee      = armed && melee_weapon(m_weapon_sig.load());
    glm::vec3       hand  = ((m_mo.slung || m_mo.kept) ? m_mo.hand_pos_world : m_mo.ctrl_pos_world) - m_world_cam_pos;
    glm::vec3       dir   = glm::length(m_mo.barrel_dir) > 0.5f ? glm::normalize(m_mo.barrel_dir) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    if (blade) {
        hand = blade_o - m_world_cam_pos;
        dir  = blade_d;
    }
    // meleereact: a swing that met a wall is over until the weapon slows down; a body behind a wall on a ray is not hit.
    const bool react = m_mel_react.load();
    fc5::gameray::set_wall_blocks(react);
    const float     reach = !armed ? 0.0f : blade ? m_mel_blade.load() : m_mel_reach.load();
    const float     back  = !armed ? 0.0f : blade ? 0.08f : 0.25f;
    const glm::vec3 p0    = hand - dir * back;   // the bare hand: a fist round the controller
    const glm::vec3 p1    = hand + dir * reach;
    // The speed against this eye's last present, in the body's axes (the controller as the player moves it: a snap turn, walking or the
    // game turning the body is no swing); the faster of the hand and the tip. The swept test starts from the segment then, in today's axes.
    const glm::mat3 B  = m_mo.body_rot, Bt = glm::transpose(B);
    const glm::vec3 bh = B * hand, b0 = B * p0, b1 = B * p1;
    bool            have = m_mel_have[e];
    const float     dt   = std::chrono::duration<float>(now - m_mel_t[e]).count();
    if (have && !(dt > 1e-3f && dt < 0.2f)) have = false;
    float     v  = 0.0f;
    glm::vec3 q0 = p0, q1 = p1;
    if (have) {
        const float     len0  = glm::length(m_mel_b1[e] - m_mel_b0[e]);
        const glm::vec3 hand0 = len0 > 1e-4f ? m_mel_b0[e] + (m_mel_b1[e] - m_mel_b0[e]) * (back / len0) : m_mel_b0[e];
        m_mel_v_hand = glm::length(bh - hand0) / dt;
        m_mel_v_tip  = glm::length(b1 - m_mel_b1[e]) / dt;
        v            = std::max(m_mel_v_hand, m_mel_v_tip);
        q0           = Bt * m_mel_b0[e];
        q1           = Bt * m_mel_b1[e];
        // melee fix 14b (probe 2, 16 Nov 09:10:05.114: the script's "ctrl" put the test controller at its glide's start in one step, through the NPC's
        // head at 17.4 m/s, under kJumpSpeed: a contact and a press; the glide itself made the second, 806 ms later): a direct set of the test controller
        // (testctrl's ctrl) since this eye's last pass is a jump too.
        const uint32_t sets     = m_dbg_ctrl_sets.load();
        const bool     test_set = sets != m_mel_ctrl_sets_seen[e];
        if (test_set || m_mel_v_hand > kJumpSpeed || m_mel_v_tip > 2.0f * kJumpSpeed) {   // tracking lost and found, a recentre: no swing
            m_mel_geo_jumps++;
            if (test_set) {
                spdlog::info("[Melee] the test controller was set (ctrl) since the last pass: {:.1f} m/s (hand {:.1f}, tip {:.1f}) is a jump, not a swing", v, m_mel_v_hand, m_mel_v_tip);
            } else if (now - m_mel_jump_said > std::chrono::seconds(5)) {
                m_mel_jump_said = now;
                spdlog::info("[Melee] the weapon jumped {:.1f} m/s (hand {:.1f}, tip {:.1f}) in the body's axes: not a swing", v, m_mel_v_hand, m_mel_v_tip);
            }
            have = false;
            v = m_mel_v_hand = m_mel_v_tip = 0.0f;
            q0 = p0;
            q1 = p1;
        }
    }
    m_mel_v = have ? 0.5f * m_mel_v + 0.5f * v : 0.0f;
    if (m_mel_inside.size() != bodies.size()) m_mel_inside.assign(bodies.size(), 0);   // the bodies changed: no entry from before counts
    // A segment against a body: how far it is from the body's surface (below 0: inside), the point of the body nearest it and the part.
    // With its bones (meleebones) the nearest of their capsules (the weapon meleepad thick, the bare hand 3 cm more), else the standing
    // capsule (meleeradius round, the bare hand 8 cm more).
    const float r   = m_mel_radius.load() + (armed ? 0.0f : 0.08f);
    const float pad = m_mel_pad.load() + (armed ? 0.0f : 0.03f);
    struct Touch { float d{ 1e9f }; float s{ 0.0f }; glm::vec3 at{ 0.0f }; int region{ -1 }; glm::vec3 p{ 0.0f }; float along{ 0.0f }; glm::vec3 vel{ 0.0f }; };
    auto touch = [&](const MeleeBody& b, const glm::vec3& a0, const glm::vec3& a1, Touch& out) {
        if (b.caps.size() >= 3) {
            for (const auto& c : b.caps) {
                float       s = 0.0f, t = 0.0f;
                const float d = seg_seg(a0, a1, c.a, c.b, s, t) - c.r - pad;
                if (d < out.d) {
                    out.d      = d;
                    out.s      = s;
                    out.at     = c.a + (c.b - c.a) * t;
                    out.p      = a0 + (a1 - a0) * s;
                    out.region = contact_part(c, t, out.at, out.p);   // melee fix 6: the part where the weapon meets it (was the capsule's own)
                }
            }
        } else {
            const glm::vec3 c0 = b.rel + glm::vec3{ 0.0f, 0.0f, kCapLow }, c1 = b.rel + glm::vec3{ 0.0f, 0.0f, kCapHigh };
            float           s = 0.0f, t = 0.0f;
            const float     d = seg_seg(a0, a1, c0, c1, s, t) - r;
            if (d < out.d) { out.d = d; out.s = s; out.at = c0 + (c1 - c0) * t; out.region = -1; out.p = a0 + (a1 - a0) * s; }
        }
    };
    constexpr int kSamples = 8;
    int           best = -1;
    Touch         best_t{};
    for (size_t i = 0; i < bodies.size(); ++i) {
        Touch now_t{};
        touch(bodies[i], p0, p1, now_t);
        now_t.along = now_t.s;
        if (have) {
            // Swept (as Chasm's VR framework's combat.rs, MIT): the weapon followed at kSamples points along it, each from where it was at
            // this eye's last present (in today's body axes) to where it is now; the deepest contact counts, with that point's own velocity.
            for (int k = 0; k < kSamples; ++k) {
                const float     u  = float(k) / float(kSamples - 1);
                const glm::vec3 a0 = q0 + (q1 - q0) * u, a1 = p0 + (p1 - p0) * u;
                Touch           tk{};
                touch(bodies[i], a0, a1, tk);
                if (tk.d < now_t.d) { now_t = tk; now_t.along = u; }
            }
            now_t.vel = ((p0 + (p1 - p0) * now_t.along) - (q0 + (q1 - q0) * now_t.along)) / dt;
        }
        const bool in = now_t.d < 0.0f;
        if (in && !m_mel_inside[i] && now_t.d < best_t.d) { best = int(i); best_t = now_t; }
        m_mel_inside[i] = in ? 1 : 0;
    }
    const float speed = std::max(v, m_mel_v);
    // melee fix 6: a swing also ends when the weapon is off every body (no contact for 80 ms) and its tip turns back (its way of travel in the
    // body's axes more than about 107 deg from the last frame's, both over 0.6 meleespeed): a player's quick back-and-forth swings never slow
    // under 0.6 meleespeed between passes (the smoothed speed), so fix 4's slow-down rule alone kept the second pass from counting on the
    // ray's path (one damage per entity per swing) and kept a swing that met a wall dead (meleereact).
    {
        for (const auto x : m_mel_inside)
            if (x) m_mel_body_t = now;
        if (have) {
            const glm::vec3 vt = (b1 - m_mel_b1[e]) / dt;
            const float     sp = glm::length(vt);
            if (sp > 0.6f * m_melee_speed.load()) {
                const glm::vec3 u = vt / sp;
                if (kRearmReverse) {
                    // melee fix 16: m_mel_swing_dir is the way the stroke began (kept through slow frames up to 400 ms); turned back past ~107 deg
                    // from it: a new stroke, whose swing starts once the weapon is off every body 80 ms (below).
                    if (glm::length(m_mel_swing_dir) < 0.5f) m_mel_swing_dir = u;
                    else if (glm::dot(u, m_mel_swing_dir) < -0.3f) {
                        m_mel_rev_pending = true;
                        m_mel_swing_dir   = u;
                    }
                    m_mel_fast_t = now;
                } else {
                    if (glm::length(m_mel_swing_dir) > 0.5f && glm::dot(u, m_mel_swing_dir) < -0.3f && now - m_mel_body_t > std::chrono::milliseconds(80)) {
                        m_mel_swing_roots.clear();   // melee fix 9
                        m_mel_swing_dead    = false;
                    }
                    m_mel_swing_dir = u;
                }
            } else if (!kRearmReverse || now - m_mel_fast_t > std::chrono::milliseconds(400)) {
                m_mel_swing_dir = glm::vec3{ 0.0f };
            }
        } else {
            m_mel_swing_dir = glm::vec3{ 0.0f };
        }
        if (kRearmReverse && m_mel_rev_pending && now - m_mel_body_t > std::chrono::milliseconds(80)) {
            m_mel_rev_pending = false;
            if (!m_mel_swing_roots.empty() || m_mel_swing_dead) {
                if (now - m_mel_rearm_said > std::chrono::milliseconds(500)) {
                    m_mel_rearm_said = now;
                    spdlog::info("[Melee] new swing: the tip turned back off the body ({} ms after the last press): {} bodies hit in the last swing can be hit again (melee fix 16)",
                                 std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_press_t).count(), m_mel_swing_roots.size());
                }
                m_mel_swing_roots.clear();
                m_mel_swing_dead = false;
            }
        }
    }
    // melee fix 5 (meleeshow on, test): the hit line against the drawn characters' capsules (the same test as a hit, less meleepad), now and swept
    // from this eye's last present; the nearest over a second is logged once a second, so a run shows whether a swing crossed a body at all.
    if (geo) {
        m_mel_geo_frames++;
        if (have) m_mel_geo_have++;
        m_mel_geo_vmax  = std::max(m_mel_geo_vmax, speed);
        m_mel_geo_chars = std::max(m_mel_geo_chars, int(drawn_chars.size()));
        for (const auto& bd : drawn_chars) {
            Touch tn{};
            touch(bd, p0, p1, tn);
            float sw = tn.d;
            if (have)
                for (int k = 0; k < kSamples; ++k) {
                    const float u = float(k) / float(kSamples - 1);
                    Touch       tk{};
                    touch(bd, q0 + (q1 - q0) * u, p0 + (p1 - p0) * u, tk);
                    sw = std::min(sw, tk.d);
                }
            m_mel_geo_sw = std::min(m_mel_geo_sw, sw);
            if (tn.d < m_mel_geo_d) {
                m_mel_geo_d      = tn.d;
                m_mel_geo_region = tn.region;
                m_mel_geo_p0     = p0;
                m_mel_geo_p1     = p1;
                m_mel_geo_at     = tn.at;
                m_mel_geo_v      = speed;
            }
        }
        if (now - m_mel_geo_said >= std::chrono::seconds(1)) {
            m_mel_geo_said   = now;
            const auto  sig  = m_weapon_sig.load();
            const char* what = !armed ? "the bare hand" : blade ? "a melee weapon, the blade from the drawn weapon" : melee_weapon(sig) ? "a melee weapon, no drawn line (meleereach)" : "a gun (meleereach)";
            if (m_mel_geo_chars == 0)
                spdlog::info("[Melee] hit line (meleeshow): no drawn character within 20 m in the last second; weapon {} ({}), fastest {:.1f} m/s (meleespeed {:.1f}), {} of {} frames with the last present, "
                             "{} jumps (not swings)", sig, what, m_mel_geo_vmax, m_melee_speed.load(), m_mel_geo_have, m_mel_geo_frames, m_mel_geo_jumps);
            else
                spdlog::info("[Melee] hit line (meleeshow): nearest {:.2f} m from a drawn character's capsules ({}; below 0: through it), swept {:.2f} m, in the last second; the line then "
                             "({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f}) from the camera, the capsule's point ({:.2f}, {:.2f}, {:.2f}); {:.1f} m/s then, fastest {:.1f} (meleespeed {:.1f}); "
                             "weapon {} ({}, reach {:.2f} m); {} of {} frames with the last present, {} jumps (not swings); {} characters drawn",
                             m_mel_geo_d, m_mel_geo_region >= 0 ? region_name(m_mel_geo_region) : "the standing capsule", m_mel_geo_sw, m_mel_geo_p0.x, m_mel_geo_p0.y, m_mel_geo_p0.z,
                             m_mel_geo_p1.x, m_mel_geo_p1.y, m_mel_geo_p1.z, m_mel_geo_at.x, m_mel_geo_at.y, m_mel_geo_at.z, m_mel_geo_v, m_mel_geo_vmax, m_melee_speed.load(), sig, what, reach,
                             m_mel_geo_have, m_mel_geo_frames, m_mel_geo_jumps, m_mel_geo_chars);
            m_mel_geo_d = m_mel_geo_sw = 1e9f;
            m_mel_geo_v = m_mel_geo_vmax = 0.0f;
            m_mel_geo_region = -1;
            m_mel_geo_frames = m_mel_geo_have = m_mel_geo_jumps = m_mel_geo_chars = 0;
        }
    }
    // melee fix 10 (the author's crash 13 Nov 15:19:35, and 12 Nov 21:06:36): both were the game's own int3 in one routine (an op-stream interpreter
    // on a worker thread, two copies: an op byte past 0x2C, the stream read into pose floats), 0.40 and 0.55 s after the mod's direct stim took its
    // target to 0. Fix 10b: the mod's direct damage is gone (meleedamage, meleestim, meleedamagehp, meleeheadx); the game's own melee, pressed at
    // the contact (meleehit 2), deals all the damage.
    if (speed < 0.6f * m_melee_speed.load()) m_mel_swing_roots.clear();   // melee fix 9: a swing is over once the weapon slows (as fix 4's entity)
    if (m_mel_swing_dead && (!react || speed < 0.6f * m_melee_speed.load())) m_mel_swing_dead = false;
    // The game's melee pressed at a hit (or, with meleeearly, before it): the aim turned onto the point hit first (on the standing
    // capsule's axis 0.9-1.6 m up; with the bones the point of the bone hit).
    // melee fix 12 (author 13 Nov 21:44Z): no press of the game's melee within 0.5 s
    // of a grab from the sling or an equip (the grab's own fast motion past the body is no blow); and only at a contact with a drawn character
    // (kEarlyPress false: no press on a swing that has not touched one, whatever meleeearly says).
    auto press_blocked = [&]() {
        // meleefist (author 23 Nov 07:39Z; his 08:37:43 log: weapon on the sling, the open
        // hand 3.6 m/s, then 6.3 m/s, into an NPC 0.6 m in front after a sprint, no grip pressed: two presses of the game's melee, the NPC killed):
        // the bare hand (no weapon, or the weapon on the sling or kept) presses the game's melee only while its grip is held (a closed fist).
        if (!armed && m_mel_fist.load() && !m_grip_held.load()) {
            if (now - m_mel_nopress_said > std::chrono::milliseconds(500)) {
                m_mel_nopress_said = now;
                spdlog::info("[Melee] the game's melee not pressed: the bare hand was open (meleefist: a punch needs the grip held)");
            }
            return true;
        }
        const auto g = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_grab_t).count();
        const auto e = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_wsig_t).count();
        if (g >= 500 && (e >= 500 || (kEquipBareNoBlock && fc5::gameweapon::bare_hands(m_weapon_sig.load())))) return false;
        if (now - m_mel_nopress_said > std::chrono::milliseconds(500)) {
            m_mel_nopress_said = now;
            spdlog::info("[Melee] the game's melee not pressed: {} ms after {} (none within 500 ms of either)", g < e ? g : e, g < e ? "a grab from the sling" : "the weapon came into the hand");
        }
        return true;
    };
    auto press = [&](const MeleeBody& b, const Touch& t) {
        m_mel_target = m_world_cam_pos + (t.region >= 0 ? t.at : b.rel + glm::vec3{ 0.0f, 0.0f, std::clamp(t.at.z - b.rel.z, 0.9f, 1.6f) });
        if (mode != 2 || press_blocked()) return;
        m_mel_pr_root = m_world_cam_pos + b.rel;   // melee fix 14b: this contact, for "one press per contact"
        m_mel_pr_t    = now;
        m_mel_pr_left = false;
        m_mel_state   = 1;
        m_mel_state_t = now;
        m_mel_aimed   = false;
        m_mel_hit_ent = 0;   // melee fix 13: a bones hit: the entity comes from the ray's next answer on this body (watch_entity), if any
        m_mel_hit_hp  = -1.0f;
        // melee fix 14b (probe 2: the watch had entity 0): the entity of this body from the ray's last answer paired with it (5 s); else the
        // damage watch looks for the recorded entity at its root on the script thread (FarCry5GameRay.cpp).
        m_mel_hit_root    = m_world_cam_pos + b.rel;
        m_mel_hit_root_ok = !b.test;
        m_mel_hit_src.clear();
        if (!b.test) {
            float dbest = 0.7f;
            for (const auto& rp : m_mel_ray_pairs) {
                if (rp.entity == 0 || now - rp.t > std::chrono::seconds(5)) continue;
                const float dh = glm::length(glm::vec2{ rp.root - m_mel_hit_root });
                if (dh < dbest && std::abs(rp.root.z - m_mel_hit_root.z) < 1.2f) {
                    dbest         = dh;
                    m_mel_hit_ent = rp.entity;
                    m_mel_hit_src = fmt::format("the game's ray answered entity {:x} on this body {} ms before (its root {:.2f} m off)", rp.entity,
                                                std::chrono::duration_cast<std::chrono::milliseconds>(now - rp.t).count(), dh);
                }
            }
        }
        m_mel_queued = false;
        if (kPressAtContact) press_or_queue();   // melee fix 16 (was press_go(true))
    };
    auto part =[](const MeleeBody& b, const Touch& t) {
        return t.region >= 0 ? fmt::format("the {} ({} bone capsules)", region_name(t.region), b.caps.size()) : std::string{ "the standing capsule" };
    };
    // meleeearly (26 Oct, author 12:44): the game's swing lands about 0.2 s
    // after the press (simulator 25 Oct: the punch out 0.2 s after it), so the press goes when the weapon, moving on as it moves now,
    // will enter a body within meleeearly ms; the pulse still waits for the contact itself.
    const int early_ms = m_mel_early_ms.load();
    if (kEarlyPress && mode == 2 && early_ms > 0 && have && speed > m_melee_speed.load() && now - m_mel_last_hit > std::chrono::milliseconds(melee_cool_ms())) {
        const glm::vec3 v0 = (p0 - q0) / dt, v1 = (p1 - q1) / dt;
        int   eb = -1;
        float et = 0.0f;
        Touch etouch{};
        for (size_t i = 0; i < bodies.size() && eb < 0; ++i) {
            if (m_mel_inside[i]) continue;
            if (use_ray && !bodies[i].test) continue;   // melee fix 9: with the ray on, the ray's own early press (as before)
            for (int k = 1; k <= 5; ++k) {
                const float t = 0.001f * float(early_ms) * float(k) / 5.0f;
                Touch       te{};
                touch(bodies[i], p0 + v0 * t, p1 + v1 * t, te);
                if (te.d < 0.0f) { eb = int(i); et = t; etouch = te; break; }
            }
        }
        if (eb >= 0) {
            m_mel_last_hit = now;
            m_mel_early_t  = now;
            m_mel_swings++;
            const auto& b = bodies[eb];
            spdlog::info("[Melee] early press: {} {:.1f} m/s will reach {} of the body at ({:.2f}, {:.2f}, {:.2f}) in about {:.0f} ms (meleeearly {}){}", armed ? "the weapon" : "the hand", speed,
                         part(b, etouch), b.rel.x, b.rel.y, b.rel.z, 1000.0f * et, early_ms, b.test ? " (test body)" : "");
            press(b, etouch);
        }
    }
    // meleeray: what the game's rays found (a frame of the game ago at most), and the rays for the next one. The swing against the game camera, so the player's own walking
    // does not sweep. The fist: 8 cm either side of the hand along the arm.
    // melee fix 9: this pass's ray answer, for the bones path below: a character found (a drawn one at the hit), a wall and where.
    bool      ray_answer = false, ray_char = false, ray_wall = false;
    glm::vec3 ray_wall_rel{ 0.0f };
    if (use_ray) {
        const glm::vec3 c = m_world_cam_pos;
        const glm::vec3 a0 = armed ? p0 : hand - dir * 0.08f, a1 = armed ? p1 : hand + dir * 0.08f;
        fc5::gameray::Seg segs[4];
        int  ns  = 0;
        auto seg = [&](const glm::vec3& x, const glm::vec3& y, bool ahead) { segs[ns++] = fc5::gameray::Seg{ c.x + x.x, c.y + x.y, c.z + x.z, c.x + y.x, c.y + y.y, c.z + y.z, ahead }; };
        seg(a0, a1, false);
        if (have) {
            seg(q1, p1, false);   // (q0, q1: the segment at this eye's last present in today's body axes: a snap turn does not sweep)
            seg(0.5f * (q0 + q1), 0.5f * (p0 + p1), false);
            if (kEarlyPress && mode == 2 && early_ms > 0 && speed > m_melee_speed.load()) seg(p1, p1 + (p1 - q1) / dt * (0.001f * float(early_ms)), true);
        }
        fc5::gameray::submit(segs, ns);
        fc5::gameray::Result res{};
        if (fc5::gameray::take(res)) {
            const bool over = speed > m_melee_speed.load();
            const bool cool = now - m_mel_last_hit > std::chrono::milliseconds(melee_cool_ms());
            auto point      = [&](const fc5::gameray::Hit& h) { return glm::vec3{ h.x, h.y, h.z }; };
            auto hit_sound  = [&]() {
                if (!armed || !m_mel_ray_sound.load()) return;
                const auto* w = fc5::gameweapon::info(m_weapon_sig.load());
                fc5::gamesound::post(w && w->holster == 1 ? 2040638u : 2276490191u, 15);
            };
            auto aim_press  = [&](const glm::vec3& at) {
                m_mel_target = at;
                if (mode != 2 || press_blocked()) return;
                m_mel_state   = 1;
                m_mel_state_t = now;
                m_mel_aimed   = false;
                m_mel_hit_ent = res.body.any ? res.body.entity : res.body_ahead.entity;   // melee fix 13: the entity whose health is watched
                m_mel_hit_hp  = res.body.any ? res.body.health : res.body_ahead.health;
                m_mel_hit_root_ok = false;   // melee fix 14b: the ray named it
                m_mel_hit_src     = "the game's ray at the contact";
                m_mel_queued      = false;
                if (kPressAtContact) press_or_queue();   // melee fix 16 (was press_go(true))
            };
            if (res.body.any) m_mel_body_t = now;   // melee fix 6 (the swing's end by reversal)
            ray_answer   = true;
            ray_char     = res.body.any && near_drawn(point(res.body) - c).ok();
            ray_wall     = res.wall.any;
            ray_wall_rel = point(res.wall) - c;
            // melee fix 9: the body the ray's hit belongs to, and whether this swing already hit it (by the bones)
            const int  ray_nb  = res.body.any ? body_at(point(res.body) - c) : -1;
            const bool ray_dup = ray_nb >= 0 && swing_has(m_world_cam_pos + bodies[ray_nb].rel);
            // melee fix 14b: the ray's entity paired with the drawn body it hit (for a later bones hit on that body); same body: replaced in place
            if (ray_char && ray_nb >= 0 && !bodies[ray_nb].test && res.body.entity != 0) {
                const glm::vec3 root = m_world_cam_pos + bodies[ray_nb].rel;
                int             slot = m_mel_ray_pair_at;
                for (int k = 0; k < 8; ++k)
                    if (m_mel_ray_pairs[k].entity == res.body.entity || glm::length(glm::vec2{ m_mel_ray_pairs[k].root - root }) < 0.3f) { slot = k; break; }
                if (slot == m_mel_ray_pair_at) m_mel_ray_pair_at = (m_mel_ray_pair_at + 1) % 8;
                m_mel_ray_pairs[slot] = MeleeRayPair{ root, res.body.entity, res.body.health, now };
            }
            if (kEarlyPress && res.body_ahead.any && over && cool && !res.body.any && !near_drawn(point(res.body_ahead) - c).ok()) {
                if (now - m_mel_nochar_said > std::chrono::seconds(2)) {
                    m_mel_nochar_said = now;
                    spdlog::info("[Melee] no drawn character at the ray's hit: no damage (early press withheld; entity {:x}, health {:.0f}, {} characters drawn)", res.body_ahead.entity,
                                 res.body_ahead.health, drawn_chars.size());
                }
            } else if (kEarlyPress && res.body_ahead.any && over && cool && !res.body.any) {
                m_mel_last_hit = now;
                m_mel_early_t  = now;
                m_mel_swings++;
                spdlog::info("[Melee] early press (the game's ray): {} {:.1f} m/s will reach entity {:x} (health {:.0f}) (meleeearly {})", armed ? "the weapon" : "the hand", speed, res.body_ahead.entity, res.body_ahead.health, early_ms);
                aim_press(point(res.body_ahead));
            }
            if (res.body.any) {
                const bool fresh = res.body.entity != m_mel_ray_ent || now - m_mel_ray_t > std::chrono::milliseconds(react ? 80 : 250);   // still touching the same one: no new hit (meleereact: off it for 80 ms)
                m_mel_ray_ent    = res.body.entity;
                m_mel_ray_t      = now;
                const bool after = now - m_mel_early_t < std::chrono::milliseconds(early_ms + 250);
                if (fresh && over && (cool || after) && m_mel_swing_dead) {
                    if (now - m_mel_out_said > std::chrono::seconds(1)) {
                        m_mel_out_said = now;
                        spdlog::info("[Melee] contact with entity {:x} not counted: this swing already met a wall (meleereact)", res.body.entity);
                    }
                } else if (fresh && over && (cool || after) && ray_dup && near_drawn(point(res.body) - c).ok()) {
                    // melee fix 9: the bones already counted this body in this swing: one hit per body per swing
                    if (now - m_mel_bones_said > std::chrono::milliseconds(500)) {
                        m_mel_bones_said = now;
                        spdlog::info("[Melee] hit (the game's ray, source ray) on entity {:x} not counted: the bones already hit this body in this swing", res.body.entity);
                    }
                    fc5::gameray::watch_entity(res.body.entity, res.body.health);   // melee fix 13: the bones hit's entity, for its damage line
                } else if (fresh && over && (cool || after) && !near_drawn(point(res.body) - c).ok()) {
                    // melee fix 4: an entity the ray reports with no drawn character there (a prop, a vehicle, a placed object): the pulse, no damage, no game melee.
                    m_mel_last_hit = now;
                    m_mel_buzz     = m_mel_buzz_amp.load();
                    const glm::vec3 at = point(res.body);
                    if (now - m_mel_nochar_said > std::chrono::milliseconds(500)) {
                        m_mel_nochar_said = now;
                        spdlog::info("[Melee] no drawn character at the ray's hit: no damage (entity {:x}, health {:.0f}, part {}, at ({:.2f}, {:.2f}, {:.2f}) from the camera, the nearest drawn "
                                     "character {:.2f} m off (needed: within {:.2f} m of its capsules or inside its standing capsule), {} drawn)", res.body.entity, res.body.health,
                                     res.body.part, at.x - c.x, at.y - c.y, at.z - c.z, std::min(near_drawn(at - c).d, 99.0f), kRayNear, drawn_chars.size());
                    }
                    hit_sound();
                } else if (fresh && over && (cool || after)) {
                    m_mel_swings++;
                    m_mel_hits++;
                    m_mel_last_hit = now;
                    const glm::vec3 at = point(res.body);
                    // melee fix 6: the part from the nearest capsule of the drawn character at the hit point (contact_part), not the game's -1.
                    const Near      nm = near_drawn(at - c);
                    // melee fix 9: "both" when this pass's bones contact is on the same body (one hit, the ray's); the body noted for this swing
                    const bool both = ray_nb >= 0 && best == ray_nb && best_t.d < 0.0f;
                    if (ray_nb >= 0) m_mel_swing_roots.push_back(m_world_cam_pos + bodies[ray_nb].rel);
                    spdlog::info("[Melee] hit (the game's ray, source {}): {}{} {:.1f} m/s into entity {:x} (health {:.0f}, part {}: {}) at ({:.2f}, {:.2f}, {:.2f}) from the camera, normal ({:.2f}, {:.2f}, {:.2f}); {}", both ? "both" : "ray", armed ? "the weapon" : "the hand",
                                 blade ? fmt::format(" ({}, the blade)", m_weapon_sig.load()) : std::string{}, speed, res.body.entity, res.body.health,
                                 nm.region >= 0 ? region_name(nm.region) : "-", nm.region >= 0 ? fmt::format("the nearest capsule {:.2f} m off{}", nm.d, nm.standing ? ", inside the standing capsule" : "")
                                                                                   : fmt::format("no capsule, the game's part {}", res.body.part),
                                 at.x - c.x, at.y - c.y, at.z - c.z, res.body.nx, res.body.ny, res.body.nz,
                                 after ? std::string{ "after the early press" } : mode == 2 ? std::string{ "the game's melee next" } : std::string{ "log only (meleehit 1)" });
                    m_mel_early_t = {};
                    m_mel_buzz    = m_mel_buzz_amp.load();
                    if (mode != 2 || kSoundAtContact) hit_sound();   // melee fix 13: at the contact, not only with the game's blow
                    // melee fix 10b: no damage from the mod; the game's own melee, pressed now (or already, early), does it.
                    if (!after) aim_press(at);
                }
            } else if (res.wall.any && over && now - m_mel_wall_t > std::chrono::milliseconds(250)) {
                m_mel_walls++;
                const glm::vec3 at = point(res.wall);
                spdlog::info("[Melee] wall (the game's ray): {} {:.1f} m/s met entity {:x} at ({:.2f}, {:.2f}, {:.2f}) from the camera, normal ({:.2f}, {:.2f}, {:.2f})", armed ? "the weapon" : "the hand", speed, res.wall.entity, at.x - c.x, at.y - c.y, at.z - c.z, res.wall.nx, res.wall.ny, res.wall.nz);
                // meleereact: the pulse grows with the swing (to full at 7 m/s), and on the world's own shapes (no entity: ground, walls, rocks) the swing ends.
                if (m_mel_wall_buzz.load() > 0.0f) m_mel_buzz = react ? std::clamp(std::max(m_mel_wall_buzz.load(), 0.35f + 0.65f * std::min(1.0f, speed / 7.0f)), 0.0f, 1.0f) : m_mel_wall_buzz.load();
                if (react && res.wall.entity == 0) m_mel_swing_dead = true;
                hit_sound();
            }
            if (res.wall.any) m_mel_wall_t = now;   // still against it: one pulse
        }
    }
    // meleebones also judges the contact as Chasm's framework does: the speed is that of the weapon's point that met the body (not the
    // hand's or the tip's), and it must be moving into the body (towards the bone's axis), so a weapon pulled out or slid along a body
    // is no blow. meleestrength: the pulse grows with that speed, from meleespeed (a quarter) to 7 m/s (6 for the fist:
    // full), up to 1.5 times past it.
    const bool  judged  = m_mel_bones.load();
    const float impact  = glm::length(best_t.vel);
    const bool  into    = !judged || glm::dot(best_t.vel, (q0 + (q1 - q0) * best_t.along) - best_t.at) < 0.0f;
    const float vmin    = m_melee_speed.load(), vfull = armed ? 7.0f : 6.0f;
    const float strength = std::clamp((impact - vmin) / std::max(vfull - vmin, 0.5f), 0.25f, 1.5f);
    if (best >= 0 && have && (judged ? impact : speed) > vmin && !into) {
        if (now - m_mel_out_said > std::chrono::seconds(2)) {
            m_mel_out_said = now;
            spdlog::info("[Melee] contact with the {} at {:.1f} m/s not counted: the weapon was moving out of the body or along it", best_t.region >= 0 ? region_name(best_t.region) : "body", impact);
        }
    }
    // melee fix 9: with the ray on, a drawn body's bones contact counts only when this pass's ray answer has no character, no wall nearer to the
    // hand than the contact (a body behind a wall), and the body was not hit in this swing (by the ray or the bones).
    std::string bones_block;
    // melee fix 14b: one press per contact. After a bones press, the weapon must leave that body (over 10 cm from its capsules) before another
    // bones contact on it within 1.5 s counts as a new one.
    if (!m_mel_pr_left) {
        if (now - m_mel_pr_t > std::chrono::milliseconds(1500)) m_mel_pr_left = true;
        else {
            int   nb = -1;
            float nd = 0.5f;
            for (size_t i = 0; i < bodies.size(); ++i)
                if (const float d = glm::length(glm::vec2{ m_world_cam_pos + bodies[i].rel - m_mel_pr_root }); d < nd) { nd = d; nb = int(i); }
            Touch tl{};
            if (nb >= 0) touch(bodies[nb], p0, p1, tl);
            if (nb < 0 || tl.d > 0.10f) m_mel_pr_left = true;
        }
    }
    const bool same_contact = best >= 0 && !m_mel_pr_left && !bodies[best].test && glm::length(glm::vec2{ m_world_cam_pos + bodies[best].rel - m_mel_pr_root }) < 0.5f;
    if (same_contact) bones_block = fmt::format("the same contact as the press {} ms ago (the weapon has not left this body since)",
                                                std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_pr_t).count());
    else if (best >= 0 && use_ray && !bodies[best].test) {
        const float dc = glm::length(best_t.p - hand);
        if (ray_char) bones_block = "the game's ray found a character in this pass (its hit counts)";
        else if (ray_wall && glm::length(ray_wall_rel - hand) < dc) bones_block = fmt::format("a wall {:.2f} m from the hand, nearer than the contact ({:.2f} m)", glm::length(ray_wall_rel - hand), dc);
        else if (swing_has(m_world_cam_pos + bodies[best].rel)) bones_block = "this body was already hit in this swing";
    }
    if (best >= 0 && have && (judged ? impact : speed) > vmin && into && !bones_block.empty()) {
        if (now - m_mel_bones_said > std::chrono::milliseconds(500)) {
            m_mel_bones_said = now;
            spdlog::info("[Melee] bones contact with the {} not counted (source bones): {}{}", best_t.region >= 0 ? region_name(best_t.region) : "body", bones_block,
                         ray_answer ? "" : "; no ray answer this pass");
        }
    }
    if (best >= 0 && have && (judged ? impact : speed) > vmin && into && bones_block.empty()) {
        m_mel_swings++;
        const bool  cool  = now - m_mel_last_hit > std::chrono::milliseconds(melee_cool_ms());
        const bool  after = now - m_mel_early_t < std::chrono::milliseconds(early_ms + 250);   // the contact of an early press
        const auto& b     = bodies[best];
        const float h     = best_t.at.z - b.rel.z;
        if (cool || after) {
            if (!b.test) m_mel_swing_roots.push_back(m_world_cam_pos + b.rel);   // melee fix 9
            m_mel_hits++;
            m_mel_last_hit = now;
            m_mel_hit_p    = m_world_cam_pos + best_t.at;
            m_mel_hit_t    = now;
            const int64_t since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_early_t).count();
            spdlog::info("[Melee] hit{} (source bones{}): {}{} {:.1f} m/s at the contact, strength {:.2f} (hand {:.1f}, tip {:.1f}, body axes) into {} of the body at ({:.2f}, {:.2f}, {:.2f}) from the camera ({:.2f} m across the ground), "
                         "{:.2f} m above its root, {:.2f} m inside, {} of the {} ({} {:.2f} m); {} bodies near; {}",
                         b.test ? " (test body)" : "", use_ray ? (ray_answer ? ", the ray's answer had no character" : ", no ray answer this pass") : "", armed ? "the weapon" : "the hand", blade ? fmt::format(" ({}, the blade)", m_weapon_sig.load()) : std::string{}, impact, strength, m_mel_v_hand, m_mel_v_tip,
                         part(b, best_t), b.rel.x, b.rel.y, b.rel.z, glm::length(glm::vec2{ b.rel }), h, -best_t.d,
                         best_t.along > 0.8f ? "the tip" : best_t.along < 0.35f && armed ? (blade ? "the hilt" : "the stock end") : "the middle", armed ? "weapon" : "hand", armed ? "reach" : "fist", armed ? reach : r, bodies.size(),
                         after ? fmt::format("{} ms after the early press", since) : mode == 2 ? std::string{ "the game's melee next" } : std::string{ "log only (meleehit 1)" });
            m_mel_early_t = {};
            const bool str = m_mel_strength.load();
            m_mel_buzz    = m_mel_buzz_amp.load() * (str ? std::min(1.0f, 0.35f + 0.55f * strength) : 1.0f);   // (also in log only: the detection can be felt in the headset)
            // melee fix 13: the game's hit sound at the contact (as the ray path's hit_sound)
            if (kSoundAtContact && mode == 2 && armed && !b.test && m_mel_ray_sound.load()) {
                const auto* w = fc5::gameweapon::info(m_weapon_sig.load());
                fc5::gamesound::post(w && w->holster == 1 ? 2040638u : 2276490191u, 15);
            }
            if (!after) press(b, best_t);
        }
    }
    if (const int left = m_mel_log.load(); left > 0 && now - m_mel_log_t > std::chrono::seconds(1)) {
        m_mel_log_t = now;
        m_mel_log   = left - 1;
        std::string list;
        for (const auto& b : bodies) list += fmt::format(" ({:.2f}, {:.2f}, {:.2f}){}", b.rel.x, b.rel.y, b.rel.z, b.test ? " test" : "");
        spdlog::info("[Melee] {} bodies near:{}; {} hand ({:.2f}, {:.2f}, {:.2f}) along ({:.2f}, {:.2f}, {:.2f}) {:.1f} m/s (hand {:.1f}, tip {:.1f}); {} hits, {} presses", bodies.size(), list,
                     armed ? "weapon" : "bare", hand.x, hand.y, hand.z, dir.x, dir.y, dir.z, speed, m_mel_v_hand, m_mel_v_tip, m_mel_hits, m_mel_presses);
    }
    m_mel_ctrl_sets_seen[e] = m_dbg_ctrl_sets.load();   // melee fix 14b
    m_mel_p0[e]   = p0;
    m_mel_p1[e]   = p1;
    m_mel_b0[e]   = b0;
    m_mel_b1[e]   = b1;
    m_mel_t[e]    = now;
    m_mel_have[e] = true;
}

// meleeshow (test): the bodies' capsules near the player and the weapon's hit line as dots over this eye's picture (after the overlays,
// as bonemap: projected with this eye's camera, ClearView rectangles).
void FarCry5Stereo::melee_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    if (!m_mel_show.load() || m_mel_mode.load() == 0 || !ctx || !eye || !m_motion.load() || m_menu_screen_on) return;
    const int fr = m_frame.load();
    if (m_hist[0].cur_frame != fr) return;
    if (!m_mel_scan) m_mel_scan = new MeleeScanState{};
    auto& st = *m_mel_scan;
    struct Dot { glm::vec3 p; int col; int size; };
    std::vector<Dot> dots;
    const glm::vec3  cam = m_world_cam_pos;
    {
        std::lock_guard _{ m_mel_mtx };
        const bool fresh = fr - m_mel_bodies_frame <= 6;
        std::vector<MeleeBody> bodies = fresh ? m_mel_bodies : std::vector<MeleeBody>{};
        if (std::chrono::steady_clock::now() < m_mel_test_until) bodies.push_back(MeleeBody{ m_mel_test_rel, true });
        for (const auto& b : bodies) {
            if (glm::length(glm::vec2{ b.rel }) > kBoneRange + 1.0f) continue;
            if (b.caps.size() >= 3) {
                for (const auto& c : b.caps)
                    for (int k = 0; k <= 3; ++k) dots.push_back({ cam + c.a + (c.b - c.a) * (float(k) / 3.0f), std::clamp(c.region, 0, 3), 1 });
            } else {
                for (int k = 0; k <= 8; ++k) dots.push_back({ cam + b.rel + glm::vec3{ 0.0f, 0.0f, kCapLow + (kCapHigh - kCapLow) * float(k) / 8.0f }, 4, 1 });
            }
        }
    }
    const auto now    = std::chrono::steady_clock::now();
    const bool flash  = now - m_mel_hit_t < std::chrono::milliseconds(400);
    const int  e      = fr & 1;
    if (m_mel_have[e])
        for (int k = 0; k <= 6; ++k) dots.push_back({ cam + m_mel_p0[e] + (m_mel_p1[e] - m_mel_p0[e]) * (float(k) / 6.0f), flash ? 1 : 5, 1 });
    if (flash) dots.push_back({ m_mel_hit_p, 6, 2 });
    if (dots.empty()) return;
    if (eye != st.tex) {
        if (st.view) st.view->Release();
        st.view = nullptr;
        st.tex  = eye;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC td{};
            eye->GetDesc(&td);
            if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
                ID3D11RenderTargetView* v = nullptr;
                dev->CreateRenderTargetView(eye, nullptr, &v);
                st.view = v;
            } else if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
                ID3D11UnorderedAccessView* v = nullptr;
                dev->CreateUnorderedAccessView(eye, nullptr, &v);
                st.view = v;
            }
            dev->Release();
        }
        if (!st.view && !st.said_no_view) {
            st.said_no_view = true;
            spdlog::info("[Melee] meleeshow: the eye picture cannot be drawn on (no render target or UAV binding)");
        }
    }
    if (!st.view) return;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return;
    D3D11_TEXTURE2D_DESC d{};
    eye->GetDesc(&d);
    const float W = float(d.Width), H = float(d.Height);
    const LONG  k = std::max<LONG>(1, LONG(std::lround(H / 1100.0f)));
    std::vector<D3D11_RECT> by_col[7], shadow;
    for (const auto& dt : dots) {
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ dt.p - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.05f) continue;
        const float x = (cl.x / cl.w * 0.5f + 0.5f) * W, y = (0.5f - cl.y / cl.w * 0.5f) * H;
        const LONG  rr = (dt.size == 2 ? 5 : 2) * k;
        if (x < float(rr + k) || y < float(rr + k) || x > W - float(rr + k) || y > H - float(rr + k)) continue;
        const LONG X = LONG(x), Y = LONG(y);
        shadow.push_back(D3D11_RECT{ X - rr - k, Y - rr - k, X + rr + k + 1, Y + rr + k + 1 });
        by_col[dt.col].push_back(D3D11_RECT{ X - rr, Y - rr, X + rr + 1, Y + rr + 1 });
    }
    // 0 torso white, 1 head red, 2 arm yellow, 3 leg green, 4 standing capsule grey, 5 weapon cyan, 6 the contact magenta
    static const float kCol[7][4]{ { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 0.15f, 0.1f, 1.0f }, { 1.0f, 0.9f, 0.15f, 1.0f }, { 0.25f, 1.0f, 0.3f, 1.0f },
                                   { 0.6f, 0.6f, 0.6f, 1.0f }, { 0.2f, 0.9f, 1.0f, 1.0f }, { 1.0f, 0.2f, 1.0f, 1.0f } };
    const float dark[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    auto clear = [&](const float* col, const std::vector<D3D11_RECT>& v) {
        for (size_t i = 0; i < v.size(); i += 512) ctx1->ClearView(st.view, col, v.data() + i, UINT(std::min<size_t>(512, v.size() - i)));
    };
    clear(dark, shadow);
    for (int c = 0; c < 7; ++c) clear(kCol[c], by_col[c]);
    ctx1->Release();
    if (st.drawn++ % 900 == 0) spdlog::info("[Melee] meleeshow: {} dots ({} draws read, {} not, {} posed, {} without a palette copy)", dots.size(), st.scanned, st.failed, st.posed, st.no_palette);
}

// The pad (the game's input thread): the press's buttons, and the hit's pulse once.
void FarCry5Stereo::melee_pad(WORD& buttons, BYTE& rt, float& buzz)
{
    buzz = m_mel_buzz.exchange(0.0f);
    if (ticks_now() >= m_mel_press_until.load()) return;
    int b = m_mel_button.load();
    // melee fix 12 (author 13 Nov 21:44Z): a melee weapon in the hand gets
    // RT, its own swing, whatever meleebutton says. The stick click (meleebutton r3, his setting) is the game's generic melee: with a machete or a
    // bat in hand the game put the weapon away, punched with the bare hands and drew the weapon again (his 21:27:49 hit: pressed 21 ms after it,
    // the game held item 0, weapon 4110, from 21:27:49.97 to 21:27:51.14). Guns keep meleebutton (the stick click: the rifle butt).
    if (m_mel_held_melee.load()) b = 1;
    else if (b == 3) b = 0;   // auto: the stick click with anything else
    if (b != 1) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b != 0) rt = 255;
}

void FarCry5Stereo::draw_melee_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    // One F11 section, Gestures > Melee (experimental), in subsections that open on a click, closed at first (author 3 Nov 11:43:
    //). Every command, saved setting and default unchanged.
    auto sub = [](const char* title) {
        ImGui::Dummy(ImVec2{ 0.0f, 4.0f });
        if (!ImGui::CollapsingHeader(title)) return false;
        ImGui::Indent(16.0f);
        return true;
    };
    int m = m_mel_mode.load();
    ImGui::TextUnformatted("Melee by hitting a body");
    bool ch = ImGui::RadioButton("Off##meleehit", &m, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("Log only##meleehit", &m, 1);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("Hit##meleehit", &m, 2);
    if (ch) {
        m_mel_mode = m;
        keep("meleehit", fmt::format("meleehit {}", m));
    }
    hint("Swing your weapon or fist into someone to hit them. Log only: just writes the hits to the log.");
    bool gf = m_mel_grip_fist.load();
    if (ImGui::Checkbox("Hold a melee weapon in the empty hand's fist##meleegripfist", &gf)) {
        m_mel_grip_fist = gf;
        keep("meleegripfist", gf ? "meleegripfist on" : "meleegripfist off");
    }
    hint("Your hand on a melee weapon sits as it does in a closed fist.");
    bool mf = m_mel_fist.load();
    if (ImGui::Checkbox("Punch only with a closed fist (grip held)##meleefist", &mf)) {
        m_mel_fist = mf;
        keep("meleefist", mf ? "meleefist on" : "meleefist off");
    }
    hint("With no weapon in your hand, a hit counts only while you hold the grip. Off: an open hand moving into someone hits too.");
    if (m != 0) {
        if (sub("Swing and button##melsub")) {
            float sp = m_melee_speed.load();
            if (ImGui::SliderFloat("Swing faster than##meleespeed", &sp, 0.5f, 10.0f, "%.1f m/s")) {
                m_melee_speed = sp;
                keep("meleespeed", fmt::format("meleespeed {:.1f}", sp));
            }
            hint("How fast you must swing for a hit to count. Slower contact does nothing.");
            int b = m_mel_button.load();
            ImGui::TextUnformatted("Melee sends");
            bool bc = ImGui::RadioButton("Auto##meleebutton", &b, 3);
            ImGui::SameLine();
            bc |= ImGui::RadioButton("Stick click (melee)##meleebutton", &b, 0);
            ImGui::SameLine();
            bc |= ImGui::RadioButton("Trigger##meleebutton", &b, 1);
            ImGui::SameLine();
            bc |= ImGui::RadioButton("Both##meleebutton", &b, 2);
            if (bc) {
                m_mel_button = b;
                keep("meleebutton", b == 1 ? "meleebutton rt" : b == 2 ? "meleebutton both" : b == 0 ? "meleebutton r3" : "meleebutton auto");
            }
            hint("Which button the melee uses: the stick click, or the trigger with a melee weapon in hand.");
            // melee fix 12: the meleeearly slider is gone (the game's melee is pressed only at a contact; meleeearly is read and kept, with no effect).
            bool rc = m_mel_react.load();
            if (ImGui::Checkbox("Quick hits: a new hit as soon as the weapon comes back in##meleereact", &rc)) {
                m_mel_react = rc;
                keep("meleereact", rc ? "meleereact on" : "meleereact off");
            }
            hint("The melee weapon follows your hand exactly, and you can hit again as soon as it comes back.");
            ImGui::Unindent(16.0f);
        }
        if (sub("What counts as a hit##melsub")) {
            bool ry = m_mel_ray.load();
            if (ImGui::Checkbox("Hits from the game's physics (test)##meleeray", &ry)) {
                m_mel_ray = ry;
                keep("meleeray", fmt::format("meleeray {}", ry ? "on" : "off"));
            }
            hint("For testing: uses the game's physics to find what your weapon hits.");
            bool bo = m_mel_bones.load();
            if (ImGui::Checkbox("Hit the body's own shape (head, torso, arms, legs)##meleebones", &bo)) {
                m_mel_bones = bo;
                keep("meleebones", bo ? "meleebones on" : "meleebones off");
            }
            hint("Hits land where the person's head, body, arms and legs really are. Off: everyone is a simple cylinder.");
            float reach = m_mel_reach.load();
            if (ImGui::SliderFloat("Gun length for hits##meleereach", &reach, 0.0f, 1.2f, "%.2f m")) {
                m_mel_reach = reach;
                keep("meleereach", fmt::format("meleereach {:.2f}", reach));
            }
            float bl = m_mel_blade.load();
            if (ImGui::SliderFloat("Melee weapon length for hits##meleeblade", &bl, 0.1f, 1.5f, "%.2f m")) {
                m_mel_blade = bl;
                keep("meleeblade", fmt::format("meleeblade {:.2f}", bl));
            }
            bool sh = m_mel_show.load();
            if (ImGui::Checkbox("Show the hit shapes (test)##meleeshow", &sh)) m_mel_show = sh;   // (not saved)
            hint("For testing: shows the hit shapes over the picture.");
            ImGui::Unindent(16.0f);
        }
        if (sub("Hit strength##melsub")) {
            ImGui::TextUnformatted("The game's own melee, pressed at the contact, does all the damage.");
            bool sg = m_mel_strength.load();
            if (ImGui::Checkbox("Harder swings, stronger pulse##meleestrength", &sg)) {
                m_mel_strength = sg;
                keep("meleestrength", sg ? "meleestrength on" : "meleestrength off");
            }
            hint("The harder you swing, the stronger the pulse in your hand.");
            ImGui::Unindent(16.0f);
        }
        if (m_mel_ray.load() && sub("Pulse and sound on walls##melsub")) {
            float wb = m_mel_wall_buzz.load();
            if (ImGui::SliderFloat("Pulse on walls and objects##meleewallbuzz", &wb, 0.0f, 1.0f, "%.2f")) {
                m_mel_wall_buzz = wb;
                keep("meleewallbuzz", fmt::format("meleewallbuzz {:.2f}", wb));
            }
            hint("Strength of the pulse when a swing hits a wall or object. 0: none.");
        bool mc = m_mel_collide.load();
        if (ImGui::Checkbox("The melee weapon stops at walls and objects (test)##meleecollide", &mc)) {
            m_mel_collide = mc;
            keep("meleecollide", fmt::format("meleecollide {}", mc ? "on" : "off"));
        }
        hint("For testing: melee weapons stop at walls and objects instead of going through.");
            bool rs = m_mel_ray_sound.load();
            if (ImGui::Checkbox("The game's hit sound at a contact##meleeraysound", &rs)) {
                m_mel_ray_sound = rs;
                keep("meleeraysound", fmt::format("meleeraysound {}", rs ? "on" : "off"));
            }
            hint("Plays the game's hit sound when your weapon hits a wall or object.");
            ImGui::Unindent(16.0f);
        }
    }
    if (sub("Melee weapons in the hand##melsub")) {
        // The weapon in hand now: a melee weapon or not (found by where the game holds it, or set here). Shown whether or not meleestill
        // is on: meleebutton auto and meleeblade use it too.
        const uint32_t sig = m_weapon_sig.load();
        if (sig != 0 && !m_unarmed) {
            int pick = 0;
            {
                std::lock_guard _{ m_mel_mtx };
                pick = m_mel_force_on.contains(sig) ? 1 : m_mel_force_off.contains(sig) ? 2 : 0;
            }
            const bool now = melee_weapon(sig);
            ImGui::Text("Weapon in hand (%u): %s", sig, now ? "a melee weapon" : "not a melee weapon");
            bool c = ImGui::RadioButton("Found by itself##meleeweapon", &pick, 0);
            ImGui::SameLine();
            c |= ImGui::RadioButton("Melee weapon##meleeweapon", &pick, 1);
            ImGui::SameLine();
            c |= ImGui::RadioButton("Not one##meleeweapon", &pick, 2);
            if (c) {
                std::string line = "meleeweapons";
                {
                    std::lock_guard _{ m_mel_mtx };
                    m_mel_force_on.erase(sig);
                    m_mel_force_off.erase(sig);
                    if (pick == 1) m_mel_force_on.insert(sig);
                    if (pick == 2) m_mel_force_off.insert(sig);
                    for (auto s : m_mel_force_on) line += fmt::format(" +{}", s);
                    for (auto s : m_mel_force_off) line += fmt::format(" -{}", s);
                }
                keep("meleeweapons", line == "meleeweapons" ? "meleeweapons auto" : line);
            }
            hint("A weapon the game holds low at your side (machete, shovel, bat) counts as a melee weapon.");
        }
        ImGui::Unindent(16.0f);
    }
}
