// Far Cry 5 VR launcher: choose VR or flat screen, the resolution multiplier and the headset, then start the game.
// It lives in the game's bin folder (next to FarCry5.exe and the mod's dxgi.dll) and writes there:
//   fc5vr_launcher.txt   "mode vr" or "mode flat", "headset quest3" and the headset's typical eye size "headsetsize <w> <h>"
//                        (read by the mod at start, src\Main.cpp and FarCry5Stereo.cpp load_forced_resolution)
//   fc5vr_settings.txt   only its "render", "resscale", "dlss", "cinematics", "loadpopup" and "sessionlogs" lines, the same ones F11 changes
// Built by build_launcher.bat (C# 5, so the compiler in Windows' .NET Framework can build it too).
// After Play in VR it stays minimised until the game closes; if the game closed during the start (VR never ran) or
// crashed, it offers a report zip (FC5VR_report_<date>.zip in bin: logs, crash report, system and Windows event
// details; nothing is uploaded). "Make a report" makes the same zip at any time.
// Upscaler "Add FSR..." installs OptiScaler (FSR in place of DLSS) from the player's own download, "Remove FSR" removes it
// (FC5VR_OptiScaler.cs).
// "Install checks" (right column): the mod's files, the OpenXR runtime and layers, ViGEmBus, the VC++ runtime, other mods in
// bin, game overlays and the game's options, each PASS / WARN / FAIL from the tester reports where the install stopped VR.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("Far Cry 5 VR launcher")]
[assembly: System.Reflection.AssemblyProduct("Far Cry 5 VR mod")]

static class Launcher
{
    const string SteamUrl = "steam://rungameid/552520";

    // The headsets in the list (author 17 Oct: other common headsets added). The mod takes each headset's field of view from
    // its runtime every frame and its eye size from the runtime at start; the size here is only a typical one (per eye at
    // 100% in its usual runtime), used when the headset cannot be asked (not connected at start). Runtimes: the OpenXR
    // runtimes it normally runs on (labels of ActiveRuntime), empty for any.
    public class Headset
    {
        public readonly string Name, Id, Connect;
        public readonly int W, H;
        public readonly string[] Runtimes;
        public Headset(string name, string id, int w, int h, string connect, params string[] runtimes)
        {
            Name = name; Id = id; W = w; H = h; Connect = connect; Runtimes = runtimes;
        }
    }
    const string VDXR = "Virtual Desktop (VDXR)", Link = "Meta Quest Link", SteamVR = "SteamVR", WMR = "Windows Mixed Reality", Pimax = "Pimax",
                 Varjo = "Varjo", Pico = "Pico Connect";
    static readonly Headset[] Headsets = {
        new Headset("Meta Quest 3", "quest3", 2064, 2208, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest 3S", "quest3s", 1832, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest 2", "quest2", 1832, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest Pro", "questpro", 1800, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Pico 4 / 4 Ultra", "pico4", 2160, 2160, "Virtual Desktop, Pico Connect or SteamVR.", VDXR, Pico, SteamVR),
        new Headset("Valve Index", "index", 2016, 2240, "SteamVR.", SteamVR),
        new Headset("HTC Vive Pro 2", "vivepro2", 2448, 2448, "SteamVR.", SteamVR),
        new Headset("HP Reverb G2", "reverbg2", 2160, 2160, "SteamVR, or Windows Mixed Reality on Windows versions that still have it.", WMR, SteamVR),
        new Headset("Bigscreen Beyond", "beyond", 2560, 2560, "SteamVR.", SteamVR),
        new Headset("Pimax Crystal", "pimaxcrystal", 2880, 2880, "Pimax Play (its OpenXR runtime) or SteamVR. The mod renders each eye's own turned view, so parallel projections are not needed.", Pimax, SteamVR),
        new Headset("Varjo Aero", "varjoaero", 2880, 2720, "Varjo Base (its OpenXR runtime) or SteamVR.", Varjo, SteamVR),
        new Headset("Other OpenXR headset", "other", 0, 0, "Any OpenXR runtime set as the system default."),
    };

    public static Headset FindHeadset(string id)
    {
        foreach (var h in Headsets) if (h.Id == id) return h;
        return null;
    }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    static string Dir;
    static string LauncherFile { get { return Path.Combine(Dir, "fc5vr_launcher.txt"); } }
    static string SettingsFile { get { return Path.Combine(Dir, "fc5vr_settings.txt"); } }

    // Steam's launch options "<launcher path>" %command% make Steam's Play open the launcher with the game's own
    // command line (FarCry5.exe and its arguments); Play then runs that command.
    static string[] GameCommand;

    [STAThread]
    static void Main(string[] args)
    {
        try { SetProcessDPIAware(); } catch { }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Dir = AppDomain.CurrentDomain.BaseDirectory;
        if (args.Length > 0 && args[0].EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(args[0])) GameCommand = args;
        if (!File.Exists(Path.Combine(Dir, "FarCry5.exe"))) {
            // The zip extracted into bin (its own bin folder inside the game's): bin\bin\FC5VR_Launcher.exe.
            var up = Path.GetDirectoryName(Dir.TrimEnd('\\', '/'));
            bool nested = up != null && File.Exists(Path.Combine(up, "FarCry5.exe"));
            MessageBox.Show(nested ? "The mod's files are in a bin folder inside the game's bin folder (" + Dir.TrimEnd('\\') + "). Move every file of this folder up one " +
                                     "level, next to FarCry5.exe in " + up + ", then start FC5VR_Launcher.exe from there."
                                   : "Put FC5VR_Launcher.exe in Far Cry 5's bin folder, next to FarCry5.exe and the mod's dxgi.dll.",
                            "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        Application.Run(new LauncherForm());
    }

    // ---- files ----

    // The upscaler list, as F11 Display > Image quality "Upscaler" (FarCry5Upscaler.cpp): OptiScaler's proxy dll in place and Dx11Upscaler in
    // OptiScaler.ini, or the dll renamed for DLSS. The FSR choices appear once OptiScaler is installed: by the launcher's Add FSR
    // (FC5VR_OptiScaler.cs) or tools\fc5_optiscaler.ps1, which write the same fc5vr_optiscaler_manifest.txt.
    public static readonly string[] UpscalerNames = { "DLSS", "FSR 3.1", "FSR through FFX (FSR 4 on Radeon RX 9000, heavier)", "FSR 2.2" };
    static readonly string[] UpscalerValues = { "", "fsr31", "ffx_12", "fsr22" };
    static string UpscalerProxy()
    {
        try {
            var manifest = Path.Combine(Dir, "fc5vr_optiscaler_manifest.txt");
            if (!File.Exists(manifest)) return "";
            var m = Regex.Match(File.ReadAllLines(manifest)[0], @"proxy (\S+)\s*$");
            return m.Success ? m.Groups[1].Value : "";
        } catch (Exception) { return ""; }
    }
    public static bool UpscalerInstalled() { return UpscalerProxy() != ""; }
    public static int ReadUpscaler()
    {
        var proxy = UpscalerProxy();
        if (proxy == "" || !File.Exists(Path.Combine(Dir, proxy))) return 0;
        try {
            foreach (var raw in File.ReadAllLines(Path.Combine(Dir, "OptiScaler.ini"))) {
                var l = raw.Trim();
                if (!l.StartsWith("Dx11Upscaler=")) continue;
                int i = Array.IndexOf(UpscalerValues, l.Substring(13));
                return i > 0 ? i : 0;
            }
        } catch (Exception) { }
        return 0;
    }
    public static void WriteUpscaler(int choice)
    {
        var proxy = UpscalerProxy();
        if (proxy == "") return;
        string on = Path.Combine(Dir, proxy), off = on + ".fc5vr_off", ini = Path.Combine(Dir, "OptiScaler.ini");
        if (choice <= 0) {
            if (File.Exists(on)) {
                if (File.Exists(off)) File.Delete(off);
                File.Move(on, off);
            }
            return;
        }
        var lines = File.ReadAllLines(ini);
        for (int i = 0; i < lines.Length; ++i) {
            if (lines[i].TrimStart().StartsWith("Dx11Upscaler=")) lines[i] = "Dx11Upscaler=" + UpscalerValues[choice];
            // Game overlays off with FSR (FC5VR_OptiScaler.cs WriteModSettings): installs made before 13 Nov had them on, and the
            // Ubisoft overlay crashes on OptiScaler's wrapped swap chain on AMD / Intel cards.
            else if (lines[i].TrimStart().StartsWith("DisableOverlays=")) lines[i] = "DisableOverlays=true";
        }
        File.WriteAllLines(ini, lines, new UTF8Encoding(false));
        if (!File.Exists(on) && File.Exists(off)) File.Move(off, on);
    }

    static string ResolutionFile { get { return Path.Combine(Dir, "fc5vr_resolution.txt"); } }

    // DLSS render scale, as F11 Display > Image quality "DLSS render scale" (fc5vr_resolution.txt "auto <ratio>").
    public static readonly string[] DlssModes = { "Native (DLAA)", "Quality (0.67)", "Balanced (0.58)", "Performance (0.50)" };
    public static readonly float[] DlssRatios = { 1.00f, 0.67f, 0.58f, 0.50f };

    // DLSS: fc5vr_settings.txt "dlss on|off" (as F11's DLSS checkbox) and fc5vr_resolution.txt "auto <ratio>".
    // A resolution file that is not "auto" (a fixed size, or "off": test setups) is left alone unless the mode is changed;
    // dlssMode is -1 then.
    public static void ReadDlss(out bool on, out int mode)
    {
        on = true;
        mode = 0;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("dlss ")) on = !(l == "dlss off" || l == "dlss 0");
                }
            if (File.Exists(ResolutionFile)) {
                var parts = File.ReadAllText(ResolutionFile).Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                float r;
                if (parts.Length >= 2 && parts[0] == "auto" && float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out r)) {
                    for (int i = 0; i < DlssRatios.Length; ++i) if (Math.Abs(DlssRatios[i] - r) < 0.02f) mode = i;
                } else if (parts.Length >= 1 && parts[0] != "auto") mode = -1;
            }
        } catch { }
    }

    // Rendering: fc5vr_settings.txt "render stereo|afw" (F11 Display "Rendering", F8). No line: AFW, the mod's start mode.
    public static bool ReadStereo()
    {
        bool stereo = false;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("render ")) stereo = l.Substring(7).Trim() == "stereo";
                }
        } catch { }
        return stereo;
    }

    // Cutscenes: fc5vr_settings.txt "cinematics flat|3d|off" (F11 Comfort > Cinematics and vehicles, read as the mod reads it).
    // 0 flat screen, 1 3D, 2 unchanged ("off", only F11 sets it: kept unless another choice is picked). No line: 3D, the mod's default.
    public static readonly string[] CutsceneModes = { "Flat screen in VR", "Full stereo VR", "Unchanged (set in F11)" };
    public static int ReadCutscenes()
    {
        int mode = 1;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (!l.StartsWith("cinematics ")) continue;
                    var a = l.Substring(11).Trim();
                    mode = a == "3d" ? 1 : (a == "off" || a == "unchanged") ? 2 : 0;
                }
        } catch { }
        return mode;
    }

    // Loading popup: fc5vr_settings.txt "loadpopup off" (F11 HUD and menus > Loading popup). No line: shown, the mod's default.
    public static bool ReadSkipPopup()
    {
        bool skip = false;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("loadpopup ")) skip = l == "loadpopup off" || l == "loadpopup 0";
                }
        } catch { }
        return skip;
    }

    // Logs during sessions: fc5vr_settings.txt "sessionlogs on|off" (F11 Main "Logs during sessions (slower)", src\FarCry5\FarCry5SessionLogs.h).
    // No line: on, the mod's default for now.
    public static bool ReadSessionLogs()
    {
        bool on = true;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("sessionlogs ")) on = !(l == "sessionlogs off" || l == "sessionlogs 0" || l == "sessionlogs false");
                }
        } catch { }
        return on;
    }

    public static void ReadChoice(out bool flat, out string headset, out float scale)
    {
        flat = false;
        headset = Headsets[0].Id;
        scale = 1.0f;
        try {
            if (File.Exists(LauncherFile)) {
                foreach (var raw in File.ReadAllLines(LauncherFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("mode ")) flat = l.Substring(5).Trim() == "flat";
                    if (l.StartsWith("headset ")) headset = l.Substring(8).Trim();
                }
            }
            if (File.Exists(SettingsFile)) {
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    float v;
                    if (l.StartsWith("resscale ") && float.TryParse(l.Substring(9).Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v))
                        scale = Math.Max(1.0f, Math.Min(2.0f, v));
                }
            }
        } catch { }
    }

    // Replaces the line starting with "<key> " (the first one; later ones removed), or adds it.
    static void SetLine(List<string> lines, string key, string line)
    {
        bool found = false;
        for (int i = 0; i < lines.Count; ++i) {
            if (lines[i].Trim().StartsWith(key + " ")) {
                if (found) { lines.RemoveAt(i--); continue; }
                lines[i] = line;
                found = true;
            }
        }
        if (!found) lines.Add(line);
    }

    // dlssMode -1: fc5vr_resolution.txt left as it is.
    // cutscenes 2: the "cinematics" line left as it is.
    public static void WriteChoice(bool flat, string headset, bool stereo, float scale, bool dlss, int dlssMode, int cutscenes, bool skipPopup, bool sessionLogs)
    {
        var hs = FindHeadset(headset);
        File.WriteAllText(LauncherFile, "mode " + (flat ? "flat" : "vr") + "\r\nheadset " + headset + "\r\n" +
                          (hs != null && hs.W > 0 ? "headsetsize " + hs.W + " " + hs.H + "\r\n" : ""), new UTF8Encoding(false));
        if (dlssMode >= 0)
            File.WriteAllText(ResolutionFile, "auto " + DlssRatios[dlssMode].ToString("0.00", CultureInfo.InvariantCulture) + "\n", new UTF8Encoding(false));
        // The settings file is the mod's: only the render, resscale, dlss and cinematics lines change (added if missing), the rest is kept as it is.
        var lines = new List<string>();
        if (File.Exists(SettingsFile)) lines.AddRange(File.ReadAllLines(SettingsFile));
        else lines.Add("# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).");
        lines.RemoveAll(l => l.Trim().StartsWith("afw "));   // the old form of the render line (the mod drops it too)
        SetLine(lines, "render", stereo ? "render stereo" : "render afw");
        SetLine(lines, "resscale", "resscale " + scale.ToString("0.0", CultureInfo.InvariantCulture));
        SetLine(lines, "dlss", dlss ? "dlss on" : "dlss off");
        if (cutscenes != 2) SetLine(lines, "cinematics", cutscenes == 0 ? "cinematics flat" : "cinematics 3d");
        // The loading popup's line only when it is skipped or already there (no line: shown).
        if (skipPopup || lines.Exists(l => l.Trim().StartsWith("loadpopup "))) SetLine(lines, "loadpopup", skipPopup ? "loadpopup off" : "loadpopup on");
        SetLine(lines, "sessionlogs", sessionLogs ? "sessionlogs on" : "sessionlogs off");
        // The mod writes with \n; keep it.
        var tmp = SettingsFile + ".launcher.tmp";
        File.WriteAllText(tmp, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
        if (File.Exists(SettingsFile)) File.Replace(tmp, SettingsFile, null);
        else File.Move(tmp, SettingsFile);
    }

    // The headset's recommended eye size, written by the mod once VR has started (fc5vr_xr_size.txt "<w> <h> <headset>";
    // without a headset it was the Quest 3's).
    public static bool HeadsetEyeSize(out int w, out int h, out string headset)
    {
        w = h = 0;
        headset = "quest3";
        try {
            var p = Path.Combine(Dir, "fc5vr_xr_size.txt");
            if (!File.Exists(p)) return false;
            var parts = File.ReadAllText(p).Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 3) headset = parts[2];
            return parts.Length >= 2 && int.TryParse(parts[0], out w) && int.TryParse(parts[1], out h) && w > 0 && h > 0;
        } catch { return false; }
    }

    // The eye size for a headset: the mod's measured one when it is that headset's, else its typical one.
    public static bool EyeSizeFor(string id, out int w, out int h, out bool measured)
    {
        string tag;
        measured = HeadsetEyeSize(out w, out h, out tag) && tag == id;
        if (measured) return true;
        var hs = FindHeadset(id);
        w = hs != null ? hs.W : 0;
        h = hs != null ? hs.H : 0;
        return w > 0;
    }

    // What the headset reported in the mod's last VR run (fc5vr_xr_view.txt, "<key> <values>" lines), if it was this headset.
    public static Dictionary<string, string> LastView(string id)
    {
        var d = new Dictionary<string, string>();
        try {
            var p = Path.Combine(Dir, "fc5vr_xr_view.txt");
            if (!File.Exists(p)) return null;
            foreach (var raw in File.ReadAllLines(p)) {
                var l = raw.Trim();
                int s = l.IndexOf(' ');
                if (s > 0) d[l.Substring(0, s)] = l.Substring(s + 1).Trim();
            }
        } catch { return null; }
        string hs;
        return d.TryGetValue("headset", out hs) && hs == id ? d : null;
    }

    // ---- the game's own options (Documents\My Games\Far Cry 5\gamerprofile.xml) ----
    // The game reads this file when it starts and writes it when its options change, so it is checked and set only
    // while the game is closed. Play in VR sets them without asking (author 21 Nov: the settings the mod needs forced;
    // players had answered Yes and still had them wrong), and the mod sets them again at each VR start (Main.cpp
    // EnforceGameOptionsAtLoad), also when the game is started without the launcher. "gameoptions off" in fc5vr_settings.txt
    // leaves them as the player sets them (the Reticle: "reticleoff off").
    // The file is read and written byte for byte (Latin-1 maps each byte to one char and back): it used to go through UTF-8,
    // which changed any non-ASCII byte of an ANSI file (an accented audio device name) into a replacement character. The values are the README's "Required game options" (the author's own file, 24 Oct):
    // VideoModeProfile 1 = the window profile, whose windowMode 3 is Borderless (0 = the exclusive Fullscreen profile),
    // AntiAliasingMode 1 = SMAA (DLSS runs inside the game's SMAA pass), HelpCrosshair 0 = Interface > Reticle off.
    public class GameOption
    {
        public readonly string Element, Attr, Want, Label;
        public string Have;   // the file's value, null when the attribute is missing
        public GameOption(string element, string attr, string want, string label) { Element = element; Attr = attr; Want = want; Label = label; }
    }

    static GameOption[] RequiredOptions()
    {
        return new[] {
            new GameOption("RenderProfile", "VideoModeProfile", "1", "Window mode: Borderless"),
            new GameOption("VideoModeProfileWindow", "windowMode", "3", "Window mode: Borderless"),
            new GameOption("RenderProfile", "FOV", "120", "Field of view: 120"),
            new GameOption("RenderProfile", "AntiAliasingMode", "1", "Anti-aliasing: SMAA (DLSS runs in its place)"),
            new GameOption("RenderProfile", "VSyncPreset", "off", "V-Sync: Off"),
            new GameOption("RenderProfile", "FrameRateLockEnabled", "0", "Frame rate limit: Off"),
            new GameOption("RenderProfile", "MotionBlur", "0", "Motion blur: Off"),
            new GameOption("RenderProfile", "ResolutionScale", "1", "Resolution scale: 100 %"),
            new GameOption("RenderProfile", "DynamicResolutionEnabled", "0", "Dynamic resolution: Off"),
            new GameOption("VideoModeProfileWindow", "hdr", "0", "HDR: Off"),
            new GameOption("ProfileSpecificGameProfile", "HelpCrosshair", "0", "Crosshair (Interface > Reticle): Off"),
        };
    }

    static readonly Encoding ProfileBytes = Encoding.GetEncoding(28591);
    static string ReadGameProfile(string p) { return ProfileBytes.GetString(File.ReadAllBytes(p)); }

    // fc5vr_settings.txt "gameoptions off" / "reticleoff off" (the mod's switches, last line wins). No line: on.
    static bool SettingOn(string name)
    {
        bool on = true;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith(name + " ")) on = !(l == name + " off" || l == name + " 0");
                }
        } catch { }
        return on;
    }

    // The options the mod's switches leave to the launcher and the mod (HelpCrosshair: reticleoff, the others: gameoptions).
    static List<GameOption> SwitchedOptions()
    {
        bool all = SettingOn("gameoptions"), reticle = SettingOn("reticleoff");
        var l = new List<GameOption>();
        foreach (var o in RequiredOptions()) if (o.Attr == "HelpCrosshair" ? reticle : all) l.Add(o);
        return l;
    }

    public static string GameProfilePath
    {
        get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), @"My Games\Far Cry 5\gamerprofile.xml"); }
    }

    // Where the element's opening tag starts (the first one), or -1.
    static int FindTag(string t, string element)
    {
        var m = new Regex("<" + Regex.Escape(element) + "[\\s/>]").Match(t);
        return m.Success ? m.Index : -1;
    }

    // The span of an attribute's value inside the element's opening tag, or false.
    static bool FindAttr(string t, string element, string attr, out int start, out int length)
    {
        start = length = 0;
        int tag = FindTag(t, element);
        if (tag < 0) return false;
        int end = t.IndexOf('>', tag);
        if (end < 0) return false;
        var m = new Regex("\\s" + Regex.Escape(attr) + "=\"([^\"]*)\"").Match(t, tag, end - tag);
        if (!m.Success) return false;
        start = m.Groups[1].Index;
        length = m.Groups[1].Length;
        return true;
    }

    static bool SameValue(string have, string want)
    {
        double a, b;
        if (double.TryParse(have, NumberStyles.Float, CultureInfo.InvariantCulture, out a) && double.TryParse(want, NumberStyles.Float, CultureInfo.InvariantCulture, out b))
            return Math.Abs(a - b) < 1e-3;
        return string.Equals(have, want, StringComparison.OrdinalIgnoreCase);
    }

    // The required options the game's file does not have (an option missing from the file counts as wrong only when
    // the file has its element). Null when there is no file yet (the game has not run) or it cannot be read.
    public static List<GameOption> WrongGameOptions()
    {
        try {
            var p = GameProfilePath;
            if (!File.Exists(p)) return null;
            var t = ReadGameProfile(p);
            var wrong = new List<GameOption>();
            foreach (var o in SwitchedOptions()) {
                int s, n;
                if (FindAttr(t, o.Element, o.Attr, out s, out n)) o.Have = t.Substring(s, n);
                else if (FindTag(t, o.Element) < 0) continue;
                if (o.Have == null || !SameValue(o.Have, o.Want)) wrong.Add(o);
            }
            return wrong;
        } catch { return null; }
    }

    // The wrong options' labels, one each (both window mode values are one option in the game's menu).
    public static List<string> WrongLabels(List<GameOption> wrong)
    {
        var l = new List<string>();
        if (wrong != null) foreach (var o in wrong) if (!l.Contains(o.Label)) l.Add(o.Label);
        return l;
    }

    // Sets the required options in the game's file (the game must be closed), a missing attribute added to its element (it
    // used to be left missing, so the option stayed as the game's default and Play in VR asked again). Backups first: the file
    // as it was the first time is kept as gamerprofile.xml.fc5vr_backup (never overwritten), and the file as it was just before
    // this change as gamerprofile.xml.fc5vr_last. Read back afterwards: throws when the file still is not as the mod needs.
    // True when the file was changed.
    public static bool FixGameOptions()
    {
        return SetOptions(SwitchedOptions());
    }

    static bool SetOptions(List<GameOption> options)
    {
        var p = GameProfilePath;
        var was = ReadGameProfile(p);
        var t = was;
        foreach (var o in options) t = SetAttrText(t, o.Element, o.Attr, o.Want) ?? t;
        if (t == was) return false;
        WriteGameProfile(p, t);
        var left = WrongGameOptions();
        if (left == null) throw new IOException("the file could not be read back after the change");
        left = left.FindAll(w => options.Exists(o => o.Element == w.Element && o.Attr == w.Attr));
        if (left.Count > 0) throw new IOException("the file still has " + string.Join(", ", left.ConvertAll(o => o.Attr + "=" + (o.Have ?? "missing"))) + " after the change");
        return true;
    }

    // The file's text with attr="want" in element's opening tag (set, or added after the element's name), or null when
    // nothing changes or the file has no such element.
    static string SetAttrText(string t, string element, string attr, string want)
    {
        int s, n;
        if (FindAttr(t, element, attr, out s, out n))
            return SameValue(t.Substring(s, n), want) ? null : t.Substring(0, s) + want + t.Substring(s + n);
        int tag = FindTag(t, element);
        if (tag < 0) return null;
        int at = tag + 1 + element.Length;
        return t.Substring(0, at) + " " + attr + "=\"" + want + "\"" + t.Substring(at);
    }

    // Backups first: the file as it was the first time is kept as gamerprofile.xml.fc5vr_backup (never overwritten), and the
    // file as it was just before this change as gamerprofile.xml.fc5vr_last. Written byte for byte; a file the player made
    // read-only is made read-only again.
    static void WriteGameProfile(string p, string t)
    {
        var bak = p + ".fc5vr_backup";
        if (!File.Exists(bak)) File.Copy(p, bak);
        File.Copy(p, p + ".fc5vr_last", true);
        var attrs = File.GetAttributes(p);
        bool readOnly = (attrs & FileAttributes.ReadOnly) != 0;
        if (readOnly) File.SetAttributes(p, attrs & ~FileAttributes.ReadOnly);
        try {
            File.WriteAllBytes(p, ProfileBytes.GetBytes(t));
        } finally {
            if (readOnly) File.SetAttributes(p, File.GetAttributes(p) | FileAttributes.ReadOnly);
        }
    }

    // The system's OpenXR runtime (the one the headset uses), from the Khronos registry key.
    public static string ActiveRuntime()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1")) {
                var v = k == null ? null : k.GetValue("ActiveRuntime") as string;
                if (string.IsNullOrEmpty(v)) return null;
                var low = v.ToLowerInvariant();
                if (low.Contains("simulator")) return "Meta XR Simulator";
                if (low.Contains("virtual desktop") || low.Contains("virtualdesktop")) return VDXR;
                if (low.Contains("oculus") || low.Contains("meta")) return Link;
                if (low.Contains("steamvr") || low.Contains("steamxr")) return SteamVR;
                if (low.Contains("mixedreality")) return WMR;
                if (low.Contains("pimax") || low.Contains("piopenxr")) return Pimax;
                if (low.Contains("varjo")) return Varjo;
                if (low.Contains("pico")) return Pico;
                return Path.GetFileNameWithoutExtension(v);
            }
        } catch { return null; }
    }

    // The runtime manifest's name and DLL (library_path, relative to the manifest), and whether that DLL exists.
    public static bool RuntimeManifest(string json, out string name, out string library)
    {
        name = library = null;
        try {
            if (string.IsNullOrEmpty(json) || !File.Exists(json)) return false;
            var t = File.ReadAllText(json);
            var n = Regex.Match(t, "\"name\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            var l = Regex.Match(t, "\"library_path\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            if (n.Success) name = n.Groups[1].Value.Replace("\\\\", "\\");
            if (!l.Success) return false;
            library = l.Groups[1].Value.Replace("\\\\", "\\").Replace("/", "\\");
            if (!Path.IsPathRooted(library)) library = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(json), library));
            return File.Exists(library);
        } catch { return false; }
    }

    public static string ActiveRuntimeJson()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1"))
                return k == null ? null : k.GetValue("ActiveRuntime") as string;
        } catch { return null; }
    }

    // DLLs in bin that other mods or overlays use to load themselves into the game, and ReShade. OptiScaler's proxy,
    // installed by Add FSR, is not one of them.
    public static List<string> ProxyDlls()
    {
        var found = new List<string>();
        var fsr = UpscalerProxy();
        foreach (var n in new[] { "openvr_api.dll", "d3d11.dll", "dinput8.dll", "version.dll", "winmm.dll", "d3d9.dll" })
            if (File.Exists(Path.Combine(Dir, n)) && !n.Equals(fsr, StringComparison.OrdinalIgnoreCase)) found.Add(n);
        try {
            foreach (var f in Directory.GetFiles(Dir, "*.dll")) {
                var vi = FileVersionInfo.GetVersionInfo(f);
                var p = (vi.ProductName ?? "") + " " + (vi.FileDescription ?? "");
                if (p.IndexOf("ReShade", StringComparison.OrdinalIgnoreCase) >= 0) found.Add(Path.GetFileName(f) + " (ReShade)");
            }
        } catch { }
        if (File.Exists(Path.Combine(Dir, "ReShade.ini"))) found.Add("ReShade.ini (ReShade)");
        return found;
    }

    // The VC++ 2015-2022 x64 runtime the installer registers ("v14.40.33810.00").
    public static Version VcRuntime()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64")) {
                if (k == null) return null;
                var v = k.GetValue("Version") as string;
                if (string.IsNullOrEmpty(v)) return null;
                Version r;
                return Version.TryParse(v.TrimStart('v', 'V'), out r) ? r : null;
            }
        } catch { return null; }
    }

    public static bool ViGEmBusInstalled()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\ViGEmBus")) return k != null;
        } catch { return false; }
    }

    // ---- install checks (the launcher's "Install checks" panel, the report's system.txt) ----
    // Each one comes from a tester report where the install, not the mod, stopped VR:
    //   10 Nov  dxgi.dll gone from bin (antivirus quarantine): the game started flat, no mod log.
    //   9 Nov, 13 Nov  third-party OpenXR implicit layers (Cheeky foveated DLSS, VectorXR, and four more) made
    //           xrCreateInstance fault / xrCreateSession fail: VR did not start.
    //   13 Nov  ViGEmBus missing: the motion controllers cannot drive the game.
    //   9 Nov, 12 Nov  the Ubisoft Connect overlay: froze the game after a GPU error; crashed it at start with FSR on AMD.
    public enum CheckState { Pass, Warn, Fail }

    public class InstallCheck
    {
        public readonly string Name;
        public CheckState State;
        public string Summary;   // one short line beside the badge
        public string Detail;    // what is wrong and how to fix it (the line's dialog)
        public string Url;       // a download or help page the dialog offers to open, or null
        public bool Blocks;      // Play in VR is refused: the mod cannot load at all
        public string FixLabel;  // a button under the line that fixes it in place (Game options), or null
        public InstallCheck(string name) { Name = name; }
        public InstallCheck Set(CheckState s, string summary, string detail) { State = s; Summary = summary; Detail = detail; return this; }
    }

    // The mod itself: dxgi.dll next to FarCry5.exe. Antivirus programs sometimes quarantine it (an unsigned dll that hooks
    // the game); then the game starts flat and the mod writes no log.
    public static bool ModDllPresent() { return File.Exists(Path.Combine(Dir, "dxgi.dll")); }

    public const string ModDllMissing = "the mod's dxgi.dll is not in bin, so the game starts without the mod (flat, no VR). Copy dxgi.dll again from " +
                                        "the release zip into bin. If it disappears again, your antivirus removed it: restore it from the antivirus's " +
                                        "quarantine and add the bin folder as an exclusion.";

    // Whether a dxgi.dll is the VR mod's (it names its settings file; ReShade, Special K, DXVK or OptiScaler saved as dxgi.dll
    // do not). other: the dll's product name when it has one.
    public static bool IsModDll(string path, out string other)
    {
        other = null;
        try {
            var vi = FileVersionInfo.GetVersionInfo(path);
            var p = ((vi.ProductName ?? "") + " " + (vi.FileDescription ?? "")).Trim();
            if (p.Length > 0) other = p;
            var data = File.ReadAllBytes(path);
            var mark = Encoding.ASCII.GetBytes("fc5vr_settings.txt");
            for (int i = 0, n = data.Length - mark.Length; i <= n; ++i) {
                if (data[i] != mark[0]) continue;
                int j = 1;
                while (j < mark.Length && data[i + j] == mark[j]) ++j;
                if (j == mark.Length) return true;
            }
            return false;
        } catch { return true; }   // unreadable (locked by the running game): not reported as foreign
    }

    // An enabled OpenXR implicit API layer: the loader puts it between the mod and the runtime in every call.
    public class XrLayer
    {
        public string Name, Manifest, Hive;
        public bool Bad;      // stopped VR on testers' PCs
        public bool Tested;   // runs with the mod on the author's or testers' PCs
    }

    // Layers that ran with the mod in VR (the author's PC: OFXR Bridge and VD's; the Pimax tester's working runs: VD's and OBS Mirror).
    static readonly string[] TestedLayers = { "OFXRBridge", "VIRTUALDESKTOP_oculus_compatibility", "OBSMirror" };
    // Layers in the two "VR did not start" reports (9 Nov xrCreateInstance fault, 13 Nov xrCreateSession failure).
    static readonly string[] BadLayers = { "CHEEKY", "VECTORXR" };

    // HKLM and HKCU Khronos\OpenXR\1\ApiLayers\Implicit, value 0 = enabled. Entries whose manifest is missing are skipped (the
    // loader skips them too), and so are layers whose manifest's enable_environment is unset or disable_environment is set.
    public static List<XrLayer> EnabledLayers()
    {
        var list = new List<XrLayer>();
        foreach (var hive in new[] { Registry.LocalMachine, Registry.CurrentUser }) {
            try {
                using (var k = hive.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit")) {
                    if (k == null) continue;
                    foreach (var path in k.GetValueNames()) {
                        var v = k.GetValue(path);
                        if (!(v is int) || (int)v != 0 || !File.Exists(path)) continue;
                        string name = Path.GetFileNameWithoutExtension(path);
                        try {
                            var t = File.ReadAllText(path);
                            var m = Regex.Match(t, "\"name\"\\s*:\\s*\"([^\"]*)\"");
                            if (m.Success) name = m.Groups[1].Value;
                            var en = Regex.Match(t, "\"enable_environment\"\\s*:\\s*\"([^\"]*)\"");
                            if (en.Success && string.IsNullOrEmpty(Environment.GetEnvironmentVariable(en.Groups[1].Value))) continue;
                            var dis = Regex.Match(t, "\"disable_environment\"\\s*:\\s*\"([^\"]*)\"");
                            if (dis.Success && !string.IsNullOrEmpty(Environment.GetEnvironmentVariable(dis.Groups[1].Value))) continue;
                        } catch { }
                        var l = new XrLayer { Name = name, Manifest = path, Hive = hive == Registry.LocalMachine ? "HKLM" : "HKCU" };
                        foreach (var b in BadLayers) if (name.IndexOf(b, StringComparison.OrdinalIgnoreCase) >= 0) l.Bad = true;
                        foreach (var g in TestedLayers) if (name.IndexOf(g, StringComparison.OrdinalIgnoreCase) >= 0) l.Tested = true;
                        list.Add(l);
                    }
                }
            } catch { }
        }
        return list;
    }

    // Ubisoft Connect's "Enable in-game overlay for supported games" (Settings > General): settings.yaml "overlay:" block,
    // "enabled: true|false". Null when Ubisoft Connect's settings are not found or say nothing about it.
    public static bool? UbisoftOverlayOn()
    {
        try {
            var p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Ubisoft Game Launcher\settings.yaml");
            if (!File.Exists(p)) return null;
            bool inOverlay = false;
            foreach (var raw in File.ReadAllLines(p)) {
                if (raw.Trim().Length == 0) continue;
                bool top = !char.IsWhiteSpace(raw[0]);
                if (top) { inOverlay = raw.Trim() == "overlay:"; continue; }
                if (!inOverlay) continue;
                var m = Regex.Match(raw.Trim(), @"^enabled:\s*(\w+)");
                if (m.Success) return m.Groups[1].Value.Equals("true", StringComparison.OrdinalIgnoreCase);
            }
        } catch { }
        return null;
    }

    // OptiScaler.ini [Hotfix] DisableOverlays=true (Add FSR writes it; Play writes it again with an FSR choice).
    static bool OptiScalerBlocksOverlays()
    {
        try {
            foreach (var raw in File.ReadAllLines(Path.Combine(Dir, "OptiScaler.ini")))
                if (raw.Trim().Replace(" ", "") == "DisableOverlays=true") return true;
        } catch { }
        return false;
    }

    const string LayersGui = "https://github.com/fredemmott/OpenXR-API-Layers-GUI/releases";
    const string ViGEmBusUrl = "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe";
    const string VcRedistUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";

    public static List<InstallCheck> InstallChecks()
    {
        var all = new List<InstallCheck>();

        // 1. The mod's files next to FarCry5.exe.
        var files = new InstallCheck("Mod files in bin");
        all.Add(files);
        var dxgi = Path.Combine(Dir, "dxgi.dll");
        string other;
        var missing = new List<string>();
        foreach (var f in new[] { "dxgi.dll", "openxr_loader.dll", "nvngx_dlss.dll" }) if (!File.Exists(Path.Combine(Dir, f))) missing.Add(f);
        string nested = File.Exists(Path.Combine(Dir, @"bin\dxgi.dll")) ? " A copy is in " + Path.Combine(Dir, "bin") + ": the zip was extracted into bin instead of " +
                                                                        "the game folder. Move the files of that inner bin folder up one level, next to FarCry5.exe." : "";
        if (!File.Exists(dxgi)) {
            files.Set(CheckState.Fail, "dxgi.dll (the mod) is missing", "T" + ModDllMissing.Substring(1) + nested);
            files.Blocks = true;
        } else if (!IsModDll(dxgi, out other)) {
            files.Set(CheckState.Fail, "dxgi.dll is not the VR mod's",
                      "bin has a dxgi.dll that is not the VR mod's" + (other != null ? " (" + other + ")" : "") + ": another mod (ReShade, Special K, DXVK, OptiScaler...) " +
                      "saved under the same name replaced it. Move that dxgi.dll out of bin and copy the mod's dxgi.dll from the release zip again." + nested);
            files.Blocks = true;
        } else if (missing.Contains("openxr_loader.dll")) {
            files.Set(CheckState.Fail, "openxr_loader.dll is missing",
                      "openxr_loader.dll is not in bin: the mod needs it to talk to the headset, so the game starts without VR (or does not start). " +
                      "Copy it again from the release zip into bin, next to FarCry5.exe." + nested);
            files.Blocks = true;
        } else if (missing.Contains("nvngx_dlss.dll")) {
            files.Set(CheckState.Warn, "nvngx_dlss.dll is missing",
                      "nvngx_dlss.dll (the DLSS runtime) is not in bin: VR starts, but DLSS cannot run. Copy it again from the release zip into bin, next to FarCry5.exe." + nested);
        } else files.Set(CheckState.Pass, "dxgi.dll, openxr_loader.dll and nvngx_dlss.dll next to FarCry5.exe",
                         "The mod (dxgi.dll), the OpenXR loader and the DLSS runtime are in " + Dir + ", next to FarCry5.exe." + nested);

        // 2. The system's OpenXR runtime.
        var rt = new InstallCheck("OpenXR runtime");
        all.Add(rt);
        var env = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        var json = !string.IsNullOrEmpty(env) ? env : ActiveRuntimeJson();
        string rname, rlib;
        if (string.IsNullOrEmpty(json))
            rt.Set(CheckState.Fail, "none set as the system default",
                   "No OpenXR runtime is set as the system default, so the mod has no headset to start. Open your headset's PC app and make it the OpenXR runtime: " +
                   "Virtual Desktop Streamer: OpenXR Runtime VDXR; SteamVR: Settings > OpenXR > Set SteamVR as OpenXR runtime; Meta Quest Link: Settings > General > " +
                   "OpenXR Runtime; Pimax Play: Settings > OpenXR.");
        else if (!RuntimeManifest(json, out rname, out rlib))
            rt.Set(CheckState.Fail, "the default runtime's files are missing",
                   "The system's OpenXR runtime (" + json + (rlib != null ? ", library " + rlib : "") + ") is missing: its app was uninstalled or moved. " +
                   "Reinstall the headset's PC app, or set another one as the OpenXR runtime.");
        else rt.Set(CheckState.Pass, (ActiveRuntime() ?? rname ?? Path.GetFileNameWithoutExtension(json)) + (string.IsNullOrEmpty(env) ? "" : " (XR_RUNTIME_JSON)"),
                    "OpenXR runtime: " + (rname ?? "?") + "\n" + json + "\nlibrary " + rlib);

        // 3. Third-party OpenXR layers.
        var lc = new InstallCheck("OpenXR layers");
        all.Add(lc);
        var layers = EnabledLayers();
        var odd = layers.FindAll(l => !l.Tested);
        if (odd.Count == 0)
            lc.Set(CheckState.Pass, layers.Count == 0 ? "none enabled" : "only ones tested with the mod",
                   layers.Count == 0 ? "No OpenXR API layer is enabled." : "Enabled, and known to run with the mod: " + string.Join(", ", layers.ConvertAll(l => l.Name)) + ".");
        else {
            var bad = odd.FindAll(l => l.Bad);
            var untested = odd.FindAll(l => !l.Bad);
            var sb = new StringBuilder();
            if (bad.Count > 0) {
                sb.Append("These OpenXR API layers stopped VR from starting on testers' PCs (VR did not start, the game stayed flat):\n");
                foreach (var l in bad) sb.Append("- " + l.Name + "\n");
            }
            if (untested.Count > 0) {
                sb.Append(bad.Count > 0 ? "\nThese were never tested with the mod:\n"
                                        : "These OpenXR API layers sit between the mod and the headset and were never tested with it. Two testers' VR did not start until they turned theirs off:\n");
                foreach (var l in untested) sb.Append("- " + l.Name + "\n");
            }
            sb.Append("\nIf VR does not start, turn them off: untick them in the OpenXR API Layers GUI tool (Yes opens its download page), or in an " +
                      "administrator Command Prompt (Ctrl+C copies this text):\n");
            foreach (var l in odd)
                sb.Append("reg add \"" + l.Hive + @"\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"" /v """ + l.Manifest + "\" /t REG_DWORD /d 1 /f\n");
            sb.Append("(/d 0 turns one back on.)");
            string summary = bad.Count > 0 ? string.Join(", ", bad.ConvertAll(l => ShortLayer(l.Name))) + ": stopped VR before" +
                                             (untested.Count > 0 ? "; " + untested.Count + " more untested" : "")
                                           : string.Join(", ", untested.ConvertAll(l => ShortLayer(l.Name))) + ": untested with the mod";
            lc.Set(bad.Count > 0 ? CheckState.Fail : CheckState.Warn, summary, sb.ToString());
            lc.Url = LayersGui;
        }

        // 4. ViGEmBus.
        var vg = new InstallCheck("ViGEmBus driver");
        all.Add(vg);
        if (ViGEmBusInstalled()) vg.Set(CheckState.Pass, "installed", "ViGEmBus is installed: the motion controllers reach the game as a virtual Xbox pad.");
        else {
            vg.Set(CheckState.Fail, "not installed: the controllers will not work",
                   "The ViGEmBus driver is not installed. The motion controllers drive the game through it (a virtual Xbox pad), so without it VR starts but the " +
                   "controllers do nothing. Install ViGEmBus 1.22.0 (Yes downloads the installer), then restart Windows.");
            vg.Url = ViGEmBusUrl;
        }

        // 5. Visual C++ runtime.
        var vcc = new InstallCheck("Visual C++ runtime");
        all.Add(vcc);
        var vc = VcRuntime();
        if (vc == null) {
            vcc.Set(CheckState.Fail, "not installed", "The Microsoft Visual C++ 2015-2022 x64 runtime is not installed: the mod needs it, or the game may close at start. " +
                                                      "Install vc_redist.x64.exe from Microsoft (Yes downloads it).");
            vcc.Url = VcRedistUrl;
        } else if (vc < new Version(14, 40)) {
            vcc.Set(CheckState.Warn, "old (" + vc + ")", "The Microsoft Visual C++ x64 runtime is old (" + vc + "): install the latest vc_redist.x64.exe from Microsoft (Yes downloads it).");
            vcc.Url = VcRedistUrl;
        } else vcc.Set(CheckState.Pass, vc.ToString(), "Microsoft Visual C++ 2015-2022 x64 runtime " + vc + ".");

        // 6. Other mods' DLLs in bin.
        var om = new InstallCheck("Other mods in bin");
        all.Add(om);
        var proxies = ProxyDlls();
        if (!UpscalerInstalled() && (File.Exists(Path.Combine(Dir, "OptiScaler.dll")) || File.Exists(Path.Combine(Dir, "OptiScaler.ini"))))
            proxies.Add("OptiScaler (not installed by Add FSR)");
        if (File.Exists(Path.Combine(Dir, "dlssg_to_fsr3_amd_is_better.dll"))) proxies.Add("dlssg_to_fsr3_amd_is_better.dll");
        if (proxies.Count == 0) om.Set(CheckState.Pass, "none found", "No other mod, overlay or ReShade loads into the game from bin.");
        else om.Set(CheckState.Warn, string.Join(", ", proxies),
                    "bin has " + string.Join(", ", proxies) + ": other mods or overlays load into the game through these and can stop the VR mod " +
                    "(openvr_api.dll makes it try SteamVR's old API first). Move them out of bin if VR does not start. For FSR use the launcher's Add FSR, " +
                    "which installs OptiScaler the way the mod expects.");

        // 7. Game overlays (Ubisoft Connect's): the FSR crash and the silent freeze.
        var ov = new InstallCheck("Game overlays");
        all.Add(ov);
        bool fsr = ReadUpscaler() > 0;
        var ubi = UbisoftOverlayOn();
        const string ubiOff = "Turn it off: Ubisoft Connect > Settings > General > untick \"Enable in-game overlay for supported games\" (it cannot be seen in the headset anyway).";
        if (fsr && OptiScalerBlocksOverlays())
            ov.Set(CheckState.Pass, "blocked by OptiScaler (FSR)", "FSR is on and OptiScaler keeps the game overlays (Ubisoft Connect, Steam, Discord) out of the game.");
        else if (fsr)
            ov.Set(CheckState.Warn, "FSR on, overlays not blocked: Play blocks them",
                   "FSR is on and OptiScaler.ini lets the game overlays load. The Ubisoft Connect overlay crashed the game at start with FSR on AMD and Intel " +
                   "cards. Play sets OptiScaler's DisableOverlays=true before the game starts.");
        else if (ubi == true)
            ov.Set(CheckState.Warn, "Ubisoft Connect overlay on",
                   "Ubisoft Connect's in-game overlay is on. On a tester's PC it turned a graphics error into a frozen game with no crash report, and with FSR " +
                   "on AMD it crashed the game at start. " + ubiOff);
        else ov.Set(CheckState.Pass, ubi == false ? "Ubisoft Connect overlay off" : "no Ubisoft Connect overlay setting found",
                    ubi == false ? "Ubisoft Connect's in-game overlay is off." : "Ubisoft Connect's settings were not found (or do not say), so its overlay was not checked. " + ubiOff);

        // 8. The game's own options.
        var go = new InstallCheck("Game options");
        all.Add(go);
        var wrong = WrongGameOptions();
        if (wrong == null) go.Set(CheckState.Warn, "not checked: the game has not saved its options yet",
                                  "The game's options file (" + GameProfilePath + ") is not there yet: start the game once (Flatscreen is fine). Play in VR and the mod then set the options the mod needs.");
        else if (wrong.Count == 0) go.Set(CheckState.Pass, "set as the mod needs", "Every game option the mod needs is set (" + GameProfilePath + ").");
        else {
            go.Set(CheckState.Warn, WrongLabels(wrong).Count + " not set as the mod needs",
                   "Game options the VR mod needs, not set yet:\n- " + string.Join("\n- ", WrongLabels(wrong)) + "\n\nPlay in VR sets them in " + GameProfilePath +
                   " (so does \"Change to correct settings\" under this line), and the mod sets them again each time the game starts in VR. " +
                   "Your other options are kept, and your file is backed up first, beside it. To keep your own: the line \"gameoptions off\" in fc5vr_settings.txt.");
            go.FixLabel = "Change to correct settings";
        }
        return all;
    }

    // XR_APILAYER_CHEEKY_foveated_dlss -> CHEEKY_foveated_dlss
    public static string ShortLayer(string name) { return name.StartsWith("XR_APILAYER_") ? name.Substring(12) : name; }

    public static bool GameRunning() { return Process.GetProcessesByName("FarCry5").Length > 0; }

    public static bool FromSteam { get { return GameCommand != null; } }

    // What to put in Steam > Far Cry 5 > Properties > Launch options.
    public static string SteamLaunchOption { get { return "\"" + Path.Combine(Dir, "FC5VR_Launcher.exe") + "\" %command%"; } }

    static string Quote(string a) { return a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0 ? a : "\"" + a.Replace("\"", "\\\"") + "\""; }

    public static void StartGame()
    {
        if (GameCommand != null) {
            // Started by Steam's Play: run the command Steam gave (its environment, SteamAppId included, is inherited).
            var rest = new List<string>();
            for (int i = 1; i < GameCommand.Length; ++i) rest.Add(Quote(GameCommand[i]));
            Process.Start(new ProcessStartInfo(GameCommand[0], string.Join(" ", rest)) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(GameCommand[0]) });
            return;
        }
        // The Steam copy starts through Steam (as from the library); another copy starts its own exe.
        if (Dir.ToLowerInvariant().Contains(@"\steamapps\")) Process.Start(SteamUrl);
        else Process.Start(new ProcessStartInfo(Path.Combine(Dir, "FarCry5.exe")) { WorkingDirectory = Dir });
    }

    public static Headset[] HeadsetList { get { return Headsets; } }
    public static string GameDir { get { return Dir; } }
}

class LauncherForm : Form
{
    readonly RadioButton vr = new RadioButton();
    readonly RadioButton flat = new RadioButton();
    readonly ComboBox scale = new ComboBox();
    readonly ComboBox headset = new ComboBox();
    readonly Label scaleInfo = new Label();
    readonly Label headsetInfo = new Label();
    readonly Label modeInfo = new Label();
    readonly ComboBox render = new ComboBox();
    readonly Label renderInfo = new Label();
    readonly CheckBox dlss = new CheckBox();
    readonly ComboBox dlssMode = new ComboBox();
    readonly ComboBox upscaler = new ComboBox();
    readonly Button fsrButton = new Button();
    readonly LinkLabel upscalerInfo = new LinkLabel();
    readonly Label dlssInfo = new Label();
    readonly ComboBox cutscenes = new ComboBox();
    readonly Label cutscenesInfo = new Label();
    readonly CheckBox skipPopup = new CheckBox();
    readonly Label popupInfo = new Label();
    readonly CheckBox sessionLogs = new CheckBox();
    readonly Label sessionLogsInfo = new Label();
    bool dlssKeepFile;   // fc5vr_resolution.txt is not "auto" (a test setup): kept unless the mode is changed
    readonly Button play = new Button();
    // Install checks: one line per Launcher.InstallChecks() entry, a PASS / WARN / FAIL badge each; a line's click shows how to fix it.
    readonly TableLayoutPanel checkList = new TableLayoutPanel();
    readonly ToolTip checkTips = new ToolTip { AutoPopDelay = 20000, InitialDelay = 300 };
    DateTime checkedAt = DateTime.MinValue;
    // Resizable window (author 20 Nov): the settings and checks scroll inside the window and the buttons stay at its bottom.
    // Wide enough, the install checks are a third column and both columns share the extra width; narrower, the checks move
    // under the settings. Relayout() sets the column widths and the labels' wrap widths from the window's width.
    readonly TableLayoutPanel table = new TableLayoutPanel();
    readonly Panel scroller = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
    readonly FlowLayoutPanel buttons = new FlowLayoutPanel();
    readonly GroupBox checksBox = new GroupBox();
    readonly Label checkHint = new Label();
    readonly List<Label> rowLabels = new List<Label>();
    readonly List<Label> infoLabels = new List<Label>();
    readonly List<Label> checkLabels = new List<Label>();
    int settingsRows;          // table rows of the settings; the narrow layout puts the checks in the row after them
    int checkTextWidth = 250;  // wrap width of a check's name and result (pixels at the current DPI once loaded)
    bool loaded, laying;

    public LauncherForm()
    {
        Text = "Far Cry 5 VR";
        FormBorderStyle = FormBorderStyle.Sizable;
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        AutoScaleDimensions = new SizeF(96f, 96f);
        Font = new Font("Segoe UI", 9.5f);
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(Launcher.GameDir, "FarCry5.exe")); } catch { }

        table.ColumnCount = 3;
        table.AutoSize = true;
        table.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        table.Padding = new Padding(14);
        table.Location = Point.Empty;
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 300));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 350));

        // Mode
        var modes = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        vr.Text = "VR"; vr.AutoSize = true;
        flat.Text = "Flatscreen"; flat.AutoSize = true; flat.Margin = new Padding(16, 3, 3, 3);
        modes.Controls.Add(vr);
        modes.Controls.Add(flat);
        AddRow(table, "Mode", modes);
        AddInfo(table, modeInfo);

        // Rendering: F11 Display "Rendering" (F8 in game).
        render.DropDownStyle = ComboBoxStyle.DropDownList;
        render.Width = 120;
        render.Items.AddRange(new object[] { "Stereo", "AFW" });
        AddRow(table, "Rendering", render);
        AddInfo(table, renderInfo);

        // Resolution multiplier: x1.0 to x2.0 in steps of 0.1, as F11 Display > Image quality.
        scale.DropDownStyle = ComboBoxStyle.DropDownList;
        scale.Width = 120;
        for (int s = 10; s <= 20; ++s) scale.Items.Add("x" + (s / 10.0).ToString("0.0", CultureInfo.InvariantCulture));
        AddRow(table, "Resolution multiplier", scale);
        AddInfo(table, scaleInfo);

        // DLSS: on/off (F11 Display > Image quality "DLSS") and the render scale ("DLSS render scale").
        var dl = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        dlss.Text = "On"; dlss.AutoSize = true; dlss.Margin = new Padding(0, 5, 12, 0);
        dlssMode.DropDownStyle = ComboBoxStyle.DropDownList;
        dlssMode.Width = 160;
        dlssMode.Items.AddRange(Launcher.DlssModes);
        dl.Controls.Add(dlss);
        dl.Controls.Add(dlssMode);
        AddRow(table, "DLSS", dl);
        AddInfo(table, dlssInfo);

        // Upscaler: F11 Display > Image quality "Upscaler". DLSS only until Add FSR has installed OptiScaler.
        var up = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        upscaler.DropDownStyle = ComboBoxStyle.DropDownList;
        upscaler.Width = 190;
        upscaler.DropDownWidth = 330;
        upscaler.Margin = new Padding(0, 1, 8, 0);
        fsrButton.AutoSize = true;
        fsrButton.Margin = new Padding(0);
        up.Controls.Add(upscaler);
        up.Controls.Add(fsrButton);
        AddRow(table, "Upscaler", up);
        AddInfo(table, upscalerInfo);
        FillUpscaler(-1);
        upscaler.SelectedIndexChanged += delegate { if (upscaler.SelectedIndex > 0) dlss.Checked = true; };
        fsrButton.Click += delegate { FsrButton(); };
        upscalerInfo.LinkClicked += delegate { OpenOptiScalerPage(); };

        // Cutscenes: F11 Comfort > Cinematics and vehicles "Cinematics (cutscenes)". "Unchanged" only when F11 has set it.
        cutscenes.DropDownStyle = ComboBoxStyle.DropDownList;
        cutscenes.Width = 260;
        for (int i = 0; i < 2; ++i) cutscenes.Items.Add(Launcher.CutsceneModes[i]);
        int cutsceneMode = Launcher.ReadCutscenes();
        if (cutsceneMode == 2) cutscenes.Items.Add(Launcher.CutsceneModes[2]);
        cutscenes.SelectedIndex = cutsceneMode;
        AddRow(table, "Cutscenes", cutscenes);
        AddInfo(table, cutscenesInfo);

        // Loading popup: F11 HUD and menus > Loading popup ("loadpopup off" skips it).
        skipPopup.Text = "Skip the loading popup"; skipPopup.AutoSize = true;
        AddRow(table, "After a load", skipPopup);
        AddInfo(table, popupInfo);

        // Logs during sessions: F11 Main "Logs during sessions (slower)" ("sessionlogs off" keeps only warnings and errors once in the game).
        sessionLogs.Text = "Logs during sessions (slower)"; sessionLogs.AutoSize = true;
        AddRow(table, "Logs", sessionLogs);
        AddInfo(table, sessionLogsInfo);

        // Headset
        headset.DropDownStyle = ComboBoxStyle.DropDownList;
        headset.Width = 200;
        foreach (var h in Launcher.HeadsetList) headset.Items.Add(h.Name);
        AddRow(table, "Headset", headset);
        AddInfo(table, headsetInfo);

        // Install checks: a third column beside every row above (under them when the window is narrow, Relayout).
        settingsRows = table.RowCount;
        checksBox.Text = "Install checks";
        checksBox.AutoSize = true;
        checksBox.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        checksBox.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        checksBox.Margin = new Padding(18, 3, 0, 0);
        checksBox.Padding = new Padding(8, 6, 8, 8);
        var checksPanel = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0) };
        checkList.ColumnCount = 2;
        checkList.AutoSize = true;
        checkList.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        checkList.Margin = new Padding(0);
        checkList.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 52));
        checkList.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 250));
        checkHint.Text = "Click a line for what it means and how to fix it.";
        checkHint.AutoSize = true;
        checkHint.MaximumSize = new Size(300, 0);
        checkHint.ForeColor = SystemColors.GrayText;
        checkHint.Margin = new Padding(0, 6, 0, 0);
        var recheck = new Button { Text = "Check again", AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
        recheck.Click += delegate { FillChecks(); };
        checksPanel.Controls.Add(checkList);
        checksPanel.Controls.Add(checkHint);
        checksPanel.Controls.Add(recheck);
        checksBox.Controls.Add(checksPanel);
        table.Controls.Add(checksBox, 2, 0);
        table.SetRowSpan(checksBox, settingsRows);
        table.RowCount = settingsRows + 1;   // the narrow layout's row for the checks (empty, no height, when wide)

        // Buttons: docked at the window's bottom, outside the scrolling part, so Play is always in view.
        buttons.AutoSize = true;
        buttons.FlowDirection = FlowDirection.RightToLeft;
        buttons.Dock = DockStyle.Bottom;
        buttons.Padding = new Padding(14, 8, 14, 12);
        buttons.Margin = new Padding(0);
        var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel };
        play.Text = "Play"; play.AutoSize = true; play.MinimumSize = new Size(90, 0);
        var report = new Button { Text = "Make a report", AutoSize = true };
        report.Click += delegate { MakeReport(null); };
        buttons.Controls.Add(close);
        buttons.Controls.Add(play);
        buttons.Controls.Add(report);

        scroller.Controls.Add(table);
        Controls.Add(buttons);
        Controls.Add(scroller);
        scroller.BringToFront();   // docked last: fills what the buttons leave
        scroller.Resize += delegate { Relayout(); };
        AcceptButton = play;
        CancelButton = close;

        bool isFlat; string hs; float sc;
        Launcher.ReadChoice(out isFlat, out hs, out sc);
        vr.Checked = !isFlat;
        flat.Checked = isFlat;
        scale.SelectedIndex = Math.Max(0, Math.Min(10, (int)Math.Round(sc * 10) - 10));
        headset.SelectedIndex = 0;
        for (int i = 0; i < Launcher.HeadsetList.Length; ++i) if (Launcher.HeadsetList[i].Id == hs) headset.SelectedIndex = i;
        render.SelectedIndex = Launcher.ReadStereo() ? 0 : 1;
        bool dlssOn; int dm;
        Launcher.ReadDlss(out dlssOn, out dm);
        dlss.Checked = dlssOn;
        dlssKeepFile = dm < 0;
        dlssMode.SelectedIndex = Math.Max(0, dm);
        skipPopup.Checked = Launcher.ReadSkipPopup();
        sessionLogs.Checked = Launcher.ReadSessionLogs();

        vr.CheckedChanged += delegate { UpdateInfo(); };
        scale.SelectedIndexChanged += delegate { UpdateInfo(); };
        render.SelectedIndexChanged += delegate { UpdateInfo(); };
        dlss.CheckedChanged += delegate { UpdateInfo(); };
        cutscenes.SelectedIndexChanged += delegate { UpdateInfo(); };
        skipPopup.CheckedChanged += delegate { UpdateInfo(); };
        sessionLogs.CheckedChanged += delegate { UpdateInfo(); };
        dlssMode.SelectedIndexChanged += delegate { dlssKeepFile = false; UpdateInfo(); };
        headset.SelectedIndexChanged += delegate { UpdateInfo(); };
        play.Click += delegate { Play(); };
        // Back in the launcher (after installing a driver, moving files): checked again, at most every 3 s.
        Activated += delegate { if ((DateTime.Now - checkedAt).TotalSeconds > 3) FillChecks(); };
        FillChecks();
        UpdateInfo();
    }

    static readonly Color PassColor = Color.FromArgb(46, 125, 50), WarnColor = Color.FromArgb(249, 168, 37), FailColor = Color.FromArgb(198, 40, 40);

    // The install checks panel, run again (or shown from checks just run): badge, name and one-line result for each check.
    void FillChecks(List<Launcher.InstallCheck> checks = null)
    {
        checkedAt = DateTime.Now;
        try { if (checks == null) checks = Launcher.InstallChecks(); }
        catch (Exception e) { checks = new List<Launcher.InstallCheck> { new Launcher.InstallCheck("Install checks").Set(Launcher.CheckState.Warn, "could not run", e.Message) }; }
        checkList.SuspendLayout();
        checkTips.RemoveAll();
        checkLabels.Clear();
        while (checkList.Controls.Count > 0) checkList.Controls[0].Dispose();
        checkList.RowStyles.Clear();
        checkList.RowCount = 0;
        foreach (var check in checks) {
            var c = check;
            var badge = new Label {
                Text = c.State == Launcher.CheckState.Pass ? "PASS" : c.State == Launcher.CheckState.Warn ? "WARN" : "FAIL",
                AutoSize = false, Size = loaded ? new Size(Px(44), Px(18)) : new Size(44, 18), TextAlign = ContentAlignment.MiddleCenter, Margin = new Padding(0, 3, 8, 0),
                Font = new Font(Font.FontFamily, 7.5f, FontStyle.Bold),
                BackColor = c.State == Launcher.CheckState.Pass ? PassColor : c.State == Launcher.CheckState.Warn ? WarnColor : FailColor,
                ForeColor = c.State == Launcher.CheckState.Warn ? Color.Black : Color.White,
            };
            var text = new Label { Text = c.Name, AutoSize = true, MaximumSize = new Size(checkTextWidth, 0), Margin = new Padding(0, 2, 0, 0), Font = new Font(Font, FontStyle.Bold) };
            var summary = new Label { Text = c.Summary, AutoSize = true, MaximumSize = new Size(checkTextWidth, 0), Margin = new Padding(0, 0, 0, 5),
                                      ForeColor = c.State == Launcher.CheckState.Fail ? FailColor : SystemColors.GrayText };
            var lines = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Margin = new Padding(0) };
            lines.Controls.Add(text);
            lines.Controls.Add(summary);
            checkLabels.Add(text);
            checkLabels.Add(summary);
            foreach (Control ctl in new Control[] { badge, text, summary, lines }) {
                ctl.Cursor = Cursors.Hand;
                ctl.Click += delegate { ShowCheck(c); };
                checkTips.SetToolTip(ctl, c.Detail);
            }
            if (c.FixLabel != null) {
                var fix = new Button { Text = c.FixLabel, AutoSize = true, Margin = new Padding(0, 0, 0, 6) };
                fix.Click += delegate { FixGameOptionsNow(); };
                lines.Controls.Add(fix);
            }
            checkList.Controls.Add(badge, 0, checkList.RowCount);
            checkList.Controls.Add(lines, 1, checkList.RowCount);
            checkList.RowCount++;
        }
        checkList.ResumeLayout();
    }

    // Game options "Change to correct settings": the game's file set to the values the mod needs, backed up first.
    void FixGameOptionsNow()
    {
        const string title = "Far Cry 5 VR: game options";
        if (Launcher.GameRunning()) {
            MessageBox.Show(this, "Far Cry 5 is running: it would write its own options back when it closes. Close it first, then press the button again.",
                            title, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        var before = Launcher.WrongLabels(Launcher.WrongGameOptions());
        try {
            Launcher.FixGameOptions();
        } catch (Exception e) {
            MessageBox.Show(this, "Could not change the game's options file:\n" + e.Message + "\nSet them in the game's Options instead.", title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var left = Launcher.WrongLabels(Launcher.WrongGameOptions());
        var changed = before.FindAll(l => !left.Contains(l));
        var msg = new StringBuilder();
        if (changed.Count > 0) msg.Append("Set:\n- " + string.Join("\n- ", changed) + "\n\n");
        if (left.Count > 0) msg.Append("Not in the file yet (the game has not written that part), so set them in the game's Options:\n- " + string.Join("\n- ", left) + "\n\n");
        if (changed.Count == 0 && left.Count == 0) msg.Append("Every game option the mod needs was already set.\n\n");
        msg.Append("Your file as it was is kept beside it: " + Path.GetFileName(Launcher.GameProfilePath) + ".fc5vr_last (just before this change) and " +
                   Path.GetFileName(Launcher.GameProfilePath) + ".fc5vr_backup (the first time the launcher changed it). To undo, copy one over gamerprofile.xml.");
        FillChecks();
        UpdateInfo();
        MessageBox.Show(this, msg.ToString(), title, MessageBoxButtons.OK, left.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
    }

    void ShowCheck(Launcher.InstallCheck c)
    {
        var icon = c.State == Launcher.CheckState.Pass ? MessageBoxIcon.Information : c.State == Launcher.CheckState.Warn ? MessageBoxIcon.Warning : MessageBoxIcon.Error;
        var title = "Far Cry 5 VR: " + c.Name + " " + (c.State == Launcher.CheckState.Pass ? "(pass)" : c.State == Launcher.CheckState.Warn ? "(warning)" : "(failed)");
        if (c.Url == null || c.State == Launcher.CheckState.Pass) {
            MessageBox.Show(this, c.Detail, title, MessageBoxButtons.OK, icon);
            return;
        }
        if (MessageBox.Show(this, c.Detail + "\n\nOpen " + c.Url + "?", title, MessageBoxButtons.YesNo, icon) != DialogResult.Yes) return;
        try { Process.Start(c.Url); }
        catch (Exception e) { MessageBox.Show(this, "Could not open " + c.Url + ":\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
    }

    // Play in VR: a check that stops the mod loading refuses to start; other failed checks ask first. False: back to the launcher.
    bool CheckInstall()
    {
        var checks = Launcher.InstallChecks();
        FillChecks(checks);
        var failed = checks.FindAll(c => c.State == Launcher.CheckState.Fail);
        var block = failed.Find(c => c.Blocks);
        if (block != null) {
            MessageBox.Show(this, "VR cannot start: " + block.Detail, "Far Cry 5 VR: " + block.Name, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return false;
        }
        if (failed.Count == 0) return true;
        var list = new StringBuilder();
        foreach (var c in failed) list.AppendLine("- " + c.Name + ": " + c.Summary);
        return MessageBox.Show(this, "These install checks failed:\n\n" + list + "\nVR may not start, or the controllers may not work. Click the line in Install checks " +
                                     "for how to fix it.\n\nStart the game anyway?", "Far Cry 5 VR: install checks", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes;
    }

    // The upscaler list and the FSR button for what is installed; select: the entry to pick, -1 the one the files say.
    void FillUpscaler(int select)
    {
        bool installed = Launcher.UpscalerInstalled();
        upscaler.Items.Clear();
        if (installed) upscaler.Items.AddRange(Launcher.UpscalerNames);
        else upscaler.Items.Add(Launcher.UpscalerNames[0]);
        upscaler.SelectedIndex = installed ? (select >= 0 ? select : Launcher.ReadUpscaler()) : 0;
        upscaler.Enabled = installed;
        fsrButton.Text = installed ? "Remove FSR" : "Add FSR...";
        upscalerInfo.Links.Clear();
        if (installed) {
            upscalerInfo.Text = "FSR uses the DLSS settings above, so choosing it ticks DLSS. The render scale drives FSR, the DLSS model does nothing. " +
                                "FSR 3.1 suits every card; FFX gives FSR 4 on Radeon RX 9000. Remove FSR deletes OptiScaler's files again.";
        } else {
            const string link = "Download OptiScaler " + OptiScalerSetup.TestedVersion;
            upscalerInfo.Text = "No NVIDIA RTX card? FSR runs in place of DLSS on any card (experimental). " + link +
                                " (free, the .7z file), then press Add FSR: the launcher finds it in Downloads and installs it.";
            upscalerInfo.Links.Add(upscalerInfo.Text.IndexOf(link, StringComparison.Ordinal), link.Length);
        }
    }

    void OpenOptiScalerPage()
    {
        try { Process.Start(OptiScalerSetup.ReleasePage); }
        catch (Exception e) { MessageBox.Show(this, "Could not open " + OptiScalerSetup.ReleasePage + ":\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
    }

    // Add FSR: OptiScaler installed from its download (found in Downloads, or chosen); Remove FSR: its files deleted.
    void FsrButton()
    {
        const string title = "Far Cry 5 VR: FSR";
        if (Launcher.GameRunning()) {
            MessageBox.Show(this, "Far Cry 5 is running. Close it first.", title, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        var bin = Launcher.GameDir;
        if (Launcher.UpscalerInstalled()) {
            if (MessageBox.Show(this, "Remove FSR? This deletes the OptiScaler files the launcher put in the game's bin folder; the mod uses DLSS again.",
                                title, MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            try { OptiScalerSetup.Uninstall(bin); }
            catch (Exception e) { MessageBox.Show(this, "Could not remove every file:\n" + e.Message, title, MessageBoxButtons.OK, MessageBoxIcon.Error); }
            FillUpscaler(-1);
            UpdateInfo();
            FillChecks();
            return;
        }
        string picked = OptiScalerSetup.FindDownloaded();
        if (picked != null) {
            var a = MessageBox.Show(this, "Install FSR from\n" + picked + "?\n\nNo lets you choose another file.", title, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (a == DialogResult.Cancel) return;
            if (a == DialogResult.No) picked = null;
        } else {
            var a = MessageBox.Show(this, "FSR needs OptiScaler " + OptiScalerSetup.TestedVersion + ", a free tool (52 MB). Open its download page now?\n\n" +
                                    "On the page, under Assets, download the .7z file, then press Add FSR again.\n" +
                                    "No: you already have it, choose the file.", title, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (a == DialogResult.Cancel) return;
            if (a == DialogResult.Yes) { OpenOptiScalerPage(); return; }
        }
        if (picked == null) {
            using (var d = new OpenFileDialog()) {
                d.Title = "Choose OptiScaler's download (.7z), or OptiScaler.dll in a folder you extracted it to";
                d.Filter = "OptiScaler (*.7z, *.zip, OptiScaler.dll)|*.7z;*.zip;OptiScaler.dll|All files (*.*)|*.*";
                var dl = OptiScalerSetup.DownloadsFolder();
                if (dl != null) d.InitialDirectory = dl;
                if (d.ShowDialog(this) != DialogResult.OK) return;
                picked = d.FileName;
            }
        }
        string temp = null;
        try {
            Cursor = Cursors.WaitCursor;
            fsrButton.Enabled = false;
            fsrButton.Text = "Installing...";
            fsrButton.Update();
            bool official;
            var src = OptiScalerSetup.Prepare(picked, out temp, out official);
            var ver = OptiScalerSetup.DllVersion(src);
            Cursor = Cursors.Default;
            if (!official && !ver.StartsWith(OptiScalerSetup.TestedVersion) &&
                MessageBox.Show(this, "This is OptiScaler " + (ver.Length > 0 ? ver : "(unknown version)") + "; the mod was tested with " + OptiScalerSetup.TestedVersion +
                                ". Install it anyway?", title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            Cursor = Cursors.WaitCursor;
            OptiScalerSetup.Install(bin, src, "fsr31", picked);
            Cursor = Cursors.Default;
            FillUpscaler(1);
            dlss.Checked = true;
            UpdateInfo();
            MessageBox.Show(this, "FSR is installed. The Upscaler is set to FSR 3.1 (FFX gives FSR 4 on Radeon RX 9000); it can also be changed in game in F11 > Display > Image quality, " +
                            "from the next game start.\n\nFSR uses the DLSS render scale: Native is the sharpest, Quality is faster.", title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        } catch (Exception e) {
            Cursor = Cursors.Default;
            MessageBox.Show(this, "FSR was not installed:\n" + e.Message, title, MessageBoxButtons.OK, MessageBoxIcon.Error);
        } finally {
            Cursor = Cursors.Default;
            fsrButton.Enabled = true;
            if (!Launcher.UpscalerInstalled()) fsrButton.Text = "Add FSR...";
            if (temp != null) try { Directory.Delete(temp, true); } catch (Exception) { }
            FillChecks();
        }
    }

    void AddRow(TableLayoutPanel t, string label, Control c)
    {
        var l = new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 16, 0) };
        rowLabels.Add(l);
        c.Margin = new Padding(0, 3, 0, 0);
        t.Controls.Add(l, 0, t.RowCount);
        t.Controls.Add(c, 1, t.RowCount);
        t.RowCount++;
    }

    void AddInfo(TableLayoutPanel t, Label info)
    {
        infoLabels.Add(info);
        info.AutoSize = true;
        info.MaximumSize = new Size(300, 0);
        info.ForeColor = SystemColors.GrayText;
        info.Margin = new Padding(0, 2, 0, 8);
        t.Controls.Add(info, 1, t.RowCount);
        t.RowCount++;
    }

    // Pixels at the window's DPI for a size given at 96 DPI (the controls made in the constructor are scaled by the form itself).
    int Px(int v)
    {
        float k = CurrentAutoScaleDimensions.Width / 96f;
        return (int)Math.Round(v * (k > 0 ? k : 1f));
    }

    int LabelColumnWidth()
    {
        int w = 0;
        foreach (var l in rowLabels) w = Math.Max(w, l.PreferredSize.Width + l.Margin.Horizontal);
        return w;
    }

    // First size: both columns side by side, at most the screen's working area (smaller, the checks go under the settings and
    // the window scrolls). The smallest window keeps the settings column at its old width with Play in view.
    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);   // DPI scaling of the controls happens here
        loaded = true;
        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        int frameW = Width - ClientSize.Width, frameH = Height - ClientSize.Height;
        int side = LabelColumnWidth() + table.Padding.Horizontal + SystemInformation.VerticalScrollBarWidth;
        int w = Math.Min(side + Px(300) + Px(350), area.Width - frameW);
        ClientSize = new Size(w, ClientSize.Height);
        Relayout();
        int h = table.GetPreferredSize(Size.Empty).Height + buttons.GetPreferredSize(new Size(w, 0)).Height;
        ClientSize = new Size(w, Math.Min(h, area.Height - frameH));
        MinimumSize = new Size(side + Px(300) + frameW, Math.Min(buttons.Height + Px(200) + frameH, Height));
        CenterToScreen();
    }

    // The columns and wrap widths for the window's current width (scroller.Resize; once loaded).
    void Relayout()
    {
        if (!loaded || laying) return;
        laying = true;
        try {
            int col0 = LabelColumnWidth();
            int avail = scroller.Width - SystemInformation.VerticalScrollBarWidth - table.Padding.Horizontal - col0;
            int setMin = Px(300), chkMin = Px(350);
            bool wide = avail >= setMin + chkMin;
            int setW = wide ? setMin + (avail - setMin - chkMin) * 300 / 650 : Math.Max(setMin, avail);
            int chkW = wide ? avail - setW : 0;
            table.SuspendLayout();
            table.ColumnStyles[1].Width = setW;
            table.ColumnStyles[2].Width = chkW;
            table.SetRowSpan(checksBox, 1);
            table.SetColumnSpan(checksBox, 1);
            if (wide) {
                table.SetCellPosition(checksBox, new TableLayoutPanelCellPosition(2, 0));
                table.SetRowSpan(checksBox, settingsRows);
                checksBox.Margin = new Padding(Px(18), Px(3), 0, 0);
            } else {
                table.SetCellPosition(checksBox, new TableLayoutPanelCellPosition(0, settingsRows));
                table.SetColumnSpan(checksBox, 2);
                checksBox.Margin = new Padding(0, Px(10), 0, 0);
            }
            // Inside the box: its padding and border, the badge column, a little room.
            int boxW = wide ? chkW - Px(18) : col0 + setW;
            checkTextWidth = Math.Max(Px(150), boxW - Px(16) - Px(52) - Px(14));
            checkList.ColumnStyles[0].Width = Px(52);
            checkList.ColumnStyles[1].Width = checkTextWidth;
            foreach (var l in checkLabels) l.MaximumSize = new Size(checkTextWidth, 0);
            checkHint.MaximumSize = new Size(checkTextWidth + Px(52), 0);
            foreach (var l in infoLabels) l.MaximumSize = new Size(setW, 0);
            table.ResumeLayout(true);
        } finally { laying = false; }
    }

    float Multiplier() { return (10 + Math.Max(0, scale.SelectedIndex)) / 10.0f; }

    Launcher.Headset Chosen() { return Launcher.HeadsetList[Math.Max(0, headset.SelectedIndex)]; }
    string HeadsetId() { return Chosen().Id; }

    // The headset row's text: how it connects, whether the system's runtime is one it uses, what the last run measured.
    string HeadsetText()
    {
        var hs = Chosen();
        var rt = Launcher.ActiveRuntime();
        var sb = new StringBuilder("Connects through " + hs.Connect.TrimEnd('.') + ". ");
        if (rt == null) sb.Append("No OpenXR runtime is set as the system default: set the headset's app (Virtual Desktop Streamer: OpenXR Runtime VDXR; SteamVR: Settings > OpenXR) as the default. ");
        else if (hs.Runtimes.Length > 0 && Array.IndexOf(hs.Runtimes, rt) < 0 && rt != "Meta XR Simulator")
            sb.Append("The system's OpenXR runtime is " + rt + ", which the " + hs.Name + " does not usually use: set " + hs.Runtimes[0] + " as the OpenXR runtime. ");
        else sb.Append("OpenXR runtime: " + rt + ". ");
        var view = Launcher.LastView(hs.Id);
        string v;
        if (view != null && view.TryGetValue("span", out v)) {
            var p = v.Split(' ');
            float sh, sv;
            if (p.Length >= 2 && float.TryParse(p[0], NumberStyles.Float, CultureInfo.InvariantCulture, out sh) && float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out sv)) {
                string sys;
                view.TryGetValue("system", out sys);
                sb.Append(string.Format(CultureInfo.InvariantCulture, "Last run: {0}, view {1:0} x {2:0} deg. ", string.IsNullOrEmpty(sys) ? hs.Name : sys, sh, sv));
                if (sh > 116) sb.Append("That is wider than the game's own view (120 deg at most): objects at the far sides can pop in. ");
            }
        }
        // Missing files, drivers, layers, overlays and game options: the Install checks panel.
        sb.Append("Connect the headset before pressing Play.");
        return sb.ToString();
    }

    void UpdateInfo()
    {
        bool isVr = vr.Checked;
        scale.Enabled = isVr;
        headset.Enabled = isVr;
        modeInfo.Text = isVr ? "Stereo 3D in the headset with motion controls. F11 in game opens the mod's menu."
                             : "The game runs on the monitor without the mod (as if it was not installed).";
        int w, h;
        bool measured;
        bool known = Launcher.EyeSizeFor(HeadsetId(), out w, out h, out measured);
        if (!isVr) scaleInfo.Text = "Not used in Flatscreen.";
        else if (known && measured)
            scaleInfo.Text = string.Format(CultureInfo.InvariantCulture, "Eye images {0}x{1} (headset {2}x{3}). Higher is sharper and slower. F11 can change it in game.",
                                           (int)(w * Multiplier()) & ~7, (int)(h * Multiplier()) & ~7, w, h);
        else if (known)
            scaleInfo.Text = string.Format(CultureInfo.InvariantCulture, "Eye images about {0}x{1} (typical for this headset {2}x{3}; the mod asks the headset for its own size at start). Higher is sharper and slower. F11 can change it in game.",
                                           (int)(w * Multiplier()) & ~7, (int)(h * Multiplier()) & ~7, w, h);
        else scaleInfo.Text = "Eye image size on top of the headset's recommended size (the mod asks the headset at start). Higher is sharper and slower. F11 can change it in game.";
        render.Enabled = isVr;
        renderInfo.Text = !isVr ? "Not used in Flatscreen."
                        : render.SelectedIndex == 0 ? "Both eyes drawn every game frame: sharpest, heaviest. F8 switches in game."
                                                    : "One eye per game frame, the other rebuilt from depth: faster. F8 switches in game.";
        dlss.Enabled = isVr;
        upscaler.Enabled = isVr && Launcher.UpscalerInstalled();
        dlssMode.Enabled = isVr && dlss.Checked;
        int ew = (int)(w * Multiplier()) & ~7, eh = (int)(h * Multiplier()) & ~7;
        float ratio = Launcher.DlssRatios[Math.Max(0, dlssMode.SelectedIndex)];
        if (!isVr) dlssInfo.Text = "Not used in Flatscreen (the game's own options apply).";
        else if (!dlss.Checked) dlssInfo.Text = "Off: the game's own anti-aliasing, rendered at the eye image size.";
        else if (dlssKeepFile) dlssInfo.Text = "The render size is a test setting (fc5vr_resolution.txt): kept unless you pick a mode.";
        else if (ratio > 0.99f) dlssInfo.Text = "DLAA: the game renders at the eye image size, DLSS smooths it (sharpest, slowest).";
        else if (known) dlssInfo.Text = string.Format(CultureInfo.InvariantCulture, "The game renders at {0}x{1} and DLSS scales it up to the eye images. Lower is faster and softer.",
                                                     (int)(ew * ratio) & ~7, (int)(eh * ratio) & ~7);
        else dlssInfo.Text = "The game renders smaller and DLSS scales it up to the eye images. Lower is faster and softer.";
        dlssInfo.Text += " Turning DLSS on or off in F11 needs a game restart.";
        cutscenes.Enabled = isVr;
        cutscenesInfo.Text = !isVr ? "Not used in Flatscreen."
                           : cutscenes.SelectedIndex == 0 ? "On a big screen hung where you look, a few seconds after the scene starts. F11 can change it in game."
                           : cutscenes.SelectedIndex == 1 ? "In 3D around you, turning with the scene's camera. The game only draws what its camera sees, so parts of the view can be empty or wrong. F11 can change it in game."
                                                          : "Kept as F11 has it: the game's camera with nothing steadied. F11 can change it in game.";
        skipPopup.Enabled = isVr;
        popupInfo.Text = !isVr ? "Not used in Flatscreen."
                       : skipPopup.Checked ? "Play starts at once after a load; the first seconds can hitch and the sidearm shows once the mod has seen it."
                                           : "After a load, \"VR mod loading\" counts down in the headset while the mod gets your weapons ready; the controllers wait until it closes.";
        sessionLogs.Enabled = isVr;
        sessionLogsInfo.Text = !isVr ? "Not used in Flatscreen."
                             : sessionLogs.Checked ? "The mod logs and measures everything while you play, for bug reports. A little slower. F11 Main can change it in game."
                                                   : "Once you are in the game, only warnings and errors are logged (the start and crash reports are kept). F11 Main can change it in game.";
        headsetInfo.Text = isVr ? HeadsetText() : "";
    }

    void Play()
    {
        if (Launcher.GameRunning()) {
            MessageBox.Show(this, "Far Cry 5 is already running. Close it first: the choice applies when the game starts.", "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (vr.Checked && !CheckInstall()) return;
        if (vr.Checked && !CheckGameOptions()) return;
        try {
            if (upscaler.SelectedIndex > 0) dlss.Checked = true;   // with DLSS off the mod never calls the upscaler
            if (upscaler.SelectedIndex >= 0) Launcher.WriteUpscaler(upscaler.SelectedIndex);
            Launcher.WriteChoice(flat.Checked, HeadsetId(), render.SelectedIndex == 0, Multiplier(),
                                 dlss.Checked, dlssKeepFile ? -1 : Math.Max(0, dlssMode.SelectedIndex), cutscenes.SelectedIndex, skipPopup.Checked, sessionLogs.Checked);
        } catch (Exception e) {
            MessageBox.Show(this, "Could not save the choice in " + Launcher.GameDir + ":\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var started = DateTime.Now;
        try {
            Launcher.StartGame();
        } catch (Exception e) {
            MessageBox.Show(this, "Saved, but the game did not start:\n" + e.Message + "\nStart Far Cry 5 from Steam or Ubisoft Connect.", "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        if (flat.Checked) {
            Close();
            return;
        }
        // VR: stay minimised until the game closes, then say so if VR never started or the game crashed.
        play.Enabled = false;
        WindowState = FormWindowState.Minimized;
        var watch = new Thread(delegate() {
            bool found;
            uint code;
            GameWatch.WaitForGame(out found, out code);
            try { BeginInvoke(new Action(delegate { GameClosed(started, found, code); })); } catch { }
        });
        watch.IsBackground = true;
        watch.Start();
    }

    // VR: the game's own options the mod needs (README "Required game options"), set in the game's file before it starts,
    // without asking (author 21 Nov: players answered Yes and still had them wrong; the mod sets them again at its start).
    // False: the player went back to the launcher.
    bool CheckGameOptions()
    {
        if (Launcher.WrongGameOptions() == null) return true;   // no file yet: the mod sets them once the game has written one
        var before = Launcher.WrongLabels(Launcher.WrongGameOptions());
        try {
            Launcher.FixGameOptions();
        } catch (Exception e) {
            var answer = MessageBox.Show(this, "Could not set the game options the VR mod needs in " + Launcher.GameProfilePath + ":\n" + e.Message +
                                         (before.Count > 0 ? "\n\nNot set:\n- " + string.Join("\n- ", before) : "") +
                                         "\n\nThe mod tries again when the game starts (its log, fc5vr_log.txt, says how it went in its [GameOptions] lines). " +
                                         "If they are still wrong in the game, set them in the game's Options.\n\nOK starts the game, Cancel goes back to the launcher.",
                                         "Far Cry 5 VR: game options", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
            if (answer == DialogResult.Cancel) return false;
        }
        UpdateInfo();
        FillChecks();
        return true;
    }

    void GameClosed(DateTime started, bool found, uint code)
    {
        var bin = Launcher.GameDir;
        var log = Report.LogPath(bin);
        var crash = Report.NewestCrash(bin);
        bool newCrash = crash != null && File.GetLastWriteTime(crash) >= started;
        bool vrRan = Report.LogSaysVrRan(log, started);
        bool badCode = code >= 0xC0000000u;
        if (found && vrRan && !newCrash && !badCode) {
            Close();   // a normal session
            return;
        }
        var why = new StringBuilder();
        if (!found) why.AppendLine("Far Cry 5 was not seen running within 3 minutes of Play.");
        else why.AppendLine(string.Format(CultureInfo.InvariantCulture, "Exit code 0x{0:X8}{1}.", code, badCode ? " (a crash)" : ""));
        if (newCrash) why.AppendLine("The mod wrote a crash report: " + Path.GetFileName(crash) + ".");
        if (!vrRan) {
            // A log older than this run is the last run's: its steps say nothing about this one.
            bool logNew = File.Exists(log) && File.GetLastWriteTime(log) >= started;
            var step = logNew ? Report.LastBootStep(log) : null;
            if (!Launcher.ModDllPresent()) why.AppendLine("VR did not start: " + Launcher.ModDllMissing);
            else if (!logNew) why.AppendLine("VR did not start: the mod wrote no log in this run, so it did not load (dxgi.dll is in bin: another program may have kept it out).");
            else why.AppendLine("VR did not start" + (step != null ? ": the last start step in the log was " + step : " (no start steps in the log).") );
            // Third-party OpenXR layers stopped VR on two testers' PCs (9 and 13 Nov).
            var odd = Launcher.EnabledLayers().FindAll(l => !l.Tested);
            if (logNew && odd.Count > 0)
                why.AppendLine("OpenXR layers enabled on this PC: " + string.Join(", ", odd.ConvertAll(l => Launcher.ShortLayer(l.Name))) + ". Layers like these stopped VR on " +
                               "testers' PCs: turn them off and try again (Install checks > OpenXR layers shows how).");
        }
        var exitInfo = "Last game run: " + (found ? string.Format(CultureInfo.InvariantCulture, "exit code 0x{0:X8}", code) : "the game was not seen running") +
                       ", Play at " + started.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + (vrRan ? ", VR ran" : ", VR did not start");
        WindowState = FormWindowState.Normal;
        Activate();
        var title = vrRan ? "Far Cry 5 crashed" : "Far Cry 5 closed during start / VR did not start";
        if (MessageBox.Show(this, why + "\nSave a report (FC5VR_report_<date>.zip in the game's bin folder) to send to the mod's author? It holds the mod's log and crash report, " +
                                  "the VR runtime's log, system details and Windows' error events. Nothing is uploaded.",
                            title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
            MakeReport(exitInfo);
        Close();
    }

    void MakeReport(string exitInfo)
    {
        try {
            Cursor = Cursors.WaitCursor;
            var path = Report.Make(Launcher.GameDir, exitInfo);
            Cursor = Cursors.Default;
            Report.ShowInExplorer(path);
        } catch (Exception e) {
            Cursor = Cursors.Default;
            MessageBox.Show(this, "Could not write the report:\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
