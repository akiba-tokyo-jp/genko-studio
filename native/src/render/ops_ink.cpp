// color-mode@1, color-model@1: the ops of a page's colour model (SPEC.md COLOR-02, COLOR-03): set_color_mode turns pages
// into another colour mode, depth or profile (their paint layers' pixels converted, nothing else changed),
// assign_profile names the profile their pixels are in (the pixels as they are), convert_profile converts them into a
// profile's space, set_inks sets the exact inks of RGB colours.

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/imaging.hpp"
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

// The colour model of a page or the book: its inks, or its RGB mode (none: 8 bits, sRGB).
struct Model {
    std::optional<PageColor> inks;
    std::optional<RgbMode> rgb;
    int depth() const { return inks ? inks->depth : rgb ? rgb->depth : 8; }
    RgbMode rgb_mode() const { return rgb.value_or(RgbMode{}); }
    bool operator==(const Model& other) const { return inks == other.inks && rgb == other.rgb; }
};

Model model_of(const core::Page& page) { return Model{page.color, page.rgb}; }

// How a conversion goes: greys onto K alone, 8 bits dithered, HDR values cut (counted).
struct Way {
    bool gray_on_k = false;
    bool dither = false;
    bool clip_hdr = false;
    std::int64_t clipped = 0;
};

// The settings colours come into a page with (its intent and black point compensation).
struct Into {
    std::string intent = "relative";
    bool bpc = true;
};

Into into_of(const Json& op) {
    Into out;
    out.intent = text_of(op, "intent", "relative");
    if (!core::is_intent(out.intent)) throw OpError("intent is perceptual, relative, saturation or absolute");
    if (const Json* bpc = field(op, "bpc"); bpc != nullptr) {
        if (!bpc->is_boolean()) throw OpError("bpc is true or false");
        out.bpc = bpc->get<bool>();
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
    if (!way.clip_hdr) throw OpError(where + " holds HDR values past 0..1: they would be cut (clip_hdr: true cuts them)");
    way.clipped += outside;
    for (std::size_t i = 0; i + stride <= values.size(); i += stride) {
        for (std::size_t c = 0; c < colours; ++c) values[i + c] = std::clamp(values[i + c], 0.0f, 1.0f);
    }
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
                    const double v = std::clamp(static_cast<double>(p.rgba[(std::size_t(y) * p.width + x) * 4 + c]), 0.0, 1.0) * 255.0;
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
            if (source.is_float() && core::traits(source.space).hdr && !(to.inks->space == source.space && to.inks->depth == 32)) {
                std::vector<float> lightness(source.pixel_count());
                for (std::size_t i = 0; i < source.pixel_count(); ++i) lightness[i] = static_cast<float>(source.value(i, 0));
                cut_hdr(lightness, 1, way, where);
                for (std::size_t i = 0; i < source.pixel_count(); ++i) source.set(i, 0, lightness[i]);
            }
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
        const core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
        if (depth == 8) {
            layer.raster_png = std::make_shared<const std::string>(write_png(ink::to_rgb8(pixels, *from.inks, to_rgb)));
            layer.raster_memo = {};
            layer.ink_raster = nullptr;
            layer.ink_memo = {};
            return;
        }
        RgbPixels rgb{pixels.width, pixels.height, ink::to_rgb_floats(pixels, *from.inks, to_rgb), false};
        if (depth == 16) cut_hdr(rgb.rgba, 4, way, where);
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

// Why the page cannot hold its paint layers in precise colour yet (what render::page and core::validate_color_document
// refuse), naming the first layer that cannot; nothing when it can.
std::optional<std::string> precise_blocker(const core::Page& page) {
    const auto precise = [](const core::Layer& l) { return l.color_raster != nullptr; };
    if (std::none_of(page.layers.begin(), page.layers.end(), precise)) return std::nullopt;
    for (const core::Layer& layer : page.layers) {
        const std::string where = layer_name(page, layer);
        if (layer.color_raster) {
            if (layer.role == core::LayerRole::Tone) return where + " is a tone layer";
            if (layer.stroke_count()) return where + " holds lines";
            if (!layer.patches.empty()) return where + " holds pictures laid on it";
            if (layer.panel_clip) return where + " keeps its lines inside the panels (はみ出し off)";
            if (layer.effect || layer.screen || layer.color) return where + " has an effect, a screen or a colour of its own";
        }
        if (!layer.visible) continue;
        if (layer.kind == core::LayerKind::Adjust && layer.blend != "normal") return where + " is a correction layer in a blend mode";
        if (core::py_truthy(layer.parent_id)) return where + " is in a folder";
        if (layer.kind == core::LayerKind::Tone || layer.role == core::LayerRole::Tone) return where + " is a tone layer";
    }
    return std::nullopt;
}

// Pages converted to a model (each page's own target: `target_for`), in place; how many changed. Refused as a whole:
// nothing changes when one layer cannot be converted.
std::int64_t convert_pages(Document& doc, const std::vector<std::size_t>& pages, const std::function<Model(const core::Page&)>& target_for,
                           Way& way) {
    std::int64_t converted = 0;
    for (const std::size_t i : pages) {
        const Model from = model_of(*doc.pages[i]);
        Model to = target_for(*doc.pages[i]);
        if (to.inks && from.inks && from.inks->space == to.inks->space) to.inks->inks = from.inks->inks;  // (the same space keeps its exact inks)
        if (from == to) continue;
        if (to.inks && to.inks->space == InkSpace::Lab) {  // (nothing changes when one layer cannot be composited in Lab)
            for (const core::Layer& layer : doc.pages[i]->layers) {
                if (!core::lab_blend(layer.blend)) {
                    throw OpError("page " + doc.pages[i]->index.repr() + ": the layer " + layer.id + "'s blend " + core::py_repr_str(layer.blend) +
                                  " is not one a Lab page composites");
                }
            }
        }
        core::Page& page = doc.edit_page(i);
        for (core::Layer& layer : page.layers) convert_layer(page, layer, from, to, way);
        page.color = std::move(to.inks);
        page.rgb = std::move(to.rgb);
        if (const auto why = precise_blocker(page)) throw core::Error("not_yet_ported", *why + ": a page of 16 or 32 bits cannot draw it in precise colour yet");
        ++converted;
    }
    return converted;
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

void set_color_mode(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string mode = text_of(op, "mode", "");
    if (mode == "multichannel") throw core::Error("not_yet_ported", "set_color_mode to multichannel is not in this build yet");
    if (mode != "rgb" && mode != "gray" && mode != "cmyk" && mode != "lab") throw OpError("mode is rgb, gray, cmyk or lab");
    const Json* depth = field(op, "depth");
    if (depth != nullptr && (!depth->is_number_integer() ||
                             (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16 && depth->get<std::int64_t>() != 32))) {
        throw OpError("depth is 8, 16 or 32");
    }
    const int bits = depth != nullptr ? static_cast<int>(depth->get<std::int64_t>()) : 8;
    const Model target = target_of(mode, bits, named_profile(doc, op), into_of(op));
    Way way = way_of(op);
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    const Json* also = field(op, "default");
    if (also != nullptr && !also->is_boolean()) throw OpError("default is true or false");
    const bool book = every || (also != nullptr && also->get<bool>());
    const std::int64_t converted = convert_pages(doc, pages, [&](const core::Page&) { return target; }, way);
    check_precise(doc);
    if (book) {
        doc.color = target.inks;
        doc.rgb = target.rgb;
    }
    c.report = report_of(converted, way);
}

// The mode a page is in, as an op names it (rgb, gray, cmyk, lab).
std::string_view mode_of(const Model& model) { return model.inks ? core::to_string(model.inks->space) : std::string_view("rgb"); }

void assign_profile(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const Json* profile = field(op, "profile");
    const bool none = profile != nullptr && *profile == Json("none");
    const std::optional<Named> named = none ? std::nullopt : named_profile(doc, op);
    if (!none && !named) throw OpError("profile is a built-in profile's id, sha256:… of one the book holds, or none (or give icc)");
    bool every = false;
    const std::vector<std::size_t> pages = pages_of(doc, op, every);
    // (the model with the profile in place of its own, or none when it cannot take it)
    const auto assigned = [&](const Model& from) -> std::optional<Model> {
        Model to = from;
        const std::string_view mode = mode_of(from);
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
        const std::optional<Model> to = assigned(from);
        if (!to) {
            if (every) continue;  // (every page: those of the profile's space)
            const std::string_view mode = mode_of(from);
            if (mode == "lab") throw OpError("page " + doc.pages[i]->index.repr() + " is a Lab page: it has no profile");
            if (none) throw OpError("page " + doc.pages[i]->index.repr() + " is a CMYK page: none is for an RGB or grey page");
            throw OpError("the profile is for another colour space (" + std::string(core::to_string(named->space)) + ") than page " +
                          doc.pages[i]->index.repr() + "'s (" + std::string(mode) + ")");
        }
        if (*to == from) continue;
        core::Page& page = doc.edit_page(i);
        page.color = to->inks;
        page.rgb = to->rgb;
        ++count;
    }
    if (every) {
        if (const std::optional<Model> to = assigned(Model{doc.color, doc.rgb})) {
            doc.color = to->inks;
            doc.rgb = to->rgb;
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
    // (each page at its own depth)
    const auto target = [&](const Model& from) { return target_of(mode, from.depth(), named, into); };
    const std::int64_t converted = convert_pages(doc, pages, [&](const core::Page& page) { return target(model_of(page)); }, way);
    check_precise(doc);
    if (every) {
        const Model book = target(Model{doc.color, doc.rgb});
        doc.color = book.inks;
        doc.rgb = book.rgb;
    }
    c.report = report_of(converted, way);
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
    registry.add("assign_profile", assign_profile);
    registry.add("convert_profile", convert_profile);
}

}  // namespace genko::render
