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

// BRUSH-01 紙質 (this build only; schema-v4.md §3 paper-texture@1): a picture of paper under a brush's line. The picture
// is an asset of the book (a grey PNG, render/paper.hpp makes it from the file taken in: its brightness is where the
// paper takes the ink, its darkness where the ink is lost); these are the settings a brush keeps with it.
struct Paper {
    std::string asset;               // "sha256:…": assets/<ab>/<64 hex>.png
    double density = 0.5;            // 0..1: how much of the ink the paper's dark parts take away
    double scale = 1.0;              // 0.1..10: one pixel of the picture is scale / 300 inch on the page
    double rotation = 0.0;           // -360..360 degrees, clockwise on the page
    bool flip_x = false;             // 左右反転
    bool flip_y = false;             // 上下反転
    bool invert = false;             // 濃淡の反転
    std::string blend = "multiply";  // multiply (乗算) | subtract (減算)
    std::string coords = "paper";    // paper (紙面固定: the page's mm) | stroke (線ごと: from the line's first point)
    std::string seam = "repeat";     // repeat (繰り返し) | mirror (折り返し)
    std::int64_t seed = 0;           // 0..2147483647: where the picture starts (render/paper.hpp)

    friend bool operator==(const Paper&, const Paper&) = default;
};

inline constexpr std::string_view kPaperFeature = "paper-texture@1";
inline constexpr std::int64_t kPaperMaxSeed = 2147483647;

// The paper as a brush keeps it: every setting, in this order (asset, density, scale, rotation, flip_x, flip_y, invert,
// blend, coords, seam, seed).
Json paper_to_json(const Paper& paper);

// The paper from its settings (the missing ones as above). PyValueError "paper …" for anything but an object of these
// keys with a valid asset ref, finite numbers in range (an int or a float, not a bool), bools and the named choices. A
// picture given as "png" is not a setting: define_brush takes it in first (render/paper.hpp).
Paper paper_from_json(const Json& data);

// The refs of the papers a book's brushes (brush.custom) keep, each once, in their order; those that cannot be read
// are left out (the reader reports them).
std::vector<std::string> paper_refs(const Json& brush_custom);

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
    std::optional<Paper> paper;  // BRUSH-01: none for every brush Python knows

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

// brushes.to_dict: the settings a book keeps (the J3 ones only when they differ from a plain round pen), and "paper"
// last for a brush with one (paper_to_json; never for one without: its settings stay as Python writes them).
Json brush_to_dict(const Brush& b);

// brushes.from_dict(key, data, base) with `custom` known: a brush from its settings, the missing ones from `base` (or
// data["base"], or the G pen). Python's errors: PyValueError with its message when a setting is out of range or of
// the wrong kind, and float()'s and int()'s (PyValueError, PyTypeError, PyUncaught) for values that are not numbers.
// "paper" (this build): null for none, an object for one (paper_from_json, checked after Python's settings); when
// data does not say, the base's.
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
