#include "render/brushwork.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/pyops.hpp"
#include "render/ink_edit.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

// PhotoCraft v0.2.0's engine/src/retouch_cmds.rs (lines 1-120: the brushes' parameters and sampling), MIT
// (native/third_party/photocraft/NOTICE.md), with Genko's mm, layers and areas (PC3 B4; shared with PC4 E5).

namespace genko::render::brushing {

using core::Json;
using core::OpError;
using image_ops::Pixels;

// --- the arguments ---------------------------------------------------------------------------------------------------

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

double number(const Json& op, const char* key, double fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a finite number");
    return v->get<double>();
}

bool flag(const Json& op, const char* key, bool fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(std::string(key) + " must be true or false");
    return v->get<bool>();
}

std::string choice(const Json& op, const char* key, std::initializer_list<const char*> allowed) {
    const Json* v = field(op, key);
    if (v == nullptr) return *allowed.begin();
    std::string listed;
    for (const char* a : allowed) {
        if (v->is_string() && v->get<std::string>() == a) return a;
        listed += (listed.empty() ? "" : ", ") + std::string(a);
    }
    throw OpError(std::string(key) + " must be one of " + listed);
}

// [x, y] in mm; none when not given.
std::optional<std::pair<double, double>> pair_mm(const Json& op, const char* key) {
    const Json* v = field(op, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_array() || v->size() != 2 || !(*v)[0].is_number() || !(*v)[1].is_number() || (*v)[0].is_boolean() || (*v)[1].is_boolean() ||
        !std::isfinite((*v)[0].get<double>()) || !std::isfinite((*v)[1].get<double>()))
        throw OpError(std::string(key) + " must be [x, y] (mm)");
    return std::make_pair((*v)[0].get<double>(), (*v)[1].get<double>());
}

double rounded(double v) { return std::round(v * 10000.0) / 10000.0; }

Brushwork brushwork(const core::Document& doc, const Json& op) {
    Brushwork b;
    b.at = core::require_page(doc, op);
    const Json* id = field(op, "layer_id");
    if (id == nullptr || !id->is_string()) throw OpError("layer_id is required");
    b.id = id->get<std::string>();
    const Json* points = field(op, "points");
    if (points == nullptr || !points->is_array() || points->empty()) throw OpError("points needs at least one [x_mm, y_mm]");
    b.points = core::parse_points(*points);
    for (const core::PenPoint& p : b.points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || (p.p && !std::isfinite(*p.p))) throw OpError("points must be finite numbers");
    }
    b.width_mm = number(op, "width_mm", 5.0);
    if (!(b.width_mm > 0.0)) throw OpError("width_mm must be more than 0");
    b.width_mm = std::min(b.width_mm, 500.0);
    b.hardness = static_cast<float>(std::clamp(number(op, "hardness", 100.0), 0.0, 100.0) / 100.0);
    b.opacity = static_cast<float>(std::clamp(number(op, "opacity", 100.0), 1.0, 100.0) / 100.0);
    b.flow = static_cast<float>(std::clamp(number(op, "flow", 100.0), 1.0, 100.0) / 100.0);
    b.spacing = static_cast<float>(std::clamp(number(op, "spacing", 25.0), 1.0, 1000.0) / 100.0);
    if (const Json* given = field(op, "blend")) {
        if (!given->is_string() || !image_ops::is_blend(given->get<std::string>()))
            throw OpError("blend must be one of normal, multiply, screen, add, overlay, darken, lighten, color_burn, color_dodge, linear_burn, "
                          "soft_light, hard_light, difference, exclusion, subtract, divide, hue, saturation, color, luminosity");
        b.blend = given->get<std::string>();
    }
    b.area = field(op, "area");
    return b;
}

// --- the layer's grid ------------------------------------------------------------------------------------------------

Box meet(const Box& a, const Box& b) {
    const Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return out.x1 > out.x0 && out.y1 > out.y0 ? out : Box{};
}

bool empty(const Box& b) { return b.width() <= 0 || b.height() <= 0; }

Box grown(const Box& b, int by) { return Box{b.x0 - by, b.y0 - by, b.x1 + by, b.y1 + by}; }

Ready ready(core::OpContext& c, std::size_t at, const std::string& id) {
    image_ops::Window window;
    Pixels like = image_ops::layer_pixels(c, at, id, Box{0, 0, 1, 1}, &window);
    return Ready{grid_of(c.doc.page(at), window.grid, id), std::move(like)};
}

Grid grid_of(const core::Page& page, Size size, const std::string& id) {
    Grid g;
    g.size = size;
    if (g.size.width <= 0 || g.size.height <= 0) throw OpError("the layer " + id + " has no pixels to work on");
    g.sx = g.size.width / page.spec.width_mm.value();
    g.sy = g.size.height / page.spec.height_mm.value();
    return g;
}

const core::Layer& layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw OpError("no layer " + id);
}

std::vector<retouch::StrokePoint> on_grid(const core::PenPoints& points, const Grid& g) {
    std::vector<retouch::StrokePoint> out;
    out.reserve(points.size());
    for (const core::PenPoint& p : points) {
        out.push_back(retouch::StrokePoint{p.x * g.sx, p.y * g.sy, static_cast<float>(std::clamp(p.p.value_or(1.0), 0.0, 1.0))});
    }
    return out;
}

retouch::Brush brush_of(const Brushwork& b, const Grid& g) {
    retouch::Brush brush;
    brush.size = static_cast<float>(std::max(g.px(b.width_mm), 1.0));
    brush.hardness = b.hardness;
    brush.opacity = b.opacity;
    brush.flow = b.flow;
    brush.spacing = b.spacing;
    return brush;
}

Json box_mm(const Box& b, const Grid& g) {
    if (empty(b)) return nullptr;
    return Json::array({rounded(b.x0 / g.sx), rounded(b.y0 / g.sy), rounded(b.width() / g.sx), rounded(b.height() / g.sy)});
}

// --- pixels over a box of the grid -------------------------------------------------------------------------------------

Pixels clear_like(const Pixels& like, const Box& box) {
    Pixels out(like.kind(), static_cast<std::uint32_t>(std::max(box.width(), 0)), static_cast<std::uint32_t>(std::max(box.height(), 0)),
               like.colours());
    out.floating = like.floating;
    out.ink = like.ink;
    return out;
}

// `part` (over `part_box`) laid into `whole` (over `whole_box`) where they meet.
void place(Pixels& whole, const Box& whole_box, const Pixels& part, const Box& part_box) {
    const Box r = meet(whole_box, part_box);
    const unsigned n = whole.all();
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float* from = part.at(std::size_t(y - part_box.y0) * std::size_t(part_box.width()) + std::size_t(x - part_box.x0));
            std::copy(from, from + n, whole.at(std::size_t(y - whole_box.y0) * std::size_t(whole_box.width()) + std::size_t(x - whole_box.x0)));
        }
    }
}

// The layer's pixels over `box` of its grid (past the grid: clear).
Pixels layer_window(core::OpContext& c, std::size_t at, const std::string& id, const Box& box, const Pixels& like, const Grid& g) {
    const Box inside = meet(box, g.whole());
    if (empty(inside)) return clear_like(like, box);
    image_ops::Window window;
    Pixels part = image_ops::layer_pixels(c, at, id, inside, &window);
    if (window.box == box) return part;
    Pixels whole = clear_like(like, box);
    place(whole, box, part, window.box);
    return whole;
}

Sample sample_of(const Json& op) {
    const std::string s = choice(op, "sample", {"current", "current_and_below", "all"});
    return s == "all" ? Sample::All : s == "current_and_below" ? Sample::CurrentAndBelow : Sample::Current;
}

// The page as it shows over `box` of a layer's grid, as pixels of the layer's kind: All — print, without its finish,
// onion skin, nombre or spot channels (image_ops::page_pixels'); CurrentAndBelow — the same without the layers above
// `id`. Past the page: clear.
Pixels shown_window(const core::Document& doc, std::size_t at, const std::string& id, Sample which, const Grid& g, const Box& box, const Pixels& like) {
    Pixels out = clear_like(like, box);
    const core::Page& page = doc.page(at);
    core::Page shown = page;
    shown.numero = false;
    if (which == Sample::CurrentAndBelow) {
        for (std::size_t i = 0; i < shown.layers.size(); ++i) {
            if (shown.layers[i].id != id) continue;
            shown.layers.erase(shown.layers.begin() + static_cast<std::ptrdiff_t>(i) + 1, shown.layers.end());
            break;
        }
    }
    // (the page drawn at the grid's resolution — the nearest when no whole dpi gives it exactly — and each pixel of the
    // box taken from where it falls)
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    int dpi = 0;
    const double guess = std::round(g.size.width * 25.4 / w_mm);
    for (const double d : {guess, guess - 1, guess + 1}) {
        if (d >= 1 && d <= 4800 && mm_to_px(w_mm, static_cast<int>(d)) == g.size.width && mm_to_px(h_mm, static_cast<int>(d)) == g.size.height) {
            dpi = static_cast<int>(d);
            break;
        }
    }
    const bool exact = dpi > 0;
    if (!exact) dpi = static_cast<int>(std::clamp(std::ceil(std::max(g.sx, g.sy) * 25.4), 1.0, 4800.0));
    const Size drawn{mm_to_px(w_mm, dpi), mm_to_px(h_mm, dpi)};
    const double fx = exact ? 1.0 : drawn.width / static_cast<double>(g.size.width), fy = exact ? 1.0 : drawn.height / static_cast<double>(g.size.height);
    const Box inside = meet(box, g.whole());
    if (empty(inside)) return out;
    const Box want = meet(Box{static_cast<int>(std::floor(inside.x0 * fx)), static_cast<int>(std::floor(inside.y0 * fy)),
                              static_cast<int>(std::ceil(inside.x1 * fx)), static_cast<int>(std::ceil(inside.y1 * fy))},
                          Box{0, 0, drawn.width, drawn.height});
    if (empty(want)) return out;
    RenderOptions options;
    options.mode = "print";
    options.finish = false;
    options.onion = false;
    options.spots = std::vector<std::string>{};
    const bool deep = !page.color && page.rgb && (page.rgb->depth != 8 || !page.rgb->profile.empty());
    options.inks = page.color.has_value();
    options.precise = deep;
    options.region = RenderRegion{want.x0, want.y0, want.width(), want.height()};
    const RenderResult made = render_page(shown, dpi, options, &doc);
    std::optional<Pixels> got;
    if (page.color && made.inks) {
        got.emplace(image_ops::from_inks(*made.inks, *page.color));
    } else if (deep && made.precise) {
        got.emplace(image_ops::from_precise(*made.precise));
    } else {
        got.emplace(image_ops::from_image(made.image));
    }
    const unsigned n = out.all();
    if (got->all() != n) throw OpError("the page's pixels are not of the layer's kind");
    const auto ww = std::size_t(want.width());
    for (int y = inside.y0; y < inside.y1; ++y) {
        const int dy = std::clamp(static_cast<int>(std::floor((y + 0.5) * fy)), want.y0, want.y1 - 1) - want.y0;
        for (int x = inside.x0; x < inside.x1; ++x) {
            const int dx = std::clamp(static_cast<int>(std::floor((x + 0.5) * fx)), want.x0, want.x1 - 1) - want.x0;
            const float* from = got->at(std::size_t(dy) * ww + std::size_t(dx));
            float* to = out.at(std::size_t(y - box.y0) * std::size_t(box.width()) + std::size_t(x - box.x0));
            std::copy(from, from + n, to);
            if (out.kind() == Pixels::Kind::Eight) {
                for (unsigned k = 0; k < n; ++k) to[k] = std::clamp(to[k], 0.0f, 1.0f);
            }
        }
    }
    return out;
}

// The pixels a stroke samples over `box`: the layer's own, or the page as it shows.
Pixels sampled(core::OpContext& c, const Brushwork& b, Sample which, const Grid& g, const Box& box, const Pixels& like) {
    if (which == Sample::Current) return layer_window(c, b.at, b.id, box, like, g);
    return shown_window(c.doc, b.at, b.id, which, g, box, like);
}

// --- the selection -----------------------------------------------------------------------------------------------------

AreaCover::AreaCover(const core::Page& page, const Json& area, const Grid& g, bool eight) {
    const Size working{mm_to_px(page.spec.width_mm.value(), raster::kWorkingDpi), mm_to_px(page.spec.height_mm.value(), raster::kWorkingDpi)};
    if (eight && g.size == working) {
        const selection::AreaMask inside = selection::area_mask(area, raster::kWorkingDpi);
        const Image mask = inside.mask.mode() == "L" ? inside.mask : inside.mask.convert("L");
        where_ = Box{static_cast<int>(inside.x0), static_cast<int>(inside.y0), static_cast<int>(inside.x0) + mask.width(),
                     static_cast<int>(inside.y0) + mask.height()};
        bytes_ = mask.tobytes();
    } else {
        held_.emplace(std::uint64_t(g.size.width) * std::uint64_t(g.size.height));
        bytes_ = ink_edit::area_coverage(page, static_cast<std::uint32_t>(g.size.width), static_cast<std::uint32_t>(g.size.height), area);
        where_ = g.whole();
    }
}

float AreaCover::at(int x, int y) const {
    if (x < where_.x0 || y < where_.y0 || x >= where_.x1 || y >= where_.y1) return 0.0f;
    return static_cast<unsigned char>(bytes_[std::size_t(y - where_.y0) * std::size_t(where_.width()) + std::size_t(x - where_.x0)]) / 255.0f;
}

std::vector<float> AreaCover::over(const Box& box) const {
    std::vector<float> out(std::size_t(std::max(box.width(), 0)) * std::size_t(std::max(box.height(), 0)), 0.0f);
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) out[std::size_t(y - box.y0) * std::size_t(box.width()) + std::size_t(x - box.x0)] = at(x, y);
    }
    return out;
}

Box AreaCover::box(const Box& limit) const {
    Box out{limit.x1, limit.y1, limit.x0, limit.y0};
    const Box r = meet(where_, limit);
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            if (at(x, y) <= 0.0f) continue;
            out = Box{std::min(out.x0, x), std::min(out.y0, y), std::max(out.x1, x + 1), std::max(out.y1, y + 1)};
        }
    }
    return empty(out) ? Box{} : out;
}

std::optional<std::vector<float>> selection_over(const core::Page& page, const Brushwork& b, const Grid& g, const Pixels& like, const Box& box) {
    if (b.area == nullptr) return std::nullopt;
    return AreaCover(page, *b.area, g, like.kind() == Pixels::Kind::Eight).over(box);
}

bool lock_alpha_of(const core::Document& doc, std::size_t at, const std::string& id) { return layer_of(doc.page(at), id).lock_alpha; }

}  // namespace genko::render::brushing
