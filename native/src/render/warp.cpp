#include "render/warp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/draw.hpp"
#include "render/op_limits.hpp"

namespace genko::render::warp {

using core::Json;

namespace {

[[noreturn]] void warp_error(const std::string& message) { throw core::OpError(message); }

// A·x = b by Gaussian elimination with partial pivoting (the largest |pivot|, the first of equal ones, as LAPACK's
// IAMAX picks): nothing when a pivot is exactly zero (numpy.linalg.solve's "Singular matrix").
template <std::size_t N>
std::optional<std::array<double, N>> solve(std::array<std::array<double, N>, N> a, std::array<double, N> b) {
    for (std::size_t col = 0; col < N; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < N; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
        }
        if (a[pivot][col] == 0.0) return std::nullopt;
        std::swap(a[pivot], a[col]);
        std::swap(b[pivot], b[col]);
        for (std::size_t row = col + 1; row < N; ++row) {
            const double factor = a[row][col] / a[col][col];
            for (std::size_t k = col; k < N; ++k) a[row][k] -= factor * a[col][k];
            b[row] -= factor * b[col];
        }
    }
    std::array<double, N> x{};
    for (std::size_t i = N; i-- > 0;) {
        double sum = b[i];
        for (std::size_t k = i + 1; k < N; ++k) sum -= a[i][k] * x[k];
        x[i] = sum / a[i][i];
    }
    return x;
}

// [(float(p[0]), float(p[1])) for p in points]
std::vector<std::pair<double, double>> float_points(const Json& points) {
    std::vector<std::pair<double, double>> out;
    for (const Json& p : core::iterate(points)) {
        const double x = core::finite_float(core::subscript(p, 0), "warp");
        const double y = core::finite_float(core::subscript(p, 1), "warp");
        out.emplace_back(x, y);
    }
    return out;
}

// warp._dense: points every step_mm along the line (pressure following)
core::PenPoints dense(const core::PenPoints& points, double step_mm = 1.5) {
    core::PenPoints out{points.front()};
    std::int64_t total = 0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const core::PenPoint& a = points[i];
        const core::PenPoint& b = points[i + 1];
        const std::int64_t n = std::max<std::int64_t>(1, core::loop_count(core::py_dist(a.x, a.y, b.x, b.y) / step_mm));
        total += n;
        limits::check_count(static_cast<double>(total), static_cast<double>(limits::kSteps), "the line through the warp");
        for (std::int64_t k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            core::PenPoint p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, std::nullopt};
            if (a.p && b.p) p.p = *a.p + (*b.p - *a.p) * t;
            out.push_back(p);
        }
    }
    return out;
}

// warp._scale_at: how much the warp grows things around (x, y)
double scale_at(const Go& go, double x, double y) {
    const double d = 0.5;
    const auto a = go(x, y);
    const auto b = go(x + d, y);
    const auto c = go(x, y + d);
    const double det = (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
    return std::sqrt(std::fabs(det)) / d;
}

}  // namespace

Go mapping(const std::vector<double>& box, const Json& warp) {
    if (box.size() != 4) {
        throw core::PyValueError(box.size() < 4 ? "not enough values to unpack (expected 4, got " + std::to_string(box.size()) + ")"
                                                : "too many values to unpack (expected 4)");
    }
    const double x0 = box[0], y0 = box[1], w = box[2], h = box[3];
    if (w <= 0 || h <= 0) warp_error("the area has no size");
    if (!warp.is_object()) throw core::PyUncaught("AttributeError", "'" + core::py_type_name(warp) + "' object has no attribute 'get'");
    if (core::truthy_at(warp, "perspective")) {
        const std::vector<std::pair<double, double>> corners = float_points(warp["perspective"]);
        if (corners.size() != 4) warp_error("perspective takes four corners: top-left, top-right, bottom-right, bottom-left");
        const std::array<std::pair<double, double>, 4> src{{{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}}};
        double area = 0.0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto [ax, ay] = corners[i];
            const auto [bx, by] = corners[(i + 1) % 4];
            area += ax * by - bx * ay;
        }
        if (std::fabs(area) < 1e-3) warp_error("the four corners must enclose an area");
        // the homography with h33 = 1: eight equations in its other eight numbers
        std::array<std::array<double, 8>, 8> a{};
        std::array<double, 8> b{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto [x, y] = src[i];
            const auto [u, v] = corners[i];
            a[2 * i] = {x, y, 1, 0, 0, 0, -u * x, -u * y};
            b[2 * i] = u;
            a[2 * i + 1] = {0, 0, 0, x, y, 1, -v * x, -v * y};
            b[2 * i + 1] = v;
        }
        const auto solved = solve(a, b);
        if (!solved) warp_error("the four corners must enclose an area");
        const std::array<double, 8> hm = *solved;
        return [hm](double x, double y) {
            const double u = hm[0] * x + hm[1] * y + hm[2];
            const double v = hm[3] * x + hm[4] * y + hm[5];
            const double s = hm[6] * x + hm[7] * y + 1.0;
            return std::pair<double, double>{u / s, v / s};
        };
    }
    if (core::truthy_at(warp, "mesh")) {
        const std::vector<std::pair<double, double>> grid = float_points(warp["mesh"]);
        // warp.mesh_size
        std::int64_t nx = 3;
        std::int64_t ny = 3;
        if (core::truthy_at(warp, "grid")) {
            nx = core::to_int(core::subscript(warp["grid"], 0));
            ny = core::to_int(core::subscript(warp["grid"], 1));
        }
        if (nx < 2 || ny < 2 || nx > 9 || ny > 9 || nx * ny != static_cast<std::int64_t>(grid.size())) {
            warp_error("mesh takes a grid of points row by row: 3×3 (nine) unless grid [across, down] says otherwise (2 to 9 each)");
        }
        const auto cols = static_cast<int>(nx - 1);
        const auto rows = static_cast<int>(ny - 1);
        const auto across = static_cast<std::size_t>(nx);
        return [grid, cols, rows, across, x0, y0, w, h](double x, double y) {
            const double fx = core::py_min(static_cast<double>(cols), core::py_max(0.0, (x - x0) / w * cols));
            const double fy = core::py_min(static_cast<double>(rows), core::py_max(0.0, (y - y0) / h * rows));
            const int cx = std::min(cols - 1, static_cast<int>(fx));
            const int cy = std::min(rows - 1, static_cast<int>(fy));
            const double tx = fx - cx;
            const double ty = fy - cy;
            const auto& p00 = grid[static_cast<std::size_t>(cy) * across + static_cast<std::size_t>(cx)];
            const auto& p10 = grid[static_cast<std::size_t>(cy) * across + static_cast<std::size_t>(cx) + 1];
            const auto& p01 = grid[static_cast<std::size_t>(cy + 1) * across + static_cast<std::size_t>(cx)];
            const auto& p11 = grid[static_cast<std::size_t>(cy + 1) * across + static_cast<std::size_t>(cx) + 1];
            double u = (1 - tx) * (1 - ty) * p00.first + tx * (1 - ty) * p10.first + (1 - tx) * ty * p01.first + tx * ty * p11.first;
            double v = (1 - tx) * (1 - ty) * p00.second + tx * (1 - ty) * p10.second + (1 - tx) * ty * p01.second +
                       tx * ty * p11.second;
            // beyond the box (a line that reaches out of it) the edge's pull carries on
            u += (fx == 0.0 || fx == static_cast<double>(cols)) ? (x - x0 - fx * w / cols) : 0.0;
            v += (fy == 0.0 || fy == static_cast<double>(rows)) ? (y - y0 - fy * h / rows) : 0.0;
            return std::pair<double, double>{u, v};
        };
    }
    warp_error("a warp is perspective (four corners) or mesh (nine points)");
}

core::Stroke warp_stroke(const core::Stroke& stroke, const Go& go) {
    core::PenPoints pts;
    const bool with_pressure = !stroke.pressure.empty();
    const std::size_t n = with_pressure ? std::min(stroke.points.size(), stroke.pressure.size()) : stroke.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        core::PenPoint p{stroke.points[i].x, stroke.points[i].y, std::nullopt};
        if (with_pressure) p.p = stroke.pressure[i];
        pts.push_back(p);
    }
    const core::PenPoints along = pts.size() > 1 ? dense(pts) : pts;
    core::PenPoints moved;
    moved.reserve(along.size());
    for (const core::PenPoint& p : along) {
        const auto [u, v] = go(p.x, p.y);
        if (!std::isfinite(u) || !std::isfinite(v)) warp_error("the transform stretches the area too far");
        moved.push_back(core::PenPoint{u, v, p.p});
    }
    core::Stroke out = core::coerce_stroke(moved);
    if (stroke.points.empty()) throw core::PyUncaught("IndexError", "list index out of range");
    const core::PointF mid = stroke.points[stroke.points.size() / 2];
    out.width_mm = core::py_round(stroke.width_mm * core::py_max(0.1, core::py_min(10.0, scale_at(go, mid.x, mid.y))), 4);
    if (!std::isfinite(out.width_mm)) warp_error("the transform stretches the area too far");
    out.kind = stroke.kind;
    out.rgb = stroke.rgb;
    out.color_rgb = stroke.color_rgb;
    out.opacity = stroke.opacity;
    out.id = stroke.id;
    return out;
}

std::optional<Warped> warp_image(const Image& image, std::int64_t x0, std::int64_t y0, const Go& go, int dpi, int cells,
                                 Resample resample) {
    const double scale = dpi / 25.4;
    const double ox = static_cast<double>(x0);
    const double oy = static_cast<double>(y0);
    const auto fwd = [&](double px, double py) {
        const auto [u, v] = go((px + ox) / scale, (py + oy) / scale);
        return std::pair<double, double>{u * scale, v * scale};
    };
    const int w = image.width();
    const int h = image.height();
    std::vector<std::vector<std::pair<double, double>>> grid(static_cast<std::size_t>(cells) + 1);
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            grid[static_cast<std::size_t>(j)].push_back(fwd(static_cast<double>(w * i) / cells, static_cast<double>(h * j) / cells));
        }
    }
    double min_x = grid[0][0].first, max_x = min_x, min_y = grid[0][0].second, max_y = min_y;
    for (const auto& row : grid) {
        for (const auto& [x, y] : row) {
            min_x = core::py_min(min_x, x);
            max_x = core::py_max(max_x, x);
            min_y = core::py_min(min_y, y);
            max_y = core::py_max(max_y, y);
        }
    }
    for (const double v : {min_x, max_x, min_y, max_y}) {
        if (!std::isfinite(v)) warp_error("the transform stretches the area too far");
    }
    const double nx0 = std::floor(min_x);
    const double ny0 = std::floor(min_y);
    const double nx1 = std::ceil(max_x) + 1;
    const double ny1 = std::ceil(max_y) + 1;
    if (nx1 - nx0 > 20000 || ny1 - ny0 > 20000) warp_error("the transform stretches the area too far");
    limits::check_coordinate(nx0, "the transform");
    limits::check_coordinate(ny0, "the transform");
    const int out_w = static_cast<int>(std::max(1.0, nx1 - nx0));
    const int out_h = static_cast<int>(std::max(1.0, ny1 - ny0));
    limits::check_picture(out_w, out_h, "the transform");
    const bool grey = image.mode() == "L";
    Image out = grey ? Image::create("L", Size{out_w, out_h}, Ink(0)) : Image::create(image.mode(), Size{out_w, out_h}, Ink{0, 0, 0, 0});
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const auto s = [&](int a, int b) {
                return std::pair<double, double>{static_cast<double>(w * a) / cells, static_cast<double>(h * b) / cells};
            };
            const auto s00 = s(i, j), s10 = s(i + 1, j), s01 = s(i, j + 1), s11 = s(i + 1, j + 1);
            const auto d00 = grid[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)];
            const auto d10 = grid[static_cast<std::size_t>(j)][static_cast<std::size_t>(i) + 1];
            const auto d01 = grid[static_cast<std::size_t>(j) + 1][static_cast<std::size_t>(i)];
            const auto d11 = grid[static_cast<std::size_t>(j) + 1][static_cast<std::size_t>(i) + 1];
            using Tri = std::array<std::pair<double, double>, 3>;
            const std::array<std::pair<Tri, Tri>, 2> triangles{{{Tri{s00, s10, s11}, Tri{d00, d10, d11}}, {Tri{s00, s11, s01}, Tri{d00, d11, d01}}}};
            for (const auto& [src, dst] : triangles) {
                Tri local{};
                for (std::size_t k = 0; k < 3; ++k) local[k] = {dst[k].first - nx0, dst[k].second - ny0};
                double lx0 = local[0].first, lx1 = local[0].first, ly0 = local[0].second, ly1 = local[0].second;
                for (const auto& [x, y] : local) {
                    lx0 = core::py_min(lx0, x);
                    lx1 = core::py_max(lx1, x);
                    ly0 = core::py_min(ly0, y);
                    ly1 = core::py_max(ly1, y);
                }
                const double bx0 = std::max(0.0, std::floor(lx0) - 1);
                const double by0 = std::max(0.0, std::floor(ly0) - 1);
                const double bx1 = std::min(static_cast<double>(out.width()), std::ceil(lx1) + 2);
                const double by1 = std::min(static_cast<double>(out.height()), std::ceil(ly1) + 2);
                if (bx1 <= bx0 || by1 <= by0) continue;
                // warp._affine_back: each output pixel of the triangle back to the source triangle
                std::array<std::array<double, 3>, 3> a{};
                for (std::size_t k = 0; k < 3; ++k) a[k] = {local[k].first - bx0, local[k].second - by0, 1.0};
                const auto cx = solve(a, std::array<double, 3>{src[0].first, src[1].first, src[2].first});
                const auto cy = solve(a, std::array<double, 3>{src[0].second, src[1].second, src[2].second});
                if (!cx || !cy) continue;
                const double coeffs[6] = {(*cx)[0], (*cx)[1], (*cx)[2], (*cy)[0], (*cy)[1], (*cy)[2]};
                const Size piece_size{static_cast<int>(bx1 - bx0), static_cast<int>(by1 - by0)};
                const Image piece = image.transform(piece_size, TransformMethod::Affine, coeffs, resample);
                Image mask = Image::create("L", piece_size, Ink(0));
                std::vector<PointD> corners;
                for (const auto& [x, y] : local) corners.push_back(PointD{x - bx0, y - by0});
                // (a little wider than the triangle, so neighbouring pieces leave no seam)
                Draw(mask).polygon(corners, Ink(255), Ink(255));
                out.paste(piece, Point{static_cast<int>(bx0), static_cast<int>(by0)}, &mask);
            }
        }
    }
    return Warped{std::move(out), static_cast<std::int64_t>(nx0), static_cast<std::int64_t>(ny0)};
}

}  // namespace genko::render::warp
