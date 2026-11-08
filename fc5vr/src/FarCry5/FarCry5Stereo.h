#pragma once

#include <Mod.hpp>
#include "FarCry5Afw.h"
#include "FarCry5Gesture.h"
#include "FarCry5Drive.h"
#include "FarCry5LocalLights.h"
#include <d3d11.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <safetyhook/easy.hpp>
#include <safetyhook/inline_hook.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdint>
#include <set>
#include <sstream>
#include <array>
#include <tuple>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Far Cry 5 (Dunia, D3D11) stereo through the game's own viewport constants.
//
// Every scene view the game draws reads its camera from one constant buffer, the
// CViewportShaderParameterProvider (shader reflection keeps the name). It is uploaded with
// Map(WRITE_DISCARD) / Unmap once per view per frame, on the render thread. Positions are
// camera-relative, so the camera is a rotation-plus-projection: InvProjectionMatrix (c0),
// ProjectionMatrix (c8), ViewRotProjectionMatrix (c20) and its previous-frame copy (c24),
// registers as matrix rows (clip = M * p), right-handed view space looking down -Z.
//
// Before each Unmap of a main-scene viewport the module rewrites those matrices for the eye of
// this frame (alternate-eye rendering): the game camera is the headset's reference, and the eye
// pose relative to that reference plus the eye's own asymmetric FOV replace the game's view.
// The framework then hands even frames to the left eye and odd frames to the right.
class FarCry5Stereo : public Mod
{
public:
    static std::shared_ptr<FarCry5Stereo>& Get()
    {
        static std::shared_ptr<FarCry5Stereo> instance{ std::make_shared<FarCry5Stereo>() };
        return instance;
    }

    std::string_view get_name() const override { return "FarCry5Stereo"; }

    std::optional<std::string> on_initialize_d3d_thread() override;
    void                       on_present() override;
    void                       on_post_present() override;
    bool                       on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    // Before the swap chain is resized: the mod's own views of the back buffer go (a view left over makes ResizeBuffers fail).
    void                       release_backbuffer_views();
    // The game keeps running (no pause menu) when another window has focus: the headset's own
    // desktop app, the VR simulator, the F11 desktop window.
    std::atomic<bool>          m_keep_running{ true };
    // DLSS in place of SMAA (FarCry5Dlss): the shaders and slots it reads, the jitter it was given.
    ID3D11PixelShader*         m_smaa_ps{ nullptr };
    UINT                       m_smaa_color_slot{ 0 };
    ID3D11ComputeShader*       m_mv_cs{ nullptr };
    UINT                       m_mv_depth_slot{ 0 }, m_mv_out_slot{ 0 };
    float                      m_dlss_jitter_px[2]{};
    std::atomic<float>         m_dlss_jitter_sign[2]{ -1.0f, -1.0f };
    uint64_t                   m_dlss_evals{ 0 };
    int                        m_dlss_evals_frame{ 0 };
    uint64_t                   m_dlss_evals_eye[2]{}, m_dlss_resets_eye[2]{}, m_prev_fallback[2]{}, m_ring_src[2][2]{}, m_hist_disp_eye[2]{}, m_stale_copies[2][11]{};
    struct FrameRec { int frame{ -1 }, views{ 0 }, player{ 0 }, weapon{ 0 }, dlss{ 0 }; bool menu{ false }, hud{ false }; float ms{ 0 }; };
    FrameRec                   m_frame_recs[16]{};
    int                        m_black_logged{ 0 };
    std::chrono::steady_clock::time_point m_rec_prev_time{};
    // Screen-space reflections: the shaders, their constant-buffer slot, and a GPU readback probe.
    std::unordered_set<ID3D11PixelShader*> m_sslr_shaders{};
    UINT                       m_sslr_cb_slot{ 0 };
    std::atomic<int>           m_sslr_probe{ 0 };
    ID3D11Buffer*              m_staging_cb{ nullptr };
    void probe_sslr(ID3D11DeviceContext* ctx);
    // Screen-space ambient occlusion (SSBC compute): its constant buffer CSSBCProvider.
    std::unordered_set<ID3D11ComputeShader*> m_ssbc_cs{};
    UINT                       m_ssbc_cb_slot{ 0 };
    std::atomic<int>           m_ssbc_probe{ 0 };
    // Test switch: skip the compute dispatches whose shader names contain one of these words (skipcs a b ...).
    std::vector<std::string>   m_skip_cs_words{};
    std::mutex                 m_skip_cs_mtx{};
    uint64_t                   m_skipped_dispatches{ 0 };
    void probe_cs_cb(ID3D11DeviceContext* ctx, UINT slot, const char* tag, UINT max_len = 256, bool vs_stage = false);
    // cbprobe NAME [N]: the constant buffer NAME as the GPU sees it, at the next N compute dispatches reading it.
    std::unordered_map<ID3D11ComputeShader*, std::vector<std::pair<std::string, UINT>>> m_cs_cbs{};
    std::string                m_cb_probe_name{};
    std::unordered_map<std::string, std::string> m_cb_vars{}; // "hash CBName" -> its variables (used ones marked *)
    std::atomic<int>           m_cb_probe{ 0 };
    void cb_probe(ID3D11DeviceContext* ctx);
    // passdump: both eyes of one stereo pair, named compute-shader inputs/outputs and constant buffers to files.
    std::unordered_map<ID3D11ComputeShader*, std::vector<std::tuple<std::string, int, UINT>>> m_cs_res{};
    std::vector<std::string> m_pd_specs{};
    std::string              m_pd_tag{};
    std::atomic<int>         m_pd_state{ 0 };
    int                      m_pd_frame0{ 0 };
    bool                     m_pd_temporal{ false }; // passdumpt: left eye now ("L") and one stereo frame later ("R")
    bool                     m_pd_temporal_right{ false }; // passdumptr: the right eye now ("L") and one stereo frame later ("R")
    std::set<std::string>    m_pd_done{};
    void pd_dispatch(ID3D11DeviceContext* ctx, bool after);
    void cd_draw();
    void log_stale(int line, int eye, bool ps_path = false);
    std::atomic<int> m_stale_log{ 0 };
    void cd_record(char kind, ID3D11Resource* res, const void* data, uint32_t size, ID3D11ComputeShader* cs, UINT x, UINT y, UINT z);
    struct CdRec { uint8_t eye{}, kind{}; uint32_t size{}; uint64_t res{}; UINT xyz[3]{}; std::string name; std::vector<uint8_t> data; };
    std::vector<CdRec> m_cd_recs{};
    std::atomic<int>   m_cd_state{ 0 };
    int                m_cd_frame0{ 0 };
    std::string        m_cd_tag{};
    bool pd_write_texture(ID3D11DeviceContext* ctx, ID3D11Resource* r, const std::string& path);
    bool pd_write_cb(ID3D11DeviceContext* ctx, UINT slot, const std::string& path);
    bool fix_tile_light(float* d);
    // lightprobe N: at the light culling dispatch (u:LightZBins) of the next N frames, every buffer it reads and its
    // constant buffers, with the game and eye cameras and the weapon placement, appended to probes/lightprobe.bin.
    std::atomic<int> m_light_probe{ 0 };
    int              m_light_probe_frame{ -1 };
    ID3D11Buffer*    m_lp_staging{ nullptr };
    UINT             m_lp_staging_size{ 0 };
    void light_probe(ID3D11DeviceContext* ctx);
    // muzzlelight off|remove|move (see fix_lights).
    std::atomic<int>  m_muzzle_light{ 0 };
    std::atomic<bool> m_light_cs{ false };
    std::atomic<int>  m_muzzle_light_log{ 0 };
    uint64_t          m_muzzle_light_fixes{ 0 };
    uint64_t          m_muzzle_seen{ 0 }, m_light_lists{ 0 };
    // fastlights on|off (hitch thread 23 Oct; default on, saved; F11 Debug > Optimisation): the game's light lists are read only
    // when muzzlelight is on. Off: every list upload is also counted for the once-a-second light line and muzzlecount (two
    // passes over the whole mapped list, ~7 ms of render-thread CPU per pair in the simulator).
    std::atomic<bool> m_fast_lights{ true };
    void fix_lights(float* d, uint32_t size, uint32_t stride);
    // expprobe N (pair lock: the auto exposure pass skipped in the right eye's call when both eyes share a game frame).
    std::atomic<int>  m_exp_probe{ 0 };
    int               m_exp_probe_frame{ -1 };
    std::atomic<int>  m_pair_lock_log{ 0 };
    void exposure_probe(ID3D11DeviceContext* ctx, bool after);
    ID3D11Resource*  m_lp_watch[2]{};        // the light buffers seen by lightprobe (pointer compare only)
    std::atomic<int> m_lp_watch_log{ 0 };   // uploads to them still to log
    // Test head motion on top of the runtime's pose (degrees: yaw, pitch, roll; cmd testhead).
    std::atomic<float>         m_test_head[3]{};
    std::atomic<float>         m_test_head_pos[3]{};   // testheadpos X Y Z (research)
    uint64_t                   m_focus_msgs_blocked{ 0 };
    void                       on_draw_ui() override;

private:
    struct Candidate
    {
        uint32_t w{ 0 }, h{ 0 };
    };

    bool install_hooks(ID3D11Device* device);

public:
    static void install_early_hooks();
    // Before the framework makes its OpenXR instance: the start headset check (other headsets) is finished first;
    // then the start notes (eye size, headset check) are written to the log file, which did not exist yet.
    static void wait_headset_check();
    static void log_start_notes();
    void        install_shader_hooks(ID3D11Device* device);

private:
    std::mutex  m_shader_hook_mtx{};
    void on_viewport_constants(float* data, uint32_t size);
    // The eye of `frame` relative to the game camera (view space, metres scaled to game units) and the
    // tangents of its FOV (left, right, up, down). False = leave the game camera alone this frame.
    bool eye_rel(int frame, int eye, glm::mat4& rel, glm::vec4& tangents, bool& headset_fov);
    void afw_present(ID3D11DeviceContext* ctx);
    ID3D11Texture2D* select_scene_depth(ID3D11DeviceContext* ctx);
    void reflect_shader(const void* code, size_t size, const char* stage);
    void poll_commands();
    void dump_viewport(const float* data, uint32_t size, const char* why);
    void recenter_from_hmd();

    static HRESULT WINAPI hk_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped);
    static void WINAPI    hk_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub);
    static HRESULT WINAPI hk_create_vs(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11VertexShader** out);
    static HRESULT WINAPI hk_create_ps(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11PixelShader** out);
    static HRESULT WINAPI hk_create_cs(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11ComputeShader** out);
    static void WINAPI    hk_cs_set_shader(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n);
    static void WINAPI    hk_dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z);
    static void WINAPI    hk_dispatch_indirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset);
    static void WINAPI    hk_draw_idx_inst_indirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset);
    static void WINAPI    hk_draw_inst_indirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset);
    static void WINAPI    hk_draw_auto(ID3D11DeviceContext* ctx);
    static void WINAPI    hk_update_subresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* src, UINT row, UINT depth);
    static void WINAPI    hk_vs_set_cbs(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11Buffer* const* bufs);
    static void WINAPI    hk_ps_set_shader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n);
    static void WINAPI    hk_ps_set_samplers(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11SamplerState* const* samplers);
    // Material samplers (wrap addressing, linear or anisotropic filtering) are swapped for copies with
    // anisotropic filtering (aniso N, 0 = the game's) and, while DLSS upscales, a mip bias of
    // log2(render / output) + mipbias so textures keep the output's detail.
    safetyhook::InlineHook m_ps_samplers_hook{};
    std::atomic<int>       m_aniso{ 16 };
    std::atomic<float>     m_mip_bias_extra{ 0.0f };
    std::atomic<bool>      m_mip_bias_auto{ true };
    std::mutex             m_sampler_mtx{};
    struct SamplerKey { ID3D11SamplerState* s; int aniso; int bias_q; bool operator==(const SamplerKey&) const = default; };
    struct SamplerKeyHash { size_t operator()(const SamplerKey& k) const { return std::hash<void*>{}(k.s) ^ (size_t(k.aniso) << 20) ^ (size_t(k.bias_q) << 28); } };
    std::unordered_map<SamplerKey, ID3D11SamplerState*, SamplerKeyHash> m_sampler_clones{};
    std::unordered_set<ID3D11SamplerState*> m_sampler_passthrough{};
    std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> m_sampler_orig{};   // clone -> the game's sampler
    void restore_game_samplers(ID3D11DeviceContext* ctx);
    uint64_t               m_sampler_swaps{ 0 };
    static void WINAPI    hk_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    static void WINAPI    hk_draw(ID3D11DeviceContext* ctx, UINT count, UINT start);
    static void WINAPI    hk_draw_indexed_instanced(ID3D11DeviceContext* ctx, UINT count, UINT inst, UINT start, INT base, UINT start_inst);
    static void WINAPI    hk_draw_instanced(ID3D11DeviceContext* ctx, UINT count, UINT inst, UINT start, UINT start_inst);
    bool                  weapon_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT instances);
    bool                  weapon_draw_impl(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT instances);
    void                  bonesrc_draw(ID3D11DeviceContext* ctx, UINT count);   // bonesrc / bonescan research probes
    void                  note_pixel_shader(ID3D11PixelShader* ps, const void* code, size_t size);
    static void WINAPI    hk_clear_dsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil);
    static void WINAPI    hk_clear_rtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT color[4]);
    static void WINAPI    hk_clear_uav_uint(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* uav, const UINT values[4]);
    static void WINAPI    hk_clear_uav_float(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* uav, const FLOAT values[4]);
    static void WINAPI    hk_om_set_rts(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
    void                  note_depth_bind(ID3D11DepthStencilView* dsv);
    void                  probe_depth(ID3D11DeviceContext* ctx);

    // Stereo (both eyes every game frame): the render thread's scene call (FC_m64+0x4d3c70, called
    // once per frame just before the game's Present) runs twice, left then right, with a Present of
    // the left eye between them, so each eye goes through the per-eye pipeline as its own frame.
    static uint8_t         hk_scene(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
    void                   install_scene_hook();
    safetyhook::InlineHook m_scene_hook{};
    // One level deeper: the views call inside the scene (FC_m64+0x4d0620: weapon pass, world pass).
    static uintptr_t       hk_views(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g);
    safetyhook::InlineHook m_views_hook{};
    std::atomic<int>       m_stereo_level{ 0 };   // 0 = scene call twice, 1 = views call twice
    std::atomic<bool>      m_stereo2{ false };
    bool                   m_in_scene{ false };
    int                    m_stereo_trace{ 0 };              // log each step of the next N stereo frames
    std::atomic<bool>      m_stereo_mid_present{ true };     // test: Present the left eye between the passes
    std::atomic<bool>      m_stereo_second{ false };         // test: run the right pass (crashes today; off = stereo is paired AER)
    std::atomic<bool>      m_stereo_restore{ false };        // test: restore the frame packet before the right pass
    std::atomic<int>       m_stereo_restore_bytes{ 0x2040 };
    std::atomic<bool>      m_stereo_flat{ false };
    std::atomic<int>       m_stereo_rec{ 0 };                // research: record the scene into a command list (1 = execute once, 2 = twice)
    std::atomic<bool>      m_snap_left{ false };           // snapshot at the next left-eye present
    // Recorded stereo: recording open from the scene to the game's Present; the right eye's command list
    // waiting for the game's present path to return.
    bool                   m_rec_open{ false };
    ID3D11CommandList*     m_rec_cl{ nullptr };
    std::chrono::steady_clock::time_point m_rec_t0{};
    safetyhook::InlineHook m_present_path_hook{};
    static uintptr_t       hk_present_path(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
    void                   rec_finish_left(ID3D11DeviceContext* imm);
    void                   dump_backbuffer(ID3D11DeviceContext* imm, const char* tag);
    int                    m_rec_dump{ 0 };
    uint64_t               m_rec_fallbacks{ 0 };
    std::atomic<int>       m_bb_probe{ 0 };
    std::atomic<bool>      m_rec_redtest{ false };
    std::atomic<bool>      m_rec_redtest2{ false };
    std::atomic<bool>      m_packet_watch{ false };
    std::atomic<int>       m_packet_dump{ 0 };
    std::atomic<bool>      m_packet_freeze{ false };
    std::atomic<bool>      m_frame_double{ true };   // default on since 17 Oct (author), F11 Debug > Experimental
    std::atomic<int>       m_frame_double_calls{ 0 };
    std::atomic<int>       m_frame_double_resyncs{ 0 };
    int                    m_fd_doubled{ 0 }, m_fd_resync_window{ 0 }, m_fd_zero[2]{}, m_fd_multi{ 0 };
    double                 m_fd_ms[2]{};
    int                    m_fd_slow{ 0 };
    double                 m_fd_max_ms{ 0.0 };
    safetyhook::InlineHook m_frame_hook{};
    static uint8_t         hk_frame(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
    // Desktop window: the game's borderless full-screen window turned into a normal window (title bar,
    // movable, resizable) of this client size. The image is stretched into it; rendering is unchanged.
    std::atomic<bool>      m_window_mode{ false };
    bool                   menu_fill_viewport(ID3D11DeviceContext* ctx, UINT count, D3D11_VIEWPORT& saved, D3D11_RECT& saved_scissor, UINT& scissors);
    std::atomic<bool>      m_menu_fill{ true };          // menus: background scene stretched over the whole image
    std::atomic<bool>      m_menu_fill_force{ false };   // test: also without the flat screen (flat mode)
    int                    m_menu_fill_logged{ 0 };
    int                    m_menu_fill_branch_logged{ 0 };
    std::atomic<int>       m_menu_src_w{ 1920 }, m_menu_src_h{ 1080 };
    std::atomic<int>       m_vp_census{ 0 };             // menufill census: log the next N draws (viewport, target, shader)
    std::atomic<int>       m_window_w{ 1600 }, m_window_h{ 900 };
    void                   start_window_thread();
    std::atomic<int>       m_window_rect[4]{};              // x y w h the window is held at (0 w = not framed yet)
    std::atomic<bool>      m_window_user_sizing{ false };
    std::atomic<bool>      m_stereo_skip_end{ false };       // test: left pass skips the scene's end-of-frame releases           // test: stereo passes without a headset
    uint64_t               m_stereo_ticks{ 0 }, m_stereo_single{ 0 }, m_stereo_skipped{ 0 };
    double                 m_stereo_ms[3]{};    // left pass, left present, right pass (sum over the window)
    std::chrono::steady_clock::time_point m_stereo_window{};

    safetyhook::InlineHook m_map_hook{};
    safetyhook::InlineHook m_unmap_hook{};
    safetyhook::InlineHook m_create_vs_hook{};
    safetyhook::InlineHook m_create_ps_hook{};
    safetyhook::InlineHook m_create_cs_hook{};
    safetyhook::InlineHook m_om_set_rts_hook{};
    safetyhook::InlineHook m_clear_dsv_hook{};
    safetyhook::InlineHook m_clear_rtv_hook{}, m_clear_uavu_hook{}, m_clear_uavf_hook{};   // ssrclearlog: clears of the reflection output
    // Water research 16 Oct (after the Far Cry 3 VR water notes): occlusion queries and predication, and planar reflection readers.
    // queryvisible (off): every occlusion query and predicate the game reads back answers "visible" and predication is dropped, so no
    // eye's water (or anything) is hidden by a query result of the other eye's call or a frame late. Counted per second either way.
    safetyhook::InlineHook m_get_data_hook{}, m_set_pred_hook{};
    static HRESULT WINAPI hk_get_data(ID3D11DeviceContext* ctx, ID3D11Asynchronous* a, void* data, UINT size, UINT flags);
    static void WINAPI    hk_set_predication(ID3D11DeviceContext* ctx, ID3D11Predicate* pred, BOOL value);
    std::atomic<bool>     m_query_visible{ true };   // default on since 17 Oct (author), F11 Debug > Experimental
    std::atomic<uint64_t> m_occl_reads{ 0 }, m_occl_hidden{ 0 }, m_occl_forced{ 0 }, m_pred_sets{ 0 }, m_pred_nulled{ 0 }, m_planar_sets{ 0 };
    std::mutex            m_query_mtx;
    std::unordered_map<ID3D11Asynchronous*, int> m_query_kind;   // D3D11_QUERY of each query seen (-1: not a query)
    // cmd moonquery (default on; author 18 Oct): the game's visibility tests for the sun and moon disks always
    // answer "visible" (queryvisible does it for every test). Begin = 27, End = 28. A test is the disk's when its draw's pixel shader
    // binds CCelestialBodyShaderParameterProvider, or it is the first test to end after a disk buffer upload in this call.
    safetyhook::InlineHook m_query_begin_hook{}, m_query_end_hook{};
    static void WINAPI    hk_query_begin(ID3D11DeviceContext* ctx, ID3D11Asynchronous* a);
    static void WINAPI    hk_query_end(ID3D11DeviceContext* ctx, ID3D11Asynchronous* a);
    int                   query_kind(ID3D11Asynchronous* a);
    std::atomic<bool>     m_moon_query{ false };   // off since 19 Oct: the game draws both disks in every call (simulator), so no test hides them
    std::unordered_set<ID3D11Asynchronous*> m_sky_queries;        // tests found to be a disk's (kept; queries are reused)
    ID3D11PixelShader*    m_query_begin_ps{ nullptr };
    int                   m_disk_upload_frame{ -10 };             // frame of the last disk buffer upload (sky_eye_fix)
    bool                  m_disk_upload_open{ false };            // no visibility test has ended since that upload
    std::atomic<uint64_t> m_sky_q_ends{ 0 }, m_sky_q_by_ps{ 0 }, m_sky_q_reads{ 0 }, m_sky_q_hidden{ 0 }, m_sky_q_forced{ 0 }, m_occl_ends{ 0 };
    // 18 Oct 22:47 log: no test ever ended after a disk upload in the same call (the sky comes after the scene's tests), so the
    // last test to end before the upload is taken too; and the disk draws are counted (a call without one = the game skipped it).
    ID3D11Asynchronous*   m_last_occl_end{ nullptr };
    int                   m_last_occl_end_frame{ -10 };
    std::string           m_last_occl_end_names;
    std::atomic<uint64_t> m_sky_q_before{ 0 };
    bool                  m_cur_ps_celestial{ false };            // the bound pixel shader binds CCelestialBodyShaderParameterProvider
    uint64_t              m_cel_draws{ 0 }, m_cel_frames{ 0 }, m_disk_frames{ 0 };
    int                   m_cel_frame{ -10 }, m_disk_frame_seen{ -10 }, m_cel_tick_frame{ 0 };
    // cmd moonclear (sky thread, 18 Oct): the disk shaders (#DEC6B2AF moon, #7D892F9E sun) read the fog volume; for their draws it is
    // replaced by a clear one (moon_fog_swap, inside blank_textures, so it needs the lens-dirt option, on by default).
    struct MoonFogSlot { UINT slot; bool radiance; int dim; };
    std::unordered_map<ID3D11PixelShader*, std::vector<MoonFogSlot>> m_moon_fog_slots;
    ID3D11ShaderResourceView* m_moon_fog_srv[4]{};                // [3D?][radiance?]
    std::atomic<bool>     m_moon_clear{ true };    // on since 19 Oct (moon shaders only: they read VFRadiance), not headset-checked
    std::atomic<bool>     m_moon_nodepth{ false };                // cmd moonnodepth (test): disk draws without the depth test
    std::atomic<float>    m_moon_test_yaw{ 0.0f };                // cmd moonyaw D (test): sun/moon disks turned D deg about world up
    // cmd moonprobe (research, off again since the moon was fixed 20 Oct; ~2 32x32 copies a frame when on): the picture around the moon after its draw and at the SMAA / DLSS input (moon_probe_*).
    std::atomic<bool>     m_moon_probe{ false };
    std::atomic<bool>     m_moon_mv{ false };      // cmd moonmv (test): moon disk draws write their first target only (20 Oct log: they have only one, so it does nothing)
    std::atomic<bool>     m_moon_depth{ true };    // cmd moondepth (20 Oct, not headset-checked): the disks keep the game's depth row
    uint64_t              m_moon_depth_fixes{ 0 };
    uint64_t              m_moon_mv_draws{ 0 };
    struct MoonProbe { ID3D11Texture2D* staging{ nullptr }; DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN }; bool busy{ false }; int stage{ 0 }; int frame{ 0 }; };
    MoonProbe             m_moon_probe_ring[8]{};
    uint64_t              m_moon_probe_n{ 0 };
    int                   m_moon_probe_draw_frame{ -10 };
    struct MoonProbeStat { uint64_t n{ 0 }, bright{ 0 }; double ratio_sum{ 0.0 }; int format{ 0 }; int log_left{ 12 }; std::string samples; };
    MoonProbeStat         m_moon_probe_stat[2]{};
    float                 m_disk_ndc[2]{};
    bool                  m_disk_ndc_ok{ false };
    int                   m_disk_ndc_frame{ -10 };
    uint64_t              m_moon_buf_seen{ 0 }, m_moon_onscreen{ 0 };
    void                  moon_probe_capture(ID3D11DeviceContext* ctx, int stage);
    void                  moon_probe_read(ID3D11DeviceContext* ctx);
    ID3D11DepthStencilState* m_moon_nodepth_dss{ nullptr };
    uint64_t              m_moon_fog_draws{ 0 };
    bool                  moon_fog_swap(ID3D11DeviceContext* ctx);
    int                   m_query_names_log{ 0 };                 // [MoonQuery] pixel shaders of visibility tests (first 24 distinct)
    std::unordered_set<std::string> m_query_names_seen;
    std::unordered_set<ID3D11PixelShader*> m_planar_ps;          // pixel shaders that read a WaterReflection texture (m_ps_mtx)
    // perfpscache on|off (saved, default off; performance thread 6 Nov, F11 Debug > Performance switches): the sun/moon disk shaders
    // kept in a set when created, so a pixel shader bind takes one lock and no search through its names (off: two locks and a
    // search for "CCelestialBody" in the bound shader's names at every bind, a few thousand times per eye).
    std::unordered_set<ID3D11PixelShader*> m_celestial_ps;       // (m_ps_mtx)
    std::atomic<bool>                      m_perf_ps_cache{ false };
    safetyhook::InlineHook m_vs_cb_hook{}, m_draw_idx_hook{}, m_draw_hook{}, m_draw_idx_i_hook{}, m_draw_i_hook{};
    ID3D11Resource*        m_unmap_res{ nullptr };
    ID3D11Buffer*          m_vs_cb0{ nullptr };      // VS constant buffer slot 0 (the camera) as last bound
    ID3D11Resource*        m_weapon_cb{ nullptr };   // the first-person pass's camera buffer
    ID3D11Resource*        m_world_cb{ nullptr };    // the player's world view camera buffer
    // Every camera buffer of this frame's first-person / world passes (a frame can have more than one).
    ID3D11Resource*        m_weapon_cbs[4]{};
    ID3D11Resource*        m_world_cbs[4]{};
    int                    m_weapon_cb_n{ 0 }, m_world_cb_n{ 0 };
    bool is_weapon_cb(const void* p) const { for (int i = 0; i < m_weapon_cb_n; ++i) if (m_weapon_cbs[i] == p) return true; return false; }
    bool is_world_cb(const void* p) const { for (int i = 0; i < m_world_cb_n; ++i) if (m_world_cbs[i] == p) return true; return false; }
    glm::vec3              m_world_cam_pos{ 0.0f };
    struct ObjConsts { uint32_t size{ 0 }; float f[32]{}; };
    std::mutex                                       m_obj_mtx;
    std::unordered_map<ID3D11Resource*, ObjConsts>   m_obj_consts;
    std::unordered_map<ID3D11Resource*, std::vector<float>> m_bone_data;   // whole skinning palettes (m_keep_bones)
    std::atomic<bool>  m_keep_bones{ false };
    struct LayoutElem { std::string semantic; UINT index{ 0 }; DXGI_FORMAT format{}; UINT slot{ 0 }; UINT offset{ 0 }; UINT inst{ 0 }; };   // inst: D3D11_INPUT_PER_INSTANCE_DATA (weaponcache)
    std::unordered_map<ID3D11InputLayout*, std::vector<LayoutElem>> m_layouts{};
    safetyhook::InlineHook m_create_il_hook{};
    safetyhook::InlineHook m_create_tex2d_hook{};   // padglyphs (FarCry5PadGlyph.cpp)
    static HRESULT WINAPI hk_create_texture2d(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out);
    static HRESULT WINAPI hk_create_input_layout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* el, UINT n, const void* code, SIZE_T size, ID3D11InputLayout** out);
    // A mesh drawn with the gloves' shader, scanned once (glovemesh): its bones' rest-pose centroids, and whether its
    // shape is a pair of hands (two clusters far out to the sides, nothing in the middle) or other clothing (a shirt,
    // sleeves) that the intro outfit draws with the same shader.
    // arm_side: a one-arm mesh with its hand (barehands): 0 the right arm (rest-pose x > 0), 1 the left, -1 not one; hand_from:
    // its bones with a rest-pose |x| below this are the forearm's.
    struct GloveMesh { bool ok{ false }; bool gloves{ false }; int bones{ 0 }; int arm_side{ -1 }; float hand_from{ 0.0f }; std::vector<glm::vec4> cent{}; };
    // barehands (author 19 Oct: player models without gloves had no hands): the bare skin comes as one mesh per arm with its
    // fingers. Such a mesh is kept as a hand, its forearm folded onto the wrist, and moved like the gloves. Shaders: m_ps_mtx.
    std::atomic<bool>  m_bare_hands{ true };
    std::unordered_set<ID3D11PixelShader*> m_bare_hand_ps{};
    uint32_t           m_bare_folded{ 0 };
    // The support / weapon hand's pivot this call (game camera space), shared by every hand mesh of that side drawn in it,
    // so a glove piece and the skin under it move by the same transform.
    glm::vec3          m_hand_pivot[2]{};
    int                m_hand_pivot_frame[2]{ -1, -1 };
    bool               is_hand_mesh(const GloveMesh* gm) const { return gm && gm->ok && (gm->gloves || (m_bare_hands.load() && gm->arm_side >= 0)); }
    void               bare_hand_fold(std::vector<float>& bones, const GloveMesh& gm, float scale);
    // fingergrip (author 19 Oct): a hand that follows its controller (the floating support hand, the weapon hand off the weapon) is open, its
    // fingers given the palm's transform (the rest pose's flat hand), and closes into the game's own pose while its grip is held.
    std::atomic<bool>  m_finger_grip{ true };
    bool               m_finger_grip_in[2]{};               // the grips by hand side (0 = the right glove's side)
    bool               m_finger_trig_in[2]{};               // the triggers by hand side (author 19 Oct 20:38: grip = the index
                                                            // pointing, grip + trigger = a fist)
    float              m_finger_curl[2]{};                  // the fingers' curl, 0 open .. 1 closed, eased per call
    float              m_index_curl[2]{};                   // the index finger's
    float              m_thumb_curl[2]{};                   // the thumb's (round 3, 20 Oct: the real thumb on a button or rest = closed)
    bool               m_finger_ttouch_in[2]{};             // the index resting on the trigger (touch sensor)
    bool               m_finger_thumb_in[2]{};              // the thumb on a face button, the thumbrest or the stick
    int                m_finger_frame[2]{ -1, -1 };
    std::atomic<int>   m_finger_test{ -1 };                 // test command fingertest 0|1|2: open / pointing / fist on both hands
    uint32_t           m_finger_count{ 0 };                 // fingernames: the hand draw being posed, by its index count (weapon_draw_impl)
    uint32_t           m_finger_prop[3]{};                  // fingergame: the weapon in hand's spare magazine props (index counts; FarCry5Reload.h mag_parts)
    uint32_t           m_finger_prop_sig{ 0 };              // ... for this weapon
    int                m_finger_prop_at{ -1000 };           // ... looked up at this frame
    std::atomic<int>   m_finger_prop_frame{ -1000 };        // the last frame one of them was drawn (weapon_draw_impl)
    // A hand's rest-pose frame, found once per mesh and side: its bones' principal axes (u along the fingers, n towards the
    // palm, from the game's own grip pose), the knuckle line and each bone's role (palm, finger, index, thumb).
    struct HandFrame { bool ok{ false }; glm::vec3 P{}, u{}, n{}, a{}, B{}; float s_k{ 0 }, len{ 0 }; bool thumb_high{ false }; std::vector<uint8_t> role; std::vector<glm::quat> rec_q, prev_q; std::vector<glm::vec3> rec_t; int rec_frame{ -1 }, stable_n{ 0 }; float rec_closed{ 0 }, wmin{ 0 }, wmax{ 0 }; };
    // The index's side learned from the game's own grip of the weapon hand (its trigger finger is the straight one), as a
    // rest-space direction for that hand; the other hand uses its mirror image (x negated). Until learned: forward.
    glm::vec3          m_index_dir{ 0.0f };
    bool               m_index_learned{ false }, m_hand_frames_reset{ false };
    std::vector<glm::vec3> m_index_pts{};   // the weapon hand's index bones (rest space): the straight ones of its weapon grip, the whole finger
    float              m_rec_norm[2]{};      // each hand's recorded grip, mean turn per weight: the less closed hand is closed further (20 Oct)
    std::atomic<bool>  m_finger_swap{ false };   // fingerswap (F11 Experimental, saved): the index and thumb on the other side, if the mod picked the wrong finger
    std::map<std::pair<int, int64_t>, HandFrame> m_hand_frames{};
    void               hand_fingers(std::vector<float>& bones, const std::vector<glm::vec4>& cent, int side, float hand_from, float scale, bool record = false);
public:
    // The controllers by hand (right, left): trigger pressed, trigger touched, thumb on a button / rest / stick.
    void               finger_inputs(bool trig_r, bool trig_l, bool ttouch_r, bool ttouch_l, bool thumb_r, bool thumb_l)
    {
        m_finger_trig_in[0] = trig_r; m_finger_trig_in[1] = trig_l; m_finger_ttouch_in[0] = ttouch_r; m_finger_ttouch_in[1] = ttouch_l;
        m_finger_thumb_in[0] = thumb_r; m_finger_thumb_in[1] = thumb_l;
    }
private:
    // An indexed indirect draw's arguments (index count, instances, start index, base vertex), read back once per args buffer
    // and offset: the bare-skin arms of some player models are indirect draws (the author's save, 19 Oct), so their mesh could not be
    // scanned. Only character-material draws of the player's weapon pass are read.
    std::map<std::pair<void*, UINT>, std::array<int32_t, 5>> m_indirect_args{};
    // fresh: read now, past the cache (neither looked up nor kept; drivehandsdirect's check of the game's arguments in a vehicle).
    bool               indirect_args(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset, std::array<int32_t, 5>& out, bool fresh = false);
    uint32_t           m_indirect_args_read{ 0 };
    // gloveany (author 19 Oct: on some player models the hands are not found): a glove-shaped mesh with finger bones drawn
    // with another character material than #D07EDC82 is kept as the gloves; its pixel shaders (guarded by m_ps_mtx).
    std::atomic<bool>  m_glove_any{ false };
    std::unordered_set<ID3D11PixelShader*> m_glove_any_ps{};
    static constexpr int kGloveAnyBones = 20;   // fingers: the author's gloves hang on 126 bones; sleeves and shirts on 1-10
    std::map<std::tuple<void*, UINT, UINT, INT>, GloveMesh> m_glove_meshes{};
    // unarmedhand (default on, 13 Oct): with no weapon in hand (the weapon pass draws only the gloves) the right hand's own
    // centre is put on the right controller, as the left hand's is on the left one (off = the gun's grip offset and the
    // last gun's calibration, which left the empty right hand offset and turned).
    std::atomic<bool>  m_unarmed_hand{ true };
    // slinghand (default on, 16 Oct): with the weapon on the sling the empty right hand turns about its own centre, which
    // sits on the right controller like the left hand's (off = about the weapon's grip point, ~17 cm away: the hand swung
    // round it, "moving on a different axis", the author's video 16 Oct).
    std::atomic<bool>  m_sling_hand_pivot{ true };
    float              m_sling_pivot_off{ 0.0f };    // last distance between the hand's centre and the grip point (m)
    // slinghandtilt (default on, 16 Oct): the slung empty right hand turned so its fingers point like the left hand's on
    // its controller (mirrored); off = the game's grip pose, fingers 21-23 deg lower (more on some weapons).
    std::atomic<bool>  m_sling_hand_tilt{ true };
    glm::quat          m_sling_tilt{ 1.0f, 0.0f, 0.0f, 0.0f };   // the eased turn, in the controller's axes
    float              m_sling_tilt_deg{ 0.0f };                  // the last measured angle between the two directions
    // emptyhandstill on|off|always (FarCry5EmptyHands.cpp; default on, saved; author 22 Oct 17:25): with no weapon, on foot, the right
    // glove is moved onto its controller by its bones like the left one (no one-update lag, fingergrip fingers), and while
    // walking, running or sprinting (always: standing too) each empty hand keeps its standing pose on its controller; the
    // same with the weapon on the sling (both hands empty there too).
    std::atomic<int>   m_empty_still{ 1 };            // 0 off, 1 while moving, 2 always
    std::atomic<int>   m_empty_log{ 0 };              // emptyhandlog N: N per-frame lines
    struct EmptyHandsState;
    EmptyHandsState*   m_ehs{ nullptr };
    void               empty_hands_frame();                                     // once per frame, after m_unarmed is set
    bool               empty_hands_bones() const;                               // this frame: the empty right glove moved by its bones
    bool               empty_hands_active() const;                              // this frame: no weapon or the weapon on the sling, on foot, switch on
    bool               empty_hands_holding() const;                             // this frame: the hold is easing in or on
    glm::quat          empty_hand_turn(int side, const glm::quat& h, const glm::quat& pre);   // the turn (game-camera axes) for that hand's palm h; pre: what
                                                                                       // turns it after (controller axes <- game-camera axes, less the controller)
    bool               empty_hands_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed);
    // emptyhandalign on|off (FarCry5EmptyHands.cpp; default on, saved; author 23 Oct): with no weapon (or the
    // weapon on the sling) each empty palm is turned to one fixed pose on its controller, the pose the right hand has on a pistol's
    // grip (the left mirrored), instead of the game's idle pose. emptyhandgrip w x y z (saved; "default"): that pose, the right
    // palm from its rest pose to its controller's axes; emptyhandpose yaw pitch roll (saved): a turn on top, in the controller's
    // axes (the left mirrored); emptyhandlearn: the pose taken from the right hand on the weapon held now; emptyhandpalmlog N.
    std::atomic<bool>  m_empty_align{ true };
    // The right palm on a pistol's grip, rest pose -> controller axes (w x y z): emptyhandlearn with the revolver (weapon 18912,
    // default calibration) in the simulator, lake-dock, 23 Oct 15:52 (agreement 1.0000; x -48.8 y 28.8 z -28.0 deg).
    static constexpr float kEmptyGripDefault[4]{ 0.8807f, -0.3334f, 0.3167f, -0.1139f };
    std::atomic<float> m_empty_grip[4]{ kEmptyGripDefault[0], kEmptyGripDefault[1], kEmptyGripDefault[2], kEmptyGripDefault[3] };
    std::atomic<float> m_empty_pose[3]{ 0.0f, 0.0f, 0.0f };         // yaw pitch roll, degrees
    std::atomic<int>   m_empty_palm_log{ 0 };
    std::atomic<int>   m_empty_learn{ 0 };                           // emptyhandlearn: draws still to average
    bool               empty_hands_aligning() const;                 // this frame: the empty palms turned to the grip pose
    // shoulderdraw on|off (FarCry5WeaponSwitch.cpp; default off, saved; F11 Experimental > Weapon switch (test); author 26 Oct): a weapon-switch gesture (fc5::gesture::weapon_switch_count())
    // opens a switch window. Phase 1, from the grip until the game draws the new weapon with a known held pose (gunstill): the
    // weapon pass is hidden but the gloves, and the right glove is an empty hand on its controller (emptyhandstill's bones). Phase 2,
    // shoulderdrawms (saved, default 1500) from then: gunstill holds the new weapon at once and fully (no 0.6 m limit, no reload),
    // so the game's draw animation does not show. shoulderdrawwait ms (saved, default 1500; the first switch from the sling took 1.08 s in the simulator): with no new weapon id by then the game
    // did not switch and the old weapon shows again. shoulderdrawtest: the window opened and pad Y tapped, as the gesture does; shoulderdrawsnap a b: eye pictures a ms after the grip and b ms after the new weapon shows. With the switch off the window still opens, watched and logged only (for comparing).
    // weapon_switch_hiding(): phase 1 now (the holster thread draws its recorded copies then); weapon_switch_target(): the new
    // weapon's id once phase 2 began (0 before); weapon_switch_holds(sig): phase 2 holds that weapon now.
    std::atomic<bool>  m_sh_draw{ false };
    // shoulderinstant on|off (saved, default on, with shoulderdraw; author 27 Oct): in phase 1 the
    // recorded copy (FarCry5Holster.cpp) of the weapon the switch is guessed to bring is drawn in the hand from the grip (weapon_switch_guess).
    std::atomic<bool>  m_sh_instant{ true };
    uint32_t           weapon_switch_guess() const;
    bool               weapon_switch_needs_pose(uint32_t sig) const;   // phase 1 brought this weapon and its held pose is not known: gunstill takes it at the end of the draw
    std::atomic<int>   m_sh_draw_ms{ 1500 }, m_sh_draw_wait{ 1500 };
    struct WeaponSwitchState;
    WeaponSwitchState* m_wsw{ nullptr };
    void               weapon_switch_frame(uint32_t frame_sig);   // once per frame, after m_unarmed is set and before empty_hands_frame
    bool               weapon_switch_hiding() const;
    uint32_t           weapon_switch_target() const;
    bool               weapon_switch_holds(uint32_t sig) const;
    void               weapon_switch_note_hidden();                // weapon_draw_impl: one weapon-pass draw hidden in phase 1
    bool               weapon_switch_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    void               draw_weapon_switch_menu(const std::function<void(const char*)>& hint, const std::function<void(const char*)>& section);
    // offhandstill on|off|always (FarCry5EmptyHands.cpp; default on, saved; author 23 Oct 22:17): with a weapon in the weapon hand and the other hand free (no two-handed hold),
    // the free glove is put on its controller with the game's turn of it, and the game swings that arm while moving (its sprint
    // takes the left hand off the gun). While walking, running or sprinting (always: standing too) its palm keeps this weapon's
    // standing pose (learned standing still, per weapon) on its controller. offhandlog N: N per-frame lines.
    std::atomic<int>   m_off_still{ 1 };                             // 0 off, 1 while moving, 2 always
    std::atomic<int>   m_off_log{ 0 };
    bool               off_hand_active() const;                      // this frame: a weapon in hand, the other hand free, on foot, the switch on
    glm::quat          off_hand_turn(const glm::quat& h);            // the free hand's turn (game-camera axes) for its palm h, once per frame
    glm::quat          empty_hand_target(int side) const;            // that glove side's palm on its controller (rest pose -> controller axes)
    void               empty_hand_learn(const glm::quat& rel_right);  // emptyhandlearn: one draw of the right palm on its controller
    bool               m_unarmed{ false };            // the last finished frame drew the gloves and no weapon
    bool               m_glove_seen_this_frame{ false };
    glm::vec3          m_wpn_hand_cam{ 0.0f };        // the weapon-side glove's centre in the weapon pass's game-camera axes
    int                m_wpn_hand_frame{ -100 };
    // gripbyhand (default on, 16 Oct evening): each weapon's grip point (the point put on the controller) is the game's own
    // right hand on that weapon (median of its glove centre over 90 weapon-pass draws, once settled): the grip setting moved
    // by that hand's offset from griphand, the rifle 40362's hand (where the one grip point for all weapons was tuned). One
    // grip point for all weapons put the SMG (32325), whose hand the game holds 23 cm further forward than the rifle's, 16 cm
    // ahead of the real hand (author 16 Oct: right hand wrong with it).
    struct GripHand { int n{ 0 }; bool ok{ false }; glm::vec3 c{ 0.0f }; glm::vec3 s[90]{}; };
    std::atomic<bool>  m_grip_save_pending{ false };   // a weapon's grip point just measured: saved to fc5vr_gunposes.txt (gun_still)
    std::unordered_map<UINT, GripHand> m_grip_hand{};       // by weapon signature (m_weapon_sig), under m_weapon_cal_mtx
    std::atomic<bool>  m_grip_by_hand{ true };
    std::atomic<float> m_grip_hand_ref[3]{ 0.06f, -0.19f, -0.215f };  // griphand: the reference weapon's right hand (m, game-camera axes)
    UINT               m_gh_sig{ 0 };
    int                m_gh_since{ 0 };
    std::atomic<bool>  m_glove_mesh_only{ true };   // cmd glovemesh: only the glove-shaped mesh is kept (off = every draw of the gloves' shader)
    uint64_t           m_glove_clothes_hidden{ 0 };
    GloveMesh          hands_scan(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    const GloveMesh*   glove_mesh(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    std::atomic<int>   m_hand_test_lo{ 0 }, m_hand_test_hi{ 0 };
    ID3D11Buffer*      m_hand_standin{ nullptr };
    ID3D11Buffer*      m_hand_cb_orig[2]{};
    bool               m_hand_cb_swapped{ false };
    bool               hands_cb_swap(ID3D11DeviceContext* ctx, const std::vector<float>& bones);
    void               hands_cb_restore(ID3D11DeviceContext* ctx);
    std::atomic<bool>  m_bone_dump{ false };
    std::string        m_bone_dump_ps{ "D07EDC82" };
    void note_object_constants(ID3D11Resource* res, const float* data, uint32_t size);
    bool world_character_draw(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start = 0, INT base = 0);
    std::atomic<bool>  m_hide_body{ true };
    // Camera matrices the game copies into other constant buffers (shadow resolve, TAA, fog, sky...):
    // the game camera's versions found by value and replaced by the eye's.
    static constexpr int kMatCount = 11;
    // prev_vp: this eye's previous ViewProj (fogprev); ctp_full: this eye now -> its previous frame with the camera's move (aoexact,
    // whatever ctptrans says); depth_scale: CameraDistances.y, the linear depth texture's value -> metres (aoexact).
    struct MatSet { int frame{ -10 }; float orig[kMatCount][16]{}; float repl[kMatCount][16]{}; float prev_vp[16]{}; float ctp_full[16]{}; float depth_scale{ 0.0f }; };
    // One set per eye (frame parity): stereo rewrites both eyes' buffers within one frame. The third is the scope's
    // render call (scopepass), which runs with the right eye's frame number.
    MatSet             m_mats_eye[3]{};
    MatSet&            mats() { return m_mats_eye[m_scope_call ? 2 : (m_frame.load() & 1)]; }
    bool               m_scope_call{ false };   // inside the scope's render call (render thread only)
    // Stereo research: the camera data of a constant buffer the game just filled, for the given eye.
    void               rewrite_for_eye(ID3D11Resource* res, float* data, uint32_t size, bool scan, bool camera, int eye);
    std::atomic<bool>  m_eye_mats{ true };
    std::atomic<int>   m_matscan_frames{ 0 };
    uint64_t           m_mat_replaced{ 0 };
    std::unordered_set<std::string> m_matscan_seen{};
    void scan_matrices(ID3D11Resource* res, float* data, uint32_t size);
    // The deferred shadow resolve's camera-space inputs (CDeferredShadowsResolveParameterProvider, 768 B).
    std::atomic<bool>  m_eye_shadows{ true };
    std::atomic<int>   m_cbdump_size{ 0 };
    std::atomic<float> m_sun_vs[3]{};
    std::atomic<bool>  m_upright_billboards{ true };
    std::atomic<bool>  m_freeze_shadow_jitter{ true };
    std::atomic<bool>  m_shadow_jitter_pair{ true }; // with the freeze off: one pattern per stereo pair
    int                m_shadow_jitter_frame{ -10 };
    bool               m_shadow_jitter_saved{ false };
    float              m_shadow_jitter[40]{};
    std::atomic<int>   m_cbdump_reg{ -1 };
    std::atomic<float> m_cbdump_val{ 0.0f };
    std::atomic<int>   m_mapcensus_frames{ 0 };
    std::unordered_map<std::string, int> m_map_census{};
    // Previous-frame camera = the same eye two frames back (per-eye temporal history, for DLSS),
    // instead of the other eye one frame back.
    std::atomic<bool>  m_same_eye_prev{ true };
    struct EyeHist { int frame{ -10 }; glm::mat4 vrp{ 1.0f }; glm::vec3 cam{ 0.0f }; };
    EyeHist            m_eye_hist[2][2]{};
    glm::mat4          m_same_prev_vrp[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    glm::vec3          m_same_prev_cam[2]{};
    uint64_t           m_shadow_fixes{ 0 };
    // asmfloor on|off (default on, saved): far-shadow tile copies drawn with MinDepth raised (asm_floor_swap); asmfloordepth D.
    std::atomic<bool>  m_asm_floor{ true };
    std::atomic<float> m_asm_floor_depth{ 0.001f };
    bool               m_asm_dsv_atlas{ false };   // the far-shadow atlas is the bound depth target (render thread)
    bool               m_asm_floor_swapped{ false };
    D3D11_VIEWPORT     m_asm_floor_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT               m_asm_floor_vpn{ 0 };
    uint64_t           m_asm_floor_draws{ 0 };
    int                m_shadow_logged{ 0 };
    bool fix_shadow_resolve(float* d);
    void asm_floor_swap(ID3D11DeviceContext* ctx);      // asmfloor
    void asm_floor_restore(ID3D11DeviceContext* ctx);   // asmfloor
    std::atomic<bool>  m_body_origin_camrel{ false };
    std::atomic<float> m_body_radius{ 0.6f };
    // bodymatch (default on): only the player's own body is hidden: draws at the player's root (the character root
    // nearest the camera within the radius, followed frame to frame, camera-relative) or right at the camera
    // (bodycore); people standing next to the player stay. Off = every character within the radius (old way).
    std::atomic<bool>  m_body_match{ true };
    std::atomic<float> m_body_match_tol{ 0.2f };    // m (across the ground): how far a draw's root may be from the player's
    std::atomic<float> m_body_core{ 0.15f };        // m: roots this close to the camera horizontally are always the player's
    glm::vec3          m_body_rel{ 0.0f }, m_body_cand{ 0.0f };
    int                m_body_rel_frame{ -100 }, m_body_cand_frame{ -100 };
    float              m_body_cand_h{ 1e9f };
    uint64_t           m_body_spared{ 0 };
    std::atomic<int>   m_body_census_frame{ -1 };
    std::atomic<uint64_t> m_body_draws_hidden{ 0 };
    int                    m_weapon_draws_this_frame{ 0 }, m_weapon_draws_last_frame{ 0 };
    // The weapon in hand: its signature (the index count of its biggest non-arm draw, last frame) and
    // per-weapon model corrections (yaw, pitch in degrees) set with weaponcal, persisted.
    UINT                   m_weapon_sig_frame{ 0 };
    std::atomic<UINT>      m_weapon_sig{ 0 };
    std::mutex             m_weapon_cal_mtx{};
    std::unordered_map<UINT, std::pair<float, float>> m_weapon_cal{};
    std::atomic<int>       m_census_frame{ -1 };
    // firecensus N (2026-11-04, author): the census for N frames from the
    // next shot the mod lets through, both eyes: every camera upload and every weapon-pass draw, drawn or dropped, with gunstilllate's state.
    std::atomic<int>       m_fire_census{ 0 };
    std::atomic<int>       m_fc_from{ -1 }, m_fc_to{ -2 };
    // firecensus N S (5 Nov, author): S shots in a row (default 1),
    // each with a 4-frame baseline 70 frames (about 0.8 s) after the shot, once the sight is back.
    std::atomic<int>       m_fire_census_shots{ 0 };
    // firecensusauto on|off (5 Nov, default off so logs stay small, not saved): the first 2 shots of each weapon held, once per
    // weapon in a session, get firecensus 6 by themselves (the log grows by a few hundred lines per weapon).
    std::atomic<bool>      m_fc_auto{ false };
    std::unordered_set<uint32_t> m_fc_auto_done{};
    std::atomic<int>       m_fc_base_from{ -1 }, m_fc_base_to{ -2 };
    bool                   census_now() const { const int f = m_frame.load(); return m_census_frame.load() == f || (f >= m_fc_from.load() && f <= m_fc_to.load()) || (f >= m_fc_base_from.load() && f <= m_fc_base_to.load()); }
    std::atomic<uint64_t>  m_weapon_skip_mask{ 0 };
    std::atomic<bool>      m_hide_arms{ true };
    std::unordered_set<ID3D11PixelShader*> m_arm_shaders{};
    std::unordered_set<ID3D11PixelShader*> m_vehicle_shaders{};   // pixel shaders binding a Vehicle_* material (the vehicle around the player)
    std::mutex                                           m_ps_mtx{};
    std::unordered_map<ID3D11PixelShader*, std::string>  m_ps_names{};
    std::unordered_map<ID3D11PixelShader*, std::vector<std::pair<std::string, UINT>>> m_ps_tex_slots{};   // throwfile: each pixel shader's textures and ("cb:" + name) constant buffers, by name
    // Pixel shader tracking: the world map (the map goes on the flat screen) and draws dropped by
    // shader resource names (live command "skipps <name>", e.g. the sun glare).
    safetyhook::InlineHook                    m_ps_set_hook{};
    // Compute shaders: names (CRC32 + bindings) and temporal history resources, swapped per eye by content.
    safetyhook::InlineHook                    m_cs_set_hook{}, m_dispatch_hook{}, m_dispatch_ind_hook{}, m_update_sub_hook{}, m_dii_hook{}, m_di_hook{}, m_dauto_hook{};
    uint64_t                                  m_indirect_draws{ 0 };
    uint64_t                                  m_update_sub_cb{ 0 };
    // Impostors (far trees): quad axes in the game camera's view space, rotated into the eye's.
    std::atomic<bool>                         m_eye_impostors{ true };
    uint64_t                                  m_impostor_fixes{ 0 };
    bool fix_impostors(float* d);
    // lampeye (FarCry5LocalLights): the lamp data slots of each compute shader (LightsLightingData, ShadowData; 255 none).
    std::unordered_map<ID3D11ComputeShader*, std::pair<UINT, UINT>> m_lamp_cs{};
    fc5::locallights::Saved lamp_swap(ID3D11DeviceContext* ctx);
    // Far tree line (global impostors, VS #0BC76EBF): its cards turn about U = (c8.z, c9.z, c10.z) of GlobalImpostors_BillboardMatrix,
    // (0,1,0) = screen up. Per draw, a stand-in block with U = the eye's roll-free up in view space (cmd impostorup).
    // Default on (author 2026-10-13: tilting far trees solved). The 2026-10-12 GPU timeouts came from fogprev, not this.
    std::atomic<bool>                         m_impostor_up{ true };
    std::atomic<int>                          m_impostor_up_log{ 0 };
    std::atomic<ID3D11VertexShader*>          m_gimp_vs[4]{};
    std::atomic<int>                          m_gimp_vs_n{ 0 };
    ID3D11Buffer*                             m_gimp_standin{ nullptr };
    ID3D11Buffer*                             m_gimp_orig{ nullptr };
    UINT                                      m_gimp_first{ 0 }, m_gimp_num{ 0 };
    bool                                      m_gimp_swapped{ false };
    uint64_t                                  m_gimp_swaps{ 0 };
    std::atomic<int>                          m_gimp_via_seen{ 0 };   // bit per draw type the far tree line was found in (logged once each)
    bool impostor_up_swap(ID3D11DeviceContext* ctx, int via, UINT count);   // via: 0 DrawInstancedIndirect, 1 DrawIndexedInstancedIndirect, 2 DrawInstanced, 3 DrawIndexedInstanced
    void impostor_up_restore(ID3D11DeviceContext* ctx);
    float                                     m_gimp_u[3]{ 0.0f, 1.0f, 0.0f };   // this eye's roll-free up in its view space
    int                                       m_gimp_u_frame{ -10 };
    void note_billboard_up(const glm::mat4& er);
    std::unordered_map<ID3D11ComputeShader*, std::string>       m_cs_names{};
    std::unordered_map<ID3D11ComputeShader*, std::vector<std::pair<bool, UINT>>> m_cs_hist{}; // (uav?, slot)
    ID3D11ComputeShader*                      m_cur_cs{ nullptr };
    uint64_t                                  m_cs_hist_swaps{ 0 };
    std::atomic<int>                          m_cs_census_frame{ -1 };
    std::unordered_map<ID3D11ComputeShader*, int> m_cs_census{};
    void note_compute_shader(ID3D11ComputeShader* cs, const void* code, size_t size);
    void swap_cs_history(ID3D11DeviceContext* ctx);
    std::unordered_set<ID3D11PixelShader*>    m_map_shaders{};
    std::unordered_set<ID3D11PixelShader*>    m_skip_shaders{};
    std::vector<std::string>                  m_skip_patterns{ "FlareDiffuseSampler" }; // the sun's lens-flare sprites (cmd skipps)
    ID3D11PixelShader*                        m_cur_ps{ nullptr };
    bool                                      m_cur_ps_skip{ false };
    // perfdrawflags (6 Nov, author): which of the per-draw shader sets the bound pixel
    // shader is in, read once at its bind (the sets change only when a shader is created, before its first bind). Each draw then skips
    // the locks and lookups for the sets it is not in. Off: every draw looks each set up as before.
    enum : uint32_t { kPsHud = 1u, kPsHudPrim = 2u, kPsTracerMesh = 4u, kPsTracer = 8u, kPsWorldTracer = 16u, kPsArm = 32u, kPsNullTex = 64u, kPsHist = 128u,
                      kPsTaaMv = 256u, kPsVehicle = 512u };
    uint32_t                                  m_cur_ps_flags{ 0 };
    bool                                      m_cur_ps_flags_ok{ false };   // the flags are this bind's (perfdrawflags was on at the bind)
    std::atomic<bool>                         m_perf_draw_flags{ false };
    bool                                      ps_maybe(uint32_t bits) const;   // false only when the bound shader is surely in none of these sets
    std::atomic<int>                          m_map_seen_frame{ -1000 };
    std::atomic<bool>                         m_map_flat{ true };
    // mapsticky (update_menu_screen): the map kept flat until it is closed, also while its shaders are not drawn.
    std::atomic<bool>                         m_map_sticky{ true };
    std::atomic<bool>                         m_map_close_press{ false };   // B, Back or Start sent to the game (note_map_close)
    int                                       m_map_close_frame{ -1000 };   // the frame of the last such press
    int                                       m_map_held_weapon{ 0 };       // frames in a row with the first-person pass, map held
    bool                                      m_map_held{ false };          // the map kept flat without its shaders now
    // binoczoom (update_binoculars): the eyes magnified while the game's binoculars narrow its camera.
    std::atomic<bool>                         m_binoc_zoom{ true };
    std::atomic<bool>                         m_binoc_press{ false };      // D-pad up sent to the game (note_binoc_press)
    std::atomic<float>                        m_binoc_mag{ 1.0f };         // the eyes' magnification now (eye_rel)
    bool                                      m_binoc_armed{ false };      // a D-pad up press on foot: the binoculars may be up
    float                                     m_binoc_base_tan{ 0.0f };    // the game camera's tan(hfov / 2) before the press
    float                                     m_binoc_peak{ 1.0f };        // the largest magnification since the press
    std::chrono::steady_clock::time_point     m_binoc_armed_at{};
    void update_binoculars(float tan_half);
public:
    void note_map_close(bool down) { static bool was = false; if (down && !was) m_map_close_press = true; was = down; }
    void note_binoc_press(bool down) { static bool was = false; if (down && !was) m_binoc_press = true; was = down; }
private:
    std::atomic<int>                          m_ps_census_frame{ -1 };
    std::atomic<int>                          m_ps_census_end{ -1 };
    std::unordered_map<ID3D11PixelShader*, int> m_ps_census_pass{};   // 1 weapon pass, 2 world, 4 other
    std::unordered_map<ID3D11PixelShader*, int> m_ps_census{};
    uint64_t                                  m_skipped_draws{ 0 };
    void set_skip_patterns(const std::vector<std::string>& pats);
    bool ps_matches_skip(const std::string& names) const;
    // Textures blanked for one draw by name (the bloom's dirty-lens glare): pixel shader -> slots.
    std::unordered_map<ID3D11PixelShader*, std::vector<UINT>> m_null_tex{};
    std::atomic<bool>  m_no_lens_dirt{ true };
    uint64_t           m_null_tex_draws{ 0 };
    bool blank_textures(ID3D11DeviceContext* ctx);
    // Per-eye temporal history: pixel shaders reading a history texture (TAA, bloom response, DOF)
    // get the history of their own eye (two frames back) instead of the other eye's.
    struct HistStash { ID3D11Texture2D* tex[2]{}; ID3D11ShaderResourceView* srv[2]{}; bool valid[2]{}; int frame[2]{ -100, -100 }; D3D11_TEXTURE2D_DESC desc{}; };
    uint64_t           m_hist_stale{ 0 };
    uint64_t           m_swaps_eye[2]{};    // per-eye history uses (probe line "per-eye history")
    uint64_t           m_stale_eye[2]{};   // per-eye histories too old to use (the game's own kept)
    // histage N (frames, default 4): an eye's own stored history is used when it is at most N frames old (was: exactly 1).
    // A pass the game runs only every other frame (headset frame rates) found its own copy 3 frames old and kept the
    // other eye's history every time (headset logs: ~2 per frame per eye "too old", the simulator none).
    std::atomic<int>   m_hist_max_age{ 4 };
    uint64_t           m_hist_aged{ 0 };
    bool               hist_recent(int diff) { if (diff == 1) return true; if (diff > 1 && diff <= m_hist_max_age.load()) { m_hist_aged++; return true; } return false; }
    std::mutex         m_stale_mtx;
    std::map<std::string, int> m_stale_by_pass;   // "CS #hash line N" -> too-old count this second
    std::atomic<bool>  m_ps_hist_key{ true };   // cmd pshistkey: pixel-pass histories keyed by shader, slot, shape and order (off: per bound texture)
    std::unordered_map<uint64_t, std::pair<int, int>> m_ps_hist_idx;
    std::unordered_map<ID3D11PixelShader*, std::vector<UINT>>   m_hist_slots{};
    std::unordered_map<uint64_t, HistStash>                     m_hist_stash{};
    std::unordered_map<uint64_t, HistStash>   m_cs_stash{};
    struct HistStash3 { ID3D11Texture3D* tex[2]{}; int frame[2]{ -100, -100 }; D3D11_TEXTURE3D_DESC desc{}; };
    std::unordered_map<uint64_t, HistStash3>  m_cs_stash3{};
    std::unordered_map<uint64_t, HistStash>   m_ssr_in_stash{};   // reflection inputs from last frame (SSLRInputTexture, PrevReflectionMotionBuffer), per eye
    std::unordered_map<uint64_t, int>         m_ssr_in_done{};    // frame each of those was last given its eye's copy (several passes read it per frame)
    std::atomic<uint64_t> m_ssr_in_swaps{ 0 };
    // cmd ssrrestore: after this eye's call (at present) the swapped reflection inputs get back what the game left in them,
    // because the game later rewrites them only in part (the left eye's reflections showed the right eye's picture).
    std::atomic<bool>     m_ssr_restore_on{ false };   // simulator 02:40: no visible change, off
    std::atomic<bool>     m_ssr_src_hist{ false };   // cmd ssrsrchist: the SSR input draws' SourceTexture treated as a per-eye history (it is this frame's picture: off)
    std::vector<std::pair<ID3D11Resource*, ID3D11Texture2D*>> m_ssr_restore{};
    uint64_t              m_ssr_restores{ 0 };
    // Research (cmd ssrwriter N): draws into the SSR input pictures (render target binds of those textures), N log lines.
    std::atomic<int>      m_ssrw_left{ 0 };
    std::unordered_set<ID3D11Resource*> m_ssrw_res{};
    bool                  m_ssrw_watch{ false };
    std::unordered_map<ID3D11ComputeShader*, std::pair<int, int>> m_cs_disp_idx{};
    std::atomic<bool>  m_hist_by_slot{ true }; // cmd histslot: compute histories stashed per shader slot (works with the game's two alternating textures)
    struct OutStash { ID3D11Texture2D* tex{}; int frame{ -100 }; D3D11_TEXTURE2D_DESC desc{}; };
    std::unordered_map<uint64_t, OutStash> m_cs_out_stash{};
    std::vector<std::pair<OutStash*, ID3D11Resource*>> m_post_copies{};
    std::atomic<bool>  m_hist_from_output{ true }; // cmd histout: per-eye compute history from the pass's own previous output
    std::atomic<bool>  m_hist_key_shape{ false };  // cmd histshape: history keys by texture shape and order among same-shape dispatches
    std::unordered_map<uint64_t, std::pair<int, int>> m_cs_shape_idx{};
    std::atomic<bool>  m_hist3d_shape{ true };     // cmd hist3dshape (with fogeye): 3D (fog volume) histories keyed by shape and order, not by shader variant
    std::unordered_map<uint64_t, std::pair<int, int>> m_cs_shape3_idx{};
    std::unordered_map<ID3D11ComputeShader*, std::array<uint64_t, 2>> m_hist_disp_cs{};
    std::atomic<float> m_shadow_new_weight{ 1.0f }; // cmd shadownew N: least share of the new frame in the temporal shadow resolve (game 0.01)
    uint64_t           m_out_hist_swaps{ 0 };
    void cs_history_post(ID3D11DeviceContext* ctx);
    std::atomic<bool>  m_pair_lock{ true };   // cmd pairlock: right eye gets the left eye's adaptive non-camera values; exposure updated once per pair
    int                m_lock_frame{ -10 };
    float              m_lock_vals[4][9]{};     // per upload of the left eye's call (pairlockslots)
    uint32_t           m_lock_set{ 0 }, m_lock_kinds{ 0 };   // slots filled / weapon-kind slots
    uint64_t           m_pair_lock_refused{ 0 };
    uint64_t           m_pair_locks{ 0 }, m_exposure_skips{ 0 };
    std::unordered_set<ID3D11ComputeShader*> m_exposure_cs{};
    std::atomic<bool>  m_eye_history{ true };
    uint64_t           m_hist_swaps{ 0 };
    bool swap_in_eye_history(ID3D11DeviceContext* ctx);
    // taamv (TAA thread, 16 Oct): the game's own TAA resolve (in-game anti-aliasing TAA; PS #902DA714, scope variant #76DF16DF)
    // takes its history at uv + the game's 8-bit motion, which has the head's turn only: with the head moving the history slid
    // (doubled, vibrating edges). Its motion texture is swapped for shadowmv's copy of this eye's call (still points carry the
    // eye's exact move since its previous call, ctp_full; points that move by themselves keep the game's code).
    std::atomic<bool>  m_taa_mv{ true };
    std::unordered_map<ID3D11PixelShader*, std::pair<UINT, UINT>> m_taa_mv_slot;   // TAA resolve -> its MotionVectorsTexture and MotionVectorsHistoryTexture slots (m_ps_mtx)
    std::unordered_map<ID3D11PixelShader*, UINT> m_taa_color_slot;   // TAA resolve -> its SceneColorTexture slot (taaskip, m_ps_mtx)
    uint64_t           m_taa_draws{ 0 }, m_taa_mv_runs{ 0 }, m_taa_mv_miss{ 0 }, m_taa_mv_size{ 0 }, m_taa_mvh_runs{ 0 };
    std::atomic<int>   m_taa_log{ 0 };   // taalog N: N draws of the resolve with their bound textures
    bool taa_mv_swap(ID3D11DeviceContext* ctx);
    // taaskip (default on): with the in-game anti-aliasing on TAA the game draws its SMAA resolve (which DLSS replaces) and then
    // its TAA resolve on top of DLSS's picture: a second temporal blend of an already resolved image with the game's turn-only
    // motion (doubled, shaking). With DLSS on and DLSS's call made, the TAA resolve's target gets DLSS's picture copied instead.
    std::atomic<bool>  m_taa_skip{ true };
    int                m_smaa_seen_frame{ -100 };
    uint64_t           m_taa_skips{ 0 }, m_taa_skip_refused{ 0 }, m_taa_sharpen_skips{ 0 };
    int                m_taa_skip_frame{ -100 };              // frame the TAA resolve was last passed over
    ID3D11PixelShader* m_taa_sharpen_ps{ nullptr };           // the TAA's sharpen pass (#F73CAD14) and its SceneColorTexture slot
    UINT               m_taa_sharpen_slot{ 0 };
    bool taa_skip(ID3D11DeviceContext* ctx);
    // dlssjitgate (default on; 16 Oct, author): DLSS replaces the SMAA resolve, so with the
    // in-game anti-aliasing off DLSS never runs, but the eyes were still drawn with its sub-pixel jitter: an aliased picture
    // shaking by up to a pixel (still head, simulator: 1.4-2.3 % of the view changed each frame against 0.7-1.0 % with DLSS off).
    // The jitter is used only while the SMAA resolve was drawn in the last 8 frames.
    std::atomic<bool>  m_dlss_jit_gate{ true };
    uint64_t           m_dlss_jit_gated{ 0 };
    bool dlss_jitter_ok(int frame) { if (!m_dlss_jit_gate.load() || frame - m_smaa_seen_frame <= 8) return true; m_dlss_jit_gated++; return false; }
    void restore_textures(ID3D11DeviceContext* ctx);
    std::atomic<int>       m_clear_log{ 0 };
    std::atomic<bool>      m_depth_trace_req{ false };
    // The scene depth: the full-size target cleared to 0 after the player camera upload. Snapshot at
    // its first release in the frame (later passes can overwrite it before present).
    ID3D11Texture2D*       m_scene_target{ nullptr };   // held with a reference (review R1: a raw pointer could outlive the texture after a resize)
    void set_scene_target(ID3D11Texture2D* t) { if (t == m_scene_target) return; if (t) t->AddRef(); if (m_scene_target) m_scene_target->Release(); m_scene_target = t; }
    bool                   m_scene_bound{ false };
    bool                   m_scene_cleared_this_frame{ false };
    ID3D11Texture2D*       m_scene_snapshot{ nullptr };
    int                    m_scene_snapshot_frame{ -1 };
    uint64_t               m_snapshots{ 0 }, m_present_snapshots{ 0 };
    uint32_t               m_scene_clears{ 0 };   // identifying clears seen since the last report
    void                   snapshot_scene_depth(ID3D11DeviceContext* ctx);
    int                    m_depth_trace_frame{ -1 };
    ID3D11Texture2D*       m_trace_target{ nullptr };
    ID3D11Texture2D*       m_trace_bound{ nullptr };
    int                    m_trace_events{ 0 };
    void                   trace_depth_sample(ID3D11DeviceContext* ctx, const char* what);
    bool                   m_hooked{ false };

    // Live state (render thread unless noted)
    std::atomic<int>      m_frame{ 0 };
    uint64_t              m_presents{ 0 };
    uint32_t              m_main_w{ 0 }, m_main_h{ 0 };          // largest perspective viewport this second
    uint32_t              m_pending_w{ 0 }, m_pending_h{ 0 };
    std::chrono::steady_clock::time_point m_size_window{};
    uint32_t              m_views_this_frame{ 0 }, m_modified_this_frame{ 0 };
    uint32_t              m_views_last_frame{ 0 }, m_modified_last_frame{ 0 };
    uint64_t              m_unmaps_total{ 0 }, m_viewports_total{ 0 };

    // Depth discovery: full-size depth targets the game binds (render thread only).
    struct DepthTarget
    {
        DXGI_FORMAT format{};
        uint32_t    binds{ 0 };          // since the last report
        uint32_t    binds_after_player{ 0 }; // bound after this frame's player camera upload
        uint32_t    last_frame{ 0 };
    };
    std::unordered_map<ID3D11Texture2D*, DepthTarget> m_depth_targets{};
    uint32_t          m_player_views_this_frame{ 0 };
    std::atomic<bool> m_probe_depth{ false };

    // AFW (flat test for now): both eye cameras per frame, the scene depth, the test switches.
    struct FrameCams
    {
        int               frame{ -1 };
        fc5::afw::Camera  cam[2]{};
        glm::mat3         game_rot{ 1.0f };   // the game camera this frame (world -> view)
        glm::vec3         game_pos{ 0.0f };
        glm::mat4         proj{ 1.0f };       // the projection written (flat test: the game's)
        // afwvehbody / afwvehmv: the vehicle's raw frame (camera minus head look, at the game camera), which the cab is fixed
        // in; game_rot / game_pos are the eyes' frame (airfollow's blend in a vehicle).
        bool              veh_valid{ false };
        glm::mat3         veh_rot{ 1.0f };
        glm::vec3         veh_pos{ 0.0f };
    };
    fc5::afw::Camera   camera_for(const FrameCams& rec, int frame, int eye);
    // Both eyes' views of the pose sample each frame was given (headset): the eye drawn and the
    // view its held partner is rebuilt for. Filled right after the framework locates the views.
    struct XrViews
    {
        int   frame{ -1 };
        float pos[2][3]{};
        float rot[2][4]{};   // x y z w
        float fov[2][4]{};   // left right up down (angles, radians)
    };
    XrViews            m_xr_views[8]{};
    void               store_xr_views(int frame);
    void               update_virtual_pad();
    void               probe_tick();
    void               update_menu_screen();
    std::chrono::steady_clock::time_point m_last_probe{};
    uint64_t           m_probe_presents{ 0 };
    std::atomic<bool>  m_menu_screen{ true };
    std::atomic<float> m_menu_distance{ 2.5f };
    // Cinematics (cutscenes, scripted cameras): detected from the game's camera and draws; shown on a
    // flat screen fixed in the room (mode 0), in 3D steadied (mode 1, default since 19 Oct, author: horizon level, heading
    // held and only re-aimed on cuts, position smoothed), or unchanged (mode 2). cmd cinematics, cineforce.
    std::atomic<int>   m_cine_mode{ 1 };
    std::atomic<int>   m_cine_force{ -1 };      // -1 detect, 0 never, 1 always (test, not saved)
    bool               m_cine_on{ false };
    int                m_cine_frames_in{ 0 }, m_cine_frames_out{ 0 };
    int                m_cine_aim_frames{ 0 }, m_cine_noweapon_frames{ 0 }, m_cine_cam_seen{ -1000 }, m_cine_weapon_frames{ 0 };
    float              m_cine_roll{ 0.0f }, m_cine_pitch{ 0.0f }, m_cine_fov{ 0.0f }, m_cine_fov_base{ 0.0f }, m_cine_game_yaw{ 0.0f };
    float              m_cine_yaw{ 0.0f };       // 3D mode: the heading held while it lasts
    glm::vec3          m_cine_cam{ 0.0f };       // 3D mode: smoothed camera position
    int                m_cine_cam_frame{ -10 };
    std::atomic<bool>  m_cine_pair{ true };       // cmd cinepair: 3D cutscene camera moved once per stereo pair (off = per eye frame)
    std::atomic<bool>  m_cine_follow{ true };     // cmd cinefollow: 3D cutscenes look where the scene camera looks (heading and pitch, no roll)
    // cinesteer (default on, saved; author 29 Oct): 3D cutscenes keep
    // the level frame at the smoothed position and only steer its heading: onto the scene camera's at once on a cut, eased toward it once
    // it is cinesteerstart deg away (about 95% in cinesteertime s, at most 90 deg/s), held again within 3 deg. Over cinefollow while on.
    std::atomic<bool>  m_cine_steer{ true };
    std::atomic<float> m_cine_steer_start{ 30.0f };   // cmd cinesteerstart (deg, 10-90)
    std::atomic<float> m_cine_steer_time{ 1.0f };     // cmd cinesteertime (s, 0.3-3)
    bool               m_cine_steering{ false };
    float              m_cine_steer_from{ 0.0f }, m_cine_prev_game_yaw{ 0.0f };
    int                m_cine_steer_logs{ 0 };
    std::chrono::steady_clock::time_point m_cine_steer_t{}, m_cine_steer_t0{};
    std::atomic<bool>  m_postfx_vr{ true };       // cmd vrpost: the game's vignette and film grain off in the headset picture
    uint64_t           m_postfx_seen{ 0 }, m_postfx_changed{ 0 };
    std::unordered_map<ID3D11PixelShader*, int> m_postfx_shaders{};   // the post pass's shaders -> their PostFx cbuffer slot (under m_ps_mtx)
    ID3D11Buffer*      m_postfx_standin{ nullptr };
    ID3D11Buffer*      m_postfx_patch{ nullptr };
    ID3D11Buffer*      m_postfx_saved_cb{ nullptr };
    UINT               m_postfx_saved_first{ 0 }, m_postfx_saved_num{ 0 }, m_postfx_saved_slot{ 0 };
    bool               m_postfx_swapped{ false };
    bool               postfx_swap(ID3D11DeviceContext* ctx);
    void               postfx_restore(ID3D11DeviceContext* ctx);
    // decaleye (default off until judged in the headset): projected decals (blood, scorch marks; PS binding CDecalProjectionProvider) take the eye's view-space
    // position into DecalProjectionMatrix, which the game built with ITS camera's view folded in. For those draws the pixel
    // shader's InvProjectionMatrix becomes (game view <- eye view) x the eye's: the decal lands where it is in the world.
    std::atomic<bool>  m_decal_eye{ true };
    std::unordered_map<ID3D11PixelShader*, int> m_decal_shaders{};   // projected-decal shaders -> their viewport cbuffer slot (under m_ps_mtx)
    ID3D11Buffer*      m_decal_standin{ nullptr };
    ID3D11Buffer*      m_decal_matbuf{ nullptr };
    const void*        m_decal_mat_for{ nullptr };   // the camera set (and frame) the matrix in m_decal_matbuf was built for
    int                m_decal_mat_frame{ -10 };
    ID3D11Buffer*      m_decal_saved_cb{ nullptr };
    UINT               m_decal_saved_first{ 0 }, m_decal_saved_num{ 0 }, m_decal_saved_slot{ 0 };
    bool               m_decal_swapped{ false };
    uint64_t           m_decal_fixes{ 0 }, m_decal_seen{ 0 }, m_decal_refused{ 0 };
    bool               decal_swap(ID3D11DeviceContext* ctx);
    void               decal_restore(ID3D11DeviceContext* ctx);
    std::string        m_cine_why{};
    void               update_cinematic();
    // The F11 menu (Framework custom menu): tabs, Basic / Advanced / Debug level (cmd menulevel, saved),
    // pointer + right-stick scroll / slider nudge with the controllers.
    std::atomic<int>   m_menu_level{ 0 };
    std::atomic<int>   m_menu_tab_request{ -1 };   // cmd menutab N: select tab N next frame
    std::atomic<int>   m_shape_log{ 0 };          // cmd shapelog N: view-shape vectors in shadow / fog buffers
    std::atomic<int>   m_proj_scan{ 0 };          // cmd projscan N: buffers holding the game camera's scale
    std::atomic<bool>  m_eye_scale{ true };       // cmd eyescale: those copies get the eye's scale
    uint64_t           m_eye_scale_fixes{ 0 };
    // cmd tilelight: the lighting pass's pixel -> world-direction matrix (CTileLightCullingParameterProvider
    // InvViewRotProjectionTexture, 128 B) gets the eye's camera.
    std::atomic<bool>  m_eye_tile_light{ true };
    std::atomic<int>   m_shadow_dbg{ 0 }; // cmd shadowdbg: bit 0/1 texture projection with the game's horizontal/vertical off-centre, bit 2/3 corners likewise
    std::atomic<bool>  m_contact_shadows{ false }; // cmd contactshadows: the sun shadow resolve's screen-space contact rays (saved since 7 Nov)
    // Shadow clarity (7 Nov 2026, research\2026-11-07_shadow_clarity.md): live, saved, 1 = the game's.
    std::atomic<float> m_sun_size{ 1.0f };        // cmd sunsize: PCSSLightSize (resolve c19.y) times this: smaller = harder sun shadow edges
    std::atomic<float> m_sun_size_max{ 1.0f };    // cmd sunsizemax: PCSSMaxPenumbraSize (c19.x) times this: the largest blur and blocker search
    std::atomic<float> m_contact_leaf{ 1.0f };    // cmd contactleaf: contact shadows of leaves and grass (c47.y), 0 = none, 1 = the game's
    std::atomic<int>   m_sun_size_logged{ 0 };    // [SunSize] lines left to log (the game's values and ours)
    std::atomic<bool>  m_prev_motion_eye{ true }; // cmd prevmotion: last frame's motion vectors (PrevMotionBuffer) per eye
    std::atomic<bool>  m_ssr_input_eye{ true };   // cmd ssrinput: water reflections trace into the eye's own last picture (not the other eye's)
    // Head look in vehicles through the game's Tobii Extended View (FarCry5Tobii): the drawn/culled camera follows
    // the head by the game's own offset, the mouse is not sent, so the head does not steer (helicopter).
    std::atomic<bool>  m_headlook{ true };        // cmd headlook (saved): while driving / piloting
    std::atomic<bool>  m_headlook_force{ false }; // cmd headlookforce: also on foot (tests)
    std::atomic<bool>  m_aim_pause{ false };      // cmd aimpause: no mouse sends at all (tests)
    std::atomic<int>   m_evlag_log{ 0 };          // cmd evlag N: N log lines matching the camera to the game's Extended View updates
    std::atomic<float> m_ev_sweep_amp{ 0.0f };    // cmd evsweep A P: Extended View yaw swept +-A deg over P frames (with aimpause)
    std::atomic<int>   m_ev_sweep_period{ 90 };
    int64_t            m_mouse_sum_x = 0;         // mouse counts sent by the aim loop so far (x)
    std::atomic<float> m_ev_gain{ 0.7f };         // cmd evgain: share of the remaining error corrected per pair
    std::atomic<float> m_ev_sign_yaw{ -1.0f }, m_ev_sign_pitch{ 1.0f };  // cmd evsign Y P: sim 2026-10-10, offset +20 yaw -> camera -15 (weight 0.75), pitch +10 -> +7.5
    std::atomic<float> m_ev_max_deg{ 150.0f };    // cmd evmax: largest offset handed to the game
    float              m_ev_yaw = 0.0f, m_ev_pitch = 0.0f, m_ev_weight = 0.0f, m_ev_err_deg = 0.0f;
    std::atomic<bool>  m_ev_hold_weight{ true };  // cmd evhold: keep the game's Extended View weight at 0.75 while head look is active
    std::atomic<int>   m_ev_mode{ 1 };            // cmd evmode: 1 feed-forward from the game's applied offset, 0 closed loop on the camera
    std::atomic<float> m_ev_aim_alpha{ 0.5f };    // cmd evaim: smoothing of the game-aim estimate per pair (1 = none)
    float              m_ev_aim_yaw = 0.0f, m_ev_aim_pitch = 0.0f;
    bool               m_ev_aim_valid = false;
    uint64_t           m_ev_updates = 0;
    std::atomic<bool>  m_fire_split{ true };      // cmd firesplit: firing on foot aims the game at the barrel, Extended View keeps the culled camera on the head
    std::atomic<bool>  m_fire_ready{ true };      // cmd fireready (author 20 Oct): with firesplit, the weapon in hand on foot, the game's aim stays on the barrel (ready to fire)
    std::atomic<bool>  m_snap_ready{ true };      // cmd snapready (author 21 Oct): snapsync with the weapon in hand (fireready keeps firesplit on)
    std::atomic<bool>  m_snap_stick{ true };      // cmd snapstick (author 28 Oct): snapsync with the stick aim picked by hand too (the mouse still heard)
    std::atomic<float> m_split_ev_max{ 330.0f };  // cmd splitevmax (author 22 Oct): the firing offset limit, handed deg (the game applies x0.75: 330 = 247 deg; room past 180 so it rarely flips sides)
    bool               m_fire_ready_was = false;
    std::atomic<bool>  m_main_strict{ true };     // cmd mainstrict: the main view size only from views with the player camera's kind of projection
    std::string        m_ui_cmds;                 // commands queued by F11 buttons (run by poll_commands)
    bool               m_split_on = false;        // firesplit offset in use (trigger held, or the aim not yet back on the head)
    std::atomic<float> m_split_slew_deg{ 12.0f };  // cmd firesplitslew: largest aim swing per send while firesplit is on (deg)
    std::atomic<int>   m_split_log_every{ 150 };  // cmd firesplitlog N: a fire split log line every N offset updates
    std::atomic<float> m_split_hold_deg{ 0.0f };   // cmd firesplithold: head moves under this leave the offset alone while firing (deg; 3 tested 09:50: no better)
    std::atomic<float> m_split_pull{ 0.1f };       // cmd firesplitpull: how fast the tracked aim follows camera minus offset per frame
    float              m_split_true_err = 0.0f;    // firesplit log: summed aim error measured with the offset steady
    int                m_split_true_n = 0;
    int                m_split_ev_stable = 0;     // offset updates since the Extended View offset last changed
    uint64_t           m_split_updates = 0;
    float              m_split_aim_yaw = 0.0f, m_split_aim_pitch = 0.0f;   // the game's aim while firesplit is on (radians)
    std::atomic<bool>  m_weapon_ev{ true };       // cmd weaponev: firesplit draws the weapon in the aim's axes (the camera is turned from it)
    // Measured aim (simulator 2026-10-11, evlag/evsweep): the camera uploaded while game Tobii update S runs carries the
    // offset update S-3 stored (the newest one, read before, was 2 updates ahead of it); a mouse send made after the
    // uploads of update S shows in the camera of S+2, an offset handed then in the camera of S+3.
    std::atomic<int>   m_ev_cam_lag{ 3 };         // cmd evcamlag N: the camera carries the offset of N updates back (0 = the newest, as before)
    std::atomic<bool>  m_fire_measure{ true };    // cmd firemeasure: firesplit aims from the measured aim (camera minus its offset), not a tracked sum
    std::atomic<int>   m_split_delay{ 1 };        // cmd firesplitdelay N: mouse sends held N pairs so they reach the camera with their offset
    std::atomic<int>   m_split_mlag{ 2 };         // cmd firesplitmlag N: the camera of update S holds the sends made at updates up to S-N
    int                m_tag_dx[32]{}, m_tag_dy[32]{};   // mouse counts sent, by the Tobii update number they were sent at
    uint64_t           m_tag_k[32]{};
    int                m_split_q_dx[4]{}, m_split_q_dy[4]{};   // firesplitdelay queue (oldest first)
    // cmd firefast: with the trigger down the aim swings onto the barrel in one send (gain 1, not aimgain), and the shot is let
    // through as soon as the send that put the aim within firegate of the barrel has gone out (headset 17-18 Oct logs: the
    // 0.10 deg gate waited for the moving gun, 35-524 ms after the press, median 70-150 ms).
    std::atomic<bool>  m_fire_fast{ true };
    float              m_split_q_res[4]{ 1e9f, 1e9f, 1e9f, 1e9f };   // firefast: aim error left after each queued send (deg; 1e9 = not a firing send)
    float              m_fast_res_sent{ 1e9f };                       // firefast: that error for the send that went out this pair
    int                m_fast_sent_frame{ -1 };   // firefast: the frame the swing send went out (this trigger press)
    std::atomic<int>   m_fire_fast_wait{ 2 };     // cmd firefastwait N: game updates between that send and the shot
    std::atomic<float> m_fire_fast_max{ 0.2f };   // cmd firefastmax F (test, not saved): the fast first shot also waits for the measured aim error under F deg
    // cmd firemoving on|off (saved; F11 Motion control): while the barrel turns, the shot also leaves once the aim is within the barrel's
    // own turn over firemovingk game updates (at most 1 deg): the one-handed pistols never sat within 0.10 deg (author 21 Oct: weapons 18912 and 35187 waited 35-262 ms on every press in his 12:41 log, the rifles often fired at once).
    std::atomic<bool>  m_fire_moving{ true };
    std::atomic<float> m_fire_moving_k{ 1.0f };   // cmd firemovingk K (test, not saved)
    glm::vec3          m_barrel_prev{ 0.0f };
    int                m_barrel_prev_frame{ -1 };
    float              m_barrel_speed{ 0.0f };     // deg per game update, smoothed
    float              m_split_meas_yaw = 0.0f, m_split_meas_pitch = 0.0f;   // last measured aim (radians)
    uint64_t           m_split_meas_seq = 0;
    float              m_lagchk_prev[3]{};        // lag check: aim with the offset of 2, 3, 4 updates back, at the previous update
    uint64_t           m_lagchk_seq = 0;
    float              m_lagchk_sum[3]{};
    int                m_lagchk_n = 0;
    float              m_split_cull_err = 0.0f;   // firesplit log: summed |head - (measured aim + offset it carries)| (camera off the head)
    float              m_split_cull_max = 0.0f;
    float              m_split_true_max = 0.0f;
    std::atomic<float> m_split_predict{ 2.5f };   // cmd firesplitpredict N: the head extrapolated N game updates for the offset (0 = off)
    bool               m_head_pred_valid = false;
    uint64_t           m_head_pred_seq = 0;
    float              m_head_pred_yaw = 0.0f, m_head_pred_pitch = 0.0f, m_head_vel_yaw = 0.0f, m_head_vel_pitch = 0.0f;   // radians per update
    int                m_split_cull_n = 0;
    void record_send(int dx, int dy);
    void inflight(uint64_t seq, int& dx, int& dy) const;
    bool headlook_active() const;
    void prune_captures();
    // Pipeline review 2026-10-10 (research\fc5_pipeline_review_improvements.md), F11 Debug > "Pipeline improvements (review)".
    // All off by default: the build behaves as before until switched on.
    std::atomic<bool>  m_save_deferred{ true };    // R6 savelater: settings written 1 s after the last change (default on since 19 Oct)
    bool               m_save_dirty = false;
    std::chrono::steady_clock::time_point m_save_dirty_at{};
    void               write_settings();
    std::atomic<bool>  m_probe_off{ false };       // P3 probeoff: no once-a-second probe block in the log
    std::atomic<bool>  m_log_buffered{ false };    // P3 logbuffered: log flushed once a second (warnings at once); on by default since 19 Oct through the release defaults (set_log_buffered)
    void               set_log_buffered(bool on);
    std::atomic<bool>  m_snap_reset_fix{ true };   // default on since 17 Oct (author), F11 Debug > Experimental  // R2 snapresetfix: DLSS reset requested when the view jump is applied
    bool               m_reset_on_apply = false;
    uint64_t           m_reset_early[2]{};      // DLSS resets used while a turn was still pending (log)
    std::atomic<bool>  m_skip_cs_any{ false };    // skipcs list not empty (checked before taking its lock)
    void               request_jump_reset() { if (m_snap_reset_fix.load() && m_motion.load() && m_body_valid) m_reset_on_apply = true; else m_dlss_reset_eyes = 3; }
    std::atomic<bool>  m_pair_jumps{ true };   // default on since 17 Oct (author), F11 Debug > Experimental      // R5 pairjumps: recentre / vehicle cuts only between pairs
    void               body_yaw_jump(float yaw);
    std::atomic<bool>  m_veh_per_pair{ true };   // default on since 17 Oct (author), F11 Debug > Experimental    // R4 vehicleperpair: vehicle state sampled once per pair (right eye)
    std::atomic<bool>  m_double_hmd_only{ false }; // R9 doublehmd: same-frame stereo only with the headset on and no menu screen
    std::atomic<bool>  m_ngx_isolate{ false };     // R12 ngxisolate: DLSS runs in its own device context state
    ID3DDeviceContextState* m_ngx_state = nullptr;
    bool               ngx_state(ID3D11DeviceContext* ctx);
    std::atomic<bool>  m_veh_front_fix{ true };   // cmd vehfrontfix: correct a front learned a quarter turn off from the travel direction (fast)
    int                m_veh_front_frames = 0;
    uint64_t           m_veh_front_fixes = 0;
    std::atomic<bool>  m_fog_eye{ true };        // cmd fogeye: volumetric fog view rays with the eye's off-centre part (on: user saw white flashes in far landscape without it)
    std::atomic<bool>  m_fog_zfix{ false };      // cmd fogzfix (old behaviour, test): fogeye also adds the off-centre part to the forward axis (the shaders already add it: double)
    std::atomic<bool>  m_sun_view{ true };       // cmd sunview: the sun and moon disks drawn from each eye's view (off: the game camera's screen position)
    uint64_t           m_sun_view_fixes{ 0 };
    std::atomic<bool>  m_fog_prev{ true };       // cmd fogprev: the fog volume history is reprojected with this eye's previous ViewProj (off: whatever the game camera left)
    uint64_t           m_fog_prev_fixes{ 0 };
    std::atomic<bool>  m_fog_axes{ true };       // cmd fogaxes (default on, author 13 Oct: better): the fog volume built along the eye's axes, not the game camera's
    uint64_t           m_fog_axes_fixes{ 0 };
    std::atomic<bool>  m_sky_view{ false };      // cmd skyview (research, off since 13 Oct: no visible effect on the far haze or the stars): a 448 B buffer's ViewInverse / ProjInverse from the eye
    uint64_t           m_sky_view_fixes{ 0 }, m_sky_proj_fixes{ 0 };
    uint64_t           m_sky_already{ 0 };       // sky buffers whose view rotation already was the eye's
    std::atomic<bool>  m_star_view{ true };      // cmd starview: the star dome drawn from the eye (head roll), not the game camera
    uint64_t           m_star_view_fixes{ 0 };
    // cmd skyeye (Tests 50, default on, not saved): stars and moon/sun disks from the eye whenever their buffer is uploaded
    // (sky_eye_fix); off = starview / sunview. The counters below run either way ([SkyEye] lines, per eye, reset each line).
    std::atomic<bool>  m_sky_eye{ true };       // default on since author 18 Oct 12:54 (stars fixed in the sky)
    bool               sky_eye_fix(float* data, uint32_t size, bool fresh);   // true: fixed (the old fix is then skipped)
    struct SkyEyeStat {
        uint64_t sun_fresh{ 0 }, sun_stale{ 0 }, sun_old{ 0 }, sun_already{ 0 }, sun_fixed{ 0 };
        uint64_t star_fresh{ 0 }, star_stale{ 0 }, star_old{ 0 }, star_noA{ 0 }, star_fixed{ 0 }, star_c13_only{ 0 }, star_late{ 0 };
        double   star_ang_sum{ 0.0 }, star_ang_max{ 0.0 };
    };
    SkyEyeStat         m_sky_eye_stat[2]{};
    int                m_sky_eye_log{ 0 };       // [SkyEye] buffer dumps written (first 6)
    int                m_sky_log{ 0 };           // [SkyView] lines written (first 4)
    std::atomic<bool>  m_lake_eye{ true };       // cmd lakeeye: the lake normal pass (CLakeNormalMapProvider InvViewProjection) gets each eye's matrix
    uint64_t           m_lake_fixes{ 0 };
    std::atomic<bool>  m_lake_data{ false };      // cmd lakedata: the lake surface grid and water lighting (CLakeDataParameterProvider InvViewProjection) get each eye's matrix
    std::atomic<int>   m_lake_log{ 0 };           // cmd lakelog N: the next N lake data buffers (values before/after)
    uint64_t           m_lake_data_fixes{ 0 };
    uint64_t           m_lake_data_skips{ 0 };
    std::atomic<bool>  m_lake_view{ true };       // cmd lakeview: the lake's own view (lake meshes into the 2048x2048 lake buffers) drawn from each eye; includes lakedata
    std::atomic<float> m_lake_overscan{ 1.0f };  // cmd lakeoverscan X (F11 Effects, saved; default 1.0, author 13 Oct): lake buffers cover the view times X (0 / off = the game's 1.625)
    std::atomic<bool>  m_lake_horizon{ true };   // cmd lakehorizon: lake buffers cover the view's full width but only up to the water horizon (sharper shorelines)
    // lakeshared (F11 Debug > Mandatory > Water, saved): both eyes' lake buffers from one view, between the eyes and covering both
    // (lake_shared_note). The ripples are worked out per lake texel (CS #1899FFB3: FBM point-sampled at the texel's world position,
    // normals from neighbouring texels, CS #8685805F), so per-eye lake views gave each eye its own ripples (author 19 Oct, doubled).
    std::atomic<bool>  m_lake_shared{ true };
    float              m_lake_shared_v[16]{};      // the shared view and projection (rows, as MatSet repl[3] / repl[2])
    float              m_lake_shared_p[16]{};
    int                m_lake_shared_frame{ -10 }; // the pair's left-eye frame they belong to
    float              m_lake_rel_v[16]{};         // last pair: right eye view * inverse(left eye view) (rows), right eye projection
    float              m_lake_rel_p[16]{};
    int                m_lake_rel_frame{ -10 };
    uint64_t           m_lake_shared_uses{ 0 };
    // lakepixel (same F11 place, saved): lake data / normal buffers holding the game camera's ViewProjection (the water lighting
    // over the eye's screen) get the eye's own exactly, never the lake view (shared or cropped): reflections the same in both eyes.
    std::atomic<bool>  m_lake_pixel{ true };
    uint64_t           m_lake_pixel_fixes{ 0 };
    void               lake_shared_note(int frame);
    uint64_t           m_lake_view_fixes{ 0 };
    std::atomic<bool>  m_refl_hist{ true };       // cmd reflhist (with lakeview): the reflection resolve's history swapped once per frame per eye (research A/B)
    uint64_t           m_lake_view_stale{ 0 };     // lake view uploads seen before this eye's camera was known (left as they were)
    uint64_t           m_lake_vista_fixes{ 0 };    // far water (CWaterVistaRenderProvider) lake views moved to the eye
    std::atomic<bool>  m_ctp_ident_guard{ true };  // cmd ctpident: an identity game CurToPrev is not matched in unrelated buffers (research A/B)
    uint64_t           m_ctp_ident_skips{ 0 };
    std::atomic<int>   m_fog_trace{ 0 };          // cmd fogtrace N: fog uploads, player views and fog dispatches of the next N frames, in order (research)
    void               fog_trace_dispatch();
    // foglate (with fogeye): fog buffers uploaded before this call's camera, fixed and written back once it is known.
    struct FogLate { ID3D11Buffer* buf{ nullptr }; int frame{ 0 }; bool done{ false }; std::vector<float> data; bool sky{ false }; };   // sky: a star dome copy (skyeye), not fog
    std::vector<FogLate> m_fog_late_list{};
    std::recursive_mutex m_fog_late_mtx;          // m_fog_late_list: scan_matrices also runs on the game's other threads (Unmap / UpdateSubresource of a deferred context)
    std::atomic<bool>  m_fog_late{ true };        // cmd foglate (research A/B, works with fogeye)
    uint64_t           m_fog_late_fixes{ 0 };
    uint64_t           m_fog_late_missed{ 0 };    // a fog pass read the buffer before this call's camera was known
    void               fog_late_check();
    void               fog_late_patch();
    std::atomic<int>   m_vs_probe{ 0 };           // cmd vsprobe HASH N: vertex shader and constant buffers of the next N draws with pixel shader #HASH
    std::string        m_vs_probe_ps{};
    std::atomic<int>   m_view_list{ 0 };          // cmd viewlist N: every camera view of the next N frames (research)
    std::atomic<float> m_fov_margin{ 0.0f };      // cmd fovmargin N: degrees rendered beyond each eye's view on every side (reprojection margin; author 13 Oct: 0)
    std::atomic<float> m_fov_scale{ 1.0f };       // cmd fovnarrow 0.7-1: each eye's view narrowed (tangent scale) at the same pixel count: sharper, less field of view
    std::atomic<bool>  m_gi_planes{ true };     // cmd giplanes: indoor GI volume planes moved from the game camera's view space to the eye's
    std::atomic<bool>  m_submit_render_pose{ true }; // cmd renderpose: layers carry the exact pose each eye was drawn for
    uint64_t           m_upload_stat[2][2][2]{}; // [impostors, shadows][eye parity][fresh, stale]
    std::atomic<float> m_aim_hold_deg{ 1.0f };  // cmd aimhold N: game camera hold zone with the trigger up (0 = always follow)
    bool               m_aim_catching_up{ false };
    uint64_t           m_render_pose_submits{ 0 };
    uint64_t           m_gi_plane_fixes{ 0 };
    std::atomic<float> m_head_sway_amp{ 0.0f }, m_head_sway_period{ 2.0f }; // cmd headsway A P
    uint64_t           m_fog_fixes{ 0 };
    uint64_t           m_tile_light_fixes{ 0 };
    // cmd pxmats: other copies of the camera taken to pixel or texture space (inverse matrices: input side,
    // forward: output side) get the eye's; cmd pxscan N logs where they are.
    std::atomic<bool>  m_eye_px_mats{ true };
    // cmd givolumes: the indoor-lighting volumes' screen tiles (CGIIntersectVolumesHiProvider ThreadToVSHi, 64 B)
    // from the eye's projection.
    std::atomic<bool>  m_eye_gi_volumes{ true };
    uint64_t           m_gi_volume_fixes{ 0 };
    std::atomic<int>   m_px_scan{ 0 };
    uint64_t           m_px_mat_fixes{ 0 };
    std::unordered_set<std::string> m_px_seen{};
    void scan_px_mats(float* data, uint32_t size);
    uint32_t           m_viewport_cb_size{ 864 };
    std::atomic<float> m_dbg_proj_shift{ 0.0f };
    std::atomic<float> m_test_fov[4]{};
    std::atomic<float> m_menu_stick_x{ 0.0f }, m_menu_stick_y{ 0.0f };
    std::atomic<bool>  m_menu_trigger{ false };
    bool               m_menu_registered{ false };
    void               draw_menu(int phase, bool* open, const std::function<void()>& framework_items);
    // Vehicles (option "vehicles", default on): in a vehicle the game has no first-person camera pass.
    // The view frame then turns with the vehicle (its heading from the direction the camera travels),
    // stick turning stops, the throttle is not held for the aim, and cinematic detection pauses.
    std::atomic<bool>  m_vehicle_follow{ true };
    std::atomic<bool>  m_in_vehicle{ false };
    std::atomic<int>   m_vehicle_force{ -1 };   // -1 detect, 0 never, 1 always (test)
    int                m_veh_in_frames{ 0 }, m_veh_out_frames{ 0 };
    std::atomic<int>   m_veh_settle{ 0 };       // 1 just after getting in: the camera turns to the front by itself
    std::chrono::steady_clock::time_point m_veh_settle_t0{};
    int                m_veh_settle_stable{ 0 };
    float              m_veh_settle_yaw{ 0.0f };
    // The vehicle model around the player (a Vehicle_* material drawn with its origin right under the
    // camera): its world axes give the heading; mesh_off turns the model's axis yaw into the heading.
    int                m_veh_mesh_frame{ -100 }, m_veh_mesh_count{ 0 };
    glm::vec3          m_veh_mesh_x{ 1, 0, 0 }, m_veh_mesh_y{ 0, 1, 0 };
    glm::vec3          m_veh_mesh_o{ 0.0f };   // vehaimfront's log: that model's origin (world)
    float              m_veh_mesh_off{ 0.0f };
    bool               m_veh_mesh_off_valid{ false };
    // vehheadingkeep (default on, saved; F11 Comfort > Cinematics and vehicles; author 6 Nov 20:59). Notes in FarCry5VehTransition.cpp.
    std::atomic<bool>  m_veh_heading_keep{ true };
    int                m_veh_head_frame{ -100 };     // the model read for the heading: in the window under the camera, or followed (tracked)
    glm::vec3          m_veh_mesh_rel{ 0.0f };       // that model's origin from the camera (world), at m_veh_head_frame
    float              m_vhk_read{ 0.0f };           // the model's front read on the last update (resync's steadiness test)
    bool               m_vhk_read_valid{ false };
    int                m_vhk_off_n{ 0 };             // updates in a row the model read steady but over 20 deg from the heading
    bool               m_vhk_lost{ false };          // the heading on the travel direction (no model read)
    uint64_t           m_vhk_tracked{ 0 }, m_vhk_resyncs{ 0 }, m_vhk_fix_skipped{ 0 }, m_vhk_noflip{ 0 }, m_vhk_lost_n{ 0 };
    int                m_vhk_aim_off_n{ 0 };         // head look updates in a row the game's aim stood over 30 deg off the vehicle's front
    uint64_t           m_vhk_aim_rearms{ 0 };        // vehaimfront armed again while driving (the game's aim left behind in a slide)
    std::chrono::steady_clock::time_point m_vhk_aim_arm_t{};
    std::atomic<bool>  m_veh_driving{ false };
    void               vehicle_probe(ID3D11DeviceContext* ctx, UINT count);
    float              m_veh_heading{ 0.0f }, m_veh_speed{ 0.0f };
    std::atomic<float> m_veh_speed_pub{ 0.0f };   // drivethrottle auto (ModConfig.cpp): m_veh_speed for the pad thread
    glm::vec3          m_veh_last_pos{ 0.0f };
    bool               m_veh_pos_valid{ false };
    std::chrono::steady_clock::time_point m_veh_last_t{};
    void               update_vehicle();
    std::atomic<bool>  m_wheel_stick{ true };
    std::atomic<bool>  m_wheel_open{ false };
    // wheelmouse off|on|auto (saved, default auto; FarCry5Stereo.cpp wheel_mouse_tick): the wheel's pick pushed through the mouse too.
    // 0 off, 1 on (always), 2 auto (only while the game's prompts show keyboard keys: padglyph::prompt_device).
    std::atomic<int>   m_wheel_mouse{ 2 };
    int                m_wm_held{ 0 };   // auto: presents the mouse was held back because the prompts are the controller's
    std::atomic<float> m_wheel_sx{ 0.0f }, m_wheel_sy{ 0.0f };
    std::atomic<bool>  m_wm_restart{ false };
    int                m_wm_x{ 0 }, m_wm_y{ 0 }, m_wm_sends{ 0 };
    float              m_wm_dx{ 0.0f }, m_wm_dy{ 0.0f };
    bool               m_wm_dir{ false }, m_wm_was{ false };
    void               wheel_mouse_tick();
    std::chrono::steady_clock::time_point m_wheel_until{};   // the aim loop stays quiet until then (the wheel's close)
    const void*        m_cine_wcb[4]{};          // the first-person pass camera buffers seen this frame (counted while flat)
    int                m_cine_wcb_n{ 0 };
    bool               m_menu_screen_on{ false };
public:
    bool flat_screen_on() const { return m_menu_screen_on; }
    // menutabs on|off (saved, default on; F11 Layout > Game menus and the map; author 7 Nov 13:56): on the flat menu screen (the game's
    // menus, the map, loads; not F12's flat play) the right trigger is the game's RB (the tab to the right; the left trigger already is LB, the tab
    // to the left), the left grip LT and the right grip RT (the map's zoom out / in, a menu's pages). Off: right trigger RT, right grip LT.
    // Pad input and button icons only (VR::on_xinput_get_state, padglyphs); play in 3D is unchanged.
    bool menu_tabs() const { return m_menu_tabs.load() && m_menu_screen_on; }
    // popupesc (author 25 Oct):
    // a tutorial pop-up pauses the game and only the keyboard's Esc closes it (his 15:50 and 15:51 pop-ups: pad A, B, X and Y,
    // A held 2 s, all ignored). The pause shows as a game camera that stops answering the aim's mouse sends; while it does, the
    // right B sends Esc instead of pad Y (VR::on_xinput_get_state).
    bool popup_esc_now() const;
    void popup_esc_tap();   // Esc (scan code), held 120 ms
private:
    std::atomic<bool>  m_popup_esc{ true };      // cmd popupesc on|off (saved; F11 HUD and menus "B closes tutorial pop-ups")
    std::atomic<bool>  m_menu_tabs{ true };      // cmd menutabs on|off (saved; F11 Layout > Game menus and the map), see menu_tabs()
    std::atomic<bool>  m_pop_frozen{ false };    // the game camera has not answered kPopSends mouse sends in a row
    std::atomic<int>   m_pop_frame{ -1000 };     // the frame popup_watch last ran (stale: the aim loop is not running)
    int                m_pop_unanswered{ 0 };    // sends with a sideways move the camera did not answer, in a row
    int                m_pop_prev_dx{ 0 };
    float              m_pop_yaw{ 0.0f }, m_pop_pit{ 0.0f };
    glm::vec3          m_pop_cam{ 0.0f };
    // popuphold (author 27 Oct 21:56: the game's prompts turned to keyboard icons; his 21:35 log had ~45 s of aim sends into
    // paused pop-ups, the likely cause): while the camera answers no sends, one probe send every 0.5 s instead of one per pair,
    // and no snap-turn sends.
    std::atomic<bool>  m_pop_hold{ true };       // cmd popuphold on|off (saved; F11 HUD and menus, "Tutorial pop-ups")
    std::chrono::steady_clock::time_point m_pop_probe_at{};
    int                m_pop_held{ 0 };          // aim sends held back since the camera stopped answering
    void               popup_watch(int dx);
    int                m_no_player_frames{ 0 };
    int                m_player_frames{ 0 };   // consecutive frames with a player camera
    // menufastback (author 8 Nov 10:34):
    // back to 3D once the player camera and the first-person pass have been there together for 10 frames.
    std::atomic<bool>  m_menu_fast_back{ true };
    int                m_menu_fp_frames{ 0 };  // consecutive frames with a player camera and the first-person (weapon) pass

public:
    bool virtual_pad_active() const { return m_use_vigem.load() && m_pad != nullptr && !m_pad_failed; }
    bool in_pad_update() const { return m_in_pad_update; }
    int  pad_slot() const { return m_pad_slot; }

private:
    bool               m_in_pad_update{ false };
    // The ViGEm virtual pad: without any pad plugged in the game never polls XInput at all.
    std::atomic<bool>  m_use_vigem{ true };
    std::atomic<int>   m_pad_slot{ -1 };
    uint32_t           m_pad_slot_tries{ 0 };
    void*              m_vigem{ nullptr };   // PVIGEM_CLIENT
    void*              m_pad{ nullptr };     // PVIGEM_TARGET
    bool               m_pad_failed{ false };
    uint64_t           m_pad_updates{ 0 };
    void               fov_scan_step();
    void               fov_hold();
    struct FovScan
    {
        int                 state{ 0 };
        std::vector<float*> cands{}, saved{};
        size_t              lo{ 0 }, hi{ 0 }, mid{ 0 };
        int                 wait{ 0 };
        float               base{ 0.0f };
        float*              found{ nullptr };
        float               scale{ 1.0f };
        float               orig{ 0.0f };
        bool                any{ false };      // vehfov pass C: no near-the-camera filter (seat data)
        bool                watch_only{ false };   // vehfovwatch: stop with the candidates found (nothing is changed)
        size_t              total{ 0 };
        size_t              c0{ 0 }, chunks{ 0 }, dropped{ 0 };   // vehfov: candidates tested 4096 at a time; copies dropped
        float               origh{ 0.0f };     // the game view's width when the scan started (deg)
        float               p00{ 0.0f };       // its projection's P00 (camera constant copies sit next to it)
        std::future<std::pair<std::vector<float*>, size_t>> job{};   // the memory scan runs off the render thread
    };
    FovScan            m_fov_scan{};
    float              m_game_fov_deg{ 0.0f };
    float              m_game_fov_logged{ 0.0f };
    std::atomic<bool>  m_lt_sent{ false };       // the pad's LT (aim down sights on foot) went to the game this poll
    std::atomic<bool>  m_dbg_lt{ false };
    std::atomic<bool>  m_sig_skip_gloves{ true };
    // weaponidsplit on|off (default on, saved): the weapon id is the largest weapon-pass draw's index count, but a big gun's mesh
    // comes in 65535-index pieces: 4 guns shared the id 65535 in the author's 13:43 log (one pose, sight line and scope entry for all).
    // On: such a gun's id is 65536 + its largest other piece (kept while that piece is still drawn). Off: 65535 for all of them.
    std::atomic<bool>  m_weapon_id_split{ true };
    // The draw size of a weapon id's own mesh (a split id, 0x10000 | its next piece, is still drawn as the 65535-index piece).
    static uint32_t    sig_mesh(uint32_t sig) { return (sig & 0x10000u) ? 65535u : sig; }
    UINT               m_weapon_sig2_frame{ 0 };   // the largest weapon-pass draw this frame other than a 65535-index piece
    static constexpr int kSigCounts = 64;
    UINT               m_sig_counts[kSigCounts]{};  // this frame's weapon-pass draw sizes (weaponidsplit keeps an id while its piece is drawn)
    int                m_sig_counts_n{ 0 };
    float              m_hit_hold{ 0.0f };
    std::chrono::steady_clock::time_point m_hit_hold_time{};
    std::atomic<int>   m_hit_hold_ms{ 300 };       // keep the last barrel hit this long after a miss (cmd hithold MS, 0 = off)
    uint64_t           m_hit_held{ 0 };   // weapon id ignores the glove mesh (cmd sigskipgloves)
    std::atomic<bool>  m_support_by_side{ true };   // support (left) hand picked by glove side, not by depth (cmd supportside glove|depth)        // test: LT held (cmd ltest on|off)
    float              m_game_cam_pos[3]{};
    float              m_fov_scan_value{ 0.0f };
    std::atomic<float> m_cull_fov{ 0.0f };   // degrees held in the game's FOV (0 = leave it)
    // vehfov N (default 120, 0 = off; author 18-19 Oct): parked in a vehicle whose game view is narrower, the mod finds the
    // float the game's view angle comes from (memory scan off the render thread, then perturbation) and holds it at N deg
    // while in that vehicle. Passes: A degrees near the camera, B radians near the camera, C degrees anywhere. [VehFov] lines.
    std::atomic<float> m_veh_fov{ 0.0f };     // off by default since the 19 Oct 20:35 crash (it never found the angle)
    bool               m_vf_entry{ false }, m_vf_running{ false }, m_vf_holding{ false }, m_vf_rad{ false };
    int                m_vf_pass{ 0 }, m_vf_stable{ 0 }, m_vf_hold_frames{ 0 };
    float              m_vf_last{ 0.0f };
    std::chrono::steady_clock::time_point m_vf_log_t{};
    void               veh_fov_tick();
    void               fov_scan_abort();
    void               fov_release(bool restore);   // vehfov: stop holding; put the game's own value back
    // vehfovwatch (default on, read-only; author 19 Oct: the parachute draws 65 deg, and changing memory crashed the game):
    // with the view angle steady and narrow in a vehicle, the floats equal to it (near the camera, then anywhere) are put
    // under CPU write watches 4 at a time for a moment each; the log names the game code that writes each one, which
    // points at where the angle comes from. Nothing in the game's memory is changed. Once per vehicle entry.
    std::atomic<bool>  m_vw_on{ false };   // off since the 19 Oct 22:27 crash: watching thread-stack slots (hundreds of writers) stalled and killed the game
    int                m_vw_state{ 0 }, m_vw_pass{ 0 }, m_vw_frames{ 0 }, m_vw_stable{ 0 }, m_vw_round{ 0 };
    bool               m_vw_done{ false };
    float              m_vw_last{ 0.0f };
    size_t             m_vw_idx{ 0 };
    std::vector<float*> m_vw_cands{};
    void               veh_fov_watch_tick();
    // vehplayerfov (default on; author 19 Oct: the parachute is drawn 65 deg wide): settled in a vehicle whose game view is
    // narrower than 110 deg, the game's own SetPlayerFOV is asked for 120 deg (the game uses the angle as the view's width in
    // the headset, on foot and in vehicles: author 20 Oct 09:09, 75 gave a 75 deg plane view and 75 on foot; the simulator's
    // x1.6 on foot did not hold); again on getting out (120 = his on-foot setting). [PlayerFov] lines.
    std::atomic<bool>  m_veh_playerfov{ true };
    // chutelook (default on; author 20 Oct: under the parachute the culling followed the chute, not the head, and jumped):
    // falling on foot (the game camera sinking faster than 3 m/s for ~0.15 s), the head look (the game's Extended View) turns
    // the culled camera onto the head, since the parachute's camera ignores the mouse; and no vehicle is entered meanwhile
    // (the parachute blinked in and out of "vehicle", recentring the view each time). [Fall] lines.
    std::atomic<bool>  m_chute_look{ true };
    // fallvmin m/s (default 6, saved; 0 = as before 25 Oct; author 25 Oct 15:52 log: sprinting down a slope sank the camera
    // 3.6 m/s, was taken for a fall and held as a vehicle for 4.5 s, the view turning with the running player by 140 deg): a
    // fall is held as a vehicle only once it has sunk faster than this (a real fall passes 6 m/s within ~0.7 s, a run down a
    // slope stays near 3-4), or once the game's parachute is seen as a vehicle.
    std::atomic<float> m_fall_vmin{ 6.0f };
    float              m_fall_peak{ 0.0f };   // the fastest sinking (most negative m_fall_vz) since the fall started
    bool               m_fall_slope_said{ false };
    // chutemin (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 09:34). His 09:27 log: three balcony jumps (09:30:30, 09:30:47, 09:31:05) were held as a parachute for 3-7 s while he ran on: the
    // game's "in air" ended (landing) 0.6-1.0 s after the jump, the fall was seen just as he landed (15 updates sinking past 3 m/s), and the
    // hold came 0.5 s later on the old rule, which never looked at the sinking again. Now a fall is held only 0.8 s or more after it was seen
    // with the fall not over (10 updates under 1 m/s), and, when the game says it is in the air (a jump or a free fall, vehscript's poll),
    // once that has lasted 1.5 s (a free fall); an open chute or no answer is held on the timing alone. fallvmin as before.
    std::atomic<bool>  m_chute_min{ true };
    // chuteland (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 12:30). His 12:30 log: on the ground by about 12:30:10, "[Fall] landed" at 12:30:19.85:
    // the old rule wants 90 updates in a row sinking under 1 m/s (3 s at 30 pairs/s), and running down the slope after the landing
    // (the camera sinking 0.9-1.3 m/s) kept restarting it. The game has no parachute getter (research/2026-10-30_vehair_chuteland.md),
    // so the landing stays a camera rule, in time: landed once the camera has not sunk faster than 1.6 m/s for 0.7 s (smoothed over
    // 0.25 s, against the head bob). An open chute sinks about 2 m/s (his 09:10 descent), so it stays held. The old rule still lands too.
    std::atomic<bool>  m_chute_land{ true };
    // chuteopen (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 13:25 play). The game's vehicle answer is type 6 (wingsuit) for the glide AND the chute opened from it (13:30:29-13:31:10: 40 s under
    // the chute, sinking 4 m/s, 5 m/s across), type 2 the paraglider class (never seen: the plain chute of 09:10 was "on foot"); it goes to
    // none on landing (in air false; 12:30:09.9, author) or
    // when the chute is cut (in air true). vehscript dropped 2 and 6 and the rules took the chute opening (the hands' pass back) for getting
    // out: "on foot again", a 2-3 s exit-animation wait with no aim, the eyes 30-90 deg off the culled camera. Now, while the game says 2 or
    // 6: held as a parachute at once (no entry settle, recentre or exit wait, the heading kept, the head look on; chutelook or not); the
    // answer going to none on the ground (in air false, sinking under 3 m/s) = landed at once (12:30 log: 10 s earlier than chuteland),
    // in the air = a fall (chutelook). [Fall] chuteopen lines.
    std::atomic<bool>  m_chute_open{ true };
    // chuteground (default on, saved; author 30 Oct). Today's late landings were jumps with no chute (no type 2 / 6): 09:30:30 held 7.2 s, 09:30:48 3.9 s, 09:31:06 8.7 s,
    // 12:29:32 4.8 s, 13:27:16 6.5 s past the game's "in air" going false, the camera sinking past 1.6 m/s while he ran down the slope.
    // Now a fall the game does not call a wingsuit or parachute ends once the game says on the ground (in air false) for 0.25 s after
    // under 1.5 s in the air (a plain chute, which the game calls on foot, opens after a longer free fall: the camera rules as before),
    // and the next fall waits for the game to say in the air again (no new fall from the run itself).
    std::atomic<bool>  m_chute_ground{ true };
    // chuteprobe (log only, default on for now, saved): while falling, under a wingsuit or parachute and 3 s after, each new draw within
    // 8 m of the camera (pixel shader and index count) is logged once with its offset ("2-8 m above" marked), and a summary at the end:
    // whether the canopy can be told apart by its draw. [ChuteProbe] lines.
    std::atomic<bool>  m_chute_probe{ true };
    bool               m_co_suit{ false };       // chuteopen: the game says type 2 or 6 now
    int                m_co_type{ -1 };
    bool               m_cg_wait_air{ false };   // chuteground: landed by the game's answer: no new fall until it says in the air
    bool               m_cg_ground{ false };     // chuteground: the game says on the ground since m_cg_ground_t0
    float              m_cg_air_run{ 0.0f };     // chuteground: the last run the game said in the air, s (0 after a landing)
    std::chrono::steady_clock::time_point m_cg_ground_t0{};
    struct ChuteDraw { glm::vec3 off{ 0.0f }; int frames{ 0 }, last_frame{ -1 }; float first{ 0.0f }, last{ 0.0f }; std::string names; };
    std::mutex                                     m_cp_mtx;
    std::map<std::pair<void*, UINT>, ChuteDraw>    m_cp_draws;   // chuteprobe: by (pixel shader, index count)
    std::atomic<bool>                              m_cp_on{ false };
    int                                            m_cp_frame{ -1 }, m_cp_logged{ 0 };
    std::chrono::steady_clock::time_point          m_cp_t0{}, m_cp_until{}, m_cp_land_t{};
    void               chute_probe(ID3D11DeviceContext* ctx, UINT count);
    void               chute_probe_tick();
    float              m_land_vz{ 0.0f };      // the camera's vertical speed, smoothed over 0.25 s (chuteland)
    bool               m_land_quiet{ false };  // sinking under 1.6 m/s since m_land_quiet_t0
    std::chrono::steady_clock::time_point m_land_quiet_t0{};
    int                m_vst_said{ 0 };   // vehscript stance: the last stance / GetPawnVehicle agreement logged (update_vehicle)
    bool               m_fall_held{ false }, m_fall_old_would{ false }, m_fall_air_on{ false }, m_fall_ended{ false };
    float              m_fall_z0{ 0.0f }, m_fall_zmin{ 0.0f }, m_fall_air_max{ 0.0f }, m_fall_sink_s{ 0.0f };
    const char*        m_fall_why{ "" };
    std::chrono::steady_clock::time_point m_fall_sink_t0{}, m_fall_air_t0{}, m_last_air_t{};
    bool               m_falling{ false }, m_fall_valid{ false };
    bool               m_chute_vehicle{ false };   // the vehicle state is a fall (held until landed)
    float              m_fall_vz{ 0.0f };
    int                m_fall_frames{ 0 }, m_land_frames{ 0 };
    glm::vec3          m_fall_pos{ 0.0f };
    std::chrono::steady_clock::time_point m_fall_t{}, m_fall_t0{};   // m_fall_t0: when the fall started (chutelook)
    bool               m_vp_in{ false }, m_vp_asked{ false };
    std::chrono::steady_clock::time_point m_vp_until{}, m_vp_t_ask{};   // fovearly: check often until; last ask
    int                m_vp_tries{ 0 }, m_vp_frames{ 0 }, m_vp_out_frames{ -1 };   // m_vp_out_frames: fovexit, updates since getting out (-1 = done)
    void               veh_playerfov_tick();

public:
    // Motion controls (right stick turning comes from the pad hook in ModConfig.cpp).
    bool motion_enabled() const { return m_motion.load(); }
    bool in_vehicle() const { return m_veh_driving.load(); }   // driving: the pad's triggers are throttle / brake
    bool in_any_vehicle() const { return m_in_vehicle.load(); }   // drivemenu (ModConfig.cpp): in a vehicle, any seat (driving or riding)
    // driveready (ModConfig.cpp): a weapon in the game's hands now (the weapon pass drew one within the last 3 frames).
    bool game_gun_out() const { return m_frame.load() - m_gun_pass_frame.load() <= 3; }
    float veh_speed_now() const { return m_veh_speed_pub.load(); }   // drivethrottle auto (ModConfig.cpp): the vehicle's speed, m/s
    // Weapon wheel (cmd wheelstick, F11 Controls): while the pad's LB is held on foot the wheel is open; the real right stick
    // goes to the game to pick a slot, no stick turning, and the aim loop sends no mouse moves (they moved the selection).
    bool wheel_stick() const { return m_wheel_stick.load(); }
    bool wheel_open() const { return m_wheel_open.load(); }
    void note_wheel(bool lb_on_foot);
    // wheelmouse (ModConfig.cpp): the right stick the game's weapon wheel is given this poll (-1..1, up positive).
    void note_wheel_stick(float x, float y) { m_wheel_sx = x; m_wheel_sy = y; }
    bool wheel_test(float& x, float& y) const { x = m_wheel_test_x.load(); y = m_wheel_test_y.load(); return m_wheel_test.load(); }
    std::atomic<bool>  m_wheel_test{ false };   // cmd wheeltest X Y | off
    std::atomic<float> m_wheel_test_x{ 0.0f }, m_wheel_test_y{ 0.0f };
    // LT from the right grip: in vehicles and menus only (on foot the grip holds the gun; gripads was removed 24 Oct).
    bool grip_sends_lt() const { return in_vehicle() || flat_screen_on(); }
    // vehtriggers (FarCry5VehTransition.cpp): driving, the right trigger shoots (LB), the left trigger is the throttle (RT) and the left grip
    // the brake (LT); the right grip sends nothing.
    bool veh_triggers_swapped() const { return m_veh_triggers.load() && in_vehicle() && !flat_screen_on(); }
    void note_veh_triggers(bool swapped, bool right_held, bool left_held);
    std::atomic<int> m_vt_shots{ 0 };   // right-trigger presses sent as LB since the layout last changed (log)
    bool debug_lt() const { return m_dbg_lt.load(); }
    void note_lt(bool sent);
    void note_grip(bool down) { m_grip_held = down; }
    void note_rb(bool down) { m_rb_down = down; }   // the pad RB the game gets (throwable: any throw records the throwable)
    std::atomic<bool> m_rb_down{ false };
    // Body gestures (FarCry5Gesture.h), once per pad poll: the grips by hand role (weapon hand, other hand).
    fc5::gesture::Output gesture_poll(bool weapon_grip, bool other_grip, bool b_down = false, bool b_mod = false);   // b_*: reloadmanual (right B, left grip raw)
    // drive (FarCry5Drive.h; author 5 Nov 17:06), once per pad poll: the raw grips of the right and left controllers.
    fc5::drive::Output   drive_poll(bool right_grip, bool left_grip);
    // dpadear (FarCry5Stereo.cpp, by drive_poll): this press of the physical left grip started with the left controller at the left ear / eye
    // (decided at the press, kept until it is let go). Both pad pollers call it; it buzzes nothing (ModConfig does).
    bool                 left_grip_at_ear(bool left_grip_raw);
    bool                 dpad_ear_on() const { return m_dpad_ear.load(); }
    // throwable: the throw's direction in the world while one is under way (FarCry5Gesture); the aim loop's verdict for it.
    bool                 throw_dir_world(glm::vec3& dir);
    std::atomic<bool>    m_throw_aimed{ false };
    // throwable (FarCry5Throwable.cpp): the game draws its throwable (a rigid weapon-pass mesh) only while the throw button
    // is held. That draw is moved from the game's left hand to the left controller (throwhand), and recorded so it can be
    // drawn again on the chest between throws (throwchest).
    // sidearm (FarCry5Holster.cpp): each weapon the game holds is recorded (its weapon-pass draws, in the gun's steadied axes) so the mod
    // can draw it where it is carried while the game holds the other one: the main weapon on the sling, the sidearm on the hip, and the
    // wanted one in the hand while a switch is pending.
    struct HolsterState;
    HolsterState*        m_hol{ nullptr };
    void                 holster_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT instances);   // a kept non-glove weapon-pass draw
    void                 holster_show(ID3D11DeviceContext* ctx);      // at the gloves draw, before it: the copies
    bool                 holster_model_build(ID3D11DeviceContext* ctx, uint32_t sig);   // holstermodel: the holster molded on that sidearm
    void                 holster_model_draw(ID3D11DeviceContext* ctx, ID3D11Buffer* const* vcb);   // holstermodel: on the hip (vcb: the gloves draw's VS b0-b4)
    void                 holster_frame_end();                         // the frame's weapon id known: commit a recording, the kinds held
    bool                 holster_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    bool                 holster_is_side(uint32_t sig);               // a sidearm: by hand (sidearmweapons), the game's data (sidearmgame) or measured (gun_still_handgun)
    int                  holster_game_side(uint32_t sig);             // FarCry5Holster.cpp sidearmgame: 1 / 0 from the game's weapon data, -1 not known
    int                  holster_handgun(uint32_t sig);               // the game's answer when known, else gun_still_handgun
    bool                 holster_game_on() const;                     // sidearmgame on: the game's equipped item is asked (FarCry5Lua equipped_query)
    std::atomic<bool>    m_hol_cur_side{ false };                     // the weapon in the game's hands now is a sidearm (for the pad thread)
    std::atomic<bool>    m_hol_hiding{ false };                       // weapon_switch_hiding() at the last frame end (for the pad thread)
    std::atomic<bool>    m_hol_known{ false };                        // the weapon in the game's hands has been recorded (for the pad thread)
    std::atomic<bool>    m_hol_side_rec{ false };                     // the sidearm slot's weapon has been recorded (for the pad thread: sidearmpeek)
    std::atomic<bool>    m_hol_side_known{ false };                   // the sidearm slot's weapon is known (sidearmsig set; for the pad thread)
    std::atomic<uint32_t> m_hol_main{ 0 };                            // the main weapon last held still (0: none; loadslot, the pad thread)
    std::atomic<uint32_t> m_hol_side_sig{ 0 };                        // that weapon (for the pad thread: sidearmy)
    std::atomic<bool>    m_hol_side_unlisted{ false };                // the saved sidearm list has weapons, the one in hand is not one (for the pad thread)
    std::atomic<bool>    m_hol_copy_hand{ true };                     // holsterhand on|off (test): the copy in the empty hand placed by where that hand holds it
    std::atomic<bool>    m_hol_glove{ true };                         // holsterglove on|off (test): the copy in the hand held by the hand recorded with it
    bool                 holster_hand_pose(uint32_t& sig, glm::vec3& pos, glm::mat3& rot, bool& empty_hand);   // the copy in the hand this frame
    void                 holster_glove_note(const void* mesh, UINT count, UINT start, INT base, const glm::mat4& Om, const std::vector<float>& bones,
                                            const std::vector<glm::vec4>& cent, int wside);
    bool                 holster_glove(const void* mesh, UINT count, UINT start, INT base, const glm::mat4& Om, std::vector<float>& bones);   // the weapon hand's bones holding the copy
    void                 holster_keep_hands(uint32_t sig);            // holsterglove: the recorded hands kept (fc5vr_holsterhands.txt)
    bool                 holster_has_saved_hand(uint32_t sig) const;
    bool                 holster_switch_hiding() const;               // the game's weapon is hidden for an instant switch (shoulder grab thread)
    bool                 holster_hides_game() const;                  // a sidearm switch pending: the game's weapon-pass draws (its switch animation) hidden
    // slingthrowhide on|off (FarCry5Holster.cpp; new, saved, default on; F11 "Hands and weapon" under slingmv; author 6 Nov video throwable_sling.mp4:
    //): a throwable taken while the game's weapon is carried (on the sling or the hip) makes the game put that weapon away and bring it
    // back after the throw; the mod draws the weapon pass at the sling, so both animations played there. From the grab until the weapon is back
    // and still (the same weapon id for 1 s after the hold, at most 3 s), the game's weapon-pass draws are hidden and the recorded copies stand
    // where the weapons are carried (as during the hold since 29 Oct). Off: the game's animations shown, as before 6 Nov.
    std::atomic<bool>    m_sling_throw_hide{ true };
    bool                 holster_throw_cover() const;                 // slingthrowhide now: the game's carried weapon hidden, the copies drawn
    bool                 holster_throw_hides(UINT count, UINT start, INT base);   // slingthrowhide: this non-glove weapon-pass draw is not drawn (counted)
    bool                 throwable_idle_part(UINT count, UINT start, INT base) const;   // FarCry5Throwable.cpp: a draw of the last frame before the throw (the gun)
    bool                 holster_has_copy(uint32_t sig);              // that weapon recorded (shoulderinstant can draw it)
    bool                 holster_records() const;                     // weapons are recorded (the sidearm, or shoulderinstant)
    // weaponcache (FarCry5Holster.cpp): each weapon's copy kept on disk (bin\fc5vr_weaponcopies), read at the next start so the sling and the hip
    // show it at once; the last main weapon and sidearm known from the start, checked against the game's inventory.
    void                 holster_cache_frame();                       // once per frame at holster_frame_end: read, make, check, save
    int                  holster_cache_make(uint32_t sig, const std::vector<uint8_t>& file, std::string& why);   // 1 made, 0 not yet (retry), -1 never
    bool                 holster_cache_save_begin(uint32_t sig);      // a save of that recorded weapon started (false: nothing to save, or not keepable)
    void                 holster_cache_save_step();                   // the save under way, one step per frame
    void                 draw_holster_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    int                  gun_still_handgun(uint32_t sig) const;       // FarCry5GunStill.cpp: 1 handgun, 0 not, -1 not measured
    struct ThrowableState;
    ThrowableState*      m_th{ nullptr };
    bool                 throwable_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, bool record_only = false);   // a non-glove weapon-pass draw (record_only: an arm draw, hidden); true: not to be drawn
    void                 throwable_chest(ID3D11DeviceContext* ctx);   // at the gloves draw, before it
    void                 throwfile_note(ID3D11DeviceContext* ctx);    // throwfile: a weapon-pass draw whose shaders the file's models can be drawn with
    void                 throwfile_step(ID3D11DeviceContext* ctx, uint64_t sel);   // throwfile: the selected throwable with no copy: read from the game's files, then built
    void                 throwable_pouch(ID3D11DeviceContext* ctx, int host = 0, UINT count = 0, UINT start = 0, INT base = 0);   // the pouch marking the grab spot, in the hands' (0) or a clothing (1) draw
    void                 throwable_pouch_cloth(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);   // throwpouchmat cloth: a hidden first-person draw, the pouch's host if it is cloth
    void                 throwable_restore(ID3D11DeviceContext* ctx);
    bool                 throwable_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    void                 throwable_note_support(const glm::mat4& A, const glm::vec3& F);   // the gloves draw's left-hand move
    bool                 m_th_cb_swapped{ false };    // a throwable draw's object constants swapped (restored with the hands')
    // bowarrowmodel (FarCry5BowArrow.cpp): manualbow's arrow is the game's own (learnt at a shot, hidden until taken, moved to the string hand).
    struct BowArrowState;
    BowArrowState*       m_ba{ nullptr };
    bool                 bow_arrow_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);   // a non-glove weapon-pass draw, the bow in hand; true: not to be drawn
    void                 bow_arrow_frame(uint32_t sig);
    void                 bow_sling_fix(uint32_t sig);   // bowsling + bowuprightkeep: m_bow_fix for the bow on the sling (the kept upright turn until measured in the hand)
    void                 bow_arrow_restore(ID3D11DeviceContext* ctx);
    // bowpull: the bow's moving parts posed between the game's rest and full draw (-1 not concerned, 0 drawn, 1 not drawn).
    int                  bow_pull_draw(ID3D11DeviceContext* ctx, UINT count, bool skinned, bool part, uint32_t obj_size, bool fresh, bool again, const float* obj, ID3D11Buffer* const* cb);
    bool                 m_ba_cb_swapped{ false };    // the arrow's object constants swapped (restored with the hands')
    glm::mat3            m_bow_fix{ 1.0f };           // bowupright (FarCry5BowArrow.cpp): the bow's turn in the hand, steadied q, for weapon m_bow_fix_sig
    uint32_t             m_bow_fix_sig{ 0 };
    // bowpin: the bow's sight pin (its reticle draw's middle) in the game camera's axes (q), and the frame it was drawn in.
    glm::vec3            m_bow_pin_q{ 0.0f };
    int                  m_bow_pin_frame{ -100 };
    glm::vec3            m_bow_pin_w{ 0.0f };          // the same point where this frame's bow is drawn (world), for reddot_draw
    // bowpinsteady (FarCry5Bow.h): the pin in the bow controller's axes (ctrl_rot_world), eased; this frame's head from the game camera and the
    // aiming eye's offset from it (taken where both come from one frame), for bowsightaim's line from this frame's eye and bow.
    glm::vec3            m_bow_pin_l{ 0.0f };
    bool                 m_bow_pin_l_ok{ false };
    uint32_t             m_bow_pin_l_sig{ 0 };
    std::chrono::steady_clock::time_point m_bow_pin_l_t{};
    glm::vec3            m_bow_head_rel{ 0.0f };       // head - game camera, world axes, update_motion of frame m_bow_head_rel_frame
    int                  m_bow_head_rel_frame{ -100 };
    glm::vec3            m_bow_eye_off[2]{};           // each eye's camera - (base camera + head_rel) of the same frame (0 left, 1 right)
    int                  m_bow_eye_off_frame[2]{ -100, -100 };
    int                  m_bow_pinsteady_said{ 0 };    // log lines given (a few per session)
    bool                 m_bow_slung_now{ false };     // bowsling: the manual bow hangs on the sling this frame (update_motion)
    glm::vec3            m_bow_sling_hand{ 0.0f };     // bowsling: the game's left hand on the bow (q), last measured, for weapon m_bow_sling_hand_sig
    uint32_t             m_bow_sling_hand_sig{ 0 };
    UINT                 m_bow_pin_count{ 0 };         // the reticle draw it is taken from: the smallest seen lately (the pin's glow)
    int                  m_bow_pin_count_frame{ -100 };
    uint64_t             m_bow_pin_tally[6]{};         // reddot_draw, last 5 s: dots in the left, right eye; none: no sight draw, hand placement old, off the picture, other
    std::chrono::steady_clock::time_point m_bow_pin_said{};
    void                 bow_pin_tally(int slot, int frame);
    void                 bow_pin_note(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    // reloadmagmodel (FarCry5ReloadMag.cpp): reloadmanual's magazine is the game's own (learnt at its first reload, hidden while out, moved to the
    // left hand, sliding into the well).
    struct ReloadMagState;
    ReloadMagState*      m_rm{ nullptr };
    bool                 reload_mag_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);   // a non-glove weapon-pass draw, a listed weapon in hand; true: not to be drawn
    // bonemap (FarCry5BoneMap.cpp; debug, off by default): each first-person bone named and drawn over the eye picture.
    struct BoneMapState;
    BoneMapState*        m_bm{ nullptr };
    void                 bonemap_note(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, int how);   // a weapon-pass draw (how: 0 drawn, 1 hidden arms, 2 indirect)
    void                 bonemap_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);                        // after the overlays
    const void*          bonemap_scan(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, bool skinned, float scale);    // a draw's vertices read once (FarCry5BoneMap.cpp)
    int                  bonemap_identify(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, bool indirect, int& alts);  // the bound draw's kDraws index (armik)
    // armik, armcensus (FarCry5ArmIkDraw.cpp; research\2026-11-08_fullbody_ik.md): the arms drawn from the shoulder to the hand the mod places.
    struct ArmIkState;
    ArmIkState*          m_ak{ nullptr };
    // how: 0 a hand draw that goes ahead (gloves, bare hands), 1 an arm or sleeve draw the mod hides, 2 clothing glovemesh hides.
    // true: its palette was moved (1, 2: draw it instead of hiding it).
    bool                 armik_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, int how);
    bool                 hands_cb_update(ID3D11DeviceContext* ctx, const std::vector<float>& bones);   // the stand-in palette rewritten, or swapped in
    // wristammo (FarCry5WristAmmo.cpp; author 5 Nov): the rounds in the gun / what it holds / what is left, under the weapon hand's wrist.
    struct WristAmmoState;
    WristAmmoState*      m_wa{ nullptr };
    void                 wrist_ammo_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);                     // after the overlays
    bool                 wrist_ammo_smooth(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, float cx, float cy, float pu, const std::string& a,
                                           const std::string& b, const std::string& t, int first, bool none_left);   // wristammostyle smooth
    std::vector<float>   m_bm_pal;              // the palette hands_cb_swap put in m_hand_standin (bonemap or armik on)
    int                  m_bm_pal_frame{ -1 };
    void                 reload_mag_frame(uint32_t sig);
    void                 reload_hand_snap(glm::mat3& rot, glm::vec3& pos);   // reloadhandsnap: the free controller's pose for drawing its hand (drawn world)
    void                 reload_mag_restore(ID3D11DeviceContext* ctx);
    // reloadhintstyle glow (FarCry5ReloadHint.cpp): the part to work next drawn into the hint mask right after the game's draw of it.
    void                 reload_hint_after(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    bool                 m_hint_queued{ false };      // reload_mag_draw queued hint mask draws for the draw it looked at
    bool                 m_rm_cb_swapped{ false };    // a rigid magazine part's object constants swapped (restored with the hands')
    // throwv2 (FarCry5Gesture): the other controller's pointing direction in the recentred reference (motion update -> gesture_poll),
    // whether a throwable's model is recorded (its peek), the recorded copy drawn in the holding hand.
    std::atomic<float>   m_other_fwd_ref[3]{ 0.0f, 0.0f, -1.0f };
    std::atomic<bool>    m_throw_split{ true };   // throwsplit on|off (saved): see the aim loop
    bool                 throwable_recorded() const;
    void                 draw_throw_v2_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                            const std::function<void(const char*)>& section);
    void                 draw_throw_size_menu(const std::function<void(const char*)>& hint);   // throwhandscale, on the F11 throwable section itself
    void                 throwable_hold_pose(glm::vec3& pos, glm::mat3& rot) const;
    void                 hud_tex_probe(ID3D11DeviceContext* ctx);   // hudtexprobe (test)
    void                 throw_all_probe(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT inst);   // throwallprobe (test)   // the holding hand's controller (world); left unchanged for the left hand
    bool                 m_hand_bones_swapped{ false };
    std::atomic<int> m_plane_scan{ 0 };
    std::atomic<bool>  m_aim_once{ true };
    std::atomic<bool>  m_mv_eye{ true };          // F1: temporal passes get the eye's CurToPrev at dispatch (cmd mveye)
    ID3D11Buffer*      m_mv_standin{ nullptr };
    ID3D11Buffer*      m_mv_matbuf{ nullptr };
    struct MvSaved { ID3D11Buffer* cb{}; UINT first{}, num{}, slot{}; } m_mv_saved{};
    uint64_t           m_mv_swaps{ 0 };
    bool mv_swap(ID3D11DeviceContext* ctx);
    void mv_restore(ID3D11DeviceContext* ctx);        // F3: one aim correction per stereo pair (cmd aimonce)
    // aoexact (default on since author 14 Oct): the AO's temporal pass (SSBC #E45A7ABE) replaced by the mod's copy of it that reprojects each pixel's
    // history with this eye's exact camera move and tests it against the depth that point had in the previous frame. The game's
    // pass takes the game's 8-bit motion vectors (no head move unless ctptrans) and compares with the current depth: indoors, any
    // head or body move put the history beside its pixel at corners and edges, it failed the depth test, and the AO started over.
    std::atomic<bool>    m_ao_exact{ true };
    ID3D11ComputeShader* m_ssbc_temporal_cs{ nullptr };   // the game's #E45A7ABE
    ID3D11ComputeShader* m_ao_exact_cs{ nullptr };
    ID3D11Buffer*        m_ao_exact_cb{ nullptr };
    bool                 m_ao_exact_failed{ false };
    const void*          m_ao_cb_for{ nullptr };
    int                  m_ao_cb_frame{ -10 };
    uint64_t             m_ao_exact_runs{ 0 }, m_ao_exact_refused{ 0 };
    bool ao_exact_dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z);   // true: dispatched (the game's pass is not run)
    // lightstat: lights per uploaded light list, [LightsLightingData, Lights][eye], reset by the per-second log.
    struct LightStat { uint32_t lists{ 0 }, min{ UINT32_MAX }, max{ 0 }, last{ UINT32_MAX }, drops{ 0 }; };
    LightStat            m_light_stat[2][2]{};
    std::atomic<float> m_aim_hold_exit{ 0.3f };   // F3: hold-zone exit (deg) once following (cmd aimholdexit)
    std::atomic<bool>  m_pair_body{ true };       // F4: body yaw changes only between pairs (cmd pairbody)
    float              m_body_yaw_pending{ 0.0f };
    uint64_t           m_aim_sends{ 0 };
    std::atomic<bool> m_cull_heads{ true };     // GPU culling planes around the head and both eyes (cmd cullheads; default on since 20 Oct, author)
    // cinecull (default on): the same while a cutscene is shown in 3D, whatever cullheads says. Cutscene cameras are 50-80 deg
    // wide while each eye sees about 54 deg to its outer side: the terrain past the camera's edge was left out (benchmark 13 Oct).
    std::atomic<bool> m_cine_cull{ true };
    bool              cull_heads_now() const { return m_cull_heads.load() || (m_cine_cull.load() && m_cine_on && m_cine_mode.load() == 1 && !m_menu_screen_on) ||
                                                      (m_veh_cull_heads.load() && m_in_vehicle.load() && !m_menu_screen_on); }
    // Vehicles (author 14 Oct): vehcullheads (default on): the GPU culling planes around the head in every vehicle (the game culls
    // with ~86 deg there); vehfade (default on): black while getting in / out until the culled camera is back on the head;
    // vehentryfollow (default on): during the entry / exit animation the view follows the game camera; vehstick (default on):
    // the right stick goes to the game while piloting with head look (turns a helicopter).
    std::atomic<bool> m_veh_cull_heads{ true };
    std::atomic<bool> m_veh_fade{ true };
    bool              m_veh_settle_entry{ false };   // the running settle is getting in (not a seat change)
    std::atomic<bool> m_veh_entry_follow{ true };
    std::atomic<bool> m_veh_stick{ true };
    // helipitch (default on, saved; author 30 Oct): piloting
    // a helicopter the right stick's up / down goes to the game as its own right stick Y (its look / pitch), pushed forward = nose
    // down. Off: never sent (as before).
    std::atomic<bool> m_heli_pitch{ true };
    // stickspeed (default on, saved; author 5 Nov 18:19): driving a car, a
    // quad, a motorcycle or a boat, the right stick's up is the throttle (RT) and down the brake / reverse (LT), with the triggers.
    std::atomic<bool> m_stick_speed{ true };
    // stickview (default on, saved; author 30 Oct 12:35): in a vehicle the right stick reaches the game only in a
    // helicopter or a plane (vehstick, helipitch); planelook's seat starts as the model's axes (y ahead, z up), so a flying vehicle's
    // view hangs on its body from take-off, and the seat is not learnt while the right stick turns the game's camera ahead of the
    // aircraft. Off: as before.
    std::atomic<bool>     m_stick_view{ true };
    std::atomic<uint64_t> m_veh_rstick_t{ 0 };   // GetTickCount64 of the last right stick sent to the game in a vehicle
    void note_veh_rstick() { m_veh_rstick_t = GetTickCount64(); }
    bool stick_view() const { return m_stick_view.load(); }
    // Getting in / out comfort (FarCry5VehTransition.cpp, author 23 Oct): vehfollowhead (default on, saved): the follow turns the view by the animation's turn
    // only, the head still turns it (before: the view's heading held on the game camera's, the head's yaw not seen for 1-3 s);
    // vehfollowsnap (default on, saved): that turn in snaps of the snap-turn angle instead of a smooth turn; vehseatguard (default
    // on, saved): getting out first reads as a seat change, that settle (a cut and a recentre) waits while no vehicle is under you.
    std::atomic<bool> m_veh_follow_head{ true };
    std::atomic<bool> m_veh_follow_snap{ true };
    std::atomic<bool> m_veh_seat_guard{ true };
    // vehaimfront (default on, saved; 5 Nov gesture driving run: getting in while the game's own camera looked far from the vehicle's
    // front, the settle cut faced the front but the game's aim stayed 79 / 128 deg off it, and the quad's bars were drawn ~20 deg off):
    // when a settle ends with its cut, the game's aim is turned onto the vehicle's front by the mouse (veh_aim_front_step, in the head
    // look's update), as after a normal get-in. Notes in FarCry5VehTransition.cpp, the step in FarCry5Stereo.cpp (send_mouse).
    std::atomic<bool> m_veh_aim_front{ true };
    std::atomic<float> m_veh_aim_front_min{ 5.0f };  // vehaimfrontmin N (test): an aim within N deg of the front is left as it is
    int               m_vaf_state{ 0 };            // 0 idle, 1 armed by a settle, 2 turning
    std::chrono::steady_clock::time_point m_vaf_t0{};
    uint64_t          m_vaf_wait_until{ 0 };       // m_ev_updates before the next look (the last send on its way)
    float             m_vaf_scale{ 0.05f };        // deg per count of the vehicle's camera (the on-foot scale, then measured)
    float             m_vaf_sent_deg{ 0.0f };      // the turn the last send asked for
    float             m_vaf_aim_at_send{ 0.0f };   // the aim's yaw at that send
    float             m_vaf_start_deg{ 0.0f };     // the aim off the front when it started
    float             m_vaf_ev_start{ 0.0f };      // the head look's yaw offset when it started
    int               m_vaf_sends{ 0 }, m_vaf_still{ 0 };
    glm::vec3         m_vaf_cam0{ 0.0f };          // the camera in the vehicle model's axes when it started
    bool              m_vaf_cam0_ok{ false };
    void              veh_aim_front_arm(std::chrono::steady_clock::time_point now, const char* why);
    void              veh_aim_front_step(float yaw_c);
    bool              veh_cam_in_axes(glm::vec3& out) const;
    bool              m_vf_init{ false };        // the running follow's start is taken (reset when a settle starts)
    float             m_vf_body0{ 0.0f };        // the body yaw at the follow's start
    float             m_vf_cam_last{ 0.0f };     // the game camera's yaw at the last step
    float             m_vf_turn{ 0.0f };         // the game camera's turn since the start (unwrapped)
    int               m_vf_snaps{ 0 };           // snaps in the running follow
    std::chrono::steady_clock::time_point m_vf_snap_t{};
    int               m_vf_seat_held{ 0 };       // frames the running seat-change settle was held (vehseatguard)
    // vehseathold (default on, saved; author 28 Oct 22:17): a boat's model is 3 m from the
    // driver's camera, outside vehicle_probe's window, so the hands' pass alone said in or out; the game draws the sidearm in the
    // boat every few seconds and the mod went on foot each time (the trigger held for the aim, the sling cut it). Now the seat is
    // learnt on the vehicle's model (the Vehicle_ draw within 8 m, the camera in its axes) when a settle ends; while the camera
    // stays within vehseatholdm of it the player is in that vehicle, and a driver who draws a weapon is still driving.
    struct VehSeatRec { int frame{ -100 }; UINT count{ 0 }; glm::vec3 cam{ 0.0f }; float dist{ 0.0f }; };
    std::atomic<bool>  m_veh_seat_hold{ true };
    std::atomic<float> m_veh_seat_hold_m{ 0.6f };
    // vehtriggers (default left, saved; author 29 Oct 10:19): driving, the right trigger is the game's vehicle
    // fire (LB), the left trigger the throttle (RT) and the left grip the brake (LT); the right grip is free. right = as before.
    std::atomic<bool>  m_veh_triggers{ true };
    // vehdrivegun (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 09:11):
    // his right trigger during the quad's entry was sent as LB (the driver's fire), the game drew the sidearm, the weapon view came back and
    // update_vehicle took it for a passenger seat (the left trigger back to LB for 6 s). A weapon view that shows while driving within 1.5 s
    // of that LB (or while it is held, or after one sent during this vehicle's entry) is the driver's sidearm: latched until the vehicle is
    // left, still driving meanwhile. First, though, the game's own answer (vehscript; seat from 1, driver's seat from 0, seat 0 = getting in):
    // its driver drives (drive and shoot), its passenger rides; the LB latch decides only without it (research\2026-10-30_vehdrivegun_downview.md).
    std::atomic<bool>    m_veh_drive_gun{ true };
    std::atomic<int64_t> m_vt_lb_t{ 0 };        // steady_clock ticks of the last pad poll sending LB from the right trigger while driving (ModConfig)
    std::atomic<bool>    m_vt_lb_held{ false };
    bool                 m_vdg_latched{ false }, m_vdg_said{ false };
    int                  m_vdg_frames{ 0 };     // frames a weapon view left the driver driving through the latch (log)
    const char*          m_vdg_rule{ nullptr };   // the rule that decided driving / riding last (log on change)
    std::string          game_seat_text() const;   // vehdrivegun: the game's seat, driver's seat and vehicle entity (log only)
    VehSeatRec         m_vsh_rec[4]{};          // by frame: the camera in the axes of the Vehicle_ draw nearest the seat (or the largest)
    glm::vec3          m_vsh_seat{ 0.0f };      // the learnt seat (the camera in the model's axes)
    UINT               m_vsh_count{ 0 };        // vehwheels (FarCry5VehHands.cpp): the learnt seat's vehicle model (its body draw's index count; 0 none)
    int                m_vsh_learn_frame{ -1000 };   // and the frame it was learnt
    bool               m_vsh_valid{ false }, m_vsh_driving{ false }, m_vsh_near{ false }, m_vsh_kept_said{ false };
    std::atomic<float> m_vsh_side{ 99.0f };     // drivequadseat (FarCry5Drive.cpp): the learnt seat's sideways place (m_vsh_seat.x; 99 not learnt), for the pad thread
    float              m_vsh_off_max{ 0.0f };   // the camera's largest distance from the seat while held (log)
    int                m_vsh_kept{ 0 };         // frames a driver's weapon did not turn the driving into riding
    void               veh_seat_probe(int frame, const float* f, UINT count, float dist);
    bool               veh_seat_update(int frame);   // update_vehicle: the camera still in the learnt seat
    void               veh_seat_learn(int frame);
    bool              vehtrans_command(const std::string& cmd, const std::string& arg, bool& persist);
    void              veh_follow_start();
    void              veh_follow_step(std::chrono::steady_clock::time_point now);
    void              draw_vehtrans_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    // mountedweapon, mountedaim (FarCry5Mounted.cpp; turret thread 5 Nov, testruns\2026-11-05_turret): a mounted gun or mortar the player
    // uses stays on its mount (the first-person pass not put on the controller), the game's aim follows the right controller's pointing
    // (or the head), the mount's turn limit is not taken for the game ignoring the mouse, and the trigger fires at once.
    std::atomic<bool> m_mounted_on{ true };       // mountedweapon on|off (saved)
    std::atomic<int>  m_mounted_aim{ 0 };         // mountedaim hand|head (saved): 0 the right controller's pointing, 1 the head
    std::atomic<bool> m_mounted_invert{ true };   // mountedinvert on|off (saved): mountedaim hand mirrored (controller up = aim down, right = left)
    float             m_mounted_ref_yaw{ 0.0f };  // mountedinvert: the yaw it mirrors about, in the body frame (the head's when the mount began)
    uint32_t          m_mounted_ref_sig{ 0 };     // mountedinvert: the weapon and the last frame the reference was used for (a new mount takes a new one)
    int               m_mounted_ref_frame{ -1000 };
    std::atomic<bool> m_mounted_held{ false };    // the weapon in hand is a mounted one (mounted_frame_end, each frame)
    uint32_t          m_mounted_sig{ 0 };         // the weapon id the decision was made for
    bool              m_mounted_final{ false };   // decided from the game's data or the id list (else from the item held now: asked again)
    std::string       m_mounted_why;              // what decided it (F11 and the log), under m_mounted_mtx
    mutable std::mutex m_mounted_mtx;
    bool              mounted_now() const { return m_mounted_on.load() && m_mounted_held.load(); }
    bool              mounted_aim_now() const { return mounted_now() && !m_in_vehicle.load(); }   // on foot: the aim loop's part
    void              mounted_frame_end();
    void              mounted_motion(const glm::vec3& cam);   // update_motion: the aim along the controller's pointing (or the head)
    glm::vec3         mounted_inverted(const glm::vec3& ctrl);   // mountedinvert: the controller's pointing mirrored about the mount's forward
    bool              mounted_command(const std::string& cmd, const std::string& arg, bool& persist);
    void              draw_mounted_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    void              spawn_back_axis(float& bx, float& by) const;   // spawnfacing: the player's back axis now, for the spawn commands
    float             m_fade_level{ 0.0f };
    bool              m_fade_was_want{ false }, m_fade_hold{ false };
    int               m_fade_cov_good{ 0 };
    std::chrono::steady_clock::time_point m_fade_hold_t0{};
    float             m_head_rel_yaw{ 0.0f };   // the head's yaw in the body frame (last update_motion)
    // airfollow (default on; author 14 Oct): driving any vehicle with head look, the body frame is the vehicle's own attitude, taken from the game
    // camera with the head look offset it carries taken out (heading and pitch in full, roll times airroll, default 1);
    // the head look then hands the head's direction in that frame straight to the game (no yaw flip in a loop).
    std::atomic<bool>  m_air_follow{ true };
    std::atomic<float> m_air_roll{ 1.0f };
    bool               m_air_target_valid{ false };
    glm::mat3          m_air_target{ 1.0f };     // world -> body: the vehicle's attitude, roll scaled (update_motion, this frame's camera)
    glm::mat3          m_air_A{ 1.0f };          // world -> view of the vehicle's aim frame (camera minus head look), unscaled
    glm::mat3          m_air_rot{ 1.0f };
    glm::vec3          m_air_prev_up{ 0.0f, 0.0f, 1.0f };
    glm::vec3          m_air_prev_fwd{ 0.0f, 1.0f, 0.0f };
    float              m_air_blend{ 0.0f };      // 0 level body frame .. 1 the vehicle's
    // vehsmooth ("Vehicle shake and bob smoothing", default 0 = off since 19 Oct) was removed 24 Oct (author): the view is fixed to the vehicle.
    uint64_t           m_air_spikes{ 0 };
    // airfollow only with "vehicles" (one F11 choice since 24 Oct: without it the view swung onto the vehicle's heading at lift-off).
    bool               air_follow_now() const { return m_air_follow.load() && m_vehicle_follow.load() && m_in_vehicle.load() && m_veh_settle.load() == 0 && !m_menu_screen_on && headlook_active(); }
    // Car comfort (FarCry5VehComfort.cpp; author 24 Oct):
    //  - groundfollow on|off (default off, saved): airfollow's pitch and roll also in cars, quads and boats. Off: only once the
    //    vehicle has been seen flying (veh_flying) or under a parachute; on the ground the horizon stays level, the view turns
    //    with the vehicle's heading only and the cab tilts and bumps around you.
    //  - vehbumps ("Car bump smoothing", the eyes' height filtered while driving) was removed 24 Oct (author).
    //  - vehvignette 0-1 (default 0 = off, saved): in a vehicle, the picture's edges darkened while it turns, speeds up, brakes
    //    or bumps (drawn with vehfade's pass).
    // m_veh_aircraft (the aircraft guard) is not used here: it also fires in cars at speed (the author's 12-14 Oct drives).
    std::atomic<bool>  m_ground_follow{ false };
    std::atomic<float> m_veh_vignette{ 0.0f };
    std::atomic<int>   m_veh_fly_force{ -1 };     // vehfly on|off|auto (test): -1 auto
    bool               m_veh_flying{ false };     // this vehicle has been seen flying (latched until out of it)
    float              m_vfly_climb_t{ 0.0f }, m_vfly_tilt_t{ 0.0f };   // seconds of a slow climb / of a steep tilt (decaying)
    float              m_vfly_tilt{ 0.0f };       // the vehicle model's tilt from level (deg, last pair)
    float              m_vfly_z[20]{};            // the camera's height every 0.25 s over the last 5 s (altitude gain)
    int                m_vfly_zn{ 0 };
    float              m_vfly_zt{ 0.0f };
    bool               m_vc_init{ false };
    glm::vec3          m_vc_vel{ 0.0f }, m_vc_acc{ 0.0f };   // the camera's velocity and acceleration, low-passed (m/s, m/s2)
    glm::quat          m_vc_q{ 1.0f, 0.0f, 0.0f, 0.0f };     // the view's body frame at the last pair
    float              m_vc_rate{ 0.0f };         // the body frame's turn rate, low-passed (deg/s)
    float              m_vig_target{ 0.0f }, m_vig_level{ 0.0f };
    bool               veh_flying() const { return m_veh_fly_force.load() >= 0 ? m_veh_fly_force.load() == 1 : m_veh_flying; }
    // airfollow's attitude is eased in only for these (the target is still read in every vehicle: AFW's cab frame uses it).
    // chutelevel (author 29 Oct 10:15 playtest): a fall or a parachute (chutelook)
    // follows the heading only unless groundfollow is on; see m_chute_level.
    bool               air_follow_attitude() const { return m_ground_follow.load() || (m_chute_vehicle && !m_chute_level.load()) || veh_flying(); }
    // planelook (FarCry5PlaneLook.cpp; the author's 27 Oct playtest: in a plane he could not look down, nor aside with the nose straight
    // up or down): airfollow's attitude read from the vehicle's own model, not the game camera less a head look the game holds back.
    std::atomic<int>   m_plane_look{ 1 };      // cmd planelook off|fly|all (saved): 0 off, 1 flying vehicles (default), 2 every vehicle (test)
    // vehnose (author 29 Oct): at the entry's settle
    // the front is the model's forward axis (y), not the model axis nearest the game camera (a plane's entry camera faces the door).
    std::atomic<bool>  m_veh_nose{ true };     // cmd vehnose on|off (saved)
    // vehnosebig (builder 5 Nov, a plane spawned at rest looked over its wing): when no vehicle model lies right under the camera (that plane's
    // origin is 2.45 m below the seat, past the 2.2 m the heading probe takes), the front is the forward axis of the largest Vehicle_ draw
    // within 5 m (planelook's probe), at the settle and while driving.
    std::atomic<bool>  m_veh_nose_big{ true };   // cmd vehnosebig on|off (saved)
    bool               m_veh_big_front{ false };  // the heading follows that largest model (no model under the camera)
    int                m_veh_settled_frame{ -1000 };   // the frame the last entry or seat change settled (planelook's first look waits after it)
    bool               veh_big_model_y(int frame, glm::vec3& y) const;
    struct PlaneRec { int frame{ -100 }; UINT count{ 0 }; glm::mat3 rot{ 1.0f }; float dist{ 0.0f }; };
    PlaneRec           m_plane_mdl[4]{};       // the vehicle's model by frame (world -> its axes; count = its indices)
    PlaneRec           m_plane_cam[4]{};       // the camera's reading of the aim frame by frame (world -> view; count 1 = head ahead, near level)
    glm::quat          m_plane_seat{ 1.0f, 0.0f, 0.0f, 0.0f };   // the model's axes -> the aim frame's (learnt with the head ahead)
    int                m_plane_seat_n{ 0 }, m_plane_seat_bad{ 0 };
    bool               m_plane_seat_axes{ false };   // stickview: the seat is the model's axes, not yet checked against a look ahead
    glm::quat          m_plane_fix{ 1.0f, 0.0f, 0.0f, 0.0f };    // view -> view: the model's attitude against the camera's reading
    int                m_plane_fix_frame{ -100 }, m_plane_fix_bad{ 0 };
    float              m_plane_blend{ 0.0f };
    uint64_t           m_plane_used{ 0 }, m_plane_nomodel{ 0 }, m_plane_refused{ 0 };
    float              m_plane_fix_sum{ 0.0f }, m_plane_fix_max{ 0.0f };
    std::chrono::steady_clock::time_point m_plane_t{}, m_plane_log_t{};
    bool               planelook_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               planelook_reset();
    void               plane_model_probe(int frame, const float* f, UINT count, float dist);
    glm::mat3          plane_look_fix(int frame, const glm::mat3& a_cam);
    void               draw_planelook_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    bool               vehcomfort_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               veh_comfort_step(std::chrono::steady_clock::time_point now, const glm::vec3& pos, bool mesh_near);
    void               draw_vehcomfort_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                            const std::function<bool(const char*, float&, float, float, const char*, float)>& slider);
    void              fade_tick(float dt);
    void              fade_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);
    void              asm_note_dispatch(ID3D11DeviceContext* ctx);   // asmlog (research)
    void              asm_note_terrain_cs(bool swapped);              // asmlog (research)
    int               frame_now() const { return m_frame.load(); }
    std::atomic<float> m_cull_margin{ 8.0f };  // degrees added on every side (cmd cullmargin)
    bool              m_cull_valid{ false };
    glm::vec3         m_cull_fwd{ 0.0f }, m_cull_side[4]{}, m_cull_side_game[4]{};
    uint64_t          m_cull_replaced{ 0 };
    glm::vec4         m_cull_planes[6]{};
    struct CullStandin { ID3D11Buffer* buf{}; UINT size{}; };
    struct CullSaved { ID3D11Buffer* cb{}; UINT first{}, num{}, slot{}; };
    CullStandin       m_cull_standin[4]{};
    CullSaved         m_cull_saved[4]{};
    int               m_cull_saved_n{ 0 };
    uint64_t          m_cull_seen{ 0 };
    ID3D11Buffer*     m_cull_planebuf{ nullptr };
    bool cull_swap(ID3D11DeviceContext* ctx);
    bool cull_fix_ready(ID3D11DeviceContext* ctx);
    ID3D11ComputeShader*       m_cull_fix_cs{ nullptr };
    ID3D11Buffer*              m_cull_scratch{ nullptr };
    ID3D11UnorderedAccessView* m_cull_scratch_uav{ nullptr };
    ID3D11Buffer*              m_cull_prm{ nullptr };
    bool                       m_cull_fix_tried{ false };
    std::atomic<bool>          m_cull_readback{ false };
    std::atomic<int>           m_cull_fix_mode{ 1 };   // debug: 0 = copies only, no fix-up pass
    void cull_restore(ID3D11DeviceContext* ctx);
    int              m_scan_fwd{ 0 };   // camscan target: 0 position, 1 view Z row, 2 forward
    std::atomic<bool> m_cine_strict{ true };   // stricter cutscene detection (cmd cinestrict)
    // cinedelay (s, default 3): the signs must hold this long before a cutscene starts; takedowns (about 2-2.5 s, the
    // camera taken by the animation, no first-person pass) end before. 0 = the old way.
    std::atomic<float> m_cine_delay{ 3.0f };
    std::chrono::steady_clock::time_point m_cine_signs_since{};
    uint64_t          m_cine_short_skipped{ 0 };
    std::chrono::steady_clock::time_point m_cine_changed_at{};
    std::atomic<bool> m_vista_eye{ true };   // far-pass camera copies with their own depth row get the eye's camera (cmd vistaeye)
    uint64_t          m_vista_replaced{ 0 };
    void capture_tick();
    int  m_capture_step{ 0 }, m_capture_n{ 0 };
    // capturein (F11 Debug "Capture in 3 s", cmd capturein S): the F11 menu closes and the capture starts S seconds later
    // (steady-clock ns, 0 none). m_pd_snap: the capture's eye pictures are armed by the dump's own first frame (same pair as the layers).
    std::atomic<int64_t> m_capture_at{ 0 };
    std::atomic<bool>    m_pd_snap{ false };
    void                 capture_buzz(float seconds);
    std::atomic<bool> m_veh_auto_recenter{ true };   // recentre after entering / leaving a vehicle (cmd vehrecenter)
    // Auto recenter (author 19 Oct, cmd autorecenter, autorecenterdist m, autorecenterwait s): once the head has been further
    // than the distance from the centre (sideways and forwards, not height) for the wait, the centre moves to the head's
    // place; the facing direction stays.
    std::atomic<bool>  m_auto_recenter{ true };
    std::atomic<float> m_auto_recenter_dist{ 0.5f };
    std::atomic<float> m_auto_recenter_wait{ 0.5f };
    std::chrono::steady_clock::time_point m_auto_recenter_since{};
    bool               m_auto_recenter_out{ false };
    bool               m_recenter_pos_requested{ false };
    void               auto_recenter_check(std::chrono::steady_clock::time_point now);
    void               recenter_position_from_hmd();
    std::atomic<bool> m_aircraft_guard{ true };      // flying: the head does not steer the game camera (cmd aircraftguard)
    bool              m_veh_aircraft{ false };
    int               m_veh_air_frames{ 0 };
    // vehair (under vehscript; author 30 Oct 12:31 log: on the quad, the game's type 0 wheeled, an off-road roll of 15 deg set
    // m_veh_aircraft, the head stopped steering the game camera and the eyes sat 100-160 deg off it for 11 s, culled): the game's
    // vehicle type while it answers (0 wheeled, 1 boat, 3 motorcycle, 4 helicopter, 5 plane; -1 no answer). A known type other
    // than 4 or 5 is never flying (m_veh_aircraft and vehcomfort's veh_flying); a helicopter, a plane or no answer: the tests as before.
    int               m_veh_game_type{ -1 };
    bool              m_vehair_said{ false };   // the "not flying" line, once per ride
    bool              veh_type_grounded() const { return m_veh_game_type >= 0 && m_veh_game_type != 4 && m_veh_game_type != 5; }
    uint64_t          m_veh_heading_spikes{ 0 };
    int               m_exit_settle{ 0 }, m_exit_stable{ 0 };
    float             m_exit_yaw{ 0.0f };
    std::chrono::steady_clock::time_point m_exit_settle_t0{};   // 0 classic, 1 A jump / B reload / X interact / Y crouch (cmd controls classic|new)
    std::atomic<uint64_t> m_pad_polls{ 0 }, m_lt_polls{ 0 };
    std::atomic<uint64_t> m_pad_last_poll{ 0 };   // GetTickCount64 of the game's last pad read
    bool               m_scope_probe_mouse{ false };   // scopelearn pressed the right mouse button (the game reads the mouse, not the pad)
    void set_menu_input(float x, float y, bool trigger) { m_menu_stick_x = x; m_menu_stick_y = y; m_menu_trigger = trigger; }
    void set_turn_input(float x) { m_turn_input = x; }
    // The controller trigger: the game only sees the fire button once its camera is on the barrel line.
    void set_trigger(bool down) { m_trigger = down; }
    // Stick aim: the right stick the game should see (-1..1), or nothing while aiming by mouse.
    bool  stick_aim() const { return m_stick_aim.load(); }
    float stick_x() const { return m_stick_x.load(); }
    // vehstick: the right stick's X goes to the game (piloting with head look, or flying with the aircraft guard).
    bool  vehicle_stick_pass() const
    {
        return m_veh_stick.load() && m_in_vehicle.load() && m_veh_driving.load() && m_veh_settle.load() == 0 &&
               (headlook_active() || (m_veh_aircraft && m_aircraft_guard.load()));
    }
    // helipitch: piloting (vehstick's conditions with its own switch). The helicopter type is checked by the caller.
    bool  heli_pitch_pass() const
    {
        return m_heli_pitch.load() && m_in_vehicle.load() && m_veh_driving.load() && m_veh_settle.load() == 0 &&
               (headlook_active() || (m_veh_aircraft && m_aircraft_guard.load()));
    }
    // stickspeed: driving (settled) a ground vehicle or a boat. type: vehscript's (0 wheeled, 1 boat, 3 motorcycle); -1 unknown: off (a
    // helicopter on the ground would take the throttle for up; the aircraft guard also fires in cars at speed, so it cannot tell).
    bool  stick_speed_pass(int type) const
    {
        if (!m_stick_speed.load() || !m_in_vehicle.load() || !m_veh_driving.load() || m_veh_settle.load() != 0) return false;
        return type == 0 || type == 1 || type == 3;
    }
    float stick_y() const { return m_stick_y.load(); }
    bool fire_open() const { return m_fire_open; }

private:
    bool update_motion(int frame, const glm::mat3& game_rot, const glm::vec3& cam);
    void motion_post_present(float dt);
    void note_stick_by_hand(const char* where);   // snapstick: the stick aim switched by hand is logged
    struct MotionFrame
    {
        int       frame{ -1 };
        bool      valid{ false };
        bool      ctrl_valid{ false };
        glm::mat3 game_rot{ 1.0f };
        float     ev_oy{ 0.0f }, ev_op{ 0.0f };   // the Extended View offset applied to that camera (read with it)
        bool      ev_ok{ false };
        uint64_t  ev_seq{ 0 };                     // game Tobii updates started when this camera was uploaded
        float     ev_l[3][2]{};                    // the offsets of 2, 3, 4 updates back (lag check)
        bool      ev_l_ok{ false };
        glm::vec3 cam{ 0.0f };
        glm::mat3 body_rot{ 1.0f };        // world -> body view
        glm::mat3 ctrl_rot_world{ 1.0f };  // controller axes -> world
        glm::vec3 ctrl_pos_world{ 0.0f };
        glm::vec3 aim_world{ 0, 1, 0 };    // where the game camera must look (shots follow the barrel)
        glm::vec3 head_world{ 0, 1, 0 };   // where the head looks (the game camera follows it between shots)
        glm::vec3 barrel_origin{ 0.0f };   // the muzzle, world
        glm::vec3 barrel_dir{ 0, 1, 0 };   // the shots' direction: the barrel, or with sightaim the gun's sight line
        glm::vec3 aim_origin{ 0.0f };      // a point of the shots' line (aim point = aim_origin + barrel_dir * distance): the muzzle,
                                           // or with sightaim the sight line's point beside the muzzle
        bool      sight_aim{ false };      // sightaim: this frame's shots follow the gun's sight line (FarCry5Sight.cpp)
        float     converge{ 25.0f };       // metres along the barrel where the shots meet it
        glm::vec3 ctrl_ref{ 0.0f };        // the controller, reference space (diagnostics)
        uint64_t  weapon_views{ 0 };
        bool      other_valid{ false };       // the other (non-weapon) hand's controller
        glm::vec3 other_pos_world{ 0.0f };
        glm::mat3 other_rot_world{ 1.0f };
        bool      slung{ false };             // sling: the weapon (ctrl_*) hangs on the sling; the weapon hand is at hand_*
        bool      bow{ false };               // manualbow (FarCry5Bow.cpp): the bow (ctrl_*) is in the other hand; the weapon hand is at hand_*
        bool      kept{ false };              // twohandkeep (FarCry5Gesture.h): the weapon (ctrl_*) held by the other hand alone; the weapon hand is at hand_*
        glm::vec3 hand_pos_world{ 0.0f };
        glm::mat3 hand_rot_world{ 1.0f };
        bool      chest_valid{ false };       // throwable: its place on the chest (FarCry5Gesture throw_pose), world
        glm::vec3 chest_pos_world{ 0.0f };
        glm::mat3 chest_rot_world{ 1.0f };
        bool      carry_valid{ false };       // sidearm: the sling's and the hip's weapon places (as ctrl_*: origin the grip), world
        glm::vec3 sling_pos_world{ 0.0f }, hip_pos_world{ 0.0f };
        glm::mat3 sling_rot_world{ 1.0f }, hip_rot_world{ 1.0f };
        bool      pouch_valid{ false };       // reloadmanual: the magazine pouch on the hip, world (FarCry5ReloadMag.cpp draws the hip's magazine there),
        glm::vec3 pouch_pos_world{ 0.0f };    // with the same head pose sample as the sidearm's hip; the body's heading there (level)
        glm::vec3 pouch_fwd_world{ 0.0f, 1.0f, 0.0f };
        glm::vec3 draw_shift{ 0.0f };         // gunanchor: added to the drawn gun and hands (eyes' base minus game camera)
        glm::vec3 muzzle_drawn{ 0.0f };       // the drawn muzzle (barrel_origin + draw_shift): scope lens test, tracers
        float     sight_dist{ 0.0f };         // sightdepth: the eased distance the red dot, its ring and the scope picture are put at (0 = off: each its own raw hit)
        bool      arm_body_valid{ false };    // armik (FarCry5ArmIkDraw.cpp): the body under the head, world: the point between the shoulders and
        glm::vec3 arm_mid_world{ 0.0f };      // the body's axes (right, up, back; level, the gestures' body heading)
        glm::vec3 arm_right_world{ 1.0f, 0.0f, 0.0f }, arm_up_world{ 0.0f, 0.0f, 1.0f }, arm_back_world{ 0.0f, -1.0f, 0.0f };
    };
    glm::mat4          m_hand_ref{ 1.0f };    // sling: the weapon controller itself, reference space
    bool               m_slung_was{ false };  // sling: last frame's state (the weapon smoothing restarts at a change)
    bool               m_kept_was{ false };   // twohandkeep: last frame's m_mo.kept
    bool               m_bow_was{ false };    // manualbow: last frame's m_mo.bow (the weapon smoothing restarts at a change)
    // twohandkeep: the weapon's pose (k) in the other controller's (reference space), taken each frame of the two-handed hold, so the weapon
    // left in the left hand stays exactly where it was drawn at the let-go.
    glm::mat4          m_keep_rel{ 1.0f };
    bool               m_keep_rel_ok{ false };
    bool               m_kept_said{ false };
    glm::vec3          m_bow_hand_cam{ 0.0f };   // manualbow: the game's left (support) hand on the bow, weapon pass game-camera axes (the bow's grip)
    int                m_bow_hand_frame{ -100 };
    uint32_t           m_bow_hand_sig{ 0 };      // the weapon it was measured on
    std::chrono::steady_clock::time_point m_armed_seen{};   // sling: the last time a weapon was drawn in hand
    uint32_t           m_sling_glove[4]{};    // sling diagnostics: glove draws while slung: moved, no hand map, no bones, hands not found
    MotionFrame        m_mo{};
    uint64_t           m_weapon_rewrites{ 0 };
    std::atomic<bool>  m_motion{ false };
    std::atomic<bool>  m_left_handed{ false };
    bool               m_motion_auto_done{ false };
    bool               m_body_valid{ false };
    float              m_body_yaw{ 0.0f };
    std::atomic<float> m_turn_input{ 0.0f }, m_dbg_turn{ 0.0f };
    std::atomic<float> m_turn_speed{ 120.0f };
    // Snap turn (default) or smooth turn on the right stick: snapturn on|off, snapdeg N, turnspeed N.
    std::atomic<bool>  m_snap_turn{ true };
    std::atomic<float> m_snap_deg{ 30.0f };
    bool               m_snap_armed{ true };
    // snapsync (default on): a snap turn turns the game camera first (the whole angle in one mouse send) and the eyes
    // turn at the first upload whose camera has turned, so the eyes never face where the game has not culled for.
    std::atomic<bool>  m_snap_sync{ true };
    // aimlead N (pairs): the game camera aimed ahead of a turning head by its turn speed (see motion_post_present).
    std::atomic<float> m_aim_lead{ 1.0f };
    bool               m_lead_valid{ false };
    float              m_lead_prev_yaw{ 0.0f }, m_lead_prev_pitch{ 0.0f }, m_lead_vel_yaw{ 0.0f }, m_lead_vel_pitch{ 0.0f };
    float              m_snap_sync_pend{ 0.0f };   // body yaw waiting for the game camera (rad)
    float              m_snap_sync_game0{ 0.0f };  // game camera yaw when the turn was sent
    int                m_snap_sync_frame0{ 0 };
    int                m_snap_sync_counts{ 0 };
    uint64_t           m_snap_sync_seq{ 0 };        // snapready: the update the snap's counts were recorded at
    bool               m_snap_sync_full{ false };   // snapready: wait for the whole turn (the firing offset reads the aim, so a half-landed send set it 20 deg off)
    std::atomic<bool>  m_veh_pitch_guard{ true };  // cmd vehpitchguard (23 Oct): a car's pitch read against the head look past 20 deg is kept
    std::atomic<bool>  m_veh_fly_guard{ true };    // vehflyguard (25 Oct, saved): a vehicle's tilt counts toward flying only while it moves (FarCry5VehComfort.cpp)
    float              m_veh_pitch_ref{ 0.0f };
    bool               m_veh_pitch_ref_ok{ false };
    int                m_veh_pitch_guarded{ 0 };
    std::atomic<bool>  m_settle_pitch{ true };     // cmd settlepitch (23 Oct): Extended View pitch follows the head during a vehicle's entry animation
    // exitpitch on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 29 Oct 10:12 playtest: he got out of a car
    // looking down, and for a second the eyes were 55 deg below the camera the game culls with, every frame past its view): settlepitch's
    // Extended View pitch during the exit animation too, so the culled camera looks up / down with the head while getting out.
    std::atomic<bool>  m_exit_pitch{ true };
    // exityaw on|off (default on since author 7 Nov 19:59Z, saved; F11 Comfort > Cinematics and vehicles, next to exitpitch; author 7 Nov 14:11, research\2026-11-07_camera_culling_transitions.md T2 / F2): during the exit animation the
    // head's yaw too goes to the game through Extended View (up to splitevmax handed), so the camera the game culls with stays on the head
    // while the animation swings it (up to 261 deg in the author's logs). The exit's follow snaps and its end read the animation's camera less
    // that offset (veh_anim_cam_yaw); once the animation is over firesplit carries on from the offset while the aim comes back (as wheellook).
    // One "[Vehicle] exityaw" line at each exit's start and end. Off: as before 7 Nov (pitch only, exitpitch).
    std::atomic<bool>  m_exit_yaw_ev{ true };
    std::atomic<bool>  m_exy_active{ false };      // exityaw: handing the yaw now
    std::atomic<float> m_exy_oy{ 0.0f };           // exityaw: the yaw offset the current camera carries (radians)
    int                m_exy_n{ 0 }, m_exy_clamped{ 0 };
    float              m_exy_max{ 0.0f };
    float              veh_anim_cam_yaw() const;   // the game camera's yaw less exityaw's offset (the exit animation's own camera)
    // cullpitch N (degrees 0-10, default 0 = off, saved; F11 Comfort > Cinematics and vehicles; research\2026-11-07_camera_culling_transitions.md F1):
    // wherever the mod points the game's culled camera through Extended View (firesplit, wheellook, the head look in vehicles, settlepitch,
    // exitpitch / exityaw), it aims N deg below the head. The game's view reaches 58 deg down and 64.3 up from its centre, Quest 3 eyes 55 down
    // and 44 up: only 3 deg spare at the bottom (362 of 598 headset culling episodes short there), 20 at the top; 4.5 deg evens them (7.5 / 15.8).
    // The game's aim is read as the camera less the offset, so shots do not move. Not on foot with empty hands (the mouse aims there, no offset).
    std::atomic<float> m_cull_pitch_deg{ 0.0f };
    float              cull_bias() const;                              // cullpitch in radians
    glm::vec3          cull_target(const glm::vec3& head_world) const; // the head's direction tilted cullpitch down (normalised)
    // chutelevel on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 29 Oct 10:15 playtest: bailing out of the plane,
    //). A fall held as a vehicle (chutelook) put the view on the fall's attitude: the game's
    // camera less the head look, which in a free fall points at the ground (his log: 47-53 deg down, vehpitchguard 451 times), so the
    // whole world tilted ~50 deg within 0.6 s and swung back on landing (~20 deg on a 1 s drop at 10:09:03). Now a fall or a parachute
    // keeps the horizon level and turns with its heading only, as cars do (the head look still read in its aim frame, so culling is as
    // before), and the eyes leave the body-based base (viewroot) over 0.15 s instead of in one pair (a 10-24 cm jump at the fall's
    // start with the camera ahead of the body). groundfollow on still follows the attitude. Off: as before.
    std::atomic<bool>  m_chute_level{ true };
    // downedfoot on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 29 Oct 13:54). A vehicle was told from the hands' pass alone: no weapon view for 20 updates = in a vehicle. Being knocked
    // down hides the weapon view too, so his log (14:48:17) took it for getting into a car: the entry recentre, the view turned with the
    // "car", its pitch guarded against the head (the eyes 38 deg off the game camera on average), then the getting-out recentre on the
    // revive (75 deg off for a second). Three more the same day (a death, two animations), all with no vehicle model near the camera.
    // Now no weapon view is a vehicle only with proof: a vehicle model under the camera since the weapon view went, the seat, or the
    // camera moving over 3 m/s for 20 updates (a vehicle whose model is not found). Falls and parachutes (chutelook) are as before.
    // On foot (walkbob): the eyes' level more than 0.5 m off the camera's height over the body (knocked down, getting up; the game
    // animates a crouch, so the level keeps up with it) jumps to it instead of easing over 0.2 s (the eyes stayed 57-65 cm above the
    // falling camera). Off: as before.
    std::atomic<bool>  m_downed_foot{ true };
    // downview (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 09:10): knocked down (the game's IsPawnDown, read in vehscript's poll), the eyes were 50-81 deg off the game
    // camera on average and their base jumped 15-96 cm getting up. While down and 1.5 s after: the view's heading is steered to the game
    // camera's (a cut on a camera jump, eased past 15 deg), the horizon level; the eyes' base eased to the game camera (at most 4 cm of
    // correction a pair, ~0.3 s), then eased back to the body view. The aim sends nothing while down (FarCry5DownView.cpp).
    std::atomic<bool>  m_down_view{ true };
    std::atomic<int>   m_down_force{ -1 };   // downforce on|off (test, unsaved): 1 fakes the down state, -1 the game's
    bool               m_dv_active{ false }, m_dv_down{ false }, m_dv_steering{ false }, m_dv_yaw_ok{ false }, m_dv_pos_ok{ false };
    float              m_dv_prev_yaw{ 0.0f }, m_dv_corr_max{ 0.0f }, m_dv_dy_sum{ 0.0f }, m_dv_dy_max{ 0.0f }, m_dv_dy_start{ 0.0f };
    int                m_dv_pair{ -1 }, m_dv_cuts{ 0 }, m_dv_steers{ 0 }, m_dv_n{ 0 }, m_dv_cut_logs{ 0 };
    glm::vec3          m_dv_pos{ 0.0f }, m_dv_off{ 0.0f }, m_dv_prev_cam{ 0.0f }, m_dv_prev_base{ 0.0f }, m_dv_prev_gcam{ 0.0f };
    std::chrono::steady_clock::time_point m_dv_t0{}, m_dv_up_t{}, m_dv_t{}, m_dv_pos_t{};
    void               update_downview();   // beside update_vehicle: the down state and the view's heading
    glm::vec3          down_view_base(int frame, const glm::vec3& cam, const glm::vec3& base_cam);   // the eyes' base eased to the game camera
    bool               down_view_holds_aim() const { return m_dv_active && m_dv_down; }
    void               down_view_release(const char* why);
    bool               downview_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               draw_downview_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    // qtehud (default on, saved; F11 Comfort > Cinematics and vehicles, under downview; author 8 Nov 11:29): with hudmarkers on, every HUD draw before the compass bar is taken for a world marker, shrunk to
    // hudmarksize and shown where the game camera saw it. A struggle's prompts (the bear's) are drawn there too, while the game camera is
    // the animation's (his 11:09:51 log: eyes 35 deg off it on average, worst 90, the camera pitched up), so they were tiny and out of
    // view. While the player is knocked down (IsPawnDown, stance 4, downview's hold) or in a QTE (IsInQTE), no draw goes to the world
    // markers: the whole HUD stays on the panel, prompts where the flat game shows them. Off: as before.
    std::atomic<bool>  m_qte_hud{ true };
    std::atomic<bool>  m_qte_hud_now{ false };   // set once a pair by update_qte_hud, read by hud_begin
    uint64_t           m_qte_hud_kept{ 0 };      // marker-phase draws kept on the panel while it holds
    std::chrono::steady_clock::time_point m_qte_hud_t0{};
    void               update_qte_hud();          // after update_downview
    bool               m_veh_proof_mesh{ false };   // a vehicle model under the camera, or within 4 m of it, since the weapon view went
    // vehnear (author 29 Oct 15:01, headset; log 15:56:42): the truck's model origin sat 2.1 m from the seated camera, never under it, so the truck counted only once it
    // drove over 3 m/s, 4 s late, and the entry animation turned the game camera 42-84 deg away from a view kept on foot. Any vehicle
    // model within 4 m of the camera is proof too (the downed, death and animation cases had none within 8 m).
    int                m_veh_close_frame{ -100 };
    bool               m_veh_proof_said{ false };
    int                m_veh_fast_frames{ 0 };      // updates in a row with the camera over 3 m/s, while no weapon view
    float              m_veh_cam_speed{ 0.0f };     // the game camera's speed, eased over 0.3 s
    int                m_downed_kept{ 0 }, m_bob_drop_snaps{ 0 };
    // bobsnapguard (under downedfoot; author 13:25 log, 13:30:42-13:31:04: ~30 snaps under
    // the chute, the body root seen 0.2-0.7 m and 1.85 m under the camera in turn): no snap under a chute, in a fall, in a vehicle or with
    // downview active; one snap, then none until the level has stayed within 0.5 m for 1 s; a root sample 1 m off the last kept one is
    // held back for 0.5 s (the bob off meanwhile) and dropped if the root comes back. Logged once per episode, with a count.
    bool               m_bsg_armed{ true }, m_bsg_pend{ false };
    float              m_bsg_ref_lvl{ 0.0f }, m_bsg_h_kept{ 0.0f };
    int                m_bsg_blocked{ 0 }, m_bsg_flips{ 0 }, m_bsg_episode{ 0 }, m_bsg_snaps{ 0 };
    bool               m_bsg_h_kept_ok{ false };
    std::chrono::steady_clock::time_point m_bsg_quiet_t0{}, m_bsg_pend_t0{}, m_bsg_last_t{};
    // vehscript on|off (default on, saved; F11 Comfort > Cinematics and vehicles, "Vehicles: ask the game"; author 29 Oct 15:22): in a vehicle or not is the game's own answer (its GetPawnVehicle for the player, fc5::lua::vehicle_state,
    // asked every 150 ms) instead of the hands' pass and the downedfoot proofs. Falls and parachutes (chutelook) still come first; with
    // no answer (off, a fault, menus, the script thread not seen) the rules above decide. Off: as before.
    std::atomic<bool>  m_veh_script{ true };
    // gamestate on|off (log only, off by default, saved; F11 Comfort > Cinematics and vehicles "Log the game's own state (test)"; command gamestatestatus): the game's own state (fc5::lua::gamestate_*) logged on each
    // change, the pause byte beside the mod's menu/cinematic/vehicle state, to check the engine natives before any rule uses them.
    std::atomic<bool>  m_gamestate{ false };
    bool               m_veh_heur_want{ false };      // what the hands' pass rules alone would say (compared every 10 s)
    int                m_vsc_n{ 0 }, m_vsc_game_in{ 0 }, m_vsc_rules_in{ 0 }, m_vsc_in_out{ 0 }, m_vsc_out_in{ 0 }, m_vsc_unknown{ 0 }, m_vsc_drv{ 0 };
    std::chrono::steady_clock::time_point m_vsc_t0{};
    std::atomic<bool>  m_wheel_force{ false };     // cmd wheelforce (test): the aim loop acts as if the weapon wheel were open
    std::atomic<bool>  m_wheel_look{ true };       // cmd wheellook (23 Oct): Extended View keeps the culled camera on the head while the weapon wheel is open
    std::atomic<bool>  m_body_start_head{ true };  // cmd bodystarthead (23 Oct): the body frame starts with the head where the game looks (was the controller)
    std::atomic<float> m_aim_back_limit{ 160.0f };  // cmd aimbacklimit (author 22 Oct, 0 = off): the game's aim at most this far round from the head
    float              m_aim_back_r{ 0.0f };
    bool               m_aim_back_valid{ false };
    int                m_aim_back_clamped{ 0 };
    std::atomic<float> m_split_ev_pitch{ 120.0f };  // cmd splitevpitch (23 Oct): the firing offset pitch limit, handed deg (x0.75 applied: 120 = 90 deg; was 69 = 52)
    // Culling coverage (cmd culllog N: N frames traced; a line per second while an eye reaches outside the game's view):
    // how far each eye's view reaches past the camera the game culls with.
    std::atomic<int>   m_cov_trace{ 0 };
    int                m_cov_frame{ -1 };
    int                m_cov_frames{ 0 }, m_cov_out{ 0 }, m_cov_out2{ 0 };
    float              m_cov_worst{ 0.0f }, m_cov_worst_yaw{ 0.0f }, m_cov_worst_pitch{ 0.0f }, m_cov_worst_roll{ 0.0f };
    float              m_cov_last{ 0.0f }, m_cov_abs_sum{ 0.0f };
    std::string        m_cov_worst_where{};
    int                m_cov_snaps{ 0 }, m_cov_snap_frames{ 0 }, m_cov_snap_timeouts{ 0 };
    std::chrono::steady_clock::time_point m_cov_t0{};
    void               cull_coverage(int frame, const glm::mat3& eye_rot, const glm::vec4& tan, const glm::mat3& game_rot, const glm::mat4& game_proj);
    // Settings that persist (bin/fc5vr_settings.txt, replayed at start): the last line of each command.
    std::map<std::string, std::string> m_persist{};
    void               save_settings();
    // Resolution multiplier (x1.0 .. x2.0 per axis) on the headset's eye size: the eye images and the
    // DLSS output; the game renders at that times the "auto" ratio of fc5vr_resolution.txt. Read at
    // start (it sizes the swap chains): a change applies at the next start.
    std::atomic<float> m_res_scale{ 1.0f };
    bool               m_settings_loaded{ false };
    // The game's HUD (Scaleform "Fire UI" shaders): 0 in the eye image, 1 on its own panel, 2 hidden.
    std::atomic<int>   m_hud_mode{ 0 };
    // Aim dot: where the shots land (the barrel ray's hit, or the convergence distance), per eye.
    std::atomic<bool>  m_aim_dot{ true };
    std::atomic<float> m_aim_dot_px{ 6.0f };
    // Radius of the red dot in pixels of an eye image w pixels wide (the old green square, aimsquare, removed 19 Oct).
    float aim_marker_px(UINT w) const
    {
        const float sq = (float)std::max<LONG>(2, (LONG)(m_aim_dot_px.load() * w / 3072.0f));
        return std::max(1.5f, 0.6f * sq);
    }
    ID3D11View*            m_bb_rtv{ nullptr };   // render target or UAV view of the dot's target
    ID3D11Texture2D*        m_bb_rtv_tex{ nullptr };
    void draw_aim_dot(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb);
    // Scopes (research\fc5_scopes_report.md, steps S1-S2; FarCry5Scope.cpp). Master switch "scope", off by default:
    // with it off none of the scope code runs. While the dominant eye is behind the weapon's scope (ocular lens from
    // scopecal, per weapon), that eye gets the lens: a magnified crop of its own image around the aim point, a black
    // tube ring, a reticle at the aim point and a dark crescent when the eye is off the scope's axis.
    // ocular lens centre (controller axes, m; default = the scoped rifle's eyepiece, measured 2026-10-12), lens radius (m), magnification
    struct ScopeCal { float off[3]{ 0.0f, 0.064f, 0.074f }; float radius{ 0.018f }; float mag{ 4.0f }; };
    struct ScopeState
    {
        int       frame{ -10 };
        bool      active{ false };        // this frame's eye is the dominant one and is behind the scope
        bool      cal_found{ false };
        UINT      sig{ 0 };
        glm::vec3 lens{ 0.0f }, axis{ 0, 1, 0 }, eye_pos{ 0.0f };
        glm::mat3 eye_rot{ 1.0f };
        float     dist{ 0.0f }, angle{ 0.0f }, radius{ 0.018f }, mag{ 1.0f };
        float     full_mag{ 1.0f };       // the optic's magnification (mag ramps up to it)
        bool      dominant{ false };      // this frame's eye is the dominant one
        glm::vec3 mlens{ 0.0f }, maxis{ 0, 1, 0 };   // scopemodel: the eyepiece on the gun, the scope's axis (to the aim point)
        float     mradius{ 0.018f };
        glm::vec3 mright{ 1, 0, 0 }, mup{ 0, 0, 1 };   // the picture's right and up: the gun's
        float     mfield{ 0.27f };                     // the field at the picture's edge (scene tangent before magnification)
        float     mangle{ 0.0f }, mfade{ 1.0f };       // the gun's axis vs the eye's line to the picture (deg); 0 = off
        bool      mvalid{ false };
        glm::vec3 cpos{ 0.0f };                        // the controller when this was computed (scope_draw_held moves the gun-fixed parts with it)
        glm::mat3 crot{ 1.0f };
    };
    // Step 3 (scopepass, research\fc5_scope_pass_plan.md P1-P3): a third call of the render-thread frame function after
    // the right eye, from a scope camera at the ocular lens looking at the aim point with the optic's narrow field of
    // view. Its present is muted (no swap chain, no OpenXR, no frame advance), it runs with the right eye's frame number
    // and its own camera set (m_mats_eye[2]), without DLSS, the weapon pass or the HUD. The picture after SMAA goes on
    // the lens in the next frames. The game's history textures are saved before the call and put back after it (the
    // scope keeps its own copies), so both eyes see the same history as without the call.
    struct ScopeCam
    {
        bool      valid{ false };
        int       frame{ -10 };          // the right eye's frame of the pair it was rendered after
        glm::vec3 pos{ 0.0f };
        glm::mat3 rot{ 1.0f };           // world -> scope view (rows right, up, back)
        float     tan_half{ 0.0f };      // half field of view (tangent) the lens needs
        float     tx{ 0.0f }, ty{ 0.0f }; // the projection's half tangents (square pixels)
        float     mag{ 1.0f };
        glm::mat4 vrp{ 1.0f };
    };
    std::atomic<bool>  m_scope_pass{ false };     // cmd scopepass on|off (saved): step 3, the scope's own render call
    ScopeState         m_scope_dom_st{};          // the dominant eye's last look-through state
    ScopeCam           m_scope_cam{};             // the camera of the scope call being made (or last made)
    ScopeCam           m_scope_img_cam{};         // the camera of the picture on the lens (captured)
    glm::mat4          m_scope_prev_vrp{ 1.0f };
    glm::vec3          m_scope_prev_cam{ 0.0f };
    int                m_scope_prev_frame{ -10 };
    int                m_scope_world_views{ 0 };  // world camera uploads in this scope call
    uint64_t           m_scope_presents_before{ 0 };
    int                m_scope_run{ 0 };          // scope calls in a row (one per pair); the lens shows the pass picture from 4 on
    bool               m_scope_pass_broken{ false };
    uint64_t           m_scope_calls{ 0 }, m_scope_captures{ 0 }, m_scope_pass_draws{ 0 }, m_scope_call_skips{ 0 };
    double             m_scope_call_ms{ 0.0 };
    double             m_scope_scan_ms{ 0.0 };   // the mod's buffer scan inside the scope calls (this 5 s window)
    int                m_scope_calls_window{ 0 };
    // Per-frame counters the scope call must not leave changed (reset at present, which the scope call does not reach).
    struct ScopeSaved
    {
        int  frame{ 0 };
        uint32_t views{ 0 }, player_views{ 0 }, modified{ 0 };
        int  weapon_draws{ 0 }, weapon_cb_n{ 0 }, cine_wcb_n{ 0 }, world_cb_n{ 0 };
        UINT weapon_sig_frame{ 0 };
        bool scene_cleared{ false }, hud_drawn{ false };
        ID3D11Resource* weapon_cb{ nullptr };
        ID3D11Resource* world_cb{ nullptr };
    } m_scope_saved{};
    // Cost cuts for the scope call (plan section 4b), each its own switch under Step 3:
    // scoperes N: the scope picture is N x N render pixels in the top-left corner of the game's targets (projection
    // off-centre so the scope's view lands there, draws into screen-sized targets scissored to it; compute passes still
    // cover the whole target); 0 = the whole render size. scopeskip MASK: passes left out of the scope view (1 sun
    // shadow draws, 2 reflections, 4 ambient occlusion, 8 volumetric fog). scopeevery N:
    // the scope call every Nth pair (the lens keeps the last picture in between).
    // scopesrc 1 (plan 4b item 4, scopes report option A): no third call; while scoped, the dominant eye's own call draws
    // the scope camera's view (at its full size, through DLSS) and that eye shows only the lens, black around it.
    // Engagement (the author's headset test 2026-10-11: the scope never engaged: no gun had an entry, and the fixed lens
    // offset rarely lines up with a real head). scopeauto: look-through from the eye's place on the gun's sight line, lens
    // on the eye's line of sight. scopelearn: each gun's magnification from the game's aim-down-sights zoom.
    std::atomic<bool>  m_scope_auto{ true };      // cmd scopeauto on|off (saved)
    std::atomic<bool>  m_scope_learn{ true };     // cmd scopelearn on|off (saved)
    // adsprobe off|optic|all (default optic, saved; F11 Weapon > Sights; author 29 Oct 13:48): sightlearn and scopelearn pressed the game's aim down sights for 0.3-1.5 s the first time a gun came near
    // the eye (5 guns in his 13:43 log: the game's narrow aim-down-sights camera, its blur and slow walk). optic: only a gun that draws
    // sight glass (a scope, a red dot) is read so, for its zoom; an iron-sight gun never (its line from the gun's axes is the barrel's,
    // and a line is still read whenever you aim down sights yourself). off: never. all: every gun (before 29 Oct).
    std::atomic<int>   m_ads_probe{ 1 };
    bool               ads_probe_ok(uint32_t sig) const { const int m = m_ads_probe.load(); return m == 2 || (m == 1 && optic_now(sig)); }
    // scopesticky on|off (default on, saved): the last magnifying entry is kept through a moment's change of weapon id (a bolt cycling)
    // only while the eye stays behind the scope; off: for 1.5 s whatever the eye does (an iron-sight gun taken within 1.5 s of a
    // scoped one showed its x3.3 lens: weapon 60537 after 23397, 13:46:46 in the author's log).
    std::atomic<bool>  m_scope_sticky_eye{ true };
    std::atomic<bool>  m_scope_probe_lt{ false }; // scopelearn's short aim-down-sights press (read by the pad hook)
    std::atomic<float> m_game_hfov{ 0.0f };       // the game camera's horizontal FOV (world view, before the eye rewrite)
    struct ScopeAds { int state{ 0 }; std::chrono::steady_clock::time_point t0{}, drop{}; float hip{ 0.0f }, min{ 0.0f }; UINT sig{ 0 }; bool probe{ false }, upgrade{ false }, optic{ false }; UINT extra[4]{}; int extra_n{ 0 }; } m_scope_ads{};
    ScopeCal           m_scope_sticky{};
    UINT               m_scope_sticky_sig{ 0 };
    std::chrono::steady_clock::time_point m_scope_sticky_at{};
    std::chrono::steady_clock::time_point m_scope_sig_since{}, m_scope_lt_up_since{};
    UINT               m_scope_sig_last{ 0 };
    bool               m_scope_lt_was{ false };
    float              m_scope_hip_fov{ 0.0f };
    struct ScopeProbeTry { int n{ 0 }; std::chrono::steady_clock::time_point last{}; };
    std::unordered_map<UINT, ScopeProbeTry> m_scope_probed{};   // zoom reads tried per weapon (a failed read is retried)
    void               scope_learn_tick(bool on_foot, bool at_scope, UINT sig, bool known);
public:
    bool               scope_probe_lt() const { return m_scope_probe_lt.load() || m_sight_probe_lt.load(); }   // scopelearn's or sightlearn's press
private:
    std::atomic<int>   m_scope_src{ 0 };
    int                m_scope_src_frame{ -10 };  // the eye frame drawn from the scope camera
    int                m_scope_eye_frame_seen{ -10 };
    bool               m_scope_src_was{ false };
    int                m_scope_hold{ 0 };         // pairs without the scope call after scopesrc 1 -> 0 (the eye's history flushes first)
    glm::mat4          m_scope_eye_vrp{ 1.0f };   // that frame's own eye camera (where the lens is in the eye)
    glm::vec3          m_scope_eye_cam{ 0.0f };
    ScopeCam           m_scope_src_cam{};
    void               scope_camera(const ScopeState& ds, int frame, ScopeCam& sc);   // at the ocular lens, at the aim point, upright as the eye
    std::atomic<int>   m_scope_res{ 0 };
    std::atomic<int>   m_scope_skip{ 0 };
    std::atomic<int>   m_scope_every{ 1 };
    uint32_t           m_scope_pair_n{ 0 };
    int                m_scope_corner{ 0 };       // corner size of the call being made (render pixels, 0 = whole)
    UINT               m_cur_rt_w{ 0 }, m_cur_rt_h{ 0 };   // the bound target's size (scope call with a corner only)
    uint64_t           m_scope_skipped_draws{ 0 }, m_scope_skipped_cs{ 0 }, m_scope_scissored{ 0 };
    bool               scope_skip_ps(ID3D11PixelShader* ps);
    bool               scope_skip_cs(ID3D11ComputeShader* cs);
    void               scope_scissor(ID3D11DeviceContext* ctx);
    void               scope_scissor_end(ID3D11DeviceContext* ctx);
    bool               scope_shadow_targets() const { return m_scope.load() && m_scope_pass.load() && (m_scope_skip.load() & 1) != 0; }
    bool               scope_call_begin(int right_frame);
    void               scope_call_end(double cpu_ms);
    void               scope_viewport(float* d, uint32_t size, int frame);
    void               scope_capture(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);
    void               scope_hist_note(ID3D11Resource* res);
    bool               scope_hist_has(ID3D11Resource* res);
    void               scope_hist_role(const void* sh, UINT slot, bool uav, ID3D11Resource* res);
    // scopehistswap on|off (default on, saved): after the scope call, the histories the game alternates between two textures
    // swap contents back, so the next eye calls find what they would have found without the scope call (author 21 Oct
    // 17:51: sun shadows soft and stuck to the view outside the scope while the pass runs).
    std::atomic<bool>  m_scope_hist_swap{ true };
    uint64_t           m_scope_hist_pairs{ 0 };   // pairs swapped back (cumulative)
    void               apply_pair_lock(float* d, int frame, int view_index, bool weapon_view);
    std::atomic<bool>  m_scope{ false };          // cmd scope on|off (master, saved)
    std::atomic<bool>  m_scope_lens{ true };      // cmd scopelens: S1 the lens picture (magnified crop of the eye image)
    std::atomic<bool>  m_scope_reticle{ true };   // cmd scopereticle: S2 reticle at the aim point
    std::atomic<bool>  m_scope_eyebox{ true };    // cmd scopeeyebox: S2 dark crescent when the eye is off the scope's axis
    std::atomic<bool>  m_scope_hide_gun{ true };  // cmd scopehidegun: the weapon pass is not drawn in the scoped eye (it would fill the lens)
    std::atomic<bool>  m_scope_other_eye{ true };   // cmd scopeothereye: in AFW (no scope pass) the picture zooms the other eye's image
    uint64_t           m_scope_other_draws{ 0 };
    void               scope_keep_clean(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img);   // FarCry5Scope.cpp
    std::atomic<int>   m_scope_eye{ 1 };          // cmd scopeeye left|right|auto: the dominant eye
    std::atomic<bool>  m_scope_any{ false };      // cmd scopeany (test): weapons without a scopecal entry use the default entry
    std::atomic<bool>  m_scope_force{ false };    // cmd scopeforce (test): look-through on whatever the eye's distance and angle
    std::atomic<float> m_scope_on_cm{ 15.0f }, m_scope_off_cm{ 20.0f }, m_scope_on_deg{ 8.0f }, m_scope_off_deg{ 12.0f };
    std::atomic<float> m_scope_eyebox_cm{ 2.5f }; // cmd scopeeyeboxcm: eye offset from the axis that darkens half the lens (1 until 2026-10-12: too strict on the gun)
    std::atomic<int>   m_scope_log{ 0 };          // cmd scopelog N: N log lines of the look-through test
    std::mutex         m_scope_mtx{};
    std::unordered_map<UINT, ScopeCal> m_scope_cal{};
    ScopeCal           m_scope_default{};
    ScopeState         m_scope_st{};
    bool               m_scope_on_eye[2]{};
    float              m_scope_angle_eye[2]{ 180.0f, 180.0f };
    std::chrono::steady_clock::time_point m_scope_since{};
    int                m_scope_drawn_frame{ -10 };
    uint64_t           m_scope_frames{ 0 }, m_scope_draws{ 0 }, m_scope_gun_skips{ 0 }, m_scope_fails{ 0 };
    void               scope_frame(int frame, const glm::vec3& eye_cam, const glm::mat3& eye_rot);
    void               scope_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img);
    bool               m_scope_dom_was{ false };   // the dominant eye was behind the scope at its last frame (magnification ramp)
    bool               scope_hides_gun() const { return m_scope.load() && m_scope_lens.load() && m_scope_hide_gun.load() && !scope_on_model() && m_scope_st.active && m_scope_st.frame == m_frame.load(); }
    // scopemodel (default on, under scope; author 2026-10-12): the scope picture on the gun's eyepiece instead of a lens held in front of the eye. The lens is a
    // disc fixed to the gun (the weapon's scopecal entry: its centre in controller axes, its radius), facing back along the
    // scope's axis; each eye pixel on it shows the scene in its own direction, magnified about the axis (FarCry5Scope.h,
    // ModelLens). The gun stays drawn. The game's own scope glass cannot be used: it covers no pixels outside aiming down
    // sights (simulator 2026-10-12, occlusion query). scopesrc 1 (that eye's whole view is the scope's) only works with it off.
    std::atomic<bool>  m_scope_model{ true };
    bool               scope_on_model() const { return m_scope_model.load(); }
    bool               scope_src_eye() const { return m_scope_src.load() == 1 && !m_scope_model.load(); }   // scopesrc 1 in effect (not with the picture on the gun)
    uint64_t           m_scope_model_draws{ 0 };
    std::atomic<bool>  m_scope_lens_debug{ false };   // cmd scopelensdebug (test): the lens disc magenta, the field's edge green
    std::atomic<float> m_scope_relief{ 0.066f };      // cmd scopelenshere [cm]: the eyepiece this far ahead of the eye
    std::atomic<float> m_scope_lens_fwd{ 0.0f };      // cmd scopelensfwd CM: the picture this far in front of the eyepiece (0: on its face)
    std::atomic<float> m_scope_cut_deg{ 20.0f };       // cmd scopecut DEG: the picture is off once the gun is angled this far from the eye's line
    bool               scope_draw_model(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img);
    bool               scope_draw_model_at(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img, const ScopeState& st, const glm::mat4& vrp, const glm::vec3& eye,
                                           const glm::mat3& eye_rot, int fr, int eye_idx, bool held);
    void               scope_draw_held(ID3D11DeviceContext* ctx, ID3D11Texture2D* img, int held, const glm::mat3& rot, const glm::vec3& pos, const glm::vec4& tan);   // AFW held eye
    std::atomic<bool>  m_scope_held{ true };      // cmd scopeheld: in AFW the rebuilt (held) eye gets the scope picture too
    uint64_t           m_scope_held_draws{ 0 };
    // The eyepiece as the game draws it (the author's headset 2026-10-12 16:37, eyepiece 23 cm ahead: a red glow ring on the
    // scope, the picture beside it): the weapon pass's Weapon_ReticleGlow draws cover the scope's glass. In the scoped eye
    // they write a mask instead (depth tested); its centre and size (GPU reduction) place the picture, rolled with the
    // gun. Too few pixels (a close hold: the glass inside the near plane) = the gun-fixed disc. Outside the scoped eye the
    // glow on a magnifying scope is not drawn (a red disc on the eyepiece).
    std::unordered_set<ID3D11VertexShader*> m_glow_vs{};   // vertex shaders reading Weapon_ReticleGlow (under m_ps_mtx)
    int                m_scope_glass_frame{ -10 };   // the eye frame the glass mask was drawn in
    float              m_scope_glass_vp[4]{};        // the weapon pass viewport of that draw
    float              m_scope_glass_wh[2]{};        // the mask's size
    uint64_t           m_scope_glass_draws{ 0 }, m_scope_glow_hidden{ 0 };
    std::atomic<bool>  m_scope_glass{ true };        // cmd scopeglass on|off: the picture placed on the glass as drawn (off = the gun-fixed disc only)
    bool               scope_glow_draw(ID3D11DeviceContext* ctx);   // a weapon draw: true = leave it out
    // Steady while firing (author 2026-10-12 23:19): the glass as drawn
    // kicks with the recoil animation. Its place on the gun is learned instead, while not firing (the stats read back,
    // without waiting, and averaged: centre and radius on the gun-fixed disc's plane, in controller axes), and the
    // picture is drawn on the gun-fixed disc at that place. cmd scopesteady on|off (default on; off = on the glass as drawn).
    struct GlassSnap
    {
        int       frame{ -10 };
        glm::mat4 vrp{ 1.0f };
        glm::vec3 eye{ 0.0f }, lens{ 0.0f }, axis{ 0, 1, 0 }, ctrl_pos{ 0.0f };
        glm::mat3 ctrl_rot{ 1.0f };
        float     vp[4]{}, W{ 1 }, H{ 1 };
        UINT      sig{ 0 };
        bool      steady{ false };   // not firing for a while, gun pointing at the eye
    } m_glass_snap[8]{};
    struct GlassLearn { glm::vec3 off{ 0.0f }; float radius{ 0.0f }; int n{ 0 }; };
    std::unordered_map<UINT, GlassLearn> m_glass_learn{};   // per weapon (render thread)
    // (author 2026-10-13 wanted the opposite: the picture held still while the gun kicks separates from the eyepiece and
    // looks glitchy; default off = it follows the weapon. scoperecoiloff hides it during the recoil animation instead.)
    std::atomic<bool>  m_scope_steady{ false };
    // scopelaunchers on|off (3 Nov, new, saved, default off; F11 Weapon > Scopes; author 16:25): off, a launcher
    // the game's data names (class 7) other than the RAT4 never shows the magnified picture, whatever its scopecal entry (the RPG-7's shipped x3.7 is its
    // aim-down-sights zoom on iron sights) and whatever sight glass it draws; scopelearn reads such a gun as 1x. On: as before.
    std::atomic<bool>  m_scope_launchers{ false };
    bool               scope_no_optic(uint32_t sig) const;   // scopelaunchers: a launcher with iron sights only (FarCry5Scope.cpp)
    std::atomic<bool>  m_scope_recoil_off{ false };   // cmd scoperecoiloff on|off: no magnified picture while firing and 0.6 s after
    std::chrono::steady_clock::time_point m_scope_fire_t{};
    // scopezoomstick (author 24 Oct): each gun's zoom factor on its optic (scopezoom K SIG,
    // saved), in steps of x1, x4, x8, x16, x32.
    std::atomic<bool>  m_scope_zoom_stick{ true };
    std::atomic<float> m_zoom_stick_y{ 0.0f }, m_zoom_stick_x{ 0.0f };   // the right stick, from the pad hook
    std::unordered_map<UINT, float> m_scope_zoom{};   // per weapon (under m_scope_mtx)
    bool               m_scope_zoom_latch{ false };   // a step taken: the stick comes back near the centre before the next
    // Sight glass (a reticle or a scope's glass) drawn in the weapon pass: only a gun drawing it now magnifies (an M60
    // without sights has the weapon id of a scoped one, whose x3.3 entry it used, author 24 Oct). m_optic_this_frame: such
    // a draw in this frame's weapon pass; m_optic_sig / m_optic_frame: the weapon id and frame it was last seen with.
    bool               m_optic_this_frame{ false };
    std::atomic<uint32_t> m_optic_sig{ 0 };
    std::atomic<int>   m_optic_frame{ -1000 };
    bool               optic_now(uint32_t sig) const { return sig != 0 && m_optic_sig.load() == sig && m_frame.load() - m_optic_frame.load() <= 30; }
    std::unordered_set<uint32_t> m_optic_warned{};   // under m_scope_mtx
    float              scope_zoom_tick(UINT sig, float optic, bool active);   // the gun's zoom factor, moved by the stick
    // The scope pass picture fits the lens: drawn at this magnification or lower (wider), or, once the zoom has ramped up,
    // up to 10 % higher (its field has 10 % to spare). Zooming out (scopezoomstick) the picture of a pair or two before is
    // a little narrower; refusing it fell back to the zoom of the eye's own image, the gun's dark scope (author 24 Oct:
    //).
    static bool        scope_pic_mag_ok(float mag, float full_mag, float pic_mag) { return mag >= 0.99f * pic_mag || (mag >= 0.99f * full_mag && mag >= 0.91f * pic_mag); }
    uint64_t           m_glass_samples{ 0 };
    void               scope_glass_learn(ID3D11DeviceContext* ctx);
    // Precision through a sight (author): in VR the game never aims down sights,
    // so it fires with its wide hip-fire spread even through a scope or red dot. While the eye looks through one, the
    // spread is scaled by sightspread (default 0.1, about aimed-down-sights; 1 = off).
    std::atomic<float> m_sight_spread{ 0.1f };
    void               sight_spread_tick(bool through);   // FarCry5Stereo.cpp (the spread hook's globals)
    bool               scope_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed);
    bool               scope_settings_migrate();   // after the settings replay: weapon 62229's release 0.15 default entry dropped (FarCry5Scope.cpp)
    // Red dot sights (FarCry5RedDot.cpp; cmd reddot, off by default). The game paints a reflex or holographic sight's dot
    // on the sight's glass (seen at the glass's distance in VR, off target when the eye is off the sight's axis). With
    // reddot on, the reticle draws write the sight-window mask instead, and each eye gets a dot where the shots land,
    // drawn only where it sees the window. Magnifying scopes (scope entry above x1) are left alone.
    std::atomic<bool>  m_reddot{ true };   // set by reddotmode (F11 Weapon > Sights "Red dot and laser"); cmd reddot: a test, not saved
    std::atomic<float> m_reddot_moa{ 10.0f };     // cmd reddotsize: dot diameter, minutes of arc
    std::atomic<float> m_reddot_bright{ 1.0f };  // cmd reddotbright 0.1-1
    std::atomic<bool>  m_reddot_glow{ true };    // cmd reddotglow: soft glow around the dot
    std::atomic<bool>  m_reddot_only{ false };   // set by reddotmode; cmd reddotonly (a test, not saved): with a reflex sight the dot shows only through its window (no aim marker outside)
    bool               m_reddot_old_lines{ false };   // the settings replay met an aimdot, reddot or reddotonly line (reddot_settings_migrate)
    // reddotmode laser: a gun that draws a sight's glass gets no laser. m_gun_reticle_this_frame: such a draw in this frame's weapon pass;
    // m_gun_no_reticle: frames of the gun drawn without it (it holds while the gun is out of the game's view); m_gun_has_sight: under 30 of them.
    bool               m_gun_reticle_this_frame{ false };
    int                m_gun_no_reticle{ 1000 };
    std::atomic<bool>  m_gun_has_sight{ false };
    // cmd reddotholo (author 30 Oct): a holographic reticle, a
    // ring around the dot like an EOTech's, at the dot's distance, so each eye sees it on the target; reddotring: its diameter, MOA.
    std::atomic<bool>  m_reddot_holo{ true };
    std::atomic<float> m_reddot_ring_moa{ 65.0f };
    std::atomic<float> m_reddot_dist{ 0.0f };    // cmd reddotdist (test, not saved): metres along the barrel, 0 = where the shots land
    std::atomic<int>   m_reddot_log{ 0 };        // cmd reddotlog N
    std::unordered_set<ID3D11PixelShader*> m_reticle_shaders{};   // pixel shaders binding Weapon_Reticle* (under m_ps_mtx)
    struct RedDotSaved
    {
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* dsv{ nullptr };
        ID3D11BlendState*       blend{ nullptr };
        float                   factor[4]{};
        UINT                    mask{ 0xffffffff };
        ID3D11PixelShader*      ps{ nullptr };
        ID3D11ClassInstance*    inst[16]{};
        UINT                    ninst{ 0 };
    } m_reddot_saved{};
    bool               m_reddot_swapped{ false };
    int                m_reddot_mask_frame{ -10 };   // the eye frame the mask was drawn in
    int                m_reddot_drawn_frame{ -10 };  // the eye frame the dot (and the aim square) was drawn in
    float              m_reddot_vp[4]{}, m_reddot_mask_wh[2]{};
    uint64_t           m_reddot_reticle_draws{ 0 }, m_reddot_mask_draws{ 0 }, m_reddot_dots{ 0 }, m_reddot_fails{ 0 };
    void               reddot_begin(ID3D11DeviceContext* ctx);
    void               reddot_end(ID3D11DeviceContext* ctx);
    bool               reddot_excluded();
    bool               reddot_has_sight() const;
    bool               reddot_no_laser();   // reddotmode laser, a gun with a reflex sight: draw_aim_dot draws nothing
    int                reddot_mode() const;   // 0 realistic, 1 laser, 2 arcade, read from m_aim_dot, m_reddot and m_reddot_only
    void               reddot_set_mode(int mode);
    static const char* reddot_mode_name(int mode);
    bool               reddot_settings_migrate();   // after the settings replay: saved aimdot, reddot and reddotonly lines become reddotmode
    void               reddot_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);
    bool               reddot_command(const std::string& cmd, const std::string& arg, bool from_settings, bool& persist_changed);
    // Closed red dot sights (the AR-C's tube sight, author 21 Oct).
    // The game fills the tube with a dark emissive disc (#F20EC748) under a glass reflection layer (#E965FE02), meant to be
    // seen only outside aiming down sights; in VR the game never aims down sights, so the glass stays dark. cmd reddotglass:
    // 0 the game's glass, 1 (default) no dark disc (reflection kept), 2 no dark disc and no reflection. Only on guns whose
    // weapon pass draws a reticle (red dot or holographic sight) and that have no magnifying scope.
    std::atomic<int>   m_reddot_glass{ 1 };
    std::unordered_set<ID3D11PixelShader*> m_sight_tint_ps{}, m_sight_glass_ps{};   // #F20EC748, #E965FE02 (under m_ps_mtx)
    std::atomic<uint32_t> m_reddot_reticle_sig{ 0 };   // the weapon whose weapon pass last drew a reticle
    uint64_t           m_reddot_glass_hidden{ 0 };
    bool               reddot_glass_draw();   // a weapon draw: true = leave it out
    // Sight line aim (FarCry5Sight.cpp; cmd sightaim, author 18 Oct).
    // The game aims down sights by putting the sight's centre on its camera's axis; that pose of the gun (learned per weapon
    // while the game aims down sights) gives the sight line in the gun's own axes. With sightaim on, the shots, the red dot,
    // the aim marker and the scope picture follow that line of the drawn gun instead of the controller's axis.
    struct SightState;
    SightState*        m_sight{ nullptr };
    std::atomic<bool>  m_sight_aim{ true };      // cmd sightaim on|off (saved)
    // sightdepth S (3 Nov, new, saved, default 0.15 s; F11 Weapon > Sights; author 12:45): the
    // distance the red dot and its ring and the scope picture are put at, eased (in 1/distance) over S seconds instead of the barrel ray's raw hit of
    // each frame; the scope picture aimed along the shots' own line (aim_origin, the sight line) instead of from the muzzle. 0 = off: as before.
    // The shots are not changed (m_mo.converge, aim_world).
    std::atomic<float> m_sight_depth{ 0.15f };
    float              m_sd_inv{ 0.0f };         // the eased 1/distance
    uint32_t           m_sd_sig{ 0 };            // the weapon it was eased for
    std::chrono::steady_clock::time_point m_sd_last{};
    float              m_sd_raw_min{ 0.0f }, m_sd_raw_max{ 0.0f }, m_sd_step_max{ 0.0f };   // sightdepth's 5 s log: raw range and the largest raw step (m)
    uint32_t           m_sd_n{ 0 };
    std::chrono::steady_clock::time_point m_sd_said{};
    float              sight_depth_tick(float hit_held, float conv);   // update_motion: m_mo.sight_dist
    // sightdata on|off (2 Nov, default off, saved; F11 next to sightaim; author 13:02, research\2026-11-02_sight_points.md): with no sight glass
    // drawn, the sight line goes through the weapon model's own sight point (FarCry5SightTable.h, 129 weapons) instead of needing a learned line. Off by
    // default: it changes where shots go, and the Shooting checks (CLAUDE.md) have not been run (no simulator on 2 Nov).
    std::atomic<bool>  m_sight_data{ false };
    std::atomic<uint32_t> m_sight_optic_count{ 0 };   // sightdata: the draw of this weapon's optic (FarCry5SightTable.h kOptics), 0 none seen; set at each frame's end
    std::atomic<bool>  m_sight_learn{ true };    // cmd sightlearn on|off: a short aim-down-sights press reads a weapon with no sight line yet
    std::atomic<bool>  m_sight_probe_lt{ false };
    ID3D11Texture2D*   m_aim_depth{ nullptr };   // the scene depth before the weapon pass (the aim march: the held gun is not in it)
    int                m_aim_depth_frame{ -1 };
    std::atomic<bool>  m_aim_clean{ false };     // cmd aimclean on|off (test, not saved): march the depth copied at the weapon pass's first draw
    void               aim_depth_snapshot(ID3D11DeviceContext* ctx);
    std::atomic<int>   m_vel_arm{ -1 };          // velshot D: velscan D frames after the next shot is released (-1 = off)
    int                m_vel_scan_at{ -1 };      // the frame of that scan
    float              m_vel_args[3]{ 200.0f, 1500.0f, 0.995f };   // sightlearn's short aim-down-sights press (read by the pad hook)
    std::atomic<int>   m_sight_log{ 0 };         // cmd sightlog N: N lines of the sight line against the barrel
    std::atomic<bool>  m_sight_eye_test{ false };   // cmd sighteye on|off (test): the test controller puts the gun's sight line on the right eye
    std::atomic<float> m_sight_eye_back{ 0.0f };    // cmd sighteye on B P: the eye B m behind the game's aim-down-sights eye point,
    std::atomic<float> m_sight_eye_pitch{ 0.0f };   // the gun aimed P deg below the eye's forward
    glm::vec3          m_sight_eye_cam[2]{};     // the eyes' cameras (world) and rotations, from the last frame of each
    glm::mat3          m_sight_eye_rot[2]{};
    void               sight_note_pose(uint32_t sig, const glm::mat4& Q, bool new_update, bool calm);   // gun_still: the game's gun pose
    void               sight_finalize(const char* how);   // the sight line from this press's still poses
    bool               sight_line(glm::vec3& o_ctrl, glm::vec3& d_ctrl);   // the drawn gun's sight line, controller axes
    void               sight_motion(const glm::vec3& barrel_origin, glm::vec3& aim_origin, glm::vec3& aim_dir);   // update_motion
    bool               sight_test_ctrl(glm::mat3& rot_world, glm::vec3& pos_world);   // sighteye
    void               sight_tick();             // once per frame (dominant scope eye path): the aim-down-sights probe
    bool               sight_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed);
    bool               gun_still_held(uint32_t sig, glm::mat4& pose);   // FarCry5GunStill.cpp: the weapon's held pose (game camera axes)
    bool               sight_gun_pose(uint32_t sig, glm::mat4& pose);   // the drawn gun's pose (held, or the game's own), game camera axes
    bool               sight_axes(uint32_t sig, glm::vec3& d, glm::vec3& u);   // a learned sight line's direction and up, the gun's axes (gunaxes)
    std::atomic<float> m_hud_distance{ 1.2f }, m_hud_width{ 1.4f }, m_hud_height{ -0.05f };
    std::atomic<bool>  m_hud_screen{ false };     // cmd hudscreen (HUD report A): HUD in the image drawn per eye as a screen at huddist
    std::atomic<float> m_hud_size_deg{ 70.0f };   // cmd hudsize: that screen's width in degrees
    std::atomic<bool>  m_hud_panel_image{ true }; // cmd hudpanel image|layer: the HUD panel drawn into the eye images (no extra headset layer)
    std::unordered_set<ID3D11PixelShader*> m_hud_shaders{};
    std::unordered_set<ID3D11PixelShader*> m_hud_solid_ps{};   // HUD shaders that read no texture (shapes): m_ps_mtx
    // hudprim (default on, saved; author 30 Oct 12:29 F6: a white and a red chevron 8 deg wide on a rock, a wolf to the right,
    // big pixel blocks): the game's other 2D primitive shader with a texture (CScenePrimitive + DiffuseSampler0, #4BA2D4B5 in
    // that frame's census) drew a screen indicator straight into the eye picture at the game's screen size, about 5x too big
    // and magnified. Taken into the HUD with the CSceneFireUi shaders when its target is the HUD image's size. m_ps_mtx.
    std::unordered_set<ID3D11PixelShader*> m_hud_prim_ps{};
    std::atomic<bool>        m_hud_prim{ true };
    uint64_t                 m_hud_prim_draws{ 0 }, m_hud_prim_left{ 0 };   // taken into the HUD / left in the picture (other size), last second
    bool hud_prim_fits(ID3D11DeviceContext* ctx);   // hudprim: the bound target has the HUD image's size
    // hudprimcam (default on, saved; author 30 Oct 13:27 F6 with hudprim): these draws read the world camera's buffer at b0 (census pass M), which holds this eye's camera, so
    // they were placed in the marker image for the eye while the world pass reads that image as the game camera's picture (the
    // red pair shown ~20 deg up and right of the enemies). Taken into the HUD, they get a copy of the game's own upload of that
    // buffer (this call's first world camera upload, before the mod's rewrite), as the flat game draws them.
    std::atomic<bool>        m_hud_prim_cam{ true };
    // hudprimworld (default on, saved; author 8 Nov 08:55): the game's GPS route on the road is drawn with the same textured primitive shader as the
    // detection chevrons (#4BA2D4B5 with the world vertex shader #7CFFEC1A: its first draw of 7 Nov came at 21:17:38, 7 s after he
    // closed the map with a waypoint set), so hudprim took the route off the eye picture into the marker image, where it was shrunk
    // to hudmarksize about its box and cut to hudmarkmax degrees: gone from the road. A chevron is one textured quad per draw (2-3
    // draws a call on 30 Oct); the route is a strip of many points (the game's "fGPSStepLength", "Width of the GPS path"). A hudprim
    // draw of more than this many vertices (count x instances) stays in the eye picture with the eye's own camera, like the world,
    // and in the scope picture too. 0 = off (every hudprim draw is HUD, as before). "hudprimworld on" = 12.
    std::atomic<int>         m_hud_prim_world_min{ 12 };
    uint64_t                 m_hud_prim_world_n[2]{};            // hudprim draws left in the eye picture as world geometry, per eye, last second
    bool hud_prim_is_world(UINT count, UINT instances) const
    {
        const int m = m_hud_prim_world_min.load();
        return m > 0 && count != 0 && (uint64_t)count * (uint64_t)(instances ? instances : 1u) > (uint64_t)m;
    }
    // hudprimkind (log only, always on, at most 40 lines a session): each new kind of hudprim draw (shader, vertex count bucket,
    // instances, topology, target format and size, depth test) once, with where it went, so a run with a waypoint set and an animal
    // noticing the player shows what tells the route from the chevrons.
    void hud_prim_kind_log(ID3D11DeviceContext* ctx, UINT count, UINT instances, bool world);
    std::unordered_set<uint64_t> m_hud_prim_kinds_seen{};
    std::vector<float>       m_hud_game_cb{};                    // the game's own world camera upload of this call
    int                      m_hud_game_cb_frame{ -1 };
    ID3D11Buffer*            m_hud_prim_standin{ nullptr };
    UINT                     m_hud_prim_standin_size{ 0 };
    ID3D11Buffer*            m_hud_prim_cb_orig{ nullptr };
    bool                     m_hud_prim_cb_swapped{ false };
    bool                     m_hud_prim_now{ false };            // this draw is a hudprim draw taken into the HUD
    uint64_t                 m_hud_prim_eye[2]{}, m_hud_prim_mark_n[2]{}, m_hud_prim_panel_n[2]{};   // per eye (L, R): taken, to the marker image, to the panel; last second
    uint64_t                 m_hud_prim_cam_n{ 0 }, m_hud_prim_cam_miss{ 0 };   // given the game's camera / not (no copy of this call, other buffer), last second
    std::unordered_set<const void*> m_hud_prim_vs_seen{};        // their vertex shaders, logged once each (what they read)
    bool hud_prim_cb_swap(ID3D11DeviceContext* ctx);
    void hud_prim_cb_restore(ID3D11DeviceContext* ctx);
    int                      m_hud_mark_widget_frame{ -1 };      // the call whose marker phase has reached a widget (control hints)
    bool                     m_hud_mark_hint_now{ false };       // this marker draw belongs to that widget
    std::atomic<bool>        m_hud_hint_test{ false };           // hudhinttest (test): every marker draw shown as a hint, on the panel
    ID3D11Texture2D*         m_hud_tex{ nullptr };
    ID3D11RenderTargetView*  m_hud_rtv{ nullptr };
    ID3D11ShaderResourceView* m_hud_srv{ nullptr };
    // HUD in the image + DLSS upscaling: the HUD is drawn into its own layer (m_hud_tex) and laid over
    // the eye image, instead of being guessed from the difference with the pre-HUD image (cmd hudlayer).
    std::atomic<bool>        m_hud_layer{ true };
    int                      m_hud_cleared_frame{ -1 };
    bool                     m_hud_drawn{ false };
    int                      m_hud_steady_frames{ 0 };   // consecutive in-game frames (the panel waits for ~1 s of them)
    ID3D11RenderTargetView*  m_hud_game_rtv{ nullptr };
    ID3D11DepthStencilView*  m_hud_game_dsv{ nullptr };
    ID3D11BlendState*        m_hud_game_blend{ nullptr };
    float                    m_hud_blend_factor[4]{};
    UINT                     m_hud_sample_mask{ 0xffffffff };
    bool                     m_hud_redirected{ false };
    std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> m_hud_blends{};
    uint64_t                 m_hud_draws{ 0 };
    bool hud_begin(ID3D11DeviceContext* ctx);
    void hud_abandon();
    bool                     m_hud_pending{ false };   // this draw is a HUD draw: redirect it if it goes ahead
    void hud_end(ID3D11DeviceContext* ctx);
    // hudmul: HUD draws that multiply the picture (the directional hit indicators: colour = dst x src) go on their own image,
    // cleared to white, and that image multiplies the world in compose. On the HUD image (empty, black) they came out black.
    std::atomic<bool>        m_hud_mul{ true };
    ID3D11Texture2D*         m_hud_mul_tex{ nullptr };
    ID3D11RenderTargetView*  m_hud_mul_rtv{ nullptr };
    ID3D11ShaderResourceView* m_hud_mul_srv{ nullptr };
    int                      m_hud_mul_cleared_frame{ -1 };
    uint64_t                 m_hud_mul_draws{ 0 };
    std::atomic<bool>        m_hud_mul_test{ false };          // hudmultest (test): the compass bar drawn as a multiplying draw
    ID3D11BlendState*        m_hud_mul_test_blend{ nullptr };
    // hudmarkers: the game draws its world markers (objective icons, their distance, edge arrows) first, for its own
    // 120 deg camera, then the compass bar (a 692x60 texture) and the other widgets. Those first draws go on their own
    // image, laid into each eye's picture through the game camera, so each marker sits on its target (FarCry5HudWorld.cpp).
    std::atomic<bool>        m_hud_markers{ true };
    std::atomic<float>       m_hud_mark_dist{ 8.0f };    // hudmarkdist: the distance (m) the markers are shown at in stereo (default 8 since 19 Oct, author)
    ID3D11Texture2D*         m_hud_mark_tex{ nullptr };
    ID3D11RenderTargetView*  m_hud_mark_rtv{ nullptr };
    ID3D11ShaderResourceView* m_hud_mark_srv{ nullptr };
    int                      m_hud_mark_cleared_frame{ -1 };
    int                      m_hud_mark_call{ -1 };          // the call the two flags below are for
    bool                     m_hud_mark_bound_seen{ false };  // this call has drawn the compass bar
    bool                     m_hud_mark_prev_bound{ false };  // the call before did (markers are only split when it did)
    uint64_t                 m_hud_mark_draws{ 0 }, m_hud_world_passes{ 0 }, m_hud_mark_scaled{ 0 }, m_hud_mark_over{ 0 }, m_hud_mark_unboxed{ 0 }, m_hud_mark_panel{ 0 };
    // hudmarksize: the markers' size (1 = the game's, which looks far too big in the headset: author 18 Oct 21:13), each
    // marker scaled about its icon. Every marker draw is drawn once more beforehand for its box (hud_mark_box_draw).
    std::atomic<float>       m_hud_mark_size{ 0.21f };   // default 0.21 since 19 Oct (author, his headset value)
    // hudmarksmooth: each marker followed as a world direction from call to call and eased over this many seconds (0 = off;
    // Author 19 Oct). hudmarklag N (test): markers taken as placed with the game camera N calls back.
    std::atomic<float>       m_hud_mark_smooth{ 0.15f };
    std::atomic<int>         m_hud_mark_lag{ 0 };
    std::atomic<int>         m_hud_mark_log{ 0 };   // hudmarklog N (test): one marker's world direction logged for N calls
    // hudmarkdump N (test, off by default since 23 Oct: its GPU read-backs stall frames, the FOV audit): the next N new prompt
    // calls logged with their draws and groups (HUDDump).
    std::atomic<int>         m_hud_mark_dump{ 0 };
    // hudmarkcap (author 29 Oct 14:50 headset picture): the
    // game draws a few markers several times bigger than the rest (a 28-texel icon over a quarter of its picture), so after the
    // common hudmarksize shrink they still covered 10 deg. Each marker is shrunk further until it is at most hudmarkmax degrees
    // across; also a lone icon put on the panel only because it came after a prompt. Last read-back: m_hud_cap_*.
    std::atomic<bool>        m_hud_mark_cap{ true };
    std::atomic<float>       m_hud_mark_max{ 4.0f };
    // hudmarkicon (author 29 Oct 16:00 at 4 deg): the icon alone kept to this many degrees as well.
    std::atomic<float>       m_hud_mark_icon{ 2.0f };
    float                    m_hud_cap_icon_max{ 0.0f };
    float                    m_hud_cap_world_max{ 0.0f }, m_hud_cap_panel_max{ 0.0f };   // largest seen (deg, before the limit), last second
    uint64_t                 m_hud_cap_frames{ 0 };                                    // calls with a marker made smaller, last second
    void hud_mark_log(ID3D11DeviceContext* ctx, const glm::mat4& ginv, float mw, float mh);
    void hud_mark_cap_read(ID3D11DeviceContext* ctx);   // hudmarkcap: its numbers read back for the probe line
    // The draw being hooked: 1 DrawIndexed, 2 Draw, 3 DrawIndexedInstanced, 4 DrawInstanced, 5 DrawIndexedInstancedIndirect,
    // 6 DrawInstancedIndirect, 0 other (DrawAuto: no box). Instanced draws: their instance count and first instance; indirect
    // ones: their arguments buffer and offset (not held).
    int                      m_hud_draw_kind{ 0 };
    UINT                     m_hud_draw_inst{ 0 }, m_hud_draw_start_inst{ 0 };
    ID3D11Buffer*            m_hud_draw_args{ nullptr };
    UINT                     m_hud_draw_args_off{ 0 };
    void hud_mark_box_draw(ID3D11DeviceContext* ctx);
    bool hud_side_target(ID3D11Device* dev, ID3D11Texture2D*& tex, ID3D11RenderTargetView*& rtv, ID3D11ShaderResourceView*& srv, const char* what);
    bool hud_mul_target(ID3D11Device* dev) { return hud_side_target(dev, m_hud_mul_tex, m_hud_mul_rtv, m_hud_mul_srv, "multiply image (hit indicators)"); }
    // After the HUD (DLSS compose or the back buffer): this call's multiplying HUD draws and world markers into its eye picture.
    void hud_world_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);
    // hudscan N: every HUD draw of the next N game calls logged (shaders, texture, blend, scissor, the VS's constants):
    // research for telling the world markers from the HUD's widgets.
    std::atomic<int>         m_hud_scan_left{ 0 };
    int                      m_hud_scan_frame{ -1 }, m_hud_scan_idx{ 0 };
    UINT                     m_hud_draw_count{ 0 }, m_hud_draw_start{ 0 };
    INT                      m_hud_draw_base{ 0 };
    void hud_scan_draw(ID3D11DeviceContext* ctx);
    // The game camera's own view-rotation-projection of this call (world pass, before the eye rewrite): the HUD's world
    // markers are placed by the game for it.
    glm::mat4                m_hud_game_vrp{ 1.0f };
    int                      m_hud_game_vrp_frame{ -1 };
    std::atomic<float> m_grip_off[3]{ 0.10f, -0.10f, -0.28f };   // the weapon's grip in the game camera's view (m)
    std::atomic<float> m_barrel_off[3]{ 0.0f, 0.03f, -0.30f };   // the muzzle from the grip, controller axes (m)
    std::atomic<float> m_grip_pitch{ -5.0f };   // grippitch (saved; -5 default, author 30 Oct 15:09)
    std::atomic<float> m_model_yaw{ 0.0f }, m_model_pitch{ 0.0f };
    std::atomic<float> m_converge_m{ 25.0f };
    std::atomic<bool>  m_converge_auto{ true };
    std::atomic<float> m_aim_gain{ 0.7f };
    std::atomic<bool>  m_trigger{ false }, m_dbg_trigger{ false };
    std::atomic<float> m_fire_gate_deg{ 0.3f };   // sim 2026-10-10: 0.75 -> release error mean 0.35 / max 0.60 deg; 0.3 -> 0.09 / 0.17 deg, +37 ms
    std::atomic<bool>  m_stick_aim{ false };
    std::atomic<float> m_stick_x{ 0.0f }, m_stick_y{ 0.0f }, m_stick_gain{ 0.065f };
    int                m_mouse_failures{ 0 };   // camera-not-following pauses since the mouse last turned the camera (2 = stick aim fallback)
    std::atomic<bool>  m_stick_auto{ false };   // stick aim switched on by that fallback, not by hand (F11 and cmd aimstick clear it)
    std::chrono::steady_clock::time_point m_stick_auto_since{};
    std::atomic<int>   m_stick_retry_s{ 10 };   // cmd aimstickretry N (tester 28 Oct): the fallback tries the mouse again after N s with the trigger up (0 = never)
    std::atomic<int>   m_fire_wait_max_ms{ 250 };   // cmd firewaitmax N (tester 28 Oct): a held trigger goes through after N ms even off the gate, aim error under 5 deg (0 = never)
    std::atomic<bool>  m_mouse_drop{ false };   // cmd mousedrop (test): the aim loop's mouse moves are dropped, as when the game ignores the mouse
    bool               m_fire_open{ false }, m_trigger_was{ false }, m_mouse_fire_down{ false };
    std::chrono::steady_clock::time_point m_trigger_since{};
    std::atomic<float> m_dbg_ctrl[3]{ 0.15f, -0.25f, -0.35f };   // flat test: the simulated controller (reference space, m)
    std::atomic<float> m_dbg_ctrl_yaw{ 0.0f }, m_dbg_ctrl_pitch{ 0.0f };
    // Weapon smoothing (option "weaponsmooth" 0..1, 0 = off): an adaptive (One Euro) filter on the weapon
    // controller's pose - steady when the hand is slow, responsive on fast swings.
    std::atomic<float> m_wsmooth{ 0.1f };   // Author 13 Oct: 0.1
    // Field of view: each eye's own angles (cmd fovexact, saved) and, with fovsize faster, the eye image
    // shrunk to the exact area at the next start (same sharpness as before, fewer pixels).
    std::atomic<bool>  m_fov_exact{ true };
    std::atomic<bool>  m_fov_faster{ false };
    bool               m_fov_logged{ false };
    float              m_cammv_sign{ 1.0f };
    bool               m_ws_init{ false };
    glm::vec3          m_ws_pos{ 0.0f }, m_ws_dpos{ 0.0f };
    glm::quat          m_ws_rot{ 1.0f, 0.0f, 0.0f, 0.0f }, m_ws_raw_prev{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3          m_ws_wvel{ 0.0f };
    float              m_ws_dang{ 0.0f };
    std::chrono::steady_clock::time_point m_ws_last{};
    std::atomic<float> m_dbg_ctrl_jitter{ 0.0f };   // flat test: random yaw/pitch noise on the simulated controller (deg)
    std::atomic<float> m_dbg_ctrl_sway{ 0.0f };     // ctrlsway A [P] (test): the simulated controller yaws A deg * sin(frame / P), a smoothly moving gun
    std::atomic<int>   m_dbg_ctrl_sway_period{ 60 };
    std::atomic<int>   m_ws_log{ 0 };               // cmd wsmoothlog N: raw vs smoothed yaw for N frames
    void               smooth_weapon(glm::mat4& k);
    // gunwobble (research 2026-11-04_gunwobble). weaponsteady (saved, default on): the weapon smoothed 4 times harder while the
    // hand is still (cutoff at rest 0.25 x weaponsmooth's), with a quicker speed estimate (3 Hz) and a steeper rise with speed, so
    // a real move opens the filter at once (less lag than without it from about 5 deg/s up).
    std::atomic<bool>  m_ws_steady{ true };
    // steadylog (not saved, default on): every 10 s with a gun in hand and the hand still, one [Steady] line (the controller as
    // read, as smoothed, the two eyes of a pair) and the gunstillstat lines (the gun hold, per eye; gunstilllate's counts).
    std::atomic<bool>  m_steady_log{ true };
    glm::mat4          m_ws_raw_in{ 0.0f };
    bool               m_ws_have_raw{ false };
    void               smooth_weapon_step(glm::mat4& k);
    void               steady_note(const glm::mat4& raw, const glm::mat4& out, bool same);
    struct SteadyStat
    {
        bool                                  have{ false };
        glm::mat4                             raw_prev{ 1.0f }, out_prev{ 1.0f };
        std::chrono::steady_clock::time_point t_prev{}, t0{};
        uint64_t                              n{ 0 }, moving{ 0 }, pairs{ 0 }, pairs_apart{ 0 };
        double                                raw_mm{ 0.0 }, raw_deg{ 0.0 }, raw_pt{ 0.0 }, out_mm{ 0.0 }, out_deg{ 0.0 }, out_pt{ 0.0 }, eye_pt{ 0.0 };
        float                                 raw_pt_max{ 0.0f }, out_pt_max{ 0.0f }, eye_pt_max{ 0.0f };
    } m_sty;
    std::atomic<bool>  m_test_ctrl{ false };
    std::atomic<bool>  m_snap_requested{ false };
    std::atomic<int>   m_dlss_reset_eyes{ 0 };   // bit per eye: reset that eye's DLSS history at its next evaluation
    // Bullet tracer streaks (#7B0DD5A7, drawn in the weapon pass): hidden on request (hidetracers).
    std::unordered_set<ID3D11PixelShader*> m_tracer_shaders{};   // every shader object with that bytecode
    uint64_t           m_tracer_hidden{ 0 };
    std::atomic<bool>  m_hide_tracers{ false };
    std::unordered_set<ID3D11PixelShader*> m_world_tracer_shaders{};   // #FD85674E, the emissive streak (world pass)
    uint64_t           m_world_tracer_hidden{ 0 };
    uint64_t           m_tracer_hidden_other{ 0 };
    std::atomic<bool>  m_show_world_tracers{ true };   // world-pass muzzle flash/smoke/spark copies: shown ("worldtracers off" hides them, test)
    std::atomic<int>   m_tracer_dump{ 0 };
    std::atomic<int>   m_tracer_diff{ 0 };
    std::atomic<int>   m_tracer_probe{ 0 };
    std::atomic<int>   m_snap_pair{ 0 };
    ID3D11Resource*    m_cur_dsv_res{ nullptr };
    UINT               m_cur_rtv_n{ 0 };
    int                m_cur_dsv_slice{ -1 };              // sunshare: the bound depth view's array slice (cascade target only)
    ID3D11DepthStencilView* m_hidmask_dsv{ nullptr };      // hidmask: the scene depth view the hidden area was stamped into (held)
    int                m_hidmask_frame{ -1 };              // the frame (eye call) it was stamped in
    bool               m_hidmask_restored{ false };        // hidmask restore: far depth already put back this call
    float              m_hidmask_tan[2][4]{};              // the last known eye image field (tangents l r u d) per eye
    bool               m_hidmask_tan_ok[2]{};
    bool               hidmask_stamp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, float z);
    struct DepthStat { int draws{ 0 }, depth_only{ 0 }, call{ 0 }; };
    std::unordered_map<ID3D11Resource*, DepthStat> m_depth_census{};
    std::atomic<int>   m_depth_census_frames{ 0 };
    std::unordered_set<ID3D11Resource*> m_shadow_targets{};
    std::atomic<int>   m_skip_shadow{ 0 };
    // Sun shadow targets found automatically (for scopeskip 1; shadowshare itself was removed in the 19 Oct switch audit).
    std::unordered_set<ID3D11Resource*> m_shadow_auto{};   // refreshed every 5 s (re-learned in the next game call)
    ID3D11Resource*    m_shadow_last_res = nullptr;
    ID3D11Resource*    m_ss_target = nullptr;   // the first one found (held)
    bool               m_shadow_last_ok  = false;
    uint64_t           m_shadow_draws_skipped{ 0 };
    std::atomic<bool>  m_menu_blit{ true };          // main menu background stretched over the image (menublit)
    std::atomic<bool>  m_menu_whole{ true };         // menuwhole: menu, video and loading pictures shown whole (blurred bands), not cut to the tall image
    bool               m_menu_blit_pending{ false };
    ID3D11ShaderResourceView* m_menu_yuv[3]{};         // the video frame's planes (luma, Cr, Cb) seen at its draw, for menu_blit
    ID3D11Buffer*             m_menu_yuv_cb{ nullptr };   // that draw's pixel constants (its fade)
    bool                      m_menu_yuv_fire{ false };   // drawn by the menu UI shader (else the scene one)
    bool                      m_menu_overlay_next{ false };   // the next draw may be the main menu's darkening quad over its video
    std::atomic<bool>         m_menu_unclip{ true };   // menuunclip: menus clipped to the 16:9 corner shown whole
    int                       m_menu_unclip_logged{ 0 };
    void                      menu_unclip(float* data, uint32_t size);
    int                       m_menu_overlay_logged{ 0 };
    void               menu_blit(ID3D11DeviceContext* ctx);
    std::unordered_map<ID3D11VertexShader*, std::string> m_vs_info{};
    std::unordered_set<ID3D11PixelShader*> m_tracer_mesh_shaders{};    // #7FA4FD7E: draws the streak mesh again (8-bit buffer)
    std::unordered_set<ID3D11PixelShader*> m_streak_colour_shaders{};  // #970A262A: the streak's colour draw
    ID3D11Buffer*      m_tracer_vb{ nullptr };
    ID3D11Buffer*      m_tracer_ib{ nullptr };
    int                m_tracer_mesh_frame{ -1000 };
    uint64_t           m_tracer_mesh_hidden{ 0 };
    // Tracers from the gun (streak_cb_swap): the last world camera upload as written + the game camera.
    std::atomic<bool>  m_tracers_from_gun{ false };
    std::atomic<float> m_streak_fixed_dist{ 40.0f };
    std::vector<float> m_streak_cb{};
    int                m_streak_cb_frame{ -1000 };
    glm::vec3          m_streak_game_cam{}, m_streak_game_right{}, m_streak_game_up{}, m_streak_game_fwd{}, m_streak_view_cam{};
    glm::mat4          m_streak_vp{ 1.0f }, m_streak_vrp{ 1.0f }, m_streak_view{ 1.0f };
    glm::vec3          m_streak_debug_from{};
    bool               m_streak_debug_from_set{ false };
    ID3D11Buffer*      m_streak_standin{ nullptr };
    UINT               m_streak_standin_size{ 0 };
    ID3D11Buffer*      m_streak_cb_orig{ nullptr };
    bool               m_streak_cb_swapped{ false };
    uint64_t           m_streak_moved{ 0 };
    bool               streak_cb_swap(ID3D11DeviceContext* ctx);
    void               streak_cb_restore(ID3D11DeviceContext* ctx);
    std::vector<std::string> m_test_skip_patterns{};
    int                m_streak_test{ 0 };
    void               set_streak_test(int group);
    std::vector<std::string> m_reload_skip_patterns{};   // reloadcasinghide (manualreload51): the spent casings, while the hand ejects (m_ps_mtx)
    bool               m_casing_hidden{ false };
    void               casing_hide_tick();               // hk_frame: follows fc5::reload::hides_game_casings()
    // Two-handed hold (the other hand's grip near the weapon's fore-end): state, switches, flat-test hand.
    std::atomic<bool>  m_two_hand{ false };
    std::atomic<bool>  m_two_hand_enabled{ true };
    // twohandblock (author 21 Oct 17:52): weapons that are
    // never held with two hands (by weapon signature; by default the Uzi, 35187: the SMG with the open reflex sight, the last gun of his 22 Oct 16:04 log; 29850 was the handgun). F11 Controls: a checkbox for the weapon in hand.
    std::set<uint32_t> m_two_hand_block{ 35187u };   // under m_weapon_cal_mtx
    bool               two_hand_blocked(uint32_t sig);
    // twohandgame on|off (new, default on, saved; F11 Controls; author 2 Nov 13:02): a machine pistol by the game's own weapon data (one-handed and class 3: SMG-11 = the Uzi, A-99, Skorpion, AJM9, Stinger; FarCry5GameWeapon.cpp)
    // is never held with two hands, whatever its weapon id. Handguns (class 1, also one-handed in the data) keep the two-handed hold, as he chose on 21 Oct.
    std::atomic<bool>  m_two_hand_game{ true };
    bool               two_hand_game_blocked(uint32_t sig) const;
    std::string        two_hand_block_line();   // "twohandblock a b c" for the settings file
    std::atomic<float> m_fore_grip{ 0.25f };      // the fore-end grab point, m along the barrel from the weapon hand
    // gripnearweapon on|off (new, default on, saved; F11 Controls under the two-handed boxes; author 5 Nov 16:45): a press of the other hand's grip within
    // gripneardist m (default 0.20, saved) of the weapon in hand, from 10 cm behind the weapon hand to 10 cm past the fore-end grab point, is the
    // weapon's whether it grabs or not (twohandblock, machine pistols, twohand off, a hold that ends during the press): until it is released it
    // is not the D-pad modifier (a stick as the D-pad, the grip combos), so it stays free for the reloads. Not on the sling, with the bow, in a
    // vehicle or with no weapon drawn. Decided at the press with the two-handed hold (update_motion). Off: as before 5 Nov.
    std::atomic<bool>  m_grip_near_weapon{ true };
    std::atomic<float> m_grip_near_dist{ 0.20f };
    // dpadear on|off (new, default on, saved; F11 next to gripnearweapon; author 7 Nov 14:08): the left grip is the modifier (a stick as
    // the D-pad, + right B throw / rockets, + menu map, + left stick click flat screen, and B as the throw for reloadmanual) only for a press that
    // starts with the left controller within dpadeardist m (default 0.15, saved, 0.05-0.40) of the left side of the head: a line from just ahead
    // of the left eye to the left ear. Until it is let go. A press anywhere else is never the modifier (it grabs, holds, brakes or descends as
    // before) and a press at the ear never brakes or descends. On foot and in vehicles. Off: any left grip press that is not the weapon's is the
    // modifier (before 7 Nov).
    std::atomic<bool>  m_dpad_ear{ true };
    std::atomic<float> m_dpad_ear_dist{ 0.15f };
    std::atomic<float> m_dpad_ear_last{ -1.0f };   // the last press: its distance from the zone's line (m; -1: not tracked), for F11
    std::atomic<bool>  m_other_near{ false };     // gripnearweapon: the current press of the other hand's grip started near the weapon
    std::atomic<bool>  m_other_grip{ false };     // the other hand's grip button (from the pad mapping)
    bool               m_other_grip_was{ false };
    // Presses of the other hand's grip (counted by set_other_grip) and the last one update_motion has decided on.
    std::atomic<uint32_t> m_other_press{ 0 }, m_other_press_seen{ 0 };
    bool               m_other_have{ false };
    glm::vec3          m_other_pos_ref{ 0.0f };
    glm::mat3          m_other_rot_ref{ 1.0f };
    // Floating left hand: the weapon pass's game camera and weapon placement of the current frame, the gloves'
    // left-hand bones (handbones a b), on/off (hands on).
    struct HandMap { int frame{ -1 }; glm::mat3 rot{ 1.0f }; glm::vec3 cam{ 0.0f }; glm::mat3 w_rot{ 1.0f }; glm::vec3 w_pos{ 0.0f }, grip{ 0.0f }; glm::mat3 corr{ 1.0f };
                     glm::vec3 grip0{ 0.0f }; bool empty{ false };    // grip0: the grip point put on the controller; empty: no weapon in hand
                     glm::mat3 cr{ 1.0f }; glm::vec3 ct{ 0.0f };   // gunstill's steadying of this pass (q -> cr q + ct), for the sidearm's recordings
                     glm::mat3 fix{ 1.0f }; };   // bowupright: the bow's turn in the hand (steadied q; world = w_pos + ctrl corr fix (cr q + ct - grip0))
    HandMap            m_hand_map{};
    std::atomic<bool>  m_hands_float{ true };
    std::atomic<int>   m_left_bone_lo{ 1 }, m_left_bone_hi{ 63 };
    uint64_t           m_hands_moved{ 0 };
    std::atomic<float> m_dbg_other_rot[3]{};
    std::atomic<int>   m_hand_axis_log{ 0 };
    std::atomic<bool>  m_scan_fast{ true };
    std::atomic<int>   m_sslr_log{ 0 };
    std::unordered_map<ID3D11ComputeShader*, int> m_sslr_reproj_cs{};   // reflection passes reading ReprojectionMatrix -> CSSLR slot
    bool               m_sslr_reproj_pending{ false };
    int                m_sslr_reproj_pending_slot{ 1 };
    void               probe_sslr_cs(ID3D11DeviceContext* ctx, UINT slot);
    // Reflections (option "reflections", off by default): the reflection draws are shown, and the passes
    // that blend them over time get each eye's own reprojection (ssr_* below).
    std::atomic<bool>  m_reflections{ true };   // default on since 17 Oct (author), F11 Debug > Experimental
    std::atomic<bool>  m_ssr_eye{ true };      // cmd ssreye: per-eye reprojection while reflections are on
    struct SsrCams { bool valid{ false }; glm::mat4 game_vrp{ 1.0f }, game_pvrp{ 1.0f }, eye_vrp{ 1.0f }, eye_pvrp{ 1.0f }; glm::vec3 game_cam{}, game_pcam{}, eye_cam{}, eye_pcam{}; };
    SsrCams            m_ssr{};
    int                m_ssr_form{ -1 };       // the game's ReprojectionMatrix form, once recognised
    int                m_ssr_tries{ 0 };
    int                m_ssr_next_try{ 0 };
    // ssrretry (default on, F11 Debug > Mandatory > Water): the form is looked for in both calls of a pair (an odd step; every
    // 30 calls landed on the call whose game camera never moves, so it took minutes), never while the menu or map is on the
    // flat screen (its camera fits no form), and a failed round of 8 is tried again 1200 calls later instead of never.
    std::atomic<bool>  m_ssr_retry{ true };
    int                m_ssr_rounds{ 0 };
    std::atomic<int>   m_snap_burst{ 0 };   // snappairs N: N snappairs of consecutive pairs
    uint64_t           m_ssr_swaps{ 0 };
    uint64_t           m_indirect_dispatches{ 0 };
    ID3D11Buffer*      m_ssr_standin{ nullptr };
    ID3D11Buffer*      m_ssr_matbuf{ nullptr };
    ID3D11Buffer*      m_ssr_saved_cb{ nullptr };
    UINT               m_ssr_saved_first{ 0 }, m_ssr_saved_num{ 0 }, m_ssr_saved_slot{ 0 };
    void               ssr_detect(ID3D11DeviceContext* ctx, UINT slot);
    bool               ssr_swap(ID3D11DeviceContext* ctx, UINT slot);
    void               ssr_restore(ID3D11DeviceContext* ctx);
    void               set_reflections(bool on, bool save);
    std::vector<glm::vec4> m_glove_cent{};   // per palette bone: weight, rest-pose vertex centroid (raw units)
    bool               m_glove_scanned{ false };
    std::atomic<bool>  m_dbg_other_set{ false }, m_dbg_other_grip{ false };
    std::atomic<float> m_dbg_other[3]{ 0.15f, -0.2f, -0.6f };
public:
    void set_other_grip(bool down) {
        if (down && !m_other_grip.load()) ++m_other_press;
        m_other_grip = down;
    }
    // The two-handed hold has decided on the current press of the other hand's grip (grab or no grab).
    bool other_grip_decided() const { return m_other_press_seen.load() == m_other_press.load(); }
    // padsim (test command): grips and sticks for the pad mapping (ModConfig), the physical left / right controller.
    struct PadSim
    {
        bool  on, lgrip, rgrip, old;
        float l[2], r[2];
    };
    PadSim pad_sim() const {
        return { m_padsim.load(), m_padsim_lgrip.load(), m_padsim_rgrip.load(), m_padsim_old.load(), { m_padsim_l[0].load(), m_padsim_l[1].load() },
                 { m_padsim_r[0].load(), m_padsim_r[1].load() } };
    }
private:
    std::atomic<bool>  m_padsim{ false }, m_padsim_lgrip{ false }, m_padsim_rgrip{ false }, m_padsim_old{ false };
    std::atomic<float> m_padsim_l[2]{ 0.0f, 0.0f }, m_padsim_r[2]{ 0.0f, 0.0f };
public:
    bool two_hand_held() const { return m_two_hand.load(); }
    bool other_grip_near_weapon() const { return m_other_near.load(); }   // gripnearweapon: this press of the other grip is the weapon's, not the D-pad's
    void note_zoom_stick(float x, float y) { m_zoom_stick_x = x; m_zoom_stick_y = y; }   // scopezoomstick: the right stick on foot
    bool left_handed() const { return m_left_handed.load(); }
    // rumble (author 24 Oct, tester): the game's pad rumble (motors 0-1) as amplitudes for the
    // left and right controllers (amp[0], amp[1]), by hand role. Called from the XInputSetState hook.
    void rumble_hands(float left_motor, float right_motor, float amp[2]);
    // shotbuzz (author 24 Oct 21:25): each of the player's shots gives the
    // hands that hold the gun one fixed pulse of their own (game thread, from the shot code; fc5::bullet).
    void shot_buzz();
    // recoil (FarCry5Recoil.cpp): this game update's kick for the gun in hand (the weapon pass and recoilaim's shots alike).
    bool recoil_kick_now(int frame, glm::mat3& R, glm::vec3& T);
private:
    std::atomic<bool>    m_shot_buzz{ true };           // cmd shotbuzz on|off (saved): the fixed pulse on every shot (off: only the game's rumble)
    std::atomic<float>   m_shot_buzz_amp{ 1.0f };       // cmd shotbuzzamp A (saved, 0.1-1): its strength (1 = the controller's strongest)
    std::atomic<int>     m_shot_buzz_ms{ 80 };          // cmd shotbuzzms N (saved, 20-250): its length
    std::atomic<int64_t> m_shot_buzz_last{ 0 };         // steady_clock ns of the last pulse (the pellets of one shot give one pulse)
    std::atomic<int64_t> m_shot_buzz_until{ 0 };        // steady_clock ns: the pulse's end; until then the game's weaker rumble is held back
    std::atomic<int>   m_rumble_mode{ 1 };          // cmd rumble game|gun|both (saved): 0 the pad's left motor on the left controller, its right one on the right (the old way); 1 the weapon hand, plus the other hand while it holds the gun (both hands with no weapon in hand or driving); 2 both hands
    std::atomic<float> m_rumble_gain{ 1.0f };       // cmd rumblegain K (saved, 0-2): strength (0 = no vibration from the game)
    std::atomic<bool>  m_rumble_armed{ false };     // gesture_poll: a weapon in hand (not on the sling), on foot
    std::atomic<bool>  m_rumble_log{ false };       // cmd rumblelog on|off (not saved): [Rumble] log lines, the motors and what each hand got
    // AFW: each eye's camera at its last capture; held views closer than afwmin degrees (and 3 mm) are not rebuilt.
    fc5::afw::Camera   m_afw_captured[2]{};
    int                m_afw_cap_frame[2]{ -100, -100 };
    struct AfwRec { int frame{}; int eye{}; int src{}; int img_frame{}; int pose_slot{}; float fov_err{}; const char* why{}; };
    std::array<AfwRec, 1024> m_afw_recs{};
    size_t             m_afw_rec_n{ 0 };
    uint64_t           m_afw_src_count[4]{}, m_afw_age_bad{ 0 }, m_afw_slot_bad{ 0 }, m_afw_fov_bad{ 0 };
    void afw_record(int frame, int eye, int src, int img_frame, int pose_slot, float fov_err, const char* why);
    void afw_dump();
    bool set_game_patch(int which, bool on);
    glm::vec3 steady_cam(int frame, const glm::vec3& cam);
    std::atomic<bool>  m_steady_view{ false };    // steady view while shooting (cmd steadyview)
    std::atomic<bool>  m_grip_held{ false };     // the weapon hand's grip (from the pad mapping)
    glm::vec3          m_body_root{ 0.0f }, m_body_root_prev{ 0.0f }, m_body_root_prev2{ 0.0f };
    int                m_body_root_frame{ -100 }, m_body_root_prev_frame{ -100 }, m_body_root_prev2_frame{ -100 };
    // rootsteady on|off (default on, saved; F11 Debug > Head tracking and view; author 29 Oct 11:15): the body root the eyes stand on (viewroot across the ground, walkbob's height) read from its last three samples, not
    // the last two. His log: the samples came with long and short steps in turn, and the prediction from the last two tripled that,
    // so the eyes' base moved 3 and 16 cm a pair in turn (2 and 12 m/s) at a steady run. Off: the last two, as before.
    std::atomic<bool>  m_root_steady{ true };
    bool               root_steady_at(float at, glm::vec3& out) const;
    int                m_steady_pair{ -100 };
    glm::vec3          m_steady_result{ 0.0f }, m_steady_off{ 0.0f };
    bool               m_steady_locked{ false };
    std::chrono::steady_clock::time_point m_steady_last_held{}, m_steady_release{};
    float              m_steady_shift{ 0.0f }, m_steady_shift_max{ 0.0f };
    uint64_t           m_steady_locks{ 0 };
    // Research 15 Oct (gun doubles and lags while walking with it held; F11 Experimental > Research 15 Oct):
    // gunanchor (off): while the steady view applies, the drawn gun, both hands and the slung weapon hang on the eyes'
    // base (steady_cam) instead of the game camera; shots (barrel_origin, the aim) keep the game camera.
    // steadyexact (off): steady_cam takes this update's camera less the camera's offset from the body root measured in
    // the root's own game update, instead of the root moved on by its last speed per frame count.
    // gunqlog N: N lines, one per game update: time, camera and eyes' base steps, camera minus base along the walk.
    std::atomic<bool>  m_gun_anchor{ true };     // default on since 16 Oct (author: the gun stays stable walking with both on)
    std::atomic<bool>  m_steady_exact{ true };   // default on since 16 Oct
    std::atomic<int>   m_gunq_log{ 0 };
    glm::vec3          m_body_root_camoff{ 0.0f };   // game camera minus the body root, same game update (steadyexact)
    bool               m_body_root_camoff_ok{ false };
    int                m_world_cam_frame{ -100 };    // the frame m_world_cam_pos was uploaded for
    bool               m_steady_applied{ false };    // the last upload's eyes stood at steady_cam's result
    int                m_gunq_pair{ -100 };
    glm::vec3          m_gunq_cam{ 0.0f }, m_gunq_base{ 0.0f };
    std::chrono::steady_clock::time_point m_gunq_t{};
    double             m_gunq_sum{ 0.0 };            // per-second log: eyes' base to game camera while holding
    float              m_gunq_max{ 0.0f };
    uint32_t           m_gunq_n{ 0 };
    void               gun_anchor_apply(int frame, const glm::vec3& cam, const glm::vec3& base_cam, bool weapon_view);
    // Research 15 Oct, shadows and water (same F11 section):
    // shadowmv (off): the temporal sun-shadow resolve (#2FFF7035) reads a MotionBuffer copy in which every still point carries
    // this eye's exact move (ctp_full, aoexact's still-point rule), its previous exact copy as PrevMotionBuffer, and its own whole
    // previous output as history (once per call, 0x1000); where both held at the eye's last call the least share of the current
    // frame is shadowmvnew (default 0.1) instead of shadownew.
    std::atomic<bool>  m_shadow_mv{ true };      // default on since 16 Oct (author: the shadow switches improve the picture)
    std::atomic<float> m_shadow_mv_new{ 0.05f };   // The author's 16 Oct choice
    struct ShadowResolveCs { UINT mv{ 255 }, prev_mv{ 255 }, depth{ 255 }, hist{ 255 }; bool temporal{ false }; bool ssr{ false }; uint32_t crc{ 0 }; };   // ssr: a reflection pass (ssrmv)
    std::unordered_map<ID3D11ComputeShader*, ShadowResolveCs> m_smv_cs;   // every sun-shadow resolve variant (m_ps_mtx)
    ID3D11ComputeShader*       m_smv_shader{ nullptr };
    ID3D11Buffer*              m_smv_cb{ nullptr };
    bool                       m_smv_failed{ false };
    ID3D11Texture2D*           m_smv_tex[2][2]{};   // [eye][copy]: exact motion in the game's 8-bit code (R8G8_UNORM)
    ID3D11ShaderResourceView*  m_smv_srv[2][2]{};
    ID3D11UnorderedAccessView* m_smv_uav[2][2]{};
    UINT                       m_smv_w{ 0 }, m_smv_h{ 0 };
    int                        m_smv_built[2][2]{ { -100, -100 }, { -100, -100 } };   // frame each copy was built for
    int                        m_smv_cur[2]{ 0, 0 };
    int                        m_smv_hist_frame[2]{ -100, -100 };   // frame this eye's resolve got its own previous output
    uint64_t                   m_smv_runs{ 0 }, m_smv_refused{ 0 }, m_smv_floor{ 0 };
    std::map<uint32_t, uint64_t> m_smv_by_crc;      // resolve dispatches per shader since the last log line
    struct SmvBound { bool on{ false }; UINT mv{ 255 }, prev{ 255 }; ID3D11ShaderResourceView* saved[2]{}; };
    SmvBound           smv_bind(ID3D11DeviceContext* ctx);
    void               smv_unbind(ID3D11DeviceContext* ctx, SmvBound& b);
    bool               smv_build(ID3D11DeviceContext* ctx, const ShadowResolveCs& r, int eye, int frame);
    // shadowkernel (off): the resolve's 3x3 filter weights (TAASamplesData c25-33 .w) set to a symmetric kernel (centre 0.25,
    // edges 0.125, corners 0.0625) instead of the copy frozen from one old game frame.
    std::atomic<bool>  m_shadow_kernel{ true };   // on by default (author 19 Oct, HEADSET_QUEUE 62)
    // prevmvonce (off): PrevMotionBuffer (0x100) takes the once-per-call history path (key mask 0x1F00): with one swap per
    // dispatch the second dispatch stored this eye's own copy as the other eye's.
    // jitterlog N: N lines per eye: DLSS jitter, the game's jitter, the shadow resolve's 3x3 weights (game's, written) and
    // TAAParams; the reflection pass's TAASamplesData and TAAFrameIndex per call ([SSLR] lines, through sslrlog).
    std::atomic<int>   m_jitter_log{ 0 };
    float              m_jit_game_px[2]{};          // the game's own jitter in this upload's projection (render pixels)
    // ssrjitterpair (off, with reflections): the right eye's reflection passes get the left call's TAASamplesData (c4-c12) and
    // TAAFrameIndex (c18.z), so both eyes run the same reflection sample pattern each pair.
    std::atomic<bool>  m_ssr_jitter_pair{ true };   // default on since 17 Oct (author), F11 Debug > Experimental
    ID3D11Buffer*      m_ssr_pair_cb{ nullptr };     // the left call's CSSLRContextProvider copy (352 B)
    int                m_ssr_pair_frame{ -100 };
    uint64_t           m_ssr_pair_copies{ 0 };
    uint32_t           m_ssr_pair_stores{ 0 }, m_ssr_pair_miss{ 0 }, m_ssr_swaps_sec{ 0 }, m_ssr_seen_sec{ 0 }, m_ssr_pereye_sec{ 0 }, m_ssr_fail[4]{};   // per-second line: left-call stores, right calls without a stored left, swaps
    // ssrmarker (debug, off): the reflection resolve's output cleared to magenta at the call's first resolve dispatch, so tiles no
    // tile class writes show. ssrptrlog N: N lines of the reflection resolve's history and output textures per dispatch.
    std::atomic<bool>  m_ssr_marker{ false };
    // ssrtileclear: at the call's first reflection resolve dispatch the output starts empty (all 0), so tiles no tile class writes
    // this call show no reflection (19 Oct: ssrtilefix, the copy of this eye's last output, folded in and removed). The copy kept an old picture at the same pixels until a tile class writes there again: reflection squares frozen on the
    // screen while the head turns (tester's Pimax video and the author's crates, 18 Oct).
    std::atomic<bool>  m_ssr_tile_clear{ true };   // default on, F11 Debug > Mandatory > Water (author 19 Oct)
    uint64_t           m_ssr_tile_clears{ 0 };
    // ssrclearlog N (research): N lines for each clear (ClearRenderTargetView / ClearUnorderedAccessView*) of a reflection resolve
    // output, saying whether it came before or after the call's first resolve dispatch, where ssrtileclear empties it.
    std::atomic<int>   m_ssr_clear_log{ 0 };
    ID3D11Resource*    m_ssr_out_res[4]{};   // reflection resolve outputs seen (not owned; compared by pointer only)
    int                m_ssr_resolve_frame{ -100 };   // frame of the last call's first resolve dispatch
    uint64_t           m_ssr_out_clears[2]{};   // per second: clears of a reflection output before / after the first resolve dispatch
    bool               ssr_is_out(ID3D11Resource* r) const { for (auto* p : m_ssr_out_res) if (p && p == r) return true; return false; }
    void               ssr_note_clear(ID3D11Resource* r, const char* kind, const float* v);
    // ssrmv (off): the reflection passes that read MotionBuffer get the exact per-eye copy shadowmv builds (head translation).
    std::atomic<bool>  m_ssr_mv{ false };
    // Saved constant buffer binding (ssredgelog). The ssrsym switch was removed in the 19 Oct switch audit.
    struct SsrSymSaved { ID3D11Buffer* cb{ nullptr }; UINT first{ 0 }, num{ 0 }, slot{ 0 }; bool on{ false }; };
    // ssredgelog N: dumps of the reflection trace passes' edge-fade constants (research; the ssredge switch is gone, 16 Oct).
    std::atomic<int>   m_ssr_edge_log{ 0 };
    std::unordered_map<ID3D11ComputeShader*, UINT> m_ssr_vig_cs;   // reflection passes with an edge fade -> CSSLRContextProvider slot (m_ps_mtx)
    SsrSymSaved        ssr_edge_bind(ID3D11DeviceContext* ctx);
    void               ssr_sym_restore(ID3D11DeviceContext* ctx, SsrSymSaved& s);
    // ssrrough (F6 shadow bugs, 7 Nov, research\2026-11-07_f6_shadow_bugs.md): for one dispatch, the pass reads a copy of its
    // constant buffer with one float changed (asmlit, which also used this, was removed on 7 Nov).
    struct CbPatchCs { UINT slot{ 255 }, size{ 0 }, off[2]{ 0xFFFFFFFFu, 0xFFFFFFFFu }; };
    struct CbPatchSaved { ID3D11Buffer* cb{ nullptr }; UINT first{ 0 }, num{ 0 }, slot{ 0 }; bool on{ false }, ps{ false };
                          ID3D11SamplerState* smp[16]{}; ID3D11SamplerState* le[16]{}; UINT smp_mask{ 0 }; };
    std::unordered_map<ID3D11ComputeShader*, CbPatchCs> m_ssr_cut_cs;   // reflection passes that read SpecularFactorCutoffThreshold (m_ps_mtx)
    std::atomic<float> m_ssr_rough{ 0.0f };       // ssrrough x: the reflections' SpecularFactorCutoffThreshold (0 = the game's)
    int                m_cbp_logged[3]{};         // the game's buffer dumped once per kind (only kind 1, ssrrough, is used)
    uint64_t           m_cbp_runs[3]{};
    float              m_cbp_val_last[3]{ -1.0f, -1.0f, -1.0f };
    ID3D11Buffer*      m_cbp_standin[3]{};        // 4096 B copies of the pass's buffer
    ID3D11Buffer*      m_cbp_val[3]{};            // 16 B: the changed value, copied into the stand-in
    CbPatchSaved       cb_patch_bind(ID3D11DeviceContext* ctx, int kind);
    void               cb_patch_restore(ID3D11DeviceContext* ctx, CbPatchSaved& s);
public:   // the shadow lab's copy hooks (free functions) reach it
    // Lights and shadows lab (7 Nov, author 16:56): F11 Debug > "Lights and shadows lab (tests)", code in FarCry5ShadowLab.cpp. Every tool is off by default,
    // not saved, and reachable by command: shadowcap, shadowcapall, passorder, shadowvals, shadowsplit 0|1|2, pairdiff <cb name> [N].
    struct LabSaved { ID3D11Buffer* cb[2]{}; UINT first[2]{}, num[2]{}, slot[2]{ 255, 255 }; int n{ 0 }; };
    std::atomic<bool>  m_lab_any{ false };          // any lab tool running: the one check in the hooks
    std::atomic<bool>  m_lab_own{ false };          // the lab's own copies (not logged by passorder)
    std::atomic<bool>  m_lab_cap_armed{ false };    // shadowcap: the next F6 / snappair also dumps the sun shadow maps
    std::atomic<bool>  m_lab_cap_always{ false };   // shadowcapall: every F6 / snappair dumps them (about 130 MB each; stops below 25 GB free)
    uint64_t           m_lab_cap_shot{ 0 };         // the present number of the F6 that started the dump (its probes\snapshot_<n>.bmp)
    std::atomic<int>   m_lab_cap_state{ 0 };        // 1 waiting for a left call's first resolve, 2 waiting for the right call's
    int                m_lab_cap_frame{ -1 };
    std::string        m_lab_cap_last{};             // the last dump folder (menu)
    std::atomic<int>   m_lab_order_state{ 0 };      // passorder: 1 waiting for a left call, 2 logging the pair
    int                m_lab_order_frame{ -1 };
    int                m_lab_order_n{ 0 };
    void*              m_lab_order_file{ nullptr }; // FILE*
    std::string        m_lab_order_last{};
    std::atomic<int64_t> m_lab_vals_until{ 0 };     // shadowvals: steady-clock ms until which the values are logged
    int                m_lab_vals_frame{ -1 }, m_lab_vals_ssr_frame{ -1 }, m_lab_vals_lines{ 0 };
    std::atomic<int>   m_lab_split{ 0 };            // shadowsplit: 0 the game's, 1 near cascades only, 2 far shadows only
    ID3D11Buffer*      m_lab_standin[2]{};          // 4096 B stand-ins: [0] the Viewport buffer (cb0), [1] CTileLightingBlended (cb2)
    ID3D11Buffer*      m_lab_v3{ nullptr };          // 16 B of 3.0 (ShadowCSMLastSliceIndex)
    ID3D11Buffer*      m_lab_v0{ nullptr };          // 16 B of 0 (a CSMSlice z row)
    ID3D11Buffer*      m_lab_staging{ nullptr };     // 4096 B readback
    uint64_t           m_lab_split_runs{ 0 };
    std::string        m_lab_pd_name{};              // pairdiff: the constant buffer's name
    std::atomic<int>   m_lab_pd_left{ 0 };          // pairs still to compare
    int                m_lab_pd_frame[2]{ -1, -1 };
    std::string        m_lab_pd_where[2]{};
    std::vector<float> m_lab_pd_vals[2]{};
    std::unordered_map<ID3D11ComputeShader*, int> m_lab_resolve{};   // 1 a sun shadow resolve (reads the far-shadow atlas, writes HalfScreenEffectsBuffer), 0 not
    void               lab_update_any();
    LabSaved           lab_dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z, bool indirect);
    void               lab_restore(ID3D11DeviceContext* ctx, LabSaved& s);
    void               lab_draw(ID3D11DeviceContext* ctx, const char* kind, UINT a, UINT b);
    void               lab_note(ID3D11DeviceContext* ctx, const char* kind, ID3D11View* view, ID3D11Resource* dst, ID3D11Resource* src);
    void               lab_snapshot();
    bool               lab_command(const std::string& cmd, const std::string& arg, const std::string& line);
    void               lab_menu(void (*hint)(const char*), void (*section)(const char*));
    bool               lab_read_cb(ID3D11DeviceContext* ctx, int stage, UINT slot, std::vector<float>& out, UINT max_bytes = 4096);
    void               lab_cap_resolve(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, int eye);
    void               lab_vals_resolve(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, int eye);
    void               lab_pairdiff(ID3D11DeviceContext* ctx, int stage, UINT slot, const std::string& where);
    bool               lab_order_frame();
    bool               lab_is_resolve(ID3D11ComputeShader* cs);
private:
    uint64_t           m_smv_ssr_runs{ 0 };
    std::atomic<int>   m_ssr_ptr_log{ 0 };
    std::atomic<int>   m_smv_trace{ 0 };   // smvtrace N (research): the sun-shadow resolve's history and outputs, per dispatch
    void               smv_trace(ID3D11DeviceContext* ctx, bool after);
    int                m_ssr_marker_frame{ -100 };
    void               ssr_debug(ID3D11DeviceContext* ctx);
    // postclean (off): the final post pass also gets ColorSeparationOpacity (c8, chromatic aberration) and
    // DistortionSharpnessAndFactor (c13) at 0 in the headset picture; postlog: one line with the game's c8 and c13.
    std::atomic<bool>  m_post_clean{ true };   // default on since 17 Oct (author), F11 Debug > Experimental
    std::atomic<int>   m_post_log{ 0 };
    ID3D11Buffer*      m_postfx_zero16{ nullptr };
    std::atomic<int>   m_frame_lock{ 0 };           // framelock auto|off|1|2 (saved, 29 Oct; replaces xrhalf; off by default 30 Oct): see runtimes::OpenXR::s_fc5_frame_lock

    std::atomic<bool>  m_no_recoil{ false }, m_no_sway{ false };
    std::atomic<bool>  m_pair_check{ false };   // F11 Debug "Check stereo pairs" (cmd paircheck on|off [N])
    std::atomic<int>   m_pair_every{ 30 };
    std::atomic<float> m_afw_min_deg{ 0.15f };
    uint64_t           m_afw_skipped{ 0 }, m_afw_fallbacks{ 0 }, m_afw_checks{ 0 }, m_afw_black{ 0 };
    UINT               m_afw_held_w{ 0 }, m_afw_held_h{ 0 };
    uint64_t           m_afw_fresh_checks{ 0 }, m_afw_fresh_black{ 0 };
    std::atomic<int>   m_afw_probe_every{ 15 };
    std::atomic<bool>  m_afw_same_pose{ true };     // afwsamepose: both eyes submitted with this frame's pose sample
    uint64_t           m_afw_same_pose_frames{ 0 };   // afwprobe N: check every Nth image for black
    std::atomic<bool>  m_afw_snap{ false };      // afwsnap: the next AFW present saves the fresh and the held image (probes\afw_*.bmp)
    std::atomic<int>   m_afw_snap_held{ -1 };    // afwsnap E: only a present whose held eye is E (0 left, 1 right)
    bool afw_mode() const { return m_afw_test.load() && !m_stereo2.load(); }
    std::atomic<bool>  m_afw_fg_push{ true };    // afwfgpush: AFW takes the weapon pass's depth push (WorldSpaceZOffset) back out of the gun's depth
    std::atomic<float> m_weapon_zoff{ 0.09f };   // the weapon pass's WorldSpaceZOffset (c52.z), from its camera upload
    // afwfpeye (AFW, on by default since 13 Oct 18:00 after the simulator runs): the first-person pass (hands, weapon) is drawn a second time, for the held eye with
    // its own camera, into a depth image of the mod's own; the rebuild then takes the held eye's gun shape from that
    // image (exact, as in stereo) and its colour from the fresh eye (or the held eye's last image) through the gun's own
    // space, instead of moving the fresh eye's gun pixels by their depth.
    std::atomic<bool>  m_afw_fp_eye{ true };
    std::atomic<int>   m_afw_fp_ss{ 2 };
    // afwgunmv (AFW, default on): the weapon pass's previous camera re-anchored to the game frame the gun's previous world
    // position comes from (DLSS history two frames old, object motion one frame: a doubled gun while walking).
    std::atomic<bool>  m_afw_gun_mv{ true };
    // afwgunpose (AFW, default on; author 24 Oct 14:45): the
    // rebuilt eye's gun takes its colour from its own last image (afwgunown) through the gun's own pose in each of the two
    // frames (its world transform), not through its place in the game camera's space. The game moves the gun in that space
    // every frame (walk bob, sprint swing; gunstill holds it still in view), so the colour came from the wrong spot of the gun:
    // the scope's turret and rings twice, a few pixels apart, in every rebuilt image while walking.
    std::atomic<bool>  m_afw_gun_pose{ true };
    // afwgunmvpose (AFW, default on since 24 Oct: the author with it): afwgunmv's previous camera re-anchored
    // through the gun's own transforms of the two frames (exact for the gun and its parts; the hands follow the gun) instead
    // of the game camera's axes.
    std::atomic<bool>  m_afw_gun_mv_pose{ true };
    struct GunWorld { int frame{ -100 }; glm::mat4 Om{ 1.0f }; };
    GunWorld           m_gun_world[4]{};   // the gun mesh's world transform by frame (afwgunpose, afwgunmvpose)
    uint64_t           m_fp_pose_n{ 0 }, m_fp_pose_miss{ 0 }, m_gun_mv_pose_n{ 0 };
    float              m_fp_pose_cm{ 0.0f }, m_fp_pose_max_cm{ 0.0f }, m_fp_pose_max_deg{ 0.0f };   // the gun's move in the game camera's space between an eye's two images
    std::atomic<bool>  m_afw_steady{ true };
    // afwroot (default on, 24 Oct; author):
    // in AFW the player's body root is taken at every frame (each is its own game update), not every second one as in Stereo.
    // walkbob sets the eyes' height from the root's: with the root one update stale every other frame, on a slope the eyes
    // climbed in two-frame steps. viewpivot and the steady view read it too.
    std::atomic<bool>  m_afw_root_every{ true };
    // afwfoot, afwrail (default on since 25 Oct, author; F11 Debug >
    // AFW): from the BioShock VR mod's rebuild, see FarCry5Afw.h.
    std::atomic<bool>  m_afw_foot{ true }, m_afw_rail{ true };
    // afwfoliage (default on, 29 Oct; author; F11 Debug > AFW): the rebuild's test for
    // a held point that moved took needles, leaves and grass for moved (the fresh eye sees past them, on other specks), so the
    // rebuilt eye showed the tree's dark inside every other frame. See FarCry5Afw.h.
    std::atomic<bool>  m_afw_foliage{ true };
    // afwcopies (default on, 29 Oct; author; F11 Debug > AFW): the mod's own weapon-pass draws (holster, sling and hand copies, the throwable on the chest or in the
    // hand, the pouch) drawn again for the held eye, as afwfpeye does with the game's weapon draws.
    std::atomic<bool>  m_afw_copies{ true };
    // afwhandfresh (default on, 2 Nov; author; F11 Debug > AFW): the rebuilt
    // eye's hands and the mod's carried copies take their colour from this frame's (fresh) image first. With afwgunown every weapon-pass
    // texel took it from the held eye's own last image through the GUN's move, so a hand that moved apart from the gun showed its old
    // fingers inside the new ones. The held eye's weapon depth gets a stencil marking those draws; the gun keeps afwgunown's shading.
    std::atomic<bool>  m_afw_hand_fresh{ true };
    // afwdepthlate (default on, only in a vehicle): AFW's depth copy retaken at present, after everything of the frame is
    // drawn (the copy at the scene depth's first release may come before the player's vehicle is drawn: the cab then
    // has the road's depth and the rebuilt eye throws it off by a frame of driving). Every 2 s in a vehicle the log
    // compares both copies at the dashboard ([AFW] vehicle depth).
    std::atomic<bool>  m_afw_depth_late{ true };
    // afwstartstereo (default on; slow start thread, 20 Oct): an AFW session renders as Stereo from the game's first menu
    // screen until the player is first in play, then AFW (the saved mode stays AFW). In AFW the game held its black first
    // screen ~90 s longer before the intro videos (simulator, same build: 194 s to play vs 104 s in Stereo; an AFW start
    // switched to Stereo at the menu: 105 s).
    std::atomic<bool>  m_afw_start_stereo{ true };
    bool               m_afw_start_held{ false };   // Stereo stands in for AFW now
    bool               m_afw_start_done{ false };   // first play reached, or the mode chosen by hand: no more stand-in
    ID3D11Texture2D*   m_depth_late_tex{ nullptr };
    ID3D11Texture2D*   m_depth_probe_stage[2]{};
    std::chrono::steady_clock::time_point m_depth_probe_t{};
    void afw_vehicle_depth(ID3D11DeviceContext* ctx);
    // afwvehbody (default 3 m since 24 Oct, author: 3 reduced the cab's doubling a lot, 0 made it worse; was 1.5 m): in a vehicle, nearer than this
    // the AFW rebuild carries both images with the vehicle (its attitude is the body frame, the seat the game camera), not as
    // static world, so the held eye's own cab is used again (with afwvehnear alone what the fresh eye could not see was filled).
    std::atomic<float> m_afw_veh_body{ 3.0f };
    // afwvehown (default on, 24 Oct): the cab nearer than afwvehbody is carried with the vehicle model's own transform at each
    // image (the largest Vehicle_ draw under the camera, as the gun now uses its own pose), not the frame of the game camera,
    // which springs inside the car at launch and bumps. A frame whose model and camera disagree by more than 25 cm or 4 deg
    // over one frame (another part of the car found) keeps the camera's frame.
    std::atomic<bool>  m_afw_veh_own{ true };
    struct VehWorld { int frame{ -100 }; UINT count{ 0 }; glm::mat3 rot{ 1.0f }; glm::vec3 pos{ 0.0f }; };
    VehWorld           m_veh_world[4]{};   // the car model's transform by frame (world -> its axes, its origin)
    uint64_t           m_veh_own_n{ 0 }, m_veh_own_miss{ 0 }, m_veh_own_reject{ 0 }, m_veh_own_checked{ 0 };
    float              m_veh_own_cm{ 0.0f }, m_veh_own_max_cm{ 0.0f }, m_veh_own_max_deg{ 0.0f };   // the camera's spring in the car over a frame
    void               afw_veh_own_stamp(int frame);
    // afwvehmv (default on; 16 Oct, still some doubling with afwvehbody): in AFW and a vehicle, DLSS gets the cab's motion (nearer
    // than afwvehbody) from the vehicle's frame over the eye's two-frame history; the game's vector covers only one frame.
    std::atomic<bool>  m_afw_veh_mv{ true };
    // vehmv (author 7 Nov 13:59 video, OFXR NVIDIA DLSS frame generation, build D2DEA5E8: heavy ghosting in vehicles): Stereo too.
    // Each eye's DLSS history is that eye's previous image, two frames back, as in AFW. Without a vehicle frame, the cab's pixels
    // got the camera's motion as if the cab stood still in the world, plus the game's 8-bit vector (rotation only, clipped at
    // speed). DLAA hid most of it; frame generation moves the cab by those vectors. On: afwvehmv's motion (points nearer than
    // afwvehbody, fixed in the vehicle's frame) in Stereo as well. Off: as before 7 Nov.
    std::atomic<bool>  m_veh_mv_stereo{ true };
    uint64_t           m_veh_mv_stereo_sets{ 0 };
    uint64_t           m_veh_mv_sets{ 0 };
    // afwvehmv for the game's TAA (DLSS off, the author's setting): the same rows go into shadowmv's exact motion copy (taamv).
    float              m_body_ctp[16]{};
    float              m_body_ctp_lim{ 0.0f };
    int                m_body_ctp_frame{ -10 };
    uint64_t           m_veh_mv_taa{ 0 };
    std::atomic<int>   m_veh_probe_log{ 6 };   // vehlog N: log the next N Vehicle_ draws' world positions
    std::atomic<float> m_afw_veh_near{ 5.0f };   // 5 m (14 Oct video: a truck's hood front reaches past 3 m)   // afwvehnear: in a vehicle, AFW takes only the fresh image nearer than this (m); 0 = off
    // The old gunsteady switch was removed in the 19 Oct switch audit (author 14 Oct: motion control worse); gunstill replaces it.
    float              m_gun_obj_now[12]{};     // the gun mesh's world transform (3x4 rows) of the last weapon pass
    UINT               m_gun_obj_count{ 0 };    // its index count (the largest weapon-pass mesh but the gloves)
    int                m_gun_obj_seen{ -100 };  // frame it was seen
    uint64_t           m_gs_frames_held{ 0 };
    glm::mat4 gun_steady(int frame, const glm::vec3& cam);   // gunstill's correction (identity with gunstill off)
    // gunstill (FarCry5GunStill.cpp; author 16 Oct evening, F11 Comfort, default on): the gun held at this weapon's standing
    // pose (learned standing still) except while the game reloads (pad X, or its left hand leaving the gun) or throws; in a
    // vehicle the gun and hands keep the eyes' smoothed position (gun_anchor_apply). Off = gunsteady's way (if on) or the game's.
    std::atomic<bool>  m_gun_still{ true };
    std::atomic<int>   m_gun_still_log{ 0 };          // gunstilllog N: N per-update lines
    std::atomic<float> m_gst_hand_cm{ 7.0f };         // gunstillhand: the game's left hand this far (cm) from its place on the gun = a reload
    std::atomic<float> m_gst_ease{ 0.15f };           // gunstillease: seconds to the game's pose and back
    // gunstillpair (author 23 Oct): in Stereo the left eye's pass is a new game update, held from the last update's pose moved on
    // (the game writes the gun after the pass's camera), the right eye's from that update's own pose. On (default): the right
    // eye takes the left eye's hold, so both eyes show the same gun. Off: each eye its own (before 23 Oct).
    std::atomic<bool>  m_gst_pair{ true };
    // gunstillpredict (test, not saved): how the next update's gun is guessed: 0 hold (the last pose), 1 moved on by the last
    // change (before 23 Oct), 2 the last change scaled by the time since the last update over the one before, 3 half the change.
    std::atomic<int>   m_gst_predict{ 1 };
    // gunaxes on|off (FarCry5GunStill.cpp; default on, saved; F11 Controls; author 29 Oct 13:48): each gun's angle in the hand comes from its own model axes (the
    // gun mesh's +Y is its barrel, +Z its top: every sight line read so far runs along +Y within 3.4 deg), turned so the barrel lies along
    // the controller's axis and the top faces up (or, once read from the game's aim down sights, its sight line along the controller and
    // the sight's top up), about the game's own right hand on it, which stays at the gun's grip point. Off: the
    // pose learned from the game standing still (gunstill), which was missing for pistols the game holds within 10 cm of the camera
    // (27144, 32472 and 29850 in his 13:43 log: the game's animation and a pivot 20 cm off the hand) and wrong for some saved guns
    // (32472 learned 31 deg off the barrel, 40362 36 deg, 51543 rolled 65 deg). Melee weapons and a mesh whose +Y the game does not
    // hold forward (a bow, a launcher) keep their way; gunaxesgame lists guns kept on the game's pose by hand (F11, per gun).
    std::atomic<bool>  m_gun_axes{ true };
    // gunposecheck on|off (FarCry5GunStill.cpp; default on, saved; F11 Controls under gunaxes; author 3 Nov 08:17): a learned pose over 20 deg off a gun's barrel or rolled over 30 deg (the MP40 40362's,
    // saved 30 Oct: 36 deg off, rolled -48) is learned again once the game holds the gun along its axes standing still for 1 s, and until
    // gunaxes has measured the gun (it cannot while the gun is in two hands) the gun is held by its axes about its origin instead of at it.
    std::atomic<bool>  m_gun_pose_check{ true };
    // gunorigin on|off (FarCry5GunStill.cpp; default on, saved; F11 Controls under gunposecheck; author 5 Nov 11:46): a handgun whose right hand the mod has not measured (no gripbyhand grip
    // point, gunaxes under 3 measures: neither can while the gun is in two hands) is held with its origin, where the game attaches it to the
    // right hand, at the grip point, at its learned angle. Before, it sat where the game holds it, at arm's length, while the point put on the
    // controller was the one-for-all grip of the reference SMG: the 6P9 26187 sat 28 cm ahead of the hand (its magazine well at -0.235 m in
    // the controller's axes, the M9's and D50's at +0.04/+0.05), so turning the controller swung it round. Off: as before 5 Nov.
    std::atomic<bool>  m_gun_origin_hold{ true };
    // gunstillswitch on|off (FarCry5GunStill.cpp; default on, saved; F11 Controls under gunorigin; author 5 Nov 13:08): the reload button held changes weapon, and that press
    // started a "reload" on the weapon it brought, so the game's own pose was shown instead of the held gun; and a learned pose off the
    // gun's axes (the 1887 Modern 32472's, 31 deg) kept that "reload" going for 6 s, the gun 23 cm / 30 deg from it all along. Now a press
    // from before a weapon came out is no reload of it, and while a gun is held by its axes, a learned pose off them does not tell a reload.
    // Off: as before 5 Nov.
    std::atomic<bool>  m_gst_switch_hold{ true };
    std::set<uint32_t> m_gun_axes_game{};          // gunaxesgame <sig> ... | none (saved; under m_weapon_cal_mtx)
    bool               gun_axes_game(uint32_t sig);
    std::string        gun_axes_game_line();
    // The weapon hand's centre (the right glove's, game-camera axes) of the last weapon pass that drew it, whether or not the game's
    // left hand was drawn too (gunaxes: one-handed pistols draw only the right glove).
    glm::vec3          m_wpn_grip_cam{ 0.0f };
    int                m_wpn_grip_frame{ -100 };
    // The weapon in hand's gunaxes state, for F11: 0 none (off, no weapon, melee), 1 measuring, 2 own axes, 3 game's pose (its mesh's
    // +Y is not held forward), 4 game's pose (gunaxesgame).
    std::atomic<int>   m_gun_axes_state{ 0 };
    const char*        gun_axes_status() const;
    // gunstilllate (author 23 Oct 19:44): the pass whose camera was placed with a guessed gun (Stereo's left eye, every AFW frame) has its weapon camera
    // uploaded again at the gun mesh's draw, from the game's own values, now with that pass's own gun pose (on_viewport_constants
    // once more): the gun and every later draw of the pass are exact (gunstilllate parts: from the first attachment on). Default on
    // (author 24 Oct 00:10).
    std::atomic<bool>  m_gst_late{ true };
    struct GstLateCam
    {
        ID3D11Buffer*      buf{ nullptr };   // the weapon camera buffer of this pass (a key only, no reference kept)
        int                frame{ -100 }, view_index{ 0 };
        uint32_t           size{ 0 };
        std::vector<float> orig;             // the game's values of that upload, before the rewrite
        bool               redone{ false };
    };
    GstLateCam         m_gst_late_cam{};
    bool               m_gst_late_reupload{ false };   // inside the re-upload (on_viewport_constants: no new view counted)
    int                m_gst_stale_frame{ -100 };      // gun_still: this frame's pass was placed with an older update's gun
    int                m_gst_late_pass_frame{ -100 };
    uint32_t           m_gst_late_pre{ 0 }, m_gst_late_pre_gloves{ 0 };   // this pass's draws before the gun mesh
    uint64_t           m_gst_late_n{ 0 }, m_gst_late_miss{ 0 }, m_gst_late_fail{ 0 }, m_gst_late_pre_sum{ 0 }, m_gst_late_pre_gloves_sum{ 0 }, m_gst_late_pre_max{ 0 };
    void               gun_still_late(ID3D11DeviceContext* ctx);
    // gunstilllate parts (author 24 Oct 00:10): the parts drawn before the gun mesh are the gun's attachments, rigid on it. Each one's place on the gun is learned
    // at the gun mesh's draw (gun = part x rel); in the next guessed pass the first known part's own transform gives the gun's
    // pose at once and the camera is placed again before that part is drawn (then once more at the gun mesh, exact).
    struct GstPart
    {
        UINT      count{ 0 }, start{ 0 };
        INT       base{ 0 };
        uint32_t  sig{ 0 };
        int       frame{ -100 }, bad{ 0 };
        glm::mat4 rel{ 1.0f };
        float     det{ 1.0f };   // gunstillpartcheck: the part's scale (its 3x3's determinant) when its place was learned
    };
    GstPart            m_gst_parts[8]{};
    // gunstillpartcheck (author 6 Nov; saved, default on;
    // F11 next to gunstilllate): a draw before the gun mesh whose transform is not finite, collapsed, or resized since its place was
    // learned (the muzzle flash, drawn only while firing) never places the camera; a guess that is not finite retires its part.
    std::atomic<bool>  m_gst_part_check{ true };
    uint64_t           m_gst_part_unfit{ 0 }, m_gst_part_refused{ 0 };
    struct GstSeen { UINT count, start; INT base; glm::mat4 P; };
    GstSeen            m_gst_seen[8]{};
    int                m_gst_seen_n{ 0 }, m_gst_seen_frame{ -100 };
    int                m_gst_est_frame{ -100 };     // gun_still: this call's gun pose is m_gst_est_obj (the re-upload at a part)
    float              m_gst_est_obj[12]{};
    int                m_gst_est_done_frame{ -100 }, m_gst_est_part{ -1 };
    float              m_gst_est_used[12]{};        // the guess the pass was placed with, checked at the gun mesh
    uint64_t           m_gst_est_n{ 0 }, m_gst_est_none{ 0 }, m_gst_est_chk{ 0 }, m_gst_est_bad{ 0 };
    double             m_gst_est_cm{ 0.0 }, m_gst_est_deg{ 0.0 };
    float              m_gst_est_max_cm{ 0.0f }, m_gst_est_max_deg{ 0.0f };
    // gunstillstat (test): part draws without kept object constants (and the last one's b1 size), with them, part lookups in a
    // guessed pass, gun meshes reaching the learning, parts learned there.
    uint64_t           m_gst_dbg_noobj{ 0 }, m_gst_dbg_seen{ 0 }, m_gst_dbg_tries{ 0 }, m_gst_dbg_mesh{ 0 }, m_gst_dbg_learn{ 0 };
    uint32_t           m_gst_dbg_noobj_size{ 0 };
    bool               gun_still_reupload(ID3D11DeviceContext* ctx);
    // slingmv on|off (FarCry5GunStill.cpp; default on, saved; F11 "Hands and weapon" under gunstilllate; author 6 Nov 13:21 video: the AR-C on the
    // sling trailed and faded into the ground while walking, its silencer first): DLSS took the slung gun's pixels from where the game's
    // motion vectors said they had been. The slung gun is drawn exactly (gunstilllate parts within 1 mm of its mesh), so its motion on
    // screen is known: this eye's camera and gun transform at the gun mesh's draw against the same eye's previous ones. While the
    // weapon hangs on the sling (Stereo), the pixels within slingmvradius of the gun's barrel line (from 45 cm behind the sling point to
    // 1 m ahead of it, along the hung gun's barrel) and not within 15 cm of a hand get that motion instead of the game's, when they are
    // weapon-pass pixels (stencil 0x40). slingmv2 (author 6 Nov 15:00 video sling_ghosting.mp4, build AE016CEC: still ghosting): the log of
    // that build showed DLSS's depth never had a stencil view ("0 used by DLSS"), so the first version never applied; without the stencil
    // the pixels are told apart by their place alone (the capsule along the barrel). Off: the game's vectors (before 6 Nov).
    std::atomic<bool>  m_sling_mv{ true };
    // gunmv (author 6 Nov 21:53 video, OFXR Hybrid with DLSS vectors, build C3F95CAF: the whole gun in hand flickered in OFXR's generated
    // frames): the gun in hand is drawn where the mod puts it (at the controller), not where the game's motion pass thinks it is, and DLSS's
    // depth has no stencil view, so its pixels got the camera's motion as if the gun stood in the world, plus the game's coarse vector.
    // On: the same exact motion and capsule as slingmv for the gun in hand (not the bow); the hand holding it moves with it, the free hand
    // keeps its own. Off: as before 6 Nov.
    std::atomic<bool>  m_gun_mv{ true };
    std::atomic<float> m_sling_mv_radius{ 0.22f };   // slingmvradius <m> (test, not saved): the capsule's radius about the barrel line
    struct SlingMvCam
    {
        int       frame{ -100 };
        glm::mat4 vrp{ 1.0f }, proj{ 1.0f };
        glm::mat3 view_rot{ 1.0f }, eye_rot{ 1.0f };
        glm::vec3 cam{ 0.0f }, eye_cam{ 0.0f };
    };
    SlingMvCam         m_slmv_cam{};                 // the weapon camera last uploaded (on_viewport_constants)
    struct SlingMvHist { int frame{ -100 }; uint32_t sig{ 0 }; glm::mat4 full{ 1.0f }, view{ 1.0f }, O{ 1.0f }; };
    SlingMvHist        m_slmv_hist[2]{};             // per eye: the gun mesh's draw: its camera (proj x view, view) and the gun's transform
    uint64_t           m_slmv_set{ 0 }, m_gunmv_set{ 0 }, m_slmv_nohist{ 0 }, m_slmv_jump{ 0 }, m_slmv_nocam{ 0 };
    float              m_slmv_max_deg{ 0.0f };       // the largest move of the gun's origin in view between an eye's two images given to DLSS
    double             m_slmv_sum_deg{ 0.0 };
    std::chrono::steady_clock::time_point m_slmv_t{};
    void               sling_mv_note(int fr);
    void               gun_still_late_part(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
    std::atomic<int64_t> m_pad_x_press{ 0 };          // the last pad X press (steady_clock ticks)
    std::atomic<int64_t> m_pad_sprint_press{ 0 };     // the last pad left stick press (sprint; steady_clock ticks)
    struct GunStillState;
    GunStillState*     m_gst{ nullptr };
    // partwatch on|off (FarCry5GunStill.cpp; log only, not saved, default on; author 5 Nov 22:47 video: the M629's long barrel, a draw
    // of its own (3924), gone at some angles in the headset, never in the simulator; also the AR-C long's scope (4 Nov) and the Vector's
    // flip-up sights (5 Nov) leaving one eye). For the gun in hand, each eye's weapon-pass draws are followed frame by frame: a draw
    // that eye has drawn for 10 frames or more and is then missing, DROPPED by the mod, or moved on the gun (its object 3 cm or more
    // off its usual place from the gun mesh's) is logged once per episode with the pass's state (guessed or its own pose, placed again
    // at a part or at the gun mesh), the draw order and the other eye's state, then when it is back; a summary every 10 s.
    // [PartWatch] lines. Nothing drawn is changed.
    struct PartWatch;
    PartWatch*         m_pw{ nullptr };
    std::atomic<bool>  m_part_watch{ true };
    void               part_watch_draw(ID3D11DeviceContext* ctx, UINT count, bool dropped);
    void               part_watch_frame(int fr);
    bool               m_veh_base_applied{ false };   // this upload's eyes stand at airfollow's base
    // gunstillfire (author 23 Oct 22:17; F11 Comfort > Weapon,
    // saved): the gun while firing. 1 hold (default): held at its standing pose as at any other time; 2 kick: held, with only a
    // recoil kick (back, up, the muzzle up) taken from the game's firing animation; 0 game: gunstillrecoil's way (before 24 Oct).
    std::atomic<int>   m_gst_fire{ 1 };
    std::atomic<float> m_gst_kick_back{ 0.04f }, m_gst_kick_lift{ 0.02f }, m_gst_kick_deg{ 6.0f };   // gunstillkick: the kick's limits (m, m, deg)
    std::atomic<int>   m_gst_kick_log{ 0 };                                                           // gunkicklog N (test)
    bool               gun_still_busy(bool reload_only = false) const;   // a reload under way, or a shot in the last 0.8 s (offhandstill)
    std::atomic<bool>  m_gst_recoil{ false };         // gunstillrecoil: the game's recoil kick kept while firing (author 16 Oct 21:29; off since 20 Oct: "shooting really suddenly move the weapon ... remove all kind of animation except reloading")
    glm::mat4          gun_still(int frame, const glm::vec3& cam);
    void               gun_still_pace(const glm::vec3& cam);   // the player's speed and sprint, every weapon pass (also with no gun: emptyhandstill)
    float              gun_still_speed(bool& sprinting, float& since_moving_s) const;
    void               gun_still_note_support(const glm::vec3& F, const glm::vec3& W);   // the gloves draw: the game's left (F) and weapon (W) hand, game-camera axes
    bool               gun_still_hand_line(glm::vec3& g);   // twohandline: the game's weapon hand -> left hand direction for the held gun, game-camera axes
    std::atomic<bool>  m_two_hand_line{ true };   // cmd twohandline: the two-handed hold lines the gun's own hand line up with your hands (off: its barrel)
    bool               m_two_hand_line_said = false;   // logged once per two-handed hold
    glm::mat3          m_pump_rel{ 1.0f };             // reloadpumpgrip: the two-handed gun's turn in the weapon hand's controller axes, before a pump stroke
    bool               m_pump_rel_ok = false;
    int                m_pump_aim_frames = 0;          // reloadpumpgrip: frames of this stroke with the aim kept (logged at its end)
    float              m_pump_aim_max = 0.0f;          // ... and how far the hands' line went from it (deg)
    // twohandlevel (author 7 Nov 20:46, all-weapons pass; saved, default on; F11 Controls under twohandline): one hand holds every gun with its sight line (or barrel) along the
    // controller, but two hands lined up each gun's own hand line (the game's right hand -> left hand), which sits 0-19 deg off the barrel
    // depending on the gun, so each gun tilted by its own amount when the left hand took it. On: every long gun (rifles, SMGs, shotguns,
    // marksman rifles, LMGs by the game's data, two-handed) lines up one common hand line instead, twohandlevelref right up (deg off the
    // barrel; default the AR-C 57819's own, so the AR-C does not change): each gun sits at the same angle in two hands against one hand.
    // Handguns, machine pistols, bows, launchers, melee weapons keep their own line. Off: each gun's own line (as before 7 Nov).
    std::atomic<bool>  m_two_hand_level{ true };
    std::atomic<float> m_two_hand_level_ref[2]{ 0.6f, 16.2f };   // the AR-C's line from the author's fc5vr_gunposes.txt (MP40 0.1 up, .308 2.3, M60 3.3, M133 6.5, M249 9.8, AK 12.3, MP5 18.9)
    std::string        gun_still_hand_line_text();   // for the twohandline log line: which line the hold used
    // onehandmatch, "Angle correction for two handed weapons (gunstock calibration)" in F11 (author 8 Nov 09:37) (author 8 Nov 09:13; saved, default on; F11 Controls under
    // twohandlevel). In two hands a long gun follows the line between the controllers; in one hand it followed the weapon controller's -Z (grip
    // pitch in), and a rifle's two-handed wrist points that controller well above the barrel, so at the let-go the gun swung up 25-40 deg.
    // On: in one hand every long gun is turned about the grip point so its own barrel (its one-hand hold, axes or pose) lies on ONE fixed
    // direction, P deg up and Y deg right of the weapon controller's -Z (saved as onehandmatch on P Y; set only in F11, never learnt in play:
    // Author 09:30 the one-hand angle to change after a two-handed hold), pitch and yaw only, at most 45 deg; every long gun so
    // sits at the same one-handed angle. The switch between the two-handed hold and one hand is eased over about 0.15 s (the let-go and the grab).
    // Default -25 0: the two-handed barrel against the weapon hand is not in the author's logs; his let-go swing was 25-40 deg (video, 8 Nov 09:13)
    // and the simulator's two-handed hold with the left hand on the drawn fore-end put the barrel 20-24 deg under the controller (AR-C -20.4,
    // M133 -23.9, M249 -24.1; all-weapons pass). Handguns, machine pistols, bows, launchers, melee weapons: unchanged. The gun's pose, grip and
    // right-hand learning read the game's own camera-space pose and are not affected. Off: the one-hand hold as before 8 Nov.
    static constexpr float kOhmDefaultP = -25.0f, kOhmDefaultY = 0.0f;
    std::atomic<bool>  m_ohm_on{ true };
    std::atomic<float> m_ohm_p{ kOhmDefaultP }, m_ohm_y{ kOhmDefaultY };   // pitch (up +) and yaw (right +) against the weapon controller's -Z, deg
    std::atomic<float> m_ohm_last_x{ 0.0f };               // readout: at the last let-go, the raw one-hand barrel's pitch above the line
    std::atomic<float> m_ohm_last_turn{ 0.0f };            // readout: the turn applied in one hand now, deg
    glm::quat          m_ohm_cur{ 1.0f, 0.0f, 0.0f, 0.0f };   // the turn applied (one hand) or the hold's (two hands), weapon controller's axes
    bool               m_ohm_cur_ok = false, m_ohm_was_two = false;
    uint32_t           m_ohm_sig    = 0;
    std::chrono::steady_clock::time_point m_ohm_t{}, m_ohm_two_t0{};
    void               one_hand_match(glm::mat4& k, const glm::mat4& k_ctrl, bool can, bool two);
    // reloadzonesfollow (author 8 Nov 20:18: the gunstock calibration): the manual reload measures its
    // gun-attached points (well, seat, handle, bolt, slide, pump, breech) in the gun's frame as DRAWN (m_mo.ctrl_rot_world, from the final k:
    // after the two-handed hold, onehandmatch, twohandkeep), but its pad poll placed them with the raw weapon controller. draw_turn: the drawn
    // gun's turn against the weapon controller's own axes (grip pitch undone, so the measured and saved geometry keep their meaning), set at
    // the end of update_motion's weapon frame; false when there is none (one hand, onehandmatch off: the poll's frame is bit for bit the raw
    // controller's, as before) or the weapon is slung, kept or a bow (their own paths). one_hand_drawn: k with the last applied one-hand turn,
    // for the press tests made before one_hand_match runs (the fore-end grab point, gripnearweapon).
    std::mutex         m_draw_turn_mtx;
    glm::mat3          m_draw_turn{ 1.0f };
    bool               m_draw_turn_on = false;
    bool               draw_turn(glm::mat3& E);
    glm::mat4          one_hand_drawn(const glm::mat4& k) const;
    // gunstocklearn (author 8 Nov 10:33: click, hold a two-handed weapon and aim with it, after 8 s the
    // angle correction is saved): an 8 s window; from 1.5 s on, each frame a long gun is held in two hands (not firing, not reloading) gives a
    // sample of its drawn barrel in the weapon controller's axes; at 8 s, with 2 s of samples or more, onehandmatch P Y become their mean (saved).
    // The only way the value is learnt (nothing in play). Position: the two-handed hold turns the gun about the weapon controller's point, as
    // the one-hand hold does, so the gun's place is the same in both by construction: only the angle is learnt.
    std::atomic<bool>  m_gsl_on{ false };
    std::chrono::steady_clock::time_point m_gsl_t0{};
    std::vector<glm::vec3> m_gsl_samples;
    float              m_gsl_good_s = 0.0f;
    std::mutex         m_gsl_mtx;
    std::string        m_gsl_msg;   // the last result, for F11
    void               gunstock_learn_start();
    void               gunstock_learn_finish();
    std::string        gunstock_learn_text();
    void               gunstock_tick();   // a light buzz on the weapon hand's controller (Stereo.cpp)
    std::string        one_hand_match_text();
    // weaponaudit (test, not saved; all-weapons pass 7 Nov): one [Audit] line for the gun in hand: how it is held (axes, sight line, grip
    // point, the game's right and left hands on it, its hand line), and with two hands how far the hold turned it from the one-hand pose.
    std::atomic<bool>  m_weapon_audit{ false };
    glm::mat4          m_aud_kc{ 1.0f }, m_aud_k{ 1.0f };   // this frame's weapon controller (grip pitch in) and the weapon's frame after the two-handed hold
    glm::vec3          m_aud_other{ 0.0f };                 // the other controller, same space
    int                m_aud_frame{ -1 };
    bool               m_aud_two{ false }, m_aud_other_ok{ false };
    void               gun_still_audit(uint32_t sig, bool axes_hold, bool melee_hold);
    // gunposebuiltin (saved, default on; all-weapons pass 7 Nov, author 20:46): each gun's standing pose, grip point and the game's right hand on it, measured for every weapon in the simulator
    // (FarCry5GunPoseTable.h), used for a gun bin\fc5vr_gunposes.txt does not have yet: the first draw of each gun sits in the hand at once
    // instead of 19-25 cm off it for 1-2 s until measured. Read at start (a change applies from the next launch).
    std::atomic<bool>  m_gun_builtin{ true };
    void               gun_still_first_draw();                    // probe: the first weapon-pass draw of a frame
    void               gun_still_forget();                        // gunstillreset, F11 Comfort button
    void               gun_still_load();                          // fc5vr_gunposes.txt: each weapon's first learned pose, across sessions
    void               gun_still_save();
    void               gun_still_gun_cb(ID3D11Buffer* cb);        // probe: the gun mesh's object buffer
    bool               gun_still_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
public:
    void               note_pad_buttons(uint16_t buttons);   // the pad buttons the game gets, once per poll (gunstill: X reload / interact, left stick press sprint)
    void               walk_left_stick(int16_t& lx, int16_t& ly);   // walkbasis: the left stick turned to the walk basis (XInput hook)
private:
    // Walk direction (FarCry5Walk.cpp; ported from the Motion V2 branch, author 22 Oct). The game walks along its aim, which fireready keeps on the barrel while a weapon is in hand.
    std::atomic<int>   m_walk_basis{ 1 };          // walkbasis game|head|hand (saved; default head)
    std::atomic<float> m_walk_rot{ 0.0f };         // basis yaw - the game's walk yaw (rad), set once per present
    std::atomic<bool>  m_walk_rot_ok{ false };     // the turn applies now (on foot, a player camera, a heading)
    float              m_walk_rel[3]{};            // per basis (1 head, 2 off hand): its heading relative to the body yaw (held while it points down)
    bool               m_walk_rel_ok[3]{};
    int                m_walk_rel_basis{ 1 };      // the walkbasis the held headings belong to
    bool               m_walk_wide{ false };       // the turn is past 20 deg (logged on change)
    std::chrono::steady_clock::time_point m_walk_wide_t{};
    std::atomic<float> m_lstick_mag{ 0.0f };       // the left stick's length the game gets (XInput hook)
    std::atomic<float> m_walk_stick[4]{};          // walklog: the left stick before and after the turn (x, y in, x, y out)
    std::atomic<int>   m_walk_log{ 0 };            // walklog N (test): N presents of camera position, yaws and stick, one line per 6
    std::atomic<int>   m_walk_log_every{ 6 };      // walklogevery K (test): one walklog line per K presents
    int64_t            m_sh_end{ 0 };              // the sprint press stamp a sprint was ended at (a shot, aiming down sights, stick eased off)
    bool               m_sh_sprinting{ false };    // the game's sprint as far as the mod can tell (sprint_update, render thread)
    bool               player_sprinting() const;   // gunstill's sprint state (FarCry5GunStill.cpp)
    void               walk_present();             // once per present, before the aim loop (motion_post_present)
    void               walk_set_rot();             // m_walk_rot from the basis and the game's aim with the mouse counts in flight
    void               walk_after_send();          // after the aim loop's mouse send
    float              m_walk_basis_yaw{ 0.0f };   // the basis heading, world yaw (walk_present)
    bool               m_walk_basis_ok{ false };
    void               sprint_update();
    bool               walk_command(const std::string& cmd, const std::string& arg, bool& persist);
    // bodyview on|off (saved, default on; F11 Comfort > Smooth motion "Body-based view"; author 29 Oct 13:49, headset): one switch for where the eyes stand on
    // foot. On: viewpivot, viewroot, viewrootsmooth, rootsteady on and walkbob off (the bob taken out), as since 29 Oct. Off: all of them
    // off, the eyes on the game camera with its bob, as before 23 Oct. Those five are no longer saved on their own (test commands now).
    std::atomic<bool>  m_body_view{ true };
    // viewpivot, viewlog (FarCry5ViewPivot.cpp, 23 Oct): the eyes' base off the game camera's neck pivot.
    std::atomic<bool>  m_view_pivot{ true };       // viewpivot on|off (test; part of bodyview)
    // aimease ms (saved, default 300, 0 = off; F11 Debug > Head tracking and view): the game's aim turned from the head to the barrel (a weapon taken in
    // hand) or back over that time instead of in one or two sends (the update_motion aim loop), so the camera on the neck does not jolt the view.
    std::atomic<int>   m_aim_ease_ms{ 300 };
    bool               m_aim_ease_barrel{ false }, m_aim_ease_valid{ false }, m_aim_ease_on{ false };
    glm::vec3          m_aim_ease_from{ 0.0f, 1.0f, 0.0f }, m_aim_ease_last{ 0.0f, 1.0f, 0.0f };
    std::chrono::steady_clock::time_point m_aim_ease_t0{};
    uint64_t           m_aim_eased{ 0 };
    std::atomic<int>   m_view_log{ 0 };            // viewlog N (test): one line per world upload
    int                m_view_log_frame{ -1 };
    struct PivotAim { int frame{ -100 }; float yaw{ 0.0f }, pitch{ 0.0f }; };
    PivotAim           m_pivot_aim[32]{};          // the aim of each world upload (for the root seen later in its frame, and the lag)
    glm::vec3          m_pivot_off{ 0.0f };        // the game camera minus the body root at the last root seen (world)
    float              m_pivot_psi{ 0.0f }, m_pivot_len{ 0.0f };   // its horizontal heading (world yaw) and length
    float              m_pivot_psi_now{ 0.0f };    // the heading estimated for this pair
    std::atomic<int>   m_view_pivot_lag{ 2 };      // the aim's turn taken 2 updates back (viewpivotlag, a test knob, retired 29 Oct)
    int                m_pivot_off_frame{ -1 };
    int                m_pivot_pair{ -100 };
    glm::vec3          m_pivot_shift{ 0.0f };      // this pair's shift of the eyes' base from the game camera
    float              m_pivot_aim_yaw{ 0.0f }, m_pivot_aim_pitch{ 0.0f }, m_pivot_head_yaw{ 0.0f };
    bool               m_pivot_ok{ false };        // this pair's shift is applied
    bool               m_pivot_applied{ false };   // ... and this upload uses it (gun_anchor_apply moves the drawn gun with it)
    bool               pivot_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               pivot_aim(float& yaw, float& pitch) const;
    void               pivot_note_aim(int frame);
    bool               pivot_aim_at(int frame, float& yaw) const;
    void               pivot_note_root(int frame, const glm::vec3& camoff);
    glm::vec3          view_pivot_shift(int frame, const glm::vec3& cam);
    // viewroot on|off (saved, default on; F11 Debug > Head tracking and view): the eyes' base across the ground = the player's body root (predicted
    // from its last two) plus the camera's offset from it with the aim level, turned to the head's heading: the neck's swing with the aim's
    // heading AND pitch left out (author 27 Oct jolts: the game camera moved 4.5 cm per update, 9-13 cm in all, as the aim went from the
    // head to the gun; viewpivot turned only the heading). Falls back to viewpivot without a fresh root or a learned offset.
    std::atomic<bool>  m_view_root{ true };
    float              m_vr_ref_fwd{ 0.0f }, m_vr_ref_right{ 0.0f };
    bool               m_vr_ref_ok{ false };
    uint64_t           m_vr_used{ 0 }, m_vr_fallback{ 0 };
    // viewrootsmooth on|off (saved, default on; same tab; author 27 Oct 20:26):
    // viewroot no longer waits for viewpivot's offset, which is refused while the camera is over 25 cm from the root (the gun held low, the
    // aim pitched down: 26-29 cm in his 21:09 log, for seconds), so the eyes no longer snap 20+ cm back onto the neck and out again; and a
    // fall back to the game camera, or the way back onto the body, is eased over 0.15 s instead of in one pair.
    std::atomic<bool>  m_vr_smooth{ true };
    float              m_vr_w{ 0.0f };             // the body-based shift's weight (eased)
    glm::vec2          m_vr_sh{ 0.0f };            // its last shift from the game camera, held while it eases out
    glm::vec2          m_vr_hd{ 0.0f };            // and the part of it turned with the head (smoothmotion keeps it out of its filter)
    glm::vec2          m_vr_prev_rel{ 0.0f };      // last pair's eyes' base from the body root (the step log)
    bool               m_vr_rel_ok{ false };
    std::chrono::steady_clock::time_point m_vr_t{}, m_vr_log_t{};
    uint64_t           m_vr_gate{ 0 }, m_vr_stale{ 0 }, m_vr_far{ 0 };   // pairs on the body with viewpivot's offset old; fall backs: root old, too far or fast
    float              m_vr_step_max{ 0.0f };      // the largest pair-to-pair step of the shift (m), last 5 s
    uint64_t           m_vr_log_used{ 0 }, m_vr_log_fb{ 0 }, m_vr_log_gate{ 0 }, m_vr_log_stale{ 0 }, m_vr_log_far{ 0 };
    // viewjump (log only, 29 Oct; author 10:07 playtest; the log had nothing
    // per frame to find them): once per stereo pair, the eyes' base (where the head is added; the mod's own part of the view) against a
    // steady motion, and its horizon's tilt. A pair whose base steps over viewjumpcm (default 4 cm) off its last step (scaled to the
    // pair's time), or whose horizon tilts over 1.5 deg, is counted; one [ViewJump] line per 5 s with the largest of each and what
    // was on then, and a line at once for one over 3x the threshold. viewjumpcm N (saved, 0 = off).
    std::atomic<float> m_vj_cm{ 4.0f };
    int                m_vj_pair{ -100 };
    bool               m_vj_ok{ false };
    glm::vec3          m_vj_cam{ 0.0f }, m_vj_step{ 0.0f }, m_vj_up{ 0.0f, 0.0f, 1.0f };
    float              m_vj_dt{ 0.0f };
    std::chrono::steady_clock::time_point m_vj_t{}, m_vj_log_t{};
    uint64_t           m_vj_n{ 0 }, m_vj_tilt_n{ 0 }, m_vj_pairs{ 0 };
    float              m_vj_max{ 0.0f }, m_vj_tilt_max{ 0.0f };
    glm::vec3          m_vj_dev{ 0.0f };           // the last counted step off a steady motion (world, m)
    std::string        m_vj_max_what, m_vj_tilt_what;
    void               view_jump_note(int frame, const glm::mat3& base, const glm::vec3& base_cam);
    // smoothmotion on|off (default on, saved; F11 Comfort > Smooth motion; author 29 Oct 11:50): on foot, the eyes' base (where the head is added: body-based position, walkbob's
    // height) through one tracking filter run on the headset's display clock, once per picture. The game moves the player by its own
    // frame times and the body is sampled unevenly (his log: the base stepped 0 and 20 cm in turn at a run); the headset shows each
    // picture at its own display time. The filter advances the base by its pace times the display time between pictures and pulls it
    // onto the game's position over smoothtau (0.06 s), with no lag at a steady pace; over 0.75 m off (a vault, a load) it starts
    // again. Both eyes of a pair stand on the same point. The head is not filtered. Off: the base as before (the filter still runs
    // for its log line).
    // Second round (author 29 Oct 12:49, headset): only the body is
    // filtered; the part of the base that turns with the head (viewroot's and viewpivot's offset of the camera from the body) is added
    // after it, so a head turn or a stick turn moves the eyes at once. An error the same way two pictures running is the player
    // starting, stopping or turning (the uneven sampling alternates): pulled in over smoothquick (0.025 s). Modelled: the view trailed
    // a start and ran on past a stop by ~15 cm at a run, now 4-8 cm (unsmoothed: ~8 cm of sampling noise), the evened steps ~30 % rougher.
    std::atomic<bool>  m_smooth_motion{ true };
    std::atomic<float> m_smooth_tau{ 0.06f };      // smoothtau S (test)
    std::atomic<float> m_smooth_quick{ 0.025f };   // smoothquick S (test; at smoothtau or more: never quick)
    glm::vec3          m_ms_head{ 0.0f };          // this pair's part of the base turned with the head (view_pivot_shift), added after the filter
    glm::vec3          m_ms_out{ 0.0f }, m_ms_err_prev{ 0.0f };
    uint64_t           m_ms_quick_n{ 0 };
    int                m_ms_pair{ -100 };
    bool               m_ms_ok{ false }, m_ms_used{ false }, m_ms_raw_ok{ false };
    glm::vec3          m_ms_x{ 0.0f }, m_ms_v{ 0.0f }, m_ms_raw{ 0.0f }, m_ms_raw_step{ 0.0f }, m_ms_out_step{ 0.0f };
    float              m_ms_dt{ 0.0f };
    int64_t            m_ms_T{ 0 };
    std::chrono::steady_clock::time_point m_ms_t{}, m_ms_log_t{};
    uint64_t           m_ms_pairs{ 0 }, m_ms_disp{ 0 }, m_ms_resets{ 0 }, m_ms_raw_n{ 0 }, m_ms_out_n{ 0 };
    float              m_ms_raw_max{ 0.0f }, m_ms_out_max{ 0.0f }, m_ms_lag_max{ 0.0f };
    glm::vec3          motion_smooth(int frame, const glm::vec3& z);
    // walkbob, boblog (FarCry5WalkBob.cpp, 23 Oct): the eyes' height without the game camera's step-rate bob.
    std::atomic<bool>  m_no_bob{ true };           // walkbob off|on (saved): true = "walkbob off", the bob taken out
    std::atomic<float> m_bob_jump{ 0.15f };        // bobjump M (test): a stance change (crouch, stand up, slide) past the held level (m)
    std::atomic<float> m_bob_ease{ 0.2f };         // bobease S (test): the eyes' ease onto a new stance level (s)
    std::atomic<float> m_bob_rlead{ 2.0f };        // bobrootlead V (test): the camera's rise or fall past V m/s comes at once (jumps); 0 = off
    std::atomic<float> m_bob_rcut{ 3.0f };         // bobrootcut F (test): the root height's smoothing, Hz at rest (one-euro)
    std::atomic<int>   m_bob_log{ 0 };             // boblog N (test)
    float              m_bob_h{ 0.0f };            // the camera's height over the body root, last root seen
    float              m_bob_root_z{ 0.0f }, m_bob_rootcam_z{ 0.0f };   // that root's height, and the camera's in its update
    int                m_bob_h_frame{ -100 };
    bool               m_bob_h_good{ false };
    std::chrono::steady_clock::time_point m_bob_root_t{};
    static constexpr int kBobHist = 32;
    struct BobH { float t{ 0.0f }; float h{ 0.0f }; };
    BobH               m_bob_hist[kBobHist]{};     // h at the last roots (t: seconds since m_bob_t0)
    int                m_bob_hist_n{ 0 };
    std::chrono::steady_clock::time_point m_bob_t0{ std::chrono::steady_clock::now() };
    int                m_bob_pair{ -100 };
    std::chrono::steady_clock::time_point m_bob_t{};
    glm::vec3          m_bob_cam{ 0.0f };
    float              m_bob_speed{ 0.0f };
    bool               m_bob_init{ false };
    int                m_bob_mode{ 0 };            // 0 standing (the level follows h), 1 moving (level held), 2 a stance change (with h)
    int                m_bob_h_used{ -100 };
    float              m_bob_lvl{ 0.0f }, m_bob_lvl_v{ 0.0f }, m_bob_target{ 0.0f };   // the eyes' height over the root, its speed, its target
    float              m_bob_chg_t{ 0.0f };        // when the stance change began
    float              m_bob_r{ 0.0f }, m_bob_rd{ 0.0f }, m_bob_rl{ 0.0f };   // the root height used (smoothed), its rate, the camera lead
    float              m_bob_shift{ 0.0f }, m_bob_w{ 0.0f }, m_bob_out{ 0.0f };
    float              bob_hist_range(float since, float& median) const;
    bool               m_bob_applied{ false };     // this pair's eyes carry a height shift (gun_anchor_apply moves the drawn gun with it)
    struct BobBase { int frame{ -100 }; float z{ 0.0f }; };
    BobBase            m_bob_base_at[8]{};         // the eyes' base height per world upload (boblog)
    bool               bob_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               bob_note_root(int frame, const glm::vec3& root, const glm::vec3& camoff);
    float              walk_bob_shift(int frame, const glm::vec3& cam);
    void               bob_note_base(int frame, float z);
    // throwarc, meleeswing (FarCry5Fc2Vr.cpp, 24 Oct).
    bool               throw_arc_command(const std::string& cmd, const std::string& arg, bool& persist);
    void               throw_melee_present();   // meleeswing's frame, a measured throw
    std::atomic<bool>  m_throw_arc{ false };       // throwarc on|off (saved)
    std::atomic<float> m_arc_speed{ 15.0f }, m_arc_up{ 10.0f };   // throwarcspeed m/s, throwarcup deg (saved)
    ID3D11View*        m_arc_view{ nullptr };
    ID3D11Texture2D*   m_arc_tex{ nullptr };
    bool               m_arc_drawn{ false };
    // throwarc's measured throws (24 Oct night): the thrown object's launch read from the game's entity transforms after each release.
    struct ArcFit { float speed{ 0.0f }, up{ 0.0f }, g{ 9.81f }; };
    std::map<UINT, ArcFit> m_arc_fit;             // per throwable (its held mesh's index count)
    ArcFit             throw_fit_now(bool& measured) const;   // the held throwable's launch (measured, or throwarcspeed / throwarcup)
    bool               m_arc_v2_gone{ false };        // throwv2: the game's throw under way (measured once from its start)
    glm::vec3          m_arc_v2_dir{ 0.0f };          // throwv2: the trajectory's launch direction while it showed (world)
    bool               m_arc_v2_dir_ok{ false };
    bool               m_arc_rel_v2{ false };         // the measured release was a throwv2 throw (against its trajectory)
    float              m_arc_rel_up_used{ 0.0f };     // the lift its camera was lowered by
    // throwarcstop on|off (saved, default on): the trajectory marched through the scene depth (aim channel 2), drawn up to where it meets it.
    std::atomic<bool>  m_arc_stop{ true };
    glm::vec3          m_arc_live_o{ 0.0f }, m_arc_live_v{ 0.0f };   // the trajectory drawn this frame (world), for the march
    float              m_arc_live_g{ 9.81f };
    int                m_arc_live_frame{ -100 };
    uint64_t           m_arc_hit_said{ 0 };
    std::atomic<bool>  m_arc_snap{ false };        // throwarcsnap (test)
    std::atomic<UINT>  m_arc_sig{ 0 };              // the throwable held now (throwable_draw)
    bool               m_arc_was_held{ false }, m_arc_wait{ false };
    UINT               m_arc_rel_sig{ 0 };
    float              m_arc_rel_pitch{ 0.0f };
    glm::vec3          m_arc_rel_head{ 0.0f, 1.0f, 0.0f };
    std::chrono::steady_clock::time_point m_arc_rel_t{};
    void               throw_arc_release(const glm::vec3& hand, const glm::vec3& head_dir);
    void               throw_arc_result();
    void               draw_throw_arc(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb);
    void               gesture_zones_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);   // gesturezones (FarCry5GestureZones.cpp)
    std::atomic<bool>  m_melee_swing{ false };     // meleeswing on|off (saved; log only)
    std::atomic<float> m_melee_speed{ 2.5f };      // meleespeed m/s (saved)
    glm::vec3          m_melee_rel{ 0.0f };
    std::chrono::steady_clock::time_point m_melee_t{}, m_melee_last{};
    bool               m_melee_have{ false };
    float              m_melee_v{ 0.0f }, m_melee_peak{ 0.0f };
    uint64_t           m_melee_swings{ 0 };
    glm::vec3          m_melee_dir{ 0.0f, 1.0f, 0.0f };   // the hand's motion (world axes, relative to the game camera)
    bool               m_melee_dir_ok{ false };            // moving fast enough to march along it (the hand-motion ray, aim channel 1)
    glm::vec3          m_melee_rel0{ 0.0f };               // the hand (from the game camera, world axes) where the swing started
    float              m_melee_travel{ 0.0f };             // how far the hand has gone since
    uint64_t           m_melee_hits{ 0 };
    bool               m_melee_pending{ false };           // a swing waiting for the marches along it
    float              m_melee_pending_v{ 0.0f }, m_melee_pending_barrel{ -1.0f };
    std::chrono::steady_clock::time_point m_melee_pending_t{};
    // meleehit (FarCry5Melee.cpp, author 24 Oct 21:59): the weapon (or the bare hand) swung into a
    // character's body (its draws' root, a standing capsule) faster than meleespeed is a hit: logged (1), or the game's melee pressed
    // with its aim turned onto the point hit (2) and a pulse on the hand.
    // meleebones (2 Nov, author 15:15): a bone's capsule from the character's own skinned draw
    // (its vertices' shape in the rest pose, posed by this frame's palette), from that frame's game camera, world axes; region 0 torso, 1 head,
    // 2 arm, 3 leg.
    struct MeleeCap { glm::vec3 a{ 0.0f }, b{ 0.0f }; float r{ 0.1f }; int region{ 0 }; };
    struct MeleeBody { glm::vec3 rel{ 0.0f }; bool test{ false }; std::vector<MeleeCap> caps{}; };   // a body's root (feet), from that frame's game camera, world axes
    std::atomic<int>   m_mel_mode{ 0 };            // meleehit 0|1|2 (off, log, hit; saved)
    std::atomic<float> m_mel_reach{ 0.55f };       // meleereach m: the weapon's length ahead of the hand (bare hand 0.12)
    std::atomic<float> m_mel_radius{ 0.30f };      // meleeradius m: the body capsule's radius
    std::atomic<int>   m_mel_button{ 0 };          // meleebutton r3|rt|both|auto (0, 1, 2, 3; saved, default r3 since release 0.12.1): auto = rt with a melee weapon, else r3
    std::atomic<bool>  m_mel_held_melee{ false };  // the weapon in hand is a melee weapon (melee_hit_frame; meleebutton auto)
    std::atomic<int>   m_mel_press_ms{ 120 };      // meleepressms: how long the button is held
    std::atomic<int>   m_mel_wait_ms{ 120 };       // meleewait: the longest wait for the game's aim before the press
    std::atomic<float> m_mel_gate{ 15.0f };        // meleegate deg: the aim this close to the point hit presses at once
    std::atomic<int>   m_mel_cool_ms{ 650 };       // meleecool: the shortest time between two hits
    std::atomic<float> m_mel_buzz_amp{ 1.0f };     // meleebuzz 0-1: the hit's pulse on the hand (0 = none)
    std::atomic<bool>  m_mel_aim_on{ true };       // meleeaim on|off: the game's aim turned onto the point hit for the press
    std::atomic<float> m_mel_slew{ 60.0f };        // meleeslew deg: the aim's largest step per send while turning onto it (firesplitslew 12 otherwise)
    // Instant melee (author 26 Oct 12:44).
    std::atomic<bool>  m_mel_ray{ false };         // meleeray on|off (saved): the game's physics rays instead of the capsules (FarCry5GameRay.cpp)
    std::atomic<bool>  m_mel_collide{ true };      // meleecollide (saved, default on, only with meleeray): a melee weapon's drawn blade lies against what it meets
    glm::quat          m_mel_col_q{ 1.0f, 0.0f, 0.0f, 0.0f };   // the blade's turn now, in the controller's axes (under m_mel_mtx)
    std::atomic<bool>  m_mel_ray_sound{ true };    // meleeraysound (saved): the game's melee hit sound at a contact
    std::atomic<float> m_mel_wall_buzz{ 0.4f };    // meleewallbuzz (saved): the pulse when a swing meets anything without health
    uint64_t           m_mel_ray_ent{ 0 };         // meleeray: the one touched last, when
    std::chrono::steady_clock::time_point m_mel_ray_t{}, m_mel_wall_t{};
    uint64_t           m_mel_walls{ 0 };
    std::atomic<int>   m_mel_early_ms{ 100 };      // meleeearly ms (saved, 0 = off): the game's melee pressed when the swing will reach a body this soon
    std::chrono::steady_clock::time_point m_mel_early_t{};   // the last early press (its contact then pulses and deals the damage)
    std::atomic<int>   m_mel_dmg{ 2 };             // meleedamage 0|1|2|3 (saved): off, find the NPC hit only, damage at contact + the game's melee, damage only
    std::atomic<float> m_mel_dmg_hp{ 200.0f };     // meleedamagehp: the damage sent at contact (the player has 250)
    bool               m_mel_dmg_watching{ false };   // the game's entity lookup watched (fc5::lua::damage_watch)
    // meleestill (author 25 Oct 10:22): a melee weapon held at a made pose, its length along the aim's axis turned by meleepitch /
    // meleeyaw / meleeroll (deg), the game's animations of it (and the hands with it) taken out (FarCry5GunStill.cpp).
    std::atomic<bool>  m_mel_still{ false };       // meleestill on|off (saved; off by default since release 0.12.1)
    std::atomic<float> m_mel_pitch{ 40.0f }, m_mel_yaw{ 0.0f }, m_mel_roll{ 0.0f };
    std::atomic<float> m_mel_blade{ 0.70f };       // meleeblade m: a melee weapon's length ahead of the hand (meleehit)
    std::atomic<float> m_mel_auto_m{ 0.62f };      // meleeauto m: a weapon whose game hand sits farther than this from the camera is a melee weapon
    std::set<uint32_t> m_mel_force_on, m_mel_force_off;   // meleeweapons +sig -sig (saved): set by hand, under m_mel_mtx
    bool               melee_weapon(uint32_t sig);
    glm::quat          melee_collide_rot();   // meleecollide: the turn on the made pose
    bool               melee_still_now() const { return m_mel_still.load() || m_mel_mode.load() > 0; }   // meleestill, or meleehit on
    glm::quat          melee_hold_rot() const;      // the made pose's turn (weapon axes -> game camera axes)
    std::atomic<int>   m_mel_log{ 0 };             // meleelog N (test): N seconds of bodies seen, one line a second
    std::mutex         m_mel_mtx;
    std::vector<MeleeBody> m_mel_cur, m_mel_bodies; // the frame being drawn, the last whole frame
    int                m_mel_cur_frame{ -100 }, m_mel_bodies_frame{ -100 };
    glm::vec3          m_mel_test_rel{ 0.0f };     // meleetest x y z [s] (test): a body standing there (from the game camera, world axes)
    std::chrono::steady_clock::time_point m_mel_test_until{};
    std::atomic<bool>  m_mel_bones{ true };        // meleebones on|off (saved, default on): bodies from their own bones (MeleeCap), else the standing capsule
    std::atomic<float> m_mel_pad{ 0.04f };         // meleepad m (saved): the weapon's half thickness against a bone's capsule (the bare hand 3 cm more)
    std::atomic<float> m_mel_head_x{ 1.5f };       // meleeheadx (saved): meleedamage times this for a head hit (bones only)
    std::atomic<bool>  m_mel_strength{ true };     // meleestrength on|off (saved): damage and pulse grow with the contact's speed (meleebones)
    std::chrono::steady_clock::time_point m_mel_out_said{};   // meleebones: the last "moving out of the body" line
    std::atomic<bool>  m_mel_show{ false };        // meleeshow on|off (test, not saved): the capsules and the weapon drawn over the eye pictures
    std::atomic<int>   m_mel_dump{ 0 };            // meleebonedump [self] (test): 1 the next whole frame's bodies logged, 2 with the player's own
    int                m_mel_dump_at{ 0 };         // meleebonedump: the first frame whose bodies count
    std::vector<MeleeCap> m_mel_self_cur, m_mel_self;   // meleebonedump self: the player's own capsules (under m_mel_mtx)
    int                m_mel_self_frame{ -100 };
    glm::vec3          m_mel_self_root_cur{ 0.0f, 0.0f, 1e9f }, m_mel_self_root{ 0.0f, 0.0f, 1e9f };   // meleebonedump self: the player's root (from the camera)
    glm::vec3          m_mel_b0[2]{}, m_mel_b1[2]{};   // the weapon segment in the body's axes at each eye's last present (the swing's speed: snap turns,
                                                       // walking and driving do not move it; author 27 Oct 21:11 log had 35 and 53 m/s)
    glm::vec3          m_mel_hit_p{ 0.0f };        // meleeshow: the last contact (world) and when
    std::chrono::steady_clock::time_point m_mel_hit_t{}, m_mel_jump_said{};
    struct MeleeScanState;
    MeleeScanState*    m_mel_scan{ nullptr };      // the characters' draws scanned (FarCry5Melee.cpp)
    // The weapon segment (hand side, tip, from the game camera) at the last present of each eye: the speed is measured between presents
    // of the same eye (from one eye's present to the other's the hand jumped 4-8 cm with the controller still: 7-16 m/s, simulator 25 Oct).
    bool               m_mel_have[2]{};
    glm::vec3          m_mel_p0[2]{}, m_mel_p1[2]{};
    std::chrono::steady_clock::time_point m_mel_t[2]{};
    float              m_mel_v_hand{ 0.0f }, m_mel_v_tip{ 0.0f };   // meleelog
    std::chrono::steady_clock::time_point m_mel_last_hit{}, m_mel_log_t{};
    float              m_mel_v{ 0.0f };
    std::vector<int>   m_mel_inside;               // per body: touching last present (a hit is entering one)
    int                m_mel_state{ 0 };           // 0 idle, 1 aiming, 2 pressed
    std::chrono::steady_clock::time_point m_mel_state_t{};
    glm::vec3          m_mel_target{ 0.0f };       // the point hit, world
    bool               m_mel_aimed{ false };       // the game's aim is on it (motion_post_present)
    float              m_mel_aim_err{ -1.0f };     // the game's aim from it, deg (motion_post_present, while aiming)
    std::atomic<int64_t> m_mel_press_until{ 0 };   // steady_clock ticks: the button held until then (read by the pad)
    std::atomic<float> m_mel_buzz{ 0.0f };         // a pulse waiting for the pad (amplitude)
    uint64_t           m_mel_hits{ 0 }, m_mel_presses{ 0 }, m_mel_swings{ 0 };
    bool               melee_command(const std::string& cmd, const std::string& arg, std::istream& rest, bool& persist);
    void               melee_note_character(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, const float* obj, ID3D11Buffer* const* cbs);   // a skinned character draw (world_character_draw)
    // drivehands (FarCry5VehHands.cpp; FarCry5Drive.h): a draw of the player's own body that hidebody would drop, while driving or piloting:
    // true = draw it (its palette swapped so only the hands show, or drivehands all), false = hide it as before. Render thread.
    bool               veh_hands_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, ID3D11Buffer* palette_cb);
    int                veh_hands_max_bone(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, const std::map<int, glm::vec3>** cent = nullptr);   // drivehands: a draw's highest palette slot, -1 not known yet
    struct VehHandsState;
    VehHandsState*     m_vh{ nullptr };
    // drivehandsgrab (FarCry5Drive.h; FarCry5VehHands.cpp): each side's hand in the draw's palette (pal, nslots slots; slot_side: each slot's side,
    // -1 none) moved as one piece onto its controller, or onto the wheel where its grip took it. palm: each side's palm slot (-1: not in this draw);
    // pivot: each side's hand centre (object space); fore: each side's rest-pose forearm-to-hand direction measured on the mesh (nullptr: the
    // bone table's arms). Each hand goes on its controller as on foot, or stays on the control the gesture drive holds with it.
    // gm: path B's arm mesh (its slots' rest-pose centres; nullptr for path A, which takes path B's move of that side this frame).
    void               veh_hands_place(ID3D11DeviceContext* ctx, std::vector<float>& pal, int nslots, const int8_t* slot_side, const int palm[2], const glm::vec3 pivot[2],
                                       const glm::vec3* fore, UINT count, UINT start, INT base, const GloveMesh* gm = nullptr, float scale = 0.0f);
    // drivewheelscan (test; FarCry5VehHands.cpp): from vehicle_probe, each Vehicle_ draw within 1.5 m of the camera while the scan runs,
    // logged with its place and turn in the vehicle's axes, to find the steering wheel's own draw (or its bone).
    void               veh_wheel_scan(ID3D11DeviceContext* ctx, const float* f, UINT count, const glm::vec3& rel);
    std::atomic<bool>  m_dh_grip[2]{};          // drive_poll: the raw grips by physical side (0 right, 1 left)
    std::atomic<bool>  m_dh_held[2]{};          // drive_poll: the gesture drive (drive on) holds its control with that hand
    // drivehandsdirect: the bare arm's indirect draw (veh_hands_draw, path B) drawn instead as a direct draw of one instance with the counts
    // read for it, so an instance count the game's GPU culling wrote as 0 does not hide it. Set by veh_hands_draw, used and cleared by
    // hk_draw_idx_inst_indirect (render thread).
    bool               m_vh_direct{ false };
    bool               m_vh_ontop{ false };   // drivehandsontop (test): veh_hands_draw sets it for a bare arm in a vehicle
    std::vector<std::pair<ID3D11DepthStencilState*, ID3D11DepthStencilState*>> m_vh_ontop_ds;   // the game's state -> the same, depth test always
    std::atomic<int>   m_gun_pass_frame{ -1000 };   // driveready: the last frame the weapon pass drew a weapon in the hands
    UINT               m_vh_direct_n{ 0 }, m_vh_direct_start{ 0 }, m_vh_direct_inst{ 0 };
    INT                m_vh_direct_base{ 0 };
    // drivehandsgrab: the weapon hand's bones as the game held the recorded sidearm (holsterglove: bone, its skinning in q') for this arm mesh,
    // and the sidearm's calibration turn and grip point (q' on a controller pose: [rot corr, pos - rot corr grip0]).
    bool               holster_vehicle_hand(const void* mesh, UINT count, UINT start, INT base, std::vector<std::pair<int, glm::mat4>>& bones, glm::mat3& corr, glm::vec3& grip0);
    void               melee_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye);   // meleeshow: after the overlays
    void               melee_hit_frame();                               // once per present (melee_frame)
    bool               melee_aim(glm::vec3& want) const;                // the game's aim for the press (motion_post_present)
    void               draw_melee_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
public:
    void               melee_pad(WORD& buttons, BYTE& rt, float& buzz) ;   // the pad (VR::on_xinput_get_state): the press and the pulse
    bool               melee_wants_characters() const { return m_mel_mode.load() > 0; }
private:
    void               melee_frame();
    void               view_log_line(int frame, const glm::mat3& eye_rot, const glm::vec3& eye_cam, const glm::vec3& cam, const glm::vec3& base_cam);
    void               draw_pivot_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint);
    void               draw_walk_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                      const std::function<void(const char*)>& section);
    struct WGame { int frame{ -100 }; glm::mat3 axes{ 1.0f }; glm::vec3 cam{ 0.0f }; };
    WGame              m_wgame[4]{};
    uint64_t           m_gun_mv_fixed{ 0 };         // afwfpss 1|2: the held eye's weapon depth at 1x or 2x2 the eye's pixels (edge smoothing)
    struct FpGunCam
    {
        int       frame{ -100 };
        glm::mat3 G{ 1.0f };   // gun space (the game camera's view space) -> this eye's view: v = G q + t
        glm::vec3 t{ 0.0f };
        glm::vec4 tan{};       // the weapon pass's tangents (left, right, up, down), no jitter
        float     pa{ 0.0f }, pb{ 0.0f };   // its projection's depth terms: metres = pb / (z + pa)
    };
    FpGunCam           m_fp_native[2]{};          // each eye's own weapon upload (the image it drew)
    FpGunCam           m_fp_held{};               // the held eye's camera of this frame's second weapon pass
    struct FpHeldCb
    {
        ID3D11Resource*    res{ nullptr };   // the game's weapon camera buffer
        int                frame{ -100 };
        uint32_t           version{ 0 };
        std::vector<float> data;             // its contents rewritten for the held eye
    };
    static constexpr int kFpSlots = 8;   // the game cycles the weapon camera through five buffers
    FpHeldCb           m_fp_cb[kFpSlots]{};
    ID3D11Buffer*      m_fp_standin[kFpSlots]{};
    UINT               m_fp_standin_size[kFpSlots]{};
    uint32_t           m_fp_standin_version[kFpSlots]{};
    int                m_fp_standin_frame[kFpSlots]{ -100, -100, -100, -100, -100, -100, -100, -100 };
    uint32_t           m_fp_cb_version{ 0 };
    ID3D11Texture2D*   m_fp_tex{ nullptr };
    ID3D11DepthStencilView*   m_fp_dsv{ nullptr };
    ID3D11ShaderResourceView* m_fp_srv{ nullptr };
    ID3D11ShaderResourceView* m_fp_cls_srv{ nullptr };   // afwhandfresh: the stencil of that depth (1 = a hand or carried copy)
    bool               m_fp_tex_stencil{ false };        // that depth has a stencil (made while afwhandfresh was on)
    // afwhandfresh: the game's depth-stencil states with the stencil set to mark each draw's class (key kept alive: its address stays its own)
    std::unordered_map<ID3D11DepthStencilState*, ID3D11DepthStencilState*> m_fp_cls_dss;
    bool               m_fp_draw_glove{ false };         // the weapon-pass draw going ahead is the gloves or bare hands (weapon_draw_impl)
    bool               m_fp_copy_now{ false };           // fp_eye_again: a copy the mod draws (holster, sling, throwable, pouch)
    uint64_t           m_fp_cls_other{ 0 };              // held-eye draws marked as a hand or carried copy since the last beat
    UINT               m_fp_w{ 0 }, m_fp_h{ 0 };
    int                m_fp_tex_ss{ 0 };
    int                m_fp_clear_frame{ -100 };   // frame of the last clear of the mod's weapon depth
    int                m_fp_drawn_frame{ -100 };   // frame whose second weapon pass drew at least once
    float              m_fp_clear{ 0.0f };         // its clear value (the far depth of the game's depth test)
    bool               m_fp_armed{ false };        // this frame's weapon upload made a held-eye camera
    uint64_t           m_fp_draws{ 0 }, m_fp_skip_target{ 0 }, m_fp_skip_uav{ 0 }, m_fp_skip_cb{ 0 }, m_fp_frames{ 0 }, m_fp_composed{ 0 };
    // The draw being replayed for the held eye: the game's state to put back.
    struct FpSaved
    {
        ID3D11RenderTargetView*  rtv[8]{};
        ID3D11DepthStencilView*  dsv{ nullptr };
        ID3D11Buffer*            vs0{ nullptr };
        ID3D11Buffer*            ps[4]{};
        bool                     ps_swapped[4]{};
        D3D11_VIEWPORT           vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT                     nvp{ 0 };
        D3D11_RECT               sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT                     nsc{ 0 };
        ID3D11DepthStencilState* dss{ nullptr };   // afwhandfresh: the game's state, put back after the held eye's draw
        UINT                     dss_ref{ 0 };
        bool                     dss_swapped{ false };
    } m_fp_saved{};
    void fp_eye_upload(const float* d, uint32_t size, int frame, const glm::mat3& base, const glm::vec3& base_cam, const glm::mat3& rot, const glm::vec3& cam,
                       bool floating, const glm::mat3& w_full, const glm::vec3& w_off, const glm::mat4& game_proj, const glm::mat4& proj, bool headset_fov,
                       const glm::mat3& view_rot, const glm::vec3& view_cam);
    bool fp_eye_begin(ID3D11DeviceContext* ctx);
    void fp_eye_end(ID3D11DeviceContext* ctx);
    void fp_eye_again(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);   // afwcopies: a mod-drawn weapon-pass draw, again for the held eye
    uint64_t m_fp_copy_draws{ 0 };   // afwcopies: those drawn again since the last beat
    bool fp_eye_target(ID3D11DeviceContext* ctx, UINT gw, UINT gh);
    int  m_fp_target_log{ 0 };   // the first weapon draws' bound depth, logged after each afwfpeye on
    uint64_t    m_fp_skip_nocam{ 0 };       // weapon draws with no held-eye camera for their frame
    int         m_fp_up_frame{ -100 };      // the last weapon upload's frame and what fp_eye_upload made of it
    const char* m_fp_up_why{ "none" };
    std::atomic<int> m_fp_trace{ 0 };       // afwfptrace N: per-present lines for N presents
    uint32_t    m_fp_frame_draws{ 0 };      // replayed draws in the current frame (afwfptrace)
    void fp_eye_feed(int frame, int eye);
    // afwfgprobe N (research): for N frames, each pass's depth state at its first draw and its projection's depth terms,
    // and the scene depth/stencil under the gun at present (the doubled weapon in AFW).
    std::atomic<int>   m_fg_probe{ 0 };
    int                m_fg_probe_seen[2]{ -1, -1 };
    float              m_pass_projz[2][2]{};      // [world, weapon][P22, P32] of the projection written this frame
    void fg_probe_draw(ID3D11DeviceContext* ctx);
    void fg_probe_present(ID3D11DeviceContext* ctx);
    std::atomic<bool>  m_camscan_req{ false };     // research: camera position copies in CPU memory (camscan, camtrack N)
    std::atomic<int>   m_camtrack_left{ 0 };   // snap command: an F6 snapshot on the next probe beat   // testctrl on: the simulated controller even with a headset (calibration tests)
    float              m_deg_per_count{ 0.05f };
    float              m_last_yaw{ 0.0f };
    int                m_last_sent_x{ 0 };
    float              m_prev_yaw_seen{ 0.0f };
    int                m_unanswered{ 0 };
    std::chrono::steady_clock::time_point m_aim_paused_until{};
    float              m_aim_err_deg{ 0.0f }, m_aim_err_max{ 0.0f };
    uint32_t           m_player_views_last_frame{ 0 };
    std::chrono::steady_clock::time_point m_last_present_time{};
    glm::mat4          m_world_proj{ 1.0f };
    bool               m_world_proj_valid{ false };
    struct PassHistory
    {
        int       cur_frame{ -1 };
        bool      prev_valid{ false };
        glm::mat4 cur_vrp{ 1.0f }, prev_vrp{ 1.0f };
        glm::vec3 cur_cam{ 0.0f }, prev_cam{ 0.0f };
    };
    PassHistory        m_hist[2]{};   // 0 world, 1 first-person pass
    std::unordered_set<ID3D11PixelShader*> m_particle_ps{};   // flameworld: particle shaders (CSceneParticleEmitterRendererParameterProvider)
    // flameworld (FarCry5Stereo.cpp, flame_upload): weapon-pass particles drawn with the eye's own camera, moved only so the game's
    // muzzle sits on the drawn one, so flames already emitted stay in the world.
    std::atomic<bool>  m_flame_world{ true };
    struct FlameCb
    {
        ID3D11Resource*    res{ nullptr };   // the game's weapon camera buffer
        int                frame{ -100 };
        std::vector<float> data;             // its contents with the particle camera
    };
    static constexpr int kFlameSlots = 8;
    FlameCb            m_flame_cb[kFlameSlots]{};
    ID3D11Buffer*      m_flame_standin[kFlameSlots]{};
    UINT               m_flame_standin_size[kFlameSlots]{};
    PassHistory        m_flame_hist{};
    ID3D11Buffer*      m_flame_cb_orig{ nullptr };
    bool               m_flame_cb_swapped{ false };
    uint64_t           m_flame_draws{ 0 }, m_flame_missed{ 0 };
    glm::vec3          m_flame_shift{ 0.0f };
    std::atomic<float> m_flame_hold{ 1.5f };   // flamehold: seconds after firing before weapon-pass particles go back on the gun
    std::chrono::steady_clock::time_point m_flame_fire_t{};   // the trigger last seen held
    float              m_flame_attach{ 1.0f };   // 0 in the world .. 1 on the gun (last upload)
    void               flame_upload(const float* d, uint32_t size, int frame, const glm::mat3& eye_rot, const glm::vec3& eye_cam, const glm::vec3& cam,
                                    const glm::mat3& w_full, const glm::vec3& w_off, const glm::mat4& proj);
    bool               flame_cb_swap(ID3D11DeviceContext* ctx);
    void               flame_cb_restore(ID3D11DeviceContext* ctx);
    std::atomic<float> m_debug_yaw_amp{ 0.0f };     // flat test: simulated head yaw sweep (degrees)
    std::atomic<int>   m_debug_yaw_period{ 90 };    // frames per sweep
    std::atomic<float> m_debug_sway_pos{ 0.0f };    // test head sideways sway, metres
    std::atomic<bool>  m_pd_after_swap{ true };
    std::atomic<int>   m_prevmotion_log{ 0 };
    std::atomic<bool>  m_debug_sway_aim{ false };
    std::atomic<int>   m_aim_why{ 0 };   // test head yaw also steers the game camera
    std::atomic<bool>  m_indirect_history{ true };  // per-eye history for indirect compute passes even with reflections off     // passdump reads inputs after the per-eye history swap
    std::atomic<int>   m_vm_test_index{ -1 };       // discovery: the player view index to move
    std::atomic<float> m_vm_test_off[3]{};
    FrameCams          m_afw_cams[4]{};
    float              m_depth_a{ 1e-5f }, m_depth_b{ 0.1f };
    ID3D11Texture2D*   m_scene_depth{ nullptr };
    uint64_t           m_scene_depth_checked{ 0 };
    std::atomic<bool>  m_afw_test{ false };      // capture + rebuild every present (no headset needed)
    std::atomic<bool>  m_afw_preview{ false };
    std::atomic<bool>  m_afw_measure{ false };   // desktop shows the left eye always (native or rebuilt)

    // The previous frame's rewritten view-rotation-projection and the game's view rotation it was
    // built from: the next frame's "previous" matrix (motion vectors, TAA) follows the eye swap.
    int       m_cur_frame{ -1 };
    bool      m_prev_valid{ false };
    glm::mat4 m_cur_vrp{ 1.0f }, m_prev_vrp{ 1.0f };
    glm::vec3 m_cur_cam{ 0.0f }, m_prev_cam{ 0.0f };

    // Headset reference (stage space): the game camera sits here. Yaw-only orientation.
    std::mutex m_ref_mtx{};
    bool       m_have_ref{ false };
    glm::mat4  m_ref_inv{ 1.0f };
    bool       m_recenter_requested{ true };
    // Recentring and head height (author 23 Oct): VDXR's first head pose read exactly (0, -0.97, 0) facing 0 deg, the startup recentre took it and the whole
    // session sat 1 m high; the headset's own recenter did not reach the mod. The first recentre waits for a tracked pose
    // (up to 3 s); loadrecenter (default on, saved): back in play after a loading screen somewhere else (a save, a respawn,
    // fast travel; or the first play), the view is recentred; headsetrecenter (default on, saved): the runtime's own
    // recenter (its LOCAL space moved) is followed by the mod's recentre.
    std::atomic<bool>  m_load_recenter{ true };
    std::atomic<bool>  m_headset_recenter{ true };
    uint32_t           m_local_changes_seen{ 0 };
    bool               m_local_change_pending{ false };
    int64_t            m_local_change_time{ 0 };
    std::chrono::steady_clock::time_point m_local_change_at{};
    glm::vec3          m_menu_cam_before{ 0.0f };
    bool               m_played_once{ false };
    // startrecenter (author 2 Nov 12:54; saved, seconds, default 3, 0 off):
    // once per game session, this long after the mod's first recentre at launch (during the intro credits), the view and the
    // screens are recentred again on where the head is then, whatever screen is up.
    std::atomic<float> m_start_recenter_s{ 3.0f };
    bool               m_start_recenter_armed{ false };
    bool               m_start_recenter_done{ false };
    std::chrono::steady_clock::time_point m_start_recenter_at{};
    // recenterscreens (2 Nov, same request; saved, default on): every recentre of the mod (the first one at launch, startrecenter, F9,
    // loadrecenter, auto recenter...) also moves the flat screen (credits, game menus, loading) and the F11 panel in front of the
    // head (VR::recenter_view). They were placed once, from the runtime's first pose at launch: VDXR's first poses are untracked
    // and face 0 deg, so the credits and F11 could sit behind the player for the whole session. Cinematics keep their own anchor.
    std::atomic<bool>  m_recenter_screens{ true };
    uint64_t           m_recenter_untracked{ 0 };
    std::chrono::steady_clock::time_point m_recenter_wait_since{};

    // Settings (live; command file and F11 panel)
    std::atomic<bool>  m_stereo_enabled{ true };
    std::atomic<bool>  m_use_hmd_rotation{ true };
    std::atomic<bool>  m_use_hmd_position{ true };
    std::atomic<bool>  m_keep_game_jitter{ true };
    std::atomic<float> m_world_scale{ 1.0f };             // game units (metres) per real metre
    std::atomic<float> m_debug_eye_offset{ 0.0f };         // flat test without headset: metres, + = right
    std::atomic<bool>  m_debug_alternate{ false };         // flat test: alternate the offset's sign per frame
    std::atomic<float> m_debug_fov_scale{ 1.0f };          // flat test: scale the projection's tangents
    std::atomic<int>   m_dump_requests{ 0 };
    std::atomic<bool>  m_log_layout{ true };
    std::string        m_last_command{};

    std::chrono::steady_clock::time_point m_last_poll{};
    std::chrono::steady_clock::time_point m_last_stats{};
};
