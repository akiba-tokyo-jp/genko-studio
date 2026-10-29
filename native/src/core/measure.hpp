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

}  // namespace genko::core
