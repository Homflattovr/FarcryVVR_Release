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
#include <charconv>
#include <cmath>
#include <cstring>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
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

// gameoptions (default on; a line "gameoptions off" in fc5vr_settings.txt leaves them as the player sets them) and reticleoff
// (default on; "reticleoff off" keeps the game's Reticle): the game's own options the VR mod needs (README "Required game
// options", the same list as the launcher's RequiredOptions), set in Documents\My Games\Far Cry 5\gamerprofile.xml at every
// VR start. Author 21 Nov: players said the options stayed as they were after they let the launcher set them. The launcher
// only changed attributes the file already had (a missing one was listed, then left missing), read and wrote the file as
// UTF-8 (an ANSI file with an accented audio device name came back changed), and ran only from Play in VR; the mod set only
// the Reticle, from its init thread, which starts about 100 ms after dxgi.dll is loaded while the game carries on loading.
// Now in DllMain, the moment the game loads dxgi.dll (FarCry5.exe only, VR mode only): the game's thread waits until the file
// is right. Only kernel32 and the registry are used there (the Documents folder from User Shell Folders, as Windows' known
// folder reads it; no shell32, no log). The file's bytes are kept except the values set; a missing attribute is added to its
// element; every other option is left as it is. The file as it was is kept as gamerprofile.xml.fc5vr_backup (the first time,
// never overwritten) and .fc5vr_last; the new text goes to a temporary file that then replaces it, and a file the player made
// read-only is made read-only again. '[GameOptions]' lines once the log is open (with a second pass if Windows' known folder
// is another Documents folder); at framework done the file is read again: an option the game put back during its start is
// logged and set again for the next start.
namespace {
struct RequiredGameOption {
    const char* element;
    const char* attr;
    const char* want;
    bool        reticle;   // HelpCrosshair: reticleoff's, the others gameoptions'
};
constexpr RequiredGameOption kRequiredGameOptions[] = {
    { "RenderProfile", "VideoModeProfile", "1", false },            // the window profile (0 = exclusive Fullscreen)
    { "VideoModeProfileWindow", "windowMode", "3", false },         // Borderless
    { "RenderProfile", "FOV", "120", false },
    { "RenderProfile", "AntiAliasingMode", "1", false },            // SMAA (DLSS runs inside the game's SMAA pass)
    { "RenderProfile", "VSyncPreset", "off", false },
    { "RenderProfile", "FrameRateLockEnabled", "0", false },
    { "RenderProfile", "MotionBlur", "0", false },
    { "RenderProfile", "ResolutionScale", "1", false },
    { "RenderProfile", "DynamicResolutionEnabled", "0", false },
    { "VideoModeProfileWindow", "hdr", "0", false },
    { "ProfileSpecificGameProfile", "HelpCrosshair", "0", true },   // Interface > Reticle off
};

struct GameOptNote {
    bool        warn;
    std::string text;
};
std::vector<GameOptNote> g_gameopt_notes;           // from DllMain time, logged once the log is open
std::wstring             g_gameopt_path;            // the file DllMain checked
bool                     g_gameopt_ran     = false;
bool                     g_gameopt_all     = true;  // gameoptions on
bool                     g_gameopt_reticle = true;  // reticleoff on

std::string gameopt_join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v) s += (s.empty() ? "" : ", ") + e;
    return s;
}

std::string gameopt_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int   n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// The last "<name> ..." line of fc5vr_settings.txt says off ("off" or "0"); no line: on.
void read_gameopt_switches() {
    std::ifstream s{ fc5::mod_file("fc5vr_settings.txt") };
    std::string   l;
    while (std::getline(s, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        if (l.rfind("gameoptions ", 0) == 0) g_gameopt_all = !(l == "gameoptions off" || l == "gameoptions 0");
        if (l.rfind("reticleoff ", 0) == 0) g_gameopt_reticle = !(l == "reticleoff off" || l == "reticleoff 0");
    }
}

// The Documents folder as Windows' known folder gives it (FOLDERID_Documents = User Shell Folders\Personal, OneDrive's
// folder when Documents is backed up there), read from the registry so it can run under the loader lock.
std::wstring documents_from_registry() {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
    wchar_t        raw[1024]{};
    DWORD          size = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Personal", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) == ERROR_SUCCESS && raw[0]) {
        wchar_t     out[1024]{};
        const DWORD n = ExpandEnvironmentStringsW(raw, out, (DWORD)std::size(out));
        if (n > 0 && n <= std::size(out) && out[0]) return out;
    }
    size = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Personal", RRF_RT_REG_SZ, nullptr, raw, &size) == ERROR_SUCCESS && raw[0]) {
        return raw;
    }
    wchar_t     prof[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring{ prof } + L"\\Documents" : std::wstring{};
}

std::wstring profile_in(const std::wstring& docs) {
    if (docs.empty()) return {};
    std::wstring p = docs;
    if (p.back() != L'\\' && p.back() != L'/') p += L'\\';
    return p + L"My Games\\Far Cry 5\\gamerprofile.xml";
}

bool same_game_value(const std::string& have, const char* want) {
    const char* we = want + std::strlen(want);
    double      a = 0.0, b = 0.0;
    const auto  ra = std::from_chars(have.data(), have.data() + have.size(), a);
    const auto  rb = std::from_chars(want, we, b);
    if (ra.ec == std::errc{} && ra.ptr == have.data() + have.size() && rb.ec == std::errc{} && rb.ptr == we) return std::fabs(a - b) < 1e-3;
    return _stricmp(have.c_str(), want) == 0;
}

// attr="want" in element's opening tag (the first one): set, or added after the element's name when the tag lacks it.
// False when the file has no such element. was: the old value, "missing" when added.
bool set_game_attr(std::string& t, const char* element, const char* attr, const char* want, std::string& was, bool& changed) {
    changed               = false;
    const std::string tag_open = std::string{ "<" } + element;
    size_t            tag  = t.find(tag_open);
    for (; tag != std::string::npos; tag = t.find(tag_open, tag + 1)) {
        const char c = tag + tag_open.size() < t.size() ? t[tag + tag_open.size()] : '\0';
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>') break;
    }
    if (tag == std::string::npos) return false;
    const size_t end = t.find('>', tag);
    if (end == std::string::npos) return false;
    const std::string at = std::string{ attr } + "=\"";
    for (size_t a = t.find(at, tag); a != std::string::npos && a < end; a = t.find(at, a + 1)) {
        if (!std::isspace((unsigned char)t[a - 1])) continue;
        const size_t v = a + at.size();
        const size_t q = t.find('"', v);
        if (q == std::string::npos || q > end) return false;
        was = t.substr(v, q - v);
        if (same_game_value(was, want)) return true;
        t.replace(v, q - v, want);
        changed = true;
        return true;
    }
    was = "missing";
    t.insert(tag + tag_open.size(), std::string{ " " } + attr + "=\"" + want + "\"");
    changed = true;
    return true;
}

bool read_whole_file(const std::wstring& p, std::string& out, DWORD& err) {
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    bool          ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < 16ll * 1024 * 1024;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok        = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

bool write_whole_file(const std::wstring& p, const std::string& t, DWORD& err) {
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    DWORD      put = 0;
    const bool ok  = WriteFile(h, t.data(), (DWORD)t.size(), &put, nullptr) && put == t.size();
    if (ok) FlushFileBuffers(h);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

// The required options set in the file at p. after_start: the check at framework done (the game has read the file by then).
void enforce_game_options(const std::wstring& p, bool after_start, std::vector<GameOptNote>& notes) {
    const std::string where = gameopt_utf8(p);
    std::string       t;
    DWORD             err = 0;
    if (!read_whole_file(p, t, err)) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            notes.push_back({ false, fmt::format("[GameOptions] no {} yet (the game writes it at its first start): the options the mod needs are set at the next VR start", where) });
        else
            notes.push_back({ true, fmt::format("[GameOptions] could not read {} (error {}): the game's options not checked", where, err) });
        return;
    }
    std::vector<std::string> set, no_element;
    int                      checked = 0;
    for (const auto& o : kRequiredGameOptions) {
        if (o.reticle ? !g_gameopt_reticle : !g_gameopt_all) continue;
        std::string was;
        bool        changed = false;
        if (!set_game_attr(t, o.element, o.attr, o.want, was, changed)) {
            no_element.push_back(o.attr);
            continue;
        }
        ++checked;
        if (changed) set.push_back(fmt::format("{} {} -> {}", o.attr, was, o.want));
    }
    const std::string skipped = std::string{ g_gameopt_all ? "" : "; gameoptions off: only the Reticle checked" } +
                                (g_gameopt_reticle ? "" : "; reticleoff off: the Reticle left as it is") +
                                (no_element.empty() ? "" : fmt::format("; not in the file (no element): {}", gameopt_join(no_element)));
    if (set.empty()) {
        notes.push_back({ false, fmt::format("[GameOptions] {}: the {} options the mod needs are set in {}{}", after_start ? "after the game's start" : "at load",
                                             checked, where, skipped) });
        return;
    }
    const auto  with = [&](const wchar_t* ext) { return p + ext; };
    CopyFileW(p.c_str(), with(L".fc5vr_backup").c_str(), TRUE);   // the first time only
    CopyFileW(p.c_str(), with(L".fc5vr_last").c_str(), FALSE);
    const DWORD fa       = GetFileAttributesW(p.c_str());
    const bool  readonly = fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_READONLY);
    if (readonly) SetFileAttributesW(p.c_str(), fa & ~FILE_ATTRIBUTE_READONLY);
    bool        done = false;
    std::string how;
    if (write_whole_file(with(L".fc5vr_tmp"), t, err)) {
        done = MoveFileExW(with(L".fc5vr_tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!done) {
            how = fmt::format("replacing it failed (error {}), written in place", GetLastError());
            DeleteFileW(with(L".fc5vr_tmp").c_str());
        }
    } else {
        how = fmt::format("no temporary file (error {}), written in place", err);
    }
    if (!done) done = write_whole_file(p, t, err);
    if (readonly) SetFileAttributesW(p.c_str(), GetFileAttributesW(p.c_str()) | FILE_ATTRIBUTE_READONLY);
    if (!done) {
        notes.push_back({ true, fmt::format("[GameOptions] could not write {} (error {}{}{}): the game starts with its options as they are ({} not as the mod needs: {})",
                                            where, err, how.empty() ? "" : "; ", how, set.size(), gameopt_join(set)) });
        return;
    }
    const std::string tail = std::string{ how.empty() ? "" : "; " } + how + (readonly ? "; the file was read-only, kept read-only" : "") + skipped;
    if (after_start)
        notes.push_back({ true, fmt::format("[GameOptions] after the game's start {} of the options were not as the mod needs in {} (the game put them back): set again, "
                                            "they apply from the next start: {}{} (the file as it was: gamerprofile.xml.fc5vr_last)",
                                            set.size(), where, gameopt_join(set), tail) });
    else
        notes.push_back({ false, fmt::format("[GameOptions] at load, set {} of the options the mod needs in {}: {}{} (the file as it was: gamerprofile.xml.fc5vr_last)",
                                             set.size(), where, gameopt_join(set), tail) });
}
}   // namespace

// DllMain time (loader lock held, the game's thread waiting in LoadLibrary): no log, no shell32.
static void EnforceGameOptionsAtLoad() {
    try {
        wchar_t     exe[MAX_PATH]{};
        const DWORD n    = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        if (n == 0 || _wcsicmp(name ? name + 1 : exe, L"FarCry5.exe") != 0) return;   // the game only (not the Arcade editor)
        if (ReadLauncherChoice().flat) return;                                          // Flatscreen: the mod stays out of the way
        g_gameopt_ran = true;
        read_gameopt_switches();
        if (!g_gameopt_all && !g_gameopt_reticle) {
            g_gameopt_notes.push_back({ false, "[GameOptions] gameoptions off and reticleoff off (fc5vr_settings.txt): the game's options left as they are" });
            return;
        }
        g_gameopt_path = profile_in(documents_from_registry());
        if (g_gameopt_path.empty()) {
            g_gameopt_notes.push_back({ true, "[GameOptions] no Documents folder found: the game's options not checked" });
            return;
        }
        enforce_game_options(g_gameopt_path, false, g_gameopt_notes);
    } catch (...) {
        g_gameopt_notes.push_back({ true, "[GameOptions] gamerprofile.xml not changed at load (error)" });
    }
}

// Init thread, once the log is open: DllMain's lines, and the known folder's Documents if it is another folder.
static void LogGameOptions() {
    if (!g_gameopt_ran) return;
    try {
        std::vector<GameOptNote> notes;
        notes.swap(g_gameopt_notes);
        if (!g_gameopt_path.empty()) {
            PWSTR docs = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs != nullptr) {
                const std::wstring known = profile_in(docs);
                if (_wcsicmp(known.c_str(), g_gameopt_path.c_str()) != 0) {
                    notes.push_back({ true, fmt::format("[GameOptions] Windows' Documents folder ({}) is not the registry's ({}): that file checked too",
                                                        gameopt_utf8(known), gameopt_utf8(g_gameopt_path)) });
                    enforce_game_options(known, false, notes);
                    g_gameopt_path = known;
                }
            }
            if (docs) CoTaskMemFree(docs);
        }
        for (const auto& n : notes) {
            if (n.warn) spdlog::warn("{}", n.text);
            else spdlog::info("{}", n.text);
        }
    } catch (...) {
        spdlog::warn("[GameOptions] could not log the game's options check (error)");
    }
}

// Framework done (the game has made its device, so it has read the file): did its start put an option back?
static void CheckGameOptionsAfterStart() {
    if (!g_gameopt_ran || g_gameopt_path.empty() || (!g_gameopt_all && !g_gameopt_reticle)) return;
    try {
        std::vector<GameOptNote> notes;
        enforce_game_options(g_gameopt_path, true, notes);
        for (const auto& n : notes) {
            if (n.warn) spdlog::warn("{}", n.text);
            else spdlog::info("{}", n.text);
        }
    } catch (...) {
        spdlog::warn("[GameOptions] could not check the game's options after its start (error)");
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
    LogGameOptions();
    fc5::crash::cleanup_old_reports();
    EnableDredIfRequested();
    UseTestRuntimeIfRequested();
    fc5::crash::log_boot_header();
    fc5::crash::start_boot_watchdog();
    // The resolution hooks (the game's CreateSwapChain) before Cheeky: its host hooks the same function from its own thread
    // (MinHook). Ours landing while its hook was being written left ours out of the chain: the game's swap chain was never
    // forced to the eye size and the game rendered at its window size, stretched into each eye (author 21 Nov 20:41 and 20:44,
    // 1.0.4: ours 2-3 ms into Cheeky's MinHook enable both runs; the good runs of 19 and 20 Nov had ours 1 ms before it).
    FarCry5Stereo::install_swap_chain_hooks();
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
    CheckGameOptionsAfterStart();
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
        // The game's own options the mod needs, before the game's thread goes on (gamerprofile.xml; logged by the init thread).
        EnforceGameOptionsAtLoad();
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
