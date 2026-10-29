// Portable checks for meta.xml, iconTex.tga and mods.ini handling. Not part of the game.
#include "game_info.h"
#include "mods.h"
#include "setup_wizard.h"

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
               "[Definition]\nname = Beta Mod\ndescription = Second one\nversion = 2\n"
               "[Preset]\nname = Default Look\ncategory = Style\ndefault = 1\n"
               "[Preset]\nname = Dark Look\ncategory = Style\n");
    write_text(root / "mods" / "Beta" / "content" / "Actor" / "Pack" / "Link.sbactorpack", "beta-link");
    write_text(root / "mods" / "Alpha" / "rules.txt", "[Definition]\nname = \"Alpha\"\n");
    fs::create_directories(root / "mods" / "Alpha" / "aoc");
    // Direct Actor/ layout without content/ wrapper:
    write_text(root / "mods" / "Gamma" / "Actor" / "Pack" / "Link.sbactorpack", "gamma-link");
    write_text(root / "mods.ini", "; keep me\n[Enabled]\nBeta=1\nGone=1\n[Other]\nx=y\n");

    auto st = mods::load(root);
    expect(st.list.size() == 3, "three mods");
    if (st.list.size() == 3) {
        expect(st.list[0].folder == "Alpha" && st.list[1].folder == "Beta", "alphabetical without [Order]");
        expect(st.list[0].name == "Alpha", "quoted name: " + st.list[0].name);
        expect(st.list[1].name == "Beta Mod" && st.list[1].version == "2", "rules name and version");
        expect(st.list[1].presets.size() == 1 && st.list[1].presets[0].active == "Default Look", "default preset parsed");
        expect(st.list[2].name == "Gamma" && !st.list[2].has_rules, "no rules uses folder name");
        expect(!st.list[0].enabled && st.list[1].enabled && !st.list[2].enabled, "missing key is off");
        expect(st.list[0].has_aoc && st.list[1].has_content && st.list[2].has_content, "content and aoc detected");
    }
    expect(mods::enabled_count(st) == 1, "one enabled");
    expect(!st.has_order, "no order yet");

    // Enable Gamma, change Beta's preset, and move Gamma first.
    std::vector<mods::Mod> reordered;
    for (auto& m : st.list) {
        if (m.folder == "Gamma") {
            m.enabled = true;
            reordered.push_back(m);
        }
    }
    for (auto& m : st.list) {
        if (m.folder != "Gamma") {
            if (m.folder == "Beta" && !m.presets.empty()) {
                m.presets[0].active = "Dark Look";
            }
            reordered.push_back(m);
        }
    }
    st.list = reordered;
    std::string err;
    expect(mods::save(root, st, true, err), "save works: " + err);
    std::string ini = read_text(root / "mods.ini");
    expect(ini.find("order=Gamma,Alpha,Beta") != std::string::npos, "order written");
    expect(ini.find("Gone=1") != std::string::npos, "keys for missing folders kept");
    expect(ini.find("[Other]") != std::string::npos && ini.find("x=y") != std::string::npos, "other sections kept");
    expect(ini.find("Beta:Style=Dark Look") != std::string::npos, "preset persisted");

    auto again = mods::load(root);
    expect(again.has_order, "order read back");
    if (again.list.size() == 3) {
        expect(again.list[0].folder == "Gamma" && again.list[0].enabled, "Gamma first and on");
        expect(again.list[2].folder == "Beta" && again.list[2].enabled, "Beta last and on");
        expect(again.list[2].presets.size() == 1 && again.list[2].presets[0].active == "Dark Look", "preset read back");
        // Beta is after Gamma and both touch Actor/Pack/Link.sbactorpack -> Beta overrides Gamma
        expect(again.list[2].overrides_mods.size() == 1 && again.list[2].overrides_mods[0] == "Gamma", "Beta overrides Gamma");
        expect(again.list[0].overridden_by_mods.size() == 1 && again.list[0].overridden_by_mods[0] == "Beta Mod", "Gamma overridden by Beta Mod");
    }

    // Test installing a folder via mods::install_mod
    fs::path ext_mod = root / "external_source" / "CoolPack";
    write_text(ext_mod / "rules.txt", "[Definition]\nname = Cool Pack\nversion = 5\n");
    write_text(ext_mod / "Actor" / "Pack" / "Horse.sbactorpack", "horse-mod");
    std::string installed_folder;
    expect(mods::install_mod(root, ext_mod, installed_folder, err), "install_mod folder works: " + err);
    expect(installed_folder == "CoolPack", "installed folder name");
    auto after_install = mods::load(root);
    expect(after_install.list.size() == 4, "four mods after install");

    fs::remove_all(root);
}


void test_sha1_and_dumps(const fs::path& root) {
    expect(gameinfo::sha1_bytes("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 of abc");

    fs::remove_all(root);
    // Simulate a Dumpling / wiiuu parent folder with separate Base (v0), Update (v208), and DLC (v80)
    fs::path base = root / "wiiuu" / "BotW_Base";
    fs::path upd = root / "wiiuu" / "BotW_Update_v208";
    fs::path dlc = root / "wiiuu" / "BotW_DLC_v80";

    write_text(base / "meta" / "meta.xml",
               "<menu><title_id>00050000101C9400</title_id><title_version>0</title_version>"
               "<longname_en>The Legend of Zelda&#xA;Breath of the Wild</longname_en></menu>");
    write_text(base / "code" / "U-King.rpx", "base_v0_rpx");
    write_text(base / "content" / "Pack" / "Bootup.pack", "base_bootup");
    write_text(base / "content" / "Actor" / "Link.sbactorpack", "base_link");

    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9400</title_id><title_version>208</title_version>"
               "<longname_en>The Legend of Zelda&#xA;Breath of the Wild</longname_en></menu>");
    write_text(upd / "code" / "U-King.rpx", "update_v208_rpx");
    write_text(upd / "content" / "Pack" / "Bootup.pack", "updated_bootup_v208");

    write_text(dlc / "meta" / "meta.xml",
               "<menu><title_id>0005000C101C9400</title_id><title_version>80</title_version></menu>");
    write_text(dlc / "content" / "0010" / "Pack" / "AocMainField.pack", "dlc_aoc_mainfield");

    auto layout = gameinfo::discover_dumps(root / "wiiuu");
    expect(layout.needs_merge, "separate dumps need merge");
    expect(layout.ready(), "separate base + update ready to merge");
    expect(layout.base_folder == base, "found base folder");
    expect(layout.update_folder == upd, "found update folder");
    expect(layout.dlc_folder == dlc, "found dlc folder");

    fs::path merged = root / "merged_game";
    std::string err;
    expect(gameinfo::merge_dumps(layout.base_folder, layout.update_folder, layout.dlc_folder, merged, err),
           "merge_dumps succeeds: " + err);

    // Base files must remain untouched while merged has Update overlay + Base files + DLC!
    expect(read_text(base / "code" / "U-King.rpx") == "base_v0_rpx", "base rpx untouched");
    expect(read_text(merged / "code" / "U-King.rpx") == "update_v208_rpx", "merged rpx from update");
    expect(read_text(merged / "content" / "Pack" / "Bootup.pack") == "updated_bootup_v208", "merged bootup from update");
    expect(read_text(merged / "content" / "Actor" / "Link.sbactorpack") == "base_link", "merged actor from base");
    expect(read_text(merged / "aoc" / "0010" / "Pack" / "AocMainField.pack") == "dlc_aoc_mainfield", "merged dlc aoc");

    auto minfo = gameinfo::read(merged);
    expect(minfo.has_rpx && minfo.has_content && minfo.has_meta && minfo.has_dlc, "merged game info complete");
    expect(minfo.title_version == 208, "merged version is 208");

    fs::remove_all(root);
}

void test_setup_wizard(const fs::path& root) {
    setupwizard::BuildProgress prog;
    expect(setupwizard::parse_progress_line("[recomp 1/4] Decompressing & parsing U-King.rpx...", prog), "parse recomp 1/4");
    expect(prog.stage == setupwizard::BuildStage::RecompileWiiUToCpp, "stage is RecompileWiiUToCpp");
    expect(prog.recompile_step == 1 && prog.recompile_total == 4, "step 1/4");

    expect(setupwizard::parse_progress_line("[recomp 3/4] Generating C++ shard 42/258 (16%) [fn_020c0000.cpp]...", prog), "parse recomp 3/4");
    expect(prog.sub_done == 42 && prog.sub_total == 258, "shard 42/258");
    expect(prog.current_item == "fn_020c0000.cpp", "current item fn_020c0000.cpp");
    expect(prog.phase_text.find("shard 42 / 258, 16%") != std::string::npos, "phase text has shard 42 / 258, 16%");

    expect(setupwizard::parse_progress_line("[recomp 4/4] Writing dispatch table & HLE stubs (100%)", prog), "parse recomp 4/4");
    expect(prog.stage_percent == 100, "recomp 4/4 stage 100%");

    expect(setupwizard::parse_progress_line("[412/1267] Building CXX object CMakeFiles/BotWRecomp.dir/generated/fn_02340000.cpp.obj", prog), "parse ninja [412/1267]");
    expect(prog.stage == setupwizard::BuildStage::CompileCppToExe, "stage is CompileCppToExe");
    expect(prog.sub_done == 412 && prog.sub_total == 1267, "412/1267");
    expect(prog.stage_percent == 32, "stage pct 32%");

    std::error_code ec;
    fs::remove_all(root, ec);
    write_text(root / "settings.ini", "# comment\nvolume=80\nscale=1\n");
    setupwizard::GamePrefs prefs;
    prefs.scale = 0.5f;
    prefs.fps = 60;
    prefs.layout = 1;
    prefs.gamepad_view = true;
    prefs.gyro = false;
    expect(setupwizard::write_game_prefs(root, prefs), "write_game_prefs");
    auto r = setupwizard::read_game_prefs(root);
    expect(r.scale == 0.5f && r.fps == 60 && r.layout == 1 && r.gamepad_view && !r.gyro, "read_game_prefs round-trip");
    expect(read_text(root / "settings.ini").find("volume=80") != std::string::npos, "preserved volume=80");
    expect(r.filter == 1 && r.display == 0, "filter/display keep the runtime defaults");
    prefs.filter = 3;
    prefs.display = 2;
    expect(setupwizard::write_game_prefs(root, prefs), "write_game_prefs filter/display");
    r = setupwizard::read_game_prefs(root);
    expect(r.filter == 3 && r.display == 2, "filter/display round-trip");
    expect(!r.show_fps, "show_fps defaults to off");
    prefs.show_fps = true;
    expect(setupwizard::write_game_prefs(root, prefs), "write_game_prefs show_fps");
    r = setupwizard::read_game_prefs(root);
    expect(r.show_fps, "show_fps round-trip");
    write_text(root / "settings.ini", "scale=0\nfilter=9\ndisplay=-4\n");
    r = setupwizard::read_game_prefs(root);
    expect(r.scale == 0.0f, "scale=0 (automatic) stays automatic");
    expect(setupwizard::write_game_prefs(root, r), "write_game_prefs auto scale");
    expect(read_text(root / "settings.ini").find("scale=0\n") != std::string::npos, "scale=0 is written back unchanged");
    expect(r.filter == 3 && r.display == 0, "filter/display are clamped");
    fs::remove_all(root, ec);
}

std::vector<uint8_t> make_stored_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    auto u16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) {
        u16(v, static_cast<uint16_t>(x & 0xffff));
        u16(v, static_cast<uint16_t>(x >> 16));
    };
    struct Entry { std::string name, data; uint32_t offset; };
    std::vector<Entry> entries;
    std::vector<uint8_t> out;
    for (const auto& [name, data] : files) {
        uint32_t off = static_cast<uint32_t>(out.size());
        u32(out, 0x04034b50u); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, 0); u32(out, static_cast<uint32_t>(data.size())); u32(out, static_cast<uint32_t>(data.size()));
        u16(out, static_cast<uint16_t>(name.size())); u16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        entries.push_back({name, data, off});
    }
    uint32_t cd_off = static_cast<uint32_t>(out.size());
    for (const auto& e : entries) {
        u32(out, 0x02014b50u); u16(out, 20); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, 0); u32(out, static_cast<uint32_t>(e.data.size())); u32(out, static_cast<uint32_t>(e.data.size()));
        u16(out, static_cast<uint16_t>(e.name.size())); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, 0); u32(out, e.offset);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_off;
    u32(out, 0x06054b50u); u16(out, 0); u16(out, 0);
    u16(out, static_cast<uint16_t>(entries.size())); u16(out, static_cast<uint16_t>(entries.size()));
    u32(out, cd_size); u32(out, cd_off); u16(out, 0);
    return out;
}

void test_cemu_import(const fs::path& root) {
    fs::remove_all(root);
    const std::string botw =
        "[Definition]\ntitleIds = 00050000101C9300,00050000101C9400,00050000101C9500\nname = Pack\nversion = 7\n";
    expect(mods::rules_target_botw(botw), "titleIds with the BotW ids match");
    expect(mods::rules_target_botw("\xEF\xBB\xBF[Definition]\r\nTitleIds = 0005000e101c9400\r\n"), "update id, BOM, CRLF and case");
    expect(!mods::rules_target_botw("[Definition]\ntitleIds = 0005000010176900\n"), "another game does not match");
    expect(!mods::rules_target_botw("[Definition]\nname = no title ids\n"), "no titleIds does not match");

    // A folder of packs, as in Cemu's graphicPacks folder.
    fs::path bundle = root / "bundle";
    write_text(bundle / "Splatoon" / "Resolution" / "rules.txt", "[Definition]\ntitleIds = 0005000010176900\nname = Splat\n");
    write_text(bundle / "BreathOfTheWild" / "Graphics" / "rules.txt", botw);
    write_text(bundle / "BreathOfTheWild" / "Graphics" / "7cd338ce4c6ea935_0000000000000079_ps.txt", "void main() {}\n");
    write_text(bundle / "BreathOfTheWild" / "Mods" / "FPS++" / "rules.txt", botw);
    write_text(bundle / "BreathOfTheWild" / "Mods" / "FPS++" / "patch_GameSpeed.asm", "[X]\nmoduleMatches = 0x6267BFD0\n");
    write_text(bundle / "BreathOfTheWild" / "A" / "Fix" / "rules.txt", botw);
    write_text(bundle / "BreathOfTheWild" / "B" / "Fix" / "rules.txt", botw);

    fs::path user = root / "user";
    write_text(user / "mods" / "Graphics" / "rules.txt", "[Definition]\nname = Mine\n");  // the user's own mod
    mods::ImportReport rep;
    std::string err;
    expect(mods::import_cemu_packs(user, bundle, rep, err), "import from a folder: " + err);
    expect(rep.imported.size() == 4, "four BotW packs imported");
    expect(rep.skipped_other_games == 1, "the Splatoon pack is skipped");
    expect(rep.with_patches == 1, "one imported pack has patch files");
    expect(read_text(user / "mods" / "Graphics" / "rules.txt").find("Mine") != std::string::npos, "the user's own mod is not overwritten");
    expect(fs::exists(user / "mods" / "BreathOfTheWild_Graphics" / "7cd338ce4c6ea935_0000000000000079_ps.txt"), "the pack's shader file is copied");
    expect(fs::exists(user / "mods" / "FPS++" / "patch_GameSpeed.asm"), "patch_*.asm is copied");
    expect(fs::exists(user / "mods" / "Fix") && fs::exists(user / "mods" / "B_Fix"), "same names get a distinct folder");
    mods::State st = mods::load(user);
    expect(mods::enabled_count(st) == 0, "imported packs are switched off");
    expect(mods::import_cemu_packs(user, bundle, rep, err) && rep.imported.size() == 4, "importing again refreshes the packs");
    expect(!fs::exists(user / "mods" / "BreathOfTheWild_Graphics_2"), "importing again does not pile up copies");
    expect(!mods::import_cemu_packs(user, user / "mods", rep, err), "the mods folder itself is refused");

    // A zip that also holds another game's packs: only the BotW part is unpacked.
    fs::path zip = root / "packs.zip";
    auto bytes = make_stored_zip({
        {"graphicPacks/BreathOfTheWild/Graphics/rules.txt", botw},
        {"graphicPacks/BreathOfTheWild/Graphics/x_ps.txt", "void main() {}\n"},
        {"graphicPacks/BreathOfTheWild/Mods/RemoveFog/rules.txt", botw},
        {"graphicPacks/Splatoon/Resolution/rules.txt", "[Definition]\ntitleIds = 0005000010176900\n"},
    });
    {
        std::ofstream out(zip, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    fs::path user2 = root / "user2";
    expect(mods::import_cemu_packs(user2, zip, rep, err), "import from a zip: " + err);
    expect(rep.imported.size() == 2 && rep.skipped_other_games == 0, "zip: BotW packs only, other games never unpacked");
    expect(fs::exists(user2 / "mods" / "Graphics" / "x_ps.txt") && fs::exists(user2 / "mods" / "RemoveFog" / "rules.txt"), "zip: files copied");
    expect(!fs::exists(user2 / "mods" / ".cemu_import_tmp"), "zip: temporary folder removed");

    // Nothing for BotW in there.
    fs::path other = root / "other";
    write_text(other / "Splatoon" / "Resolution" / "rules.txt", "[Definition]\ntitleIds = 0005000010176900\n");
    expect(!mods::import_cemu_packs(root / "user3", other, rep, err) && !err.empty(), "a folder without BotW packs is reported");
}


// What a Cemu-style graphic pack offers in its settings: categories, conditions between them, defaults.
const char* kVisualPack =
    "[Definition]\n"
    "titleIds = 00050000101c9400,00050000101c9500,00050000101c9300\n"
    "name = \"Visual Tweaks\"\n"
    "path = \"The Legend of Zelda: Breath of the Wild/Graphics/Visual Tweaks\"\n"
    "description = First line.\\nSecond line # still text\n"
    "version = 7\n"
    "\n"
    "[Default]\n"
    "$drawDistance = 1.0\n"
    "$texQuality = 2\n"
    "\n"
    "[Preset]\nname = Default (1x)\ncategory = Draw Distance\ndefault = 1\n$drawDistance = 1.0\n"
    "[Preset]\nname = Far (2x)\ncategory = Draw Distance\n$drawDistance = 2.0\n"
    "[Preset]\nname = Very far (4x)\ncategory = Draw Distance\n$drawDistance = 4.0   # inline comment\n"
    "\n"
    "[Preset]\nname = Normal\ncategory = Texture Quality\ndefault = 1\n$texQuality = 2\n"
    "[Preset]\nname = High\ncategory = Texture Quality\n$texQuality = 3\n"
    "[Preset]\nname = Extreme\ncategory = Texture Quality\ncondition = $drawDistance < 4\n$texQuality = 4\n"
    "\n"
    "[Preset]\nname = Cull hints on\ncategory = Distant objects\ncondition = $drawDistance >= 2\ndefault = 1\n$cull = 1\n"
    "[Preset]\nname = Cull hints off\ncategory = Distant objects\ncondition = $drawDistance >= 2\n$cull = 0\n";

const mods::PresetChoice* find_choice(const std::vector<mods::PresetChoice>& v, const std::string& label) {
    for (const auto& c : v)
        if (c.label == label) return &c;
    return nullptr;
}

void pick(mods::Mod& m, const std::string& category, const std::string& option) {
    for (auto& pc : m.presets)
        if (pc.category == category) pc.active = option;
}

void test_presets(const fs::path& root) {
    fs::remove_all(root);
    write_text(root / "mods" / "Visual Tweaks" / "rules.txt", kVisualPack);
    write_text(root / "mods" / "Visual Tweaks" / "content" / "Pack" / "x.pack", "x");
    write_text(root / "mods" / "Single" / "rules.txt",
               "[Definition]\nname = Single\n"
               "[Preset]\nname = A\ndefault = 1\n[Preset]\nname = B\ndefault = 1\n[Preset]\nname = C\n");
    write_text(root / "mods" / "Plain" / "rules.txt", "[Definition]\nname = Plain\n");
    write_text(root / "mods" / "Broken" / "rules.txt",
               "[Definition]\nname = Broken\n"
               "[Preset]\nname = Always\ncategory = Q\n"
               "[Preset]\nname = Never\ncategory = Q\ncondition = $$$ nonsense\n");

    auto st = mods::load(root);
    mods::Mod* vt = nullptr;
    mods::Mod* single = nullptr;
    mods::Mod* plain = nullptr;
    mods::Mod* broken = nullptr;
    for (auto& m : st.list) {
        if (m.folder == "Visual Tweaks") vt = &m;
        else if (m.folder == "Single") single = &m;
        else if (m.folder == "Plain") plain = &m;
        else if (m.folder == "Broken") broken = &m;
    }
    expect(vt && single && plain && broken, "presets: all four test mods found");
    if (!(vt && single && plain && broken)) return;

    // Parsing.
    expect(vt->name == "Visual Tweaks" && vt->version == "7", "presets: name and version");
    expect(vt->path == "The Legend of Zelda: Breath of the Wild/Graphics/Visual Tweaks", "presets: path: " + vt->path);
    expect(vt->description == "First line.\nSecond line # still text", "presets: description keeps its line break: " + vt->description);
    expect(vt->preset_defs.size() == 8 && vt->presets.size() == 3, "presets: 8 presets in 3 categories");
    expect(vt->default_vars.size() == 2, "presets: [Default] variables kept");
    expect(vt->presets[0].category == "Draw Distance" && vt->presets[0].options.size() == 3, "presets: category order and options");
    expect(vt->presets[0].default_option == "Default (1x)" && vt->presets[1].default_option == "Normal", "presets: defaults");
    expect(vt->preset_defs[2].vars.size() == 1 && vt->preset_defs[2].vars[0].second == "4.0", "presets: inline comment stripped from a value");

    // Default view: Distant objects needs drawDistance >= 2, so it is hidden.
    auto v = mods::visible_presets(*vt);
    expect(v.size() == 2, "presets: two drop-downs at the defaults");
    const auto* tq = find_choice(v, "Texture Quality");
    expect(tq && tq->options.size() == 3 && tq->active == "Normal", "presets: all texture options offered at 1x");
    expect(!find_choice(v, "Distant objects"), "presets: dependent category hidden at 1x");

    // Picking a farther draw distance reveals the dependent category and uses its default.
    pick(*vt, "Draw Distance", "Far (2x)");
    v = mods::visible_presets(*vt);
    const auto* far_cat = find_choice(v, "Distant objects");
    expect(v.size() == 3 && far_cat && far_cat->options.size() == 2 && far_cat->active == "Cull hints on", "presets: dependent category appears at 2x");
    tq = find_choice(v, "Texture Quality");
    expect(tq && tq->options.size() == 3, "presets: Extreme still offered at 2x");

    // At 4x, Extreme is no longer offered and a pick of it falls back to the default.
    pick(*vt, "Texture Quality", "Extreme");
    v = mods::visible_presets(*vt);
    tq = find_choice(v, "Texture Quality");
    expect(tq && tq->active == "Extreme", "presets: Extreme in effect at 2x");
    pick(*vt, "Draw Distance", "Very far (4x)");
    v = mods::visible_presets(*vt);
    tq = find_choice(v, "Texture Quality");
    expect(tq && tq->options.size() == 2 && tq->active == "Normal", "presets: Extreme dropped and pick falls back at 4x");
    expect(vt->presets[1].active == "Extreme", "presets: the stored pick is not overwritten by the fallback");
    pick(*vt, "Draw Distance", "Default (1x)");
    tq = find_choice(mods::visible_presets(*vt), "Texture Quality");
    expect(tq && tq->active == "Extreme", "presets: the pick comes back when it is offered again");

    // Reset.
    pick(*vt, "Draw Distance", "Far (2x)");
    mods::reset_presets(*vt);
    expect(vt->presets[0].active == "Default (1x)" && vt->presets[1].active == "Normal", "presets: reset to defaults");

    // First default = 1 wins, and a pack without categories is labelled Preset.
    expect(single->presets.size() == 1 && single->presets[0].default_option == "A", "presets: first default wins");
    v = mods::visible_presets(*single);
    expect(v.size() == 1 && v[0].label == "Preset" && v[0].options.size() == 3 && v[0].active == "A", "presets: uncategorised pack");

    // No presets at all, and a condition that does not parse.
    expect(mods::visible_presets(*plain).empty(), "presets: a pack without presets has no settings");
    v = mods::visible_presets(*broken);
    expect(v.size() == 1 && v[0].options.size() == 1 && v[0].options[0] == "Always", "presets: an unreadable condition hides that option");

    // Save, read back, and the ini value is matched without regard to case.
    vt->enabled = true;
    pick(*vt, "Draw Distance", "Far (2x)");
    pick(*vt, "Texture Quality", "High");
    pick(*single, "", "C");
    std::string err;
    expect(mods::save(root, st, false, err), "presets: save works: " + err);
    std::string ini = read_text(root / "mods.ini");
    expect(ini.find("Visual Tweaks:Draw Distance=Far (2x)") != std::string::npos, "presets: category key written");
    expect(ini.find("Single=C") != std::string::npos, "presets: uncategorised key written");
    auto again = mods::load(root);
    for (auto& m : again.list) {
        if (m.folder != "Visual Tweaks") continue;
        expect(m.enabled && m.presets[0].active == "Far (2x)" && m.presets[1].active == "High", "presets: picks read back");
        auto vv = mods::visible_presets(m);
        expect(vv.size() == 3 && find_choice(vv, "Distant objects"), "presets: dependent category present after reload");
    }
    write_text(root / "mods.ini", "[Presets]\nVISUAL TWEAKS:draw distance = very FAR (4x)\n");
    again = mods::load(root);
    for (auto& m : again.list)
        if (m.folder == "Visual Tweaks") expect(m.presets[0].active == "Very far (4x)", "presets: ini value matched without case: " + m.presets[0].active);

    // mods.ini stays short: picks that are just the default are not written, and no empty [Presets] appears.
    fs::remove(root / "mods.ini");
    auto fresh = mods::load(root);
    expect(mods::save(root, fresh, false, err), "presets: save with defaults: " + err);
    ini = read_text(root / "mods.ini");
    expect(ini.find("[Presets]") == std::string::npos && ini.find("Draw Distance") == std::string::npos, "presets: defaults are not written");
    // A pick put back to the default replaces the line that was there, instead of leaving the old pick.
    for (auto& m : fresh.list)
        if (m.folder == "Visual Tweaks") pick(m, "Draw Distance", "Far (2x)");
    expect(mods::save(root, fresh, false, err) && read_text(root / "mods.ini").find("Visual Tweaks:Draw Distance=Far (2x)") != std::string::npos,
           "presets: non-default pick written");
    for (auto& m : fresh.list)
        if (m.folder == "Visual Tweaks") pick(m, "Draw Distance", "Default (1x)");
    expect(mods::save(root, fresh, false, err), "presets: save back to default");
    ini = read_text(root / "mods.ini");
    expect(ini.find("Draw Distance=Default (1x)") != std::string::npos && ini.find("Far (2x)") == std::string::npos,
           "presets: a pick put back to the default replaces the old line");

    fs::remove_all(root);
}

}  // namespace

int main() {
    test_meta();
    test_tga();
    test_mods(fs::temp_directory_path() / "launcher_data_test");
    test_sha1_and_dumps(fs::temp_directory_path() / "launcher_dump_test");
    test_setup_wizard(fs::temp_directory_path() / "launcher_setup_test");
    test_cemu_import(fs::temp_directory_path() / "launcher_cemu_import_test");
    test_presets(fs::temp_directory_path() / "launcher_presets_test");
    if (g_fails) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "launcher data tests passed\n";
    return 0;
}
