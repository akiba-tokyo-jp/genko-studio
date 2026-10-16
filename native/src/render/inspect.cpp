#include "render/inspect.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "render/color_canvas.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

namespace genko::render {

namespace {

using core::Json;

// An ink of a pixel (channel c of pixel i) as render/ink takes inks.
double ink_value(const core::InkPixels& p, std::size_t i, unsigned c) {
    if (p.is_float()) return from_canvas(p.space, c, p.value(i, c));
    return ink::decode_sample(p.space, c, p.samples[i * p.channels() + c], p.depth);
}

// An ink pixel's values, samples, alpha and screen colour.
void ink_pixel(Json& out, const core::InkPixels& p, std::size_t i, const core::PageColor& color) {
    const unsigned n = p.colours();
    Json values = Json::array(), stored = Json::array();
    std::vector<double> inks(n);
    for (unsigned c = 0; c < n; ++c) {
        inks[c] = ink_value(p, i, c);
        values.push_back(inks[c]);
        if (p.is_float()) stored.push_back(p.value(i, c));
        else stored.push_back(p.samples[i * p.channels() + c]);
    }
    out["values"] = std::move(values);
    out["stored"] = std::move(stored);
    out["alpha"] = p.value(i, n);
    const std::array<double, 3> shown = ink::srgb_of(color, inks);
    out["srgb"] = Json::array();
    for (const double v : shown) out["srgb"].push_back(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

// An RGB page's values (encoded, its profile's) as the screen shows them.
Json screen_of(const core::Page& page, std::array<double, 3> rgb) {
    for (double& v : rgb) v = std::clamp(v, 0.0, 1.0);
    RgbSpace::of(page).to_srgb(rgb.data(), 1);
    Json out = Json::array();
    for (const double v : rgb) out.push_back(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    return out;
}

// An RGB value as the page's depth stores it (8, 16: integers; 32: the float).
Json stored_rgb(double v, int depth) {
    if (depth == 32) return Json(static_cast<double>(static_cast<float>(v)));
    const double most = depth == 16 ? 65535.0 : 255.0;
    return Json(std::lround(std::clamp(v, 0.0, 1.0) * most));
}

}  // namespace

Json pixel_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, const std::optional<std::string>& layer_id,
              int dpi) {
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!std::isfinite(x_mm) || !std::isfinite(y_mm) || x_mm < 0 || y_mm < 0 || x_mm >= w_mm || y_mm >= h_mm) {
        throw core::Error("key", "the point is off the page");
    }
    Json out = Json::object();
    out["page"] = page.index.json();
    out["mode"] = page.color ? std::string(core::to_string(page.color->space)) : std::string("rgb");
    const int depth = page.color ? page.color->depth : page.rgb ? page.rgb->depth : 8;
    out["depth"] = depth;
    if (layer_id) {
        const auto found = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == *layer_id; });
        if (found == page.layers.end()) throw core::Error("key", "no layer " + *layer_id);
        const core::Layer& layer = *found;
        out["layer"] = layer.id;
        // (the raster lies stretched over the page)
        const auto index = [&](std::uint32_t width, std::uint32_t height) {
            const auto x = std::min<std::uint32_t>(width - 1, static_cast<std::uint32_t>(x_mm / w_mm * width));
            const auto y = std::min<std::uint32_t>(height - 1, static_cast<std::uint32_t>(y_mm / h_mm * height));
            return std::size_t(y) * width + x;
        };
        if (layer.ink_raster && page.color) {
            const std::shared_ptr<const core::InkPixels> pixels = decoded_ink(layer.ink_raster);
            out["precision"] = pixels->depth;
            ink_pixel(out, *pixels, index(pixels->width, pixels->height), *page.color);
        } else if (layer.color_raster) {
            const core::ColorRasterView view(*layer.color_raster);
            const bool u16 = static_cast<unsigned char>((*layer.color_raster)[5]) == 2;
            out["precision"] = u16 ? "u16" : "f32";
            const auto p = view.pixel(index(view.width(), view.height()));
            out["values"] = Json::array({p[0], p[1], p[2]});
            out["stored"] = Json::array();
            for (unsigned c = 0; c < 3; ++c) out["stored"].push_back(u16 ? Json(std::lround(p[c] * 65535)) : Json(p[c]));
            out["alpha"] = p[3];
            out["srgb"] = screen_of(page, {p[0], p[1], p[2]});
        } else if (layer.raster_png && !layer.raster_png->empty()) {
            const Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
            const std::size_t i = index(static_cast<std::uint32_t>(picture.width()), static_cast<std::uint32_t>(picture.height()));
            const int x = static_cast<int>(i % static_cast<std::size_t>(picture.width())), y = static_cast<int>(i / static_cast<std::size_t>(picture.width()));
            const std::string b = picture.crop(Box{x, y, x + 1, y + 1}).tobytes();
            out["precision"] = 8;
            std::array<double, 3> v{};
            out["values"] = Json::array();
            out["stored"] = Json::array();
            for (unsigned c = 0; c < 3; ++c) {
                const int s = static_cast<unsigned char>(b[c]);
                v[c] = s / 255.0;
                out["values"].push_back(v[c]);
                out["stored"].push_back(s);
            }
            out["alpha"] = static_cast<unsigned char>(b[3]) / 255.0;
            out["srgb"] = screen_of(page, v);
        } else {
            out["values"] = nullptr;
            out["stored"] = nullptr;
            out["alpha"] = 0.0;
            out["srgb"] = nullptr;
        }
        return out;
    }
    RenderOptions options = proof_options();
    options.inks = page.color.has_value();
    options.precise = !page.color.has_value();
    options.skip_unported = true;
    const int x = static_cast<int>(std::floor(x_mm / 25.4 * dpi)), y = static_cast<int>(std::floor(y_mm / 25.4 * dpi));
    const int pw = mm_to_px(w_mm, dpi), ph = mm_to_px(h_mm, dpi);
    options.region = RenderRegion{std::clamp(x, 0, std::max(0, pw - 1)), std::clamp(y, 0, std::max(0, ph - 1)), 1, 1};
    const RenderResult made = render_page(page, dpi, options, episode);
    const std::string shown = made.image.convert("RGB").tobytes();
    Json srgb = Json::array();
    for (unsigned c = 0; c < 3; ++c) srgb.push_back(static_cast<unsigned char>(shown[c]));
    if (made.inks && page.color) {
        ink_pixel(out, *made.inks, 0, *page.color);
        out["srgb"] = std::move(srgb);  // (the page as it is shown there: its frames and words too)
        return out;
    }
    out["values"] = Json::array();
    out["stored"] = Json::array();
    if (made.precise) {  // (the page's values where it composites in precise colour)
        const core::ColorRasterView view(*made.precise);
        const auto p = view.pixel(0);
        for (unsigned c = 0; c < 3; ++c) {
            out["values"].push_back(p[c]);
            out["stored"].push_back(stored_rgb(p[c], depth));
        }
        out["alpha"] = p[3];
    } else {
        for (unsigned c = 0; c < 3; ++c) {
            out["values"].push_back(srgb[c].get<int>() / 255.0);
            out["stored"].push_back(stored_rgb(srgb[c].get<int>() / 255.0, depth));
        }
        out["alpha"] = 1.0;
    }
    out["srgb"] = std::move(srgb);
    return out;
}

}  // namespace genko::render
