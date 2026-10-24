#include "mods.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_set>

namespace mods {
namespace {

namespace fs = std::filesystem;

constexpr int kUserDlc = -1;  // owner of files from <user>/aoc
constexpr size_t kConflictLines = 50;

struct Node {
    std::string name;
    std::string host;
    bool file = false;
    bool base = false;  // the game dump has a file at this path
    int owner = kUserDlc;
    std::map<std::string, Node> kids;  // key: lowercase name
};

struct Conflict {
    std::string path;  // "content/..." or "aoc/..."
    int loser = 0, winner = 0;
};

std::mutex g_case_mu;
std::mutex g_open_mu;
Node g_content;
Node g_aoc;
std::string g_content_base;
std::string g_aoc_base;
uint64_t g_title = 0x0005000C101C9400ull;
bool g_title_from_meta = false;
bool g_pack = false;
bool g_inited = false;
bool g_no_mods = false;
std::vector<ModInfo> g_mods;
std::vector<Conflict> g_conflicts;
std::unordered_set<uint16_t> g_owned;
std::unordered_set<std::string> g_opened;
std::map<std::string, std::map<std::string, std::string>> g_case_cache;
size_t g_files = 0;
size_t g_replacing = 0;
size_t g_added = 0;

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool iless(const std::string& a, const std::string& b) {
    std::string la = lower_ascii(a), lb = lower_ascii(b);
    return la != lb ? la < lb : a < b;
}

std::string trim(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return std::string(s.substr(a, b - a));
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// Calls fn(section_lowercase, key_lowercase, value) for every key = value line.
template <class Fn>
void each_ini_line(std::string_view text, Fn fn) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string line = trim(text.substr(at, nl - at));
        at = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            section = lower_ascii(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        if (key.empty()) continue;
        fn(section, key, trim(line.substr(eq + 1)));
    }
}

bool valid_folder(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (unsigned char c : s) {
        if (c < 32) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

// Narrow host path, the form fopen and the rest of fs.cpp use. Empty when a name cannot be
// represented (non-ANSI names on Windows); such files are skipped.
std::string path_str(const fs::path& p) {
    try {
        return p.string();
    } catch (...) {
        return {};
    }
}

std::string read_text(const fs::path& p, size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return {};
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    return s;
}

std::vector<std::string> split_rel(std::string_view rel) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return true;
        }
        if (cur == "..") return false;
        parts.push_back(cur);
        cur.clear();
        return true;
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (!flush()) return {};
        } else {
            cur.push_back(c);
        }
    }
    if (!flush()) return {};
    return parts;
}

struct GuestVol {
    enum class Id { Other, Content, Aoc, Code, Meta } id = Id::Other;
    std::string rel;
};

GuestVol parse_guest(std::string guest) {
    for (char& c : guest)
        if (c == '\\') c = '/';
    std::string cur;
    std::vector<std::string> parts;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char c : guest) {
        if (c == '/') flush();
        else cur.push_back(c);
    }
    flush();
    GuestVol v;
    if (parts.size() < 2 || parts[0] != "vol") return v;
    auto join_from = [&](size_t i) {
        std::string r;
        for (; i < parts.size(); i++) {
            if (!r.empty()) r += '/';
            r += parts[i];
        }
        return r;
    };
    if (parts[1] == "content") {
        v.id = GuestVol::Id::Content;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1] == "code") {
        v.id = GuestVol::Id::Code;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1] == "meta") {
        v.id = GuestVol::Id::Meta;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1].compare(0, 3, "aoc") == 0) {
        const std::string& name = parts[1];
        if (name.size() == 3 + 16) {
            for (size_t i = 3; i < name.size(); i++)
                if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return v;
        } else if (name.size() != 3) {
            return v;
        }
        v.id = GuestVol::Id::Aoc;
        v.rel = join_from(2);
        return v;
    }
    return v;
}

const Node* walk(const Node& root, const std::string& rel, bool& file, bool& occluded) {
    file = false;
    occluded = false;
    if (rel.empty()) return root.kids.empty() ? nullptr : &root;
    const Node* n = &root;
    auto parts = split_rel(rel);
    if (parts.empty()) {
        occluded = true;
        return nullptr;
    }
    for (size_t i = 0; i < parts.size(); i++) {
        auto it = n->kids.find(lower_ascii(parts[i]));
        if (it == n->kids.end()) return nullptr;
        n = &it->second;
        if (n->file && i + 1 < parts.size()) {
            occluded = true;
            return nullptr;
        }
    }
    file = n->file;
    return n;
}

std::string lookup_component(const std::string& dir, const std::string& want_lower) {
    std::lock_guard<std::mutex> lock(g_case_mu);
    auto& cached = g_case_cache[dir];
    if (cached.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = path_str(it->path().filename());
            if (!name.empty()) cached.emplace(lower_ascii(name), name);
        }
        cached.emplace("", "");  // mark the directory as cached even when it is empty
    }
    auto it = cached.find(want_lower);
    if (it == cached.end()) return {};
    return it->second;
}

// Host path of base/rel. Tries the exact spelling first, then matches each component without
// regard to ASCII case (Linux and macOS hosts; Windows already ignores case).
std::string case_path(const std::string& base, const std::string& rel) {
    if (base.empty()) return {};
    fs::path exact(base);
    auto parts = split_rel(rel);
    for (const auto& part : parts) exact /= part;
    std::error_code ec;
    auto st = fs::status(exact, ec);
    if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) return path_str(exact);
    if (rel.empty()) return {};
    std::string cur = base;
    for (const auto& part : parts) {
        fs::path next = fs::path(cur) / part;
        st = fs::status(next, ec);
        if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) {
            cur = path_str(next);
            continue;
        }
        std::string actual = lookup_component(cur, lower_ascii(part));
        if (actual.empty()) return {};
        cur = path_str(fs::path(cur) / actual);
    }
    return cur;
}

bool is_file(const std::string& host, uint64_t& size) {
    if (host.empty()) return false;
    std::error_code ec;
    auto st = fs::status(fs::path(host), ec);
    if (ec || !fs::is_regular_file(st)) return false;
    auto n = fs::file_size(fs::path(host), ec);
    if (ec) return false;
    size = static_cast<uint64_t>(n);
    return true;
}

bool is_dir(const std::string& host) {
    if (host.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(host), ec);
}

// Child of `dir` named `want` in any ASCII case, or empty.
fs::path child_ci(const fs::path& dir, const std::string& want) {
    std::error_code ec;
    if (fs::is_directory(dir / want, ec)) return dir / want;
    std::string lw = lower_ascii(want);
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        if (lower_ascii(path_str(it->path().filename())) == lw) return it->path();
    }
    return {};
}

std::string owner_name(int owner) {
    if (owner < 0 || owner >= (int)g_mods.size()) return "user DLC folder";
    return g_mods[owner].folder;
}

// Later calls win. A path that an earlier mod already provided is recorded as a conflict.
void insert_file(Node& root, const std::string& rel, const std::string& host, int owner, bool base, const char* vol) {
    auto parts = split_rel(rel);
    if (parts.empty()) return;
    Node* n = &root;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::string key = lower_ascii(parts[i]);
        Node& child = n->kids[key];
        if (child.name.empty()) {
            child.name = parts[i];
            child.owner = owner;
        }
        if (child.file) {
            // A lower-priority mod had a file where this one has a folder.
            if (child.owner != kUserDlc && child.owner != owner) g_conflicts.push_back({std::string(vol) + "/" + rel, child.owner, owner});
            child.file = false;
            child.host.clear();
        }
        n = &child;
    }
    std::string key = lower_ascii(parts.back());
    Node& leaf = n->kids[key];
    if ((leaf.file || !leaf.kids.empty()) && leaf.owner != kUserDlc && leaf.owner != owner) g_conflicts.push_back({std::string(vol) + "/" + rel, leaf.owner, owner});
    leaf.name = parts.back();
    leaf.host = host;
    leaf.file = true;
    leaf.base = base;
    leaf.owner = owner;
    leaf.kids.clear();
}

// Adds every regular file below host_root. Returns the file count; `replaces` counts files that
// also exist in the game dump below `base`.
size_t add_tree(Node& root, const fs::path& host_root, const std::string& base, int owner, const char* vol, size_t& replaces) {
    std::error_code ec;
    if (host_root.empty() || !fs::is_directory(host_root, ec)) return 0;
    size_t nfiles = 0, skipped = 0;
    fs::recursive_directory_iterator it(host_root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(sec)) continue;
        std::string host = path_str(it->path());
        std::string generic;
        try {
            generic = it->path().lexically_relative(host_root).generic_string();
        } catch (...) {
            generic.clear();
        }
        if (host.empty() || generic.empty() || generic.rfind("..", 0) == 0) {
            skipped++;
            continue;
        }
        uint64_t sz = 0;
        bool in_base = is_file(case_path(base, generic), sz);
        if (in_base) replaces++;
        insert_file(root, generic, host, owner, in_base, vol);
        nfiles++;
    }
    if (skipped) LOG("mods: %zu files under %s skipped (name not readable)", skipped, path_str(host_root).c_str());
    return nfiles;
}

fs::path aoc_content_root(const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return {};
    fs::path content = child_ci(folder, "content");
    if (!content.empty() && !child_ci(content, "0010").empty()) return content;
    if (!child_ci(folder, "0010").empty()) return folder;
    if (!content.empty()) return content;
    return folder;
}

void tally(const Node& node) {
    for (const auto& [key, child] : node.kids) {
        if (child.file) {
            g_files++;
            if (child.base) g_replacing++;
            else g_added++;
        } else {
            tally(child);
        }
    }
}

bool hex4(const std::string& s) {
    if (s.size() != 4) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

void collect_owned(const Node& root) {
    for (const auto& [key, child] : root.kids)
        if (!child.file && hex4(key)) g_owned.insert(static_cast<uint16_t>(std::strtoul(key.c_str(), nullptr, 16)));
}

void collect_owned_base(const std::string& base) {
    if (base.empty()) return;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(base), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
        std::string name = lower_ascii(path_str(it->path().filename()));
        if (hex4(name)) g_owned.insert(static_cast<uint16_t>(std::strtoul(name.c_str(), nullptr, 16)));
    }
}

uint64_t parse_title(const std::string& game) {
    if (game.empty()) return 0;
    fs::path root(game);
    std::string text = read_text(root / "meta" / "meta.xml", 64 * 1024);
    if (text.empty()) text = read_text(root / "code" / "app.xml", 64 * 1024);
    std::string low = lower_ascii(text);
    auto at = low.find("title_id");
    if (at == std::string::npos) return 0;
    std::string hex;
    for (size_t i = at; i < low.size(); i++) {
        unsigned char c = static_cast<unsigned char>(low[i]);
        if (std::isxdigit(c)) {
            hex.push_back(static_cast<char>(c));
            if (hex.size() == 16) break;
        } else if (!hex.empty()) {
            hex.clear();
        }
    }
    if (hex.size() != 16) return 0;
    return std::strtoull(hex.c_str(), nullptr, 16);
}

bool base_nonempty(const std::string& base) {
    if (!is_dir(base)) return false;
    std::error_code ec;
    fs::directory_iterator it(fs::path(base), ec);
    return !ec && it != fs::directory_iterator();
}

// Cemu graphic pack code patches and shader replacements sit next to rules.txt. They are not
// supported; say so instead of silently doing nothing.
size_t ignored_extras(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_regular_file(sec)) continue;
        std::string name = lower_ascii(path_str(it->path().filename()));
        bool patch = name.rfind("patch_", 0) == 0 || name == "patches.txt";
        bool shader = name.size() > 7 && (name.find("_ps.txt") != std::string::npos || name.find("_vs.txt") != std::string::npos ||
                                          name.find("_gs.txt") != std::string::npos);
        if (patch || shader) n++;
    }
    return n;
}

std::string base_for(GuestVol::Id id) {
    if (id == GuestVol::Id::Content) return g_content_base;
    if (id == GuestVol::Id::Aoc) return g_aoc_base;
    return {};
}

const Node& tree_for(GuestVol::Id id) { return id == GuestVol::Id::Aoc ? g_aoc : g_content; }

void reset() {
    g_content = Node{};
    g_aoc = Node{};
    g_content_base.clear();
    g_aoc_base.clear();
    g_title = 0x0005000C101C9400ull;
    g_title_from_meta = false;
    g_pack = false;
    g_no_mods = false;
    g_mods.clear();
    g_conflicts.clear();
    g_owned.clear();
    g_opened.clear();
    g_case_cache.clear();
    g_files = g_replacing = g_added = 0;
}

void load_mods(const fs::path& user) {
    fs::path mods_dir = user / "mods";
    std::error_code ec;
    std::vector<std::string> present;
    for (fs::directory_iterator it(mods_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        std::string name = path_str(it->path().filename());
        if (valid_folder(name)) present.push_back(name);
    }
    std::string ini = read_text(user / "mods.ini", 256 * 1024);
    Config cfg = parse_config(ini);
    std::vector<std::string> order = load_order(cfg, present);
    LOG("mods: %zu mod folders in %s, %zu enabled%s", present.size(), path_str(mods_dir).c_str(), order.size(),
        ini.empty() ? " (no mods.ini)" : "");
    for (const auto& e : cfg.enabled) {
        if (!e.enabled) continue;
        bool found = false;
        for (const auto& p : present)
            if (lower_ascii(p) == lower_ascii(e.folder)) found = true;
        if (!found) LOG("mods: %s is enabled in mods.ini but has no folder in mods/", e.folder.c_str());
    }

    for (const std::string& folder : order) {
        fs::path dir = mods_dir / folder;
        Rules rules = parse_rules(read_text(dir / "rules.txt", 64 * 1024));
        ModInfo info;
        info.folder = folder;
        info.name = rules.name.empty() ? folder : rules.name;
        info.description = rules.description;
        info.version = rules.version;
        int owner = (int)g_mods.size();
        g_mods.push_back(info);
        ModInfo& m = g_mods.back();
        if (!rules.title_ids.empty()) {
            bool usa = false;
            for (const auto& t : rules.title_ids)
                if (t.find("101c9400") != std::string::npos) usa = true;
            if (!usa) LOG("mods: %s: rules.txt titleIds do not list 00050000101C9400 (BotW USA); loading it anyway", folder.c_str());
        }
        fs::path content = child_ci(dir, "content");
        fs::path aoc = child_ci(dir, "aoc");
        if (!content.empty()) m.content_files = add_tree(g_content, content, g_content_base, owner, "content", m.replaces);
        if (!aoc.empty()) m.aoc_files = add_tree(g_aoc, aoc_content_root(aoc), g_aoc_base, owner, "aoc", m.replaces);
        size_t extras = ignored_extras(dir);
        std::string label = m.name == folder ? folder : m.name + " [" + folder + "]";
        if (!m.version.empty()) label += " version " + m.version;
        size_t total = m.content_files + m.aoc_files;
        LOG("mod %d: %s: %zu files (%zu content, %zu aoc), %zu replace game files, %zu new", owner + 1, label.c_str(), total,
            m.content_files, m.aoc_files, m.replaces, total - m.replaces);
        if (content.empty() && aoc.empty()) LOG("mod %d: %s has no content or aoc folder, nothing to load", owner + 1, folder.c_str());
        if (extras)
            LOG("mod %d: %s: %zu code patch or shader files ignored (not supported)", owner + 1, folder.c_str(), extras);
    }

    for (const Conflict& c : g_conflicts) {
        if (c.loser >= 0 && c.loser < (int)g_mods.size()) g_mods[c.loser].lost++;
    }
    for (size_t i = 0; i < g_conflicts.size() && i < kConflictLines; i++) {
        const Conflict& c = g_conflicts[i];
        LOG("mods: conflict %s: %s wins over %s", c.path.c_str(), owner_name(c.winner).c_str(), owner_name(c.loser).c_str());
    }
    if (g_conflicts.size() > kConflictLines) LOG("mods: %zu more conflicts not listed", g_conflicts.size() - kConflictLines);
    if (!g_conflicts.empty()) LOG("mods: %zu conflicting files in total, the later mod in the load order wins", g_conflicts.size());
}

}  // namespace

// ---- file formats ----

Rules parse_rules(std::string_view text) {
    Rules r;
    // Only [Definition] matters. Cemu packs have one; with more, the first value of a key wins.
    each_ini_line(text, [&](const std::string& sec, const std::string& key, const std::string& value) {
        if (sec != "definition") return;
        r.has_definition = true;
        std::string k = lower_ascii(key);
        std::string v = unquote(value);
        if (k == "name" && r.name.empty()) r.name = v;
        else if (k == "description" && r.description.empty()) r.description = v;
        else if (k == "version" && r.version.empty()) r.version = v;
        else if (k == "titleids" && r.title_ids.empty()) {
            std::string cur;
            for (char c : v + ",") {
                if (c == ',') {
                    std::string t = lower_ascii(trim(cur));
                    if (!t.empty()) r.title_ids.push_back(t);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
        }
    });
    // Cemu writes "\n" in descriptions as a literal backslash-n; keep it on one log line.
    for (std::string* s : {&r.name, &r.description}) {
        std::string o;
        for (size_t i = 0; i < s->size(); i++) {
            if ((*s)[i] == '\\' && i + 1 < s->size() && (*s)[i + 1] == 'n') {
                o += ' ';
                i++;
            } else {
                o += (*s)[i];
            }
        }
        *s = trim(o);
    }
    return r;
}

bool Config::is_enabled(std::string_view folder) const {
    std::string want = lower_ascii(std::string(folder));
    bool on = false;
    for (const auto& e : enabled)
        if (lower_ascii(e.folder) == want) on = e.enabled;  // a repeated key: the last one counts
    return on;
}

Config parse_config(std::string_view text) {
    Config cfg;
    each_ini_line(text, [&](const std::string& sec, const std::string& key, const std::string& value) {
        if (sec == "enabled") {
            std::string v = lower_ascii(unquote(value));
            bool on = v == "1" || v == "true" || v == "yes" || v == "on";
            if (valid_folder(key)) cfg.enabled.push_back({key, on});
        } else if (sec == "order" && lower_ascii(key) == "order") {
            cfg.has_order = true;
            cfg.order.clear();
            std::string cur;
            for (char c : unquote(value) + ",") {
                if (c == ',') {
                    std::string f = trim(cur);
                    cur.clear();
                    if (!valid_folder(f)) continue;
                    bool dup = false;
                    for (const auto& o : cfg.order)
                        if (lower_ascii(o) == lower_ascii(f)) dup = true;
                    if (!dup) cfg.order.push_back(f);
                } else {
                    cur.push_back(c);
                }
            }
        }
    });
    return cfg;
}

std::vector<std::string> load_order(const Config& cfg, const std::vector<std::string>& present) {
    std::vector<std::string> on;
    for (const auto& p : present)
        if (cfg.is_enabled(p)) on.push_back(p);
    std::sort(on.begin(), on.end(), iless);
    if (!cfg.has_order) return on;
    std::vector<std::string> listed, unlisted;
    for (const auto& o : cfg.order)
        for (const auto& p : on)
            if (lower_ascii(p) == lower_ascii(o)) listed.push_back(p);
    for (const auto& p : on)
        if (std::find(listed.begin(), listed.end(), p) == listed.end()) unlisted.push_back(p);
    unlisted.insert(unlisted.end(), listed.begin(), listed.end());
    return unlisted;
}

// ---- runtime ----

void init(const std::string& game_root, const std::string& user_root, bool enabled) {
    reset();
    g_inited = true;
    g_no_mods = !enabled;
    if (!game_root.empty()) {
        fs::path game(game_root);
        g_content_base = path_str(game / "content");
        fs::path aoc = aoc_content_root(game / "aoc");
        if (!aoc.empty()) g_aoc_base = path_str(aoc);
        uint64_t base = parse_title(game_root);
        if (base != 0) {
            g_title = (base & 0xFFFFFFFFULL) | 0x0005000C00000000ULL;
            g_title_from_meta = true;
        }
    }
    fs::path user = user_root.empty() ? fs::path() : fs::path(user_root);
    fs::path user_aoc = user.empty() ? fs::path() : aoc_content_root(user / "aoc");
    size_t user_aoc_files = 0, user_aoc_replaces = 0;
    if (!user_aoc.empty()) user_aoc_files = add_tree(g_aoc, user_aoc, g_aoc_base, kUserDlc, "aoc", user_aoc_replaces);
    if (user_aoc_files)
        LOG("mods: user DLC folder %s (%zu %s, below mods)", path_str(user_aoc).c_str(), user_aoc_files,
            user_aoc_files == 1 ? "file" : "files");

    if (g_no_mods) LOG("mods: off for this run (--no-mods)");
    else if (!user.empty()) load_mods(user);

    uint64_t pack_sz = 0;
    if (is_file(case_path(g_aoc_base, "0010/Pack/AocMainField.pack"), pack_sz)) g_pack = true;
    bool fd = false, fo = false;
    const Node* pk = walk(g_aoc, "0010/Pack/AocMainField.pack", fd, fo);
    if (pk && fd) g_pack = true;
    tally(g_content);
    tally(g_aoc);
    collect_owned(g_aoc);
    collect_owned_base(g_aoc_base);
    if (g_pack) g_owned.insert(0x0010);

    if (!g_mods.empty() || g_files)
        LOG("mods: %zu mods loaded, %zu overlay files, %zu replace a game file, %zu new", g_mods.size(), g_files, g_replacing,
            g_added);
    char mount[40];
    snprintf(mount, sizeof mount, "/vol/aoc%016llx", static_cast<unsigned long long>(g_title));
    if (g_pack) {
        LOG("DLC: installed as %s (AocMainField.pack found%s)", mount, g_title_from_meta ? "" : ", title id assumed USA");
    } else if (g_aoc.kids.empty() && !base_nonempty(g_aoc_base)) {
        LOG("DLC: not installed");
    } else {
        LOG("DLC: not installed. Add-on files were found, but AocMainField.pack was not, so the game is told the DLC is missing");
    }
}

void log_shutdown() {
    if (!g_inited) return;
    static bool once = false;
    if (once) return;
    once = true;
    if (!g_files) return;
    LOG("mods: %zu distinct overlay files were opened this run", opened_files());
}

Query query(const std::string& guest) {
    Query q;
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return q;
    // No content mods: leave /vol/content to the plain host path, exactly as without mod support.
    if (vol.id == GuestVol::Id::Content && g_content.kids.empty()) return q;
    bool file = false, occluded = false;
    const Node* node = walk(tree_for(vol.id), vol.rel, file, occluded);
    if (occluded) {
        q.kind = Query::Kind::Missing;
        return q;
    }
    if (node && file) {
        uint64_t sz = 0;
        if (is_file(node->host, sz)) {
            q.kind = Query::Kind::File;
            q.host = node->host;
            q.overlay = true;
            q.size = sz;
            return q;
        }
    }
    if (node && !file) {
        q.kind = Query::Kind::Dir;
        q.overlay = true;
        q.host = vol.rel.empty() ? base_for(vol.id) : case_path(base_for(vol.id), vol.rel);
        if (!is_dir(q.host)) q.host.clear();
        return q;
    }
    std::string host = case_path(base_for(vol.id), vol.rel);
    uint64_t sz = 0;
    if (is_file(host, sz)) {
        q.kind = Query::Kind::File;
        q.host = host;
        q.size = sz;
        return q;
    }
    if (is_dir(host) || (vol.rel.empty() && is_dir(base_for(vol.id)))) {
        q.kind = Query::Kind::Dir;
        q.host = host.empty() && vol.rel.empty() ? base_for(vol.id) : host;
        return q;
    }
    q.kind = Query::Kind::Missing;
    return q;
}

ListStatus list_dir(const std::string& guest, std::vector<DirItem>& out) {
    out.clear();
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return ListStatus::NotVolume;
    if (vol.id == GuestVol::Id::Content && g_content.kids.empty()) return ListStatus::NotVolume;
    bool file = false, occluded = false;
    const Node* node = walk(tree_for(vol.id), vol.rel, file, occluded);
    if (occluded) return ListStatus::Missing;
    if (node && file) return ListStatus::NotDir;
    std::string base_dir = vol.rel.empty() ? base_for(vol.id) : case_path(base_for(vol.id), vol.rel);
    bool base_is_dir = is_dir(base_dir);
    if (!base_is_dir) {
        uint64_t sz = 0;
        if (is_file(base_dir, sz) && !(node && !file)) return ListStatus::NotDir;
    }
    bool overlay_dir = node && !file;
    if (!base_is_dir && !overlay_dir) return ListStatus::Missing;

    std::map<std::string, DirItem> merged;
    if (base_is_dir) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(base_dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = path_str(it->path().filename());
            if (name.empty() || name[0] == '.') continue;
            DirItem item;
            item.name = name;
            std::string host = path_str(it->path());
            uint64_t sz = 0;
            if (is_file(host, sz)) {
                item.size = sz;
            } else if (is_dir(host)) {
                item.dir = true;
            } else {
                continue;
            }
            merged[lower_ascii(name)] = item;
        }
    }
    if (overlay_dir) {
        for (const auto& [key, child] : node->kids) {
            DirItem item;
            item.name = child.name.empty() ? key : child.name;
            item.overlay = true;
            if (child.file) {
                uint64_t sz = 0;
                if (!is_file(child.host, sz)) continue;
                item.size = sz;
            } else {
                item.dir = true;
            }
            merged[key] = item;
        }
    }
    out.reserve(merged.size());
    for (auto& [_, item] : merged) out.push_back(std::move(item));  // std::map keeps them sorted by lowercase name
    return ListStatus::Ok;
}

bool read_only_volume(const std::string& guest) {
    GuestVol vol = parse_guest(guest);
    return vol.id == GuestVol::Id::Content || vol.id == GuestVol::Id::Aoc || vol.id == GuestVol::Id::Code ||
           vol.id == GuestVol::Id::Meta;
}

void note_opened(const std::string& guest) {
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return;
    std::string key = (vol.id == GuestVol::Id::Aoc ? "aoc:" : "content:") + lower_ascii(vol.rel);
    std::lock_guard<std::mutex> lock(g_open_mu);
    g_opened.insert(key);
}

bool dlc_ready() { return g_pack; }

uint64_t aoc_title_id() { return g_title; }

std::string aoc_mount_path() {
    char buf[40];
    snprintf(buf, sizeof buf, "/vol/aoc%016llx", static_cast<unsigned long long>(g_title));
    return buf;
}

bool owns_content(uint16_t id) { return g_pack && g_owned.count(id) != 0; }

bool disabled_by_flag() { return g_no_mods; }
const std::vector<ModInfo>& loaded() { return g_mods; }
size_t conflicts() { return g_conflicts.size(); }
size_t index_files() { return g_files; }
size_t replacing_files() { return g_replacing; }
size_t added_files() { return g_added; }
size_t opened_files() {
    std::lock_guard<std::mutex> lock(g_open_mu);
    return g_opened.size();
}

std::string summary() {
    if (g_no_mods) return "Off for this run (--no-mods)";
    if (g_mods.empty()) return "None loaded";
    std::string s = std::to_string(g_mods.size()) + ": ";
    for (size_t i = g_mods.size(); i-- > 0;) {  // highest priority first
        if (i + 1 != g_mods.size()) s += ", ";
        s += g_mods[i].name;
    }
    if (s.size() > 40) s = s.substr(0, 37) + "...";
    return s;
}

}  // namespace mods
