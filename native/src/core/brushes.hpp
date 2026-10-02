#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Pen and brush kinds (Python's genko/brushes.py): the presets people pick, the brushes people make (J3: tips,
// stamps, scatter, patterns, textures, mixing) and how a stroke's kind is checked. A line keeps its points and its
// kind; drawing it with its brush is the renderer's (render/brushes.hpp).

namespace genko::core {

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

inline constexpr std::string_view kDefaultBrush = "gpen";

// BRUSHES, in Python's order (gpen first: the default).
std::span<const Brush> builtin_brushes();
const Brush* find_builtin(std::string_view key);

// brushes.brush(key), the brushes people made being `custom` (Python's CUSTOM, in the order they were first
// registered): LEGACY names first ("oil" → marker), then the presets, then `custom`, else the G pen. PyTypeError
// "unhashable type: 'list'" for a list or a dict.
Brush find_brush(const Json& key, std::span<const Brush> custom);

// brushes.to_dict: the settings a book keeps (the J3 ones only when they differ from a plain round pen).
Json brush_to_dict(const Brush& b);

// brushes.from_dict(key, data, base) with `custom` known: a brush from its settings, the missing ones from `base` (or
// data["base"], or the G pen). Python's errors: PyValueError with its message when a setting is out of range or of
// the wrong kind, and float()'s and int()'s (PyValueError, PyTypeError, PyUncaught) for values that are not numbers.
Brush brush_from_dict(std::string_view key, const Json& data, const std::optional<std::string>& base,
                      std::span<const Brush> custom);

// brushes.register(definitions) into `custom` (Python's CUSTOM): each brush that makes sense, in order (a brush may
// start from one registered before it); built-in names are never replaced, a key registered again keeps its place.
// Those Python skips (ValueError, TypeError, KeyError) are skipped; its other errors (OverflowError) go through.
void register_brushes(const Json& definitions, std::vector<Brush>& custom);

// ops._brush_kind: the kind as kept on a stroke (LEGACY names mapped: oil → marker). OpError "kind must be one of
// gpen, maru, … (or a brush defined in the book with define_brush)" for a kind that is neither a preset nor one of
// the book's brushes.
std::string brush_kind(const Json& kind, const Document& doc);

// brushes.brush(key) as the ops see it: the book's own brushes registered, as Python's reader registers them.
Brush brush_for(const Json& key, const Document& doc);

}  // namespace genko::core
