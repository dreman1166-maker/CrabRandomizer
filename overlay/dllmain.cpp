// CrabRandomizer overlay - a standalone Dear ImGui menu drawn over Crab Champions.
//
// WHY THIS EXISTS / WHY THE EARLIER ATTEMPTS FAILED
//   v1.8.0 tried to draw from UE4SS Lua by hooking AHUD:ReceiveDrawHUD. In a live session
//   that hook fired ZERO times: Crab Champions renders its entire HUD through UMG and
//   never reaches AHUD::DrawHUD. There is no Lua-reachable draw path in this game.
//
//   This hooks IDXGISwapChain::Present instead, which sits BELOW all of that - it is the
//   call that puts the finished frame on screen, so nothing the game does with UMG can
//   hide us. The game is DX11 (UE4SS reports "Debugging Tools (DX11)").
//
// WHY IT DOES NOT USE THE UE4SS C++ API
//   RE-UE4SS's UEPseudo submodule 404s, so its SDK cannot be assembled from a fresh clone
//   by anyone. We only need UE4SS to LOAD us, which it does with LoadLibrary - so we
//   satisfy its export contract with trivial stubs and depend on none of its headers.
//   Dependencies are just ImGui + MinHook + the Windows SDK.
//
// SAFETY
//   Present runs on the render thread. Everything here is written to fail quiet:
//   initialisation happens once, guarded; if any step fails the hook becomes a passthrough
//   for the rest of the process lifetime rather than retrying every frame. The overlay
//   never touches game memory, actors or replicated state - it only reads and writes two
//   text files that the Lua mod polls. It cannot desync co-op.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ============================ state ============================

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
PresentFn oPresent = nullptr;

WNDPROC  oWndProc  = nullptr;
HWND     gWindow   = nullptr;
ID3D11Device*        gDevice  = nullptr;
ID3D11DeviceContext* gContext = nullptr;
ID3D11RenderTargetView* gRTV  = nullptr;

bool gInitDone   = false;   // imgui + rtv ready
bool gInitFailed = false;   // latched: never try again, just pass through
bool gMenuOpen   = false;

std::string gScriptsDir;    // ...\Mods\CrabRandomizer\Scripts\
std::string gStatus = "loaded";

// ---- config mirrored from randoconfig.txt ----
std::map<std::string, std::string> gConfig;
std::vector<std::string> gConfigOrder;
bool gConfigLoaded = false;

const char* kToggles[] = {
    "randomizeWeaponMods", "randomizeAbilityMods", "randomizeMeleeMods",
    "randomizeGrenadeMods", "randomizePerks", "randomizeRelics",
    "randomizeWeapon", "randomizeAbility", "randomizeMelee",
};
const char* kToggleLabels[] = {
    "Weapon mods", "Ability mods", "Melee mods",
    "Grenade mods", "Perks", "Relics",
    "Weapon", "Ability", "Melee",
};
const char* kPresets[]  = { "off", "gentle", "default", "chaos", "mirror", "swap" };
const char* kCoopModes[] = { "independent", "mirror", "swap" };
const char* kRollModes[] = { "weighted", "withinRarity", "uniform" };

// ============================ file interop ============================
//
// The Lua mod owns the config. We read it to show current values and append commands to
// uicommand.txt, which main.lua polls (Config.uiBridge). We deliberately do NOT reach into
// the game - a text file cannot crash the render thread or desync a session.

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void LoadConfig() {
    gConfig.clear();
    gConfigOrder.clear();
    std::ifstream f(gScriptsDir + "randoconfig.txt");
    if (!f.is_open()) { gStatus = "randoconfig.txt not found"; return; }
    std::string line;
    while (std::getline(f, line)) {
        size_t c = line.find(';');
        if (c != std::string::npos) line = line.substr(0, c);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq));
        std::string v = Trim(line.substr(eq + 1));
        if (k.empty()) continue;
        if (!gConfig.count(k)) gConfigOrder.push_back(k);
        gConfig[k] = v;
    }
    gConfigLoaded = true;
    gStatus = "config loaded (" + std::to_string(gConfig.size()) + " keys)";
}

// Appended, never truncated: the Lua side consumes and clears. Writing the whole file
// would race with it mid-read.
void SendCommand(const std::string& cmd) {
    std::ofstream f(gScriptsDir + "uicommand.txt", std::ios::app);
    if (!f.is_open()) { gStatus = "could not write uicommand.txt"; return; }
    f << cmd << "\n";
    gStatus = "sent: " + cmd;
}

void SetConfig(const std::string& key, const std::string& val) {
    gConfig[key] = val;
    SendCommand("set " + key + " " + val);
}

bool GetBool(const std::string& key) {
    auto it = gConfig.find(key);
    return it != gConfig.end() && it->second == "true";
}

int GetInt(const std::string& key, int def) {
    auto it = gConfig.find(key);
    if (it == gConfig.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

std::string GetStr(const std::string& key, const char* def) {
    auto it = gConfig.find(key);
    return it == gConfig.end() ? def : it->second;
}

// Locate ...\Mods\CrabRandomizer\Scripts\ relative to the game exe.
void ResolveScriptsDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe(buf);
    size_t slash = exe.find_last_of("\\/");
    std::string dir = (slash == std::string::npos) ? "." : exe.substr(0, slash);
    gScriptsDir = dir + "\\Mods\\CrabRandomizer\\Scripts\\";
}

// ============================ the menu ============================

void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!ImGui::Begin("CrabRandomizer", &gMenuOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.24f, 1.0f), "CrabRandomizer");
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", gStatus.c_str());
    ImGui::Separator();

    if (!gConfigLoaded) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Config not loaded.");
        ImGui::TextWrapped("Expected: %srandoconfig.txt", gScriptsDir.c_str());
        if (ImGui::Button("Retry")) LoadConfig();
        ImGui::End();
        return;
    }

    // ---- presets ----
    ImGui::TextDisabled("PRESETS");
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(kPresets[i])) {
            SendCommand(std::string("preset ") + kPresets[i]);
            // The Lua side rewrites the config; pull it back shortly after.
            gStatus = std::string("preset ") + kPresets[i] + " sent";
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("WHAT GETS RANDOMIZED");
    for (int i = 0; i < IM_ARRAYSIZE(kToggles); ++i) {
        bool v = GetBool(kToggles[i]);
        if (ImGui::Checkbox(kToggleLabels[i], &v))
            SetConfig(kToggles[i], v ? "true" : "false");
        if (i == 5) ImGui::Separator();   // slot arrays | base loadout
    }

    ImGui::Spacing();
    ImGui::TextDisabled("MODES");

    std::string coop = GetStr("coopMode", "independent");
    int coopIdx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kCoopModes); ++i)
        if (coop == kCoopModes[i]) coopIdx = i;
    if (ImGui::Combo("Co-op", &coopIdx, kCoopModes, IM_ARRAYSIZE(kCoopModes)))
        SetConfig("coopMode", kCoopModes[coopIdx]);
    if (coopIdx == 1) ImGui::TextDisabled("  both players get the SAME roll (host applies it)");
    if (coopIdx == 2) ImGui::TextDisabled("  players trade loadouts; needs 2+ players");

    std::string roll = GetStr("rollMode", "weighted");
    int rollIdx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kRollModes); ++i)
        if (roll == kRollModes[i]) rollIdx = i;
    if (ImGui::Combo("Roll", &rollIdx, kRollModes, IM_ARRAYSIZE(kRollModes)))
        SetConfig("rollMode", kRollModes[rollIdx]);

    int islands = GetInt("islandsBeforeRandomizing", 3);
    if (ImGui::SliderInt("Islands between shuffles", &islands, 1, 10))
        SetConfig("islandsBeforeRandomizing", std::to_string(islands));

    int minRar = GetInt("minimumRarity", 1);
    if (ImGui::SliderInt("Minimum rarity", &minRar, 1, 4))
        SetConfig("minimumRarity", std::to_string(minRar));

    if (rollIdx == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("RARITY WEIGHTS");
        const char* wk[] = { "rarityWeight1", "rarityWeight2", "rarityWeight3", "rarityWeight4" };
        const char* wl[] = { "Common", "Uncommon", "Rare", "Legendary" };
        for (int i = 0; i < 4; ++i) {
            int w = GetInt(wk[i], 25);
            if (ImGui::SliderInt(wl[i], &w, 0, 100)) SetConfig(wk[i], std::to_string(w));
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Shuffle now", ImVec2(150, 30))) SendCommand("now");
    ImGui::SameLine();
    if (ImGui::Button("Undo", ImVec2(90, 30)))         SendCommand("undo");
    ImGui::SameLine();
    if (ImGui::Button("Status", ImVec2(90, 30)))       SendCommand("status");
    ImGui::SameLine();
    if (ImGui::Button("Reload config", ImVec2(120, 30))) LoadConfig();

    ImGui::Spacing();
    ImGui::TextDisabled("Ctrl+K or Insert closes this. Changes are applied by the Lua mod.");
    ImGui::End();
}

// ============================ hooks ============================

LRESULT WINAPI HookedWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Toggle on Ctrl+K or Insert. Insert is a safety valve: if the game swallows Ctrl+K
    // there is still a way in.
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if ((ctrl && wp == 'K') || wp == VK_INSERT) {
            gMenuOpen = !gMenuOpen;
            if (gMenuOpen) LoadConfig();
            return 0;   // swallow, so the game does not also act on it
        }
    }

    // Only feed input to ImGui while visible, so gameplay input is untouched when closed.
    if (gMenuOpen && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp))
        return 1;

    return CallWindowProc(oWndProc, hWnd, msg, wp, lp);
}

bool InitFromSwapChain(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&gDevice))) return false;
    gDevice->GetImmediateContext(&gContext);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    gWindow = desc.OutputWindow;
    if (!gWindow) return false;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)) || !backBuffer)
        return false;
    HRESULT hr = gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                     // no imgui.ini beside the game exe
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding  = 4.0f;

    if (!ImGui_ImplWin32_Init(gWindow)) return false;
    if (!ImGui_ImplDX11_Init(gDevice, gContext)) return false;

    oWndProc = (WNDPROC)SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    ResolveScriptsDir();
    LoadConfig();
    return true;
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (gInitFailed) return oPresent(sc, sync, flags);

    if (!gInitDone) {
        if (!InitFromSwapChain(sc)) {
            // Latch off rather than retrying sixty times a second forever.
            gInitFailed = true;
            return oPresent(sc, sync, flags);
        }
        gInitDone = true;
    }

    if (gMenuOpen && gRTV) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawMenu();
        ImGui::Render();
        gContext->OMSetRenderTargets(1, &gRTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return oPresent(sc, sync, flags);
}

// Get the Present address by creating a throwaway device+swapchain and reading its vtable.
// Standard technique: the vtable is per-class, so slot 8 on our dummy is the same function
// the game's swapchain uses.
void* FindPresent() {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "CRDummyWnd";
    if (!RegisterClassExA(&wc)) return nullptr;

    HWND wnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                             0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) { UnregisterClassA(wc.lpszClassName, wc.hInstance); return nullptr; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        want, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);

    void* present = nullptr;
    if (SUCCEEDED(hr) && sc) {
        void** vtable = *reinterpret_cast<void***>(sc);
        present = vtable[8];   // IDXGISwapChain::Present
    }
    if (sc)  sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(wnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return present;
}

DWORD WINAPI Bootstrap(LPVOID) {
    // The game needs its own device up before a dummy one behaves; a short wait avoids
    // racing startup. This runs on our own thread, never the render thread.
    Sleep(4000);

    if (MH_Initialize() != MH_OK) return 0;
    void* present = FindPresent();
    if (!present) { MH_Uninitialize(); return 0; }

    if (MH_CreateHook(present, &HookedPresent, (LPVOID*)&oPresent) != MH_OK) {
        MH_Uninitialize();
        return 0;
    }
    MH_EnableHook(present);
    return 0;
}

} // namespace

// ============================ entry points ============================

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
    }
    return TRUE;
}

// UE4SS loads C++ mods with LoadLibrary and looks for these. We implement them as stubs so
// it is satisfied without us depending on a single UE4SS header - which matters because
// RE-UE4SS's UEPseudo submodule 404s and its SDK cannot currently be built by anyone.
extern "C" __declspec(dllexport) void* start_mod()   { return nullptr; }
extern "C" __declspec(dllexport) void  uninstall_mod(void*) {}
