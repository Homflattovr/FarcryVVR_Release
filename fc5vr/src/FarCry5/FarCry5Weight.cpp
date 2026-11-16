// weaponweight (see FarCry5Weight.h for the model; research\2026-11-12_nativeweapon). The settings, the weapon's heft from the game's
// weapon class, the render thread's step after the weapon smoothing, and two log lines:
//   [Weight]   every 10 s while a gun with weight is in hand: what it got and how far it trailed the hand.
//   [Handling] every 10 s while a gun is in hand (handlinglog, on by default, with weaponweight 0 too): how far the gun as drawn is from
//              its own controller, by the hold (two hands, the one-hand line, the stock) and by the smoothing and the weight. It is the
//              guard for hand placement: the second part must stay small whatever is switched on.
#include "FarCry5Weight.h"
#include "FarCry5GameWeapon.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <istream>
#include <string>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace fc5::weight
{
namespace
{
    using clock = std::chrono::steady_clock;

    std::atomic<float> g_weight{ 0.0f };      // weaponweight 0-100 (saved; 0 = off: the weapon smoothing alone, as before)
    std::atomic<float> g_inertia{ 38.0f };    // weaponweightinertia ms (saved): a rifle's turn inertia time at 100
    std::atomic<float> g_move{ 0.45f };       // weaponweightmove 0-1 (saved): the place's inertia time against the turn's
    std::atomic<float> g_drag{ 1.0f };        // weaponweightdrag 0.5-2 (saved)
    std::atomic<float> g_lim_deg{ 5.0f };     // weaponweightlimit DEG CM (saved): a rifle's limits
    std::atomic<float> g_lim_cm{ 1.2f };
    std::atomic<float> g_two{ 0.6f };         // weaponweighttwohand 0.2-1 (saved)
    std::atomic<float> g_sight{ 0.75f };      // weaponweightsighted 0.2-1 (saved)
    std::atomic<bool>  g_log{ false };        // weaponweightlog (not saved): one line per step
    std::atomic<bool>  g_handling{ true };    // handlinglog (not saved)
    std::atomic<float> g_test_heft{ -1.0f };  // weaponweightheft H (test, not saved): this heft for every gun, -1 = by its class

    // The game's weapon class (selWeaponClass, FarCry5WeaponTable.h) to a heft: 1 = a rifle. 0 = no weight (melee, bows, throwables,
    // tools: a swing or a throw must not trail the hand; the bow has its own steadier).
    float class_heft(const fc5::weapons::Info* w, const char*& kind)
    {
        kind = nullptr;
        if (!w) return 0.0f;
        float h = 0.0f;
        switch (w->cls) {
        case 1: kind = "handgun"; h = 0.30f; break;
        case 2: kind = "rifle"; h = 1.00f; break;
        case 3: kind = w->one_handed ? "machine pistol" : "SMG"; h = w->one_handed ? 0.35f : 0.60f; break;
        case 4: kind = w->one_handed ? "sawed-off shotgun" : "shotgun"; h = w->one_handed ? 0.55f : 1.05f; break;
        case 5: kind = "sniper rifle"; h = 1.30f; break;
        case 6: kind = "machine gun"; h = 1.90f; break;
        case 7: kind = w->one_handed ? "grenade launcher" : "launcher"; h = w->one_handed ? 0.80f : 1.80f; break;
        case 9: kind = "flamethrower"; h = 1.40f; break;
        case 17: kind = "harpoon"; h = 0.90f; break;
        default: return 0.0f;
        }
        static const char* const kHeavy[]{ "/D50", "/Cannon", "Revolver", "Elephantgun", "700Nitro", "/AMR", "SA-50", "/1887", "MBP", "MG42" };
        if (w->name)
            for (const char* s : kHeavy)
                if (std::strstr(w->name, s)) { h *= 1.25f; break; }
        return h;
    }

    Tuning tuning()
    {
        Tuning t{};
        t.inertia_ms = g_inertia.load();
        t.move_share = g_move.load();
        t.drag       = g_drag.load();
        t.lim_deg    = g_lim_deg.load();
        t.lim_cm     = g_lim_cm.load();
        t.two_hand   = g_two.load();
        t.sighted    = g_sight.load();
        return t;
    }

    // Render thread only.
    State             s_st;
    clock::time_point s_last{};
    uint32_t          s_sig{ 0 };
    struct Stat
    {
        clock::time_point t0{};
        uint64_t          n{ 0 }, limited{ 0 }, resets{ 0 }, two{ 0 }, sighted{ 0 };
        double            lag_deg{ 0.0 }, lag_cm{ 0.0 };
        float             peak_deg{ 0.0f }, peak_cm{ 0.0f };
    } s_ws;
    struct Hand
    {
        clock::time_point t0{};
        uint64_t          n{ 0 }, held{ 0 };
        double            hold_deg{ 0.0 }, hold_cm{ 0.0 }, lag_deg{ 0.0 }, lag_cm{ 0.0 };
        float             hold_deg_max{ 0.0f }, hold_cm_max{ 0.0f }, lag_deg_max{ 0.0f }, lag_cm_max{ 0.0f };
    } s_hd;
    // handlinglog's events: a weapon switch (the new gun's first 1.5 s) and a manual reload (from its first step to 0.5 s after its
    // last): the worst the gun was from its controller in it, one line each, so a test has one number per gun to compare.
    struct Event
    {
        bool              on{ false };
        const char*       what{ "" };
        uint32_t          from{ 0 }, sig{ 0 };
        clock::time_point t0{}, t_last{};
        uint64_t          n{ 0 };
        float             hold_deg{ 0.0f }, hold_cm{ 0.0f }, lag_deg{ 0.0f }, lag_cm{ 0.0f };
    } s_ev_switch, s_ev_reload;
    uint32_t s_hd_sig{ 0 };

    void event_end(Event& e, clock::time_point now)
    {
        if (e.on && e.n > 0)
            spdlog::info("[Handling] {} (weapon {}{}, {:.1f} s, {} controller samples): the gun was at most {:.2f} deg / {:.2f} cm from its controller by lag (smoothing and weight), "
                         "{:.1f} deg / {:.1f} cm by the hold | weaponweight {:.0f}",
                         e.what, e.sig, e.from ? fmt::format(", from {}", e.from) : std::string{}, std::chrono::duration<float>(now - e.t0).count(), e.n, e.lag_deg, e.lag_cm, e.hold_deg,
                         e.hold_cm, g_weight.load());
        e = Event{};
    }
    void event_add(Event& e, float hd, float hc, float ld, float lc)
    {
        e.n++;
        e.hold_deg = std::max(e.hold_deg, hd);
        e.hold_cm  = std::max(e.hold_cm, hc);
        e.lag_deg  = std::max(e.lag_deg, ld);
        e.lag_cm   = std::max(e.lag_cm, lc);
    }

    float angle_deg(const glm::mat4& a, const glm::mat4& b)
    {
        const glm::quat qa = glm::normalize(glm::quat_cast(glm::mat3{ a })), qb = glm::normalize(glm::quat_cast(glm::mat3{ b }));
        return glm::degrees(glm::length(rotvec(qa, qb)));
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "weaponweight") {
        // weaponweight 0-100 (also on = 50, off = 0)
        const float v = arg == "on" ? 50.0f : arg == "off" ? 0.0f : fv;
        g_weight      = std::clamp(v, 0.0f, 100.0f);
        persist       = true;
        spdlog::info("[Weight] weaponweight {:.0f}: {}", g_weight.load(),
                     g_weight.load() <= 0.0f ? "off: the gun is the smoothed hand, as before" : "fast swings trail the hand and settle; a steady or slow move is followed exactly");
        return true;
    }
    if (cmd == "weaponweightinertia") { g_inertia = std::clamp(fv, 5.0f, 120.0f); persist = true; return true; }
    if (cmd == "weaponweightmove") { g_move = std::clamp(fv, 0.0f, 1.0f); persist = true; return true; }
    if (cmd == "weaponweightdrag") { g_drag = std::clamp(fv, 0.5f, 2.0f); persist = true; return true; }
    if (cmd == "weaponweightlimit") {
        float cm = g_lim_cm.load();
        rest >> cm;
        g_lim_deg = std::clamp(fv, 0.5f, 15.0f);
        g_lim_cm  = std::clamp(cm, 0.1f, 5.0f);
        persist   = true;
        return true;
    }
    if (cmd == "weaponweighttwohand") { g_two = std::clamp(fv, 0.2f, 1.0f); persist = true; return true; }
    if (cmd == "weaponweightsighted") { g_sight = std::clamp(fv, 0.2f, 1.0f); persist = true; return true; }
    if (cmd == "weaponweightlog") { g_log = on; return true; }
    if (cmd == "weaponweightheft") { g_test_heft = arg.empty() || arg == "off" ? -1.0f : std::clamp(fv, 0.0f, 3.0f); return true; }
    if (cmd == "handlinglog") { g_handling = on; return true; }
    return false;
}

Settings settings()
{
    Settings s{};
    s.weight     = g_weight.load();
    s.inertia_ms = g_inertia.load();
    s.move_share = g_move.load();
    s.drag       = g_drag.load();
    s.lim_deg    = g_lim_deg.load();
    s.lim_cm     = g_lim_cm.load();
    s.two_hand   = g_two.load();
    s.sighted    = g_sight.load();
    return s;
}

const char* weapon_kind(uint32_t weapon_sig, float& heft)
{
    const char* kind = nullptr;
    heft             = class_heft(fc5::gameweapon::info(weapon_sig), kind);
    if (const float t = g_test_heft.load(); t >= 0.0f && kind) heft = t;
    return heft > 0.0f ? kind : nullptr;
}

void apply(glm::mat4& k, bool same, bool fresh, uint32_t weapon_sig, bool two_hand, bool sighted, float world_scale)
{
    const auto  now = clock::now();
    const float dt  = std::chrono::duration<float>(now - s_last).count();
    s_last          = now;
    const float w   = g_weight.load() / 100.0f;
    float       heft = 0.0f;
    const char* kind = w > 0.0f ? weapon_kind(weapon_sig, heft) : nullptr;
    if (!kind || fresh || weapon_sig != s_sig) {
        // Off, a weapon with no weight, the smoothing started over (sling, bow, kept) or another weapon: the gun is the hand.
        if (s_st.init && kind) s_ws.resets++;
        s_st.init = false;
        s_sig     = weapon_sig;
        if (!kind) return;
    }
    const Params pr = params(w, heft, two_hand, sighted, world_scale, tuning());
    glm::vec3    p{ k[3] };
    glm::quat    q = glm::quat_cast(glm::mat3{ k });
    const Step   s = step(s_st, p, q, dt, same, pr);
    if (s_st.init && (pr.tau_turn >= 0.002f || pr.tau_move >= 0.002f)) {
        k    = glm::mat4_cast(q);
        k[3] = glm::vec4{ p, 1.0f };
    }
    if (same) return;
    const float ws     = std::max(1e-3f, world_scale);
    const float lag_cm = 100.0f * s.lag_m / ws, lag_dg = glm::degrees(s.lag_rad);
    if (g_log.load())
        spdlog::info("[Weight] step {:.1f} ms ({} sub-steps): the gun {:.2f} deg / {:.2f} cm from the hand{}{}", 1000.0f * dt, s.substeps, lag_dg, lag_cm, s.limited ? ", at its limit" : "",
                     s.reset ? ", started over" : "");
    auto& st = s_ws;
    if (st.t0 == clock::time_point{}) st.t0 = now;
    st.n++;
    st.lag_deg += lag_dg;
    st.lag_cm += lag_cm;
    st.peak_deg = std::max(st.peak_deg, lag_dg);
    st.peak_cm  = std::max(st.peak_cm, lag_cm);
    if (s.limited) st.limited++;
    if (s.reset) st.resets++;
    if (two_hand) st.two++;
    if (sighted) st.sighted++;
    if (const float span = std::chrono::duration<float>(now - st.t0).count(); span >= 10.0f) {
        if (st.n >= 20) {
            const double n = double(st.n);
            spdlog::info("[Weight] last {:.0f} s, weapon {} ({}, heft {:.2f}; two hands {:.0f} %, sighted {:.0f} % of the time): weight {:.0f} %, inertia {:.0f} / {:.0f} ms (turn / move, now), "
                         "drag {:.2f}, limit {:.1f} deg / {:.1f} cm | the gun against the smoothed hand: {:.3f} deg / {:.3f} cm mean, {:.2f} deg / {:.2f} cm peak, at its limit in {} of {} "
                         "samples, started over {} times",
                         span, weapon_sig, kind, heft, 100.0 * double(st.two) / n, 100.0 * double(st.sighted) / n, 100.0f * w, 1000.0f * pr.tau_turn, 1000.0f * pr.tau_move, pr.drag,
                         glm::degrees(pr.lim_rad), 100.0f * pr.lim_m / ws, st.lag_deg / n, st.lag_cm / n, st.peak_deg, st.peak_cm, st.limited, st.n, st.resets);
        }
        st = Stat{};
    }
}

void handling_note(const glm::mat4& ctrl, const glm::mat4& held, const glm::mat4& out, bool hold_active, bool reload_now, uint32_t weapon_sig, float world_scale)
{
    if (!g_handling.load()) return;
    const auto now = clock::now();
    auto&      st  = s_hd;
    if (st.t0 == clock::time_point{}) st.t0 = now;
    const float ws = std::max(1e-3f, world_scale);
    // By the hold: the gun the mod aims (two hands, the one-hand line, the stock) against the weapon controller itself.
    const float hd = angle_deg(ctrl, held), hc = 100.0f * glm::length(glm::vec3{ held[3] } - glm::vec3{ ctrl[3] }) / ws;
    // By the smoothing and the weight: the gun as drawn against the gun the hold asked for (this is lag, never intended).
    const float ld = angle_deg(held, out), lc = 100.0f * glm::length(glm::vec3{ out[3] } - glm::vec3{ held[3] }) / ws;
    st.n++;
    if (hold_active) st.held++;
    st.hold_deg += hd;
    st.hold_cm += hc;
    st.lag_deg += ld;
    st.lag_cm += lc;
    st.hold_deg_max = std::max(st.hold_deg_max, hd);
    st.hold_cm_max  = std::max(st.hold_cm_max, hc);
    st.lag_deg_max  = std::max(st.lag_deg_max, ld);
    st.lag_cm_max   = std::max(st.lag_cm_max, lc);
    // The events.
    if (weapon_sig != s_hd_sig) {
        event_end(s_ev_switch, now);
        event_end(s_ev_reload, now);
        if (s_hd_sig != 0 && weapon_sig != 0) {
            s_ev_switch.on   = true;
            s_ev_switch.what = "weapon switch, the new gun's first 1.5 s";
            s_ev_switch.from = s_hd_sig;
            s_ev_switch.sig  = weapon_sig;
            s_ev_switch.t0   = now;
        }
        s_hd_sig = weapon_sig;
    }
    if (s_ev_switch.on) {
        event_add(s_ev_switch, hd, hc, ld, lc);
        if (now - s_ev_switch.t0 >= std::chrono::milliseconds(1500)) event_end(s_ev_switch, now);
    }
    if (reload_now && !s_ev_reload.on) {
        s_ev_reload.on   = true;
        s_ev_reload.what = "manual reload";
        s_ev_reload.sig  = weapon_sig;
        s_ev_reload.t0   = now;
    }
    if (s_ev_reload.on) {
        if (reload_now) s_ev_reload.t_last = now;
        event_add(s_ev_reload, hd, hc, ld, lc);
        if (now - s_ev_reload.t_last >= std::chrono::milliseconds(500)) event_end(s_ev_reload, now);
    }
    const float span = std::chrono::duration<float>(now - st.t0).count();
    if (span < 10.0f) return;
    if (st.n >= 20) {
        const double n = double(st.n);
        spdlog::info("[Handling] last {:.0f} s, weapon {} in hand, {} controller samples (a hold in {:.0f} %): the gun against its own controller | by the hold (two hands, one-hand line, stock): "
                     "{:.2f} deg / {:.2f} cm mean, {:.1f} deg / {:.1f} cm max | by the smoothing and the weight (lag): {:.3f} deg / {:.3f} cm mean, {:.2f} deg / {:.2f} cm max | weaponweight {:.0f}",
                     span, weapon_sig, st.n, 100.0 * double(st.held) / n, st.hold_deg / n, st.hold_cm / n, st.hold_deg_max, st.hold_cm_max, st.lag_deg / n, st.lag_cm / n, st.lag_deg_max,
                     st.lag_cm_max, g_weight.load());
    }
    st = Hand{};
}
}
