// reloadmanual (see FarCry5Reload.h): the magazine changed by hand. The state, on the pad poll:
//   mag_in_gun   the gun's magazine seated (false from the drop to the click)
//   hand_mag     a magazine in the other hand (taken at the pouch, held while the grip lasts)
//   engaged      that magazine in the well, following the hand up (insert 0..1); the click seats it
//   need_rack    the clip ran dry: the charging handle must be racked once a fresh magazine is seated (mag_fresh)
//   racking      the handle held and pulled (rack 0..1); the click chambers a round
//   game_pending the game's own reload, tapped at the drop, has not refilled the clip yet
// The gun fires only with its magazine seated, nothing to rack and the game's clip refilled.
//
// The game's ammo: the game reloads itself (its reload key tapped at the drop; or its own reload once the clip runs dry, bAutoReload 1 for
// the AR-C), so the clip and the reserve stay the game's (no rounds made or lost). The AR-C's data (research 2026-11-02_manualreload.md):
// clip 30, a reload 2.05 s (0.83 + 0.83 + 0.56), from empty 2.75 s (0.83 + 0.83 + 1.53). Clip and reserve are read from the game every
// 100 ms on its script thread (FarCry5Lua.cpp, GetPlayerAmmoIsEmpty's own route); without them, timers stand in.
#include "FarCry5Reload.h"
#include "FarCry5GameSound.h"
#include "FarCry5Fingers.h"
#include "FarCry5Lua.h"
#include "FarCry5ReloadMatch.h"
#include <glm/gtc/quaternion.hpp>
#include "FarCry5GameWeapon.h"
#include "FarCry5ReloadParts.h"
#include "FarCry5Gesture.h"
#include "FarCry5SwitchSpeed.h"
#include "ModPath.h"

#include <mmsystem.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <istream>
#include <iterator>
#include <thread>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace fc5::reload
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr int kGun  = gesture::kWeaponHand;
    constexpr int kHand = gesture::kOtherHand;

    constexpr uint32_t kArc = 57819u;   // the AR-C (the author holster log, grip (0.13, -0.11, -0.28))

    // Built-in parts per weapon (manualreload6, 2 Nov). From the bone map thread's first-person models (research
    // 2026-11-02_bonemap_models.txt): each weapon's main draw (its id), the MAGAZINE (CLIP, MAGASINE) slot in it, the slots that move back
    // with the pull (SLIDE, CHARGING_HANDLE and its tabs, BOLT), the top round's draw (BULLET) and the game's spare magazine props. Author
    // 2 Nov 14:01: only the
    // AR-C was listed and built in. A pistol's slide is its handle, taken at its rear (the serrations); its magazine sits in the grip, so its
    // well's axis is the magazine's own length, not the gun's up; it goes further in (x1.6 reloadtravel) and the slide comes back less far
    // (x0.75 reloadrack: 4.5 cm at the default 6).
    struct Builtin
    {
        uint32_t    sig;              // the weapon id: its main draw's index count
        const char* name;
        uint32_t    mag;              // the magazine's slot in the main draw
        uint32_t    round;            // the top round's draw (0: none) ...
        uint32_t    round_bones[2];   // ... and its slots
        int         nround;
        uint32_t    props[3];         // the spare magazine (and its round) the game's left hand brings in during its reload: hidden
        uint32_t    handle[4];        // the main draw's slots that move back with the pull (the first: beside the ejection port)
        int         nhandle;
        uint32_t    grab[2];          // where the hand takes it: the centre of these ...
        int         ngrab;
        bool        pistol;           // ... or the rear 2.5 cm of the slide (and the pistol's travel, pull and well axis above)
        uint32_t    also;             // another draw that moves with the pull (a pistol's slide sights; 0: none) ...
        uint32_t    also_bone;        // ... its slot
        bool        listed;           // in reloadweapons by default
        float       casing[3];        // the casing port (FX_CASING, model space m: x right, y forward, z up; research\weapondata\weapon_parts.csv): where
                                      // a round leaves at the pull (0 0 0: beside the handle's first part)
        int         kind{ 0 };        // kMagFed (0), or kTube: a pump shotgun, loaded a shell at a time, the pump its handle (manualreload9)
        int         clip{ 0 };        // the game's clip (research\weapondata\reload_data.csv): a tube's capacity until a larger clip is seen
        bool        semi{ false };    // fires again by itself (mustPrepareShot 0 in reload_data.csv: the SPAS-12): no pump after a shot
        uint32_t    belt_from{ 0 }, belt_to{ 0 };   // a belt's rounds in the main draw (slots belt_from..belt_to), part of the magazine (the
                                      // round draw then holds the rest of the belt, every slot of it: nround of them)
        bool        norack{ false };  // nothing to rack (the flamethrower's canister): no handle to pull, and no rack after running dry (manualreload12)
        float       lever_deg{ 0.0f };      // a lever action (kTube, manualreload30): its lever (handle[0]) thrown this far down about ...
        float       lever_pivot[3]{};       // ... its hinge (model space m: x right, y forward, z up); the other handle parts (a bolt) slide back
        bool        rround{ false };  // allreload: the round draw is rigid (the M79's grenade, the RPG-7's rocket: one piece)
        bool        bolt{ false };    // allreload: a bolt action (the .308): its bolt (the handle) worked after every shot, with reloadbolt
    };
    constexpr int kMagFed = 0, kTube = 1, kRevolver = 2, kBelt = 3;
    // allreload (author 3 Nov 14:44): each kind of weapon on its own switch (saved, default on).
    // On, every built-in weapon of that kind reloads by hand, ticked in reloadweapons or not; off, only the ticked ones (as before).
    // manualreload47 (author 3 Nov 14:45; the
    // tracker thread: 20 magazine guns, the AKs, MP5s, Vector, BZ19, MS16, SVD, SA-50, SMG-11, Skorpion and others, still on the game's reload,
    // not ticked in his saved reloadweapons): the remaining kinds on their own switches too: reloadmag (magazine guns: rifles, SMGs, pistols,
    // marksman rifles), reloadpump (pump shotguns), reloadrevolver (revolvers).
    enum WType { kTypeFlame, kTypeGl, kTypeRocket, kTypeLever, kTypeBreak, kTypeBelt, kTypeBolt, kTypeMag, kTypePump, kTypeRevolver, kTypeOther, kTypeCount };
    constexpr const char* kTypeCmd[kTypeCount]{ "reloadflame", "reloadgl", "reloadrocket", "reloadlever", "reloadbreak", "reloadbelt", "reloadbolt",
                                                "reloadmag", "reloadpump", "reloadrevolver", "reloadother" };
    constexpr const char* kTypeName[kTypeCount]{ "the flamethrower (canister)", "the M79 grenade launcher (break open, one grenade)",
                                                 "the RPG-7 (a rocket at the muzzle)", "lever actions (1887, 1888, repeaters)",
                                                 "double barrels (Sledge, sawed-off)", "the M60 (belt) and M249 (box)", "bolt actions (the .308: the bolt after each shot)",
                                                 "magazine guns (rifles, SMGs, pistols)", "pump shotguns", "revolvers",
                                                 "every other gun (no built-in parts: learnt at its first reload; Mars energy cells)" };
    // allreload2 (author 3 Nov 21:16): reloadother (saved, default on), the guns no row
    // below covers (other_gun): the M712, the Mark IV and other .44 models, the Lost on Mars energy weapons and its flamethrower, the
    // Vietnam guns that are other models, the arcade map's unnamed ids. A magazine reload whose magazine is learnt from the game's own reload
    // at the first quick B (the mod's drawn magazine until then).
    std::atomic<bool>     g_type_on[kTypeCount]{ true, true, true, true, true, true, true, true, true, true, true };
    constexpr Builtin kBuiltin[]{
        // The AR-C and its long version (WPN_AR15, WPN_AR15_SNIPER): 1 MAGAZINE, 3036 BULLET, the spare 5106 (WPN_ATT_ARC_MAG); 7
        // CHARGING_HANDLE, 8 and 9 its tabs, 6 the bolt it pulls (the game's reload from empty moved all four back 7.0-7.5 cm, 2 Nov 12:24).
        { 57819, "AR-C", 1, 3036, { 0 }, 1, { 5106 }, { 7, 8, 9, 6 }, 4, { 8, 9 }, 2, false, 0, 0, true, { 0.0045f, 0.1338f, 0.1087f } },
        { 62229, "AR-C (long)", 1, 3036, { 0 }, 1, { 5106 }, { 7, 8, 9, 6 }, 4, { 8, 9 }, 2, false, 0, 0, true, { 0.0045f, 0.1338f, 0.1087f } },
        // The M16A1 (WPN_DLCV_M16A1; manualreload24, from the RE thread's research\weapondata\reload_parts.csv, not on the arcade map): 1 MAGAZINE,
        // the AR-C's round 3036, 6 CHARGING_HANDLE and 7 its left tab, 5 the bolt. No spare prop known; not listed by default (not seen yet).
        { 40752, "M16A1", 1, 3036, { 0 }, 1, { 5853 }, { 6, 7, 5 }, 3, { 6, 7 }, 2, false, 0, 0, false, { 0.0045f, 0.1338f, 0.1087f } },
        // Pistols: the slide, its sights (a small draw of their own on the slide's bone), the round on top of the magazine.
        { 24417, "1911", 5, 1152, { 0 }, 1, {}, { 7 }, 1, { 7 }, 1, true, 18, 0, true, { 0.0015f, 0.0233f, 0.0669f } },
        { 26043, "1911 (extended)", 5, 1152, { 0 }, 1, {}, { 7 }, 1, { 7 }, 1, true, 18, 0, true, { 0.0015f, 0.0233f, 0.0669f } },
        { 22182, "P226", 5, 1152, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, true, 18, 0, true, { 0.0010f, 0.0340f, 0.0648f } },
        { 29136, "M9", 4, 1224, { 0 }, 1, {}, { 2 }, 1, { 2 }, 1, true, 18, 0, true, { -0.0008f, 0.0408f, 0.0796f } },
        { 28596, "D50", 7, 2304, { 0, 1 }, 2, {}, { 1 }, 1, { 1 }, 1, true, 78, 1, true, { 0.0000f, 0.0263f, 0.0838f } },
        { 26187, "6P9", 5, 600, { 0 }, 1, {}, { 2 }, 1, { 2 }, 1, true, 54, 1, true, { 0.0024f, 0.0518f, 0.0745f } },
        { 32325, "A99", 2, 1152, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, true, 0, 0, true, { 0.0087f, 0.1159f, 0.0928f } },
        // Not listed by default (tick them in F11, or reloadweapons): parts from the bone map only, never seen moving.
        { 33765, "AJM9", 3, 2448, { 0, 1 }, 2, {}, { 2 }, 1, { 2 }, 1, true, 0, 0, false, { -0.0008f, 0.0408f, 0.0796f } },
        { 35187, "MAC11", 1, 1152, { 0 }, 1, {}, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0086f, 0.0050f, 0.0665f } },
        { 28968, "AK-47", 1, 0, {}, 0, { 3930, 4914, 3036 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0434f, 0.1893f, 0.1248f } },
        { 6867, "AK-47", 1, 0, {}, 0, { 3930, 4914, 3036 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0434f, 0.1893f, 0.1248f } },
        { 45582, "AK-47 (classic)", 1, 2748, { 0 }, 1, { 3930, 4914, 3036 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0265f, 0.1893f, 0.1191f } },
        { 53007, "AK-47 (no stock)", 1, 2748, { 0 }, 1, { 3930, 4914, 3036 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0265f, 0.1893f, 0.1191f } },
        { 60537, "AK-47 (modern)", 1, 2748, { 0 }, 1, { 3930, 4914, 3036 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0265f, 0.1893f, 0.1191f } },
        { 37359, "MP5", 1, 1224, { 0 }, 1, {}, { 5, 6 }, 2, { 5 }, 1, false, 0, 0, false, { -0.0057f, 0.1282f, 0.1375f } },
        { 35868, "MP5K", 1, 1224, { 0 }, 1, {}, { 5, 6 }, 2, { 5 }, 1, false, 0, 0, false, { -0.0057f, 0.1282f, 0.1375f } },
        { 35709, "MP5SD", 1, 1224, { 0 }, 1, {}, { 5, 6 }, 2, { 5 }, 1, false, 0, 0, false, { -0.0057f, 0.1282f, 0.1375f } },
        { 39750, "Vector .45", 5, 1152, { 0 }, 1, {}, { 3, 2 }, 2, { 3 }, 1, false, 0, 0, false, { 0.0152f, 0.2082f, 0.0663f } },
        { 41649, "Skorpion", 6, 1224, { 0 }, 1, {}, { 3 }, 1, { 3 }, 1, false, 0, 0, false, { -0.0007f, 0.1074f, 0.0998f } },
        // The MS16s (manualreload24, reload_parts.csv): their rounds 4554 (BULLET_01..03), the handle 11 OPERATING_HANDLE pulling 8 BOLT.
        { 35790, "MS16 (long)", 3, 4554, { 0, 1 }, 2, {}, { 11, 8 }, 2, { 11 }, 1, false, 0, 0, false, { 0.0000f, 0.1914f, 0.0528f } },
        { 32688, "MS16 (short)", 3, 4554, { 0, 1 }, 2, {}, { 11, 8 }, 2, { 11 }, 1, false, 0, 0, false, { 0.0000f, 0.1846f, 0.0528f } },
        { 42555, "SVD", 3, 1518, { 0 }, 1, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0001f, 0.2193f, 0.0843f } },
        { 23397, "SA-50", 3, 2748, { 0, 1 }, 2, {}, { 5 }, 1, { 5 }, 1, false, 0, 0, false, { 0.0109f, -0.1152f, 0.1024f } },   // rounds 2748 (reload_parts.csv)
        // Learnt in the simulator (manualreload15; testruns\2026-11-02_reload3, the clip fired empty into the ground, the game's reload watched),
        // with the bone map's slot names. Not listed by default. BZ19 (WPN_BZ19: 2 AMMO_CLIP, the helical magazine under the barrel, learnt
        // as 39408/2, moving out along -0.14 -0.99 0.08; 6 CHARGE; no round draw). .308 (WPN_308SNIPER: 5 MAGAZINE with the rounds 3036
        // BULLET_01/02, learnt as 3036/0+1 and 24474/5; the spare magazine 3318 and 9168 the game's hand brings in; 2 BOLT). MP40: its
        // magazine 40362/6 (434 vertices, 44 cm out in the reload; the learn took the 9 mm round 1224 that flew 129 cm, as the MP5's and
        // Skorpion's round), no handle known (its slots have no names: the drawn bar). HTI .50 (WPN_HTI50, from the bone map, not learnt:
        // 3 MAGAZINE, the rounds 2748 BULLET_01/02, 1 BOLT).
        // reloadbizonhandle (author 5 Nov 15:51, screenshot):
        // slot 6 (the bone map's CHARGE, 421 vertices, 9 cm long) is the fire selector; slot 5 (named HAMMER, 479 vertices) is the one that
        // moved 5 cm in the game's own reload (the 4 Nov 19:54 learn: "moved: 39408/2:66cm 39408/5:5cm"): the charging handle.
        { 39408, "BZ19", 2, 0, {}, 0, {}, { 5 }, 1, { 5 }, 1, false, 0, 0, false, { -0.0120f, 0.1220f, 0.0756f } },
        // allreload: the .308 is a bolt action (reload_data.csv: mustPrepareShot 1, 0.8 s, clip 5): with reloadbolt its BOLT (2) is worked
        // by the left hand after every shot, as a charging handle (back to its stop, the fired case out; let go, a round chambered).
        { 24474, ".308", 5, 3036, { 0, 1 }, 2, { 3318, 9168 }, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { -0.0008f, 0.1449f, 0.0275f }, kMagFed, 5, false, 0, 0, false,
          0.0f, {}, false, true },
        // The MP40's cocking handle: 3 SLIDE_01 (the knob) and 2 SLIDE_02 (reload_parts.csv, manualreload24).
        { 40362, "MP40", 6, 1224, { 0 }, 1, {}, { 3, 2 }, 2, { 3 }, 1, false, 0, 0, false, { 0.0062f, 0.2872f, 0.1104f } },
        // The MP34 (WPN_MP34): 5 MAGAZINE, rounds 2448 slots 0 and 1, 2 BOLT as its handle: its learn on the arcade map (reload4: 2448/0+1,
        // 43215/5) and reload_parts.csv agree.
        { 43215, "MP34", 5, 2448, { 0, 1 }, 2, {}, { 2 }, 1, { 2 }, 1, false, 0, 0, false, { 0.0119f, 0.3314f, 0.0463f } },
        // (reloadboltpin, author 5 Nov: mustPrepareShot 1 in reload_data.csv, as the .308.)
        { 42000, "HTI .50", 3, 2748, { 0, 1 }, 2, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, false, { 0.0000f, -0.1332f, 0.1049f }, kMagFed, 5, false, 0, 0, false, 0.0f, {}, false, true },
        // Special weapons (manualreload12; author 2 Nov 17:58).
        // Not listed by default. The flamethrower (WPN_FlameThrower: 9 CLIP, the fuel canister, 3192 vertices; reload_data clip 100): the
        // canister is its magazine, nothing to rack. The M249 (WPN_M249_MODERN: 4 MAGASINE, the ammo box under it; 5 HANDLE, the charging
        // handle on its right; 2 BOLT moves with it; FX_CASING 0.0209 0.1469 0.0838): the box swapped as a magazine (the cover and belt not
        // opened yet: research\2026-11-02_manualreload12.md), the handle racked after running dry.
        { 47430, "Flamethrower", 9, 0, {}, 0, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kMagFed, 100, false, 0, 0, true },
        { 23676, "Flamethrower", 9, 0, {}, 0, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kMagFed, 100, false, 0, 0, true },
        // The box takes its belt with it: the rounds 6..17 (BULLET_01..12) of the main draw and the round draw 7476 (12 slots; 25302: 6..15 and
        // 1695, 9 slots).
        // (reloadm249cover, author 5 Nov, 15:41: the M249, a magazine gun until now: its box and belt as before, under a cover worked as the M60's.)
        { 58416, "M249", 4, 7476, {}, 12, {}, { 5, 2 }, 2, { 5 }, 1, false, 0, 0, false, { 0.0209f, 0.1469f, 0.0838f }, kBelt, 80, false, 6, 17 },
        { 25302, "M249", 4, 1695, {}, 9, {}, { 5, 2 }, 2, { 5 }, 1, false, 0, 0, false, { 0.0209f, 0.1469f, 0.0838f }, kBelt, 80, false, 6, 15 },
        // The M60s (WPN_M60E4, WPN_DLC_M60VN): belt-fed, no box in the hand. 1 UPPER_SHELL, the feed cover (its node at 0 0.104 0.162, the
        // hinge); 4 RELOAD_CHARGE, the charging handle; the belt: 6..16 (BULLET_02..12) of the main draw and the round draw (BULLET_01..12; 7200,
        // 2262, 6696, 2070). Quick B lifts the cover (the belt in comes out with it), the next belt from the left hip goes into the tray, quick
        // B closes it; the handle racked after running dry. FX_CASING 0.0169 0.0413 0.1325; reload_data clip 100.
        { 61926, "M60", 6, 7200, {}, 12, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0169f, 0.0413f, 0.1325f }, kBelt, 100, false, 6, 16 },
        { 26238, "M60", 6, 2262, {}, 12, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0169f, 0.0413f, 0.1325f }, kBelt, 100, false, 6, 16 },
        { 63120, "M60 (Vietnam)", 6, 6696, {}, 12, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0169f, 0.0413f, 0.1325f }, kBelt, 100, false, 6, 16 },
        { 26430, "M60 (Vietnam)", 6, 2070, {}, 12, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0169f, 0.0413f, 0.1325f }, kBelt, 100, false, 6, 16 },
        // Pump shotguns (manualreload9; author 2 Nov 15:15): no magazine; the shell is the round draw (BULLET, at the loading port under the receiver), loaded one at a time;
        // the PUMP slot is the handle, worked after each shot (reloadpumpshot). The M133s: 1 PUMP, 2 LOADING_GATE; the SPAS-12: 1 PUMP and 2
        // its BOLT, which moves with it. Listed by default since manualreload20 (author 2 Nov 21:29; the arcade runs loaded, pumped and ejected 35499, 29841 and 42549; the other M133s share their code). reload_data.csv (engine internals thread):
        // clip 7 each, shell by shell (selReloadType 1); the M133s must pump between shots (mustPrepareShot 1, 0.65-0.7 s), the SPAS-12 not.
        { 29259, "M133", 0, 834, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, true, { -0.0001f, 0.2082f, 0.0327f }, kTube, 7 },
        { 34497, "M133 (short)", 0, 834, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, true, { -0.0001f, 0.2082f, 0.0327f }, kTube, 7 },
        { 20586, "M133 (DLC)", 0, 834, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, true, { -0.0001f, 0.2082f, 0.0327f }, kTube, 7 },
        { 35499, "M133 (modern)", 0, 834, { 0 }, 1, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, true, { -0.0001f, 0.1843f, 0.0906f }, kTube, 7 },
        { 29841, "M133 (modern long)", 0, 1014, { 1 }, 1, {}, { 1 }, 1, { 1 }, 1, false, 0, 0, true, { -0.0001f, 0.1843f, 0.0906f }, kTube, 7 },
        // Revolvers (manualreload10): the M629 .44 (its two models). No magazine: the round draw (6912 / 3369: BULLET_01..06 and CAP) is the
        // cylinder's six rounds, one slot each, shown or not by chamber; the crane, drum and extractor swing out (kRevolvers). Clip 6
        // (reload_data.csv, selReloadType 0: the game reloads it whole, hidden).
        { 18912, "M629 (.44)", 0, 6912, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 6 },
        { 9540, "M629 (.44)", 0, 3369, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 6 },
        // Lever actions (manualreload30; reload_parts.csv): the 1887 and 1888 shotguns (3 LEVER, hinged at (0, 0.133, 0.013); shells 1668; 4
        // LOADER) and the repeater rifles (1 LEVER at (0, 0.142, 0.013), 4 BOLT pulled back by it; round 1152; 5 LOADING_GATE). Loaded a shell
        // (a round) at a time from the left hip as the pumps; the lever worked by quick right B (down and up: the chamber's shell thrown out,
        // the next from the tube chambered). Clip 7 (reload_data.csv: the 1887s); the repeaters' 8 until a larger one is seen. Not listed by
        // default until seen in a run.
        { 27144, "1887", 0, 1668, { 0 }, 1, {}, { 3 }, 1, { 3 }, 1, false, 0, 0, false, { 0.0f, 0.1928f, 0.0660f }, kTube, 7, false, 0, 0, false, 50.0f, { 0.0f, 0.133f, 0.013f } },
        { 32472, "1888", 0, 1668, { 0 }, 1, {}, { 3 }, 1, { 3 }, 1, false, 0, 0, false, { 0.0f, 0.1690f, 0.0735f }, kTube, 7, false, 0, 0, false, 50.0f, { 0.0f, 0.133f, 0.013f } },
        { 25368, "Repeater (classic)", 0, 1152, { 0 }, 1, {}, { 1, 4 }, 2, { 1 }, 1, false, 0, 0, false, { 0.0019f, 0.1425f, 0.0676f }, kTube, 8, false, 0, 0, false, 55.0f,
          { 0.0f, 0.142f, 0.013f } },
        { 39012, "Repeater (modern)", 0, 1152, { 0 }, 1, {}, { 1, 4 }, 2, { 1 }, 1, false, 0, 0, false, { 0.0019f, 0.1425f, 0.0676f }, kTube, 8, false, 0, 0, false, 55.0f,
          { 0.0f, 0.142f, 0.013f } },
        // Break-action shotguns (manualreload29; reload_parts.csv: WPN_SLEDGE and WPN_SLEDGE_SAWED_OFF, 1 BARREL hinged at (0, 0.178, 0.011),
        // 2 EJECTOR; their shells' draw 1668, BULLET_01 and 02 in slots 0 and 1; clip 2): a revolver of two chambers (kBreaks below). Quick B
        // opens it (the barrels tip down, the fired shells thrown out), a shell at a time from the left hip into each barrel, quick B closes it.
        // Not listed by default until seen in a run.
        { 35490, "Sledge (double barrel)", 0, 1668, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 2 },
        { 31470, "Sledge (sawed-off)", 0, 1668, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 2 },
        { 42549, "SPAS-12", 0, 834, { 0 }, 1, {}, { 1, 2 }, 2, { 1 }, 1, false, 0, 0, true, { -0.0037f, 0.1718f, 0.0969f }, kTube, 7, true },
        // allreload: the other models of the lever actions and double barrels (reload_parts.csv and weapon_parts.csv: the same parts,
        // another shell or round draw; the 1888 short's lever is slot 4). Not listed by default; on with reloadlever / reloadbreak.
        { 39360, "Repeater (modern long)", 0, 1152, { 0 }, 1, {}, { 1, 4 }, 2, { 1 }, 1, false, 0, 0, false, { 0.0019f, 0.1425f, 0.0676f }, kTube, 8, false, 0, 0, false, 55.0f,
          { 0.0f, 0.142f, 0.013f } },
        { 25020, "Repeater (classic short)", 0, 1152, { 0 }, 1, {}, { 1, 4 }, 2, { 1 }, 1, false, 0, 0, false, { 0.0019f, 0.1425f, 0.0676f }, kTube, 8, false, 0, 0, false, 55.0f,
          { 0.0f, 0.142f, 0.013f } },
        { 21309, "1888 (modern)", 0, 1188, { 0 }, 1, {}, { 3 }, 1, { 3 }, 1, false, 0, 0, false, { 0.0f, 0.1690f, 0.0735f }, kTube, 7, false, 0, 0, false, 50.0f, { 0.0f, 0.133f, 0.013f } },
        { 24504, "1888 (short)", 0, 1668, { 0 }, 1, {}, { 4 }, 1, { 4 }, 1, false, 0, 0, false, { 0.0f, 0.1690f, 0.0735f }, kTube, 7, false, 0, 0, false, 50.0f, { 0.0f, 0.133f, 0.013f } },
        { 22809, "Sledge (double barrel)", 0, 1188, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 2 },
        { 11256, "Sledge (sawed-off)", 0, 744, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 2 },
        // allreload: one-shot launchers, as revolvers of one chamber (kSingles below), their round a rigid draw. The M79 (WPN_M79; the
        // arcade run of 2 Nov: draws 29850 and 2298, the grenade rigid, 538 vertices; reload_data clip 1, its reload 2.26 s): a break action. The
        // RPG-7 (WPN_RPG7_Modern 23475 and its other model 21123; the same run: its rocket the rigid draw 8232, 1720 vertices; reload_data clip
        // 1, its reload 4.2 s): nothing opens, the rocket goes in at the muzzle. Not listed by default; on with reloadgl / reloadrocket.
        { 29850, "M79", 0, 2298, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 16755, "M79", 0, 1245, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 23475, "RPG-7", 0, 2778, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 21123, "RPG-7", 0, 2778, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        // allreload2: the weapons not touched yet whose parts the game's files give (research\weapondata\reload_parts.csv, weapon_parts.csv,
        // reload_data.csv; none seen in a run).
        // The P08 (WPN_LUGER_CLASSIC 11997/21948, WPN_LUGER_EXTENDED 12000/23958): 1 MAGASINE, the magazine; 8 REAR_TOGGLE_LINK, the toggle,
        // worked as a pistol's slide (pulled straight back by its rear: the real toggle folds up, a guess); no round draw; FX_CASING 0 0.0406
        // 0.0730; clip 8.
        { 11997, "P08", 1, 0, {}, 0, {}, { 8 }, 1, { 8 }, 1, true, 0, 0, false, { 0.0000f, 0.0406f, 0.0730f }, kMagFed, 8 },
        { 21948, "P08", 1, 0, {}, 0, {}, { 8 }, 1, { 8 }, 1, true, 0, 0, false, { 0.0000f, 0.0406f, 0.0730f }, kMagFed, 8 },
        { 12000, "P08 (long)", 1, 0, {}, 0, {}, { 8 }, 1, { 8 }, 1, true, 0, 0, false, { 0.0000f, 0.0406f, 0.0730f }, kMagFed, 8 },
        { 23958, "P08 (long)", 1, 0, {}, 0, {}, { 8 }, 1, { 8 }, 1, true, 0, 0, false, { 0.0000f, 0.0406f, 0.0730f }, kMagFed, 8 },
        // The MG42 (WPN_MG42 24471; its other id 65535 is the index count's cap, left out): belt-fed as the M60. 3 UPPER_SHELL, the feed
        // cover (kBeltCovers); 20 RELOAD_HANDLE, the charging handle, and 19 RELOAD_SLIDE with it; the belt: 4..14 (BULLET_02..12) of the
        // main draw and the round draw 2526 (11 slots). FX_CASING 0.0049 0.1352 0.0552; clip 100.
        { 24471, "MG42", 4, 2526, {}, 11, {}, { 20, 19 }, 2, { 20 }, 1, false, 0, 0, false, { 0.0049f, 0.1352f, 0.0552f }, kBelt, 100, false, 4, 14 },
        // reloadmg42 (author 5 Nov; 16:34:57: the one in the hand is 72736, an unknown gun: no handle, no cover). 65536 + its second draw 7200, the first drawn as 65535 (FarCry5Stereo::sig_mesh).
        // weapon_parts.csv's other WPN_MG42: 3 UPPER_SHELL, 15 EJECTOR, 17 RELOAD_SLIDE: the slots of 24471 less 2 from there on, so 18 RELOAD_HANDLE and the rounds 4..12 (a guess from that order); its round draw 7200, 11 slots (as read).
        { 72736, "MG42", 4, 7200, {}, 11, {}, { 18, 17 }, 2, { 18 }, 1, false, 0, 0, false, { 0.0049f, 0.1352f, 0.0552f }, kBelt, 100, false, 4, 12 },
        // The classic RPG-7 (WPN_RPG7_Classic 23589/26211, the same hammer and trigger as the modern one; also the Vietnam RPG-7, if it is
        // this model): the modern one's rocket 8232 (WPN_SHVL_PRJ_RPG), a guess: its own was never seen. The RAT4 (WPN_RAT4 28497; the
        // arcade map's ids 80077 and 69646): its missile WPN_RAT4_MISSILE 3192, loaded at the front as the RPG-7's rocket (a guess; reload
        // 4.4 s). Clip 1. With reloadrocket.
        { 23589, "RPG-7 (classic)", 0, 2778, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 26211, "RPG-7 (classic)", 0, 2778, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 28497, "RAT4", 0, 3192, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 80077, "RAT4", 0, 3192, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
        { 69646, "RAT4", 0, 3192, { 0 }, 1, {}, {}, 0, {}, 0, false, 0, 0, false, {}, kRevolver, 1, false, 0, 0, false, 0.0f, {}, true },
    };
    // A revolver's cylinder (the bone map: WPN_M629's 1 CRANE, 2 DRUM, 3 EXTRACTOR; its rounds' draw BULLET_01..06 in slots 0-5, CAP 6;
    // research\weapondata\weapon_parts.json: the CRANE node at (-0.0026, 0.1213, 0.0321), its hinge along the barrel). It swings out to the
    // left, as a Smith & Wesson's.
    struct RevolverDef
    {
        uint32_t main, rounds;
    };
    constexpr RevolverDef kRevolvers[]{ { 18912, 6912 }, { 9540, 3369 } };
    constexpr RevolverDef kBreaks[]{ { 35490, 1668 }, { 31470, 1668 }, { 22809, 1188 }, { 11256, 744 } };   // manualreload29 (43: two more models)
    // allreload: one-shot launchers (Revolver's rigid_rounds, noopen, muzzle). The M79 (reload_parts.csv, weapon_parts.csv): 2
    // BARREL_ROTATION (the barrel, 2715 of its 7968 vertices) hinged at its node (0, 0.196, -0.001), in front of the breech and under the bore;
    // 3 SHELL_EXTRACTOR and the leaf sight on the barrel (4 IS_ROTATION, 5 IS_ELEVATION, 6 IS_LOCK) tip with it; 1 BARREL_LATCH stays. Its
    // grenade slides 7 cm into the breech. The RPG-7: nothing opens; its rocket's tail slides the last 22 cm into the tube; the game's reload (4.2 s)
    // kept out of the view after a shot.
    struct SingleDef
    {
        uint32_t main, rounds;
        float    pivot[3];
        float    deg;
        bool     muzzle;
        uint32_t swing[6];
        int      nswing;
        float    travel, busy_s;
        bool     door{ false };   // reloadratdoor
    };
    constexpr SingleDef kSingles[]{
        { 29850, 2298, { 0.0f, 0.196f, -0.001f }, -40.0f, false, { 2, 3, 4, 5, 6 }, 5, 0.07f, 2.6f },
        { 16755, 1245, { 0.0f, 0.196f, -0.001f }, -40.0f, false, { 2, 3, 4, 5, 6 }, 5, 0.07f, 2.6f },
        // (reloadrpgdraw: the rocket is WPN_RPG7_Standard 2778 in the author's game, 5 Nov 14:05's census, and in every run since 2 Nov 21:51; the 8232 of the first run was never drawn again, so the rocket was never known, its muzzle
        // never measured and nothing could go in: author. The classic one: the same, a guess.)
        { 23475, 2778, {}, 0.0f, true, {}, 0, 0.22f, 4.6f },
        { 21123, 2778, {}, 0.0f, true, {}, 0, 0.22f, 4.6f },
        { 23589, 2778, {}, 0.0f, true, {}, 0, 0.22f, 4.6f },   // allreload2: the classic RPG-7
        { 26211, 2778, {}, 0.0f, true, {}, 0, 0.22f, 4.6f },
        // reloadratdoor (author 5 Nov): 3 Rear_Lock, 4 Handle_Rear, 5 Rear_Tube open, hinged at their front top (the model, set_door_pivot).
        // (Run final_f, from the side: 5 is the rear cone, 3 and 4 small fittings far ahead of it, about which it flew a metre up: 5 alone, about its own front top.)
        { 28497, 3192, {}, -80.0f, false, { 5 }, 1, 0.25f, 4.8f, true },   // allreload2: the RAT4 (its reload 4.4 s)
        { 80077, 3192, {}, -80.0f, false, { 5 }, 1, 0.25f, 4.8f, true },
        { 69646, 3192, {}, -80.0f, false, { 5 }, 1, 0.25f, 4.8f, true },
    };
    std::atomic<uint32_t> g_door_sig{ 0 };     // set_door_pivot: the weapon ...
    std::atomic<float>    g_door_p[3]{};       // ... and its hinge (model space)
    std::atomic<uint32_t> g_lead_sig{ 0 };     // set_round_lead: the weapon ...
    std::atomic<float>    g_lead_m{ 0.0f };    // ... and its round's lead point from its centre (m)
    std::atomic<uint32_t> g_muzzle_sig{ 0 };   // reloadmuzzlezone: the launcher ...
    std::atomic<float>    g_muzzle_m{ 0.0f };  // ... and its depth from the muzzle to the seated round's tail (m)
    // The kind of a built-in weapon for its switch (allreload; -1: none, the ticked list alone decides).
    int wtype(const Builtin& b)
    {
        if (b.norack) return kTypeFlame;
        if (b.bolt) return kTypeBolt;
        if (b.kind == kBelt || (b.kind == kMagFed && b.belt_to > 0)) return kTypeBelt;
        if (b.kind == kTube && b.lever_deg != 0.0f) return kTypeLever;
        if (b.kind == kRevolver) {
            for (const auto& d : kSingles)
                if (d.main == b.sig) return d.muzzle || d.door ? kTypeRocket : kTypeGl;
            for (const auto& d : kBreaks)
                if (d.main == b.sig) return kTypeBreak;
            return kTypeRevolver;
        }
        if (b.kind == kTube) return kTypePump;
        if (b.kind == kMagFed) return kTypeMag;
        return -1;
    }
    constexpr float       kBreakDeg = -38.0f;                               // the barrels tipped down this far open
    // reloadpartstable on|off (manualreload26; saved, default off; author 2 Nov 21:40): a gun model not in kBuiltin takes its parts from the RE thread's table (FarCry5ReloadParts.h, from the game's files: magazine,
    // rounds, handle, bolt, casing port), so its magazine is known before any reload. A model that another id of kBuiltin shares (a skin or an
    // attachment's mesh: the same WPN_ name) also takes that row's kind, props, pistol slide and clip. Magazine guns, and pumps with such a
    // sibling; not lever, break, belt or revolver models (their flow needs more than the table has). Off: learnt at the first game reload.
    std::atomic<bool>                  g_parts_table{ false };
    std::mutex                         g_table_mtx;
    std::map<uint32_t, struct Builtin> g_table_rows;   // made once per id (pointers stay valid: std::map)
    const Builtin* table_builtin(uint32_t sig)
    {
        const fc5::reloadparts::Parts* t = fc5::reloadparts::find(sig);
        if (!t) return nullptr;
        std::lock_guard _{ g_table_mtx };
        if (const auto it = g_table_rows.find(sig); it != g_table_rows.end()) return it->second.sig ? &it->second : nullptr;
        Builtin  b{};
        const Builtin* sib = nullptr;
        for (const auto& k : kBuiltin)
            if (const auto* kt = fc5::reloadparts::find(k.sig); kt && std::strcmp(kt->model, t->model) == 0) {
                sib = &k;
                break;
            }
        const bool ok = t->kind == fc5::reloadparts::kMag ? t->mag >= 0 : (t->kind == fc5::reloadparts::kPump && sib && sib->kind == kTube);
        if (ok) {
            if (sib) b = *sib;
            b.sig    = sig;
            b.name   = t->model;
            b.listed = false;
            b.mag    = t->mag >= 0 ? uint32_t(t->mag) : 0u;
            if (t->round != 0 || !sib) {   // the round draw is its own model (the shell, the cartridge): the sibling's when the table has none
                b.round  = t->round;
                b.nround = std::min<int>(t->nround, 2);
                for (int i = 0; i < b.nround; ++i) b.round_bones[i] = uint32_t(i);
            }
            b.nhandle = 0;
            for (const int16_t h : { t->handle, t->handle2, t->bolt })
                if (h >= 0 && b.nhandle < 4) b.handle[b.nhandle++] = uint32_t(h);
            b.ngrab = 0;
            if (t->handle >= 0) b.grab[b.ngrab++] = uint32_t(t->handle);
            else if (t->bolt >= 0) b.grab[b.ngrab++] = uint32_t(t->bolt);
            if (!sib) {
                static constexpr const char* kHandguns[]{ "M9", "1911", "P226", "D50", "6P9", "A99", "AJM9", "LUGER" };
                b.pistol = false;
                for (const char* h : kHandguns) b.pistol = b.pistol || std::strstr(t->model, h) != nullptr;
            }
            b.also      = b.pistol ? t->also : 0u;
            b.also_bone = 0;
            for (int i = 0; i < 3; ++i) b.casing[i] = t->casing[i];
            if (b.kind == kMagFed && !sib) b.clip = 0;
        } else {
            b.sig = 0;   // not usable: remembered, so the table is not looked at again
        }
        const auto it = g_table_rows.emplace(sig, b).first;
        if (ok) spdlog::info("[Reload] weapon {} ({}): parts from the table{}: magazine slot {}, rounds {} ({}), handle {} part(s)", sig, t->model,
                             sib ? fmt::format(" (as {}, id {})", sib->name, sib->sig) : std::string{}, b.mag, b.round, b.nround, b.nhandle);
        return ok ? &it->second : nullptr;
    }
    const Builtin* builtin(uint32_t sig)
    {
        for (const auto& b : kBuiltin)
            if (b.sig == sig) return &b;
        return g_parts_table.load() && sig != 0 ? table_builtin(sig) : nullptr;
    }
    // allreload2 (reloadother): a gun with no row above, known by the game's item for its id (FarCry5GameWeapon.cpp, the game's weapon data),
    // or one of the arcade map's ids missing from that data (newer items; the reload tracker's "not named yet" table). A magazine reload,
    // its magazine learnt from the game's own reload: a pistol's or an SMG's travel for the handguns and machine pistols; nothing to rack
    // for the Lost on Mars energy weapons (an energy cell swapped, the author's choice to judge), the launchers and the flamethrowers.
    // Not: shotguns (no magazine; the 1887s, M133s, SPAS-12 and double barrels have rows), bows, melee, throwables, the Magnopulser
    // (reload_data.csv: clip 0, reloadType 2, it never reloads).
    struct Other
    {
        bool        ok{ false }, pistol{ false }, norack{ false };
        const char* name{ "" };
    };
    // (reloadnomars, author 5 Nov: the game's reload again. The arcade map's ids that were listed here were all theirs: 65536 + a WPN_DLCM draw, 73729 8193, 78994 13458,
    // 80074 14538, 80476 14940, 87904 22368, 88912 23376, and the DLC heavy pistol 30567; the gun with the 72736 is the MG42, with its row now.)
    Other other_gun(uint32_t sig)
    {
        Other o{};
        if (sig == 0 || builtin(sig)) return o;
        if (const fc5::weapons::Info* w = fc5::gameweapon::info(sig); w && w->name) {
            const std::string n = w->name;
            const auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
            const int c = w->cls;
            if (!(c == 1 || c == 2 || c == 3 || c == 5 || c == 6 || c == 7 || c == 9) || has("Magnopuls") || has("DLC_Mars") || has("DLCM")) return o;
            o.ok     = true;
            o.name   = w->name;
            o.pistol = c == 1 || (c == 3 && w->one_handed);
            o.norack = c == 7 || c == 9;
            return o;
        }
        return o;
    }
    std::set<uint32_t> default_weapons()
    {
        std::set<uint32_t> s;
        for (const auto& b : kBuiltin)
            if (b.listed) s.insert(b.sig);
        return s;
    }

    // Defaults. The pouch: on the belt at the front of the left hip (the sidearm's hip mirrored, a little forward); its zone as large as the
    // sidearm's (0.16) less a little, the hand comes down to it without looking. The magazine's top held 7 cm above the left controller's
    // grip point (a fist round the magazine's body, its feed lips above the thumb). The well's travel: a STANAG magazine goes ~5 cm up into
    // the well before it latches; the AR's charging handle travels ~7 cm (6 asked, the hand's point lags the handle).
    constexpr float kPouchDefault[3]{ -0.22f, 0.60f, 0.02f };
    constexpr float kPouchZoneDefault = 0.14f;
    constexpr float kHandMagDefault[3]{ 0.0f, 0.07f, -0.01f };
    constexpr float kTravelDefault = 0.05f, kRackDefault = 0.06f;
    constexpr int   kTapDefault    = 250;    // ms: a quick press of B drops the magazine; longer, it is the game's Y (heal)
    // The well (2 Nov test: magazines engaged 4.9 cm off the axis and up to 39 deg from it, one already at its seat, so the slide started
    // beside the well or clicked at once): only from below, close to the axis, nearly upright.
    constexpr float kEngageLat     = 0.05f;  // the magazine's top within this of the well's axis engages it ...
    constexpr float kEngageBelow   = 0.08f;  // ... from this far below the start of its travel ...
    constexpr float kEngageTop     = 0.6f;   // ... up to this much of the travel in (not at the seat: it must slide in)
    constexpr float kEngageCos     = 0.64f;  // ... standing within 50 deg of the well's axis
    constexpr float kLeaveLat      = 0.08f;  // pulled this far off the axis (or back below the engage range + 2 cm): out of the well
    // reloadwidewell (author 5 Nov: the flamethrower's zone; 14:05:47: 3.5 s to find it, in at 2 cm off the axis, 31 deg): a big tank and a long rocket are taken further from the axis, further out, at any depth and turned up to 70 deg.
    // reloadwellzone (author 5 Nov): the values above raised (until now 3.5 cm, 6 cm, 40 %, 40 deg, 6 cm), an AK's further: a curved magazine, tilted in.
    constexpr float kAkEngageLat = 0.075f, kAkEngageBelow = 0.11f, kAkEngageCos = 0.42f, kAkLeaveLat = 0.11f;
    // reloadbeltzone (author 5 Nov: M249; 16:34:21-50: let go 1, 4, 5 cm from the well and turned 136-153 deg by the game's hold, or 17 cm under it on its axis, never taken, or taken and pulled out):
    // a belt or box is laid in the open tray, not slid along a line: brought this near with the cover open, at any turn, it goes in.
    constexpr float kBeltLoadZone = 0.20f;
    constexpr float kDoorLoadZone = 0.25f;   // reloadratdoor: the hand this near the open back, a missile in it: it goes in
    constexpr float kWideEngageLat = 0.08f, kWideEngageBelow = 0.12f, kWideEngageCos = 0.34f, kWideLeaveLat = 0.12f;
    constexpr float kHandleZone    = 0.10f;  // the left grip pressed this close to the charging handle takes it (when it must be racked) ...
    constexpr float kHandleZoneIdle = 0.07f; // ... or this close otherwise (a pull that only moves it), not to take a support grip for it
    constexpr float kSlideZoneIdle  = 0.055f; // a pistol's: the support hand cups the gun just below its slide
    constexpr float kRackLineZone   = 0.065f; // reloadrackzone: ... or this close to the handle's parts' line while it must be racked
    // reloadwellzone (author 5 Nov; until now 8, 6, 4.5, 5 cm): its handle stands out on the left, 15 cm ahead of the grip. Not the line when idle: it runs 6 cm from the fore-end, where the support hand is.
    constexpr float kMs16HandleZone = 0.13f, kMs16ZoneIdle = 0.09f, kMs16LineZone = 0.085f;
    // All-weapons pass (8 Nov, simulator): the MP5's cocking handle sits on its fore-end, 4-6 cm from where the game draws the support hand,
    // so the left grip put on the fore-end took the handle (a pull that only moves it) instead of the two-handed hold. Idle (nothing to rack),
    // the MP5, MP5SD and MP5K's handle is taken this close only; when it must be racked, kHandleZone and kRackLineZone as on every gun.
    constexpr float kMp5ZoneIdle = 0.035f;
    constexpr float kBusySecs      = 3.2f;   // the game's reload kept out of the view at most this long (the AR-C's longest: 2.75 s)
    // A pump shotgun (kTube): the shell's top held 4 cm above the left controller and 4 cm forward (between thumb and fingers); it goes up
    // into the loading port 0.6 times reloadtravel (3 cm); the pump comes back 1.3 times reloadrack (7.8 cm); the left grip takes the pump
    // within 12 cm of its centre (the support hand holds it anyway).
    constexpr float kShellHand[3]{ 0.0f, 0.04f, -0.04f };
    constexpr float kRoundHand[3]{ 0.0f, 0.03f, -0.05f };   // a revolver's round: its nose 3 cm above the left controller, 5 cm ahead
    constexpr float kPumpZone      = 0.12f;
    constexpr float kPumpFwd       = 0.2f;   // after a full pull, back under this much of the stroke: the pump has gone forward
    // reloadpumpgrip (author 7 Nov 20:35; his log 21:28-21:30, the M133 29259): six left grip
    // presses on the fore-end, 0.08-0.12 m from the two-handed hold's point, were all the two-handed hold and never the pump: the pump was
    // taken only within kPumpZone of its parts' centre, (-0.050, -0.028, -0.164) in the controller's axes, which on the M133 is 36.5 cm of
    // pump and action bars, so its centre sits back by the receiver, 15-17 cm behind where the hand holds the fore-end (reloadhandgrabw
    // -0.308). Every cycle went by quick right B. Now a press within kPumpGripZone of the whole part's line takes it, and the fore-end
    // already held (the grip kept from before the shot) takes it without a new press once the chamber needs it. The stroke is the left hand
    // coming back towards the weapon hand (the distance between the controllers), not along the right controller's axis: with two hands the
    // gun points from one hand to the other, and a turn of the right wrist moved the left hand along that axis without any stroke.
    constexpr float kPumpGripZone  = 0.10f;

    // manualreload43 (author 3 Nov 14:45): the
    // defaults are the author's saved settings of 3 Nov: reloadmanual, reloadweaponsounds and reloadgamehold on (were off).
    std::atomic<bool>  g_on{ true };
    std::atomic<bool>  g_log{ true };
    std::atomic<int>   g_key{ 0 };            // reloadkey: 0 keyboard R, 1 pad X
    std::atomic<int>   g_tap_ms{ kTapDefault };
    std::atomic<float> g_pouch[3]{ kPouchDefault[0], kPouchDefault[1], kPouchDefault[2] };
    std::atomic<float> g_pouch_zone{ kPouchZoneDefault };
    std::atomic<float> g_hand_off[3]{ kHandMagDefault[0], kHandMagDefault[1], kHandMagDefault[2] };
    std::atomic<float> g_travel{ kTravelDefault }, g_rack_len{ kRackDefault };
    std::atomic<bool>  g_model_on{ true };
    std::atomic<bool>  g_model_shown{ false };
    std::atomic<int64_t> g_scan_until{ 0 };
    std::atomic<int>   g_force{ 0 };          // reloadforce (test): 1 dry, 2 out, 3 full; consumed by the next active poll

    std::mutex                          g_list_mtx;   // reloadweapons, reloadgeom, reloadmagmesh (the pad poll, F11, the render thread)
    std::set<uint32_t>                  g_weapons{ default_weapons() };   // the AR-C and the pistols of kBuiltin
    std::map<uint32_t, Geom>            g_geom{ { kArc, Geom{} } };
    std::map<uint32_t, std::vector<MagPart>> g_parts;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_handle_parts;   // reloadhandlepart: weapon -> (draw's index count, bone or kRigidBone)
    std::atomic<int>   g_handle_next{ 0 };
    std::atomic<bool>  g_pouch_shown{ false }, g_handle_shown{ false };
    std::atomic<bool>  g_drop_fall{ true };
    // reloadpouchneck on|off (manualreload21; saved, default off; author 2 Nov 21:34):
    // the hip's magazine hung from the neck (below and behind the eyes, as the shoulders are) and on its own heading, which follows the head's
    // only past 60 deg, so looking down or aside at the hip no longer moves it. Off: from the eyes, on the body's heading (35 deg), as before.
    std::atomic<bool>  g_pouch_neck{ false };
    // reloadpouchanchor on|off (manualreload49; saved, default on; author 3 Nov 19:08; testruns\2026-11-03_hipmag: the head still, the right controller turned 45 deg right moved the hip's magazine 4.8 cm, 30 deg
    // down 10.2 cm, a move of 20 cm under 1 px): the game camera swings on the neck as the gun turns, viewpivot keeps the eyes out of that swing
    // and gun_anchor_apply moves the gun, the hands, the sling, the sidearm's hip and the chest copies with the eyes, but not the magazine pouch,
    // which stayed on the swinging camera. On: the pouch goes with the eyes too. Off: as before (a test).
    std::atomic<bool>  g_pouch_anchor{ true };
    // reloadrockin <deg> (manualreload23; saved, default 0: off; author 2 Nov 21:40): an AK's magazine goes in as the game's does, its front lug hooked first, tilted this far (its bottom forward), and rocked back
    // up into the well as it is pushed home.
    std::atomic<float> g_rock_deg{ 0.0f };
    constexpr float    kRockGameDeg = 20.0f;   // reloadgamehold on, reloadrockin 0: the AK's magazine rocked in about as the game's reload does
    // reloadrevdump on|off (manualreload31; saved, default on; author 3 Nov 08:10): the
    // revolver's fired cases drop out as its cylinder swings open (its ejector pushed at once). Off: only with the muzzle tipped up.
    std::atomic<bool>  g_rev_dump{ true };
    // A pump being pulled by the left hand (manualreload31; author 3 Nov 08:10): the
    // two-handed hold's left hand then follows its controller (FarCry5Stereo.cpp), which is on the pump.
    std::atomic<bool>  g_pump_now{ false };
    std::atomic<bool>  g_pump_grip{ true };    // reloadpumpgrip on|off (saved, default on): above, kPumpGripZone
    std::atomic<bool>  g_pump_aim{ false };    // reloadpumpgrip: a stroke under way: the gun keeps its two-handed direction on the weapon hand (FarCry5Stereo.cpp)
    float              g_pump_d0 = 0.0f;       // reloadpumpgrip: the controllers' distance with the pump forward (the stroke's start)
    clock::time_point  g_pump_d_t{};
    // manualreload38 (author 3 Nov 11:54): a magazine, shell or round from the hip in the left hand (the pad poll's g_hand_mag, for the
    // render thread's two-handed hold).
    std::atomic<bool>  g_mag_hand_now{ false };
    // reloadseathold on|off (manualreload43; saved, default on; author 3 Nov 14:38): the left grip that brought a shell, round or magazine in stays the reload's until it is let
    // go. Until now the grip went back to the pad as the shell seated, still held: a new press for the two-handed hold (FarCry5Stereo.cpp), 
    // and a shotgun's loading port is within its 25 cm of the fore-end, so the hand took the gun at once.
    std::atomic<bool>  g_seat_hold_on{ true };
    // reloadroundslide on|off (manualreload43; saved, default on; author 3 Nov 14:45): his log of 15:32 shows three rounds taken, one in the chamber's mouth at 40 deg (the limit)
    // and 2.3 cm short of its seat, let go 2 cm short; two never in (3 and 5 cm off the axis, over the 3.5 cm). The last 3.5 cm of the push
    // brings the left controller against the right hand, just behind the open cylinder. So a round is taken by its chamber within 5 cm of
    // its axis and 55 deg, anywhere along its travel, and then drops home by itself in 0.15 s, as a round let fall into an open cylinder.
    std::atomic<bool>  g_round_slide{ true };
    constexpr float    kRoundEngageLat = 0.05f, kRoundEngageCos = 0.57f, kRoundSlideMs = 150.0f;
    bool               g_seat_hold  = false;   // the pad poll: the grip held since its shell, round or magazine went in
    bool               g_hand_was   = false;   // the pad poll: g_hand_mag at the last poll
    // bheal on|off (manualreload38; saved, default on; author 3 Nov 11:54): right B never taps the pad's Y (the game's previous weapon).
    // A press is held back for reloadtap ms (quick: nothing, or with manual reload the magazine drop); held longer it is the pad's Y, kept down
    // at least bhealms ms from when it starts, so the game sees a hold (heal), never a tap (switch). A throw (left grip + B) sends no Y.
    // Off: right B is the pad's Y as before (tap: previous weapon; held: heal).
    // The kept round (manualreload39 / manualreload45, author 3 Nov; the switch reloadkeepone retired 9 Nov, always on): now only for the guns the
    // ammo ledger does not cover (below), the pump and lever shotguns: the mod counts their shells itself, the game's clip is kept one above them
    // (a shell loaded by hand fills it, silently), and the gun does not fire with one left in the game's clip while there is a reserve: load
    // another first. The M79 and the rockets keep no round (reloadgrenadefill / reloadrocketfill fill their clip at 0 instead).
    // reloadfullmag (bAutoReload written 0 in the gun's data) retired 9 Nov: it never stopped the game's reload at empty (research\2026-11-08_ammosystem
    // \TEST.md round 1: 580 blocks written, the game still reloaded); the ledger does it instead.
    // noautoswitch on|off (saved, default on; author 7 Nov 08:53; his log
    // of 09:50:55: the AR-C's last round fired with no reserve, clip 0 reserve 0, and 260 ms later the game put the bow in his hands by itself):
    // reloadkeepone's kept round also with no reserve left, so the game's clip never reaches 0 with nothing to reload and the game never
    // switches weapon by itself. That last round does not fire (the wrist shows 0); picked-up ammo makes the gun fire again as usual. Only
    // while reloadkeepone is on and with the manual reload's guns (not the double barrels, the M79 nor the RPG, which keep no round). Off: the
    // last round fires and the game switches to another weapon, as before.
    std::atomic<bool>  g_no_switch{ true };
    // The ammo ledger (author 8 Nov 20:51; research\2026-11-08_ammosystem\DESIGN.md "top-up ledger"; always on since 9 Nov 08:28,
    // Author, its switch ammoledger retired). For magazine and belt guns, revolvers and break actions of 2 or more (not tubes, the M79, rockets or bows): the game's clip is kept
    // from ever reaching 0 by silent top-ups (RefillPocketedWeaponClip, nothing taken from the reserve) when it falls to 1 with a real round left,
    // so the game never starts its reload and no round is kept back (reloadkeepone, its "filled again" and keep_last are off for these guns).
    // The rounds a top-up adds are the gun's surplus: a magazine gun's reload logic sees the game's clip less its surplus (the real count: the
    // wrist shows it at full size, the trigger stops at 0, the handle is owed at 0); a revolver's and break action's count stays its chambers.
    // The reserve: a magazine's load (the fill at the drop) takes what the fill put in plus the surplus it replaced (at most the reserve), so
    // exactly what the magazine was missing; a round loaded by hand into a chamber takes 1; a top-up takes nothing. A rack's thrown round
    // becomes surplus. The trigger is held while the game's clip is at 1 with a real round left and its top-up not landed (a failed top-up:
    // that round stays held, as reloadkeepone did, and the top-up is asked again every 2 s), so the game's clip never reaches 0.
    std::atomic<bool>  g_led_now{ false };
    std::atomic<int>   g_led_real{ -1 };
    // The one-shot hold (coordinator 9 Nov: a full-auto gun at 900 rpm fires every ~65 ms, a long frame could let one more through): with one
    // real round left the hold is armed, and the player's next shot (the game's shot code, FarCry5BulletSpeed.cpp) holds the trigger at once
    // (ModConfig's pad output: RT 0), not at the next clip read. Cleared by the poll at 0 (the empty gun's own hold) or above 1 (a load).
    std::atomic<bool>  g_led_arm{ false }, g_led_shot_hold{ false };      // the ledger's real count this poll (ledger_low: the game's clip read every update while it is low)    // this poll: the gun in hand is the ledger's (keep_one() is off for it)
    std::atomic<bool>  g_led_hold{ false };   // the trigger held: the game's clip at 1, its top-up not landed
    struct Ledger
    {
        int               surplus = 0;    // rounds in the game's clip from top-ups (and a rack's thrown rounds): not real
        int               fired   = 0;    // rounds fired since the last load (the game's clip going down), logged when the gun is empty
        int               real_last = -1;
        int               g_last  = -1;   // the game's clip at the last poll
        bool              topup   = false, absorb = false;
        clock::time_point t{};
    };
    std::map<uint32_t, Ledger> g_ledger_st;   // per weapon sig (g_mtx)
    // reloadbutton on|off (manualreload48; saved, default on; author 3 Nov 19:55): the reload button (left Y, the pad's X) still goes to
    // the game, and its reload is shown as the game plays it (not kept out of the view). Whatever the hands were doing is dropped (a magazine
    // or shell in the left hand, a handle being racked, the cylinder or feed cover open), and the weapon counts as loaded from the game's clip:
    // its magazine seated and chambered; a pump's tube and chamber, a revolver's chambers, as the clip grows. Reloads the game would start by
    // itself stay held back (reloadkeepone). Off: the reload button is the game's as before and the manual reload's count does not follow it.
    std::atomic<bool>  g_btn_on{ true };
    std::atomic<bool>  g_btn_down{ false }, g_btn_edge{ false };   // note_reload_button (the pad poll, ModConfig.cpp)
    std::atomic<int64_t> g_btn_test_until{ 0 };                    // reloadbuttontest
    std::atomic<bool>  g_btn_now{ false };                         // button_reload(), for the render thread
    std::atomic<bool>  g_rec{ false }, g_game_hand{ false };       // reloadrec, reloadgamehand (reloadmatch; tests, not saved)
    std::atomic<bool>  g_hand_game{ true };                        // reloadhandgame (reloadmatch; saved)
    // reloadnogame (author 4 Nov: the drop played the whole reload's sounds, the revolver's barrel
    // out of nowhere, the AR-C's rounds at the well; the switch retired 9 Nov, always on: the ammo ledger's loads need it): where the manual reload tapped the game's reload to fill its clip
    // (hidden, but its sounds, its hands and some of its parts still showed), the clip is filled by the game's own call with no reload. The buttons that learn from the game's reload
    // ("Something wrong") still tap it; a fill that fails falls back to the game's reload by itself (clip_fill_failed / clip_fill_short).
    bool               g_tap_real = false;                         // this tap is to be seen and learnt: the real reload
    // reloadbuttonguard on|off, reloadbuttonguardms N (manualreload51; saved, default on, 500; author 3 Nov 21:59): while a manual step is under way (a magazine, shell or round in the
    // left hand or going in, the handle or pump held, the magazine out of the gun, the cylinder or feed cover open, a lever moving, and
    // kGuardTailMs after), a press of left Y reaches the game (its reload, or its pick-up) only once held reloadbuttonguardms. A thumb
    // brushing it while the left hand works does nothing; a deliberate hold still reloads. Decided per press. Off: every press, as before.
    std::atomic<bool>    g_guard_on{ true };
    std::atomic<int>     g_guard_ms{ 500 };
    // reloadbuttonguardall on|off (manualreload55; saved, default on; author 4 Nov 11:37):
    // the guard above at all times while a gun of the manual reload is in hand, not only during a manual step. His log of 12:32-12:33 had
    // 12 presses of left Y in 80 s, none during a step, so each one went to the game at once and played its full reload, 4 of them with the
    // clip already full (8 of 8 on the 1911). A short press now does nothing (logged); held reloadbuttonguardms (0.5 s) it reloads, or picks
    // up, as before. Off: only during a manual step (manualreload51). Default off since author 5 Nov 07:38: a press reloads, unless
    // the left hand is at work (the guard above) or the clip is already full (his 4 presses at 8 of 8).
    std::atomic<bool>    g_guard_all{ false };
    std::atomic<bool>    g_clip_full_now{ false };   // a magazine gun with its magazine in, racked, its clip at the largest seen
    std::atomic<int64_t> g_step_ms{ -1000000 };   // the last pad poll a manual step was under way (steady clock, ms)
    constexpr int64_t    kGuardTailMs = 500;
    constexpr float    kBtnMaxMs   = 8000.0f;   // a button reload followed at most this long (a pump's 8 shells, one at a time)
    constexpr float    kBtnQuietMs = 1500.0f;   // ... and over once its clip has not grown for this long
    // reloadnostuck on|off (allreloadfire; saved, default on; author 3 Nov 16:25): a weapon the mod holds from firing as empty (a shotgun's chamber and tube, the chambers of a revolver,
    // a double barrel, the M79 or the RPG-7, a charging handle or bolt still to work after a fresh magazine) while the game's clip has rounds
    // is loaded from the game's clip: at once when nothing can go in by hand (its well, loading port or muzzle never measured), else after
    // kStuckSecs with nothing done towards the reload (no round, shell or magazine in the hand, nothing being racked, the game not reloading).
    // Off: the hold stays until the weapon is loaded by hand.
    std::atomic<bool>  g_no_stuck{ true };
    constexpr float    kStuckSecs = 6.0f;
    constexpr float    kPendingMaxSecs = 12.0f;   // reloadnostuck: the game's reload tapped and its clip not grown: not waited for after this
    // reloadhighlight (manualreload40; saved, default on): the boxes measured per weapon this session (g_list_mtx).
    std::atomic<bool>  g_hl_on{ true };
    std::atomic<bool>  g_hint_glow{ true };   // reloadhintstyle glow|boxes (saved, default glow; FarCry5ReloadHint.cpp)
    // reloadhandsnap on|off (manualreload41; saved, default on; author 3 Nov 12:37): the drawn free hand holds the part it works where that part is
    // drawn (FarCry5ReloadMag.cpp, reload_hand_snap), not at its controller.
    std::atomic<bool>  g_hand_snap{ true };
    // reloadhandgrab on|off (manualreload44; saved, default on; author 3 Nov 14:44): with
    // reloadhandsnap, the hand pulling a charging handle, slide or pump is drawn at one place on it per weapon, wherever the grip was pressed
    // (until now: where it was when the pull began). The place is the left controller's at rest on the handle (the gun controller's axes, m,
    // right-handed): learnt from the game's own reload from empty (reloadhandgrabw, saved), else built in, else from the handle point with the
    // AR-C's own grip (its game reload, 2 Nov runs: the controller 4.4 cm left, 3.1 cm up, 6.3 cm behind the handle; a pump: 3.5 cm under it).
    std::atomic<bool>  g_hand_grab{ true };
    // manualreload46 (author 3 Nov 16:31):
    //  - reloadroundpinch on|off (saved, default on): a revolver's round or a shotgun's shell from the hip is pinched between the thumb and
    //    the index tips (FarCry5Fingers.cpp kRound), its base at the pinch, in front of the fist (until now: in the fist, its nose 3 cm
    //    above and 5 cm ahead of the controller, the fingers round a pistol magazine).
    //  - reloadmagpalm on|off (saved, default on): a magazine is held in the whole hand, its body in the fist: the fist (the controller) at
    //    the middle of the magazine's length, 7 cm at most below its top, measured on the gun's magazine (its highlight box). Until now with
    //    reloadgamehold the game's own hold, at the magazine's bottom (its top 10 to 23 cm above the controller: held by its base).
    //    Before the magazine is measured: reloadhandmag (default 7 cm, a fist round the body).
    std::atomic<bool>  g_round_pinch{ true }, g_mag_palm{ true };
    // reloadmaglower on|off (saved, default on; author 5 Nov): a magazine gun's magazine in
    // the fist, round the middle of its lower half, the fingers closed round it; not the game's hold (reloadhandgame, FarCry5ReloadMatch.h; its fingers, FarCry5ReloadFingers.h, or learnt). Not a box, a belt or a
    // canister (the M249, the M60s, the flamethrower), a shell or a round. Off: the game's hold.
    std::atomic<bool>  g_mag_lower{ true };
    constexpr float    kPinchAt[3]{ 0.0f, 0.025f, -0.07f };   // the thumb and index tips' pinch from the left controller (its raw axes, m)
    constexpr float    kRoundPinchLen = 0.03f, kShellPinchLen = 0.05f;   // the pinch at the round's base: its nose this far ahead; a shell's top
    constexpr float    kPalmAt[3]{ 0.0f, 0.0f, -0.01f };      // the fist's middle, round a magazine's body
    std::map<uint32_t, glm::vec3> g_grab_hold;   // reloadhandgrabw (g_hold_mtx)
    struct GrabDefault
    {
        uint32_t sig;
        float    c[3];
    };
    constexpr GrabDefault kGrabBuiltin[]{
        { 57819, { -0.090f, 0.078f, 0.132f } },   // AR-C: runlogs 3 Nov 13:32, two game reloads from empty (18 frames each, 8.6 and 7.9 cm from the handle point)
    };
    constexpr float kGrabPinch[3]{ 0.044f, -0.031f, -0.063f };   // the handle point from the left controller (gun controller's axes), the AR-C's
    constexpr float kGrabPump[3]{ 0.0f, 0.035f, 0.0f };          // a pump's centre from the left controller (the fist round it)
    // manualreload54 (author 3 Nov 21:46; 4 Nov 08:48, "finish with the grip and magazine placement fix"):
    //  - reloadslidegrab x y z | default (saved; F11 Manual reload): a handgun's slide with no grip learnt or built in is gripped at its rear
    //    from above, the fist round the serrations: the handle point from the left controller (gun controller's axes, m). Until now the AR-C's
    //    grip, which put the hand about 4 cm above and 9 cm behind a pistol's slide, open in the air (testruns 3 Nov, P226 and P08 f_handle).
    //    Default: 4.4 cm in from the controller as the AR-C's (the fist's middle is beside the controller), 0.5 cm under it, 1 cm ahead.
    //  - reloadmagfist on|off (saved, default on; F11 Manual reload, under "Magazine in the whole hand"): a handgun's magazine in the middle of
    //    the fist sideways. The fist's middle is beside the controller, toward the other hand, by as much as the gun's grip is beside the other
    //    controller (its well, mirrored: 4.0 to 5.8 cm for the pistols). Until now on the controller's line, so the magazine sat beside the
    //    fist, outside it (the P226, M9 and 6P9 c_hand pictures), and the P08's ran along the outside of the hand.
    constexpr float    kGrabSlideDefault[3]{ 0.044f, -0.005f, -0.010f };
    std::atomic<float> g_slide_grab[3]{ kGrabSlideDefault[0], kGrabSlideDefault[1], kGrabSlideDefault[2] };
    std::atomic<bool>  g_mag_fist{ true };
    bool grab_plausible(const glm::vec3& c, const glm::vec3& handle) { return glm::length(c - handle) <= 0.12f; }
    std::string grab_line()   // reloadhandgrabw's saved form (call with g_hold_mtx held)
    {
        std::string s = "reloadhandgrabw";
        for (const auto& [w, v] : g_grab_hold) s += fmt::format(" {}:{:.3f},{:.3f},{:.3f}", w, v.x, v.y, v.z);
        if (g_grab_hold.empty()) s += " none";
        return s;
    }
    std::map<uint32_t, std::array<HlBox, kHlCount>> g_hl;
    constexpr int      kHealMsDefault = 700;
    std::atomic<bool>  g_b_heal{ true };
    std::atomic<int>   g_heal_ms{ kHealMsDefault };
    std::mutex         g_heal_mtx;              // the game's pad polls and the virtual pad's updates both ask
    bool               g_hb_down  = false;      // right B down at the last ask
    bool               g_hb_throw = false;      // this press is (or became) the throw
    bool               g_hb_y     = false;      // this press is the pad's Y now (or its minimum hold still runs)
    clock::time_point  g_hb_t0{}, g_hb_y0{};
    std::atomic<float> g_drop_floor{ 1.6f };
    std::atomic<uint32_t> g_ejects{ 0 };   // rounds ejected by a pull of the handle (FarCry5ReloadMag.cpp draws each)
    std::atomic<bool>  g_pump_shot{ true };   // reloadpumpshot: a pump shotgun's pump must be worked after each shot
    // reloadcasinghide on|off (manualreload51; saved, default on; author 3 Nov 21:59): with a gun whose fired shell leaves by the hand (a pump or lever shotgun with reloadpumpshot, a bolt action with
    // reloadbolt), the game's own spent-casing effect is hidden while it is in hand (FarCry5Stereo::casing_hide_tick). The game still throws
    // its casing as it cycles the gun by itself after the shot, and the mod throws the shell again when the pump, lever or bolt is worked:
    // two shells. Off: both, as before.
    std::atomic<bool>  g_casing_on{ true };
    std::atomic<bool>  g_casing_now{ false };  // hides_game_casings()
    std::atomic<float> g_cover_deg{ -70.0f };  // reloadcoverdeg: a belt-fed gun's feed cover lifted this far about its hinge (negative: its back up)
    // reloadbeltgrip on|off (manualreload56; saved, default on; author 4 Nov 15:52): a belt-fed gun's
    // feed cover, open with a belt seated in the tray, is closed by a new left grip press near the tray (kCoverZone), and the charging handle
    // must then be racked after every belt, not only after running dry. Quick right B still closes it too. Off: quick right B closes the cover,
    // and the handle is racked only after running dry (manualreload12).
    std::atomic<bool>  g_belt_grip{ true };
    constexpr float    kCoverZone = 0.15f;      // the left grip pressed this close to the tray closes the open cover (reloadcoverpush off)
    // reloadcoverpush on|off (saved, default on; author 5 Nov 22:47):
    // with reloadbeltgrip, the open feed cover glows (the handle's blue) once a new belt is in; the left grip near it (kCoverPushZone from the
    // tray or from the point kCoverAbove over it) takes it, and it follows the hand down: pushed kCoverPushTravel down from where it was
    // taken, it shuts (then rack the handle). Let go before that and it springs back up. Quick right B no longer shuts it over a new belt.
    // Off: as before (a left grip near the tray, or a quick B, shuts it at once).
    std::atomic<bool>  g_cover_push{ true };
    constexpr float    kCoverPushZone   = 0.25f;   // the left grip this near the tray, or the point kCoverAbove over it, takes the cover
    constexpr float    kCoverAbove      = 0.10f;
    constexpr float    kCoverPushTravel = 0.10f;   // the hand this far down (along the gun's up) from where it took the cover: shut ...
    constexpr float    kCoverShutAt     = 0.08f;   // ... at this much of its lift left
    bool               g_cover_hold = false;       // the cover in the left hand
    float              g_cover_f = 1.0f, g_cover_f0 = 1.0f, g_cover_h0 = 0.0f;
    int                g_cover_tick = 10;
    // reloadratslide on|off (saved, default on; author 5 Nov 22:47: the RAT4's): with the back open,
    // the missile's nose (or the hand) brought within kRatEntryZone of the back opening (the seated missile's tail: its whole length behind
    // the seated nose) engages it on the tube's axis, and it goes in through the back with the hand pushing forward: kRatPush in, it slides
    // home by itself (kRatAutoSecs; let go then, it still goes home). Pulled back (kRatPullOut) or off the line (kRatLeaveLat): out again.
    // Off: as before (brought near the back or the seat, it is in at once).
    std::atomic<bool>  g_rat_slide{ true };
    constexpr float    kRatEntryZone = 0.22f, kRatPush = 0.15f, kRatPullOut = 0.12f, kRatLeaveLat = 0.20f, kRatAutoSecs = 0.30f, kRatEngageCos = 0.17f;
    bool               g_rat_on = false, g_rat_auto = false;   // engaged by reloadratslide; sliding home by itself
    glm::vec3          g_rat_h0{ 0.0f };
    float              g_rat_i0 = 0.0f, g_rat_auto_i0 = 0.0f;
    clock::time_point  g_rat_auto_t{};
    // reloadlauncherfill on|off (saved, default on; author 5 Nov 22:47: the big launcher's missile): a one-shot launcher (M79, RPG-7, RAT4) taken in hand with an empty clip: the game draws no round
    // then, at the hip or on the gun (reloadrocketfill's finding after a shot), until its own automatic reload. Its clip is filled at once
    // (reloadnogame: no reload of the game's), so the round shows at the hip, and the mod's count keeps the chamber empty: loaded by hand.
    std::atomic<bool>  g_launcher_fill{ true };
    // reloadhiploaded on|off (saved, default on; author 6 Nov 12:40: the M79's grenade, the RAT4's
    // rocket not shown on picking the weapon up): both were picked up loaded (clip 1), and the hip's round was drawn only while a
    // chamber was empty (his log: "the hip's magazine wanted in 172, drawn in 0"). On: a one-shot launcher with a spare in the reserve shows
    // it at the hip loaded too, and after a shot before it is opened (ReloadMag). Off: only with an empty chamber to load, as before.
    std::atomic<bool>  g_hip_loaded{ true };
    // reloadgrenadefill on|off (saved, default on; author 6 Nov 12:40: the M79;
    // his log 13:30:47.0 "shot (clip 1 -> 0)", the game's own reload then, its clip back at 48.8): reloadrocketfill (the clip filled at once
    // after the shot that empties it, so the game starts no reload of its own) for the M79 too. Its fired grenade stays in the chamber in the
    // mod's count (fired, not live: it fires again only once loaded by hand; reloadrigidspent hides it). Off: the RPG-7 and RAT4 only.
    std::atomic<bool>  g_grenade_fill{ true };
    // reloadgrenadeseat on|off (saved, default on; author 6 Nov 12:40: the M79's grenade; final_m's M79_open
    // picture: the grenade beside the open breech, not in it): the game's grenade at rest sits off the bore, and the mod's grenade on the gun
    // is pinned where the game's sat. Measured once on the model (ReloadMag): the barrel's back ring gives the bore's centre and the breech
    // face; the grenade is moved so its middle is on the bore and its base at the breech face. Off: where the game's sits (at the next pick-up).
    std::atomic<bool>  g_grenade_seat{ true };
    // reloadsamegun on|off (saved, default on; author 6 Nov 12:40): the arcade map's RAT4 is seen as
    // 80077 or 69646 (weaponidsplit: its second largest piece changes) and for a frame or so as 28497. Each change reset the reload: after a
    // shot, the clip reloadrocketfill had filled read as a loaded launcher (13:30:30.9 "1 live round(s)"), and the pouch then refused a
    // missile ("every chamber holds a round"). On: 69646 is taken as 80077 (the same draws), and a change among the three keeps the state.
    std::atomic<bool>  g_same_gun{ true };
    // reloadshellhip on|off (saved, default on; author 6 Nov): a break action's spare shell at the hip was placed with its nose 3 cm above the pouch's centre, lying along the body's
    // heading, so the shell itself sat behind and above the zone the left grip tests (a ball of reloadpouchzone round the pouch's centre;
    // his 13:24 takes at 13 and 9 cm from it). On: the middle of the shell (its vertices' extent on the model) on the zone's centre, the
    // same heading. Off: as before. The double barrels only (Sledge, sawed-off); the take's log line gives the hand's offset from the centre.
    std::atomic<bool>  g_shell_hip{ true };
    bool same_rat(uint32_t a, uint32_t b)
    {
        auto rat = [](uint32_t s) { return s == 28497u || s == 80077u || s == 69646u; };
        return g_same_gun.load() && a != b && rat(a) && rat(b);
    }

    // The reload's sounds are the game's own only (manualreload50b, author 3 Nov 22:07):
    // the mod's own clicks (reloadsound, reloadsoundvol, bin\fc5vr_reload_<name>.wav) are gone; both commands are still read from old
    // settings files and do nothing. A step with no game sound for the weapon is silent until its sounds are learnt from the game.
    enum Snd { kSndMagOut, kSndMagIn, kSndRackBack, kSndRackFwd, kSndEject, kSndRoundLand, kSndCount };
    // reloadgamesounds <mag_out> <mag_in> <rack_back> <rack_fwd> <eject> <round_land> (2 Nov, saved; each id or id:type, type 15 if left out; 0: none; author 13:29
    //): a sound with an id is the game's own (its Wwise event, FarCry5GameSound.cpp), played by the game; the ids and types come from soundwatch.
    std::atomic<uint32_t> g_game_snd[kSndCount]{};
    std::atomic<int>      g_game_snd_type[kSndCount]{};
    constexpr const char* kSndName[kSndCount]{ "mag_out", "mag_in", "rack_back", "rack_fwd", "eject", "round_land" };

    // reloadweaponsounds on|off (manualreload11; saved, default on since manualreload43; F11 Weapon > Manual reload > Sounds; author 2 Nov 15:17): each weapon's own game reload sounds. The arcade map's
    // sound run (soundwatch, 2 Nov 17:15, research\weapondata\weapon_sounds.csv) logged each weapon's reload as the game plays it: a timed list of
    // sounds (type 15) on its animation; the second run (2 Nov 20:19, 46 weapons) added the AR-C (long), MP34, Vector, .308, HTI .50, SVD, SA-50,
    // M249, M60s and flamethrower (manualreload18). All are kept here (seq: the normal reload, then '|' and the reload from empty where it differs). Which one
    // is the magazine going out or in, and which the bolt, is read from their order, their timing against the animation's phases
    // (research\weapondata\reload_data.csv) and the ids shared with other weapons (shared ones are hand and pouch noises): a guess until heard. F11
    // lists the weapon's sounds to play and pick for each event (reloadsoundpick, saved).
    struct WeaponSnd
    {
        uint32_t    sig;
        const char* name;
        uint32_t    def[4];   // kSndMagOut (a revolver's cylinder out; a break action's opening; a belt-fed gun's feed cover lifted), kSndMagIn (a
                              // shell or round in; a belt laid in the tray), kSndRackBack, kSndRackFwd (a pump's or lever's back and forward; a
                              // belt-fed gun's cover closed); 0: none
        const char* seq;      // "id@s ..." from the start of the game's reload
    };
    // manualreload36: the magazine's out and in below are now those reload7's full walk (3 Nov) learnt from the game's own reload (reloadsoundlearn:
    // the sound nearest the moment the magazine left and reached its seat, research\weapondata\reload7_soundlearn_lines.txt) where it learnt them:
    // the 1911, the AR-Cs and M16A1, BZ19, Vector, MP40, MS16, SVD (changed); the MP5s, Skorpion, SMG-11, A-99, AK (confirmed).
    constexpr WeaponSnd kWeaponSnd[]{
        // The 1911: 446562585, 1302339480, 2635244640 and 2330490764 also come with the bow's reload (hand noises). Its own: 2148877110 at the
        // start, 4104195309 between (on 9 magazine weapons: the next magazine from the pouch?), 1980945621 as the clip refills (the magazine in).
        { 24417, "1911", { 1302339480, 1980945621, 0, 0 }, "2148877110@0 446562585@0.25 1302339480@0.38 4104195309@0.55 2635244640@0.89 2330490764@1.01 1980945621@1.27" },
        // The AR-C and the M16A1: the normal reload 3863307842 .. 1879999235 0.84 s apart, as the animation's mag out and mag in (0.83 s); from
        // empty, 2347389021 and 672015488 0.07 s apart late in it: the bolt catch and the bolt going home.
        { 57819, "AR-C", { 36064529, 1879999235, 2347389021, 672015488 },
          "3949169702@0 3240498188@0.15 3863307842@0.28 4246560353@0.39 4119235673@0.52 894646864@0.80 1879999235@1.12 | 3949169702@0 142655700@0.01 4104195309@0.17 "
          "2645125847@0.20 36064529@0.47 1558528152@0.67 2347389021@0.92 672015488@0.99 3949169702@1.40 1879999235@1.66" },
        { 40752, "M16A1", { 36064529, 1879999235, 2347389021, 672015488 },
          "3863307842@0 4246560353@0.13 4119235673@0.27 894646864@0.54 1879999235@0.86 | 3949169702@0 142655700@0.02 4104195309@0.19 2645125847@0.21 36064529@0.51 "
          "1558528152@0.71 2347389021@0.95 672015488@1.00 3949169702@1.43 1879999235@1.69" },
        // The AKs: two pairs, the magazine rocked out (2501554889, 2519555196) and rocked in and latched (2015139108, 3944883295); no bolt in this
        // reload.
        { 45582, "AK47", { 2519555196, 3944883295, 0, 0 },
          "3952672413@0 1530560007@0.15 2501554889@0.43 2519555196@0.53 2015139108@1.14 3944883295@1.22 453559250@1.70 2714586708@1.70 4087086420@1.94" },
        { 60537, "AK-M", { 2519555196, 3944883295, 0, 0 },
          "3952672413@0 1530560007@0.17 2501554889@0.47 2519555196@0.56 2015139108@1.16 3944883295@1.26 453559250@1.74 2714586708@1.74 4087086420@1.97" },
        // The MP5s: the cocking handle locked back first (2623623013, the Skorpion's last sound too), the magazine out and in, the handle slapped
        // home last (3216115768).
        { 37359, "MP5", { 1600661443, 1598804907, 2623623013, 3216115768 },
          "765652777@0 2623623013@0.21 1600661443@0.38 707977784@0.40 4104195309@0.80 707977784@1.35 4255759295@1.35 1598804907@1.47 142655700@1.66 765652777@1.76 3216115768@1.89" },
        { 35709, "MP5SD", { 1600661443, 1598804907, 2623623013, 3216115768 },
          "765652777@0 2623623013@0.23 1600661443@0.39 707977784@0.41 4104195309@0.84 707977784@1.41 4255759295@1.41 1598804907@1.52 142655700@1.71 765652777@1.82 3216115768@1.97" },
        { 40362, "MP40", { 3161353446, 1222288339, 0, 3216115768 },
          "765652777@0 2399997595@0.05 3161353446@0.28 707977784@0.30 4104195309@0.57 4255759295@1.02 707977784@1.04 3173559946@1.24 1222288339@1.37 142655700@1.37 765652777@1.57 3216115768@1.67" },
        { 32325, "A-99", { 3161353446, 1196230858, 0, 227546542 },
          "765652777@0 3998086659@0.09 3161353446@0.19 4104195309@0.49 1558528152@0.90 4255759295@1.22 707977784@1.28 1196230858@1.41 3173559946@1.41 765652777@1.52 227546542@1.62" },
        { 41649, "Skorpion", { 1091223143, 1084418834, 0, 2623623013 },
          "1401029484@0 1091223143@0.19 4104195309@0.40 4183953579@0.58 1558528152@0.77 1084418834@1.09 765652777@1.35 2623623013@1.58" },
        { 35187, "SMG-11", { 4072137171, 1503857625, 0, 0 },
          "4273459077@0 2557073973@0.24 765652777@0.40 | 765652777@0 2399997595@0.38 4072137171@0.60 4104195309@0.95 4255759295@1.30 1503857625@1.54 765652777@1.78" },
        { 32688, "MS16", { 331978632, 3021044961, 2347389021, 1756342197 },
          "3949169702@0 1955069032@0.13 3263990427@0.13 1328308594@0.37 331978632@0.37 3426825925@0.66 1530560007@0.66 707977784@0.70 996595760@0.77 712069802@0.96 "
          "1558528152@1.05 712069802@1.33 3037247292@1.46 2347389021@1.46 707977784@1.48 1756342197@1.59 3021044961@1.61 3244002017@1.92 329984622@1.98 182769431@2.31 3263990427@2.39" },
        // The AR-C (long), an AR-15: its reload is the AR-C's from empty, id for id (the bolt catch 2347389021 and the bolt home 672015488 0.08 s
        // apart); the AR-C's magazine out and in.
        { 62229, "AR-C (long)", { 36064529, 1879999235, 2347389021, 672015488 },
          "3949169702@0 142655700@0.02 4104195309@0.19 2645125847@0.23 36064529@0.52 1558528152@0.74 2347389021@0.98 672015488@1.06 3949169702@1.48 "
          "1879999235@1.76" },
        // The BZ19 (a guess by timing): 1328308594 comes with the MS16's magazine out; 4162193694 just before 1222288339 (0.13 s after the MP40's
        // magazine in); 2623623013 is the MP5's handle locked back; 136858057 its last sound.
        { 39408, "BZ19", { 1328308594, 1222288339, 2623623013, 136858057 },
          "3949169702@0 142655700@0.24 1141761174@0.27 707977784@0.31 1222288339@0.38 813072728@0.48 2623623013@0.53 2929463349@0.53 212395715@0.63 "
          "1328308594@0.63 212395715@0.76 3949169702@0.87 932335367@0.93 1741665042@1.31 3875340163@1.59 4162193694@1.65 1222288339@1.69 "
          "3949169702@1.89 894646864@2.03 136858057@2.05" },
        // The MP34: the MP40's magazine out (3161353446) and in (3173559946), and the MP5's handle slapped home (3216115768).
        { 43215, "MP34", { 3161353446, 3173559946, 0, 3216115768 },
          "3396252413@0 227546542@0.19 142655700@0.23 1389234585@0.30 3161353446@0.30 707977784@0.54 932335367@0.71 2714586708@0.71 1951546418@0.89 "
          "4255759295@1.02 473899547@1.08 3173559946@1.08 2830281633@1.17 3216115768@1.54" },
        // The Vector: 1361332603 (the SA-50's too) as the magazine out, 1558528152 (on 7 weapons, before their magazine in) as the magazine in,
        // the AR-C's bolt catch 2347389021 and 1756342197 0.09 s later (as the MS16's bolt pair).
        { 39750, "Vector .45", { 1361332603, 1756342197, 2347389021, 0 },
          "3949169702@0 2496096951@0.17 142655700@0.19 1361332603@0.26 4104195309@0.56 707977784@0.70 1558528152@0.92 2347389021@1.24 707977784@1.24 "
          "1756342197@1.33 2496096951@1.86" },
        // The bolt rifles (.308, HTI .50): after a shot the bolt back, the case out (230258062, as the 4570's lever) and the bolt home. The .308
        // and the SVD share a magazine family: 685386288 (0.8 s) as out and 2897211156 (1.7-1.9 s) as in.
        { 24474, ".308", { 685386288, 2897211156, 258748968, 828282296 },
          "197801827@0 527343943@0.58 140018523@0.69 685386288@0.87 2897211156@1.90 1787635528@2.26 1584245029@2.84 4082832186@2.84 | after a shot: "
          "837719625@0.47 258748968@0.54 230258062@0.62 828282296@0.79 1877510187@0.92" },
        // The HTI .50 (a guess by timing): 3213135265 as the magazine out, 1854374129 as in; its bolt from the shot's cycle.
        { 42000, "HTI .50", { 3213135265, 1854374129, 1543224786, 338296967 },
          "607201573@0 212395715@0.28 212395715@0.41 4087086420@0.68 3213135265@0.87 3263990427@0.87 3263990427@1.39 1854374129@1.51 2687094087@1.89 "
          "2347389021@2.65 3590713978@2.84 3263990427@3.61 4087086420@3.74 | after a shot: 4087086420@0.41 212395715@0.58 1543224786@0.71 "
          "2714586708@0.75 4155180925@0.79 230258062@0.92 338296967@1.20 212395715@1.57 453559250@1.57" },
        // The SVD: the .308's magazine family; 1674143517 the handle let go late in the reload (a guess).
        { 42555, "SVD", { 63729309, 881012671, 0, 1674143517 },
          "197801827@0 2551167301@0 527343943@0.29 1271967362@0.41 140018523@0.50 707977784@0.52 63729309@0.69 685386288@0.82 2551167301@0.89 "
          "299217318@1.36 1879999235@1.38 2060777317@1.40 765652777@1.43 299217318@1.55 2551167301@1.58 2897211156@1.73 707977784@1.86 "
          "2551167301@1.94 881012671@1.94 3497450392@2.27 1674143517@2.29 299217318@2.39 4082832186@2.78" },
        // The SA-50 (a guess by timing): 1361332603 as the Vector's magazine out, 2680273823 as in, 3518503605 the last sound as the bolt.
        { 23397, "SA-50", { 1361332603, 2680273823, 0, 3518503605 },
          "2710214575@0 36064529@0.19 4287217292@0.59 1361332603@0.68 2482505378@0.73 1057168247@1.13 4255759295@1.77 2680273823@1.91 2786310394@1.97 "
          "3518503605@2.28" },
        // The belt-fed guns (a guess by timing; out: the feed cover lifted, in: the belt laid, forward: the cover closed): 2239810187 at 1.0 s in
        // all three as the cover lifted.
        { 58416, "M249", { 2239810187, 1280606850, 0, 3402396016 },
          "3396618169@0 3497450392@0.44 3944643529@0.49 2239810187@0.97 2914806255@1.36 2740693043@1.92 1280606850@2.56 296586361@3.07 "
          "3362915382@3.20 4087086420@3.42 4242188108@3.65 1931132178@3.65 3402396016@4.01 14396713@4.74 3396618169@4.93 1377005697@5.14" },
        { 61926, "M60", { 2239810187, 3617193354, 0, 2306894516 },
          "3396618169@0 285911188@0.28 270063068@0.36 1706044680@0.36 2239810187@1.00 3396618169@1.27 3617193354@2.03 285911188@2.39 2001244691@2.57 "
          "1706044680@2.77 538817150@2.77 285911188@3.22 2306894516@3.48 3396618169@3.80 285911188@4.35 2038897719@4.76 1802956604@4.99 "
          "418318789@5.22 212395715@5.61 285911188@5.83 3396618169@5.96" },
        { 63120, "M60 (Vietnam)", { 2239810187, 3617193354, 0, 2306894516 },
          "3396618169@0 285911188@0.27 270063068@0.39 1706044680@0.39 2239810187@1.04 3396618169@1.27 3617193354@2.03 285911188@2.37 2001244691@2.56 "
          "1706044680@2.77 538817150@2.77 285911188@3.22 2306894516@3.47 3396618169@3.78 285911188@4.29 2038897719@4.69 1802956604@4.90 "
          "418318789@5.13" },
        // The flamethrower (a guess by timing): the tank off (1387461854, 0.95 s) and latched on (2263686978, 2.40 s); nothing to rack.
        { 47430, "Flamethrower", { 1387461854, 2263686978, 0, 0 },
          "1716510816@0 4046621848@0.21 929710320@0.43 2617050538@0.53 2365769168@0.53 4179622636@0.91 1387461854@0.95 3776927689@1.42 "
          "3567760237@1.74 1141523645@2.13 2700219186@2.32 2263686978@2.40 4098469661@2.87 1716510816@3.10 1636315076@3.20" },
        // The M133s: after each shot the game pumps with 72867933 (back, with 3403294259) and 784439553 (forward); each shell goes in with
        // 2703300039 (635540705 0.45 s later on many weapons: the hand back to the pouch). Heard alike on the modern ones (35499, 29841), the same
        // ids assumed for the others (not in the run).
        { 35499, "M133 (modern)", { 0, 2703300039, 72867933, 784439553 },
          "1120877695@0 2714586708@0.47 2703300039@0.94 635540705@1.41 2703300039@1.75 635540705@2.19 4087086420@3.78 1512702908@3.91 | after a shot: 4087086420@0.35 "
          "72867933@0.48 3403294259@0.48 784439553@0.69 4087086420@0.76" },
        { 29841, "M133 (modern, long)", { 0, 2703300039, 72867933, 784439553 },
          "1120877695@0 2714586708@0.45 2703300039@0.95 635540705@1.43 4087086420@1.48 4087086420@1.61 1512702908@1.61 | after a shot: 4087086420@0.36 72867933@0.47 "
          "3403294259@0.47 784439553@0.67 4087086420@0.75" },
        { 29259, "M133", { 0, 2703300039, 72867933, 784439553 }, "(not in the run: the modern M133's ids)" },
        { 34497, "M133 (short)", { 0, 2703300039, 72867933, 784439553 }, "(not in the run: the modern M133's ids)" },
        { 20586, "M133 (DLC)", { 0, 2703300039, 72867933, 784439553 }, "(not in the run: the modern M133's ids)" },
        // The SPAS-12 loads a shell with 1025465418 (between two 635540705); it is not pumped in its reload, so the M133's pump stands in.
        { 42549, "SPAS-12", { 0, 1025465418, 72867933, 784439553 },
          "212395715@0 2474513108@0.02 4087086420@0.31 635540705@0.53 1025465418@0.85 635540705@1.35 1120877695@1.56 3263990427@1.89" },
        // Not reloaded by hand yet (lever actions and break actions), kept for when they are.
        { 27144, "1887", { 0, 2703300039, 72867933, 784439553 },
          "1120877695@0 1512702908@0.10 1225430372@0.14 72867933@0.25 635540705@0.50 3224516626@0.92 2703300039@0.98 3224516626@1.77 2703300039@1.82 635540705@2.37 "
          "2714586708@4.11 3276664763@4.19 784439553@4.27 4087086420@4.38 | after a shot: 2603587974@0.30 72867933@0.30 1621353718@0.45" },
        { 32472, "1887 (modern)", { 0, 2703300039, 72867933, 784439553 },
          "1120877695@0 1512702908@0.09 1225430372@0.15 72867933@0.26 635540705@0.51 3224516626@0.97 2703300039@1.02 3276664763@1.69 784439553@1.77 4087086420@1.92" },
        { 39012, "4570 (short)", { 0, 813072728, 3389542460, 1752179380 },
          "1120877695@0 1512702908@0.28 813072728@0.50 635540705@0.50 813072728@0.86 3224516626@1.01 1530560007@1.03 4087086420@1.18 813072728@1.37 635540705@1.37 "
          "1636315076@1.44 4087086420@1.72 2714586708@1.77 | after a shot: 3389542460@0.31 230258062@0.37 1752179380@0.42" },
        { 25368, "4570 (long)", { 0, 813072728, 3389542460, 1752179380 },
          "1120877695@0 1512702908@0.28 813072728@0.50 635540705@0.50 813072728@0.87 1530560007@1.04 3224516626@1.04 4087086420@1.21 1636315076@1.45 4087086420@1.73 "
          "2714586708@1.77 | after a shot: 3389542460@0.33 230258062@0.40 1752179380@0.44" },
        { 35490, "SBS", { 607201573, 1709847176, 0, 4217591709 },
          "607201573@0 1095402538@0.06 36064529@0.40 635540705@0.75 1709847176@0.77 526173240@1.00 3643047457@1.11 1277015305@1.60 4217591709@1.74 1025973305@1.86" },
        { 31470, "D2", { 607201573, 1709847176, 0, 4217591709 },
          "607201573@0 1095402538@0.06 36064529@0.43 635540705@0.80 1709847176@0.80 526173240@1.04 3643047457@1.16 1277015305@1.65 4217591709@1.79 1025973305@1.91" },
    };
    const WeaponSnd* weapon_snd(uint32_t sig)
    {
        for (const auto& w : kWeaponSnd)
            if (w.sig == sig) return &w;
        return nullptr;
    }
    std::atomic<bool>     g_wsnd{ true };      // reloadweaponsounds (manualreload43: default on)
    // reloadgamehold / reloadhandmagw (manualreload17): the magazine's top in the left hand per weapon, as the game's hand holds it in its reload.
    std::atomic<bool>               g_game_hold{ true };   // manualreload43: default on
    std::mutex                      g_hold_mtx;
    std::map<uint32_t, glm::vec3>   g_hold_mag;
    // manualreload27: the holds learnt on the arcade map (reload5 run, 2 Nov), built in: used with reloadgamehold on for a weapon with none
    // learnt or set (a reloadhandmagw entry comes first). The magazine's top in the left controller's axes, m.
    struct HoldDefault { uint32_t sig; float top[3]; };
    constexpr HoldDefault kHoldBuiltin[]{
        { 24417, { 0.008f, 0.098f, -0.049f } },    // 1911 (its own magazine part in the game's hand)
        { 32325, { -0.022f, 0.131f, -0.005f } },   // A99
        { 32688, { 0.026f, 0.123f, -0.007f } },    // MS16 (short)
        { 35187, { 0.002f, 0.109f, -0.080f } },    // MAC11
        { 35709, { -0.057f, 0.100f, -0.035f } },   // MP5SD
        { 37359, { -0.059f, 0.095f, -0.039f } },   // MP5
        { 39408, { -0.017f, 0.061f, -0.132f } },   // BZ19 (the helical magazine, held at its rear)
        { 39750, { -0.016f, 0.230f, -0.069f } },   // Vector .45
        { 40362, { -0.079f, 0.124f, -0.007f } },   // MP40
        { 41649, { 0.005f, 0.082f, 0.001f } },     // Skorpion
        { 42555, { 0.003f, 0.083f, 0.023f } },     // SVD
        { 43215, { 0.057f, 0.039f, 0.050f } },     // MP34 (its side magazine)
        { 57819, { 0.050f, 0.176f, 0.033f } },     // AR-C (the spare magazine, draw 5106)
        { 58416, { 0.029f, 0.153f, -0.016f } },    // M249
        { 62229, { 0.038f, 0.173f, 0.021f } },     // AR-C (long)
        { 40752, { 0.041f, 0.172f, 0.022f } },     // M16A1 (the spare magazine, draw 5853; reload6)
        // manualreload36: the AKs, not measured. reload7's -GameAnim pictures show the game's left hand holding the new magazine at its lower
        // half, its top about a hand's width up, as the AR-Cs' hold; the spare is not among the mod's weapon draws (reload7's full walk: no part
        // but the gun's body within 15 cm of the game's hand), so it cannot be learnt. The AR-Cs' hold until one is measured.
        { 45582, { 0.040f, 0.170f, 0.025f } },     // AK-47 (classic)
        { 60537, { 0.040f, 0.170f, 0.025f } },     // AK-47 (modern)
    };
    std::string hold_line()   // reloadhandmagw's saved form (call with g_hold_mtx held)
    {
        std::string s = "reloadhandmagw";
        for (const auto& [w, v] : g_hold_mag) s += fmt::format(" {}:{:.3f},{:.3f},{:.3f}", w, v.x, v.y, v.z);
        if (g_hold_mag.empty()) s += " none";
        return s;
    }
    std::atomic<uint32_t> g_snd_sig{ 0 };      // the listed weapon in hand (the pad poll), for sound()

    // reloadsoundseq on|off (manualreload42; saved, default on; author 3 Nov 12:58): the game's reload sounds
    // (research\weapondata\weapon_sounds.csv: each sound and its time from the first) cut into four sequences at three times, and each manual
    // event plays its sequence, with the game's own spacing. Sounds before `from` and after `to` are left out. The cuts per weapon: built in
    // below, changed in F11 (reloadsoundseqcut, saved). A weapon's own F11 pick for an event comes before its sequence (reloadsoundseqpass).
    struct SeqSnd
    {
        uint32_t    sig;
        const char* name;
        int         n;
        uint32_t    id[16];
        float       t[16];
        float       cut[5];   // from, out|in, in|back, back|forward, to (s)
    };
    // reloadsoundseqpass (3 Nov, author 14:39): every weapon of the sound run, each step's sounds read from research\weapondata\weapon_sounds.csv and the
    // magazine's track in reload7 (reload7_soundlearn_lines.txt: when the magazine leaves its seat and is back in it), one table per weapon in
    // research\2026-11-03_reloadsoundseqpass.md. Only the chosen sounds are listed, at the game's times; a step borrowed from elsewhere (the
    // shot's own bolt, lever or pump cycle; the AR-C's bolt or the 1911's slide where the game's reload has none) is put after the reload, at
    // made-up times that keep its own spacing.
    // - The AR-C's reload from empty (reload1, the 2 Nov run's first: the gun ran dry) is the sequence the author picked from, one sound a step:
    //   3240498188 out, 3863307842 in, 4246560353 bolt back, 4119235673 forward. The AR-C (long) and M16A1 share it (the M16A1's own run
    //   caught the same ids). Their normal reload (reload2) has no bolt.
    // - The 1911 and the other pistols: the author's 1911 picks (446562585, 1302339480, 2635244640, 2330490764).
    // - Magazine in: 4255759295 is heard on 5 guns 0.1-0.3 s before the magazine is home (the magazine entering), 2347389021 likewise on the
    //   Vector and MS16, 1222288339 as it latches (MP40, BZ19); a click is kept with what comes within 0.2 s before it.
    // - Bolt: 3497450392 starts the SVD's and the M249's handle pair; the bolt rifles' and lever guns' from their shot's cycle; the MP5's handle
    //   locked back (2623623013) and slapped home (3216115768, also the MP40's and MP34's last sound). Other rifles and SMGs: the AR-C's;
    //   machine pistols: the 1911's slide.
    // - Pump guns (M133s, SPAS-12): a shell in (2703300039; the SPAS's 1025465418), the M133's pump after a shot (72867933 + 3403294259 back,
    //   784439553 forward). Lever guns: a round in, the lever from the shot's cycle. Break actions (Sledges, M79): open, a shell in, closed.
    // - Launchers and the flamethrower (for when they are reloaded by hand): the rocket or tank in, the tank off.
    constexpr SeqSnd kSeqSnd[]{
        // reloadmatch: first, so a gun recorded here comes before its older row below: its own reload's sounds, each read against its magazine's and slide's moves.
#include "FarCry5ReloadMatchSeq.inc"
        { 57819, "AR-C", 7, { 3949169702u, 3240498188u, 3863307842u, 4246560353u, 4119235673u, 894646864u, 1879999235u }, { 0.00f, 0.15f, 0.28f, 0.39f, 0.52f, 0.80f, 1.12f }, { 0.100f, 0.220f, 0.330f, 0.450f, 0.600f } },
        { 62229, "AR-C (long)", 7, { 3949169702u, 3240498188u, 3863307842u, 4246560353u, 4119235673u, 894646864u, 1879999235u }, { 0.00f, 0.15f, 0.28f, 0.39f, 0.52f, 0.80f, 1.12f }, { 0.100f, 0.220f, 0.330f, 0.450f, 0.600f } },
        { 40752, "M16A1", 7, { 3949169702u, 3240498188u, 3863307842u, 4246560353u, 4119235673u, 894646864u, 1879999235u }, { 0.00f, 0.15f, 0.28f, 0.39f, 0.52f, 0.80f, 1.12f }, { 0.100f, 0.220f, 0.330f, 0.450f, 0.600f } },
        { 24417, "1911", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 26043, "1911 (extended)", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 22182, "P226", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 29136, "M9", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 28596, "D50", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 33765, "AJM9", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 26187, "6P9", 6, { 2148877110u, 446562585u, 1302339480u, 2635244640u, 2330490764u, 1980945621u }, { 0.00f, 0.24f, 0.37f, 0.91f, 1.03f, 1.29f }, { 0.200f, 0.300f, 0.600f, 0.970f, 1.100f } },
        { 45582, "AK-47 (classic)", 7, { 2501554889u, 2519555196u, 2015139108u, 3944883295u, 453559250u, 2714586708u, 4087086420u }, { 0.47f, 0.56f, 1.16f, 1.25f, 1.73f, 1.73f, 1.97f }, { 0.420f, 0.860f, 1.490f, 1.850f, 2.020f } },
        { 60537, "AK-47 (modern)", 7, { 2501554889u, 2519555196u, 2015139108u, 3944883295u, 453559250u, 2714586708u, 4087086420u }, { 0.47f, 0.56f, 1.16f, 1.25f, 1.73f, 1.73f, 1.97f }, { 0.420f, 0.860f, 1.490f, 1.850f, 2.020f } },
        { 28968, "AK-47", 7, { 2501554889u, 2519555196u, 2015139108u, 3944883295u, 453559250u, 2714586708u, 4087086420u }, { 0.47f, 0.56f, 1.16f, 1.25f, 1.73f, 1.73f, 1.97f }, { 0.420f, 0.860f, 1.490f, 1.850f, 2.020f } },
        { 6867, "AK-47", 7, { 2501554889u, 2519555196u, 2015139108u, 3944883295u, 453559250u, 2714586708u, 4087086420u }, { 0.47f, 0.56f, 1.16f, 1.25f, 1.73f, 1.73f, 1.97f }, { 0.420f, 0.860f, 1.490f, 1.850f, 2.020f } },
        { 53007, "AK-47 (no stock)", 7, { 2501554889u, 2519555196u, 2015139108u, 3944883295u, 453559250u, 2714586708u, 4087086420u }, { 0.47f, 0.56f, 1.16f, 1.25f, 1.73f, 1.73f, 1.97f }, { 0.420f, 0.860f, 1.490f, 1.850f, 2.020f } },
        { 39750, "Vector .45", 5, { 1361332603u, 2347389021u, 1756342197u, 4246560353u, 4119235673u }, { 0.25f, 1.21f, 1.29f, 2.00f, 2.13f }, { 0.200f, 0.730f, 1.645f, 2.065f, 2.180f } },
        { 32688, "MS16 (short)", 10, { 1328308594u, 331978632u, 3037247292u, 2347389021u, 1756342197u, 3021044961u, 3244002017u, 329984622u, 182769431u, 3263990427u }, { 0.37f, 0.37f, 1.46f, 1.46f, 1.59f, 1.61f, 1.92f, 1.98f, 2.31f, 2.39f }, { 0.320f, 0.915f, 1.765f, 2.145f, 2.440f } },
        { 35790, "MS16 (long)", 10, { 1328308594u, 331978632u, 3037247292u, 2347389021u, 1756342197u, 3021044961u, 3244002017u, 329984622u, 182769431u, 3263990427u }, { 0.37f, 0.37f, 1.46f, 1.46f, 1.59f, 1.61f, 1.92f, 1.98f, 2.31f, 2.39f }, { 0.320f, 0.915f, 1.765f, 2.145f, 2.440f } },
        { 39408, "BZ19", 8, { 1328308594u, 212395715u, 212395715u, 3875340163u, 4162193694u, 1222288339u, 4246560353u, 4119235673u }, { 0.64f, 0.64f, 0.77f, 1.61f, 1.65f, 1.70f, 2.40f, 2.53f }, { 0.590f, 1.190f, 2.050f, 2.465f, 2.580f } },
        { 37359, "MP5", 5, { 1600661443u, 4255759295u, 1598804907u, 2623623013u, 3216115768u }, { 0.38f, 1.35f, 1.47f, 1.75f, 1.89f }, { 0.330f, 0.865f, 1.610f, 1.820f, 1.940f } },
        { 35709, "MP5SD", 5, { 1600661443u, 4255759295u, 1598804907u, 2623623013u, 3216115768u }, { 0.38f, 1.35f, 1.47f, 1.75f, 1.89f }, { 0.330f, 0.865f, 1.610f, 1.820f, 1.940f } },
        { 35868, "MP5K", 5, { 1600661443u, 4255759295u, 1598804907u, 2623623013u, 3216115768u }, { 0.38f, 1.35f, 1.47f, 1.75f, 1.89f }, { 0.330f, 0.865f, 1.610f, 1.820f, 1.940f } },
        { 40362, "MP40", 5, { 3161353446u, 3173559946u, 1222288339u, 4246560353u, 3216115768u }, { 0.28f, 1.24f, 1.37f, 1.50f, 1.67f }, { 0.230f, 0.760f, 1.435f, 1.585f, 1.720f } },
        { 43215, "MP34", 8, { 1389234585u, 3161353446u, 4255759295u, 473899547u, 3173559946u, 2830281633u, 4246560353u, 3216115768u }, { 0.30f, 0.30f, 1.02f, 1.08f, 1.08f, 1.17f, 1.35f, 1.54f }, { 0.250f, 0.660f, 1.260f, 1.445f, 1.590f } },
        { 32325, "A99", 6, { 3161353446u, 4255759295u, 1196230858u, 3173559946u, 2635244640u, 2330490764u }, { 0.19f, 1.22f, 1.41f, 1.41f, 1.90f, 2.02f }, { 0.140f, 0.705f, 1.655f, 1.960f, 2.070f } },
        { 41649, "Skorpion", 4, { 1091223143u, 1084418834u, 2635244640u, 2330490764u }, { 0.19f, 1.09f, 1.80f, 1.92f }, { 0.140f, 0.640f, 1.445f, 1.860f, 1.970f } },
        { 35187, "MAC11", 4, { 4072137171u, 1503857625u, 2635244640u, 2330490764u }, { 0.60f, 1.54f, 2.10f, 2.22f }, { 0.550f, 1.070f, 1.820f, 2.160f, 2.270f } },
        { 42555, "SVD", 6, { 63729309u, 685386288u, 2897211156u, 881012671u, 3497450392u, 1674143517u }, { 0.69f, 0.82f, 1.74f, 1.92f, 2.28f, 2.32f }, { 0.640f, 1.280f, 2.100f, 2.300f, 2.370f } },
        { 24474, ".308", 8, { 140018523u, 685386288u, 2897211156u, 837719625u, 258748968u, 230258062u, 828282296u, 1877510187u }, { 0.69f, 0.87f, 1.90f, 3.47f, 3.54f, 3.62f, 3.79f, 3.92f }, { 0.640f, 1.385f, 2.685f, 3.705f, 3.970f } },
        { 42000, "HTI .50", 8, { 3213135265u, 2347389021u, 3590713978u, 212395715u, 1543224786u, 4155180925u, 230258062u, 338296967u }, { 0.87f, 2.65f, 2.84f, 4.08f, 4.21f, 4.29f, 4.42f, 4.70f }, { 0.820f, 1.760f, 3.460f, 4.560f, 4.750f } },
        { 23397, "SA-50", 7, { 1361332603u, 2482505378u, 4255759295u, 2680273823u, 2786310394u, 4246560353u, 3518503605u }, { 0.68f, 0.73f, 1.77f, 1.91f, 1.97f, 2.15f, 2.28f }, { 0.630f, 1.250f, 2.060f, 2.215f, 2.330f } },
        { 58416, "M249", 5, { 2740693043u, 296586361u, 3362915382u, 3497450392u, 3944643529u }, { 1.92f, 3.07f, 3.20f, 3.50f, 3.55f }, { 1.870f, 2.495f, 3.350f, 3.525f, 3.600f } },
        { 25302, "M249", 5, { 2740693043u, 296586361u, 3362915382u, 3497450392u, 3944643529u }, { 1.92f, 3.07f, 3.20f, 3.50f, 3.55f }, { 1.870f, 2.495f, 3.350f, 3.525f, 3.600f } },
        { 61926, "M60", 6, { 2239810187u, 2306894516u, 270063068u, 1706044680u, 2038897719u, 1802956604u }, { 1.00f, 3.48f, 4.30f, 4.30f, 4.76f, 4.99f }, { 0.950f, 2.240f, 3.890f, 4.530f, 5.040f } },
        { 26238, "M60", 6, { 2239810187u, 2306894516u, 270063068u, 1706044680u, 2038897719u, 1802956604u }, { 1.00f, 3.48f, 4.30f, 4.30f, 4.76f, 4.99f }, { 0.950f, 2.240f, 3.890f, 4.530f, 5.040f } },
        { 63120, "M60 (Vietnam)", 6, { 2239810187u, 2306894516u, 270063068u, 1706044680u, 2038897719u, 1802956604u }, { 1.00f, 3.48f, 4.30f, 4.30f, 4.76f, 4.99f }, { 0.950f, 2.240f, 3.890f, 4.530f, 5.040f } },
        { 26430, "M60 (Vietnam)", 6, { 2239810187u, 2306894516u, 270063068u, 1706044680u, 2038897719u, 1802956604u }, { 1.00f, 3.48f, 4.30f, 4.30f, 4.76f, 4.99f }, { 0.950f, 2.240f, 3.890f, 4.530f, 5.040f } },
        { 47430, "Flamethrower", 5, { 4179622636u, 1387461854u, 2700219186u, 2263686978u, 4098469661u }, { 0.93f, 0.99f, 2.46f, 2.54f, 3.08f }, { 0.880f, 1.725f, 2.810f, 2.810f, 3.130f } },
        { 23676, "Flamethrower", 5, { 4179622636u, 1387461854u, 2700219186u, 2263686978u, 4098469661u }, { 0.93f, 0.99f, 2.46f, 2.54f, 3.08f }, { 0.880f, 1.725f, 2.810f, 2.810f, 3.130f } },
        { 35499, "M133 (modern)", 4, { 2703300039u, 72867933u, 3403294259u, 784439553u }, { 0.94f, 2.00f, 2.00f, 2.21f }, { 0.890f, 0.915f, 1.470f, 2.105f, 2.260f } },
        { 29841, "M133 (modern long)", 4, { 2703300039u, 72867933u, 3403294259u, 784439553u }, { 0.94f, 2.00f, 2.00f, 2.21f }, { 0.890f, 0.915f, 1.470f, 2.105f, 2.260f } },
        { 29259, "M133", 4, { 2703300039u, 72867933u, 3403294259u, 784439553u }, { 0.94f, 2.00f, 2.00f, 2.21f }, { 0.890f, 0.915f, 1.470f, 2.105f, 2.260f } },
        { 34497, "M133 (short)", 4, { 2703300039u, 72867933u, 3403294259u, 784439553u }, { 0.94f, 2.00f, 2.00f, 2.21f }, { 0.890f, 0.915f, 1.470f, 2.105f, 2.260f } },
        { 20586, "M133 (DLC)", 4, { 2703300039u, 72867933u, 3403294259u, 784439553u }, { 0.94f, 2.00f, 2.00f, 2.21f }, { 0.890f, 0.915f, 1.470f, 2.105f, 2.260f } },
        { 42549, "SPAS-12", 4, { 1025465418u, 72867933u, 3403294259u, 784439553u }, { 0.89f, 2.00f, 2.00f, 2.21f }, { 0.840f, 0.865f, 1.445f, 2.105f, 2.260f } },
        { 27144, "1887", 5, { 3224516626u, 2703300039u, 2603587974u, 72867933u, 1621353718u }, { 0.97f, 1.02f, 2.32f, 2.32f, 2.46f }, { 0.920f, 0.945f, 1.670f, 2.390f, 2.510f } },
        { 32472, "1887 (modern)", 5, { 3224516626u, 2703300039u, 2603587974u, 72867933u, 1621353718u }, { 0.97f, 1.02f, 2.32f, 2.32f, 2.46f }, { 0.920f, 0.945f, 1.670f, 2.390f, 2.510f } },
        { 39012, "Repeater (modern)", 4, { 813072728u, 3389542460u, 230258062u, 1752179380u }, { 0.50f, 2.31f, 2.37f, 2.42f }, { 0.450f, 0.475f, 1.405f, 2.395f, 2.470f } },
        { 25368, "Repeater (classic)", 4, { 813072728u, 3389542460u, 230258062u, 1752179380u }, { 0.50f, 2.31f, 2.37f, 2.42f }, { 0.450f, 0.475f, 1.405f, 2.395f, 2.470f } },
        // All-weapons pass (8 Nov, simulator run2: "no game sound for rack_back / mag_in"): the other models of the same guns, their family's sequence.
        { 39360, "Repeater (modern long)", 4, { 813072728u, 3389542460u, 230258062u, 1752179380u }, { 0.50f, 2.31f, 2.37f, 2.42f }, { 0.450f, 0.475f, 1.405f, 2.395f, 2.470f } },
        { 25020, "Repeater (classic short)", 4, { 813072728u, 3389542460u, 230258062u, 1752179380u }, { 0.50f, 2.31f, 2.37f, 2.42f }, { 0.450f, 0.475f, 1.405f, 2.395f, 2.470f } },
        { 24504, "1888 (short)", 5, { 3224516626u, 2703300039u, 2603587974u, 72867933u, 1621353718u }, { 0.97f, 1.02f, 2.32f, 2.32f, 2.46f }, { 0.920f, 0.945f, 1.670f, 2.390f, 2.510f } },
        { 26211, "RPG-7 (classic)", 3, { 3633379800u, 3240498188u, 3621502332u }, { 0.34f, 0.53f, 0.81f }, { 0.290f, 0.315f, 0.670f, 0.670f, 0.860f } },
        { 35490, "Sledge (double barrel)", 5, { 607201573u, 1095402538u, 526173240u, 1277015305u, 4217591709u }, { 0.00f, 0.06f, 1.00f, 1.60f, 1.74f }, { 0.000f, 0.530f, 1.300f, 1.300f, 1.790f } },
        { 31470, "Sledge (sawed-off)", 5, { 607201573u, 1095402538u, 526173240u, 1277015305u, 4217591709u }, { 0.00f, 0.06f, 1.00f, 1.60f, 1.74f }, { 0.000f, 0.530f, 1.300f, 1.300f, 1.790f } },
        { 29850, "M79", 5, { 1523209242u, 2576125734u, 1609459330u, 2576125734u, 480803686u }, { 0.25f, 0.25f, 0.83f, 0.83f, 1.08f }, { 0.200f, 0.540f, 0.955f, 0.955f, 1.130f } },
        { 23475, "RPG-7", 3, { 3633379800u, 3240498188u, 3621502332u }, { 0.34f, 0.53f, 0.81f }, { 0.290f, 0.315f, 0.670f, 0.670f, 0.860f } },
        // manualreload53: the M629 (.44), from its game reload from empty in the handgun simulator run (testruns\2026-11-03_handguns, m629_sounds.txt;
        // Author 3 Nov 21:46). Out: the cylinder swung out (0.13-0.36 s); in (each
        // round): the click as the rounds seat (1.89 s); forward: the cylinder swung back in (2.17-2.19 s). The cases falling (0.73-0.86 s) and
        // the speedloader (1.15-1.59 s) are left out: a round at a time plays the click only.
        { 18912, "M629 (.44)", 10, { 2148877110u, 3549290863u, 712069802u, 1401029484u, 996595760u, 1980945621u, 712069802u, 2015844864u, 1401029484u, 3549290863u },
          { 0.13f, 0.23f, 0.23f, 0.32f, 0.36f, 1.89f, 2.17f, 2.19f, 2.19f, 2.19f }, { 0.100f, 0.450f, 2.000f, 2.000f, 2.300f } },
        { 9540, "M629 (.44)", 10, { 2148877110u, 3549290863u, 712069802u, 1401029484u, 996595760u, 1980945621u, 712069802u, 2015844864u, 1401029484u, 3549290863u },
          { 0.13f, 0.23f, 0.23f, 0.32f, 0.36f, 1.89f, 2.17f, 2.19f, 2.19f, 2.19f }, { 0.100f, 0.450f, 2.000f, 2.000f, 2.300f } },
        { 80077, "RAT4", 3, { 2966265881u, 4259575618u, 3375869218u }, { 0.55f, 0.74f, 0.81f }, { 0.500f, 0.525f, 0.835f, 0.835f, 0.860f } },
        { 69646, "RAT4", 3, { 2966265881u, 4259575618u, 3375869218u }, { 0.55f, 0.74f, 0.81f }, { 0.500f, 0.525f, 0.835f, 0.835f, 0.860f } },
    };
    std::atomic<bool>                        g_seq_on{ true };
    // reloadsoundfill on|off (manualreload50b; saved, default on; author 3 Nov 21:46,
    // 21:47: the three examples applied to other handguns): a handgun with no sequence of its own (the P08s, and
    // handguns learnt at their first reload) plays the 1911's game sounds, as the other pistols do. Off: such a handgun is silent.
    std::atomic<bool>                        g_snd_fill{ true };
    std::map<uint32_t, std::array<float, 5>> g_seq_cut;   // reloadsoundseqcut (g_seq_mtx)
    std::mutex                               g_seq_mtx;
    const SeqSnd* seq_snd(uint32_t sig)
    {
        for (const auto& s : kSeqSnd)
            if (s.sig == sig) return &s;
        return nullptr;
    }
    std::array<float, 5> seq_cuts(const SeqSnd& s)   // (call with g_seq_mtx held)
    {
        if (const auto it = g_seq_cut.find(s.sig); it != g_seq_cut.end()) return it->second;
        return { s.cut[0], s.cut[1], s.cut[2], s.cut[3], s.cut[4] };
    }
    // The sounds of sequence k (0 out, 1 in, 2 back, 3 forward): the ids and their delays from the sequence's first.
    int seq_group(const SeqSnd& s, int k, uint32_t* ids, float* dl, int max)
    {
        std::array<float, 5> c{};
        {
            std::lock_guard _{ g_seq_mtx };
            c = seq_cuts(s);
        }
        int   n  = 0;
        float t0 = -1.0f;
        for (int i = 0; i < s.n && n < max; ++i) {
            const float t = s.t[i];
            if (t < c[0] || t > c[4]) continue;
            const int g = t < c[1] ? 0 : t < c[2] ? 1 : t < c[3] ? 2 : 3;
            if (g != k) continue;
            if (t0 < 0.0f) t0 = t;
            ids[n] = s.id[i];
            dl[n]  = t - t0;
            n++;
        }
        return n;
    }
    std::string seq_text(const SeqSnd& s, int k)
    {
        uint32_t ids[16];
        float    dl[16];
        const int n = seq_group(s, k, ids, dl, 16);
        std::string t;
        for (int i = 0; i < n; ++i) t += fmt::format("{}{}{}", i ? " " : "", ids[i], dl[i] > 0.0f ? fmt::format("@+{:.2f}", dl[i]) : std::string{});
        return n ? t : std::string{ "(none)" };
    }
    std::mutex            g_pick_mtx;          // g_snd_pick (the pad poll, F11, the render thread)
    // reloadsoundpick <id>:<out>,<in>,<back>,<fwd> ... | none (saved, the whole list each time): per weapon and event, - the built-in sound, 0
    // none, or a game sound id (type 15).
    std::map<uint32_t, std::array<int64_t, 4>> g_snd_pick;
    std::string pick_line()   // reloadsoundpick's saved form (call with g_pick_mtx held)
    {
        std::string s = "reloadsoundpick";
        for (const auto& [w, a] : g_snd_pick) {
            s += fmt::format(" {}:", w);
            for (int i = 0; i < 4; ++i) s += (i ? "," : "") + (a[size_t(i)] < 0 ? std::string{ "-" } : std::to_string(a[size_t(i)]));
        }
        if (g_snd_pick.empty()) s += " none";
        return s;
    }
    // reloadsoundlearn on|off (manualreload33; saved, default off; author 3 Nov 08:20): his AR-C and 1911 picks were each a sound earlier than the
    // guesses by timing alone. So the sounds are taken from the game's reload itself: FarCry5ReloadMag.cpp follows the gun's magazine part (or the
    // spare it brings in) and its handle through the reload, and gives each moment (the magazine out of its seat, back in, the handle back, home)
    // the game's handling sound (type 15) nearest it in time. Kept per weapon in reloadsoundlearnt (saved, the whole list each time), used after a
    // pick and before the built-in guess.
    std::atomic<bool> g_snd_learn{ false };
    std::map<uint32_t, std::array<int64_t, 4>> g_snd_learnt;   // under g_pick_mtx
    std::string learnt_line()   // call with g_pick_mtx held
    {
        std::string s = "reloadsoundlearnt";
        for (const auto& [w, a] : g_snd_learnt) {
            s += fmt::format(" {}:", w);
            for (int i = 0; i < 4; ++i) s += (i ? "," : "") + (a[size_t(i)] < 0 ? std::string{ "-" } : std::to_string(a[size_t(i)]));
        }
        if (g_snd_learnt.empty()) s += " none";
        return s;
    }

    // The gun in hand: status and fixes (manualreload52; author 3 Nov 22:07, and "move the switch 'learnt from the game' ... into
    // buttons like 'Something wrong with reload sound' and clicking the button launch a reloading sequence with the held weapon"). The mod ships
    // with each gun's data; where it is missing or wrong, a button learns that part again from the game's own reload of the gun in hand.
    // reloadproblem <id>:<mask hex> ... | none (saved, the whole list each time): the parts the player marked wrong, per weapon (kept until
    // "It's right now", so the next fix can read them from the settings). reloadlearntuse <id>:<mask hex> ... | none (saved): the parts whose
    // data learnt again by a button is used ahead of the built-in data: the game's hold of the magazine ahead of the fist (reloadmagpalm), the
    // learnt sounds ahead of the built-in sequence.
    enum Item { kItData, kItMag, kItHold, kItWell, kItHandle, kItGrab, kItSound, kItRound, kItCount };
    constexpr const char* kItName[kItCount]{ "Gun data", "Magazine (its model)", "Hand holding the magazine", "Magazine well", "Charging handle, slide or pump",
                                             "Hand on the handle", "Reload sounds", "Ejected round" };
    std::map<uint32_t, uint32_t> g_problem;      // reloadproblem (g_hold_mtx)
    std::map<uint32_t, uint32_t> g_learnt_use;   // reloadlearntuse (g_hold_mtx)
    std::atomic<uint32_t> g_relearn_sig{ 0 }, g_relearn_items{ 0 };   // a learning asked by a button: this weapon, these parts
    std::atomic<int>      g_relearn_req{ 0 };     // F11 -> the pad poll: start the game's reload for it
    std::atomic<uint32_t> g_empty_sig{ 0 };       // reloadkeepone lifted for this weapon until the game's own reload from empty (the hand on the handle)
    std::atomic<uint32_t> g_sig_now{ 0 };         // the weapon in hand (the pad poll), for the render thread
    std::atomic<int>      g_clip_now{ -1 }, g_reserve_now{ -1 };
    std::mutex            g_relearn_mtx;
    std::string           g_relearn_msg;          // the last word on a learning (F11), under g_relearn_mtx
    std::array<std::string, kItCount> g_relearnt_at;   // when each part of g_relearn_sig was learnt again this session (g_relearn_mtx)
    void relearn_say(const std::string& m)
    {
        spdlog::info("[Reload] relearn: {}", m);
        std::lock_guard _{ g_relearn_mtx };
        g_relearn_msg = m;
    }
    void relearnt(uint32_t sig, int it)   // a part learnt from the game's reload: noted when a button asked for it
    {
        if (sig == 0 || sig != g_relearn_sig.load() || !(g_relearn_items.load() & (1u << it))) return;
        g_relearn_items &= ~(1u << it);
        const auto t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm    lt{};
        localtime_s(&lt, &t);
        {
            std::lock_guard _{ g_relearn_mtx };
            g_relearnt_at[size_t(it)] = fmt::format("{:02d}:{:02d}", lt.tm_hour, lt.tm_min);
        }
        relearn_say(fmt::format("weapon {}: {} learnt again from the game's reload: check it, then 'It's right now'", sig, kItName[it]));
    }
    bool uses_learnt(uint32_t sig, int it)
    {
        std::lock_guard _{ g_hold_mtx };
        const auto i = g_learnt_use.find(sig);
        return i != g_learnt_use.end() && (i->second & (1u << it));
    }
    std::string mask_line(const char* cmd, const std::map<uint32_t, uint32_t>& m)   // reloadproblem / reloadlearntuse (g_hold_mtx held)
    {
        std::string s = cmd;
        for (const auto& [w, b] : m)
            if (b) s += fmt::format(" {}:{:x}", w, b);
        if (s == cmd) s += " none";
        return s;
    }
    bool parse_masks(const std::string& arg, std::istream& rest, std::map<uint32_t, uint32_t>& m)
    {
        m.clear();
        std::string t = arg;
        while (!t.empty() && t != "none") {
            const auto c = t.find(':');
            if (c != std::string::npos) {
                const uint32_t w = uint32_t(std::strtoul(t.c_str(), nullptr, 10));
                const uint32_t b = uint32_t(std::strtoul(t.c_str() + c + 1, nullptr, 16));
                if (w != 0 && b != 0) m[w] = b;
            }
            t.clear();
            rest >> t;
        }
        return true;
    }
    // reloadsoundcat on|off (manualreload25; saved, default off; author 2 Nov 21:51): a pick made in F11 is for the weapon's category, every
    // weapon of it; kept in reloadsoundpick under the category's number (1-6: weapon ids are index counts, far larger). A category's pick comes
    // before the weapon's own (picked or built in).
    std::atomic<bool> g_snd_cat{ false };
    enum SndCat : uint32_t { kCatHandgun = 1, kCatRifle, kCatShotgun, kCatRevolver, kCatBelt, kCatOther, kCatEnd };
    constexpr const char* kCatName[kCatEnd]{ "", "handguns", "rifles and SMGs", "shotguns", "revolvers", "belt-fed guns", "the flamethrower" };
    // F11 (manualreload32; author 3 Nov 08:17): the category a pick goes to (0: the held weapon's own). F11 only, not saved.
    int g_snd_target = 0;
    uint32_t snd_cat(uint32_t sig)
    {
        const Builtin* b = builtin(sig);
        if (!b) {   // a weapon learnt at its first reload: a magazine gun (allreload2: a handgun, or the energy weapons and launchers as the flamethrower)
            const Other o = other_gun(sig);
            return o.norack ? kCatOther : o.pistol ? kCatHandgun : kCatRifle;
        }
        if (b->kind == kTube) return b->lever_deg != 0.0f && b->round == 1152 ? kCatRifle : kCatShotgun;   // the lever-action repeaters' rifle round (manualreload30)
        if (b->kind == kRevolver) return b->clip == 2 ? kCatShotgun : kCatRevolver;   // a break action (manualreload29): a shotgun
        if (b->kind == kBelt) return kCatBelt;
        if (b->norack) return kCatOther;
        return b->pistol ? kCatHandgun : kCatRifle;
    }
    // The game sound for event k with weapon sig: -1 none decided here (then the global reloadgamesounds, or none), 0 none (picked)
    // (picked), else the id; from: where it came from.
    // reloadsoundfill: the weapon's own sequence, or a handgun's borrowed 1911 sequence (borrowed set then).
    const SeqSnd* seq_for(uint32_t sig, bool* borrowed = nullptr)
    {
        if (borrowed) *borrowed = false;
        if (const SeqSnd* s = seq_snd(sig)) return s;
        if (!g_snd_fill.load() || sig == 0 || snd_cat(sig) != kCatHandgun) return nullptr;
        if (borrowed) *borrowed = true;
        return seq_snd(24417);
    }
    int64_t weapon_sound(uint32_t sig, int k, const char** from = nullptr)
    {
        if (from) *from = "";
        if (!g_wsnd.load() || sig == 0 || k < 0 || k >= 4) return -1;
        {
            std::lock_guard _{ g_pick_mtx };
            if (g_snd_cat.load())
                if (const auto it = g_snd_pick.find(snd_cat(sig)); it != g_snd_pick.end() && it->second[size_t(k)] >= 0) {
                    if (from) *from = "picked for the category";
                    return it->second[size_t(k)];
                }
            if (const auto it = g_snd_pick.find(sig); it != g_snd_pick.end() && it->second[size_t(k)] >= 0) {
                if (from) *from = "picked";
                return it->second[size_t(k)];
            }
            if (g_snd_learn.load() || uses_learnt(sig, kItSound))   // manualreload52: learnt again by its button, always used
                if (const auto it = g_snd_learnt.find(sig); it != g_snd_learnt.end() && it->second[size_t(k)] > 0) {
                    if (from) *from = "learnt from the game's reload";
                    return it->second[size_t(k)];
                }
        }
        if (const WeaponSnd* w = weapon_snd(sig); w && w->def[k] != 0) {
            if (from) *from = "built in";
            return w->def[k];
        }
        return -1;
    }
    std::string weapon_sound_text(uint32_t sig)   // reloadweaponsounds: the four events' sounds for this weapon
    {
        std::string t;
        for (int k = 0; k < 4; ++k) {
            const char*   from = "";
            const int64_t w    = weapon_sound(sig, k, &from);
            t += fmt::format("{}{} {}", k ? ", " : "", kSndName[k], w > 0 ? fmt::format("{} ({})", w, from) : w == 0 ? std::string{ "none (picked)" } : std::string{ "none" });
        }
        return t;
    }

    void sound(int k, float delay_s = 0.0f)
    {
        // manualreload50b: the game's own sounds only (reloadweaponsounds).
        if (!g_wsnd.load()) return;
        // reloadweaponsounds: the weapon's own game sound for this event (picked or built in), else reloadgamesounds' (one for every weapon).
        const char*   from = "";
        const uint32_t sig = g_snd_sig.load();
        // reloadsoundseq (manualreload42): this event's whole sequence of the game's own sounds, when the weapon has one.
        // reloadsoundseqpass: a weapon's own F11 pick for this event comes first (a pick for the whole category does not: it would hide every
        // gun's own sequence).
        bool own_pick = false;
        if (k >= 0 && k < 4) {
            std::lock_guard _{ g_pick_mtx };
            if (const auto it = g_snd_pick.find(sig); it != g_snd_pick.end() && it->second[size_t(k)] >= 0) own_pick = true;
            // manualreload52: sounds learnt again by "Something wrong" come ahead of the built-in sequence.
            if (const auto it = g_snd_learnt.find(sig); it != g_snd_learnt.end() && it->second[size_t(k)] > 0) own_pick = own_pick || uses_learnt(sig, kItSound);
        }
        if (g_wsnd.load() && g_seq_on.load() && k >= 0 && k < 4 && !own_pick)
            if (const SeqSnd* sq = seq_for(sig)) {
                uint32_t ids[16];
                float    dl[16];
                const int n  = seq_group(*sq, k, ids, dl, 16);
                int       ok = 0;
                for (int i = 0; i < n; ++i) ok += fc5::gamesound::post(ids[i], 15, delay_s + dl[i]) ? 1 : 0;
                if (g_log.load()) spdlog::info("[Reload] game sound sequence {} for weapon {} ({}): {} ({} posted)", kSndName[k], sig, sq->name, seq_text(*sq, k), ok);
                if (n > 0 && ok > 0) return;
            }
        const int64_t w    = weapon_sound(sig, k, &from);
        if (w > 0) {
            const bool ok = fc5::gamesound::post(uint32_t(w), 15, delay_s);
            if (g_log.load()) spdlog::info("[Reload] game sound {} for weapon {}: {} ({}){}", kSndName[k], sig, w, from, ok ? "" : ": the game's sound call not usable: none");
            return;
        }
        if (w < 0) {
            if (const uint32_t id = g_game_snd[k].load(); id != 0 && fc5::gamesound::post(id, g_game_snd_type[k].load(), delay_s)) return;
        }
        if (k >= 0 && k < 4 && g_log.load()) spdlog::info("[Reload] no game sound for {} with weapon {}: silent (learn it from the game's reload)", kSndName[k], sig);
    }

    // A learnt or saved magazine part is a weapon's (its bones low; see kMaxWeaponBone).
    bool part_ok(const MagPart& p) { return p.prop || p.rigid || (p.nbones > 0 && std::all_of(p.bones, p.bones + p.nbones, [](uint32_t b) { return b <= kMaxWeaponBone; })); }
    // A built-in magazine (kBuiltin): its slot of the main mesh, its top round's draw, and the game's spare magazine props hidden. The AR-C's
    // top round (3036) moved 69 cm in the game's hidden reload of 2 Nov 12:24 (it went with the game's left hand) and stayed in the gun when
    // the magazine left it, until it was made part of the magazine.
    std::vector<MagPart> builtin_parts(const Builtin& b)
    {
        std::vector<MagPart> v;
        if (b.kind == kMagFed || b.kind == kBelt) {   // a tube's only part in the hand is its shell (the round draw), a revolver's a round
            MagPart m{};
            m.count = b.sig & 0x10000u ? 65535u : b.sig;   // (FarCry5Stereo::sig_mesh: the MG42's)
            if (b.kind == kMagFed || b.mag < b.belt_from) m.bones[m.nbones++] = b.mag;   // (the M249's box under its cover; the M60's mag is its belt's first slot)
            if (b.belt_to > 0)
                for (uint32_t i = b.belt_from; i <= b.belt_to && m.nbones < 16; ++i) m.bones[m.nbones++] = i;   // the belt's rounds in the gun
            v.push_back(m);
        }
        if (b.round != 0) {
            MagPart r{};
            r.count = b.round;
            if (b.rround)
                r.rigid = true;   // allreload: the M79's grenade, the RPG-7's rocket (one piece)
            else if (b.belt_to > 0)
                for (int i = 0; i < b.nround && i < 16; ++i) r.bones[r.nbones++] = uint32_t(i);   // the rest of the belt: every slot
            else
                for (int i = 0; i < b.nround && i < 2; ++i) r.bones[r.nbones++] = b.round_bones[i];
            v.push_back(r);
        }
        for (const uint32_t pc : b.props)
            if (pc != 0) {
                MagPart p{};
                p.count = pc;
                p.prop  = true;
                v.push_back(p);
            }
        return v;
    }
    std::atomic<uint32_t>               g_back_sig{ 0 };
    std::atomic<float>                  g_back[3]{ 0.0f, 0.0f, 1.0f };

    // The state (pad poll, under g_mtx); the atomics are for the render thread and F11.
    std::mutex        g_mtx;
    std::atomic<bool> g_in_hand{ false };
    std::atomic<bool> g_hide{ false };        // hides_game_reload()
    std::atomic<bool> g_busy_now{ false };
    std::atomic<bool> g_pending_now{ false };   // g_game_pending, for the render thread (manualreload28)
    std::atomic<uint32_t> g_drops{ 0 };
    std::atomic<int64_t>  g_drop_ms{ 0 };
    uint32_t          g_sig = 0;
    bool              g_mag_in_gun = true;
    bool              g_hand_mag   = false;
    bool              g_engaged    = false;
    float             g_insert     = 0.0f;
    int               g_insert_cm  = 0;       // the last centimetre ticked
    clock::time_point g_engage_t{};           // reloadroundslide: when the round went into its chamber's mouth
    bool              g_need_rack  = false;
    bool              g_geom_log   = false;   // reloadforce geom: the test geometry line at the next active poll
    bool              g_mag_fresh  = false;   // a magazine seated since the clip ran dry
    bool              g_racking    = false;
    bool              g_rack_done  = false;   // this pull clicked (no more until the grip is let go)
    float             g_rack       = 0.0f;
    int               g_rack_cm    = 0;
    glm::vec3         g_rack_r0{ 0.0f };      // where the hand took the handle, in the gun controller's axes
    bool              g_game_pending = false; // the game's reload tapped, its clip not refilled yet
    int               g_clip_at_tap  = -1;
    clock::time_point g_tap_t{};
    int               g_retaps       = 0;
    clock::time_point g_busy_until{};         // the game's reload animation kept out of the view until then
    clock::time_point g_seat_t{};
    bool              g_key_pending  = false; // the game's reload key to send at the next poll with the key free
    clock::time_point g_pad_until{};          // reloadkey x: pad X held until then
    bool              g_b_was   = false;      // right B last poll
    bool              g_b_mine  = false;      // this press is the reload's (held back) ...
    bool              g_b_pass  = false;      // ... or passed to the game as Y (held past reloadtap, or not ours)
    clock::time_point g_b_t0{};
    bool              g_pouch_in = false;     // the free hand in the pouch zone (entry tick)
    int               g_last_clip = -1;
    std::map<uint32_t, int> g_clip_max{ { kArc, 30 } };   // the largest clip seen per weapon (the AR-C's data: 30)
    clock::time_point g_drop_t{};
    clock::time_point g_take_t{};
    // A pump shotgun (kTube): the shells loaded by hand in its tube (-1: not read from the game's clip yet) and its chamber (0 empty, 1 a live
    // shell, 2 a fired one). The game's clip is refilled by its own reload (tapped, hidden) when a shell goes in and the clip has fewer.
    int               g_tube    = -1;
    int               g_chamber = 1;
    bool              g_bolt_owed = false;    // a bolt action fired: its bolt to work before the next shot (allreload, reloadbolt)
    // reloadhlwhen reload|shot|always (saved, default reload; author 6 Nov 14:10): when the reload highlights (reloadhintstyle glow or
    // boxes, the charging handle's blue ball) are drawn. reload: only with the gun run dry, or once a reload is begun (the magazine dropped or in the
    // hand, a shell or round taken, a feed cover, cylinder or action opened) until the gun is ready to fire again, and whenever an action is needed
    // before it fires again (reloadhlaction, author 6 Nov 15:05: the bolt, pump or
    // lever to work after a shot with rounds left, the handle to rack); always: as before, also the handle dimmer whenever the free hand is near it
    // (a machine gun held by its fore-end while firing). "shot" (the build of 6 Nov 14:40) is now the same as reload. Only the drawing: the hands'
    // logic (twohandkeep's regrab, the zones) reads the parts as before.
    std::atomic<int>  g_hl_when{ 0 };         // 0 reload, 2 always (1, shot: as 0)
    bool              g_hl_started = false;   // reloadhlwhen: a reload begun on the gun in hand, until it is ready again
    // reloadboltturn on|off, reloadboltdeg N, reloadboltlift M (saved; default on, 60 deg, 0.04 m; author 6 Nov 12:38): a bolt action's bolt follows the hand in four steps.
    // Up: turned about its axis as the hand rises (reloadboltdeg once it has risen reloadboltlift). Back: only once fully up, pulled to its stop (the fired case out). Forward:
    // pushed home (the next round fed). Down: turned down as the hand lowers, locked: the round chambered, it fires. Let go anywhere: it stays there (no shot until locked).
    // Either hand works it (the right one with twohandkeep). Off: pulled straight back and let go, as before.
    std::atomic<bool>  g_bolt_turn{ true };
    // reloadboltparts on|off (saved, default on; author 6 Nov 13:55, bolt actions): reloadboltpin keeps
    // only the handle part (the .308's BOLT, the HTI .50's BOLT) at rest; the game's own cycle after each shot still moved the parts beside it
    // (the .308's BOLT_STOP, the HTI .50's BOLT_COVER: palette slots named BOLT_* in the bone map). Those are kept where they sit at rest too.
    std::atomic<bool>  g_bolt_parts{ true };
    std::atomic<float> g_bolt_deg{ 60.0f }, g_bolt_lift{ 0.04f };
    float             g_bolt_up     = 0.0f;    // the bolt turned up (0 locked down .. 1 fully up)
    bool              g_bolt_open   = false;   // let go out of its lock (turned up or back): stays there, no shot
    bool              g_bolt_pulled = false;   // pulled to its stop since it was last locked (the chamber emptied)
    bool              g_bolt_fed    = false;   // pushed home after that pull: a round fed into the chamber
    constexpr float   kBoltHome = 0.03f, kBoltUpAt = 0.95f, kBoltLockAt = 0.08f;
    // A lever action (manualreload30): its lever worked by quick right B, down and back up in kLeverSecs (the chamber's shell thrown out at
    // the bottom, the next from the tube chambered on the way up).
    bool              g_lever_req = false, g_lever_cyc = false, g_lever_fwd = false;
    // reloadleverhand on|off (saved, default on; author 5 Nov): with the left grip holding the gun's fore-end, the right hand dropped away from the left and brought back works the lever,
    // which follows it: the shell out once it is well down, the next chambered on the way up. Read against the left hand, so the two hands swinging the gun together do nothing. Only while the chamber
    // needs it (a fired shell or empty), so a lever is never thrown from a live round by a slip. Quick right B still works it, with one hand or any time.
    std::atomic<bool> g_lever_hand_on{ true };
    // reloadactionopen on|off (saved, default on; author 5 Nov): the lever thrown down, or the pump left back, stays open: the shell in the chamber out, and with it open a shell or round goes in
    // from anywhere near the port (kActLoadZone; no angle, no pushing). Closed by the right hand brought up, a new left grip on the fore-end, the pump pushed forward, or quick right B; the next one chambers then.
    std::atomic<bool> g_act_on{ true };
    // reloadhandhold on|off (saved, default on; author 5 Nov; run final_e, the .308: the bolt
    // stays, but the game's cycle takes the right hand off the grip): for this long after a shot (the longest mustPrepareShot's, the MBP .50's 1.4 s) the weapon hand is drawn as it was before it.
    std::atomic<bool>    g_hand_hold{ true }, g_cycle_gun{ false };
    // reloadflick on|off (saved, default on; author 5 Nov 22:51): the gun turned this fast
    // closes what B opened: a lever, a pump, a break action or the RAT4's back about the gun's right axis (muzzle up or down), a revolver's cylinder about the others (sideways).
    std::atomic<bool>    g_flick{ true };
    // (Run final_h: measured from one pad poll to the next, a slow turn's 2 deg steps read as 3000 deg/s and closed it, and the fast turn then found nothing open. Now the turn over the last 0.2 s: this many degrees.)
    constexpr float      kFlickDeg = 35.0f, kFlickWindowMs = 200.0f;
    std::atomic<int64_t> g_cycle_until{ 0 };
    constexpr int        kCycleMs = 1600;
    std::atomic<bool> g_rack_loss{ true };   // reloadrackloss on|off (saved, default on): a rack with a round in costs a bullet
    bool              g_act_open = false, g_act_close_req = false, g_act_base_ok = false;
    bool              g_act_open_req = false;   // reloadactionopen: a quick B to open it
    clock::time_point g_act_open_t{};           // ... when it was last opened
    // Author 5 Nov (log 15:40:59-15:41:03: closed 0.1-0.5 s after each opening): closed by the hand only after this wait, and this far up.
    constexpr float   kActCloseWaitMs = 700.0f, kActCloseUp = 0.08f;
    // reloadrackloss: the rounds a rack threw out, which the game's clip still counts: no call sets it (author 5 Nov). Taken off its clip for everything the mod
    // does and shows; once the clip is filled they come off the reserve, the price of the rounds it no longer holds.
    struct MagLoss { int lost{ 0 }, real{ -1 }; };
    std::map<uint32_t, MagLoss> g_mag_loss;
    glm::vec3         g_act_base{ 0.0f };   // the lever open: the right hand's lowest from the left, since the left took the fore-end
    constexpr float   kActLoadZone = 0.10f;
    std::atomic<bool> g_shot_mute{ true };   // reloadshotmute on|off (saved, default on): the game's own pump, lever or bolt sounds after a shot not played (the hand's stroke plays them)
    bool              g_lever_hand = false, g_lever_down = false, g_lever_base_ok = false;   // following the hand; past the shell's throw; the rest below has been read
    float             g_lever_f = 0.0f;
    glm::vec3         g_lever_base{ 0.0f }, g_lever_dir{ 0.0f };   // the right hand from the left, in the left's axes: at rest (a slow average); the gun's down at the start
    clock::time_point g_lever_hand_t{}, g_lever_seen_t{};
    constexpr float   kLeverStroke = 0.07f;   // the right hand this far down: the lever fully thrown (m)
    clock::time_point g_lever_t{};
    constexpr float   kLeverSecs = 0.45f;
    // A revolver (kRevolver): its six chambers (0 empty, 1 a live round, 2 a fired case; read from the game's clip when it comes into the
    // hand), the next to fire, the cylinder swung out or not (since g_open_t), the fired cases dropped out of it.
    int               g_cyl[6]{};
    int               g_cyl_n     = 6;       // its chambers (a break action: 2; the others marked 3, no chamber; manualreload29)
    bool              g_cyl_known = false;
    int               g_cyl_next  = 0;
    clock::time_point g_stuck_t{};           // reloadnostuck: the last poll the weapon was not held from firing as empty
    bool              g_open      = false;
    clock::time_point g_open_t{};
    bool              g_btn_reload = false;  // reloadbutton: the game's reload from its button, followed (shown) since g_btn_t
    bool              g_btn_nothing = false; // ... pressed with the clip already full: the game has nothing to play
    clock::time_point g_btn_t{}, g_btn_grow_t{};
    int               g_btn_clip   = -1;     // ... its clip at the press, then the largest since
    std::atomic<uint32_t> g_dumps{ 0 }, g_dump_mask{ 0 };
    constexpr float   kSwingSecs  = 0.18f;   // the cylinder swings out or in in this long
    std::mutex        g_view_mtx;
    View              g_view{};
    // The controllers at the last active poll (reloadwellhere, reloadhandlehere).
    bool              g_last_ok = false, g_last_mirror = false;
    glm::vec3         g_last_gun_pos{ 0.0f }, g_last_hand_pos{ 0.0f };
    glm::mat3         g_last_gun_rot{ 1.0f }, g_last_hand_rot{ 1.0f };
    std::mutex        g_save_mtx;
    std::string       g_save_line;   // a line to run (and so save) through the command queue

    float ms_since(clock::time_point t, clock::time_point now) { return std::chrono::duration<float, std::milli>(now - t).count(); }

    void buzz(Out& o, int hand, float amp, float sec)
    {
        if (amp > o.buzz[hand]) {
            o.buzz[hand]   = amp;
            o.buzz_s[hand] = sec;
        }
    }

    glm::vec3 mirror_x(glm::vec3 v, bool m)
    {
        if (m) v.x = -v.x;
        return v;
    }

    std::string geom_line()   // reloadgeom's saved form (call with g_list_mtx held)
    {
        std::string s = "reloadgeom";
        for (const auto& [w, g] : g_geom)
            s += fmt::format(" {}:{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{}", w, g.well.x, g.well.y, g.well.z, g.axis.x, g.axis.y, g.axis.z, g.handle.x,
                             g.handle.y, g.handle.z, g.measured ? 1 : 0, g.handle_set ? 1 : 0);
        if (g_geom.empty()) s += " none";
        return s;
    }

    std::string parts_line()   // reloadmagmesh's saved form (call with g_list_mtx held)
    {
        std::string s = "reloadmagmesh";
        for (const auto& [w, v] : g_parts) {
            if (v.empty()) continue;
            s += fmt::format(" {}:", w);
            for (size_t i = 0; i < v.size(); ++i) {
                const MagPart& p = v[i];
                s += fmt::format("{}{}/", i ? "," : "", p.count);
                if (p.prop) s += "p";
                else if (p.rigid) s += "r";
                else
                    for (int b = 0; b < p.nbones; ++b) s += fmt::format("{}{}", b ? "+" : "", p.bones[b]);
            }
        }
        if (s == "reloadmagmesh") s += " none";
        return s;
    }

    std::string handle_line()   // reloadhandlepart's saved form (call with g_list_mtx held)
    {
        std::string s = "reloadhandlepart";
        for (const auto& [w, p] : g_handle_parts)
            s += p.second == kRigidBone ? fmt::format(" {}:{}/r", w, p.first) : fmt::format(" {}:{}/{}", w, p.first, p.second);
        if (g_handle_parts.empty()) s += " none";
        return s;
    }

    std::string list_text()
    {
        std::lock_guard _{ g_list_mtx };
        std::string s;
        for (const uint32_t w : g_weapons) s += (s.empty() ? "" : " ") + std::to_string(w);
        return s;
    }

    std::string state_text()
    {
        if (!g_in_hand.load()) return "no listed weapon in hand";
        if (builtin(g_sig) && builtin(g_sig)->kind == kRevolver) {
            int live = 0, fired = 0;
            for (const int c : g_cyl) { live += c == 1; fired += c == 2; }
            std::string s = fmt::format("cylinder {}, {} live round(s), {} fired case(s)", g_open ? "open" : "closed", live, fired);
            if (g_hand_mag) s += g_engaged ? fmt::format(", a round going in ({:.0f} %)", g_insert * 100.0f) : ", a round in the left hand";
            if (g_game_pending) s += ", the game still reloading";
            return s;
        }
        if (g_tube >= 0 || (builtin(g_sig) && builtin(g_sig)->kind == kTube)) {
            std::string s = fmt::format("{} shell(s) in the tube, the chamber {}", std::max(0, g_tube), g_chamber == 1 ? "loaded" : g_chamber == 2 ? "holding a fired shell" : "empty");
            if (g_hand_mag) s += g_engaged ? fmt::format(", a shell going in ({:.0f} %)", g_insert * 100.0f) : ", a shell in the left hand";
            if (g_racking) s += fmt::format(", the pump back {:.0f} %", g_rack * 100.0f);
            if (g_game_pending) s += ", the game still reloading";
            return s;
        }
        std::string s = g_mag_in_gun ? "magazine in" : (g_engaged ? fmt::format("magazine going in ({:.0f} %)", g_insert * 100.0f) : "magazine out");
        if (g_hand_mag) s += ", one in the left hand";
        if (g_need_rack) s += g_racking ? fmt::format(", racking ({:.0f} %)", g_rack * 100.0f) : ", handle to rack";
        else if (g_bolt_owed) s += g_racking ? fmt::format(", working the bolt ({:.0f} %)", g_rack * 100.0f) : ", bolt to work";
        if (g_game_pending) s += ", the game still reloading";
        return s;
    }

    void reset_state(const char* why)
    {
        if (g_log.load() && (!g_mag_in_gun || g_hand_mag || g_need_rack || g_game_pending)) spdlog::info("[Reload] state reset ({}): was {}", why, state_text());
        g_mag_in_gun   = true;
        g_hand_mag     = false;
        g_engaged      = false;
        g_insert       = 0.0f;
        g_need_rack    = false;
        g_mag_fresh    = false;
        g_racking      = false;
        g_rack         = 0.0f;
        g_game_pending = false;
        g_key_pending  = false;
        g_last_clip    = -1;
        g_tube         = -1;
        g_chamber      = 1;
        g_bolt_owed    = false;
        g_hl_started   = false;  // reloadhlwhen
        g_bolt_up      = 0.0f;   // reloadboltturn
        g_bolt_open    = g_bolt_pulled = g_bolt_fed = false;
        g_cyl_known    = false;
        g_lever_req = g_lever_cyc = g_lever_hand = g_lever_down = g_act_open = false;
        g_open         = false;
        g_seat_hold    = false;
        g_hand_was     = false;
        g_btn_reload   = false;
        g_btn_now      = false;
        g_cover_hold   = false;   // reloadcoverpush
        g_rat_on       = false;   // reloadratslide
        g_rat_auto     = false;
        g_stuck_t      = clock::now();
    }
}

bool on() { return g_on.load(); }
bool in_hand() { return g_in_hand.load(); }
bool hand_hold_gun() { return g_hand_hold.load() && g_in_hand.load() && g_cycle_gun.load(); }
bool hand_hold_now() { return hand_hold_gun() && clock::now().time_since_epoch().count() < g_cycle_until.load(); }
bool bolt_parts() { return g_bolt_parts.load(); }
bool hides_game_reload() { return g_hide.load(); }
bool game_busy() { return g_busy_now.load(); }
bool game_pending() { return g_pending_now.load(); }
bool pump_held() { return g_pump_now.load(); }
bool pump_aim_hold() { return g_pump_aim.load(); }
bool pump_grip() { return g_pump_grip.load(); }
bool mag_in_left_hand() { return g_mag_hand_now.load(); }
bool b_heal_only() { return g_b_heal.load(); }
bool keep_one() { return !g_led_now.load() && (g_empty_sig.load() == 0 || g_empty_sig.load() != g_sig_now.load()); }   // (ammoledger: no round kept)
bool ledger_now() { return g_led_now.load(); }
// (9 Nov, run 9DC155EC: after a top-up the game's clip reads 30 while the gun holds its last round; read every 100 ms by the game's clip, the
// mod missed 30 -> 29 and a 31st round, a surplus one, left before the trigger was held. Read every update while the real count is low.)
void ledger_on_shot()
{
    if (g_led_arm.exchange(false)) {
        g_led_shot_hold = true;
        if (g_log.load()) spdlog::info("[Reload] ammoledger: the last round left: the trigger held at once (the game's shot, before the next clip read)");
    }
}
bool ledger_shot_hold() { return g_led_shot_hold.load(); }
bool ledger_low() { return g_led_now.load() && g_led_real.load() >= 0 && g_led_real.load() <= 4; }
// reloadkeepone's round kept with this reserve: with a reserve, or with none while noautoswitch is on (the game then never sees the gun empty).
static bool keep_res(int reserve) { return reserve > 0 || g_no_switch.load(); }
bool button_reload() { return g_btn_now.load(); }
bool hides_game_casings() { return g_casing_now.load(); }
bool relearn_armed(uint32_t sig, int item) { return sig != 0 && sig == g_relearn_sig.load() && (g_relearn_items.load() & (1u << item)); }
bool reload_button_pass(bool down)
{
    // The game's polls and the virtual pad's updates both come here (ModConfig.cpp): atomics, and each press decided once.
    static std::atomic<bool>    was{ false }, pass{ false }, held{ false }, said{ false }, tap{ false };
    static std::atomic<int64_t> t0{ 0 }, sent_t{ 0 };
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
    if (now < g_btn_test_until.load()) down = true;   // reloadbuttontest (test): left Y held, through the guard as a real press
    // The author's play of 4 Nov 22:35-22:36: a press held just past the guard (510, 503, 504 ms) reached the game for a moment only, and it reloaded nothing: once let through, the game sees the
    // button down at least this long.
    constexpr int64_t kSentMinMs = 150;
    // ... and at most this long. The game's button is a reload up to 300 ms and a change of weapon (to the sidearm) from 350 ms (run hold1, P226 and AR-C, 60-700 ms): a press of 0.9 s through the
    // guard changed the AR-C to the 1911 with no reload (run button1; author 4 Nov). So with a gun of the manual reload in hand every press is a tap for
    // the game, however long it is held. Held on by kResumeMs, it goes to the game again, so a long hold still does what it does in the game.
    constexpr int64_t kSentMs = 200, kResumeMs = 700;
    if (!down && tap.load() && pass.load() && now - sent_t.load() < kSentMinMs) {
        note_reload_button(true);
        return true;
    }
    if (!down) {
        if (was.exchange(false) && held.load() && !pass.load() && g_log.load())
            spdlog::info("[Reload] reloadbuttonguard: left Y let go after {} ms: not sent to the game", now - t0.load());
        pass = held = tap = false;
        note_reload_button(false);
        return false;
    }
    if (!was.exchange(true)) {
        t0         = now;
        said       = false;
        const bool in_step = now - g_step_ms.load() <= kGuardTailMs;
        const bool full    = g_clip_full_now.load();
        const bool step    = g_guard_on.load() && g_on.load() && g_in_hand.load() && !g_btn_now.load() && (in_step || g_guard_all.load() || full);
        held       = step;
        pass       = !step;
        tap        = g_on.load() && g_in_hand.load();
        sent_t     = now;
        if (step && g_log.load())
            spdlog::info("[Reload] reloadbuttonguard: left Y pressed {}: held back (sent to the game once held {} ms)",
                         in_step ? "with the left hand at work" : full ? "with the clip full" : "with a gun of the manual reload in hand (reloadbuttonguardall)", g_guard_ms.load());
    }
    if (!pass.load() && now - t0.load() >= g_guard_ms.load()) {
        pass = true;
        sent_t = now;
        if (g_log.load()) spdlog::info("[Reload] reloadbuttonguard: left Y held {} ms: sent to the game", now - t0.load());
    }
    if (!pass.load()) return false;
    if (tap.load() && now - sent_t.load() >= kSentMs) {
        if (now - t0.load() < (held.load() ? g_guard_ms.load() : 0) + kResumeMs) {
            note_reload_button(false);
            return false;
        }
        if (!said.exchange(true) && g_log.load()) spdlog::info("[Reload] reloadbuttonguard: left Y still held after {} ms: sent to the game again (a long hold)", now - t0.load());
    }
    note_reload_button(true);
    return true;
}
void note_reload_button(bool down)
{
    if (!down) g_btn_down = false;
    else if (!g_btn_down.exchange(true)) g_btn_edge = true;
}
bool highlight_on() { return g_hl_on.load(); }
bool hint_glow() { return g_hint_glow.load(); }
bool hand_snap_on() { return g_hand_snap.load(); }
bool rec_on() { return g_rec.load(); }
// reloadmaglower: the fist's middle from the left controller on this gun (the drawn hand stands differently on each): rm_gen.py, from the drawn palm.
glm::vec3 fist_at(uint32_t sig)
{
    for (const auto& r : fc5::reloadmatch::kFists)
        if (r.sig == sig) return glm::vec3{ r.c[0], r.c[1], r.c[2] };
    return glm::vec3{ 0.0f, 0.03f, -0.025f };
}
bool mag_lower(uint32_t sig)
{
    if (!g_mag_lower.load()) return false;
    const Builtin* b = builtin(sig);
    if (b) return b->kind == kMagFed && b->belt_to == 0 && !b->norack;
    return !other_gun(sig).norack;
}
bool hand_game(uint32_t sig, HandGame& h)
{
    if (!g_hand_game.load()) return false;
    for (const auto& r : fc5::reloadmatch::kRows) {
        if (r.sig != sig) continue;
        h            = HandGame{};
        h.mag        = r.has_mag;
        h.mag_c      = glm::vec3{ r.mag_c[0], r.mag_c[1], r.mag_c[2] };
        h.mag_turn   = glm::mat3_cast(glm::quat{ r.mag_q[0], r.mag_q[1], r.mag_q[2], r.mag_q[3] });
        h.grab       = r.has_grab;
        h.grab_c     = glm::vec3{ r.grab_c[0], r.grab_c[1], r.grab_c[2] };
        h.grab_turn  = glm::mat3_cast(glm::quat{ r.grab_q[0], r.grab_q[1], r.grab_q[2], r.grab_q[3] });
        h.slide_lock = r.slide_lock;
        return true;
    }
    return false;
}
bool game_hand_now() { return g_game_hand.load() && g_btn_now.load() && g_in_hand.load(); }
bool hand_grab_on() { return g_hand_grab.load(); }
bool round_pinch_on() { return g_round_pinch.load(); }

std::string set_grab_hold(uint32_t sig, const glm::vec3& c, const glm::vec3& handle)
{
    if (sig == 0 || !grab_plausible(c, handle)) return {};
    std::lock_guard _{ g_hold_mtx };
    if (const auto it = g_grab_hold.find(sig); it != g_grab_hold.end() && glm::length(it->second - c) < 0.005f) return {};
    g_grab_hold[sig] = c;
    relearnt(sig, kItGrab);
    return grab_line();
}

int grab_hold(uint32_t sig, bool pump, const glm::vec3& handle, glm::vec3& c)
{
    {
        std::lock_guard _{ g_hold_mtx };
        if (const auto it = g_grab_hold.find(sig); it != g_grab_hold.end() && grab_plausible(it->second, handle)) {
            c = it->second;
            return 1;
        }
    }
    for (const auto& b : kGrabBuiltin)
        if (b.sig == sig) {
            c = glm::vec3{ b.c[0], b.c[1], b.c[2] };
            return 2;
        }
    if (!pump) {
        // manualreload54: a handgun's slide (reloadslidegrab)
        const Builtin* bt = builtin(sig);
        if ((bt && bt->pistol) || (!bt && other_gun(sig).pistol)) {
            c = handle - glm::vec3{ g_slide_grab[0].load(), g_slide_grab[1].load(), g_slide_grab[2].load() };
            return 4;
        }
    }
    const float* o = pump ? kGrabPump : kGrabPinch;
    c              = handle - glm::vec3{ o[0], o[1], o[2] };
    return 3;
}
void set_highlight(uint32_t sig, int which, const HlBox& b)
{
    if (sig == 0 || which < 0 || which >= kHlCount) return;
    std::lock_guard _{ g_list_mtx };
    g_hl[sig][which] = b;
}
bool highlight(uint32_t sig, int which, HlBox& b)
{
    if (which < 0 || which >= kHlCount) return false;
    std::lock_guard _{ g_list_mtx };
    const auto it = g_hl.find(sig);
    if (it == g_hl.end() || !it->second[which].ok) return false;
    b = it->second[which];
    return true;
}
bool b_heal_y(bool b_down)
{
    std::lock_guard _{ g_heal_mtx };
    const auto now      = clock::now();
    const int  hold_ms  = g_heal_ms.load();
    const bool y_runs   = g_hb_y && ms_since(g_hb_y0, now) < float(hold_ms);
    if (b_down && !g_hb_down) {
        g_hb_t0    = now;
        g_hb_throw = false;
        if (!y_runs) g_hb_y = false;   // a new press during the minimum hold carries it on (no tap in between)
    } else if (!b_down && g_hb_down && !g_hb_y && !g_hb_throw && g_log.load()) {
        spdlog::info("[Reload] bheal: quick right B ({:.0f} ms): no weapon switch", ms_since(g_hb_t0, now));
    }
    g_hb_down = b_down;
    if (b_down && !g_hb_throw && !g_hb_y && ms_since(g_hb_t0, now) >= float(g_tap_ms.load())) {
        g_hb_y  = true;
        g_hb_y0 = now;
        if (g_log.load()) spdlog::info("[Reload] bheal: right B held {} ms: the pad's Y (heal), held at least {} ms", g_tap_ms.load(), hold_ms);
    }
    const bool y = g_hb_y && !g_hb_throw && (b_down || ms_since(g_hb_y0, now) < float(hold_ms));
    if (!y && !b_down) g_hb_y = false;
    return y;
}
void b_heal_throw()
{
    std::lock_guard _{ g_heal_mtx };
    g_hb_throw = true;
}
bool log_on() { return g_log.load(); }
float travel_m() { return g_travel.load(); }
float pouch_zone() { return g_pouch_zone.load(); }
uint32_t drops() { return g_drops.load(); }
int64_t  drop_ms() { return g_drop_ms.load(); }
bool scanning() { return clock::now().time_since_epoch().count() < g_scan_until.load(); }
bool model_on() { return g_model_on.load(); }
bool sound_learn_on() { return g_snd_learn.load() || (g_relearn_items.load() & (1u << kItSound)); }
int sound_spread(uint32_t id)
{
    // manualreload36: how many weapons of the sound run play this id in their reload (kWeaponSnd's lists); 7 or more: a hand or pouch noise.
    static const std::map<uint32_t, int> spread = [] {
        std::map<uint32_t, int> m;
        for (const auto& w : kWeaponSnd) {
            std::set<uint32_t> ids;
            for (const char* p = w.seq; *p;) {
                char*               e = nullptr;
                const unsigned long v = std::strtoul(p, &e, 10);
                if (e == p) { ++p; continue; }
                if (*e == '@') ids.insert(uint32_t(v));
                p = e;
            }
            for (const uint32_t i : ids) m[i]++;
        }
        return m;
    }();
    const auto it = spread.find(id);
    return it != spread.end() ? it->second : 0;
}
std::string learn_sounds(uint32_t sig, const int64_t ids[4])
{
    std::lock_guard _{ g_pick_mtx };
    auto& a = g_snd_learnt.try_emplace(sig, std::array<int64_t, 4>{ -1, -1, -1, -1 }).first->second;
    for (size_t i = 0; i < 4; ++i)
        if (ids[i] > 0) a[i] = ids[i];
    relearnt(sig, kItSound);
    return learnt_line();
}
void set_model_shown(bool shown) { g_model_shown = shown; }
bool model_shown() { return g_model_shown.load(); }
void set_pouch_shown(bool shown) { g_pouch_shown = shown; }
bool pouch_shown() { return g_pouch_shown.load(); }
void set_handle_shown(bool shown) { g_handle_shown = shown; }
bool handle_shown() { return g_handle_shown.load(); }
int  take_handle_next() { return g_handle_next.exchange(0); }
bool  drop_fall_on() { return g_drop_fall.load(); }
uint32_t ejects() { return g_ejects.load(); }
void round_landed() { sound(kSndRoundLand); }
bool round_part(uint32_t sig, uint32_t& count, uint32_t& bone, uint32_t& port_bone)
{
    const Builtin* b = builtin(sig);
    if (!b || b->round == 0 || b->nround < 1 || b->nhandle < 1) return false;
    count     = b->round;            // BULLET (the bone map thread)
    bone      = b->round_bones[0];
    port_bone = b->pistol || b->nhandle == 1 ? b->handle[0] : b->handle[b->nhandle - 1];   // the slide, else the bolt: the port is beside it, on the right
    return true;
}
bool casing_port(uint32_t sig, glm::vec3& p)
{
    const Builtin* b = builtin(sig);
    if (!b || (b->casing[0] == 0.0f && b->casing[1] == 0.0f && b->casing[2] == 0.0f)) return false;
    p = glm::vec3{ b->casing[0], b->casing[1], b->casing[2] };
    return true;
}
bool mag_own_axis(uint32_t sig)
{
    const Builtin* b = builtin(sig);
    return b && b->pistol;
}
bool tube_fed(uint32_t sig)
{
    const Builtin* b = builtin(sig);
    return b && b->kind == kTube;
}
bool lever_parts(uint32_t sig, glm::vec3& pivot, float& deg)
{
    const Builtin* b = builtin(sig);
    if (!b || b->kind != kTube || b->lever_deg == 0.0f) return false;
    pivot = glm::vec3{ b->lever_pivot[0], b->lever_pivot[1], b->lever_pivot[2] };
    deg   = b->lever_deg;
    return true;
}
bool revolver_fed(uint32_t sig)
{
    const Builtin* b = builtin(sig);
    return b && b->kind == kRevolver;
}
bool belt_fed(uint32_t sig)
{
    const Builtin* b = builtin(sig);
    return b && b->kind == kBelt;
}
bool belt_parts(uint32_t sig, Belt& r)
{
    if (!belt_fed(sig)) return false;
    r          = Belt{};
    r.main     = sig & 0x10000u ? 65535u : sig;   // (FarCry5Stereo::sig_mesh: the MG42's)
    r.cover    = 1;
    r.pivot    = glm::vec3{ 0.0f, 0.104f, 0.162f };
    if (sig == 24471 || sig == 72736) {   // allreload2: the MG42's cover, 3 UPPER_SHELL, its node (-0.031, 0.215, 0.143) taken as the hinge (a guess)
        r.cover = 3;
        r.pivot = glm::vec3{ 0.0f, 0.215f, 0.143f };
    }
    if (sig == 58416 || sig == 25302) r.pivot = glm::vec3{ 0.0f, 0.195f, 0.143f };   // reloadm249cover: 1 COVER, its node taken as the hinge, as the M60's (weapon_parts.csv)
    r.open_deg = g_cover_deg.load();
    return true;
}
bool revolver_parts(uint32_t sig, Revolver& r)
{
    for (const auto& d : kRevolvers)
        if (d.main == sig) {
            r        = Revolver{};
            r.main   = d.main;
            r.swing[0] = 1; r.swing[1] = 2; r.swing[2] = 3;
            r.nswing = 3;
            r.rounds = d.rounds;
            for (uint32_t i = 0; i < 6; ++i) r.round_bones[i] = i;
            r.cap_bone = 6;
            r.pivot    = glm::vec3{ -0.0026f, 0.1213f, 0.0321f };
            r.open_deg = -80.0f;
            return true;
        }
    for (const auto& d : kBreaks)
        if (d.main == sig) {
            r          = Revolver{};
            r.main     = d.main;
            r.swing[0] = 1; r.swing[1] = 2;   // BARREL, EJECTOR
            r.nswing   = 2;
            r.rounds   = d.rounds;
            r.round_bones[0] = 0; r.round_bones[1] = 1;
            r.nround   = 2;
            r.cap_bone = 0xFFFFu;   // none
            r.pivot    = glm::vec3{ 0.0f, 0.178f, 0.011f };
            r.axis     = glm::vec3{ 1.0f, 0.0f, 0.0f };
            r.open_deg = kBreakDeg;
            r.brk      = true;
            return true;
        }
    for (const auto& d : kSingles)   // allreload: the M79, the RPG-7
        if (d.main == sig) {
            r          = Revolver{};
            r.main     = d.main & 0x10000u ? 65535u : d.main;   // (FarCry5Stereo::sig_mesh: the arcade map's RAT4s)
            for (int i = 0; i < d.nswing && i < 6; ++i) r.swing[r.nswing++] = d.swing[i];
            r.rounds   = d.rounds;
            r.round_bones[0] = 0;   // a rigid draw's vertices are kept under 0 (its parts' key is kRigidBone)
            r.nround   = 1;
            r.cap_bone = 0xFFFFu;   // none
            r.pivot    = d.door && g_door_sig.load() == sig ? glm::vec3{ g_door_p[0].load(), g_door_p[1].load(), g_door_p[2].load() } : glm::vec3{ d.pivot[0], d.pivot[1], d.pivot[2] };
            r.axis     = glm::vec3{ 1.0f, 0.0f, 0.0f };
            r.open_deg = d.deg;
            r.door     = d.door;
            r.brk      = !d.muzzle;
            r.rigid_rounds = true;
            r.noopen   = d.muzzle;
            r.muzzle   = d.muzzle;
            r.spent_gone = d.muzzle || d.door;
            r.travel   = d.travel;
            r.busy_s   = d.busy_s;
            return true;
        }
    return false;
}
void set_door_pivot(uint32_t sig, const glm::vec3& p)
{
    g_door_p[0] = p.x;
    g_door_p[1] = p.y;
    g_door_p[2] = p.z;
    g_door_sig  = sig;
}
bool bolt_fed(uint32_t sig)
{
    const Builtin* b = builtin(sig);
    return b && b->bolt && b->kind == kMagFed && g_type_on[kTypeBolt].load();
}
void set_round_lead(uint32_t sig, float m)
{
    g_lead_m   = m;
    g_lead_sig = sig;
}
uint32_t reload_sig(uint32_t sig) { return g_same_gun.load() && sig == 69646u ? 80077u : sig; }
bool grenade_seat() { return g_grenade_seat.load(); }
bool shell_hip() { return g_shell_hip.load(); }
void set_muzzle_depth(uint32_t sig, float m)
{
    g_muzzle_m   = m;
    g_muzzle_sig = sig;
}
float drop_floor() { return g_drop_floor.load(); }
bool rack_needed()
{
    std::lock_guard _{ g_mtx };
    return g_need_rack;
}

std::string take_save_line()
{
    std::lock_guard _{ g_save_mtx };
    std::string s;
    s.swap(g_save_line);
    return s;
}

void get_hand_mag(float v[3])
{
    for (int i = 0; i < 3; ++i) v[i] = g_hand_off[i].load();
}

void get_pouch(float pos[3])
{
    for (int i = 0; i < 3; ++i) pos[i] = g_pouch[i].load();
}

bool pouch_neck() { return g_pouch_neck.load(); }
bool pouch_anchor() { return g_pouch_anchor.load(); }

float rock_deg(uint32_t sig)
{
    // F11 streamlining (author 3 Nov 12:36): reloadgamehold also rocks an AK's
    // magazine in as the game's reload does (kRockGameDeg), unless reloadrockin (now a test command, no longer in F11) sets an angle.
    const Builtin* b = builtin(sig);
    if (!b || b->kind != kMagFed || std::strncmp(b->name, "AK", 2) != 0) return 0.0f;
    const float d = g_rock_deg.load();
    return d > 0.0f ? d : g_game_hold.load() ? kRockGameDeg : 0.0f;
}

bool is_listed(uint32_t sig)
{
    if (sig == 0) return false;
    {
        std::lock_guard _{ g_list_mtx };
        if (g_weapons.contains(sig)) return true;
    }
    // allreload: its kind's switch on (reloadflame, reloadgl, reloadrocket, reloadlever, reloadbreak, reloadbelt, reloadbolt; manualreload47:
    // reloadmag, reloadpump, reloadrevolver).
    const Builtin* b = builtin(sig);
    const int      t = b ? wtype(*b) : -1;
    if (t >= 0) return g_type_on[t].load();
    return !b && g_type_on[kTypeOther].load() && other_gun(sig).ok;   // allreload2: reloadother
}

Geom geom(uint32_t sig)
{
    Geom g{};
    {
        std::lock_guard _{ g_list_mtx };
        if (auto it = g_geom.find(sig); it != g_geom.end()) g = it->second;
    }
    if (g_back_sig.load() == sig && sig != 0) g.back = glm::vec3{ g_back[0].load(), g_back[1].load(), g_back[2].load() };
    return g;
}

std::string set_measured(uint32_t sig, const glm::vec3& well, const glm::vec3& axis)
{
    Revolver   rv{};
    const bool far_ok = revolver_parts(sig, rv) && (rv.muzzle || rv.door);   // allreload: the RPG-7's muzzle is further out than a magazine well
    if (!(glm::length(well) <= (far_ok ? 0.80f : 0.30f))) {
        spdlog::info("[Reload] weapon {}: a well measured {:.2f} m from the controller: not taken", sig, glm::length(well));
        return {};
    }
    std::lock_guard _{ g_list_mtx };
    Geom& g    = g_geom[sig];
    g.well     = well;
    g.axis     = glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    g.measured = true;
    return geom_line();
}

namespace
{
    std::map<uint32_t, std::pair<glm::vec3, glm::vec3>> g_handle_span;   // reloadrackzone: the handle's parts' line (g_list_mtx)
}

void set_handle_span(uint32_t sig, const glm::vec3& front, const glm::vec3& rear)
{
    if (!(glm::length(front) <= 0.60f && glm::length(rear) <= 0.60f)) return;
    std::lock_guard _{ g_list_mtx };
    g_handle_span[sig] = { front, rear };
}

std::string set_handle_point(uint32_t sig, const glm::vec3& handle)
{
    if (!(glm::length(handle) <= 0.40f)) return {};
    std::lock_guard _{ g_list_mtx };
    Geom& g = g_geom[sig];
    if (g.handle_set || glm::length(g.handle - handle) < 0.01f) return {};
    g.handle = handle;
    return geom_line();
}

// manualreload28: a learnt hold is kept only inside the box every good one fell in (reload5 and reload6: right -0.08..0.06, up 0.04..0.23,
// back -0.13..0.05 m). reload6's AKs gave 17 cm to the right (the magazine still at the gun as the hand came near), and 88912 a draw held at
// the wrist all through its reload: both outside.
bool hold_plausible(const glm::vec3& t)
{
    // manualreload53: the back limit from -0.16 to -0.09: the handgun run (4 Nov) learnt the P226's, D50's, M9's and P08's tops 10-22 cm behind the
    // hand, which the pictures show wrong (the magazine above or beside the fist); the 1911's -0.05 looked right.
    return std::abs(t.x) <= 0.10f && t.y >= 0.02f && t.y <= 0.26f && t.z >= -0.09f && t.z <= 0.08f;
}

std::string set_hold_mag(uint32_t sig, const glm::vec3& top)
{
    if (sig == 0 || !hold_plausible(top)) return {};
    std::lock_guard _{ g_hold_mtx };
    if (const auto it = g_hold_mag.find(sig); it != g_hold_mag.end() && glm::length(it->second - top) < 0.005f) return {};
    g_hold_mag[sig] = top;
    relearnt(sig, kItHold);
    return hold_line();
}

bool hold_mag(uint32_t sig, glm::vec3& top)
{
    {
        std::lock_guard _{ g_hold_mtx };
        if (const auto it = g_hold_mag.find(sig); it != g_hold_mag.end() && hold_plausible(it->second)) {   // (one saved by v27 may be outside)
            top = it->second;
            return true;
        }
    }
    for (const auto& h : kHoldBuiltin)   // manualreload27: built in from the reload5 run
        if (h.sig == sig) {
            top = glm::vec3{ h.top[0], h.top[1], h.top[2] };
            return true;
        }
    return false;
}

bool game_hold() { return g_game_hold.load(); }

bool handle_part(uint32_t sig, uint32_t& count, uint32_t& bone, bool& learnt)
{
    std::lock_guard _{ g_list_mtx };
    const auto it = g_handle_parts.find(sig);
    learnt        = it != g_handle_parts.end();
    if (!learnt) return false;
    count = it->second.first;
    bone  = it->second.second;
    return true;
}

bool handle_parts(uint32_t sig, HandleParts& h)
{
    h = HandleParts{};
    {
        std::lock_guard _{ g_list_mtx };
        if (const auto it = g_handle_parts.find(sig); it != g_handle_parts.end()) {
            h.count    = it->second.first;
            h.bones[0] = it->second.second;
            h.nbones   = 1;
            h.grab[0]  = it->second.second;
            h.ngrab    = 1;
            h.saved    = true;
            return true;
        }
    }
    // Built in (kBuiltin): the AR-C's 7 CHARGING_HANDLE, 8 and 9 its left and right tabs (where the fingers pull it), 6 the bolt it pulls
    // back (the game's reload from empty, 2 Nov 12:24, moved all four back 7.0-7.5 cm together); a pistol's slide, with its sights' draw,
    // taken at its rear.
    const Builtin* b = builtin(sig);
    if (b && b->kind == kRevolver) return true;   // no handle: known, so none is guessed or learnt (count 0)
    if (!b || b->nhandle < 1) return false;
    h.count = b->sig & 0x10000u ? 65535u : b->sig;   // (FarCry5Stereo::sig_mesh: the MG42's)
    for (int i = 0; i < b->nhandle && i < 4; ++i) h.bones[h.nbones++] = b->handle[i];
    for (int i = 0; i < b->ngrab && i < 2; ++i) h.grab[h.ngrab++] = b->grab[i];
    h.grab_rear  = b->pistol;
    // reloadhintglow2 (author 5 Nov 12:47, the AK's blue ball in the middle of the receiver): an AK's handle is its whole bolt carrier
    // (SLIDE, slot 2), whose centre is under the dust cover; the knob is its part furthest out on the right.
    if (const auto* t = fc5::reloadparts::find(sig); t && !b->pistol && b->kind == kMagFed && std::strstr(t->model, "WPN_AK47") != nullptr) h.grab_right = true;
    h.also_count = b->also;
    h.also_bone  = b->also_bone;
    h.slide      = b->pistol;
    return true;
}

bool mag_builtin(uint32_t sig)
{
    std::lock_guard _{ g_list_mtx };
    return builtin(sig) != nullptr && !g_parts.contains(sig);
}

std::string set_handle_part(uint32_t sig, uint32_t count, uint32_t bone)
{
    std::lock_guard _{ g_list_mtx };
    g_handle_parts[sig] = { count, bone };
    return handle_line();
}

void note_back(uint32_t sig, const glm::vec3& back)
{
    if (glm::length(back) < 1e-4f) return;
    const glm::vec3 b = glm::normalize(back);
    for (int i = 0; i < 3; ++i) g_back[i] = b[i];
    g_back_sig = sig;
}

bool mag_parts(uint32_t sig, MagPart* out, int max, int& n)
{
    n = 0;
    std::lock_guard _{ g_list_mtx };
    const auto it = g_parts.find(sig);
    if (it == g_parts.end() || it->second.empty()) {
        const Builtin* b = builtin(sig);
        if (!b) return false;
        for (const auto& p : builtin_parts(*b))
            if (n < max) out[n++] = p;
        return true;
    }
    for (const auto& p : it->second)
        if (n < max) out[n++] = p;
    return true;
}

bool mag_learnt(uint32_t sig)
{
    std::lock_guard _{ g_list_mtx };
    const auto it = g_parts.find(sig);
    return it != g_parts.end() && !it->second.empty();
}

std::string learn_mag(uint32_t sig, const MagPart* parts, int n)
{
    std::lock_guard _{ g_list_mtx };
    std::vector<MagPart> v;
    for (int i = 0; i < n; ++i)
        if (part_ok(parts[i])) v.push_back(parts[i]);
    if (std::none_of(v.begin(), v.end(), [](const MagPart& p) { return !p.prop; })) v.clear();
    if (v.empty()) g_parts.erase(sig);
    else {
        g_parts[sig] = v;
        relearnt(sig, kItMag);
    }
    return parts_line();
}

std::string mag_text(uint32_t sig)
{
    std::lock_guard _{ g_list_mtx };
    std::string s;
    std::vector<MagPart> v;
    if (const auto it = g_parts.find(sig); it != g_parts.end()) v = it->second;
    else if (const Builtin* b = builtin(sig)) v = builtin_parts(*b);
    for (const auto& p : v) {
        s += (s.empty() ? "" : ", ") + std::to_string(p.count);
        if (p.prop) s += " (prop, hidden)";
        else if (p.rigid) s += " (rigid)";
        else {
            s += " bone";
            for (int b = 0; b < p.nbones; ++b) s += fmt::format("{}{}", b ? "+" : " ", p.bones[b]);
        }
    }
    if (!s.empty() && !g_parts.contains(sig)) s += " (built in)";
    return s;
}

void update(const In& in_game, Out& out)
{
    std::lock_guard _{ g_mtx };
    const auto now    = clock::now();
    // reloadrackloss: from here on the clip is the game's less the rounds a rack threw out.
    In in = in_game;
    in.sig = reload_sig(in.sig);   // reloadsamegun
    // ammoledger: its guns count a rack's thrown round as surplus instead (below), so reloadrackloss is for tubes only then.
    const Builtin* bt0     = builtin(in.sig);
    const bool     led_any = !(bt0 && bt0->kind == kTube);
    if (in.sig != 0 && in.ammo_ok && in.clip >= 0 && !led_any) {
        MagLoss& ml = g_mag_loss[in.sig];
        if (ml.lost > 0 && in.clip > ml.real) {   // filled
            const int n = std::min(ml.lost, std::max(in.reserve, 0));
            if (n > 0) fc5::lua::reserve_take_request(n);
            if (g_log.load()) spdlog::info("[Reload] reloadrackloss: the clip filled ({} -> {}): the {} round(s) the rack threw out come off the reserve {}", ml.real, in.clip, ml.lost, in.reserve);
            ml.lost = 0;
        }
        ml.real = in.clip;
        in.clip = std::max(0, in.clip - ml.lost);
    }
    // ... and with the game's clip full, nothing can fill it: at a drop or a reload press they come off the reserve at once.
    auto settle_loss = [&]() {
        const auto it = g_mag_loss.find(in.sig);
        const auto cm = g_clip_max.find(in.sig);
        if (!in.ammo_ok || it == g_mag_loss.end() || it->second.lost <= 0 || cm == g_clip_max.end() || it->second.real < cm->second) return;
        const int n = std::min(it->second.lost, std::max(in.reserve, 0));
        if (n > 0) fc5::lua::reserve_take_request(n);
        if (g_log.load()) spdlog::info("[Reload] reloadrackloss: the game's clip is full ({}): the {} round(s) the rack threw out come off the reserve {}", it->second.real, it->second.lost, in.reserve);
        it->second.lost = 0;
        in.clip         = it->second.real;
    };
    const bool listed = in.listed && g_on.load();
    const bool active = listed && in.active;
    const bool b_edge = in.b_down && !g_b_was;
    g_b_was           = in.b_down;
    const bool btn_edge = g_btn_edge.exchange(false);   // reloadbutton: the reload button pressed since the last poll

    // Right B: a press that starts with a listed weapon in hand (and no left grip: left grip + B is the throw) is held back from the game.
    // Let go within reloadtap ms: the magazine drops. Held longer: from then on it is the game's Y as before (held: heal).
    bool drop = false;
    // On the sling (author 2 Nov 14:42): a listed weapon
    // the game gave hangs on the sling until the right grip takes it; a quick B is held back then too (it does nothing), not the game's
    // previous weapon.
    const bool slung_b = listed && in.slung && !active;
    if (b_edge) {
        g_b_mine = (active || slung_b) && (!in.b_mod || g_hand_mag || g_racking);   // the left grip holding the reload's magazine or handle is no modifier
        g_b_pass = !g_b_mine;
        g_b_t0   = now;
    }
    if (in.b_down) {
        if (g_b_mine && !g_b_pass && ms_since(g_b_t0, now) >= float(g_tap_ms.load())) {
            g_b_pass = true;
            if (g_log.load()) spdlog::info("[Reload] right B held {} ms: the game's Y from now (heal)", g_tap_ms.load());
        }
        if (g_b_mine && !g_b_pass) out.y_off = true;
    } else if (g_b_mine) {
        if (!g_b_pass && active) drop = true;   // let go before reloadtap: a quick press
        else if (!g_b_pass && slung_b && g_log.load())
            spdlog::info("[Reload] quick right B with weapon {} on the sling: nothing (grip the gun first; the game's previous weapon held back)", in.sig);
        g_b_mine = false;
        g_b_pass = false;
    }

    if (!listed) {
        // Another weapon (or reloadmanual off): the next listed one starts loaded (the game's own clip decides from there).
        if (g_in_hand.load() || !g_mag_in_gun || g_need_rack) reset_state(in.listed ? "reloadmanual off" : "no listed weapon in the game's hands");
    }
    if (!active) {
        // Not listed, or a vehicle, the sling, a menu, the flat screen: nothing shown or held back; a magazine out stays out.
        g_in_hand  = false;
        g_hide     = false;
        g_busy_now = false;
        g_pending_now = false;
        g_pump_now = false;
        g_pump_aim = false;
        g_mag_hand_now = false;
        g_casing_now = false;
        g_seat_hold = g_hand_was = false;
        g_btn_reload = false;
        g_btn_now    = false;
        // allreloadfire: another weapon while not active (the weapon wheel, the sling, a heal) starts the state again too. Without it the
        // next weapon kept the last one's chambers and clip (3 Nov 16:23 log: the 1887 came in with the M133's empty tube and chamber, and
        // the RPG-7 with the revolver's chambers known and the M249's clip 57, read as a shot that emptied it): held from firing for good.
        // An empty hand (0) keeps the last weapon, so the next one still starts again.
        if (in.sig != 0 && in.sig != g_sig) {
            if (g_sig != 0 && !same_rat(g_sig, in.sig)) reset_state("another weapon");   // reloadsamegun
            g_sig = in.sig;
        }
        g_pouch_in = false;
        g_stuck_t  = now;   // reloadnostuck: its wait counts only while the weapon is in the hand
        std::lock_guard v{ g_view_mtx };
        g_view = View{};
        return;
    }
    if (in.sig != g_sig) {
        if (g_sig != 0 && same_rat(g_sig, in.sig)) {
            if (g_log.load()) spdlog::info("[Reload] reloadsamegun: weapon {} now seen as {}: the same RAT4, its reload kept ({})", g_sig, in.sig, state_text());
        } else if (g_sig != 0) {
            reset_state("another weapon");
        }
        g_sig = in.sig;
    }
    g_snd_sig = in.sig;
    g_sig_now = in.sig;
    g_clip_now    = in.ammo_ok ? in.clip : -1;
    g_reserve_now = in.ammo_ok ? in.reserve : -1;
    if (!g_in_hand.load() && g_log.load()) {
        spdlog::info("[Reload] weapon {} in hand: manual reload (quick right B drops the magazine, the left hip holds the next)", in.sig);
        if (g_wsnd.load()) spdlog::info("[Reload] weapon {}: its game reload sounds: {}", in.sig, weapon_sound_text(in.sig));
    }
    g_in_hand = true;

    g_last_ok       = in.gun_ok && in.hand_ok;
    g_last_mirror   = in.mirror;
    g_last_gun_pos  = in.gun_pos;
    g_last_gun_rot  = in.gun_rot;
    g_last_hand_pos = in.hand_pos;
    g_last_hand_rot = in.hand_rot;
    const Geom g      = geom(in.sig);
    const bool m      = in.mirror;
    // A pistol (kBuiltin): its magazine goes further into the grip, its slide comes back less far than a rifle's handle.
    const Builtin* bt     = builtin(in.sig);
    // allreload2: a gun with no row (reloadother): a handgun's travel, and nothing to rack for the energy weapons and launchers.
    const Other    oth    = bt ? Other{} : other_gun(in.sig);
    const bool     pistol = (bt && bt->pistol) || oth.pistol;
    const bool     norack = (bt && bt->norack) || oth.norack;
    // A pump shotgun: a shell at a time up into the loading port, the pump worked (manualreload9).
    const bool     tube   = bt && bt->kind == kTube;
    // A revolver: its cylinder swung out by quick B, a round at a time into the next empty chamber (manualreload10).
    const bool     rev    = bt && bt->kind == kRevolver;
    // A belt-fed gun (manualreload12): the magazine is its belt, in and out only with the feed cover open.
    const bool     belt   = bt && bt->kind == kBelt;
    // A lever action (manualreload30): a tube loaded by hand, its lever worked by quick right B.
    const bool     lever  = tube && bt->lever_deg != 0.0f;
    // A break action (manualreload29): a revolver of two chambers, its fired shells thrown out as it opens.
    Revolver       rvp{};
    const bool     brk    = rev && revolver_parts(in.sig, rvp) && rvp.brk;
    if (rev) {
        g_cyl_n = std::clamp(revolver_parts(in.sig, rvp) ? rvp.nround : 6, 1, 6);
        for (int i = g_cyl_n; i < 6; ++i) g_cyl[i] = 3;
    }
    // allreload: the RPG-7 has nothing to open (always open for loading, a quick B does nothing), its rocket goes in at the muzzle and
    // leaves nothing when fired. The M79 is a break action of one chamber. A bolt action (the .308, reloadbolt) works its bolt after each shot.
    const bool     noopen = rev && rvp.noopen;
    const bool     gone   = rev && rvp.spent_gone;
    const bool     boltg  = bt && bt->bolt && bt->kind == kMagFed && g_type_on[kTypeBolt].load();
    g_cycle_gun           = (tube && !bt->semi && g_pump_shot.load()) || boltg;   // reloadhandhold
    if (noopen && !g_open) {
        g_open   = true;
        g_open_t = now - std::chrono::seconds(1);
    }
    // reloadmuzzlezone (author 5 Nov 15:51: the RPG's zone): a muzzle-loaded round goes in at the muzzle, so its
    // travel is the measured depth from the muzzle to its seated tail (the RPG-7's table 22 cm started the way in inside the tube).
    const bool  mz_known = rev && rvp.muzzle && g_muzzle_sig.load() == in.sig && g_muzzle_m.load() > 0.0f;
    // reloadratslide: the RAT4's missile goes in from the back: its whole length (the seated nose to the back, where its tail ends) is its way in.
    const bool  rat_slide = rev && rvp.door && g_rat_slide.load();
    const float door_len  = g_lead_sig.load() == in.sig && g_lead_m.load() > 0.0f ? 2.0f * g_lead_m.load() : 0.50f;
    const float travel = rat_slide ? std::clamp(door_len, 0.25f, 1.0f)
                       : mz_known ? std::clamp(g_muzzle_m.load(), 0.10f, 0.60f)
                       : rev && rvp.travel > 0.0f ? std::clamp(rvp.travel * g_travel.load() / kTravelDefault, 0.02f, 0.25f)
                                                  : std::clamp(g_travel.load() * (pistol ? 1.6f : tube ? 0.6f : rev ? 0.7f : 1.0f), 0.02f, 0.16f);
    const float rack_len = std::clamp(g_rack_len.load() * (pistol ? 0.75f : tube || boltg ? 1.3f : 1.0f), 0.02f, 0.15f);
    // reloadboltturn: the bolt worked in four steps (up, back, forward, down). Switched off with it open: closed.
    const bool  boltt    = boltg && g_bolt_turn.load();
    if (!boltt && (g_bolt_open || g_bolt_up > 0.0f)) {
        g_bolt_up   = 0.0f;
        g_bolt_open = g_bolt_pulled = g_bolt_fed = false;
        if (!g_racking) g_rack = 0.0f;
    }
    const bool  ms16_h    = bt && std::strncmp(bt->name, "MS16", 4) == 0;   // reloadwellzone
    const bool  mp5_h     = bt && std::strncmp(bt->name, "MP5", 3) == 0;   // MP5, MP5SD, MP5K: kMp5ZoneIdle
    const float zone_idle = pistol ? kSlideZoneIdle : tube ? kPumpZone : ms16_h ? kMs16ZoneIdle : mp5_h ? kMp5ZoneIdle : kHandleZoneIdle;
    const float zone_rack = ms16_h ? kMs16HandleZone : kHandleZone;

    // reloadforce (test, no headset): dry = the clip ran out (a rack is needed after the next magazine), out = the magazine dropped,
    // full = everything back as loaded.
    if (const int f = g_force.exchange(0); f != 0) {
        // take (test): a magazine in the left hand as from the pouch; geom (test): one [Reload] test geometry line below.
        // A pump shotgun: dry = every shell fired (the tube empty, a fired shell in the chamber); out = nothing (no magazine).
        if (f == 1 && tube) { g_tube = 0; g_chamber = 2; }
        else if (f == 1 && rev) { for (int i = 0; i < g_cyl_n; ++i) g_cyl[i] = gone ? 0 : 2; g_cyl_known = true; }   // a rocket: none left (allreload)
        else if (f == 1) { g_need_rack = !norack; g_mag_fresh = false; }   // nothing to rack (the flamethrower): no rack owed
        else if (f == 2) { drop = !tube || lever; }   // a revolver: the cylinder swung out or back in; a lever action: its lever worked (manualreload34)
        else if (f == 4) { g_hand_mag = true; g_take_t = now; g_engaged = false; g_insert = 0.0f; }
        else if (f == 5) { g_geom_log = true; }
        else reset_state("reloadforce full");
        if (g_log.load()) spdlog::info("[Reload] reloadforce {}: {}", f == 1 ? "dry" : f == 2 ? "out" : f == 3 ? "full" : f == 4 ? "take" : "geom", state_text());
    }

    // The ammo ledger (see Ledger): which guns, the surplus, the top-up, the trigger hold; a magazine gun's clip from here on is the real count.
    // Kept per weapon object (a gun spawned again is a new object with a real clip), else per sig.
    const bool led_mag = led_any && in.ammo_ok && in.clip >= 0 && !tube && !rev;
    const bool led_rev = led_any && in.ammo_ok && in.clip >= 0 && rev && !noopen && !gone && !(brk && bt && bt->rround) && !rvp.rigid_rounds && g_cyl_n >= 2;
    g_led_now = led_mag || led_rev;
    Ledger* led = nullptr;
    if (led_mag || led_rev) {
        const uint64_t wobj = fc5::lua::weapon_object();
        led                 = &g_ledger_st[wobj ? uint32_t(wobj ^ (wobj >> 32)) | 1u : in.sig];
        const int G         = in.clip;
        if (led->g_last >= 0 && G > led->g_last) {
            if (led->topup) {
                led->surplus += G - led->g_last;
                if (g_log.load()) spdlog::info("[Reload] ammoledger: topped up {} -> {} (nothing taken): {} surplus round(s) in the game's clip", led->g_last, G, led->surplus);
            } else if (led_mag && led->surplus > 0) {
                // A load (the mod's fill at the drop, or the game's own reload): the clip is real again, and the reserve gives the surplus it replaced.
                const int n = std::min(led->surplus, std::max(in.reserve, 0));
                if (n > 0) fc5::lua::reserve_take_request(n);
                if (g_log.load()) spdlog::info("[Reload] ammoledger: loaded {} -> {}: {} more round(s) off the reserve {} for the surplus replaced ({} left)", led->g_last, G, n, in.reserve, led->surplus - n);
                led->surplus -= n;
            } else if (led_rev) {
                led->surplus = 0;   // (a revolver's count is its chambers: the clip is a tank either way)
            }
            if (!led->topup) led->fired = 0;   // a load: the count starts again
            led->topup  = false;
            led->absorb = false;
        } else if (led->absorb && led_mag && G > 0 && g_clip_max.count(in.sig) && G >= g_clip_max[in.sig]) {
            // A load asked with the game's clip already full (of surplus): the reserve gives what the magazine was missing.
            const int n = std::min(led->surplus, std::max(in.reserve, 0));
            if (n > 0) fc5::lua::reserve_take_request(n);
            if (g_log.load()) spdlog::info("[Reload] ammoledger: loaded with the clip already full ({}): {} round(s) off the reserve {} for the surplus replaced", G, n, in.reserve);
            led->surplus -= n;
            led->absorb = false;
        }
        if (led->g_last >= 0 && G < led->g_last) led->fired += led->g_last - G;   // shots (every one a real round: the trigger stops at 0)
        led->g_last = G;
        led->surplus = std::clamp(led->surplus, 0, std::max(0, G));
        int live_now = 0;
        for (int i = 0; i < g_cyl_n; ++i) live_now += g_cyl[i] == 1;
        const int real = led_mag ? G - led->surplus : (g_cyl_known ? live_now : G);
        if (led_rev && led->real_last == 0 && real > 0) led->fired = 0;   // a revolver loaded again by hand
        if (real == 0 && led->real_last > 0 && g_log.load())
            spdlog::info("[Reload] ammoledger: weapon {} fired empty: {} round(s) since its load, no game reload (the game's clip at {}, {} surplus)", in.sig, led->fired, G, led->surplus);
        led->real_last = real;
        g_led_real     = real;
        if (real >= 2 || real <= 0) { g_led_arm = false; g_led_shot_hold = false; }   // a load, or the empty gun's own hold from here
        else if (!g_led_shot_hold.load()) g_led_arm = true;                          // one round left: its shot holds the trigger at once
        // The top-up: the game's clip at 1 with a real round still in (its next shot would take the game's clip to 0).
        if (G <= 1 && real >= 1) {
            if (!led->topup || ms_since(led->t, now) > 2000.0f) {
                if (led->topup && g_log.load()) spdlog::info("[Reload] ammoledger: the top-up asked {:.0f} ms ago never landed (clip {}): asked again; the round stays held", ms_since(led->t, now), G);
                led->topup = true;
                led->t     = now;
                fc5::lua::clip_fill_request(0);
                if (g_log.load()) spdlog::info("[Reload] ammoledger: the game's clip at {} with {} real round(s): topped up silently (no reload of the game's, nothing taken)", G, real);
            }
            g_led_hold = true;
        } else {
            g_led_hold = false;
        }
        if (led_mag) in.clip = std::max(0, real);
    } else {
        g_led_hold      = false;
        g_led_real      = -1;
        g_led_arm       = false;
        g_led_shot_hold = false;
    }

    // The game's ammo. Its clip at 0: the round in the chamber is gone too, so the handle must be racked once a fresh magazine is in. The
    // game then reloads by itself (bAutoReload): kept out of the view as the tapped one.
    const int prev_clip = g_last_clip;
    if (tube && in.ammo_ok && in.clip >= 0) {
        // A pump shotgun's own count: the shells loaded by hand and the chamber, apart from the game's clip (which its own reload fills, a
        // shell at a time, when one goes in by hand). Taken from the clip when the gun comes into the hand loaded.
        if (g_tube < 0) {
            g_chamber = in.clip > 0 ? 1 : 0;
            g_tube    = std::max(0, in.clip - 1);
            if (g_log.load()) spdlog::info("[Reload] pump shotgun {} in hand: {} shell(s) in the tube, the chamber {} (from the game's clip {}, reserve {})", in.sig, g_tube,
                                           g_chamber ? "loaded" : "empty", in.clip, in.reserve);
        } else if (prev_clip >= 0 && in.clip < prev_clip) {
            // A shot (the clip went down): the chamber now holds a fired shell, to be pumped out (reloadpumpshot), or the next shell is
            // chambered by itself (reloadpumpshot off).
            for (int i = 0; i < prev_clip - in.clip; ++i) {
                if ((!g_pump_shot.load() || bt->semi) && g_tube > 0) {
                    g_tube--;
                    g_chamber = 1;
                } else {
                    g_chamber = 2;
                }
            }
            if (g_log.load()) spdlog::info("[Reload] shot (clip {} -> {}): {} shell(s) in the tube, the chamber {}", prev_clip, in.clip, g_tube,
                                           g_chamber == 1 ? "loaded" : "holding the fired shell: work the pump");
        }
    }
    if (rev && in.ammo_ok && in.clip >= 0) {
        // A revolver's chambers: taken from the clip when it comes into the hand (that many live rounds); each shot fires the next live one.
        if (!g_cyl_known) {
            g_cyl_known = true;
            g_cyl_next  = 0;
            for (int i = 0; i < 6; ++i) g_cyl[i] = i >= g_cyl_n ? 3 : i < in.clip ? 1 : 0;
            if (g_log.load()) spdlog::info("[Reload] {} {} in hand: {} live round(s) in the {} (from the game's clip, reserve {})", brk ? "break action" : "revolver", in.sig,
                                           std::min(in.clip, g_cyl_n), brk ? "barrels" : "cylinder", in.reserve);
            // reloadlauncherfill: a launcher taken in hand empty: its clip filled at once, so the game draws its round (at the hip); the chamber stays empty.
            if (g_launcher_fill.load() && rvp.rigid_rounds && in.clip == 0 && in.reserve > 0 && !g_game_pending && !g_key_pending) {
                g_key_pending  = true;
                g_game_pending = true;
                g_clip_at_tap  = in.clip;
                g_tap_t        = now;
                g_retaps       = 2;
                g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
                if (g_log.load()) spdlog::info("[Reload] reloadlauncherfill: launcher {} in hand empty (reserve {}): its clip filled at once (no reload of the game's), so its round shows at the hip; the chamber stays empty: load it by hand",
                                               in.sig, in.reserve);
            }
        } else if (prev_clip >= 0 && in.clip < prev_clip) {
            for (int n = 0; n < prev_clip - in.clip; ++n)
                for (int k = 0; k < g_cyl_n; ++k) {
                    const int i = (g_cyl_next + k) % g_cyl_n;
                    if (g_cyl[i] != 1) continue;
                    g_cyl[i]    = gone ? 0 : 2;   // a rocket leaves nothing (allreload)
                    g_cyl_next  = (i + 1) % g_cyl_n;
                    break;
                }
            int live = 0;
            for (const int c : g_cyl) live += c == 1;
            if (g_log.load()) spdlog::info("[Reload] shot (clip {} -> {}): {} live round(s) left in the cylinder", prev_clip, in.clip, live);
            // reloadrocketfill (author 5 Nov: the RAT's rocket; RPG: "leftover of animation after shooting (automatic reload)"): the hip's rocket is drawn in the game's own draw of it, and the game draws
            // none with an empty clip; its automatic reload starts then, too. Filled at once (reloadnogame), it has nothing to reload, and the mod's count says the launcher is empty.
            // reloadgrenadefill: the M79 too (its fired grenade stays in the chamber, counted fired: it fires again only loaded by hand).
            if ((gone || (g_grenade_fill.load() && rvp.rigid_rounds)) && in.clip == 0 && in.reserve > 0 && !g_game_pending && !g_key_pending) {
                g_key_pending  = true;
                g_game_pending = true;
                g_clip_at_tap  = in.clip;
                g_tap_t        = now;
                g_retaps       = 2;
                g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
                if (g_log.load()) spdlog::info("[Reload] {}: the clip filled at once: the {} stays at the hip, no reload of the game's", gone ? "reloadrocketfill" : "reloadgrenadefill",
                                               gone ? "rocket" : "grenade");
            }
        }
    }
    // A bolt action's shot (allreload, reloadbolt): the fired case stays in the chamber until the bolt is worked. The last round of the
    // magazine is the charging handle's rack after the next magazine, as before (below).
    if (boltg && in.ammo_ok && in.clip >= 0 && prev_clip >= 0 && in.clip < prev_clip) {
        const int dry_b = (keep_one() && keep_res(in.reserve)) ? 1 : 0;
        if (in.clip > dry_b && !g_bolt_owed) {
            g_bolt_owed = true;
            if (g_log.load()) spdlog::info("[Reload] shot (clip {} -> {}): work the bolt ({})", prev_clip, in.clip,
                                           boltt ? "grip on it: up, back to its stop, forward, down" : "left grip on it, back to its stop, let go");
        }
    }
    // The shot's own cycle (mustPrepareShot): the game plays it by itself, and the hand's stroke plays it again.
    if (in.ammo_ok && prev_clip >= 0 && in.clip < prev_clip && !g_btn_reload && g_cycle_gun.load())
        g_cycle_until = (now + std::chrono::milliseconds(kCycleMs)).time_since_epoch().count();   // reloadhandhold
    if (in.ammo_ok && prev_clip >= 0 && in.clip < prev_clip && !g_btn_reload && g_shot_mute.load() && ((tube && !bt->semi && g_pump_shot.load()) || boltg)) fc5::gamesound::mute_handling(1500);   // (the M133's own pump sound comes as late as 1.2 s: run final_a's recording)
    if (in.ammo_ok && in.clip >= 0) {
        int& cmax = g_clip_max[in.sig];
        cmax      = std::max(cmax, in.clip);
        // reloadkeepone: a magazine or belt-fed gun runs dry with one round left in the game's clip (no game reload then).
        const int dry_at = (keep_one() && !tube && !rev && keep_res(in.reserve)) ? 1 : 0;
        if (in.clip <= dry_at && in.clip != g_last_clip && !(g_last_clip >= 0 && g_last_clip <= dry_at) && !g_btn_reload) {   // reloadbutton: not the game's reload from its button
            if (!g_need_rack && !tube && !rev && !norack) {
                g_need_rack = true;
                g_mag_fresh = false;
                if (g_log.load())
                    spdlog::info("[Reload] the clip ran dry (reserve {}): the handle must be racked after the next magazine; {}", in.reserve,
                                 dry_at ? "one round kept in the game's clip (reloadkeepone): the game starts no reload of its own"
                                        : "the game's own reload kept out of the view");
            }
            if (in.clip == 0)
                g_busy_until = std::max(g_busy_until, now + std::chrono::milliseconds(int(std::max(kBusySecs, rev ? rvp.busy_s : 0.0f) * 1000.0f)));
        }
        if (g_game_pending && g_clip_at_tap >= 0 && in.clip > g_clip_at_tap) {
            g_game_pending = false;
            g_busy_until   = std::min(g_busy_until, now + std::chrono::milliseconds(900));   // the animation's end
            if (g_log.load()) spdlog::info("[Reload] the game refilled the clip {} -> {} (reserve {}) {:.0f} ms after its reload was tapped", g_clip_at_tap, in.clip, in.reserve, ms_since(g_tap_t, now));
        }
        // A pump shotgun's game reload puts a shell in at a time: kept out of the view while the clip keeps growing.
        if ((tube || rev) && !g_btn_reload && prev_clip >= 0 && in.clip > prev_clip && now < g_busy_until + std::chrono::milliseconds(300))
            g_busy_until = std::max(g_busy_until, now + std::chrono::milliseconds(1100));
        g_last_clip = in.clip;
    }
    if (g_game_pending && !(in.ammo_ok && g_clip_at_tap >= 0) && ms_since(g_tap_t, now) > kBusySecs * 1000.0f) {
        g_game_pending = false;   // no ammo read: the reload's longest time stands in
        if (g_log.load()) spdlog::info("[Reload] no clip read from the game: its reload taken as done {:.0f} ms after the tap", ms_since(g_tap_t, now));
    }

    // A spare magazine: the reserve, or the rounds the game's own reload already put in the clip after it ran dry while the empty magazine is
    // still in by hand (2 Nov 12:25: the clip ran dry with 40 in reserve, the game reloaded them all unseen, then reserve 0 refused the drop
    // and emptied the hip for good, the handle still to rack: no way to fire again).
    // (More than the round reloadkeepone keeps there: author 5 Nov, AR-C run dry: that one round read as loaded by the game, the drop filled nothing, the handle racked, and the gun held at clip 1 asking for a drop.)
    const bool owed     = g_need_rack && !g_mag_fresh && in.ammo_ok && in.clip > (keep_one() && keep_res(in.reserve) ? 1 : 0);
    // A pump shotgun: a shell to load while the game holds more than is loaded by hand (its clip, filled by its own reload) or has a reserve,
    // and while the tube has room (the largest clip seen).
    const int  by_hand   = std::max(0, g_tube) + (g_chamber == 1 ? 1 : 0);
    const auto cmax_it   = g_clip_max.find(in.sig);
    const int  tube_cap  = std::max(cmax_it != g_clip_max.end() ? cmax_it->second : 0, bt && bt->clip > 0 ? bt->clip : 8);
    const bool tube_full = tube && by_hand >= tube_cap;
    // reloadkeepone (manualreload45): a shotgun or revolver keeps one round more in the game's clip than it holds by hand.
    // (8 Nov, author 20:44: the revolver fires all 6 with no game reload, the Sledge played the game's at empty: a break action of two barrels
    // keeps one too: the clip at 1 with a barrel still live is filled again by the same hidden tap as the revolver's (reloadnogame, below),
    // so both barrels fire and the clip never reaches 0. Not the M79: one round, nothing to keep beside it: its clip is filled at 0 by
    // reloadgrenadefill, the same hidden tap.)
    const bool keep_tr   = keep_one() && (tube || (rev && !noopen && !(brk && bt && bt->rround))) && tube_cap >= (brk ? 2 : 4);
    const bool keep_last = keep_tr && in.ammo_ok && in.clip <= 1 && keep_res(in.reserve);
    int        cyl_live = 0, cyl_target = -1;
    // reloadbutton (manualreload48): the reload button pressed: the game's own reload, shown; the hands' part dropped, the weapon loaded.
    if (btn_edge && g_btn_on.load()) {
        settle_loss();
        fc5::gamesound::mute_handling(0);
        if (g_log.load())
            spdlog::info("[Reload] reloadbutton: the reload button pressed with weapon {} (clip {}, reserve {}; was {}): the game's own reload, shown; "
                         "the weapon loaded from its clip", in.sig, in.clip, in.reserve, state_text());
        g_hand_mag = g_engaged = g_racking = g_rack_done = false;
        g_seat_hold = g_hand_was = false;
        g_insert = g_rack = 0.0f;
        g_lever_req = g_lever_cyc = g_lever_hand = g_lever_down = g_act_open = false;
        if (g_open && !noopen) {
            g_open   = false;
            g_open_t = now - std::chrono::seconds(1);   // shut at once: the game's reload plays its own
        }
        g_mag_in_gun   = true;
        g_need_rack    = g_mag_fresh = g_bolt_owed = false;
        g_bolt_up      = 0.0f;   // reloadboltturn: the game's reload leaves it closed
        g_bolt_open    = g_bolt_pulled = g_bolt_fed = false;
        g_game_pending = g_key_pending = false;
        g_busy_until   = now;
        g_btn_reload   = true;
        g_btn_t = g_btn_grow_t = now;
        g_btn_clip     = in.ammo_ok ? in.clip : -1;
        // (A press with the magazine out: reloadnogame filled the clip at the drop, so the game has no reload to play: the gun is ready at once, not 3 s later. Run button3.)
        const auto cm  = g_clip_max.find(in.sig);
        g_btn_nothing  = in.ammo_ok && cm != g_clip_max.end() && in.clip >= cm->second;
    }
    if (g_btn_reload && (g_hand_mag || g_engaged || g_racking || !g_mag_in_gun)) {
        g_btn_reload = false;   // the hands took over again (a magazine dropped or taken, a handle racked)
        if (g_log.load()) spdlog::info("[Reload] reloadbutton: the hands took over from the game's reload ({})", state_text());
    }
    if (g_btn_reload) {
        if (in.ammo_ok && in.clip > g_btn_clip) {
            g_btn_clip   = in.clip;
            g_btn_grow_t = now;
        }
        if (in.ammo_ok && in.clip >= 0) {
            // As reloadnostuck: the game's clip, one fewer while reloadkeepone keeps one more there.
            const int avail = std::max(0, keep_tr && keep_res(in.reserve) ? in.clip - 1 : in.clip);
            if (tube) {
                g_chamber = avail > 0 ? 1 : 0;
                g_tube    = std::max(0, avail - 1);
            } else if (rev) {
                for (int i = 0; i < g_cyl_n; ++i) g_cyl[i] = i < avail ? 1 : 0;
                g_cyl_known = true;
                g_cyl_next  = 0;
            }
        }
        const float since = ms_since(g_btn_t, now);
        // (Its clip grows late in the animation, a rifle's up to 2 s in: 3 s before giving up on a clip that has not grown at all. Until now 1.5 s: the view went back to hiding the reload half way.)
        const bool grown = g_btn_grow_t != g_btn_t;
        const float quiet = grown ? kBtnQuietMs : g_btn_nothing ? 300.0f : 3000.0f;
        if (since > kBtnMaxMs || (since > quiet && ms_since(g_btn_grow_t, now) > quiet)) {
            g_btn_reload = false;
            if (g_log.load()) spdlog::info("[Reload] reloadbutton: the game's reload over after {:.1f} s (clip {}, reserve {}): {}", since / 1000.0f, in.clip, in.reserve, state_text());
        }
    }
    g_btn_now = g_btn_reload;
    for (int i = 0; i < g_cyl_n; ++i) {
        cyl_live += g_cyl[i] == 1;
        if (cyl_target < 0 && g_cyl[i] == 0) cyl_target = i;
    }
    const bool cyl_full  = rev && g_cyl_known && cyl_target < 0;
    const bool no_spare  = tube ? in.ammo_ok && in.reserve <= 0 && in.clip <= by_hand
                         : rev  ? in.ammo_ok && in.reserve <= 0 && in.clip <= cyl_live
                                : in.ammo_ok && in.reserve == 0 && !owed;
    const float open_t   = ms_since(g_open_t, now) / (kSwingSecs * 1000.0f);
    float       open_f   = std::clamp(g_open ? open_t : 1.0f - open_t, 0.0f, 1.0f);
    // reloadcoverpush: the feed cover in the left hand follows it down; shut at the bottom, back up if let go.
    if (g_cover_hold && (!belt || !g_open || g_hand_mag || g_engaged)) g_cover_hold = false;
    if (g_cover_hold) {
        if (!in.grip || !in.gun_ok || !in.hand_ok) {
            g_cover_hold = false;
            g_open_t     = now - std::chrono::milliseconds(int(g_cover_f * kSwingSecs * 1000.0f));   // back up from where it is
            open_f       = g_cover_f;
            if (g_log.load()) spdlog::info("[Reload] reloadcoverpush: the cover let go at {:.0f} % of its lift: it springs back open (push it all the way down)", g_cover_f * 100.0f);
        } else {
            const float h  = glm::dot(in.hand_pos - in.gun_pos, in.gun_rot[1]);   // the hand's height along the gun's up
            g_cover_f      = std::clamp(g_cover_f0 - (g_cover_h0 - h) / kCoverPushTravel, 0.0f, 1.0f);
            const int tick = int(g_cover_f * 10.0f);
            if (tick < g_cover_tick) buzz(out, kHand, 0.15f, 0.012f);   // a tick per tenth of the way down
            g_cover_tick = tick;
            open_f       = g_cover_f;
            if (g_cover_f <= kCoverShutAt) {
                g_cover_hold = false;
                g_open       = false;
                g_open_t     = now - std::chrono::seconds(1);
                open_f       = 0.0f;
                g_need_rack  = true;   // as reloadbeltgrip's left grip: the handle is racked next
                g_mag_fresh  = true;
                sound(kSndRackFwd);
                buzz(out, kHand, 0.6f, 0.05f);
                buzz(out, kGun, 0.5f, 0.05f);
                if (g_log.load()) spdlog::info("[Reload] reloadcoverpush: the feed cover pushed down shut ({:.1f} cm down): rack the charging handle", (g_cover_h0 - h) * 100.0f);
            }
        }
    }
    if (drop && noopen) {
        drop = false;
        if (g_log.load()) spdlog::info("[Reload] quick right B with the {}: nothing to open (take a rocket from the left hip, push its tail into the muzzle)", bt->name);
    }
    if (drop && rev) {
        drop = false;
        if (g_engaged) {
            if (g_log.load()) spdlog::info("[Reload] quick right B: a round is going into the cylinder: it stays open");
        } else {
            g_open   = !g_open;
            g_open_t = now - std::chrono::milliseconds(int((1.0f - open_f) * kSwingSecs * 1000.0f));   // from where it is
            sound(g_open ? kSndMagOut : kSndRackFwd);
            buzz(out, kGun, 0.4f, 0.04f);
            if (g_log.load()) spdlog::info("[Reload] quick right B: the {} ({})", brk ? (g_open ? "action broken open" : "action closed") : g_open ? "cylinder swung out" : "cylinder swung back in",
                                           state_text());
        }
    }
    if (drop && belt) {
        // The feed cover: quick B lifts it (the belt in the gun comes out with it: the drop below) and closes it again.
        if (g_engaged) {
            drop = false;
            if (g_log.load()) spdlog::info("[Reload] quick right B: a belt is going into the tray: the cover stays open");
        } else if (g_cover_push.load() && g_belt_grip.load() && (g_cover_hold || (g_open && g_mag_in_gun))) {
            // reloadcoverpush: over a new belt the cover is shut by the hand, pushed down.
            drop = false;
            if (g_log.load()) spdlog::info("[Reload] quick right B: a new belt is in the tray: push the feed cover down with the left hand (reloadcoverpush)");
        } else {
            g_open   = !g_open;
            g_open_t = now - std::chrono::milliseconds(int((1.0f - open_f) * kSwingSecs * 1000.0f));
            buzz(out, kGun, 0.4f, 0.04f);
            if (g_log.load()) spdlog::info("[Reload] quick right B: the feed cover {} ({})", g_open ? "lifted" : "closed", state_text());
            if (!g_open) {
                sound(kSndRackFwd);
                drop = false;
                // reloadbeltgrip: closed on a new belt, the handle is racked next (as with the left grip)
                if (g_belt_grip.load() && g_mag_in_gun && g_seat_t > g_drop_t) {
                    g_need_rack = true;
                    g_mag_fresh = true;
                }
            } else if (!g_mag_in_gun) {
                sound(kSndMagOut);
                drop = false;
            }   // else the belt comes out (the drop, its sound)
        }
    }
    if (drop && tube) {
        drop = false;
        // reloadactionopen (author 5 Nov): below.
        if (g_act_open) g_act_close_req = true;
        else if (g_act_on.load()) g_act_open_req = true;
        else if (lever) g_lever_req = true;   // worked below
        else if (g_log.load()) spdlog::info("[Reload] quick right B with a pump shotgun: nothing (shells go in one at a time from the left hip; the pump is the handle)");
    }

    // The drop.
    if (drop) {
        if (!g_mag_in_gun) {
            if (g_log.load()) spdlog::info("[Reload] quick right B: the magazine is already out");
        } else if (no_spare) {
            buzz(out, kGun, 0.3f, 0.03f);
            if (g_log.load()) spdlog::info("[Reload] quick right B: no spare ammo (reserve 0, clip {}): the magazine stays in", in.clip);
        } else {
            g_mag_in_gun = false;
            g_engaged    = false;
            g_drop_t     = now;
            g_drops++;
            g_drop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            buzz(out, kGun, 0.35f, 0.04f);
            sound(kSndMagOut);
            settle_loss();
            const auto it   = g_clip_max.find(in.sig);
            const bool full = in.ammo_ok && it != g_clip_max.end() && in.clip >= it->second && in.clip > 0;
            if (full || owed) {
                if (g_log.load()) spdlog::info("[Reload] magazine dropped (weapon {}, clip {} {}, reserve {}): no game reload needed", in.sig, in.clip,
                                               owed ? "loaded by the game's own reload after running dry" : "full", in.reserve);
            } else {
                g_key_pending  = true;
                g_game_pending = true;
                g_clip_at_tap  = in.ammo_ok ? in.clip : -1;
                g_tap_t        = now;
                g_retaps       = 0;
                g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
                if (g_log.load()) spdlog::info("[Reload] magazine dropped (weapon {}, clip {}, reserve {}): the game's reload tapped ({}), its animation kept out of the view", in.sig,
                                               in.ammo_ok ? std::to_string(in.clip) : std::string{ "?" }, in.ammo_ok ? std::to_string(in.reserve) : std::string{ "?" },
                                               g_key.load() == 1 ? "pad X" : "key R");
            }
        }
    }

    // manualreload52: "Something wrong" (F11): the game's own reload of the gun in hand, watched and learnt (FarCry5ReloadMag.cpp), as at a drop
    // but with the magazine left in. It waits for room in the clip (one shot), and goes with the weapon.
    static bool relearn_wait = false;
    if (g_relearn_req.exchange(0) != 0) relearn_wait = true;
    if (relearn_wait && g_relearn_sig.load() != in.sig) relearn_wait = false;
    // manualreload55: the wait ends once no part it is for is left to learn (his log of 4 Nov 12:31-12:33: the sounds were learnt from the
    // drop's own game reload, the wait stayed armed and tapped the game's reload after a later shot; the hand on the handle, which needs a
    // reload from empty, is not waited for here).
    if (relearn_wait && !(g_relearn_items.load() & ((1u << kItMag) | (1u << kItHold) | (1u << kItSound)))) relearn_wait = false;
    if (g_empty_sig.load() != 0 && g_empty_sig.load() != in.sig) {
        relearn_say(fmt::format("weapon {} left the hands: its reload from empty not learnt (reloadkeepone back on)", g_empty_sig.load()));
        g_empty_sig = 0;
    }
    if (g_empty_sig.load() == in.sig && in.ammo_ok && prev_clip == 0 && in.clip > 0) {
        g_empty_sig = 0;   // the game's reload from empty done: reloadkeepone back on
        if (g_relearn_items.load() & (1u << kItGrab)) relearn_say(fmt::format("weapon {}: the game's reload from empty done, but its hand on the handle was not seen (see the log)", in.sig));
    }
    if (relearn_wait) {
        const auto cm   = g_clip_max.find(in.sig);
        const bool full = in.ammo_ok && cm != g_clip_max.end() && in.clip >= cm->second;
        if (tube || rev) {
            relearn_wait = false;
            relearn_say("only a magazine gun can be learnt again in the game for now (a shotgun's or revolver's reload is learnt in the simulator runs)");
        } else if (!g_mag_in_gun || g_hand_mag || g_engaged || g_racking || g_game_pending || now < g_busy_until) {
            // the reload by hand first
        } else if (!in.ammo_ok || in.reserve <= 0) {
            relearn_wait = false;
            relearn_say("no spare ammo: the game will not reload");
        } else if (full) {
            static clock::time_point said{};
            if (ms_since(said, now) > 10000.0f) {
                said = now;
                relearn_say(fmt::format("weapon {}: the clip is full: fire one shot (into the ground) and the game's reload starts", in.sig));
            }
        } else {
            relearn_wait   = false;
            g_tap_real     = true;
            g_key_pending  = true;
            g_game_pending = true;
            g_clip_at_tap  = in.clip;
            g_tap_t        = now;
            g_retaps       = 0;
            g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
            relearn_say(fmt::format("weapon {}: the game's reload started (clip {}): watching it", in.sig, in.clip));
        }
    }

    const bool      track = in.gun_ok && in.hand_ok;
    const glm::vec3 well  = in.gun_pos + in.gun_rot * mirror_x(g.well, m);
    glm::vec3       axis  = in.gun_rot * mirror_x(g.axis, m);
    axis                  = glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    const glm::vec3 handle = in.gun_pos + in.gun_rot * mirror_x(g.handle, m);
    // reloadrackzone: the hand's distance to the handle's parts' line (a handle placed by hand: its point only).
    float rack_line_d = 1e9f;
    {
        std::pair<glm::vec3, glm::vec3> sp{};
        bool                            have_sp = false;
        {
            std::lock_guard sl{ g_list_mtx };
            if (const auto it = g_handle_span.find(in.sig); it != g_handle_span.end() && !g.handle_set) { sp = it->second; have_sp = true; }
        }
        if (have_sp) {
            const glm::vec3 a  = in.gun_pos + in.gun_rot * mirror_x(sp.first, m);
            const glm::vec3 ab = in.gun_pos + in.gun_rot * mirror_x(sp.second, m) - a;
            const float     t  = glm::dot(ab, ab) > 1e-8f ? std::clamp(glm::dot(in.hand_pos - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            rack_line_d        = glm::length(in.hand_pos - (a + ab * t));
        }
    }
    glm::vec3       back   = in.gun_rot * mirror_x(g.back, m);
    back                   = glm::length(back) > 1e-4f ? glm::normalize(back) : glm::vec3{ 0.0f, 0.0f, 1.0f };
    // reloadboltturn: the gun's up (the well's axis, square to its back): the hand rises along it to turn the bolt up.
    glm::vec3       bup    = axis - back * glm::dot(axis, back);
    bup                    = glm::length(bup) > 1e-3f ? glm::normalize(bup) : in.gun_rot[1];
    // A muzzle-loaded rocket (allreload): held by its middle, 2 cm above the left controller; its tail, the point that goes into the
    // muzzle, behind it by the length the model gave (set_round_lead; 0.25 m until measured).
    const float     lead_m = g_lead_sig.load() == in.sig && g_lead_m.load() > 0.0f ? g_lead_m.load() : 0.25f;
    HandGame        hg{};
    const bool      hg_on  = !rev && !tube && !m && hand_game(in.sig, hg);   // (the left-handed hold is not recorded: as before)
    const bool      hg_mag = hg_on && hg.mag && !mag_lower(in.sig);
    // reloadhandgame: a shell or round where the SPAS-12's own left hand holds one (the game's only left hand that loads them): pointing along the well as ever.
    HandGame        hs{};
    const bool      hs_on  = tube && !m && hand_game(in.sig, hs) && hs.mag;   // (not a revolver's round: author 5 Nov: between the index and the thumb, not in the palm)
    // (reloadrocketgrip, author 5 Nov: its tail this far behind the hand, the hand on its lower part, no longer at its middle.)
    // (reloadratdoor: nose first from behind: the same grip on its lower part, so the nose, the lead point, is far ahead of the hand.)
    const glm::vec3 hoff = rev && rvp.door ? glm::vec3{ 0.0f, 0.02f, -std::max(0.0f, 2.0f * lead_m - 0.07f) }
                         : rev && rvp.muzzle ? glm::vec3{ 0.0f, 0.02f, std::min(lead_m, 0.07f) }
                         : hs_on ? hs.mag_c
                         : (rev || tube) && g_round_pinch.load() ? [&] {
                                      // reloadroundpinch: the round's or shell's base between the thumb and index tips, it points along the well's axis.
                                      const glm::vec3 ua = glm::length(g.axis) > 1e-4f ? glm::normalize(g.axis) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                                      // (kRoundPinchLen less 5 cm: in the pictures a round stood that far past the finger tips: run fam5, the M629.)
                                      const glm::vec3 t  = glm::vec3{ kPinchAt[0], kPinchAt[1], kPinchAt[2] } + ua * (tube ? kShellPinchLen : kRoundPinchLen - 0.05f);
                                      return glm::vec3{ (m ? -1.0f : 1.0f) * t.x, t.y, t.z };
                                  }()
                         : rev  ? glm::vec3{ (m ? -1.0f : 1.0f) * kRoundHand[0], kRoundHand[1], kRoundHand[2] }
                         : tube ? glm::vec3{ (m ? -1.0f : 1.0f) * kShellHand[0], kShellHand[1], kShellHand[2] }
                                : [&] {
                                      glm::vec3 t{ g_hand_off[0].load(), g_hand_off[1].load(), g_hand_off[2].load() };
                                      HlBox     mb{};
                                      const bool learnt_hold = uses_learnt(in.sig, kItHold);   // manualreload52: learnt again by its button
                                      // reloadhandgame: the game's hold: its top from the magazine's centre, turned as the game holds it.
                                      if (hg_mag && !learnt_hold && highlight(in.sig, kHlMag, mb)) {
                                          t = hg.mag_c + hg.mag_turn * (g.well - mb.c);
                                      } else if (g_mag_palm.load() && !learnt_hold) {
                                          // reloadmagpalm: the fist at the middle of the magazine's length (7 cm below its top at most), on its
                                          // body (a curved one's middle is ahead of its top): the top at the fist + the axis that far - that offset.
                                          if (highlight(in.sig, kHlMag, mb) && glm::length(g.axis) > 1e-4f) {
                                              const glm::vec3 ua  = glm::normalize(g.axis);
                                              const glm::vec3 d   = mb.c - g.well;
                                              const glm::vec3 lat = d - ua * glm::dot(d, ua);
                                              // reloadmaglower: its lower half, a little over half its length below its top (mb.a[1]: half its length). (3/4 left the hand under the AK's curved magazine, a gap between them: run final_c.)
                                              const float     dep = mag_lower(in.sig) ? std::clamp(glm::length(mb.a[1]) * 1.15f, 0.03f, 0.14f) : std::clamp(glm::length(mb.a[1]), 0.02f, 0.07f);
                                              const glm::vec3 at  = mag_lower(in.sig) ? fist_at(in.sig) : glm::vec3{ kPalmAt[0], kPalmAt[1], kPalmAt[2] };
                                              t                   = at + ua * dep - lat;
                                          }
                                          // reloadmagfist (manualreload54): a handgun's magazine in the fist's middle sideways, beside the controller
                                          // toward the other hand as the gun's grip is beside the other controller.
                                          if (pistol && g_mag_fist.load() && !mag_lower(in.sig)) t.x += std::clamp(-g.well.x, 0.0f, 0.07f);
                                      } else if (g_game_hold.load() || learnt_hold) {
                                          hold_mag(in.sig, t);   // reloadgamehold: where the game's hand holds it
                                      }
                                      return glm::vec3{ (m ? -1.0f : 1.0f) * t.x, t.y, t.z };
                                  }();
    if (g_geom_log) {
        // reloadforce geom (test scripts, tools\manualreload): this weapon's numbers in the weapon controller's axes (x right, y up, z back;
        // the right hand's, not mirrored), from which a script puts the simulated left controller at the well or the handle.
        g_geom_log = false;
        const glm::vec3 bk = glm::transpose(in.gun_rot) * back;
        spdlog::info("[Reload] test geometry weapon {}: well {:.4f} {:.4f} {:.4f} axis {:.4f} {:.4f} {:.4f} handle {:.4f} {:.4f} {:.4f} back {:.4f} {:.4f} {:.4f} "
                     "travel {:.3f} rack {:.3f} handmag {:.3f} {:.3f} {:.3f} measured {} pistol {} tube {} lever {} magturn {}",
                     in.sig, g.well.x, g.well.y, g.well.z, g.axis.x, g.axis.y, g.axis.z, g.handle.x, g.handle.y, g.handle.z, bk.x, bk.y, bk.z, travel, rack_len,
                     (m ? -1.0f : 1.0f) * hoff.x, hoff.y, hoff.z, g.measured ? 1 : 0, pistol ? 1 : 0, tube ? 1 : rev ? 2 : belt ? 3 : 0, lever ? 1 : 0,
                     [&] {
                         const glm::quat q = glm::quat_cast(hg_mag && !uses_learnt(in.sig, kItHold) ? hg.mag_turn : glm::mat3{ 1.0f });
                         return fmt::format("{:.4f} {:.4f} {:.4f} {:.4f}", q.w, q.x, q.y, q.z);
                     }());
    }
    // (reloadmaglower: with the hand round the lower half, its top is up to 16 cm from the hand, and a 30 deg turn of the wrist swung it out of the well: author AK 14:02: taken and 3 times. The hand's place
    // decides, not its turn.)
    const glm::vec3 top    = in.hand_pos + (!tube && !rev && !belt && mag_lower(in.sig) ? in.gun_rot : in.hand_rot) * hoff;   // the held magazine's top
    // The held magazine's own axis: it sits in the left hand as it sits in the gun on the right controller (FarCry5ReloadMag.cpp, kHand), so
    // its axis is the well's axis turned by the left controller (until 2 Nov 14:42 the left controller's up: a pistol's magazine, 35 deg
    // from it, had to be held tilted, "the magazine had to go to a weird place before the animation started").
    // reloadhandgame: turned as the game's hand holds it.
    glm::vec3       mag_up = in.hand_rot * ((hg_mag && !uses_learnt(in.sig, kItHold) ? hg.mag_turn : glm::mat3{ 1.0f }) * mirror_x(g.axis, m));
    mag_up                 = glm::length(mag_up) > 1e-4f ? glm::normalize(mag_up) : glm::normalize(in.hand_rot[1]);
    const glm::vec3 d      = top - well;
    const float     along  = glm::dot(d, axis);                // 0 seated, -travel at the start of the travel
    const float     lat    = glm::length(d - axis * along);
    const bool      wide_well = norack || (rev && (rvp.muzzle || rvp.door));   // reloadwidewell
    const bool      ak_well   = bt && bt->kind == kMagFed && std::strncmp(bt->name, "AK", 2) == 0;   // reloadwellzone
    const float     eng_lat   = wide_well ? kWideEngageLat : ak_well ? kAkEngageLat : kEngageLat;
    const float     eng_below = wide_well ? kWideEngageBelow : ak_well ? kAkEngageBelow : kEngageBelow;
    // (Or the hand as it would be with no turn: the game's hold of the M60's box is turned 169 deg, a hand cannot follow that: author 5 Nov, 6 tries at 3-8 cm from the tray, never taken. The drawn hand and magazine
    // are brought to the well from there.)
    const glm::vec3 up_plain = glm::length(g.axis) > 1e-4f ? glm::normalize(in.hand_rot * mirror_x(g.axis, m)) : mag_up;
    const bool      upright = glm::dot(mag_up, axis) > (wide_well ? kWideEngageCos : ak_well ? kAkEngageCos : !tube && !rev && mag_lower(in.sig) ? 0.5f : kEngageCos) || (hg_mag && glm::dot(up_plain, axis) > kEngageCos);
    const float     pouch_d = in.pouch_ok && in.hand_ok ? glm::length(in.hand_pos - in.pouch) : -1.0f;
    const float     zone    = std::clamp(g_pouch_zone.load(), 0.05f, 0.35f);
    const bool      pin     = pouch_d >= 0.0f && pouch_d < zone * (g_pouch_in ? 1.15f : 1.0f);
    if (pin && !g_pouch_in && !in.grip && !g_hand_mag) buzz(out, kHand, 0.2f, 0.03f);
    g_pouch_in = pin;

    // The other hand: the pouch, the magazine, the handle.
    if (in.pressed && in.hand_ok) {
        if (!g_hand_mag && pin) {
            out.took_press = true;
            if (tube_full || cyl_full) {
                buzz(out, kHand, 0.3f, 0.03f);
                if (g_log.load() && cyl_full) spdlog::info("[Reload] left grip in the pouch: every chamber holds a round or a case (swing the cylinder out, muzzle up to drop the cases)");
                else if (g_log.load()) spdlog::info("[Reload] left grip in the pouch: the tube is full ({} of {})", by_hand, tube_cap);
            } else if (no_spare) {
                buzz(out, kHand, 0.3f, 0.03f);
                if (g_log.load()) spdlog::info("[Reload] left grip in the pouch: no spare ammo (reserve 0): no magazine");
            } else {
                g_hand_mag = true;
                g_take_t   = now;
                buzz(out, kHand, 0.5f, 0.05f);
                if (g_log.load()) spdlog::info("[Reload] {} taken from the pouch ({:.2f} m from its centre, zone {:.2f}){}{}", tube ? "shell" : rev ? "round" : "magazine", pouch_d, zone,
                                               // reloadshellhip: where the hand was from the zone's centre, in the body's axes (cm), to place the hip's round by
                                               [&] {
                                                   const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
                                                   glm::vec3       f = in.pouch_fwd - up * glm::dot(in.pouch_fwd, up);
                                                   f                 = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec3{ 0.0f, 0.0f, -1.0f };
                                                   const glm::vec3 r = glm::cross(f, up);
                                                   const glm::vec3 d = in.hand_pos - in.pouch;
                                                   return fmt::format(" (the hand {:.0f} right, {:.0f} up, {:.0f} forward cm)", glm::dot(d, r) * 100.0f, glm::dot(d, up) * 100.0f,
                                                                      glm::dot(d, f) * 100.0f);
                                               }(),
                                               rev ? fmt::format(" (for chamber {}; the cylinder {})", cyl_target + 1, g_open ? "open" : "closed: quick right B swings it out")
                                               : tube ? fmt::format(" ({} loaded of {})", by_hand, tube_cap)
                                               : g_mag_in_gun ? " (the gun's magazine still in: quick right B drops it)" : fmt::format(", {:.0f} ms after the drop", ms_since(g_drop_t, now)));
            }
        } else if (rev && rvp.door && g_open && open_f >= 0.99f && !g_hand_mag && !g_engaged && track && glm::length(in.hand_pos - (well - axis * (2.0f * lead_m))) < kDoorLoadZone) {
            // reloadratdoor: the left grip at the back closes it.
            out.took_press = true;
            g_open         = false;
            g_open_t       = now - std::chrono::milliseconds(int((1.0f - open_f) * kSwingSecs * 1000.0f));
            sound(kSndRackFwd);
            buzz(out, kHand, 0.6f, 0.05f);
            buzz(out, kGun, 0.5f, 0.05f);
            if (g_log.load()) spdlog::info("[Reload] left grip at the back: closed ({})", state_text());
        } else if (belt && g_belt_grip.load() && g_open && open_f >= 0.99f && g_mag_in_gun && !g_hand_mag && !g_engaged && !g_racking && !g_cover_hold && track &&
                   (g_cover_push.load() ? std::min(glm::length(in.hand_pos - well), glm::length(in.hand_pos - (well + in.gun_rot[1] * kCoverAbove))) < kCoverPushZone
                                        : glm::length(in.hand_pos - well) < kCoverZone)) {
          if (g_cover_push.load()) {
            // reloadcoverpush: the cover taken; it follows the hand down (above, after open_f).
            out.took_press = true;
            g_cover_hold   = true;
            g_cover_f0     = open_f;
            g_cover_f      = open_f;
            g_cover_h0     = glm::dot(in.hand_pos - in.gun_pos, in.gun_rot[1]);
            g_cover_tick   = 10;
            buzz(out, kHand, 0.35f, 0.03f);
            if (g_log.load()) spdlog::info("[Reload] reloadcoverpush: feed cover taken ({:.2f} m from the tray): push it down {:.0f} cm to shut it", glm::length(in.hand_pos - well),
                                           kCoverPushTravel * 100.0f);
          } else {
            // reloadbeltgrip (manualreload56): the left grip closes the feed cover over the new belt; the handle is racked next.
            out.took_press = true;
            g_open         = false;
            g_open_t       = now - std::chrono::milliseconds(int((1.0f - open_f) * kSwingSecs * 1000.0f));
            g_need_rack    = true;
            g_mag_fresh    = true;
            sound(kSndRackFwd);
            buzz(out, kHand, 0.6f, 0.05f);
            buzz(out, kGun, 0.5f, 0.05f);
            if (g_log.load()) spdlog::info("[Reload] left grip on the cover ({:.2f} m from the tray): the feed cover closed (reloadbeltgrip): rack the charging handle", glm::length(in.hand_pos - well));
          }
        } else if (!rev && !lever && !norack && !g_hand_mag && track &&
                   (glm::length(in.hand_pos - (g_act_open ? handle + back * rack_len : handle)) < (g_need_rack || g_bolt_owed || g_act_open ? zone_rack : zone_idle) ||
                    (boltt && g_bolt_open && glm::length(in.hand_pos - (handle + back * (g_rack * rack_len))) < zone_rack) ||   // reloadboltturn: where it was left
                    ((g_need_rack || g_bolt_owed) && rack_line_d < (ms16_h ? kMs16LineZone : kRackLineZone)) ||   // reloadrackzone: anywhere on the glowing part
                    (tube && g_pump_grip.load() && rack_line_d < kPumpGripZone) ||   // reloadpumpgrip: a pump anywhere along it, whatever the chamber holds
                    // reloadhandgame: or where the controller is with the drawn hand on the slide as the game has it (well behind the slide: the fingers reach forward)
                    (hg_on && hg.grab && (g_need_rack || g_bolt_owed) && glm::length(in.hand_pos - (in.gun_pos + in.gun_rot * hg.grab_c)) < kHandleZone))) {
            // Always taken (author 2 Nov 12:32): it moves with the pull; it chambers a round only after a fresh magazine once the clip ran dry.
            // A pump shotgun's pump is where the support hand holds the gun: the press stays the two-handed hold's too.
            out.took_press = !tube;
            const float rb0 = boltt && g_bolt_open ? g_rack : 0.0f;   // reloadboltturn: left turned up or back
            const bool  rd0 = boltt && g_bolt_open && g_rack_done;
            g_racking      = true;
            g_rack_done    = false;
            g_rack         = 0.0f;
            g_rack_cm      = 0;
            g_rack_r0      = glm::transpose(in.gun_rot) * (in.hand_pos - in.gun_pos);
            if (g_act_open) {   // the pump left back: taken there, to be pushed forward
                g_act_open  = false;
                g_rack      = 1.0f;
                g_rack_done = true;
                g_rack_r0   = glm::transpose(in.gun_rot) * (in.hand_pos - back * rack_len - in.gun_pos);
            }
            if (tube) {   // reloadpumpgrip: the stroke from here (left back: already a full stroke)
                g_pump_d0  = glm::length(in.hand_pos - in.gun_pos) + g_rack * rack_len;
                g_pump_d_t = now;
            }
            if (boltt) {
                // reloadboltturn: taken again where it was left, it goes on from there (the grab point put where the hand would have taken it locked).
                g_rack      = rb0;
                g_rack_done = rd0;
                g_rack_r0   = glm::transpose(in.gun_rot) * (in.hand_pos - back * (rb0 * rack_len) - bup * (g_bolt_up * std::max(0.01f, g_bolt_lift.load())) - in.gun_pos);
                g_bolt_open = false;
            }
            buzz(out, kHand, 0.35f, 0.03f);
            if (g_log.load() && tube)
                spdlog::info("[Reload] pump taken ({:.2f} m from its centre, {} from its parts' line; {} shell(s) in the tube, the chamber {})", glm::length(in.hand_pos - handle),
                             rack_line_d < 1e8f ? fmt::format("{:.2f} m", rack_line_d) : std::string{ "-" }, g_tube,
                             g_chamber == 1 ? "loaded" : g_chamber == 2 ? "holding a fired shell" : "empty");
            else if (g_log.load() && boltt)
                spdlog::info("[Reload] bolt taken ({:.2f} m from it; up {:.0f} %, back {:.0f} %; {}): up, back to its stop, forward, down", glm::length(in.hand_pos - handle),
                             g_bolt_up * 100.0f, g_rack * 100.0f, g_bolt_owed ? "the fired case in" : g_need_rack ? "the chamber empty" : "a round in");
            else if (g_log.load())
                spdlog::info("[Reload] charging handle taken ({:.2f} m from it{}){}", glm::length(in.hand_pos - handle),
                             rack_line_d < 1e8f ? fmt::format(", {:.2f} m from its parts' line", rack_line_d) : std::string{},
                             !g_need_rack ? " (nothing to chamber: the pull only moves it)" : !g_mag_in_gun ? " (no magazine in: the pull only moves it)"
                                          : !g_mag_fresh ? " (the empty magazine still in: drop it first)" : "");
        } else if (tube && !lever && !g_hand_mag && !g_racking && track && g_log.load()) {
            // reloadpumpgrip: a press with a pump gun that took nothing of the reload's (the two-handed hold, a zone): where it was.
            spdlog::info("[Reload] left grip pressed {:.2f} m from the pump's centre, {} from its parts' line (zones {:.2f} / {:.2f}{}): not the pump",
                         glm::length(in.hand_pos - handle), rack_line_d < 1e8f ? fmt::format("{:.2f} m", rack_line_d) : std::string{ "-" }, kPumpZone, kPumpGripZone,
                         g_pump_grip.load() ? "" : ", reloadpumpgrip off");
        }
    }
    // reloadpumpgrip: the fore-end already held (the two-handed hold kept from before the shot): the pump is the hand's without a new press
    // once the chamber needs it (a fired shell in it, or empty with shells in the tube).
    if (tube && !lever && g_pump_grip.load() && in.grip && !in.pressed && in.hand_ok && track && !g_racking && !g_hand_mag && !g_engaged && !g_seat_hold && !g_act_open &&
        g_chamber != 1 && (g_chamber == 2 || g_tube > 0) && (rack_line_d < kPumpGripZone || glm::length(in.hand_pos - handle) < kPumpZone)) {
        g_racking   = true;
        g_rack_done = false;
        g_rack      = 0.0f;
        g_rack_cm   = 0;
        g_rack_r0   = glm::transpose(in.gun_rot) * (in.hand_pos - in.gun_pos);
        g_pump_d0   = glm::length(in.hand_pos - in.gun_pos);
        g_pump_d_t  = now;
        buzz(out, kHand, 0.35f, 0.03f);
        if (g_log.load())
            spdlog::info("[Reload] reloadpumpgrip: pump taken with the grip already held ({:.2f} m from its parts' line; {} shell(s) in the tube, the chamber {})",
                         rack_line_d < 1e8f ? rack_line_d : -1.0f, g_tube, g_chamber == 2 ? "holding a fired shell" : "empty");
    }
    if (g_hand_mag && !in.grip && !(g_rat_on && g_rat_auto && g_engaged)) {   // (reloadratslide: sliding home by itself, it goes on)
        if (g_log.load()) spdlog::info("[Reload] left grip let go with the magazine {} ({:.2f} m from the well, {:.2f} off its axis){}", g_engaged ? "partly in" : "in the hand",
                                       glm::length(d), lat, pin ? ": back in the pouch" : ": dropped");
        g_hand_mag = false;
        g_engaged  = false;
        g_insert   = 0.0f;
    }
    // A pump shotgun's pump going forward after a full pull: the next shell from the tube into the chamber.
    auto pump_forward = [&]() {
        g_rack_done = false;
        sound(kSndRackFwd);
        buzz(out, kHand, 0.6f, 0.05f);
        buzz(out, kGun, 0.6f, 0.05f);
        if (g_tube > 0) {
            g_tube--;
            g_chamber = 1;
            if (g_log.load()) spdlog::info("[Reload] click: pump forward, a shell chambered ({} left in the tube)", g_tube);
        } else if (g_log.load()) {
            spdlog::info("[Reload] pump forward: the tube is empty, nothing chambered (load shells from the left hip)");
        }
    };
    // reloadleverhand: the lever by the hand.
    if (lever && g_lever_hand_on.load()) {
        const glm::vec3 fore  = glm::transpose(in.gun_rot) * (in.hand_pos - in.gun_pos);   // the left hand in the gun's axes: on the fore-end?
        const bool      two   = track && in.grip && !g_hand_mag && !g_engaged && !g_racking && !g_seat_hold && fore.z < -0.08f && fore.z > -0.75f && std::abs(fore.x) < 0.20f && std::abs(fore.y) < 0.25f;
        const glm::vec3 rel   = glm::transpose(in.hand_rot) * (in.gun_pos - in.hand_pos);
        const float     dt    = std::clamp(ms_since(g_lever_seen_t, now) / 1000.0f, 0.0f, 0.1f);
        g_lever_seen_t        = now;
        if (!two) {
            if (g_lever_hand) {
                g_lever_hand = false;
                if (g_lever_down) {   // let go with the lever down: it closes by itself
                    g_lever_cyc = true;
                    g_lever_fwd = false;
                    g_lever_t   = now - std::chrono::milliseconds(int(kLeverSecs * 500.0f));
                }
                g_lever_down = false;
            }
            g_lever_base_ok = false;
        } else if (!g_lever_hand) {
            g_lever_base    = g_lever_base_ok ? g_lever_base + (rel - g_lever_base) * std::min(dt / 0.5f, 1.0f) : rel;
            g_lever_base_ok = true;
            g_lever_dir     = glm::transpose(in.hand_rot) * -in.gun_rot[1];
            // (reloadleverflick, author 5 Nov 22:50:
            // the hand's stroke works the lever only when the chamber needs it, and comes back up by itself; it no longer opens the action, nor throws a live round out. The 3 cm and 7 cm stroke of before.)
            if (!g_act_open && g_chamber != 1 && !g_lever_cyc && !g_btn_reload && glm::dot(rel - g_lever_base, g_lever_dir) > 0.03f) {
                g_lever_hand   = true;
                g_lever_down   = false;
                g_lever_hand_t = now;
                if (g_log.load()) spdlog::info("[Reload] reloadleverhand: the right hand takes the lever (chamber {}; {} in the tube)", g_chamber, g_tube);
            }
        } else {
            g_lever_f = std::clamp(glm::dot(rel - g_lever_base, g_lever_dir) / kLeverStroke, 0.0f, 1.0f);
            if (!g_lever_down && g_lever_f >= 0.7f) {
                g_lever_down = true;
                sound(kSndRackBack);
                buzz(out, kGun, 0.7f, 0.05f);
                if (g_chamber != 0) {
                    g_ejects++;
                    sound(kSndEject, 0.03f);
                }
                if (g_log.load()) spdlog::info("[Reload] reloadleverhand: the lever down ({}; {} in the tube)", g_chamber == 2 ? "the fired shell thrown out" : g_chamber == 1 ? "the live shell thrown out" : "the chamber was empty", g_tube);
                g_chamber = 0;
            }
            if (g_lever_down && g_lever_f <= 0.35f) {
                g_lever_down = false;
                pump_forward();
                buzz(out, kGun, 0.7f, 0.05f);
                g_lever_hand_t = now;
            }
            // Over when it is home; or if the hands were only moved apart, nothing thrown, after a while.
            if (g_lever_f <= 0.08f || (!g_lever_down && ms_since(g_lever_hand_t, now) > 1500.0f)) {
                g_lever_hand    = false;
                g_lever_base_ok = false;
            }
        }
    }
    // reloadflick: a flick of the gun closes what B opened.
    {
        static std::deque<std::pair<clock::time_point, glm::mat3>> hist;   // the gun's turn over the last kFlickWindowMs
        static clock::time_point flick_t{};
        float                    pitch_s = 0.0f, side_s = 0.0f;   // degrees turned in that window
        if (in.gun_ok) {
            while (!hist.empty() && ms_since(hist.front().first, now) > kFlickWindowMs) hist.pop_front();
            if (!hist.empty()) {
                const glm::mat3 R  = glm::transpose(hist.front().second) * in.gun_rot;   // the turn over the window, in the gun's axes (x right, y up, z back)
                const float     c  = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
                const float     a  = std::acos(c);
                glm::vec3       ax{ R[1][2] - R[2][1], R[2][0] - R[0][2], R[0][1] - R[1][0] };
                if (glm::length(ax) > 1e-6f && a > 1e-4f) {
                    ax              = glm::normalize(ax);
                    const float deg = glm::degrees(a);
                    pitch_s         = std::abs(ax.x) * deg;
                    side_s          = std::sqrt(ax.y * ax.y + ax.z * ax.z) * deg;
                }
            }
            hist.emplace_back(now, in.gun_rot);
        } else {
            hist.clear();
        }
        if (g_flick.load() && ms_since(flick_t, now) > 400.0f && !g_hand_mag && !g_engaged) {
            if (tube && g_act_open && pitch_s > kFlickDeg && ms_since(g_act_open_t, now) > 300.0f) {
                flick_t         = now;
                hist.clear();
                g_act_close_req = true;
                if (g_log.load()) spdlog::info("[Reload] reloadflick: flicked ({:.0f} deg in 0.2 s about its right axis): closing", pitch_s);
            } else if (rev && !noopen && !rvp.door && g_open && open_f >= 0.99f && (brk ? pitch_s : side_s) > kFlickDeg) {   // (author 6 Nov: its back closes by B or the left grip only)
                flick_t  = now;
                hist.clear();
                g_open   = false;
                g_open_t = now;
                sound(kSndRackFwd);
                buzz(out, kGun, 0.4f, 0.04f);
                if (g_log.load()) spdlog::info("[Reload] reloadflick: flicked ({:.0f} deg in 0.2 s {}): the {} ({})", brk || rvp.door ? pitch_s : side_s, brk || rvp.door ? "about its right axis" : "sideways",
                                               rvp.door ? "back closed" : brk ? "action closed" : "cylinder swung back in", state_text());
            }
        }
    }
    // reloadactionopen: closed again: the next one chambered.
    if (tube && g_act_open) {
        if (lever) {
            const glm::vec3 fore = glm::transpose(in.gun_rot) * (in.hand_pos - in.gun_pos);
            const bool      two  = track && in.grip && !g_hand_mag && !g_engaged && !g_seat_hold && fore.z < -0.08f && fore.z > -0.75f && std::abs(fore.x) < 0.20f && std::abs(fore.y) < 0.25f;
            const glm::vec3 rel  = glm::transpose(in.hand_rot) * (in.gun_pos - in.hand_pos);
            const glm::vec3 dn   = glm::transpose(in.hand_rot) * -in.gun_rot[1];
            if (!two) {
                g_act_base_ok = false;
            } else if (!g_act_base_ok) {
                g_act_base    = rel;
                g_act_base_ok = true;
                if (in.pressed && ms_since(g_act_open_t, now) > kActCloseWaitMs) g_act_close_req = true;   // a new left grip on the fore-end
            } else {
                // lower yet: the right hand still going down; and whatever it does just after the opening
                if (glm::dot(rel - g_act_base, dn) > 0.0f || ms_since(g_act_open_t, now) < kActCloseWaitMs) g_act_base = rel;
                if (glm::dot(rel - g_act_base, dn) < -kActCloseUp) g_act_close_req = true;
            }
        }
        if (g_act_close_req) {
            g_act_close_req = false;
            g_act_open      = false;
            g_lever_base_ok = false;
            if (g_log.load()) spdlog::info("[Reload] reloadactionopen: closed ({} in the tube)", g_tube);
            if (lever) {   // the second half of its cycle: it comes up, the next chambered half way (below)
                g_lever_cyc = true;
                g_lever_fwd = false;
                g_lever_t   = now - std::chrono::milliseconds(int(kLeverSecs * 500.0f));
            } else {
                pump_forward();
            }
        }
    } else {
        g_act_close_req = false;
    }
    // reloadactionopen: opened by a quick B: stays open, the chamber's shell out.
    if (tube && g_act_open_req) {
        g_act_open_req = false;
        if (g_act_open || g_engaged || g_racking || g_lever_cyc || g_lever_hand) {
            if (g_log.load()) spdlog::info("[Reload] reloadactionopen: quick right B: not now (a shell going in, or the action moving)");
        } else {
            g_act_open      = true;
            g_act_open_t    = now;
            g_act_base_ok   = false;
            g_lever_base_ok = false;
            g_rack_done     = false;
            sound(kSndRackBack);
            buzz(out, kGun, 0.7f, 0.05f);
            if (g_chamber != 0) {
                g_ejects++;
                sound(kSndEject, 0.03f);
            }
            if (g_log.load()) spdlog::info("[Reload] reloadactionopen: quick right B: open ({}; {} in the tube)", g_chamber == 2 ? "the fired shell thrown out" : g_chamber == 1 ? "the live shell thrown out" : "the chamber was empty", g_tube);
            g_chamber = 0;
        }
    } else {
        g_act_open_req = false;
    }
    // A lever action's lever (manualreload30): thrown down by quick right B, the chamber's shell out at once; the next chambered on the way up.
    if (lever && g_lever_req && g_act_open) {
        g_lever_req     = false;
        g_act_open      = false;
        g_lever_base_ok = false;
        if (g_log.load()) spdlog::info("[Reload] reloadactionopen: quick right B: closed ({} in the tube)", g_tube);
        pump_forward();
    }
    if (lever && g_lever_req) {
        g_lever_req = false;
        if (g_lever_cyc) {
            if (g_log.load()) spdlog::info("[Reload] quick right B: the lever is already moving");
        } else {
            g_lever_cyc = true;
            g_lever_fwd = false;
            g_lever_t   = now;
            sound(kSndRackBack);
            buzz(out, kGun, 0.7f, 0.05f);
            if (g_chamber != 0) {
                g_ejects++;
                sound(kSndEject, 0.03f);
            }
            if (g_log.load()) spdlog::info("[Reload] quick right B: the lever thrown down ({}; {} in the tube)", g_chamber == 2 ? "the fired shell thrown out" : g_chamber == 1 ? "the live shell thrown out"
                                                                                                               : "the chamber was empty", g_tube);
            g_chamber = 0;
        }
    }
    if (g_lever_cyc) {
        const float t = ms_since(g_lever_t, now) / (kLeverSecs * 1000.0f);
        if (t >= 0.5f && !g_lever_fwd) {
            g_lever_fwd = true;
            pump_forward();
        }
        if (t >= 1.0f) g_lever_cyc = false;
    }
    if (g_racking && !in.grip) {
        if (tube) {
            // reloadactionopen: let go with it back: it stays open.
            if (g_rack_done && g_act_on.load() && g_rack >= 0.6f) {
                g_rack_done = false;
                g_act_open  = true;
                if (g_log.load()) spdlog::info("[Reload] reloadactionopen: the pump left back: open, shells go in ({} in the tube)", g_tube);
            } else if (g_rack_done) {
                pump_forward();
            }
        } else if (boltt) {
            // reloadboltturn: it stays where it was let go; locked, it is closed.
            g_bolt_open = g_bolt_up > kBoltLockAt || g_rack > kBoltHome;
            if (!g_bolt_open) g_bolt_up = 0.0f;
            if (g_log.load()) spdlog::info("[Reload] bolt let go {}", g_bolt_open ? fmt::format("open (up {:.0f} %, back {:.0f} %): it stays there, no shot until it is locked down", g_bolt_up * 100.0f, g_rack * 100.0f)
                                                                           : std::string{ "locked" });
        } else {
            if (g_log.load() && !g_rack_done) spdlog::info("[Reload] charging handle let go at {:.0f} % ({:.1f} cm of {:.1f}): not racked", g_rack * 100.0f, g_rack * rack_len * 100.0f, rack_len * 100.0f);
            if (g_rack_done) sound(kSndRackFwd);   // the bolt goes home
        }
        g_racking = false;
        if (!(boltt && g_bolt_open)) g_rack = 0.0f;
    }

    // A revolver's open cylinder, the muzzle tipped up (the gun's forward more than 37 deg above level): the fired cases slide out. A break
    // action's ejector throws its fired shells out as it opens (manualreload29).
    if (rev && g_open && open_f >= 0.99f && (brk || g_rev_dump.load() || (in.gun_ok && glm::dot(-back, glm::vec3{ 0.0f, 1.0f, 0.0f }) > 0.6f))) {
        uint32_t mask = 0;
        for (int i = 0; i < g_cyl_n; ++i)
            if (g_cyl[i] == 2) {
                g_cyl[i] = 0;
                mask |= 1u << i;
            }
        if (mask) {
            g_dump_mask = mask;
            g_dumps++;
            sound(kSndEject);
            buzz(out, kGun, 0.3f, 0.05f);
            if (g_log.load()) spdlog::info("[Reload] {} (chambers mask {:#x}; {} live left)", brk ? "opened: the ejector threw the fired shells out"
                                                                         : g_rev_dump.load() ? "opened: the fired cases dropped out of the cylinder" : "muzzle up: the fired cases dropped out of the cylinder",
                                           mask, cyl_live);
        }
    }
    const int rev_target = [&] {
        for (int i = 0; i < g_cyl_n; ++i) if (g_cyl[i] == 0) return i;
        return -1;
    }();

    // Into the well (a pump shotgun: a shell into the loading port, the gun loaded or not; a revolver: a round into its open cylinder).
    if (g_hand_mag && (!g_mag_in_gun || tube || (rev && g_open && open_f >= 0.99f && rev_target >= 0)) && (!belt || (g_open && open_f >= 0.99f)) && track) {
        const bool rslide = rev && g_round_slide.load();
        const bool act_load = (tube && g_act_open && glm::length(d) < kActLoadZone) ||   // reloadactionopen
                              (belt && std::min({ glm::length(d), glm::length(in.hand_pos - well), glm::length(in.hand_pos + in.gun_rot * hoff - well) }) < kBeltLoadZone) ||   // reloadbeltzone (the cover open: above; run final_e, the M249: its fist 14 cm behind the box's top, the hand turned: neither of the first two came near. The third: the hand's place only)
                              (rev && rvp.door && !rat_slide && std::min(glm::length(d), glm::length(in.hand_pos - (well - axis * (2.0f * lead_m)))) < kDoorLoadZone);   // reloadratdoor (open: above)
        if (!g_engaged) {
            g_rat_on   = false;
            g_rat_auto = false;
        }
        // reloadratslide: the back opening, where the seated missile's tail is (its way in starts there).
        const glm::vec3 rat_entry = well - axis * travel;
        if (!g_engaged) {
            const bool rat_take = rat_slide && std::min(glm::length(top - rat_entry), glm::length(in.hand_pos - rat_entry)) < kRatEntryZone && glm::dot(mag_up, axis) > kRatEngageCos;
            const bool take = rat_slide ? rat_take
                            : act_load ? true
                                       : rslide ? lat < (wide_well ? eng_lat : kRoundEngageLat) && along > -(travel + eng_below) && along < 0.01f && glm::dot(mag_up, axis) > (wide_well ? kWideEngageCos : kRoundEngageCos)
                                     : lat < eng_lat && along > -(travel + eng_below) && along < (wide_well || ak_well ? 0.01f : -travel * (1.0f - kEngageTop)) && upright;
            if (take) {
                g_engaged   = true;
                g_engage_t  = now;
                if (rat_slide) {
                    g_rat_on   = true;
                    g_rat_auto = false;
                    g_rat_h0   = in.hand_pos;
                    g_rat_i0   = std::clamp((along + travel) / travel, 0.0f, 0.5f);   // the nose's way in already (at most half)
                    if (g_log.load()) spdlog::info("[Reload] reloadratslide: missile at the back ({:.1f} cm from the opening, the hand {:.1f} cm): push it in {:.0f} cm and it slides home",
                                                   glm::length(top - rat_entry) * 100.0f, glm::length(in.hand_pos - rat_entry) * 100.0f, kRatPush * 100.0f);
                }
                g_insert_cm = int(std::floor(std::clamp((along + travel) / travel, 0.0f, 1.0f) * travel * 100.0f));
                buzz(out, kHand, 0.3f, 0.025f);
                if (g_log.load()) spdlog::info("[Reload] {} ({:.1f} cm below its seat, {:.1f} cm off the axis, {:.0f} deg from it)",
                                               tube ? "shell in the loading port" : rev ? "round in the cylinder" : "magazine in the well",
                                               -along * 100.0f, lat * 100.0f,
                                               glm::degrees(std::acos(std::clamp(glm::dot(mag_up, axis), -1.0f, 1.0f))));
            }
        } else if (g_rat_on) {
            // reloadratslide: out again when pulled back or off the tube's line (not once it slides home by itself).
            const glm::vec3 dh   = in.hand_pos - g_rat_h0;
            const float     push = glm::dot(dh, axis);
            const float     off  = glm::length(dh - axis * push);
            if (!g_rat_auto && (push < -kRatPullOut || off > kRatLeaveLat)) {
                g_engaged = false;
                g_rat_on  = false;
                g_insert  = 0.0f;
                if (g_log.load()) spdlog::info("[Reload] reloadratslide: missile pulled out of the back ({:.1f} cm back, {:.1f} cm off the line)", -push * 100.0f, off * 100.0f);
            }
        } else if (!act_load && (lat > (wide_well ? kWideLeaveLat : ak_well ? kAkLeaveLat : kLeaveLat) || along < -(travel + eng_below + 0.02f))) {
            g_engaged = false;
            g_insert  = 0.0f;
            if (g_log.load()) spdlog::info("[Reload] magazine pulled out of the well ({:.1f} cm off the axis, {:.1f} cm below the seat)", lat * 100.0f, -along * 100.0f);
        }
        if (g_engaged) {
            // Below the start of the travel the magazine is drawn where the hand holds it, on the well's axis (negative: no jump up to the
            // start of the travel when it engages; 2 Nov 14:42).
            g_insert     = std::clamp((along + travel) / travel, -(eng_below + 0.02f) / travel, 1.0f);
            if (rslide) g_insert = std::max(g_insert, std::clamp(ms_since(g_engage_t, now) / kRoundSlideMs, 0.0f, 1.0f));   // reloadroundslide: it drops home
            if (act_load) g_insert = 1.0f;
            if (g_rat_on) {
                // reloadratslide: it goes in with the hand (along the tube's axis), and kRatPush in it slides home by itself.
                const float push = glm::dot(in.hand_pos - g_rat_h0, axis);
                const float ins  = std::clamp(g_rat_i0 + push / travel, 0.0f, 1.0f);
                if (!g_rat_auto && push >= kRatPush) {
                    g_rat_auto    = true;
                    g_rat_auto_t  = now;
                    g_rat_auto_i0 = ins;
                    if (g_log.load()) spdlog::info("[Reload] reloadratslide: pushed {:.1f} cm in ({:.0f} % of its way): it slides home", push * 100.0f, ins * 100.0f);
                }
                g_insert = g_rat_auto ? std::max(ins, g_rat_auto_i0 + (1.0f - g_rat_auto_i0) * std::clamp(ms_since(g_rat_auto_t, now) / (kRatAutoSecs * 1000.0f), 0.0f, 1.0f)) : ins;
            }
            const int cm = int(std::floor(std::max(0.0f, g_insert) * travel * 100.0f));
            if (cm > g_insert_cm) buzz(out, kHand, 0.14f, 0.012f);   // a tick per centimetre going in
            g_insert_cm = cm;
            if (g_insert >= 0.999f && rev) {
                // The round home in its chamber; the game's clip refilled by its own reload (tapped once, hidden) if it holds fewer.
                g_hand_mag       = false;
                g_engaged        = false;
                g_insert         = 0.0f;
                g_cyl[rev_target] = 1;
                g_cyl_known      = true;
                if (led_rev && in.ammo_ok && in.reserve > 0) {
                    fc5::lua::reserve_take_request(1);   // ammoledger: the round off the reserve (the game's fills for it take nothing)
                    if (g_log.load()) spdlog::info("[Reload] ammoledger: a round loaded by hand: 1 off the reserve {}", in.reserve);
                }
                buzz(out, kHand, 0.5f, 0.04f);
                buzz(out, kGun, 0.4f, 0.04f);
                sound(kSndMagIn);
                int live = 0;
                for (const int c : g_cyl) live += c == 1;
                // (Never for more than the clip holds: 6 live wanted 7 in a clip of 6: author 5 Nov: nothing to do, the wait for it stayed, and the next rounds went in with no refill: 6 loaded, 2 shots.)
                const bool tap = in.ammo_ok && in.clip < std::min(live + (keep_tr ? 1 : 0), tube_cap) && in.reserve > 0 && !g_game_pending;
                if (tap) {
                    g_key_pending  = true;
                    g_game_pending = true;
                    g_clip_at_tap  = in.clip;
                    g_tap_t        = now;
                    g_retaps       = 2;
                    g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
                }
                if (g_log.load()) spdlog::info("[Reload] click: round loaded in chamber {} ({} live; the game's clip {}{})", rev_target + 1, live,
                                               in.ammo_ok ? std::to_string(in.clip) : std::string{ "?" }, tap ? ": its reload tapped, hidden" : "");
            } else if (g_insert >= 0.999f && tube) {
                // The shell pushed into the tube. The game's clip is refilled by its own reload (tapped once, hidden) if it holds fewer
                // than are now loaded by hand.
                g_hand_mag = false;
                g_engaged  = false;
                g_insert   = 0.0f;
                g_tube     = std::max(0, g_tube) + 1;
                buzz(out, kHand, 0.6f, 0.05f);
                buzz(out, kGun, 0.5f, 0.05f);
                sound(kSndMagIn);
                const int loaded = g_tube + (g_chamber == 1 ? 1 : 0);
                const bool tap   = in.ammo_ok && in.clip < std::min(loaded + (keep_tr ? 1 : 0), tube_cap) && in.reserve > 0 && !g_game_pending;
                if (tap) {
                    g_key_pending  = true;
                    g_game_pending = true;
                    g_clip_at_tap  = in.clip;
                    g_tap_t        = now;
                    g_retaps       = 2;   // no second tap: the game's shotgun reload goes on by itself
                    g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
                }
                if (g_log.load()) spdlog::info("[Reload] click: shell loaded ({} in the tube, the chamber {}; the game's clip {}{})", g_tube,
                                               g_chamber == 1 ? "loaded" : g_chamber == 2 ? "holding a fired shell" : "empty",
                                               in.ammo_ok ? std::to_string(in.clip) : std::string{ "?" }, tap ? ": its reload tapped, hidden" : "");
            } else if (g_insert >= 0.999f) {
                g_mag_in_gun = true;
                g_hand_mag   = false;
                g_engaged    = false;
                g_insert     = 0.0f;
                g_seat_t     = now;
                if (g_need_rack) g_mag_fresh = true;
                buzz(out, kHand, 0.8f, 0.06f);
                sound(kSndMagIn);
                buzz(out, kGun, 0.7f, 0.06f);
                if (g_log.load()) spdlog::info("[Reload] click: magazine seated {:.0f} ms after the drop ({:.0f} ms after taking it){}{}", ms_since(g_drop_t, now), ms_since(g_take_t, now),
                                               g_need_rack ? "; the handle must be racked" : "", g_game_pending ? "; the game still reloading" : "");
            }
        }
    } else {
        g_engaged = false;
    }

    // reloadboltturn: a bolt action's bolt up, back, forward and down, each following the hand from where it took the bolt (that point moves with the gun).
    if (g_racking && track && boltt) {
        const float     lift   = std::max(0.01f, g_bolt_lift.load());
        glm::vec3       r0     = in.gun_pos + in.gun_rot * g_rack_r0;
        auto            rebase = [&](const glm::vec3& v) { g_rack_r0 += glm::transpose(in.gun_rot) * v; r0 += v; };
        const float     up_was = g_bolt_up;
        const bool      back_was = g_rack > kBoltHome;
        if (back_was) {
            g_bolt_up = 1.0f;   // back: it cannot turn
            g_rack    = std::clamp(glm::dot(in.hand_pos - r0, back) / rack_len, 0.0f, 1.0f);
        } else {
            g_bolt_up = std::clamp(glm::dot(in.hand_pos - r0, bup) / lift, 0.0f, 1.0f);   // home: it turns
            if (up_was < kBoltUpAt && g_bolt_up >= kBoltUpAt) {
                // Fully up: the pull starts from here (the hand's drift back while it turned not counted).
                if (const float bd = glm::dot(in.hand_pos - r0, back); bd > 0.0f) rebase(back * bd);
                buzz(out, kHand, 0.4f, 0.03f);
                if (g_log.load()) spdlog::info("[Reload] bolt turned up ({:.0f} deg): pull it back", g_bolt_deg.load());
            }
            g_rack = g_bolt_up >= kBoltUpAt ? std::clamp(glm::dot(in.hand_pos - r0, back) / rack_len, 0.0f, 1.0f) : 0.0f;   // pulled only once fully up
            if (g_rack > kBoltHome) g_bolt_up = 1.0f;
        }
        if (back_was && g_rack <= kBoltHome) {
            // Home again: the turn down starts from the hand's height now (its drift while it pulled not counted).
            rebase(bup * (glm::dot(in.hand_pos - r0, bup) - lift));
            g_bolt_up = 1.0f;
        }
        const int cm = int(std::floor(g_rack * rack_len * 100.0f));
        if (cm != g_rack_cm) buzz(out, kHand, 0.14f, 0.012f);
        g_rack_cm = cm;
        if (g_rack >= 0.999f && !g_rack_done) {
            // To its stop: whatever the chamber held leaves by the ejection port (the fired case, or a live round: one less in the magazine, reloadrackloss).
            g_rack_done = true;
            buzz(out, kHand, 0.75f, 0.05f);
            buzz(out, kGun, 0.75f, 0.05f);
            sound(kSndRackBack);
            const bool fired_case = g_bolt_owed && !g_bolt_pulled;
            const bool chamber_in = !g_bolt_pulled || g_bolt_fed;
            if (chamber_in && (fired_case || (!g_need_rack && g_mag_in_gun && !(in.ammo_ok && in.clip <= (keep_one() && keep_res(in.reserve) ? 1 : 0))))) {
                g_ejects++;
                sound(kSndEject);
                const bool lost = g_rack_loss.load() && in.ammo_ok && !fired_case;
                if (lost && led && led_mag) led->surplus++;   // ammoledger: the thrown round is surplus
                else if (lost) g_mag_loss[in.sig].lost++;
                if (g_log.load()) spdlog::info("[Reload] bolt back: {} ejected ({}; the game's clip {}, the mod's {})", fired_case ? "the fired case" : "a live round", lost ? "one less in the magazine" : "nothing lost",
                                               in.ammo_ok ? std::to_string(g_mag_loss[in.sig].real) : std::string{ "?" }, in.ammo_ok ? std::to_string(in.clip - (lost ? 1 : 0)) : std::string{ "?" });
            } else if (g_log.load()) {
                spdlog::info("[Reload] bolt back: the chamber was empty");
            }
            g_bolt_pulled = true;
            g_bolt_fed    = false;
        }
        if (g_rack_done && g_rack <= kBoltHome) {
            // Pushed home: the next round from the magazine into the chamber (a seated magazine with rounds).
            g_rack_done = false;
            g_bolt_fed  = g_mag_in_gun && (!g_need_rack || g_mag_fresh);
            buzz(out, kHand, 0.5f, 0.04f);
            if (g_log.load()) spdlog::info("[Reload] bolt pushed home ({}): turn it down to lock it", g_bolt_fed ? "a round fed" : g_mag_in_gun ? "the magazine empty: nothing fed" : "no magazine: nothing fed");
        }
        if (up_was > kBoltLockAt && g_bolt_up <= kBoltLockAt && g_rack <= kBoltHome) {
            // Turned down: locked.
            g_bolt_up = 0.0f;
            sound(kSndRackFwd);
            buzz(out, kHand, 0.6f, 0.05f);
            buzz(out, kGun, 0.6f, 0.05f);
            const char* what = "turned down again without being pulled back: nothing changed";
            if (g_bolt_pulled && g_bolt_fed) {
                if (g_need_rack && g_mag_fresh && g_mag_in_gun) {
                    g_need_rack = false;
                    g_mag_fresh = false;
                    what        = "a round chambered from the fresh magazine";
                } else {
                    what = g_bolt_owed ? "the fired case out, a round chambered" : "a round chambered";
                }
                g_bolt_owed = false;
            } else if (g_bolt_pulled) {
                what = "the chamber empty (nothing was fed)";
            }
            if (g_log.load()) spdlog::info("[Reload] click: bolt locked down: {}", what);
            g_bolt_pulled = g_bolt_fed = false;
        }
    } else
    // The charging handle: pulled back along the gun from where it was taken (that point moves with the gun).
    if (g_racking && track) {
        const glm::vec3 r0 = in.gun_pos + in.gun_rot * g_rack_r0;
        if (tube && !lever && g_pump_grip.load()) {
            // reloadpumpgrip: the pump's stroke is the left hand coming back towards the weapon hand. With the pump forward the start
            // follows the hand out at once and in slowly (0.8 s), so arms easing while aiming do not count as a stroke (a stroke takes 0.2-0.4 s).
            const float d  = glm::length(in.hand_pos - in.gun_pos);
            const float dt = std::clamp(ms_since(g_pump_d_t, now) / 1000.0f, 0.0f, 0.1f);
            g_pump_d_t     = now;
            if (d > g_pump_d0) g_pump_d0 = d;
            else if (!g_rack_done && g_pump_d0 - d < 0.4f * rack_len) g_pump_d0 += (d - g_pump_d0) * std::min(1.0f, dt / 0.8f);
            g_rack = std::clamp((g_pump_d0 - d) / rack_len, 0.0f, 1.0f);
        } else {
            g_rack = std::clamp(glm::dot(in.hand_pos - r0, back) / rack_len, 0.0f, 1.0f);
        }
        const int cm       = int(std::floor(g_rack * rack_len * 100.0f));
        if (cm > g_rack_cm && !g_rack_done) buzz(out, kHand, 0.14f, 0.012f);
        g_rack_cm = cm;
        if (tube && g_rack >= 0.999f && !g_rack_done) {
            // The pump to its stop: the shell in the chamber (fired or not) leaves by the ejection port.
            g_rack_done = true;
            buzz(out, kHand, 0.75f, 0.05f);
            buzz(out, kGun, 0.75f, 0.05f);
            sound(kSndRackBack);
            if (g_chamber != 0) {
                g_ejects++;
                sound(kSndEject, 0.03f);
                if (g_log.load()) spdlog::info("[Reload] pump back: the {} shell ejected (a round ejected by the pull; {} in the tube)", g_chamber == 2 ? "fired" : "live", g_tube);
            } else if (g_log.load()) {
                spdlog::info("[Reload] pump back: the chamber was empty ({} in the tube)", g_tube);
            }
            g_chamber = 0;
        } else if (tube && g_rack_done && g_rack < kPumpFwd) {
            pump_forward();
        } else if (!tube && g_rack_done && g_rack < 0.35f) {
            // reloadrackagain (author 5 Nov): pushed forward, the grip still held: ready for the next pull.
            g_rack_done = false;
            sound(kSndRackFwd);
            buzz(out, kHand, 0.4f, 0.03f);
        } else if (!tube && g_rack >= 0.999f && !g_rack_done) {
            g_rack_done = true;
            buzz(out, kHand, 0.75f, 0.05f);
            buzz(out, kGun, 0.75f, 0.05f);
            sound(kSndRackBack);
            // A round in the chamber (none to rack, and not an empty clip) comes out of the ejection port (author 2 Nov 13:53). Seen only: the game's clip keeps its count.
            // (No magazine in: nothing feeds the next one: until now each pull threw out another. At the round reloadkeepone keeps: the gun is dry.)
            if (!g_need_rack && g_mag_in_gun && !(in.ammo_ok && in.clip <= (keep_one() && keep_res(in.reserve) ? 1 : 0))) {
                g_ejects++;
                sound(kSndEject);
                // (author 5 Nov: g_mag_loss. A bolt's fired case is no round.)
                const bool lost = g_rack_loss.load() && in.ammo_ok && !g_bolt_owed;
                if (lost && led && led_mag) led->surplus++;   // ammoledger: the thrown round is surplus
                else if (lost) g_mag_loss[in.sig].lost++;
                if (g_log.load()) spdlog::info("[Reload] a round ejected by the pull ({}; the game's clip {}, the mod's {})", lost ? "one less in the magazine" : "nothing lost", in.ammo_ok ? std::to_string(g_mag_loss[in.sig].real) : std::string{ "?" }, in.ammo_ok ? std::to_string(in.clip - (lost ? 1 : 0)) : std::string{ "?" });
            }
            if (g_bolt_owed && !g_need_rack) {
                g_bolt_owed = false;   // allreload: the fired case out (ejected above), the next round chambered as the bolt goes home
                if (g_log.load()) spdlog::info("[Reload] click: bolt worked, the fired case out, a round chambered");
            } else if (g_need_rack && g_mag_fresh && g_mag_in_gun) {
                g_need_rack = false;
                g_mag_fresh = false;
                g_bolt_owed = false;
                if (g_log.load()) spdlog::info("[Reload] click: charging handle racked, a round chambered{}", g_game_pending ? " (the game still reloading)" : "");
            } else if (g_log.load()) {
                spdlog::info("[Reload] charging handle pulled all the way: {}", !g_need_rack ? "nothing to chamber (a round already in)"
                                                                               : !g_mag_in_gun ? "no magazine in" : "the empty magazine still in: drop it first (quick right B)");
            }
        }
    }

    // The game's reload: tapped once at the drop; tapped again if the clip has not grown 3.5 s after the tap with the magazine seated (a
    // sprint or a switch cut it short), twice at most.
    if (g_game_pending && g_mag_in_gun && in.ammo_ok && g_clip_at_tap >= 0 && ms_since(g_tap_t, now) > 3500.0f && ms_since(g_seat_t, now) > 300.0f && g_retaps < 2) {
        g_retaps++;
        g_key_pending = true;
        g_tap_t       = now;
        g_clip_at_tap = in.clip;
        g_busy_until  = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
        if (g_log.load()) spdlog::info("[Reload] the game's clip still {} 3.5 s after its reload was tapped: tapped again ({})", in.clip, g_retaps);
    }
    // reloadnogame: a fill asked for and the game's calls failed, or the reserve is too low for them: its reload after all, so the gun is never left stuck.
    static bool filling = false;
    const bool fill_short = filling && fc5::lua::clip_fill_short();
    if (fill_short && in.ammo_ok && g_clip_max.count(in.sig) && in.clip >= g_clip_max[in.sig]) g_game_pending = false;   // the clip full: nothing for the game's reload to do either
    if (filling && (!g_game_pending || fill_short || fc5::lua::clip_fill_failed())) {
        if (g_game_pending) {
            g_tap_real     = true;
            g_key_pending  = true;
            g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
        }
        filling = false;
    }
    if (g_key_pending && !g_tap_real && !fc5::lua::clip_fill_failed() && g_empty_sig.load() != in.sig) {
        // reloadnogame: no reload of the game's behind the manual one.
        g_key_pending = false;
        g_busy_until  = now;
        filling       = true;
        // ammoledger: a magazine gun's load (the surplus it replaces taken by the ledger once it lands); a revolver's or break action's fill takes
        // nothing (each round loaded by hand took its own).
        if (led && led_mag) { led->topup = false; led->absorb = true; }
        fc5::lua::clip_fill_request(led_mag ? 1 : led_rev ? 0 : -1);
    } else if (g_key_pending) {
        g_key_pending = false;
        g_tap_real    = false;
        if (g_key.load() == 1) g_pad_until = now + std::chrono::milliseconds(120);
        else out.key_scan = 0x13;   // R, the game's reload on the keyboard
    }
    if (now < g_pad_until) out.buttons |= 0x4000;   // XINPUT_GAMEPAD_X

    // A pump shotgun: the support hand on the pump keeps the two-handed hold, and the gun fires with a live shell in the chamber and the pump
    // home (the game's own reload, filling its clip in the background, does not hold it back: a shot ends it, as in the game).
    // reloadseathold: a grip still held once its shell, round or magazine went in stays taken until it is let go (no new press, no hold).
    if (g_hand_was && !g_hand_mag && in.grip && g_seat_hold_on.load()) {
        g_seat_hold = true;
        if (g_log.load()) spdlog::info("[Reload] reloadseathold: the left grip stays the reload's until it is let go (then a new press on the gun holds it)");
    }
    if (!in.grip || g_hand_mag || !g_seat_hold_on.load()) g_seat_hold = false;
    g_hand_was     = g_hand_mag;
    // reloadnostuck (allreloadfire): never left unable to fire while the game's clip has rounds.
    {
        // The rounds the mod may hold: the game's clip, one fewer while reloadkeepone keeps one more there (manualreload45).
        const int  avail    = keep_tr && keep_res(in.reserve) ? in.clip - 1 : in.clip;
        const bool tube_dry = tube && g_chamber != 1 && g_tube <= 0;   // nothing chambered, nothing to pump or lever in
        const bool cyl_dry  = rev && g_cyl_known && cyl_live == 0 && (noopen || (!g_open && open_f <= 0.01f));   // closed and no live round
        const bool owed_dry = !tube && !rev && g_mag_in_gun && ((g_need_rack && g_mag_fresh) || g_bolt_owed);   // a fresh magazine, the handle or bolt not worked
        // The game's reload tapped and its clip never grown (tapped again twice by then): not waited for after kPendingMaxSecs.
        if (g_no_stuck.load() && g_game_pending && g_mag_in_gun && in.ammo_ok && in.clip > 0 && ms_since(g_tap_t, now) > kPendingMaxSecs * 1000.0f) {
            g_game_pending = false;
            if (g_log.load()) spdlog::info("[Reload] reloadnostuck: the game's reload tapped {:.0f} s ago never grew its clip ({}): not waited for any more", ms_since(g_tap_t, now) / 1000.0f, in.clip);
        }
        const bool doing    = g_hand_mag || g_engaged || g_racking || g_bolt_open || g_seat_hold || g_lever_cyc || g_game_pending || now < g_busy_until;
        // (noautoswitch: the kept round with no reserve: nothing to load it from, so not held, or the game's reload was tapped again and again.)
        const bool held     = in.ammo_ok && in.clip > 0 && !doing && (tube_dry || cyl_dry || owed_dry || keep_last) && !(avail <= 0 && in.reserve <= 0);
        const bool by_hand  = owed_dry || g.measured;   // a well, loading port or muzzle measured: a round can go in by hand
        const float wait_ms = by_hand ? kStuckSecs * 1000.0f : 300.0f;
        // (Not by itself once something can go in by hand: the left Y press reloads now: an empty revolver or shotgun left alone loaded itself after kStuckSecs, free rounds: run final_a, author 5 Nov.)
        if (!g_no_stuck.load() || !held || (by_hand && g_btn_on.load())) {
            g_stuck_t = now;
        } else if (ms_since(g_stuck_t, now) >= wait_ms) {
            const float held_s = ms_since(g_stuck_t, now) / 1000.0f;
            const char* what   = "";
            if (avail <= 0) {
                // reloadkeepone's last round (manualreload45) with nothing loaded by hand: the game's own reload, tapped and kept out of the
                // view, fills its clip; the next poll past its end loads the weapon from it.
                g_key_pending  = true;
                g_game_pending = true;
                g_clip_at_tap  = in.clip;
                g_tap_t        = now;
                g_retaps       = 0;   // tapped again if its clip has not grown 3.5 s after (below)
                g_busy_until   = now + std::chrono::milliseconds(int(std::max(kBusySecs, rev ? rvp.busy_s : 0.0f) * 1000.0f));
                what           = "the game's reload tapped (hidden) to fill its clip";
            } else if (tube) {
                g_chamber = 1;
                g_tube    = std::max(0, avail - 1);
                what      = "loaded from the game's clip, it fires";
            } else if (rev) {
                const int n = std::min(avail, g_cyl_n);
                for (int i = 0; i < n; ++i) g_cyl[i] = 1;
                g_cyl_next = 0;
                cyl_live   = n;
                cyl_target = -1;
                for (int i = 0; i < g_cyl_n; ++i)
                    if (cyl_target < 0 && g_cyl[i] == 0) cyl_target = i;
                what = "loaded from the game's clip, it fires";
            } else {
                g_need_rack = false;
                g_mag_fresh = false;
                g_bolt_owed = false;
                what        = "the handle or bolt taken as worked, it fires";
            }
            g_stuck_t = now;
            if (g_log.load())
                spdlog::info("[Reload] reloadnostuck: weapon {} held from firing {:.1f} s with {} in the game's clip, reserve {} ({}): {} ({})", in.sig, held_s, in.clip,
                             in.reserve, by_hand ? "nothing done towards its reload" : "nothing can go in by hand: its well never measured", what, state_text());
        }
    }
    out.grip_taken = g_hand_mag || g_seat_hold || (g_racking && !tube) || g_cover_hold;   // (reloadcoverpush: the cover in the hand)
    static bool keep_said = false;   // reloadkeepone (manualreload45): said once each time the last round in the game's clip holds the trigger
    if (keep_last && !keep_said && g_log.load())
        spdlog::info("[Reload] reloadkeepone: one round left in the game's clip of weapon {} (reserve {}): no shot until a {} is loaded (the game starts no reload of its own)",
                     in.sig, in.reserve, tube ? "shell" : "round");
    keep_said = keep_last;
    // reloadkeepone with a full cylinder or tube: the clip has no room for the round it keeps, so the last round by hand met "one left in the game's clip: no shot" (author 5 Nov: 6 live, 5 shots): filled again
    // (reloadnogame), so every round by hand fires.
    if (keep_tr && in.ammo_ok && in.clip <= 1 && in.reserve > 0 && !g_game_pending && !g_key_pending && !g_btn_reload && (rev ? cyl_live : by_hand) >= 1) {
        g_key_pending  = true;
        g_game_pending = true;
        g_clip_at_tap  = in.clip;
        g_tap_t        = now;
        g_retaps       = 2;
        g_busy_until   = now + std::chrono::milliseconds(int(kBusySecs * 1000.0f));
        if (g_log.load()) spdlog::info("[Reload] reloadkeepone: {} by hand and {} in the game's clip: filled again", rev ? cyl_live : by_hand, in.clip);
    }
    out.no_fire    = noopen ? (g_engaged || (g_cyl_known && cyl_live == 0))   // the RPG-7: a rocket in, not going in (allreload)
                   : rev    ? (g_open || open_f > 0.01f || g_engaged || (g_cyl_known && cyl_live == 0) || keep_last)
                   : tube   ? (g_chamber != 1 || g_engaged || (g_racking && g_rack > 0.15f) || g_lever_cyc || g_lever_hand || g_act_open || keep_last)
                            : (!g_mag_in_gun || g_need_rack || g_bolt_owed || g_game_pending || g_engaged || g_racking || (belt && (g_open || open_f > 0.01f)) ||
                               (boltt && (g_bolt_open || g_bolt_up > 0.0f || g_rack > 0.0f)) ||   // reloadboltturn: fires only locked down
                               (keep_one() && in.ammo_ok && in.clip <= 1 && keep_res(in.reserve)));   // reloadkeepone: the round kept in the clip
    if (g_led_hold.load()) out.no_fire = true;   // ammoledger: the game's clip at 1, its top-up not landed
    const bool busy = now < g_busy_until && !g_btn_reload;   // reloadbutton: the game's reload shown, not kept out of the view
    g_busy_now      = busy;
    g_pending_now   = g_game_pending;
    g_pump_now      = tube && !lever && g_racking;
    g_pump_aim      = g_pump_grip.load() && tube && !lever && g_racking && (g_rack > 0.05f || g_rack_done);
    g_mag_hand_now  = g_hand_mag || g_seat_hold;
    // reloadcasinghide: the fired shell leaves by the hand (the pump, the lever, the bolt), so the game's own casing is not shown.
    g_casing_now    = g_casing_on.load() && ((tube && !bt->semi && g_pump_shot.load()) || boltg);
    // reloadbuttonguard: the left hand at work (not the game's reload from its button; no longer a magazine merely out or a cylinder merely open, author 5 Nov: a press reloads then).
    {
        const auto cm   = g_clip_max.find(in.sig);
        g_clip_full_now = !tube && !rev && in.ammo_ok && g_mag_in_gun && !g_need_rack && cm != g_clip_max.end() && in.clip >= cm->second;
    }
    if (!g_btn_reload && (g_hand_mag || g_engaged || g_racking || g_seat_hold || g_lever_cyc || g_lever_hand))
        g_step_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    g_hide          = noopen ? (busy || g_hand_mag)
                    : rev  ? (busy || g_hand_mag || open_f > 0.0f)
                    : tube ? (busy || g_hand_mag || (g_racking && g_rack > 0.15f)) : (busy || !g_mag_in_gun || g_hand_mag || g_racking || (belt && open_f > 0.0f));

    View v{};
    v.active       = true;
    v.sig          = in.sig;
    v.pose_ok      = in.gun_ok && in.hand_ok;
    v.gun_pos      = in.gun_pos;
    v.gun_rot      = in.gun_rot;
    v.hand_pos     = in.hand_pos;
    v.hand_rot     = in.hand_rot;
    v.pouch_mag    = in.pouch_ok && !no_spare;
    v.pouch        = in.pouch;
    v.pouch_fwd    = in.pouch_fwd;
    v.pouch_r      = zone;
    v.pouch_near   = pin && !g_hand_mag;
    v.hand_mag     = g_hand_mag && !g_engaged;
    v.mag_in_gun   = g_mag_in_gun;
    v.engaged      = g_engaged;
    v.insert       = g_insert;
    v.well_ball    = g_hand_mag && (!g_mag_in_gun || tube || (rev && g_open)) && (!belt || g_open);
    v.well_near    = g_engaged || (lat < eng_lat * 1.5f && along > -(travel + eng_below) - 0.03f && along < 0.03f);
    v.rack_ball    = rev || lever || norack ? false : tube ? (!g_hand_mag && (g_chamber != 1 || g_racking)) : ((g_mag_in_gun && (g_need_rack || g_bolt_owed)) || g_racking || (boltt && g_bolt_open) || (!g_hand_mag && track && glm::length(in.hand_pos - handle) < zone_idle * 1.5f));
    v.racking      = g_racking;
    v.bolt         = boltg;   // reloadboltpin: its bolt at rest unless the hand works it
    v.bolt_up      = boltt ? g_bolt_up : 0.0f;
    v.bolt_deg     = g_bolt_deg.load();
    v.bolt_open    = boltt && (g_bolt_open || g_racking);
    v.rack         = g_rack;
    v.travel       = travel;
    v.rack_len     = rack_len;
    v.g            = g;
    v.hand_mag_off = hoff;
    v.hand_mag_turn = hg_mag && !uses_learnt(in.sig, kItHold) ? hg.mag_turn : glm::mat3{ 1.0f };
    v.hand_mag_game = hg_mag && !uses_learnt(in.sig, kItHold);
    v.hand_mag_c    = hg.mag_c;
    v.slide_lock    = tube && !lever && g_act_open ? rack_len : hg_on && g_need_rack && !tube && !rev ? hg.slide_lock : 0.0f;
    v.tube         = tube;
    if (g_lever_cyc) {
        const float t = std::clamp(ms_since(g_lever_t, now) / (kLeverSecs * 1000.0f), 0.0f, 1.0f);
        v.lever_f     = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
    } else {
        v.lever_f = g_act_open ? 1.0f : g_lever_hand ? g_lever_f : 0.0f;
    }
    v.tube_n       = g_tube;
    v.chamber      = g_chamber;
    v.revolver     = rev;
    v.belt         = belt;
    v.open_f       = open_f;
    for (int i = 0; i < 6; ++i) v.cyl[i] = g_cyl[i];
    v.target       = rev_target;
    v.hip_loaded   = g_hip_loaded.load();   // reloadhiploaded
    v.dumps        = g_dumps.load();
    v.dump_mask    = g_dump_mask.load();
    // reloadhlwhen: the highlights drawn only with the gun run dry, a reload begun, or an action needed before it fires (always: as before).
    {
        const bool magfed  = !rev && !tube;
        const bool begun   = g_hand_mag || g_engaged || (magfed && !g_mag_in_gun) || g_open || open_f > 0.01f || g_act_open || g_cover_hold;
        if (begun) g_hl_started = true;
        else if (tube ? g_chamber == 1 : rev || (g_mag_in_gun && !g_need_rack && !g_bolt_owed && !(boltt && g_bolt_open) && !g_racking)) g_hl_started = false;
        const bool cyl_dry = g_cyl_known ? cyl_live == 0 : (in.ammo_ok && in.clip == 0);
        const bool dry_now = magfed ? (g_need_rack || (keep_one() && in.ammo_ok && in.clip <= 1 && keep_res(in.reserve)) || (in.ammo_ok && in.clip == 0))
                           : tube   ? g_tube == 0 && g_chamber != 1
                                    : cyl_dry;
        // reloadhlaction: the gun cannot fire until its action is worked: a bolt owed or left open, a pump or lever action's chamber not live
        // (a fired shell after a shot, or none chambered), a lever cycling, a handle to rack.
        const bool action  = g_bolt_owed || (boltt && g_bolt_open) || (tube && g_chamber != 1) || g_lever_cyc || (!rev && !tube && !norack && g_need_rack);
        v.hl_show          = g_hl_when.load() >= 2 || dry_now || g_hl_started || action;
    }
    // reloadhighlight: the part to work next. A magazine gun run dry: its magazine (drop it), then the well while a magazine is in the hand,
    // then the handle once a fresh one is seated. A pump shotgun: its pump while the chamber needs it.
    if (g_hl_on.load()) {
        const bool magfed = !rev && !tube;
        const bool dry    = g_need_rack || (keep_one() && in.ammo_ok && in.clip <= 1 && keep_res(in.reserve)) || (in.ammo_ok && in.clip == 0);
        v.hl_mag    = magfed && !belt && g_mag_in_gun && dry && !g_mag_fresh && !g_racking && !g_hand_mag;
        v.hl_well   = magfed && (!belt || g_open) && !g_mag_in_gun && g_hand_mag;   // (reloadbeltzone: a belt's too, so the tray can be found)
        v.hl_handle = !rev && !lever && !norack && !g_racking && !g_hand_mag &&
                      (tube ? g_chamber != 1 : ((g_mag_in_gun && ((g_need_rack && g_mag_fresh) || (g_bolt_owed && !g_need_rack))) || (boltt && g_bolt_open)));
        // reloadhintglow2: a revolver's cylinder (run dry with rounds left: open it; open: empty it, load it, close it), a lever action's lever.
        const bool cyl_dry_hl = g_cyl_known ? cyl_live == 0 : (in.ammo_ok && in.clip == 0);
        v.hl_cyl   = rev && !noopen && !g_hand_mag && !g_engaged && (g_open || open_f > 0.01f || (cyl_dry_hl && in.ammo_ok && in.reserve > 0));
        // reloadcoverpush: a belt-fed gun's open feed cover over a new belt (shut it), brighter while in the hand.
        v.hl_cover = belt && g_belt_grip.load() && g_cover_push.load() && g_open && g_mag_in_gun && !g_hand_mag && !g_engaged;
        v.cover_held = g_cover_hold;
        v.hl_lever = lever && !g_hand_mag && !g_engaged && !g_lever_cyc && !g_lever_hand && !g_act_open && g_chamber != 1;
    }
    // wristammo (FarCry5WristAmmo.cpp): the count by hand (the reload thread, 5 Nov: cyl_live and by_hand on tube and revolver guns, not
    // the game's clip, which can hold more than is loaded by hand), from the state as this poll leaves it.
    {
        int live = 0;
        for (int i = 0; i < g_cyl_n; ++i) live += g_cyl[i] == 1;
        const int byh     = std::max(0, g_tube) + (g_chamber == 1 ? 1 : 0);
        const int kept_mg = !tube && !rev && keep_one() && in.ammo_ok && keep_res(in.reserve) ? 1 : 0;   // dry_at: a magazine gun stops at clip 1
        const int kept_tr = keep_tr && in.ammo_ok && keep_res(in.reserve) ? 1 : 0;                       // a tube or cylinder: one more in the clip
        const int clip_av = in.ammo_ok && in.clip >= 0 ? std::max(0, in.clip - (tube || rev ? kept_tr : kept_mg)) : -1;
        if (tube) {
            v.rounds     = g_tube >= 0 ? byh : -1;
            v.rounds_cap = tube_cap;
        } else if (rev) {
            v.rounds     = g_cyl_known ? live : -1;
            v.rounds_cap = g_cyl_n;
        } else {
            v.rounds      = clip_av < 0 ? -1 : g_mag_in_gun ? clip_av : 0;
            v.rounds_keep = kept_mg;
        }
        v.rounds_aside = clip_av >= 0 && v.rounds >= 0 ? std::max(0, clip_av - v.rounds) : 0;
        v.fire_held    = out.no_fire;
    }
    std::lock_guard vl{ g_view_mtx };
    g_view = v;
}

bool view(View& v)
{
    if (!g_in_hand.load()) return false;
    std::lock_guard _{ g_view_mtx };
    v = g_view;
    return v.active;
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    auto three = [&](std::atomic<float>* dst, const float* def, float lim) {
        float v[3]{ def[0], def[1], def[2] };
        if (!arg.empty()) {
            v[0] = fv;
            rest >> v[1] >> v[2];
        }
        for (int i = 0; i < 3; ++i) dst[i] = std::clamp(v[i], -lim, lim);
        persist = true;
    };
    if (cmd == "reloadmanual") {
        g_on    = on;
        persist = true;
        spdlog::info("[Reload] reloadmanual {}: {}", on ? "on" : "off",
                     on ? fmt::format("with a weapon of reloadweapons ({}) in hand, a quick right B drops the magazine and the left hip holds the next", list_text())
                        : std::string{ "the game reloads as before (right B: previous weapon / heal)" });
        return true;
    }
    if (cmd == "reloadweapons") {
        // manualreload20: "v2" marks a list saved since the pump shotguns became default. A list without it (saved before) gets them added, so a
        // saved list does not keep them off; F11 writes "v2", so a shotgun unticked there stays off.
        constexpr uint32_t kNewInV2[]{ 29259, 34497, 20586, 35499, 29841, 42549 };
        std::set<uint32_t> s;
        bool               v2 = false;
        auto add = [&](const std::string& t) {
            if (t == "v2") v2 = true;
            if (t.empty() || t == "none" || t == "v2") return;
            char*               e = nullptr;
            const unsigned long v = std::strtoul(t.c_str(), &e, 10);
            if (e && *e == 0 && v != 0) s.insert(uint32_t(v));
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        if (!v2) {
            std::string added;
            for (const uint32_t w : kNewInV2)
                if (s.insert(w).second) added += " " + std::to_string(w);
            if (!added.empty()) spdlog::info("[Reload] reloadweapons: a list from before the pump shotguns were listed by default: added{}", added);
        }
        {
            std::lock_guard _{ g_list_mtx };
            g_weapons = s;
        }
        persist = true;
        spdlog::info("[Reload] reloadweapons: {}", list_text().empty() ? "none" : list_text());
        return true;
    }
    if (cmd == "reloadkey") {
        g_key   = arg == "x" ? 1 : 0;
        persist = true;
        spdlog::info("[Reload] reloadkey {}: the game's reload is {}", g_key.load() ? "x" : "r", g_key.load() ? "the pad's X tapped" : "the keyboard's R tapped");
        return true;
    }
    if (cmd == "reloadtap") { g_tap_ms = std::clamp(arg.empty() ? kTapDefault : int(fv), 100, 800); persist = true; return true; }
    if (cmd == "reloadpouch") { three(g_pouch, kPouchDefault, 1.2f); return true; }
    if (cmd == "reloadpartstable") {
        g_parts_table = arg.empty() || on;
        persist       = true;
        spdlog::info("[Reload] reloadpartstable {}", g_parts_table.load() ? "on: a gun not built in takes its parts from the game files' table (FarCry5ReloadParts.h)"
                                                                         : "off: a gun not built in is learnt at its first game reload");
        return true;
    }
    if (cmd == "reloadrockin") {
        g_rock_deg = std::clamp(arg.empty() ? 0.0f : fv, 0.0f, 35.0f);
        persist    = true;
        spdlog::info("[Reload] reloadrockin {:.0f}{}", g_rock_deg.load(), g_rock_deg.load() > 0.0f ? " deg: an AK's magazine hooked at its front and rocked in" : ": off, straight in");
        return true;
    }
    if (cmd == "reloadroundslide") {
        g_round_slide = arg.empty() || on;
        persist       = true;
        spdlog::info("[Reload] reloadroundslide {}", g_round_slide.load() ? "on: a revolver's round taken by its chamber within 5 cm and 55 deg drops home by itself"
                                                                         : "off: a round is pushed all the way in by hand (within 3.5 cm and 40 deg)");
        return true;
    }
    if (cmd == "reloadseathold") {
        g_seat_hold_on = arg.empty() || on;
        persist        = true;
        spdlog::info("[Reload] reloadseathold {}", g_seat_hold_on.load() ? "on: after a shell, round or magazine goes in, the left grip holds the gun only after a new press"
                                                                          : "off: the grip still held as it goes in can take the gun at once");
        return true;
    }
    if (cmd == "reloadrevdump") {
        g_rev_dump = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadrevdump {}", g_rev_dump.load() ? "on: a revolver's fired cases drop out as its cylinder swings open" : "off: only with the muzzle tipped up");
        return true;
    }
    if (cmd == "reloadpouchanchor") {
        g_pouch_anchor = arg.empty() || on;
        persist        = true;
        spdlog::info("[Reload] reloadpouchanchor {}", g_pouch_anchor.load() ? "on: the hip's magazine goes with the eyes (viewpivot), as the gun and the hands do: turning the gun no longer moves it"
                                                                             : "off: the hip's magazine stays on the game camera, which swings as the gun turns (as before)");
        return true;
    }
    if (cmd == "reloadpouchneck") {
        g_pouch_neck = arg.empty() || on;
        persist      = true;
        spdlog::info("[Reload] reloadpouchneck {}", g_pouch_neck.load() ? "on: the hip's magazine hangs from the neck, on a heading that follows the head's only past 60 deg"
                                                                         : "off: the hip's magazine hangs from the eyes, on the body's heading");
        return true;
    }
    if (cmd == "reloadpouchzone") { g_pouch_zone = std::clamp(arg.empty() ? kPouchZoneDefault : fv, 0.05f, 0.35f); persist = true; return true; }
    if (cmd == "reloadhandmag") { three(g_hand_off, kHandMagDefault, 0.4f); return true; }
    if (cmd == "reloadtravel") { g_travel = std::clamp(arg.empty() ? kTravelDefault : fv, 0.02f, 0.12f); persist = true; return true; }
    if (cmd == "reloadrack") { g_rack_len = std::clamp(arg.empty() ? kRackDefault : fv, 0.02f, 0.15f); persist = true; return true; }
    if (cmd == "reloadlog") { g_log = arg.empty() || on; return true; }
    // Switches retired 9 Nov with the ammo ledger always on (author 08:28): reloadnogame and reloadkeepone (always on), ammoledger (always on), reloadfullmag (removed: it never worked). A saved line of
    // theirs from an older settings file is taken and dropped (not saved again), with nothing done.
    if (cmd == "reloadnogame" || cmd == "reloadkeepone" || cmd == "ammoledger" || cmd == "reloadfullmag") return true;
    if (cmd == "reloadhandgame") {
        g_hand_game = arg.empty() || on;
        fc5::fingers::use_builtin_grips(g_hand_game.load());
        persist     = true;
        spdlog::info("[Reload] reloadhandgame {}", g_hand_game.load() ? "on: a handgun's magazine in the hand, its hand on the slide and its slide held back when empty as in the game's own reload"
                                                                      : "off: the magazine in the fist and the slide grip as before");
        return true;
    }
    if (cmd == "reloadrec") {
        g_rec = arg.empty() || on;
        fc5::fingers::track_bends(g_rec.load());
        fc5::gamesound::record(g_rec.load() || g_snd_learn.load());
        spdlog::info("[Reload] reloadrec {}", g_rec.load() ? "on: the game's reload and the manual steps recorded frame by frame in fc5vr_reloadrec.csv" : "off");
        return true;
    }
    if (cmd == "reloadgamehand") {
        g_game_hand = arg.empty() || on;
        spdlog::info("[Reload] reloadgamehand {}", g_game_hand.load() ? "on: in the game's reload from its button the left hand stays where the game has it" : "off");
        return true;
    }
    if (cmd == "reloadmagmodel") {
        g_model_on = arg.empty() || on;
        if (!g_model_on.load()) g_model_shown = false;
        persist = true;
        spdlog::info("[Reload] reloadmagmodel {}", g_model_on.load() ? "on: the game's own magazine (learnt at the first drop with each weapon)" : "off: the drawn magazine only");
        return true;
    }
    if (cmd == "reloadgeom") {
        // The whole list each time (saved as it is): reloadgeom 57819:wx,wy,wz,ax,ay,az,hx,hy,hz,measured ... | none.
        std::map<uint32_t, Geom> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            std::vector<float>  v;
            std::stringstream   ss{ t.substr(c + 1) };
            std::string         n;
            while (std::getline(ss, n, ',')) v.push_back(std::strtof(n.c_str(), nullptr));
            if (w == 0 || v.size() < 9) return;
            Geom g{};
            // The points (the well, the handle), not the axis nor the flags after them (until 2 Nov 12:00 the flags were clamped to 0.5 too, and
            // read as 0). manualreload39: until 3 Nov the axis was clamped as well and normalised again, so each save and load tilted it further
            // from the gun's up (the AR-C's 10 deg grew to 33, the revolver's round axis to 49): magazines and rounds no longer lined up with
            // their wells. A measured axis is measured again at the first quiet hold of each session, so the saved ones mend themselves.
            for (size_t i = 0; i < 9; ++i)
                if (i < 3 || i >= 6) v[i] = std::clamp(v[i], -0.5f, 0.5f);
            g.well     = { v[0], v[1], v[2] };
            g.axis     = glm::length(glm::vec3{ v[3], v[4], v[5] }) > 1e-3f ? glm::normalize(glm::vec3{ v[3], v[4], v[5] }) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            g.handle   = { v[6], v[7], v[8] };
            g.measured   = v.size() >= 10 && v[9] > 0.5f;
            g.handle_set = v.size() >= 11 && v[10] > 0.5f;
            if (glm::length(g.well) > 0.30f) {
                // 2 Nov: the first learn measured the AR-C's well 1.4 m away (the arms taken for the magazine), saved clamped to (-0.5, -0.5, -0.5)
                // and, its flag lost, kept as if set by hand: the hand's and the hip's magazines and the yellow ball were placed from it.
                spdlog::info("[Reload] reloadgeom {}: a well {:.2f} m from the controller dropped (the default until measured on the magazine)", w, glm::length(g.well));
                const Geom d{};
                g.well     = d.well;
                g.axis     = d.axis;
                g.measured = false;
            }
            if (glm::length(g.handle) > 0.40f) {
                g.handle     = Geom{}.handle;
                g.handle_set = false;
            }
            mp[uint32_t(w)] = g;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_list_mtx };
            g_geom = mp;
            line   = geom_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadmagmesh") {
        // The whole list each time: reloadmagmesh 57819:4321/12+13,987/r,555/p ... | none (learnt again at the next drop).
        std::map<uint32_t, std::vector<MagPart>> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long   w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            std::vector<MagPart>  v;
            std::stringstream     ss{ t.substr(c + 1) };
            std::string           n;
            while (std::getline(ss, n, ',')) {
                const size_t sl = n.find('/');
                MagPart      p{};
                p.count = uint32_t(std::strtoul(n.substr(0, sl).c_str(), nullptr, 10));
                if (p.count == 0) continue;
                const std::string kind = sl == std::string::npos ? std::string{ "r" } : n.substr(sl + 1);
                if (kind == "p") p.prop = true;
                else if (kind == "r" || kind.empty()) p.rigid = true;
                else {
                    std::stringstream bs{ kind };
                    std::string       b;
                    while (std::getline(bs, b, '+') && p.nbones < 4) {
                        const unsigned long bi = std::strtoul(b.c_str(), nullptr, 10);
                        if (bi < 256) p.bones[p.nbones++] = uint32_t(bi);
                    }
                    if (p.nbones == 0) continue;
                }
                if (!part_ok(p)) {
                    spdlog::info("[Reload] reloadmagmesh {}: draw {} bone {} dropped (a bone above {}: the arms or the body, not the weapon)", w, p.count, p.bones[p.nbones - 1], kMaxWeaponBone);
                    continue;
                }
                if (v.size() < 8) v.push_back(p);
            }
            if (std::none_of(v.begin(), v.end(), [](const MagPart& q) { return !q.prop; })) v.clear();   // a prop alone: not learnt
            if (w != 0 && !v.empty()) mp[uint32_t(w)] = v;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_list_mtx };
            g_parts = mp;
            line    = parts_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloaddrop") {
        g_drop_fall = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloaddrop {}", g_drop_fall.load() ? "on: the dropped magazine falls to the ground and lies there 20 s" : "off: the dropped magazine vanishes");
        return true;
    }
    if (cmd == "reloadsound" || cmd == "reloadsoundvol") {
        spdlog::info("[Reload] {}: removed (manualreload50b): the reload plays the game's own sounds only", cmd);
        return true;
    }
    if (cmd == "reloadgamesounds") {
        std::string names, t = arg;
        for (int i = 0; i < kSndCount; ++i) {
            if (i > 0 && !(rest >> t)) t.clear();
            char*          e  = nullptr;
            const uint32_t id = uint32_t(std::strtoul(t.c_str(), &e, 0));
            const int      ty = e && *e == ':' ? std::atoi(e + 1) : 15;
            g_game_snd_type[i] = ty;
            g_game_snd[i]      = id;
            names += id ? fmt::format(" {}={}:{}", kSndName[i], id, ty) : fmt::format(" {}=none", kSndName[i]);
        }
        persist = true;
        spdlog::info("[Reload] reloadgamesounds:{}", names);
        return true;
    }
    if (cmd == "reloadcoverdeg") {
        g_cover_deg = std::clamp(arg.empty() ? -70.0f : fv, -120.0f, 120.0f);
        persist     = true;
        spdlog::info("[Reload] reloadcoverdeg {:.0f}: a belt-fed gun's feed cover lifts {:.0f} deg about its hinge", g_cover_deg.load(), g_cover_deg.load());
        return true;
    }
    if (cmd == "reloadcoverpush") {
        g_cover_push = arg.empty() || on;
        persist      = true;
        spdlog::info("[Reload] reloadcoverpush {}", g_cover_push.load() ? "on: a belt-fed gun's open cover glows over a new belt; left grip near it takes it, push it down to shut it"
                                                                        : "off: a left grip near the tray (or a quick right B) shuts the cover at once");
        return true;
    }
    if (cmd == "reloadratslide") {
        g_rat_slide = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadratslide {}", g_rat_slide.load() ? "on: the RAT4's missile goes in through the open back, pushed forward by the hand"
                                                                      : "off: the RAT4's missile brought near the open back is in at once");
        return true;
    }
    if (cmd == "reloadlauncherfill") {
        g_launcher_fill = arg.empty() || on;
        persist         = true;
        spdlog::info("[Reload] reloadlauncherfill {}", g_launcher_fill.load() ? "on: a launcher taken in hand empty gets its clip filled at once, so its round shows at the hip (the chamber stays empty)"
                                                                              : "off: a launcher taken in hand empty shows no round until the game's own reload");
        return true;
    }
    if (cmd == "reloadhiploaded") {
        g_hip_loaded = arg.empty() || on;
        persist      = true;
        spdlog::info("[Reload] reloadhiploaded {}", g_hip_loaded.load() ? "on: a one-shot launcher's spare round shows at the hip with the launcher loaded too"
                                                                        : "off: a one-shot launcher's round shows at the hip only with an empty chamber to load");
        return true;
    }
    if (cmd == "reloadgrenadefill") {
        g_grenade_fill = arg.empty() || on;
        persist        = true;
        spdlog::info("[Reload] reloadgrenadefill {}", g_grenade_fill.load() ? "on: the M79's clip filled at once after its shot (no reload of the game's; load it by hand)"
                                                                            : "off: the M79's shot is followed by the game's own reload, as before (the RPG-7 and RAT4 still filled at once)");
        return true;
    }
    if (cmd == "reloadgrenadeseat") {
        g_grenade_seat = arg.empty() || on;
        persist        = true;
        spdlog::info("[Reload] reloadgrenadeseat {}", g_grenade_seat.load() ? "on: the M79's grenade sits on its bore, its base at the breech (from the next pick-up)"
                                                                            : "off: the M79's grenade where the game's sits (from the next pick-up)");
        return true;
    }
    if (cmd == "reloadshellhip") {
        g_shell_hip = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadshellhip {}", g_shell_hip.load() ? "on: a double barrel's spare shell at the hip has its middle on the hip zone's centre"
                                                                      : "off: a double barrel's spare shell at the hip has its nose 3 cm above the zone's centre, as before");
        return true;
    }
    if (cmd == "reloadsamegun") {
        g_same_gun = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadsamegun {}", g_same_gun.load() ? "on: the RAT4's ids 28497, 80077 and 69646 are one weapon to the reload (no reset between them)"
                                                                    : "off: each RAT4 id is its own weapon (a change resets the reload)");
        return true;
    }
    if (cmd == "reloadbeltgrip") {
        g_belt_grip = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadbeltgrip {}", g_belt_grip.load() ? "on: a belt-fed gun: quick right B drops the belt and lifts the cover, left grip lays the new belt, left grip on it closes the cover, left grip racks the handle"
                                                                      : "off: quick right B closes the cover; the handle racked only after running dry");
        return true;
    }
    if (cmd == "bheal") {
        g_b_heal = arg.empty() || on;
        persist  = true;
        spdlog::info("[Reload] bheal {}", g_b_heal.load() ? "on: right B held heals; a quick press switches no weapon"
                                                          : "off: right B is the pad's Y as before (tap: previous weapon; held: heal)");
        return true;
    }
    if (cmd == "reloadsoundfill") {
        g_snd_fill = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadsoundfill {}", g_snd_fill.load() ? "on: a handgun with no sounds of its own uses the 1911's game sounds"
                                                                       : "off: a handgun with no sounds of its own is silent");
        return true;
    }
    if (cmd == "reloadsoundseq") {
        g_seq_on = arg.empty() || on;
        persist  = true;
        spdlog::info("[Reload] reloadsoundseq {}", g_seq_on.load() ? "on: each event plays its sequence of the game's own reload sounds (every weapon of the sound run; a weapon's own pick comes first)"
                                                                   : "off: one sound per event (picked, learnt or built in)");
        return true;
    }
    if (cmd == "reloadsoundseqcut") {
        // The whole list each time: reloadsoundseqcut 57819:0.10,0.60,0.85,0.98,1.80 ... | none (the built-in cuts).
        std::map<uint32_t, std::array<float, 5>> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            std::array<float, 5> v{};
            std::stringstream    ss{ t.substr(c + 1) };
            std::string          x;
            int                  i = 0;
            while (i < 5 && std::getline(ss, x, ',')) v[size_t(i++)] = std::clamp(std::strtof(x.c_str(), nullptr), 0.0f, 10.0f);
            if (w == 0 || i < 5) return;
            std::sort(v.begin(), v.end());
            mp[uint32_t(w)] = v;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        {
            std::lock_guard _{ g_seq_mtx };
            g_seq_cut = mp;
        }
        persist = true;
        spdlog::info("[Reload] reloadsoundseqcut: {} weapon(s) with their own cuts", mp.size());
        return true;
    }
    if (cmd == "reloadroundpinch") {
        g_round_pinch = arg.empty() || on;
        persist       = true;
        spdlog::info("[Reload] reloadroundpinch {}", g_round_pinch.load() ? "on: a round or a shell held between the thumb and index tips, in front of the fist"
                                                                          : "off: a round or a shell held in the fist");
        return true;
    }
    if (cmd == "reloadmagpalm") {
        g_mag_palm = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadmagpalm {}", g_mag_palm.load() ? "on: a magazine held in the whole hand, the fist at the middle of its length (7 cm below its top at most)"
                                                                    : "off: reloadhandmag, or the game's own hold with reloadgamehold");
        return true;
    }
    if (cmd == "reloadslidegrab") {
        if (arg == "default") {
            for (int i = 0; i < 3; ++i) g_slide_grab[i] = kGrabSlideDefault[i];
            persist = true;
        } else {
            three(g_slide_grab, kGrabSlideDefault, 0.12f);
        }
        spdlog::info("[Reload] reloadslidegrab {:.3f} {:.3f} {:.3f}: a handgun's slide with no grip learnt or built in, its handle point from the left controller "
                     "(gun controller's axes, m: x right, y up, z back){}",
                     g_slide_grab[0].load(), g_slide_grab[1].load(), g_slide_grab[2].load(),
                     g_slide_grab[0].load() == kGrabSlideDefault[0] && g_slide_grab[1].load() == kGrabSlideDefault[1] && g_slide_grab[2].load() == kGrabSlideDefault[2]
                         ? " (default)" : "");
        return true;
    }
    if (cmd == "reloadmagfist") {
        g_mag_fist = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadmagfist {}", g_mag_fist.load() ? "on: a handgun's magazine in the middle of the fist sideways (beside the controller as the gun's grip is beside the other)"
                                                                    : "off: a handgun's magazine on the controller's line, as before 4 Nov");
        return true;
    }
    if (cmd == "reloadhandgrab") {
        g_hand_grab = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadhandgrab {}", g_hand_grab.load() ? "on: the hand pulling a handle, slide or pump drawn at one place on it per weapon (the game's grip), wherever the grip was pressed"
                                                                      : "off: the hand stays where it took the handle");
        return true;
    }
    if (cmd == "reloadhandgrabw") {
        // The whole list each time: reloadhandgrabw 57819:-0.090,0.078,0.132 ... | none
        std::map<uint32_t, glm::vec3> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            glm::vec3           gv{ 0.0f };
            if (w == 0 || std::sscanf(t.c_str() + c + 1, "%f,%f,%f", &gv.x, &gv.y, &gv.z) != 3) return;
            if (glm::length(gv) <= 0.40f) mp[uint32_t(w)] = gv;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_hold_mtx };
            g_grab_hold = mp;
            line        = grab_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadhandsnap") {
        g_hand_snap = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadhandsnap {}", g_hand_snap.load() ? "on: the free hand drawn on the magazine going in and on the handle, slide or pump it pulls"
                                                                      : "off: the free hand drawn at its controller");
        return true;
    }
    if (cmd == "reloadhighlight") {
        g_hl_on = arg.empty() || on;
        persist = true;
        spdlog::info("[Reload] reloadhighlight {}", g_hl_on.load() ? "on: the gun's part to work next glows (the empty magazine, the well, the handle or pump)" : "off");
        return true;
    }
    if (cmd == "reloadhlwhen") {
        // reloadhlaction: "shot" (saved by the 14:40 build) is reload now: an action owed after a shot always lights.
        const int w = arg == "reload" || arg == "shot" ? 0 : arg == "always" ? 2 : -1;
        if (w < 0) {
            spdlog::info("[Reload] reloadhlwhen {}: unknown (reload or always); stays {}", arg, g_hl_when.load() >= 2 ? "always" : "reload");
            return true;
        }
        g_hl_when = w;
        persist   = true;
        spdlog::info("[Reload] reloadhlwhen {}", w == 0 ? "reload: the highlights only with the gun run dry, a reload begun, or an action needed before it fires (bolt, pump, lever, handle)"
                                                        : "always: the highlights as before (also the handle while the free hand is near it)");
        return true;
    }
    if (cmd == "reloadhintstyle") {
        if (arg != "glow" && arg != "boxes") {
            spdlog::info("[Reload] reloadhintstyle {}: unknown (glow or boxes); stays {}", arg, g_hint_glow.load() ? "glow" : "boxes");
            return true;
        }
        g_hint_glow = arg == "glow";
        persist     = true;
        spdlog::info("[Reload] reloadhintstyle {}", g_hint_glow.load() ? "glow: the gun's own magazine, handle, slide or pump glows; a ghost magazine shows the well (a part that cannot glow keeps its box)"
                                                                     : "boxes: the boxes round the parts and the well and handle balls, as before");
        return true;
    }
    for (int t = 0; t < kTypeCount; ++t)   // allreload: each kind of weapon on its own switch
        if (cmd == kTypeCmd[t]) {
            g_type_on[t] = arg.empty() || on;
            persist      = true;
            spdlog::info("[Reload] {} {}: {}", cmd, g_type_on[t].load() ? "on" : "off", g_type_on[t].load() ? fmt::format("{} reload(s) by hand, ticked or not", kTypeName[t])
                                                                                                          : fmt::format("{} only when ticked in reloadweapons", kTypeName[t]));
            return true;
        }
    if (cmd == "reloadnostuck") {
        g_no_stuck = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadnostuck {}", g_no_stuck.load() ? "on: a weapon held from firing as empty while the game's clip has rounds is loaded from that clip (at once when nothing can go in by hand, else after 6 s with nothing done)"
                                                                    : "off: a weapon held from firing as empty stays so until loaded by hand");
        return true;
    }
    if (cmd == "reloadbutton") {
        g_btn_on = arg.empty() || on;
        persist  = true;
        spdlog::info("[Reload] reloadbutton {}", g_btn_on.load() ? "on: the reload button (left Y) still reloads, as the game shows it; the manual reload counts the weapon loaded"
                                                                 : "off: the reload button is the game's, and the manual reload does not follow it");
        return true;
    }
    if (cmd == "reloadbuttonguard") {
        g_guard_on = arg.empty() || on;
        persist    = true;
        spdlog::info("[Reload] reloadbuttonguard {}", g_guard_on.load() ? "on: during a manual step, left Y reaches the game only once held reloadbuttonguardms"
                                                                        : "off: every press of left Y reaches the game");
        return true;
    }
    if (cmd == "reloadactionopen") {
        g_act_on = arg.empty() || on;
        persist  = true;
        spdlog::info("[Reload] reloadactionopen {}", g_act_on.load() ? "on: the lever thrown down or the pump left back stays open, and shells go in from near the port" : "off: a lever or pump goes back and forth in one stroke");
        return true;
    }
    if (cmd == "reloadflick") {
        g_flick = arg.empty() || on;
        persist = true;
        spdlog::info("[Reload] reloadflick {}", g_flick.load() ? "on: a flick of the gun closes what B opened (sideways for a revolver)" : "off: B or the hand closes it");
        return true;
    }
    if (cmd == "reloadboltturn") {
        g_bolt_turn = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadboltturn {}", g_bolt_turn.load() ? "on: a bolt action's bolt turned up, pulled back, pushed forward and turned down by the hand, each step following it"
                                                                     : "off: the bolt pulled straight back to its stop and let go");
        return true;
    }
    if (cmd == "reloadboltparts") {
        g_bolt_parts = arg.empty() || on;
        persist      = true;
        spdlog::info("[Reload] reloadboltparts {}", g_bolt_parts.load() ? "on: a bolt action's other bolt parts (bolt stop, bolt cover) kept at rest on the gun, as its bolt"
                                                                         : "off: they move with the game's own cycle after a shot");
        return true;
    }
    if (cmd == "reloadboltdeg") {
        g_bolt_deg = std::clamp(arg.empty() ? 60.0f : fv, -120.0f, 120.0f);
        persist    = true;
        spdlog::info("[Reload] reloadboltdeg {:.0f}: the bolt turns this far up (negative: the other way)", g_bolt_deg.load());
        return true;
    }
    if (cmd == "reloadboltlift") {
        g_bolt_lift = std::clamp(arg.empty() ? 0.04f : fv, 0.01f, 0.12f);
        persist     = true;
        spdlog::info("[Reload] reloadboltlift {:.3f} m: the hand rises this far to turn the bolt fully up", g_bolt_lift.load());
        return true;
    }
    if (cmd == "reloadhandhold") {
        g_hand_hold = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadhandhold {}", g_hand_hold.load() ? "on: the game's cycle after a shot does not move the weapon hand" : "off: it does");
        return true;
    }
    if (cmd == "reloadrackloss") {
        g_rack_loss = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadrackloss {}", g_rack_loss.load() ? "on: a rack with a round in costs a bullet from the magazine" : "off: seen only");
        return true;
    }
    if (cmd == "reloadmaglower") {
        g_mag_lower = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadmaglower {}", g_mag_lower.load() ? "on: a magazine held in the fist round its lower half, the fingers round it" : "off: as the game's hand holds it");
        return true;
    }
    if (cmd == "reloadshotmute") {
        g_shot_mute = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadshotmute {}", g_shot_mute.load() ? "on: the game's own pump, lever or bolt sounds after a shot not played" : "off: played, as before");
        return true;
    }
    if (cmd == "reloadleverhand") {
        g_lever_hand_on = arg.empty() || on;
        persist         = true;
        spdlog::info("[Reload] reloadleverhand {}", g_lever_hand_on.load() ? "on: with the left grip on the fore-end, the right hand dropped and brought back works a lever action's lever; quick right B too"
                                                                           : "off: a lever action's lever by quick right B only");
        return true;
    }
    if (cmd == "reloadbuttonguardall") {
        g_guard_all = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadbuttonguardall {}", g_guard_all.load() ? "on: with a gun of the manual reload in hand, left Y reaches the game only once held reloadbuttonguardms"
                                                                            : "off: only during a manual step (reloadbuttonguard)");
        return true;
    }
    if (cmd == "reloadbuttontest") {   // reloadbuttontest <ms> (test): left Y held this long, through reloadbuttonguard as a real press (padtap goes round the guard)
        const int ms = std::clamp(std::atoi(arg.c_str()), 10, 5000);
        g_btn_test_until = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() + ms;
        spdlog::info("[Reload] reloadbuttontest: left Y held {} ms", ms);
        return true;
    }
    if (cmd == "reloadbuttonguardms") {
        g_guard_ms = std::clamp(arg.empty() ? 500 : int(fv), 150, 1500);
        persist    = true;
        spdlog::info("[Reload] reloadbuttonguardms {}: during a manual step, left Y reaches the game once held {} ms", g_guard_ms.load(), g_guard_ms.load());
        return true;
    }
    if (cmd == "reloadcasinghide") {
        g_casing_on = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadcasinghide {}", g_casing_on.load() ? "on: the game's own spent casing hidden with a gun whose shell leaves by the pump, lever or bolt"
                                                                        : "off: the game's own spent casing shown too");
        return true;
    }
    if (cmd == "noautoswitch") {
        g_no_switch = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] noautoswitch {}", g_no_switch.load() ? "on: with no ammo left the last round stays in the game's clip unfired, so the game never switches weapon by itself"
                                                                    : "off: with no ammo left the last round fires and the game switches to another weapon by itself");
        return true;
    }
    if (cmd == "bhealms") {
        g_heal_ms = std::clamp(arg.empty() ? kHealMsDefault : int(fv), 300, 2000);
        persist   = true;
        spdlog::info("[Reload] bhealms {}: a held right B is the pad's Y for at least {} ms", g_heal_ms.load(), g_heal_ms.load());
        return true;
    }
    if (cmd == "reloadsoundlearn") {
        g_snd_learn = arg.empty() || on;
        fc5::gamesound::record(g_snd_learn.load());
        persist = true;
        spdlog::info("[Reload] reloadsoundlearn {}", g_snd_learn.load() ? "on: each game reload's handling sounds matched to its magazine's and handle's moves, kept per weapon (reloadsoundlearnt) "
                                                                          "and used after a pick, before the built-in guess"
                                                                        : "off: picks and the built-in guesses only");
        return true;
    }
    if (cmd == "reloadproblem" || cmd == "reloadlearntuse") {
        std::map<uint32_t, uint32_t> mp;
        parse_masks(arg, rest, mp);
        std::string line;
        {
            std::lock_guard _{ g_hold_mtx };
            (cmd == "reloadproblem" ? g_problem : g_learnt_use) = mp;
            line = mask_line(cmd.c_str(), mp);
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadsoundlearnt") {
        // The whole list each time: reloadsoundlearnt 57819:3863307842,4119235673,-,- ... | none
        std::map<uint32_t, std::array<int64_t, 4>> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            if (w == 0) return;
            std::array<int64_t, 4> a{ -1, -1, -1, -1 };
            std::stringstream      ss{ t.substr(c + 1) };
            std::string            v;
            for (size_t i = 0; i < 4 && std::getline(ss, v, ','); ++i)
                if (!v.empty() && v != "-") a[i] = int64_t(std::strtoull(v.c_str(), nullptr, 10) & 0xFFFFFFFFull);
            if (std::any_of(a.begin(), a.end(), [](int64_t x) { return x > 0; })) mp[uint32_t(w)] = a;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_pick_mtx };
            g_snd_learnt = mp;
            line         = learnt_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadweaponsounds") {
        g_wsnd  = arg.empty() || on;
        persist = true;
        spdlog::info("[Reload] reloadweaponsounds {}", g_wsnd.load() ? "on: each weapon's own game reload sounds (built in from the arcade map's sound run, or picked in F11)"
                                                                     : "off: no reload sounds (or reloadgamesounds')");
        return true;
    }
    if (cmd == "reloadgamehold") {
        g_game_hold = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadgamehold {}", g_game_hold.load() ? "on: a magazine in the left hand sits where the game's hand holds that weapon's spare (learnt from its reload; "
                                                                       "else reloadhandmag)"
                                                                     : "off: reloadhandmag for every weapon");
        return true;
    }
    if (cmd == "reloadhandmagw") {
        // The whole list each time: reloadhandmagw 57819:0.012,0.064,-0.020 ... | none
        std::map<uint32_t, glm::vec3> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            glm::vec3           v{ 0.0f };
            if (w == 0 || std::sscanf(t.c_str() + c + 1, "%f,%f,%f", &v.x, &v.y, &v.z) != 3) return;
            if (glm::length(v) <= 0.25f) mp[uint32_t(w)] = v;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_hold_mtx };
            g_hold_mag = mp;
            line       = hold_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadsoundcat") {
        g_snd_cat = arg.empty() || on;
        persist   = true;
        spdlog::info("[Reload] reloadsoundcat {}", g_snd_cat.load() ? "on: the sounds picked in F11 are for the weapon's category (handguns, rifles and SMGs, shotguns, revolvers, belt-fed guns, the flamethrower)"
                                                                   : "off: the sounds picked in F11 are for that weapon");
        return true;
    }
    if (cmd == "reloadsoundpick") {
        // The whole list each time: reloadsoundpick 57819:4246560353,894646864,-,0 ... | none. Each value: - the built-in sound, 0 none, or an id.
        std::map<uint32_t, std::array<int64_t, 4>> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':');
            if (t.empty() || t == "none" || c == std::string::npos) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            if (w == 0) return;
            std::array<int64_t, 4> a{ -1, -1, -1, -1 };
            std::stringstream      ss{ t.substr(c + 1) };
            std::string            v;
            for (size_t i = 0; i < 4 && std::getline(ss, v, ','); ++i)
                if (!v.empty() && v != "-") a[i] = int64_t(std::strtoull(v.c_str(), nullptr, 10) & 0xFFFFFFFFull);
            if (std::any_of(a.begin(), a.end(), [](int64_t x) { return x >= 0; })) mp[uint32_t(w)] = a;
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_pick_mtx };
            g_snd_pick = mp;
            line       = pick_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadsoundtry") {
        // reloadsoundtry <id> | <event> (test): play a game sound id (type 15), or the weapon in hand's sound for mag_out|mag_in|rack_back|rack_fwd.
        int k = -1;
        for (int i = 0; i < kSndCount; ++i)
            if (arg == kSndName[i]) k = i;
        if (k >= 0) sound(k);
        else if (const unsigned long id = std::strtoul(arg.c_str(), nullptr, 10); id != 0)
            spdlog::info("[Reload] reloadsoundtry {}: {}", id, fc5::gamesound::post(uint32_t(id), 15, 0.0f) ? "posted" : "the game's sound call not usable");
        return true;
    }
    if (cmd == "reloadpumpgrip") {
        g_pump_grip = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadpumpgrip {}", g_pump_grip.load() ? "on: the left grip takes a pump shotgun's pump anywhere along the fore-end, or held there already once the chamber needs it; "
                                                                        "the stroke is the left hand coming back towards the right, and the gun keeps its aim on the right hand during it"
                                                                      : "off: the pump taken only near its centre on a new press, pulled along the right controller (as before)");
        return true;
    }
    if (cmd == "reloadpumpshot") {
        g_pump_shot = arg.empty() || on;
        persist     = true;
        spdlog::info("[Reload] reloadpumpshot {}", g_pump_shot.load() ? "on: a pump shotgun's pump is worked after each shot (pull it back and push it forward)"
                                                                       : "off: a pump shotgun chambers its next shell by itself after a shot (the pump only after loading an empty gun)");
        return true;
    }
    if (cmd == "reloaddropfloor") {
        g_drop_floor = std::clamp(arg.empty() ? 1.6f : fv, 0.3f, 2.5f);
        persist      = true;
        return true;
    }
    if (cmd == "reloadhandlepart") {
        // The whole list each time: reloadhandlepart 57819:57819/5,... | none (found again at the next reload from empty).
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> mp;
        auto add = [&](const std::string& t) {
            const size_t c = t.find(':'), sl = t.find('/');
            if (t.empty() || t == "none" || c == std::string::npos || sl == std::string::npos || sl < c) return;
            const unsigned long w = std::strtoul(t.substr(0, c).c_str(), nullptr, 10);
            const unsigned long n = std::strtoul(t.substr(c + 1, sl - c - 1).c_str(), nullptr, 10);
            const std::string   b = t.substr(sl + 1);
            const unsigned long bi = b == "r" ? kRigidBone : std::strtoul(b.c_str(), nullptr, 10);
            if (w == 0 || n == 0 || (bi != kRigidBone && bi > kMaxWeaponBone)) return;
            if (w == kArc && n == kArc && (bi == 6 || bi == 9)) {
                // Saved by earlier builds: 9 (the nearest part, its right tab) or 6 (learnt from the game's reload from empty: the bolt, so the
                // blue ball sat at the chamber, 2 Nov 12:32). The built-in handle (7, with its tabs and the bolt) stands instead.
                spdlog::info("[Reload] reloadhandlepart {}: part {}/{} dropped ({}): the built-in charging handle {}/7 (with its tabs 8, 9 and the bolt 6) instead", w, n, bi,
                             bi == 6 ? "the bolt" : "the handle's right tab", kArc);
                return;
            }
            mp[uint32_t(w)] = { uint32_t(n), uint32_t(bi) };
        };
        add(arg);
        std::string t;
        while (rest >> t) add(t);
        std::string line;
        {
            std::lock_guard _{ g_list_mtx };
            g_handle_parts = mp;
            line           = handle_line();
        }
        persist = true;
        spdlog::info("[Reload] {}", line);
        return true;
    }
    if (cmd == "reloadwellhere" || cmd == "reloadhandlehere") {
        // From the hands at the last poll: the held magazine's top (reloadwellhere: hold it seated where it should go) or the left
        // controller (reloadhandlehere: hold it on the charging handle), in the weapon controller's axes. Saved through reloadgeom.
        View v{};
        if (!view(v)) {
            spdlog::info("[Reload] {}: no listed weapon in hand", cmd);
            return true;
        }
        std::string line;
        {
            std::lock_guard _{ g_mtx };
            if (!g_last_ok) {
                spdlog::info("[Reload] {}: the controllers were not tracked at the last poll", cmd);
                return true;
            }
            const bool      lh  = g_last_mirror;
            const glm::vec3 hof{ (lh ? -1.0f : 1.0f) * g_hand_off[0].load(), g_hand_off[1].load(), g_hand_off[2].load() };
            glm::vec3       p   = glm::transpose(g_last_gun_rot) * ((cmd == "reloadwellhere" ? g_last_hand_pos + g_last_hand_rot * hof : g_last_hand_pos) - g_last_gun_pos);
            if (lh) p.x = -p.x;
            std::lock_guard l{ g_list_mtx };
            Geom& g = g_geom[v.sig];
            if (cmd == "reloadwellhere") {
                g.well      = p;
                glm::vec3 a = glm::transpose(g_last_gun_rot) * glm::normalize(g_last_hand_rot[1]);
                if (lh) a.x = -a.x;
                g.axis     = glm::length(a) > 1e-3f ? glm::normalize(a) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                g.measured = false;
            } else {
                g.handle     = p;
                g.handle_set = true;
            }
            line = geom_line();
            spdlog::info("[Reload] {} for weapon {}: ({:.3f}, {:.3f}, {:.3f}) m in the weapon controller's axes", cmd, v.sig, p.x, p.y, p.z);
        }
        // Saved like the other per-weapon lists: the whole reloadgeom line goes through the command queue (take_save_line, render thread).
        {
            std::lock_guard _{ g_save_mtx };
            g_save_line = line;
        }
        return true;
    }
    if (cmd == "reloadscan") {
        const float sec = std::clamp(arg.empty() ? 10.0f : fv, 0.0f, 120.0f);
        g_scan_until    = (clock::now() + std::chrono::milliseconds(int(sec * 1000.0f))).time_since_epoch().count();
        spdlog::info("[Reload] reloadscan {:.0f} s: each weapon-pass draw of a listed weapon logged once, and the moving bones of each ([ReloadMag] lines)", sec);
        return true;
    }
    if (cmd == "reloadforce") {
        g_force = arg == "dry" ? 1 : arg == "out" ? 2 : arg == "full" ? 3 : arg == "take" ? 4 : arg == "geom" ? 5 : 0;
        if (arg == "button") g_btn_edge = true;   // reloadmatch: as the reload button pressed (the game's reload, shown)
        spdlog::info("[Reload] reloadforce {} (at the next poll with a listed weapon in hand)", arg);
        return true;
    }
    return false;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint, uint32_t sig)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("Manual reload: quick B drops the magazine, take the next from your left hip##reloadmanual", &v)) cmds += v ? "reloadmanual on\n" : "reloadmanual off\n";
    hint("With a ticked weapon in your hands (the AR-C is ticked already): a quick press of right B drops the magazine. Held longer, right B heals (Debug > Buttons: Right B). "
         "With no ticked weapon in hand a quick B does nothing (switch weapons at your shoulders). "
         "A magazine always sits on your left hip: left grip there takes it. Bring it under the gun, where the yellow ball shows the magazine well, and push it "
         "up: it slides in with a tick every centimetre and clicks home. If you had fired every round, rack the charging handle too: left grip on the "
         "blue ball at the back of the gun (its T handle; a pistol: the back of its slide), pull back until it clicks. You can pull the handle at any time: it moves, but only chambers a "
         "round after a fresh magazine once the gun ran empty. The gun does not fire until all that is done and the game has refilled the clip. "
         "On by default (3 Nov). Off: the game reloads as before. Command reloadmanual on/off.");
    bool hlt = g_hl_on.load();
    if (ImGui::Checkbox("Show hints: the part to work next lights up##reloadhighlight", &hlt)) cmds += hlt ? "reloadhighlight on\n" : "reloadhighlight off\n";
    hint("On (default, author 3 Nov): a glowing box round the gun's part to work next, measured on the gun's model: orange the empty magazine "
         "(drop it), yellow the magazine well while a magazine is in your left hand, blue the charging handle, slide or pump when it must be worked. "
         "Seen through the hands and the gun. The balls stay as before. Command reloadhighlight on/off.");
    bool rb = g_btn_on.load();
    if (ImGui::Checkbox("Keep the automatic reload button (left Y reloads as in the game)##reloadbutton", &rb)) cmds += rb ? "reloadbutton on\n" : "reloadbutton off\n";
    hint("On (default, author 3 Nov): the reload button (left Y) reloads as in the game, its animation shown, with the manual reload on: whatever "
         "the hands were doing is dropped and the weapon counts as loaded from the game's clip. The game's own reloads at an empty clip stay held "
         "back (Debug > Gun: 'Empty magazine: the game does not reload by itself'). Off: the button is the game's and the manual reload does not follow it. Command reloadbutton on/off.");
    // F11 release clean-up, Gestures > Manual reload, no longer experimental (author 6 Nov 14:06): every other switch, slider and the
    // gun-in-hand panel in Debug, in the subsections that open on a click, closed at first. Debug starts with the hint style and reloadhlwhen.
    // Buttons (right B heal, the accidental-reload guard) shows with the manual reload off too, as these switches did; the rest needs it on.
    // Trimmed: reloadmagfist shows only with reloadmaglower off (it does nothing with it on), the handgun slide grip only with
    // reloadhandgrab on. reloadrockin left the menu on 3 Nov: reloadgamehold covers it (rock_deg). Every command and saved setting kept.
    auto sub = [](const char* title) {
        ImGui::Dummy(ImVec2{ 0.0f, 4.0f });
        if (!ImGui::CollapsingHeader(title)) return false;
        ImGui::Indent(16.0f);
        return true;
    };
    ImGui::Dummy(ImVec2{ 0.0f, 4.0f });
    if (!ImGui::CollapsingHeader("Debug##rldebug")) return;
    ImGui::Indent(16.0f);
    struct DebugIndent {
        ~DebugIndent() { ImGui::Unindent(16.0f); }
    } debug_indent;
    if (hlt) {
        int style = g_hint_glow.load() ? 0 : 1;
        ImGui::Indent();
        ImGui::TextUnformatted("Hint style:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Glow on the gun's own parts##reloadhintstyle", &style, 0)) cmds += "reloadhintstyle glow\n";
        ImGui::SameLine();
        if (ImGui::RadioButton("Boxes (before)##reloadhintstyle", &style, 1)) cmds += "reloadhintstyle boxes\n";
        hint("Glow (default, author 5 Nov): the part itself lights up on the gun's model, with a soft halo and a bright outline: orange the empty magazine "
             "(drop it), mint the magazine on your hip (take it), a yellow ghost of the magazine seated in the gun while one is in your hand (where it goes; "
             "brighter once close enough), blue the charging handle, slide, pump or lever (also, dimmer, where its blue ball showed: your hand near it, during a reload), "
             "blue a revolver's cylinder when it is run dry and while it is open. Where your hand covers it, faint moving lines show it through. "
             "A part the glow cannot draw keeps its box. Boxes: the boxes and the yellow and blue balls, as before. Command reloadhintstyle glow/boxes.");
        // reloadhlwhen (author 6 Nov 14:10), with the hint style: Manual reload > Debug, shown with the hints on.
        int hw = g_hl_when.load() >= 2 ? 2 : 0;
        ImGui::TextUnformatted("Highlights show:");
        ImGui::SameLine();
        if (ImGui::RadioButton("When needed##reloadhlwhen", &hw, 0)) cmds += "reloadhlwhen reload\n";
        ImGui::SameLine();
        if (ImGui::RadioButton("Always (before)##reloadhlwhen", &hw, 2)) cmds += "reloadhlwhen always\n";
        hint("When needed (default, author 6 Nov): the gun's parts glow (or their boxes and the handle's blue ball show) only when it cannot fire "
             "without you: run out of rounds, a reload begun (B dropped the magazine, a magazine, shell or round taken from the hip, a feed cover, "
             "cylinder or action opened) until it is ready again, or an action to work before the next shot (a pump shotgun's pump, a bolt or lever "
             "after a shot, a handle to rack). Always: as before, also the handle, dimmer, whenever your free hand is near it (holding a machine gun "
             "by its fore-end while firing). Command reloadhlwhen reload/always.");
        ImGui::Unindent();
    }
    if (sub("Buttons##rlsub")) {
        bool bh = g_b_heal.load();
        if (ImGui::Checkbox("Right B: hold to heal, no weapon switch##bheal", &bh)) cmds += bh ? "bheal on\n" : "bheal off\n";
        hint("On (default, author 3 Nov): a quick right B switches no weapon (switch at your shoulders); with manual reload it still drops the "
             "magazine. Held past the quick press (Quick B press below, default 250 ms), right B heals as before. With the left grip it throws. "
             "Off: right B is the game's previous weapon as before (held: heal). Command bheal on/off.");
        bool bg = g_guard_on.load();
        if (ImGui::Checkbox("No accidental reload: hold the button during a manual step##reloadbuttonguard", &bg)) cmds += bg ? "reloadbuttonguard on\n" : "reloadbuttonguard off\n";
        hint("On (default, author 3 Nov): while the left hand is at work (a magazine, shell or round in it or going in, the handle or pump held, the "
             "magazine out, the cylinder or cover open, and half a second after), a press of the reload button (left Y) reaches the game only once held "
             "for the time below. A thumb brushing it does nothing; hold it to reload. At any other time a press works at once. Off: every press goes "
             "to the game. Commands reloadbuttonguard on/off, reloadbuttonguardms N.");
        if (bg) {
            bool ga = g_guard_all.load();
            if (ImGui::Checkbox("... at all times with the gun in hand, not only during a manual step##reloadbuttonguardall", &ga))
                cmds += ga ? "reloadbuttonguardall on\n" : "reloadbuttonguardall off\n";
            hint("On (default, 4 Nov): whenever a gun of the manual reload is in your hand, a short press of the reload button (left Y) does nothing; "
                 "hold it for the time below to reload, or to pick something up. The author's log of 4 Nov had 12 presses in 80 s outside any manual step, "
                 "each one playing the game's full reload. Off: only during a manual step, as on 3 Nov. Command reloadbuttonguardall on/off.");
            int gm = g_guard_ms.load();
            if (ImGui::SliderInt("During a manual step: hold the reload button at least (ms)##reloadbuttonguardms", &gm, 150, 1500)) cmds += fmt::format("reloadbuttonguardms {}\n", gm);
        }
        int tap = g_tap_ms.load();
        if (ImGui::SliderInt("Quick B press: shorter than (ms)##reloadtap", &tap, 100, 600)) cmds += fmt::format("reloadtap {}\n", tap);
        hint("Released sooner than this, right B drops the magazine; held longer, it is the game's B (heal) from that moment.");
        if (bh) {
            int hm = g_heal_ms.load();
            if (ImGui::SliderInt("Heal: hold the game's button at least (ms)##bhealms", &hm, 300, 2000)) cmds += fmt::format("bhealms {}\n", hm);
            hint("A held right B is sent to the game for at least this long, so the game always sees a hold (heal) and never a tap (previous weapon). "
                 "Raise it if a short hold still switches weapon. Command bhealms N.");
        }
        int k = g_key.load();
        if (ImGui::RadioButton("Game's reload: keyboard R##reloadkeyr", k == 0)) cmds += "reloadkey r\n";
        ImGui::SameLine();
        if (ImGui::RadioButton("pad X##reloadkeyx", k == 1)) cmds += "reloadkey x\n";
        hint("How the mod starts the game's own reload at the drop (the game counts the ammo). R (default) cannot pick anything up; X is the pad's reload, but "
             "also its interact button.");
        ImGui::Unindent(16.0f);
    }
    if (!v) {
        ImGui::TextDisabled("The rest shows with the manual reload on.");
        return;
    }
    const bool listed = sig != 0 && is_listed(sig);
    // manualreload52: the gun in hand, each part's data: correct (built in, saved or learnt), unknown (guessed or not measured yet) or a
    // problem (none, or marked wrong); "Something wrong" marks it and, where the game's reload shows it, learns it again.
    ImGui::Dummy(ImVec2{ 0.0f, 4.0f });
    if (sig != 0 && ImGui::CollapsingHeader("The gun in hand: what is known##rlstatus", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(16.0f);
        const Builtin* bt  = builtin(sig);
        const Other    oth = bt ? Other{} : other_gun(sig);
        const fc5::weapons::Info* wi = fc5::gameweapon::info(sig);
        const char* name = bt ? bt->name : oth.ok ? oth.name : wi && wi->name ? wi->name : "a gun the mod does not know";
        const int   kind = bt ? bt->kind : kMagFed;
        const bool  magfed = kind == kMagFed || kind == kBelt, tubek = kind == kTube, revk = kind == kRevolver;
        const char* kname  = !bt && !oth.ok ? "no manual reload" : tubek ? (bt->lever_deg != 0.0f ? "lever shotgun" : "pump shotgun") : revk ? "revolver or break action"
                           : kind == kBelt ? "belt-fed" : (bt && bt->pistol) || oth.pistol ? "handgun" : "magazine gun";
        ImGui::Text("Held gun: %s (id %u), %s%s. Clip %d, reserve %d.", name, sig, kname, listed ? "" : " (not reloaded by hand now)", g_clip_now.load(), g_reserve_now.load());
        uint32_t prob = 0, use = 0;
        {
            std::lock_guard _{ g_hold_mtx };
            if (const auto i = g_problem.find(sig); i != g_problem.end()) prob = i->second;
            if (const auto i = g_learnt_use.find(sig); i != g_learnt_use.end()) use = i->second;
        }
        struct Row { int st; std::string txt; bool learn; };   // st 0 correct, 1 unknown, 2 problem
        std::array<Row, kItCount> r{};
        for (auto& x : r) x = Row{ -1, {}, false };
        r[kItData] = bt ? Row{ 0, "built in", false } : oth.ok ? Row{ 1, "guessed from the game's class of gun", false } : Row{ 2, "none", false };
        HlBox hb{};
        if (magfed) {
            r[kItMag] = mag_learnt(sig) ? Row{ 0, "learnt from the game's reload", true } : bt ? Row{ 0, "built in", true } : Row{ 1, "learnt at its first game reload", true };
            glm::vec3 t{};
            const bool hl = hold_mag(sig, t);
            r[kItHold] = (use & (1u << kItHold)) && hl ? Row{ 0, "the game's own hold, learnt again", true }
                       : g_mag_palm.load() ? (highlight(sig, kHlMag, hb) ? Row{ 0, "in the fist, placed from the magazine's model", true }
                                                                        : Row{ 1, "the magazine's model not measured yet (hold the gun still a moment)", true })
                       : g_game_hold.load() && hl ? Row{ 0, "the game's own hold, learnt", true } : Row{ 1, "a fixed place in the hand", true };
        } else {
            r[kItMag]  = bt && bt->round != 0 ? Row{ 0, tubek ? "the shell, built in" : "the round, built in", false } : Row{ 1, "no round model known", false };
            r[kItHold] = Row{ 0, g_round_pinch.load() ? "pinched by the thumb and index" : "in the fist", false };
        }
        r[kItWell] = highlight(sig, kHlWell, hb) || geom(sig).measured ? Row{ 0, "measured on the model", false } : Row{ 1, "measured once the gun is held still", false };
        HandleParts hp{};
        const bool norack = (bt && bt->norack) || oth.norack;
        if (revk || norack) {
            r[kItHandle] = Row{ 0, "nothing to rack", false };
        } else {
            const bool hk = handle_parts(sig, hp);
            r[kItHandle]  = hk ? Row{ 0, hp.saved ? "saved" : "built in", false } : Row{ 1, "learnt at a game reload from empty", false };
            bool learnt = false, builtin_g = false;
            {
                std::lock_guard _{ g_hold_mtx };
                learnt = g_grab_hold.contains(sig);
            }
            for (const auto& b : kGrabBuiltin) builtin_g = builtin_g || b.sig == sig;
            r[kItGrab] = learnt ? Row{ 0, "learnt from the game's reload", true } : builtin_g ? Row{ 0, "built in", true }
                       : tubek ? Row{ 0, "the pump in the fist", false }
                       : (bt && bt->pistol) || oth.pistol ? Row{ 1, "a handgun's slide grip, built in (reloadslidegrab), not yet seen in the headset", true }
                                                          : Row{ 1, "the AR-C's grip, borrowed", true };
        }
        {
            bool borrowed = false;
            const SeqSnd* sq = seq_for(sig, &borrowed);
            bool lrn = false, pk = false;
            {
                std::lock_guard _{ g_pick_mtx };
                lrn = g_snd_learnt.contains(sig);
                pk  = g_snd_pick.contains(sig);
            }
            const WeaponSnd* ws = weapon_snd(sig);
            r[kItSound] = (use & (1u << kItSound)) && lrn ? Row{ 0, "learnt again from the game's reload", magfed }
                        : pk ? Row{ 0, "picked in Sounds below", magfed }
                        : sq && !borrowed ? Row{ 0, "the game's own sequence, built in", magfed }
                        : sq ? Row{ 1, "the 1911's, borrowed", magfed }
                        : ws ? Row{ 0, "the game's sounds, built in", magfed }
                        : lrn ? Row{ 0, "learnt from the game's reload", magfed } : Row{ 2, "none: silent", magfed };
        }
        {
            uint32_t c = 0, b = 0, pb = 0;
            r[kItRound] = norack || revk ? Row{ 0, "nothing ejected by hand", false }
                        : round_part(sig, c, b, pb) ? Row{ 0, "built in", false } : Row{ 1, "none drawn", false };
        }
        std::array<std::string, kItCount> at;
        std::string msg;
        {
            std::lock_guard _{ g_relearn_mtx };
            if (g_relearn_sig.load() == sig) at = g_relearnt_at;
            msg = g_relearn_msg;
        }
        static const ImVec4 kCol[3]{ ImVec4{ 0.45f, 0.85f, 0.45f, 1.0f }, ImVec4{ 0.95f, 0.8f, 0.3f, 1.0f }, ImVec4{ 1.0f, 0.4f, 0.35f, 1.0f } };
        static const char*  kSt[3]{ "correct", "unknown", "problem" };
        for (int i = 0; i < kItCount; ++i) {
            if (r[size_t(i)].st < 0) continue;
            const bool marked = prob & (1u << i);
            const int  st     = marked ? 2 : r[size_t(i)].st;
            ImGui::TextColored(kCol[st], "%s: %s", kItName[i], kSt[st]);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s%s%s%s)", r[size_t(i)].txt.c_str(), marked ? "; you marked it wrong" : "",
                                at[size_t(i)].empty() ? "" : "; learnt again at ", at[size_t(i)].c_str());
            ImGui::SameLine();
            auto set_mask = [&](std::map<uint32_t, uint32_t>& mp, const char* c, int bit, bool on) {
                std::lock_guard _{ g_hold_mtx };
                uint32_t& mv = mp[sig];
                mv          = on ? (mv | (1u << bit)) : (mv & ~(1u << bit));
                if (mv == 0) mp.erase(sig);
                cmds += mask_line(c, mp) + "\n";
            };
            if (marked) {
                if (ImGui::SmallButton(fmt::format("It's right now##rlok{}", i).c_str())) {
                    set_mask(g_problem, "reloadproblem", i, false);
                    // manualreload55: and no more learning of it
                    if (g_relearn_sig.load() == sig) {
                        g_relearn_items &= ~(1u << i);
                        if (i == kItGrab && g_empty_sig.load() == sig) g_empty_sig = 0;
                    }
                }
                continue;
            }
            if (!ImGui::SmallButton(fmt::format("Something wrong##rlfix{}", i).c_str())) continue;
            set_mask(g_problem, "reloadproblem", i, true);
            if (!r[size_t(i)].learn) {
                relearn_say(fmt::format("weapon {}: {} marked wrong (saved in the settings for the next fix; the game's reload does not show it)", sig, kItName[i]));
                continue;
            }
            // Forget what was learnt for this part, then learn it again from the game's reload of this gun.
            g_relearn_sig = sig;
            g_relearn_items |= 1u << i;
            if (i == kItSound) {
                {
                    std::lock_guard _{ g_pick_mtx };
                    g_snd_learnt.erase(sig);
                    cmds += learnt_line() + "\n";
                }
                fc5::gamesound::record(true);
                set_mask(g_learnt_use, "reloadlearntuse", kItSound, true);
            } else if (i == kItHold) {
                {
                    std::lock_guard _{ g_hold_mtx };
                    g_hold_mag.erase(sig);
                    cmds += hold_line() + "\n";
                }
                set_mask(g_learnt_use, "reloadlearntuse", kItHold, true);
            } else if (i == kItMag) {
                std::lock_guard _{ g_list_mtx };
                g_parts.erase(sig);
                cmds += parts_line() + "\n";
            } else if (i == kItGrab) {
                {
                    std::lock_guard _{ g_hold_mtx };
                    g_grab_hold.erase(sig);
                    cmds += grab_line() + "\n";
                }
                g_empty_sig = sig;
                relearn_say(fmt::format("weapon {}: fire it empty (into the ground, no one in the line of fire): the game's own reload from empty shows its hand "
                                        "on the handle (reloadkeepone is lifted for this gun until then)", sig));
                continue;
            }
            g_relearn_req = 1;
            relearn_say(fmt::format("weapon {}: {} forgotten: the game's reload of this gun will start (fire one shot first if the clip is full)", sig, kItName[i]));
        }
        if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
        hint("Each part of the reload for the gun in your hands: correct when the mod has it (built in, saved or learnt from the game's own reload), "
             "unknown when it is guessed or measured later, a problem when there is none or you marked it. 'Something wrong' marks it (saved, so the "
             "next fix knows) and, for the magazine, the hand holding it, the hand on the handle and the sounds, forgets what was learnt and learns it "
             "again from the game's own reload of this gun: the reload starts by itself (after one shot if the clip is full); for the hand on the handle "
             "fire the gun empty. Commands reloadproblem, reloadlearntuse.");
        ImGui::Unindent(16.0f);
    }
    if (sig != 0) {
        bool b = is_listed(sig);
        if (ImGui::Checkbox(fmt::format("The weapon in your hands (id {}) reloads by hand###reloadhere", sig).c_str(), &b)) {
            std::set<uint32_t> s;
            {
                std::lock_guard _{ g_list_mtx };
                s = g_weapons;
            }
            if (b) s.insert(sig);
            else s.erase(sig);
            std::string line = "reloadweapons v2";   // manualreload20: a list made with the pump shotguns listed by default
            for (const uint32_t w : s) line += " " + std::to_string(w);
            if (s.empty()) line += " none";
            cmds += line + "\n";
        }
        if (const Builtin* bw = builtin(sig); bw && wtype(*bw) >= 0 && g_type_on[wtype(*bw)].load())
            hint(fmt::format("On through its kind's switch ({}, below): untick that switch to choose this weapon here.", kTypeCmd[wtype(*bw)]).c_str());
        Revolver rs{};
        // reloadlauncherfill (a one-shot launcher: the M79, the RPG-7, the RAT4)
        if (revolver_parts(sig, rs) && rs.rigid_rounds) {
            bool lf = g_launcher_fill.load();
            if (ImGui::Checkbox("Launcher taken in hand empty: its round shown at the hip at once##reloadlauncherfill", &lf)) cmds += lf ? "reloadlauncherfill on\n" : "reloadlauncherfill off\n";
            bool hl = g_hip_loaded.load();
            if (ImGui::Checkbox("Spare round at the hip with the launcher loaded too##reloadhiploaded", &hl)) cmds += hl ? "reloadhiploaded on\n" : "reloadhiploaded off\n";
            hint("On: picked up loaded, or just fired and not yet opened, a launcher with rounds in reserve shows the next one at your left hip. Command reloadhiploaded on/off.");
            if (rs.brk && !rs.door) {
                bool gf = g_grenade_fill.load();
                if (ImGui::Checkbox("Grenade launcher: no game reload after a shot##reloadgrenadefill", &gf)) cmds += gf ? "reloadgrenadefill on\n" : "reloadgrenadefill off\n";
                hint("On: after a shot the game plays no reload or hand animation; open it, take a grenade from your hip and load it by hand. Command reloadgrenadefill on/off.");
                bool gs = g_grenade_seat.load();
                if (ImGui::Checkbox("Grenade launcher: grenade seated in the barrel##reloadgrenadeseat", &gs)) cmds += gs ? "reloadgrenadeseat on\n" : "reloadgrenadeseat off\n";
                hint("On: the grenade on the gun sits in the bore, its base at the breech (measured on the model). Takes effect at the next pick-up. Command reloadgrenadeseat on/off.");
            }
            if (rs.door) {
                bool sg = g_same_gun.load();
                if (ImGui::Checkbox("RAT4: its changing ids are one weapon##reloadsamegun", &sg)) cmds += sg ? "reloadsamegun on\n" : "reloadsamegun off\n";
                hint("On: the RAT4 seen under another id (28497, 80077, 69646) keeps its reload instead of starting again loaded. Command reloadsamegun on/off.");
            }
        }
        // reloadshellhip (a double barrel: the Sledge, the sawed-off)
        if (revolver_parts(sig, rs) && rs.brk && !rs.rigid_rounds) {
            bool sh = g_shell_hip.load();
            if (ImGui::Checkbox("Double barrel: spare shell centred on the hip zone##reloadshellhip", &sh)) cmds += sh ? "reloadshellhip on\n" : "reloadshellhip off\n";
            hint("On: the shell at your left hip sits in the middle of the place the left grip takes it from. Off: as before, its nose 3 cm above that "
                 "place and the shell behind it. Command reloadshellhip on/off.");
        }
        if (revolver_parts(sig, rs) && rs.noopen)
            hint(fmt::format("The {} (id {}): after a shot, take a rocket from your left hip (it stands up there), hold it pointing the way the launcher "
                             "points, bring its tail to the front of the tube (the yellow ball) and push it in backward until it clicks. Nothing to open; "
                             "a quick right B does nothing. It fires only with a rocket in.", builtin(sig)->name, sig).c_str());
        else if (revolver_parts(sig, rs) && rs.door)
        {
            hint((g_rat_slide.load()
                      ? fmt::format("The {} (id {}): a quick right B opens the back. Take a missile from your left hip, nose first, bring its nose to the open back and push it "
                                    "forward into the tube: a little way in, it slides home. A quick right B, a flick or the left grip at the back closes it. It fires only closed, loaded.", builtin(sig)->name, sig)
                      : fmt::format("The {} (id {}): a quick right B opens the back. Take a missile from your left hip and bring it to the open back, where it goes in. A quick right B or the left grip at the back closes it. It fires only closed, loaded.", builtin(sig)->name, sig))
                     .c_str());
            bool rs2 = g_rat_slide.load();
            if (ImGui::Checkbox("The missile goes in through the back, pushed by the hand##reloadratslide", &rs2)) cmds += rs2 ? "reloadratslide on\n" : "reloadratslide off\n";
        }
        else if (revolver_parts(sig, rs) && rs.rigid_rounds)
            hint(fmt::format("The {} (id {}): a quick right B breaks it open (the barrel tips down, the fired case is thrown out). Take a grenade from your "
                             "left hip and push it into the breech from behind (the yellow ball) until it clicks; a quick right B closes it. It fires only "
                             "closed, loaded.", builtin(sig)->name, sig).c_str());
        else if (const Builtin* bo = builtin(sig); bo && bo->bolt && g_type_on[kTypeBolt].load())
            hint(g_bolt_turn.load()
                     ? fmt::format("The {} (id {}): a bolt action. After every shot, work the bolt (a grip on the blue ball, either hand): raise the hand to turn it up, "
                                   "pull it back to its stop (the fired case flies out), push it forward, lower the hand to turn it down: locked, it fires. The magazine "
                                   "as other guns: a quick right B drops it, the next from your left hip.", bo->name, sig).c_str()
                     : fmt::format("The {} (id {}): a bolt action. After every shot, work the bolt with your left hand (left grip on the blue ball, pull back to "
                                   "its stop: the fired case flies out; let go: a round goes in). The magazine as other guns: a quick right B drops it, the next "
                                   "from your left hip.", bo->name, sig).c_str());
        else if (const Builtin* br = builtin(sig); br && br->kind == kRevolver)
            hint(fmt::format("The {} (id {}): a revolver. A quick right B swings the cylinder out to the left; tip the muzzle up and the fired cases drop out. "
                             "Take a round from your left hip and push it into the cylinder from behind (the yellow ball), a click per round, into the next "
                             "empty chamber. A quick right B swings it back in. It fires only closed.", br->name, sig).c_str());
        else if (const Builtin* bb = builtin(sig); bb && bb->kind == kBelt) {
            hint((g_belt_grip.load()
                     ? fmt::format("The {} (id {}): belt-fed. 1. A quick right B drops the belt and lifts the feed cover. 2. Take a belt from your left hip and "
                                   "push it up into the tray (the yellow ball) until it clicks. 3. Let go; the cover glows blue: left grip on it and push it down to shut it "
                                   "(reloadcoverpush off: left grip near the tray shuts it at once). 4. Left grip "
                                   "on the charging handle (the blue ball) and rack it. It fires only with the cover closed and the handle racked. "
                                   "Commands reloadbeltgrip on/off, reloadcoverpush on/off, reloadcoverdeg D (how far the cover lifts).", bb->name, sig)
                     : fmt::format("The {} (id {}): belt-fed. A quick right B lifts the feed cover (the belt in the gun comes out). Take a belt from your left hip and "
                                   "push it up into the tray (the yellow ball) until it clicks, then a quick right B closes the cover. After running dry, rack the "
                                   "charging handle (the blue ball). It fires only with the cover closed. Command reloadcoverdeg D sets how far the cover lifts.", bb->name, sig))
                 .c_str());
            {
                bool bgp = g_belt_grip.load();
                if (ImGui::Checkbox("Belt-fed: left grip closes the cover, then rack the handle##reloadbeltgrip", &bgp)) cmds += bgp ? "reloadbeltgrip on\n" : "reloadbeltgrip off\n";
                bool cpp = g_cover_push.load();
                if (ImGui::Checkbox("Belt-fed: the cover is pushed down by the hand to shut it##reloadcoverpush", &cpp)) cmds += cpp ? "reloadcoverpush on\n" : "reloadcoverpush off\n";
            }
        }
        else if (const Builtin* bn = builtin(sig); bn && bn->norack)
            hint(fmt::format("The {} (id {}): a quick right B drops its canister; take the next from your left hip and push it into place until it clicks. "
                             "Nothing to rack.", bn->name, sig).c_str());
        else if (const Builtin* bt = builtin(sig); bt && bt->kind == kTube)
            hint(fmt::format("The {} (id {}): a pump shotgun or lever action. A quick right B opens it, a second one closes it. No magazine: take a shell from your left hip and push it up into the loading port under the gun "
                             "(the yellow ball), a click per shell. Work the pump with your left hand (left grip on it, pull back to the stop, push forward) to "
                             "chamber a shell, and after every shot. Its shell and pump are built in, from the game's model.", bt->name, sig).c_str());
        else if (const Builtin* bt2 = builtin(sig))
            hint(fmt::format("The {} (id {}): its magazine, top round and {} are built in, from the game's model. The mod knows weapons by an id, not by name. The AR-C "
                             "(57819) is the one tuned; the pistols (1911, P226, M9, D50, 6P9, A99) are listed by default.", bt2->name, sig, bt2->pistol ? "slide" : "handle").c_str());
        else if (const Other ot = other_gun(sig); ot.ok)
            hint(fmt::format("{} (id {}): no built-in parts. With reloadother (Weapon kinds, below) it reloads by hand as a magazine gun: a quick right B drops "
                             "the magazine{}, the game's own reload runs hidden and its magazine is learnt from it (keep the gun still for 3 s the first time; "
                             "until then the mod's drawn magazine stands in). Take the next from your left hip and push it in until it clicks.{}",
                             ot.name, sig, ot.norack ? " (an energy weapon's cell, a launcher's round, a canister)" : "",
                             ot.norack ? " Nothing to rack." : " After running dry, rack the handle or slide (learnt from the game's reload from empty, else the blue ball).").c_str());
        else
            hint("The mod knows weapons by an id, not by name. Magazine-fed guns only (rifles, SMGs, pistols); this one has no built-in parts: its magazine is learnt at "
                 "the game's first reload of it (keep the gun still for 3 s).");
    } else {
        ImGui::TextDisabled("No weapon in your hands now.");
    }
    ImGui::Text("Weapons: %s", list_text().empty() ? "none" : list_text().c_str());
    {
        std::lock_guard _{ g_mtx };
        const std::string s = state_text();
        ImGui::Text("Now: %s", s.c_str());
    }
    // allreload: each kind of weapon on its own switch, all on at first (author 3 Nov 14:44).
    if (sub("Weapon kinds (each on its own switch)##rlsub")) {
        for (int t = 0; t < kTypeCount; ++t) {
            bool on_t = g_type_on[t].load();
            if (ImGui::Checkbox(fmt::format("{}##{}", kTypeName[t], kTypeCmd[t]).c_str(), &on_t)) cmds += fmt::format("{} {}\n", kTypeCmd[t], on_t ? "on" : "off");
        }
        hint("On (default, 3 Nov): every weapon of that kind reloads by hand, ticked above or not. Off: only the ticked ones. The flamethrower: a quick "
             "B drops its canister, the next from the hip clicks in. The M79: a quick B breaks it open, a grenade from the hip goes into the breech, a "
             "quick B closes it. The RPG-7: a rocket from the hip goes in at the muzzle, tail first. Lever actions: shells from the hip, a quick B works "
             "the lever. Double barrels: a quick B opens them, a shell per barrel, a quick B closes. The M60: a quick B lifts the cover, a belt from the "
             "hip, a quick B closes; the M249: its box swapped. Bolt actions (the .308): the bolt worked with the left hand after every shot. Magazine "
             "guns, pump shotguns and revolvers as before, every one of them. Every other gun (3 Nov evening): the M712, the Mark IV, the Lost on Mars "
             "weapons (an energy cell swapped), the arcade map's unnamed guns: a magazine learnt at its first reload. Commands reloadflame, reloadgl, "
             "reloadrocket, reloadlever, reloadbreak, reloadbelt, reloadbolt, reloadmag, reloadpump, reloadrevolver, reloadother on/off.");
        ImGui::Unindent(16.0f);
    }
    if (sub("Hip and left hand##rlsub")) {
        bool hs = g_hand_snap.load();
        if (ImGui::Checkbox("Hand stays on the magazine and the handle##reloadhandsnap", &hs)) cmds += hs ? "reloadhandsnap on\n" : "reloadhandsnap off\n";
        hint("On (default, author 3 Nov): while a magazine slides into the well your drawn left hand holds it there, on the well's line; while you pull "
             "the charging handle, slide or pump, the hand stays on it and moves exactly with it. It eases in and out in about a tenth of a second. "
             "Your real hand may be a few centimetres off; the drawn one is not. Off: the hand is drawn at your controller. Command reloadhandsnap on/off.");
        bool rp = g_round_pinch.load();
        if (ImGui::Checkbox("Rounds and shells between thumb and index##reloadroundpinch", &rp)) cmds += rp ? "reloadroundpinch on\n" : "reloadroundpinch off\n";
        hint("On (default, author 3 Nov): a revolver round or a shotgun shell taken from the hip is pinched between your thumb and index tips, "
             "its base at the pinch, a few centimetres in front of your fist. Off: held in the fist. Command reloadroundpinch on/off.");
        bool mpalm = g_mag_palm.load();
        if (ImGui::Checkbox("Magazine in the whole hand##reloadmagpalm", &mpalm)) cmds += mpalm ? "reloadmagpalm on\n" : "reloadmagpalm off\n";
        hint("On (default, author 3 Nov): a magazine taken from the hip sits in your fist at the middle of its length (7 cm below its top at most), "
             "measured on the gun's own magazine. Off: the hand offset below, or the game's own hold (at the magazine's bottom) with the game hold on. "
             "Command reloadmagpalm on/off.");
        bool ml = g_mag_lower.load();
        if (ImGui::Checkbox("Magazine held in the fist, round its lower half##reloadmaglower", &ml)) cmds += ml ? "reloadmaglower on\n" : "reloadmaglower off\n";
        hint("On (default): a magazine gun's magazine sits in the fist, round the middle of its lower half, the fingers closed round it. Off: as the game's own hand holds it, or from under. Boxes, belts, shells and rounds "
             "keep their own holds. Command reloadmaglower on/off.");
        if (mpalm && !g_mag_lower.load()) {   // (it acts only there: with reloadmaglower on a handgun's magazine sits round its lower half)
            bool mfist = g_mag_fist.load();
            if (ImGui::Checkbox("A handgun's magazine in the middle of the fist##reloadmagfist", &mfist)) cmds += mfist ? "reloadmagfist on\n" : "reloadmagfist off\n";
            hint("On (default, 4 Nov, not yet seen in the headset): a handgun's magazine sits in the middle of your fist sideways. Your fist's middle is beside "
                 "the controller, toward the other hand, as far as the gun's grip is beside the other controller (4 to 6 cm). Off: on the controller's line, "
                 "as before, where it sat beside the fist (the P08's along the outside of the hand). Command reloadmagfist on/off.");
        }
        bool hgm = g_hand_game.load();
        if (ImGui::Checkbox("A handgun's hand and magazine as in the game's own reload##reloadhandgame", &hgm)) cmds += hgm ? "reloadhandgame on\n" : "reloadhandgame off\n";
        hint("On (default, author 4 Nov): recorded from the game's own reload, gun by gun: the magazine sits in your hand as the game holds it, your hand goes on the slide as the game's does, "
             "and an empty gun's slide stays back until you rack it. Off: the magazine in the fist and the slide grip below. Command reloadhandgame on/off.");
        bool hg = g_hand_grab.load();
        if (ImGui::Checkbox("Hand grabs the handle, slide or pump as in the game's reload##reloadhandgrab", &hg)) cmds += hg ? "reloadhandgrab on\n" : "reloadhandgrab off\n";
        hint("On (default, author 3 Nov): with the box above, wherever you take the charging handle, slide or pump, the drawn hand goes to the same place "
             "on it: where the game's own hand holds it in its reload (learnt from a game reload from empty, built in for the AR-C, a handgun's slide "
             "grip below for a handgun, else the AR-C's grip on this gun's handle; a pump in the fist). Off: the hand stays where you took it. Command reloadhandgrab on/off.");
        if (hg) {   // (reloadhandgrab's grip for a handgun: nothing to set with it off)
            float sg[3]{ g_slide_grab[0].load(), g_slide_grab[1].load(), g_slide_grab[2].load() };
            if (ImGui::DragFloat3("A handgun's slide grip (m)##reloadslidegrab", sg, 0.001f, -0.12f, 0.12f, "%.3f"))
                cmds += fmt::format("reloadslidegrab {:.3f} {:.3f} {:.3f}\n", sg[0], sg[1], sg[2]);
            ImGui::SameLine();
            if (ImGui::SmallButton("Default##reloadslidegrab")) cmds += "reloadslidegrab default\n";
            hint("A handgun's slide with no grip learnt from the game or built in (4 Nov, not yet seen in the headset): where its rear is from your "
                 "left controller, in the gun's axes (x right, y up, z back, m). The default (0.044 -0.005 -0.010) puts the fist round the rear of "
                 "the slide from above; until now pistols borrowed the AR-C's grip, which left the hand open in the air above and behind the slide. "
                 "Raise the second number to lower the hand, lower the third to move it back. Command reloadslidegrab x y z, or reloadslidegrab default.");
        }
        float z = g_pouch_zone.load();
        if (ImGui::SliderFloat("Hip pouch zone (m)##reloadpouchzone", &z, 0.06f, 0.30f, "%.2f")) cmds += fmt::format("reloadpouchzone {:.2f}\n", z);
        float p[3]{ g_pouch[0].load(), g_pouch[1].load(), g_pouch[2].load() };
        if (ImGui::SliderFloat3("Pouch: right, down, forward from the eyes (m)##reloadpouch", p, -0.8f, 0.9f, "%.2f")) cmds += fmt::format("reloadpouch {:.2f} {:.2f} {:.2f}\n", p[0], p[1], p[2]);
        hint("Where the magazine waits (default the front of the left hip: 0.22 m to the left, 0.60 m below the eyes). It turns with your body, not your head.");
        bool gh = g_game_hold.load();
        if (ImGui::Checkbox("Follow the game's reload: its hand's hold, the AK's magazine rocked in##reloadgamehold", &gh)) cmds += gh ? "reloadgamehold on\n" : "reloadgamehold off\n";
        {
            glm::vec3 t{ 0.0f };
            if (sig != 0 && hold_mag(sig, t)) ImGui::Text("This weapon, learnt from the game's reload: %.3f %.3f %.3f", t.x, t.y, t.z);
            else ImGui::TextUnformatted("This weapon: not learnt yet (the game's own reload, seen once, learns it)");
        }
        hint("On by default (3 Nov). The hold below is used only with 'Magazine in the whole hand' off (that switch places the magazine from its model). "
             "Off: the magazine's top sits at the place below for every weapon and goes straight in. On: for each weapon whose own game "
             "reload has been seen (a game reload with this gun in hand, once), the magazine sits in your left hand where the game's hand holds its "
             "spare one; and an AK's magazine goes in as the game puts it in, its front hooked into the well first, tilted 20 degrees, rocked up "
             "into line as you push it home (before 3 Nov a separate 'AK magazine rocked in' slider).");
        // reloadhandmag: the place for every weapon (gamehold off), or for those whose game reload was not seen yet.
        float h[3]{ g_hand_off[0].load(), g_hand_off[1].load(), g_hand_off[2].load() };
        if (ImGui::SliderFloat3("Magazine's top in the left hand: right, up, back (m)##reloadhandmag", h, -0.2f, 0.2f, "%.3f"))
            cmds += fmt::format("reloadhandmag {:.3f} {:.3f} {:.3f}\n", h[0], h[1], h[2]);
        hint("Where the magazine's top sits in your left hand when it does not follow the game's hand (the box above off, or a weapon whose game reload was not seen yet).");
        ImGui::Unindent(16.0f);
    }
    if (sub("Gun: magazine, handle, pump##rlsub")) {
        bool ns = g_no_stuck.load();
        if (ImGui::Checkbox("Never left unable to fire##reloadnostuck", &ns)) cmds += ns ? "reloadnostuck on\n" : "reloadnostuck off\n";
        hint("On (default, author 3 Nov): a weapon the manual reload holds as empty (a shotgun, a revolver, a double barrel, the M79, the RPG-7, a handle "
             "or bolt to work) while the game still has rounds in its clip is loaded from the game's clip: at once if nothing can go in by hand (its "
             "well or muzzle not measured yet), else after 6 s with nothing done towards the reload. Off: it stays held until loaded by hand. "
             "Command reloadnostuck on/off.");
        bool nsw = g_no_switch.load();
        if (ImGui::Checkbox("Out of ammo: the game does not switch weapon by itself (pump and lever shotguns)##noautoswitch", &nsw)) cmds += nsw ? "noautoswitch on\n" : "noautoswitch off\n";
        hint("On (default, author 7 Nov): a pump or lever shotgun with no spare ammo left keeps its last shell unfired (the wrist shows 0), so the game "
             "never sees it empty and does not put another weapon in your hands; pick up ammo and it fires again. Off: the last shell fires and the game "
             "switches weapon by itself. (Magazine guns, revolvers and double barrels never let the game's clip reach 0: every round fires and the "
             "game reloads nothing by itself, since 9 Nov.) Command noautoswitch on/off.");
        {
            bool ao = g_act_on.load();
            if (ImGui::Checkbox("Lever and pump: stay open once thrown, shells go in from near the port##reloadactionopen", &ao)) cmds += ao ? "reloadactionopen on\n" : "reloadactionopen off\n";
            bool rl = g_rack_loss.load();
            if (ImGui::Checkbox("A rack with a round in costs a bullet from the magazine##reloadrackloss", &rl)) cmds += rl ? "reloadrackloss on\n" : "reloadrackloss off\n";
            bool hh = g_hand_hold.load();
            bool fl = g_flick.load();
            if (ImGui::Checkbox("A flick of the gun closes what B opened (a revolver: sideways)##reloadflick", &fl)) cmds += fl ? "reloadflick on\n" : "reloadflick off\n";
            if (ImGui::Checkbox("Pump, lever, bolt: the game's cycle after a shot does not move the weapon hand##reloadhandhold", &hh)) cmds += hh ? "reloadhandhold on\n" : "reloadhandhold off\n";
            bool bt2 = g_bolt_turn.load();
            if (ImGui::Checkbox("Bolt actions: the bolt turned up, pulled back, pushed forward, turned down##reloadboltturn", &bt2)) cmds += bt2 ? "reloadboltturn on\n" : "reloadboltturn off\n";
            hint("On (default, author 6 Nov): grip the bolt (either hand), raise the hand to turn it up, pull it back to its stop (the fired case flies out), push it forward "
                 "(a round goes in), lower the hand to turn it down: locked, it fires. The bolt follows the hand at every step; let go anywhere and it stays there. "
                 "Off: pulled straight back and let go. Commands reloadboltturn on/off, reloadboltdeg N, reloadboltlift M.");
            bool bp2 = g_bolt_parts.load();
            if (ImGui::Checkbox("Bolt actions: the bolt stop and bolt cover kept still after a shot##reloadboltparts", &bp2)) cmds += bp2 ? "reloadboltparts on\n" : "reloadboltparts off\n";
            hint("On (default, author 6 Nov 13:55, 'the bolt action had some slight movements'): the parts beside the bolt (the .308's bolt stop, the HTI .50's "
                 "bolt cover) stay where they sit at rest, as the bolt does; the game's own cycle after each shot no longer moves them. Off: they move with it. "
                 "Command reloadboltparts on/off.");
            if (bt2) {
                float bd = g_bolt_deg.load();
                if (ImGui::SliderFloat("Bolt turn (deg; negative turns it the other way)##reloadboltdeg", &bd, -120.0f, 120.0f, "%.0f")) cmds += fmt::format("reloadboltdeg {:.0f}\n", bd);
                float bl = g_bolt_lift.load() * 100.0f;
                if (ImGui::SliderFloat("Hand rise to turn it fully up (cm)##reloadboltlift", &bl, 1.0f, 12.0f, "%.1f")) cmds += fmt::format("reloadboltlift {:.3f}\n", bl / 100.0f);
            }
        }
        {
            bool lh = g_lever_hand_on.load();
            if (ImGui::Checkbox("Lever actions: work the lever with the right hand##reloadleverhand", &lh)) cmds += lh ? "reloadleverhand on\n" : "reloadleverhand off\n";
            hint("On (default): with the left grip holding the fore-end, drop the right hand about 5 cm and bring it back: the lever follows it. Only when the chamber needs it. Quick right B still works it. "
                 "Command reloadleverhand on/off.");
        }
        float tr = g_travel.load(), rk = g_rack_len.load();
        if (ImGui::SliderFloat("Magazine travel into the well (m)##reloadtravel", &tr, 0.02f, 0.10f, "%.3f")) cmds += fmt::format("reloadtravel {:.3f}\n", tr);
        if (ImGui::SliderFloat("Charging handle pull (m)##reloadrack", &rk, 0.02f, 0.12f, "%.3f")) cmds += fmt::format("reloadrack {:.3f}\n", rk);
        hint("A pistol's magazine goes 1.6 times the travel into its grip, and its slide comes back 0.75 times the pull (8 and 4.5 cm at the defaults).");
        bool ps = g_pump_shot.load();
        if (ImGui::Checkbox("Pump shotguns: work the pump after each shot##reloadpumpshot", &ps)) cmds += ps ? "reloadpumpshot on\n" : "reloadpumpshot off\n";
        hint("On (default): after each shot a pump shotgun (the M133s, when ticked) does not fire again until you pull its pump back to the stop (the fired "
             "shell flies out) and push it forward. The SPAS-12 fires again by itself, as in the game. Off: the next shell is chambered by itself; the pump is needed only after loading an empty gun. Shells "
             "always go in one at a time from the left hip.");
        bool pg = g_pump_grip.load();
        if (ImGui::Checkbox("Pump shotguns: take the pump anywhere on the fore-end, grip held or not##reloadpumpgrip", &pg)) cmds += pg ? "reloadpumpgrip on\n" : "reloadpumpgrip off\n";
        hint("On (default, author 7 Nov: the M133's pump did not work): the left grip on the fore-end takes the pump wherever you hold it along the gun, "
             "and if you keep holding the fore-end after a shot, just pull it back and push it forward, no new press. The stroke is your left hand "
             "coming back towards your right one, and while it moves the gun keeps its aim on your right hand (it no longer swings with the left). "
             "Off: as before (a new press near the pump's middle, pulled along the right controller). Command reloadpumpgrip on/off.");
        bool ch = g_casing_on.load();
        if (ImGui::Checkbox("One shell only: hide the game's casing when the hand ejects##reloadcasinghide", &ch)) cmds += ch ? "reloadcasinghide on\n" : "reloadcasinghide off\n";
        hint("On (default, author 3 Nov): with a pump or lever shotgun worked after each shot (above), or a bolt action, the game's own spent casing "
             "is hidden while it is in hand, so only the shell thrown out by your pump, lever or bolt is seen (the game throws one too as it cycles "
             "the gun by itself). Other casings are hidden too while that gun is in hand. Off: both. Command reloadcasinghide on/off.");
        bool rd = g_rev_dump.load();
        if (ImGui::Checkbox("Revolver: fired cases drop out as it opens##reloadrevdump", &rd)) cmds += rd ? "reloadrevdump on\n" : "reloadrevdump off\n";
        hint("On (default): the fired cases leave the cylinder as soon as it is swung open, so only the live rounds stay in it. Off: they stay until "
             "you tip the muzzle up (more than 37 degrees above level).");
        bool rsl = g_round_slide.load();
        if (ImGui::Checkbox("Revolver: a round near its chamber drops in by itself##reloadroundslide", &rsl)) cmds += rsl ? "reloadroundslide on\n" : "reloadroundslide off\n";
        bool sh = g_seat_hold_on.load();
        if (ImGui::Checkbox("No grip on the gun until a new press once a shell or magazine is in##reloadseathold", &sh)) cmds += sh ? "reloadseathold on\n" : "reloadseathold off\n";
        bool df = g_drop_fall.load();
        if (ImGui::Checkbox("The dropped magazine falls to the ground##reloaddrop", &df)) cmds += df ? "reloaddrop on\n" : "reloaddrop off\n";
        hint("On (default): a quick B lets the game's magazine fall from the gun, tumbling, to the ground, where it lies 20 s. Off: it vanishes.");
        float fl = g_drop_floor.load();
        if (ImGui::SliderFloat("Ground below the game's eyes (m)##reloaddropfloor", &fl, 0.5f, 2.2f, "%.2f")) cmds += fmt::format("reloaddropfloor {:.2f}\n", fl);
        hint("Where the dropped magazine lands: this far below the game's eyes when it was dropped (1.6 standing). Raise it if it sinks into the ground, lower it if it "
             "floats.");
        // The weapon in hand's magazine and "Learn it again" (the author uses it when the wrong part drops; manual reload thread 12:45).
        if (listed) {
            const std::string mt = mag_text(sig);
            ImGui::Text("This weapon's magazine: %s", mt.empty() ? "not learnt yet (drop one)" : mt.c_str());
            if (!mt.empty() && ImGui::Button("Learn this weapon's magazine again at the next drop##reloadmagforget")) {
                std::map<uint32_t, std::vector<MagPart>> mp;
                {
                    std::lock_guard _{ g_list_mtx };
                    mp = g_parts;
                }
                mp.erase(sig);
                std::string line = "reloadmagmesh";
                for (const auto& [w, vv] : mp) {
                    line += fmt::format(" {}:", w);
                    for (size_t i = 0; i < vv.size(); ++i) {
                        line += fmt::format("{}{}/", i ? "," : "", vv[i].count);
                        if (vv[i].prop) line += "p";
                        else if (vv[i].rigid) line += "r";
                        else
                            for (int b = 0; b < vv[i].nbones; ++b) line += fmt::format("{}{}", b ? "+" : "", vv[i].bones[b]);
                    }
                }
                if (mp.empty()) line += " none";
                cmds += line + "\n";
            }
        }
        ImGui::Unindent(16.0f);
    }
    if (sub("Sounds##rlsub")) {
        bool ws = g_wsnd.load();
        if (ImGui::Checkbox("The game's own reload sounds, per weapon##reloadweaponsounds", &ws)) cmds += ws ? "reloadweaponsounds on\n" : "reloadweaponsounds off\n";
        hint("On: the magazine (or shell, or round) going out and in and the bolt (or pump) use this weapon's own sounds from the game, taken from its "
             "reload on the arcade map. Which of its sounds is which is a guess until heard: below, play each and pick. On by default (3 Nov). Off: no reload sounds "
             "(the mod has no sounds of its own since 3 Nov).");
        bool sf = g_snd_fill.load();
        if (ImGui::Checkbox("A handgun with no sounds of its own uses the 1911's##reloadsoundfill", &sf)) cmds += sf ? "reloadsoundfill on\n" : "reloadsoundfill off\n";
        hint("On (default, author 3 Nov): a handgun with no game sounds of its own (the P08) plays the 1911's. A revolver or other gun with none "
             "(the .44 Magnum) stays silent until its sounds are learnt from the game. Off: such a handgun is silent. Command reloadsoundfill on/off.");
        // reloadsoundseq (manualreload42): the four sequences of the weapon in hand, each played, and the three cuts between them.
        {
            bool sq = g_seq_on.load();
            if (ImGui::Checkbox("The game's own sounds in four sequences (out, in, bolt back, bolt forward)##reloadsoundseq", &sq)) cmds += sq ? "reloadsoundseq on\n" : "reloadsoundseq off\n";
            hint("On (default, author 3 Nov): with the game's own sounds ticked above, each manual step plays its part of the game's own reload, "
                 "with the game's spacing, instead of one sound: magazine out, magazine in, bolt back (and the case), bolt forward. Built in for every "
                 "weapon of the sound run (a pump's or lever's back and forward, a shell in, a break action opened and closed). A pick below for this "
                 "weapon alone comes first; a pick for the whole category does not. Off: one sound per step as before. Command reloadsoundseq on/off.");
            bool          borrowed = false;
            const SeqSnd* sqs      = listed ? seq_for(sig, &borrowed) : nullptr;
            if (sq && sqs && borrowed) ImGui::TextDisabled("This handgun has no sequence of its own: the 1911's (reloadsoundfill). Cuts changed here change the 1911's.");
            if (sq && sqs) {
                static const char* kSeqName[4]{ "Magazine out", "Magazine in", "Bolt back", "Bolt forward" };
                for (int k = 0; k < 4; ++k) {
                    if (ImGui::Button(fmt::format("Play##reloadseqplay{}", k).c_str())) sound(k);
                    ImGui::SameLine();
                    ImGui::Text("%s: %s", kSeqName[k], seq_text(*sqs, k).c_str());
                }
                std::array<float, 5> c{};
                {
                    std::lock_guard _{ g_seq_mtx };
                    c = seq_cuts(*sqs);
                }
                std::string tl;
                for (int i = 0; i < sqs->n; ++i) tl += fmt::format("{}{:.2f}", i ? " " : "", sqs->t[i]);
                ImGui::TextDisabled("The sequence's sounds (%s): at %s s", sqs->name, tl.c_str());
                static const char* kCutName[5]{ "Start after (s)##rlseqc0", "Out | in (s)##rlseqc1", "In | bolt back (s)##rlseqc2", "Back | forward (s)##rlseqc3", "End at (s)##rlseqc4" };
                bool changed = false;
                for (int i = 0; i < 5; ++i) changed |= ImGui::SliderFloat(kCutName[i], &c[size_t(i)], 0.0f, sqs->t[sqs->n - 1] + 0.1f, "%.2f");
                if (ImGui::Button("Built-in cuts for this weapon##rlseqreset")) {
                    std::lock_guard _{ g_seq_mtx };
                    auto mp = g_seq_cut;
                    mp.erase(sqs->sig);
                    std::string line = "reloadsoundseqcut";
                    for (const auto& [w, cv] : mp) line += fmt::format(" {}:{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}", w, cv[0], cv[1], cv[2], cv[3], cv[4]);
                    if (mp.empty()) line += " none";
                    cmds += line + "\n";
                }
                hint("Each sound goes to the step whose part of the timeline holds it. Move a cut, then press Play to hear the step. Saved per weapon.");
                if (changed) {
                    std::lock_guard _{ g_seq_mtx };
                    auto mp       = g_seq_cut;
                    mp[sqs->sig]  = c;
                    std::string line = "reloadsoundseqcut";
                    for (const auto& [w, cv] : mp) line += fmt::format(" {}:{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}", w, cv[0], cv[1], cv[2], cv[3], cv[4]);
                    cmds += line + "\n";
                }
            }
        }
        if (ImGui::Button("Play the magazine sound##reloadsoundtest")) sound(kSndMagIn);
        ImGui::SameLine();
        if (ImGui::Button("Play the bolt sound##reloadsoundtest2")) sound(kSndRackFwd);
        if (ws && listed) {   // the picks are per weapon (or its category): a ticked weapon in hand
            const Builtin*   bt   = builtin(sig);
            const int        kind = bt ? bt->kind : kMagFed;
            const char*      ev[4]{ kind == kRevolver ? "Cylinder out" : "Magazine out", kind == kTube ? "Shell in" : kind == kRevolver ? "Round in" : "Magazine in",
                                    kind == kTube ? "Pump back" : "Bolt back", kind == kTube ? "Pump forward" : "Bolt forward" };
            const WeaponSnd* w    = weapon_snd(sig);
            bool sc = g_snd_cat.load();
            if (ImGui::Checkbox("Pick the sounds for the whole category, not this weapon##reloadsoundcat", &sc)) cmds += sc ? "reloadsoundcat on\n" : "reloadsoundcat off\n";
            hint(fmt::format("On: a sound picked below is for a whole category (this weapon's: {}, or the one chosen below), and comes before each "
                             "weapon's own (picked or built in). The categories: handguns, rifles and SMGs, shotguns, revolvers, belt-fed guns, the "
                             "flamethrower. Off (default): for this weapon only.", kCatName[snd_cat(sig)]).c_str());
            // manualreload32: the category the picks go to, chosen (the held weapon's sounds can be given to another category).
            const uint32_t own_cat = snd_cat(sig);
            if (sc) {
                g_snd_target = std::clamp(g_snd_target, 0, int(kCatEnd) - 1);
                const std::string own_label = fmt::format("This weapon's: {}", kCatName[own_cat]);
                if (ImGui::BeginCombo("Picks go to##reloadsoundtarget", g_snd_target == 0 ? own_label.c_str() : kCatName[g_snd_target])) {
                    if (ImGui::Selectable(own_label.c_str(), g_snd_target == 0)) g_snd_target = 0;
                    for (int c = 1; c < int(kCatEnd); ++c)
                        if (ImGui::Selectable(fmt::format("{}##sndtgt{}", kCatName[c], c).c_str(), g_snd_target == c)) g_snd_target = c;
                    ImGui::EndCombo();
                }
                hint("The category the sounds you pick below are given to. This weapon's own by default; choose another to give it this weapon's sounds "
                     "(for example a shotgun's pump for every rifle's bolt). Not saved: back to the held weapon's own when the game restarts.");
            }
            const uint32_t tgt_cat = sc ? (g_snd_target > 0 ? uint32_t(g_snd_target) : own_cat) : 0;
            const uint32_t key     = sc ? tgt_cat : sig;   // where a pick goes: the category's number, or the weapon's id
            std::array<int64_t, 4> cur{ -1, -1, -1, -1 };
            std::map<uint32_t, std::array<int64_t, 4>> mp;
            {
                std::lock_guard _{ g_pick_mtx };
                mp = g_snd_pick;
                if (const auto it = mp.find(key); it != mp.end()) cur = it->second;
            }
            auto pick = [&](int ek, int64_t val) {
                auto& a = mp.try_emplace(key, std::array<int64_t, 4>{ -1, -1, -1, -1 }).first->second;
                a[size_t(ek)] = val;
                if (std::all_of(a.begin(), a.end(), [](int64_t x) { return x < 0; })) mp.erase(key);
                std::string line = "reloadsoundpick";
                for (const auto& [ww, aa] : mp) {
                    line += fmt::format(" {}:", ww);
                    for (int i = 0; i < 4; ++i) line += (i ? "," : "") + (aa[size_t(i)] < 0 ? std::string{ "-" } : std::to_string(aa[size_t(i)]));
                }
                if (mp.empty()) line += " none";
                cmds += line + "\n";
            };
            // manualreload32: every category's picks, visible (and cleared) here.
            if (sc) {
                ImGui::TextDisabled("The sounds picked per category (out, in, back, forward; - built in for each weapon):");
                for (uint32_t c = 1; c < kCatEnd; ++c) {
                    const auto  it  = mp.find(c);
                    std::string row = fmt::format("{}{}:", kCatName[c], c == own_cat ? " (this weapon's)" : "");
                    if (it == mp.end()) row += " none";
                    else
                        for (int i = 0; i < 4; ++i) row += (i ? ", " : " ") + (it->second[size_t(i)] < 0 ? std::string{ "-" } : it->second[size_t(i)] == 0 ? std::string{ "none" }
                                                                                                                          : std::to_string(it->second[size_t(i)]));
                    ImGui::Text("%s", row.c_str());
                    if (it != mp.end()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(fmt::format("Clear##sndcatclear{}", c).c_str())) {
                            auto m2 = mp;
                            m2.erase(c);
                            std::string line = "reloadsoundpick";
                            for (const auto& [ww, aa] : m2) {
                                line += fmt::format(" {}:", ww);
                                for (int i = 0; i < 4; ++i) line += (i ? "," : "") + (aa[size_t(i)] < 0 ? std::string{ "-" } : std::to_string(aa[size_t(i)]));
                            }
                            if (m2.empty()) line += " none";
                            cmds += line + "\n";
                        }
                    }
                }
                if (tgt_cat != own_cat)
                    ImGui::TextColored(ImVec4{ 1.0f, 0.8f, 0.3f, 1.0f }, "Picks below go to %s, not this weapon's category (%s).", kCatName[tgt_cat], kCatName[own_cat]);
            }
            ImGui::Text("The %s (id %u): %s", w ? w->name : "weapon in hand", sig, w ? "" : "not in the arcade map's sound run: no sounds of its own");
            for (int e = 0; e < 4; ++e) {
                const char*   from = "";
                const int64_t id   = weapon_sound(sig, e, &from);
                if (ImGui::SmallButton(fmt::format("Play##wsndev{}", e).c_str())) sound(e);
                ImGui::SameLine();
                ImGui::Text("%s: %s", ev[e], id > 0 ? fmt::format("{} ({})", id, from).c_str() : id == 0 ? "none (picked)" : "none");
                if (cur[size_t(e)] >= 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(fmt::format("Built in again##wsndreset{}", e).c_str())) pick(e, -1);
                }
                if (id > 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(fmt::format("No sound##wsndown{}", e).c_str())) pick(e, 0);
                }
            }
            if (w) {
                ImGui::TextDisabled("Its reload in the game, in order (s from the start; after | the reload from empty or after a shot):");
                std::stringstream ss{ w->seq };
                std::string       tok;
                int               row = 0;
                while (ss >> tok) {
                    if (tok == "|" || tok.find('@') == std::string::npos) {
                        ImGui::TextDisabled("%s", tok.c_str());
                        continue;
                    }
                    const uint32_t id = uint32_t(std::strtoul(tok.c_str(), nullptr, 10));
                    if (ImGui::SmallButton(fmt::format("Play##wsndseq{}", row).c_str())) fc5::gamesound::post(id, 15, 0.0f);
                    ImGui::SameLine();
                    ImGui::Text("%-24s", tok.c_str());
                    for (int e = 0; e < 4; ++e) {
                        ImGui::SameLine();
                        // (a pick for another category: marked by that category's own choice)
                        const bool is = sc && tgt_cat != own_cat ? cur[size_t(e)] == int64_t(id) : weapon_sound(sig, e) == int64_t(id);
                        if (ImGui::SmallButton(fmt::format("{}{}##wsndpick{}_{}", is ? "* " : "", ev[e], row, e).c_str())) pick(e, int64_t(id));
                    }
                    ++row;
                }
                hint(sc ? "Play a sound, then click the event it belongs to (an asterisk marks the one in use); the choice is saved for the category chosen in "
                          "'Picks go to'. Sounds shared by many weapons are mostly the hands and the pouch."
                        : "Play a sound, then click the event it belongs to (an asterisk marks the one in use); the choice is saved for this weapon. "
                          "Sounds shared by many weapons are mostly the hands and the pouch.");
            }
        }
        ImGui::Unindent(16.0f);
    }
    if (sub("Tests and per-weapon tuning##rlsub")) {
        bool pa = g_pouch_anchor.load();
        if (ImGui::Checkbox("Hip magazine still when the gun turns##reloadpouchanchor", &pa)) cmds += pa ? "reloadpouchanchor on\n" : "reloadpouchanchor off\n";
        hint("On (default, author 3 Nov): the hip's magazine is placed where your eyes are, as the gun and the hands are, so turning the gun no longer "
             "slides it a few centimetres (the game's camera swings on its neck as the gun turns). Off: as before.");
        bool pn = g_pouch_neck.load();
        if (ImGui::Checkbox("Hip magazine fixed to the body, not the head (test)##reloadpouchneck", &pn)) cmds += pn ? "reloadpouchneck on\n" : "reloadpouchneck off\n";
        hint("On: the hip's magazine hangs from your neck instead of your eyes, so bending the head to look at it does not move it, and it turns with your "
             "body only once your head has turned more than 60 degrees (35 off), so a glance down to the left does not swing it. Off (default): as before.");
        bool md = g_model_on.load();
        if (ImGui::Checkbox("Use the game's own magazine##reloadmagmodel", &md)) cmds += md ? "reloadmagmodel on\n" : "reloadmagmodel off\n";
        hint("On (default): the game's own magazine waits on your hip, leaves the gun at the drop, sits in your left hand once taken and slides into the well, "
             "and the gun's own charging handle moves when you pull it. The AR-C's and the pistols' magazines are built in; for other weapons it is learnt at the first time the game "
             "reloads them (keep the gun still for 3 s). Until then a drawn magazine stands in.");
        bool pt = g_parts_table.load();
        if (ImGui::Checkbox("Guns not built in: parts from the game's files (test)##reloadpartstable", &pt)) cmds += pt ? "reloadpartstable on\n" : "reloadpartstable off\n";
        hint("On: a gun the mod has no row for (another skin or attachment, a DLC gun) takes its magazine, rounds, charging handle and casing port from a "
             "table made from the game's files, so it reloads by hand at once, before the game's own reload is ever seen. Magazine guns and pump shotguns; "
             "tick the gun itself above as before. Off (default): its magazine is learnt at its first game reload.");
        bool sl = g_snd_learn.load();
        if (ImGui::Checkbox("Learn each weapon's sounds from the game's reload (test)##reloadsoundlearn", &sl)) cmds += sl ? "reloadsoundlearn on\n" : "reloadsoundlearn off\n";
        hint("On: at each of the game's own reloads, the magazine's and the bolt's moves are followed and each is given the game's sound played at that moment; "
             "kept per weapon and used (after your picks, before the guesses). The log says what was matched. Off (default): picks and guesses only.");
        bool rc = g_rec.load();
        if (ImGui::Checkbox("Record reloads, frame by frame, to fc5vr_reloadrec.csv (test)##reloadrec", &rc)) cmds += rc ? "reloadrec on\n" : "reloadrec off\n";
        hint("On: every frame of the game's own reload and of your manual steps is written to bin\\fc5vr_reloadrec.csv: the left hand, the magazine, the slide and the fingers "
             "against the gun, and the game's sounds, to compare the two. Not saved. Command reloadrec on/off.");
        bool gh = g_game_hand.load();
        if (ImGui::Checkbox("Reload button: show the game's own left hand (test)##reloadgamehand", &gh)) cmds += gh ? "reloadgamehand on\n" : "reloadgamehand off\n";
        hint("On: while the game's reload runs from the reload button, the left hand is drawn where the game's animation has it on the gun, not at your controller. "
             "Not saved. Command reloadgamehand on/off.");
        if (listed) {
            const Geom g = geom(sig);
            ImGui::Text("Well (%s): %.3f %.3f %.3f, handle %.3f %.3f %.3f (weapon hand's controller: right, up, back)", g.measured ? "measured on the game's magazine" : "default",
                        g.well.x, g.well.y, g.well.z, g.handle.x, g.handle.y, g.handle.z);
            if (ImGui::Button("Set the well: hold a magazine where it sits seated, then click##reloadwellhere")) cmds += "reloadwellhere\n";
            hint("With a magazine in your left hand, hold it where it would sit fully in the gun (upright as it goes in) and click: the well point and its axis are taken "
                 "from your hands for this weapon. Point the controller's ray at the button and click with the right trigger.");
            if (ImGui::Button("Set the charging handle: hold the left controller on it, then click##reloadhandlehere")) cmds += "reloadhandlehere\n";
            {
                uint32_t hc = 0, hb = 0;
                bool     hl = false;
                if (handle_part(sig, hc, hb, hl))
                    ImGui::Text("The gun's charging handle part: draw %u %s", hc, hb == kRigidBone ? "(whole)" : fmt::format("bone {}", hb).c_str());
                else if (const Builtin* bt = builtin(sig)) {
                    std::string bl;
                    for (int i = 0; i < bt->nhandle; ++i) bl += fmt::format("{}{}", i ? ", " : "", bt->handle[i]);
                    ImGui::Text("The gun's %s part: built in for the %s (draw %u bone(s) %s%s)", bt->pistol ? "slide" : "charging handle", bt->name, sig, bl.c_str(),
                                bt->also ? fmt::format(", its sights' draw {}", bt->also).c_str() : "");
                }
                else
                    ImGui::TextDisabled("The gun's charging handle part: not found yet (the part nearest the blue ball moves)");
                if (ImGui::Button("Try the next part as the charging handle##reloadhandlenext")) g_handle_next++;
                hint("If pulling the handle moves the wrong part of the gun (or none), click this, then pull again: the next part of the gun nearest the blue "
                     "ball is taken (each click the next). The choice is saved. It is also found by itself when the game reloads from empty.");
            }
        } else {
            ImGui::TextDisabled("Hold a weapon ticked above to see and set its magazine well and charging handle.");
        }
        ImGui::Unindent(16.0f);
    }
}
}
