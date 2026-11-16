// playerskin (author 15 Nov 2026 13:51Z, 14:40Z; research\2026-11-15_playerskin\README.md): the player's look by command and from F11 Debug > Player look.
// See FarCry5Skin.h for the commands; the game's calls are in FarCry5Lua.cpp (skin_equip, skin_force, skin_worn, skin_menu).
#include "FarCry5Skin.h"
#include "FarCry5SkinTable.h"
#include "FarCry5Lua.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::skin
{
    namespace
    {
        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
            return s;
        }
        bool is_hex(const std::string& s)
        {
            if (s.empty() || s.size() > 18) return false;
            size_t i = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
            if (i >= s.size()) return false;
            for (; i < s.size(); ++i)
                if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
            return true;
        }
        uint64_t id_of(const SkinEntry& e) { return e.desc ? e.desc : e.shop; }
    }

    bool command(const std::string& cmd, const std::string& arg)
    {
        if (cmd == "skinworn") { fc5::lua::skin_worn(); return true; }
        if (cmd == "skinwardrobe" || cmd == "skinforce") { fc5::lua::skin_force(); return true; }   // ForceCharacterCustomization opens the wardrobe (run R2, 15 Nov)
        if (cmd == "skinmenu") { fc5::lua::skin_menu(); return true; }
        if (cmd != "skinequip") return false;
        std::string a = arg, opt;
        if (const size_t sp = a.find(' '); sp != std::string::npos) { opt = lower(a.substr(sp + 1)); a = a.substr(0, sp); }
        std::string which;
        if (const size_t at = a.find('@'); at != std::string::npos) { which = lower(a.substr(at + 1)); a = a.substr(0, at); }
        const bool force = opt.find("force") != std::string::npos && opt.find("noforce") == std::string::npos;   // run R2: it opens the wardrobe menu
        if (a.empty()) { spdlog::info("[Skin] skinequip: no id or name given"); return true; }
        if (is_hex(a) && which.empty()) {
            const uint64_t id = std::strtoull(a.c_str(), nullptr, 16);
            if (id == 0) { spdlog::info("[Skin] skinequip: id 0"); return true; }
            fc5::lua::skin_equip(id, force, nullptr);
            return true;
        }
        const std::string want = lower(a);
        for (const auto& e : kSkins) {
            if (lower(e.name) != want) continue;
            // An outfit (run R3, 15 Nov: its own ids do not equip): each of its parts' desc ids, as the game's wardrobe would wear them.
            if ((which.empty() && e.desc == 0 && *e.parts) || which == "parts") {
                std::vector<std::string> ids;
                for (const char* p = e.parts; *p;) {
                    const char* q = std::strchr(p, ' ');
                    ids.emplace_back(p, q ? size_t(q - p) : std::strlen(p));
                    p = q ? q + 1 : p + std::strlen(p);
                }
                int n = 0, skipped = 0;
                for (size_t k = 0; k < ids.size(); ++k) {
                    const uint64_t id = ids[k] == "-" ? 0 : std::strtoull(ids[k].c_str(), nullptr, 16);
                    if (id == 0) { ++skipped; continue; }
                    fc5::lua::skin_equip(id, force && k + 1 == ids.size(), fmt::format("{} part {}", e.name, k + 1).c_str());
                    ++n;
                }
                spdlog::info("[Skin] skinequip {}: an outfit of {} parts, {} asked{}", e.name, ids.size(), n, skipped ? fmt::format(", {} without an item id (not worn)", skipped) : std::string{});
                return true;
            }
            const uint64_t id = which == "shop" ? e.shop : which == "desc" ? e.desc : which == "other" ? e.other : id_of(e);
            if (id == 0) { spdlog::info("[Skin] skinequip {}: it has no {} id", e.name, which.empty() ? "desc or shop" : which); return true; }
            fc5::lua::skin_equip(id, force, e.name);
            return true;
        }
        spdlog::info("[Skin] skinequip {}: not a hex id nor a name in the table", a);
        return true;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        static int  slot_at = 0;
        static int  gender_at = 0;   // 0 all, 1 male, 2 female
        static char filter[64]{};
        static bool show_prem = false;
        static std::vector<const char*> slots;
        if (slots.empty()) {
            std::set<std::string> seen;
            slots.push_back("(all)");
            for (const auto& e : kSkins)
                if (seen.insert(e.slot).second) slots.push_back(e.slot);
        }
        hint("Changes what your character wears. It's real: if you save, it stays in that save.");
        // (No buttons for skinmenu / skinwardrobe: in the 15 Nov runs both paused the game into a menu that Escape and pad B did not leave.)
        if (ImGui::Button("Log what is worn##skinworn")) cmds += "skinworn\n";
        hint("Writes what you wear to the log.");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo("Slot##skinslot", &slot_at, slots.data(), int(slots.size()));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        static const char* kG[]{ "Both", "Male", "Female" };
        ImGui::Combo("Gender##skingender", &gender_at, kG, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Name contains##skinfilter", filter, sizeof(filter));
        ImGui::Checkbox("Show premium items (paid: use only those you own)##skinprem", &show_prem);
        const std::string status = fc5::lua::skin_status();
        ImGui::TextWrapped("%s", status.empty() ? "Nothing asked yet." : status.c_str());
        const std::string want = lower(filter);
        const std::string slot = slots[size_t(std::clamp(slot_at, 0, int(slots.size()) - 1))];
        ImGui::BeginChild("##skinlist", ImVec2(0.0f, 320.0f), true);
        int shown = 0;
        for (const auto& e : kSkins) {
            if (slot_at != 0 && slot != e.slot) continue;
            if (gender_at == 1 && e.gender == 'F') continue;
            if (gender_at == 2 && e.gender == 'M') continue;
            if (e.premium && !show_prem) continue;
            if (!want.empty() && (lower(e.name) + " " + lower(e.group)).find(want) == std::string::npos) continue;
            ImGui::PushID(&e);
            if (e.desc == 0 && !*e.parts) ImGui::TextDisabled("Wear");   // no id the game wears (run R3)
            else if (ImGui::SmallButton("Wear")) cmds += fmt::format("skinequip {}\n", e.name);
            ImGui::SameLine();
            if (e.premium) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", e.name);
            else ImGui::TextUnformatted(e.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s, shop %016llx, desc %016llx", e.slot, *e.group ? " / " : "", e.group, (unsigned long long)e.shop, (unsigned long long)e.desc);
            ImGui::PopID();
            if (++shown >= 400) { ImGui::TextDisabled("(more: type part of a name)"); break; }
        }
        if (shown == 0) ImGui::TextDisabled("Nothing matches.");
        ImGui::EndChild();
    }
}
