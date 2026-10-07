# Downloads the third-party headers nr-bridge builds against into third_party\ (git-ignored).
# Pinned to the same commits MGPU Bridge's CI uses, so behaviour matches upstream.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\.."
$dest = Join-Path $repo 'third_party'

$deps = @(
    @{ Name = 'ngx';     Repo = 'NVIDIA/DLSS';     Sha = 'a291cc7d2cc642a51566f3dfd5376f635cd1b284'; Sub = 'include'; Check = 'nvsdk_ngx.h' },
    @{ Name = 'reshade'; Repo = 'crosire/reshade'; Sha = '18deaa52de0c425a78b329e9cb3c497281cd00ec'; Sub = 'include'; Check = 'reshade.hpp' }
)

New-Item -ItemType Directory -Force $dest | Out-Null
$tmp = Join-Path $env:TEMP "nr-bridge-deps"
New-Item -ItemType Directory -Force $tmp | Out-Null

foreach ($d in $deps) {
    $out = Join-Path $dest $d.Name
    if (Test-Path (Join-Path $out $d.Check)) { Write-Host "$($d.Name): present"; continue }
    $tgz = Join-Path $tmp "$($d.Name).tar.gz"
    Write-Host "$($d.Name): downloading $($d.Repo)@$($d.Sha.Substring(0,8))"
    curl.exe -fsSL -o $tgz "https://codeload.github.com/$($d.Repo)/tar.gz/$($d.Sha)"
    if ($LASTEXITCODE -ne 0) { throw "download failed for $($d.Name)" }
    $x = Join-Path $tmp $d.Name
    Remove-Item -Recurse -Force $x -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $x | Out-Null
    tar -xzf $tgz -C $x
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $($d.Name)" }
    $inc = Get-ChildItem $x -Directory | Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName $d.Sub }
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    Move-Item $inc $out
    if (-not (Test-Path (Join-Path $out $d.Check))) { throw "$($d.Name): $($d.Check) missing after extract" }
    Write-Host "$($d.Name): ok -> $out"
}

# bridge\ keeps upstream's layout: it includes "../ext/ngx/nvsdk_ngx.h" and ext\reshade\reshade.hpp.
$bridgeExt = Join-Path $repo 'bridge\ext'
if (Test-Path (Join-Path $repo 'bridge')) {
    foreach ($d in $deps) {
        $to = Join-Path $bridgeExt $d.Name
        Remove-Item -Recurse -Force $to -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $bridgeExt | Out-Null
        Copy-Item -Recurse (Join-Path $dest $d.Name) $to
    }
    Write-Host "bridge\ext: mirrored"
}
