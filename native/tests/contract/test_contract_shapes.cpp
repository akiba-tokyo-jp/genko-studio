// pen-shapes@1 (PC3 C-b, the user's decision 7, SPEC.md COLOR-07「シェイプ」, schema-v4.md「ペンの層の形の部品（pen-shapes@1）」):
// a pen layer's shape parts as a person or an agent uses them — added (on the pen layer, on a new one; from an area),
// edited (moved keeping the live shape, made again, a path, the path operations, a subpath appended, the components
// merged, fill, stroke, opacity, over, name, order), combined, moved between layers, made a patch or lines, rasterized;
// drawn in their place among the layer's patches and lines (over); the selection's cut, paste, transform and delete
// with them; the eraser's three ways, ゆがみ; duplicated and merged; the bucket's walls; drawn on every kind of page; a
// fill layer's new gradient forms; through the command line saved, opened again, undone and redone (the feature named
// only while there are parts), `genko shapes`, inspect --paths; a pen layer without parts saved and drawn as before;
// the PSD's shape layers (one part: a shape layer, mixed: a group, clipped: pixels) read by psd-tools and by import_psd.
// (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/path_raster.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace pr = render::path_raster;
namespace vec = core::vector;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

const core::Layer& layer_of(const core::Page& page, std::string_view id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + std::string(id));
}

std::string ink_of(const core::Page& page) {
    for (const core::Layer& layer : page.layers)
        if (layer.role == core::LayerRole::Ink) return layer.id;
    throw std::runtime_error("no ink layer");
}

// A page of 60 × 80 mm without panels; its ink layer drawn whole (not cut at panels).
core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("shapes", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    return run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", ink_of(doc.page(0))}, {"panel_clip", false}, {"panel_each", false}});
}

Json black() { return Json{{"rgb", Json::array({0, 0, 0})}}; }

Json rect_shape(double x, double y, double w, double h) { return Json{{"kind", "rect"}, {"rect", Json::array({x, y, w, h})}}; }

Json square_path(double x, double y, double w) {
    return Json{{"subpaths", Json::array({Json{{"closed", true},
                                               {"knots", Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + w}),
                                                                      Json::array({x, y + w})})}}})}};
}

render::Image drawn(const core::Document& doc, int dpi = 100) {
    render::RenderOptions options;
    options.mode = "print";
    return render::render_page(doc.page(0), dpi, options, &doc).image.convert("RGB");
}

int grey_at(const render::Image& image, double x_mm, double y_mm, int dpi = 100) {
    const auto px = image.getpixel(static_cast<int>(x_mm / 25.4 * dpi), static_cast<int>(y_mm / 25.4 * dpi));
    return static_cast<int>((px[0] + px[1] + px[2]) / 3);
}

// The part's filled area (mm²): its coverage at 254 dpi.
double area_of(const vec::Path& path) {
    const auto b = path.control_bounds();
    if (!b) return 0;
    const double k = 10;
    const pr::PixelBox box{static_cast<int>(std::floor((*b)[0] * k)) - 2, static_cast<int>(std::floor((*b)[1] * k)) - 2,
                           static_cast<int>(std::ceil((*b)[2] * k)) + 2, static_cast<int>(std::ceil((*b)[3] * k)) + 2};
    const std::vector<float> cover = pr::path_coverage(pr::to_pixels(path, 254), box);
    return std::accumulate(cover.begin(), cover.end(), 0.0) / (k * k);
}

const vec::ShapePart& part_of(const core::Document& doc, std::string_view layer, std::string_view id) {
    for (const vec::ShapePart& part : layer_of(doc.page(0), layer).shapes->items)
        if (part.id == id) return part;
    throw std::runtime_error("no part " + std::string(id));
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

}  // namespace

class TestContractShapes : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr, const QProcessEnvironment& env = {}) {
        const auto ran = genko::test::run_genko(args, env);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString make_book(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        return book;
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool has_feature(const Json& saved, const char* feature) {
        return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // add_shape_part, edit_shape_part, combine_shape_parts, move_shape_parts: what each makes and reports; what is not
    // well formed or not on a pen layer refused, the book as it was.
    void partsMadeAndEdited() {
        core::Document book = small_book();
        const std::string ink = ink_of(book.page(0));
        core::ApplyResult made = run_ops(book, Json::array({Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"},
                                                                 {"shape", rect_shape(10, 10, 20, 10)}, {"fill", black()}}}));
        Json report = made.results[0];
        QCOMPARE(report["layer"], Json(ink));
        QCOMPARE(report["part"], Json("r1"));
        QCOMPARE(report["kind"], Json("rect"));
        QCOMPARE(report["bounds"], Json::array({10, 10, 20, 10}));  // ([x, y, w, h] of what it draws)
        book = made.doc;
        const vec::ShapePart& r1 = part_of(book, ink, "r1");
        QVERIFY(r1.live.has_value() && r1.live->kind == vec::LiveShape::Kind::Rect);
        QVERIFY(near(area_of(r1.path), 200, 0.3));
        // a new pen layer of its own, over the one drawn on (Photoshop's shape layer)
        made = run_ops(book, Json::array({Json{{"op", "add_shape_part"}, {"page", 1}, {"new_layer", Json{{"id", "pen2"}, {"name", "楕円形"}, {"after", ink}}},
                                               {"id", "e1"}, {"shape", Json{{"kind", "ellipse"}, {"rect", Json::array({5, 40, 20, 10})}}}}}));
        book = made.doc;
        const core::Layer& pen2 = layer_of(book.page(0), "pen2");
        QVERIFY(pen2.kind == core::LayerKind::Strokes);
        QCOMPARE(pen2.title, std::string("楕円形"));
        QCOMPARE(pen2.shapes->items.size(), std::size_t{1});
        // on a layer that is not a pen layer: refused
        const core::Document painted = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint1"}});
        QVERIFY(refusal(painted, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "paint1"}, {"shape", rect_shape(0, 0, 5, 5)}}).find("pen layer") !=
                std::string::npos);
        for (const Json& bad : {Json{{"kind", "hexagon"}}, Json{{"kind", "rect"}}, Json{{"kind", "rect"}, {"rect", Json::array({0, 0, 5})}},
                                Json{{"kind", "polygon"}, {"rect", Json::array({0, 0, 5, 5})}, {"sides", 2}}}) {
            QVERIFY2(!refusal(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"shape", bad}}).empty(), bad.dump().c_str());
        }
        QVERIFY(!refusal(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"shape", rect_shape(0, 0, 5, 5)}}).empty());
        // from an area (選択範囲 → シェイプに変換): its outline traced
        made = run_ops(book, Json::array({Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "a1"},
                                               {"area", Json{{"rect", Json::array({40, 40, 10, 20})}}}, {"fill", black()}}}));
        QVERIFY2(near(area_of(part_of(made.doc, ink, "a1").path), 200, 8), vec::to_json(part_of(made.doc, ink, "a1").path).dump().c_str());
        // edited: moved (the live shape kept), made again, a path given (the live shape gone)
        const auto edit = [&](const core::Document& doc, Json terms) {
            terms["op"] = "edit_shape_part";
            terms["page"] = 1;
            terms["layer_id"] = ink;
            terms["id"] = "r1";
            return run(doc, terms);
        };
        core::Document moved = edit(book, Json{{"move_mm", Json::array({5, 2})}});
        QVERIFY(part_of(moved, ink, "r1").live.has_value());
        QCOMPARE(part_of(moved, ink, "r1").live->rect[0], 15.0);
        QCOMPARE(part_of(moved, ink, "r1").path.control_bounds()->at(1), 12.0);
        core::Document again = edit(book, Json{{"shape", Json{{"kind", "rounded_rect"}, {"radii", 2}}}});
        QVERIFY(part_of(again, ink, "r1").live->radii[0] == 2.0);
        QVERIFY(area_of(part_of(again, ink, "r1").path) < 200);
        core::Document pathed = edit(book, Json{{"path", square_path(0, 0, 10)}});
        QVERIFY(!part_of(pathed, ink, "r1").live.has_value());
        // a subpath appended with an operation, every subpath's operation, the components merged (the same area)
        core::Document holed = edit(book, Json{{"append", Json{{"path", square_path(15, 12, 4)}, {"op", "subtract"}}}});
        QCOMPARE(part_of(holed, ink, "r1").path.subpaths.size(), std::size_t{2});
        QVERIFY(part_of(holed, ink, "r1").path.subpaths[1].op == vec::PathOp::Subtract);
        QVERIFY(near(area_of(part_of(holed, ink, "r1").path), 200 - 16, 0.3));
        const core::Document crossed = edit(holed, Json{{"operation", "intersect"}});
        QVERIFY(near(area_of(part_of(crossed, ink, "r1").path), 16, 0.3));
        const core::Document merged = edit(holed, Json{{"merge_components", true}});
        QVERIFY(near(area_of(part_of(merged, ink, "r1").path), 200 - 16, 0.3));
        QVERIFY(!refusal(holed, Json{{"op", "edit_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"operation", "union"}}).empty());
        QVERIFY(!refusal(holed, Json{{"op", "edit_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"operation", "combine"}, {"subpath", 5}}).empty());
        // fill, stroke, opacity, over, name; order
        core::Document dressed = edit(book, Json{{"fill", Json{{"rgb", nullptr}}},
                                                 {"stroke", Json{{"width_mm", 1}, {"rgb", Json::array({255, 0, 0})}, {"align", "inside"}}},
                                                 {"opacity", 0.5}, {"over", true}, {"name", "枠"}});
        const vec::ShapePart& d = part_of(dressed, ink, "r1");
        QCOMPARE(d.fill, (Json{{"rgb", nullptr}}));
        QVERIFY(d.stroke && d.stroke->width_mm == 1.0 && d.stroke->align == vec::StrokeAlign::Inside);
        QCOMPARE(d.opacity, 0.5);
        QVERIFY(d.over);
        QCOMPARE(*d.name, std::string("枠"));
        dressed = run(dressed, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r2"}, {"shape", rect_shape(20, 15, 20, 10)}});
        const core::Document ordered = edit(dressed, Json{{"order", "front"}});
        QCOMPARE(layer_of(ordered.page(0), ink).shapes->items.back().id, std::string("r1"));
        // combined (Combine Shapes over two parts): the first takes the second's subpaths with the operation
        const core::ApplyResult combined = run_ops(dressed, Json::array({Json{{"op", "combine_shape_parts"}, {"page", 1}, {"layer_id", ink},
                                                                              {"ids", Json::array({"r1", "r2"})}, {"operation", "exclude"}}}));
        const core::Layer& one = layer_of(combined.doc.page(0), ink);
        QCOMPARE(one.shapes->items.size(), std::size_t{1});
        QVERIFY(one.shapes->items[0].path.subpaths[1].op == vec::PathOp::Exclude);
        QVERIFY(near(area_of(one.shapes->items[0].path), 200 + 200 - 2 * 50, 0.5));
        QVERIFY(!refusal(dressed, Json{{"op", "combine_shape_parts"}, {"page", 1}, {"layer_id", ink}, {"ids", Json::array({"r1"})}}).empty());
        // moved to another pen layer
        const core::Document carried = run(dressed, Json{{"op", "move_shape_parts"}, {"page", 1}, {"layer_id", ink}, {"ids", Json::array({"r2"})}, {"to_layer", "pen2"}});
        QCOMPARE(layer_of(carried.page(0), ink).shapes->items.size(), std::size_t{1});
        QCOMPARE(layer_of(carried.page(0), "pen2").shapes->items.size(), std::size_t{2});
        // its own drawing: a patch, lines and a fill (the 図形 of before)
        const core::ApplyResult patched = run_ops(book, Json::array({Json{{"op", "edit_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"to", "patch"}}}));
        QVERIFY(layer_of(patched.doc.page(0), ink).shapes->items.empty());
        QCOMPARE(layer_of(patched.doc.page(0), ink).patches.size(), std::size_t{1});
        QCOMPARE(patched.results[0]["part"], Json(nullptr));
        QVERIFY(std::abs(grey_at(drawn(patched.doc), 20, 15) - grey_at(drawn(book), 20, 15)) <= 8);
        const core::Document lined = edit(dressed, Json{{"to", "lines"}});
        QVERIFY(layer_of(lined.page(0), ink).stroke_count() > 0);
        const core::Document flat = run(dressed, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", ink}, {"shape", true}});
        QVERIFY(layer_of(flat.page(0), ink).shapes->items.empty());
        QCOMPARE(layer_of(flat.page(0), ink).patches.size(), std::size_t{2});
        // deleted with the line editor's delete (the part's id as a line's)
        const core::Document gone = run(dressed, Json{{"op", "vector_edit"}, {"page", 1}, {"layer_id", ink}, {"action", "delete"}, {"stroke_id", "r2"}});
        QCOMPARE(layer_of(gone.page(0), ink).shapes->items.size(), std::size_t{1});
    }

    // Drawn in their place: the parts after the layer's patches and before its lines; `over` after its lines; the
    // part's opacity; a fill layer's new gradient forms.
    void drawnInTheirPlace() {
        core::Document book = small_book();
        const std::string ink = ink_of(book.page(0));
        book = run(book, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({5, 30}), Json::array({55, 30})})},
                              {"width_mm", 3}, {"rgb", Json::array({0, 0, 0})}});
        const core::Document under = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "w"}, {"shape", rect_shape(20, 20, 20, 20)},
                                                    {"fill", Json{{"rgb", Json::array({255, 255, 255})}}}});
        QVERIFY(grey_at(drawn(under), 30, 30) < 60);  // (the line over the part)
        const core::Document over = run(under, Json{{"op", "edit_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "w"}, {"over", true}});
        QVERIFY(grey_at(drawn(over), 30, 30) > 240);  // (the part over the line)
        // a part added and deleted: the layer drawn as it was (a pen layer without parts: as before)
        const core::Document gone = run(under, Json{{"op", "vector_edit"}, {"page", 1}, {"layer_id", ink}, {"action", "delete"}, {"stroke_id", "w"}});
        QVERIFY(layer_of(gone.page(0), ink).shapes == vec::empty_shapes());
        QVERIFY(drawn(gone).tobytes() == drawn(book).tobytes());
        const core::Document half = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"shape", rect_shape(20, 50, 20, 20)},
                                                   {"fill", black()}, {"opacity", 0.5}});
        const int grey = grey_at(drawn(half), 30, 60);
        QVERIFY2(grey > 100 && grey < 160, std::to_string(grey).c_str());
        // a fill layer's gradient: reflected (the same both sides of its start), reverse, diamond, angle
        const auto fill_layer = [&](const char* shape, bool reverse) {
            Json g{{"shape", shape}, {"from", Json::array({30, 40})}, {"to", Json::array({55, 40})}, {"rgb_from", Json::array({0, 0, 0})},
                   {"rgb_to", Json::array({255, 255, 255})}};
            if (reverse) g["reverse"] = true;
            return drawn(run(small_book(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "gradient"}, {"id", "g"}, {"gradient", g}}));
        };
        const render::Image reflected = fill_layer("reflected", false);
        QVERIFY(std::abs(grey_at(reflected, 15, 40) - grey_at(reflected, 45, 40)) <= 3);
        QVERIFY(grey_at(reflected, 30.2, 40) < 20);
        const render::Image linear = fill_layer("linear", false), back = fill_layer("linear", true);
        QVERIFY(std::abs(grey_at(linear, 40, 40) + grey_at(back, 40, 40) - 255) <= 4);
        const render::Image diamond = fill_layer("diamond", false);
        QVERIFY(std::abs(grey_at(diamond, 40, 40) - grey_at(diamond, 30, 50)) <= 4);
        const render::Image angle = fill_layer("angle", false);
        QVERIFY(std::abs(grey_at(angle, 30, 30) - grey_at(angle, 30, 50)) > 20);
        QVERIFY(!refusal(small_book(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "gradient"}, {"gradient", Json{{"shape", "spiral"}}}}).empty());
    }

    // レイヤー → ラスタライズ → シェイプ keeps the look: the parts become patches in the order they are drawn (a part over
    // the lines after one under them, whatever their order in the list); with a part over the layer's lines — which only
    // pixels keep over them — the layer is drawn in whole as it shows (a paint layer: the op says so).
    void rasterizedAsTheyShow() {
        core::Document book = small_book();
        const std::string ink = ink_of(book.page(0));
        const core::Document lined = run(book, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink},
                                                    {"points", Json::array({Json::array({5, 30}), Json::array({55, 30})})}, {"width_mm", 3},
                                                    {"rgb", Json::array({0, 0, 0})}});
        const core::Document over = run(lined, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "w"}, {"over", true},
                                                    {"shape", rect_shape(20, 20, 20, 20)}, {"fill", Json{{"rgb", Json::array({255, 255, 255})}}}});
        QVERIFY(grey_at(drawn(over), 30, 30) > 240);  // (the white part over the line)
        const core::ApplyResult whole = run_ops(over, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", ink}, {"shape", true}}}));
        QCOMPARE(whole.results.at(0)["whole"], Json(true));
        const core::Layer& flat = layer_of(whole.doc.page(0), ink);
        QVERIFY(flat.kind == core::LayerKind::Raster && flat.shapes->items.empty() && flat.stroke_count() == 0);
        const render::Image seen = drawn(whole.doc);
        QVERIFY2(grey_at(seen, 30, 30) > 240, std::to_string(grey_at(seen, 30, 30)).c_str());  // (still over the line)
        QVERIFY(grey_at(seen, 10, 30) < 60 && grey_at(seen, 30, 50) > 240);
        // no lines: the parts as patches in the order they are drawn (the one over the other first in the list)
        core::Document parts = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "top"}, {"over", true},
                                              {"shape", rect_shape(20, 20, 20, 20)}, {"fill", Json{{"rgb", Json::array({255, 255, 255})}}}});
        parts = run(parts, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "under"}, {"shape", rect_shape(10, 10, 40, 40)}, {"fill", black()}});
        QVERIFY(grey_at(drawn(parts), 30, 30) > 240 && grey_at(drawn(parts), 15, 15) < 60);
        const core::ApplyResult patched = run_ops(parts, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", ink}, {"shape", true}}}));
        QVERIFY(!patched.results.at(0).contains("whole"));
        const core::Layer& pen = layer_of(patched.doc.page(0), ink);
        QVERIFY(pen.kind != core::LayerKind::Raster && pen.shapes->items.empty());
        QCOMPARE(pen.patches.size(), std::size_t{2});
        const render::Image after = drawn(patched.doc);
        QVERIFY2(grey_at(after, 30, 30) > 240, std::to_string(grey_at(after, 30, 30)).c_str());
        QVERIFY(grey_at(after, 15, 15) < 60);
    }

    // The selection with parts (render/selection): one inside moved whole by transform_area (its live shape kept), one
    // its edge crosses cut in two (the halves' areas the whole's); delete_area takes the part's share; copied and pasted.
    void theSelection() {
        core::Document book = small_book();
        const std::string ink = ink_of(book.page(0));
        book = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"shape", rect_shape(10, 10, 20, 10)}, {"fill", black()}});
        const core::Document moved = run(book, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", ink}, {"area", Json{{"rect", Json::array({5, 5, 40, 30})}}},
                                                    {"matrix", Json::array({1, 0, 0, 1, 10, 20})}});
        const auto& parts = layer_of(moved.page(0), ink).shapes->items;
        QCOMPARE(parts.size(), std::size_t{1});
        QCOMPARE(parts[0].path.control_bounds()->at(0), 20.0);
        QVERIFY(parts[0].live.has_value());
        QVERIFY(grey_at(drawn(moved), 30, 35) < 40 && grey_at(drawn(moved), 15, 15) > 240);
        // half of it moved: two parts, their areas the whole's
        const core::Document cut = run(book, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", ink}, {"area", Json{{"rect", Json::array({20, 0, 40, 40})}}},
                                                  {"matrix", Json::array({1, 0, 0, 1, 0, 30})}});
        const auto& halves = layer_of(cut.page(0), ink).shapes->items;
        QCOMPARE(halves.size(), std::size_t{2});
        QVERIFY(near(area_of(halves[0].path) + area_of(halves[1].path), 200, 1.0));
        // deleted in an area: the rest stays a part
        const core::Document deleted = run(book, Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", ink}, {"area", Json{{"rect", Json::array({0, 0, 20, 40})}}}});
        QCOMPARE(layer_of(deleted.page(0), ink).shapes->items.size(), std::size_t{1});
        QVERIFY(near(area_of(layer_of(deleted.page(0), ink).shapes->items[0].path), 100, 1.0));
        // copied (lifted from a copy) and pasted on another pen layer: a new id, through the paste's matrix
        core::Layer copy = layer_of(book.page(0), ink);
        const Json whole = render::selection::resolve(Json{{"rect", Json::array({0, 0, 60, 80})}}, book.page(0), &book);
        render::selection::Items lifted = render::selection::lift(copy, whole, book.page(0));
        QCOMPARE(lifted.shapes.size(), std::size_t{1});
        const Json items = render::selection::items_to_json(lifted);
        QVERIFY(items.contains("shapes"));
        core::Document pasted = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen2"}});
        pasted = run(pasted, Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pen2"}, {"items", items}, {"matrix", Json::array({1, 0, 0, 1, 0, 40})}});
        const auto& copied = layer_of(pasted.page(0), "pen2").shapes->items;
        QCOMPARE(copied.size(), std::size_t{1});
        QVERIFY(copied[0].id != "r1");
        QCOMPARE(copied[0].path.control_bounds()->at(1), 50.0);
        // pasted on a paint layer: drawn into its pixels
        core::Document onto = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint1"}});
        onto = run(onto, Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint1"}, {"panel_clip", false}, {"panel_each", false}});
        onto = run(onto, Json{{"op", "paste"}, {"page", 1}, {"layer_id", "paint1"}, {"items", items}, {"matrix", Json::array({1, 0, 0, 1, 0, 40})}});
        QVERIFY(layer_of(onto.page(0), "paint1").shapes->items.empty());
        QVERIFY(grey_at(drawn(onto), 20, 55) < 40);
    }

    // The eraser (its three ways): a band taken out of a closed part; whole, to_crossing: every part it touches gone;
    // ゆがみ bends the parts with the lines; duplicate_layer gives new ids; merge_down keeps parts on a pen layer and
    // draws them into a paint one; the bucket stops at a part's ring.
    void erasedBentAndMerged() {
        core::Document book = small_book();
        const std::string ink = ink_of(book.page(0));
        book = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "r1"}, {"shape", rect_shape(10, 10, 20, 20)}, {"fill", black()}});
        const Json band{{"op", "erase"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({20, 0}), Json::array({20, 40})})}, {"width_mm", 2}};
        const core::Document erased = run(book, band);
        const auto& left = layer_of(erased.page(0), ink).shapes->items;
        QCOMPARE(left.size(), std::size_t{1});
        QVERIFY2(near(area_of(left[0].path), 400 - 40, 1.5), std::to_string(area_of(left[0].path)).c_str());
        for (const char* mode : {"whole", "to_crossing"}) {
            Json whole = band;
            whole["mode"] = mode;
            QVERIFY2(layer_of(run(book, whole).page(0), ink).shapes->items.empty(), mode);
        }
        Json missed = band;
        missed["points"] = Json::array({Json::array({50, 0}), Json::array({50, 40})});
        QCOMPARE(layer_of(run(book, missed).page(0), ink).shapes->items.size(), std::size_t{1});
        // ゆがみ: the part pushed along
        const core::Document pushed = run(book, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", ink}, {"width_mm", 16},
                                                     {"points", Json::array({Json::array({20, 20}), Json::array({26, 20})})}});
        const vec::ShapePart& bent = layer_of(pushed.page(0), ink).shapes->items[0];
        QVERIFY(!bent.live.has_value());
        QVERIFY(bent.path != layer_of(book.page(0), ink).shapes->items[0].path);
        // duplicated: new ids; merged down onto a pen layer (its parts after the lower one's) and onto a paint one
        const core::Document twice = run(book, Json{{"op", "duplicate_layer"}, {"page", 1}, {"id", ink}});
        std::vector<std::string> ids;
        for (const core::Layer& layer : twice.page(0).layers)
            for (const vec::ShapePart& part : layer.shapes->items) ids.push_back(part.id);
        QCOMPARE(ids.size(), std::size_t{2});
        QVERIFY(ids[0] != ids[1]);
        core::Document stacked = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "upper"}});  // (on top: over a pen layer)
        stacked = run(stacked, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "upper"}, {"id", "u1"}, {"shape", rect_shape(40, 40, 5, 5)}});
        const std::size_t top = stacked.page(0).layers.size() - 1;
        const std::string below = stacked.page(0).layers[top - 1].id;
        const std::size_t had = stacked.page(0).layers[top - 1].shapes->items.size();
        stacked = run(stacked, Json{{"op", "set_layer"}, {"page", 1}, {"id", "upper"}, {"panel_clip", stacked.page(0).layers[top - 1].panel_clip},
                                    {"panel_each", stacked.page(0).layers[top - 1].panel_each}});  // (as the one below: merged as lines)
        const core::Document joined = run(stacked, Json{{"op", "merge_down"}, {"page", 1}, {"id", "upper"}});
        QCOMPARE(layer_of(joined.page(0), below).shapes->items.size(), had + 1);
        QCOMPARE(layer_of(joined.page(0), below).shapes->items.back().id, std::string("u1"));
        core::Document painted = small_book();
        const std::string ink2 = ink_of(painted.page(0));
        painted = run(painted, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink2}, {"shape", rect_shape(10, 10, 20, 20)}, {"fill", black()}});
        const int before = grey_at(drawn(painted), 20, 20);
        const core::Document flattened = run(painted, Json{{"op", "merge_visible"}, {"page", 1}});
        QVERIFY(std::abs(grey_at(drawn(flattened), 20, 20) - before) <= 4);
        // the bucket on a paint layer stops at a ring (the part's stroke, the page as the reference)
        core::Document ring = small_book();
        const std::string ink3 = ink_of(ring.page(0));
        ring = run(ring, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink3}, {"shape", rect_shape(10, 10, 30, 30)}, {"fill", nullptr},
                              {"stroke", Json{{"width_mm", 1}, {"rgb", Json::array({0, 0, 0})}}}});
        ring = run(ring, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint1"}});
        ring = run(ring, Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint1"}, {"panel_clip", false}, {"panel_each", false}});
        ring = run(ring, Json{{"op", "fill"}, {"page", 1}, {"layer_id", "paint1"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({255, 0, 0})}});
        const render::Image filled = drawn(ring);
        const auto inside = filled.getpixel(static_cast<int>(25 / 25.4 * 100), static_cast<int>(25 / 25.4 * 100));
        const auto outside = filled.getpixel(static_cast<int>(50 / 25.4 * 100), static_cast<int>(60 / 25.4 * 100));
        QVERIFY(inside[0] > 200 && inside[1] < 60);
        QVERIFY(outside[1] > 200);  // (not red: the ring held)
    }

    // Every kind of page draws the part (a representative pixel): 8-, 16-, 32-bit RGB, an RGB page of a profile,
    // grey, bitmap, indexed, CMYK, Lab, multichannel.
    void everyKindOfPage() {
        for (const char* mode : {"rgb8", "rgb16", "rgb32", "p3", "gray", "bitmap", "indexed", "cmyk", "lab", "multichannel"}) {
            core::Document book = small_book();
            const std::string m = mode;
            Json ops = Json::array();
            if (m == "rgb16" || m == "rgb32") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", m == "rgb16" ? 16 : 32}});
            if (m == "p3") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}, {"profile", "display-p3"}});
            if (m == "gray" || m == "bitmap") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
            if (m == "bitmap") ops.push_back(Json{{"op", "to_bitmap"}, {"page", 1}, {"method", "threshold"}});
            if (m == "indexed") ops.push_back(Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}});
            if (m == "cmyk") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
            if (m == "lab") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}});
            if (m == "multichannel") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}});
            if (!ops.empty()) book = run_ops(book, ops).doc;
            const core::Document shaped = run(book, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink_of(book.page(0))},
                                                         {"shape", rect_shape(10, 10, 20, 20)}, {"fill", black()},
                                                         {"stroke", Json{{"width_mm", 1}, {"rgb", Json::array({0, 0, 0})}}}});
            const render::Image seen = drawn(shaped);
            QVERIFY2(grey_at(seen, 20, 20) < 70, (m + " " + std::to_string(grey_at(seen, 20, 20))).c_str());
            QVERIFY2(grey_at(seen, 45, 60) > 200, (m + " " + std::to_string(grey_at(seen, 45, 60))).c_str());
            QVERIFY2(std::abs(grey_at(seen, 45, 60) - grey_at(drawn(book), 45, 60)) <= 2, mode);
        }
    }

    // Through the command line: the parts saved (their own asset; pen-shapes@1 named while there are any), opened again
    // the same, undone and redone; a pen layer without parts saved and drawn as before (a part added and deleted leaves
    // the same project); inspect --paths; `genko shapes`.
    void savedAndUndone() {
        const QString book = make_book(QStringLiteral("shapes.genko"));
        Json saved = project(book);
        std::string ink;
        for (const Json& layer : saved["pages"][0]["layers"])
            if (layer.value("role", std::string()) == "ink") ink = layer["id"].get<std::string>();
        QVERIFY(!ink.empty());
        Json reply = apply(book, QStringLiteral("s0.json"),
                           Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({20, 40}), Json::array({150, 120})})},
                                             {"width_mm", 1}}}));
        QVERIFY(reply.value("ok", false));
        const Json plain = project(book);
        QVERIFY(!has_feature(plain, "pen-shapes@1"));
        int exit = -1;
        reply = apply(book, QStringLiteral("s1.json"),
                      Json::array({Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", ink}, {"id", "star1"},
                                        {"shape", Json{{"kind", "star"}, {"rect", Json::array({40, 40, 60, 60})}, {"sides", 5}}}, {"fill", black()}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        saved = project(book);
        QVERIFY(has_feature(saved, "pen-shapes@1"));
        bool found = false;
        for (const Json& layer : saved["pages"][0]["layers"]) {
            if (layer.value("id", std::string()) != ink) continue;
            found = layer.contains("shapes_blob");
            QVERIFY(fs::exists(core::path_from_utf8(book.toStdString() + "/assets/" + layer["shapes_blob"].get<std::string>())) ||
                    !layer["shapes_blob"].get<std::string>().empty());
        }
        QVERIFY(found);
        const Json inspected = genko({QStringLiteral("inspect"), book, QStringLiteral("--paths"), QStringLiteral("--page"), QStringLiteral("1")});
        bool listed = false;
        for (const Json& layer : inspected.value("layers", Json::array()))
            for (const Json& shape : layer.value("shapes", Json::array())) listed = listed || shape.value("kind", std::string()) == "star";
        QVERIFY2(listed, inspected.dump().c_str());
        // undone, redone
        genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(project(book)["pages"], plain["pages"]);
        QVERIFY(!has_feature(project(book), "pen-shapes@1"));
        genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY(has_feature(project(book), "pen-shapes@1"));
        // the part deleted: the pen layer saved and drawn as it was before it had one
        reply = apply(book, QStringLiteral("s2.json"),
                      Json::array({Json{{"op", "vector_edit"}, {"page", 1}, {"layer_id", ink}, {"action", "delete"}, {"stroke_id", "star1"}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        const Json after = project(book);
        QCOMPARE(after["pages"].dump(), plain["pages"].dump());
        QCOMPARE(after.value("features", Json::array()), plain.value("features", Json::array()));
        // genko shapes: this machine's presets (the settings folder's shapes.json)
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), path(QStringLiteral("config")));
        const Json listed_presets = genko({QStringLiteral("shapes"), QStringLiteral("list")}, &exit, env);
        QCOMPARE(exit, 0);
        QVERIFY(listed_presets["groups"].size() >= 5);
        const std::string first = listed_presets["groups"][0]["shapes"][0]["name"].get<std::string>();
        const Json placed = genko({QStringLiteral("shapes"), QStringLiteral("path"), QString::fromStdString(first), QStringLiteral("--rect"), QStringLiteral("10,10,40,20")},
                                  &exit, env);
        QCOMPARE(exit, 0);
        const auto box = vec::path_from_json(placed["path"]).control_bounds();
        QVERIFY(box && near((*box)[1], 10, 1e-6) && near((*box)[3], 30, 1e-6));
        const Json kept = genko({QStringLiteral("shapes"), QStringLiteral("new"), QStringLiteral("--path"), QStringLiteral("M 0 0 L 100 0 L 50 100 Z"),
                                 QStringLiteral("--name"), QStringLiteral("くさび")},
                                &exit, env);
        QCOMPARE(exit, 0);
        QCOMPARE(kept["name"], Json("くさび"));
        QVERIFY(QFile::exists(path(QStringLiteral("config/shapes.json"))));
        genko({QStringLiteral("shapes"), QStringLiteral("edit"), QStringLiteral(R"({"action": "rename", "preset": "くさび", "name": "楔"})")}, &exit, env);
        QCOMPARE(exit, 0);
        genko({QStringLiteral("shapes"), QStringLiteral("path"), QStringLiteral("楔"), QStringLiteral("--rect"), QStringLiteral("0,0,10,10")}, &exit, env);
        QCOMPARE(exit, 0);
        genko({QStringLiteral("shapes"), QStringLiteral("reset")}, &exit, env);
        QCOMPARE(exit, 0);
        genko({QStringLiteral("shapes"), QStringLiteral("path"), QStringLiteral("楔"), QStringLiteral("--rect"), QStringLiteral("0,0,10,10")}, &exit, env);
        QVERIFY(exit != 0);
    }

    // PSD (the user's decision 3: always written and read): a pen layer of one part a shape layer (SoCo, vsms/vmsk,
    // vstk, vogk), a mixed one a group of its pictures, parts and lines, a clipped one pixels; psd-tools reads them;
    // import_psd reads a shape layer back as a pen layer of one part.
    void psdShapeLayers() {
        const QString book = make_book(QStringLiteral("psd.genko"));
        Json reply = apply(book, QStringLiteral("p.json"),
                           Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "one"}, {"name", "四角"}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "one"}, {"panel_clip", false}, {"panel_each", false}},
                                        Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "one"}, {"id", "box"}, {"shape", rect_shape(20, 20, 60, 40)},
                                             {"fill", Json{{"rgb", Json::array({200, 30, 30})}}},
                                             {"stroke", Json{{"width_mm", 1}, {"rgb", Json::array({0, 0, 0})}}}},
                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "mixed"}, {"name", "混ぜ"}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "mixed"}, {"panel_clip", false}, {"panel_each", false}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "mixed"}, {"points", Json::array({Json::array({20, 100}), Json::array({150, 120})})},
                                             {"width_mm", 1}},
                                        Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "mixed"}, {"shape", Json{{"kind", "ellipse"}, {"rect", Json::array({30, 90, 40, 30})}}},
                                             {"fill", black()}},
                                        Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "mixed"}, {"over", true}, {"name", "上の形"},
                                             {"shape", rect_shape(80, 90, 30, 30)}, {"fill", Json{{"gradient", Json{{"shape", "linear"}}}}}},
                                        // (a pen layer without parts, its vector mask: a layer of its own)
                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "masked"}, {"name", "マスク付き"}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "masked"}, {"points", Json::array({Json::array({20, 150}), Json::array({150, 160})})},
                                             {"width_mm", 2}},
                                        Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "masked"}, {"path", square_path(10, 140, 60)}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        int exit = -1;
        const Json out = genko({QStringLiteral("export"), book, path(QStringLiteral("out.psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                QStringLiteral("--dpi"), QStringLiteral("100"), QStringLiteral("--json")},
                               &exit);
        QVERIFY2(exit == 0, out.dump().c_str());
        if (!genko::test::python_ref().isEmpty()) {
            constexpr const char* kRead = R"PY(
import json, sys
from psd_tools import PSDImage
from psd_tools.constants import Tag
psd = PSDImage.open(sys.argv[2])
def info(layer):
    d = {"name": layer.name, "kind": layer.kind}
    if layer.kind == "group":
        d["children"] = [info(c) for c in layer]
    blocks = layer.tagged_blocks
    has = lambda tag: blocks is not None and tag in blocks
    d["soco"] = has(Tag.SOLID_COLOR_SHEET_SETTING)
    d["gdfl"] = has(Tag.GRADIENT_FILL_SETTING)
    d["vstk"] = has(Tag.VECTOR_STROKE_DATA)
    d["vogk"] = has(Tag.VECTOR_ORIGINATION_DATA)
    d["vector"] = bool(layer.has_vector_mask())
    if layer.has_vector_mask():
        d["knots"] = sum(len(sub) for sub in layer.vector_mask.paths)
    return d
json.dump({"layers": [info(l) for l in psd]}, open(sys.argv[1], "w"), ensure_ascii=False)
)PY";
            const auto ran = genko::test::run(genko::test::python_ref(),
                                              {QStringLiteral("-c"), QString::fromUtf8(kRead), path(QStringLiteral("psd.json")), path(QStringLiteral("out.psd"))},
                                              genko::test::python_env(path(QStringLiteral("pyenv"))));
            QVERIFY2(ran.exit_code == 0, ran.err.constData());
            const Json view = genko::test::read_json(path(QStringLiteral("psd.json")));
            Json one, mixed, masked;
            for (const Json& layer : view["layers"]) {
                if (layer["name"] == Json("四角")) one = layer;
                if (layer["name"] == Json("混ぜ")) mixed = layer;
                if (layer["name"] == Json("マスク付き")) masked = layer;
            }
            QVERIFY2(one.is_object() && mixed.is_object() && masked.is_object(), view.dump().c_str());
            QCOMPARE(masked["kind"], Json("pixel"));
            QVERIFY(masked["vector"].get<bool>());
            QCOMPARE(one["kind"], Json("shape"));
            QVERIFY(one["soco"].get<bool>() && one["vstk"].get<bool>() && one["vogk"].get<bool>() && one["vector"].get<bool>());
            QCOMPARE(one["knots"], Json(4));
            QCOMPARE(mixed["kind"], Json("group"));
            std::vector<std::string> inside;
            for (const Json& child : mixed["children"]) inside.push_back(child["kind"].get<std::string>() + ":" + child["name"].get<std::string>());
            QVERIFY2(inside.size() == 3, view.dump().c_str());
            QCOMPARE(inside[0], std::string("shape:混ぜ・形 1"));
            QVERIFY2(inside[1].starts_with("pixel:"), inside[1].c_str());
            QCOMPARE(inside[2], std::string("shape:上の形"));
            QVERIFY(mixed["children"][2]["gdfl"].get<bool>());
        }
        // import_psd: each shape layer a pen layer of one part
        const QString other = make_book(QStringLiteral("again.genko"));
        reply = apply(other, QStringLiteral("i.json"),
                      Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", path(QStringLiteral("out.psd")).toStdString()}, {"fit", "paper"}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        const Json again = project(other);
        QVERIFY(has_feature(again, "pen-shapes@1"));
        int pens = 0;
        for (const Json& layer : again["pages"][0]["layers"]) {
            if (!layer.contains("shapes_blob")) continue;
            ++pens;
            QCOMPARE(layer.value("kind", std::string()), std::string("strokes"));
        }
        QCOMPARE(pens, 3);  // (四角, and the group's two shape layers)
    }

    // PSD: a pen layer written as a group (its parts and lines) carries on the group's record its mask with masks@1's
    // settings — not used (disabled), its density and feather — and its vector mask's density (the user's decision 3), as
    // a layer's record does; on an 8-bit RGB page and on a CMYK page alike (each a group of its parts and lines).
    void psdGroupMaskSettings() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        for (const bool cmyk : {false, true}) {
            const QString name = cmyk ? QStringLiteral("group-cmyk") : QStringLiteral("group-rgb");
            const QString book = make_book(name + QStringLiteral(".genko"));
            Json ops = Json::array();
            if (cmyk) ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
            for (const Json& op :
                 {Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "mixed"}, {"name", "混ぜ"}},
                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "mixed"}, {"panel_clip", false}, {"panel_each", false}},
                  Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "mixed"}, {"points", Json::array({Json::array({20, 100}), Json::array({150, 120})})}, {"width_mm", 1}},
                  Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "mixed"}, {"shape", Json{{"kind", "ellipse"}, {"rect", Json::array({30, 90, 40, 30})}}},
                       {"fill", black()}},
                  Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "mixed"}, {"over", true}, {"shape", rect_shape(80, 90, 30, 30)},
                       {"fill", Json{{"rgb", Json::array({200, 30, 30})}}}},
                  Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "mixed"}, {"area", Json{{"rect", Json::array({0, 0, 100, 297})}}}, {"density", 0.5},
                       {"feather_mm", 2}},
                  Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "mixed"}, {"enabled", false}},
                  Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "mixed"}, {"path", square_path(10, 80, 150)}, {"density", 50}}})
                ops.push_back(op);
            const Json reply = apply(book, name + QStringLiteral(".json"), ops);
            QVERIFY2(reply.value("ok", false), reply.dump().c_str());
            int exit = -1;
            const QString file = path(name + QStringLiteral(".psd"));
            const Json out = genko({QStringLiteral("export"), book, file, QStringLiteral("--format"), QStringLiteral("psd"), QStringLiteral("--dpi"),
                                    QStringLiteral("100"), QStringLiteral("--json")},
                                   &exit);
            QVERIFY2(exit == 0, out.dump().c_str());
            constexpr const char* kRead = R"PY(
import json, sys
from psd_tools import PSDImage
psd = PSDImage.open(sys.argv[2])
out = {"width": psd.width}
for l in psd.descendants():
    d = {"kind": l.kind, "vector": bool(l.has_vector_mask())}
    data = l._record.mask_data
    if data is not None:
        d["disabled"] = bool(data.flags.mask_disabled)
        d["relative"] = bool(data.flags.pos_relative_to_layer)
        if data.parameters is not None:
            p = data.parameters
            d.update({"user_density": p.user_mask_density, "user_feather": p.user_mask_feather, "vector_density": p.vector_mask_density})
    if l.kind == "group":
        d["children"] = [c.kind for c in l]
    out[l.name] = d
json.dump(out, open(sys.argv[1], "w"), ensure_ascii=False)
)PY";
            const QString json = file + QStringLiteral(".json");
            const auto ran = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), QString::fromUtf8(kRead), json, file},
                                              genko::test::python_env(path(QStringLiteral("pyenv"))));
            QVERIFY2(ran.exit_code == 0, ran.err.constData());
            const Json view = genko::test::read_json(json);
            QVERIFY2(view.contains("混ぜ"), view.dump().c_str());
            const Json& group = view["混ぜ"];
            QCOMPARE(group["kind"], Json("group"));
            QVERIFY2(group["children"].size() == 3 && group["children"][0] == Json("shape") && group["children"][2] == Json("shape"), view.dump().c_str());
            QVERIFY(group["vector"].get<bool>());
            // (not used; set_layer_mask's mask fixed to the page: not linked)
            QVERIFY2(group.value("disabled", false) && group.value("relative", false), group.dump().c_str());
            QCOMPARE(group["user_density"], Json(128));
            const double px_per_mm = view["width"].get<double>() / project(book)["spec"]["width_mm"].get<double>();
            QVERIFY2(std::fabs(group["user_feather"].get<double>() - 2 * px_per_mm) < 1e-6, group.dump().c_str());
            QCOMPARE(group["vector_density"], Json(128));
        }
    }
};

QTEST_GUILESS_MAIN(TestContractShapes)
#include "test_contract_shapes.moc"
