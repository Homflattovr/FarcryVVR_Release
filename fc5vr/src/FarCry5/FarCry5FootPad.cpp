// On-foot buttons: stickjump and footbuttons (see FarCry5FootPad.h; author 11 Nov 2026).
#include "FarCry5FootPad.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <istream>
#include <mutex>
#include <string>

namespace fc5::footpad
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        constexpr float kFire    = 0.7f;   // |y| past this fires
        constexpr float kRearm   = 0.3f;   // |y| under this ends the press; the stick under this (both axes) is centred
        constexpr float kVert    = 0.5f;   // a push counts only with |x| < kVert * |y| (about 27 deg off straight up or down)
        constexpr float kTurn    = 0.5f;   // |x| past this first: a turn until centred
        constexpr int   kMinMs   = 100;    // a flick still holds the button this long, so the game's poll sees it
        constexpr int   kStaleMs = 300;    // no poll this long (the F11 menu, a load): a stick already out waits for the centre

        enum class St : int { Idle, Turn, Up, Down, Wait };

        std::atomic<bool> g_stick{ true };
        std::atomic<bool> g_buttons{ true };

        std::mutex        g_mtx;   // the state below (both pad pollers come here)
        St                g_st{ St::Idle };
        clock::time_point g_last{}, g_down_t{};
        St                g_last_press{ St::Idle };
        int               g_said{ 0 };
        int               g_jumps{ 0 }, g_crouches{ 0 }, g_blocked{ 0 };

        const char* name(St s)
        {
            switch (s) {
            case St::Idle: return "centre";
            case St::Turn: return "turn";
            case St::Up: return "jump (A)";
            case St::Down: return "crouch (B)";
            default: return "waits for the centre";
            }
        }
    }

    bool stick_jump() { return g_stick.load(); }
    bool foot_buttons() { return g_buttons.load(); }

    StickOut stick_poll(float x, float y, bool allowed)
    {
        StickOut out{};
        std::lock_guard _{ g_mtx };
        const auto now = clock::now();
        if (now - g_last > std::chrono::milliseconds(kStaleMs)) g_st = St::Wait;   // first poll after a gap: no press from a stick already out
        g_last = now;
        const bool on      = g_stick.load();
        const bool centred = std::abs(x) < kRearm && std::abs(y) < kRearm;
        const St   was     = g_st;
        if (!on || !allowed) {
            if (g_st == St::Up || g_st == St::Down || (!centred && g_st == St::Idle)) {
                if (on && g_st != St::Wait && g_said < 40) {
                    ++g_said;
                    spdlog::info("[FootPad] stickjump: the right stick's {} ended or held back (not allowed now: vehicle, wheel, D-pad, scope zoom, menu); "
                                 "waits for the centre", g_st == St::Idle ? "push" : name(g_st));
                }
                if (g_st == St::Idle) ++g_blocked;
                g_st = St::Wait;
            }
            if (centred) g_st = St::Idle;
        } else {
            switch (g_st) {
            case St::Wait:
            case St::Turn:
                if (centred) g_st = St::Idle;
                break;
            case St::Up:
            case St::Down:
                if (std::abs(y) < kRearm || (g_st == St::Up ? y < 0.0f : y > 0.0f)) g_st = centred ? St::Idle : St::Wait;
                break;
            case St::Idle:
                if (std::abs(y) > kFire && std::abs(x) < kVert * std::abs(y)) {
                    g_st         = y > 0.0f ? St::Up : St::Down;
                    g_down_t     = now;
                    g_last_press = g_st;
                    (g_st == St::Up ? g_jumps : g_crouches)++;
                } else if (std::abs(x) > kTurn) {
                    g_st = St::Turn;
                }
                break;
            }
        }
        if (g_st != was && (g_st == St::Up || g_st == St::Down) && g_said < 60) {
            ++g_said;
            spdlog::info("[FootPad] stickjump: right stick ({:+.2f}, {:+.2f}): {} ({} jumps, {} crouches, {} pushes held back so far)", x, y, name(g_st), g_jumps,
                         g_crouches, g_blocked);
        }
        const bool flick = now - g_down_t < std::chrono::milliseconds(kMinMs);
        out.a            = g_st == St::Up || (flick && g_last_press == St::Up && on && allowed);
        out.b            = g_st == St::Down || (flick && g_last_press == St::Down && on && allowed);
        out.turn_block   = g_st == St::Up || g_st == St::Down;
        return out;
    }

    bool command(const std::string& cmd, const std::string& arg, float, std::istream&, bool& persist)
    {
        if (cmd != "stickjump" && cmd != "footbuttons") return false;
        const bool on = arg.empty() || arg == "on" || arg == "1" || arg == "true";
        persist       = true;
        if (cmd == "stickjump") {
            g_stick = on;
            spdlog::info("[FootPad] stickjump {}", on ? "on: on foot the right stick up jumps (A), down crouches / slides (B)" : "off: the right stick only turns on foot");
        } else {
            g_buttons = on;
            spdlog::info("[FootPad] footbuttons {}", on ? "on: on foot left X held is the weapon wheel (LB), the left trigger held the D-pad modifier"
                                                        : "off: on foot left X crouches (B), the left trigger is the weapon wheel (LB)");
        }
        return true;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        bool sj = g_stick.load();
        if (ImGui::Checkbox("On foot: right stick up jumps, down crouches / slides##stickjump", &sj)) cmds += sj ? "stickjump on\n" : "stickjump off\n";
        hint("On foot, push the right stick up to jump and pull it down to crouch (or slide while sprinting). Off: the right stick only turns.");
        bool fb = g_buttons.load();
        if (ImGui::Checkbox("On foot: left X is the weapon wheel, left trigger is the modifier (D-pad, throw, map)##footbuttons", &fb)) cmds += fb ? "footbuttons on\n" : "footbuttons off\n";
        hint("On foot, hold left X for the weapon wheel, and hold the left trigger with a stick for the D-pad. Off: left X crouches and the left trigger is the weapon wheel.");
        if (fb && !sj) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "With the stick jump off and left X on the weapon wheel, nothing crouches on foot.");
    }
}
