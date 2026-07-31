// Lua syntax checker using fengari (pure-JS Lua 5.3 VM).
// Uses luaL_loadstring, which COMPILES a chunk without executing it - so it catches
// real syntax errors without ever calling UE4SS-only globals like RegisterHook,
// FindAllOf, etc. that don't exist outside the game (calling them would be a runtime
// error, not what we're checking for here).
const fs = require("fs");
const { lua, lauxlib, to_luastring, to_jsstring } = require("fengari");

const file = process.argv[2];
if (!file) {
    console.error("usage: node check.js <file.lua>");
    process.exit(2);
}

const src = fs.readFileSync(file, "utf8");
const L = lauxlib.luaL_newstate();
const status = lauxlib.luaL_loadstring(L, to_luastring(src));

if (status === lua.LUA_OK) {
    console.log("PARSE OK: " + file);
    process.exit(0);
} else {
    const msg = to_jsstring(lua.lua_tostring(L, -1));
    console.error("PARSE ERROR in " + file + ": " + msg);
    process.exit(1);
}
