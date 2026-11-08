<#
fc5_optiscaler.ps1: opt-in FSR for the Far Cry 5 VR mod through OptiScaler (https://github.com/optiscaler/OptiScaler).

The mod's DLSS code is not changed. OptiScaler answers the mod's existing NGX (DLSS) calls and runs FSR in their place,
one FSR context per eye (the mod already creates one DLSS feature per eye). Without OptiScaler installed, nothing changes.

  install   : copies an extracted OptiScaler release into bin\ under a proxy name the game loads (winmm.dll or version.dll,
              never dxgi.dll: that is the mod), writes bin\OptiScaler.ini with the mod's settings, and lists every file it
              wrote in bin\fc5vr_optiscaler_manifest.txt. Refuses to overwrite any file it did not write.
  uninstall : deletes exactly the files in the manifest (and OptiScaler.log). The mod is back to plain DLSS.
  set       : changes the upscaler in the installed OptiScaler.ini (next game start).
  status    : what is installed, the upscaler, and the last lines of OptiScaler.log that matter.

Upscalers (OptiScaler's D3D11 choices):
  fsr31  : FSR 3.1 ported to native D3D11 (default here: no D3D12 bridge, every GPU)
  ffx_12 : AMD's FidelityFX DLLs through a D3D12 bridge (D3D11on12): FSR 4 on Radeon RX 9000, FSR 3.1.x elsewhere. Heavier.
  fsr22  : FSR 2.2.1 native D3D11 (OptiScaler's own fallback)
  dlss   : (set only) OptiScaler turned off: its proxy is renamed to <name>.fc5vr_off (the same name the mod and the launcher use), so the game runs plain DLSS; set fsr31 etc. turns it on again.

Examples (PowerShell, with this script in the game's bin folder, next to FarCry5.exe and the mod's dxgi.dll):
  .\fc5_optiscaler.ps1 install -Source $env:USERPROFILE\Downloads\OptiScaler_0.9.4 -Upscaler fsr31
  .\fc5_optiscaler.ps1 set -Upscaler ffx_12
  .\fc5_optiscaler.ps1 status
  .\fc5_optiscaler.ps1 uninstall
#>
param(
    [Parameter(Position = 0)][ValidateSet('install', 'uninstall', 'set', 'status')][string]$Action = 'status',
    [string]$Source,
    [ValidateSet('fsr31', 'ffx_12', 'fsr22', 'dlss')][string]$Upscaler = 'fsr31',
    [ValidateSet('', 'winmm.dll', 'version.dll', 'wininet.dll', 'winhttp.dll')][string]$ProxyName = '',
    [string]$GameBin = '',   # the game's bin folder; empty = the folder this script is in (put it next to FarCry5.exe)
    [switch]$Verbose2
)

$ErrorActionPreference = 'Stop'
if (-not $GameBin) { $GameBin = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not (Test-Path -LiteralPath (Join-Path $GameBin 'FarCry5.exe'))) { throw "No FarCry5.exe in ${GameBin}: put this script in the game's bin folder (next to FarCry5.exe and the mod's dxgi.dll), or pass -GameBin <that folder>." }
$Manifest = Join-Path $GameBin 'fc5vr_optiscaler_manifest.txt'
$Ini      = Join-Path $GameBin 'OptiScaler.ini'
$Log      = Join-Path $GameBin 'OptiScaler.log'

# ---------------------------------------------------------------- PE imports (which proxy name the game loads)
if (-not ('Fc5PeImports' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
public static class Fc5PeImports {
    static uint U32(byte[] b, long o) { return BitConverter.ToUInt32(b, (int)o); }
    static ushort U16(byte[] b, long o) { return BitConverter.ToUInt16(b, (int)o); }
    public static List<string> Read(string path) {
        var names = new List<string>();
        byte[] b = File.ReadAllBytes(path);
        long pe = U32(b, 0x3C);
        if (U32(b, pe) != 0x4550) return names;
        int nsec = U16(b, pe + 6);
        int optSize = U16(b, pe + 20);
        long opt = pe + 24;
        bool pe32plus = U16(b, opt) == 0x20B;
        long dirs = opt + (pe32plus ? 112 : 96);
        long sec = opt + optSize;
        Func<uint, long> rva2off = rva => {
            for (int i = 0; i < nsec; i++) {
                long s = sec + i * 40;
                uint va = U32(b, s + 12), vs = U32(b, s + 8), raw = U32(b, s + 20), rs = U32(b, s + 16);
                uint size = Math.Max(vs, rs);
                if (rva >= va && rva < va + size) return (long)rva - va + raw;
            }
            return -1;
        };
        Func<long, string> cstr = off => {
            var sb = new StringBuilder();
            while (off >= 0 && off < b.Length && b[off] != 0) sb.Append((char)b[off++]);
            return sb.ToString();
        };
        // 1 = import directory (20-byte descriptors, name at +12), 13 = delay import (32-byte descriptors, name at +4)
        foreach (var d in new[] { new[] { 1, 20, 12 }, new[] { 13, 32, 4 } }) {
            uint rva = U32(b, dirs + d[0] * 8);
            if (rva == 0) continue;
            long off = rva2off(rva);
            for (; off >= 0 && off + d[1] <= b.Length; off += d[1]) {
                uint nameRva = U32(b, off + d[2]);
                if (nameRva == 0) break;
                names.Add(cstr(rva2off(nameRva)).ToLowerInvariant());
            }
        }
        return names;
    }
}
"@
}

function Get-GameImports {
    # FarCry5.exe and FC_m64.dll import none of the proxy names; bink2w64.dll imports winmm.dll (builder, 3 Nov 2026).
    # So every module the game ships in bin is read, except the mod's own dxgi.dll and files this script wrote.
    $mine = @(Read-Manifest)
    $all = @()
    $mods = @(Get-ChildItem -LiteralPath $GameBin -File | Where-Object { $_.Extension -in '.exe', '.dll' -and $_.Name -ne 'dxgi.dll' -and $mine -notcontains $_.Name })
    foreach ($m in $mods) {
        try { foreach ($n in [Fc5PeImports]::Read($m.FullName)) { $all += [pscustomobject]@{ Name = $n; By = $m.Name } } }
        catch { Write-Warning "could not read the imports of $($m.Name): $_" }
    }
    return $all
}

function Get-ProxyName {
    if ($ProxyName) { return $ProxyName }
    $imports = Get-GameImports
    foreach ($n in @('winmm.dll', 'version.dll', 'wininet.dll', 'winhttp.dll')) {
        $by = @($imports | Where-Object { $_.Name -eq $n } | ForEach-Object { $_.By } | Sort-Object -Unique)
        if ($by.Count -gt 0 -and -not (Test-Path -LiteralPath (Join-Path $GameBin $n))) {
            Write-Host "  $n is imported by $($by -join ', ')"
            return $n
        }
    }
    throw "None of winmm.dll, version.dll, wininet.dll, winhttp.dll is imported by a module in bin (or each already exists there). Pass -ProxyName winmm.dll to force one."
}

# ---------------------------------------------------------------- OptiScaler.ini editing (keeps the release's file and comments)
function Set-IniValue([string]$path, [string]$section, [string]$key, [string]$value) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($x in @(Get-Content -LiteralPath $path -Encoding UTF8)) { $lines.Add([string]$x) }
    $cur = ''; $sectionEnd = -1; $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i].Trim()
        if ($l -match '^\[(.+)\]$') {
            if ($cur -eq $section -and -not $done) { $sectionEnd = $i; break }
            $cur = $Matches[1]
            continue
        }
        if ($cur -eq $section -and $l -match ('^' + [regex]::Escape($key) + '\s*=')) {
            $lines[$i] = "$key=$value"; $done = $true; break
        }
    }
    if (-not $done) {
        if ($cur -eq $section -and $sectionEnd -lt 0) { $lines.Add("$key=$value") }
        elseif ($sectionEnd -ge 0) { $lines.Insert($sectionEnd, "$key=$value") }
        else { $lines.Add("[$section]"); $lines.Add("$key=$value") }
    }
    [System.IO.File]::WriteAllLines($path, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-ModSettings([string]$path, [string]$up) {
    # The mod's choices; every other key keeps OptiScaler's "auto". See README.md next to this script for why.
    $s = @(
        @('Upscalers', 'Dx11Upscaler', $up),
        # No frame generation: the headset runtime (Virtual Desktop / SteamVR / OFXR) paces the eye images, not the desktop window.
        @('FrameGen', 'Enabled', 'false'), @('FrameGen', 'FGInput', 'nofg'), @('FrameGen', 'FGOutput', 'nofg'),
        # No ImGui overlay or splash on the game's window: the mod builds the eye images (and the HUD layer) from that back buffer.
        @('Menu', 'OverlayMenu', 'false'), @('Menu', 'DisableSplash', 'true'), @('Menu', 'ShowFps', 'false'),
        # Only the DLSS route is used; the game has no FSR / XeSS / FFX of its own.
        @('Inputs', 'EnableDlssInputs', 'true'), @('Inputs', 'EnableFsr2Inputs', 'false'), @('Inputs', 'EnableFsr3Inputs', 'false'),
        @('Inputs', 'EnableFfxInputs', 'false'), @('Inputs', 'EnableXeSSInputs', 'false'), @('Inputs', 'EnableHotSwapping', 'false'),
        # One sharpener only: the mod's own (F11 "DLSS sharpening", dlsssharp) still runs after the upscaler.
        @('Sharpness', 'OverrideSharpness', 'true'), @('Sharpness', 'Sharpness', '0.0'),
        # Each eye's camera: about 96 deg vertical (Quest 3 class). FSR uses it only to judge depth differences.
        @('FSR', 'VerticalFov', '96.0'),
        # The mod sizes the render target itself; no ratio overrides, no output scaling, no mip/aniso overrides.
        @('OutputScaling', 'Enabled', 'false'), @('UpscaleRatio', 'UpscaleRatioOverrideEnabled', 'false'),
        @('QualityOverrides', 'QualityRatioOverrideEnabled', 'false'),
        # Nothing else loaded or blocked.
        @('Plugins', 'LoadAsiPlugins', 'false'), @('Plugins', 'LoadReshade', 'false'), @('Plugins', 'LoadSpecialK', 'false'),
        @('fakenvapi', 'UseFakenvapi', 'false'), @('Hotfix', 'CheckForUpdate', 'false'), @('Hotfix', 'DisableOverlays', 'false'),
        # A log to check which upscaler ran, at Info (Trace, the default, writes every frame).
        @('Log', 'LogToFile', 'true'), @('Log', 'LogLevel', '2'), @('Log', 'SingleFile', 'true')
    )
    foreach ($e in $s) { Set-IniValue $path $e[0] $e[1] $e[2] }
}

function Get-InstalledProxy {
    # The proxy name is on the manifest's first line ("... proxy winmm.dll").
    $first = if (Test-Path -LiteralPath $Manifest) { Get-Content -LiteralPath $Manifest -TotalCount 1 } else { '' }
    if ($first -match 'proxy (\S+)') { return $Matches[1] }
    return 'winmm.dll'
}

function Test-GameClosed {
    if (Get-Process -Name 'FarCry5' -ErrorAction SilentlyContinue) { throw 'Far Cry 5 is running: close it first.' }
}

function Read-Manifest {
    if (-not (Test-Path $Manifest)) { return @() }
    return Get-Content -LiteralPath $Manifest | Where-Object { $_ -and -not $_.StartsWith('#') }
}

switch ($Action) {
    'install' {
        Test-GameClosed
        if (-not $Source) { throw '-Source <extracted OptiScaler release folder> is needed.' }
        if (-not (Test-Path (Join-Path $Source 'OptiScaler.dll'))) { throw "No OptiScaler.dll in $Source (extract the release archive first; the source code will not do)." }
        if (-not (Test-Path (Join-Path $Source 'OptiScaler.ini'))) { throw "No OptiScaler.ini in $Source." }
        if (Read-Manifest) { throw "OptiScaler is already installed (see $Manifest). Run uninstall first, or 'set' to change the upscaler." }
        if (-not (Test-Path (Join-Path $GameBin 'dxgi.dll'))) { Write-Warning 'bin\dxgi.dll (the mod) is not there: OptiScaler will be installed but the mod is not.' }
        $proxy = Get-ProxyName
        Write-Host "Proxy name: $proxy (loaded by the game; the mod stays dxgi.dll)"
        # What to copy: the release, minus its setup scripts and the extras the mod does not use.
        $skip = '^(setup_.*|Remove.*OptiScaler.*|.*\.bat|.*\.sh|nvapi64\.dll|fakenvapi\.ini|fakenvapi\.log|dlssg_to_fsr3_amd_is_better\.dll|dlss-enabler.*\.dll|OptiScaler\.dll|OptiScaler\.ini|!!.*)$'
        $srcRoot = (Resolve-Path $Source).Path.TrimEnd('\')
        $files = Get-ChildItem -LiteralPath $srcRoot -Recurse -File | Where-Object { $_.Name -notmatch $skip }
        $plan = @(@{ From = (Join-Path $srcRoot 'OptiScaler.dll'); To = $proxy }, @{ From = (Join-Path $srcRoot 'OptiScaler.ini'); To = 'OptiScaler.ini' })
        foreach ($f in $files) { $plan += @{ From = $f.FullName; To = $f.FullName.Substring($srcRoot.Length + 1) } }
        $clash = $plan | Where-Object { Test-Path -LiteralPath (Join-Path $GameBin $_.To) } | ForEach-Object { $_.To }
        if ($clash) { throw "These already exist in bin and were not written by this script; nothing was copied: $($clash -join ', ')" }
        $written = @("# fc5_optiscaler.ps1 install $(Get-Date -Format s) from $srcRoot, proxy $proxy")
        try {
            foreach ($p in $plan) {
                $dst = Join-Path $GameBin $p.To
                # Folders one level at a time, each one listed so uninstall removes exactly what was made.
                $rel = ''
                foreach ($seg in @((Split-Path $p.To -Parent) -split '\\' | Where-Object { $_ })) {
                    $rel = if ($rel) { "$rel\$seg" } else { $seg }
                    $full = Join-Path $GameBin $rel
                    if (-not (Test-Path -LiteralPath $full)) { New-Item -ItemType Directory -Path $full | Out-Null; $written += "$rel\" }
                }
                Copy-Item -LiteralPath $p.From -Destination $dst
                $written += $p.To
                if ($Verbose2) { Write-Host "  + $($p.To)" }
            }
        } finally {
            Set-Content -LiteralPath $Manifest -Value $written -Encoding UTF8
        }
        Write-ModSettings $Ini $Upscaler
        Write-Host ("Installed {0} files (list: {1}); upscaler {2}. Next game start uses it; check bin\OptiScaler.log after a run." -f ($written.Count - 1), $Manifest, $Upscaler)
    }
    'uninstall' {
        Test-GameClosed
        $items = Read-Manifest
        if (-not $items) { Write-Host 'OptiScaler is not installed by this script (no manifest).'; break }
        $dirs = @()
        foreach ($i in $items) {
            $p = Join-Path $GameBin $i
            if ($i.EndsWith('\')) { $dirs += $p; continue }
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
            if (Test-Path -LiteralPath "$p.fc5vr_off") { Remove-Item -LiteralPath "$p.fc5vr_off" -Force }   # proxy turned off by the Upscaler choice (DLSS)
        }
        foreach ($d in ($dirs | Sort-Object Length -Descending)) {
            if ((Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Recurse -File)) { Remove-Item -LiteralPath $d -Recurse -Force }
        }
        if (Test-Path $Log) { Remove-Item -LiteralPath $Log -Force }
        Remove-Item -LiteralPath $Manifest -Force
        Write-Host 'OptiScaler removed; the mod runs plain DLSS again.'
    }
    'set' {
        Test-GameClosed
        if (-not (Test-Path $Ini) -or -not (Read-Manifest)) { throw 'OptiScaler is not installed by this script.' }
        # DLSS = OptiScaler not loaded at all (its proxy renamed to .fc5vr_off, as F11 and the launcher do), not OptiScaler's own DLSS pass-through.
        $proxy = Get-InstalledProxy
        $on = Join-Path $GameBin $proxy; $off = "$on.fc5vr_off"
        if ($Upscaler -eq 'dlss') {
            if (Test-Path -LiteralPath $on) { Move-Item -LiteralPath $on -Destination $off -Force }
            Write-Host "OptiScaler off ($proxy -> $proxy.fc5vr_off): plain DLSS from the next game start."
        } else {
            if (-not (Test-Path -LiteralPath $on) -and (Test-Path -LiteralPath $off)) { Move-Item -LiteralPath $off -Destination $on }
            Set-IniValue $Ini 'Upscalers' 'Dx11Upscaler' $Upscaler
            Write-Host "OptiScaler on, Dx11Upscaler=$Upscaler (from the next game start)."
        }
    }
    'status' {
        $items = Read-Manifest
        if (-not $items) { Write-Host 'OptiScaler: not installed (the mod runs plain DLSS).'; break }
        Write-Host "OptiScaler: installed, $($items.Count) files ($Manifest)"
        Write-Host ('  ' + ((Get-Content $Manifest -TotalCount 1) -replace '^# ', ''))
        $up = (Select-String -LiteralPath $Ini -Pattern '^Dx11Upscaler=' | Select-Object -First 1).Line
        $proxy = Get-InstalledProxy
        if (Test-Path -LiteralPath (Join-Path $GameBin $proxy)) { Write-Host "  on ($proxy), $up" }
        else { Write-Host "  off ($proxy.fc5vr_off): the game runs plain DLSS; $up is kept for when it is turned on again" }
        if (Test-Path $Log) {
            Write-Host '  OptiScaler.log (feature creation, upscaler, errors):'
            Select-String -LiteralPath $Log -Pattern 'Creating new|HandleId|feature|Falling back|changing backend|nvngx call|ERROR|error' |
                Select-Object -Last 25 | ForEach-Object { Write-Host ('    ' + $_.Line) }
        } else { Write-Host '  no OptiScaler.log yet (the game has not run with it, or OptiScaler did not load: check the proxy name).' }
    }
}
