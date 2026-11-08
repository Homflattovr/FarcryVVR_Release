#pragma once

// hitchlog N (test command, not saved; hitch thread 23 Oct): one "[Hitch]" line for each of the next N stereo pairs longer than
// hitchms (default 35 ms, start to start), giving the CPU time of the game call and of the extra call, and inside each the time in
// the present hook (each mod's on_present, the swap chain's Present, each mod's on_post_present), xrWaitFrame, xrBeginFrame,
// xrEndFrame, the eye copies, the mod's own present-side blocks and blocking read-backs, plus that pair's GPU times once they
// arrive and what the frame was (on foot / vehicle, moving, turning, weapon, menu). The counters are running totals read before
// and after each call: a few atomics per call, always on; the lines only while hitchlog counts down.

// perfsplit on|off (test command, not saved; F11 Debug > Performance switches, 6 Nov): where the render thread's CPU goes, one
// "[PerfSplit]" line per 5 s. Each hook on the render thread is timed once (the outermost one; nanoseconds, not hitchlog's whole
// microseconds per call, which round most hooks to 0), and inside it the time in the original method (the driver, through call_orig).
// The mod's own share of a hook is the difference. Costs about 4 clock reads per hook call while on (~1-2 ms per pair): read the split,
// not the frame rate, while it is on.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace fc5::hitch
{
    // The mod's own blocks (microsecond totals).
    // The hook groups (draws and dispatches, state setters and clears, Map / Unmap / UpdateSubresource, shader and layout creation
    // on the render thread and on other threads) are timed only while hitchlog is on (g_active), the original call included.
    enum Block : int { kHudWorld = 0, kCompose, kOverlays, kAimMarch, kPairCheck, kPollCommands, kStats5s, kSyncRead,
                       kDrawHooks, kStateHooks, kMapHooks, kCreateRender, kCreateOther,
                       kMapOrig, kScanMatrices, kObjConsts, kViewportConsts, kFixLights, kBlockCount };   // inside the Map / Unmap hooks
    inline std::atomic<int64_t>  g_block_us[kBlockCount]{};
    inline std::atomic<uint32_t> g_sync_reads{ 0 };   // blocking Map read-backs done (indirect args, buffer reads)
    inline std::atomic<uint32_t> g_creates{ 0 };      // shaders and input layouts created
    inline std::atomic<bool>     g_active{ false };
    inline std::atomic<uint32_t> g_render_tid{ 0 };

    struct Timer
    {
        Block                                 b;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit Timer(Block blk) : b(blk) {}
        ~Timer() { g_block_us[b] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
    };

    // perfsplit: render thread only, written and read on it (no atomics needed for the sums).
    inline std::atomic<bool> g_split{ false };
    inline int64_t           g_split_hook_ns[kBlockCount]{};   // outermost hooks (the original call included); sub-blocks at any depth
    inline int64_t           g_split_orig_ns[kBlockCount]{};   // inside those outermost hooks: the original methods (driver)
    inline int64_t           g_split_calls[kBlockCount]{};
    inline thread_local int  t_split_depth = 0;                // hooks open on this thread while perfsplit is on
    inline thread_local int  t_split_block = -1;               // the outermost hook's group while it runs on the render thread
    inline thread_local bool t_split_in_orig = false;          // inside a timed original (a nested hook's original is not counted twice)
    bool split_render_thread();                                // this is the render thread (FarCry5Hitch.cpp)

    struct HookTimer
    {
        Block                                 b;
        bool                                  on;
        bool                                  nested = false, split = false;
        std::chrono::steady_clock::time_point t0{};
        explicit HookTimer(Block blk) : b(blk), on(g_active.load(std::memory_order_relaxed))
        {
            if (g_split.load(std::memory_order_relaxed)) {
                nested = true;
                const bool outer = t_split_depth++ == 0;
                // The outermost hook of the render thread; the Map sub-blocks (driver, scan, objects, viewport, lights) at any depth there.
                split = (outer || b >= kMapOrig) && split_render_thread();
                if (split && outer) t_split_block = int(b);
            }
            if (on || split) t0 = std::chrono::steady_clock::now();
        }
        ~HookTimer()
        {
            if (on || split) {
                const auto d = std::chrono::steady_clock::now() - t0;
                if (on) g_block_us[b] += std::chrono::duration_cast<std::chrono::microseconds>(d).count();
                if (split) {
                    g_split_hook_ns[b] += std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
                    g_split_calls[b]++;
                    if (t_split_depth == 1) t_split_block = -1;
                }
            }
            if (nested) t_split_depth--;
        }
    };
    // perfsplit: around the original method in call_orig, inside an outermost render-thread hook.
    struct OrigTimer
    {
        int                                   b;
        std::chrono::steady_clock::time_point t0;
        explicit OrigTimer(int blk) : b(blk), t0(std::chrono::steady_clock::now()) { t_split_in_orig = true; }
        ~OrigTimer()
        {
            t_split_in_orig = false;
            g_split_orig_ns[b] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
    };
    // perfsplit's draw stages (6 Nov, author): inclusive times of named parts of the
    // render thread's draw and dispatch hooks (an original method called inside a part is included in it), one [PerfDraw] line per 5 s
    // while perfsplit is on. Only inside an outermost render-thread hook (t_split_block set), so nothing is counted twice across threads.
    enum Stage : int
    {
        kStDrawIndexed, kStDrawIndexedInst, kStDraw, kStDrawInst, kStDrawIndirect, kStDispatch, kStDispatchIndirect,   // whole hooks
        kStPrelude,            // DrawIndexed(Instanced): probes, menu viewport, before the filters
        kStWeaponDrawWeapon,   // weapon_draw for a weapon-pass draw (first-person camera buffer)
        kStWeaponDrawOther,    // weapon_draw for every other draw (the world, shadows, post, HUD)
        kStWorldChar,          // world_character_draw (inside the above)
        kStProbes,             // chute_probe + vehicle_probe (inside the above)
        kStDlssResolve,        // DLSS at the SMAA draw (inside the above)
        kStTexChecks,          // bonemap, taaskip, lens dirt / eye history / DOF texture swaps
        kStOrigDraw,           // the game's draw itself (and the first-person extra draw)
        kStAfter,              // the restores after the draw, DLSS after-SMAA and the scope capture
        kStageCount
    };
    inline int64_t g_stage_ns[kStageCount]{};
    inline int64_t g_stage_calls[kStageCount]{};
    struct StageTimer
    {
        int                                   s;
        bool                                  on;
        std::chrono::steady_clock::time_point t0{};
        explicit StageTimer(int st) : s(st), on(t_split_block >= 0) { if (on) t0 = std::chrono::steady_clock::now(); }
        void stop()
        {
            if (!on) return;
            on = false;
            g_stage_ns[s] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            g_stage_calls[s]++;
        }
        ~StageTimer() { stop(); }
    };
    // Shader / layout creation: counted, and timed by the thread it runs on.
    Block create_block();

    // What the frame was, filled by the mod at the pair's start.
    struct State
    {
        bool     vehicle = false, menu = false, cinematic = false, weapon = false, scope_call = false;
        uint32_t weapon_sig = 0;
        float    cam[3]{};       // game camera position (m)
        float    fwd[3]{};       // game camera forward
        float    head_q[4]{};    // headset left-eye rotation x y z w
    };

    // One per stereo pair, from the frame hook: the pair's start (before the game call), around the game call, around the
    // extra call, and its end. id is the pair's GPU timing id.
    void pair_start(uint64_t id, const State& s);
    void game_call_begin();
    void game_call_end();
    void extra_call_begin();
    void extra_call_end();
    void mark_scope_call();   // the pair had the scope pass's third call

    void set_split(bool on);        // perfsplit on|off
    bool split();
    std::string split_summary();    // the last [PerfSplit] line (F11), empty before the first
    std::string draw_summary();     // the last [PerfDraw] line (F11), empty before the first
    void set_count(int n);          // hitchlog N
    void set_csv(int n);            // hitchcsv N
    bool active();
    void set_threshold(float ms);   // hitchms M
    int  count();
    float threshold();
}
