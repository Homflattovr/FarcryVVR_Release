#pragma once

// DLSS (DLAA at the eye's render size) in place of the game's SMAA resolve, one NGX feature per eye:
// alternate-eye rendering gives each eye its own history. Inputs: SMAA's input colour, the scene
// depth and the game's encoded motion vectors (its motion-vector compute pass), the mod's own
// projection jitter.

#include <cstdint>
#include <d3d11.h>

#include <string>

namespace fc5::dlss
{
    void set_enabled(bool on);
    // ofxrguides (default on): OFXR Bridge's DLSS frame generation needs each eye image tagged with the DLSS evaluation it
    // came from; the tag OFXR puts on the DLSS output is carried to the eye's swapchain image before its release.
    void tag_xr_image(int eye, ID3D11Texture2D* image);
    void set_ofxr_tags(bool on);
    bool ofxr_tags();
    std::string ofxr_tag_stats();   // since the last call
    // ofxrdlss11 on|off (saved, default on; OFXR thread 6 Nov): when OFXR Bridge's route takes DLSS data (DLSS or FSR frame
    // generation, or DLSS motion vectors) and gets none from the D3D12 path after 3 new features, DLSS runs on D3D11 for the rest of
    // the game run (OFXR's D3D11 route with receipts). Back on D3D12 when OFXR's route stops taking DLSS data.
    void set_ofxr_fallback(bool on);
    bool ofxr_fallback();
    std::string ofxr_status();   // OFXR's route, whether it takes DLSS data, its publication count (F11)
    // NGX DLSS feature flags (1 HDR, 2 MV low res, 4 MV jittered, 8 depth inverted, 64 auto exposure); recreates the features.
    void set_flags(int flags);
    // The DLSS model preset (0 default, 10 J, 11 K = DLSS 4 transformer); recreates the features.
    void set_preset(int preset);
    int  preset();
    void set_debug(int mode);
    // perfdlss12 (performance thread, 6 Nov; saved, default on since author 6 Nov 10:21): NGX evaluates DLSS through its D3D12 path on the mod's own
    // D3D12 queue (shared textures and fences, GPU-side waits only) instead of its D3D11 call, which held the render thread.
    void        set_d3d12(bool on);
    bool        d3d12();
    // perfdlss12flush on|off (test, not saved, default on): the D3D11 flush that sends its fence signal to the D3D12 queue
    // (off: the signal goes with D3D11's next submission; [DlssEval] shows what the flush costs).
    void        set_d3d12_flush(bool on);
    // perfdlss12thread on|off (saved, default on since 6 Nov): NGX's D3D12 recording (1.9 ms per evaluation) and the submission on a worker
    // thread; the render thread only signals, flushes and queues D3D11's GPU wait.
    void        set_d3d12_thread(bool on);
    bool        d3d12_thread();
    std::string d3d12_status();
    // [DlssEval] (with perfsplit on): the evaluation call's CPU time and how often the GPU had already caught up at its return.
    std::string eval_summary();
    // Contrast-adaptive sharpening after DLSS (0 off .. 1).
    void set_sharpness(float s);
    float sharpness();
    bool enabled();

    // The motion-vector compute pass (#47C5C6CD) of this frame: its depth input and encoded output.
    void note_motion_pass(ID3D11Resource* depth, DXGI_FORMAT depth_view_format, ID3D11Resource* encoded_mv);
    void motion_stat(int n);   // cmd dlssmvstat N (test): how much of the eye takes the game's vector, next N evaluations
    void set_motion_best(bool on);   // cmd dlssmvbest: each pixel's game vector compared with the camera motion with and without the move
    bool motion_best();
    void set_motion_check(bool on);   // cmd dlssmvcheck: an object's own motion only where this eye's previous depth confirms it
    bool motion_check();
    void  set_motion_sway(float px);   // cmd dlssmvsway on|off|<px>: swaying foliage's own motion passes above px + 5 % (0 = off, on = 0.5)
    float motion_sway();
    void motion_dump(int n);   // cmd dlssmvdump N (test): game, camera and final vectors and depth to probes
    void set_dlaa_phases(int n);   // cmd dlaaphases 8|16 (research 15 Oct): jitter phases per eye in DLAA (no upscaling)
    int  dlaa_phases();

    // At the SMAA resolve draw: evaluates DLSS for this eye into the bound target. True when the
    // output was written (the SMAA draw is then skipped).
    bool resolve(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11RenderTargetView* target, int eye, float jitter_x_px, float jitter_y_px,
                 bool reset);

    // Sub-pixel jitter for the given frame in pixels (Halton 2,3), and its use for the projection.
    void jitter_for(int frame, float& x_px, float& y_px);

    // Upscaling: the eye's size when the game renders below it (0: DLAA at the render size).
    void set_output_size(UINT w, UINT h);
    // After the SMAA draw (upscaling only): keeps the low-resolution image before the HUD.
    void after_smaa(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target);
    // At present (upscaling only): the eye image at the eye's size = DLSS output with the HUD
    // (what the back buffer adds over the pre-HUD image) laid on top. Null when not upscaling.
    ID3D11Texture2D* compose(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11ShaderResourceView* hud_layer = nullptr);
    // Eye images composed per HUD method since the last call (layer over DLSS, layer over the stretched
    // game image, difference).
    std::string compose_stats();
    // Exact camera motion for DLSS (this eye's inverse camera and previous camera, rows), and the mode:
    // 0 the game's vectors, 1 camera motion + the game's where something moves (default), 2 camera only.
    void set_camera(const float* inv_rows, const float* prev_rows, const float* prev_rot_rows);
    // afwvehmv: after set_camera, for this camera: nearer than `metres` the motion of points fixed in the vehicle (rows: this
    // eye's clip -> its previous image's clip through the vehicle's frame), in place of the game's vectors.
    void set_body_motion(const float* rows, float metres);
    // slingmv (FarCry5Stereo.h), for the next evaluation only: a weapon-pass pixel (stencil 0x40) whose point (clip -> this eye's view
    // through inv_proj_rows) lies within `radius` m of `centre` and not within hand_r[i] (0: none) of hands[3i..3i+2] moves by `rows`
    // (this eye's clip -> its previous image's clip), in place of the game's vector.
    void set_sling_motion(const float* rows, const float* inv_proj_rows, const float a[3], const float b[3], float radius, const float hands[6], const float hand_r[2]);
    void clear_sling_motion();
    void sling_motion_stats(uint64_t& with_stencil, uint64_t& by_shape);   // DLSS evaluations since the last call
    void set_camera_motion(int mode, float sign);
    // AFW (afwobjmv): the game's own object motion (its vector minus the camera's, where something moves) times s,
    // for an eye whose previous image is s game updates old. 1 = unchanged (stereo pairs).
    void set_object_motion_scale(float s);
    void set_object_motion_gun(bool on);   // afwobjmvgun (research): the weapon pass scaled too (the doubled gun of 12 Oct)
    // Pipeline review P7 (cmd dlsstrim N, bits): 1 = DLSS reads the camera motion texture itself (no copy),
    // 2 = no pre-HUD copy when the HUD is its own layer, 4 = no low-resolution back-buffer copy when DLSS produced the image.
    void set_trims(int bits);
    // HUD report A: the next compose draws the HUD as a flat screen in front of the head, seen from this eye.
    // tan = the eye image's edges (left, right, up, down tangents), eye = eye position from the head centre in head
    // axes (m), dist (m), half sizes as tangents from the head centre, yoff = screen centre height (tangent).
    void set_hud_screen(const float tan[4], const float eye[3], float dist, float half_x, float half_y, float yoff);
    uint64_t hud_screen_frames();
    // The F11 menu drawn into this frame's composed eye image (eye = what compose returned) as a flat panel:
    // params = this eye's tangents (l r u d), then the panel centre, right axis / half width, up axis / half height
    // and normal in the eye's axes (4 floats each, the 4th unused). False when nothing was drawn.
    bool draw_panel(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, ID3D11Texture2D* ui, const float params[20]);
    uint64_t panel_frames();
    int trims();
    int  camera_motion();
    void compare_motion(int n);
    bool upscaling();
    // Output width / render width while upscaling (1 otherwise).
    float upscale_ratio();

    std::string status();
    void        new_frame();
    // This frame's DLSS output before the HUD (eye size), null without one (scopesrc reads the scope's view from it).
    ID3D11ShaderResourceView* output_srv();
}
