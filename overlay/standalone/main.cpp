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

// ============================ theme: TACTILE / PHYSICAL ============================
//
// The panel is a piece of analogue hardware: a die-cast chassis, faceplates screwed down onto
// it, keys that travel when pressed, slide switches whose gate lights, faders in milled
// grooves. Depth comes entirely from layered fills plus 1px highlight / shadow edges - no
// blur, no image, no extra font, only ImGui + ImDrawList.
//
// TRANSPARENCY CONTRACT (do not break)
//   The window is keyed out with LWA_COLORKEY on RGB(0,0,0) and ImGui composites onto a
//   cleared TRANSPARENT BLACK buffer, so an on-screen value is roughly colour x alpha.
//   Two consequences drive every number below:
//     1. anything landing on exactly #000000 becomes a see-through hole, so the darkest ink
//        here is a very dark blue (#0B1014 = 0.043,0.063,0.078) and it is used for every
//        shadow edge in the file - there is no pure black anywhere, not even at alpha 0;
//     2. a colour key is binary, so a soft shadow drawn OUTSIDE the chassis would not fade
//        into the game, it would sit there as a hard dark rectangle. Nothing is drawn outside
//        the chassis rect, and every gradient wash is inset past the corner radius so it can
//        never bleed across a rounded corner into the transparent area.
//
// COLOUR SEMANTICS (the whole palette, and what each hue is allowed to mean)
//   Everything structural is neutral die-cast slate. Exactly three hues carry meaning and
//   none of them is ever used decoratively:
//       orange  - the one control that FIRES something. Nothing else, anywhere.
//       teal    - ON state: a lit switch gate, the live preset, the selected segment.
//       red     - fault: config missing, game link down.
//   sand is ink for the wordmark only, and the pale teal LCD backlight is the same hue family
//   as the on-state. There is no per-section colour coding: six lamps in six colours encode
//   nothing and are the loudest amateur tell a panel like this can have.
//
// TYPE
//   The atlas is a 13px bitmap font and may not be swapped, so there are exactly three sizes:
//   26px (an exact 2x of the atlas, so it stays crisp - a 19px upscale would be the SOFTEST
//   text on the panel, inverting the hierarchy it is trying to create), 13px for everything
//   that carries meaning, and 11px reserved for pure ornament. Hierarchy otherwise comes from
//   tracking, case and relief.
//
// GRID
//   4/8px throughout. Row heights are 24 / 32 / 48, gaps are 8 or 16, and ItemSpacing is
//   pushed to zero for the whole window so every position below is the literal pixel it says.

namespace Col {
    // chassis (die-cast, cool slate with a warm cast)
    const ImVec4 caseTop    = ImVec4(0.145f, 0.184f, 0.208f, 1.00f);
    const ImVec4 caseBot    = ImVec4(0.094f, 0.125f, 0.145f, 1.00f);
    const ImVec4 caseEdge   = ImVec4(0.247f, 0.310f, 0.345f, 1.00f);
    const ImVec4 shadow     = ImVec4(0.043f, 0.063f, 0.078f, 1.00f);   // darkest legal ink
    const ImVec4 white      = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);

    // raised faceplates and key caps
    const ImVec4 plateTop   = ImVec4(0.188f, 0.235f, 0.267f, 1.00f);
    const ImVec4 plateBot   = ImVec4(0.118f, 0.157f, 0.184f, 1.00f);
    const ImVec4 keyTop     = ImVec4(0.227f, 0.282f, 0.318f, 1.00f);
    const ImVec4 keyBot     = ImVec4(0.133f, 0.176f, 0.204f, 1.00f);
    const ImVec4 keyTopHot  = ImVec4(0.286f, 0.349f, 0.388f, 1.00f);
    const ImVec4 keyBotHot  = ImVec4(0.169f, 0.220f, 0.251f, 1.00f);

    // milled recesses / grooves
    const ImVec4 slotTop    = ImVec4(0.055f, 0.082f, 0.098f, 1.00f);
    const ImVec4 slotBot    = ImVec4(0.098f, 0.133f, 0.157f, 1.00f);

    // ink. textFaint is deliberately bright: at 0.435 it failed 4.5:1 against the milled
    // slots and every unselected segment label turned to mud.
    const ImVec4 text       = ImVec4(0.945f, 0.937f, 0.898f, 1.00f);   // warm white silkscreen
    const ImVec4 textDim    = ImVec4(0.749f, 0.776f, 0.784f, 1.00f);
    const ImVec4 textFaint  = ImVec4(0.600f, 0.651f, 0.671f, 1.00f);
    const ImVec4 sand       = ImVec4(0.949f, 0.855f, 0.678f, 1.00f);   // wordmark ink only

    // the three semantic accents
    const ImVec4 orange     = ImVec4(1.000f, 0.545f, 0.239f, 1.00f);   // fires something
    const ImVec4 teal       = ImVec4(0.235f, 0.851f, 0.749f, 1.00f);   // on / live
    const ImVec4 red        = ImVec4(0.918f, 0.353f, 0.310f, 1.00f);   // fault

    // backlit readouts (same hue family as the on-state, so no fourth hue enters the file)
    const ImVec4 lcdGlass   = ImVec4(0.043f, 0.071f, 0.067f, 1.00f);
    const ImVec4 lcdBg      = ImVec4(0.051f, 0.086f, 0.078f, 1.00f);
    const ImVec4 lcdOn      = ImVec4(0.561f, 0.949f, 0.878f, 1.00f);
}

// ---------------------------------------------------------------- geometry constants
//
// Everything is derived from these. There is not one magic offset in the layout below: the
// switch bank splits on kPanelW * 0.5f, its height is computed from its row count, and every
// right-hand element is measured back from the panel's right edge.

const float kPanelW    = 520.0f;   // 8 * 65: divides by 2 (260) and by 3 with 8px gaps (168)
const float kLabelW    = 136.0f;   // engraved label gutter for selectors and faders
const float kReadW     =  96.0f;   // backlit readout at the end of a fader row
const float kGap       =   8.0f;
const float kGapL      =  16.0f;
const float kRowH      =  24.0f;   // section plate, switch row, footer
const float kCtlH      =  32.0f;   // selector, fader, secondary key
const float kFireH     =  48.0f;   // the one key that fires
const float kHeadH     =  64.0f;   // header plate
const float kHintH     =  16.0f;   // fixed-height explanatory line (fixed so nothing reflows)
const float kCaseRound =  10.0f;

const float kTypeBig   =  26.0f;   // exactly 2x the atlas -> every texel lands on a 2x2 block
const float kTypeBody  =  13.0f;   // the atlas's native size: the floor for anything meaningful
const float kTypeMicro =  11.0f;   // ornament only (footer, masthead sub-line)

/// Which preset was last sent. The mod's config has no "which preset am I" key, so this is a
/// local memory of intent - without it the panel cannot answer "what am I running", which is
/// the first question anyone asks of a row of six identical keys.
int gLastPreset = -1;

/// Collapsed mode leaves only the header, the readout and the actions on screen. A 520x~1000
/// slab of opaque die-cast occludes a lot of play area, so it is worth being able to shrink
/// it mid-run. Default is expanded: nothing is hidden until the player asks for it.
bool gExpanded = true;

// ---------------------------------------------------------------- maths / colour

inline float Clamp01(float v)                 { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float Lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(Lerpf(a.x, b.x, t), Lerpf(a.y, b.y, t), Lerpf(a.z, b.z, t), Lerpf(a.w, b.w, t));
}
inline ImU32  U32(const ImVec4& c)           { return ImGui::GetColorU32(c); }
inline ImU32  U32A(const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
inline ImVec2 Off(const ImVec2& p, float x, float y) { return ImVec2(p.x + x, p.y + y); }
inline float  Snap(float v) { return (float)(int)(v + 0.5f); }   // bitmap glyphs smear off-pixel

// AddRectFilled reads "no corner bits" as "round everything", so a naive mask can silently
// round a corner we deliberately squared off.
inline ImDrawFlags MaskCorners(ImDrawFlags f, ImDrawFlags m) {
    ImDrawFlags r = f & m;
    return r ? r : ImDrawFlags_RoundCornersNone;
}

/// Per-widget eased value kept in the window's own state storage - no globals, no allocation.
/// This is what gives the switches and the selector shuttle real travel instead of a snap.
float Anim(const char* key, float target, float speed) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(ImGui::GetID(key), target);
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    float k = dt * speed;
    if (k > 1.0f) k = 1.0f;
    *v += (target - *v) * k;
    if (*v > target - 0.0015f && *v < target + 0.0015f) *v = target;
    return *v;
}

/// Triangle wave 0..1..0 over `period` seconds. Deliberately avoids <math.h>: imgui.h does
/// not pull it in and this translation unit must not grow dependencies.
float Pulse(float period) {
    double t = ImGui::GetTime() / (double)period;
    t -= (double)(long long)t;
    float f = (float)t;
    return f < 0.5f ? f * 2.0f : 2.0f - f * 2.0f;
}

/// ASCII upper-case into a small ring of buffers - faceplates are silkscreened in caps.
const char* Up(const char* s) {
    static char buf[6][80];
    static int slot = 0;
    slot = (slot + 1) % 6;
    char* d = buf[slot];
    int i = 0;
    for (; s[i] && i < 78; ++i) { char c = s[i]; d[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
    d[i] = '\0';
    return d;
}

// ---------------------------------------------------------------- letter-spaced text
//
// The built-in font is a 13px bitmap and may not be swapped, so hierarchy has to come from
// size, tracking and relief. Drawing glyph by glyph buys real letter-spacing, and tracked
// caps are the single biggest reason this reads as engraved hardware lettering rather than as
// debug text. Every glyph origin is snapped to a whole pixel: fractional tracking on a bitmap
// atlas lands glyphs between texels and turns the whole panel to mush.

float TextW(float size, const char* s, float sp) {
    ImFont* f = ImGui::GetFont();
    if (!f || f->FontSize <= 0.0f || !s) return 0.0f;
    float k = size / f->FontSize, w = 0.0f;
    int n = 0;
    for (const char* p = s; *p; ++p, ++n) w += f->GetCharAdvance((ImWchar)(unsigned char)*p) * k;
    if (n > 1) w += sp * (float)(n - 1);
    return w;
}

void TextSp(ImDrawList* dl, ImVec2 p, float size, float sp, ImU32 col, const char* s) {
    ImFont* f = ImGui::GetFont();
    if (!f || f->FontSize <= 0.0f || !s) return;
    float k = size / f->FontSize, x = p.x, y = Snap(p.y);
    char one[2] = { 0, 0 };
    for (const char* q = s; *q; ++q) {
        if (*q != ' ') { one[0] = *q; dl->AddText(f, size, ImVec2(Snap(x), y), col, one); }
        x += f->GetCharAdvance((ImWchar)(unsigned char)*q) * k + sp;
    }
}

/// Cut INTO the surface: a light edge catches underneath the stroke. Static labels only -
/// on interactive text the 0.085 white pass just softens the stroke for nothing.
void TextEngraved(ImDrawList* dl, ImVec2 p, float size, float sp, const ImVec4& col, const char* s) {
    TextSp(dl, Off(p, 0, 1), size, sp, U32A(Col::white, 0.085f), s);
    TextSp(dl, p, size, sp, U32(col), s);
}

/// Sitting ON the surface: a dark edge falls underneath the stroke. Used on anything raised,
/// where the drop shadow is buying real contrast rather than softness.
void TextPrinted(ImDrawList* dl, ImVec2 p, float size, float sp, const ImVec4& col, const char* s) {
    TextSp(dl, Off(p, 0, 1), size, sp, U32A(Col::shadow, 0.72f), s);
    TextSp(dl, p, size, sp, U32(col), s);
}

// ---------------------------------------------------------------- panel primitives

/// Vertical gradient with real rounded corners. AddRectFilledMultiColor cannot round, so the
/// shape is banded - flat top facet, gradient waist, flat bottom facet. The flats double as
/// the moulded facets of an injection-moulded key, which is exactly the look wanted.
void GradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& top, const ImVec4& bot,
              float r, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
    if (b.x - a.x < 1.0f || b.y - a.y < 1.0f) return;
    ImU32 t = U32(top), o = U32(bot);
    if (r <= 0.5f) { dl->AddRectFilledMultiColor(a, b, t, t, o, o); return; }
    if (b.y - a.y <= r * 2.0f + 1.0f) {                       // too short for a waist: two facets
        float mid = (a.y + b.y) * 0.5f;
        dl->AddRectFilled(a, ImVec2(b.x, mid), U32(Mix(top, bot, 0.20f)), r,
                          MaskCorners(flags, ImDrawFlags_RoundCornersTop));
        dl->AddRectFilled(ImVec2(a.x, mid), b, U32(Mix(top, bot, 0.80f)), r,
                          MaskCorners(flags, ImDrawFlags_RoundCornersBottom));
        return;
    }
    dl->AddRectFilled(a, ImVec2(b.x, a.y + r), t, r, MaskCorners(flags, ImDrawFlags_RoundCornersTop));
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + r), ImVec2(b.x, b.y - r), t, t, o, o);
    dl->AddRectFilled(ImVec2(a.x, b.y - r), b, o, r, MaskCorners(flags, ImDrawFlags_RoundCornersBottom));
}

/// Stands proud of the surface: gradient face, dark seating outline, 1px lit edge along the
/// top, 1px shadow edge along the bottom.
void Raised(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, const ImVec4& top, const ImVec4& bot,
            float hi = 0.20f, float lo = 0.55f) {
    GradRect(dl, a, b, top, bot, r);
    dl->AddRect(a, b, U32A(Col::shadow, 0.80f), r, 0, 1.0f);
    float in = r * 0.72f + 1.0f;
    if (b.x - a.x > in * 2.0f + 2.0f) {
        dl->AddLine(ImVec2(a.x + in, a.y + 1.5f), ImVec2(b.x - in, a.y + 1.5f), U32A(Col::white, hi), 1.0f);
        dl->AddLine(ImVec2(a.x + in, b.y - 1.5f), ImVec2(b.x - in, b.y - 1.5f), U32A(Col::shadow, lo), 1.0f);
    }
}

/// Milled into the surface: the light is inverted - shade at the top of the well, a
/// catch-light on the bottom lip.
void Recess(ImDrawList* dl, ImVec2 a, ImVec2 b, float r,
            const ImVec4& top = Col::slotTop, const ImVec4& bot = Col::slotBot) {
    GradRect(dl, a, b, top, bot, r);
    dl->AddRect(a, b, U32A(Col::shadow, 0.85f), r, 0, 1.0f);
    float in = r * 0.72f + 1.0f;
    if (b.x - a.x > in * 2.0f + 2.0f) {
        dl->AddLine(ImVec2(a.x + in, a.y + 1.5f), ImVec2(b.x - in, a.y + 1.5f), U32A(Col::shadow, 0.60f), 1.0f);
        dl->AddLine(ImVec2(a.x + in, b.y - 0.5f), ImVec2(b.x - in, b.y - 0.5f), U32A(Col::white, 0.09f), 1.0f);
    }
}

/// Countersunk screw head with a driver slot. Four hold the chassis together, two hold the
/// switch bank down. They are never used as ornament on a rule or a heading - a rivet with no
/// referent is just noise.
void Screw(ImDrawList* dl, ImVec2 c, float rad, float dx, float dy) {
    if (rad < 2.5f) return;
    dl->AddCircleFilled(Off(c, 0, 0.6f), rad + 0.6f, U32A(Col::shadow, 0.70f), 16);
    dl->AddCircleFilled(c, rad, U32(Col::plateBot), 16);
    dl->AddNgonFilled(c, rad - 1.0f, U32(Mix(Col::plateTop, Col::caseEdge, 0.40f)), 6);
    dl->AddCircleFilled(Off(c, 0, -0.5f), rad - 2.0f, U32(Mix(Col::plateTop, Col::text, 0.08f)), 12);
    float L = rad - 1.4f;
    dl->AddLine(ImVec2(c.x - dx * L, c.y - dy * L), ImVec2(c.x + dx * L, c.y + dy * L),
                U32A(Col::shadow, 0.92f), 1.6f);
    dl->AddCircle(c, rad, U32A(Col::shadow, 0.55f), 16, 1.0f);
}

/// Panel-mount indicator lamp. `on` drives the filament and the bloom around it.
void Lamp(ImDrawList* dl, ImVec2 c, float r, const ImVec4& col, float on) {
    on = Clamp01(on);
    if (on > 0.01f) {
        dl->AddCircleFilled(c, r * 3.0f, U32A(col, 0.06f * on), 20);
        dl->AddCircleFilled(c, r * 1.9f, U32A(col, 0.12f * on), 20);
    }
    dl->AddCircleFilled(Off(c, 0, 0.5f), r + 1.2f, U32A(Col::shadow, 0.75f), 18);   // bezel well
    dl->AddCircleFilled(c, r, U32(Mix(Col::slotTop, col, 0.10f + 0.30f * on)), 18);
    if (on > 0.01f) {
        dl->AddCircleFilled(c, r * 0.64f, U32A(col, 0.55f + 0.45f * on), 16);
        dl->AddCircleFilled(Off(c, -r * 0.22f, -r * 0.26f), r * 0.26f, U32A(Col::white, 0.55f * on), 10);
    }
    dl->AddCircle(c, r, U32A(Col::shadow, 0.70f), 18, 1.0f);
}

/// Brushed tooling marks. One hairline every few pixels, so the big flats do not read as flat
/// vector fill. This is the ONLY texture pass on the faceplates - a second one (vent grille,
/// scanlines) just competes with it under the controls. Caller keeps the span inside the
/// rounded silhouette.
void Brushed(ImDrawList* dl, ImVec2 a, ImVec2 b, float step) {
    int i = 0;
    for (float y = a.y; y < b.y; y += step, ++i)
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), U32A(Col::white, (i % 3 == 0) ? 0.022f : 0.011f), 1.0f);
}

/// Backlit readout: sunken glass, scan lines, and text with a faked bloom (four low-alpha
/// stamps under the crisp pass - the closest thing to a glow without a shader).
void Lcd(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* s, const ImVec4& col, int align) {
    Recess(dl, a, b, 4.0f, Col::lcdGlass, Col::lcdBg);
    for (float y = a.y + 3.0f; y < b.y - 1.0f; y += 3.0f)
        dl->AddLine(ImVec2(a.x + 1.0f, y), ImVec2(b.x - 1.0f, y), U32A(Col::shadow, 0.26f), 1.0f);
    dl->AddRectFilledMultiColor(Off(a, 2, 2), ImVec2(b.x - 2.0f, a.y + (b.y - a.y) * 0.45f),
                                U32A(Col::white, 0.055f), U32A(Col::white, 0.055f),
                                U32A(Col::white, 0.0f), U32A(Col::white, 0.0f));
    if (!s || !*s) return;
    float w = TextW(kTypeBody, s, 0.6f);
    float x = a.x + 8.0f;
    if (align == 1) x = (a.x + b.x) * 0.5f - w * 0.5f;
    if (align == 2) x = b.x - 8.0f - w;
    float y = (a.y + b.y) * 0.5f - kTypeBody * 0.5f;
    dl->PushClipRect(Off(a, 3, 2), Off(b, -3, -2), true);
    TextSp(dl, ImVec2(x - 1, y), kTypeBody, 0.6f, U32A(col, 0.16f), s);
    TextSp(dl, ImVec2(x + 1, y), kTypeBody, 0.6f, U32A(col, 0.16f), s);
    TextSp(dl, ImVec2(x, y - 1), kTypeBody, 0.6f, U32A(col, 0.16f), s);
    TextSp(dl, ImVec2(x, y + 1), kTypeBody, 0.6f, U32A(col, 0.16f), s);
    TextSp(dl, ImVec2(x, y),     kTypeBody, 0.6f, U32(col), s);
    dl->PopClipRect();
}

// ---------------------------------------------------------------- theme

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowPadding     = ImVec2(kGapL, kGapL);   // 16/16: the chassis border, in grid units
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(kGap, kGap);
    s.ItemInnerSpacing  = ImVec2(kGap, 4);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    s.WindowRounding    = kCaseRound;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 4.0f;

    // The chassis, its edge and all of its shading are drawn by hand, so ImGui's own
    // decorations are switched off rather than fought with.
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;

    // NOTE: the "invisible" entries below are alpha 0 but are still spelled with the legal
    // dark-blue ink rather than 0,0,0. ImGui early-outs on zero alpha so pure black would not
    // rasterise today - but raising either alpha later (a child window, a frame border) would
    // then punch a colour-keyed hole. Keeping the RGB legal removes the trap, and a grep for
    // pure black across this file stays empty.
    const ImVec4 clear = ImVec4(Col::shadow.x, Col::shadow.y, Col::shadow.z, 0.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = Col::caseBot;     // safety net beneath the painted chassis
    c[ImGuiCol_ChildBg]              = clear;
    c[ImGuiCol_PopupBg]              = Col::plateBot;
    c[ImGuiCol_Border]               = Col::shadow;
    c[ImGuiCol_BorderShadow]         = clear;

    c[ImGuiCol_Text]                 = Col::text;
    c[ImGuiCol_TextDisabled]         = Col::textFaint;

    c[ImGuiCol_FrameBg]              = Col::slotTop;
    c[ImGuiCol_FrameBgHovered]       = Col::slotBot;
    c[ImGuiCol_FrameBgActive]        = Col::plateBot;

    c[ImGuiCol_Button]               = Col::keyBot;
    c[ImGuiCol_ButtonHovered]        = Col::keyBotHot;
    c[ImGuiCol_ButtonActive]         = Col::plateBot;

    c[ImGuiCol_Header]               = ImVec4(Col::teal.x, Col::teal.y, Col::teal.z, 0.26f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(Col::teal.x, Col::teal.y, Col::teal.z, 0.38f);
    c[ImGuiCol_HeaderActive]         = ImVec4(Col::teal.x, Col::teal.y, Col::teal.z, 0.52f);

    c[ImGuiCol_CheckMark]            = Col::teal;
    c[ImGuiCol_SliderGrab]           = Col::keyTopHot;
    c[ImGuiCol_SliderGrabActive]     = Col::text;

    c[ImGuiCol_Separator]            = Col::shadow;
    c[ImGuiCol_SeparatorHovered]     = Col::caseEdge;
    c[ImGuiCol_SeparatorActive]      = Col::teal;

    c[ImGuiCol_ScrollbarBg]          = Col::slotTop;
    c[ImGuiCol_ScrollbarGrab]        = Col::keyTop;
    c[ImGuiCol_ScrollbarGrabHovered] = Col::keyTopHot;
    c[ImGuiCol_ScrollbarGrabActive]  = Col::teal;

    // StyleColorsDark() leaves several entries (table row backgrounds, drag/drop, nav) at
    // RGB(0,0,0) with alpha 0. They are inert under the current blend state, but the colour
    // key is unforgiving enough that none of them is left holding a black.
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        if (c[i].x == 0.0f && c[i].y == 0.0f && c[i].z == 0.0f)
            c[i] = ImVec4(clear.x, clear.y, clear.z, c[i].w);
}

// ---------------------------------------------------------------- layout helper

/// A pure vertical gap. ItemSpacing is zeroed for the whole window, so a Dummy of height h
/// advances exactly h - which is what lets every position in DrawMenu be a literal pixel.
/// Full width so the auto-resized window stays pinned to kPanelW.
inline void Gap(float h) { ImGui::Dummy(ImVec2(kPanelW, h)); }

// ---------------------------------------------------------------- custom widgets

/// Section heading as a milled nameplate: a slot cut into the chassis with engraved small
/// caps in it, then a machined rule running out to the right edge. `accent` is only allowed
/// to tint the first 48px of that rule, at 30% - a heading is a structural mark, not a place
/// to introduce a hue that encodes nothing.
void SectionHeader(const char* label, ImVec4 accent) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(kPanelW, kRowH));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const char* up = Up(label);
    ImVec2 a = p, b = ImVec2(p.x + TextW(kTypeBody, up, 1.6f) + 24.0f, p.y + kRowH);
    float mid = (a.y + b.y) * 0.5f;

    Recess(dl, a, b, 4.0f, ImVec4(0.063f, 0.090f, 0.106f, 1.0f), ImVec4(0.106f, 0.145f, 0.169f, 1.0f));
    TextEngraved(dl, ImVec2(a.x + 12.0f, mid - kTypeBody * 0.5f), kTypeBody, 1.6f, Col::textDim, up);

    float x0 = b.x + kGap, x1 = p.x + kPanelW;
    if (x1 > x0 + 8.0f) {
        dl->AddLine(ImVec2(x0, mid - 0.5f), ImVec2(x1, mid - 0.5f), U32A(Col::shadow, 0.85f), 1.0f);
        dl->AddLine(ImVec2(x0, mid + 0.5f), ImVec2(x1, mid + 0.5f), U32A(Col::white, 0.07f), 1.0f);
        dl->AddLine(ImVec2(x0, mid - 0.5f), ImVec2(x0 + 48.0f, mid - 0.5f), U32A(accent, 0.30f), 1.0f);
    }
}

/// Fixed-height explanatory line, indented into the control gutter so it hangs under the
/// thing it explains. Fixed height on purpose: a hint that appears and disappears with the
/// selection makes the whole panel below it jump.
void HintRow(const char* s) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(kPanelW, kHintH));
    if (!s || !*s) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    TextEngraved(dl, ImVec2(p.x + kLabelW, p.y + 1.0f), kTypeBody, 0.4f, Col::textFaint, s);
}

/// Illuminated slide switch on a full-width row. The shuttle physically travels and the gate
/// it uncovers lights teal, so a bank of eight reads as on/off at a glance - which a column
/// of identical tick boxes never does. The entire row is the hit target and lights on hover,
/// so this reads as a list of statements rather than a field of checkmarks.
bool ToggleSwitch(const char* label, bool* v, float rowW = 232.0f) {
    if (rowW < 96.0f) rowW = 96.0f;
    const float sw = 40.0f, sh = 20.0f, shut = 18.0f, kr = (sh - 4.0f) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, ImVec2(rowW, kRowH));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }   // flips on press, like a real switch
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();

    ImGui::PushID(label);
    float t  = Anim("##t", *v ? 1.0f : 0.0f, 18.0f);
    float hl = Anim("##h", hov ? 1.0f : 0.0f, 20.0f);
    ImGui::PopID();

    if (hl > 0.01f)
        dl->AddRectFilled(Off(p, -6.0f, 0.0f), ImVec2(p.x + rowW, p.y + kRowH),
                          U32A(Col::white, 0.035f * hl), 4.0f);

    ImVec2 a(p.x, p.y + (kRowH - sh) * 0.5f), b(p.x + sw, p.y + (kRowH + sh) * 0.5f);
    float mid = (a.y + b.y) * 0.5f;

    Recess(dl, a, b, sh * 0.5f);                                 // housing

    float sx = a.x + 2.0f + t * (sw - 4.0f - shut);
    ImVec2 gateA(a.x + 2.0f, a.y + 2.0f), gateB(sx, b.y - 2.0f);
    if (gateB.x - gateA.x > 3.0f) {                              // the lit gate
        GradRect(dl, gateA, gateB, Mix(Col::slotTop, Col::teal, 0.60f * t),
                 Mix(Col::slotBot, Col::teal, 0.90f * t), kr);
        if (t > 0.20f)
            dl->AddLine(ImVec2(gateA.x + 2.0f, gateA.y + 1.5f), ImVec2(gateB.x - 2.0f, gateA.y + 1.5f),
                        U32A(Col::white, 0.20f * t), 1.0f);
    }
    if (t < 0.85f)                                               // unlit side keeps an "off" ring
        dl->AddCircle(ImVec2(b.x - 8.0f, mid), 2.6f, U32A(Col::textFaint, 0.45f * (1.0f - t)), 12, 1.4f);

    float dy = act ? 1.0f : 0.0f;                                // shuttle
    ImVec2 ka(sx, a.y + 2.0f + dy), kb(sx + shut, b.y - 2.0f + dy);
    Raised(dl, ka, kb, kr, hov ? Col::keyTopHot : Col::keyTop, hov ? Col::keyBotHot : Col::keyBot, 0.26f, 0.60f);
    for (int i = -1; i <= 1; ++i) {                              // finger grooves
        float gx = (ka.x + kb.x) * 0.5f + (float)i * 4.0f;
        dl->AddLine(ImVec2(gx, ka.y + 4.0f), ImVec2(gx, kb.y - 4.0f), U32A(Col::shadow, 0.55f), 1.0f);
        dl->AddLine(ImVec2(gx + 1.0f, ka.y + 4.0f), ImVec2(gx + 1.0f, kb.y - 4.0f), U32A(Col::white, 0.10f), 1.0f);
    }

    dl->PushClipRect(ImVec2(p.x + sw + kGap, p.y), ImVec2(p.x + rowW, p.y + kRowH), true);
    TextSp(dl, ImVec2(p.x + sw + kGap, p.y + (kRowH - kTypeBody) * 0.5f), kTypeBody, 0.0f,
           U32(*v ? Col::text : Mix(Col::textFaint, Col::textDim, hl)), label);
    dl->PopClipRect();
    return changed;
}

/// Injection-moulded key cap on a skirt. The face travels down into the skirt on press and
/// the gap under it closes, so the click is felt as much as seen. Caps are neutral: identity
/// comes from the legend, state comes from `lit` (teal), and the only orange cap on the panel
/// is the one that fires.
bool KeyButtonEx(const char* id, const char* label, ImVec2 size, bool primary, float lit) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size);
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();
    lit = Clamp01(lit);

    float skirtH = primary ? 5.0f : 4.0f;
    float travel = Anim(id, act ? 1.0f : 0.0f, 32.0f) * (skirtH - 1.0f);
    float r = primary ? 7.0f : 6.0f;

    ImVec2 sa = p, sb = ImVec2(p.x + size.x, p.y + size.y);
    ImVec2 fa(p.x, p.y + travel), fb(p.x + size.x, p.y + size.y - skirtH + travel);

    // skirt / side wall
    GradRect(dl, sa, sb,
             primary ? Mix(Col::orange, Col::shadow, 0.62f) : Mix(Col::plateBot, Col::shadow, 0.35f),
             primary ? Mix(Col::orange, Col::shadow, 0.80f) : Mix(Col::shadow, Col::plateBot, 0.25f), r);
    dl->AddRect(sa, sb, U32A(Col::shadow, 0.90f), r, 0, 1.0f);

    // face
    ImVec4 fTop, fBot;
    if (primary) {
        fTop = Mix(Col::orange, Col::white,  hov ? 0.30f : 0.20f);
        fBot = Mix(Col::orange, Col::shadow, hov ? 0.20f : 0.30f);
    } else {
        fTop = Mix(hov ? Col::keyTopHot : Col::keyTop, Col::teal, 0.16f * lit);
        fBot = Mix(hov ? Col::keyBotHot : Col::keyBot, Col::teal, 0.10f * lit);
    }
    if (act) { fTop = Mix(fTop, Col::shadow, 0.16f); fBot = Mix(fBot, Col::shadow, 0.10f); }
    Raised(dl, fa, fb, r, fTop, fBot, primary ? 0.42f : 0.24f, primary ? 0.35f : 0.55f);

    // hazard chevrons on the primary cap: this is the control that actually fires something
    if (primary) {
        dl->PushClipRect(Off(fa, 1, 1), Off(fb, -1, -1), true);
        float fh = fb.y - fa.y, sk = fh * 0.42f;
        for (int s = 0; s < 2; ++s) {
            float ox = (s == 0) ? fa.x + 2.0f : fb.x - 34.0f;
            for (int i = 0; i < 3; ++i) {
                float x = ox + (float)i * 9.0f;
                dl->AddQuadFilled(ImVec2(x, fb.y), ImVec2(x + 4.0f, fb.y),
                                  ImVec2(x + 4.0f + sk, fa.y), ImVec2(x + sk, fa.y),
                                  U32A(Col::shadow, 0.15f));
            }
        }
        dl->PopClipRect();
    }

    // a lamp in the corner of the live cap, plus a lit bottom facet. This is the only way the
    // panel can answer "which preset am I running" without the player reading six legends.
    if (!primary && lit > 0.01f) {
        ImVec2 ba(fa.x + r, fb.y - 3.5f), bb(fb.x - r, fb.y - 1.5f);
        dl->AddRectFilledMultiColor(ImVec2(ba.x, ba.y - 8.0f), ImVec2(bb.x, ba.y),
                                    U32A(Col::teal, 0.0f), U32A(Col::teal, 0.0f),
                                    U32A(Col::teal, 0.16f * lit), U32A(Col::teal, 0.16f * lit));
        dl->AddRectFilled(ba, bb, U32A(Col::teal, 0.95f * lit), 1.0f);
        Lamp(dl, ImVec2(fb.x - 11.0f, fa.y + 10.0f), 3.0f, Col::teal, lit);
    }

    const char* up = Up(label);
    float sp = primary ? 2.6f : 1.6f;
    float fs = primary ? kTypeBig : kTypeBody;
    float tw = TextW(fs, up, sp);
    ImVec2 tp((fa.x + fb.x) * 0.5f - tw * 0.5f, (fa.y + fb.y) * 0.5f - fs * 0.5f);
    if (primary) {                                               // dark ink on a bright cap
        TextSp(dl, Off(tp, 0, 1), fs, sp, U32A(Col::white, 0.30f), up);
        TextSp(dl, tp, fs, sp, U32(Mix(Col::shadow, Col::orange, 0.10f)), up);
    } else {
        TextPrinted(dl, tp, fs, sp, (hov || lit > 0.5f) ? Col::text : Col::textDim, up);
    }
    return pressed;
}

/// The one control that fires. Signature preserved for the existing call site.
bool PrimaryButton(const char* label, ImVec2 size) {
    if (size.x <= 0.0f) size.x = kPanelW;
    if (size.y <= 0.0f) size.y = kFireH;
    return KeyButtonEx(label, label, size, true, 0.0f);
}

/// Three-position selector. Replaces a dropdown: every option is legible at rest, the detents
/// are visible, and the shuttle slides between them. No popup, no combo, no
/// click-to-discover-what-the-options-were. Each segment is its own hit target, so hover
/// tells you what you are about to pick before you commit.
bool Selector(const char* id, const char* label, const char* const* items, int count, int* idx) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!items || count <= 0 || !idx) { Gap(kCtlH); return false; }
    if (*idx < 0) *idx = 0;
    if (*idx > count - 1) *idx = count - 1;

    ImGui::PushID(id);
    ImVec2 a(p.x + kLabelW, p.y), b(p.x + kPanelW, p.y + kCtlH);
    float segW = (b.x - a.x - 6.0f) / (float)count;

    bool changed = false;
    int  hovIdx  = -1;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 3.0f + segW * (float)i, a.y));
        if (ImGui::InvisibleButton(items[i], ImVec2(segW, kCtlH)) && *idx != i) { *idx = i; changed = true; }
        if (ImGui::IsItemHovered()) hovIdx = i;
    }

    TextEngraved(dl, ImVec2(p.x, (a.y + b.y) * 0.5f - kTypeBody * 0.5f), kTypeBody, 1.1f,
                 Col::textDim, Up(label));
    Recess(dl, a, b, 6.0f);

    for (int i = 1; i < count; ++i) {                            // detents
        float x = a.x + 3.0f + segW * (float)i;
        dl->AddLine(ImVec2(x, a.y + 6.0f), ImVec2(x, b.y - 6.0f), U32A(Col::shadow, 0.75f), 1.0f);
        dl->AddLine(ImVec2(x + 1.0f, a.y + 6.0f), ImVec2(x + 1.0f, b.y - 6.0f), U32A(Col::white, 0.05f), 1.0f);
    }

    float t = Anim("shuttle", (float)*idx, 22.0f);
    ImVec2 ka(a.x + 3.0f + t * segW, a.y + 3.0f), kb(ka.x + segW, b.y - 3.0f);
    bool shutHot = (hovIdx == *idx);
    Raised(dl, ka, kb, 5.0f, shutHot ? Col::keyTopHot : Col::keyTop,
           shutHot ? Col::keyBotHot : Col::keyBot, 0.26f, 0.60f);
    dl->AddRectFilled(ImVec2(ka.x + 8.0f, kb.y - 4.0f), ImVec2(kb.x - 8.0f, kb.y - 2.5f),
                      U32A(Col::teal, 0.90f), 1.0f);            // the lit index bar = "this one"

    for (int i = 0; i < count; ++i) {
        const char* up = Up(items[i]);
        ImVec2 tp(a.x + 3.0f + segW * ((float)i + 0.5f) - TextW(kTypeBody, up, 0.8f) * 0.5f,
                  (a.y + b.y) * 0.5f - kTypeBody * 0.5f);
        if (i == *idx) TextPrinted(dl, tp, kTypeBody, 0.8f, Col::text, up);
        else           TextSp(dl, tp, kTypeBody, 0.8f, U32(i == hovIdx ? Col::textDim : Col::textFaint), up);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kCtlH));
    ImGui::PopID();
    return changed;
}

/// A fader in a milled groove: detent ticks, a lit throw behind the cap, a knurled cap with
/// an index line, and a backlit readout. Drag it, click anywhere on the track, or wheel it.
/// Hand-rolled rather than SliderInt because SliderBehavior lives in imgui_internal.h and
/// this file is allowed only imgui.h.
/// `names` (optional) turns the readout into a WORD - "LEGENDARY" says what 4 never will.
bool Fader(const char* id, const char* label, int* v, int lo, int hi,
           const char* const* names = nullptr, const char* suffix = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!v || hi <= lo) { Gap(kCtlH); return false; }
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;

    const float capW = 14.0f, capH = 20.0f;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushID(id);
    float tx0 = p.x + kLabelW;
    float tx1 = p.x + kPanelW - kReadW - kGapL;
    ImGui::SetCursorScreenPos(ImVec2(tx0, p.y));
    ImGui::InvisibleButton("track", ImVec2(tx1 - tx0, kCtlH));
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();

    float usable = (tx1 - tx0) - capW;
    if (usable < 1.0f) usable = 1.0f;
    bool changed = false;
    if (act || (hov && io.MouseWheel != 0.0f)) {
        int nv;
        if (act) nv = lo + (int)(Clamp01((io.MousePos.x - tx0 - capW * 0.5f) / usable) * (float)(hi - lo) + 0.5f);
        else     nv = *v + (io.MouseWheel > 0.0f ? 1 : -1);
        if (nv < lo) nv = lo;
        if (nv > hi) nv = hi;
        if (nv != *v) { *v = nv; changed = true; }
    }

    float t    = (float)(*v - lo) / (float)(hi - lo);
    float cx   = tx0 + t * usable;
    float midY = p.y + kCtlH * 0.5f;

    TextEngraved(dl, ImVec2(p.x, midY - kTypeBody * 0.5f), kTypeBody, 1.1f, Col::textDim, Up(label));

    ImVec2 ga(tx0, midY - 5.0f), gb(tx1, midY + 5.0f);
    Recess(dl, ga, gb, 5.0f);                                    // groove
    if (cx + capW * 0.5f > ga.x + 4.0f) {                        // lit throw: metal catching light,
        ImVec2 fa(ga.x + 2.0f, ga.y + 2.0f), fb(cx + capW * 0.5f, gb.y - 2.0f);   // not a hue
        GradRect(dl, fa, fb, Mix(Col::slotBot, Col::text, 0.42f), Mix(Col::slotBot, Col::text, 0.20f), 3.0f);
        dl->AddLine(ImVec2(fa.x + 1.0f, fa.y + 1.5f), ImVec2(fb.x - 1.0f, fa.y + 1.5f), U32A(Col::white, 0.22f), 1.0f);
    }
    int ticks = (hi - lo <= 12) ? (hi - lo) : 10;                // detent ticks under the groove
    if (ticks < 1) ticks = 1;
    for (int i = 0; i <= ticks; ++i) {
        float x = tx0 + capW * 0.5f + usable * ((float)i / (float)ticks);
        float len = (i == 0 || i == ticks) ? 5.0f : 3.5f;
        dl->AddLine(ImVec2(x, gb.y + 2.0f), ImVec2(x, gb.y + 2.0f + len), U32A(Col::shadow, 0.75f), 1.0f);
        dl->AddLine(ImVec2(x + 1.0f, gb.y + 2.0f), ImVec2(x + 1.0f, gb.y + 2.0f + len), U32A(Col::white, 0.06f), 1.0f);
    }

    float dy = act ? 1.0f : 0.0f;                                // cap
    ImVec2 ka(cx, midY - capH * 0.5f + dy), kb(cx + capW, midY + capH * 0.5f + dy);
    dl->AddRectFilled(Off(ka, -1.0f, 2.0f), Off(kb, 1.0f, 3.0f), U32A(Col::shadow, 0.45f), 4.0f);
    Raised(dl, ka, kb, 4.0f, (hov || act) ? Col::keyTopHot : Col::keyTop,
           (hov || act) ? Col::keyBotHot : Col::keyBot, 0.30f, 0.62f);
    for (int i = -1; i <= 1; ++i) {                              // knurling
        float gy = midY + dy + (float)i * 4.0f;
        dl->AddLine(ImVec2(ka.x + 2.5f, gy), ImVec2(kb.x - 2.5f, gy), U32A(Col::shadow, 0.45f), 1.0f);
        dl->AddLine(ImVec2(ka.x + 2.5f, gy + 1.0f), ImVec2(kb.x - 2.5f, gy + 1.0f), U32A(Col::white, 0.09f), 1.0f);
    }
    dl->AddRectFilled(ImVec2(ka.x + 1.5f, midY - 1.0f + dy), ImVec2(kb.x - 1.5f, midY + 0.5f + dy),
                      U32A(Col::text, (act || hov) ? 1.0f : 0.80f), 1.0f);

    char out[40];
    if (names) {
        int ni = *v - lo;
        if (ni < 0) ni = 0;
        if (ni > hi - lo) ni = hi - lo;
        snprintf(out, sizeof(out), "%s", Up(names[ni]));
    } else {
        snprintf(out, sizeof(out), "%d%s", *v, suffix ? suffix : "");
    }
    Lcd(dl, ImVec2(p.x + kPanelW - kReadW, midY - 12.0f), ImVec2(p.x + kPanelW, midY + 12.0f),
        out, Col::lcdOn, 1);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kCtlH));
    ImGui::PopID();
    return changed;
}

/// The four rarity weights as one normalised strip. Rarity is a LADDER, so it is drawn as a
/// value ladder - four brightness steps of the one readout colour - rather than the four
/// unrelated hues every other mod menu reaches for. Four independent 0-100 faders tell you
/// the numbers; this tells you the shape of the roll, which is the thing being tuned.
void ShareBar(const char* label, const int* w, int n) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(kPanelW, kRowH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!w) n = 0;
    if (n > 4) n = 4;

    float midY = p.y + kRowH * 0.5f;
    TextEngraved(dl, ImVec2(p.x, midY - kTypeBody * 0.5f), kTypeBody, 1.1f, Col::textDim, Up(label));

    ImVec2 a(p.x + kLabelW, p.y + 2.0f), b(p.x + kPanelW, p.y + kRowH - 2.0f);
    Recess(dl, a, b, 4.0f);

    int total = 0;
    for (int i = 0; i < n; ++i) total += (w[i] > 0 ? w[i] : 0);
    if (total <= 0) {
        TextSp(dl, ImVec2(a.x + 10.0f, (a.y + b.y) * 0.5f - kTypeBody * 0.5f), kTypeBody, 1.2f,
               U32(Col::red), "ALL WEIGHTS AT ZERO - NOTHING CAN ROLL");
        return;
    }

    const float step[4] = { 0.28f, 0.50f, 0.75f, 1.00f };
    float inner = (b.x - a.x) - 4.0f;
    float x = a.x + 2.0f;
    dl->PushClipRect(Off(a, 2, 2), Off(b, -2, -2), true);
    for (int i = 0; i < n; ++i) {
        int   wi  = (w[i] > 0 ? w[i] : 0);
        float seg = inner * (float)wi / (float)total;
        if (seg <= 0.5f) continue;
        ImVec4 fill = Mix(Col::slotBot, Col::lcdOn, step[i]);
        GradRect(dl, ImVec2(x, a.y + 2.0f), ImVec2(x + seg, b.y - 2.0f),
                 Mix(fill, Col::white, 0.18f), fill, 2.0f);
        if (x > a.x + 2.5f)                                      // a milled seam between shares
            dl->AddLine(ImVec2(x, a.y + 2.0f), ImVec2(x, b.y - 2.0f), U32A(Col::shadow, 0.85f), 1.0f);
        char pc[12];
        snprintf(pc, sizeof(pc), "%d%%", (int)((float)wi * 100.0f / (float)total + 0.5f));
        float pw = TextW(kTypeBody, pc, 0.4f);
        if (seg > pw + 10.0f)
            TextSp(dl, ImVec2(x + (seg - pw) * 0.5f, (a.y + b.y) * 0.5f - kTypeBody * 0.5f),
                   kTypeBody, 0.4f, U32A(Col::shadow, 0.85f), pc);
        x += seg;
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------- the chassis

void DrawChassis(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float r = kCaseRound;
    GradRect(dl, a, b, Col::caseTop, Col::caseBot, r);

    // Every wash below starts at a.y+r and ends at b.y-r. Between those the rounded rect is
    // full width, so nothing can spill across a corner arc into the keyed-out area.
    const float yTop = a.y + r, yBot = b.y - r;
    if (yBot > yTop) {
        Brushed(dl, ImVec2(a.x + 1.0f, yTop), ImVec2(b.x - 1.0f, yBot), 4.0f);
        float sun = (a.y + 96.0f < yBot) ? a.y + 96.0f : yBot;                       // sunset
        dl->AddRectFilledMultiColor(ImVec2(a.x + 2.0f, yTop), ImVec2(b.x - 2.0f, sun),
                                    U32A(Col::orange, 0.070f), U32A(Col::orange, 0.042f),
                                    U32A(Col::orange, 0.0f), U32A(Col::orange, 0.0f));
        float lag = (b.y - 74.0f > yTop) ? b.y - 74.0f : yTop;                        // lagoon bounce
        dl->AddRectFilledMultiColor(ImVec2(a.x + 2.0f, lag), ImVec2(b.x - 2.0f, yBot),
                                    U32A(Col::teal, 0.0f), U32A(Col::teal, 0.0f),
                                    U32A(Col::teal, 0.042f), U32A(Col::teal, 0.028f));
    }

    dl->AddRect(a, b, U32A(Col::caseEdge, 0.85f), r, 0, 1.0f);                       // machined edge
    dl->AddRect(Off(a, 1, 1), Off(b, -1, -1), U32A(Col::white, 0.055f), r - 1.0f, 0, 1.0f);
    dl->AddLine(ImVec2(a.x + r, a.y + 1.5f), ImVec2(b.x - r, a.y + 1.5f), U32A(Col::white, 0.16f), 1.0f);
    dl->AddLine(ImVec2(a.x + r, b.y - 1.5f), ImVec2(b.x - r, b.y - 1.5f), U32A(Col::shadow, 0.75f), 1.0f);

    Screw(dl, Off(a,  12.0f,  12.0f), 4.2f,  0.707f,  0.707f);
    Screw(dl, ImVec2(b.x - 12.0f, a.y + 12.0f), 4.2f, 0.707f, -0.707f);
    Screw(dl, ImVec2(a.x + 12.0f, b.y - 12.0f), 4.2f, 0.0f,    1.000f);
    Screw(dl, Off(b, -12.0f, -12.0f), 4.2f,  1.000f,  0.0f);
}

// ============================ the menu ============================

void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("CrabRandomizer", nullptr, flags)) { ImGui::End(); return; }

    // F9 fires a shuffle with no cursor travel at all, which matters because the alternative
    // is aiming at a button mid-fight. Polled rather than bound: this window is
    // WS_EX_NOACTIVATE so it never receives WM_KEYDOWN, and the message loop only dispatches
    // hotkey id 1. Edge-detected, and only live while the panel is open (F8 hides it), so it
    // cannot fire behind the player's back.
    {
        static bool sF9Down = false;
        bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !sF9Down && gConfigLoaded) SendCommand("now");
        sF9Down = down;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;

    // Every gap in this function is an explicit Gap() call, so ImGui's own inter-item spacing
    // would double-count. Zeroing it is what makes the layout below literal.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    // The chassis must be painted UNDER everything, but its height is only known once the
    // content has been laid out (the rarity bank appears and disappears with the roll mode,
    // and the whole body collapses). Split the draw list, build the controls into channel 1,
    // then paint channel 0 at the exact final size. No one-frame lag, no guessing from the
    // previous frame's size.
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);

    // ---- header plate: wordmark, status readout, link lamp, collapse key, close key ----
    ImGui::InvisibleButton("##drag", ImVec2(kPanelW, kHeadH));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 old = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(old.x + io.MouseDelta.x, old.y + io.MouseDelta.y));
    }
    // Re-read AFTER the possible move: SetWindowPos also shifts the layout cursor, so the
    // plate we are about to draw has to be measured from the new origin or it tears.
    const ImVec2 wp = ImGui::GetWindowPos();
    const float  x0 = wp.x + pad.x;
    const ImVec2 ha(x0, wp.y + pad.y), hb(x0 + kPanelW, wp.y + pad.y + kHeadH);

    Raised(dl, ha, hb, 8.0f, Col::plateTop, Col::plateBot, 0.22f, 0.55f);
    dl->AddRectFilledMultiColor(Off(ha, 8, 8), ImVec2(hb.x - 8.0f, ha.y + 26.0f),
                                U32A(Col::orange, 0.06f), U32A(Col::orange, 0.030f),
                                U32A(Col::orange, 0.0f), U32A(Col::orange, 0.0f));

    // masthead: 26px wordmark and an 11px unit line sharing one baseline
    const char* kMark = "CRAB RANDOMIZER";
    float markW = TextW(kTypeBig, kMark, 2.0f);
    TextEngraved(dl, ImVec2(ha.x + 16.0f, ha.y + 8.0f), kTypeBig, 2.0f, Col::sand, kMark);
    TextEngraved(dl, ImVec2(ha.x + 16.0f + markW + 12.0f, ha.y + 8.0f + kTypeBig - kTypeMicro - 2.0f),
                 kTypeMicro, 2.0f, Col::textFaint, "LOADOUT SHUFFLE UNIT");

    // top-right keys. Laid out right to left from the plate edge, so nothing is pinned to a
    // literal x and the pair cannot drift if kPanelW ever changes.
    float keyY = ha.y + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(hb.x - 16.0f - 24.0f, keyY));
    {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##close", ImVec2(24.0f, 24.0f))) gMenuOpen = false;
        bool chov = ImGui::IsItemHovered(), cact = ImGui::IsItemActive();
        float d = cact ? 2.0f : 0.0f;
        GradRect(dl, cp, Off(cp, 24.0f, 24.0f), Mix(Col::plateBot, Col::shadow, 0.45f), Col::shadow, 5.0f);
        ImVec2 ca(cp.x, cp.y + d), cb(cp.x + 24.0f, cp.y + 21.0f + d);
        Raised(dl, ca, cb, 5.0f,
               chov ? Mix(Col::keyTopHot, Col::red, 0.35f) : Col::keyTop,
               chov ? Mix(Col::keyBotHot, Col::red, 0.30f) : Col::keyBot, 0.24f, 0.55f);
        ImVec2 c((ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f);
        dl->AddLine(Off(c, -4, -3.4f), Off(c, 4, 4.6f), U32A(Col::shadow, 0.75f), 2.4f);
        dl->AddLine(Off(c,  4, -3.4f), Off(c, -4, 4.6f), U32A(Col::shadow, 0.75f), 2.4f);
        dl->AddLine(Off(c, -4, -4.4f), Off(c, 4, 3.6f), U32(chov ? Col::text : Col::textDim), 1.8f);
        dl->AddLine(Off(c,  4, -4.4f), Off(c, -4, 3.6f), U32(chov ? Col::text : Col::textDim), 1.8f);
    }
    if (gConfigLoaded) {
        ImGui::SetCursorScreenPos(ImVec2(hb.x - 16.0f - 24.0f - kGap - 64.0f, keyY));
        if (KeyButtonEx("##expand", gExpanded ? "Less" : "More", ImVec2(64.0f, 24.0f), false, 0.0f))
            gExpanded = !gExpanded;
    }

    // link state, right-aligned as a group so the caption is centred on its own lamp whatever
    // it says (LINK and DOWN are different widths - a shared literal x puts one of them off).
    const bool  linked = (gGameHwnd != nullptr);
    const char* linkTx = linked ? "LINK" : "DOWN";
    float linkW  = 10.0f + 6.0f + TextW(kTypeBody, linkTx, 1.0f);
    float linkX  = hb.x - 16.0f - linkW;
    float rowY   = ha.y + 40.0f;                       // second header row, 20px tall
    Lamp(dl, ImVec2(linkX + 5.0f, rowY + 10.0f), 4.0f, linked ? Col::teal : Col::red,
         linked ? 0.90f : (0.35f + 0.65f * Pulse(1.1f)));
    TextEngraved(dl, ImVec2(linkX + 16.0f, rowY + 10.0f - kTypeBody * 0.5f), kTypeBody, 1.0f,
                 linked ? Col::textFaint : Col::red, linkTx);

    Lcd(dl, ImVec2(ha.x + 16.0f, rowY), ImVec2(linkX - 12.0f, rowY + 20.0f),
        gStatus.c_str(), Col::lcdOn, 0);

    ImGui::SetCursorScreenPos(ImVec2(x0, hb.y + kGapL));

    if (!gConfigLoaded) {
        // ---- fault state ----
        ImVec2 ea = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPanelW, 88.0f));
        Recess(dl, ea, ImVec2(ea.x + kPanelW, ea.y + 88.0f), 7.0f);
        Lamp(dl, Off(ea, 24.0f, 24.0f), 4.8f, Col::red, 0.35f + 0.65f * Pulse(0.9f));
        TextPrinted(dl, Off(ea, 40.0f, 17.0f), kTypeBody, 1.6f, Col::red, "CONFIG NOT LOADED");
        ImGui::SetCursorScreenPos(Off(ea, 40.0f, 40.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Col::textFaint);
        ImGui::PushTextWrapPos(ea.x + kPanelW - 24.0f);
        ImGui::TextWrapped("looked in: %s", gScriptsDir.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(x0, ea.y + 88.0f + kGap));
        if (KeyButtonEx("retry", "Retry", ImVec2(168.0f, kCtlH), false, 0.0f)) LoadConfig();
        ImGui::SetCursorScreenPos(ImVec2(x0, ea.y + 88.0f + kGap + kCtlH + kGapL));
    } else {
        // ---- actions, hoisted to the top ----
        // The highest-frequency control on the panel sits at a FIXED offset from the window
        // origin. Down at the bottom it moved ~130px the moment the roll mode stopped being
        // "weighted" and the rarity bank vanished; muscle memory cannot survive that.
        if (PrimaryButton("Shuffle now", ImVec2(kPanelW, kFireH))) SendCommand("now");
        Gap(kGap);
        {
            const float kw = (kPanelW - kGap * 2.0f) / 3.0f;     // 168 exactly
            ImVec2 gp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(kPanelW, kCtlH));
            ImGui::SetCursorScreenPos(gp);
            if (KeyButtonEx("undo", "Undo", ImVec2(kw, kCtlH), false, 0.0f)) SendCommand("undo");
            ImGui::SetCursorScreenPos(ImVec2(gp.x + kw + kGap, gp.y));
            if (KeyButtonEx("status", "Status", ImVec2(kw, kCtlH), false, 0.0f)) SendCommand("status");
            ImGui::SetCursorScreenPos(ImVec2(gp.x + (kw + kGap) * 2.0f, gp.y));
            if (KeyButtonEx("reload", "Reload", ImVec2(kw, kCtlH), false, 0.0f)) LoadConfig();
            ImGui::SetCursorScreenPos(ImVec2(gp.x, gp.y + kCtlH));
        }
    }

    if (gConfigLoaded && gExpanded) {
        Gap(kGapL);

        // ---- presets: six identical keys, told apart by their legend, and the live one lit ----
        SectionHeader("PRESETS", Col::teal);
        Gap(kGap);
        {
            const float kw = (kPanelW - kGap * 2.0f) / 3.0f;
            ImVec2 gp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(kPanelW, kCtlH * 2.0f + kGap));
            for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
                ImGui::SetCursorScreenPos(ImVec2(gp.x + (float)(i % 3) * (kw + kGap),
                                                 gp.y + (float)(i / 3) * (kCtlH + kGap)));
                float lit = Anim(kPresets[i], (i == gLastPreset) ? 1.0f : 0.0f, 14.0f);
                if (KeyButtonEx(kPresets[i], kPresets[i], ImVec2(kw, kCtlH), false, lit)) {
                    gLastPreset = i;
                    SendCommand(std::string("preset ") + kPresets[i]);
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(gp.x, gp.y + kCtlH * 2.0f + kGap));
        }
        Gap(kGapL);

        // ---- switch bank: two columns split by a milled groove (the old '|' divider) ----
        SectionHeader("WHAT GETS RANDOMIZED", Col::teal);
        Gap(kGap);
        {
            const int   leftRows = 5;                                   // slot arrays | base loadout
            const float bpad = kGapL, capH = kGapL, gutter = 24.0f;
            const float colW  = (kPanelW - bpad * 2.0f - gutter) * 0.5f;
            const float bankH = bpad * 2.0f + capH + kGap + (float)leftRows * kRowH;

            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(kPanelW, bankH));
            ImVec2 ba = bp, bb = ImVec2(bp.x + kPanelW, bp.y + bankH);
            Raised(dl, ba, bb, 8.0f, Col::plateTop, Col::plateBot, 0.20f, 0.55f);
            Brushed(dl, Off(ba, 8, 8), Off(bb, -8, -8), 5.0f);

            const float leftX  = ba.x + bpad;
            const float rightX = ba.x + kPanelW - bpad - colW;
            const float divX   = ba.x + kPanelW * 0.5f;                 // derived, not pinned
            const float rowTop = ba.y + bpad + capH + kGap;

            Recess(dl, ImVec2(divX - 2.0f, ba.y + 12.0f), ImVec2(divX + 2.0f, bb.y - 12.0f), 2.0f);
            TextEngraved(dl, ImVec2(leftX,  ba.y + bpad), kTypeBody, 1.8f, Col::textFaint, "SLOT ARRAYS");
            TextEngraved(dl, ImVec2(rightX, ba.y + bpad), kTypeBody, 1.8f, Col::textFaint, "BASE LOADOUT");

            for (int i = 0; i < IM_ARRAYSIZE(kToggles); ++i) {
                bool left = (i < leftRows);
                ImGui::SetCursorScreenPos(ImVec2(left ? leftX : rightX,
                                                 rowTop + (float)(left ? i : i - leftRows) * kRowH));
                bool v = GetBool(kToggles[i]);
                if (ToggleSwitch(kToggleLabels[i], &v, colW))
                    SetConfig(kToggles[i], v ? "true" : "false");
            }

            Screw(dl, Off(ba,  9.0f,  9.0f), 3.4f, 0.707f,  0.707f);
            Screw(dl, Off(bb, -9.0f, -9.0f), 3.4f, 0.707f, -0.707f);
            ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bankH));
        }
        Gap(kGapL);

        // ---- modes ----
        SectionHeader("MODES", Col::teal);
        Gap(kGap);

        std::string coop = GetStr("coopMode", "independent");
        int ci = 0; for (int i = 0; i < 3; ++i) if (coop == kCoopModes[i]) ci = i;
        if (Selector("coop", "Co-op", kCoopModes, 3, &ci)) SetConfig("coopMode", kCoopModes[ci]);
        HintRow(ci == 1 ? "ONE ROLL - EVERY PLAYER GETS THE SAME RESULT, THE HOST APPLIES IT"
              : ci == 2 ? "PLAYERS TRADE LOADOUTS - NEEDS TWO OR MORE PLAYERS"
                        : "EACH PLAYER ROLLS THEIR OWN LOADOUT");
        Gap(kGap);

        std::string roll = GetStr("rollMode", "weighted");
        int ri = 0; for (int i = 0; i < 3; ++i) if (roll == kRollModes[i]) ri = i;
        if (Selector("roll", "Roll", kRollModes, 3, &ri)) SetConfig("rollMode", kRollModes[ri]);
        HintRow(ri == 1 ? "REROLLS STAY IN THE TIER THE SLOT ALREADY HAD"
              : ri == 2 ? "EVERY CANDIDATE EQUALLY LIKELY - WEIGHTS IGNORED"
                        : "RARITY BIASED BY THE WEIGHTS BELOW");
        Gap(kGap);

        int isl = GetInt("islandsBeforeRandomizing", 3);
        if (Fader("islands", "Islands / shuffle", &isl, 1, 10))
            SetConfig("islandsBeforeRandomizing", std::to_string(isl));

        static const char* kRarityNames[4] = { "Common", "Uncommon", "Rare", "Legendary" };
        int mr = GetInt("minimumRarity", 1);
        if (Fader("minrar", "Minimum rarity", &mr, 1, 4, kRarityNames))
            SetConfig("minimumRarity", std::to_string(mr));

        // ---- rarity weights (weighted roll only) ----
        if (ri == 0) {
            Gap(kGapL);
            SectionHeader("RARITY WEIGHTS", Col::teal);
            Gap(kGap);
            const char* wk[4] = { "rarityWeight1", "rarityWeight2", "rarityWeight3", "rarityWeight4" };
            int wv[4];
            for (int i = 0; i < 4; ++i) wv[i] = GetInt(wk[i], 25);
            for (int i = 0; i < 4; ++i)
                if (Fader(wk[i], kRarityNames[i], &wv[i], 0, 100, nullptr, "%"))
                    SetConfig(wk[i], std::to_string(wv[i]));
            ShareBar("Resulting share", wv, 4);
        }
        Gap(kGap);
    }

    // ---- footer: silkscreened service note ----
    {
        ImVec2 fp = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPanelW, kRowH));
        dl->AddLine(ImVec2(fp.x, fp.y + 3.0f), ImVec2(fp.x + kPanelW, fp.y + 3.0f), U32A(Col::shadow, 0.55f), 1.0f);
        dl->AddLine(ImVec2(fp.x, fp.y + 4.0f), ImVec2(fp.x + kPanelW, fp.y + 4.0f), U32A(Col::white, 0.05f), 1.0f);
        TextEngraved(dl, ImVec2(fp.x + 2.0f, fp.y + 11.0f), kTypeMicro, 1.4f, Col::textFaint,
                     "F8  HIDE / SHOW      F9  SHUFFLE");
        const char* note = "CHANGES ARE APPLIED BY THE LUA MOD";
        TextEngraved(dl, ImVec2(fp.x + kPanelW - TextW(kTypeMicro, note, 1.4f) - 2.0f, fp.y + 11.0f),
                     kTypeMicro, 1.4f, Col::textFaint, note);

        // One-shot fit check, MEASURED rather than guessed at: the window is auto-sized and
        // has no scrollbar, so on a short display a fully expanded panel would simply run off
        // the bottom with no way to reach the last section. If that is the case on the very
        // first frame, start collapsed instead. Fires once per process; the player can always
        // expand again, and the one frame of overflow is never seen.
        static bool sFitChecked = false;
        if (!sFitChecked) {
            sFitChecked = true;
            float panelH = (fp.y + kRowH + pad.y) - wp.y;          // the true laid-out height
            if (gExpanded && panelH > io.DisplaySize.y - 32.0f) gExpanded = false;
        }

        // ---- paint the chassis underneath, at its exact final size ----
        // The window auto-fits to CursorMaxPos + WindowPadding, and the footer Dummy above is
        // the last item submitted, so this rect IS the window - computed, not measured a frame
        // late. ImGui's content clip rect is inset by half the window padding, which would
        // shave the machined edge and the rounded corners clean off the left and right of the
        // chassis, so the clip is replaced (not intersected) for this one draw.
        ImVec2 caseA = wp;
        ImVec2 caseB(wp.x + kPanelW + pad.x * 2.0f, fp.y + kRowH + pad.y);
        split.SetCurrentChannel(dl, 0);
        dl->PushClipRect(caseA, caseB, false);
        DrawChassis(dl, caseA, caseB);
        dl->PopClipRect();
        split.Merge(dl);
    }

    ImGui::PopStyleVar();
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
