--[[
    CrabRandomizer - in-game HUD overlay menu
    =========================================

    Draws a real menu OVER the game. Not the UE4SS debug window, not chat.

    HOW THIS IS POSSIBLE AT ALL
      UE4SS v3.0.1 exposes no drawing API to Lua, which is why every earlier attempt at a
      menu stalled. But AHUD::DrawHUD() calls ReceiveDrawHUD(SizeX, SizeY), which is a
      BlueprintImplementableEvent - and that means it is a real UFunction, so RegisterHook
      can hook it. Inside that hook the HUD itself exposes DrawRect / DrawText / DrawLine
      as UFunctions we can call. That is the drawing API.

    WHY THIS FILE IS SEPARATE AND DEFAULTS TO OFF
      This is the only code in the mod that runs EVERY FRAME. The randomizer took seven
      releases to stop crashing a co-op client, and a per-frame hook is by far the easiest
      way to undo that. So:
        - it lives in its own file and is loaded with pcall; if it fails to load at all,
          main.lua carries on and the randomizer is unaffected
        - Config.overlay defaults to false. Nothing here executes until it is turned on
        - the draw callback bails on the FIRST error and disables itself permanently rather
          than throwing once per frame forever
        - it never caches the HUD, the Canvas, or any font. Every frame uses only the object
          UE4SS just handed us. Caching a native pointer across frames is exactly the bug
          that caused the 1.4.1 crash (a stale FOutputDevice), and a HUD pointer is far
          more volatile than that was
        - when the menu is closed the hook returns immediately, so the cost is one Lua
          comparison per frame

    WHAT IT CANNOT DO
      DrawText has no font metrics we can rely on without more native calls, so the layout
      is a fixed grid rather than measured. It looks like a terminal, not like the HTML
      mockup. That is a deliberate trade: every extra native call per frame is risk.
]]

local M = {}

-- API handed in by main.lua so this file never reaches into the randomizer's internals.
local API = nil

-- ===================== state =====================

local Open       = false
local Cursor     = 1
local Disabled   = false   -- latched true after the first draw error, permanently
local ErrCount   = 0
local Notice     = nil     -- transient line shown under the menu
local NoticeTick = 0

-- ===================== palette =====================
-- FLinearColor is 0..1 floats, not 0..255.

local function rgba(r, g, b, a)
    return { R = r / 255, G = g / 255, B = b / 255, A = a }
end

local C = {
    panel    = rgba(9, 20, 28, 0.92),
    header   = rgba(255, 139, 61, 0.22),
    row      = rgba(255, 255, 255, 0.04),
    rowSel   = rgba(255, 139, 61, 0.30),
    edge     = rgba(255, 255, 255, 0.16),
    text     = rgba(233, 243, 247, 1.0),
    dim      = rgba(143, 169, 183, 1.0),
    orange   = rgba(255, 139, 61, 1.0),
    teal     = rgba(52, 216, 196, 1.0),
    purple   = rgba(176, 124, 255, 1.0),
    gold     = rgba(255, 212, 94, 1.0),
    ok       = rgba(93, 219, 138, 1.0),
    off      = rgba(95, 124, 138, 1.0),
}

-- ===================== layout =====================

local PAD      = 12
local ROW_H    = 20
local HEADER_H = 30
local FOOTER_H = 22
local WIDTH    = 430
local TEXT_SCALE = 1.0

-- ===================== menu model =====================
-- Built from the API so this file has no duplicate copy of the config schema.

local function BuildRows()
    local rows = {}

    rows[#rows + 1] = { kind = "head", label = "PRESETS" }
    rows[#rows + 1] = { kind = "preset" }

    rows[#rows + 1] = { kind = "head", label = "RANDOMIZE" }
    for _, t in ipairs(API.toggles()) do
        rows[#rows + 1] = { kind = "toggle", key = t.key, label = t.label }
    end

    rows[#rows + 1] = { kind = "head", label = "MODES" }
    rows[#rows + 1] = { kind = "choice", key = "coopMode", label = "Co-op",
                        values = { "independent", "mirror", "swap" } }
    rows[#rows + 1] = { kind = "choice", key = "rollMode", label = "Roll",
                        values = { "weighted", "withinRarity", "uniform" } }
    rows[#rows + 1] = { kind = "number", key = "islandsBeforeRandomizing",
                        label = "Islands between shuffles", min = 1, max = 20, step = 1 }

    rows[#rows + 1] = { kind = "head", label = "ACTIONS" }
    rows[#rows + 1] = { kind = "action", label = "Shuffle now",  act = "now" }
    rows[#rows + 1] = { kind = "action", label = "Undo last",    act = "undo" }
    rows[#rows + 1] = { kind = "action", label = "Print status", act = "status" }

    return rows
end

local Rows = nil

local function Selectable(i)
    local r = Rows[i]
    return r ~= nil and r.kind ~= "head"
end

local function MoveCursor(dir)
    if not Rows then return end
    local n = #Rows
    for _ = 1, n do
        Cursor = Cursor + dir
        if Cursor < 1 then Cursor = n elseif Cursor > n then Cursor = 1 end
        if Selectable(Cursor) then return end
    end
end

local function SetNotice(fmt, ...)
    Notice = string.format(fmt, ...)
    NoticeTick = 180 -- roughly 3 seconds at 60fps; counted down in the draw hook
end

-- ===================== input handling =====================

local function CycleChoice(row, dir)
    local cur = API.get(row.key)
    local idx = 1
    for i, v in ipairs(row.values) do if v == cur then idx = i break end end
    idx = idx + dir
    if idx < 1 then idx = #row.values elseif idx > #row.values then idx = 1 end
    API.set(row.key, row.values[idx])
    SetNotice("%s = %s", row.key, row.values[idx])
end

local function Adjust(row, dir)
    local v = tonumber(API.get(row.key)) or row.min
    v = v + dir * (row.step or 1)
    if v < row.min then v = row.min elseif v > row.max then v = row.max end
    API.set(row.key, v)
    SetNotice("%s = %d", row.key, v)
end

--- Left/right on the preset row walks the preset list; enter applies it.
local PresetIdx = 3 -- "default"

local function ApplyPreset()
    local names = API.presets()
    local name = names[PresetIdx]
    API.applyPreset(name)
    Rows = BuildRows()
    SetNotice("Applied preset '%s'", name)
end

function M.OnKey(action)
    if not Open and action ~= "toggle" then return end

    if action == "toggle" then
        Open = not Open
        if Open then
            Rows = BuildRows()
            if not Selectable(Cursor) then MoveCursor(1) end
        end
        return
    end

    local row = Rows and Rows[Cursor]
    if not row then return end

    if action == "up" then
        MoveCursor(-1)
    elseif action == "down" then
        MoveCursor(1)
    elseif action == "left" or action == "right" then
        local dir = (action == "right") and 1 or -1
        if row.kind == "choice" then
            CycleChoice(row, dir)
        elseif row.kind == "number" then
            Adjust(row, dir)
        elseif row.kind == "preset" then
            local names = API.presets()
            PresetIdx = PresetIdx + dir
            if PresetIdx < 1 then PresetIdx = #names elseif PresetIdx > #names then PresetIdx = 1 end
        elseif row.kind == "toggle" then
            API.set(row.key, not API.get(row.key))
        end
    elseif action == "enter" then
        if row.kind == "toggle" then
            local nv = not API.get(row.key)
            API.set(row.key, nv)
            SetNotice("%s = %s", row.key, tostring(nv))
        elseif row.kind == "preset" then
            ApplyPreset()
        elseif row.kind == "action" then
            if row.act == "now" then
                API.randomizeNow(); SetNotice("Shuffled.")
            elseif row.act == "undo" then
                API.undo(); SetNotice("Undid the last shuffle.")
            elseif row.act == "status" then
                API.status(); SetNotice("Status written to the log/console.")
            end
        elseif row.kind == "choice" then
            CycleChoice(row, 1)
        end
    elseif action == "close" then
        Open = false
    end
end

function M.IsOpen() return Open end

-- ===================== drawing =====================

--- Every draw goes through here so a single failure can latch the whole overlay off
--- rather than throwing sixty times a second.
local function SafeDraw(hud, fn)
    local ok, err = pcall(fn)
    if not ok then
        ErrCount = ErrCount + 1
        if ErrCount >= 3 then
            Disabled = true
            Open = false
            if API and API.log then
                API.log("Overlay disabled after repeated draw errors: %s", tostring(err))
            end
        end
        return false
    end
    return true
end

local function Rect(hud, x, y, w, h, col)
    hud:DrawRect(col, x, y, w, h)
end

local function Text(hud, s, x, y, col, scale)
    -- Font = nil makes the HUD fall back to its default font, which avoids having to
    -- resolve and hold a UFont pointer across frames.
    hud:DrawText(s, col, x, y, nil, scale or TEXT_SCALE, false)
end

local function ValueFor(row)
    if row.kind == "toggle" then
        return API.get(row.key) and "ON" or "off", API.get(row.key) and C.ok or C.off
    elseif row.kind == "choice" then
        local v = tostring(API.get(row.key))
        local col = C.teal
        if row.key == "coopMode" then
            if v == "mirror" then col = C.purple elseif v == "swap" then col = C.gold end
        end
        return v, col
    elseif row.kind == "number" then
        return tostring(API.get(row.key)), C.orange
    elseif row.kind == "preset" then
        return API.presets()[PresetIdx] or "?", C.orange
    end
    return "", C.dim
end

--- The hook body. Keep this as short as possible: it is on the render path.
local function Draw(hud, sizeX, sizeY)
    if not Rows then Rows = BuildRows() end

    local rowCount = #Rows
    local height = HEADER_H + rowCount * ROW_H + FOOTER_H + PAD
    local x = math.floor(sizeX * 0.5 - WIDTH * 0.5)
    local y = math.floor(sizeY * 0.5 - height * 0.5)

    -- panel + 1px border drawn as four thin rects
    Rect(hud, x, y, WIDTH, height, C.panel)
    Rect(hud, x, y, WIDTH, 1, C.edge)
    Rect(hud, x, y + height - 1, WIDTH, 1, C.edge)
    Rect(hud, x, y, 1, height, C.edge)
    Rect(hud, x + WIDTH - 1, y, 1, height, C.edge)

    -- header
    Rect(hud, x, y, WIDTH, HEADER_H, C.header)
    Text(hud, "CrabRandomizer  v" .. API.version(), x + PAD, y + 8, C.text, 1.1)
    local auth = API.authoritySummary()
    Text(hud, auth, x + WIDTH - PAD - (#auth * 7), y + 9, C.dim, 0.85)

    -- rows
    local ry = y + HEADER_H + 4
    for i, row in ipairs(Rows) do
        if row.kind == "head" then
            Text(hud, row.label, x + PAD, ry + 4, C.dim, 0.8)
        else
            local selected = (i == Cursor)
            if selected then
                Rect(hud, x + 4, ry, WIDTH - 8, ROW_H - 2, C.rowSel)
                Text(hud, ">", x + 6, ry + 3, C.orange, 0.95)
            end
            Text(hud, row.label, x + PAD + 8, ry + 3, selected and C.text or C.dim, 0.95)

            local val, vcol = ValueFor(row)
            if val ~= "" then
                Text(hud, val, x + WIDTH - PAD - (#val * 8), ry + 3, vcol, 0.95)
            end
        end
        ry = ry + ROW_H
    end

    -- footer
    local fy = y + height - FOOTER_H
    Rect(hud, x + 1, fy, WIDTH - 2, 1, C.edge)
    if Notice and NoticeTick > 0 then
        NoticeTick = NoticeTick - 1
        Text(hud, Notice, x + PAD, fy + 5, C.ok, 0.85)
    else
        Text(hud, "arrows move/change   enter apply   ctrl+K close", x + PAD, fy + 5, C.dim, 0.8)
    end
end

-- ===================== hook registration =====================

function M.Init(api)
    API = api

    local ok, err = pcall(function()
        RegisterHook("/Script/Engine.HUD:ReceiveDrawHUD", function(Context, SizeX, SizeY)
            -- Cheapest possible early-out: closed or dead means one comparison per frame.
            if Disabled or not Open then return end

            SafeDraw(nil, function()
                local hud = Context:get()
                if hud == nil then return end
                local sx = SizeX and SizeX:get() or 1920
                local sy = SizeY and SizeY:get() or 1080
                if sx <= 0 or sy <= 0 then return end
                Draw(hud, sx, sy)
            end)
        end)
    end)

    if not ok then
        Disabled = true
        return false, tostring(err)
    end
    return true
end

function M.Status()
    if Disabled then return "disabled (draw errors)" end
    return Open and "open" or "ready (closed)"
end

return M
