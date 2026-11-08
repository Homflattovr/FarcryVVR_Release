// padglyphs (FarCry5PadGlyph.h; author 6 Nov): the game's button icons replaced by the VR controllers' buttons.
//  - CreateTexture2D (FarCry5Stereo::hk_create_texture2d): a BC3 icon-sized texture is hashed (FNV-1a 64 of its block rows); one of the game's
//    icons (kKnown) is created DEFAULT instead of IMMUTABLE, with the controller's icon as its data, and kept (one reference) so the present
//    thread can write it again when the layout changes (a vehicle, vehtriggers, the letters, padglyphs off). If the DEFAULT texture is refused,
//    it is created as the game asked, with the controller's icon, and is not updated later.
//  - UpdateSubresource of a whole icon-sized BC3 texture (in case the game fills its icons that way): the same, with the source swapped.
//  - frame(): each kept texture shows the icon for the current layout; one the game released (only this file's reference left) is let go.
//    With padglyphs off the icons are still found and kept, showing the game's own data, so switching it on again changes them at once.
//  - The icons are drawn and compressed once per icon, letters and size (FarCry5PadGlyphDraw.cpp), the game's own data kept per hash.
#include "FarCry5Drive.h"
#include "FarCry5PadGlyph.h"
#include "FarCry5PadGlyphDraw.h"
#include "FarCry5PadGlyphTable.h"

#include <Windows.h>
#include <imgui.h>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <istream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace fc5::padglyph
{
    namespace
    {
        enum class Mode : uint8_t { Auto, Touch, Index, Off };
        // Which pad layout the mod is in (ModConfig.cpp): on foot (and the flat menu screen with menutabs off), a vehicle with drivebuttons
        // (types 0, 1, 3), a land vehicle on the triggers, a helicopter or plane, the flat menu screen with menutabs on (the triggers LB / RB,
        // the grips LT / RT).
        enum class Veh : uint8_t { Foot, CarButtons, LandTriggers, Air, Menu };

        struct Layout
        {
            Mode    mode{ Mode::Auto };
            Letters letters{ Letters::Touch };
            bool    keys{ true };
            Veh     veh{ Veh::Foot };
            bool    swapped{ false };
            bool operator==(const Layout& o) const { return mode == o.mode && letters == o.letters && keys == o.keys && veh == o.veh && swapped == o.swapped; }
        };

        struct Kept
        {
            ID3D11Texture2D* tex{ nullptr };
            const Known*     known{ nullptr };
            bool             updatable{ false };
            Icon             shown{ Icon::None };     // Icon::None: the game's own icon
            Letters          shown_letters{ Letters::Touch };
        };

        std::atomic<int>  g_mode{ int(Mode::Auto) };
        std::atomic<bool> g_keys{ true };
        std::atomic<int>  g_letters_now{ int(Letters::Touch) };
        std::atomic<int>  g_veh_now{ int(Veh::Foot) };
        std::atomic<bool> g_swapped_now{ false };

        std::mutex                                          g_mtx;   // everything below
        std::vector<Kept>                                   g_kept;
        std::unordered_map<uint64_t, std::vector<uint8_t>>  g_orig;    // the game's own data, by hash
        std::map<std::tuple<int, int, int, int>, std::vector<uint8_t>> g_drawn;   // icon, letters, w, h -> BC3
        std::unordered_map<uint64_t, const Known*>          g_by_hash;
        std::vector<uint64_t>                               g_unknown;   // candidates that are none of the game's icons (first few, for the log)
        std::string                                         g_profile;
        struct Stats
        {
            int candidates{ 0 }, matched{ 0 }, created_default{ 0 }, created_as_asked{ 0 }, via_update{ 0 }, rewrites{ 0 }, released{ 0 };
        } g_st;

        thread_local bool t_self_write = false;

        // promptdevice: the kept icons by texture (rebuilt by frame() under g_mtx) and the family the HUD drew last.
        std::unordered_map<const void*, Kind> g_kind_of;
        std::atomic<int>                      g_dev{ 0 };
        uint64_t                              g_dev_flips{ 0 };   // under g_mtx
        ULONGLONG                             g_dev_logged{ 0 };

        // padglyphprobe
        uint8_t*                g_base{ nullptr };
        safetyhook::MidHook     g_probe_hook{};
        std::atomic<bool>       g_probe_on{ false };
        std::atomic<uint32_t>   g_probe_val{ 0 }, g_probe_al{ 0 };
        std::atomic<uint64_t>   g_probe_hits{ 0 };

        // padonly: the game's device pick (FC_m64.dll +0x2851c0, every frame) sorts the input devices by their idle time [dev+0E8h] and
        // the one used last becomes the game's device (its prompts, its weapon wheel). Types 2 and 3 are the keyboard and the mouse, 4
        // the pad. The hook at +0x2851f2 (just after movss xmm6, [rax+0E8h]) gives the keyboard and the mouse an idle time of 1e9 s, so
        // the pad wins whenever it is there; with no pad the keyboard still wins (it is still in the list, last).
        safetyhook::MidHook     g_pad_hook{};
        std::atomic<bool>       g_pad_on{ true };
        std::atomic<uint64_t>   g_pad_seen{ 0 }, g_pad_pushed{ 0 };

        void build_index()
        {
            if (!g_by_hash.empty()) return;
            for (const auto& k : kKnown) g_by_hash.emplace(k.hash, &k);
        }

        uint32_t row_bytes(UINT w) { return ((w + 3) / 4) * 16; }
        uint32_t block_rows(UINT h) { return (h + 3) / 4; }

        // What the mod sends a pad button from, in each layout (ModConfig.cpp's on_xinput_get_state).
        Icon pad_icon(Pad p, Veh v, bool sw)
        {
            const bool trig_layout = v == Veh::LandTriggers || v == Veh::Air;
            switch (p) {
            case Pad::A: return Icon::FaceA;
            case Pad::B: return v == Veh::CarButtons ? Icon::FaceX2 : Icon::FaceX;   // drivebuttons: left X twice is the handbrake
            case Pad::X: return Icon::FaceY;
            case Pad::Y: return Icon::FaceB;
            case Pad::LB:
                if (v == Veh::CarButtons) return Icon::TrigR;    // drivebuttons: the right trigger shoots
                return trig_layout && sw ? Icon::TrigR : Icon::TrigL;
            case Pad::RB: return v == Veh::Menu ? Icon::TrigR : Icon::LGripB;   // menutabs: the right trigger is the tab to the right
            case Pad::LT:
                if (v == Veh::CarButtons) return Icon::FaceX;    // brake on X
                if (v == Veh::Menu) return Icon::GripL;          // menutabs: the map zooms out on the left grip
                return trig_layout && sw ? Icon::GripL : Icon::GripR;
            case Pad::RT:
                if (v == Veh::CarButtons) return Icon::FaceA;    // throttle on A
                if (v == Veh::Menu) return Icon::GripR;          // menutabs: the map zooms in on the right grip
                return trig_layout && sw ? Icon::TrigL : Icon::TrigR;
            case Pad::LS: return Icon::StickL;
            case Pad::RS: return Icon::StickR;
            case Pad::LSPress: return Icon::StickLPress;
            case Pad::RSPress: return Icon::StickRPress;
            case Pad::DPad: return Icon::LGripStick;
            case Pad::DUp: return Icon::LGripUp;
            case Pad::DDown: return Icon::LGripDown;
            case Pad::DLeft: return Icon::LGripLeft;
            case Pad::DRight: return Icon::LGripRight;
            case Pad::Start: return Icon::Menu;
            // drivemenu (FarCry5Drive.h): in a vehicle (all: on foot too) the menu button alone opens the map with a short press.
            case Pad::Back: return fc5::drive::menu_mode() >= (v == Veh::Foot || v == Veh::Menu ? 2 : 1) ? Icon::Menu : Icon::LGripMenu;
            default: return Icon::None;
            }
        }

        // A key's action under the game's default keys, as the pad button that does it (or a stick direction). DirectInput scan codes.
        Icon key_icon(uint8_t dik, Veh v, bool sw)
        {
            auto pad = [&](Pad p) { return pad_icon(p, v, sw); };
            if (v == Veh::Air) {
                switch (dik) {
                case 57: return pad(Pad::RT);   // Space: up / faster
                case 46: return pad(Pad::LT);   // C: down / slower
                default: break;
                }
            } else if (v != Veh::Foot && v != Veh::Menu) {
                switch (dik) {
                case 17: return pad(Pad::RT);        // W: throttle
                case 31: return pad(Pad::LT);        // S: brake
                case 57: return pad(Pad::B);         // Space: handbrake
                case 46: return pad(Pad::A);         // C: change seat
                case 18: return pad(Pad::X);         // E: get out
                case 33: return pad(Pad::RSPress);   // F: horn
                case 42: return pad(Pad::LSPress);   // Left Shift: drive on its own
                default: break;
                }
            }
            switch (dik) {
            case 17: return Icon::StickLUp;      // W
            case 31: return Icon::StickLDown;    // S
            case 30: return Icon::StickLLeft;    // A
            case 32: return Icon::StickLRight;   // D
            case 42: return pad(Pad::LSPress);   // Left Shift: sprint
            case 57: return pad(Pad::A);         // Space: jump
            case 46: return pad(Pad::B);         // C: crouch
            case 18: return pad(Pad::X);         // E: interact
            case 19: return pad(Pad::X);         // R: reload
            case 33: return pad(Pad::RSPress);   // F: takedown
            case 29: return pad(Pad::Y);         // Left Ctrl: heal
            case 45: return pad(Pad::Y);         // X: last weapon
            case 16: return pad(Pad::LB);        // Q: weapon wheel
            case 20: return pad(Pad::DRight);    // T: throw a rock
            case 44: return pad(Pad::DUp);       // Z: binoculars
            case 34: return pad(Pad::DLeft);     // G: companion
            case 35: return pad(Pad::DRight);    // H: companion
            case 50: return pad(Pad::Back);      // M: map
            case 1: return pad(Pad::B);          // Esc: back
            case 28: return pad(Pad::A);         // Enter: confirm
            case 26: return pad(Pad::DUp);       // [: radio
            case 27: return pad(Pad::DLeft);     // ]: radio
            default: return Icon::None;
            }
        }

        Icon mouse_icon(Mouse m, Veh v, bool sw)
        {
            if (v == Veh::Menu) v = Veh::Foot;   // menutabs: the mouse's prompts as before
            switch (m) {
            case Mouse::Left: return pad_icon(v == Veh::Foot ? Pad::RT : Pad::LB, v, sw);   // shoot (in a vehicle the pad's LB)
            case Mouse::Right: return v == Veh::Foot ? pad_icon(Pad::LT, v, sw) : Icon::None;   // aim
            case Mouse::Middle: return v == Veh::Foot ? pad_icon(Pad::RB, v, sw) : Icon::None;  // throw
            default: return Icon::None;
            }
        }

        Layout layout_now()
        {
            Layout l{};
            l.mode    = Mode(g_mode.load());
            l.keys    = g_keys.load();
            l.veh     = Veh(g_veh_now.load());
            l.swapped = g_swapped_now.load();
            l.letters = l.mode == Mode::Index ? Letters::Index : l.mode == Mode::Touch ? Letters::Touch : Letters(g_letters_now.load());
            return l;
        }

        Icon wanted(const Known& k, const Layout& l)
        {
            if (l.mode == Mode::Off) return Icon::None;
            switch (k.kind) {
            case Kind::Pad: return pad_icon(Pad(k.code), l.veh, l.swapped);
            case Kind::Key: return l.keys ? key_icon(k.code, l.veh, l.swapped) : Icon::None;
            case Kind::Mouse: return l.keys ? mouse_icon(Mouse(k.code), l.veh, l.swapped) : Icon::None;
            }
            return Icon::None;
        }

        // g_mtx held. The BC3 data a kept texture shows: the drawn icon, or the game's own.
        const std::vector<uint8_t>* data_for(const Known& k, Icon icon, Letters letters)
        {
            if (icon == Icon::None) {
                auto it = g_orig.find(k.hash);
                return it == g_orig.end() ? nullptr : &it->second;
            }
            const auto key = std::make_tuple(int(icon), int(letters), int(k.w), int(k.h));
            auto       it  = g_drawn.find(key);
            if (it != g_drawn.end()) return &it->second;
            std::vector<uint8_t> rgba(size_t(k.w) * k.h * 4);
            render(icon, letters, k.w, k.h, rgba.data());
            std::vector<uint8_t> bc(size_t(row_bytes(k.w)) * block_rows(k.h));
            encode_bc3(rgba.data(), k.w, k.h, bc.data());
            return &g_drawn.emplace(key, std::move(bc)).first->second;
        }

        // g_mtx held: the Known for this data, or nullptr; keeps the game's data and counts.
        const Known* recognise(UINT w, UINT h, const void* data, UINT pitch)
        {
            build_index();
            g_st.candidates++;
            const uint32_t rb = row_bytes(w), rows = block_rows(h);
            if (pitch < rb) return nullptr;
            const uint64_t hh = hash_rows(data, int(rb), int(rows), int(pitch));
            auto           it = g_by_hash.find(hh);
            if (it == g_by_hash.end() || it->second->w != w || it->second->h != h) {
                if (g_unknown.size() < 12 && std::find(g_unknown.begin(), g_unknown.end(), hh) == g_unknown.end()) g_unknown.push_back(hh);
                return nullptr;
            }
            g_st.matched++;
            auto& o = g_orig[hh];
            if (o.empty()) {
                o.resize(size_t(rb) * rows);
                for (uint32_t r = 0; r < rows; ++r) std::memcpy(o.data() + size_t(r) * rb, static_cast<const uint8_t*>(data) + size_t(r) * pitch, rb);
            }
            return it->second;
        }

        bool icon_size(UINT w, UINT h) { return (w == 64 && h == 64) || (w == 96 && h == 96) || (w == 92 && h == 64); }
        bool bc3(DXGI_FORMAT f) { return f == DXGI_FORMAT_BC3_UNORM || f == DXGI_FORMAT_BC3_UNORM_SRGB || f == DXGI_FORMAT_BC3_TYPELESS; }

        // padglyphprobe: the hook only copies; frame() logs.
        bool read_u32(uintptr_t p, uint32_t& v)
        {
            __try {
                v = *reinterpret_cast<const uint32_t*>(p);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        void probe_hit(SafetyHookContext& c)
        {
            if (!g_probe_on.load(std::memory_order_relaxed)) return;
            uint32_t v = 0;
            if (read_u32(c.r14, v)) g_probe_val.store(v, std::memory_order_relaxed);
            g_probe_al.store(uint32_t(c.rax & 0xff), std::memory_order_relaxed);
            g_probe_hits.fetch_add(1, std::memory_order_relaxed);
        }
        bool probe_code_matches(const uint8_t* at)
        {
            // mov r14d, [r14]; mov [rbp+0B8h], al; cmp r14d, 0AF35B6EDh
            static const uint8_t kAt[16]{ 0x45, 0x8B, 0x36, 0x88, 0x85, 0xB8, 0x00, 0x00, 0x00, 0x41, 0x81, 0xFE, 0xED, 0xB6, 0x35, 0xAF };
            __try { return std::memcmp(at, kAt, 16) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void probe_install()
        {
            static bool tried = false;
            if (tried) return;
            tried = true;
            if (!g_base || !probe_code_matches(g_base + 0x154bcf5)) {
                spdlog::info("[PadGlyph] padglyphprobe: the prompt device code is not at +0x154bcf5 (other game build, or not decrypted yet): no probe");
                return;
            }
            g_probe_hook = safetyhook::create_mid(g_base + 0x154bcf5, probe_hit);
            spdlog::info("[PadGlyph] padglyphprobe hook {}", g_probe_hook ? "installed" : "failed");
        }

        void pad_hit(SafetyHookContext& c)
        {
            g_pad_seen.fetch_add(1, std::memory_order_relaxed);
            if (!g_pad_on.load(std::memory_order_relaxed)) return;
            const uint32_t type = uint32_t(c.rdx);   // mov edx, [rax+8] at +0x2851ce: the device's type
            if (type != 2 && type != 3) return;
            c.xmm6.f32[0] = 1.0e9f;
            g_pad_pushed.fetch_add(1, std::memory_order_relaxed);
        }
        bool pad_code_matches(const uint8_t* at)
        {
            // movss xmm6, [rax+0E8h]; lea r14, [rbp+48h]; mov [rsp+48h], ecx; mov cl, 1
            static const uint8_t kAt[18]{ 0xF3, 0x0F, 0x10, 0xB0, 0xE8, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x75, 0x48, 0x89, 0x4C, 0x24, 0x48, 0xB1, 0x01 };
            __try { return std::memcmp(at, kAt, 18) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        // Present thread, from frame(): tries once a second for a minute (the code may not be decrypted at the first frames).
        void pad_install()
        {
            static int       tries = 0;
            static ULONGLONG next  = 0;
            if (g_pad_hook || tries >= 60 || !g_base) return;
            const ULONGLONG now = GetTickCount64();
            if (now < next) return;
            next = now + 1000;
            tries++;
            if (!pad_code_matches(g_base + 0x2851ea)) {
                if (tries == 60)
                    spdlog::info("[PadGlyph] padonly: the game's device pick is not at +0x2851ea (other game build?): the game can still switch to the keyboard");
                return;
            }
            g_pad_hook = safetyhook::create_mid(g_base + 0x2851f2, pad_hit);
            spdlog::info("[PadGlyph] padonly hook {} (now {})", g_pad_hook ? "installed" : "failed",
                         g_pad_on.load() ? "on: the game stays on the controller" : "off");
        }

        void log_status(const char* why)
        {
            std::lock_guard _{ g_mtx };
            const Layout l = layout_now();
            int shown = 0, keys = 0, fixed = 0;
            for (const auto& k : g_kept) {
                if (k.shown != Icon::None) shown++;
                if (k.known->kind != Kind::Pad) keys++;
                if (!k.updatable) fixed++;
            }
            std::string unk;
            for (auto h : g_unknown) unk += fmt::format(" {:016x}", h);
            static const char* kMode[]{ "auto", "touch", "index", "off" };
            static const char* kVeh[]{ "on foot", "vehicle (drivebuttons)", "vehicle (triggers)", "aircraft", "menu screen (menutabs)" };
            spdlog::info("[PadGlyph] {}: padglyphs {}, letters {}, keys {}, {}{}; profile '{}'; {} icons kept ({} keyboard/mouse, {} not updatable), {} showing "
                         "the controller; {} candidates, {} recognised, {} created DEFAULT, {} as asked, {} by UpdateSubresource, {} rewrites, {} released; "
                         "unknown icon-sized BC3:{}",
                         why, kMode[int(l.mode)], l.letters == Letters::Index ? "Index" : "Touch", l.keys ? "on" : "off", kVeh[int(l.veh)],
                         l.swapped ? ", triggers swapped" : "", g_profile, g_kept.size(), keys, fixed, shown, g_st.candidates, g_st.matched,
                         g_st.created_default, g_st.created_as_asked, g_st.via_update, g_st.rewrites, g_st.released, unk.empty() ? " none" : unk);
        }
    }

    bool candidate(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init)
    {
        return d && init && init->pSysMem && d->MipLevels == 1 && d->ArraySize == 1 && bc3(d->Format) && icon_size(d->Width, d->Height) &&
               (d->BindFlags & D3D11_BIND_SHADER_RESOURCE) && d->CPUAccessFlags == 0;
    }

    HRESULT on_create_texture2d(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out, const CreateFn& create)
    {
        const Known* k = nullptr;
        std::vector<uint8_t> mine;
        Icon icon = Icon::None;
        Letters letters = Letters::Touch;
        {
            std::lock_guard _{ g_mtx };
            k = recognise(d->Width, d->Height, init->pSysMem, init->SysMemPitch);
            if (k) {
                const Layout l = layout_now();
                icon = wanted(*k, l);
                letters = l.letters;
                if (icon != Icon::None) {
                    if (auto* p = data_for(*k, icon, letters)) mine = *p;
                    else icon = Icon::None;
                }
            }
        }
        if (!k || !out) return create(d, init, out);
        D3D11_SUBRESOURCE_DATA sd = *init;
        if (icon != Icon::None) {
            sd.pSysMem     = mine.data();
            sd.SysMemPitch = row_bytes(d->Width);
        }
        D3D11_TEXTURE2D_DESC dd = *d;
        dd.Usage = D3D11_USAGE_DEFAULT;
        HRESULT hr = create(&dd, &sd, out);
        bool updatable = SUCCEEDED(hr) && *out;
        if (!updatable) {
            hr = create(d, &sd, out);
            if (FAILED(hr) || !*out) {
                if (icon != Icon::None) hr = create(d, init, out);
                return hr;
            }
        }
        std::lock_guard _{ g_mtx };
        (updatable ? g_st.created_default : g_st.created_as_asked)++;
        (*out)->AddRef();
        g_kept.push_back(Kept{ *out, k, updatable, icon, letters });
        return hr;
    }

    void on_update_subresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void*& src, UINT& row)
    {
        if (t_self_write || !dst || !src || box || sub != 0) return;
        D3D11_RESOURCE_DIMENSION dim{};
        dst->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
        auto* tex = static_cast<ID3D11Texture2D*>(dst);
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.MipLevels != 1 || d.ArraySize != 1 || !bc3(d.Format) || !icon_size(d.Width, d.Height)) return;
        std::lock_guard _{ g_mtx };
        auto kept = std::find_if(g_kept.begin(), g_kept.end(), [&](const Kept& kt) { return kt.tex == tex; });
        const Known* k = recognise(d.Width, d.Height, src, row);
        if (!k) {
            if (kept != g_kept.end()) {   // one of ours now holds something else: let it go
                kept->tex->Release();
                *kept = g_kept.back();
                g_kept.pop_back();
                g_st.released++;
            }
            return;
        }
        const Layout l = layout_now();
        Icon icon = wanted(*k, l);
        const std::vector<uint8_t>* p = icon == Icon::None ? nullptr : data_for(*k, icon, l.letters);
        if (!p) icon = Icon::None;
        g_st.via_update++;
        if (kept != g_kept.end()) {
            *kept = Kept{ tex, k, kept->updatable, icon, l.letters };   // the game wrote another icon into one of ours
        } else {
            tex->AddRef();
            g_kept.push_back(Kept{ tex, k, d.Usage == D3D11_USAGE_DEFAULT, icon, l.letters });
        }
        if (p) {
            src = p->data();   // g_drawn's entries are never removed, so the data outlives the call
            row = row_bytes(d.Width);
        }
    }

    void frame(ID3D11DeviceContext* ctx, const FrameIn& in)
    {
        // The layout the mod is in (ModConfig.cpp's btn_layout and vehtriggers).
        Veh v = Veh::Foot;
        if (!in.flat) {
            if (in.buttons && (in.veh_type == 0 || in.veh_type == 1 || in.veh_type == 3)) v = Veh::CarButtons;
            else if (in.in_vehicle) v = (in.veh_type == 4 || in.veh_type == 5) ? Veh::Air : Veh::LandTriggers;
        } else if (in.menu_tabs) {
            v = Veh::Menu;   // menutabs: the triggers are the tabs, the grips LT / RT
        }
        g_veh_now     = int(v);
        g_swapped_now = in.swapped && v != Veh::Foot;
        const std::string prof = in.profile ? in.profile : "";
        g_letters_now = int(prof.find("index_controller") != std::string::npos ? Letters::Index : Letters::Touch);

        static ULONGLONG next_prune = 0, next_probe = 0;
        static uint64_t  probe_last = ~0ull;
        static int       probe_lines = 0;
        const ULONGLONG  now = GetTickCount64();
        {
            std::lock_guard _{ g_mtx };
            if (prof != g_profile) {
                spdlog::info("[PadGlyph] interaction profile '{}': {} letters", prof, prof.find("index_controller") != std::string::npos ? "Index" : "Touch");
                g_profile = prof;
            }
            const Layout l = layout_now();
            for (auto& k : g_kept) {
                if (!k.updatable || !ctx) continue;
                const Icon w = wanted(*k.known, l);
                if (w == k.shown && (w == Icon::None || k.shown_letters == l.letters)) continue;
                const std::vector<uint8_t>* p = data_for(*k.known, w, l.letters);
                if (!p) continue;
                t_self_write = true;
                ctx->UpdateSubresource(k.tex, 0, nullptr, p->data(), row_bytes(k.known->w), 0);
                t_self_write = false;
                k.shown = w;
                k.shown_letters = l.letters;
                g_st.rewrites++;
            }
            // promptdevice: the icon textures by pointer, for note_hud_texture (the kept list changes at creation, an update or the prune).
            g_kind_of.clear();
            for (const auto& k : g_kept) g_kind_of.emplace(static_cast<const void*>(k.tex), k.known->kind);
            if (now >= next_prune) {
                next_prune = now + 5000;
                for (size_t i = 0; i < g_kept.size();) {
                    const ULONG n = g_kept[i].tex->AddRef();
                    g_kept[i].tex->Release();
                    if (n <= 2) {   // only this file's reference left
                        g_kept[i].tex->Release();
                        g_kept[i] = g_kept.back();
                        g_kept.pop_back();
                        g_st.released++;
                    } else {
                        ++i;
                    }
                }
            }
        }
        static Layout last{};
        static bool   first = true;
        const Layout  l = layout_now();
        if (first || !(l == last)) {
            first = false;
            last = l;
            log_status("layout");
        }
        pad_install();
        {
            static ULONGLONG next_pad = 0;
            static uint64_t  pad_last = 0;
            if (g_pad_hook && now >= next_pad) {
                next_pad = now + 5000;
                const uint64_t pushed = g_pad_pushed.load();
                if (pushed != pad_last) {
                    if (pad_last == 0)
                        spdlog::info("[PadGlyph] padonly: kept the keyboard and mouse out of the game's device pick ({} of {} device checks so far)",
                                     pushed, g_pad_seen.load());
                    pad_last = pushed;
                }
            }
        }
        if (g_probe_on.load()) {
            probe_install();
            if (now >= next_probe) {
                next_probe = now + 1000;
                const uint64_t key = (uint64_t(g_probe_val.load()) << 8) | g_probe_al.load();
                if (key != probe_last && probe_lines < 40) {
                    probe_last = key;
                    probe_lines++;
                    spdlog::info("[PadGlyph] padglyphprobe: the prompt code compares {:08x} (al {:02x}), {} hits so far", g_probe_val.load(), g_probe_al.load(),
                                 g_probe_hits.load());
                }
            }
        }
    }

    void set_game_base(uint8_t* base) { g_base = base; }

    void note_hud_texture(ID3D11Resource* r)
    {
        if (!r) return;
        std::unique_lock lk{ g_mtx, std::try_to_lock };
        if (!lk.owns_lock()) return;
        const auto it = g_kind_of.find(static_cast<const void*>(r));   // a texture's ID3D11Resource is the same pointer (single inheritance)
        if (it == g_kind_of.end()) return;
        const int dev = it->second == Kind::Pad ? 1 : 2;
        const int was = g_dev.exchange(dev);
        if (was == dev) return;
        g_dev_flips++;
        const ULONGLONG now = GetTickCount64();
        if (was == 0 || now - g_dev_logged >= 1000) {   // at most one line a second (a frame with both kinds would flip every draw)
            g_dev_logged = now;
            spdlog::info("[PadGlyph] promptdevice: the game's prompts now show {} ({} changes so far)",
                         dev == 1 ? "the controller's buttons" : "keyboard or mouse keys", g_dev_flips);
        }
    }

    int prompt_device() { return g_dev.load(std::memory_order_relaxed); }

    bool command(const std::string& cmd, const std::string& arg, std::istream&, bool& persist)
    {
        if (cmd == "padonly") {
            persist = true;
            const bool v = !(arg == "off" || arg == "0" || arg == "false");
            g_pad_on = v;
            spdlog::info("[PadGlyph] padonly {}{}", v ? "on: the game stays on the controller (prompts and weapon wheel), whatever the mod's mouse and keys do"
                                                       : "off: the game switches to the keyboard when the mod moves the mouse or presses a key",
                         g_pad_hook ? "" : " (hook not installed yet)");
            return true;
        }
        if (cmd.rfind("padglyph", 0) != 0) return false;
        if (cmd == "padglyphs") {
            persist = true;
            const Mode m = arg == "off" ? Mode::Off : arg == "touch" ? Mode::Touch : arg == "index" ? Mode::Index : Mode::Auto;
            g_mode = int(m);
            static const char* kMode[]{ "auto: the letters of the controllers in use", "touch: right A B, left X Y", "index: A and B on each hand",
                                        "off: the game's own icons" };
            spdlog::info("[PadGlyph] padglyphs {}", kMode[int(m)]);
            return true;
        }
        if (cmd == "padglyphkeys") {
            persist = true;
            const bool v = arg.empty() || arg == "on" || arg == "1" || arg == "true";
            g_keys = v;
            spdlog::info("[PadGlyph] padglyphkeys {}", v ? "on: keyboard and mouse icons show the controller too" : "off: the game's keyboard and mouse icons");
            return true;
        }
        if (cmd == "padglyphprobe") {
            persist = false;
            const bool v = arg.empty() || arg == "on" || arg == "1" || arg == "true";
            g_probe_on = v;
            spdlog::info("[PadGlyph] padglyphprobe {}", v ? "on (research: logs what the prompt code compares when it picks the device)" : "off");
            return true;
        }
        if (cmd == "padglyphlog") {
            persist = false;
            log_status("padglyphlog");
            return true;
        }
        return false;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        bool po = g_pad_on.load();
        if (ImGui::Checkbox("Keep the game on the controller##padonly", &po)) cmds += po ? "padonly on\n" : "padonly off\n";
        hint("On (default): the game never switches to keyboard mode, so its prompts and its weapon wheel stay the controller's even when the "
             "mod's aiming moves the mouse or it presses a key. Off: the game's own switching. Command padonly on/off.");
        if (!g_pad_hook) ImGui::TextDisabled("(the game's device pick was not found yet)");
        int m = g_mode.load();
        static const char* kItems[]{ "Auto (from the controllers)", "Meta Touch letters", "Index letters", "Off (the game's icons)" };
        static const char* kCmd[]{ "auto", "touch", "index", "off" };
        if (ImGui::Combo("Button icons##padglyphs", &m, kItems, 4)) cmds += fmt::format("padglyphs {}\n", kCmd[m]);
        hint("The game's button prompts show the VR controller's button that does it: X for the pad's B, B for its Y, the trigger or grip for "
             "the shoulder buttons, in vehicles too. Auto picks Index letters on Index controllers, Touch letters (right A B, left X Y) otherwise. "
             "Icons already on screen change at once; a few only after the next load. Command padglyphs auto/touch/index/off.");
        if (m == int(Mode::Off)) return;
        bool k = g_keys.load();
        if (ImGui::Checkbox("Keyboard and mouse prompts too##padglyphkeys", &k)) cmds += k ? "padglyphkeys on\n" : "padglyphkeys off\n";
        hint("The game switches to keyboard prompts when the mod's aiming moves the mouse. On: those show the controller too (E shows Y, W the "
             "left stick up). Command padglyphkeys on/off.");
        int kept = 0, shown = 0;
        {
            std::lock_guard _{ g_mtx };
            kept = int(g_kept.size());
            for (const auto& t : g_kept) shown += t.shown != Icon::None;
        }
        ImGui::Text("%d of the game's icons found, %d showing the controller", kept, shown);
        if (ImGui::Button("Log status##padglyphlog")) cmds += "padglyphlog\n";
        bool pr = g_probe_on.load();
        if (ImGui::Checkbox("Log the game's prompt device (research)##padglyphprobe", &pr)) cmds += pr ? "padglyphprobe on\n" : "padglyphprobe off\n";
        hint("Research: logs the value the game's prompt code compares when it picks pad or keyboard icons. Command padglyphprobe on/off.");
    }
}
