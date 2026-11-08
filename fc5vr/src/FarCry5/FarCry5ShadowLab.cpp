// Lights and shadows lab (7 Nov 2026; author 16:56): F11 Debug > "Lights and shadows lab (tests)". Brief and background:
// research\2026-11-07_lights_shadows\2026-11-07_shadowlab_brief.md and 2026-11-07_lights_shadows_findings.md (sections 4, 5, 7, 9).
// Every tool here is off by default, not saved, and reachable by command:
//   shadowcap [now|off]  the next F6 / snappair also dumps the sun shadow maps and the resolve's constant buffers (dumps\shadowcap_*)
//   passorder            every draw, dispatch, clear and copy of one frame pair in order (probes\passorder_<time>.txt)
//   shadowvals [s]       the far-shadow, cascade and reflection values once per frame per eye for s seconds (default 5), [ShadowVals]
//   shadowsplit 0|1|2    debug view: the game's sun shadows / near cascades only / far shadows only (the resolve's own buffer copy)
//   pairdiff <cb> [N]    every float of a constant buffer that differs between the left and right call of a pair, N pairs, [PairDiff]
// lowsuncsm (three sun cascades in the evening and at night, lab\lowsun_cascades.md) left the lab on 7 Nov 19:47: on by default,
// in F11 Debug > Sun shadows (FarCry5Stereo.cpp).
#include "FarCry5Stereo.h"
#include "FarCry5LodFov.h"

#include <d3d11_1.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

int fc5_frame_call();     // FarCry5Stereo.cpp: 1 the game's call, 2 the extra one (framedouble)
int fc5_shdump_words();   // shdump: words read from probes\shdump.txt at start (-1 not read yet, 0 no file)
int fc5_shdump_count();   // shaders disassembled into probes\shaders\

namespace
{
    const char* kProbes = "fc5vr_probes";
    const char* kDumps  = "fc5vr_dumps";

    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    struct OwnGuard
    {
        std::atomic<bool>& f;
        explicit OwnGuard(std::atomic<bool>& a) : f(a) { f = true; }
        ~OwnGuard() { f = false; }
    };

    std::string tex_desc(ID3D11Resource* r)
    {
        if (!r) return "null";
        D3D11_RESOURCE_DIMENSION dim{};
        r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            return fmt::format("{}x{}{} f{} m{}", d.Width, d.Height, d.ArraySize > 1 ? fmt::format("x{}", d.ArraySize) : std::string{}, (int)d.Format, d.MipLevels);
        }
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
            D3D11_TEXTURE3D_DESC d{};
            static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
            return fmt::format("{}x{}x{} 3D f{} m{}", d.Width, d.Height, d.Depth, (int)d.Format, d.MipLevels);
        }
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
            return d.StructureByteStride ? fmt::format("buf {} B /{}", d.ByteWidth, d.StructureByteStride) : fmt::format("buf {} B", d.ByteWidth);
        }
        return fmt::format("dim {}", (int)dim);
    }

    std::string view_desc(ID3D11View* v)
    {
        if (!v) return "-";
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        std::string s = fmt::format("{}@{}", tex_desc(r), (void*)r);
        if (r) r->Release();
        return s;
    }

    // Bytes per texel of the formats the shadow maps and their index use (0 = not handled).
    UINT texel_bytes(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: return 8;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 4;
        case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: return 2;
        case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: return 1;
        default: return 0;
        }
    }

    const char* kCapTextures[]{ "ASMLongRangeIndexTexture", "ASMDepthAtlasTexture", "ASMDepthExtentMapAtlasTexture", "CSMShadowMap" };
    const char* kCapBuffers[]{ "CViewportShaderParameterProvider", "CDeferredShadowsResolveParameterProvider", "CTileLightingBlendedParameterProvider",
                               "CAdaptiveShadowMapResolveParameterProvider" };

    // The world box of one cascade from its three CSMSlice rows (uvz = row . (p, 1); the box is uvz in [0,1]^3): the box's width,
    // height and depth (1 / |row|) and its centre (the point where every row gives 0.5).
    struct SliceBox { float w, h, d; glm::vec3 c; bool ok; };
    SliceBox slice_box(const float* s)
    {
        SliceBox b{};
        const glm::vec3 a{ s[0], s[1], s[2] }, bb{ s[4], s[5], s[6] }, c{ s[8], s[9], s[10] };
        b.w = 1.0f / std::max(1e-9f, glm::length(a));
        b.h = 1.0f / std::max(1e-9f, glm::length(bb));
        b.d = 1.0f / std::max(1e-9f, glm::length(c));
        glm::mat3 m{};   // glm: m[col][row]; rows a, bb, c
        for (int i = 0; i < 3; ++i) { m[i][0] = a[i]; m[i][1] = bb[i]; m[i][2] = c[i]; }
        const float det = glm::determinant(m);
        b.ok = std::abs(det) > 1e-18f;
        if (b.ok) b.c = glm::inverse(m) * glm::vec3{ 0.5f - s[3], 0.5f - s[7], 0.5f - s[11] };
        return b;
    }

    std::string f4(const float* v) { return fmt::format("({:.6g} {:.6g} {:.6g} {:.6g})", v[0], v[1], v[2], v[3]); }

    std::string hms()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        return fmt::format("{:02}{:02}{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

bool FarCry5Stereo::lab_is_resolve(ID3D11ComputeShader* cs)
{
    if (!cs) return false;
    std::lock_guard _{ m_ps_mtx };
    if (auto it = m_lab_resolve.find(cs); it != m_lab_resolve.end()) return it->second != 0;
    auto       nt = m_cs_names.find(cs);
    const bool r  = nt != m_cs_names.end() && nt->second.find("ASMDepthAtlasTexture") != std::string::npos && nt->second.find("u:HalfScreenEffectsBuffer") != std::string::npos;
    if (nt != m_cs_names.end()) m_lab_resolve[cs] = r ? 1 : 0;   // only once the shader is known (a pointer reused later gets its new entry below)
    return r;
}

void FarCry5Stereo::lab_update_any()
{
    m_lab_any = m_lab_cap_state.load() > 0 || m_lab_order_state.load() > 0 || m_lab_vals_until.load() > 0 || m_lab_split.load() != 0 || m_lab_pd_left.load() > 0;
}

// One constant buffer of a stage (0 VS, 1 PS, 2 CS) as the GPU sees it now (one short stall). The window bound with offsets.
bool FarCry5Stereo::lab_read_cb(ID3D11DeviceContext* ctx, int stage, UINT slot, std::vector<float>& out, UINT max_bytes)
{
    out.clear();
    if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return false;
    ID3D11Buffer* cb = nullptr;
    UINT          first = 0, num = 0;
    if (stage == 0) ctx1->VSGetConstantBuffers1(slot, 1, &cb, &first, &num);
    else if (stage == 1) ctx1->PSGetConstantBuffers1(slot, 1, &cb, &first, &num);
    else ctx1->CSGetConstantBuffers1(slot, 1, &cb, &first, &num);
    ctx1->Release();
    if (!cb) return false;
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    if (!m_lab_staging) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth      = 4096;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) { dev->CreateBuffer(&sd, nullptr, &m_lab_staging); dev->Release(); }
    }
    const UINT off = first * 16;
    UINT       len = bd.ByteWidth > off ? bd.ByteWidth - off : 0;
    if (num > 0 && num * 16 < len) len = num * 16;
    len = std::min({ len, max_bytes, 4096u }) & ~3u;
    bool ok = false;
    if (m_lab_staging && len > 0) {
        OwnGuard  g{ m_lab_own };
        D3D11_BOX box{ off, 0, 0, off + len, 1, 1 };
        ctx->CopySubresourceRegion(m_lab_staging, 0, 0, 0, 0, cb, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(m_lab_staging, 0, D3D11_MAP_READ, 0, &m))) {
            out.resize(len / 4);
            std::memcpy(out.data(), m.pData, len);
            ctx->Unmap(m_lab_staging, 0);
            ok = true;
        }
    }
    cb->Release();
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Hooks: every Dispatch / DispatchIndirect (just before the original), every draw, clear and copy (passorder), and each F6 picture.
// ---------------------------------------------------------------------------------------------------------------------------------

FarCry5Stereo::LabSaved FarCry5Stereo::lab_dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z, bool indirect)
{
    LabSaved                  s{};
    ID3D11ComputeShader* const cs = m_cur_cs;
    if (!cs || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return s;
    const int frame = m_frame.load(), eye = frame & 1;
    if (m_lab_order_state.load() > 0) lab_order_frame();
    std::vector<std::pair<std::string, UINT>>       cbs;
    std::vector<std::tuple<std::string, int, UINT>> res;
    std::string                                     crc;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_cs_cbs.find(cs); it != m_cs_cbs.end()) cbs = it->second;
        if (auto it = m_cs_res.find(cs); it != m_cs_res.end()) res = it->second;
        if (auto it = m_cs_names.find(cs); it != m_cs_names.end()) crc = it->second.substr(0, 9);
    }
    auto cb_slot = [&](const char* name) -> UINT {
        for (const auto& [n, sl] : cbs) if (n == name) return sl;
        return 255;
    };
    if (m_lab_order_state.load() == 2 && m_lab_order_file) {
        // passorder: the pass, its inputs and outputs by the names the shader gives them.
        std::string line = fmt::format("{} {} call{} {} {}x{}x{} CS {} |", m_lab_order_n++, eye ? "R" : "L", fc5_frame_call(), indirect ? "DispatchIndirect" : "Dispatch", x, y, z, crc);
        std::string t, u;
        for (const auto& [n, type, sl] : res) {
            if (type == 1) {
                ID3D11UnorderedAccessView* v = nullptr;
                ctx->CSGetUnorderedAccessViews(sl, 1, &v);
                u += fmt::format(" {}=u{}:{}", n, sl, view_desc(v));
                if (v) v->Release();
            } else {
                ID3D11ShaderResourceView* v = nullptr;
                ctx->CSGetShaderResources(sl, 1, &v);
                t += fmt::format(" {}=t{}:{}", n, sl, view_desc(v));
                if (v) v->Release();
            }
        }
        line += " t:" + t + " | u:" + u + " | cb:";
        for (const auto& [n, sl] : cbs) line += fmt::format(" {}@b{}", n, sl);
        std::fprintf(static_cast<FILE*>(m_lab_order_file), "%s\n", line.c_str());
    }
    const bool resolve = lab_is_resolve(cs);
    if (resolve) {
        // shadowcap: the first resolve of the left call, then of the right call of the same pair.
        const int st = m_lab_cap_state.load();
        if (st == 1 && eye == 0) {
            m_lab_cap_frame = frame;
            lab_cap_resolve(ctx, cs, 0);
            m_lab_cap_state = 2;
        } else if (st == 2 && eye == 1 && frame == m_lab_cap_frame + 1) {
            lab_cap_resolve(ctx, cs, 1);
            m_lab_cap_state = 0;
            lab_update_any();
        } else if (st == 2 && frame > m_lab_cap_frame + 3) {
            spdlog::info("[ShadowLab] shadowcap: no right-eye resolve after frame {} (AFW or single eye?): only the left eye was saved", m_lab_cap_frame);
            m_lab_cap_state = 0;
            lab_update_any();
        }
    }
    if (const int64_t until = m_lab_vals_until.load(); until > 0) {
        if (now_ms() > until) {
            m_lab_vals_until = 0;
            spdlog::info("[ShadowVals] done ({} lines)", m_lab_vals_lines);
            lab_update_any();
        } else {
            if (resolve && m_lab_vals_frame != frame) {
                m_lab_vals_frame = frame;
                lab_vals_resolve(ctx, cs, eye);
            }
            if (m_lab_vals_ssr_frame != frame) {
                CbPatchCs p{};
                bool      ssr = false;
                {
                    std::lock_guard _{ m_ps_mtx };
                    if (auto it = m_ssr_cut_cs.find(cs); it != m_ssr_cut_cs.end()) { p = it->second; ssr = true; }
                }
                std::vector<float> v;
                if (ssr && lab_read_cb(ctx, 2, p.slot, v, 352) && v.size() >= 88) {
                    m_lab_vals_ssr_frame = frame;
                    m_lab_vals_lines++;
                    spdlog::info("[ShadowVals] f{} {} call{} SSLR {} b{}: c19 {} (SpecularFactorCutoffThreshold c19.z {:.4f}{}), c21.z (cutoff fade range inverse) {:.4f}", frame, eye ? "R" : "L",
                                 fc5_frame_call(), crc, p.slot, f4(&v[76]), v[78], m_ssr_rough.load() > 0.0f ? fmt::format(", ssrrough {:.2f} on", m_ssr_rough.load()) : std::string{}, v[86]);
                }
            }
        }
    }
    if (m_lab_pd_left.load() > 0 && !m_lab_pd_name.empty()) {
        if (const UINT sl = cb_slot(m_lab_pd_name.c_str()); sl != 255) lab_pairdiff(ctx, 2, sl, "CS " + crc);
    }
    // shadowsplit last, so the dumps and logs above read the game's own buffers.
    const int split = m_lab_split.load();
    if (split != 0 && resolve && !m_scope_call) {
        const UINT slot = cb_slot(split == 1 ? "CViewportShaderParameterProvider" : "CTileLightingBlendedParameterProvider");
        if (slot == 255) return s;
        const int k = split == 1 ? 0 : 1;
        if (!m_lab_standin[k] || !m_lab_v3 || !m_lab_v0) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                D3D11_BUFFER_DESC sd{};
                sd.Usage     = D3D11_USAGE_DEFAULT;
                sd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                sd.ByteWidth = 4096;
                if (!m_lab_standin[k]) dev->CreateBuffer(&sd, nullptr, &m_lab_standin[k]);
                sd.ByteWidth = 16;
                const float three[4]{ 3.0f, 3.0f, 3.0f, 3.0f }, zero[4]{};
                D3D11_SUBRESOURCE_DATA i3{ three, 0, 0 }, i0{ zero, 0, 0 };
                if (!m_lab_v3) dev->CreateBuffer(&sd, &i3, &m_lab_v3);
                if (!m_lab_v0) dev->CreateBuffer(&sd, &i0, &m_lab_v0);
                dev->Release();
            }
        }
        if (!m_lab_standin[k] || !m_lab_v3 || !m_lab_v0) return s;
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return s;
        ID3D11Buffer* cb = nullptr;
        UINT          first = 0, num = 0;
        ctx1->CSGetConstantBuffers1(slot, 1, &cb, &first, &num);
        ctx1->Release();
        if (!cb) return s;
        D3D11_BUFFER_DESC bd{};
        cb->GetDesc(&bd);
        const UINT off = first * 16;
        const UINT len = std::min(4096u, bd.ByteWidth > off ? bd.ByteWidth - off : 0u);
        const UINT need = split == 1 ? 864u : 144u;   // ShadowCSMLastSliceIndex at byte 860; the CSMSlice z rows end at byte 144
        if (len < need) { cb->Release(); return s; }
        {
            OwnGuard  g{ m_lab_own };
            D3D11_BOX all{ off, 0, 0, off + len, 1, 1 };
            ctx->CopySubresourceRegion(m_lab_standin[k], 0, 0, 0, 0, cb, 0, &all);
            if (split == 1) {
                D3D11_BOX one{ 0, 0, 0, 4, 1, 1 };
                ctx->CopySubresourceRegion(m_lab_standin[k], 0, 860, 0, 0, m_lab_v3, 0, &one);
            } else {
                D3D11_BOX row{ 0, 0, 0, 16, 1, 1 };
                for (UINT o : { 32u, 80u, 128u }) ctx->CopySubresourceRegion(m_lab_standin[k], 0, o, 0, 0, m_lab_v0, 0, &row);
            }
        }
        ctx->CSSetConstantBuffers(slot, 1, &m_lab_standin[k]);
        s.cb[0]    = cb;
        s.first[0] = first;
        s.num[0]   = num;
        s.slot[0]  = slot;
        s.n        = 1;
        if (m_lab_split_runs++ == 0) spdlog::info("[ShadowLab] shadowsplit {}: first resolve patched ({} b{}, {} B from byte {})", split, crc, slot, len, off);
    }
    return s;
}

void FarCry5Stereo::lab_restore(ID3D11DeviceContext* ctx, LabSaved& s)
{
    ID3D11DeviceContext1* ctx1 = nullptr;
    const bool            have = SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1;
    for (int i = 0; i < s.n; ++i) {
        if (have) ctx1->CSSetConstantBuffers1(s.slot[i], 1, &s.cb[i], &s.first[i], &s.num[i]);
        else ctx->CSSetConstantBuffers(s.slot[i], 1, &s.cb[i]);
        if (s.cb[i]) s.cb[i]->Release();
    }
    if (ctx1) ctx1->Release();
    s = {};
}

void FarCry5Stereo::lab_draw(ID3D11DeviceContext* ctx, const char* kind, UINT a, UINT b)
{
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    const bool order = m_lab_order_state.load() > 0 && lab_order_frame();
    const bool pd    = m_lab_pd_left.load() > 0 && !m_lab_pd_name.empty();
    if (!order && !pd) return;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ID3D11PixelShader* ps = m_cur_ps;
    std::string        vinfo, pnames;
    std::vector<std::pair<std::string, UINT>> pslots;
    {
        std::lock_guard _{ m_ps_mtx };
        if (vs) if (auto it = m_vs_info.find(vs); it != m_vs_info.end()) vinfo = it->second;
        if (ps) {
            if (auto it = m_ps_names.find(ps); it != m_ps_names.end()) pnames = it->second.substr(0, 9);
            if (auto it = m_ps_tex_slots.find(ps); it != m_ps_tex_slots.end()) pslots = it->second;
        }
    }
    if (vs) vs->Release();
    if (order) {
        std::string line = fmt::format("{} {} call{} {} {} {} VS {} PS {} |", m_lab_order_n++, (m_frame.load() & 1) ? "R" : "L", fc5_frame_call(), kind, a, b, vinfo.substr(0, 9), ps ? pnames : std::string{ "-" });
        std::string t, cbn;
        for (const auto& [n, sl] : pslots) {
            if (n.rfind("cb:", 0) == 0) { cbn += fmt::format(" {}@ps{}", n.substr(3), sl); continue; }
            if (sl >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) continue;
            ID3D11ShaderResourceView* v = nullptr;
            ctx->PSGetShaderResources(sl, 1, &v);
            t += fmt::format(" {}=t{}:{}", n, sl, view_desc(v));
            if (v) v->Release();
        }
        ID3D11RenderTargetView* rtv[8]{};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(8, rtv, &dsv);
        std::string rt;
        for (int i = 0; i < 8; ++i) {
            if (!rtv[i]) continue;
            rt += fmt::format(" rt{}={}", i, view_desc(rtv[i]));
            rtv[i]->Release();
        }
        if (dsv) { rt += fmt::format(" ds={}", view_desc(dsv)); dsv->Release(); }
        // The vertex shader's constant buffers: "Name@bN" in its reflection summary.
        for (size_t p = vinfo.find(' '); p != std::string::npos && p + 1 < vinfo.size(); p = vinfo.find(' ', p + 1)) {
            const size_t at = vinfo.find("@b", p + 1), sp = vinfo.find(' ', p + 1);
            if (at != std::string::npos && (sp == std::string::npos || at < sp) && vinfo[p + 1] != '[') cbn += fmt::format(" {}@vs{}", vinfo.substr(p + 1, at - p - 1), vinfo.substr(at + 2, sp == std::string::npos ? std::string::npos : sp - at - 2));
        }
        line += " t:" + t + " |" + rt + " | cb:" + cbn;
        std::fprintf(static_cast<FILE*>(m_lab_order_file), "%s\n", line.c_str());
    }
    if (pd) {
        for (const auto& [n, sl] : pslots) {
            if (n.size() == m_lab_pd_name.size() + 3 && n.rfind("cb:", 0) == 0 && n.compare(3, std::string::npos, m_lab_pd_name) == 0) {
                lab_pairdiff(ctx, 1, sl, "PS " + pnames);
                return;
            }
        }
        const size_t at = vinfo.find(" " + m_lab_pd_name + "@b");
        if (at != std::string::npos) lab_pairdiff(ctx, 0, (UINT)std::atoi(vinfo.c_str() + at + m_lab_pd_name.size() + 3), "VS " + vinfo.substr(0, 9));
    }
}

void FarCry5Stereo::lab_note(ID3D11DeviceContext* ctx, const char* kind, ID3D11View* view, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (m_lab_own.load() || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || m_lab_order_state.load() <= 0 || !lab_order_frame()) return;
    std::string d = view ? view_desc(view) : fmt::format("{}@{}", tex_desc(dst), (void*)dst);
    std::string line = fmt::format("{} {} call{} {} dst={}", m_lab_order_n++, (m_frame.load() & 1) ? "R" : "L", fc5_frame_call(), kind, d);
    if (src) line += fmt::format(" src={}@{}", tex_desc(src), (void*)src);
    std::fprintf(static_cast<FILE*>(m_lab_order_file), "%s\n", line.c_str());
}

// passorder: opens the file at the first event of a left call, closes it after the right call. True while the pair is logged.
bool FarCry5Stereo::lab_order_frame()
{
    const int frame = m_frame.load();
    if (m_lab_order_state.load() == 1 && (frame & 1) == 0) {
        std::error_code ec;
        std::filesystem::create_directories(kProbes, ec);
        m_lab_order_last = fmt::format("{}/passorder_{}.txt", kProbes, hms());
        FILE* f          = nullptr;
        fopen_s(&f, m_lab_order_last.c_str(), "w");
        if (!f) {
            spdlog::info("[ShadowLab] passorder: cannot write {}", m_lab_order_last);
            m_lab_order_state = 0;
            lab_update_any();
            return false;
        }
        std::fprintf(f, "# passorder: frames %d (left call) and %d (right call); one line per Draw*, Dispatch*, Clear*, Copy* on the render thread.\n"
                        "# index eye call kind args shaders | t: inputs (name=slot:WxH[xArray] fFormat mMips@resource) | u: / rt ds: outputs | cb: constant buffers\n",
                     frame, frame + 1);
        m_lab_order_file  = f;
        m_lab_order_frame = frame;
        m_lab_order_n     = 0;
        m_lab_order_state = 2;
        spdlog::info("[ShadowLab] passorder: logging frames {} and {} to {}", frame, frame + 1, m_lab_order_last);
    }
    if (m_lab_order_state.load() != 2) return false;
    if (frame > m_lab_order_frame + 1 || frame < m_lab_order_frame) {
        if (m_lab_order_file) std::fclose(static_cast<FILE*>(m_lab_order_file));
        m_lab_order_file  = nullptr;
        m_lab_order_state = 0;
        spdlog::info("[ShadowLab] passorder: {} lines in {}", m_lab_order_n, m_lab_order_last);
        lab_update_any();
        return false;
    }
    return true;
}

void FarCry5Stereo::lab_snapshot()
{
    const bool once = m_lab_cap_armed.exchange(false);
    if (!once && !m_lab_cap_always.load()) return;
    // shadowcapall (8 Nov, the dawn squares come and go within 2-3 s, so the dump must come with the F6 that caught one): about
    // 130 MB per press, so stop before D: nears the 20 GB floor (CLAUDE.md rule 6).
    std::error_code ec;
    const auto      sp = std::filesystem::space(kDumps, ec);
    if (!ec && sp.available < (25ull << 30)) {
        m_lab_cap_always = false;
        spdlog::warn("[ShadowLab] shadowcap: not saved, only {:.1f} GB free on the dumps drive (25 GB needed); shadowcapall turned off",
                     sp.available / 1073741824.0);
        return;
    }
    m_lab_cap_shot  = m_presents;
    m_lab_cap_state = 1;
    lab_update_any();
    spdlog::info("[ShadowLab] shadowcap{}: F6 / snappair at present {}: the next pair's first sun shadow resolve of each eye is saved to "
                 "dumps\\shadowcap_<frame>_<eye>", once ? "" : "all", m_lab_cap_shot);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// shadowcap: the far-shadow index, atlas and extent map, the three cascades, and the resolve's four constant buffers, for one eye.
// ---------------------------------------------------------------------------------------------------------------------------------

void FarCry5Stereo::lab_cap_resolve(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, int eye)
{
    const int   frame = m_frame.load();
    const auto  t0    = std::chrono::steady_clock::now();
    std::string crc;
    std::vector<std::pair<std::string, UINT>>       cbs;
    std::vector<std::tuple<std::string, int, UINT>> res;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_cs_names.find(cs); it != m_cs_names.end()) crc = it->second.substr(0, 9);
        if (auto it = m_cs_cbs.find(cs); it != m_cs_cbs.end()) cbs = it->second;
        if (auto it = m_cs_res.find(cs); it != m_cs_res.end()) res = it->second;
    }
    const std::string dir = fmt::format("{}/shadowcap_{}_{}", kDumps, frame, eye ? "R" : "L");
    std::error_code   ec;
    std::filesystem::create_directories(dir, ec);
    std::string readme = fmt::format("shadowcap, frame {} ({} eye, call {}), first sun shadow resolve {} of the call, {} local time.\n"
                                     "Settings now: shadowsplit {}, ssrrough {:.2f}, Stereo {}, framedouble {}.\n\n",
                                     frame, eye ? "right" : "left", fc5_frame_call(), crc, hms(), m_lab_split.load(), m_ssr_rough.load(), m_stereo2.load(), m_frame_double.load());
    if (m_lab_cap_shot) readme += fmt::format("Started by the F6 / snappair at present {}: its picture is probes\\snapshot_{}.bmp (taken just before this pair).\n\n",
                                              m_lab_cap_shot, m_lab_cap_shot);
    int files = 0;
    // Textures: <name>_s<slice>_m<mip>.raw = int32 'FC5R', width, height, DXGI format, bytes per texel, then the rows (tools\stereo\shadowcap_view.py).
    for (const char* want : kCapTextures) {
        bool found = false;
        for (const auto& [n, type, sl] : res) {
            if (type != 0 || n != want) continue;
            found = true;
            ID3D11ShaderResourceView* v = nullptr;
            ctx->CSGetShaderResources(sl, 1, &v);
            if (!v) { readme += fmt::format("{} (t{}): nothing bound\n", n, sl); break; }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            v->GetDesc(&vd);
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            v->Release();
            ID3D11Texture2D* t = nullptr;
            if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) {
                readme += fmt::format("{} (t{}): not a 2D texture ({})\n", n, sl, tex_desc(r));
                if (r) r->Release();
                break;
            }
            r->Release();
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            const UINT bpt = texel_bytes(d.Format);
            readme += fmt::format("{} (t{}): {}x{}, format {} (view format {}, view dimension {}), {} mips, {} slices, {} B per texel\n", n, sl, d.Width, d.Height, (int)d.Format,
                                  (int)vd.Format, (int)vd.ViewDimension, d.MipLevels, d.ArraySize, bpt);
            spdlog::info("[ShadowLab] shadowcap {} {}: {}x{} format {} (view {}), {} mips, {} slices", eye ? "R" : "L", n, d.Width, d.Height, (int)d.Format, (int)vd.Format, d.MipLevels, d.ArraySize);
            if (bpt == 0) { readme += "  format not handled: not saved\n"; t->Release(); break; }
            const UINT mips = std::min<UINT>(d.MipLevels, std::string_view{ want } == "ASMLongRangeIndexTexture" ? 3u : 1u);   // the index is read at mip 1
            for (UINT a = 0; a < std::min<UINT>(d.ArraySize, 8); ++a) {
                for (UINT m = 0; m < mips; ++m) {
                    const UINT          w = std::max(1u, d.Width >> m), h = std::max(1u, d.Height >> m);
                    D3D11_TEXTURE2D_DESC sd{};
                    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format; sd.SampleDesc = { 1, 0 };
                    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Device* dev = nullptr;
                    ctx->GetDevice(&dev);
                    ID3D11Texture2D* st = nullptr;
                    if (dev) { dev->CreateTexture2D(&sd, nullptr, &st); dev->Release(); }
                    if (!st) { readme += fmt::format("  slice {} mip {}: staging failed\n", a, m); continue; }
                    {
                        OwnGuard g{ m_lab_own };
                        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, D3D11CalcSubresource(m, a, d.MipLevels), nullptr);
                    }
                    D3D11_MAPPED_SUBRESOURCE mp{};
                    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) {
                        const std::string path = fmt::format("{}/{}_s{}_m{}.raw", dir, n, a, m);
                        FILE*             f    = nullptr;
                        fopen_s(&f, path.c_str(), "wb");
                        if (f) {
                            const int32_t hdr[5]{ 0x52354346, (int32_t)w, (int32_t)h, (int32_t)d.Format, (int32_t)bpt };
                            std::fwrite(hdr, 4, 5, f);
                            for (UINT y = 0; y < h; ++y) std::fwrite(static_cast<const uint8_t*>(mp.pData) + size_t(y) * mp.RowPitch, 1, size_t(w) * bpt, f);
                            std::fclose(f);
                            files++;
                        }
                        ctx->Unmap(st, 0);
                    }
                    st->Release();
                }
            }
            t->Release();
            break;
        }
        if (!found) readme += fmt::format("{}: not bound by this resolve\n", want);
    }
    // Constant buffers as text, every float with its register (and its bits, for the integers).
    float cam[3]{};
    readme += "\n";
    for (const char* want : kCapBuffers) {
        UINT slot = 255;
        for (const auto& [n, sl] : cbs) if (n == want) slot = sl;
        std::vector<float> v;
        if (slot == 255 || !lab_read_cb(ctx, 2, slot, v)) { readme += fmt::format("{}: not read\n", want); continue; }
        if (std::string_view{ want } == "CViewportShaderParameterProvider" && v.size() >= 176) { cam[0] = v[172]; cam[1] = v[173]; cam[2] = v[174]; }
        const std::string path = fmt::format("{}/{}.txt", dir, want);
        FILE*             f    = nullptr;
        fopen_s(&f, path.c_str(), "w");
        if (!f) continue;
        std::fprintf(f, "%s at b%u, %zu bytes as bound, frame %d, %s eye\n", want, slot, v.size() * 4, frame, eye ? "right" : "left");
        for (size_t r = 0; r + 3 < v.size(); r += 4) {
            uint32_t u[4];
            std::memcpy(u, &v[r], 16);
            std::fprintf(f, "c%-3zu % .9g % .9g % .9g % .9g   (%08X %08X %08X %08X)\n", r / 4, v[r], v[r + 1], v[r + 2], v[r + 3], u[0], u[1], u[2], u[3]);
        }
        std::fclose(f);
        files++;
        readme += fmt::format("{} (b{}): {} B -> {}.txt\n", want, slot, v.size() * 4, want);
    }
    readme = fmt::format("Camera position (the eye's, CViewportShaderParameterProvider c43): {:.3f} {:.3f} {:.3f}\n", cam[0], cam[1], cam[2]) + readme;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    readme += fmt::format("\n{} files, {:.0f} ms. PNGs: py -3 tools\\stereo\\shadowcap_view.py \"{}\"\n", files, ms, dir);
    if (FILE* f = nullptr; fopen_s(&f, (dir + "/README.txt").c_str(), "w") == 0 && f) { std::fputs(readme.c_str(), f); std::fclose(f); }
    m_lab_cap_last = dir;
    spdlog::info("[ShadowLab] shadowcap {}: {} files in {} ({:.0f} ms), camera ({:.2f} {:.2f} {:.2f})", eye ? "R" : "L", files, dir, ms, cam[0], cam[1], cam[2]);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// shadowvals: the values that decide near cascades against far shadows, once per frame per eye.
// ---------------------------------------------------------------------------------------------------------------------------------

void FarCry5Stereo::lab_vals_resolve(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, int eye)
{
    std::vector<std::pair<std::string, UINT>> cbs;
    std::string                               crc;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_cs_cbs.find(cs); it != m_cs_cbs.end()) cbs = it->second;
        if (auto it = m_cs_names.find(cs); it != m_cs_names.end()) crc = it->second.substr(0, 9);
    }
    auto slot = [&](const char* name) -> UINT {
        for (const auto& [n, sl] : cbs) if (n == name) return sl;
        return 255;
    };
    std::vector<float> vp, dsr, tlb, as;
    const bool         ok_vp  = lab_read_cb(ctx, 2, slot("CViewportShaderParameterProvider"), vp, 880) && vp.size() >= 216;
    const bool         ok_dsr = lab_read_cb(ctx, 2, slot("CDeferredShadowsResolveParameterProvider"), dsr, 304) && dsr.size() >= 76;
    const bool         ok_tlb = lab_read_cb(ctx, 2, slot("CTileLightingBlendedParameterProvider"), tlb, 176) && tlb.size() >= 44;
    const bool         ok_as  = lab_read_cb(ctx, 2, slot("CAdaptiveShadowMapResolveParameterProvider"), as, 224) && as.size() >= 56;
    const int          frame  = m_frame.load();
    const glm::vec3    cam    = ok_vp ? glm::vec3{ vp[172], vp[173], vp[174] } : glm::vec3{};
    std::string        line   = fmt::format("[ShadowVals] f{} {} call{} {} cam ({:.2f} {:.2f} {:.2f})", frame, eye ? "R" : "L", fc5_frame_call(), crc, cam.x, cam.y, cam.z);
    if (ok_vp) line += fmt::format(" | ShadowCSMLastSliceIndex {:.3g}", vp[215]);
    if (ok_dsr) line += fmt::format(" | CSMFadeFactors ({:.4g} {:.4g} {:.4g})", dsr[72], dsr[73], dsr[74]);
    if (ok_tlb) {
        line += fmt::format(" | CSMSliceFade {} ShadowMapSize {}", f4(&tlb[36]), f4(&tlb[40]));
        for (int k = 0; k < 3; ++k) {
            const SliceBox b = slice_box(&tlb[k * 12]);
            const glm::vec3 o = b.c - cam;
            line += fmt::format(" | slice{} {:.1f} x {:.1f} x {:.1f} m, centre {}", k, b.w, b.h, b.d,
                                b.ok ? fmt::format("({:.1f} {:.1f} {:.1f}) = cam + ({:.1f} {:.1f} {:.1f}), {:.1f} m away", b.c.x, b.c.y, b.c.z, o.x, o.y, o.z, glm::length(glm::vec2{ o.x, o.y })) : std::string{ "?" });
        }
    }
    if (ok_as) {
        line += fmt::format(" | ASM c0 {} c1 {} c2 {} c8 {} c9 {} c10 {} c11 {}", f4(&as[0]), f4(&as[4]), f4(&as[8]), f4(&as[32]), f4(&as[36]), f4(&as[40]), f4(&as[44]));
        line += fmt::format(" (receiver warp ({:.4g} {:.4g} {:.4g}), default factor {:.3g})", as[32], as[33], as[34], as[35]);
    }
    if (m_lab_split.load() != 0) line += fmt::format(" | shadowsplit {} on", m_lab_split.load());
    m_lab_vals_lines++;
    spdlog::info("{}", line);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// pairdiff: the named buffer as the first pass of each call that reads it sees it, left against right.
// ---------------------------------------------------------------------------------------------------------------------------------

void FarCry5Stereo::lab_pairdiff(ID3D11DeviceContext* ctx, int stage, UINT slot, const std::string& where)
{
    const int frame = m_frame.load(), eye = frame & 1;
    if (eye == 0) {
        if (m_lab_pd_frame[0] == frame) return;
        if (lab_read_cb(ctx, stage, slot, m_lab_pd_vals[0])) { m_lab_pd_frame[0] = frame; m_lab_pd_where[0] = where; }
        return;
    }
    if (m_lab_pd_frame[1] == frame || m_lab_pd_frame[0] != frame - 1) return;
    if (!lab_read_cb(ctx, stage, slot, m_lab_pd_vals[1])) return;
    m_lab_pd_frame[1] = frame;
    m_lab_pd_where[1] = where;
    // Labels: the variable that starts at or before each register (from any compute shader's reflection of this buffer).
    std::map<int, std::string> vars;
    {
        std::lock_guard _{ m_ps_mtx };
        const std::string tail = " " + m_lab_pd_name;
        for (const auto& [k, v] : m_cb_vars) {
            if (k.size() < tail.size() || k.compare(k.size() - tail.size(), tail.size(), tail) != 0) continue;
            std::istringstream is{ v };
            for (std::string w; is >> w;) {
                const size_t at = w.rfind("@c");
                if (at == std::string::npos) continue;
                const int r = std::atoi(w.c_str() + at + 2), c = w.find('.', at) != std::string::npos ? std::atoi(w.c_str() + w.find('.', at) + 1) : 0;
                vars[r * 4 + c] = w.substr(w[0] == '*' ? 1 : 0, at - (w[0] == '*' ? 1 : 0));
            }
            break;
        }
    }
    const auto&  l = m_lab_pd_vals[0];
    const auto&  r = m_lab_pd_vals[1];
    const size_t n = std::min(l.size(), r.size());
    std::string  list;
    int          diff = 0;
    for (size_t i = 0; i < n; ++i) {
        if (std::memcmp(&l[i], &r[i], 4) == 0) continue;
        diff++;
        if (diff > 48) continue;
        std::string label;
        if (auto it = vars.upper_bound((int)i); it != vars.begin()) label = std::prev(it)->second;
        list += fmt::format(" c{}.{}{} L {:.7g} R {:.7g} (R-L {:.4g});", i / 4, "xyzw"[i % 4], label.empty() ? std::string{} : " " + label, l[i], r[i], r[i] - l[i]);
    }
    const int left = m_lab_pd_left.load() - 1;
    m_lab_pd_left  = std::max(0, left);
    spdlog::info("[PairDiff] {} frames {}/{} ({} / {}): {} of {} floats differ{}{}", m_lab_pd_name, frame - 1, frame, m_lab_pd_where[0], m_lab_pd_where[1], diff, n,
                 diff ? ":" : "", diff > 48 ? list + " ..." : list);
    if (left <= 0) {
        spdlog::info("[PairDiff] {} done", m_lab_pd_name);
        lab_update_any();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Commands and the F11 section.
// ---------------------------------------------------------------------------------------------------------------------------------

bool FarCry5Stereo::lab_command(const std::string& cmd, const std::string& arg, const std::string& line)
{
    if (cmd == "shadowcap") {
        if (arg == "off") { m_lab_cap_armed = false; m_lab_cap_state = 0; }
        else if (arg == "now") { m_lab_cap_shot = 0; m_lab_cap_state = 1; }
        else m_lab_cap_armed = true;
        spdlog::info("[FC5 cmd] {}: {}", line, arg == "off" ? "off" : arg == "now" ? "the next pair's first sun shadow resolves are saved" : "the next F6 / snappair also saves the sun shadow maps");
    } else if (cmd == "shadowcapall") {
        m_lab_cap_always = !(arg == "off" || arg == "0");
        spdlog::info("[FC5 cmd] {}: {}", line, m_lab_cap_always.load() ? "every F6 / snappair also saves the sun shadow maps (about 130 MB each)" : "off");
    } else if (cmd == "passorder") {
        if (m_lab_order_state.load() == 0) m_lab_order_state = 1;
        spdlog::info("[FC5 cmd] {}: the next frame pair's passes in order go to probes\\passorder_<time>.txt", line);
    } else if (cmd == "shadowvals") {
        const float s = arg == "off" ? 0.0f : arg.empty() ? 5.0f : std::clamp((float)std::atof(arg.c_str()), 0.0f, 60.0f);
        m_lab_vals_until = s > 0.0f ? now_ms() + (int64_t)(s * 1000.0f) : 0;
        m_lab_vals_frame = m_lab_vals_ssr_frame = -1;
        m_lab_vals_lines = 0;
        spdlog::info("[FC5 cmd] {}: far-shadow, cascade and reflection values for {:.0f} s ([ShadowVals])", line, s);
    } else if (cmd == "shadowsplit") {
        const int v     = arg == "off" || arg.empty() ? 0 : std::clamp(std::atoi(arg.c_str()), 0, 2);
        m_lab_split     = v;
        m_lab_split_runs = 0;
        spdlog::info("[FC5 cmd] {}: sun shadows shown: {}", line, v == 0 ? "the game's" : v == 1 ? "near cascades only (no far-shadow blend)" : "far shadows only (no cascade box holds a point)");
    } else if (cmd == "pairdiff") {
        std::istringstream ls{ line };
        std::string        c, name;
        int                n = 10;
        ls >> c >> name >> n;
        if (name.empty() || name == "off") { m_lab_pd_left = 0; spdlog::info("[FC5 cmd] {}: off", line); }
        else {
            m_lab_pd_name  = name;
            m_lab_pd_frame[0] = m_lab_pd_frame[1] = -1;
            m_lab_pd_left  = std::clamp(n, 1, 1000);
            spdlog::info("[FC5 cmd] {}: {} pairs of {}, left call against right call ([PairDiff])", line, m_lab_pd_left.load(), name);
        }
    } else {
        return false;
    }
    lab_update_any();
    return true;
}

void FarCry5Stereo::lab_menu(void (*hint)(const char*), void (*section)(const char*))
{
    if (!ImGui::CollapsingHeader("Lights and shadows lab (tests)")) return;
    ImGui::TextWrapped("Tools to find out what causes the shadow squares, the bar on the water, the roof sheen and the leaf shadows (7 Nov). "
                       "The tools are not saved; each is off until used. They log or save files, except the debug view. The fix being tested is saved.");
    section("Captures");
    {
        bool armed = m_lab_cap_armed.load();
        if (ImGui::Checkbox("F6 also saves the shadow maps", &armed)) m_ui_cmds += armed ? "shadowcap\n" : "shadowcap off\n";
        hint("The next F6 (or snappair) also saves, for each eye, the far-shadow atlas, its index and extent map, the three near cascades and "
             "the shadow pass's four constant buffers into dumps\\shadowcap_<frame>_<eye> (about 130 MB; one short stall). Turns itself off "
             "after one capture. tools\\stereo\\shadowcap_view.py makes pictures of them. Command: shadowcap [now|off].");
        bool always = m_lab_cap_always.load();
        if (ImGui::Checkbox("Every F6 saves the shadow maps", &always)) m_ui_cmds += always ? "shadowcapall on\n" : "shadowcapall off\n";
        hint("The same save as above with every F6 (or snappair), until you untick it, so the F6 that catches a black square brings its "
             "shadow maps with it. About 130 MB per F6 in dumps\\, and a short stall each time; the README in each folder names the F6 "
             "picture it goes with. Turns itself off when less than 25 GB is free. Not saved. Command: shadowcapall on|off.");
        if (m_lab_cap_state.load() > 0) ImGui::TextDisabled("waiting for the next shadow pass...");
        else if (!m_lab_cap_last.empty()) ImGui::TextDisabled("last: %s", m_lab_cap_last.c_str());
    }
    if (ImGui::Button("Log the order of every pass for one frame pair")) m_ui_cmds += "passorder\n";
    hint("Every draw, compute pass, clear and copy of the next left and right call, in order, with its shaders, inputs, outputs and "
         "constant buffers, to probes\\passorder_<time>.txt (a few MB). Command: passorder.");
    if (m_lab_order_state.load() > 0) ImGui::TextDisabled("logging...");
    else if (!m_lab_order_last.empty()) ImGui::TextDisabled("last: %s", m_lab_order_last.c_str());
    if (ImGui::Button("Log the far-shadow and reflection values for 5 s")) m_ui_cmds += "shadowvals\n";
    hint("Once per frame and eye, as [ShadowVals] in the log: the far-shadow index matrix and its factors, the three near cascade boxes "
         "in metres (size and centre), the last cascade index, the cascade fade, and the reflections' roughness cut-off. Costs some "
         "frame rate while it runs. Command: shadowvals [seconds].");
    section("What you see (debug view, live)");
    {
        int sp = m_lab_split.load();
        ImGui::TextUnformatted("Sun shadows shown:");
        ImGui::SameLine();
        bool ch = ImGui::RadioButton("The game's", &sp, 0);
        ImGui::SameLine();
        ch = ImGui::RadioButton("Near cascades only", &sp, 1) || ch;
        ImGui::SameLine();
        ch = ImGui::RadioButton("Far shadows only", &sp, 2) || ch;
        if (ch) m_ui_cmds += fmt::format("shadowsplit {}\n", sp);
        hint("Near cascades only: the sharp shadows never hand over to the far shadows, so beyond the last cascade the ground reads lit. "
             "Far shadows only: no near cascade is used; within 10 m the ground then reads lit, beyond it the far shadows alone show. "
             "Only the main shadow pass changes (water, glass and particles keep the game's). Not saved. Command: shadowsplit 0|1|2.");
    }
    section("Left eye against right eye");
    {
        static const char* kPd[]{ "CGlobalShaderParameterProvider", "CVegetationSimGlobalParam_CS", "CGrassForceParameterProvider" };
        static const char* kLabel[]{ "Compare wind and time (10 pairs)", "Compare vegetation sway (10 pairs)", "Compare grass push (10 pairs)" };
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(kLabel[i])) m_ui_cmds += fmt::format("pairdiff {} 10\n", kPd[i]);
        }
        hint("Logs as [PairDiff] every value of that buffer that differs between the left and the right eye's call of the same frame, "
             "as the first pass of each call reads it. Different wind or time values mean the leaves sway differently in each eye. "
             "Command: pairdiff <buffer name> [pairs].");
        if (m_lab_pd_left.load() > 0) ImGui::TextDisabled("comparing %s, %d pairs left", m_lab_pd_name.c_str(), m_lab_pd_left.load());
    }
    section("Shader dumps");
    {
        const int w = fc5_shdump_words();
        if (w < 0) ImGui::TextDisabled("probes\\shdump.txt: not checked yet (no shader created since the start)");
        else if (w == 0) ImGui::TextDisabled("probes\\shdump.txt: not found at start (no shader dumps)");
        else ImGui::TextDisabled("probes\\shdump.txt: %d words; %d shaders dumped to probes\\shaders", w, fc5_shdump_count());
        hint("A file probes\\shdump.txt present at the game start (one word per line: a shader CRC like #2FFF7035, or a bound resource "
             "name) writes the disassembly of every matching shader to probes\\shaders\\, which the probes clean-up leaves alone.");
    }
}
