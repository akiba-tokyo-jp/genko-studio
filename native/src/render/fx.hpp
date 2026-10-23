#pragma once

#include <algorithm>
#include <array>
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
// and b as they were; grey as it is; or, a kernel about a middle, a Lab page's L, a and b each, Ctx::space Lab) or the
// colours the inks mean (back by the page's rules, render/ink).
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
    // (D2) The layer's own samples read in place instead of `data` (a whole kind's base box on a 32-bit precise layer, its
    // rows whole: no copy of them — design 4.3): a kernel of a whole kind reads by get, at, px or base(), never `data`.
    const float* view = nullptr;

    Window() = default;
    Window(const Rect& r, std::size_t channels) : rect(r), ch(channels), data(r.area() * channels, 0.0f) {}
    int w() const { return rect.width(); }
    const float* base() const { return view != nullptr ? view : data.data(); }
    std::size_t index(int x, int y) const { return ((std::size_t(y - rect.y0) * std::size_t(rect.width())) + std::size_t(x - rect.x0)) * ch; }
    // The sample at (x, y); 0 outside.
    float get(int x, int y, std::size_t c) const { return rect.contains(x, y) ? base()[index(x, y) + c] : 0.0f; }
    // The sample at (x, y), past the window's edge its edge pixel's (a kernel's window is cut to the layer's grid: this
    // repeats the grid's edge, upstream's read_clamped).
    float at(int x, int y, std::size_t c) const {
        if (rect.empty()) return 0.0f;
        return base()[index(std::clamp(x, rect.x0, rect.x1 - 1), std::clamp(y, rect.y0, rect.y1 - 1)) + c];
    }
    // The pixel at (x, y) (inside).
    const float* px(int x, int y) const { return base() + index(x, y); }
    // The sample with an edge rule relative to `area`.
    float get_edge(int x, int y, std::size_t c, Edge edge, const Rect& area) const;
    // The bilinear sample of the premultiplied pixel at (x, y) (pixel centres at .5) into `out` (straight colour, the
    // alpha last when `alpha`).
    void sample(float x, float y, Edge edge, const Rect& area, bool alpha, float* out) const;
    // The samples inside `out` (an identity pass).
    std::vector<float> crop(const Rect& out) const;
};

// (D2) A picture a kind reads beside the layer: width × height pixels of 8-bit straight RGBA bytes, rows from the top.
struct Picture {
    int width = 0, height = 0;
    std::string rgba;
    bool empty() const { return width <= 0 || height <= 0; }
    // Channel c of the pixel at (x, y), 0..1, clamped to the picture's edge.
    float at(int x, int y, unsigned c) const {
        const std::size_t i = (std::size_t(std::clamp(y, 0, height - 1)) * std::size_t(width) + std::size_t(std::clamp(x, 0, width - 1))) * 4 + c;
        return static_cast<unsigned char>(rgba[i]) / 255.0f;
    }
};

// (D5) What a whole kind works out once on its base box's window besides its settings (Kernel::prepare): an object of the
// kind's own, given to each tile's run (Ctx::prepared).
struct Prepared {
    virtual ~Prepared() = default;
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
    // (D1) How the layer's samples are held: each a whole number of 1/levels (255: 8 bits, 65535: 16 bits — the rank
    // filters' exact bins, render/fx_rank), 0 for floats (or the colours the inks mean); and the least colour sample
    // where it shows (0 but for floats: with `largest`, whether every sample is within 0..1).
    unsigned levels = 0;
    double least = 0;
    // (D1) The selection's coverage of each pixel of the layer's grid (a byte a pixel, `grid_width` a row; none:
    // everywhere), for a kind that reads it itself (平均: the mean of what the selection covers), and a plane over the grid
    // it reads beside the layer (ぼかし（レンズ）'s depth: the layer's mask; a value 0..1 a pixel, none: none).
    const std::string* cover = nullptr;
    std::uint32_t grid_width = 0;
    const std::vector<float>* plane = nullptr;
    float coverage(int x, int y) const {
        return cover == nullptr ? 1.0f : static_cast<unsigned char>((*cover)[std::size_t(y) * grid_width + std::size_t(x)]) / 255.0f;
    }
    // (D2) A picture a kind reads beside the layer (置き換え's displacement map, a pixel of it a working 200 dpi pixel — the
    // caller's, render/ops_filters: a layer of the book, a file's data or one the book holds); none: none.
    const Picture* map = nullptr;
    // (D3) Where the surface's (0, 0) is on the layer's grid (0, 0 but for a part of it worked alone — フィルターギャラリー's
    // preview, PlaceScope —; `bounds` is then that part's own coordinates, past its edges): a kernel that draws marks by the
    // grid's coordinates adds it.
    int x0 = 0, y0 = 0;
    // (D6) A page of inks' colour mode (none: not a page of inks): a colour a kind lays of its own (レンズ補正's edge) goes
    // onto the plates by the page's rules (render/ink inks_by_rules).
    const core::PageColor* ink = nullptr;
    // (D4) The surface's height (grid_width its width: the layer's grid — the page — but for a part of it, PlaceScope): 雲模様's
    // pattern is as large as the page is (PhotoCraft's extra_cmds.rs clouds: a quarter of the document's longer side).
    std::uint32_t grid_height = 0;
    // (D5) What the kind prepared on its base box before its tiles (Kernel::prepare; none: nothing).
    const Prepared* prepared = nullptr;
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
    // (D1) A whole kind's work done once on the base box's window before its tiles (平均's mean), given to each tile's run
    // as its settings; none: the settings as they are.
    std::function<core::Json(const core::Json& params, const Window& whole, const Ctx& ctx)> once;
    // (D1) The bytes of its own work a pixel of a tile's window takes past what the engine counts (ノイズを軽減's planes):
    // held in the imaging budget with the window.
    std::size_t work_bytes = 0;
    // (D2) The work done once whatever the layer's pixels (置き換え's map read or drawn — a page drawn, a file decoded), in
    // the units of work(): added to its weight. None: nothing.
    std::function<double(const core::Json& params)> setup;
    // (D6) Its base box is the frame of the picture — the box of what shows on the layer (a photo placed on the page, a smart
    // object's picture), a part worked alone its PlaceScope's —, not the selection's box: a selection only keeps the result
    // to it (レンズ補正, 広角補正: their centre is the lens's). Nothing shows: nothing changes.
    bool shown_frame = false;
    // (D5) A whole kind's work done once on the base box's window that is not settings — planes of the whole the tiles
    // read (Camera Raw's haze and guided tone bases) —, after once, on the caller's thread (stopped as any run: StopScope;
    // saying how far it is with ProgressScope::put up to kPrepareShare, the tiles the rest). Each tile's run has it as
    // Ctx::prepared. Called only when the kind's halo for these settings is Halo::whole() (the run reads its base box's
    // window once); with a margin it is not and Ctx::prepared is none — the run works without it (D4's ぼかしギャラリー
    // prepares its reduced levels only for the large blurs). None: nothing.
    std::function<std::shared_ptr<const Prepared>(const core::Json& params, const Window& whole, const Ctx& ctx)> prepare;
    // (D5) The bytes prepare's arrays take at most: held in the imaging budget by the run before the threads' share is
    // decided (fewer threads, not a refusal, when both do not fit), for as long as the run. None: nothing.
    std::function<std::uint64_t(const core::Json& params, const Ctx& ctx)> prepare_bytes;
    // (D5) The bytes a tile's run holds of its own past the tile and its window — a whole kind working each tile's part of
    // its base box grown by what it reaches, in planes of its own (Camera Raw's) —, for a tile of `side`: held in the imaging
    // budget with each thread's tile. None: nothing.
    std::function<std::uint64_t(const core::Json& params, const Ctx& ctx, int side)> tile_bytes;
};
using Kernels = std::vector<Kernel>;
inline constexpr double kDefaultCost = 50;
// (D5) How much of a run's progress a kind's prepare says (its tiles the rest).
inline constexpr double kPrepareShare = 0.1;

// Each group's kernels (its own file; a group fills its own).
Kernels blur_kernels();          // render/fx_blur.cpp — D0: blur_gaussian, blur_box; D1: the rest of ぼかし
Kernels blur_shaped_kernels();   // render/fx_blur2.cpp — D1: blur_smart, blur_shape, blur_lens (blur_kernels takes them)
Kernels sharpen_kernels();       // render/fx_sharpen.cpp — D1
Kernels noise_kernels();         // render/fx_noise.cpp — D1
Kernels other_kernels();         // render/fx_other.cpp — D1
Kernels video_kernels();         // render/fx_video.cpp — D1
Kernels distort_kernels();       // render/fx_distort.cpp — D2: 変形
Kernels pixelate_kernels();      // render/fx_pixelate.cpp — D2: ピクセレート
Kernels stylize_kernels();       // render/fx_stylize.cpp — D2: 表現手法 (油彩: render/fx_oil.cpp, oil_kernel)
Kernel oil_kernel();             // render/fx_oil.cpp — D2: 油彩 (stylize_kernels takes it)
Kernels gallery_kernels();       // D3
Kernels render_kernels();        // render/fx_render.cpp — D4: 雲模様 1・2, ファイバー, 逆光, 照明効果
Kernels blur_gallery_kernels();  // render/fx_gallery_blur.cpp — D4: ぼかしギャラリー
Kernels camera_raw_kernels();    // render/fx_camera_raw.cpp — D5: Camera Raw フィルター
Kernels lens_kernels();          // render/fx_lens.cpp — D6: lens_correction, adaptive_wide_angle
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

// --- box passes in whole numbers (D2's and D3's blurs: D2's review P3-9) ------------------------------------------------

// Odd box widths whose threefold convolution is a Gaussian of `sigma` (Kovesi; PhotoCraft's fxutil.rs gauss_box_radii), as
// radii.
std::array<int, 3> gauss_box_radii(float sigma);
// A running mean of radius r along a line of `len` elements `stride` words apart (`n` channels each), its ends repeated
// (fxutil.rs box_rows_n), in place, each mean rounded half away from zero to a whole number. `line`: scratch.
void whole_box_line(std::int64_t* first, std::size_t len, std::size_t stride, std::size_t n, int r, std::vector<std::int64_t>& line);
// Box passes of `radii` along the rows, then along the columns, of a w × h × n buffer of floats, in whole numbers of
// `unit` (each sample clamped to ±most first, one not finite 0): exact sums, so the same pixels give the same bytes
// whatever buffer — tile, window — holds them. Nothing when every radius is 0.
void whole_box_passes(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, const std::vector<int>& radii, double unit, double most);

// The pixels of a band of rows a whole kind's base box is read in (its window filled a band at a time, the band's
// reading held in the imaging budget with it: D2's review P2-1).
inline constexpr std::size_t kReadBandPixels = 16384;

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
    // (D1) Its samples each a whole number of 1/levels (Ctx::levels; 0: floats), and the least colour sample where it
    // shows (after content(); 0 but for floats).
    virtual unsigned levels() const { return 0; }
    virtual double least() const { return 0.0; }
    // (D2) Its samples from row y on as the straight floats a window holds (colours, then the alpha; rows whole), read in
    // place — a 32-bit precise layer's, as stored —; none for any other.
    virtual const float* floats(int y) const {
        (void)y;
        return nullptr;
    }
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
// render::Cancelled when the run is stopped (StopScope). `reads` (D1): what its kernel may read besides the layer —
// the selection's coverage when the caller keeps the result to it itself (an 8-bit layer, `cover` none: Ctx::cover), a
// plane over the grid (Ctx::plane), a picture of its own (D2: Ctx::map).
struct Reads {
    const std::string* within = nullptr;
    const std::vector<float>* plane = nullptr;
    const Picture* map = nullptr;  // (D2: Ctx::map)
};
void apply(Surface& surface, const Kernel& kernel, const core::Json& params, core::FilterInks inks, const Rect& bounds, const std::string* cover,
           double s, const Rect* limit = nullptr, const Tiling& tiling = {}, const Reads& reads = {});

// While it lives, the runs on this thread say how far they are (0..1, each run from 0) in `done`.
class ProgressScope {
public:
    explicit ProgressScope(std::atomic<double>& done);
    ~ProgressScope();
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

    // (D6) How far a run on this thread is (0..1) — an op that is not a kind's run (消点) says it so —; none: nowhere.
    static void put(double done);

private:
    std::atomic<double>* before_;
};

// (D3) While it lives, the runs on this thread work a part of a layer's grid (フィルターギャラリー's preview): the surface's
// (0, 0) at `x`, `y` of the grid (Ctx::x0, y0) and the base box `bounds` on the grid (the selection's box, else the
// grid) — what the op's run on the whole grid has there.
class PlaceScope {
public:
    PlaceScope(int x, int y, const Rect& bounds);
    ~PlaceScope();
    PlaceScope(const PlaceScope&) = delete;
    PlaceScope& operator=(const PlaceScope&) = delete;
    // The one living on this thread (none: the surface is the grid).
    static const PlaceScope* current();
    int x() const { return x_; }
    int y() const { return y_; }
    const Rect& bounds() const { return bounds_; }

private:
    const PlaceScope* before_;
    int x_, y_;
    Rect bounds_;
};

// (D6) While it lives, what a kind resolves as it runs on this thread — レンズ補正's 自動拡大・縮小, 広角補正's model, focal
// length and how straight its lines came out — is put into `into` by its key (filter_raster's report, render/ops_filters;
// a kind puts it from its `once`, on the thread that runs it).
class NoteScope {
public:
    explicit NoteScope(core::Json& into);
    ~NoteScope();
    NoteScope(const NoteScope&) = delete;
    NoteScope& operator=(const NoteScope&) = delete;
    // Put on the scope living on this thread (none: nowhere).
    static void put(std::string_view key, core::Json value);

private:
    core::Json* before_;
};

// How much work a kind is on a grid of `pixels` at scale `s` (its cost a pixel — on samples of `levels`, Ctx::levels: a
// rank filter of floats costs more — and the engine's reading and writing of a pixel, kPixelWork, × the pixels), and
// whether that is a heavy one: the window then works it on a thread of its own with 中止 and its preview at the screen's
// resolution (design 4.4, the user's decision 4 — never refused for it).
double work(std::string_view kind, const core::Json& params, std::uint64_t pixels, double s, unsigned levels = 255);
inline constexpr double kPixelWork = 10;
inline constexpr double kHeavyWork = 4e8;
// A layer's grid as a filter works it (a precise or ink layer's own; an 8-bit layer's, or lines alone, the page's at the
// working 200 dpi — at its dpi on a deep page) and its scale.
struct Grid {
    std::uint32_t width = 0, height = 0;
    double s = 1;
    unsigned levels = 255;  // (D1: 0 for a layer of floats — a 32-bit page's —, Ctx::levels)
};
Grid layer_grid(const core::Page& page, const core::Layer& layer);
// The work of `kind` on a layer of the page (its grid, its scale).
double layer_work(const core::Page& page, const core::Layer& layer, std::string_view kind, const core::Json& params);
// A heavy filter's preview at the screen's resolution (design 4.4): a copy of the book whose layer `id` on the page at
// `at` — a precise or ink layer of more than `most` pixels on its long side — holds its pixels averaged down to fit (its
// grid stretched over the page as before, so a filter's lengths scale with it); otherwise the book as it is.
core::Document preview_book(const core::Document& doc, std::size_t at, const std::string& id, int most);

}  // namespace genko::render::fx
