#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/brush_dabs.hpp"
#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/dabs/coverage.hpp"
#include "render/dabs/dynamics.hpp"
#include "render/image.hpp"

// brushes@1 (PC4 E1, SPEC.md COLOR-09「ブラシのダブの描き方」): a Genko line drawn with a dab brush. The line stays a line
// (its points, pressure and inputs: the user's decision 3); render/brushes.cpp draw() lays its dabs at the resolution
// asked through this file: the dabs fixed once in mm (render/dabs/dynamics.hpp) for the line's content and its brush's
// settings, remembered for the last few hundred lines, and laid over the part of the page asked (render/dabs/coverage.hpp).
//
// The line's seed is its brush's seed xor the FNV-1a 64 hash of its id (as a paper's "stroke" coordinates take it):
// every line has its own jitter, and the same line always the same.

namespace genko::render::dabs {

// The pictures of the dab brushes (tips, the dual brush's tips, textures' tiles) this process knows, by their asset ref
// (content-addressed, as render/paper.hpp keeps the papers': only bytes that hash to their ref are kept). The books'
// pictures are made known with their brushes (render/brushes.hpp register_book, follow_book) and when a page is drawn.
bool keep_picture(const std::string& ref, core::Bytes png);
void keep_pictures(const std::map<std::string, core::Bytes>& pictures);
core::Bytes picture(const std::string& ref);
void clear_pictures();
// (PC4 E2) Where a picture this process does not hold yet is looked for when it is asked for (the window's: the person's
// brush library, render/brush_presets use_library_pictures) — read then, only the one asked, kept when its bytes hash to
// its ref; none: nowhere. The ops never look there (a book keeps its own pictures).
void set_picture_source(std::function<core::Bytes(const std::string& ref)> source);
// Whether a picture is held now (without looking for it).
bool holds_picture(const std::string& ref);

// What a line gives its dab brush beyond its points: the pen's inputs (brushes@1, one per point; lists that do not fit
// the points are not used, as the writer does not write them), the flow the window drew it with, and the colours of the
// colour dynamics (straight sRGB 0..1: its 描画色 and 背景色, white when it has none).
struct LineInputs {
    std::span<const double> tilt;     // x and y a point (degrees)
    std::span<const double> time_ms;  // a point
    std::span<const double> wheel;    // a point
    std::optional<double> flow;
    std::array<float, 3> color{20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f};
    std::optional<std::array<float, 3>> background;
    // (PC4 E1) Whether its dabs are remembered for the next time it is drawn: not a line being drawn (its points change
    // with every move of the pen, each set of them asked once).
    bool remember = true;
};

// The input points of a line: its points (mm) and pressure (none: full), its barrel's turn, tilt, time and wheel.
std::vector<InputPoint> input_points(const core::PenPoints& points, std::span<const double> rotation, const LineInputs& inputs);

// The dab brush of a line: the brush's settings with the line's width as its size, its flow (the line's, else the
// brush's), colours and seed.
LineBrush line_brush(const core::DabSettings& settings, double width_mm, std::string_view line_seed, const LineInputs& inputs);

// A line's dabs (primary and the dual brush's), in mm.
struct LineDabs {
    std::vector<Dab> dabs, dual;
};
// The dabs of a line, remembered for the last lines asked (by the line's content and the brush's settings; at most
// kDabsKept dabs all told; `remember` false: not kept). core::Error("too_large") past kMaxDabs dabs or kMaxDabWork, or a
// diameter past 500 mm.
std::shared_ptr<const LineDabs> line_dabs(const LineBrush& brush, const std::vector<InputPoint>& points, bool remember = true);
// The most dabs remembered (a dab is 88 bytes: about 90 MB).
inline constexpr std::size_t kDabsKept = 1000000;
// The dabs remembered now (tests).
std::size_t remembered_dabs();

// The brush ready to lay its dabs at `dpi` (its tips' mips and texture prepared; remembered). core::Error("missing_asset")
// for a picture this process does not know.
std::shared_ptr<const BrushContext> context(const core::DabSettings& settings, int dpi);

// The pixels the line's dabs reach at `dpi` on a page of `size` (nothing: none of them on it).
std::optional<Box> reach(const BrushContext& ctx, const LineDabs& dabs, Size size);

// The whole: a line's coverage at `dpi` over `within` (a part of the page; none: all of it).
std::shared_ptr<const LineCover> cover(const core::DabSettings& settings, double width_mm, std::string_view line_seed,
                                       const core::PenPoints& points, std::span<const double> rotation, const LineInputs& inputs, int dpi,
                                       Size size, std::optional<Box> within);

// define_brush's "format": "photocraft": upstream's BrushSettings JSON (camelCase, pixels of the working 200 dpi) as
// Genko's dabs settings, and the pictures it holds (its sampled tips and texture tile, as grey PNGs by their ref).
// `ignored` gets the settings that are the line's or the window's here (color, background, mode, smoothing).
// core::Error("value") for what cannot be read.
core::Json dabs_from_photocraft(const core::Json& upstream, std::map<std::string, core::Bytes>& pictures, std::vector<std::string>& ignored);

// A picture given to define_brush as a file (base64 PNG) taken in as a tip ("ink": its alpha when it has any
// transparency, else its darkness — as an image tip's ink) or a texture (its lightness): a grey PNG, its ref.
// core::Error("value") for a picture that cannot be read, or past `edge` pixels a side.
std::pair<std::string, core::Bytes> take_in_picture(std::string_view file, bool ink, std::uint32_t edge);

// The pencil tool's brush (鉛筆（ハード）: hard, aliased, 10 % spacing) as define_brush takes it, kept by a book under
// kPencilKey — or kPencilKey_2, _3 … when a brush of one's own of other settings has the key there (review P3-2: the
// window picks the key, MainWindow::pencil_key; one's own brush is never written over).
core::Json pencil_brush();
inline constexpr std::string_view kPencilKey = "my_genko_pencil";
// Whether a brush's settings (brush_to_dict) are the pencil's (its key aside).
bool is_pencil(const core::Json& settings);

}  // namespace genko::render::dabs
