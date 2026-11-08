#pragma once

// GPU time per pass (Metro Exodus VR review, 6 Nov 2026, item 1; cmd gputime passes N): for the next N stereo pairs, a timestamp at every
// change of the bound colour/depth targets and at every switch between draws and compute, in each call of the pair. Each segment is named
// by the call (left, right, scope) and what it drew into (known targets by name: scene depth, sun shadow, far-shadow atlas; others by
// size and format) or, for compute, by the compute shader's census name. After N pairs the mean ms per pair of each name is logged,
// largest first ([PassTime] lines). Read without waiting (D3D11_ASYNC_GETDATA_DONOTFLUSH); off unless asked; nothing changes on screen.
//
// Timestamps mark where the GPU pipeline finishes a segment, so overlapping work shifts a little time between neighbours; the totals and
// the ranking are what this is for.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::passtime
{
    // Start a capture of N pairs (N <= 0: stop).
    void start(int pairs);
    bool active();    // a pair is being recorded now
    bool wanted();    // a capture is running (pairs to record or to read)

    // Around one pair (both eye calls and the scope call), on the render thread, with the immediate context.
    void pair_begin(ID3D11DeviceContext* ctx);
    void pair_end(ID3D11DeviceContext* ctx);
    // Start of a call inside the pair: 1 game call (left eye), 2 extra call (right eye), 3 scope call.
    void call_begin(ID3D11DeviceContext* ctx, int call);

    // OMSetRenderTargets(AndUnorderedAccessViews) on the render thread. label: the depth target's name when it is a known one (scene depth, sun shadow, far-shadow atlas), or nullptr.
    void bind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, const char* label);
    // A dispatch on the render thread: true when the caller should pass a label for this compute shader (it changed).
    bool dispatch_needs_label(const void* cs);
    void dispatch(ID3D11DeviceContext* ctx, const void* cs, const std::string& label);
    // A draw: when the last segment was compute, the draws after it start a new segment under the current targets.
    bool in_compute();
    void resume_draws(ID3D11DeviceContext* ctx);

    // Collects finished pairs (never waits); logs the report once N pairs are read.
    void poll(ID3D11DeviceContext* ctx);
    // The last report's first lines (for F11).
    std::string summary();
}
