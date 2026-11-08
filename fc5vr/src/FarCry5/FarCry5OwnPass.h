#pragma once

// The mod's own short passes drawn inside the game's frame (hidmask): while one runs on this thread, the context hooks pass every
// call straight to D3D11, so none of their per-draw tracking (current pixel shader, bound depth target, scene-depth binding) sees it.

namespace fc5::ownpass
{
    inline thread_local bool t_active = false;

    inline bool active() { return t_active; }

    struct Scope
    {
        bool prev;
        Scope() : prev(t_active) { t_active = true; }
        ~Scope() { t_active = prev; }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
