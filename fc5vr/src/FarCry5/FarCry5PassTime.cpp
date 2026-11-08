#include "FarCry5PassTime.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace fc5::passtime
{
    namespace
    {
        constexpr int      kSets   = 4;      // pairs in flight
        constexpr int      kStamps = 4096;   // timestamps per pair (about 2 x 300 target changes + 2 x 160 compute runs measured)
        constexpr uint16_t kEnd    = 0xFFFF;

        struct Set
        {
            ID3D11Query* disjoint = nullptr;
            ID3D11Query* ts[kStamps]{};
            uint16_t     key[kStamps]{};
            int          n       = 0;
            bool         pending = false;
        };
        Set*             g_sets  = nullptr;   // kSets, made at the first capture
        bool             g_ready = false, g_failed = false;
        int              g_cur = -1, g_next = 0;
        std::atomic<int> g_want{ 0 };   // pairs still to start
        int              g_target = 0, g_read = 0, g_started = 0;
        uint64_t         g_overflow = 0, g_disjoint = 0, g_skipped = 0;

        std::vector<std::string>                  g_keys;
        std::unordered_map<std::string, uint16_t> g_key_ix;
        std::vector<double>                       g_sum;    // ms over the capture, per key
        std::vector<uint32_t>                     g_cnt;    // segments over the capture, per key
        double                                    g_pair_sum = 0.0;
        double                                    g_call_sum[4]{};

        int                     g_call = 0;
        UINT                    g_last_n = ~0u;
        ID3D11RenderTargetView* g_last_rtv0 = nullptr;
        ID3D11DepthStencilView* g_last_dsv  = nullptr;
        std::string             g_bind_key;   // the current targets' name (without the call)
        const void*             g_last_cs = nullptr;
        bool                    g_in_cs   = false;

        std::mutex  g_sum_mtx;
        std::string g_summary;

        const char* call_name(int c) { return c == 1 ? "L" : c == 2 ? "R" : c == 3 ? "S" : "-"; }

        uint16_t intern(const std::string& k)
        {
            if (auto it = g_key_ix.find(k); it != g_key_ix.end()) return it->second;
            if (g_keys.size() >= kEnd - 1) return uint16_t(kEnd - 1);   // "(too many names)"
            const uint16_t ix = uint16_t(g_keys.size());
            g_keys.push_back(k);
            g_sum.push_back(0.0);
            g_cnt.push_back(0);
            g_key_ix.emplace(k, ix);
            return ix;
        }

        bool create(ID3D11DeviceContext* ctx)
        {
            if (g_ready || g_failed) return g_ready;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return false;
            g_sets  = new Set[kSets];
            bool ok = true;
            for (int i = 0; i < kSets && ok; ++i) {
                D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                ok = SUCCEEDED(dev->CreateQuery(&d, &g_sets[i].disjoint));
                d.Query = D3D11_QUERY_TIMESTAMP;
                for (auto& q : g_sets[i].ts) ok = ok && SUCCEEDED(dev->CreateQuery(&d, &q));
            }
            dev->Release();
            g_ready  = ok;
            g_failed = !ok;
            spdlog::info("[PassTime] {} timestamp queries {}", kSets * kStamps, ok ? "created" : "FAILED (no per-pass times)");
            return ok;
        }

        void stamp(ID3D11DeviceContext* ctx, uint16_t key)
        {
            if (g_cur < 0) return;
            Set& s = g_sets[g_cur];
            if (s.n >= kStamps - 1) { g_overflow++; return; }   // the last slot is kept for the pair's end
            s.key[s.n] = key;
            ctx->End(s.ts[s.n]);
            s.n++;
        }

        std::string view_desc(ID3D11View* v)
        {
            if (!v) return "none";
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            if (!r) return "none";
            D3D11_RESOURCE_DIMENSION dim{};
            r->GetType(&dim);
            std::string out = "?";
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                out = d.ArraySize > 1 ? fmt::format("{}x{}x{} f{}", d.Width, d.Height, d.ArraySize, (int)d.Format) : fmt::format("{}x{} f{}", d.Width, d.Height, (int)d.Format);
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                D3D11_TEXTURE3D_DESC d{};
                static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
                out = fmt::format("3D {}x{}x{} f{}", d.Width, d.Height, d.Depth, (int)d.Format);
            }
            r->Release();
            return out;
        }

        // A finished pair: each segment's time added to its name.
        bool read(ID3D11DeviceContext* ctx, Set& s)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(s.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
            static uint64_t t[kStamps];
            for (int i = 0; i < s.n; ++i)
                if (ctx->GetData(s.ts[i], &t[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
            s.pending = false;
            if (dj.Disjoint || dj.Frequency == 0 || s.n < 2) { g_disjoint++; return true; }
            const double to_ms = 1000.0 / double(dj.Frequency);
            for (int i = 0; i + 1 < s.n; ++i) {
                const uint16_t k = s.key[i];
                if (k >= g_sum.size() || t[i + 1] < t[i]) continue;
                const double ms = double(t[i + 1] - t[i]) * to_ms;
                g_sum[k] += ms;
                g_cnt[k]++;
                const char c = g_keys[k].empty() ? '-' : g_keys[k][0];
                g_call_sum[c == 'L' ? 1 : c == 'R' ? 2 : c == 'S' ? 3 : 0] += ms;
            }
            if (t[s.n - 1] >= t[0]) g_pair_sum += double(t[s.n - 1] - t[0]) * to_ms;
            g_read++;
            return true;
        }

        void report()
        {
            const int    pairs = std::max(1, g_read);
            const double per   = 1.0 / double(pairs);
            std::vector<int> order;
            for (int i = 0; i < (int)g_keys.size(); ++i) if (g_cnt[i] > 0) order.push_back(i);
            std::sort(order.begin(), order.end(), [](int a, int b) { return g_sum[a] > g_sum[b]; });
            std::string head = fmt::format("{} pairs: span {:.2f} ms per pair = left call {:.2f} + right call {:.2f} + scope call {:.2f} + outside the calls {:.2f}; {} names; "
                                           "{} pairs disjoint, {} skipped, {} stamps over the limit",
                                           g_read, g_pair_sum * per, g_call_sum[1] * per, g_call_sum[2] * per, g_call_sum[3] * per, g_call_sum[0] * per, order.size(), g_disjoint,
                                           g_skipped, g_overflow);
            spdlog::info("[PassTime] {}", head);
            spdlog::info("[PassTime]   ms/pair  segments/pair  name (L left call, R right call, S scope call; targets as colour [+ depth], fN = DXGI format)");
            std::string top;
            int         shown = 0;
            for (int i : order) {
                if (shown >= 80) break;
                const double ms = g_sum[i] * per;
                if (ms < 0.02 && shown >= 20) break;
                spdlog::info("[PassTime]   {:7.3f}  {:6.1f}  {}", ms, double(g_cnt[i]) * per, g_keys[i]);
                if (shown < 8) top += fmt::format("{:.2f} {}; ", ms, g_keys[i]);
                shown++;
            }
            // The same names with the call taken off: what one pass costs in both eyes together.
            std::unordered_map<std::string, double> both;
            for (int i : order) both[g_keys[i].size() > 2 ? g_keys[i].substr(2) : g_keys[i]] += g_sum[i] * per;
            std::vector<std::pair<std::string, double>> b(both.begin(), both.end());
            std::sort(b.begin(), b.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            spdlog::info("[PassTime]   both eyes together, largest 30:");
            for (int i = 0; i < (int)b.size() && i < 30; ++i) spdlog::info("[PassTime]   {:7.3f}  {}", b[i].second, b[i].first);
            spdlog::info("[PassTime] done");
            std::lock_guard _{ g_sum_mtx };
            g_summary = head + "\n" + top;
        }
    }

    void start(int pairs)
    {
        if (pairs <= 0) {
            g_want = 0;
            g_target = 0;
            spdlog::info("[PassTime] stopped");
            return;
        }
        g_keys.clear();
        g_key_ix.clear();
        g_sum.clear();
        g_cnt.clear();
        g_pair_sum = 0.0;
        for (auto& c : g_call_sum) c = 0.0;
        g_read = g_started = 0;
        g_overflow = g_disjoint = g_skipped = 0;
        g_target = std::min(pairs, 2000);
        g_want   = g_target;
        spdlog::info("[PassTime] per-pass GPU times for the next {} pairs", g_target);
    }

    bool active() { return g_cur >= 0; }
    bool wanted() { return g_target > 0; }

    void pair_begin(ID3D11DeviceContext* ctx)
    {
        g_cur = -1;
        if (g_want.load() <= 0 || !ctx || !create(ctx)) return;
        Set& s = g_sets[g_next];
        if (s.pending && !read(ctx, s)) { g_skipped++; return; }
        g_next = (g_next + 1) % kSets;
        s.n    = 0;
        g_cur  = int(&s - g_sets);
        g_want--;
        g_started++;
        g_call      = 0;
        g_last_n    = ~0u;
        g_last_rtv0 = nullptr;
        g_last_dsv  = nullptr;
        g_bind_key  = "(targets of the previous pair)";
        g_last_cs   = nullptr;
        g_in_cs     = false;
        ctx->Begin(s.disjoint);
        stamp(ctx, intern("- before the left call"));
    }

    void pair_end(ID3D11DeviceContext* ctx)
    {
        if (g_cur < 0 || !ctx) return;
        Set& s = g_sets[g_cur];
        s.key[s.n] = kEnd;
        ctx->End(s.ts[s.n]);
        s.n++;
        ctx->End(s.disjoint);
        s.pending = true;
        g_cur     = -1;
    }

    void call_begin(ID3D11DeviceContext* ctx, int call)
    {
        if (g_cur < 0 || !ctx) return;
        g_call  = call;
        g_in_cs = false;
        g_last_cs = nullptr;
        stamp(ctx, intern(fmt::format("{} start of the call ({})", call_name(call), g_bind_key)));
    }

    void bind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, const char* label)
    {
        if (g_cur < 0 || !ctx) return;
        ID3D11RenderTargetView* rtv0 = (rtvs && n > 0) ? rtvs[0] : nullptr;
        UINT k = 0;
        for (UINT i = 0; rtvs && i < n; ++i) if (rtvs[i]) ++k;
        if (k == g_last_n && rtv0 == g_last_rtv0 && dsv == g_last_dsv && !g_in_cs) return;
        const bool same = k == g_last_n && rtv0 == g_last_rtv0 && dsv == g_last_dsv;
        g_last_n    = k;
        g_last_rtv0 = rtv0;
        g_last_dsv  = dsv;
        g_in_cs     = false;
        g_last_cs   = nullptr;
        if (!same) {
            if (k == 0 && !dsv) g_bind_key = "no targets";
            else {
                g_bind_key = k ? fmt::format("{}{}", view_desc(rtv0), k > 1 ? fmt::format(" (+{} more)", k - 1) : std::string{}) : std::string{ "depth only" };
                if (dsv) g_bind_key += label ? fmt::format(" + {}", label) : " + depth " + view_desc(dsv);
            }
        }
        stamp(ctx, intern(fmt::format("{} {}", call_name(g_call), g_bind_key)));
    }

    bool dispatch_needs_label(const void* cs) { return g_cur >= 0 && (!g_in_cs || cs != g_last_cs); }

    void dispatch(ID3D11DeviceContext* ctx, const void* cs, const std::string& label)
    {
        if (g_cur < 0 || !ctx || (g_in_cs && cs == g_last_cs)) return;
        g_in_cs   = true;
        g_last_cs = cs;
        stamp(ctx, intern(fmt::format("{} compute {}", call_name(g_call), label.empty() ? fmt::format("cs {}", cs) : label.substr(0, 80))));
    }

    bool in_compute() { return g_cur >= 0 && g_in_cs; }

    void resume_draws(ID3D11DeviceContext* ctx)
    {
        if (g_cur < 0 || !ctx || !g_in_cs) return;
        g_in_cs   = false;
        g_last_cs = nullptr;
        stamp(ctx, intern(fmt::format("{} {}", call_name(g_call), g_bind_key)));
    }

    void poll(ID3D11DeviceContext* ctx)
    {
        if (!g_ready || !ctx || g_target <= 0) return;
        for (int i = 0; i < kSets; ++i)
            if (g_sets[i].pending) read(ctx, g_sets[i]);
        bool busy = false;
        for (int i = 0; i < kSets; ++i) busy = busy || g_sets[i].pending;
        if (g_want.load() <= 0 && !busy && g_cur < 0 && g_started > 0) {
            report();
            g_target  = 0;
            g_started = 0;
        }
    }

    std::string summary()
    {
        std::lock_guard _{ g_sum_mtx };
        return g_summary;
    }
}
