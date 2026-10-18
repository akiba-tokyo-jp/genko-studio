#include "formats/psd_adjust.hpp"

#include <algorithm>
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
                                               std::vector<std::string>& notes, const core::Document* doc) {
    if (layer.kind != core::LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object()) return std::nullopt;
    const std::optional<render::psd::AdjustBlock> block = render::psd::write_adjustment(*layer.adjust, page, notes, doc);
    if (!block) return std::nullopt;
    PsdAdjustLayer out;
    out.name = !layer.title.empty() ? layer.title : std::string("色調補正");
    out.opacity = layer.opacity;
    out.blend = core::native_of(*layer.adjust).blend;
    out.clip = layer.clip;
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
        out.mask = render::read_png(*layer.mask->png).convert("L").resize(size);
    const auto tagged = [&out](const std::string& key, std::string payload) {
        payload.append((4 - payload.size() % 4) % 4, '\0');
        out.blocks += "8BIM" + key;
        put_u32(out.blocks, static_cast<std::uint32_t>(payload.size()));
        out.blocks += payload;
    };
    tagged(block->key, block->payload);
    if (!block->companion_key.empty()) tagged(block->companion_key, block->companion);  // (brit's CgEd)
    return out;
}

std::vector<std::string> psd_adjust_notes(const core::Document& doc) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const core::Page& page = doc.page(i);
        for (const core::Layer& layer : page.layers) {
            if (layer.kind != core::LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object() || !layer.visible) continue;
            std::vector<std::string> notes;
            try {
                (void)render::psd::write_adjustment(*layer.adjust, page, notes, &doc);
            } catch (const std::exception&) {  // (the export itself says what it cannot write)
                continue;
            }
            for (std::string& note : notes) {
                if (std::find(out.begin(), out.end(), note) == out.end()) out.push_back(std::move(note));
            }
        }
    }
    return out;
}

}  // namespace genko::formats
