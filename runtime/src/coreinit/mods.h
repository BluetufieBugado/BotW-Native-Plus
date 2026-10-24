// Content mods and DLC visibility. The game dump is never written.
//
// Mods live in <user>/mods/<Folder>/. A mod folder may hold rules.txt (Cemu graphic pack style,
// [Definition] name / description / version), content/ and aoc/. Files under content/ replace or
// add to /vol/content at the same relative path, aoc/ does the same for the DLC volume.
// <user>/mods.ini says which mods are on ([Enabled] Folder = 1) and in which order
// ([Order] order = A, B, C, later wins). See docs/mods.md.
//
// The index is built once by init() and is read-only afterwards, so lookups need no lock.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mods {

// ---- file formats (pure, no file system access) ----

struct Rules {
    bool has_definition = false;
    std::string name, description, version;
    std::vector<std::string> title_ids;  // [Definition] titleIds, lowercase hex, may be empty
};
Rules parse_rules(std::string_view text);

struct Config {
    struct Entry {
        std::string folder;
        bool enabled = false;
    };
    std::vector<Entry> enabled;      // [Enabled], in file order
    std::vector<std::string> order;  // [Order] order = ..., lowest priority first
    bool has_order = false;
    bool is_enabled(std::string_view folder) const;  // folder names compare case-insensitively
};
Config parse_config(std::string_view text);

// Enabled folders from `present` (the subfolders of <user>/mods), lowest priority first. With an
// [Order] list, listed folders keep that order and enabled folders it does not name go before
// them, alphabetically. Without one the whole list is alphabetical (case-insensitive).
std::vector<std::string> load_order(const Config& cfg, const std::vector<std::string>& present);

// ---- runtime ----

struct ModInfo {
    std::string folder, name, description, version;
    size_t content_files = 0, aoc_files = 0;
    size_t replaces = 0;  // files that replace a file of the game dump (or DLC)
    size_t lost = 0;      // files hidden by a later mod
};

struct Query {
    enum class Kind { Other, Missing, File, Dir };
    Kind kind = Kind::Other;
    std::string host;
    bool overlay = false;
    uint64_t size = 0;
};

struct DirItem {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    bool overlay = false;
};

enum class ListStatus { NotVolume, Missing, NotDir, Ok };

// enabled = false (--no-mods) skips <user>/mods entirely; DLC detection still runs.
void init(const std::string& game_root, const std::string& user_root, bool enabled = true);
void log_shutdown();

// `guest` is an absolute Cafe path (/vol/content/..., /vol/aoc...). Other means the caller should
// use its own host path: not content or aoc, or content with no mod files loaded.
Query query(const std::string& guest);
ListStatus list_dir(const std::string& guest, std::vector<DirItem>& out);
// True for /vol/content, /vol/code, /vol/meta, and /vol/aoc. Those volumes are never created or
// overwritten on the host.
bool read_only_volume(const std::string& guest);

void note_opened(const std::string& guest);

bool dlc_ready();
uint64_t aoc_title_id();
std::string aoc_mount_path();
bool owns_content(uint16_t id);

bool disabled_by_flag();                // --no-mods
const std::vector<ModInfo>& loaded();  // lowest priority first
size_t conflicts();                     // files provided by more than one mod
size_t index_files();
size_t replacing_files();
size_t added_files();
size_t opened_files();
std::string summary();  // one line for the settings menu

}  // namespace mods
