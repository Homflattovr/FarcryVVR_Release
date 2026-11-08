// gameweapon (1 Nov, research/2026-11-01_weapon_data.md): the mod knows a weapon by the largest mesh of the weapon pass, the game by its
// item. The item the game says the player holds (GetEquippedItemID, FarCry5Lua equipped_item, asked every 150 ms on the script thread) is
// kept for the weapon id drawn once both stayed the same for 45 frames, never while a throwable is out (its draws carry the throwable's
// id). FarCry5WeaponTable.h then says what the game's data says about it: its class (bow, pistol, rifle...), one- or two-handed, the
// holster it is carried on, the sight bone and ADS FOVs. Used by sidearmgame (FarCry5Holster.cpp) and bowgame (FarCry5Bow.cpp).
#include "FarCry5GameWeapon.h"
#include "FarCry5Lua.h"

#include <map>
#include <mutex>

#include <spdlog/spdlog.h>

namespace fc5::gameweapon
{
    namespace
    {
        std::mutex                   g_mx;
        std::map<uint32_t, uint64_t> g_item;   // weapon id -> the game's item
        uint32_t                     g_sig    = 0;
        uint64_t                     g_cand   = 0;
        int                          g_frames = 0;
        // bare hands (fistsnotmain, author 7 Nov 10:58: in the car the game put his fists in his hands, and the sling then showed and drew
        // them instead of his AR-C): the weapon ids drawn while the game says the player holds nothing (item 0) for kBareFrames frames in a
        // row. An id later seen with a real item is taken off again.
        constexpr int                kBareFrames = 30;
        std::map<uint32_t, bool>     g_bare;
        uint32_t                     g_zero_sig    = 0;
        int                          g_zero_frames = 0;
    }

    void frame(uint32_t sig, bool quiet)
    {
        uint64_t item = 0;
        const bool fresh = fc5::lua::equipped_item(item);
        if (!quiet && sig != 0 && fresh && item == 0) {
            if (sig == g_zero_sig) ++g_zero_frames;
            else {
                g_zero_sig    = sig;
                g_zero_frames = 0;
            }
            if (g_zero_frames == kBareFrames) {
                std::lock_guard _{ g_mx };
                if (!g_bare[sig]) {
                    g_bare[sig] = true;
                    spdlog::info("[Weapon] gameweapon: weapon {} drawn while the game holds no item: the bare hands (never the main weapon, the sling's or the game's last weapon)", sig);
                }
            }
        } else {
            g_zero_frames = 0;
        }
        if (quiet || sig == 0 || !fresh || item == 0) {
            g_frames = 0;
            return;
        }
        if (sig == g_sig && item == g_cand) ++g_frames;
        else {
            g_sig    = sig;
            g_cand   = item;
            g_frames = 0;
        }
        if (g_frames != 45) return;
        std::lock_guard _{ g_mx };
        uint64_t& kept = g_item[sig];
        if (kept == item) return;
        kept           = item;
        if (const auto b = g_bare.find(sig); b != g_bare.end() && b->second) {
            const auto* bw = fc5::weapons::find(item);
            if (!bw || bw->cls != kFistClass) {
                b->second = false;
                spdlog::info("[Weapon] gameweapon: weapon {} now held as item {:x}: no longer taken for the bare hands", sig, item);
            }
        }
        const auto* w  = fc5::weapons::find(item);
        if (w)
            spdlog::info("[Weapon] gameweapon: weapon {} is the game's item {:x} ({}): class {}, {}, holster {}, sight bone {}", sig, item, w->name, w->cls,
                         w->one_handed ? "one-handed" : "two-handed", w->holster, w->sight);
        else
            spdlog::info("[Weapon] gameweapon: weapon {} is the game's item {:x} (not in the weapon table)", sig, item);
    }

    const fc5::weapons::Info* info(uint32_t sig)
    {
        if (sig == 0) return nullptr;
        std::lock_guard _{ g_mx };
        const auto it = g_item.find(sig);
        return it == g_item.end() ? nullptr : fc5::weapons::find(it->second);
    }

    bool bare_hands(uint32_t sig)
    {
        if (sig == 0) return false;
        std::lock_guard _{ g_mx };
        if (const auto b = g_bare.find(sig); b != g_bare.end() && b->second) return true;
        const auto it = g_item.find(sig);
        const auto* w = it == g_item.end() ? nullptr : fc5::weapons::find(it->second);
        return w && w->cls == kFistClass;
    }

    uint64_t item(uint32_t sig)
    {
        if (sig == 0) return 0;
        std::lock_guard _{ g_mx };
        const auto it = g_item.find(sig);
        return it == g_item.end() ? 0 : it->second;
    }

    std::string status()
    {
        std::lock_guard _{ g_mx };
        std::string out;
        for (const auto& [sig, item] : g_item) {
            const auto* w = fc5::weapons::find(item);
            out += fmt::format(" {}={}", sig, w ? w->name : "?");
        }
        return fmt::format("{} weapons known:{}", g_item.size(), out.empty() ? " none" : out);
    }
}
