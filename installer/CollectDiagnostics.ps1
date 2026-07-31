# Collects everything needed to diagnose a CrabRandomizer / UE4SS problem into one zip
# on the Desktop. Read-only: it copies files, changes nothing.
$ErrorActionPreference = "SilentlyContinue"

Write-Host "Collecting CrabRandomizer diagnostics..." -ForegroundColor Cyan

# --- find the game ---
$game = $null
foreach ($d in (Get-PSDrive -PSProvider FileSystem)) {
    foreach ($r in @(
        "Program Files (x86)\Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "SteamLibrary\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64",
        "Steam\steamapps\common\Crab Champions\CrabChampions\Binaries\Win64")) {
        $p = Join-Path $d.Root $r
        if (Test-Path (Join-Path $p "CrabChampions-Win64-Shipping.exe")) { $game = $p; break }
    }
    if ($game) { break }
}
if (-not $game) {
    Write-Host "Could not find Crab Champions. Edit this script and set `$game manually." -ForegroundColor Red
    Read-Host "Press Enter"; exit 1
}
Write-Host "Game: $game" -ForegroundColor Green

$out = Join-Path $env:TEMP "CrabDiag"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $out -Force | Out-Null

$scripts = Join-Path $game "Mods\CrabRandomizer\Scripts"

# --- summary ---
$sum = New-Object System.Collections.Generic.List[string]
$sum.Add("CrabRandomizer diagnostics  -  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$sum.Add("game dir : $game")

$mainLua = Join-Path $scripts "main.lua"
if (Test-Path $mainLua) {
    $m = Select-String -Path $mainLua -Pattern 'local MOD_VERSION = "([^"]+)"'
    $sum.Add("mod ver  : " + $(if ($m) { $m.Matches[0].Groups[1].Value } else { "unknown" }))
} else {
    $sum.Add("mod ver  : NOT INSTALLED (no main.lua at $scripts)")
}

# UE4SS must sit beside the exe or nothing loads at all - the single most common problem.
$sum.Add("UE4SS.dll beside exe : " + (Test-Path (Join-Path $game "UE4SS.dll")))
$sum.Add("dwmapi.dll beside exe: " + (Test-Path (Join-Path $game "dwmapi.dll")))

$modsTxt = Join-Path $game "Mods\mods.txt"
if (Test-Path $modsTxt) {
    $line = (Select-String -Path $modsTxt -Pattern 'CrabRandomizer').Line
    $sum.Add("mods.txt entry : " + $(if ($line) { $line.Trim() } else { "MISSING - mod will not load" }))
}
$sum.Add("game exe size  : " + (Get-Item (Join-Path $game "CrabChampions-Win64-Shipping.exe")).Length)
$sum | Set-Content (Join-Path $out "SUMMARY.txt") -Encoding utf8

# --- files ---
$copied = 0
foreach ($f in @(
    (Join-Path $scripts "crabrandomizer.log"),
    (Join-Path $scripts "crabrandomizer-dump.txt"),
    (Join-Path $scripts "randoconfig.txt"),
    (Join-Path $game "Mods\mods.txt"),
    (Join-Path $game "UE4SS-settings.ini"),
    (Join-Path $game "UE4SS.log"))) {
    if (Test-Path $f) {
        Copy-Item $f $out -Force
        Write-Host "  + $(Split-Path $f -Leaf)" -ForegroundColor Green
        $copied++
    } else {
        Write-Host "  - missing: $(Split-Path $f -Leaf)" -ForegroundColor DarkYellow
    }
}

# --- newest crash dumps (UE writes these under LocalAppData) ---
$crashRoot = Join-Path $env:LOCALAPPDATA "CrabChampions\Saved\Crashes"
$dumps = Get-ChildItem -Path $crashRoot -Recurse -Include *.dmp,*.log -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 4
foreach ($d in $dumps) {
    Copy-Item $d.FullName (Join-Path $out ("crash_" + $d.Name)) -Force
    Write-Host "  + crash_$($d.Name)" -ForegroundColor Green
    $copied++
}
if (-not $dumps) { Write-Host "  - no crash dumps under $crashRoot" -ForegroundColor DarkYellow }

# --- zip to Desktop ---
$zip = Join-Path ([Environment]::GetFolderPath("Desktop")) "CrabRandomizer-diagnostics.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip
Remove-Item $out -Recurse -Force

Write-Host ""
Write-Host "Done - $copied file(s) collected." -ForegroundColor Cyan
Write-Host "Send this file: $zip" -ForegroundColor Yellow
Read-Host "Press Enter to close"
