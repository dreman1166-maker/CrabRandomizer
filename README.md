# CrabRandomizer

A [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) Lua mod for **Crab Champions** that rerolls your
weapon mods, ability mods, melee mods, grenade mods, perks and relics — and optionally your gun,
ability and melee themselves — every N islands you clear.

Co-op extras: **mirror** mode (everyone gets the same rolled loadout) and **swap** mode (players
trade loadouts with each other).

> Inspired by [Crab Randomizer](https://www.nexusmods.com/crabchampions/mods/16) by **SamelCamel**.
> This is an independent rebuild rather than a copy — the concept is shared, the implementation is
> new — written after the original stopped working against UE4SS 3.0.1 and a game update.
> Please go endorse the original.

---

## Install

**Vortex cannot install this.** The Crab Champions Vortex extension only deploys `.pak` files;
this is Lua that belongs in `Binaries/Win64/Mods/`. Installing is manual either way.

**1. Install UE4SS v3.0.1+ first.** Its files must sit *directly* in your game's Win64 folder,
beside the exe:

```
...\Crab Champions\CrabChampions\Binaries\Win64\
    CrabChampions-Win64-Shipping.exe
    dwmapi.dll              <-- HERE, not in a subfolder
    UE4SS.dll
    UE4SS-settings.ini
    Mods\
```

If extracting UE4SS leaves you with `Win64\UE4SS_v3.0.1\` containing those files, move everything
up one level. **This is the single most common reason mods appear not to work.**

**2. Install the mod** — either:
- Download a [release](../../releases), extract, run `Install.bat`, or
- Copy `Mods/CrabRandomizer` into `Binaries\Win64\Mods\` and add `CrabRandomizer : 1` to
  `Binaries\Win64\Mods\mods.txt`

**3. Verify** — launch, press <kbd>F10</kbd>, run `randomizestatus`. You want a version line and
`Health: pools OK 9/9`.

---

## Quick start

Press <kbd>F10</kbd> for the console, then pick a preset:

| Preset | What it does |
|---|---|
| `randomizepreset gentle` | inventory only, same-rarity swaps, every 6 islands |
| `randomizepreset default` | inventory only, weighted rolls, every 6 islands |
| `randomizepreset chaos` | everything including your gun, every single island |
| `randomizepreset mirror` | co-op: everyone gets the SAME loadout |
| `randomizepreset swap` | co-op: players TRADE loadouts |
| `randomizepreset off` | disable |

## Controls

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | quick menu |
| <kbd>Ctrl</kbd>+<kbd>R</kbd> | reroll now |
| <kbd>Numpad 1-8</kbd> | toggle options (menu open) |
| <kbd>Shift</kbd>+<kbd>Numpad 1-5</kbd> | reroll only that slot type |
| <kbd>Numpad .</kbd> | undo last shuffle |

All rebindable in `randoconfig.txt`.

> Keybinds aren't console commands, so UE4SS gives them no output device. Run any console command
> once per session and keybind text starts appearing. Their *actions* always work regardless.

## Console commands

| Command | What it does |
|---|---|
| `randomizenow` | force a shuffle |
| `randomizeundo` | revert the most recent shuffle |
| `randomizehistory [n]` | what changed, slot by slot |
| `randomizestatus` | version, settings, item counts, health |
| `randomizeauthority` | who this instance can affect (co-op check) |
| `randomizeset <key> <value>` | change one setting and save it |
| `randomizepreset <name>` | apply a preset |
| `randomizeonce <key>` | reroll just one slot type |
| `randomizelist <pool>` | list every perk/mod/relic in the game |
| `randomizedump` | write a diagnostic file for bug reports |

---

## Co-op

This rewrites replicated player state, which only works on the machine holding network
authority — normally the **host** in a listen-server session.

- The **host** should run it. Their copy randomizes everyone.
- Harmless if a joining player also has it: it detects it has no authority and skips.
- `randomizeauthority` tells you exactly which players your instance can affect.

**Only the authoritative machine mutates anything.** An earlier version let a client reroll its
own base loadout, which fired `ServerEquipInventory` as a client→server RPC and crashed joining
clients every island. See [CHANGELOG](CHANGELOG.md).

---

## Development

```bash
cd tools
npm install          # fengari (pure-JS Lua 5.3)
node check.js    ../Mods/CrabRandomizer/Scripts/main.lua    # syntax only
node runtests.js ../Mods/CrabRandomizer/Scripts/main.lua    # full suite
```

`tools/mock_ue4ss.lua` fakes enough of UE4SS (`FindAllOf`, `RegisterHook`, `RegisterKeyBind`,
TArray semantics, `HasAuthority`, `IsValid`, FOutputDevice) to load the **real** `main.lua`
headless and exercise it. The suite covers presets, mirror/swap, undo, the authority gate,
dry-run, per-slot degradation, class-name fallback, stale-asset guards, console output routing,
and the island-clear deferral.

It has caught real shipped bugs — a nonexistent `Key.NUM_PERIOD`, a crash on non-integer
`randomizehistory` input, and a "fix" that would have silently disabled the whole mod.

Build release archives:

```powershell
.\build.ps1 -Version 1.4.4
```

## Structure

```
Mods/CrabRandomizer/Scripts/main.lua   the mod
Mods/CrabRandomizer/Scripts/randoconfig.txt
installer/                             Install.bat / Uninstall.bat + PowerShell
tools/                                 mocked-UE4SS test harness
build.ps1                              assembles release zips
```

## Auto-update

Run **`installer/Update.bat`** any time. It checks GitHub releases, compares against the
`MOD_VERSION` in your installed `main.lua`, and installs the newer version **keeping your
`randoconfig.txt` settings** (new options get added at their defaults, and it writes a `.bak`).
No GitHub account or token needed.

To never be on a stale version, launch the game with **`installer/Play Crab Champions.bat`**
instead of Steam's Play button — it updates first, then launches. If GitHub is unreachable it
skips the update and launches anyway.

```powershell
.\Update.ps1              # interactive
.\Update.ps1 -Silent      # for launch scripts / scheduled tasks
.\Update.ps1 -CheckOnly   # exit 10 = update available, 0 = current
```

> **Why not Velopack / a normal updater framework?** Those manage a standalone *application* —
> they own an install directory, replace an exe, and relaunch a process. This mod is a `.lua` file
> read by UE4SS inside the game's process: there's no binary of ours to replace. And UE4SS's Lua
> sandbox has no networking API, so the mod cannot update itself from the inside. An external
> script is the only thing that can do the job here, so that's what this is.

## Notes on distribution

- **Nexus quarantines archives containing `.bat`/`.ps1`** (`-ExecutionPolicy Bypass` plus
  recursive file operations reads as a dropper to their scanner). The Nexus release is
  Lua-only; the installer ships in the GitHub release.
- **There is no auto-update.** UE4SS's Lua sandbox has no networking API, and this isn't a
  standalone app for an updater framework to attach to. `randomizestatus` reports your version.

## License

MIT — see [LICENSE](LICENSE).
