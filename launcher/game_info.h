// Reads what the launcher shows about a game folder: title and version from meta\meta.xml
// and the icon from meta\iconTex.tga. Plain C++, no Win32, so it can be tested anywhere.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gameinfo {

struct Icon {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // top-down rows, 4 bytes per pixel, straight alpha
    bool empty() const { return width <= 0 || height <= 0 || bgra.empty(); }
};

struct Info {
    bool folder_exists = false;
    bool has_rpx = false;          // code\U-King.rpx
    bool has_meta = false;         // meta\meta.xml parsed
    std::string long_name;         // longname_en, lines joined by '\n'
    std::string title_id;          // upper-case hex, 16 chars when present
    uint32_t title_version = 0;
    bool has_title_version = false;
    Icon icon;
};

// The update this build was made for. v1.5.0 is title version 208.
constexpr uint32_t kExpectedTitleVersion = 208;

Info read(const std::filesystem::path& game_folder);

// "v1.5.0" for known BotW title versions, else "v<number>".
std::string version_name(uint32_t title_version);
// "USA", "Europe", "Japan" from the BotW title id, else empty.
std::string region_name(const std::string& title_id);

// Exposed for tests.
bool parse_meta(const std::string& xml, Info& out);
bool decode_tga(const std::vector<uint8_t>& file, Icon& out);

}  // namespace gameinfo
