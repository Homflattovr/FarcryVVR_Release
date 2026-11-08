#pragma once
// reloadhintstyle glow|boxes (FarCry5ReloadHint.cpp; author 5 Nov 08:28): with reloadhighlight on, the gun's part to
// work next glows on the game's own model instead of a box round it.
//  - Each part is drawn a second time right after the game's draw of it, with the game's vertex shader and constants (only that part's bones
//    placed, every other bone collapsed onto it), into a hint mask at the size of the weapon pass's depth: once with the game's depth test (where
//    the part is seen) and once without (where it is, also behind the hands). The red dot sight's window mask works the same way.
//  - At present the zones overlay (FarCry5GestureZones.cpp) lights those pixels of the eye picture: a soft coloured glow over the visible part, a
//    brighter rim at its outline, a halo round it, and faint moving lines where it is hidden.
//  - Two groups, one colour each: the magazine (orange: the empty one in the gun, drop it; mint: the one at the hip, take it; yellow: a ghost of
//    the magazine seated in the well while one is in the hand, brighter once close enough) and the charging handle, slide or pump (blue).
//  - A part with no mask this frame (not known on the model, a draw the mask could not take) keeps its box, as before.
//  - reloadhintglow2 (author 5 Nov 12:47): the handle group also takes a lever action's lever and a revolver's cylinder, and the handle glows
//    (dimmer) wherever its blue ball would show; its glow is brighter than the magazine's (strength 1.7).
#include <d3d11.h>

#include <cstdint>
#include <vector>

namespace fc5::reloadhint
{
    enum { kMag = 0, kHandle = 1, kGroups = 2 };

    // A mask draw of the draw reload_mag_draw looks at, run after the game's draw of it (FarCry5Stereo::reload_hint_after).
    struct Job
    {
        int                group{ kMag };
        bool               seen{ true };    // also the depth-tested pass (false: only the see-through one, the well's ghost)
        bool               rigid{ false };  // data: a rigid draw's object constants (32 floats), else a skinned draw's palette (3072 floats)
        std::vector<float> data;
    };
    void begin_draw(UINT count, UINT start, INT base, int frame);   // reload_mag_draw: a new draw (the queue emptied)
    void queue(Job&& j);
    // The group's colour this frame (rgb) and strength (1 normal, more once the hand is close enough).
    void set_colour(int group, float r, float g, float b, float strength);

    // For the overlay: the mask of this eye frame, eye pixel -> mask uv (q * mscale + moff), and each group's colour (strength 0: not drawn).
    struct Composite
    {
        ID3D11ShaderResourceView* srv{ nullptr };
        float                     mscale[2]{}, moff[2]{};
        float                     texel[2]{};   // the mask's texel (uv)
        float                     col[kGroups][4]{};
    };
    bool composite(int frame, float eye_w, float eye_h, Composite& c);
    bool drawn(int group, int frame);   // the group went into this frame's mask: its box and ball are left out
}
