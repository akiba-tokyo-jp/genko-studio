#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "render/image.hpp"
#include "render/image_ops.hpp"

// PC3 B4 (SPEC.md COLOR-06「修復」): the dabs of the retouching brushes and the way their strokes are laid on a layer —
// PhotoCraft v0.2.0's paint/src/lib.rs (StrokePoint, Stroke, Dab, dabs, dab_coverage) and paint/src/retouch.rs
// (Footprint, stroke_coverage, over_native, apply_coverage), MIT (native/third_party/photocraft/NOTICE.md), and the clone
// source's mapping (engine/src/presets/clone_source.rs: Mapping, transform_matrix, resample).
//
// A stroke is points on a layer's own grid (pixels) and a round brush (size, hardness, spacing, flow; the pressure
// scales the size). Two shapes of stroke cover every retouching tool (upstream's):
// - accumulate, then composite (here): the dabs build one coverage (flow builds up toward each dab's ceiling, c ← c +
//   v·(ceil − c)), and a picture made once from the pixels before the stroke (a cloned source, a healed patch, an earlier
//   state) is laid through it at the stroke's opacity — what the stroke painted is never sampled again (Clone Stamp,
//   Healing Brush, Spot Healing Brush, History Brush);
// - sequential dabs (PC4 adds `apply_dab_stroke` here, for Dodge, Burn, Sponge, Blur, Sharpen and Smudge: each dab edits
//   a working copy the next dab sees, mixed back at the stroke's opacity).
// The pixels are image_ops::Pixels — a layer's own values of any of the three kinds (8 bits, precise, inks: CMYK, Lab,
// grey and a multichannel page's plates) — so the same code paints every kind; only a blend other than normal goes
// through the colours (rgba_of, and an ink layer's back by the page's rules).

namespace genko::render::retouch {

// One input sample on the layer's grid (pixels); a mouse's pressure is 1.
struct StrokePoint {
    double x = 0.0;
    double y = 0.0;
    float pressure = 1.0f;
};

// The round brush every retouching tool shares (Photoshop's: the tool's diameter, hardness, opacity, flow, spacing).
struct Brush {
    float size = 20.0f;      // the diameter (pixels of the grid)
    float hardness = 1.0f;   // 0..1
    float opacity = 1.0f;    // 0..1, the stroke's
    float flow = 1.0f;       // 0..1, each dab's build-up
    float spacing = 0.25f;   // between dabs, of the diameter
    bool pressure_size = true;  // the pressure scales the diameter
};

// A dab placed along the stroke.
struct Dab {
    double x = 0.0;
    double y = 0.0;
    float radius = 0.5f;
    float alpha = 1.0f;    // the flow: how much it builds up
    float opacity = 1.0f;  // the ceiling it builds up to
    std::size_t index = 0;
};

// The dabs of a stroke, `spacing` × the diameter apart along the points (the first point's dab first; a stroke of one
// point is one dab).
std::vector<Dab> dabs(const Brush& brush, const std::vector<StrokePoint>& points);
// The coverage of a round dab at distance d from its centre: the anti-aliased edge, a smoothstep from the hard core.
float dab_coverage(float d, float radius, float hardness);
// The pixels a dab may cover (one pixel of the edge's slack).
Box dab_rect(const Dab& dab);

// One dab's coverage (× its flow) over its box.
struct Footprint {
    Dab dab;
    Box rect;
    std::vector<float> cov;
    float at(int x, int y) const { return cov[std::size_t(y - rect.y0) * std::size_t(rect.width()) + std::size_t(x - rect.x0)]; }
};
Footprint footprint(const Dab& dab, float hardness);

// A stroke's whole coverage (0..1) over its box (the union of its dabs'); empty for no dabs.
struct Coverage {
    Box box;
    std::vector<float> cov;
    bool empty() const { return cov.empty(); }
    float at(int x, int y) const { return cov[std::size_t(y - box.y0) * std::size_t(box.width()) + std::size_t(x - box.x0)]; }
};
Coverage stroke_coverage(const Brush& brush, const std::vector<StrokePoint>& points);
// The same coverage with `margin` pixels of nothing around it (the Healing Brush's boundary for its solve).
Coverage padded(const Coverage& coverage, int margin);

// Normal's "source over the destination" at coverage k, in the layer's own values (colours then alpha; any colour
// model); with lock_alpha the alpha is kept.
void over(float* dst, const float* src, unsigned colours, float k, bool lock_alpha);

// A picture laid through a stroke's coverage onto a window of a layer: each pixel weighted by its coverage × the opacity
// × the selection's (`selection`: one value per pixel of the window, none: everywhere). `target` holds the layer's pixels
// over `box` (its grid's pixels), `paint` the picture over the same box. A pixel that is clear stays clear with lock_alpha;
// a blend other than normal composites the colours (image_ops::composite: W3C's), an ink layer's back by the page's rules.
// The box of the pixels it changed (none: empty).
Box apply_coverage(image_ops::Pixels& target, const Box& box, const Coverage& coverage, float opacity, const std::vector<float>* selection,
                   bool lock_alpha, const image_ops::Pixels& paint, std::string_view blend);

// --- the clone source (Window › Clone Source) --------------------------------------------------------------------------

// How a clone stroke samples: a pixel d of the destination reads the source at `source + m · (d − anchor)` (m row-major
// 2 × 2: identity for a plain offset).
struct CloneMapping {
    std::pair<double, double> source;
    std::pair<double, double> anchor;
    std::array<double, 4> m{1.0, 0.0, 0.0, 1.0};

    bool is_translation() const;
    // The whole-pixel offset (the identity's).
    std::pair<int, int> offset() const;
    std::pair<double, double> map(double x, double y) const;
    // The source's pixels a destination box needs (a two-pixel apron for the bilinear).
    Box source_box(const Box& dst) const;
};
// The matrix taking the destination's offsets to the source's for a source's width and height (%), rotation (degrees,
// counter-clockwise on the page as Photoshop's) and flips.
std::array<double, 4> clone_matrix(double width_pct, double height_pct, double rotation_deg, bool flip_h, bool flip_v);
// The source (its pixels over `from`) resampled onto `dst` through the mapping: bilinear, premultiplied by the alpha;
// past `from`, clear.
image_ops::Pixels resample(const image_ops::Pixels& src, const Box& from, const Box& dst, const CloneMapping& map);

}  // namespace genko::render::retouch
