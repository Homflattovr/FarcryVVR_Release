#pragma once

// sunshare (pipeline review, 6 Nov 2026, item 5): the sun's cascaded shadow map is drawn once per stereo pair. Both eye calls
// replay the same game frame with the same sun, and the cascade fit is widened to both eyes (shadowfit, default on), so the right
// eye's call reuses what the left call drew: its depth-only draws and its clears into the cascade target are skipped, and the
// cascade rows it uploads (CSMSlice0..2) are set to the rows the map was drawn with.
//
// What 10 Oct's "sun shadows once per pair" did differently (black rogue shadows, DECISIONS 2026-10-10): it skipped the draws but
// not the clears, into every registered target, which could include the far-shadow (ASM) atlas, whose tiles the right call still
// allocated. Here only the target whose shape matches the cascade buffer's ShadowMapSize is shared, never the ASM atlas or its
// 512x256 temporary, and the clears go with the draws. Every 150th pair the right call draws its own map and the GPU counts the
// texels that differ from the left call's ([SunShare] check pair lines).
//
// sunstagger N (on top of sunshare): the far cascades (slices 1 and 2) are redrawn every N/2 and N pairs instead of every pair,
// sooner when their fit moved more than sunstaggerdrift texels; between refreshes the lighting is given the rows each slice was
// drawn with, so a slice's content and its rows always match. Only when the cascades are slices of a texture array drawn one slice
// at a time (checked on the first pairs; otherwise the stagger stays off and says why in the log).
//
// All default off, not saved.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::sunshare
{
    void set_on(bool on);
    bool on();
    void set_stagger(int n);          // 1 = every pair (off)
    int  stagger();
    void set_drift(float texels);     // a far slice whose fit moved more than this is redrawn at the next pair (default 0)
    void set_check_every(int pairs);  // 0 = no check pairs (default 150)

    // Any constant buffer upload of 224..512 B on the render thread, before it reaches the GPU (scan_matrices). When it is a
    // cascade buffer (ShadowMapSize in c10, three slices of a shadow projection in c0-c8), learns the map size and may rewrite
    // the slice rows. call: g_frame_call (1 left, 2 right, 3 scope).
    void note_cb(int call, uint32_t size, float* d);

    // A depth-only draw target seen in the game's call: the cascade target if its shape matches the map size. asm_atlas,
    // asm_tmp: the far-shadow atlas and its temporary when known (never shared).
    void consider(ID3D11Resource* res, ID3D11Resource* asm_atlas, ID3D11Resource* asm_tmp);
    bool is_cascade(ID3D11Resource* res);
    // The array slice a depth view writes (one-slice view of a texture array), or -1.
    int  slice_of(ID3D11DepthStencilView* dsv);
    // The depth-target list was cleared (every 5 s): learn the target again (a freed target's address can be reused).
    void forget();

    // Around the pair (hk_frame, render thread): before the game call; between the calls; after the right call.
    void pair_begin();
    void between_calls(ID3D11DeviceContext* ctx);
    void after_right(ID3D11DeviceContext* ctx);
    void poll(ID3D11DeviceContext* ctx);
    // The scope call drew into the cascade target (scope camera's fit): every slice is redrawn next pair.
    void scope_drew();

    // A draw with the cascade target bound (slice: slice_of the bound view): true = skip it.
    bool skip_draw(int call, ID3D11Resource* res, int slice);
    // A clear of a depth view: true = the caller skips the game's clear (this may have cleared the slices being redrawn itself).
    bool skip_clear(ID3D11DeviceContext* ctx, int call, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil);

    std::string status();
}
