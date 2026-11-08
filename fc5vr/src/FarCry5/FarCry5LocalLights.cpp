#include "FarCry5LocalLights.h"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace fc5::locallights
{
    namespace
    {
        // The game's layouts (disassembly of the tiled lighting #02297990 and its variants):
        // LightingData (144 B): posCS.xyz rcpRadius2 | color attenuation | direction flags | transform | spotFactors unused
        //   iesProfileIndex | clipPlanes[4] (n.xyz, w: inside where dot(n, p) - w > 0). flags: 1 spot (projective shadow),
        //   4 or 8 shadowed, 16 clip planes, >> 9 the ShadowData index.
        // ShadowProjTextureData (96 B): params0..3 the projection's columns for x, y, z, 1 of a camera-relative position
        //   (spot), or params0.xyz the light's camera-relative position (point, cube shadow); params4, projectorScaleOffset.
        const char* kHlsl = R"(
struct L144 { float4 a[9]; };
struct S96  { float4 p[6]; };
StructuredBuffer<L144>   InL : register(t0);
StructuredBuffer<S96>    InS : register(t1);
RWStructuredBuffer<L144> OutL : register(u0);
RWStructuredBuffer<S96>  OutS : register(u1);
cbuffer P : register(b0) { float3 D; uint CountL; uint CountS; uint HasS; uint2 Pad; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= CountL) return;
    L144 l = InL[i];
    const uint flags = asuint(l.a[2].w);
    // The pixel this eye places at p_eye is the game's p_eye + D: every camera-relative value goes the other way.
    l.a[0].xyz -= D;
    [unroll] for (uint k = 5; k < 9; ++k) l.a[k].w -= dot(l.a[k].xyz, D);
    OutL[i] = l;
    if (HasS != 0 && (flags & 12u) != 0u) {
        const uint si = flags >> 9;
        if (si < CountS) {
            S96 s = InS[si];
            if ((flags & 1u) != 0u) s.p[3] += D.x * s.p[0] + D.y * s.p[1] + D.z * s.p[2];
            else s.p[0].xyz -= D;
            OutS[si] = s;
        }
    }
}
)";

        struct Copy
        {
            ID3D11Buffer*              buf{ nullptr };
            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            UINT                       bytes{ 0 }, stride{ 0 };
        };
        // One moved copy per (lamp buffer, shadow buffer) pair the eye call's dispatches read: the culling passes, the
        // lighting variants without ShadowData and those with it, the bounce pass's LightsLightingDataOut. With a single
        // copy (before 2 Nov) the variants with and without ShadowData took turns and rebuilt it about 15 times per eye call.
        struct Entry
        {
            Copy            l, s;
            ID3D11Resource* src_l{ nullptr };
            ID3D11Resource* src_s{ nullptr };   // null: built without a shadow buffer
            int64_t         key{ -1 };
            uint64_t        gen{ 0 };
            uint64_t        used{ 0 };
        };
        constexpr int        kEntries = 6;
        ID3D11ComputeShader* g_cs = nullptr;
        ID3D11Buffer*        g_cb = nullptr;
        bool                 g_failed = false;
        Entry                g_e[kEntries];
        uint64_t             g_tick = 0;
        // The game's lamp buffers the copies were made from: a write to one of them (Unmap after a Map, UpdateSubresource)
        // or a pass that writes lamp data makes every copy stale (g_gen), so a lamp list uploaded again within the same
        // eye call is never read from the copy of the old list.
        std::atomic<ID3D11Resource*> g_watch[kEntries * 2]{};
        std::atomic<uint64_t> g_gen{ 1 };
        std::atomic<bool>    g_on{ true };
        std::atomic<uint64_t> g_swaps{ 0 }, g_builds{ 0 }, g_refused{ 0 }, g_drops{ 0 };

        bool make_copy(ID3D11Device* dev, Copy& c, UINT bytes, UINT stride)
        {
            if (c.buf && c.bytes == bytes && c.stride == stride) return true;
            if (c.srv) { c.srv->Release(); c.srv = nullptr; }
            if (c.uav) { c.uav->Release(); c.uav = nullptr; }
            if (c.buf) { c.buf->Release(); c.buf = nullptr; }
            D3D11_BUFFER_DESC d{ bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride };
            if (FAILED(dev->CreateBuffer(&d, nullptr, &c.buf)) || !c.buf) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = bytes / stride;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = bytes / stride;
            dev->CreateShaderResourceView(c.buf, &sv, &c.srv);
            dev->CreateUnorderedAccessView(c.buf, &uv, &c.uav);
            c.bytes = bytes; c.stride = stride;
            return c.srv && c.uav;
        }

        bool setup(ID3D11Device* dev)
        {
            if (g_cs) return true;
            if (g_failed) return false;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            auto      fn   = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!fn || FAILED(fn(kHlsl, strlen(kHlsl), "fc5_lampeye", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[LampEye] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no compiler");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC cd{ 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            dev->CreateBuffer(&cd, nullptr, &g_cb);
            if (!g_cs || !g_cb) { g_failed = true; return false; }
            spdlog::info("[LampEye] ready");
            return true;
        }

        // The structured buffer behind an SRV and its layout (0 when it is not a structured buffer of that stride).
        ID3D11Buffer* source(ID3D11ShaderResourceView* v, UINT stride, UINT& bytes)
        {
            bytes = 0;
            if (!v) return nullptr;
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            if (!r) return nullptr;
            D3D11_RESOURCE_DIMENSION dim{};
            r->GetType(&dim);
            if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) { r->Release(); return nullptr; }
            auto*             b = static_cast<ID3D11Buffer*>(r);
            D3D11_BUFFER_DESC d{};
            b->GetDesc(&d);
            if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || d.StructureByteStride != stride) { r->Release(); return nullptr; }
            bytes = d.ByteWidth;
            return b;   // a reference: the caller releases it
        }
    }

    namespace
    {
        // lampdump: the game's lamp list and lamp shadow list as this dispatch reads them, once, into the log (CPU read-back
        // through staging copies; the one Map waits for the GPU). Every lamp with light or a shadow: its index, posCS,
        // colour, flags decoded and ShadowData index; every ShadowData entry a lamp points at, and the 7 after it, so a
        // point lamp's cube faces (if the shader reads si + face) show up as the entries that follow it.
        std::atomic<int> g_dump{ 0 };

        bool read_back(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT bytes, std::vector<float>& out)
        {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return false;
            D3D11_BUFFER_DESC d{ bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
            ID3D11Buffer*     st = nullptr;
            const bool        made = SUCCEEDED(dev->CreateBuffer(&d, nullptr, &st)) && st;
            dev->Release();
            if (!made) return false;
            ctx->CopyResource(st, src);
            D3D11_MAPPED_SUBRESOURCE m{};
            bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData;
            if (ok) {
                out.assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + bytes / 4);
                ctx->Unmap(st, 0);
            }
            st->Release();
            return ok;
        }

        void dump(ID3D11DeviceContext* ctx, ID3D11Buffer* rl, UINT bl, ID3D11Buffer* rs, UINT bs, const float delta[3], int64_t key)
        {
            std::vector<float> L, S;
            if (!read_back(ctx, rl, bl, L) || (rs && !read_back(ctx, rs, bs, S))) {
                spdlog::info("[LampDump] read-back failed");
                return;
            }
            const uint32_t nl = bl / 144, ns = rs ? bs / 96 : 0;
            spdlog::info("[LampDump] eye call {} (eye {}): {} lamp entries, {} shadow entries; eye offset from the game camera ({:.3f}, {:.3f}, {:.3f}) m", key / 2, (key / 2) & 1,
                         nl, ns, delta[0], delta[1], delta[2]);
            std::vector<int> want(ns, 0);
            int              shown = 0;
            for (uint32_t i = 0; i < nl && shown < 96; ++i) {
                const float* e = L.data() + i * 36;
                uint32_t     fl = 0;
                std::memcpy(&fl, e + 11, 4);
                if (!(e[4] > 0.0f || e[5] > 0.0f || e[6] > 0.0f) && (fl & 12u) == 0) continue;
                ++shown;
                const uint32_t si = fl >> 9;
                spdlog::info("[LampDump] lamp {:3} posCS ({:8.3f} {:8.3f} {:8.3f}) {:5.2f} m rcpR2 {:.4g} colour ({:.3g} {:.3g} {:.3g}) dir ({:.2f} {:.2f} {:.2f}) flags 0x{:X} ({}{}{}{}) shadow {}", i,
                             e[0], e[1], e[2], std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), e[3], e[4], e[5], e[6], e[8], e[9], e[10], fl, (fl & 1u) ? "spot" : "point",
                             (fl & 4u) ? " shadow4" : "", (fl & 8u) ? " shadow8" : "", (fl & 16u) ? " clip" : "", (fl & 12u) ? fmt::format("{}", si) : std::string("-"));
                if ((fl & 12u) != 0)
                    for (uint32_t k = si; k < std::min(ns, si + 8); ++k) want[k] = std::max(want[k], k == si ? 2 : 1);
            }
            for (uint32_t k = 0; k < ns; ++k) {
                if (!want[k]) continue;
                const float* p = S.data() + k * 24;
                spdlog::info("[LampDump] shadow {:3}{} p0 ({:.4g} {:.4g} {:.4g} {:.4g}) p1 ({:.4g} {:.4g} {:.4g} {:.4g}) p2 ({:.4g} {:.4g} {:.4g} {:.4g}) p3 ({:.4g} {:.4g} {:.4g} {:.4g}) "
                             "p4 ({:.4g} {:.4g} {:.4g} {:.4g}) p5 ({:.4g} {:.4g} {:.4g} {:.4g})",
                             k, want[k] == 2 ? " (a lamp's)" : "", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18],
                             p[19], p[20], p[21], p[22], p[23]);
            }
        }
    }

    Saved swap(ID3D11DeviceContext* ctx, const Raw& raw, UINT slot_l, UINT slot_s, const float delta[3], int64_t key)
    {
        Saved s{};
        if (!ctx || slot_l > 127) return s;
        ID3D11ShaderResourceView* vl = nullptr;
        ID3D11ShaderResourceView* vs = nullptr;
        ctx->CSGetShaderResources(slot_l, 1, &vl);
        if (slot_s <= 127) ctx->CSGetShaderResources(slot_s, 1, &vs);
        UINT          bl = 0, bs = 0;
        ID3D11Buffer* rl = source(vl, 144, bl);
        ID3D11Buffer* rs = source(vs, 96, bs);
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        // This eye call's copy of exactly this pair of lamp buffers, made since the last write to either.
        const uint64_t gen = g_gen.load();
        Entry*         e   = nullptr;
        for (auto& x : g_e)
            if (x.key == key && x.gen == gen && x.src_l == rl && x.src_s == rs && x.l.buf) { e = &x; break; }
        bool build = false;
        if (!e && rl) {
            // A slot to (re)build: the one that held this pair before (its buffers already have the size), else the least recently used.
            for (auto& x : g_e) if (x.src_l == rl && x.src_s == rs) { e = &x; break; }
            if (!e) {
                e = &g_e[0];
                for (auto& x : g_e) if (x.used < e->used) e = &x;
            }
            build = true;
        }
        bool ok = rl && dev && e && setup(dev) && make_copy(dev, e->l, bl, 144) && (!rs || make_copy(dev, e->s, bs, 96));
        if (dev) dev->Release();
        if (!ok) {
            g_refused++;
            if (e && build) e->key = -1;
            if (rl) rl->Release();
            if (rs) rs->Release();
            if (vl) vl->Release();
            if (vs) vs->Release();
            return s;
        }
        if (g_dump.load() > 0 && rs) {
            g_dump--;
            dump(ctx, rl, bl, rs, bs, delta, key);
        }
        e->used = ++g_tick;
        if (build) {
            // The shadow buffer whole first (entries no lamp points at stay as they are), then the moved values.
            e->key   = key;
            e->gen   = gen;
            e->src_l = rl;
            e->src_s = rs;
            const int at = int(e - g_e);
            g_watch[at * 2]     = rl;
            g_watch[at * 2 + 1] = rs;
            g_builds++;
            if (rs) ctx->CopyResource(e->s.buf, rs);
            ID3D11ComputeShader*       old_cs = nullptr;
            ID3D11ShaderResourceView*  old_t[2]{};
            ID3D11UnorderedAccessView* old_u[2]{};
            ID3D11Buffer*              old_b = nullptr;
            ctx->CSGetShader(&old_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 2, old_t);
            ctx->CSGetUnorderedAccessViews(0, 2, old_u);
            ctx->CSGetConstantBuffers(0, 1, &old_b);
            struct { float d[3]; UINT nl; UINT ns; UINT has; UINT pad[2]; } p{ { delta[0], delta[1], delta[2] }, bl / 144, rs ? bs / 96 : 0u, rs ? 1u : 0u, { 0, 0 } };
            raw.update(ctx, g_cb, &p);
            ID3D11ShaderResourceView*  t[2]{ vl, rs ? vs : nullptr };
            ID3D11UnorderedAccessView* u[2]{ e->l.uav, rs ? e->s.uav : nullptr };
            raw.set_cs(ctx, g_cs);
            ctx->CSSetShaderResources(0, 2, t);
            ctx->CSSetUnorderedAccessViews(0, 2, u, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &g_cb);
            raw.dispatch(ctx, (bl / 144 + 63) / 64, 1, 1);
            ID3D11UnorderedAccessView* nu[2]{};
            ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
            raw.set_cs(ctx, old_cs);
            ctx->CSSetShaderResources(0, 2, old_t);
            ctx->CSSetUnorderedAccessViews(0, 2, old_u, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &old_b);
            if (old_cs) old_cs->Release();
            for (auto* v : old_t) if (v) v->Release();
            for (auto* v : old_u) if (v) v->Release();
            if (old_b) old_b->Release();
        }
        rl->Release();
        if (rs) rs->Release();
        // The dispatch reads the copies (the game's views are put back after it).
        s.on = true; s.slot_l = slot_l; s.slot_s = rs ? slot_s : 255; s.old_l = vl; s.old_s = rs ? vs : nullptr;
        if (!rs && vs) vs->Release();
        ctx->CSSetShaderResources(slot_l, 1, &e->l.srv);
        if (rs) ctx->CSSetShaderResources(slot_s, 1, &e->s.srv);
        g_swaps++;
        return s;
    }

    void note_write(ID3D11Resource* res)
    {
        if (!res) return;
        for (auto& w : g_watch)
            if (w.load(std::memory_order_relaxed) == res) { g_gen++; g_drops++; return; }
    }

    void invalidate()
    {
        g_gen++;
        g_drops++;
    }

    void restore(ID3D11DeviceContext* ctx, Saved& s)
    {
        if (!s.on) return;
        ctx->CSSetShaderResources(s.slot_l, 1, &s.old_l);
        if (s.slot_s <= 127) ctx->CSSetShaderResources(s.slot_s, 1, &s.old_s);
        if (s.old_l) s.old_l->Release();
        if (s.old_s) s.old_s->Release();
        s = Saved{};
    }

    void stats(uint64_t& swaps, uint64_t& builds, uint64_t& refused, uint64_t& drops)
    {
        swaps   = g_swaps.exchange(0);
        builds  = g_builds.exchange(0);
        refused = g_refused.exchange(0);
        drops   = g_drops.exchange(0);
    }

    bool enabled() { return g_on.load(); }

    void dump_next() { g_dump = 1; }

    bool command(const std::string& cmd, float, bool on)
    {
        if (cmd == "lampdump") {
            dump_next();
            spdlog::info("[LampDump] the next lighting pass that reads lamp shadows is logged");
            return true;
        }
        if (cmd != "lampeye") return false;
        g_on = on;
        spdlog::info("[LampEye] {}", on ? "on" : "off");
        return true;
    }
}
