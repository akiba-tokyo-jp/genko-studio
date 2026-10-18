// vector@1 (PC3 C-a, SPEC.md COLOR-07「パス」「ベクトルマスク」, schema-v4.md「パス・ベクトル（vector@1）」): a page's paths and a
// layer's vector mask as a person or an agent uses them — set, appended, renamed (the work path saved), deleted (a
// clipping path naming it too); the path kinds of an area (path, path_name, shape_part, vector_mask, antialias) filling
// and making the selection; fill_area's blend into a paint layer's pixels; stroke_path's pen lines and the eraser along
// a path; make_work_path from an area and text_to_path from a line's letters; the vector mask drawn on 8-, 16- and 32-bit
// RGB, CMYK, Lab and grey pages (density, feather, inverted, off, reveal all, hide all), on a correction layer, merged
// and rasterized into the layer mask; saved, opened again, undone and redone through the command line, the feature
// named only while there are paths; a PSD's paths (2000…, 1025, 2999) and vmsk written and read back by psd-tools and by
// import_psd; `genko inspect --paths` and `genko export --format ai`. (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
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

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("pc3", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    return doc;
}

constexpr int kW = 472, kH = 630;  // (60 × 80 mm at 200 dpi)
const std::string kPaint = "paint0000001";  // the paint layer painted() adds

// The book with an opaque grey paint layer over its page (clear in its first 50 px).
core::Document painted() {
    core::Document doc = small_book();
    std::string bytes(std::size_t(kW) * kH * 4, '\0');
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (std::size_t(y) * kW + x) * 4;
            bytes[at] = bytes[at + 1] = bytes[at + 2] = static_cast<char>(100);
            bytes[at + 3] = static_cast<char>(x < 50 ? 0 : 255);
        }
    }
    core::Layer layer;
    layer.id = kPaint;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    layer.raster_png = std::make_shared<const std::string>(render::write_png(render::Image::frombytes("RGBA", render::Size{kW, kH}, bytes)));
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + id);
}

// [x, y, w, h] as a path's JSON (one closed subpath, its corners).
Json rect(double x, double y, double w, double h) {
    return Json{{"subpaths", Json::array({Json{{"closed", true},
                                               {"knots", Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + h}),
                                                                      Json::array({x, y + h})})}}})}};
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

// A fill's patch's covered area in its pixels (a "mask" patch's picture is its coverage, "L").
double covered_area(const core::Patch& patch) {
    if (patch.attrs.value("mode", std::string()) != "mask") throw std::runtime_error("not a mask patch");
    const render::Image a = render::open_image(*patch.png).convert("L");
    const std::string bytes = a.tobytes();
    double s = 0;
    for (const char c : bytes) s += static_cast<unsigned char>(c) / 255.0;
    return s;
}

}  // namespace

class TestContractPaths : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
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
    Json apply(const QString& book, const QString& name, const Json& ops, int* exit_out = nullptr) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit_out != nullptr) *exit_out = exit;
        else if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool has_feature(const Json& saved, const char* feature) {
        return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // set_path, rename_path, delete_path: the work path drawn and added to (op), saved under a name (the work path gone),
    // a saved path replaced and renamed (its clipping path following), deleted (the clipping path with it); what is not
    // there or not well formed refused, the book as it was.
    void pathsOfAPage() {
        core::Document doc = small_book();
        const core::ApplyResult made = run_ops(doc, Json::array({Json{{"op", "set_path"}, {"page", 1}, {"path", rect(10, 10, 20, 20)}}}));
        QCOMPARE(made.results.size(), std::size_t(1));
        QCOMPARE(made.results[0]["name"], Json("work"));
        doc = made.doc;
        QVERIFY(doc.page(0).work_path.has_value());
        doc = run(doc, Json{{"op", "set_path"}, {"page", 1}, {"name", "work"}, {"path", rect(15, 15, 5, 5)}, {"append", "subtract"}});
        QCOMPARE(doc.page(0).work_path->subpaths.size(), std::size_t(2));
        QCOMPARE(doc.page(0).work_path->subpaths[1].op, vec::PathOp::Subtract);
        // (the coverage: the square with a hole)
        const auto cover = render::path_raster::path_coverage(*doc.page(0).work_path, render::path_raster::PixelBox{0, 0, 60, 80});
        QCOMPARE(cover[std::size_t(12) * 60 + 12], 1.0f);
        QCOMPARE(cover[std::size_t(17) * 60 + 17], 0.0f);
        doc = run(doc, Json{{"op", "rename_path"}, {"page", 1}, {"to", "輪郭"}});
        QVERIFY(!doc.page(0).work_path.has_value());
        QCOMPARE(doc.page(0).paths.size(), std::size_t(1));
        QCOMPARE(doc.page(0).paths[0].name, std::string("輪郭"));
        doc = run(doc, Json{{"op", "set_path"}, {"page", 1}, {"name", "輪郭"}, {"path", rect(1, 1, 2, 2)}});
        QCOMPARE(doc.page(0).paths.size(), std::size_t(1));
        QCOMPARE(doc.page(0).paths[0].path.subpaths[0].knots[0].anchor.x, 1.0);
        doc.edit_page(0).clipping_path = vec::ClippingPath{"輪郭", 0.5};
        doc = run(doc, Json{{"op", "rename_path"}, {"page", 1}, {"name", "輪郭"}, {"to", " 線画 "}});
        QCOMPARE(doc.page(0).paths[0].name, std::string("線画"));
        QCOMPARE(doc.page(0).clipping_path->name, std::string("線画"));
        // refused
        QVERIFY(refusal(doc, Json{{"op", "delete_path"}, {"page", 1}}).find("the page has no work path") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "delete_path"}, {"page", 1}, {"name", "ない"}}).find("no path named 'ない'") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "rename_path"}, {"page", 1}, {"name", "線画"}, {"to", "work"}}).find("work path") != std::string::npos);
        QVERIFY(!refusal(doc, Json{{"op", "set_path"}, {"page", 1}, {"name", ""}, {"path", rect(0, 0, 1, 1)}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "set_path"}, {"page", 1}, {"path", Json{{"subpaths", Json::array({Json{{"knots", Json::array({Json::array({"a", 1})})}}})}}}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "set_path"}, {"page", 1}, {"path", rect(0, 0, 1, 1)}, {"append", "xor"}}).empty());
        doc = run(doc, Json{{"op", "set_path"}, {"page", 1}, {"name", "二つ目"}, {"path", rect(0, 0, 5, 5)}});
        QVERIFY(refusal(doc, Json{{"op", "rename_path"}, {"page", 1}, {"name", "二つ目"}, {"to", "線画"}}).find("already on the page") != std::string::npos);
        doc = run(doc, Json{{"op", "delete_path"}, {"page", 1}, {"name", "線画"}});
        QCOMPARE(doc.page(0).paths.size(), std::size_t(1));
        QVERIFY(!doc.page(0).clipping_path.has_value());
    }

    // The path kinds of an area: a path's exact coverage (grey at its edge, two-valued with antialias false), a saved or
    // the work path by its name, a shape part's path, a layer's vector mask's path; the modifiers (invert, feather) on them.
    void pathAreas() {
        core::Document doc = run(painted(), Json{{"op", "set_path"}, {"page", 1}, {"name", "四角"}, {"path", rect(10.1, 10.1, 20, 20)}});
        const core::Page& page = doc.page(0);
        const auto at = [](const render::Image& m, double x_mm, double y_mm) {
            return static_cast<int>(m.getpixel(static_cast<int>(x_mm / 25.4 * 200), static_cast<int>(y_mm / 25.4 * 200))[0]);
        };
        const render::Image by_path = render::selection::to_mask(Json{{"path", rect(10.1, 10.1, 20, 20)}}, page, &doc);
        QCOMPARE(at(by_path, 20, 20), 255);
        QCOMPARE(at(by_path, 5, 20), 0);
        const render::Image by_name = render::selection::to_mask(Json{{"path_name", "四角"}}, page, &doc);
        QCOMPARE(by_name.tobytes(), by_path.tobytes());
        // (the edge at 10.1 mm falls inside a pixel at 200 dpi: grey there, two-valued without antialias)
        const int edge_x = static_cast<int>(10.1 / 25.4 * 200);
        const int grey = static_cast<int>(by_path.getpixel(edge_x, 120)[0]);
        QVERIFY2(grey > 0 && grey < 255, std::to_string(grey).c_str());
        const render::Image hard = render::selection::to_mask(Json{{"path", rect(10.1, 10.1, 20, 20)}, {"antialias", false}}, page, &doc);
        const int two = static_cast<int>(hard.getpixel(edge_x, 120)[0]);
        QVERIFY(two == 0 || two == 255);
        const render::Image inverted = render::selection::to_mask(Json{{"path_name", "四角"}, {"invert", true}}, page, &doc);
        QCOMPARE(at(inverted, 20, 20), 0);
        const render::Image soft = render::selection::to_mask(Json{{"path_name", "四角"}, {"feather_mm", 2}}, page, &doc);
        QVERIFY(at(soft, 10.1, 20) > 30 && at(soft, 10.1, 20) < 225);
        QVERIFY(refusal(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"path_name", "ない"}}}})
                    .find("no path named") != std::string::npos);
        // (a vector mask's path; a shape part's)
        core::Document masked = run(doc, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path_name", "四角"}, {"density", 50}});
        const render::Image by_mask = render::selection::to_mask(Json{{"vector_mask", "paint0000001"}}, masked.page(0), &masked);
        QCOMPARE(by_mask.tobytes(), by_path.tobytes());  // (its path: not its density)
        vec::ShapePart part;
        part.id = "part1";
        part.path = vec::path_from_json(rect(30, 40, 10, 10));
        core::Document shaped = doc;
        for (core::Layer& layer : shaped.edit_page(0).layers) {
            if (layer.kind == core::LayerKind::Strokes) {
                layer.shapes = vec::make_shapes({part});
                const render::Image by_part =
                    render::selection::to_mask(Json{{"shape_part", Json{{"layer_id", layer.id}, {"id", "part1"}}}}, shaped.page(0), &shaped);
                QCOMPARE(at(by_part, 35, 45), 255);
                QCOMPARE(at(by_part, 20, 20), 0);
                break;
            }
        }
        // fill_area with a path (path.fill): a picture of the path's shape on the layer
        const core::Document filled =
            run(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"path_name", "四角"}}}, {"rgb", Json::array({200, 0, 0})}});
        const core::Layer& paint = layer_of(filled.page(0), kPaint);
        QCOMPARE(paint.patches.size(), std::size_t(1));
        const double expected = 20.0 / 25.4 * 300 * 20.0 / 25.4 * 300;  // (a fill's picture: 300 dpi)
        QVERIFY2(std::fabs(covered_area(paint.patches[0]) - expected) / expected < 0.01, std::to_string(covered_area(paint.patches[0])).c_str());
    }

    // fill_area's blend (Fill Path's mode): the paint layer's own pixels laid over by the colour through the blend, the
    // pixels outside the path as they were; normal as before (a picture); a pen layer refused; on precise and ink pages.
    void fillWithABlend() {
        const core::Document doc = run(painted(), Json{{"op", "set_path"}, {"page", 1}, {"path", rect(30, 10, 20, 20)}});
        const Json fill{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"path_name", "work"}}},
                        {"rgb", Json::array({255, 0, 0})}, {"blend", "multiply"}};
        const core::Document multiplied = run(doc, fill);
        const core::Layer& paint = layer_of(multiplied.page(0), kPaint);
        QVERIFY(paint.patches.empty());
        const render::Image pixels = render::open_image(*paint.raster_png).convert("RGBA");
        const auto inside = pixels.getpixel(static_cast<int>(40 / 25.4 * 200), static_cast<int>(20 / 25.4 * 200));
        const auto outside = pixels.getpixel(static_cast<int>(40 / 25.4 * 200), static_cast<int>(60 / 25.4 * 200));
        QCOMPARE(static_cast<int>(inside[0]), 100);  // (100 × 255/255: red multiplied keeps the red)
        QCOMPARE(static_cast<int>(inside[1]), 0);
        QCOMPARE(static_cast<int>(outside[0]), 100);
        QCOMPARE(static_cast<int>(outside[1]), 100);
        const core::Document plain = run(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"path_name", "work"}}},
                                                   {"rgb", Json::array({255, 0, 0})}, {"blend", "normal"}});
        QCOMPARE(layer_of(plain.page(0), "paint0000001").patches.size(), std::size_t(1));
        const std::string ink = doc.page(0).first_layer(core::LayerRole::Ink)->id;
        Json on_pen = fill;
        on_pen["layer_id"] = ink;
        QVERIFY(refusal(doc, on_pen).find("paint layer") != std::string::npos);
        Json no_blend = fill;
        no_blend["blend"] = "sparkle";
        QVERIFY(!refusal(doc, no_blend).empty());
        // (a 16-bit page and a CMYK page: their pixels too, darker inside)
        for (const char* mode : {"rgb16", "cmyk"}) {
            core::Document book = doc;
            book = run(book, std::string(mode) == "rgb16" ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}
                                                         : Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
            Json dark = fill;
            dark["rgb"] = Json::array({0, 0, 0});
            const core::Document after = run(book, dark);
            const render::Image seen = drawn(after);
            QVERIFY2(grey_at(seen, 40, 20) + 40 < grey_at(seen, 40, 60), mode);
        }
    }

    // stroke_path: each subpath a pen line along it (closed: back to its start), the brush, the pencil, the eraser; with
    // simulate_pressure thin at its ends; on a paint layer too.
    void strokeAPath() {
        core::Document doc = run(small_book(), Json{{"op", "set_path"}, {"page", 1}, {"path", rect(10, 10, 20, 20)}});
        doc = run(doc, Json{{"op", "set_path"}, {"page", 1}, {"name", "線"},
                            {"path", Json{{"subpaths", Json::array({Json{{"closed", false},
                                                                         {"knots", Json::array({Json{{"anchor", Json::array({10, 50})}, {"in", Json::array({10, 50})},
                                                                                                     {"out", Json::array({20, 40})}},
                                                                                                Json::array({40, 50})})}}})}}}});
        const std::string ink = doc.page(0).first_layer(core::LayerRole::Ink)->id;
        const std::size_t before = doc.page(0).first_layer(core::LayerRole::Ink)->stroke_count();
        const core::ApplyResult stroked = run_ops(doc, Json::array({Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", ink}, {"width_mm", 0.5}}}));
        QCOMPARE(stroked.results[0]["strokes"], Json(1));
        const core::Layer& layer = layer_of(stroked.doc.page(0), ink);
        QCOMPARE(layer.stroke_count(), before + 1);
        const core::Stroke& line = *layer.strokes->items.back();
        QCOMPARE(line.points.front().x, line.points.back().x);  // (closed: back to its start)
        QCOMPARE(line.points.front().y, line.points.back().y);
        QCOMPARE(line.width_mm, 0.5);
        const core::Document pencil = run(doc, Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", ink}, {"name", "線"}, {"tool", "pencil"}, {"simulate_pressure", true}});
        const core::Stroke& curve = *layer_of(pencil.page(0), ink).strokes->items.back();
        QCOMPARE(curve.kind, std::string("pencil"));
        QVERIFY(curve.points.size() > 10);  // (the curve flattened)
        QVERIFY(!curve.pressure.empty() && curve.pressure.front() < curve.pressure[curve.pressure.size() / 2]);  // (thin at its start)
        // (the eraser along the square: the line drawn along it cut away)
        const core::Document erased = run(stroked.doc, Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", ink}, {"tool", "eraser"}, {"width_mm", 3}});
        QCOMPARE(layer_of(erased.page(0), ink).stroke_count(), before);
        // (a paint layer takes the line too)
        const core::Document on_paint = run(run(painted(), Json{{"op", "set_path"}, {"page", 1}, {"path", rect(10, 10, 20, 20)}}),
                                            Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", "paint0000001"}});
        QCOMPARE(layer_of(on_paint.page(0), "paint0000001").stroke_count(), std::size_t(1));
        QVERIFY(refusal(doc, Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", ink}, {"tool", "airbrush"}}).find("brush, pencil or eraser") != std::string::npos);
        QVERIFY(refusal(small_book(), Json{{"op", "stroke_path"}, {"page", 1}, {"layer_id", ink}}).find("no work path") != std::string::npos);
    }

    // make_work_path: an area's outline as curves (a rectangle's corners corners, its area kept), the tolerance, an empty
    // area refused; text_to_path: a placed line's letters as the work path, what is not a letter named in the notes.
    void workPaths() {
        const core::Document doc = small_book();
        const Json area{{"rect", Json::array({10, 10, 30, 20})}};
        const core::ApplyResult made = run_ops(doc, Json::array({Json{{"op", "make_work_path"}, {"page", 1}, {"area", area}}}));
        QCOMPARE(made.results[0]["subpaths"], Json(1));
        const vec::Path& path = *made.doc.page(0).work_path;
        QVERIFY(std::count_if(path.subpaths[0].knots.begin(), path.subpaths[0].knots.end(), [](const vec::Knot& k) { return !k.smooth; }) >= 4);
        const auto cover = render::path_raster::path_coverage(render::path_raster::to_pixels(path, 25.4), render::path_raster::PixelBox{0, 0, 60, 80});
        double total = 0;
        for (const float v : cover) total += v;
        // (the area as the selection draws it at 200 dpi — a rectangle without antialias takes the pixels its edge touches —
        // within 1 %; about 30 × 20 mm)
        const render::Image drawn_area = render::selection::to_mask(area, doc.page(0), &doc, render::selection::kSelDpi);
        double expected = 0;
        for (const char v : drawn_area.tobytes()) expected += static_cast<unsigned char>(v) / 255.0;
        expected *= (25.4 / render::selection::kSelDpi) * (25.4 / render::selection::kSelDpi);
        QVERIFY2(std::fabs(total - expected) / expected < 0.01, (std::to_string(total) + " " + std::to_string(expected)).c_str());
        QVERIFY2(std::fabs(total - 600) / 600 < 0.02, std::to_string(total).c_str());
        const core::ApplyResult ring = run_ops(doc, Json::array({Json{{"op", "make_work_path"}, {"page", 1}, {"tolerance_mm", 1.0},
                                                                      {"area", Json{{"subtract", Json::array({Json{{"ellipse", Json::array({5, 5, 40, 40})}},
                                                                                                             Json{{"ellipse", Json::array({15, 15, 20, 20})}}})}}}}}));
        QCOMPARE(ring.results[0]["subpaths"], Json(2));
        QCOMPARE(ring.doc.page(0).work_path->subpaths[1].op, vec::PathOp::Exclude);
        QVERIFY(!refusal(doc, Json{{"op", "make_work_path"}, {"page", 1}, {"area", Json{{"rect", Json::array({100, 100, 5, 5})}}}}).empty());
        // text_to_path
        core::Document lettered = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "t1"}, {"text", "台詞"}, {"x_mm", 15}, {"y_mm", 15}});
        const core::ApplyResult letters = run_ops(lettered, Json::array({Json{{"op", "text_to_path"}, {"id", "t1"}}}));
        QVERIFY(letters.results[0]["subpaths"].get<int>() >= 2);
        QVERIFY(letters.results[0]["notes"].dump().find("balloon") != std::string::npos);
        const vec::Path& glyphs = *letters.doc.page(0).work_path;
        const auto box = glyphs.control_bounds();
        QVERIFY(box && (*box)[0] > 10 && (*box)[2] < 60);  // (where the letters are on the page)
        QCOMPARE(refusal(lettered, Json{{"op", "text_to_path"}, {"id", "t1"}, {"to", "shape"}}).substr(0, 14), std::string("not_yet_ported"));
        QVERIFY(!refusal(lettered, Json{{"op", "text_to_path"}, {"id", "ない"}}).empty());
    }

    // set_vector_mask: the layer shows only inside its path, on every kind of page; its density lets the outside half show,
    // its feather softens the edge, inverted it hides the inside, off it hides nothing; reveal all, hide all; deleted; on a
    // correction layer; merged; rasterized into the layer mask (the look the same); refused on a folder and without a mask.
    void vectorMaskDrawn() {
        const Json half = rect(0, 0, 30, 80);
        for (const char* mode : {"rgb8", "rgb16", "rgb32", "cmyk", "lab", "gray"}) {
            core::Document book = painted();
            const std::string m = mode;
            if (m == "rgb16" || m == "rgb32") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", m == "rgb16" ? 16 : 32}});
            if (m == "cmyk") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
            if (m == "lab") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}});
            if (m == "gray") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
            const Json mask{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path", half}};
            const core::Document masked = run(book, mask);
            const render::Image whole = drawn(book), seen = drawn(masked);
            const int paper = grey_at(seen, 45, 60), shown = grey_at(seen, 20, 60);
            QVERIFY2(paper > shown + 60, (m + " " + std::to_string(paper) + " " + std::to_string(shown)).c_str());
            QVERIFY2(std::abs(shown - grey_at(whole, 20, 60)) <= 1, mode);
            Json dense = mask;
            dense["density"] = 50;
            const int mid = grey_at(drawn(run(book, dense)), 45, 60);
            QVERIFY2(mid > shown + 10 && mid < paper - 10, (m + " " + std::to_string(mid)).c_str());
            Json soft = mask;
            soft["feather_mm"] = 4;
            const int edge = grey_at(drawn(run(book, soft)), 30, 60);
            QVERIFY2(edge > shown + 10 && edge < paper - 10, (m + " " + std::to_string(edge)).c_str());
            const core::Document inverted = run(masked, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"invert", true}});
            QVERIFY2(grey_at(drawn(inverted), 20, 60) > grey_at(drawn(inverted), 45, 60) + 60, mode);
            const core::Document off = run(masked, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"enabled", false}});
            QVERIFY2(std::abs(grey_at(drawn(off), 45, 60) - grey_at(whole, 45, 60)) <= 1, mode);
        }
        const core::Document book = painted();
        const core::Document hidden = run(book, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"hide_all", true}});
        QVERIFY(grey_at(drawn(hidden), 20, 60) > 240);
        QVERIFY(layer_of(hidden.page(0), "paint0000001").vector_mask->path.empty());
        const core::Document revealed = run(hidden, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"reveal_all", true}});
        QCOMPARE(drawn(revealed).tobytes(), drawn(book).tobytes());
        const core::Document gone = run(revealed, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"delete", true}});
        QVERIFY(!layer_of(gone.page(0), "paint0000001").vector_mask.has_value());
        QVERIFY(refusal(book, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"density", 50}}).find("no vector mask") != std::string::npos);
        QVERIFY(!refusal(book, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path", half}, {"hide_all", true}}).empty());
        QVERIFY(!refusal(book, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path", half}, {"density", 150}}).empty());
        const core::ApplyResult reported = run_ops(book, Json::array({Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path", half},
                                                                           {"linked", false}, {"feather_mm", 1.5}}}));
        QCOMPARE(reported.results[0]["vector_mask"]["linked"], Json(false));
        QCOMPARE(reported.results[0]["vector_mask"]["feather_mm"], Json(1.5));
        // rasterized into the layer mask: the look the same, the vector mask gone
        const core::Document masked = run(book, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "paint0000001"}, {"path", half}});
        const core::ApplyResult raster = run_ops(masked, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "paint0000001"}, {"vector_mask", true}}}));
        const core::Layer& flat = layer_of(raster.doc.page(0), kPaint);
        QVERIFY(!flat.vector_mask.has_value() && flat.mask.has_value());
        QVERIFY(std::abs(grey_at(drawn(raster.doc), 20, 60) - grey_at(drawn(masked), 20, 60)) <= 2);
        QVERIFY(std::abs(grey_at(drawn(raster.doc), 45, 60) - grey_at(drawn(masked), 45, 60)) <= 2);
        QVERIFY(refusal(book, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "paint0000001"}, {"vector_mask", true}}).find("no vector mask") != std::string::npos);
        // merged down onto a pen layer below: the masked look drawn in, no vector mask left
        core::Document stacked = masked;
        {
            core::Layer under;  // (an empty paint layer under it)
            under.id = "under000001";
            under.kind = core::LayerKind::Raster;
            under.panel_clip = false;
            auto& layers = stacked.edit_page(0).layers;
            layers.insert(layers.end() - 1, std::move(under));
        }
        const core::Document merged = run(stacked, Json{{"op", "merge_down"}, {"page", 1}, {"id", "paint0000001"}});
        for (const core::Layer& layer : merged.page(0).layers) QVERIFY(!layer.vector_mask.has_value());
        QVERIFY(grey_at(drawn(merged), 45, 60) > grey_at(drawn(merged), 20, 60) + 60);
        // a correction layer: its change only inside its vector mask
        core::Document corrected = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"id", "adj1"}, {"kind", "adjust"}, {"adjust", Json{{"kind", "invert"}}}});
        const int both_before = grey_at(drawn(corrected), 45, 60);
        corrected = run(corrected, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "adj1"}, {"path", half}});
        QVERIFY(std::abs(grey_at(drawn(corrected), 45, 60) - grey_at(drawn(book), 45, 60)) <= 1);  // (outside: not inverted)
        QVERIFY(std::abs(grey_at(drawn(corrected), 20, 60) - both_before) > 60 || grey_at(drawn(corrected), 20, 60) > 140);
        core::Document foldered = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"id", "f1"}, {"kind", "folder"}});
        QVERIFY(refusal(foldered, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", "f1"}, {"reveal_all", true}}).find("folder") != std::string::npos);
    }

    // Through the command line: the book saved with its paths and vector mask (vector@1 named), opened again the same,
    // undone and redone; a book without them names no feature; inspect --paths; export --format ai.
    void savedAndUndone() {
        const QString book = make_book(QStringLiteral("paths.genko"));
        QVERIFY(!has_feature(project(book), "vector@1"));
        Json saved = project(book);
        std::string layer_id;
        for (const Json& layer : saved["pages"][0]["layers"]) {
            if (layer.value("kind", std::string()) == "strokes" || layer.value("role", std::string()) == "ink") {
                layer_id = layer["id"].get<std::string>();
                break;
            }
        }
        QVERIFY(!layer_id.empty());
        Json reply = apply(book, QStringLiteral("p1.json"),
                           Json::array({Json{{"op", "set_path"}, {"page", 1}, {"name", "輪郭"}, {"path", rect(20, 20, 40, 30)}},
                                        Json{{"op", "set_path"}, {"page", 1}, {"path", rect(30, 30, 10, 10)}},
                                        Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", layer_id}, {"path_name", "輪郭"}, {"feather_mm", 1}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        saved = project(book);
        QVERIFY(has_feature(saved, "vector@1"));
        QCOMPARE(saved["pages"][0]["paths"][0]["name"], Json("輪郭"));
        QVERIFY(saved["pages"][0].contains("work_path"));
        Json inspected = genko({QStringLiteral("inspect"), book, QStringLiteral("--paths"), QStringLiteral("--page"), QStringLiteral("1")});
        QCOMPARE(inspected["paths"][0]["name"], Json("輪郭"));
        QCOMPARE(inspected["paths"][0]["knots"], Json(4));
        QCOMPARE(inspected["work_path"]["subpaths"], Json(1));
        QCOMPARE(inspected["layers"][0]["id"], Json(layer_id));
        QCOMPARE(inspected["layers"][0]["vector_mask"]["feather_mm"], Json(1));
        const Json all = genko({QStringLiteral("inspect"), book, QStringLiteral("--paths")});
        QCOMPARE(all["pages"].size(), std::size_t(1));
        // Illustrator
        int exit = -1;
        const Json ai = genko({QStringLiteral("export"), book, path(QStringLiteral("paths.ai")), QStringLiteral("--format"), QStringLiteral("ai"),
                               QStringLiteral("--json")},
                              &exit);
        QVERIFY2(exit == 0, ai.dump().c_str());
        QCOMPARE(ai["paths"], Json(2));
        const std::string text = genko::test::read_bytes(path(QStringLiteral("paths.ai")));
        QVERIFY(text.starts_with("%!PS-Adobe-2.0 EPSF-1.2\n"));
        QVERIFY(text.find("%%Note: 輪郭\n") != std::string::npos && text.find(" m\n") != std::string::npos && text.find("%%EOF") != std::string::npos);
        genko({QStringLiteral("export"), book, path(QStringLiteral("none.ai")), QStringLiteral("--format"), QStringLiteral("ai"), QStringLiteral("--paths"),
               QStringLiteral("ない")},
              &exit);
        QCOMPARE(exit, 1);
        // undone and redone
        genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        saved = project(book);
        QVERIFY(!has_feature(saved, "vector@1"));
        QVERIFY(!saved["pages"][0].contains("paths"));
        genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        saved = project(book);
        QVERIFY(has_feature(saved, "vector@1"));
        QCOMPARE(saved["pages"][0]["paths"][0]["name"], Json("輪郭"));
    }

    // PSD (the user's decision 3: always written and read): the saved paths as resources 2000…, the work path 1025, a
    // layer's vector mask as its vmsk — psd-tools reads them —, and import_psd reading them back onto a page.
    void psdPathsAndMasks() {
        const QString book = make_book(QStringLiteral("psd.genko"));
        Json saved = project(book);
        const std::string layer_id = "paintpsd0001";  // (a paint layer: the PSD's layers are its pixel layers)
        Json reply = apply(book, QStringLiteral("p.json"),
                           Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"id", layer_id}, {"kind", "paint"}, {"panel_each", false}},
                                        Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer_id}, {"area", Json{{"rect", Json::array({5, 5, 150, 200})}}},
                                             {"rgb", Json::array({0, 0, 0})}, {"blend", "multiply"}},
                                        Json{{"op", "set_path"}, {"page", 1}, {"name", "輪郭"}, {"path", rect(20, 20, 40, 30)}},
                                        Json{{"op", "set_path"}, {"page", 1}, {"path", rect(30, 30, 10, 10)}},
                                        Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", layer_id}, {"path", rect(10, 10, 100, 120)}, {"linked", false}}}));
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
psd = PSDImage.open(sys.argv[2])
res = psd.image_resources
paths = {}
for key in range(2000, 2998):
    if key in res:
        paths[str(key)] = res[key].name if hasattr(res[key], "name") else ""
masks = []
for layer in psd.descendants():
    if layer.has_vector_mask():
        vm = layer.vector_mask
        knots = [[k.anchor[1], k.anchor[0]] for sub in vm.paths for k in sub]
        masks.append({"name": layer.name, "inverted": vm.inverted, "not_linked": vm.not_linked, "disabled": vm.disabled, "knots": knots})
json.dump({"paths": paths, "work": 1025 in res, "masks": masks, "size": [psd.width, psd.height]}, open(sys.argv[1], "w"), ensure_ascii=False)
)PY";
            const auto ran = genko::test::run(genko::test::python_ref(),
                                              {QStringLiteral("-c"), QString::fromUtf8(kRead), path(QStringLiteral("psd.json")), path(QStringLiteral("out.psd"))},
                                              genko::test::python_env(path(QStringLiteral("pyenv"))));
            QVERIFY2(ran.exit_code == 0, ran.err.constData());
            const Json view = genko::test::read_json(path(QStringLiteral("psd.json")));
            QVERIFY2(view["paths"].contains("2000"), view.dump().c_str());
            QVERIFY(view["work"].get<bool>());
            QCOMPARE(view["masks"].size(), std::size_t(1));
            QVERIFY(view["masks"][0]["not_linked"].get<bool>());
            QVERIFY(!view["masks"][0]["disabled"].get<bool>());
            // (the mask's corners as fractions of the canvas: 10 mm of the page's width)
            const double w_mm = saved["spec"]["width_mm"].get<double>();
            QVERIFY2(std::fabs(view["masks"][0]["knots"][0][0].get<double>() - 10 / w_mm) < 1e-4, view.dump().c_str());
        }
        // import_psd: the layer's vector mask and the page's paths back
        const QString other = make_book(QStringLiteral("again.genko"));
        reply = apply(other, QStringLiteral("i.json"),
                      Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", path(QStringLiteral("out.psd")).toStdString()}, {"fit", "paper"}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        const Json again = project(other);
        QVERIFY(has_feature(again, "vector@1"));
        bool found = false;
        for (const Json& layer : again["pages"][0]["layers"]) {
            if (!layer.contains("vector_mask")) continue;
            found = true;
            const Json& knots = layer["vector_mask"]["path"]["subpaths"][0]["knots"];
            QVERIFY2(std::fabs(knots[0][0].get<double>() - 10) < 0.05 && std::fabs(knots[2][1].get<double>() - 130) < 0.05, layer.dump().c_str());
            QCOMPARE(layer["vector_mask"].value("linked", true), false);
        }
        QVERIFY(found);
        QCOMPARE(again["pages"][0]["paths"][0]["name"], Json("輪郭"));
        QVERIFY(again["pages"][0].contains("work_path"));
    }
};

QTEST_GUILESS_MAIN(TestContractPaths)
#include "test_contract_paths.moc"
