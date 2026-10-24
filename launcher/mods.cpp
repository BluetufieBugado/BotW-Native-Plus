#include "mods.h"
#include "unzip.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace mods {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

std::string path_to_utf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path utf8_to_path(std::string_view s) {
    std::u8string u8(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return std::filesystem::path(u8);
}

bool read_text(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

struct Ini {
    struct Section {
        std::string name;  // as written
        std::vector<std::pair<std::string, std::string>> keys;
    };
    std::vector<Section> sections;

    Section* find(const std::string& name) {
        for (auto& s : sections)
            if (lower(s.name) == lower(name)) return &s;
        return nullptr;
    }
};

Ini parse_ini(const std::string& text) {
    Ini ini;
    std::istringstream in(text);
    std::string line;
    Ini::Section* cur = nullptr;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            auto close = line.find(']');
            std::string name = trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            cur = ini.find(name);
            if (!cur) {
                ini.sections.push_back({name, {}});
                cur = &ini.sections.back();
            }
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (!cur) {
            ini.sections.push_back({"", {}});
            cur = &ini.sections.back();
        }
        cur->keys.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return ini;
}

std::vector<std::string> split_order(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v + ",") {
        if (c == ',') {
            cur = trim(cur);
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

bool is_botw_content_subdir(std::string_view name) {
    static constexpr std::string_view kKnown[] = {
        "actor", "pack", "model", "ui", "map", "sound", "system", "message",
        "effect", "physics", "event", "voice", "terrain", "quest", "font", "movie",
        "shader", " NAV", "nav", " camera", "camera", "demo"
    };
    std::string l = lower(std::string(name));
    for (auto k : kKnown) {
        if (l == trim(std::string(k))) return true;
    }
    return false;
}

bool is_patch_filename(std::string_view fname) {
    std::string l = lower(std::string(fname));
    if (l == "patches.txt") return true;
    if (l.size() > 10 && l.rfind("patch_", 0) == 0 && l.substr(l.size() - 4) == ".txt") return true;
    return false;
}

bool is_shader_filename(std::string_view fname) {
    std::string l = lower(std::string(fname));
    if (l.size() > 5 && l.substr(l.size() - 5) == ".glsl") return true;
    if (l.size() > 7) {
        std::string tail = l.substr(l.size() - 7);
        if (tail == "_ps.txt" || tail == "_vs.txt" || tail == "_gs.txt") return true;
    }
    return false;
}

bool looks_like_mod_root(const std::filesystem::path& dir) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / "rules.txt", ec)) return true;
    if (std::filesystem::is_directory(dir / "content", ec)) return true;
    if (std::filesystem::is_directory(dir / "aoc", ec)) return true;
    if (std::filesystem::is_directory(dir / "shaders", ec)) return true;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        std::string name = path_to_utf8(it->path().filename());
        if (it->is_directory(e2) && (is_botw_content_subdir(name) || lower(name) == "0010")) return true;
        if (it->is_regular_file(e2) && (is_patch_filename(name) || is_shader_filename(name))) return true;
    }
    return false;
}

std::filesystem::path resolve_mod_root(const std::filesystem::path& top_dir) {
    std::filesystem::path cur = top_dir;
    for (int depth = 0; depth < 2; ++depth) {
        if (looks_like_mod_root(cur)) return cur;
        std::error_code ec;
        std::filesystem::path single_subdir;
        int subdir_count = 0;
        int file_count = 0;
        for (auto it = std::filesystem::directory_iterator(cur, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            std::string name = path_to_utf8(it->path().filename());
            if (!name.empty() && name[0] == '.') continue;
            if (it->is_directory(e2)) {
                subdir_count++;
                single_subdir = it->path();
            } else if (it->is_regular_file(e2)) {
                std::string l = lower(name);
                if (l != "readme.txt" && l != "readme.md" && l != "info.json" && l != "manifest.json") {
                    file_count++;
                }
            }
        }
        if (subdir_count == 1 && file_count == 0) {
            cur = single_subdir;
        } else {
            break;
        }
    }
    return cur;
}

void collect_tree_rel_paths(const std::filesystem::path& base,
                            const std::string& prefix,
                            std::vector<std::string>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        auto rel = it->path().lexically_relative(base);
        std::string r = lower(path_to_utf8(rel));
        for (char& c : r) if (c == '\\') c = '/';
        out.push_back(prefix.empty() ? r : (prefix + "/" + r));
    }
}

}  // namespace

std::filesystem::path mods_dir(const std::filesystem::path& root) { return root / "mods"; }
std::filesystem::path ini_path(const std::filesystem::path& root) { return root / "mods.ini"; }

void parse_rules(const std::string& text, Mod& out) {
    out.presets.clear();
    out.texture_rules = 0;
    std::istringstream in(text);
    std::string line;
    enum class Sec { None, Definition, Preset, TextureRedefine };
    Sec sec = Sec::None;

    std::string cur_preset_name;
    std::string cur_preset_cat;
    bool cur_preset_default = false;
    auto flush_preset = [&]() {
        if (sec != Sec::Preset || cur_preset_name.empty()) return;
        PresetCategory* cat = nullptr;
        for (auto& pc : out.presets) {
            if (pc.category == cur_preset_cat) {
                cat = &pc;
                break;
            }
        }
        if (!cat) {
            out.presets.push_back(PresetCategory{cur_preset_cat, {}, "", ""});
            cat = &out.presets.back();
        }
        if (std::find(cat->options.begin(), cat->options.end(), cur_preset_name) == cat->options.end()) {
            cat->options.push_back(cur_preset_name);
        }
        if (cur_preset_default || cat->default_option.empty()) {
            cat->default_option = cur_preset_name;
            cat->active = cur_preset_name;
        }
        cur_preset_name.clear();
        cur_preset_cat.clear();
        cur_preset_default = false;
    };

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            flush_preset();
            std::string l = lower(line);
            if (l.rfind("[definition]", 0) == 0) sec = Sec::Definition;
            else if (l.rfind("[preset]", 0) == 0) sec = Sec::Preset;
            else if (l.rfind("[textureredefine]", 0) == 0) {
                sec = Sec::TextureRedefine;
                out.texture_rules++;
            } else {
                sec = Sec::None;
            }
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, eq)));
        std::string v = unquote(line.substr(eq + 1));
        if (sec == Sec::Definition) {
            if (k == "name" && !v.empty()) out.name = v;
            else if (k == "description") out.description = v;
            else if (k == "version") out.version = v;
        } else if (sec == Sec::Preset) {
            if (k == "name") cur_preset_name = v;
            else if (k == "category") cur_preset_cat = v;
            else if (k == "default") cur_preset_default = (v == "1" || lower(v) == "true");
        }
    }
    flush_preset();
}

State load(const std::filesystem::path& root) {
    State st;
    if (root.empty()) return st;
    std::error_code ec;
    std::vector<Mod> found;
    std::map<std::string, std::vector<std::string>> mod_files;

    auto dir = mods_dir(root);
    if (std::filesystem::is_directory(dir, ec)) {
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            Mod m;
            m.folder = path_to_utf8(it->path().filename());
            if (!m.folder.empty() && m.folder[0] == '.') continue;
            m.name = m.folder;

            std::filesystem::path mod_root = resolve_mod_root(it->path());
            std::string rules;
            if (read_text(mod_root / "rules.txt", rules) ||
                (mod_root != it->path() && read_text(it->path() / "rules.txt", rules))) {
                m.has_rules = true;
                parse_rules(rules, m);
            }

            std::vector<std::string> rels;
            if (std::filesystem::is_directory(mod_root / "content", e2)) {
                size_t before = rels.size();
                collect_tree_rel_paths(mod_root / "content", "content", rels);
                m.content_files += static_cast<uint32_t>(rels.size() - before);
                m.has_content = true;
            }
            for (auto dit = std::filesystem::directory_iterator(mod_root, e2);
                 !e2 && dit != std::filesystem::directory_iterator(); dit.increment(e2)) {
                std::error_code e3;
                std::string dname = path_to_utf8(dit->path().filename());
                if (dit->is_directory(e3) && is_botw_content_subdir(dname)) {
                    size_t before = rels.size();
                    collect_tree_rel_paths(dit->path(), "content/" + lower(dname), rels);
                    m.content_files += static_cast<uint32_t>(rels.size() - before);
                    m.has_content = true;
                } else if (dit->is_regular_file(e3)) {
                    if (is_patch_filename(dname)) m.patch_files++;
                    if (is_shader_filename(dname)) m.shader_files++;
                }
            }
            if (std::filesystem::is_directory(mod_root / "aoc", e2)) {
                size_t before = rels.size();
                if (std::filesystem::is_directory(mod_root / "aoc" / "content" / "0010", e2)) {
                    collect_tree_rel_paths(mod_root / "aoc" / "content" / "0010", "aoc/0010", rels);
                } else {
                    collect_tree_rel_paths(mod_root / "aoc", "aoc", rels);
                }
                m.aoc_files += static_cast<uint32_t>(rels.size() - before);
                m.has_aoc = true;
            } else if (std::filesystem::is_directory(mod_root / "0010", e2)) {
                size_t before = rels.size();
                collect_tree_rel_paths(mod_root / "0010", "aoc/0010", rels);
                m.aoc_files += static_cast<uint32_t>(rels.size() - before);
                m.has_aoc = true;
            }
            if (std::filesystem::is_directory(mod_root / "shaders", e2)) {
                for (auto sit = std::filesystem::directory_iterator(mod_root / "shaders", e2);
                     !e2 && sit != std::filesystem::directory_iterator(); sit.increment(e2)) {
                    std::error_code e3;
                    if (sit->is_regular_file(e3) && is_shader_filename(path_to_utf8(sit->path().filename()))) {
                        m.shader_files++;
                    }
                }
            }

            mod_files[m.folder] = std::move(rels);
            found.push_back(std::move(m));
        }
    }
    std::sort(found.begin(), found.end(), [](const Mod& a, const Mod& b) { return a.folder < b.folder; });

    std::string text;
    Ini ini;
    if (read_text(ini_path(root), text)) ini = parse_ini(text);
    if (auto* en = ini.find("Enabled")) {
        for (auto& [k, v] : en->keys)
            for (auto& m : found)
                if (lower(m.folder) == lower(k)) m.enabled = trim(v) == "1";
    }
    if (auto* pr = ini.find("Presets")) {
        for (auto& [k, v] : pr->keys) {
            std::string mod_key = k;
            std::string cat_key;
            auto colon = k.find(':');
            if (colon != std::string::npos) {
                mod_key = trim(k.substr(0, colon));
                cat_key = trim(k.substr(colon + 1));
            }
            for (auto& m : found) {
                if (lower(m.folder) != lower(mod_key)) continue;
                for (auto& pc : m.presets) {
                    if (lower(pc.category) == lower(cat_key) || (cat_key.empty() && m.presets.size() == 1)) {
                        if (std::find(pc.options.begin(), pc.options.end(), v) != pc.options.end()) {
                            pc.active = v;
                        }
                    }
                }
            }
        }
    }

    std::vector<std::string> order;
    if (auto* ord = ini.find("Order")) {
        for (auto& [k, v] : ord->keys)
            if (lower(k) == "order") {
                order = split_order(v);
                st.has_order = true;
            }
    }
    for (auto& name : order) {
        auto it = std::find_if(found.begin(), found.end(), [&](const Mod& m) { return lower(m.folder) == lower(name); });
        if (it == found.end()) continue;
        st.list.push_back(std::move(*it));
        found.erase(it);
    }
    for (auto& m : found) st.list.push_back(std::move(m));

    // Compute conflicts between enabled mods in load order.
    std::map<std::string, size_t> file_owner;
    for (size_t i = 0; i < st.list.size(); ++i) {
        if (!st.list[i].enabled) continue;
        for (const auto& f : mod_files[st.list[i].folder]) {
            auto it = file_owner.find(f);
            if (it != file_owner.end() && it->second != i) {
                size_t prev = it->second;
                st.list[prev].overridden_files++;
                const std::string& prev_name = st.list[prev].name;
                const std::string& cur_name = st.list[i].name;
                if (std::find(st.list[i].overrides_mods.begin(), st.list[i].overrides_mods.end(), prev_name) == st.list[i].overrides_mods.end()) {
                    st.list[i].overrides_mods.push_back(prev_name);
                }
                if (std::find(st.list[prev].overridden_by_mods.begin(), st.list[prev].overridden_by_mods.end(), cur_name) == st.list[prev].overridden_by_mods.end()) {
                    st.list[prev].overridden_by_mods.push_back(cur_name);
                }
            }
            file_owner[f] = i;
        }
    }

    return st;
}

bool save(const std::filesystem::path& root, const State& st, bool write_order, std::string& err) {
    if (root.empty()) {
        err = "No user data folder.";
        return false;
    }
    std::string text;
    Ini ini;
    if (read_text(ini_path(root), text)) ini = parse_ini(text);

    auto* en = ini.find("Enabled");
    if (!en) {
        ini.sections.push_back({"Enabled", {}});
        en = &ini.sections.back();
    }
    for (auto& m : st.list) {
        bool set = false;
        for (auto& [k, v] : en->keys)
            if (lower(k) == lower(m.folder)) {
                v = m.enabled ? "1" : "0";
                set = true;
            }
        if (!set) en->keys.emplace_back(m.folder, m.enabled ? "1" : "0");
    }

    if (write_order || st.has_order) {
        std::string order;
        for (auto& m : st.list) {
            if (!order.empty()) order += ",";
            order += m.folder;
        }
        auto* ord = ini.find("Order");
        if (!ord) {
            ini.sections.push_back({"Order", {}});
            ord = &ini.sections.back();
        }
        bool set = false;
        for (auto& [k, v] : ord->keys)
            if (lower(k) == "order") {
                v = order;
                set = true;
            }
        if (!set) ord->keys.emplace_back("order", order);
    }

    bool any_presets = false;
    for (const auto& m : st.list) {
        if (!m.presets.empty()) {
            any_presets = true;
            break;
        }
    }
    if (any_presets || ini.find("Presets")) {
        auto* pr = ini.find("Presets");
        if (!pr) {
            ini.sections.push_back({"Presets", {}});
            pr = &ini.sections.back();
        }
        for (const auto& m : st.list) {
            for (const auto& pc : m.presets) {
                if (pc.active.empty()) continue;
                std::string key = pc.category.empty() ? m.folder : (m.folder + ":" + pc.category);
                bool set = false;
                for (auto& [k, v] : pr->keys) {
                    if (lower(k) == lower(key) || (pc.category.empty() && lower(k) == lower(m.folder))) {
                        k = key;
                        v = pc.active;
                        set = true;
                    }
                }
                if (!set) pr->keys.emplace_back(key, pc.active);
            }
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    auto path = ini_path(root);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "Could not write mods.ini.";
            return false;
        }
        bool first = true;
        for (auto& s : ini.sections) {
            if (!first) out << "\r\n";
            first = false;
            if (!s.name.empty()) out << "[" << s.name << "]\r\n";
            for (auto& [k, v] : s.keys) out << k << "=" << v << "\r\n";
        }
        if (!out) {
            err = "Could not write mods.ini.";
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        err = "Could not replace mods.ini: " + ec.message();
        return false;
    }
    return true;
}

int enabled_count(const State& st) {
    int n = 0;
    for (auto& m : st.list) n += m.enabled ? 1 : 0;
    return n;
}

bool install_mod(const std::filesystem::path& user_root,
                 const std::filesystem::path& src_path,
                 std::string& installed_folder_out,
                 std::string& err) {
    if (user_root.empty()) {
        err = "Set the user data folder in Settings first.";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(src_path, ec)) {
        err = "Selected mod path does not exist.";
        return false;
    }

    std::string stem = path_to_utf8(src_path.stem());
    if (stem.empty()) stem = path_to_utf8(src_path.filename());
    if (stem.empty()) stem = "InstalledMod";

    std::filesystem::path mdir = mods_dir(user_root);
    std::filesystem::create_directories(mdir, ec);
    if (ec) {
        err = "Could not create mods folder: " + ec.message();
        return false;
    }

    std::filesystem::path staging = mdir / utf8_to_path(".install_tmp_" + stem);
    std::filesystem::remove_all(staging, ec);

    bool is_dir = std::filesystem::is_directory(src_path, ec);
    bool is_zip = false;
    if (!is_dir && std::filesystem::is_regular_file(src_path, ec)) {
        std::string ext = lower(path_to_utf8(src_path.extension()));
        is_zip = (ext == ".zip");
    }

    if (is_zip) {
        if (!unzip_to(src_path, staging, err)) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    } else if (is_dir) {
        std::filesystem::create_directories(staging, ec);
        std::filesystem::copy(src_path, staging,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "Could not copy mod folder: " + ec.message();
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    } else {
        err = "Select a mod folder or a .zip archive.";
        return false;
    }

    std::filesystem::path resolved = resolve_mod_root(staging);
    if (!looks_like_mod_root(resolved)) {
        std::filesystem::remove_all(staging, ec);
        err = "That folder or .zip does not look like a BotW mod (no rules.txt, content, aoc, Actor/Pack folders, or patches.txt found).";
        return false;
    }

    std::string final_folder = stem;
    if (resolved != staging) {
        std::string inner = path_to_utf8(resolved.filename());
        if (!inner.empty() && lower(inner) != "content" && lower(inner) != "aoc") {
            final_folder = inner;
        }
    }
    for (char& c : final_folder) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }

    std::filesystem::path dest = mdir / utf8_to_path(final_folder);
    std::filesystem::remove_all(dest, ec);
    std::filesystem::rename(resolved, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::create_directories(dest, ec);
        std::filesystem::copy(resolved, dest,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::filesystem::remove_all(staging, ec);
    if (ec) {
        err = "Could not finalize mod installation: " + ec.message();
        return false;
    }

    installed_folder_out = final_folder;
    State st = load(user_root);
    bool found_mod = false;
    for (auto& m : st.list) {
        if (lower(m.folder) == lower(final_folder)) {
            m.enabled = true;
            found_mod = true;
        }
    }
    if (!found_mod) {
        Mod m;
        m.folder = final_folder;
        m.name = final_folder;
        m.enabled = true;
        st.list.push_back(std::move(m));
    }
    return save(user_root, st, st.has_order, err);
}

}  // namespace mods
