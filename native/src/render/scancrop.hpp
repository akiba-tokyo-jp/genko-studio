#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "render/codecs/image.hpp"

// 写真の切り抜きと角度補正 (file.automate.cropAndStraightenPhotos, PC5 I-h; SPEC.md AUTO-01): the photos on a flatbed
// scan found, each one's rotation measured, and each cut out upright — PhotoCraft v0.2.0's algo/src/scancrop.rs (MIT,
// native/third_party/photocraft/NOTICE.md), its steps as it computes them:
//   1. the background: the median colour of the border (a scanner lid is uniform);
//   2. the foreground: the pixels whose colour lies farther from it than Otsu's threshold (on a copy made at most 800 px a
//      side), closed and opened (specks inside a photo filled, dust dropped), split into 8-connected parts — those under 1%
//      of the scan dropped;
//   3. the rotation: the minimum-area rectangle around each part's convex hull (Freeman & Shapira 1975);
//   4. the rectangle shrunk by a pixel of the small copy (the soft border dropped), in the scan's pixels.
// Genko cuts each one out of the scan at its own pixels, depth and colours (render/resample's projection, bicubic).

namespace genko::render::scancrop {

// A photo found: its centre and its sides along its own axes (scan pixels), the angle of its x axis in the scan (degrees,
// −45..45, clockwise) and its area as a share of the scan.
struct FoundPhoto {
    std::array<double, 2> center{0, 0};
    double width = 0, height = 0;
    double angle = 0;
    double area = 0;
    // Its four corners in the scan, clockwise from its top left.
    std::array<std::array<double, 2>, 4> corners() const;
};

// The photos on a scan of any layout and sample (grey, RGB, CMYK; with or without an alpha — a clear pixel taken as
// white), in reading order (rows by the centre's height, in bands of a third of the smallest photo, then left to right).
// None on a scan smaller than 8 px a side or one of a uniform colour.
std::vector<FoundPhoto> find_photos(const codecs::CodecImage& scan);

// The photo cut out of the scan upright: `width` × `height` (rounded) of the scan's layout, sample, profile and
// resolution, opaque; interpolated bicubically (a 32-bit scan's values past 1 kept).
codecs::CodecImage cut_photo(const codecs::CodecImage& scan, const FoundPhoto& photo);

}  // namespace genko::render::scancrop
