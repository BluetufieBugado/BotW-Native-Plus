// BotWRecomp: built-in resolution scaling (botw_texture_rules.inc) plus enabled mods'
// [TextureRedefine] rules and custom GLSL shader overrides.
// Built-in resolution scaling is placed first in GetActiveGraphicPacks() followed by enabled
// mods in load order so mod [TextureRedefine] rules take priority when matching the same texture.
#include <cmath>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "../src/coreinit/mods.h"

extern float g_render_scale;  // runtime: --scale

namespace {
struct Row {
    int width, height, depth;
    std::vector<unsigned int> formats, formats_excluded;
    std::vector<int> tilemodes, tilemodes_excluded;
    int overwrite_format;
    int ow;
    double ow_add;
    int oh;
    double oh_add;
};
const Row kRows[] = {
#include "botw_texture_rules.inc"
};

void append_expanded_texture_rules(const mods::TextureRedefineRule& src,
                                   std::vector<GraphicPack2::TextureRule>& out) {
    const std::vector<int32_t> widths = src.width.empty() ? std::vector<int32_t>{-1} : src.width;
    const std::vector<int32_t> heights = src.height.empty() ? std::vector<int32_t>{-1} : src.height;
    const std::vector<int32_t> depths = src.depth.empty() ? std::vector<int32_t>{-1} : src.depth;

    std::vector<unsigned int> fmt_white;
    fmt_white.reserve(src.formats.size());
    for (int32_t f : src.formats) fmt_white.push_back(static_cast<unsigned int>(f));

    std::vector<unsigned int> fmt_black;
    fmt_black.reserve(src.formats_excluded.size());
    for (int32_t f : src.formats_excluded) fmt_black.push_back(static_cast<unsigned int>(f));

    std::vector<int> tm_white(src.tile_modes.begin(), src.tile_modes.end());
    std::vector<int> tm_black(src.tile_modes_excluded.begin(), src.tile_modes_excluded.end());

    for (int32_t w : widths) {
        for (int32_t h : heights) {
            for (int32_t d : depths) {
                GraphicPack2::TextureRule rule;
                auto& f = rule.filter_settings;
                f.width = w;
                f.height = h;
                f.depth = d;
                f.format_whitelist = fmt_white;
                f.format_blacklist = fmt_black;
                f.tilemode_whitelist = tm_white;
                f.tilemode_blacklist = tm_black;

                auto& o = rule.overwrite_settings;
                o.width = src.overwrite_width;
                o.height = src.overwrite_height;
                o.depth = src.overwrite_depth;
                o.format = src.overwrite_format;
                if (src.has_lod_bias) {
                    o.lod_bias = static_cast<int>(std::llround(src.overwrite_lod_bias * 64.0f));
                }
                o.anistropic_value = src.overwrite_anisotropy;
                out.push_back(std::move(rule));
            }
        }
    }
}

std::vector<std::shared_ptr<GraphicPack2>> build_active_packs() {
    std::vector<std::shared_ptr<GraphicPack2>> v;
    if (g_render_scale != 1.0f) {
        auto gp = std::make_shared<GraphicPack2>();
        gp->m_name = "BotWRecomp resolution";
        for (const Row& r : kRows) {
            GraphicPack2::TextureRule rule;
            auto& f = rule.filter_settings;
            f.width = r.width;
            f.height = r.height;
            f.depth = r.depth;
            f.format_whitelist = r.formats;
            f.format_blacklist = r.formats_excluded;
            f.tilemode_whitelist = r.tilemodes;
            f.tilemode_blacklist = r.tilemodes_excluded;
            auto& o = rule.overwrite_settings;
            o.width = static_cast<int>(g_render_scale * r.ow + r.ow_add);
            o.height = static_cast<int>(g_render_scale * r.oh + r.oh_add);
            o.format = r.overwrite_format;
            gp->m_rules.push_back(std::move(rule));
        }
        v.push_back(std::move(gp));
    }

    const auto& enabled = mods::enabled_mods();
    for (const auto& mod : enabled) {
        if (mod.texture_rules.empty()) continue;
        auto gp = std::make_shared<GraphicPack2>();
        gp->m_name = mod.name;
        for (const auto& tr : mod.texture_rules) {
            append_expanded_texture_rules(tr, gp->m_rules);
        }
        if (!gp->m_rules.empty()) {
            v.push_back(std::move(gp));
        }
    }
    return v;
}
}  // namespace

const std::vector<std::shared_ptr<GraphicPack2>>& GraphicPack2::GetActiveGraphicPacks() {
    thread_local uint64_t t_cached_version = 0;
    thread_local float t_cached_scale = -1.0f;
    thread_local std::vector<std::shared_ptr<GraphicPack2>> t_packs;

    const uint64_t cur_ver = mods::graphic_pack_version();
    if (t_cached_version != cur_ver || t_cached_scale != g_render_scale) {
        t_packs = build_active_packs();
        t_cached_version = cur_ver;
        t_cached_scale = g_render_scale;
    }
    return t_packs;
}

const std::string* GraphicPack2::FindCustomShaderSource(uint64_t baseHash, uint64_t auxHash,
                                                        GP_SHADER_TYPE type, bool /*isVulkan*/,
                                                        bool /*isMetal*/) {
    thread_local std::string t_shader_src;
    mods::ShaderStage stage = mods::ShaderStage::Pixel;
    if (type == GP_SHADER_TYPE::VERTEX) stage = mods::ShaderStage::Vertex;
    else if (type == GP_SHADER_TYPE::GEOMETRY) stage = mods::ShaderStage::Geometry;

    auto found = mods::find_custom_shader(baseHash, auxHash, stage);
    if (!found) return nullptr;
    t_shader_src = std::move(*found);
    return &t_shader_src;
}
