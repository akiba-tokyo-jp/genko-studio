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

// Forget the registered brushes (a new process starts without them; the tests use this between books).
void clear_custom();

// The person's own brush library lives in the config folder, which is settled in M6: these throw NotYetPorted.
std::filesystem::path library_path();
core::Json load_library();
void save_to_library(std::string_view key, const std::optional<core::Json>& data);

// A line's coverage at a resolution, only around the line: an "L" picture and where its corner is on the page.
struct Coverage {
    Image mask;
    Point origin;
};

// draw(size, points, dpi, width_mm, kind, seed, rotation, pressure_opacity): nothing when the line has no points or
// lies off the page.
std::optional<Coverage> draw(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind,
                             std::string_view seed = {}, std::span<const double> rotation = {},
                             double pressure_opacity = 0.0);

// The box draw() would cover, without drawing it (nothing where draw() returns nothing).
std::optional<Box> extent(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind);

}  // namespace genko::render::brushes
