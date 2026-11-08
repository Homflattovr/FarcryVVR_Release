#include "FarCry5Stereo.h"
#include "FarCry5Aim.h"
#include "FarCry5Dlss.h"
#include "FarCry5Bow.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

// Red dot sights (cmd reddot, off by default; F11 Weapon > "Red dot sights").
//
// The game paints a reflex or holographic sight's dot on the sight's glass: a reticle texture mapped on the glass mesh
// with fixed UVs (vertex shaders #B5492747/#E5DDC9A6 with Weapon_Reticle, #8B10D54B etc. with Weapon_ReticleGlow), no
// projection to distance, faded by the angle between the glass and the view (ViewFadeBias/Power). Flat, that works
// because aiming down sights puts the game camera on the sight's axis. In VR the dot is seen at the glass's distance
// (a double image when the eyes are on the target) and moves off the aim point as soon as the eye is off that axis.
//
// With reddot on, those draws no longer draw the game's dot. They instead write a mask (render size, depth
// tested against the gun, so only the visible window counts), and at present each eye gets a dot at the aim point
// (where the shots land: the barrel ray's hit, else the convergence distance) projected into that eye, drawn only
// where the mask says the window is. The dot is at the target's distance, so it stays on target wherever the eye is
// behind the glass, and leaves the window when the eye moves too far off the sight, like a real reflex sight.
//
// reddotholo (on by default; author 30 Oct): a
// holographic reticle: a ring (reddotring MOA across, 65 by default) with four ticks through it at 12, 3, 6 and 9 o'clock
// and a small centre dot (the author's 1 Nov reference picture). It is placed exactly like the dot (at the aim point,
// projected into each eye), so both eyes fuse it on the target as a real holographic sight does, and it is cut by the
// same window mask: a small reflex window shows part of the ring.
//
// reddotmode (saved, default laser; author 5 Nov), F11 Weapon > Sights "Red dot and laser", the one choice. The laser is the aim marker (draw_aim_dot, or reddot_draw outside the
// window). realistic: the sights' dots only. laser: the laser on guns without a reflex sight. arcade: the laser on all guns, also around a
// reflex sight's window. It sets m_aim_dot, m_reddot and m_reddot_only; the commands aimdot, reddot and reddotonly remain as tests, not saved.
namespace fc5::reddot
{
    namespace
    {
        // The mask: every pixel the sight's glass covers.
        const char* kMaskPs = R"(
float4 main() : SV_Target0 { return float4(1.0f, 1.0f, 1.0f, 1.0f); }
)";
        // The dot, drawn into a box of the eye image (copied first: typed UAV loads are not allowed in cs_5_0).
        const char* kDotCs = R"(
Texture2D<float4>   g_src  : register(t0);
Texture2D<float>    g_mask : register(t1);
RWTexture2D<float4> g_dst  : register(u0);
SamplerState        g_lin  : register(s0);
cbuffer P : register(b0)
{
    float2 c;      float r;    float glow;
    float2 origin; float2 box;
    float4 col;
    float2 mscale; float2 moff;
    float  sq;     float disc;                 // the aim marker's size (0 = none) and shape, as draw_aim_dot draws it
    float  ring;   float ringw;                // reddotholo: the ring's radius and half line width in pixels (0 = plain dot)
};
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)box.x || id.y >= (uint)box.y) return;
    const float2 q = float2(id) + origin + 0.5f;   // eye pixel centre
    const float  d = length(q - c);
    float        s = d - r;                        // distance to the dot's edge
    float        g = glow > 0.0f ? 0.45f * exp(-(d * d) / (glow * glow)) : 0.0f;
    // reddotholo: the ring, four slightly bolder ticks through it at 12, 3, 6 and 9 o'clock (0.82 to 1.35 of its radius,
    // round ends) and the small dot, as one shape (s = distance to its edge); glow is then a faint soft edge's width.
    if (ring > 0.0f) {
        const float2 v  = abs(q - c);
        const float  t0 = 0.82f * ring, t1 = 1.35f * ring, tw = 1.25f * ringw;
        s = min(s, abs(d - ring) - ringw);
        s = min(s, length(v - float2(clamp(v.x, t0, t1), 0.0f)) - tw);
        s = min(s, length(v - float2(0.0f, clamp(v.y, t0, t1))) - tw);
        const float so = max(s, 0.0f);
        g = glow > 0.0f ? 0.2f * exp(-(so * so) / (glow * glow)) : 0.0f;
    }
    const float  a = saturate(0.5f - s);           // anti-aliased edge
    const float  m = disc < 0.0f ? 1.0f : saturate(g_mask.SampleLevel(g_lin, q * mscale + moff, 0));   // bowpin (disc < 0): no window
    float3       o = g_src[id].rgb;
    // Outside the sight's window: the usual aim marker, so hip fire keeps it. The red laser dot (radius sq, dark rim)...
    if (sq > 0.0f && disc > 0.0f) {
        const float rim = saturate(sq + 1.5f - d), core = saturate(sq + 0.5f - d);
        o = lerp(o, float3(0.0f, 0.0f, 0.0f), rim * (1.0f - m));
        o = lerp(o, float3(1.0f, 0.08f, 0.05f), core * (1.0f - m));
    }
    // Through the window: the red dot.
    g_dst[id] = float4(lerp(o, col.rgb, saturate(max(a, g)) * m), g_src[id].a);
}
)";
        constexpr UINT kBox = 256;   // largest box (dot + glow, or the holographic ring) in eye pixels

        struct Cb { float v[20]; };
        ID3D11PixelShader*         g_ps      = nullptr;
        ID3D11ComputeShader*       g_cs      = nullptr;
        ID3D11Buffer*              g_cb      = nullptr;
        ID3D11SamplerState*        g_smp     = nullptr;
        bool                       g_failed  = false;
        ID3D11Texture2D*           g_mask    = nullptr;
        ID3D11RenderTargetView*    g_mrtv    = nullptr;
        ID3D11ShaderResourceView*  g_msrv    = nullptr;
        UINT                       g_mw = 0, g_mh = 0;
        ID3D11Texture2D*           g_stash   = nullptr;
        ID3D11ShaderResourceView*  g_ssrv    = nullptr;
        DXGI_FORMAT                g_sfmt    = DXGI_FORMAT_UNKNOWN;
        ID3D11Texture2D*           g_work    = nullptr;
        ID3D11UnorderedAccessView* g_wuav    = nullptr;

        bool init(ID3D11Device* dev)
        {
            if ((g_ps && g_cs) || g_failed) return !g_failed;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            auto compile = [&](const char* src, const char* name, const char* target) -> ID3DBlob* {
                ID3DBlob* code = nullptr;
                ID3DBlob* err  = nullptr;
                if (!compile_fn || FAILED(compile_fn(src, strlen(src), name, nullptr, nullptr, "main", target, 0, 0, &code, &err))) {
                    spdlog::error("[RedDot] {} failed: {}", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                    if (err) err->Release();
                    return nullptr;
                }
                return code;
            };
            ID3DBlob* ps = compile(kMaskPs, "reddot_mask", "ps_5_0");
            ID3DBlob* cs = compile(kDotCs, "reddot_dot", "cs_5_0");
            if (ps) { dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps); ps->Release(); }
            if (cs) { dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_cs); cs->Release(); }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_cb);
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            dev->CreateSamplerState(&sd, &g_smp);
            g_failed = !(g_ps && g_cs && g_cb && g_smp);
            spdlog::info("[RedDot] shaders {}", g_failed ? "unavailable" : "ready");
            return !g_failed;
        }

        // A typeless format and its UNORM view (an sRGB back buffer is read and written without conversion).
        void family(DXGI_FORMAT f, DXGI_FORMAT& tl, DXGI_FORMAT& un)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                tl = DXGI_FORMAT_R8G8B8A8_TYPELESS; un = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                tl = DXGI_FORMAT_B8G8R8A8_TYPELESS; un = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                tl = DXGI_FORMAT_R10G10B10A2_TYPELESS; un = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                tl = DXGI_FORMAT_R16G16B16A16_TYPELESS; un = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            default: tl = f; un = f; break;
            }
        }

        // The mask at the size of the target the glass is drawn into.
        bool mask_for(ID3D11Device* dev, UINT w, UINT h)
        {
            if (g_mask && g_mw == w && g_mh == h) return true;
            if (g_msrv) g_msrv->Release();
            if (g_mrtv) g_mrtv->Release();
            if (g_mask) g_mask->Release();
            g_msrv = nullptr; g_mrtv = nullptr; g_mask = nullptr;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = w; nd.Height = h; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc.Count = 1;
            nd.Format = DXGI_FORMAT_R8_UNORM; nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_mask))) {
                dev->CreateRenderTargetView(g_mask, nullptr, &g_mrtv);
                dev->CreateShaderResourceView(g_mask, nullptr, &g_msrv);
            }
            g_mw = w; g_mh = h;
            spdlog::info("[RedDot] sight window mask {}x{}: {}", w, h, g_mrtv && g_msrv ? "ready" : "failed");
            return g_mrtv && g_msrv;
        }
    }
}

// A reticle draw of the weapon pass (its pixel shader binds Weapon_Reticle or Weapon_ReticleGlow). The draw goes ahead
// with the mask shader into the window mask instead of the game's targets (reddot_end restores the game's state after
// it): the game's painted dot is gone and the pixels where the reticle mesh is visible are known.
void FarCry5Stereo::reddot_begin(ID3D11DeviceContext* ctx)
{
    using namespace fc5::reddot;
    if (!m_reddot.load() || !m_motion.load() || m_scope_call || reddot_excluded()) return;
    m_reddot_reticle_draws++;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    // The mask has the size of the depth target (or of the first colour target without one).
    ID3D11Resource* res = nullptr;
    if (dsv) dsv->GetResource(&res);
    else if (rtvs[0]) rtvs[0]->GetResource(&res);
    D3D11_TEXTURE2D_DESC td{};
    if (res) { static_cast<ID3D11Texture2D*>(res)->GetDesc(&td); res->Release(); }
    bool ok = td.Width > 0 && td.SampleDesc.Count == 1 && init(dev) && mask_for(dev, td.Width, td.Height);
    dev->Release();
    if (!ok) {
        for (auto* v : rtvs) if (v) v->Release();
        if (dsv) dsv->Release();
        m_reddot_fails++;
        return;   // the game's own reticle stays
    }
    const int fr = m_frame.load();
    if (m_reddot_mask_frame != fr) {
        const float zero[4]{};
        ctx->ClearRenderTargetView(g_mrtv, zero);
        m_reddot_mask_frame = fr;
    }
    UINT           nvp = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&nvp, &vp);
    m_reddot_vp[0] = vp.TopLeftX; m_reddot_vp[1] = vp.TopLeftY; m_reddot_vp[2] = vp.Width; m_reddot_vp[3] = vp.Height;
    m_reddot_mask_wh[0] = (float)td.Width; m_reddot_mask_wh[1] = (float)td.Height;
    // Save what the draw changes; the game's colour targets are kept (and put back) as they are.
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) m_reddot_saved.rtv[i] = rtvs[i];
    m_reddot_saved.dsv = dsv;
    ctx->OMGetBlendState(&m_reddot_saved.blend, m_reddot_saved.factor, &m_reddot_saved.mask);
    m_reddot_saved.ninst = 16;
    ctx->PSGetShader(&m_reddot_saved.ps, m_reddot_saved.inst, &m_reddot_saved.ninst);
    ctx->OMSetRenderTargets(1, &g_mrtv, dsv);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->PSSetShader(g_ps, nullptr, 0);
    m_reddot_swapped = true;
    m_reddot_mask_draws++;
    if (m_reddot_log.load() > 0) {
        m_reddot_log--;
        std::string names;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_ps_names.find(m_reddot_saved.ps); it != m_ps_names.end()) names = it->second;
        }
        spdlog::info("[RedDot] glass draw into the mask, frame {}: target {}x{} viewport ({:.0f},{:.0f} {:.0f}x{:.0f}) depth {} | ps {}", fr, td.Width, td.Height, vp.TopLeftX,
                     vp.TopLeftY, vp.Width, vp.Height, dsv ? "yes" : "no", names.substr(0, 160));
    }
}

void FarCry5Stereo::reddot_end(ID3D11DeviceContext* ctx)
{
    if (!m_reddot_swapped) return;
    m_reddot_swapped = false;
    auto& s = m_reddot_saved;
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    ctx->OMSetBlendState(s.blend, s.factor, s.mask);
    ctx->PSSetShader(s.ps, s.inst, s.ninst);
    for (auto*& v : s.rtv) { if (v) v->Release(); v = nullptr; }
    if (s.dsv) s.dsv->Release();
    if (s.blend) s.blend->Release();
    if (s.ps) s.ps->Release();
    for (UINT i = 0; i < s.ninst && i < 16; ++i) if (s.inst[i]) { s.inst[i]->Release(); s.inst[i] = nullptr; }
    s.dsv = nullptr; s.blend = nullptr; s.ps = nullptr; s.ninst = 0;
}

// Magnifying scopes keep their own reticle (and the scope lens): only weapons whose scope entry is above 1x, and any
// frame where the scope lens is in use.
bool FarCry5Stereo::reddot_excluded()
{
    // manualbow (author 2 Nov 12:01): a bow's pin sight is a few millimetres of glow,
    // so the aim point seen from each eye rarely fell inside it and the dot showed in one eye or none. The bow keeps the game's own pin
    // glow (on the sight itself, seen by both eyes).
    if ((m_mo.bow || m_bow_slung_now) && m_motion.load()) return true;   // bowsling: nor on the sling
    if (m_scope.load() && m_scope_st.active && m_scope_st.frame == m_frame.load()) return true;
    std::lock_guard _{ m_scope_mtx };
    const auto it = m_scope_cal.find(m_weapon_sig.load());
    return it != m_scope_cal.end() && it->second.mag > 1.01f;
}

// reddotglass, a weapon draw: the closed red dot sight's dark disc (and at 2 its glass) on a gun with a reticle and no
// magnifying scope is left out (true).
bool FarCry5Stereo::reddot_glass_draw()
{
    const int mode = m_reddot_glass.load();
    bool      tint = false, glass = false;
    {
        std::lock_guard _{ m_ps_mtx };
        tint  = m_sight_tint_ps.contains(m_cur_ps);
        glass = mode >= 2 && m_sight_glass_ps.contains(m_cur_ps);
    }
    if (!tint && !glass) return false;
    const uint32_t sig = m_weapon_sig.load();
    if (sig == 0 || m_reddot_reticle_sig.load() != sig || reddot_excluded()) return false;
    m_reddot_glass_hidden++;
    return true;
}

// The aim marker was drawn by reddot_draw in this eye frame (outside the sight's window only): draw_aim_dot skips it.
bool FarCry5Stereo::reddot_has_sight() const
{
    return m_reddot.load() && m_reddot_drawn_frame == m_frame.load();
}

// reddotmode laser, a gun with a reflex sight: no laser in an eye frame without the sight's glass draw either (reddot_draw then does not
// run, for one when the sight is out of the game's view). A magnifying scope (reddot_excluded) and the bow are guns without a reflex sight.
bool FarCry5Stereo::reddot_no_laser()
{
    return m_reddot.load() && m_reddot_only.load() && m_gun_has_sight.load() && !reddot_excluded();
}

int FarCry5Stereo::reddot_mode() const
{
    if (!m_aim_dot.load()) return 0;
    return m_reddot.load() && m_reddot_only.load() ? 1 : 2;
}

const char* FarCry5Stereo::reddot_mode_name(int mode)
{
    return mode == 0 ? "realistic" : mode == 1 ? "laser" : "arcade";
}

void FarCry5Stereo::reddot_set_mode(int mode)
{
    m_reddot      = true;
    m_aim_dot     = mode != 0;
    m_reddot_only = mode != 2;
    m_persist["reddotmode"] = fmt::format("reddotmode {}", reddot_mode_name(mode));
    spdlog::info("[RedDot] reddotmode {}: {}", reddot_mode_name(mode), mode == 0 ? "the red dot only in reflex sights, no laser" :
                 mode == 1 ? "the red dot only in reflex sights, a laser on guns without one" : "the red dot and a laser on all guns");
}

// Settings saved before reddotmode: the nearest of the three choices. The old "Off" (nothing drawn) is realistic and the old "Aim dot only"
// (a dot on every gun) arcade. True when the settings must be written again, without the old lines.
bool FarCry5Stereo::reddot_settings_migrate()
{
    if (!m_reddot_old_lines) return false;
    m_reddot_old_lines = false;
    reddot_set_mode(reddot_mode());
    return true;
}

// PRESENT: the dot into this eye's image (the DLSS eye image or, in the full-size mode, the back buffer), where the sight's window is.
void FarCry5Stereo::reddot_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    using namespace fc5::reddot;
    const int fr = m_frame.load();
    // bowpin (author 2 Nov 12:26): with the manual bow, a small dot on the bow's sight pin itself
    // (where its glow draw sits: m_bow_pin_q, FarCry5BowArrow.cpp), drawn into each eye: the game drew its pin's glow in one eye only.
    // bowpin aim (default; author 2 Nov 13:40): the
    // pin is a hand's length from the eye and the eye sits above the arrow, so the pin on a target shot high, and looking at the target each
    // eye saw the pin's dot elsewhere (one of the two is lost). The dot goes where the arrow's line meets the scene instead, the same point in
    // both eyes, as a reflex sight's.
    const int  pmode   = fc5::bow::pin_mode();
    const bool bowdot  = m_mo.bow && pmode != 0 && m_motion.load();
    const bool bowaim  = bowdot && pmode == 2;
    const bool pin_new = bowaim || fr - m_bow_pin_frame <= 30, map_new = bowaim || m_frame.load() - m_hand_map.frame <= 1;
    const bool pin     = bowdot && pin_new && map_new;
    // Each eye frame's outcome counted for a 5 s line (author 2 Nov 12:52: still not a dot in each eye): 0 left, 1 right, 2 no sight draw,
    // 3 hand placement old, 4 off the eye's picture, 5 other.
    int pin_out = !bowdot ? -1 : !pin_new ? 2 : !map_new ? 3 : 5;
    struct Tally
    {
        FarCry5Stereo* t;
        const int&     slot;
        int            frame;
        ~Tally() { if (slot >= 0) t->bow_pin_tally(slot, frame); }
    } tally{ this, pin_out, fr };
    // bowdotarrow (author 5 Nov 22:47): no dot with the bow until an arrow
    // is on it; the aim marker is left out too (draw_aim_dot).
    if (m_mo.bow && m_motion.load() && !fc5::bow::dot_wanted()) {
        pin_out = -1;
        return;
    }
    if (!m_reddot.load() || !eye || (!pin && (m_reddot_mask_frame != fr || !g_msrv)) || !m_motion.load() || !m_mo.ctrl_valid || m_menu_screen_on) return;
    if (m_hist[0].cur_frame != fr || m_scope_src_frame == fr) return;
    // bowdoteye (author 3 Nov 14:38): the bow's dot is lined up for the
    // aiming eye (bowsighteye: the string hand's eye, the right one unless left-handed), so the other eye gets none, nor the aim marker.
    if (pin && fc5::bow::dot_one_eye() && ((fr & 1) != 0) != !m_left_handed.load()) {
        m_reddot_drawn_frame = fr;
        pin_out              = -1;
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    eye->GetDesc(&d);
    // Where the shots land, seen from this eye.
    const float     hit  = fc5::aim::last_hit();
    // Barrel hits under 2 m are ignored (the march can meet the gun itself at the eye: 0.6 m in the simulator), as the scope camera does.
    const float     far_m = m_mo.converge > 2.0f ? m_mo.converge : m_converge_m.load();
    // sightdepth (FarCry5Sight.cpp): the eased distance, so an eye off the shots' line does not see the dot jump with every raw hit.
    // bowpin aim: the shots' own distance (m_mo.converge, held over a miss) in both eyes; this eye's barrel hit put the two eyes' dots at
    // 4.5-6 m and 25 m in one frame pair (simulator run, research\bowcal_2026-11-03).
    const float     dist = m_reddot_dist.load() > 0.5f ? m_reddot_dist.load() : (bowaim && m_mo.converge > 2.0f) ? m_mo.converge : m_mo.sight_dist > 0.0f ? m_mo.sight_dist : (hit > 2.0f ? hit : far_m);
    // bowpin: this frame's pin where the bow was drawn (bow_pin_note), else the last one seen, placed with the latest hand placement.
    const glm::vec3 p    = (!pin || bowaim) ? m_mo.aim_origin + m_mo.barrel_dir * dist
                         : m_bow_pin_frame == fr ? m_bow_pin_w
                                                 : m_hand_map.w_pos + m_hand_map.w_rot * (m_bow_pin_q - m_hand_map.grip);
    auto project = [&](const glm::vec3& w, glm::vec2& out) {
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ w - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.01f) return false;
        out = { (cl.x / cl.w * 0.5f + 0.5f) * d.Width, (0.5f - cl.y / cl.w * 0.5f) * d.Height };
        return true;
    };
    glm::vec2 c{}, e{};
    if (pin) pin_out = 4;
    if (!project(p, c)) return;
    // The dot's size: an angle (minutes of arc, diameter), measured in this eye's pixels at the dot.
    const glm::vec3 ray  = p - m_hist[0].cur_cam;
    const float     len  = glm::length(ray);
    glm::vec3       side = glm::cross(ray / std::max(len, 1e-3f), glm::vec3{ 0, 0, 1 });
    if (glm::length(side) < 1e-3f) side = { 1, 0, 0 };
    const float half = glm::radians(m_reddot_moa.load() / 60.0f) * 0.5f;
    // bowpin: 3 mm across on the pin; 15 MOA where the arrow lands.
    if (!project(pin && !bowaim ? p + glm::normalize(side) * 0.0015f
                                : p + glm::normalize(side) * len * std::tan(bowaim ? glm::radians(15.0f / 60.0f) * 0.5f : half), e)) return;
    float r    = std::max(1.0f, glm::length(e - c));
    float glow = m_reddot_glow.load() ? 2.5f * r : 0.0f;
    // reddotholo: the ring's radius, measured the same way from its diameter. The look follows the author's 1 Nov picture (the
    // 30 Oct thin ring was not liked): a line 16 % of the radius wide, the ticks, a small centre dot (reddotsize
    // sizes the plain dot only) and, with reddotglow, a soft edge along the lines instead of the glow around the dot.
    float ring = 0.0f, ringw = 0.0f;
    if (m_reddot_holo.load() && !pin) {
        const float ring_moa = m_reddot_ring_moa.load();
        glm::vec2   re{};
        if (project(p + glm::normalize(side) * len * std::tan(glm::radians(ring_moa / 60.0f) * 0.5f), re)) {
            ring  = std::max(4.0f, glm::length(re - c));
            ringw = std::max(0.7f, 0.08f * ring);
            r     = std::max(0.9f, 0.06f * ring);
            glow  = glow > 0.0f ? ringw + 0.7f : 0.0f;
        }
    }
    // The aim marker (draw_aim_dot's red dot) is drawn here too, outside the window only.
    // reddotonly (author 18 Oct 23:09): no aim marker outside the window.
    const float sq   = (m_aim_dot.load() && !m_reddot_only.load() && !pin) ? aim_marker_px(d.Width) : 0.0f;
    const float ringx = ring > 0.0f ? 1.35f * ring + 1.25f * ringw + 2.0f * glow : 0.0f;   // to the ticks' outer ends
    const float ext  = std::min<float>(kBox / 2 - 1, std::ceil(std::max({ r, 2.0f * glow, sq + 1.0f, ringx }) + 2.0f));
    const float x0 = std::clamp(std::floor(c.x - ext), 0.0f, (float)d.Width), x1 = std::clamp(std::ceil(c.x + ext), 0.0f, (float)d.Width);
    const float y0 = std::clamp(std::floor(c.y - ext), 0.0f, (float)d.Height), y1 = std::clamp(std::ceil(c.y + ext), 0.0f, (float)d.Height);
    if (x1 <= x0 || y1 <= y0) return;
    if (pin) pin_out = 5;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    bool ok = init(dev);
    if (ok && (!g_stash || g_sfmt != d.Format)) {
        // The box is copied into a typeless texture read as UNORM (an sRGB back buffer is not decoded), the dot drawn
        // into a second one with UAV binding and copied back: works for the DLSS eye image and the back buffer alike.
        for (auto** p : { (IUnknown**)&g_ssrv, (IUnknown**)&g_stash, (IUnknown**)&g_wuav, (IUnknown**)&g_work }) if (*p) { (*p)->Release(); *p = nullptr; }
        DXGI_FORMAT tl{}, un{};
        family(d.Format, tl, un);
        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = kBox; nd.Height = kBox; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = tl; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = un; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_stash))) dev->CreateShaderResourceView(g_stash, &vd, &g_ssrv);
        nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = un; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_work))) dev->CreateUnorderedAccessView(g_work, &ud, &g_wuav);
        g_sfmt = d.Format;
        spdlog::info("[RedDot] eye image {}x{} format {} (bind 0x{:x}): dot box {}", d.Width, d.Height, (int)d.Format, d.BindFlags, g_ssrv && g_wuav ? "ready" : "failed");
    }
    dev->Release();
    if (!ok || !g_ssrv || !g_wuav || d.SampleDesc.Count != 1) { m_reddot_fails++; return; }

    const D3D11_BOX box{ (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    ctx->CopySubresourceRegion(g_stash, 0, 0, 0, 0, eye, 0, &box);
    const D3D11_BOX wbox{ 0, 0, 0, (UINT)(x1 - x0), (UINT)(y1 - y0), 1 };
    // Eye pixel q -> mask uv: the eye image is the render viewport upscaled.
    const float mw = std::max(1.0f, m_reddot_mask_wh[0]), mh = std::max(1.0f, m_reddot_mask_wh[1]);
    const float sx = m_reddot_vp[2] / (float)d.Width / mw, sy = m_reddot_vp[3] / (float)d.Height / mh;
    const float b  = std::clamp(m_reddot_bright.load(), 0.1f, 1.0f);
    Cb cb{ { c.x, c.y, r, glow, x0, y0, x1 - x0, y1 - y0, b, 0.08f * b, 0.05f * b, 1.0f, sx, sy, m_reddot_vp[0] / mw, m_reddot_vp[1] / mh, sq, pin ? -1.0f : 1.0f, ring, ringw } };
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);

    ID3D11ComputeShader*       prev_cs  = nullptr;
    ID3D11ShaderResourceView*  prev_srv[2]{};
    ID3D11UnorderedAccessView* prev_uav = nullptr;
    ID3D11Buffer*              prev_cb  = nullptr;
    ID3D11SamplerState*        prev_smp = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 2, prev_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ctx->CSGetSamplers(0, 1, &prev_smp);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2]{ g_ssrv, g_msrv };
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_wuav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->CSSetSamplers(0, 1, &g_smp);
    ctx->Dispatch((UINT(x1 - x0) + 7) / 8, (UINT(y1 - y0) + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv[2]{};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 2, null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, prev_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    ctx->CSSetSamplers(0, 1, &prev_smp);
    if (prev_cs) prev_cs->Release();
    for (auto* s : prev_srv) if (s) s->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    if (prev_smp) prev_smp->Release();
    ctx->CopySubresourceRegion(eye, 0, (UINT)x0, (UINT)y0, 0, g_work, 0, &wbox);
    m_reddot_dots++;
    if (pin) pin_out = (fr & 1) ? 1 : 0;
    // The aim marker went in with the dot: draw_aim_dot leaves this eye frame alone (not with bowpin's pin dot). bowpin aim: the marker is
    // the same point drawn again with a dark rim (author 2 Nov 15:47: two different dots), so it is left out too.
    if (!pin || bowaim) m_reddot_drawn_frame = fr;
    if (m_reddot_log.load() > 0) {
        m_reddot_log--;
        spdlog::info("[RedDot] dot, frame {} ({} eye): at ({:.1f}, {:.1f}) of {}x{}, radius {:.1f} px, ring {:.1f} px (line {:.1f}), {:.1f} m away ({})", fr,
                     (fr & 1) ? "right" : "left", c.x, c.y, d.Width, d.Height, r, ring, 2.0f * ringw, dist,
                     m_reddot_dist.load() > 0.5f ? "fixed distance" : (hit > 2.0f ? "barrel ray hit" : "convergence"));
    }
}

// bowpin: one eye frame's outcome (reddot_draw), and every 5 s while the manual bow is in hand a line of them.
void FarCry5Stereo::bow_pin_tally(int slot, int frame)
{
    if (slot < 0 || slot > 5) return;
    auto& t = m_bow_pin_tally;
    t[slot]++;
    const auto now = std::chrono::steady_clock::now();
    if (m_bow_pin_said.time_since_epoch().count() != 0 && now - m_bow_pin_said < std::chrono::seconds(5)) return;
    if (m_bow_pin_said.time_since_epoch().count() != 0 && now - m_bow_pin_said < std::chrono::seconds(30))
        spdlog::info("[RedDot] bowpin last 5 s: the sight dot drawn in the left eye {} and the right eye {} times; not drawn: no sight draw seen {}, hand placement old {}, "
                     "off the eye's picture {}, other {} ({})",
                     t[0], t[1], t[2], t[3], t[4], t[5],
                     fc5::bow::pin_mode() == 2 ? fmt::format("where the arrow lands, {:.1f} m", m_mo.converge)
                                               : fmt::format("on reticle draw {}, last seen frame {}, now {}", m_bow_pin_count, m_bow_pin_frame, frame));
    m_bow_pin_said = now;
    for (auto& v : t) v = 0;
}

// Red dot commands (own chain). True when cmd was one.
bool FarCry5Stereo::reddot_command(const std::string& cmd, const std::string& arg, bool from_settings, bool& persist_changed)
{
    if (cmd.rfind("reddot", 0) != 0) return false;
    const bool  on = arg == "on" || arg == "1" || arg == "true";
    const float fv = arg.empty() ? 0.0f : std::strtof(arg.c_str(), nullptr);
    auto persist = [&](const std::string& key, const std::string& line) {
        m_persist[key] = line;
        if (!from_settings) persist_changed = true;
    };
    if (cmd == "reddot") { m_reddot = on; if (from_settings) m_reddot_old_lines = true; }
    else if (cmd == "reddotmode") {
        const int mode = arg == "realistic" ? 0 : arg == "laser" ? 1 : arg == "arcade" ? 2 : -1;
        if (mode < 0) { spdlog::info("[RedDot] reddotmode '{}': realistic, laser or arcade", arg); return true; }
        reddot_set_mode(mode);
        m_reddot_old_lines = false;
        if (!from_settings) persist_changed = true;
    }
    else if (cmd == "reddotsize") { m_reddot_moa = std::clamp(fv, 1.0f, 30.0f); persist(cmd, fmt::format("reddotsize {:.1f}", m_reddot_moa.load())); }
    else if (cmd == "reddotbright") { m_reddot_bright = std::clamp(fv, 0.1f, 1.0f); persist(cmd, fmt::format("reddotbright {:.2f}", m_reddot_bright.load())); }
    else if (cmd == "reddotonly") { m_reddot_only = on; if (from_settings) m_reddot_old_lines = true; }
    else if (cmd == "reddotholo") { m_reddot_holo = on; persist(cmd, on ? "reddotholo on" : "reddotholo off"); }
    else if (cmd == "reddotring") { m_reddot_ring_moa = std::clamp(fv, 20.0f, 120.0f); persist(cmd, fmt::format("reddotring {:.0f}", m_reddot_ring_moa.load())); }
    else if (cmd == "reddotglow") { m_reddot_glow = on; persist(cmd, on ? "reddotglow on" : "reddotglow off"); }
    else if (cmd == "reddotglass") {
        m_reddot_glass = std::clamp((int)fv, 0, 2);
        persist(cmd, fmt::format("reddotglass {}", m_reddot_glass.load()));
        spdlog::info("[RedDot] reddotglass {}: closed red dot sights {}", m_reddot_glass.load(),
                     m_reddot_glass.load() == 0 ? "keep the game's dark glass" : m_reddot_glass.load() == 1 ? "clear (no dark disc, glass reflection kept)" : "clear (no dark disc, no glass reflection)");
    }
    else if (cmd == "reddotdist") { m_reddot_dist = std::max(0.0f, fv); }   // test: a fixed distance along the barrel (0 = where the shots land)
    else if (cmd == "reddotlog") { m_reddot_log = (int)fv; }
    else if (cmd == "reddotstatus") {
        spdlog::info("[RedDot] status: mode {} (gun draws a sight's glass {}, no laser on it {}); reddot {} size {:.1f} MOA bright {:.2f} glow {} holo {} ring {:.0f} MOA; weapon {} {}; reticle draws into the mask {} (draws {}), "
                     "dots drawn {}, failures {}; mask {:.0f}x{:.0f}, last mask frame {} (now {})",
                     reddot_mode_name(reddot_mode()), m_gun_has_sight.load(), reddot_no_laser(), m_reddot.load(), m_reddot_moa.load(), m_reddot_bright.load(), m_reddot_glow.load(), m_reddot_holo.load(), m_reddot_ring_moa.load(), m_weapon_sig.load(),
                     reddot_excluded() ? "(magnifying scope: excluded)" : "", m_reddot_mask_draws, m_reddot_reticle_draws, m_reddot_dots, m_reddot_fails,
                     m_reddot_mask_wh[0], m_reddot_mask_wh[1], m_reddot_mask_frame, m_frame.load());
    } else {
        spdlog::info("[RedDot] unknown command '{}'", cmd);
    }
    return true;
}
