#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Pen and brush kinds (Python's genko/brushes.py): the presets, the book's own brushes and how a stroke's kind is
// checked. Only the settings the ops use are read here; drawing with them is the renderer's.

namespace genko::core {

struct Brush {
    std::string key;
    std::string label;
    double width_mm = 1.0;
    double min_pressure = 0.15;
    double gamma = 1.0;
    double opacity = 1.0;
    std::int64_t stabilize = 3;
    bool taper = true;
    std::string texture;
    std::optional<std::vector<std::int64_t>> rgb;
    bool fixed_width = false;
    std::string tip = "round";
    double tip_angle = 45.0;
    double tip_ratio = 0.25;
    bool tip_follow = false;
    bool tip_rotation = false;
    std::string tip_png;
    double spacing = 0.0;
    double scatter = 0.0;
    double size_jitter = 0.0;
    bool turn_jitter = false;
    std::int64_t count = 1;
    std::string pattern;
    double speed = 0.0;
    std::int64_t post_smooth = 0;
    std::string aa = "normal";
    double stamp_size = 1.0;
    double mix = 0.0;
    double stretch = 0.0;
};

// BRUSHES, in Python's order (gpen first: the default).
std::span<const Brush> builtin_brushes();
const Brush* find_builtin(std::string_view key);

// ops._brush_kind: the kind as kept on a stroke (LEGACY names mapped: oil → marker). OpError "kind must be one of
// gpen, maru, … (or a brush defined in the book with define_brush)" for a kind that is neither a preset nor one of
// the book's brushes.
std::string brush_kind(const Json& kind, const Document& doc);

// brushes.brush(key): the preset, else the book's own brush (as Python registers the book's brushes when it reads
// the book: in order, those that make sense), else the G pen.
Brush brush_for(const Json& key, const Document& doc);

// brushes.from_dict(key, data): the brush, or nothing when Python would refuse it (ValueError, TypeError, KeyError).
// `registered`: the book's brushes made so far (a brush may start from one of them).
std::optional<Brush> brush_from_dict(const std::string& key, const Json& data, std::span<const Brush> registered);

}  // namespace genko::core
