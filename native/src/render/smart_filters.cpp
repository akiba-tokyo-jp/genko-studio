// A smart object's smart filters drawn (render/smart_filters.hpp, PC3 A2). Upstream PhotoCraft v0.2.0's
// engine/src/smart_cmds.rs:236-308 (blend_surfaces, mask_mix, apply_smart_filters: each visible filter on what is below it,
// laid over it in its blend mode at its opacity — normal at 100 % takes its place —; the filter mask mixing the unfiltered
// and the filtered picture, premultiplied by their alphas; MIT, see third_party/photocraft/NOTICE.md), worked in Genko's
// three ways with Genko's filters.

#include "render/smart_filters.hpp"

#include <algorithm>
#include <cmath>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/channels.hpp"
#include "render/color_filters.hpp"
#include "render/fill.hpp"
#include "render/ink.hpp"
#include "render/ink_filters.hpp"
#include "render/ops_adjust.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/resample.hpp"
#include "render/selection.hpp"
#include "render/smart_bends.hpp"
#include "storage/asset_store.hpp"

namespace genko::render::smart_filters {

using core::Json;

namespace {

constexpr int kMaskDpi = 150;  // (a layer's mask: ops.MASK_DPI)
constexpr int kBand = 256;     // (rows mixed at a time: a canvas of a band, not of the page)

// Python's ValueError and TypeError (and an op's own refusal) of a filter's settings become the op's error, as
// filter_raster makes them.
template <class Work>
auto refused(const Work& work) {
    try {
        return work();
    } catch (const core::PyUncaught&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() != "value" && error.code() != "type") throw;
        throw core::OpError(error.what());
    }
}

// The page the filters work on: the smart object's page's paper (their sizes in pixels scaled to the picture's
// resolution as on a layer of that page), and its colours where the way works in them.
core::Page stand_in(const core::Page& page, bool rgb, bool inks) {
    core::Page out;
    out.id = page.id;
    out.index = page.index;
    out.spec = page.spec;
    out.binding = page.binding;
    if (rgb) out.rgb = page.rgb;
    if (inks) out.color = page.color;
    return out;
}

Json op_of(const Filter& f) {
    Json op = f.params.is_object() ? f.params : Json::object();
    op["kind"] = f.kind;
    return op;
}

// One filter on a precise layer's raster (its new GKCR bytes): filter_raster's way.
std::string precise_step(const core::Document& doc, const core::Page& work, const core::Layer& layer, const Filter& f) {
    return refused([&] {
        std::optional<adjust_ops::FilterWork> own;
        if (adjust_ops::handles(op_of(f))) own = adjust_ops::filter_work(doc, work, f.kind, f.params);
        if (own && !own->python) return adjust_ops::filter_precise(*own, doc, work, layer, f.kind, nullptr);
        return color_filters::apply(work, layer, f.kind, own ? own->params : f.params, nullptr);
    });
}

// One filter on ink pixels (their new GKIR bytes): filter_raster's way on a page of inks.
std::string ink_step(const core::Document& doc, const core::Page& work, const core::InkPixels& inks, const Filter& f) {
    return refused([&] {
        std::optional<adjust_ops::FilterWork> own;
        if (adjust_ops::handles(op_of(f))) own = adjust_ops::filter_work(doc, work, f.kind, f.params);
        if (own && !own->python) return adjust_ops::filter_inks(*own, doc, work, inks, f.kind, nullptr);
        return ink_filters::apply(work, inks, f.kind, own ? own->params : f.params, nullptr);
    });
}

core::Layer raster_layer(std::string raster) {
    core::Layer layer;
    layer.id = "smart_filter";
    layer.kind = core::LayerKind::Raster;
    layer.color_raster = std::make_shared<const std::string>(std::move(raster));
    return layer;
}

bool replaces(const Filter& f) { return (f.blend.empty() || f.blend == "normal") && f.opacity >= 1; }

// --- the 8-bit page: through 16 bits ---------------------------------------------------------------------------------

Image step_8bit(const core::Document& doc, const core::Page& work, const Filter& f, const Image& rgba) {
    const int w = rgba.width(), h = rgba.height();
    const std::string in = rgba.tobytes();
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const core::Layer layer = raster_layer(core::encode_color_pixels(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), "u16", [&](std::size_t i) {
        return std::array<double, 4>{p[i * 4] / 255.0, p[i * 4 + 1] / 255.0, p[i * 4 + 2] / 255.0, p[i * 4 + 3] / 255.0};
    }));
    const std::string out = precise_step(doc, work, layer, f);
    const core::ColorRasterView view(out);
    if (view.width() != static_cast<std::uint32_t>(w) || view.height() != static_cast<std::uint32_t>(h))
        throw core::Error("value", "a smart filter changed the picture's size");
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        const std::array<double, 4> v = view.pixel(i);
        for (unsigned c = 0; c < 4; ++c) bytes[i * 4 + c] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v[c], 0.0, 1.0) * 255.0)));
    }
    return Image::frombytes("RGBA", Size{w, h}, bytes);
}

// The filtered picture through the mask over the unfiltered one, premultiplied (PhotoCraft's mask_mix).
template <class Get, class Put>
void mix_through(std::size_t count, unsigned colours, const Image& mask, const Get& base, const Get& filtered, const Put& put) {
    const std::string m = mask.tobytes();
    std::vector<double> b(colours + 1), f(colours + 1), o(colours + 1);
    for (std::size_t i = 0; i < count; ++i) {
        const double k = static_cast<unsigned char>(m[i]) / 255.0;
        base(i, b.data());
        filtered(i, f.data());
        const double ba = b[colours], fa = f[colours];
        const double oa = ba + (fa - ba) * k;
        o[colours] = oa;
        for (unsigned c = 0; c < colours; ++c) {
            const double pm = b[c] * ba + (f[c] * fa - b[c] * ba) * k;
            o[c] = oa > 0 ? pm / oa : 0.0;
        }
        put(i, o.data());
    }
}

// --- inks: the canvas's values as ink pixels and back --------------------------------------------------------------

core::InkPixels pixels_of(const InkPicture& picture, const core::PageColor& color) {
    const unsigned n = picture.channels;
    core::InkPixels out = core::blank_ink_pixels(color.space, color.depth, static_cast<std::uint32_t>(picture.size.width),
                                                 static_cast<std::uint32_t>(picture.size.height), color.planes());
    const std::size_t count = std::size_t(picture.size.width) * picture.size.height;
    const unsigned all = out.channels();
    for (std::size_t i = 0; i < count; ++i) {
        const double* v = picture.values.data() + i * (n + 1);
        for (unsigned c = 0; c < n && c < out.colours(); ++c) {
            const double ink = from_canvas(color.space, c, v[c]);
            if (out.is_float()) out.floats[i * all + c] = static_cast<float>(ink);
            else out.samples[i * all + c] = ink::encode_sample(color.space, c, ink, color.depth);
        }
        if (out.has_alpha()) out.set(i, out.colours(), v[n]);
    }
    return out;
}

// --- the filters that bend the picture (PC3 A5, render/smart_bends): each way's values as a plane and back ----------------

// The box of what a picture of `size` shows (an alpha above 0), none when it shows nothing.
template <class Alpha>
std::optional<Box> shown_box(Size size, const Alpha& alpha) {
    Box b{size.width, size.height, 0, 0};
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            if (!(alpha(std::size_t(y) * size.width + x) > 0)) continue;
            b.x0 = std::min(b.x0, x);
            b.y0 = std::min(b.y0, y);
            b.x1 = std::max(b.x1, x + 1);
            b.y1 = std::max(b.y1, y + 1);
        }
    }
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return std::nullopt;
    return b;
}

Image bend_8bit(const core::Page& page, const Filter& f, const Image& rgba) {
    const Size size = rgba.size();
    const auto read = [&rgba](int x0, int y0, int x1, int y1) { return resample::from_rgba8(rgba.crop(Box{x0, y0, x1, y1})); };
    const auto bent = smart_bends::bend(page, f.kind, f.params, size, rgba.getbbox(), read);
    if (!bent) return rgba;
    Image out = bent->whole ? Image::create("RGBA", size, Ink{0, 0, 0, 0}) : rgba;
    if (bent->plane.width > 0 && bent->plane.height > 0) out.paste(resample::to_rgba8(bent->plane), Point{bent->x0, bent->y0});
    return out;
}

ColorPicture bend_precise(const core::Page& page, const Filter& f, const ColorPicture& picture) {
    const Size size = picture.size;
    const auto read = [&](int x0, int y0, int x1, int y1) {
        resample::Plane plane(x1 - x0, y1 - y0, 4, true);
        plane.hdr = true;  // (linear light: a 32-bit page's past 1 kept)
        for (int y = std::max(0, y0); y < std::min(size.height, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(size.width, x1); ++x) {
                const auto& p = picture.pixels[std::size_t(y) * size.width + x];
                float* v = plane.at(x - x0, y - y0);
                for (unsigned c = 0; c < 4; ++c) v[c] = static_cast<float>(p[c]);
            }
        }
        return plane;
    };
    const auto shown = shown_box(size, [&](std::size_t i) { return picture.pixels[i][3]; });
    const auto bent = smart_bends::bend(page, f.kind, f.params, size, shown, read);
    if (!bent) return picture;
    ColorPicture out = bent->whole ? ColorPicture{size, std::vector<std::array<double, 4>>(picture.pixels.size(), {0, 0, 0, 0})} : picture;
    for (int y = 0; y < bent->plane.height; ++y) {
        for (int x = 0; x < bent->plane.width; ++x) {
            const int px = bent->x0 + x, py = bent->y0 + y;
            if (px < 0 || py < 0 || px >= size.width || py >= size.height) continue;
            const float* v = bent->plane.at(x, y);
            out.pixels[std::size_t(py) * size.width + px] = {v[0], v[1], v[2], v[3]};
        }
    }
    return out;
}

InkPicture bend_inks(const core::Page& page, const Filter& f, const InkPicture& picture) {
    const Size size = picture.size;
    const unsigned n = picture.channels;
    const auto read = [&](int x0, int y0, int x1, int y1) {
        resample::Plane plane(x1 - x0, y1 - y0, static_cast<int>(n + 1), true);
        for (int y = std::max(0, y0); y < std::min(size.height, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(size.width, x1); ++x) {
                const double* p = picture.values.data() + (std::size_t(y) * size.width + x) * (n + 1);
                float* v = plane.at(x - x0, y - y0);
                for (unsigned c = 0; c <= n; ++c) v[c] = static_cast<float>(p[c]);
            }
        }
        return plane;
    };
    const auto shown = shown_box(size, [&](std::size_t i) { return picture.values[i * (n + 1) + n]; });
    const auto bent = smart_bends::bend(page, f.kind, f.params, size, shown, read);
    if (!bent) return picture;
    InkPicture out = bent->whole ? InkPicture{size, n, std::vector<double>(picture.values.size(), 0.0)} : picture;
    for (int y = 0; y < bent->plane.height; ++y) {
        for (int x = 0; x < bent->plane.width; ++x) {
            const int px = bent->x0 + x, py = bent->y0 + y;
            if (px < 0 || py < 0 || px >= size.width || py >= size.height) continue;
            const float* v = bent->plane.at(x, y);
            std::copy_n(v, n + 1, out.values.data() + (std::size_t(py) * size.width + px) * (n + 1));
        }
    }
    return out;
}

InkPicture picture_of(const core::InkPixels& inks, unsigned channels) {
    InkPicture out{Size{static_cast<int>(inks.width), static_cast<int>(inks.height)}, channels,
                   std::vector<double>(inks.pixel_count() * (channels + 1), 0.0)};
    const unsigned n = inks.colours(), all = inks.channels();
    for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
        double* to = out.values.data() + i * (channels + 1);
        for (unsigned c = 0; c < n && c < channels; ++c) {
            const double ink = inks.is_float() ? inks.floats[i * all + c] : ink::decode_sample(inks.space, c, inks.samples[i * all + c], inks.depth);
            to[c] = to_canvas(inks.space, c, ink);
        }
        to[channels] = inks.has_alpha() ? inks.value(i, n) : 1.0;
    }
    return out;
}

}  // namespace

std::vector<Filter> shown(const core::Layer& layer) {
    std::vector<Filter> out;
    if (!core::smart::filters_drawn(layer)) return out;
    for (const Json& f : (*layer.smart)["filters"]) {
        if (!f.is_object()) continue;
        if (const Json* visible = core::get(f, "visible"); visible != nullptr && visible->is_boolean() && !visible->get<bool>()) continue;
        Filter one;
        one.kind = core::py_str(core::py_get(f, "kind", Json("")));
        if (const Json* params = core::get(f, "params"); params != nullptr && params->is_object()) one.params = *params;
        if (const Json* blend = core::get(f, "blend"); blend != nullptr && blend->is_string()) one.blend = blend->get<std::string>();
        if (const Json* opacity = core::get(f, "opacity"); opacity != nullptr && opacity->is_number()) one.opacity = std::clamp(opacity->get<double>(), 0.0, 1.0);
        out.push_back(std::move(one));
    }
    return out;
}

std::optional<std::string> mask_ref(const core::Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return std::nullopt;
    const Json* mask = core::get(*layer.smart, "filter_mask");
    if (mask == nullptr || !mask->is_object()) return std::nullopt;
    if (const Json* enabled = core::get(*mask, "enabled"); enabled != nullptr && enabled->is_boolean() && !enabled->get<bool>()) return std::nullopt;
    const Json* asset = core::get(*mask, "asset");
    if (asset == nullptr || !asset->is_string()) return std::nullopt;
    return asset->get<std::string>();
}

core::Bytes mask_bytes(const core::Document& doc, const std::string& ref) {
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) return held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(ref)) return nullptr;
    auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(ref, ".png");
    if (!bytes) return nullptr;
    return std::make_shared<const std::string>(std::move(*bytes));
}

Image mask_picture(const core::Document& doc, const std::string& ref, Size size) {
    const core::Bytes bytes = mask_bytes(doc, ref);
    if (!bytes) throw core::Error("missing_asset", "the smart object's filter mask " + ref + " is not in the book");
    const Image mask = read_png(*bytes, kPillowOpenLimits).convert("L");
    return mask.resize_region(size, Box{0, 0, size.width, size.height}, Resample::Bilinear);
}

Image on_8bit(const core::Document& doc, const core::Page& page, const core::Layer& layer, const Image& rgba_in, int dpi) {
    (void)dpi;
    const Image rgba = rgba_in.mode() == "RGBA" ? rgba_in : rgba_in.convert("RGBA");
    const core::Page work = stand_in(page, false, false);
    Image now = rgba;
    for (const Filter& f : shown(layer)) {
        Image out = core::smart::bend_filter(f.kind) ? bend_8bit(page, f, now) : step_8bit(doc, work, f, now);
        now = replaces(f) ? std::move(out) : detail::blend_over(now, out, f.blend, f.opacity, nullptr);
    }
    const auto mask = mask_ref(layer);
    if (!mask) return now;
    const Image m = mask_picture(doc, *mask, rgba.size());
    const std::string b = rgba.tobytes(), f = now.convert("RGBA").tobytes();
    std::string out(b.size(), '\0');
    const auto get = [](const std::string& bytes) {
        return [&bytes](std::size_t i, double* v) {
            for (unsigned c = 0; c < 4; ++c) v[c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0;
        };
    };
    mix_through(std::size_t(rgba.width()) * rgba.height(), 3, m, get(b), get(f), [&](std::size_t i, const double* v) {
        for (unsigned c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v[c], 0.0, 1.0) * 255.0)));
    });
    return Image::frombytes("RGBA", rgba.size(), out);
}

ColorPicture on_precise(const core::Document& doc, const core::Page& page, const core::Layer& layer, const ColorPicture& picture,
                        const RgbSpace& space, int dpi) {
    (void)dpi;
    const core::Page work = stand_in(page, true, false);
    const std::string precision = page.rgb && page.rgb->depth == 32 ? "f32" : "u16";
    const int w = picture.size.width, h = picture.size.height;
    const Box whole{0, 0, w, h};
    ColorPicture now = picture;
    for (const Filter& f : shown(layer)) {
        ColorPicture out;
        if (core::smart::bend_filter(f.kind)) {  // (PC3 A5: the picture bent, in linear light)
            out = bend_precise(page, f, now);
        } else {
            const core::Layer raster = raster_layer(core::encode_color_pixels(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), precision, [&](std::size_t i) {
                const auto& p = now.pixels[i];
                return std::array<double, 4>{space.encode(0, p[0]), space.encode(1, p[1]), space.encode(2, p[2]), p[3]};
            }));
            const std::string bytes = precise_step(doc, work, raster, f);
            const core::ColorRasterView view(bytes);
            if (view.width() != static_cast<std::uint32_t>(w) || view.height() != static_cast<std::uint32_t>(h))
                throw core::Error("value", "a smart filter changed the picture's size");
            out = color_picture(view, picture.size, whole, space);
        }
        if (replaces(f)) {
            now = std::move(out);
            continue;
        }
        // (laid over what is below in its blend mode at its opacity, a band of rows at a time, as the canvas mixes layers)
        for (int y0 = 0; y0 < h; y0 += kBand) {
            const int y1 = std::min(h, y0 + kBand);
            const Size band{w, y1 - y0};
            const auto first = static_cast<std::ptrdiff_t>(std::size_t(y0) * w), last = static_cast<std::ptrdiff_t>(std::size_t(y1) * w);
            const ColorPicture below{band, {now.pixels.begin() + first, now.pixels.begin() + last}};
            const ColorPicture above{band, {out.pixels.begin() + first, out.pixels.begin() + last}};
            ColorCanvas canvas(Image::create("RGBA", band, Ink{0, 0, 0, 0}), space);
            canvas.blend(below, 1.0, false, "normal");
            canvas.blend(above, f.opacity, false, f.blend);
            const ColorPicture mixed = canvas.picture();
            std::copy(mixed.pixels.begin(), mixed.pixels.end(), now.pixels.begin() + first);
        }
    }
    const auto mask = mask_ref(layer);
    if (!mask) return now;
    const Image m = mask_picture(doc, *mask, picture.size);
    ColorPicture out{picture.size, std::vector<std::array<double, 4>>(now.pixels.size())};
    const auto get = [](const ColorPicture& from) {
        return [&from](std::size_t i, double* v) { std::copy(from.pixels[i].begin(), from.pixels[i].end(), v); };
    };
    mix_through(now.pixels.size(), 3, m, get(picture), get(now), [&](std::size_t i, const double* v) { out.pixels[i] = {v[0], v[1], v[2], v[3]}; });
    return out;
}

InkPicture on_inks(const core::Document& doc, const core::Page& page, const core::Layer& layer, const InkPicture& picture, int dpi) {
    (void)dpi;
    const core::PageColor& color = *page.color;
    const core::Page work = stand_in(page, false, true);
    const unsigned n = picture.channels;
    const int w = picture.size.width, h = picture.size.height;
    InkPicture now = picture;
    for (const Filter& f : shown(layer)) {
        InkPicture out;
        if (core::smart::bend_filter(f.kind)) {  // (PC3 A5: the picture bent, on the canvas's values)
            out = bend_inks(page, f, now);
        } else {
            const std::string bytes = ink_step(doc, work, pixels_of(now, color), f);
            const core::InkPixels inks = core::decode_ink_raster(bytes);
            if (inks.width != static_cast<std::uint32_t>(w) || inks.height != static_cast<std::uint32_t>(h))
                throw core::Error("value", "a smart filter changed the picture's size");
            out = picture_of(inks, n);
        }
        if (replaces(f)) {
            now = std::move(out);
            continue;
        }
        for (int y0 = 0; y0 < h; y0 += kBand) {
            const int y1 = std::min(h, y0 + kBand);
            const Size band{w, y1 - y0};
            const auto first = static_cast<std::ptrdiff_t>(std::size_t(y0) * w * (n + 1)), last = static_cast<std::ptrdiff_t>(std::size_t(y1) * w * (n + 1));
            const InkPicture below{band, n, {now.values.begin() + first, now.values.begin() + last}};
            const InkPicture above{band, n, {out.values.begin() + first, out.values.begin() + last}};
            InkCanvas canvas(color, band);
            canvas.blend(below, 1.0, false, "normal");
            canvas.blend(above, f.opacity, false, f.blend);
            const InkPicture mixed = picture_of(canvas.ink_pixels(color.depth, true), n);
            std::copy(mixed.values.begin(), mixed.values.end(), now.values.begin() + first);
        }
    }
    const auto mask = mask_ref(layer);
    if (!mask) return now;
    const Image m = mask_picture(doc, *mask, picture.size);
    InkPicture out{picture.size, n, std::vector<double>(now.values.size(), 0.0)};
    const auto get = [n](const InkPicture& from) {
        return [&from, n](std::size_t i, double* v) { std::copy_n(from.values.data() + i * (n + 1), n + 1, v); };
    };
    mix_through(std::size_t(w) * h, n, m, get(picture), get(now), [&](std::size_t i, const double* v) { std::copy_n(v, n + 1, out.values.data() + i * (n + 1)); });
    return out;
}

core::Json settled(const core::Document& doc, const core::Page& page, std::string_view kind, const core::Json& given) {
    const std::string name(kind);
    if (name.starts_with("plugin:")) throw core::OpError("a filter plugin cannot be a smart filter (rasterize the smart object to run it)");
    if (!core::smart::filter_kind(name)) throw core::OpError("the filter " + name + " cannot be a smart filter");
    if (core::smart::bend_filter(name)) return refused([&] { return smart_bends::settled(name, given, page); });  // (PC3 A5)
    const Json params = given.is_object() ? given : Json::object();
    if (params.contains("channel")) {  // (a channel or plate of the page, PC1c's target — not levels' own r, g, b)
        std::optional<channels::Target> target;
        try {
            target = channels::target_of(doc, Json{{"op", "filter_raster"}, {"page", page.index.json()}, {"channel", params["channel"]}});
        } catch (const std::exception&) {
        }
        if (target) throw core::OpError("a smart filter works on the whole picture, not on one channel");
    }
    if (name == "color_lookup" && (params.contains("lut") || params.contains("data") || params.contains("file_name")))
        throw core::OpError("a smart filter's colour lookup is one of the looks (look), not a table");
    Json op = params;
    op["kind"] = name;
    return refused([&] {
        std::optional<adjust_ops::FilterWork> own;
        if (adjust_ops::handles(op)) own = adjust_ops::filter_work(doc, page, name, params);
        if (!own || own->python) color_filters::check(name, own ? own->params : params);
        if (own && !own->python && own->local != nullptr) {  // (a kind that looks beyond each pixel: every way it is drawn)
            const core::AdjustKind* found = core::adjust_kind(name);
            if (!own->local->eight && !own->local->precise) core::adjust_not_yet("a smart filter's " + name, found != nullptr ? found->group : "PC2");
            if (!own->local->precise) core::adjust_not_yet("on precise pixels, the filter " + name, found != nullptr ? found->group : "PC2");
            if (page.color && !own->local->inks) core::adjust_not_yet("on a page of inks, the filter " + name, found != nullptr ? found->group : "PC2");
        }
        return own ? own->params : params;
    });
}

std::size_t push_filter(core::Document& doc, const core::Page& page, core::Layer& layer, const std::string& kind, const Json& params,
                        const std::optional<Json>& area) {
    if (layer.kind != core::LayerKind::Smart || !layer.smart || !layer.smart->is_object()) throw core::OpError("layer " + layer.id + " is not a smart object");
    if (layer.locked) throw core::OpError("the layer is locked");
    const Json stored = settled(doc, page, kind, params);
    const Json* now = core::get(*layer.smart, "filters");
    Json filters = now != nullptr && now->is_array() ? *now : Json::array();
    if (filters.size() >= core::smart::kFiltersMax)
        throw core::OpError("a smart object holds at most " + std::to_string(core::smart::kFiltersMax) + " smart filters");
    filters.push_back(Json{{"kind", kind}, {"params", stored}, {"blend", "normal"}, {"opacity", 1.0}, {"visible", true}});
    // (a selection becomes the filter mask, when it has none: PhotoCraft's add_smart_filter)
    const Json* mask = core::get(*layer.smart, "filter_mask");
    std::optional<Json> made;
    if (area && !(mask != nullptr && mask->is_object())) {
        const std::string png = mask_of_area(page, *area);
        const std::string ref = core::bytes_ref(png);
        doc.smart_files[ref] = std::make_shared<const std::string>(png);
        made = Json{{"asset", ref}, {"enabled", true}};
    }
    const std::size_t index = filters.size() - 1;
    core::smart::put_setting(layer, "filters", std::move(filters));
    if (made) core::smart::put_setting(layer, "filter_mask", std::move(*made));
    Json& s = *layer.smart;
    if (Json* psd = s.contains("psd") ? &s["psd"] : nullptr; psd != nullptr && psd->is_object()) psd->erase("shown");  // (PC3 A3)
    return index;
}

std::string mask_of_area(const core::Page& page, const core::Json& area) {
    const double w = std::max(1.0, core::py_round_whole(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const double h = std::max(1.0, core::py_round_whole(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
    const selection::AreaMask shown = selection::area_mask(area, kMaskDpi);
    mask.paste(shown.mask, Point{static_cast<int>(shown.x0), static_cast<int>(shown.y0)});
    return *fills::png_bytes(mask);
}

}  // namespace genko::render::smart_filters
