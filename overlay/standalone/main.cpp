// CrabRandomizer overlay - Plan B: a SEPARATE transparent always-on-top window.
//
// See docs/overlay-gui-plan.md. Nothing here is injected into Crab Champions. This is its
// own process with its own D3D11 device and swapchain, drawing ImGui into a layered
// topmost window positioned over the game.
//
// WHY THIS SHAPE
//   Five in-process attempts each failed on a different assumption about how UE4 renders:
//   the HUD hook never fired, the WndProc never saw keys, hooking both Present and Present1
//   double-ran the frame and crashed, and a cached RTV drew into a buffer that is never
//   displayed. Every one was a guess about internals.
//
//   This design has no such dependency. It never touches the game's device, context,
//   buffers or render state, so it cannot crash the render thread - it is not on it. Being
//   visible depends only on Windows compositing.
//
//   It is also not a .pak, so it plays no part in the host/client content matching that
//   broke co-op invites (see crabrandomizer-coop-pak-mismatch), and it is not a DLL in the
//   game, so it can never be the cause of a game crash. Close the window to remove it.
//
// LIMITATION
//   Does not appear over EXCLUSIVE fullscreen. Borderless and windowed are fine, which is
//   this game's default. Detected and reported rather than silently showing nothing.
//
// TRANSPARENCY
//   WS_EX_LAYERED + a black colour key. Simple and dependable. The consequence is that pure
//   black pixels become see-through, so the UI palette deliberately avoids #000000.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <map>
#include <vector>
#include <fstream>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ============================ globals ============================

HWND  gHwnd     = nullptr;      // our overlay window
HWND  gGameHwnd = nullptr;      // the game's window we track
bool  gMenuOpen = true;         // start visible so a first run is never a mystery
bool  gRunning  = true;
bool  gClickThrough = false;

ID3D11Device*           gDevice  = nullptr;
ID3D11DeviceContext*    gContext = nullptr;
IDXGISwapChain*         gSwap    = nullptr;
ID3D11RenderTargetView* gRTV     = nullptr;

std::string gScriptsDir;
std::string gStatus = "starting";

std::map<std::string, std::string> gConfig;
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
const char* kPresets[]   = { "off", "gentle", "default", "chaos", "mirror", "swap" };
const char* kCoopModes[] = { "independent", "mirror", "swap" };
const char* kRollModes[] = { "weighted", "withinRarity", "uniform" };

void Log(const char* fmt, ...) {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t s = p.find_last_of("\\/");
    p = (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "\\CrabOverlayApp.log";
    FILE* f = fopen(p.c_str(), "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n"); fclose(f);
}

// ============================ find the game ============================

DWORD FindGamePid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "CrabChampions-Win64-Shipping.exe") == 0) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct FindCtx { DWORD pid; HWND found; };

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    // Skip tool/among tiny windows; we want the actual game viewport.
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 200) return TRUE;
    c->found = h;
    return FALSE;
}

HWND FindGameWindow() {
    DWORD pid = FindGamePid();
    if (!pid) return nullptr;
    FindCtx c{ pid, nullptr };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

// ============================ file interop ============================
// Identical contract to the in-process build: the Lua mod owns the config, we read it and
// append commands it polls. Deliberately no memory access of any kind.

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void ResolveScriptsDir() {
    // Alongside the game exe, discovered from the running process, so the overlay works
    // wherever Steam put it.
    const char* rel[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Crab Champions\\CrabChampions\\Binaries\\Win64\\",
    };
    DWORD pid = FindGamePid();
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            char buf[MAX_PATH]{}; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameA(h, 0, buf, &n)) {
                std::string exe(buf);
                size_t s = exe.find_last_of("\\/");
                if (s != std::string::npos) {
                    gScriptsDir = exe.substr(0, s) + "\\Mods\\CrabRandomizer\\Scripts\\";
                    CloseHandle(h);
                    return;
                }
            }
            CloseHandle(h);
        }
    }
    gScriptsDir = std::string(rel[0]) + "Mods\\CrabRandomizer\\Scripts\\";
}

void LoadConfig() {
    gConfig.clear();
    std::ifstream f(gScriptsDir + "randoconfig.txt");
    if (!f.is_open()) { gStatus = "randoconfig.txt not found"; gConfigLoaded = false; return; }
    std::string line;
    while (std::getline(f, line)) {
        size_t c = line.find(';');
        if (c != std::string::npos) line = line.substr(0, c);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq));
        if (!k.empty()) gConfig[k] = Trim(line.substr(eq + 1));
    }
    gConfigLoaded = true;
    gStatus = "config loaded (" + std::to_string(gConfig.size()) + " keys)";
}

void SendCommand(const std::string& cmd) {
    std::ofstream f(gScriptsDir + "uicommand.txt", std::ios::app);
    if (!f.is_open()) { gStatus = "cannot write uicommand.txt"; return; }
    f << cmd << "\n";
    gStatus = "sent: " + cmd;
}

void SetConfig(const std::string& k, const std::string& v) {
    gConfig[k] = v;
    SendCommand("set " + k + " " + v);
}

bool GetBool(const std::string& k) { auto i = gConfig.find(k); return i != gConfig.end() && i->second == "true"; }
int  GetInt(const std::string& k, int d) { auto i = gConfig.find(k); if (i == gConfig.end()) return d; try { return std::stoi(i->second); } catch (...) { return d; } }
std::string GetStr(const std::string& k, const char* d) { auto i = gConfig.find(k); return i == gConfig.end() ? d : i->second; }

// ============================ the menu ============================
// Lifted from the in-process build unchanged - it was always correct; only the surface it
// drew onto was wrong.

void DrawMenu() {
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CrabRandomizer", &gMenuOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End(); return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.24f, 1.0f), "CrabRandomizer");
    ImGui::SameLine(); ImGui::TextDisabled("| %s", gStatus.c_str());
    if (!gGameHwnd) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "  [game not running]");
    }
    ImGui::Separator();

    if (!gConfigLoaded) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Config not loaded.");
        ImGui::TextWrapped("Looked in: %s", gScriptsDir.c_str());
        if (ImGui::Button("Retry")) LoadConfig();
        ImGui::End(); return;
    }

    ImGui::TextDisabled("PRESETS");
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(kPresets[i])) SendCommand(std::string("preset ") + kPresets[i]);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("WHAT GETS RANDOMIZED");
    for (int i = 0; i < IM_ARRAYSIZE(kToggles); ++i) {
        bool v = GetBool(kToggles[i]);
        if (ImGui::Checkbox(kToggleLabels[i], &v)) SetConfig(kToggles[i], v ? "true" : "false");
        if (i == 5) ImGui::Separator();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("MODES");

    std::string coop = GetStr("coopMode", "independent");
    int ci = 0; for (int i = 0; i < 3; ++i) if (coop == kCoopModes[i]) ci = i;
    if (ImGui::Combo("Co-op", &ci, kCoopModes, 3)) SetConfig("coopMode", kCoopModes[ci]);
    if (ci == 1) ImGui::TextDisabled("  both players get the SAME roll (host applies it)");
    if (ci == 2) ImGui::TextDisabled("  players trade loadouts; needs 2+ players");

    std::string roll = GetStr("rollMode", "weighted");
    int ri = 0; for (int i = 0; i < 3; ++i) if (roll == kRollModes[i]) ri = i;
    if (ImGui::Combo("Roll", &ri, kRollModes, 3)) SetConfig("rollMode", kRollModes[ri]);

    int isl = GetInt("islandsBeforeRandomizing", 3);
    if (ImGui::SliderInt("Islands between shuffles", &isl, 1, 10))
        SetConfig("islandsBeforeRandomizing", std::to_string(isl));

    int mr = GetInt("minimumRarity", 1);
    if (ImGui::SliderInt("Minimum rarity", &mr, 1, 4))
        SetConfig("minimumRarity", std::to_string(mr));

    if (ri == 0) {
        ImGui::Spacing(); ImGui::TextDisabled("RARITY WEIGHTS");
        const char* wk[] = { "rarityWeight1","rarityWeight2","rarityWeight3","rarityWeight4" };
        const char* wl[] = { "Common","Uncommon","Rare","Legendary" };
        for (int i = 0; i < 4; ++i) {
            int w = GetInt(wk[i], 25);
            if (ImGui::SliderInt(wl[i], &w, 0, 100)) SetConfig(wk[i], std::to_string(w));
        }
    }

    ImGui::Spacing(); ImGui::Separator();
    if (ImGui::Button("Shuffle now", ImVec2(140, 30))) SendCommand("now");
    ImGui::SameLine(); if (ImGui::Button("Undo", ImVec2(85, 30)))   SendCommand("undo");
    ImGui::SameLine(); if (ImGui::Button("Status", ImVec2(85, 30))) SendCommand("status");
    ImGui::SameLine(); if (ImGui::Button("Reload", ImVec2(85, 30))) LoadConfig();

    ImGui::Spacing();
    ImGui::TextDisabled("F8 hides/shows this window. Changes are applied by the Lua mod.");
    ImGui::End();
}

// ============================ d3d for OUR window ============================

bool CreateDevice(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
        D3D11_SDK_VERSION, &sd, &gSwap, &gDevice, &got, &gContext);
    if (FAILED(hr)) { Log("D3D11CreateDeviceAndSwapChain failed hr=0x%08X", (unsigned)hr); return false; }

    ID3D11Texture2D* bb = nullptr;
    gSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (!bb) return false;
    gDevice->CreateRenderTargetView(bb, nullptr, &gRTV);
    bb->Release();
    return gRTV != nullptr;
}

void ReleaseRTV() { if (gRTV) { gRTV->Release(); gRTV = nullptr; } }

void RebuildRTV() {
    ReleaseRTV();
    ID3D11Texture2D* bb = nullptr;
    gSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (bb) { gDevice->CreateRenderTargetView(bb, nullptr, &gRTV); bb->Release(); }
}

// ============================ window ============================

LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (gDevice && w != SIZE_MINIMIZED) {
            ReleaseRTV();
            gSwap->ResizeBuffers(0, (UINT)LOWORD(l), (UINT)HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
            RebuildRTV();
        }
        return 0;
    case WM_DESTROY: gRunning = false; PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// Click-through while the menu is hidden so gameplay input passes straight through;
// interactive while it is shown.
void SetClickThrough(bool on) {
    if (on == gClickThrough) return;
    gClickThrough = on;
    LONG_PTR ex = GetWindowLongPtr(gHwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(gHwnd, GWL_EXSTYLE, ex);
}

// Follow the game window's position and size.
void TrackGame() {
    HWND g = FindGameWindow();
    if (g != gGameHwnd) {
        gGameHwnd = g;
        Log("game window %s (%p)", g ? "found" : "lost", (void*)g);
    }
    if (!gGameHwnd) return;

    RECT r{};
    if (!GetWindowRect(gGameHwnd, &r)) return;
    RECT o{};
    GetWindowRect(gHwnd, &o);
    if (memcmp(&r, &o, sizeof(RECT)) != 0) {
        SetWindowPos(gHwnd, HWND_TOPMOST, r.left, r.top,
                     r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    }
}

} // namespace

// ============================ entry ============================

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    Log("=== CrabRandomizer overlay (standalone) starting ===");

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "CrabRandomizerOverlayWnd";
    RegisterClassExA(&wc);

    gHwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, "CrabRandomizer Overlay",
        WS_POPUP, 100, 100, 1280, 720,
        nullptr, nullptr, inst, nullptr);
    if (!gHwnd) { Log("CreateWindowEx failed"); return 1; }

    // Black is the transparency key, so the palette avoids pure black. Simple and reliable
    // - no DirectComposition, no per-pixel alpha plumbing to get wrong.
    SetLayeredWindowAttributes(gHwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (!CreateDevice(gHwnd)) { Log("device creation failed"); return 1; }

    ShowWindow(gHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(gHwnd);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    // Never fully transparent, and never pure black - the colour key would eat it.
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.12f, 0.97f);

    ImGui_ImplWin32_Init(gHwnd);
    ImGui_ImplDX11_Init(gDevice, gContext);

    ResolveScriptsDir();
    LoadConfig();
    Log("scripts dir: %s", gScriptsDir.c_str());

    // Global hotkey so F8 works while the GAME has focus - our window is NOACTIVATE and
    // never takes focus, so a WM_KEYDOWN would never arrive on its own.
    if (!RegisterHotKey(gHwnd, 1, 0, VK_F8)) Log("RegisterHotKey(F8) failed - use the tray/close button");

    ULONGLONG lastTrack = 0;
    MSG msg{};
    while (gRunning) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { gRunning = false; break; }
            if (msg.message == WM_HOTKEY && msg.wParam == 1) {
                gMenuOpen = !gMenuOpen;
                if (gMenuOpen) LoadConfig();
                Log("menu %s (hotkey)", gMenuOpen ? "shown" : "hidden");
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!gRunning) break;

        ULONGLONG now = GetTickCount64();
        if (now - lastTrack > 250) { lastTrack = now; TrackGame(); }

        SetClickThrough(!gMenuOpen);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (gMenuOpen) DrawMenu();
        ImGui::Render();

        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // black == transparent
        gContext->OMSetRenderTargets(1, &gRTV, nullptr);
        gContext->ClearRenderTargetView(gRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        gSwap->Present(1, 0);   // vsync; this is our own swapchain, not the game's

        static bool once = false;
        if (!once) {
            once = true;
            ImDrawData* dd = ImGui::GetDrawData();
            Log("first frame drawn: %.0fx%.0f verts=%d", io.DisplaySize.x, io.DisplaySize.y,
                dd ? dd->TotalVtxCount : -1);
        }
    }

    UnregisterHotKey(gHwnd, 1);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseRTV();
    if (gSwap) gSwap->Release();
    if (gContext) gContext->Release();
    if (gDevice) gDevice->Release();
    DestroyWindow(gHwnd);
    Log("=== exited cleanly ===");
    return 0;
}
