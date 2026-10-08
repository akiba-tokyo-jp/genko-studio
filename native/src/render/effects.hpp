#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Effect lines (効果線; Python's genko/effects.py), drawn from a few settings: the same effect always gives the same
// lines. Kinds: focus (集中線), speed (流線, straight or along a path), uni_flash (ウニフラッシュ), beta_flash
// (ベタフラッシュ) and white; each inside its panel (or the page out to the bleed), clear of the shapes in `avoid` and
// inside `within`. The geometry is Python's to the last bit (random.Random seeded with the effect's seed or id,
// CPython's math), so the drawing has the same pixels and an effect turned into pen lines the same points.

namespace genko::render::effects {

struct XY {
    double x = 0.0;
    double y = 0.0;
};

// KINDS, in Python's order.
const std::vector<std::string>& kinds();

// effects.validate(kind, params): core::PyValueError with Python's message for settings that cannot be drawn.
void validate(const core::Json& kind, const core::Json& params);

struct Line {
    std::vector<std::array<double, 3>> points;  // x, y (mm), pressure
    double width_mm = 0.0;
};

struct Fill {
    std::vector<XY> points;
    std::vector<std::int64_t> rgb;
};

struct Shape {
    bool ellipse = false;
    std::array<double, 4> box{};  // ellipse: cx, cy, rx, ry
    std::vector<XY> path;         // a path
};

struct Geometry {
    std::vector<Line> lines;
    std::vector<Fill> fills;
    std::vector<std::int64_t> rgb;
    std::vector<XY> outline;
    std::vector<Shape> avoid;
    std::optional<std::vector<XY>> within;
};

// effects.geometry(effect, page): the lines and fills in page mm.
Geometry geometry(const core::Json& effect, const core::Page& page);

// effects.area(effect, page): the effect's panel outline (mm) and its box [x, y, w, h] (the page out to the bleed when
// it has no panel).
std::pair<std::vector<XY>, std::array<double, 4>> panel_area(const core::Json& effect, const core::Page& page);
// The centre of a focus line or flash: params' "center", else the middle of its box.
XY centre(const core::Json& params, const std::array<double, 4>& box);
// effects._inner(params, box): the clear middle's radii (mm).
XY inner_size(const core::Json& params, const std::array<double, 4>& box);

// Whether render._draw_effects draws this effect (a known kind, visible); TypeError (Python's AttributeError) for an
// effect that is not an object.
bool drawn(const core::Json& effect);

// effects.draw(image, effect, page, dpi) for the box `box` of a page of size `size`: `image` (RGBA, the box's size)
// with the effect over it.
Image draw(Image image, const core::Json& effect, const core::Page& page, int dpi, Size size, const Box& box);

// effects.INK
inline const std::vector<std::int64_t> kInk{15, 15, 15};

}  // namespace genko::render::effects
