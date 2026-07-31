<#
.SYNOPSIS
    Auto-updates CrabRandomizer from GitHub releases.

.DESCRIPTION
    Does the job an updater framework (Velopack et al) would normally do, except those
    manage a standalone APPLICATION - they replace an exe and relaunch a process. This
    mod is a Lua file read by UE4SS inside the game's process: there is no binary of ours
    to replace, and UE4SS's Lua sandbox has no networking API, so the mod cannot update
    itself from the inside. This script is the piece that can.

    Checks the latest GitHub release, compares it against the MOD_VERSION declared in the
    installed main.lua, and installs the new version if it is newer - preserving your
    randoconfig.txt settings.

    Needs no GitHub token: the repo is public.

.PARAMETER GameDir
    ...\CrabChampions\Binaries\Win64. Auto-detected if omitted.

.PARAMETER CheckOnly
    Report whether an update exists, change nothing. Exit code 10 = update available.

.PARAMETER Silent
    No prompts, minimal output. For scheduled tasks / launch scripts.

.EXAMPLE
    .\Update.ps1
.EXAMPLE
    .\Update.ps1 -Silent
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [switch]$CheckOnly,
    [switch]$Silent
)

$ErrorActionPreference = "Stop"
$Repo = "dreman1166-maker/CrabRandomizer"

function Say($msg, $color = "Gray") { if (-not $Silent) { Write-Host $msg -ForegroundColor $color } }
function Fail($msg) { Write-Host $msg -ForegroundColor Red; if (-not $Silent) { Read-Host "Press Enter to close" }; exit 1 }

Say "CrabRandomizer updater" "Cyan"
Say "----------------------" "Cyan"

# ---------- locate the install ----------
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
    Fail "Could not find your Crab Champions Win64 folder. Re-run with:  .\Update.ps1 -GameDir `"<path>`""
}
Say "Game folder : $GameDir"

$modDir = Join-Path $GameDir "Mods\CrabRandomizer"
$mainLua = Join-Path $modDir "Scripts\main.lua"
$configPath = Join-Path $modDir "Scripts\randoconfig.txt"

# ---------- installed version ----------
$installed = $null
if (Test-Path $mainLua) {
    $m = Select-String -Path $mainLua -Pattern 'local MOD_VERSION = "([^"]+)"'
    if ($m) { $installed = $m.Matches[0].Groups[1].Value }
}
Say ("Installed   : " + $(if ($installed) { "v$installed" } else { "not installed" }))

# ---------- latest release ----------
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" `
                             -Headers @{ "User-Agent" = "CrabRandomizer-Updater" } -TimeoutSec 20
} catch {
    Fail "Could not reach GitHub: $($_.Exception.Message)"
}

$latest = $rel.tag_name -replace '^v', ''
Say "Latest      : v$latest"

function Compare-Version($a, $b) {
    # -1 a<b, 0 equal, 1 a>b. Pads so 1.4 vs 1.4.4 compares sanely.
    $x = @($a -split '\.' | ForEach-Object { [int]($_ -replace '\D', '') })
    $y = @($b -split '\.' | ForEach-Object { [int]($_ -replace '\D', '') })
    $n = [Math]::Max($x.Count, $y.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $xi = if ($i -lt $x.Count) { $x[$i] } else { 0 }
        $yi = if ($i -lt $y.Count) { $y[$i] } else { 0 }
        if ($xi -lt $yi) { return -1 }
        if ($xi -gt $yi) { return 1 }
    }
    return 0
}

if ($installed -and (Compare-Version $installed $latest) -ge 0) {
    Say "Already up to date." "Green"
    if (-not $Silent -and -not $CheckOnly) { Read-Host "Press Enter to close" }
    exit 0
}

Say ""
Say "Update available: v$latest" "Yellow"
if ($CheckOnly) { exit 10 }

# ---------- don't fight the running game for file locks ----------
if (Get-Process -Name "CrabChampions-Win64-Shipping" -ErrorAction SilentlyContinue) {
    Fail "Crab Champions is running. Close it first, then run this again."
}

# Prefer the installer-bearing asset; fall back to the Nexus (Lua-only) one.
$asset = $rel.assets | Where-Object { $_.name -like "*.zip" -and $_.name -notlike "*-nexus*" } | Select-Object -First 1
if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1 }
if (-not $asset) { Fail "That release has no .zip asset." }

$tmp = Join-Path $env:TEMP ("CrabRandomizer-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
$zipPath = Join-Path $tmp $asset.name

try {
    Say "Downloading $($asset.name)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath `
                      -Headers @{ "User-Agent" = "CrabRandomizer-Updater" } -TimeoutSec 120
    Expand-Archive -Path $zipPath -DestinationPath $tmp -Force

    $newMod = Join-Path $tmp "Mods\CrabRandomizer"
    if (-not (Test-Path $newMod)) { Fail "Downloaded archive is not laid out as expected." }

    # Preserve settings: keep every value the user already had, add new keys at default.
    $existing = [ordered]@{}
    if (Test-Path $configPath) {
        foreach ($line in (Get-Content $configPath)) {
            $clean = ($line -replace ';.*$', '').Trim()
            if ($clean -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') { $existing[$matches[1]] = $matches[2].Trim() }
        }
        Copy-Item $configPath "$configPath.bak" -Force
        Say "Kept $($existing.Count) existing setting(s); backup at randoconfig.txt.bak" "Green"
    }

    $destMods = Join-Path $GameDir "Mods"
    New-Item -ItemType Directory -Path $destMods -Force | Out-Null
    if (Test-Path $modDir) { Remove-Item $modDir -Recurse -Force }
    Copy-Item $newMod $destMods -Recurse -Force

    if ($existing.Count -gt 0) {
        $merged = New-Object System.Collections.Generic.List[string]
        $seen = @{}
        foreach ($line in (Get-Content $configPath)) {
            $clean = ($line -replace ';.*$', '').Trim()
            if ($clean -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') {
                $k = $matches[1]; $seen[$k] = $true
                if ($existing.Contains($k)) { $merged.Add("$k=$($existing[$k])") }
                else { $merged.Add($line); Say "  new setting: $k" "Cyan" }
            } else { $merged.Add($line) }
        }
        foreach ($k in $existing.Keys) { if (-not $seen.ContainsKey($k)) { $merged.Add("$k=$($existing[$k])") } }
        Set-Content -Path $configPath -Value $merged -Encoding utf8
    }

    # keep it enabled
    $modsTxt = Join-Path $destMods "mods.txt"
    if (Test-Path $modsTxt) {
        $c = Get-Content $modsTxt -Raw
        if ($c -notmatch "(?m)^\s*CrabRandomizer\s*:") {
            Add-Content -Path $modsTxt -Value "`r`nCrabRandomizer : 1"
        } elseif ($c -match "(?m)^\s*CrabRandomizer\s*:\s*0") {
            Set-Content -Path $modsTxt -Value ($c -replace "(?m)^\s*CrabRandomizer\s*:\s*0", "CrabRandomizer : 1")
        }
    }

    $now = (Select-String -Path $mainLua -Pattern 'local MOD_VERSION = "([^"]+)"').Matches[0].Groups[1].Value
    Say ""
    Say "Updated to v$now" "Green"
    if ($now -ne $latest) { Say "(warning: archive reported v$now but the release is tagged v$latest)" "Yellow" }
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not $Silent) { Read-Host "Press Enter to close" }
