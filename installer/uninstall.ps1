$ErrorActionPreference = "Stop"

Write-Host "CrabRandomizer uninstaller" -ForegroundColor Cyan
Write-Host "---------------------------" -ForegroundColor Cyan

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

$found = Find-CrabChampionsWin64
$target = $null
if ($found.Count -eq 1) {
    $target = $found[0]
    Write-Host "Found: $target" -ForegroundColor Green
} elseif ($found.Count -gt 1) {
    Write-Host "Found more than one install:"
    for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "  [$i] $($found[$i])" }
    $target = $found[[int](Read-Host "Enter the number to use")]
} else {
    Write-Host "Could not auto-detect it. Paste the full path to your ...\Binaries\Win64 folder:" -ForegroundColor Yellow
    $target = (Read-Host "Path").Trim('"')
}

$destMod = Join-Path $target "Mods\CrabRandomizer"
$modsTxt = Join-Path $target "Mods\mods.txt"

if (-not (Test-Path $destMod)) {
    Write-Host "CrabRandomizer is not installed at $destMod - nothing to remove." -ForegroundColor Yellow
} else {
    # Keep the config: reinstalling later should not lose tuned settings, and it is a
    # tiny text file. Anyone who wants it truly gone can delete the .bak beside it.
    $cfg = Join-Path $destMod "Scripts\randoconfig.txt"
    if (Test-Path $cfg) {
        $keep = Join-Path $target "Mods\crabrandomizer-randoconfig-backup.txt"
        Copy-Item $cfg $keep -Force
        Write-Host "Saved a copy of your config to: $keep" -ForegroundColor Green
    }
    Remove-Item $destMod -Recurse -Force
    Write-Host "Removed $destMod" -ForegroundColor Green
}

if (Test-Path $modsTxt) {
    $lines = Get-Content $modsTxt
    $filtered = $lines | Where-Object { $_ -notmatch '^\s*CrabRandomizer\s*:' }
    if ($filtered.Count -ne $lines.Count) {
        Set-Content -Path $modsTxt -Value $filtered -Encoding utf8
        Write-Host "Removed the CrabRandomizer line from mods.txt" -ForegroundColor Green
    } else {
        Write-Host "No CrabRandomizer line found in mods.txt" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Uninstalled. UE4SS and any other mods were left untouched." -ForegroundColor Cyan
Read-Host "Press Enter to close"
