#pragma once
// bonemap on|off (debug, off by default, not saved; F11 Debug > "Bone and part map"; author 2 Nov 10:30): every bone of every first-person draw (the held weapon and its attachments, the gloves, the bare arms and
// their body draw, sleeves) as a dot with its name, drawn over the eye picture where the draw puts it.
//  - The names come from the game's own model files (FarCry5BoneNames.h, made by tools\bonemap\gen.py; research\2026-11-02_bonemap.md):
//    a draw is known by its index count (the mod's part and weapon ids) and, where two models share a count, by how many palette
//    slots its vertices use. Palette slot i of a skinned draw (VS b3, matrix i) is the model's i-th skin bone.
//  - A bone's dot is its joint (the model's rest joint through the slot's palette matrix and the object transform, VS b1), a rigid
//    draw's its vertices' centre; both placed in the drawn world as the weapon pass places them (reloadmagmodel's mapping).
//  - Unknown draws are labelled by id only: "count/slot" (the ids reloadmagmesh and reloadhandlepart take).
//  - The game's body (bonemapbody): the player's body bones as GetBonePos gives them (FarCry5Lua.cpp, every 100 ms), in magenta.
// Nothing here changes what the game draws: it reads the draws' constants and draws dots and text over the picture at present.
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::bonemap
{
    enum Group : int { kWeapon = 1, kHand = 2, kArm = 3, kBody = 4, kOther = 5 };
    enum Labels : int { kNames = 0, kIds = 1, kBoth = 2 };

    struct Opts
    {
        bool  on{ false };
        bool  show[6]{ false, true, true, true, true, true };   // by Group
        bool  hidden{ true };       // bonemaphidden: the draws the mod hides (the bare arms with hand models on), dimmed
        bool  game_body{ false };   // bonemapbody: the game's body bones (GetBonePos)
        int   labels{ kNames };     // bonemaplabels names|ids|both
        float size{ 1.0f };         // bonemapsize: text and dot scale
        int   max{ 120 };           // bonemapmax: labels drawn at most (nearest the view's centre first)
        float near_m{ 0.0f };       // bonemapnear: only bones within this of either controller (m; 0: all)
        bool  joints{ true };       // bonemapat joints|skin: a skinned bone at its joint, or at its vertices' centre
    };
    Opts opts();
    bool on();
    bool take_dump();   // bonemapdump: true once after the command (the next frame's draws are logged)

    // A draw by its index count. max_bone: the highest palette slot its vertices use (-1 unknown). indirect: an indirect draw (its
    // count rounded up to 192 is tried as well: the gloves' 14316 indices are drawn as 14400). Returns the kDraws index or -1; alts:
    // how many other models share that count and slot count.
    int         find(uint32_t count, int max_bone, bool indirect, int& alts);
    // Every first-person model (kind 2-4) find could take for this count (indirect: the padded counts too), up to max; returns how many.
    int         candidates(uint32_t count, bool indirect, int* out, int max);
    uint32_t    index_count(int draw);   // the model's own index count (an indirect draw's is rounded up)
    const char* model(int draw);
    int         kind(int draw);     // 1 weapon, 2 bare arms, 3 hand wear, 4 sleeves and outfits
    int         nbones(int draw);   // 0: a rigid draw
    const char* slot_name(int draw, int slot);
    int         slot_group(int draw, int slot);
    bool        slot_joint(int draw, int slot, float out[3]);   // rest-space joint (m)

    const uint8_t* glyph(char c);   // 8 x 14 bitmap (bit 7 left), rows top down
    constexpr int  kGlyphW = 8, kGlyphH = 14;

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
    std::string status();   // F11 line: draws named this frame, unknown ones
    void        note_status(int named, int unknown, int bones, int body);
}
