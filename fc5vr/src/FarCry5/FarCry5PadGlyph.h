#pragma once
// padglyphs (author 6 Nov): the game's button prompts show what to press on the VR controllers, whatever device the game thinks it is used with.
//  - The game draws each prompt's button as a 64 x 64 icon texture (96 x 96 for its large prompts, 92 x 64 for wide keyboard keys), BC3, one
//    mip: ui\common\textures\buttons\crossplatform\{xboxone,xenon,ps4,third_party,kbm}\tx_*.xbt. Each icon is recognised when it is created
//    (ID3D11Device::CreateTexture2D, or a later UpdateSubresource of the whole texture) by the FNV-1a 64 of its data (FarCry5PadGlyphTable.h, made
//    by tools\padglyphs\gen_table.py from the game's files), and its pixels are replaced by the controller's own button (FarCry5PadGlyphDraw.cpp).
//  - Xbox icons: the Touch control the mod sends that pad button from (ModConfig.cpp), so the pad's B (sent by left X) shows X, its Y (sent by
//    right B) shows B, LB (left trigger) a left trigger, LT (right grip) a right grip, RB (left grip + right B) both, the D-pad the left grip with
//    a stick, Back the left grip with the menu button. In a vehicle the icons follow the vehicle layout the mod is in (drivebuttons: throttle on
//    A, brake on X, handbrake X twice; vehtriggers left: the triggers and grips swapped).
//  - Keyboard and mouse icons (padglyphkeys on): the game switches its prompts to the keyboard when the mod's mouse moves (aiming) or keys
//    arrive, so each key's icon shows the Touch control that does that key's action under the game's default keys (E interact: the pad's X,
//    so the left Y; W: the left stick up). A key with no pad action keeps the game's icon.
//  - Letters: Touch (right A B, left X Y; also Pico and Reverb G2) or Index (A and B on each hand, with L or R), from the OpenXR interaction
//    profile when padglyphs is auto.
// Commands:
//  - padglyphs auto|touch|index|off (saved, default auto): off shows the game's own icons again.
//  - padglyphkeys on|off (saved, default on): the keyboard and mouse icons too.
//  - padglyphlog: one line of what was found and replaced.
//  - padglyphprobe on|off (not saved, default off; research): logs the value the game's prompt code compares when it picks the device
//    (FC_m64 +0x154bcf5), for a later fix that keeps the game itself on the pad.
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::padglyph
{
    using CreateFn = std::function<HRESULT(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**)>;

    // Cheap test: could this texture be one of the game's button icons (BC3, one mip, an icon's size, with data)?
    bool candidate(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init);
    // CreateTexture2D of a candidate: creates it through create, with the controller's icon in place of the game's when it is one.
    HRESULT on_create_texture2d(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out, const CreateFn& create);
    // UpdateSubresource: when the game writes a whole icon into a texture, src and row are pointed at the controller's icon instead.
    void on_update_subresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void*& src, UINT& row);

    struct FrameIn
    {
        bool        flat{ false };        // the flat menu screen is up (the pad layout on foot)
        bool        menu_tabs{ false };   // menutabs on (FarCry5Stereo::menu_tabs): on the flat screen the triggers are LB / RB, the grips LT / RT
        bool        in_vehicle{ false };  // driving (FarCry5Stereo::in_vehicle)
        int         veh_type{ -1 };       // lua::vehicle_state's type (4 helicopter, 5 plane), -1 unknown
        bool        buttons{ false };     // drive::buttons(): drivebuttons on
        bool        swapped{ false };     // veh_triggers_swapped()
        const char* profile{ "" };        // the OpenXR interaction profile
    };
    // Present thread: the icons follow the controllers and the vehicle layout; textures the game let go are released; probe lines.
    void frame(ID3D11DeviceContext* ctx, const FrameIn& in);

    // install_scene_hook: FC_m64.dll's base, for padglyphprobe.
    void set_game_base(uint8_t* base);

    bool command(const std::string& cmd, const std::string& arg, std::istream& rest, bool& persist);
    // F11 HUD and menus > Button icons.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);

    // promptdevice (wheelmouse auto, FarCry5Stereo::wheel_mouse_tick): the draw hook passes each texture a HUD draw reads; when it is one of
    // the game's button icons, its family is the device the game's prompts are for now. prompt_device(): 0 no icon drawn yet, 1 the
    // controller's (pad) icons, 2 keyboard or mouse icons. Any thread; never waits for the icon lock (a busy lock skips that texture).
    void note_hud_texture(ID3D11Resource* r);
    int  prompt_device();
}
