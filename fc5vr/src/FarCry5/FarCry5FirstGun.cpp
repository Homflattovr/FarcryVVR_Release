// firstguntip (author 16 Nov 14:45Z).
//
// When: the first time a gun is looted. FarCry5Lua's loot watch asks the player's inventory every 250 ms for every gun of the weapon table
// (selWeaponClass 1 pistol to 7 launcher: not melee weapons, the bow or throwables); a gun carried that was not at the read before is a new
// gun. The watch runs with the headset on and motion controls, outside the flat screen (a load drops its baseline, so the guns a save holds
// never count) and not in test mode. The popup then shows once play allows it: on foot, no menu, flat screen, cinematic or loading popup, the
// game not paused by one of its own pop-ups (IsGamePaused's byte 0; 10 s after the loot at most), 0.5 s after the loot at the earliest. In a new game that is the opening's
// first gun. It shows once: "firstguntip off" is saved when it closes (the watch then stops); ticking it again in F11 (or "firstguntip on")
// shows it at the next gun looted.
// Where: on the framework's UI layer, where the F11 menu would be (as "VR mod loading"), no pointer.
// While it shows: the game is paused with its own PauseGame (FarCry5Lua.cpp; firstgunpause off: not paused), none of the player's controller
// input reaches the game (ModConfig.cpp) and the aim sends no mouse moves (the paused camera answers none, which would count toward the stick
// fallback). It closes on a press of right A or the right trigger that starts 2 s or more after it showed (a trigger already held when it
// came is not taken), or when F11 opens. The resume is asked as it closes. The flat screen does not close it (logged): if the pause itself
// brought the flat screen, the popup still shows there and A still closes it.
#include "FarCry5FirstGun.h"
#include "FarCry5Lua.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <istream>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::firstgun
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr float kAfterS = 0.5f;  // after the loot at the earliest (the pick-up's animation)
    constexpr float kGamePauseMaxS = 10.0f;   // waited at most this long after the loot for IsGamePaused's byte to read 0
    constexpr float kMinS  = 2.0f;   // shown at least this long before a press closes it (read it; a trigger held as it came)

    std::atomic<bool>      g_on{ true };          // still to show (saved off once shown)
    std::atomic<bool>      g_pause_on{ true };    // firstgunpause
    std::atomic<bool>      g_show_req{ false };   // firstguntip show
    std::atomic<bool>      g_close_req{ false };  // a press, or firstguntip close
    std::atomic<bool>      g_showing{ false };
    std::atomic<bool>      g_shown_new{ false };  // shown for the first time and closed: the caller saves it
    std::atomic<long long> g_show_ticks{ 0 };     // when it showed (clock ticks), for the pad's thread
    std::atomic<bool>      g_up_seen{ false };    // the pad's thread: A and the trigger seen up since it showed
    std::atomic<bool>      g_paused{ false };     // the pause was asked for this popup (for the text)
    std::atomic<bool>      g_swallow{ false };    // the pad's thread: the press that closed it, held back until let go

    // Render thread only.
    clock::time_point r_loot_t{};
    bool              r_loot{ false };             // a gun looted, the popup waiting for play
    uint64_t          r_loot_item{ 0 };
    bool              r_have_n{ false };
    uint32_t          r_seen_n{ 0 };               // the loot watch's count of new guns, as last seen
    bool              r_first{ false };            // this showing is the first-gun one (not "show")
    bool              r_menu_was{ false };
    bool              r_flat_was{ false };

    float secs(clock::duration d) { return std::chrono::duration<float>(d).count(); }
    float shown_s() { return secs(clock::now().time_since_epoch() - clock::duration(g_show_ticks.load())); }

    void open(bool first, uint64_t item)
    {
        r_first     = first;
        g_close_req = false;
        g_up_seen   = false;
        g_swallow   = false;
        g_show_ticks = clock::now().time_since_epoch().count();
        g_showing   = true;
        const bool pause = g_pause_on.load() && fc5::lua::pause_ready();
        g_paused = pause;
        if (pause) fc5::lua::pause_request(true);
        spdlog::info("[FirstGun] the weapon tips popup shows ({}; item {:x}): {}; right A or the right trigger closes it after {:.0f} s", first ? "the first gun looted" : "firstguntip show",
                     item, pause ? "the game's PauseGame asked, controller input held" : g_pause_on.load() ? "PauseGame not usable: the game is not paused, controller input held" : "firstgunpause off: the game is not paused, controller input held",
                     kMinS);
    }

    void close(const char* why)
    {
        g_showing = false;
        if (g_paused.exchange(false) || fc5::lua::paused_by_mod()) fc5::lua::pause_request(false);
        if (r_first) {
            g_on        = false;
            g_shown_new = true;
        }
        spdlog::info("[FirstGun] the weapon tips popup closed after {:.1f} s ({}){}", shown_s(), why, r_first ? ": firstguntip off saved (shown once)" : "");
        r_first = false;
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    (void)fv;
    (void)rest;
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "firstgunpause") {
        g_pause_on = on;
        persist    = true;
        spdlog::info("[FirstGun] firstgunpause {}: {}", on ? "on" : "off",
                     on ? "the game paused (its own PauseGame) while the weapon tips popup shows" : "the game runs on while the weapon tips popup shows (controller input still held)");
        return true;
    }
    if (cmd != "firstguntip") return false;
    if (arg == "show") {
        g_show_req = true;
        spdlog::info("[FirstGun] firstguntip show: the weapon tips popup shows now (not saved)");
        return true;
    }
    if (arg == "close") {
        g_close_req = true;
        spdlog::info("[FirstGun] firstguntip close (not saved)");
        return true;
    }
    g_on    = on;
    persist = true;
    spdlog::info("[FirstGun] firstguntip {}: {}", on ? "on" : "off",
                 on ? "the weapon tips popup shows the next time a gun is looted (once)" : "the weapon tips popup was shown (or is not wanted)");
    return true;
}

void frame(const Frame& f)
{
    fc5::lua::pause_frame();   // a resume the script thread has not made within 1 s is made here
    const auto now = clock::now();
    if (!g_showing.load()) {
        r_menu_was = f.menu_open;
        r_flat_was = f.flat;
        if (g_show_req.exchange(false)) {
            open(false, 0);
            return;
        }
        // The loot watch: on while still to show and watching; off drops its baseline and any loot not shown yet.
        const bool watch = g_on.load() && f.watch;
        fc5::lua::gun_watch(watch);
        uint64_t       item = 0;
        const uint32_t n    = fc5::lua::gun_new_count(&item);
        if (!r_have_n || !watch) {
            r_have_n = true;
            r_seen_n = n;
            if (r_loot && !watch) spdlog::info("[FirstGun] the gun looted ({:x}) not shown before the watch stopped (a load, a menu or the setting): dropped", r_loot_item);
            r_loot = false;
            return;
        }
        if (n != r_seen_n) {
            r_seen_n = n;
            if (!r_loot) spdlog::info("[FirstGun] a gun looted ({:x}): the weapon tips popup shows once play allows it", item);
            r_loot      = true;
            r_loot_t    = now;
            r_loot_item = item;
        }
        if (!r_loot || !f.play || f.menu_open || secs(now - r_loot_t) < kAfterS) return;
        if (fc5::lua::pause_byte() > 0 && secs(now - r_loot_t) < kGamePauseMaxS) return;   // one of the game's own pop-ups has paused it: after that one (10 s at most)
        r_loot = false;
        open(true, r_loot_item);
        return;
    }
    g_show_req = false;
    if (g_close_req.exchange(false)) close("a press of right A or the right trigger, or firstguntip close");
    else if (f.menu_open && !r_menu_was) close("F11 opened");
    if (f.flat != r_flat_was && g_showing.load())   // not closed by it: the game's own paused pop-ups keep the 3D view, but if this pause brings the flat screen the popup still shows there
        spdlog::info("[FirstGun] the flat screen {} while the weapon tips popup shows ({:.1f} s; IsGamePaused's byte {})", f.flat ? "came" : "went", shown_s(), fc5::lua::pause_byte());
    r_flat_was = f.flat;
    r_menu_was = f.menu_open;
}

bool take_shown() { return g_shown_new.exchange(false); }

bool showing() { return g_showing.load(); }

bool hold_input(bool down)
{
    if (g_showing.load()) {
        if (!down) g_up_seen = true;
        else if (shown_s() < kMinS) g_up_seen = false;   // a press that starts before 2 s must be let go first
        else if (g_up_seen.load() && !g_close_req.load()) {
            g_close_req = true;
            g_swallow   = true;
        }
        return true;
    }
    // The closing press: held back until let go (right A is the game's jump, the trigger fires).
    if (g_swallow.load() && !down) g_swallow = false;
    return g_swallow.load();
}

namespace
{
    constexpr ImU32 kBodyC = IM_COL32(205, 196, 178, 255), kDimC = IM_COL32(120, 110, 95, 255), kSlotC = IM_COL32(237, 117, 33, 255),
                    kGunC = IM_COL32(237, 117, 33, 255), kGunDarkC = IM_COL32(176, 84, 22, 255), kInkC = IM_COL32(19, 17, 15, 255);

    // A numbered disc (the rows and the figures share the numbers).
    void badge(ImDrawList* dl, ImVec2 c, float r, const char* t)
    {
        dl->AddCircleFilled(c, r, kSlotC, 24);
        const float  fs = r * 1.35f;
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float  k  = fs / std::max(1.0f, ImGui::GetFontSize());
        dl->AddText(nullptr, fs, ImVec2{ c.x - 0.5f * ts.x * k, c.y - 0.5f * ts.y * k }, kInkC, t);
    }

    void label(ImDrawList* dl, ImVec2 c, float fs, ImU32 col, const char* t)   // centred on c
    {
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float  k  = fs / std::max(1.0f, ImGui::GetFontSize());
        dl->AddText(nullptr, fs, ImVec2{ c.x - 0.5f * ts.x * k, c.y - 0.5f * ts.y * k }, col, t);
    }

    // A standing figure about 10 units tall in a cell w wide from p0, s px a unit. back: seen from behind (the player's right on the viewer's
    // right); front: facing the viewer (the player's right on the viewer's left). The slots: 1 the holster at the right hip, 2 the sling across
    // the front of the hips (front only), 3 both shoulders (the rifles on the back seen from behind). The preview (research README) draws the
    // same coordinates.
    // Filled convex shape, its points turned clockwise on screen if they are not (ImGui's anti-aliased fill wants that; mirrored shapes flip it).
    void poly(ImDrawList* dl, std::initializer_list<ImVec2> pts, ImU32 col)
    {
        ImVec2 v[8];
        int    n = 0;
        for (const ImVec2& p : pts)
            if (n < 8) v[n++] = p;
        float area = 0.0f;
        for (int i = 0; i < n; ++i) area += v[i].x * v[(i + 1) % n].y - v[(i + 1) % n].x * v[i].y;
        if (area < 0.0f) std::reverse(v, v + n);
        dl->AddConvexPolyFilled(v, n, col);
    }

    // A rounded bar: a thick line with round ends.
    void capsule(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col)
    {
        dl->AddLine(a, b, col, 2.0f * r);
        dl->AddCircleFilled(a, r, col, 20);
        dl->AddCircleFilled(b, r, col, 20);
    }

    // A standing figure about 10 units tall in a cell w wide from p0, s px a unit, drawn as a filled silhouette with the weapons in orange.
    // back: seen from behind (the player's right on the viewer's right); front: facing the viewer (the player's right on the viewer's left).
    // The slots: 1 the holster at the right hip, 2 the sling across the front of the hips (front only), 3 both shoulders (two rifles crossed on
    // the back, seen from behind). The preview (research README, preview_render.py) draws the same coordinates.
    void figure(ImDrawList* dl, ImVec2 p0, float w, float s, bool back)
    {
        const float cx = p0.x + 0.5f * w;
        auto        P  = [&](float x, float y) { return ImVec2{ cx + x * s, p0.y + y * s }; };
        const float R  = back ? 1.0f : -1.0f;   // the player's right, in screen x
        const float br = s * 0.5f;              // badge radius
        // body
        dl->AddCircleFilled(P(0.0f, 0.95f), 0.72f * s, kBodyC, 32);
        capsule(dl, P(0.0f, 1.5f), P(0.0f, 2.1f), 0.24f * s, kBodyC);
        poly(dl, { P(-1.0f, 2.05f), P(1.0f, 2.05f), P(1.3f, 2.55f), P(0.92f, 4.75f), P(-0.92f, 4.75f), P(-1.3f, 2.55f) }, kBodyC);
        poly(dl, { P(-0.95f, 4.6f), P(0.95f, 4.6f), P(1.02f, 5.55f), P(-1.02f, 5.55f) }, kBodyC);
        for (float x : { -1.0f, 1.0f }) {
            capsule(dl, P(x * 1.2f, 2.5f), P(x * 1.75f, 4.2f), 0.29f * s, kBodyC);
            capsule(dl, P(x * 1.75f, 4.2f), P(x * 1.98f, 5.75f), 0.25f * s, kBodyC);
            dl->AddCircleFilled(P(x * 2.02f, 6.0f), 0.3f * s, kBodyC, 20);
            capsule(dl, P(x * 0.55f, 5.3f), P(x * 0.62f, 7.5f), 0.42f * s, kBodyC);
            capsule(dl, P(x * 0.62f, 7.5f), P(x * 0.68f, 9.35f), 0.33f * s, kBodyC);
            capsule(dl, P(x * 0.6f, 9.6f), P(x * 0.9f, 9.62f), 0.2f * s, kBodyC);
        }
        dl->AddLine(P(-2.8f, 9.9f), P(2.8f, 9.9f), kDimC, 2.0f);   // the floor
        // A rifle: from o (the stock's end) along ang, L units long; flip mirrors it across its length (the magazine on the other side).
        auto rifle = [&](float ox, float oy, float ang, float L, float flip) {
            const float ux = std::cos(ang), uy = std::sin(ang), vx = -uy * flip * 0.7f, vy = ux * flip * 0.7f;
            auto        Q  = [&](float a, float b) { return P(ox + (ux * a + vx * b) * L, oy + (uy * a + vy * b) * L); };
            poly(dl, { Q(0.0f, -0.05f), Q(0.22f, -0.04f), Q(0.22f, 0.05f), Q(0.0f, 0.08f) }, kGunC);         // stock
            poly(dl, { Q(0.2f, -0.045f), Q(0.6f, -0.045f), Q(0.6f, 0.045f), Q(0.2f, 0.045f) }, kGunC);      // receiver
            poly(dl, { Q(0.44f, 0.04f), Q(0.52f, 0.04f), Q(0.5f, 0.15f), Q(0.43f, 0.14f) }, kGunDarkC);     // magazine
            poly(dl, { Q(0.3f, 0.04f), Q(0.34f, 0.04f), Q(0.33f, 0.1f), Q(0.3f, 0.1f) }, kGunDarkC);        // grip
            capsule(dl, Q(0.6f, -0.012f), Q(0.98f, -0.012f), 0.016f * L * s, kGunC);                        // barrel
            poly(dl, { Q(0.36f, -0.045f), Q(0.46f, -0.045f), Q(0.46f, -0.075f), Q(0.36f, -0.075f) }, kGunDarkC);   // sight
        };
        // 3: the main weapons crossed on the back (seen from behind), grabbed at either shoulder
        if (back) {
            rifle(-0.85f, 5.6f, std::atan2(-4.7f, 2.4f), 5.0f, 1.0f);
            rifle(0.85f, 5.6f, std::atan2(-4.7f, -2.4f), 5.0f, -1.0f);
        }
        for (float x : { -1.0f, 1.0f }) {
            dl->AddLine(P(x * 1.15f, 2.2f), P(x * 2.25f, 1.45f), kDimC, 1.5f);
            badge(dl, P(x * 2.6f, 1.25f), br, "3");
        }
        // 1: the handgun in the holster at the right hip (the holster, the grip above it)
        poly(dl, { P(R * 1.0f, 5.0f), P(R * 1.42f, 5.0f), P(R * 1.36f, 6.25f), P(R * 1.06f, 6.25f) }, kGunDarkC);
        poly(dl, { P(R * 1.02f, 4.55f), P(R * 1.3f, 4.48f), P(R * 1.42f, 5.02f), P(R * 1.06f, 5.05f) }, kGunC);
        dl->AddLine(P(R * 1.45f, 5.3f), P(R * 3.25f, 5.3f), kDimC, 1.5f);
        badge(dl, P(R * 3.65f, 5.3f), br, "1");
        // 2: the selected main weapon on the sling, across the front of the hips, the stock at the right hip
        if (!back) {
            rifle(R * 2.1f, 6.1f, R < 0.0f ? -0.06f : 3.14159265f + 0.06f, 4.3f, R < 0.0f ? 1.0f : -1.0f);
            dl->AddLine(P(-R * 2.2f, 6.0f), P(-R * 3.25f, 6.6f), kDimC, 1.5f);
            badge(dl, P(-R * 3.65f, 6.65f), br, "2");
        }
        // which side is which, and the view
        label(dl, P(R * 2.55f, 7.1f), s * 0.7f, kDimC, "R");
        label(dl, P(-R * 2.55f, 7.1f), s * 0.7f, kDimC, "L");
        label(dl, P(0.0f, 10.75f), s * 0.85f, kDimC, back ? "Back" : "Front");
    }
}

bool draw()
{
    if (!g_showing.load()) return false;
    static const ImVec4 kAccent{ 0.93f, 0.46f, 0.13f, 1.00f };
    static const ImVec4 kText{ 0.95f, 0.92f, 0.86f, 1.00f };
    static const ImVec4 kHint{ 0.66f, 0.62f, 0.55f, 1.00f };
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{ disp.x * 0.5f, disp.y * 0.5f }, ImGuiCond_Always, ImVec2{ 0.5f, 0.5f });
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.075f, 0.068f, 0.060f, 0.94f });
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{ 0.55f, 0.27f, 0.09f, 1.00f });
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 28.0f, 22.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::Begin("##fc5vr_firstguntip", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextColored(kAccent, "Your weapons in VR");
    ImGui::Spacing();
    struct Row { const char* num; const char* label; const char* text; };
    static const Row kRows[]{
        { "1", "Handguns", "In the holster: right grip at your right hip." },
        { "2", "Selected main weapon", "On the sling: right grip in front of your hips." },
        { "3", "Main weapons", "Right grip at your right or left shoulder." },
        { "X", "Weapon wheel", "Press X to pick any weapon." },
    };
    const float fh = ImGui::GetFontSize(), bw = fh * 1.3f;
    float       lw = 0.0f, tw = 0.0f;
    for (const auto& r : kRows) {
        lw = std::max(lw, ImGui::CalcTextSize(r.label).x);
        tw = std::max(tw, ImGui::CalcTextSize(r.text).x);
    }
    const float x0 = ImGui::GetCursorPosX(), col = x0 + bw + lw + 24.0f, width = bw + lw + 24.0f + tw;
    // The two figures, side by side and centred over the rows.
    {
        const float  size = 330.0f, s = size / 11.8f, cw = 9.0f * s, gap = 70.0f;
        const ImVec2 p0   = ImGui::GetCursorScreenPos();
        const float  left = p0.x + std::max(0.0f, 0.5f * (width - (2.0f * cw + gap)));
        ImGui::Dummy(ImVec2{ width, size });
        figure(dl, ImVec2{ left, p0.y + 0.5f * s }, cw, s, false);
        figure(dl, ImVec2{ left + cw + gap, p0.y + 0.5f * s }, cw, s, true);
    }
    ImGui::Spacing();
    for (const auto& r : kRows) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        badge(dl, ImVec2{ p.x + 0.45f * fh, p.y + 0.5f * fh }, 0.42f * fh, r.num);
        ImGui::SetCursorPosX(x0 + bw);
        ImGui::TextColored(kAccent, "%s", r.label);
        ImGui::SameLine(col);
        ImGui::TextUnformatted(r.text);
    }
    ImGui::Spacing();
    ImGui::TextColored(kHint, "%s", "Main weapons are all weapons except handguns.");
    ImGui::Spacing();
    const float left = kMinS - shown_s();
    const char* state = g_paused.load() && fc5::lua::paused_by_mod() ? "The game is paused. " : "";
    if (left > 0.0f)
        ImGui::TextColored(kHint, "%sPress A or the trigger to continue (in %d s)", state, std::max(1, int(std::ceil(left))));
    else
        ImGui::TextColored(kHint, "%sPress A or the trigger to continue", state);
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    return true;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("Weapon tips at the first gun looted##firstguntip", &v)) cmds += v ? "firstguntip on\n" : "firstguntip off\n";
    hint("The first time you pick up a gun, a popup shows where to take your weapons from. It shows once; tick it to see it again.");
    bool p = g_pause_on.load();
    if (ImGui::Checkbox("Pause the game while the weapon tips show##firstgunpause", &p)) cmds += p ? "firstgunpause on\n" : "firstgunpause off\n";
    hint("Pauses the game while the weapon tips popup shows.");
    if (ImGui::Button("Show the weapon tips now##firstgunshow")) cmds += "firstguntip show\n";
    hint("Closes this menu and shows the weapon tips popup now.");
}
}
