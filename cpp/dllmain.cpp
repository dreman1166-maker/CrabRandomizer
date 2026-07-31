// CrabRandomizerUI - a UE4SS C++ mod that draws a real clickable menu for CrabRandomizer.
//
// WHY THIS EXISTS
//   UE4SS exposes no ImGui/drawing bindings to Lua mods (upstream issue #1072, still
//   open), so the Lua mod's "menu" can only be printed text. C++ mods CAN use ImGui, so
//   this provides checkboxes, sliders and buttons.
//
// WHAT IT IS NOT
//   register_tab() adds a tab to UE4SS's own debug window. This is NOT painted over the
//   game viewport - there is no UE4SS API for that. A true in-game overlay needs a UMG
//   widget cooked into a .pak with the game modkit.
//
// HOW IT TALKS TO THE LUA MOD
//   Deliberately through files, not through UE4SS internals:
//     randoconfig.txt   settings, the same file the Lua mod already reads/writes
//     uicommand.txt     one-line action ("now", "undo", "reload"), polled and deleted
//                       by the Lua mod
//   That keeps every tested behaviour in Lua (106 tests) and this layer purely
//   presentational, with no dependency on undocumented C++/Lua interop that would break
//   on a UE4SS update.

#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Ordered so the file keeps a stable, human-readable shape when rewritten.
struct ConfigFile
{
    std::vector<std::string> lines;                 // verbatim, comments preserved
    std::map<std::string, size_t> keyToLine;        // key -> index into lines
    std::map<std::string, std::string> values;

    bool load(const fs::path& p)
    {
        lines.clear(); keyToLine.clear(); values.clear();
        std::ifstream in(p);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line))
        {
            lines.push_back(line);
            std::string clean = line;
            if (const auto c = clean.find(';'); c != std::string::npos) clean = clean.substr(0, c);
            clean = trim(clean);
            if (const auto eq = clean.find('='); eq != std::string::npos)
            {
                std::string k = trim(clean.substr(0, eq));
                std::string v = trim(clean.substr(eq + 1));
                if (!k.empty())
                {
                    keyToLine[k] = lines.size() - 1;
                    values[k] = v;
                }
            }
        }
        return true;
    }

    bool save(const fs::path& p) const
    {
        std::ofstream out(p, std::ios::trunc);
        if (!out) return false;
        for (const auto& l : lines) out << l << "\n";
        return true;
    }

    void set(const std::string& key, const std::string& value)
    {
        if (const auto it = keyToLine.find(key); it != keyToLine.end())
        {
            lines[it->second] = key + "=" + value;
        }
        else
        {
            lines.push_back(key + "=" + value);
            keyToLine[key] = lines.size() - 1;
        }
        values[key] = value;
    }

    bool getBool(const std::string& key, bool fallback = false) const
    {
        const auto it = values.find(key);
        if (it == values.end()) return fallback;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v == "true";
    }

    int getInt(const std::string& key, int fallback = 0) const
    {
        const auto it = values.find(key);
        if (it == values.end()) return fallback;
        try { return std::stoi(it->second); } catch (...) { return fallback; }
    }

    std::string getStr(const std::string& key, const std::string& fallback = {}) const
    {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
};

} // namespace

class CrabRandomizerUI : public RC::CppUserModBase
{
public:
    CrabRandomizerUI() : RC::CppUserModBase()
    {
        ModName = STR("CrabRandomizerUI");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Clickable menu for CrabRandomizer");
        ModAuthors = STR("dreman1166-maker");

        // MUST come before register_tab or UE4SS crashes.
        UE4SS_ENABLE_IMGUI()

        register_tab(STR("CrabRandomizer"), [](CppUserModBase* instance) {
            static_cast<CrabRandomizerUI*>(instance)->render();
        });
    }

    ~CrabRandomizerUI() override = default;

    void on_unreal_init() override
    {
        // Resolve paths once Unreal is up; UE4SS's working directory is the game's
        // Win64 folder, which is where Mods/ lives.
        m_configPath  = fs::path("Mods") / "CrabRandomizer" / "Scripts" / "randoconfig.txt";
        m_commandPath = fs::path("Mods") / "CrabRandomizer" / "Scripts" / "uicommand.txt";
        reload();
    }

private:
    fs::path m_configPath;
    fs::path m_commandPath;
    ConfigFile m_cfg;
    bool m_loaded = false;
    std::string m_status = "not loaded";

    void reload()
    {
        m_loaded = m_cfg.load(m_configPath);
        m_status = m_loaded ? "loaded " + m_configPath.string()
                            : "COULD NOT READ " + m_configPath.string();
    }

    void persist()
    {
        if (!m_cfg.save(m_configPath))
        {
            m_status = "FAILED to write config";
            return;
        }
        // Ask the Lua mod to re-read it; nothing is applied until it does.
        writeCommand("reload");
        m_status = "saved";
    }

    void writeCommand(const std::string& cmd)
    {
        std::ofstream out(m_commandPath, std::ios::trunc);
        if (!out) { m_status = "FAILED to write command file"; return; }
        out << cmd << "\n";
        m_status = "sent: " + cmd;
    }

    // Checkbox bound directly to a config key.
    void boolRow(const char* label, const char* key)
    {
        bool v = m_cfg.getBool(key);
        if (ImGui::Checkbox(label, &v))
        {
            m_cfg.set(key, v ? "true" : "false");
            persist();
        }
    }

    void intRow(const char* label, const char* key, int lo, int hi)
    {
        int v = m_cfg.getInt(key, lo);
        if (ImGui::SliderInt(label, &v, lo, hi))
        {
            m_cfg.set(key, std::to_string(v));
            persist();
        }
    }

    void comboRow(const char* label, const char* key, const std::vector<const char*>& options)
    {
        const std::string cur = m_cfg.getStr(key, options.empty() ? "" : options[0]);
        int idx = 0;
        for (size_t i = 0; i < options.size(); ++i)
            if (cur == options[i]) { idx = static_cast<int>(i); break; }

        if (ImGui::Combo(label, &idx, options.data(), static_cast<int>(options.size())))
        {
            m_cfg.set(key, options[idx]);
            persist();
        }
    }

    void render()
    {
        if (!m_loaded)
        {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "CrabRandomizer config not found.");
            ImGui::TextWrapped("Expected: %s", m_configPath.string().c_str());
            ImGui::TextWrapped("Install the Lua mod first, then press Reload.");
            if (ImGui::Button("Reload")) reload();
            return;
        }

        ImGui::TextDisabled("Changes are written to randoconfig.txt and applied by the Lua mod.");
        ImGui::Separator();

        if (ImGui::Button("Reroll Now"))  writeCommand("now");
        ImGui::SameLine();
        if (ImGui::Button("Undo Last"))   writeCommand("undo");
        ImGui::SameLine();
        if (ImGui::Button("Reload File")) reload();

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Presets"))
        {
            const char* presets[] = { "off", "gentle", "default", "chaos", "mirror", "swap" };
            for (const char* p : presets)
            {
                if (ImGui::Button(p)) writeCommand(std::string("preset ") + p);
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }

        if (ImGui::CollapsingHeader("Inventory", ImGuiTreeNodeFlags_DefaultOpen))
        {
            boolRow("Weapon mods",  "randomizeWeaponMods");
            boolRow("Ability mods", "randomizeAbilityMods");
            boolRow("Melee mods",   "randomizeMeleeMods");
            boolRow("Grenade mods", "randomizeGrenadeMods");
            boolRow("Perks",        "randomizePerks");
            boolRow("Relics",       "randomizeRelics");
        }

        if (ImGui::CollapsingHeader("Base loadout"))
        {
            ImGui::TextDisabled("Rerolls the gun/ability/melee themselves - a big swing.");
            boolRow("Weapon",  "randomizeWeapon");
            boolRow("Ability", "randomizeAbility");
            boolRow("Melee",   "randomizeMelee");
        }

        if (ImGui::CollapsingHeader("Rolling", ImGuiTreeNodeFlags_DefaultOpen))
        {
            comboRow("Roll mode", "rollMode", { "weighted", "withinRarity", "uniform" });
            intRow("Islands between rerolls", "islandsBeforeRandomizing", 1, 20);
            intRow("Minimum rarity", "minimumRarity", 1, 4);
            ImGui::TextDisabled("Rarity weights (weighted mode only)");
            intRow("Common",    "rarityWeight1", 0, 100);
            intRow("Rare",      "rarityWeight2", 0, 100);
            intRow("Epic",      "rarityWeight3", 0, 100);
            intRow("Legendary", "rarityWeight4", 0, 100);
        }

        if (ImGui::CollapsingHeader("Co-op"))
        {
            comboRow("Co-op mode", "coopMode", { "independent", "mirror", "swap" });
            ImGui::TextWrapped("Only the HOST applies changes. A joining client detects it "
                               "has no authority and skips.");
        }

        if (ImGui::CollapsingHeader("Advanced"))
        {
            boolRow("Dry run (log only, change nothing)", "dryRun");
            boolRow("Write log file", "logToFile");
            boolRow("In-game chat commands", "chatCommands");
            intRow("Shuffle delay after island (ms)", "shuffleDelayMs", 0, 10000);
            ImGui::TextDisabled("Delay exists because shuffling during the island "
                                "transition crashed clients. 0 = immediate.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", m_status.c_str());
    }
};

#define CRABRANDOMIZERUI_API __declspec(dllexport)
extern "C"
{
    CRABRANDOMIZERUI_API RC::CppUserModBase* start_mod() { return new CrabRandomizerUI(); }
    CRABRANDOMIZERUI_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
