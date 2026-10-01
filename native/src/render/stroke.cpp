#include "render/stroke.hpp"

#include <cmath>
#include <vector>

#include "core/error.hpp"
#include "core/pynum.hpp"

namespace genko::render {

namespace {

// Python's max(a, b) / min(a, b) for floats (the first unless the second is larger / smaller).
double pmax(double a, double b) { return b > a ? b : a; }
double pmin(double a, double b) { return b < a ? b : a; }

int round_int(double v) {
    if (!std::isfinite(v)) throw core::Error("value", "cannot convert float to integer");
    return static_cast<int>(std::nearbyint(v));
}

}  // namespace

int stroke_mm_to_px(double mm, int dpi) {
    const int px = round_int(mm / 25.4 * dpi);
    return px > 1 ? px : 1;
}

void stamp_polyline(Draw& draw, const core::PenPoints& points, int dpi, double width_mm, const Ink& fill, bool coords_mm) {
    if (points.size() < 2) return;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const core::PenPoint& a = points[i];
        const core::PenPoint& b = points[i + 1];
        const double pressure = a.p.value_or(1.0);
        const int radius = [&] {
            const int r = round_int(stroke_mm_to_px(width_mm * pmax(0.15, pressure), dpi) / 2.0);
            return r > 1 ? r : 1;
        }();
        double ax = a.x;
        double ay = a.y;
        double bx = b.x;
        double by = b.y;
        if (coords_mm) {
            ax = stroke_mm_to_px(ax, dpi);
            ay = stroke_mm_to_px(ay, dpi);
            bx = stroke_mm_to_px(bx, dpi);
            by = stroke_mm_to_px(by, dpi);
        }
        const double distance = core::py_pow(core::py_pow(bx - ax, 2) + core::py_pow(by - ay, 2), 0.5);
        const auto steps = static_cast<int>(pmax(1.0, std::trunc(distance)));
        for (int k = 0; k <= steps; ++k) {
            const double t = static_cast<double>(k) / steps;
            const double x = ax + (bx - ax) * t;
            const double y = ay + (by - ay) * t;
            draw.ellipse(BoxF{x - radius, y - radius, x + radius, y + radius}, fill);
        }
    }
}

void draw_stroke_mm(Draw& draw, const core::PenPoints& points, int dpi, double width_mm, const Ink& fill,
                    bool pressure_scale, double floor) {
    if (points.empty()) return;
    const double scale = dpi / 25.4;
    struct P {
        double x, y, r;
    };
    std::vector<P> pts;
    pts.reserve(points.size());
    for (const core::PenPoint& pt : points) {
        const double pressure = (pt.p && pressure_scale) ? *pt.p : 1.0;
        const double radius = pmax(0.5, width_mm * pmax(floor, pmin(1.5, pressure)) * scale / 2);
        pts.push_back(P{pt.x * scale, pt.y * scale, radius});
    }
    if (pts.size() == 1) {
        const P& p = pts[0];
        draw.ellipse(BoxF{p.x - p.r, p.y - p.r, p.x + p.r, p.y + p.r}, fill);
        return;
    }
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const P& a = pts[i];
        const P& b = pts[i + 1];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length = core::py_hypot(dx, dy);
        if (length > 1e-6) {
            const double nx = -dy / length;
            const double ny = dx / length;
            const std::vector<PointD> quad{{a.x + nx * a.r, a.y + ny * a.r},
                                           {b.x + nx * b.r, b.y + ny * b.r},
                                           {b.x - nx * b.r, b.y - ny * b.r},
                                           {a.x - nx * a.r, a.y - ny * a.r}};
            draw.polygon(quad, fill);
        }
        draw.ellipse(BoxF{b.x - b.r, b.y - b.r, b.x + b.r, b.y + b.r}, fill);
    }
    const P& p = pts[0];
    draw.ellipse(BoxF{p.x - p.r, p.y - p.r, p.x + p.r, p.y + p.r}, fill);
}

}  // namespace genko::render
