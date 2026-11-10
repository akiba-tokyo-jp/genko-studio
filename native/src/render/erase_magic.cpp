// PC3 B1 (SPEC.md COLOR-06「マジック消しゴムツール」): the op magic_erase — the Magic Eraser: the magic wand's region
// around a point (selection_ext::wand_region on 8-bit colours, as the wand) erased to transparency on a paint layer,
// weighted by the region's coverage, the tool's opacity and the selection; on a layer whose transparency is protected
// (lock_alpha) the pixels are painted toward the background colour instead, their alpha kept. Only the alpha changes
// otherwise: an 8-bit, precise or ink layer keeps its colours in its own values (image_ops::Pixels). PhotoCraft v0.2.0's
// algo/src/erase.rs (magic_erase) and engine/src/eraser_cmds.rs (magic_eraser, lines 85-121), MIT
// (native/third_party/photocraft/NOTICE.md).

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/image_ops.hpp"
#include "render/ops_pc3.hpp"
#include "render/selection_ext.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

double number(const Json& op, const char* key, std::optional<double> fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) {
        if (!fallback) throw OpError(std::string(key) + " is required");
        return *fallback;
    }
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a finite number");
    return v->get<double>();
}

bool flag(const Json& op, const char* key, bool fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(std::string(key) + " must be true or false");
    return v->get<bool>();
}

// The pixel of a grid (width × height over the page) under a point (mm); none off the page.
std::optional<std::pair<int, int>> pixel_at(const core::Page& page, std::uint32_t w, std::uint32_t h, double x_mm, double y_mm) {
    const double fx = x_mm / page.spec.width_mm.value() * w;
    const double fy = y_mm / page.spec.height_mm.value() * h;
    if (!(fx >= 0 && fy >= 0 && fx < w && fy < h)) return std::nullopt;
    return std::make_pair(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
}

std::vector<selection_ext::Rgba8> eight_bits(const std::vector<image_ops::Rgba>& px) {
    std::vector<selection_ext::Rgba8> out(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            const float v = std::isfinite(px[i][k]) ? std::clamp(px[i][k], 0.0f, 1.0f) : 0.0f;
            out[i][k] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
        }
    }
    return out;
}

// The wand's region as coverage (0..1) on a grid of width × height.
std::vector<float> region_on(const selection_ext::Region& region, Size grid, std::uint32_t width, std::uint32_t height) {
    Image mask = selection_ext::region_image(region, grid);
    const Size size{static_cast<int>(width), static_cast<int>(height)};
    if (!(grid == size)) mask = mask.resize(size, Resample::Bilinear);
    return selection_ext::from_l(mask);
}

}  // namespace

void magic_erase(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* given = field(op, "layer_id");
    if (given == nullptr || !given->is_string()) throw OpError("layer_id is required");
    const std::string id = given->get<std::string>();
    const double x = number(op, "x_mm", std::nullopt), y = number(op, "y_mm", std::nullopt);
    const float tolerance = static_cast<float>(std::clamp(number(op, "tolerance", 32.0), 0.0, 255.0));
    const float opacity = static_cast<float>(std::clamp(number(op, "opacity", 1.0), 0.0, 1.0));
    const bool contiguous = flag(op, "contiguous", true);
    const bool antialias = flag(op, "antialias", true);
    const Json* sample_given = field(op, "sample");
    if (sample_given != nullptr && (!sample_given->is_string() || (*sample_given != Json("layer") && *sample_given != Json("page")))) {
        throw OpError("sample is layer or page");
    }
    const bool whole_page = sample_given != nullptr && *sample_given == Json("page");
    std::array<float, 3> background{1.0f, 1.0f, 1.0f};
    if (const Json* bg = field(op, "background")) {
        if (!bg->is_array() || bg->size() != 3) throw OpError("background is [r, g, b] (0..255 each)");
        for (std::size_t i = 0; i < 3; ++i) {
            const Json& v = (*bg)[i];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::int64_t>() > 255) throw OpError("background is [r, g, b] (0..255 each)");
            background[i] = static_cast<float>(v.get<std::int64_t>()) / 255.0f;
        }
    }
    {
        const core::Page& page = doc.page(at);
        const auto layer = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == id; });
        if (layer == page.layers.end()) throw OpError("no layer " + id);
        // (a click off the page erases nothing, and the book is left as it was)
        if (!(x >= 0 && y >= 0 && x < page.spec.width_mm.value() && y < page.spec.height_mm.value())) {
            c.report = Json{{"erased", false}};
            return;
        }
    }
    // the region: the layer's own colours, or the page as it shows (Sample All Layers)
    std::optional<selection_ext::Region> region;
    Size region_grid;
    if (whole_page) {
        const Pixels seen = image_ops::page_pixels(doc, doc.page(at));
        const image_ops::Held held(std::uint64_t(seen.size()) * (sizeof(image_ops::Rgba) + 4));
        const auto px = eight_bits(image_ops::rgba_of(seen));
        region_grid = Size{static_cast<int>(seen.width()), static_cast<int>(seen.height())};
        if (const auto seed = pixel_at(doc.page(at), seen.width(), seen.height(), x, y)) {
            region = selection_ext::wand_region(px, selection_ext::Rect{0, 0, region_grid.width, region_grid.height}, *seed, tolerance, contiguous,
                                                antialias);
        }
    }
    Pixels p = image_ops::layer_pixels(c, at, id);  // (refused when locked or not a paint layer; its lines drawn in)
    const core::Page& page = doc.page(at);
    const bool lock = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == id; })->lock_alpha;
    // (the colours as RGB: for the layer's own region, and to paint an ink layer's toward the background)
    std::optional<image_ops::Held> rgba_held;
    std::vector<image_ops::Rgba> rgba;
    if (!whole_page || (lock && p.kind() == Pixels::Kind::Inks)) {
        rgba_held.emplace(std::uint64_t(p.size()) * sizeof(image_ops::Rgba));
        rgba = image_ops::rgba_of(p);
    }
    if (!whole_page) {
        const image_ops::Held held(std::uint64_t(p.size()) * 4);
        const auto px = eight_bits(rgba);
        region_grid = Size{static_cast<int>(p.width()), static_cast<int>(p.height())};
        if (const auto seed = pixel_at(page, p.width(), p.height(), x, y)) {
            region = selection_ext::wand_region(px, selection_ext::Rect{0, 0, region_grid.width, region_grid.height}, *seed, tolerance, contiguous,
                                                antialias);
        }
    }
    if (!region || opacity <= 0.0f) {
        c.report = Json{{"erased", false}};
        return;
    }
    const std::vector<float> cover = region_on(*region, region_grid, p.width(), p.height());
    std::optional<std::vector<float>> chosen;
    if (const Json* area = field(op, "area")) chosen = image_ops::coverage(page, *area, p.width(), p.height(), p.kind() == Pixels::Kind::Eight);
    const unsigned n = p.colours();
    bool erased = false;
    if (lock && p.kind() == Pixels::Kind::Inks) {
        // (an ink layer's colours toward the background's by the page's rules: what it means changed, its inks found)
        std::vector<image_ops::Rgba> moved = rgba;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const float k = cover[i] * opacity * (chosen ? (*chosen)[i] : 1.0f);
            if (k <= 0.0f) continue;
            erased = true;
            for (std::size_t ch = 0; ch < 3; ++ch) moved[i][ch] += (background[ch] - moved[i][ch]) * std::min(k, 1.0f);
        }
        if (erased) image_ops::from_rgba(p, moved);
    } else {
        for (std::size_t i = 0; i < p.size(); ++i) {
            const float k = std::min(cover[i] * opacity * (chosen ? (*chosen)[i] : 1.0f), 1.0f);
            if (k <= 0.0f) continue;
            erased = true;
            float* v = p.at(i);
            if (lock) {  // (the transparency protected: painted toward the background colour, the alpha kept)
                for (unsigned ch = 0; ch < std::min(n, 3u); ++ch) v[ch] += (background[ch] - v[ch]) * k;
                continue;
            }
            const float a = v[n] * (1.0f - k);
            if (k >= 1.0f || a <= 0.0f) {
                std::fill(v, v + p.all(), 0.0f);  // (fully erased: the empty pixel)
            } else {
                v[n] = a;
            }
        }
    }
    if (erased) image_ops::put_layer_pixels(doc, at, id, p);
    c.report = Json{{"erased", erased}};
}

}  // namespace genko::render
