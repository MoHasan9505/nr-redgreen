# Watches one Cyberpunk 2077 launch: when it starts, any error dialog it shows (with the text and
# how long after start), and when it exits. Saves ReShade.log to results\ under the given label.
#   tools\watch-launch.ps1 -Label run3-addon-off
param(
    [Parameter(Mandatory)] [string]$Label,
    [string]$GameDir = '',  # default: NRB_GAME_DIR, then the Steam libraries (tools\game-dir.ps1)
    [int]$StartTimeoutMinutes = 45,
    [int]$Launches = 1   # watch this many launches back to back; each gets "<Label>-<n>"
)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$repo = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'game-dir.ps1')
$GameDir = Resolve-GameDir $GameDir
$bin = Join-Path $GameDir 'bin\x64'

function Get-DialogText([int]$procId) {
    try { return Find-DialogText $procId } catch { return $null }
}

function Find-DialogText([int]$procId) {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $procId)
    foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
        if ($w.Current.Name -ne 'Error') { continue }
        $texts = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition) |
            ForEach-Object { $_.Current.Name } | Where-Object { $_ -and $_.Length -gt 3 }
        return ($texts -join ' ')
    }
    return $null
}

$base = $Label
$lastPid = 0
for ($n = 1; $n -le $Launches; $n++) {
$Label = if ($Launches -gt 1) { "$base-$n" } else { $base }
$deadline = (Get-Date).AddMinutes($StartTimeoutMinutes)
$p = $null
while (-not ($p = Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue | Where-Object Id -ne $lastPid | Select-Object -First 1)) {
    if ((Get-Date) -gt $deadline) { "[$Label] game never started within $StartTimeoutMinutes min"; exit 0 }
    Start-Sleep -Milliseconds 500
}
$start = Get-Date
"[$Label] Cyberpunk started $($start.ToString('HH:mm:ss')) (pid $($p.Id))"
$addons = (Get-ChildItem $bin -Filter '*.addon64' | ForEach-Object Name) -join ', '
"[$Label] add-ons loadable at launch: " + $(if ($addons) { $addons } else { '(none)' })
"[$Label] ReShade present: " + (Test-Path (Join-Path $bin 'dxgi.dll'))

$errorSeen = $false
while (-not $p.HasExited) {
    if (-not $errorSeen) {
        $t = Get-DialogText $p.Id
        if ($t) { $errorSeen = $true; "[$Label] ERROR DIALOG at +{0:N1}s: {1}" -f ((Get-Date) - $start).TotalSeconds, $t }
    }
    Start-Sleep -Milliseconds 500
}
# A launcher stub that exits within seconds is usually handing over to a new game process (Steam
# relaunch). Follow that one instead of reporting a 3-second "run" (run9).
if (((Get-Date) - $start).TotalSeconds -lt 20) {
    $handoff = (Get-Date).AddSeconds(30)
    $next = $null
    while (-not $next -and (Get-Date) -lt $handoff) {
        $next = Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue | Where-Object Id -ne $p.Id | Select-Object -First 1
        if (-not $next) { Start-Sleep -Milliseconds 500 }
    }
    if ($next) {
        "[$Label] pid $($p.Id) exited after {0:N1}s and pid $($next.Id) started - following it" -f ((Get-Date) - $start).TotalSeconds
        $p = $next
        while (-not $p.HasExited) {
            if (-not $errorSeen) {
                $t = Get-DialogText $p.Id
                if ($t) { $errorSeen = $true; "[$Label] ERROR DIALOG at +{0:N1}s: {1}" -f ((Get-Date) - $start).TotalSeconds, $t }
            }
            Start-Sleep -Milliseconds 500
        }
    } else {
        "[$Label] GAME DID NOT START: the process exited after {0:N1}s and no new one appeared within 30 s" -f ((Get-Date) - $start).TotalSeconds
    }
}
$end = Get-Date
$lastPid = $p.Id
"[$Label] exited $($end.ToString('HH:mm:ss')) after {0:N1} min; error dialog seen: {1}" -f ($end - $start).TotalMinutes, $(if ($errorSeen) { 'YES' } else { 'no' })
# The game writes CrashInfo.json when it dies on an error, even if the dialog was missed.
$crashInfo = "$env:LOCALAPPDATA\CD Projekt Red\Cyberpunk 2077\CrashInfo.json"
if ((Test-Path $crashInfo) -and (Get-Item $crashInfo).LastWriteTime -gt $start) {
    $pm = (Get-Content $crashInfo -Raw | ConvertFrom-Json).Data.postMortem
    "[$Label] CRASH recorded by the game: timeCrash $($pm.timeCrash) (UTC), sessionLength $($pm.sessionLength) min"
} else {
    "[$Label] no crash recorded by the game"
}
$log = Join-Path $bin 'ReShade.log'
if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -lt $start) {
    "[$Label] ReShade.log was not written by this launch (last written $((Get-Item $log).LastWriteTime.ToString('HH:mm:ss'))) - ReShade never loaded; no verdict"
} elseif (Test-Path $log) {
    $dst = Join-Path $repo ("results\{0}_{1}_ReShade.log" -f (Get-Date -Format 'yyyy-MM-dd_HHmm'), $Label)
    Copy-Item $log $dst -Force
    "[$Label] ReShade.log saved to $dst"
    $slLog = Join-Path $bin 'sl-standin.log'
    if ((Test-Path $slLog) -and (Get-Item $slLog).LastWriteTime -gt $start) {
        $slDst = Join-Path $repo ("results\{0}_{1}_sl-standin.log" -f (Get-Date -Format 'yyyy-MM-dd_HHmm'), $Label)
        Copy-Item $slLog $slDst -Force
        "[$Label] sl-standin.log saved to $slDst"
    }
    # One-line verdict on the neural stage, from the bridge's own log lines.
    $txt = Get-Content $dst
    $init = $txt | Select-String '\[P4\.1\] Init: result=(0x[0-9A-F]+)' | Select-Object -Last 1
    $first = $txt | Select-String '\[V53\] first neural frame' | Select-Object -First 1
    $verdict = if ($first) { 'NEURAL FRAMES ON SCREEN' } elseif ($txt | Select-String '\[NRB2\]') { 'NGX refused (FAIL_OutOfDate) - transport only' } elseif ($init) { "NGX Init $($init.Matches[0].Groups[1].Value), no neural frame" } else { 'stream never reached NGX' }
    "[$Label] neural stage: $verdict"
}
}
