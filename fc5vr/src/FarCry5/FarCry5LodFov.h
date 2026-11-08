#pragma once
// lodfov (FarCry5LodFov.cpp): far detail as at the flat FOV, through the game's render config read from its archive.
#include <string>

namespace fc5::lodfov
{
    // Early (before the game reads its render config): reads the lodfov line of fc5vr_settings.txt, builds the changed
    // config and hooks the file reads. Off: nothing is hooked.
    void install_early();
    // lodfov off|on|<scale>: saved; used at the next game start.
    void set(const std::string& arg);
    float       scale();          // 0 = off
    std::string describe();       // for the log and the F11 menu
    // shadowres off|<n> (saved; next start): the sun's cascaded shadow map size in the same changed config (game: 1536 at ultrahigh).
    void set_shadow_res(const std::string& arg);
    int  shadow_res();            // the setting, 0 = the game's
    int  shadow_res_active();     // this run's, 0 = the game's
    // vtbias <x> (next start): added to the terrain virtual texture's LOD bias (game: 0 at Terrain high; below 0 = finer).
    // Not saved on its own since 6 Nov (texdetail): it follows the saved mipbias line, F11 Display "Texture detail" setting both.
    void  set_vt_bias(const std::string& arg);
    float vt_bias();
    float vt_bias_active();
    // sunranges off|<near>/<mid>/<far> (saved; next start; shadow clarity 7 Nov): the Shadow presets' SunShadowRange0/1/2 in metres
    // (game: 4 / 11 / 30), where each of the three sun cascades ends.
    void        set_sun_ranges(const std::string& arg);
    std::string sun_ranges();         // the setting: "off" or "a/b/c"
    std::string sun_ranges_active();  // this run's, for the F11 hint
    // lowsuncsm off|on (on by default since 7 Nov 19:47; saved; next start; lights-and-shadows 7 Nov): DisableShadowNightRange="1" in the Shadow presets, so the game
    // keeps three sun cascades in the evening and at night (it merges cascades 1 and 2 into a 22 m one from 18:00-18:30 to 04:00-05:00).
    void set_low_sun_csm(bool on);
    bool low_sun_csm();          // the setting
    bool low_sun_csm_active();   // this run's
    // perfleafshadow, perfgrassshadow, perflampshadow, perfsoftshadow, perfreflect, perffog on|off (saved; next start; performance
    // thread 6 Nov): one game render setting each, a step down in the same changed config. True when cmd was one of them.
    bool perf_command(const std::string& cmd, const std::string& arg);
    int  perf();              // the settings' bits (next start)
    int  perf_active();       // this run's bits
    int  perf_bit(const char* cmd);
}
