#include "ModConfig.h"
#include "FarCry5/FarCry5Stereo.h"
#include "FarCry5/FarCry5Afw.h"
#include "ModPath.h"
#include "CrashLog.h"
#include "Framework.hpp"
#include "dgxiProxy.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <d3d12.h>
#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <fstream>
#include <vector>

// vrframework's release builds log to a null sink, so no log file is written.
// Replace the default logger with a real file (vr_log.txt, in the same folder
// as vr_config.txt: the Starfield folder, or %APPDATA%\VR\Starfield when the
// game folder isn't writable) so testers can send it. Overwritten each launch, after the previous one is
// kept in fc5vr_logs\ (newest 10, with Virtual Desktop's OpenXR.log of that run beside it).
// Returns true when this run continues the current log instead: the game's first process (started by Steam or Ubisoft
// Connect) loads the mod too and relaunches the game within seconds; since the log opens first thing, rotating there
// would keep that stub's few lines as "the previous run" (two of the ten slots per launch, 2026-10-23).
static bool ContinuesRelaunchedLog(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || fad.nFileSizeHigh != 0 || fad.nFileSizeLow > 256 * 1024) {
        return false;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto t = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    if (t(now) - t(fad.ftLastWriteTime) > 60ull * 10000000ull) {
        return false;   // older than a minute: a run of its own
    }
    std::ifstream f{ path, std::ios::binary };
    std::string   text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // That start never presented: the stub process (or a crash in the first seconds, kept in the same file).
    return text.find("[Boot] step") != std::string::npos && text.find("first present") == std::string::npos;
}

static bool KeepPreviousLog(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) {
        return false;
    }
    if (ContinuesRelaunchedLog(path)) {
        return true;
    }
    const auto dir = path.parent_path() / "fc5vr_logs";
    fs::create_directories(dir, ec);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    SYSTEMTIME st{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        FILETIME local{};
        FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
    }
    char stamp[32]{};
    sprintf_s(stamp, "%04u-%02u-%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fs::copy_file(path, dir / (std::string("fc5vr_log_") + stamp + ".txt"), fs::copy_options::overwrite_existing, ec);
    // VDXR rewrites this file when the next OpenXR instance starts, so now it still describes the previous run.
    const fs::path vdxr = "C:\\ProgramData\\Virtual Desktop\\OpenXR.log";
    if (fs::exists(vdxr, ec)) {
        fs::copy_file(vdxr, dir / (std::string("fc5vr_log_") + stamp + "_vdxr.txt"), fs::copy_options::overwrite_existing, ec);
    }
    std::vector<fs::directory_entry> logs;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("fc5vr_log_", 0) == 0 && name.find("_vdxr") == std::string::npos) {
            logs.push_back(e);
        }
    }
    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
    for (size_t i = 10; i < logs.size(); ++i) {
        auto vd = logs[i].path();
        vd.replace_filename(vd.stem().string() + "_vdxr.txt");
        fs::remove(logs[i].path(), ec);
        fs::remove(vd, ec);
    }
    return false;
}

static constexpr const char* kLogPattern = "[%H:%M:%S.%e] [%L] [tid:%t] %v";

// The first thing the init thread does (before the early hooks, the headset check and the framework), so a crash in
// the first seconds leaves this run's log, not the previous one's.
void InstallFileLogger() {
    try {
        const auto path = Framework::get_persistent_dir("fc5vr_log.txt");
        bool       relaunch = false;
        try {
            relaunch = KeepPreviousLog(path);
        } catch (...) {
        }
        auto logger = spdlog::basic_logger_mt("fc5vr", path.string(), !relaunch);
        logger->set_pattern(kLogPattern);
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
        if (relaunch) {
            spdlog::info("---- the game started again (process {}): the lines above are the process that relaunched it ----", GetCurrentProcessId());
        }
        spdlog::info("fc5vr log started, writing to {}", path.string());
        fc5::crash::log_ready(path.wstring());
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what()); // keep the null logger
    }
}

// Framework's constructor makes a null default logger and sets its pattern on every logger: ours back on top.
static void RestoreFileLogger() {
    if (auto logger = spdlog::get("fc5vr")) {
        spdlog::set_default_logger(logger);
        logger->set_pattern(kLogPattern);
        logger->flush_on(spdlog::level::info);
    }
}

bool verifyLeftHandedCoordinates() {
    // Create forward vector (in left-handed, +Z is forward)
    glm::vec3 forward(0.0f, 0.0f, 1.0f);

    // Create test rotation 90 degrees around Y axis
    glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Apply rotation
    glm::vec3 result = glm::vec3(rotationY * glm::vec4(forward, 0.0f));

    // In left-handed: rotating +Z 90° around Y should give +X
    // In right-handed: rotating +Z 90° around Y should give -X
    bool isLeftHanded = glm::abs(result.x - 1.0f) < 0.00001f;

    return isLeftHanded;
}


// Research aid: with sfvr_dred.txt in the game folder, D3D12 Device Removed Extended Data
// (breadcrumbs + page faults) is turned on before the game creates its device, so a GPU fault
// can be explained in the log (NativeStereo reports it).
void EnableDredIfRequested() {
    if (GetFileAttributesA(fc5::mod_file("sfvr_dred.txt").c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    using get_debug_interface_t = HRESULT(WINAPI*)(REFIID, void**);
    const auto d3d12 = LoadLibraryA("d3d12.dll");
    const auto get_debug_interface = d3d12 ? reinterpret_cast<get_debug_interface_t>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    if (get_debug_interface && SUCCEEDED(get_debug_interface(IID_PPV_ARGS(&settings))) && settings) {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->Release();
        OutputDebugStringA("[sfvr] DRED enabled\n");
    }
}

// Test runtime for this game only (Meta XR Simulator): bin/fc5vr_xr_runtime.txt holds the path of an
// OpenXR runtime manifest; the loader reads XR_RUNTIME_JSON before the system's active runtime.
static void UseTestRuntimeIfRequested() {
    FILE* f = nullptr;
    if (fopen_s(&f, fc5::mod_file("fc5vr_xr_runtime.txt").c_str(), "r") != 0 || f == nullptr) {
        return;
    }
    char path[1024]{};
    if (fgets(path, sizeof(path), f)) {
        for (char* c = path; *c; ++c) {
            if (*c == '\r' || *c == '\n') { *c = 0; break; }
        }
        if (path[0]) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", path);
            OutputDebugStringA("[fc5vr] XR_RUNTIME_JSON set from fc5vr_xr_runtime.txt\n");
        }
    }
    fclose(f);
}

// The launcher (FC5VR_Launcher.exe) writes bin/fc5vr_launcher.txt: "mode vr" or "mode flat", and
// "headset quest3". No file: VR, as before the launcher. The resolution multiplier is the resscale line
// in fc5vr_settings.txt, the same one F11 changes.
struct LauncherChoice {
    bool        flat = false;
    std::string headset;
};

static LauncherChoice ReadLauncherChoice() {
    LauncherChoice c;
    FILE*          f = nullptr;
    if (fopen_s(&f, fc5::mod_file("fc5vr_launcher.txt").c_str(), "r") != 0 || f == nullptr) {
        return c;
    }
    char line[256]{};
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' ')) l.pop_back();
        if (l.rfind("mode ", 0) == 0) c.flat = l.substr(5) == "flat";
        if (l.rfind("headset ", 0) == 0) c.headset = l.substr(8);
    }
    fclose(f);
    return c;
}

// reticleoff (default on; a line "reticleoff off" in fc5vr_settings.txt leaves the player's choice): the game's reticle off in
// VR (Options > Interface > Reticle). It stays fixed in the middle of the HUD whatever the gun does. Author 19 Nov: many testers
// still had it (the launcher only offered it, with a Yes/No for all the game options, and a game started without the launcher
// was never checked). HelpCrosshair="0" in Documents\My Games\Far Cry 5\gamerprofile.xml, set here before the game reads the
// file (this thread starts while the game loads dxgi.dll), added when the file does not have it (the game's default is on).
// The file as it was is kept as gamerprofile.xml.fc5vr_backup (the first time, never overwritten) and .fc5vr_last, as the
// launcher does; the new text goes to a temporary file first, then replaces it.
static void ReticleOffInProfile() {
    try {
        {
            std::ifstream s{ fc5::mod_file("fc5vr_settings.txt") };
            std::string   l;
            while (std::getline(s, l)) {
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (l == "reticleoff off") {
                    spdlog::info("[Launcher] reticleoff off (fc5vr_settings.txt): the game's Reticle option left as it is");
                    return;
                }
            }
        }
        PWSTR docs = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || docs == nullptr) {
            if (docs) CoTaskMemFree(docs);
            spdlog::info("[Launcher] reticleoff: no Documents folder, the game's Reticle option not checked");
            return;
        }
        const std::filesystem::path p = std::filesystem::path{ docs } / L"My Games" / L"Far Cry 5" / L"gamerprofile.xml";
        CoTaskMemFree(docs);
        std::string t;
        {
            std::ifstream f{ p, std::ios::binary };
            if (!f) {
                spdlog::info("[Launcher] reticleoff: no Documents\\My Games\\Far Cry 5\\gamerprofile.xml yet (the game writes it at its first start)");
                return;
            }
            t.assign(std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{});
        }
        const std::string el  = "<ProfileSpecificGameProfile";
        const size_t      tag = t.find(el + ' ');
        const size_t      end = tag == std::string::npos ? std::string::npos : t.find('>', tag);
        if (end == std::string::npos) {
            spdlog::info("[Launcher] reticleoff: gamerprofile.xml has no ProfileSpecificGameProfile, the game's Reticle option not checked");
            return;
        }
        const std::string at = "HelpCrosshair=\"";
        size_t            a  = t.find(at, tag);
        while (a != std::string::npos && a < end && !std::isspace((unsigned char)t[a - 1])) a = t.find(at, a + 1);
        std::string was = "missing";
        if (a != std::string::npos && a < end) {
            const size_t v = a + at.size();
            const size_t q = t.find('"', v);
            if (q == std::string::npos || q > end) return;
            was = t.substr(v, q - v);
            if (was == "0") {
                spdlog::info("[Launcher] reticleoff: the game's Reticle is already off");
                return;
            }
            t.replace(v, q - v, "0");
        } else {
            t.insert(tag + el.size(), " HelpCrosshair=\"0\"");
        }
        std::error_code ec;
        auto            with = [&](const wchar_t* ext) { auto c = p; c += ext; return c; };
        if (!std::filesystem::exists(with(L".fc5vr_backup"), ec)) std::filesystem::copy_file(p, with(L".fc5vr_backup"), ec);
        std::filesystem::copy_file(p, with(L".fc5vr_last"), std::filesystem::copy_options::overwrite_existing, ec);
        const DWORD fa = GetFileAttributesW(p.c_str());
        if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p.c_str(), fa & ~FILE_ATTRIBUTE_READONLY);
        {
            std::ofstream o{ with(L".fc5vr_tmp"), std::ios::binary | std::ios::trunc };
            o.write(t.data(), (std::streamsize)t.size());
            if (!o) {
                spdlog::warn("[Launcher] reticleoff: could not write gamerprofile.xml.fc5vr_tmp, the game's Reticle option left as it is");
                return;
            }
        }
        if (!MoveFileExW(with(L".fc5vr_tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            spdlog::warn("[Launcher] reticleoff: could not replace gamerprofile.xml (error {}), the game's Reticle option left as it is", GetLastError());
            return;
        }
        spdlog::info("[Launcher] reticleoff: the game's Reticle turned off in gamerprofile.xml (HelpCrosshair was {}; the file as it was: gamerprofile.xml.fc5vr_last)", was);
    } catch (...) {
        spdlog::warn("[Launcher] reticleoff: gamerprofile.xml not changed (error)");
    }
}

// cheeky (default on; a line "cheeky off" in fc5vr_settings.txt skips it): CheekyFoveatedDLSS (ClarkCheekyKent, GPL v3; the player
// installs it, the mod neither ships nor links it) loaded by the mod. Its Standalone package puts version.dll and a
// CheekyFoveatedDLSS\ folder beside FarCry5.exe, but Windows never loads that version.dll in this game: only the mod's dxgi.dll
// imports VERSION.dll, and System32's copy is already in the process by then (author 19 Nov 21:00: no Cheeky log, no ini; the
// same on the 5 Nov trial). So when both are there, the mod loads bin\version.dll itself, by full path, before its early hooks
// and before the game makes its D3D11 device. Cheeky's loader forwards the VERSION exports to System32 and starts its host
// (CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll) on its own thread; its log and ini are written in that folder.
// Cheeky keeps its own menu key, F8 (author 19 Nov 21:24); the mod no longer uses F8 (AFW / Stereo is in F11 only).
static bool g_cheeky_loaded = false;

static void LoadCheekyIfInstalled() {
    try {
        const std::filesystem::path bin    = std::filesystem::path{ fc5::exe_dir() };
        const std::filesystem::path folder = bin / L"CheekyFoveatedDLSS";
        const std::filesystem::path host   = folder / L"CheekyFoveatedDLSSHost.dll";
        const std::filesystem::path loader = bin / L"version.dll";
        std::error_code             ec;
        if (!std::filesystem::exists(host, ec)) {
            return;   // not installed: nothing to say
        }
        {
            std::ifstream s{ fc5::mod_file("fc5vr_settings.txt") };
            std::string   l;
            while (std::getline(s, l)) {
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (l == "cheeky off") {
                    spdlog::info("[Cheeky] CheekyFoveatedDLSS found in bin but not loaded: cheeky off (fc5vr_settings.txt)");
                    return;
                }
            }
        }
        if (!std::filesystem::exists(loader, ec)) {
            spdlog::warn("[Cheeky] bin\\CheekyFoveatedDLSS found but no version.dll beside FarCry5.exe: copy Cheeky's version.dll from its "
                         "Standalone zip too (not its dxgi.dll, which would replace the mod's). Cheeky not loaded");
            return;
        }
        // Cheeky's loader names its host's entry point; another mod's version.dll is left alone.
        std::string bytes;
        {
            std::ifstream f{ loader, std::ios::binary };
            bytes.assign(std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{});
        }
        if (bytes.find("CheekyHost_Start") == std::string::npos) {
            spdlog::warn("[Cheeky] bin\\CheekyFoveatedDLSS found but bin\\version.dll is not Cheeky's loader (another mod's?): Cheeky not loaded");
            return;
        }
        if (GetModuleHandleW(loader.c_str()) != nullptr) {
            g_cheeky_loaded = true;
            spdlog::info("[Cheeky] Windows already loaded bin\\version.dll (Cheeky's loader): nothing to do");
            return;
        }
        std::string sys_version = "not loaded yet";
        if (HMODULE v = GetModuleHandleW(L"version.dll")) {
            wchar_t p[MAX_PATH]{};
            GetModuleFileNameW(v, p, MAX_PATH);
            sys_version = std::filesystem::path{ p }.string();
        }
        fc5::crash::step("cheeky");
        HMODULE     h   = LoadLibraryW(loader.c_str());
        const DWORD err = h ? 0 : GetLastError();
        if (!h) {
            spdlog::warn("[Cheeky] LoadLibrary of bin\\version.dll (Cheeky's loader) FAILED, Windows error {}: Cheeky not running", err);
            return;
        }
        g_cheeky_loaded = true;
        spdlog::info("[Cheeky] Cheeky's loader bin\\version.dll loaded by the mod at {} (the game's version.dll: {}); its host starts on its own "
                     "thread, log and ini in bin\\CheekyFoveatedDLSS. cheeky off in fc5vr_settings.txt skips it",
                     fmt::ptr(h), sys_version);
    } catch (...) {
        spdlog::warn("[Cheeky] not loaded (error while checking bin\\CheekyFoveatedDLSS)");
    }
}

// A few seconds later (framework done): did Cheeky's host and runtime come up?
static void LogCheekyState() {
    if (!g_cheeky_loaded) return;
    const std::filesystem::path folder = std::filesystem::path{ fc5::exe_dir() } / L"CheekyFoveatedDLSS";
    std::error_code             ec;
    const bool                  host    = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll") != nullptr;
    const bool                  runtime = GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") != nullptr;
    const bool                  log     = std::filesystem::exists(folder / L"CheekyFoveatedDLSS-Host.log", ec);
    const bool                  ini     = std::filesystem::exists(folder / L"CheekyFoveatedDLSS.ini", ec);
    spdlog::info("[Cheeky] host {}, runtime {}, host log {}, CheekyFoveatedDLSS.ini {}{}", host ? "loaded" : "NOT loaded",
                 runtime ? "loaded" : "not loaded", log ? "written" : "missing", ini ? "present" : "missing",
                 host ? "" : " (see bin\\CheekyFoveatedDLSS for Cheeky's own log)");
}

void InitThread(HINSTANCE hModule) {
    fc5::crash::step("init thread");
    const LauncherChoice launcher = ReadLauncherChoice();
    if (launcher.flat) {
        // Flatscreen: the mod stays out of the way (no hooks, no OpenXR); dxgi.dll only forwards to the system's.
        try {
            KeepPreviousLog(fc5::mod_file("fc5vr_log.txt"));
            auto logger = spdlog::basic_logger_mt("fc5vr", fc5::mod_file("fc5vr_log.txt"), true);
            logger->set_pattern("[%H:%M:%S.%e] [%L] [tid:%t] %v");
            logger->info("[Launcher] mode flat (fc5vr_launcher.txt): the mod is off this run, the game runs on the monitor as without the mod");
            logger->flush();
        } catch (...) {
        }
        return;
    }
    InstallFileLogger();
    fc5::crash::step("log open");
    ReticleOffInProfile();
    fc5::crash::cleanup_old_reports();
    EnableDredIfRequested();
    UseTestRuntimeIfRequested();
    fc5::crash::log_boot_header();
    fc5::crash::start_boot_watchdog();
    // Before the early hooks, where Windows would have loaded it as the mod's own import (its DllMain then ran before the mod's).
    LoadCheekyIfInstalled();
    // Before the game makes its D3D11 device: the shader catalogue needs every shader it creates.
    fc5::crash::step("early hooks");
    FarCry5Stereo::install_early_hooks();
    Sleep(5000);
    fc5::crash::step("headset check");
    FarCry5Stereo::wait_headset_check();
    fc5::crash::step("framework");
    g_framework = std::make_unique<Framework>(hModule);
    RestoreFileLogger();
    fc5::crash::step("framework done");
    LogCheekyState();
    // AFW shaders compiled while the game boots (they cost the first frame ~4 s when compiled there).
    fc5::afw::precompile();
    FarCry5Stereo::log_start_notes();
    {
        char rt[1024]{};
        if (GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof(rt)) > 0) spdlog::info("[XR] OpenXR runtime for this session: {} (XR_RUNTIME_JSON)", rt);
    }
    spdlog::info("[Launcher] mode vr, headset {}", launcher.headset.empty() ? std::string{ "not chosen (no fc5vr_launcher.txt)" } : launcher.headset);
#ifdef GLM_FORCE_LEFT_HANDED
    spdlog::info("GLM_FORCE_LEFT_HANDED is defined function={} clip={}", verifyLeftHandedCoordinates(), GLM_CONFIG_CLIP_CONTROL);
    assert(verifyLeftHandedCoordinates());
#else
    spdlog::info("GLM_FORCE_LEFT_HANDED is not defined");
#endif

}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Crash filter, CRT handlers and the report thread first: a crash in the first seconds gets a report too.
        fc5::crash::install_early(hModule);
//        do {
//            Sleep(250);
//        } while (!IsDebuggerPresent());
//        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitThread, NULL, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        fc5::crash::process_exit(lpReserved != nullptr);
        break;
    }
    return TRUE;
}
