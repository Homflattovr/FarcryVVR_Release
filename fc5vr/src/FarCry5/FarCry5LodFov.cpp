// lodfov (author 28 Oct 2026, route A of research\2026-10-28_fov_lod_popin.md): far detail as at the flat FOV.
// The game picks LOD with LodScale and KillLodScale times its zoom factor (FOV / 75 deg)^0.75 (simulator, 28 Oct): x1.42 at the
// mod's 120 deg camera, so objects switch to coarser models and disappear sooner than on the flat screen. The scales live in
// engine\settings\DefaultRenderConfig.xml (common.dat, LZ4 block); the parsed values were not found in memory. So when the game
// reads that entry from common.dat, the read gets a changed copy instead: the Geometry presets' LodScale and KillLodScale times
// the scale (on = 0.70: ultrahigh 0.5 -> 0.35), comments blanked to make room, compressed to exactly the original size (the
// archive's index is not touched, nothing on disk changes). Off: no hook, the game reads its own bytes.
// shadowres and vtbias (blur research, 5 Nov 2026, research\2026-11-05_blur): two more values of the same config, by the same way.
// shadowres <n>: the Shadow presets' CascadedShadowMapSize (ultrahigh 1536): the sun's cascades are fitted to the mod's 120 deg game
// camera, much wider than the flat 75 deg one, so each shadow texel covers about 1.7-2x more ground. vtbias <x>: added to the
// Terrain presets' VirtualTextureLODBias (high 0.0): the terrain's virtual texture does not use the sampler mip bias (mipbias).
// Since 6 Nov (texdetail) vtbias is no setting of its own: it follows the saved mipbias (F11 Display "Texture detail" sets both).
// Both off by default; each one alone also builds the changed config.
// perf switches (performance thread, author 6 Nov 2026, research\2026-11-06_perf): game render
// costs that Stereo pays twice per pair (each eye's call renders the whole frame), each set one step down in the same config:
// perfleafshadow (tree leaves cast into one shadow cascade, as at medium), perfgrassshadow (grass shadows only in the nearest
// cascade, as at very high), perflampshadow (local light shadow maps updated once per call instead of twice: per picture as on
// the flat screen), perfsoftshadow (PCSS soft shadow filter level 1), perfreflect (screen-space reflections mode 2, as at high),
// perffog (volumetric fog 196 instead of 256, as at medium). Each off by default (the game's values); saved; next start.
// sunranges (shadow clarity, 7 Nov 2026, research\2026-11-07_shadow_clarity.md): the Shadow presets' SunShadowRange0/1/2, the far
// distance of each sun cascade (game 4 / 11 / 30 m; only "low" names them, the others use the code's defaults), set or added in every
// preset but the cinematic and editor ones. Test, off by default; saved; next start.
#include "FarCry5LodFov.h"

#include "FarCry5Defaults.h"
#include "../ModPath.h"

#include <windows.h>
#include <winternl.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fc5::lodfov
{
namespace
{
    constexpr uint64_t kHash  = 0x9d542a475c8ccadfull;   // engine\settings\DefaultRenderConfig.xml
    constexpr float    kOn    = 0.70f;                   // 1 / 1.4226: LOD as at the game's own 75 deg
    inline bool        NT_OK(NTSTATUS s) { return s >= 0; }

    std::atomic<float> g_scale{ 0.0f };                  // the setting (next start)
    float              g_active = 0.0f;                  // what this run's config was built with
    std::atomic<int>   g_shadow_res{ 0 };                // shadowres: the setting (next start), 0 = the game's
    std::atomic<float> g_vt_bias{ 0.0f };                // vtbias: the setting (next start), 0 = the game's
    int                g_shadow_active = 0;              // this run's
    float              g_vt_active     = 0.0f;
    std::atomic<int>   g_perf{ 0 };                      // perf switches: the setting (next start), bits of kPerf
    std::mutex         g_ranges_mx;                      // guards g_ranges (commands and the F11 menu)
    std::string        g_ranges        = "off";          // sunranges: the setting (next start), "off" or "a/b/c"
    std::string        g_ranges_active = "off";          // this run's (written once at the start)
    int                g_perf_active   = 0;              // this run's
    bool               g_built         = false;          // this run's config is the changed one
    std::string        g_why    = "off";
    std::wstring       g_dat;                            // full path of the archive holding the entry
    uint64_t           g_off = 0;                        // the entry's compressed bytes in it
    uint32_t           g_csize = 0, g_usize = 0;
    std::vector<uint8_t> g_block;                        // the changed entry, g_csize bytes
    std::atomic<int>   g_reads{ 0 }, g_patched{ 0 };

    // --- LZ4 block -------------------------------------------------------------------------------------------------------------
    bool lz4_decode(const uint8_t* s, size_t n, std::vector<uint8_t>& d, size_t usize)
    {
        d.assign(usize, 0);
        size_t ip = 0, op = 0;
        while (ip < n) {
            const int tok = s[ip++];
            size_t    lit = tok >> 4;
            if (lit == 15) { uint8_t x; do { if (ip >= n) return false; x = s[ip++]; lit += x; } while (x == 255); }
            if (ip + lit > n || op + lit > usize) return false;
            std::memcpy(&d[op], s + ip, lit);
            ip += lit; op += lit;
            if (ip >= n) break;
            if (ip + 2 > n) return false;
            const size_t off = s[ip] | (s[ip + 1] << 8);
            ip += 2;
            size_t ml = tok & 15;
            if (ml == 15) { uint8_t x; do { if (ip >= n) return false; x = s[ip++]; ml += x; } while (x == 255); }
            ml += 4;
            if (off == 0 || off > op || op + ml > usize) return false;
            for (size_t k = 0; k < ml; ++k, ++op) d[op] = d[op - off];
        }
        return op == usize && ip == n;
    }

    struct Seq { size_t lit_start, lit_len, off, match_len; };   // match_len 0: the last sequence (literals only)

    size_t ext_bytes(size_t v) { return v < 15 ? 0 : (v - 15) / 255 + 1; }
    size_t encoded_size(const std::vector<Seq>& q)
    {
        size_t s = 0;
        for (const auto& x : q) {
            s += 1 + ext_bytes(x.lit_len) + x.lit_len;
            if (x.match_len) s += 2 + ext_bytes(x.match_len - 4);
        }
        return s;
    }
    // Greedy LZ4 (4-byte hash, 64 KB window); the last 5 bytes are literals, no match starts in the last 12.
    std::vector<Seq> lz4_parse(const std::vector<uint8_t>& t)
    {
        std::vector<Seq> q;
        const size_t     n = t.size();
        std::vector<int64_t> head(1 << 16, -1);
        auto h4 = [&](size_t i) { uint32_t v; std::memcpy(&v, &t[i], 4); return (v * 2654435761u) >> 16; };
        size_t anchor = 0, i = 0;
        while (n >= 13 && i + 12 < n) {
            const uint32_t h = h4(i);
            const int64_t  c = head[h];
            head[h]          = int64_t(i);
            if (c >= 0 && i - size_t(c) <= 65535 && std::memcmp(&t[c], &t[i], 4) == 0) {
                size_t len = 4;
                while (i + len + 5 < n && t[c + len] == t[i + len]) ++len;
                q.push_back({ anchor, i - anchor, i - size_t(c), len });
                for (size_t k = i + 1; k < i + len && k + 4 <= n; ++k) head[h4(k)] = int64_t(k);
                i += len;
                anchor = i;
            } else {
                ++i;
            }
        }
        q.push_back({ anchor, n - anchor, 0, 0 });
        return q;
    }
    // Grows the block by exactly one byte: a match of 5-18 bytes gives its last byte to the next sequence's literals (under 14).
    bool grow_one(std::vector<Seq>& q)
    {
        for (size_t i = 0; i + 1 < q.size(); ++i) {
            auto& a = q[i];
            auto& b = q[i + 1];
            if (a.match_len < 5 || a.match_len - 4 > 14 || b.lit_len >= 14) continue;
            a.match_len--;
            b.lit_start--;
            b.lit_len++;
            return true;
        }
        return false;
    }
    std::vector<uint8_t> lz4_write(const std::vector<Seq>& q, const std::vector<uint8_t>& t)
    {
        std::vector<uint8_t> o;
        auto put_len = [&](size_t v) { if (v < 15) return; v -= 15; while (v >= 255) { o.push_back(255); v -= 255; } o.push_back(uint8_t(v)); };
        for (const auto& x : q) {
            const size_t ml = x.match_len ? x.match_len - 4 : 0;
            o.push_back(uint8_t((std::min<size_t>(x.lit_len, 15) << 4) | std::min<size_t>(ml, 15)));
            put_len(x.lit_len);
            o.insert(o.end(), t.begin() + x.lit_start, t.begin() + x.lit_start + x.lit_len);
            if (!x.match_len) break;
            o.push_back(uint8_t(x.off & 0xff));
            o.push_back(uint8_t(x.off >> 8));
            put_len(ml);
        }
        return o;
    }

    // --- the archive entry ----------------------------------------------------------------------------------------------------
    std::vector<uint8_t> read_file(const std::wstring& p, uint64_t off = 0, size_t n = 0)
    {
        std::ifstream f{ p, std::ios::binary };
        if (!f) return {};
        if (!n) { f.seekg(0, std::ios::end); n = size_t(f.tellg()); }
        std::vector<uint8_t> b(n);
        f.seekg(std::streamoff(off));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
        if (size_t(f.gcount()) != n) return {};
        return b;
    }
    bool find_entry(const std::wstring& fat)
    {
        const auto b = read_file(fat);
        if (b.size() < 0x18) return false;
        uint32_t n;
        std::memcpy(&n, &b[0x14], 4);
        for (uint32_t i = 0; i < n && 0x18 + size_t(i + 1) * 20 <= b.size(); ++i) {
            uint32_t w[5];
            std::memcpy(w, &b[0x18 + size_t(i) * 20], 20);
            if ((uint64_t(w[1]) << 32 | w[0]) != kHash) continue;
            if ((w[2] & 3) != 2) { g_why = "the config entry is not LZ4 (scheme " + std::to_string(w[2] & 3) + ")"; return false; }
            g_usize = w[2] >> 2;
            g_off   = uint64_t(w[3]) << 3 | (w[4] >> 29);
            g_csize = w[4] & 0x1fffffff;
            return true;
        }
        return false;
    }

    std::string fmt_scale(float v)
    {
        std::string s = fmt::format("{:.3f}", v);
        while (s.size() > 3 && s.back() == '0') s.pop_back();
        return s;
    }

    // The Geometry presets' LodScale and KillLodScale times s (a preset without them, "high", gets s: the game's default is 1).
    // Returns the changes made, empty when the text is not as expected.
    std::string change_text(std::string& t, float s)
    {
        const size_t g0 = t.find("<Geometry>"), g1 = t.find("</Geometry>");
        if (g0 == std::string::npos || g1 == std::string::npos || g1 < g0) return {};
        std::string geo = t.substr(g0, g1 - g0), log;
        // Each <quality ...> opening tag of the section.
        std::string out;
        size_t      pos = 0;
        while (true) {
            const size_t q = geo.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = geo.find('>', q);
            if (e == std::string::npos) return {};
            out += geo.substr(pos, q - pos);
            std::string tag = geo.substr(q, e - q);   // without '>' (and a closing '/')
            const bool  self_close = !tag.empty() && tag.back() == '/';
            if (self_close) tag.pop_back();
            std::smatch m;
            std::string id = std::regex_search(tag, m, std::regex{ R"re(id="([^"]*)")re" }) ? m[1].str() : "?";
            for (const char* key : { "KillLodScale", "LodScale" }) {
                const std::regex r{ std::string{ "(\\s)" } + key + "=\"([0-9.]+)\"" };
                if (std::regex_search(tag, m, r)) {
                    const float v  = std::strtof(m[2].str().c_str(), nullptr);
                    const auto  nv = fmt_scale(v * s);
                    tag            = m.prefix().str() + m[1].str() + key + "=\"" + nv + "\"" + m.suffix().str();
                    log += fmt::format(" {} {} {} -> {};", id, key, m[2].str(), nv);
                } else {
                    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
                    tag += std::string{ " " } + key + "=\"" + fmt_scale(s) + "\" ";
                    log += fmt::format(" {} {} (1) -> {};", id, key, fmt_scale(s));
                }
            }
            out += tag + (self_close ? "/>" : ">");
            pos = e + 1;
        }
        out += geo.substr(pos);
        t = t.substr(0, g0) + out + t.substr(g1);
        return log;
    }

    // Each <quality ...> tag of <section> that has key="value": the value replaced by fn(old value), which returns the new one
    // (empty: left alone). Returns the changes made; "!" when the section is not there.
    template <class Fn>
    std::string edit_section(std::string& t, const char* section, const char* key, Fn fn)
    {
        const std::string open = std::string{ "<" } + section + ">", close = std::string{ "</" } + section + ">";
        const size_t      s0 = t.find(open), s1 = t.find(close);
        if (s0 == std::string::npos || s1 == std::string::npos || s1 < s0) return "!";
        std::string sec = t.substr(s0, s1 - s0), out, log;
        size_t      pos = 0;
        const std::regex rk{ std::string{ "(\\s)" } + key + "\\s*=\\s*\"([-0-9.]+)\"" };
        while (true) {
            const size_t q = sec.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = sec.find('>', q);
            if (e == std::string::npos) return "!";
            out += sec.substr(pos, q - pos);
            std::string tag = sec.substr(q, e - q);
            std::smatch m;
            const std::string id = std::regex_search(tag, m, std::regex{ R"re(id="([^"]*)")re" }) ? m[1].str() : "?";
            if (std::regex_search(tag, m, rk)) {
                const std::string nv = fn(m[2].str());
                if (!nv.empty() && nv != m[2].str()) {
                    tag = m.prefix().str() + m[1].str() + key + "=\"" + nv + "\"" + m.suffix().str();
                    log += fmt::format(" {} {} {} -> {};", id, key, m[2].str(), nv);
                }
            }
            out += tag + ">";
            pos = e + 1;
        }
        out += sec.substr(pos);
        t = t.substr(0, s0) + out + t.substr(s1);
        return log;
    }

    // Each <quality ...> tag of <section> (cinematic and editor presets left alone): key set to want where the tag has a larger
    // value, or added where the tag has none and add is true (the game's default for it is larger). Returns the changes made; "!"
    // when the section is not there.
    std::string lower_section(std::string& t, const char* section, const char* key, double want, bool add)
    {
        const std::string open = std::string{ "<" } + section + ">", close = std::string{ "</" } + section + ">";
        const size_t      s0 = t.find(open), s1 = t.find(close);
        if (s0 == std::string::npos || s1 == std::string::npos || s1 < s0) return "!";
        std::string      sec = t.substr(s0, s1 - s0), out, log;
        size_t           pos = 0;
        const std::regex rk{ std::string{ "(\\s)" } + key + "\\s*=\\s*\"([-0-9.]+)\"" };
        const std::string wv = fmt::format("{}", want);
        while (true) {
            const size_t q = sec.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = sec.find('>', q);
            if (e == std::string::npos) return "!";
            out += sec.substr(pos, q - pos);
            std::string tag = sec.substr(q, e - q);   // without '>' ('/' kept at the end of a closed tag)
            std::smatch m;
            const std::string id = std::regex_search(tag, m, std::regex{ R"re(id\s*=\s*"([^"]*)")re" }) ? m[1].str() : "?";
            const bool skip = id.rfind("cinematic", 0) == 0 || id == "editor" || id == "off";
            if (!skip && std::regex_search(tag, m, rk)) {
                if (std::strtod(m[2].str().c_str(), nullptr) > want) {
                    tag = m.prefix().str() + m[1].str() + key + "=\"" + wv + "\"" + m.suffix().str();
                    log += fmt::format(" {} {} {} -> {};", id, key, m[2].str(), wv);
                }
            } else if (!skip && add) {
                const bool closed = !tag.empty() && tag.back() == '/';
                if (closed) tag.pop_back();
                while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
                tag += std::string{ " " } + key + "=\"" + wv + "\"" + (closed ? " /" : " ");
                log += fmt::format(" {} {} (default) -> {};", id, key, wv);
            }
            out += tag + ">";
            pos = e + 1;
        }
        out += sec.substr(pos);
        t = t.substr(0, s0) + out + t.substr(s1);
        return log;
    }

    // The perf switches: command, bit, section, key, the value one step down, added where missing (the game's default is larger).
    struct PerfKey { const char* cmd; int bit; const char* section; const char* key; double want; bool add; const char* what; };
    constexpr PerfKey kPerf[] = {
        { "perfleafshadow", 1, "Shadow", "ForceLeafSingleSlice", 0.0, false, "leaves in one shadow cascade" },   // special: set to 1 (below)
        { "perfgrassshadow", 2, "Shadow", "LastGrassShadowCascade", 0.0, false, "grass shadows in the nearest cascade only" },
        { "perflampshadow", 4, "Shadow", "LLSMFrameDynamicUpdates", 1.0, true, "lamp shadow maps updated once per call" },
        { "perfsoftshadow", 8, "Shadow", "PCSSQuality", 1.0, false, "soft shadow filter level 1" },
        { "perfreflect", 16, "Environment", "SSLRMode", 2.0, false, "screen-space reflections mode 2" },
        { "perffog", 32, "VolumetricFog", "VolumetricFogValue", 196.0, false, "volumetric fog 196" },
    };

    // ForceLeafSingleSlice is a flag where 1 is the cheaper one (low and medium): set to 1 where it is 0, added where missing.
    std::string leaf_single_slice(std::string& t)
    {
        const size_t s0 = t.find("<Shadow>"), s1 = t.find("</Shadow>");
        if (s0 == std::string::npos || s1 == std::string::npos || s1 < s0) return "!";
        std::string      sec = t.substr(s0, s1 - s0), out, log;
        size_t           pos = 0;
        const std::regex rk{ R"re((\s)ForceLeafSingleSlice\s*=\s*"([0-9]+)")re" };
        while (true) {
            const size_t q = sec.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = sec.find('>', q);
            if (e == std::string::npos) return "!";
            out += sec.substr(pos, q - pos);
            std::string tag = sec.substr(q, e - q);
            std::smatch m;
            const std::string id = std::regex_search(tag, m, std::regex{ R"re(id\s*=\s*"([^"]*)")re" }) ? m[1].str() : "?";
            if (id.rfind("cinematic", 0) != 0 && id != "editor") {
                if (std::regex_search(tag, m, rk)) {
                    if (m[2].str() != "1") { tag = m.prefix().str() + m[1].str() + "ForceLeafSingleSlice=\"1\"" + m.suffix().str(); log += fmt::format(" {} ForceLeafSingleSlice {} -> 1;", id, m[2].str()); }
                } else {
                    const bool closed = !tag.empty() && tag.back() == '/';
                    if (closed) tag.pop_back();
                    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
                    tag += std::string{ " ForceLeafSingleSlice=\"1\"" } + (closed ? " /" : " ");
                    log += fmt::format(" {} ForceLeafSingleSlice (0) -> 1;", id);
                }
            }
            out += tag + ">";
            pos = e + 1;
        }
        out += sec.substr(pos);
        t = t.substr(0, s0) + out + t.substr(s1);
        return log;
    }

    // Each <quality ...> tag of <section> (cinematic, editor and off presets left alone): key set to value, added where the tag has
    // none. Returns the changes made; "!" when the section is not there.
    std::string set_section(std::string& t, const char* section, const char* key, const std::string& value)
    {
        const std::string open = std::string{ "<" } + section + ">", close = std::string{ "</" } + section + ">";
        const size_t      s0 = t.find(open), s1 = t.find(close);
        if (s0 == std::string::npos || s1 == std::string::npos || s1 < s0) return "!";
        std::string      sec = t.substr(s0, s1 - s0), out, log;
        size_t           pos = 0;
        const std::regex rk{ std::string{ "(\\s)" } + key + "\\s*=\\s*\"([-0-9.]+)\"" };
        while (true) {
            const size_t q = sec.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = sec.find('>', q);
            if (e == std::string::npos) return "!";
            out += sec.substr(pos, q - pos);
            std::string tag = sec.substr(q, e - q);
            std::smatch m;
            const std::string id = std::regex_search(tag, m, std::regex{ R"re(id\s*=\s*"([^"]*)")re" }) ? m[1].str() : "?";
            const bool skip = id.rfind("cinematic", 0) == 0 || id == "editor" || id == "off";
            if (!skip && std::regex_search(tag, m, rk)) {
                if (m[2].str() != value) {
                    tag = m.prefix().str() + m[1].str() + key + "=\"" + value + "\"" + m.suffix().str();
                    log += fmt::format(" {} {} {} -> {};", id, key, m[2].str(), value);
                }
            } else if (!skip) {
                const bool closed = !tag.empty() && tag.back() == '/';
                if (closed) tag.pop_back();
                while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
                tag += std::string{ " " } + key + "=\"" + value + "\"" + (closed ? " /" : " ");
                log += fmt::format(" {} {} (default) -> {};", id, key, value);
            }
            out += tag + ">";
            pos = e + 1;
        }
        out += sec.substr(pos);
        t = t.substr(0, s0) + out + t.substr(s1);
        return log;
    }

    // "a/b/c" with 0.5 <= a < b < c <= 200 (metres), written back as the values will be given; "off" for anything else.
    std::string parse_ranges(const std::string& arg)
    {
        if (arg.empty() || arg == "off") return "off";
        float r[3]{};
        if (std::sscanf(arg.c_str(), "%f/%f/%f", &r[0], &r[1], &r[2]) != 3) return "off";
        if (!(r[0] >= 0.5f && r[0] < r[1] && r[1] < r[2] && r[2] <= 200.0f)) return "off";
        return fmt::format("{:g}/{:g}/{:g}", r[0], r[1], r[2]);
    }

    std::string perf_names(int bits)
    {
        std::string s;
        for (const auto& k : kPerf) if (bits & k.bit) s += (s.empty() ? "" : ", ") + std::string{ k.what };
        return s.empty() ? std::string{ "off" } : s;
    }

    // lodfov, shadowres, vtbias and the perf switches, those set. Returns the changes made, empty with g_why set when one could not be made.
    std::string change_all(std::string& t, float s, int shadow, float vt, int perf, const std::string& ranges)
    {
        std::string log;
        if (s > 0.0f && std::abs(s - 1.0f) >= 1e-3f) {
            const std::string c = change_text(t, s);
            if (c.empty()) { g_why = "the config's Geometry section is not as expected"; return {}; }
            log += c;
        }
        if (shadow > 0) {
            const std::string c = edit_section(t, "Shadow", "CascadedShadowMapSize", [&](const std::string&) { return std::to_string(shadow); });
            if (c == "!" || c.empty()) { g_why = "the config's Shadow section has no CascadedShadowMapSize to change"; return {}; }
            log += c;
        }
        if (vt != 0.0f) {
            const std::string c = edit_section(t, "Terrain", "VirtualTextureLODBias",
                                               [&](const std::string& v) { return fmt::format("{:.2f}", std::strtof(v.c_str(), nullptr) + vt); });
            if (c == "!" || c.empty()) { g_why = "the config's Terrain section has no VirtualTextureLODBias to change"; return {}; }
            log += c;
        }
        if (ranges != "off") {
            float r[3]{};
            if (std::sscanf(ranges.c_str(), "%f/%f/%f", &r[0], &r[1], &r[2]) != 3) { g_why = "sunranges did not parse"; return {}; }
            for (int k = 0; k < 3; ++k) {
                const std::string key = fmt::format("SunShadowRange{}", k);
                const std::string c   = set_section(t, "Shadow", key.c_str(), fmt::format("{:g}", r[k]));
                if (c == "!") { g_why = "the config has no Shadow section (sunranges)"; return {}; }
                log += c;
            }
        }
        for (const auto& k : kPerf) {
            if (!(perf & k.bit)) continue;
            const std::string c = k.bit == 1 ? leaf_single_slice(t) : lower_section(t, k.section, k.key, k.want, k.add);
            if (c == "!") { g_why = fmt::format("the config has no {} section ({})", k.section, k.cmd); return {}; }
            // Nothing to change (already at that step or below) is not a failure: the switch then does nothing.
            log += c.empty() ? fmt::format(" {}: nothing to change;", k.cmd) : c;
        }
        return log;
    }

    // Comment text replaced by spaces (they compress to almost nothing); then spaces added or removed in the longest comment so
    // the text keeps its size.
    bool fit_text(std::string& t, size_t want)
    {
        size_t best = 0, best_len = 0;
        for (size_t p = 0; (p = t.find("<!--", p)) != std::string::npos;) {
            const size_t e = t.find("-->", p + 4);
            if (e == std::string::npos) return false;
            for (size_t k = p + 4; k < e; ++k) if (t[k] != '\n' && t[k] != '\r') t[k] = ' ';
            if (e - p > best_len) { best = p + 4; best_len = e - p - 4; }
            p = e + 3;
        }
        if (t.size() > want) {
            const size_t d = t.size() - want;
            if (best_len < d + 2) return false;
            t.erase(best + 1, d);
        } else if (t.size() < want) {
            t.insert(best + 1, want - t.size(), ' ');
        }
        return t.size() == want;
    }

    bool build(float s, int shadow, float vt, int perf, const std::string& ranges)
    {
        const std::wstring dir = std::filesystem::path{ fc5::exe_dir() }.parent_path().parent_path().wstring() + L"\\data_final\\pc\\";
        // patch.dat would override common.dat: the entry is looked for there first.
        for (const wchar_t* a : { L"patch", L"common" }) {
            if (find_entry(dir + a + L".fat")) { g_dat = dir + a + L".dat"; break; }
        }
        if (g_dat.empty()) { if (g_why == "off") g_why = "the config entry was not found in patch.fat or common.fat"; return false; }
        const auto comp = read_file(g_dat, g_off, g_csize);
        std::vector<uint8_t> plain;
        if (comp.size() != g_csize || !lz4_decode(comp.data(), comp.size(), plain, g_usize)) { g_why = "the config entry did not decompress"; return false; }
        std::string t{ plain.begin(), plain.end() };
        const std::string changes = change_all(t, s, shadow, vt, perf, ranges);
        if (changes.empty()) return false;
        if (!fit_text(t, g_usize)) { g_why = "no room in the config's comments"; return false; }
        std::vector<uint8_t> nt{ t.begin(), t.end() };
        auto q = lz4_parse(nt);
        size_t sz = encoded_size(q);
        if (sz > g_csize) { g_why = fmt::format("the changed config compresses to {} bytes, more than the {} available", sz, g_csize); return false; }
        while (sz < g_csize && grow_one(q)) sz++;
        if (sz != g_csize) { g_why = fmt::format("could not size the changed config to {} bytes ({})", g_csize, sz); return false; }
        g_block = lz4_write(q, nt);
        std::vector<uint8_t> check;
        if (g_block.size() != g_csize || !lz4_decode(g_block.data(), g_block.size(), check, g_usize) || check != nt) { g_why = "the changed config did not check out"; g_block.clear(); return false; }
        g_why = fmt::format("lodfov {}, shadowres {}, vtbias {}, perf {}, sunranges {} ({}):{}", s > 0.0f ? "x" + fmt_scale(s) : std::string{ "off" },
                            shadow > 0 ? std::to_string(shadow) : std::string{ "off" }, vt != 0.0f ? fmt::format("{:+.2f}", vt) : std::string{ "off" },
                            perf_names(perf), ranges, std::filesystem::path{ g_dat }.filename().string(), changes);
        return true;
    }

    // --- the read --------------------------------------------------------------------------------------------------------------
    using NtReadFile_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    NtReadFile_t g_read_orig = nullptr;                  // NtReadFile as it was (hook_read's copy of its first 8 bytes, then back to it)

    bool is_our_file(HANDLE h)
    {
        wchar_t p[MAX_PATH * 2]{};
        const DWORD n = GetFinalPathNameByHandleW(h, p, DWORD(std::size(p)), FILE_NAME_NORMALIZED);
        if (!n || n >= std::size(p)) return false;
        const std::wstring a{ p }, b = std::filesystem::path{ g_dat }.filename().wstring();
        return a.size() >= b.size() && _wcsicmp(a.c_str() + a.size() - b.size(), b.c_str()) == 0;
    }
    void patch(uint8_t* buf, uint64_t off, uint64_t len)
    {
        const uint64_t a = std::max(off, g_off), b = std::min(off + len, g_off + g_csize);
        if (a >= b) return;
        std::memcpy(buf + (a - off), g_block.data() + (a - g_off), size_t(b - a));
        const int n = ++g_patched;
        spdlog::info("[LodFov] the game read its render config ({} bytes at {} of {} in a {}-byte read): the changed copy given ({} so far); {}", b - a, a,
                     std::filesystem::path{ g_dat }.filename().string(), len, n, g_why);
    }

    NTSTATUS NTAPI hk_read(HANDLE h, HANDLE ev, PVOID apc, PVOID apc_ctx, PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len, PLARGE_INTEGER at, PULONG key)
    {
        // The read's place: the given offset, else the file's own position.
        uint64_t off = 0;
        bool     known = false;
        if (at && at->QuadPart >= 0) { off = uint64_t(at->QuadPart); known = true; }
        else {
            LARGE_INTEGER zero{}, cur{};
            if (SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) { off = uint64_t(cur.QuadPart); known = true; }
        }
        const bool hit = known && off < g_off + g_csize && off + len > g_off && is_our_file(h);
        const NTSTATUS st = g_read_orig(h, ev, apc, apc_ctx, iosb, buf, len, at, key);
        if (!hit) return st;
        const int n = ++g_reads;
        if (st == STATUS_PENDING) {
            // Asynchronous: the bytes land later; the changed copy goes in as soon as the read is done (the IO status changes).
            spdlog::info("[LodFov] read {} of the render config is asynchronous ({} bytes at {}): waiting for it", n, len, off);
            for (int i = 0; i < 2000000 && iosb->Status == STATUS_PENDING; ++i) YieldProcessor();
            if (iosb->Status == STATUS_PENDING) { spdlog::warn("[LodFov] the read did not finish: the game's own config is used"); return st; }
        }
        if (NT_OK(st) || st == STATUS_PENDING) patch(static_cast<uint8_t*>(buf), off, len);
        else spdlog::info("[LodFov] read {} of the render config failed ({:#x})", n, uint32_t(st));
        return st;
    }

    // --- the hook on NtReadFile ------------------------------------------------------------------------------------------------
    // ntdllhook (Beta 0.4 player report, 7 Nov 2026, research\2026-11-07_beta04_ntdll_crash.md): this used safetyhook's inline hook,
    // which makes the target's page read-write and NOT executable while it writes, and sends every thread that runs into that page
    // through its exception handler. NtReadFile's page in ntdll also holds the system calls every thread makes all the time
    // (NtWaitForSingleObject, NtClose, NtSetEvent, NtWriteFile ...), and the game and Ubisoft Connect start their threads at that
    // moment. On one player's PC (Windows 10 22H2 19045, Pimax runtime) the game died in ntdll within 2 ms of the hook on two starts
    // in a row, once with a DEP fault (code run on a page that was not executable) in ntdll's system call stubs, with no mod report
    // (our report thread waits in that same page). Release defaults (shadowres 2048, mipbias -0.5) made this hook run for everyone.
    // Now no page is ever made non-executable: the stub starts 4C 8B D1 (mov r10,rcx) B8 imm32 (mov eax,n) at an 8-byte aligned
    // address, and one aligned 8-byte write changes only its second instruction into E9 rel32 (jmp to our thunk). Any thread is at
    // +0, +3 or +8 of it; before and after the write each of those starts a whole instruction, so no thread can run half of the old
    // bytes and half of the new. A stub that is not as expected (another program's hook, another Windows) is left alone: the game's
    // own render config. The hook stays for the whole run (never taken off, so nothing is patched at exit either).
    uint8_t* alloc_near(uint8_t* target, size_t size)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
        const uintptr_t lo   = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t hi   = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        const uintptr_t t    = reinterpret_cast<uintptr_t>(target) & ~(gran - 1);
        for (uintptr_t d = gran; d < 0x7FF00000ull; d += gran) {
            const uintptr_t below = t > d ? t - d : 0, above = t + d;
            for (const uintptr_t a : { below, above }) {
                if (a < lo || a + size > hi) continue;
                if (void* m = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return static_cast<uint8_t*>(m);
            }
        }
        return nullptr;
    }

    bool hook_read(std::string& why)
    {
        auto* p = reinterpret_cast<uint8_t*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtReadFile"));
        if (!p) { why = "no NtReadFile"; return false; }
        if ((reinterpret_cast<uintptr_t>(p) & 7) != 0 || p[0] != 0x4C || p[1] != 0x8B || p[2] != 0xD1 || p[3] != 0xB8) {
            why = fmt::format("NtReadFile does not start as expected ({:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} at {}): another program may have "
                              "changed it", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], fmt::ptr(p));
            return false;
        }
        uint8_t* m = alloc_near(p, 64);
        if (!m) { why = "no free memory within 2 GB of ntdll for the hook"; return false; }
        const int64_t rel = int64_t(reinterpret_cast<uintptr_t>(m)) - int64_t(reinterpret_cast<uintptr_t>(p + 8));
        if (rel < INT32_MIN || rel > INT32_MAX) { VirtualFree(m, 0, MEM_RELEASE); why = "the hook's memory is too far from ntdll"; return false; }
        // m: jmp [rip+0] -> hk_read. m+16: NtReadFile's own mov r10,rcx; mov eax,n, then jmp [rip+0] -> NtReadFile+8 (its system call).
        static const uint8_t kJmpAbs[6]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        const uintptr_t      to_hook = reinterpret_cast<uintptr_t>(&hk_read), back = reinterpret_cast<uintptr_t>(p + 8);
        std::memset(m, 0xCC, 64);
        std::memcpy(m, kJmpAbs, 6);
        std::memcpy(m + 6, &to_hook, 8);
        std::memcpy(m + 16, p, 8);
        std::memcpy(m + 24, kJmpAbs, 6);
        std::memcpy(m + 30, &back, 8);
        DWORD old = 0;
        if (!VirtualProtect(m, 64, PAGE_EXECUTE_READ, &old)) { VirtualFree(m, 0, MEM_RELEASE); why = "the hook's memory could not be made executable"; return false; }
        FlushInstructionCache(GetCurrentProcess(), m, 64);
        g_read_orig = reinterpret_cast<NtReadFile_t>(m + 16);   // before the write: a thread may enter hk_read right after it
        LONG64 before = 0, after = 0;
        std::memcpy(&before, p, 8);
        uint8_t       nb[8]{ p[0], p[1], p[2], 0xE9 };
        const int32_t r32 = int32_t(rel);
        std::memcpy(nb + 4, &r32, 4);
        std::memcpy(&after, nb, 8);
        // Executable all along (read-write-execute for the write): no thread ever faults on this page.
        if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) { why = fmt::format("NtReadFile's page could not be made writable ({})", GetLastError()); return false; }
        const LONG64 seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), after, before);
        DWORD        tmp  = 0;
        VirtualProtect(p, 8, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), p, 8);
        if (seen != before) { why = "NtReadFile changed while it was being hooked"; return false; }
        return true;
    }

    float parse(const std::string& arg)
    {
        if (arg == "on") return kOn;
        if (arg == "off" || arg.empty()) return 0.0f;
        const float v = std::strtof(arg.c_str(), nullptr);
        return (v > 0.05f && v < 1.5f) ? v : 0.0f;
    }
    int parse_shadow(const std::string& arg)
    {
        if (arg == "off" || arg.empty()) return 0;
        const int v = std::atoi(arg.c_str());
        return (v >= 512 && v <= 8192) ? v : 0;
    }
    float parse_vt(const std::string& arg)
    {
        if (arg == "off" || arg.empty()) return 0.0f;
        const float v = std::strtof(arg.c_str(), nullptr);
        return std::abs(v) < 0.01f ? 0.0f : std::clamp(v, -3.0f, 3.0f);
    }
}

void install_early()
{
    // The saved line (the settings are applied later than the game reads its config).
    // texdetail (author 6 Nov 2026): the ground's bias is the saved
    // "mipbias" line, F11 Display "Texture detail" now setting both. A "vtbias" line left from before is migrated: the finer of the two.
    // releasedefaults (author 6 Nov 2026): the release defaults (FarCry5Defaults.h: shadowres 2048, mipbias -0.5) first, then
    // fc5vr_settings.txt over them line by line, as the settings replay does; before, a file without those lines (a first start, a
    // file from an older release) left the game's own render config here while the replay used the defaults.
    std::stringstream f;
    f << fc5::kReleaseSettings << '\n';
    if (std::ifstream user{ fc5::mod_file("fc5vr_settings.txt") }; user) f << user.rdbuf();
    float         vt_mip = 0.0f, vt_old = 0.0f;
    for (std::string line; std::getline(f, line);) {
        std::istringstream ls{ line };
        std::string        cmd, arg;
        ls >> cmd >> arg;
        if (cmd == "lodfov") g_scale = parse(arg);
        else if (cmd == "shadowres") g_shadow_res = parse_shadow(arg);
        else if (cmd == "sunranges") { std::lock_guard lk{ g_ranges_mx }; g_ranges = parse_ranges(arg); }
        else if (cmd == "mipbias") vt_mip = parse_vt(arg);
        else if (cmd == "vtbias") vt_old = parse_vt(arg);
        else for (const auto& k : kPerf) if (cmd == k.cmd) g_perf = arg == "on" ? (g_perf.load() | k.bit) : (g_perf.load() & ~k.bit);
    }
    g_vt_bias = std::min(vt_mip, vt_old);
    float       s  = g_scale.load();
    const int   sh = g_shadow_res.load();
    const float vt = g_vt_bias.load();
    const int   pf = g_perf.load();
    if (s <= 0.0f || std::abs(s - 1.0f) < 1e-3f) s = 0.0f;
    std::string rg;
    { std::lock_guard lk{ g_ranges_mx }; rg = g_ranges; }
    if (s <= 0.0f && sh <= 0 && vt == 0.0f && pf == 0 && rg == "off") { spdlog::info("[LodFov] lodfov, shadowres, vtbias, sunranges and the perf switches off: the game's own render config"); return; }
    if (!build(s, sh, vt, pf, rg)) {
        spdlog::warn("[LodFov] lodfov {} / shadowres {} / vtbias {:+.2f} / perf {} / sunranges {}: not used, {}", fmt_scale(s), sh, vt, perf_names(pf), rg, g_why);
        if (rg == "off") return;
        // sunranges is a test: without it the other settings still get their changed config.
        if (s <= 0.0f && sh <= 0 && vt == 0.0f && pf == 0) return;
        if (!build(s, sh, vt, pf, "off")) { spdlog::warn("[LodFov] without sunranges: not used either, {}", g_why); return; }
        spdlog::info("[LodFov] built without sunranges");
    } else {
        g_ranges_active = rg;
    }
    if (std::string hw; !hook_read(hw)) { spdlog::warn("[LodFov] the read hook was not made ({}): not used, the game's own render config", hw); return; }
    g_active        = s;
    g_shadow_active = sh;
    g_vt_active     = vt;
    g_perf_active   = pf;
    g_built         = true;
    spdlog::info("[LodFov] lodfov {}: changed render config ready ({} bytes at {} in {}, {} bytes of text): {}", fmt_scale(s), g_csize, g_off,
                 std::filesystem::path{ g_dat }.filename().string(), g_usize, g_why);
}

void set(const std::string& arg)
{
    g_scale = parse(arg);
    spdlog::info("[LodFov] lodfov {} saved: used from the next game start (this run: {})", g_scale.load() > 0.0f ? fmt_scale(g_scale.load()) : "off", describe());
}

float scale() { return g_scale.load(); }

void set_shadow_res(const std::string& arg)
{
    g_shadow_res = parse_shadow(arg);
    spdlog::info("[LodFov] shadowres {} saved: used from the next game start (this run: {})", g_shadow_res.load() > 0 ? std::to_string(g_shadow_res.load()) : "off",
                 g_shadow_active > 0 ? std::to_string(g_shadow_active) : "the game's");
}
int shadow_res() { return g_shadow_res.load(); }

void set_sun_ranges(const std::string& arg)
{
    const std::string r = parse_ranges(arg);
    { std::lock_guard lk{ g_ranges_mx }; g_ranges = r; }
    spdlog::info("[LodFov] sunranges {} saved: used from the next game start (this run: {})", r, g_ranges_active);
}
std::string sun_ranges()
{
    std::lock_guard lk{ g_ranges_mx };
    return g_ranges;
}
std::string sun_ranges_active() { return g_ranges_active == "off" ? std::string{ "the game's (4 / 11 / 30 m)" } : g_ranges_active + " m"; }
int shadow_res_active() { return g_shadow_active; }

void set_vt_bias(const std::string& arg)
{
    g_vt_bias = parse_vt(arg);
    spdlog::info("[LodFov] vtbias {:+.2f} saved: used from the next game start (this run: {:+.2f})", g_vt_bias.load(), g_vt_active);
}
float vt_bias() { return g_vt_bias.load(); }
float vt_bias_active() { return g_vt_active; }

bool perf_command(const std::string& cmd, const std::string& arg)
{
    for (const auto& k : kPerf) {
        if (cmd != k.cmd) continue;
        if (arg == "on") g_perf = g_perf.load() | k.bit;
        else if (arg == "off" || arg.empty()) g_perf = g_perf.load() & ~k.bit;
        else return true;
        spdlog::info("[LodFov] {} {} saved ({}): used from the next game start (this run: {})", k.cmd, (g_perf.load() & k.bit) ? "on" : "off", k.what,
                     (g_perf_active & k.bit) ? "on" : "off");
        return true;
    }
    return false;
}
int perf() { return g_perf.load(); }
int perf_active() { return g_perf_active; }
int perf_bit(const char* cmd)
{
    for (const auto& k : kPerf) if (std::strcmp(cmd, k.cmd) == 0) return k.bit;
    return 0;
}

std::string describe()
{
    if (!g_built) return "off";
    std::string s = g_active > 0.0f ? "x" + fmt_scale(g_active) : std::string{ "LOD off" };
    if (g_shadow_active > 0) s += fmt::format(", sun shadow maps {}", g_shadow_active);
    if (g_vt_active != 0.0f) s += fmt::format(", ground texture bias {:+.2f}", g_vt_active);
    if (g_perf_active != 0) s += ", " + perf_names(g_perf_active);
    if (g_ranges_active != "off") s += ", sun cascades end at " + g_ranges_active + " m";
    return fmt::format("{}, given to the game {} times", s, g_patched.load());
}
}
