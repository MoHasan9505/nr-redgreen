# Builds bridge\ and the tools under tools\ with the newest installed MSVC (via vcvars64) and Ninja.
#   tools\build.ps1                 # everything
#   tools\build.ps1 bridge          # one target: bridge, nr-probe, xadapter-probe
# Output: build\<name>\ under the repo root.
param([string]$Tool = 'all')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\.."

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio / Build Tools install with the C++ x64 tools was found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# vcvars64.bat calls vswhere itself and expects it on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"

$tools = @(Get-Item (Join-Path $repo 'bridge') -ErrorAction SilentlyContinue) + @(Get-ChildItem $PSScriptRoot -Directory) |
    Where-Object { $_ -and (Test-Path (Join-Path $_.FullName 'CMakeLists.txt')) }
if ($Tool -ne 'all') { $tools = $tools | Where-Object Name -eq $Tool }
if (-not $tools) { throw "no tool named '$Tool' under tools\" }

foreach ($t in $tools) {
    $build = Join-Path $repo "build\$($t.Name)"
    Write-Host "== $($t.Name)"
    $ErrorActionPreference = 'Continue'
    cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$($t.FullName)`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul && cmake --build `"$build`" 2>&1"
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) { throw "build failed for $($t.Name) ($LASTEXITCODE)" }
}
