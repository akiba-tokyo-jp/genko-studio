// PC5 J-d2 (SPEC.md PROD-01「計測」): 計測値を記録's values as PhotoCraft v0.2.0's own tests check them —
// crates/engine/src/analysis_cmds/tests.rs (features_area_perimeter_circularity, record_selection_rows_respect_scale_and_data_points,
// ruler_protractor_and_record, count_groups_markers_and_undo, log_delete_and_csv_export, place_scale_marker_builds_one_group_in_one_step)
// — and more: the square, the disc, the slanted line, the 8 directions, a hole; the gray values in the 8-, 16- and 32-bit
// ranges; a selection counted on the page's grid and given in the page's pixels; the data points; the CSV; the question
// measure_values; 目盛りマーカーを配置 as one op (a group of a pen layer's rectangle and a type layer), refused and undone.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/measure.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "render/image.hpp"
#include "render/measure_features.hpp"
#include "render/ops_registry.hpp"
#include "render/selection.hpp"

using namespace genko;
using core::Json;
namespace m = render::measure;

namespace {

constexpr double kHalf = std::numbers::sqrt2 / 2.0;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor(actor));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
Json report(const core::Document& doc, const Json& op) {
    const auto applied = run_ops(doc, Json::array({op}));
    return applied.results.empty() ? Json() : applied.results[0];
}
std::string refusal(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    try {
        run_ops(doc, Json::array({op}), actor);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}
Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// A `w` × `h` pixel page at `dpi` (white paper, no layers or panels).
core::Document book(int w = 64, int h = 48, int dpi = 200) {
    const double wm = w * 25.4 / dpi, hm = h * 25.4 / dpi;
    auto doc = core::new_episode("計測", core::Num(1), 1, core::PageSpec::custom(wm, hm, wm, hm, 0, 0, 0, 0, 0, dpi, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// A rectangle of the page's pixels at `dpi` as an area (mm).
Json rect_px(double x, double y, double w, double h, int dpi = 200) {
    const double k = 25.4 / dpi;
    return Json{{"rect", Json::array({x * k, y * k, w * k, h * k})}};
}

std::vector<std::uint8_t> mask_of(std::size_t w, std::size_t h, const std::function<bool(std::size_t, std::size_t)>& in) {
    std::vector<std::uint8_t> out(w * h, 0);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) out[y * w + x] = in(x, y) ? 1 : 0;
    return out;
}

double perimeter_of_square(std::size_t n) {
    const auto f = m::measure_features(mask_of(n + 4, n + 4, [&](std::size_t x, std::size_t y) { return x >= 2 && y >= 2 && x < n + 2 && y < n + 2; }),
                                       std::vector<float>((n + 4) * (n + 4), 0.5f), n + 4, n + 4, {0, 0}, 1.0);
    return f.at(0).perimeter;
}

// The pixels the page's selection grid counts in an area (as record counts them: coverage ≥ 50 %).
std::size_t counted(const core::Document& doc, const Json& area) {
    const render::Image mask = render::selection::to_mask(area, doc.page(0), &doc, render::selection::grid_dpi(doc.page(0)));
    std::size_t n = 0;
    for (const char v : mask.tobytes()) n += static_cast<unsigned char>(v) >= 128 ? 1 : 0;
    return n;
}

}  // namespace

class TestMeasureFeatures : public QObject {
    Q_OBJECT

private slots:
    // analysis_cmds/tests.rs features_area_perimeter_circularity: a 10 × 10 square and a slanted line of 5 pixels (one
    // 8-connected feature); a disc.
    void theSquareTheLineAndTheDisc() {
        const std::size_t w = 30, h = 20;
        std::vector<std::uint8_t> mask(w * h, 0);
        for (std::size_t y = 2; y < 12; ++y)
            for (std::size_t x = 2; x < 12; ++x) mask[y * w + x] = 1;
        for (std::size_t i = 0; i < 5; ++i) mask[(5 + i) * w + 20 + i] = 1;
        std::vector<float> gray(w * h);
        for (std::size_t i = 0; i < gray.size(); ++i) gray[i] = i % 2 == 0 ? 0.0f : 1.0f;
        const auto f = m::measure_features(mask, gray, w, h, {100, 200}, 255.0);
        QCOMPARE(f.size(), std::size_t(2));
        QCOMPARE(f[0].area, 100.0);
        QCOMPARE(f[0].x0, std::int64_t(102));
        QCOMPARE(f[0].y0, std::int64_t(202));
        QCOMPARE(f[0].x1, std::int64_t(112));
        QCOMPARE(f[0].y1, std::int64_t(212));
        // (straight edges exact, the corners cut by √½: 4·9 + 4·√½)
        QVERIFY2(std::fabs(f[0].perimeter - (36.0 + 4.0 * kHalf)) < 1e-9, std::to_string(f[0].perimeter).c_str());
        QVERIFY(f[0].circularity() > 0.75 && f[0].circularity() < 0.9);
        QCOMPARE(f[0].gray_min, 0.0);
        QCOMPARE(f[0].gray_max, 255.0);
        QVERIFY(std::fabs(f[0].gray_mean - 127.5) < 3.0);
        std::uint64_t sum = 0;
        for (const std::uint64_t v : f[0].histogram) sum += v;
        QCOMPARE(sum, std::uint64_t(100));
        // the slanted line: 5 pixels, its steps √2 each way (8 · √½ … as marching squares makes it)
        QCOMPARE(f[1].area, 5.0);
        QCOMPARE(f[1].width(), std::int64_t(5));
        QCOMPARE(f[1].height(), std::int64_t(5));
        QVERIFY(f[1].perimeter > 0);
        // a disc's circularity nears 1 (a digital disc reads ≈0.89, as ImageJ's)
        const std::size_t d = 101;
        const auto disc = mask_of(d, d, [](std::size_t x, std::size_t y) { return std::hypot(double(x) - 50.0, double(y) - 50.0) <= 40.0; });
        const auto g = m::measure_features(disc, std::vector<float>(d * d, 0.5f), d, d, {0, 0}, 1.0);
        QCOMPARE(g.size(), std::size_t(1));
        QVERIFY2(g[0].circularity() > 0.85, std::to_string(g[0].circularity()).c_str());
        QVERIFY(std::fabs(g[0].perimeter / (2.0 * std::numbers::pi * 40.0) - 1.0) < 0.08);
        QVERIFY(std::fabs(g[0].gray_median - 0.5) < 1e-4);
        // the circularity is 1 at most (a single pixel: area 1, its contour 4·√½)
        const auto one = m::measure_features(mask_of(3, 3, [](std::size_t x, std::size_t y) { return x == 1 && y == 1; }), std::vector<float>(9, 0.0f), 3,
                                             3, {0, 0}, 255.0);
        QCOMPARE(one.at(0).perimeter, 4.0 * kHalf);
        QVERIFY(one.at(0).circularity() <= 1.0);
    }

    // 8 directions: pixels touching at a corner are one feature, a gap two (in the order of their first pixel); a hole: the
    // ring's area without it, its contour inside too.
    void eightDirectionsAndAHole() {
        const auto corner = m::measure_features(mask_of(4, 4, [](std::size_t x, std::size_t y) { return (x == 0 && y == 0) || (x == 1 && y == 1) || (x == 2 && y == 2); }),
                                                std::vector<float>(16, 0.0f), 4, 4, {0, 0}, 255.0);
        QCOMPARE(corner.size(), std::size_t(1));
        QCOMPARE(corner[0].area, 3.0);
        const auto apart = m::measure_features(mask_of(5, 3, [](std::size_t x, std::size_t y) { return y == 1 && (x == 0 || x == 2 || x == 4); }),
                                               std::vector<float>(15, 0.0f), 5, 3, {0, 0}, 255.0);
        QCOMPARE(apart.size(), std::size_t(3));
        QCOMPARE(apart[2].x0, std::int64_t(4));
        // (a U: its arms meet below, one feature though the first row sees two)
        const auto u = m::measure_features(mask_of(5, 4, [](std::size_t x, std::size_t y) { return x == 0 || x == 4 || y == 3; }), std::vector<float>(20, 0.0f), 5,
                                           4, {0, 0}, 255.0);
        QCOMPARE(u.size(), std::size_t(1));
        QCOMPARE(u[0].area, 11.0);
        // a ring: 8 × 8 with a 4 × 4 hole
        const auto ring = m::measure_features(
            mask_of(12, 12, [](std::size_t x, std::size_t y) { return x >= 2 && y >= 2 && x < 10 && y < 10 && !(x >= 4 && y >= 4 && x < 8 && y < 8); }),
            std::vector<float>(144, 0.25f), 12, 12, {0, 0}, 255.0);
        QCOMPARE(ring.size(), std::size_t(1));
        QCOMPARE(ring[0].area, 48.0);
        QVERIFY(std::fabs(perimeter_of_square(8) - (28.0 + 4.0 * kHalf)) < 1e-9);
        QVERIFY2(std::fabs(ring[0].perimeter - (perimeter_of_square(8) + perimeter_of_square(4))) < 1e-9, std::to_string(ring[0].perimeter).c_str());
        QCOMPARE(ring[0].gray_mean, 0.25 * 255.0);
    }

    // The gray values: the range (what 1 reads as), the median (of an even count the two middle ones' mean), the summary
    // (the area-weighted mean, the median of every value).
    void grayValuesAndTheSummary() {
        std::vector<std::uint16_t> odd{3, 1, 2};
        QCOMPARE(m::median(odd), 2.0 / 65535.0);
        std::vector<std::uint16_t> even{4, 1, 3, 2};
        QCOMPARE(m::median(even), 2.5 / 65535.0);
        std::vector<std::uint16_t> none;
        QCOMPARE(m::median(none), 0.0);
        const std::size_t w = 6, h = 1;
        const auto mask = mask_of(w, h, [](std::size_t x, std::size_t) { return x != 2; });
        const std::vector<float> gray{0.0f, 0.5f, 0.9f, 1.0f, 1.0f, 1.0f};
        for (const double range : {255.0, 32768.0, 1.0}) {
            const auto f = m::measure_features(mask, gray, w, h, {0, 0}, range);
            QCOMPARE(f.size(), std::size_t(2));
            QCOMPARE(f[0].gray_max, 0.5 * range);
            QCOMPARE(f[1].gray_min, 1.0 * range);
            const m::Feature all = m::summary(f, 0.75 * range);
            QCOMPARE(all.area, 5.0);
            QVERIFY(std::fabs(all.gray_mean - (0.25 * 2 + 1.0 * 3) / 5.0 * range) < 1e-9 * range);
            QCOMPARE(all.gray_min, 0.0);
            QCOMPARE(all.gray_max, range);
            QCOMPARE(all.x0, std::int64_t(0));
            QCOMPARE(all.x1, std::int64_t(6));
        }
        // a 32-bit value past 1: the mean keeps it, the histogram and the median take it as 1
        const auto bright = m::measure_features(mask_of(2, 1, [](std::size_t, std::size_t) { return true; }), std::vector<float>{2.0f, 1.0f}, 2, 1, {0, 0}, 1.0);
        QCOMPARE(bright[0].gray_mean, 1.5);
        QCOMPARE(bright[0].gray_max, 2.0);
        QCOMPARE(bright[0].histogram[255], std::uint64_t(2));
        QCOMPARE(bright[0].gray_median, 1.0);
    }

    // record_selection_rows_respect_scale_and_data_points on the 8-, 16- and 32-bit pages: two rectangles (a summary and a
    // row for each), the scale's lengths, the white paper's gray in the depth's range; no selection: the whole page.
    void recordOnTheThreeDepths() {
        for (const int depth : {8, 16, 32}) {
            core::Document doc = book();
            if (depth != 8) doc = run(doc, op("set_color_mode", {{"mode", "rgb"}, {"depth", depth}}));
            doc = run(doc, op("set_measurement_scale", {{"pixel_length", 2}, {"logical_length", 1}, {"units", "mm"}}));
            QCOMPARE(render::selection::grid_dpi(doc.page(0)), 200);
            const Json one = rect_px(4, 4, 10, 10), two = rect_px(30, 20, 20, 5);
            const Json area{{"union", Json::array({one, two})}};
            const m::Recorded made = m::record(doc, doc.page(0), "auto", area, 7, "2026-10-01T00:00:00+09:00");
            QCOMPARE(made.source, std::string("selection"));
            QCOMPARE(made.grid_dpi, 200);
            QCOMPARE(made.rows.size(), std::size_t(3));
            const Json& sum = made.rows[0];
            QCOMPARE(sum["label"], Json("計測 7"));
            QCOMPARE(sum["source"], Json("選択範囲"));
            QCOMPARE(sum["count"], Json(2));
            QCOMPARE(sum["scaleUnits"], Json("mm"));
            QCOMPARE(sum["scaleFactor"], Json(0.5));
            QCOMPARE(sum["scale"], Json("2 pixels = 1.0000 mm"));
            QCOMPARE(sum["dateTime"], Json("2026-10-01T00:00:00+09:00"));
            QCOMPARE(sum["document"], Json("計測（1 ページ）"));
            const double a = static_cast<double>(counted(doc, one)), b = static_cast<double>(counted(doc, two));
            QCOMPARE(sum["area"].get<double>(), (a + b) * 0.25);
            const double white = depth == 8 ? 255.0 : depth == 16 ? 32768.0 : 1.0;
            QVERIFY2(std::fabs(sum["grayMean"].get<double>() - white) < white * 0.01, (std::to_string(depth) + ": " + sum.dump()).c_str());
            QCOMPARE(sum["integratedDensity"].get<double>(), core::py_round((a + b) * sum["grayMean"].get<double>(), 4));
            QCOMPARE(made.rows[1]["label"], Json("フィーチャ 1"));
            QCOMPARE(made.rows[1]["area"].get<double>(), a * 0.25);
            QCOMPARE(made.rows[2]["area"].get<double>(), b * 0.25);
            QVERIFY(made.rows[2]["width"].get<double>() > made.rows[2]["height"].get<double>());
            QCOMPARE(made.rows[2]["count"], Json(1));
            // no selection: the whole page, one feature (no feature rows)
            const m::Recorded whole = m::record(doc, doc.page(0), "selection", std::nullopt, 8, "t");
            QCOMPARE(whole.rows.size(), std::size_t(1));
            QCOMPARE(whole.rows[0]["area"].get<double>(), 64.0 * 48.0 * 0.25);
            QCOMPARE(whole.rows[0]["width"].get<double>(), 32.0);
            QVERIFY(std::fabs(whole.rows[0]["grayMedian"].get<double>() - white) < white * 0.01);
        }
        // a page of 400 dpi: its 8-bit selection counted on the 200 dpi grid, given in the page's pixels (×2 a side)
        const core::Document fine = book(128, 96, 400);
        const m::Recorded made = m::record(fine, fine.page(0), "selection", std::nullopt, 1, "t");
        QCOMPARE(made.grid_dpi, 200);
        QCOMPARE(made.rows[0]["area"].get<double>(), 128.0 * 96.0);
        QCOMPARE(made.rows[0]["width"].get<double>(), 128.0);
        // a selection that is empty, a page too large to count: refused
        bool refused = false;
        try {
            (void)m::record(fine, fine.page(0), "selection", Json{{"rect", Json::array({-50, -50, 1, 1})}}, 1, "t");
        } catch (const core::OpError& error) {
            refused = std::string(error.what()) == "the selection is empty";
        }
        QVERIFY(refused);
    }

    // ruler_protractor_and_record, count_groups_markers_and_undo: the ruler's row (its length by the scale, its angle) and
    // the count's; auto picks the ruler, then the count; a ruler asked of a page without one refused.
    void theRulerAndTheCount() {
        core::Document doc = book(100, 100, 254);  // (10 pixels a mm)
        bool refused = false;
        try {
            (void)m::record(doc, doc.page(0), "ruler", std::nullopt, 1, "t");
        } catch (const core::OpError& error) {
            refused = std::string(error.what()).starts_with("the page has no ruler line");
        }
        QVERIFY(refused);
        doc = run(doc, op("set_ruler_line", {{"start", {1, 5}}, {"end", {4, 1}}}));
        doc = run(doc, op("set_measurement_scale", {{"pixel_length", 10}, {"logical_length", 1}, {"units", "cm"}}));
        const m::Recorded ruler = m::record(doc, doc.page(0), "auto", std::nullopt, 1, "t");
        QCOMPARE(ruler.source, std::string("ruler"));
        QCOMPARE(ruler.rows.size(), std::size_t(1));
        QCOMPARE(ruler.rows[0]["length"], Json(5.0));
        QVERIFY(std::fabs(ruler.rows[0]["angle"].get<double>() - 53.1301) < 1e-3);
        QCOMPARE(ruler.rows[0]["source"], Json("ものさしツール"));
        QVERIFY(!ruler.rows[0].contains("area"));
        doc = run(doc, op("set_ruler_line", {{"clear", true}}));
        doc = run(doc, op("add_count", {{"at_mm", {1, 1}}}));
        doc = run(doc, op("add_count", {{"at_mm", {2, 2}}}));
        doc = run(doc, op("new_count_group"));
        doc = run(doc, op("add_count", {{"at_mm", {3, 3}}}));
        const m::Recorded count = m::record(doc, doc.page(0), "auto", std::nullopt, 2, "t");
        QCOMPARE(count.source, std::string("count"));
        QCOMPARE(count.rows[0]["count"], Json(3));
        QCOMPARE(count.rows[0]["source"], Json("カウントツール"));
        // the question: the same rows, the book as it was (nothing saved, journalled or in the history)
        const auto asked = run_ops(doc, Json::array({op("measure_values", {{"source", "count"}})}));
        QVERIFY(asked.unchanged);
        QCOMPARE(asked.results[0]["rows"][0]["count"], Json(3));
        QCOMPARE(asked.results[0]["columns"][0], (Json{{"key", "label"}, {"name", "ラベル"}}));
        const Json selection = report(doc, op("measure_values", {{"source", "selection"}, {"area", rect_px(0, 0, 10, 10, 254)}}));
        QCOMPARE(selection["source"], Json("selection"));
        QCOMPARE(selection["grid_dpi"], Json(200));
        QVERIFY(selection["rows"][0]["area"].get<double>() > 0);
        QVERIFY(refusal(doc, op("measure_values", {{"source", "pen"}})).find("source is auto, selection, ruler or count") != std::string::npos);
        QVERIFY(refusal(doc, op("measure_values", {{"source", "ruler"}, {"area", rect_px(0, 0, 10, 10)}})).find("only for source selection") != std::string::npos);
    }

    // selectDataPoints and log_delete_and_csv_export: the points chosen (a whole list, changes, refused keys, reset), the rows
    // kept to them, the columns any row has, the CSV (Photoshop's columns' names, a quoted field, the histogram's numbers).
    void dataPointsAndTheCsv() {
        const m::DataPoints all;
        QCOMPARE(all.selection.size(), std::size_t(19));
        QCOMPARE(all.ruler, m::available("ruler"));
        QCOMPARE(all.count.back(), std::string("count"));
        m::DataPoints chosen = m::choose_data_points(all, Json{{"selection", {"area", "label"}}});
        QCOMPARE(chosen.selection, (std::vector<std::string>{"label", "area"}));
        chosen = m::choose_data_points(chosen, Json{{"selection", {{"perimeter", true}, {"label", false}}}});
        QCOMPARE(chosen.selection, (std::vector<std::string>{"area", "perimeter"}));
        QCOMPARE(chosen.ruler, all.ruler);
        for (const Json& wrong : {Json{{"ruler", {"area"}}}, Json{{"pen", {"label"}}}, Json{{"count", {{"count", 1}}}}, Json{{"selection", "area"}}}) {
            bool refused = false;
            try {
                (void)m::choose_data_points(chosen, wrong);
            } catch (const core::OpError&) {
                refused = true;
            }
            QVERIFY2(refused, wrong.dump().c_str());
        }
        QCOMPARE(m::choose_data_points(chosen, Json{{"reset", true}}), all);
        QCOMPARE(m::data_points_from_json(m::to_json(chosen)), chosen);
        QCOMPARE(m::data_points_from_json(Json("nonsense")), all);
        // kept rows and the CSV
        const Json row{{"label", "計測 1"}, {"dateTime", "t"}, {"document", "a,\"b\""}, {"area", 100.0}, {"histogram", {1, 2, 3}}};
        const Json kept = m::kept(row, chosen.selection);
        QCOMPARE(kept, (Json{{"area", 100.0}}));
        const Json ruler{{"label", "計測 2"}, {"length", 5.0}};
        const std::vector<Json> rows{row, ruler};
        QCOMPARE(m::log_columns(rows), (std::vector<std::string>{"label", "dateTime", "document", "area", "histogram", "length"}));
        QCOMPARE(m::csv_field(Json("a,\"b\"")), std::string("\"a,\"\"b\"\"\""));
        QCOMPARE(m::csv_field(Json::array({1, 2, 3})), std::string("1 2 3"));
        QCOMPARE(m::csv_field(Json(nullptr)), std::string());
        const std::string text = m::csv(rows);
        QCOMPARE(text, std::string("ラベル,日時,ドキュメント,面積,ヒストグラム,長さ\n計測 1,t,\"a,\"\"b\"\"\",100.0,1 2 3,\n計測 2,,,,,5.0\n"));
        QCOMPARE(m::column_name("grayMean"), std::string("グレー値（平均）"));
        QCOMPARE(m::columns().size(), std::size_t(21));
        // (2026-11-03T21:05:00+09:00; on a machine at UTC 2026-11-03T12:05:00Z)
        const std::string now = m::now_text();
        QVERIFY2(now.size() >= 20 && now[4] == '-' && now[10] == 'T' && (now.back() == 'Z' || now[now.size() - 3] == ':'), now.c_str());
    }

    // place_scale_marker_builds_one_group_in_one_step: a group 目盛りマーカー of a pen layer's rectangle and a type layer, one
    // op (one step of the history); the nice length; no text; refused (too long, a colour not black or white, a page held
    // by an agent); on a 16-bit page too.
    void theScaleMarker() {
        core::Document doc = book(400, 300, 254);  // (40 × 30 mm, 10 pixels a mm)
        doc = run(doc, op("set_measurement_scale", {{"pixel_length", 50}, {"logical_length", 1}, {"units", "cm"}}));
        QCOMPARE(m::nice_marker_length(doc.page(0)), 1.0);  // (a fifth of 400 pixels = 1.6 cm → 1 cm)
        const auto applied = run_ops(doc, Json::array({op("place_scale_marker", {{"color", "black"}})}));
        const Json r = applied.results[0];
        QCOMPARE(r["label"], Json("1 cm"));
        QCOMPARE(r["pixels"], Json(50.0));
        const core::Page& page = applied.doc.page(0);
        QCOMPARE(page.layers.size(), std::size_t(3));
        const core::Layer& group = page.layers.back();
        QCOMPARE(group.kind, core::LayerKind::Folder);
        QVERIFY(group.group);
        QCOMPARE(group.title, std::string("目盛りマーカー"));
        QCOMPARE(group.id, r["group"].get<std::string>());
        const core::Layer& bar = page.layers[0];
        QCOMPARE(bar.id, r["bar"].get<std::string>());
        QCOMPARE(bar.title, std::string("目盛り"));
        QCOMPARE(bar.kind, core::LayerKind::Strokes);
        QCOMPARE(bar.parent_id, Json(group.id));
        QVERIFY(bar.shapes && bar.shapes->items.size() == 1);
        QCOMPARE(r["bar_mm"][2], Json(5.0));  // (50 pixels)
        QVERIFY(r["bar_mm"][1].get<double>() + r["bar_mm"][3].get<double>() < 30.0);
        const core::Layer& text = page.layers[1];
        QCOMPARE(text.kind, core::LayerKind::Type);
        QCOMPARE(text.id, r["text"].get<std::string>());
        QCOMPARE(text.parent_id, Json(group.id));
        QCOMPARE(text.title, std::string("1 cm"));
        QCOMPARE(text.type->text, std::string("1 cm"));
        // without the text, white, on top: a bar alone
        const Json r2 = report(applied.doc, op("place_scale_marker", {{"length", 2}, {"text", false}, {"color", "white"}, {"position", "top"}}));
        QCOMPARE(r2["pixels"], Json(100.0));
        QVERIFY(r2["text"].is_null());
        // refused: wider than the trim, a colour, a size
        QVERIFY(refusal(doc, op("place_scale_marker", {{"length", 100}})).find("it must fit the trim's") != std::string::npos);
        QVERIFY(refusal(doc, op("place_scale_marker", {{"color", "red"}})).find("color is black or white") != std::string::npos);
        QVERIFY(refusal(doc, op("place_scale_marker", {{"size_pt", 2000}})).find("size_pt is 1 to 1000") != std::string::npos);
        // a page an agent holds: the person's op refused, the book as it was
        core::Document held = doc;
        held.page_locks[held.pages[0]->id] = "ai:claude";
        QVERIFY(!refusal(held, op("place_scale_marker")).empty());
        // the 16-bit page: the same group
        core::Document deep = run(book(400, 300, 254), op("set_color_mode", {{"mode", "rgb"}, {"depth", 16}}));
        const core::Document marked = run(deep, op("place_scale_marker", {{"length", 100}}));  // (no scale: 100 pixels)
        QCOMPARE(marked.page(0).layers.back().title, std::string("目盛りマーカー"));
        QCOMPARE(marked.page(0).layers.size(), std::size_t(3));
    }
};

QTEST_GUILESS_MAIN(TestMeasureFeatures)
#include "test_measure_features.moc"
