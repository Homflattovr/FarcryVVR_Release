#pragma once
// padglyphs (FarCry5PadGlyph.cpp): the button icons drawn for the VR controllers, and their BC3 (DXT5) compression. Plain C++, no Windows or
// D3D: tools\padglyphs\preview.cpp builds the same file with g++ to look at every icon as a PNG.
#include <cstdint>

namespace fc5::padglyph
{
    // What the player presses on the controllers, as an icon.
    enum class Icon : uint8_t
    {
        None,
        FaceA,        // right A (Touch), right A (Index)
        FaceB,        // right B
        FaceX,        // left X (Touch), left A (Index)
        FaceY,        // left Y (Touch), left B (Index)
        FaceX2,       // left X pressed twice (drivebuttons' handbrake)
        TrigL,        // left trigger
        TrigR,        // right trigger
        GripL,        // left grip
        GripR,        // right grip
        LGripB,       // left grip held + right B (throw, RB)
        Menu,         // the menu button (left controller)
        LGripMenu,    // left grip held + menu button (map, Back)
        StickL,       // left stick
        StickR,       // right stick
        StickLPress,  // left stick pressed
        StickRPress,  // right stick pressed
        StickLUp,     // left stick pushed up / down / left / right (keyboard W A S D)
        StickLDown,
        StickLLeft,
        StickLRight,
        LGripStick,   // left grip held + a stick pushed (the pad's D-pad)
        LGripUp,
        LGripDown,
        LGripLeft,
        LGripRight,
        Count
    };

    // Letters on the face buttons: Touch-style (right A B, left X Y; also Pico, Reverb G2) or Index-style (A and B on both hands, with L / R).
    enum class Letters : uint8_t
    {
        Touch,
        Index
    };

    const char* icon_name(Icon i);

    // The icon into rgba (w * h * 4 bytes, straight alpha, as the game's own icons), centred, scaled to the smaller side (64 = the game's
    // usual icon; 96 for its large prompts; 92 x 64 for a wide keyboard key).
    void render(Icon icon, Letters letters, int w, int h, uint8_t* rgba);

    // BC3 / DXT5 of rgba (w and h multiples of 4): (w / 4) * (h / 4) blocks of 16 bytes, rows of blocks top down.
    void encode_bc3(const uint8_t* rgba, int w, int h, uint8_t* out);

    // FNV-1a 64 of the texture's bytes (block rows of row_bytes each, row_pitch apart): how the game's icons are recognised.
    uint64_t hash_rows(const void* data, int row_bytes, int rows, int row_pitch);
}
