// spawncommand (author 4 Nov: in the hand, the held weapon dropped, as a Debug subsection, plus other
// things not meant for the player lying on the ground like mounted guns; research\2026-11-04_weapon_spawn.md): F11 Debug > Spawn command. The list is FarCry5SpawnTable.h; the game's own calls
// are in FarCry5Lua.cpp (spawn_request), on the script thread.
// vehiclespawn (author 5 Nov): F11 Debug > Vehicle spawn, from FarCry5VehicleTable.h (the game's shop
// vehicles); the game's calls in FarCry5Lua.cpp (vehicle_request, vehicle_remove_request).
// spawnfacing, mountedlist, spawnyaw (turret thread 5 Nov, research\2026-11-05_turret): the spawns go where the player faces now (FarCry5Mounted.cpp
// spawn_back_axis), a ground entity turned to that heading; a row of the mounted guns at the top of Spawn command; spawnyaw N (test) adds to the turn.
#include "FarCry5Spawn.h"
#include "FarCry5SpawnTable.h"
#include "FarCry5VehicleTable.h"
#include "FarCry5Lua.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::spawn
{
    namespace
    {
        // spawnyaw N (test, not saved; turret thread 5 Nov): degrees added to the turn of a ground spawn (spawnfacing turns it to the player's
        // heading, assuming the entity's forward is its +y as for the vehicles: if a mounted gun comes facing the player, spawnyaw 180).
        std::atomic<float> g_yaw_add{ 0.0f };
        // mountedlist: the mounted guns of the spawn list, in one row (F11 Debug > Spawn command). The 5 Nov run mounted the 360 mortar with pad X; the
        // 360 M2 and M60 did not mount (held or tapped, looked at or not): the 180 and "dynamic" entries are there to try.
        struct Quick { const char* label; const char* name; };
        constexpr Quick kMountedQuick[]{
            { "M2 360", "turret_ground360_m2hb" },
            { "M2 180", "turret_ground180_m2hb" },
            { "M2 dynamic", "turret_ground360_m2hb.ige_dlz_turret_ground360_m2hb_dynamic" },
            { "M60 360", "turret_ground360_m60e4" },
            { "M60 180", "turret_ground180_m60e4" },
            { "M60 dynamic", "turret_ground360_m60e4.ige_dlz_turret_ground360_m60e4_dynamic" },
            { "Mortar", "turret_ground360_mortar" },
            { "Mortar cluster", "turret_ground360_mortar.cluster" },
        };
    }

    uint64_t pickup_of(uint64_t item)
    {
        for (const auto& e : kTable)
            if (e.item == item) return e.archetype;
        return 0;
    }

    bool command(const std::string& cmd, const std::string& arg, float bx, float by)
    {
        if (cmd == "spawnyaw") {   // spawnyaw N (test, degrees, not saved)
            g_yaw_add = std::clamp(std::strtof(arg.c_str(), nullptr), -180.0f, 180.0f);
            spdlog::info("[Spawn] spawnyaw {:+.0f} deg: added to the turn of ground spawns", g_yaw_add.load());
            return true;
        }
        if (cmd != "spawnhand" && cmd != "spawnground" && cmd != "spawnvehicle" && cmd != "spawnvehiclein" && cmd != "spawnvehicleremove") return false;
        if (cmd == "spawnvehicleremove") {   // vehiclespawn
            fc5::lua::vehicle_remove_request();
            return true;
        }
        const uint64_t id = std::strtoull(arg.c_str(), nullptr, 16);
        // Ahead of the player: against the back axis, level (spawnfacing: where the player faces now, FarCry5Mounted.cpp).
        const float len = std::sqrt(bx * bx + by * by);
        const float fx  = len > 0.01f ? -bx / len : 0.0f;
        const float fy  = len > 0.01f ? -by / len : 1.0f;
        if (cmd == "spawnvehicle" || cmd == "spawnvehiclein") {   // vehiclespawn: spawnvehicle <archetype hex> (placed), spawnvehiclein <archetype hex> (placed, the player seated in it)
            if (id == 0) {
                spdlog::info("[Spawn] {}: no id given", cmd);
                return true;
            }
            float dist = 6.0f;   // (not in the list: a car's distance)
            for (const auto& v : kVehicles)
                if (v.archetype == id) dist = v.dist;
            fc5::lua::vehicle_request(id, fx, fy, dist, cmd == "spawnvehiclein");
            return true;
        }
        if (id == 0) {
            spdlog::info("[Spawn] {}: no id given", cmd);
            return true;
        }
        fc5::lua::spawn_request(cmd == "spawnhand" ? id : 0, cmd == "spawnground" ? id : 0, fx, fy, g_yaw_add.load());
        return true;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        static int  kind_at = 0;
        static char filter[64]{};
        static bool show_marked = false;   // entries the game's own names mark as not for use
        static std::vector<const char*> kinds;
        if (kinds.empty()) {
            std::set<std::string> seen;
            for (const auto& e : kTable)
                if (seen.insert(e.kind).second) kinds.push_back(e.kind);
        }
        hint("In hand: the game adds that item to your inventory and draws it, and the weapon you were holding falls in front of you. On the ground: its world entity appears in front of you. "
             "Added items are real: if you save, they may stay in that save, and a dropped weapon comes back with default attachments.");
        ImGui::SetNextItemWidth(220.0f);
        if (!kinds.empty()) ImGui::Combo("Kind##spawnkind", &kind_at, kinds.data(), int(kinds.size()));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Name contains##spawnfilter", filter, sizeof(filter));
        std::string want = filter;
        std::transform(want.begin(), want.end(), want.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
        ImGui::Checkbox("Also show what the game marks as not for use##spawnmarked", &show_marked);
        hint("Names with DONOTUSE, DELETE_ME, LEGACY, NPC, PLACEHOLDER, E3ONLY, zz_ and the like: cut or unfinished items, shown in orange. They may not work, and one could crash the game.");
        // mountedlist: the mounted guns in one row.
        ImGui::TextUnformatted("Mounted guns:");
        for (const auto& q : kMountedQuick) {
            uint64_t arch = 0;
            for (const auto& e : kTable)
                if (std::strcmp(e.name, q.name) == 0) { arch = e.archetype; break; }
            if (arch == 0) continue;
            ImGui::SameLine();
            if (ImGui::SmallButton(fmt::format("{}##mountedquick", q.label).c_str())) cmds += fmt::format("spawnground {:x}\n", arch);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", q.name);
        }
        hint("Each puts that mounted gun on the ground in front of you, its handles towards you (the Mounted kind below has them all). Walk to it and use it with pad X "
             "(on 5 Nov the 360 mortar mounted; the 360 M2 and M60 did not: try the 180 and dynamic ones). If one comes facing you, spawnyaw 180 turns the next ones.");
        const std::string status = fc5::lua::spawn_status();
        ImGui::TextWrapped("%s", status.empty() ? "Nothing asked yet." : status.c_str());
        ImGui::BeginChild("##spawnlist", ImVec2(0.0f, 320.0f), true);
        const std::string kind = kinds.empty() ? std::string{} : std::string{ kinds[size_t(std::clamp(kind_at, 0, int(kinds.size()) - 1))] };
        int shown = 0;
        for (const auto& e : kTable) {
            // With a name typed, every kind; else the kind chosen.
            if (want.empty()) {
                if (kind != e.kind) continue;
            } else {
                std::string n = e.name;
                std::transform(n.begin(), n.end(), n.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
                if (n.find(want) == std::string::npos) continue;
            }
            if (e.marked && !show_marked) continue;
            ImGui::PushID(&e);
            if (e.item != 0) {
                if (ImGui::SmallButton("In hand")) cmds += fmt::format("spawnhand {:x}\n", e.item);
            } else {
                ImGui::TextDisabled("In hand");
            }
            ImGui::SameLine();
            if (e.archetype != 0) {
                if (ImGui::SmallButton("On the ground")) cmds += fmt::format("spawnground {:x}\n", e.archetype);
            } else {
                ImGui::TextDisabled("On the ground");
            }
            ImGui::SameLine();
            if (e.marked) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", e.name);
            else ImGui::TextUnformatted(e.name);
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("Nothing matches.");
        ImGui::EndChild();
    }

    // vehiclespawn
    void draw_vehicle_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        static int  kind_at = 0;
        static char filter[64]{};
        static bool show_marked = false;   // what the game's own names mark as cut or a test
        static bool get_in      = true;
        static std::vector<const char*> kinds;
        if (kinds.empty()) {
            std::set<std::string> seen;
            for (const auto& v : kVehicles)
                if (seen.insert(v.kind).second) kinds.push_back(v.kind);
        }
        hint("The vehicle appears a few metres in front of you (a helicopter or plane further off), turned the way you face. Face open, flat ground: it is placed on the ground "
             "straight ahead, through anything in between. A boat on land stays on land: face the water from the shore. Spawned vehicles are real: if you save, they may stay in that save.");
        ImGui::Checkbox("Get in after it appears##vehspawnin", &get_in);
        hint("The game seats you in its driver seat, the way a mission script does. Off: it is only placed.");
        ImGui::SetNextItemWidth(220.0f);
        if (!kinds.empty()) ImGui::Combo("Kind##vehspawnkind", &kind_at, kinds.data(), int(kinds.size()));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Name contains##vehspawnfilter", filter, sizeof(filter));
        std::string want = filter;
        std::transform(want.begin(), want.end(), want.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
        ImGui::Checkbox("Also show what the game marks as cut##vehspawnmarked", &show_marked);
        if (ImGui::Button("Remove the vehicles spawned here##vehspawnremove")) cmds += "spawnvehicleremove\n";
        hint("Every vehicle spawned from this menu since the game started, except the one you are in.");
        const std::string status = fc5::lua::spawn_status();
        ImGui::TextWrapped("%s", status.empty() ? "Nothing asked yet." : status.c_str());
        ImGui::BeginChild("##vehspawnlist", ImVec2(0.0f, 320.0f), true);
        const std::string kind = kinds.empty() ? std::string{} : std::string{ kinds[size_t(std::clamp(kind_at, 0, int(kinds.size()) - 1))] };
        int shown = 0;
        for (const auto& v : kVehicles) {
            // With a name typed, every kind (the shop's name or the entity's); else the kind chosen.
            if (want.empty()) {
                if (kind != v.kind) continue;
            } else {
                std::string n = std::string{ v.name } + " " + v.entity;
                std::transform(n.begin(), n.end(), n.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
                if (n.find(want) == std::string::npos) continue;
            }
            if (v.marked && !show_marked) continue;
            ImGui::PushID(&v);
            if (ImGui::SmallButton("Spawn")) cmds += fmt::format("{} {:x}\n", get_in ? "spawnvehiclein" : "spawnvehicle", v.archetype);
            ImGui::SameLine();
            if (v.marked) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", v.name);
            else ImGui::TextUnformatted(v.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v.entity);
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("Nothing matches.");
        ImGui::EndChild();
    }
}
