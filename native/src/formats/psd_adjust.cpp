#include "formats/psd_adjust.hpp"

#include <cstdint>

#include "core/adjust.hpp"
#include "render/png.hpp"
#include "render/psd_adjust.hpp"

namespace genko::formats {

namespace {

void put_u32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xff);
}

}  // namespace

std::optional<PsdAdjustLayer> psd_adjust_layer(const core::Layer& layer, const core::Page& page, render::Size size,
                                               std::vector<std::string>& notes) {
    if (layer.kind != core::LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object()) return std::nullopt;
    const std::optional<render::psd::AdjustBlock> block = render::psd::write_adjustment(*layer.adjust, page, notes);
    if (!block) return std::nullopt;
    PsdAdjustLayer out;
    out.name = !layer.title.empty() ? layer.title : std::string("色調補正");
    out.opacity = layer.opacity;
    out.blend = core::native_of(*layer.adjust).blend;
    out.clip = layer.clip;
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
        out.mask = render::read_png(*layer.mask->png).convert("L").resize(size);
    std::string payload = block->payload;
    payload.append((4 - payload.size() % 4) % 4, '\0');
    out.blocks = "8BIM" + block->key;
    put_u32(out.blocks, static_cast<std::uint32_t>(payload.size()));
    out.blocks += payload;
    return out;
}

}  // namespace genko::formats
