#include "FarCry5HidMask.h"
#include "FarCry5OwnPass.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <openxr/openxr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

namespace fc5::hidmask
{
    namespace
    {
        // Near and far variants of one vertex shader: the depth is part of the shader, so nothing is uploaded per stamp (no Map,
        // no UpdateSubresource: those go through the mod's hooks).
        const char* kSrc = R"(
float4 main(float2 p : POSITION) : SV_Position { return float4(p, DEPTH_Z, 1.0); }
)";
        // hidmask black (author 14 Nov 12:23Z): the
        // hidden triangles filled black in the finished eye image, so the headset's reprojection shows black there on a fast turn instead
        // of the leftovers the game no longer draws. Two variants: an ordinary colour target (the swap chain's eye image, DLAA) and a
        // UAV-only image (the DLSS eye image), written from the pixel shader with no render target bound (like hudmarksize's boxes).
        // afwpereye: a third variant (MARK) writes AFW's "no texel here" value into its R32F depth copy (kHiddenMark).
        const char* kBlackSrc = R"(
#ifdef USE_UAV
RWTexture2D<float4> img : register(u0);
void main(float4 pos : SV_Position) { img[uint2(pos.xy)] = float4(0.0, 0.0, 0.0, 1.0); }
#elif defined(MARK)
float4 main(float4 pos : SV_Position) : SV_Target { return float4(1.0e30, 1.0e30, 1.0e30, 1.0); }
#else
float4 main(float4 pos : SV_Position) : SV_Target { return float4(0.0, 0.0, 0.0, 1.0); }
#endif
)";
        std::atomic<int> g_mode{ kOff };
        // hidmask cover (author 14 Nov 12:43Z): 1 = the runtime's whole hidden area; 0.5 = each inner corner of the mesh moved half
        // way out to the image edge, along its line from the eye's straight-ahead point, so the masked band is half as deep. 50 % is the
        // default (author 12:44Z).
        std::atomic<float> g_cover{ kDefaultCover };
        // hidmaskturn (author 14 Nov 16:14Z): in a head turn
        // faster than this (degrees per second; 0 = never), the frames are drawn without the mask, so a late frame the headset shifts
        // shows picture at the lens edge, not black. The speed holds for about 0.3 s after the turn slows (store_xr_views).
        std::atomic<float>    g_turn_dps{ kDefaultTurnDps };
        std::atomic<float>    g_head_dps{ 0.0f };
        std::atomic<bool>     g_turning{ false };
        std::atomic<uint64_t> g_turn_frames{ 0 };

        std::mutex             g_mtx;   // the triangles (fetch on any thread, stamp on the render thread)
        std::vector<XrVector2f> g_tri[2];   // per eye: the hidden triangles' corners, three per triangle, in tangent units (z = -1 plane)
        bool                   g_have = false;
        bool                   g_refetch = false;            // a changed hidden area to read again
        std::atomic<uint32_t>  g_gen{ 1 };                   // mesh generation: the eye buffers are rebuilt when it moves
        int                    g_tries = 0;
        bool                   g_no_ext_logged = false;
        std::chrono::steady_clock::time_point g_next_try{};

        ID3D11VertexShader*      g_vs[2]{};   // 0: far (z 0), 1: near (z 1)
        ID3D11InputLayout*       g_layout = nullptr;
        ID3D11DepthStencilState* g_dss    = nullptr;
        ID3D11RasterizerState*   g_rs     = nullptr;
        ID3DDeviceContextState*  g_state  = nullptr;
        bool                     g_failed = false;

        struct EyeVb
        {
            ID3D11Buffer* vb = nullptr;
            UINT          count = 0;
            uint32_t      gen = 0;
            float         tan[4]{};
            float         hidden = 0.0f;   // share of the eye image (0-1)
        };
        EyeVb g_eye[2];

        ID3D11PixelShader* g_ps_black[3]{};   // 0: render target, 1: UAV, 2: AFW's hidden mark (afwpereye)
        bool               g_black_failed = false;
        bool               g_black_logged = false;   // one line for an eye image it cannot draw into
        std::atomic<uint64_t> g_black[2]{};          // [eye]
        std::atomic<uint64_t> g_marks[2]{};          // [eye] afwpereye: AFW depth copies marked
        std::atomic<uint64_t> g_black_fail{ 0 };

        std::atomic<uint64_t> g_stamps[2][2]{};   // [eye][near]
        std::atomic<uint64_t> g_fail{ 0 };
        bool                  g_vp_logged = false;

        bool init(ID3D11Device* dev)
        {
            if (g_state || g_failed) return g_state != nullptr;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            bool ok = fn != nullptr;
            ID3DBlob* near_code = nullptr;
            for (int k = 0; k < 2 && ok; ++k) {
                const D3D_SHADER_MACRO defs[]{ { "DEPTH_Z", k ? "1.0" : "0.0" }, { nullptr, nullptr } };
                ID3DBlob* code = nullptr;
                ID3DBlob* err  = nullptr;
                ok = SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5_hidmask", defs, nullptr, "main", "vs_5_0", 0, 0, &code, &err)) &&
                     SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vs[k]));
                if (!ok) spdlog::error("[HidMask] shader: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                if (k == 1 && code) { near_code = code; code = nullptr; }
                if (code) code->Release();
            }
            if (ok && near_code) {
                const D3D11_INPUT_ELEMENT_DESC el{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
                ok = SUCCEEDED(dev->CreateInputLayout(&el, 1, near_code->GetBufferPointer(), near_code->GetBufferSize(), &g_layout));
            }
            if (near_code) near_code->Release();
            if (ok) {
                D3D11_DEPTH_STENCIL_DESC d{};
                d.DepthEnable    = TRUE;
                d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                d.DepthFunc      = D3D11_COMPARISON_ALWAYS;
                d.StencilEnable  = FALSE;   // the game's stencil stays as it cleared it
                ok = SUCCEEDED(dev->CreateDepthStencilState(&d, &g_dss));
            }
            if (ok) {
                D3D11_RASTERIZER_DESC r{};
                r.FillMode        = D3D11_FILL_SOLID;
                r.CullMode        = D3D11_CULL_NONE;
                r.DepthClipEnable = FALSE;   // z exactly 0 or 1
                ok = SUCCEEDED(dev->CreateRasterizerState(&r, &g_rs));
            }
            ID3D11Device1* dev1 = nullptr;
            if (ok && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
                const D3D_FEATURE_LEVEL lvl = dev->GetFeatureLevel();
                ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &lvl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state));
                dev1->Release();
            } else {
                ok = false;
            }
            if (!ok) {
                g_failed = true;
                spdlog::error("[HidMask] could not make its shaders or states: hidmask stays off");
            } else {
                spdlog::info("[HidMask] ready");
            }
            return ok;
        }

        // The eye's triangles in the eye image's clip space, for these tangents.
        bool build(ID3D11Device* dev, int eye, const float tan[4])
        {
            EyeVb& e = g_eye[eye];
            bool same = e.vb != nullptr && e.gen == g_gen.load();
            for (int k = 0; k < 4 && same; ++k) same = std::abs(e.tan[k] - tan[k]) < 1e-6f;
            if (same) return true;
            if (e.vb) { e.vb->Release(); e.vb = nullptr; e.count = 0; }
            e.gen = g_gen.load();
            std::vector<float> v;
            {
                std::lock_guard _{ g_mtx };
                const auto& t = g_tri[eye];
                v.reserve(t.size() * 2);
                const float l = tan[0], r = tan[1], u = tan[2], d = tan[3];
                const float k = 1.0f - std::clamp(g_cover.load(), 0.1f, 1.0f);   // share of the way out to the edge
                const bool  centred = l < 0.0f && r > 0.0f && d < 0.0f && u > 0.0f;
                for (const auto& p0 : t) {
                    XrVector2f p = p0;
                    if (k > 0.0f && centred && (p.x != 0.0f || p.y != 0.0f)) {
                        // Where the line from straight ahead through p leaves the image (p * te); a corner on or past the edge stays.
                        const float tx = p.x > 0.0f ? r / p.x : p.x < 0.0f ? l / p.x : 1e30f;
                        const float ty = p.y > 0.0f ? u / p.y : p.y < 0.0f ? d / p.y : 1e30f;
                        const float te = std::min(tx, ty);
                        if (te > 1.0f) {
                            const float s = 1.0f + k * (te - 1.0f);
                            p.x *= s;
                            p.y *= s;
                        }
                    }
                    v.push_back((2.0f * p.x - (r + l)) / (r - l));
                    v.push_back((2.0f * p.y - (u + d)) / (u - d));
                }
            }
            if (v.size() < 6) return false;
            // The hidden share of the image: each triangle clipped to the image square by sampling (cheap, once per tangent change).
            constexpr int kGrid = 128;
            int hidden = 0;
            for (int gy = 0; gy < kGrid; ++gy) {
                for (int gx = 0; gx < kGrid; ++gx) {
                    const float x = -1.0f + (gx + 0.5f) * (2.0f / kGrid), y = -1.0f + (gy + 0.5f) * (2.0f / kGrid);
                    for (size_t i = 0; i + 5 < v.size(); i += 6) {
                        const float ax = v[i], ay = v[i + 1], bx = v[i + 2], by = v[i + 3], cx = v[i + 4], cy = v[i + 5];
                        const float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
                        const float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
                        const float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
                        const bool  neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                        if (!(neg && pos)) { hidden++; break; }
                    }
                }
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = UINT(v.size() * sizeof(float));
            bd.Usage     = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{ v.data(), 0, 0 };
            if (FAILED(dev->CreateBuffer(&bd, &init, &e.vb))) { e.vb = nullptr; return false; }
            e.count  = UINT(v.size() / 2);
            e.hidden = float(hidden) / float(kGrid * kGrid);
            std::memcpy(e.tan, tan, sizeof e.tan);
            spdlog::info("[HidMask] {} eye: {} hidden triangles, cover {:.0f} %, {:.1f} % of the eye image (tangents L {:.3f} R {:.3f} U {:.3f} D {:.3f})", eye ? "right" : "left", e.count / 3,
                         g_cover.load() * 100.0f, e.hidden * 100.0f, tan[0], tan[1], tan[2], tan[3]);
            return true;
        }
    }

    namespace
    {
        bool black_init(ID3D11Device* dev)
        {
            if (g_ps_black[0] || g_black_failed) return g_ps_black[0] != nullptr;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            bool ok = fn != nullptr;
            for (int k = 0; k < 3 && ok; ++k) {
                const D3D_SHADER_MACRO uav[]{ { "USE_UAV", "1" }, { nullptr, nullptr } };
                const D3D_SHADER_MACRO mrk[]{ { "MARK", "1" }, { nullptr, nullptr } };
                ID3DBlob* code = nullptr;
                ID3DBlob* err  = nullptr;
                ok = SUCCEEDED(fn(kBlackSrc, std::strlen(kBlackSrc), "fc5_hidmask_black", k == 1 ? uav : k == 2 ? mrk : nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &err)) &&
                     SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_black[k]));
                if (!ok) spdlog::error("[HidMask] black shader: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                if (code) code->Release();
            }
            if (!ok) {
                for (auto*& p : g_ps_black) { if (p) p->Release(); p = nullptr; }
                g_black_failed = true;
                spdlog::error("[HidMask] could not make the black fill's shaders: the hidden area is not drawn black");
            }
            return ok;
        }

        // A typed view format for the eye image (a TYPELESS image needs one; a UAV cannot be sRGB).
        DXGI_FORMAT view_format(DXGI_FORMAT f, bool uav)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return uav ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return uav ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:                                return f;
            }
        }
    }

    void set_mode(int m)
    {
        m = std::clamp(m, 0, 2);
        if (g_mode.exchange(m) != m) spdlog::info("[HidMask] hidmask {}", mode_name(m));
    }
    int mode() { return g_mode.load(); }
    void set_cover(float c)
    {
        c = std::clamp(c, 0.1f, 1.0f);
        if (std::abs(g_cover.exchange(c) - c) > 1e-4f) {
            g_gen++;   // the eye buffers are rebuilt with the new cover
            spdlog::info("[HidMask] cover {:.0f} % of the runtime's hidden area", c * 100.0f);
        }
    }
    float cover() { return g_cover.load(); }
    void set_turn_limit(float dps)
    {
        dps = std::clamp(dps, 0.0f, 1000.0f);
        if (std::abs(g_turn_dps.exchange(dps) - dps) > 0.01f) spdlog::info("[HidMask] off in head turns faster than {:.0f} deg/s{}", dps, dps <= 0.0f ? " (never)" : "");
    }
    float turn_limit() { return g_turn_dps.load(); }
    void head_speed(float dps)
    {
        g_head_dps        = dps;
        const float lim   = g_turn_dps.load();
        const bool  fast  = lim > 0.0f && dps > lim;
        g_turning         = fast;
        if (fast && g_mode.load() != kOff) g_turn_frames++;
    }
    bool turning() { return g_turning.load(); }
    const char* mode_name(int m) { return m == kDepth ? "depth" : m == kRestore ? "restore" : "off"; }

    void fetch(uint64_t instance, uint64_t session, bool has_ext)
    {
        {
            std::lock_guard _{ g_mtx };
            if (g_have && !g_refetch) return;
        }
        if (!has_ext) {
            if (!g_no_ext_logged) {
                g_no_ext_logged = true;
                spdlog::info("[HidMask] the OpenXR runtime does not offer XR_KHR_visibility_mask: no hidden-area mask (hidmask does nothing)");
            }
            return;
        }
        if (instance == 0 || session == 0 || g_tries >= 10) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < g_next_try) return;
        g_next_try = now + std::chrono::seconds(3);
        g_tries++;
        PFN_xrGetVisibilityMaskKHR get = nullptr;
        const XrInstance inst = reinterpret_cast<XrInstance>(instance);
        const XrSession  sess = reinterpret_cast<XrSession>(session);
        if (XR_FAILED(xrGetInstanceProcAddr(inst, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get))) || !get) {
            spdlog::info("[HidMask] xrGetVisibilityMaskKHR not found (try {})", g_tries);
            return;
        }
        std::vector<XrVector2f> tri[2];
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrVisibilityMaskKHR m{ XR_TYPE_VISIBILITY_MASK_KHR };
            XrResult r = get(sess, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
            if (XR_FAILED(r) || m.vertexCountOutput == 0 || m.indexCountOutput < 3) {
                spdlog::info("[HidMask] {} eye: the runtime gives no hidden triangles (result {}, {} corners, {} indices; try {})", eye ? "right" : "left", (int)r,
                             m.vertexCountOutput, m.indexCountOutput, g_tries);
                return;
            }
            std::vector<XrVector2f> verts(m.vertexCountOutput);
            std::vector<uint32_t>   idx(m.indexCountOutput);
            m.vertexCapacityInput = m.vertexCountOutput;
            m.vertices            = verts.data();
            m.indexCapacityInput  = m.indexCountOutput;
            m.indices             = idx.data();
            r = get(sess, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
            if (XR_FAILED(r)) {
                spdlog::info("[HidMask] {} eye: reading the hidden triangles failed (result {})", eye ? "right" : "left", (int)r);
                return;
            }
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                if (idx[i] >= verts.size() || idx[i + 1] >= verts.size() || idx[i + 2] >= verts.size()) continue;
                tri[eye].push_back(verts[idx[i]]);
                tri[eye].push_back(verts[idx[i + 1]]);
                tri[eye].push_back(verts[idx[i + 2]]);
            }
        }
        std::lock_guard _{ g_mtx };
        g_tri[0] = std::move(tri[0]);
        g_tri[1] = std::move(tri[1]);
        g_have   = !g_tri[0].empty() && !g_tri[1].empty();
        g_refetch = false;
        g_gen++;
        spdlog::info("[HidMask] hidden-area mesh from the runtime: left {} triangles, right {} triangles", g_tri[0].size() / 3, g_tri[1].size() / 3);
    }

    bool ready()
    {
        std::lock_guard _{ g_mtx };
        return g_have;
    }

    void refetch()
    {
        std::lock_guard _{ g_mtx };
        g_refetch    = true;
        g_tries      = 0;
        g_next_try   = {};
        spdlog::info("[HidMask] the runtime reports a changed hidden area: reading it again");
    }

    bool wants_fetch()
    {
        std::lock_guard _{ g_mtx };
        return !g_have || g_refetch;
    }

    bool stamp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, int eye, const float tangents[4], float z)
    {
        if (!ctx || !dsv || eye < 0 || eye > 1 || !ready()) return false;
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;   // the game's own context only (not a recording)
        if (!(tangents[1] > tangents[0]) || !(tangents[2] > tangents[3])) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        const bool ok = init(dev) && build(dev, eye, tangents);
        dev->Release();
        if (!ok) { g_fail++; return false; }
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim{};
        if (res) res->GetType(&dim);
        D3D11_TEXTURE2D_DESC td{};
        if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        if (res) res->Release();
        if (td.Width == 0 || td.Height == 0 || td.SampleDesc.Count != 1) { g_fail++; return false; }
        if (!g_vp_logged) {
            // What the game's viewport is at this point (the stamp covers the whole target; if the game renders into a smaller part
            // of it, the mask would be misplaced: this line says).
            g_vp_logged = true;
            D3D11_VIEWPORT vp{};
            UINT           n = 1;
            ctx->RSGetViewports(&n, &vp);
            spdlog::info("[HidMask] first stamp: depth target {}x{} format {}; the game's viewport at the clear {}: {:.0f},{:.0f} {:.0f}x{:.0f}", td.Width, td.Height,
                         (int)td.Format, n ? "set" : "none", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
        }
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) { g_fail++; return false; }
        const EyeVb&  e = g_eye[eye];
        fc5::ownpass::Scope own;   // the hooks pass these calls through untouched
        ID3DDeviceContextState* prev = nullptr;
        ctx1->SwapDeviceContextState(g_state, &prev);
        const UINT    stride = 8, offset = 0;
        ID3D11Buffer* vb     = e.vb;
        ctx->IASetInputLayout(g_layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_vs[z > 0.5f ? 1 : 0], nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(g_rs);
        const D3D11_VIEWPORT vp{ 0.0f, 0.0f, float(td.Width), float(td.Height), 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetDepthStencilState(g_dss, 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        ctx->OMSetRenderTargets(0, nullptr, dsv);
        ctx->Draw(e.count, 0);
        // Leave nothing of the game's bound in the mod's state object.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11Buffer* none = nullptr;
        ctx->IASetVertexBuffers(0, 1, &none, &stride, &offset);
        ctx1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        ctx1->Release();
        g_stamps[eye][z > 0.5f ? 1 : 0]++;
        return true;
    }

    namespace
    {
        // The hidden triangles of eye 0/1 filled in tex: black (an eye image: render target or UAV-only), or AFW's hidden mark (mark: an
        // R32F render target).
        bool fill_area(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4], bool mark);
    }

    bool black(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4]) { return fill_area(ctx, tex, eye, tangents, false); }

    bool mark(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4]) { return fill_area(ctx, tex, eye, tangents, true); }

    namespace
    {
    bool fill_area(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, const float tangents[4], bool mark)
    {
        if (!ctx || !tex || eye < 0 || eye > 1 || !ready()) return false;
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        if (!(tangents[1] > tangents[0]) || !(tangents[2] > tangents[3])) return false;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        const bool rt  = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        const bool uav = !rt && !mark && (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        bool ok = td.Width > 0 && td.Height > 0 && td.SampleDesc.Count == 1 && td.ArraySize == 1 && (rt || uav) && init(dev) && black_init(dev) && build(dev, eye, tangents);
        ID3D11RenderTargetView*    rtv = nullptr;
        ID3D11UnorderedAccessView* uv  = nullptr;
        if (ok) {
            fc5::ownpass::Scope own;
            if (rt) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format        = view_format(td.Format, false);
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ok = SUCCEEDED(dev->CreateRenderTargetView(tex, &rd, &rtv)) && rtv;
            } else {
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format        = view_format(td.Format, true);
                ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                ok = SUCCEEDED(dev->CreateUnorderedAccessView(tex, &ud, &uv)) && uv;
            }
        }
        dev->Release();
        if (!ok) {
            g_black_fail++;
            if (!g_black_logged && !g_failed && !g_black_failed) {
                g_black_logged = true;
                spdlog::info("[HidMask] {}: cannot draw into the {} {}x{} format {} bind {:#x} samples {} layers {} ({})", mark ? "AFW mark" : "black fill",
                             mark ? "AFW depth copy" : "eye image", td.Width, td.Height, (int)td.Format, td.BindFlags, td.SampleDesc.Count, td.ArraySize,
                             mark ? "the rebuild may take the hidden area's texels" : "the hidden area is not drawn black");
            }
            if (rtv) rtv->Release();
            if (uv) uv->Release();
            return false;
        }
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) {
            if (rtv) rtv->Release();
            if (uv) uv->Release();
            g_black_fail++;
            return false;
        }
        const EyeVb& e = g_eye[eye];
        {
            fc5::ownpass::Scope own;   // the hooks pass these calls through untouched
            ID3DDeviceContextState* prev = nullptr;
            ctx1->SwapDeviceContextState(g_state, &prev);
            const UINT    stride = 8, offset = 0;
            ID3D11Buffer* vb     = e.vb;
            ctx->IASetInputLayout(g_layout);
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_vs[0], nullptr, 0);
            ctx->PSSetShader(g_ps_black[mark ? 2 : rt ? 0 : 1], nullptr, 0);
            ctx->RSSetState(g_rs);
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, float(td.Width), float(td.Height), 0.0f, 1.0f };
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetDepthStencilState(nullptr, 0);   // no depth target bound: no depth test
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            if (rt) ctx->OMSetRenderTargets(1, &rtv, nullptr);
            else    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &uv, nullptr);
            ctx->Draw(e.count, 0);
            // Leave nothing of the game's bound in the mod's state object.
            if (rt) ctx->OMSetRenderTargets(0, nullptr, nullptr);
            else    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
            ID3D11Buffer* none = nullptr;
            ctx->IASetVertexBuffers(0, 1, &none, &stride, &offset);
            ctx->PSSetShader(nullptr, nullptr, 0);
            ctx1->SwapDeviceContextState(prev, nullptr);
            if (prev) prev->Release();
        }
        ctx1->Release();
        if (rtv) rtv->Release();
        if (uv) uv->Release();
        (mark ? g_marks : g_black)[eye]++;
        return true;
    }
    }

    std::string status()
    {
        return fmt::format("hidmask {} (cover {:.0f} %): mesh {}; hidden {:.1f} % / {:.1f} % of the left / right image; stamps near {} / {}, far (restore) {} / {}; not drawn {}; "
                           "black fills {} / {}, not drawn {}; AFW depth copies marked {} / {} (afwpereye); off in fast head turns (over {:.0f} deg/s, now {:.0f}) for {} frames",
                           mode_name(g_mode.load()), g_cover.load() * 100.0f, ready() ? "from the runtime" : "none", g_eye[0].hidden * 100.0f, g_eye[1].hidden * 100.0f, g_stamps[0][1].load(),
                           g_stamps[1][1].load(), g_stamps[0][0].load(), g_stamps[1][0].load(), g_fail.load(), g_black[0].load(), g_black[1].load(), g_black_fail.load(),
                           g_marks[0].load(), g_marks[1].load(), g_turn_dps.load(), g_head_dps.load(), g_turn_frames.load());
    }
}
