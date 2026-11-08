// gesturezones on|off (debug, not saved; F11 Gestures "Debug: show the gesture zones"; author 24 Oct): each gesture zone (FarCry5Gesture.cpp) drawn into both eyes'
// pictures as the see-through ball it is (a grip press counts when the hand's point is within the zone size of its centre),
// with a brighter rim at its outline, and a small white ball on each controller's tested point.
//  - Shaded as balls (author 24 Oct 12:16): the near and far surfaces (the far one
//    fainter, seen through the ball) lit from above, with latitude / longitude lines every 30 deg on the zones.
//  - Colours: blue the shoulder (switch weapon), orange the sling, green the throwable. Brighter while that hand is inside,
//    white rim while the gesture holds (the sling: the weapon in hand; the throwable: in the hand). Half faded when it cannot
//    act now (no weapon in hand for the sling, a vehicle, a cinematic), faint grey when switched off.
//  - Each eye pixel's ray (the eye's XR pose and field of view, as hudmarkers) is met with each ball in tracking space, so
//    the balls sit where the gesture code looks for the hands, for this frame's head pose. Always on top of the scene (no
//    depth test): the zones are within reach of the body.
//  - bowarrow (manualbow, FarCry5Bow.cpp; author 1 Nov 21:24): the arrow as three capsules (shaft,
//    point, fletching), drawn whether the zones are shown or not (the nock zone's amber ball was removed on 6 Nov).
//  - reloadmanual (FarCry5Reload.cpp; author 2 Nov): a magazine (three dark capsules: its
//    front and back edges, curving forward to the bottom, and its base plate) in the hip pouch, in the left hand while held (unless the
//    game's own magazine is drawn there, FarCry5ReloadMag.cpp) and sliding up the well's axis while it goes in; the well point as a small
//    yellow ball while a magazine is in the hand (brighter once close enough), and the charging handle as a blue ball with a bar across it
//    that moves back with the pull while it must be racked. Drawn whether the zones are shown or not, from the controllers' poses of the
//    frame drawn. Once the game's own magazine is drawn at the hip, and its own handle moves with the pull, the drawn ones are left out.
//  - reloadhintstyle glow (FarCry5ReloadHint.cpp; author 5 Nov 08:28): the part to work next lit on the gun's own
//    model from the hint mask: a soft coloured glow over it where it is seen (pulsing gently), a bright outline, a halo round it, and faint
//    moving lines where it is hidden (behind the hand, or the well's ghost magazine inside the gun). That part's box and ball are left out.
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"
#include "FarCry5Bow.h"
#include "FarCry5Reload.h"
#include "FarCry5ReloadHint.h"
#include "FarCry5Drive.h"

#include <mods/VR.hpp>
#include "FarCry5Record.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace
{
    constexpr int kBalls = 10;  // the zones and the two hand points (and bowarrow's nock zone, reloadmanual's well and handle)
    constexpr int kZoneMax = 5; // of them, the zones
    constexpr int kSegs  = 12;  // bowarrow: the arrow's capsules; reloadmanual: the magazines and the handle
    constexpr int kBoxes = 4;   // reloadhighlight (manualreload40): the gun's parts to work next

    const char* kZones = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);
#ifdef HL_GLOW
Texture2D<float4>   g_hint : register(t1);   // reloadhintstyle glow: R, G the magazine seen, anywhere; B, A the handle seen, anywhere
SamplerState        g_lin  : register(s0);
#endif
cbuffer P : register(b0)
{
    uint   w; uint h; uint n; uint pad0;
    float4 tn;            // tangents of the eye's left, right, up and down edges
    float4 eye;           // the eye's position (tracking space)
    float4 ax; float4 ay; float4 az;   // the eye's axes (tracking space): right, up, back
    float4 ball[10];      // centre (tracking space), radius (m)
    float4 col[10];       // colour, fill opacity
    float4 rim[10];       // rim opacity, rim whiteness, grid lines (1 = on)
    float4 sa[12];        // bowarrow, reloadmanual: capsule ends (tracking space), radius (m) in sa.w
    float4 sb[12];
    float4 scol[12];      // colour, opacity
    uint   ns; uint nb; float pulse; uint pad3;
    float4 bc[4];         // reloadhighlight: each box's centre (tracking space) ...
    float4 bx[4]; float4 by[4]; float4 bz[4];   // ... its half-axis vectors ...
    float4 bcol[4];       // ... colour, fill opacity
    float4 hm;            // reloadhintstyle glow: eye pixel -> hint mask uv (q * hm.xy + hm.zw) ...
    float4 hc[2];         // ... each group's colour and strength (0: not in the mask this frame) ...
    float4 hp;            // ... the halo's reach (eye pixels), the time (s), the hidden lines' spacing (eye pixels)
};
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float4       c  = g_src.Load(int3(p, 0));
    const float2 uv = (float2(p) + 0.5) / float2(w, h);
    const float3 d  = normalize(ax.xyz * lerp(tn.x, tn.y, uv.x) + ay.xyz * lerp(tn.z, tn.w, uv.y) - az.xyz);
    for (uint i = 0; i < n; ++i) {
        const float3 oc = eye.xyz - ball[i].xyz;
        const float  r  = ball[i].w;
        const float  b  = dot(oc, d);
        const float  m2 = dot(oc, oc) - b * b;   // the ray's closest approach to the centre, squared
        if (m2 >= r * r) continue;
        const float s = sqrt(r * r - m2);
        if (-b + s <= 0.0) continue;               // behind the eye
        const float m    = sqrt(m2) / r;           // 0 through the centre, 1 at the outline
        const float edge = 1.0 - smoothstep(0.985, 1.0, m);   // soft outline (no stair steps)
        // The far surface first (half as opaque), then the near one (none when the eye is inside the ball).
        for (int k = 1; k >= 0; --k) {
            const float t = k == 1 ? -b + s : -b - s;
            if (t <= 0.0) continue;
            const float3 nrm  = (oc + d * t) / r;  // the surface's normal (tracking space, y up)
            const float  lit  = 0.45 + 0.55 * saturate(dot(nrm, float3(-0.29, 0.91, 0.29)));
            const float  lat  = asin(clamp(nrm.y, -1.0, 1.0)), lon = atan2(nrm.z, nrm.x);
            const float  step = 0.5235988;          // 30 deg
            const float  gl   = abs(frac(lat / step + 0.5) - 0.5) * step * r;              // to the nearest latitude line (m)
            const float  gn   = abs(frac(lon / step + 0.5) - 0.5) * step * r * cos(lat);   // to the nearest longitude line (m)
            const float  grid = rim[i].z * (1.0 - smoothstep(0.0015, 0.003, min(gl, gn)));
            const float  a    = max(col[i].a * (k == 1 ? 0.5 : 1.0), grid * (k == 1 ? 0.25 : 0.55)) * edge;
            c.rgb = lerp(c.rgb, lerp(col[i].rgb * lit, float3(1.0, 1.0, 1.0), 0.35 * grid), a);
        }
        const float band = smoothstep(0.92, 0.97, m) * edge;   // the rim
        c.rgb = lerp(c.rgb, lerp(col[i].rgb, float3(1.0, 1.0, 1.0), rim[i].y), rim[i].x * band);
    }
    // bowarrow: capsules (the ray's closest approach to each segment), shaded round, soft edged.
    for (uint j = 0; j < ns; ++j) {
        const float3 a  = sa[j].xyz, u = sb[j].xyz - sa[j].xyz;
        const float  r  = sa[j].w;
        const float3 w0 = eye.xyz - a;
        const float  uu = dot(u, u), ud = dot(u, d), uw = dot(u, w0), dw = dot(d, w0);
        const float  den = uu - ud * ud;
        float s = den > 1e-10 ? saturate((uw - ud * dw) / den) : 0.0;
        const float t = max(s * ud - dw, 0.0);
        s = uu > 1e-10 ? saturate(dot(u, eye.xyz + d * t - a) / uu) : 0.0;
        const float3 q  = a + u * s;
        const float  tq = dot(q - eye.xyz, d);
        if (tq <= 0.0) continue;                    // behind the eye
        const float dist = length(q - (eye.xyz + d * tq));
        if (dist >= r) continue;
        const float m2   = dist / r;
        const float lit  = 0.45 + 0.55 * sqrt(saturate(1.0 - m2 * m2));
        const float edge = 1.0 - smoothstep(0.75, 1.0, m2);
        c.rgb = lerp(c.rgb, scol[j].rgb * lit, scol[j].a * edge);
    }
#ifdef HL_GLOW
    // reloadhintstyle glow: the part itself, from the hint mask. Two rings of 8 taps find the nearest part pixels for the halo and the seen part's
    // outline; lit with a screen-like add that fades over pixels already bright.
    if (hc[0].a > 0.0 || hc[1].a > 0.0) {
        const float2 q  = float2(p) + 0.5;
        const float4 m0 = g_hint.SampleLevel(g_lin, q * hm.xy + hm.zw, 0);
        float2 near1 = float2(0.0, 0.0), near2 = float2(0.0, 0.0), seen_min = float2(1.0, 1.0);
        [unroll] for (int k = 0; k < 8; ++k) {
            const float  ang = float(k) * 0.7853982 + 0.3926991;
            const float2 dir = float2(cos(ang), sin(ang));
            const float4 s1  = g_hint.SampleLevel(g_lin, (q + dir * (hp.x * 0.4)) * hm.xy + hm.zw, 0);
            const float4 s2  = g_hint.SampleLevel(g_lin, (q + dir * hp.x) * hm.xy + hm.zw, 0);
            near1    = max(near1, max(s1.ga, s1.rb));
            near2    = max(near2, max(s2.ga, s2.rb));
            seen_min = min(seen_min, s1.rb);
        }
        const float pulse = 0.5 + 0.5 * sin(hp.y * 7.539822);                       // 1.2 Hz
        const float band  = abs(frac(q.y / max(hp.z, 2.0) - hp.y * 1.2) - 0.5) * 2.0;   // 0 on a line .. 1 between, scrolling up
        const float lines = 1.0 - smoothstep(0.0, 0.45, band);
        [unroll] for (int g = 0; g < 2; ++g) {
            const float st = hc[g].a;
            if (st <= 0.0) continue;
            const float seen   = g == 0 ? m0.r : m0.b;
            const float covered = max(g == 0 ? m0.g : m0.a, seen);
            const float hidden = saturate(covered - seen);
            const float rim    = saturate(seen - (g == 0 ? seen_min.x : seen_min.y));   // the seen part's outline
            const float halo   = saturate(max((g == 0 ? near1.x : near1.y) * 0.8, (g == 0 ? near2.x : near2.y) * 0.4) - covered);
            float a = seen * (0.14 + 0.10 * pulse) + rim * 0.75 + hidden * (0.10 + 0.30 * lines) + halo * (0.45 + 0.25 * pulse);
            a       = saturate(a * st);
            const float3 col = lerp(hc[g].rgb, float3(1.0, 1.0, 1.0), 0.35 * rim);
            c.rgb += col * a * (1.0 - 0.6 * saturate(c.rgb));
        }
    }
#endif
#ifdef HL_BOXES
    // reloadhighlight: glowing boxes (the ray met with each box in its own unit cube; the near and far faces, brighter at the edges).
    for (uint q = 0; q < nb; ++q) {
        const float3 A0 = bx[q].xyz, A1 = by[q].xyz, A2 = bz[q].xyz;
        const float3 o  = eye.xyz - bc[q].xyz;
        const float3 l2 = float3(dot(A0, A0), dot(A1, A1), dot(A2, A2));
        const float3 lo = float3(dot(o, A0), dot(o, A1), dot(o, A2)) / l2;
        float3       ld = float3(dot(d, A0), dot(d, A1), dot(d, A2)) / l2;
        ld              = ld + (step(0.0, ld) * 2.0 - 1.0) * 1e-7;
        const float3 t0 = (-1.0 - lo) / ld, t1 = (1.0 - lo) / ld;
        const float3 tn3 = min(t0, t1), tf3 = max(t0, t1);
        const float  tin = max(max(tn3.x, tn3.y), tn3.z);
        const float  tfa = min(min(tf3.x, tf3.y), tf3.z);
        if (tfa <= max(tin, 0.0)) continue;
        float a = 0.0;
        for (int k = 0; k < 2; ++k) {
            const float t = k == 0 ? tin : tfa;
            if (t <= 0.0) continue;                   // the eye inside the box: no near face
            const float3 s3   = abs(lo + ld * t);
            const float  mid  = max(min(s3.x, s3.y), min(max(s3.x, s3.y), s3.z));   // the second largest: near 1 at an edge
            const float  edge = smoothstep(0.82, 0.97, mid);
            a = max(a, (k == 0 ? 1.0 : 0.5) * lerp(bcol[q].a * (0.65 + 0.35 * pulse), 0.85, edge));
        }
        c.rgb = lerp(c.rgb, bcol[q].rgb, a);
    }
#endif
    g_dst[p] = c;
}
)";

    struct Cb
    {
        UINT  w, h, n, pad0;
        float tn[4];
        float eye[4];
        float ax[4], ay[4], az[4];
        float ball[kBalls][4];
        float col[kBalls][4];
        float rim[kBalls][4];
        float sa[kSegs][4];
        float sb[kSegs][4];
        float scol[kSegs][4];
        UINT  ns, nb;
        float pulse;
        UINT  pad3;
        float bc[kBoxes][4];
        float bx[kBoxes][4], by[kBoxes][4], bz[kBoxes][4];
        float bcol[kBoxes][4];
        float hm[4];          // reloadhintstyle glow (see the shader)
        float hc[fc5::reloadhint::kGroups][4];
        float hp[4];
    };

    ID3D11ComputeShader*       g_cs     = nullptr;
    ID3D11Buffer*              g_cb     = nullptr;
    bool                       g_failed = false;
    ID3D11Texture2D*           g_for    = nullptr;   // the eye texture the copies below were made for (not held)
    D3D11_TEXTURE2D_DESC       g_desc{};
    ID3D11Texture2D*           g_stash  = nullptr;
    ID3D11ShaderResourceView*  g_srv    = nullptr;
    ID3D11Texture2D*           g_mid    = nullptr;
    ID3D11UnorderedAccessView* g_uav    = nullptr;
    bool                       g_tex_failed = false;
    uint64_t                   g_drawn  = 0;
    bool                       g_was_on = false;
    bool                       g_hl_failed = false;   // reloadhighlight's boxes not in the shader
    bool                       g_glow_failed = false; // reloadhintstyle glow not in the shader (the boxes are drawn instead)
    ID3D11SamplerState*        g_smp    = nullptr;    // reloadhintstyle glow: the hint mask, linear

    DXGI_FORMAT typeless_of(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default: return f;
        }
    }
    DXGI_FORMAT view_of(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
        }
    }

    void release_textures()
    {
        if (g_srv) g_srv->Release();
        if (g_stash) g_stash->Release();
        if (g_uav) g_uav->Release();
        if (g_mid) g_mid->Release();
        g_srv = nullptr; g_stash = nullptr; g_uav = nullptr; g_mid = nullptr;
    }

    bool ready(ID3D11Device* dev, ID3D11Texture2D* eye)
    {
        if (g_failed) return false;
        if (!g_cs) {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            // reloadhighlight (manualreload40): the boxes compiled in; should that fail, the zones as before without them (logged), so a slip in
            // the new lines cannot take the balls and the bow's arrow away.
            // reloadhintstyle glow: the glow compiled in first; should that fail, the boxes as before (logged).
            const D3D_SHADER_MACRO glow_defs[]{ { "HL_BOXES", "1" }, { "HL_GLOW", "1" }, { nullptr, nullptr } };
            if (compile_fn && FAILED(compile_fn(kZones, strlen(kZones), "zones", glow_defs, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Zones] reloadhintstyle glow did not compile (the boxes are drawn instead): {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                err  = nullptr;
                code = nullptr;
                g_glow_failed = true;
            }
            const D3D_SHADER_MACRO hl_defs[]{ { "HL_BOXES", "1" }, { nullptr, nullptr } };
            if (compile_fn && !code && FAILED(compile_fn(kZones, strlen(kZones), "zones", hl_defs, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Zones] reloadhighlight's boxes did not compile (not drawn): {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                err  = nullptr;
                code = nullptr;
                g_hl_failed = true;
            }
            if (!compile_fn || (!code && FAILED(compile_fn(kZones, strlen(kZones), "zones", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)))) {
                spdlog::error("[Zones] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_cb);
            D3D11_SAMPLER_DESC smd{};
            smd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            smd.MaxLOD   = D3D11_FLOAT32_MAX;
            dev->CreateSamplerState(&smd, &g_smp);
            if (!g_smp) g_glow_failed = true;
            if (!g_cs || !g_cb) { g_failed = true; return false; }
        }
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        if (eye != g_for || d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format) {
            release_textures();
            g_for = eye; g_desc = d; g_tex_failed = false;
            D3D11_TEXTURE2D_DESC nd = d;
            nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            nd.Format    = typeless_of(d.Format);
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view_of(d.Format); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_stash))) dev->CreateShaderResourceView(g_stash, &sd, &g_srv);
            nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = view_of(d.Format); ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_mid))) dev->CreateUnorderedAccessView(g_mid, &ud, &g_uav);
            g_tex_failed = !g_srv || !g_uav;
            spdlog::info("[Zones] eye picture {}x{} format {} (samples {}): {}", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, g_tex_failed ? "no copies (not drawn)" : "ready");
        }
        return !g_tex_failed && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
}

// After the throw arc, before vehfade: the zones over this eye's picture.
void FarCry5Stereo::gesture_zones_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    const bool on = fc5::gesture::zone_show() && m_motion.load() && !m_menu_screen_on;
    if (on != g_was_on) spdlog::info("[Zones] gesture zones {}", on ? "shown" : "hidden");
    g_was_on = on;
    // bowarrow (manualbow): the arrow and the nock zone, also with the zones hidden.
    fc5::bow::ArrowView av{};
    const bool arrow = m_motion.load() && !m_menu_screen_on && !flat_screen_on() && fc5::bow::arrow_view(av);
    // reloadmanual: the magazines, the well and the handle.
    fc5::reload::View rv{};
    const bool reload = m_motion.load() && !m_menu_screen_on && !flat_screen_on() && !m_cine_on && fc5::reload::view(rv);
    // drive (FarCry5Drive.cpp): the wheel, handlebars or stick while driving with drive and driveshow on.
    fc5::drive::View dv{};
    const bool drive = m_motion.load() && !m_menu_screen_on && !flat_screen_on() && !m_cine_on && fc5::drive::view(dv);
    if ((!on && !arrow && !reload && !drive) || !ctx || !eye) return;
    const int   fr = m_frame.load();
    const auto& v  = m_xr_views[fr & 7];
    if (v.frame != fr) return;
    const int e = fr & 1;
    // The head for the zones: between the eyes, turned as the eyes are (they are parallel on a Quest).
    const glm::vec3 p0{ v.pos[0][0], v.pos[0][1], v.pos[0][2] }, p1{ v.pos[1][0], v.pos[1][1], v.pos[1][2] };
    const glm::quat q0{ v.rot[0][3], v.rot[0][0], v.rot[0][1], v.rot[0][2] }, q1{ v.rot[1][3], v.rot[1][0], v.rot[1][1], v.rot[1][2] };   // w x y z
    glm::mat4       head = glm::mat4_cast(glm::slerp(q0, q1, 0.5f));
    head[3]              = glm::vec4{ 0.5f * (p0 + p1), 1.0f };

    Cb cb{};
    for (int k = 0; k < 4; ++k) cb.tn[k] = std::tan(v.fov[e][k]);
    const glm::vec3 pe = e ? p1 : p0;
    const glm::mat3 re = glm::mat3_cast(e ? q1 : q0);
    for (int k = 0; k < 3; ++k) {
        cb.eye[k] = pe[k];
        cb.ax[k]  = re[0][k];
        cb.ay[k]  = re[1][k];
        cb.az[k]  = re[2][k];
    }
    fc5::gesture::ZoneView zv[kZoneMax]{};
    const int              nz = on ? fc5::gesture::zone_views(head, m_left_handed.load(), zv, kZoneMax) : 0;   // the rest kept for the hand points, the nock zone, the well and the handle
    // By gesture: the shoulder (switch weapon), the sling, the throwable, the sidearm on the hip.
    static const float kHue[4][3]{ { 0.25f, 0.6f, 1.0f }, { 1.0f, 0.55f, 0.1f }, { 0.25f, 1.0f, 0.4f }, { 0.75f, 0.35f, 1.0f } };
    UINT n = 0;
    for (int i = 0; i < nz; ++i) {
        const auto& z = zv[i];
        const float* hue = kHue[std::min(i, 3)];
        float fill = 0.16f, rim = 0.75f, white = 0.0f;
        float rgb[3]{ hue[0], hue[1], hue[2] };
        if (!z.on) {
            for (float& c : rgb) c = 0.6f;
            fill = 0.06f;
            rim  = 0.3f;
        } else if (!z.available) {
            fill = 0.1f;
            rim  = 0.4f;
        } else {
            if (z.inside) { fill = 0.32f; rim = 0.95f; }
            if (z.held) { fill = std::max(fill, 0.3f); rim = 1.0f; white = 0.8f; }
        }
        cb.ball[n][0] = z.centre.x; cb.ball[n][1] = z.centre.y; cb.ball[n][2] = z.centre.z; cb.ball[n][3] = z.radius;
        cb.col[n][0]  = rgb[0]; cb.col[n][1] = rgb[1]; cb.col[n][2] = rgb[2]; cb.col[n][3] = fill;
        cb.rim[n][0]  = rim; cb.rim[n][1] = white; cb.rim[n][2] = 1.0f;
        n++;
    }
    // The point of each controller the zones test: a 2 cm white ball.
    for (int h = 0; h < 2 && on; ++h) {
        glm::vec3 hp{};
        if (!fc5::gesture::hand_point(h, hp)) continue;
        cb.ball[n][0] = hp.x; cb.ball[n][1] = hp.y; cb.ball[n][2] = hp.z; cb.ball[n][3] = 0.02f;
        cb.col[n][0]  = 1.0f; cb.col[n][1] = 1.0f; cb.col[n][2] = 1.0f; cb.col[n][3] = 0.85f;
        cb.rim[n][0]  = 1.0f; cb.rim[n][1] = 0.0f;
        n++;
    }
    // bowarrow's nock zone ball removed (author 6 Nov 14:02).
    cb.n = n;
    UINT ns = 0;
    if (arrow && av.arrow && glm::length(av.tip - av.tail) > 0.05f) {
        // The arrow: a dark shaft (4 mm), a steel point (8 mm, the front 5 cm) and red fletching (9 mm, 3-11 cm from the nock end).
        const glm::vec3 f = glm::normalize(av.tip - av.tail);
        auto seg = [&](const glm::vec3& a, const glm::vec3& b, float r, float cr, float cg, float cbl, float op) {
            if (ns >= kSegs) return;
            cb.sa[ns][0] = a.x; cb.sa[ns][1] = a.y; cb.sa[ns][2] = a.z; cb.sa[ns][3] = r;
            cb.sb[ns][0] = b.x; cb.sb[ns][1] = b.y; cb.sb[ns][2] = b.z;
            cb.scol[ns][0] = cr; cb.scol[ns][1] = cg; cb.scol[ns][2] = cbl; cb.scol[ns][3] = op;
            ns++;
        };
        seg(av.tail, av.tip - f * 0.05f, 0.004f, 0.32f, 0.24f, 0.16f, 1.0f);
        seg(av.tip - f * 0.05f, av.tip, 0.008f, 0.75f, 0.75f, 0.78f, 1.0f);
        seg(av.tail + f * 0.03f, av.tail + f * 0.11f, 0.009f, 0.85f, 0.15f, 0.1f, 1.0f);
    }
    // reloadhintstyle glow: this eye frame's hint mask (FarCry5ReloadHint.cpp), if a part went into it.
    fc5::reloadhint::Composite hint{};
    bool                       glow = false;
    if (reload && fc5::reload::highlight_on() && fc5::reload::hint_glow() && !g_glow_failed && m_scope_src_frame != fr) {
        D3D11_TEXTURE2D_DESC ed{};
        eye->GetDesc(&ed);
        glow = fc5::reloadhint::composite(fr, float(ed.Width), float(ed.Height), hint);
        if (glow) {
            cb.hm[0] = hint.mscale[0]; cb.hm[1] = hint.mscale[1]; cb.hm[2] = hint.moff[0]; cb.hm[3] = hint.moff[1];
            for (int g = 0; g < fc5::reloadhint::kGroups; ++g) for (int k = 0; k < 4; ++k) cb.hc[g][k] = hint.col[g][k];
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            cb.hp[0] = std::max(4.0f, 0.0045f * float(ed.Width));   // the halo's reach: ~9 eye pixels on a 2000-wide eye
            cb.hp[1] = float(std::fmod(t, 1000.0));
            cb.hp[2] = std::max(3.0f, 0.0028f * float(ed.Width));   // the hidden lines' spacing
        }
    }
    const bool glow_mag    = glow && hint.col[fc5::reloadhint::kMag][3] > 0.0f;
    const bool glow_handle = glow && hint.col[fc5::reloadhint::kHandle][3] > 0.0f;
    if (reload) {
        // The controllers now (tracking space): the weapon hand's (the gun) and the other's (the held magazine).
        static auto vr = VR::get();
        const bool  lh = m_left_handed.load();
        auto pose = [&](int idx, glm::vec3& p, glm::mat3& r) {
            if (idx <= 0) return false;
            const glm::mat4 m = vr->get_transform(idx);
            if (!std::isfinite(m[3][0]) || (m[0][0] == 0.0f && m[1][1] == 0.0f && m[2][2] == 0.0f)) return false;
            p = glm::vec3{ m[3] };
            r = glm::mat3{ m };
            return true;
        };
        glm::vec3 gp{}, hp{};
        glm::mat3 gr{ 1.0f }, hr{ 1.0f };
        bool gun  = pose(lh ? vr->get_left_controller_index() : vr->get_right_controller_index(), gp, gr);
        bool hand = pose(lh ? vr->get_right_controller_index() : vr->get_left_controller_index(), hp, hr);
        if (m_test_ctrl.load() && rv.pose_ok) {
            // testctrl (tools\manualreload): the simulated controllers the reload reads, not the runtime's (the 2 Nov 17:18 arcade run drew the well's
            // ball 10-15 cm off the 1911 that way, while the magazine went in at the grip).
            gp = rv.gun_pos; gr = rv.gun_rot; hp = rv.hand_pos; hr = rv.hand_rot;
            gun = hand = true;
        }
        const float     mx  = lh ? -1.0f : 1.0f;
        auto mir = [&](glm::vec3 p) { p.x *= mx; return p; };
        const glm::vec3 well = gp + gr * mir(rv.g.well);
        glm::vec3       U    = gr * mir(rv.g.axis);
        U                    = glm::length(U) > 1e-4f ? glm::normalize(U) : glm::vec3{ 0.0f, 1.0f, 0.0f };
        glm::vec3       back = gr * mir(rv.g.back);
        back                 = glm::length(back) > 1e-4f ? glm::normalize(back) : glm::vec3{ 0.0f, 0.0f, 1.0f };
        auto seg = [&](const glm::vec3& a, const glm::vec3& b, float r, float cr, float cg, float cbl, float op) {
            if (ns >= kSegs) return;
            cb.sa[ns][0] = a.x; cb.sa[ns][1] = a.y; cb.sa[ns][2] = a.z; cb.sa[ns][3] = r;
            cb.sb[ns][0] = b.x; cb.sb[ns][1] = b.y; cb.sb[ns][2] = b.z;
            cb.scol[ns][0] = cr; cb.scol[ns][1] = cg; cb.scol[ns][2] = cbl; cb.scol[ns][3] = op;
            ns++;
        };
        // A 30-round magazine from its top T: up u (into the gun), front f: ~18 cm long, 6.5 cm front to back at the top, its bottom 3 cm forward.
        auto magazine = [&](const glm::vec3& T, glm::vec3 u, glm::vec3 f, float op) {
            u = glm::length(u) > 1e-4f ? glm::normalize(u) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            f = f - u * glm::dot(f, u);
            if (glm::length(f) < 1e-4f) return;
            f = glm::normalize(f);
            seg(T + f * 0.021f, T - u * 0.165f + f * 0.050f, 0.012f, 0.16f, 0.16f, 0.17f, op);   // the front edge
            seg(T - f * 0.021f, T - u * 0.160f + f * 0.008f, 0.012f, 0.20f, 0.20f, 0.21f, op);   // the back edge
            seg(T - u * 0.172f + f * 0.004f, T - u * 0.177f + f * 0.056f, 0.013f, 0.09f, 0.09f, 0.10f, op);   // the base plate
        };
        // A pump shotgun's shell (FarCry5Reload.h, tube_fed): a 12 gauge shell (7 cm, 1.9 cm across, red with a brass base) lying along f, its
        // top at T.
        auto shell = [&](const glm::vec3& T, glm::vec3 u, glm::vec3 f, float op) {
            u = glm::length(u) > 1e-4f ? glm::normalize(u) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            f = f - u * glm::dot(f, u);
            if (glm::length(f) < 1e-4f) return;
            f = glm::normalize(f);
            const glm::vec3 c = T - u * 0.0095f;
            seg(c - f * 0.022f, c + f * 0.035f, 0.0095f, 0.62f, 0.08f, 0.06f, op);   // the hull
            seg(c - f * 0.035f, c - f * 0.022f, 0.0100f, 0.78f, 0.62f, 0.25f, op);   // the brass head
        };
        // A revolver's round (manualreload10): a .44 cartridge (4 cm, 1.15 cm across, brass with a lead nose), its nose at T, lying along f.
        auto cartridge = [&](const glm::vec3& T, const glm::vec3& f0, float op) {
            const glm::vec3 f = glm::length(f0) > 1e-4f ? glm::normalize(f0) : glm::vec3{ 0.0f, 0.0f, -1.0f };
            seg(T - f * 0.040f, T - f * 0.010f, 0.0058f, 0.80f, 0.64f, 0.26f, op);   // the case
            seg(T - f * 0.010f, T - f * 0.002f, 0.0050f, 0.45f, 0.45f, 0.47f, op);   // the bullet
        };
        auto spare = [&](const glm::vec3& T, const glm::vec3& u, const glm::vec3& f, float op) {
            if (rv.revolver) cartridge(T, f, op);
            else if (rv.tube) shell(T, u, f, op);
            else magazine(T, u, f, op);
        };
        if (rv.pouch_mag && !fc5::reload::pouch_shown()) {   // (the game's own magazine is drawn there once known: FarCry5ReloadMag.cpp)
            const float op = rv.pouch_near ? 1.0f : 0.92f;
            spare(rv.pouch + glm::vec3{ 0.0f, 0.06f, 0.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f }, rv.pouch_fwd, op);
        }
        const bool model = fc5::reload::model_shown();
        if (rv.hand_mag && hand && !model) spare(hp + hr * rv.hand_mag_off, hr[1], -hr[2], 1.0f);
        if (rv.engaged && gun && !model) spare(well - U * (1.0f - rv.insert) * rv.travel, U, -back, 1.0f);
        if (rv.well_ball && gun && n < kBalls && !glow_mag) {   // (glow: the ghost magazine in the well instead)
            // reloadmuzzlezone (author 5 Nov 15:51: the RPG's circle by the grip): a muzzle-loaded round's way in starts at the muzzle, its
            // travel ahead of the seated tail (the well).
            fc5::reload::Revolver mrp{};
            const glm::vec3 wb = rv.revolver && fc5::reload::revolver_parts(rv.sig, mrp) && mrp.muzzle ? well - U * rv.travel : well;
            cb.ball[n][0] = wb.x; cb.ball[n][1] = wb.y; cb.ball[n][2] = wb.z; cb.ball[n][3] = 0.025f;
            cb.col[n][0]  = 1.0f; cb.col[n][1] = 0.85f; cb.col[n][2] = 0.15f; cb.col[n][3] = rv.well_near ? 0.35f : 0.12f;
            cb.rim[n][0]  = rv.well_near ? 1.0f : 0.5f; cb.rim[n][1] = rv.well_near ? 0.6f : 0.0f; cb.rim[n][2] = 0.0f;
            n++;
        }
        // reloadhighlight (manualreload40): the part to work next, its box measured on the gun's model.
        if (gun && fc5::reload::highlight_on() && !g_hl_failed && rv.hl_show) {   // (reloadhlwhen)
            auto box = [&](int which, float r, float g, float b) {
                fc5::reload::HlBox hb{};
                if (cb.nb >= kBoxes || !fc5::reload::highlight(rv.sig, which, hb)) return;
                const glm::vec3 c  = gp + gr * mir(hb.c);
                const glm::vec3 a0 = gr * mir(hb.a[0]), a1 = gr * mir(hb.a[1]), a2 = gr * mir(hb.a[2]);
                const UINT      i  = cb.nb++;
                for (int k = 0; k < 3; ++k) {
                    cb.bc[i][k] = c[k];
                    cb.bx[i][k] = a0[k];
                    cb.by[i][k] = a1[k];
                    cb.bz[i][k] = a2[k];
                }
                cb.bcol[i][0] = r; cb.bcol[i][1] = g; cb.bcol[i][2] = b; cb.bcol[i][3] = 0.14f;
            };
            // reloadhintstyle glow: a part lit on the model keeps no box.
            if (rv.hl_mag && !glow_mag) box(fc5::reload::kHlMag, 1.0f, 0.42f, 0.12f);
            if (rv.hl_well && !glow_mag) box(fc5::reload::kHlWell, 1.0f, 0.85f, 0.15f);
            if (rv.hl_handle && !glow_handle) box(fc5::reload::kHlHandle, 0.3f, 0.65f, 1.0f);
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            cb.pulse       = 0.5f + 0.5f * float(std::sin(t * 2.0 * 3.14159265 * 1.2));   // 1.2 Hz
        }
        if (rv.rack_ball && gun) {
            const glm::vec3 h0 = gp + gr * mir(rv.g.handle);
            const glm::vec3 h  = h0 + back * rv.rack * rv.rack_len;   // the handle follows the pull
            if (n < kBalls && !glow_handle && rv.hl_show) {   // (glow: the handle itself lit instead; reloadhlwhen)
                cb.ball[n][0] = h0.x; cb.ball[n][1] = h0.y; cb.ball[n][2] = h0.z; cb.ball[n][3] = 0.03f;
                cb.col[n][0]  = 0.3f; cb.col[n][1] = 0.6f; cb.col[n][2] = 1.0f; cb.col[n][3] = rv.racking ? 0.06f : 0.16f;
                cb.rim[n][0]  = rv.racking ? 0.4f : 0.8f; cb.rim[n][1] = 0.0f; cb.rim[n][2] = 0.0f;
                n++;
            }
            if (!fc5::reload::handle_shown()) {   // the game's own handle moves once its part is known (FarCry5ReloadMag.cpp)
                const glm::vec3 side = glm::normalize(gr[0]);
                seg(h - side * 0.028f, h + side * 0.028f, 0.007f, 0.25f, 0.25f, 0.27f, 1.0f);   // the handle's T bar
                seg(h, h - back * 0.03f, 0.005f, 0.22f, 0.22f, 0.24f, 1.0f);                     // its stem, into the gun
            }
        }
    }
    if (drive) {
        // A dark grey control, see-through while free, more solid while held; a hand near enough to take it lights it orange; each holding
        // hand a small white ball where it holds.
        auto seg = [&](const glm::vec3& a, const glm::vec3& b, float r, float cr, float cg, float cbl, float op) {
            if (ns >= kSegs) return;
            cb.sa[ns][0] = a.x; cb.sa[ns][1] = a.y; cb.sa[ns][2] = a.z; cb.sa[ns][3] = r;
            cb.sb[ns][0] = b.x; cb.sb[ns][1] = b.y; cb.sb[ns][2] = b.z;
            cb.scol[ns][0] = cr; cb.scol[ns][1] = cg; cb.scol[ns][2] = cbl; cb.scol[ns][3] = op;
            ns++;
        };
        const bool  held = dv.held[0] || dv.held[1], reach = dv.in_reach[0] || dv.in_reach[1];
        const float op   = held ? 0.75f : reach ? 0.6f : 0.35f;
        const float cr = reach && !held ? 1.0f : 0.22f, cg = reach && !held ? 0.55f : 0.22f, cbl = reach && !held ? 0.1f : 0.24f;
        if (dv.style == fc5::drive::kWheel) {
            constexpr int kRim = 10;   // the rim as 10 capsules, and one spoke to the wheel's top (it turns with the wheel)
            auto rim = [&](float a) { return dv.centre + dv.radius * (std::cos(a) * dv.u + std::sin(a) * dv.v); };
            for (int i = 0; i < kRim; ++i) {
                const float a0 = dv.angle + 6.2831853f * float(i) / kRim, a1 = dv.angle + 6.2831853f * float(i + 1) / kRim;
                seg(rim(a0), rim(a1), 0.012f, cr, cg, cbl, op);
            }
            seg(dv.centre, rim(dv.angle + 1.5707963f), 0.008f, 0.85f, 0.45f, 0.1f, op);
        } else if (dv.style == fc5::drive::kBars) {
            const glm::vec3 e = dv.radius * (std::cos(dv.angle) * dv.u - std::sin(dv.angle) * dv.n);
            seg(dv.centre - e, dv.centre + e, 0.011f, cr, cg, cbl, op);
            seg(dv.centre + e * 0.62f, dv.centre + e, 0.019f, 0.12f, 0.12f, 0.13f, op);   // the grips
            seg(dv.centre - e * 0.62f, dv.centre - e, 0.019f, 0.12f, 0.12f, 0.13f, op);
        } else if (dv.style == fc5::drive::kStick) {
            seg(dv.base, dv.stick_top, 0.014f, cr, cg, cbl, op);
            seg(dv.stick_top - dv.v * 0.01f, dv.stick_top + dv.v * 0.03f, 0.022f, 0.12f, 0.12f, 0.13f, op);   // the handle
        }
        for (int h = 0; h < 2; ++h) {
            if (!dv.held[h] || n >= kBalls) continue;
            cb.ball[n][0] = dv.hold[h].x; cb.ball[n][1] = dv.hold[h].y; cb.ball[n][2] = dv.hold[h].z; cb.ball[n][3] = 0.025f;
            cb.col[n][0]  = 1.0f; cb.col[n][1] = 1.0f; cb.col[n][2] = 1.0f; cb.col[n][3] = 0.6f;
            cb.rim[n][0]  = 1.0f; cb.rim[n][1] = 0.0f; cb.rim[n][2] = 0.0f;
            n++;
        }
    }
    cb.n  = n;
    cb.ns = ns;
    if (n == 0 && ns == 0 && cb.nb == 0 && !glow) return;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = ready(dev, eye);
    dev->Release();
    if (!ok) return;
    cb.w = g_desc.Width;
    cb.h = g_desc.Height;
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                    static_cast<const void*>(&cb), 0u, 0u);   // past the mod's hook, which swaps camera matrices it recognises
    ctx->CopyResource(g_stash, eye);
    ID3D11ComputeShader*       prev_cs  = nullptr;
    ID3D11ShaderResourceView*  prev_srv = nullptr;
    ID3D11UnorderedAccessView* prev_uav = nullptr;
    ID3D11Buffer*              prev_cb  = nullptr;
    ID3D11ShaderResourceView*  prev_srv1 = nullptr;   // reloadhintstyle glow: the hint mask's slot and its sampler
    ID3D11SamplerState*        prev_smp  = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &prev_srv);
    ctx->CSGetShaderResources(1, 1, &prev_srv1);
    ctx->CSGetSamplers(0, 1, &prev_smp);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_srv);
    ID3D11ShaderResourceView* hint_srv = glow ? hint.srv : nullptr;
    ctx->CSSetShaderResources(1, 1, &hint_srv);
    ctx->CSSetSamplers(0, 1, &g_smp);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((g_desc.Width + 7) / 8, (g_desc.Height + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv[2]{};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 2, null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &prev_srv);
    ctx->CSSetShaderResources(1, 1, &prev_srv1);
    ctx->CSSetSamplers(0, 1, &prev_smp);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    if (prev_cs) prev_cs->Release();
    if (prev_srv) prev_srv->Release();
    if (prev_srv1) prev_srv1->Release();
    if (prev_smp) prev_smp->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    ctx->CopyResource(eye, g_mid);
    if (on && (++g_drawn % 900) == 1) {
        // Each zone from the head, in the head's level frame (right, up, forward; cm), and its state.
        const glm::vec3 hp{ head[3] };
        const glm::vec3 r = glm::normalize(glm::vec3{ head[0].x, 0.0f, head[0].z }), f = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, r);
        std::string     s;
        for (int i = 0; i < nz; ++i) {
            const glm::vec3 o = zv[i].centre - hp;
            s += fmt::format("{}{} {:+.0f} {:+.0f} {:+.0f} cm r {:.0f} ({}{}{})", i ? "; " : "", fc5::gesture::name(i), glm::dot(o, r) * 100.0f, o.y * 100.0f, glm::dot(o, f) * 100.0f,
                             zv[i].radius * 100.0f, zv[i].on ? (zv[i].available ? "can act" : "not now") : "off", zv[i].inside ? ", hand inside" : "", zv[i].held ? ", held" : "");
        }
        spdlog::info("[Zones] {} eye pictures drawn ({} balls, eye picture {}x{}); from the head (right, up, forward): {}", g_drawn, n, g_desc.Width, g_desc.Height, s);
    }
}
