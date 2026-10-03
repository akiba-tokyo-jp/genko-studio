#pragma once

// Internal to genko_render: the parts of the page renderer (render.py, placement.py, frames.py, filters.py) shared
// by page.cpp, panels.cpp and composite.cpp.

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/draw.hpp"
#include "render/image.hpp"
#include "render/page.hpp"

namespace genko::render::detail {

// A point on the page in mm, as a float.
using PointMM = core::PointF;

// What a render is doing: the page, its size in pixels and the options.
struct Ctx {
    const core::Page* page = nullptr;
    const core::Document* episode = nullptr;
    int dpi = 0;
    Size size;
    std::string mode;
    bool finish = false;
    bool rough = false;
    bool dots = true;  // (Python's _NO_DOTS is not set)
    bool screen_dots = false;
    bool skip_unported = false;
    std::vector<std::string>* omitted = nullptr;
    std::stop_token stop;
};

// Throws Cancelled when the render is no longer wanted.
void check_cancel(const Ctx& ctx);

// Something this build does not draw yet: left out and recorded with skip_unported (true), NotYetPorted otherwise.
bool skip_unported(const Ctx& ctx, const std::string& element);

// --- geometry (genko/placement.py and genko/frames.py are core's: core/placement.hpp, core/frames.hpp) ----------

// render._xy: each coordinate through mm_to_px.
std::pair<int, int> xy(double x_mm, double y_mm, int dpi);
PointD xy_point(double x_mm, double y_mm, int dpi);

// The points as floats (for drawing).
std::vector<PointMM> mm_points(std::span<const core::Point> points);
// frames.outline, placement.bleed_outline and frames.offset with the points as floats.
std::vector<PointMM> outline_mm(const core::Frame& frame);
std::optional<std::vector<PointMM>> bleed_outline_mm(const core::Page& page, const core::Frame* frame,
                                                     std::optional<double> beyond_mm = 8.0);
std::vector<PointMM> offset_outline(const std::vector<PointMM>& points, double d);
// render._panel_of: the panel (a leaf that cuts the layers) that has this point.
const core::Frame* panel_of(const core::Page& page, double x, double y);

// --- panel masks and borders, drawn in page coordinates on `part` (the box `area` of the page) ------------------

void fill_frame(Draw& draw, const core::Frame& frame, int dpi, int fill = 255);
bool bleed_shape_mask(Draw& draw, const core::Page& page, const core::Frame& frame, int dpi);
// render._clip_mask over `area` (nothing when no panel cuts the layers).
std::optional<Image> clip_mask(const core::Page& page, Size size, int dpi, const Box& area);
// render._draw_frame_mask over `area`.
Image frame_mask(const core::Page& page, const core::Frame& frame, Size size, int dpi, const Box& area);
// render._draw_frames onto the RGB picture `part` of the box `area`.
void draw_frames(Image& part, const Box& area, const core::Page& page, Size size, int dpi);
void draw_border(Draw& draw, const std::vector<PointMM>& points, double width_mm, int dpi, const core::Json* style,
                 std::string_view seed);
std::vector<std::vector<PointMM>> dashes(const std::vector<PointMM>& points, double on, double off);
std::vector<PointMM> rough_outline(const std::vector<PointMM>& points, std::string_view seed, double amount_mm);
void draw_crop_marks(Draw& draw, const core::Page& page, int dpi);

// --- compositing (composite.cpp) -----------------------------------------------------------------------------------

// render._blend_over: `over` onto `base` in a blend mode, with an opacity and a clipping alpha.
Image blend_over(const Image& base, const Image& over, std::string_view mode, double opacity, const Image* clip);
// render.fill_layer_image over `area`.
Image fill_layer_image(const core::Layer& layer, Size size, int dpi, bool mono, const Box& area);
// render.layer_effects (the whole of `raster`: the caller gives it a margin).
Image layer_effects(const core::Layer& layer, Image raster, int dpi);
// How far layer_effects reaches (px): a picture computed this much larger gives exact pixels inside.
int effect_margin(const core::Layer& layer, int dpi);
// render._adjusted over `area`: a correction layer at work on what is under it.
Image adjusted(const Ctx& ctx, Image rgba, const core::Layer& layer, const Image* clip, const Box& area);
// Whether the page has a correction layer whose filter looks beyond each pixel or at where it is on the page (blur,
// mosaic, wave, noise, …: render/filters.cpp): a part of such a page is cut from the whole page drawn.
bool needs_whole_page(const core::Page& page);
// render._has_colour
bool has_colour(const Image& image);
// The picture of a mask (or of a raster layer) resized to the page, the part `area` only.
Image resized_part(const Image& source, Size size, const Box& area, Resample resample);

// render.gradient_colours(gradient_t(gx, gy, spec), spec) one point at a time (gradient_fill): the colour, each band
// np.round()ed and cast to uint8 as numpy does, and the opacity (float64) at (gx, gy) in mm.
class GradientColours {
public:
    explicit GradientColours(const core::Json& spec);
    ~GradientColours();
    GradientColours(const GradientColours&) = delete;
    GradientColours& operator=(const GradientColours&) = delete;
    void at(double gx, double gy, unsigned char rgb[3], double& opacity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// render._layer_strokes(layer, size, dpi, panel_mask, raster[, page=page]) over the whole picture of `ctx` (ctx.page,
// size and dpi): the layer's fills and lines drawn from their data (nothing when it has none). `with_page`: each line
// cut by the panel it begins in when the layer asks for it. (The ops that bake a layer's lines into its pixels.)
std::optional<Image> layer_lines(const Ctx& ctx, const core::Layer& layer, const Image* panel_mask, const Image* raster,
                                 bool with_page);

}  // namespace genko::render::detail
