// PC2c (SPEC.md COLOR-04): the blends, the Blending of Apply Image and Calculations, the fade's mix (PhotoCraft v0.2.0
// color/blend.rs, engine/channel_cmds.rs:403-427, engine/edit_menu_cmds.rs:152-190, MIT: third_party/photocraft/NOTICE.md)
// and a paint layer's pixels of each kind as the PC2c ops read and write them (render/image_ops.hpp).

#include "render/image_ops.hpp"

#include <algorithm>
#include <cmath>

#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render::image_ops {

using core::InkSpace;
using core::Json;
using core::OpError;

// --- the blends (color/blend.rs) ----------------------------------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 20> kBlends{"normal",     "multiply",   "screen",     "add",        "overlay",
                                                   "darken",     "lighten",    "color_burn", "color_dodge", "linear_burn",
                                                   "soft_light", "hard_light", "difference", "exclusion",  "subtract",
                                                   "divide",     "hue",        "saturation", "color",      "luminosity"};

bool separable(std::string_view mode) { return mode != "hue" && mode != "saturation" && mode != "color" && mode != "luminosity"; }

float color_burn(float cb, float cs) {
    if (cb >= 1.0f) return 1.0f;
    if (cs <= 0.0f) return 0.0f;
    return 1.0f - std::min((1.0f - cb) / cs, 1.0f);
}
float color_dodge(float cb, float cs) {
    if (cb <= 0.0f) return 0.0f;
    if (cs >= 1.0f) return 1.0f;
    return std::min(cb / (1.0f - cs), 1.0f);
}
float hard_light(float cb, float cs) {
    if (cs <= 0.5f) return cb * 2.0f * cs;
    const float s = 2.0f * cs - 1.0f;
    return cb + s - cb * s;
}
// Photoshop's Soft Light (not W3C's in the upper half)
float soft_light(float cb, float cs) {
    return cs <= 0.5f ? 2.0f * cb * cs + cb * cb * (1.0f - 2.0f * cs) : 2.0f * cb * (1.0f - cs) + std::sqrt(std::max(cb, 0.0f)) * (2.0f * cs - 1.0f);
}

using Rgb = std::array<float, 3>;
float lum(const Rgb& c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
Rgb clip_color(const Rgb& c) {
    const float l = lum(c);
    const float n = std::min({c[0], c[1], c[2]});
    const float x = std::max({c[0], c[1], c[2]});
    Rgb out = c;
    if (n < 0.0f) {
        const float d = l - n;
        for (float& v : out) v = std::fabs(d) < 1e-12f ? l : l + (v - l) * l / d;
    }
    if (x > 1.0f) {
        const float d = x - l;
        for (float& v : out) v = std::fabs(d) < 1e-12f ? l : l + (v - l) * (1.0f - l) / d;
    }
    return out;
}
Rgb set_lum(const Rgb& c, float l) {
    const float d = l - lum(c);
    return clip_color({c[0] + d, c[1] + d, c[2] + d});
}
float sat(const Rgb& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
Rgb set_sat(const Rgb& c, float s) {
    std::array<std::size_t, 3> idx{0, 1, 2};
    std::stable_sort(idx.begin(), idx.end(), [&c](std::size_t a, std::size_t b) { return c[a] < c[b]; });
    Rgb out{};
    const std::size_t imin = idx[0], imid = idx[1], imax = idx[2];
    if (c[imax] > c[imin]) {
        out[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
        out[imax] = s;
    }
    out[imin] = 0.0f;
    return out;
}

}  // namespace

bool is_blend(std::string_view mode) { return std::find(kBlends.begin(), kBlends.end(), mode) != kBlends.end(); }

float blend_channel(std::string_view mode, float cb, float cs) {
    if (mode == "multiply") return cb * cs;
    if (mode == "screen") return cb + cs - cb * cs;
    if (mode == "add") return std::min(cb + cs, 1.0f);
    if (mode == "overlay") return hard_light(cs, cb);
    if (mode == "darken") return std::min(cb, cs);
    if (mode == "lighten") return std::max(cb, cs);
    if (mode == "color_burn") return color_burn(cb, cs);
    if (mode == "color_dodge") return color_dodge(cb, cs);
    if (mode == "linear_burn") return std::max(cb + cs - 1.0f, 0.0f);
    if (mode == "soft_light") return soft_light(cb, cs);
    if (mode == "hard_light") return hard_light(cb, cs);
    if (mode == "difference") return std::fabs(cb - cs);
    if (mode == "exclusion") return cb + cs - 2.0f * cb * cs;
    if (mode == "subtract") return std::max(cb - cs, 0.0f);
    if (mode == "divide") {
        if (cs <= 0.0f) return cb <= 0.0f ? 0.0f : 1.0f;
        return std::min(cb / cs, 1.0f);
    }
    return cs;  // (normal; a non-separable mode: blend_rgb)
}

std::array<float, 3> blend_rgb(std::string_view mode, const Rgb& cb, const Rgb& cs) {
    if (mode == "hue") return set_lum(set_sat(cs, sat(cb)), lum(cb));
    if (mode == "saturation") return set_lum(set_sat(cb, sat(cs)), lum(cb));
    if (mode == "color") return set_lum(cs, lum(cb));
    if (mode == "luminosity") return set_lum(cb, lum(cs));
    return {blend_channel(mode, cb[0], cs[0]), blend_channel(mode, cb[1], cs[1]), blend_channel(mode, cb[2], cs[2])};
}

Rgba composite(std::string_view mode, const Rgba& backdrop, const Rgba& source, float opacity) {
    const float ab = backdrop[3];
    const float as = source[3] * opacity;
    if (as <= 0.0f) return backdrop;
    const Rgb cb{backdrop[0], backdrop[1], backdrop[2]};
    const Rgb cs{source[0], source[1], source[2]};
    const Rgb b = blend_rgb(mode, cb, cs);
    const float ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) return Rgba{0, 0, 0, 0};
    Rgba out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float premul = (1.0f - as) * ab * cb[i] + (1.0f - ab) * as * cs[i] + as * ab * b[i];
        out[i] = premul / ao;
    }
    out[3] = ao;
    return out;
}

// --- Apply Image / Calculations (channel_cmds.rs:403-427) --------------------------------------------------------------

float Blending::apply(float base, float top) const {
    float out = 0;
    switch (kind) {
        case Kind::Mode: out = blend_channel(mode, base, top); break;
        case Kind::Add: out = (base + top) / scale + offset; break;
        case Kind::Subtract: out = (base - top) / scale + offset; break;
    }
    return std::clamp(out, 0.0f, 1.0f);
}

Blending blending_of(const Json& op) {
    const auto number = [&op](const char* key, double fallback, double lo, double hi) {
        const Json* v = core::get(op, key);
        if (v == nullptr || v->is_null()) return fallback;
        if (!v->is_number() || v->is_boolean()) throw OpError(std::string(key) + " must be a number");
        const double x = v->get<double>();
        if (!std::isfinite(x)) throw OpError(std::string(key) + " must be finite");
        return std::clamp(x, lo, hi);
    };
    Blending out;
    const Json* name = core::get(op, "blending");
    const std::string mode = name == nullptr || name->is_null() ? std::string("multiply") : name->is_string() ? name->get<std::string>() : std::string();
    if (name != nullptr && !name->is_null() && !name->is_string()) throw OpError("blending must be a blend's name");
    out.scale = static_cast<float>(number("scale", 1, 1, 2));
    out.offset = static_cast<float>(number("offset", 0, -255, 255)) / 255.0f;
    if (mode == "add") {
        out.kind = Blending::Kind::Add;
    } else if (mode == "subtract") {
        out.kind = Blending::Kind::Subtract;
    } else if (is_blend(mode) && separable(mode)) {
        out.kind = Blending::Kind::Mode;
    } else if (is_blend(mode)) {
        throw OpError("blending " + core::py_repr_str(mode) + " is not separable (a blend of each channel by itself, add or subtract)");
    } else {
        throw OpError("blending must be one of normal, multiply, screen, add, overlay, darken, lighten, color_burn, color_dodge, linear_burn, "
                      "soft_light, hard_light, difference, exclusion, subtract, divide");
    }
    out.mode = mode;
    return out;
}

// --- Fade (edit_menu_cmds.rs:152-190) -------------------------------------------------------------------------------

void fade_values(const float* pb, const float* pa, float* out, std::size_t m, bool alpha, float k) {
    if (!alpha) {
        for (std::size_t c = 0; c < m; ++c) out[c] = pb[c] + (pa[c] - pb[c]) * k;
        return;
    }
    const float ab = pb[m - 1], aa = pa[m - 1];
    const float ao = ab + (aa - ab) * k;
    for (std::size_t c = 0; c + 1 < m; ++c) {
        const float pm = pb[c] * ab + (pa[c] * aa - pb[c] * ab) * k;
        out[c] = ao > 1e-9f ? pm / ao : 0.0f;
    }
    out[m - 1] = ao;
}

Rgba fade_rgba(const Rgba& before, const Rgba& after, std::string_view mode, float k) {
    Rgba out{};
    if (mode == "normal") {
        fade_values(before.data(), after.data(), out.data(), 4, true, k);
        return out;
    }
    const Rgba blended = composite(mode, before, after, 1.0f);
    fade_values(before.data(), blended.data(), out.data(), 4, true, k);
    return out;
}

// --- a paint layer's pixels ----------------------------------------------------------------------------------------------

struct Pixels::Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};

Pixels::Pixels(Kind kind, std::uint32_t width, std::uint32_t height, unsigned colours)
    : kind_(kind), width_(width), height_(height), colours_(colours), work_(std::make_unique<Work>()) {
    const std::uint64_t n = std::uint64_t(width) * height * (colours + 1);
    if (!genko_imaging_budget_reserve(&work_->reservation, n * sizeof(float))) detail::throw_imaging_error();
    v_.assign(static_cast<std::size_t>(n), 0.0f);
}
Pixels::Pixels(const Pixels& other) : Pixels(other.kind_, other.width_, other.height_, other.colours_) {
    v_ = other.v_;
    floating = other.floating;
    ink = other.ink;
}
Pixels::Pixels(Pixels&&) noexcept = default;
Pixels& Pixels::operator=(Pixels&&) noexcept = default;
Pixels::~Pixels() = default;

Pixels from_image(const Image& picture) {
    const Image rgba = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    Pixels p(Pixels::Kind::Eight, static_cast<std::uint32_t>(rgba.width()), static_cast<std::uint32_t>(rgba.height()), 3);
    const std::string bytes = rgba.tobytes();
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        for (unsigned c = 0; c < 4; ++c) v[c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0f;
    }
    return p;
}

Image to_image(const Pixels& p) {
    std::string bytes(p.size() * 4, '\0');
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float* v = p.at(i);
        for (unsigned c = 0; c < 4; ++c) {
            const float x = std::isfinite(v[c]) ? std::clamp(v[c], 0.0f, 1.0f) : 0.0f;
            bytes[i * 4 + c] = static_cast<char>(std::lround(x * 255.0f));
        }
    }
    return Image::frombytes("RGBA", Size{static_cast<int>(p.width()), static_cast<int>(p.height())}, bytes);
}

Pixels from_precise(const std::string& raster) {
    const core::ColorRasterView view(raster);
    Pixels p(Pixels::Kind::Precise, view.width(), view.height(), 3);
    p.floating = view.metadata("")["precision"] == "f32";
    for (std::size_t i = 0; i < p.size(); ++i) {
        const auto px = view.pixel(i);
        float* v = p.at(i);
        for (unsigned c = 0; c < 4; ++c) v[c] = static_cast<float>(px[c]);
    }
    return p;
}

std::string to_precise(const Pixels& p) {
    return core::encode_color_pixels(p.width(), p.height(), p.floating ? "f32" : "u16", [&](std::size_t i) {
        std::array<double, 4> out{};
        const float* v = p.at(i);
        for (unsigned c = 0; c < 4; ++c) {
            if (!std::isfinite(v[c])) throw core::Error("value", "the result exceeds the finite working range");
            out[c] = p.floating && c < 3 ? static_cast<double>(v[c]) : std::clamp(static_cast<double>(v[c]), 0.0, 1.0);
        }
        return out;
    });
}

Pixels from_inks(const core::InkPixels& r, const core::PageColor& color) {
    const unsigned n = r.colours();
    Pixels p(Pixels::Kind::Inks, r.width, r.height, n);
    p.ink = color;
    if (r.is_float()) {  // (32 bits: the canvas's values themselves)
        for (std::size_t i = 0; i < p.size(); ++i) {
            float* v = p.at(i);
            for (unsigned c = 0; c <= n; ++c) v[c] = static_cast<float>(r.value(i, c));
        }
        return p;
    }
    const double most = r.max();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const std::uint16_t* s = r.samples.data() + i * (n + 1);
        float* v = p.at(i);
        for (unsigned c = 0; c < n; ++c) v[c] = static_cast<float>(to_canvas(r.space, c, ink::decode_sample(r.space, c, s[c], r.depth)));
        v[n] = static_cast<float>(s[n] / most);
    }
    return p;
}

std::string to_inks(const Pixels& p) {
    const core::PageColor& color = *p.ink;
    const InkSpace space = color.space;
    const int depth = color.depth;
    const unsigned n = p.colours();
    core::InkPixels out = core::blank_ink_pixels(space, depth, p.width(), p.height(), space == InkSpace::Multi ? n : 0);
    const double most = out.max();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float* v = p.at(i);
        for (unsigned c = 0; c <= n; ++c) {
            if (!std::isfinite(v[c])) throw core::Error("value", "the result exceeds the finite working range");
        }
        if (depth == 32) {  // (floats: a grey's HDR kept, the rest 0..1; clear as a blank raster holds it)
            if (!(v[n] > 0)) continue;
            for (unsigned c = 0; c < n; ++c) out.set(i, c, v[c]);
            out.set(i, n, v[n]);
            continue;
        }
        const auto alpha = static_cast<std::uint16_t>(std::lround(std::clamp(static_cast<double>(v[n]), 0.0, 1.0) * most));
        if (alpha == 0) continue;  // (clear: as a blank raster holds it)
        std::uint16_t* s = out.samples.data() + i * (n + 1);
        for (unsigned c = 0; c < n; ++c) s[c] = ink::encode_sample(space, c, from_canvas(space, c, std::clamp(static_cast<double>(v[c]), 0.0, 1.0)), depth);
        s[n] = alpha;
    }
    return core::encode_ink_raster(out);
}

namespace {

std::size_t layer_at(const core::Page& page, const std::string& id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == id) return i;
    }
    throw OpError("no layer " + id);
}

}  // namespace

Pixels layer_pixels(core::OpContext& c, std::size_t at, const std::string& id) {
    core::Document& doc = c.doc;
    {
        const core::Layer& seen = doc.page(at).layers[layer_at(doc.page(at), id)];
        if (seen.locked) throw OpError("the layer is locked");
        if (seen.kind != core::LayerKind::Raster && seen.kind != core::LayerKind::Strokes)
            throw OpError("the layer " + id + " is not a paint layer (pen or paint)");
        // (precise pen lines become precise pixels, as filter_raster makes them)
        if (!doc.page(at).color && core::has_color_strokes(seen)) {
            const Json convert{{"op", "convert_layer"}, {"page", doc.page(at).index.json()}, {"id", id}, {"to", "paint"}};
            core::OpContext inner{doc, convert, c.actor};
            (*ops_registry().find("convert_layer"))(inner);
        }
    }
    core::Page& page = doc.edit_page(at);
    core::Layer& layer = page.layers[layer_at(page, id)];
    if (layer.color_raster) {
        if (layer.stroke_count() > 0 || !layer.patches.empty()) throw OpError("the lines of the layer " + id + " are not in its precise pixels yet");
        return from_precise(*layer.color_raster);
    }
    if (page.color && !layer.raster_png) return from_inks(ink_edit::with_marks(page, layer, &doc), *page.color);
    // (pen lines and fills become pixels, as filter_raster bakes them; an RGB page of a profile's: its values)
    if (layer.stroke_count() > 0 || !layer.patches.empty()) raster::bake_vectors(page, layer, core::profiled_rgb(page));
    return from_image(raster::layer_pixels(page, layer));
}

void put_layer_pixels(core::Document& doc, std::size_t at, const std::string& id, const Pixels& p) {
    core::Page& page = doc.edit_page(at);
    core::Layer& layer = page.layers[layer_at(page, id)];
    switch (p.kind()) {
        case Pixels::Kind::Eight:
            raster::save_raster(page, layer, to_image(p));
            break;
        case Pixels::Kind::Precise:
            layer.color_raster = std::make_shared<const std::string>(to_precise(p));
            break;
        case Pixels::Kind::Inks:
            layer.ink_raster = std::make_shared<const std::string>(to_inks(p));
            layer.ink_memo = {};
            layer.strokes = core::empty_strokes();  // (its marks are in its inks: as filter_raster leaves it)
            layer.patches.clear();
            break;
    }
    layer.kind = core::LayerKind::Raster;
}

std::vector<double> meant(const Pixels& p) {
    const core::PageColor& color = *p.ink;
    const unsigned n = p.colours();
    std::vector<double> inks(p.size() * n);
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(color.space, c, p.at(i)[c]);
    std::vector<double> rgb(p.size() * 3);
    ink::Rules(color).meaning(inks.data(), rgb.data(), p.size());
    return rgb;
}

void from_meant(Pixels& p, const std::vector<double>& rgb) {
    const core::PageColor& color = *p.ink;
    const unsigned n = p.colours();
    std::vector<double> inks(p.size() * n), out(p.size() * n);
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(color.space, c, p.at(i)[c]);
    std::size_t k = 0;  // (asked for each pixel once, in their order)
    ink::Rules(color).correct(inks.data(), out.data(), p.size(), [&](const std::array<double, 3>&) {
        const std::size_t i = k++;
        return std::array<double, 3>{rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
    });
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < n; ++c) p.at(i)[c] = static_cast<float>(to_canvas(color.space, c, out[i * n + c]));
}

Pixels page_pixels(const core::Document& doc, const core::Page& page) {
    RenderOptions options;
    options.mode = "print";
    options.finish = false;
    options.onion = false;
    options.spots = std::vector<std::string>{};
    const bool deep = !page.color && page.rgb && (page.rgb->depth != 8 || !page.rgb->profile.empty());
    options.inks = page.color.has_value();
    options.precise = deep;
    core::Page shown = page;  // (without its nombre)
    shown.numero = false;
    const RenderResult made = render_page(shown, raster::kWorkingDpi, options, &doc);
    if (page.color && made.inks) return from_inks(*made.inks, *page.color);
    if (deep && made.precise) return from_precise(*made.precise);
    return from_image(made.image);
}

Pixels shown_layer_pixels(const core::Document& doc, const core::Page& page, const core::Layer& layer) {
    const bool lines = layer.stroke_count() > 0 || !layer.patches.empty();
    if (layer.color_raster && !lines) return from_precise(*layer.color_raster);
    if (page.color && !layer.raster_png && (layer.ink_raster || layer.kind == core::LayerKind::Raster || layer.kind == core::LayerKind::Strokes))
        return from_inks(ink_edit::with_marks(page, layer, &doc), *page.color);
    if ((layer.kind == core::LayerKind::Raster || layer.kind == core::LayerKind::Strokes) && !layer.color_raster)
        return from_image(raster::layer_pixels(page, layer));
    return from_image(layer_image(page, layer, raster::kWorkingDpi, &doc, true));
}

std::vector<Rgba> rgba_of(const Pixels& p) {
    std::vector<Rgba> out(p.size());
    if (p.kind() == Pixels::Kind::Inks) {
        const std::vector<double> rgb = meant(p);
        for (std::size_t i = 0; i < p.size(); ++i)
            out[i] = Rgba{static_cast<float>(rgb[i * 3]), static_cast<float>(rgb[i * 3 + 1]), static_cast<float>(rgb[i * 3 + 2]), p.at(i)[p.colours()]};
        return out;
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float* v = p.at(i);
        out[i] = Rgba{v[0], v[1], v[2], v[3]};
    }
    return out;
}

std::vector<float> coverage(const core::Page& page, const Json& area, std::uint32_t width, std::uint32_t height, bool eight) {
    std::vector<float> out(std::size_t(width) * height, 0.0f);
    if (eight) {  // (as filter_raster's 8-bit way: the area at the working dpi over a picture of the layer's size)
        const selection::AreaMask inside = selection::area_mask(area, raster::kWorkingDpi);
        Image mask = Image::create("L", Size{static_cast<int>(width), static_cast<int>(height)}, Ink(0));
        mask.paste(inside.mask, Point{static_cast<int>(inside.x0), static_cast<int>(inside.y0)});
        const std::string bytes = mask.tobytes();
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
        return out;
    }
    const std::string bytes = ink_edit::area_coverage(page, width, height, area);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

double grid_scale(const core::Page& page, std::uint32_t width, std::uint32_t height) {
    // (the page at the working dpi, as an 8-bit paint layer's pixels are: its pixels are those, exactly)
    if (static_cast<int>(width) == mm_to_px(page.spec.width_mm.value(), raster::kWorkingDpi) &&
        static_cast<int>(height) == mm_to_px(page.spec.height_mm.value(), raster::kWorkingDpi))
        return 1.0;
    return ink_edit::raster_scale(page, width, height);
}

}  // namespace genko::render::image_ops
