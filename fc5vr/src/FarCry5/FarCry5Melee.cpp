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
//  - meleeearly ms (default 100, 0 = off; author 26 Oct 12:44): with meleehit 2
//    the game's melee is pressed once the swing, moving on as it moves, will reach a body within that time (its blow lands ~0.2 s after the
//    press); the pulse (and meleedamage) wait for the contact.
//  - meleedamage 0|1|2|3 (default 2): at the contact the NPC at the body's root is found among the entities the game
//    looks up by id and (2, 3) given meleedamagehp (200; the player has 250) of the game's damage from the player at once (FarCry5Lua.cpp); 1 only finds and
//    logs it; 3 sends no game melee at all. meleedamagestatus logs its counters.
//  - meleeray on|off (new, saved, default off; 2 Nov, author 15:15; FarCry5GameRay.cpp): instead of the capsules the game's own
//    physics ray cast along the weapon and along the path of its tip and middle: a hit is on the real shape of whatever has health, wherever it stands, crouches or lies, the NPC
//    known from the hit itself (meleedamage needs no search), the point hit its aim. meleewallbuzz (0.4, 0 = none): the pulse when a swing meets anything else (a wall, a car).
//    meleeearly: a ray from the tip on along its speed. The test body stays a capsule. meleeraytest ox oy oz dx dy dz [mask] [flags] (x right, y ahead, z up from the camera's heading;
//    default 5 m ahead): one cast, every hit logged. meleeraymask, meleerayflags (0x2DBF as IsEntityInRay, 6 every hit). meleeraystatus.
//  - meleebones on|off (default on; 2 Nov, author 15:15): each character near the
//    player (4 m) is the shape of its own skinned draws instead of one standing capsule: every palette slot (bone) of a character draw is
//    a capsule fitted once to the vertices it moves most (rest pose: their centre, main axis, 5-95 % of their length, 60 % of their
//    distance from the axis; the draw's index and vertex ranges read back once, one draw every 10 frames), posed each frame by the
//    slot's palette matrix (VS b3) and the object transform (VS b1), as the GPU skins it. So a crouching, lying or turning body, an
//    arm or a head is hit where it is drawn; the weapon (meleepad 0.04 m thick, the bare hand 3 cm more) touching any capsule is the
//    contact, aimed at that point. The capsule's rest height names the part (head above 1.45 m, legs below 0.85 m, arms out to the
//    sides, else the torso); a head hit sends meleeheadx (1.5) times meleedamagehp. A draw that cannot be read (no palette copy, an
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
//  - Tests: meleetest x y z [s] (a body standing at x right, y ahead, z up from the game camera, for s seconds, default 20), meleepress
//    (the press now, straight ahead), meleelog N (N seconds of the bodies seen, one line a second).
#include "FarCry5Stereo.h"
#include "FarCry5GameRay.h"
#include "FarCry5GameSound.h"
#include "FarCry5GameWeapon.h"
#include "FarCry5Lua.h"
#include "FarCry5Record.h"

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
int64_t ticks_now() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
const char* button_name(int b, bool melee_weapon)
{
    if (b == 3) b = melee_weapon ? 1 : 0;
    return b == 1 ? "RT" : b == 2 ? "the stick click and RT" : "the stick click";
}

// meleebones
constexpr float kBoneRange = 4.0f;    // characters this near (across the ground) get their bones' capsules
constexpr float kJumpSpeed = 30.0f;   // m/s: the hand faster than any swing (tracking lost and found). The tip whips round much faster than the hand in a hard swing (author 3 Nov 13:31: tip 32.6, hand 10.8, a real swing), so it gets twice this
const char*     region_name(int r) { return r == 1 ? "head" : r == 2 ? "arm" : r == 3 ? "leg" : "torso"; }
glm::mat4       from_rows(const float* f)   // a 3x4 row-major constant (CSceneTransform, a palette matrix)
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
struct BoneCap { int slot{ 0 }; glm::vec3 a{ 0.0f }, b{ 0.0f }; float r{ 0.1f }; int region{ 0 }; int n{ 0 }; };
struct DrawScan
{
    bool                 ok{ false };
    std::vector<BoneCap> caps;
    glm::vec3            lo{ 1e9f }, hi{ -1e9f };   // its vertices' extent (rest space)
    int                  verts{ 0 };
    std::string          why;                       // not ok: why
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
    c.r = r;
    c.n = int(pts.size());
    // The part by its rest place (the rest pose stands on its root, z up): head, legs, arms out to the sides, else the torso.
    c.region = m.z > 1.45f && std::abs(m.x) < 0.25f ? 1 : m.z < 0.85f ? 3 : std::abs(m.x) > 0.22f ? 2 : 0;
    return true;
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
    if (cmd == "meleebones") {
        m_mel_bones = arg != "off" && arg != "0";
        persist     = true;
        spdlog::info("[Melee] meleebones {}", m_mel_bones.load() ? "on (bodies from their own bones)" : "off (standing capsules)");
        return true;
    }
    if (cmd == "meleestrength") { m_mel_strength = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleepad") { m_mel_pad = num(0.0f, 0.3f, 0.04f); persist = true; return true; }
    if (cmd == "meleeheadx") { m_mel_head_x = num(0.1f, 10.0f, 1.5f); persist = true; return true; }
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
    if (cmd == "meleestill") { m_mel_still = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleepitch") { m_mel_pitch = num(-180.0f, 180.0f, 40.0f); persist = true; return true; }
    if (cmd == "meleeyaw") { m_mel_yaw = num(-180.0f, 180.0f, 0.0f); persist = true; return true; }
    if (cmd == "meleeroll") { m_mel_roll = num(-180.0f, 180.0f, 0.0f); persist = true; return true; }
    if (cmd == "meleeblade") { m_mel_blade = num(0.1f, 1.5f, 0.70f); persist = true; return true; }
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
    if (cmd == "meleecool") { m_mel_cool_ms = int(num(150.0f, 3000.0f, 650.0f)); persist = true; return true; }
    if (cmd == "meleebuzz") { m_mel_buzz_amp = num(0.0f, 1.0f, 1.0f); persist = true; return true; }
    if (cmd == "meleeaim") { m_mel_aim_on = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleeslew") { m_mel_slew = num(1.0f, 90.0f, 60.0f); persist = true; return true; }
    if (cmd == "meleeearly") { m_mel_early_ms = int(num(0.0f, 400.0f, 100.0f)); persist = true; return true; }
    if (cmd == "meleedamage") {
        const int d = std::clamp(std::atoi(arg.c_str()), 0, 3);
        m_mel_dmg   = d;
        persist     = true;
        const bool ok = fc5::lua::damage_watch(d > 0);
        m_mel_dmg_watching = d > 0 && ok;
        spdlog::info("[Melee] meleedamage {} ({}){}", d, d == 0 ? "off" : d == 1 ? "the NPC hit found, no damage" : d == 2 ? "damage at contact and the game's melee" : "damage at contact only",
                     d > 0 && !ok ? ": the game's code is not ready yet (tried again in play)" : "");
        return true;
    }
    if (cmd == "meleeray") { m_mel_ray = arg != "off" && arg != "0"; persist = true; spdlog::info("[Melee] meleeray {}", m_mel_ray.load() ? "on" : "off"); return true; }
    if (cmd == "meleecollide") { m_mel_collide = arg != "off" && arg != "0"; persist = true; spdlog::info("[Melee] meleecollide {}", m_mel_collide.load() ? "on" : "off"); return true; }
    if (cmd == "meleeraysound") { m_mel_ray_sound = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleewallbuzz") { m_mel_wall_buzz = num(0.0f, 1.0f, 0.4f); persist = true; return true; }
    if (cmd == "meleeraymask") { fc5::gameray::set_mask(uint32_t(std::strtoul(arg.c_str(), nullptr, 0))); return true; }
    if (cmd == "meleerayfine") { fc5::gameray::set_fine(arg != "off" && arg != "0"); return true; }   // test (default on): a ray must meet a body part, not only the outer capsule
    if (cmd == "meleerayflags") { fc5::gameray::set_flags(std::atoi(arg.c_str())); return true; }
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
    if (cmd == "meleedamagehp") { m_mel_dmg_hp = num(1.0f, 100000.0f, 200.0f); persist = true; return true; }
    if (cmd == "meleedamageself") {   // test: the damage stim sent to the player from itself (meleedamage on)
        fc5::lua::damage_self(num(1.0f, 1000.0f, 10.0f));
        return true;
    }
    if (cmd == "meleedamageprobe") {   // test: the recorded entities nearest the game camera (meleedamage on)
        fc5::lua::probe_entities(m_world_cam_pos.x, m_world_cam_pos.y, m_world_cam_pos.z);
        return true;
    }
    if (cmd == "meleedamagestatus") { spdlog::info("[Melee] meleedamage {}: {}", m_mel_dmg.load(), fc5::lua::damage_status()); return true; }
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

// The made pose's turn, the weapon's axes to the game camera's: its length (+Y) along -Z (the aim's axis, as a gun's barrel) with
// its +Z up, rolled about it, turned right by meleeyaw and up by meleepitch.
glm::quat FarCry5Stereo::melee_hold_rot() const
{
    const glm::quat base  = glm::angleAxis(glm::radians(-90.0f), glm::vec3{ 1.0f, 0.0f, 0.0f });   // +Y -> -Z, +Z -> +Y
    const glm::quat roll  = glm::angleAxis(glm::radians(m_mel_roll.load()), glm::vec3{ 0.0f, 0.0f, -1.0f });
    const glm::quat yaw   = glm::angleAxis(glm::radians(-m_mel_yaw.load()), glm::vec3{ 0.0f, 1.0f, 0.0f });
    const glm::quat pitch = glm::angleAxis(glm::radians(m_mel_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f });
    return glm::normalize(pitch * yaw * roll * base);
}

glm::quat FarCry5Stereo::melee_collide_rot()
{
    if (!m_mel_collide.load() || !m_mel_ray.load()) return glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    std::lock_guard _{ m_mel_mtx };
    return m_mel_col_q;
}

// world_character_draw: a skinned character draw of the world pass; obj = its object transform (VS b1, 3x4 rows, world), cbs = its VS
// constant buffers 0-4 (b2: the vertex scale, b3: the palette).
void FarCry5Stereo::melee_note_character(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, const float* obj, ID3D11Buffer* const* cbs)
{
    const glm::vec3 origin{ obj[3], obj[7], obj[11] };
    const glm::vec3 rel = origin - m_world_cam_pos;
    const float     horiz = std::sqrt(rel.x * rel.x + rel.y * rel.y);
    if (horiz > 20.0f || std::abs(rel.z) > 6.0f) return;   // (20 m: meleelog shows who is around)
    const bool self = horiz < 0.5f && rel.z < 0.3f;   // the player's own (its root 4 cm from the camera, 1.6 m below; author 25 Oct 11:18 log: a 0.33 m away hit by his own hand)
    const int  dump = m_mel_dump.load();
    if (self && dump < 2) return;
    const int frame = m_frame.load();

    // meleebones: this draw's bones as capsules (its rest-pose fit, posed by its palette now).
    std::vector<MeleeCap> caps;
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
                        per[heavy].push_back(p);
                        sc.lo = glm::min(sc.lo, p);
                        sc.hi = glm::max(sc.hi, p);
                        sc.verts++;
                    }
                    for (const auto& [slot, pts] : per) {
                        BoneCap c{};
                        c.slot = slot;
                        if (slot < 256 && fit_capsule(pts, c)) sc.caps.push_back(c);
                    }
                    sc.ok = !sc.caps.empty();
                    if (!sc.ok) sc.why = fmt::format("{} vertices, no slot with 10 or more", sc.verts);
                }
            }
            sc.ok ? st.scanned++ : st.failed++;
            if (st.said.size() < 64 && st.said.insert(count).second) {
                if (sc.ok) {
                    int nr[4]{};
                    for (const auto& c : sc.caps) nr[c.region]++;
                    spdlog::info("[Melee] meleebones: draw {} ({} vertices, rest {:.2f}..{:.2f} m high, {:.2f} m wide): {} bones ({} head, {} torso, {} arm, {} leg)", count, sc.verts, sc.lo.z,
                                 sc.hi.z, sc.hi.x - sc.lo.x, sc.caps.size(), nr[1], nr[0], nr[2], nr[3]);
                } else {
                    spdlog::info("[Melee] meleebones: draw {} not read ({}): its body stays the standing capsule", count, sc.why);
                }
            }
            si = st.scans.emplace(key, std::move(sc)).first;
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
                    caps.push_back(MeleeCap{ xform(M, c.a) - m_world_cam_pos, xform(M, c.b) - m_world_cam_pos, c.r, c.region });
                }
            }
        }
    }

    std::lock_guard _{ m_mel_mtx };
    if (frame != m_mel_cur_frame) {
        if (m_mel_cur_frame >= 0 && frame > m_mel_cur_frame) {
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
            return;
        }
    }
    if (m_mel_cur.size() < 48) m_mel_cur.push_back(MeleeBody{ rel, false, std::move(caps) });
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
    // The press: once the aim is on the point hit (or after meleewait), held meleepressms, the aim kept 150 ms more.
    if (m_mel_state == 1) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_state_t).count();
        if (m_mel_aimed || !m_mel_aim_on.load() || since >= m_mel_wait_ms.load()) {
            m_mel_press_until = ticks_now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(m_mel_press_ms.load())).count();
            m_mel_presses++;
            spdlog::info("[Melee] pressed {} ms after the hit ({}; the game's aim {:.1f} deg from the point hit): {} for {} ms", since, m_mel_aimed ? "the game's aim on the point hit" : m_mel_aim_on.load() ? "meleewait: the aim not there yet" : "meleeaim off", m_mel_aim_err,
                         button_name(m_mel_button.load(), m_mel_held_melee.load()), m_mel_press_ms.load());
            m_mel_state   = 2;
            m_mel_state_t = now;
        }
    } else if (m_mel_state == 2 && now - m_mel_state_t > std::chrono::milliseconds(m_mel_press_ms.load() + 150)) {
        m_mel_state = 0;
    }
    const int e = m_frame.load() & 1;   // this present's eye
    if (m_mel_dmg.load() > 0 && mode > 0 && !m_mel_dmg_watching && m_player_views_last_frame != 0) m_mel_dmg_watching = fc5::lua::damage_watch(true);
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
    }
    // meleebonedump: the bodies of a frame noted after the command, with their capsules.
    if (const int dump = m_mel_dump.load(); dump > 0 && bodies_frame >= m_mel_dump_at) {
        m_mel_dump = 0;
        spdlog::info("[Melee] meleebonedump: frame {}, {} bodies within 20 m (meleebones {}; {} draws read, {} not, {} posed, {} without a palette copy)", bodies_frame, bodies.size(),
                     m_mel_bones.load() ? "on" : "off", m_mel_scan ? m_mel_scan->scanned : 0, m_mel_scan ? m_mel_scan->failed : 0, m_mel_scan ? m_mel_scan->posed : 0,
                     m_mel_scan ? m_mel_scan->no_palette : 0);
        auto list = [&](const char* who, const glm::vec3& root, const std::vector<MeleeCap>& caps) {
            float top = -1e9f, bottom = 1e9f;
            int   nr[4]{};
            for (const auto& c : caps) {
                top    = std::max({ top, c.a.z - root.z + c.r, c.b.z - root.z + c.r });
                bottom = std::min({ bottom, c.a.z - root.z - c.r, c.b.z - root.z - c.r });
                nr[std::clamp(c.region, 0, 3)]++;
            }
            spdlog::info("[Melee]   {} root ({:.2f}, {:.2f}, {:.2f}) from the camera: {} capsules ({} head, {} torso, {} arm, {} leg){}", who, root.x, root.y, root.z, caps.size(), nr[1], nr[0], nr[2], nr[3],
                         caps.empty() ? std::string{ ": the standing capsule" } : fmt::format(", {:.2f} to {:.2f} m above the root", bottom, top));
            for (size_t k = 0; k < caps.size() && k < 40; ++k) {
                const auto& c = caps[k];
                spdlog::info("[Melee]     {:<5} ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f}) above the root, r {:.2f}", region_name(c.region), c.a.x - root.x, c.a.y - root.y, c.a.z - root.z,
                             c.b.x - root.x, c.b.y - root.y, c.b.z - root.z, c.r);
            }
        };
        for (const auto& b : bodies) if (!b.test) list("body", b.rel, b.caps);
        if (dump >= 2) list("the player's own body (never hit)", self_root, self_caps);
    }
    const bool use_ray = m_mel_ray.load();
    if (use_ray) bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [](const MeleeBody& b) { return !b.test; }), bodies.end());   // meleeray: the game's rays instead (the test body stays)
    // The weapon's segment from the game camera: the stock end behind the hand to the tip ahead (the bare hand: a fist).
    const bool      armed = !m_unarmed && !m_mo.slung && !m_mo.kept;   // twohandkeep: the right hand holds nothing
    // meleestill: a melee weapon's blade, along its made pose's length (the game camera's axes are the controller's).
    const bool      blade = armed && melee_still_now() && melee_weapon(m_weapon_sig.load());
    m_mel_held_melee      = armed && melee_weapon(m_weapon_sig.load());
    const glm::vec3 hand  = ((m_mo.slung || m_mo.kept) ? m_mo.hand_pos_world : m_mo.ctrl_pos_world) - m_world_cam_pos;
    glm::vec3       dir   = glm::length(m_mo.barrel_dir) > 0.5f ? glm::normalize(m_mo.barrel_dir) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    if (blade) dir = glm::normalize(m_mo.ctrl_rot_world * (glm::mat3_cast(melee_hold_rot()) * glm::vec3{ 0.0f, 1.0f, 0.0f }));
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
        if (m_mel_v_hand > kJumpSpeed || m_mel_v_tip > 2.0f * kJumpSpeed) {   // tracking lost and found, a recentre: no swing
            if (now - m_mel_jump_said > std::chrono::seconds(5)) {
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
                if (d < out.d) { out.d = d; out.s = s; out.at = c.a + (c.b - c.a) * t; out.region = c.region; out.p = a0 + (a1 - a0) * s; }
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
    const int   dmg   = m_mel_dmg.load();
    // The game's melee pressed at a hit (or, with meleeearly, before it): the aim turned onto the point hit first (on the standing
    // capsule's axis 0.9-1.6 m up; with the bones the point of the bone hit).
    auto press = [&](const MeleeBody& b, const Touch& t) {
        m_mel_target = m_world_cam_pos + (t.region >= 0 ? t.at : b.rel + glm::vec3{ 0.0f, 0.0f, std::clamp(t.at.z - b.rel.z, 0.9f, 1.6f) });
        if (mode != 2 || dmg == 3) return;   // meleedamage 3: the damage alone, no game melee
        m_mel_state   = 1;
        m_mel_state_t = now;
        m_mel_aimed   = false;
    };
    auto part = [](const MeleeBody& b, const Touch& t) {
        return t.region >= 0 ? fmt::format("the {} ({} bone capsules)", region_name(t.region), b.caps.size()) : std::string{ "the standing capsule" };
    };
    // meleeearly (26 Oct, author 12:44): the game's swing lands about 0.2 s
    // after the press (simulator 25 Oct: the punch out 0.2 s after it), so the press goes when the weapon, moving on as it moves now,
    // will enter a body within meleeearly ms; the pulse and meleedamage still wait for the contact itself.
    const int early_ms = m_mel_early_ms.load();
    if (mode == 2 && dmg != 3 && early_ms > 0 && have && speed > m_melee_speed.load() && now - m_mel_last_hit > std::chrono::milliseconds(m_mel_cool_ms.load())) {
        const glm::vec3 v0 = (p0 - q0) / dt, v1 = (p1 - q1) / dt;
        int   eb = -1;
        float et = 0.0f;
        Touch etouch{};
        for (size_t i = 0; i < bodies.size() && eb < 0; ++i) {
            if (m_mel_inside[i]) continue;
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
            if (mode == 2 && dmg != 3 && early_ms > 0 && speed > m_melee_speed.load()) seg(p1, p1 + (p1 - q1) / dt * (0.001f * float(early_ms)), true);
        }
        fc5::gameray::submit(segs, ns);
        fc5::gameray::Result res{};
        if (fc5::gameray::take(res)) {
            // meleecollide: the blade from the hand with a surface on it at s metres: it goes where its tip just rests on that plane: its height above the plane kept, the rest along the surface.
            glm::quat col_want{ 1.0f, 0.0f, 0.0f, 0.0f };
            if (m_mel_collide.load() && blade && res.blade.any && reach > 0.05f) {
                const float     s = res.blade.along - 0.08f;   // (the first segment starts 8 cm behind the hand)
                const glm::vec3 n{ res.blade.nx, res.blade.ny, res.blade.nz };
                const float     dn = glm::dot(dir, n);
                if (s > 0.02f && s < reach && dn < -0.01f) {
                    glm::vec3   t  = dir - n * dn;
                    const float tl = glm::length(t);
                    // Head on: the side it last went, else up the surface
                    t = tl > 0.05f ? t / tl : glm::normalize(glm::vec3{ 0.0f, 0.0f, 1.0f } - n * n.z + glm::vec3{ 1e-3f, 0.0f, 0.0f });
                    const float     h    = std::min(s * -dn, reach);
                    const glm::vec3 want = glm::normalize(t * std::sqrt(std::max(reach * reach - h * h, 0.0f)) - n * h);
                    // The turn in the controller's axes, where the made pose is built
                    const glm::mat3 Cr   = m_mo.ctrl_rot_world;
                    const glm::mat3 CrT  = glm::transpose(Cr);
                    const glm::vec3 from = glm::normalize(CrT * dir), to = glm::normalize(CrT * want), ax = glm::cross(from, to);
                    col_want             = glm::normalize(glm::quat{ 1.0f + glm::dot(from, to), ax.x, ax.y, ax.z });
                }
            }
            {
                std::lock_guard _{ m_mel_mtx };
                m_mel_col_q = glm::normalize(glm::slerp(m_mel_col_q, col_want, 0.5f));
            }
            const bool over = speed > m_melee_speed.load();
            const bool cool = now - m_mel_last_hit > std::chrono::milliseconds(m_mel_cool_ms.load());
            auto point      = [&](const fc5::gameray::Hit& h) { return glm::vec3{ h.x, h.y, h.z }; };
            auto hit_sound  = [&]() {
                if (!armed || !m_mel_ray_sound.load()) return;
                const auto* w = fc5::gameweapon::info(m_weapon_sig.load());
                fc5::gamesound::post(w && w->holster == 1 ? 2040638u : 2276490191u, 15);
            };
            auto aim_press  = [&](const glm::vec3& at) {
                m_mel_target = at;
                if (mode != 2 || dmg == 3) return;
                m_mel_state   = 1;
                m_mel_state_t = now;
                m_mel_aimed   = false;
            };
            if (res.body_ahead.any && over && cool && !res.body.any) {
                m_mel_last_hit = now;
                m_mel_early_t  = now;
                m_mel_swings++;
                spdlog::info("[Melee] early press (the game's ray): {} {:.1f} m/s will reach entity {:x} (health {:.0f}) (meleeearly {})", armed ? "the weapon" : "the hand", speed, res.body_ahead.entity, res.body_ahead.health, early_ms);
                aim_press(point(res.body_ahead));
            }
            if (res.body.any) {
                const bool fresh = res.body.entity != m_mel_ray_ent || now - m_mel_ray_t > std::chrono::milliseconds(250);   // still touching the same one: no new hit
                m_mel_ray_ent    = res.body.entity;
                m_mel_ray_t      = now;
                const bool after = now - m_mel_early_t < std::chrono::milliseconds(early_ms + 250);
                if (fresh && over && (cool || after)) {
                    m_mel_swings++;
                    m_mel_hits++;
                    m_mel_last_hit = now;
                    const glm::vec3 at = point(res.body);
                    spdlog::info("[Melee] hit (the game's ray): {}{} {:.1f} m/s into entity {:x} (health {:.0f}, part {}) at ({:.2f}, {:.2f}, {:.2f}) from the camera, normal ({:.2f}, {:.2f}, {:.2f}); {}", armed ? "the weapon" : "the hand",
                                 blade ? fmt::format(" ({}, the blade)", m_weapon_sig.load()) : std::string{}, speed, res.body.entity, res.body.health, res.body.part, at.x - c.x, at.y - c.y, at.z - c.z, res.body.nx, res.body.ny, res.body.nz,
                                 after ? std::string{ "after the early press" } : mode == 2 && dmg != 3 ? std::string{ "the game's melee next" } : mode == 2 ? std::string{ "damage only (meleedamage 3)" } : std::string{ "log only (meleehit 1)" });
                    m_mel_early_t = {};
                    m_mel_buzz    = m_mel_buzz_amp.load();
                    if (mode != 2 || dmg == 3) hit_sound();
                    if (dmg > 0) fc5::lua::queue_damage_id(res.body.entity, m_mel_dmg_hp.load(), dmg >= 2 && mode == 2);
                    if (!after) aim_press(at);
                }
            } else if (res.wall.any && over && now - m_mel_wall_t > std::chrono::milliseconds(250)) {
                m_mel_walls++;
                const glm::vec3 at = point(res.wall);
                spdlog::info("[Melee] wall (the game's ray): {} {:.1f} m/s met entity {:x} at ({:.2f}, {:.2f}, {:.2f}) from the camera, normal ({:.2f}, {:.2f}, {:.2f})", armed ? "the weapon" : "the hand", speed, res.wall.entity, at.x - c.x, at.y - c.y, at.z - c.z, res.wall.nx, res.wall.ny, res.wall.nz);
                if (m_mel_wall_buzz.load() > 0.0f) m_mel_buzz = m_mel_wall_buzz.load();
                hit_sound();
            }
            if (res.wall.any) m_mel_wall_t = now;   // still against it: one pulse
        }
    }
    // meleebones also judges the contact as Chasm's framework does: the speed is that of the weapon's point that met the body (not the
    // hand's or the tip's), and it must be moving into the body (towards the bone's axis), so a weapon pulled out or slid along a body
    // is no blow. meleestrength: the damage and the pulse grow with that speed, from meleespeed (a quarter) to 7 m/s (6 for the fist:
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
    if (best >= 0 && have && (judged ? impact : speed) > vmin && into) {
        m_mel_swings++;
        const bool  cool  = now - m_mel_last_hit > std::chrono::milliseconds(m_mel_cool_ms.load());
        const bool  after = now - m_mel_early_t < std::chrono::milliseconds(early_ms + 250);   // the contact of an early press
        const auto& b     = bodies[best];
        const float h     = best_t.at.z - b.rel.z;
        if (cool || after) {
            m_mel_hits++;
            m_mel_last_hit = now;
            m_mel_hit_p    = m_world_cam_pos + best_t.at;
            m_mel_hit_t    = now;
            const int64_t since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_early_t).count();
            spdlog::info("[Melee] hit{}: {}{} {:.1f} m/s at the contact, strength {:.2f} (hand {:.1f}, tip {:.1f}, body axes) into {} of the body at ({:.2f}, {:.2f}, {:.2f}) from the camera ({:.2f} m across the ground), "
                         "{:.2f} m above its root, {:.2f} m inside, {} of the {} ({} {:.2f} m); {} bodies near; {}",
                         b.test ? " (test body)" : "", armed ? "the weapon" : "the hand", blade ? fmt::format(" ({}, the blade)", m_weapon_sig.load()) : std::string{}, impact, strength, m_mel_v_hand, m_mel_v_tip,
                         part(b, best_t), b.rel.x, b.rel.y, b.rel.z, glm::length(glm::vec2{ b.rel }), h, -best_t.d,
                         best_t.along > 0.8f ? "the tip" : best_t.along < 0.35f && armed ? (blade ? "the hilt" : "the stock end") : "the middle", armed ? "weapon" : "hand", armed ? "reach" : "fist", armed ? reach : r, bodies.size(),
                         after ? fmt::format("{} ms after the early press", since) : mode == 2 && dmg != 3 ? std::string{ "the game's melee next" } : mode == 2 ? std::string{ "damage only (meleedamage 3)" } : std::string{ "log only (meleehit 1)" });
            m_mel_early_t = {};
            const bool str = m_mel_strength.load();
            m_mel_buzz    = m_mel_buzz_amp.load() * (str ? std::min(1.0f, 0.35f + 0.55f * strength) : 1.0f);   // (also in log only: the detection can be felt in the headset)
            // meleedamage: the NPC at this body's root found by the game's own entity positions, and (2, 3) the damage sent now
            // (meleeheadx times it on the head).
            if (dmg > 0 && (!b.test || dmg == 1)) {   // (a test body: found only, never sent)
                const glm::vec3 root = m_world_cam_pos + b.rel;
                const float     hp   = m_mel_dmg_hp.load() * (best_t.region == 1 ? m_mel_head_x.load() : 1.0f) * (str ? strength : 1.0f);
                fc5::lua::queue_damage(root.x, root.y, root.z, hp, dmg >= 2 && mode == 2 && !b.test);
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
    if (b == 3) b = m_mel_held_melee.load() ? 1 : 0;   // auto: a melee weapon swings with RT
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
    hint("Swing your weapon (or your empty hand) into someone, faster than a push: Hit presses the game's melee at that moment, aimed at where "
         "you hit them, with a pulse in the hand. Log only: each hit is felt as the pulse and written to the log ([Melee]), the game does nothing. "
         "A gun counts from just behind your hand to about half a metre ahead of it, a melee weapon along its blade. Off (default): the "
         "subsections below about hits are hidden.");
    if (m != 0) {
        if (sub("Swing and button##melsub")) {
            float sp = m_melee_speed.load();
            if (ImGui::SliderFloat("Swing faster than##meleespeed", &sp, 0.5f, 10.0f, "%.1f m/s")) {
                m_melee_speed = sp;
                keep("meleespeed", fmt::format("meleespeed {:.1f}", sp));
            }
            hint("A hit needs the weapon to move at least this fast (your real swings are 3 to 16 m/s); slower contact, like walking into someone, does nothing.");
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
            hint("Stick click (default): the game's melee and takedown. Auto: the fire trigger with a melee weapon in hand (the game swings a machete, "
                 "a shovel or a bat with it), the stick click with anything else.");
            int early = m_mel_early_ms.load();
            if (ImGui::SliderInt("Press the game's melee before contact##meleeearly", &early, 0, 300, "%d ms")) {
                m_mel_early_ms = early;
                keep("meleeearly", fmt::format("meleeearly {}", early));
            }
            hint("The game's own melee lands about 0.2 s after it is pressed: it is pressed this long before your swing will reach the body, so its blow lands "
                 "closer to your contact. 0: pressed at the contact. Too long: a swing you stop short still makes the game hit.");
            ImGui::Unindent(16.0f);
        }
        if (sub("What counts as a hit##melsub")) {
            bool ry = m_mel_ray.load();
            if (ImGui::Checkbox("Hits from the game's physics (test)##meleeray", &ry)) {
                m_mel_ray = ry;
                keep("meleeray", fmt::format("meleeray {}", ry ? "on" : "off"));
            }
            hint("Instead of a capsule round each character, the game's own physics ray along your weapon and its path: hits the real body, crouched or lying too, and knows who it hit. "
                 "Off (default): the capsules below.");
            bool bo = m_mel_bones.load();
            if (ImGui::Checkbox("Hit the body's own shape (head, torso, arms, legs)##meleebones", &bo)) {
                m_mel_bones = bo;
                keep("meleebones", bo ? "meleebones on" : "meleebones off");
            }
            hint("On (default): someone within 4 m is hit where their head, torso, arms and legs are drawn, crouching, lying or turning, from their "
                 "own animated bones. Off: everyone is a standing cylinder over their feet, 30 cm round.");
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
            hint("Dots over the picture: each body's capsules (white torso, red head, yellow arms, green legs) and your weapon's hit line (cyan, red "
                 "for a moment after a contact). Not saved.");
            ImGui::Unindent(16.0f);
        }
        if (sub("Damage##melsub")) {
            int d = m_mel_dmg.load();
            ImGui::TextUnformatted("Damage at the moment of contact");
            bool dc = ImGui::RadioButton("Off##meleedamage", &d, 0);
            ImGui::SameLine();
            dc |= ImGui::RadioButton("Find the NPC only##meleedamage", &d, 1);
            ImGui::SameLine();
            dc |= ImGui::RadioButton("Damage + game melee##meleedamage", &d, 2);
            ImGui::SameLine();
            dc |= ImGui::RadioButton("Damage only##meleedamage", &d, 3);
            if (dc) {
                m_mel_dmg          = d;
                m_mel_dmg_watching = fc5::lua::damage_watch(d > 0) && d > 0;
                keep("meleedamage", fmt::format("meleedamage {}", d));
            }
            hint("At the contact the mod finds the NPC you hit among the game's own entities and sends it the game's damage at once, from you, as a "
                 "bullet would. Damage + game melee (default): the game's blow follows too. Damage only: no game animation at all. Find the NPC only: "
                 "written to the log, no damage.");
            if (d >= 2) {
                float hp = m_mel_dmg_hp.load();
                if (ImGui::SliderFloat("Damage per hit##meleedamagehp", &hp, 10.0f, 2000.0f, "%.0f")) {
                    m_mel_dmg_hp = hp;
                    keep("meleedamagehp", fmt::format("meleedamagehp {:.0f}", hp));
                }
            }
            if (m_mel_bones.load()) {
                float hx = m_mel_head_x.load();
                if (ImGui::SliderFloat("Head hits: damage x##meleeheadx", &hx, 1.0f, 4.0f, "%.1f")) {
                    m_mel_head_x = hx;
                    keep("meleeheadx", fmt::format("meleeheadx {:.1f}", hx));
                }
                hint("With the body's own shape on, a blow to the head sends this many times the damage per hit.");
            }
            bool sg = m_mel_strength.load();
            if (ImGui::Checkbox("Harder swings hit harder##meleestrength", &sg)) {
                m_mel_strength = sg;
                keep("meleestrength", sg ? "meleestrength on" : "meleestrength off");
            }
            hint("On (default): the damage at contact and the pulse in the hand grow with how fast the part of the weapon that met the body was "
                 "moving: a quarter just over 'Swing faster than', full at 7 m/s (a fist 6), up to 1.5 times past it. Off: every hit the same.");
            ImGui::Unindent(16.0f);
        }
        if (m_mel_ray.load() && sub("Pulse and sound on walls##melsub")) {
            float wb = m_mel_wall_buzz.load();
            if (ImGui::SliderFloat("Pulse on walls and objects##meleewallbuzz", &wb, 0.0f, 1.0f, "%.2f")) {
                m_mel_wall_buzz = wb;
                keep("meleewallbuzz", fmt::format("meleewallbuzz {:.2f}", wb));
            }
            hint("A short pulse in the hand when a swing meets anything that is not a character. 0: none.");
        bool mc = m_mel_collide.load();
        if (ImGui::Checkbox("The melee weapon stops at walls and objects (test)##meleecollide", &mc)) {
            m_mel_collide = mc;
            keep("meleecollide", fmt::format("meleecollide {}", mc ? "on" : "off"));
        }
        hint("A melee weapon's blade lies against what it meets instead of going through, turned about your hand. Your hand stays where it is. Melee weapons only.");
            bool rs = m_mel_ray_sound.load();
            if (ImGui::Checkbox("The game's hit sound at a contact##meleeraysound", &rs)) {
                m_mel_ray_sound = rs;
                keep("meleeraysound", fmt::format("meleeraysound {}", rs ? "on" : "off"));
            }
            hint("The game's own melee hit sound when your weapon meets a wall or object, or a character when the game's blow is not pressed. Not with bare hands. "
                 "Both only with hits from the game's physics.");
            ImGui::Unindent(16.0f);
        }
    }
    if (sub("Melee weapons in the hand##melsub")) {
        // meleestill: melee weapons in the hand (with or without meleehit).
        bool st = m_mel_still.load();
        if (ImGui::Checkbox("Melee weapons: lined up with the controller, no game animation##meleestill", &st)) {
            m_mel_still = st;
            keep("meleestill", st ? "meleestill on" : "meleestill off");
        }
        hint("A machete, a shovel or a bat is held at a fixed pose on your controller, its length along the angles below, and the game's own "
             "idle, running and swing animations of the weapon and your hands are taken out: the weapon moves only with your hand. Off (default): "
             "the game's animations show.");
        if (st) {
            float p = m_mel_pitch.load(), y = m_mel_yaw.load(), r = m_mel_roll.load();
            if (ImGui::SliderFloat("Melee weapon: tilt up##meleepitch", &p, -90.0f, 135.0f, "%.0f deg")) {
                m_mel_pitch = p;
                keep("meleepitch", fmt::format("meleepitch {:.0f}", p));
            }
            if (ImGui::SliderFloat("Melee weapon: turn right##meleeyaw", &y, -90.0f, 90.0f, "%.0f deg")) {
                m_mel_yaw = y;
                keep("meleeyaw", fmt::format("meleeyaw {:.0f}", y));
            }
            if (ImGui::SliderFloat("Melee weapon: roll##meleeroll", &r, -180.0f, 180.0f, "%.0f deg")) {
                m_mel_roll = r;
                keep("meleeroll", fmt::format("meleeroll {:.0f}", r));
            }
            if (ImGui::Button("Default angles##meleeangles")) {
                m_mel_pitch = 40.0f;
                m_mel_yaw   = 0.0f;
                m_mel_roll  = 0.0f;
                keep("meleepitch", "meleepitch 40");
                keep("meleeyaw", "meleeyaw 0");
                keep("meleeroll", "meleeroll 0");
            }
            hint("0 tilt: the weapon points where a gun's barrel would. Tilt it up until the blade continues your fist; roll turns the edge.");
        }
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
            hint("Found by itself: a weapon the game holds low at your side (machete, shovel, bat) counts as a melee weapon.");
        }
        ImGui::Unindent(16.0f);
    }
}
