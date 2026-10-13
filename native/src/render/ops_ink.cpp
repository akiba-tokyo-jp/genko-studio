// color-mode@1: the ops of CMYK and Lab pages (SPEC.md COLOR-02): set_color_mode turns pages into another colour mode
// (their paint layers' pixels converted, nothing else changed), set_inks sets the exact inks of RGB colours.

#include <algorithm>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::InkSpace;
using core::Json;
using core::OpContext;
using core::OpError;
using core::PageColor;

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

std::string text_of(const Json& op, const char* key, std::string fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_string()) throw OpError(std::string(key) + " must be a string");
    return v->get<std::string>();
}

// The pages an op names: its "page" (none, "" or 0: every page), as set_paper reads it.
std::vector<std::size_t> pages_of(const Document& doc, const Json& op, bool& every) {
    const Json* page = core::get(op, "page");
    every = page == nullptr || page->is_null() || *page == Json("") || core::py_equals(*page, Json(0));
    std::vector<std::size_t> out;
    if (every) {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) out.push_back(i);
        return out;
    }
    if (!core::py_big_int_text(*page)) {
        const core::Num index(core::to_int(*page));
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.pages[i]->index == index) out.push_back(i);
        }
    }
    if (out.empty()) throw OpError("page not found");
    return out;
}

// A profile the book holds (on its pages or as its pages' mode), by its ref.
core::Bytes held_profile(const Document& doc, const std::string& ref) {
    if (doc.color && doc.color->profile == ref && doc.color->icc) return doc.color->icc;
    for (const auto& page : doc.pages) {
        if (page->color && page->color->profile == ref && page->color->icc) return page->color->icc;
    }
    return nullptr;
}

// A layer's pixels in the new mode (none: an RGB page; `from` none: the layer is in RGB now).
void convert_layer(core::Layer& layer, const std::optional<PageColor>& from, const std::optional<PageColor>& to, bool gray_on_k,
                   int rgb_depth) {
    if (to) {
        std::optional<core::InkPixels> pixels;
        if (layer.ink_raster) {
            if (!from) throw OpError("the layer " + layer.id + " holds ink pixels on an RGB page");
            pixels = ink::convert(core::decode_ink_raster(*layer.ink_raster), *from, *to);
        } else if (layer.raster_png && !layer.raster_png->empty()) {
            pixels = ink::from_rgb(open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA"), *to, gray_on_k);
        } else if (layer.color_raster) {
            const core::ColorRasterView view(*layer.color_raster);
            std::vector<float> rgba(std::size_t(view.width()) * view.height() * 4);
            for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
                const auto p = view.pixel(i);
                for (unsigned c = 0; c < 4; ++c) rgba[i * 4 + c] = static_cast<float>(p[c]);
            }
            pixels = ink::from_srgb(rgba, view.width(), view.height(), *to, gray_on_k);
        }
        if (!pixels) return;
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*pixels));
        layer.ink_memo = {};
        layer.raster_png = nullptr;
        layer.raster_memo = {};
        layer.raster_relpath.reset();
        layer.color_raster = nullptr;
        return;
    }
    if (!layer.ink_raster || !from) return;  // (an RGB layer on a page that becomes RGB: as it is)
    const core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
    if (rgb_depth == 16) {  // (asked for: precise sRGB pixels, 16 bits)
        const std::vector<float> rgba = ink::to_srgb_floats(pixels, *from);
        layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
            pixels.width, pixels.height, "u16", [&](std::size_t i) {
                return std::array<double, 4>{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]};
            }));
    } else {
        layer.raster_png = std::make_shared<const std::string>(write_png(ink::to_rgba8(pixels, *from)));
        layer.raster_memo = {};
    }
    layer.ink_raster = nullptr;
    layer.ink_memo = {};
}

void set_color_mode(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string mode = text_of(op, "mode", "");
    if (mode != "rgb" && mode != "cmyk" && mode != "lab") throw OpError("mode is rgb, cmyk or lab");
    std::optional<PageColor> target;
    const Json* depth = field(op, "depth");
    if (depth != nullptr && (!depth->is_number_integer() || (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16))) {
        throw OpError("depth is 8 or 16");
    }
    const int bits = depth != nullptr ? static_cast<int>(depth->get<std::int64_t>()) : 8;
    if (mode != "rgb") {
        PageColor made;
        made.space = mode == "cmyk" ? InkSpace::Cmyk : InkSpace::Lab;
        made.depth = bits;
        made.intent = text_of(op, "intent", "relative");
        if (!core::is_intent(made.intent)) throw OpError("intent is perceptual, relative, saturation or absolute");
        const Json* icc = field(op, "icc");
        const Json* profile = field(op, "profile");
        if (made.space == InkSpace::Cmyk) {
            if ((icc == nullptr) == (profile == nullptr)) throw OpError("a CMYK page needs its ICC profile: icc (base64) or profile (sha256:… of one the book has)");
            if (icc != nullptr) {
                if (!icc->is_string()) throw OpError("icc is the profile's bytes in base64");
                std::string bytes = core::a2b_base64(icc->get<std::string>());
                if (const auto why = ink::profile_problem(bytes)) throw OpError("the ICC profile cannot be used: " + *why);
                made.profile = core::bytes_ref(bytes);
                made.icc = std::make_shared<const std::string>(std::move(bytes));
            } else {
                if (!profile->is_string()) throw OpError("profile is sha256:<64 hex>");
                made.profile = profile->get<std::string>();
                made.icc = held_profile(doc, made.profile);
                if (!made.icc) throw OpError("the book has no ICC profile " + made.profile + " (give it as icc)");
            }
        } else if (icc != nullptr || profile != nullptr) {
            throw OpError("a Lab page has no ICC profile");
        }
        target = std::move(made);
    }
    const std::string gray = text_of(op, "gray", "profile");
    if (gray != "profile" && gray != "k") throw OpError("gray is profile or k");
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const Json* also = field(op, "default");
    if (also != nullptr && !also->is_boolean()) throw OpError("default is true or false");
    const bool book = every || (also != nullptr && also->get<bool>());

    if (target && target->space == InkSpace::Lab) {  // (nothing changes when one layer cannot be composited in Lab)
        for (const std::size_t i : pages) {
            for (const core::Layer& layer : doc.pages[i]->layers) {
                if (!core::lab_blend(layer.blend)) {
                    throw OpError("page " + doc.pages[i]->index.repr() + ": the layer " + layer.id + "'s blend " + core::py_repr_str(layer.blend) +
                                  " is not one a Lab page composites");
                }
            }
        }
    }
    std::int64_t converted = 0;
    for (const std::size_t i : pages) {
        const std::optional<PageColor> from = doc.pages[i]->color;
        std::optional<PageColor> to = target;
        if (to && from && from->space == to->space) to->inks = from->inks;  // (the same space keeps its exact inks)
        if (from == to) continue;
        core::Page& page = doc.edit_page(i);
        for (core::Layer& layer : page.layers) convert_layer(layer, from, to, gray == "k", bits);
        page.color = std::move(to);
        ++converted;
    }
    if (book) doc.color = target;
    c.report = Json{{"converted", converted}};
}

std::vector<core::InkEntry> inks_from(const Json& value, InkSpace space) {
    if (!value.is_array()) throw OpError("inks is a list of [[r, g, b], [values]]");
    std::vector<core::InkEntry> out;
    for (const Json& item : value) {
        if (!item.is_array() || item.size() != 2 || !item[0].is_array() || item[0].size() != 3 || !item[1].is_array()) {
            throw OpError("each ink is [[r, g, b], [values]]");
        }
        core::InkEntry entry;
        for (unsigned k = 0; k < 3; ++k) {
            const Json& v = item[0][k];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::int64_t>() > 255) throw OpError("an ink's RGB is three integers 0..255");
            entry.rgb[k] = static_cast<int>(v.get<std::int64_t>());
        }
        for (const Json& v : item[1]) {
            if (!v.is_number() || v.is_boolean()) throw OpError("an ink value is a number");
            entry.ink.push_back(v.get<double>());
        }
        try {
            core::check_ink(space, entry.ink);
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void set_inks(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const Json* inks = field(op, "inks");
    if (inks == nullptr) throw OpError("inks is a list of [[r, g, b], [values]]");
    const Json* replace = field(op, "replace");
    if (replace != nullptr && !replace->is_boolean()) throw OpError("replace is true or false");
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    std::int64_t changed = 0;
    const auto apply = [&](PageColor& color) {
        const std::vector<core::InkEntry> given = inks_from(*inks, color.space);
        std::vector<core::InkEntry> table = replace != nullptr && replace->get<bool>() ? std::vector<core::InkEntry>{} : color.inks;
        for (const core::InkEntry& entry : given) {
            const auto at = std::find_if(table.begin(), table.end(), [&](const core::InkEntry& e) { return e.rgb == entry.rgb; });
            if (at != table.end()) *at = entry;
            else table.push_back(entry);
        }
        if (table.size() > core::kInksMax) throw OpError("a page has at most " + std::to_string(core::kInksMax) + " inks");
        color.inks = std::move(table);
    };
    for (const std::size_t i : pages) {
        if (!doc.pages[i]->color) {
            if (every) continue;  // (every page: the CMYK and Lab ones)
            throw OpError("page " + doc.pages[i]->index.repr() + " is an RGB page: it has no inks");
        }
        apply(*doc.edit_page(i).color);
        ++changed;
    }
    if (every && doc.color) apply(*doc.color);
    c.report = Json{{"pages", changed}};
}

}  // namespace

void register_ink_ops(core::OpRegistry& registry) {
    registry.add("set_color_mode", set_color_mode);
    registry.add("set_inks", set_inks);
}

}  // namespace genko::render
