// gameray (2 Nov, author 15:15; research/2026-11-02_game_raycast.md): the game's own physics ray cast, so a swing can touch the
// real shapes of bodies and walls. Read statically from this build, IsEntityInRay's native (+0x2FA7FA0) and the exported FCE_Editor_RayCastPhysics (+0x3168690 -> +0x3121960):
//   +0x793250 (world [+0x4F7E828], float3* start, float3* delta, array* hits, filter*, ignore*, int flags); a critical section is taken inside.
//   filter: +0x70AD30 (filter*, mask, 0), destroyed by +0x70F750; IsEntityInRay's mask is 0x2DBF and flags 5, the editor's 0x25BF and 6. Simulator 2 Nov: flags 5 gives the nearest hit, 6 all of them.
//   ignore: two zero qwords, destroyed by +0xE81370. hits: { Hit* p; u32 capacity; u32 count (top bit a flag) }, destroyed by +0x6DF020.
//   A hit is 48 bytes: +0 position, +0xC normal, +0x18 u32 the physics body, +0x1C metres from the start. +0x78D890 (world, body) -> object; +0xF66220 (object + 8, u64* out) -> u64* the entity id.
// GetPawnHealth's native (+0x2FA2900) takes any id and gives 0 with no health component: a body here is what has health.
// A real NPC (simulator 3 Nov, the author's dog, health 400, research/2026-11-02_game_raycast.md) has two physics bodies: an outer capsule (the hit's +0x20 bytes ffff0000) and body parts (ffff pp 4d, pp the part: 0, 3, 4, 6, 9, 0xC, 0xD,
// 0xE, 0x11 seen). meleerayfine (default on): once an entity has shown parts, a ray through its capsule that meets no part is a miss. The player has the capsule alone.
// All on the script thread, from the game tick (FarCry5Lua.cpp). The checks below refuse anything but these exact bytes, and a fault turns it off.
#include "FarCry5GameRay.h"
#include "FarCry5MeleeDmgWatch.h"
#include "FarCry5Lua.h"   // melee fix 14b: entity_at_root
#include "FarCry5ReloadParts.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fc5::gameray
{
    namespace
    {
        struct RawHit { float px, py, pz, nx, ny, nz; uint32_t body; float along; uint8_t rest[16]; };
        static_assert(sizeof(RawHit) == 48);
        struct RawArray { RawHit* p; uint32_t capacity; uint32_t count; };

        using CastFn    = void(__fastcall*)(void* world, const float* start, const float* delta, RawArray* hits, void* filter, void* ignore, int flags);
        using FilterFn  = void*(__fastcall*)(void* filter, uint32_t mask, uint64_t zero);
        using DestroyFn = void(__fastcall*)(void* p);
        using BodyFn    = void*(__fastcall*)(void* world, uint32_t body);
        using IdFn      = uint64_t*(__fastcall*)(void* object, uint64_t* out);
        using HealthFn  = float(__fastcall*)(uint64_t* id);
        using LocalFn   = uint64_t*(__fastcall*)(uint64_t* out);

        uint8_t*  g_base = nullptr;
        bool      g_tried = false, g_ok = false;
        void**    g_world = nullptr;
        CastFn    g_cast = nullptr;
        FilterFn  g_filter = nullptr;
        DestroyFn g_filter_d = nullptr, g_array_d = nullptr, g_ignore_d = nullptr;
        BodyFn    g_body = nullptr;
        IdFn      g_id = nullptr;
        HealthFn  g_health = nullptr;
        LocalFn   g_local = nullptr;

        std::atomic<bool>     g_failed{ false };
        std::atomic<uint32_t> g_mask{ 0x2DBF };
        std::atomic<bool>     g_fine{ true };
        std::unordered_set<uint64_t> g_has_parts;   // script thread only
        std::atomic<int>      g_flags{ 6 };         // 6: every hit; 5: the nearest only, so a ray from inside the player's own shape sees nothing else
        std::atomic<bool>     g_seg_pending{ false }, g_res_ready{ false }, g_test_pending{ false };
        std::atomic<bool>     g_wall_blocks{ false };                           // meleereact
        std::atomic<bool>     g_probe_pending{ false }, g_probe_ready{ false };   // weaponcollide
        Seg                   g_probe_seg{};
        Hit                   g_probe_hit{};
        std::atomic<uint64_t> g_casts{ 0 }, g_hits{ 0 };
        std::mutex            g_mx;
        Seg                   g_segs[8]{};
        int                   g_segs_n = 0;
        Result                g_res{};
        float                 g_test_s[3]{}, g_test_d[3]{};
        int64_t               g_test_mask = -1;
        int                   g_test_flags = -1;

        bool same(const uint8_t* p, std::initializer_list<uint8_t> want)
        {
            return std::memcmp(p, want.begin(), want.size()) == 0;
        }

        bool ready()
        {
            if (g_tried) return g_ok;
            g_tried = true;
            g_base  = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return false;
            const bool ok_cast = same(g_base + 0x793250, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xE9, 0x49 });
            // IsEntityInRay's native: the code this was read from
            const bool ok_nat  = same(g_base + 0x2FA7FA0, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x4C, 0x89, 0x40, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x33, 0xDB, 0x48, 0x8B });
            const bool ok_jmps = g_base[0x70AD30] == 0xE9 && g_base[0x70F750] == 0xE9 && g_base[0x6DF020] == 0xE9 && g_base[0xE81370] == 0xE9 && g_base[0x78D890] == 0xE9 && g_base[0xF66220] == 0xE9;
            if (!ok_cast || !ok_nat || !ok_jmps) {
                spdlog::error("[GameRay] unexpected code (cast {}, native {}, helpers {}): not used", ok_cast, ok_nat, ok_jmps);
                return false;
            }
            g_world    = reinterpret_cast<void**>(g_base + 0x4F7E828);
            g_cast     = reinterpret_cast<CastFn>(g_base + 0x793250);
            g_filter   = reinterpret_cast<FilterFn>(g_base + 0x70AD30);
            g_filter_d = reinterpret_cast<DestroyFn>(g_base + 0x70F750);
            g_array_d  = reinterpret_cast<DestroyFn>(g_base + 0x6DF020);
            g_ignore_d = reinterpret_cast<DestroyFn>(g_base + 0xE81370);
            g_body     = reinterpret_cast<BodyFn>(g_base + 0x78D890);
            g_id       = reinterpret_cast<IdFn>(g_base + 0xF66220);
            g_health   = reinterpret_cast<HealthFn>(g_base + 0x2FA2900);
            g_local    = reinterpret_cast<LocalFn>(g_base + 0x2FA1E50);
            g_ok       = true;
            spdlog::info("[GameRay] the game's ray cast is ready");
            return true;
        }

        constexpr int kMaxHits = 32;
        struct Out { RawHit hit; uint64_t entity; float health; };

        // One cast, copied out (no C++ objects here: the __try). -1: a fault.
        int cast_raw(const float* s, const float* d, uint32_t mask, int flags, Out* out)
        {
            __try {
                void* world = *g_world;
                if (!world) return 0;
                alignas(16) uint8_t filter[32]{};
                uint64_t            ignore[2]{};
                RawArray            arr{};
                g_filter(filter, mask, 0);
                g_cast(world, s, d, &arr, filter, ignore, flags);
                g_filter_d(filter);
                const uint32_t n = arr.count & 0x7FFFFFFFu;
                int            m = 0;
                for (uint32_t i = 0; i < n && m < kMaxHits; ++i, ++m) {
                    out[m].hit    = arr.p[i];
                    out[m].entity = 0;
                    out[m].health = 0.0f;
                    if (void* o = g_body(world, arr.p[i].body)) {
                        uint64_t  tmp = 0;
                        uint64_t* id  = g_id(static_cast<uint8_t*>(o) + 8, &tmp);
                        if (id && *id != 0 && *id != ~0ull) {
                            uint64_t e    = *id;
                            out[m].entity = e;
                            out[m].health = g_health(&e);
                        }
                    }
                }
                g_array_d(&arr);
                g_ignore_d(ignore);
                return m;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        uint64_t player_id()
        {
            __try {
                uint64_t  tmp = 0;
                uint64_t* p   = g_local(&tmp);
                return p ? *p : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        void keep(Hit& h, float& best, const Out& o)
        {
            if (o.hit.along >= best) return;
            best = o.hit.along;
            h    = Hit{ true, o.entity, o.hit.px, o.hit.py, o.hit.pz, o.hit.nx, o.hit.ny, o.hit.nz, o.health, o.hit.rest[3] != 0 ? int(o.hit.rest[2]) : -1, o.hit.along };
        }

        void fail()
        {
            g_failed = true;
            spdlog::error("[GameRay] a fault in the game's ray cast: turned off");
        }

        // melee fix 13: the damage watch (watch_damage). One at a time; a new press replaces it (the old one's line first).
        // melee fix 14b: root_ok/rx-rz: the bones hit's body root (world) for the entity search (searched: done once); src: where the entity came from.
        struct DamageWatch { bool on; uint64_t entity; float start; bool started; int64_t press_ms, contact_to_press; bool root_ok, searched; float rx, ry, rz; std::string src; };
        std::mutex        g_dw_mx;
        DamageWatch       g_dw{};
        std::atomic<bool> g_dw_on{ false };

        int64_t mono_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        bool health_raw(uint64_t entity, float& out)
        {
            __try {
                uint64_t e = entity;
                out        = g_health(&e);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Script thread (tick): read, compare, log once.
        void damage_tick()
        {
            DamageWatch w;
            {
                std::lock_guard _{ g_dw_mx };
                if (!g_dw.on) return;
                w = g_dw;
            }
            const int64_t t = mono_ms() - w.press_ms;
            auto end = [&](const std::string& line) {
                spdlog::info("{}", line);
                std::lock_guard _{ g_dw_mx };
                if (g_dw.press_ms == w.press_ms) { g_dw.on = false; g_dw_on = false; }
            };
            if (w.entity == 0) {
                // melee fix 14b: the bones hit's entity from the recorded entities at the body's root (once, at the first tick after the press)
                if (w.root_ok && !w.searched) {
                    float       eh = -1.0f, ed = 0.0f;
                    std::string why;
                    const uint64_t id = fc5::lua::entity_at_root(w.rx, w.ry, w.rz, eh, ed, why);
                    {
                        std::lock_guard _{ g_dw_mx };
                        if (g_dw.press_ms != w.press_ms) return;
                        g_dw.searched = true;
                        if (id != 0) {
                            g_dw.entity  = id;
                            g_dw.start   = eh;
                            g_dw.started = eh >= 0.0f;
                            g_dw.src     = fmt::format("the recorded entity at the body's root ({:.2f} m off), found {} ms after the press", ed, t);
                        }
                        w = g_dw;
                    }
                    if (id == 0) spdlog::info("[Melee] damage watch: no entity at the bones hit's body root ({:.2f}, {:.2f}, {:.2f}): {}; waiting up to 1 s for the game's ray on it", w.rx, w.ry, w.rz, why);
                    else spdlog::info("[Melee] damage watch: entity {:x} (health {:.1f}) from {}", id, eh, w.src);
                }
                if (w.entity == 0) {
                    if (t > 1000) end(fmt::format("[Melee] damage not watched (no watch armed on entity 0): the bones hit had no entity: none from the game's ray within 1 s of the press, {} ({} ms after the contact)",
                                                  w.root_ok ? "none recorded at its body's root" : "no body root to look at", w.contact_to_press));
                    return;
                }
            }
            float h = 0.0f;
            if (!health_raw(w.entity, h)) {
                end(fmt::format("[Melee] damage watch of entity {:x}: a fault reading its health, stopped", w.entity));
                return;
            }
            if (!w.started) {
                std::lock_guard _{ g_dw_mx };
                if (g_dw.press_ms == w.press_ms) { g_dw.start = h; g_dw.started = true; }
                w.start = h;
            }
            const bool changed = std::abs(h - w.start) > 0.01f;
            if (!changed) fc5::meleedmgwatch::arm(w.entity, h, w.press_ms, w.press_ms - w.contact_to_press);   // melee fix 14 probe: once per press, when on
            if (changed) {
                fc5::meleedmgwatch::landed(w.entity, w.start, h, true);
                end(fmt::format("[Melee] damage landed: entity {:x} health {:.1f} -> {:.1f}, {} ms after the press ({} ms after the contact){}", w.entity, w.start, h, t, t + w.contact_to_press,
                                h <= 0.0f ? ": a kill" : ""));
            } else if (t > 1500) {
                fc5::meleedmgwatch::landed(w.entity, w.start, h, false);
                end(fmt::format("[Melee] damage landed: none, entity {:x} health {:.1f} unchanged 1500 ms after the press (the game's blow did no damage)", w.entity, h));
            }
        }
    }

    void watch_damage(uint64_t entity, float health_at_contact, int64_t contact_to_press_ms, bool root_ok, float rx, float ry, float rz, const std::string& src)
    {
        DamageWatch   old{};
        const int64_t now = mono_ms();
        bool          kept = false, waiting = false;
        {
            std::lock_guard _{ g_dw_mx };
            old = g_dw;
            // melee fix 14b (probe 2: the watch of contact 1 replaced 806 ms later by a second press): a watch on an entity is not replaced within
            // its 3 s, nor while meleedmgwatch's write watch still waits for its write; only one still looking for its entity (0) is.
            waiting = fc5::meleedmgwatch::waiting();
            kept    = (old.on && old.entity != 0 && now - old.press_ms < 3000) || waiting;
            if (!kept) {
                g_dw = DamageWatch{ true, entity, health_at_contact, entity != 0 && health_at_contact >= 0.0f, now, contact_to_press_ms, root_ok && entity == 0, false, rx, ry, rz,
                                    entity != 0 ? (src.empty() ? std::string{ "the press" } : src) : std::string{} };
                g_dw_on = true;
            }
        }
        if (kept) {
            spdlog::info("[Melee] damage watch{} kept: this press {} ms after its own is not watched (a watch is not replaced within its 3 s unless it caught its write){}",
                         old.on && old.entity != 0 ? fmt::format(" of entity {:x}", old.entity) : std::string{}, now - old.press_ms, waiting ? "; meleedmgwatch's write watch still waiting" : "");
            return;
        }
        if (old.on) spdlog::info("[Melee] damage watch {}replaced by the next press {} ms after its own (it had found no entity yet)", old.entity ? fmt::format("of entity {:x} ", old.entity) : std::string{},
                                 now - old.press_ms);
        if (entity != 0) spdlog::info("[Melee] damage watch: entity {:x} from {}", entity, src.empty() ? std::string{ "the press" } : src);
        else if (!root_ok) spdlog::info("[Melee] damage watch: no entity and no body root for this press (a test body or the ray's press without an entity): looking only for the game's ray on it");
    }

    void watch_entity(uint64_t entity, float health)
    {
        std::lock_guard _{ g_dw_mx };
        if (!g_dw.on || g_dw.entity != 0 || entity == 0 || mono_ms() - g_dw.press_ms > 1000) return;
        g_dw.entity  = entity;
        g_dw.start   = health;   // read at this ray answer: damage already landed before it is not seen (the line then says no change)
        g_dw.started = health >= 0.0f;
    }

    void submit(const Seg* segs, int n)
    {
        if (g_failed.load()) return;
        std::lock_guard _{ g_mx };
        g_segs_n = std::min(n, 8);
        std::copy(segs, segs + g_segs_n, g_segs);
        g_seg_pending = true;
    }

    bool take(Result& out)
    {
        if (!g_res_ready.exchange(false)) return false;
        std::lock_guard _{ g_mx };
        out = g_res;
        return true;
    }

    bool due() { return g_seg_pending.load() || g_test_pending.load() || g_probe_pending.load() || g_dw_on.load(); }

    void submit_probe(const Seg& seg)
    {
        if (g_failed.load()) return;
        std::lock_guard _{ g_mx };
        g_probe_seg     = seg;
        g_probe_pending = true;
    }

    bool take_probe(Hit& out)
    {
        if (!g_probe_ready.exchange(false)) return false;
        std::lock_guard _{ g_mx };
        out = g_probe_hit;
        return true;
    }

    bool ground_z(float x, float y, float z, float& out)
    {
        if (g_failed.load() || !ready()) return false;
        static Out hits[kMaxHits];
        const float s[3]{ x, y, z }, d[3]{ 0.0f, 0.0f, -8.0f };
        const int   n = cast_raw(s, d, g_mask.load(), 6, hits);
        if (n < 0) { fail(); return false; }
        const uint64_t me = player_id();
        float          best = 1e30f;
        for (int i = 0; i < n; ++i) {
            if (hits[i].entity != 0 && hits[i].entity == me) continue;
            if (hits[i].hit.along < best) {
                best = hits[i].hit.along;
                out  = hits[i].hit.pz;
            }
        }
        return best < 1e29f;
    }

    void tick()
    {
        if (g_failed.load() || !ready()) {
            g_seg_pending = false;
            g_test_pending = false;
            g_probe_pending = false;
            g_dw_on = false;
            return;
        }
        struct DmgWatchTick   // meleedmgwatch (melee fix 14 probe): marks this tick as the frame point for its log
        {
            DmgWatchTick() { fc5::meleedmgwatch::tick_begin(); }
            ~DmgWatchTick() { fc5::meleedmgwatch::tick_end(); }
        } dmg_watch_tick;
        damage_tick();   // melee fix 13
        static Out out[kMaxHits];
        const uint64_t me = player_id();
        // weaponcollide: its one segment, every hit asked for (flags 6) so the player's own shape and a character in the way do not hide the wall behind.
        if (g_probe_pending.exchange(false)) {
            Seg sg;
            {
                std::lock_guard _{ g_mx };
                sg = g_probe_seg;
            }
            const float s[3]{ sg.x0, sg.y0, sg.z0 }, d[3]{ sg.x1 - sg.x0, sg.y1 - sg.y0, sg.z1 - sg.z0 };
            const int   n = cast_raw(s, d, g_mask.load(), 6, out);
            if (n < 0) { fail(); return; }
            g_casts.fetch_add(1, std::memory_order_relaxed);
            Hit   wall{};
            float bw = 1e30f;
            for (int i = 0; i < n; ++i) {
                if (out[i].entity != 0 && out[i].entity == me) continue;
                if (out[i].health > 0.0f) continue;        // a character or an animal: melee's business, not the wall's
                if (out[i].hit.along < 0.03f) continue;     // the start is inside it
                keep(wall, bw, out[i]);
            }
            std::lock_guard _{ g_mx };
            g_probe_hit   = wall;
            g_probe_ready = true;
        }
        if (g_test_pending.exchange(false)) {
            float s[3], d[3];
            int64_t mk;
            int     fl;
            {
                std::lock_guard _{ g_mx };
                std::copy(g_test_s, g_test_s + 3, s);
                std::copy(g_test_d, g_test_d + 3, d);
                mk = g_test_mask;
                fl = g_test_flags;
            }
            const uint32_t mask  = mk < 0 ? g_mask.load() : uint32_t(mk);
            const int      flags = fl < 0 ? g_flags.load() : fl;
            const int      n     = cast_raw(s, d, mask, flags, out);
            if (n < 0) { fail(); return; }
            spdlog::info("[GameRay] test: ({:.2f}, {:.2f}, {:.2f}) along ({:.2f}, {:.2f}, {:.2f}), mask {:#x} flags {}: {} hits, the player is {:x}", s[0], s[1], s[2], d[0], d[1], d[2], mask, flags, n, me);
            for (int i = 0; i < n; ++i) {
                const auto& h = out[i].hit;
                spdlog::info("[GameRay]   hit {}: at ({:.2f}, {:.2f}, {:.2f}), normal ({:.2f}, {:.2f}, {:.2f}), along {:.3f}, body {}, entity {:x}{}, health {:.1f}, then {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x}", i, h.px, h.py, h.pz, h.nx, h.ny, h.nz, h.along, h.body,
                             out[i].entity, out[i].entity == me ? " (the player)" : "", out[i].health, h.rest[0], h.rest[1], h.rest[2], h.rest[3], h.rest[4], h.rest[5], h.rest[6], h.rest[7], h.rest[8], h.rest[9], h.rest[10], h.rest[11], h.rest[12], h.rest[13], h.rest[14], h.rest[15]);
            }
        }
        if (!g_seg_pending.exchange(false)) return;
        Seg segs[8];
        int ns;
        {
            std::lock_guard _{ g_mx };
            ns = g_segs_n;
            std::copy(g_segs, g_segs + ns, segs);
        }
        Result res{};
        const uint32_t mask  = g_mask.load();
        const int      flags = g_flags.load();
        // "along" is compared within one segment: a segment's nearest of each kind, the first segment that has one kept.
        for (int k = 0; k < ns; ++k) {
            const float s[3]{ segs[k].x0, segs[k].y0, segs[k].z0 }, d[3]{ segs[k].x1 - segs[k].x0, segs[k].y1 - segs[k].y0, segs[k].z1 - segs[k].z0 };
            const int   n = cast_raw(s, d, mask, flags, out);
            if (n < 0) { fail(); return; }
            g_casts.fetch_add(1, std::memory_order_relaxed);
            g_hits.fetch_add(uint64_t(n), std::memory_order_relaxed);
            Hit   body{}, part{}, wall{};
            float bb = 1e30f, bp = 1e30f, bw = 1e30f;
            for (int i = 0; i < n; ++i) {
                if (out[i].entity != 0 && out[i].entity == me) continue;
                if (out[i].health > 0.0f) {
                    keep(body, bb, out[i]);
                    if (out[i].hit.rest[3] != 0) {
                        keep(part, bp, out[i]);
                        if (g_has_parts.size() > 512) g_has_parts.clear();
                        g_has_parts.insert(out[i].entity);
                    }
                } else {
                    keep(wall, bw, out[i]);
                }
            }
            if (g_fine.load()) {
                if (part.any) body = part;
                else if (body.any && g_has_parts.contains(body.entity)) body = Hit{};
            }
            // meleereact: behind a wall on this segment. The world's own shapes (no entity) always hide; an object with an entity (a prop, a vehicle, what the
            // character carries) only when it is clearly nearer than the body (40 cm), so a rifle held before a chest does not shield it.
            if (g_wall_blocks.load() && body.any && wall.any && wall.along < body.along && (wall.entity == 0 || wall.along < body.along - 0.40f)) body = Hit{};
            if (k == 0) res.blade = wall;
            if (segs[k].ahead) {
                if (body.any && !res.body_ahead.any) res.body_ahead = body;
            } else {
                if (body.any && !res.body.any) res.body = body;
                if (wall.any && !res.wall.any) res.wall = wall;
            }
        }
        std::lock_guard _{ g_mx };
        g_res       = res;
        g_res_ready = true;
    }

    void test(const float start[3], const float delta[3], int64_t mask, int flags)
    {
        std::lock_guard _{ g_mx };
        std::copy(start, start + 3, g_test_s);
        std::copy(delta, delta + 3, g_test_d);
        g_test_mask    = mask;
        g_test_flags   = flags;
        g_test_pending = true;
    }

    void set_mask(uint32_t mask) { g_mask = mask; }
    void set_flags(int flags) { g_flags = flags; }
    void set_fine(bool on) { g_fine = on; }
    void set_wall_blocks(bool on) { g_wall_blocks = on; }

    std::string status()
    {
        return fmt::format("{}, mask {:#x} flags {}, {} casts, {} hits", g_failed.load() ? "turned off by a fault" : g_tried ? (g_ok ? "ready" : "not usable") : "not used yet", g_mask.load(), g_flags.load(), g_casts.load(), g_hits.load());
    }
}
