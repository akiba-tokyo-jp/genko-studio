#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"

// A page drawn from its data (Python's genko/render.py): the paper and its fills, the layers in order (pixels, pen
// lines with every brush, fills and gradients, corrections, effects, masks, blend modes, opacity, clipping), the
// panel borders, a jacket's folds (render/covers.hpp), crop marks, the ruler's points, the lines of dialogue (in their
// balloons, render/text/balloons.hpp, set under a layer when they ask; the others as labels) and the onion skin, the
// same pixels as the Python baseline.
// (A page in precise colour, which Python does not have, is composited in its own canvas: render/color_canvas.hpp.)
//
// Not drawn in this step (render::NotYetPorted names them, unless RenderOptions::skip_unported): placed pictures
// ("placed") and the monochrome finish of a painting app's colour layer ("finish");
// correction layers other than the colour adjustments ("adjust:<kind>"); what a line's letters cannot be drawn with
// yet (a font file that does not open: "default_font"; a warp without a map: "text_warp"). Tone layers, effect lines
// and a layer's screen (render/tones.hpp, render/effects.hpp), the 3D guides (render/prims.hpp) and nombres are drawn.

namespace genko::render {

// A render that is no longer wanted (RenderOptions::stop was requested).
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("render cancelled") {}
};

// A part of the page, in pixels at the render's resolution.
struct RenderRegion {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct RenderOptions {
    std::string mode = "print";  // print | proof | name
    bool crop_marks = false;
    bool onion = true;
    // The monochrome print finish: nothing = by the page (mono pages yes, colour pages no).
    std::optional<bool> finish;
    // Lines as plain polylines (the screen's first look at a page; never for output).
    bool rough = false;
    // false: tones as flat greys (e-books).
    bool dots = true;
    // render.SCREEN_DOTS: tones shown as dots on screen too.
    bool screen_dots = false;
    // Leave out what this build does not draw yet (listed in RenderResult::omitted) instead of throwing
    // NotYetPorted. For a preview on screen only, never for output.
    bool skip_unported = false;
    // Only this part of the page: the same pixels as drawing the whole page and cutting this part out.
    std::optional<RenderRegion> region;
    // The page is already one frame of its animation (core::anim::at_frame): drawn as it is. Otherwise an animation
    // page draws as its first frame.
    bool at_frame = false;
    // Requested: the render stops soon with Cancelled.
    std::stop_token stop;
};

struct RenderResult {
    Image image;                       // "RGB", the page or the region's size
    std::vector<std::string> omitted;  // with skip_unported: what was left out (each once, in the order met)
};

// RenderOptions with mode "proof" (render_frame's default).
RenderOptions proof_options();

// The roles exported (render.EXPORT_ROLES).
std::vector<core::LayerRole> export_plan(const core::Page& page);

// max(1, round(mm / 25.4 * dpi)).
int mm_to_px(double mm, int dpi);

// A box in mm as pixels: (x0, y0, x1, y1), each corner through mm_to_px.
Box rect_px(const core::Rect& rect, int dpi);

// The same participation rule for page rendering and destructive layer editing.
bool uses_color_precision(const core::Page& page, bool print);
class ColorCanvas;
// Blend an editable precise-colour stroke layer without an RGBA8 intermediate.
void blend_color_strokes(ColorCanvas& canvas, const core::Page& page, const core::Layer& layer, int dpi, Box area, const core::Document* episode = nullptr);
// Same paper/legacy-page-fill background used by regular drawing and destructive flatten.
Image page_background(const core::Page& page, Size size, bool name_or_proof);

RenderResult render_page(const core::Page& page, int dpi, const RenderOptions& options = {},
                         const core::Document* episode = nullptr);

// One panel, cut from the page (bleed panels with their bleed). core::Error("key") for a frame that is not there.
Image render_frame(const core::Page& page, std::string_view frame_id, int dpi, const RenderOptions& options = proof_options(),
                   const core::Document* episode = nullptr);

// Two pages side by side as the open book shows them (page index first and second).
Image render_spread(const core::Document& episode, const core::Num& first, const core::Num& second, int dpi = 150,
                    const RenderOptions& options = {}, bool to_trim = false);

// anim.cel_image: one cel alone on a clear page ("RGBA"): its pixels (resized to the page) and its lines, without the
// panels, its mask or its opacity (the onion skin and the light table).
Image cel_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode = nullptr);

// One layer alone over a transparent page (its pixels, fills and lines, panel clip and mask).
Image layer_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode = nullptr,
                  bool skip_unported = false, bool bake_color = false);
std::optional<Image> drawable_layer_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode = nullptr,
                  bool skip_unported = false, bool bake_color = false);

// One layer of a page as the layered exports write it (Python's psd.page_layers): its name, its picture ("RGBA" of the
// page's size) and, for a pixel layer, its settings (`settings`: opacity, blend, clip, and its mask, "L" of the page's
// size, white shows).
struct PageLayer {
    std::string name;
    Image image;
    bool settings = false;
    double opacity = 1.0;
    std::string blend = "normal";
    bool clip = false;
    std::optional<Image> mask;
};

// psd.page_layers(page, episode, dpi), bottom to top: the paper, the page's fills, each visible exported pixel layer
// (not the name and draft), the pen lines of the ink layer (3 px, as render._stroke draws them), the tones, the effect
// lines, the panel borders, one layer for each placed line (its balloon and letters), the nombres. Each is given to
// `each` as it is made (they are never all held). NotYetPorted("placed") for a placed picture (its art before the
// monochrome finish is not drawn by this build yet). (A layer in precise colour, which Python does not have: its
// 8-bit picture.)
void page_layers(const core::Page& page, const core::Document& episode, int dpi, const std::function<void(PageLayer&&)>& each);

// Pure black and white: grey above `threshold` is white; with `screen` the greys become a pattern (dot, line, cross or
// noise; 書き出しでのトーン化).
Image to_bitonal(const Image& image, int threshold = 180, const core::Json* screen = nullptr);

// Would drawing this page now mean drawing many lines from scratch (no recent drawing of them at this resolution)?
bool rough_needed(const core::Page& page, int dpi);

// Where the page's nombres are drawn at `dpi` (print and proof): for each, the box (pixels) of its letters with the
// margin whose colours decide whether it gets its white halo — what the page has under it changes how it is drawn.
// Empty when the page shows none.
std::vector<Box> nombre_areas(const core::Page& page, int dpi, const core::Document* episode = nullptr);

// The remembered layer pictures and masks are forgotten (a new process starts without them).
void clear_render_caches();

}  // namespace genko::render
