#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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
    // color-mode@1: a CMYK or Lab page's inks too (RenderResult::inks, at the page's depth). Other pages have none.
    bool inks = false;
    // color-model@1: an RGB page's values too where it composites in precise colour (RenderResult::precise: its layers'
    // values, and where the frames, words and marks are drawn over them in 8 bits, those colours in its space).
    bool precise = false;
    // channels@1 (render/channels.hpp): the page's spot channels printed over the picture (their ids; none: every one —
    // an empty list: none, for the exports that write them as plates of their own). Only the picture: not the inks or
    // values.
    std::optional<std::vector<std::string>> spots;
    // channels@1, the channel panel's view (the screen): the page's colour planes shown (none: every one) and the alpha
    // channels laid over the picture in their colours (their ids).
    std::optional<std::vector<bool>> planes;
    std::vector<std::string> overlays;
    // smart-objects@1 (render/smart.hpp): a smart object's contents page — its layers alone over nothing ("RGBA"): no paper,
    // panels' borders, lines, nombres, effect lines, guides or marks (render_contents).
    bool contents = false;
};

struct RenderResult {
    Image image;                       // "RGB", the page or the region's size (a CMYK or Lab page: as the screen shows it)
    std::vector<std::string> omitted;  // with skip_unported: what was left out (each once, in the order met)
    std::optional<core::InkPixels> inks;  // with RenderOptions::inks on a CMYK or Lab page: its inks (opaque)
    std::optional<std::string> precise;   // with RenderOptions::precise: GKCR f32 of the page's values (its profile's)
};

// RenderOptions with mode "proof" (render_frame's default).
RenderOptions proof_options();

// The roles exported (render.EXPORT_ROLES).
std::vector<core::LayerRole> export_plan(const core::Page& page);

// The most pixels render_page draws at once (a page, or the region asked for): past it, core::Error("image_too_large").
inline constexpr std::int64_t kMaxAreaPixels = 1'500'000'000;

// max(1, round(mm / 25.4 * dpi)).
int mm_to_px(double mm, int dpi);

// A box in mm as pixels: (x0, y0, x1, y1), each corner through mm_to_px.
Box rect_px(const core::Rect& rect, int dpi);

// The same participation rule for page rendering and destructive layer editing.
bool uses_color_precision(const core::Page& page, bool print);
class ColorCanvas;
// Blend an editable precise-colour stroke layer without an RGBA8 intermediate.
void blend_color_strokes(ColorCanvas& canvas, const core::Page& page, const core::Layer& layer, int dpi, Box area, const core::Document* episode = nullptr);
// PC2d (document:color-depth-model): whether a layer with precise colour of its own (precise pixels, precise lines) is
// drawn as more than those alone — with lines on its precise pixels, pictures laid on it, effects, its colour, a
// screen, or as a tone —, and the layer so drawn as the page draws it in proof (a precise merge composites it so; a
// tone as the 8-bit merge takes it).
bool precise_layer_styled(const core::Layer& layer);
void blend_precise_layer(ColorCanvas& canvas, const core::Page& page, const core::Layer& layer, int dpi, Box area, const core::Document* episode = nullptr);
// Same paper/legacy-page-fill background used by regular drawing and destructive flatten.
Image page_background(const core::Page& page, Size size, bool name_or_proof);

// color-mode@1: layers of a CMYK or Lab page (as given, in order) composited as the page composites them (proof), onto
// nothing or (paper) its paper, over the whole page at dpi: their inks with alpha, at the page's depth. What merging
// them makes on such a page (render/ink_edit).
core::InkPixels composite_ink_layers(const core::Page& page, const std::vector<core::Layer>& layers, int dpi, const core::Document* episode,
                                     bool paper = false);

RenderResult render_page(const core::Page& page, int dpi, const RenderOptions& options = {},
                         const core::Document* episode = nullptr);

// smart-objects@1 (render/smart.hpp): a smart object's contents page drawn as its layers alone (RenderOptions::contents):
// "RGBA" over nothing, with (precise) the values of a page in precise colour, its alpha the layers'. A part of it as the
// whole cut there; never in tiles, never its channels.
RenderResult render_contents(const core::Page& page, int dpi, const RenderOptions& options, const core::Document* episode = nullptr);

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
// color-model@1: does a layer's picture as layer_image and drawable_layer_image make it (`tinted`: its colour laid on)
// hold the page's values — an RGB page of a profile's paint pixels (core::holds_page_values), the fills and lines drawn
// on them brought into them —, not sRGB? Not with an effect (drawn in sRGB: the pixels brought to sRGB first), nor
// tinted (its colour is sRGB).
bool picture_holds_page_values(const core::Page& page, const core::Layer& layer, bool tinted);

// One layer of a page as the layered exports write it (Python's psd.page_layers): its name, its picture ("RGBA" of the
// page's size) and, for a pixel layer, its settings (`settings`: opacity, blend, clip, and its mask, "L" of the page's
// size, white shows).
struct PageLayer {
    std::string name;
    Image image;
    // color-mode@1: on a CMYK or Lab page, a paint layer's own inks over the page (ink_canvas.hpp's InkPicture values:
    // per pixel the colour channels then the alpha); `image` is then how the screen shows them. Other layers: none (their
    // colours are the page's by the rules).
    std::shared_ptr<const std::vector<double>> inks;
    // color-model@1: on an RGB page of a profile, `image` holds the page's values (a paint layer's pixels), not sRGB.
    bool page_values = false;
    bool settings = false;
    double opacity = 1.0;
    std::string blend = "normal";
    bool clip = false;
    std::optional<Image> mask;
    // (PC3 A3) the page layer it is the picture of (empty: none, the paper, fills, lines, panels…), and — a smart object's
    // picture (one that draws as a layer of its own too) — that smart object, the page's: a PSD writes it as a placed
    // layer of its contents.
    std::string id;
    const core::Layer* smart = nullptr;
    // vector@1 (PC3 C-a): the layer's vector mask (a PSD writes it as its vmsk block; a picture of the layer lays it on its
    // alpha when it is on, as the mask)
    std::optional<core::vector::VectorMask> vector_mask;
    // pen-shapes@1 (PC3 C-b): a pen layer's shape part — a PSD writes it as a shape layer (render/psd_shapes), `image`
    // its look —; and the group a pen layer of parts and lines is written as: "end" (below its layers: the group's end) and
    // "group" (above them: the group with the layer's name and settings), no picture of their own.
    const core::vector::ShapePart* shape = nullptr;
    std::string section;
};

// psd.page_layers(page, episode, dpi), bottom to top: the paper, the page's fills, each visible exported pixel layer
// (not the name and draft), the pen lines of the ink layer (3 px, as render._stroke draws them), the tones, the effect
// lines, the panel borders, one layer for each placed line (its balloon and letters), the nombres. Each is given to
// `each` as it is made (they are never all held). NotYetPorted("placed") for a placed picture (its art before the
// monochrome finish is not drawn by this build yet). (A layer in precise colour, which Python does not have: its
// 8-bit picture.) `line_layers` (PSD-01, the editable PSD): each placed line is handed to it, in its place among the
// layers, instead of being given as a layer of its own. `adjust_layers` (PC2, the user's decision 5: the PSD's adjustment
// layers): each correction layer shown is handed to it in its place among the layers (Python's leaves them out: their
// change is in the merged picture only). A pen layer with shape parts (pen-shapes@1, PC3 C-b) is given in its place:
// with `shapes` (a PSD) as a shape layer of its one part or as a group of its pictures, parts and lines
// (render/psd_shapes' plan), else (and where a group would not show it the same) as one layer of its look.
void page_layers(const core::Page& page, const core::Document& episode, int dpi, const std::function<void(PageLayer&&)>& each,
                 const std::function<void(const core::StoryLine&)>* line_layers = nullptr,
                 const std::function<void(const core::Layer&)>* adjust_layers = nullptr, bool shapes = false);

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
