# Phase 4.1: which FSR (FidelityFX) entry points does Cyberpunk 2077 use, and how?
#   tools\ffx-scan.ps1                 # writes results\ffx-scan.txt and prints a summary
#
# The FFX tap (depth + motion vectors for the neural stage) must hook the upscale dispatch.
# Cyberpunk ships two FSR stacks in bin\x64:
#   - amd_fidelityfx_dx12.dll   FidelityFX API (FSR 3.1+/4): ffxCreateContext / ffxDispatch / ...
#   - ffx_fsr3upscaler_x64.dll  FSR 3.0-style API (CDPR-signed): ffxFsr3UpscalerContextDispatch / ...
# This lists what each FFX DLL exports and which modules in bin\x64 import from them statically.
# A static import is hooked by patching the importer's IAT; a module that imports nothing from
# them loads them with LoadLibrary/GetProcAddress, which needs a GetProcAddress hook instead.
# Uses dumpbin from the same Visual Studio install as tools\build.ps1.
param(
    [string]$GameDir = ''   # default: NRB_GAME_DIR, then the Steam libraries (tools\game-dir.ps1)
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'game-dir.ps1')
$GameDir = Resolve-GameDir $GameDir
$bin = Join-Path $GameDir 'bin\x64'
if (-not (Test-Path (Join-Path $bin 'Cyberpunk2077.exe'))) { throw "Cyberpunk2077.exe not found in $bin" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio / Build Tools install with the C++ x64 tools was found.' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
if (-not (Test-Path $dumpbin)) { throw "dumpbin.exe not found at $dumpbin" }

$out = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { $out.Add($s); Write-Host $s }

$ffxDlls = Get-ChildItem $bin -File -Filter '*.dll' | Where-Object { $_.Name -match '^(amd_fidelityfx|ffx_)' }
Say "== FFX DLLs in bin\x64"
foreach ($d in $ffxDlls) {
    $ver = (Get-Item $d.FullName).VersionInfo.FileVersion
    $sig = (Get-AuthenticodeSignature $d.FullName).SignerCertificate.Subject
    Say ("{0}  version {1}  signer {2}" -f $d.Name, $ver, $sig)
    $exports = & $dumpbin /nologo /exports $d.FullName | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    Say ("  exports ({0}): {1}" -f $exports.Count, ($exports -join ' '))
}

Say ""
Say "== Static imports from the FFX DLLs, by importing module"
$names = $ffxDlls | ForEach-Object { $_.Name.ToLowerInvariant() }
$importers = Get-ChildItem $bin -File | Where-Object { $_.Extension -in '.exe', '.dll' -and $_.Name -notmatch '^(amd_fidelityfx|ffx_)' }
$any = $false
foreach ($m in $importers) {
    $text = & $dumpbin /nologo /imports $m.FullName 2>$null
    $current = $null
    $hits = @{}
    foreach ($line in $text) {
        if ($line -match '^\s{4}(\S+\.dll)\s*$') { $current = $Matches[1].ToLowerInvariant(); continue }
        if ($current -and $names -contains $current -and $line -match '^\s+[0-9A-F]+\s+(\S+)\s*$') {
            if (-not $hits[$current]) { $hits[$current] = New-Object System.Collections.Generic.List[string] }
            $hits[$current].Add($Matches[1])
        }
    }
    foreach ($k in $hits.Keys) {
        $any = $true
        Say ("{0} imports from {1}: {2}" -f $m.Name, $k, ($hits[$k] -join ' '))
    }
}
if (-not $any) { Say "(none - the FFX DLLs are loaded at run time with LoadLibrary/GetProcAddress)" }

Say ""
Say "== Delay-loaded imports of Cyberpunk2077.exe (FFX-related lines only)"
$delay = & $dumpbin /nologo /imports (Join-Path $bin 'Cyberpunk2077.exe') | Select-String -SimpleMatch -Pattern 'ffx', 'fidelityfx'
if ($delay) { $delay | ForEach-Object { Say $_.Line } } else { Say "(no FFX names in the exe's import tables)" }

$dst = Join-Path $repo 'results\ffx-scan.txt'
New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
$out | Set-Content $dst -Encoding utf8
Write-Host ""
Write-Host "Saved to $dst"
