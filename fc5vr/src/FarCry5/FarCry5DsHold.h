#pragma once
// drivekeephold: while drivereadykeep keeps the driver's sidearm out, the game's own drive-and-shoot put-away timer is held off
// (FarCry5DsHold.cpp; author 14 Nov).
#include <iosfwd>
#include <string>

namespace fc5::dshold
{
    // drivekeephold on|off (saved, default on; FarCry5Drive.cpp passes it here); drivekeepholdscan, drivekeepholdlist,
    // drivekeepholdset S|off (tests, not saved).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    bool on();
    // Driving with driveready: the one memory scan is started early, so the times are found before the first draw. Cheap every frame.
    void prepare();
    // Every frame: want true while drivereadykeep keeps the sidearm out (the times held long), false otherwise (the game's own put back).
    void hold(bool want);
    // The times are written now (the hold in effect).
    bool holding();
}
