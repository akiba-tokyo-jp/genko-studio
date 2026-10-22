#pragma once

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/stroke_tools.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/retouch_dabs.hpp"

// PC3 B4 (SPEC.md COLOR-06「修復」), shared with PC4 E5 (COLOR-09「画素を変える筆」): what the brushes that change a paint
// layer's pixels take and how they find their way on it — the arguments every brush takes (brushwork: points in mm, a
// diameter, hardness, opacity, flow, spacing, the blend, the selection), the layer's grid (its pixels per mm), the
// windows of a layer or of the page as it shows, and the selection's coverage on the grid. render/ops_retouch (the
// retouching brushes) and render/ops_paint_tools (Dodge, Burn, Sponge, Blur, Sharpen, Smudge, the Mixer Brush, the
// Background Eraser, Color Replacement) take them from here.

namespace genko::render::brushing {

// --- the arguments ---------------------------------------------------------------------------------------------------

// A key of the op (none when it is not given or null).
const core::Json* field(const core::Json& op, const char* key);
// A number, true or false, one of the names allowed (the first when not given); OpError for anything else.
double number(const core::Json& op, const char* key, double fallback);
bool flag(const core::Json& op, const char* key, bool fallback);
std::string choice(const core::Json& op, const char* key, std::initializer_list<const char*> allowed);
// [x, y] in mm; none when not given.
std::optional<std::pair<double, double>> pair_mm(const core::Json& op, const char* key);
// A length or a place reported in mm (4 decimals).
double rounded(double v);

// What every brush takes (Photoshop's: a diameter, hardness, opacity, flow and spacing; the blend; the selection).
struct Brushwork {
    std::size_t at = 0;
    std::string id;
    core::PenPoints points;  // (mm, a pressure each when given)
    double width_mm = 5.0;
    float hardness = 1.0f, opacity = 1.0f, flow = 1.0f, spacing = 0.25f;
    std::string blend = "normal";
    const core::Json* area = nullptr;
};
// The op's page, layer_id, points, width_mm (0..500), hardness (0..100), opacity (1..100), flow (1..100), spacing
// (1..1000 % of the diameter), blend and area. OpError for what is missing or wrong.
Brushwork brushwork(const core::Document& doc, const core::Json& op);

// --- the layer's grid ------------------------------------------------------------------------------------------------

// A layer's grid over its page: its size and pixels per mm.
struct Grid {
    Size size;
    double sx = 1.0, sy = 1.0;
    Box whole() const { return Box{0, 0, size.width, size.height}; }
    double px(double mm) const { return mm * std::sqrt(sx * sy); }
};

Box meet(const Box& a, const Box& b);
bool empty(const Box& b);
Box grown(const Box& b, int by);

// The layer made ready to be worked (refused when locked or not a paint layer; precise pen lines made precise pixels, as
// image_ops::layer_pixels makes them), its grid, and a pixel of it (the kind its pixels are: `like`).
struct Ready {
    Grid grid;
    image_ops::Pixels like;
};
Ready ready(core::OpContext& c, std::size_t at, const std::string& id);
// The grid of `size` pixels over the page (OpError for a grid of no pixels).
Grid grid_of(const core::Page& page, Size size, const std::string& id);

const core::Layer& layer_of(const core::Page& page, const std::string& id);
bool lock_alpha_of(const core::Document& doc, std::size_t at, const std::string& id);

// The points on the grid (pixels), and the round brush of the diameter, hardness, opacity, flow and spacing given.
std::vector<retouch::StrokePoint> on_grid(const core::PenPoints& points, const Grid& g);
retouch::Brush brush_of(const Brushwork& b, const Grid& g);
// A box of the grid as [x, y, w, h] in mm (null for none).
core::Json box_mm(const Box& b, const Grid& g);

// --- pixels over a box of the grid -------------------------------------------------------------------------------------

image_ops::Pixels clear_like(const image_ops::Pixels& like, const Box& box);
// `part` (over `part_box`) laid into `whole` (over `whole_box`) where they meet.
void place(image_ops::Pixels& whole, const Box& whole_box, const image_ops::Pixels& part, const Box& part_box);
// The layer's pixels over `box` of its grid (past the grid: clear).
image_ops::Pixels layer_window(core::OpContext& c, std::size_t at, const std::string& id, const Box& box, const image_ops::Pixels& like, const Grid& g);

// Which pixels a brush samples (Photoshop's サンプル).
enum class Sample { Current, CurrentAndBelow, All };
Sample sample_of(const core::Json& op);
// The page as it shows over `box` of a layer's grid, as pixels of the layer's kind: All — print, without its finish,
// onion skin, nombre or spot channels (image_ops::page_pixels'); CurrentAndBelow — the same without the layers above
// `id`. Past the page: clear.
image_ops::Pixels shown_window(const core::Document& doc, std::size_t at, const std::string& id, Sample which, const Grid& g, const Box& box,
                               const image_ops::Pixels& like);
// The pixels a stroke samples over `box`: the layer's own, or the page as it shows.
image_ops::Pixels sampled(core::OpContext& c, const Brushwork& b, Sample which, const Grid& g, const Box& box, const image_ops::Pixels& like);

// --- the selection -----------------------------------------------------------------------------------------------------

// An area's coverage over a layer's grid: an 8-bit layer's on the page at the working dpi as filter_raster draws its area,
// any other grid's through ink_edit's area_coverage (an 8-bit picture of another size too: stretched over the page).
class AreaCover {
public:
    AreaCover(const core::Page& page, const core::Json& area, const Grid& g, bool eight);
    float at(int x, int y) const;
    // Each pixel of `box`.
    std::vector<float> over(const Box& box) const;
    // The pixels it covers at all, within `limit`.
    Box box(const Box& limit) const;

private:
    Box where_;
    std::string bytes_;
    std::optional<image_ops::Held> held_;
};

std::optional<std::vector<float>> selection_over(const core::Page& page, const Brushwork& b, const Grid& g, const image_ops::Pixels& like,
                                                 const Box& box);

}  // namespace genko::render::brushing
