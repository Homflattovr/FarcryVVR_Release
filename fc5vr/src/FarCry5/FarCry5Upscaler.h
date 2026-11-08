#pragma once
// The upscaler list (F11 Display > Image quality "Upscaler"; author 3 Nov 14:37): which upscaler answers the
// mod's DLSS calls. tools\optiscaler\fc5_optiscaler.ps1 installs OptiScaler once; this only switches it, for the next game start.
#include <string>

namespace fc5::upscaler {
enum { kDlss = 0, kFsr31 = 1, kFfx = 2, kFsr22 = 3 };

bool installed();                       // OptiScaler installed by the script (its manifest is in bin)
int  current();                         // what the next game start uses (read once, then kept by set(): the menu asks every frame)
int  at_start();                        // what this game start really loaded (the proxy's NGX export in the process); logged once
bool set(int choice);                   // DLSS: the proxy dll renamed out of the way; else back in place and Dx11Upscaler written in OptiScaler.ini
const std::string& last_error();        // why the last set() failed, empty if it did not
}
