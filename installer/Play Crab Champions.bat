@echo off
REM Checks for a CrabRandomizer update, installs it, THEN launches the game.
REM Use this instead of Steam's Play button and you can never be on a stale version.
REM If GitHub is unreachable the update step is skipped and the game still launches.
echo Checking for CrabRandomizer updates...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update.ps1" -Silent
echo Launching Crab Champions...
start "" "steam://rungameid/774801"
