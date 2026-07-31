CrabRandomizer - for Crab Champions
====================================

Rerolls your weapon mods, ability mods, melee mods, grenade mods, perks and relics -
and optionally your gun, ability and melee themselves - every N islands you clear.
Co-op mirror and swap modes. Built for UE4SS v3.0.1+.

This archive contains only two plain text files (a Lua script and a config file).
There is no installer and no executable of any kind - installing is a copy/paste.

REQUIREMENTS
------------
- Crab Champions (Steam), Early Access "beta" branch, as of 2026-07-31.
  Not separately verified on the Default Public Version.
- UE4SS v3.0.1 or newer: https://github.com/UE4SS-RE/RE-UE4SS/releases

INSTALL
-------
STEP 1 - Install UE4SS first (this is where almost everyone goes wrong)

Its files must sit DIRECTLY in your game's Win64 folder, beside the game .exe:

    ...\Crab Champions\CrabChampions\Binaries\Win64\
        CrabChampions-Win64-Shipping.exe
        dwmapi.dll              <-- HERE
        UE4SS.dll               <-- not in a subfolder
        UE4SS-settings.ini
        Mods\

If extracting UE4SS leaves you with Win64\UE4SS_v3.0.1\ containing those files, move
everything up one level into Win64 and delete the empty folder. Otherwise UE4SS never
loads and nothing below matters.

To find the folder: Steam library -> right-click Crab Champions -> Manage ->
Browse local files -> CrabChampions -> Binaries -> Win64.

STEP 2 - Copy this mod in

Copy the "Mods\CrabRandomizer" folder from this archive into:

    ...\Crab Champions\CrabChampions\Binaries\Win64\Mods\

You should end up with:

    Win64\Mods\CrabRandomizer\Scripts\main.lua
    Win64\Mods\CrabRandomizer\Scripts\randoconfig.txt

STEP 3 - Enable it

Open Win64\Mods\mods.txt in Notepad and add this line:

    CrabRandomizer : 1

Put it above the "; Built-in keybinds, do not move up!" comment near the bottom.
UE4SS ignores any mod folder that is not listed in this file.

STEP 4 - Check it worked

Launch the game, press F10 for the console, and type:

    randomizestatus

You should see the version, your settings, and how many items it found, ending with a
line like "Health: pools OK 9/9".

  No console on F10?        UE4SS is not loading - recheck Step 1.
  Console but no output?    Not enabled - recheck Step 3.
  A pool shows 0 entries?   A game update renamed something. Run randomizedump and
                            post the file it writes in the mod's Bugs tab.

UNINSTALL
---------
Delete Win64\Mods\CrabRandomizer and remove the "CrabRandomizer : 1" line from
mods.txt. Nothing else is touched.

QUICK START
-----------
Press F10 and pick a preset:

    randomizepreset gentle    inventory only, same-rarity swaps, every 6 islands
    randomizepreset default   inventory only, weighted rolls, every 6 islands
    randomizepreset chaos     everything including your gun, every single island
    randomizepreset mirror    co-op: both players get the SAME rolled loadout
    randomizepreset swap      co-op: players TRADE loadouts with each other
    randomizepreset off       disable it

CONTROLS
--------
    Ctrl+K              quick menu
    Ctrl+R              reroll right now
    Numpad 1-8          toggle options (while the menu is open)
    Shift+Numpad 1-5    reroll ONLY that one slot type
    Numpad .            undo the last shuffle

All rebindable in randoconfig.txt.

The menu prints into the UE4SS console (F10); it is not a drawn on-screen panel -
UE4SS does not let Lua mods draw one. Keybind text also needs the console: run any
console command once per session and keybind output appears there from then on. The
keybind ACTIONS always work regardless.

CONSOLE COMMANDS
----------------
    randomizenow                 force a shuffle
    randomizeundo                revert the most recent shuffle
    randomizehistory [count]     what changed, slot by slot (last 50 kept)
    randomizestatus              version, all settings, and item counts
    randomizeauthority           who this instance can affect (co-op check)
    randomizeset <key> <value>   change one setting and save it
    randomizepreset <name>       apply a preset
    randomizeonce <key>          reroll just one slot type
    randomizelist <pool>         list every perk/mod/relic in the game
    randomizedump                write a diagnostic file for bug reports

Every setting is documented inline in Scripts\randoconfig.txt.

CO-OP
-----
This rewrites replicated player state, which only works on the machine with network
authority - normally the HOST.

  - The HOST should run it. Their copy randomizes everyone.
  - Harmless if a joining player also has it: it detects it has no authority and
    skips instead of desyncing.
  - Run "randomizeauthority" to see exactly who your instance can affect.

UPDATING
--------
Replace Scripts\main.lua with the new one. KEEP your own randoconfig.txt - new
settings fall back to sensible defaults if they are missing from your file, and the
mod logs which ones it defaulted.

WHAT HAS AND HASN'T BEEN TESTED IN-GAME
---------------------------------------
Confirmed working in-game on the beta branch: the mod loads and finds everything
(113 perks, 91 weapon mods, 66 relics, 42 ability mods, 13 melee mods, 20 weapons,
7 abilities, 5 melees); config loading and console output work.

Covered by an automated test suite (67 tests) but not yet watched in a live session:
undo, mirror/swap modes, and the co-op authority gate. If any of those misbehave, run
"randomizedump" and attach the file to a bug report.

CREDITS
-------
Inspired by "Crab Randomizer" by SamelCamel (NexusMods #16). This is an independent
rebuild rather than a copy - the concept is shared, the implementation is new - written
after the original stopped working against UE4SS 3.0.1 and a game update. Please go
endorse the original.

Built on UE4SS by the UE4SS-RE team.
