#pragma once

// The lines of BRUSH-01's reference pages (header only, not part of the product): test_paper draws them on its
// full-size pages, test_contract_paper has Python's baseline draw the inherited brushes' page with the same lines.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/brushes.hpp"
#include "core/model.hpp"

namespace genko::test::paper_lines {

// A line of the brush `kind`, `width` mm wide, through (x, y) pressed p.
inline core::Stroke line(std::string id, std::string kind, double width, const std::vector<std::array<double, 3>>& points) {
    core::Stroke s;
    s.id = std::move(id);
    s.kind = std::move(kind);
    s.width_mm = width;
    for (const auto& [x, y, p] : points) {
        s.points.push_back(core::PointF{x, y});
        s.pressure.push_back(p);
    }
    return s;
}

// A wave of n points from (x0, y) to (x1, y), pressed from `p0` to `p1`.
inline std::vector<std::array<double, 3>> wave(double x0, double x1, double y, double amplitude, double p0, double p1, int n = 24) {
    std::vector<std::array<double, 3>> out;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        out.push_back({x0 + (x1 - x0) * t, y + amplitude * std::sin(t * 6.283185307179586), p0 + (p1 - p0) * t});
    }
    return out;
}

// The second reference page (100 × 80 mm): every inherited brush, none with a paper, three lines to a row.
inline std::vector<core::StrokePtr> inherited() {
    std::vector<core::StrokePtr> out;
    int n = 0;
    for (const core::Brush& b : core::builtin_brushes()) {
        const double x = 6 + (n % 3) * 30.0;
        const double y = 7 + (n / 3) * 10.0;
        const double width = std::min(b.width_mm, 3.0);
        out.push_back(std::make_shared<const core::Stroke>(line("inherited-" + b.key, b.key, width, wave(x, x + 24, y, 2, 0.2, 1.0, 16))));
        ++n;
    }
    return out;
}

}  // namespace genko::test::paper_lines
