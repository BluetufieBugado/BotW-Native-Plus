#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "mods.h"
#include "unzip.h"

#include "coreinit/mod_expr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    if (l.size() > 10 && l.rfind("patch_", 0) == 0 && (l.substr(l.size() - 4) == ".txt" || l.substr(l.size() - 4) == ".asm")) return true;
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
    if (std::filesystem::is_regular_file(dir / "info.json", ec)) return true;
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

namespace {

// Cuts a trailing comment the way the game does: # anywhere, // and " ;" outside double quotes.
std::string strip_inline_comment(const std::string& line) {
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') in_quotes = !in_quotes;
        if (in_quotes) continue;
        if (c == '#') return line.substr(0, i);
        if (c == ';' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) return line.substr(0, i);
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') return line.substr(0, i);
    }
    return line;
}

// Cemu writes a line break inside a description as a literal backslash and n.
std::string unescape_newlines(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            o += '\n';
            i++;
        } else {
            o += s[i];
        }
    }
    return o;
}

// Variable name as the expression evaluator knows it: no leading $, no ":type" suffix.
std::string var_name(std::string name) {
    if (!name.empty() && name[0] == '$') name.erase(0, 1);
    auto colon = name.find(':');
    if (colon != std::string::npos) name.erase(colon);
    return trim(name);
}

void set_var(std::vector<std::pair<std::string, double>>& vars, const std::string& raw_name, double val) {
    std::string name = var_name(raw_name);
    if (name.empty()) return;
    for (auto& kv : vars) {
        if (kv.first == name) {
            kv.second = val;
            return;
        }
    }
    vars.emplace_back(std::move(name), val);
}

void eval_into(std::vector<std::pair<std::string, double>>& vars, const std::string& name, const std::string& expr) {
    double v = 0.0;
    if (mods::expr::eval(expr, vars, &v, true)) set_var(vars, name, v);
}

// Rebuilds Mod::presets (one entry per category, in order of first appearance) from preset_defs.
void build_categories(Mod& out) {
    out.presets.clear();
    for (const auto& pd : out.preset_defs) {
        PresetCategory* cat = nullptr;
        for (auto& pc : out.presets)
            if (pc.category == pd.category) {
                cat = &pc;
                break;
            }
        if (!cat) {
            out.presets.push_back(PresetCategory{pd.category, {}, "", ""});
            cat = &out.presets.back();
        }
        if (std::find(cat->options.begin(), cat->options.end(), pd.name) == cat->options.end()) cat->options.push_back(pd.name);
    }
    for (auto& pc : out.presets) {
        // The first option marked default = 1 wins, as in the game. Without one, the first option.
        pc.default_option = pc.options.front();
        for (const auto& pd : out.preset_defs)
            if (pd.category == pc.category && pd.is_default) {
                pc.default_option = pd.name;
                break;
            }
        pc.active = pc.default_option;
    }
}

}  // namespace

void parse_rules(const std::string& text, Mod& out) {
    out.presets.clear();
    out.preset_defs.clear();
    out.default_vars.clear();
    out.texture_rules = 0;
    std::istringstream in(text);
    std::string line;
    enum class Sec { None, Definition, Default, Preset, TextureRedefine };
    Sec sec = Sec::None;

    PresetOption cur;
    bool have_preset = false;
    auto flush_preset = [&]() {
        if (have_preset && !cur.name.empty()) out.preset_defs.push_back(std::move(cur));
        cur = PresetOption{};
        have_preset = false;
    };

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            flush_preset();
            std::string l = lower(line);
            if (l.rfind("[definition]", 0) == 0) sec = Sec::Definition;
            else if (l.rfind("[default]", 0) == 0) sec = Sec::Default;
            else if (l.rfind("[preset]", 0) == 0) {
                sec = Sec::Preset;
                have_preset = true;
            } else if (l.rfind("[textureredefine]", 0) == 0) {
                sec = Sec::TextureRedefine;
                out.texture_rules++;
            } else {
                sec = Sec::None;
            }
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string raw_k = trim(line.substr(0, eq));
        std::string k = lower(raw_k);
        // Names, descriptions and paths are free text: a # or // in them is part of the text.
        bool free_text = (sec == Sec::Definition && (k == "name" || k == "description" || k == "path")) ||
                         (sec == Sec::Preset && k == "name");
        std::string v = unquote(free_text ? line.substr(eq + 1) : strip_inline_comment(line.substr(eq + 1)));

        if (!raw_k.empty() && raw_k[0] == '$') {
            // $variable = expression: a preset's own values, or the defaults before any preset.
            if (sec == Sec::Preset) cur.vars.emplace_back(raw_k, v);
            else out.default_vars.emplace_back(raw_k, v);
            continue;
        }
        if (sec == Sec::Definition) {
            if (k == "name" && !v.empty()) out.name = v;
            else if (k == "description") out.description = unescape_newlines(v);
            else if (k == "version") out.version = v;
            else if (k == "path") out.path = v;
        } else if (sec == Sec::Preset) {
            if (k == "name") cur.name = v;
            else if (k == "category") cur.category = v;
            else if (k == "default") cur.is_default = (v == "1" || lower(v) == "true" || lower(v) == "yes");
            else if (k == "condition") cur.condition = v;
        } else if (sec == Sec::Default) {
            out.default_vars.emplace_back(raw_k, v);
        }
    }
    flush_preset();
    build_categories(out);
}

std::vector<PresetChoice> visible_presets(const Mod& mod) {
    std::vector<PresetChoice> out;
    std::vector<std::pair<std::string, double>> vars;
    for (const auto& kv : mod.default_vars) eval_into(vars, kv.first, kv.second);

    for (size_t ci = 0; ci < mod.presets.size(); ci++) {
        const PresetCategory& pc = mod.presets[ci];
        std::vector<const PresetOption*> offered;
        for (const auto& pd : mod.preset_defs) {
            if (pd.category != pc.category) continue;
            if (!pd.condition.empty()) {
                double c = 0.0;
                if (!mods::expr::eval(pd.condition, vars, &c, true) || std::fabs(c) <= 1e-12) continue;
            }
            offered.push_back(&pd);
        }
        if (offered.empty()) continue;

        PresetChoice ch;
        ch.category_index = ci;
        ch.label = pc.category.empty() ? "Preset" : pc.category;
        const PresetOption* chosen = nullptr;
        const PresetOption* dflt = nullptr;
        for (const PresetOption* po : offered) {
            if (std::find(ch.options.begin(), ch.options.end(), po->name) == ch.options.end()) ch.options.push_back(po->name);
            if (!chosen && !pc.active.empty() && lower(po->name) == lower(pc.active)) chosen = po;
            if (!dflt && po->is_default) dflt = po;
        }
        if (!chosen) chosen = dflt ? dflt : offered.front();
        ch.active = chosen->name;
        out.push_back(std::move(ch));
        for (const auto& kv : chosen->vars) eval_into(vars, kv.first, kv.second);
    }
    return out;
}

void reset_presets(Mod& mod) {
    for (auto& pc : mod.presets) pc.active = pc.default_option;
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
                        // The game matches preset names without regard to case; keep the declared spelling.
                        for (const auto& o : pc.options)
                            if (lower(o) == lower(v)) {
                                pc.active = o;
                                break;
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
        bool created = false;
        if (!pr) {
            ini.sections.push_back({"Presets", {}});
            pr = &ini.sections.back();
            created = true;
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
                // A pick that is just the pack's default only needs a line when one is already there
                // (to put an earlier choice back), so mods.ini stays short with many packs installed.
                if (!set && pc.active != pc.default_option) pr->keys.emplace_back(key, pc.active);
            }
        }
        if (created && pr->keys.empty()) ini.sections.pop_back();  // nothing to say: no empty [Presets]
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


std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            if (esc == 'n') out.push_back('\n');
            else if (esc == 'r') {}
            else if (esc == 't') out.push_back(' ');
            else out.push_back(esc);
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return trim(out);
}

void ensure_bnp_rules(const std::filesystem::path& mod_dir, std::string* out_display_name) {
    std::error_code ec;
    std::filesystem::path info_p = mod_dir / "info.json";
    if (!std::filesystem::is_regular_file(info_p, ec)) return;
    std::string jtxt;
    if (!read_text(info_p, jtxt)) return;
    std::string name = extract_json_string(jtxt, "name");
    std::string desc = extract_json_string(jtxt, "desc");
    std::string ver  = extract_json_string(jtxt, "version");
    if (name.empty()) return;
    if (out_display_name) {
        *out_display_name = (!ver.empty() && name.find(ver) == std::string::npos) ? (name + " " + ver) : name;
    }
    std::filesystem::path rules_p = mod_dir / "rules.txt";
    if (std::filesystem::is_regular_file(rules_p, ec)) return;
    std::string desc_line = desc;
    size_t nl = desc_line.find('\n');
    if (nl != std::string::npos) desc_line = trim(desc_line.substr(0, nl));
    if (desc_line.empty()) desc_line = name;
    std::ofstream out(rules_p, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "[Definition]\r\n"
        << "titleIds = 00050000101C9300,00050000101C9400,00050000101C9500\r\n"
        << "name = " << name << "\r\n"
        << "path = \"The Legend of Zelda: Breath of the Wild/Mods/" << name << "\"\r\n"
        << "description = " << desc_line << "\r\n"
        << "version = 7\r\n";
}

bool extract_archive_to(const std::filesystem::path& archive_path, const std::filesystem::path& dest, std::string& err) {
    std::string zip_err;
    if (unzip_to(archive_path, dest, zip_err)) return true;
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    std::wstring cmd = L"C:\\Windows\\System32\\tar.exe -xf \"" + archive_path.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (exit_code == 0) return true;
    }
#endif
    err = zip_err.empty() ? "Could not extract archive (.zip/.bnp/.7z)." : zip_err;
    return false;
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
    bool is_archive = false;
    if (!is_dir && std::filesystem::is_regular_file(src_path, ec)) {
        std::string ext = lower(path_to_utf8(src_path.extension()));
        is_archive = (ext == ".zip" || ext == ".bnp" || ext == ".7z");
    }

    if (is_archive) {
        if (!extract_archive_to(src_path, staging, err)) {
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
        err = "Select a mod folder or a .zip / .bnp / .7z archive.";
        return false;
    }

    std::filesystem::path resolved = resolve_mod_root(staging);
    if (!looks_like_mod_root(resolved)) {
        std::filesystem::remove_all(staging, ec);
        err = "That folder or archive does not look like a BotW mod (no rules.txt, info.json, content, aoc, Actor/Pack folders, or patches.txt found).";
        return false;
    }

    std::string bnp_name;
    ensure_bnp_rules(resolved, &bnp_name);

    std::string final_folder = !bnp_name.empty() ? bnp_name : stem;
    for (char& c : final_folder) {
        if (c == ' ') c = '_';
    }
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

bool rules_target_botw(const std::string& rules_text) {
    std::istringstream in(lower(rules_text));
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.rfind("titleids", 0) != 0) continue;
        // Game, update and DLC ids of the Japanese, US and European releases share these tails.
        if (t.find("101c9300") != std::string::npos || t.find("101c9400") != std::string::npos ||
            t.find("101c9500") != std::string::npos)
            return true;
    }
    return false;
}

namespace {

constexpr const char* kPackMarker = ".cemu_pack";  // tells our own imported folders from the user's mods

std::string safe_folder_name(std::string s) {
    for (char& c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s;
}

bool has_patch_file(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && is_patch_filename(path_to_utf8(it->path().filename()))) return true;
    }
    return false;
}

struct PackImporter {
    std::filesystem::path root, dest_root;
    ImportReport* rep = nullptr;
    std::set<std::string> used;  // lowercase folder names taken during this run

    bool name_is_free(const std::string& n) const {
        if (n.empty() || used.count(lower(n))) return false;
        std::error_code ec;
        std::filesystem::path d = dest_root / utf8_to_path(n);
        // Free if nothing is there, or if it is a pack we imported earlier (then it gets refreshed).
        return !std::filesystem::exists(d, ec) || std::filesystem::exists(d / kPackMarker, ec);
    }

    void import_pack(const std::filesystem::path& dir) {
        std::string base = safe_folder_name(path_to_utf8(dir.filename()));
        if (base.empty()) base = "CemuPack";
        std::vector<std::string> candidates{base};
        if (dir.parent_path() != root) {
            std::string parent = safe_folder_name(path_to_utf8(dir.parent_path().filename()));
            if (!parent.empty()) candidates.push_back(parent + "_" + base);
        }
        candidates.push_back("Cemu_" + base);
        for (int n = 2; n < 100; ++n) candidates.push_back(base + "_" + std::to_string(n));
        std::string chosen;
        for (const std::string& c : candidates) {
            if (name_is_free(c)) {
                chosen = c;
                break;
            }
        }
        if (chosen.empty()) {
            rep->failed++;
            return;
        }
        std::error_code ec;
        std::filesystem::path dest = dest_root / utf8_to_path(chosen);
        std::filesystem::remove_all(dest, ec);
        std::filesystem::create_directories(dest, ec);
        std::filesystem::copy(dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove_all(dest, ec);
            rep->failed++;
            return;
        }
        std::ofstream(dest / kPackMarker, std::ios::binary) << "imported from the Cemu community graphic packs\n";
        used.insert(lower(chosen));
        rep->imported.push_back(chosen);
        if (has_patch_file(dest)) rep->with_patches++;
    }

    void scan(const std::filesystem::path& dir, int depth) {
        std::string rules;
        if (read_text(dir / "rules.txt", rules)) {
            if (rules_target_botw(rules)) import_pack(dir);
            else rep->skipped_other_games++;
            return;  // a pack's own subfolders are part of the pack
        }
        if (depth >= 6) return;
        std::vector<std::filesystem::path> subs;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            std::string name = path_to_utf8(it->path().filename());
            if (!name.empty() && name[0] == '.') continue;
            subs.push_back(it->path());
        }
        std::sort(subs.begin(), subs.end());  // same folder names resolve the same way every time
        for (const auto& s : subs) scan(s, depth + 1);
    }
};

}  // namespace

bool import_cemu_packs(const std::filesystem::path& user_root, const std::filesystem::path& src,
                       ImportReport& report, std::string& err) {
    report = ImportReport{};
    if (user_root.empty()) {
        err = "Set the user data folder in Settings first.";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        err = "The selected path does not exist.";
        return false;
    }
    std::filesystem::path mdir = mods_dir(user_root);
    std::filesystem::create_directories(mdir, ec);
    if (ec) {
        err = "Could not create the mods folder: " + ec.message();
        return false;
    }

    std::filesystem::path root = src, tmp;
    bool from_zip = false;
    if (std::filesystem::is_regular_file(src, ec)) {
        if (lower(path_to_utf8(src.extension())) != ".zip") {
            err = "Choose a .zip of Cemu graphic packs, or a folder that holds them.";
            return false;
        }
        from_zip = true;
        tmp = mdir / ".cemu_import_tmp";
        std::filesystem::remove_all(tmp, ec);
        // The community archive covers every game; only unpack what is named for Breath of the Wild.
        auto keep = [](const std::string& name) {
            std::string l = lower(name);
            return l.find("breathofthewild") != std::string::npos || l.find("breath of the wild") != std::string::npos ||
                   l.find("botw") != std::string::npos;
        };
        std::string zerr;
        if (!unzip_to(src, tmp, zerr, keep)) {
            std::filesystem::remove_all(tmp, ec);
            err = "Could not read the archive: " + zerr + ". Extract it with Windows and use the folder option instead.";
            return false;
        }
        root = tmp;
    } else if (std::filesystem::is_directory(src, ec)) {
        std::string s = lower(path_to_utf8(std::filesystem::weakly_canonical(src, ec)));
        std::string m = lower(path_to_utf8(std::filesystem::weakly_canonical(mdir, ec)));
        if (s == m || (s.rfind(m, 0) == 0 && s.size() > m.size() && (s[m.size()] == '/' || s[m.size()] == '\\'))) {
            err = "That folder is already inside the mods folder. Choose the folder where you keep the Cemu packs.";
            return false;
        }
    } else {
        err = "Choose a .zip of Cemu graphic packs, or a folder that holds them.";
        return false;
    }

    PackImporter imp;
    imp.root = root;
    imp.dest_root = mdir;
    imp.rep = &report;
    imp.scan(root, 0);
    if (from_zip) std::filesystem::remove_all(tmp, ec);

    if (report.imported.empty()) {
        err = "No Breath of the Wild graphic packs were found there.";
        if (report.skipped_other_games > 0) err += " " + std::to_string(report.skipped_other_games) + " pack(s) for other games were ignored.";
        return false;
    }
    return true;
}

}  // namespace mods
