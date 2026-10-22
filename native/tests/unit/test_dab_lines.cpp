// PC4 E1 (brushes@1, SPEC.md COLOR-09「ブラシのダブの描き方」): the line ops with a dab brush, without Python —
// define_brush's dabs (Genko's and upstream's settings, the pictures given as files taken in and kept by the book, what is
// refused), add_stroke's inputs (tilt, time, wheel stretched over the points kept; the flow; the 背景色 of the colour
// dynamics) and its line drawn — the same pixels drawn whole or in parts, again after saving and opening, after Undo and
// Redo —, its blend painted into a paint layer's pixels on 8-bit, 16-bit, 32-bit, CMYK, Lab and grey pages (refused on a pen
// layer; normal keeps a line as before), erase's brush and block, set_brush's kind, 背景色 and reset, 下地混色 on every kind
// of page, brush.get (`genko brushes get`); and every brush that is not a dab brush drawn as before (its pixels the same
// with the new keys of draw given or not).

#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::Box;
using render::Size;
using render::image_ops::Pixels;
namespace fs = std::filesystem;
namespace brushes = render::brushes;

namespace {

constexpr double kInch = 25.4;
constexpr int kSide = 400;  // (2 inches at the working 200 dpi)

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of one page (2 × 2 inches) with its pen layers, no panels.
core::Document pen_book() {
    core::Document doc = core::new_episode("ダブ", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    doc.edit_page(0).numero = false;
    return doc;
}

// A page of one 8-bit paint layer "paint", opaque light grey but a clear corner; its pixels `side` across the page (400:
// the working 200 dpi; 600: a layer of 300 dpi, as a picture opened on its own grid is).
core::Document paint_book(int side = kSide) {
    core::Document doc = pen_book();
    auto& p = doc.edit_page(0);
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    std::string bytes;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            for (int c = 0; c < 3; ++c) bytes.push_back(static_cast<char>(200));
            bytes.push_back(static_cast<char>(x < 20 && y < 20 ? 0 : 255));
        }
    }
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", Size{side, side}, bytes));
    p.layers.push_back(layer);
    return doc;
}

core::Document as_kind(const core::Document& doc, const std::string& kind) {
    if (kind == "8") return doc;
    if (kind == "16" || kind == "32") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", kind == "16" ? 16 : 32}});
    if (kind == "cmyk") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    if (kind == "lab") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}});
    return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
}

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> all{"8", "16", "32", "cmyk", "lab", "gray"};
    return all;
}

Pixels whole(core::Document& doc, const std::string& id = "paint") {
    const Json op = Json::object();
    const core::Actor actor("human:test");
    core::OpContext c{doc, op, actor};
    render::image_ops::Window window;
    (void)render::image_ops::layer_pixels(c, 0, id, Box{0, 0, 1, 1}, &window);
    return render::image_ops::layer_pixels(c, 0, id, Box{0, 0, window.grid.width, window.grid.height});
}

// A dab brush defined: soft, scattered a little, with a texture (procedural) — every part of the engine.
Json define_dab(const std::string& key = "my_dab", Json dabs = Json::parse(R"({"hardness": 0.4, "spacing": 0.15,
            "scatter": {"enabled": true, "scatter": {"jitter": 0.5}}, "texture": {"enabled": true, "depth": 0.4}, "seed": 5})")) {
    return Json{{"op", "define_brush"}, {"key", key}, {"label", "ダブ"}, {"engine", "dabs"}, {"dabs", std::move(dabs)}};
}

Json stroke(const std::string& kind, const std::string& layer = "ink") {
    return Json{{"op", "add_stroke"}, {"page", 1}, {"layer", layer}, {"kind", kind}, {"width_mm", 3.0}, {"stabilize", 0}, {"taper", false},
                {"points", Json::parse("[[8, 25, 0.5], [25, 30, 1.0], [42, 22, 0.8]]")}};
}

const core::Layer& ink(const core::Document& doc) { return *doc.page(0).first_layer(core::LayerRole::Ink); }

render::Image ink_picture(const core::Document& doc, int dpi = 200) {
    brushes::register_book(doc);
    return render::layer_image(doc.page(0), ink(doc), dpi, &doc);
}

int alpha_at(const render::Image& rgba, int x, int y) { return static_cast<int>(rgba.getpixel(x, y).back()); }

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

void save_next(const core::Document& doc, const fs::path& dir, const Json& ops) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = storage::read_disk_state(dir).revision;
    request.ops = ops;
    storage::Saver(lock).save(doc, request);
}

std::string grey_png(int w, int h, int value) {
    return render::write_png(render::Image::frombytes("L", Size{w, h}, std::string(std::size_t(w) * h, static_cast<char>(value))));
}

}  // namespace

class TestDabLines : public QObject {
    Q_OBJECT

private slots:
    void init() {
        brushes::clear_custom();
        render::dabs::clear_pictures();
    }

    // define_brush: a dab brush's settings (only those that differ kept), its pictures given as files taken in (a tip's
    // ink, a texture's lightness) and kept by the book; a picture the book keeps named by its ref; upstream's settings
    // read, what is not the brush's here said; what cannot be refused (and nothing changed); "engine": null back to
    // drawing as its base does.
    void defineBrushTakesDabs() {
        const std::string tip_file = core::b64encode(render::write_png(render::Image::create("RGBA", Size{24, 16}, render::Ink{0, 0, 0, 255})));
        const std::string tile_file = core::b64encode(grey_png(32, 32, 90));
        Json dabs = Json::parse(R"({"hardness": 0.5, "texture": {"enabled": true}, "dual": {"enabled": true}})");
        dabs["tip"] = Json{{"png", tip_file}};
        dabs["texture"]["pattern"] = Json{{"png", tile_file}};
        dabs["dual"]["tip"] = Json{{"png", tip_file}};
        const core::Document doc = run(pen_book(), define_dab("my_pic", dabs));
        const Json& kept = doc.brush_custom["my_pic"];
        QCOMPARE(kept["engine"], Json("dabs"));
        const core::DabSettings s = core::dabs_from_json(kept["dabs"]);
        QCOMPARE(s.hardness, 0.5);
        QVERIFY(!s.tip.empty() && s.tip == s.dual.tip && !s.texture.pattern.asset.empty());
        QCOMPARE(doc.brush_tips.size(), std::size_t(2));
        QVERIFY(doc.brush_tips.contains(s.tip) && doc.brush_tips.contains(s.texture.pattern.asset));
        QVERIFY(core::uses_brushes(doc));
        QVERIFY_THROWS_NO_EXCEPTION(core::validate_brushes(doc));
        // named by its ref, once the book keeps it
        Json again = Json::parse(R"({"tip": {"asset": ""}})");
        again["tip"]["asset"] = s.tip;
        const core::Document named = run(doc, define_dab("my_named", again));
        QCOMPARE(core::dabs_from_json(named.brush_custom["my_named"]["dabs"]).tip, s.tip);
        Json missing = Json::parse(R"({"tip": {"asset": ""}})");
        missing["tip"]["asset"] = "sha256:" + std::string(64, 'a');
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_m", missing)})).find("the book has no brush picture") != std::string::npos);
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_x", Json::parse(R"({"nope": 1})"))})).find("nope") != std::string::npos);
        Json engine = define_dab("my_e");
        engine["engine"] = "ink";
        QVERIFY(refusal(pen_book(), Json::array({engine})).find("engine must be dabs") != std::string::npos);
        Json broken = Json::parse(R"({"tip": {"png": "bm90IGEgcGljdHVyZQ=="}})");
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_b", broken)})).find("tip's picture") != std::string::npos);
        // upstream's settings ("format": "photocraft"): read, and what is the line's here said
        Json up = Json{{"op", "define_brush"}, {"key", "my_up"}, {"label", "上流"}, {"engine", "dabs"}, {"format", "photocraft"},
                       {"dabs", Json::parse(R"({"size": 30, "hardness": 0.2, "color": [1, 0, 0, 1], "tip": {"sampled": {"width": 2, "height": 2, "data": [0, 1, 1, 0]}}})")}};
        const auto result = run_ops(pen_book(), Json::array({up}));
        const core::DabSettings us = core::dabs_from_json(result.doc.brush_custom["my_up"]["dabs"]);
        QVERIFY(std::abs(us.size_mm - 30 * 25.4 / 200) < 1e-12);
        QCOMPARE(result.doc.brush_tips.size(), std::size_t(1));
        QVERIFY(result.warnings.dump().find("color") != std::string::npos);
        // engine null: draws as its base does
        Json plain = Json{{"op", "define_brush"}, {"key", "my_pic"}, {"label", "ふつう"}, {"engine", nullptr}};
        const core::Document back = run(doc, plain);
        QVERIFY(!back.brush_custom["my_pic"].contains("engine"));
        // the keys are this build's own: never said to be unknown
        QVERIFY(run_ops(pen_book(), Json::array({define_dab()})).warnings.dump().find("unknown") == std::string::npos);
    }

    // add_stroke with a dab brush: the inputs stretched over the points it keeps, the flow, the line drawn (soft: values
    // between 0 and 255), the same drawn again and in parts (a region of the page), refused past its dabs' limit.
    void aDabLineIsALine() {
        core::Document doc = run(pen_book(), define_dab());
        Json op = stroke("my_dab");
        op["tilt"] = Json::parse("[[0, 0], [30, -20], [60, 10]]");
        op["time_ms"] = Json::parse("[0, 40, 90]");
        op["wheel"] = Json::parse("[1, 0.5, 0.25]");
        op["flow"] = 0.6;
        const auto result = run_ops(doc, Json::array({op}));
        QVERIFY(result.warnings.dump().find("unknown") == std::string::npos);
        doc = result.doc;
        const core::Stroke& line = *ink(doc).strokes->items.back();
        QCOMPARE(line.kind, std::string("my_dab"));
        QCOMPARE(line.tilt.size(), 2 * line.points.size());
        QCOMPARE(line.time_ms.size(), line.points.size());
        QCOMPARE(line.wheel.size(), line.points.size());
        QCOMPARE(line.time_ms.front(), 0.0);
        QCOMPARE(line.time_ms.back(), 90.0);
        QCOMPARE(*line.flow, 0.6);
        QVERIFY(core::uses_brushes(doc));
        const render::Image a = ink_picture(doc);
        int partial = 0, full = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const int v = alpha_at(a, x, y);
                partial += v > 0 && v < 255 ? 1 : 0;
                full += v == 255 ? 1 : 0;
            }
        }
        QVERIFY2(partial > 200, qPrintable(QString::number(partial)));
        QCOMPARE(a.tobytes(), ink_picture(doc).tobytes());
        // the page drawn in a part: the same pixels as the whole there
        brushes::register_book(doc);
        render::RenderOptions whole_options;
        const render::Image page = render::render_page(doc.page(0), 200, whole_options, &doc).image;
        render::RenderOptions part_options;
        part_options.region = render::RenderRegion{100, 150, 120, 90};
        const render::Image part = render::render_page(doc.page(0), 200, part_options, &doc).image;
        QCOMPARE(part.tobytes(), page.crop(Box{100, 150, 220, 240}).tobytes());
        // inputs that are not one a point: refused
        Json bad = stroke("my_dab");
        bad["tilt"] = Json::parse("[[0, 0]]");
        QVERIFY(refusal(doc, Json::array({bad})).find("tilt is") != std::string::npos);
        bad = stroke("my_dab");
        bad["flow"] = 2;
        QVERIFY(refusal(doc, Json::array({bad})).find("flow is") != std::string::npos);
        // too many dabs: refused, nothing kept
        Json dense = define_dab("my_dense", Json::parse(R"({"spacing": 0.01})"));
        Json long_line = stroke("my_dense");
        long_line["width_mm"] = 0.01;
        // (the steps are at least 1/400 inch: 320 times across the page, 16 m of line, are 250,000 of them)
        Json zigzag = Json::array();
        for (int i = 0; i < 320; ++i) zigzag.push_back(Json::array({i % 2 == 0 ? 0 : 50, 1 + i % 40}));
        long_line["points"] = zigzag;
        QVERIFY(refusal(doc, Json::array({dense, long_line})).find("dabs") != std::string::npos);
        // an erasing brush does not draw a line
        QVERIFY(refusal(doc, Json::array({define_dab("my_erasing", Json::parse(R"({"erase": true})")), stroke("my_erasing")}))
                    .find("erases") != std::string::npos);
    }

    // Every brush that is not a dab brush draws its line exactly as before: the new arguments of draw (a line's inputs,
    // a part of the page) change nothing of its pixels, and a book without dab brushes and inputs is not brushes@1.
    void everyOtherBrushDrawsAsBefore() {
        const core::PenPoints points{{3, 4, 0.3}, {10, 12, 0.9}, {18, 6, 0.6}, {25, 14, 1.0}};
        const std::vector<double> tilt{1, 2, 3, 4, 5, 6, 7, 8};
        render::dabs::LineInputs inputs;
        inputs.tilt = tilt;
        inputs.flow = 0.2;
        for (const core::Brush& b : core::builtin_brushes()) {
            const auto plain = brushes::draw(Size{300, 300}, points, 200, 1.5, b.key, "seed-1", {}, 0.3);
            const auto given = brushes::draw(Size{300, 300}, points, 200, 1.5, b.key, "seed-1", {}, 0.3, &inputs, Box{0, 0, 50, 50});
            QCOMPARE(plain.has_value(), given.has_value());
            if (!plain) continue;
            QVERIFY2(plain->origin == given->origin && plain->mask.tobytes() == given->mask.tobytes(), b.key.c_str());
            QVERIFY(!given->dabs);
            QCOMPARE(brushes::extent(Size{300, 300}, points, 200, 1.5, b.key), brushes::extent(Size{300, 300}, points, 200, 1.5, b.key));
        }
        core::Document doc = run(pen_book(), stroke("gpen"));
        QVERIFY(!core::uses_brushes(doc));
    }

    // The colour dynamics: the line keeps the 背景色 it mixes towards (the op's, else the book's), and its pixels carry
    // colours between the two.
    void colourDynamicsKeepTheirBackground() {
        core::Document doc = run(pen_book(), Json{{"op", "set_brush"}, {"background_rgb", Json::array({0, 0, 255})}});
        doc = run(doc, define_dab("my_colour", Json::parse(R"({"spacing": 0.6, "color": {"enabled": true, "fg_bg": {"jitter": 1}}, "seed": 2})")));
        Json op = stroke("my_colour");
        op["rgb"] = Json::array({255, 0, 0});
        doc = run(doc, op);
        QCOMPARE(*ink(doc).strokes->items.back()->background_rgb, (std::vector<std::int64_t>{0, 0, 255}));
        const render::Image a = ink_picture(doc);
        bool blueish = false, reddish = false;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const auto p = a.getpixel(x, y);
                if (p[3] < 250) continue;
                blueish = blueish || p[2] > 150;
                reddish = reddish || p[0] > 150;
            }
        }
        QVERIFY(blueish && reddish);
        Json own = stroke("my_colour");
        own["background_rgb"] = Json::array({0, 255, 0});
        QCOMPARE(*ink(run(doc, own)).strokes->items.back()->background_rgb, (std::vector<std::int64_t>{0, 255, 0}));
        own["background_rgb"] = Json::array({0, 256, 0});
        QVERIFY(refusal(doc, Json::array({own})).find("background_rgb") != std::string::npos);
    }

    // add_stroke's blend on a paint layer of each kind: painted into its pixels (multiply darkens where the line goes,
    // nothing else changes), no line kept; a dab brush's too; on a pen layer refused; "normal" a line as before.
    void blendPaintsAPaintLayer() {
        for (const std::string& kind : kinds()) {
            core::Document doc = as_kind(paint_book(), kind);
            doc = run(doc, define_dab());
            const Pixels before = whole(doc);
            Json op = stroke("gpen", "");
            op.erase("layer");
            op["layer_id"] = "paint";
            op["blend"] = "multiply";
            op["rgb"] = Json::array({60, 60, 60});
            const auto result = run_ops(doc, Json::array({op}));
            core::Document painted = result.doc;
            QVERIFY(result.warnings.dump().find("unknown") == std::string::npos);
            const core::Layer& layer = painted.page(0).layers[0];
            QCOMPARE(layer.stroke_count(), std::size_t(0));
            const Pixels after = whole(painted);
            QCOMPARE(after.width(), before.width());
            const std::size_t w = before.width();
            const double k = w / (2 * kInch);  // (pixels a mm)
            const std::size_t on = std::size_t(std::lround(30 * k)) * w + std::size_t(std::lround(25 * k));
            const std::size_t off = std::size_t(std::lround(45 * k)) * w + std::size_t(std::lround(45 * k));
            QVERIFY2(!std::equal(before.at(on), before.at(on) + before.all(), after.at(on)), kind.c_str());
            QVERIFY2(std::equal(before.at(off), before.at(off) + before.all(), after.at(off)), kind.c_str());
            // the dab brush's line too
            Json dab = op;
            dab["kind"] = "my_dab";
            const Pixels dabbed = whole(*std::make_unique<core::Document>(run(doc, dab)));
            QVERIFY2(!std::equal(before.at(on), before.at(on) + before.all(), dabbed.at(on)), kind.c_str());
        }
        core::Document pen = pen_book();
        Json on_pen = stroke("gpen");
        on_pen["blend"] = "multiply";
        QVERIFY(refusal(pen, Json::array({on_pen})).find("blend paints a paint layer's pixels") != std::string::npos);
        on_pen["blend"] = "normal";
        QCOMPARE(ink(run(pen, on_pen)).stroke_count(), std::size_t(1));
        Json unknown = stroke("gpen");
        unknown.erase("layer");
        unknown["layer_id"] = "paint";
        unknown["blend"] = "sparkle";
        QVERIFY(refusal(paint_book(), Json::array({unknown})).find("blend is not one of the blends") != std::string::npos);
    }

    // erase's brush (a dab brush of the book) and block on a paint layer of each kind: the alpha taken where it went
    // (partly at opacity 0.5), nothing elsewhere — on a layer of the working 200 dpi and on one of 300 dpi (a picture's own
    // grid: kept its size, the right and the bottom too) —; a brush that is not a dab brush refused.
    void eraseByBrushAndBlock() {
        for (const int side : {kSide, 600}) {  // (the working 200 dpi, and a layer of 300 dpi: a picture's own grid)
            for (const std::string& kind : kinds()) {
                core::Document doc = run(as_kind(paint_book(side), kind), define_dab("my_hard", Json::parse(R"({"hardness": 1, "spacing": 0.1})")));
                const Pixels before = whole(doc);
                const std::size_t w = before.width();
                const double k = w / (2 * kInch);
                const auto at = [&](double x_mm, double y_mm) { return std::size_t(std::lround(y_mm * k)) * w + std::size_t(std::lround(x_mm * k)); };
                const auto alpha = [&](const Pixels& p, std::size_t i) { return p.at(i)[p.colours()]; };
                for (const Json& how : {Json{{"brush", "my_hard"}}, Json{{"block", true}}}) {
                    Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}};
                    for (const auto& [key, value] : how.items()) op[key] = value;
                    const Pixels after = whole(*std::make_unique<core::Document>(run(doc, op)));
                    QCOMPARE(after.width(), before.width());  // (its grid kept: the right and the bottom too)
                    QCOMPARE(after.height(), before.height());
                    QVERIFY2(alpha(after, at(25, 25)) < 0.01f, kind.c_str());
                    QVERIFY2(alpha(after, at(25, 40)) > 0.99f, kind.c_str());
                    QVERIFY2(alpha(after, after.width() * after.height() - 1) > 0.99f, kind.c_str());
                    op["opacity"] = 0.5;
                    const Pixels half = whole(*std::make_unique<core::Document>(run(doc, op)));
                    QVERIFY2(std::abs(alpha(half, at(25, 25)) - 0.5f) < 0.02f, qPrintable(QString::fromStdString(kind) + " " + QString::number(alpha(half, at(25, 25)))));
                }
            }
        }
        // a layer of 300 dpi (not the working 200: a picture's own grid) keeps its size, erased where the eraser went
        for (const Json& how : {Json{{"brush", "my_hard"}}, Json{{"block", true}}}) {
            const core::Document fine = run(paint_book(600), define_dab("my_hard", Json::parse(R"({"hardness": 1, "spacing": 0.1})")));
            Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}};
            for (const auto& [key, value] : how.items()) op[key] = value;
            const core::Document erased = run(fine, op);
            const render::Image picture = render::raster::layer_pixels(erased.page(0), erased.page(0).layers[0]);
            QCOMPARE(picture.width(), 600);
            QCOMPARE(picture.height(), 600);
            const double k = 600 / (2 * kInch);
            QCOMPARE(alpha_at(picture, static_cast<int>(25 * k), static_cast<int>(25 * k)), 0);  // (where it went)
            QCOMPARE(alpha_at(picture, static_cast<int>(25 * k), static_cast<int>(40 * k)), 255);
            QCOMPARE(alpha_at(picture, 599, 599), 255);  // (the right and the bottom kept)
            QCOMPARE(alpha_at(picture, 599, static_cast<int>(25 * k)), 255);
        }
        core::Document doc = paint_book();
        Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}, {"brush", "gpen"}};
        QVERIFY(refusal(doc, Json::array({op})).find("brush must be a dab brush") != std::string::npos);
        op.erase("brush");
        op["opacity"] = 0.5;
        QVERIFY(refusal(doc, Json::array({op})).find("opacity is the eraser's brush's") != std::string::npos);
    }

    // set_brush: the kind add_stroke draws with when it names none, the 背景色, reset back to the G pen.
    void setBrushKindBackgroundReset() {
        core::Document doc = run(pen_book(), define_dab());
        doc = run(doc, Json{{"op", "set_brush"}, {"kind", "my_dab"}, {"background_rgb", Json::array({1, 2, 3})}, {"width_mm", 2.0}});
        QCOMPARE(*doc.brush_kind, std::string("my_dab"));
        QCOMPARE(*doc.brush_background_rgb, (std::vector<std::int64_t>{1, 2, 3}));
        Json op = stroke("my_dab");
        op.erase("kind");
        QCOMPARE(ink(run(doc, op)).strokes->items.back()->kind, std::string("my_dab"));
        QVERIFY(refusal(doc, Json::array({Json{{"op", "set_brush"}, {"kind", "nothing"}}})).find("kind must be one of") != std::string::npos);
        const core::Document reset = run(doc, Json{{"op", "set_brush"}, {"reset", true}});
        QVERIFY(!reset.brush_kind && !reset.brush_background_rgb);
        QCOMPARE(reset.brush_width_mm, core::Document{}.brush_width_mm);
        QCOMPARE(ink(run(reset, op)).strokes->items.back()->kind, std::string("gpen"));
        QVERIFY(run_ops(doc, Json::array({Json{{"op", "set_brush"}, {"reset", true}}})).warnings.dump().find("unknown") == std::string::npos);
    }

    // 下地混色 (design 0.4, the leftover of PC2) on the paint layer of each kind: the grey under the blue line picked up
    // (the line drawn greyer than without mix: a precise or ink layer draws its lines into its pixels), each piece's
    // colour moving toward it with the pen's inputs carried; a precise line's pieces keep a precise colour, of the line's
    // precision.
    void mixingOnEveryKindOfLayer() {
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(paint_book(), kind);
            Json op = stroke("gpen", "");
            op.erase("layer");
            op["layer_id"] = "paint";
            op["rgb"] = Json::array({0, 0, 255});
            op["mix"] = 0.8;
            op["stretch"] = 0.0;
            op["tilt"] = Json::parse("[[10, 20], [30, 40], [50, 60]]");
            const bool precise = kind == "16" || kind == "32";
            if (precise) op["color_rgb"] = Json{{"precision", kind == "16" ? "u16" : "f32"}, {"values", Json::array({0.0, 0.0, 1.0})}};
            Json plain_op = op;
            plain_op.erase("mix");
            const auto mixed = run_ops(doc, Json::array({op}));
            const auto plain = run_ops(doc, Json::array({plain_op}));
            QVERIFY2(mixed.warnings.dump().find("color_rgb") == std::string::npos || !precise, kind.c_str());
            // drawn: at the line's middle point (25, 30 mm), greyer than the line without mix
            const auto colour_at = [](const core::Document& d) {
                brushes::register_book(d);
                return render::render_page(d.page(0), 200, render::RenderOptions{}, &d).image.convert("RGB").getpixel(197, 236);
            };
            const std::vector<double> m = colour_at(mixed.doc), q = colour_at(plain.doc);
            const QString seen = QStringLiteral("%1: mixed %2,%3,%4 plain %5,%6,%7").arg(QString::fromStdString(kind)).arg(m[0]).arg(m[1]).arg(m[2]).arg(q[0]).arg(q[1]).arg(q[2]);
            if (kind != "gray") QVERIFY2(q[2] - q[0] > 50, qPrintable(seen));  // (the plain line blue, as far as the page shows it)
            QVERIFY2(m[0] > q[0] + 60 && m[2] - m[0] < q[2] - q[0] + 1, qPrintable(seen));
            // the pieces themselves (an 8-bit layer keeps them as lines)
            const core::Page& page = doc.page(0);
            const core::Layer& layer = page.layers[0];
            core::Stroke line;
            line.id = "line";
            line.points = {core::PointF{8, 25}, core::PointF{25, 30}, core::PointF{42, 22}};
            line.width_mm = 3.0;
            line.rgb = std::vector<std::int64_t>{0, 0, 255};
            line.tilt = {10, 20, 30, 40, 50, 60};
            line.time_ms = {0, 16, 32};
            if (precise) line.color_rgb = op["color_rgb"];
            const std::vector<core::Stroke> pieces = render::raster::mixed_pieces(page, layer, line, 0.8, 0.0);
            QVERIFY2(pieces.size() > 1, kind.c_str());
            for (const core::Stroke& piece : pieces) {
                QVERIFY2((*piece.rgb)[0] > 100 && (*piece.rgb)[2] < 250, qPrintable(QString::fromStdString(kind + " " + Json(*piece.rgb).dump())));
                QCOMPARE(piece.tilt.size(), 2 * piece.points.size());
                QCOMPARE(piece.time_ms.size(), piece.points.size());
                QCOMPARE(piece.color_rgb.has_value(), precise);
                if (precise) {
                    QCOMPARE((*piece.color_rgb)["precision"].get<std::string>(), std::string(kind == "16" ? "u16" : "f32"));
                    QVERIFY((*piece.color_rgb)["values"][0].get<double>() > 0.4);
                }
            }
            if (kind == "8") QCOMPARE(mixed.doc.page(0).layers[0].stroke_count(), pieces.size());
        }
    }

    // brush.get (`genko brushes get <key> [--book <dir>]`): a dab brush of the book with every setting (the defaults too,
    // a round tip as "round"); a built-in brush without dabs; a brush that is not there refused.
    void brushGetSaysEverySetting() {
        QTemporaryDir tmp;
        const QString book = tmp.path() + QStringLiteral("/b.genko");
        const fs::path dir = core::path_from_utf8(book.toStdString());
        const core::Document doc = pen_book();
        write_book(doc, dir);
        const auto defined = run_ops(doc, Json::array({define_dab()}));
        save_next(defined.doc, dir, defined.journal_ops);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), tmp.path() + QStringLiteral("/config"));
        auto ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("my_dab"), QStringLiteral("--book"), book}, env);
        QVERIFY2(ran.finished && ran.exit_code == 0, qPrintable(ran.described()));
        Json got = genko::test::one_line(ran.out);
        QCOMPARE(got["key"], Json("my_dab"));
        QCOMPARE(got["from"], Json("book"));
        QCOMPARE(got["engine"], Json("dabs"));
        QCOMPARE(got["dabs"]["hardness"], Json(0.4));
        QCOMPARE(got["dabs"]["size_mm"].get<double>(), core::DabSettings{}.size_mm);
        QCOMPARE(got["dabs"]["tip"], Json("round"));
        QVERIFY(got["dabs"].contains("dual") && got["dabs"]["dual"].contains("spacing"));
        QCOMPARE(core::dabs_from_json(got["dabs"]), *core::brush_for(Json("my_dab"), defined.doc).dabs);  // (read back the same)
        ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("gpen")}, env);
        QVERIFY2(ran.finished && ran.exit_code == 0, qPrintable(ran.described()));
        got = genko::test::one_line(ran.out);
        QCOMPARE(got["from"], Json("builtin"));
        QVERIFY(got["engine"].is_null() && !got.contains("dabs"));
        QCOMPARE(got["settings"]["width_mm"], Json(core::find_builtin("gpen")->width_mm));
        ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("my_none")}, env);
        QCOMPARE(ran.exit_code, 1);
        QCOMPARE(genko::test::one_line(ran.out)["code"], Json("key"));
    }

    // Saved, opened (brushes@1, the pictures), drawn the same; Undo gives back the book without the line (and without
    // the feature once the brush goes too); Redo the same pixels again.
    void savedOpenedUndoneRedone() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8((tmp.path() + QStringLiteral("/d.genko")).toStdString());
        core::Document doc = pen_book();
        write_book(doc, dir);
        Json dabs = Json::parse(R"({"hardness": 0.3, "texture": {"enabled": true, "depth": 0.5}})");
        dabs["texture"]["pattern"] = Json{{"png", core::b64encode(grey_png(16, 16, 128))}};
        const auto defined = run_ops(doc, Json::array({define_dab("my_saved", dabs)}));
        save_next(defined.doc, dir, defined.journal_ops);
        const auto drawn = run_ops(defined.doc, Json::array({stroke("my_saved")}));
        save_next(drawn.doc, dir, drawn.journal_ops);
        const render::Image here = ink_picture(drawn.doc);
        brushes::clear_custom();
        render::dabs::clear_pictures();
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(std::find(loaded.document.features.begin(), loaded.document.features.end(), "brushes@1") != loaded.document.features.end());
        QCOMPARE(ink_picture(loaded.document).tobytes(), here.tobytes());
        storage::restore(dir, "human:test", false, false);  // undo the line
        brushes::clear_custom();
        QCOMPARE(ink(storage::load_document(dir).document).stroke_count(), std::size_t(0));
        storage::restore(dir, "human:test", true, false);  // redo
        brushes::clear_custom();
        render::dabs::clear_pictures();
        QCOMPARE(ink_picture(storage::load_document(dir).document).tobytes(), here.tobytes());
        storage::restore(dir, "human:test", false, false);
        storage::restore(dir, "human:test", false, false);  // the brush undone too: no feature
        const auto bare = storage::load_document(dir);
        QVERIFY(std::find(bare.document.features.begin(), bare.document.features.end(), "brushes@1") == bare.document.features.end());
    }
};

QTEST_GUILESS_MAIN(TestDabLines)
#include "test_dab_lines.moc"
