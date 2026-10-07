# Verify-NRCheck.ps1 - MGPU Bridge DLSS-NR check
#
# Put this script, nrcheck.exe and nvngx.dll_nrcheck.dll in the game folder,
# beside the mgpu\ folder (the same folder as the add-on). Close the game.
# Right-click this file > Run with PowerShell.
#
# It tests whether your driver and your nvngx_dlssnr.dll can start DLSS-NR on
# each of your GPUs, with no game running, and writes nrcheck_report.txt.

param([int]$Synthetic = 0)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$exe = Join-Path $here 'nrcheck.exe'
$dll = Join-Path $here 'nvngx.dll_nrcheck.dll'
foreach ($f in @($exe, $dll)) {
    if (-not (Test-Path $f)) {
        Write-Host "Missing: $f" -ForegroundColor Red
        Write-Host "Keep nrcheck.exe, nvngx.dll_nrcheck.dll and this script together."
        Read-Host 'Press Enter to close'
        exit 1
    }
}

$snippet = $null
foreach ($p in @((Join-Path $here 'mgpu\nvngx_dlssnr.dll'), (Join-Path $here 'nvngx_dlssnr.dll'))) {
    if (Test-Path $p) { $snippet = $p; break }
}
if (-not $snippet) {
    Write-Host 'nvngx_dlssnr.dll not found in mgpu\ or in this folder.' -ForegroundColor Red
    Write-Host 'Run this from the game folder where the add-on is installed.'
    Read-Host 'Press Enter to close'
    exit 1
}

& $exe --snippet $snippet --synthetic $Synthetic

$report = Join-Path $here 'nrcheck_report.txt'
$hash = (Get-FileHash -Algorithm SHA256 $snippet).Hash
Add-Content $report ''
Add-Content $report "nvngx_dlssnr.dll SHA-256: $hash"
if (Test-Path (Join-Path $here 'nvngx_dlssnr.dll')) {
    if ($snippet -ne (Join-Path $here 'nvngx_dlssnr.dll')) {
        Add-Content $report 'NOTE: a second nvngx_dlssnr.dll is also beside the game exe.'
    }
}
$os = (Get-CimInstance Win32_OperatingSystem)
Add-Content $report "Windows: $($os.Caption) $($os.Version)"

# One zip with the report and every per-case NGX log, so there is one file to attach.
$zip = Join-Path $here 'nrcheck_results.zip'
$files = @($report) + @(Get-ChildItem -Path $here -Filter 'nvngx*_start*_adapter*.log' | ForEach-Object { $_.FullName })
Compress-Archive -Path $files -DestinationPath $zip -Force

Write-Host ''
Write-Host "Results written to: $zip" -ForegroundColor Green
Write-Host 'Please attach nrcheck_results.zip to your GitHub issue.'
Read-Host 'Press Enter to close'
