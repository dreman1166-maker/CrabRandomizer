# Changelog

Versions are declared in `Mods/CrabRandomizer/Scripts/main.lua` (`MOD_VERSION`); CI fails if
this file has no matching entry. Check yours in game with `randomizestatus`.

## [1.6.0]

**Fixes the client crash on island clear.** Two diagnostic bundles from an affected
machine showed the same thing: ZERO "cleared an island" lines in the mod log, while the
game crashed on every island clear with an access violation inside UE4SS.dll (twice, 37
bytes apart in the same function; bad addresses 0x8 and 0xFFFFFFFFFFFFFFFF).

The handler was dying on object access BEFORE it could log anything.
ClientOnClearedIsland fires during level teardown, so `Context:get()`, `.PlayerState`,
`:IsValid()` and `:GetFullName()` can all touch freed or null memory - a native fault no
pcall can catch. Deferring the shuffle by 2.5s (1.4.4) did not help, because the player
identity lookup still ran inline inside the hook.

The hook now touches NOTHING: it increments a counter and schedules. Island counting is
per-machine rather than per-player, since deriving a player key was the very thing that
required reading PlayerState. ClientOnClearedIsland is a Client RPC that fires once per
machine, so this is equivalent in practice.

## [1.5.1]

**Fixes pools getting stuck partially loaded.** Data assets load lazily: a scan at
mod-load finds only a fraction of them (a real log showed WeaponModDA=4 at startup vs 91
once in a lobby). RefreshAllPools only rescanned pools that were empty or invalid, so a
partially-filled pool never grew and the mod silently rerolled from a fraction of the
real item list. It now always rescans, and logs when a pool grows.

Also confirmed from that log: this build has NO separate grenade slot - CrabGrenadeModDA
does not exist, so grenades are the "Ability" slot. The per-subsystem degradation handled
it exactly as intended, disabling only that one slot type.

## [1.5.0]

**In-game chat commands** - the closest thing to an in-game menu that is actually
possible from Lua.

Type `!rand help` in the game's own chat box. No F10 console needed:

```
!rand now              reroll now
!rand undo             undo the last shuffle
!rand status           version and current settings
!rand auth             co-op authority check
!rand history          recent changes
!rand preset <name>    off | gentle | default | chaos | mirror | swap
!rand set <key> <val>  change any setting
```

Disable with `chatCommands=false`; change the trigger with `chatPrefix`.

Why this rather than a drawn overlay: UE4SS exposes no ImGui or drawing bindings to Lua
mods (upstream issue #1072, still open), and a custom UMG overlay needs a widget asset
authored in Unreal Editor and cooked into a `.pak` with the game modkit. The chat box
(`CrabGameStateUI:OnChatTextCommitted`) is hookable, so it becomes the control surface.

## [1.4.4]

**Fixes a hard crash for joining clients on island transitions.**

- **Removed a null dereference.** `RandomizeEveryone` called `UEHelpers.GetPlayerController()`,
  which does `Controller.Pawn:IsValid()` without first checking whether `Pawn` is null. During an
  island transition the pawn is destroyed and respawned, so `Pawn` is transiently null and that
  call reads offset `0x8` of null — a native access violation no `pcall` can catch. A crash dump
  confirmed the fault address and named `UE4SS.dll` as the faulting module. The call had also
  become dead code in 1.4.3.
- **The shuffle is now deferred ~2.5s after an island clear** (`shuffleDelayMs`) instead of running
  synchronously inside the hook, which fired during level teardown. The original Crab Randomizer
  used a 3s delay on this same trigger — strong evidence its author hit the same instability.
  Set `shuffleDelayMs=0` for the old behaviour.
- Test harness now exercises the island-clear path end to end (92 assertions).

## [1.4.3]

- **Base-loadout rerolls are authority-only.** Previously `RandomizeBaseLoadout` allowed
  `hasAuthority OR IsSamePlayerState(...)`, letting a joining client reroll its own gear — which
  fires `ServerEquipInventory` as a client→server RPC and crashed clients every island. The loop
  also iterated all players rather than only authoritative ones. Only the authoritative machine
  mutates anything now. Solo play is unaffected: a solo player *is* the authority.

## [1.4.2]

- Guarded every write against unloaded/stale data assets (`IsValid` before any property write,
  RPC, or undo restore). Fails **open** with a warning if `IsValid()` isn't callable, rather than
  silently filtering everything out and disabling the mod.
- Pools rebuild themselves if their cache goes stale, instead of being scanned once forever.

## [1.4.1]

- <kbd>Ctrl</kbd>+<kbd>K</kbd> and the other keybinds now display in the in-game console. Keybinds
  aren't console commands, so UE4SS hands them no output device; the mod caches one from the first
  console command of the session (the approach UE4SS's own `ConsoleCommandsMod` uses).
- Menu shows `Numpad 1` rather than `Numpad ONE`, and surfaces `randomizeGrenadeMods`.

## [1.4.0]

- Added a grenade-mod slot (`randomizeGrenadeMods`). Some builds fold grenades into the "Ability"
  slot; if `CrabGrenadeModDA` doesn't exist, that one slot type disables itself and says so.

## [1.3.1]

- Console commands print into the **in-game** console. `print()` only reaches UE4SS's debug GUI
  window, which ships hidden, so every command previously looked like it did nothing.

## [1.3.0]

- Co-op **mirror** and **swap** modes.
- `randomizeauthority`, `randomizeundo`, `randomizelist`, `randomizedump`, `randomizepreset`.
- Class-name fallback: a renamed game class is reported instead of silently breaking.
- Per-subsystem degradation, optional file logging, dry-run mode, rebindable keys, direct reroll
  hotkey, `rollMode` replacing the redundant `randomizeWithinRarity` (auto-migrated).
- Installer no longer wipes your config on upgrade.

## [1.2.0]

- Per-invocation `pcall` on every keybind callback; readable "Player N" labels.

## [1.1.1]

- Fixed `randomizehistory` crashing on non-integer input; `pcall` parity across all console
  commands; hardened `rarityWeight` lookup against an int/float subtype mismatch.

## [1.1.0]

- Weighted rarity rolls, shuffle history, per-slot manual reroll.

## [1.0.1]

- `minimumRarity` floor, `randomSeed`, <kbd>Ctrl</kbd>+<kbd>K</kbd> quick menu, `randomizeset`,
  version string.

## [1.0.0]

- Initial rebuilt release.
