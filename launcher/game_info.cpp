#include "game_info.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gameinfo {

namespace {

namespace fs = std::filesystem;

bool read_file(const fs::path& p, std::vector<uint8_t>& out, size_t cap) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    if (n <= 0 || static_cast<uint64_t>(n) > cap) return false;
    in.seekg(0);
    out.resize(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(in);
}

// Text of the first <tag ...>text</tag>, without decoding.
bool element(const std::string& xml, const std::string& tag, std::string& out) {
    std::string open = "<" + tag;
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        size_t after = pos + open.size();
        if (after < xml.size() && (xml[after] == '>' || std::isspace(static_cast<unsigned char>(xml[after])) || xml[after] == '/')) break;
        pos = after;
    }
    if (pos == std::string::npos) return false;
    size_t gt = xml.find('>', pos);
    if (gt == std::string::npos) return false;
    if (gt > 0 && xml[gt - 1] == '/') {
        out.clear();
        return true;
    }
    size_t end = xml.find("</" + tag, gt + 1);
    if (end == std::string::npos) return false;
    out = xml.substr(gt + 1, end - gt - 1);
    return true;
}

void put_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string decode_entities(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '&') {
            out += in[i];
            continue;
        }
        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += in[i];
            continue;
        }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? static_cast<uint32_t>(std::strtoul(ent.c_str() + 2, nullptr, 16))
                                                                              : static_cast<uint32_t>(std::strtoul(ent.c_str() + 1, nullptr, 10));
            put_utf8(out, cp);
        } else {
            out += in.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct Sha1Ctx {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t total = 0;
    uint8_t buf[64] = {};
    size_t buf_len = 0;

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(p[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            size_t take = std::min(len, sizeof(buf) - buf_len);
            std::memcpy(buf + buf_len, data, take);
            buf_len += take;
            data += take;
            len -= take;
            if (buf_len == sizeof(buf)) {
                block(buf);
                buf_len = 0;
            }
        }
    }

    std::string final_hex() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        uint8_t len_be[8];
        for (int i = 0; i < 8; i++) len_be[7 - i] = static_cast<uint8_t>(bits >> (i * 8));
        update(len_be, 8);
        char hex[41];
        std::snprintf(hex, sizeof hex, "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
        return hex;
    }
};

bool has_dlc_pack(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "aoc" / "0010" / "Pack" / "AocMainField.pack", ec)) return true;
    if (fs::is_regular_file(dir / "aoc" / "content" / "0010" / "Pack" / "AocMainField.pack", ec)) return true;
    if (fs::is_regular_file(dir / "content" / "0010" / "Pack" / "AocMainField.pack", ec)) return true;
    if (fs::is_regular_file(dir / "0010" / "Pack" / "AocMainField.pack", ec)) return true;
    return false;
}

fs::path resolve_dlc_content_src(const fs::path& dlc_dir) {
    std::error_code ec;
    if (fs::is_directory(dlc_dir / "content" / "0010", ec)) return dlc_dir / "content";
    if (fs::is_directory(dlc_dir / "0010", ec)) return dlc_dir;
    if (fs::is_directory(dlc_dir / "aoc" / "0010", ec)) return dlc_dir / "aoc";
    if (fs::is_directory(dlc_dir / "aoc" / "content" / "0010", ec)) return dlc_dir / "aoc" / "content";
    if (fs::is_directory(dlc_dir / "content", ec)) return dlc_dir / "content";
    return dlc_dir;
}

}  // namespace

std::string sha1_bytes(const void* data, size_t len) {
    Sha1Ctx ctx;
    if (data && len > 0) ctx.update(static_cast<const uint8_t*>(data), len);
    return ctx.final_hex();
}

std::string sha1_file(const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) return {};
    Sha1Ctx ctx;
    std::vector<uint8_t> buf(64 * 1024);
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got > 0) ctx.update(buf.data(), static_cast<size_t>(got));
    }
    if (in.bad()) return {};
    return ctx.final_hex();
}

bool parse_meta(const std::string& xml, Info& out) {
    std::string v;
    bool any = false;
    if (element(xml, "longname_en", v)) {
        std::string text = decode_entities(v);
        std::string joined, line;
        for (size_t i = 0; i <= text.size(); i++) {
            if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
                line = trim(line);
                if (!line.empty()) {
                    if (!joined.empty()) joined += '\n';
                    joined += line;
                }
                line.clear();
            } else {
                line += text[i];
            }
        }
        out.long_name = joined;
        any = true;
    }
    if (element(xml, "title_id", v)) {
        std::string id = trim(v);
        for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out.title_id = id;
        any = true;
    }
    if (element(xml, "title_version", v)) {
        std::string t = trim(v);
        if (!t.empty() && std::isdigit(static_cast<unsigned char>(t[0]))) {
            out.title_version = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
            out.has_title_version = true;
            any = true;
        }
    }
    return any;
}

bool decode_tga(const std::vector<uint8_t>& f, Icon& out) {
    if (f.size() < 18) return false;
    int id_len = f[0];
    int cmap_type = f[1];
    int type = f[2];
    int w = f[12] | (f[13] << 8);
    int h = f[14] | (f[15] << 8);
    int bpp = f[16];
    int desc = f[17];
    if (cmap_type != 0 || (type != 2 && type != 10)) return false;
    if (bpp != 24 && bpp != 32) return false;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    int bytes = bpp / 8;
    size_t pos = 18 + static_cast<size_t>(id_len);
    size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> px(count * 4);
    auto put = [&](size_t i, const uint8_t* s) {
        px[i * 4 + 0] = s[0];
        px[i * 4 + 1] = s[1];
        px[i * 4 + 2] = s[2];
        px[i * 4 + 3] = bytes == 4 ? s[3] : 255;
    };
    if (type == 2) {
        if (pos + count * static_cast<size_t>(bytes) > f.size()) return false;
        for (size_t i = 0; i < count; i++) put(i, &f[pos + i * static_cast<size_t>(bytes)]);
    } else {
        size_t i = 0;
        while (i < count) {
            if (pos >= f.size()) return false;
            uint8_t head = f[pos++];
            size_t n = static_cast<size_t>(head & 0x7F) + 1;
            if (i + n > count) return false;
            if (head & 0x80) {
                if (pos + static_cast<size_t>(bytes) > f.size()) return false;
                for (size_t k = 0; k < n; k++) put(i + k, &f[pos]);
                pos += static_cast<size_t>(bytes);
            } else {
                if (pos + n * static_cast<size_t>(bytes) > f.size()) return false;
                for (size_t k = 0; k < n; k++) put(i + k, &f[pos + k * static_cast<size_t>(bytes)]);
                pos += n * static_cast<size_t>(bytes);
            }
            i += n;
        }
    }
    bool top_down = (desc & 0x20) != 0;
    // Some dumps store an alpha channel that is all zero. Treat that as opaque.
    if (bytes == 4) {
        bool any_alpha = false;
        for (size_t i = 0; i < count && !any_alpha; i++) any_alpha = px[i * 4 + 3] != 0;
        if (!any_alpha)
            for (size_t i = 0; i < count; i++) px[i * 4 + 3] = 255;
    }
    out.width = w;
    out.height = h;
    out.bgra.resize(px.size());
    size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; y++) {
        int src = top_down ? y : h - 1 - y;
        std::copy_n(px.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(src) * row), row,
                    out.bgra.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * row));
    }
    return true;
}

Info read(const fs::path& game) {
    Info info;
    std::error_code ec;
    if (game.empty() || !fs::is_directory(game, ec)) return info;
    info.folder_exists = true;
    info.has_rpx = fs::is_regular_file(game / "code" / "U-King.rpx", ec);
    info.has_content = fs::is_directory(game / "content", ec);
    info.has_dlc = has_dlc_pack(game);
    std::vector<uint8_t> data;
    if (read_file(game / "meta" / "meta.xml", data, 1 << 20)) {
        std::string xml(data.begin(), data.end());
        info.has_meta = parse_meta(xml, info);
    }
    data.clear();
    if (read_file(game / "meta" / "iconTex.tga", data, 8 << 20)) decode_tga(data, info.icon);
    return info;
}

std::string version_name(uint32_t v) {
    switch (v) {
        case 0: return "v1.0.0";
        case 80: return "DLC v3.0";
        case 208: return "v1.5.0";
        default: return "v" + std::to_string(v);
    }
}

std::string region_name(const std::string& id) {
    if (id.size() < 8) return {};
    std::string low = id.substr(id.size() - 8);
    if (low == "101C9400") return "USA";
    if (low == "101C9500") return "Europe";
    if (low == "101C9300") return "Japan";
    return {};
}

RpxVerify verify_rpx(const fs::path& game_or_rpx) {
    RpxVerify v;
    std::error_code ec;
    fs::path rpx_path = game_or_rpx;
    fs::path game_dir;
    if (fs::is_directory(game_or_rpx, ec)) {
        game_dir = game_or_rpx;
        rpx_path = game_or_rpx / "code" / "U-King.rpx";
    } else if (rpx_path.parent_path().filename() == "code") {
        game_dir = rpx_path.parent_path().parent_path();
    }
    if (!fs::is_regular_file(rpx_path, ec)) {
        v.summary = "code\\U-King.rpx not found.";
        return v;
    }
    v.exists = true;
    v.size = static_cast<uint64_t>(fs::file_size(rpx_path, ec));
    v.sha1 = sha1_file(rpx_path);
    v.hash_match = (v.sha1 == kUsaV208RpxSha1);

    if (!game_dir.empty()) {
        Info info = read(game_dir);
        v.region = region_name(info.title_id);
        v.title_version = info.title_version;
        v.has_title_version = info.has_title_version;
        v.meta_usa_v208 = (v.region == "USA" && info.has_title_version && info.title_version == kExpectedTitleVersion);
    }

    if (v.hash_match) {
        v.summary = "Verified USA v1.5.0 (v208) U-King.rpx (SHA-1: " + v.sha1.substr(0, 12) + "...)";
    } else if (!v.region.empty() && v.region != "USA") {
        v.summary = v.region + " dump detected. BotW-Native requires the USA v1.5.0 (00050000101C9400, v208) dump.";
    } else if (v.has_title_version && v.title_version < kExpectedTitleVersion) {
        v.summary = "Unpatched base game (" + version_name(v.title_version) +
                    ") detected. Select your v1.5.0 (v208) Update folder to merge.";
    } else if (v.meta_usa_v208) {
        v.summary = "USA v1.5.0 (v208) meta.xml detected (U-King.rpx SHA-1: " +
                    (v.sha1.empty() ? std::string("none") : v.sha1.substr(0, 12) + "...") + ")";
    } else {
        v.summary = "U-King.rpx found (SHA-1: " + (v.sha1.empty() ? std::string("none") : v.sha1.substr(0, 12) + "...") + ")";
    }
    return v;
}

std::string DumpLayout::summary() const {
    if (!merged_game.empty()) {
        std::string s = "Ready: Merged USA v1.5.0 (v208) game folder";
        if (!dlc_folder.empty() || base_info.has_dlc) s += " + DLC (v80)";
        return s;
    }
    if (!base_folder.empty() && !update_folder.empty()) {
        std::string s = "Ready to merge: Base Game + Update v1.5.0 (v208)";
        if (!dlc_folder.empty()) s += " + DLC (v80)";
        return s;
    }
    if (!base_folder.empty()) {
        return "Base game found, but Update v1.5.0 (v208) folder is still needed.";
    }
    if (!update_folder.empty()) {
        return "Update v1.5.0 (v208) found, but Base Game folder is still needed.";
    }
    return "No Breath of the Wild Wii U dump folders found yet.";
}

DumpLayout discover_dumps(const fs::path& root) {
    DumpLayout out;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return out;

    std::vector<fs::path> candidates;
    auto should_skip_dir = [](const std::string& name_lower) {
        return name_lower == "content" || name_lower == "code" || name_lower == "meta" ||
               name_lower == "aoc" || name_lower == "0010" || name_lower == "shadercache" ||
               name_lower == ".git" || name_lower == "generated" || name_lower == "build";
    };

    auto scan_dir = [&](auto& self, const fs::path& dir, int depth) -> void {
        std::error_code dec;
        bool is_cand = fs::is_regular_file(dir / "meta" / "meta.xml", dec) ||
                       fs::is_regular_file(dir / "code" / "U-King.rpx", dec) ||
                       fs::is_regular_file(dir / "content" / "0010" / "Pack" / "AocMainField.pack", dec) ||
                       fs::is_regular_file(dir / "0010" / "Pack" / "AocMainField.pack", dec);
        if (is_cand) candidates.push_back(dir);
        if (depth >= 3) return;
        for (fs::directory_iterator it(dir, dec); !dec && it != fs::directory_iterator(); it.increment(dec)) {
            std::error_code sec;
            if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
            std::string fname;
            try {
                fname = lower_ascii(it->path().filename().string());
            } catch (...) {
                continue;
            }
            if (should_skip_dir(fname)) continue;
            self(self, it->path(), depth + 1);
        }
    };

    scan_dir(scan_dir, root, 0);
    // If `root` itself was a single base/update folder, also scan its parent directory (1 level) for sibling update/DLC dumps.
    if (candidates.size() == 1 && candidates[0] == root && root.has_parent_path()) {
        fs::path parent = root.parent_path();
        for (fs::directory_iterator it(parent, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (!it->is_directory(sec) || it->path() == root) continue;
            bool is_cand = fs::is_regular_file(it->path() / "meta" / "meta.xml", sec) ||
                           fs::is_regular_file(it->path() / "code" / "U-King.rpx", sec) ||
                           fs::is_regular_file(it->path() / "content" / "0010" / "Pack" / "AocMainField.pack", sec);
            if (is_cand) candidates.push_back(it->path());
        }
    }

    for (const fs::path& cand : candidates) {
        Info info = read(cand);
        std::string tid = info.title_id;
        bool is_dlc = (tid.rfind("0005000C", 0) == 0) ||
                      (!info.has_rpx && (fs::is_regular_file(cand / "content" / "0010" / "Pack" / "AocMainField.pack", ec) ||
                                         fs::is_regular_file(cand / "0010" / "Pack" / "AocMainField.pack", ec)));
        bool is_update = (tid.rfind("0005000E", 0) == 0);
        bool is_base = (tid.rfind("00050000", 0) == 0);

        if (is_dlc) {
            if (out.dlc_folder.empty()) out.dlc_folder = cand;
            continue;
        }
        if (is_update) {
            if (out.update_folder.empty()) {
                out.update_folder = cand;
                out.update_info = info;
            }
            continue;
        }
        if (is_base || info.has_rpx) {
            if (info.has_title_version && info.title_version == kExpectedTitleVersion && out.base_folder.empty()) {
                out.base_folder = cand;
                out.base_info = info;
            } else if (out.base_folder.empty()) {
                out.base_folder = cand;
                out.base_info = info;
            } else if (info.has_title_version && info.title_version == kExpectedTitleVersion && out.update_folder.empty()) {
                out.update_folder = cand;
                out.update_info = info;
            }
        }
    }

    if (!out.base_folder.empty()) {
        if (out.base_info.has_dlc && out.dlc_folder.empty()) {
            out.dlc_folder = out.base_folder / "aoc";
        }
        bool base_is_v208 = out.base_info.has_title_version && out.base_info.title_version == kExpectedTitleVersion;
        if (base_is_v208 && out.update_folder.empty()) {
            out.merged_game = out.base_folder;
            out.needs_merge = false;
        } else if (!out.update_folder.empty() && out.update_folder != out.base_folder) {
            out.needs_merge = true;
        }
    }
    return out;
}

bool merge_dumps(const fs::path& base_dir,
                 const fs::path& update_dir,
                 const fs::path& dlc_dir,
                 const fs::path& target_game_dir,
                 std::string& err,
                 MergeProgressFn progress) {
    std::error_code ec;
    if (base_dir.empty() || !fs::is_directory(base_dir, ec)) {
        err = "Base game folder does not exist.";
        return false;
    }
    if (update_dir.empty() || !fs::is_directory(update_dir, ec)) {
        err = "Update v1.5.0 (v208) folder does not exist.";
        return false;
    }
    if (target_game_dir.empty()) {
        err = "Target merged game folder path is empty.";
        return false;
    }

    struct CopyItem {
        fs::path src;
        fs::path dst;
        std::string phase;
    };
    std::vector<CopyItem> items;

    auto collect_tree = [&](const fs::path& src_root, const fs::path& dst_root, const std::string& phase) {
        std::error_code tec;
        if (!fs::is_directory(src_root, tec)) return;
        for (fs::recursive_directory_iterator it(src_root, fs::directory_options::skip_permission_denied, tec);
             !tec && it != fs::recursive_directory_iterator(); it.increment(tec)) {
            std::error_code sec;
            if (!it->is_regular_file(sec)) continue;
            fs::path rel = it->path().lexically_relative(src_root);
            if (rel.empty()) continue;
            items.push_back({it->path(), dst_root / rel, phase});
        }
    };

    for (const char* sub : {"code", "content", "meta"}) {
        collect_tree(base_dir / sub, target_game_dir / sub, "Linking Base Game files");
    }
    for (const char* sub : {"code", "content", "meta"}) {
        collect_tree(update_dir / sub, target_game_dir / sub, "Applying Update v1.5.0 (v208)");
    }
    if (!dlc_dir.empty() && fs::is_directory(dlc_dir, ec)) {
        fs::path dlc_src = resolve_dlc_content_src(dlc_dir);
        collect_tree(dlc_src, target_game_dir / "aoc", "Linking DLC (v80) files");
    }

    if (items.empty()) {
        err = "No game files found in Base or Update folders.";
        return false;
    }

    int total = static_cast<int>(items.size());
    for (int i = 0; i < total; i++) {
        const CopyItem& item = items[i];
        if (progress && (i == 0 || i + 1 == total || (i % 25) == 0)) {
            progress(i + 1, total, item.phase);
        }
        fs::create_directories(item.dst.parent_path(), ec);
        if (ec) {
            err = "Could not create directory " + item.dst.parent_path().string();
            return false;
        }
        // Always unlink destination first so overlaying Update over Base never mutates Base via hardlink.
        fs::remove(item.dst, ec);
        ec.clear();
        fs::create_hard_link(item.src, item.dst, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(item.src, item.dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                err = "Could not link or copy " + item.src.filename().string() + ": " + ec.message();
                return false;
            }
        }
    }
    if (progress) progress(total, total, "Merged game folder ready");
    return true;
}

}  // namespace gameinfo
