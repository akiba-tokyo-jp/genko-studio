// color-mode@1, color-model@1: the ops of a page's colour model (SPEC.md COLOR-02, COLOR-03): set_color_mode turns pages
// into another colour mode, depth or profile (their paint layers' pixels converted, nothing else changed),
// assign_profile names the profile their pixels are in (the pixels as they are), convert_profile converts them into a
// profile's space, set_inks sets the exact inks of RGB colours.

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/color_canvas.hpp"
#include "render/image.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/quantize.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::InkSpace;
using core::Json;
using core::OpContext;
using core::OpError;
using core::PageColor;
using core::RgbMode;

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
    const auto held = [&](const std::optional<PageColor>& inks, const std::optional<RgbMode>& rgb) -> core::Bytes {
        if (inks && inks->profile == ref && inks->icc) return inks->icc;
        if (rgb && rgb->profile == ref && rgb->icc) return rgb->icc;
        return nullptr;
    };
    if (core::Bytes icc = held(doc.color, doc.rgb)) return icc;
    for (const auto& page : doc.pages) {
        if (core::Bytes icc = held(page->color, page->rgb)) return icc;
    }
    return nullptr;
}

// A profile an op names ("profile": a built-in id, "sha256:…" of a built-in or of one the book holds; "icc": its bytes
// in base64): its space and ref, and its bytes. srgb and lab-d50 are Little CMS's own: no bytes ("" ref).
struct Named {
    core::ProfileSpace space = core::ProfileSpace::Rgb;
    std::string ref;
    core::Bytes icc;
};

std::optional<Named> named_profile(const Document& doc, const Json& op) {
    const Json* icc = field(op, "icc");
    const Json* profile = field(op, "profile");
    if (icc != nullptr && profile != nullptr) throw OpError("give the profile as profile or as icc, not both");
    Named out;
    if (icc != nullptr) {
        if (!icc->is_string()) throw OpError("icc is the profile's bytes in base64");
        std::string bytes = core::a2b_base64(icc->get<std::string>());
        const auto space = ink::profile_space(bytes);
        if (!space || *space == core::ProfileSpace::Lab) throw OpError("the ICC profile cannot be used: Little CMS cannot read it as an RGB, grey or CMYK profile");
        if (const auto why = ink::profile_problem(bytes, *space)) throw OpError("the ICC profile cannot be used: " + *why);
        out.space = *space;
        out.ref = core::bytes_ref(bytes);
        out.icc = std::make_shared<const std::string>(std::move(bytes));
        return out;
    }
    if (profile == nullptr) return std::nullopt;
    if (!profile->is_string()) throw OpError("profile is a built-in profile's id or sha256:<64 hex>");
    const std::string name = profile->get<std::string>();
    if (name.starts_with("sha256:")) {
        if (const core::BuiltinProfile* builtin = core::builtin_profile_by_ref(name)) {
            out.space = builtin->space;
            out.ref = name;
            out.icc = builtin->bytes;
            return out;
        }
        out.icc = held_profile(doc, name);
        if (!out.icc) throw OpError("the book has no ICC profile " + name + " (give it as icc)");
        const auto space = ink::profile_space(*out.icc);
        if (!space) throw OpError("the ICC profile cannot be used: Little CMS cannot read it");
        out.space = *space;
        out.ref = name;
        return out;
    }
    const core::BuiltinProfile* builtin = core::builtin_profile(name);
    if (builtin == nullptr) throw OpError("no built-in profile " + core::py_repr_str(name) + " (srgb, display-p3, adobe-rgb-compat, prophoto-compat, "
                                          "linear-srgb, rec2020, gray-gamma-2.2, sgray, lab-d50, coated-cmyk)");
    out.space = builtin->space;
    out.ref = builtin->ref();
    out.icc = builtin->bytes;
    return out;
}

// The colour model of a page or the book: its inks, or its RGB mode (none: sRGB, Python's), and with none the depth its
// paint pixels are held in — 8, or that of its most precise raster (put_color_raster's, u16 or f32).
struct Model {
    std::optional<PageColor> inks;
    std::optional<RgbMode> rgb;
    int held = 8;
    int depth() const { return inks ? inks->depth : rgb ? rgb->depth : held; }
    RgbMode rgb_mode() const {
        RgbMode out = rgb.value_or(RgbMode{});
        out.depth = depth();
        return out;
    }
    bool operator==(const Model& other) const { return inks == other.inks && rgb == other.rgb && depth() == other.depth(); }
};

// The depth a page of no colour model holds its paint pixels in: its most precise raster's.
int held_depth(const core::Page& page) {
    int out = 8;
    for (const core::Layer& layer : page.layers) {
        if (layer.color_raster && layer.color_raster->size() > 5) out = std::max(out, (*layer.color_raster)[5] == 2 ? 16 : 32);
    }
    return out;
}

Model model_of(const core::Page& page) { return Model{page.color, page.rgb, page.color || page.rgb ? 8 : held_depth(page)}; }

// A page of no colour model whose target — its depth not said — is sRGB at the depth it holds its pixels in already:
// still none.
Model kept_plain(Model to, const Model& from) {
    if (from.inks || from.rgb || to.inks || !to.rgb || to.rgb->depth != from.held) return to;
    RgbMode plain = *to.rgb;
    plain.depth = 8;
    if (!plain.is_default()) return to;
    to.rgb.reset();
    to.held = from.held;
    return to;
}

// How a conversion goes: greys onto K alone, 8 bits dithered, HDR values cut (counted).
struct Way {
    bool gray_on_k = false;
    bool dither = false;
    bool clip_hdr = false;
    std::int64_t clipped = 0;
};

// How a conversion goes into a page (intent, bpc: its intent and black point compensation) and the settings colours come
// into the page with afterwards, kept as the page's own (page_intent, page_bpc; without them the conversion's).
struct Into {
    std::string intent = "relative";
    bool bpc = true;
    std::string page_intent = "relative";
    bool page_bpc = true;
};

Into into_of(const Json& op) {
    Into out;
    out.intent = text_of(op, "intent", "relative");
    if (!core::is_intent(out.intent)) throw OpError("intent is perceptual, relative, saturation or absolute");
    if (const Json* bpc = field(op, "bpc"); bpc != nullptr) {
        if (!bpc->is_boolean()) throw OpError("bpc is true or false");
        out.bpc = bpc->get<bool>();
    }
    out.page_intent = text_of(op, "page_intent", out.intent);
    if (!core::is_intent(out.page_intent)) throw OpError("page_intent is perceptual, relative, saturation or absolute");
    out.page_bpc = out.bpc;
    if (const Json* bpc = field(op, "page_bpc"); bpc != nullptr) {
        if (!bpc->is_boolean()) throw OpError("page_bpc is true or false");
        out.page_bpc = bpc->get<bool>();
    }
    return out;
}

// The model a mode makes at a depth (`named`: the profile the op names, or none).
Model target_of(std::string_view mode, int depth, const std::optional<Named>& named, const Into& into) {
    Model out;
    const auto wrong_space = [&](std::string_view page) {
        return OpError("the profile is for another colour space (" + std::string(core::to_string(named->space)) + "): a " + std::string(page) +
                       " page needs a profile of its own space");
    };
    if (mode == "rgb") {
        RgbMode rgb;
        rgb.depth = depth;
        rgb.intent = into.intent;
        rgb.bpc = into.bpc;
        if (named) {
            if (named->space != core::ProfileSpace::Rgb) throw wrong_space("RGB");
            if (named->icc) {
                if (const auto why = ink::profile_problem(*named->icc, core::ProfileSpace::Rgb)) throw OpError("the ICC profile cannot be used: " + *why);
            }
            rgb.profile = named->ref;  // (srgb: none)
            rgb.icc = named->icc;
        }
        if (!rgb.is_default()) out.rgb = std::move(rgb);
        return out;
    }
    PageColor made;
    made.depth = depth;
    made.intent = into.intent;
    made.bpc = into.bpc;
    if (mode == "lab") {
        made.space = InkSpace::Lab;
        if (named && !(named->space == core::ProfileSpace::Lab)) throw OpError("a Lab page has no ICC profile");
    } else if (mode == "gray") {
        made.space = InkSpace::Gray;
        const Named grey = named ? *named : Named{core::ProfileSpace::Gray, core::builtin_profile("sgray")->ref(), core::builtin_profile("sgray")->bytes};
        if (grey.space != core::ProfileSpace::Gray) throw wrong_space("grey");
        made.profile = grey.ref;
        made.icc = grey.icc;
    } else {
        made.space = InkSpace::Cmyk;
        if (!named) {
            throw OpError("a CMYK page needs its ICC profile: profile (coated-cmyk, or sha256:… of one the book has) or icc (base64)");
        }
        if (named->space != core::ProfileSpace::Cmyk) throw wrong_space("CMYK");
        made.profile = named->ref;
        made.icc = named->icc;
    }
    out.inks = std::move(made);
    return out;
}

// The model a conversion makes (`to`, its intent and black point compensation the conversion's), as the page keeps it:
// with the page's own (page_intent, page_bpc) — an RGB page at 8 bits in sRGB with the defaults has none.
Model kept_as(Model to, const Into& into) {
    if (to.inks) {
        to.inks->intent = into.page_intent;
        to.inks->bpc = into.page_bpc;
        return to;
    }
    if (!to.rgb && into.page_intent == "relative" && into.page_bpc) return to;  // (a page of no colour model, at its held depth, stays one)
    RgbMode rgb = to.rgb_mode();
    rgb.intent = into.page_intent;
    rgb.bpc = into.page_bpc;
    to.rgb = rgb.is_default() ? std::nullopt : std::optional<RgbMode>(std::move(rgb));
    return to;
}

std::string layer_name(const core::Page& page, const core::Layer& layer) {
    return "page " + page.index.repr() + ": the layer " + layer.id + (layer.title.empty() ? std::string() : " (" + layer.title + ")");
}

// HDR values (colours past 0..1) of pixels going where they cannot be kept: refused, or cut when asked (the pixels
// counted). `stride` 4: RGBA (the colours checked); 1: one value each.
void cut_hdr(std::vector<float>& values, std::size_t stride, Way& way, const std::string& where) {
    const std::size_t colours = stride == 4 ? 3 : 1;
    std::int64_t outside = 0;
    for (std::size_t i = 0; i + stride <= values.size(); i += stride) {
        for (std::size_t c = 0; c < colours; ++c) {
            if (values[i + c] < 0.0f || values[i + c] > 1.0f) {
                ++outside;
                break;
            }
        }
    }
    if (outside == 0) return;
    if (!way.clip_hdr) throw OpError(where + " holds HDR values past 0..1, which cannot be converted as they are (clip_hdr: true cuts them and converts)");
    way.clipped += outside;
    for (std::size_t i = 0; i + stride <= values.size(); i += stride) {
        for (std::size_t c = 0; c < colours; ++c) values[i + c] = std::clamp(values[i + c], 0.0f, 1.0f);
    }
}

// A grey raster's HDR (32 bits: lightness past 0..1) going where it cannot be kept: refused, or cut when asked (counted).
void cut_grey_hdr(core::InkPixels& pixels, Way& way, const std::string& where) {
    if (!pixels.is_float() || !core::traits(pixels.space).hdr) return;
    std::vector<float> lightness(pixels.pixel_count());
    for (std::size_t i = 0; i < pixels.pixel_count(); ++i) lightness[i] = static_cast<float>(pixels.value(i, 0));
    cut_hdr(lightness, 1, way, where);
    for (std::size_t i = 0; i < pixels.pixel_count(); ++i) pixels.set(i, 0, lightness[i]);
}

// A paint layer's RGB pixels (raster_png or color_raster) as straight RGBA floats, the page's values.
struct RgbPixels {
    std::uint32_t width = 0, height = 0;
    std::vector<float> rgba;
    bool from_png = false;
};

std::optional<RgbPixels> rgb_pixels(const core::Layer& layer) {
    RgbPixels out;
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        out.width = view.width();
        out.height = view.height();
        out.rgba.resize(std::size_t(out.width) * out.height * 4);
        for (std::size_t i = 0; i < std::size_t(out.width) * out.height; ++i) {
            const auto p = view.pixel(i);
            for (unsigned c = 0; c < 4; ++c) out.rgba[i * 4 + c] = static_cast<float>(p[c]);
        }
        return out;
    }
    if (!layer.raster_png || layer.raster_png->empty()) return std::nullopt;
    const Image image = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
    out.width = static_cast<std::uint32_t>(image.width());
    out.height = static_cast<std::uint32_t>(image.height());
    out.from_png = true;
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(image.raw()->image[y]);
        for (std::size_t x = 0; x < std::size_t(image.width()) * 4; ++x) out.rgba[std::size_t(y) * image.width() * 4 + x] = row[x] / 255.0f;
    }
    return out;
}

// A sample's deterministic noise in [0, 1) (8 bits dithered: the same picture always gives the same pixels).
double noise(std::uint32_t x, std::uint32_t y, unsigned c) {
    std::uint32_t h = x * 73856093u ^ y * 19349663u ^ (c + 1) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xffffffu) / 16777216.0;
}

// The pixels written as the layer's at an RGB page's depth: 8 a PNG (rounded, or dithered: an 8-bit value stays
// itself), 16 GKCR u16, 32 GKCR f32.
void put_rgb(core::Layer& layer, const RgbPixels& p, int depth, bool dither) {
    layer.ink_raster = nullptr;
    layer.ink_memo = {};
    if (depth == 8) {
        Image out = Image::create("RGBA", Size{static_cast<int>(p.width), static_cast<int>(p.height)}, Ink{0, 0, 0, 0});
        for (std::uint32_t y = 0; y < p.height; ++y) {
            auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
            for (std::uint32_t x = 0; x < p.width; ++x) {
                for (unsigned c = 0; c < 4; ++c) {
                    double v = std::clamp(static_cast<double>(p.rgba[(std::size_t(y) * p.width + x) * 4 + c]), 0.0, 1.0) * 255.0;
                    // (an 8-bit value a float's rounding left a hair off: itself, whatever the noise)
                    if (std::abs(v - std::round(v)) < 1e-4) v = std::round(v);
                    const double q = dither && c < 3 ? std::floor(v + noise(x, y, c)) : std::round(v);
                    row[x * 4 + c] = static_cast<unsigned char>(std::clamp(q, 0.0, 255.0));
                }
            }
        }
        layer.raster_png = std::make_shared<const std::string>(write_png(out));
        layer.raster_memo = {};
        layer.color_raster = nullptr;
        return;
    }
    layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(p.width, p.height, depth == 16 ? "u16" : "f32", [&](std::size_t i) {
        std::array<double, 4> out{};
        for (unsigned c = 0; c < 4; ++c) {
            out[c] = p.rgba[i * 4 + c];
            if (depth == 16 || c == 3) out[c] = std::clamp(out[c], 0.0, 1.0);
        }
        return out;
    }));
    layer.raster_png = nullptr;
    layer.raster_memo = {};
    layer.raster_relpath.reset();
}

// A layer's pixels in the new model.
void convert_layer(const core::Page& page, core::Layer& layer, const Model& from, const Model& to, Way& way) {
    const std::string where = layer_name(page, layer);
    const RgbMode from_rgb = from.rgb_mode(), to_rgb = to.rgb_mode();
    if (to.inks) {
        std::optional<core::InkPixels> pixels;
        if (layer.ink_raster) {
            if (!from.inks) throw OpError("the layer " + layer.id + " holds ink pixels on an RGB page");
            core::InkPixels source = core::decode_ink_raster(*layer.ink_raster);
            // (a grey's HDR stays only in a grey page of the same profile at 32 bits: the same values)
            if (!(to.inks->space == source.space && to.inks->profile == from.inks->profile && to.inks->depth == 32)) cut_grey_hdr(source, way, where);
            pixels = ink::convert(source, *from.inks, *to.inks, way.gray_on_k);
        } else if (layer.raster_png && !layer.raster_png->empty()) {
            pixels = ink::from_rgb(open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA"), *to.inks, way.gray_on_k, from_rgb.icc);
        } else if (layer.color_raster) {
            RgbPixels rgb = *rgb_pixels(layer);
            cut_hdr(rgb.rgba, 4, way, where);
            pixels = ink::from_srgb(rgb.rgba, rgb.width, rgb.height, *to.inks, way.gray_on_k, from_rgb.icc);
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
    const int depth = to.depth();
    if (layer.ink_raster) {
        if (!from.inks) return;
        core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
        // (a grey's HDR stays only in sRGB at 32 bits — sGray's greys are sRGB's by their value —; elsewhere it is cut
        // or the conversion refused, before the transforms clamp it)
        const bool numeric = from.inks->profile == core::builtin_profile("sgray")->ref() && !to_rgb.icc && to_rgb.intent == "relative" && to_rgb.bpc;
        if (!(depth == 32 && numeric)) cut_grey_hdr(pixels, way, where);
        if (depth == 8) {
            layer.raster_png = std::make_shared<const std::string>(write_png(ink::to_rgb8(pixels, *from.inks, to_rgb)));
            layer.raster_memo = {};
            layer.ink_raster = nullptr;
            layer.ink_memo = {};
            return;
        }
        RgbPixels rgb{pixels.width, pixels.height, ink::to_rgb_floats(pixels, *from.inks, to_rgb), false};
        put_rgb(layer, rgb, depth, way.dither);
        return;
    }
    std::optional<RgbPixels> rgb = rgb_pixels(layer);
    if (!rgb) return;
    const bool same_space = from_rgb.profile == to_rgb.profile;
    const int now = rgb->from_png ? 8 : static_cast<unsigned char>((*layer.color_raster)[5]) == 2 ? 16 : 32;
    if (same_space && now == depth) return;  // (as it is: its own bytes)
    if (!same_space) {
        cut_hdr(rgb->rgba, 4, way, where);
        ink::rgb_to_rgb(rgb->rgba, from_rgb.icc, to_rgb);
    } else if (depth != 32) {
        cut_hdr(rgb->rgba, 4, way, where);
    }
    put_rgb(layer, *rgb, depth, way.dither);
}

// Why the page cannot be drawn as it is now yet — in precise colour: a page of 16 or 32 bits (precise pixels), an RGB
// page of a profile (its values, at 8 bits too) — naming the first layer that cannot (what render::page and
// core::validate_color_document refuse); nothing when it can. (PC2d, document:color-depth-model: tone layers, layers in
// a folder, corrections that move shapes, precise layers with lines, pictures, effects, a screen or a colour of their
// own, v2 line layers' rasters are drawn: render/page precise_layer.)
std::optional<std::string> precise_blocker(const core::Page& page) {
    const bool precise = std::any_of(page.layers.begin(), page.layers.end(), [](const core::Layer& l) { return l.color_raster != nullptr; });
    if (!precise && !core::profiled_rgb(page)) return std::nullopt;
    for (const core::Layer& layer : page.layers) {
        const std::string where = layer_name(page, layer);
        const bool holds_drawing = layer.kind == core::LayerKind::Raster || layer.kind == core::LayerKind::Strokes || layer.kind == core::LayerKind::Tone;
        if (layer.color_raster && !holds_drawing) return where + " is not a layer that holds pixels";
        if (!layer.visible || layer.kind == core::LayerKind::Folder) continue;
        // (a correction layer's own blend is not looked at, as Python does not: its native.blend is drawn — the user's
        // decision 2 of PC2)
        if (layer.kind != core::LayerKind::Adjust && !ColorCanvas::supports_blend(layer.blend))
            return where + " has the blend " + core::py_repr_str(layer.blend);
    }
    return std::nullopt;
}

void refuse_undrawable(const core::Page& page) {
    if (const auto why = precise_blocker(page)) {
        throw core::Error("not_yet_ported", *why + ": a page in precise colour (16 or 32 bits, or of a profile) cannot draw it yet");
    }
}

// The size of a layer's paint pixels (from the header of its PNG, GKCR or GKIR), or none.
std::optional<std::pair<std::uint64_t, std::uint64_t>> pixels_size(const core::Layer& layer) {
    const auto le32 = [](const std::string& b, std::size_t at) {
        std::uint32_t v = 0;
        for (int k = 3; k >= 0; --k) v = v << 8 | static_cast<unsigned char>(b[at + k]);
        return v;
    };
    if (layer.color_raster && layer.color_raster->size() >= 16) return std::pair<std::uint64_t, std::uint64_t>{le32(*layer.color_raster, 8), le32(*layer.color_raster, 12)};
    if (layer.ink_raster) {
        const core::InkRasterHeader h = core::ink_raster_header(*layer.ink_raster);
        return std::pair<std::uint64_t, std::uint64_t>{h.width, h.height};
    }
    if (!layer.raster_png || layer.raster_png->empty()) return std::nullopt;
    const std::string& png = *layer.raster_png;
    if (png.size() >= 24 && png.compare(1, 3, "PNG") == 0 && png.compare(12, 4, "IHDR") == 0) {
        const auto be32 = [&](std::size_t at) {
            std::uint32_t v = 0;
            for (int k = 0; k < 4; ++k) v = v << 8 | static_cast<unsigned char>(png[at + k]);
            return v;
        };
        return std::pair<std::uint64_t, std::uint64_t>{be32(16), be32(20)};
    }
    const Image image = open_image(png, kPillowOpenLimits);
    return std::pair<std::uint64_t, std::uint64_t>{static_cast<std::uint64_t>(image.width()), static_cast<std::uint64_t>(image.height())};
}

// The book's precise pixels (GKCR) as they will be after converting pages to their targets: past the budget (1 GiB),
// refused before any pixel is made.
void check_budget(const Document& doc, const std::vector<std::pair<std::size_t, Model>>& plan) {
    std::uint64_t bytes = 0;
    std::unordered_set<const std::string*> held;
    const auto add = [&](std::uint64_t more) {
        bytes += more;
        if (bytes > core::kColorRasterBookBytes) {
            throw OpError("the precise pixels cannot be kept: high-precision color raster book budget exceeded (the book holds 1 GiB of them at most)");
        }
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const core::Page& page = doc.page(i);
        const auto planned = std::find_if(plan.begin(), plan.end(), [&](const auto& p) { return p.first == i; });
        for (const core::Layer& layer : page.layers) {
            if (planned == plan.end()) {
                if (layer.color_raster && held.insert(layer.color_raster.get()).second) add(layer.color_raster->size());
                continue;
            }
            const Model& to = planned->second;
            const int depth = to.depth();
            if (to.inks || depth == 8) continue;  // (inks, or 8 bits: no precise raster)
            const Model from = model_of(page);
            const bool kept = layer.color_raster && !from.inks && from.rgb_mode().profile == to.rgb_mode().profile &&
                              ((*layer.color_raster)[5] == 2 ? 16 : 32) == depth;
            if (kept) {
                if (held.insert(layer.color_raster.get()).second) add(layer.color_raster->size());
                continue;
            }
            if (const auto size = pixels_size(layer)) add(16 + size->first * size->second * 4 * (depth == 16 ? 2 : 4));
        }
    }
}

// Pages converted to a model (each page's own target: `target_for`, its intent and black point compensation the
// conversion's), in place, keeping the page's own way (`into`'s page_intent, page_bpc); how many changed. Refused as a
// whole: nothing changes when one layer cannot be converted.
std::int64_t convert_pages(Document& doc, const std::vector<std::size_t>& pages, const std::function<Model(const core::Page&)>& target_for,
                           Way& way, const Into& into) {
    std::vector<std::pair<std::size_t, Model>> plan;
    std::vector<Model> keep;  // (each planned page's model as it keeps it)
    for (const std::size_t i : pages) {
        const Model from = model_of(*doc.pages[i]);
        Model to = target_for(*doc.pages[i]);
        if (to.inks && from.inks && from.inks->space == to.inks->space) to.inks->inks = from.inks->inks;  // (the same space keeps its exact inks)
        const Model kept = kept_as(to, into);
        if (from == kept) continue;
        // (a correction layer's blend is its native.blend: its own is not looked at, as Python does not — PC2's decision 2)
        const auto blend_of = [](const core::Layer& layer) {
            if (layer.kind != core::LayerKind::Adjust) return layer.blend;
            return layer.adjust ? core::native_of(*layer.adjust).blend : std::string("normal");
        };
        if (to.inks && to.inks->space == InkSpace::Lab) {  // (nothing changes when one layer cannot be composited in Lab)
            for (const core::Layer& layer : doc.pages[i]->layers) {
                if (!core::lab_blend(blend_of(layer))) {
                    throw OpError("page " + doc.pages[i]->index.repr() + ": the layer " + layer.id + "'s blend " + core::py_repr_str(blend_of(layer)) +
                                  " is not one a Lab page composites");
                }
            }
        }
        if (to.inks && to.inks->space == InkSpace::Multi) {  // (nor in a multichannel page's plates: the separable blends only)
            for (const core::Layer& layer : doc.pages[i]->layers) {
                if (!core::multi_blend(blend_of(layer))) {
                    throw OpError("page " + doc.pages[i]->index.repr() + ": the layer " + layer.id + "'s blend " + core::py_repr_str(blend_of(layer)) +
                                  " is not one a multichannel page composites");
                }
            }
        }
        plan.emplace_back(i, std::move(to));
        keep.push_back(kept);
    }
    check_budget(doc, plan);
    for (std::size_t k = 0; k < plan.size(); ++k) {
        const auto& [i, to] = plan[k];
        const Model from = model_of(*doc.pages[i]);
        core::Page& page = doc.edit_page(i);
        for (core::Layer& layer : page.layers) convert_layer(page, layer, from, to, way);
        page.color = std::move(keep[k].inks);
        page.rgb = std::move(keep[k].rgb);
        refuse_undrawable(page);
    }
    return static_cast<std::int64_t>(plan.size());
}

Way way_of(const Json& op) {
    Way way;
    const auto flag = [&](const char* key) {
        const Json* v = field(op, key);
        if (v != nullptr && !v->is_boolean()) throw OpError(std::string(key) + " is true or false");
        return v != nullptr && v->get<bool>();
    };
    const std::string gray = text_of(op, "gray", "profile");
    if (gray != "profile" && gray != "k") throw OpError("gray is profile or k");
    way.gray_on_k = gray == "k";
    way.dither = flag("dither");
    way.clip_hdr = flag("clip_hdr");
    return way;
}

Json report_of(std::int64_t converted, const Way& way) {
    Json out{{"converted", converted}};
    if (way.clipped > 0) out["clipped"] = way.clipped;
    return out;
}

// The book's precise pixels as they must be after a conversion (the budget): said as the op's refusal.
void check_precise(const Document& doc) {
    try {
        core::validate_color_document(doc);
    } catch (const core::Error& error) {
        if (error.code() == "not_yet_ported") throw;
        throw OpError(std::string("the precise pixels cannot be kept: ") + error.what() + " (the book holds 1 GiB of them at most)");
    }
}

// The op's "default": the book's default model too (new pages), besides the pages it names.
bool default_of(const Json& op) {
    const Json* also = field(op, "default");
    if (also != nullptr && !also->is_boolean()) throw OpError("default is true or false");
    return also != nullptr && also->get<bool>();
}

void to_multichannel(OpContext& c);

void set_color_mode(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string mode = text_of(op, "mode", "");
    if (mode == "multichannel") {
        to_multichannel(c);
        return;
    }
    if (mode != "rgb" && mode != "gray" && mode != "cmyk" && mode != "lab") throw OpError("mode is rgb, gray, cmyk, lab or multichannel");
    const Json* depth = field(op, "depth");
    if (depth != nullptr && (!depth->is_number_integer() ||
                             (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16 && depth->get<std::int64_t>() != 32))) {
        throw OpError("depth is 8, 16 or 32");
    }
    // (no depth said: 8 bits — but a page of no colour model keeps the precision its precise rasters hold)
    const int bits = depth != nullptr ? static_cast<int>(depth->get<std::int64_t>()) : 0;
    const std::optional<Named> named = named_profile(doc, op);
    const Into into = into_of(op);
    const Model target = target_of(mode, bits != 0 ? bits : 8, named, into);
    const auto target_for = [&](const core::Page& page) {
        if (bits != 0 || page.color || page.rgb) return target;
        return kept_plain(target_of(mode, held_depth(page), named, into), model_of(page));
    };
    Way way = way_of(op);
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = every || default_of(op);
    const std::int64_t converted = convert_pages(doc, pages, target_for, way, into);
    check_precise(doc);
    if (book) {
        const Model kept = kept_as(target, into);
        doc.color = kept.inks;
        doc.rgb = kept.rgb;
    }
    c.report = report_of(converted, way);
}

// The mode a page is in, as an op names it (rgb, gray, cmyk, lab).
std::string_view mode_of(const Model& model) { return model.inks ? core::to_string(model.inks->space) : std::string_view("rgb"); }

// color-model@1 (PC1d): the mode of a page in one of Image › Mode's own (indexed, bitmap, duotone, multichannel), or none.
std::optional<std::string> special_mode(const Model& model) {
    if (model.rgb && model.rgb->table) return std::string("indexed");
    if (model.inks && (model.inks->bitmap || !model.inks->duotone.empty() || model.inks->space == InkSpace::Multi)) {
        return std::string(core::mode_name(*model.inks));
    }
    return std::nullopt;
}

void assign_profile(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const Json* profile = field(op, "profile");
    const bool none = profile != nullptr && *profile == Json("none");
    const std::optional<Named> named = none ? std::nullopt : named_profile(doc, op);
    if (!none && !named) throw OpError("profile is a built-in profile's id, sha256:… of one the book holds, or none (or give icc)");
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = default_of(op);
    // (the model with the profile in place of its own, or none when it cannot take it)
    const auto assigned = [&](const Model& from) -> std::optional<Model> {
        Model to = from;
        const std::string_view mode = mode_of(from);
        if (from.rgb && from.rgb->table) return std::nullopt;  // (an indexed page is sRGB's: its table's colours)
        if (mode == "rgb") {
            if (named && named->space != core::ProfileSpace::Rgb) return std::nullopt;
            if (named && named->icc) {
                if (const auto why = ink::profile_problem(*named->icc, core::ProfileSpace::Rgb)) throw OpError("the ICC profile cannot be used: " + *why);
            }
            RgbMode rgb = from.rgb_mode();
            rgb.profile = named ? named->ref : std::string();
            rgb.icc = named ? named->icc : nullptr;
            to.rgb = rgb.is_default() ? std::nullopt : std::optional<RgbMode>(rgb);
            return to;
        }
        if (mode == "gray") {
            const Named grey = named ? *named : Named{core::ProfileSpace::Gray, core::builtin_profile("sgray")->ref(), core::builtin_profile("sgray")->bytes};
            if (grey.space != core::ProfileSpace::Gray) return std::nullopt;
            to.inks->profile = grey.ref;
            to.inks->icc = grey.icc;
            return to;
        }
        if (mode == "cmyk" && named && named->space == core::ProfileSpace::Cmyk) {
            to.inks->profile = named->ref;
            to.inks->icc = named->icc;
            return to;
        }
        return std::nullopt;
    };
    std::int64_t count = 0;
    for (const std::size_t i : pages) {
        const Model from = model_of(*doc.pages[i]);
        std::optional<Model> to = assigned(from);
        if (to) to = kept_plain(*to, from);
        if (!to) {
            if (every) continue;  // (every page: those of the profile's space)
            const std::string_view mode = mode_of(from);
            if (mode == "lab") throw OpError("page " + doc.pages[i]->index.repr() + " is a Lab page: it has no profile");
            if (from.rgb && from.rgb->table) throw OpError("page " + doc.pages[i]->index.repr() + " is an indexed page: its colours are sRGB's");
            if (mode == "multichannel") throw OpError("page " + doc.pages[i]->index.repr() + " is a multichannel page: its plates have no profile");
            if (none) throw OpError("page " + doc.pages[i]->index.repr() + " is a CMYK page: none is for an RGB or grey page");
            throw OpError("the profile is for another colour space (" + std::string(core::to_string(named->space)) + ") than page " +
                          doc.pages[i]->index.repr() + "'s (" + std::string(mode) + ")");
        }
        if (*to == from) continue;
        core::Page& page = doc.edit_page(i);
        page.color = to->inks;
        page.rgb = to->rgb;
        refuse_undrawable(page);  // (a page of a profile is drawn in precise colour: what it cannot draw yet, refused)
        ++count;
    }
    if (every || book) {
        if (const std::optional<Model> to = assigned(Model{doc.color, doc.rgb})) {
            doc.color = to->inks;
            doc.rgb = to->rgb;
        } else if (!every) {
            throw OpError("the book's default mode is of another colour space than the profile's");
        } else if (count == 0) {
            throw OpError("no page is of the profile's colour space");
        }
    }
    c.report = Json{{"assigned", count}};
}

void convert_profile(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::optional<Named> named = named_profile(doc, op);
    if (!named) throw OpError("profile is a built-in profile's id or sha256:… of one the book holds (or give icc)");
    const std::string_view mode = named->space == core::ProfileSpace::Rgb    ? "rgb"
                                  : named->space == core::ProfileSpace::Gray ? "gray"
                                  : named->space == core::ProfileSpace::Cmyk ? "cmyk"
                                                                             : "lab";
    const Into into = into_of(op);
    Way way = way_of(op);
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = every || default_of(op);
    // (an indexed, bitmap, duotone or multichannel page is not converted into a profile: its mode first, set_color_mode)
    for (const std::size_t i : pages) {
        if (const auto special = special_mode(model_of(*doc.pages[i]))) {
            throw OpError("page " + doc.pages[i]->index.repr() + (*special == "indexed" ? " is an " : " is a ") + *special + " page: convert_profile takes an RGB, grey, CMYK or Lab page");
        }
    }
    // (each page at its own depth)
    const auto target = [&](const Model& from) { return kept_plain(target_of(mode, from.depth(), named, into), from); };
    const std::int64_t converted = convert_pages(doc, pages, [&](const core::Page& page) { return target(model_of(page)); }, way, into);
    check_precise(doc);
    if (book) {
        const Model kept = kept_as(target(Model{doc.color, doc.rgb}), into);
        doc.color = kept.inks;
        doc.rgb = kept.rgb;
    }
    c.report = report_of(converted, way);
}

std::vector<core::InkEntry> inks_from(const Json& value, const PageColor& color) {
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
            core::check_ink(color, entry.ink);
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
        const std::vector<core::InkEntry> given = inks_from(*inks, color);
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

// set_text_gamma (color-model@1, SPEC COLOR-03 PC1b; PhotoCraft's Color Settings › blendTextGamma, kept in the book so
// the book draws the same in the app, on the command line and through MCP): {"op": "set_text_gamma", "gamma": 1..2.2 |
// false | null}. 1, false or null: none (the letters mixed as Python's).
void set_text_gamma(OpContext& c) {
    const auto given = c.op.find("gamma");  // (null is one of its values: none)
    if (given == c.op.end()) throw OpError("gamma is a number from 1 to 2.2 (1, false or null: none)");
    const Json* gamma = &*given;
    std::optional<double> value;
    if (gamma->is_boolean()) {
        if (gamma->get<bool>()) value = 1.45;  // (Photoshop's own, as upstream's `true`)
    } else if (gamma->is_number()) {
        const double g = gamma->get<double>();
        if (!std::isfinite(g) || g < 1.0 || g > core::kTextGammaMax) throw OpError("gamma is a number from 1 to 2.2 (1, false or null: none)");
        if (g > 1.0) value = g;
    } else if (!gamma->is_null()) {
        throw OpError("gamma is a number from 1 to 2.2 (1, false or null: none)");
    }
    c.doc.text_gamma = value;
    c.report = Json{{"gamma", value ? Json(*value) : Json()}};
}

// --- PC1d: Image › Mode's own modes (color-model@1, SPEC.md COLOR-03) -------------------------------------------------
// to_indexed, set_color_table, to_bitmap, set_duotone and set_color_mode's multichannel: PhotoCraft v0.2.0's
// mode_cmds.rs and multichannel_cmds.rs (MIT, third_party/photocraft/NOTICE.md), its "document" a page. The user's
// decision (2026-10-16): indexed, bitmap and multichannel flatten the page's pixel layers into one as PhotoCraft does —
// by Genko's own flatten (merge_visible's), its frames, lines and balloons kept as they are —; duotone and the colour
// table change no pixel.

// The page of an id now (an op run inside this one may have made the book's pages anew).
std::size_t page_at(const Document& doc, const std::string& id) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->id == id) return i;
    }
    throw OpError("the page is no longer in the book");
}

std::vector<std::string> ids_of(const Document& doc, const std::vector<std::size_t>& pages) {
    std::vector<std::string> out;
    for (const std::size_t i : pages) out.push_back(doc.pages[i]->id);
    return out;
}

std::string page_name(const core::Page& page) { return "page " + page.index.repr(); }

// A page's own way of taking colours in (its intent and black point compensation), kept through a conversion.
Into own_way(const Model& from) {
    Into out;
    out.intent = from.inks ? from.inks->intent : from.rgb_mode().intent;
    out.bpc = from.inks ? from.inks->bpc : from.rgb_mode().bpc;
    out.page_intent = out.intent;
    out.page_bpc = out.bpc;
    return out;
}

double number_in(const Json& op, const char* key, double fallback, double lo, double hi, bool whole = false) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    const auto said = [](double d) { return std::to_string(static_cast<long long>(d)); };
    const std::string why = std::string(key) + " is a " + (whole ? "whole " : "") + "number from " + said(lo) + " to " + said(hi);
    if (!v->is_number() || v->is_boolean() || (whole && !v->is_number_integer())) throw OpError(why);
    const double d = v->get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) throw OpError(why);
    return d;
}

bool flag_in(const Json& op, const char* key, bool fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(std::string(key) + " is true or false");
    return v->get<bool>();
}

// A colour as PhotoCraft takes one: "#rrggbb" or [r, g, b] (whole numbers 0..255).
std::optional<std::array<int, 3>> colour_value(const Json& v) {
    std::array<int, 3> out{};
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) return std::nullopt;
        for (unsigned k = 0; k < 3; ++k) out[k] = std::stoi(s.substr(k * 2, 2), nullptr, 16);
        return out;
    }
    if (!v.is_array() || v.size() != 3) return std::nullopt;
    for (unsigned k = 0; k < 3; ++k) {
        if (!v[k].is_number_integer() || v[k].get<std::int64_t>() < 0 || v[k].get<std::int64_t>() > 255) return std::nullopt;
        out[k] = static_cast<int>(v[k].get<std::int64_t>());
    }
    return out;
}

// The model's pixel layers flattened and converted (a page's own): run on each page an op names, by its id.
struct Special {
    std::int64_t flattened = 0;
    std::int64_t converted = 0;
    std::int64_t discarded = 0;  // (the hidden layers the flattens left out)
    // (the report's "flattened", and "discarded" when there were any)
    void into(Json& report) const {
        report["flattened"] = flattened;
        if (discarded > 0) report["discarded"] = discarded;
    }
};

// A page converted to a model, its own way kept (how indexed, bitmap and multichannel bring a page to the mode they work
// from: sRGB 8 bits, grey 8 bits, …); whether it changed.
std::int64_t convert_page(Document& doc, std::size_t at, const Model& to, Way& way) {
    const Into into = own_way(model_of(doc.page(at)));
    return convert_pages(doc, {at}, [&](const core::Page&) { return to; }, way, into);
}

// The process inks as Multichannel shows them (PhotoCraft's multichannel_cmds.rs: CYAN, MAGENTA, YELLOW, BLACK; a grey's
// and Lab's plates Color::BLACK).
constexpr std::array<int, 3> kCyan{0, 174, 239}, kMagenta{236, 0, 140}, kYellow{255, 242, 0}, kProcessBlack{35, 31, 32}, kBlack{0, 0, 0};

core::Plate plate_of(std::string name, const std::array<int, 3>& colour, bool key, std::vector<core::CurvePoint> curve = {}) {
    core::Plate out;
    out.name = std::move(name);
    out.color = colour;
    out.key = key;
    out.curve = std::move(curve);
    return out;
}

// The multichannel model a page's makes at a depth (PhotoCraft's inks_of: RGB → Cyan, Magenta, Yellow — 1 − its values,
// greys on all three —; CMYK → its four, greys on Black; Lab → Alpha 1..3; grey → Black; duotone → one plate for each
// ink through its curve). The page's intent, black point compensation and profile (CMYK's, a grey's) stay its own.
Model multichannel_of(const Model& from, int depth) {
    core::PageColor made;
    made.space = InkSpace::Multi;
    made.depth = depth;
    if (from.inks && from.inks->space == InkSpace::Multi) {
        made = *from.inks;
        made.depth = depth;
        return Model{made, std::nullopt, 8};
    }
    if (from.inks) {
        const core::PageColor& f = *from.inks;
        made.intent = f.intent;
        made.bpc = f.bpc;
        if (f.space == InkSpace::Cmyk) {
            made.from = "cmyk";
            made.profile = f.profile;
            made.icc = f.icc;
            made.plates = {plate_of("Cyan", kCyan, false), plate_of("Magenta", kMagenta, false), plate_of("Yellow", kYellow, false),
                           plate_of("Black", kProcessBlack, true)};
        } else if (f.space == InkSpace::Lab) {
            made.from = "lab";
            made.plates = {plate_of("Alpha 1", kBlack, true), plate_of("Alpha 2", kBlack, false), plate_of("Alpha 3", kBlack, false)};
        } else if (!f.duotone.empty()) {
            made.from = "duotone";
            made.profile = f.profile;
            made.icc = f.icc;
            for (const core::DuotoneInk& ink : f.duotone) made.plates.push_back(plate_of(ink.name, ink.color, true, ink.curve));
        } else {
            made.from = "gray";
            made.profile = f.profile;
            made.icc = f.icc;
            made.plates = {plate_of("Black", kBlack, true)};
        }
        return Model{made, std::nullopt, 8};
    }
    const RgbMode rgb = from.rgb_mode();
    made.from = "rgb";
    made.intent = rgb.intent;
    made.bpc = rgb.bpc;
    made.plates = {plate_of("Cyan", kCyan, true), plate_of("Magenta", kMagenta, true), plate_of("Yellow", kYellow, true)};
    return Model{made, std::nullopt, 8};
}

// Why a page's model cannot become multichannel (PhotoCraft's SOURCES: RGB, CMYK, Lab, grey, duotone; an RGB page in
// sRGB — a multichannel page takes RGB's values as inks), or none.
std::optional<std::string> not_to_multichannel(const Model& from) {
    const auto special = special_mode(from);
    if (special == "indexed" || special == "bitmap") return (*special == "indexed" ? "is an " : "is a ") + *special + " page: convert it to rgb or gray first (set_color_mode)";
    if (from.rgb && !from.rgb->profile.empty()) return "is an RGB page of a profile: convert it to sRGB first (convert_profile srgb)";
    return std::nullopt;
}

void to_multichannel(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const Json* depth = field(op, "depth");
    if (depth != nullptr && (!depth->is_number_integer() ||
                             (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16 && depth->get<std::int64_t>() != 32))) {
        throw OpError("depth is 8, 16 or 32");
    }
    const int bits = depth != nullptr ? static_cast<int>(depth->get<std::int64_t>()) : 0;  // (none: each page's own)
    Way way = way_of(op);
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = every || default_of(op);
    for (const std::size_t i : pages) {
        if (const auto why = not_to_multichannel(model_of(*doc.pages[i]))) throw OpError(page_name(*doc.pages[i]) + " " + *why);
    }
    const Model book_from{doc.color, doc.rgb, 8};
    if (book) {
        if (const auto why = not_to_multichannel(book_from)) throw OpError("the book's default mode " + *why);
    }
    Special done;
    for (const std::string& id : ids_of(doc, pages)) {
        std::size_t at = page_at(doc, id);
        const Model before = model_of(doc.page(at));
        const bool already = before.inks && before.inks->space == InkSpace::Multi;
        if (already && (bits == 0 || bits == before.depth())) continue;
        if (!already && flatten_page(c, at, &done.discarded)) ++done.flattened;  // (PhotoCraft: Multichannel flattens)
        at = page_at(doc, id);
        const Model from = model_of(doc.page(at));
        done.converted += convert_page(doc, at, multichannel_of(from, bits != 0 ? bits : from.depth()), way);
    }
    check_precise(doc);
    if (book) {
        const Model kept = multichannel_of(book_from, bits != 0 ? bits : book_from.depth());
        doc.color = kept.inks;
        doc.rgb.reset();
    }
    Json report = report_of(done.converted, way);
    done.into(report);
    c.report = std::move(report);
}

// to_indexed (PhotoCraft's Image › Mode › Indexed Color): {"op", "page"?, "palette": selective|perceptual|adaptive|exact|
// systemMac|systemWindows|web|uniform, "colors": 2..256, "forced": blackWhite|none|primaries|web, "transparency": bool,
// "dither": diffusion|none|pattern|noise, "amount": 0..100, "default"?, "clip_hdr"?} (PhotoCraft's defaults). Each page:
// its pixel layers flattened into one, the page in sRGB at 8 bits (a page of another mode, depth or profile converted
// first — its own intent and black point compensation —, and said in the report), its palette made from that layer's
// pixels and the layer mapped onto it with the dither; the page indexed (its table). A pixel of alpha under ½ takes the
// transparent index when the palette has room for one.
void to_indexed(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const auto kind = quantize::palette_kind(text_of(op, "palette", "selective"));
    if (!kind) throw OpError("palette is selective, perceptual, adaptive, exact, systemMac, systemWindows, web or uniform");
    const auto forced = quantize::forced_of(text_of(op, "forced", "blackWhite"));
    if (!forced) throw OpError("forced is blackWhite, none, primaries or web");
    const auto dither = quantize::dither_of(text_of(op, "dither", "diffusion"));
    if (!dither) throw OpError("dither is diffusion, none, pattern or noise");
    const auto colors = static_cast<std::size_t>(number_in(op, "colors", 256, 2, 256, true));
    const auto amount = static_cast<float>(number_in(op, "amount", 75, 0, 100) / 100.0);
    const bool transparency = flag_in(op, "transparency", true);
    Way way;
    way.clip_hdr = flag_in(op, "clip_hdr", false);
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = every || default_of(op);
    for (const std::size_t i : pages) {
        const auto special = special_mode(model_of(*doc.pages[i]));
        if (special == "bitmap" || special == "multichannel") {
            throw OpError(page_name(*doc.pages[i]) + " is a " + *special + " page: Indexed Color takes an RGB, grey, duotone, CMYK, Lab or indexed page");
        }
    }
    const Model book_from{doc.color, doc.rgb, 8};
    if (book) {
        const auto special = special_mode(book_from);
        if (special == "bitmap" || special == "multichannel") throw OpError("the book's default mode is " + *special + ": Indexed Color takes another");
    }
    Special done;
    Json each = Json::array();
    std::optional<core::ColorTable> first;
    for (const std::string& id : ids_of(doc, pages)) {
        std::size_t at = page_at(doc, id);
        const std::optional<std::string> flat = flatten_page(c, at, &done.discarded);
        if (flat) ++done.flattened;
        at = page_at(doc, id);
        done.converted += convert_page(doc, at, Model{}, way);  // (sRGB at 8 bits: the page's own way)
        at = page_at(doc, id);
        core::Page& page = doc.edit_page(at);
        core::Layer* layer = nullptr;
        for (core::Layer& l : page.layers) {
            if (flat && l.id == *flat && l.raster_png && !l.raster_png->empty()) layer = &l;
        }
        std::vector<quantize::Rgba> px;
        std::size_t width = 1;
        Image picture;
        if (layer != nullptr) {
            picture = open_image(*layer->raster_png, kPillowOpenLimits).convert("RGBA");
            width = static_cast<std::size_t>(picture.width());
            const std::string bytes = picture.tobytes();
            px.resize(bytes.size() / 4);
            for (std::size_t i = 0; i < px.size(); ++i) {
                quantize::Rgba p{};
                for (unsigned k = 0; k < 4; ++k) p[k] = static_cast<unsigned char>(bytes[i * 4 + k]) / 255.0f;
                if (!transparency) {  // (over white)
                    for (unsigned k = 0; k < 3; ++k) p[k] = p[k] * p[3] + (1.0f - p[3]);
                    p[3] = 1.0f;
                }
                px[i] = p;
            }
        }
        const bool clear = transparency && std::any_of(px.begin(), px.end(), [](const quantize::Rgba& p) { return p[3] < 0.5f; });
        std::vector<quantize::Rgb8> palette;
        try {
            palette = quantize::build_palette(px, *kind, clear ? std::max<std::size_t>(colors - 1, 2) : colors, *forced);
        } catch (const core::Error& error) {
            throw OpError(page_name(page) + ": " + error.what());
        }
        std::optional<std::size_t> transparent;
        if (clear && palette.size() < 256) {
            palette.push_back({255, 255, 255});
            transparent = palette.size() - 1;
        }
        if (layer != nullptr) {
            quantize::quantize(px, width, palette, *dither, amount, transparent);
            std::string bytes(px.size() * 4, '\0');
            for (std::size_t i = 0; i < px.size(); ++i) {
                for (unsigned k = 0; k < 4; ++k) bytes[i * 4 + k] = static_cast<char>(std::lround(std::clamp(px[i][k], 0.0f, 1.0f) * 255.0f));
            }
            layer->raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", picture.size(), bytes)));
            layer->raster_memo = {};
            layer->raster_relpath.reset();
        }
        core::ColorTable table;
        for (const quantize::Rgb8& colour : palette) table.colors.push_back({colour[0], colour[1], colour[2]});
        if (transparent) table.transparent = static_cast<int>(*transparent);
        RgbMode mode;
        mode.table = table;
        page.rgb = mode;
        page.color.reset();
        if (!first) first = table;
        each.push_back(Json{{"page", page.index.json()}, {"colors", table.colors.size()}, {"transparent", transparent ? Json(*transparent) : Json()}});
    }
    if (book) {  // (the pages it makes: the first page's table, or one of nothing)
        core::ColorTable table = first.value_or(core::ColorTable{});
        if (!first) {
            for (const quantize::Rgb8& colour : quantize::build_palette({}, *kind, colors, *forced)) table.colors.push_back({colour[0], colour[1], colour[2]});
        }
        RgbMode mode;
        mode.table = std::move(table);
        doc.rgb = std::move(mode);
        doc.color.reset();
    }
    Json report = each.empty() ? Json::object() : Json{{"colors", each[0]["colors"]}, {"transparent", each[0]["transparent"]}};
    done.into(report);
    report["converted"] = done.converted;
    if (way.clipped > 0) report["clipped"] = way.clipped;
    if (each.size() > 1) report["pages"] = std::move(each);
    c.report = std::move(report);
}

// The pixels of an indexed page's paint layers through a change of its table (PhotoCraft's Color Table): each pixel that
// shows takes its index in the old table — the nearest colour — and the new table's colour there (an index past the new
// table: its nearest), transparent when that index is the new transparent one. Its name and draft layers (not printed:
// their pixels are not the table's) keep theirs; the page shows them in the new table.
void retable(core::Page& page, const core::ColorTable& from, const core::ColorTable& to) {
    for (core::Layer& layer : page.layers) {
        if (!layer.raster_png || layer.raster_png->empty()) continue;
        if (layer.role == core::LayerRole::Name || layer.role == core::LayerRole::Draft) continue;
        const Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
        std::string bytes = picture.tobytes();
        for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
            auto* p = reinterpret_cast<unsigned char*>(bytes.data() + i);
            if (p[3] == 0) continue;
            const std::size_t k = from.nearest(p[0], p[1], p[2]);
            const std::array<int, 3>& c = k < to.colors.size() ? to.colors[k] : to.colors[to.nearest(p[0], p[1], p[2])];
            for (unsigned j = 0; j < 3; ++j) p[j] = static_cast<unsigned char>(c[j]);
            if (to.transparent && static_cast<std::size_t>(*to.transparent) == k) p[3] = 0;
        }
        layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", picture.size(), bytes)));
        layer.raster_memo = {};
        layer.raster_relpath.reset();
    }
}

// set_color_table (PhotoCraft's Image › Mode › Color Table): {"op", "page"?, "table": custom|blackBody|grayscale|spectrum|
// systemMac|systemWindows|web, "colors"?: ["#rrggbb" | [r, g, b], …], "entries"?: {"index": colour}, "transparent"?:
// index | null}. An indexed page's table replaced (its pixels keep their index: their colour follows the table, as
// Photoshop's). The table in the report.
void set_color_table(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string preset = text_of(op, "table", "custom");
    std::optional<std::vector<quantize::Rgb8>> named;
    if (preset != "custom") {
        named = quantize::preset_table(preset);
        if (!named) throw OpError("table is custom, blackBody, grayscale, spectrum, systemMac, systemWindows or web");
    }
    const auto changed = [&](core::ColorTable table) {
        if (named) {
            table.colors.clear();
            for (const quantize::Rgb8& colour : *named) table.colors.push_back({colour[0], colour[1], colour[2]});
        }
        if (const Json* colors = field(op, "colors"); colors != nullptr) {
            if (!colors->is_array() || colors->empty() || colors->size() > core::kColorTableMax) {
                throw OpError("colors is a list of 1..256 colours (\"#rrggbb\" or [r, g, b])");
            }
            table.colors.clear();
            for (const Json& v : *colors) {
                const auto colour = colour_value(v);
                if (!colour) throw OpError("colors is a list of 1..256 colours (\"#rrggbb\" or [r, g, b])");
                table.colors.push_back(*colour);
            }
        }
        if (const Json* entries = field(op, "entries"); entries != nullptr) {
            if (!entries->is_object()) throw OpError("entries is {\"index\": colour, …}");
            for (const auto& [key, v] : entries->items()) {
                std::size_t at = 0;
                if (key.empty() || !std::all_of(key.begin(), key.end(), [](char ch) { return ch >= '0' && ch <= '9'; }) || key.size() > 3 ||
                    (at = static_cast<std::size_t>(std::stoi(key))) >= table.colors.size()) {
                    throw OpError("entry " + core::py_repr_str(key) + " is not an index of the table (" + std::to_string(table.colors.size()) + " colours)");
                }
                const auto colour = colour_value(v);
                if (!colour) throw OpError("entry " + key + ": a colour is \"#rrggbb\" or [r, g, b]");
                table.colors[at] = *colour;
            }
        }
        if (const auto it = op.find("transparent"); it != op.end()) {
            if (it->is_null()) {
                table.transparent.reset();
            } else if (it->is_number_integer() && it->get<std::int64_t>() >= 0 && static_cast<std::size_t>(it->get<std::int64_t>()) < table.colors.size()) {
                table.transparent = static_cast<int>(it->get<std::int64_t>());
            } else {
                throw OpError("transparent is an index of the table or null");
            }
        }
        if (table.transparent && static_cast<std::size_t>(*table.transparent) >= table.colors.size()) table.transparent.reset();
        return table;
    };
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    std::optional<core::ColorTable> shown;
    for (const std::size_t i : pages) {
        const core::Page& seen = doc.page(i);
        if (!seen.rgb || !seen.rgb->table) {
            if (every) continue;  // (every page: the indexed ones)
            throw OpError(page_name(seen) + " is not an indexed page (to_indexed first)");
        }
        const core::ColorTable now = changed(*seen.rgb->table);
        if (!shown) shown = now;
        if (now == *seen.rgb->table) continue;
        core::Page& page = doc.edit_page(i);
        retable(page, *page.rgb->table, now);
        page.rgb->table = now;
    }
    if (every && doc.rgb && doc.rgb->table) {
        doc.rgb->table = changed(*doc.rgb->table);
        if (!shown) shown = doc.rgb->table;
    }
    if (!shown) throw OpError("no page is an indexed page (to_indexed first)");
    c.report = core::to_json(*shown);
}

// to_bitmap (PhotoCraft's Image › Mode › Bitmap): {"op", "page"?, "method": diffusion|threshold|pattern|halftone,
// "frequency": 1..999 (53), "angle": -180..180 (45), "shape": round|ellipse|line|square|diamond|cross, "default"?}. A grey
// page only (as PhotoCraft). Its pixel layers flattened into one, over white, its lightness made 0 or 1 by the method
// (a halftone's cell: the layer's dpi ÷ the frequency); the page a bitmap page of 8 bits (its look recorded).
void to_bitmap(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    core::BitmapLook look;
    look.method = text_of(op, "method", "diffusion");
    if (look.method == "threshold50") look.method = "threshold";
    if (std::find(core::kBitmapMethods.begin(), core::kBitmapMethods.end(), look.method) == core::kBitmapMethods.end()) {
        throw OpError("method is diffusion, threshold, pattern or halftone");
    }
    look.frequency = number_in(op, "frequency", 53, 1, 999);
    look.angle = number_in(op, "angle", 45, -180, 180);
    look.shape = text_of(op, "shape", "round");
    if (std::find(core::kHalftoneShapes.begin(), core::kHalftoneShapes.end(), look.shape) == core::kHalftoneShapes.end()) {
        throw OpError("shape is round, ellipse, line, square, diamond or cross");
    }
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = every || default_of(op);
    const auto grey_only = [](const Model& m) { return m.inks && m.inks->space == InkSpace::Gray && !m.inks->bitmap && m.inks->duotone.empty(); };
    for (const std::size_t i : pages) {
        if (!grey_only(model_of(*doc.pages[i]))) throw OpError(page_name(*doc.pages[i]) + " is not a grey page: Bitmap takes a grey page (set_color_mode gray first)");
    }
    const Model book_from{doc.color, doc.rgb, 8};
    if (book && !grey_only(book_from)) throw OpError("the book's default mode is not grey: Bitmap takes a grey page");
    Special done;
    for (const std::string& id : ids_of(doc, pages)) {
        std::size_t at = page_at(doc, id);
        const std::optional<std::string> flat = flatten_page(c, at, &done.discarded);
        if (flat) ++done.flattened;
        at = page_at(doc, id);
        Model eight = model_of(doc.page(at));
        eight.inks->depth = 8;
        Way way;
        way.clip_hdr = true;  // (HDR lightness past white is white in 1 bit)
        convert_page(doc, at, eight, way);
        at = page_at(doc, id);
        core::Page& page = doc.edit_page(at);
        for (core::Layer& layer : page.layers) {
            if (!flat || layer.id != *flat || !layer.ink_raster) continue;
            const core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
            std::vector<float> grey(pixels.pixel_count());
            for (std::size_t i = 0; i < grey.size(); ++i) {  // (over white)
                const double a = pixels.value(i, 1);
                grey[i] = static_cast<float>(std::clamp(pixels.value(i, 0), 0.0, 1.0) * a + (1.0 - a));
            }
            quantize::BitmapMethod method;
            method.kind = look.method == "threshold" ? quantize::BitmapMethod::Kind::Threshold
                          : look.method == "pattern" ? quantize::BitmapMethod::Kind::Pattern
                          : look.method == "halftone" ? quantize::BitmapMethod::Kind::Halftone
                                                       : quantize::BitmapMethod::Kind::Diffusion;
            const double width_mm = page.spec.width_mm.value();
            const double dpi = width_mm > 0 ? pixels.width / (width_mm / 25.4) : 600.0;  // (the layer's own resolution)
            method.cell = static_cast<float>(std::max(dpi / look.frequency, 2.0));
            method.angle = static_cast<float>(look.angle);
            method.shape = quantize::halftone_shape(look.shape);
            quantize::to_bitmap(grey, pixels.width, method);
            core::InkPixels out = core::blank_ink_pixels(InkSpace::Gray, 8, pixels.width, pixels.height);
            for (std::size_t i = 0; i < grey.size(); ++i) {
                out.set(i, 0, grey[i]);
                out.set(i, 1, 1.0);
            }
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(out));
            layer.ink_memo = {};
        }
        page.color->bitmap = look;
        page.color->duotone.clear();
    }
    if (book) {
        doc.color->depth = 8;
        doc.color->bitmap = look;
    }
    Json report = Json::object();
    done.into(report);
    c.report = std::move(report);
}

// PhotoCraft's default duotone inks: black, then a warm brown, gold and slate blue (its generic names; Photoshop's
// Pantone presets are not reproduced).
std::vector<core::DuotoneInk> default_inks(std::size_t n) {
    static const std::array<std::pair<const char*, std::array<int, 3>>, 4> kAll{
        {{"Black", {0, 0, 0}}, {"Warm Brown", {158, 97, 46}}, {"Gold", {219, 173, 46}}, {"Slate Blue", {69, 97, 148}}}};
    std::vector<core::DuotoneInk> out;
    for (std::size_t i = 0; i < n && i < kAll.size(); ++i) {
        core::DuotoneInk ink;
        ink.name = kAll[i].first;
        ink.color = kAll[i].second;
        out.push_back(std::move(ink));
    }
    return out;
}

// set_duotone (PhotoCraft's Image › Mode › Duotone): {"op", "page"?, "type": monotone|duotone|tritone|quadtone, "inks"?:
// [{"name", "color": "#rrggbb" | [r, g, b], "curve": [[in, out], …] (0..100)}, …], "default"?}. A grey or duotone page
// printed in 1..4 inks; its pixels as they are (set_color_mode gray takes the inks off).
void set_duotone(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string type = text_of(op, "type", "duotone");
    const std::size_t n = type == "monotone" ? 1 : type == "duotone" ? 2 : type == "tritone" ? 3 : type == "quadtone" ? 4 : 0;
    if (n == 0) throw OpError("type is monotone, duotone, tritone or quadtone");
    std::vector<core::DuotoneInk> inks;
    if (const Json* given = field(op, "inks"); given != nullptr) {
        if (!given->is_array()) throw OpError("inks is [{\"name\", \"color\", \"curve\"}, …]");
        const std::vector<core::DuotoneInk> defaults = default_inks(std::min<std::size_t>(std::max(n, given->size()), 4));
        for (std::size_t i = 0; i < given->size() && i < 4; ++i) {
            const Json& item = (*given)[i];
            if (!item.is_object()) throw OpError("inks is [{\"name\", \"color\", \"curve\"}, …]");
            core::DuotoneInk ink = i < defaults.size() ? defaults[i] : core::DuotoneInk{"Ink " + std::to_string(i + 1), {0, 0, 0}};
            if (const Json* name = field(item, "name"); name != nullptr) {
                if (!name->is_string()) throw OpError("an ink's name is text");
                ink.name = name->get<std::string>();
            }
            if (const Json* colour = field(item, "color"); colour != nullptr) {
                const auto value = colour_value(*colour);
                if (!value) throw OpError("an ink's color is \"#rrggbb\" or [r, g, b]");
                ink.color = *value;
            }
            if (const Json* curve = field(item, "curve"); curve != nullptr) {
                if (!curve->is_array()) throw OpError("an ink's curve is [[in, out], …] (0..100)");
                std::vector<core::CurvePoint> points;
                for (const Json& p : *curve) {
                    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() || p[0].is_boolean() || p[1].is_boolean()) {
                        throw OpError("an ink's curve is [[in, out], …] (0..100)");
                    }
                    points.push_back({p[0].get<double>(), p[1].get<double>()});
                }
                std::stable_sort(points.begin(), points.end(), [](const core::CurvePoint& a, const core::CurvePoint& b) { return a[0] < b[0]; });
                if (points.size() >= 2) ink.curve = std::move(points);  // (PhotoCraft: fewer points keep the ink's)
            }
            inks.push_back(std::move(ink));
        }
    } else {
        inks = default_inks(n);
    }
    if (inks.size() < n) {
        const std::vector<core::DuotoneInk> more = default_inks(n);
        for (std::size_t i = inks.size(); i < n; ++i) inks.push_back(more[i]);
    }
    inks.resize(n);
    try {
        core::check_duotone(inks);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const bool book = default_of(op);
    const auto takes = [](const std::optional<core::PageColor>& color) { return color && color->space == InkSpace::Gray && !color->bitmap; };
    for (const std::size_t i : pages) {
        if (!takes(doc.pages[i]->color)) {
            if (every) continue;  // (every page: the grey and duotone ones)
            throw OpError(page_name(*doc.pages[i]) + " is not a grey page: Duotone takes a grey or duotone page (set_color_mode gray first)");
        }
        doc.edit_page(i).color->duotone = inks;
    }
    if (every || book) {
        if (takes(doc.color)) doc.color->duotone = inks;
        else if (book) throw OpError("the book's default mode is not grey: Duotone takes a grey or duotone page");
    }
    Json names = Json::array();
    for (const core::DuotoneInk& ink : inks) names.push_back(ink.name);
    c.report = Json{{"inks", std::move(names)}};
}

}  // namespace

void register_ink_ops(core::OpRegistry& registry) {
    registry.add("set_text_gamma", set_text_gamma);
    registry.add("set_color_mode", set_color_mode);
    registry.add("set_inks", set_inks);
    registry.add("assign_profile", assign_profile);
    registry.add("convert_profile", convert_profile);
    registry.add("to_indexed", to_indexed);
    registry.add("set_color_table", set_color_table);
    registry.add("to_bitmap", to_bitmap);
    registry.add("set_duotone", set_duotone);
}

}  // namespace genko::render
