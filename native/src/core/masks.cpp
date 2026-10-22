#include "core/masks.hpp"

#include <cmath>
#include <string>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", why); }

}  // namespace

bool mask_has_settings(const Mask& mask) { return mask.linked || mask.density != 1.0 || mask.feather_mm != 0.0; }

void write_mask_settings(const Mask& mask, Json& out) {
    if (mask.linked) out["linked"] = true;
    if (mask.density != 1.0) out["density"] = py_round(mask.density, 4);
    if (mask.feather_mm != 0.0) out["feather_mm"] = py_round(mask.feather_mm, 4);
}

void read_mask_settings(const Json& raw, Mask& mask) {
    if (!raw.is_object()) return;
    if (const auto it = raw.find("linked"); it != raw.end()) {
        if (!it->is_boolean()) bad("a mask's linked must be true or false");
        mask.linked = it->get<bool>();
    }
    const auto number = [&](const char* key, double lo, double hi) -> std::optional<double> {
        const auto it = raw.find(key);
        if (it == raw.end()) return std::nullopt;
        if (!it->is_number() || it->is_boolean()) bad(std::string("a mask's ") + key + " must be a number");
        const double v = it->get<double>();
        if (!std::isfinite(v) || v < lo || v > hi) bad(std::string("a mask's ") + key + " must be " + py_repr(Json(lo)) + ".." + py_repr(Json(hi)));
        return v;
    };
    if (const auto v = number("density", 0.0, 1.0)) mask.density = *v;
    if (const auto v = number("feather_mm", 0.0, kMaskFeatherMax)) mask.feather_mm = *v;
}

// (both look at the book's pages and the pages of the contents it writes: smart::written_pages)
bool uses_masks(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        const Page& page = *written.page;
        for (const Layer& layer : page.layers)
            if (layer.mask && mask_has_settings(*layer.mask)) return true;
        for (const Channel& channel : page.channels)
            if (channel.quick_mask) return true;
    }
    return false;
}

void validate_masks(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        const Page& page = *written.page;
        const std::string where =
            written.contents.empty() ? "page " + py_str(page.index.json()) + ": " : "the smart object contents " + written.contents + ": ";
        for (const Layer& layer : page.layers) {
            if (!layer.mask) continue;
            const Mask& m = *layer.mask;
            if (!(m.density >= 0.0 && m.density <= 1.0)) bad(where + "the mask of the layer " + layer.id + " has a density outside 0..1");
            if (!(m.feather_mm >= 0.0 && m.feather_mm <= kMaskFeatherMax)) {
                bad(where + "the mask of the layer " + layer.id + " has a feather outside 0.." + py_repr(Json(kMaskFeatherMax)) + " mm");
            }
        }
        int quick = 0;
        for (const Channel& channel : page.channels) {
            if (!channel.quick_mask) continue;
            if (channel.kind != ChannelKind::Alpha) bad(where + "a spot channel cannot be the Quick Mask");
            if (++quick > 1) bad(where + "a page has one Quick Mask at most");
        }
    }
}

const Channel* quick_mask_of(const Page& page) {
    for (const Channel& channel : page.channels)
        if (channel.quick_mask) return &channel;
    return nullptr;
}

}  // namespace genko::core
