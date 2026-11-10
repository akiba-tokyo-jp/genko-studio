// A smart object's smart filters that bend its picture (render/smart_bends.hpp, PC3 A5). PhotoCraft v0.2.0's
// engine/src/distort_cmds.rs:233-272 (Liquify, Puppet Warp and Perspective Warp kept as smart filters and applied again
// to the picture — apply_to_surface, puppet_surface over what it shows, perspective_surface; MIT, see
// third_party/photocraft/NOTICE.md), with 節 C's algorithms (render/liquify_field, render/puppet, render/perspective_warp)
// and maps (render/layer_transform's readers).

#include "render/smart_bends.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <mutex>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/layer_transform.hpp"
#include "render/liquify_field.hpp"
#include "render/liquify_strokes.hpp"
#include "render/own_pixels.hpp"
#include "render/perspective_warp.hpp"
#include "render/puppet.hpp"

namespace genko::render::smart_bends {

using core::Json;
using core::OpError;
using own_pixels::clamp_window;
using own_pixels::Window;
using own_pixels::window_of;

namespace {

constexpr int kDpi = 200;                   // (the working grid: a puppet warp's mesh is on it, as transform_layers')
constexpr double kPx = kDpi / 25.4;         // (working pixels a mm)
constexpr double kUnit = 25.4 / kDpi;       // (mm: a working pixel)
constexpr double kLink = 0.5 / kPx;         // (a perspective warp's corners within half a working pixel are one)

void only(const Json& params, std::initializer_list<std::string_view> keys, const std::string& what) {
    if (!params.is_object()) throw OpError(what + "'s settings are an object");
    for (const auto& [key, value] : params.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) throw OpError(what + " has no key " + key);
    }
}

resample::Interp interp_of(const Json& params) {
    const Json* v = core::get(params, "interp");
    if (v == nullptr || v->is_null()) return resample::Interp::Bicubic;
    const auto found = v->is_string() ? resample::interp_from(v->get<std::string>()) : std::nullopt;
    if (!found) throw OpError("interp must be nearest, bilinear or bicubic");
    return *found;
}

std::string interp_name(resample::Interp interp) {
    return interp == resample::Interp::Nearest ? "nearest" : interp == resample::Interp::Bilinear ? "bilinear" : "bicubic";
}

// A perspective warp's planes, straightened when it says so (its spec's destinations then the straightened ones).
std::vector<perspective::Plane> planes_of(layer_transform::PerspectiveSpec& spec) {
    std::vector<perspective::Plane> planes;
    for (std::size_t i = 0; i < spec.src.size(); ++i) {
        perspective::Plane p;
        for (std::size_t k = 0; k < 4; ++k) {
            p.src[k] = {spec.src[i][k].x, spec.src[i][k].y};
            p.dst[k] = {spec.dst[i][k].x, spec.dst[i][k].y};
        }
        planes.push_back(p);
    }
    if (spec.straighten) {
        perspective::straighten(planes, *perspective::straighten_from(*spec.straighten), kLink);
        for (std::size_t i = 0; i < planes.size(); ++i)
            for (std::size_t k = 0; k < 4; ++k) spec.dst[i][k] = layer_transform::XY{planes[i].dst[k][0], planes[i].dst[k][1]};
        spec.straighten.reset();
    }
    return planes;
}

std::optional<double> mesh_of(const Json& params) {
    const Json* v = core::get(params, "mesh_mm");
    if (v == nullptr || v->is_null()) return std::nullopt;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError("mesh_mm must be a number");
    return v->get<double>();
}

// The window of the page's pixels the plane of `size` covers within, from a box of the page in mm.
Window pixels_of(double x0, double y0, double x1, double y1, double sx, double sy, int pad, Size size) {
    return clamp_window(window_of(x0 * sx, y0 * sy, x1 * sx, y1 * sy, pad), size.width, size.height);
}

// --- a puppet warp's mesh, remembered ------------------------------------------------------------------------------

// The mesh deformed for a coverage and pins (the same ones: the same mesh, bit for bit); the last few kept, compared
// whole (the screen draws the same smart object at each zoom).
struct Solved {
    std::string pins;  // (the warp's settings as kept)
    puppet::Coverage coverage;
    std::shared_ptr<const puppet::Deformed> deformed;
};

std::shared_ptr<const puppet::Deformed> deformed_for(const puppet::Coverage& coverage, const puppet::Warp& warp, const std::string& pins) {
    static std::mutex mutex;
    static std::list<Solved> kept;
    const auto same = [&](const Solved& s) {
        return s.pins == pins && s.coverage.width == coverage.width && s.coverage.height == coverage.height && s.coverage.x0 == coverage.x0 &&
               s.coverage.y0 == coverage.y0 && s.coverage.alpha == coverage.alpha;
    };
    {
        std::lock_guard lock(mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (!same(*it)) continue;
            kept.splice(kept.begin(), kept, it);
            return kept.front().deformed;
        }
    }
    auto made = std::make_shared<const puppet::Deformed>(puppet::deform(coverage, warp));
    std::lock_guard lock(mutex);
    kept.push_front(Solved{pins, coverage, made});
    while (kept.size() > 4) kept.pop_back();
    return made;
}

// The window of the page's pixels a forward map (source pixels → the page's) takes the source window to: its edges and a
// grid inside it sampled (render/layer_transform's way), padded, within the page.
Window landing(const resample::Forward& f, const Window& src, Size size) {
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    constexpr int kSamples = 24;
    for (int j = 0; j <= kSamples; ++j) {
        for (int i = 0; i <= kSamples; ++i) {
            const auto [px, py] = f(src.width() * static_cast<double>(i) / kSamples, src.height() * static_cast<double>(j) / kSamples);
            if (!std::isfinite(px) || !std::isfinite(py)) continue;
            x0 = std::min(x0, px);
            y0 = std::min(y0, py);
            x1 = std::max(x1, px);
            y1 = std::max(y1, py);
        }
    }
    if (x0 > x1) return Window{};
    const int pad = 4 + static_cast<int>(std::ceil(std::max(src.width(), src.height()) / static_cast<double>(kSamples)));
    return clamp_window(window_of(x0, y0, x1, y1, pad), size.width, size.height);
}

}  // namespace

Json settled(std::string_view kind, const Json& params, const core::Page& page) {
    (void)page;
    if (kind == "liquify") {
        only(params, {"strokes", "mesh_mm"}, "liquify");
        const Json* strokes = core::get(params, "strokes");
        if (strokes == nullptr) throw OpError("strokes is [{tool, size_mm, density, pressure, rate, points, amount?}…]");
        (void)read_liquify_strokes(*strokes);
        Json out{{"strokes", *strokes}};
        if (const auto mesh = mesh_of(params)) {
            if (*mesh < kUnit || *mesh > 64.0 * kUnit)
                throw OpError("mesh_mm must be between " + core::py_repr(Json(kUnit)) + " and " + core::py_repr(Json(64.0 * kUnit)));
            out["mesh_mm"] = *mesh;
        }
        return out;
    }
    if (kind == "perspective_warp") {
        only(params, {"planes", "straighten", "interp"}, "perspective");
        layer_transform::PerspectiveSpec spec = layer_transform::perspective_from_json(params);
        const std::vector<perspective::Plane> planes = planes_of(spec);
        if (!perspective::Map::make(planes, kLink)) throw OpError("a plane is degenerate: its corners must make a quad");
        Json out = layer_transform::perspective_to_json(spec);
        out["interp"] = interp_name(interp_of(params));
        return out;
    }
    if (kind == "puppet_warp") {
        only(params, {"pins", "mode", "density", "expansion_mm", "interp"}, "puppet");
        Json out = layer_transform::puppet_to_json(layer_transform::puppet_from_json(params));
        out["interp"] = interp_name(interp_of(params));
        return out;
    }
    throw OpError("the filter " + std::string(kind) + " does not bend the picture");
}

std::optional<Bent> bend(const core::Page& page, std::string_view kind, const Json& params, Size size, const std::optional<Box>& shown, const Reader& read) {
    const double pw = page.spec.width_mm.value(), ph = page.spec.height_mm.value();
    if (size.width <= 0 || size.height <= 0 || !(pw > 0) || !(ph > 0)) return std::nullopt;
    const double sx = size.width / pw, sy = size.height / ph;  // (the picture's pixels a mm)
    if (kind == "liquify") {
        const Json* strokes = core::get(params, "strokes");
        if (strokes == nullptr) throw OpError("strokes is [{tool, size_mm, density, pressure, rate, points, amount?}…]");
        const liquify::Field field = liquify_field(pw, ph, mesh_of(params), read_liquify_strokes(*strokes));
        const auto moved = field.moved_box();
        if (field.is_identity() || !moved) return std::nullopt;
        const double most = field.max_displacement();
        // (what the field reaches drawn again from the picture around it; the rest as it was: liquify_strokes' own way)
        const Window dst = pixels_of((*moved)[0], (*moved)[1], (*moved)[2], (*moved)[3], sx, sy, 2, size);
        if (dst.empty()) return std::nullopt;
        const int pad = static_cast<int>(std::ceil(most * std::max(sx, sy))) + 3;
        const Window src = clamp_window(Window{dst.x0 - pad, dst.y0 - pad, dst.x1 + pad, dst.y1 + pad}, size.width, size.height);
        const resample::Plane plane = read(src.x0, src.y0, src.x1, src.y1);
        const auto g = [&](double X, double Y) {
            const double x = X / sx, y = Y / sy;
            const auto d = field.sample_cubic(x, y);
            return std::pair<double, double>{(x + d[0]) * sx - src.x0, (y + d[1]) * sy - src.y0};
        };
        return Bent{resample::remap(plane, g, dst.width(), dst.height(), resample::Interp::Bicubic, dst.x0, dst.y0), dst.x0, dst.y0, false};
    }
    if (!shown || shown->width() <= 0 || shown->height() <= 0) return std::nullopt;
    const resample::Interp interp = interp_of(params);
    if (kind == "perspective_warp") {
        layer_transform::PerspectiveSpec spec = layer_transform::perspective_from_json(params);
        const std::vector<perspective::Plane> planes = planes_of(spec);
        if (perspective::is_identity(planes)) return std::nullopt;
        const auto made = perspective::Map::make(planes, kLink);
        if (!made) throw OpError("a plane is degenerate: its corners must make a quad");
        const perspective::Map map = *made;
        const Window src = clamp_window(Window{shown->x0 - 1, shown->y0 - 1, shown->x1 + 1, shown->y1 + 1}, size.width, size.height);
        const resample::Forward f = [&](double u, double v) {
            const auto p = map.map((u + src.x0) / sx, (v + src.y0) / sy);
            return std::pair<double, double>{p[0] * sx, p[1] * sy};
        };
        const Window dst = landing(f, src, size);
        if (dst.empty()) return Bent{resample::Plane(0, 0, 4, true), 0, 0, true};
        const resample::Plane plane = read(src.x0, src.y0, src.x1, src.y1);
        return Bent{resample::mesh(plane, f, dst.width(), dst.height(), interp, dst.x0, dst.y0), dst.x0, dst.y0, true};
    }
    if (kind != "puppet_warp") throw OpError("the filter " + std::string(kind) + " does not bend the picture");
    const puppet::Warp w = layer_transform::puppet_from_json(params);
    if (w.is_identity()) return std::nullopt;
    // (solved in the working 200 dpi pixels, over what the picture shows grown by the expansion: transform_layers' way)
    puppet::Warp px = w;
    px.expansion = w.expansion * kPx;
    for (puppet::Pin& p : px.pins) {
        p.src = {p.src[0] * kPx, p.src[1] * kPx};
        p.dst = {p.dst[0] * kPx, p.dst[1] * kPx};
    }
    const double grow = std::max(px.expansion, 0.0) / kPx + 1.0;
    const Window work = clamp_window(window_of((shown->x0 / sx - grow) * kPx, (shown->y0 / sy - grow) * kPx, (shown->x1 / sx + grow) * kPx,
                                               (shown->y1 / sy + grow) * kPx, 0),
                                     static_cast<int>(std::ceil(pw * kPx)), static_cast<int>(std::ceil(ph * kPx)));
    if (work.empty()) return std::nullopt;
    // (the coverage: each working pixel the most alpha of the picture's pixels over it)
    const auto at_x = [&](double working) { return std::clamp(static_cast<int>(std::floor(working / kPx * sx)), 0, size.width); };
    const auto at_y = [&](double working) { return std::clamp(static_cast<int>(std::floor(working / kPx * sy)), 0, size.height); };
    const Window under{at_x(work.x0), at_y(work.y0), std::max(at_x(work.x0) + 1, std::min(size.width, at_x(work.x1) + 1)),
                       std::max(at_y(work.y0) + 1, std::min(size.height, at_y(work.y1) + 1))};
    const resample::Plane seen = read(under.x0, under.y0, under.x1, under.y1);
    puppet::Coverage cov;
    cov.width = work.width();
    cov.height = work.height();
    cov.x0 = work.x0;
    cov.y0 = work.y0;
    cov.alpha.assign(std::size_t(cov.width) * std::size_t(cov.height), 0.0f);
    const int a = seen.channels - 1;
    for (int j = 0; j < cov.height; ++j) {
        const int py0 = at_y(work.y0 + j), py1 = std::max(py0 + 1, at_y(work.y0 + j + 1));
        for (int i = 0; i < cov.width; ++i) {
            const int px0 = at_x(work.x0 + i), px1 = std::max(px0 + 1, at_x(work.x0 + i + 1));
            float most = 0.0f;
            for (int y = py0; y < py1; ++y) {
                if (y - under.y0 < 0 || y - under.y0 >= seen.height) continue;
                for (int x = px0; x < px1; ++x) {
                    if (x - under.x0 < 0 || x - under.x0 >= seen.width) continue;
                    most = std::max(most, seen.at(x - under.x0, y - under.y0)[a]);
                }
            }
            cov.alpha[std::size_t(j) * std::size_t(cov.width) + std::size_t(i)] = most;
        }
    }
    const std::shared_ptr<const puppet::Deformed> deformed = deformed_for(cov, px, core::dump_canonical(layer_transform::puppet_to_json(w)));
    if (deformed->mesh.empty()) return std::nullopt;  // (nothing shown to pin)
    // (the mesh's triangles drawn where they went: render/layer_transform's through())
    const Window src = clamp_window(Window{shown->x0 - 1, shown->y0 - 1, shown->x1 + 1, shown->y1 + 1}, size.width, size.height);
    const double kx = sx / kPx, ky = sy / kPx;
    std::vector<resample::Corner> corners;
    corners.reserve(deformed->verts.size());
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (std::size_t i = 0; i < deformed->verts.size(); ++i) {
        const double dx = deformed->verts[i][0] * kx, dy = deformed->verts[i][1] * ky;
        corners.push_back(resample::Corner{dx, dy, deformed->mesh.verts[i][0] * kx - src.x0, deformed->mesh.verts[i][1] * ky - src.y0});
        x0 = std::min(x0, dx);
        y0 = std::min(y0, dy);
        x1 = std::max(x1, dx);
        y1 = std::max(y1, dy);
    }
    std::vector<std::array<std::size_t, 3>> tris;
    tris.reserve(deformed->order.size());
    for (const std::size_t t : deformed->order) tris.push_back(deformed->mesh.tris[t]);
    const Window dst = clamp_window(window_of(x0, y0, x1, y1, 2), size.width, size.height);
    if (dst.empty()) return Bent{resample::Plane(0, 0, 4, true), 0, 0, true};
    const resample::Plane plane = read(src.x0, src.y0, src.x1, src.y1);
    return Bent{resample::triangles(plane, corners, tris, dst.width(), dst.height(), interp, dst.x0, dst.y0), dst.x0, dst.y0, true};
}

}  // namespace genko::render::smart_bends
