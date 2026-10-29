#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// measure@1 (PC5 節 J、SPEC.md PROD-01「計測」・schema-v4.md「制作の補助」): a page's measuring — its measurement scale
// (イメージ → 解析 → 計測スケールを設定), the count tool's groups and marks, the ruler tool's line. The page's `measure`:
//   {"scale": {"pixel_length": >0, "logical_length": >0, "units": str} | null,   (null: 1 pixels = 1 pixels, Photoshop's
//                                                                                 初期設定; pixel_length in the page's pixels)
//    "count_groups": [{"name": str, "color": [r, g, b], "marker_size": 1..10, "label_size": 8..72, "visible": bool,
//                      "points": [[x_mm, y_mm], …]}],
//    "active_group": 0.. (an index of count_groups; 0 when there is none),
//    "ruler": {"start": [x_mm, y_mm], "end": [x_mm, y_mm], "protractor": [x_mm, y_mm] | null} | null}
// After PhotoCraft v0.2.0's doc/src/analysis.rs (MIT). A page whose measuring is all the defaults has none (not written,
// no feature). The measurement log is the app's for the session only (not in the book: J-d2). PC5 J-0 is the frame; the
// ops, tools and PSD 1074 are J-d1's (render/ops_measure, app/canvas_measure, render/psd_production).

namespace genko::core {

struct Document;

inline constexpr std::string_view kMeasureFeature = "measure@1";
inline constexpr std::string_view kMeasureKey = "measure";
inline constexpr std::size_t kCountGroupsMax = 256;
inline constexpr std::size_t kCountPointsMax = 10000;  // marks a group holds

struct MeasurementScale {
    double pixel_length = 1;
    double logical_length = 1;
    std::string units = "pixels";

    bool operator==(const MeasurementScale&) const = default;
};

struct CountGroup {
    std::string name;
    std::array<int, 3> color{255, 0, 0};
    int marker_size = 1;
    int label_size = 8;
    bool visible = true;
    std::vector<std::array<double, 2>> points;

    bool operator==(const CountGroup&) const = default;
};

struct RulerLine {
    std::array<double, 2> start{};
    std::array<double, 2> end{};
    std::optional<std::array<double, 2>> protractor;

    bool operator==(const RulerLine&) const = default;
};

struct Measure {
    std::optional<MeasurementScale> scale;
    std::vector<CountGroup> count_groups;
    std::size_t active_group = 0;
    std::optional<RulerLine> ruler;

    bool empty() const { return !scale && count_groups.empty() && active_group == 0 && !ruler; }
    bool operator==(const Measure&) const = default;
};

Json to_json(const Measure& measure);
// core::Error("value") says what is not so.
Measure measure_from_json(const Json& value);
void check_measure(const Measure& measure);

bool uses_measure(const Document& doc);
void validate_measure(const Document& doc);

// --- PC5 J-d1: the measuring's values (PhotoCraft v0.2.0's doc/src/analysis.rs and engine/src/analysis_cmds.rs, MIT) ----

struct Page;

// A page's pixels a mm: its resolution (350 dpi when it has none), the pixels the measurement scale and the ruler's
// readout count in.
double page_px_per_mm(const Page& page);
// Photoshop's 初期設定: 1 pixels = 1 pixels (a page without a scale).
bool is_default(const MeasurementScale& scale);
// Logical units a page pixel (1 for none or the default).
double scale_factor(const std::optional<MeasurementScale>& scale);
// The scale's words as the measurement log's Scale column has them ("100 pixels = 2.5000 cm").
std::string describe(const MeasurementScale& scale);
// The scale as the ops and `genko inspect --measure` answer it: {pixel_length, logical_length, units, factor, text,
// default} (none: the default).
Json scale_json(const std::optional<MeasurementScale>& scale);

// A new count group: the page's `n`th (0 first) — its name カウントグループ n+1 and Photoshop's colours in turn unless given.
CountGroup new_count_group(std::size_t n, const std::optional<std::string>& name = std::nullopt,
                           const std::optional<std::array<int, 3>>& color = std::nullopt);
// Every count mark of the page.
std::size_t count_total(const Measure& measure);
// The count groups as the ops and `genko inspect --measure` answer them: {total, active_group, groups: [{name, color,
// marker_size, label_size, visible, count, points}]}.
Json count_json(const Measure& measure);

// The ものさしツール's readout of a line (its places in mm on a page of `px_per_mm`): X and Y its start, W and H, A the
// angle (degrees counter-clockwise from the page's x with y up, -180..180; with a protractor, between its two arms, 0..180),
// L1 and L2 the arms' lengths — in the page's pixels, as Photoshop's options bar —, the same in mm, and `length` L1 by
// the measurement scale (its units).
struct RulerReadout {
    double x = 0, y = 0, w = 0, h = 0, l1 = 0;
    std::optional<double> l2;
    double x_mm = 0, y_mm = 0, w_mm = 0, h_mm = 0, l1_mm = 0;
    std::optional<double> l2_mm;
    double angle = 0;
    double length = 0;
    std::string units = "pixels";
    bool protractor = false;
};
RulerReadout ruler_readout(const RulerLine& ruler, const std::optional<MeasurementScale>& scale, double px_per_mm);
Json to_json(const RulerReadout& readout);
// レイヤーの角度補正's turn (degrees clockwise on the page, y down) that lays the line level or upright, whichever is nearer
// (core::lens::straighten_angle).
double straighten_turn(const RulerLine& ruler);
// The largest upright rectangle inside a `w` × `h` rectangle turned by `radians` (its width and height): what 角度補正
// with 切り抜き keeps.
std::array<double, 2> inscribed(double w, double h, double radians);

}  // namespace genko::core
