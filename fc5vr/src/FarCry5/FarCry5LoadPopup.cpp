// loadpopup (author 30 Oct: when a save loads, a small popup in the headset while the controllers are blocked, counting down
// until the mod's load-time work is done; a launcher option skips it, shown by default).
//
// Back in play somewhere else (a save loaded, a respawn, fast travel, or the first play) the popup hangs where the F11 menu would (the framework's
// UI layer, no pointer) and the player's controller input is not sent to the game (ModConfig.cpp: buttons, triggers, sticks and grips; the mod's
// own presses for the look at the sidearm slot and the camera's steering still go). Once shown it closes once, at least 2 s after play came back
// and 1.5 s after it showed:
//  - the weapon in hand has been recorded (held still 1 s; nothing to do in bare hands or off foot),
//  - the sidearm has been recorded or the look at the sidearm slot is done (sidearmpeek; nothing to do with it or the sidearm off),
//  - no weapon-switch scan is running or due (switchfast: the scan a load asks for reads GB of memory and slows the game while it runs),
//  - loadslot has nothing left to do (the main weapon taken out after the look when the game holds neither it nor the sidearm, so it hangs on the sling),
// or 10 s after it showed whatever is left (logged; author 2 Nov 21:47, was 30 s). blocking() and draw() check that clock
// themselves, so the controllers come back and the popup goes at 10 s even on frames where frame() does not run.
//
// When it shows (author 7 Nov 11:04, and it should
// not come back after the menu). All of it is decided here from the flat screen, the eye images and the camera (frame()):
//  - Somewhere else: back in 3D from the flat screen more than 20 m from where play last stood settled (or no play yet: the first play). That
//    place is taken only from settled play, never from a camera that was in 3D for a moment: 7 Nov 11:56 the world map's own 3D terrain view
//    passed for play for 0.4 s, 9 km from the car, and closing the map then read as 'somewhere else' (the popup twice, and after a menu).
//    Where play stands is taken once play has settled as below (at least 1.5 s in 3D, also with loadpopupsettle 0), not in a cinematic.
//    Moving play (a car, a helicopter) also counts as near when back within its last speed times the time since (4 s at most): the game runs
//    on about a second each side of the map or pause menu, and 7 Nov 14:36 a car at 25 m/s came back 34 m on after the map (drivemenu's
//    short press), so the popup came after the map again. Fast travel and respawns land hundreds of metres to km away.
//  - Settled: loadpopupsettle s (saved, default 2) in 3D without the flat screen or a cinematic, and no black eye image for the last second.
//    7 Nov 11:34: the world 1.5 s, then black, a menu screen, then play (the popup came twice); 11:51: 10 s of black 3D after a helicopter spawn
//    (the popup came and went on the black). After 12 s in 3D it shows whatever the image (a scene dark enough to read as black).
//  - Until it settles the popup waits unseen; the controllers are already held back (the mod's load-time work starts with play), not in a
//    cinematic and not on the flat screen. A return near where play stood (the map closed, a menu) drops a waiting popup.
//  - Shown once: the flat screen (the pause menu, the map, another loading screen) closes it for good; back in play near there it is not shown
//    again, only a return somewhere else shows it again.
// loadpopupsettle 0: shown at the return to 3D, as before 7 Nov (the place still from settled play, and still shown once).
// The countdown is an estimate: 2.5 s for the weapon, 7 s for the sidearm, 9 s for loadslot (2 s once its pick is under way), the latest scan's time
// (10 s before the first) for the scan, all counted from the return to play.
#include "FarCry5LoadPopup.h"
#include "FarCry5Gesture.h"
#include "FarCry5SwitchSpeed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <istream>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::loadpopup
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr float kMinS = 2.0f, kMinShownS = 1.5f, kMaxS = 10.0f;   // kMaxS: the hard limit (s shown)
    constexpr float kClearS    = 1.0f;    // settled: no black eye image for this long
    constexpr float kWaitMaxS  = 12.0f;   // settled anyway after this long in 3D (a dark scene; 7 Nov 11:51 had 10 s of black)
    constexpr float kPlaceS    = 1.5f;    // where play stands: only after this long (or loadpopupsettle if longer) in 3D with no black eye image
    constexpr float kElsewhere = 20.0f;   // m from where play stood: somewhere else (loadrecenter's test)
    constexpr float kCarryS    = 4.0f;    // s of travel at play's last speed also counted as 'near' (the vehicle drives on while the flat screen fades in and out)
    constexpr float kSpeedMax  = 80.0f;   // m/s: a faster step between two frames of settled play is a jump, not travel
    enum : int { kWeapon = 1, kSide = 2, kScan = 4, kSlot = 8, kAll = 15 };

    std::atomic<bool>  g_on{ true };
    std::atomic<float> g_settle_s{ 2.0f };
    std::atomic<bool>  g_stop_req{ false };
    std::atomic<bool>  g_shown{ false };
    std::atomic<bool>  g_waiting{ false };   // armed, in 3D, not shown yet: the controllers held back, nothing drawn
    std::atomic<int>   g_done{ 0 };          // kWeapon | kSide | kScan | kSlot
    std::atomic<float> g_eta{ 0.0f };        // seconds left (estimate)
    std::atomic<float> g_elapsed{ 0.0f };    // s since play came back (the progress bar)
    std::atomic<long long> g_deadline{ 0 };  // clock time (ticks) past which blocking() and draw() stop whatever frame() does

    bool      past_deadline() { return clock::now().time_since_epoch().count() >= g_deadline.load(); }
    long long ticks_in(clock::time_point now, float s) { return (now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(s))).time_since_epoch().count(); }
    float     secs(clock::duration d) { return std::chrono::duration<float>(d).count(); }

    // Render thread only.
    bool              r_flat = false;        // the flat screen was on last frame
    bool              r_settled = false;     // play settled since the last return to 3D (the popup's moment)
    int               r_armed = 0;           // 0 nothing to show, 1 the first play, 2 somewhere else
    bool              r_have_pos = false;    // r_pos is known (some play has settled)
    float             r_pos[3]{};            // where play last stood settled (the world camera)
    clock::time_point r_pos_t{};             // when r_pos was taken
    bool              r_speed_ok = false;    // r_speed is from consecutive frames of settled play
    float             r_speed = 0.0f;        // how fast the world camera travelled in settled play (m/s, smoothed over about 0.5 s)
    bool              r_started = false;     // the flat screen has been seen once (nothing before: the game's start)
    clock::time_point r_3d_t0{}, r_black_t{}, r_play_t0{}, r_show_t{}, r_armed_t{}, r_hands_t{};
    bool              r_black_seen = false;
    bool              r_scan_seen = false;   // a scan ran since play came back
    float             r_done_at[4]{};        // when each step was done (s since play came back; logged)
    int               r_done_was = 0;

    const char* why_left(int done)
    {
        return (done & kScan) == 0 ? "the weapon switch scan" : (done & kSlot) == 0 ? "the main weapon on the sling" : (done & kSide) == 0 ? "the sidearm" : "the weapon in hand";
    }
    float dist(const float a[3], const float b[3])
    {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    (void)rest;
    if (cmd == "loadpopupsettle") {
        g_settle_s = std::clamp(std::isfinite(fv) ? fv : 2.0f, 0.0f, 10.0f);
        persist    = true;
        spdlog::info("[LoadPopup] loadpopupsettle {:.1f}: {}", g_settle_s.load(),
                     g_settle_s.load() > 0.0f ? "shown once play has been in 3D this long with no black eye image for 1 s (12 s at most)" : "shown at the return to 3D");
        return true;
    }
    if (cmd != "loadpopup") return false;
    g_on    = !(arg == "off" || arg == "0" || arg == "false");
    persist = true;
    if (!g_on.load()) g_stop_req = true;
    spdlog::info("[LoadPopup] loadpopup {}: {}", g_on.load() ? "on" : "off",
                 g_on.load() ? "\"VR mod loading\" after a load, the controllers held back until the mod's load-time work is done" : "no popup after a load");
    return true;
}

void frame(const Steps& s)
{
    const auto now = clock::now();
    if (g_stop_req.exchange(false)) {
        g_shown   = false;
        g_waiting = false;
        r_armed   = 0;
    }
    if (s.black) {
        r_black_t    = now;
        r_black_seen = true;
    }

    // The flat screen (a menu, the map, a loading screen or a flat cinematic): a shown popup closes for good, a waiting one waits for play.
    if (s.flat) r_started = true;
    if (!r_started) return;
    if (s.flat) {
        if (!r_flat) {
            r_flat = true;
            if (g_shown.exchange(false))
                spdlog::info("[LoadPopup] closed by the flat screen after {:.1f} s shown ({}{}): not shown again unless play comes back somewhere else",
                             secs(now - r_show_t), g_done.load() == kAll ? "all done" : why_left(g_done.load()), g_done.load() == kAll ? "" : " not done");
            else if (r_armed)
                spdlog::info("[LoadPopup] the flat screen again before play settled ({:.1f} s in 3D): still waiting for play", secs(now - r_3d_t0));
        }
        g_waiting = false;
        r_settled = false;
        return;
    }

    // Back in 3D: somewhere else (or the first play) arms it; near where play stood drops a waiting one.
    if (r_flat) {
        r_flat    = false;
        r_3d_t0   = now;
        r_settled = false;
        // Near = within kElsewhere, plus how far play's last speed carries in the time since it stood there (kCarryS at most): the game runs on
        // for about a second each side of the map or the pause menu (the fade before the flat screen, the 45 frames of player camera before the
        // mod's return to 3D), so a car at 25 m/s comes back 30-40 m on (7 Nov 14:36: 34 m, the popup after closing the map in a car).
        const float moved = r_have_pos ? dist(s.cam, r_pos) : 0.0f;
        const float carry = r_have_pos ? r_speed * std::min(secs(now - r_pos_t), kCarryS) : 0.0f;
        const float near_m = kElsewhere + carry;
        if (g_on.load() && (!r_have_pos || moved > near_m)) {
            if (!r_armed) {
                r_armed_t   = now;
                r_hands_t   = now;
                r_scan_seen = false;
                r_done_was  = 0;
            }
            r_armed  = r_have_pos ? 2 : 1;
            g_done   = 0;
            g_deadline = ticks_in(now, kWaitMaxS + 1.0f);
            if (r_have_pos)
                spdlog::info("[LoadPopup] back in play somewhere else ({:.0f} m from where play last stood, over {:.0f} m: {:.0f} m + {:.1f} m/s for {:.1f} s): controller input held back, \"VR mod loading\" shown once play settles",
                             moved, near_m, kElsewhere, r_speed, std::min(secs(now - r_pos_t), kCarryS));
            else
                spdlog::info("[LoadPopup] first play: controller input held back, \"VR mod loading\" shown once play settles");
        } else if (r_armed) {
            spdlog::info("[LoadPopup] back in play {:.0f} m from where play last stood (the map or a menu closed; near: {:.0f} m): the waiting popup dropped, controller input sent again", moved, near_m);
            r_armed = 0;
        } else if (r_have_pos && moved > kElsewhere)
            spdlog::info("[LoadPopup] back in play {:.0f} m from where play last stood, within the {:.0f} m that {:.1f} m/s carries (the map or a menu closed while moving): no popup",
                         moved, near_m, r_speed);
    }

    // Settled: in 3D long enough with no black eye image, not in a cinematic (12 s at most).
    if (s.cine) r_3d_t0 = now;
    const float in3d   = secs(now - r_3d_t0);
    const float settle = g_settle_s.load();
    const bool  clear  = !r_black_seen || secs(now - r_black_t) >= kClearS;
    if (!r_settled && !s.cine && (settle <= 0.0f || (in3d >= settle && clear) || in3d >= kWaitMaxS)) {
        r_settled = true;
        if (r_armed) {
            r_show_t   = now;
            r_play_t0  = r_3d_t0;
            g_deadline = ticks_in(now, kMaxS);
            g_elapsed  = 0.0f;
            g_shown    = true;
            spdlog::info("[LoadPopup] {}: \"VR mod loading\" shown after {:.1f} s in 3D ({}; {:.1f} s since the load's first return to play), controller input held back ({:.0f} s at most)",
                         r_armed == 1 ? "first play" : "back in play somewhere else", in3d, in3d >= kWaitMaxS && !clear ? "the eye images still black, shown anyway" : "play settled",
                         secs(now - r_armed_t), kMaxS);
            r_armed = 0;
        }
    }
    if (!s.cine && ((in3d >= std::max(kPlaceS, settle) && clear) || in3d >= kWaitMaxS)) {   // where play stands (not the map's 3D view for a moment)
        // Play's speed, from consecutive frames of settled play only (none across the flat screen; a step over kSpeedMax is a jump and left out).
        const float dt = r_have_pos ? secs(now - r_pos_t) : 0.0f;
        if (r_have_pos && dt > 0.0f && dt <= 0.25f) {
            const float v = dist(s.cam, r_pos) / dt;
            if (v <= kSpeedMax) {
                r_speed    = r_speed_ok ? r_speed + (v - r_speed) * (dt / (0.5f + dt)) : v;
                r_speed_ok = true;
            }
        } else if (dt > 0.25f || !r_have_pos) {
            r_speed    = 0.0f;
            r_speed_ok = false;
        }
        r_pos[0]   = s.cam[0];
        r_pos[1]   = s.cam[1];
        r_pos[2]   = s.cam[2];
        r_pos_t    = now;
        r_have_pos = true;
    }
    g_waiting = r_armed != 0 && !s.cine && g_on.load();
    if (!g_shown.load() && !r_armed) return;

    // The mod's load-time work, from the return to play (it starts with play, shown or not).
    const float t  = secs(now - (g_shown.load() ? r_play_t0 : r_3d_t0));
    const float ts = g_shown.load() ? secs(now - r_show_t) : 0.0f;
    if (s.armed) r_hands_t = now;
    const bool bare = !s.armed && now - r_hands_t > std::chrono::milliseconds(1500);   // nothing to record in bare hands
    const int  peek = fc5::gesture::side_peek_state();
    float      scan_s = 0.0f, took = 0.0f;
    const int  scan = fc5::switchspeed::scan_state(scan_s, took);
    if (scan == 2) r_scan_seen = true;
    int done = 0;
    if (!s.allowed || bare || s.weapon_recorded) done |= kWeapon;
    if (!s.allowed || bare || peek < 0 || peek == 1 || s.side_recorded) done |= kSide;
    if (scan == 0 || (scan == 3 && r_scan_seen)) done |= kScan;   // none found yet: one scan since the load is enough
    const int slot = fc5::gesture::load_slot_state();
    if (!s.allowed || slot == 0) done |= kSlot;
    for (int i = 0; i < 4; i++)
        if ((done & (1 << i)) && !(r_done_was & (1 << i))) r_done_at[i] = t;
    r_done_was = done;
    g_done     = done;
    if (!g_shown.load()) return;
    g_elapsed = t;

    float eta = std::max(kMinS - t, kMinShownS - ts);
    if (!(done & kWeapon)) eta = std::max(eta, 2.5f - t);
    if (!(done & kSide)) eta = std::max(eta, 7.0f - t);
    if (!(done & kSlot)) eta = std::max(eta, slot == 2 ? 2.0f : 9.0f - t);
    if (!(done & kScan)) {
        const float each = took > 0.5f ? took : 10.0f;
        eta = std::max(eta, scan == 2 ? each - scan_s : scan_s + each);
    }
    if (done != kAll) eta = std::max(eta, 1.0f);
    g_eta = std::clamp(eta, 0.0f, std::max(0.0f, kMaxS - ts));

    if ((done == kAll && t >= kMinS && ts >= kMinShownS) || ts >= kMaxS) {
        g_shown = false;
        if (done == kAll)
            spdlog::info("[LoadPopup] done after {:.1f} s shown, {:.1f} s after play came back (weapon {:.1f} s, sidearm {:.1f} s, weapon switch scan {:.1f} s, main weapon on the sling {:.1f} s): controller input sent again",
                         ts, t, r_done_at[0], r_done_at[1], r_done_at[2], r_done_at[3]);
        else
            spdlog::info("[LoadPopup] closed after {:.0f} s shown with {} not done (weapon {}, sidearm {}: look {}, weapon switch scan state {}, loadslot {}): controller input sent again",
                         ts, why_left(done), (done & kWeapon) ? "done" : "not recorded", (done & kSide) ? "done" : "not recorded", peek, scan, slot);
    }
}

// Both check the hard limit's clock themselves: past it the popup is gone and the controllers are back even if frame() has not run since.
bool blocking() { return (g_shown.load() || g_waiting.load()) && !past_deadline(); }

bool draw()
{
    if (!g_shown.load() || past_deadline()) return false;
    static const ImVec4 kAccent{ 0.93f, 0.46f, 0.13f, 1.00f };
    static const ImVec4 kText{ 0.95f, 0.92f, 0.86f, 1.00f };
    static const ImVec4 kHint{ 0.66f, 0.62f, 0.55f, 1.00f };
    static const ImVec4 kDone{ 0.45f, 0.80f, 0.40f, 1.00f };
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{ disp.x * 0.5f, disp.y * 0.5f }, ImGuiCond_Always, ImVec2{ 0.5f, 0.5f });
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.075f, 0.068f, 0.060f, 0.94f });
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{ 0.55f, 0.27f, 0.09f, 1.00f });
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{ 0.16f, 0.14f, 0.12f, 1.00f });
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 28.0f, 22.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::Begin("##fc5vr_loadpopup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
    const int   done = g_done.load();
    const float eta = g_eta.load(), t = g_elapsed.load();
    ImGui::TextColored(kAccent, "VR mod loading");
    ImGui::Text("Controls back in %d s", std::max(1, int(std::ceil(eta))));
    ImGui::ProgressBar(std::clamp(t / std::max(0.1f, t + eta), 0.0f, 1.0f), ImVec2{ 440.0f, 10.0f }, "");
    const char* labels[4]{ "Weapon in hand", "Sidearm on the hip", "Faster weapon switch", "Main weapon on the sling" };
    for (int i = 0; i < 4; i++) {
        const bool d = (done & (1 << i)) != 0;
        ImGui::TextColored(d ? kDone : kHint, d ? "done" : "....");
        ImGui::SameLine(96.0f);
        ImGui::TextColored(d ? kText : kHint, "%s", labels[i]);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return true;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("\"VR mod loading\" popup after a load##loadpopup", &v)) cmds += v ? "loadpopup on\n" : "loadpopup off\n";
    hint("On (default): after a save loads (also a respawn or fast travel), a small popup counts down while the mod records your weapon and "
         "sidearm and looks for the weapons in memory (faster weapon switch); the controllers do nothing until it closes (10 s at most, even if something is not done yet). "
         "It shows once per load, not after the pause menu or the map. "
         "Off: play starts at once, with a few seconds of hitches and no sidearm copy until then. The launcher's \"Skip the loading popup\" sets it too.");
    float st = g_settle_s.load();
    ImGui::SliderFloat("Wait for the game to show (s)##loadpopupsettle", &st, 0.0f, 5.0f, st <= 0.0f ? "at once" : "%.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char line[48];
        std::snprintf(line, sizeof(line), "loadpopupsettle %.1f\n", st);
        cmds += line;
    }
    hint("How long play has to be on screen (in 3D, no black picture for 1 s) before the popup shows, so it does not come up on a loading "
         "picture that is about to go again. The controllers are already held back while it waits. 2 s by default; 0 = at the return to 3D, as before.");
}
}
