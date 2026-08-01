<#
.SYNOPSIS
    Temporarily disables (or restores) every .pak content mod, to test co-op joins.

.DESCRIPTION
    In Unreal co-op, .pak content mods that add or alter gameplay actors have to match on
    BOTH machines. When the host spawns an actor whose class the joining client does not
    have, the client cannot resolve it and gets dropped - which looks exactly like
    "<player> has joined the game" immediately followed by "<player> has left the game".

    This is the single most common cause of a co-op join failing on a modded UE4 game, and
    it has nothing to do with the UE4SS Lua mods, which are per-machine and are NOT
    replicated.

    Nothing is deleted. The paks are MOVED to a sibling folder (~mods_disabled) and can be
    put back with -Restore.

.PARAMETER Restore
    Move the paks back.

.PARAMETER Keep
    Names (or partial names) to leave enabled. e.g. -Keep BPLoader,UltimateMM

.EXAMPLE
    .\TogglePakMods.ps1
.EXAMPLE
    .\TogglePakMods.ps1 -Restore
#>
[CmdletBinding()]
param(
    [switch]$Restore,
    [string[]]$Keep = @(),
    [string]$GameDir
)

$ErrorActionPreference = "Stop"

function Find-GameDir {
    $rel = @(
        "Program Files (x86)\Steam\steamapps\common\Crab Champions",
        "SteamLibrary\steamapps\common\Crab Champions",
        "Steam\steamapps\common\Crab Champions"
    )
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        foreach ($r in $rel) {
            $c = Join-Path $d.Root $r
            if (Test-Path (Join-Path $c "CrabChampions\Content\Paks")) { return $c }
        }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir) {
    Write-Host "Could not find Crab Champions. Pass -GameDir '<path to Crab Champions>'" -ForegroundColor Red
    Read-Host "Press Enter"; exit 1
}

$mods     = Join-Path $GameDir "CrabChampions\Content\Paks\~mods"
$disabled = Join-Path $GameDir "CrabChampions\Content\Paks\~mods_disabled"

if (Get-Process -Name "CrabChampions-Win64-Shipping" -ErrorAction SilentlyContinue) {
    Write-Host "Crab Champions is running. Close it first." -ForegroundColor Red
    Read-Host "Press Enter"; exit 1
}

if ($Restore) {
    if (-not (Test-Path $disabled)) {
        Write-Host "Nothing to restore - $disabled does not exist." -ForegroundColor Yellow
        Read-Host "Press Enter"; exit 0
    }
    New-Item -ItemType Directory -Path $mods -Force | Out-Null
    $n = 0
    Get-ChildItem $disabled -Filter *.pak | ForEach-Object {
        Move-Item $_.FullName (Join-Path $mods $_.Name) -Force; $n++
        Write-Host "  restored $($_.Name)" -ForegroundColor Green
    }
    if (-not (Get-ChildItem $disabled -ErrorAction SilentlyContinue)) { Remove-Item $disabled -Force }
    Write-Host "`nRestored $n pak mod(s)." -ForegroundColor Cyan
    Read-Host "Press Enter"; exit 0
}

if (-not (Test-Path $mods)) {
    Write-Host "No ~mods folder at $mods - nothing to disable." -ForegroundColor Yellow
    Read-Host "Press Enter"; exit 0
}

New-Item -ItemType Directory -Path $disabled -Force | Out-Null
$moved = 0; $kept = 0

Get-ChildItem $mods -Filter *.pak | ForEach-Object {
    $skip = $false
    foreach ($k in $Keep) { if ($_.Name -like "*$k*") { $skip = $true } }
    if ($skip) {
        Write-Host "  KEPT     $($_.Name)" -ForegroundColor Cyan; $kept++
    } else {
        Move-Item $_.FullName (Join-Path $disabled $_.Name) -Force
        Write-Host "  disabled $($_.Name)" -ForegroundColor DarkYellow; $moved++
    }
}

Write-Host ""
Write-Host "Disabled $moved pak mod(s), kept $kept." -ForegroundColor Cyan
Write-Host "They are in ~mods_disabled - nothing was deleted." -ForegroundColor Gray
Write-Host ""
Write-Host "NOW: have your friend try joining again." -ForegroundColor Yellow
Write-Host "  joins fine  -> a pak mod was the cause; add them back a few at a time" -ForegroundColor Gray
Write-Host "  still drops -> paks are innocent, tell Claude and we look elsewhere" -ForegroundColor Gray
Write-Host ""
Write-Host "Put them back with:  .\TogglePakMods.ps1 -Restore" -ForegroundColor Gray
Read-Host "Press Enter to close"
