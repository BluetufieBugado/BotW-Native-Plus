// Mods in <user data>\mods\<folder>\ and their on/off state and order in <user data>\mods.ini.
//
// mods.ini:
//   [Enabled]
//   <folder> = 1        ; 1 on, 0 off, missing means off
//   [Order]
//   order = a,b,c       ; later wins. Without it the game sorts folders by name.
//
// Plain C++, no Win32.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mods {

struct Mod {
    std::string folder;       // folder name under mods\, also the mods.ini key
    std::string name;         // rules.txt [Definition] name, else the folder name
    std::string description;  // rules.txt description, may be empty
    std::string version;      // rules.txt version, may be empty
    bool has_rules = false;
    bool has_content = false;
    bool has_aoc = false;
    bool enabled = false;
};

struct State {
    std::vector<Mod> list;  // display and load order, first loads first, last wins
    bool has_order = false; // mods.ini had an [Order] line
};

std::filesystem::path mods_dir(const std::filesystem::path& user_root);
std::filesystem::path ini_path(const std::filesystem::path& user_root);

State load(const std::filesystem::path& user_root);

// Rewrites [Enabled], and [Order] when write_order is set (or it was there before).
// Other sections and keys for folders that are gone are kept.
bool save(const std::filesystem::path& user_root, const State& state, bool write_order, std::string& err);

int enabled_count(const State& state);

// Exposed for tests.
void parse_rules(const std::string& text, Mod& out);

}  // namespace mods
