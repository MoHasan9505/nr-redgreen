# Finds the Cyberpunk 2077 folder for the other scripts. Dot-source it, then call Resolve-GameDir $GameDir.
# Order: the -GameDir argument, then the NRB_GAME_DIR environment variable, then every Steam library
# listed in Steam's libraryfolders.vdf.
function Resolve-GameDir([string]$GameDir) {
    $explicit = if ($GameDir) { $GameDir } elseif ($env:NRB_GAME_DIR) { $env:NRB_GAME_DIR } else { $null }
    if ($explicit) {
        if (-not (Test-Path (Join-Path $explicit 'bin\x64\Cyberpunk2077.exe'))) { throw "Cyberpunk2077.exe not found in $explicit\bin\x64" }
        return (Resolve-Path $explicit).Path
    }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches |
                ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libs | Select-Object -Unique) {
            $dir = Join-Path $lib 'steamapps\common\Cyberpunk 2077'
            if (Test-Path (Join-Path $dir 'bin\x64\Cyberpunk2077.exe')) { return (Resolve-Path $dir).Path }
        }
    }
    throw 'Cyberpunk 2077 was not found in any Steam library. Pass -GameDir "<game folder>" or set NRB_GAME_DIR.'
}
