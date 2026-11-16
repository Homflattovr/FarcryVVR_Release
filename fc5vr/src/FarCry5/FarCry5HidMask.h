#pragma once

// hidmask (pipeline review, 6 Nov 2026, item 3): the part of each eye image the headset's lens never shows (the runtime's
// hidden-area mesh, XR_KHR_visibility_mask) is stamped into the scene depth right after the game clears it, at the near plane of the
// game's reversed depth (1.0). Every later draw that tests depth fails there, so the G-buffer and the passes that test depth skip
// those pixels. Mode "restore" puts far depth (0.0, sky) back over the same triangles when the scene depth is first unbound, so the
// lighting, fog and post passes that read depth see sky there instead of the near plane. On by default ("hidmask on" = restore; author
// 8 Nov), saved; nothing on screen inside the lens should change. A runtime that gives no hidden area: nothing happens.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::hidmask
{
    enum Mode : int { kOff = 0, kDepth = 1, kRestore = 2 };
    void set_mode(int m);
    int  mode();
    const char* mode_name(int m);
    // How much of the runtime's hidden area is masked (0.1-1; 0.5 = the masked band half as deep, a margin toward the visible area).
    constexpr float kDefaultCover = 0.5f;   // Author 14 Nov 12:44Z ("hidmask on" = 50 %)
    void  set_cover(float c);
    float cover();
    // hidmaskturn (author 14 Nov 16:14Z): no mask in a head turn faster than this many degrees per second (0 = never). head_speed: the
    // head's turn speed at each new pose sample (held about 0.3 s); turning: the mask is left out now.
    constexpr float kDefaultTurnDps = 90.0f;
    void  set_turn_limit(float dps);
    float turn_limit();
    void  head_speed(float dps);
    bool  turning();

    // The runtime's hidden-area triangles of both eyes (once; retried a few times while the runtime gives none). instance/session are
    // the OpenXR handles (XrInstance / XrSession, passed as integers so this header needs no OpenXR include); has_ext: the extension
    // was enabled at instance creation.
    void fetch(uint64_t instance, uint64_t session, bool has_ext);
    bool ready();
    // The runtime reported a changed hidden area: read it again (the old one stays in use until the new one is read).
    void refetch();
    bool wants_fetch();   // no mesh yet, or a re-read asked for

    // Stamp eye 0/1's hidden area into dsv at depth z (1 = near: nothing drawn later passes the reversed depth test; 0 = far: sky).
    // tangents: the eye image's own field (tan of left, right, up, down angles; left and down negative), as the projection the game
    // renders this eye with. Saves and restores the whole context state (ID3DDeviceContextState swap). False if not drawn.
    bool stamp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, int eye, const float tangents[4], float z);

    // hidmask black (author 14 Nov): fill eye 0/1's hidden area black in the finished eye image tex (a render target, or a UAV-only
    // image), so the headset's reprojection on a fast head turn shows black there. Same tangents as stamp. False if not drawn.
    bool black(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4]);

    // afwpereye (AFW): the hidden area of eye 0/1 written as kHiddenMark into an R32F render target (AFW's linear depth copy of that eye's
    // image), so the rebuild never takes a texel from there (the game drew nothing in it: black, or leftovers). False if not drawn.
    constexpr float kHiddenMark = 1.0e30f;
    bool mark(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4]);

    // Counters since start, and the hidden share of each eye image (for the log and F11).
    std::string status();
}
