// pixel-aspect@1 (PC5 H-f): the pages' pixel aspect ratio, its menu's ratios and its op (core/pixel_aspect.hpp).

#include "core/pixel_aspect.hpp"

#include <array>
#include <cmath>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// (PhotoCraft v0.2.0's view_cmds.rs PIXEL_ASPECTS and pixel_aspect_ratio — MIT —, in Photoshop's order and words)
constexpr std::array<PixelAspectPreset, 8> kPresets{{
    {"square", "正方形", 1.0},
    {"d1DvNtsc", "D1/DV NTSC (0.91)", 0.91},
    {"d1DvPal", "D1/DV PAL (1.09)", 1.09},
    {"d1DvNtscWidescreen", "D1/DV NTSC ワイドスクリーン (1.21)", 1.21},
    {"hdv1080", "HDV 1080/DVCPRO HD 720 (1.33)", 1.33},
    {"d1DvPalWidescreen", "D1/DV PAL ワイドスクリーン (1.46)", 1.46},
    {"dvcproHd1080", "DVCPRO HD 1080 (1.5)", 1.5},
    {"anamorphic2To1", "アナモルフィック 2:1 (2)", 2.0},
}};

std::string ratio_words(double ratio) { return py_repr(Json(ratio)); }

}  // namespace

std::span<const PixelAspectPreset> pixel_aspect_presets() { return kPresets; }

std::optional<PixelAspectPreset> pixel_aspect_preset_of(double ratio) {
    for (const PixelAspectPreset& preset : kPresets) {
        if (std::abs(preset.ratio - ratio) < 1e-9) return preset;
    }
    return std::nullopt;
}

bool square_pixels(double ratio) { return std::abs(ratio - 1.0) < 1e-12; }

bool pixel_aspect_allowed(double ratio) { return std::isfinite(ratio) && ratio >= kPixelAspectMin && ratio <= kPixelAspectMax; }

double pixel_aspect_from_json(const Json& value) {
    if (!value.is_number() || value.is_boolean()) throw Error("value", "a page's pixel_aspect is a number (0.1..10)");
    const double ratio = value.get<double>();
    if (!pixel_aspect_allowed(ratio)) throw Error("value", "a page's pixel_aspect is between 0.1 and 10, not " + ratio_words(ratio));
    return ratio;
}

bool uses_pixel_aspect(const Document& doc) {
    for (const PagePtr& page : doc.pages) {
        if (page && !square_pixels(page->pixel_aspect)) return true;
    }
    return false;
}

void validate_pixel_aspect(const Document& doc) {
    for (const PagePtr& page : doc.pages) {
        if (page && !pixel_aspect_allowed(page->pixel_aspect)) {
            throw Error("value", "page " + page->index.repr() + ": the pixel aspect ratio is between 0.1 and 10, not " + ratio_words(page->pixel_aspect));
        }
    }
}

void register_pixel_aspect_ops(OpRegistry& registry) {
    registry.add("set_pixel_aspect", [](OpContext& c) {
        const std::size_t at = require_page(c.doc, c.op);
        const Json* ratio = get(c.op, "ratio");
        const Json* preset = get(c.op, "preset");
        if ((ratio == nullptr) == (preset == nullptr)) throw OpError("give ratio (0.1..10) or preset (one of 表示 → ピクセル縦横比's), not both");
        double value = 1.0;
        if (preset != nullptr) {
            if (!preset->is_string()) throw OpError("preset is the name of a ratio (square, d1DvNtsc, …)");
            const std::string& id = preset->get_ref<const std::string&>();
            bool found = false;
            for (const PixelAspectPreset& p : kPresets) {
                if (p.id == id) {
                    value = p.ratio;
                    found = true;
                }
            }
            if (!found) {
                std::string names;
                for (const PixelAspectPreset& p : kPresets) names += (names.empty() ? "" : ", ") + std::string(p.id);
                throw OpError("no pixel aspect ratio " + py_repr(*preset) + " (" + names + ")");
            }
        } else {
            if (!ratio->is_number() || ratio->is_boolean()) throw OpError("ratio is a number (the width of a pixel over its height, 0.1..10)");
            value = ratio->get<double>();
            if (!pixel_aspect_allowed(value)) throw OpError("ratio is between 0.1 and 10, not " + ratio_words(value));
        }
        if (square_pixels(value)) value = 1.0;
        if (c.doc.page(at).pixel_aspect != value) c.doc.edit_page(at).pixel_aspect = value;
        c.report = Json{{"page", c.doc.page(at).index.json()}, {"pixel_aspect", value}};
    });
}

}  // namespace genko::core
