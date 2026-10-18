#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "render/image.hpp"
#include "render/image_ops.hpp"

// PC3 節 C (C-0, SPEC.md COLOR-06, docs/cpp-migration/ARCHITECTURE.md「再標本化」): drawing pixels again through a map —
// PhotoCraft v0.2.0's algo/src/transform.rs (the projective warp), warp.rs (the mesh warp) and resample.rs (Image Size's
// separable filters), MIT (native/third_party/photocraft/NOTICE.md) — on a plane of floats that any of the page's kinds of
// pixels becomes: 8-bit RGBA (its bytes / 255), precise RGBA16/32 (its samples; f32's colours above 1 kept: HDR), an ink
// layer's planes and alpha (the ink canvas's values), and grey (a layer mask, a channel: no alpha, the edge carried on
// outside).
//
// Coordinates are the plane's pixels (a pixel (x, y) covers [x, x + 1) × [y, y + 1)): a destination pixel takes the
// source at the inverse map of its centre, less half a pixel. Colours are gathered premultiplied by their alpha (no dark
// fringe at a clear edge); outside the source is clear (a plane with alpha) or its nearest edge (grey). The callers map the
// mm of the page onto each plane's own grid (a pixel's width is the paper's width over the plane's).

namespace genko::render::resample {

// Free Transform's interpolation (Photoshop's menu; nearest, bilinear, bicubic: Catmull-Rom, Keys a = −0.5).
enum class Interp { Nearest, Bilinear, Bicubic };
std::optional<Interp> interp_from(std::string_view text);
// Image Size's filters (preserve_details: Lanczos, with a light sharpening when it enlarges).
enum class Filter { Nearest, Bilinear, Bicubic, Lanczos, PreserveDetails };
std::optional<Filter> filter_from(std::string_view text);

// A 3×3 projective map, row-major: [x', y', w'] = H · [x, y, 1].
struct Homography {
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::pair<double, double> apply(double x, double y) const;
    Homography mul(const Homography& o) const;
    std::optional<Homography> inverse() const;
    // The affine map x' = a·x + c·y + e, y' = b·x + d·y + f (the selection's matrices' order).
    static Homography affine(const std::array<double, 6>& abcdef);
    // The map taking the box [x0, y0, x1, y1] onto `quad` (its corners clockwise from the top left); none when the box or
    // the quad is degenerate.
    static std::optional<Homography> rect_to_quad(const std::array<double, 4>& box, const std::array<std::array<double, 2>, 4>& quad);
    // The linear scale near (x, y) (the square root of the Jacobian's determinant).
    double local_scale(double x, double y) const;
    bool is_affine() const { return m[6] == 0.0 && m[7] == 0.0 && m[8] == 1.0; }
};

// A plane of pixels: `channels` floats each, row-major; with `alpha` the last of them is the (straight) alpha. `hdr`: the
// colours are not cut at 1 (a 32-bit precise plane's); otherwise every value is kept within 0..1.
struct Plane {
    int width = 0;
    int height = 0;
    int channels = 4;
    bool alpha = true;
    bool hdr = false;
    std::vector<float> v;

    Plane() = default;
    Plane(int w, int h, int n, bool with_alpha) : width(w), height(h), channels(n), alpha(with_alpha), v(std::size_t(w) * std::size_t(h) * std::size_t(n), 0.0f) {}
    float* at(int x, int y) { return v.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * std::size_t(channels); }
    const float* at(int x, int y) const { return v.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * std::size_t(channels); }
};

// The plane drawn again on a destination plane of `width` × `height` whose pixel (0, 0) is the source's (`x0`, `y0`) after
// the map: each destination pixel takes the source at h⁻¹(x0 + x + 0.5, y0 + y + 0.5) − 0.5. A scale below one half (at
// the source's middle) first shrinks the source by a power of 2 (bicubic), so a reduction does not alias. A whole-pixel
// move (h a translation by whole pixels) only moves the pixels: their values exact.
Plane project(const Plane& src, const Homography& h, int width, int height, Interp interp, int x0 = 0, int y0 = 0);
// The plane drawn through a forward map f (source pixels → destination pixels): the source cut into cells of 4 pixels
// (512 a side at most), each cell's two triangles drawn where f takes them, the source position found by barycentric
// weights; where the map folds over itself, the later cell wins. An affine f (the identity too) is drawn exactly.
using Forward = std::function<std::pair<double, double>(double, double)>;
Plane mesh(const Plane& src, const Forward& f, int width, int height, Interp interp, int x0 = 0, int y0 = 0);
// Image Size: the plane resized to `width` × `height` by a separable filter (a reduction's support widened by its
// factor), premultiplied.
Plane resize(const Plane& src, int width, int height, Filter filter);

// A turn or flip of the whole plane: its pixels moved, their values exact (PhotoCraft's Turn::pixel_map).
enum class Turn { FlipH, FlipV, Rotate180, Cw90, Ccw90 };
std::optional<Turn> turn_from(std::string_view text);  // (90cw, 90ccw, 180, flip_h, flip_v)
Plane turn(const Plane& src, Turn t);
// The plane moved by whole pixels onto one of `width` × `height` (outside: clear, or the edge carried on for grey).
Plane translate(const Plane& src, int dx, int dy, int width, int height);
// The map of a turn in continuous coordinates for a w × h plane (its pixels' edges), as an affine [a, b, c, d, e, f].
std::array<double, 6> turn_affine(Turn t, double w, double h);

// --- the page's kinds of pixels ---------------------------------------------------------------------------------------

// An 8-bit RGBA picture as a plane (its bytes / 255), and back (each value × 255 rounded).
Plane from_rgba8(const Image& rgba);
Image to_rgba8(const Plane& plane);
// A grey picture ("L": a layer mask, a channel at 8 bits) as a plane without alpha, and back.
Plane from_gray(const Image& l);
Image to_gray(const Plane& plane);
// A paint layer's pixels (render/image_ops: 8 bits, precise, inks) as a plane and back (the same kind, depth and colour
// mode as `like`; a 32-bit precise layer's HDR kept).
Plane from_pixels(const image_ops::Pixels& pixels);
image_ops::Pixels to_pixels(const Plane& plane, const image_ops::Pixels& like);

}  // namespace genko::render::resample
