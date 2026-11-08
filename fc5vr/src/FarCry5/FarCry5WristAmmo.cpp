// wristammo (FarCry5WristAmmo.h; author 5 Nov 11:51): the rounds in the gun, what it holds and what is left, under the weapon hand's wrist.
//  - The numbers: FarCry5Reload.cpp's count by hand (reload::View rounds, rounds_cap, rounds_aside, rounds_keep, fire_held) while the manual
//    reload has the weapon in hand, else the game's clip, clip size and reserve (lua::player_ammo).
//  - Drawn as bonemap's labels are (FarCry5BoneMap.cpp): the wrist point through this eye's camera (m_hist[0]), then ClearView rectangles
//    (its 8 x 14 font on a dark box), sized from the point's distance so the text keeps wristammosize metres high. Each eye draws the same
//    point with its own camera, so the numbers sit at the wrist's depth. No depth test: they are always on top, as the gesture zones.
//  - wristsprint (author 7 Nov 10:59, build 1605A19C):
//    the wrist point is the controller's pose as gun_anchor_apply left it, which already holds draw_shift (the eyes' base less the game
//    camera: steady view, walkbob, viewpivot); adding draw_shift again put the numbers off the hand by the sprint's bob. Only this update's
//    pose is used (m_mo.frame), else the last point held in the eye's own frame; the size is taken across the view, not along world axes.
#include "FarCry5Stereo.h"
#include "FarCry5WristAmmo.h"
#include "FarCry5BoneMap.h"
#include "FarCry5Lua.h"
#include "FarCry5Reload.h"
#include "FarCry5GameWeapon.h"
#include "FarCry5Record.h"

#include <d3d11_1.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <istream>
#include <mutex>
#include <string>
#include <vector>

namespace fc5::wristammo
{
    namespace
    {
        std::mutex        g_mtx;
        Opts              g_o{};
        std::atomic<bool> g_on{ true };
    }

    Opts opts()
    {
        std::lock_guard _{ g_mtx };
        return g_o;
    }
    bool on() { return g_on.load(); }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        if (cmd.rfind("wristammo", 0) != 0) return false;
        persist = true;   // saved
        if (cmd == "wristammo") {
            const bool v = arg.empty() || arg == "on" || arg == "1" || arg == "true";
            {
                std::lock_guard _{ g_mtx };
                g_o.on = v;
            }
            g_on = v;
            spdlog::info("[WristAmmo] wristammo {}", v ? "on: the rounds in the gun / what it holds / what is left, under the weapon hand's wrist" : "off");
            return true;
        }
        if (cmd == "wristammopos") {
            float y = -0.05f, z = 0.10f;
            std::string ys, zs;
            rest >> ys >> zs;
            if (!ys.empty()) y = std::strtof(ys.c_str(), nullptr);
            if (!zs.empty()) z = std::strtof(zs.c_str(), nullptr);
            std::lock_guard _{ g_mtx };
            g_o.pos[0] = std::clamp(arg.empty() ? 0.0f : fv, -0.3f, 0.3f);
            g_o.pos[1] = std::clamp(y, -0.3f, 0.3f);
            g_o.pos[2] = std::clamp(z, -0.3f, 0.4f);
            spdlog::info("[WristAmmo] wristammopos {:.3f} {:.3f} {:.3f} (m: right, up, back from the weapon controller)", g_o.pos[0], g_o.pos[1], g_o.pos[2]);
            return true;
        }
        if (cmd == "wristammostyle") {
            const bool sm = arg != "pixel";
            {
                std::lock_guard _{ g_mtx };
                g_o.smooth = sm;
            }
            spdlog::info("[WristAmmo] wristammostyle {}", sm ? "smooth: digits drawn from strokes, Far Cry 5 colours" : "pixel: the 8 x 14 font");
            return true;
        }
        if (cmd == "wristammosize") {
            std::lock_guard _{ g_mtx };
            g_o.size = std::clamp(arg.empty() ? 0.018f : fv, 0.005f, 0.06f);
            spdlog::info("[WristAmmo] wristammosize {:.3f} m", g_o.size);
            return true;
        }
        persist = false;
        return false;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        const Opts o = opts();
        bool       v = o.on;
        if (ImGui::Checkbox("Ammo under the wrist##wristammo", &v)) cmds += v ? "wristammo on\n" : "wristammo off\n";
        hint("Rounds in the gun / what it holds / rounds left, under the weapon hand's wrist. With the manual reload, the rounds you loaded by hand; "
             "amber while the gun cannot fire yet (rack the handle, work the bolt, close the cylinder), red when empty. Command wristammo on/off.");
        if (!v) return;
        float p[3]{ o.pos[0] * 100.0f, o.pos[1] * 100.0f, o.pos[2] * 100.0f };
        bool  moved = false;
        moved |= ImGui::SliderFloat("Right (cm)##wristammox", &p[0], -30.0f, 30.0f, "%.1f");
        moved |= ImGui::SliderFloat("Up (cm)##wristammoy", &p[1], -30.0f, 30.0f, "%.1f");
        moved |= ImGui::SliderFloat("Back towards the elbow (cm)##wristammoz", &p[2], -30.0f, 40.0f, "%.1f");
        if (moved) cmds += fmt::format("wristammopos {:.3f} {:.3f} {:.3f}\n", p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f);
        hint("From the weapon controller, in its own axes (mirrored when left-handed). Default 0, -5, 10 cm: under the wrist. Command wristammopos right up back (m).");
        float sz = o.size * 100.0f;
        if (ImGui::SliderFloat("Text height (cm)##wristammosize", &sz, 0.5f, 6.0f, "%.1f")) cmds += fmt::format("wristammosize {:.3f}\n", sz * 0.01f);
        hint("Default 1.8 cm (the first number; the others are smaller). Command wristammosize m.");
        bool sm = o.smooth;
        if (ImGui::Checkbox("Smooth digits in Far Cry 5 colours##wristammostyle", &sm)) cmds += sm ? "wristammostyle smooth\n" : "wristammostyle pixel\n";
        hint("On: smooth digits on a dark panel with an orange bar. Off: the square pixel font of the first version. Command wristammostyle smooth/pixel.");
    }
}

// wristammostyle smooth (author 5 Nov 13:06): the digits are strokes
// (rounded capsules: tall, condensed, chamfered corners), each eye pixel's distance to them gives a smooth edge one pixel wide at any size, with a
// soft dark rim. Under them a rounded dark panel, see-through, with an orange bar at its left edge. Only the panel's box of the eye picture is
// copied out, drawn by a compute shader and copied back, so the cost is a few hundred thousand pixels per eye.
// Colours (picked to match the game's HUD, not taken from its files): warm white numbers, the game's orange for "not ready" and the bar, red at 0.
namespace
{
    template <typename R, typename... A>
    R wa_call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
    {
        if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
        return h.call<R>(ctx, a...);
    }

    // The glyphs: polylines (one per stroke) in a 0.56 x 1 cell, y up, as segments: 0-9, '/', '-' (the table of research\2026-11-05_wristammo2\glyphs.py).
    const char* kWaText = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);
cbuffer P : register(b0)
{
    uint   w; uint h; uint n; uint pad0;
    float4 org;      // the text's origin in the box (px: left, baseline), pixels per text unit (the big digits' height), stroke half-width
    float4 panel;    // the panel (px in the box): left, top, right, bottom
    float4 pst;      // its corner radius (px), opacity; the rim's width (px) and opacity
    float4 bar;      // the orange bar (px): left, top, right, bottom
    float4 cbg;      // the panel's colour
    float4 cbar;     // the bar's colour
    float4 ch[16];   // each character: x (text units), scale, glyph (0-9, 10 '/', 11 '-'), colour index
    float4 col[4];
};
static const float4 kSeg[74] = {
    float4(0.200, 0.080, 0.360, 0.080),
    float4(0.360, 0.080, 0.480, 0.200),
    float4(0.480, 0.200, 0.480, 0.800),
    float4(0.480, 0.800, 0.360, 0.920),
    float4(0.360, 0.920, 0.200, 0.920),
    float4(0.200, 0.920, 0.080, 0.800),
    float4(0.080, 0.800, 0.080, 0.200),
    float4(0.080, 0.200, 0.200, 0.080),
    float4(0.140, 0.740, 0.320, 0.920),
    float4(0.320, 0.920, 0.320, 0.080),
    float4(0.080, 0.800, 0.200, 0.920),
    float4(0.200, 0.920, 0.360, 0.920),
    float4(0.360, 0.920, 0.480, 0.800),
    float4(0.480, 0.800, 0.480, 0.600),
    float4(0.480, 0.600, 0.080, 0.140),
    float4(0.080, 0.140, 0.080, 0.080),
    float4(0.080, 0.080, 0.480, 0.080),
    float4(0.080, 0.920, 0.360, 0.920),
    float4(0.360, 0.920, 0.480, 0.800),
    float4(0.480, 0.800, 0.480, 0.570),
    float4(0.480, 0.570, 0.410, 0.500),
    float4(0.410, 0.500, 0.180, 0.500),
    float4(0.410, 0.500, 0.480, 0.430),
    float4(0.480, 0.430, 0.480, 0.200),
    float4(0.480, 0.200, 0.360, 0.080),
    float4(0.360, 0.080, 0.080, 0.080),
    float4(0.380, 0.080, 0.380, 0.920),
    float4(0.380, 0.920, 0.080, 0.340),
    float4(0.080, 0.340, 0.480, 0.340),
    float4(0.480, 0.920, 0.080, 0.920),
    float4(0.080, 0.920, 0.080, 0.560),
    float4(0.080, 0.560, 0.360, 0.560),
    float4(0.360, 0.560, 0.480, 0.440),
    float4(0.480, 0.440, 0.480, 0.200),
    float4(0.480, 0.200, 0.360, 0.080),
    float4(0.360, 0.080, 0.080, 0.080),
    float4(0.360, 0.920, 0.200, 0.920),
    float4(0.200, 0.920, 0.080, 0.800),
    float4(0.080, 0.800, 0.080, 0.200),
    float4(0.080, 0.200, 0.200, 0.080),
    float4(0.200, 0.080, 0.360, 0.080),
    float4(0.360, 0.080, 0.480, 0.200),
    float4(0.480, 0.200, 0.480, 0.420),
    float4(0.480, 0.420, 0.360, 0.540),
    float4(0.360, 0.540, 0.080, 0.540),
    float4(0.080, 0.920, 0.480, 0.920),
    float4(0.480, 0.920, 0.480, 0.800),
    float4(0.480, 0.800, 0.220, 0.080),
    float4(0.200, 0.500, 0.080, 0.596),
    float4(0.080, 0.596, 0.080, 0.800),
    float4(0.080, 0.800, 0.200, 0.920),
    float4(0.200, 0.920, 0.360, 0.920),
    float4(0.360, 0.920, 0.480, 0.800),
    float4(0.480, 0.800, 0.480, 0.596),
    float4(0.480, 0.596, 0.360, 0.500),
    float4(0.360, 0.500, 0.200, 0.500),
    float4(0.200, 0.500, 0.080, 0.404),
    float4(0.080, 0.404, 0.080, 0.200),
    float4(0.080, 0.200, 0.200, 0.080),
    float4(0.200, 0.080, 0.360, 0.080),
    float4(0.360, 0.080, 0.480, 0.200),
    float4(0.480, 0.200, 0.480, 0.404),
    float4(0.480, 0.404, 0.360, 0.500),
    float4(0.200, 0.080, 0.360, 0.080),
    float4(0.360, 0.080, 0.480, 0.200),
    float4(0.480, 0.200, 0.480, 0.800),
    float4(0.480, 0.800, 0.360, 0.920),
    float4(0.360, 0.920, 0.200, 0.920),
    float4(0.200, 0.920, 0.080, 0.800),
    float4(0.080, 0.800, 0.080, 0.580),
    float4(0.080, 0.580, 0.200, 0.460),
    float4(0.200, 0.460, 0.480, 0.460),
    float4(0.060, 0.000, 0.400, 1.000),
    float4(0.080, 0.500, 0.480, 0.500)
};
static const uint2 kRange[12] = { uint2(0, 8), uint2(8, 2), uint2(10, 7), uint2(17, 9), uint2(26, 3), uint2(29, 7), uint2(36, 9), uint2(45, 3), uint2(48, 15), uint2(63, 9), uint2(72, 1), uint2(73, 1) };
float seg_d(float2 p, float4 s)
{
    const float2 pa = p - s.xy, ba = s.zw - s.xy;
    const float  t  = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * t);
}
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float4       c = g_src.Load(int3(p, 0));
    const float2 q = float2(p) + 0.5;
    // The panel: a rounded box, see-through.
    {
        const float2 ctr = 0.5 * (panel.xy + panel.zw), hs = 0.5 * (panel.zw - panel.xy) - pst.x;
        const float2 dq  = abs(q - ctr) - hs;
        const float  d   = length(max(dq, 0.0)) + min(max(dq.x, dq.y), 0.0) - pst.x;
        c.rgb = lerp(c.rgb, cbg.rgb, saturate(0.5 - d) * pst.y);
    }
    // The bar.
    {
        const float2 dq = max(bar.xy - q, q - bar.zw);
        c.rgb = lerp(c.rgb, cbar.rgb, saturate(0.5 - max(dq.x, dq.y)));
    }
    // The characters: the nearest stroke (text units), its character's colour.
    const float2 tu = float2(q.x - org.x, org.y - q.y) / org.z;
    float best = 1e9; uint bc = 0;
    for (uint i = 0; i < n; ++i) {
        const float  s  = ch[i].y;
        const float2 lp = (tu - float2(ch[i].x, 0.0)) / s;
        if (lp.x < -0.3 || lp.x > 0.9 || lp.y < -0.3 || lp.y > 1.3) continue;
        const uint2 r = kRange[(uint)ch[i].z];
        float dm = 1e9;
        for (uint k = 0; k < r.y; ++k) dm = min(dm, seg_d(lp, kSeg[r.x + k]));
        const float d = (dm - org.w) * s;
        if (d < best) { best = d; bc = (uint)ch[i].w; }
    }
    const float dpx = best * org.z;
    c.rgb *= 1.0 - saturate(0.5 - (dpx - pst.z)) * pst.w;   // the rim
    c.rgb = lerp(c.rgb, col[bc & 3].rgb, saturate(0.5 - dpx));
    g_dst[p] = c;
}
)";

    struct WaCb
    {
        uint32_t w, h, n, pad0;
        float    org[4], panel[4], pst[4], bar[4], cbg[4], cbar[4];
        float    ch[16][4];
        float    col[4][4];
    };

    struct WaGpu
    {
        bool                       failed{ false };
        ID3D11ComputeShader*       cs{ nullptr };
        ID3D11Buffer*              cb{ nullptr };
        DXGI_FORMAT                fmt{ DXGI_FORMAT_UNKNOWN };
        UINT                       tw{ 0 }, th{ 0 };
        ID3D11Texture2D*           stash{ nullptr };
        ID3D11ShaderResourceView*  srv{ nullptr };
        ID3D11Texture2D*           mid{ nullptr };
        ID3D11UnorderedAccessView* uav{ nullptr };
        bool                       said{ false };
        uint64_t                   drawn{ 0 };
    } g_wa;

    DXGI_FORMAT wa_typeless(DXGI_FORMAT f)
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
    DXGI_FORMAT wa_view(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
        }
    }
    void wa_release_tex()
    {
        if (g_wa.srv) g_wa.srv->Release();
        if (g_wa.stash) g_wa.stash->Release();
        if (g_wa.uav) g_wa.uav->Release();
        if (g_wa.mid) g_wa.mid->Release();
        g_wa.srv = nullptr; g_wa.stash = nullptr; g_wa.uav = nullptr; g_wa.mid = nullptr;
        g_wa.tw = g_wa.th = 0;
    }
    // The shader once, and the two box textures (at least w x h, the eye's format family).
    bool wa_ready(ID3D11Device* dev, DXGI_FORMAT fmt, UINT w, UINT h)
    {
        if (g_wa.failed) return false;
        if (!g_wa.cs) {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!compile_fn || FAILED(compile_fn(kWaText, strlen(kWaText), "wristammo", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[WristAmmo] the smooth digits' shader failed (the pixel font is drawn instead): {}",
                              err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                if (err) err->Release();
                g_wa.failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_wa.cs);
            code->Release();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(WaCb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_wa.cb);
            if (!g_wa.cs || !g_wa.cb) {
                spdlog::error("[WristAmmo] the smooth digits' shader or buffer not created (the pixel font is drawn instead)");
                g_wa.failed = true;
                return false;
            }
        }
        if (fmt != g_wa.fmt || w > g_wa.tw || h > g_wa.th) {
            wa_release_tex();
            g_wa.fmt = fmt;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = std::max<UINT>(w, 640); nd.Height = std::max<UINT>(h, 256);
            nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT;
            nd.Format    = wa_typeless(fmt);
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = wa_view(fmt); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_wa.stash))) dev->CreateShaderResourceView(g_wa.stash, &sd, &g_wa.srv);
            nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = wa_view(fmt); ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_wa.mid))) dev->CreateUnorderedAccessView(g_wa.mid, &ud, &g_wa.uav);
            const bool ok = g_wa.srv && g_wa.uav;
            spdlog::info("[WristAmmo] smooth digits: box textures {}x{} format {}: {}", nd.Width, nd.Height, (int)fmt, ok ? "ready" : "not created (the pixel font is drawn instead)");
            if (!ok) {
                wa_release_tex();
                g_wa.failed = true;
                return false;
            }
            g_wa.tw = nd.Width;
            g_wa.th = nd.Height;
        }
        return true;
    }
}

// The smooth draw at c (eye pixels), the big digits pu pixels high: a, then "/" b and "/" t smaller. first: 0 ready, 1 not ready (orange), 2
// empty (red); none_left: the third number red. False: not drawn (the shader, its textures, a box too big): the caller draws the pixel font.
bool FarCry5Stereo::wrist_ammo_smooth(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, float cx, float cy, float pu, const std::string& a,
                                      const std::string& b, const std::string& t, int first, bool none_left)
{
    if (g_wa.failed || pu < 4.0f) return false;
    D3D11_TEXTURE2D_DESC d{};
    eye->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1) return false;
    // The layout in text units (the big digits' height): as the preview in research\2026-11-05_wristammo2.
    WaCb  cb{};
    float x = 0.0f;
    int   n = 0;
    const std::string parts[3]{ a, "/" + b, "/" + t };
    for (int i = 0; i < 3; ++i) {
        const float sc = i == 0 ? 1.0f : 0.62f;
        for (const char ch : parts[i]) {
            if (n >= 16) break;
            cb.ch[n][0] = x;
            cb.ch[n][1] = sc;
            cb.ch[n][2] = float(ch >= '0' && ch <= '9' ? ch - '0' : ch == '/' ? 10 : 11);
            cb.ch[n][3] = float(i == 0 ? 0 : (ch == '/' || i == 1) ? 1 : 2);   // colours: the first number, the size and slashes, the rounds left
            x += (ch == '/' ? 0.50f : 0.60f) * sc;
            ++n;
        }
        if (i == 0) x += 0.06f;
    }
    const float pad = 0.28f, acc = 0.09f;
    const float bw = (x + 2.0f * pad + acc) * pu, bh = (1.0f + 2.0f * pad) * pu;
    // The box in the eye picture, clipped to it.
    const float fx0 = cx - 0.5f * bw, fy0 = cy - 0.5f * bh;
    const LONG  X0 = std::max(0L, LONG(std::floor(fx0))) , Y0 = std::max(0L, LONG(std::floor(fy0)));
    const LONG  X1 = std::min(LONG(d.Width), LONG(std::ceil(fx0 + bw))), Y1 = std::min(LONG(d.Height), LONG(std::ceil(fy0 + bh)));
    if (X1 <= X0 || Y1 <= Y0) return true;   // off this eye's picture: nothing to draw
    const UINT w = UINT(X1 - X0), h = UINT(Y1 - Y0);
    if (w > 2048 || h > 1024) return false;   // held against the eyes: the pixel font (clipped) stands in
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const bool ok = wa_ready(dev, d.Format, w, h);
    dev->Release();
    if (!ok) return false;

    // The box's own pixels (its origin at X0, Y0).
    const float ox = fx0 - float(X0), oy = fy0 - float(Y0);
    cb.w = w; cb.h = h; cb.n = UINT(n);
    cb.org[0] = ox + (pad + acc) * pu; cb.org[1] = oy + bh - pad * pu; cb.org[2] = pu; cb.org[3] = 0.075f;
    cb.panel[0] = ox + 1.0f; cb.panel[1] = oy + 1.0f; cb.panel[2] = ox + bw - 1.0f; cb.panel[3] = oy + bh - 1.0f;
    cb.pst[0] = 0.18f * pu; cb.pst[1] = 0.62f; cb.pst[2] = std::max(1.0f, 0.04f * pu); cb.pst[3] = 0.55f;
    cb.bar[0] = cb.panel[0] + 0.10f * pu; cb.bar[2] = cb.bar[0] + std::max(1.5f, acc * 0.6f * pu);
    cb.bar[1] = cb.panel[1] + 0.22f * pu; cb.bar[3] = cb.panel[3] - 0.22f * pu;
    const float bg[4]{ 0.06f, 0.05f, 0.045f, 1.0f }, orange[4]{ 0.93f, 0.42f, 0.13f, 1.0f };
    std::memcpy(cb.cbg, bg, sizeof bg);
    std::memcpy(cb.cbar, orange, sizeof orange);
    static const float kFirst[3][4]{ { 0.97f, 0.95f, 0.90f, 1.0f },    // ready: warm white
                                     { 0.98f, 0.55f, 0.16f, 1.0f },    // not ready: orange
                                     { 0.88f, 0.20f, 0.14f, 1.0f } };  // empty: red
    static const float kGrey[4]{ 0.72f, 0.68f, 0.62f, 1.0f }, kLeft[4]{ 0.88f, 0.85f, 0.79f, 1.0f };
    std::memcpy(cb.col[0], kFirst[std::clamp(first, 0, 2)], sizeof kGrey);
    std::memcpy(cb.col[1], kGrey, sizeof kGrey);
    std::memcpy(cb.col[2], none_left ? kFirst[2] : kLeft, sizeof kGrey);

    wa_call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_wa.cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                       static_cast<const void*>(&cb), 0u, 0u);   // past the mod's hook, as the gesture zones
    const D3D11_BOX src{ UINT(X0), UINT(Y0), 0, UINT(X1), UINT(Y1), 1 };
    ctx->CopySubresourceRegion(g_wa.stash, 0, 0, 0, 0, eye, 0, &src);
    ID3D11ComputeShader*       prev_cs  = nullptr;
    ID3D11ShaderResourceView*  prev_srv = nullptr;
    ID3D11UnorderedAccessView* prev_uav = nullptr;
    ID3D11Buffer*              prev_cb  = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &prev_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ctx->CSSetShader(g_wa.cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_wa.srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_wa.uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_wa.cb);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &prev_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    if (prev_cs) prev_cs->Release();
    if (prev_srv) prev_srv->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    const D3D11_BOX back{ 0, 0, 0, w, h, 1 };
    ctx->CopySubresourceRegion(eye, 0, UINT(X0), UINT(Y0), 0, g_wa.mid, 0, &back);
    if (g_wa.drawn++ % 900 == 0)
        spdlog::info("[WristAmmo] smooth \"{}/{}/{}\" at {:.0f},{:.0f}: box {}x{}, digits {:.0f} px high", a, b, t, cx, cy, w, h, pu);
    return true;
}

struct FarCry5Stereo::WristAmmoState
{
    ID3D11View*      view{ nullptr };
    ID3D11Texture2D* tex{ nullptr };
    bool             said_no_view{ false };
    uint64_t         drawn{ 0 };
    int              said_src{ -1 };            // the source last logged: 0 the game's clip, 1 the count by hand
    int              said_diff{ -1000 };        // the count by hand less the game's clip, last logged
    std::string      text;                      // the text last drawn
    glm::vec3        rel{ 0.0f };               // wristsprint: the last wrist point less this eye pass's camera, held when m_mo is not this frame's
    int              rel_frame{ -1000 };        // the frame it was taken in
    uint64_t         held{ 0 };                 // frames drawn from the held point
};

// After the overlays (both eye-picture paths): the numbers at the weapon hand's wrist, in this eye's picture.
void FarCry5Stereo::wrist_ammo_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    if (!fc5::wristammo::on() || !ctx || !eye || !m_motion.load() || m_menu_screen_on || m_cine_on) return;
    const int fr = m_frame.load();
    if (m_hist[0].cur_frame != fr) return;
    // The weapon hand: the weapon controller, or with the bow in the other hand the weapon hand's own (hand_*). Slung: no gun in hand.
    if (m_mo.slung) return;
    const bool      bow = m_mo.bow;
    if (!bow && !m_mo.ctrl_valid) return;
    int clip = -1, reserve = -1, cap = -1;
    if (!fc5::lua::player_ammo(clip, reserve, &cap)) return;

    int  rounds = clip, full = cap, left = reserve;
    bool held = false, by_hand = false;
    fc5::reload::View rv{};
    if (fc5::reload::view(rv) && rv.active && rv.rounds >= 0) {
        by_hand = true;
        rounds  = rv.rounds;
        full    = rv.rounds_cap > 0 ? rv.rounds_cap : cap > 0 ? std::max(1, cap - rv.rounds_keep) : -1;
        left    = reserve + rv.rounds_aside;
        held    = rv.fire_held;
    }
    if (full <= 0 && rounds <= 0 && left <= 0) return;   // the hands: nothing to count
    // A melee weapon: the game gives it a clip and a reserve of its own (run final_b, the pipe: 1/1/8).
    if (const fc5::weapons::Info* w = fc5::gameweapon::info(m_weapon_sig.load()); w && w->cls == 0) return;

    auto& s = m_wa ? *m_wa : *(m_wa = new WristAmmoState{});
    {
        // The log: the source at each change, and the count by hand against the game's clip whenever they part (the reload thread's check).
        const int src = by_hand ? 1 : 0;
        const int diff = by_hand ? rounds - clip : 0;
        if (src != s.said_src || diff != s.said_diff) {
            spdlog::info("[WristAmmo] {}: {} / {} / {} (game clip {} of {}, reserve {}{})", by_hand ? "count by hand" : "the game's count", rounds, full, left, clip, cap, reserve,
                         by_hand ? fmt::format("; by hand {} {} the clip, aside {}, kept {}{}", rounds, diff == 0 ? "matches" : diff > 0 ? "is above" : "is below",
                                               rv.rounds_aside, rv.rounds_keep, held ? ", the gun held from firing" : "")
                                 : std::string{});
            s.said_src  = src;
            s.said_diff = diff;
        }
    }

    // The eye picture's view (as bonemap): a render target or an unordered access view for ClearView.
    if (eye != s.tex) {
        if (s.view) s.view->Release();
        s.view = nullptr;
        s.tex  = eye;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC td{};
            eye->GetDesc(&td);
            if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
                ID3D11RenderTargetView* v = nullptr;
                dev->CreateRenderTargetView(eye, nullptr, &v);
                s.view = v;
            } else if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
                ID3D11UnorderedAccessView* v = nullptr;
                dev->CreateUnorderedAccessView(eye, nullptr, &v);
                s.view = v;
            }
            dev->Release();
        }
        if (!s.view && !s.said_no_view) {
            s.said_no_view = true;
            spdlog::info("[WristAmmo] the eye picture cannot be drawn on (no render target or UAV binding): nothing drawn");
        }
    }

    const fc5::wristammo::Opts o = fc5::wristammo::opts();
    const glm::mat3 R   = bow ? m_mo.hand_rot_world : m_mo.ctrl_rot_world;
    const glm::vec3 off{ left_handed() ? -o.pos[0] : o.pos[0], o.pos[1], o.pos[2] };
    // wristsprint: ctrl_pos_world / hand_pos_world already carry draw_shift (gun_anchor_apply moves them onto the eyes' base, as the drawn
    // gun and hands); it is not added again. A pose from another frame than this eye picture's (m_mo updated ahead of the present) would put
    // the numbers on that frame's camera: the last point is kept in this eye's frame for up to 3 frames instead.
    glm::vec3 P{ 0.0f };
    if (m_mo.frame == fr) {
        P          = (bow ? m_mo.hand_pos_world : m_mo.ctrl_pos_world) + R * off;
        s.rel      = P - m_hist[0].cur_cam;
        s.rel_frame = fr;
    } else if (fr - s.rel_frame >= 0 && fr - s.rel_frame <= 3) {
        P = m_hist[0].cur_cam + s.rel;
        if (s.held++ % 300 == 0) spdlog::info("[WristAmmo] wristsprint: the hand's pose is from frame {} at the picture of frame {}: the last point held ({} times)", m_mo.frame, fr, s.held);
    } else {
        return;
    }

    D3D11_TEXTURE2D_DESC d{};
    eye->GetDesc(&d);
    const float W = float(d.Width), H = float(d.Height);
    auto proj = [&](const glm::vec3& p, glm::vec2& px) {
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ p - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.05f) return false;
        px = { (cl.x / cl.w * 0.5f + 0.5f) * W, (0.5f - cl.y / cl.w * 0.5f) * H };
        return true;
    };
    glm::vec2 c{};
    if (!proj(P, c)) return;
    // Pixels per metre at the wrist: a 1 cm step across the view (level, square to the line from the eye), so the size does not breathe as
    // the head turns against the world's axes (wristsprint). Looking straight down on it: the largest step along the world's three axes.
    float ppm = 0.0f;
    {
        glm::vec3       across = glm::cross(P - m_hist[0].cur_cam, glm::vec3{ 0.0f, 0.0f, 1.0f });
        const float     al     = glm::length(across);
        glm::vec2       q{};
        if (al > 1e-3f && proj(P + across * (0.01f / al), q)) ppm = glm::length(q - c) / 0.01f;
    }
    if (ppm <= 0.0f) {
        for (int a = 0; a < 3; ++a) {
            glm::vec3 e{ 0.0f };
            e[a] = 0.01f;
            glm::vec2 q{};
            if (proj(P + e, q)) ppm = std::max(ppm, glm::length(q - c) / 0.01f);
        }
    }
    if (ppm <= 0.0f) return;
    const float k = std::clamp(o.size * ppm / float(fc5::bonemap::kGlyphH), 1.0f, 16.0f);   // pixels per font pixel

    const std::string a = rounds >= 0 ? std::to_string(rounds) : std::string{ "-" };
    const std::string b = "/" + (full > 0 ? std::to_string(full) : std::string{ "-" });
    const std::string t = "/" + (left >= 0 ? std::to_string(left) : std::string{ "-" });
    const std::string all = a + b + t;
    s.text = all;
    // wristammostyle smooth (default): the smooth digits; the pixel font below when they cannot be drawn, or with wristammostyle pixel.
    if (o.smooth && wrist_ammo_smooth(ctx, eye, c.x, c.y, o.size * ppm, a, b.substr(1), t.substr(1), rounds == 0 ? 2 : held ? 1 : 0, left == 0)) return;
    if (!s.view) return;
    const float pad = 2.0f * k;
    const float tw = float(all.size() * fc5::bonemap::kGlyphW) * k, th = float(fc5::bonemap::kGlyphH) * k;
    const float x0 = c.x - 0.5f * tw, y0 = c.y - 0.5f * th;
    if (x0 - pad > W || y0 - pad > H || x0 + tw + pad < 0.0f || y0 + th + pad < 0.0f) return;   // off this eye's picture

    auto rect = [&](float l, float tp, float r, float bt, std::vector<D3D11_RECT>& v) {
        D3D11_RECT q{ LONG(std::lround(std::max(0.0f, l))), LONG(std::lround(std::max(0.0f, tp))), LONG(std::lround(std::min(W, r))), LONG(std::lround(std::min(H, bt))) };
        if (q.right > q.left && q.bottom > q.top) v.push_back(q);
    };
    std::vector<D3D11_RECT> box, first, rest_r, last;
    rect(x0 - pad, y0 - pad, x0 + tw + pad, y0 + th + pad, box);
    for (size_t i = 0; i < all.size(); ++i) {
        std::vector<D3D11_RECT>& dst = i < a.size() ? first : i < a.size() + b.size() ? rest_r : last;
        const uint8_t* g  = fc5::bonemap::glyph(all[i]);
        const float    gx = x0 + float(i * fc5::bonemap::kGlyphW) * k;
        for (int row = 0; row < fc5::bonemap::kGlyphH; ++row) {
            const uint8_t bits = g[row];
            for (int bx = 0; bx < 8;) {
                if (!(bits & (0x80 >> bx))) { ++bx; continue; }
                int ex = bx;
                while (ex < 8 && (bits & (0x80 >> ex))) ++ex;
                rect(gx + float(bx) * k, y0 + float(row) * k, gx + float(ex) * k, y0 + float(row + 1) * k, dst);
                bx = ex;
            }
        }
    }
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return;
    auto clear = [&](const float col[4], const std::vector<D3D11_RECT>& v) {
        for (size_t i = 0; i < v.size(); i += 512) ctx1->ClearView(s.view, col, v.data() + i, UINT(std::min<size_t>(512, v.size() - i)));
    };
    static const float kBox[4]{ 0.02f, 0.02f, 0.025f, 1.0f };
    static const float kWhite[4]{ 0.95f, 0.95f, 0.95f, 1.0f };
    static const float kAmber[4]{ 1.0f, 0.62f, 0.12f, 1.0f };
    static const float kRed[4]{ 1.0f, 0.22f, 0.18f, 1.0f };
    static const float kGrey[4]{ 0.62f, 0.66f, 0.70f, 1.0f };
    clear(kBox, box);
    clear(rounds == 0 ? kRed : held ? kAmber : kWhite, first);
    clear(kGrey, rest_r);
    clear(left == 0 ? kRed : kGrey, last);
    ctx1->Release();
    if (s.drawn++ % 900 == 0)
        spdlog::info("[WristAmmo] \"{}\" ({}) at {:.0f},{:.0f} in an eye picture {}x{}, {:.1f} px per font pixel; draw_shift {:.1f} cm (in the hand's pose, not added again)", all,
                     by_hand ? "by hand" : "the game's", c.x, c.y, d.Width, d.Height, k, glm::length(m_mo.draw_shift) * 100.0f);
}
