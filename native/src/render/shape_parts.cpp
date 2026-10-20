#include "render/shape_parts.hpp"

#include <algorithm>
#include <cmath>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/path_trace.hpp"
#include "render/selection.hpp"

// PhotoCraft v0.2.0 crates/vector/src/lib.rs (176-378: Paint::from_fill, CompiledShape) and engine/src/vector_cmds.rs
// (364-433: transform_shape; 1011-1035: merge_components), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::shape_parts {

using core::Json;
using core::vector::Knot;
using core::vector::Subpath;
namespace vec = core::vector;

namespace {

// --- paints ---------------------------------------------------------------------------------------------------------

// A fill or a stroke's colour source: a colour (straight sRGB 0..1) or a gradient (a fill layer's: render/composite's
// GradientColours over the page in mm).
struct Paint {
    enum class Kind { None, Solid, Gradient };
    Kind kind = Kind::None;
    std::array<float, 4> rgba{0, 0, 0, 0};
    std::shared_ptr<const detail::GradientColours> gradient;
    double px_per_mm = 1.0;
    std::optional<std::array<float, 3>> guide;  // (every colour this one, its alpha kept)

    std::array<float, 4> at(int x, int y) const {
        if (kind == Kind::Solid) return rgba;
        if (kind == Kind::None) return {0, 0, 0, 0};
        unsigned char rgb[3];
        double opacity = 1.0;
        gradient->at((x + 0.5) / px_per_mm, (y + 0.5) / px_per_mm, rgb, opacity);
        if (guide) return {(*guide)[0], (*guide)[1], (*guide)[2], static_cast<float>(std::clamp(opacity, 0.0, 1.0))};
        return {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, static_cast<float>(std::clamp(opacity, 0.0, 1.0))};
    }
};

std::array<float, 3> rgb_of(const std::vector<std::int64_t>& rgb) {
    const auto c = [&](std::size_t k) { return static_cast<float>(std::clamp<std::int64_t>(k < rgb.size() ? rgb[k] : 0, 0, 255)) / 255.0f; };
    return {c(0), c(1), c(2)};
}

// The paint of a fill (null: none — a stroke's null fill is the layer's ink).
Paint paint_of(const Json& fill, double px_per_mm, const std::vector<std::int64_t>* colour, bool null_is_ink) {
    Paint p;
    p.px_per_mm = px_per_mm;
    if (colour != nullptr) p.guide = rgb_of(*colour);
    const auto solid = [&](const std::vector<std::int64_t>& rgb) {
        p.kind = Paint::Kind::Solid;
        const auto c = p.guide ? *p.guide : rgb_of(rgb);
        p.rgba = {c[0], c[1], c[2], 1.0f};
    };
    if (fill.is_null()) {
        if (null_is_ink) solid(kInkColour);
        return p;
    }
    if (!fill.is_object()) return p;
    if (const auto g = fill.find("gradient"); g != fill.end() && g->is_object()) {
        p.kind = Paint::Kind::Gradient;
        p.gradient = std::make_shared<const detail::GradientColours>(*g);
        return p;
    }
    if (const auto rgb = fill.find("rgb"); rgb != fill.end()) {
        if (rgb->is_null()) {
            solid(kInkColour);
        } else if (rgb->is_array()) {
            std::vector<std::int64_t> values;
            for (const Json& v : *rgb) values.push_back(v.is_number() ? v.get<std::int64_t>() : 0);
            solid(values);
        }
    }
    return p;  // (a pattern, PC4's: nothing yet)
}

Box box_of(const path_raster::PixelBox& b) { return Box{b.x0, b.y0, b.x1, b.y1}; }
path_raster::PixelBox pixel_box_of(const Box& b) { return path_raster::PixelBox{b.x0, b.y0, b.x1, b.y1}; }

Box joined(const std::optional<Box>& a, const Box& b) {
    if (!a) return b;
    return Box{std::min(a->x0, b.x0), std::min(a->y0, b.y0), std::max(a->x1, b.x1), std::max(a->y1, b.y1)};
}

Box clipped(const Box& a, const Box& b) { return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)}; }

// The whole plane, for an inverted fill (the caller cuts it to what it draws).
constexpr Box kPlane{-(1 << 29), -(1 << 29), 1 << 29, 1 << 29};

unsigned char byte(float v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

bool closed(const Path& path) {
    return std::all_of(path.subpaths.begin(), path.subpaths.end(), [](const Subpath& s) { return s.closed; });
}

bool fills(const Json& fill) {
    if (!fill.is_object()) return false;
    const auto g = fill.find("gradient");
    if (g != fill.end() && g->is_object()) return true;
    return fill.contains("rgb");
}

// --- drawing ------------------------------------------------------------------------------------------------------------

struct Compiled::Impl {
    std::optional<path_raster::Rasterizer> fill;
    Paint fill_paint;
    std::optional<path_raster::Rasterizer> stroke;
    Paint stroke_paint;
    float stroke_opacity = 1.0f;
    std::optional<path_raster::Rasterizer> align;  // (the fill's area, cutting an inside or outside stroke)
    bool inside = false;
    float opacity = 1.0f;
    std::optional<Box> box;
};

Compiled::Compiled(const ShapePart& part, double dpi, const std::vector<std::int64_t>* colour) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    const double k = dpi / 25.4;
    m.opacity = static_cast<float>(std::clamp(part.opacity, 0.0, 1.0));
    const Path px = path_raster::to_pixels(part.path, dpi);
    path_raster::Rasterizer area = path_raster::fill_rasterizer(px);
    m.fill_paint = paint_of(part.fill, k, colour, false);
    if (m.fill_paint.kind != Paint::Kind::None && !part.path.empty()) {
        if (px.inverted) {
            m.box = kPlane;
        } else if (const auto b = area.pixel_bounds()) {
            m.box = box_of(*b);
        }
        m.fill = area;
    }
    if (part.stroke && part.stroke->width_mm > 0 && part.stroke->opacity > 0 && !part.path.empty()) {
        path_raster::StrokeStyle style = path_raster::stroke_style(*part.stroke, k);
        if (closed(part.path) && part.stroke->align != vec::StrokeAlign::Center) {
            // (inside or outside: a centred stroke twice as wide, cut by the fill's area)
            style.width *= 2.0;
            m.align = area;
            m.inside = part.stroke->align == vec::StrokeAlign::Inside;
        }
        path_raster::Rasterizer outline = path_raster::stroke_rasterizer(px, style);
        if (const auto b = outline.pixel_bounds()) m.box = joined(m.box, box_of(*b));
        m.stroke = std::move(outline);
        m.stroke_paint = paint_of(part.stroke->fill, k, colour, true);
        m.stroke_opacity = static_cast<float>(std::clamp(part.stroke->opacity, 0.0, 1.0));
        if (m.stroke_paint.kind == Paint::Kind::None) m.stroke.reset();
    }
}

Compiled::~Compiled() = default;
Compiled::Compiled(Compiled&&) noexcept = default;
Compiled& Compiled::operator=(Compiled&&) noexcept = default;

std::optional<Box> Compiled::box() const { return impl_->box; }

std::vector<std::array<float, 4>> Compiled::rgba(const Box& box) const {
    const Impl& m = *impl_;
    const int w = std::max(0, box.width()), h = std::max(0, box.height());
    std::vector<std::array<float, 4>> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), {0, 0, 0, 0});
    if (out.empty()) return out;
    const path_raster::PixelBox pb = pixel_box_of(box);
    if (m.fill) {
        const std::vector<float> cover = m.fill->render(pb);
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (cover[i] <= 0.0f) continue;
            std::array<float, 4> p = m.fill_paint.at(box.x0 + static_cast<int>(i % w), box.y0 + static_cast<int>(i / w));
            p[3] *= cover[i];
            out[i] = p;
        }
    }
    if (m.stroke) {
        std::vector<float> cover = m.stroke->render(pb);
        if (m.align) {
            const std::vector<float> area = m.align->render(pb);
            for (std::size_t i = 0; i < cover.size(); ++i) cover[i] *= m.inside ? area[i] : 1.0f - area[i];
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (cover[i] <= 0.0f) continue;
            const std::array<float, 4> s = m.stroke_paint.at(box.x0 + static_cast<int>(i % w), box.y0 + static_cast<int>(i / w));
            std::array<float, 4>& o = out[i];
            const float sa = s[3] * cover[i] * m.stroke_opacity;
            const float da = o[3];
            const float a = sa + da * (1.0f - sa);
            if (a > 0.0f) {
                for (int c = 0; c < 3; ++c) o[c] = (s[c] * sa + o[c] * da * (1.0f - sa)) / a;
            }
            o[3] = a;
        }
    }
    if (m.opacity < 1.0f) {
        for (auto& p : out) p[3] *= m.opacity;
    }
    return out;
}

void draw(Image& out, const Box& area, int dpi, const std::vector<const ShapePart*>& parts, const std::vector<std::int64_t>* colour) {
    constexpr int kBand = 128;
    for (const ShapePart* part : parts) {
        const Compiled one(*part, dpi, colour);
        const std::optional<Box> b = one.box();
        if (!b) continue;
        const Box at = clipped(*b, area);
        if (at.width() <= 0 || at.height() <= 0) continue;
        for (int y = at.y0; y < at.y1; y += kBand) {
            const Box band{at.x0, y, at.x1, std::min(at.y1, y + kBand)};
            const std::vector<std::array<float, 4>> rgba = one.rgba(band);
            std::string bytes(rgba.size() * 4, '\0');
            bool seen = false;
            for (std::size_t i = 0; i < rgba.size(); ++i) {
                const unsigned char a = byte(rgba[i][3]);
                if (a == 0) continue;
                seen = true;
                bytes[i * 4] = static_cast<char>(byte(rgba[i][0]));
                bytes[i * 4 + 1] = static_cast<char>(byte(rgba[i][1]));
                bytes[i * 4 + 2] = static_cast<char>(byte(rgba[i][2]));
                bytes[i * 4 + 3] = static_cast<char>(a);
            }
            if (!seen) continue;
            const Image piece = Image::frombytes("RGBA", Size{band.width(), band.height()}, bytes);
            out.alpha_composite(piece, Point{band.x0 - area.x0, band.y0 - area.y0});
        }
    }
}

Image picture(const Box& area, int dpi, const std::vector<const ShapePart*>& parts, const std::vector<std::int64_t>* colour) {
    Image out = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    draw(out, area, dpi, parts, colour);
    return out;
}

std::optional<core::Patch> as_patch(const core::Page* page, const ShapePart& part) {
    const int dpi = fills::kFillDpi;
    const Compiled one(part, dpi);
    std::optional<Box> b = one.box();
    if (!b) return std::nullopt;
    if (part.path.inverted) {  // (an inverted fill: as far as the page)
        if (page == nullptr) return std::nullopt;
        b = clipped(*b, Box{0, 0, mm_to_px(page->spec.width_mm.value(), dpi), mm_to_px(page->spec.height_mm.value(), dpi)});
    }
    if (b->width() <= 0 || b->height() <= 0) return std::nullopt;
    limits::check_picture(b->width(), b->height(), "the shape");
    const Image drawn = picture(*b, dpi, {&part});
    core::Patch templ;
    templ.attrs["mode"] = "image";
    templ.attrs["opacity"] = 1.0;
    return selection::to_patch(drawn, b->x0, b->y0, templ, dpi);
}

void rasterize_parts(const core::Page& page, core::Layer& layer) {
    for (const ShapePart& part : layer.shapes->items) {
        if (auto patch = as_patch(&page, part)) layer.patches.push_back(std::move(*patch));
    }
    layer.shapes = vec::empty_shapes();
}

// --- geometry ---------------------------------------------------------------------------------------------------------

namespace {

void grow(std::optional<std::array<double, 4>>& box, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (!box) {
        box = std::array<double, 4>{x, y, x, y};
        return;
    }
    auto& b = *box;
    b[0] = std::min(b[0], x);
    b[1] = std::min(b[1], y);
    b[2] = std::max(b[2], x);
    b[3] = std::max(b[3], y);
}

}  // namespace

std::optional<std::array<double, 4>> drawn_bounds(const ShapePart& part) {
    if (part.path.empty() || part.path.inverted) return std::nullopt;
    constexpr double kTol = 0.001;  // (mm)
    std::optional<std::array<double, 4>> fill_box;
    for (const path_raster::Polyline& line : path_raster::flatten_path(part.path, kTol))
        for (const auto& [x, y] : line.pts) grow(fill_box, x, y);
    std::optional<std::array<double, 4>> out;
    const bool filled = fills(part.fill);
    if (filled && fill_box) out = fill_box;
    if (part.stroke && part.stroke->width_mm > 0 && part.stroke->opacity > 0) {
        if (closed(part.path) && part.stroke->align == vec::StrokeAlign::Inside) {
            if (fill_box && !out) out = fill_box;
        } else {
            path_raster::StrokeStyle style = path_raster::stroke_style(*part.stroke, 1.0);
            if (closed(part.path) && part.stroke->align == vec::StrokeAlign::Outside) style.width *= 2.0;
            const auto lines = path_raster::flatten_path(part.path, std::max(1e-3, std::min(0.01, style.width * 0.1)));
            for (const auto& poly : path_raster::stroke_polygons(lines, style, kTol))
                for (const auto& [x, y] : poly) grow(out, x, y);
        }
    }
    return out;
}

std::optional<std::pair<double, double>> first_anchor(const ShapePart& part) {
    for (const Subpath& s : part.path.subpaths)
        if (!s.knots.empty()) return std::pair<double, double>{s.knots.front().anchor.x, s.knots.front().anchor.y};
    return std::nullopt;
}

namespace {

constexpr std::int64_t kBakePixels = 40'000'000;  // (the grid a bake works on: past it, a coarser one, down to 200 dpi)

// The resolution a box of `mm` (w, h) is baked at: `dpi`, or less so that it stays within kBakePixels (200 dpi at least).
double bake_dpi(double w_mm, double h_mm, double dpi) {
    const double px = (w_mm / 25.4 * dpi) * (h_mm / 25.4 * dpi);
    if (px > static_cast<double>(kBakePixels)) dpi = std::max(200.0, dpi * std::sqrt(static_cast<double>(kBakePixels) / px));
    return dpi;
}

// The coverage of `first` (times `second`'s, times its complement, or alone) over the grid of `dpi`, traced into a path
// in mm within `tolerance_mm`.
enum class With { Alone, Inside, Outside };
Path traced(const path_raster::Rasterizer& first, const path_raster::Rasterizer* second, With with, double dpi, double tolerance_mm) {
    std::optional<path_raster::PixelBox> b = first.pixel_bounds();
    if (!b) return Path{};
    if (with == With::Inside && second != nullptr) {
        const std::optional<path_raster::PixelBox> o = second->pixel_bounds();
        if (!o) return Path{};
        *b = path_raster::PixelBox{std::max(b->x0, o->x0), std::max(b->y0, o->y0), std::min(b->x1, o->x1), std::min(b->y1, o->y1)};
        if (b->empty()) return Path{};
    }
    const path_raster::PixelBox box{b->x0 - 2, b->y0 - 2, b->x1 + 2, b->y1 + 2};
    limits::check_picture(box.width(), box.height(), "the shape");
    std::vector<float> values = first.render(box);
    if (second != nullptr && with != With::Alone) {
        const std::vector<float> other = second->render(box);
        for (std::size_t i = 0; i < values.size(); ++i) values[i] *= with == With::Inside ? other[i] : 1.0f - other[i];
    }
    const double k = 25.4 / dpi;
    return path_trace::trace_mask(values, box, 0.0f, std::max(tolerance_mm / k, 0.05)).transformed({k, 0.0, 0.0, k, 0.0, 0.0});
}

std::array<double, 2> size_mm(const Path& path) {
    const auto b = path.control_bounds();
    if (!b) return {0.0, 0.0};
    return {(*b)[2] - (*b)[0], (*b)[3] - (*b)[1]};
}

}  // namespace

std::pair<double, double> shares(const Path& path, const Path& area, double dpi) {
    if (path.empty()) return {0.0, 0.0};
    const auto [w, h] = size_mm(path);
    dpi = bake_dpi(w, h, dpi);
    const path_raster::Rasterizer p = path_raster::fill_rasterizer(path_raster::to_pixels(path, dpi));
    const std::optional<path_raster::PixelBox> b = p.pixel_bounds();
    if (!b) return {0.0, 0.0};
    limits::check_picture(b->width(), b->height(), "the shape");
    const std::vector<float> mine = p.render(*b);
    const std::vector<float> theirs = path_raster::fill_rasterizer(path_raster::to_pixels(area, dpi)).render(*b);
    double in = 0.0, out = 0.0;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        in += mine[i] * theirs[i];
        out += mine[i] * (1.0f - theirs[i]);
    }
    const double px_mm2 = (25.4 / dpi) * (25.4 / dpi);
    return {in * px_mm2, out * px_mm2};
}

Path baked(const Path& path, double tolerance_mm, double dpi) {
    if (path.empty()) return Path{};
    Path plain = path;
    plain.inverted = false;
    const auto [w, h] = size_mm(plain);
    dpi = bake_dpi(w, h, dpi);
    Path out = traced(path_raster::fill_rasterizer(path_raster::to_pixels(plain, dpi)), nullptr, With::Alone, dpi, tolerance_mm);
    out.inverted = path.inverted;  // (the inversion is laid on last: the same area)
    return out;
}

Path cut(const Path& path, const Path& area, bool inside, double tolerance_mm, double dpi) {
    if (path.empty()) return Path{};
    if (path.inverted) throw core::OpError("an inverted shape cannot be cut by an area");
    const auto [w, h] = size_mm(path);
    dpi = bake_dpi(w, h, dpi);
    const path_raster::Rasterizer p = path_raster::fill_rasterizer(path_raster::to_pixels(path, dpi));
    const path_raster::Rasterizer a = path_raster::fill_rasterizer(path_raster::to_pixels(area, dpi));
    return traced(p, &a, inside ? With::Inside : With::Outside, dpi, tolerance_mm);
}

Path without(const Path& path, const std::vector<std::vector<path_raster::XY>>& polys, double tolerance_mm, double dpi) {
    if (path.empty()) return Path{};
    if (path.inverted) throw core::OpError("an inverted shape cannot be erased");
    const auto [w, h] = size_mm(path);
    dpi = bake_dpi(w, h, dpi);
    const double k = dpi / 25.4;
    std::vector<std::vector<path_raster::XY>> px;
    px.reserve(polys.size());
    for (const auto& poly : polys) {
        std::vector<path_raster::XY> one;
        one.reserve(poly.size());
        for (const auto& [x, y] : poly) one.emplace_back(x * k, y * k);
        px.push_back(std::move(one));
    }
    path_raster::Rasterizer band;
    band.add_component(px, vec::PathOp::Combine, vec::FillRule::NonZero);
    const path_raster::Rasterizer p = path_raster::fill_rasterizer(path_raster::to_pixels(path, dpi));
    return traced(p, &band, With::Outside, dpi, tolerance_mm);
}

// --- bending and moving ------------------------------------------------------------------------------------------------

namespace {

vec::XY lerp(const vec::XY& a, const vec::XY& b, double t) { return vec::XY{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

// The cubic [p0, c1, c2, p1] between t0 and t1 (de Casteljau, twice).
std::array<vec::XY, 4> piece(const std::array<vec::XY, 4>& s, double t0, double t1) {
    const auto split_right = [](const std::array<vec::XY, 4>& c, double t) {  // (the part from t to 1)
        const vec::XY a = lerp(c[0], c[1], t), b = lerp(c[1], c[2], t), d = lerp(c[2], c[3], t);
        const vec::XY e = lerp(a, b, t), f = lerp(b, d, t);
        const vec::XY g = lerp(e, f, t);
        return std::array<vec::XY, 4>{g, f, d, c[3]};
    };
    const auto split_left = [](const std::array<vec::XY, 4>& c, double t) {  // (the part from 0 to t)
        const vec::XY a = lerp(c[0], c[1], t), b = lerp(c[1], c[2], t), d = lerp(c[2], c[3], t);
        const vec::XY e = lerp(a, b, t), f = lerp(b, d, t);
        const vec::XY g = lerp(e, f, t);
        return std::array<vec::XY, 4>{c[0], a, e, g};
    };
    const std::array<vec::XY, 4> right = t0 > 0.0 ? split_right(s, t0) : s;
    if (t1 >= 1.0) return right;
    return split_left(right, (t1 - t0) / (1.0 - t0));
}

double hull_length(const std::array<vec::XY, 4>& s) {
    return std::hypot(s[1].x - s[0].x, s[1].y - s[0].y) + std::hypot(s[2].x - s[1].x, s[2].y - s[1].y) + std::hypot(s[3].x - s[2].x, s[3].y - s[2].y);
}

Json moved_fill(const Json& fill, const std::function<vec::XY(const vec::XY&)>& go) {
    if (!fill.is_object() || !fill.contains("gradient") || !fill["gradient"].is_object()) return fill;
    Json out = fill;
    Json& g = out["gradient"];
    for (const char* key : {"from", "to"}) {
        const auto it = g.find(key);
        if (it == g.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) continue;
        const vec::XY p = go(vec::XY{(*it)[0].get<double>(), (*it)[1].get<double>()});
        *it = Json::array({core::py_round(p.x, 4), core::py_round(p.y, 4)});
    }
    return out;
}

}  // namespace

Path bent(const Path& path, const MapFunction& go, double step_mm) {
    const auto map = [&](const vec::XY& p) {
        const auto [x, y] = go(p.x, p.y);
        return vec::XY{core::py_round(x, 4), core::py_round(y, 4)};
    };
    Path out = path;
    for (Subpath& s : out.subpaths) {
        if (s.knots.size() < 2) {
            for (Knot& k : s.knots) k = Knot::corner(map(k.anchor).x, map(k.anchor).y);
            continue;
        }
        const std::vector<std::array<vec::XY, 4>> segments = s.segments();
        std::vector<Knot> knots;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const std::size_t n = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(hull_length(segments[i]) / std::max(step_mm, 0.01))), 1, 4096);
            for (std::size_t j = 0; j < n; ++j) {
                const std::array<vec::XY, 4> q = piece(segments[i], static_cast<double>(j) / n, static_cast<double>(j + 1) / n);
                if (knots.empty()) {
                    const vec::XY a = map(q[0]);
                    knots.push_back(Knot{a, a, map(q[1]), s.knots.front().smooth});
                } else {
                    knots.back().out = map(q[1]);
                }
                const vec::XY b = map(q[3]);
                const bool original = j + 1 == n;  // (the next original anchor; the cuts between are smooth)
                const bool smooth = original ? s.knots[(i + 1) % s.knots.size()].smooth : true;
                knots.push_back(Knot{b, map(q[2]), b, smooth});
            }
        }
        if (s.closed && knots.size() > 1) {  // (the last cut is the first anchor again)
            knots.front().in = knots.back().in;
            knots.pop_back();
        } else if (!s.closed && !knots.empty()) {
            knots.front().in = knots.front().anchor;
        }
        s.knots = std::move(knots);
    }
    return out;
}

ShapePart transformed(const ShapePart& part, const vec::Affine& m, bool scale_stroke) {
    ShapePart out = part;
    out.path = part.path.transformed(m);
    const auto go = [&](const vec::XY& p) { return vec::XY{m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]}; };
    out.fill = moved_fill(part.fill, go);
    if (out.stroke) {
        out.stroke->fill = moved_fill(part.stroke->fill, go);
        if (scale_stroke) out.stroke->width_mm = core::py_round(part.stroke->width_mm * std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2])), 4);
    }
    // (PhotoCraft's transform_shape: the live shape kept while the map keeps the axes)
    const double a = m[0], b = m[1], c = m[2], d = m[3], tx = m[4], ty = m[5];
    std::optional<vec::LiveShape> live;
    if (part.live) {
        const vec::LiveShape& l = *part.live;
        const bool aligned = std::fabs(b) < 1e-12 && std::fabs(c) < 1e-12;
        const bool turned = std::fabs(a) < 1e-12 && std::fabs(d) < 1e-12;
        const auto rect = [&](const std::array<double, 4>& r) {
            const vec::XY p = go({r[0], r[1]}), q = go({r[0] + r[2], r[1] + r[3]});
            return std::array<double, 4>{std::min(p.x, q.x), std::min(p.y, q.y), std::fabs(q.x - p.x), std::fabs(q.y - p.y)};
        };
        if (aligned && a > 0 && d > 0) {
            vec::LiveShape n = l;
            n.rect = {l.rect[0] * a + tx, l.rect[1] * d + ty, l.rect[2] * a, l.rect[3] * d};
            const double k = std::min(a, d);
            for (double& r : n.radii) r *= k;
            n.from = go(l.from);
            n.to = go(l.to);
            live = n;
        } else if (aligned || turned) {
            const double k = std::min(std::fabs(a) + std::fabs(b), std::fabs(c) + std::fabs(d));
            const bool even = std::all_of(l.radii.begin(), l.radii.end(), [&](double r) { return std::fabs(r - l.radii[0]) < 1e-9; });
            vec::LiveShape n = l;
            if (l.kind == vec::LiveShape::Kind::Rect && even) {
                n.rect = rect(l.rect);
                for (double& r : n.radii) r *= k;
                live = n;
            } else if (l.kind == vec::LiveShape::Kind::Ellipse) {
                n.rect = rect(l.rect);
                live = n;
            } else if (l.kind == vec::LiveShape::Kind::Line) {
                n.from = go(l.from);
                n.to = go(l.to);
                n.weight_mm = l.weight_mm * k;
                live = n;
            }
        }
    }
    if (live) {
        for (double& v : live->rect) v = core::py_round(v, 4);
        for (double& v : live->radii) v = core::py_round(v, 4);
        live->from = vec::XY{core::py_round(live->from.x, 4), core::py_round(live->from.y, 4)};
        live->to = vec::XY{core::py_round(live->to.x, 4), core::py_round(live->to.y, 4)};
        live->weight_mm = core::py_round(live->weight_mm, 4);
    }
    out.live = live;
    return out;
}

}  // namespace genko::render::shape_parts
