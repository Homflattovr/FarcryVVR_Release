#pragma once
// playerskin (author 15 Nov 2026 13:51Z; research\2026-11-15_playerskin\README.md).
// Commands (nothing saved):
//   skinequip <hex id | table name>[@shop|@desc|@other] [force]   the game's EquipWolfskinItem for the player (force: then ForceCharacterCustomization)
//                                                                     (a name: its desc id, or its shop id when it has none; @ picks the id)
//   skinwardrobe ForceCharacterCustomization alone: the game opens its wardrobe menu (15 Nov run R2; skinforce: the same)
//   skinworn     every table row asked with HasCustomizationEquipped and IsWearingWolfskinItem; the true ones logged
//   skinmenu     OpenAvatarCreationMenu (test: 15 Nov skinsM11, the game paused on a black screen for 30+ s; pad B did not leave it)
// F11 Debug > Player look. The game's calls are in FarCry5Lua.cpp, on the script thread; the items in FarCry5SkinTable.h.
#include <functional>
#include <string>

namespace fc5::skin
{
    bool command(const std::string& cmd, const std::string& arg);
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
