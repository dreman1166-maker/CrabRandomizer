<#
.SYNOPSIS
    Assembles CrabRandomizer release archives.

.DESCRIPTION
    Produces two zips in dist/ :

      CrabRandomizer-<ver>.zip          mod + README + Install.bat/Uninstall.bat
                                        -> GitHub releases
      CrabRandomizer-<ver>-nexus.zip    mod + README only, NO scripts
                                        -> Nexus, whose scanner quarantines archives
                                           containing .bat/.ps1

    Refuses to build if main.lua's MOD_VERSION doesn't match -Version, and runs the
    test suite first unless -SkipTests. A release should never disagree with itself.

.EXAMPLE
    .\build.ps1 -Version 1.4.4
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$mainLua = Join-Path $root "Mods\CrabRandomizer\Scripts\main.lua"

# --- version must match the source of truth ---
$declared = (Select-String -Path $mainLua -Pattern 'local MOD_VERSION = "([^"]+)"').Matches[0].Groups[1].Value
if ($declared -ne $Version) {
    Write-Host "Version mismatch." -ForegroundColor Red
    Write-Host "  main.lua declares : $declared"
    Write-Host "  you asked to build: $Version"
    Write-Host "Update MOD_VERSION in main.lua (and CHANGELOG.md) first."
    exit 1
}
Write-Host "Version $Version matches main.lua" -ForegroundColor Green

# --- tests ---
if (-not $SkipTests) {
    Write-Host "Running test suite..." -ForegroundColor Cyan
    Push-Location (Join-Path $root "tools")
    try {
        if (-not (Test-Path "node_modules")) {
            Write-Host "  installing dev deps..."
            npm install --silent | Out-Null
        }
        $out = node runtests.js "..\Mods\CrabRandomizer\Scripts\main.lua" 2>&1 | Out-String
        $summary = ($out.Trim().Split("`n") | Select-Object -Last 1).Trim()
        Write-Host $summary

        # Parse the summary rather than grepping for "FAIL": PowerShell's -match is
        # case-insensitive, so it hits the word "fail" inside legitimate PASS lines such
        # as "fail-open: still randomizes".
        if ($summary -notmatch '(\d+)\s+passed,\s+(\d+)\s+failed') {
            Write-Host "Could not read the test summary - refusing to build." -ForegroundColor Red
            Write-Host $out
            exit 1
        }
        if ([int]$Matches[2] -ne 0) {
            Write-Host "$($Matches[2]) test(s) failed - refusing to build." -ForegroundColor Red
            Write-Host (($out.Split("`n") | Select-String -Pattern '^\s*FAIL' -CaseSensitive) -join "`n")
            exit 1
        }
    } finally { Pop-Location }
} else {
    Write-Host "Skipping tests (-SkipTests)" -ForegroundColor Yellow
}

# --- stage ---
$dist = Join-Path $root "dist"
$stage = Join-Path $dist "_stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item (Join-Path $root "Mods") $stage -Recurse
# README.txt lives beside the scripts in the repo; the zip wants it at the root.
$innerReadme = Join-Path $stage "Mods\CrabRandomizer\README.txt"
if (Test-Path $innerReadme) { Move-Item $innerReadme (Join-Path $stage "README.txt") -Force }

# --- Nexus build: no executables, or the archive gets auto-quarantined ---
$nexusZip = Join-Path $dist "CrabRandomizer-$Version-nexus.zip"
Remove-Item $nexusZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $nexusZip
Write-Host "Built $nexusZip" -ForegroundColor Green

# --- GitHub build: installer included ---
Copy-Item (Join-Path $root "installer\*") $stage -Force
$ghZip = Join-Path $dist "CrabRandomizer-$Version.zip"
Remove-Item $ghZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $ghZip
Write-Host "Built $ghZip" -ForegroundColor Green

Remove-Item $stage -Recurse -Force
Write-Host ""
Write-Host "Upload $([IO.Path]::GetFileName($ghZip)) to GitHub releases," -ForegroundColor Cyan
Write-Host "and $([IO.Path]::GetFileName($nexusZip)) to Nexus." -ForegroundColor Cyan
