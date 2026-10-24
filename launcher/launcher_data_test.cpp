// Portable checks for meta.xml, iconTex.tga and mods.ini handling. Not part of the game.
//   g++ -std=c++20 -I. launcher/launcher_data_test.cpp launcher/game_info.cpp launcher/mods.cpp -o launcher_data_test
#include "game_info.h"
#include "mods.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void expect(bool cond, const std::string& msg) {
    if (cond) return;
    std::cerr << "FAIL " << msg << "\n";
    g_fails++;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 2x2 uncompressed 32-bit TGA, bottom-up rows.
std::vector<uint8_t> tiny_tga() {
    std::vector<uint8_t> f(18, 0);
    f[2] = 2;
    f[12] = 2;
    f[14] = 2;
    f[16] = 32;
    f[17] = 8;
    // bottom row: blue, green; top row: red, white (BGRA)
    const uint8_t px[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    f.insert(f.end(), px, px + sizeof px);
    return f;
}

void test_meta() {
    gameinfo::Info info;
    std::string xml =
        "<?xml version=\"1.0\"?><menu>"
        "<title_id type=\"hexBinary\" length=\"8\">00050000101c9400</title_id>"
        "<title_version type=\"unsignedInt\" length=\"4\">208</title_version>"
        "<longname_en type=\"string\" length=\"512\">Line one&#xA;Line &amp; two</longname_en>"
        "</menu>";
    expect(gameinfo::parse_meta(xml, info), "meta parses");
    expect(info.title_id == "00050000101C9400", "title id upper-case: " + info.title_id);
    expect(info.has_title_version && info.title_version == 208, "title version 208");
    expect(info.long_name == "Line one\nLine & two", "long name entities: " + info.long_name);
    expect(gameinfo::version_name(208) == "v1.5.0", "208 is v1.5.0");
    expect(gameinfo::region_name(info.title_id) == "USA", "region USA");
    gameinfo::Info bad;
    expect(!gameinfo::parse_meta("not xml", bad), "garbage rejected");
}

void test_tga() {
    gameinfo::Icon icon;
    expect(gameinfo::decode_tga(tiny_tga(), icon), "tga decodes");
    expect(icon.width == 2 && icon.height == 2 && icon.bgra.size() == 16, "tga size");
    if (icon.bgra.size() == 16) {
        expect(icon.bgra[2] == 255 && icon.bgra[0] == 0, "top-left is red after flip");
        expect(icon.bgra[8] == 255 && icon.bgra[9] == 0, "bottom-left is blue");
    }
    gameinfo::Icon none;
    expect(!gameinfo::decode_tga(std::vector<uint8_t>(10, 0), none), "short tga rejected");
}

void test_mods(const fs::path& root) {
    fs::remove_all(root);
    write_text(root / "mods" / "Beta" / "rules.txt",
               "[Definition]\nname = Beta Mod\ndescription = Second one\nversion = 2\n");
    fs::create_directories(root / "mods" / "Beta" / "content");
    write_text(root / "mods" / "Alpha" / "rules.txt", "[Definition]\nname = \"Alpha\"\n");
    fs::create_directories(root / "mods" / "Alpha" / "aoc");
    fs::create_directories(root / "mods" / "Gamma" / "content");
    write_text(root / "mods.ini", "; keep me\n[Enabled]\nBeta=1\nGone=1\n[Other]\nx=y\n");

    auto st = mods::load(root);
    expect(st.list.size() == 3, "three mods");
    if (st.list.size() == 3) {
        expect(st.list[0].folder == "Alpha" && st.list[1].folder == "Beta", "alphabetical without [Order]");
        expect(st.list[0].name == "Alpha", "quoted name: " + st.list[0].name);
        expect(st.list[1].name == "Beta Mod" && st.list[1].version == "2", "rules name and version");
        expect(st.list[2].name == "Gamma" && !st.list[2].has_rules, "no rules uses folder name");
        expect(!st.list[0].enabled && st.list[1].enabled && !st.list[2].enabled, "missing key is off");
        expect(st.list[0].has_aoc && st.list[1].has_content, "content and aoc detected");
    }
    expect(mods::enabled_count(st) == 1, "one enabled");
    expect(!st.has_order, "no order yet");

    // Enable Gamma and move it first.
    std::vector<mods::Mod> reordered;
    for (auto& m : st.list) if (m.folder == "Gamma") { m.enabled = true; reordered.push_back(m); }
    for (auto& m : st.list) if (m.folder != "Gamma") reordered.push_back(m);
    st.list = reordered;
    std::string err;
    expect(mods::save(root, st, true, err), "save works: " + err);
    std::string ini = read_text(root / "mods.ini");
    expect(ini.find("order=Gamma,Alpha,Beta") != std::string::npos, "order written");
    expect(ini.find("Gone=1") != std::string::npos, "keys for missing folders kept");
    expect(ini.find("[Other]") != std::string::npos && ini.find("x=y") != std::string::npos, "other sections kept");

    auto again = mods::load(root);
    expect(again.has_order, "order read back");
    if (again.list.size() == 3) {
        expect(again.list[0].folder == "Gamma" && again.list[0].enabled, "Gamma first and on");
        expect(again.list[2].folder == "Beta" && again.list[2].enabled, "Beta last and on");
    }
    fs::remove_all(root);
}

}  // namespace

int main() {
    test_meta();
    test_tga();
    test_mods(fs::temp_directory_path() / "launcher_data_test");
    if (g_fails) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "launcher data tests passed\n";
    return 0;
}
