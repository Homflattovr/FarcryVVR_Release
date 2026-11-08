#pragma once
// throwfile (author 3 Nov: the selected throwable on the chest as soon as it is selected, never a fake throw): a thrown item's model
// and textures, read from the player's own game files (FarCry5ThrowFile.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace fc5::throwfile
{
    // One texture: the file's mips, largest first. dxgi as stored (a non-sRGB format; srgb: read through the sRGB view).
    struct Tex
    {
        std::string          name;   // lower case, no folder or extension (wpn_grenade_01_al)
        bool                 srgb{ false };
        uint32_t             w{ 0 }, h{ 0 }, mips{ 0 }, dxgi{ 0 };
        std::vector<uint8_t> data;
    };
    // One draw of the model's most detailed level. Its indices count from its stream's first vertex.
    struct Part
    {
        uint32_t count{ 0 }, start{ 0 };
        uint32_t stride{ 0 }, vb_off{ 0 };
        int      mat{ 0 };
    };
    struct Model
    {
        uint64_t                      item{ 0 };
        std::string                   file;   // what was read, or why not
        std::vector<uint8_t>          vb;
        std::vector<uint32_t>         ib;
        std::vector<Part>             parts;
        float                         pos_scale{ 0.0f }, uv_range{ 0.0f }, uv_scale{ 0.0f };
        std::vector<std::vector<Tex>> mats;
        std::vector<std::vector<uint8_t>> weapon_cb;   // per material: the pixel shader's Weapon constants (528 bytes), from the material's own values
        uint32_t                      cut_from{ 0 }, cut_to{ 0 };   // the triangles before and after its alpha cut (for the log)
        float                         box[6]{};   // its bounding box in its own axes, metres: min xyz, max xyz (0: none in the file)
    };

    bool     knows(uint64_t item);              // this thrown item has a model in the table
    bool     load(uint64_t item, Model& out);   // reads it (blocking: not on a render thread); false: out.file says why
    uint64_t path_hash(const char* path);       // the archives' name of a file: CRC-64 of its lower-case path with backslashes
}
