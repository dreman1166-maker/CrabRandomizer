<#
.SYNOPSIS
    Installs the CrabRandomizer overlay (clickable in-game menu) and updates the Lua mod.

.DESCRIPTION
    Does everything needed on a second machine:
      - updates the CrabRandomizer Lua mod to the latest release
      - downloads and installs the overlay DLL to Mods\CrabRandomizerOverlay\dlls\main.dll
      - adds "CrabRandomizerOverlay : 1" to mods.txt if it is not already there

    The overlay is a DLL, NOT a content .pak, so it does NOT have to match your co-op
    partner's mods and cannot cause the join failures that pak mismatches do.

.PARAMETER SkipLuaUpdate
    Only install the overlay; leave the Lua mod alone.

.PARAMETER Uninstall
    Remove the overlay and disable it in mods.txt. Leaves the Lua mod untouched.
#>
[CmdletBinding()]
param(
    [switch]$SkipLuaUpdate,
    [switch]$Uninstall,
    [string]$GameDir
)

$ErrorActionPreference = "Stop"
$Repo = "dreman1166-maker/CrabRandomizer"

function Say($m, $c = "Gray") { Write-Host $m -ForegroundColor $c }
function Die($m) { Write-Host $m -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

Say "CrabRandomizer overlay installer" "Cyan"
Say "-------------------------------" "Cyan"

function Find-GameDir {
    $rel = @(
        "Program Files (x86)\Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "SteamLibrary\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64"
    )
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        foreach ($r in $rel) {
            $c = Join-Path $d.Root $r
            if (Test-Path (Join-Path $c "CrabChampions-Win64-Shipping.exe")) { return $c }
        }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir -or -not (Test-Path $GameDir)) {
    Die "Could not find Crab Champions. Re-run with:  -GameDir `"<path to ...\Binaries\Win64>`""
}
Say "Game folder : $GameDir"

if (Get-Process -Name "CrabChampions-Win64-Shipping" -ErrorAction SilentlyContinue) {
    Die "Crab Champions is running. Close it first, then run this again."
}

$modsDir    = Join-Path $GameDir "Mods"
$overlayDir = Join-Path $modsDir "CrabRandomizerOverlay"
$modsTxt    = Join-Path $modsDir "mods.txt"

# ---------------- uninstall ----------------
if ($Uninstall) {
    if (Test-Path $overlayDir) { Remove-Item $overlayDir -Recurse -Force; Say "Removed $overlayDir" "Green" }
    if (Test-Path $modsTxt) {
        $c = Get-Content $modsTxt -Raw
        if ($c -match "(?m)^\s*CrabRandomizerOverlay\s*:\s*1") {
            Set-Content $modsTxt ($c -replace "(?m)^\s*CrabRandomizerOverlay\s*:\s*1", "CrabRandomizerOverlay : 0") -Encoding utf8
            Say "Disabled in mods.txt" "Green"
        }
    }
    Say "`nOverlay removed. The randomizer itself is untouched." "Cyan"
    Read-Host "Press Enter to close"; exit 0
}

# ---------------- Lua mod ----------------
if (-not $SkipLuaUpdate) {
    Say "`nUpdating the CrabRandomizer Lua mod..." "Yellow"
    try {
        $u = "https://raw.githubusercontent.com/$Repo/main/installer/Update.ps1"
        $script = Invoke-RestMethod -Uri $u -TimeoutSec 30
        $sb = [ScriptBlock]::Create($script)
        & $sb -GameDir $GameDir -Silent
        Say "Lua mod updated." "Green"
    } catch {
        Say "Lua update failed ($($_.Exception.Message)) - continuing with the overlay." "DarkYellow"
    }
}

# ---------------- overlay ----------------
Say "`nInstalling the overlay..." "Yellow"

$tmp = Join-Path $env:TEMP ("CrabOverlay-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    $url = "https://github.com/$Repo/releases/download/overlay-v1/CrabRandomizerOverlay.zip"
    $zip = Join-Path $tmp "overlay.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip -TimeoutSec 120
    Expand-Archive -Path $zip -DestinationPath $tmp -Force

    $dll = Get-ChildItem $tmp -Recurse -Filter "main.dll" | Select-Object -First 1
    if (-not $dll) { Die "The downloaded archive contained no main.dll" }

    $dest = Join-Path $overlayDir "dlls"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    Copy-Item $dll.FullName (Join-Path $dest "main.dll") -Force
    Say "Installed $($dll.Length) bytes to $dest\main.dll" "Green"
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------- enable ----------------
if (-not (Test-Path $modsTxt)) { Die "No mods.txt at $modsTxt - is UE4SS installed?" }

$c = Get-Content $modsTxt -Raw
if ($c -notmatch "(?m)^\s*CrabRandomizerOverlay\s*:") {
    Add-Content -Path $modsTxt -Value "`r`nCrabRandomizerOverlay : 1"
    Say "Added CrabRandomizerOverlay : 1 to mods.txt" "Green"
} elseif ($c -match "(?m)^\s*CrabRandomizerOverlay\s*:\s*0") {
    Set-Content $modsTxt ($c -replace "(?m)^\s*CrabRandomizerOverlay\s*:\s*0", "CrabRandomizerOverlay : 1") -Encoding utf8
    Say "Enabled in mods.txt" "Green"
} else {
    Say "Already enabled in mods.txt" "Gray"
}

# a stale log from a previous run makes diagnosis confusing
Remove-Item (Join-Path $GameDir "CrabOverlay.log") -Force -ErrorAction SilentlyContinue

Say ""
Say "Done. Launch the game and press F8  (Insert also works)." "Cyan"
Say ""
Say "If nothing appears, send back this file:" "Gray"
Say "  $GameDir\CrabOverlay.log" "Gray"
Say ""
Say "To remove the overlay later:  .\InstallOverlay.ps1 -Uninstall" "DarkGray"
Read-Host "Press Enter to close"
