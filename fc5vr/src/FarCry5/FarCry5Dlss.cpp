#include "FarCry5Dlss.h"
#include "FarCry5GpuTime.h"
#include "FarCry5Hitch.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <d3dcompiler.h>
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fc5::dlss
{
    namespace
    {
        std::atomic<bool> g_enabled{ true };
        std::atomic<int>  g_flags{ NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted };
        std::atomic<bool> g_recreate{ false };
        std::atomic<int>  g_debug{ 0 }; // 1: SMAA's input copied through unchanged (checks the colour path)
        std::string       g_status = "off";

        // This frame's motion-vector pass inputs (weak: the game keeps them alive).
        ID3D11Resource* g_depth       = nullptr;
        DXGI_FORMAT     g_depth_view  = DXGI_FORMAT_UNKNOWN;
        ID3D11Resource* g_encoded_mv  = nullptr;

        bool                  g_init_tried = false;
        bool                  g_ready      = false;
        ID3D11Device*         g_device     = nullptr;
        NVSDK_NGX_Parameter*  g_params     = nullptr;
        std::atomic<int>      g_preset{ 11 };   // preset K (author 13 Oct: preset M smeared the near ground)
        NVSDK_NGX_Handle*     g_feature[2]{};
        UINT                  g_feature_w[2]{}, g_feature_h[2]{}, g_feature_ow[2]{};
        // The previous size's feature per eye, kept: menus run DLSS at 1920x1080, the world at the eye size, and creating a
        // feature took 50-80 ms per eye at each switch (the author's 27 Oct 21:35 log: 10 creations, 86-114 ms frames).
        NVSDK_NGX_Handle*     g_spare[2]{};
        UINT                  g_spare_w[2]{}, g_spare_h[2]{}, g_spare_ow[2]{};
        std::atomic<int>      g_spare_reuses{ 0 };

        // OFXR Bridge's DLSS frame generation (and its FSR / hybrid routes) takes the motion vectors and depth of the DLSS
        // evaluation whose output reaches an eye image: it tags the output texture (private data) and follows the tag through
        // full copies. The game's passes after DLSS (SMAA target, HUD) are draws, so no eye image ever carried it and OFXR
        // skipped every frame ("native_stereo_unavailable", 30 Oct). Each eye's tag is kept here and put on that eye's
        // swapchain image just before it is released (tag_xr_image). Layout and GUID from OFXR v663 d3d11_dlss_guides.
        struct OfxrReceipt { uint64_t owner, stream, publication; uint32_t width, height; };
        constexpr GUID kOfxrReceipt{ 0x62935ac7, 0x5aac, 0x47d9, { 0xaa, 0x59, 0x97, 0x61, 0x46, 0x3f, 0x96, 0x28 } };
        OfxrReceipt           g_receipt[2]{};
        std::atomic<bool>     g_ofxr_tags{ true };
        std::atomic<uint32_t> g_tags_set{ 0 }, g_tags_size{ 0 };

        // ofxrdlss11 (OFXR thread, 6 Nov; research\2026-11-06_ofxrdlss). OFXR (v773) takes the DLSS data from two hooks:
        // - NGX's D3D11 evaluation: OFXR copies the vectors and depth across and tags the output (the receipt above);
        // - NGX's D3D12 evaluation: OFXR records its copies into the evaluation's own list and follows that list to whatever
        //   queue runs it. In a D3D11 session it accepts it only on the D3D12 device its bridge made for the game's GPU, which
        //   is the mod's own (D3D12 has one device per GPU in a process); the eye image is then matched by size, no receipt.
        // OFXR asks for DLSS data only on its routes that use it (DLSS frame generation, FSR frame generation, or optical flow
        // with "DLSS motion vectors"); on plain optical flow it never does, so 0 publications there is normal.
        // While OFXR asks and gets nothing, the features are made again (at most 3 times: OFXR only knows features made after
        // its hooks). If it still gets nothing and DLSS runs on D3D12, DLSS goes back to D3D11 for this game run (the 30 Oct
        // route, receipts), unless ofxrdlss11 is off. When OFXR stops asking, DLSS returns to D3D12.
        struct OfxrSettingsV2 { uint32_t size; int32_t enabled, backend, preset, scale, backward, motion_vectors, frame_generation; };
        struct OfxrSnapshotV2
        {
            uint32_t       size, version;
            OfxrSettingsV2 desired;
            uint64_t       revision;
            uint32_t       sessions, pending, bypass, ready, errors;
            int32_t        last_error;
            uint32_t       providers;
        };
        struct OfxrStatsV2
        {
            uint32_t size, version;
            uint64_t published, depth, invalid, copies, copy_failures, resolves, missing, stale, queue_mismatch, matched, temporal, invalid_rej, used, last_used;
            uint32_t status;
            uint64_t fsr_attempts, fsr_used, fsr_failures;
            uint32_t fsr_status;
        };
        HMODULE ofxr_layer()
        {
            static std::atomic<HMODULE> h{ nullptr };
            if (!h.load()) h = GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll");
            return h.load();
        }
        bool ofxr_snapshot(OfxrSnapshotV2& s)
        {
            using Fn = int (*)(OfxrSnapshotV2*);
            const HMODULE l = ofxr_layer();
            const auto    f = l ? reinterpret_cast<Fn>(GetProcAddress(l, "OFXR_GetControlSnapshotV2")) : nullptr;
            s         = {};
            s.size    = sizeof(s);
            s.version = 2;
            s.desired.size = sizeof(s.desired);
            return f && f(&s) != 0;
        }
        bool ofxr_stats(OfxrStatsV2& s)
        {
            using Fn = int (*)(OfxrStatsV2*);
            const HMODULE l = ofxr_layer();
            const auto    f = l ? reinterpret_cast<Fn>(GetProcAddress(l, "OFXR_GetDlssGuideStatisticsV2")) : nullptr;
            s         = {};
            s.size    = sizeof(s);
            s.version = 2;
            return f && f(&s) != 0;
        }
        // OFXR's route as its session reads it (openxr_layer.cpp, dlss_motion_vectors): DLSS data is wanted when vectors are
        // set to DLSS or the frame generation is not OFXR's own optical flow.
        bool ofxr_wants_dlss(const OfxrSnapshotV2& s) { return s.desired.enabled != 0 && (s.desired.motion_vectors == 1 || s.desired.frame_generation != 0); }
        const char* ofxr_route_name(int fg) { return fg == 2 ? "NVIDIA DLSS frame generation" : fg == 1 ? "FSR frame generation" : "OFXR optical flow"; }

        std::atomic<bool>     g_ofxr_fallback{ true };   // ofxrdlss11 (saved, default on)
        std::atomic<bool>     g_ofxr_d11{ false };       // this game run: DLSS moved back to D3D11 for OFXR
        std::atomic<bool>     g_ofxr_wants{ false };     // OFXR's route uses DLSS data (read about once a second)
        std::atomic<int>      g_ofxr_fg{ -1 }, g_ofxr_mv{ -1 };   // OFXR's frame generation and vectors settings (-1: no OFXR)
        std::atomic<uint64_t> g_ofxr_published{ 0 };      // OFXR's DLSS publications so far (its own counter)
        std::atomic<uint64_t> g_evals_ok{ 0 };            // ofxrdlss11: the mod's DLSS evaluations so far (none in menus, the map, loads)

        void keep_ofxr_receipt(int eye, ID3D11Resource* output)
        {
            if (eye < 0 || eye > 1 || output == nullptr) return;
            OfxrReceipt r{};
            UINT        n = sizeof(r);
            if (FAILED(output->GetPrivateData(kOfxrReceipt, &n, &r)) || n != sizeof(r) || r.publication == 0) r = {};
            g_receipt[eye] = r;
        }

        ID3D11ComputeShader* g_decode_cs = nullptr; // encoded R8G8 -> pixel motion R16G16F
        ID3D11ComputeShader* g_cammv_cs = nullptr;  // exact camera motion (kCamMv)
    std::atomic<int>     g_trims{ 7 };   // all three on by default since 17 Oct (author)           // review P7 copy trims (set_trims)
    bool                 g_layer_last = false;   // the last compose used the HUD layer
    // HUD on a virtual screen (report A): cbuffer words 1..15 for the next compose (set_hud_screen).
    float                g_hud_screen[15]{};
    bool                 g_hud_screen_on = false;
    uint64_t             g_hud_screen_frames = 0;
    bool                 g_camonly_valid = false;
        ID3D11Buffer*        g_cammv_cb = nullptr;
        float                g_cam_rows[48]{};       // inv(this eye's camera) rows, then previous camera rows, then previous without the move
        bool                 g_cam_valid = false;
        float                g_body_rows[16]{};      // afwvehmv: clip -> previous clip for points fixed in the vehicle (camera-relative)
        float                g_body_lim = 0.0f;      // afwvehmv: nearer than this (m) the vehicle's motion (0 = off)
        float                g_sl_rows[48]{};        // slingmv: clip -> previous clip, clip -> eye view, centre+radius, hand 0, hand 1, prm4
        bool                 g_sl_on = false;        // slingmv: for the next evaluation (set at the gun mesh's draw)
        uint64_t             g_sl_used = 0, g_sl_nosten = 0;   // slingmv: evaluations with the stencil view, by the shape alone
        bool                 g_sl_fmt_logged = false;           // slingmv: the depth's format logged once when it has no stencil view
        std::atomic<int>     g_cammv_mode{ 1 };      // 0 game vectors, 1 camera + game where something moves, 2 camera only
        std::atomic<float>   g_cammv_sign{ 1.0f };
        std::atomic<float>   g_objmv_scale{ 1.0f };  // afwobjmv: object motion scale (2 in AFW)
    // (dlssmvnbr and dlssmvtrans were removed on 18 Oct: with dlssmvbest they did nothing, and dlssmvtrans never acted at all.)
        std::atomic<bool>    g_objmv_gun{ false };   // afwobjmvgun (research): the old way, the weapon pass scaled too
        std::atomic<int>     g_mv_compare{ 0 };
        std::atomic<int>     g_mv_stat{ 0 };   // cmd dlssmvstat N (test)
    // dlssmvbest (author 17 Oct: grass and trees blur when strafing): the game's vectors are a mix. Points its motion pass
    // reprojects from depth (ground, rocks, buildings) carry the camera's turn only (its CurrentToPrev has no move); what it
    // draws with its own motion (trees, grass, bushes) carries the move too (simulator dump, testruns6-10-17_foliage). Off,
    // mode 3 compared every pixel with the turn only and gave foliage the sideways move twice, and AFW's object scale cancelled
    // the move on foliage in mode 1. On: each pixel is compared with both camera motions; the nearer one is what the game's
    // vector already holds, and only the rest is added as the object's own motion.
    std::atomic<bool>    g_mv_best{ true };
    // dlssmvcheck (author 19 Oct: vehicles, people, animals): an object's own motion from the game is used only if this eye's
    // previous depth confirms it. A parked tractor's harrow carried a steady ~20 px a frame of motion in the game's vectors
    // while standing still (simulator, 19 Oct); its history then landed beside it. Where the previous depth matches the
    // point staying put and not the game's motion, it is taken as still.
    std::atomic<bool>    g_mv_check{ true };
    // dlssmvsway (author 30 Oct: foliage smeared even standing still, the sign and chains beside it sharp): with dlssmvbest the
    // game's object motion counted only above 2 px + 15 % of the camera motion, so leaves and grass swaying a pixel or two a
    // frame were reprojected as still. Their small vectors are precise (the 8-bit code is quartic: ~0.1 px steps at 1 px), so
    // with this the cut-off is these px + 5 %. 0 = off.
    std::atomic<float>   g_mv_sway{ 0.0f };
    std::atomic<int>     g_mv_dump{ 0 };   // cmd dlssmvdump (test): the next N evaluations' game, camera and final vectors and depth to probes
        ID3D11ComputeShader* g_depth_cs  = nullptr; // any depth SRV -> R32F
        ID3D11ComputeShader* g_copy_cs   = nullptr; // colour SRV -> typed UAV (output -> target is a copy)
        ID3D11ComputeShader* g_vis_cs    = nullptr; // motion vectors as an image (debug)

        void d12_idle();   // perfdlss12: waits until the D3D12 queue has finished (before a resource it uses goes)

        struct Tex
        {
            ID3D11Texture2D*           tex = nullptr;
            ID3D11ShaderResourceView*  srv = nullptr;
            ID3D11UnorderedAccessView* uav = nullptr;
            ID3D12Resource*            r12 = nullptr;   // perfdlss12: the same texture opened on the D3D12 device (shared)
            bool                       shared = false;
            UINT                       w = 0, h = 0;
            DXGI_FORMAT                fmt = DXGI_FORMAT_UNKNOWN;
            void release()
            {
                if (r12) { d12_idle(); r12->Release(); }
                if (uav) uav->Release();
                if (srv) srv->Release();
                if (tex) tex->Release();
                uav = nullptr; srv = nullptr; tex = nullptr; r12 = nullptr; shared = false; w = h = 0;
            }
        };
        Tex g_mv, g_depth32, g_color_in, g_out, g_mv_cam, g_mv_camonly;
        Tex  g_prev_depth[2];          // dlssmvcheck: each eye's depth of its previous evaluation
        bool g_prev_depth_ok[2]{};
        Tex g_hi, g_pre_hud, g_bb_low, g_eye;   // upscaling: DLSS output, pre-HUD and final low images, composed eye
        UINT g_out_w = 0, g_out_h = 0;
        std::atomic<UINT> g_render_w{ 0 };
        bool g_have_hi = false;
        ID3D11ComputeShader* g_compose_cs = nullptr;
        ID3D11ComputeShader* g_compose_layer_cs = nullptr;
        ID3D11Buffer*        g_compose_cb       = nullptr;
        ID3D11ComputeShader* g_panel_cs = nullptr;
        ID3D11Buffer*        g_panel_cb = nullptr;
        Tex                  g_panel_base;
        ID3D11Texture2D*     g_panel_ui_tex = nullptr;   // the texture g_panel_ui_srv was made for (not owned)
        ID3D11ShaderResourceView* g_panel_ui_srv = nullptr;
        std::atomic<uint64_t> g_panel_frames{ 0 };
        std::atomic<uint64_t> g_layer_frames{ 0 }, g_layer_low_frames{ 0 }, g_diff_frames{ 0 };
        ID3D11ComputeShader* g_sharpen_cs = nullptr;
        ID3D11Buffer*        g_sharpen_cb = nullptr;
        std::atomic<float>   g_sharpness{ 0.05f };  // Author 13 Oct: 0.05 (0.4 doubled frame-to-frame shimmer in VR: 18% of pixels vs 10%)
        Tex                  g_sharp;
        ID3D11SamplerState*  g_linear     = nullptr;
        // (The sun-shadow bias mask, dlssbias / dlssbiascam / dlssbiasrange, was removed on 16 Oct: only DLSS preset F reads
        // pInBiasCurrentColorMask, and the mod runs the transformer presets, so it cost two passes per eye for nothing.)

        // perfdlss12 (performance thread, 6 Nov): NGX's D3D11 evaluation held the render thread for about 4.7-5.8 ms per eye
        // (28 % of its samples inside NGX_D3D11_EVALUATE_DLSS_EXT, for a 2.6 ms GPU job; with DLSS off the pair took 10-11.6 ms
        // less CPU). With this on, DLSS is evaluated by NGX's D3D12 path on a queue of the mod's own D3D12 device on the same
        // GPU: the inputs and the output are D3D11 textures shared with it, and the two APIs wait for each other on the GPU
        // only (shared fences: D3D11 signals after the input passes, the D3D12 queue waits, evaluates and signals, the D3D11
        // context waits before it reads the output). The render thread only records and submits.
        struct D12
        {
            bool                       tried = false, ready = false;
            std::string                why = "off";
            ID3D11Device5*             dev11 = nullptr;
            ID3D11DeviceContext*       ctx11 = nullptr;   // the immediate context this was made for (not owned)
            ID3D11DeviceContext4*      ctx4 = nullptr;
            ID3D12Device*              dev = nullptr;
            ID3D12CommandQueue*        queue = nullptr;
            static constexpr int       kSlots = 8;        // 4 pairs of evaluations in flight before the CPU waits for one
            ID3D12CommandAllocator*    alloc[kSlots]{};
            ID3D12GraphicsCommandList* list[kSlots]{};
            UINT64                     slot_done[kSlots]{};   // to11 value that tells the slot's list has finished
            int                        slot = 0;
            ID3D12Fence*               to12 = nullptr;    // D3D11 -> D3D12: the inputs are written
            ID3D11Fence*               to12_11 = nullptr;
            UINT64                     to12_v = 0;
            ID3D12Fence*               to11 = nullptr;    // D3D12 -> D3D11: the output is written
            ID3D11Fence*               to11_11 = nullptr;
            UINT64                     to11_v = 0;
            HANDLE                     event = nullptr;
            NVSDK_NGX_Parameter*       params = nullptr;
            uint64_t                   slot_waits = 0, idles = 0, evals = 0;
        };
        D12               g_d12;
        std::atomic<bool> g_d12_done{ false };   // d12_init finished (g_d12.why and ready are final): read by the F11 menu
        std::atomic<bool> g_d3d12_wanted{ true };   // default on since 6 Nov (author 10:21)
        bool              g_features_12 = false;   // the features in g_feature / g_spare were made on D3D12

        // perfdlss12thread (performance thread, 6 Nov): NGX's D3D12 recording took 1.9 ms of render-thread CPU per evaluation
        // (patch 4 build; the same with the OFXR layer off). With this on, a worker thread records each evaluation's list and
        // submits it; the render thread only signals, flushes and queues D3D11's wait for a fence value it picks in advance.
        // Every other NGX call (feature creation and release) waits until the worker has nothing left, so NGX is never called
        // from two threads at once.
        struct D12Job
        {
            int                              slot = 0;
            NVSDK_NGX_Handle*                feature = nullptr;
            NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
            ID3D12Resource*                  res[4]{};   // colour, depth, motion (read), output (UAV)
            UINT64                           v12 = 0, v11 = 0;
        };
        std::atomic<bool>          g_d12_thread{ true };   // default on since 6 Nov (A/B 35.9 -> 39.2-45.5 pairs/s; author 10:21)
        bool                       g_worker_started = false;   // render thread only
        std::mutex                 g_jm;
        std::condition_variable    g_jcv, g_jdone;
        std::deque<D12Job>         g_jobs;
        bool                       g_jbusy = false;
        std::atomic<int64_t>       g_wk_ns{ 0 }, g_wk_n{ 0 };
        std::atomic<uint32_t>      g_wk_err{ 0 };

        // The barriers and NGX's evaluation into an open list (the render thread's inline path and the worker).
        NVSDK_NGX_Result record12(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Handle* feature, NVSDK_NGX_D3D12_DLSS_Eval_Params& ep, ID3D12Resource* const res[4])
        {
            // Shared textures are in the common state on the D3D12 side between the two APIs' work.
            D3D12_RESOURCE_BARRIER b[4]{};
            for (int i = 0; i < 4; ++i) {
                b[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[i].Transition.pResource   = res[i];
                b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b[i].Transition.StateAfter  = i < 3 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            cl->ResourceBarrier(4, b);
            const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature, g_d12.params, &ep);
            for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            cl->ResourceBarrier(4, b);
            cl->Close();
            return r;
        }

        void worker_main()
        {
            for (;;) {
                D12Job j;
                {
                    std::unique_lock lk{ g_jm };
                    g_jcv.wait(lk, [] { return !g_jobs.empty(); });
                    j = g_jobs.front();
                    g_jobs.pop_front();
                    g_jbusy = true;
                }
                const auto t0 = std::chrono::steady_clock::now();
                ID3D12CommandAllocator*    al = g_d12.alloc[j.slot];
                ID3D12GraphicsCommandList* cl = g_d12.list[j.slot];
                NVSDK_NGX_Result           r  = NVSDK_NGX_Result_Fail;
                const bool recorded = SUCCEEDED(al->Reset()) && SUCCEEDED(cl->Reset(al, nullptr));
                if (recorded) r = record12(cl, j.feature, j.ep, j.res);
                // The fence value is signalled whatever happened, or D3D11 would wait for ever; the list runs if it was recorded.
                g_d12.queue->Wait(g_d12.to12, j.v12);
                if (recorded) {
                    ID3D12CommandList* lists[1]{ cl };
                    g_d12.queue->ExecuteCommandLists(1, lists);
                }
                g_d12.queue->Signal(g_d12.to11, j.v11);
                if (NVSDK_NGX_FAILED(r)) g_wk_err = uint32_t(r);
                g_wk_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                g_wk_n++;
                {
                    std::lock_guard lk{ g_jm };
                    g_jbusy = false;
                    if (g_jobs.empty()) g_jdone.notify_all();
                }
            }
        }

        // Waits until the worker has submitted everything it was given (before any other NGX call or a fence signal from here).
        void worker_idle()
        {
            if (!g_worker_started) return;
            std::unique_lock lk{ g_jm };
            g_jdone.wait(lk, [] { return g_jobs.empty() && !g_jbusy; });
        }

        void d12_idle()
        {
            if (!g_d12.ready || !g_d12.queue || !g_d12.to11) return;
            worker_idle();
            const UINT64 v = ++g_d12.to11_v;
            g_d12.queue->Signal(g_d12.to11, v);
            if (g_d12.to11->GetCompletedValue() < v && SUCCEEDED(g_d12.to11->SetEventOnCompletion(v, g_d12.event))) WaitForSingleObject(g_d12.event, 2000);
            g_d12.idles++;
        }

        bool d12_init(ID3D11DeviceContext* ctx)
        {
            if (g_d12.tried) return g_d12.ready && g_d12.ctx11 == ctx;
            g_d12.tried = true;
            auto fail = [](std::string why) {
                g_d12.why = why;
                g_d12_done = true;
                spdlog::error("[DLSS] perfdlss12: {}; DLSS stays on D3D11", why);
                return false;
            };
            if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return fail("not the immediate context");
            if (FAILED(g_device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_d12.dev11)))) return fail("no ID3D11Device5 (D3D11.4 fences)");
            if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_d12.ctx4)))) return fail("no ID3D11DeviceContext4");
            g_d12.ctx11 = ctx;
            IDXGIDevice*  dxdev   = nullptr;
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxdev)))) {
                dxdev->GetAdapter(&adapter);
                dxdev->Release();
            }
            if (!adapter) return fail("no DXGI adapter for the D3D11 device");
            using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
            const HMODULE m      = LoadLibraryW(L"d3d12.dll");
            const auto    create = m ? reinterpret_cast<CreateFn>(GetProcAddress(m, "D3D12CreateDevice")) : nullptr;
            const HRESULT hr     = create ? create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g_d12.dev)) : E_NOINTERFACE;
            adapter->Release();
            if (FAILED(hr) || !g_d12.dev) return fail(fmt::format("D3D12 device on the game's adapter failed 0x{:x}", (uint32_t)hr));
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(g_d12.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_d12.queue)))) return fail("D3D12 queue failed");
            for (int i = 0; i < D12::kSlots; ++i) {
                if (FAILED(g_d12.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&g_d12.alloc[i]))))
                    return fail("D3D12 command allocator failed");
                if (FAILED(g_d12.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d12.alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                                        reinterpret_cast<void**>(&g_d12.list[i]))))
                    return fail("D3D12 command list failed");
                g_d12.list[i]->Close();
            }
            // Both fences are made on D3D12 and opened on D3D11 (shared NT handles).
            auto fence = [](ID3D12Fence*& f12, ID3D11Fence*& f11) {
                if (FAILED(g_d12.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&f12)))) return false;
                HANDLE h = nullptr;
                if (FAILED(g_d12.dev->CreateSharedHandle(f12, nullptr, GENERIC_ALL, nullptr, &h)) || !h) return false;
                const HRESULT r = g_d12.dev11->OpenSharedFence(h, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f11));
                CloseHandle(h);
                return SUCCEEDED(r) && f11 != nullptr;
            };
            if (!fence(g_d12.to12, g_d12.to12_11) || !fence(g_d12.to11, g_d12.to11_11)) return fail("shared fence failed");
            g_d12.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            wchar_t dir[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, dir);
            auto r = NVSDK_NGX_D3D12_Init_with_ProjectID("6b3f5c2e-1d4a-4f7b-9c8e-2a1b3c4d5e6f", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir, g_d12.dev);
            if (NVSDK_NGX_FAILED(r)) r = NVSDK_NGX_D3D12_Init(231313132ull, dir, g_d12.dev);
            if (NVSDK_NGX_FAILED(r)) return fail(fmt::format("NGX D3D12 init failed 0x{:x}", (uint32_t)r));
            r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_d12.params);
            if (NVSDK_NGX_FAILED(r) || !g_d12.params) return fail(fmt::format("NGX D3D12 parameters failed 0x{:x}", (uint32_t)r));
            int available = 0;
            NVSDK_NGX_Parameter_GetI(g_d12.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            if (!available) return fail("DLSS not available on D3D12");
            g_d12.ready = true;
            g_d12.why   = "ready";
            g_d12_done  = true;
            spdlog::info("[DLSS] perfdlss12: NGX D3D12 ready (own device and queue on the game's GPU, shared fences)");
            return true;
        }

        // The next slot's list, open for recording. The CPU waits only if that slot's last list has not finished on the GPU
        // (kSlots evaluations behind).
        int d12_slot()
        {
            const int k = g_d12.slot;
            g_d12.slot  = (k + 1) % D12::kSlots;
            if (g_d12.to11->GetCompletedValue() < g_d12.slot_done[k]) {
                g_d12.slot_waits++;
                if (SUCCEEDED(g_d12.to11->SetEventOnCompletion(g_d12.slot_done[k], g_d12.event))) WaitForSingleObject(g_d12.event, 2000);
            }
            return k;
        }

        int d12_begin()
        {
            const int k = d12_slot();
            if (FAILED(g_d12.alloc[k]->Reset())) return -1;
            if (FAILED(g_d12.list[k]->Reset(g_d12.alloc[k], nullptr))) return -1;
            return k;
        }

        // D3D11 textures DLSS reads or writes, shared with the D3D12 device while perfdlss12 is on.
        bool make_tex(Tex& t, UINT w, UINT h, DXGI_FORMAT fmt, bool shared = false)
        {
            if (t.tex && t.w == w && t.h == h && t.fmt == fmt && t.shared == shared) return true;
            t.release();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (shared) d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &t.tex))) return false;
            if (FAILED(g_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) return false;
            if (FAILED(g_device->CreateUnorderedAccessView(t.tex, nullptr, &t.uav))) return false;
            if (shared) {
                IDXGIResource1* dr = nullptr;
                HANDLE          hs = nullptr;
                if (SUCCEEDED(t.tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dr)))) {
                    dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hs);
                    dr->Release();
                }
                if (!hs) { t.release(); return false; }
                const HRESULT r = g_d12.dev ? g_d12.dev->OpenSharedHandle(hs, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&t.r12)) : E_FAIL;
                CloseHandle(hs);
                if (FAILED(r) || !t.r12) { t.r12 = nullptr; t.release(); return false; }
            }
            t.w = w; t.h = h; t.fmt = fmt; t.shared = shared;
            return true;
        }

        // A feature goes with the API it was made on (a D3D12 one only once the queue no longer uses it).
        void release_feature(NVSDK_NGX_Handle*& f)
        {
            if (!f) return;
            if (g_features_12) {
                d12_idle();
                NVSDK_NGX_D3D12_ReleaseFeature(f);
            } else {
                NVSDK_NGX_D3D11_ReleaseFeature(f);
            }
            f = nullptr;
        }

        // Evaluation timing (perfsplit on): the NGX call's own CPU time and whether the GPU had already finished everything
        // before it when it returned (then the call waited for the GPU).
        int64_t        g_ev_ns[2]{}, g_ev_n[2]{}, g_ev_idle[2]{};   // [0] D3D11 path, [1] D3D12 path (record + submit)
        int64_t        g_ev12_ns[3]{};   // D3D12 path by part: NGX recording (with the barriers), D3D11 signal + flush, queue wait/run/signal + D3D11 wait
        std::atomic<bool> g_d12_flush{ true };   // perfdlss12flush (test): the D3D11 flush after its signal
        int64_t        g_ev_window = 0;
        std::string    g_ev_last;
        std::mutex     g_ev_mtx;   // g_ev_last (the F11 menu reads it)
        ID3D11Query*   g_ev_query = nullptr;

        void eval_window()
        {
            const int64_t now = int64_t(GetTickCount64());
            if (!g_ev_window) g_ev_window = now;
            if (now - g_ev_window < 5000) return;
            g_ev_window = now;
            std::string line;
            for (int k = 0; k < 2; ++k) {
                if (!g_ev_n[k]) continue;
                line += fmt::format("{}{} {:.2f} ms CPU each over {} evaluations, the GPU already caught up at its return in {} of them", line.empty() ? "" : "; ",
                                    k ? "D3D12 (perfdlss12: record, submit, fence waits queued)" : "NGX D3D11 evaluate call", double(g_ev_ns[k]) / double(g_ev_n[k]) / 1e6,
                                    g_ev_n[k], g_ev_idle[k]);
                if (k == 1) {
                    const double n = double(g_ev_n[1]) * 1e6;
                    line += fmt::format(" (NGX recording {:.2f}, D3D11 signal and flush {:.2f}{}, queue submit and waits {:.2f})", double(g_ev12_ns[0]) / n,
                                        double(g_ev12_ns[1]) / n, g_d12_flush.load() ? "" : " [flush off]", double(g_ev12_ns[2]) / n);
                    g_ev12_ns[0] = g_ev12_ns[1] = g_ev12_ns[2] = 0;
                }
                g_ev_ns[k] = g_ev_n[k] = g_ev_idle[k] = 0;
            }
            if (const int64_t n = g_wk_n.exchange(0)) line += fmt::format("; worker thread (perfdlss12thread) recording and submitting {:.2f} ms each over {}", double(g_wk_ns.exchange(0)) / double(n) / 1e6, n);
            if (g_d12.ready) line += fmt::format("; D3D12 waits for a free list {}, queue idles {}", std::exchange(g_d12.slot_waits, 0), std::exchange(g_d12.idles, 0));
            if (line.empty()) return;
            {
                std::lock_guard _{ g_ev_mtx };
                g_ev_last = line;
            }
            spdlog::info("[DlssEval] last 5 s: {}", line);
        }

        const char* kDecode = R"(
Texture2D<float2> g_in : register(t0);
RWTexture2D<float2> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h, iw, ih;
    g_out.GetDimensions(w, h);
    g_in.GetDimensions(iw, ih);
    if (tid.x >= w || tid.y >= h) return;
    uint2 src = min(uint2((float2(tid) + 0.5f) * float2(iw, ih) / float2(w, h)), uint2(iw - 1, ih - 1));
    float2 x = g_in[src].xy - 0.498039216f;
    float2 ax = abs(x);
    float2 s = (x >= 0.0f) ? 1.0f : -1.0f;
    float2 t = 2.0f * ax;
    float2 quartic = s * (t * t * t * t);
    float2 lin = 4.83004713f * x - s * 1.41502368f;
    float2 decoded = (ax < 0.334370166f) ? quartic : lin;
    g_out[tid] = 0.100000001f * decoded * float2(w, h);
})";
        // Exact camera motion for DLSS: the game's own vectors are 8-bit at half resolution (made for its
        // motion blur), too coarse for head turns. Each pixel's position from depth and this eye's camera,
        // seen by this eye's previous camera, gives the motion; where the game's vector differs clearly
        // (something moving by itself: people, animals, swaying plants) the game's is kept.
        const char* kCamMv = R"(
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_game : register(t1);
Texture2D<uint2> g_sten : register(t2);
Texture2D<float> g_pdepth : register(t3);
RWTexture2D<float2> g_out : register(u0);
RWTexture2D<float2> g_camonly : register(u1);
cbuffer C : register(b0) { float4 inv0, inv1, inv2, inv3; float4 pr0, pr1, pr2, pr3; float4 prm; float4 prm2; float4 rr0, rr1, rr2, rr3; float4 bb0, bb1, bb2, bb3; float4 prm3;
                           float4 sm0, sm1, sm2, sm3; float4 si0, si1, si2, si3; float4 slc; float4 slh0; float4 slh1; float4 prm4; };
// prm4.x > 0.5: slingmv. sm: this eye's clip -> its previous image's clip through the slung gun's own motion; si: clip -> this eye's view
// (the weapon pass's projection); slc.xyz, prm4.yzw: the two ends of the slung gun's barrel line in view, slc.w: the radius about it (m);
// slh0, slh1: the hands in view and the radius kept out (0: none). With the stencil view (prm2.y > 0), only weapon-pass pixels; without it,
// any pixel in the capsule (the depth has no stencil view in the game's motion pass).
// prm3.y: dlssmvbest (each pixel's game vector compared with both the camera motion with and without the move).
// prm3.z: dlssmvcheck with this eye's previous depth bound at t3.
// prm3.w > 0: dlssmvsway, the object-motion cut-off in px under dlssmvbest (plus 5 % of the camera motion) instead of prm.z / prm.w.
// prm2.x: object motion scale (AFW); prm2.y > 0: stencil bits of the weapon pass (gun, hands), never scaled: its
// vectors come from the weapon camera's own previous view, which already spans this eye's whole history.
// prm2.z, prm2.w: unused (dlssmvnbr, dlssmvtrans until 18 Oct). rr: this eye's previous camera without its move (rotation only), as the
// game's own vectors see it (its CurrentToPrev has no translation: walking, leaning and driving are not in them).
float objScale(uint2 tid) { return (prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0) ? 1.0f : prm2.x; }
// This eye's camera motion of pixel p (px), from its depth (rot: without the camera's move); false when it leaves the view volume.
bool camAt(int2 p, uint w, uint h, bool rot, out float2 cam)
{
    cam = float2(0.0f, 0.0f);
    p = clamp(p, int2(0, 0), int2(w - 1, h - 1));
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    q = rot ? float4(dot(rr0, q), dot(rr1, q), dot(rr2, q), dot(rr3, q)) : float4(dot(pr0, q), dot(pr1, q), dot(pr2, q), dot(pr3, q));
    if (q.w <= 1e-6f) return false;
    q.xy /= q.w;
    cam = (float2(q.x * 0.5f + 0.5f, 0.5f - q.y * 0.5f) - uv) * float2(w, h) * prm.x;
    return true;
}
// This eye's previous device depth expected at pixel p if it stayed put (full camera motion), and where it was then (px).
bool prevStill(int2 p, uint w, uint h, out float2 at, out float pd)
{
    at = float2(0.0f, 0.0f); pd = 0.0f;
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    q = float4(dot(pr0, q), dot(pr1, q), dot(pr2, q), dot(pr3, q));
    if (q.w <= 1e-6f) return false;
    pd = q.z / q.w;
    at = float2(q.x / q.w * 0.5f + 0.5f, 0.5f - q.y / q.w * 0.5f) * float2(w, h);
    return true;
}
// afwvehmv (prm3.x > 0): a point nearer than prm3.x metres, in a vehicle, is fixed in the vehicle's frame; its motion over this
// eye's history is the vehicle's (bb: clip -> the previous image's clip through that frame). The game's own vector for it is
// one game update long while an AFW eye's history is two: a frame of driving was missing (the doubled cab).
bool bodyAt(int2 p, uint w, uint h, out float2 mv)
{
    mv = float2(0.0f, 0.0f);
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    if (dot(q.xyz, q.xyz) >= prm3.x * prm3.x) return false;
    float4 b = float4(dot(bb0, c), dot(bb1, c), dot(bb2, c), dot(bb3, c));
    if (b.w <= 1e-6f) return false;
    b.xy /= b.w;
    mv = (float2(b.x * 0.5f + 0.5f, 0.5f - b.y * 0.5f) - uv) * float2(w, h) * prm.x;
    return true;
}
// slingmv: a weapon-pass point near the slung gun (not at a hand) moves with the gun.
bool slingAt(int2 p, uint w, uint h, out float2 mv)
{
    mv = float2(0.0f, 0.0f);
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 e = float4(dot(si0, c), dot(si1, c), dot(si2, c), dot(si3, c));
    if (abs(e.w) <= 1e-12f) return false;
    const float3 v = e.xyz / e.w;
    const float3 ab = prm4.yzw - slc.xyz;
    const float  t  = saturate(dot(v - slc.xyz, ab) / max(dot(ab, ab), 1e-6f));
    if (length(v - (slc.xyz + t * ab)) > slc.w) return false;
    if (slh0.w > 0.0f && length(v - slh0.xyz) < slh0.w) return false;
    if (slh1.w > 0.0f && length(v - slh1.xyz) < slh1.w) return false;
    float4 b = float4(dot(sm0, c), dot(sm1, c), dot(sm2, c), dot(sm3, c));
    if (b.w <= 1e-6f) return false;
    b.xy /= b.w;
    mv = (float2(b.x * 0.5f + 0.5f, 0.5f - b.y * 0.5f) - uv) * float2(w, h) * prm.x;
    return true;
}
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 mv = g_game[tid];
    float2 bm;
    if (prm4.x > 0.5f && (prm2.y < 0.5f || (g_sten[tid].y & (uint)prm2.y) != 0)) {
        float2 sm;
        if (slingAt(int2(tid), w, h, sm)) {
            float2 cam0;
            g_out[tid] = sm;
            g_camonly[tid] = camAt(int2(tid), w, h, false, cam0) ? cam0 : float2(0.0f, 0.0f);
            return;
        }
    }
    if (prm3.x > 0.0f && !(prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0) && bodyAt(int2(tid), w, h, bm)) {
        float2 cam0;
        g_out[tid] = bm;
        g_camonly[tid] = camAt(int2(tid), w, h, false, cam0) ? cam0 : float2(0.0f, 0.0f);
        return;
    }
    float2 camv = float2(0.0f, 0.0f);
    float2 cam;
    if (camAt(int2(tid), w, h, false, cam)) {
        camv = cam;
        if (prm3.y > 0.5f && prm.y > 0.5f) {
            // dlssmvbest. The game's vector holds the turn only where its motion pass reprojects depth (ground, rocks,
            // buildings) and the move as well where things are drawn with their own motion (trees, grass, bushes): whichever
            // of the two camera motions it is nearer to is what it already holds, and only the rest is the object's own.
            // AFW: the camera part of the game's vector already spans this eye's history (the mod gives the game this eye's
            // previous camera), but an object's own motion spans one game update while the history is two: with afwobjmv
            // (k = 2) only that rest is doubled. The weapon pass (gun, hands) has its own whole vector.
            const bool gun = prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0;
            float2 rot = cam;
            const bool haveRot = camAt(int2(tid), w, h, true, rot);
            const float2 g = mv;
            const float2 ref = (!gun && haveRot && length(g - rot) < length(g - cam)) ? rot : cam;
            const float2 r = g - ref;
            const float  rel = prm3.w > 0.0f ? 0.05f : prm.y > 1.5f ? 0.0f : prm.w;   // mode 1: plus 15 % of the camera motion; mode 3: 1.5 px flat
            const float  thr = prm3.w > 0.0f ? prm3.w : prm.z;                          // dlssmvsway: swaying leaves pass
            bool moving = length(r) > thr + rel * length(ref);
            // A game vector at its 8-bit code's end (0.1 of the view per frame: a very fast turn) is clipped, not an object's motion.
            if (moving && !gun && any(abs(mv) >= 0.099f * float2(w, h))) moving = false;
            if (moving && !gun) {
                // The game's vector covers a 2x2 block at half resolution: while you move, a near grass blade or leaf gets
                // the vector of a blade or the ground beside it (parallax), tens of px off; standing, its own sway stays under
                // 5 px (simulator, 17 Oct). Only a vector no pixel of the block's neighbourhood explains, with or without the
                // move, is the object's own motion.
                const int2 b = int2(tid / 2) * 2 - 1;
                [loop] for (int i = 0; i < 16; ++i) {
                    const int2 n = b + int2(i & 3, i >> 2);
                    float2 cf, cr;
                    if (camAt(n, w, h, false, cf) && length(g - cf) <= thr + rel * length(cf)) { moving = false; break; }
                    if (haveRot && camAt(n, w, h, true, cr) && length(g - cr) <= thr + rel * length(cr)) { moving = false; break; }
                }
            }
            if (moving && !gun && prm3.z > 0.5f) {
                // dlssmvcheck: where was this point in this eye's previous image? Still: at `at`, with depth pd; moving as the
                // game says: at `at` plus its own motion. Only a clear match for staying put (within 1 % in depth) where the
                // game's place does not match drops the motion; flat surfaces that match both keep it.
                float2 at; float pd;
                if (prevStill(int2(tid), w, h, at, pd) && pd > 1e-6f) {
                    const float2 am = at + r * objScale(tid);
                    if (all(at >= 0.0f) && all(at < float2(w, h)) && all(am >= 0.0f) && all(am < float2(w, h))) {
                        const float eS = abs(g_pdepth[int2(at)] - pd) / pd;
                        const float eM = abs(g_pdepth[int2(am)] - pd) / pd;
                        if (eS < 0.01f && eM > 4.0f * eS + 0.02f) moving = false;
                    }
                }
            }
            mv = moving ? (gun ? mv : cam + r * objScale(tid)) : cam;
            g_out[tid] = mv;
            g_camonly[tid] = camv;
            return;
        }
        // dlssmvbest off: the merge as it ran from 13 Oct. The game's vector is compared with the turn-only camera motion
        // (the old dlssmvtrans test was `trans && !camAt(...)`, and HLSL evaluates both sides, so that switch never changed it).
        float2 ref = cam;
        if (!camAt(int2(tid), w, h, true, ref)) ref = cam;
        // AFW: the game's object motion spans one update, this eye's previous image prm2.x updates.
        const float k = objScale(tid);
        if (prm.y > 1.5f) {
            // Mode 3: the eye's camera motion plus the game's own residual (objects that move by themselves) only.
            float2 r = mv - ref;
            mv = cam + (length(r) > prm.z ? r * k : float2(0.0f, 0.0f));
        } else {
            const bool moving = prm.y > 0.5f && length(mv - ref) > prm.z + prm.w * length(ref);
            mv = moving ? (k != 1.0f ? cam + (mv - cam) * k : mv) : cam;
        }
    }
    g_out[tid] = mv;
    g_camonly[tid] = camv;
})";
        const char* kDepth = R"(
Texture2D<float> g_in : register(t0);
RWTexture2D<float> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    g_out[tid] = g_in[tid];
})";
        const char* kCopy = R"(
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    g_out[tid] = g_in[tid];
})";

        const char* kVis = R"(
Texture2D<float2> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 m = g_in[tid];
    g_out[tid] = float4(saturate(abs(m.x) * 0.1f), saturate(abs(m.y) * 0.1f), saturate(length(m) * 0.02f), 1);
})";

        const char* kCompose = R"(
Texture2D<float4> g_hi : register(t0);
Texture2D<float4> g_pre : register(t1);
Texture2D<float4> g_bb : register(t2);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float4 hi = g_hi[tid];
    float4 pre = g_pre.SampleLevel(g_lin, uv, 0);
    float4 bb = g_bb.SampleLevel(g_lin, uv, 0);
    float3 d = abs(bb.rgb - pre.rgb);
    float a = saturate((max(d.r, max(d.g, d.b)) - 0.012f) * 25.0f);
    g_out[tid] = float4(lerp(hi.rgb, bb.rgb, a), 1.0f);
})";


        // The HUD as its own layer (drawn by the game into a transparent target, premultiplied): over the
        // DLSS image, or over the stretched game image when DLSS gave nothing this frame (g_base_low).
        const char* kComposeLayer = R"(
Texture2D<float4> g_hi : register(t0);
Texture2D<float4> g_hud : register(t1);
Texture2D<float4> g_bb : register(t2);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) {
    uint g_base_low; uint g_screen; float g_dist; float g_yoff;   // HUD screen (hudscreen): on, distance (m), centre height (tangent)
    float4 g_tan;                                                   // this eye image's edges as tangents: left, right, up, down
    float4 g_eye;                                                   // eye position from the head centre in head axes (m), xyz
    float4 g_half;                                                  // the screen's half size as tangents seen from the head centre: x, y
};
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float3 base = g_base_low != 0 ? g_bb.SampleLevel(g_lin, uv, 0).rgb : g_hi[tid].rgb;
    float2 huv = uv;
    if (g_screen != 0) {
        // The HUD as a flat screen in front of the head: this eye's ray through the pixel meets the plane z = -g_dist.
        float2 t = float2(lerp(g_tan.x, g_tan.y, uv.x), lerp(g_tan.z, g_tan.w, uv.y));
        float  k = g_dist + g_eye.z;
        float2 s = (g_eye.xy + t * k) / g_dist;
        huv = float2(0.5f + s.x / (2.0f * g_half.x), 0.5f - (s.y - g_yoff) / (2.0f * g_half.y));
    }
    float4 hud = (all(huv > 0.0f) && all(huv < 1.0f)) ? g_hud.SampleLevel(g_lin, huv, 0) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_out[tid] = float4(hud.rgb + base * (1.0f - saturate(hud.a)), 1.0f);
})";
        // A flat panel (the F11 menu) drawn into this eye's image: each pixel's ray from the eye meets the panel's plane,
        // given in this eye's own axes, so both eyes see one panel at its real place and depth (no extra headset layer).
        const char* kPanel = R"(
Texture2D<float4> g_base : register(t0);
Texture2D<float4> g_ui : register(t1);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) {
    float4 g_tan;   // this eye image's edges as tangents: left, right, up, down
    float4 g_c;     // panel centre in eye axes (m)
    float4 g_ux;    // panel right axis divided by its half width
    float4 g_uy;    // panel up axis divided by its half height
    float4 g_n;     // panel normal
};
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float3 base = g_base[tid].rgb;
    float3 d = float3(lerp(g_tan.x, g_tan.y, uv.x), lerp(g_tan.z, g_tan.w, uv.y), -1.0f);
    float  dn = dot(g_n.xyz, d);
    float4 ui = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (abs(dn) > 1e-6f) {
        float s = dot(g_n.xyz, g_c.xyz) / dn;
        if (s > 0.0f) {
            float3 p   = s * d - g_c.xyz;
            float2 puv = float2(0.5f + 0.5f * dot(p, g_ux.xyz), 0.5f - 0.5f * dot(p, g_uy.xyz));
            if (all(puv > 0.0f) && all(puv < 1.0f)) ui = g_ui.SampleLevel(g_lin, puv, 0);
        }
    }
    g_out[tid] = float4(ui.rgb + base * (1.0f - saturate(ui.a)), 1.0f);
})";
        // Contrast-adaptive sharpening (after AMD CAS): neighbours weighted by local contrast.
        const char* kSharpen = R"(
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) { float g_sharp; float3 pad; };
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    int2 p = int2(tid);
    int2 mx2 = int2(w - 1, h - 1);
    float3 c = g_in[p].rgb;
    float3 n = g_in[clamp(p + int2(0, -1), 0, mx2)].rgb;
    float3 s = g_in[clamp(p + int2(0, 1), 0, mx2)].rgb;
    float3 e = g_in[clamp(p + int2(1, 0), 0, mx2)].rgb;
    float3 o = g_in[clamp(p + int2(-1, 0), 0, mx2)].rgb;
    float3 mn = min(c, min(min(n, s), min(e, o)));
    float3 mx = max(c, max(max(n, s), max(e, o)));
    float3 amp = sqrt(saturate(min(mn, 1.0f - mx) / max(mx, 1e-4f)));
    float3 wgt = -amp * lerp(0.125f, 0.2f, g_sharp);
    float3 r = (c + wgt * (n + s + e + o)) / (1.0f + 4.0f * wgt);
    g_out[tid] = float4(saturate(r), g_in[p].a);
})";

        void sharpen(ID3D11DeviceContext* ctx, Tex& src, Tex& dst)
        {
            if (!g_sharpen_cb) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                g_device->CreateBuffer(&bd, nullptr, &g_sharpen_cb);
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (g_sharpen_cb && SUCCEEDED(ctx->Map(g_sharpen_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float v[4]{ g_sharpness.load(), 0, 0, 0 };
                std::memcpy(m.pData, v, 16);
                ctx->Unmap(g_sharpen_cb, 0);
            }
            ID3D11ComputeShader* prev_cs = nullptr;
            ID3D11Buffer*        prev_cb = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetConstantBuffers(0, 1, &prev_cb);
            ctx->CSSetShader(g_sharpen_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_sharpen_cb);
            ctx->CSSetShaderResources(0, 1, &src.srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &dst.uav, nullptr);
            ctx->Dispatch((dst.w + 7) / 8, (dst.h + 7) / 8, 1);
            ID3D11ShaderResourceView*  ns = nullptr;
            ID3D11UnorderedAccessView* nu = nullptr;
            ctx->CSSetShaderResources(0, 1, &ns);
            ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &prev_cb);
            if (prev_cs) prev_cs->Release();
            if (prev_cb) prev_cb->Release();
        }

        ID3D11ComputeShader* compile(const char* src, const char* name)
        {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            if (!compile_fn) return nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (FAILED(compile_fn(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[DLSS] shader {} failed: {}", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                return nullptr;
            }
            ID3D11ComputeShader* cs = nullptr;
            g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
            code->Release();
            return cs;
        }

        bool init(ID3D11DeviceContext* ctx)
        {
            if (g_init_tried) return g_ready;
            g_init_tried = true;
            ctx->GetDevice(&g_device);
            wchar_t dir[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, dir);
            auto r = NVSDK_NGX_D3D11_Init_with_ProjectID("6b3f5c2e-1d4a-4f7b-9c8e-2a1b3c4d5e6f", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir, g_device);
            if (NVSDK_NGX_FAILED(r)) {
                spdlog::info("[DLSS] NGX init with a project id failed (0x{:x}): trying an application id", (uint32_t)r);
                r = NVSDK_NGX_D3D11_Init(231313132ull, dir, g_device);
            }
            if (NVSDK_NGX_FAILED(r)) {
                g_status = fmt::format("NGX init failed 0x{:x}", (uint32_t)r);
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
            if (NVSDK_NGX_FAILED(r) || !g_params) {
                g_status = fmt::format("NGX parameters failed 0x{:x}", (uint32_t)r);
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            int available = 0;
            NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            if (!available) {
                g_status = "DLSS not available on this GPU/driver";
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            g_decode_cs = compile(kDecode, "fc5_dlss_decode_mv");
            g_cammv_cs  = compile(kCamMv, "fc5_dlss_camera_mv");
            g_depth_cs  = compile(kDepth, "fc5_dlss_depth");
            g_copy_cs   = compile(kCopy, "fc5_dlss_copy");
            g_vis_cs    = compile(kVis, "fc5_dlss_vis");
            g_compose_cs = compile(kCompose, "fc5_dlss_compose");
            g_compose_layer_cs = compile(kComposeLayer, "fc5_dlss_compose_layer");
            g_panel_cs = compile(kPanel, "fc5_dlss_panel");
            g_sharpen_cs = compile(kSharpen, "fc5_dlss_sharpen");
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            g_device->CreateSamplerState(&sd, &g_linear);
            if (!g_decode_cs || !g_depth_cs || !g_copy_cs) {
                g_status = "shader compile failed";
                return false;
            }
            g_ready  = true;
            g_status = "ready";
            spdlog::info("[DLSS] NGX ready (DLSS super sampling available)");
            return true;
        }

        void run_cs(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, UINT w, UINT h)
        {
            ID3D11ComputeShader*       prev_cs  = nullptr;
            ID3D11ShaderResourceView*  prev_srv = nullptr;
            ID3D11UnorderedAccessView* prev_uav = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 1, &prev_srv);
            ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &in);
            ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ID3D11ShaderResourceView*  null_srv = nullptr;
            ID3D11UnorderedAccessView* null_uav = nullptr;
            ctx->CSSetShaderResources(0, 1, &null_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &prev_srv);
            UINT keep = (UINT)-1;
            ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, &keep);
            if (prev_cs) prev_cs->Release();
            if (prev_srv) prev_srv->Release();
            if (prev_uav) prev_uav->Release();
        }

        DXGI_FORMAT typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }
    }

    void set_d3d12(bool on) { g_d3d12_wanted = on; }
    void set_d3d12_flush(bool on) { g_d12_flush = on; }
    void set_d3d12_thread(bool on) { g_d12_thread = on; }
    bool d3d12_thread() { return g_d12_thread.load(); }
    bool d3d12() { return g_d3d12_wanted.load(); }
    std::string d3d12_status()
    {
        const bool done = g_d12_done.load();
        if (!g_d3d12_wanted.load()) return done && g_d12.ready ? "off (the D3D12 device is kept until the game closes)" : "off";
        if (!done) return "on: starts at the next DLSS evaluation";
        if (g_d12.ready && g_ofxr_d11.load()) return fmt::format("on, but DLSS is on D3D11 for OFXR this game run (ofxrdlss11; {} evaluations on D3D12 before)", g_d12.evals);
        return g_d12.ready ? fmt::format("on, {} evaluations on D3D12", g_d12.evals) : "failed: " + g_d12.why + " (DLSS stays on D3D11; tried once per game start)";
    }
    std::string eval_summary()
    {
        std::lock_guard _{ g_ev_mtx };
        return g_ev_last;
    }

    void set_debug(int mode) { g_debug = mode; }
    // dlssmenurelease (author 9 Nov 11:05). A frame generation tool that takes DLSS's
    // data (OFXR's NVIDIA DLSS frame generation) starts only once the game has made and run a DLSS feature, and keeps generating
    // in menus from the last data (menus run no DLSS): black blinking. OFXR drops a feature's data when the feature is released
    // (its NGX ReleaseFeature hook retires the stream). So at the flat screen's start the mod releases its own DLSS features (both
    // eyes and the kept spares), as before the first play; the first evaluation back in play makes them again (about 50-110 ms per
    // eye, once per menu). Nothing tool-specific: only the mod's own DLSS. Same thread as the evaluations (the present thread).
    namespace
    {
        std::atomic<bool> g_menu_release{ false };   // dlssmenurelease (saved, default off: author 9 Nov 11:30)
        uint32_t          g_menu_releases = 0;
    }
    void set_menu_release(bool on) { g_menu_release = on; }
    bool menu_release() { return g_menu_release.load(); }
    void flat_screen(bool flat)
    {
        static bool prev = false;
        if (flat == prev) return;
        prev = flat;
        if (!flat || !g_menu_release.load() || !g_ready) return;
        int n = 0;
        for (auto* f : g_feature) n += f != nullptr;
        for (auto* f : g_spare) n += f != nullptr;
        if (n == 0) return;
        if (g_features_12) worker_idle();   // perfdlss12thread: no feature is touched while the worker runs
        for (auto& f : g_feature) release_feature(f);
        for (auto& f : g_spare) release_feature(f);
        for (int e = 0; e < 2; ++e) g_feature_w[e] = g_feature_h[e] = g_feature_ow[e] = g_spare_w[e] = g_spare_h[e] = g_spare_ow[e] = 0;
        if (g_menu_releases++ < 20 || g_menu_releases % 50 == 0)
            spdlog::info("[DLSS] dlssmenurelease: {} DLSS feature(s) released for the flat screen (made again at the first evaluation in play; {} so far)", n, g_menu_releases);
    }
    void set_flags(int flags) { g_flags = flags; g_recreate = true; }
    void set_preset(int preset) { g_preset = preset; g_recreate = true; }
    int  preset() { return g_preset.load(); }
    void set_enabled(bool on) { g_enabled = on; if (!on) g_status = "off"; if (on && !g_ready) g_init_tried = false; }
    bool enabled() { return g_enabled.load(); }

    void tag_xr_image(int eye, ID3D11Texture2D* image)
    {
        if (!g_ofxr_tags.load() || eye < 0 || eye > 1 || image == nullptr) return;
        // ofxrdlss11: OFXR's route and its own publication counter, about once a second (eye images come at 2 x the frame rate).
        static uint32_t images = 0, starved = 0, retries = 0;
        static uint64_t seen   = 0, evals_seen = 0;
        if (++images >= 120) {
            images = 0;
            OfxrSnapshotV2 snap{};
            if (ofxr_snapshot(snap)) {
                g_ofxr_fg = snap.desired.frame_generation;
                g_ofxr_mv = snap.desired.motion_vectors;
                const bool wants = ofxr_wants_dlss(snap);
                if (wants != g_ofxr_wants.load()) {
                    spdlog::info("[DLSS] ofxrdlss11: OFXR's route is now {}{}: it {} DLSS data", ofxr_route_name(snap.desired.frame_generation),
                                 snap.desired.motion_vectors == 1 ? " with DLSS motion vectors" : "", wants ? "takes" : "takes no");
                    starved = retries = 0;
                }
                g_ofxr_wants = wants;
                if (!wants && g_ofxr_d11.exchange(false)) spdlog::info("[DLSS] ofxrdlss11: OFXR takes no DLSS data any more: DLSS back on D3D12");
            }
            OfxrStatsV2 st{};
            if (ofxr_stats(st)) g_ofxr_published = st.published;
        }
        const bool tagged = g_receipt[eye].publication != 0;
        // Progress = a receipt (D3D11 route) or OFXR's own publication counter moving (D3D12 route, no receipt). Counted on the
        // left eye's images only, so 300 = 300 frames. Nothing is counted while OFXR takes no DLSS data (plain optical flow):
        // before, the features were made again 3 times in every run with OFXR armed (a hitch each).
        // Only frames the mod ran DLSS in count (author 7 Nov 11:41 and 11:51: the map and a load, no DLSS, read as: 3 recreations, then D3D11 for the rest of the run while OFXR was taking D3D12 data at 68 a second).
        if (eye == 0 && g_ofxr_wants.load() && g_ready && g_enabled.load()) {
            const uint64_t pub = g_ofxr_published.load();
            const uint64_t ev  = g_evals_ok.load();
            const bool     ran = ev != evals_seen;
            evals_seen = ev;
            if (tagged || pub != seen) {
                seen    = pub;
                starved = 0;
            } else if (ran && ++starved >= 300) {
                starved = 0;
                if (retries < 3) {
                    // OFXR knows only DLSS features created after it hooked NGX, which it does once the XR session runs; the
                    // mod's features may be older (the menu): new features, at most 3 times.
                    retries++;
                    g_recreate = true;
                    spdlog::info("[DLSS] ofxrguides: OFXR ({}) got no DLSS data from {} in 300 frames: DLSS features recreated ({} of 3)",
                                 ofxr_route_name(g_ofxr_fg.load()), g_features_12 ? "D3D12" : "D3D11", retries);
                } else if (retries == 3 && g_features_12 && g_ofxr_fallback.load() && !g_ofxr_d11.load()) {
                    retries = 0;   // the D3D11 route gets its own 3 tries
                    g_ofxr_d11 = true;
                    spdlog::info("[DLSS] ofxrdlss11: OFXR got no DLSS data from D3D12 after 3 new features: DLSS goes back to D3D11 for this game run "
                                 "(slower render thread; ofxrdlss11 off keeps D3D12)");
                } else if (retries == 3) {
                    retries = 4;
                    spdlog::warn("[DLSS] ofxrguides: OFXR ({}) still gets no DLSS data from {}; giving up for this route", ofxr_route_name(g_ofxr_fg.load()),
                                 g_features_12 ? "D3D12" : "D3D11");
                }
            }
        }
        if (!tagged) return;
        D3D11_TEXTURE2D_DESC d{};
        image->GetDesc(&d);
        // OFXR accepts a tag only on an image of the DLSS output's size (a stretched eye image is not that output).
        if (d.Width != g_receipt[eye].width || d.Height != g_receipt[eye].height) { g_tags_size++; return; }
        if (SUCCEEDED(image->SetPrivateData(kOfxrReceipt, sizeof(OfxrReceipt), &g_receipt[eye]))) g_tags_set++;
    }
    void set_ofxr_fallback(bool on)
    {
        g_ofxr_fallback = on;
        if (!on && g_ofxr_d11.exchange(false)) spdlog::info("[DLSS] ofxrdlss11 off: DLSS back on D3D12");
    }
    bool ofxr_fallback() { return g_ofxr_fallback.load(); }
    std::string ofxr_status()
    {
        if (!ofxr_layer()) return "OFXR Bridge is not loaded";
        const int fg = g_ofxr_fg.load();
        if (fg < 0) return "OFXR loaded, its route not read yet";
        return fmt::format("OFXR route: {}{}; {}; OFXR's DLSS publications {}{}", ofxr_route_name(fg), g_ofxr_mv.load() == 1 ? " + DLSS motion vectors" : "",
                           g_ofxr_wants.load() ? "takes DLSS data" : "takes no DLSS data (nothing to send)", g_ofxr_published.load(),
                           g_ofxr_d11.load() ? "; DLSS moved to D3D11 for OFXR" : "");
    }
    void set_ofxr_tags(bool on) { g_ofxr_tags = on; }
    bool ofxr_tags() { return g_ofxr_tags.load(); }
    std::string ofxr_tag_stats()
    {
        // Diagnosis (30 Oct): whether OFXR hooked NGX's D3D11 entries (an inline hook starts with a jump) and what its guide
        // statistics say (OFXR_DlssGuideStatisticsV2, provider API v2 of v663).
        std::string hooks;
        for (const wchar_t* m : { L"_nvngx.dll", L"nvngx_dlss.dll" }) {
            const HMODULE h = GetModuleHandleW(m);
            // perf thread 6 Nov: the D3D12 entries too (perfdlss12 calls them; NGX's D3D12 recording took 1.9 ms of CPU per evaluation).
            for (const char* fn : { "NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_CreateFeature1", "NVSDK_NGX_D3D11_EvaluateFeature", "NVSDK_NGX_D3D12_CreateFeature",
                                    "NVSDK_NGX_D3D12_EvaluateFeature" }) {
                const auto* p = h ? reinterpret_cast<const uint8_t*>(GetProcAddress(h, fn)) : nullptr;
                hooks += fmt::format(" {}{}{}={}", m[0] == L'_' ? "core:" : "dlss:", fn[14] == '2' ? "12:" : "", fn + 16, p ? fmt::format("{:02x}", *p) : std::string("-"));
            }
        }
        std::string stats = " no OFXR stats";
        OfxrStatsV2 st{};
        if (ofxr_stats(st)) {
            stats = fmt::format(" OFXR guides: published {}, with depth {}, invalid {}, copy failures {}, resolves {}, missing streams {}, queue mismatches {}, matched {}, used {}, status {}",
                                st.published, st.depth, st.invalid, st.copy_failures, st.resolves, st.missing, st.queue_mismatch, st.matched, st.used, st.status);
            OfxrSnapshotV2 snap{};
            if (ofxr_snapshot(snap))
                stats += fmt::format("; route {}{} (enabled {}, backend {}, sessions {}, ready {}){}{}", ofxr_route_name(snap.desired.frame_generation),
                                     snap.desired.motion_vectors == 1 ? " + DLSS motion vectors" : "", snap.desired.enabled, snap.desired.backend, snap.sessions, snap.ready,
                                     ofxr_wants_dlss(snap) ? "" : ": takes no DLSS data", g_ofxr_d11.load() ? "; DLSS on D3D11 for OFXR (ofxrdlss11)" : "");
        }
        return fmt::format("{} eye images tagged, {} of another size; last publications L {} R {};{}{}", g_tags_set.exchange(0), g_tags_size.exchange(0),
                           g_receipt[0].publication, g_receipt[1].publication, hooks, stats);
    }
    std::string status() { return g_status; }

    void set_sharpness(float s) { g_sharpness = std::clamp(s, 0.0f, 1.0f); }
    float sharpness() { return g_sharpness.load(); }
    void set_output_size(UINT w, UINT h) { g_out_w = w; g_out_h = h; }
    bool upscaling() { return g_enabled.load() && g_out_w > 0; }
    float upscale_ratio()
    {
        const UINT rw = g_render_w.load();
        return (upscaling() && rw > 0 && g_out_w > rw) ? float(g_out_w) / float(rw) : 1.0f;
    }

    void after_smaa(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target)
    {
        if (!g_have_hi || !target) return;
        if ((g_trims.load() & 2) && g_layer_last) return;   // trim 2: the HUD is its own layer, the pre-HUD copy is not read
        ID3D11Resource* r = nullptr;
        target->GetResource(&r);
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        target->GetDesc(&vd);
        if (make_tex(g_pre_hud, d.Width, d.Height, typed(vd.Format))) ctx->CopyResource(g_pre_hud.tex, r);
        r->Release();
    }

    ID3D11Texture2D* compose(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11ShaderResourceView* hud_layer)
    {
        if (!backbuffer) return nullptr;
        // With the HUD as a layer, a frame without a DLSS image still gets the HUD (over the stretched
        // game image); the difference method needs both the DLSS image and the pre-HUD copy.
        const bool layer = hud_layer != nullptr && g_compose_layer_cs != nullptr && g_hi.tex != nullptr;
        g_layer_last     = layer;
        if (!layer && (!g_have_hi || !g_pre_hud.tex || !g_compose_cs)) return nullptr;
        D3D11_TEXTURE2D_DESC d{};
        backbuffer->GetDesc(&d);
        if (!make_tex(g_bb_low, d.Width, d.Height, typed(d.Format))) return nullptr;
        // The stretched game image is only read when there is no DLSS image (trim 4 skips the copy otherwise).
        if (!((g_trims.load() & 4) && layer && g_have_hi)) ctx->CopyResource(g_bb_low.tex, backbuffer);
        if (!make_tex(g_eye, g_hi.w, g_hi.h, typed(d.Format))) return nullptr;
        ID3D11ComputeShader*       prev_cs = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ID3D11ShaderResourceView*  srv[3]{ g_hi.srv, layer ? hud_layer : g_pre_hud.srv, g_bb_low.srv };
        if (layer) {
            const UINT low = g_have_hi ? 0u : 1u;
            if (!g_compose_cb) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) { dev->CreateBuffer(&bd, nullptr, &g_compose_cb); dev->Release(); }
            }
            uint32_t cbv[16]{};
            cbv[0] = low;
            if (g_hud_screen_on) {
                std::memcpy(&cbv[1], g_hud_screen, 15 * sizeof(float));   // g_screen flag + parameters (set_hud_screen)
                g_hud_screen_frames++;
            }
            g_hud_screen_on = false;   // given for one compose only
            if (g_compose_cb) ctx->UpdateSubresource(g_compose_cb, 0, nullptr, cbv, 0, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_compose_cb);
            ctx->CSSetShader(g_compose_layer_cs, nullptr, 0);
            (low ? g_layer_low_frames : g_layer_frames)++;
        } else {
            ctx->CSSetShader(g_compose_cs, nullptr, 0);
            g_diff_frames++;
        }
        ctx->CSSetShaderResources(0, 3, srv);
        ctx->CSSetSamplers(0, 1, &g_linear);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_eye.uav, nullptr);
        ctx->Dispatch((g_eye.w + 7) / 8, (g_eye.h + 7) / 8, 1);
        ID3D11ShaderResourceView*  null_srv[3]{};
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ctx->CSSetShaderResources(0, 3, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ctx->CSSetShader(prev_cs, nullptr, 0);
        if (prev_cs) prev_cs->Release();
        if (layer) { ID3D11Buffer* none = nullptr; ctx->CSSetConstantBuffers(0, 1, &none); }
        return g_eye.tex;
    }

    void set_camera(const float* inv_rows, const float* prev_rows, const float* prev_rot_rows)
    {
        std::memcpy(g_cam_rows, inv_rows, 16 * sizeof(float));
        std::memcpy(g_cam_rows + 16, prev_rows, 16 * sizeof(float));
        std::memcpy(g_cam_rows + 32, prev_rot_rows, 16 * sizeof(float));
        g_cam_valid = true;
        g_body_lim  = 0.0f;   // set_body_motion follows when it applies to this camera
    }
    void set_body_motion(const float* rows, float metres)
    {
        std::memcpy(g_body_rows, rows, 16 * sizeof(float));
        g_body_lim = metres;
    }
    void set_sling_motion(const float* rows, const float* inv_proj_rows, const float a[3], const float b[3], float radius, const float hands[6], const float hand_r[2])
    {
        std::memcpy(g_sl_rows, rows, 16 * sizeof(float));
        std::memcpy(g_sl_rows + 16, inv_proj_rows, 16 * sizeof(float));
        g_sl_rows[32] = a[0]; g_sl_rows[33] = a[1]; g_sl_rows[34] = a[2]; g_sl_rows[35] = radius;
        g_sl_rows[36] = hands[0]; g_sl_rows[37] = hands[1]; g_sl_rows[38] = hands[2]; g_sl_rows[39] = hand_r[0];
        g_sl_rows[40] = hands[3]; g_sl_rows[41] = hands[4]; g_sl_rows[42] = hands[5]; g_sl_rows[43] = hand_r[1];
        g_sl_rows[44] = 1.0f; g_sl_rows[45] = b[0]; g_sl_rows[46] = b[1]; g_sl_rows[47] = b[2];   // prm4.x: on; prm4.yzw: the barrel line's far end
        g_sl_on = true;
    }
    void clear_sling_motion() { g_sl_on = false; }
    void sling_motion_stats(uint64_t& with_stencil, uint64_t& by_shape)
    {
        with_stencil = g_sl_used;
        by_shape     = g_sl_nosten;
        g_sl_used = g_sl_nosten = 0;
    }
    void set_hud_screen(const float tan[4], const float eye[3], float dist, float half_x, float half_y, float yoff)
    {
        uint32_t one = 1;
        std::memcpy(&g_hud_screen[0], &one, 4);   // g_screen
        g_hud_screen[1] = dist;
        g_hud_screen[2] = yoff;
        for (int k = 0; k < 4; ++k) g_hud_screen[3 + k] = tan[k];
        for (int k = 0; k < 3; ++k) g_hud_screen[7 + k] = eye[k];
        g_hud_screen[10] = 0.0f;
        g_hud_screen[11] = half_x;
        g_hud_screen[12] = half_y;
        g_hud_screen[13] = g_hud_screen[14] = 0.0f;
        g_hud_screen_on = true;
    }
    uint64_t hud_screen_frames() { return g_hud_screen_frames; }
    bool draw_panel(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, ID3D11Texture2D* ui, const float params[20])
    {
        if (!ctx || !eye || !ui || !g_panel_cs || !g_eye.tex || eye != g_eye.tex || !g_eye.uav) return false;
        if (ui != g_panel_ui_tex) {
            if (g_panel_ui_srv) g_panel_ui_srv->Release();
            g_panel_ui_srv = nullptr;
            g_panel_ui_tex = ui;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) { dev->CreateShaderResourceView(ui, nullptr, &g_panel_ui_srv); dev->Release(); }
        }
        if (!g_panel_ui_srv) return false;
        if (!g_panel_cb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) { dev->CreateBuffer(&bd, nullptr, &g_panel_cb); dev->Release(); }
            if (!g_panel_cb) return false;
        }
        if (!make_tex(g_panel_base, g_eye.w, g_eye.h, g_eye.fmt)) return false;
        ctx->CopyResource(g_panel_base.tex, g_eye.tex);
        ctx->UpdateSubresource(g_panel_cb, 0, nullptr, params, 0, 0);
        ID3D11ComputeShader* prev_cs = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ID3D11ShaderResourceView* srv[2]{ g_panel_base.srv, g_panel_ui_srv };
        ctx->CSSetShader(g_panel_cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_panel_cb);
        ctx->CSSetShaderResources(0, 2, srv);
        ctx->CSSetSamplers(0, 1, &g_linear);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_eye.uav, nullptr);
        ctx->Dispatch((g_eye.w + 7) / 8, (g_eye.h + 7) / 8, 1);
        ID3D11ShaderResourceView*  null_srv[2]{};
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ID3D11Buffer*              null_cb  = nullptr;
        ctx->CSSetShaderResources(0, 2, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &null_cb);
        ctx->CSSetShader(prev_cs, nullptr, 0);
        if (prev_cs) prev_cs->Release();
        g_panel_frames++;
        return true;
    }
    uint64_t panel_frames() { return g_panel_frames.load(); }
    void set_trims(int bits) { g_trims = bits & 7; }
    int trims() { return g_trims.load(); }
    void set_camera_motion(int mode, float sign) { g_cammv_mode = std::clamp(mode, 0, 3); g_cammv_sign = sign < 0.0f ? -1.0f : 1.0f; }
    int camera_motion() { return g_cammv_mode.load(); }
    void set_object_motion_scale(float s) { g_objmv_scale = s; }
    void set_object_motion_gun(bool on) { g_objmv_gun = on; }
    void compare_motion(int n) { g_mv_compare = n; }
    void motion_stat(int n) { g_mv_stat = n; }
    void set_motion_best(bool on) { g_mv_best = on; }
    void set_motion_check(bool on) { g_mv_check = on; }
    bool motion_check() { return g_mv_check.load(); }
    void set_motion_sway(float px) { g_mv_sway = std::clamp(px, 0.0f, 2.0f); }
    float motion_sway() { return g_mv_sway.load(); }
    bool motion_best() { return g_mv_best.load(); }
    std::atomic<int> g_mv_dump_total{ 0 };
    void motion_dump(int n) { g_mv_dump_total = n; g_mv_dump = n; }

    std::string compose_stats()
    {
        return fmt::format("HUD layer over DLSS {}, over the stretched image {}, difference {}", g_layer_frames.exchange(0), g_layer_low_frames.exchange(0), g_diff_frames.exchange(0));
    }

    ID3D11ShaderResourceView* output_srv() { return g_have_hi ? g_hi.srv : nullptr; }

    void new_frame()
    {
        g_have_hi = false;
        g_depth      = nullptr;
        g_encoded_mv = nullptr;
    }

    void note_motion_pass(ID3D11Resource* depth, DXGI_FORMAT depth_view_format, ID3D11Resource* encoded_mv)
    {
        g_depth      = depth;
        g_depth_view = depth_view_format;
        g_encoded_mv = encoded_mv;
    }

    std::atomic<int> g_dlaa_phases{ 8 };
    void set_dlaa_phases(int n) { g_dlaa_phases = std::clamp(n, 4, 32); }
    int  dlaa_phases() { return g_dlaa_phases.load(); }

    void jitter_for(int frame, float& x_px, float& y_px)
    {
        // Halton (2, 3), 8 phases per eye: the same eye sees phases 0..7 on its own frames.
        // More phases when upscaling (each output pixel needs several input samples).
        const int phases = g_out_w > 0 ? 16 : std::clamp(g_dlaa_phases.load(), 4, 32);   // dlaaphases (default 8)
        const int n = ((frame >> 1) % phases) + 1;
        auto halton = [](int i, int b) {
            float f = 1.0f, r = 0.0f;
            while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
            return r;
        };
        x_px = halton(n, 2) - 0.5f;
        y_px = halton(n, 3) - 0.5f;
    }

    bool resolve(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11RenderTargetView* target, int eye, float jx, float jy, bool reset)
    {
        if (!g_enabled.load() || !color || !target) return false;
        if (!init(ctx)) return false;
        if (!g_depth || !g_encoded_mv) { g_status = "no motion-vector pass this frame"; return false; }
        const bool use12  = g_d3d12_wanted.load() && !g_ofxr_d11.load() && d12_init(ctx);   // perfdlss12 (ofxrdlss11: D3D11 while OFXR needs it)
        const bool timing = fc5::hitch::split();                      // [DlssEval] with perfsplit on

        ID3D11Resource* color_res = nullptr;
        color->GetResource(&color_res);
        ID3D11Resource* target_res = nullptr;
        target->GetResource(&target_res);
        D3D11_TEXTURE2D_DESC cd{}, td{};
        static_cast<ID3D11Texture2D*>(color_res)->GetDesc(&cd);
        static_cast<ID3D11Texture2D*>(target_res)->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC cvd{};
        color->GetDesc(&cvd);
        D3D11_RENDER_TARGET_VIEW_DESC tvd{};
        target->GetDesc(&tvd);
        bool ok = false;
        fc5::gputime::begin(ctx, fc5::gputime::kDlssPrep);
        bool in_eval = false;
        ID3D11Texture2D* mv_in = nullptr;   // the motion DLSS reads (g_mv, or the camera motion with trim 1)
        int                        slot    = -1;        // perfdlss12: this evaluation's D3D12 list
        ID3D12GraphicsCommandList* cl      = nullptr;
        bool                       cl_open = false;
        do {
            if (cd.Width != td.Width || cd.Height != td.Height) { g_status = "colour and target sizes differ"; break; }
            const UINT w = cd.Width, h = cd.Height;
            const DXGI_FORMAT work = typed(cvd.Format);
            // Inputs in formats DLSS reads: colour copied to a typed texture, depth to R32F, motion decoded.
            if (!make_tex(g_color_in, w, h, work, use12) || !make_tex(g_out, w, h, work, use12) || !make_tex(g_depth32, w, h, DXGI_FORMAT_R32_FLOAT, use12) ||
                !make_tex(g_mv, w, h, DXGI_FORMAT_R16G16_FLOAT, use12)) {
                g_status = fmt::format("texture allocation failed (format {})", (int)work);
                break;
            }
            mv_in = g_mv.tex;
            run_cs(ctx, g_copy_cs, color, g_color_in.uav, w, h);
            if (g_debug.load() == 1) {
                ctx->CopyResource(target_res, g_color_in.tex);
                g_status = "debug: colour pass-through";
                ok = true;
                break;
            }
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC dv{};
                dv.Format = g_depth_view != DXGI_FORMAT_UNKNOWN ? g_depth_view : DXGI_FORMAT_R32_FLOAT;
                dv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                dv.Texture2D.MipLevels = 1;
                ID3D11ShaderResourceView* dsrv = nullptr;
                if (FAILED(g_device->CreateShaderResourceView(g_depth, &dv, &dsrv))) { g_status = fmt::format("depth view failed (format {})", (int)dv.Format); break; }
                run_cs(ctx, g_depth_cs, dsrv, g_depth32.uav, w, h);
                dsrv->Release();
            }
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC mv{};
                mv.Format = DXGI_FORMAT_R8G8_UNORM;
                mv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                mv.Texture2D.MipLevels = 1;
                ID3D11ShaderResourceView* msrv = nullptr;
                if (FAILED(g_device->CreateShaderResourceView(g_encoded_mv, &mv, &msrv))) { g_status = "motion view failed"; break; }
                run_cs(ctx, g_decode_cs, msrv, g_mv.uav, w, h);
                msrv->Release();
            }
            if (g_cammv_mode.load() != 0 && g_cam_valid && g_cammv_cs && make_tex(g_mv_cam, w, h, DXGI_FORMAT_R16G16_FLOAT, use12)) {
                if (!g_cammv_cb) {
                    D3D11_BUFFER_DESC bd{};
                    bd.ByteWidth = 31 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;   // slingmv: 19 -> 31 float4
                    g_device->CreateBuffer(&bd, nullptr, &g_cammv_cb);
                }
                if (g_cammv_cb) {
                    float cb[124]{};
                    std::memcpy(cb, g_cam_rows, 32 * sizeof(float));
                    std::memcpy(cb + 40, g_cam_rows + 32, 16 * sizeof(float));   // rr0-rr3 after prm, prm2
                    std::memcpy(cb + 56, g_body_rows, 16 * sizeof(float));       // afwvehmv: bb0-bb3
                    cb[72] = g_body_lim;                                          // prm3.x
                    cb[73] = g_mv_best.load() ? 1.0f : 0.0f;                     // prm3.y: dlssmvbest
                    if (g_sl_on) std::memcpy(cb + 76, g_sl_rows, 48 * sizeof(float));   // slingmv: sm, si, slc, slh0, slh1, prm4 (prm4.x 0: off)
                    cb[32] = g_cammv_sign.load();
                    cb[33] = g_cammv_mode.load() == 1 ? 1.0f : g_cammv_mode.load() == 3 ? 2.0f : 0.0f;
                    cb[34] = g_cammv_mode.load() == 3 ? 1.5f : 2.0f;    // px: differences below this are the game vectors' coarseness
                    cb[35] = 0.15f;   // plus 15 % of the camera motion
                    cb[36] = g_objmv_scale.load();
                    // AFW object motion scale: the weapon pass (stencil 0x40) keeps its own vectors (they already span the eye's history).
                    ID3D11ShaderResourceView* ssrv = nullptr;
                    if ((cb[36] != 1.0f || g_mv_best.load() || g_body_lim > 0.0f || g_sl_on) && g_depth && !g_objmv_gun.load()) {
                        D3D11_TEXTURE2D_DESC dd{};
                        static_cast<ID3D11Texture2D*>(g_depth)->GetDesc(&dd);
                        DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN;
                        if (dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) sf = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
                        if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS || dd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) sf = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                        if (sf != DXGI_FORMAT_UNKNOWN && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                            sv.Format = sf; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
                            if (FAILED(g_device->CreateShaderResourceView(g_depth, &sv, &ssrv))) ssrv = nullptr;
                        }
                    }
                    cb[37] = ssrv ? 64.0f : 0.0f;
                    if (g_sl_on) {   // slingmv: given for this evaluation only; without the stencil view the pixels are found by their place alone
                        if (ssrv) g_sl_used++; else g_sl_nosten++;
                        if (!ssrv && !g_sl_fmt_logged && g_depth) {
                            g_sl_fmt_logged = true;
                            D3D11_TEXTURE2D_DESC dd{};
                            static_cast<ID3D11Texture2D*>(g_depth)->GetDesc(&dd);
                            spdlog::info("[SlingMV] DLSS's depth has no stencil view (format {}, bind flags 0x{:x}, {}x{}, objmvgun {}): the slung gun is found by its "
                                         "place (the capsule along its barrel)", (int)dd.Format, (unsigned)dd.BindFlags, dd.Width, dd.Height, g_objmv_gun.load() ? "on" : "off");
                        }
                        g_sl_on = false;
                    }
                    // dlssmvcheck: this eye's depth of its previous evaluation (not after a history reset or a size change).
                    const int  pe   = eye & 1;
                    const bool pchk = g_mv_check.load() && g_mv_best.load() && !reset && g_prev_depth_ok[pe] && g_prev_depth[pe].w == w && g_prev_depth[pe].h == h;
                    cb[74] = pchk ? 1.0f : 0.0f;                                  // prm3.z
                    cb[75] = std::max(0.0f, g_mv_sway.load());                    // prm3.w: dlssmvsway
                    ctx->UpdateSubresource(g_cammv_cb, 0, nullptr, cb, 0, 0);
                    ID3D11ComputeShader*       prev_cs = nullptr;
                    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
                    ID3D11ShaderResourceView*  srv[4]{ g_depth32.srv, g_mv.srv, ssrv, pchk ? g_prev_depth[pe].srv : nullptr };
                    ctx->CSSetShader(g_cammv_cs, nullptr, 0);
                    ctx->CSSetShaderResources(0, 4, srv);
                    if (ssrv) ssrv->Release();   // the binding keeps it alive until unbound below
                    ctx->CSSetConstantBuffers(0, 1, &g_cammv_cb);
                    ID3D11UnorderedAccessView* uavs[2]{ g_mv_cam.uav, make_tex(g_mv_camonly, w, h, DXGI_FORMAT_R16G16_FLOAT) ? g_mv_camonly.uav : nullptr };
                    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                    g_camonly_valid = uavs[1] != nullptr;
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ID3D11ShaderResourceView*  null_srv[4]{};
                    ID3D11UnorderedAccessView* null_uav[2]{};
                    ID3D11Buffer*              null_cb  = nullptr;
                    ctx->CSSetShaderResources(0, 4, null_srv);
                    ctx->CSSetUnorderedAccessViews(0, 2, null_uav, nullptr);
                    // This evaluation's depth is the next one's previous depth for this eye.
                    if (make_tex(g_prev_depth[pe], w, h, DXGI_FORMAT_R32_FLOAT)) {
                        ctx->CopyResource(g_prev_depth[pe].tex, g_depth32.tex);
                        g_prev_depth_ok[pe] = true;
                    }
                    ctx->CSSetConstantBuffers(0, 1, &null_cb);
                    ctx->CSSetShader(prev_cs, nullptr, 0);
                    if (prev_cs) prev_cs->Release();
                    if (g_mv_compare.load() > 0) {
                        // Test: the game's vector next to the camera one at a few points (pixels).
                        g_mv_compare--;
                        D3D11_TEXTURE2D_DESC sd{};
                        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R16G16_FLOAT; sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ID3D11Texture2D* sg = nullptr;
                        ID3D11Texture2D* sc = nullptr;
                        g_device->CreateTexture2D(&sd, nullptr, &sg);
                        g_device->CreateTexture2D(&sd, nullptr, &sc);
                        if (sg && sc) {
                            ctx->CopyResource(sg, g_mv.tex);
                            ctx->CopyResource(sc, g_mv_cam.tex);
                            D3D11_MAPPED_SUBRESOURCE mg{}, mc{};
                            if (SUCCEEDED(ctx->Map(sg, 0, D3D11_MAP_READ, 0, &mg)) && SUCCEEDED(ctx->Map(sc, 0, D3D11_MAP_READ, 0, &mc))) {
                                std::string line;
                                for (const float fy : { 0.35f, 0.45f, 0.6f }) {
                                    for (const float fx : { 0.2f, 0.5f, 0.8f }) {
                                        const UINT x = UINT(fx * w), y = UINT(fy * h);
                                        auto rd  = [&](const D3D11_MAPPED_SUBRESOURCE& m) {
                                            const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch) + x * 2;
                                            return std::pair{ glm::unpackHalf1x16(p[0]), glm::unpackHalf1x16(p[1]) };
                                        };
                                        const auto g = rd(mg), c = rd(mc);
                                        line += fmt::format(" ({:.2f},{:.2f}) game ({:.2f},{:.2f}) camera ({:.2f},{:.2f}) |", fx, fy, g.first, g.second, c.first, c.second);
                                    }
                                }
                                spdlog::info("[DLSS] motion vectors (px):{}", line);
                                ctx->Unmap(sg, 0);
                                ctx->Unmap(sc, 0);
                            }
                        }
                        if (sg) sg->Release();
                        if (sc) sc->Release();
                    }
                    if (g_mv_dump.load() > 0 && g_camonly_valid) {
                        // Test (cmd dlssmvdump N): the game's decoded vectors, the camera motion with the move, the vectors DLSS gets
                        // (R16G16F, px) and the depth (R32F) of this eye, raw rows, plus the camera rows: probes\mvdump_<eye>_*.raw
                        // (tools\dlss\mvdump.py reads them).
                        // More than one pair (dlssmvdump N > 2): each evaluation's files get its number (mvdump_q<k>_...), so
                        // consecutive frames can be compared (an object's real move against its game vector).
                        const int   left = --g_mv_dump;
                        const int   total = g_mv_dump_total.load();
                        const std::string pre = total > 2 ? fmt::format("mvdump_q{:02}_", total - 1 - left) : std::string("mvdump_");
                        const char* names[4]{ "game", "cam", "final", "depth" };
                        Tex*        texs[4]{ &g_mv, &g_mv_camonly, &g_mv_cam, &g_depth32 };
                        for (int i = 0; i < 4; ++i) {
                            D3D11_TEXTURE2D_DESC sd{};
                            texs[i]->tex->GetDesc(&sd);
                            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.BindFlags = 0; sd.MiscFlags = 0;
                            ID3D11Texture2D* st = nullptr;
                            if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st))) continue;
                            ctx->CopyResource(st, texs[i]->tex);
                            D3D11_MAPPED_SUBRESOURCE m{};
                            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                                if (std::FILE* f = std::fopen(fmt::format("fc5vr_probes/{}{}_{}_{}x{}.raw", pre, eye, names[i], sd.Width, sd.Height).c_str(), "wb")) {
                                    for (UINT y = 0; y < sd.Height; ++y) std::fwrite(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, 1, size_t(sd.Width) * 4, f);
                                    std::fclose(f);
                                }
                                ctx->Unmap(st, 0);
                            }
                            st->Release();
                        }
                        if (std::FILE* f = std::fopen(fmt::format("fc5vr_probes/{}{}_rows.txt", pre, eye).c_str(), "w")) {
                            for (int i = 0; i < 48; ++i) std::fprintf(f, "%.9g%c", g_cam_rows[i], (i % 4 == 3) ? '\n' : ' ');
                            std::fprintf(f, "mode %d best %d check %d sign %g\n", g_cammv_mode.load(), g_mv_best.load() ? 1 : 0, g_mv_check.load() ? 1 : 0, g_cammv_sign.load());
                            std::fclose(f);
                        }
                        spdlog::info("[DLSS] mvdump eye {}: game, camera and final vectors and depth written to probes", eye);
                    }
                    if (g_mv_stat.load() > 0 && g_camonly_valid) {
                        // Test (cmd dlssmvstat N, the next N evaluations): on how much of the eye the final vector is not the
                        // head motion (the game's vector was taken), and the mean distance between the two there.
                        g_mv_stat--;
                        D3D11_TEXTURE2D_DESC sd{};
                        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R16G16_FLOAT; sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ID3D11Texture2D* sf = nullptr;
                        ID3D11Texture2D* sc = nullptr;
                        g_device->CreateTexture2D(&sd, nullptr, &sf);
                        g_device->CreateTexture2D(&sd, nullptr, &sc);
                        if (sf && sc) {
                            ctx->CopyResource(sf, g_mv_cam.tex);
                            ctx->CopyResource(sc, g_mv_camonly.tex);
                            D3D11_MAPPED_SUBRESOURCE mf{}, mc{};
                            if (SUCCEEDED(ctx->Map(sf, 0, D3D11_MAP_READ, 0, &mf)) && SUCCEEDED(ctx->Map(sc, 0, D3D11_MAP_READ, 0, &mc))) {
                                uint64_t n = 0, used = 0;
                                double   dist = 0.0, cam = 0.0;
                                // Where: a map at 1/16 size, white = the game's vector taken (probes/mvstat_<eye>.pgm, the last one kept).
                                const UINT           mw = w / 16, mh = h / 16;
                                std::vector<uint8_t> map(size_t(mw) * mh, 0);
                                for (UINT y = 0; y < h; y += 2) {
                                    const uint16_t* pf = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mf.pData) + y * mf.RowPitch);
                                    const uint16_t* pc = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mc.pData) + y * mc.RowPitch);
                                    for (UINT x = 0; x < w; x += 2) {
                                        const float fx = glm::unpackHalf1x16(pf[x * 2]), fy = glm::unpackHalf1x16(pf[x * 2 + 1]);
                                        const float cx = glm::unpackHalf1x16(pc[x * 2]), cy = glm::unpackHalf1x16(pc[x * 2 + 1]);
                                        const float d  = std::hypot(fx - cx, fy - cy);
                                        n++;
                                        cam += std::hypot(cx, cy);
                                        if (d > 0.01f) {
                                            used++;
                                            dist += d;
                                            if (x / 16 < mw && y / 16 < mh) { auto& m = map[size_t(y / 16) * mw + x / 16]; m = uint8_t(std::min(255, m + 4)); }
                                        }
                                    }
                                }
                                if (std::FILE* f = std::fopen(fmt::format("fc5vr_probes/mvstat_{}.pgm", eye).c_str(), "wb")) {
                                    std::fprintf(f, "P5\n%u %u\n255\n", mw, mh);
                                    std::fwrite(map.data(), 1, map.size(), f);
                                    std::fclose(f);
                                }
                                spdlog::info("[DLSS] mvstat eye {}: game vector on {:.2f}% of the image (mean {:.1f} px from the head motion there), head motion mean {:.1f} px, dlssmvbest {}",
                                             eye, n ? 100.0 * used / n : 0.0, used ? dist / used : 0.0, n ? cam / n : 0.0, g_mv_best.load() ? "on" : "off");
                                ctx->Unmap(sf, 0);
                                ctx->Unmap(sc, 0);
                            }
                        }
                        if (sf) sf->Release();
                        if (sc) sc->Release();
                    }
                    if (g_trims.load() & 1) mv_in = g_mv_cam.tex;   // trim 1: DLSS reads the camera motion as it is
                    else ctx->CopyResource(g_mv.tex, g_mv_cam.tex);
                }
            }
            if (g_debug.load() == 2) {
                // Zero motion: tells whether the decoded vectors are what blurs the result.
                const float zero[4]{};
                ctx->ClearUnorderedAccessViewFloat(g_mv.uav, zero);
            }
            if (g_debug.load() == 3) {
                // Show |motion| in pixels as grey (x10) instead of the image.
                run_cs(ctx, g_vis_cs, g_mv.srv, g_out.uav, w, h);
                ctx->CopyResource(target_res, g_out.tex);
                g_status = "debug: motion vectors";
                ok = true;
                break;
            }
            if (g_recreate.exchange(false) || use12 != g_features_12) {   // perfdlss12 switched: the features are made again on the other API
                for (auto& f : g_feature) release_feature(f);
                for (auto& f : g_spare) release_feature(f);
                g_features_12 = use12;
            }
            g_render_w = w;
            const bool up = g_out_w > w && g_out_h > h;
            const UINT ow = up ? g_out_w : w, oh = up ? g_out_h : h;
            if (up && !make_tex(g_hi, ow, oh, work, use12)) { g_status = "upscale texture allocation failed"; break; }
            if (g_feature[eye] && (g_feature_w[eye] != w || g_feature_h[eye] != h || g_feature_ow[eye] != ow) && g_spare[eye] &&
                g_spare_w[eye] == w && g_spare_h[eye] == h && g_spare_ow[eye] == ow) {
                // Back to the kept size: swap the two (its history is old, so it starts again).
                std::swap(g_feature[eye], g_spare[eye]);
                std::swap(g_feature_w[eye], g_spare_w[eye]);
                std::swap(g_feature_h[eye], g_spare_h[eye]);
                std::swap(g_feature_ow[eye], g_spare_ow[eye]);
                if (g_spare_reuses.fetch_add(1) < 8)
                    spdlog::info("[DLSS] feature for eye {} reused: {}x{} -> {}x{} (kept from before; {}x{} kept in its place)", eye, w, h, ow, oh, g_spare_w[eye], g_spare_h[eye]);
                reset = true;
            }
            if (!g_feature[eye] || g_feature_w[eye] != w || g_feature_h[eye] != h || g_feature_ow[eye] != ow) {
                if (use12) worker_idle();   // perfdlss12thread: NGX's parameters and features are not touched while the worker runs
                release_feature(g_spare[eye]);
                g_spare[eye]    = g_feature[eye];   // kept for a switch back (a menu and the world)
                g_spare_w[eye]  = g_feature_w[eye];
                g_spare_h[eye]  = g_feature_h[eye];
                g_spare_ow[eye] = g_feature_ow[eye];
                g_feature[eye]  = nullptr;
                NVSDK_NGX_DLSS_Create_Params cp{};
                cp.Feature.InWidth = w; cp.Feature.InHeight = h;
                cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
                const float ratio = (float)w / (float)ow;
                cp.Feature.InPerfQualityValue = !up ? NVSDK_NGX_PerfQuality_Value_DLAA
                                              : ratio > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                              : ratio > 0.55f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                                              : NVSDK_NGX_PerfQuality_Value_MaxPerf;
                cp.InFeatureCreateFlags = g_flags.load();
                // The DLSS model: 0 = the library's default, 10 = J, 11 = K (DLSS 4 transformer, needs a 310.x nvngx_dlss.dll).
                const int preset = g_preset.load();
                for (const char* p : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                       NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                       NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality }) {
                    NVSDK_NGX_Parameter_SetUI(use12 ? g_d12.params : g_params, p, (unsigned int)preset);
                }
                if (use12 && !cl_open) {
                    // perfdlss12: this evaluation's D3D12 list, opened here on the render thread (the evaluation below goes into it too).
                    worker_idle();
                    slot = d12_begin();
                    if (slot < 0) { g_status = "perfdlss12: no D3D12 command list"; break; }
                    cl      = g_d12.list[slot];
                    cl_open = true;
                }
                const auto r = use12 ? NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &g_feature[eye], g_d12.params, &cp) : NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature[eye], g_params, &cp);
                if (NVSDK_NGX_FAILED(r)) { g_status = fmt::format("feature creation failed 0x{:x}", (uint32_t)r); g_feature[eye] = nullptr; break; }
                g_feature_w[eye] = w; g_feature_h[eye] = h; g_feature_ow[eye] = ow;
                spdlog::info("[DLSS] feature for eye {} created: {}x{} -> {}x{} ({}), colour format {}, preset {}, flags 0x{:x}{}", eye, w, h, ow, oh, up ? "upscaling" : "DLAA", (int)work, g_preset.load(),
                             g_flags.load(), use12 ? ", on D3D12 (perfdlss12)" : "");
                reset = true;
            }
            fc5::gputime::end(ctx, fc5::gputime::kDlssPrep);
            fc5::gputime::begin(ctx, fc5::gputime::kDlssEval);
            in_eval = true;
            float frame_ms = 11.1f;
            {
                // ofxrguides: OFXR refuses a DLSS evaluation without a frame time as its frame generation's guides
                // ("invalid publication", 30 Oct); 0 was passed. The time since this eye's last evaluation, 1-100 ms.
                static LARGE_INTEGER last[2]{}, freq{};
                LARGE_INTEGER now{};
                if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&now);
                const float ms = last[eye].QuadPart ? float(double(now.QuadPart - last[eye].QuadPart) * 1000.0 / double(freq.QuadPart)) : 11.1f;
                last[eye] = now;
                frame_ms = std::clamp(ms, 1.0f, 100.0f);
            }
            // [DlssEval] (perfsplit on): an event query just before the call tells whether the GPU had caught up when it returned.
            if (timing && !g_ev_query) {
                D3D11_QUERY_DESC qd{};
                qd.Query = D3D11_QUERY_EVENT;
                g_device->CreateQuery(&qd, &g_ev_query);
            }
            if (timing && g_ev_query) ctx->End(g_ev_query);
            const auto t_ev0 = std::chrono::steady_clock::now();
            ID3D11Texture2D* const out_tex = up ? g_hi.tex : g_out.tex;
            NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
            if (use12) {
                Tex* const out_t = up ? &g_hi : &g_out;
                Tex* const mv_t  = mv_in == g_mv_cam.tex ? &g_mv_cam : &g_mv;
                ID3D12Resource* const res[4]{ g_color_in.r12, g_depth32.r12, mv_t->r12, out_t->r12 };
                NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
                ep.Feature.pInColor          = g_color_in.r12;
                ep.Feature.pInOutput         = out_t->r12;
                ep.pInDepth                  = g_depth32.r12;
                ep.pInMotionVectors          = mv_t->r12;
                ep.InJitterOffsetX           = jx;
                ep.InJitterOffsetY           = jy;
                ep.InRenderSubrectDimensions = { w, h };
                ep.InReset                   = reset ? 1 : 0;
                ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
                ep.InFrameTimeDeltaInMsec    = frame_ms;
                // perfdlss12thread: the worker records and submits, unless this evaluation's list is already open (a feature was
                // just made into it on this thread).
                const bool threaded = g_d12_thread.load() && !cl_open;
                if (threaded) {
                    if (!g_worker_started) {
                        std::thread(worker_main).detach();
                        g_worker_started = true;
                        spdlog::info("[DLSS] perfdlss12thread: NGX's D3D12 recording on its own thread");
                    }
                    if (const uint32_t e = g_wk_err.exchange(0)) g_status = fmt::format("evaluation failed 0x{:x} (D3D12, worker)", e);
                    // D3D11 signals after its input passes and flushes; the worker's list waits for that on the GPU. D3D11's next
                    // command waits on the GPU for the value the worker will signal after the list.
                    g_d12.ctx4->Signal(g_d12.to12_11, ++g_d12.to12_v);
                    if (g_d12_flush.load()) ctx->Flush();
                    const auto t_fl = std::chrono::steady_clock::now();
                    D12Job job;
                    job.slot    = d12_slot();
                    job.feature = g_feature[eye];
                    job.ep      = ep;
                    std::copy(res, res + 4, job.res);
                    job.v12     = g_d12.to12_v;
                    job.v11     = ++g_d12.to11_v;
                    g_d12.slot_done[job.slot] = job.v11;
                    {
                        std::lock_guard lk{ g_jm };
                        g_jobs.push_back(job);
                    }
                    g_jcv.notify_one();
                    g_d12.ctx4->Wait(g_d12.to11_11, job.v11);
                    g_d12.evals++;
                    if (timing) {
                        g_ev12_ns[1] += std::chrono::duration_cast<std::chrono::nanoseconds>(t_fl - t_ev0).count();
                        g_ev12_ns[2] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_fl).count();
                    }
                } else {
                    worker_idle();   // its fence values come first
                    if (!cl_open) {
                        slot = d12_begin();
                        if (slot < 0) { g_status = "perfdlss12: no D3D12 command list"; break; }
                        cl      = g_d12.list[slot];
                        cl_open = true;
                    }
                    r       = record12(cl, g_feature[eye], ep, res);
                    cl_open = false;
                    const auto t_rec = std::chrono::steady_clock::now();
                    // The list runs even if the evaluation failed (a feature made in it is set up there).
                    // GPU-side hand-over: D3D11 signals after its input passes, D3D12 waits for that, runs, signals; D3D11 waits for
                    // that before its next command (the sharpening or the copy that reads the output). The flush sends the signal.
                    g_d12.ctx4->Signal(g_d12.to12_11, ++g_d12.to12_v);
                    if (g_d12_flush.load()) ctx->Flush();
                    const auto t_fl = std::chrono::steady_clock::now();
                    g_d12.queue->Wait(g_d12.to12, g_d12.to12_v);
                    ID3D12CommandList* lists[1]{ cl };
                    g_d12.queue->ExecuteCommandLists(1, lists);
                    g_d12.queue->Signal(g_d12.to11, ++g_d12.to11_v);
                    g_d12.slot_done[slot] = g_d12.to11_v;
                    g_d12.ctx4->Wait(g_d12.to11_11, g_d12.to11_v);
                    g_d12.evals++;
                    if (timing) {
                        const auto t_end = std::chrono::steady_clock::now();
                        g_ev12_ns[0] += std::chrono::duration_cast<std::chrono::nanoseconds>(t_rec - t_ev0).count();
                        g_ev12_ns[1] += std::chrono::duration_cast<std::chrono::nanoseconds>(t_fl - t_rec).count();
                        g_ev12_ns[2] += std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_fl).count();
                    }
                }
            } else {
                NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
                ep.Feature.pInColor  = g_color_in.tex;
                ep.Feature.pInOutput = out_tex;
                ep.pInDepth          = g_depth32.tex;
                ep.pInMotionVectors  = mv_in;
                ep.InJitterOffsetX   = jx;
                ep.InJitterOffsetY   = jy;
                ep.InRenderSubrectDimensions = { w, h };
                ep.InReset           = reset ? 1 : 0;
                ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
                ep.InFrameTimeDeltaInMsec = frame_ms;
                r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature[eye], g_params, &ep);
            }
            if (timing) {
                const int k = use12 ? 1 : 0;
                g_ev_ns[k] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_ev0).count();
                g_ev_n[k]++;
                if (g_ev_query && ctx->GetData(g_ev_query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) g_ev_idle[k]++;
            }
            if (NVSDK_NGX_FAILED(r)) { g_status = fmt::format("evaluation failed 0x{:x}{}", (uint32_t)r, use12 ? " (D3D12)" : ""); break; }
            g_evals_ok++;
            keep_ofxr_receipt(eye, out_tex);
            if (up) {
                // The game's own low-resolution path goes on (SMAA, then the HUD); the eye image is
                // composed at present from the DLSS output and the HUD.
                if (g_sharpness.load() > 0.0f && g_sharpen_cs && make_tex(g_sharp, ow, oh, work)) {
                    sharpen(ctx, g_hi, g_sharp);
                    ctx->CopyResource(g_hi.tex, g_sharp.tex);
                }
                g_have_hi = true;
                g_status  = fmt::format("DLSS {}x{} -> {}x{} per eye", w, h, ow, oh);
                ok        = false;
                break;
            }
            // DLSS output (sharpened) -> the SMAA target (same size; formats of one family copy directly).
            if (g_sharpness.load() > 0.0f && g_sharpen_cs && make_tex(g_sharp, w, h, work)) {
                sharpen(ctx, g_out, g_sharp);
                ctx->CopyResource(target_res, g_sharp.tex);
            } else {
                ctx->CopyResource(target_res, g_out.tex);
            }
            g_status = fmt::format("DLAA {}x{} per eye", w, h);
            ok = true;
        } while (false);
        if (cl_open) cl->Close();   // left before its submission: not run (the allocator is reset at its next use)
        fc5::gputime::end(ctx, in_eval ? fc5::gputime::kDlssEval : fc5::gputime::kDlssPrep);
        if (color_res) color_res->Release();
        if (target_res) target_res->Release();
        if (timing) eval_window();
        return ok;
    }
}
