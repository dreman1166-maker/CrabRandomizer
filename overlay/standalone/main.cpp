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

// "Grenade mods" is deliberately absent. This build of Crab Champions ships no
// CrabGrenadeModDA data assets - the mod logs "GrenadeMods randomization DISABLED" and
// degrades that slot - so the checkbox could never do anything and only looked broken.
// The config key still exists; it is settable from the console if a future patch adds them.
const char* kToggles[] = {
    "randomizeWeaponMods", "randomizeAbilityMods", "randomizeMeleeMods",
    "randomizePerks", "randomizeRelics",
    "randomizeWeapon", "randomizeAbility", "randomizeMelee",
};
const char* kToggleLabels[] = {
    "Weapon mods", "Ability mods", "Melee mods",
    "Perks", "Relics",
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

// ============================ theme ============================
//
// "Modern Dark" - the Linear / Raycast / Vercel system, taken from the ui-ux-pro-max design
// database rather than from taste. Its exact tokens:
//
//     bg-deep #020203 · base #050506 · elevated #0a0a0c
//     surface rgba(255,255,255,0.05) · border rgba(255,255,255,0.08)
//     foreground #EDEDEF · muted #8A8F98 · radius 16
//     [x] no pure #000000    [x] no solid grey borders - rgba only
//
// WHAT THE PREVIOUS VERSION GOT WRONG
//   It was skeuomorphic: bevelled key caps, faux die-cast metal, gradient-filled buttons,
//   engraved plates. That vocabulary reads as a 2005 media-player skin, not premium
//   software. Modern high-end UI is FLAT and takes its depth from three things only:
//     1. translucent white surfaces stacked on a near-black base (never grey on grey)
//     2. hairline rgba borders - never a solid grey line, which is the amateur tell
//     3. generous, strictly consistent spacing
//   No bevels, no gradients on controls, no ornament that carries no information.
//
// TRANSPARENCY CONTRACT
//   The window is keyed out on RGB(0,0,0), so pure black becomes a see-through hole. The
//   database independently forbids pure #000000 for OLED reasons, so both agree. The
//   darkest ink here is #050506.
//
// COLOUR SEMANTICS
//   Exactly one accent, used sparingly - orange, the game's own colour, reserved for the
//   control that FIRES something and for the live/on state. Red means fault. Everything
//   else is neutral. The previous six section colours encoded nothing and are gone.

namespace Col {
    // surfaces - near-black base, translucent white for elevation
    const ImVec4 bgDeep    = ImVec4(0.020f, 0.020f, 0.024f, 0.98f);   // #050506
    const ImVec4 elevated  = ImVec4(1.000f, 1.000f, 1.000f, 0.032f);
    const ImVec4 surface   = ImVec4(1.000f, 1.000f, 1.000f, 0.050f);
    const ImVec4 surfaceHi = ImVec4(1.000f, 1.000f, 1.000f, 0.090f);
    const ImVec4 surfaceAc = ImVec4(1.000f, 1.000f, 1.000f, 0.130f);

    // hairlines - ALWAYS rgba, never a solid grey
    const ImVec4 border    = ImVec4(1.000f, 1.000f, 1.000f, 0.080f);
    const ImVec4 borderHi  = ImVec4(1.000f, 1.000f, 1.000f, 0.140f);

    // ink
    const ImVec4 text      = ImVec4(0.929f, 0.929f, 0.937f, 1.00f);   // #EDEDEF
    const ImVec4 muted     = ImVec4(0.541f, 0.561f, 0.596f, 1.00f);   // #8A8F98
    const ImVec4 faint     = ImVec4(0.400f, 0.420f, 0.450f, 1.00f);

    // the single accent, plus one fault colour
    const ImVec4 accent    = ImVec4(1.000f, 0.545f, 0.239f, 1.00f);   // #FF8B3D
    const ImVec4 accentDim = ImVec4(1.000f, 0.545f, 0.239f, 0.16f);
    const ImVec4 accentMid = ImVec4(1.000f, 0.545f, 0.239f, 0.30f);
    const ImVec4 danger    = ImVec4(0.937f, 0.325f, 0.314f, 1.00f);
}

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // 4px scale, dense tier - this is a compact panel over a running game, not a page.
    s.WindowPadding    = ImVec2(16, 14);
    s.FramePadding     = ImVec2(10, 7);
    s.ItemSpacing      = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.IndentSpacing    = 16.0f;
    s.ScrollbarSize    = 10.0f;
    s.GrabMinSize      = 8.0f;

    s.WindowRounding    = 14.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 8.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 8.0f;

    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;   // borders are hairlines we draw, not frame outlines
    s.PopupBorderSize  = 1.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = Col::bgDeep;
    c[ImGuiCol_ChildBg]          = Col::elevated;
    c[ImGuiCol_PopupBg]          = ImVec4(0.043f, 0.043f, 0.051f, 0.99f);
    c[ImGuiCol_Border]           = Col::border;
    c[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]             = Col::text;
    c[ImGuiCol_TextDisabled]     = Col::muted;

    c[ImGuiCol_FrameBg]          = Col::surface;
    c[ImGuiCol_FrameBgHovered]   = Col::surfaceHi;
    c[ImGuiCol_FrameBgActive]    = Col::surfaceAc;

    c[ImGuiCol_TitleBg]          = Col::bgDeep;
    c[ImGuiCol_TitleBgActive]    = Col::bgDeep;
    c[ImGuiCol_TitleBgCollapsed] = Col::bgDeep;

    c[ImGuiCol_Button]           = Col::surface;
    c[ImGuiCol_ButtonHovered]    = Col::surfaceHi;
    c[ImGuiCol_ButtonActive]     = Col::surfaceAc;

    c[ImGuiCol_Header]           = Col::surface;
    c[ImGuiCol_HeaderHovered]    = Col::surfaceHi;
    c[ImGuiCol_HeaderActive]     = Col::surfaceAc;

    c[ImGuiCol_CheckMark]        = Col::accent;
    c[ImGuiCol_SliderGrab]       = Col::text;
    c[ImGuiCol_SliderGrabActive] = Col::accent;

    c[ImGuiCol_Separator]        = Col::border;
    c[ImGuiCol_SeparatorHovered] = Col::borderHi;
    c[ImGuiCol_SeparatorActive]  = Col::accent;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Col::surfaceHi;
    c[ImGuiCol_ScrollbarGrabHovered] = Col::surfaceAc;
    c[ImGuiCol_ScrollbarGrabActive]  = Col::accentMid;
}

// ---------------------------------------------------------------- widgets

/// Section label: muted, uppercase, letter-spaced. No rule, no coloured tick, no icon.
/// Whitespace separates; a line under every heading is visual debt.
void SectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, Col::faint);
    // Manual tracking: ImGui has no letter-spacing, and spaced small caps is the cheapest
    // single thing that makes a label read as designed rather than as debug output.
    char spaced[128] = { 0 };
    int n = 0;
    for (const char* p = text; *p && n < 125; ++p) {
        spaced[n++] = *p;
        if (*(p + 1)) spaced[n++] = ' ';
    }
    ImGui::TextUnformatted(spaced);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2));
}

/// Pill toggle. Flat translucent track, accent fill when on. No bevel, no gradient.
bool ToggleSwitch(const char* label, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 18.0f, w = 32.0f, r = h * 0.5f;

    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    bool hot = ImGui::IsItemHovered();

    ImU32 track = ImGui::GetColorU32(*v ? Col::accent : (hot ? Col::surfaceHi : Col::surface));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, r);
    if (!*v)
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Col::border), r, 0, 1.0f);

    // Knob: white on the accent for maximum contrast, muted when off.
    float kx = *v ? (p.x + w - r) : (p.x + r);
    dl->AddCircleFilled(ImVec2(kx, p.y + r), r - 4.0f,
                        ImGui::GetColorU32(*v ? ImVec4(1, 1, 1, 1) : Col::muted));

    ImGui::SameLine(0, 12);
    ImGui::PushStyleColor(ImGuiCol_Text, *v ? Col::text : Col::muted);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    return changed;
}

/// Solid accent button - the one control that fires something. Flat fill, no gradient.
bool PrimaryButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,        Col::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.000f, 0.620f, 0.353f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.902f, 0.463f, 0.176f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.075f, 0.043f, 0.020f, 1.0f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

/// Preset chip. Accent-tinted when live, hairline-outlined when not.
bool PresetChip(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        Col::accentMid);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::accentMid);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::accent);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::surface);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::muted);
    }
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col::surfaceAc);
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);

    if (!active) {   // hairline outline drawn after, so it sits on top of the fill
        ImGui::GetWindowDrawList()->AddRect(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            ImGui::GetColorU32(Col::border), ImGui::GetStyle().FrameRounding, 0, 1.0f);
    }
    return r;
}

// ============================ the menu ============================

int gLastPreset = -1;   // the mod's config has no "which preset" key; remember intent here

void DrawMenu() {
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CrabRandomizer", &gMenuOpen,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        return;
    }

    // ---- header ----
    ImGui::PushStyleColor(ImGuiCol_Text, Col::text);
    ImGui::TextUnformatted("CrabRandomizer");
    ImGui::PopStyleColor();

    {
        // Status as a dot plus one muted word, right-aligned. Quieter and more legible
        // than a coloured banner, and it never competes with the accent.
        const char* word = gGameHwnd ? "connected" : "no game";
        ImVec4 dotCol = gGameHwnd ? Col::accent : Col::danger;
        float tw = ImGui::CalcTextSize(word).x;
        ImGui::SameLine();
        float x = ImGui::GetWindowContentRegionMax().x - tw - 10.0f;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x - 6.0f, p.y + ImGui::GetTextLineHeight() * 0.5f), 3.0f,
            ImGui::GetColorU32(dotCol));
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(word);
        ImGui::PopStyleColor();
    }

    if (!gConfigLoaded) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, Col::danger);
        ImGui::TextWrapped("Config not found");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::faint);
        ImGui::TextWrapped("%s", gScriptsDir.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Retry", ImVec2(-1, 30))) LoadConfig();
        ImGui::End();
        return;
    }

    // ---- presets ----
    SectionLabel("PRESETS");
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
        if (i == 3) ImGui::NewLine();          // 3 per row keeps the panel narrow
        else if (i) ImGui::SameLine(0, 6);
        if (PresetChip(kPresets[i], gLastPreset == i)) {
            SendCommand(std::string("preset ") + kPresets[i]);
            gLastPreset = i;
        }
    }

    // ---- toggles ----
    SectionLabel("RANDOMIZE");
    for (int i = 0; i < IM_ARRAYSIZE(kToggles); ++i) {
        bool v = GetBool(kToggles[i]);
        if (ToggleSwitch(kToggleLabels[i], &v)) {
            SetConfig(kToggles[i], v ? "true" : "false");
            gLastPreset = -1;                   // no longer a clean preset
        }
        if (i == 4) ImGui::Dummy(ImVec2(0, 6)); // slot arrays | base loadout
    }

    // ---- modes ----
    SectionLabel("MODE");
    ImGui::PushItemWidth(-1);

    std::string coop = GetStr("coopMode", "independent");
    int ci = 0; for (int i = 0; i < 3; ++i) if (coop == kCoopModes[i]) ci = i;
    if (ImGui::Combo("##coop", &ci, kCoopModes, 3)) {
        SetConfig("coopMode", kCoopModes[ci]); gLastPreset = -1;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, Col::faint);
    if (ci == 0) ImGui::TextUnformatted("Each player rolls their own");
    if (ci == 1) ImGui::TextUnformatted("Everyone gets the same roll");
    if (ci == 2) ImGui::TextUnformatted("Players trade loadouts");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4));
    std::string roll = GetStr("rollMode", "weighted");
    int ri = 0; for (int i = 0; i < 3; ++i) if (roll == kRollModes[i]) ri = i;
    if (ImGui::Combo("##roll", &ri, kRollModes, 3)) {
        SetConfig("rollMode", kRollModes[ri]); gLastPreset = -1;
    }

    ImGui::Dummy(ImVec2(0, 4));
    int isl = GetInt("islandsBeforeRandomizing", 3);
    if (ImGui::SliderInt("##islands", &isl, 1, 10, "%d islands between shuffles")) {
        SetConfig("islandsBeforeRandomizing", std::to_string(isl)); gLastPreset = -1;
    }
    int mr = GetInt("minimumRarity", 1);
    if (ImGui::SliderInt("##minrar", &mr, 1, 4, "minimum rarity %d")) {
        SetConfig("minimumRarity", std::to_string(mr)); gLastPreset = -1;
    }

    if (ri == 0) {
        SectionLabel("RARITY WEIGHTS");
        const char* wk[] = { "rarityWeight1", "rarityWeight2", "rarityWeight3", "rarityWeight4" };
        const char* wl[] = { "Common %d", "Uncommon %d", "Rare %d", "Legendary %d" };
        for (int i = 0; i < 4; ++i) {
            int w = GetInt(wk[i], 25);
            ImGui::PushID(i);
            if (ImGui::SliderInt("##w", &w, 0, 100, wl[i])) {
                SetConfig(wk[i], std::to_string(w)); gLastPreset = -1;
            }
            ImGui::PopID();
        }
    }
    ImGui::PopItemWidth();

    // ---- actions ----
    ImGui::Dummy(ImVec2(0, 10));
    if (PrimaryButton("Shuffle now", ImVec2(-1, 36))) SendCommand("now");

    ImGui::Dummy(ImVec2(0, 6));
    float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    if (ImGui::Button("Undo",   ImVec2(third, 28))) SendCommand("undo");
    ImGui::SameLine();
    if (ImGui::Button("Status", ImVec2(third, 28))) SendCommand("status");
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(third, 28))) LoadConfig();

    // ---- footer ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, Col::faint);
    ImGui::TextUnformatted("F8 hide");
    ImGui::SameLine(0, 12);
    ImGui::TextUnformatted(gStatus.c_str());
    ImGui::PopStyleColor();

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
    ApplyTheme();

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
