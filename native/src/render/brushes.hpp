#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/brushes.hpp"
#include "core/geometry.hpp"
#include "core/json.hpp"
#include "render/image.hpp"

// How a vector line is drawn with its brush (Python's genko/brushes.py: draw and the parts it calls). The brushes
// themselves (the presets, from_dict, to_dict, register) are core's (core/brushes.hpp); this keeps the brushes this
// process knows (Python's CUSTOM), as brushes.brush(kind) looks them up when a line is drawn.

namespace genko::render::brushes {

using Brush = core::Brush;

inline constexpr std::string_view kDefault = core::kDefaultBrush;

// brush(key): LEGACY names first ("oil" → marker), then the built-in ones, then the registered ones, else the G pen.
Brush brush(std::string_view key);

// The built-in brushes and then the registered ones.
std::vector<Brush> everything();

// register(definitions): make a book's (or the library's) brushes known; ones that do not make sense are skipped,
// and built-in names are never replaced. Thread-safe.
void register_brushes(const core::Json& definitions);

// A book read: its brushes made known (register), each in place of one of the same key, and kept as the book's last
// known brushes for follow_book.
void register_book(const core::Json& definitions);
// After a change to the book: only its brushes that are new or changed since it was last followed (define_brush, as
// Python's op puts the brush into CUSTOM) are made known again. The others stay as they are: the person's own brushes
// and the edits made to them this session are never replaced by the book's older copy, and nothing is ever taken out.
void follow_book(const core::Json& definitions);
// The same for a book with the pictures of its brushes' papers (BRUSH-01), which are made known too (paper::keep_all).
void register_book(const core::Document& doc);
void follow_book(const core::Document& doc);

// Forget the registered brushes and the book's last known ones (a new process starts without them; the tests use this
// between books).
void clear_custom();

// CUSTOM[key] = from_dict(key, data): one brush made known, or made again in its place (Python's errors when its
// settings do not make sense: PyValueError, PyTypeError …, and nothing changes); and one forgotten
// (CUSTOM.pop(key, None)). A built-in name is never replaced (PyValueError).
void define_brush(std::string_view key, const core::Json& data);
void forget_brush(std::string_view key);

// _decode_tip: the ink of an image tip given as base64 PNG (its alpha, or its darkness), or nothing for a picture that
// cannot be opened.
std::optional<Image> tip_ink(const std::string& base64_png);

// The person's own brushes (key → settings) in the config folder's brushes.json (Python's library_path,
// load_library, save_to_library; the folder is the app's: app/config.hpp). A file that cannot be read is an empty
// library; saving writes json.dumps({"brushes": …}, ensure_ascii=False, indent=1). Beyond Python, so a library is
// never lost: saving refuses (PyUncaught OSError "the brush library cannot be read, so it is left as it is: <path>")
// when the file is there but cannot be read as a library, where Python would write only the one brush over it; and
// the file is written whole or not at all (storage::write_atomic).
std::filesystem::path library_path(const std::filesystem::path& config_dir);
core::Json load_library(const std::filesystem::path& config_dir);
// (BRUSH-01) A brush's "paper" is never written into brushes.json, which Python's app reads and writes too (its
// edit would drop it): it goes to brush_papers.json beside it ({"genko_brush_papers": 1, "papers": {key: paper}},
// written whole or not at all) with its picture in brush_papers/<64 hex>.png (from paper::bytes; written first). The
// rest of the brush goes to brushes.json as before; forgetting a brush forgets its paper too. core::Error
// ("missing_asset") for a paper whose picture this process does not know; PyUncaught OSError when a file cannot be
// written (brushes.json is written first).
void save_to_library(const std::filesystem::path& config_dir, std::string_view key, const std::optional<core::Json>& data);

// BRUSH-01: the papers of the person's own brushes (this build only).
std::filesystem::path library_papers_path(const std::filesystem::path& config_dir);
// load_library with, for each brush brush_papers.json has a paper for, that paper ("paper", last) when its picture is
// there and holds what its name says (made known: paper::keep); otherwise the brush as brushes.json has it. A
// brush_papers.json that cannot be read gives no papers.
core::Json load_own_brushes(const std::filesystem::path& config_dir);

// A brush's settings with its paper's picture given as a picture file ("paper": {"png": base64, …}, as define_brush and a
// .genkobrush take it) taken in (paper::take_in, made known) and given by its "asset"; settings without one as they
// are. core::Error from take_in, core::Error("value") for a png that is not base64.
core::Json with_paper_taken_in(const core::Json& definition);
// The other way, for settings that go to another computer (a .genkobrush, define_brush into a book that does not keep
// the picture): the paper's "asset" given as its picture ("png": base64 of the grey PNG). core::Error("missing_asset")
// when this process does not know it.
core::Json with_paper_picture(const core::Json& definition);

// A line's coverage at a resolution, only around the line: an "L" picture and where its corner is on the page.
struct Coverage {
    Image mask;
    Point origin;
};

// draw(size, points, dpi, width_mm, kind, seed, rotation, pressure_opacity): nothing when the line has no points or
// lies off the page. A brush with a paper (BRUSH-01) then lays it under the coverage (render/paper.hpp apply: the
// line's seed and its first point for coords "stroke"); a brush without one draws exactly as Python's.
std::optional<Coverage> draw(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind,
                             std::string_view seed = {}, std::span<const double> rotation = {},
                             double pressure_opacity = 0.0);

// The box draw() would cover, without drawing it (nothing where draw() returns nothing).
std::optional<Box> extent(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind);

}  // namespace genko::render::brushes
