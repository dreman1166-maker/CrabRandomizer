// Executes test_main.lua against the real main.lua under a mocked UE4SS env.
// main.lua resolves its config/log paths relative to CWD, so we run from a temp
// sandbox that mirrors the expected Mods/CrabRandomizer/Scripts/ layout.
const fs = require("fs");
const path = require("path");
const { lua, lauxlib, lualib, to_luastring } = require("fengari");

const toolDir = __dirname;
if (!process.argv[2]) {
    console.error("usage: node runtests.js <path-to-main.lua>");
    process.exit(2);
}
// Resolve BEFORE the chdir below - otherwise a relative path (e.g. the one build.ps1
// and CI pass) would be re-resolved against the sandbox and fail to open.
const mainLua = path.resolve(process.argv[2]);

const sandbox = path.join(toolDir, "sandbox");
const scriptsDir = path.join(sandbox, "Mods", "CrabRandomizer", "Scripts");
fs.rmSync(sandbox, { recursive: true, force: true });
fs.mkdirSync(scriptsDir, { recursive: true });

process.chdir(sandbox);

const L = lauxlib.luaL_newstate();
lualib.luaL_openlibs(L);

// fengari ships a browser-oriented `io` with no open/lines. main.lua genuinely uses
// io.open/io.lines for config, logging and dumps, so back them with real fs here.
function pushJs(name, fn) {
    lua.lua_pushcfunction(L, fn);
    lua.lua_setglobal(L, to_luastring(name));
}
pushJs("__fs_read", (L) => {
    const p = lua.lua_tojsstring(L, 1);
    try {
        lua.lua_pushstring(L, to_luastring(fs.readFileSync(p, "utf8")));
    } catch (e) {
        lua.lua_pushnil(L);
    }
    return 1;
});
pushJs("__fs_write", (L) => {
    fs.writeFileSync(lua.lua_tojsstring(L, 1), lua.lua_tojsstring(L, 2) || "", "utf8");
    return 0;
});
pushJs("__fs_append", (L) => {
    fs.appendFileSync(lua.lua_tojsstring(L, 1), lua.lua_tojsstring(L, 2) || "", "utf8");
    return 0;
});
pushJs("__fs_remove", (L) => {
    try { fs.unlinkSync(lua.lua_tojsstring(L, 1)); } catch (e) { /* already gone */ }
    return 0;
});

lauxlib.luaL_dostring(L, to_luastring(`
io.open = function(path, mode)
    mode = mode or "r"
    if mode:find("r") then
        local content = __fs_read(path)
        if not content then return nil, path .. ": No such file" end
        local consumed = false
        return {
            read = function(_, fmt)
                if consumed then return nil end
                consumed = true
                return content
            end,
            lines = function() return content:gmatch("[^\\n]+") end,
            close = function() return true end,
        }
    end
    local buf = {}
    return {
        write = function(_, s) buf[#buf + 1] = tostring(s) end,
        close = function()
            if mode:find("a") then __fs_append(path, table.concat(buf))
            else __fs_write(path, table.concat(buf)) end
            return true
        end,
    }
end

io.lines = function(path)
    local content = __fs_read(path)
    if not content then error(path .. ": No such file") end
    return content:gmatch("[^\\n]+")
end
`));

// let require() find mock_ue4ss.lua in the tool dir
const pkgPath = path.join(toolDir, "?.lua").replace(/\\/g, "/");
lauxlib.luaL_dostring(L, to_luastring(`package.path = "${pkgPath};" .. package.path`));

const mainPath = mainLua.replace(/\\/g, "/");
const cfgPath = path.join(scriptsDir, "randoconfig.txt").replace(/\\/g, "/");
lauxlib.luaL_dostring(L, to_luastring(`MAIN_LUA_PATH = "${mainPath}"`));
lauxlib.luaL_dostring(L, to_luastring(`TEST_CONFIG_PATH = "${cfgPath}"`));

const testFile = path.join(toolDir, "test_main.lua");
const status = lauxlib.luaL_dofile(L, to_luastring(testFile));
if (status !== lua.LUA_OK) {
    console.error("LUA ERROR: " + lua.lua_tojsstring(L, -1));
    process.exit(1);
}
