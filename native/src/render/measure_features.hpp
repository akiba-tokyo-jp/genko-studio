#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PC5 J-d2 (SPEC.md PROD-01「計測」): イメージ → 解析 → 計測値を記録's values and the measurement log's columns —
// PhotoCraft v0.2.0's crates/engine/src/analysis_cmds.rs (COLUMNS, available, DataPoints, measure_features, median,
// summary, selection_rows, record, log_columns, csv_field, log_csv; MIT: native/third_party/photocraft/NOTICE.md), its
// formulas as they are: the selection (coverage ≥ 50 %) split into 8-connected features, a summary row first and a row for
// each feature when there are more than one; area the pixels counted, perimeter the marching-squares contour through the
// pixels' centres (straight edges exact, √½ a diagonal step), circularity 4π·area/perimeter² (1 at most), the gray values
// the luminosity 0.30 R + 0.59 G + 0.11 B of the page's look in its depth's range (8 bits 0..255, 16 bits 0..32768, 32 bits
// 0..1), integrated density area × mean, the lengths and areas by the page's measurement scale.
//
// Genko counts a selection on the page's selection grid (render/selection grid_dpi: 200 dpi on an 8-bit page, its pixel
// layers' finest on a precise or ink page) and gives the pixels in the page's resolution (core::page_px_per_mm): an area
// times (page px / grid px)², a length times page px / grid px. The gray values: an 8-bit page's look (as the screen shows
// it); a precise page's values (16 bits, 32 bits linear as they are); a grey ink page's ink turned to its lightness; a CMYK
// or Lab page's look (its proof, as the screen shows it).
//
// The measurement log itself is the app's for the session (app/measurement_log): not in the book.

namespace genko::render::measure {

// One feature of a selection (or the summary of all): its bounds in pixels [x0, x1) × [y0, y1).
struct Feature {
    double area = 0;
    double perimeter = 0;
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool bounded = false;
    double gray_min = 0, gray_max = 0, gray_mean = 0, gray_median = 0;
    std::vector<std::uint64_t> histogram;  // (256 bins of the gray 0..1)

    double circularity() const;
    std::int64_t width() const { return x1 - x0; }
    std::int64_t height() const { return y1 - y0; }
};

// The feature rows 計測値を記録 gives at most (the review's P1-1: a lacy selection — screentone dots — has hundreds of
// thousands): past it the summary counts them all, the rows are the first ones', and a note says so.
inline constexpr std::size_t kFeatureRowsMax = 10000;
// What measuring a selection may hold at once (its mask, gray values, labels, the features' table, the pictures drawn for
// it): the imaging budget record opens (ImageAllocationBudget), as the filters'.
inline constexpr std::uint64_t kMeasureBudget = 2ULL * 1024 * 1024 * 1024;

// Bytes borrowed from the imaging budget the caller is in (none: nothing counted), held until this goes. core::OpError
// "the selection measured is too large to count in memory" when the budget has not that much left.
class Leases {
public:
    void borrow(std::uint64_t bytes);

private:
    std::vector<std::shared_ptr<void>> held_;
};

// measure_features: `mask` (w × h, non-zero: selected) split into 8-connected features in the order of their first pixel
// (scan order), each measured against `gray` (0..1, w × h; NaN read as 0); `origin` offsets the bounds; `range` what a gray
// of 1 reads as. Only the first `detailed` have their histogram and median (the others' histogram empty, median 0). The
// labels and the tables borrowed into `leases` (none: its own, given back on return).
std::vector<Feature> measure_features(std::span<const std::uint8_t> mask, std::span<const float> gray, std::size_t w, std::size_t h,
                                      std::array<std::int64_t, 2> origin, double range,
                                      std::size_t detailed = std::numeric_limits<std::size_t>::max(), Leases* leases = nullptr);
// The median of values 0..65535 (as 0..1): the upper middle one, or of an even count the mean of the two middle ones.
double median(std::vector<std::uint16_t>& values);
// The summary over features: their areas and perimeters added, bounds joined, the area-weighted mean, `median` given (the
// median of every value), the histogram given (none: the features' added).
Feature summary(const std::vector<Feature>& features, double median_of_all, const std::vector<std::uint64_t>* histogram_of_all = nullptr);

// --- the data points (the log's columns) -------------------------------------------------------------------------------

struct Column {
    std::string_view key;      // (PhotoCraft's key: label, dateTime, …)
    std::string_view name;     // (Photoshop's column, as the log shows it and the CSV heads it)
};
// The 21 columns in Photoshop's order.
std::span<const Column> columns();
// The column's name of a key (the key itself when it is none of them).
std::string column_name(std::string_view key);
// The sources: selection | ruler | count.
inline constexpr std::array<std::string_view, 3> kSources{"selection", "ruler", "count"};
// What a source can record: the common points and its own.
std::vector<std::string> available(std::string_view source);

// データポイントを選択: what 計測値を記録 keeps of each source's rows (every point it has by default).
struct DataPoints {
    std::vector<std::string> selection = available("selection");
    std::vector<std::string> ruler = available("ruler");
    std::vector<std::string> count = available("count");

    const std::vector<std::string>& of(std::string_view source) const;
    std::vector<std::string>& of(std::string_view source);
    bool operator==(const DataPoints&) const = default;
};
// PhotoCraft's selectDataPoints over `now`: {selection?, ruler?, count?: [keys] (the whole list) | {key: bool} (changes),
// reset?: bool}; each list kept in the columns' order. core::OpError says what is wrong (a key the source cannot record…).
DataPoints choose_data_points(const DataPoints& now, const core::Json& given);
core::Json to_json(const DataPoints& points);
// Read back (the machine's setting): what it cannot read is the default.
DataPoints data_points_from_json(const core::Json& value);

// --- 計測値を記録 --------------------------------------------------------------------------------------------------------

struct Recorded {
    std::string source;         // selection | ruler | count
    int grid_dpi = 0;           // (selection: the grid it was counted on)
    std::vector<core::Json> rows;  // each {key: value} of every point the source has, in the columns' order
    std::vector<std::string> notes;
    std::size_t features_left_out = 0;  // (the features past kFeatureRowsMax: no row of their own, in the summary all the same)
};
// The rows 計測値を記録 adds for the page: `source` auto (the selection — `area` given —, else the ruler line, else the
// count marks, else the whole page as a selection) | selection | ruler | count; `area` the selection (an op's plain area;
// none: the whole page); `number` the log's measurement number (its label 計測 n); `now` its date and time.
// core::OpError: no ruler line, an empty selection, a selection too large to count.
Recorded record(const core::Document& doc, const core::Page& page, std::string_view source, const std::optional<core::Json>& area,
                std::size_t number, const std::string& now);
// The date and time now, as the log writes it (ISO 8601 with the machine's offset: 2026-11-03T21:05:00+09:00).
std::string now_text();
// The rows kept to the data points of their source (`source`).
core::Json kept(const core::Json& row, const std::vector<std::string>& points);

// 目盛りマーカーを配置's length when none is given: a "nice" one (1, 2 or 5 × 10ⁿ) near a fifth of the trim's width, in the
// page's measurement scale's units (PhotoCraft's nice_length over the image's width).
double nice_marker_length(const core::Page& page);

// --- the log's CSV -------------------------------------------------------------------------------------------------------

// The columns any row has, in Photoshop's order.
std::vector<std::string> log_columns(std::span<const core::Json> rows);
// One field: text as it is (but one starting with = + - @, a tab or CR: a ' before it, never a spreadsheet's formula), a
// list (the histogram) its items with spaces, numbers as JSON writes them, none empty; quoted when it holds a comma, a
// quote or a line break.
std::string csv_field(const core::Json& value);
// The rows as CSV: a head of the columns' names, a line for each row (UTF-8, "\n").
std::string csv(std::span<const core::Json> rows);

}  // namespace genko::render::measure
