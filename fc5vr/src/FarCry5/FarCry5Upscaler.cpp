// The upscaler list: see FarCry5Upscaler.h. The mod's DLSS code is not touched: with OptiScaler's proxy dll in place it answers the NGX calls, and with the
// dll renamed the game starts without it, on real DLSS. A dll the process has loaded can still be renamed, so the list works with the game running.
#include "FarCry5Upscaler.h"
#include "ModPath.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fc5::upscaler {
namespace {
    namespace fs = std::filesystem;
    constexpr const char* kOff = ".fc5vr_off";   // tools\optiscaler\fc5_optiscaler.ps1 uninstall removes this name too
    bool        g_read = false;
    std::string g_proxy, g_err;
    int         g_cur  = kDlss;

    // The manifest's first line ends with "proxy winmm.dll".
    std::string proxy_name()
    {
        std::ifstream f{ fc5::mod_file("fc5vr_optiscaler_manifest.txt") };
        std::string   line;
        if (!f || !std::getline(f, line)) return {};
        const auto at = line.rfind("proxy ");
        if (at == std::string::npos) return {};
        std::string n = line.substr(at + 6);
        while (!n.empty() && (n.back() == '\r' || n.back() == ' ')) n.pop_back();
        return n;
    }

    std::string ini_upscaler()
    {
        std::ifstream f{ fc5::mod_file("OptiScaler.ini") };
        std::string   line;
        while (std::getline(f, line)) {
            if (line.rfind("Dx11Upscaler=", 0) != 0) continue;
            std::string v = line.substr(13);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
        return {};
    }

    bool write_ini_upscaler(const char* value)
    {
        const std::string path = fc5::mod_file("OptiScaler.ini");
        std::ifstream     in{ path, std::ios::binary };
        if (!in) return false;
        std::vector<std::string> lines;
        std::string              line;
        bool                     found = false;
        while (std::getline(in, line)) {
            if (line.rfind("Dx11Upscaler=", 0) == 0) {
                const bool cr = !line.empty() && line.back() == '\r';
                line          = std::string{ "Dx11Upscaler=" } + value + (cr ? "\r" : "");
                found         = true;
            }
            lines.push_back(line);
        }
        in.close();
        if (!found) return false;
        std::ofstream out{ path, std::ios::binary | std::ios::trunc };
        for (const auto& l : lines) out << l << '\n';
        return out.good();
    }
}

namespace {
    int choice_of(const std::string& v) { return v == "fsr31" ? kFsr31 : v == "ffx_12" ? kFfx : v == "fsr22" ? kFsr22 : kDlss; }

    void read_files()
    {
        if (g_read) return;
        g_read  = true;
        g_proxy = proxy_name();
        std::error_code ec;
        g_cur = g_proxy.empty() || !fs::exists(fc5::mod_file(g_proxy.c_str()), ec) ? kDlss : choice_of(ini_upscaler());
    }
}

bool installed()
{
    read_files();
    return !g_proxy.empty();
}

int current()
{
    read_files();
    return g_cur;
}

const std::string& last_error() { return g_err; }

int at_start()
{
    static const int start = []() -> int {
        const std::string proxy = proxy_name();
        const HMODULE     mod   = proxy.empty() ? nullptr : GetModuleHandleA(proxy.c_str());
        const bool        opti  = mod && GetProcAddress(mod, "NVSDK_NGX_D3D11_EvaluateFeature") != nullptr;
        const std::string v     = opti ? ini_upscaler() : std::string{};
        if (opti) spdlog::info("[Upscaler] OptiScaler ({}) loaded, Dx11Upscaler={}", proxy, v);
        else spdlog::info("[Upscaler] DLSS (OptiScaler {})", proxy.empty() ? "not installed" : "off");
        if (opti && v == "dlss") {
            // Left over from the removed "DLSS (OptiScaler)" choice (author 5 Nov: same speed and picture as the mod's DLSS): OptiScaler runs
            // NVIDIA's DLSS itself this time; it is turned off for the next start, so the mod's own DLSS answers again (as choosing DLSS does).
            std::error_code ec;
            const fs::path  on  = fc5::mod_file(proxy.c_str());
            const fs::path  off = fc5::mod_file((proxy + kOff).c_str());
            if (!fs::exists(off, ec)) fs::rename(on, off, ec);
            spdlog::warn("[Upscaler] Dx11Upscaler=dlss is OptiScaler's own DLSS, a choice the mod no longer offers: {}", ec || fs::exists(on, ec) ? "OptiScaler could not be turned off, choose DLSS in F11" : "OptiScaler off from the next start (the mod's own DLSS)");
            g_read = false;   // current() reads the files again: DLSS
            return kDlss;
        }
        return opti ? choice_of(v) : kDlss;
    }();
    return start;
}

bool set(int choice)
{
    at_start();
    read_files();
    g_err.clear();
    const std::string& proxy = g_proxy;
    if (proxy.empty()) {
        g_err = "OptiScaler is not installed";
        return false;
    }
    const fs::path  on  = fc5::mod_file(proxy.c_str());
    const fs::path  off = fc5::mod_file((proxy + kOff).c_str());
    std::error_code ec;
    if (choice == kDlss) {
        if (fs::exists(on, ec)) fs::rename(on, off, ec);
    } else {
        if (!write_ini_upscaler(choice == kFfx ? "ffx_12" : choice == kFsr22 ? "fsr22" : "fsr31")) {
            g_err = "OptiScaler.ini could not be changed";
            return false;
        }
        if (!fs::exists(on, ec) && fs::exists(off, ec)) fs::rename(off, on, ec);
    }
    if (ec) {
        g_err = proxy + " could not be renamed: " + ec.message();
        spdlog::warn("[Upscaler] {}", g_err);
        return false;
    }
    g_cur = choice;
    spdlog::info("[Upscaler] {} from the next game start", choice == kDlss ? "DLSS" : choice == kFfx ? "ffx_12" : choice == kFsr22 ? "FSR 2.2" : "FSR 3.1");
    return true;
}
}
