#include "mods.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
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

}  // namespace

std::filesystem::path mods_dir(const std::filesystem::path& root) { return root / "mods"; }
std::filesystem::path ini_path(const std::filesystem::path& root) { return root / "mods.ini"; }

void parse_rules(const std::string& text, Mod& out) {
    std::istringstream in(text);
    std::string line;
    bool in_def = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            in_def = lower(line).rfind("[definition]", 0) == 0;
            continue;
        }
        if (!in_def) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, eq)));
        std::string v = unquote(line.substr(eq + 1));
        if (k == "name" && !v.empty()) out.name = v;
        else if (k == "description") out.description = v;
        else if (k == "version") out.version = v;
    }
}

State load(const std::filesystem::path& root) {
    State st;
    if (root.empty()) return st;
    std::error_code ec;
    std::vector<Mod> found;
    auto dir = mods_dir(root);
    if (std::filesystem::is_directory(dir, ec)) {
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            Mod m;
            auto u8 = it->path().filename().u8string();
            m.folder.assign(u8.begin(), u8.end());
            m.name = m.folder;
            std::string rules;
            if (read_text(it->path() / "rules.txt", rules)) {
                m.has_rules = true;
                parse_rules(rules, m);
            }
            m.has_content = std::filesystem::is_directory(it->path() / "content", e2);
            m.has_aoc = std::filesystem::is_directory(it->path() / "aoc", e2);
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

}  // namespace mods
