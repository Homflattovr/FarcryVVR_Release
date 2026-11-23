// bodyik, bodydump (FarCry5BodyIk.h; research\2026-11-11_bodyik\README.md): the player's own outfit drawn as a body under the headset.
//  - update_motion calls bodyik_frame once a frame: the head pose (reference space) gives the body (FarCry5BodyIk.cpp body_pose), each
//    controller's move since the last frame carries an older hand forward. The body is made at the game camera; the view's set-up then
//    moves it to the eyes' base (bodyik_base), where the eyes stand and the hands are drawn (viewroot, viewpivot, the steady view).
//  - weapon_draw_impl calls bodyik_draw for each player draw of the hide branch: the gloves and bare hands that go ahead (how 0, after the
//    hands' moves), the arms and sleeves the mod hides (how 1) and the clothing glovemesh hides (how 2). world_character_draw calls it for
//    the player's own draws of the world pass (how 3: the legs and feet), which hidebody hides.
//  - A draw's palette slots get their body part from the mesh itself (the rest-pose centres the hands' scan already reads, glove_mesh), so
//    no table of models is needed: any outfit, direct or indirect draws. The new palette is bound with the hands' stand-in (b3 and b4)
//    and put back after the draw by the draw hooks.
//  - Left as it is today: a draw whose mesh or palette is not read yet, a mounted weapon with mountedhands off, and the eye behind a scope
//    (mountedhands on: the mounted pass's own hand map, drawn where the game draws it, FarCry5Mounted.cpp). In a vehicle
//    bodyikveh (bodyik_veh below) draws the game's seated body instead, its arms going to the hands drivehands places.
//  - bodydump N: the next N frames' player draws written to bin\fc5vr_bodydump.bin with every matrix, so tools\fullbodyik\bench\replay.py
//    can run the same math on them offline and draw the result.
//  - bodyikseatdump N: the next N frames' seated draws (bodyikveh) written to bin\fc5vr_seatdump.bin with seat()'s whole input (the hands as
//    drivehands moved them, held or free) and its output, so tools\fullbodyik\bench\seatreplay.py runs the seated arms offline on real drives.
#include "FarCry5Stereo.h"
#include "FarCry5BodyIk.h"
#include "FarCry5Gesture.h"
#include "FarCry5Record.h"

#include <mods/VR.hpp>

#include <d3d11_1.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <istream>
#include <mutex>
#include <set>
#include <vector>

namespace fc5::bodyik
{
    namespace
    {
        std::mutex         g_mtx;
        Opts               g_o{};
        std::atomic<bool>  g_on{ false };
        std::atomic<int>   g_dump{ 0 };
        std::atomic<int>   g_snaplog{ 0 };   // bodyiksnaplog
        std::atomic<int>   g_seatdump{ 0 };
        std::atomic<bool>  g_roles{ true };    // handroles (always on since 13 Nov, author 19:50Z: no longer a setting)
        std::atomic<int>   g_placed{ 0 }, g_left{ 0 }, g_hands{ 0 }, g_carried{ 0 }, g_legs{ 0 };
        std::atomic<float> g_reach[2]{ 0.0f, 0.0f }, g_gap{ 0.0f }, g_fit{ 0.0f }, g_cal{ 0.0f };
        std::atomic<bool>  g_seated{ false };
        // bodyikcalibrate: the window and its samples (g_cal_mtx), the last result for F11.
        std::mutex                            g_cal_mtx;
        int                                   g_cal_state{ 0 };   // 0 idle, 1 waiting for the T-pose, 2 sampling
        bool                                  g_cal_started{ false };
        std::chrono::steady_clock::time_point g_cal_t0{};
        std::vector<CalSample>                g_cal_samples;
        std::string                           g_cal_msg;          // the result's details (F11 and the popup)
        int                                   g_cal_result{ 0 };   // 0 none this session, 1 done, -1 not taken
        std::chrono::steady_clock::time_point g_cal_done_t{};
        std::string                           g_cal_why;           // not taken: why
        // bodyikintro: the welcome popup.
        std::atomic<bool>                     g_intro_on{ true };   // not shown yet (saved off once shown)
        int                                   g_intro_state{ 0 };   // 0 waiting for play, 1 showing, 2 done
        bool                                  g_intro_shown_new{ false };
        std::chrono::steady_clock::time_point g_intro_play_t{}, g_intro_t0{};
        std::string body_line(const Opts& o)
        {
            return fmt::format("bodyikbody {:.3f} {:.3f} {:.3f}", o.cal_span, o.cal_drop, o.cal_grip);
        }
    }

    Opts opts()
    {
        std::lock_guard _{ g_mtx };
        return g_o;
    }
    bool on() { return g_on.load(); }
    bool seated() { return g_seated.load(); }

    int cal_state()
    {
        std::lock_guard _{ g_cal_mtx };
        return g_cal_state;
    }
    bool cal_take_start()
    {
        std::lock_guard _{ g_cal_mtx };
        const bool s  = g_cal_started;
        g_cal_started = false;
        return s;
    }
    void cal_add(const CalSample& smp)
    {
        std::lock_guard _{ g_cal_mtx };
        if (g_cal_state == 2 && g_cal_samples.size() < 4000) g_cal_samples.push_back(smp);
    }
    bool cal_tick()
    {
        std::lock_guard _{ g_cal_mtx };
        if (g_cal_state == 0) return false;
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_cal_t0).count();
        if (g_cal_state == 1 && t >= kCalWait) {
            g_cal_state   = 2;
            g_cal_started = true;   // the sampling's buzz
            return false;
        }
        if (g_cal_state == 2 && t >= kCalWait + kCalSample) {
            g_cal_state = 0;
            return true;
        }
        return false;
    }
    // The window has ended: solve it; ok: the measures applied (the caller saves the returned line), else the old ones kept ("" returned).
    std::string cal_finish(CalResult* out)
    {
        std::vector<CalSample> smp;
        {
            std::lock_guard _{ g_cal_mtx };
            smp = std::move(g_cal_samples);
            g_cal_samples.clear();
        }
        const CalResult r = cal_solve(smp);
        if (out) *out = r;
        std::string line, msg;
        if (r.ok) {
            {
                std::lock_guard _{ g_mtx };
                g_o.cal      = true;
                g_o.cal_span = r.span;
                g_o.cal_drop = r.drop;
                g_o.cal_grip = r.grip;
                line         = body_line(g_o);
            }
            msg = fmt::format("Arm span {:.0f} cm (controller to controller), shoulders {:.0f} cm below your eyes, each arm about {:.0f} cm "
                              "(shoulder to wrist), height about {:.0f} cm. The drawn arms are {:.0f}% of the model's.",
                              r.span * 100.0f, r.drop * 100.0f, r.arm * 100.0f, r.height * 100.0f, cal_arm_k(opts(), 1.0f) * 100.0f);
            spdlog::info("[BodyIk] calibration: span {:.3f} m, the hands {:.3f} m below the eyes and {:+.3f} m behind them, {:.3f} m apart in height; "
                         "the drawn wrist {:.3f} m from the controller ({}); height about {:.2f} m, arm about {:.3f} m; {} of {} samples used",
                         r.span, r.drop, r.back, r.level, r.grip, r.grip_measured ? "measured on the drawn body" : "the usual", r.height, r.arm, r.used, r.total);
        } else {
            msg = r.total == 0 ? std::string{ "no head or controller pose was read (motion controls off, or the game paused?)" } : r.why;
            spdlog::info("[BodyIk] calibration not taken: {} ({} samples)", msg, r.total);
        }
        {
            std::lock_guard _{ g_cal_mtx };
            g_cal_msg    = r.ok ? msg : std::string{};
            g_cal_why    = r.ok ? std::string{} : msg;
            g_cal_result = r.ok ? 1 : -1;
            g_cal_done_t = std::chrono::steady_clock::now();
        }
        return line;
    }
    // F11's line under the button while a calibration runs ("" otherwise: the menu then says done, not taken or the saved measures).
    std::string cal_text()
    {
        std::lock_guard _{ g_cal_mtx };
        if (g_cal_state == 0) return {};
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_cal_t0).count();
        if (g_cal_state == 1)
            return fmt::format("Get into the T-pose: measuring in {:.0f} s", std::max(1.0f, std::ceil(kCalWait - t)));
        return fmt::format("Measuring: hold still, arms straight ({:.0f} s)", std::max(1.0f, std::ceil(kCalWait + kCalSample - t)));
    }

    void intro_play(bool in_play)
    {
        const bool calibrated = opts().cal;   // (g_mtx before g_cal_mtx, never inside it)
        std::lock_guard _{ g_cal_mtx };
        const auto now = std::chrono::steady_clock::now();
        if (g_intro_state != 0) return;
        if (!g_intro_on.load() || calibrated) {   // shown before, or a calibration already saved: nothing to tell
            g_intro_state = 2;
            return;
        }
        if (!in_play || g_cal_state != 0) {
            g_intro_play_t = {};
            return;
        }
        if (g_intro_play_t == std::chrono::steady_clock::time_point{}) g_intro_play_t = now;
        if (std::chrono::duration<float>(now - g_intro_play_t).count() >= kIntroAfter) {
            g_intro_state     = 1;
            g_intro_t0        = now;
            g_intro_shown_new = true;
            g_intro_on        = false;
            spdlog::info("[BodyIk] the welcome popup shows (first use): where Calibrate my body is; bodyikintro off saved");
        }
    }
    bool intro_take_shown()
    {
        std::lock_guard _{ g_cal_mtx };
        const bool s      = g_intro_shown_new;
        g_intro_shown_new = false;
        return s;
    }
    void intro_dismiss()
    {
        std::lock_guard _{ g_cal_mtx };
        if (g_intro_state == 1) g_intro_state = 2;
    }

    namespace
    {
        constexpr float kPopupWidth = 640.0f;   // the popup's text width
        // The pose to take, drawn: a figure facing you, arms straight out at shoulder height (seated: on a chair). size: the canvas's height.
        void draw_pose(float size, bool sit, float pulse)
        {
            static const ImU32 kBody = IM_COL32(242, 235, 219, 255), kArm = IM_COL32(237, 117, 33, 255), kDim = IM_COL32(120, 110, 95, 255);
            const float w = size * 1.25f, s = size / 10.0f;   // s: one unit; the figure is about 10 units tall
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (kPopupWidth - w) * 0.5f));   // centred in the popup
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2{ w, size });
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float cx = p0.x + 0.5f * w;
            auto P = [&](float x, float y) { return ImVec2{ cx + x * s, p0.y + y * s }; };
            const float th = std::max(3.0f, s * 0.45f);   // line thickness
            // head, neck and torso
            dl->AddCircle(P(0.0f, 1.0f), 0.85f * s, kBody, 24, th);
            dl->AddLine(P(0.0f, 1.85f), P(0.0f, 2.4f), kBody, th);
            const float hip_y = sit ? 5.6f : 5.4f;
            dl->AddLine(P(-0.9f, 2.4f), P(0.9f, 2.4f), kBody, th);   // shoulders
            dl->AddLine(P(0.0f, 2.4f), P(0.0f, hip_y), kBody, th);
            dl->AddLine(P(-0.6f, hip_y), P(0.6f, hip_y), kBody, th);
            // arms straight out at shoulder height, hands open (the controllers)
            const float reach = 4.6f;
            dl->AddLine(P(0.9f, 2.4f), P(reach, 2.4f), kArm, th * 1.2f);
            dl->AddLine(P(-0.9f, 2.4f), P(-reach, 2.4f), kArm, th * 1.2f);
            dl->AddCircleFilled(P(reach + 0.35f, 2.4f), 0.38f * s, kArm, 16);
            dl->AddCircleFilled(P(-reach - 0.35f, 2.4f), 0.38f * s, kArm, 16);
            // a level guide at shoulder height, pulsing
            const ImU32 guide = IM_COL32(237, 117, 33, int(60 + 120 * pulse));
            dl->AddLine(P(-reach - 1.0f, 2.4f), P(reach + 1.0f, 2.4f), guide, 1.5f);
            if (!sit) {
                dl->AddLine(P(-0.5f, hip_y), P(-0.8f, 9.7f), kBody, th);
                dl->AddLine(P(0.5f, hip_y), P(0.8f, 9.7f), kBody, th);
                dl->AddLine(P(-2.5f, 9.8f), P(2.5f, 9.8f), kDim, 2.0f);   // the floor
            } else {
                // seen from the front on a chair: thighs towards you (short), shins down, the seat and the chair's legs
                dl->AddLine(P(-0.5f, hip_y), P(-0.7f, 6.6f), kBody, th);
                dl->AddLine(P(0.5f, hip_y), P(0.7f, 6.6f), kBody, th);
                dl->AddLine(P(-0.7f, 6.6f), P(-0.8f, 9.7f), kBody, th);
                dl->AddLine(P(0.7f, 6.6f), P(0.8f, 9.7f), kBody, th);
                dl->AddRect(P(-1.6f, 5.9f), P(1.6f, 6.3f), kDim, 2.0f, 0, 2.0f);   // the seat
                dl->AddLine(P(-1.5f, 6.3f), P(-1.5f, 9.7f), kDim, 2.0f);
                dl->AddLine(P(1.5f, 6.3f), P(1.5f, 9.7f), kDim, 2.0f);
                dl->AddLine(P(-2.5f, 9.8f), P(2.5f, 9.8f), kDim, 2.0f);
            }
        }
        struct PopupStyle
        {
            PopupStyle()
            {
                const ImVec2 disp = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos(ImVec2{ disp.x * 0.5f, disp.y * 0.5f }, ImGuiCond_Always, ImVec2{ 0.5f, 0.5f });
                ImGui::PushFont(nullptr, 26.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.075f, 0.068f, 0.060f, 0.94f });
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{ 0.55f, 0.27f, 0.09f, 1.00f });
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.95f, 0.92f, 0.86f, 1.00f });
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{ 0.16f, 0.14f, 0.12f, 1.00f });
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{ 0.93f, 0.46f, 0.13f, 1.00f });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 28.0f, 22.0f });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
                ImGui::Begin("##fc5vr_bodyikpopup", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kPopupWidth);
            }
            ~PopupStyle()
            {
                ImGui::PopTextWrapPos();
                ImGui::End();
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(5);
                ImGui::PopFont();
            }
        };
        const ImVec4 kAccent{ 0.93f, 0.46f, 0.13f, 1.00f }, kHint{ 0.66f, 0.62f, 0.55f, 1.00f }, kDone{ 0.45f, 0.80f, 0.40f, 1.00f },
            kBad{ 0.92f, 0.38f, 0.30f, 1.00f };
    }

    bool popup_draw()
    {
        int         state, result, intro;
        float       t, since_done, since_intro;
        std::string msg, why;
        {
            std::lock_guard _{ g_cal_mtx };
            const auto now = std::chrono::steady_clock::now();
            state          = g_cal_state;
            result         = g_cal_result;
            intro          = g_intro_state;
            t              = std::chrono::duration<float>(now - g_cal_t0).count();
            since_done     = result != 0 ? std::chrono::duration<float>(now - g_cal_done_t).count() : 1e9f;
            since_intro    = std::chrono::duration<float>(now - g_intro_t0).count();
            if (intro == 1 && since_intro > kIntroShow) g_intro_state = intro = 2;
            msg = g_cal_msg;
            why = g_cal_why;
        }
        const bool  sit   = seated();
        const float pulse = 0.5f + 0.5f * std::sin(t * 6.0f);
        if (state == 1 || state == 2) {
            PopupStyle _p;
            ImGui::TextColored(kAccent, "Body calibration");
            ImGui::TextWrapped("%s", sit ? "Sit straight, look ahead, both arms straight out to the sides at shoulder height, palms down."
                                         : "Stand straight, look ahead, both arms straight out to the sides at shoulder height, palms down.");
            draw_pose(260.0f, sit, pulse);
            if (state == 1) {
                ImGui::PushFont(nullptr, 72.0f);
                char n[8];
                std::snprintf(n, sizeof(n), "%d", int(std::max(1.0f, std::ceil(kCalWait - t))));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (kPopupWidth - ImGui::CalcTextSize(n).x) * 0.5f));
                ImGui::TextColored(kAccent, "%s", n);
                ImGui::PopFont();
                ImGui::TextColored(kHint, "Get into the pose: measuring starts at the buzz");
            } else {
                ImGui::TextColored(kAccent, "Measuring: hold still, arms straight");
                ImGui::ProgressBar(std::clamp((t - kCalWait) / kCalSample, 0.0f, 1.0f), ImVec2{ 440.0f, 12.0f }, "");
                ImGui::TextColored(kHint, "%d s, until the second buzz", int(std::max(1.0f, std::ceil(kCalWait + kCalSample - t))));
            }
            return true;
        }
        if (result != 0 && since_done < kCalShow) {
            PopupStyle _p;
            if (result > 0) {
                ImGui::TextColored(kDone, "Calibration done");
                ImGui::TextWrapped("%s", msg.c_str());
                ImGui::TextColored(kHint, "Saved. You can lower your arms.");
            } else {
                ImGui::TextColored(kBad, "Calibration not taken");
                ImGui::TextWrapped("%s.", why.c_str());
                ImGui::TextColored(kHint, "The old measures are kept. Try again from F11 > Motion control > Hands, body and arms.");
            }
            return true;
        }
        if (intro == 1) {
            PopupStyle _p;
            ImGui::TextColored(kAccent, "Fit the body to you");
            ImGui::Text("Calibrate once so the drawn arms are as long as yours:");
            ImGui::TextColored(kAccent, "F11 (or click both thumbsticks together) > Motion control > Hands, body and arms > Calibrate my body (T-pose)");
            ImGui::Text("It counts down, then you hold this pose for 3 s:");
            draw_pose(180.0f, sit, pulse);
            if (!on()) ImGui::TextWrapped("To see your arms and body, choose \"Full body and arms\" there (You see).");
            ImGui::TextWrapped("Playing seated? Tick \"I play seated\" there, and calibrate seated.");
            ImGui::TextColored(kHint, "This closes in %d s (shown once).", int(std::max(1.0f, std::ceil(kIntroShow - since_intro))));
            return true;
        }
        return false;
    }
    bool hand_roles_on() { return g_roles.load(); }
    int  dump_frames() { return g_dump.load(); }
    int  snap_log_frames() { return g_snaplog.load(); }
    void snap_log_done() { if (g_snaplog.load() > 0) g_snaplog--; }
    void dump_frame_done() { if (g_dump.load() > 0) g_dump--; }
    int  seat_dump_frames() { return g_seatdump.load(); }
    void seat_dump_frame_done() { if (g_seatdump.load() > 0) g_seatdump--; }

    void note_status(int placed, int left, int hands, int carried, int legs, float reach_r, float reach_l, float gap, float fit, float cal)
    {
        g_placed = placed; g_left = left; g_hands = hands; g_carried = carried; g_legs = legs;
        g_reach[0] = reach_r; g_reach[1] = reach_l; g_gap = gap; g_fit = fit; g_cal = cal;
    }

    std::string status()
    {
        return fmt::format("last second: {} draws placed ({} of the legs), {} left as they were; hands read {} times, carried {}; reach right {:.0f}% left {:.0f}%; "
                           "widest gap {:.1f} cm; wrist fit {:.1f} mm; the game's hips {:+.1f} cm from the body's",
                           g_placed.load(), g_legs.load(), g_left.load(), g_hands.load(), g_carried.load(), g_reach[0].load() * 100.0f, g_reach[1].load() * 100.0f,
                           g_gap.load() * 100.0f, g_fit.load() * 1000.0f, g_cal.load() * 100.0f);
    }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        // armik and its settings (8 Nov) are gone: an old settings line is read and dropped.
        if (cmd.rfind("armik", 0) == 0 || cmd == "armshoulder" || cmd == "armcensus") {
            persist = false;
            spdlog::info("[BodyIk] {}: armik was replaced by bodyik (F11 Experimental > Body and arms); the line is dropped", cmd);
            return true;
        }
        if (cmd == "handroles") {
            const bool v = arg == "on" || arg == "1" || arg == "true";
            g_roles      = v;
            persist      = true;
            spdlog::info("[BodyIk] handroles {}: {}", arg, v ? "a mesh's hands, palm and fingers are read from the rest skeleton (every outfit)" : "off: the hands by shape and by the bone tables, as before");
            return true;
        }
        if (cmd == "seated" || cmd == "bodyikseated") {
            const bool v = arg == "on" || arg == "1" || arg == "true";
            {
                std::lock_guard _{ g_mtx };
                g_o.seated = v;
            }
            g_seated = v;
            persist  = true;
            spdlog::info("[BodyIk] {} {}: {}", cmd, arg, v ? "seated play: the drawn hips stay on the seat and the torso leans to the head, no crouch from the head, room scale does not walk"
                                                           : "standing play, as before");
            return true;
        }
        if (cmd == "bodyikcalibrate") {
            persist = false;
            std::lock_guard _{ g_cal_mtx };
            g_cal_state   = 1;
            g_cal_started = false;   // the buzz comes when the measure starts
            g_cal_t0    = std::chrono::steady_clock::now();
            g_cal_samples.clear();
            g_cal_result = 0;
            if (g_intro_state == 1) g_intro_state = 2;
            spdlog::info("[BodyIk] calibration: {:.0f} s to get into the T-pose, then {:.0f} s measured (the popup shows it, F11 closed)", kCalWait, kCalSample);
            return true;
        }
        if (cmd == "bodyikintro") {
            // bodyikintro on|off (saved; default on: the welcome popup still to show; off once shown). "bodyikintro show": show it now, for
            // kIntroShow s or until F11 opens (a test: also in test mode, nothing saved).
            std::lock_guard _{ g_cal_mtx };
            if (arg == "show") {
                persist       = false;
                g_intro_state = 1;
                g_intro_t0    = std::chrono::steady_clock::now();
                spdlog::info("[BodyIk] bodyikintro show: the welcome popup shows now ({:.0f} s, or until F11 opens)", kIntroShow);
                return true;
            }
            const bool v = arg == "on" || arg == "1" || arg == "true";
            persist      = true;
            g_intro_on   = v;
            if (!v && g_intro_state == 0) g_intro_state = 2;
            spdlog::info("[BodyIk] bodyikintro {}: {}", arg, v ? "the welcome popup shows after the first seconds of play" : "the welcome popup was shown (or is not wanted)");
            return true;
        }
        if (cmd.rfind("bodyik", 0) != 0 && cmd != "bodydump") return false;
        const bool                   on = arg == "on" || arg == "1" || arg == "true";
        std::unique_lock<std::mutex> lk{ g_mtx };
        persist   = true;
        auto said = [&](const std::string& what) { spdlog::info("[BodyIk] {} {}: {}", cmd, arg, what); };
        if (cmd == "bodydump") {
            persist = false;
            g_dump  = std::clamp(arg.empty() ? 2 : int(fv), 1, 20);
            lk.unlock();
            said("the next frames' player draws written to fc5vr_bodydump.bin");
            return true;
        }
        if (cmd == "bodyikseatdump") {
            persist    = false;
            g_seatdump = std::clamp(arg.empty() ? 270 : int(fv), 1, 900);
            lk.unlock();
            said(fmt::format("the next {} frames' seated draws (in a vehicle) written to fc5vr_seatdump.bin", g_seatdump.load()));
            return true;
        }
        if (cmd == "bodyik") {
            g_o.on = on;
            g_on   = on;
            lk.unlock();
            said(on ? "the outfit drawn as a body under the headset (needs hidearms and hidebody on)" : "off: the mod's hands only, as before");
            return true;
        }
        if (cmd == "bodyikcarry") {
            // Replaced by bodyikworn (14 Nov): a saved line is read and dropped at the next save.
            persist = false;
            lk.unlock();
            said("no longer a setting: see bodyikworn (the carried things worn on the drawn body)");
            return true;
        }
        if (cmd == "bodyikworn") {
            g_o.worn = on;
            lk.unlock();
            said(on ? "the slung weapon, the sidearm's holster, the throwables' pouch and the magazine worn on the drawn body, their grab zones with them (with bodyik on, on foot)"
                    : "off: they are placed from the eyes (slingpos, sidearmpos, throwpos, reloadpouch), as with the hands only");
            return true;
        }
        if (cmd == "bodyikwornsling" || cmd == "bodyikwornholster" || cmd == "bodyikwornpouch" || cmd == "bodyikwornmag") {
            // x y z (saved): the thing's move from its place on the body (model metres: right, forward, up; mirrored for a left-handed player).
            // No argument: back to its place.
            const int part = cmd == "bodyikwornsling" ? kWornSling : cmd == "bodyikwornholster" ? kWornHolster : cmd == "bodyikwornpouch" ? kWornPouch : kWornMag;
            float     y = 0.0f, z = 0.0f;
            if (!arg.empty()) rest >> y >> z;
            const glm::vec3 v = arg.empty() ? glm::vec3{ 0.0f } : glm::clamp(glm::vec3{ fv, y, z }, glm::vec3{ -0.3f }, glm::vec3{ 0.3f });
            g_o.worn_adj[part] = v;
            lk.unlock();
            const glm::vec3 r = worn_rest(part) + v;
            said(fmt::format("the {} {:.0f} cm right, {:.0f} cm forward, {:.0f} cm up of its place on the body (now {:.0f}, {:.0f}, {:.0f} cm from the hips' middle at the floor)",
                             worn_name(part), v.x * 100.0f, v.y * 100.0f, v.z * 100.0f, r.x * 100.0f, r.y * 100.0f, r.z * 100.0f));
            return true;
        }
        if (cmd == "bodyikhunch") {
            g_o.hunch = on;
            lk.unlock();
            said(on ? "looking down, the back curls and the hips stay where you stand (with the holster and the magazine on them)"
                    : "off: looking down, the whole torso swings back about the neck (the hips go back), as before 14 Nov");
            return true;
        }
        if (cmd == "bodyikhunchgain") {
            // g (saved): how much of the bow the curled back takes (author 14 Nov 16:49Z with all of it).
            g_o.hunch_gain = std::clamp(arg.empty() ? 0.5f : fv, 0.0f, 1.0f);
            const float g  = g_o.hunch_gain;
            lk.unlock();
            said(fmt::format("looking down, the upper back bows {:.0f}% of the bow ({:.0f} deg looking 75 deg down; 0: the torso stays upright)", g * 100.0f, g * 35.0f));
            return true;
        }
        if (cmd == "bodyikveh") {
            g_o.veh = on;
            lk.unlock();
            said(on ? "driving, the seated body drawn as the game poses it, its arms going to the hands on the controllers (with bodyik on)" : "off: driving, the hands only (drivehands)");
            return true;
        }
        if (cmd == "bodyikbody") {
            // bodyikbody SPAN DROP GRIP (saved; written by the calibration): the player's arm span and the hands' height under the eyes in a
            // T-pose, the drawn wrist's distance from the controller (real metres). bodyikbody off: the model's own arms and shoulders.
            if (arg == "off" || arg == "0" || arg.empty()) {
                g_o.cal = false;
                lk.unlock();
                {
                    std::lock_guard _{ g_cal_mtx };
                    g_cal_msg.clear();
                }
                said("no calibration: the model's own arms and shoulders");
                return true;
            }
            float drop = g_o.cal_drop, grip = g_o.cal_grip;
            rest >> drop >> grip;
            g_o.cal      = fv >= 1.1f && fv <= 2.3f;
            g_o.cal_span = std::clamp(fv, 1.1f, 2.3f);
            g_o.cal_drop = std::clamp(drop, 0.05f, 0.45f);
            g_o.cal_grip = std::clamp(grip, 0.0f, 0.18f);
            const Opts o = g_o;
            lk.unlock();
            said(o.cal ? fmt::format("arm span {:.0f} cm, the hands {:.0f} cm below the eyes, the drawn wrist {:.1f} cm from the controller: the arms {:.0f}% of the model's, "
                                     "the shoulders {:+.1f} cm lower (at world scale 1)", o.cal_span * 100.0f, o.cal_drop * 100.0f, o.cal_grip * 100.0f,
                                     cal_arm_k(o, 1.0f) * 100.0f, cal_shoulder_dy(o, 1.0f) * 100.0f)
                       : std::string{ "not a person's arm span (1.1 to 2.3 m): no calibration" });
            return true;
        }
        if (cmd == "bodyikseatswing") {
            g_o.seat_swing = on;
            lk.unlock();
            said(on ? "seated, the elbow swings about the shoulder-hand line so the forearm rolls less (at most 100 deg)"
                    : "off: seated, the elbow on the game's side and the forearm rolling all the hand's turn, as before 13 Nov");
            return true;
        }
        if (cmd == "bodyikseatfollow") {
            g_o.seat_follow = on;
            lk.unlock();
            said(on ? "seated, every hand (free or holding): the elbow as standing (down, out and a little back, a third by the hand's turn), the bones their length"
                    : "off: seated, the two-bone arm (the elbow on the game's side of the shoulder-hand line; bodyikseatswing applies), as before 14 Nov");
            return true;
        }
        if (cmd == "bodyiklegs") { g_o.legs = on; lk.unlock(); said(on ? "the legs and feet drawn" : "the legs stay hidden"); return true; }
        if (cmd == "bodyiklegsupright") {   // test, not saved (default on)
            g_o.legs_upright = on;
            lk.unlock();
            said(on ? "the game's legs posed upside down (ankles 10 cm or more above the hips, a cutscene hanging the player) stay hidden"
                    : "off: the game's legs drawn under the body whatever their pose, upside down too (before 16 Nov)");
            return true;
        }
        if (cmd == "bodyikfeet") {
            int   to = g_o.feet_to;
            float pv = g_o.feet_pelvis;
            rest >> to >> pv;
            g_o.feet_max    = glm::radians(std::clamp(arg.empty() ? 40.0f : fv, 0.0f, 60.0f));
            g_o.feet_to     = arg.empty() ? 1 : std::clamp(to, 0, 2);
            g_o.feet_pelvis = arg.empty() ? 0.5f : std::clamp(pv, 0.0f, 1.0f);
            lk.unlock();
            said(fmt::format("the legs turned until the game's feet point {} (at most {:.0f} deg; 0: the hip line alone, as before 13 Nov); the waistband takes {:.0f}% of it",
                             g_o.feet_to == 2 ? "as the game has them, its steps where it walks" : g_o.feet_to == 1 ? "where the game walks" : "along the drawn body", glm::degrees(g_o.feet_max), 100.0f * g_o.feet_pelvis));
            return true;
        }
        if (cmd == "bodyiksnaphold") {
            // bodyiksnaphold on|off [S [DEG]] (test, not saved; default on, 0.6 s, 8 deg): through a snap turn the legs' turn and the walk turn hold
            // until the game's heading is back within DEG of where it was against the drawn body, S at most.
            persist     = false;
            float deg   = g_o.snap_hold_deg;
            float sec   = g_o.snap_hold_s;
            rest >> sec >> deg;
            g_o.snap_hold     = arg.empty() || on;
            g_o.snap_hold_s   = std::clamp(sec, 0.05f, 2.0f);
            g_o.snap_hold_deg = std::clamp(deg, 1.0f, 30.0f);
            const float s2 = g_o.snap_hold_s, d2 = g_o.snap_hold_deg;
            const bool  v  = g_o.snap_hold;
            lk.unlock();
            said(v ? fmt::format("through a snap turn the legs and the walk turn hold until the game's heading is back within {:.0f} deg of the body ({:.2f} s at most)", d2, s2)
                   : std::string{ "off: through a snap turn the legs and the walk turn chase the game camera that turns first, as before 14 Nov" });
            return true;
        }
        if (cmd == "bodyikwalkstick") {
            // bodyikwalkstick on|off (test, not saved; default on): the walk turn starts and ends with the left stick, not the game's eased speed.
            persist        = false;
            g_o.walk_stick = arg.empty() || on;
            const bool v   = g_o.walk_stick;
            lk.unlock();
            said(v ? "the body counts as walking the moment the left stick is pushed, and as standing the moment it is let go"
                   : "off: the body counts as walking from the game's speed eased over 0.2 s (about 0.1 s late on a start, 0.4 s on a stop), as before 15 Nov");
            return true;
        }
        if (cmd == "bodyikcarrydrawn") {
            // bodyikcarrydrawn on|off (test, not saved; default on, FarCry5BodyIk.h): the hands carried to a frame's draws before its hand draw by
            // the controllers' move as drawn (after draw_shift), not as update_motion read them.
            persist         = false;
            g_o.carry_drawn = arg.empty() || on;
            const bool v    = g_o.carry_drawn;
            lk.unlock();
            said(v ? "the arms drawn before the hands (the coat's sleeves) take the hands carried by the controllers as they are drawn"
                   : "off: the arms drawn before the hands take the hands carried by the controllers as update_motion read them, before the eyes' base "
                     "moved them (as before 16 Nov: the left eye's coat sleeves shook in Stereo)");
            return true;
        }
        if (cmd == "bodyikturnease") {
            // bodyikturnease FEET WALK (test, not saved; default 0.15 0.2 s; 0.25 0.3 until 14 Nov): the legs' turn and the walk turn's easing.
            persist    = false;
            float walk = g_o.walk_tau;
            rest >> walk;
            g_o.feet_tau = std::clamp(arg.empty() ? 0.15f : fv, 0.02f, 1.0f);
            g_o.walk_tau = std::clamp(arg.empty() ? 0.2f : walk, 0.02f, 1.0f);
            const float a = g_o.feet_tau, b = g_o.walk_tau;
            lk.unlock();
            said(fmt::format("the legs' turn eases over {:.2f} s, the walk turn over {:.2f} s (0.25 and 0.3 until 14 Nov)", a, b));
            return true;
        }
        if (cmd == "bodyiksnaplog") {
            // bodyiksnaplog N (test): one [BodyIk] snaplog line per frame for the next N frames (the snap, the hold, the game's heading, the legs).
            persist   = false;
            g_snaplog = std::clamp(arg.empty() ? 120 : int(fv), 1, 3000);
            lk.unlock();
            said(fmt::format("the next {} frames logged", g_snaplog.load()));
            return true;
        }
        if (cmd == "bodyikwalkturn") {
            g_o.walk_turn = on;
            lk.unlock();
            said(on ? "walking, the drawn body turns onto the game's heading" : "off: the drawn body keeps the gestures' body yaw while walking");
            return true;
        }
        if (cmd == "bodyikcollar") { g_o.collar = on; lk.unlock(); said(on ? "collars and hoods drawn" : "neck parts shrunk onto the neck"); return true; }
        if (cmd == "bodyikneck") {
            float back = g_o.neck_back;
            rest >> back;
            g_o.neck_down = std::clamp(arg.empty() ? 0.146f : fv, 0.0f, 0.4f);
            g_o.neck_back = std::clamp(back, -0.1f, 0.3f);
            lk.unlock();
            said("the neck joint below and behind the eyes set");
            return true;
        }
        if (cmd == "bodyikback") { g_o.back = std::clamp(arg.empty() ? 0.05f : fv, -0.1f, 0.3f); lk.unlock(); said("the body's place behind the neck set"); return true; }
        if (cmd == "bodyikstretch") { g_o.stretch = std::clamp(arg.empty() ? 1.25f : fv, 1.0f, 1.6f); lk.unlock(); said("most arm lengthening set"); return true; }
        if (cmd == "bodyiksoft") { g_o.soft = std::clamp(arg.empty() ? 0.97f : fv, 0.5f, 1.0f); lk.unlock(); said("soft reach set"); return true; }
        if (cmd == "bodyikelbow") {
            float ms = g_o.elbow_ms;
            rest >> ms;
            g_o.pole_hand = std::clamp(arg.empty() ? 0.35f : fv, 0.0f, 1.0f);
            g_o.elbow_ms  = std::clamp(ms, 0.0f, 500.0f);
            lk.unlock();
            said("the elbow's direction (the hand's share, easing ms) set");
            return true;
        }
        if (cmd == "bodyiklean") {
            float sec = g_o.follow_s;
            rest >> sec;
            g_o.leash    = std::clamp(arg.empty() ? 0.10f : fv, 0.0f, 0.3f);
            g_o.follow_s = std::clamp(sec, 0.0f, 5.0f);
            lk.unlock();
            said("the hips' leash and catch-up time set");
            return true;
        }
        if (cmd == "bodyikbow") {
            float gain = g_o.bow_gain;
            rest >> gain;
            g_o.bow_from = glm::radians(std::clamp(arg.empty() ? 40.0f : fv, 0.0f, 90.0f));
            g_o.bow_gain = std::clamp(gain, 0.0f, 1.5f);
            lk.unlock();
            said("the head pitch (deg) past which the torso bows with the head, and how much of it (0: never)");
            return true;
        }
        persist = false;
        return false;
    }

    // Author 13 Nov 19:44Z: Motion control > Hands, body and arms; its first choice is Hands only / Full body and arms (bodyik off / on).
    void draw_mode(std::string& cmds, const std::function<void(const char*)>& hint, bool seated_row)
    {
        int m = opts().on ? 1 : 0;
        if (ImGui::Combo("You see##bodyik", &m, "Hands only\0Full body and arms\0")) cmds += m == 1 ? "bodyik on\n" : "bodyik off\n";
        hint("Hands only: just your hands on the controllers. Full body and arms: your character's body is drawn under you, with arms reaching to your hands.");
        // The T-pose calibration and seated play (author 13 Nov 21:42Z).
        const Opts o = opts();
        if (ImGui::Button("Calibrate my body (T-pose)##bodyikcalibrate")) cmds += "bodyikcalibrate\n";
        hint("Stand (or sit) straight, look ahead and hold both arms out to the sides for 3 s. It fits the drawn arms and shoulders to your size.");
        if (o.cal) {
            ImGui::SameLine();
            if (ImGui::Button("Forget##bodyikbodyoff")) cmds += "bodyikbody off\n";
            hint("Goes back to the model's own arm length and shoulders.");
        }
        // Author 14 Nov 11:24Z: no countdown or left in the menu once it is made: "Calibration done" (or why not), then the measures.
        int         result;
        std::string why;
        {
            std::lock_guard _{ g_cal_mtx };
            result = g_cal_result;
            why    = g_cal_why;
        }
        if (const std::string t = cal_text(); !t.empty()) ImGui::TextColored(ImVec4{ 0.93f, 0.46f, 0.13f, 1.0f }, "%s", t.c_str());
        else if (result < 0) ImGui::TextColored(ImVec4{ 0.92f, 0.38f, 0.30f, 1.0f }, "Calibration not taken: %s. Try again.", why.c_str());
        else if (o.cal) {
            ImGui::TextColored(ImVec4{ 0.45f, 0.80f, 0.40f, 1.0f }, "Calibration done");
            ImGui::TextDisabled("Arm span %.0f cm, shoulders %.0f cm below your eyes; the drawn arms %.0f%% of the model's.", o.cal_span * 100.0f,
                                o.cal_drop * 100.0f, cal_arm_k(o, 1.0f) * 100.0f);
        } else ImGui::TextDisabled("Not calibrated: the model's own arms and shoulders.");
        if (!seated_row) return;   // F11 Main (author 15 Nov 20:03Z): "I play seated" only in Motion control > Hands, body and arms
        bool st = seated();
        if (ImGui::Checkbox("I play seated##seated", &st)) cmds += st ? "seated on\n" : "seated off\n";
        hint("For playing in a chair: the body stays seated and leaning does not make the character walk.");
    }

    // 13 Nov (author 19:44:35Z): bodyiklegs, bodyikwalkturn,
    // bodyikveh (on) and bodyikfeet (40 deg) have no rows; typed they still act for a session, never saved. The arm and torso tuning, the
    // dump are in the Debug level only; handroles is always on (author 13 Nov 19:50Z).
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, bool debug)
    {
        const Opts o = opts();
        if (o.on) {
            bool cy = o.worn;
            if (ImGui::Checkbox("Sling, sidearm, throwables and magazine worn on the body##bodyikworn", &cy)) cmds += cy ? "bodyikworn on\n" : "bodyikworn off\n";
            hint("The sling, holster, throwables pouch and magazine sit on the drawn body and move with it. Off: placed from your head. Command bodyikworn.");
            bool hu = o.hunch;
            if (ImGui::Checkbox("Looking down, the back curls (hips stay)##bodyikhunch", &hu)) cmds += hu ? "bodyikhunch on\n" : "bodyikhunch off\n";
            hint("Looking down, the upper back bends so the hips and legs stay in place. Off: the whole torso swings back. Command bodyikhunch.");
            float hg = o.hunch_gain;
            if (ImGui::SliderFloat("How far the back curls##bodyikhg", &hg, 0.0f, 1.0f, "%.2f")) cmds += fmt::format("bodyikhunchgain {:.2f}\n", hg);
            hint("How much the upper back bends when you look down. 0: the torso stays upright. Command bodyikhunchgain.");
            bool c = o.collar;
            if (ImGui::Checkbox("Collar and hood##bodyikc", &c)) cmds += c ? "bodyikcollar on\n" : "bodyikcollar off\n";
            hint("Off: shrinks the collar and hood onto the neck, if they block the view when you look down.");
            float nk[2]{ o.neck_down, o.neck_back };
            if (ImGui::SliderFloat2("Neck below / behind the eyes (m)##bodyikn", nk, 0.0f, 0.3f, "%.3f")) cmds += fmt::format("bodyikneck {:.3f} {:.3f}\n", nk[0], nk[1]);
            float bk = o.back;
            if (ImGui::SliderFloat("Body further back (m)##bodyikb", &bk, -0.05f, 0.25f, "%.3f")) cmds += fmt::format("bodyikback {:.3f}\n", bk);
            hint("Moves the whole body back so you look down on your chest, not into the collar.");
        }
        if (!debug) return;
        ImGui::TextDisabled("Debug");
        if (ImGui::Button("Write two frames' player draws to a file##bodydump")) cmds += "bodydump 2\n";
        if (ImGui::Button("Write 3 s of the seated arms to a file##bodyikseatdump")) cmds += "bodyikseatdump 270\n";
        hint("Writes the seated arm positions of the next frames to a file, for testing. Command bodyikseatdump.");
        hint("Writes the body positions of the next frames to a file, for testing. Command bodydump.");
        if (!o.on) return;
        bool sh = o.snap_hold;
        if (ImGui::Checkbox("Snap turns: the legs hold until the game has turned##bodyiksnaphold", &sh)) cmds += sh ? "bodyiksnaphold on\n" : "bodyiksnaphold off\n";
        hint("During a snap turn the legs stay still until the game has turned, so they don't twist and come back. Command bodyiksnaphold.");
        bool cd = o.carry_drawn;
        if (ImGui::Checkbox("Sleeves drawn before the hands follow the hands as drawn##bodyikcarrydrawn", &cd)) cmds += cd ? "bodyikcarrydrawn on\n" : "bodyikcarrydrawn off\n";
        hint("Keeps coat sleeves in step with the gloves, so they don't shake. Command bodyikcarrydrawn.");
        ImGui::TextWrapped("%s", status().c_str());
        float st = o.stretch;
        if (ImGui::SliderFloat("Most arm lengthening##bodyikst", &st, 1.0f, 1.6f, "%.2f")) cmds += fmt::format("bodyikstretch {:.3f}\n", st);
        hint("How far the arm may stretch to stay on your hand before the hand leaves the wrist.");
        float el[2]{ o.pole_hand, o.elbow_ms };
        if (ImGui::SliderFloat("Elbow follows the hand's turn##bodyike", &el[0], 0.0f, 1.0f, "%.2f")) cmds += fmt::format("bodyikelbow {:.2f} {:.0f}\n", el[0], el[1]);
        float bw[2]{ glm::degrees(o.bow_from), o.bow_gain };
        if (ImGui::SliderFloat("Torso bows when looking down past (deg)##bodyikbw", &bw[0], 0.0f, 90.0f, "%.0f")) cmds += fmt::format("bodyikbow {:.0f} {:.2f}\n", bw[0], bw[1]);
        hint("Looking further down than this, the torso leans with the head. Command bodyikbow.");
        float ln[2]{ o.leash, o.follow_s };
        if (ImGui::SliderFloat("Lean before the hips follow (m)##bodyikln", &ln[0], 0.0f, 0.3f, "%.2f")) cmds += fmt::format("bodyiklean {:.2f} {:.2f}\n", ln[0], ln[1]);
    }
}

namespace
{
    using namespace fc5::bodyik;
    glm::mat4 affine(const glm::mat3& r, const glm::vec3& t)
    {
        glm::mat4 m{ r };
        m[3] = glm::vec4{ t, 1.0f };
        return m;
    }
    const char* const kDumpPath = "fc5vr_bodydump.bin";
}

// bodyikseatdump: one record per seated draw, seat()'s input and output (tools\fullbodyik\bench\seatreplay.py reads it):
// "SEAT", frame, count, result (int32); moved[2] hidden[2] held[2] fin_hands swing follow hands 0 0 (uint8); stretch, pole_hand (float);
// nslots (int32); M (16), shift (3), hand[2] (2 x 16; column-major); game, fin (256 x 12 each, rows); cent (256 x 4); out (256 x 12: the
// drawn palette, the game's when seat() did not move it).
namespace
{
    const char* const kSeatDumpPath = "fc5vr_seatdump.bin";
}
struct SeatDumpFile
{
    FILE*      f{ nullptr };
    int        frame{ -1 };
    int        draws{ 0 };
    std::mutex mtx;
};
struct FarCry5Stereo::BodyIkState
{
    fc5::bodyik::Follow                   follow{};
    fc5::bodyik::Frame                    f{};
    fc5::bodyik::HeadIn                   in{};
    glm::mat4                             ctrl[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };   // each side's controller last frame (world)
    bool                                  ctrl_ok[2]{ false, false };
    glm::mat4                             carry[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    std::chrono::steady_clock::time_point last_t{};
    int                                   frames{ 0 };
    // the last second
    int                                   placed{ 0 }, left{ 0 }, hands{ 0 }, carried{ 0 }, legs{ 0 };
    float                                 reach[2]{}, gap{ 0.0f }, fit{ 0.0f };
    // bodydump
    FILE*                                 dump{ nullptr };
    int                                   dump_frame{ -1 };
    int                                   dump_base_frame{ -1 };
    // bodyikseatdump
    SeatDumpFile                          seat_dump;
    int                                   said{ 0 };
    glm::vec3                             base{ 0.0f };   // the eyes' base from the game camera (world), as applied to this frame's body
    // bodyikveh
    fc5::bodyik::SeatState                seat{};
    std::set<uint64_t>                    veh_said;       // (count, outcome) logged this drive
    int                                   veh_frame{ -1000 };
    std::vector<float>                    veh_pal, veh_game, veh_out;
    int                                   veh_lines{ 0 };
    std::chrono::steady_clock::time_point legs_log_t{};    // bodyikfeet's log
    glm::vec3                             legs_log_pos{ 0.0f };
    float                                 walk_off{ 0.0f };      // bodyikwalkturn: the drawn body's turn from the gestures' body yaw (radians, + left)
    // bodyiksnaphold (FarCry5BodyIk.h)
    float                                 hold_last_yaw{ 0.0f };  // the body yaw the last frame (m_body_yaw: a snap jumps it)
    bool                                  hold_yaw_ok{ false };
    bool                                  hold{ false };          // a snap in flight: the legs' turn and the walk turn hold
    bool                                  hold_eyes{ false };     //   the eyes have turned
    bool                                  hold_sent{ false };     //   it was a synced snap (sent to the game camera first)
    int                                   hold_eframe{ 0 };       //   the frame they turned
    float                                 hold_r0{ 0.0f };        //   the game's heading against the drawn body before the snap (radians)
    float                                 hold_turn{ 0.0f };      //   the snap (radians)
    float                                 hold_max{ 0.0f };       //   the most the game's heading went off hold_r0 (radians)
    std::chrono::steady_clock::time_point hold_t0{}, hold_te{};
    int                                   hold_lines{ 0 };
    float                                 walk_speed{ 0.0f };    //   the game's own speed (m/s, eased), from its legs draw's origin
    glm::vec3                             walk_pos{ 0.0f };
    int                                   walk_frame{ -1000 };
    // bodyikwalkstick's log: each start and stop of the stick, and when the game's eased speed crossed 0.4 m/s after it (the old rule's moment)
    bool                                  ws_stick{ false }, ws_speed{ false }, ws_walking{ false };
    std::chrono::steady_clock::time_point ws_stick_t{};
    float                                 ws_lag_ms{ -1.0f };     // the old rule's delay after this stick edge (ms; -1 not yet)
    int                                   ws_lines{ 0 };
    std::chrono::steady_clock::time_point veh_line_t[2]{};
    std::chrono::steady_clock::time_point worn_log_t{};    // bodyikworn's log
    // bodyikcarrydrawn (FarCry5BodyIk.h): the controllers as drawn (after draw_shift) at the last frame's first arm draw, and the frame taken
    glm::mat4                             dctrl[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    bool                                  dctrl_ok[2]{ false, false };
    int                                   dcarry_frame{ -1000 };
    // its log: the hand each way of carrying would have given (the wrist, world as drawn) against the hand then drawn, by frame parity
    // (Stereo: even the left eye, odd the right), every 5 s
    glm::vec3                             pred_w[2][2]{};         // [side][0 as update_motion read the controllers, 1 as drawn]
    bool                                  pred_ok[2]{ false, false };
    double                                cerr_sum[2][2]{};       // [parity][way] mm
    float                                 cerr_max[2][2]{};
    int                                   cerr_n[2]{};
    std::chrono::steady_clock::time_point cerr_t{};
    float                                 worn_max_down{ -90.0f }, worn_max_curl{ 0.0f }, worn_max_lean{ -90.0f };   // since that log (deg)
};

// Once a frame (update_motion, with m_ref_mtx held and a reference pose): the body from the head, each hand's controller move since the last frame.
void FarCry5Stereo::bodyik_frame(const glm::mat4& head, const glm::vec3& cam, const glm::mat3& body_to_world)
{
    if (!m_bk) m_bk = new BodyIkState{};
    auto&                    st = *m_bk;
    const fc5::bodyik::Opts  o  = fc5::bodyik::opts();
    fc5::bodyik::HeadIn      in{};
    const glm::mat3          body  = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, fc5::gesture::body_yaw(), glm::vec3{ 0.0f, 1.0f, 0.0f }) };
    in.head       = m_ref_inv * head;
    // The simulator's test head (testhead Y P R, testheadpos X Y Z) as the eyes get it (the stereo view: turned about itself, moved in the
    // reference frame), so the body is under the head the picture is taken from. Until 11 Nov the body stood under the untouched head.
    if (m_test_head[0].load() != 0.0f || m_test_head[1].load() != 0.0f || m_test_head[2].load() != 0.0f)
        in.head = in.head * glm::rotate(glm::mat4{ 1.0f }, glm::radians(m_test_head[0].load()), glm::vec3{ 0, 1, 0 }) *
                  glm::rotate(glm::mat4{ 1.0f }, glm::radians(m_test_head[1].load()), glm::vec3{ 1, 0, 0 }) *
                  glm::rotate(glm::mat4{ 1.0f }, glm::radians(m_test_head[2].load()), glm::vec3{ 0, 0, 1 });
    in.head[3] += glm::vec4{ glm::vec3{ m_test_head_pos[0].load(), m_test_head_pos[1].load(), m_test_head_pos[2].load() } / std::max(1e-3f, m_world_scale.load()), 0.0f };
    in.pivot      = glm::vec3{ in.head[3] } + glm::mat3{ in.head } * glm::vec3{ 0.0f, -fc5::gesture::kNeckPivotDown, fc5::gesture::kNeckPivotBack };
    in.pivot_down = fc5::gesture::kNeckPivotDown;
    in.pivot_back = fc5::gesture::kNeckPivotBack;
    in.fwd        = glm::mat3{ m_ref_inv } * (body * glm::vec3{ 0.0f, 0.0f, -1.0f });
    in.to_world   = body_to_world;
    in.origin     = cam;
    in.scale      = m_world_scale.load();
    const auto now = std::chrono::steady_clock::now();
    in.dt          = st.last_t.time_since_epoch().count() ? std::clamp(std::chrono::duration<float>(now - st.last_t).count(), 0.0f, 0.1f) : 0.011f;
    st.last_t      = now;
    // bodyiksnaphold (FarCry5BodyIk.h; author 14 Nov 21:58Z): from a snap's send to the game camera
    // (snapsync: the eyes turn some frames later) or the eyes' turn (without it: the game camera follows some updates later) until the game's heading
    // is back where it was against the drawn body, the legs' turn and the walk turn hold still: they no longer chase the snap's angle for those frames
    // and ease back after it.
    {
        const float jump = st.hold_yaw_ok ? std::remainder(m_body_yaw - st.hold_last_yaw, glm::two_pi<float>()) : 0.0f;
        st.hold_last_yaw = m_body_yaw;
        st.hold_yaw_ok   = true;
        const bool  sent = m_snap_sync_pend != 0.0f;
        // the eyes turned by a snap this frame: a synced snap is no longer in flight, or the body yaw jumped with the stick pushed past the snap since
        // it was last let go (a smooth turn moves a few degrees a pair)
        const bool  eyes  = (st.hold && st.hold_sent && !sent && !st.hold_eyes) || (m_snap_turn.load() && !m_snap_armed && std::abs(jump) > glm::radians(5.0f));
        const bool  veh   = in_vehicle() || m_in_vehicle.load();
        const auto& f     = st.f;
        const int   frame = m_frame.load();
        if (o.on && o.snap_hold && (sent || eyes) && !veh) {
            if (!st.hold) {
                st.hold      = true;
                st.hold_eyes = false;
                st.hold_r0   = f.root_ang;   // the last legs draw's, before the snap
                st.hold_turn = sent ? m_snap_sync_pend : jump;
                st.hold_t0   = now;
                st.hold_max  = 0.0f;
                st.hold_sent = false;
            }
            if (sent) st.hold_sent = true;
            if (eyes) {
                st.hold_eyes   = true;
                st.hold_te     = now;
                st.hold_eframe = frame;
            }
        }
        if (st.hold) {
            const float off       = std::remainder(f.root_ang - st.hold_r0, glm::two_pi<float>());
            const bool  legs_new  = f.feet_frame >= frame - 2;   // the legs were drawn lately (else root_ang is old)
            if (legs_new) st.hold_max = std::max(st.hold_max, std::abs(off));
            const float held      = std::chrono::duration<float>(now - st.hold_t0).count();
            const bool  after     = st.hold_eyes && !sent && frame - st.hold_eframe >= 2;
            const bool  back      = after && legs_new && f.feet_frame > st.hold_eframe && std::abs(off) < glm::radians(o.snap_hold_deg);
            const char* why       = !o.on || !o.snap_hold ? "switched off" : veh ? "a vehicle" : held > o.snap_hold_s ? "the most it holds" : after && !legs_new ? "the legs not drawn" : nullptr;
            if (back || why) {
                st.hold = false;
                if (st.hold_lines < 60 || st.hold_lines % 25 == 0)
                    spdlog::info("[BodyIk] snap hold: a {:+.0f} deg snap; the legs and the walk turn held {:.0f} ms, {:.0f} ms after the eyes turned; the game's heading went "
                                 "up to {:.1f} deg off where it was against the drawn body, {}",
                                 glm::degrees(st.hold_turn), held * 1000.0f, st.hold_eyes ? std::chrono::duration<float>(now - st.hold_te).count() * 1000.0f : -1.0f,
                                 glm::degrees(st.hold_max), back ? fmt::format("back within {:.1f} deg", glm::degrees(off)) : fmt::format("let go at {:.1f} deg ({})", glm::degrees(off), why));
                st.hold_lines++;
            }
        }
        st.f.turn_hold = st.hold;
    }
    // bodyikwalkturn (author 13 Nov, walking forward to the left): the game's own speed
    // from its legs draw's origin, and while it walks the drawn body eased (0.3 s) onto the game's heading (the last frame's legs say how far
    // off it the drawn body is). Standing, or the legs not drawn, it eases back onto the gestures' body yaw.
    {
        const auto& f     = st.f;
        const bool  fresh = f.feet_frame >= 0 && f.feet_frame >= m_frame.load() - 2;
        if (fresh && f.feet_frame != st.walk_frame) {
            if (f.feet_frame - st.walk_frame <= 3) {
                const float v = glm::length(f.root_pos - st.walk_pos) / std::max(1e-3f, in.scale) / std::max(1e-3f, in.dt * float(f.feet_frame - st.walk_frame));
                if (std::isfinite(v) && v < 30.0f) st.walk_speed += (v - st.walk_speed) * (1.0f - std::exp(-in.dt / 0.2f));
            }
            st.walk_pos   = f.root_pos;
            st.walk_frame = f.feet_frame;
        }
        if (!fresh) st.walk_speed = 0.0f;
        // bodyikwalkstick: walking from the player's own stick, at once (room scale's push, with the stick let go, by the speed as before).
        const bool stick   = m_motion.load() && !m_in_vehicle.load() && m_lstick_mag.load() > o.stick_on;
        const bool speed   = st.walk_speed > 0.4f;
        const bool walking = o.walk_stick ? (stick || (roomscale_following() && speed)) : speed;
        if (stick != st.ws_stick) {   // the log: a start or a stop of the stick; the line comes once the old rule would have followed (or 1.5 s)
            if (st.ws_stick_t.time_since_epoch().count() && st.ws_lag_ms < 0.0f && (st.ws_lines < 40 || st.ws_lines % 20 == 0))
                spdlog::info("[BodyIk] walk {}: the game's eased speed did not cross 0.4 m/s before the stick changed again", st.ws_stick ? "start" : "stop");
            if (st.ws_stick_t.time_since_epoch().count() && st.ws_lag_ms < 0.0f) st.ws_lines++;
            st.ws_stick   = stick;
            st.ws_stick_t = now;
            st.ws_lag_ms  = -1.0f;
        }
        if (speed != st.ws_speed) {
            st.ws_speed = speed;
            if (st.ws_stick_t.time_since_epoch().count() && st.ws_lag_ms < 0.0f && speed == st.ws_stick) {
                st.ws_lag_ms = std::chrono::duration<float>(now - st.ws_stick_t).count() * 1000.0f;
                if (st.ws_lines < 40 || st.ws_lines % 20 == 0)
                    spdlog::info("[BodyIk] walk {}: the stick {} {:.0f} ms before the game's eased speed crossed 0.4 m/s (the body {} with the {}; walk turn now {:+.1f} deg)",
                                 stick ? "start" : "stop", stick ? "pushed" : "let go", st.ws_lag_ms, o.walk_stick ? "followed" : "waited", o.walk_stick ? "stick" : "speed",
                                 glm::degrees(st.walk_off));
                st.ws_lines++;
            }
        }
        st.ws_walking = walking;
        const float want = st.hold ? st.walk_off   // bodyiksnaphold: holds through a snap
                         : o.on && o.walk_turn && fresh && walking ? std::clamp(st.walk_off + f.root_ang, -0.6f, 0.6f) : 0.0f;
        st.walk_off += (want - st.walk_off) * (1.0f - std::exp(-in.dt / std::max(0.02f, o.walk_tau)));   // bodyikturnease (0.3 s until 14 Nov)
        if (!std::isfinite(st.walk_off)) st.walk_off = 0.0f;
        const glm::vec3 up_r = glm::mat3{ m_ref_inv } * glm::vec3{ 0.0f, 1.0f, 0.0f };
        in.fwd               = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, st.walk_off, up_r) } * in.fwd;
    }
    st.in          = in;
    if (fc5::bodyik::snap_log_frames() > 0) {   // bodyiksnaplog N (test)
        fc5::bodyik::snap_log_done();
        const auto& f = st.f;
        spdlog::info("[BodyIk] snaplog frame {} body yaw {:.1f} snap in flight {:+.1f} hold {} | game heading {:+.1f} (before the snap {:+.1f}) | legs turn {:+.1f} asked {:+.1f} "
                     "(legs frame {}) | walk turn {:+.1f} game speed {:.2f}",
                     m_frame.load(), glm::degrees(m_body_yaw), glm::degrees(m_snap_sync_pend), st.hold ? 1 : 0, glm::degrees(f.root_ang), glm::degrees(st.hold_r0),
                     glm::degrees(f.feet), glm::degrees(f.feet_raw), f.feet_frame, glm::degrees(st.walk_off), st.walk_speed);
    }
    // The model's right hand is on the weapon's controller, its left on the other one.
    const glm::mat4 cnow[2]{ affine(m_mo.ctrl_rot_world, m_mo.ctrl_pos_world), affine(m_mo.other_rot_world, m_mo.other_pos_world) };
    const bool      cok[2]{ m_mo.ctrl_valid, m_mo.other_valid };
    for (int s = 0; s < 2; ++s) {
        glm::mat4 c{ 1.0f };
        if (cok[s] && st.ctrl_ok[s]) {
            c = cnow[s] * glm::inverse(st.ctrl[s]);
            if (glm::length(glm::vec3{ cnow[s][3] } - glm::vec3{ st.ctrl[s][3] }) > 0.5f) c = glm::mat4{ 1.0f };   // a teleport, a turn: no carry
        }
        st.carry[s]   = c;
        st.ctrl[s]    = cnow[s];
        st.ctrl_ok[s] = cok[s];
    }
    // bodyikcarrydrawn: the hands are carried at the frame's first arm draw instead (bodyik_draw), from the controllers as drawn.
    const glm::mat4 kId{ 1.0f };
    fc5::bodyik::begin_frame(st.f, m_frame.load(), in.dt, fc5::bodyik::body_pose(o, in, st.follow), o.carry_drawn ? kId : st.carry[0],
                             o.carry_drawn ? kId : st.carry[1]);
    st.base = glm::vec3{ 0.0f };   // at the game camera until the view says where the eyes stand (bodyik_base)
    // bodyikworn (FarCry5Gesture.h set_carry_anchor, FarCry5BodyIk.h worn_place; author 14 Nov 15:07Z): each carried thing's place on this frame's drawn body (world,
    // before the eyes' base: the carried things get that shift in gun_anchor_apply as the body does in bodyik_base), in tracking space, given
    // as a move from the eyes on the body's heading (so a recentre or a snap turn between this frame and the gestures' next poll moves it with
    // the head, as it moves the body). Replaces bodyikcarry (placed from the eyes, moved by the hips' lag only; 12-13 Nov).
    {
        const auto& b  = st.f.body;
        const bool  on = o.worn && b.ok && !in_vehicle() && !m_in_vehicle.load();
        glm::mat4   rel[fc5::gesture::kCarryParts]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
        if (on) {
            const glm::mat3 Rt  = glm::transpose(body_to_world);
            const glm::mat4 ref = glm::inverse(m_ref_inv);
            const float     sc  = std::max(1e-3f, in.scale);
            auto to_trk = [&](const glm::mat4& w) {
                glm::mat4 r{ Rt * glm::mat3{ w } };
                r[3] = glm::vec4{ Rt * (glm::vec3{ w[3] } - cam) / sc, 1.0f };
                return ref * r;
            };
            const glm::mat4 E  = glm::translate(glm::mat4{ 1.0f }, glm::vec3{ head[3] }) * glm::rotate(glm::mat4{ 1.0f }, fc5::gesture::body_yaw(), glm::vec3{ 0.0f, 1.0f, 0.0f });
            const glm::mat4 Ei = glm::inverse(E);
            const bool      mirror = m_left_handed.load();
            static_assert(int(fc5::gesture::kCarrySling) == int(fc5::bodyik::kWornSling) && int(fc5::gesture::kCarryHolster) == int(fc5::bodyik::kWornHolster) &&
                          int(fc5::gesture::kCarryPouch) == int(fc5::bodyik::kWornPouch) && int(fc5::gesture::kCarryMag) == int(fc5::bodyik::kWornMag));
            bool ok = true;
            for (int i = 0; i < fc5::gesture::kCarryParts; ++i) {
                rel[i] = Ei * to_trk(fc5::bodyik::worn_place(b, o, i, mirror));
                for (int c = 0; c < 4; ++c) ok = ok && std::isfinite(rel[i][c].x + rel[i][c].y + rel[i][c].z + rel[i][c].w);
            }
            fc5::gesture::set_carry_anchor(ok, rel);
            // Once a minute while worn: where each one is from the eyes (m: right, up, back on the body's heading), for the headset report; and the
            // most the head looked down, the back curled and the torso leaned forward since the last line (author 16:49Z; the lean: hips to neck from upright, + forward, without the curl; the upper back is the lean + the curl).
            const auto      t    = std::chrono::steady_clock::now();
            const float     down = glm::degrees(std::asin(std::clamp(-(glm::mat3{ in.head } * glm::vec3{ 0.0f, 0.0f, -1.0f }).y, -1.0f, 1.0f)));
            const glm::vec3 ax   = b.neck - b.hips;
            const float     lean = glm::length(ax) > 1e-4f ? glm::degrees(std::asin(std::clamp(glm::dot(glm::normalize(ax), b.fwd), -1.0f, 1.0f))) : 0.0f;
            if (std::isfinite(down)) st.worn_max_down = std::max(st.worn_max_down, down);
            if (std::isfinite(lean)) st.worn_max_lean = std::max(st.worn_max_lean, lean);
            st.worn_max_curl = std::max(st.worn_max_curl, glm::degrees(b.hunch_up));
            if (ok && t - st.worn_log_t > std::chrono::seconds(60)) {
                st.worn_log_t = t;
                std::string l;
                for (int i = 0; i < fc5::gesture::kCarryParts; ++i)
                    l += fmt::format("{}{} ({:+.2f}, {:+.2f}, {:+.2f})", i ? ", " : "", fc5::bodyik::worn_name(i), rel[i][3].x, rel[i][3].y, rel[i][3].z);
                spdlog::info("[BodyIk] worn: {} m from the eyes (right, up, back on the body's heading); looking down {:.0f} deg, the back curled {:.0f} deg, "
                             "the torso leaning {:+.0f} deg; the last minute at most: looking down {:.0f}, curled {:.0f}, leaning {:+.0f} (curl gain {:.2f})",
                             l, down, glm::degrees(b.hunch_up), lean, st.worn_max_down, st.worn_max_curl, st.worn_max_lean, o.hunch_gain);
                st.worn_max_down = -90.0f;
                st.worn_max_curl = 0.0f;
                st.worn_max_lean = -90.0f;
            }
        } else fc5::gesture::set_carry_anchor(false, rel);
    }
    // bodyikfeet's log (author 13 Nov: the legs, the feet and the steps to the left walking forward): once a second while the legs are drawn,
    // the angles the run is read for (+ left of the drawn body's heading) and how fast the game's own body moves (its legs draw's origin).
    {
        const auto& f   = st.f;
        const float sec = std::chrono::duration<float>(now - st.legs_log_t).count();
        if (o.legs && f.feet_frame >= f.frame - 2 && sec >= 1.0f) {
            glm::vec3   hf = glm::mat3{ in.head } * glm::vec3{ 0.0f, 0.0f, -1.0f }, bf = in.fwd;
            hf.y = bf.y = 0.0f;
            const float hy = glm::length(hf) > 1e-4f && glm::length(bf) > 1e-4f ? std::atan2(glm::cross(bf, hf).y, glm::dot(bf, hf)) : 0.0f;
            const float sp = st.legs_log_t.time_since_epoch().count() && sec < 3.0f ? glm::length(f.root_pos - st.legs_log_pos) / std::max(1e-3f, in.scale) / sec : 0.0f;
            spdlog::info("[BodyIk] legs: the game's heading {:+.1f} deg, its hip line squared by {:+.1f}, the feet so squared {:+.1f} (from the game's heading "
                         "{:+.1f}){}; turn {:+.1f} (asked {:+.1f}); the game moving {:.2f} m/s; the head {:+.1f} from the body; walk turn {:+.1f} (+ left of the drawn body)",
                         glm::degrees(f.root_ang), glm::degrees(f.hip_ang), glm::degrees(f.feet_body), glm::degrees(f.feet_body - f.root_ang),
                         f.feet_read ? "" : " (feet not read)", glm::degrees(f.feet), glm::degrees(f.feet_raw), sp,
                         glm::degrees(hy), glm::degrees(st.walk_off));
            st.legs_log_t   = now;
            st.legs_log_pos = f.root_pos;
        }
    }
    if (++st.frames >= 90) {
        fc5::bodyik::note_status(st.placed, st.left, st.hands, st.carried, st.legs, st.reach[0], st.reach[1], st.gap, st.fit, st.f.cal);
        st.placed = st.left = st.hands = st.carried = st.legs = 0;
        st.gap = st.fit = 0.0f;
        st.frames = 0;
    }
    // bodydump: one record per frame, then one per draw (bodyik_draw).
    if (fc5::bodyik::dump_frames() > 0) {
        if (!st.dump) {
            fopen_s(&st.dump, kDumpPath, "wb");
            if (st.dump) std::fwrite("BIK1", 1, 4, st.dump);
        } else fc5::bodyik::dump_frame_done();
        if (st.dump && fc5::bodyik::dump_frames() > 0) {
            const int32_t frame = m_frame.load();
            std::fwrite("FRAM", 1, 4, st.dump);
            std::fwrite(&frame, 4, 1, st.dump);
            std::fwrite(&in.head, 4, 16, st.dump);
            std::fwrite(&in.pivot, 4, 3, st.dump);
            std::fwrite(&in.fwd, 4, 3, st.dump);
            std::fwrite(&in.to_world, 4, 9, st.dump);
            std::fwrite(&in.origin, 4, 3, st.dump);
            const float fl[4]{ in.scale, in.dt, in.pivot_down, in.pivot_back };
            std::fwrite(fl, 4, 4, st.dump);
            std::fwrite(&st.carry[0], 4, 16, st.dump);
            std::fwrite(&st.carry[1], 4, 16, st.dump);
            st.dump_frame = frame;
        }
    }
    if (st.dump && fc5::bodyik::dump_frames() <= 0) {
        std::fclose(st.dump);
        st.dump = nullptr;
        spdlog::info("[BodyIk] bodydump: {} written", kDumpPath);
    }
}

// bodyikcalibrate (author 13 Nov 21:42Z): once a frame from update_motion (m_ref_mtx held, a reference pose), body IK on or off. A buzz on both
// controllers when the measure starts and when it ends; meanwhile one sample a frame (the head and both controllers in the reference space,
// and with the body drawn the solve's straight-arm reach); at the end the measures are applied and saved (bodyikbody).
void FarCry5Stereo::bodyik_cal_tick()
{
    static auto vr = VR::get();
    if (fc5::bodyik::cal_state() != 2) return;
    // The controllers as update_motion has them: the headset's, or with the flat test on (testctrl) the simulated ones, ctrl X Y Z and lctrl X Y Z
    // (reference space, m). The simulator's own controllers are not where the test puts the hands (14 Nov run: every sample 'less than 1 m apart').
    const bool test = m_test_ctrl.load();
    if (fc5::bodyik::cal_state() == 2 && vr->is_hmd_active()) {
        auto pose = [&](int idx, glm::mat4& out) {
            if (idx < 0) return false;
            const glm::mat4 m = vr->get_transform(idx);
            if (!std::isfinite(m[3][0]) || (m[0][0] == 0.0f && m[1][1] == 0.0f && m[2][2] == 0.0f)) return false;
            out = m_ref_inv * m;
            return true;
        };
        fc5::bodyik::CalSample smp{};
        bool                   got = pose(0, smp.head);
        if (got) {
            // testheadpos moves the head as bodyik_frame has it (the eyes' move, world units over the scale).
            smp.head[3] += glm::vec4{ glm::vec3{ m_test_head_pos[0].load(), m_test_head_pos[1].load(), m_test_head_pos[2].load() } / std::max(1e-3f, m_world_scale.load()), 0.0f };
            if (test) {
                // The weapon hand is the right one unless left-handed; the measure does not care which is which.
                got = m_dbg_other_set.load();
                smp.c[0] = glm::vec3{ m_dbg_ctrl[0].load(), m_dbg_ctrl[1].load(), m_dbg_ctrl[2].load() };
                smp.c[1] = glm::vec3{ m_dbg_other[0].load(), m_dbg_other[1].load(), m_dbg_other[2].load() };
            } else {
                glm::mat4 l{ 1.0f }, r{ 1.0f };
                const int li = vr->get_left_controller_index(), ri = vr->get_right_controller_index();
                got          = li > 0 && ri > 0 && pose(li, l) && pose(ri, r);
                smp.c[0]     = glm::vec3{ r[3] };
                smp.c[1]     = glm::vec3{ l[3] };
            }
        }
        if (got) {
            smp.scale = m_world_scale.load();
            // With the body drawn, the last frame's solve of each arm (its drawn shoulder to the drawn wrist over the model's arm).
            if (m_bk && fc5::bodyik::on())
                for (int s = 0; s < 2; ++s) {
                    const auto& sd = m_bk->f.side[s];
                    if (sd.arm.ok && sd.arm_exact && sd.arm_frame >= m_frame.load() - 2) smp.reach[s] = sd.arm.reach;
                }
            fc5::bodyik::cal_add(smp);
        }
    }
}

// bodyikcalibrate's clock, the buzzes and the result; bodyikintro's welcome (once per present, poll_commands' path: it runs with the F11 menu
// open, in menus and paused too, so a calibration always ends; until 14 Nov it ended only in update_motion and could stay at "0 s").
void FarCry5Stereo::bodyik_cal_poll(bool play, bool menu_open)
{
    static auto vr = VR::get();
    auto buzz = [&] {
        vr->trigger_haptic_vibration(0.0f, 0.08f, 1.0f, 0.5f, vr->get_left_joystick());
        vr->trigger_haptic_vibration(0.0f, 0.08f, 1.0f, 0.5f, vr->get_right_joystick());
    };
    if (fc5::bodyik::cal_state() != 0) {
        const bool ended = fc5::bodyik::cal_tick();
        if (fc5::bodyik::cal_take_start()) {
            buzz();
            spdlog::info("[BodyIk] calibration: measuring for {:.0f} s, the controllers {}", fc5::bodyik::kCalSample,
                         m_test_ctrl.load() ? (m_dbg_other_set.load() ? "of the flat test (ctrl and lctrl)" : "of the flat test, but no lctrl set: nothing to measure")
                                            : "of the headset");
        }
        if (ended) {
            buzz();
            fc5::bodyik::CalResult res{};
            const std::string      line = fc5::bodyik::cal_finish(&res);
            if (!line.empty()) {
                m_persist["bodyikbody"] = line;
                save_settings();
            }
        }
    }
    // The welcome: once, after the first seconds of play (the caller's test: on foot with motion controls, no menu, flat screen, cinematic
    // or loading popup, not a scripted test run); F11 opened closes it.
    fc5::bodyik::intro_play(play && !menu_open);
    if (menu_open) fc5::bodyik::intro_dismiss();
    if (fc5::bodyik::intro_take_shown()) {
        m_persist["bodyikintro"] = "bodyikintro off";
        save_settings();
    }
}

// The eyes' base from the game camera (gun_anchor_apply, each view set-up of the frame). The eyes stand at the base and the hands are drawn
// from it (draw_shift); the body goes there too. With a long gun the game camera leans out over the weapon: 12 cm ahead of the base in the
// 11 Nov run (MBP .50), which put the chest that far in front of the eyes and the weapon hand inside it.
void FarCry5Stereo::bodyik_base(const glm::vec3& shift)
{
    if (!m_bk || !(glm::length(shift) < 5.0f * std::max(1.0f, m_world_scale.load()))) return;
    auto&           st = *m_bk;
    const glm::vec3 d  = shift - st.base;
    if (d == glm::vec3{ 0.0f }) return;
    st.f.body.neck += d;
    st.f.body.hips += d;
    st.f.body.eye += d;
    st.base = shift;
}

// bodyikcarrydrawn (FarCry5BodyIk.h): at the frame's first arm draw (the weapon pass: gun_anchor_apply has moved the controllers onto the eyes'
// base by draw_shift, as the hand map that draws the hands has them), each controller's move since the last frame's first arm draw. With the
// switch on it carries the hands not drawn yet this frame (begin_frame was given identities); either way both carried wrists are kept for the log.
void FarCry5Stereo::bodyik_carry_drawn(int frame, bool use)
{
    auto& st        = *m_bk;
    st.dcarry_frame = frame;
    // The weapon hand: its controller where this pass's hand map put it (the hand map is made at the weapon pass's upload, and an upload after
    // it in the same frame could move ctrl_pos_world again); the other hand floats on other_pos_world.
    const glm::vec3 wpos = m_hand_map.frame == frame ? m_hand_map.w_pos : m_mo.ctrl_pos_world;
    const glm::mat4 cnow[2]{ affine(m_mo.ctrl_rot_world, wpos), affine(m_mo.other_rot_world, m_mo.other_pos_world) };
    const bool      cok[2]{ m_mo.ctrl_valid, m_mo.other_valid };
    glm::mat4       c[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    const float     sc = std::max(1e-3f, m_world_scale.load());
    for (int s = 0; s < 2; ++s) {
        if (cok[s] && st.dctrl_ok[s]) {
            c[s] = cnow[s] * glm::inverse(st.dctrl[s]);
            if (glm::length(glm::vec3{ cnow[s][3] } - glm::vec3{ st.dctrl[s][3] }) > 0.5f * sc) c[s] = glm::mat4{ 1.0f };   // a teleport, a turn: no carry
        }
        st.dctrl[s]    = cnow[s];
        st.dctrl_ok[s] = cok[s];
        // The hand before this frame's carry: with the switch off begin_frame has carried it already (by st.carry).
        const auto&     sd    = st.f.side[s];
        const bool      drawn = sd.exact && sd.hand_frame == st.f.frame;
        const glm::mat4 prev  = use ? sd.hand : glm::inverse(st.carry[s]) * sd.hand;
        const glm::vec4 wr{ fc5::bodyik::rest_joint(fc5::bodyik::kJWrist, s), 1.0f };
        st.pred_w[s][0] = glm::vec3{ st.carry[s] * prev * wr };
        st.pred_w[s][1] = glm::vec3{ c[s] * prev * wr };
        st.pred_ok[s]   = !drawn && sd.hand_frame > -1000 && std::isfinite(st.pred_w[s][0].x + st.pred_w[s][1].x);
    }
    if (use) fc5::bodyik::carry_hands(st.f, c[0], c[1]);
}

bool FarCry5Stereo::bodyik_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, int how)
{
    const bool ik_on   = fc5::bodyik::on();
    const bool dumping = m_bk && m_bk->dump != nullptr;
    if (!ik_on && !dumping) return false;
    if (!m_bk || count == 0) return false;   // no frame yet; an indirect draw whose counts were not read
    auto&      st    = *m_bk;
    const int  frame = m_frame.load();
    const bool world = how == 3;
    m_keep_bones     = true;   // the palettes' CPU copies (note_object_constants)
    auto leave = [&](const char* why) {
        st.left++;
        if (st.said < 40) {
            st.said++;
            spdlog::info("[BodyIk] {} draw of {} indices left as it was: {}", world ? "world" : how == 0 ? "hand" : "hidden", count, why);
        }
        return false;
    };
    // drivehands places the arms there. vehgunarm (23 Nov): driving with a weapon out, the weapon pass's hand on the gun (the game's one bare arm
    // or glove) gets its forearm and upper arm from the body under the headset, as on foot (the author's 08:38 quad drive: the gun hand had no arm).
    if (in_vehicle() && !(how == 0 && m_veh_gun_arm.load() && game_gun_out())) return false;
    const auto& hm = active_hand_map();   // (mountedhands: on a mount the mounted pass's map, the identity)
    if (!world) {
        if (!m_vs_cb0 || !is_weapon_cb(m_vs_cb0)) return false;
        if (frame - hm.frame > 1) return leave("the weapon pass is not placed on the controller (a mounted weapon with mountedhands off, or no controller)");
        if (m_hand_cb_swapped && !m_hand_bones_swapped) return leave("another move's own constants are bound");
    } else if (m_hand_cb_swapped) return leave("a stand-in palette is already bound");

    // The game's palette (the original buffer when the hands' stand-in is bound), the palette as it would be drawn now, the object transform
    // and the vertex positions' scale.
    ID3D11Buffer* cb[4]{};
    ctx->VSGetConstantBuffers(0, 4, cb);
    struct Rel { ID3D11Buffer** b; ~Rel() { for (int i = 0; i < 4; ++i) if (b[i]) b[i]->Release(); } } rel{ cb };
    ID3D11Buffer*      pal_buf = m_hand_bones_swapped ? m_hand_cb_orig[0] : cb[3];
    std::vector<float> orig, fin;
    float              obj[12]{};
    bool               have_obj = false;
    float              scale    = 1.13f / 32767.0f;
    {
        std::lock_guard _{ m_obj_mtx };
        if (pal_buf) if (auto it = m_bone_data.find(pal_buf); it != m_bone_data.end()) orig = it->second;
        if (cb[1]) if (auto it = m_obj_consts.find(cb[1]); it != m_obj_consts.end() && it->second.size >= 48) { std::memcpy(obj, it->second.f, sizeof(obj)); have_obj = true; }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
    }
    if (m_hand_bones_swapped) {
        if (m_bm_pal_frame == frame && m_bm_pal.size() >= 3072) fin = m_bm_pal;
    } else fin = orig;
    if (orig.size() < 3072 || fin.size() < 3072 || !have_obj) return leave("its palette or object transform is not read yet");

    // The mesh: each slot's weight and rest-pose centre (the hands' scan, once per mesh). On a deferred context only a mesh already scanned.
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ifmt{};
    UINT          ioff = 0;
    ctx->IAGetIndexBuffer(&ib, &ifmt, &ioff);
    if (ib) ib->Release();
    const GloveMesh* gm = nullptr;
    if (fc5::rec::is_deferred(ctx)) {
        if (auto it = m_glove_meshes.find(std::make_tuple(static_cast<void*>(ib), count, start, base)); it != m_glove_meshes.end()) gm = &it->second;
    } else gm = glove_mesh(ctx, count, start, base);
    if (!gm || !gm->ok || gm->cent.size() < 256) return leave("its vertices are not read (no bones, or not read yet)");
    // Each slot's weight and rest centre with the weights on the bones the shader gives them (GloveMesh::rc).
    const std::vector<glm::vec4>& rc = gm->rc.size() >= 256 ? gm->rc : gm->cent;
    glm::vec4 cent[256];
    int       nslots = 0;
    for (int s = 0; s < 256; ++s) {
        const glm::vec4& c = rc[s];
        cent[s]            = glm::vec4{ c.x, c.y * scale, c.z * scale, c.w * scale };
        if (c.x > 0.0f) nslots = s + 1;
    }

    // Object space -> the world as drawn: the object transform, then in the weapon pass its placement on the controller (as the hands,
    // bonemap and reloadmagmodel: drawn = w_pos + w_rot (rot (x - cam) - grip)).
    const glm::mat4 Om = fc5::bodyik::from_rows(obj);
    glm::mat4       w2d{ 1.0f };
    if (!world) {
        const glm::mat3 L = hm.w_rot * hm.rot;
        w2d               = affine(L, hm.w_pos - hm.w_rot * hm.grip - L * hm.cam);
    }
    fc5::bodyik::DrawIn in{};
    in.pass   = how;
    in.mesh   = (uint64_t(reinterpret_cast<uintptr_t>(ib)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(count) << 32) ^ (uint64_t(start) << 8) ^ uint64_t(uint32_t(base));
    if (in.mesh == 0) in.mesh = 1;
    in.game   = orig.data();
    in.fin    = fin.data();
    in.nslots = nslots;
    in.cent   = cent;
    in.M      = w2d * Om;
    if (!world) {
        // armfine (FarCry5Stereo.h): the same product in doubles (w2d's own place is a difference of map-size numbers, and so is the
        // product's). Taken with the switch on; the distance between the two is noted either way (gunstillstat).
        const glm::dmat3 Ld{ hm.w_rot * hm.rot };
        glm::dmat4       wd{ Ld };
        wd[3] = glm::dvec4{ glm::dvec3{ hm.w_pos } - glm::dvec3{ hm.w_rot * hm.grip } - Ld * glm::dvec3{ hm.cam }, 1.0 };
        const glm::mat4 Mf{ wd * glm::dmat4{ Om } };
        const float     mm = 1000.0f * glm::length(glm::vec3{ Mf[3] } - glm::vec3{ in.M[3] });
        s9_note(2, mm);
        if (m_arm_fine.load() && mm < 20.0f) in.M = Mf;
    }
    if (how == 0 && fc5::bodyik::hand_roles_on() && gm->hr_any() && gm->hr_fold.size() >= 256) {
        in.folded = gm->hr_fold.data();   // handroles folded these onto the wrist (bare_hand_fold)
    } else if (how == 0 && gm->arm_side >= 0) {
        in.fold_side = gm->arm_side;
        in.fold_from = gm->hand_from * scale;
    }
    std::vector<float>    out(3072);
    fc5::bodyik::DrawInfo di{};
    const fc5::bodyik::Opts o = fc5::bodyik::opts();
    if (!world && st.dcarry_frame != frame) bodyik_carry_drawn(frame, o.carry_drawn);   // bodyikcarrydrawn: before the frame's first arm
    bool was_exact[2]{};
    for (int s = 0; s < 2; ++s) was_exact[s] = st.f.side[s].exact && st.f.side[s].hand_frame == st.f.frame;
    const int               r = fc5::bodyik::draw(st.f, o, in, out.data(), &di);
    // bodyikcarrydrawn's log: at the frame's hand draw, how far each way of carrying put the wrist from the hand drawn.
    for (int s = 0; s < 2; ++s) {
        if (!di.solved[s] || was_exact[s] || !st.pred_ok[s]) continue;
        st.pred_ok[s]      = false;
        const glm::vec3 w  = glm::vec3{ st.f.side[s].hand * glm::vec4{ fc5::bodyik::rest_joint(fc5::bodyik::kJWrist, s), 1.0f } };
        const int       p  = frame & 1;
        const float     sc = std::max(1e-3f, m_world_scale.load());
        for (int k = 0; k < 2; ++k) {
            const float mm = 1000.0f * glm::length(w - st.pred_w[s][k]) / sc;
            if (!std::isfinite(mm)) continue;
            st.cerr_sum[p][k] += mm;
            st.cerr_max[p][k] = std::max(st.cerr_max[p][k], mm);
        }
        st.cerr_n[p]++;
    }
    if (const auto now = std::chrono::steady_clock::now(); now - st.cerr_t > std::chrono::seconds(5)) {
        if (st.cerr_n[0] + st.cerr_n[1] > 0 && st.cerr_t.time_since_epoch().count()) {
            auto part = [&](int p) {
                const int n = std::max(1, st.cerr_n[p]);
                return fmt::format("{} {}: as read {:.1f} mm (max {:.1f}), as drawn {:.1f} mm (max {:.1f})", p ? "odd frames (Stereo: right eye)" : "even frames (Stereo: left eye)",
                                   st.cerr_n[p], st.cerr_sum[p][0] / n, st.cerr_max[p][0], st.cerr_sum[p][1] / n, st.cerr_max[p][1]);
            };
            spdlog::info("[BodyIk] carry, last 5 s: the wrist carried to the draws before the hand draw, from the hand then drawn: {}; {}; bodyikcarrydrawn {}",
                         part(0), part(1), o.carry_drawn ? "on (as drawn used)" : "off (as read used)");
        }
        st.cerr_t = now;
        for (int p = 0; p < 2; ++p) {
            st.cerr_n[p] = 0;
            for (int k = 0; k < 2; ++k) st.cerr_sum[p][k] = st.cerr_max[p][k] = 0.0f;
        }
    }

    if (st.dump && st.dump_frame == frame) {
        if (st.dump_base_frame != frame) {   // once per frame, before its draws: where the eyes' base is from the game camera
            st.dump_base_frame = frame;
            std::fwrite("BASE", 1, 4, st.dump);
            std::fwrite(&st.base, 4, 3, st.dump);
        }
        const int32_t hd[8]{ how, int32_t(count), int32_t(start), base, nslots, in.folded ? -2 : in.fold_side, r, m_hand_bones_swapped ? 1 : 0 };   // fold side -2: handroles (the folded slots have a zero matrix in fin)
        std::fwrite("DRAW", 1, 4, st.dump);
        std::fwrite(hd, 4, 8, st.dump);
        std::fwrite(&in.mesh, 8, 1, st.dump);
        const float fl[2]{ scale, in.fold_from };
        std::fwrite(fl, 4, 2, st.dump);
        std::fwrite(&Om, 4, 16, st.dump);
        std::fwrite(&w2d, 4, 16, st.dump);
        std::fwrite(orig.data(), 4, 3072, st.dump);
        std::fwrite(fin.data(), 4, 3072, st.dump);
        std::fwrite(cent, 4, 1024, st.dump);
        std::fwrite(out.data(), 4, 3072, st.dump);
    }
    for (int s = 0; s < 2; ++s) {
        if (di.solved[s]) st.hands++;
        if (di.predicted[s]) st.carried++;
        if (di.wrist_fit[s] >= 0.0f) st.fit = std::max(st.fit, di.wrist_fit[s]);
        const auto& a = st.f.side[s].arm;
        if (a.ok && st.f.side[s].arm_frame == st.f.frame) {
            st.reach[s] = a.reach;
            st.gap      = std::max(st.gap, a.gap);
        }
    }
    if (!ik_on) return false;
    if (r != fc5::bodyik::kMoved) {
        if (r == fc5::bodyik::kNothing && how == 0) return false;   // hands only: as the mod drew them
        return leave(fc5::bodyik::result_name(r));
    }
    const bool bound = world ? hands_cb_swap(ctx, out) : hands_cb_update(ctx, out);
    if (!bound) return leave("the stand-in palette could not be bound");
    st.placed++;
    if (world) st.legs++;
    return true;
}

static void seat_dump_write(SeatDumpFile& st, const fc5::bodyik::SeatIn& in, UINT count, bool hands, int r, const float* out)
{
    std::lock_guard _{ st.mtx };
    if (in.frame != st.frame) {   // a new frame: one fewer to write
        if (st.frame >= 0 && st.f) {
            fc5::bodyik::seat_dump_frame_done();
            std::fflush(st.f);
        }
        st.frame = in.frame;
    }
    if (st.f && fc5::bodyik::seat_dump_frames() <= 0) {
        std::fclose(st.f);
        st.f     = nullptr;
        st.frame = -1;
        spdlog::info("[BodyIk] bodyikseatdump: {} written ({} seated draws)", kSeatDumpPath, st.draws);
        return;
    }
    if (fc5::bodyik::seat_dump_frames() <= 0) return;
    if (!st.f) {
        fopen_s(&st.f, kSeatDumpPath, "wb");
        if (!st.f) {
            spdlog::info("[BodyIk] bodyikseatdump: {} could not be opened", kSeatDumpPath);
            while (fc5::bodyik::seat_dump_frames() > 0) fc5::bodyik::seat_dump_frame_done();
            return;
        }
        std::fwrite("SEA1", 1, 4, st.f);
        st.draws = 0;
        spdlog::info("[BodyIk] bodyikseatdump: writing {} frames of seated draws to {}", fc5::bodyik::seat_dump_frames(), kSeatDumpPath);
    }
    FILE*         f = st.f;
    const int32_t hd[3]{ int32_t(in.frame), int32_t(count), int32_t(r) };
    const uint8_t fl[12]{ uint8_t(in.moved[0]), uint8_t(in.moved[1]), uint8_t(in.hidden[0]), uint8_t(in.hidden[1]), uint8_t(in.held[0]), uint8_t(in.held[1]),
                          uint8_t(in.fin_hands), uint8_t(in.swing), uint8_t(in.follow), uint8_t(hands), 0, 0 };
    const float   fv[2]{ in.stretch, in.pole_hand };
    const int32_t ns = in.nslots;
    std::fwrite("SEAT", 1, 4, f);
    std::fwrite(hd, 4, 3, f);
    std::fwrite(fl, 1, 12, f);
    std::fwrite(fv, 4, 2, f);
    std::fwrite(&ns, 4, 1, f);
    std::fwrite(&in.M, 4, 16, f);
    std::fwrite(&in.shift, 4, 3, f);
    std::fwrite(&in.hand[0], 4, 16, f);
    std::fwrite(&in.hand[1], 4, 16, f);
    std::fwrite(in.game, 4, 3072, f);
    std::fwrite(in.fin, 4, 3072, f);
    std::fwrite(in.cent, 4, 1024, f);
    std::fwrite(r == fc5::bodyik::kMoved ? out : in.game, 4, 3072, f);
    st.draws++;
}

// ---- bodyikveh: the seated body while driving (fc5::bodyik::seat) ----
// drivehands (FarCry5VehHands.cpp) shows only the hands of the player's seated body, the rest folded onto them; bodyikveh draws the body as the
// game seats it, moved onto the eyes' base (draw_shift, where the hands are drawn from), the head left out, and turns each arm so its wrist
// meets the hand drivehandsgrab placed. Called for the draws drivehands shows (hands true: its palette, the hands placed and the fingers posed)
// and for those it hides (bodyik_veh_hidden: the shirt, the trousers, the boots).
bool FarCry5Stereo::bodyik_veh(ID3D11DeviceContext* ctx, std::vector<float>& pal, const std::vector<float>& game, UINT count, UINT start, INT base, bool hands)
{
    if (!fc5::bodyik::on() || !fc5::bodyik::opts().veh || count == 0 || pal.size() < 3072 || game.size() < 3072) return false;
    if (!m_hide_arms.load() || !m_hide_body.load() || !veh_hands_active()) return false;
    if (!m_bk) m_bk = new BodyIkState{};
    auto&     st    = *m_bk;
    const int frame = m_frame.load();
    if (frame - st.veh_frame > 180) {   // a new drive: its draws logged again
        st.veh_said.clear();
        st.veh_lines = 0;
    }
    st.veh_frame = frame;
    auto say = [&](int outcome, const std::string& text) {
        if (st.veh_said.size() < 64 && st.veh_said.insert((uint64_t(count) << 8) | uint64_t(outcome)).second) spdlog::info("[BodyIk] seated: draw {}: {}", count, text);
    };
    // The mesh (the hands' scan; on a deferred context only one already scanned), the vertex scale, the object transform.
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ifmt{};
    UINT          ioff = 0;
    ctx->IAGetIndexBuffer(&ib, &ifmt, &ioff);
    if (ib) ib->Release();
    const GloveMesh* gm = nullptr;
    if (fc5::rec::is_deferred(ctx)) {
        if (auto it = m_glove_meshes.find(std::make_tuple(static_cast<void*>(ib), count, start, base)); it != m_glove_meshes.end()) gm = &it->second;
    } else gm = glove_mesh(ctx, count, start, base);
    if (!gm || !gm->ok || gm->cent.size() < 256) {
        say(1, "left to drivehands: its vertices are not read (yet)");
        return false;
    }
    ID3D11Buffer* cb[3]{};
    ctx->VSGetConstantBuffers(0, 3, cb);
    float obj[12]{};
    bool  have_obj = false;
    float scale    = 1.13f / 32767.0f;
    {
        std::lock_guard _{ m_obj_mtx };
        if (cb[1]) if (auto it = m_obj_consts.find(cb[1]); it != m_obj_consts.end() && it->second.size >= 48) { std::memcpy(obj, it->second.f, sizeof(obj)); have_obj = true; }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
    }
    for (auto* b : cb) if (b) b->Release();
    if (!have_obj) {
        say(2, "left to drivehands: no object transform");
        return false;
    }
    const std::vector<glm::vec4>& rc = gm->rc.size() >= 256 ? gm->rc : gm->cent;
    glm::vec4 cent[256];
    int       nslots = 0;
    for (int s = 0; s < 256; ++s) {
        cent[s] = glm::vec4{ rc[s].x, rc[s].y * scale, rc[s].z * scale, rc[s].w * scale };
        if (rc[s].x > 0.0f) nslots = s + 1;
    }
    fc5::bodyik::SeatIn in{};
    in.frame     = frame;
    in.game      = game.data();
    in.fin       = pal.data();
    in.nslots    = nslots;
    in.cent      = cent;
    in.M         = fc5::bodyik::from_rows(obj);
    in.shift     = m_mo.valid && frame - m_mo.frame <= 1 ? m_mo.draw_shift : glm::vec3{ 0.0f };
    in.fin_hands = hands;
    in.stretch   = fc5::bodyik::opts().stretch;
    in.swing     = fc5::bodyik::opts().seat_swing;
    in.follow    = fc5::bodyik::opts().seat_follow;
    in.pole_hand = std::clamp(fc5::bodyik::opts().pole_hand, 0.0f, 1.0f);
    for (int sd = 0; sd < 2; ++sd) in.moved[sd] = veh_hand_move(sd, in.hand[sd], in.hidden[sd], in.held[sd]);
    st.veh_out.resize(3072);
    fc5::bodyik::SeatInfo si{};
    const int             r = fc5::bodyik::seat(st.seat, in, st.veh_out.data(), &si);
    if (fc5::bodyik::seat_dump_frames() > 0 || st.seat_dump.f) seat_dump_write(st.seat_dump, in, count, hands, r, st.veh_out.data());
    if (r != fc5::bodyik::kMoved) {
        say(10 + r, fmt::format("left to drivehands: {}", fc5::bodyik::result_name(r)));
        return false;
    }
    pal = st.veh_out;
    // A hand past the arm's reach, a forearm asked to roll far or an elbow swung (seatswing), or a moved hand whose arm has no solve: one line
    // per side every 2 s at most (30 a drive).
    for (int sd = 0; sd < 2; ++sd) {
        const bool is_far  = si.solved[sd] && (si.lunge[sd] > 0.005f || si.k[sd] > 1.01f);
        const bool is_roll = si.solved[sd] && (std::abs(si.roll[sd]) > glm::radians(80.0f) || std::abs(si.swing[sd]) > glm::radians(15.0f) ||
                                               si.bend[sd] > glm::radians(50.0f));
        if (!is_far && !is_roll && !si.unsolved[sd]) continue;
        const auto now = std::chrono::steady_clock::now();
        if (st.veh_lines >= 30 || now - st.veh_line_t[sd] < std::chrono::seconds(2)) continue;
        st.veh_line_t[sd] = now;
        st.veh_lines++;
        spdlog::info("[BodyIk] seated: the {} hand {} (draw {})", sd == 0 ? "right" : "left",
                     si.unsolved[sd] ? std::string{ "moved but its arm has no solve yet: drivehands' fold kept" }
                                     : in.follow ? fmt::format("at {:.0f}% of the arm's length, {}: the elbow as standing, the shoulder reached {:.1f} cm, the arm lengthened x{:.2f}, the "
                                                               "forearm moved {:.1f} cm onto the hand, the forearm asked to roll {:+.0f} deg, the wrist bent {:.0f} deg, the elbow {:+.0f} out {:+.0f} forward "
                                                               "{:+.0f} up (cm from the shoulder; seatfollow on)",
                                                               si.reach[sd] * 100.0f, in.held[sd] ? "held" : "free", si.lunge[sd] * 100.0f, si.k[sd], si.gap[sd] * 100.0f,
                                                               glm::degrees(si.roll[sd]), glm::degrees(si.bend[sd]), si.elbow[sd].x * 100.0f, si.elbow[sd].y * 100.0f,
                                                               si.elbow[sd].z * 100.0f)
                                     : fmt::format("at {:.0f}% of the arm's length: the shoulder reached {:.1f} cm, the arm lengthened x{:.2f}, the forearm moved {:.1f} cm onto the hand; "
                                                   "the hand asked the forearm to roll {:+.0f} deg, the elbow swung {:+.0f} deg, the wrist bent {:.0f} deg (seatfollow {}, seatswing {})",
                                                   si.reach[sd] * 100.0f, si.lunge[sd] * 100.0f, si.k[sd], si.gap[sd] * 100.0f, glm::degrees(si.roll[sd]),
                                                   glm::degrees(si.swing[sd]), glm::degrees(si.bend[sd]), in.follow ? "on" : "off", in.swing ? "on" : "off"),
                     count);
    }
    say(3, fmt::format("seated ({} slots placed; {} hand draw): right arm {}, left arm {}", si.placed, hands ? "drivehands'" : "not a",
                       si.solved[0] ? fmt::format("to the hand (reach {:.0f}%, {:.1f} cm short)", si.reach[0] * 100.0f, si.gap[0] * 100.0f) : std::string{ "as the game has it" },
                       si.solved[1] ? fmt::format("to the hand (reach {:.0f}%, {:.1f} cm short)", si.reach[1] * 100.0f, si.gap[1] * 100.0f) : std::string{ "as the game has it" }));
    return true;
}

bool FarCry5Stereo::bodyik_veh_hidden(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, ID3D11Buffer* palette_cb)
{
    if (!fc5::bodyik::on() || !fc5::bodyik::opts().veh || count == 0 || !palette_cb || m_hand_cb_swapped || !veh_hands_active()) return false;
    if (!m_bk) m_bk = new BodyIkState{};
    auto& st = *m_bk;
    {
        std::lock_guard _{ m_obj_mtx };
        auto            it = m_bone_data.find(palette_cb);
        if (it == m_bone_data.end() || it->second.size() < 3072) return false;
        st.veh_game = it->second;
    }
    st.veh_pal = st.veh_game;
    if (!bodyik_veh(ctx, st.veh_pal, st.veh_game, count, start, base, false)) return false;
    return hands_cb_swap(ctx, st.veh_pal);
}

// vehbodyindirect (FarCry5VehHands.cpp): a seated body draw that comes indirect (the game's sidearm out while driving: 7725 and 3210 indices)
// read for its shoulders, elbows and wrists only (bodyik::seat keeps them for the hand draws' arms); the draw itself stays hidden (no palette
// swapped in, so nothing of it is drawn: no sleeve in the game's own pose next to the arms the mod places). true: read.
bool FarCry5Stereo::bodyik_veh_joints(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, ID3D11Buffer* palette_cb)
{
    if (!fc5::bodyik::on() || !fc5::bodyik::opts().veh || count == 0 || !palette_cb || !veh_hands_active()) return false;
    if (!m_bk) m_bk = new BodyIkState{};
    auto& st = *m_bk;
    {
        std::lock_guard _{ m_obj_mtx };
        auto            it = m_bone_data.find(palette_cb);
        if (it == m_bone_data.end() || it->second.size() < 3072) return false;
        st.veh_game = it->second;
    }
    std::vector<float> scratch = st.veh_game;   // the posed palette is not used: only the joints bodyik::seat keeps
    return bodyik_veh(ctx, scratch, st.veh_game, count, start, base, false);
}

// ---- handroles: a mesh's hands from the rest skeleton (fc5::bodyik::hand_roles) ----
bool FarCry5Stereo::hand_roles_mesh(const GloveMesh* gm) const
{
    return gm && fc5::bodyik::hand_roles_on() && gm->hr_any();
}

// Reads the mesh's hands once the bound draw's vertex scale is known (VS b2); again if the scale changes. true: it holds a hand.
bool FarCry5Stereo::hand_roles_note(ID3D11DeviceContext* ctx, const GloveMesh* gm)
{
    if (!fc5::bodyik::hand_roles_on() || !ctx || !gm || !gm->ok || gm->cent.size() < 256) return false;
    float scale = 0.0f;
    {
        ID3D11Buffer* g2 = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &g2);
        std::lock_guard _{ m_obj_mtx };
        if (g2) if (auto it = m_obj_consts.find(g2); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
        if (g2) g2->Release();
    }
    if (scale <= 0.0f || (gm->hr_scale > 0.0f && std::abs(scale - gm->hr_scale) < 0.01f * scale)) return gm->hr_any();   // not known yet, or read already
    // The roles from the true centres (GloveMesh::rc); the hands' code goes on with cent for the slots that are hand or cuff.
    const std::vector<glm::vec4>& rc = gm->rc.size() >= 256 ? gm->rc : gm->cent;
    glm::vec4 cm[256];
    for (int s = 0; s < 256; ++s) {
        const glm::vec4& c = rc[s];
        cm[s]              = glm::vec4{ c.x, c.y * scale, c.z * scale, c.w * scale };
    }
    const fc5::bodyik::HandRoles hr = fc5::bodyik::hand_roles(cm, 256);
    gm->hr_scale = scale;
    gm->hr_cent.assign(256, glm::vec4{ 0.0f });
    gm->hr_fold.assign(256, uint8_t(0));
    int nhand = 0, ncuff = 0, nfold = 0;
    // hr_std: a mesh that covers the hand unevenly (a bandage, a strap, fingerless or long gloves, a suit's hand, hair) has its centre off
    // the full hand's (fc5::bodyik::hand_centre: the bare arms' and the full gloves', within 1.5 cm on every one of them): it takes that
    // one, carried by its hand bone, so the hand sits on the controller as theirs does.
    for (int side = 0; side < 2; ++side) {
        gm->hr_hand[side] = hr.hand[side];
        gm->hr_palm[side] = hr.hand[side] ? hr.palm[side] : -1;
        gm->hr_std[side]  = hr.hand[side] && hr.palm[side] >= 0 && hr.uneven[side];
    }
    for (int s = 0; s < 256; ++s) {
        const glm::vec4& c = gm->cent[s];
        if (c.x <= 0.0f && rc[s].x <= 0.0f) continue;
        const fc5::bodyik::HandSlot& h    = hr.slot[s];
        const int                    kind = hr.hand[h.side] ? int(h.kind) : int(fc5::bodyik::kHandNone);
        if (kind == fc5::bodyik::kHandNone) {
            gm->hr_fold[s] = 1;
            nfold++;
            continue;
        }
        // The hand's and the cuff's slots as the hands' code has always had them (cent): among them the hand's centre and palm are found as
        // before, so a mesh that is all hand and cuff (the gloves) is treated exactly as with handroles off.
        gm->hr_cent[s] = c;
        (kind == fc5::bodyik::kHandCuff ? ncuff : nhand)++;
    }
    const float off_cm[2]{ 100.0f * hr.centre_off[0], 100.0f * hr.centre_off[1] };
    spdlog::info("[Hands] handroles: a mesh of {} bones (scale {:.3g}): right hand {} (hand bone slot {}, {} finger slots, weight {:.0f} of {:.0f}, centre {:.1f} cm off the full hand's{}), "
                 "left hand {} (hand bone slot {}, {} finger slots, weight {:.0f} of {:.0f}, centre {:.1f} cm off{}); {} hand slots, {} cuff slots go with the hand, {} slots folded onto the wrist; "
                 "by shape it was {}",
                 gm->bones, scale, hr.hand[0] ? "yes" : "no", hr.palm[0], hr.fingers[0], hr.weight[0], hr.side_weight[0], off_cm[0], gm->hr_std[0] ? ": the full hand's is used" : "",
                 hr.hand[1] ? "yes" : "no", hr.palm[1], hr.fingers[1], hr.weight[1], hr.side_weight[1], off_cm[1], gm->hr_std[1] ? ": the full hand's is used" : "", nhand, ncuff, nfold,
                 gm->gloves ? (gm->bones >= kGloveAnyBones ? "gloves" : "glove-shaped with too few bones (hidden)") : gm->arm_side >= 0 ? "one arm with its hand" : "not a hand (hidden)");
    // The bare arms (by shape one arm with its hand) keep their own rules, untouched: Run D (11 Nov, testruns\2026-11-11_bodyikD) showed their
    // hand turned and their fingers posed differently with handroles on (the hand's slots, the fold and, for the indirect draw of 14400 indices
    // the tables do not know by its count, the fingers all came from the skeleton). handroles is for the meshes those rules do not know.
    if (gm->arm_side >= 0) {
        for (int side = 0; side < 2; ++side) { gm->hr_hand[side] = false; gm->hr_palm[side] = -1; gm->hr_std[side] = false; }
        gm->hr_cent.assign(256, glm::vec4{ 0.0f });
        gm->hr_fold.assign(256, uint8_t(0));
        spdlog::info("[Hands] handroles: the mesh of {} bones is a bare arm: left to the bare arms' own rules, as with handroles off", gm->bones);
    }
    return gm->hr_any();
}

// Every slot that is not hand or cuff folded onto the wrist of its side (the palm's matrix, after the hand's move, applied to the rest
// wrist joint), so the mesh ends at the wrist like a glove. A side without a hand in this mesh goes to the other side's wrist.
void FarCry5Stereo::hand_roles_fold(std::vector<float>& bones, const GloveMesh& gm)
{
    if (bones.size() < 3072 || gm.hr_fold.size() < 256 || gm.cent.size() < 256) return;
    glm::vec3 wrist[2]{};
    bool      ok[2]{ false, false };
    for (int side = 0; side < 2; ++side) {
        const int p = gm.hr_palm[side];
        if (!gm.hr_hand[side] || p < 0 || p > 255) continue;
        const glm::vec3 w = fc5::bodyik::rest_joint(fc5::bodyik::kJWrist, side);
        const float*    m = bones.data() + p * 12;
        wrist[side]       = glm::vec3{ m[0] * w.x + m[1] * w.y + m[2] * w.z + m[3], m[4] * w.x + m[5] * w.y + m[6] * w.z + m[7], m[8] * w.x + m[9] * w.y + m[10] * w.z + m[11] };
        ok[side]          = std::isfinite(wrist[side].x + wrist[side].y + wrist[side].z);
    }
    if (!ok[0] && !ok[1]) return;
    for (int s = 0; s < 256; ++s) {
        if (!gm.hr_fold[s]) continue;
        int side = (gm.rc.size() >= 256 && gm.rc[s].x > 0.0f ? gm.rc[s].y : gm.cent[s].y) > 0.0f ? 0 : 1;
        if (!ok[side]) side = 1 - side;
        float* m = bones.data() + s * 12;
        std::fill(m, m + 12, 0.0f);
        m[3] = wrist[side].x; m[7] = wrist[side].y; m[11] = wrist[side].z;
    }
    m_bare_folded++;
}
