#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/brushes.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// BRUSH-01 紙質: the paper a brush's lines are laid on (this build only: docs/cpp-migration/SPEC.md BRUSH-01,
// schema-v4.md paper-texture@1). A picture file is taken in once as a grey picture (take_in) that the book keeps as an
// asset; the brush keeps its settings (core::Paper). Every step from a pixel of the page to the ink left on it is fixed
// here, in whole numbers wherever it can be, so a line draws the same pixels after saving, on another computer (no
// library maths: sin and cos are this file's, IEEE double arithmetic without contraction, GenkoFlags), at any
// resolution (the picture is fixed to the page's mm) and in print.
//
// The order, for a pixel (X, Y) of a line's coverage at `dpi` (render/brushes.hpp draw):
//   1. the brush's own coverage: its tip, stamps, texture and edges (as Python's brushes.draw), then its pressure's
//      lightening (pressure_opacity);
//   2. the paper: x_mm = (X + 0.5) * 25.4 / dpi (and y), less the anchor (0, 0 for coords "paper"; the line's first
//      point for "stroke"); turned by -rotation: u_mm = dx * cos + dy * sin, v_mm = dy * cos - dx * sin; in pixels of
//      the picture: u = u_mm / (scale * 25.4 / 300) (and v); flip_x: u = -u, flip_y: v = -v; then in 1/256 pixel,
//      U = floor((u - 0.5) * 256) + ou * 256 (and V), where (ou, ov) come from the seed (offsets); the four pixels
//      around it (whole steps of U >> 8, V >> 8; the picture repeated, or mirrored, seam) mixed with weights of 1/256
//      (bilinear, rounded half up); invert: 255 - value. The ink taken away: A = ((255 - value) * D + 127) / 255 with
//      D = floor(density * 255 + 0.5); multiply: cover * (255 - A) / 255 rounded half up; subtract: max(0, cover - A);
//   3. the line's opacity (the stroke's × the brush's: render/page.cpp), then the lines of one colour together.

namespace genko::render::paper {

// One pixel of a paper picture is 1/300 inch (25.4 / 300 mm) on the page at scale 1.
inline constexpr double kBaseDpi = 300.0;
// The largest picture a paper takes (each side, in pixels) and the largest file.
inline constexpr int kMaxSide = 4096;
inline constexpr std::size_t kMaxFileBytes = 64ULL * 1024 * 1024;

// The grey picture of a paper: 255 where the paper takes all of the ink, 0 where it takes the most.
struct Grain {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> values;  // row by row

    int at(std::int64_t x, std::int64_t y) const { return values[static_cast<std::size_t>(y * width + x)]; }
};

// A picture file (PNG, JPEG, BMP or GIF) taken in as a paper: laid over white (what is transparent is white paper:
// it takes nothing from the ink), made grey as Pillow's convert("L") makes it (ITU-R 601-2) and written as a grey PNG:
// the bytes the book keeps. A 16-bit grey picture is taken by its upper 8 bits first (Pillow's convert("L") cuts its
// values at 255, which would make a scan of paper almost white; this build's own choice: Python has no paper). core::Error
// with these codes (the reason in English):
//   paper_file_too_large  more than kMaxFileBytes
//   paper_too_large       a side over kMaxSide pixels (refused from its header, before its pixels are decoded)
//   paper_empty           no pixels
//   paper_format          a picture this build does not read (TIFF, WebP, PSD)
//   paper_unreadable      not a picture, or a broken one
std::string take_in(std::string_view file_bytes);

// The grain a kept picture holds (take_in's PNG; any other PNG is made grey the same way). core::Error as take_in's.
Grain grain_of(std::string_view png);

// The pictures this process knows, by their asset ref: content-addressed, so one ref is always the same picture, of
// whichever book it came (the books' papers are made known with their brushes: render/brushes.hpp register_book,
// follow_book; render_page makes its book's known). Only bytes that hash to their ref are kept (keep: whether they
// did): another picture under a ref is never drawn, nor passed on to another book or a brush file. Thread-safe.
bool keep(const std::string& ref, core::Bytes png);
void keep_all(const std::map<std::string, core::Bytes>& papers);
// The PNG of a ref (null when it is not known).
core::Bytes bytes(const std::string& ref);
// Its grain, decoded once (a few kept). core::Error("missing_asset") for a ref this process does not know.
std::shared_ptr<const Grain> grain(const std::string& ref);
// Forget them all (the tests: another computer).
void clear();

// The pixel of the picture where it starts (ou, ov): from the seed (SplitMix64 of it; for coords "stroke" xor'd with
// the FNV-1a hash of the line's seed, its id, so each line starts elsewhere), modulo the picture's size.
std::pair<std::int64_t, std::int64_t> offsets(const core::Paper& paper, std::string_view line_seed, int width, int height);

// sin and cos of an angle in degrees: whole quarter turns exactly, the rest by a fixed polynomial (only +, -, *, /),
// the same bits on every computer.
std::pair<double, double> sin_cos_degrees(double degrees);

// One pixel of the page as the paper sees it (step 2 above), each step kept: the fixture's (data/paper/samples.json).
struct Sample {
    double x_mm = 0, y_mm = 0;  // the pixel's centre on the page
    double u = 0, v = 0;        // in pixels of the picture (before the offset)
    std::int64_t U = 0, V = 0;  // in 1/256 pixel, the offset added
    int value = 0;              // 0..255 (inverted when invert)
};
Sample sample(const Grain& grain, const core::Paper& paper, int dpi, std::int64_t px, std::int64_t py, double anchor_x_mm,
              double anchor_y_mm, std::pair<std::int64_t, std::int64_t> offset);

// The ink a coverage value keeps on a pixel of paper of this value (step 2's last part).
int blend(int cover, int value, const core::Paper& paper);

// The paper laid under a line's coverage ("L", its corner at `origin` on the page at `dpi`): every pixel through
// sample and blend. `line_seed` and the anchor (the line's first point, mm) are used for coords "stroke".
// core::Error("missing_asset") when the picture is not known.
void apply(Image& mask, Point origin, int dpi, const core::Paper& paper, std::string_view line_seed, double anchor_x_mm,
           double anchor_y_mm);

}  // namespace genko::render::paper
