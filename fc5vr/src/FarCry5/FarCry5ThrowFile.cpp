// throwfile (author 3 Nov; research\2026-11-03_throwable_models.md): a thrown item's first-person model and textures, read from the player's own game
// files. Nothing of the game's is shipped: the table below holds only names.
//
// An archive is data_final\pc\<name>.fat + .dat. FAT2, version 10: entry count at 0x14, 20-byte entries from 0x18: u32 name high, u32 name low,
// u32 (size << 2 | scheme), u32 offset high, u32 (offset low << 29 | stored size). Scheme 0 is stored, else one LZ4 block. The name is CRC-64
// (reflected polynomial 0xD800000000000000, no initial value or final xor) of the lower-case path with backslashes. patch wins over installpkg.
//
// A model (.xbg, HSEM) is a run of chunks: magic, version, size, data size, children. RMTL names its materials, PCMP and UCMP give the position
// and texture scales, LODS holds its levels, the first the most detailed. A material (TAM) names its textures; a texture (.xbt, TBX) is a DDS at
// the offset in its header, and its header's byte 0x10 is 1 where it is read as sRGB. A material's values: u32 CRC-32 of the constant's name, 34 58 77 1D, a size byte (or FE and a u32
// distance back from that byte to the same size and bytes stored earlier), the bytes.
//
// An "opaquealpha" material (pipe bomb, dynamite): the game's shader for it cuts holes where the colour texture's alpha is low. The shader this draws with has no alpha test, so the
// holes are cut in the mesh here instead: triangles wholly below 0.5 are dropped, those on an edge are split up to three times, and a piece is kept by the alpha at its middle.
#include "FarCry5ThrowFile.h"

#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace fc5::throwfile
{
    namespace
    {
        // Item ids: research\weapondata\throwables.csv. Models: research\2026-11-02_bonemap_models.txt, their hashes from tools\gamestate\throwmodels.py.
        // look: a model whose Weapon material stands in for a material of this one that is not the Weapon kind (load).
        struct ItemModel { uint64_t item; uint64_t file; const char* name; uint64_t look{ 0 }; };
        constexpr uint64_t  kDogBone = 0x22BC2C58D6C52FF8;   // graphics\weapons\personalweapons\throwables\wpn_dog_bone\gp_dog_bone.xbg: an opaque, non-metal Weapon material
        constexpr ItemModel kItems[]{
            { 0x00200739732adedc, 0x63BF446E7E3A0C3B, "WPN_Grenade" },
            { 0x00200756db498c6b, 0x7DA24ED195B5AECE, "WPN_Grenade_02" },
            { 0x00200739732e0f9b, 0xA13258326FED53A1, "WPN_Molotov" },
            { 0x002007479aaa1868, 0xEC4274DAC93AFE9B, "WPN_Dynamite" },
            { 0x00200758bc0015a5, 0xE068D12D999A5D52, "WPN_PIPEBOMB" },
            { 0x002007583c1932e0, 0xF410E738BABFA5AB, "WPN_C4" },
            { 0x0020075973c8a32d, 0x250C76D9FAB66FAC, "WPN_Proximity_Mine" },
            { 0x0020073973355f29, 0xAADDC8B3D7A5E1A8, "WPN_Throwing_Knife" },   // (not WPN_HookKnife, which also has 2502 indices but its length on another axis: author 4 Nov. The game's own draw has this file's scales.)
            { 0x00200739732e8e7d, 0xB4A1FDC2425724E0, "WPN_Rock" },
            { 0x002007542aaf7525, 0x7923F9B3FD8A9C40, "WPN_CAN" },
            { 0x00200749e40ea2ab, 0x160A0B9F49A32181, "WPN_Dog_Bone" },
            // Bait (author 5 Nov), Bait++ and Bait_UNI59: all three entities name graphics\weapons\personalweapons\throwables\bait\gp_bait_01.xbg. Its material is Character_Skin_Cinematic.
            { 0x00200739733262ef, 0xFF17C3EFE83ACA9A, "GP_Bait_01", kDogBone },
            { 0x0020074b43c18333, 0xFF17C3EFE83ACA9A, "GP_Bait_01", kDogBone },
            { 0x00200753336211c8, 0xFF17C3EFE83ACA9A, "GP_Bait_01", kDogBone },
        };

        // The Weapon constant buffer (a compiled shader's reflection in patchshadersobj: the same 62 constants in every shader that has it): CRC-32 of the name, offset, size.
        struct WeaponVar { uint32_t crc; uint16_t off, size; };
        constexpr uint32_t  kWeaponCbSize = 528;
        constexpr WeaponVar kWeaponVars[]{
            { 0xEE205BA5u, 0, 16 },   // BaseColor
            { 0x5289A81Du, 16, 16 },   // BaseColor0
            { 0x258E988Bu, 32, 16 },   // BaseColor1
            { 0xBC87C931u, 48, 16 },   // BaseColor2
            { 0xCB80F9A7u, 64, 16 },   // BaseColor3
            { 0x967E6AFDu, 80, 16 },   // DirtBaseColor
            { 0xBBDC137Du, 96, 16 },   // FluidBaseColor
            { 0x4AE0C24Au, 112, 16 },   // FluidMaterialProperties
            { 0x97602DFDu, 128, 16 },   // MaterialPickingID
            { 0x923D3EDAu, 144, 16 },   // MaterialProperties0
            { 0xE53A0E4Cu, 160, 16 },   // MaterialProperties1
            { 0x7C335FF6u, 176, 16 },   // MaterialProperties2
            { 0x0B346F60u, 192, 16 },   // MaterialProperties3
            { 0x34AEF3A7u, 208, 16 },   // PatternColor
            { 0x02565789u, 224, 16 },   // PatternColorB
            { 0x723CA306u, 240, 16 },   // PatternColorG
            { 0x1FE147EDu, 256, 16 },   // PatternColorR
            { 0xE72F3534u, 272, 16 },   // PatternRotation
            { 0x4A006C47u, 288, 16 },   // RustBaseColor
            { 0x3622A8B7u, 304, 16 },   // RustMaterialProperties
            { 0xC83C5EFCu, 320, 8 },   // PatternOffset
            { 0x3A3B65A2u, 328, 8 },   // PatternTiling
            { 0xACA1F4B8u, 336, 8 },   // Tiling
            { 0x31CD456Bu, 344, 8 },   // Tiling0
            { 0x46CA75FDu, 352, 8 },   // Tiling1
            { 0xDFC32447u, 360, 8 },   // Tiling2
            { 0xA8C414D1u, 368, 8 },   // Tiling3
            { 0x8692C2F2u, 376, 4 },   // AlphaOffset
            { 0x1B26088Du, 380, 4 },   // ColorMode0
            { 0x6C21381Bu, 384, 4 },   // ColorMode1
            { 0xF52869A1u, 388, 4 },   // ColorMode2
            { 0x822F5937u, 392, 4 },   // ColorMode3
            { 0xE214165Fu, 396, 4 },   // DirtMaskOffset
            { 0xF9DDA931u, 400, 4 },   // DirtMaskScale
            { 0x6C72CA31u, 404, 4 },   // DirtSmoothness
            { 0x0F813706u, 408, 4 },   // FluidMaskOffset
            { 0xD47FD0B1u, 412, 4 },   // FluidMaskScale
            { 0x81E7EB68u, 416, 4 },   // FluidSmoothness
            { 0x954BA1ECu, 420, 4 },   // MaterialIndex0
            { 0xE24C917Au, 424, 4 },   // MaterialIndex1
            { 0x7B45C0C0u, 428, 4 },   // MaterialIndex2
            { 0x0C42F056u, 432, 4 },   // MaterialIndex3
            { 0x5B5E4DD8u, 436, 4 },   // NormalIndex0
            { 0x2C597D4Eu, 440, 4 },   // NormalIndex1
            { 0xB5502CF4u, 444, 4 },   // NormalIndex2
            { 0xC2571C62u, 448, 4 },   // NormalIndex3
            { 0x7EA1DBAAu, 452, 4 },   // PatternAlphaOffset
            { 0x4516F5F5u, 456, 4 },   // PatternAlphaScale
            { 0x7519ECCBu, 460, 4 },   // PatternArrayIndex
            { 0xDA560C32u, 464, 4 },   // PatternArrayModeEnabled
            { 0x90B0D430u, 468, 4 },   // PatternChannel
            { 0x07933F18u, 472, 4 },   // PatternMode
            { 0xF7895503u, 476, 4 },   // PatternSampling
            { 0xD49C6542u, 480, 4 },   // PatternUVChannel
            { 0xC97C32CBu, 484, 4 },   // RustMaskOffset
            { 0x25A3AF8Bu, 488, 4 },   // RustMaskScale
            { 0x471AEEA5u, 492, 4 },   // RustSmoothness
            { 0x8401CF4Eu, 496, 4 },   // SkinMaskOffset0
            { 0xF306FFD8u, 500, 4 },   // SkinMaskOffset1
            { 0x6A0FAE62u, 504, 4 },   // SkinMaskOffset2
            { 0x1D089EF4u, 508, 4 },   // SkinMaskOffset3
            { 0x61C9C582u, 512, 4 },   // SkinMaskScale
        };

        constexpr size_t kMaxFile = 64u * 1024 * 1024;

        struct Entry { uint64_t off; uint32_t stored, size, scheme; };
        struct Archive
        {
            std::filesystem::path                  dat;
            std::unordered_map<uint64_t, Entry>    e;
        };
        std::mutex           g_mx;
        std::vector<Archive> g_arch;
        bool                 g_opened{ false };

        std::filesystem::path pc_folder()
        {
            wchar_t p[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
            return std::filesystem::path{ p }.parent_path().parent_path() / L"data_final" / L"pc";   // bin\FarCry5.exe
        }

        bool read_all(const std::filesystem::path& f, std::vector<uint8_t>& out)
        {
            std::ifstream s{ f, std::ios::binary };
            if (!s) return false;
            s.seekg(0, std::ios::end);
            const auto n = s.tellg();
            s.seekg(0);
            if (n <= 0) return false;
            out.resize(size_t(n));
            s.read(reinterpret_cast<char*>(out.data()), n);
            return bool(s);
        }

        // The archives, once (the indexes stay in memory: about 16 MB).
        void open_archives()
        {
            if (g_opened) return;
            g_opened = true;
            const auto root = pc_folder();
            for (const wchar_t* n : { L"patch", L"worlds\\installpkg" }) {
                std::vector<uint8_t> b;
                Archive              a;
                const auto           base = root / n;
                auto                 fat  = base;
                fat += L".fat";
                a.dat = base;
                a.dat += L".dat";
                if (!read_all(fat, b) || b.size() < 0x18 || std::memcmp(b.data(), "FAT2", 4) != 0 && std::memcmp(b.data(), "2TAF", 4) != 0) {
                    spdlog::info("[ThrowFile] archive {} not read", fat.string());
                    continue;
                }
                uint32_t count = 0;
                std::memcpy(&count, b.data() + 0x14, 4);
                if (size_t(count) * 20 + 0x18 > b.size()) {
                    spdlog::info("[ThrowFile] archive {}: index too short ({} entries, {} bytes)", fat.string(), count, b.size());
                    continue;
                }
                a.e.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t v[5];
                    std::memcpy(v, b.data() + 0x18 + size_t(i) * 20, 20);
                    a.e[(uint64_t(v[0]) << 32) | v[1]] = Entry{ (uint64_t(v[3]) << 3) | (v[4] >> 29), v[4] & 0x1FFFFFFFu, v[2] >> 2, v[2] & 3u };
                }
                spdlog::info("[ThrowFile] archive {}: {} entries", fat.string(), count);
                g_arch.push_back(std::move(a));
            }
        }

        bool lz4(const uint8_t* s, size_t n, uint8_t* d, size_t usize)
        {
            size_t ip = 0, op = 0;
            while (ip < n) {
                const uint8_t tok = s[ip++];
                size_t        lit = tok >> 4;
                if (lit == 15) {
                    uint8_t x;
                    do {
                        if (ip >= n) return false;
                        x = s[ip++];
                        lit += x;
                    } while (x == 255);
                }
                if (ip + lit > n || op + lit > usize) return false;
                std::memcpy(d + op, s + ip, lit);
                ip += lit;
                op += lit;
                if (ip >= n || op >= usize) break;
                if (ip + 2 > n) return false;
                const size_t off = size_t(s[ip]) | (size_t(s[ip + 1]) << 8);
                ip += 2;
                size_t ml = tok & 15;
                if (ml == 15) {
                    uint8_t x;
                    do {
                        if (ip >= n) return false;
                        x = s[ip++];
                        ml += x;
                    } while (x == 255);
                }
                ml += 4;
                if (off == 0 || off > op || op + ml > usize) return false;
                for (size_t k = 0; k < ml; ++k, ++op) d[op] = d[op - off];
            }
            return op == usize;
        }

        bool read_file(uint64_t h, std::vector<uint8_t>& out)
        {
            std::lock_guard _{ g_mx };
            open_archives();
            for (const auto& a : g_arch) {
                const auto it = a.e.find(h);
                if (it == a.e.end()) continue;
                const Entry& e = it->second;
                if (e.size > kMaxFile || e.stored > kMaxFile) return false;
                std::ifstream s{ a.dat, std::ios::binary };
                if (!s) return false;
                s.seekg(std::streamoff(e.off));
                std::vector<uint8_t> c(e.stored);
                s.read(reinterpret_cast<char*>(c.data()), std::streamsize(c.size()));
                if (!s) return false;
                if (e.scheme == 0) {
                    out = std::move(c);
                    return true;
                }
                out.resize(e.size);
                return lz4(c.data(), c.size(), out.data(), out.size());
            }
            return false;
        }

        struct Chunk { size_t off{ 0 }, size{ 0 }; };
        // A top-level chunk of a model: its magic reads backwards in the file (LODS is "SDOL").
        Chunk find_chunk(const std::vector<uint8_t>& b, const char* name)
        {
            char want[4]{ name[3], name[2], name[1], name[0] };
            for (size_t off = 0x20; off + 20 <= b.size();) {
                uint32_t size = 0;
                std::memcpy(&size, b.data() + off + 8, 4);
                if (size < 20 || off + size > b.size()) break;
                if (std::memcmp(b.data() + off, want, 4) == 0) return Chunk{ off, size };
                off += size;
            }
            return {};
        }

        // Every "graphics\....<ext>" in a block of a file, in order.
        std::vector<std::string> paths_in(const uint8_t* p, size_t n, const char* ext)
        {
            std::vector<std::string> out;
            const size_t             el = std::strlen(ext);
            auto                     lc = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
            for (size_t i = 0; i + 9 < n; ++i) {
                if (lc(char(p[i])) != 'g' || std::memcmp(p + i + 1, "raphics", 7) != 0 || (p[i + 8] != '\\' && p[i + 8] != '/')) continue;
                size_t e = i + 9;
                while (e < n && p[e] >= 0x20 && p[e] < 0x7F) ++e;
                std::string s(reinterpret_cast<const char*>(p + i), e - i);
                std::string l = s;
                std::transform(l.begin(), l.end(), l.begin(), lc);
                const size_t at = l.find(ext);
                if (at != std::string::npos) out.push_back(s.substr(0, at + el));
                i = e;
            }
            return out;
        }

        // The Weapon constants a material gives; the rest stay zero.
        std::vector<uint8_t> weapon_constants(const std::vector<uint8_t>& md)
        {
            std::vector<uint8_t> cb(kWeaponCbSize, 0);
            static const uint8_t kMark[4]{ 0x34, 0x58, 0x77, 0x1D };
            for (const auto& v : kWeaponVars) {
                uint8_t key[8];
                std::memcpy(key, &v.crc, 4);
                std::memcpy(key + 4, kMark, 4);
                const auto it = std::search(md.begin(), md.end(), key, key + 8);
                if (it == md.end()) continue;
                size_t p = size_t(it - md.begin()) + 8;
                if (p + 5 > md.size()) continue;
                if (md[p] == 0xFE) {
                    uint32_t dist = 0;
                    std::memcpy(&dist, md.data() + p + 1, 4);
                    if (dist > p) continue;
                    p -= dist;
                }
                const size_t n = md[p];
                if (n == 0 || n > v.size || p + 1 + n > md.size()) continue;
                std::memcpy(cb.data() + v.off, md.data() + p + 1, n);
            }
            return cb;
        }

        // The alpha of a BC3 texture's largest mip.
        bool bc3_alpha(const Tex& t, std::vector<uint8_t>& out)
        {
            if (t.dxgi != 77 || size_t((t.w + 3) / 4) * ((t.h + 3) / 4) * 16 > t.data.size()) return false;   // DXGI_FORMAT_BC3_UNORM
            out.assign(size_t(t.w) * t.h, 255);
            const uint8_t* p = t.data.data();
            for (uint32_t by = 0; by < (t.h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (t.w + 3) / 4; ++bx, p += 16) {
                    int pal[8]{ p[0], p[1] };
                    if (p[0] > p[1]) { for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * p[0] + i * p[1]) / 7; }
                    else             { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * p[0] + i * p[1]) / 5; pal[6] = 0; pal[7] = 255; }
                    uint64_t bits = 0;
                    std::memcpy(&bits, p + 2, 6);
                    for (uint32_t k = 0; k < 16; ++k) {
                        const uint32_t x = bx * 4 + k % 4, y = by * 4 + k / 4;
                        if (x < t.w && y < t.h) out[size_t(y) * t.w + x] = uint8_t(pal[(bits >> (3 * k)) & 7]);
                    }
                }
            return true;
        }

        struct AlphaCut
        {
            const std::vector<uint8_t>& alpha;
            uint32_t                    w, h, stride;
            float                       uv_scale;
            std::vector<uint8_t>&       vb;      // the part's stream is the end of it: new vertices go after it
            size_t                      vb_off;
            std::vector<uint32_t>&      out;

            bool solid(const int16_t* uv) const
            {
                const float s = uv[0] * uv_scale, t = uv[1] * uv_scale;
                const uint32_t x = std::min(uint32_t((s - std::floor(s)) * w), w - 1), y = std::min(uint32_t((t - std::floor(t)) * h), h - 1);
                return alpha[size_t(y) * w + x] >= 128;
            }
            const uint8_t* vert(uint32_t v) const { return vb.data() + vb_off + size_t(v) * stride; }
            static void uv_of(const uint8_t* p, int16_t* uv) { std::memcpy(uv, p + 8, 4); }
            // The vertex halfway between two: position and both texture places; the rest (normal, tangent, bones) from the first.
            uint32_t middle(uint32_t a, uint32_t b)
            {
                std::vector<uint8_t> n(vert(a), vert(a) + stride);
                int16_t pa[8], pb[8], pn[8];
                std::memcpy(pa, vert(a), 16);
                std::memcpy(pb, vert(b), 16);
                for (int i = 0; i < 8; ++i) pn[i] = int16_t((int32_t(pa[i]) + pb[i]) / 2);
                std::memcpy(n.data(), pn, 16);
                const uint32_t at = uint32_t((vb.size() - vb_off) / stride);
                vb.insert(vb.end(), n.begin(), n.end());
                return at;
            }
            void tri(uint32_t a, uint32_t b, uint32_t c, int depth)
            {
                int16_t ua[2], ub[2], uc[2];
                uv_of(vert(a), ua);
                uv_of(vert(b), ub);
                uv_of(vert(c), uc);
                const int16_t pts[7][2]{ { ua[0], ua[1] }, { ub[0], ub[1] }, { uc[0], uc[1] },
                                         { int16_t((ua[0] + ub[0]) / 2), int16_t((ua[1] + ub[1]) / 2) },
                                         { int16_t((ub[0] + uc[0]) / 2), int16_t((ub[1] + uc[1]) / 2) },
                                         { int16_t((uc[0] + ua[0]) / 2), int16_t((uc[1] + ua[1]) / 2) },
                                         { int16_t((ua[0] + ub[0] + uc[0]) / 3), int16_t((ua[1] + ub[1] + uc[1]) / 3) } };
                int n = 0;
                for (const auto& q : pts) n += solid(q) ? 1 : 0;
                if (n == 7 || (depth == 0 && solid(pts[6]))) {
                    out.insert(out.end(), { a, b, c });
                    return;
                }
                if (n == 0 || depth == 0) return;
                const uint32_t ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
                tri(a, ab, ca, depth - 1);
                tri(ab, b, bc, depth - 1);
                tri(ca, bc, c, depth - 1);
                tri(ab, bc, ca, depth - 1);
            }
        };

        // A DDS with a DX10 header inside a TBX file.
        bool read_tex(const std::string& path, Tex& t)
        {
            std::vector<uint8_t> x;
            if (!read_file(path_hash(path.c_str()), x) || x.size() < 0x20 || std::memcmp(x.data(), "TBX", 3) != 0) return false;
            uint32_t o = 0;
            std::memcpy(&o, x.data() + 8, 4);
            if (size_t(o) + 148 > x.size() || std::memcmp(x.data() + o, "DDS ", 4) != 0 || std::memcmp(x.data() + o + 84, "DX10", 4) != 0) return false;
            std::memcpy(&t.h, x.data() + o + 12, 4);
            std::memcpy(&t.w, x.data() + o + 16, 4);
            std::memcpy(&t.mips, x.data() + o + 28, 4);
            std::memcpy(&t.dxgi, x.data() + o + 128, 4);
            t.mips = std::max(t.mips, 1u);
            t.srgb = x[0x10] == 1;
            t.data.assign(x.begin() + o + 148, x.end());
            std::string l = path;
            std::transform(l.begin(), l.end(), l.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
            const size_t slash = l.find_last_of("\\/");
            t.name = l.substr(slash == std::string::npos ? 0 : slash + 1);
            if (const size_t dot = t.name.rfind('.'); dot != std::string::npos) t.name.resize(dot);
            return true;
        }
    }

    uint64_t path_hash(const char* path)
    {
        static const auto table = [] {
            std::vector<uint64_t> t(256);
            for (uint64_t i = 0; i < 256; ++i) {
                uint64_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xD800000000000000ull : c >> 1;
                t[size_t(i)] = c;
            }
            return t;
        }();
        uint64_t c = 0;
        for (const char* p = path; *p; ++p) {
            const char ch = *p == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(*p)));
            c             = table[size_t((c ^ uint64_t(static_cast<unsigned char>(ch))) & 255)] ^ (c >> 8);
        }
        return c;
    }

    bool knows(uint64_t item)
    {
        return std::any_of(std::begin(kItems), std::end(kItems), [&](const ItemModel& m) { return m.item == item; });
    }

    bool load(uint64_t item, Model& out)
    {
        out      = Model{};
        out.item = item;
        const auto it = std::find_if(std::begin(kItems), std::end(kItems), [&](const ItemModel& m) { return m.item == item; });
        if (it == std::end(kItems)) { out.file = "no model in the table"; return false; }
        out.file = it->name;
        auto fail = [&](const char* why) { out.file += ": "; out.file += why; return false; };
        std::vector<uint8_t> b;
        if (!read_file(it->file, b)) return fail("model not read");
        const Chunk lods = find_chunk(b, "LODS"), pc = find_chunk(b, "PCMP"), uc = find_chunk(b, "UCMP"), rm = find_chunk(b, "RMTL");
        if (!lods.size || !pc.size || !uc.size || !rm.size) return fail("a chunk is missing");
        float f2[2];
        std::memcpy(f2, b.data() + pc.off + 20, 8);
        out.pos_scale = f2[1];
        std::memcpy(f2, b.data() + uc.off + 20, 8);
        out.uv_range = f2[0];
        out.uv_scale = f2[1];
        if (const Chunk bb = find_chunk(b, "BBOX"); bb.size >= 44) std::memcpy(out.box, b.data() + bb.off + 20, sizeof(out.box));
        // The most detailed level: (distance, stream count) streams (format, stride, vertices, offset), part count, parts (stream, -, material,
        // first index, last vertex), vertex bytes, 16-aligned vertices, index count, 16-aligned 16-bit indices.
        size_t       p   = lods.off + 20 + 8;
        const size_t end = lods.off + lods.size;
        auto         u32 = [&](size_t at) { uint32_t v = 0; if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4); return v; };
        const uint32_t ns = u32(p + 4);
        p += 8;
        if (ns == 0 || ns > 8) return fail("stream count");
        struct Stream { uint32_t stride, off; };
        std::vector<Stream> streams;
        for (uint32_t i = 0; i < ns; ++i, p += 16) streams.push_back(Stream{ u32(p + 4), u32(p + 12) });
        const uint32_t npr = u32(p);
        p += 4;
        if (npr == 0 || npr > 16) return fail("part count");
        struct Prim { uint32_t stream, mat, first; };
        std::vector<Prim> prims;
        for (uint32_t i = 0; i < npr; ++i, p += 20) prims.push_back(Prim{ u32(p), u32(p + 8), u32(p + 12) });
        const uint32_t vbs = u32(p);
        p += 4;
        p += (16 - p % 16) % 16;
        const size_t vbo = p;
        p += vbs;
        const uint32_t nidx = u32(p);
        p += 4;
        p += (16 - p % 16) % 16;
        const size_t ibo = p;
        if (vbo + vbs > end || ibo + size_t(nidx) * 2 > end || vbs == 0 || nidx == 0) return fail("the level's sizes");
        out.vb.assign(b.begin() + vbo, b.begin() + vbo + vbs);
        out.ib.resize(nidx);
        for (uint32_t i = 0; i < nidx; ++i) {
            uint16_t v = 0;
            std::memcpy(&v, b.data() + ibo + size_t(i) * 2, 2);
            out.ib[i] = v;
        }

        // Its materials, in order: only the game's Weapon material (the C4's laser is a glow of its own). Each material's textures.
        // The draw is always the Weapon shader's, so with a look model (bait) another material's Weapon constants and textures fill what this material has none of.
        auto is_weapon = [](const std::vector<uint8_t>& md) {
            static const char kWeapon[] = "Weapon";
            for (size_t k = 1; k + sizeof(kWeapon) <= md.size(); ++k)
                if (md[k - 1] < 0x20 && std::memcmp(md.data() + k, kWeapon, sizeof(kWeapon)) == 0) return true;
            return false;
        };
        std::vector<uint8_t> look;
        if (std::vector<uint8_t> lb; it->look && read_file(it->look, lb)) {
            if (const Chunk lr = find_chunk(lb, "RMTL"); lr.size)
                for (const auto& lp : paths_in(lb.data() + lr.off, lr.size, ".material.bin")) {
                    std::vector<uint8_t> lm;
                    if (read_file(path_hash(lp.c_str()), lm) && is_weapon(lm)) { look = std::move(lm); break; }
                }
        }
        const auto           mat_paths = paths_in(b.data() + rm.off, rm.size, ".material.bin");
        std::vector<uint8_t> weapon(mat_paths.size(), 0), cut(mat_paths.size(), 0);
        out.mats.resize(mat_paths.size());
        out.weapon_cb.resize(mat_paths.size());
        for (size_t i = 0; i < mat_paths.size(); ++i) {
            std::vector<uint8_t> md;
            if (!read_file(path_hash(mat_paths[i].c_str()), md)) continue;
            const bool own = is_weapon(md);
            weapon[i]      = own || !look.empty();
            if (!weapon[i]) continue;
            static const char kCut[] = "opaquealpha";
            cut[i] = std::search(md.begin(), md.end(), kCut, kCut + sizeof(kCut) - 1) != md.end();
            out.weapon_cb[i] = weapon_constants(own ? md : look);
            auto add_tex = [&](const std::vector<uint8_t>& from) {
                for (const auto& tp : paths_in(from.data(), from.size(), ".xbt")) {
                    Tex t;
                    if (read_tex(tp, t)) out.mats[i].push_back(std::move(t));
                    else spdlog::info("[ThrowFile] {}: texture {} not read", it->name, tp);
                }
            };
            if (!own) add_tex(look);   // first: the last texture of a slot wins (throwfile_step), so the model's own stay on top
            add_tex(md);
        }
        for (uint32_t i = 0; i < npr; ++i) {
            const Prim& q = prims[i];
            if (q.stream >= ns || q.mat >= mat_paths.size() || !weapon[q.mat]) continue;
            const uint32_t next = i + 1 < npr ? prims[i + 1].first : nidx;
            if (next <= q.first || next > nidx) return fail("a part's indices");
            out.parts.push_back(Part{ next - q.first, q.first, streams[q.stream].stride, streams[q.stream].off, int(q.mat) });
        }
        if (out.parts.empty()) return fail("no part in the Weapon material");

        // The alpha cut, part by part, into a new index list. New vertices go after the vertex data, so only the last stream's parts can take it.
        uint32_t last_off = 0;
        for (const auto& q : streams) last_off = std::max(last_off, q.off);
        std::vector<uint32_t> cut_ib;
        for (auto& q : out.parts) {
            const uint32_t first = uint32_t(cut_ib.size());
            bool           done  = false;
            if (cut[size_t(q.mat)] && q.vb_off == last_off) {
                std::vector<uint8_t> alpha;
                const auto           tex = std::find_if(out.mats[size_t(q.mat)].begin(), out.mats[size_t(q.mat)].end(), [](const Tex& t) { return t.name.size() > 3 && t.name.compare(t.name.size() - 3, 3, "_al") == 0; });
                if (tex != out.mats[size_t(q.mat)].end() && bc3_alpha(*tex, alpha)) {
                    AlphaCut c{ alpha, tex->w, tex->h, q.stride, out.uv_scale, out.vb, q.vb_off, cut_ib };
                    for (uint32_t i = 0; i + 2 < q.count; i += 3) c.tri(out.ib[q.start + i], out.ib[q.start + i + 1], out.ib[q.start + i + 2], 3);
                    out.cut_from += q.count / 3;
                    out.cut_to += (uint32_t(cut_ib.size()) - first) / 3;
                    done = true;
                }
            }
            if (!done) cut_ib.insert(cut_ib.end(), out.ib.begin() + q.start, out.ib.begin() + q.start + q.count);
            q.start = first;
            q.count = uint32_t(cut_ib.size()) - first;
        }
        out.ib = std::move(cut_ib);
        return true;
    }
}
