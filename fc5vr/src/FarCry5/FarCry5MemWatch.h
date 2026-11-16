#pragma once

// memwatch (author 13 Nov 20:45).
// A line a minute, from its own thread (no render-thread work besides the creation hooks below):
//   [Mem] t=M min | RAM: the process's private bytes and working set, handles, threads | VRAM: the process's local and shared video
//   memory (DXGI QueryVideoMemoryInfo) and the budget | mod heap: what the mod's own C++ new holds (blocks, MB) | mod GPU: the D3D11
//   buffers and textures the mod's code made that are still alive (count, MB, of which staging = system memory), how many it made since
//   the last line | other modules' D3D11 resources (DLSS, overlays; the game's own engine is not counted) | the change of each since
//   the first line and since the last.
//   [Mem] mod GPU by call site: the biggest call sites (live count, MB, the last one's size) and every site whose live count grew since
//   the last line, by its return address in dxgi.dll (dxgi.pdb names the function and line; two frames: the call and its caller).
// A leak shows as a number that only climbs over a long session: mod heap, mod GPU or one call site's count (game streaming moves RAM and
// VRAM up and down; the mod's own numbers should be flat once every feature has been used once).
//
// How the mod's GPU resources are counted: ID3D11Device CreateBuffer, CreateTexture1D, CreateTexture2D and CreateTexture3D are taken in
// the device's vtable (the existing inline CreateTexture2D hook for padglyphs stays as it is; this runs before it). A call whose return
// address is in dxgi.dll (or in another module that is not the game's FC_m64.dll / FarCry5.exe) gets a small tag object attached with
// SetPrivateDataInterface; D3D releases the tag when the resource is destroyed, which takes it off the count. The game's own calls only
// pay two address compares. Not counted: what the mod made before the first present (a handful of startup objects), textures made
// through the padglyphs trampoline, views and shaders (small), and game resources the mod holds a reference to (weapon copies, at most
// 8 weapons; scope history copies, at most 160).
//
// Commands (never saved): memwatch (a line now), memwatch S (every S seconds, default 60, at least 10; 0 off), memwatch sites (every
// live call site, one line each), memwatch detail (the full RAM walk once: a hitch of up to ~100 ms), memheaps N (the heaps line every N
// memory lines, default 10, 0 never).

#include <string>

struct ID3D11Device;

namespace fc5::memwatch
{
    // Once (the first present, render thread): the creation hooks and the logging thread.
    void start(ID3D11Device* dev);
    bool command(const std::string& cmd, const std::string& arg, float fv);
}
