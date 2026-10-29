// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): a page's measuring as PhotoCraft v0.2.0's own tests check it —
// crates/doc/src/analysis.rs (the scale's factor and words) and crates/engine/src/analysis_cmds/tests.rs (the scale set,
// the default and the refusals; the ruler's readout and the protractor; レイヤーの角度補正 of a layer and of the page cut to
// the rectangle inside it; the inscribed rectangle; the count groups and marks; the info) — on Genko's pages: places in mm,
// lengths in the page's pixels (a 254 dpi page: 10 pixels a mm), the turn and the cut the same as transform_layers' and
// crop_page's on the 8-bit, 16-bit and CMYK pages, the marks and the line carried by the page size ops, the question
// measure_info answered without a change, PSD's 1074 and a .pcraft's measurement, MCP's apply_ops not taking the ops.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/measure.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "render/image.hpp"
#include "render/layer_bounds.hpp"
#include "render/ops_measure.hpp"
#include "render/ops_production.hpp"
#include "render/ops_registry.hpp"
#include "render/pcraft_measure.hpp"
#include "render/pcraft_production.hpp"
#include "render/psd_annotations.hpp"
#include "render/raster.hpp"
#include "service/studio_service.hpp"

using namespace genko;
using core::Json;
namespace pa = render::psd_annotations;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor(actor));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
Json report(const core::Document& doc, const Json& op) {
    const auto applied = run_ops(doc, Json::array({op}));
    return applied.results.empty() ? Json() : applied.results[0];
}
std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
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

// A 30 × 20 mm page at 254 dpi (10 pixels a mm) without panels.
core::Document book(double w = 30, double h = 20) {
    auto doc = core::new_episode("計測", core::Num(1), 1, core::PageSpec::custom(w, h, 10, 10, 1, 1, 1, 1, 1, 254, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// A paint layer "a": a bar across the page's middle at the working 200 dpi.
void add_bar(core::Document& doc) {
    auto& p = doc.edit_page(0);
    const int w = static_cast<int>(std::lround(p.spec.width_mm.value() / 25.4 * 200)), h = static_cast<int>(std::lround(p.spec.height_mm.value() / 25.4 * 200));
    std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
    for (int y = h / 2 - 3; y < h / 2 + 3; ++y)
        for (int x = w / 6; x < w * 5 / 6; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(20);
            bytes[i + 3] = static_cast<char>(255);
        }
    core::Layer layer;
    layer.id = "a";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
    p.layers.push_back(std::move(layer));
}

const core::Measure& measure(const core::Document& doc) { return *doc.page(0).measure; }

// The page's layers' pixels and paper the same.
void same_page(const core::Document& a, const core::Document& b) {
    const core::Page& x = a.page(0);
    const core::Page& y = b.page(0);
    QCOMPARE(x.spec.width_mm.value(), y.spec.width_mm.value());
    QCOMPARE(x.spec.height_mm.value(), y.spec.height_mm.value());
    QCOMPARE(x.layers.size(), y.layers.size());
    for (std::size_t i = 0; i < x.layers.size(); ++i) {
        QCOMPARE(bool(x.layers[i].raster_png), bool(y.layers[i].raster_png));
        if (x.layers[i].raster_png) QVERIFY(*x.layers[i].raster_png == *y.layers[i].raster_png);
        QCOMPARE(bool(x.layers[i].color_raster), bool(y.layers[i].color_raster));
        if (x.layers[i].color_raster) QVERIFY(*x.layers[i].color_raster == *y.layers[i].color_raster);
        QCOMPARE(bool(x.layers[i].ink_raster), bool(y.layers[i].ink_raster));
        if (x.layers[i].ink_raster) QVERIFY(*x.layers[i].ink_raster == *y.layers[i].ink_raster);
    }
}

}  // namespace

class TestMeasure : public QObject {
    Q_OBJECT

private slots:
    // doc/src/analysis.rs: scale_factor_and_text; the 初期設定 is none.
    void scaleFactorAndText() {
        const core::MeasurementScale s{100, 2.5, "cm"};
        QVERIFY(std::fabs(core::scale_factor(s) - 0.025) < 1e-12);
        QCOMPARE(core::describe(s), std::string("100 pixels = 2.5000 cm"));
        QVERIFY(core::is_default(core::MeasurementScale{}));
        QCOMPARE(core::scale_factor(std::nullopt), 1.0);
        QCOMPARE(core::describe(core::MeasurementScale{12.5, 1, "mm"}), std::string("12.5 pixels = 1.0000 mm"));
        const Json j = core::scale_json(std::nullopt);
        QCOMPARE(j["default"], Json(true));
        QCOMPARE(j["text"], Json("1 pixels = 1.0000 pixels"));
    }

    // analysis_cmds/tests.rs measurement_scale_set_query_default_and_undo, and the paper's mm (Genko's).
    void scaleSetDefaultAndRefusals() {
        core::Document doc = book();
        const Json r = report(doc, op("set_measurement_scale", {{"pixel_length", 100}, {"logical_length", 2.5}, {"units", "cm"}}));
        QCOMPARE(r["scale"]["factor"], Json(0.025));
        QCOMPARE(r["scale"]["text"], Json("100 pixels = 2.5000 cm"));
        doc = run(doc, op("set_measurement_scale", {{"pixel_length", 100}, {"logical_length", 2.5}, {"units", "cm"}}));
        QCOMPARE(measure(doc).scale->units, std::string("cm"));
        QVERIFY(core::uses_measure(doc));
        // (the lengths not given are the page's)
        doc = run(doc, op("set_measurement_scale", {{"units", "mm"}}));
        QCOMPARE(measure(doc).scale->pixel_length, 100.0);
        QCOMPARE(measure(doc).scale->units, std::string("mm"));
        QVERIFY(refusal(doc, op("set_measurement_scale", {{"pixel_length", 0}})).find("pixel_length must be a number greater than 0") != std::string::npos);
        QVERIFY(refusal(doc, op("set_measurement_scale", {{"units", " "}})).find("units must not be empty") != std::string::npos);
        QVERIFY(refusal(doc, op("set_measurement_scale", {{"preset", "bogus"}})).find("preset is custom, default or paper_mm") != std::string::npos);
        QVERIFY(refusal(doc, op("set_measurement_scale", {{"preset", "default"}, {"units", "cm"}})).find("takes no lengths") != std::string::npos);
        QVERIFY(refusal(doc, op("set_measurement_scale")).find("give pixel_length") != std::string::npos);
        // 初期設定: none kept, nothing written (no measure@1)
        doc = run(doc, op("set_measurement_scale", {{"preset", "default"}}));
        QVERIFY(!doc.page(0).measure);
        QVERIFY(!core::uses_measure(doc));
        // the paper's mm: the page's pixels a mm (254 dpi: 10) = 1 mm
        doc = run(doc, op("set_measurement_scale", {{"preset", "paper_mm"}}));
        QCOMPARE(measure(doc).scale->pixel_length, 10.0);
        QCOMPARE(measure(doc).scale->logical_length, 1.0);
        QCOMPARE(measure(doc).scale->units, std::string("mm"));
        // (1 pixels = 1 pixels given by hand is the default too)
        doc = run(doc, op("set_measurement_scale", {{"pixel_length", 1}, {"logical_length", 1}, {"units", "pixels"}}));
        QVERIFY(!doc.page(0).measure);
    }

    // analysis_cmds/tests.rs ruler_protractor_and_record (the readout), in the page's pixels (10 a mm) and mm.
    void rulerReadoutAndProtractor() {
        core::RulerLine line;
        line.start = {1, 5};
        line.end = {4, 1};
        core::RulerReadout r = core::ruler_readout(line, std::nullopt, 10);
        QCOMPARE(r.l1, 50.0);
        QCOMPARE(r.w, 30.0);
        QCOMPARE(r.h, -40.0);
        QCOMPARE(r.x, 10.0);
        QCOMPARE(r.y, 50.0);
        QCOMPARE(r.l1_mm, 5.0);
        QVERIFY(std::fabs(r.angle - 53.1301) < 1e-3);
        QVERIFY(!r.protractor && !r.l2);
        line.protractor = std::array<double, 2>{1, 9};  // (a second arm straight down from the vertex)
        r = core::ruler_readout(line, std::nullopt, 10);
        QVERIFY(std::fabs(r.angle - (180.0 - 36.8699)) < 1e-3);
        QCOMPARE(*r.l2, 40.0);
        QCOMPARE(*r.l2_mm, 4.0);
        // the length by the scale: 10 pixels = 1 cm
        r = core::ruler_readout(line, core::MeasurementScale{10, 1, "cm"}, 10);
        QCOMPARE(r.length, 5.0);
        QCOMPARE(r.units, std::string("cm"));
        const Json j = core::to_json(r);
        QCOMPARE(j["protractor"], Json(true));
        QCOMPARE(j["l2"], Json(40.0));
    }

    // analysis_cmds/tests.rs ruler_protractor_and_record (the op): set, the protractor alone, the refusals, cleared.
    void rulerLineOps() {
        core::Document doc = book();
        Json r = report(doc, op("set_ruler_line", {{"start", {1, 5}}, {"end", {4, 1}}}));
        QCOMPARE(r["info"]["l1"], Json(50.0));
        QCOMPARE(r["info"]["w"], Json(30.0));
        doc = run(doc, op("set_ruler_line", {{"start", {1, 5}}, {"end", {4, 1}}}));
        QVERIFY(measure(doc).ruler && !measure(doc).ruler->protractor);
        r = report(doc, op("set_ruler_line", {{"protractor", {1, 9}}}));
        QVERIFY(std::fabs(r["info"]["angle"].get<double>() - (180.0 - 36.8699)) < 1e-3);
        QCOMPARE(r["info"]["l2"], Json(40.0));
        doc = run(doc, op("set_ruler_line", {{"protractor", {1, 9}}}));
        QVERIFY(measure(doc).ruler->protractor);
        doc = run(doc, op("set_ruler_line", {{"protractor", nullptr}}));
        QVERIFY(!measure(doc).ruler->protractor);
        QVERIFY(refusal(doc, op("set_ruler_line", {{"start", {1, 1}}})).find("give both start and end") != std::string::npos);
        QVERIFY(refusal(doc, op("set_ruler_line", {{"start", {1, 1}}, {"end", {"a", 2}}})).find("end is a place [x, y] (mm) on the page") != std::string::npos);
        QVERIFY(refusal(doc, op("set_ruler_line")).find("give start and end") != std::string::npos);
        doc = run(doc, op("set_ruler_line", {{"clear", true}}));
        QVERIFY(!doc.page(0).measure);  // (nothing else: none kept)
        QVERIFY(refusal(doc, op("set_ruler_line", {{"protractor", {1, 1}}})).find("the page has no ruler line") != std::string::npos);
        QVERIFY(refusal(doc, op("straighten_layer", {{"crop", true}})).find("the page has no ruler line") != std::string::npos);
    }

    // analysis_cmds/tests.rs inscribed_rect.
    void inscribedRect() {
        std::array<double, 2> r = core::inscribed(100, 50, 0);
        QVERIFY(std::fabs(r[0] - 100) < 1e-9 && std::fabs(r[1] - 50) < 1e-9);
        r = core::inscribed(100, 100, std::numbers::pi / 4);
        QVERIFY(std::fabs(r[0] - 70.710678) < 1e-4 && std::fabs(r[1] - 70.710678) < 1e-4);
        // (a long thin one: its short side across)
        r = core::inscribed(100, 10, 0.3);
        QVERIFY(r[0] > 0 && r[1] > 0 && r[0] < 100 && r[1] < 10);
        // the turn that levels a line, either way, and lays a steep one upright
        core::RulerLine line;
        line.start = {1, 3};
        line.end = {7, 2.5};
        QVERIFY(std::fabs(core::straighten_turn(line) - 4.7636) < 1e-3);
        line.end = {7, 3.5};
        QVERIFY(std::fabs(core::straighten_turn(line) + 4.7636) < 1e-3);
        line.end = {1.5, 9};
        QVERIFY(std::fabs(std::fabs(core::straighten_turn(line)) - 4.7636) < 1e-3);
    }

    // analysis_cmds/tests.rs straighten_layer_and_image: a layer turned about the page's middle by transform_layers' own
    // turn (the paper as it was), the line cleared; the page turned and cut to the rectangle inside it — crop_page's own —
    // on the 8-bit, 16-bit and CMYK pages.
    void straightenLayerAndPage() {
        for (const char* mode : {"", "rgb16", "cmyk"}) {
            core::Document doc = book();
            add_bar(doc);
            if (std::string(mode) == "rgb16") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
            if (std::string(mode) == "cmyk") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"depth", 16}, {"profile", "coated-cmyk"}});
            doc = run(doc, op("set_ruler_line", {{"start", {1, 3}}, {"end", {7, 2.5}}}));
            const core::Document before = doc;
            // the layer
            const Json r = report(doc, op("straighten_layer", {{"ids", {"a"}}}));
            QCOMPARE(r["cropped"], Json(false));
            QVERIFY2(std::fabs(r["angle"].get<double>() - 4.7636) < 1e-3, r.dump().c_str());
            const core::Document turned = run(doc, op("straighten_layer", {{"ids", {"a"}}}));
            QCOMPARE(turned.page(0).spec.width_mm.value(), 30.0);
            QVERIFY(!turned.page(0).measure);  // (the line cleared, nothing else: none kept)
            const double turn = core::straighten_turn(*before.page(0).measure->ruler) * std::numbers::pi / 180;
            const double cs = std::cos(turn), sn = std::sin(turn);
            const core::Document by_hand = run(before, op("transform_layers", {{"ids", {"a"}},
                                                                             {"matrix", {cs, sn, -sn, cs, 15 - (cs * 15 - sn * 10), 10 - (sn * 15 + cs * 10)}},
                                                                             {"box", {0, 0, 30, 20}}}));
            same_page(turned, by_hand);
            const auto box_before = render::layer_bounds::of_layer(before, before.page(0), "a");
            const auto box_after = render::layer_bounds::of_layer(turned, turned.page(0), "a");
            QVERIFY(box_before && box_after);
            QVERIFY2((*box_after)[3] > (*box_before)[3] + 0.5, mode);  // (a level bar turned: taller)
            // the page turned and cut
            const Json rc = report(before, op("straighten_layer", {{"crop", true}}));
            QCOMPARE(rc["cropped"], Json(true));
            const core::Document cut = run(before, op("straighten_layer", {{"crop", true}}));
            const auto [iw, ih] = core::inscribed(30, 20, turn);
            QVERIFY2(std::fabs(cut.page(0).spec.width_mm.value() - iw) < 2e-3, mode);
            QVERIFY(std::fabs(cut.page(0).spec.height_mm.value() - ih) < 2e-3);
            QVERIFY(cut.page(0).spec.width_mm.value() < 30 && cut.page(0).spec.height_mm.value() < 20);
            const core::Document cropped = run(before, op("crop_page", {{"rect", {core::py_round((30 - iw) / 2, 4), core::py_round((20 - ih) / 2, 4),
                                                                                  core::py_round(iw, 4), core::py_round(ih, 4)}},
                                                                         {"angle", core::py_round(-turn * 180 / std::numbers::pi, 6)}}));
            same_page(cut, cropped);
            QVERIFY(!cut.page(0).measure);
        }
        // 45°: the page's paper smaller both ways
        core::Document doc = book(30, 30);
        doc = run(doc, op("set_ruler_line", {{"start", {0, 0}}, {"end", {4, 4}}}));
        const Json r = report(doc, op("straighten_layer", {{"crop", true}}));
        QCOMPARE(std::fabs(r["angle"].get<double>()), 45.0);
        QVERIFY(r["paper_mm"][0].get<double>() < 30 && r["paper_mm"][1].get<double>() < 30);
        // a level line: nothing turned, the line kept
        doc = run(doc, op("set_ruler_line", {{"start", {1, 1}}, {"end", {5, 1}}}));
        const auto applied = run_ops(doc, Json::array({op("straighten_layer", {{"crop", true}})}));
        QCOMPARE(applied.results[0]["angle"], Json(0.0));
        QVERIFY(applied.doc.page(0).measure->ruler);
        QCOMPARE(applied.doc.page(0).spec.width_mm.value(), 30.0);
        // the refusals
        QVERIFY(refusal(doc, op("straighten_layer", {{"ids", {"a"}}, {"crop", true}})).find("not both") != std::string::npos);
        QVERIFY(refusal(doc, op("straighten_layer", {{"crop", false}})).find("give ids") != std::string::npos);
        QVERIFY(refusal(doc, op("straighten_layer", {{"ids", {"a"}}, {"trim", "fit"}})).find("trim is only for crop: true") != std::string::npos);
    }

    // analysis_cmds/tests.rs count_groups_markers_and_undo (in mm: 10 pixels a mm, the default reach 6 pixels).
    void countGroupsAndMarks() {
        core::Document doc = book();
        Json r = report(doc, op("add_count", {{"at_mm", {1, 1}}}));
        QCOMPARE(r["number"], Json(1));
        QCOMPARE(r["count"]["total"], Json(1));
        doc = run(doc, op("add_count", {{"at_mm", {1, 1}}}));
        QCOMPARE(measure(doc).count_groups.size(), std::size_t(1));
        QCOMPARE(measure(doc).count_groups[0].name, std::string("カウントグループ 1"));
        QCOMPARE(measure(doc).count_groups[0].color, (std::array<int, 3>{255, 0, 0}));
        doc = run(doc, op("add_count", {{"at_mm", {2, 2}}}));
        doc = run(doc, op("new_count_group", {{"name", "Cells"}, {"color", "#00ff00"}}));
        QCOMPARE(measure(doc).active_group, std::size_t(1));
        r = report(doc, op("add_count", {{"at_mm", {5, 5}}}));
        QCOMPARE(r["count"]["groups"][1]["count"], Json(1));
        QCOMPARE(r["group"], Json(1));
        doc = run(doc, op("add_count", {{"at_mm", {5, 5}}}));
        doc = run(doc, op("remove_count", {{"at_mm", {2.1, 1.9}}}));
        QCOMPARE(measure(doc).count_groups[0].points, (std::vector<std::array<double, 2>>{{1, 1}}));
        doc = run(doc, op("move_count", {{"group", 0}, {"index", 0}, {"to", {1.2, 1.3}}}));
        QCOMPARE(measure(doc).count_groups[0].points[0], (std::array<double, 2>{1.2, 1.3}));
        QVERIFY(refusal(doc, op("remove_count", {{"at_mm", {9, 9}}})).find("no count mark there") != std::string::npos);
        QVERIFY(refusal(doc, op("remove_count", {{"group", 0}, {"index", 5}})).find("no count mark 5 in count group 0") != std::string::npos);
        QVERIFY(refusal(doc, op("set_count_group", {{"group", 0}, {"marker_size", 50}})).find("marker_size is a whole number 1..10") != std::string::npos);
        doc = run(doc, op("set_count_group", {{"group", 0}, {"marker_size", 10}, {"label_size", 20}, {"visible", false}, {"name", "Spots"}}));
        const core::CountGroup& g = measure(doc).count_groups[0];
        QVERIFY(g.marker_size == 10 && g.label_size == 20 && !g.visible && g.name == "Spots");
        QCOMPARE(measure(doc).active_group, std::size_t(1));  // (options alone do not make it the active one)
        // (a hidden group's marks are not found by place)
        QVERIFY(refusal(doc, op("remove_count", {{"at_mm", {1.2, 1.3}}})).find("no count mark there") != std::string::npos);
        doc = run(doc, op("clear_count"));
        QCOMPARE(core::count_total(measure(doc)), std::size_t(1));  // (the active group's only)
        doc = run(doc, op("clear_count", {{"group", "all"}}));
        QCOMPARE(core::count_total(measure(doc)), std::size_t(0));
        doc = run(doc, op("add_count", {{"at_mm", {3, 3}}}));
        QCOMPARE(core::count_total(measure(doc)), std::size_t(1));
        doc = run(doc, op("delete_count_group", {{"group", 1}}));
        QCOMPARE(measure(doc).count_groups.size(), std::size_t(1));
        QCOMPARE(measure(doc).active_group, std::size_t(0));
        doc = run(doc, op("set_count_group", {{"group", 0}}));
        QVERIFY(refusal(doc, op("set_count_group", {{"group", 7}})).find("no count group 7 on the page") != std::string::npos);
        QVERIFY(refusal(doc, op("add_count", {{"x", 1}})).find("at_mm is a place") != std::string::npos);
        QVERIFY(refusal(doc, op("add_count", {{"at_mm", {1, 1}}, {"group", 3}})).find("no count group 3 on the page") != std::string::npos);
        QVERIFY(refusal(doc, op("new_count_group", {{"color", "red"}})).find("count group's colour") != std::string::npos);
        // the new groups' colours in Photoshop's turn
        doc = run(doc, op("new_count_group"));
        QCOMPARE(measure(doc).count_groups[1].color, (std::array<int, 3>{0, 153, 255}));
        QCOMPARE(measure(doc).count_groups[1].name, std::string("カウントグループ 2"));
        // every group gone and nothing else: none kept
        doc = run(doc, op("delete_count_group"));
        doc = run(doc, op("delete_count_group"));
        QVERIFY(!doc.page(0).measure);
        QVERIFY(refusal(doc, op("clear_count")).find("the page has no count group") != std::string::npos);
    }

    // analysis_cmds/tests.rs analysis_info_and_cmyk_document: the question answered, the book as it was (nothing saved,
    // journalled or put in the history: OpRegistry::add_query).
    void measureInfo() {
        core::Document doc = book();
        doc = run(doc, op("add_count", {{"at_mm", {0.3, 0.4}}}));
        doc = run(doc, op("set_ruler_line", {{"start", {0, 0}}, {"end", {0.3, 0.4}}}));
        QVERIFY(render::ops_registry().is_query("measure_info"));
        const auto applied = run_ops(doc, Json::array({op("measure_info")}));
        QVERIFY(applied.unchanged);
        QVERIFY(applied.journal_ops.empty());
        const Json& r = applied.results[0];
        QCOMPARE(r["count"]["total"], Json(1));
        QCOMPARE(r["dpi"], Json(254.0));
        QCOMPARE(r["ruler"]["info"]["l1"], Json(5.0));
        QCOMPARE(r["scale"]["default"], Json(true));
        QCOMPARE(r["notes"], Json(0));
    }

    // The page size ops carry the marks and the line where the page goes (and the notes, J-c's).
    void carriedByThePageSizeOps() {
        core::Document doc = book();
        doc = run(doc, op("add_count", {{"at_mm", {2, 3}}}));
        doc = run(doc, op("set_ruler_line", {{"start", {1, 1}}, {"end", {5, 1}}, {"protractor", {1, 4}}}));
        doc = run(doc, op("add_annotation", {{"at_mm", {6, 7}}}));
        core::Document turned = run(doc, op("rotate_pages", {{"turn", "90cw"}}));
        // (90° clockwise on a 30 × 20 page: (x, y) → (20 − y, x))
        QCOMPARE(measure(turned).count_groups[0].points[0], (std::array<double, 2>{17, 2}));
        QCOMPARE(measure(turned).ruler->start, (std::array<double, 2>{19, 1}));
        QCOMPARE(measure(turned).ruler->end, (std::array<double, 2>{19, 5}));
        QCOMPARE(*measure(turned).ruler->protractor, (std::array<double, 2>{16, 1}));
        QCOMPARE(turned.page(0).annotations[0].at_mm, (std::array<double, 2>{13, 6}));
        // (a box on the working grid, 0.127 mm a pixel: moved by whole pixels)
        core::Document cut = run(doc, op("crop_page", {{"rect", {1.27, 1.27, 25.4, 15.24}}}));
        QCOMPARE(measure(cut).count_groups[0].points[0], (std::array<double, 2>{0.73, 1.73}));
        QCOMPARE(measure(cut).ruler->start, (std::array<double, 2>{-0.27, -0.27}));
        QCOMPARE(cut.page(0).annotations[0].at_mm, (std::array<double, 2>{4.73, 5.73}));
    }

    // MCP's apply_ops does not take them (the C++ build's own, as J-a's and J-c's); none is a frame any more but 目盛り
    // マーカー (J-d2).
    void notTheAgents() {
        for (const char* name : {"set_measurement_scale", "add_count", "remove_count", "move_count", "clear_count", "new_count_group",
                                 "delete_count_group", "set_count_group", "set_ruler_line", "straighten_layer", "measure_info"}) {
            QVERIFY2(!service::agent_op(name), name);
            for (const std::string_view frame : render::production_op_names()) QVERIFY2(frame != name, name);
        }
        const auto frames = render::production_op_names();
        QVERIFY(std::find(frames.begin(), frames.end(), std::string_view("place_scale_marker")) != frames.end());
    }

    // PSD's 1074 (annotations_map.rs scale_resource_roundtrip_and_keep_rules): written and read back, one this build
    // cannot read none, the default none; through the file's canvas on the page (the file's pixels → the page's); 1080 not
    // written once the page has its own marks.
    void psdScaleAndCount() {
        const core::MeasurementScale s{150, 2, "mm"};
        QCOMPARE(*pa::scale_of(pa::scale_block(s)), s);
        QVERIFY(pa::scale_block(s).size() % 2 == 0);
        QVERIFY(!pa::scale_of(std::string("\0\0\0\x10\x01", 5)));
        QVERIFY(!pa::scale_of({}));
        // read onto a page: a 300 × 200 pixel file on the 30 × 20 mm page — 10 file pixels a mm, the page's 10 too
        core::Document doc = book();
        core::Page& page = doc.edit_page(0);
        const core::vector::Affine onto{30, 0, 0, 20, 0, 0};
        render::psd_production::ReadContext context;
        context.onto = &onto;
        context.width_px = 600;  // (20 file pixels a mm: the page's pixel is 2 of the file's)
        context.height_px = 400;
        QVERIFY(pa::read_scale(page, pa::scale_block(s), context));
        QCOMPARE(page.measure->scale->pixel_length, 75.0);
        QCOMPARE(page.measure->scale->units, std::string("mm"));
        QVERIFY(!pa::scale_fingerprint(page, nullptr).empty());
        // (the page has one now: not read over it)
        QVERIFY(!pa::read_scale(page, pa::scale_block(core::MeasurementScale{3, 1, "cm"}), context));
        // written back in the file's pixels
        render::psd_production::WriteContext writing;
        writing.width_px = 600;
        writing.height_px = 400;
        QCOMPARE(*pa::scale_of(*pa::write_scale(page, writing)), s);
        // the default is none (kept as it was)
        core::Page bare = book().page(0);
        QVERIFY(!pa::read_scale(bare, pa::scale_block(core::MeasurementScale{}), context));
        QVERIFY(!pa::write_scale(bare, writing));
        // 1080
        QVERIFY(!pa::counts_own(bare));
        core::Document counted = run(book(), op("add_count", {{"at_mm", {1, 1}}}));
        QVERIFY(pa::counts_own(counted.page(0)));
        QVERIFY(render::psd_production::copies_resource(1074));
        QVERIFY(!render::psd_production::copies_resource(1080));
    }

    // A .pcraft's measurement (PhotoCraft's Measurement, its fields' defaults filled as serde reads them): the places in the
    // document's pixels → mm, the scale's pixels → the page's, written back; one not of the shape said and left out.
    void pcraftMeasurement() {
        namespace pm = render::pcraft_measure;
        const Json value = Json::parse(R"({"scale": {"pixel_length": 20, "logical_length": 1, "units": "mm"},
            "count_groups": [{"points": [[10, 20], [30, 40]]}, {"name": "B", "color": {"mode": "Rgb", "c": [0, 1, 0, 0], "alpha": 1},
            "marker_size": 50, "label_size": 3, "visible": false}], "active_count_group": 1,
            "ruler": {"start": [0, 0], "end": [100, 0]}})");
        // (a document of 10 pixels a mm onto a page of 10 pixels a mm)
        const auto m = pm::measure_of(value, 0.1, 10);
        QVERIFY(m);
        QCOMPARE(m->scale->pixel_length, 20.0);
        QCOMPARE(m->count_groups.size(), std::size_t(2));
        QCOMPARE(m->count_groups[0].name, std::string("カウントグループ 1"));
        QCOMPARE(m->count_groups[0].color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(m->count_groups[0].points[1], (std::array<double, 2>{3, 4}));
        QCOMPARE(m->count_groups[1].color, (std::array<int, 3>{0, 255, 0}));
        QCOMPARE(m->count_groups[1].marker_size, 10);  // (kept to its range)
        QCOMPARE(m->count_groups[1].label_size, 8);
        QVERIFY(!m->count_groups[1].visible);
        QCOMPARE(m->active_group, std::size_t(1));
        QCOMPARE(m->ruler->end, (std::array<double, 2>{10, 0}));
        // the defaults alone (every document's scale): none
        QVERIFY(!pm::measure_of(Json::parse(R"({"scale": {"pixel_length": 1, "logical_length": 1, "units": "pixels"}, "count_groups": []})"), 0.1, 10));
        QVERIFY(!pm::measure_of(Json::object(), 0.1, 10));
        // read onto a page and written back
        core::Document doc = book();
        render::pcraft_production::ReadContext context;
        context.page = &doc.edit_page(0);
        context.mm = 0.1;
        std::vector<std::string> said;
        context.say = [&](std::string s) { said.push_back(std::move(s)); };
        pm::read(value, context);
        QVERIFY(said.empty());
        QCOMPARE(doc.page(0).measure->count_groups.size(), std::size_t(2));
        render::pcraft_production::WriteContext writing;
        writing.px = 10;
        const auto written = pm::write(doc.page(0), writing);
        QVERIFY(written);
        QCOMPARE((*written)["count_groups"][0]["points"][1], Json::array({30.0, 40.0}));
        QCOMPARE((*written)["scale"]["pixel_length"], Json(20.0));
        QCOMPARE((*written)["active_count_group"], Json(1));
        QCOMPARE((*written)["ruler"]["protractor"], Json(nullptr));
        QCOMPARE(*pm::measure_of(*written, 0.1, 10), *doc.page(0).measure);
        // not of the shape: said, left out
        core::Document other = book();
        context.page = &other.edit_page(0);
        pm::read(Json::parse(R"({"count_groups": [{"points": "x"}]})"), context);
        QCOMPARE(said, (std::vector<std::string>{"計測（カウント・ものさし）は読めない形のため外しました"}));
        QVERIFY(!other.page(0).measure);
        QVERIFY(!pm::write(other.page(0), writing));
    }
};

QTEST_GUILESS_MAIN(TestMeasure)
#include "test_measure.moc"
