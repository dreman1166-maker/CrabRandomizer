-- Regression tests for CrabRandomizer main.lua, run against the mocked UE4SS env.
-- Loads the REAL main.lua (path passed as arg) so these test shipped behavior.

require("mock_ue4ss")

local MAIN_PATH = MAIN_LUA_PATH or error("MAIN_LUA_PATH not set")

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
        io.write("  PASS  " .. name .. "\n")
    else
        failed = failed + 1
        io.write("  FAIL  " .. name .. (detail and ("  -- " .. tostring(detail)) or "") .. "\n")
    end
end

-- NOTE: does not touch Mock.playerStates - callers set those up first so the mod sees
-- the intended player roster on load.
-- keepConfig=true preserves the on-disk randoconfig.txt (used by the migration test);
-- otherwise it is deleted, because the mod legitimately PERSISTS config via SaveConfig
-- and a setting from an earlier test would otherwise leak into later ones.
local function loadMain(keepConfig)
    if not keepConfig then __fs_remove(TEST_CONFIG_PATH) end
    Mock.chatSent = 0
    Mock.registeredCommands = {}
    Mock.registeredKeybinds = {}
    Mock.clearOutput()
    local chunk, err = loadfile(MAIN_PATH)
    if not chunk then error("could not load main.lua: " .. tostring(err)) end
    chunk()
end

-- =====================================================================
io.write("\n[1] Loads cleanly and registers its surface\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()

local expectedCommands = {
    "randomizenow", "randomizeundo", "randomizestatus", "randomizeauthority",
    "randomizeset", "randomizepreset", "randomizeonce", "randomizelist",
    "randomizehistory", "randomizedump",
}
for _, c in ipairs(expectedCommands) do
    check("command registered: " .. c, Mock.registeredCommands[c] ~= nil)
end
check("island-clear hook registered", Mock.hooks["/Script/CrabChampions.CrabPC:ClientOnClearedIsland"] ~= nil)
check("keybinds registered (>=16)", #Mock.registeredKeybinds >= 16, #Mock.registeredKeybinds)
check("no nil key in any keybind", (function()
    for _, kb in ipairs(Mock.registeredKeybinds) do
        if kb.key == nil then return false end
    end
    return true
end)())
check("health report printed on load", Mock.outputContains("Health: pools OK 9/9"))

-- =====================================================================
io.write("\n[2] Presets\n")
Mock.clearOutput()
Mock.runCommand("randomizepreset", "chaos")
check("chaos preset applied", Mock.outputContains("Applied preset 'chaos'"))
check("chaos sets islands=1", Mock.outputContains("islandsBeforeRandomizing = 1"))
check("chaos sets rollMode=uniform", Mock.outputContains("rollMode = uniform"))

Mock.clearOutput()
Mock.runCommand("randomizepreset", "nonsense")
check("unknown preset shows usage", Mock.outputContains("Usage: randomizepreset"))

-- =====================================================================
io.write("\n[3] Independent mode actually changes perks\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
local before = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizenow")
local after = Mock.perkNames(Mock.playerStates[1])
check("perks changed after randomizenow", before ~= after, before .. " -> " .. after)

-- =====================================================================
io.write("\n[4] Undo restores the previous loadout\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
local original = Mock.perkNames(Mock.playerStates[1])
Mock.runCommand("randomizenow")
local shuffled = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizeundo")
local restored = Mock.perkNames(Mock.playerStates[1])
check("shuffle changed perks", original ~= shuffled)
check("undo restored exact original", restored == original, original .. " vs restored " .. restored)
check("undo reported success", Mock.outputContains("Undo of shuffle"))

Mock.clearOutput()
Mock.runCommand("randomizeundo")
check("second undo on empty history is graceful", Mock.outputContains("Nothing to undo"))

-- =====================================================================
io.write("\n[5] Mirror mode gives both players identical loadouts\n")
Mock.playerStates = { Mock.makePlayerState(true), Mock.makePlayerState(true) }
loadMain()
Mock.seedDistinctLoadouts()
Mock.runCommand("randomizepreset", "mirror")
local p1before, p2before = Mock.perkNames(Mock.playerStates[1]), Mock.perkNames(Mock.playerStates[2])
check("players started with different perks", p1before ~= p2before, p1before .. " / " .. p2before)
Mock.runCommand("randomizenow")
local p1, p2 = Mock.perkNames(Mock.playerStates[1]), Mock.perkNames(Mock.playerStates[2])
check("mirror: both players identical after shuffle", p1 == p2, p1 .. " / " .. p2)
check("mirror: loadout actually changed", p1 ~= p1before)

-- =====================================================================
io.write("\n[6] Swap mode trades loadouts between players\n")
Mock.playerStates = { Mock.makePlayerState(true), Mock.makePlayerState(true) }
loadMain()
Mock.seedDistinctLoadouts()
Mock.runCommand("randomizepreset", "swap")
local a0, b0 = Mock.perkNames(Mock.playerStates[1]), Mock.perkNames(Mock.playerStates[2])
Mock.runCommand("randomizenow")
local a1, b1 = Mock.perkNames(Mock.playerStates[1]), Mock.perkNames(Mock.playerStates[2])
check("swap: player1 now holds player2's old perks", a1 == b0, a1 .. " expected " .. b0)
check("swap: player2 now holds player1's old perks", b1 == a0, b1 .. " expected " .. a0)

Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "swap")
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("swap with 1 player is refused, not crashed", Mock.outputContains("needs at least 2 players"))

-- =====================================================================
io.write("\n[7] Authority gate blocks non-authoritative clients\n")
Mock.playerStates = { Mock.makePlayerState(false) }
loadMain()
Mock.runCommand("randomizepreset", "default")
local beforeNoAuth = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("no-authority: perks untouched", Mock.perkNames(Mock.playerStates[1]) == beforeNoAuth)
check("no-authority: explains why", Mock.outputContains("Not authoritative"))

Mock.clearOutput()
Mock.runCommand("randomizeauthority")
check("authority report says host must run it", Mock.outputContains("authority over NOTHING"))

Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.clearOutput()
Mock.runCommand("randomizeauthority")
check("authority report confirms it can apply", Mock.outputContains("This instance CAN apply changes"))

-- =====================================================================
io.write("\n[8] Dry run computes but does not mutate\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
Mock.runCommand("randomizeset", "dryRun", "true")
local dryBefore = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("dry-run: nothing mutated", Mock.perkNames(Mock.playerStates[1]) == dryBefore)
check("dry-run: logged intended changes", Mock.outputContains("[dry-run]"))

-- =====================================================================
io.write("\n[9] Per-subsystem degradation: one missing pool doesn't kill the rest\n")
Mock.missingClasses = { CrabRelicDA = true }
Mock.pools.CrabRelicDA = {}
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
local perksBefore = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("degraded: relics reported disabled", Mock.outputContains("Relics randomization DISABLED"))
check("degraded: perks still randomized", Mock.perkNames(Mock.playerStates[1]) ~= perksBefore)
Mock.missingClasses = {}
-- Must include IsValid, like every other mock asset. Without it the mod's fail-open
-- path trips globally and leaks into later tests (it silently disabled the stale-asset
-- guard for the whole run when this was missing).
Mock.pools.CrabRelicDA = (function()
    local t = {}
    for i = 1, 6 do
        t[i] = { Rarity = ((i - 1) % 4) + 1, Name = { ToString = function() return "Relic" .. i end },
                 GetFullName = function() return "DataAsset Relic" .. i end,
                 IsValid = function(self) return not self._dead end }
    end
    return t
end)()

-- =====================================================================
io.write("\n[10] Class-name fallback resolves and says so\n")
Mock.missingClasses = { CrabPerkDA = true }
Mock.pools.CrabPerkDataAsset = Mock.pools.CrabPerkDA
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
check("fallback name resolved", Mock.outputContains("fallback name 'CrabPerkDataAsset' resolved"))
check("still healthy with fallback", Mock.outputContains("Health: pools OK 9/9"))
Mock.missingClasses = {}
Mock.pools.CrabPerkDataAsset = nil

-- =====================================================================
io.write("\n[11] randomizehistory edge cases (the v1.1.1 crash class)\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
Mock.runCommand("randomizenow")
for _, arg in ipairs({ "3.5", "-2.5", "-5", "0", "999", "abc" }) do
    Mock.clearOutput()
    local ok = pcall(Mock.runCommand, "randomizehistory", arg)
    check("randomizehistory " .. arg .. " does not crash", ok)
    check("randomizehistory " .. arg .. " has no negative count",
        not Mock.outputContains("Last -"))
end

-- =====================================================================
io.write("\n[12] HasAuthority unavailable fails open with a loud warning\n")
Mock.playerStates = { Mock.makePlayerState(true, { authorityErrors = true }) }
loadMain()
Mock.runCommand("randomizepreset", "default")
local failOpenBefore = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("fail-open: still randomizes", Mock.perkNames(Mock.playerStates[1]) ~= failOpenBefore)
check("fail-open: warns loudly", Mock.outputContains("authority gate is NOT active"))

-- =====================================================================
io.write("\n[13] Legacy randomizeWithinRarity migration\n")
Mock.playerStates = { Mock.makePlayerState(true) }
Mock.clearOutput()
do
    local f = io.open(TEST_CONFIG_PATH, "w")
    f:write("randomizeWithinRarity=true\nrandomizePerks=true\n")
    f:close()
end
loadMain(true)
check("legacy key migrated to rollMode", Mock.outputContains("migrated to rollMode=withinRarity"))
os.remove(TEST_CONFIG_PATH)

-- =====================================================================
-- Regression guard for a bug found only by real in-game testing: UE4SS's print() goes
-- to a debug GUI window that ships HIDDEN, so a command that only print()s looks like it
-- did nothing. Output the player is meant to read must reach the FOutputDevice (ar).
io.write("\n[14] Console output actually reaches the in-game console (ar:Log)\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizestatus")
check("randomizestatus writes version to ar", Mock.arContains("version = 1.4.4"))
check("randomizestatus writes pool info to ar", Mock.arContains("Pool CrabPerkDA"))

Mock.runCommand("randomizeauthority")
check("randomizeauthority writes to ar", Mock.arContains("Authority report"))

Mock.runCommand("randomizepreset")
check("randomizepreset usage writes to ar", Mock.arContains("Usage: randomizepreset"))

Mock.runCommand("randomizehistory")
check("randomizehistory writes to ar", Mock.arContains("No shuffles recorded") or Mock.arContains("change(s)"))

-- =====================================================================
-- Ctrl+K and the other keybinds are NOT console commands, so they get no output device.
-- Before the LastAr cache they printed only to UE4SS's hidden debug window, which made
-- the quick menu look completely broken.
io.write("\n[14b] Keybind output reaches the in-game console\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()

local function findKeybind(keyName)
    for _, kb in ipairs(Mock.registeredKeybinds) do
        if kb.key == keyName then return kb.cb end
    end
    return nil
end

local ctrlK = findKeybind("KEY_K")
check("Ctrl+K bind exists", ctrlK ~= nil)

-- Bind the output device the way a real session does: run any console command once.
Mock.runCommand("randomizestatus")
local arAfterCommand = Mock.lastAr
arAfterCommand.lines = {}

if ctrlK then ctrlK() end
check("Ctrl+K menu text reaches the console", (function()
    for _, l in ipairs(arAfterCommand.lines) do
        if l:find("quick menu", 1, true) then return true end
    end
    return false
end)(), table.concat(arAfterCommand.lines, " | "))

-- =====================================================================
io.write("\n[15] Grenade mods shuffle when the pool exists\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "chaos")
local gBefore = Mock.grenadeNames(Mock.playerStates[1])
Mock.clearOutput()
-- Rolling the same item back is legitimate, so retry rather than assert on one roll.
local gChanged = false
for _ = 1, 5 do
    Mock.runCommand("randomizenow")
    if Mock.grenadeNames(Mock.playerStates[1]) ~= gBefore then gChanged = true break end
end
check("grenade mods changed", gChanged,
    gBefore .. " -> " .. Mock.grenadeNames(Mock.playerStates[1]))
check("chaos preset enables grenade mods", Mock.outputContains("randomizeGrenadeMods = true")
    or (function() Mock.runCommand("randomizestatus") return Mock.arContains("randomizeGrenadeMods = true") end)())

Mock.clearOutput()
Mock.runCommand("randomizeundo")
check("undo restores grenade mods", Mock.grenadeNames(Mock.playerStates[1]) == gBefore)

-- The likely real-world case: this build may have no separate grenade slot at all.
io.write("\n[16] A build with NO grenade slot degrades safely\n")
Mock.missingClasses = { CrabGrenadeModDA = true }
local savedGrenadePool = Mock.pools.CrabGrenadeModDA
Mock.pools.CrabGrenadeModDA = {}
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "chaos")
local perksBefore16 = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
local ok16 = pcall(Mock.runCommand, "randomizenow")
check("no crash without a grenade pool", ok16)
check("grenade subsystem reports itself disabled", Mock.outputContains("GrenadeMods randomization DISABLED"))
check("everything else still shuffles", Mock.perkNames(Mock.playerStates[1]) ~= perksBefore16)
Mock.missingClasses = {}
Mock.pools.CrabGrenadeModDA = savedGrenadePool

-- =====================================================================
-- Regression guard for the island-transition crash class. Writing a data asset that the
-- engine unloaded during level teardown is a NATIVE access violation - no pcall catches
-- it - so nothing dead may ever reach a property write or an RPC.
io.write("\n[17] Stale/unloaded assets are never written (crash guard)\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")

-- Simulate the whole perk pool being unloaded on an island transition.
for _, da in ipairs(Mock.pools.CrabPerkDA) do da._dead = true end
local perksBeforeDead = Mock.perkNames(Mock.playerStates[1])
Mock.clearOutput()
local okDead = pcall(Mock.runCommand, "randomizenow")
check("no crash when every perk asset is dead", okDead)
check("dead perks were not written", Mock.perkNames(Mock.playerStates[1]) == perksBeforeDead)
for _, da in ipairs(Mock.pools.CrabPerkDA) do da._dead = nil end

-- A pool that went stale should be rescanned and recover, not stay broken forever.
Mock.clearOutput()
Mock.runCommand("randomizenow")
check("pool recovers once assets are live again",
    Mock.perkNames(Mock.playerStates[1]) ~= perksBeforeDead)

-- Only SOME assets dead: the live ones must still be usable.
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
for i, da in ipairs(Mock.pools.CrabPerkDA) do if i % 2 == 0 then da._dead = true end end
local partialBefore = Mock.perkNames(Mock.playerStates[1])
local okPartial = pcall(Mock.runCommand, "randomizenow")
check("no crash with a partially-unloaded pool", okPartial)
check("still shuffles using the live half", Mock.perkNames(Mock.playerStates[1]) ~= partialBefore)
for _, da in ipairs(Mock.pools.CrabPerkDA) do da._dead = nil end

-- The fail-open path: if IsValid() isn't callable at all, the mod must keep working
-- rather than filtering everything out and silently doing nothing.
io.write("\n[18] IsValid() unavailable fails OPEN, not silently dead\n")
Mock.playerStates = { Mock.makePlayerState(true) }
local savedIsValid = {}
for i, da in ipairs(Mock.pools.CrabPerkDA) do
    savedIsValid[i] = da.IsValid
    da.IsValid = nil -- method genuinely absent
end
loadMain()
-- The warning fires on the FIRST liveness check, which happens during load-time pool
-- scanning - so capture it before anything clears the buffer.
check("warns that protection is disabled", Mock.outputContains("stale-asset protection is DISABLED"))
Mock.runCommand("randomizepreset", "default")
local noIsValidBefore = Mock.perkNames(Mock.playerStates[1])
local okNoIV = pcall(Mock.runCommand, "randomizenow")
check("no crash when IsValid is missing", okNoIV)
check("still shuffles (fails open, not closed)",
    Mock.perkNames(Mock.playerStates[1]) ~= noIsValidBefore)
for i, da in ipairs(Mock.pools.CrabPerkDA) do da.IsValid = savedIsValid[i] end

-- =====================================================================
-- Regression guard for the joining-client crash. A non-authoritative client must never
-- call ServerEquipInventory: on a client that becomes a real client->server RPC, which
-- crashed the joiner every island while the host was fine. Only authority mutates.
io.write("\n[19] A non-authoritative client never touches base loadout\n")
Mock.playerStates = { Mock.makePlayerState(false) } -- joining client: no authority
loadMain()
Mock.runCommand("randomizepreset", "chaos")         -- chaos enables weapon/ability/melee
Mock.playerStates[1].equipCalls = 0
Mock.clearOutput()
local okClient = pcall(Mock.runCommand, "randomizenow")
check("no crash on a client", okClient)
check("client did NOT call ServerEquipInventory", Mock.playerStates[1].equipCalls == 0,
    "equipCalls=" .. tostring(Mock.playerStates[1].equipCalls))
-- RandomizeBaseLoadout is no longer even reached for a client, so the message the user
-- actually sees is the mod/perk/relic one from RandomizeEveryone.
check("client explains it is not authoritative", Mock.outputContains("Not authoritative over"))

-- And the host still does apply it, so the fix didn't disable the feature.
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "chaos")
Mock.playerStates[1].equipCalls = 0
Mock.runCommand("randomizenow")
check("host DOES apply base loadout", Mock.playerStates[1].equipCalls > 0,
    "equipCalls=" .. tostring(Mock.playerStates[1].equipCalls))

-- Mixed lobby: host authoritative over both, joiner's own copy must stay inert.
Mock.playerStates = { Mock.makePlayerState(true), Mock.makePlayerState(false) }
loadMain()
Mock.runCommand("randomizepreset", "chaos")
Mock.playerStates[1].equipCalls = 0
Mock.playerStates[2].equipCalls = 0
Mock.runCommand("randomizenow")
check("mixed lobby: authoritative player equipped", Mock.playerStates[1].equipCalls > 0)
check("mixed lobby: non-authoritative player skipped", Mock.playerStates[2].equipCalls == 0,
    "equipCalls=" .. tostring(Mock.playerStates[2].equipCalls))

-- =====================================================================
-- The island-clear path is the one that crashes real clients. It must (a) count islands
-- correctly and (b) DEFER the shuffle instead of mutating during level teardown.
io.write("\n[20] Island clear counts, then DEFERS the shuffle off the transition frame\n")
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
Mock.runCommand("randomizeset", "islandsBeforeRandomizing", "3")

local islandBefore = Mock.perkNames(Mock.playerStates[1])
Mock.lastDelayMs = nil
Mock.clearOutput()
Mock.clearIsland()
check("island 1 of 3 does not shuffle", Mock.perkNames(Mock.playerStates[1]) == islandBefore)
Mock.clearIsland()
check("island 2 of 3 does not shuffle", Mock.perkNames(Mock.playerStates[1]) == islandBefore)
Mock.clearIsland()
check("island 3 of 3 DOES shuffle", Mock.perkNames(Mock.playerStates[1]) ~= islandBefore)
check("shuffle was deferred, not run inside the hook", (Mock.lastDelayMs or 0) > 0,
    "delay=" .. tostring(Mock.lastDelayMs))
check("tells the player a shuffle is coming", Mock.outputContains("shuffling in"))

-- Counter resets, so it takes another full 3 islands.
local afterFirst = Mock.perkNames(Mock.playerStates[1])
Mock.clearIsland()
check("counter reset after firing", Mock.perkNames(Mock.playerStates[1]) == afterFirst)

-- shuffleDelayMs=0 restores the old synchronous behaviour for anyone who wants it.
Mock.playerStates = { Mock.makePlayerState(true) }
loadMain()
Mock.runCommand("randomizepreset", "default")
Mock.runCommand("randomizeset", "islandsBeforeRandomizing", "1")
Mock.runCommand("randomizeset", "shuffleDelayMs", "0")
Mock.lastDelayMs = nil
local syncBefore = Mock.perkNames(Mock.playerStates[1])
Mock.clearIsland()
check("shuffleDelayMs=0 shuffles immediately", Mock.perkNames(Mock.playerStates[1]) ~= syncBefore)
check("shuffleDelayMs=0 uses no deferral", Mock.lastDelayMs == nil)

-- A non-authoritative client clearing an island must not crash or mutate.
Mock.playerStates = { Mock.makePlayerState(false) }
loadMain()
Mock.runCommand("randomizepreset", "chaos")
Mock.runCommand("randomizeset", "islandsBeforeRandomizing", "1")
Mock.playerStates[1].equipCalls = 0
local clientIslandBefore = Mock.perkNames(Mock.playerStates[1])
local okIsland = pcall(Mock.clearIsland)
check("client island clear does not crash", okIsland)
check("client island clear mutates nothing", Mock.perkNames(Mock.playerStates[1]) == clientIslandBefore)
check("client island clear sends no RPC", Mock.playerStates[1].equipCalls == 0)

-- =====================================================================
io.write(string.format("\n==== %d passed, %d failed ====\n", passed, failed))
if failed > 0 then os.exit(1) end
