#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」): one formula, three ways (design 4.1, 4.3). PhotoCraft v0.2.0's filter core
// (algo/src/lib.rs 506-966: halo, Ctx, kernel, output_area, apply_tiled; image.rs: Image), MIT
// (third_party/photocraft/NOTICE.md), ported once: a kind's kernel works a window of straight float samples (colours,
// then the alpha) for an output rectangle, and the engine runs it over a paint layer's pixels of each kind — an 8-bit
// layer's RGBA, a precise layer's samples (GKCR: the page's values, HDR kept), an ink layer's canvas values (GKIR) —
// tile by tile, each tile reading only its window (the tile grown by the kind's halo, the grid's edge pixels repeated
// past it), writing its result into the layer's new bytes directly (no float copy of the whole layer), on as many
// threads as the imaging budget holds the windows of. What a tile gets does not depend on the tiling nor the threads
// (the same bytes).
//
// A page of inks is worked as its kind says (core::FilterInks): each plate's value as it is, its lightness (CMYK and
// multichannel plates as 1 − ink, back after; a Lab page's L alone — the kernel then works one colour, as grey —, its a
// and b as they were; grey as it is) or the colours the inks mean (back by the page's rules, render/ink).
// A selection mixes the result with the layer by its coverage (upstream's apply_tiled: straight, every channel).
//
// Lengths in a kind's settings are pixels of the working 200 dpi; a kernel scales them to the layer's grid by Ctx::s.
// Each group of PC4 adds its kinds' kernels in its own file (render/fx_blur.cpp: D0's Gaussian and box blurs, …) to its
// group's function below; render/ops_filters.cpp makes each a filter_raster kind of all three ways (LocalFilter).

namespace genko::render::fx {

// A rectangle of pixels [x0, x1) × [y0, y1) (upstream's photocraft_geom::Rect).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 > x0 ? x1 - x0 : 0; }
    int height() const { return y1 > y0 ? y1 - y0 : 0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    std::size_t area() const { return std::size_t(width()) * std::size_t(height()); }
    Rect inflate(int r) const { return Rect{x0 - r, y0 - r, x1 + r, y1 + r}; }
    Rect intersect(const Rect& o) const;
    Rect unite(const Rect& o) const;  // (the box holding both; an empty one adds nothing)
    bool operator==(const Rect&) const = default;
};

// The colours of the samples a kernel works (the way and the kind's FilterInks decide them).
enum class Space { Rgb, Gray, Cmyk, Lab, Multi };

// How samples outside a window's area are read (upstream's Edge).
enum class Edge { Transparent, Repeat, Wrap };

// A rectangle of interleaved straight samples — `ch` a pixel, the alpha last (upstream's algo::Image).
struct Window {
    Rect rect;
    std::size_t ch = 4;
    std::vector<float> data;

    Window() = default;
    Window(const Rect& r, std::size_t channels) : rect(r), ch(channels), data(r.area() * channels, 0.0f) {}
    int w() const { return rect.width(); }
    std::size_t index(int x, int y) const { return ((std::size_t(y - rect.y0) * std::size_t(rect.width())) + std::size_t(x - rect.x0)) * ch; }
    // The sample at (x, y); 0 outside.
    float get(int x, int y, std::size_t c) const { return rect.contains(x, y) ? data[index(x, y) + c] : 0.0f; }
    // The sample at (x, y), past the window's edge its edge pixel's (a kernel's window is cut to the layer's grid: this
    // repeats the grid's edge, upstream's read_clamped).
    float at(int x, int y, std::size_t c) const {
        if (rect.empty()) return 0.0f;
        return data[index(std::clamp(x, rect.x0, rect.x1 - 1), std::clamp(y, rect.y0, rect.y1 - 1)) + c];
    }
    // The pixel at (x, y) (inside).
    const float* px(int x, int y) const { return data.data() + index(x, y); }
    // The sample with an edge rule relative to `area`.
    float get_edge(int x, int y, std::size_t c, Edge edge, const Rect& area) const;
    // The bilinear sample of the premultiplied pixel at (x, y) (pixel centres at .5) into `out` (straight colour, the
    // alpha last when `alpha`).
    void sample(float x, float y, Edge edge, const Rect& area, bool alpha, float* out) const;
    // The samples inside `out` (an identity pass).
    std::vector<float> crop(const Rect& out) const;
};

// What a kernel runs in (upstream's Ctx, with Genko's scale): the base box (distortions are centred on it and sample
// within it: a selection's box, else the layer's grid), the samples' space and colours (the alpha after them), and how
// many of the layer's pixels one pixel of the working 200 dpi spans (s: 1 for an 8-bit layer, 3 on a 600 dpi page).
struct Ctx {
    Rect bounds;
    Space space = Space::Rgb;
    unsigned colours = 3;
    bool alpha = true;
    double s = 1;
    // The largest magnitude of a sample on the layer (1 but for HDR: a kernel's exact sums are scaled by it).
    double largest = 1;
    // A length of the working 200 dpi on the layer's grid.
    float px(double working) const { return static_cast<float>(working * s); }
};

// How far outside an output tile a kernel reads (upstream's Halo): a margin of pixels, or the whole base box.
struct Halo {
    bool bounds = false;
    int radius = 0;
    static Halo margin(int r) { return Halo{false, r < 0 ? 0 : r}; }
    static Halo whole() { return Halo{true, 0}; }
};

// A kind's formula: its halo, its kernel (the output rectangle's samples, `ch` a pixel, from a window holding the
// rectangle grown by the halo cut to the layer's grid — the base box for a whole one: past the grid's edge a kernel reads
// its edge pixel, Window::at; a window never holds more than the grid, whatever the halo), how much work a pixel is (to
// tell a heavy filter), and whether it writes where the layer is clear (a render: its output is the base box).
struct Kernel {
    std::string_view kind;
    std::function<Halo(const core::Json& params, const Ctx& ctx)> halo;
    std::function<std::vector<float>(const core::Json& params, const Window& src, const Rect& out, const Ctx& ctx)> run;
    std::function<double(const core::Json& params, const Ctx& ctx)> cost;  // (none: kDefaultCost)
    bool fills = false;
};
using Kernels = std::vector<Kernel>;
inline constexpr double kDefaultCost = 50;

// Each group's kernels (its own file; a group fills its own).
Kernels blur_kernels();          // render/fx_blur.cpp — D0: blur_gaussian, blur_box; D1: the rest of ぼかし
Kernels sharpen_kernels();       // D1
Kernels noise_kernels();         // D1
Kernels other_kernels();         // D1
Kernels video_kernels();         // D1
Kernels distort_kernels();       // D2
Kernels pixelate_kernels();      // D2
Kernels stylize_kernels();       // D2
Kernels gallery_kernels();       // D3
Kernels render_kernels();        // D4
Kernels blur_gallery_kernels();  // D4
Kernels camera_raw_kernels();    // D5
Kernels lens_kernels();          // D6
const Kernels& kernels();
const Kernel* kernel(std::string_view kind);

// --- the shared pieces of the kernels (upstream's image.rs, blur.rs; for the groups and 節 E's brushes) ----------------

// Premultiplies the colours by the alpha (in place; nothing without an alpha), and back (a clear pixel's colours 0).
void premultiply(std::vector<float>& data, std::size_t ch, bool alpha);
void unpremultiply(std::vector<float>& data, std::size_t ch, bool alpha);
// The normalized Gaussian of standard deviation `sigma` (radius ⌈3σ⌉; [1] below 0.05).
std::vector<float> gaussian_kernel(float sigma);
// The separable convolution of `src` (premultiplied inside) for `out`: `src` holds `out` grown by the kernels' radii.
std::vector<float> conv_sep(const Window& src, const Rect& out, const std::vector<float>& kx, const std::vector<float>& ky, bool alpha);
// A Gaussian blur of standard deviation `sigma` (the layer's pixels) for `out` — the exact kernel to 4, three box passes
// beyond (Kovesi; their sums exact in whole numbers: the same whatever window holds them) — and its halo. `largest`: the
// largest magnitude of a sample (Ctx::largest).
std::vector<float> gaussian(const Window& src, const Rect& out, bool alpha, float sigma, double largest = 1);
int gaussian_halo(float sigma);
// A box blur of radius `r` pixels (the mean of the (2r + 1)² square) for `out`: running sums exact in whole numbers.
std::vector<float> box(const Window& src, const Rect& out, bool alpha, int r, double largest = 1);

// --- running a kernel on a layer's pixels ----------------------------------------------------------------------------

// How a run is split (tests): the tile's side (0: by the halo, upstream's auto_tile) and the threads (0: as many as
// band_threads() and the imaging budget allow).
struct Tiling {
    int tile = 0;
    int threads = 0;
};

// The layer's pixels as a run reads and writes them (one of the three ways). Reads are of the layer as it was; writes go
// to its new bytes.
class Surface {
public:
    virtual ~Surface() = default;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
    virtual unsigned colours() const = 0;   // (the alpha after them)
    virtual Space space() const = 0;
    // `r` (inside the grid) as straight values — its colours, then its alpha.
    virtual void read(const Rect& r, float* out) const = 0;
    // `r` set from straight values (its colours, then its alpha).
    virtual void write(const Rect& r, const float* in) = 0;
    // The box of the pixels that show (alpha above 0), and the largest magnitude of a sample there (1 at most for all but
    // a 32-bit precise layer).
    virtual Rect content() const = 0;
    virtual double largest() const { return 1.0; }
    // (a page of inks) its colour mode, for the colours the inks mean
    virtual const core::PageColor* ink() const { return nullptr; }
};
// The three ways' surfaces: an 8-bit RGBA picture's bytes, a precise raster's (GKCR), an ink raster (its pixels — kept by
// the caller while the surface lives —, the page's mode). After a run, an 8-bit surface's new picture (take_image) and
// the others' new bytes (take_bytes).
std::unique_ptr<Surface> eight_surface(const Image& rgba);
std::unique_ptr<Surface> precise_surface(const std::string& raster);
std::unique_ptr<Surface> ink_surface(const core::InkPixels& inks, const core::PageColor& color);
Image take_image(Surface& surface);
std::string take_bytes(Surface& surface);

// The kernel of `kind` with its settings (their normal form) over the surface (design 4.3): `bounds` the base box,
// `cover` each pixel's coverage by the selection (0..255, a byte a pixel; none: everywhere), `limit` the part worked
// (none: as the halo reaches from what shows — the base box for a whole kind or a render), `s` the scale. Its samples
// are worked as `inks` says on a page of inks. OpError (too_large) when the imaging budget cannot hold one window;
// render::Cancelled when the run is stopped (StopScope).
void apply(Surface& surface, const Kernel& kernel, const core::Json& params, core::FilterInks inks, const Rect& bounds, const std::string* cover,
           double s, const Rect* limit = nullptr, const Tiling& tiling = {});

// While it lives, the runs on this thread say how far they are (0..1, each run from 0) in `done`.
class ProgressScope {
public:
    explicit ProgressScope(std::atomic<double>& done);
    ~ProgressScope();
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

private:
    std::atomic<double>* before_;
};

// How much work a kind is on a grid of `pixels` at scale `s` (its cost a pixel × the pixels), and whether that is a
// heavy one: the window then works it on a thread of its own with 中止 and its preview at the screen's resolution
// (design 4.4, the user's decision 4 — never refused for it).
double work(std::string_view kind, const core::Json& params, std::uint64_t pixels, double s);
inline constexpr double kHeavyWork = 4e8;
// A layer's grid as a filter works it (a precise or ink layer's own; an 8-bit layer's, or lines alone, the page's at the
// working 200 dpi — at its dpi on a deep page) and its scale.
struct Grid {
    std::uint32_t width = 0, height = 0;
    double s = 1;
};
Grid layer_grid(const core::Page& page, const core::Layer& layer);
// The work of `kind` on a layer of the page (its grid, its scale).
double layer_work(const core::Page& page, const core::Layer& layer, std::string_view kind, const core::Json& params);
// A heavy filter's preview at the screen's resolution (design 4.4): a copy of the book whose layer `id` on the page at
// `at` — a precise or ink layer of more than `most` pixels on its long side — holds its pixels averaged down to fit (its
// grid stretched over the page as before, so a filter's lengths scale with it); otherwise the book as it is.
core::Document preview_book(const core::Document& doc, std::size_t at, const std::string& id, int most);

}  // namespace genko::render::fx
