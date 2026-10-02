#pragma once

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
// panel borders, crop marks, the ruler's points and the onion skin, the same pixels as the Python baseline.
//
// Not drawn in this step (render::NotYetPorted names them, unless RenderOptions::skip_unported): lines and balloons
// ("balloons"), 3D guides ("prims"), placed pictures ("placed"), nombres ("nombre"), a jacket's folds ("covers"),
// animation pages ("anim") and the monochrome finish of a painting app's colour layer ("finish"); correction layers
// other than the colour adjustments ("adjust:<kind>"). Tone layers, effect lines and a layer's screen are drawn
// (render/tones.hpp, render/effects.hpp).

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

RenderResult render_page(const core::Page& page, int dpi, const RenderOptions& options = {},
                         const core::Document* episode = nullptr);

// One panel, cut from the page (bleed panels with their bleed). core::Error("key") for a frame that is not there.
Image render_frame(const core::Page& page, std::string_view frame_id, int dpi, const RenderOptions& options = proof_options(),
                   const core::Document* episode = nullptr);

// Two pages side by side as the open book shows them (page index first and second).
Image render_spread(const core::Document& episode, const core::Num& first, const core::Num& second, int dpi = 150,
                    const RenderOptions& options = {}, bool to_trim = false);

// One layer alone over a transparent page (its pixels, fills and lines, panel clip and mask).
Image layer_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode = nullptr,
                  bool skip_unported = false);

// Pure black and white: grey above `threshold` is white; with `screen` the greys become a pattern (dot, line, cross or
// noise; 書き出しでのトーン化).
Image to_bitonal(const Image& image, int threshold = 180, const core::Json* screen = nullptr);

// Would drawing this page now mean drawing many lines from scratch (no recent drawing of them at this resolution)?
bool rough_needed(const core::Page& page, int dpi);

// The remembered layer pictures and masks are forgotten (a new process starts without them).
void clear_render_caches();

}  // namespace genko::render
