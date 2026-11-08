// reloadhintstyle glow (FarCry5ReloadHint.h): the reload's part to work next drawn again into a hint mask right after the game's draw of it,
// for the zones overlay to light on the eye picture (FarCry5GestureZones.cpp). reload_mag_draw (FarCry5ReloadMag.cpp) queues the jobs for the
// draw it looks at (each a palette with only the part's bones placed, or a rigid part's object constants); hk_draw_indexed runs them after the
// game's draw, while that draw's shaders, buffers, viewport and depth are still bound.
#include "FarCry5Stereo.h"
#include "FarCry5ReloadHint.h"
#include "FarCry5Record.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace fc5::reloadhint
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        // Every pixel the part covers: 1 into the channel the blend state lets through.
        const char* kMaskPs = R"(
float4 main() : SV_Target0 { return float4(1.0f, 1.0f, 1.0f, 1.0f); }
)";
        constexpr UINT kPalBytes = 12288;   // a weapon draw's bone palette (256 bones of 3x4)
        constexpr UINT kObjBytes = 128;     // a rigid draw's object constants

        ID3D11PixelShader*        g_ps       = nullptr;
        ID3D11BlendState*         g_write[4] = {};        // writes R, G, B or A only
        ID3D11DepthStencilState*  g_nodepth  = nullptr;   // the see-through pass
        ID3D11Buffer*             g_pal      = nullptr;
        ID3D11Buffer*             g_obj      = nullptr;
        bool                      g_failed   = false;
        ID3D11Texture2D*          g_mask     = nullptr;   // R, G: the magazine seen, anywhere; B, A: the handle seen, anywhere
        ID3D11RenderTargetView*   g_mrtv     = nullptr;
        ID3D11ShaderResourceView* g_msrv     = nullptr;
        UINT                      g_mw = 0, g_mh = 0;
        int                       g_mask_frame = -100;
        float                     g_vp[4]{};               // the weapon pass's viewport in the mask
        int                       g_drawn_frame[kGroups]{ -100, -100 };
        float                     g_col[kGroups][4]{};

        // The queue: the jobs for one draw (render thread).
        std::vector<Job> q_jobs;
        UINT             q_count = 0, q_start = 0;
        INT              q_base  = 0;
        int              q_frame = -100;

        // The draws that went into this frame's mask: one seen again is a second weapon pass of the same frame (framedouble), which the eye
        // picture is left with, so the mask starts again from it (one gun, not two side by side).
        struct Seen { UINT count, start; INT base; int group; };
        std::vector<Seen> g_seen;

        // The log: a line every 10 s while parts go in, the first time each way.
        uint64_t          g_n_draws[kGroups]{}, g_n_fail{}, g_n_skip{}, g_n_again{};
        clock::time_point g_said{};
        bool              g_said_ready = false;
        const char*       g_last_fail  = "";

        bool init(ID3D11Device* dev)
        {
            if (g_ps || g_failed) return !g_failed;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!compile_fn || FAILED(compile_fn(kMaskPs, strlen(kMaskPs), "reloadhint_mask", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &err))) {
                spdlog::error("[ReloadHint] mask shader failed: {} (the boxes are drawn instead)", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps);
            code->Release();
            for (int c = 0; c < 4; ++c) {
                D3D11_BLEND_DESC bd{};
                bd.RenderTarget[0].BlendEnable           = FALSE;
                bd.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(1u << c);
                dev->CreateBlendState(&bd, &g_write[c]);
            }
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable    = FALSE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
            dd.StencilEnable  = FALSE;
            dev->CreateDepthStencilState(&dd, &g_nodepth);
            auto cb = [&](UINT bytes, ID3D11Buffer** out) {
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = bytes; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                dev->CreateBuffer(&d, nullptr, out);
            };
            cb(kPalBytes, &g_pal);
            cb(kObjBytes, &g_obj);
            g_failed = !g_ps || !g_nodepth || !g_pal || !g_obj || !g_write[0] || !g_write[1] || !g_write[2] || !g_write[3];
            spdlog::info("[ReloadHint] glow mask shaders {}", g_failed ? "unavailable (the boxes are drawn instead)" : "ready");
            return !g_failed;
        }

        bool mask_for(ID3D11Device* dev, UINT w, UINT h)
        {
            if (g_mask && g_mw == w && g_mh == h) return true;
            if (g_msrv) g_msrv->Release();
            if (g_mrtv) g_mrtv->Release();
            if (g_mask) g_mask->Release();
            g_msrv = nullptr; g_mrtv = nullptr; g_mask = nullptr;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = w; nd.Height = h; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc.Count = 1;
            nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_mask))) {
                dev->CreateRenderTargetView(g_mask, nullptr, &g_mrtv);
                dev->CreateShaderResourceView(g_mask, nullptr, &g_msrv);
            }
            g_mw = w; g_mh = h;
            g_mask_frame = -100;
            spdlog::info("[ReloadHint] glow mask {}x{}: {}", w, h, g_mrtv && g_msrv ? "ready" : "failed");
            return g_mrtv && g_msrv;
        }
    }

    void begin_draw(UINT count, UINT start, INT base, int frame)
    {
        q_jobs.clear();
        q_count = count;
        q_start = start;
        q_base  = base;
        q_frame = frame;
    }

    void queue(Job&& j)
    {
        if (j.group < 0 || j.group >= kGroups || q_jobs.size() >= 6) return;
        if (j.rigid ? j.data.size() < kObjBytes / 4 : j.data.size() < kPalBytes / 4) return;
        q_jobs.push_back(std::move(j));
    }

    void set_colour(int group, float r, float g, float b, float strength)
    {
        if (group < 0 || group >= kGroups) return;
        g_col[group][0] = r; g_col[group][1] = g; g_col[group][2] = b; g_col[group][3] = strength;
    }

    bool composite(int frame, float eye_w, float eye_h, Composite& c)
    {
        if (!g_msrv || g_mask_frame != frame || eye_w < 1.0f || eye_h < 1.0f || g_mw == 0 || g_mh == 0) return false;
        bool any = false;
        for (int g = 0; g < kGroups; ++g) {
            const bool on = g_drawn_frame[g] == frame;
            for (int k = 0; k < 4; ++k) c.col[g][k] = on ? g_col[g][k] : 0.0f;
            if (!on) c.col[g][3] = 0.0f;
            any = any || on;
        }
        if (!any) return false;
        const float mw = float(g_mw), mh = float(g_mh);
        // Eye pixel q -> mask uv: the eye picture is the weapon pass's viewport, upscaled (as reddot_draw maps its window mask).
        c.srv       = g_msrv;
        c.mscale[0] = g_vp[2] / eye_w / mw;
        c.mscale[1] = g_vp[3] / eye_h / mh;
        c.moff[0]   = g_vp[0] / mw;
        c.moff[1]   = g_vp[1] / mh;
        c.texel[0]  = 1.0f / mw;
        c.texel[1]  = 1.0f / mh;
        return true;
    }

    bool drawn(int group, int frame) { return group >= 0 && group < kGroups && g_drawn_frame[group] == frame; }
}

// After the game's draw of a part reload_mag_draw queued jobs for: each job drawn into the hint mask, with this draw's own vertex shader, index and
// vertex buffers, viewport and depth; only the bone palette (or a rigid part's object constants) is the job's. Everything changed is put back.
void FarCry5Stereo::reload_hint_after(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    namespace rh = fc5::reloadhint;
    m_hint_queued = false;
    if (const auto now = rh::clock::now(); now - rh::g_said > std::chrono::seconds(10)) {
        if (rh::g_n_draws[0] + rh::g_n_draws[1] + rh::g_n_skip + rh::g_n_fail + rh::g_n_again > 0)
            spdlog::info("[ReloadHint] last 10 s: part draws into the glow mask: magazine {}, handle {}; a second pass in the same frame (mask started again) {}; skipped {} "
                         "(scope, HUD, menu); failed {}{}", rh::g_n_draws[0], rh::g_n_draws[1], rh::g_n_again, rh::g_n_skip, rh::g_n_fail,
                         rh::g_n_fail ? fmt::format(" (last: {})", rh::g_last_fail) : std::string{});
        rh::g_said       = now;
        rh::g_n_draws[0] = rh::g_n_draws[1] = rh::g_n_skip = rh::g_n_fail = rh::g_n_again = 0;
    }
    std::vector<rh::Job> jobs;
    jobs.swap(rh::q_jobs);
    const int frame = m_frame.load();
    if (jobs.empty() || rh::q_count != count || rh::q_start != start || rh::q_base != base || rh::q_frame != frame) return;
    if (fc5::rec::is_deferred(ctx) || m_scope_call || m_hud_redirected || m_menu_screen_on || !ctx) {
        rh::g_n_skip++;
        return;
    }
    auto fail = [&](const char* why) {
        rh::g_n_fail++;
        rh::g_last_fail = why;
    };
    // A draw that also writes UAVs is never drawn twice (as afwfpeye).
    {
        ID3D11UnorderedAccessView* uav[8]{};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uav);
        bool any_uav = false;
        for (auto* u : uav) if (u) { any_uav = true; u->Release(); }
        if (any_uav) return fail("the draw writes UAVs");
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return fail("no device");
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    auto release_targets = [&]() {
        for (auto*& v : rtvs) { if (v) v->Release(); v = nullptr; }
        if (dsv) dsv->Release();
        dsv = nullptr;
    };
    // The mask has the size of the depth target (or of the first colour target without one), as reddot's window mask.
    D3D11_TEXTURE2D_DESC td{};
    {
        ID3D11Resource* res = nullptr;
        if (dsv) dsv->GetResource(&res);
        else if (rtvs[0]) rtvs[0]->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim{};
        if (res) res->GetType(&dim);
        if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        if (res) res->Release();
    }
    if (td.Width == 0 || td.SampleDesc.Count != 1 || !rh::init(dev) || !rh::mask_for(dev, td.Width, td.Height)) {
        release_targets();
        dev->Release();
        return fail(td.Width == 0 ? "no target bound" : td.SampleDesc.Count != 1 ? "a multisampled target" : "no mask");
    }
    bool again = false;
    if (rh::g_mask_frame == frame)
        for (const auto& j : jobs)
            for (const auto& d : rh::g_seen)
                if (d.count == count && d.start == start && d.base == base && d.group == j.group) again = true;
    if (rh::g_mask_frame != frame || again) {
        const float zero[4]{};
        ctx->ClearRenderTargetView(rh::g_mrtv, zero);
        if (again) rh::g_n_again++;
        rh::g_mask_frame = frame;
        rh::g_seen.clear();
        for (int& f : rh::g_drawn_frame) f = -100;
    }
    {
        UINT           nvp = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nvp, &vp);
        if (nvp == 0 || vp.Width < 1.0f) { vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f; vp.Width = float(td.Width); vp.Height = float(td.Height); }
        rh::g_vp[0] = vp.TopLeftX; rh::g_vp[1] = vp.TopLeftY; rh::g_vp[2] = vp.Width; rh::g_vp[3] = vp.Height;
    }

    // What the passes change, saved.
    ID3D11BlendState*        blend = nullptr;
    float                    factor[4]{};
    UINT                     smask = 0;
    ctx->OMGetBlendState(&blend, factor, &smask);
    ID3D11DepthStencilState* gdss = nullptr;
    UINT                     gref = 0;
    ctx->OMGetDepthStencilState(&gdss, &gref);
    ID3D11RasterizerState*   grs = nullptr;
    ctx->RSGetState(&grs);
    ID3D11PixelShader*       ps = nullptr;
    ID3D11ClassInstance*     inst[16]{};
    UINT                     ninst = 16;
    ctx->PSGetShader(&ps, inst, &ninst);
    ID3D11Buffer*            vs_was[4]{};   // slots 1 to 4
    ctx->VSGetConstantBuffers(1, 4, vs_was);

    // The seen pass: the game's own depth test, no depth or stencil writes, an equal depth passing (the part already wrote its own) and the part
    // pulled a little towards the eye (a vertex shared with a collapsed bone can sit a hair behind where the game drew it).
    D3D11_DEPTH_STENCIL_DESC dd{};
    bool                     depth = dsv != nullptr;
    if (gdss) {
        gdss->GetDesc(&dd);
        depth = depth && dd.DepthEnable;
    } else {
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    }
    const bool rev = dd.DepthFunc == D3D11_COMPARISON_GREATER || dd.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;   // reversed Z: nearer is larger
    ID3D11DepthStencilState* seen_dss = nullptr;
    ID3D11RasterizerState*   seen_rs  = nullptr;
    if (depth) {
        D3D11_DEPTH_STENCIL_DESC sd = dd;
        sd.DepthWriteMask           = D3D11_DEPTH_WRITE_MASK_ZERO;
        sd.StencilEnable            = FALSE;
        if (sd.DepthFunc == D3D11_COMPARISON_LESS) sd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (sd.DepthFunc == D3D11_COMPARISON_GREATER) sd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        dev->CreateDepthStencilState(&sd, &seen_dss);
        D3D11_RASTERIZER_DESC rd{};
        if (grs) grs->GetDesc(&rd);
        else {
            rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE;
        }
        rd.DepthBias            = rev ? 48 : -48;
        rd.SlopeScaledDepthBias = rev ? 1.0f : -1.0f;
        rd.DepthBiasClamp       = 0.0f;
        dev->CreateRasterizerState(&rd, &seen_rs);
        if (!seen_dss || !seen_rs) depth = false;
    }
    dev->Release();

    call_orig<void>(m_om_set_rts_hook, 33, ctx, 1u, static_cast<ID3D11RenderTargetView* const*>(&rh::g_mrtv), dsv);
    call_orig<void>(m_ps_set_hook, 9, ctx, rh::g_ps, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
    for (const auto& j : jobs) {
        if (j.rigid) {
            float o[32];
            std::memcpy(o, j.data.data(), sizeof(o));
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(rh::g_obj), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o), 0u, 0u);
            ID3D11Buffer* nb[3]{ rh::g_obj, vs_was[1], rh::g_obj };   // slots 1 to 3, as reload_mag_draw's rigid parts
            call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(nb));
        } else {
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(rh::g_pal), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(j.data.data()), 0u, 0u);
            ID3D11Buffer* nb[2]{ rh::g_pal, rh::g_pal };   // slots 3 and 4, as the gloves' and the magazine's palettes
            call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(nb));
        }
        const int ch = j.group * 2;
        if (j.seen && depth) {
            ctx->OMSetBlendState(rh::g_write[ch], nullptr, 0xffffffffu);
            ctx->OMSetDepthStencilState(seen_dss, 0);
            ctx->RSSetState(seen_rs);
            call_orig<void>(m_draw_idx_hook, 12, ctx, count, start, base);
        }
        ctx->OMSetBlendState(rh::g_write[ch + 1], nullptr, 0xffffffffu);
        ctx->OMSetDepthStencilState(rh::g_nodepth, 0);
        ctx->RSSetState(grs);
        call_orig<void>(m_draw_idx_hook, 12, ctx, count, start, base);
        if (j.seen && !depth) {   // no depth to test against: what is drawn counts as seen
            ctx->OMSetBlendState(rh::g_write[ch], nullptr, 0xffffffffu);
            call_orig<void>(m_draw_idx_hook, 12, ctx, count, start, base);
        }
        rh::g_drawn_frame[j.group] = frame;
        rh::g_n_draws[j.group]++;
        if (rh::g_seen.size() < 16) rh::g_seen.push_back({ count, start, base, j.group });
    }

    // Put back.
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 4u, static_cast<ID3D11Buffer* const*>(vs_was));
    call_orig<void>(m_ps_set_hook, 9, ctx, ps, static_cast<ID3D11ClassInstance* const*>(inst), ninst);
    call_orig<void>(m_om_set_rts_hook, 33, ctx, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT), static_cast<ID3D11RenderTargetView* const*>(rtvs), dsv);
    ctx->OMSetBlendState(blend, factor, smask);
    ctx->OMSetDepthStencilState(gdss, gref);
    ctx->RSSetState(grs);
    for (auto* b : vs_was) if (b) b->Release();
    if (ps) ps->Release();
    for (UINT i = 0; i < ninst && i < 16; ++i) if (inst[i]) inst[i]->Release();
    if (blend) blend->Release();
    if (gdss) gdss->Release();
    if (grs) grs->Release();
    if (seen_dss) seen_dss->Release();
    if (seen_rs) seen_rs->Release();
    release_targets();

    if (!rh::g_said_ready) {
        rh::g_said_ready = true;
        spdlog::info("[ReloadHint] first part drawn into the glow mask (frame {}, mask {}x{}, viewport {:.0f},{:.0f} {:.0f}x{:.0f}, depth test {}{})", frame, rh::g_mw, rh::g_mh,
                     rh::g_vp[0], rh::g_vp[1], rh::g_vp[2], rh::g_vp[3], depth ? "on" : "off", rev ? ", reversed Z" : "");
    }
}
