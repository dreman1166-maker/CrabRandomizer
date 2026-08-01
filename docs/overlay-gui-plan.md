# CrabRandomizer — plan for a proper in-game GUI

Written 2026-08-01, after five failed iterations. This exists so the next attempt is driven
by what has been *proven in a live session*, not by reasoning about how the game probably
works. Every claim below marked **PROVEN** came out of a log from Dre's machine.

---

## 1. What is settled

### Lua cannot draw. Closed.
**PROVEN.** v1.8.0 hooked `AHUD:ReceiveDrawHUD` — a `BlueprintImplementableEvent`, so a
genuinely hookable UFunction. In a live session the hook fired **0 times**. Crab Champions
renders its entire HUD through UMG and never reaches `AHUD::DrawHUD`.

Worse, leaving that hook registered across the actor teardown on co-op invite-accept
contributed to instability. Removed entirely in v1.8.1. **Do not revisit.**

### The UE4SS C++ API is unavailable. Closed.
RE-UE4SS's `UEPseudo` submodule 404s on `main` and `v3.0.1`. The SDK cannot be assembled
from a clean clone by anyone. Confirmed via `gh api` (404) and a repo search (no results).

*But this only ever blocked using UE4SS's API.* A plain DLL that UE4SS `LoadLibrary`s needs
nothing from it — satisfy the export contract with stubs:

```cpp
extern "C" __declspec(dllexport) void* start_mod()   { return nullptr; }
extern "C" __declspec(dllexport) void  uninstall_mod(void*) {}
```

**PROVEN** — UE4SS logs `Starting C++ mod 'CrabRandomizerOverlay'` and the DLL runs.
Conflating "C++ menu" with "C++ menu *via UE4SS*" cost several days.

### D3D11 hooking works. The DLL runs.
**PROVEN**, all from `CrabOverlay.log`:

| | |
|---|---|
| `Present` resolved and hooked | `Present=0x7FFCFDDE9960` |
| Hook fires every frame | the F8 poll lives inside it and responds instantly |
| ImGui initialises | `ImGui initialised, window=0x041F0822` |
| Toggle works | `menu OPENED (polled)` on every press |
| No crash | current build is stable |

**Nothing is visible.** That is the entire remaining problem.

---

## 2. Failures so far, and the lesson from each

| # | Symptom | Real cause | Lesson |
|---|---|---|---|
| 1 | Nothing at all | Hooked only `Present`, no logging | **Never ship a hook with no logging.** Turned rounds 1–2 into guesswork. |
| 2 | Ctrl+K dead, Ctrl+R fine | `WndProc` hook never received keys — UE4SS hooks the same window; UE4 uses raw input | Don't rely on window messages. Poll inside the render hook. |
| 3 | Crash on open | Hooked **both** `Present` and `Present1`; DXGI routes `Present1` → `Present`, so `NewFrame()` ran twice per frame | Redundant "insurance" hooks are not free. |
| 4 | Opens, invisible | RTV cached at init; DXGI flip model rotates buffer 0 every frame | Acquire the back buffer **per frame**. |
| 5 | Still invisible | Under investigation — `DisplaySize` or target | Instrument *before* iterating. |

**The meta-lesson: I iterated against a live game instead of instrumenting first.** Rounds
3–5 each cost a crash or a wasted session. The current build finally logs
`DisplaySize` + `verts` + `cmdlists`, which is what should have existed in round 1.

---

## 3. The decisive next data point

The installed build logs, once, on first draw:

```
draw: DisplaySize=1920x1080 verts=2431 cmdlists=1
```

This **fully partitions** the remaining possibilities:

- **`verts=0` or `DisplaySize=0x0`** → ImGui emitted no geometry. `ImGui_ImplWin32_NewFrame`
  derives `DisplaySize` from `GetClientRect`, which returns empty in some fullscreen modes.
  Already fixed in this build (taken from the swapchain instead). **It will now work.**
- **`verts>0` and still invisible** → geometry is produced but never reaches the screen.
  In-process rendering is the wrong bet. **Go to Plan B.**

Do not write another line of rendering code before reading this.

---

## 4. Plan A — finish the in-process overlay

Only if `verts>0` but invisible. In descending order of likelihood:

1. **Render state.** UE4 leaves blend/depth/rasteriser state bound at Present. ImGui's DX11
   backend sets its own, but a bound depth-stencil view with depth-test enabled can discard
   the quads. Fix: `OMSetRenderTargets(1, &rtv, nullptr)` — already done — plus explicitly
   clear depth state.
2. **Wrong swapchain.** UE4 can create more than one. Log the `sc` pointer each frame; if it
   varies, re-init on change.
3. **Multi-threaded RHI.** UE4 has a dedicated RHI thread. Drawing on the immediate context
   from the Present thread is normally safe (Present runs on the RHI thread), but verify the
   thread ID is stable.
4. **Fullscreen mode.** Ask Dre to switch to **borderless windowed** and retest. Exclusive
   fullscreen changes the presentation path.

---

## 5. Plan B — separate transparent overlay window (RECOMMENDED)

**Why this is the better architecture regardless.** Its correctness does not depend on a
single assumption about how the game renders — which is precisely the thing that has been
wrong five times.

### Design

```
  ┌─────────────────────────────────────┐
  │  Game process (unchanged)           │
  │    UE4SS → CrabRandomizer.lua       │
  │             ↕ randoconfig.txt       │
  │             ↕ uicommand.txt         │
  └─────────────────────────────────────┘
                    ↕  (files only)
  ┌─────────────────────────────────────┐
  │  CrabOverlay.exe  (separate process)│
  │    layered topmost window           │
  │    own D3D11 device + swapchain     │
  │    ImGui                            │
  └─────────────────────────────────────┘
```

**Nothing is injected.** No hook, no shared device, no shared render state.

### Why it cannot fail the way the others did
- Never touches the game's device, context, buffers or state → cannot crash the render thread
- Not a `.pak` → immune to the host/client pak matching that broke co-op invites
- Not a DLL in the game → cannot be blamed for any game crash
- Visibility depends only on Windows compositing, not on UE4 internals
- Trivially removable: close the exe

### Known limitation
Does not appear over **exclusive** fullscreen. Borderless and windowed are fine, and this
game defaults to borderless. Detect exclusive fullscreen and tell the user rather than
silently showing nothing.

### Build steps

1. **Window** — `CreateWindowEx` with
   `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP`,
   `WS_POPUP`. Per-pixel alpha via `DwmExtendFrameIntoClientArea` or a
   `DXGI_ALPHA_MODE_PREMULTIPLIED` composition swapchain.
2. **Track the game window** — find `CrabChampions-Win64-Shipping` by process, poll
   `GetWindowRect` ~10 Hz, match position/size. Hide when the game is not foreground.
3. **Click-through when idle** — add `WS_EX_TRANSPARENT` while the menu is closed so gameplay
   input passes through; remove it while open so the menu is clickable.
4. **Render** — standard ImGui + D3D11 in our own swapchain. No hooking anywhere.
5. **Hotkey** — `RegisterHotKey(VK_F8)` globally, so it works while the game has focus.
6. **Interop** — reuse exactly what the DLL already does: read `randoconfig.txt`, append to
   `uicommand.txt`. **This code is already written and correct** — lift it verbatim from
   `overlay/dllmain.cpp`.
7. **Launch** — a small `StartOverlay.bat`, or have the Lua mod spawn it. Prefer manual first;
   auto-launch only once it is proven.

### Reuse
`DrawMenu()` in `overlay/dllmain.cpp` is complete and correct — presets, 9 toggles, co-op
mode, roll mode, sliders, rarity weights, action buttons. **It moves across unchanged.**
Only the windowing and device-creation layer is new. Realistically ~200 new lines.

---

## 6. Plan C — UMG widget in a `.pak`

The native-looking route, and **proven possible in this game**: `UltimateMM_P.pak` (already
in Dre's `~mods`) contains `WBP_Spawner.uasset`, `WBP_SpawnerItemRow.uasset`,
`BP_Spawner.uasset` — a real in-game menu by *remortal*, loaded by `BPLoader_P.pak`.

**Blocked on asset authoring, not code.** Needs Unreal Editor 4.27 + the Crab Champions
modkit, and someone to build the widget. Claude cannot author `.uasset` files.

**Also carries a real cost:** it is a `.pak`, so both co-op players must install it or joins
break — the exact failure that cost an evening. See `crabrandomizer-coop-pak-mismatch`.

Only worth it if a native look matters more than the ImGui route working.

---

## 7. Testing discipline — the actual fix

Every failure above traces to testing in the live game instead of instrumenting first.

1. **Log before you build.** Any hook ships with logging on day one, covering: installed,
   fired, initialised, produced-geometry, rendered.
2. **One hypothesis per build.** Round 3 crashed because two "fixes" shipped together.
3. **Never add redundant insurance.** The `Present1` hook was insurance against a
   possibility the log had already ruled out — and it caused the crash.
4. **Prefer a build that reports over a build that guesses.** A log line beats an hour of
   reasoning about UE4 internals.
5. **Solo-test anything new before co-op.** The randomizer took seven releases to stabilise;
   never risk it for a cosmetic feature.

---

## 8. Recommendation

1. **Read one log line** (`draw: ... verts=N`). Free, decisive, already installed.
2. **If `verts=0`** — done, current build works.
3. **If `verts>0`** — build **Plan B**. Do not spend more rounds on in-process rendering;
   the geometry exists and the game's pipeline is eating it, and every further attempt is
   another guess about internals that has been wrong five times.
4. **Plan C only** if the native look is worth the editor work and the pak-matching cost.

**Regardless of route: the randomizer stays untouched.** v1.8.1 works, co-op works, mirror
and swap cover the weapon/ability/melee. No GUI work is permitted to regress that.
