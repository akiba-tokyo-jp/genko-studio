#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// brushes@1 (PC4 E-0, schema-v4.md §3, SPEC.md COLOR-08「ブラシ・塗り・パターン・プリセット」): a brush's dab engine — PhotoCraft
// v0.2.0's paint/src/brush.rs BrushSettings (MIT: native/third_party/photocraft/NOTICE.md), in Genko's units and names
// (mm, snake_case). A brush of one's own (brush.custom[key]) with "engine": "dabs" keeps these under "dabs": its line is
// still a line (points, pressure, its kind: the user's decision 3) drawn by laying dabs along it (E1). Only the settings
// that differ from the defaults below are written; a book whose brushes have none and whose lines have none of the
// inputs below is written as before (no feature).
//
// The pictures (a sampled tip, the dual brush's, a texture's tile) are assets of the book: grey PNGs (8 or 16 bits) of
// at most 2500 pixels a side (a texture's 1024), held in Document::brush_tips by their ref, as the papers are.

namespace genko::core {

inline constexpr std::string_view kBrushesFeature = "brushes@1";
inline constexpr std::string_view kDabEngine = "dabs";
inline constexpr std::uint32_t kMaxTipEdge = 2500;     // (upstream's abr_map MAX_TIP_EDGE)
inline constexpr std::uint32_t kMaxPatternEdge = 1024;  // (MAX_PATTERN_EDGE)
inline constexpr std::int64_t kMaxDabSeed = 2147483647;

// What drives a dynamic setting (Photoshop's コントロール).
enum class DabControl { Off, Fade, Pressure, Tilt, Wheel, Rotation, InitialDirection, Direction };

// A dynamic setting: a random jitter per dab and a control with its floor (upstream's Dynamic).
struct DabDynamic {
    double jitter = 0.0;           // 0..1 (the scatter's amount: 0..10)
    DabControl control = DabControl::Off;
    std::int64_t fade_steps = 25;  // 1..9999 (フェード)
    double minimum = 0.0;          // 0..1
    friend bool operator==(const DabDynamic&, const DabDynamic&) = default;
};

// How a grey mask (a texture, the dual brush's tip) is laid on the coverage.
enum class DabMaskMode { Multiply, Subtract, Darken, Overlay, ColorDodge, ColorBurn, LinearBurn, HardMix, LinearHeight, Height };

// A texture's picture: one made by the program (noise, canvas, paper, dots; `size` pixels a side) or a tile of the
// book's (`asset`, a grey PNG). One pixel of it is the texture's scale / 300 inch on the page (as a paper's).
struct DabPattern {
    std::string procedural = "paper";  // noise | canvas | paper | dots (when no asset)
    std::int64_t size = 128;           // 1..1024
    std::int64_t seed = 1;             // 0..kMaxDabSeed
    std::string asset;                 // "sha256:…": a tile of the book's (then the rest is not used)
    friend bool operator==(const DabPattern&, const DabPattern&) = default;
};

struct DabShape {  // シェイプ
    bool enabled = false;
    DabDynamic size, angle, roundness;
    bool flip_x_jitter = false, flip_y_jitter = false;
    friend bool operator==(const DabShape&, const DabShape&) = default;
};

struct DabScatter {  // 散布
    bool enabled = false;
    DabDynamic scatter;  // (its jitter is the amount, 0..10: 1 = up to one radius)
    bool both_axes = false;
    std::int64_t count = 1;  // 1..16
    DabDynamic count_jitter;
    friend bool operator==(const DabScatter&, const DabScatter&) = default;
};

struct DabTexture {  // テクスチャ
    bool enabled = false;
    DabPattern pattern;
    bool invert = false;
    double scale = 1.0;       // 0.01..10
    double brightness = 0.0;  // -1..1
    double contrast = 0.0;    // -1..1
    bool each_tip = false;
    DabMaskMode mode = DabMaskMode::Multiply;
    double depth = 1.0;       // 0..1
    DabDynamic depth_jitter;
    friend bool operator==(const DabTexture&, const DabTexture&) = default;
};

struct DabDual {  // デュアルブラシ
    bool enabled = false;
    DabMaskMode mode = DabMaskMode::Multiply;
    std::string tip;  // "" round, else an asset ref
    double size_mm = 2.54, hardness = 1.0, roundness = 1.0, angle = 0.0, spacing = 0.25, scatter = 0.0;
    bool both_axes = false;
    std::int64_t count = 1;
    bool flip = false;
    friend bool operator==(const DabDual&, const DabDual&) = default;
};

struct DabColor {  // カラー
    bool enabled = false;
    bool per_tip = true;
    DabDynamic fg_bg;
    double hue_jitter = 0.0, saturation_jitter = 0.0, brightness_jitter = 0.0;  // 0..1
    double purity = 0.0;                                                        // -1..1
    friend bool operator==(const DabColor&, const DabColor&) = default;
};

struct DabTransfer {  // 伝達
    bool enabled = false;
    DabDynamic opacity, flow;
    friend bool operator==(const DabTransfer&, const DabTransfer&) = default;
};

struct DabPose {  // ブラシポーズ
    bool enabled = false;
    double tilt_x = 0.0, tilt_y = 0.0;  // -90..90 degrees
    double rotation = 0.0;              // 0..360 degrees
    double pressure = 1.0;              // 0..1
    bool override_tilt = false, override_rotation = false, override_pressure = false;
    friend bool operator==(const DabPose&, const DabPose&) = default;
};

// A dab brush's settings (upstream's BrushSettings, its pixels as mm: 2.54 mm is upstream's 20 pixels at the working
// 200 dpi; its colours, blend and smoothing are the line's and the window's, not the brush's).
struct DabSettings {
    double size_mm = 2.54;  // 0.01..500: the size the brush is picked at (the line keeps its own width_mm)
    double hardness = 1.0;  // 0..1
    double spacing = 0.1;   // 0.01..10 of the diameter
    double angle = 0.0;     // -360..360 degrees, counter-clockwise
    double roundness = 1.0;  // 0..1
    bool flip_x = false, flip_y = false;
    bool aliased = false;  // (the pencil's: no anti-aliasing)
    double opacity = 1.0, flow = 1.0;  // 0..1
    bool pressure_size = true, pressure_opacity = false;
    bool erase = false;
    std::string tip;  // "" round, else an asset ref (a sampled tip)
    DabShape shape;
    DabScatter scatter;
    DabTexture texture;
    DabDual dual;
    DabColor color;
    DabTransfer transfer;
    DabPose pose;
    bool noise = false, wet_edges = false, build_up = false;
    double build_up_rate = 20.0;  // 1..1000 dabs a second
    bool protect_texture = false;
    std::int64_t seed = 0;  // 0..kMaxDabSeed
    friend bool operator==(const DabSettings&, const DabSettings&) = default;
};

// The settings as a book keeps them: only those that differ from the defaults, in the order above (a section only when
// it differs; within it, only its settings that differ). The defaults are an empty object.
Json dabs_to_json(const DabSettings& settings);
// The settings from that form. core::Error("value", "dabs …") for an unknown key, a value of the wrong kind (a number
// that is not finite, a bool for a number, a whole number given as a fraction), out of its range, a choice not named
// above, or a picture that is not an asset ref.
DabSettings dabs_from_json(const Json& data);
// The pictures the settings name (the tip, the dual brush's tip, the texture's tile), each once.
std::vector<std::string> dab_assets(const DabSettings& settings);

// Whether a brush of one's own (its settings as brush.custom keeps them) draws with dabs ("engine": "dabs").
bool is_dab_brush(const Json& brush);
// Every picture the book's dab brushes name, each once, in their order (those that cannot be read left out: the reader
// reports them).
std::vector<std::string> dab_asset_refs(const Json& brush_custom);

// A line's inputs (brushes@1; strokes_blob's "tilt", "time_ms", "wheel" — one per point, as "r" — and "flow"): their
// check. core::Error("format") for a list that is not one per point (tilt: x and y per point), a tilt past ±90°, a time
// below 0 or not finite, a wheel or a flow out of 0..1.
void validate_stroke_inputs(const Stroke& stroke);
// Whether a list of a line's inputs is there and is `per_point` values for each of its `points`; and whether the line
// has an input that is written (a list that fits, or a flow). An op that changes a line's points without carrying its
// inputs (as liquify carries "rotation") leaves lists that no longer fit: they are not written (E1 carries them).
bool inputs_fit(const std::vector<double>& values, std::size_t points, std::size_t per_point);
bool has_stroke_inputs(const Stroke& stroke);

// Whether the book uses brushes@1 (a dab brush, a line with inputs, the brush state's kind or background_rgb).
bool uses_brushes(const Document& doc);
// The book's brushes@1 as the reader and writer check it: each dab brush's settings (dabs_from_json; "dabs" without
// "engine", or an engine but dabs: refused), its pictures held (Document::brush_tips) and readable as tips, the brush
// state's kind (1..40 characters) and background_rgb (three whole numbers 0..255). core::Error("value") says what.
void validate_brushes(const Document& doc);
// A tip's or a texture's picture: a grey PNG of 8 or 16 bits, at most `edge` pixels a side (its header read). The
// reason when it is not, else none.
std::optional<std::string> check_tip_png(std::string_view png, std::uint32_t edge);

}  // namespace genko::core
