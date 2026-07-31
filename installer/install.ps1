$ErrorActionPreference = "Stop"

Write-Host "CrabRandomizer installer" -ForegroundColor Cyan
Write-Host "-------------------------" -ForegroundColor Cyan

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$modsSource = Join-Path $scriptDir "Mods\CrabRandomizer"

if (-not (Test-Path $modsSource)) {
    Write-Host "Could not find Mods\CrabRandomizer next to this script - did you extract the whole zip?" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

function Find-CrabChampionsWin64 {
    $candidates = @()
    $relPaths = @(
        "Program Files (x86)\Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "SteamLibrary\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64"
    )
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        foreach ($rel in $relPaths) {
            $candidate = Join-Path $drive.Root $rel
            if ((Test-Path $candidate) -and ($candidates -notcontains $candidate)) {
                $candidates += $candidate
            }
        }
    }
    return $candidates
}

# Parses "key=value" lines, ignoring ';' comments and blanks. Returns an ordered
# hashtable so we can tell which keys a user's existing config already has.
function Read-ConfigKeys($path) {
    $map = [ordered]@{}
    if (-not (Test-Path $path)) { return $map }
    foreach ($line in (Get-Content $path)) {
        $clean = ($line -replace ';.*$', '').Trim()
        if ($clean -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') {
            $map[$matches[1]] = $matches[2].Trim()
        }
    }
    return $map
}

Write-Host "Searching for a Crab Champions install..."
$found = Find-CrabChampionsWin64
$target = $null

if ($found.Count -eq 1) {
    $target = $found[0]
    Write-Host "Found: $target" -ForegroundColor Green
} elseif ($found.Count -gt 1) {
    Write-Host "Found more than one install:"
    for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "  [$i] $($found[$i])" }
    $idx = Read-Host "Enter the number to use"
    $target = $found[[int]$idx]
} else {
    Write-Host "Could not auto-detect it. Paste the full path to your ...\Binaries\Win64 folder below," -ForegroundColor Yellow
    Write-Host "(right-click the folder in Explorer, Copy as path, then paste and press Enter):"
    $target = (Read-Host "Path").Trim('"')
}

if (-not (Test-Path $target)) {
    Write-Host "That path does not exist: $target" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

if (-not (Test-Path (Join-Path $target "UE4SS.dll"))) {
    Write-Host ""
    Write-Host "WARNING: UE4SS.dll was not found directly inside:" -ForegroundColor Yellow
    Write-Host "  $target" -ForegroundColor Yellow
    Write-Host "UE4SS itself has to sit right next to the game's .exe (not in a subfolder) or it" -ForegroundColor Yellow
    Write-Host "never loads, and this mod won't do anything even if installed correctly. Fix that" -ForegroundColor Yellow
    Write-Host "first if you haven't already: https://github.com/UE4SS-RE/RE-UE4SS/releases" -ForegroundColor Yellow
    Write-Host ""
}

$destMods = Join-Path $target "Mods"
$destMod = Join-Path $destMods "CrabRandomizer"
$destConfig = Join-Path $destMod "Scripts\randoconfig.txt"
$newConfigSource = Join-Path $modsSource "Scripts\randoconfig.txt"

if (-not (Test-Path $destMods)) {
    Write-Host "No Mods folder found at $destMods - creating it." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $destMods -Force | Out-Null
}

# Preserve the user's tuned settings across upgrades. Previous installer versions
# deleted the whole mod folder, silently reverting every customized setting.
$existingConfig = [ordered]@{}
$hadExistingConfig = Test-Path $destConfig
if ($hadExistingConfig) {
    $existingConfig = Read-ConfigKeys $destConfig
    Write-Host "Found an existing config with $($existingConfig.Count) setting(s) - these will be kept." -ForegroundColor Green
    $backup = "$destConfig.bak"
    Copy-Item $destConfig $backup -Force
    Write-Host "Backed it up to: $backup" -ForegroundColor Green
}

if (Test-Path $destMod) {
    Remove-Item $destMod -Recurse -Force
}
Copy-Item -Path $modsSource -Destination $destMods -Recurse -Force
Write-Host "Installed mod files to: $destMod" -ForegroundColor Green

if ($hadExistingConfig) {
    # Rebuild the config from the NEW template (so fresh keys and their comments land),
    # substituting the user's previous value wherever they had already set that key.
    $newLines = Get-Content $newConfigSource
    $merged = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($line in $newLines) {
        $clean = ($line -replace ';.*$', '').Trim()
        if ($clean -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') {
            $key = $matches[1]
            $seen[$key] = $true
            if ($existingConfig.Contains($key)) {
                $merged.Add("$key=$($existingConfig[$key])")
            } else {
                $merged.Add($line)
                Write-Host "  new setting added with its default: $key" -ForegroundColor Cyan
            }
        } else {
            $merged.Add($line)
        }
    }
    # Carry over anything the user had that the new template no longer ships, so a
    # hand-added or renamed key is never silently dropped.
    foreach ($key in $existingConfig.Keys) {
        if (-not $seen.ContainsKey($key)) {
            $merged.Add("$key=$($existingConfig[$key])")
            Write-Host "  kept your setting not in this version's template: $key" -ForegroundColor Cyan
        }
    }
    Set-Content -Path $destConfig -Value $merged -Encoding utf8
    Write-Host "Merged your existing settings into the updated config." -ForegroundColor Green
}

$modsTxt = Join-Path $destMods "mods.txt"
if (Test-Path $modsTxt) {
    $content = Get-Content $modsTxt -Raw
    if ($content -notmatch "(?m)^\s*CrabRandomizer\s*:") {
        Add-Content -Path $modsTxt -Value "`r`nCrabRandomizer : 1"
        Write-Host "Added 'CrabRandomizer : 1' to mods.txt" -ForegroundColor Green
    } elseif ($content -match "(?m)^\s*CrabRandomizer\s*:\s*0") {
        $content = $content -replace "(?m)^\s*CrabRandomizer\s*:\s*0", "CrabRandomizer : 1"
        Set-Content -Path $modsTxt -Value $content
        Write-Host "mods.txt listed CrabRandomizer as disabled - enabled it." -ForegroundColor Green
    } else {
        Write-Host "mods.txt already has CrabRandomizer enabled." -ForegroundColor Green
    }
} else {
    Write-Host "No mods.txt found at $modsTxt yet." -ForegroundColor Yellow
    Write-Host "Launch the game once with UE4SS installed so it generates one, then add this line to it:" -ForegroundColor Yellow
    Write-Host "  CrabRandomizer : 1" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Done. Launch the game, press F10 to open the UE4SS console, and confirm you see:" -ForegroundColor Cyan
Write-Host "  [CrabRandomizer] v1.4.1 loaded." -ForegroundColor Cyan
Write-Host "Then try 'randomizepreset default' and 'randomizeauthority' in that console." -ForegroundColor Cyan
Read-Host "Press Enter to close"
