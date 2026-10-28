// Mods in <user data>\mods\<folder>\ and their on/off state, order, and presets in <user data>\mods.ini.
//
// mods.ini:
//   [Enabled]
//   <folder> = 1        ; 1 on, 0 off, missing means off
//   [Order]
//   order = a,b,c       ; later wins. Without it the game sorts folders by name.
//   [Presets]
//   <folder> = <preset>
//   <folder>:<category> = <preset>
//
// Plain C++, no Win32.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mods {

struct PresetCategory {
    std::string category;               // empty for single-category graphic packs
    std::vector<std::string> options;   // preset names in declaration order
    std::string default_option;         // option marked default = 1, or options.front()
    std::string active;                 // currently selected option
};

struct Mod {
    std::string folder;       // folder name under mods\, also the mods.ini key
    std::string name;         // rules.txt [Definition] name, else the folder name
    std::string description;  // rules.txt description, may be empty
    std::string version;      // rules.txt version, may be empty
    bool has_rules = false;
    bool has_content = false;
    bool has_aoc = false;
    bool enabled = false;

    uint32_t content_files = 0;
    uint32_t aoc_files = 0;
    uint32_t patch_files = 0;
    uint32_t shader_files = 0;
    uint32_t texture_rules = 0;
    uint32_t overridden_files = 0;
    std::vector<std::string> overrides_mods;
    std::vector<std::string> overridden_by_mods;
    std::vector<PresetCategory> presets;
};

struct State {
    std::vector<Mod> list;  // display and load order, first loads first, last wins
    bool has_order = false; // mods.ini had an [Order] line
};

std::filesystem::path mods_dir(const std::filesystem::path& user_root);
std::filesystem::path ini_path(const std::filesystem::path& user_root);

State load(const std::filesystem::path& user_root);

// Rewrites [Enabled], [Presets], and [Order] when write_order is set (or it was there before).
// Other sections and keys for folders that are gone are kept.
bool save(const std::filesystem::path& user_root, const State& state, bool write_order, std::string& err);

int enabled_count(const State& state);

// Installs a mod folder or .zip archive into <user_root>/mods/<ModName>, enables it in mods.ini,
// and returns the installed folder name in installed_folder_out.
bool install_mod(const std::filesystem::path& user_root,
                 const std::filesystem::path& src_path,
                 std::string& installed_folder_out,
                 std::string& err);

struct ImportReport {
    std::vector<std::string> imported;  // folders written under mods\, all left switched off
    int skipped_other_games = 0;        // rules.txt found, but the pack is for another game
    int failed = 0;                     // packs that could not be copied
    int with_patches = 0;               // imported packs that ship patch files
};

// Imports the Breath of the Wild graphic packs from the Cemu community: src is a .zip of packs or a
// folder that holds them (for example Cemu's graphicPacks folder). A pack is a folder with a rules.txt
// whose titleIds list a BotW title. Packs are copied into <user_root>/mods and left switched off. A mod
// you installed yourself under the same name is never overwritten.
bool import_cemu_packs(const std::filesystem::path& user_root, const std::filesystem::path& src,
                       ImportReport& report, std::string& err);

// Exposed for tests.
void parse_rules(const std::string& text, Mod& out);
bool rules_target_botw(const std::string& rules_text);

}  // namespace mods
