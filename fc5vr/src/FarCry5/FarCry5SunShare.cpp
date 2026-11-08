#include "FarCry5SunShare.h"
#include "FarCry5OwnPass.h"
#include "FarCry5ShadowShare.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

// Render thread only, apart from the setters (atomics). See the header for what this does.

namespace fc5::sunshare
{
    namespace
    {
        std::atomic<bool>  g_on{ false };
        std::atomic<int>   g_stagger{ 1 };
        std::atomic<float> g_drift{ 0.0f };
        std::atomic<int>   g_check_every{ 150 };

        constexpr int kSlices = 3;   // CSMSlice0..2 (census: 3 slices)

        uint32_t        g_S = 0;              // ShadowMapSize from the cascade buffer
        uint32_t        g_target_S = 0;       // ShadowMapSize of the target's own buffers (FC5 also uploads a 1536 one; never touched)
        int             g_size_logs = 0;
        ID3D11Resource* g_target = nullptr;   // held (AddRef) until forget()
        ID3D11Resource* g_prev   = nullptr;   // the target before the last forget() (not held; compared only)
        D3D11_TEXTURE2D_DESC g_desc{};
        bool            g_is_array = false;   // cascades are slices of a texture array
        const char*     g_shape = "";
        ID3D11DepthStencilView* g_slice_dsv[8]{};
        DXGI_FORMAT     g_slice_fmt = DXGI_FORMAT_UNKNOWN;

        // Per-slice state of the far-cascade stagger.
        float    g_drawn[kSlices][12]{};   // rows the slice was last drawn with
        bool     g_have_drawn[kSlices]{};
        int      g_age[kSlices]{};          // pairs since the slice was last drawn
        bool     g_fresh[kSlices]{ true, true, true };
        bool     g_any_stale = false;
        bool     g_dirty_all = true;
        int      g_slices_good = 0;          // pairs in a row seen drawing one slice at a time
        bool     g_slices_ok   = false;
        bool     g_told_multi  = false;

        // The current pair.
        uint64_t g_pair = 0;
        bool     g_check = false;
        bool     g_share_pair = false;
        bool     g_stashed = false;
        float    g_rows[kSlices * 12]{};     // the game's rows of this pair's left call (as the game wrote them)
        uint64_t g_rows_pair = 0;            // pair those rows belong to (0 = none)
        uint32_t g_rows_size = 0;            // byte size of that buffer
        // The target's cascade buffers in upload order, per call: which of the left call's held the rows the map is drawn with.
        // The right call's buffer at the same place gets them, whatever fit the right call made (8 Nov: its first slice can be
        // 7.7 % smaller than the left call's, past close_rows' 5 %, and was then left with its own rows on the left call's map).
        uint32_t g_l_idx = 0, g_r_idx = 0, g_l_mask = 0;
        uint32_t g_seen_mask = 0;
        bool     g_seen_multi = false;
        uint32_t g_seen_draws = 0;

        // Counters (since the last report).
        uint64_t c_pairs = 0, c_skip_r_draw = 0, c_skip_r_clear = 0, c_skip_stale_draw = 0, c_skip_stale_clear = 0, c_split_clear = 0;
        uint64_t c_cb_r = 0, c_cb_stale = 0, c_cb_other = 0, c_cb_refit = 0, c_cb_size = 0, c_redraw[kSlices]{}, c_moved[kSlices]{};
        uint64_t c_checks = 0;
        uint64_t t_pairs = 0;                // since start
        std::string g_last_check = "none yet";
        int      g_shape_logs = 0;
        uint32_t g_mask_last  = 0;           // slices the last full pair drew (report)

        bool depth_fmt(DXGI_FORMAT f)
        {
            return f == DXGI_FORMAT_R16_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D16_UNORM ||
                   f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT;
        }

        float len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // The same sun: slice 0's depth axis (row 2) points the same way in both sets of rows.
        bool same_sun(const float* x, const float* y)
        {
            const float* a = x + 8;
            const float* b = y + 8;
            const float  la = len3(a), lb = len3(b);
            return la > 0.0f && lb > 0.0f && (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb) >= 0.9999f;
        }

        // A cascade buffer: ShadowMapSize at c10 = (S, S, 1/S, 1/S), and c0-c8 three slices of one sun projection: the same depth
        // axis (row 2) in all three, and each wider than the one before (row 0's scale = 1 / width, smaller for the far slices).
        int g_reject_logs = 0;

        bool is_csm(uint32_t size, const float* d, uint32_t& S)
        {
            if (size < 224 || size > 512) return false;
            const float s = d[40];
            if (!(s >= 256.0f && s <= 16384.0f) || std::abs(d[41] - s) > 0.5f || std::abs(d[42] * s - 1.0f) > 1e-3f || std::abs(d[43] * s - 1.0f) > 1e-3f) return false;
            // ShadowMapSize matches; the slice rows must look like three slices of one sun projection (else: logged a few times, left alone).
            auto reject = [&](const char* why, int k) {
                if (g_reject_logs < 4) {
                    g_reject_logs++;
                    std::string r;
                    for (int i = 0; i < 36; i += 4) r += fmt::format("({:.4g} {:.4g} {:.4g} {:.4g}) ", d[i], d[i + 1], d[i + 2], d[i + 3]);
                    spdlog::info("[SunShare] {} B buffer with ShadowMapSize {} left alone: slice {} {}; c0-c8 {}", size, s, k, why, r);
                }
                return false;
            };
            float z0[3]{};
            float prev_scale = 1e30f;
            for (int k = 0; k < kSlices; ++k) {
                const float* a = d + k * 12;
                const float* c = d + k * 12 + 8;
                const float sa = len3(a), sb = len3(a + 4), sc = len3(c);
                if (!(sa > 1e-7f) || !(sb > 1e-7f) || !(sc > 1e-9f) || !std::isfinite(a[3]) || !std::isfinite(c[3])) return reject("has an empty or invalid row", k);
                if (sa > prev_scale * 1.0001f) return reject("is not wider than the one before", k);   // nearest slice first
                prev_scale = sa;
                const float z[3]{ c[0] / sc, c[1] / sc, c[2] / sc };
                if (k == 0) std::memcpy(z0, z, sizeof z);
                else if (z[0] * z0[0] + z[1] * z0[1] + z[2] * z0[2] < 0.9999f) return reject("has another depth axis than slice 0", k);
            }
            S = uint32_t(s + 0.5f);
            return true;
        }

        // Two sets of slice rows describe the same fit up to an eye's difference: same sun axis, widths within 5 %, centres
        // within 10 % of the width.
        bool close_rows(const float* x, const float* y)
        {
            for (int k = 0; k < kSlices; ++k) {
                for (int r = 0; r < 3; ++r) {
                    const float* a = x + k * 12 + r * 4;
                    const float* b = y + k * 12 + r * 4;
                    const float la = len3(a), lb = len3(b);
                    if (!(la > 0.0f) || std::abs(la - lb) > 0.05f * la) return false;
                    if ((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb) < 0.9999f) return false;
                    if (r < 2 && std::abs(a[3] - b[3]) > 0.1f) return false;   // uv offset: 0.1 = a tenth of the slice
                }
            }
            return true;
        }

        // How far slice k's current fit is from the rows it was drawn with: uv offset change in texels; 1e9 when the sun or the
        // slice's scale changed.
        float drift_texels(int k)
        {
            const float* a = g_rows + k * 12;
            const float* b = g_drawn[k];
            float t = 0.0f;
            for (int r = 0; r < 3; ++r) {
                for (int i = 0; i < 3; ++i) {
                    const float l = std::max(len3(b + r * 4), 1e-12f);
                    if (std::abs(a[r * 4 + i] - b[r * 4 + i]) > 1e-4f * l) return 1e9f;
                }
                if (r < 2) t = std::max(t, std::abs(a[r * 4 + 3] - b[r * 4 + 3]) * float(g_S));
            }
            return t;
        }

        void drop_slice_views()
        {
            for (auto*& v : g_slice_dsv) {
                if (v) v->Release();
                v = nullptr;
            }
            g_slice_fmt = DXGI_FORMAT_UNKNOWN;
        }

        void reset_stagger(const char* why)
        {
            for (int k = 0; k < kSlices; ++k) { g_have_drawn[k] = false; g_age[k] = 0; g_fresh[k] = true; }
            g_any_stale   = false;
            g_dirty_all   = true;
            g_slices_good = 0;
            g_slices_ok   = false;
            if (why && g_stagger.load() > 1) spdlog::info("[SunShare] far-cascade stagger restarts: {}", why);
        }

        void report()
        {
            const double n = double(std::max<uint64_t>(1, c_pairs));
            spdlog::info("[SunShare] last {} pairs: right call skipped {:.1f} draws and {:.2f} clears per pair, {:.2f} buffers set to the left rows "
                         "({:.2f} of them with another fit of its own), {:.2f} buffers of another map size left alone; "
                         "stagger {} ({}): far slices redrawn {:.0f} % / {:.0f} % of pairs (moved {:.0f} % / {:.0f} %), {:.1f} stale draws and {:.2f} stale clears skipped per pair, "
                         "{:.2f} array clears split, {:.2f} buffers set to the drawn rows, {} other cascade buffers left alone; slices drawn in the last full pair 0x{:x}; checks {}, last {}",
                         c_pairs, c_skip_r_draw / n, c_skip_r_clear / n, c_cb_r / n, c_cb_refit / n, c_cb_size / n, g_stagger.load(), g_slices_ok ? "per slice" : "not per slice", 100.0 * c_redraw[1] / n,
                         100.0 * c_redraw[2] / n, 100.0 * c_moved[1] / n, 100.0 * c_moved[2] / n, c_skip_stale_draw / n, c_skip_stale_clear / n, c_split_clear / n,
                         c_cb_stale / n, c_cb_other, g_mask_last, c_checks, g_last_check);
            c_pairs = c_skip_r_draw = c_skip_r_clear = c_skip_stale_draw = c_skip_stale_clear = c_split_clear = c_cb_r = c_cb_stale = c_cb_other = c_cb_refit = c_cb_size = c_checks = 0;
            for (int k = 0; k < kSlices; ++k) c_redraw[k] = c_moved[k] = 0;
        }
    }

    void set_on(bool on)
    {
        if (g_on.exchange(on) != on) spdlog::info("[SunShare] sun shadows once per pair {}", on ? "on" : "off");
    }
    bool on() { return g_on.load(); }
    void set_stagger(int n)
    {
        n = std::clamp(n, 1, 16);
        g_stagger = n;
        spdlog::info("[SunShare] far-cascade stagger {} ({}){}", n, n <= 1 ? "off" : "slice 1 every " + std::to_string(std::max(1, n / 2)) + ", slice 2 every " + std::to_string(n) + " pairs",
                     n > 1 && !g_on.load() ? "; needs sunshare on" : "");
    }
    int  stagger() { return g_stagger.load(); }
    void set_drift(float t)
    {
        g_drift = std::max(0.0f, t);
        spdlog::info("[SunShare] far slice redrawn when its fit moved more than {} texels", g_drift.load());
    }
    void set_check_every(int n)
    {
        g_check_every = std::max(0, n);
        spdlog::info("[SunShare] check pair every {} pairs{}", g_check_every.load(), n <= 0 ? " (no checks)" : "");
    }

    void note_cb(int call, uint32_t size, float* d)
    {
        if (!g_on.load() || call < 1 || call > 2) return;
        uint32_t S = 0;
        if (!is_csm(size, d, S)) return;
        if (!g_target) {
            // Learning: the map size the next depth-only target is matched against. FC5 alternates a 3072 and a 1536 buffer,
            // so keep the larger one (the sun cascades); a target of the small map's shape could otherwise be taken.
            if (S > g_S) {
                if (g_size_logs < 6) { g_size_logs++; spdlog::info("[SunShare] cascade buffer: map size {} (was {})", S, g_S); }
                g_S = S;
            }
            return;
        }
        if (S != g_target_S) { c_cb_size++; return; }   // another shadow map's buffer: never touched
        if (call == 1) {
            const uint32_t idx = g_l_idx++;
            if (g_rows_pair != g_pair) {
                // The pair's first cascade buffer: the game's rows for this frame; the slices drawn this pair keep them.
                std::memcpy(g_rows, d, sizeof g_rows);
                g_rows_pair = g_pair;
                g_rows_size = size;
                for (int k = 0; k < kSlices; ++k) {
                    if (g_fresh[k]) { std::memcpy(g_drawn[k], d + k * 12, 48); g_have_drawn[k] = true; }
                }
            } else if (!close_rows(g_rows, d)) {
                c_cb_other++;
                return;
            }
            if (idx < 32) g_l_mask |= 1u << idx;
            if (!g_any_stale) return;
            for (int k = 0; k < kSlices; ++k) {
                if (!g_fresh[k] && g_have_drawn[k]) std::memcpy(d + k * 12, g_drawn[k], 48);
            }
            c_cb_stale++;
            return;
        }
        // Right call: the map is the left call's, so the lighting gets the rows it was drawn with. The buffer is matched by its
        // place among the target's buffers (the left call's at that place held the drawn rows), its size and the sun, not by
        // how close its fit is: the right call may fit its cascades differently, and its own rows on the left map misplace
        // every shadow in the right eye.
        const uint32_t idx = g_r_idx++;
        if (!g_share_pair || g_rows_pair != g_pair) return;
        if (idx >= 32 || !(g_l_mask & (1u << idx)) || size != g_rows_size || !same_sun(g_rows, d)) { c_cb_other++; return; }
        if (!close_rows(g_rows, d)) c_cb_refit++;
        for (int k = 0; k < kSlices; ++k) {
            if (g_have_drawn[k]) std::memcpy(d + k * 12, g_drawn[k], 48);
        }
        c_cb_r++;
    }

    void consider(ID3D11Resource* res, ID3D11Resource* asm_atlas, ID3D11Resource* asm_tmp)
    {
        if (!g_on.load() || !res || res == g_target || g_S == 0) return;
        if (res == asm_atlas || res == asm_tmp) return;
        D3D11_RESOURCE_DIMENSION dim{};
        res->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        const uint32_t S = g_S;
        const char* shape = nullptr;
        bool array = false;
        if (d.Width == S && d.Height == S && d.ArraySize >= uint32_t(kSlices)) { shape = "texture array"; array = true; }
        else if (d.ArraySize == 1 && ((d.Width == 3 * S && d.Height == S) || (d.Width == S && d.Height == 3 * S))) shape = "strip of three";
        else if (d.ArraySize == 1 && d.Width == 2 * S && d.Height == 2 * S) shape = "2x2 atlas";
        const bool ok = shape && depth_fmt(d.Format) && d.SampleDesc.Count == 1 && !(d.Width == 2560 && d.Height == 1792) && !(d.Width == 512 && d.Height == 256);
        if (!ok) {
            if (g_shape_logs < 12 && !g_target) {
                g_shape_logs++;
                spdlog::info("[SunShare] depth-only target {} {}x{} x{} format {}: not the cascade map (size {})", (void*)res, d.Width, d.Height, d.ArraySize, (int)d.Format, S);
            }
            return;
        }
        if (g_target) return;   // one cascade target
        res->AddRef();
        g_target   = res;
        g_target_S = S;
        g_desc     = d;
        g_is_array = array;
        g_shape    = shape;
        drop_slice_views();
        g_dirty_all = true;   // learned during the left call: this pair's right call draws its own map
        if (res != g_prev) {
            reset_stagger(nullptr);
            spdlog::info("[SunShare] sun cascade target {} {}x{} x{} format {} ({}, map size {})", (void*)res, d.Width, d.Height, d.ArraySize, (int)d.Format, shape, S);
        }
    }

    bool is_cascade(ID3D11Resource* res) { return res && res == g_target; }

    int slice_of(ID3D11DepthStencilView* dsv)
    {
        if (!dsv) return -1;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        if (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY && vd.Texture2DArray.ArraySize == 1) return int(vd.Texture2DArray.FirstArraySlice);
        return -1;
    }

    void forget()
    {
        g_prev = g_target;
        if (g_target) g_target->Release();
        g_target = nullptr;
        g_target_S = 0;
        drop_slice_views();
        g_share_pair = false;
        g_any_stale  = false;
        g_dirty_all  = true;   // the right call of this pair may draw its own map
        for (auto& f : g_fresh) f = true;
    }

    void pair_begin()
    {
        ++g_pair;
        g_stashed    = false;
        g_seen_mask  = 0;
        g_seen_multi = false;
        g_seen_draws = 0;
        g_l_idx = g_r_idx = g_l_mask = 0;
        for (auto& f : g_fresh) f = true;
        g_any_stale = false;
        if (!g_on.load() || !g_target) { g_share_pair = g_check = false; return; }
        c_pairs++;
        t_pairs++;
        const int every = g_check_every.load();
        g_check      = every > 0 && (g_pair % uint64_t(every)) == 0;
        g_share_pair = !g_check;
        for (auto& a : g_age) a++;
        const int n = g_stagger.load();
        if (n > 1 && g_slices_ok && !g_check && !g_dirty_all && g_rows_pair != 0) {
            for (int k = 1; k < kSlices; ++k) {
                const int  max_age = k == 1 ? std::max(1, n / 2) : n;
                const bool moved   = g_have_drawn[k] && drift_texels(k) > g_drift.load();
                if (moved) c_moved[k]++;
                g_fresh[k] = !g_have_drawn[k] || g_age[k] >= max_age || moved;
                // Spread the two far slices over different pairs when both come due by age alone.
                if (k == 2 && g_fresh[2] && g_fresh[1] && !moved && g_have_drawn[2] && g_age[2] <= n) g_fresh[2] = false;
                if (!g_fresh[k]) g_any_stale = true;
            }
        }
        g_dirty_all = false;
        for (int k = 0; k < kSlices; ++k) {
            if (g_fresh[k]) { g_age[k] = 0; c_redraw[k]++; }
        }
        if (c_pairs >= 900) report();
    }

    void between_calls(ID3D11DeviceContext* ctx)
    {
        if (!g_on.load() || !g_target) return;
        // Do the game's draws into the cascade array name one slice each? Three such pairs in a row before staggering.
        if (g_seen_draws > 0 && !g_any_stale) {
            g_mask_last = g_seen_mask | (g_seen_multi ? 0x80000000u : 0u);
            if (!g_is_array || g_seen_multi) {
                g_slices_good = 0;
                g_slices_ok   = false;
                if (g_stagger.load() > 1 && !g_told_multi) {
                    g_told_multi = true;
                    spdlog::info("[SunShare] far-cascade stagger not possible: {}", g_is_array ? "the game draws into several cascade slices through one view" : "the cascades are not slices of a texture array");
                }
            } else if ((g_seen_mask & 7u) == 7u) {
                if (++g_slices_good >= 3 && !g_slices_ok) {
                    g_slices_ok = true;
                    if (g_stagger.load() > 1) spdlog::info("[SunShare] far-cascade stagger ready: one slice per view, slices seen 0x{:x}", g_seen_mask);
                }
            }
        }
        if (g_check) g_stashed = fc5::shadowshare::stash(ctx, g_target);
    }

    void after_right(ID3D11DeviceContext* ctx)
    {
        if (!g_on.load() || !g_target || !g_check) return;
        if (g_stashed) fc5::shadowshare::compare(ctx, g_target, uint32_t(g_pair));
        g_dirty_all = true;   // the right call drew its own map: every slice again next pair
    }

    void poll(ID3D11DeviceContext* ctx)
    {
        uint32_t pair = 0;
        uint64_t diff = 0, total = 0;
        if (!fc5::shadowshare::result(ctx, pair, diff, total)) return;
        c_checks++;
        g_last_check = fmt::format("{} of {} texels differ ({:.4f} %)", diff, total, total ? 100.0 * double(diff) / double(total) : 0.0);
        spdlog::info("[SunShare] check pair {}: the right call's own cascade map vs the left call's: {}", pair, g_last_check);
    }

    void scope_drew() { g_dirty_all = true; }

    bool skip_draw(int call, ID3D11Resource* res, int slice)
    {
        if (!g_target || res != g_target) return false;
        if (call == 1) {
            g_seen_draws++;
            if (slice < 0) g_seen_multi = true;
            else if (slice < 32) g_seen_mask |= 1u << slice;
            if (g_any_stale && slice >= 1 && slice < kSlices && !g_fresh[slice]) {
                c_skip_stale_draw++;
                return true;
            }
            return false;
        }
        if (call == 2) {
            if (g_share_pair) { c_skip_r_draw++; return true; }
            return false;
        }
        if (call == 3) scope_drew();
        return false;
    }

    bool skip_clear(ID3D11DeviceContext* ctx, int call, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil)
    {
        if (!g_target || !dsv) return false;
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) res->Release();
        if (res != g_target) return false;
        if (call == 2) {
            if (g_share_pair) { c_skip_r_clear++; return true; }
            return false;
        }
        if (call == 3) { scope_drew(); return false; }
        if (call != 1 || !g_any_stale) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2DARRAY) return false;
        const UINT first = vd.Texture2DArray.FirstArraySlice, count = vd.Texture2DArray.ArraySize;
        if (count == 1) {
            if (first >= 1 && first < UINT(kSlices) && !g_fresh[first]) { c_skip_stale_clear++; return true; }
            return false;
        }
        // One clear over several slices: clear only the slices drawn this pair, through views of our own.
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        if (g_slice_fmt != vd.Format) {
            drop_slice_views();
            g_slice_fmt = vd.Format;
        }
        bool ok = true;
        for (UINT s = first; s < first + count && s < 8; ++s) {
            if (!g_slice_dsv[s]) {
                D3D11_DEPTH_STENCIL_VIEW_DESC one = vd;
                one.Flags                           = 0;
                one.Texture2DArray.FirstArraySlice  = s;
                one.Texture2DArray.ArraySize        = 1;
                if (FAILED(dev->CreateDepthStencilView(g_target, &one, &g_slice_dsv[s]))) { ok = false; break; }
            }
        }
        dev->Release();
        if (!ok || first + count > 8) return false;
        {
            fc5::ownpass::Scope own;
            for (UINT s = first; s < first + count; ++s) {
                if (s >= 1 && s < UINT(kSlices) && !g_fresh[s]) continue;
                ctx->ClearDepthStencilView(g_slice_dsv[s], flags, depth, stencil);
            }
        }
        c_split_clear++;
        return true;
    }

    std::string status()
    {
        if (!g_on.load()) return "sunshare off";
        if (!g_target) return fmt::format("sunshare on, no cascade target yet (map size {})", g_S);
        return fmt::format("sunshare on: {} {}x{} x{}, {} pairs, stagger {} ({}), last check {}", g_shape, g_desc.Width, g_desc.Height, g_desc.ArraySize, t_pairs, g_stagger.load(),
                           g_slices_ok ? "per slice" : "not per slice", g_last_check);
    }
}
