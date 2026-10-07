# Installs the nr-bridge add-on into Cyberpunk 2077 next to its ReShade, or removes it again.
#   tools\deploy.ps1               # install (or update) the current build
#   tools\deploy.ps1 -Uninstall    # remove nr-bridge and put back everything install moved aside
#   tools\deploy.ps1 -Disable      # keep everything installed but stop ReShade loading the add-on
#   tools\deploy.ps1 -Enable       # undo -Disable
#   tools\deploy.ps1 -UseRTInitFix # install RTInitFix's sl.interposer.dll instead of ours (sl-standin)
#
# Install moves conflicting files into bin\x64\_nr-bridge-backup\<time>\ instead of deleting them:
#   - other neural-rendering add-ons (RenoDX DLSS 5 and its overlay): one neural path at a time
#   - nvngx_dlssnr.dll beside the exe: the game's Streamline would bind it to the wrong GPU
# Uninstall moves them back.
param(
    [string]$GameDir = '',  # default: NRB_GAME_DIR, then the Steam libraries (tools\game-dir.ps1)
    [switch]$Uninstall,
    [switch]$Disable,
    [switch]$Enable,
    [switch]$UseRTInitFix
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'game-dir.ps1')
$GameDir = Resolve-GameDir $GameDir
$bin = Join-Path $GameDir 'bin\x64'
if (-not (Test-Path (Join-Path $bin 'Cyberpunk2077.exe'))) { throw "Cyberpunk2077.exe not found in $bin" }
if (Get-Process -Name 'Cyberpunk2077' -ErrorAction SilentlyContinue) { throw 'Cyberpunk 2077 is running - quit it first.' }

$backupRoot = Join-Path $bin '_nr-bridge-backup'
$marker = Join-Path $backupRoot 'installed.json'
$conflictPatterns = @('renodx-dlss5*.addon64', 'renodx-dlss.addon64', 'dlss5-lab-overlay-*.addon64', 'nvngx_dlssnr.dll')
$ours = @(
    'nvngx.dll_mgpu_bridge.addon64',   # must contain "nvngx.dll": the DLSS-NR snippet's caller gate
    'mgpu.ini', 'gpu1.ini', 'ReShade2.ini',
    'mgpu\nvngx_dlssnr.dll',
    'reshade-shaders\Shaders\mgpu_depth_tap.fx',
    'nvapi64.dll',                     # nvapi-gate: hides NVAPI from game-side callers, passes ours through
    'sl.interposer.dll',               # sl-standin (ours): Streamline off, NVIDIA hidden from the game, DXGI through ReShade
    'sl-standin.ini',                  # its settings (HideNvidia, MaskDXR); kept across redeploys
    'GFSDK_Aftermath_Lib.x64.dll'      # RTInitFix: NVIDIA Aftermath stand-in
)
# Game files the RT-init fix replaces. The game's originals are moved aside (not deleted) the first
# time, and -Uninstall puts them back. Without this the game fails "Ray Tracing initialization" on
# the R9700 whenever an NVIDIA card is in the machine - see docs/rt-init-error.md.
$rtfix = Join-Path $repo 'vendor\rtinitfix'
# sl.interposer.dll: ours by default. RTInitFix's loads System32\dxgi.dll directly, so the game's
# swapchain bypasses ReShade (no overlay, bridge never arms - run8). -UseRTInitFix for an A/B.
$interposer = if ($UseRTInitFix) { Join-Path $rtfix 'sl.interposer.dll' } else { Join-Path $repo 'build\sl-standin\sl.interposer.dll' }
$replacements = [ordered]@{
    'sl.interposer.dll'           = $interposer
    'GFSDK_Aftermath_Lib.x64.dll' = (Join-Path $rtfix 'GFSDK_Aftermath_Lib.x64.dll')
}

function Read-Marker { if (Test-Path $marker) { Get-Content $marker -Raw | ConvertFrom-Json } else { $null } }

# ReShade only loads *.addon64, so a renamed add-on stays on disk but is never loaded.
$addonPath = Join-Path $bin 'nvngx.dll_mgpu_bridge.addon64'
if ($Disable) {
    if (Test-Path $addonPath) { Move-Item $addonPath "$addonPath.off" -Force }
    Write-Host "nr-bridge add-on disabled (renamed to .addon64.off)."
    return
}
if ($Enable) {
    if (Test-Path "$addonPath.off") { Move-Item "$addonPath.off" $addonPath -Force }
    Write-Host "nr-bridge add-on enabled."
    return
}

if ($Uninstall) {
    $m = Read-Marker
    if (-not $m) { throw "nr-bridge is not installed here (no $marker)" }
    foreach ($rel in $m.files) {
        $p = Join-Path $bin $rel
        if (Test-Path $p) { Remove-Item $p -Force; Write-Host "removed  $rel" }
    }
    Remove-Item "$addonPath.off" -Force -ErrorAction SilentlyContinue
    $mgpuDir = Join-Path $bin 'mgpu'
    if ((Test-Path $mgpuDir) -and -not (Get-ChildItem $mgpuDir -Force)) { Remove-Item $mgpuDir }
    foreach ($b in $m.backups) {
        $dir = Join-Path $backupRoot $b
        if (-not (Test-Path $dir)) { continue }
        Get-ChildItem $dir -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($dir.Length + 1)
            $to = Join-Path $bin $rel
            New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
            Move-Item $_.FullName $to -Force
            Write-Host "restored $rel"
        }
        Remove-Item $dir -Recurse -Force
    }
    Remove-Item $marker -Force
    if (-not (Get-ChildItem $backupRoot -Force)) { Remove-Item $backupRoot }
    Write-Host 'nr-bridge uninstalled.'
    return
}

# ---- install ----
$addon = Join-Path $repo 'build\bridge\mgpu_bridge.addon64'
$snippet = Join-Path $repo 'vendor\nvidia\nvngx_dlssnr.dll'
$gate = Join-Path $repo 'build\nvapi-gate\nvapi64.dll'
foreach ($f in @($addon, $snippet, $gate) + @($replacements.Values)) { if (-not (Test-Path $f)) { throw "missing $f (build with tools\build.ps1; RTInitFix DLLs go in vendor\rtinitfix)" } }
# A failed build can leave no DLL or a stale one: never install an sl-standin older than its source.
if (-not $UseRTInitFix) {
    $src = Get-ChildItem (Join-Path $repo 'tools\sl-standin') -File | Sort-Object LastWriteTime | Select-Object -Last 1
    if ((Get-Item $interposer).LastWriteTime -lt $src.LastWriteTime) {
        throw "build\sl-standin\sl.interposer.dll is older than tools\sl-standin\$($src.Name) - tools\build.ps1 sl-standin failed or was not run. Fix the build, or deploy with -UseRTInitFix."
    }
}

$m = Read-Marker
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $backupRoot $stamp
$moved = @()
function Move-Aside([string]$rel) {
    $from = Join-Path $bin $rel
    $to = Join-Path $backupDir $rel
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Move-Item $from $to
    Write-Host "moved aside $rel"
    $script:moved += $rel
}

# Conflicting neural paths and the snippet beside the exe.
foreach ($pat in $conflictPatterns) {
    Get-ChildItem $bin -File -Filter $pat | ForEach-Object { Move-Aside $_.Name }
}
# On a first install, keep any pre-existing file we are about to overwrite.
if (-not $m) {
    foreach ($rel in $ours) { if (Test-Path (Join-Path $bin $rel)) { Move-Aside $rel } }
}
# Game files the RT-init fix replaces: keep the game's original whenever what is there is not ours.
# "Ours" = the file being installed, RTInitFix's copy, or any sl-standin build (switching between
# them must not back one up as if it were the game's original, or -Uninstall would restore it).
function Test-Ours([string]$path, [string]$rel) {
    $h = (Get-FileHash $path).Hash
    if ($h -eq (Get-FileHash $replacements[$rel]).Hash) { return $true }
    $rt = Join-Path $rtfix $rel
    if ((Test-Path $rt) -and $h -eq (Get-FileHash $rt).Hash) { return $true }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('sl-standin (nr-bridge)')
}
foreach ($rel in $replacements.Keys) {
    $cur = Join-Path $bin $rel
    if ((Test-Path $cur) -and -not (Test-Ours $cur $rel)) { Move-Aside $rel }
}

# Our files.
$assets = Join-Path $repo 'bridge\assets'
New-Item -ItemType Directory -Force (Join-Path $bin 'mgpu'), (Join-Path $bin 'reshade-shaders\Shaders') | Out-Null
Copy-Item $addon $addonPath -Force
Remove-Item "$addonPath.off" -Force -ErrorAction SilentlyContinue
Copy-Item $snippet (Join-Path $bin 'mgpu\nvngx_dlssnr.dll') -Force
Copy-Item $gate (Join-Path $bin 'nvapi64.dll') -Force
foreach ($rel in $replacements.Keys) { Copy-Item $replacements[$rel] (Join-Path $bin $rel) -Force }
$slIni = Join-Path $bin 'sl-standin.ini'
if (-not (Test-Path $slIni)) { Copy-Item (Join-Path $repo 'tools\sl-standin\sl-standin.ini') $slIni }
Copy-Item (Join-Path $assets 'gpu1.ini') $bin -Force
Copy-Item (Join-Path $assets 'ReShade2.ini') $bin -Force
Copy-Item (Join-Path $assets 'reshade-shaders\Shaders\mgpu_depth_tap.fx') (Join-Path $bin 'reshade-shaders\Shaders') -Force

# mgpu.ini: upstream's file with the first-run profile for this rig applied on top.
$overrides = [ordered]@{
    'Depth'        = '3'   # NRB16: depth from FSR's upscale dispatch (run29: "looked much better"). 0 = colour only
    'MVec'         = '3'   # real motion vectors, from FSR's upscale dispatch (NRB10). Needs FSR 3.0 or FSR 4 selected in
                           # game: with no FSR the arm waits for a motion-vector source that never comes
    'Calib'        = '0'   # nothing to calibrate against without the game's own NGX
    'DcompOverlay' = '1'   # one monitor, on the neural card (the 5070); the render card is headless
    'PresentVsync' = '0'   # run20: V-sync on the bridge's own present cost ~15 fps (35 -> 50 on a 100 Hz panel)
    'ProducerCopyQueue' = '1'  # NRB12: the R9700's PCIe copy on its own copy queue, not the game's (run21: the
                               # game lost ~10 fps to it). Set 0 in bin\x64\mgpu.ini to compare
    'SignalAt'     = '2'   # NRB15: hand each frame to the 5070 from finish_present, one game frame sooner
    'AutoCap'      = '1'   # NRB17: pace the game so frames never queue on the 5070 (run29: +11.5 ms with depth)
    'GpuPipeline'  = '1'   # NRB14: the 5070 takes the next frame while the current one evaluates (run23: idle
                           # ~3.3 of 15.6 ms per frame). Set 0 in bin\x64\mgpu.ini to compare
}
$ini = Get-Content (Join-Path $assets 'mgpu.ini') -Raw
foreach ($k in $overrides.Keys) {
    if ($ini -notmatch "(?m)^$k=") { throw "mgpu.ini has no $k= key to override" }
    $ini = $ini -replace "(?m)^$k=.*$", "$k=$($overrides[$k])"
}
$header = "; nr-bridge first-run profile (tools\deploy.ps1): " + (($overrides.Keys | ForEach-Object { "$_=$($overrides[$_])" }) -join ' ') + "`r`n"
Set-Content -Path (Join-Path $bin 'mgpu.ini') -Value ($header + $ini) -NoNewline -Encoding ascii

$backups = @()
if ($m) { $backups = @($m.backups) }
if ($moved.Count -gt 0) { $backups += $stamp }
New-Item -ItemType Directory -Force $backupRoot | Out-Null
@{ installed = (Get-Date -Format s); files = $ours; backups = $backups } | ConvertTo-Json | Set-Content $marker -Encoding ascii

$h = (Get-FileHash (Join-Path $bin 'nvngx.dll_mgpu_bridge.addon64')).Hash.Substring(0, 12)
Write-Host "nr-bridge installed into $bin (add-on sha256 $h)."
Write-Host ("sl.interposer.dll: " + $(if ($UseRTInitFix) { "RTInitFix (ReShade cannot see the game's swapchain)" } else { 'sl-standin - settings in sl-standin.ini, log in sl-standin.log' }))
