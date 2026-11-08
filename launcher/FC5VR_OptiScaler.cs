// FSR for the Far Cry 5 VR mod through OptiScaler (https://github.com/optiscaler/OptiScaler, GPL-3.0): the launcher's
// "Add FSR..." and "Remove FSR". The mod does not ship OptiScaler; the player downloads OptiScaler's own release and the
// launcher installs it from that file (the .7z as downloaded, a .zip, or the OptiScaler.dll of an extracted folder).
// It does what tools\fc5_optiscaler.ps1 install / uninstall does, and writes the same fc5vr_optiscaler_manifest.txt, so
// either one can remove what the other installed:
//   install   : OptiScaler.dll copied into bin under a proxy name the game loads (winmm.dll, which bink2w64.dll imports;
//               never dxgi.dll: that is the mod), its other files beside it, OptiScaler.ini with the mod's settings, and
//               every file and folder written listed in the manifest (first line "... proxy winmm.dll", read by the mod's
//               F11 Upscaler list, FarCry5Upscaler.cpp). Nothing that is already in bin is overwritten.
//   uninstall : deletes exactly what the manifest lists (and OptiScaler.log). The mod is back to plain DLSS.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

static class OptiScalerSetup
{
    // The version tested with the mod, and its official archive (GitHub release v0.9.4).
    public const string TestedVersion = "0.9.4";
    public const string ReleasePage = "https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4";
    const string ArchiveSha256 = "575cb4df866116093df75af607e37fd70e10f5163e0f23fd5c804142e80ef0ad";   // Optiscaler_0.9.4-final.20260718._MM.7z

    public static readonly string[] ProxyNames = { "winmm.dll", "version.dll", "wininet.dll", "winhttp.dll" };

    static string Manifest(string bin) { return Path.Combine(bin, "fc5vr_optiscaler_manifest.txt"); }

    // A manifest path ("D3D12_Optiscaler\\D3D12Core.dll", folders end in \\) under bin.
    static string Under(string bin, string rel) { return Path.Combine(bin, rel.TrimEnd('\\').Replace('\\', Path.DirectorySeparatorChar)); }

    public static bool Installed(string bin) { return ManifestItems(bin).Count > 0; }

    static List<string> ManifestItems(string bin)
    {
        var items = new List<string>();
        try {
            if (!File.Exists(Manifest(bin))) return items;
            foreach (var raw in File.ReadAllLines(Manifest(bin))) {
                var l = raw.Trim().TrimStart('\uFEFF');
                if (l.Length > 0 && !l.StartsWith("#")) items.Add(l);
            }
        } catch (Exception) { }
        return items;
    }

    // ---- finding the download ----

    [DllImport("shell32.dll")] static extern int SHGetKnownFolderPath([MarshalAs(UnmanagedType.LPStruct)] Guid id, uint flags, IntPtr token, out IntPtr path);

    public static string DownloadsFolder()
    {
        try {
            IntPtr p;
            if (SHGetKnownFolderPath(new Guid("374DE290-123F-4565-9164-39C4925E467B"), 0, IntPtr.Zero, out p) == 0) {
                var s = Marshal.PtrToStringUni(p);
                Marshal.FreeCoTaskMem(p);
                if (Directory.Exists(s)) return s;
            }
        } catch (Exception) { }
        var d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        return Directory.Exists(d) ? d : null;
    }

    // The newest OptiScaler 0.9.4 in the Downloads folder: its archive (.7z or .zip) or a folder extracted from it
    // (its OptiScaler.dll, one folder level down at most). Null if there is none.
    public static string FindDownloaded()
    {
        var dl = DownloadsFolder();
        if (dl == null) return null;
        string best = null;
        var bestTime = DateTime.MinValue;
        Action<string, DateTime> take = delegate(string p, DateTime t) { if (t > bestTime) { best = p; bestTime = t; } };
        try {
            foreach (var f in Directory.GetFiles(dl, "*optiscaler*")) {
                var n = Path.GetFileName(f).ToLowerInvariant();
                if (n.Contains(TestedVersion) && (n.EndsWith(".7z") || n.EndsWith(".zip"))) take(f, File.GetLastWriteTime(f));
            }
            foreach (var d in Directory.GetDirectories(dl, "*optiscaler*")) {
                if (!Path.GetFileName(d).Contains(TestedVersion)) continue;
                var dll = FindDll(d);
                if (dll != null) take(dll, Directory.GetLastWriteTime(d));
            }
        } catch (Exception) { }
        return best;
    }

    // OptiScaler.dll in the folder or in one of its subfolders (an archive extracted into a folder of its own name).
    static string FindDll(string dir)
    {
        var p = Path.Combine(dir, "OptiScaler.dll");
        if (File.Exists(p)) return p;
        try {
            foreach (var d in Directory.GetDirectories(dir)) {
                p = Path.Combine(d, "OptiScaler.dll");
                if (File.Exists(p)) return p;
            }
        } catch (Exception) { }
        return null;
    }

    // ---- unpacking ----

    // A folder holding OptiScaler.dll and OptiScaler.ini for what the player picked: the .7z or .zip (unpacked into
    // temp, returned in tempDir to delete afterwards), OptiScaler.dll itself, or a folder. official: the archive is the
    // official 0.9.4 one (its SHA-256). Throws with a message for the player.
    public static string Prepare(string picked, out string tempDir, out bool official)
    {
        tempDir = null;
        official = false;
        string dll = null;
        if (Directory.Exists(picked)) dll = FindDll(picked);
        else if (Path.GetFileName(picked).Equals("OptiScaler.dll", StringComparison.OrdinalIgnoreCase)) dll = picked;
        else {
            var ext = Path.GetExtension(picked).ToLowerInvariant();
            if (ext != ".7z" && ext != ".zip") throw new Exception("Choose OptiScaler's download (the .7z file), or OptiScaler.dll in a folder you extracted it to.");
            official = Sha256(picked) == ArchiveSha256;
            tempDir = Path.Combine(Path.GetTempPath(), "fc5vr_optiscaler_" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(tempDir);
            if (ext == ".zip") ZipFile.ExtractToDirectory(picked, tempDir);
            else Extract7z(picked, tempDir);
            dll = FindDll(tempDir);
            if (dll == null) throw new Exception(Path.GetFileName(picked) + " has no OptiScaler.dll inside: it is not an OptiScaler release (the \"Source code\" downloads will not do).");
        }
        if (dll == null) throw new Exception("No OptiScaler.dll in " + picked + ".");
        var src = Path.GetDirectoryName(dll);
        if (!File.Exists(Path.Combine(src, "OptiScaler.ini"))) throw new Exception("No OptiScaler.ini next to " + dll + ": extract the whole OptiScaler archive.");
        return src;
    }

    // The .7z through 7-Zip if it is installed, else Windows' own tar (it reads .7z on Windows 11).
    static void Extract7z(string archive, string dir)
    {
        var tools = new List<KeyValuePair<string, string>>();
        foreach (var root in new[] { RegistryView.Registry64, RegistryView.Registry32 }) {
            try {
                using (var b = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, root))
                using (var k = b.OpenSubKey(@"SOFTWARE\7-Zip")) {
                    var p = k == null ? null : (k.GetValue("Path64") ?? k.GetValue("Path")) as string;
                    if (!string.IsNullOrEmpty(p)) tools.Add(new KeyValuePair<string, string>(Path.Combine(p, "7z.exe"), "x -y \"-o" + dir + "\" \"" + archive + "\""));
                }
            } catch (Exception) { }
        }
        foreach (var pf in new[] { Environment.GetEnvironmentVariable("ProgramW6432"), Environment.GetEnvironmentVariable("ProgramFiles"), Environment.GetEnvironmentVariable("ProgramFiles(x86)") })
            if (!string.IsNullOrEmpty(pf)) tools.Add(new KeyValuePair<string, string>(Path.Combine(pf, @"7-Zip\7z.exe"), "x -y \"-o" + dir + "\" \"" + archive + "\""));
        tools.Add(new KeyValuePair<string, string>(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"), "-x -f \"" + archive + "\" -C \"" + dir + "\""));
        var tried = new List<string>();
        foreach (var t in tools) {
            if (!File.Exists(t.Key) || tried.Contains(t.Key.ToLowerInvariant())) continue;
            tried.Add(t.Key.ToLowerInvariant());
            try {
                var psi = new ProcessStartInfo(t.Key, t.Value) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = dir };
                using (var p = Process.Start(psi)) {
                    if (!p.WaitForExit(180000)) { try { p.Kill(); } catch (Exception) { } continue; }
                    if (p.ExitCode == 0 && FindDll(dir) != null) return;
                }
            } catch (Exception) { }
        }
        throw new Exception("This PC could not unpack the .7z file (Windows 10 cannot without 7-Zip). Unpack it yourself: " +
                            "right-click it > Extract All (Windows 11), or with 7-Zip (7-zip.org). Then press Add FSR again and choose OptiScaler.dll in the unpacked folder.");
    }

    static string Sha256(string path)
    {
        using (var s = File.OpenRead(path))
        using (var h = SHA256.Create()) {
            var sb = new StringBuilder();
            foreach (var x in h.ComputeHash(s)) sb.Append(x.ToString("x2"));
            return sb.ToString();
        }
    }

    // OptiScaler.dll's file version ("0.9.4.0"), or "".
    public static string DllVersion(string src)
    {
        try { return FileVersionInfo.GetVersionInfo(Path.Combine(src, "OptiScaler.dll")).FileVersion ?? ""; }
        catch (Exception) { return ""; }
    }

    // ---- which proxy name the game loads: the DLL names the game's own modules import ----

    static uint U32(byte[] b, int o) { return BitConverter.ToUInt32(b, o); }
    static ushort U16(byte[] b, int o) { return BitConverter.ToUInt16(b, o); }

    // The imported DLL names (import and delay-import tables) of a PE file, lower case. Reads only the headers and the
    // import tables, so the game's large modules are not read whole.
    static List<string> Imports(string path)
    {
        var names = new List<string>();
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            Func<long, int, byte[]> read = delegate(long off, int n) {
                var buf = new byte[n];
                if (off < 0 || off >= fs.Length) return null;
                fs.Position = off;
                int got = 0;
                while (got < n) { int r = fs.Read(buf, got, n - got); if (r <= 0) break; got += r; }
                return buf;
            };
            var dos = read(0, 64);
            if (dos == null || dos[0] != 'M' || dos[1] != 'Z') return names;
            int pe = (int)U32(dos, 0x3C);
            var h = read(pe, 24);
            if (h == null || U32(h, 0) != 0x4550) return names;
            int nsec = U16(h, 6), optSize = U16(h, 20);
            var opt = read(pe + 24, optSize);
            if (opt == null || optSize < 96) return names;
            bool pe32plus = U16(opt, 0) == 0x20B;
            int dirs = pe32plus ? 112 : 96;
            var sec = read(pe + 24 + optSize, nsec * 40);
            if (sec == null) return names;
            Func<uint, long> rva2off = delegate(uint rva) {
                for (int i = 0; i < nsec; i++) {
                    int s = i * 40;
                    uint va = U32(sec, s + 12), vs = U32(sec, s + 8), raw = U32(sec, s + 20), rs = U32(sec, s + 16);
                    uint size = Math.Max(vs, rs);
                    if (rva >= va && rva < va + size) return (long)rva - va + raw;
                }
                return -1;
            };
            Func<long, string> cstr = delegate(long off) {
                var b = read(off, 260);
                if (b == null) return "";
                int n = Array.IndexOf(b, (byte)0);
                return Encoding.ASCII.GetString(b, 0, n < 0 ? b.Length : n);
            };
            // 1 = import directory (20-byte descriptors, name at +12), 13 = delay import (32-byte descriptors, name at +4)
            foreach (var d in new[] { new[] { 1, 20, 12 }, new[] { 13, 32, 4 } }) {
                if (dirs + d[0] * 8 + 8 > optSize) continue;
                uint rva = U32(opt, dirs + d[0] * 8);
                if (rva == 0) continue;
                long off = rva2off(rva);
                for (int guard = 0; off >= 0 && guard < 4096; off += d[1], guard++) {
                    var desc = read(off, d[1]);
                    if (desc == null) break;
                    uint nameRva = U32(desc, d[2]);
                    if (nameRva == 0) break;
                    names.Add(cstr(rva2off(nameRva)).ToLowerInvariant());
                }
            }
        }
        return names;
    }

    // The first of ProxyNames that a module in bin imports (the mod's dxgi.dll and OptiScaler's own files left out)
    // and that is not already a file there. Null if none.
    static string ProxyName(string bin, out string importedBy)
    {
        importedBy = null;
        var mine = ManifestItems(bin);
        var by = new Dictionary<string, string>();
        foreach (var f in Directory.GetFiles(bin)) {
            var n = Path.GetFileName(f);
            var ext = Path.GetExtension(f).ToLowerInvariant();
            if ((ext != ".exe" && ext != ".dll") || n.Equals("dxgi.dll", StringComparison.OrdinalIgnoreCase) || mine.Contains(n)) continue;
            try {
                foreach (var i in Imports(f)) if (!by.ContainsKey(i)) by[i] = n;
            } catch (Exception) { }
        }
        foreach (var n in ProxyNames)
            if (by.ContainsKey(n) && !File.Exists(Path.Combine(bin, n))) { importedBy = by[n]; return n; }
        return null;
    }

    // ---- OptiScaler.ini: the mod's settings (keeps the release's file and comments) ----

    static void SetIniValue(List<string> lines, string section, string key, string value)
    {
        string cur = "";
        int sectionEnd = -1;
        for (int i = 0; i < lines.Count; i++) {
            var l = lines[i].Trim();
            var m = Regex.Match(l, @"^\[(.+)\]$");
            if (m.Success) {
                if (cur == section) { sectionEnd = i; break; }
                cur = m.Groups[1].Value;
                continue;
            }
            if (cur == section && Regex.IsMatch(l, "^" + Regex.Escape(key) + @"\s*=")) { lines[i] = key + "=" + value; return; }
        }
        if (sectionEnd >= 0) lines.Insert(sectionEnd, key + "=" + value);
        else if (cur == section) lines.Add(key + "=" + value);
        else { lines.Add("[" + section + "]"); lines.Add(key + "=" + value); }
    }

    // As fc5_optiscaler.ps1 Write-ModSettings; every other key keeps OptiScaler's "auto".
    static void WriteModSettings(string ini, string upscaler)
    {
        var lines = new List<string>(File.ReadAllLines(ini));
        string[][] s = {
            new[] { "Upscalers", "Dx11Upscaler", upscaler },
            // No frame generation: the headset runtime paces the eye images, not the desktop window.
            new[] { "FrameGen", "Enabled", "false" }, new[] { "FrameGen", "FGInput", "nofg" }, new[] { "FrameGen", "FGOutput", "nofg" },
            // No ImGui overlay or splash on the game's window: the mod builds the eye images (and the HUD layer) from that back buffer.
            new[] { "Menu", "OverlayMenu", "false" }, new[] { "Menu", "DisableSplash", "true" }, new[] { "Menu", "ShowFps", "false" },
            // Only the DLSS route is used; the game has no FSR / XeSS / FFX of its own.
            new[] { "Inputs", "EnableDlssInputs", "true" }, new[] { "Inputs", "EnableFsr2Inputs", "false" }, new[] { "Inputs", "EnableFsr3Inputs", "false" },
            new[] { "Inputs", "EnableFfxInputs", "false" }, new[] { "Inputs", "EnableXeSSInputs", "false" }, new[] { "Inputs", "EnableHotSwapping", "false" },
            // One sharpener only: the mod's own (F11 "DLSS sharpening") still runs after the upscaler.
            new[] { "Sharpness", "OverrideSharpness", "true" }, new[] { "Sharpness", "Sharpness", "0.0" },
            // Each eye's camera: about 96 deg vertical (Quest 3 class). FSR uses it only to judge depth differences.
            new[] { "FSR", "VerticalFov", "96.0" },
            // The mod sizes the render target itself; no ratio overrides, no output scaling.
            new[] { "OutputScaling", "Enabled", "false" }, new[] { "UpscaleRatio", "UpscaleRatioOverrideEnabled", "false" },
            new[] { "QualityOverrides", "QualityRatioOverrideEnabled", "false" },
            // Nothing else loaded or blocked.
            new[] { "Plugins", "LoadAsiPlugins", "false" }, new[] { "Plugins", "LoadReshade", "false" }, new[] { "Plugins", "LoadSpecialK", "false" },
            new[] { "fakenvapi", "UseFakenvapi", "false" }, new[] { "Hotfix", "CheckForUpdate", "false" }, new[] { "Hotfix", "DisableOverlays", "false" },
            // A log to check which upscaler ran, at Info.
            new[] { "Log", "LogToFile", "true" }, new[] { "Log", "LogLevel", "2" }, new[] { "Log", "SingleFile", "true" },
        };
        foreach (var e in s) SetIniValue(lines, e[0], e[1], e[2]);
        File.WriteAllLines(ini, lines.ToArray(), new UTF8Encoding(false));
    }

    // ---- install / uninstall ----

    // The release's setup scripts and the extras the mod does not use stay out (as fc5_optiscaler.ps1).
    static readonly Regex Skip = new Regex(@"^(setup_.*|Remove.*OptiScaler.*|.*\.bat|.*\.sh|nvapi64\.dll|fakenvapi\.ini|fakenvapi\.log|dlssg_to_fsr3_amd_is_better\.dll|dlss-enabler.*\.dll|OptiScaler\.dll|OptiScaler\.ini|!!.*)$",
                                           RegexOptions.IgnoreCase);

    // Installs from src (a folder with OptiScaler.dll and OptiScaler.ini) into bin with the given Dx11Upscaler.
    // from: what the player picked, for the manifest's first line. Returns the proxy name. Throws with a message.
    public static string Install(string bin, string src, string upscaler, string from)
    {
        if (Installed(bin)) throw new Exception("FSR (OptiScaler) is already installed.");
        string importedBy;
        var proxy = ProxyName(bin, out importedBy);
        if (proxy == null) throw new Exception("None of " + string.Join(", ", ProxyNames) + " is loaded by the game's files in bin, or each is already there (another mod?). Nothing was copied.");
        var srcRoot = Path.GetFullPath(src).TrimEnd('\\', '/');
        var plan = new List<KeyValuePair<string, string>>();   // from, to (relative to bin)
        plan.Add(new KeyValuePair<string, string>(Path.Combine(srcRoot, "OptiScaler.dll"), proxy));
        plan.Add(new KeyValuePair<string, string>(Path.Combine(srcRoot, "OptiScaler.ini"), "OptiScaler.ini"));
        foreach (var f in Directory.GetFiles(srcRoot, "*", SearchOption.AllDirectories)) {
            if (Skip.IsMatch(Path.GetFileName(f))) continue;
            plan.Add(new KeyValuePair<string, string>(f, f.Substring(srcRoot.Length + 1).Replace(Path.DirectorySeparatorChar, '\\')));
        }
        var clash = new List<string>();
        foreach (var p in plan) if (File.Exists(Under(bin, p.Value)) || Directory.Exists(Under(bin, p.Value))) clash.Add(p.Value);
        if (clash.Count > 0) throw new Exception("These are already in the game's bin folder and were not put there by the launcher, so nothing was copied: " + string.Join(", ", clash));
        var written = new List<string>();
        written.Add("# FC5VR_Launcher install " + DateTime.Now.ToString("s") + " from " + from + " (OptiScaler " + DllVersion(srcRoot) + "), proxy " + proxy);
        try {
            foreach (var p in plan) {
                // Folders one level at a time, each one listed so uninstall removes exactly what was made.
                int cut = p.Value.LastIndexOf('\\');
                if (cut > 0) {
                    var parent = p.Value.Substring(0, cut);
                    var rel = "";
                    foreach (var seg in parent.Split(new[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries)) {
                        rel = rel.Length > 0 ? rel + "\\" + seg : seg;
                        var full = Under(bin, rel);
                        if (!Directory.Exists(full)) { Directory.CreateDirectory(full); written.Add(rel + "\\"); }
                    }
                }
                File.Copy(p.Key, Under(bin, p.Value));
                written.Add(p.Value);
            }
        } finally {
            File.WriteAllLines(Manifest(bin), written.ToArray(), new UTF8Encoding(false));
        }
        WriteModSettings(Path.Combine(bin, "OptiScaler.ini"), upscaler);
        return proxy;
    }

    // Deletes exactly what the manifest lists (a proxy turned off as <name>.fc5vr_off too), OptiScaler.log and the manifest.
    public static void Uninstall(string bin)
    {
        var items = ManifestItems(bin);
        var dirs = new List<string>();
        foreach (var i in items) {
            var p = Under(bin, i);
            if (i.EndsWith("\\")) { dirs.Add(p); continue; }
            if (File.Exists(p)) File.Delete(p);
            if (File.Exists(p + ".fc5vr_off")) File.Delete(p + ".fc5vr_off");
        }
        dirs.Sort(delegate(string a, string b) { return b.Length.CompareTo(a.Length); });
        foreach (var d in dirs)
            if (Directory.Exists(d) && Directory.GetFiles(d, "*", SearchOption.AllDirectories).Length == 0) Directory.Delete(d, true);
        var log = Path.Combine(bin, "OptiScaler.log");
        if (File.Exists(log)) File.Delete(log);
        if (File.Exists(Manifest(bin))) File.Delete(Manifest(bin));
    }
}
