#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/stroke_geom.hpp"
#include "render/image.hpp"

// Pen and brush kinds (Python's genko/brushes.py): how a vector line is drawn, the presets people pick, and the
// brushes people make (J3: tips, stamps, scatter, patterns, textures, mixing). A line keeps its points and its kind;
// its look is made at render time, at the render's resolution.

namespace genko::render::brushes {

struct Brush {
    std::string key;
    std::string label;
    double width_mm = 1.0;      // the default size
    double min_pressure = 0.15;  // how thin a light touch gets (share of the width)
    double gamma = 1.0;          // pressure curve: >1 needs more force for a thick line
    double opacity = 1.0;
    std::int64_t stabilize = 3;
    bool taper = true;
    std::string texture;  // "" | grain (pencil) | soft (airbrush) | dry (brush) | water
    std::optional<std::vector<std::int64_t>> rgb;  // a fixed colour (white)
    bool fixed_width = false;
    // J3: the tip and how it is laid down
    std::string tip = "round";  // round | flat | image
    double tip_angle = 45.0;
    double tip_ratio = 0.25;
    bool tip_follow = false;
    bool tip_rotation = false;
    std::string tip_png;  // base64 picture of an image tip
    double spacing = 0.0;
    double scatter = 0.0;
    double size_jitter = 0.0;
    bool turn_jitter = false;
    std::int64_t count = 1;
    std::string pattern;  // "" | dots | dash | lace | grass | hearts | stars | leaves
    double speed = 0.0;
    std::int64_t post_smooth = 0;
    std::string aa = "normal";  // none | weak | normal | strong
    double stamp_size = 1.0;
    double mix = 0.0;
    double stretch = 0.0;

    friend bool operator==(const Brush&, const Brush&) = default;
};

inline constexpr std::string_view kDefault = "gpen";

// The built-in brushes, in Python's order.
const std::vector<Brush>& builtin();

// brush(key): LEGACY names first ("oil" → marker), then the built-in ones, then the registered ones, else the G pen.
Brush brush(std::string_view key);

// The built-in brushes and then the registered ones.
std::vector<Brush> everything();

// to_dict: the settings a book keeps (the J3 ones only when they differ from a plain round pen).
core::Json to_dict(const Brush& b);

// from_dict(key, data, base): a brush from its settings, the missing ones from `base` (or data["base"], or the
// G pen). core::Error("value") with Python's message when one is out of range or of the wrong kind.
Brush from_dict(std::string_view key, const core::Json& data, const std::optional<std::string>& base = std::nullopt);

// register(definitions): make a book's (or the library's) brushes known; ones that do not make sense are skipped,
// and built-in names are never replaced. Thread-safe.
void register_brushes(const core::Json& definitions);

// Forget the registered brushes (a new process starts without them; the tests use this between books).
void clear_custom();

// The person's own brush library lives in the config folder, which is settled in M6: these throw NotYetPorted.
std::filesystem::path library_path();
core::Json load_library();
void save_to_library(std::string_view key, const std::optional<core::Json>& data);

// brushes.smoothed (後補正).
core::PenPoints smoothed(const core::PenPoints& points, std::int64_t strength);

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
