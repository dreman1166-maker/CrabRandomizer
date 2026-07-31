-- Mocked UE4SS environment: enough of the API surface for main.lua to actually RUN
-- headless, so mirror/swap/undo/preset logic can be exercised instead of eyeballed.
-- Deliberately mimics the real quirks: TArray elements are wrappers needing :get(),
-- FindAllOf returns already-unwrapped objects, HasAuthority is a method.

Mock = { registeredCommands = {}, registeredKeybinds = {}, hooks = {}, output = {} }

Key = setmetatable({}, { __index = function(_, k) return "KEY_" .. k end })
ModifierKey = { SHIFT = "SHIFT", CONTROL = "CONTROL", ALT = "ALT" }

local realPrint = print
function print(s)
    table.insert(Mock.output, (tostring(s):gsub("\n$", "")))
end
function Mock.dumpOutput()
    for _, line in ipairs(Mock.output) do realPrint(line) end
end
function Mock.clearOutput() Mock.output = {} end
function Mock.outputContains(needle)
    for _, line in ipairs(Mock.output) do
        if line:find(needle, 1, true) then return true end
    end
    return false
end

-- ---------- fake data assets ----------
-- Real UE4SS UObjects expose IsValid(); the mod relies on it to avoid writing objects
-- that were unloaded during a level transition. Set da._dead = true to simulate an
-- asset that got garbage-collected between islands.
local function makeDA(name, rarity)
    local da
    da = {
        Rarity = rarity,
        Name = { ToString = function() return name end },
        GetFullName = function(self) return "DataAsset " .. name end,
        IsValid = function(self) return not (self or da)._dead end,
    }
    return da
end

Mock.pools = {}
local function buildPool(prefix, count)
    local t = {}
    for i = 1, count do
        table.insert(t, makeDA(prefix .. i, ((i - 1) % 4) + 1))
    end
    return t
end

Mock.pools.CrabWeaponModDA  = buildPool("WMod", 12)
Mock.pools.CrabAbilityModDA = buildPool("AMod", 8)
Mock.pools.CrabMeleeModDA   = buildPool("MMod", 8)
Mock.pools.CrabGrenadeModDA = buildPool("GMod", 7)
Mock.pools.CrabPerkDA       = buildPool("Perk", 16)
Mock.pools.CrabRelicDA      = buildPool("Relic", 6)
Mock.pools.CrabWeaponDA     = buildPool("Weapon", 5)
Mock.pools.CrabAbilityDA    = buildPool("Ability", 4)
Mock.pools.CrabMeleeDA      = buildPool("Melee", 3)

-- ---------- fake TArray ----------
-- Uses 1-based indices here; main.lua must not care, because it records whatever index
-- ForEach hands it and matches the same value back on undo.
local function makeTArray(items)
    return {
        _items = items,
        GetArrayNum = function(self) return #self._items end,
        ForEach = function(self, cb)
            for i, item in ipairs(self._items) do
                cb(i, { get = function() return item end, set = function(_, v) self._items[i] = v end })
            end
        end,
    }
end

-- ---------- fake CrabPS ----------
local psCounter = 0
function Mock.makePlayerState(hasAuthority, opts)
    opts = opts or {}
    psCounter = psCounter + 1
    local id = psCounter
    local ps
    ps = {
        _id = id,
        _arrays = {
            WeaponMods  = makeTArray({ { WeaponModDA  = Mock.pools.CrabWeaponModDA[1] },
                                       { WeaponModDA  = Mock.pools.CrabWeaponModDA[2] } }),
            AbilityMods = makeTArray({ { AbilityModDA = Mock.pools.CrabAbilityModDA[1] } }),
            MeleeMods   = makeTArray({ { MeleeModDA   = Mock.pools.CrabMeleeModDA[1] } }),
            -- 3 slots, not 1: with a single slot a legitimate same-item reroll (1-in-7)
            -- makes "did it change?" assertions flaky.
            GrenadeMods = makeTArray({ { GrenadeModDA = Mock.pools.CrabGrenadeModDA[1] },
                                       { GrenadeModDA = Mock.pools.CrabGrenadeModDA[2] },
                                       { GrenadeModDA = Mock.pools.CrabGrenadeModDA[3] } }),
            Perks       = makeTArray({ { PerkDA = Mock.pools.CrabPerkDA[1] },
                                       { PerkDA = Mock.pools.CrabPerkDA[2] },
                                       { PerkDA = Mock.pools.CrabPerkDA[3] } }),
            Relics      = makeTArray({ { RelicDA = Mock.pools.CrabRelicDA[1] } }),
        },
        WeaponDA  = Mock.pools.CrabWeaponDA[1],
        AbilityDA = Mock.pools.CrabAbilityDA[1],
        MeleeDA   = Mock.pools.CrabMeleeDA[1],
        equipCalls = 0,
        IsValid = function() return true end,
        HasAuthority = function()
            if opts.authorityErrors then error("HasAuthority unavailable") end
            return hasAuthority
        end,
        GetFullName = function(self) return "CrabPS PersistentLevel.CrabPS_C_" .. self._id end,
        GetPropertyValue = function(self, name) return self._arrays[name] end,
        ServerEquipInventory = function(self, w, a, m)
            self.WeaponDA, self.AbilityDA, self.MeleeDA = w, a, m
            self.equipCalls = self.equipCalls + 1
        end,
    }
    return ps
end

Mock.playerStates = {}

-- Distinct per-player starting loadouts so swap-mode results are unambiguous.
function Mock.seedDistinctLoadouts()
    for pi, ps in ipairs(Mock.playerStates) do
        local offset = (pi - 1) * 5
        ps._arrays.Perks._items[1].PerkDA = Mock.pools.CrabPerkDA[1 + offset]
        ps._arrays.Perks._items[2].PerkDA = Mock.pools.CrabPerkDA[2 + offset]
        ps._arrays.Perks._items[3].PerkDA = Mock.pools.CrabPerkDA[3 + offset]
    end
end

function Mock.grenadeNames(ps)
    local out = {}
    for _, slot in ipairs(ps._arrays.GrenadeMods._items) do
        table.insert(out, slot.GrenadeModDA.Name.ToString())
    end
    return table.concat(out, ",")
end

function Mock.perkNames(ps)
    local out = {}
    for _, slot in ipairs(ps._arrays.Perks._items) do
        table.insert(out, slot.PerkDA.Name.ToString())
    end
    return table.concat(out, ",")
end

-- ---------- UE4SS globals ----------
Mock.missingClasses = {}

function FindAllOf(className)
    if Mock.missingClasses[className] then return nil end
    if className == "CrabPS" then
        if #Mock.playerStates == 0 then return nil end
        return Mock.playerStates
    end
    local p = Mock.pools[className]
    if p and #p > 0 then return p end
    return nil
end

function RegisterHook(name, cb) Mock.hooks[name] = cb end

function RegisterConsoleCommandHandler(name, cb) Mock.registeredCommands[name] = cb end

function RegisterKeyBind(key, a, b)
    local cb = b or a
    table.insert(Mock.registeredKeybinds, { key = key, cb = cb })
end

-- Records the deferral and runs the callback immediately, so the island-clear path is
-- actually exercised. Mock.lastDelayMs lets tests assert the work really was deferred
-- rather than run synchronously inside the hook (running it inline crashes real clients).
Mock.lastDelayMs = nil
function ExecuteWithDelay(ms, fn)
    Mock.lastDelayMs = ms
    if type(fn) == "function" then fn() end
end

-- Fires the island-clear hook the way the game does: callback(Context, bFlawless),
-- where Context is a wrapper you must :get().
function Mock.clearIsland(ps)
    local cb = Mock.hooks["/Script/CrabChampions.CrabPC:ClientOnClearedIsland"]
    if not cb then error("island-clear hook not registered") end
    local crabPC = {
        IsValid = function() return true end,
        PlayerState = ps or Mock.playerStates[1],
    }
    return cb({ get = function() return crabPC end }, false)
end

-- Fake FOutputDevice: this is what the IN-GAME console receives. UE4SS's print() goes to
-- its own hidden debug GUI, so anything the player should see must arrive here.
-- Must report type()=="FOutputDevice" and be userdata-like, because the mod guards on
-- both before writing (mirroring UE4SS's own ConsoleCommandsMod check).
function Mock.makeAr()
    local ar = { lines = {} }
    function ar:Log(s) table.insert(self.lines, tostring(s)) end
    function ar:type() return "FOutputDevice" end
    return ar
end

-- The real guard also requires type(target)=="userdata"; a Lua table reports "table",
-- so tests override type() to let the mock through while keeping the mod's real check.
local rawtype = type
function type(v)
    if rawtype(v) == "table" and rawtype(rawget(v, "Log")) == "function" and rawget(v, "lines") then
        return "userdata"
    end
    return rawtype(v)
end

function Mock.runCommand(name, ...)
    local cb = Mock.registeredCommands[name]
    if not cb then error("no such command: " .. name) end
    Mock.lastAr = Mock.makeAr()
    return cb(name, { ... }, Mock.lastAr)
end

function Mock.arContains(needle)
    if not Mock.lastAr then return false end
    for _, line in ipairs(Mock.lastAr.lines) do
        if line:find(needle, 1, true) then return true end
    end
    return false
end

package.loaded["UEHelpers"] = {
    GetPlayerController = function()
        return {
            IsValid = function() return true end,
            PlayerState = Mock.playerStates[1],
            ServerSendChatMessage = function() Mock.chatSent = (Mock.chatSent or 0) + 1 end,
        }
    end,
}

-- Fires the chat-box hook the way the game does: callback(Context, Text, CommitMethod)
-- where Text is a wrapped FText needing :get() then :ToString().
function Mock.sendChat(str)
    local cb = Mock.hooks["/Script/CrabChampions.CrabGameStateUI:OnChatTextCommitted"]
    if not cb then error("chat hook not registered") end
    local ftext = { ToString = function() return str end }
    return cb(nil, { get = function() return ftext end }, 0)
end
