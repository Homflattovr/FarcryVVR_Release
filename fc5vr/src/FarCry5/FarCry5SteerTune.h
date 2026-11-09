#pragma once
// Steering lab (test; steering thread, author 7 Nov 21:37):
// the game's own wheeled-vehicle steering tuning found in its memory and changed live, a per-frame probe of the car's answer and a
// timed step of the stick the game reads, to measure where the delay comes from (FarCry5SteerTune.cpp).
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::steertune
{
    // steerscan, steerlist [N], steerset FIELD V|xK|off, steerset off, steerprobe [S]|off, steerstep RT X1 MS1 [X2 MS2 ...],
    // steerfind auto [wide]|list|off, steersweep, steerpeek K OFF [N], steerpoke K OFF V (tests, never saved).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Once per present on the render thread, after the vehicle tick: the car's heading (rad), speed (m/s) and whether the player drives.
    void frame(float heading, float speed, bool driving);
    // The game's own pad poll (VR::on_xinput_get_state, not the virtual pad's update): the left stick X it is handed (-1..1).
    void note_poll(float lx);
    // steerstep, steerfind, steersweep: the stick X and RT to hand the game now (both pad paths); false when none runs.
    bool step_value(float& x, int& rt);
    // driveautoturn (author 9 Nov 11:09): the left stick full left for ms, full right for ms, centred; no throttle. FarCry5VehHands.cpp calls it once a
    // drive when the car's wheel is not known yet, so the game's hands turn it and drivehandsmodel learns it at once.
    void auto_turn(int ms);
    // F11 Experimental > Steering lab (test).
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
