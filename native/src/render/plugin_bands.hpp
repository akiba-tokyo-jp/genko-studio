#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"

// A filter plugin's run over a picture in bands (PC5 J-f, plugins:WASM-ABI-v1, docs/PLUGIN_WASM.md): the pixels the
// plugin gets as normalised f32 samples in the picture's own colour model (PhotoCraft's plug-in ABI v1:
// `format = depth | alpha << 8 | mode << 16`, the mode numbered as in the PSD file format), cut into tile-aligned bands of
// about 4 MiB each grown by the plugin's overlap (the picture's edge pixels repeated past it), each band handed to the
// plugin (a fresh instance of a WebAssembly module, or a native plugin of protocol 2) with its settings and `_image`
// (where the band is), and what comes back cleaned — NaN and infinities keep the sample they had, integer depths and the
// alpha kept to 0..1 — and laid over the picture by the selection's coverage: original + (filtered − original) ×
// coverage. PhotoCraft v0.2.0's crates/plugins/src/apply.rs (MIT, native/third_party/photocraft/NOTICE.md), on Genko's
// pictures instead of PhotoCraft's tiled surfaces.
//
// (review P2-1) The picture is never held whole in f32: the bands read it row by row from where it is (an 8-bit picture,
// a precise raster, a page's inks) and write each tile row of bands back once all of that row is made; the rows a band's
// margin reaches above it, already written, come from a strip kept of them as they were (overlap rows at most). What a
// run holds — the band and its original, a tile row of results, the strip — is borrowed from the imaging budget the
// caller is in (ImageAllocationBudget): core::OpError when it has not that much left.

namespace genko::render::plugin_bands {

inline constexpr unsigned kAlphaBit = 1u << 8;
inline constexpr int kTile = 256;  // (PhotoCraft's TILE_SIZE: the bands' grid on the picture)

// The colour modes of `format`, as PSD numbers them.
enum class Mode : unsigned { Bitmap = 0, Gray = 1, Rgb = 3, Cmyk = 4, Multichannel = 7, Duotone = 8, Lab = 9 };
std::string mode_name(Mode mode);
// The mode a page's inks are given in (CMYK, Lab, grey — a duotone page's Duotone, a bitmap page's Bitmap —,
// multichannel; a channel's one plane of density: grey). Their samples go as InkPixels::value reads them (CMYK: 0 no ink;
// Lab: the ICC encoding, a and b centred near 0.5; grey: lightness, 0 black).
Mode mode_of_inks(const core::PageColor& color, core::InkSpace space);

// A picture as a plugin is given it: `channels` samples a pixel (the colours, then the straight alpha when `alpha`), each
// 0..1 (a 32-bit picture's colours may be beyond), read and written a row segment at a time where it is kept.
struct Picture {
    int width = 0;
    int height = 0;
    unsigned channels = 4;
    int depth = 8;  // 8 | 16 | 32: the picture's own (the samples are f32 at every depth)
    bool alpha = true;
    Mode mode = Mode::Rgb;
    // Row y's samples of the columns x0 ≤ x < x1 (inside the picture) into `out` ((x1 − x0) × channels), as they are.
    std::function<void(int y, int x0, int x1, float* out)> read;
    // And back.
    std::function<void(int y, int x0, int x1, const float* in)> write;
    // The selection's coverage (0..1) of row y's columns x0..x1; none: every pixel fully.
    std::function<void(int y, int x0, int x1, float* out)> coverage;

    unsigned format() const { return static_cast<unsigned>(depth) | (alpha ? kAlphaBit : 0u) | (static_cast<unsigned>(mode) << 16); }
};

// A picture held whole as samples, rows from the top (tests, small pictures): read and written in place.
struct Samples {
    int width = 0;
    int height = 0;
    unsigned channels = 4;
    int depth = 8;
    bool alpha = true;
    Mode mode = Mode::Rgb;
    std::vector<float> data;

    unsigned format() const { return static_cast<unsigned>(depth) | (alpha ? kAlphaBit : 0u) | (static_cast<unsigned>(mode) << 16); }
};
// The Picture over Samples (and a coverage of one value a pixel, or none), in place.
Picture picture_of(Samples& samples, const std::vector<float>* coverage = nullptr);

// What the plugin says of itself that the bands need (its manifest's `overlap` and `area`), and its host's limits.
struct Shape {
    unsigned overlap = 0;                 // pixels of context around each output pixel (0..256)
    bool whole_canvas = false;            // area "canvas": every pixel, the clear ones too; else the content's box
    std::size_t band_bytes = 4u << 20;    // the bands' target size (f32)
    std::size_t memory_bytes = 512u << 20;  // the plugin's memory cap: a band (with its overlap) takes half at most
};

// One band through the plugin: `band` (width × height × channels, in place), its settings with `_image`.
using BandWork = std::function<void(std::vector<float>& band, int width, int height, const core::Json& params)>;

// The box a run writes: the picture's content (its pixels with alpha above 0; a picture without alpha: all of it) grown
// by the overlap, or the whole picture, kept to the coverage's (pixels above 0). Empty: nothing to do.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
Box output_area(const Picture& picture, const Shape& shape);

// The most bytes a run over a picture of this size holds at once (the band and its original, a tile row of results, the
// strip of rows its margins reach, the rows read to find the content's box): what it borrows from the imaging budget.
std::uint64_t held_bytes(int width, int height, unsigned channels, const Shape& shape);

// Runs the plugin over `picture` band by band, written back where it is kept. `params`: the settings as given (each band
// gets them with `_image`). render::Cancelled when `stop` is asked between bands; the work's own errors as thrown.
// core::Error("plugin_runner") when a band does not fit the plugin's memory; core::OpError when the imaging budget has
// not room for what the run holds.
void run(Picture& picture, const Shape& shape, const core::Json& params, const BandWork& work, const std::stop_token& stop = {});

}  // namespace genko::render::plugin_bands
