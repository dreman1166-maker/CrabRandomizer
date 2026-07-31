--[[
    CrabRandomizer - for UE4SS v3.0.1 / Crab Champions (beta branch)

    Rebuilt from scratch after the original (NexusMods #16, SamelCamel, v1.2.0) stopped
    working. See README.txt for install/co-op notes and CREDITS.

    Design notes worth knowing before editing this file:

      * Data-asset pools are resolved through a CANDIDATE NAME LIST, not a single
        hardcoded class name. A game patch renaming e.g. CrabPerkDA is the single most
        likely way this mod dies (it's what killed the original), so every lookup tries
        alternates and logs exactly which name resolved. Add new alternates to PoolDefs.
      * Direct property writes are gated on ps:HasAuthority(). In listen-server co-op
        only the HOST has authority over both players' CrabPS, so the host is who should
        run this. A joining client detects it has no authority and skips instead of
        desyncing.
      * Every hook, console command, and keybind CALLBACK body is individually pcall
        wrapped. Note that the pcall around RegisterKeyBind only protects registration -
        SafeCallback is what protects the actual per-press invocation.
      * Anything that mutates state records an undo entry stamped with a generation id,
        so "randomizeundo" can revert one entire shuffle rather than a single slot.
]]

local okHelpers, UEHelpers = pcall(require, "UEHelpers")
if not okHelpers then UEHelpers = nil end

-- Bump on every release re-uploaded to Nexus. There is no auto-update path: UE4SS's Lua
-- sandbox has no networking API, and this isn't a standalone app for something like
-- Velopack to attach to. Nexus/Vortex's own "update available" tracking is the signal;
-- this constant just lets anyone confirm which build they're running.
local MOD_VERSION = "1.8.0"

local MOD_TAG = "[CrabRandomizer]"

local BASE_PATHS = {
    "Mods/CrabRandomizer/Scripts/",
    "ue4ss/Mods/CrabRandomizer/Scripts/",
}

-- ===================== Logging =====================

local LogFilePath = nil       -- resolved lazily, once a writable base path is known
local LogFileFailed = false   -- stop retrying after the first failure
local InLogCall = false       -- guard against recursion if file logging itself logs

local Config -- forward declaration; logging checks Config.logToFile

-- print() writes to UE4SS's own debug GUI window, which ships HIDDEN
-- (GuiConsoleVisible=0 in UE4SS-settings.ini). For output to appear in the in-game
-- console the player actually typed into, we must also write to the FOutputDevice
-- passed to the command callback - the same pattern UE4SS's bundled ConsoleCommandsMod
-- uses. Without this, every command looks like it silently did nothing.
local CurrentAr = nil

-- Keybind callbacks are NOT console commands, so UE4SS hands them no output device.
-- Without caching one, Ctrl+K and friends would print only to the hidden debug window
-- and look completely broken. UE4SS's own bundled ConsoleCommandsMod caches the device
-- globally and reuses it later behind exactly this type check, so this follows shipped
-- precedent rather than holding an arbitrary native pointer. Populated by the first
-- console command run in a session; until then, keybind output goes to the debug
-- window / log file only.

--- ONLY writes to the device UE4SS handed us for the CURRENTLY EXECUTING console
--- command. It must never use a cached one.
---
--- 1.4.1 cached the last device (LastAr) so keybind output could reach the console too.
--- That put a dereference of a stale native pointer on EVERY log call: once the output
--- device is destroyed (level change, console closed) both target:type() and target:Log()
--- touch freed memory, which is a native fault pcall cannot catch. A client log ended
--- exactly at a log() call during an island transition with the game dead immediately
--- after, which is what that would look like.
---
--- The cost is that keybind output no longer appears in the in-game console - it still
--- goes to print() and to crabrandomizer.log. Not crashing wins.
local function ArLog(line)
    if CurrentAr == nil then return end
    pcall(function()
        if type(CurrentAr) == "userdata" and CurrentAr:type() == "FOutputDevice" then
            CurrentAr:Log(line)
        end
    end)
end

local function WriteLogLine(line)
    if LogFileFailed then return end
    if not Config or not Config.logToFile then return end
    if not LogFilePath then return end
    local f = io.open(LogFilePath, "a")
    if not f then
        LogFileFailed = true
        return
    end
    f:write(line .. "\n")
    f:close()
end

local function log(fmt, ...)
    local ok, msg = pcall(string.format, fmt, ...)
    if not ok then msg = tostring(fmt) end
    local line = MOD_TAG .. " " .. msg
    print(line .. "\n")
    ArLog(line) -- mirror into the in-game console (commands AND keybinds)
    if not InLogCall then
        InLogCall = true
        pcall(WriteLogLine, line)
        InLogCall = false
    end
end

-- ===================== Config =====================

-- rollMode replaces the old randomizeWithinRarity boolean. Those two settings were
-- mutually redundant: within-rarity filtering narrows candidates to a single tier
-- BEFORE weighting can apply, so rarityWeight had no effect while it was on. One
-- setting with three explicit values removes the ambiguity.
--   "weighted"     - rarityWeight1-4 bias the roll (default; commons more likely)
--   "withinRarity" - only reroll into the same rarity tier the slot already had
--   "uniform"      - every candidate equally likely, weights ignored
local VALID_ROLL_MODES = { weighted = true, withinRarity = true, uniform = true }

--   "independent" - each player rolls their own loadout (default)
--   "mirror"      - roll once, every player gets the SAME result
--   "swap"        - players trade existing loadouts with each other; nothing is rolled
local VALID_COOP_MODES = { independent = true, mirror = true, swap = true }

Config = {
    rollMode = "weighted",
    coopMode = "independent",

    randomizeWeaponMods = false,
    randomizeAbilityMods = false,
    randomizeMeleeMods = false,
    randomizeGrenadeMods = false,
    randomizePerks = false,
    randomizeRelics = false,

    randomizeWeapon = false,
    randomizeAbility = false,
    randomizeMelee = false,

    islandsBeforeRandomizing = 6,
    minimumRarity = 1,
    randomSeed = 0,

    rarityWeight1 = 50,
    rarityWeight2 = 25,
    rarityWeight3 = 15,
    rarityWeight4 = 10,

    announceInChat = false,
    logToFile = false,
    dryRun = false,

    -- In-game chat commands: type e.g. "!rand help" in the game chat box. This is the
    -- nearest thing to an in-game menu Lua can reach (UE4SS has no Lua drawing API).
    chatCommands = true,
    chatPrefix = "!rand",

    -- Poll for commands from the optional CrabRandomizerUI C++ mod (the ImGui menu).
    -- Harmless with no C++ mod installed: it just never finds a command file.
    uiBridge = true,

    -- Milliseconds to wait after an island clear before shuffling. The transition
    -- destroys and respawns pawns; touching player state during that window is what
    -- crashed clients. 0 = shuffle immediately (old behaviour, NOT recommended).
    shuffleDelayMs = 2500,

    -- Drawn on-screen menu (overlay.lua). OFF by default and deliberately so: it is the
    -- only part of this mod that runs every frame, and the randomizer took seven releases
    -- to stop crashing a co-op client. Turn it on with `randomizeset overlay true` once
    -- you are happy the rest is stable, and it takes effect on the next launch.
    overlay = false,

    keyQuickMenu = "K",
    keyQuickMenuMod = "CONTROL",
    keyRerollNow = "R",
    keyRerollNowMod = "CONTROL",
}

local CONFIG_KEY_ORDER = {
    "rollMode", "coopMode",
    "randomizeWeaponMods", "randomizeAbilityMods", "randomizeMeleeMods",
    "randomizeGrenadeMods", "randomizePerks", "randomizeRelics",
    "randomizeWeapon", "randomizeAbility", "randomizeMelee",
    "islandsBeforeRandomizing", "minimumRarity", "randomSeed",
    "rarityWeight1", "rarityWeight2", "rarityWeight3", "rarityWeight4",
    "announceInChat", "logToFile", "dryRun", "shuffleDelayMs",
    "chatCommands", "chatPrefix", "uiBridge", "overlay",
    "keyQuickMenu", "keyQuickMenuMod", "keyRerollNow", "keyRerollNowMod",
}

local ResolvedBasePath = nil

local function ValidateConfig()
    if not VALID_ROLL_MODES[Config.rollMode] then
        log("rollMode '%s' is not valid (weighted/withinRarity/uniform) - falling back to weighted", tostring(Config.rollMode))
        Config.rollMode = "weighted"
    end
    if not VALID_COOP_MODES[Config.coopMode] then
        log("coopMode '%s' is not valid (independent/mirror/swap) - falling back to independent", tostring(Config.coopMode))
        Config.coopMode = "independent"
    end
    if Config.islandsBeforeRandomizing < 1 then Config.islandsBeforeRandomizing = 1 end
    if Config.minimumRarity < 1 then Config.minimumRarity = 1 end
end

local function LoadConfig()
    local path
    for _, base in ipairs(BASE_PATHS) do
        local candidate = base .. "randoconfig.txt"
        local f = io.open(candidate, "r")
        if f then
            f:close()
            path = candidate
            ResolvedBasePath = base
            break
        end
    end

    if not path then
        -- No config on disk: still pick a base path so logging/dumps have somewhere to go.
        ResolvedBasePath = BASE_PATHS[1]
        LogFilePath = ResolvedBasePath .. "crabrandomizer.log"
        log("randoconfig.txt not found in any known location - using built-in defaults (everything off)")
        return
    end

    LogFilePath = ResolvedBasePath .. "crabrandomizer.log"
    log("Loading config from %s", path)

    for line in io.lines(path) do
        local clean = line:gsub(";.*$", ""):gsub("%s+$", "")
        local key, value = clean:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
        if key and value ~= "" then
            if Config[key] ~= nil then
                local current = Config[key]
                if type(current) == "boolean" then
                    Config[key] = (value:lower() == "true")
                elseif type(current) == "number" then
                    local n = tonumber(value)
                    if n then Config[key] = math.floor(n) end
                else
                    Config[key] = value
                end
            elseif key == "randomizeWithinRarity" then
                -- Migration from <=1.2.0. Kept so existing configs don't silently
                -- change behavior on upgrade.
                if value:lower() == "true" then
                    Config.rollMode = "withinRarity"
                    log("NOTE: randomizeWithinRarity=true is deprecated; migrated to rollMode=withinRarity")
                else
                    log("NOTE: randomizeWithinRarity is deprecated and was ignored (see rollMode)")
                end
            end
        end
    end

    ValidateConfig()
end

local function SaveConfig()
    local base = ResolvedBasePath or BASE_PATHS[1]
    local path = base .. "randoconfig.txt"
    local f = io.open(path, "w")
    if not f then
        log("Could not open %s for writing - this change will not persist across restarts", path)
        return
    end
    for _, key in ipairs(CONFIG_KEY_ORDER) do
        f:write(string.format("%s=%s\n", key, tostring(Config[key])))
    end
    f:close()
    log("Saved config to %s", path)
end

local function ApplySeed()
    if Config.randomSeed ~= 0 then
        local ok = pcall(math.randomseed, Config.randomSeed)
        if ok then
            log("Random seed fixed at %d - shuffles are reproducible. For matching rolls in co-op, BOTH players must set the same randomSeed AND the same rollMode/weights.", Config.randomSeed)
        end
    else
        local ok = pcall(function() math.randomseed(os.time()) end)
        if not ok then log("os.time unavailable - falling back to Lua's default RNG seed") end
    end
end

-- ===================== Presets =====================

local Presets = {
    off = {
        randomizeWeaponMods = false, randomizeAbilityMods = false, randomizeMeleeMods = false, randomizeGrenadeMods = false,
        randomizePerks = false, randomizeRelics = false,
        randomizeWeapon = false, randomizeAbility = false, randomizeMelee = false,
        coopMode = "independent",
    },
    gentle = {
        randomizeWeaponMods = true, randomizeAbilityMods = true, randomizeMeleeMods = true, randomizeGrenadeMods = true,
        randomizePerks = true, randomizeRelics = true,
        randomizeWeapon = false, randomizeAbility = false, randomizeMelee = false,
        rollMode = "withinRarity", islandsBeforeRandomizing = 6, minimumRarity = 1,
        coopMode = "independent",
    },
    default = {
        randomizeWeaponMods = true, randomizeAbilityMods = true, randomizeMeleeMods = true, randomizeGrenadeMods = true,
        randomizePerks = true, randomizeRelics = true,
        randomizeWeapon = false, randomizeAbility = false, randomizeMelee = false,
        rollMode = "weighted", islandsBeforeRandomizing = 6, minimumRarity = 1,
        coopMode = "independent",
    },
    chaos = {
        randomizeWeaponMods = true, randomizeAbilityMods = true, randomizeMeleeMods = true, randomizeGrenadeMods = true,
        randomizePerks = true, randomizeRelics = true,
        randomizeWeapon = true, randomizeAbility = true, randomizeMelee = true,
        rollMode = "uniform", islandsBeforeRandomizing = 1, minimumRarity = 1,
        coopMode = "independent",
    },
    -- weapon/ability/melee used to be false in both of these, because mirror and swap were
    -- only implemented for the slot arrays - leaving them on would have handed each player
    -- a DIFFERENT gun while claiming to mirror. They are handled properly now, so the
    -- presets no longer have to switch off the most visible half of the loadout.
    mirror = {
        randomizeWeaponMods = true, randomizeAbilityMods = true, randomizeMeleeMods = true, randomizeGrenadeMods = true,
        randomizePerks = true, randomizeRelics = true,
        randomizeWeapon = true, randomizeAbility = true, randomizeMelee = true,
        rollMode = "weighted", islandsBeforeRandomizing = 1, coopMode = "mirror",
    },
    swap = {
        randomizeWeaponMods = true, randomizeAbilityMods = true, randomizeMeleeMods = true, randomizeGrenadeMods = true,
        randomizePerks = true, randomizeRelics = true,
        randomizeWeapon = true, randomizeAbility = true, randomizeMelee = true,
        islandsBeforeRandomizing = 1, coopMode = "swap",
    },
}

local PRESET_ORDER = { "off", "gentle", "default", "chaos", "mirror", "swap" }

local function ApplyPreset(name)
    local preset = Presets[name]
    if not preset then return false end
    for k, v in pairs(preset) do
        Config[k] = v
    end
    ValidateConfig()
    return true
end

LoadConfig()
ApplySeed()

-- ===================== Data asset pools (with rename fallback) =====================
--
-- IMPORTANT: only the FIRST candidate in each list is a name confirmed present in this
-- game build. The alternates are speculative guesses at what a future rename might look
-- like - they cost nothing when unused, and the real value is that this table is the one
-- place to add a known-good new name when a patch breaks something. Whatever resolves
-- gets logged by name, so a rename shows up as a clear message instead of silence.

local PoolDefs = {
    { key = "CrabWeaponModDA",  candidates = { "CrabWeaponModDA",  "CrabWeaponModDataAsset",  "WeaponModDA" } },
    { key = "CrabAbilityModDA", candidates = { "CrabAbilityModDA", "CrabAbilityModDataAsset", "AbilityModDA" } },
    { key = "CrabMeleeModDA",   candidates = { "CrabMeleeModDA",   "CrabMeleeModDataAsset",   "MeleeModDA" } },
    -- Grenade mods: present in older builds (the original Duckos mod used
    -- CrabGrenadeModDA/GrenadeMods). This build may instead fold grenades into the
    -- "Ability" slot. If this pool doesn't resolve, per-subsystem degradation disables
    -- just this one and says so - nothing else is affected.
    { key = "CrabGrenadeModDA", candidates = { "CrabGrenadeModDA", "CrabGrenadeModDataAsset", "GrenadeModDA" } },
    { key = "CrabPerkDA",       candidates = { "CrabPerkDA",       "CrabPerkDataAsset",       "PerkDA" } },
    { key = "CrabRelicDA",      candidates = { "CrabRelicDA",      "CrabRelicDataAsset",      "RelicDA" } },
    { key = "CrabWeaponDA",     candidates = { "CrabWeaponDA",     "CrabWeaponDataAsset" } },
    { key = "CrabAbilityDA",    candidates = { "CrabAbilityDA",    "CrabAbilityDataAsset" } },
    { key = "CrabMeleeDA",      candidates = { "CrabMeleeDA",      "CrabMeleeDataAsset" } },
}

local PLAYER_STATE_CANDIDATES = { "CrabPS", "CrabPlayerState" }

local Pools = {}
local ResolvedPoolNames = {}
for _, def in ipairs(PoolDefs) do
    Pools[def.key] = {}
    ResolvedPoolNames[def.key] = nil
end

local function TryFindAllOf(className)
    local ok, found = pcall(FindAllOf, className)
    if ok and found and #found > 0 then return found end
    return nil
end

--- True only if the object is currently a live, valid UObject.
--- Anything cached across a level transition MUST be checked with this before use:
--- writing a stale pointer into replicated state is a native crash, not a Lua error,
--- so no pcall anywhere will save us from it.
local IsValidUnavailable = false

local function IsLive(obj)
    if obj == nil then return false end
    -- If IsValid() isn't callable on these objects at all, we must NOT treat that as
    -- "dead" - doing so would filter out every item and silently disable the whole mod.
    -- Unavailable check => fail OPEN (with one loud warning). Only an explicit false
    -- from a working IsValid() blocks a write.
    if IsValidUnavailable then return true end
    local ok, valid = pcall(function() return obj:IsValid() end)
    if not ok then
        IsValidUnavailable = true
        log("WARNING: IsValid() is not callable on this build's objects - stale-asset protection is DISABLED. If you get crashes on island transitions, please report this line.")
        return true
    end
    return valid == true
end

--- Rebuilds a pool, discarding anything that is no longer a live object.
local function ScanPool(def)
    for _, name in ipairs(def.candidates) do
        local found = TryFindAllOf(name)
        if found then
            local live = {}
            for _, da in ipairs(found) do
                if IsLive(da) then table.insert(live, da) end
            end
            if #live > 0 then
                Pools[def.key] = live
                ResolvedPoolNames[def.key] = name
                if name ~= def.candidates[1] then
                    log("NOTE: '%s' not found, but fallback name '%s' resolved (%d entries). The game likely renamed this class - please report it.",
                        def.candidates[1], name, #live)
                end
                return true
            end
        end
    end
    return false
end

--- IMPORTANT: earlier versions cached each pool once and never rescanned, on the
--- assumption that data assets are static. That is NOT safe across island transitions -
--- the level teardown can unload/GC those objects, leaving the cache full of dangling
--- pointers which then get written into player inventory. That is a prime suspect for
--- hard crashes on island clear, so correctness wins over the saved scans here: a pool
--- whose cache no longer looks live is rebuilt from scratch. This runs on shuffle
--- triggers only, never per frame.
--- ALWAYS rescans. Two earlier attempts at being clever here were both wrong:
---   1. "data assets are static, scan once"  -> a pool cached during a level transition
---      could hold unloaded objects.
---   2. "only rescan if empty or invalid"    -> WORSE. Assets load LAZILY, so an early
---      scan finds a partial set (observed: WeaponModDA=4 at startup vs 91 in a lobby).
---      A partially-filled pool is neither empty nor invalid, so it never grew and the
---      mod silently rerolled from 4 items instead of 91.
--- This runs on shuffle triggers only - never per frame - so 9 FindAllOf calls is not a
--- cost worth optimising against correctness again.
local function RefreshAllPools()
    for _, def in ipairs(PoolDefs) do
        local before = #Pools[def.key]
        ScanPool(def)
        local after = #Pools[def.key]
        if after > before and before > 0 then
            log("%s grew %d -> %d (assets finished loading)", def.key, before, after)
        end
    end
end

local function GetPool(key)
    return Pools[key] or {}
end

local ResolvedPlayerStateName = nil

local function FindAllPlayerStates()
    if ResolvedPlayerStateName then
        local found = TryFindAllOf(ResolvedPlayerStateName)
        if found then return found end
        ResolvedPlayerStateName = nil -- resolved name stopped working; re-probe below
    end
    for _, name in ipairs(PLAYER_STATE_CANDIDATES) do
        local found = TryFindAllOf(name)
        if found then
            if name ~= PLAYER_STATE_CANDIDATES[1] then
                log("NOTE: '%s' not found, but fallback name '%s' resolved. Please report this.",
                    PLAYER_STATE_CANDIDATES[1], name)
            end
            ResolvedPlayerStateName = name
            return found
        end
    end
    return nil
end

local function HealthReport()
    local okCount, total, parts = 0, #PoolDefs, {}
    for _, def in ipairs(PoolDefs) do
        local n = #Pools[def.key]
        if n > 0 then okCount = okCount + 1 end
        table.insert(parts, string.format("%s=%d", def.key:gsub("^Crab", ""), n))
    end
    if okCount == total then
        log("Health: pools OK %d/%d (%s)", okCount, total, table.concat(parts, " "))
    else
        log("Health: pools %d/%d RESOLVED - some are empty (%s). Empty pools usually mean a game update renamed a class; run 'randomizedump' and check the class-name list.",
            okCount, total, table.concat(parts, " "))
    end
end

-- ===================== Slot definitions =====================

-- ORDER MATTERS: entries 1-5 are index-aligned with ToggleDefs 1-5 for the
-- Shift+Numpad per-slot reroll binds. Append new slot types at the END so that
-- alignment is preserved.
local SlotDefs = {
    { key = "randomizeWeaponMods",  property = "WeaponMods",  daField = "WeaponModDA",  pool = "CrabWeaponModDA" },
    { key = "randomizeAbilityMods", property = "AbilityMods", daField = "AbilityModDA", pool = "CrabAbilityModDA" },
    { key = "randomizeMeleeMods",   property = "MeleeMods",   daField = "MeleeModDA",   pool = "CrabMeleeModDA" },
    { key = "randomizePerks",       property = "Perks",       daField = "PerkDA",       pool = "CrabPerkDA" },
    { key = "randomizeRelics",      property = "Relics",      daField = "RelicDA",      pool = "CrabRelicDA" },
    -- May not exist in this build (grenades might be the "Ability" slot). Safe either
    -- way: an unresolvable pool disables only itself, with a log line saying so.
    { key = "randomizeGrenadeMods", property = "GrenadeMods", daField = "GrenadeModDA", pool = "CrabGrenadeModDA" },
}

-- Per-subsystem degradation: a pool that can't resolve disables only its own slot type,
-- logged once, instead of spamming every pass or taking the whole shuffle down with it.
local DisabledSubsystems = {}

local function SubsystemAvailable(def)
    if #GetPool(def.pool) > 0 then
        if DisabledSubsystems[def.key] then
            DisabledSubsystems[def.key] = nil
            log("%s randomization recovered (%s now has entries)", def.property, def.pool)
        end
        return true
    end
    if not DisabledSubsystems[def.key] then
        DisabledSubsystems[def.key] = true
        log("%s randomization DISABLED for now - no %s data assets found. Everything else keeps working.", def.property, def.pool)
    end
    return false
end

-- ===================== Selection =====================

local function GetRarityWeight(rarity)
    if not rarity then return 1 end
    -- math.floor normalizes the key whether UE4SS returns rarity as a Lua integer or
    -- float subtype: tostring(1) is "1" but tostring(1.0) is "1.0", and only the former
    -- matches a rarityWeightN key. Comparisons elsewhere are subtype-agnostic; only this
    -- string-key lookup is not.
    local w = Config["rarityWeight" .. tostring(math.floor(rarity))]
    if not w or w < 0 then return 1 end
    return w
end

local function UniformPick(candidates)
    return candidates[math.random(1, #candidates)]
end

local function WeightedPick(candidates)
    if Config.rollMode ~= "weighted" then return UniformPick(candidates) end
    local weights, total = {}, 0
    for i, da in ipairs(candidates) do
        local w = GetRarityWeight(da.Rarity)
        weights[i] = w
        total = total + w
    end
    if total <= 0 then return UniformPick(candidates) end
    local roll = math.random() * total
    local cumulative = 0
    for i, w in ipairs(weights) do
        cumulative = cumulative + w
        if roll <= cumulative then return candidates[i] end
    end
    return candidates[#candidates] -- floating point safety net
end

local function BuildBasePool(def)
    local rawPool = GetPool(def.pool)
    if #rawPool == 0 then return nil end

    -- Liveness first: reading .Rarity off an unloaded asset is itself a native fault.
    local livePool = {}
    for _, da in ipairs(rawPool) do
        if IsLive(da) then table.insert(livePool, da) end
    end
    if #livePool == 0 then
        log("Every %s entry is unloaded right now - skipping it this pass rather than risking a stale write.", def.pool)
        return nil
    end

    local basePool = {}
    for _, da in ipairs(livePool) do
        local okR, rarity = pcall(function() return da.Rarity end)
        if okR and (not rarity or rarity >= Config.minimumRarity) then
            table.insert(basePool, da)
        end
    end
    -- Fall back to the LIVE pool, never the raw one - the raw pool may contain dead
    -- objects and this fallback previously let them back into circulation.
    if #basePool == 0 then
        log("minimumRarity=%d filtered out every %s entry - ignoring the floor for this pass", Config.minimumRarity, def.pool)
        basePool = livePool
    end
    return basePool
end

-- ===================== History / undo =====================

local MAX_HISTORY = 50
local History = {}
local Generation = 0

local function SafeDAName(da)
    if not da then return "<none>" end
    local ok, name = pcall(function() return da.Name:ToString() end)
    if ok and name and name ~= "" then return name end
    return "<unknown>"
end

--- restore carries everything randomizeundo needs to put a slot back. Slot identity is
--- (playerKey, property, index) rather than a held userdata reference, because the TArray
--- element wrapper isn't guaranteed to stay valid across frames. The index is whatever
--- ForEach handed us, and undo re-walks with ForEach and matches the same value - so it
--- works whether UE4SS's ForEach index is 0- or 1-based.
local function RecordHistory(entry)
    entry.generation = Generation
    table.insert(History, entry)
    while #History > MAX_HISTORY do table.remove(History, 1) end
end

-- ===================== Player identity =====================

local function CounterKey(ps)
    local ok, name = pcall(function() return ps:GetFullName() end)
    return ok and name or "unknown"
end

-- GetFullName() is stable and unique but unreadable in logs. Map each distinct CrabPS to
-- a short "Player N" label (first-seen order) for display, keyed off the same identity.
local PlayerLabels = {}
local PlayerLabelCount = 0

local function GetPlayerLabel(ps)
    local key = CounterKey(ps)
    if not PlayerLabels[key] then
        PlayerLabelCount = PlayerLabelCount + 1
        PlayerLabels[key] = "Player " .. PlayerLabelCount
    end
    return PlayerLabels[key]
end

local function GetLocalPlayerController()
    if not UEHelpers then return nil end
    local ok, pc = pcall(UEHelpers.GetPlayerController)
    if not ok or not pc or not pc:IsValid() then return nil end
    return pc
end

local function GetLocalPlayerState()
    local pc = GetLocalPlayerController()
    if not pc then return nil end
    local ps = pc.PlayerState
    if ps and ps:IsValid() then return ps end
    return nil
end

local function IsSamePlayerState(a, b)
    if not a or not b then return false end
    local ok, result = pcall(function() return a:GetFullName() == b:GetFullName() end)
    return ok and result
end

-- ===================== Chat announcements =====================
-- UNVERIFIED: CrabPC:ServerSendChatMessage(ChatMessage) is confirmed to EXIST in this
-- build's function list, but the call has not been observed working from Lua. Default is
-- off; if enabling it does nothing (or errors into the log), that's the reason.

local ChatFailedOnce = false

local function AnnounceInChat(message)
    if not Config.announceInChat then return end
    if ChatFailedOnce then return end
    local pc = GetLocalPlayerController()
    if not pc then return end
    local ok, err = pcall(function() pc:ServerSendChatMessage(message) end)
    if not ok then
        ChatFailedOnce = true
        log("Chat announcement failed (this feature is unverified - disabling it for this session): %s", tostring(err))
    end
end

-- ===================== Core slot randomization =====================

--- Reads a CrabPS array property, returning nil (and logging) if it isn't reachable.
local function ReadSlotArray(ps, def)
    local ok, arr = pcall(function() return ps:GetPropertyValue(def.property) end)
    if not ok or arr == nil then
        log("Could not read property %s off a CrabPS: %s", def.property, tostring(arr))
        return nil
    end
    return arr
end

--- Applies `chooser(currentDA, index) -> newDA` across every slot of one array property.
--- chooser returning nil leaves that slot alone. Honors dryRun. Caller must have already
--- confirmed authority.
local function ApplyToSlots(ps, def, playerLabel, chooser)
    if not IsLive(ps) then return 0 end
    local arr = ReadSlotArray(ps, def)
    if not arr then return 0 end

    local changed = 0
    arr:ForEach(function(index, elem)
        local slot = elem:get()
        if slot == nil then return end

        local currentDA = slot[def.daField]
        local chosen = chooser(currentDA, index)
        if chosen == nil then return end

        -- LAST LINE OF DEFENCE: never write a dead object into replicated state. A stale
        -- pointer here is a native access violation, which no pcall can catch, so this
        -- check must happen immediately before the assignment - not just at scan time.
        if not IsLive(chosen) then
            log("Skipped %s[%s] for %s - the chosen item was no longer valid (stale asset).",
                def.property, tostring(index), playerLabel)
            return
        end

        if Config.dryRun then
            log("[dry-run] %s %s[%s]: %s -> %s", playerLabel, def.property, tostring(index),
                SafeDAName(currentDA), SafeDAName(chosen))
        else
            slot[def.daField] = chosen
            RecordHistory({
                player = playerLabel,
                playerKey = CounterKey(ps),
                slot = def.property,
                index = index,
                daField = def.daField,
                beforeDA = currentDA,
                before = SafeDAName(currentDA),
                after = SafeDAName(chosen),
                kind = "slot",
            })
        end
        changed = changed + 1
    end)
    return changed
end

--- Independent mode: each slot rolls its own replacement, avoiding immediate duplicates
--- within this player's pass.
local function RandomizeSlotArray(ps, def, playerLabel)
    if not SubsystemAvailable(def) then return end
    local basePool = BuildBasePool(def)
    if not basePool then return end

    local available = {}
    for _, da in ipairs(basePool) do table.insert(available, da) end

    ApplyToSlots(ps, def, playerLabel, function(currentDA)
        local candidates = available

        if Config.rollMode == "withinRarity" and currentDA and currentDA.Rarity then
            local sameRarity = {}
            for _, da in ipairs(available) do
                if da.Rarity == currentDA.Rarity then table.insert(sameRarity, da) end
            end
            if #sameRarity > 0 then candidates = sameRarity end
        end

        if #candidates == 0 then return nil end

        local chosen = WeightedPick(candidates)
        for i, da in ipairs(available) do
            if da == chosen then table.remove(available, i) break end
        end
        if #available == 0 then
            for _, da in ipairs(basePool) do table.insert(available, da) end
        end
        return chosen
    end)
end

--- Mirror mode: one roll per slot index, applied identically to every player.
--- Note: rollMode=withinRarity is meaningless here (players' current rarities differ),
--- so mirror always rolls from the shared filtered pool.
local function MirrorSlotArray(playerList, def)
    if not SubsystemAvailable(def) then return end
    local basePool = BuildBasePool(def)
    if not basePool then return end

    local plan = {}
    local function planFor(index)
        local k = tostring(index)
        if plan[k] == nil then plan[k] = WeightedPick(basePool) end
        return plan[k]
    end

    for _, entry in ipairs(playerList) do
        ApplyToSlots(entry.ps, def, entry.label, function(_, index)
            return planFor(index)
        end)
    end
end

--- Swap mode: rotate existing loadouts between players. Nothing is rolled - player N
--- receives player N+1's items (wrapping), so with two players they simply trade.
local function SwapSlotArray(playerList, def)
    if not SubsystemAvailable(def) then return end
    if #playerList < 2 then
        log("coopMode=swap needs at least 2 players - skipping %s", def.property)
        return
    end

    -- Snapshot every player's current DAs first, so rotation reads pre-swap state.
    local snapshots = {}
    for i, entry in ipairs(playerList) do
        local snap = {}
        local arr = ReadSlotArray(entry.ps, def)
        if arr then
            arr:ForEach(function(index, elem)
                local slot = elem:get()
                if slot ~= nil then snap[tostring(index)] = slot[def.daField] end
            end)
        end
        snapshots[i] = snap
    end

    for i, entry in ipairs(playerList) do
        local donor = snapshots[(i % #playerList) + 1]
        ApplyToSlots(entry.ps, def, entry.label, function(_, index)
            return donor[tostring(index)]
        end)
    end
end

-- ===================== Base weapon / ability / melee =====================

--- AUTHORITY ONLY. An earlier version also allowed this when the PlayerState was simply
--- the local player's ("hasAuthority or IsSamePlayerState"), so that a client could at
--- least reroll its own gear. That was wrong and it crashed joining clients: on a client
--- this fires ServerEquipInventory as a client->server RPC every island, which the host
--- does not do because it applies locally. Symptom was the host being fine while the
--- joiner crashed on every loadout change. Only the authoritative machine mutates
--- anything now - the same rule the mod/perk/relic path already used. Solo play is
--- unaffected: a solo player IS the authority.
--- Rolls ONE base loadout. Split out of RandomizeBaseLoadout so coopMode=mirror can roll
--- a single result and hand the same one to every player.
---
--- A slot is left nil when that slot is not being randomized; nil means "keep whatever
--- this player already has", which is inherently per-player and so is resolved inside
--- RandomizeBaseLoadout rather than here.
local function RollBaseLoadout()
    local roll = {}

    if Config.randomizeWeapon then
        local validWeapons = {}
        for _, da in ipairs(GetPool("CrabWeaponDA")) do
            local ok, full = pcall(function() return da:GetFullName() end)
            if ok and not string.find(full, "Enemy") then table.insert(validWeapons, da) end
        end
        if #validWeapons > 0 then roll.weapon = UniformPick(validWeapons) end
    end

    if Config.randomizeAbility then
        local pool = GetPool("CrabAbilityDA")
        if #pool > 0 then roll.ability = UniformPick(pool) end
    end

    if Config.randomizeMelee then
        local pool = GetPool("CrabMeleeDA")
        if #pool > 0 then roll.melee = UniformPick(pool) end
    end

    return roll
end

--- `forced` (optional) supplies an already-decided {weapon, ability, melee}. mirror passes
--- one shared roll to everybody; swap passes the donor player's snapshot. Omit it and this
--- rolls independently, which is what independent mode and solo play want.
local function RandomizeBaseLoadout(ps, hasAuthority, localPS, playerLabel, forced)
    if not hasAuthority then
        log("Skipping weapon/ability/melee swap for %s - not authoritative (the HOST applies this for everyone)", playerLabel)
        return
    end

    local beforeWeapon, beforeAbility, beforeMelee = ps.WeaponDA, ps.AbilityDA, ps.MeleeDA
    local weapon, ability, melee = beforeWeapon, beforeAbility, beforeMelee

    local roll = forced or RollBaseLoadout()
    if roll.weapon  ~= nil then weapon  = roll.weapon  end
    if roll.ability ~= nil then ability = roll.ability end
    if roll.melee   ~= nil then melee   = roll.melee   end

    if Config.dryRun then
        log("[dry-run] %s base loadout: %s / %s / %s -> %s / %s / %s", playerLabel,
            SafeDAName(beforeWeapon), SafeDAName(beforeAbility), SafeDAName(beforeMelee),
            SafeDAName(weapon), SafeDAName(ability), SafeDAName(melee))
        return
    end

    -- Same hazard as the slot writes: these go over an RPC into replicated state, so a
    -- stale pointer is a native crash. Bail rather than send anything not currently live.
    if not (IsLive(ps) and IsLive(weapon) and IsLive(ability) and IsLive(melee)) then
        log("Skipped base loadout swap for %s - one of the items was no longer valid (stale asset).", playerLabel)
        return
    end

    local ok, err = pcall(function() ps:ServerEquipInventory(weapon, ability, melee) end)
    if not ok then
        log("ServerEquipInventory failed: %s", tostring(err))
        return
    end

    RecordHistory({
        player = playerLabel,
        playerKey = CounterKey(ps),
        slot = "BaseLoadout",
        kind = "base",
        beforeWeapon = beforeWeapon, beforeAbility = beforeAbility, beforeMelee = beforeMelee,
        before = string.format("%s/%s/%s", SafeDAName(beforeWeapon), SafeDAName(beforeAbility), SafeDAName(beforeMelee)),
        after = string.format("%s/%s/%s", SafeDAName(weapon), SafeDAName(ability), SafeDAName(melee)),
    })
end

-- ===================== Authority iteration =====================

local HasAuthorityUnavailableWarned = false

local function CollectPlayers()
    local allPS = FindAllPlayerStates()
    if not allPS then return nil end

    local list = {}
    for _, ps in ipairs(allPS) do
        if ps and ps:IsValid() then
            local hasAuthOk, hasAuth = pcall(function() return ps:HasAuthority() end)
            if not hasAuthOk then
                hasAuth = true -- fail open rather than disabling the mod entirely
                if not HasAuthorityUnavailableWarned then
                    HasAuthorityUnavailableWarned = true
                    log("WARNING: ps:HasAuthority() is not callable in this build - the co-op authority gate is NOT active and every instance will attempt direct writes. This may desync in co-op. Please report this.")
                end
            end
            table.insert(list, { ps = ps, hasAuth = hasAuth, label = GetPlayerLabel(ps) })
        end
    end
    if #list == 0 then return nil end
    return list
end

-- ===================== Shuffle orchestration =====================


local function RandomizeEveryone()
    local anyMods = false
    for _, def in ipairs(SlotDefs) do
        if Config[def.key] then anyMods = true break end
    end
    local anyBase = Config.randomizeWeapon or Config.randomizeAbility or Config.randomizeMelee

    if not anyMods and not anyBase then
        log("Everything is disabled in config - nothing to randomize (try 'randomizepreset default')")
        return
    end

    local players = CollectPlayers()
    if not players then
        log("No player states found - nothing to randomize")
        return
    end

    Generation = Generation + 1

    -- DELIBERATELY not calling GetLocalPlayerState() here any more. It routes into
    -- UEHelpers.GetPlayerController(), which does `Controller.Pawn:IsValid()` WITHOUT
    -- checking whether Pawn is null first. During an island transition the pawn is
    -- destroyed and respawned, so Pawn is transiently null and that call dereferences
    -- null - a read at offset 0x8, which is exactly the access violation seen in a real
    -- crash dump, inside UE4SS.dll. No pcall can catch a native fault. Nothing below
    -- needs this value since base-loadout swaps became authority-only.
    local localPS = nil

    local authoritative = {}
    for _, entry in ipairs(players) do
        if entry.hasAuth then
            table.insert(authoritative, entry)
        else
            log("Not authoritative over %s - skipping its mod/perk/relic rewrite (the HOST should run this mod for it to apply to everyone)", entry.label)
        end
    end

    -- mirror/swap are no-ops with one player, and the usual cause of "mirror isn't
    -- working" is the second player never reaching this list. Say so plainly.
    if Config.coopMode ~= "independent" then
        log("coopMode=%s: %d player(s) found, %d authoritative",
            Config.coopMode, #players, #authoritative)
        if #authoritative < 2 then
            log("  -> only %d player(s) can be written to, so %s has nothing to act on. "
                .. "Run this on the HOST; a joining client is never authoritative.",
                #authoritative, Config.coopMode)
        end
    end

    if anyMods and #authoritative > 0 then
        if Config.coopMode == "mirror" then
            for _, def in ipairs(SlotDefs) do
                if Config[def.key] then MirrorSlotArray(authoritative, def) end
            end
        elseif Config.coopMode == "swap" then
            for _, def in ipairs(SlotDefs) do
                if Config[def.key] then SwapSlotArray(authoritative, def) end
            end
        else
            for _, entry in ipairs(authoritative) do
                for _, def in ipairs(SlotDefs) do
                    if Config[def.key] then RandomizeSlotArray(entry.ps, def, entry.label) end
                end
            end
        end
    end

    -- Iterate `authoritative`, not `players`: a non-authoritative client must not touch
    -- base loadouts at all (see RandomizeBaseLoadout - doing so crashed joining clients).
    --
    -- This branch used to roll independently for every player REGARDLESS of coopMode, so
    -- mirror and swap only ever applied to mods/perks/relics. The weapon and melee are the
    -- most visible things a player has, so mirror looked completely broken even though the
    -- slot arrays behind it were mirroring correctly.
    if anyBase then
        if Config.coopMode == "mirror" then
            local shared = RollBaseLoadout()
            for _, entry in ipairs(authoritative) do
                RandomizeBaseLoadout(entry.ps, entry.hasAuth, localPS, entry.label, shared)
            end

        elseif Config.coopMode == "swap" then
            if #authoritative < 2 then
                log("coopMode=swap needs at least 2 players - skipping base loadout")
            else
                -- Snapshot everyone BEFORE writing anything, or player 2 would receive
                -- what player 1 was just given rather than what they originally had.
                -- Only carry the slots that are actually enabled; a disabled slot stays
                -- nil so the receiving player keeps their own.
                local snaps = {}
                for i, entry in ipairs(authoritative) do
                    local snap = {}
                    if Config.randomizeWeapon  then snap.weapon  = entry.ps.WeaponDA  end
                    if Config.randomizeAbility then snap.ability = entry.ps.AbilityDA end
                    if Config.randomizeMelee   then snap.melee   = entry.ps.MeleeDA   end
                    snaps[i] = snap
                end
                for i, entry in ipairs(authoritative) do
                    RandomizeBaseLoadout(entry.ps, entry.hasAuth, localPS, entry.label,
                                         snaps[(i % #authoritative) + 1])
                end
            end

        else
            for _, entry in ipairs(authoritative) do
                RandomizeBaseLoadout(entry.ps, entry.hasAuth, localPS, entry.label)
            end
        end
    end

    if Config.dryRun then
        log("[dry-run] no changes were actually applied (set dryRun=false to arm it)")
    else
        local modeNote = (Config.coopMode ~= "independent") and (" [" .. Config.coopMode .. "]") or ""
        AnnounceInChat("[CrabRandomizer] Loadouts shuffled!" .. modeNote)
    end
end

local function RerollSlotOnly(def)
    log("Manually rerolling just %s", def.property)
    local players = CollectPlayers()
    if not players then
        log("No player states found - nothing to do")
        return
    end

    Generation = Generation + 1

    local authoritative = {}
    for _, entry in ipairs(players) do
        if entry.hasAuth then
            table.insert(authoritative, entry)
        else
            log("Not authoritative over %s - skipping", entry.label)
        end
    end
    if #authoritative == 0 then return end

    if Config.coopMode == "mirror" then
        MirrorSlotArray(authoritative, def)
    elseif Config.coopMode == "swap" then
        SwapSlotArray(authoritative, def)
    else
        for _, entry in ipairs(authoritative) do
            RandomizeSlotArray(entry.ps, def, entry.label)
        end
    end
end

-- ===================== Undo =====================

local function UndoLastShuffle()
    if #History == 0 then
        log("Nothing to undo - no shuffles recorded this session")
        return
    end

    local targetGen = History[#History].generation
    local batch = {}
    for i = #History, 1, -1 do
        if History[i].generation == targetGen then
            table.insert(batch, History[i])
        else
            break
        end
    end
    if #batch == 0 then
        log("Nothing to undo")
        return
    end

    local players = CollectPlayers()
    if not players then
        log("Cannot undo - no player states found right now")
        return
    end

    local byKey = {}
    for _, entry in ipairs(players) do byKey[CounterKey(entry.ps)] = entry end

    -- Group slot restores by (player, property) so each array is walked once.
    local groups = {}
    local baseRestores = {}
    for _, rec in ipairs(batch) do
        if rec.kind == "base" then
            table.insert(baseRestores, rec)
        else
            local gk = rec.playerKey .. "|" .. rec.slot
            groups[gk] = groups[gk] or { playerKey = rec.playerKey, property = rec.slot, daField = rec.daField, items = {} }
            groups[gk].items[tostring(rec.index)] = rec.beforeDA
        end
    end

    local restored, failed = 0, 0

    for _, group in pairs(groups) do
        local entry = byKey[group.playerKey]
        if not entry then
            failed = failed + 1
        elseif not entry.hasAuth then
            log("Cannot undo %s for %s - not authoritative", group.property, entry.label)
            failed = failed + 1
        else
            local arr = ReadSlotArray(entry.ps, { property = group.property })
            if not arr then
                failed = failed + 1
            else
                arr:ForEach(function(index, elem)
                    local want = group.items[tostring(index)]
                    if want == nil then return end
                    local slot = elem:get()
                    if slot == nil then return end
                    -- History entries are captured from an earlier shuffle, so they are
                    -- the MOST likely thing to have gone stale. Never restore a dead one.
                    if not IsLive(want) then
                        failed = failed + 1
                        return
                    end
                    slot[group.daField] = want
                    restored = restored + 1
                end)
            end
        end
    end

    for _, rec in ipairs(baseRestores) do
        local entry = byKey[rec.playerKey]
        if entry and IsLive(entry.ps) and IsLive(rec.beforeWeapon)
                 and IsLive(rec.beforeAbility) and IsLive(rec.beforeMelee) then
            local ok = pcall(function()
                entry.ps:ServerEquipInventory(rec.beforeWeapon, rec.beforeAbility, rec.beforeMelee)
            end)
            if ok then restored = restored + 1 else failed = failed + 1 end
        else
            failed = failed + 1
        end
    end

    -- Drop the reverted batch so a second undo walks back a further shuffle.
    for _ = 1, #batch do table.remove(History, #History) end

    if failed > 0 then
        log("Undo of shuffle #%d: restored %d slot(s), %d could not be restored", targetGen, restored, failed)
    else
        log("Undo of shuffle #%d complete - restored %d slot(s)", targetGen, restored)
    end
end

-- ===================== Trigger =====================

-- Counts this machine's own island clears. Deliberately NOT keyed by player: deriving a
-- key means touching CrabPC.PlayerState and calling GetFullName() on it, and doing that
-- inside the hook is what crashed clients (see below). ClientOnClearedIsland is a Client
-- RPC that fires once per machine anyway, so a single counter is equivalent here.
local IslandCount = 0

local function OnIslandCleared()
    IslandCount = IslandCount + 1
    log("Island cleared (%d/%d before next shuffle)", IslandCount, Config.islandsBeforeRandomizing)

    if IslandCount < Config.islandsBeforeRandomizing then return end
    IslandCount = 0

    -- DO NOT randomize synchronously inside this hook. ClientOnClearedIsland fires
    -- during level teardown, while pawns/actors are being destroyed and respawned -
    -- touching player state in that window is what crashes clients. The original
    -- Crab Randomizer had the same island-clear trigger and deferred its work by 3s,
    -- which is a strong hint the author hit this too. Wait until the transition has
    -- settled, then shuffle.
    local delay = Config.shuffleDelayMs
    if delay <= 0 then
        RefreshAllPools()
        RandomizeEveryone()
        return
    end

    log("Island target reached - shuffling in %dms (after the transition settles)", delay)
    ExecuteWithDelay(delay, function()
        local ok, err = pcall(function()
            RefreshAllPools()
            RandomizeEveryone()
        end)
        if not ok then log("Deferred shuffle failed: %s", tostring(err)) end
    end)
end

-- CRITICAL: this callback must touch NO game objects.
--
-- Evidence from a real crash: a client's log contained ZERO "cleared an island" lines
-- yet the game crashed on every island clear, with an access violation inside UE4SS.dll
-- (twice, 37 bytes apart in the same function; bad addresses 0x8 and 0xFFFFFFFFFFFFFFFF).
-- The handler was dying on object access BEFORE it could log anything.
--
-- ClientOnClearedIsland fires during level teardown, when the pawn and PlayerState are
-- being destroyed. Reading Context:get().PlayerState, or calling :IsValid()/:GetFullName()
-- on it, dereferences freed or null memory - a native fault no pcall can catch. Deferring
-- the SHUFFLE was not enough, because the identity lookup still ran inline.
--
-- So: do not call Context:get(). Do not read PlayerState. Just count and schedule.
RegisterHook("/Script/CrabChampions.CrabPC:ClientOnClearedIsland", function(Context, bWasFlawlessClear)
    local ok, err = pcall(OnIslandCleared)
    if not ok then log("Error handling island-clear: %s", tostring(err)) end
end)

-- ===================== Console commands =====================

local function Command(name, handler)
    RegisterConsoleCommandHandler(name, function(cmd, parts, ar)
        -- Route log() into the in-game console for the duration of this command, then
        -- clear it so background/hook logging doesn't touch a stale output device.
        CurrentAr = ar
        local ok, err = pcall(handler, parts or {})
        if not ok then log("%s failed: %s", name, tostring(err)) end
        CurrentAr = nil
        return true
    end)
end

Command("randomizenow", function()
    log("Manual randomize triggered via console command")
    RefreshAllPools()
    RandomizeEveryone()
end)

Command("randomizeundo", function()
    UndoLastShuffle()
end)

Command("randomizestatus", function()
    log("version = %s", MOD_VERSION)
    for _, key in ipairs(CONFIG_KEY_ORDER) do
        log("%s = %s", key, tostring(Config[key]))
    end
    RefreshAllPools()
    for _, def in ipairs(PoolDefs) do
        log("Pool %s [%s] has %d entries", def.key,
            ResolvedPoolNames[def.key] or "UNRESOLVED", #Pools[def.key])
    end
    HealthReport()
end)

Command("randomizeauthority", function()
    local players = CollectPlayers()
    if not players then
        log("No player states found - are you in a run?")
        return
    end
    local localPS = GetLocalPlayerState()
    log("Authority report (%d player state(s) visible):", #players)
    for _, entry in ipairs(players) do
        local isLocal = IsSamePlayerState(entry.ps, localPS)
        log("  %s: authority=%s local=%s", entry.label, tostring(entry.hasAuth), tostring(isLocal))
    end
    local anyAuth = false
    for _, entry in ipairs(players) do
        if entry.hasAuth then anyAuth = true break end
    end
    if anyAuth then
        log("  -> This instance CAN apply changes. If you are hosting, shuffles affect everyone listed above.")
    else
        log("  -> This instance has authority over NOTHING. You are a joining client; the HOST needs to run the mod.")
    end
end)

Command("randomizeset", function(parts)
    local key, value = parts[1], parts[2]
    if not key or not value or Config[key] == nil then
        local keys = {}
        for _, k in ipairs(CONFIG_KEY_ORDER) do table.insert(keys, k) end
        log("Usage: randomizeset <key> <value>")
        log("Valid keys: %s", table.concat(keys, ", "))
        return
    end

    local current = Config[key]
    if type(current) == "boolean" then
        Config[key] = (value:lower() == "true")
    elseif type(current) == "number" then
        local n = tonumber(value)
        if not n then
            log("'%s' is not a number", value)
            return
        end
        Config[key] = math.floor(n)
        if key == "randomSeed" then ApplySeed() end
    else
        Config[key] = value
    end

    ValidateConfig()
    log("%s = %s", key, tostring(Config[key]))
    SaveConfig()

    -- Turning the overlay on used to need a game restart, because the module was only
    -- loaded during startup. Load it here too so `randomizeset overlay true` connects
    -- Ctrl+K immediately. TryLoadOverlay is a global defined further down; globals resolve
    -- when this runs, not when it is defined, so the ordering is fine.
    if key == "overlay" then
        if Config.overlay and Overlay == nil then
            if TryLoadOverlay and TryLoadOverlay() then
                log("Overlay is live - press Ctrl+K now. Arrows move, enter applies.")
            else
                log("Overlay could not be started. Ctrl+K still opens the text menu.")
            end
        elseif not Config.overlay and Overlay ~= nil then
            -- Can't unregister a UE4SS hook, so close it and stop drawing. The hook stays
            -- resident but early-outs on the first comparison.
            if Overlay.IsOpen() then Overlay.OnKey("close") end
            Overlay = nil
            log("Overlay switched off. Ctrl+K returns to the text menu on next launch.")
        end
    end
end)

Command("randomizepreset", function(parts)
    local name = parts[1]
    if not name or not Presets[name] then
        log("Usage: randomizepreset <name>")
        log("Presets: %s", table.concat(PRESET_ORDER, ", "))
        log("  off     - disable all randomization")
        log("  gentle  - mods/perks/relics only, same-rarity rerolls, every 6 islands")
        log("  default - mods/perks/relics only, weighted rerolls, every 6 islands")
        log("  chaos   - everything including base weapon/ability/melee, every island, uniform")
        log("  mirror  - co-op: both players get the SAME rolled loadout, every 3 islands")
        log("  swap    - co-op: players TRADE loadouts with each other, every 3 islands")
        return
    end
    ApplyPreset(name)
    log("Applied preset '%s'", name)
    for _, key in ipairs(CONFIG_KEY_ORDER) do
        log("%s = %s", key, tostring(Config[key]))
    end
    SaveConfig()
end)

Command("randomizeonce", function(parts)
    local key = parts[1]
    if not key then
        local keys = {}
        for _, def in ipairs(SlotDefs) do table.insert(keys, def.key) end
        log("Usage: randomizeonce <key> - rerolls just that slot type regardless of its config toggle")
        log("Valid keys: %s", table.concat(keys, ", "))
        return
    end
    for _, def in ipairs(SlotDefs) do
        if def.key == key then
            RefreshAllPools()
            RerollSlotOnly(def)
            return
        end
    end
    log("'%s' isn't a mod/perk/relic slot key (weapon/ability/melee base swaps aren't single-slot rerolls)", key)
end)

Command("randomizelist", function(parts)
    local which = parts[1]
    RefreshAllPools()

    if not which then
        log("Usage: randomizelist <pool>")
        local keys = {}
        for _, def in ipairs(PoolDefs) do table.insert(keys, def.key) end
        log("Pools: %s", table.concat(keys, ", "))
        log("Shortcuts: weaponmods, abilitymods, meleemods, perks, relics, weapons, abilities, melees")
        return
    end

    local aliases = {
        weaponmods = "CrabWeaponModDA", abilitymods = "CrabAbilityModDA",
        meleemods = "CrabMeleeModDA", perks = "CrabPerkDA", relics = "CrabRelicDA",
        weapons = "CrabWeaponDA", abilities = "CrabAbilityDA", melees = "CrabMeleeDA",
    }
    local poolKey = aliases[which:lower()] or which
    local pool = Pools[poolKey]
    if not pool then
        log("Unknown pool '%s'", which)
        return
    end
    if #pool == 0 then
        log("Pool %s is empty", poolKey)
        return
    end

    log("%s (%d entries, resolved as '%s'):", poolKey, #pool, ResolvedPoolNames[poolKey] or "?")
    for i, da in ipairs(pool) do
        local rarity = "?"
        local okR, r = pcall(function() return da.Rarity end)
        if okR and r then rarity = tostring(math.floor(r)) end
        log("  [%d] %s (rarity %s)", i, SafeDAName(da), rarity)
    end
end)

Command("randomizehistory", function(parts)
    if #History == 0 then
        log("No shuffles recorded yet this session")
        return
    end
    -- count must be a non-negative integer: math.floor guards against fractional input
    -- producing fractional table indices (History[47.5] is nil), and the clamp stops a
    -- negative count from printing a nonsensical negative header.
    local count = math.floor(tonumber(parts[1]) or 15)
    if count < 0 then count = 0 end
    local start = math.max(1, #History - count + 1)
    log("Last %d change(s) (of %d recorded, max %d kept):", (#History - start + 1), #History, MAX_HISTORY)
    for i = start, #History do
        local h = History[i]
        log("  [#%d] %s %s: %s -> %s", h.generation or 0, h.player, h.slot, h.before, h.after)
    end
    log("Use 'randomizeundo' to revert the most recent shuffle (#%d).", History[#History].generation or 0)
end)

Command("randomizedump", function()
    RefreshAllPools()
    local base = ResolvedBasePath or BASE_PATHS[1]
    local path = base .. "crabrandomizer-dump.txt"
    local f = io.open(path, "w")
    if not f then
        log("Could not write dump to %s", path)
        return
    end

    f:write("CrabRandomizer diagnostic dump\n")
    f:write("version = " .. MOD_VERSION .. "\n")
    f:write("config base path = " .. tostring(ResolvedBasePath) .. "\n\n")

    f:write("[config]\n")
    for _, key in ipairs(CONFIG_KEY_ORDER) do
        f:write(string.format("%s=%s\n", key, tostring(Config[key])))
    end

    f:write("\n[class resolution]\n")
    f:write(string.format("PlayerState resolved as: %s\n", tostring(ResolvedPlayerStateName)))
    for _, def in ipairs(PoolDefs) do
        f:write(string.format("%s -> %s (%d entries)\n", def.key,
            tostring(ResolvedPoolNames[def.key]), #Pools[def.key]))
    end

    f:write("\n[disabled subsystems]\n")
    local anyDisabled = false
    for k in pairs(DisabledSubsystems) do
        f:write(k .. "\n")
        anyDisabled = true
    end
    if not anyDisabled then f:write("(none)\n") end

    f:write("\n[history]\n")
    if #History == 0 then
        f:write("(empty)\n")
    else
        for i, h in ipairs(History) do
            f:write(string.format("[%d] gen#%s %s %s: %s -> %s\n", i, tostring(h.generation),
                tostring(h.player), tostring(h.slot), tostring(h.before), tostring(h.after)))
        end
    end

    f:close()
    log("Wrote diagnostic dump to %s - attach this file when reporting a problem.", path)
end)

-- ===================== In-game chat commands =====================
--
-- The closest thing to an in-game menu that pure Lua can actually reach. UE4SS exposes
-- no ImGui/drawing bindings to Lua (upstream issue #1072, still open), and a custom UMG
-- overlay needs a cooked widget asset built in the Unreal Editor with the game's modkit.
-- What IS hookable is the game's own chat box - CrabGameStateUI:OnChatTextCommitted -
-- so commands can be typed in-game without touching the F10 console at all.
--
-- Type e.g.  !rand help  in chat.

local function ChatReply(fmt, ...)
    local ok, msg = pcall(string.format, fmt, ...)
    if not ok then msg = tostring(fmt) end
    log("%s", msg) -- always goes to console/log
    -- Also try to echo into chat so it's visible in-game. Unverified API; if it fails
    -- once we stop trying rather than spamming errors every command.
    if ChatFailedOnce then return end
    local pc = GetLocalPlayerController()
    if not pc then return end
    local sent = pcall(function() pc:ServerSendChatMessage("[Rand] " .. msg) end)
    if not sent then ChatFailedOnce = true end
end

local ChatHandlers = {
    help = function()
        ChatReply("Commands: %s now | undo | status | auth | history | preset <name> | set <key> <value>",
            Config.chatPrefix)
        ChatReply("Presets: %s", table.concat(PRESET_ORDER, ", "))
    end,
    now = function()
        ChatReply("Rerolling now...")
        RefreshAllPools()
        RandomizeEveryone()
    end,
    undo = function() UndoLastShuffle() end,
    status = function()
        ChatReply("v%s | rollMode=%s coopMode=%s islands=%d minRarity=%d dryRun=%s",
            MOD_VERSION, Config.rollMode, Config.coopMode,
            Config.islandsBeforeRandomizing, Config.minimumRarity, tostring(Config.dryRun))
    end,
    auth = function()
        local players = CollectPlayers()
        if not players then ChatReply("No players found.") return end
        local mine = 0
        for _, e in ipairs(players) do if e.hasAuth then mine = mine + 1 end end
        if mine > 0 then
            ChatReply("This game CAN apply changes to %d/%d player(s).", mine, #players)
        else
            ChatReply("This game has authority over NOTHING - the host must run the mod.")
        end
    end,
    history = function()
        if #History == 0 then ChatReply("No shuffles yet.") return end
        local shown = math.min(5, #History)
        ChatReply("Last %d change(s):", shown)
        for i = #History - shown + 1, #History do
            local h = History[i]
            ChatReply("  %s: %s -> %s", h.slot, h.before, h.after)
        end
    end,
    preset = function(arg)
        local name = (arg or ""):match("^%S*")
        if not name or name == "" or not Presets[name] then
            ChatReply("Presets: %s", table.concat(PRESET_ORDER, ", "))
            return
        end
        ApplyPreset(name)
        SaveConfig()
        ChatReply("Applied preset '%s' (islands=%d, rollMode=%s, coopMode=%s)",
            name, Config.islandsBeforeRandomizing, Config.rollMode, Config.coopMode)
    end,
    set = function(arg)
        local key, value = (arg or ""):match("^(%S+)%s+(%S+)")
        if not key or Config[key] == nil then
            ChatReply("Usage: %s set <key> <value>", Config.chatPrefix)
            return
        end
        local cur = Config[key]
        if type(cur) == "boolean" then
            Config[key] = (value:lower() == "true")
        elseif type(cur) == "number" then
            local n = tonumber(value)
            if not n then ChatReply("'%s' is not a number", value) return end
            Config[key] = math.floor(n)
            if key == "randomSeed" then ApplySeed() end
        else
            Config[key] = value
        end
        ValidateConfig()
        SaveConfig()
        ChatReply("%s = %s", key, tostring(Config[key]))
    end,
}

--- Returns true if the text was one of our commands (so it was handled here).
local function HandleChatText(raw)
    if not Config.chatCommands then return false end
    local text = tostring(raw or "")
    text = text:gsub("^%s+", ""):gsub("%s+$", "")
    if text == "" then return false end

    local prefix = Config.chatPrefix
    if text:sub(1, #prefix):lower() ~= prefix:lower() then return false end

    local rest = text:sub(#prefix + 1):gsub("^%s+", "")
    local cmd, arg = rest:match("^(%S*)%s*(.*)$")
    cmd = (cmd or ""):lower()
    if cmd == "" then cmd = "help" end

    local handler = ChatHandlers[cmd]
    if not handler then
        ChatReply("Unknown command '%s'. Try: %s help", cmd, prefix)
        return true
    end
    handler(arg)
    return true
end

do
    local ok, err = pcall(function()
        RegisterHook("/Script/CrabChampions.CrabGameStateUI:OnChatTextCommitted",
            function(Context, Text, CommitMethod)
                local handled, herr = pcall(function()
                    local t = Text and Text:get()
                    if t == nil then return end
                    -- Chat text is usually an FText; fall back to tostring if not.
                    local s
                    local okStr = pcall(function() s = t:ToString() end)
                    if not okStr or s == nil then s = tostring(t) end
                    HandleChatText(s)
                end)
                if not handled then log("Chat command error: %s", tostring(herr)) end
            end)
    end)
    if not ok then
        log("Could not hook the chat box (in-game chat commands unavailable): %s", tostring(err))
    end
end

-- ===================== C++ UI bridge =====================
--
-- Optional companion mod (CrabRandomizerUI) draws a real ImGui menu, which Lua cannot do
-- itself. It talks to us through files rather than any UE4SS-internal interop, so a
-- UE4SS update can't silently break the link and neither mod needs the other to exist:
--   randoconfig.txt  settings (already our own format)
--   uicommand.txt    a one-line action we consume and delete
--
-- Polling only starts if the command file appears, so there is no cost when the C++ mod
-- isn't installed.

local UI_COMMAND_FILE = "uicommand.txt"

local function ReadAndClearUICommand()
    local base = ResolvedBasePath or BASE_PATHS[1]
    local path = base .. UI_COMMAND_FILE
    local f = io.open(path, "r")
    if not f then return nil end
    local line = f:read("*l")
    f:close()
    -- Consume it so the action fires exactly once.
    os.remove(path)
    if line == nil then return nil end
    return (line:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function HandleUICommand(cmd)
    if cmd == nil or cmd == "" then return end
    local verb, arg = cmd:match("^(%S+)%s*(.*)$")
    verb = (verb or ""):lower()

    if verb == "now" then
        log("UI: reroll requested")
        RefreshAllPools()
        RandomizeEveryone()
    elseif verb == "undo" then
        log("UI: undo requested")
        UndoLastShuffle()
    elseif verb == "reload" then
        LoadConfig()
        ApplySeed()
        log("UI: config reloaded (islands=%d rollMode=%s coopMode=%s)",
            Config.islandsBeforeRandomizing, Config.rollMode, Config.coopMode)
    elseif verb == "preset" then
        local name = arg:match("^%S*")
        if name and Presets[name] then
            ApplyPreset(name)
            SaveConfig()
            log("UI: applied preset '%s'", name)
        else
            log("UI: unknown preset '%s'", tostring(name))
        end
    else
        log("UI: unknown command '%s'", tostring(verb))
    end
end

if Config.uiBridge then
    local ok = pcall(function()
        LoopAsync(500, function()
            local okPoll, err = pcall(function()
                local cmd = ReadAndClearUICommand()
                if cmd then HandleUICommand(cmd) end
            end)
            if not okPoll then log("UI bridge error: %s", tostring(err)) end
            return false -- keep looping
        end)
    end)
    if not ok then log("Could not start the UI bridge poll loop (C++ menu will not work)") end
end

-- ===================== Keybinds =====================

-- Registration is pcall'd, but that only covers setup. SafeCallback gives each callback
-- the same per-invocation protection the console commands have, since an error thrown on
-- key press happens long after the registration pcall has returned.
local function SafeCallback(fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then log("Keybind handler error: %s", tostring(err)) end
    end
end

local function ResolveKey(name, fallbackName)
    if name and name ~= "" and Key[name] ~= nil then return Key[name], name end
    if name and name ~= "" then
        log("Key '%s' is not a valid UE4SS key name - falling back to %s", tostring(name), fallbackName)
    end
    return Key[fallbackName], fallbackName
end

local function ResolveModifier(name)
    if not name or name == "" or name:lower() == "none" then return nil end
    if ModifierKey[name] ~= nil then return ModifierKey[name] end
    log("Modifier '%s' is not valid (CONTROL/ALT/SHIFT/NONE) - ignoring it", tostring(name))
    return nil
end

local function Bind(keyName, modName, fallbackKeyName, callback)
    local keyCode = select(1, ResolveKey(keyName, fallbackKeyName))
    local mod = ResolveModifier(modName)
    if mod then
        RegisterKeyBind(keyCode, { mod }, SafeCallback(callback))
    else
        RegisterKeyBind(keyCode, SafeCallback(callback))
    end
end

local MenuOpen = false

-- ToggleDefs[1..5] must stay index-aligned with SlotDefs[1..5] for the Shift+Numpad
-- per-slot rerolls. `label` exists because def.num:gsub("NUM_","") renders "ONE", not
-- "1", which reads badly in the menu.
local ToggleDefs = {
    { num = "NUM_ONE",   label = "1", key = "randomizeWeaponMods" },
    { num = "NUM_TWO",   label = "2", key = "randomizeAbilityMods" },
    { num = "NUM_THREE", label = "3", key = "randomizeMeleeMods" },
    { num = "NUM_FOUR",  label = "4", key = "randomizePerks" },
    { num = "NUM_FIVE",  label = "5", key = "randomizeRelics" },
    { num = "NUM_SIX",   label = "6", key = "randomizeWeapon" },
    { num = "NUM_SEVEN", label = "7", key = "randomizeAbility" },
    { num = "NUM_EIGHT", label = "8", key = "randomizeMelee" },
}

local function PrintMenu()
    log("========== CrabRandomizer v%s quick menu (%s to close, auto-closes in 8s) ==========", MOD_VERSION, Config.keyQuickMenu)
    for _, def in ipairs(ToggleDefs) do
        log("  [Numpad %s] Toggle %s -> currently %s",
            def.label, def.key, tostring(Config[def.key]))
    end
    log("  [Numpad 9] Force randomize now      [Numpad 0] Show full status")
    log("  [Shift+Numpad 1-5] Reroll ONLY weaponmods/abilitymods/meleemods/perks/relics")
    log("  [Numpad .] Undo the last shuffle")
    -- No free numpad key for grenade mods, so surface its state here rather than hiding it.
    log("  randomizeGrenadeMods = %s  (no numpad slot; use: randomizeset randomizeGrenadeMods true/false)",
        tostring(Config.randomizeGrenadeMods))
    log("  rollMode=%s  coopMode=%s  islands=%d  minRarity=%d  seed=%d  dryRun=%s",
        Config.rollMode, Config.coopMode, Config.islandsBeforeRandomizing,
        Config.minimumRarity, Config.randomSeed, tostring(Config.dryRun))
    log("  weights=%d/%d/%d/%d  (console: randomizepreset / randomizeundo / randomizelist / randomizedump / randomizeauthority)",
        Config.rarityWeight1, Config.rarityWeight2, Config.rarityWeight3, Config.rarityWeight4)
    log("=====================================================================================")
end

-- ===================== drawn overlay (optional) =====================
--
-- overlay.lua hooks AHUD:ReceiveDrawHUD and paints a menu over the game. It is loaded
-- through pcall and behind a config flag so that neither a missing file, a syntax error,
-- nor a failed hook registration can stop the randomizer from working. Overlay stays nil
-- in every one of those cases and every call site below checks it.

Overlay = nil

--- Global on purpose: the randomizeset handler above is defined earlier in the file and
--- calls this by name at runtime, so Ctrl+K can be connected without restarting the game.
--- Returns true if the overlay is now live.
function TryLoadOverlay()
    if Overlay ~= nil then return true end

    local okLoad, mod = pcall(require, "overlay")
    if not okLoad then
        log("Overlay could not be loaded (the randomizer is unaffected): %s", tostring(mod))
        return false
    else
        -- Deliberately a narrow, explicit surface rather than handing overlay.lua the
        -- whole Config table and the internal functions. It can only do these things.
        local api = {
            version = function() return MOD_VERSION end,
            log     = log,
            get     = function(k) return Config[k] end,
            set     = function(k, v)
                Config[k] = v
                ValidateConfig()
                SaveConfig()
            end,
            presets = function() return PRESET_ORDER end,
            applyPreset = function(name) ApplyPreset(name) end,
            toggles = function()
                local out = {}
                for _, d in ipairs(ToggleDefs) do
                    -- Reuse the numpad labels already defined for the text menu so the two
                    -- never drift apart.
                    out[#out + 1] = { key = d.key, label = (d.key:gsub("^randomize", "")) }
                end
                return out
            end,
            randomizeNow = function() RefreshAllPools(); RandomizeEveryone() end,
            undo         = function() UndoLastShuffle() end,
            status       = function()
                for _, k in ipairs(CONFIG_KEY_ORDER) do log("%s = %s", k, tostring(Config[k])) end
            end,
            authoritySummary = function()
                local players = CollectPlayers()
                if not players then return "no players" end
                local auth = 0
                for _, e in ipairs(players) do if e.hasAuth then auth = auth + 1 end end
                return string.format("%d players / %d auth", #players, auth)
            end,
        }

        local okInit, errInit = mod.Init(api)
        if okInit then
            Overlay = mod
            log("Overlay ready - press Ctrl+K in game to open it")
            return true
        else
            log("Overlay hook failed (the randomizer is unaffected): %s", tostring(errInit))
            return false
        end
    end
end

if Config.overlay then TryLoadOverlay() end

local okMenu, errMenu = pcall(function()
    Bind(Config.keyQuickMenu, Config.keyQuickMenuMod, "K", function()
        -- With the drawn overlay active, Ctrl+K drives IT and the text quick-menu stays
        -- out of the way. Falling through to the log-printed menu as well would mean
        -- every keypress spammed the console behind the panel.
        if Overlay then
            Overlay.OnKey("toggle")
            log("Overlay %s", Overlay.IsOpen() and "opened" or "closed")
            return
        end

        MenuOpen = not MenuOpen
        if MenuOpen then
            PrintMenu()
            ExecuteWithDelay(8000, function()
                if MenuOpen then
                    MenuOpen = false
                    log("Quick menu auto-closed (timed out)")
                end
            end)
        else
            log("Quick menu closed.")
        end
    end)

    -- Arrow keys drive the drawn overlay.
    --
    -- Bound unconditionally rather than only when the overlay loaded at startup: the
    -- overlay can now be switched on mid-session with `randomizeset overlay true`, and
    -- keybinds can only be registered once, here. Gating this on Overlay would have given
    -- a menu that opens but cannot be navigated - which is exactly the kind of half-wired
    -- feature that looks like a crash to whoever is holding the controller.
    --
    -- Cost when unused is nil-check plus a boolean, and the game still receives the key:
    -- UE4SS keybinds observe input, they do not consume it.
    local nav = { UP = "up", DOWN = "down", LEFT = "left", RIGHT = "right", ENTER = "enter" }
    for keyName, action in pairs(nav) do
        local code = Key[keyName]
        if code then
            RegisterKeyBind(code, SafeCallback(function()
                if Overlay and Overlay.IsOpen() then Overlay.OnKey(action) end
            end))
        end
    end

    -- Standalone reroll that does NOT require opening the menu first.
    Bind(Config.keyRerollNow, Config.keyRerollNowMod, "R", function()
        log("Direct reroll hotkey pressed")
        RefreshAllPools()
        RandomizeEveryone()
    end)

    for _, def in ipairs(ToggleDefs) do
        RegisterKeyBind(Key[def.num], SafeCallback(function()
            if not MenuOpen then return end
            MenuOpen = false
            Config[def.key] = not Config[def.key]
            log("%s is now %s", def.key, tostring(Config[def.key]))
            SaveConfig()
        end))
    end

    -- ToggleDefs[1..5] and SlotDefs[1..5] are the same five slot types in the same order,
    -- so Shift+the same numpad key rerolls just that one slot, independent of its toggle.
    for i = 1, 5 do
        local def = SlotDefs[i]
        RegisterKeyBind(Key[ToggleDefs[i].num], { ModifierKey.SHIFT }, SafeCallback(function()
            if not MenuOpen then return end
            MenuOpen = false
            RefreshAllPools()
            RerollSlotOnly(def)
        end))
    end

    RegisterKeyBind(Key.NUM_NINE, SafeCallback(function()
        if not MenuOpen then return end
        MenuOpen = false
        log("Manual randomize triggered from quick menu")
        RefreshAllPools()
        RandomizeEveryone()
    end))

    RegisterKeyBind(Key.NUM_ZERO, SafeCallback(function()
        if not MenuOpen then return end
        MenuOpen = false
        for _, key in ipairs(CONFIG_KEY_ORDER) do log("%s = %s", key, tostring(Config[key])) end
    end))

    -- Numpad period is called DECIMAL in UE4SS's Key table, not NUM_PERIOD.
    RegisterKeyBind(Key.DECIMAL, SafeCallback(function()
        if not MenuOpen then return end
        MenuOpen = false
        UndoLastShuffle()
    end))
end)
if not okMenu then
    log("Could not register keybinds: %s", tostring(errMenu))
end

-- ===================== Startup =====================

do
    local ok = pcall(RefreshAllPools)
    if not ok then log("Initial pool population failed, will retry on first island clear") end
end

log("v%s loaded. %s%s = quick menu, %s%s = reroll now (open the console with F10 to see output).",
    MOD_VERSION,
    (Config.keyQuickMenuMod ~= "" and Config.keyQuickMenuMod:lower() ~= "none") and (Config.keyQuickMenuMod .. "+") or "",
    Config.keyQuickMenu,
    (Config.keyRerollNowMod ~= "" and Config.keyRerollNowMod:lower() ~= "none") and (Config.keyRerollNowMod .. "+") or "",
    Config.keyRerollNow)
HealthReport()
