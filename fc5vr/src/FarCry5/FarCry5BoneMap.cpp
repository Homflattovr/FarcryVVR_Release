// bonemap (FarCry5BoneMap.h; author 2 Nov 10:30): every bone of the first-person draws named and drawn over the eye picture.
//  - bonemap_note (each weapon-pass draw that goes ahead, the bare arms the mod hides, the gloves' indirect draw): the draw is
//    looked up in FarCry5BoneNames.h by its index count; its object transform (VS b1) and palette (VS b3: the CPU copy the mod keeps,
//    or the moved hands' palette hands_cb_swap uploaded) put each slot's joint in the game world of the weapon pass; the pass's
//    placement (m_hand_map, as reloadmagmodel) puts it in the drawn world. Its vertices are read once per draw (a GPU read, at most
//    one draw every 5 frames) for the palette slots they use (models sharing a count are told apart by that) and a rigid draw's centre.
//  - bonemap_draw (after the overlays, before the F11 panel): each point through this eye's camera (m_hist[0], as throwarc) as a dot
//    with its label (ClearView rectangles: an 8 x 14 font, a dark shadow under it).
//  - bonemapdump: one frame's draws in the log, each slot with its place on the weapon controller (cm: right, up, back).
#include "FarCry5Stereo.h"
#include "FarCry5BoneMap.h"
#include "FarCry5BoneNames.h"
#include "FarCry5Lua.h"
#include "FarCry5Record.h"

#include <d3d11_1.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <istream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace fc5::bonemap
{
    namespace
    {
        std::mutex        g_mtx;
        Opts              g_o{};
        std::atomic<bool> g_on{ false };
        std::atomic<bool> g_dump{ false };
        std::atomic<int>  g_named{ 0 }, g_unknown{ 0 }, g_bones{ 0 }, g_body{ 0 };

        // DejaVu Sans Mono Bold 13 px, thresholded to 8 x 14 (tools\bonemap\font.py), characters 32-126.
        constexpr uint8_t kFont[95][kGlyphH] = {
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // space
            { 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00 },  // !
            { 0x00, 0x00, 0x64, 0x64, 0x64, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // "
            { 0x00, 0x00, 0x00, 0x12, 0x16, 0x7F, 0x24, 0x2C, 0xFE, 0x68, 0x48, 0x00, 0x00, 0x00 },  // #
            { 0x00, 0x10, 0x10, 0x3C, 0x70, 0x70, 0x3C, 0x1E, 0x16, 0x5E, 0x3C, 0x10, 0x10, 0x00 },  // $
            { 0x00, 0x00, 0x70, 0xD0, 0xD0, 0x72, 0x18, 0x4E, 0x0B, 0x0B, 0x0E, 0x00, 0x00, 0x00 },  // %
            { 0x00, 0x00, 0x38, 0x64, 0x30, 0x30, 0x7B, 0xCF, 0xCE, 0x6E, 0x3E, 0x00, 0x00, 0x00 },  // &
            { 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '
            { 0x0C, 0x08, 0x18, 0x18, 0x10, 0x30, 0x30, 0x10, 0x18, 0x18, 0x08, 0x0C, 0x00, 0x00 },  // (
            { 0x30, 0x10, 0x18, 0x18, 0x18, 0x08, 0x08, 0x18, 0x18, 0x18, 0x10, 0x30, 0x00, 0x00 },  // )
            { 0x00, 0x00, 0x10, 0x52, 0x3C, 0x3C, 0x52, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // *
            { 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0xFE, 0xFE, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00 },  // +
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x10, 0x30, 0x00 },  // ,
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // -
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00 },  // .
            { 0x00, 0x00, 0x06, 0x04, 0x04, 0x0C, 0x08, 0x18, 0x10, 0x30, 0x20, 0x60, 0x40, 0x00 },  // /
            { 0x00, 0x00, 0x3C, 0x64, 0x66, 0x7E, 0x7E, 0x66, 0x66, 0x64, 0x3C, 0x00, 0x00, 0x00 },  // 0
            { 0x00, 0x00, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00 },  // 1
            { 0x00, 0x00, 0x3C, 0x4C, 0x06, 0x0E, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00, 0x00, 0x00 },  // 2
            { 0x00, 0x00, 0x3C, 0x46, 0x06, 0x3C, 0x06, 0x06, 0x06, 0x46, 0x3C, 0x00, 0x00, 0x00 },  // 3
            { 0x00, 0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0x4C, 0x7E, 0x0C, 0x0C, 0x0C, 0x00, 0x00, 0x00 },  // 4
            { 0x00, 0x00, 0x7C, 0x60, 0x60, 0x7C, 0x4E, 0x06, 0x06, 0x4E, 0x38, 0x00, 0x00, 0x00 },  // 5
            { 0x00, 0x00, 0x1C, 0x20, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00 },  // 6
            { 0x00, 0x00, 0x7E, 0x06, 0x0C, 0x0C, 0x1C, 0x18, 0x18, 0x30, 0x30, 0x00, 0x00, 0x00 },  // 7
            { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00 },  // 8
            { 0x00, 0x00, 0x3C, 0x64, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x4C, 0x38, 0x00, 0x00, 0x00 },  // 9
            { 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00 },  // :
            { 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x30, 0x00 },  // ;
            { 0x00, 0x00, 0x00, 0x00, 0x02, 0x0E, 0x78, 0x60, 0x78, 0x1E, 0x02, 0x00, 0x00, 0x00 },  // <
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x00, 0x7E, 0x7E, 0x00, 0x00, 0x00, 0x00 },  // =
            { 0x00, 0x00, 0x00, 0x00, 0x40, 0x70, 0x1E, 0x06, 0x1E, 0x70, 0x40, 0x00, 0x00, 0x00 },  // >
            { 0x00, 0x00, 0x3C, 0x06, 0x06, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00 },  // ?
            { 0x00, 0x00, 0x3C, 0x62, 0x4E, 0xD2, 0x92, 0xB2, 0x92, 0xD2, 0x4E, 0x62, 0x1E, 0x00 },  // @
            { 0x00, 0x00, 0x18, 0x3C, 0x3C, 0x2C, 0x64, 0x7E, 0x66, 0x46, 0xC3, 0x00, 0x00, 0x00 },  // A
            { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00 },  // B
            { 0x00, 0x00, 0x1C, 0x32, 0x60, 0x60, 0x60, 0x60, 0x60, 0x32, 0x1C, 0x00, 0x00, 0x00 },  // C
            { 0x00, 0x00, 0x78, 0x6E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6E, 0x78, 0x00, 0x00, 0x00 },  // D
            { 0x00, 0x00, 0x7E, 0x60, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00 },  // E
            { 0x00, 0x00, 0x7E, 0x60, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00 },  // F
            { 0x00, 0x00, 0x1C, 0x32, 0x60, 0x60, 0x6E, 0x62, 0x62, 0x36, 0x1E, 0x00, 0x00, 0x00 },  // G
            { 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00 },  // H
            { 0x00, 0x00, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00 },  // I
            { 0x00, 0x00, 0x3C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0C, 0x4C, 0x38, 0x00, 0x00, 0x00 },  // J
            { 0x00, 0x00, 0x66, 0x6C, 0x7C, 0x78, 0x78, 0x6C, 0x6C, 0x66, 0x67, 0x00, 0x00, 0x00 },  // K
            { 0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00 },  // L
            { 0x00, 0x00, 0x66, 0x66, 0x6E, 0x7E, 0x5A, 0x5A, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00 },  // M
            { 0x00, 0x00, 0x66, 0x66, 0x76, 0x76, 0x5E, 0x4E, 0x4E, 0x4E, 0x46, 0x00, 0x00, 0x00 },  // N
            { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00 },  // O
            { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00 },  // P
            { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x04, 0x00 },  // Q
            { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x67, 0x00, 0x00, 0x00 },  // R
            { 0x00, 0x00, 0x3C, 0x60, 0x60, 0x70, 0x3C, 0x0E, 0x06, 0x46, 0x3C, 0x00, 0x00, 0x00 },  // S
            { 0x00, 0x00, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00 },  // T
            { 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00 },  // U
            { 0x00, 0x00, 0xC6, 0x66, 0x66, 0x66, 0x64, 0x2C, 0x3C, 0x3C, 0x38, 0x00, 0x00, 0x00 },  // V
            { 0x00, 0x00, 0xC3, 0xC3, 0xDB, 0xDA, 0x5A, 0x7E, 0x6E, 0x66, 0x66, 0x00, 0x00, 0x00 },  // W
            { 0x00, 0x00, 0x66, 0x66, 0x3C, 0x38, 0x18, 0x3C, 0x3C, 0x66, 0xC6, 0x00, 0x00, 0x00 },  // X
            { 0x00, 0x00, 0xC7, 0x66, 0x6C, 0x3C, 0x38, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00 },  // Y
            { 0x00, 0x00, 0x7E, 0x06, 0x0E, 0x1C, 0x18, 0x38, 0x70, 0x60, 0x7E, 0x00, 0x00, 0x00 },  // Z
            { 0x1C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x00, 0x00 },  // [
            { 0x00, 0x00, 0x40, 0x60, 0x20, 0x30, 0x10, 0x18, 0x08, 0x0C, 0x04, 0x04, 0x06, 0x00 },  // backslash
            { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x38, 0x00, 0x00 },  // ]
            { 0x00, 0x00, 0x18, 0x3C, 0x6C, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ^
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF },  // _
            { 0x00, 0x30, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // `
            { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x46, 0x06, 0x7E, 0x66, 0x66, 0x7E, 0x00, 0x00, 0x00 },  // a
            { 0x60, 0x60, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00 },  // b
            { 0x00, 0x00, 0x00, 0x00, 0x1C, 0x32, 0x60, 0x60, 0x60, 0x32, 0x1C, 0x00, 0x00, 0x00 },  // c
            { 0x06, 0x06, 0x06, 0x06, 0x3E, 0x6E, 0x66, 0x46, 0x66, 0x6E, 0x3E, 0x00, 0x00, 0x00 },  // d
            { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x66, 0x7E, 0x60, 0x62, 0x3C, 0x00, 0x00, 0x00 },  // e
            { 0x0E, 0x18, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00 },  // f
            { 0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x3C },  // g
            { 0x60, 0x60, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00 },  // h
            { 0x18, 0x18, 0x00, 0x00, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00 },  // i
            { 0x08, 0x08, 0x00, 0x00, 0x38, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x18, 0x18, 0x78 },  // j
            { 0x60, 0x60, 0x60, 0x60, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x66, 0x00, 0x00, 0x00 },  // k
            { 0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x1E, 0x00, 0x00, 0x00 },  // l
            { 0x00, 0x00, 0x00, 0x00, 0xFE, 0xDA, 0xDA, 0xDA, 0xDA, 0xDA, 0xDA, 0x00, 0x00, 0x00 },  // m
            { 0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00 },  // n
            { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00 },  // o
            { 0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60 },  // p
            { 0x00, 0x00, 0x00, 0x00, 0x3E, 0x6E, 0x66, 0x46, 0x66, 0x6E, 0x3E, 0x06, 0x06, 0x06 },  // q
            { 0x00, 0x00, 0x00, 0x00, 0x3E, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00 },  // r
            { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x60, 0x70, 0x3C, 0x0E, 0x46, 0x3C, 0x00, 0x00, 0x00 },  // s
            { 0x00, 0x00, 0x30, 0x30, 0x7E, 0x30, 0x30, 0x30, 0x30, 0x18, 0x1E, 0x00, 0x00, 0x00 },  // t
            { 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00, 0x00, 0x00 },  // u
            { 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x2C, 0x3C, 0x3C, 0x18, 0x00, 0x00, 0x00 },  // v
            { 0x00, 0x00, 0x00, 0x00, 0xC3, 0xC3, 0xDA, 0x5A, 0x7E, 0x6E, 0x66, 0x00, 0x00, 0x00 },  // w
            { 0x00, 0x00, 0x00, 0x00, 0x66, 0x3C, 0x3C, 0x18, 0x3C, 0x6C, 0x66, 0x00, 0x00, 0x00 },  // x
            { 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x3C, 0x3C, 0x18, 0x18, 0x30, 0x70 },  // y
            { 0x00, 0x00, 0x00, 0x00, 0x7E, 0x0E, 0x1C, 0x18, 0x30, 0x70, 0x7E, 0x00, 0x00, 0x00 },  // z
            { 0x0E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00, 0x00 },  // {
            { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },  // |
            { 0x70, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00 },  // }
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ~
        };

        const char* const kGroupName[6]{ "", "weapon parts", "hands and fingers", "arms (wrist to shoulder)", "body", "other" };
    }

    Opts opts()
    {
        std::lock_guard _{ g_mtx };
        return g_o;
    }
    bool on() { return g_on.load(); }
    bool take_dump() { return g_dump.exchange(false); }

    const uint8_t* glyph(char c)
    {
        const int i = int(uint8_t(c)) - 32;
        return kFont[(i >= 0 && i < 95) ? i : ('?' - 32)];
    }

    int find(uint32_t count, int max_bone, bool indirect, int& alts)
    {
        using namespace fc5::bonenames;
        alts = 0;
        // kDraws is sorted by count: [lo, hi) the entries of counts in (count - 191, count] (the indirect draws' padding), else just count.
        const uint32_t from = indirect && count > 191 ? count - 191 : count;
        const Draw*    lo   = std::lower_bound(kDraws, kDraws + kDrawN, from, [](const Draw& d, uint32_t c) { return d.count < c; });
        int            best = -1, best_s = 1 << 30;
        for (const Draw* d = lo; d < kDraws + kDrawN && d->count <= count; ++d) {
            const bool exact = d->count == count;
            if (!exact && (d->count + 191) / 192 * 192 != count) continue;
            int sc = exact ? 0 : 50;
            if (max_bone == -2) sc += d->nbones <= 1 ? d->nbones : 30;   // the draw has no palette
            else if (max_bone >= 0) sc += d->nbones > max_bone ? std::min(20, d->nbones - max_bone - 1) : 40;
            if (indirect && d->kind == 3) sc -= 1;   // the gloves are the indirect draw of the weapon pass
            if (sc < best_s) {
                best_s = sc;
                best   = int(d - kDraws);
                alts   = 0;
            } else if (sc == best_s) {
                alts++;
            }
        }
        return best;
    }
    int candidates(uint32_t count, bool indirect, int* out, int max)
    {
        using namespace fc5::bonenames;
        const uint32_t from = indirect && count > 191 ? count - 191 : count;
        const Draw*    lo   = std::lower_bound(kDraws, kDraws + kDrawN, from, [](const Draw& d, uint32_t c) { return d.count < c; });
        int            n    = 0;
        for (const Draw* d = lo; d < kDraws + kDrawN && d->count <= count && n < max; ++d) {
            if (d->count != count && (d->count + 191) / 192 * 192 != count) continue;
            if (d->kind < 2) continue;
            out[n++] = int(d - kDraws);
        }
        return n;
    }
    uint32_t index_count(int d) { return d >= 0 && d < bonenames::kDrawN ? bonenames::kDraws[d].count : 0; }
    const char* model(int d) { return d >= 0 && d < bonenames::kDrawN ? bonenames::kMesh[bonenames::kDraws[d].mesh] : "?"; }
    int         kind(int d) { return d >= 0 && d < bonenames::kDrawN ? bonenames::kDraws[d].kind : 0; }
    int         nbones(int d) { return d >= 0 && d < bonenames::kDrawN ? bonenames::kDraws[d].nbones : 0; }
    const char* slot_name(int d, int slot)
    {
        if (d < 0 || d >= bonenames::kDrawN || slot < 0 || slot >= bonenames::kDraws[d].nbones) return nullptr;
        return bonenames::kName[bonenames::kSlot[bonenames::kDraws[d].first + uint32_t(slot)]];
    }
    int slot_group(int d, int slot)
    {
        if (d < 0 || d >= bonenames::kDrawN) return kOther;
        if (slot < 0 || slot >= bonenames::kDraws[d].nbones) return bonenames::kDraws[d].kind == 1 ? kWeapon : kOther;
        return bonenames::kGroup[bonenames::kDraws[d].first + uint32_t(slot)];
    }
    bool slot_joint(int d, int slot, float out[3])
    {
        if (d < 0 || d >= bonenames::kDrawN || slot < 0 || slot >= bonenames::kDraws[d].nbones) return false;
        const int16_t* j = bonenames::kJoint[bonenames::kDraws[d].first + uint32_t(slot)];
        for (int k = 0; k < 3; ++k) out[k] = float(j[k]) * 0.001f;
        return true;
    }

    void note_status(int named, int unknown, int bones, int body)   // a negative value: kept
    {
        if (named >= 0) g_named = named;
        if (unknown >= 0) g_unknown = unknown;
        if (bones >= 0) g_bones = bones;
        if (body >= 0) g_body = body;
    }
    std::string status()
    {
        return fmt::format("{} draws named, {} not in the tables, {} bones placed; {} game body bones", g_named.load(), g_unknown.load(), g_bones.load(), g_body.load());
    }

    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
    {
        (void)rest;
        persist = false;   // debug: never saved
        if (cmd.rfind("bonemap", 0) != 0) return false;
        const bool                  on = arg == "on" || arg == "1" || arg == "true";
        std::unique_lock<std::mutex> lk{ g_mtx };
        auto said = [&](const std::string& what) { spdlog::info("[BoneMap] {} {}: {}", cmd, arg, what); };
        if (cmd == "bonemap") {
            g_o.on = on;
            g_on   = on;
            lk.unlock();
            said(on ? "each first-person bone drawn with its name (F11 Debug > Bone and part map)" : "off");
            return true;
        }
        static const char* const kShow[]{ "", "bonemapweapon", "bonemaphands", "bonemaparms", "bonemapbodydraw", "bonemapother" };
        for (int g = 1; g <= 5; ++g)
            if (cmd == kShow[g]) {
                g_o.show[g] = on;
                lk.unlock();
                said(fmt::format("{} {}", kGroupName[g], on ? "shown" : "not shown"));
                return true;
            }
        if (cmd == "bonemaphidden") {
            g_o.hidden = on;
            lk.unlock();
            said(on ? "the draws the mod hides (bare arms) shown dimmed" : "only what is drawn");
            return true;
        }
        if (cmd == "bonemapbody") {
            lk.unlock();
            const bool ok = fc5::lua::body_bones(on);
            {
                std::lock_guard _{ g_mtx };
                g_o.game_body = on && ok;
            }
            said(!ok ? "not usable (the game's script thread or GetBonePos not found)" : on ? "the game's body bones (GetBonePos every 100 ms) in magenta" : "off");
            return true;
        }
        if (cmd == "bonemaplabels") {
            g_o.labels = arg == "ids" ? kIds : arg == "both" ? kBoth : kNames;
            lk.unlock();
            said("labels set");
            return true;
        }
        if (cmd == "bonemapat") {
            g_o.joints = arg != "skin";
            lk.unlock();
            said(arg != "skin" ? "a skinned bone at its joint" : "a skinned bone at its vertices' centre");
            return true;
        }
        if (cmd == "bonemapsize") {
            g_o.size = std::clamp(arg.empty() ? 1.0f : fv, 0.5f, 4.0f);
            lk.unlock();
            said("text scale set");
            return true;
        }
        if (cmd == "bonemapmax") {
            g_o.max = std::clamp(arg.empty() ? 120 : int(fv), 1, 1000);
            lk.unlock();
            said("label count set");
            return true;
        }
        if (cmd == "bonemapnear") {
            g_o.near_m = std::clamp(arg.empty() ? 0.0f : fv, 0.0f, 3.0f);
            const bool close_only = g_o.near_m > 0.0f;
            lk.unlock();
            said(close_only ? "only bones near either controller" : "every bone");
            return true;
        }
        if (cmd == "bonemapdump") {
            g_dump = true;
            lk.unlock();
            said("the next frame's draws logged ([BoneMap] dump lines)");
            return true;
        }
        return false;
    }

    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
    {
        const Opts o = opts();
        bool       v = o.on;
        if (ImGui::Checkbox("Show every first-person bone with its name##bonemap", &v)) cmds += v ? "bonemap on\n" : "bonemap off\n";
        hint("Debug (off by default, not saved). Every bone of the held weapon, its attachments, the gloves, the bare arms and sleeves as a dot with its name "
             "from the game's own model files, where the game draws it. Unknown draws show their id (index count / palette slot): the ids "
             "reloadmagmesh and reloadhandlepart take. Map: research\\2026-11-02_bonemap.md. Command bonemap on/off.");
        if (!v) return;
        ImGui::Text("%s", status().c_str());
        static const char* const kLabel[6]{ "", "Weapon parts (yellow)##bmw", "Hands and fingers (cyan)##bmh", "Arms, wrist to shoulder (green)##bma",
                                            "Body bones of the arms' draw (orange)##bmb", "Other (grey)##bmo" };
        static const char* const kCmd[6]{ "", "bonemapweapon", "bonemaphands", "bonemaparms", "bonemapbodydraw", "bonemapother" };
        for (int g = 1; g <= 5; ++g) {
            bool s = o.show[g];
            if (ImGui::Checkbox(kLabel[g], &s)) cmds += fmt::format("{} {}\n", kCmd[g], s ? "on" : "off");
            if (g % 2 == 1 && g < 5) ImGui::SameLine();
        }
        bool h = o.hidden;
        if (ImGui::Checkbox("Also the draws the mod hides (bare arms), dimmed##bmhid", &h)) cmds += h ? "bonemaphidden on\n" : "bonemaphidden off\n";
        hint("With hand models on, the mod hides the game's bare forearms and sleeves: their bones still show where the game animates its own arms.");
        bool b = o.game_body;
        if (ImGui::Checkbox("The game's body bones (magenta)##bmgb", &b)) cmds += b ? "bonemapbody on\n" : "bonemapbody off\n";
        hint("The player's body as the game's GetBonePos gives it every 100 ms (head, spine, shoulders, elbows, hands, legs; holster points where the skeleton has them). "
             "For the sling and the holsters.");
        int l = o.labels;
        ImGui::TextUnformatted("Labels:");
        ImGui::SameLine();
        if (ImGui::RadioButton("names##bml0", l == kNames)) cmds += "bonemaplabels names\n";
        ImGui::SameLine();
        if (ImGui::RadioButton("ids##bml1", l == kIds)) cmds += "bonemaplabels ids\n";
        ImGui::SameLine();
        if (ImGui::RadioButton("both##bml2", l == kBoth)) cmds += "bonemaplabels both\n";
        bool j = o.joints;
        if (ImGui::Checkbox("Dots on the joints (off: on the skin around each bone)##bmat", &j)) cmds += j ? "bonemapat joints\n" : "bonemapat skin\n";
        float sz = o.size;
        if (ImGui::SliderFloat("Text size##bmsize", &sz, 0.5f, 4.0f, "%.1f")) cmds += fmt::format("bonemapsize {:.1f}\n", sz);
        float nr = o.near_m;
        if (ImGui::SliderFloat("Only near a hand (m, 0 = all)##bmnear", &nr, 0.0f, 1.0f, "%.2f")) cmds += fmt::format("bonemapnear {:.2f}\n", nr);
        int mx = o.max;
        if (ImGui::SliderInt("Labels at most##bmmax", &mx, 10, 400)) cmds += fmt::format("bonemapmax {}\n", mx);
        if (ImGui::Button("Log this frame's draws and bones##bmdump")) cmds += "bonemapdump\n";
        hint("One [BoneMap] dump line per draw in fc5vr_log.txt: its id, the model, each slot's name, vertices and place on the weapon controller (cm: right, up, back).");
    }
}

namespace
{
    constexpr UINT kPaletteBytes = 12288;

    glm::mat4 from_rows(const float* f)
    {
        glm::mat4 m{ 1.0f };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[c][r] = f[r * 4 + c];
        return m;
    }
    glm::mat4 affine(const glm::mat3& r, const glm::vec3& t)
    {
        glm::mat4 m{ r };
        m[3] = glm::vec4{ t, 1.0f };
        return m;
    }
    glm::vec3 point(const glm::mat4& m, const glm::vec3& p) { return glm::vec3{ m * glm::vec4{ p, 1.0f } }; }

    // A copy of FarCry5ReloadMag.cpp's helper: a GPU buffer read back (a staging copy; immediate context only).
    bool read_gpu(ID3D11DeviceContext* ctx, ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        if (!b) return false;
        D3D11_BUFFER_DESC d{};
        b->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ID3D11Buffer* st = nullptr;
        if (dev) { dev->CreateBuffer(&d, nullptr, &st); dev->Release(); }
        if (!st) return false;
        ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
            ctx->Unmap(st, 0);
        }
        st->Release();
        return ok;
    }

    struct Scan   // one draw's vertices (rest space), read once
    {
        bool                     ok{ false };
        bool                     skinned{ false };
        int                      max_bone{ -1 };   // the highest palette slot any vertex uses (any of its 4 weights)
        std::map<int, glm::vec3> cent;             // slot (the vertex's heaviest) -> its vertices' centre
        std::map<int, int>       n;
        glm::vec3                all{ 0.0f };      // every vertex's centre
        int                      verts{ 0 };
    };
    using ScanKey = std::tuple<void*, void*, UINT, UINT, INT>;   // index buffer, vertex buffer, count, start, base

    struct Pt
    {
        glm::vec3   p{ 0.0f };   // drawn world
        int         group{ fc5::bonemap::kOther };
        bool        hidden{ false };
        bool        game{ false };   // the game's body (GetBonePos)
        uint32_t    count{ 0 };
        int         slot{ -1 };      // -1: a rigid draw
        int         draw{ -1 };      // kDraws index, -1 unknown
        const char* name{ nullptr };
    };
}

struct FarCry5Stereo::BoneMapState
{
    int                     frame{ -1 };
    std::vector<Pt>         cur, shown;
    int                     shown_frame{ -100 };
    std::map<ScanKey, Scan> scans;
    std::set<ScanKey>       failed;
    int                     scan_next{ 0 };
    bool                    dump_now{ false };
    int                     dump_lines{ 0 };
    int                     named{ 0 }, unknown{ 0 };
    std::set<uint32_t>      unknown_said;
    ID3D11View*             view{ nullptr };
    ID3D11Texture2D*        tex{ nullptr };
    uint64_t                drawn{ 0 };
    bool                    said_no_view{ false };
};

// A weapon-pass draw: how 0 drawn (direct), 1 a draw the mod hides (the bare arms), 2 an indirect draw (counts from its arguments).
void FarCry5Stereo::bonemap_note(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, int how)
{
    if (!fc5::bonemap::on() || count == 0 || !ctx || !m_motion.load() || !is_weapon_cb(m_vs_cb0)) return;
    if (!m_bm) m_bm = new BoneMapState{};
    auto&     s     = *m_bm;
    const int frame = m_frame.load();
    if (frame != s.frame) {
        if (s.dump_now) spdlog::info("[BoneMap] dump: {} draws logged", s.dump_lines);
        fc5::bonemap::note_status(s.named, s.unknown, int(s.cur.size()), -1);
        s.shown       = std::move(s.cur);
        s.shown_frame = s.frame;
        s.cur.clear();
        s.named = s.unknown = 0;
        s.frame      = frame;
        s.dump_now   = fc5::bonemap::take_dump();
        s.dump_lines = 0;
        if (s.dump_now) {
            const auto& hm = m_hand_map;
            spdlog::info("[BoneMap] dump of frame {} (weapon {}; the pass placed {} frame(s) ago; weapon controller at ({:.2f}, {:.2f}, {:.2f}))", frame, m_weapon_sig.load(),
                         frame - hm.frame, m_mo.ctrl_pos_world.x, m_mo.ctrl_pos_world.y, m_mo.ctrl_pos_world.z);
        }
    }
    const fc5::bonemap::Opts o = fc5::bonemap::opts();
    if (how == 1 && !o.hidden) return;
    m_keep_bones   = true;   // the palettes' CPU copies (note_object_constants)
    const auto& hm = m_hand_map;
    if (frame - hm.frame > 1) return;   // the weapon pass not placed this frame

    ID3D11Buffer* cb[4]{};
    ctx->VSGetConstantBuffers(0, 4, cb);
    struct Rel { ID3D11Buffer** b; ~Rel() { for (int i = 0; i < 4; ++i) if (b[i]) b[i]->Release(); } } rel{ cb };
    float              obj[32]{};
    bool               have = false, skinned = false;
    std::vector<float> pal;
    float              scale = 0.0f;
    if (cb[3]) {
        D3D11_BUFFER_DESC d{};
        cb[3]->GetDesc(&d);
        skinned = d.ByteWidth == kPaletteBytes;
    }
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end(); it != m_obj_consts.end() && it->second.size >= 48) {
            std::memcpy(obj, it->second.f, sizeof(obj));
            have = true;
        }
        if (skinned) {
            if (cb[3] == m_hand_standin && m_bm_pal_frame == frame) pal = m_bm_pal;   // the moved hands' palette (hands_cb_swap)
            else if (auto it = m_bone_data.find(cb[3]); it != m_bone_data.end() && it->second.size() >= 3072) pal = it->second;
        }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-7f && it->second.f[5] < 1e-2f) scale = it->second.f[5];
    }
    if (!have || (skinned && pal.size() < 3072)) return;   // a draw another move set its own constants for (reloadmagmodel, bowarrowmodel, throwable)

    // Its vertices, once (one draw every 5 frames: each is a GPU read).
    ID3D11Buffer* vb = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ib_fmt{};
    UINT          ib_off = 0;
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    const ScanKey key{ ib, vb, count, start, base };
    auto          si = s.scans.find(key);
    if (si == s.scans.end() && !s.failed.contains(key) && frame >= s.scan_next && !fc5::rec::is_deferred(ctx) && scale > 0.0f && vb && ib && s.scans.size() < 600) {
        s.scan_next = frame + 5;
        Scan               m{};
        ID3D11InputLayout* il = nullptr;
        ctx->IAGetInputLayout(&il);
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        if (il) il->Release();
        auto find_el = [&](const char* sem, DXGI_FORMAT fm) -> const LayoutElem* {
            for (const auto& e : layout) if (_stricmp(e.semantic.c_str(), sem) == 0 && e.index == 0 && e.slot == 0 && (fm == DXGI_FORMAT_UNKNOWN || e.format == fm)) return &e;
            return nullptr;
        };
        const LayoutElem* pe = find_el("POSITION", DXGI_FORMAT_R16G16B16A16_SINT);
        const LayoutElem* be = find_el("BLENDINDICES", DXGI_FORMAT_UNKNOWN);
        const LayoutElem* we = find_el("BLENDWEIGHT", DXGI_FORMAT_UNKNOWN);
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ctx->IAGetPrimitiveTopology(&topo);
        std::vector<uint8_t> vbd, ibd;
        if (pe && pe->offset == 0 && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && stride >= 8 && stride <= 128 && (!skinned || be) && read_gpu(ctx, vb, vbd) &&
            read_gpu(ctx, ib, ibd)) {
            const bool          i32 = ib_fmt == DXGI_FORMAT_R32_UINT;
            std::set<uint32_t>  seen;
            std::map<int, glm::vec3> sum;
            glm::vec3           all{ 0.0f };
            for (UINT k = 0; k < count; ++k) {
                const size_t io = size_t(ib_off) + size_t(start + k) * (i32 ? 4 : 2);
                if (io + (i32 ? 4 : 2) > ibd.size()) break;
                uint32_t idx = 0;
                std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
                if (!seen.insert(idx).second) continue;
                const size_t vo = size_t(voff) + size_t(int64_t(idx) + base) * stride;
                if (vo + stride > vbd.size()) continue;
                const uint8_t* v = vbd.data() + vo;
                int16_t        q[3];
                std::memcpy(q, v, 6);
                const glm::vec3 p{ float(q[0]) * scale, float(q[1]) * scale, float(q[2]) * scale };
                all += p;
                m.verts++;
                if (!skinned) continue;
                const uint8_t* bi = v + be->offset;
                int            heavy = bi[0];
                if (we && we->offset + 4 <= stride) {
                    float w[4]{};
                    const uint8_t* wp = v + we->offset;
                    if (we->format == DXGI_FORMAT_B8G8R8A8_UNORM) { w[0] = wp[2]; w[1] = wp[1]; w[2] = wp[0]; w[3] = wp[3]; }
                    else for (int c = 0; c < 4; ++c) w[c] = wp[c];
                    int hk = 0;
                    for (int c = 0; c < 4; ++c) {
                        if (w[c] > w[hk]) hk = c;
                        if (w[c] > 0.0f) m.max_bone = std::max(m.max_bone, int(bi[c]));
                    }
                    heavy = bi[hk];
                } else {
                    m.max_bone = std::max(m.max_bone, heavy);
                }
                sum[heavy] += p;
                m.n[heavy]++;
            }
            for (const auto& [b, sm] : sum) m.cent[b] = sm / float(m.n[b]);
            if (m.verts > 0) m.all = all / float(m.verts);
            m.ok      = m.verts > 0;
            m.skinned = skinned;
        }
        if (m.ok) si = s.scans.emplace(key, std::move(m)).first;
        else s.failed.insert(key);
    }
    if (vb) vb->Release();
    if (ib) ib->Release();
    const Scan* sc = si != s.scans.end() ? &si->second : nullptr;

    int       alts = 0;
    const int mb   = !skinned ? -2 : sc ? sc->max_bone : -1;
    const int d    = fc5::bonemap::find(count, mb, how == 2, alts);
    if (d >= 0) s.named++;
    else {
        s.unknown++;
        if (s.unknown_said.size() < 64 && s.unknown_said.insert(count).second)
            spdlog::info("[BoneMap] draw {} indices ({}, {}{}) not in the tables: labelled by id", count, how == 2 ? "indirect" : how == 1 ? "hidden" : "drawn",
                         skinned ? "skinned" : "rigid", sc && skinned ? fmt::format(", palette slots up to {}", sc->max_bone) : std::string{});
    }

    // Rest space -> game world (b1, the slot's palette matrix), then the weapon pass's placement: game world -> drawn world.
    const glm::mat3 L   = hm.w_rot * hm.rot;
    const glm::mat4 w2d = affine(L, hm.w_pos - hm.w_rot * hm.grip - L * hm.cam);
    const glm::mat4 Om  = from_rows(obj);
    std::string     dump;
    const glm::mat4 ci = glm::inverse(affine(m_mo.ctrl_rot_world, m_mo.ctrl_pos_world));
    auto add = [&](int slot, const glm::vec3& rest_p, const glm::mat4& T, int group) {
        Pt pt{};
        pt.p      = point(w2d * T, rest_p);
        pt.group  = group;
        pt.hidden = how == 1;
        pt.count  = count;
        pt.slot   = slot;
        pt.draw   = d;
        pt.name   = d >= 0 ? (slot >= 0 ? fc5::bonemap::slot_name(d, slot) : fc5::bonemap::nbones(d) == 1 ? fc5::bonemap::slot_name(d, 0) : nullptr) : nullptr;
        if (s.dump_now) {
            const glm::vec3 c = point(ci, pt.p) * 100.0f;
            const int       n = sc ? (slot >= 0 ? (sc->n.contains(slot) ? sc->n.at(slot) : 0) : sc->verts) : -1;
            dump += fmt::format(" {}:{}[{}v]({:.1f},{:.1f},{:.1f})", slot >= 0 ? std::to_string(slot) : std::string{ "r" }, pt.name ? pt.name : "?", n, c.x, c.y, c.z);
        }
        if (o.show[std::clamp(group, 1, 5)]) s.cur.push_back(pt);
    };
    if (skinned) {
        const int nb = d >= 0 ? fc5::bonemap::nbones(d) : 0;
        if (nb > 0) {
            for (int b = 0; b < nb && b < 256; ++b) {
                float     j[3]{};
                glm::vec3 rp{ 0.0f };
                const bool have_cent = sc && sc->cent.contains(b);
                if (o.joints && fc5::bonemap::slot_joint(d, b, j)) rp = glm::vec3{ j[0], j[1], j[2] };
                else if (have_cent) rp = sc->cent.at(b);
                else continue;
                add(b, rp, Om * from_rows(pal.data() + size_t(b) * 12), fc5::bonemap::slot_group(d, b));
            }
        } else if (sc) {
            for (const auto& [b, c] : sc->cent)
                if (b < 256 && sc->n.at(b) >= 3) add(b, c, Om * from_rows(pal.data() + size_t(b) * 12), fc5::bonemap::kOther);
        }
    } else {
        add(-1, sc ? sc->all : glm::vec3{ 0.0f }, Om, d >= 0 && fc5::bonemap::kind(d) == 1 ? fc5::bonemap::kWeapon : fc5::bonemap::kOther);
    }
    if (s.dump_now && s.dump_lines < 200) {
        s.dump_lines++;
        spdlog::info("[BoneMap] dump: draw {} from {} base {} ({}, {}): {}{}; slots (name [vertices] (right, up, back) cm on the weapon controller):{}", count, start, base,
                     how == 2 ? "indirect" : how == 1 ? "hidden by the mod" : "drawn", skinned ? "skinned" : "rigid", d >= 0 ? fc5::bonemap::model(d) : "not in the tables",
                     d >= 0 ? fmt::format(" ({} slots{}{})", fc5::bonemap::nbones(d), alts ? fmt::format(", {} other models share it", alts) : std::string{},
                                          fc5::bonenames::kDraws[d].count != count ? fmt::format(", its {} indices padded", fc5::bonenames::kDraws[d].count) : std::string{})
                            : std::string{},
                     dump.empty() ? " none" : dump);
    }
}

// After the overlays: each point of the frame's draws (this frame's, else the last one's) and the game's body as dots and labels.
void FarCry5Stereo::bonemap_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    if (!fc5::bonemap::on() || !ctx || !eye || !m_motion.load() || m_menu_screen_on) return;
    const int fr = m_frame.load();
    if (m_hist[0].cur_frame != fr) return;
    if (!m_bm) m_bm = new BoneMapState{};
    auto&                    s = *m_bm;
    const fc5::bonemap::Opts o = fc5::bonemap::opts();
    // This frame's draws (the weapon pass is drawn before the present), else the last complete frame's.
    std::vector<Pt>          pts = (s.frame == fr || (s.frame == fr - 1 && !s.cur.empty())) ? s.cur : s.shown_frame >= fr - 2 ? s.shown : std::vector<Pt>{};
    int                      nbody = 0;
    if (o.game_body) {
        float       pos[64][3]{};
        const char* names[64]{};
        int64_t     age = -1;
        const int   n   = fc5::lua::body_bones_read(pos, names, 64, age);
        if (age >= 0 && age < 1000)
            for (int i = 0; i < n; ++i) {
                Pt pt{};
                pt.p     = glm::vec3{ pos[i][0], pos[i][1], pos[i][2] };
                pt.group = fc5::bonemap::kBody;
                pt.game  = true;
                pt.name  = names[i];
                pts.push_back(pt);
                nbody++;
            }
    }
    fc5::bonemap::note_status(-1, -1, -1, nbody);
    if (pts.empty()) return;

    // The eye picture's view (as throwarc): a render target or an unordered access view for ClearView.
    if (eye != s.tex) {
        if (s.view) s.view->Release();
        s.view = nullptr;
        s.tex  = eye;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC td{};
            eye->GetDesc(&td);
            if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
                ID3D11RenderTargetView* v = nullptr;
                dev->CreateRenderTargetView(eye, nullptr, &v);
                s.view = v;
            } else if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
                ID3D11UnorderedAccessView* v = nullptr;
                dev->CreateUnorderedAccessView(eye, nullptr, &v);
                s.view = v;
            }
            dev->Release();
        }
        if (!s.view && !s.said_no_view) {
            s.said_no_view = true;
            spdlog::info("[BoneMap] the eye picture cannot be drawn on (no render target or UAV binding): nothing drawn");
        }
    }
    if (!s.view) return;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return;
    D3D11_TEXTURE2D_DESC d{};
    eye->GetDesc(&d);
    const float W = float(d.Width), H = float(d.Height);
    const int   k = std::max(1, int(std::lround(o.size * H / 1100.0f)));   // pixels per font pixel

    // Near a hand only (bonemapnear): the controllers, and the weapon hand when it is not on the weapon (sling, bow).
    std::vector<glm::vec3> hands;
    if (m_mo.ctrl_valid) hands.push_back(m_mo.ctrl_pos_world);
    if (m_mo.other_valid) hands.push_back(m_mo.other_pos_world);
    if (m_mo.slung || m_mo.bow || m_mo.kept) hands.push_back(m_mo.hand_pos_world);
    struct Lab { float x, y, dist; size_t i; };
    std::vector<Lab> labs;
    labs.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt& p = pts[i];
        if (o.near_m > 0.0f && !p.game) {
            float best = 1e9f;
            for (const auto& h : hands) best = std::min(best, glm::length(p.p - h));
            if (best > o.near_m) continue;
        }
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ p.p - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.05f) continue;
        const float x = (cl.x / cl.w * 0.5f + 0.5f) * W, y = (0.5f - cl.y / cl.w * 0.5f) * H;
        const float edge = 8.0f * float(k);   // the dot and its shadow stay inside the picture
        if (x < edge || y < edge || x > W - edge || y > H - edge) continue;
        labs.push_back({ x, y, (x - 0.5f * W) * (x - 0.5f * W) + (y - 0.5f * H) * (y - 0.5f * H), i });
    }
    std::sort(labs.begin(), labs.end(), [](const Lab& a, const Lab& b) { return a.dist < b.dist; });
    if (labs.size() > size_t(o.max)) labs.resize(size_t(o.max));

    static const float kCol[6][3]{ { 0.9f, 0.9f, 0.9f }, { 1.0f, 0.88f, 0.2f }, { 0.3f, 0.9f, 1.0f }, { 0.4f, 1.0f, 0.45f }, { 1.0f, 0.6f, 0.2f }, { 0.8f, 0.8f, 0.8f } };
    const float        dark[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    std::map<int, std::vector<D3D11_RECT>> by_col;   // colour index (group, +8 dimmed, 6 the game's body) -> rectangles
    std::vector<D3D11_RECT>                shadow;
    std::vector<D3D11_RECT>                placed;   // labels' boxes so far (a label that would cover one moves down, else is left out)
    auto box = [](LONG x0, LONG y0, LONG x1, LONG y1) { return D3D11_RECT{ x0, y0, x1, y1 }; };
    auto overlaps = [&](const D3D11_RECT& r) {
        for (const auto& q : placed) if (r.left < q.right && q.left < r.right && r.top < q.bottom && q.top < r.bottom) return true;
        return false;
    };
    for (const auto& l : labs) {
        const Pt&  p   = pts[l.i];
        const int  ci  = p.game ? 6 : std::clamp(p.group, 0, 5) + (p.hidden ? 8 : 0);
        auto&      rs  = by_col[ci];
        const LONG x   = LONG(l.x), y = LONG(l.y), r = LONG(2 * k);
        shadow.push_back(box(x - r - k, y - r - k, x + r + k + 1, y + r + k + 1));
        rs.push_back(box(x - r, y - r, x + r + 1, y + r + 1));
        // The label's text.
        std::string t;
        const std::string id = p.game ? std::string{} : fmt::format("{}/{}", p.count, p.slot >= 0 ? std::to_string(p.slot) : std::string{ "r" });
        if (p.game) t = p.name ? p.name : "?";
        else if (o.labels == fc5::bonemap::kIds || !p.name) t = id;
        else if (o.labels == fc5::bonemap::kBoth) t = id + " " + p.name;
        else t = p.name;
        if (t.size() > 40) t.resize(40);
        const LONG tw = LONG(t.size()) * fc5::bonemap::kGlyphW * k, th = fc5::bonemap::kGlyphH * k;
        D3D11_RECT tb = box(x + 4 * k, y - th / 2, x + 4 * k + tw, y - th / 2 + th);
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok; ++tries) {
            if (!overlaps(tb)) ok = true;
            else { tb.top += th; tb.bottom += th; }
        }
        if (!ok || tb.top < 0 || tb.right + k > LONG(W) || tb.bottom + k > LONG(H)) continue;
        placed.push_back(tb);
        for (size_t c = 0; c < t.size(); ++c) {
            const uint8_t* g  = fc5::bonemap::glyph(t[c]);
            const LONG     gx = tb.left + LONG(c) * fc5::bonemap::kGlyphW * k;
            for (int row = 0; row < fc5::bonemap::kGlyphH; ++row) {
                const uint8_t bits = g[row];
                for (int bx = 0; bx < 8;) {
                    if (!(bits & (0x80 >> bx))) { ++bx; continue; }
                    int ex = bx;
                    while (ex < 8 && (bits & (0x80 >> ex))) ++ex;
                    const D3D11_RECT gr = box(gx + bx * k, tb.top + row * k, gx + ex * k, tb.top + (row + 1) * k);
                    rs.push_back(gr);
                    shadow.push_back(box(gr.left + k, gr.top + k, gr.right + k, gr.bottom + k));
                    bx = ex;
                }
            }
        }
    }
    auto clear = [&](const float col[4], const std::vector<D3D11_RECT>& v) {
        for (size_t i = 0; i < v.size(); i += 512) ctx1->ClearView(s.view, col, v.data() + i, UINT(std::min<size_t>(512, v.size() - i)));
    };
    clear(dark, shadow);
    for (const auto& [ci, v] : by_col) {
        const float* c   = ci == 6 ? nullptr : kCol[ci & 7];
        const float  dim = ci >= 8 ? 0.55f : 1.0f;
        const float  col[4]{ c ? c[0] * dim : 1.0f, c ? c[1] * dim : 0.35f, c ? c[2] * dim : 1.0f, 1.0f };
        clear(col, v);
    }
    ctx1->Release();
    if (s.drawn++ % 900 == 0)
        spdlog::info("[BoneMap] {} points ({} of the game's body), {} labelled, in eye pictures {}x{} (text {} px per font pixel); {}", pts.size(), nbody, placed.size(), d.Width,
                     d.Height, k, fc5::bonemap::status());
}
