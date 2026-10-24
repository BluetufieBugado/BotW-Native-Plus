// Checks for the mod index (runtime/src/coreinit/mods.cpp): mods.ini and rules.txt parsing, load
// order, priority, case-insensitive lookup and merged directory listings. Builds a small fake
// game folder and user folder in the system temp directory; no game files are needed.
#include "coreinit/mods.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) g_pass++;                                             \
        else {                                                          \
            g_fail++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void test_config() {
    auto c = mods::parse_config(
        "\xEF\xBB\xBF; comment\n[Enabled]\nAlpha = 1\r\nbeta=0\n  Gamma =1 \n# x\n[Order]\norder = Gamma, alpha , Gamma,,beta\n");
    CHECK(c.is_enabled("Alpha"));
    CHECK(c.is_enabled("ALPHA"));
    CHECK(!c.is_enabled("Beta"));
    CHECK(c.is_enabled("gamma"));
    CHECK(!c.is_enabled("Missing"));
    CHECK(c.has_order);
    CHECK((c.order == std::vector<std::string>{"Gamma", "alpha", "beta"}));

    auto none = mods::parse_config("");
    CHECK(!none.has_order && none.enabled.empty());

    // Keys outside [Enabled] do not enable anything; a repeated key uses the last value.
    auto odd = mods::parse_config("Alpha = 1\n[Other]\nBeta = 1\n[enabled]\nGamma = 1\nGamma = 0\n");
    CHECK(!odd.is_enabled("Alpha") && !odd.is_enabled("Beta") && !odd.is_enabled("Gamma"));
}

static void test_order() {
    std::vector<std::string> present = {"zeta", "Alpha", "mid", "Off"};
    auto cfg = mods::parse_config("[Enabled]\nzeta=1\nalpha=1\nMID=1\noff=0\n");
    CHECK((mods::load_order(cfg, present) == std::vector<std::string>{"Alpha", "mid", "zeta"}));
    // Listed ones in the given order; enabled but unlisted ones first, alphabetically.
    cfg = mods::parse_config("[Enabled]\nzeta=1\nalpha=1\nMID=1\noff=0\n[Order]\norder=zeta,off,Alpha,NotThere\n");
    CHECK((mods::load_order(cfg, present) == std::vector<std::string>{"mid", "zeta", "Alpha"}));
}

static void test_rules() {
    auto r = mods::parse_rules(
        "[Definition]\ntitleIds = 00050000101C9300,00050000101C9400\nname = \"Better Things\"\npath = \"The Legend of Zelda: Breath "
        "of the Wild/Mods/Better\"\ndescription = Line one\\nline two\nversion = 7\n\n[Preset]\nname = Other\n");
    CHECK(r.has_definition);
    CHECK(r.name == "Better Things");
    CHECK(r.description == "Line one line two");
    CHECK(r.version == "7");
    CHECK(r.title_ids.size() == 2 && r.title_ids[1] == "00050000101c9400");
    auto empty = mods::parse_rules("name = x\n");
    CHECK(!empty.has_definition && empty.name.empty());
}

static void test_index(const fs::path& root) {
    fs::path game = root / "game", user = root / "user";
    put(game / "content" / "Actor" / "Pack" / "Link.sbactorpack", "base-link");
    put(game / "content" / "Actor" / "Pack" / "Horse.sbactorpack", "base-horse");
    put(game / "content" / "System" / "Version.txt", "base-version");
    put(game / "aoc" / "0010" / "Pack" / "AocMainField.pack", "dlc-pack");
    put(game / "aoc" / "0010" / "Map" / "Field.txt", "dlc-field");

    // ModA: lower case paths, replaces Link and adds a file.
    put(user / "mods" / "ModA" / "rules.txt", "[Definition]\nname = Mod A\ndescription = first\nversion = 2\n");
    put(user / "mods" / "ModA" / "content" / "actor" / "pack" / "link.sbactorpack", "A-link");
    put(user / "mods" / "ModA" / "content" / "Actor" / "Pack" / "NewThing.sbactorpack", "A-new");
    put(user / "mods" / "ModA" / "patch_fps.txt", "[x]\n");
    // ModB: also replaces Link, plus the DLC folder.
    put(user / "mods" / "ModB" / "Content" / "Actor" / "Pack" / "Link.sbactorpack", "B-link");
    put(user / "mods" / "ModB" / "aoc" / "0010" / "Map" / "Field.txt", "B-field");
    // ModC: present but disabled.
    put(user / "mods" / "ModC" / "content" / "System" / "Version.txt", "C-version");

    // Alphabetical: ModB loads after ModA and wins.
    put(user / "mods.ini", "[Enabled]\nModA = 1\nModB = 1\nModC = 0\n");
    mods::init(game.string(), user.string());
    CHECK(mods::loaded().size() == 2);
    CHECK(mods::loaded().size() == 2 && mods::loaded()[0].name == "Mod A" && mods::loaded()[0].version == "2");
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].name == "ModB");  // no rules.txt: folder name
    CHECK(mods::loaded().size() == 2 && mods::loaded()[0].content_files == 2 && mods::loaded()[0].replaces == 1);
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].aoc_files == 1 && mods::loaded()[1].replaces == 2);
    CHECK(mods::conflicts() == 1);

    auto q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && q.overlay && slurp(q.host) == "B-link");
    q = mods::query("/vol/content/ACTOR/pack/LINK.SBACTORPACK");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "B-link");
    q = mods::query("/vol/content//Actor/./Pack/../Pack/newthing.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "A-new");
    q = mods::query("/vol/content/Actor/Pack/Horse.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && !q.overlay && slurp(q.host) == "base-horse");
    q = mods::query("/vol/content/actor/pack/horse.sbactorpack");  // base lookups ignore case too
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "base-horse");
    q = mods::query("/vol/content/System/Version.txt");  // ModC is off
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "base-version");
    q = mods::query("/vol/content/Actor/Pack/Nope.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::Missing);
    q = mods::query("/vol/content/Actor");
    CHECK(q.kind == mods::Query::Kind::Dir);
    q = mods::query("/vol/save/80000001/game_data.sav");
    CHECK(q.kind == mods::Query::Kind::Other);

    // DLC through the title-suffixed mount the game uses.
    CHECK(mods::dlc_ready());
    CHECK(mods::owns_content(0x0010));
    std::string aoc = mods::aoc_mount_path();
    q = mods::query(aoc + "/0010/Map/Field.txt");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "B-field");
    q = mods::query(aoc + "/0010/Pack/AocMainField.pack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "dlc-pack");

    std::vector<mods::DirItem> items;
    CHECK(mods::list_dir("/vol/content/actor/Pack", items) == mods::ListStatus::Ok);
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(items[0].name == "Horse.sbactorpack" && !items[0].overlay);
        CHECK(items[1].name == "Link.sbactorpack" && items[1].overlay && items[1].size == 6);
        CHECK(items[2].name == "NewThing.sbactorpack" && items[2].overlay);
    }
    CHECK(mods::list_dir("/vol/content/Actor/Pack/Link.sbactorpack", items) == mods::ListStatus::NotDir);
    CHECK(mods::list_dir("/vol/content/NoSuchDir", items) == mods::ListStatus::Missing);
    CHECK(mods::list_dir("/vol/save", items) == mods::ListStatus::NotVolume);
    CHECK(mods::read_only_volume("/vol/content/x") && mods::read_only_volume("/vol/code/x") && !mods::read_only_volume("/vol/save/x"));
    CHECK(mods::summary() == "2: ModB, Mod A");

    // [Order] reverses priority: ModA now wins.
    put(user / "mods.ini", "[Enabled]\nmoda = 1\nmodb = 1\n[Order]\norder = ModB, ModA\n");
    mods::init(game.string(), user.string());
    q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "A-link");
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].folder == "ModA" && mods::loaded()[0].lost == 1);

    // --no-mods: content goes straight to the host path again.
    mods::init(game.string(), user.string(), false);
    CHECK(mods::disabled_by_flag() && mods::loaded().empty());
    q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::Other);
    CHECK(mods::list_dir("/vol/content/Actor", items) == mods::ListStatus::NotVolume);
    CHECK(mods::summary() == "Off for this run (--no-mods)");

    // No mods.ini: nothing is enabled.
    fs::remove(user / "mods.ini");
    mods::init(game.string(), user.string());
    CHECK(mods::loaded().empty() && mods::index_files() == 0);
    CHECK(mods::query("/vol/content/System/Version.txt").kind == mods::Query::Kind::Other);
    CHECK(mods::summary() == "None loaded");
}

int main() {
    fs::path root = fs::temp_directory_path() / "botw_mods_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    test_config();
    test_order();
    test_rules();
    test_index(root);
    fs::remove_all(root, ec);
    printf("mods_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
