#include "game_info.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace gameinfo {

namespace {

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out, size_t cap) {
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

}  // namespace

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

Info read(const std::filesystem::path& game) {
    Info info;
    std::error_code ec;
    if (game.empty() || !std::filesystem::is_directory(game, ec)) return info;
    info.folder_exists = true;
    info.has_rpx = std::filesystem::is_regular_file(game / "code" / "U-King.rpx", ec);
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

}  // namespace gameinfo
