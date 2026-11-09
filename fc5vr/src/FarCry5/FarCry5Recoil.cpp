// recoil (author 7 Nov 20:13). See FarCry5Recoil.h.
//
// What the kick is. Each of the player's shots (the game's shot code, the call shotbuzz already uses) starts one kick: the gun moves back
// along its barrel and a little up, and its muzzle turns up about the grip, with a small random turn to one side and roll; then it
// settles back. Each kick follows the same curve, a fast rise (rise_s) and a slower settle (fall_s), at most 1 at its peak:
//     k(t) = (1 - e^(-t/rise)) e^(-t/fall) / peak
// The kicks of a burst add up (a fast automatic sits a little further back and higher while it fires), and their sum is eased into at most
// kCap single kicks (cap tanh(sum / cap)), so a long burst neither climbs without end nor stops moving.
// Its size by the game's weapon class (FarCry5WeaponTable.h): a handgun flips its muzzle most, a shotgun or a sniper rifle kicks hardest
// and longest, an SMG and a machine gun kick little per shot; a few heavy guns (D50, revolvers, the elephant gun, .50 rifles, the 1887)
// half as much again. None for melee weapons, bows, throwables, the flamethrower and the tools.
//
// Where it is applied (FarCry5Stereo.cpp, the weapon pass's camera): to the gun and the hands drawn with it, about the grip point put on the
// controller, after gunstill's hold. recoilaim (default on; author 8 Nov 08:55): update_motion turns
// the shots' line (the muzzle, the aim or sight line, its direction) by the same kick about the same point, so the bullets, the red dot
// and the aim loop follow the kicked gun; a shot starts its kick only after it has left, so the next shots of a burst climb with it.
// Off: the shots, the culled camera and the aim error are as without recoil. The scope picture rides on the glass and kicks with it
// (less: recoilscope).
//
// Lighter with two hands (recoiltwohand), and the shot pulse in the hands follows the weapon (recoilbuzz).
#include "FarCry5Recoil.h"
#include "FarCry5GameWeapon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

namespace fc5::recoil
{
namespace
{
    using clock = std::chrono::steady_clock;
    int64_t now_ns() { return clock::now().time_since_epoch().count(); }

    std::atomic<bool>  g_on{ true };       // recoil (saved; default on for Beta 0.4.6, author 9 Nov 11:34)
    std::atomic<bool>  g_buzz{ true };     // recoilbuzz (saved)
    std::atomic<bool>  g_aim{ true };      // recoilaim (saved): the shots follow the kicked gun
    std::atomic<bool>  g_log{ false };     // recoillog (not saved)
    std::atomic<float> g_scale{ 1.0f };    // recoilscale (saved, 0-2)
    std::atomic<float> g_two{ 0.6f };      // recoiltwohand (saved, 0-1): the kick's share while both hands hold the gun
    std::atomic<float> g_scope{ 0.4f };    // recoilscope (saved, 0-1): the kick's share while the eye is behind a magnifying scope

    // The player's shots: steady_clock ns and a random word each, written by the game thread, read by the render thread.
    constexpr int           kShots = 32;
    std::atomic<int64_t>    g_shot_t[kShots]{};
    std::atomic<uint32_t>   g_shot_r[kShots]{};
    std::atomic<uint32_t>   g_shot_n{ 0 };
    std::atomic<int64_t>    g_shot_last{ 0 };
    std::atomic<uint32_t>   g_seed{ 0x2545F491u };
    // recoiltest N MS: N kicks MS apart from now, with no shot fired (simulator checks and captures without firing).
    std::atomic<int64_t>    g_test_t0{ 0 };
    std::atomic<int>        g_test_n{ 0 }, g_test_ms{ 100 };

    constexpr float kCap = 1.6f;   // a burst's kick: at most this many single kicks

    struct Profile
    {
        const char* kind;
        float       back_cm, lift_cm, pitch_deg, yaw_deg, roll_deg;   // one kick at its peak
        float       rise_s, fall_s;
        float       buzz_amp, buzz_len;                               // recoilbuzz: the shot pulse's strength and length, times
    };
    //                                   kind               back  lift  pitch  yaw   roll   rise    fall   buzz  len
    constexpr Profile kHandgun        { "handgun",          2.0f, 0.8f, 7.0f,  1.2f, 2.0f, 0.018f, 0.075f, 0.80f, 1.0f };
    constexpr Profile kMachinePistol  { "machine pistol",   1.4f, 0.5f, 4.0f,  1.2f, 1.5f, 0.015f, 0.060f, 0.70f, 0.8f };
    constexpr Profile kSmg            { "SMG",              1.4f, 0.4f, 2.2f,  0.8f, 1.0f, 0.015f, 0.060f, 0.70f, 0.8f };
    constexpr Profile kRifle          { "rifle",            2.2f, 0.5f, 2.8f,  0.8f, 1.0f, 0.018f, 0.080f, 0.90f, 1.0f };
    constexpr Profile kShotgun        { "shotgun",          4.0f, 1.0f, 7.0f,  1.2f, 2.0f, 0.025f, 0.140f, 1.00f, 1.5f };
    constexpr Profile kSniper         { "sniper rifle",     3.5f, 0.8f, 5.0f,  0.8f, 1.5f, 0.025f, 0.140f, 1.00f, 1.5f };
    constexpr Profile kMachineGun     { "machine gun",      1.6f, 0.4f, 1.8f,  1.0f, 1.0f, 0.015f, 0.070f, 0.85f, 0.9f };
    constexpr Profile kLauncher       { "launcher",         3.5f, 1.2f, 5.0f,  0.8f, 1.0f, 0.035f, 0.200f, 1.00f, 2.0f };
    constexpr Profile kHarpoon        { "harpoon",          2.5f, 0.6f, 3.0f,  0.8f, 1.0f, 0.020f, 0.100f, 0.90f, 1.2f };

    // The game's weapon class (selWeaponClass) to a profile; heavy: 1.5 for the guns that kick hardest in their class, else 1.
    const Profile* profile(uint32_t sig, float& heavy)
    {
        heavy = 1.0f;
        const auto* w = fc5::gameweapon::info(sig);
        if (!w) return nullptr;
        const Profile* p = nullptr;
        switch (w->cls) {
        case 1: p = &kHandgun; break;
        case 2: p = &kRifle; break;
        case 3: p = w->one_handed ? &kMachinePistol : &kSmg; break;
        case 4: p = &kShotgun; break;
        case 5: p = &kSniper; break;
        case 6: p = &kMachineGun; break;
        case 7: p = &kLauncher; break;
        case 17: p = &kHarpoon; break;
        default: return nullptr;   // melee 0, flamethrower 9, C4 11, mines 12, torch 13, bow 15, fishing rod 18, fists 20, ...
        }
        static const char* const kHeavy[]{ "/D50", "/Cannon", "Revolver", "Elephantgun", "700Nitro", "/AMR", "SA-50", "/1887" };
        if (w->name)
            for (const char* h : kHeavy)
                if (std::strstr(w->name, h)) { heavy = 1.5f; break; }
        return p;
    }

    float env(float t, float rise, float fall)
    {
        if (t < 0.0f) return 0.0f;
        const float tp   = rise * std::log1p(fall / rise);
        const float peak = (1.0f - std::exp(-tp / rise)) * std::exp(-tp / fall);
        return (1.0f - std::exp(-t / rise)) * std::exp(-t / fall) / peak;
    }
    float rnd(uint32_t r, int which)   // -1..1 from 10 bits of the shot's random word
    {
        return float((r >> (10 * which)) & 1023u) / 511.5f - 1.0f;
    }

    // Render thread: this update's kick, and recoillog's burst line.
    int       c_key{ INT_MIN };
    bool      c_ok{ false };
    glm::mat3 c_R{ 1.0f };
    glm::vec3 c_T{ 0.0f };
    bool      b_on{ false };
    float     b_back{ 0.0f }, b_lift{ 0.0f }, b_pitch{ 0.0f }, b_yaw{ 0.0f }, b_sum{ 0.0f };
    int       b_shots{ 0 };
    int64_t   b_t0{ 0 };
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "recoil") {
        g_on    = on;
        persist = true;
        spdlog::info("[Recoil] recoil {}: {}", on ? "on" : "off",
                     on ? "each shot kicks the drawn gun back and its muzzle up, then it settles (the shots stay on the controller)" : "no kick on the drawn gun");
        return true;
    }
    if (cmd == "recoilscale") { g_scale = std::clamp(arg.empty() ? 1.0f : fv, 0.0f, 2.0f); persist = true; return true; }
    if (cmd == "recoiltwohand") { g_two = std::clamp(arg.empty() ? 0.6f : fv, 0.0f, 1.0f); persist = true; return true; }
    if (cmd == "recoilscope") { g_scope = std::clamp(arg.empty() ? 0.4f : fv, 0.0f, 1.0f); persist = true; return true; }
    if (cmd == "recoilbuzz") { g_buzz = on; persist = true; return true; }
    if (cmd == "recoilaim") {
        g_aim   = on;
        persist = true;
        spdlog::info("[Recoil] recoilaim {}: {}", on ? "on" : "off", on ? "the shots follow the kicked gun" : "the shots stay on the controller; only the picture kicks");
        return true;
    }
    if (cmd == "recoillog") { g_log = on; return true; }
    if (cmd == "recoiltest") {   // recoiltest [N] [MS]: N kicks (1) MS apart (100) from now, no shot fired; needs recoil on
        int n = arg.empty() ? 1 : std::clamp(int(fv), 1, 30);
        std::string ms;
        rest >> ms;
        g_test_ms = std::clamp(ms.empty() ? 100 : std::atoi(ms.c_str()), 30, 2000);
        g_test_n  = n;
        g_test_t0 = now_ns();
        spdlog::info("[Recoil] recoiltest: {} kick(s) {} ms apart{}", n, g_test_ms.load(), g_on.load() ? "" : " (recoil is off: nothing shows)");
        return true;
    }
    return false;
}

void note_shot()
{
    if (!g_on.load()) return;
    const int64_t now = now_ns();
    if (now - g_shot_last.load() < 30'000'000ll) return;   // 30 ms: a shotgun's pellets are one shot
    g_shot_last = now;
    uint32_t s = g_seed.load();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    g_seed = s;
    const uint32_t i = g_shot_n.fetch_add(1) % kShots;
    g_shot_r[i]      = s;
    g_shot_t[i]      = now;
}

bool kick(int update_key, uint32_t weapon_sig, bool two_hand, bool scope, glm::mat3& R, glm::vec3& T)
{
    if (!g_on.load() || g_scale.load() <= 0.0f) {
        c_key = INT_MIN;
        b_on  = false;
        return false;
    }
    if (update_key == c_key) {
        R = c_R;
        T = c_T;
        return c_ok;
    }
    c_key = update_key;
    c_ok  = false;
    float          heavy = 1.0f;
    const Profile* p     = profile(weapon_sig, heavy);
    const int64_t  now   = now_ns();
    const float    window = 7.0f * (p ? p->fall_s : 0.2f);
    float sum = 0.0f, sum_pitch = 0.0f, sum_yaw = 0.0f, sum_roll = 0.0f;
    int   live = 0;
    auto  add  = [&](int64_t t, uint32_t r) {
        const float age = float(double(now - t) * 1e-9);
        if (age < 0.0f || age > window) return;
        const float f = env(age, p->rise_s, p->fall_s);
        sum += f;
        sum_pitch += f * (1.0f + 0.15f * rnd(r, 2));
        sum_yaw += f * rnd(r, 0);
        sum_roll += f * rnd(r, 1);
        live++;
    };
    if (p) {
        for (int i = 0; i < kShots; ++i)
            if (const int64_t t = g_shot_t[i].load(); t != 0) add(t, g_shot_r[i].load());
        // recoiltest: the kicks due by now.
        if (const int64_t t0 = g_test_t0.load(); t0 != 0)
            for (int i = 0; i < g_test_n.load(); ++i) add(t0 + int64_t(i) * g_test_ms.load() * 1'000'000ll, 0x9E3779B9u * uint32_t(i + 1));
    }
    if (!p || live == 0 || sum < 1e-4f) {
        if (b_on && g_log.load())
            spdlog::info("[Recoil] burst: {} shot(s) over {:.2f} s, peak back {:.1f} cm, up {:.1f} cm, muzzle up {:.1f} deg, aside {:.1f} deg (sum {:.2f} kicks)",
                         b_shots, double(now - b_t0) * 1e-9, b_back, b_lift, b_pitch, b_yaw, b_sum);
        b_on = false;
        return false;
    }
    const float k = kCap * std::tanh(sum / kCap) / sum;
    const float m = g_scale.load() * (two_hand ? g_two.load() : 1.0f) * (scope ? g_scope.load() : 1.0f);
    const float pitch = p->pitch_deg * heavy * sum_pitch * k * m;
    const float yaw   = p->yaw_deg * sum_yaw * k * m;
    const float roll  = p->roll_deg * sum_roll * k * m;
    const float back  = 0.01f * p->back_cm * heavy * sum * k * m;
    const float lift  = 0.01f * p->lift_cm * heavy * sum * k * m;
    // The muzzle (-z) up is a positive turn about x; yaw about the gun's up, roll about its barrel.
    c_R = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(yaw), glm::vec3{ 0, 1, 0 }) * glm::rotate(glm::mat4{ 1.0f }, glm::radians(pitch), glm::vec3{ 1, 0, 0 }) *
                     glm::rotate(glm::mat4{ 1.0f }, glm::radians(roll), glm::vec3{ 0, 0, 1 }) };
    c_T  = glm::vec3{ 0.0f, lift, back };
    c_ok = true;
    R    = c_R;
    T    = c_T;
    if (g_log.load()) {
        if (!b_on) {
            b_on = true;
            b_t0 = now;
            b_back = b_lift = b_pitch = b_yaw = b_sum = 0.0f;
            b_shots = 0;
            spdlog::info("[Recoil] weapon {:08X}: {} (x{:.1f}), strength {:.2f}{}{}", weapon_sig, p->kind, heavy, g_scale.load(), two_hand ? ", two hands" : "",
                         scope ? ", behind the scope" : "");
        }
        b_shots = std::max(b_shots, live);
        b_back  = std::max(b_back, 100.0f * back);
        b_lift  = std::max(b_lift, 100.0f * lift);
        b_pitch = std::max(b_pitch, pitch);
        b_yaw   = std::max(b_yaw, std::abs(yaw));
        b_sum   = std::max(b_sum, sum);
    }
    return true;
}

bool buzz(uint32_t weapon_sig, float& amp_mul, float& len_mul)
{
    amp_mul = len_mul = 1.0f;
    if (!g_on.load() || !g_buzz.load()) return false;
    float          heavy = 1.0f;
    const Profile* p     = profile(weapon_sig, heavy);
    if (!p) return false;
    amp_mul = p->buzz_amp;
    len_mul = p->buzz_len * (heavy > 1.0f ? 1.3f : 1.0f);
    return true;
}

bool aim_follows() { return g_on.load() && g_aim.load(); }

Settings settings()
{
    return Settings{ g_on.load(), g_aim.load(), g_buzz.load(), g_scale.load(), g_two.load(), g_scope.load() };
}

const char* weapon_kind(uint32_t weapon_sig, float& size)
{
    float          heavy = 1.0f;
    const Profile* p     = profile(weapon_sig, heavy);
    size                 = p ? heavy : 0.0f;
    return p ? p->kind : nullptr;
}
}
