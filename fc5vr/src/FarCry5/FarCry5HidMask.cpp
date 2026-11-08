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
        std::atomic<int> g_mode{ kOff };

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
                for (const auto& p : t) {
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
            spdlog::info("[HidMask] {} eye: {} hidden triangles, {:.1f} % of the eye image (tangents L {:.3f} R {:.3f} U {:.3f} D {:.3f})", eye ? "right" : "left", e.count / 3,
                         e.hidden * 100.0f, tan[0], tan[1], tan[2], tan[3]);
            return true;
        }
    }

    void set_mode(int m)
    {
        m = std::clamp(m, 0, 2);
        if (g_mode.exchange(m) != m) spdlog::info("[HidMask] hidmask {}", mode_name(m));
    }
    int mode() { return g_mode.load(); }
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

    std::string status()
    {
        return fmt::format("hidmask {}: mesh {}; hidden {:.1f} % / {:.1f} % of the left / right image; stamps near {} / {}, far (restore) {} / {}; not drawn {}",
                           mode_name(g_mode.load()), ready() ? "from the runtime" : "none", g_eye[0].hidden * 100.0f, g_eye[1].hidden * 100.0f, g_stamps[0][1].load(),
                           g_stamps[1][1].load(), g_stamps[0][0].load(), g_stamps[1][0].load(), g_fail.load());
    }
}
