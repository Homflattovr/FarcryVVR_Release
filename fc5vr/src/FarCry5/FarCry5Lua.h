#pragma once

// The game's own SetPlayerFOV, called by the mod for the local player (see FarCry5Lua.cpp). No script text is run.

#include <cstdint>
#include <string>

namespace fc5::lua
{
    // From the frame hook: finds the game's code once; in_play gates the call.
    void frame_tick(bool in_play);
    // SetPlayerFOV(angle deg, blend s, override) for the local player, made at the next game camera update on the game's
    // script thread.
    void queue_fov(float angle, float blend, bool flag);
    std::string status();
    // meleedamage (FarCry5Lua.cpp): on: the game's entity lookup watched (ids recorded); queue_damage: at the next game update the
    // recorded entity nearest (x, y, z) (a body's root, world) is found and, with send, given amount of damage from the player.
    bool        damage_watch(bool on);
    // throwlibrary (FarCry5Lua.cpp): on: the game's selected throwable asked every 200 ms on its script thread. selected_throwable: its item id (0: off, none, or not asked yet).
    void        throwable_query(bool on);
    uint64_t    selected_throwable();
    // gamedraw, gameholster (tests): the game's own weapon change at the next game update: this item drawn, or the weapon put away.
    void        game_draw(uint64_t item, bool holster);
    // slotcarried (FarCry5Lua.cpp): these items (at most 8; 0 ends the list; n 0 stops asking) asked every 150 ms on the script thread: does the player still
    // carry each (the inventory's own lookup, as CheckPawnWeaponInventoryByDescriptorId). item_carried: 1 carried, 0 not, -1 unknown (not watched, not asked
    // yet, the answer over 1 s old, unexpected code or a fault).
    void        carried_watch(const uint64_t* items, int n);
    // weaponcache (FarCry5Holster.cpp): the same for the sling's and hip's weapons kept from the last start (at most 4), asked beside the slots' items.
    void        carried_watch_kept(const uint64_t* items, int n);
    int         item_carried(uint64_t item);
    // spawncommand (FarCry5Spawn.cpp): at the next game update. item: added to the player's inventory and drawn, the held weapon dropped as its world entity; archetype: that entity on the ground. (fx, fy): ahead of the player.
    // The ground entity is turned to (fx, fy) plus yaw_add degrees (spawnfacing, spawnyaw); a dropped weapon is not turned.
    void        spawn_request(uint64_t item, uint64_t archetype, float fx, float fy, float yaw_add = 0.0f);
    // vehiclespawn (FarCry5Spawn.cpp): at the next game update, that vehicle dist metres ahead, turned the way the player faces; get_in: the player then seated in its driver seat.
    // vehicle_remove_request: the vehicles spawned this session removed, except the one the player is in. What happened: spawn_status.
    // clear (spawnvehicle[in] <id>@clear; author 11 Nov 19:13: test vehicles): the vehicle is placed on the first flat empty patch found around the player instead
    // (12 to 42 m away, behind first), looked for with ground rays a metre apart over the vehicle's footprint; none: ahead, with a warning.
    void        vehicle_request(uint64_t archetype, float fx, float fy, float dist, bool get_in, bool clear = false);
    void        vehicle_remove_request();
    // drive (FarCry5Drive.cpp): the archetype a vehicle entity was spawned from by vehicle_request this session (0: not one of those).
    uint64_t    spawned_vehicle_archetype(uint64_t id);
    // reloadnogame (FarCry5Reload.cpp): the held weapon's clip filled at the next script update, with no reload of the game's: its reserve goes down by what went in.
    void        clip_fill_request(int mode = -1);   // mode (ammoledger): -1 as before, 0 a top-up (nothing taken from the reserve), 1 a load (a clip already full is no failure)
    // the ammo ledger (FarCry5Reload.cpp): the weapon object in hand at the latest ammo read (inventory vtable +0x308), 0 with none or no
    // answer in the last second. Only its address: read through FarCry5SwitchSpeed.cpp's checked reads, never dereferenced here.
    uint64_t    weapon_object();
    void        reserve_take_request(int n);   // n rounds out of the weapon in hand's reserve (on the script thread, as RemoveItem does it)
    bool        clip_fill_failed();   // the game's calls did not fill the clip (this session): the manual reload taps the game's reload as before
    bool        clip_fill_short();    // this fill not made (fewer rounds left than the clip lacks, or no answer): this one reload is the game's, whose count is exact. Read once
    std::string spawn_status();   // what the last one did, in words
    // playerskin (FarCry5Skin.cpp; author 15 Nov; research\2026-11-15_playerskin\README.md): on the script thread at the next game update.
    // skin_equip: EquipWolfskinItem's native (&player, id), then (force) ForceCharacterCustomization's (&player); the item looked up by the game
    // (an id it does not know: nothing happens). skin_worn: HasCustomizationEquipped and IsWearingWolfskinItem asked for every row of
    // FarCry5SkinTable.h with its shop and desc ids, the true ones logged ([Skin] lines). skin_menu: OpenAvatarCreationMenu's Lua binding.
    void        skin_equip(uint64_t id, bool force, const char* name);
    void        skin_force();
    void        skin_worn();
    void        skin_menu();
    std::string skin_status();
    bool        spawn_drawing(); // a spawnhand asked and its item not yet seen in the hand (melee fix 1: the game shows other weapons meanwhile)
    void        queue_damage_id(uint64_t id, float amount, bool send);   // meleeray: the entity the ray hit
    void        queue_damage(float x, float y, float z, float amount, bool send);
    std::string damage_status();
    bool        damage_killed_recent(int64_t ms);   // melee fix 4: the mod's damage took a target to 0 or below within the last ms
    void        damage_self(float amount);                    // meleedamageself (test): the damage stim sent to the player itself
    void        probe_entities(float x, float y, float z);   // meleedamageprobe (test): the recorded entities nearest a point, logged
    // melee fix 14b: ON THE GAME'S SCRIPT THREAD ONLY (gameray's tick). The recorded entity (damage_watch(true) records the game's entity lookups)
    // nearest a body's root (world): within 0.7 m across the ground and 1.2 m up or down, not the player, reading a living pawn's health (above 0,
    // below 100000). 0: none; why says what was looked at (or why nothing could be). dist: its root's distance across the ground.
    uint64_t    entity_at_root(float x, float y, float z, float& health, float& dist, std::string& why);
    // vehscript (FarCry5Lua.cpp): on: the game's GetPawnVehicle asked for the local player every 150 ms on its script thread.
    // vehicle_state: -1 unknown (off, failed, or no answer in the last second), 0 on foot, 1 in a vehicle; driver -1 unknown, 0, 1;
    // type (in a vehicle): 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle, 4 helicopter, 5 plane, 6 wingsuit, else -1.
    // gameweapon (FarCry5Lua.cpp): on: the game's GetEquippedItemID asked for the player every 150 ms on its script thread. equipped_item:
    // the item's database id (0 none; fc5::weapons::find in FarCry5WeaponTable.h), false when off, failed or no answer in the last second.
    void        equipped_query(bool on);
    bool        equipped_item(uint64_t& item);
    // reloadmanual (FarCry5Reload.cpp): on: the player's weapon's clip and its reserve asked every 100 ms on the script thread, the way
    // GetPlayerAmmoIsEmpty's native reads them. player_ammo: false when off, failed, or no answer in the last second.
    void        ammo_query(bool on);
    bool        player_ammo(int& clip, int& reserve, int* cap = nullptr);   // cap: the clip's size (wristammo; -1 not read)
    // reloadkeepone (manualreload39): asked at every game update instead of every 100 ms while the clip is low, so a magazine gun's shots
    // are stopped with one round left (an AR-C fires every 75 ms).
    void        ammo_fast(bool on);
    void        vehicle_query(bool on);
    int         vehicle_state(int& driver, int& type);
    std::string vehicle_status();
    // vehdrivegun (log only): the game's seat, its driver's seat and the vehicle entity from the last answer (-1, -1, 0 when on foot or unknown).
    void        vehicle_seat(int& seat, int& dseat, uint64_t& id);
    // downview: on: the game's IsPawnDown asked for the player in the same 150 ms script-thread poll (with or without vehscript).
    // pawn_down: -1 unknown (off, failed, no answer in the last second), 0, 1. down_failed: a fault or unexpected code (off for the session).
    void        down_query(bool on);
    int         pawn_down();
    // qtehud: on: the game's IsInQTE asked for the player in the same poll. pawn_qte: -1 unknown (off, failed, no answer in the last
    // second), 0, 1 (a struggle, a takedown or any other quick-time event).
    void        qte_query(bool on);
    int         pawn_qte();
    bool        down_failed();
    // GetPawnStance for the player while vehscript asks (same poll): 0 standing, 1 crouched, 4 downed, 18 seated in a vehicle; -1 unknown.
    int         pawn_stance();
    // chutemin: IsInAir for the player while vehscript asks (same poll): 1 a jump or free fall, 0 on the ground or under an open chute, -1 unknown.
    int         pawn_in_air();
    // gamestate (log only, off by default, saved; F11 Comfort > Cinematics and vehicles): the player's state from the game's own script functions (vehicle and its type,
    // seat, riding, stance, QTE, down, dead, in air, in water, FOV, near FOV) every 100 ms on the script thread, and the pause byte
    // every frame; a [GameState] line on each change, the pause line with the mod's own menu/cinematic/vehicle state beside it.
    void        gamestate_query(bool on);
    void        gamestate_frame(bool mod_menu, bool mod_cine, bool mod_vehicle);
    std::string gamestate_status();
    // Cutscenes (author 13 Nov 14:22): the
    // game's scripts starting and stopping cinematic sequences, counted from the same Lua bindings gamestate logs. cine_watch()
    // installs them once the game's script layer is ready, whatever gamestate says (true once installed); the counters only grow:
    // starts = StartCinematicScene(_v2), StartCinematicMetaSequence and the StartCinematicSequence*WithMetaId calls; stops =
    // StopCinematicSequence, StopCinematicMetaSequence and SkipCinematicSequence (both registrations of each).
    bool        cine_watch();
    uint64_t    cine_starts();
    uint64_t    cine_stops();
    // gamestatebones (log only, test, not saved): with gamestate on, once a second the game's world positions of the player's body bones,
    // the holster/hand attach points its weapon data names and the equipped item's sight bones, relative to the player. False: not usable.
    bool        gamestate_bones(bool on);
    // bonemapbody (FarCry5BoneMap.h; debug, not saved): every 100 ms on the script thread, the player's body bones (GetBonePos, as gamestatebones).
    // body_bones_read: the bones the skeleton has (world positions), their names, how old the read is (ms; -1 none yet). False/0: not usable.
    bool        body_bones(bool on);
    int         body_bones_read(float (*pos)[3], const char** names, int max, int64_t& age_ms);
    // firstgunpause (FarCry5FirstGun.cpp): the game's own PauseGame (its native +0x2FAC5C0), called on the game's script thread at the next game
    // update or script string. pause_ready: the native checked (false: not usable, the popup only holds the controls). pause_request: asks a pause
    // or the resume. paused_by_mod: paused or a pause asked. pause_byte: IsGamePaused's byte (-1 unknown). pause_frame (render thread, every
    // frame): a resume not made within 1 s is made there (the game update may stop while paused); a pause not made within 3 s is given up.
    // firstguntip's loot watch: gun_watch(true) asks the inventory for every gun of the weapon table every 250 ms in play (its first read the
    // baseline); false stops it and drops the baseline (a load must not read as a loot). gun_new_count: new guns seen so far (grows), the last one's item.
    void        gun_watch(bool on);
    uint32_t    gun_new_count(uint64_t* item);
    bool        pause_ready();
    void        pause_request(bool pause);
    bool        paused_by_mod();
    int         pause_byte();
    void        pause_frame();
}
