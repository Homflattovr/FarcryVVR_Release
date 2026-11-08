// The game's own SetPlayerFOV, called by the mod (author 19 Oct: widen the parachute's 65 deg view through the game's
// script API instead of changing memory). The native behind the Lua function SetPlayerFOV (FC_m64+0x2FBEA10, reached
// from its Lua binding +0x2F61460): (player id*, angle deg, blend s, override flag, byte). Simulator 19 Oct 23:50, on foot:
// (1.745, 0.5, true) moved the view 120 -> 1.7 deg in 0.5 s, (120, 0.5, true) back; with the flag false nothing changed.
//
// Safety: the game's code is not thread-safe. The call is made inside the game's own camera update (the Tobii slot),
// and only when that thread is the one the game runs its scripts on: the game's own CScriptSystem::ExecuteString calls
// are watched by an inline hook that only records the thread and passes the call on (CryHook5's route, code at
// FC_m64+0xdfa424 verified byte for byte). No script text is ever run by the mod.
//
// meleedamage (26 Oct, author 12:44):
// the native behind the Lua function SendPierceStimWithOrigin (+0x2FB6760: (target id*, origin id, damage); its binding +0x2F59B80
// reads (id, id, float)) sends the target a damage stim from the origin, its direction from origin to target. The NPC hit is found
// by position: the game's entity lookup by id (+0xB27D60: (manager [+0x4FAC1E0], handle* out, id), a jmp into protected code; a
// handle holds its entity at +0x10, a refcount at +8, the null handle is +0x4CB9E70) is watched only while meleedamage is on and
// records the ids it finds; at a hit the recorded ids' positions are read with the native behind GetEntityPosition (+0x2FA09C0:
// (id*, axis 0-2) -> float) on the game's script thread, and the one nearest the body's root is the target. All read statically
// from this build's FC_m64.dll (26 Oct), not run yet.
//
// vehscript (29 Oct, author 15:22): whether the player is in a vehicle, from the game itself instead of
// the hands' pass. The native behind the Lua function GetPawnVehicle (+0x2FA2C30, from its binding +0x2F4A3A0 registered at
// +0x2F76E58): (pawn id*, vehicle id* out) -> out; the pawn from its id (+0x2F8E610, the entity lookup), its current vehicle
// (+0x1C72980, protected code), and that vehicle's entity id, or -1 when it has none. The seat: GetPawnVehicleSeat's native
// (+0x2FA2C80, binding +0x2F4A450): (pawn id*) -> seat index (0 without one). The driver's seat: what GetDriverSeatIndex's binding
// (+0x2F80E30) does inline: the vehicle from its id (+0x2F8E8C0), then its driver seat index (+0x1C73890 on the vehicle + 0x20).
// The type: GetPawnVehicleType's native (+0x2FA2CD0, binding +0x2F4A4F0): (pawn id*) -> 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle,
// 4 helicopter, 5 plane, 6 wingsuit, 7 none (the engine natives report of 30 Oct). All plain reads, made like SetPlayerFOV (the camera update on the script thread), under SEH: a fault turns vehscript off.
// Read statically from this build's FC_m64.dll (29 Oct), not run yet.
//
// gamestate (30 Oct, log only, research/2026-10-30_engine_natives.md): the player's state as the game's own script functions give it,
// read every 100 ms on the script thread like vehscript: GetPawnVehicleType (+0x2FA2CD0; 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle,
// 4 helicopter, 5 plane, 6 wingsuit, 7 none, from the VEHICLE_TYPE_* string order), GetPawnRidingEntity (+0x2FA2AE0), GetPawnStance
// (+0x2FA2B50), IsInQTE (+0x2FA8E80), IsPawnDown (+0x2FA9900), IsPawnDead (+0x2FA97D0), IsInAir (+0x2FA8D20), IsInWater (+0x2FA8F90),
// GetPlayerFOV (+0x2FA2F40), GetPlayerNearFOV (+0x2FA3670); IsGamePaused's byte (*(+0x4EB59F8) + 0x68, set by paused menu pages) read every
// frame; the Lua bindings that start, stop or skip cinematics or switch the camera, and the cinematic black bars (+0x1476B40, a jmp), logged
// as they are called. All read statically from this build's FC_m64.dll (30 Oct), not run yet.
//
// reloadmanual (2 Nov, research/2026-11-02_manualreload.md): the player's clip and reserve, read every 100 ms on the script thread the
// way the native behind the Lua function GetPlayerAmmoIsEmpty (+0x2FA2E40, its 0xF2 bytes checked whole) does it: the entity from the
// player's id (the lookup +0xB27D60, manager [+0x4FAC1E0]; the handle's entity at +0x10), its inventory ([entity+0xC8] ? +0xF48EE0(
// [entity+0xC8], entity+0xA8, 0) : +0xCCD250(entity, &[+0xF64790()+0x18])), the weapon in hand (inventory vtable +0x308), its ammo item
// (weapon vtable +0x50), the reserve (inventory vtable +0x1B0 (inventory, item)) and the clip (weapon vtable +0x40); the handle released
// as the native does (refcount at +8, +0x26E320 at the last; the null handle +0x4CB9E70 is never released). The native only adds
// the two and compares with 0. Read statically from this build's FC_m64.dll (2 Nov), not run yet; a fault turns it off.
#include "FarCry5Lua.h"
#include "FarCry5Gesture.h"
#include "FarCry5GameRay.h"
#include "FarCry5Spawn.h"
#include "FarCry5GameSound.h"
#include "FarCry5Tobii.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <utility>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

namespace fc5::lua
{
    namespace
    {
        using ExecFn    = bool(__fastcall*)(void* self, const char* code, bool raw);
        using FovFn     = void(__fastcall*)(uint64_t* id, float angle, float blend, bool flag, uint8_t extra);
        using LocalIdFn = uint64_t*(__fastcall*)(uint64_t* out);   // GetLocalPlayerId's helper (+0x2FA1E50)

        uint8_t*                   g_base     = nullptr;
        ExecFn                     g_exec     = nullptr;
        FovFn                      g_fov      = nullptr;
        LocalIdFn                  g_local_id = nullptr;
        safetyhook::InlineHook     g_hook{};
        bool                       g_tried    = false;
        bool                       g_ready    = false;
        std::atomic<unsigned long> g_game_tid{ 0 };    // the thread the game runs its scripts on
        std::atomic<uint64_t>      g_game_calls{ 0 };
        std::atomic<unsigned long> g_tick_tid{ 0 };    // the game's camera update thread
        std::atomic<bool>          g_in_play{ false };
        std::atomic<bool>          g_fov_pending{ false };
        std::atomic<float>         g_fov_a{ 0.0f }, g_fov_b{ 0.0f };
        std::atomic<bool>          g_fov_flag{ false };
        std::atomic<uint64_t>      g_fov_calls{ 0 };

        // meleedamage
        using LookupFn = void*(__fastcall*)(void* mgr, void** out, uint64_t id);
        using PosFn    = float(__fastcall*)(uint64_t* id, int axis);
        using StimFn   = void(__fastcall*)(uint64_t* target, uint64_t origin, float damage);
        using HealthFn = float(__fastcall*)(uint64_t* id);   // GetPawnHealth's native (+0x2FA2900)
        PosFn                  g_pos  = nullptr;
        HealthFn               g_health = nullptr;
        std::atomic<uint64_t>  g_dmg_id{ 0 };   // meleeray: the entity the ray hit (0: find it by the place)
        std::atomic<bool>      g_self_pending{ false };   // meleedamageself (test): the damage stim sent to the player
        std::atomic<float>     g_self_amount{ 0.0f };
        uint64_t               g_after_id   = 0;          // the last target, its health read again 30 updates later
        uint32_t               g_after_tick = 0;
        float                  g_after_h0   = 0.0f;
        StimFn                 g_stim = nullptr;
        safetyhook::InlineHook g_lookup_hook{};
        std::atomic<bool>      g_watch{ false };
        bool                   g_watch_tried = false, g_watch_ok = false;
        constexpr int          kIds = 2048;
        std::atomic<uint64_t>  g_ids[kIds]{};       // ids the lookup found an entity for (by hash)
        std::atomic<uint32_t>  g_id_tick[kIds]{};   // ... and the game update (g_tick_n) it was last seen in
        std::atomic<uint32_t>  g_tick_n{ 0 };       // game updates (game_tick)
        std::atomic<uint64_t>  g_lookups{ 0 };
        std::atomic<bool>      g_dmg_pending{ false };
        std::atomic<float>     g_dmg_x{ 0.0f }, g_dmg_y{ 0.0f }, g_dmg_z{ 0.0f }, g_dmg_amount{ 0.0f };
        std::atomic<bool>      g_dmg_send{ false };
        std::atomic<uint64_t>  g_dmg_sent{ 0 }, g_dmg_found{ 0 }, g_dmg_missed{ 0 };
        std::atomic<int64_t>   g_dmg_t{ 0 };
        std::atomic<bool>      g_probe_pending{ false };   // meleedamageprobe (test)
        std::atomic<float>     g_probe_x{ 0.0f }, g_probe_y{ 0.0f }, g_probe_z{ 0.0f };

        // vehscript
        using PawnVehFn    = uint64_t*(__fastcall*)(uint64_t* id, uint64_t* out);   // GetPawnVehicle's native (+0x2FA2C30)
        using PawnSeatFn   = int(__fastcall*)(uint64_t* id);                         // GetPawnVehicleSeat's native (+0x2FA2C80)
        using VehObjFn     = uint8_t*(__fastcall*)(uint64_t id);                     // the vehicle from its id (+0x2F8E8C0)
        using DriverSeatFn = int(__fastcall*)(uint8_t* seats);                       // its driver seat index (+0x1C73890, vehicle + 0x20)
        using PawnTypeFn   = int(__fastcall*)(uint64_t* id);                         // GetPawnVehicleType's native (+0x2FA2CD0)
        PawnVehFn             g_pawn_veh    = nullptr;
        PawnSeatFn            g_pawn_seat   = nullptr;
        VehObjFn              g_veh_obj     = nullptr;
        DriverSeatFn          g_driver_seat = nullptr;
        PawnTypeFn            g_pawn_type   = nullptr;
        std::atomic<bool>     g_veh_on{ false };
        bool                  g_veh_tried = false, g_veh_ok = false;
        std::atomic<bool>     g_veh_failed{ false };
        std::atomic<int>      g_veh_state{ -1 }, g_veh_driver{ -1 }, g_veh_seat{ -1 }, g_veh_dseat{ -1 }, g_veh_type{ -1 };
        std::atomic<uint64_t> g_veh_id{ 0 }, g_veh_queries{ 0 };
        std::atomic<int64_t>  g_veh_t{ 0 };     // the last answer (now_ms)
        int64_t               g_veh_ask_t = 0;  // the last query (script thread only)
        int                   g_veh_said  = 0;

        // gamestate (log only)
        using PawnIntFn   = int(__fastcall*)(uint64_t* id);     // GetPawnVehicleType (+0x2FA2CD0), GetPawnStance (+0x2FA2B50)
        using PawnBoolFn  = bool(__fastcall*)(uint64_t* id);    // IsInQTE, IsPawnDown, IsPawnDead, IsInAir, IsInWater
        using PawnFloatFn = float(__fastcall*)(uint64_t* id);   // GetPlayerFOV (+0x2FA2F40), GetPlayerNearFOV (+0x2FA3670)
        struct GameState
        {
            uint64_t veh = ~0ull, ride = ~0ull;
            int      vtype = -1, seat = -1, dseat = -1, stance = -1;
            bool     qte = false, down = false, dead = false, air = false, water = false;
            float    fov = 0.0f, nearfov = 0.0f;
            int64_t  cam_vt = -1;        // the active camera's vtable (FC_m64 rva), -1 none, -2 the read faulted
        };
        PawnIntFn             g_gs_vtype = nullptr, g_gs_stance = nullptr;
        PawnBoolFn            g_gs_qte = nullptr, g_gs_down = nullptr, g_gs_dead = nullptr, g_gs_air = nullptr, g_gs_water = nullptr;
        PawnFloatFn           g_gs_fov = nullptr, g_gs_nearfov = nullptr;
        PawnVehFn             g_gs_ride  = nullptr;   // GetPawnRidingEntity (+0x2FA2AE0), GetPawnVehicle's shape
        uint8_t**             g_gs_pause = nullptr;   // IsGamePaused (+0x2FA8580): *(*(+0x4EB59F8) + 0x68)
        std::atomic<bool>     g_gs_on{ false };
        bool                  g_gs_tried = false, g_gs_ok = false;
        std::atomic<bool>     g_gs_failed{ false };
        GameState             g_gs_last{};            // script thread only
        bool                  g_gs_have = false;
        int64_t               g_gs_ask_t = 0;
        std::atomic<uint64_t> g_gs_reads{ 0 }, g_gs_changes{ 0 };
        int                   g_gs_pause_last = -1;   // render thread only

        // downview: IsPawnDown (+0x2FA9900) for the player, read in vehscript's 150 ms poll on the script thread (its own switch,
        // with or without vehscript and gamestate); a fault turns it off for the session.
        PawnBoolFn            g_down_fn = nullptr;
        std::atomic<bool>     g_down_on{ false };
        bool                  g_down_tried = false, g_down_ok = false;
        std::atomic<bool>     g_down_failed{ false };
        std::atomic<int>      g_down_state{ -1 };
        std::atomic<int64_t>  g_down_t{ 0 };     // the last answer (now_ms)
        // qtehud: IsInQTE (+0x2FA8E80) for the player in the same poll (its own switch); a fault turns it off for the session.
        PawnBoolFn            g_qte_fn = nullptr;
        std::atomic<bool>     g_qte_on{ false };
        bool                  g_qte_tried = false, g_qte_ok = false;
        std::atomic<bool>     g_qte_failed{ false };
        std::atomic<int>      g_qte_state{ -1 };
        std::atomic<int64_t>  g_qte_t{ 0 };
        // GetPawnStance (+0x2FA2B50) in the same poll while vehscript asks: 0 standing, 1 crouched, 4 downed, 18 seated in a vehicle (09:27 log).
        PawnIntFn             g_stance_fn = nullptr;
        std::atomic<int>      g_stance{ -1 };
        std::atomic<int64_t>  g_stance_t{ 0 };
        // chutemin: IsInAir (+0x2FA8D20) in the same poll while vehscript asks: true from leaving the ground to landing or the chute opening.
        PawnBoolFn            g_air_fn = nullptr;
        std::atomic<int>      g_air{ -1 };
        std::atomic<int64_t>  g_air_t{ 0 };

        // gamestate: the game's scripts starting, stopping or skipping cinematics, taking the camera, and the cinematic black bars,
        // logged as they are called (their Lua bindings, int(lua_State*), watched while gamestate is on; nothing changed).
        struct ScriptWatch { uint32_t rva; const char* name; };
        constexpr ScriptWatch kWatch[]{
            { 0x2F89B90, "StartCinematicScene" }, { 0x2F89DD0, "StartCinematicScene_v2" },
            { 0x2F65090, "StartCinematicMetaSequence" }, { 0x31B8D60, "StartCinematicMetaSequence (sequence manager)" },
            { 0x2F65430, "StartCinematicSequenceWithMetaId" }, { 0x31B9130, "StartCinematicSequenceWithMetaId (sequence manager)" },
            { 0x2F652D0, "StartCinematicSequenceFromBeginningWithMetaId" }, { 0x31B8FC0, "StartCinematicSequenceFromBeginningWithMetaId (sequence manager)" },
            { 0x2F65130, "StartCinematicSequenceAtSyncPointWithMetaId" }, { 0x31B8E10, "StartCinematicSequenceAtSyncPointWithMetaId (sequence manager)" },
            { 0x2F667F0, "StopCinematicSequence" }, { 0x31B9620, "StopCinematicSequence (sequence manager)" },
            { 0x2F66700, "StopCinematicMetaSequence" }, { 0x31B92A0, "StopCinematicMetaSequence (sequence manager)" },
            { 0x2F637E0, "SkipCinematicSequence" }, { 0x31B8C30, "SkipCinematicSequence (sequence manager)" },
            { 0x2F67620, "SwitchCamera" }, { 0x2F5B440, "SetCloseUpCamera" },
        };
        constexpr int          kWatchN = int(sizeof(kWatch) / sizeof(kWatch[0]));
        safetyhook::InlineHook g_watch_hooks[kWatchN]{};
        std::atomic<uint64_t>  g_watch_calls{ 0 };
        safetyhook::InlineHook g_bars_hook{};
        std::atomic<int>       g_bars{ -1 };   // the last DisplayCinematicBlackBars (+0x1476B40, bool), -1 none yet

        template <int N> int __fastcall hk_script(void* L)
        {
            if (g_gs_on.load(std::memory_order_relaxed)) {
                g_watch_calls++;
                spdlog::info("[GameState] script calls {}", kWatch[N].name);
            }
            return g_watch_hooks[N].call<int>(L);
        }

        template <int... I> void watch_install(std::integer_sequence<int, I...>, uint8_t* base, int& ok)
        {
            ((g_watch_hooks[I] = safetyhook::create_inline(reinterpret_cast<void*>(base + kWatch[I].rva), reinterpret_cast<void*>(&hk_script<I>)), ok += bool(g_watch_hooks[I])), ...);
        }

        void __fastcall hk_bars(uint8_t on)
        {
            if (g_gs_on.load(std::memory_order_relaxed)) {
                spdlog::info("[GameState] cinematic black bars {} (were {})", on ? "on" : "off", g_bars.load());
                g_bars = on ? 1 : 0;
            }
            g_bars_hook.call<void>(on);
        }

        // gamestate: the camera the game projects HUD points with, found as Map3DPosInto2DHud's native (+0x2FAB890) does: the camera list
        // at +0x4FB3110 (count in the high dword of +0x10, first entry *[+8]), its view index (+0xE69C50), the view service (the function
        // pointer at +0x4CCDA60), the service's camera for that index (vtable +0x198), all game code. Only the camera's vtable is kept: one
        // per camera kind (player, map, photo mode, death, a bone during an animation, cinematic, vehicle...), named once the run shows them.
        bool                  g_gs_cam_ok = true;
        int64_t gs_cam_read(uint8_t* base)
        {
            __try {
                uint8_t* list = *reinterpret_cast<uint8_t**>(base + 0x4FB3110);
                if (!list || ((*reinterpret_cast<uint64_t*>(list + 0x10) >> 32) & 0x7FFFFFFF) == 0) return -1;
                void* first = **reinterpret_cast<void***>(list + 8);
                if (!first) return -1;
                const uint8_t idx = reinterpret_cast<uint8_t(__fastcall*)(void*)>(base + 0xE69C50)(first);
                void*  tmp[2]{};
                void** svcp = reinterpret_cast<void**(__fastcall*)(void**)>(*reinterpret_cast<void**>(base + 0x4CCDA60))(tmp);
                void*  svc  = svcp ? *svcp : nullptr;
                if (!svc) return -1;
                void* cam = (*reinterpret_cast<void* (__fastcall***)(void*, uint32_t)>(svc))[0x198 / 8](svc, idx);
                if (!cam) return -1;
                return int64_t(*reinterpret_cast<uint8_t**>(cam) - base);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        const char* vehicle_type_name(int t)
        {
            // The VEHICLE_TYPE_* strings in FC_m64.dll, in their stored order (7 = none, GetVehicleType's default).
            static const char* kNames[]{ "wheeled", "boat", "paraglider", "motorcycle", "helicopter", "plane", "wingsuit", "none" };
            return t >= 0 && t < 8 ? kNames[t] : t == -1 ? "-" : "?";
        }

        // gamestate: the reads, under SEH (no C++ objects here): false on a fault.
        bool gs_read(uint64_t me, GameState& s)
        {
            __try {
                uint64_t id = me, out = ~0ull;
                if (g_pawn_veh) {
                    g_pawn_veh(&id, &out);
                    s.veh = out;
                    if (out != ~0ull && out != 0) {
                        id     = me;
                        s.seat = g_pawn_seat(&id);
                        if (uint8_t* v = g_veh_obj(out)) s.dseat = g_driver_seat(v + 0x20);
                    }
                }
                id = me; out = ~0ull;
                g_gs_ride(&id, &out);
                s.ride = out;
                id = me; s.vtype   = g_gs_vtype(&id);
                id = me; s.stance  = g_gs_stance(&id);
                id = me; s.qte     = g_gs_qte(&id);
                id = me; s.down    = g_gs_down(&id);
                id = me; s.dead    = g_gs_dead(&id);
                id = me; s.air     = g_gs_air(&id);
                id = me; s.water   = g_gs_water(&id);
                id = me; s.fov     = g_gs_fov(&id);
                id = me; s.nearfov = g_gs_nearfov(&id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        int gs_pause_read()
        {
            __try {
                const uint8_t* g = *g_gs_pause;
                return g ? g[0x68] : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        std::string gs_text(const GameState& s)
        {
            const bool in = s.veh != ~0ull && s.veh != 0;
            return fmt::format("vehicle {} (type {} {}, seat {}, driver's seat {}), riding {}, stance {}, QTE {}, down {}, dead {}, in air {}, in water {}, FOV {:.2f}, near FOV {:.2f}, camera {}",
                               in ? fmt::format("{:x}", s.veh) : std::string{ "none" }, s.vtype, vehicle_type_name(s.vtype), s.seat, s.dseat,
                               s.ride != ~0ull && s.ride != 0 ? fmt::format("{:x}", s.ride) : std::string{ "none" }, s.stance, s.qte, s.down, s.dead, s.air, s.water, s.fov,
                               s.nearfov, s.cam_vt >= 0 ? fmt::format("vtable +{:#x}", s.cam_vt) : s.cam_vt == -1 ? std::string{ "none" } : std::string{ "fault" });
        }

        // gameweapon (1 Nov, research/2026-11-01_weapon_data.md): the item the game says the player holds, from GetEquippedItemID's native
        // (+0x2FA0C20: (pawn id*) -> the item's database id, the disLibItemId of its CItemDescriptor; 0 or -1 with none), asked every 150 ms
        // on the script thread like vehscript. FarCry5WeaponTable.h turns the id into what the game's weapon data says about it.
        using EquipFn = uint64_t(__fastcall*)(uint64_t* id);
        EquipFn               g_eq_fn = nullptr;
        std::atomic<bool>     g_eq_on{ false };
        bool                  g_eq_tried = false, g_eq_ok = false;
        std::atomic<bool>     g_eq_failed{ false };
        std::atomic<uint64_t> g_eq_item{ 0 };
        std::atomic<int64_t>  g_eq_t{ 0 };     // the last answer (now_ms)
        int64_t               g_eq_ask_t = 0;  // script thread only

        // reloadmanual: the clip and the reserve (see the top). The native's own bytes, so every offset used below is this build's.
        constexpr uint8_t kAmmoNative[0xF2]{
            0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x01, 0x48, 0x8d, 0x54, 0x24, 0x48, 0x48, 0x8b, 0x0d, 0x8a, 0x93, 0x00, 0x02, 0x48, 0x89,
            0x74, 0x24, 0x50, 0x4c, 0x89, 0x74, 0x24, 0x20, 0xe8, 0xfb, 0x4e, 0xb8, 0xfd, 0x48, 0x8b, 0x5c, 0x24, 0x48, 0x48, 0x8b, 0x7b, 0x10, 0x48, 0x85,
            0xff, 0x0f, 0x84, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x14, 0x44, 0x0f, 0xb6, 0x44, 0x24,
            0x40, 0x48, 0x8d, 0x97, 0xa8, 0x00, 0x00, 0x00, 0xe8, 0x4b, 0x60, 0xfa, 0xfd, 0xeb, 0x19, 0xe8, 0xf4, 0x18, 0xfc, 0xfd, 0x48, 0x8b, 0xcf, 0x8b,
            0x50, 0x18, 0x89, 0x54, 0x24, 0x40, 0x48, 0x8d, 0x54, 0x24, 0x40, 0xe8, 0xa0, 0xa3, 0xd2, 0xfd, 0x48, 0x8b, 0xf0, 0x48, 0x85, 0xc0, 0x74, 0x3f,
            0x48, 0x8b, 0x00, 0x48, 0x8b, 0xce, 0xff, 0x90, 0x08, 0x03, 0x00, 0x00, 0x4c, 0x8b, 0xf0, 0x48, 0x85, 0xc0, 0x74, 0x2b, 0x48, 0x8b, 0x10, 0x48,
            0x8b, 0xc8, 0x48, 0x8b, 0x3e, 0xff, 0x52, 0x50, 0x48, 0x8b, 0xd0, 0x48, 0x8b, 0xce, 0xff, 0x97, 0xb0, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x16, 0x49,
            0x8b, 0xce, 0x8b, 0xf8, 0xff, 0x52, 0x40, 0x03, 0xc7, 0x40, 0x0f, 0x94, 0xc7, 0xeb, 0x03, 0x40, 0x32, 0xff, 0x4c, 0x8b, 0x74, 0x24, 0x20, 0x48,
            0x8d, 0x05, 0x6a, 0x6f, 0xd1, 0x01, 0x48, 0x8b, 0x74, 0x24, 0x50, 0x48, 0x3b, 0xd8, 0x74, 0x17, 0x83, 0xc9, 0xff, 0xf0, 0x0f, 0xc1, 0x4b, 0x08,
            0x83, 0xf9, 0x01, 0x75, 0x0a, 0x48, 0x8b, 0x4c, 0x24, 0x48, 0xe8, 0xf9, 0xb3, 0x2c, 0xfd, 0x40, 0x0f, 0xb6, 0xc7, 0x48, 0x83, 0xc4, 0x28, 0x5f,
            0x5b, 0xc3 };
        using AmLookupFn  = void*(__fastcall*)(void* mgr, uint8_t** out, uint64_t id);
        using AmInvFn     = uint8_t*(__fastcall*)(uint8_t* comp, uint8_t* key, uint8_t flag);   // +0xF48EE0
        using AmKeyFn     = uint8_t*(__fastcall*)();                                            // +0xF64790
        using AmFindFn    = uint8_t*(__fastcall*)(uint8_t* ent, uint32_t* key);                 // +0xCCD250
        using AmReleaseFn = void(__fastcall*)(uint8_t* handle);                                 // +0x26E320
        using AmObjFn     = uint8_t*(__fastcall*)(uint8_t* self);
        using AmCountFn   = int(__fastcall*)(uint8_t* self, uint8_t* item);
        using AmClipFn    = int(__fastcall*)(uint8_t* self);
        using AmTakeFn    = void(__fastcall*)(uint8_t* self, uint8_t* item, int count, int flags);
        std::atomic<bool>    g_am_on{ false };
        bool                 g_am_tried = false, g_am_ok = false;
        std::atomic<bool>    g_am_failed{ false };
        std::atomic<int>     g_am_clip{ -1 }, g_am_reserve{ -1 };
        std::atomic<int>     g_am_cap{ -1 };    // the clip's size (the weapon's vtable +0x48, as reloadnogame reads it; wristammo)
        std::atomic<int64_t> g_am_t{ 0 };     // the last answer (now_ms)
        int64_t              g_am_ask_t = 0;  // script thread only
        std::atomic<bool>    g_am_fast{ false };   // ammo_fast: asked at every game update
        int                  g_am_said_clip = -2, g_am_said_res = -2;

        // reloadnogame (author 4 Nov: the magazine's drop plays the whole reload's sounds; the AK's right hand moves by itself; the revolver's barrel, the AR-C's rounds at the well): the clip
        // filled with no reload of the game's, so none of it runs behind the manual one. RefillPocketedWeaponClip +0x2FAE850 (id*, item) fills it (ForceFullAmmoInClip +0x2F9C560 did not, in
        // play). What went in leaves the reserve as RemoveItem +0x2FB3740 does it: inventory vtable +0x170 (ammo item, count, 0x12), the ammo the am_read below reads. (GiveAmmoToPlayer with a
        // negative count raised the reserve to its maximum, 67 -> 72 in the 4 Nov campaign run: free ammo.)
        std::atomic<bool> g_fill_req{ false }, g_fill_failed{ false }, g_fill_short{ false };
        std::atomic<uint64_t> g_am_obj{ 0 };   // reloadfullmag: the weapon object in hand at the latest ammo read (am_tick), 0 with none
        int               g_fill_misses = 0;   // script thread only
        std::atomic<int>  g_take_req{ 0 };
        using FillFn   = void(__fastcall*)(uint64_t* id);
        bool fill_call(uint64_t me)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<FillFn>(g_base + 0x2F9C560)(&id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool refill_call(uint64_t me, uint64_t item)   // RefillPocketedWeaponClip +0x2FAE850 (id*, item)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<uint8_t(__fastcall*)(uint64_t*, uint64_t)>(g_base + 0x2FAE850)(&id, item);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Under SEH (no C++ objects): 1 read, 0 no weapon or inventory (not an error), -1 a fault. cap: the clip's size (the weapon's vtable +0x48: GiveAmmoClipsToPlayer reads it so). take: that many
        // rounds out of the reserve first.
        // obj: the weapon object in hand (reloadfullmag: FarCry5SwitchSpeed.cpp finds its common properties block from it), 0 with none.
        int am_read(uint64_t me, int& clip, int& reserve, int* cap = nullptr, int take = 0, uint64_t* obj = nullptr)
        {
            __try {
                uint8_t* h = nullptr;
                reinterpret_cast<AmLookupFn>(g_base + 0xB27D60)(*reinterpret_cast<void**>(g_base + 0x4FAC1E0), &h, me);
                if (!h) return 0;
                int      r   = 0;
                uint8_t* ent = *reinterpret_cast<uint8_t**>(h + 0x10);
                if (ent) {
                    uint8_t* inv  = nullptr;
                    uint8_t* comp = *reinterpret_cast<uint8_t**>(ent + 0xC8);
                    if (comp) {
                        inv = reinterpret_cast<AmInvFn>(g_base + 0xF48EE0)(comp, ent + 0xA8, 0);
                    } else {
                        uint32_t key = *reinterpret_cast<uint32_t*>(reinterpret_cast<AmKeyFn>(g_base + 0xF64790)() + 0x18);
                        inv          = reinterpret_cast<AmFindFn>(g_base + 0xCCD250)(ent, &key);
                    }
                    if (inv) {
                        uint8_t* wpn = (*reinterpret_cast<AmObjFn**>(inv))[0x308 / 8](inv);
                        if (obj) *obj = reinterpret_cast<uint64_t>(wpn);
                        if (wpn) {
                            uint8_t* item = (*reinterpret_cast<AmObjFn**>(wpn))[0x50 / 8](wpn);
                            if (take > 0 && item) (*reinterpret_cast<AmTakeFn**>(inv))[0x170 / 8](inv, item, take, 0x12);
                            reserve       = (*reinterpret_cast<AmCountFn**>(inv))[0x1B0 / 8](inv, item);
                            clip          = (*reinterpret_cast<AmClipFn**>(wpn))[0x40 / 8](wpn);
                            if (cap) *cap = (*reinterpret_cast<AmClipFn**>(wpn))[0x48 / 8](wpn);
                            r             = 1;
                        }
                    }
                }
                if (h != g_base + 0x4CB9E70 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(h + 8)) == 0) reinterpret_cast<AmReleaseFn>(g_base + 0x26E320)(h);
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        void fill_tick()
        {
            if (g_fill_failed.load() || !g_local_id) return;
            static const uint8_t kFill[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09 };
            static const uint8_t kRefill[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
            if (std::memcmp(g_base + 0x2F9C560, kFill, sizeof(kFill)) != 0 || std::memcmp(g_base + 0x2FAE850, kRefill, sizeof(kRefill)) != 0) {
                g_fill_failed = true;
                spdlog::error("[Reload] reloadnogame: unexpected code at the game's ammo calls: not used");
                return;
            }
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            int c0 = -1, r0 = -1, c1 = -1, r1 = -1, cap = 0;
            if (am_read(me, c0, r0, &cap) != 1) {
                g_fill_failed = true;
                spdlog::error("[Reload] reloadnogame: a fault in the game's ammo calls: turned off");
                return;
            }
            // The last rounds, fewer than the clip lacks: the calls fill it whole, so the game's own reload, this once. (Its count is exact; its sounds are heard behind this one magazine.)
            if (cap > 0 && (c0 >= cap || r0 < cap - c0)) {
                g_fill_short = true;
                spdlog::info("[Reload] reloadnogame: clip {} of {}, reserve {}: the game's reload this time, its count is exact", c0, cap, r0);
                return;
            }
            const char* how = "RefillPocketedWeaponClip";
            const uint64_t item = g_eq_item.load();
            if (item != 0 && refill_call(me, item)) am_read(me, c1, r1);
            if (c1 <= c0) {
                how = "ForceFullAmmoInClip";
                if (fill_call(me)) am_read(me, c1, r1);
            }
            if (c1 <= c0) {
                g_fill_short = true;
                if (++g_fill_misses >= 3) g_fill_failed = true;   // (the third time: for the session)
                spdlog::info("[Reload] reloadnogame: the game's calls did not fill the clip ({} -> {}, reserve {}; {} time(s)): its reload is tapped", c0, c1, r1, g_fill_misses);
                return;
            }
            g_fill_misses  = 0;
            const int went = c1 - c0;
            int       r2   = r1;
            if (r1 >= r0) am_read(me, c1, r2, nullptr, went);   // (a call that took them from the reserve itself: none taken again)
            spdlog::info("[Reload] reloadnogame: clip {} -> {} of {}, reserve {} -> {} ({}, no game reload)", c0, c1, cap, r0, r2, how);
        }

        void am_tick()
        {
            g_am_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            int       clip = -1, reserve = -1, cap = -1;
            uint64_t  obj  = 0;
            const int r    = am_read(me, clip, reserve, &cap, 0, &obj);
            g_am_obj       = r == 1 ? obj : 0;   // reloadfullmag
            if (r < 0) {
                g_am_on     = false;
                g_am_failed = true;
                spdlog::error("[Reload] a fault reading the player's clip and reserve: turned off (timers stand in)");
                return;
            }
            if (r == 0) return;
            if (clip < 0 || clip > 10000 || reserve < 0 || reserve > 100000) {
                if (g_am_said_clip != -3) spdlog::info("[Reload] the game gave clip {} reserve {}: not used", clip, reserve);
                g_am_said_clip = -3;
                return;
            }
            if (g_am_said_clip == -2 || ((clip == 0) != (g_am_said_clip == 0)) || clip > g_am_said_clip || (reserve != g_am_said_res && clip >= g_am_said_clip)) {
                spdlog::info("[Reload] the game: clip {}, reserve {}", clip, reserve);   // at the first read, running dry and refills (not every shot)
            }
            g_am_said_clip = clip;
            g_am_said_res  = reserve;
            g_am_clip      = clip;
            g_am_reserve   = reserve;
            g_am_cap       = cap >= 0 && cap <= 10000 ? cap : -1;
            g_am_t         = now_ms();
        }

        bool eq_read(uint64_t me, uint64_t& item)
        {
            __try {
                uint64_t id = me;
                item        = g_eq_fn(&id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // throwlibrary (2 Nov, author 21:40; research\2026-11-02_throwables.md): the selected throwable, as the HUD asks when the game's SwitchThrowable event comes (+0x2393EF0 -> +0x24F5EA0):
        // the player's inventory (the route of am_read) vtable +0x2C8 (inventory, slot), slot 2. GetEquippedItemID's native (+0x2FA0C20) is the same call with slot 0. GetExplosiveEquipmentType and Name
        // are not it: on the player they answer -1 and nothing (simulator 2 Nov 23:58).
        using ThrSlotFn = uint64_t(__fastcall*)(uint8_t* inv, int slot);
        constexpr int         kThrSlots = 6;
        bool                  g_thr_tried = false, g_thr_ok = false;
        std::atomic<bool>     g_thr_on{ false };
        std::atomic<int64_t>  g_thr_ask_t{ 0 };
        std::atomic<uint64_t> g_thr_item{ 0 };
        uint64_t              g_thr_said[kThrSlots]{};

        int thr_read(uint64_t me, uint64_t* out)
        {
            __try {
                uint8_t* h = nullptr;
                reinterpret_cast<AmLookupFn>(g_base + 0xB27D60)(*reinterpret_cast<void**>(g_base + 0x4FAC1E0), &h, me);
                if (!h) return 0;
                int      r   = 0;
                uint8_t* ent = *reinterpret_cast<uint8_t**>(h + 0x10);
                if (ent) {
                    uint8_t* inv  = nullptr;
                    uint8_t* comp = *reinterpret_cast<uint8_t**>(ent + 0xC8);
                    if (comp) {
                        inv = reinterpret_cast<AmInvFn>(g_base + 0xF48EE0)(comp, ent + 0xA8, 0);
                    } else {
                        uint32_t key = *reinterpret_cast<uint32_t*>(reinterpret_cast<AmKeyFn>(g_base + 0xF64790)() + 0x18);
                        inv          = reinterpret_cast<AmFindFn>(g_base + 0xCCD250)(ent, &key);
                    }
                    if (inv) {
                        ThrSlotFn fn = (*reinterpret_cast<ThrSlotFn**>(inv))[0x2C8 / 8];
                        for (int i = 0; i < kThrSlots; ++i) out[i] = fn(inv, i);
                        r = 1;
                    }
                }
                if (h != g_base + 0x4CB9E70 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(h + 8)) == 0) reinterpret_cast<AmReleaseFn>(g_base + 0x26E320)(h);
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // slotcarried (7 Nov; author 20:51; research\2026-11-07_slotforget):
        // does the player still carry an item, asked the way CheckPawnWeaponInventoryByDescriptorId's native (+0x2F95B40) asks it: the inventory (the route of
        // am_read, +0xF48EE0) vtable +0x130 (inventory, item id) gives the item's entry, null when the player does not have it (the native then answers 0; with an
        // entry, 1 for a weapon and 2 for another item). Read statically from FC_m64.dll (the call at +0x2F95BFE), not run yet. Asked every 150 ms on the script
        // thread for the items the shoulder slots remember (carried_watch, at most kCarry), and weaponcache's kept sling and hip weapons (carried_watch_kept,
        // at most kCarryKept, in the slots after them).
        using CarryFn = uint8_t*(__fastcall*)(uint8_t* inv, uint64_t item);
        constexpr int         kCarryShoulders = 8, kCarryKept = 4, kCarry = kCarryShoulders + kCarryKept;
        bool                  g_carry_tried = false, g_carry_ok = false;
        std::atomic<bool>     g_carry_failed{ false };
        std::atomic<uint64_t> g_carry_ask[kCarry]{};    // the items watched (0: none)
        std::atomic<uint64_t> g_carry_item[kCarry]{};   // the item each answer is for
        std::atomic<int>      g_carry_ans[kCarry]{};    // 0 not carried, 1 carried
        std::atomic<int64_t>  g_carry_t{ 0 }, g_carry_ask_t{ 0 };
        uint64_t              g_carry_said_item[kCarry]{};
        int                   g_carry_said[kCarry]{};

        bool carry_on()
        {
            if (!g_carry_ok || g_carry_failed.load()) return false;
            for (int i = 0; i < kCarry; ++i)
                if (g_carry_ask[i].load() != 0) return true;
            return false;
        }

        int carry_read(uint64_t me, const uint64_t* items, int* out)
        {
            __try {
                uint8_t* h = nullptr;
                reinterpret_cast<AmLookupFn>(g_base + 0xB27D60)(*reinterpret_cast<void**>(g_base + 0x4FAC1E0), &h, me);
                if (!h) return 0;
                int      r   = 0;
                uint8_t* ent = *reinterpret_cast<uint8_t**>(h + 0x10);
                if (ent) {
                    uint8_t* inv  = nullptr;
                    uint8_t* comp = *reinterpret_cast<uint8_t**>(ent + 0xC8);
                    if (comp) {
                        inv = reinterpret_cast<AmInvFn>(g_base + 0xF48EE0)(comp, ent + 0xA8, 0);
                    } else {
                        uint32_t key = *reinterpret_cast<uint32_t*>(reinterpret_cast<AmKeyFn>(g_base + 0xF64790)() + 0x18);
                        inv          = reinterpret_cast<AmFindFn>(g_base + 0xCCD250)(ent, &key);
                    }
                    if (inv) {
                        CarryFn fn = (*reinterpret_cast<CarryFn**>(inv))[0x130 / 8];
                        for (int i = 0; i < kCarry; ++i) out[i] = items[i] != 0 && items[i] != ~0ull && fn(inv, items[i]) != nullptr ? 1 : 0;
                        r = 1;
                    }
                }
                if (h != g_base + 0x4CB9E70 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(h + 8)) == 0) reinterpret_cast<AmReleaseFn>(g_base + 0x26E320)(h);
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        void carry_tick()
        {
            g_carry_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            uint64_t items[kCarry]{};
            int      ans[kCarry]{};
            for (int i = 0; i < kCarry; ++i) items[i] = g_carry_ask[i].load();
            const int r = carry_read(me, items, ans);
            if (r < 0) {
                g_carry_failed = true;
                spdlog::error("[GameWeapon] slotcarried: a fault asking the inventory for an item: turned off");
                return;
            }
            if (r == 0) return;
            for (int i = 0; i < kCarry; ++i) {
                g_carry_item[i] = items[i];
                g_carry_ans[i]  = ans[i];
                if (items[i] != 0 && (items[i] != g_carry_said_item[i] || ans[i] != g_carry_said[i]))
                    spdlog::info("[GameWeapon] slotcarried: item {:x} {}", items[i], ans[i] ? "carried" : "NOT carried (the inventory has no entry for it)");
                g_carry_said_item[i] = items[i];
                g_carry_said[i]      = ans[i];
            }
            g_carry_t = now_ms();
        }

        // gamedraw <item hex> | gameholster (tests, 3 Nov; research\2026-11-02_throwables.md): the game's own weapon change, with no pad. DrawWeapon's local path (+0x19C7CD0, from the binding's native +0x2F6F8F0),
        // for the player: with +0x1BB5140 (the component +0xEE40F0 (comp, entity + 0xA8, 0)) true, inventory vtable +0x3E8 (inventory, item), else +0x398 (inventory, item, true). With no name it draws slot 5, then 6.
        // gameholster: Holster's native (+0x2FA6CD0: bool (pawn id*), the game's put-away). On the script thread; a fault turns them off.
        using InvDrawFn   = void(__fastcall*)(uint8_t* inv, uint64_t item, uint8_t flag);
        using InvDraw2Fn  = void(__fastcall*)(uint8_t* inv, uint64_t item);
        using HolsterFn   = uint8_t(__fastcall*)(uint64_t* id);
        using AmIsFn      = uint8_t(__fastcall*)(uint8_t* comp);
        std::atomic<uint64_t> g_draw_item{ 0 };
        std::atomic<bool>     g_draw_holster{ false }, g_draw_failed{ false };
        bool                  g_draw_tried = false, g_draw_ok = false;

        // 0 not usable, 1 by +0x398, 2 by +0x3E8, -1 a fault
        int draw_item(uint64_t me, uint64_t item)
        {
            __try {
                uint8_t* h = nullptr;
                reinterpret_cast<AmLookupFn>(g_base + 0xB27D60)(*reinterpret_cast<void**>(g_base + 0x4FAC1E0), &h, me);
                if (!h) return 0;
                int      r   = 0;
                uint8_t* ent = *reinterpret_cast<uint8_t**>(h + 0x10);
                if (ent) {
                    uint8_t* comp = *reinterpret_cast<uint8_t**>(ent + 0xC8);
                    uint8_t* inv  = nullptr;
                    uint8_t* pawn = nullptr;
                    if (comp) {
                        inv  = reinterpret_cast<AmInvFn>(g_base + 0xF48EE0)(comp, ent + 0xA8, 0);
                        pawn = reinterpret_cast<AmInvFn>(g_base + 0xEE40F0)(comp, ent + 0xA8, 0);
                    } else {
                        uint32_t key = *reinterpret_cast<uint32_t*>(reinterpret_cast<AmKeyFn>(g_base + 0xF64790)() + 0x18);
                        inv          = reinterpret_cast<AmFindFn>(g_base + 0xCCD250)(ent, &key);
                        uint32_t k2  = *reinterpret_cast<uint32_t*>(reinterpret_cast<AmKeyFn>(g_base + 0xEF8990)() + 0x20);
                        pawn         = reinterpret_cast<AmFindFn>(g_base + 0xCCD250)(ent, &k2);
                    }
                    if (inv && pawn) {
                        if (reinterpret_cast<AmIsFn>(g_base + 0x1BB5140)(pawn)) {
                            (*reinterpret_cast<InvDraw2Fn**>(inv))[0x3E8 / 8](inv, item);
                            r = 2;
                        } else {
                            (*reinterpret_cast<InvDrawFn**>(inv))[0x398 / 8](inv, item, 1);
                            r = 1;
                        }
                    }
                }
                if (h != g_base + 0x4CB9E70 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(h + 8)) == 0) reinterpret_cast<AmReleaseFn>(g_base + 0x26E320)(h);
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        int holster_now(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return reinterpret_cast<HolsterFn>(g_base + 0x2FA6CD0)(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        void draw_tick()
        {
            const uint64_t item    = g_draw_item.exchange(0);
            const bool     holster = g_draw_holster.exchange(false);
            if (!g_local_id || g_draw_failed.load()) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            if (holster) {
                const int r = holster_now(me);
                if (r < 0) g_draw_failed = true;
                spdlog::info("[GameWeapon] gameholster: the game's Holster answers {}", r);
            }
            if (item) {
                const int r = draw_item(me, item);
                if (r < 0) g_draw_failed = true;
                spdlog::info("[GameWeapon] gamedraw {:x}: {}", item, r < 0 ? "a fault: turned off" : r == 0 ? "no inventory" : r == 1 ? "asked by the inventory's +0x398" : "asked by the inventory's +0x3E8");
            }
        }

        // spawncommand (author 4 Nov; research\2026-11-04_weapon_spawn.md). The game's Lua natives, called directly, read statically from this build (each takes the entity id by pointer):
        //   AddItem +0x2F90900, RemoveItem +0x2FB3740 (id*, item, count; a negative count: as many as the inventory says); RefillPocketedWeaponClip +0x2FAE850 (id*, item);
        //   GetEntityPositionVec3 +0x2FA0A50 (id*, float3* out); SpawnEntityFromArchetype_Legacy's +0x2FC2A80 (u64* out, archetype, x, y, z, ax, ay, az; degrees; *out -1: not made).
        // The held weapon is dropped by placing its world entity (its data's arkPickupArchetype) and removing the item: only if the entity was made, so a weapon is never just lost.
        // A fault turns it all off. Nothing here has been run yet.
        using SpItemCountFn = void(__fastcall*)(uint64_t* id, uint64_t item, int count);
        using SpItemFn      = uint8_t(__fastcall*)(uint64_t* id, uint64_t item);
        using SpPosFn       = void(__fastcall*)(uint64_t* id, float* out);
        using SpSpawnFn     = uint64_t*(__fastcall*)(uint64_t* out, uint64_t archetype, float x, float y, float z, float ax, float ay, float az);
        struct SpReq { uint64_t item, archetype; float fx, fy; float dist = 1.5f; bool veh = false, get_in = false; float yaw_add = 0.0f; };   // veh, dist, get_in: vehiclespawn; yaw_add: spawnyaw
        std::mutex            g_sp_mx;
        SpReq                 g_sp{};
        std::string           g_sp_status;
        std::atomic<bool>     g_sp_pending{ false }, g_sp_failed{ false };
        bool                  g_sp_tried = false, g_sp_ok = false;
        std::atomic<uint64_t> g_sp_want{ 0 };   // the item being drawn
        int                   g_sp_tries = 0;
        int64_t               g_sp_next  = 0;

        void sp_say(const std::string& s)
        {
            spdlog::info("[Spawn] {}", s);
            std::lock_guard _{ g_sp_mx };
            g_sp_status = s;
        }

        bool sp_item_count(size_t rva, uint64_t me, uint64_t item, int count)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<SpItemCountFn>(g_base + rva)(&id, item, count);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool sp_refill(uint64_t me, uint64_t item)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<SpItemFn>(g_base + 0x2FAE850)(&id, item);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool sp_pos(uint64_t me, float* p)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<SpPosFn>(g_base + 0x2FA0A50)(&id, p);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        // false: a fault. id: the entity, 0 if the game made none.
        // yaw (vehiclespawn): degrees about the up axis, the native's third angle (it scales all three by pi/180, +0x412C5C8, and sets them as the entity's angles).
        bool sp_make(uint64_t archetype, float x, float y, float z, uint64_t& id, float yaw = 0.0f)
        {
            __try {
                uint64_t  out = 0;
                uint64_t* r   = reinterpret_cast<SpSpawnFn>(g_base + 0x2FC2A80)(&out, archetype, x, y, z, 0.0f, 0.0f, yaw);
                id            = r ? *r : 0;
                if (id == ~0ull) id = 0;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        // An entity on the surface dist metres ahead of the player. False: a fault (reported, all off).
        // turned (spawnfacing, 5 Nov turret thread): the entity's forward turned onto the player's (fx, fy) plus yaw_add degrees, as vs_spawn turns a
        // vehicle (so a mounted gun's handles face the player); else as before (angles 0: a weapon dropped from the hand).
        bool sp_place(uint64_t me, uint64_t archetype, float fx, float fy, float dist, uint64_t& id, bool turned = false, float yaw_add = 0.0f)
        {
            id = 0;
            float p[3]{};
            if (!sp_pos(me, p)) return false;
            const float x = p[0] + fx * dist, y = p[1] + fy * dist;
            float       z = p[2];
            const bool  ground = fc5::gameray::ground_z(x, y, p[2] + 1.5f, z);
            const float yaw    = turned ? std::remainder(std::atan2(-fx, fy) * 57.29578f + yaw_add, 360.0f) : 0.0f;
            const bool  ok     = sp_make(archetype, x, y, z + 0.05f, id, yaw);
            spdlog::info("[Spawn] entity {:x} at ({:.2f}, {:.2f}, {:.2f}), yaw {:.0f} deg{}, the player at ({:.2f}, {:.2f}, {:.2f}), {}: {}", archetype, x, y, z + 0.05f, yaw,
                         yaw_add != 0.0f ? fmt::format(" (spawnyaw {:+.0f})", yaw_add) : std::string{}, p[0], p[1], p[2], ground ? "the ground found" : "no ground found, the player's height",
                         !ok ? std::string{ "a fault" } : id ? fmt::format("entity {:x} made", id) : std::string{ "not made" });
            return ok;
        }

        void sp_fail(const char* what)
        {
            g_sp_failed = true;
            g_sp_want   = 0;
            sp_say(fmt::format("a fault in {}: the spawn command is off until the game restarts", what));
        }

        // vehiclespawn (author 5 Nov; tools\vehspawn\README.md): F11 Debug > Vehicle spawn.
        // The vehicle is made like a ground spawn (+0x2FC2A80), dist metres ahead, turned the way the player faces, 0.4 m above the ground found under it. Then, if asked,
        // the player is seated in it with the native behind the Lua function ForceSetPlayerInVehicle (binding +0x2F45220 calls +0x2F9D700 as (pawn id*, seat, vehicle id):
        // the pawn's component from 0xEF8990's type as GetPawnVehicle's lookup +0x2F8E610 uses, the vehicle's from 0xEF8CF0's as the vehicle lookup +0x2F8E8C0 does).
        // The seat given is the vehicle's driver seat (+0x1C73890 on vehicle + 0x20, counted from 0 as vehscript reads it). Seated is checked with GetPawnVehicle.
        // "Remove": RemoveEntity's native (binding +0x2F56820 calls +0x2FB3140 (id*) -> +0x19E3290(id, 0)) on the vehicles spawned this session, not the one the player is in.
        // All read statically from this build's FC_m64.dll (5 Nov), never run.
        using VsSeatFn   = void(__fastcall*)(uint64_t* pawn, int seat, uint64_t vehicle);
        using VsRemoveFn = void(__fastcall*)(uint64_t* id);
        using VsInFn     = uint64_t*(__fastcall*)(uint64_t* id, uint64_t* out);   // GetPawnVehicle's native (+0x2FA2C30)
        using VsSeatOfFn = int(__fastcall*)(uint64_t* id);                         // GetPawnVehicleSeat's native (+0x2FA2C80), from 1
        using VsObjFn    = uint8_t*(__fastcall*)(uint64_t id);                     // the vehicle from its id (+0x2F8E8C0)
        using VsDriverFn = int(__fastcall*)(uint8_t* seats);                       // its driver seat index (+0x1C73890, vehicle + 0x20)
        bool                  g_vs_tried = false, g_vs_seat_ok = false, g_vs_remove_ok = false;
        std::atomic<bool>     g_vs_remove{ false };
        std::atomic<uint64_t> g_vs_enter{ 0 };   // the vehicle to seat the player in
        int                   g_vs_tries = 0, g_vs_calls = 0;
        int64_t               g_vs_next  = 0;
        int                   g_vs_leave = 0;    // spawned from inside another vehicle: 1 the game's exit (pad X) held, 2 out (waiting to seat), 0 not needed
        int64_t               g_vs_leave_t0 = 0;
        std::vector<uint64_t> g_vs_made;         // the vehicles spawned this session (script thread only)
        std::mutex            g_vs_arch_mx;      // drive: each spawned vehicle's archetype, by entity (read from the pad poll)
        std::vector<std::pair<uint64_t, uint64_t>> g_vs_arch;

        // The player's vehicle (in: -1 or 0 none) and seat (from 1); false: a fault.
        bool vs_in(uint64_t me, uint64_t& in, int& seat)
        {
            __try {
                uint64_t id = me, out = ~0ull;
                reinterpret_cast<VsInFn>(g_base + 0x2FA2C30)(&id, &out);
                in   = out;
                seat = -1;
                if (out != ~0ull && out != 0) {
                    id   = me;
                    seat = reinterpret_cast<VsSeatOfFn>(g_base + 0x2FA2C80)(&id);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        // Whether the game has the vehicle object yet, and its driver seat (from 0; -1 none). False: a fault.
        bool vs_obj(uint64_t veh, bool& is_vehicle, int& dseat)
        {
            __try {
                uint8_t* v = reinterpret_cast<VsObjFn>(g_base + 0x2F8E8C0)(veh);
                is_vehicle = v != nullptr;
                dseat      = v ? reinterpret_cast<VsDriverFn>(g_base + 0x1C73890)(v + 0x20) : -1;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool vs_seat(uint64_t me, int seat, uint64_t veh)
        {
            __try {
                uint64_t id = me;
                reinterpret_cast<VsSeatFn>(g_base + 0x2F9D700)(&id, seat, veh);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        bool vs_remove(uint64_t veh)
        {
            __try {
                uint64_t id = veh;
                reinterpret_cast<VsRemoveFn>(g_base + 0x2FB3140)(&id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void vs_spawn(uint64_t me, const SpReq& q)
        {
            float p[3]{};
            if (!sp_pos(me, p)) { sp_fail("the player's position"); return; }
            const float x = p[0] + q.fx * q.dist, y = p[1] + q.fy * q.dist;
            float       z = p[2];
            const bool  ground = fc5::gameray::ground_z(x, y, p[2] + 3.0f, z);   // (8 m down from 3 m above the player's feet)
            // The entity's forward (+y) turned onto the player's (fx, fy), counter-clockwise from above: a guess at the game's convention, the log says what it got.
            const float yaw = std::atan2(-q.fx, q.fy) * 57.29578f;
            uint64_t    id  = 0;
            if (!sp_make(q.archetype, x, y, z + 0.4f, id, yaw)) { sp_fail("the game's spawn"); return; }
            spdlog::info("[Spawn] vehicle {:x} at ({:.2f}, {:.2f}, {:.2f}), yaw {:.0f} deg, the player at ({:.2f}, {:.2f}, {:.2f}), {}: {}", q.archetype, x, y, z + 0.4f, yaw, p[0], p[1], p[2],
                         ground ? "the ground found" : "no ground found, the player's height", id ? fmt::format("entity {:x} made", id) : std::string{ "not made" });
            if (id == 0) {
                sp_say(fmt::format("the game made no vehicle for {:x} (perhaps it is not loaded here)", q.archetype));
                return;
            }
            g_vs_made.push_back(id);
            {
                std::lock_guard _{ g_vs_arch_mx };
                g_vs_arch.emplace_back(id, q.archetype);
            }
            if (q.get_in && g_vs_seat_ok) {
                g_vs_tries = 0;
                g_vs_calls = 0;
                g_vs_leave = 0;
                g_vs_next  = now_ms() + 500;   // (its components a few updates to come)
                g_vs_enter = id;
                sp_say(fmt::format("vehicle {:x} placed {:.0f} m ahead (id {:x}); seating you in it", q.archetype, q.dist, id));
            } else {
                sp_say(fmt::format("vehicle {:x} placed {:.0f} m ahead (id {:x}){}", q.archetype, q.dist, id, q.get_in ? "; seating not used: unexpected code at the game's seat calls" : ""));
            }
        }

        // The seat (every 0.5-1.5 s, at most 2 asks and 8 checks) and the removal, on the script thread.
        void vs_tick(uint64_t me)
        {
            if (g_vs_remove.exchange(false)) {
                uint64_t in   = ~0ull;
                int      seat = -1;
                if (!vs_in(me, in, seat)) { sp_fail("GetPawnVehicle"); return; }
                std::vector<uint64_t> kept;
                int                   gone = 0;
                for (const uint64_t v : g_vs_made) {
                    if (v == in) { kept.push_back(v); continue; }
                    if (!vs_remove(v)) { sp_fail("RemoveEntity"); return; }
                    if (g_vs_enter.load() == v) g_vs_enter = 0;
                    ++gone;
                }
                g_vs_made = kept;
                sp_say(fmt::format("{} spawned vehicle{} removed{}", gone, gone == 1 ? "" : "s", kept.empty() ? "" : "; the one you are in is kept"));
            }
            const uint64_t want = g_vs_enter.load();
            if (want == 0 || now_ms() < g_vs_next) return;
            uint64_t in   = ~0ull;
            int      seat = -1, dseat = -1;
            bool     is_veh = false;
            if (!vs_in(me, in, seat) || !vs_obj(want, is_veh, dseat)) { sp_fail("the vehicle reads"); return; }
            if (in == want) {
                sp_say(fmt::format("in vehicle {:x}: seat {} (counted from 1; the driver's is {} counted from 0)", want, seat, dseat));
                g_vs_enter = 0;
                return;
            }
            // Spawned from inside another vehicle (drive tests: car, then quad, then helicopter): ForceSetPlayerInVehicle does not move a seated
            // player (builder 5 Nov: "placed, but you are not in it", asked 2 times). Get out first with the game's own exit, pad X held through
            // padtap (let go as soon as the game says the player is out, so a held X does not get back in), then seat from the ground.
            const bool in_other = in != ~0ull && in != 0;
            if (in_other && g_vs_leave == 0 && g_vs_calls == 0) {
                std::istringstream ms{ "2500" };
                bool               persist = false;
                fc5::gesture::command("padtap", "4000", 0.0f, ms, persist);
                g_vs_leave    = 1;
                g_vs_leave_t0 = now_ms();
                spdlog::info("[Spawn] vehicle {:x}: you are in vehicle {:x} (seat {}): the game's exit (pad X) held to get out first", want, in, seat);
                sp_say(fmt::format("vehicle {:x} placed; getting you out of vehicle {:x} first", want, in));
                g_vs_next = now_ms() + 100;
                return;
            }
            if (g_vs_leave == 1) {
                const int64_t t = now_ms() - g_vs_leave_t0;
                if (!in_other) {
                    std::istringstream ms{ "10" };
                    bool               persist = false;
                    fc5::gesture::command("padtap", "0", 0.0f, ms, persist);   // X let go
                    g_vs_leave = 2;
                    spdlog::info("[Spawn] vehicle {:x}: out of the vehicle after {} ms of pad X; seating in 1.5 s", want, t);
                    g_vs_next = now_ms() + 1500;   // (the exit animation)
                    return;
                }
                if (t > 2600) {
                    g_vs_leave = 0;
                    g_vs_enter = 0;
                    sp_say(fmt::format("vehicle {:x} placed, but you are still in vehicle {:x} after 2.5 s of pad X (get out yourself, then use spawnvehiclein again)", want, in));
                    return;
                }
                g_vs_next = now_ms() + 100;
                return;
            }
            if (++g_vs_tries > 8 || (g_vs_calls >= 2 && g_vs_tries > 4)) {
                sp_say(fmt::format("vehicle {:x} placed, but you are not in it ({}; the game says you are in {:x})", want,
                                   !is_veh ? "the game has no vehicle object for it" : fmt::format("asked {} times", g_vs_calls), in));
                g_vs_enter = 0;
                return;
            }
            if (is_veh && g_vs_calls < 2) {
                const int s = dseat >= 0 ? dseat : 0;
                if (!vs_seat(me, s, want)) { sp_fail("ForceSetPlayerInVehicle"); return; }
                ++g_vs_calls;
                spdlog::info("[Spawn] vehicle {:x}: ForceSetPlayerInVehicle asked, seat {} (the driver's seat {})", want, s, dseat);
                g_vs_next = now_ms() + 1500;   // (any entry animation)
            } else {
                g_vs_next = now_ms() + 500;
            }
        }

        void spawn_tick()
        {
            if (!g_local_id || g_sp_failed.load()) { g_sp_pending = false; g_sp_want = 0; g_vs_enter = 0; g_vs_remove = false; return; }
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            if (g_vs_enter.load() != 0 || g_vs_remove.load()) {   // vehiclespawn
                vs_tick(me);
                if (g_sp_failed.load()) return;
            }
            if (g_sp_pending.exchange(false)) {
                SpReq q;
                {
                    std::lock_guard _{ g_sp_mx };
                    q = g_sp;
                }
                if (q.archetype != 0 && q.veh) {   // vehiclespawn
                    vs_spawn(me, q);
                } else if (q.archetype != 0) {
                    uint64_t id = 0;
                    if (!sp_place(me, q.archetype, q.fx, q.fy, 1.5f, id, true, q.yaw_add)) { sp_fail("the game's spawn"); return; }   // spawnfacing: turned to the player's heading
                    sp_say(id ? fmt::format("entity {:x} placed on the ground ahead: id {:x}", q.archetype, id) : fmt::format("the game made no entity for {:x} (perhaps it is not loaded here)", q.archetype));
                }
                if (q.item != 0) {
                    uint64_t cur = 0;
                    if (g_eq_fn && !eq_read(me, cur)) cur = 0;
                    if (cur == ~0ull) cur = 0;
                    std::string dropped = "nothing held to drop";
                    if (cur == q.item) {
                        dropped = "already held";
                    } else if (cur != 0) {
                        const uint64_t pick = fc5::spawn::pickup_of(cur);
                        uint64_t       id   = 0;
                        if (pick != 0 && !sp_place(me, pick, q.fx, q.fy, 0.6f, id)) { sp_fail("the game's spawn"); return; }
                        if (id != 0) {
                            if (!sp_item_count(0x2FB3740, me, cur, -1)) { sp_fail("RemoveItem"); return; }
                            dropped = fmt::format("{:x} dropped", cur);
                        } else {
                            dropped = fmt::format("{:x} kept: {}", cur, pick ? "the game made no entity for it" : "it has no world entity");
                        }
                    }
                    if (!sp_item_count(0x2F90900, me, q.item, 1)) { sp_fail("AddItem"); return; }
                    if (!sp_refill(me, q.item)) { sp_fail("the clip's refill"); return; }
                    const int r = draw_item(me, q.item);
                    if (r < 0) { sp_fail("the draw"); return; }
                    sp_say(fmt::format("item {:x} added and asked for ({}); {}", q.item, r == 0 ? "no inventory" : "drawing", dropped));
                    g_sp_want  = q.item;
                    g_sp_tries = 0;
                    g_sp_next  = now_ms() + 700;
                }
                return;
            }
            const uint64_t want = g_sp_want.load();
            if (want == 0 || now_ms() < g_sp_next) return;
            uint64_t cur = 0;
            if (g_eq_fn && !eq_read(me, cur)) cur = 0;
            if (cur == want) {
                sp_say(fmt::format("item {:x} in the hand", want));
                g_sp_want = 0;
            } else if (++g_sp_tries > 4) {
                sp_say(fmt::format("item {:x} added, but the game holds {:x} after 5 tries", want, cur));
                g_sp_want = 0;
            } else {
                const int r = draw_item(me, want);
                if (r < 0) { sp_fail("the draw"); return; }
                g_sp_next = now_ms() + 700;
            }
        }

        void thr_tick()
        {
            g_thr_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            uint64_t   slots[kThrSlots]{};
            const int  r = thr_read(me, slots);
            if (r < 0) {
                g_thr_on = false;
                spdlog::error("[Throwable] a fault reading the inventory's slots: turned off");
                return;
            }
            if (r == 0) return;
            if (std::memcmp(slots, g_thr_said, sizeof(slots)) != 0) {
                std::memcpy(g_thr_said, slots, sizeof(slots));
                spdlog::info("[Throwable] the inventory's slots: 0 {:x}, 1 {:x}, 2 (the selected throwable) {:x}, 3 {:x}, 4 {:x}, 5 {:x}", slots[0], slots[1], slots[2], slots[3], slots[4], slots[5]);
            }
            g_thr_item = slots[2] == ~0ull ? 0 : slots[2];
        }

        void eq_tick()
        {
            g_eq_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            uint64_t item = 0;
            if (!eq_read(me, item)) {
                g_eq_on     = false;
                g_eq_failed = true;
                spdlog::error("[Weapon] gameweapon: a fault reading GetEquippedItemID for the player {:x}: turned off (the mod's own measures decide)", me);
                return;
            }
            if (item == ~0ull) item = 0;
            if (item != g_eq_item.load()) spdlog::info("[Weapon] gameweapon: the game says the player holds item {:x}", item);
            g_eq_item = item;
            g_eq_t    = now_ms();
        }

        // gamestatebones (log only, test; research\2026-11-01_weapon_data.md): once a second with gamestate on, where the game puts the
        // player's body bones, the attach points its weapon data names (holster_*, righthand_*: research\weapondata\weapons.csv) and the
        // equipped item's sight bones (IRONSIGHT, CROSSHAIR, IRON_SIGHT), from GetBonePos's native (+0x2F9EBC0: (entity id*, float3* out,
        // CStringID* = CRC-32 of the name) -> world position, (0,0,0) for a bone the entity has not) and GetEquippedItemID's (+0x2FA0C20:
        // (pawn id*) -> item id), relative to the player entity's position (GetEntityPosition's native +0x2FA09C0). Decides whether the
        // sling, the holsters and the sight lines can come from the game.
        using BonePosFn  = float*(__fastcall*)(uint64_t* id, float* out, uint32_t* hash);
        using EquippedFn = uint64_t(__fastcall*)(uint64_t* id);
        using EntPosFn   = float(__fastcall*)(uint64_t* id, int axis);
        BonePosFn         g_gsb_bone  = nullptr;
        EquippedFn        g_gsb_equip = nullptr;
        EntPosFn          g_gsb_pos   = nullptr;
        std::atomic<bool> g_gsb_on{ false };
        bool              g_gsb_ok    = false;
        int64_t           g_gsb_t     = 0;
        uint64_t          g_gsb_item  = 0;
        constexpr const char* kBodyPoints[]{ "Head", "Neck", "Spine2", "Spine1", "Hips", "LeftShoulder", "RightShoulder", "LeftHand", "RightHand",
                                             "holster_pistol", "holster_rifle", "holster_rifle_multi", "holster_bow", "holster_machete",
                                             "holster_onehand", "righthand_rifles", "righthand_pistols", "riflestrap_primary" };
        constexpr const char* kItemPoints[]{ "IRONSIGHT", "CROSSHAIR", "IRON_SIGHT" };
        constexpr int         kBodyN = int(sizeof(kBodyPoints) / sizeof(kBodyPoints[0])), kItemN = int(sizeof(kItemPoints) / sizeof(kItemPoints[0]));

        uint32_t crc32_name(const char* s)
        {
            uint32_t c = 0xFFFFFFFFu;
            for (; *s; ++s) {
                c ^= uint8_t(*s);
                for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
            return ~c;
        }

        // Under SEH (no C++ objects): body[kBodyN][3], item[kItemN][3] world positions, origin[3]; false on a fault.
        bool gsb_read(uint64_t me, float (*body)[3], float (*item)[3], float* origin, uint64_t& item_id)
        {
            __try {
                uint64_t id = me;
                for (int a = 0; a < 3; ++a) { id = me; origin[a] = g_gsb_pos(&id, a); }
                for (int i = 0; i < kBodyN; ++i) {
                    uint32_t h = crc32_name(kBodyPoints[i]);
                    id = me;
                    g_gsb_bone(&id, body[i], &h);
                }
                id      = me;
                item_id = g_gsb_equip(&id);
                for (int i = 0; i < kItemN; ++i) {
                    item[i][0] = item[i][1] = item[i][2] = 0.0f;
                    if (item_id == 0 || item_id == ~0ull) continue;
                    uint32_t h  = crc32_name(kItemPoints[i]);
                    uint64_t it = item_id;
                    g_gsb_bone(&it, item[i], &h);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void gsb_tick(uint64_t me)
        {
            g_gsb_t = now_ms();
            if (!g_gsb_ok) return;
            float    body[kBodyN][3]{}, item[kItemN][3]{}, o[3]{};
            uint64_t item_id = 0;
            if (!gsb_read(me, body, item, o, item_id)) {
                g_gsb_on = false;
                g_gsb_ok = false;
                spdlog::error("[GameState] gamestatebones: a fault reading bone positions: turned off");
                return;
            }
            auto rel = [&](const float* p) {
                if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) return std::string{ "-" };
                return fmt::format("({:+.2f}, {:+.2f}, {:+.2f})", p[0] - o[0], p[1] - o[1], p[2] - o[2]);
            };
            std::string b, it;
            for (int i = 0; i < kBodyN; ++i) b += fmt::format(" {} {}", kBodyPoints[i], rel(body[i]));
            for (int i = 0; i < kItemN; ++i) it += fmt::format(" {} {}", kItemPoints[i], rel(item[i]));
            spdlog::info("[GameState] bones from the player at ({:.2f}, {:.2f}, {:.2f}):{}; equipped item {:x}{}:{}", o[0], o[1], o[2], b, item_id,
                         item_id != g_gsb_item ? " (new)" : "", it);
            g_gsb_item = item_id;
        }

        // bonemapbody (FarCry5BoneMap.h; debug, not saved): every 100 ms while asked, the player's body bones as GetBonePos gives them
        // (world positions), for the bone map's magenta dots. A name the skeleton has not comes back as the entity's origin: left out.
        constexpr const char* kBmBody[]{ "Root", "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "Camera",
                                         "LeftShoulder", "LeftArm", "LeftArmTwistA", "LeftElbow", "LeftForeArm", "LeftForeArmTwistB", "LeftHand",
                                         "LeftHandThumb1", "LeftHandIndex1", "LeftHandMiddle1", "LeftHandPinky1",
                                         "RightShoulder", "RightArm", "RightArmTwistA", "RightElbow", "RightForeArm", "RightForeArmTwistB", "RightHand",
                                         "RightHandThumb1", "RightHandIndex1", "RightHandMiddle1", "RightHandPinky1", "RightHand_GunPlacement",
                                         "LeftUpLeg", "LeftLeg", "LeftKnee", "LeftFoot", "LeftToe", "RightUpLeg", "RightLeg", "RightKnee", "RightFoot", "RightToe",
                                         "Attachment", "AttachmentPivot", "IK_RightHand", "IK_LeftHand",
                                         "holster_pistol", "holster_rifle", "holster_rifle_multi", "holster_bow", "holster_machete", "holster_onehand",
                                         "righthand_rifles", "righthand_pistols", "riflestrap_primary" };
        constexpr int     kBmBodyN = int(sizeof(kBmBody) / sizeof(kBmBody[0]));
        std::atomic<bool> g_bmb_on{ false };
        int64_t           g_bmb_t    = 0;
        std::mutex        g_bmb_mtx;
        float             g_bmb_pos[kBmBodyN][3]{};
        float             g_bmb_origin[3]{};
        int64_t           g_bmb_at   = 0;   // when they were read (0: never)
        uint64_t          g_bmb_reads = 0;

        // Under SEH (no C++ objects): out[kBmBodyN][3] world positions and the entity's origin; false on a fault.
        bool bmb_read(uint64_t me, float (*out)[3], float* origin)
        {
            __try {
                uint64_t id = me;
                for (int a = 0; a < 3; ++a) { id = me; origin[a] = g_gsb_pos(&id, a); }
                for (int i = 0; i < kBmBodyN; ++i) {
                    uint32_t h = crc32_name(kBmBody[i]);
                    id         = me;
                    out[i][0] = out[i][1] = out[i][2] = 0.0f;
                    g_gsb_bone(&id, out[i], &h);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void bmb_tick()
        {
            g_bmb_t = now_ms();
            if (!g_gsb_ok || !g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            float pos[kBmBodyN][3]{}, o[3]{};
            if (!bmb_read(me, pos, o)) {
                g_bmb_on = false;
                spdlog::error("[BoneMap] bonemapbody: a fault reading the player's bone positions: turned off");
                return;
            }
            std::lock_guard _{ g_bmb_mtx };
            std::memcpy(g_bmb_pos, pos, sizeof(pos));
            std::memcpy(g_bmb_origin, o, sizeof(o));
            g_bmb_at = now_ms();
            if (g_bmb_reads++ == 0) {
                int found = 0;
                for (int i = 0; i < kBmBodyN; ++i)
                    if (std::abs(pos[i][0] - o[0]) + std::abs(pos[i][1] - o[1]) + std::abs(pos[i][2] - o[2]) > 0.002f) found++;
                spdlog::info("[BoneMap] bonemapbody: first read: {} of {} names are bones of the player's skeleton", found, kBmBodyN);
            }
        }

        // On the game's script thread, every 100 ms: the player's state as the game's own script functions give it; a line on each change.
        void gs_tick()
        {
            g_gs_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            GameState s{};
            if (!gs_read(me, s)) {
                g_gs_on     = false;
                g_gs_failed = true;
                spdlog::error("[GameState] a fault reading the player's state ({:x}): gamestate turned off", me);
                return;
            }
            if (g_gs_cam_ok) {
                s.cam_vt = gs_cam_read(g_base);
                if (s.cam_vt == -2) {
                    g_gs_cam_ok = false;
                    spdlog::error("[GameState] a fault finding the active camera: the camera no longer read (the rest goes on)");
                }
            }
            g_gs_reads++;
            const GameState& p = g_gs_last;
            std::string what;
            auto add = [&](bool changed, const char* name) { if (changed) what += what.empty() ? name : fmt::format(", {}", name); };
            add(!g_gs_have, "first read");
            add(g_gs_have && (s.veh != p.veh || s.seat != p.seat || s.dseat != p.dseat), "vehicle");
            add(g_gs_have && s.vtype != p.vtype, "vehicle type");
            add(g_gs_have && s.ride != p.ride, "riding");
            add(g_gs_have && s.stance != p.stance, "stance");
            add(g_gs_have && s.qte != p.qte, "QTE");
            add(g_gs_have && s.down != p.down, "down");
            add(g_gs_have && s.dead != p.dead, "dead");
            add(g_gs_have && s.air != p.air, "in air");
            add(g_gs_have && s.water != p.water, "in water");
            add(g_gs_have && (std::abs(s.fov - p.fov) > 0.05f || std::abs(s.nearfov - p.nearfov) > 0.05f), "FOV");
            add(g_gs_have && s.cam_vt != p.cam_vt, "camera");
            if (!what.empty()) {
                g_gs_changes++;
                spdlog::info("[GameState] {}: {}", what, gs_text(s));
            }
            g_gs_last = s;
            g_gs_have = true;
            if (g_gsb_on.load() && now_ms() - g_gsb_t >= 1000) gsb_tick(me);
        }

        // vehscript: the reads, under SEH (no C++ objects here): false on a fault.
        bool veh_read(uint64_t me, uint64_t& veh, int& seat, int& dseat, int& type)
        {
            __try {
                uint64_t id = me, out = ~0ull;
                g_pawn_veh(&id, &out);
                veh   = out;
                seat  = -1;
                dseat = -1;
                type  = -1;
                if (out != ~0ull && out != 0) {
                    id   = me;
                    seat = g_pawn_seat(&id);
                    id   = me;
                    type = g_pawn_type(&id);
                    if (uint8_t* v = g_veh_obj(out)) dseat = g_driver_seat(v + 0x20);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // GetPawnStance, under SEH: -2 a fault.
        int stance_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_stance_fn(&id);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // chutemin: IsInAir, under SEH: 1, 0, -2 a fault.
        int air_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_air_fn(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // downview: IsPawnDown, under SEH: 1 down, 0 not, -2 a fault.
        int down_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_down_fn(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // qtehud: IsInQTE, under SEH: 1 in a QTE, 0 not, -2 a fault.
        int qte_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_qte_fn(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // On the game's script thread: GetPawnVehicle for the local player; every change logged (downview: IsPawnDown too).
        void veh_tick()
        {
            g_veh_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            if (g_down_on.load()) {
                const int d = down_read(me);
                if (d == -2) {
                    g_down_on     = false;
                    g_down_failed = true;
                    g_down_state  = -1;
                    spdlog::error("[Down] downview: a fault reading IsPawnDown for the player {:x}: downview off for this session", me);
                } else {
                    g_down_state = d;
                    g_down_t     = now_ms();
                }
            }
            if (g_qte_on.load()) {
                const int q = qte_read(me);
                if (q == -2) {
                    g_qte_on     = false;
                    g_qte_failed = true;
                    g_qte_state  = -1;
                    spdlog::error("[HUD] qtehud: a fault reading IsInQTE for the player {:x}: QTEs no longer read this session (knocked down still is)", me);
                } else {
                    g_qte_state = q;
                    g_qte_t     = now_ms();
                }
            }
            if (!g_veh_on.load()) return;
            if (g_stance_fn) {
                const int s = stance_read(me);
                if (s == -2) {
                    g_stance_fn = nullptr;
                    g_stance    = -1;
                    spdlog::error("[Vehicle] vehscript: a fault reading GetPawnStance for the player {:x}: the stance no longer read", me);
                } else {
                    g_stance   = s;
                    g_stance_t = now_ms();
                }
            }
            if (g_air_fn) {
                const int a = air_read(me);
                if (a == -2) {
                    g_air_fn = nullptr;
                    g_air    = -1;
                    spdlog::error("[Vehicle] vehscript: a fault reading IsInAir for the player {:x}: in the air no longer read (chutemin falls back to its timing rule)", me);
                } else {
                    g_air   = a;
                    g_air_t = now_ms();
                }
            }
            uint64_t veh = ~0ull;
            int      seat = -1, dseat = -1, type = -1;
            if (!veh_read(me, veh, seat, dseat, type)) {
                g_veh_on     = false;
                g_veh_failed = true;
                g_veh_state  = -1;
                spdlog::error("[Vehicle] vehscript: a fault reading GetPawnVehicle for the player {:x}: turned off (the mod's own rules decide)", me);
                return;
            }
            const int st  = (veh != ~0ull && veh != 0) ? 1 : 0;
            // GetPawnVehicleSeat counts from 1 (0 during the ~2 s entry animation), the driver's seat lookup from 0 (research\2026-10-30_engine_natives.md,
            // The author's 09:27 log: quad driver 1, heli pilot 1, heli passenger 3; the old seat == dseat called the driver a passenger). Seat 0: no answer (-1).
            const int drv = st ? (seat >= 1 ? ((seat - 1 == dseat) ? 1 : 0) : -1) : 0;
            g_veh_queries++;
            g_veh_t = now_ms();
            // vehdrivegun (30 Oct): a change of the seat or the driver's seat alone is logged too (both are under question).
            if (st != g_veh_state.load() || (st && (drv != g_veh_driver.load() || veh != g_veh_id.load() || type != g_veh_type.load() || seat != g_veh_seat.load() ||
                                                    dseat != g_veh_dseat.load())) || g_veh_said < 3) {
                g_veh_said++;
                static const char* kTypes[]{ "wheeled", "boat", "paraglider", "motorcycle", "helicopter", "plane", "wingsuit", "none" };
                if (st) spdlog::info("[Vehicle] vehscript: the game says in a vehicle (entity {:x}, type {} {}, seat {}, driver's seat {}: {})", veh, type,
                                     type >= 0 && type <= 7 ? kTypes[type] : "?", seat, dseat, drv == 1 ? "driver" : drv == 0 ? "passenger" : "getting in (seat 0)");
                else spdlog::info("[Vehicle] vehscript: the game says on foot");
            }
            g_veh_id     = st ? veh : 0;
            g_veh_seat   = seat;
            g_veh_dseat  = dseat;
            g_veh_type   = type;
            g_veh_driver = drv;
            g_veh_state  = st;
        }

        void* __fastcall hk_lookup(void* mgr, void** out, uint64_t id)
        {
            void* r = g_lookup_hook.call<void*>(mgr, out, id);
            if (g_watch.load(std::memory_order_relaxed) && out && id != ~0ull && id != 0) {
                const uint8_t* h = static_cast<const uint8_t*>(*out);
                if (h && h != g_base + 0x4CB9E70 && *reinterpret_cast<void* const*>(h + 0x10)) {
                    const size_t k = size_t(((id ^ (id >> 29)) * 0x9E3779B97F4A7C15ull) >> 53) & (kIds - 1);
                    g_ids[k].store(id, std::memory_order_relaxed);
                    g_id_tick[k].store(g_tick_n.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                g_lookups.fetch_add(1, std::memory_order_relaxed);
            }
            return r;
        }

        // meleedamageprobe (test): the 12 recorded entities nearest a point, with their positions from it.
        void probe_tick()
        {
            g_probe_pending = false;
            if (!g_pos) return;
            const float    px = g_probe_x.load(), py = g_probe_y.load(), pz = g_probe_z.load();
            const uint32_t now = g_tick_n.load();
            struct Near { float d; uint64_t id; float x, y, z; };
            std::vector<Near> v;
            for (int k = 0; k < kIds; ++k) {
                uint64_t id = g_ids[k].load(std::memory_order_relaxed);
                if (id == 0 || now - g_id_tick[k].load(std::memory_order_relaxed) > 120) continue;
                const float x = g_pos(&id, 0) - px, y = g_pos(&id, 1) - py, z = g_pos(&id, 2) - pz;
                v.push_back(Near{ std::sqrt(x * x + y * y + z * z), id, x, y, z });
            }
            std::sort(v.begin(), v.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
            std::string list;
            for (size_t i = 0; i < v.size() && i < 12; ++i) list += fmt::format(" {:x} ({:+.2f}, {:+.2f}, {:+.2f})", v[i].id, v[i].x, v[i].y, v[i].z);
            spdlog::info("[Melee] meleedamageprobe: {} entities recorded in the last 120 updates ({} lookups seen); nearest to ({:.2f}, {:.2f}, {:.2f}):{}", v.size(), g_lookups.load(), px, py,
                         pz, list);
        }

        // On the game's script thread: the recorded entity nearest the body's root (across the ground within 0.7 m, its root within
        // 1.2 m up or down), not the player; then the damage stim from the player to it.
        void damage_tick()
        {
            g_dmg_pending = false;
            if (!g_pos || !g_stim || !g_local_id) return;
            uint64_t       tmp    = 0;
            uint64_t*      idp    = g_local_id(&tmp);
            const uint64_t player = idp ? *idp : 0;
            // meleeray: the hit said who
            if (uint64_t target = g_dmg_id.exchange(0); target != 0) {
                const int64_t ms = now_ms() - g_dmg_t.load();
                g_dmg_found++;
                if (!g_dmg_send.load() || target == player) {
                    spdlog::info("[Melee] meleedamage (find only): entity {:x}, {} ms after the hit", target, ms);
                    return;
                }
                const float h0 = g_health ? g_health(&target) : -1.0f;
                g_stim(&target, player, g_dmg_amount.load());
                const float h1 = g_health ? g_health(&target) : -1.0f;
                g_dmg_sent++;
                g_after_id   = target;
                g_after_tick = g_tick_n.load();
                g_after_h0   = h0;
                spdlog::info("[Melee] meleedamage: {:.0f} damage sent to entity {:x} from the player {:x} ({} ms after the hit); its health {:.1f} -> {:.1f}", g_dmg_amount.load(), target, player, ms, h0, h1);
                return;
            }
            const float    bx = g_dmg_x.load(), by = g_dmg_y.load(), bz = g_dmg_z.load();
            const uint32_t now = g_tick_n.load();
            uint64_t       best   = 0;
            float          best_d = 1e9f, bpx = 0.0f, bpy = 0.0f, bpz = 0.0f;
            int            looked = 0;
            std::string    near_list;
            for (int k = 0; k < kIds; ++k) {
                uint64_t id = g_ids[k].load(std::memory_order_relaxed);
                if (id == 0 || id == player || now - g_id_tick[k].load(std::memory_order_relaxed) > 120) continue;
                looked++;
                const float x = g_pos(&id, 0), y = g_pos(&id, 1), z = g_pos(&id, 2);
                const float dh = std::sqrt((x - bx) * (x - bx) + (y - by) * (y - by)), dz = z - bz;
                if (dh < 3.0f && near_list.size() < 600) near_list += fmt::format(" {:x} ({:+.2f}, {:+.2f}, {:+.2f}) {:.2f} m", id, x - bx, y - by, dz, dh);
                if (dh < 0.7f && std::abs(dz) < 1.2f && dh < best_d) { best = id; best_d = dh; bpx = x; bpy = y; bpz = z; }
            }
            const int64_t ms = now_ms() - g_dmg_t.load();
            if (!best) {
                g_dmg_missed++;
                spdlog::info("[Melee] meleedamage: no entity at the body's root ({:.2f}, {:.2f}, {:.2f}) among {} recorded ({} ms after the hit); within 3 m:{}", bx, by, bz, looked, ms,
                             near_list.empty() ? std::string{ " none" } : near_list);
                return;
            }
            g_dmg_found++;
            if (!g_dmg_send.load()) {
                spdlog::info("[Melee] meleedamage (find only): entity {:x} at ({:.2f}, {:.2f}, {:.2f}), {:.2f} m from the body's root, {} recorded, {} ms after the hit;{}", best, bpx, bpy, bpz,
                             best_d, looked, ms, near_list);
                return;
            }
            uint64_t    target = best;
            const float h0     = g_health ? g_health(&target) : -1.0f;
            g_stim(&target, player, g_dmg_amount.load());
            const float h1 = g_health ? g_health(&target) : -1.0f;
            g_dmg_sent++;
            g_after_id   = best;
            g_after_tick = g_tick_n.load();
            g_after_h0   = h0;
            spdlog::info("[Melee] meleedamage: {:.0f} damage sent to entity {:x} from the player {:x} ({:.2f} m from the body's root, {} ms after the hit); its health {:.1f} -> {:.1f}",
                         g_dmg_amount.load(), best, player, best_d, ms, h0, h1);
        }

        uint8_t* rel_target(uint8_t* at)   // E8 / E9 rel32
        {
            return at + 5 + *reinterpret_cast<int32_t*>(at + 1);
        }

        bool __fastcall hk_exec(void* self, const char* code, bool raw)
        {
            g_game_tid = GetCurrentThreadId();
            g_game_calls++;
            return g_hook.call<bool>(self, code, raw);
        }

        // Inside the game's camera update (the Tobii slot), on the game's own thread.
        void game_tick()
        {
            g_tick_tid = GetCurrentThreadId();
            g_tick_n.fetch_add(1, std::memory_order_relaxed);
            if (g_after_id && g_in_play.load() && g_tick_n.load() - g_after_tick >= 30 && g_game_tid.load() == GetCurrentThreadId()) {
                uint64_t id = g_after_id;
                g_after_id  = 0;
                spdlog::info("[Melee] meleedamage: entity {:x}'s health 30 updates later {:.1f} (before the stim {:.1f})", id, g_health ? g_health(&id) : -1.0f, g_after_h0);
            }
            const bool veh_due = (g_veh_on.load() || g_down_on.load() || g_qte_on.load()) && now_ms() - g_veh_ask_t >= 150;   // vehscript, downview, qtehud
            const bool gs_due  = g_gs_on.load() && now_ms() - g_gs_ask_t >= 100;     // gamestate
            const bool snd_due = fc5::gamesound::due();                              // gamesound
            const bool thr_due = g_thr_on.load() && now_ms() - g_thr_ask_t.load() >= 200;   // throwlibrary
            const bool drw_due = g_draw_item.load() != 0 || g_draw_holster.load();            // gamedraw, gameholster
            const bool sp_due  = g_sp_pending.load() || g_sp_want.load() != 0 || g_vs_enter.load() != 0 || g_vs_remove.load();   // spawncommand, vehiclespawn
            const bool fill_due = g_fill_req.load() || g_take_req.load() > 0;                 // reloadnogame, a rack's lost round
            const bool ray_due = fc5::gameray::due();                                // gameray (meleeray)
            const bool eq_due  = g_eq_on.load() && now_ms() - g_eq_ask_t >= 150;     // gameweapon
            const bool am_due  = g_am_on.load() && (g_am_fast.load() || now_ms() - g_am_ask_t >= 100);     // reloadmanual (every update while the clip is low)
            const bool bm_due  = g_bmb_on.load() && now_ms() - g_bmb_t >= 100;      // bonemapbody
            const bool carry_due = carry_on() && now_ms() - g_carry_ask_t.load() >= 150;   // slotcarried
            if (!g_in_play.load() || (!g_fov_pending.load() && !g_dmg_pending.load() && !g_probe_pending.load() && !g_self_pending.load() && !veh_due && !gs_due && !snd_due && !thr_due && !drw_due && !ray_due && !eq_due && !am_due &&
                                      !bm_due && !sp_due && !fill_due && !carry_due)) return;
            const unsigned long script_tid = g_game_tid.load();
            if (script_tid == 0 || script_tid != GetCurrentThreadId()) {
                static int told = 0;
                if (told++ % 600 == 0)
                    spdlog::info("[PlayerFov] waiting: the game update runs on thread {}, its scripts on thread {}; not called", GetCurrentThreadId(), script_tid);
                return;
            }
            if (veh_due) veh_tick();                   // vehscript
            if (gs_due) gs_tick();                     // gamestate
            if (snd_due) fc5::gamesound::tick();       // gamesound
            if (thr_due) thr_tick();                   // throwlibrary
            if (drw_due) draw_tick();                  // gamedraw, gameholster
            if (sp_due) spawn_tick();                  // spawncommand
            if (fill_due && g_fill_req.exchange(false)) fill_tick();   // reloadnogame
            if (fill_due && g_local_id) {
                if (const int n = g_take_req.exchange(0); n > 0) {
                    uint64_t       tmp = 0;
                    uint64_t*      idp = g_local_id(&tmp);
                    const uint64_t me  = idp ? *idp : 0;
                    int            c   = -1, r = -1;
                    if (me != 0 && me != ~0ull) am_read(me, c, r, nullptr, n);
                }
            }
            if (ray_due) fc5::gameray::tick();         // gameray (meleeray)
            if (eq_due) eq_tick();                     // gameweapon
            if (am_due) am_tick();                     // reloadmanual
            if (bm_due) bmb_tick();                    // bonemapbody
            if (carry_due) carry_tick();               // slotcarried
            if (g_dmg_pending.load()) damage_tick();   // meleedamage
            if (g_probe_pending.load()) probe_tick();
            if (g_self_pending.exchange(false) && g_stim && g_local_id) {
                uint64_t  tmp = 0;
                uint64_t* idp = g_local_id(&tmp);
                uint64_t  me  = idp ? *idp : 0;
                if (me != 0 && me != ~0ull) {
                    const float h0 = g_health ? g_health(&me) : -1.0f;
                    g_stim(&me, me, g_self_amount.load());
                    const float h1 = g_health ? g_health(&me) : -1.0f;
                    g_after_id   = me;
                    g_after_tick = g_tick_n.load();
                    g_after_h0   = h0;
                    spdlog::info("[Melee] meleedamageself: {:.0f} damage sent to the player {:x} from itself; health {:.1f} -> {:.1f}", g_self_amount.load(), me, h0, h1);
                }
            }
            if (!g_fov_pending.load()) return;
            g_fov_pending = false;
            if (!g_fov || !g_local_id) return;
            uint64_t  tmp = 0;
            uint64_t* idp = g_local_id(&tmp);
            uint64_t  id  = idp ? *idp : 0;
            if (id == 0 || id == ~0ull) return;
            g_fov(&id, g_fov_a.load(), g_fov_b.load(), g_fov_flag.load(), 0);
            g_fov_calls++;
            spdlog::info("[PlayerFov] SetPlayerFOV({:.1f} deg, {:.2f} s, override {}) for the local player", g_fov_a.load(), g_fov_b.load(), g_fov_flag.load());
        }

        bool resolve()
        {
            g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return false;
            // CryHook5's pattern "89 70 30 48 8B 0D", found once, at +0xdfa424 in this build; ExecuteString is the jmp 0x1D further.
            static const uint8_t kSig[]{ 0x89, 0x70, 0x30, 0x48, 0x8B, 0x0D };
            uint8_t* loc = g_base + 0xdfa424;
            if (std::memcmp(loc, kSig, sizeof(kSig)) != 0) {
                spdlog::error("[PlayerFov] unexpected code at FC_m64.dll+0xdfa424: not used");
                return false;
            }
            uint8_t* c = loc + 3 + 0x1D;
            if (*c != 0xE8 && *c != 0xE9) {
                spdlog::error("[PlayerFov] no call to ExecuteString at FC_m64.dll+{:#x}: not used", c - g_base);
                return false;
            }
            g_exec = reinterpret_cast<ExecFn>(rel_target(c));
            static const uint8_t kFovSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70 };
            if (std::memcmp(g_base + 0x2FBEA10, kFovSig, sizeof(kFovSig)) != 0 || g_base[0x2FA1E50] == 0xCC) {
                spdlog::error("[PlayerFov] unexpected code at SetPlayerFOV's native (FC_m64.dll+0x2fbea10): not used");
                return false;
            }
            g_fov      = reinterpret_cast<FovFn>(g_base + 0x2FBEA10);
            g_local_id = reinterpret_cast<LocalIdFn>(g_base + 0x2FA1E50);
            g_hook     = safetyhook::create_inline(reinterpret_cast<void*>(g_exec), reinterpret_cast<void*>(&hk_exec));
            if (!g_hook) {
                spdlog::error("[PlayerFov] could not watch the game's script thread: not used");
                return false;
            }
            fc5::tobii::set_game_tick(&game_tick);
            spdlog::info("[PlayerFov] ready: ExecuteString at FC_m64.dll+{:#x} watched for the script thread; SetPlayerFOV at +0x2fbea10", reinterpret_cast<uint8_t*>(g_exec) - g_base);
            return true;
        }
    }

    void frame_tick(bool in_play)
    {
        if (!g_tried) {
            g_tried = true;
            g_ready = resolve();
        }
        g_in_play = in_play;
    }

    void queue_fov(float angle, float blend, bool flag)
    {
        if (!g_ready) return;
        g_fov_a       = angle;
        g_fov_b       = blend;
        g_fov_flag    = flag;
        g_fov_pending = true;
    }

    bool damage_watch(bool on)
    {
        if (!on) {
            g_watch = false;
            return true;
        }
        if (!g_ready) return false;
        if (!g_watch_tried) {
            g_watch_tried = true;
            // The lookup is a jmp into protected code; the two natives start as read on 26 Oct.
            static const uint8_t kPosSig[]{ 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01 };   // push rbx; sub rsp,40h; mov r8,[rcx]
            static const uint8_t kStimSig[]{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56 };   // mov rax,rsp; mov [rax+10h],rdx; push rbp; push rsi
            const bool lookup_ok = g_base[0xB27D60] == 0xE9;
            const bool pos_ok    = g_base[0x2FA09C0] == 0x40 && std::memcmp(g_base + 0x2FA09C1, kPosSig, sizeof(kPosSig)) == 0;
            const bool stim_ok   = std::memcmp(g_base + 0x2FB6760, kStimSig, sizeof(kStimSig)) == 0;
            if (!lookup_ok || !pos_ok || !stim_ok) {
                spdlog::error("[Melee] meleedamage: unexpected code (entity lookup {}, position {}, damage stim {}): not used", lookup_ok, pos_ok, stim_ok);
                return false;
            }
            g_pos         = reinterpret_cast<PosFn>(g_base + 0x2FA09C0);
            static const uint8_t kHealthSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 };   // GetPawnHealth's native
            g_health      = std::memcmp(g_base + 0x2FA2900, kHealthSig, sizeof(kHealthSig)) == 0 ? reinterpret_cast<HealthFn>(g_base + 0x2FA2900) : nullptr;
            g_stim        = reinterpret_cast<StimFn>(g_base + 0x2FB6760);
            g_lookup_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xB27D60), reinterpret_cast<void*>(&hk_lookup));
            g_watch_ok    = static_cast<bool>(g_lookup_hook);
            spdlog::info("[Melee] meleedamage: {}", g_watch_ok ? "the game's entity lookup watched (FC_m64.dll+0xb27d60)" : "the entity lookup could not be hooked: not used");
        }
        g_watch = g_watch_ok;
        return g_watch_ok;
    }

    void queue_damage_id(uint64_t id, float amount, bool send)
    {
        if (!g_watch.load()) return;
        g_dmg_id      = id;
        g_dmg_amount  = amount;
        g_dmg_send    = send;
        g_dmg_t       = now_ms();
        g_dmg_pending = true;
    }

    void queue_damage(float x, float y, float z, float amount, bool send)
    {
        if (!g_watch.load()) return;
        g_dmg_id      = 0;
        g_dmg_x       = x;
        g_dmg_y       = y;
        g_dmg_z       = z;
        g_dmg_amount  = amount;
        g_dmg_send    = send;
        g_dmg_t       = now_ms();
        g_dmg_pending = true;
    }

    void damage_self(float amount)
    {
        if (!g_watch.load()) return;
        g_self_amount  = amount;
        g_self_pending = true;
    }

    void probe_entities(float x, float y, float z)
    {
        if (!g_watch.load()) return;
        g_probe_x       = x;
        g_probe_y       = y;
        g_probe_z       = z;
        g_probe_pending = true;
    }

    std::string damage_status()
    {
        return fmt::format("lookup watched {} ({} lookups seen), {} hits with an entity found, {} sent, {} without one", g_watch.load(), g_lookups.load(), g_dmg_found.load(),
                           g_dmg_sent.load(), g_dmg_missed.load());
    }

    void equipped_query(bool on)
    {
        if (!on || !g_ready || g_eq_failed.load()) {
            g_eq_on = false;
            return;
        }
        if (!g_eq_tried) {
            g_eq_tried = true;
            static const uint8_t kEquipSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01 };   // GetEquippedItemID's native
            g_eq_ok = std::memcmp(g_base + 0x2FA0C20, kEquipSig, sizeof(kEquipSig)) == 0;
            if (g_eq_ok) {
                g_eq_fn = reinterpret_cast<EquipFn>(g_base + 0x2FA0C20);
                spdlog::info("[Weapon] gameweapon: GetEquippedItemID (FC_m64.dll+0x2fa0c20) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[Weapon] gameweapon: unexpected code at GetEquippedItemID's native (FC_m64.dll+0x2fa0c20): not used");
            }
        }
        g_eq_on = g_eq_ok;
    }

    void throwable_query(bool on)
    {
        if (!on || !g_ready) {
            g_thr_on = false;
            return;
        }
        if (!g_thr_tried) {
            g_thr_tried = true;
            // GetEquippedItemID's native, at the call: xor edx,edx (slot 0); call [rax+2C8h]. And the helpers of am_read.
            static const uint8_t kSlotCall[]{ 0x33, 0xD2, 0xFF, 0x90, 0xC8, 0x02, 0x00, 0x00 };
            g_thr_ok = std::memcmp(g_base + 0x2FA0C90, kSlotCall, sizeof(kSlotCall)) == 0 && g_base[0xB27D60] == 0xE9 && g_base[0xF48EE0] == 0xE9 && g_base[0xF64790] == 0xE9 && g_base[0xCCD250] == 0xE9;
            if (g_thr_ok) spdlog::info("[Throwable] the game's selected throwable asked every 200 ms on its script thread (inventory slot 2)");
            else spdlog::error("[Throwable] unexpected code at GetEquippedItemID's slot call or its helpers: not used");
        }
        g_thr_on = g_thr_ok;
    }

    void game_draw(uint64_t item, bool holster)
    {
        if (!g_ready) return;
        if (!g_draw_tried) {
            g_draw_tried = true;
            static const uint8_t k3E8[]{ 0xFF, 0x90, 0xE8, 0x03, 0x00, 0x00 }, k398[]{ 0xFF, 0x90, 0x98, 0x03, 0x00, 0x00 }, kHol[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x01 };
            g_draw_ok = std::memcmp(g_base + 0x19C7EC0, k3E8, sizeof(k3E8)) == 0 && std::memcmp(g_base + 0x19C7ECE, k398, sizeof(k398)) == 0 && std::memcmp(g_base + 0x2FA6CD0, kHol, sizeof(kHol)) == 0 &&
                        g_base[0xEE40F0] == 0xE9 && g_base[0xEF8990] == 0xE9 && g_base[0x1BB5140] == 0xE9 && g_base[0xF48EE0] == 0xE9 && g_base[0xF64790] == 0xE9 && g_base[0xCCD250] == 0xE9 && g_base[0xB27D60] == 0xE9;
            if (!g_draw_ok) spdlog::error("[GameWeapon] unexpected code at the game's weapon change: not used");
        }
        if (!g_draw_ok) return;
        if (holster) g_draw_holster = true;
        if (item) g_draw_item = item;
    }

    static bool carry_usable()
    {
        if (!g_ready || g_carry_failed.load()) return false;
        if (!g_carry_tried) {
            g_carry_tried = true;
            // CheckPawnWeaponInventoryByDescriptorId's native at its inventory call: mov rax,[rax]; mov rdx,rsi; call [rax+130h]. And the helpers of am_read.
            static const uint8_t kCall[]{ 0x48, 0x8B, 0x00, 0x48, 0x8B, 0xD6, 0xFF, 0x90, 0x30, 0x01, 0x00, 0x00 };
            g_carry_ok = std::memcmp(g_base + 0x2F95BF8, kCall, sizeof(kCall)) == 0 && g_base[0xB27D60] == 0xE9 && g_base[0xF48EE0] == 0xE9 && g_base[0xF64790] == 0xE9 &&
                         g_base[0xCCD250] == 0xE9;
            if (g_carry_ok) spdlog::info("[GameWeapon] slotcarried: the inventory asked whether the shoulder slots' weapons are still carried, every 150 ms on the script thread (vtable +0x130)");
            else spdlog::error("[GameWeapon] slotcarried: unexpected code at CheckPawnWeaponInventoryByDescriptorId's inventory call or its helpers: not used");
        }
        return g_carry_ok;
    }

    void carried_watch(const uint64_t* items, int n)
    {
        if (!carry_usable()) return;
        for (int i = 0; i < kCarryShoulders; ++i) g_carry_ask[i] = i < n ? items[i] : 0;
    }

    void carried_watch_kept(const uint64_t* items, int n)
    {
        if (!carry_usable()) return;
        for (int i = 0; i < kCarryKept; ++i) g_carry_ask[kCarryShoulders + i] = i < n ? items[i] : 0;
    }

    int item_carried(uint64_t item)
    {
        if (item == 0 || !g_carry_ok || g_carry_failed.load() || now_ms() - g_carry_t.load() > 1000) return -1;
        for (int i = 0; i < kCarry; ++i)
            if (g_carry_item[i].load() == item) return g_carry_ans[i].load();
        return -1;
    }

    void clip_fill_request() { g_fill_short = false; g_fill_req = true; }
    uint64_t weapon_object() { return (g_am_on.load() && now_ms() - g_am_t.load() < 1000) ? g_am_obj.load() : 0; }
    void reserve_take_request(int n) { g_take_req += n; }
    bool clip_fill_failed() { return g_fill_failed.load(); }
    bool clip_fill_short() { return g_fill_short.exchange(false); }

    // spawncommand, vehiclespawn: the game's calls checked once. False (said in the status): not usable now.
    static bool sp_usable()
    {
        if (!g_ready) { sp_say("not asked: the game's scripts are not ready"); return false; }
        if (!g_sp_tried) {
            g_sp_tried = true;
            static const uint8_t kItem[]{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x41, 0x8B, 0xF8 };
            static const uint8_t kRefill[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xFA };
            static const uint8_t kPos[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x48, 0x8B, 0xFA };
            static const uint8_t kSpawn[]{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x74, 0x24, 0x70 };
            static const uint8_t kEquip[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01 };
            g_sp_ok = std::memcmp(g_base + 0x2F90900, kItem, sizeof(kItem)) == 0 && std::memcmp(g_base + 0x2FB3740, kItem, sizeof(kItem)) == 0 && std::memcmp(g_base + 0x2FAE850, kRefill, sizeof(kRefill)) == 0 &&
                      std::memcmp(g_base + 0x2FA0A50, kPos, sizeof(kPos)) == 0 && std::memcmp(g_base + 0x2FC2A80, kSpawn, sizeof(kSpawn)) == 0 && std::memcmp(g_base + 0x2FA0C20, kEquip, sizeof(kEquip)) == 0;
            if (g_sp_ok && !g_eq_fn) g_eq_fn = reinterpret_cast<EquipFn>(g_base + 0x2FA0C20);
            game_draw(0, false);   // (its checks of the draw's code)
            if (!g_draw_ok) g_sp_ok = false;
            if (!g_sp_ok) spdlog::error("[Spawn] unexpected code at the game's inventory or spawn calls: not used");
        }
        if (!g_sp_ok) { sp_say("not used: this game's code is not what it was read from"); return false; }
        if (g_sp_failed.load()) { sp_say("off: an earlier fault"); return false; }
        return true;
    }

    void spawn_request(uint64_t item, uint64_t archetype, float fx, float fy, float yaw_add)
    {
        if (!sp_usable()) return;
        {
            std::lock_guard _{ g_sp_mx };
            g_sp         = SpReq{ item, archetype, fx, fy };
            g_sp.yaw_add = yaw_add;
        }
        g_sp_pending = true;
    }

    // vehiclespawn: its own calls checked once (the seat and the removal; the spawn itself is spawncommand's).
    static void vs_check()
    {
        if (g_vs_tried) return;
        g_vs_tried = true;
        static const uint8_t kVehSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xDA, 0xE8 };   // GetPawnVehicle's native
        static const uint8_t kSeatOfSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 };             // GetPawnVehicleSeat's native
        static const uint8_t kObjSig[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC1, 0x48, 0x8D, 0x54, 0x24, 0x58 };   // the vehicle from its id
        static const uint8_t kSeatSig[]{ 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x01 };   // ForceSetPlayerInVehicle's native
        static const uint8_t kRemoveSig[]{ 0x48, 0x8B, 0x09, 0x33, 0xD2, 0xE9 };                                       // RemoveEntity's native
        const bool in_ok   = std::memcmp(g_base + 0x2FA2C30, kVehSig, sizeof(kVehSig)) == 0 && rel_target(g_base + 0x2FA2C3C) == g_base + 0x2F8E610 &&
                             std::memcmp(g_base + 0x2FA2C80, kSeatOfSig, sizeof(kSeatOfSig)) == 0;
        const bool obj_ok  = std::memcmp(g_base + 0x2F8E8C0, kObjSig, sizeof(kObjSig)) == 0 && g_base[0x1C73890] == 0xE9;
        const bool seat_ok = std::memcmp(g_base + 0x2F9D700, kSeatSig, sizeof(kSeatSig)) == 0;
        g_vs_seat_ok   = in_ok && obj_ok && seat_ok;
        g_vs_remove_ok = in_ok && std::memcmp(g_base + 0x2FB3140, kRemoveSig, sizeof(kRemoveSig)) == 0 && rel_target(g_base + 0x2FB3145) == g_base + 0x19E3290;
        if (!g_vs_seat_ok || !g_vs_remove_ok)
            spdlog::error("[Spawn] vehiclespawn: unexpected code (GetPawnVehicle {}, the vehicle lookup {}, ForceSetPlayerInVehicle {}, RemoveEntity {}): {}{}", in_ok, obj_ok, seat_ok, g_vs_remove_ok,
                          g_vs_seat_ok ? "" : "no seating", g_vs_remove_ok ? "" : (g_vs_seat_ok ? "no removal" : ", no removal"));
    }

    void vehicle_request(uint64_t archetype, float fx, float fy, float dist, bool get_in)
    {
        if (!sp_usable()) return;
        vs_check();
        {
            std::lock_guard _{ g_sp_mx };
            g_sp = SpReq{ 0, archetype, fx, fy, dist, true, get_in };
        }
        g_sp_pending = true;
    }

    uint64_t spawned_vehicle_archetype(uint64_t id)
    {
        if (id == 0) return 0;
        std::lock_guard _{ g_vs_arch_mx };
        for (const auto& [e, a] : g_vs_arch)
            if (e == id) return a;
        return 0;
    }

    void vehicle_remove_request()
    {
        if (!sp_usable()) return;
        vs_check();
        if (!g_vs_remove_ok) { sp_say("not removed: unexpected code at the game's RemoveEntity"); return; }
        g_vs_remove = true;
    }

    std::string spawn_status()
    {
        std::lock_guard _{ g_sp_mx };
        return g_sp_status;
    }

    uint64_t selected_throwable()
    {
        return g_thr_on.load() ? g_thr_item.load() : 0;
    }

    void ammo_query(bool on)
    {
        if (!on || !g_ready || g_am_failed.load()) {
            g_am_on = false;
            return;
        }
        if (!g_am_tried) {
            g_am_tried = true;
            g_am_ok    = std::memcmp(g_base + 0x2FA2E40, kAmmoNative, sizeof(kAmmoNative)) == 0 && g_base[0xB27D60] == 0xE9 && g_base[0xF48EE0] == 0xE9 &&
                      g_base[0xF64790] == 0xE9 && g_base[0xCCD250] == 0xE9;
            if (g_am_ok) spdlog::info("[Reload] the player's clip and reserve read every 100 ms on the game's script thread (GetPlayerAmmoIsEmpty's route, FC_m64.dll+0x2fa2e40)");
            else spdlog::error("[Reload] unexpected code at GetPlayerAmmoIsEmpty's native (FC_m64.dll+0x2fa2e40) or its helpers: the clip is not read (timers stand in)");
        }
        g_am_on = g_am_ok;
    }

    void ammo_fast(bool on) { g_am_fast = on; }

    bool player_ammo(int& clip, int& reserve, int* cap)
    {
        clip    = g_am_clip.load();
        reserve = g_am_reserve.load();
        if (cap) *cap = g_am_cap.load();
        return g_am_on.load() && now_ms() - g_am_t.load() <= 1000;
    }

    bool equipped_item(uint64_t& item)
    {
        item = g_eq_item.load();
        return g_eq_on.load() && now_ms() - g_eq_t.load() <= 1000;
    }

    void vehicle_query(bool on)
    {
        if (!on || !g_ready || g_veh_failed.load()) {
            g_veh_on = false;
            return;
        }
        if (!g_veh_tried) {
            g_veh_tried = true;
            static const uint8_t kVehSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xDA, 0xE8 };   // GetPawnVehicle's native
            static const uint8_t kSeatSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 };               // GetPawnVehicleSeat's native
            static const uint8_t kTypeSig[]{ 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x09, 0xE8 };                            // GetPawnVehicleType's native
            static const uint8_t kObjSig[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC1, 0x48, 0x8D, 0x54, 0x24, 0x58 };   // the vehicle from its id
            const bool veh_ok  = std::memcmp(g_base + 0x2FA2C30, kVehSig, sizeof(kVehSig)) == 0 && rel_target(g_base + 0x2FA2C3C) == g_base + 0x2F8E610;
            const bool seat_ok = std::memcmp(g_base + 0x2FA2C80, kSeatSig, sizeof(kSeatSig)) == 0;
            const bool obj_ok  = std::memcmp(g_base + 0x2F8E8C0, kObjSig, sizeof(kObjSig)) == 0 && g_base[0x1C73890] == 0xE9;
            const bool type_ok = std::memcmp(g_base + 0x2FA2CD0, kTypeSig, sizeof(kTypeSig)) == 0;
            g_veh_ok = veh_ok && seat_ok && obj_ok && type_ok;
            if (g_veh_ok) {
                g_pawn_veh    = reinterpret_cast<PawnVehFn>(g_base + 0x2FA2C30);
                g_pawn_seat   = reinterpret_cast<PawnSeatFn>(g_base + 0x2FA2C80);
                g_veh_obj     = reinterpret_cast<VehObjFn>(g_base + 0x2F8E8C0);
                g_driver_seat = reinterpret_cast<DriverSeatFn>(g_base + 0x1C73890);
                g_pawn_type   = reinterpret_cast<PawnTypeFn>(g_base + 0x2FA2CD0);
                static const uint8_t kStanceSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C };   // GetPawnStance's native
                if (std::memcmp(g_base + 0x2FA2B50, kStanceSig, sizeof(kStanceSig)) == 0) g_stance_fn = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2B50);
                else spdlog::error("[Vehicle] vehscript: unexpected code at GetPawnStance (FC_m64.dll+0x2fa2b50): the stance not read");
                static const uint8_t kAirSig[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 };   // IsInAir's native
                if (std::memcmp(g_base + 0x2FA8D20, kAirSig, sizeof(kAirSig)) == 0) g_air_fn = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8D20);
                else spdlog::error("[Vehicle] vehscript: unexpected code at IsInAir (FC_m64.dll+0x2fa8d20): in the air not read");
                spdlog::info("[Vehicle] vehscript: GetPawnVehicle (FC_m64.dll+0x2fa2c30) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[Vehicle] vehscript: unexpected code (GetPawnVehicle {}, its seat {}, the driver's seat {}, its type {}): not used", veh_ok, seat_ok, obj_ok, type_ok);
            }
        }
        g_veh_on = g_veh_ok;
    }

    int vehicle_state(int& driver, int& type)
    {
        driver = -1;
        type   = -1;
        if (!g_veh_on.load() || now_ms() - g_veh_t.load() > 1000) return -1;
        const int st = g_veh_state.load();
        if (st >= 0) driver = g_veh_driver.load();
        if (st == 1) type = g_veh_type.load();
        return st;
    }

    void vehicle_seat(int& seat, int& dseat, uint64_t& id)
    {
        const bool ok = g_veh_on.load() && now_ms() - g_veh_t.load() <= 1000 && g_veh_state.load() == 1;
        seat  = ok ? g_veh_seat.load() : -1;
        dseat = ok ? g_veh_dseat.load() : -1;
        id    = ok ? g_veh_id.load() : 0;
    }

    void down_query(bool on)
    {
        if (!on || !g_ready || g_down_failed.load()) {
            g_down_on    = false;
            g_down_state = -1;
            return;
        }
        if (!g_down_tried) {
            g_down_tried = true;
            static const uint8_t kDownSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54 };   // IsPawnDown's native
            g_down_ok = std::memcmp(g_base + 0x2FA9900, kDownSig, sizeof(kDownSig)) == 0;
            if (g_down_ok) {
                g_down_fn = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA9900);
                spdlog::info("[Down] downview: IsPawnDown (FC_m64.dll+0x2fa9900) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[Down] downview: unexpected code at IsPawnDown (FC_m64.dll+0x2fa9900): not used (the view as before when knocked down)");
            }
        }
        g_down_on = g_down_ok;
    }

    void qte_query(bool on)
    {
        if (!on || !g_ready || g_qte_failed.load()) {
            g_qte_on    = false;
            g_qte_state = -1;
            return;
        }
        if (!g_qte_tried) {
            g_qte_tried = true;
            static const uint8_t kQteSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 };   // IsInQTE's native (30 Oct)
            g_qte_ok = std::memcmp(g_base + 0x2FA8E80, kQteSig, sizeof(kQteSig)) == 0;
            if (g_qte_ok) {
                g_qte_fn = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8E80);
                spdlog::info("[HUD] qtehud: IsInQTE (FC_m64.dll+0x2fa8e80) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[HUD] qtehud: unexpected code at IsInQTE (FC_m64.dll+0x2fa8e80): not used (knocked down still is)");
            }
        }
        g_qte_on = g_qte_ok;
    }

    int pawn_qte()
    {
        if (!g_qte_on.load() || now_ms() - g_qte_t.load() > 1000) return -1;
        return g_qte_state.load();
    }

    int pawn_down()
    {
        if (!g_down_on.load() || now_ms() - g_down_t.load() > 1000) return -1;
        return g_down_state.load();
    }

    int pawn_stance()
    {
        if (!g_veh_on.load() || !g_stance_fn || now_ms() - g_stance_t.load() > 1000) return -1;
        return g_stance.load();
    }

    int pawn_in_air()
    {
        if (!g_veh_on.load() || !g_air_fn || now_ms() - g_air_t.load() > 1000) return -1;
        return g_air.load();
    }

    bool down_failed() { return g_down_failed.load() || (g_down_tried && !g_down_ok); }

    std::string vehicle_status()
    {
        return fmt::format("vehscript {}{}: {} answers, the game says {} (entity {:x}, type {}, seat {}, driver's seat {}), last answer {} ms ago", g_veh_on.load() ? "asking" : "not asking",
                           g_veh_failed.load() ? " (failed)" : "", g_veh_queries.load(),
                           g_veh_state.load() < 0 ? "nothing yet" : g_veh_state.load() ? (g_veh_driver.load() == 1 ? "in a vehicle, driving" : g_veh_driver.load() == 0 ? "in a vehicle, riding" : "in a vehicle, getting in") : "on foot",
                           g_veh_id.load(), g_veh_type.load(), g_veh_seat.load(), g_veh_dseat.load(), g_veh_t.load() ? now_ms() - g_veh_t.load() : -1);
    }

    void gamestate_query(bool on)
    {
        if (!on || !g_ready || g_gs_failed.load()) {
            g_gs_on = false;
            return;
        }
        if (!g_gs_tried) {
            g_gs_tried = true;
            if (!g_veh_tried) {   // GetPawnVehicle and the seats: vehscript's checks and pointers, its own switch left as it is
                const bool was = g_veh_on.load();
                vehicle_query(true);
                g_veh_on = was;
            }
            // The natives' first bytes as read on 30 Oct (research\2026-10-30_engine_natives.md).
            struct Sig { uint32_t rva; std::initializer_list<uint8_t> b; const char* name; };
            const Sig sigs[]{
                { 0x2FA2CD0, { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x09, 0xE8 }, "GetPawnVehicleType" },
                { 0x2FA2B50, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C }, "GetPawnStance" },
                { 0x2FA8E80, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 }, "IsInQTE" },
                { 0x2FA9900, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54 }, "IsPawnDown" },
                { 0x2FA97D0, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x58 }, "IsPawnDead" },
                { 0x2FA8D20, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 }, "IsInAir" },
                { 0x2FA8F90, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 }, "IsInWater" },
                { 0x2FA2F40, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x48 }, "GetPlayerFOV" },
                { 0x2FA3670, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x48 }, "GetPlayerNearFOV" },
                { 0x2FA2AE0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xDA }, "GetPawnRidingEntity" },
                { 0x2FA8580, { 0x48, 0x8B, 0x05, 0x71, 0xD4, 0xF0, 0x01, 0x0F, 0xB6, 0x40, 0x68, 0xC3 }, "IsGamePaused" },
            };
            std::string bad;
            for (const auto& s : sigs)
                if (std::memcmp(g_base + s.rva, s.b.begin(), s.b.size()) != 0) bad += fmt::format(" {}", s.name);
            g_gs_ok = bad.empty() && g_veh_ok;
            if (g_gs_ok) {
                g_gs_vtype   = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2CD0);
                g_gs_stance  = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2B50);
                g_gs_qte     = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8E80);
                g_gs_down    = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA9900);
                g_gs_dead    = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA97D0);
                g_gs_air     = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8D20);
                g_gs_water   = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8F90);
                g_gs_fov     = reinterpret_cast<PawnFloatFn>(g_base + 0x2FA2F40);
                g_gs_nearfov = reinterpret_cast<PawnFloatFn>(g_base + 0x2FA3670);
                g_gs_ride    = reinterpret_cast<PawnVehFn>(g_base + 0x2FA2AE0);
                g_gs_pause   = reinterpret_cast<uint8_t**>(g_base + 0x4EB59F8);   // IsGamePaused's global (mov rax,[rip+0x1F0D471])
                spdlog::info("[GameState] gamestate: the player's state from the game's script functions every 100 ms on its script thread, the pause byte every frame");
            } else {
                spdlog::error("[GameState] gamestate: unexpected code ({}{}): not used", bad.empty() ? "" : "changed:", bad.empty() ? " GetPawnVehicle" : bad);
            }
        }
        if (g_gs_ok && !g_watch_hooks[0] && !g_bars_hook) {
            int hooked = 0;
            watch_install(std::make_integer_sequence<int, kWatchN>{}, g_base, hooked);
            if (g_base[0x1476B40] == 0xE9) g_bars_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1476B40), reinterpret_cast<void*>(&hk_bars));
            spdlog::info("[GameState] gamestate: {} of {} cinematic and camera script functions watched, black bars {}", hooked, kWatchN, g_bars_hook ? "watched" : "not watched");
        }
        g_gs_on = g_gs_ok;
    }

    void gamestate_frame(bool mod_menu, bool mod_cine, bool mod_vehicle)
    {
        if (!g_gs_on.load() || !g_gs_pause) return;
        const int p = gs_pause_read();
        if (p == g_gs_pause_last) return;
        if (p == -2) {
            g_gs_on     = false;
            g_gs_failed = true;
            spdlog::error("[GameState] a fault reading the pause byte: gamestate turned off");
            return;
        }
        spdlog::info("[GameState] pause byte {} -> {} (the mod: menu screen {}, cinematic {}, vehicle {})", g_gs_pause_last, p, mod_menu, mod_cine, mod_vehicle);
        g_gs_pause_last = p;
    }

    namespace
    {
        // GetBonePos, GetEquippedItemID and GetEntityPosition's natives, checked once (gamestatebones, bonemapbody).
        bool gsb_setup()
        {
            if (!g_ready) return false;
            if (g_gsb_ok) return true;
            static const uint8_t kBoneSig[]{ 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xE0 };    // GetBonePos's native
            static const uint8_t kEquipSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01 };        // GetEquippedItemID's
            static const uint8_t kPosSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01 };                                   // GetEntityPosition's
            const bool bone_ok  = std::memcmp(g_base + 0x2F9EBC0, kBoneSig, sizeof(kBoneSig)) == 0;
            const bool equip_ok = std::memcmp(g_base + 0x2FA0C20, kEquipSig, sizeof(kEquipSig)) == 0;
            const bool pos_ok   = std::memcmp(g_base + 0x2FA09C0, kPosSig, sizeof(kPosSig)) == 0;
            if (!bone_ok || !equip_ok || !pos_ok) {
                spdlog::error("[GameState] gamestatebones: unexpected code (GetBonePos {}, GetEquippedItemID {}, GetEntityPosition {}): not used", bone_ok, equip_ok, pos_ok);
                return false;
            }
            g_gsb_bone  = reinterpret_cast<BonePosFn>(g_base + 0x2F9EBC0);
            g_gsb_equip = reinterpret_cast<EquippedFn>(g_base + 0x2FA0C20);
            g_gsb_pos   = reinterpret_cast<EntPosFn>(g_base + 0x2FA09C0);
            g_gsb_ok    = true;
            return true;
        }
    }

    bool gamestate_bones(bool on)
    {
        if (!on) {
            g_gsb_on = false;
            return true;
        }
        if (!g_gsb_ok) {
            if (!gsb_setup()) return false;
            spdlog::info("[GameState] gamestatebones: {} body points and {} sight bones logged once a second while gamestate is on", kBodyN, kItemN);
        }
        g_gsb_on = true;
        return true;
    }

    bool body_bones(bool on)
    {
        if (!on) {
            g_bmb_on = false;
            return true;
        }
        if (!gsb_setup()) return false;
        g_bmb_on = true;
        return true;
    }

    int body_bones_read(float (*pos)[3], const char** names, int max, int64_t& age_ms)
    {
        std::lock_guard _{ g_bmb_mtx };
        age_ms = g_bmb_at ? now_ms() - g_bmb_at : -1;
        if (!g_bmb_at) return 0;
        int n = 0;
        for (int i = 0; i < kBmBodyN && n < max; ++i) {
            const float* p = g_bmb_pos[i];
            if (std::abs(p[0] - g_bmb_origin[0]) + std::abs(p[1] - g_bmb_origin[1]) + std::abs(p[2] - g_bmb_origin[2]) <= 0.002f) continue;   // not a bone of the skeleton
            if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) continue;
            std::memcpy(pos[n], p, sizeof(float) * 3);
            names[n] = kBmBody[i];
            n++;
        }
        return n;
    }

    std::string gamestate_status()
    {
        return fmt::format("gamestate {}{}: {} reads, {} changes logged, pause byte {}, black bars {}, {} cinematic/camera script calls; last: {}", g_gs_on.load() ? "on" : "off",
                           g_gs_failed.load() ? " (failed)" : "", g_gs_reads.load(), g_gs_changes.load(), g_gs_pause_last, g_bars.load(), g_watch_calls.load(), g_gs_have ? gs_text(g_gs_last) : std::string{ "none" });
    }

    std::string status()
    {
        return fmt::format("{}; game script thread {} ({} script calls seen), game update thread {}, SetPlayerFOV calls {}",
                           g_ready ? "ready" : (g_tried ? "not usable" : "not looked for yet"), g_game_tid.load(), g_game_calls.load(), g_tick_tid.load(), g_fov_calls.load());
    }
}
