# Starts Cyberpunk 2077 through Steam.
#   tools\launch.ps1
#   tools\launch.ps1 -Watch run8     # also run tools\watch-launch.ps1 with that label
#   tools\launch.ps1 -Direct         # start Cyberpunk2077.exe itself instead of through Steam
#
# Earlier versions closed the NVIDIA App and overlay and could restart the NVIDIA display service, on the
# theory that they caused NGX's intermittent FAIL_OutOfDate. The cause was our own 5-argument Init call
# (NRB11, docs/bridge-changes.md); none of that is needed. If the bridge's output ever fails to appear on
# screen with a [V49] line naming the topmost composition slot, another overlay holds that slot - closing
# the NVIDIA overlay is the first thing to try.
param(
    [string]$Watch,
    [switch]$Direct,
    [string]$SteamAppId = '1091500',
    [string]$GameDir = ''   # default: NRB_GAME_DIR, then the Steam libraries (tools\game-dir.ps1)
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'game-dir.ps1')
$GameDir = Resolve-GameDir $GameDir
$exe = Join-Path $GameDir 'bin\x64\Cyberpunk2077.exe'
if (Get-Process -Name 'Cyberpunk2077' -ErrorAction SilentlyContinue) { throw 'Cyberpunk 2077 is already running.' }

# Through Steam by default: started directly, the exe exits within seconds when Steam does not
# take over the launch (run9).
if ($Direct) { Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) }
else { Start-Process "steam://rungameid/$SteamAppId" }
Write-Host 'Cyberpunk 2077 starting.'
if ($Watch) { & (Join-Path $PSScriptRoot 'watch-launch.ps1') -Label $Watch -GameDir $GameDir }
