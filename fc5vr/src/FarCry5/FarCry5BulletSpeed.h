#pragma once

// Bullet speed (FarCry5BulletSpeed.cpp): research tools for where the game keeps a flying bullet's velocity and its source.
// Scans run on their own thread (results logged when done).

#include <cstdint>
#include <vector>

namespace fc5::bullet
{
    // Every float3 in writable memory with a size between lo and hi (m/s) pointing along dir (cosine >= mindot).
    void scan(const float dir[3], float lo, float hi, float mindot);
    // The candidates whose value changed since the scan (or the last next()), logged.
    void next();
    // Write watches (camscan's DR0-3) on up to 4 velocity candidates; camunwatch logs the writers.
    void watch(const std::vector<int>& idx, unsigned long render_tid);
    // A float value: every address holding it exactly; next() keeps those still holding it (list: probes/fltscan.txt).
    void flt_scan(float value);
    void flt_next(float value, bool list);
    void flt_watch(const std::vector<int>& idx, unsigned long render_tid, bool reads);
    bool busy();

    // bulletspeed: the speed of fired bullets times k (1-4), set in each weapon's projectile record (FC_m64+0x1e10d90 hook).
    void  install_speed_hook(uint8_t* fc_m64_base);
    void  set_speed_scale(float k);
    float speed_scale();
    bool  speed_available();
    void  speed_status();
    void  note_player_fire();   // the mod's fire gate, each frame the player's trigger is down
    // The mod let the trigger through to the game (press_ms after the press): the next shot code call logs how long the game took.
    void  note_shot_release(uint64_t press_ms, uint64_t weapon);
    // shotbuzz (FarCry5Stereo::shot_buzz) and recoil: called on the game thread at each of the player's shots, from the shot code above:
    // the player's trigger down within 500 ms, and (shotowner) the first call after each of the mod's releases or a later one from the
    // player's weapon objects so found, so other characters' shots are left out.
    void  set_player_shot_callback(void (*cb)());
    // Every shot code call so far, any character's (the hook above; 0 while it is off).
    uint64_t shot_calls();
    // drivegunthrottle rt's guard (ModConfig.cpp; gunthrottle3, 22 Nov): the driver's own sidearm told from other characters' guns.
    // note_drive_pad: each poll of the drive buttons layout, whether the pad sends LB (the drive-and-shoot fire) and right A's throttle.
    // A weapon object (r14) firing within 300 ms of LB is the driver's sidearm (up to 4 kept); one of them firing with LB and the
    // throttle both up for over 1.5 s is someone else's gun and is dropped. driver_shot_calls: shot code calls from those objects so far;
    // other_shot_calls: every other call (enemies, other characters).
    void     note_drive_pad(bool lb, bool throttle);
    uint64_t driver_shot_calls();
    uint64_t other_shot_calls();
    // shotowner on|off (default on, not saved; F11 Tests): off is the rule before 15 Nov (the weapon of the press's first shot code call).
    void  set_shot_owner(bool on);
    bool  shot_owner();

    // FC2 VR item 2 (23 Oct): the weapon's fire function (FC_m64+0x1e37110) right after its protected call [vtbl+0x278] fills the shot's
    // origin [rbp-0x58] and direction [rbp-0x68] (hook at +0x1e375fa, bytes checked); the projectile spawn and the shot event read them.
    //  shotprobe N: the next N calls logged ([ShotProbe]). bulletaim / bulletorigin (shots rewritten along the barrel) were removed with
    //  F11 Experimental > Research for alternatives (author 30 Oct).
    bool install_shot_hook(uint8_t* fc_m64_base);
    void shot_probe(int n);
    // Each present: the game camera (position, forward) and the barrel (direction, drawn muzzle), world space, z up.
    void set_view(const float cam[3], const float fwd[3], const float aim[3], const float muzzle[3], bool aim_valid, bool muzzle_at_wall);
}
