#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

// PC3 節 B (B-0, SPEC.md COLOR-06): the selection algorithms on coverage masks — PhotoCraft v0.2.0's algo/selection.rs
// (MIT, native/third_party/photocraft/NOTICE.md), in float as it computes them: the magic wand and its 8-bit region,
// Color Range's samples, families and tones, the Euclidean distance transform and what it makes (expand, contract,
// border), smooth, feather, the anti-aliased polygon. A mask is `w × h` row-major floats in 0..1 over an area; a Region
// is 8-bit coverage over its box. render/area_kinds turns their results into the areas the ops take ({"mask"}).

namespace genko::render::selection_ext {

// A box of pixels [x0, x1) × [y0, y1) (PhotoCraft's Rect).
struct Rect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    friend bool operator==(const Rect&, const Rect&) = default;
};

// A selected region: 8-bit coverage over `box` (row-major), the box of the pixels touched (the anti-aliased fringe too).
struct Region {
    Rect box;
    std::vector<std::uint8_t> mask;
    // Coverage (0..1) at (x, y); 0 outside the box.
    float at(int x, int y) const;
};

using Rgba = std::array<float, 4>;
using Rgba8 = std::array<std::uint8_t, 4>;

// magic_wand: the pixels like the seed's (each channel within `tolerance` levels of 255, alpha too); `contiguous`: only
// those 4-joined to the seed. `px` covers `area`. With `anti_alias`, the edge pixels take their 3×3 neighbourhood.
std::vector<float> magic_wand(const std::vector<Rgba>& px, const Rect& area, std::pair<int, int> seed, float tolerance, bool contiguous,
                              bool anti_alias);
// wand_region: the same on 8-bit pixels by a span fill, anti-aliasing only the edges within the box touched. None when the
// seed is outside `area` or nothing is selected.
std::optional<Region> wand_region(const std::vector<Rgba8>& img, const Rect& area, std::pair<int, int> seed, float tolerance,
                                  bool contiguous, bool anti_alias);

// Color Range: coverage falls off linearly with the largest channel difference (levels) over fuzziness + 1; a clear pixel
// is never selected.
std::vector<float> color_range(const std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness);
// One sample of Sampled Colors: a colour and where it was picked (area pixels; for Localized Color Clusters).
struct RangeSample {
    std::array<float, 3> color{};
    std::optional<std::pair<float, float>> at;
};
// Several samples: each pixel its best match; with `localized`, a sample fades to 0 at that many pixels from where it was
// picked. `w`: the row width of `px`.
std::vector<float> color_range_samples(const std::vector<Rgba>& px, std::size_t w, const std::vector<RangeSample>& samples, float fuzziness,
                                       std::optional<float> localized);
// Reds, Yellows, Greens, Cyans, Blues, Magentas (`center` 0, 60, …, 300°): how near the hue (none 60° away) × chroma.
std::vector<float> hue_range(const std::vector<Rgba>& px, float center);
// Highlights, Midtones, Shadows: luminance (Rec. 601, 0..255) in lo..hi full, falling off over `falloff` levels.
std::vector<float> tone_range(const std::vector<Rgba>& px, float lo, float hi, float falloff);

// The Euclidean distance (pixels) to the nearest `inside` pixel (Felzenszwalb & Huttenlocher).
std::vector<float> edt(const std::vector<bool>& inside, std::size_t w, std::size_t h);
// Select › Modify: expand and contract by r pixels (a pixel whose centre is d from the nearest selected centre is covered
// by r + 1 − d, clamped), a border band of width r straddling the edge, smooth (a box average of radius r re-thresholded),
// feather (a Gaussian of σ = radius / 2).
std::vector<float> expand(const std::vector<float>& m, std::size_t w, std::size_t h, float r);
std::vector<float> contract(const std::vector<float>& m, std::size_t w, std::size_t h, float r);
std::vector<float> border(const std::vector<float>& m, std::size_t w, std::size_t h, float r);
std::vector<float> smooth(const std::vector<float>& m, std::size_t w, std::size_t h, float r);
std::vector<float> feather(const std::vector<float>& m, std::size_t w, std::size_t h, float radius);

// The even-odd coverage of a polygon (pixel coordinates) over `area`: exact across each of 4 sub-rows a pixel (1, its
// centre's, without anti-aliasing).
std::vector<float> polygon(const std::vector<std::pair<float, float>>& points, const Rect& area, bool anti_alias);

// --- to and from the drawing build's pictures -------------------------------------------------------------------------

// An "L" picture's values / 255, and back (each value × 255 rounded, clamped).
std::vector<float> from_l(const Image& mask);
Image to_l(const std::vector<float>& m, Size size);
// A region as a page-sized "L" picture at `size` (outside its box 0).
Image region_image(const Region& region, Size size);
// The area a page-sized "L" picture at `dpi` is, as the ops take one ({"mask": {"box", "png"}}, cropped to what it
// covers): render/selection's from_mask. None when it covers nothing.
std::optional<core::Json> to_area(const Image& mask, int dpi);

}  // namespace genko::render::selection_ext
