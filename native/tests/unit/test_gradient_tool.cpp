// PC4 E4 (SPEC.md COLOR-09「塗りつぶし・グラデーション」): the gradient tool's and the paint bucket's ops without Python —
// the gradient's shapes (PhotoCraft v0.2.0's algo/src/paint.rs gradient_t_shapes on Genko's from–to, C-b's forms), reverse,
// dither breaking an 8-bit layer's bands (paint_cmds.rs gradient_dither_breaks_banding), the transparency stops, a stop's
// colour of the book's 描画色 and 背景色 or a precise one, gradient_fill with this build's keys painting a paint layer's own
// values on 8-bit, 16-bit, 32-bit, CMYK, Lab and grey pages (finer than 8 bits on a precise one; a selection's area only;
// a large layer in bands), a pen layer's picture as before (a blend refused), the calls without the new keys as before (a
// picture); fill with a blend, a precise layer's color_rgb, an ink layer's inks; fill_area by 色の近さ (the bucket's other
// basis); saved, opened, undone and redone.

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page_internal.hpp"
#include "render/paint_fill.hpp"
#include "render/raster.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::Box;
using render::Size;
using render::image_ops::Pixels;
using render::image_ops::Rgba;
namespace fs = std::filesystem;

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

core::Document pen_book() {
    core::Document doc = core::new_episode("グラデーション", core::Num(1), 1,
                                           core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    doc.edit_page(0).numero = false;
    return doc;
}

// A page of one 8-bit paint layer "paint", opaque grey (200), `side` pixels across the page.
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
    for (int i = 0; i < side * side; ++i) bytes += std::string("\xC8\xC8\xC8\xFF", 4);
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

// The colours the pixels show (an ink layer's as its inks mean them), straight, each 0..1.
std::vector<Rgba> shown(const Pixels& p) {
    if (p.kind() == Pixels::Kind::Inks) return render::image_ops::rgba_of(p);
    std::vector<Rgba> out;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float* v = p.at(i);
        out.push_back(p.colours() == 1 ? Rgba{v[0], v[0], v[0], v[1]} : Rgba{v[0], v[1], v[2], v[3]});
    }
    return out;
}

std::size_t at_mm(const Pixels& p, double x_mm, double y_mm) {
    const double k = p.width() / (2 * kInch);
    return std::size_t(std::lround(y_mm * k - 0.5)) * p.width() + std::size_t(std::lround(x_mm * k - 0.5));
}

Json black_white(double x0 = 5, double x1 = 45) {
    return Json{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", "paint"}, {"from", Json::array({x0, 25})}, {"to", Json::array({x1, 25})},
                {"stops", Json::parse("[[0, [0, 0, 0]], [1, [255, 255, 255]]]")}, {"dither", false}};
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

}  // namespace

class TestGradientTool : public QObject {
    Q_OBJECT

private slots:
    // gradient_t's shapes (upstream's test on Genko's from–to: (0, 0) → (10, 0); the angle counter-clockwise on the page from
    // the direction from → to), reverse, and the transparency stops times the colour stops' opacity
    void theShapes() {
        const auto t_at = [](const std::string& shape, double x, double y, bool reverse = false) {
            Json spec{{"from", Json::array({0, 0})}, {"to", Json::array({10, 0})}, {"shape", shape}};
            if (reverse) spec["reverse"] = true;
            const render::detail::GradientColours colours(spec, {{0, {0, 0, 0}, 1}, {1, {255, 255, 255}, 1}});
            double rgba[4];
            colours.at(x, y, rgba);
            return rgba[0] / 255.0;
        };
        QCOMPARE(t_at("linear", 5, 3), 0.5);
        QCOMPARE(t_at("linear", -5, 0), 0.0);
        QCOMPARE(t_at("radial", 0, 5), 0.5);
        QCOMPARE(t_at("reflected", -5, 0), 0.5);
        QCOMPARE(t_at("diamond", 2.5, 2.5), 0.5);
        QVERIFY(std::abs(t_at("angle", 0, -5) - 0.25) < 1e-9);  // (a quarter turn up the page)
        QCOMPARE(t_at("linear", 2.5, 0, true), 0.75);
        // the transparency stops: their opacity times the colour stops'
        Json spec{{"from", Json::array({0, 0})}, {"to", Json::array({10, 0})}, {"transparency", Json::parse("[[0, 1], [1, 0]]")}};
        const render::detail::GradientColours faded(spec, {{0, {10, 20, 30}, 1}, {1, {10, 20, 30}, 0.5}});
        double rgba[4];
        faded.at(5, 0, rgba);
        QVERIFY(std::abs(rgba[3] - 0.75 * 0.5) < 1e-9);
        QCOMPARE(rgba[1], 20.0);
        // the 8-bit form of the fill layers takes them too
        Json old_form{{"from", Json::array({0, 0})}, {"to", Json::array({10, 0})}, {"rgb_from", Json::array({0, 0, 0})},
                      {"rgb_to", Json::array({0, 0, 0})}, {"transparency", Json::parse("[[0, 1], [1, 0]]")}};
        const render::detail::GradientColours eight(old_form);
        unsigned char rgb[3];
        double opacity = 0;
        eight.at(10, 0, rgb, opacity);
        QCOMPARE(opacity, 0.0);
    }

    // gradient_fill with this build's keys on a paint layer of each kind: the layer's own values (no picture laid on it),
    // dark to light along the drag, the rest of the page as it was out of a selection's area; finer than 8 bits on a precise
    // layer.
    void newKeysPaintTheLayersValues() {
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(paint_book(), kind);
            const auto result = run_ops(doc, Json::array({black_white()}));
            QVERIFY2(result.warnings.dump().find("unknown") == std::string::npos, result.warnings.dump().c_str());
            core::Document painted = result.doc;
            QVERIFY2(painted.page(0).layers[0].patches.empty(), kind.c_str());
            QVERIFY(painted.page(0).layers[0].kind == core::LayerKind::Raster);
            const Pixels p = whole(painted);
            const std::vector<Rgba> c = shown(p);
            const Rgba left = c[at_mm(p, 2, 25)], right = c[at_mm(p, 48, 25)], mid = c[at_mm(p, 25, 25)];
            const bool inks = p.kind() == Pixels::Kind::Inks;  // (an ink page's black and white as its rules print them)
            QVERIFY2(left[0] < (inks ? 0.25f : 0.03f) && right[0] > (inks ? 0.9f : 0.97f), qPrintable(QString("%1: %2 %3").arg(kind.c_str()).arg(left[0]).arg(right[0])));
            QVERIFY2(std::abs(mid[0] - 0.5f) < (inks ? 0.1f : 0.03f), qPrintable(QString("%1: %2").arg(kind.c_str()).arg(mid[0])));
            // along the row: never darker to the right
            const std::size_t row = at_mm(p, 0, 25) / p.width() * p.width();
            for (std::size_t x = 1; x < p.width(); ++x) QVERIFY2(c[row + x][0] + (inks ? 0.01f : 2e-3f) >= c[row + x - 1][0], kind.c_str());
            if (kind == "16" || kind == "32") {
                std::set<float> levels;
                for (std::size_t x = 0; x < p.width(); ++x) levels.insert(p.at(row + x)[0]);
                QVERIFY2(levels.size() > 300, qPrintable(QString("%1: %2 levels").arg(kind.c_str()).arg(levels.size())));
            }
            // a selection's area only
            Json in_area = black_white();
            in_area["area"] = Json{{"poly", Json::parse("[[0, 0], [50.8, 0], [50.8, 20], [0, 20]]")}};
            core::Document part = run(doc, in_area);
            const std::vector<Rgba> a = shown(whole(part));
            const std::vector<Rgba> before = shown(whole(*std::make_unique<core::Document>(doc)));
            QVERIFY2(a[at_mm(p, 2, 10)][0] < 0.03f, kind.c_str());
            QVERIFY2(std::abs(a[at_mm(p, 2, 40)][0] - before[at_mm(p, 2, 40)][0]) < 1e-6f, kind.c_str());
        }
    }

    // dither (an 8-bit layer's): a flat gradient smooth without it, varied a level with it, on the same mean
    void ditherBreaksBanding() {
        Json flat = black_white();
        flat["stops"] = Json::parse("[[0, [128, 128, 128]], [1, [128, 128, 128]]]");
        core::Document smooth = run(paint_book(), flat);
        flat["dither"] = true;
        core::Document noisy = run(paint_book(), flat);
        const Pixels a = whole(smooth), b = whole(noisy);
        std::set<float> plain, varied;
        double sum = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            plain.insert(a.at(i)[0]);
            varied.insert(b.at(i)[0]);
            sum += b.at(i)[0];
            QVERIFY(std::abs(b.at(i)[0] - 128.0f / 255.0f) <= 1.0f / 255.0f + 1e-6f);
        }
        QCOMPARE(plain.size(), std::size_t(1));
        QVERIFY(varied.size() > 1);
        QVERIFY(std::abs(sum / static_cast<double>(b.size()) - 128.0 / 255.0) < 0.5 / 255.0);
        // (the same pixels each time: the noise is of the place)
        core::Document again = run(paint_book(), flat);
        const Pixels c = whole(again);
        for (std::size_t i = 0; i < b.size(); ++i) QCOMPARE(c.at(i)[0], b.at(i)[0]);
        // reverse: light to dark
        Json back = black_white();
        back["reverse"] = true;
        core::Document reversed = run(paint_book(), back);
        const Pixels r = whole(reversed);
        QVERIFY(r.at(at_mm(r, 2, 25))[0] > 0.97f && r.at(at_mm(r, 48, 25))[0] < 0.03f);
    }

    // the transparency stops on each kind of page: opaque black at the start, the grey under it at the end; opacity and a
    // blend
    void transparencyOpacityAndBlend() {
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(paint_book(), kind);
            const std::vector<Rgba> grey = shown(whole(*std::make_unique<core::Document>(doc)));
            Json op = black_white();
            op["stops"] = Json::parse("[[0, [0, 0, 0]], [1, [0, 0, 0]]]");
            op["transparency"] = Json::parse("[[0, 1], [1, 0]]");
            core::Document faded = run(doc, op);
            const Pixels p = whole(faded);
            const std::vector<Rgba> c = shown(p);
            const bool inks = p.kind() == Pixels::Kind::Inks;
            QVERIFY2(c[at_mm(p, 2, 25)][0] < (inks ? 0.25f : 0.03f), kind.c_str());
            QVERIFY2(std::abs(c[at_mm(p, 48, 25)][0] - grey[at_mm(p, 48, 25)][0]) < 0.01f, kind.c_str());
            op.erase("transparency");
            op["opacity"] = 0.5;
            core::Document half = run(doc, op);
            const std::vector<Rgba> h = shown(whole(half));
            QVERIFY2(std::abs(h[at_mm(p, 25, 25)][0] - grey[at_mm(p, 25, 25)][0] * 0.5f) < (inks ? 0.1f : 0.02f), kind.c_str());
            op.erase("opacity");
            op["stops"] = Json::parse("[[0, [255, 255, 255]], [1, [128, 128, 128]]]");
            op["blend"] = "multiply";
            core::Document multiplied = run(doc, op);
            const std::vector<Rgba> m = shown(whole(multiplied));
            QVERIFY2(std::abs(m[at_mm(p, 2, 25)][0] - grey[at_mm(p, 2, 25)][0]) < 0.02f, kind.c_str());
            QVERIFY2(std::abs(m[at_mm(p, 48, 25)][0] - grey[at_mm(p, 48, 25)][0] * 128.0f / 255.0f) < (inks ? 0.06f : 0.02f), kind.c_str());
        }
        Json bad = black_white();
        bad["transparency"] = Json::parse("[[0, 2], [1, 0]]");
        QVERIFY(refusal(paint_book(), Json::array({bad})).find("transparency is") != std::string::npos);
        bad = black_white();
        bad["blend"] = "sparkle";
        QVERIFY(refusal(paint_book(), Json::array({bad})).find("blend is one of the blends") != std::string::npos);
        bad = black_white();
        bad["opacity"] = 1.5;
        QVERIFY(refusal(paint_book(), Json::array({bad})).find("opacity is 0..1") != std::string::npos);
        bad = black_white();
        bad["shape"] = "star";
        QVERIFY(refusal(paint_book(), Json::array({bad})).find("shape must be") != std::string::npos);
    }

    // a stop's colour: the book's 描画色 and 背景色 (set_brush), a precise colour (16 bits: as it is, not rounded to 8)
    void liveAndPreciseColours() {
        core::Document doc = run(paint_book(), Json{{"op", "set_brush"}, {"rgb", Json::array({200, 0, 0})}, {"background_rgb", Json::array({0, 0, 200})}});
        Json op = black_white();
        op["stops"] = Json::parse(R"([[0, "foreground"], [1, "background"]])");
        core::Document painted = run(doc, op);
        const Pixels p = whole(painted);
        QVERIFY(std::abs(p.at(at_mm(p, 2, 25))[0] - 200.0f / 255.0f) < 0.01f && p.at(at_mm(p, 2, 25))[2] < 0.01f);
        QVERIFY(std::abs(p.at(at_mm(p, 48, 25))[2] - 200.0f / 255.0f) < 0.01f && p.at(at_mm(p, 48, 25))[0] < 0.01f);
        // no 背景色 in the book: white
        Json white = black_white();
        white["rgb_from"] = "foreground";
        white["rgb_to"] = "background";
        white.erase("stops");
        core::Document plain = run(paint_book(), white);
        const Pixels w = whole(plain);
        QVERIFY(w.at(at_mm(w, 48, 25))[0] > 0.97f && w.at(at_mm(w, 48, 25))[2] > 0.97f);
        // a precise colour on a page of 16 bits
        core::Document precise = as_kind(paint_book(), "16");
        Json exact = black_white();
        exact["stops"] = Json::parse(R"([[0, {"precision": "u16", "values": [0.3333, 0.3333, 0.3333]}], [1, {"precision": "u16", "values": [0.3333, 0.3333, 0.3333]}]])");
        core::Document flat = run(precise, exact);
        const Pixels f = whole(flat);
        QVERIFY2(std::abs(f.at(at_mm(f, 25, 25))[0] - 0.3333f) < 1.0f / 65535.0f + 1e-6f, qPrintable(QString::number(f.at(at_mm(f, 25, 25))[0], 'g', 9)));
        exact["stops"][0][1] = Json{{"precision", "u8"}, {"values", Json::array({0, 0, 0})}};
        QVERIFY(refusal(precise, Json::array({exact})).find("a stop's colour") != std::string::npos);
    }

    // a new (empty) paint layer of a page of 16 or 32 bits: a gradient and a fill make it precise (its pixels finer than 8
    // bits, no picture laid on it); an alpha channel of such a page takes a gradient as before (lent as an 8-bit picture)
    void emptyLayersOfPrecisePagesBecomePrecise() {
        for (const std::string& kind : {std::string("16"), std::string("32")}) {
            const core::Document doc = run(as_kind(paint_book(), kind), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "fresh"}});
            const core::Layer* made = nullptr;
            for (const core::Layer& l : doc.page(0).layers)
                if (l.id == "fresh") made = &l;
            QVERIFY(made != nullptr && made->color_raster == nullptr);
            Json op = black_white();
            op["layer_id"] = "fresh";
            op.erase("dither");
            core::Document painted = run(doc, op);
            const core::Layer& fresh = painted.page(0).layers[core::layer_by_id(painted.page(0), "fresh")];
            QVERIFY2(fresh.color_raster != nullptr && fresh.patches.empty(), kind.c_str());
            const Pixels p = whole(painted, "fresh");
            QVERIFY(p.kind() == Pixels::Kind::Precise);
            const std::size_t row = at_mm(p, 0, 25) / p.width() * p.width();
            std::set<float> levels;
            for (std::size_t x = 0; x < p.width(); ++x) levels.insert(p.at(row + x)[0]);
            QVERIFY2(levels.size() > 300, qPrintable(QString("%1: %2 levels").arg(kind.c_str()).arg(levels.size())));
            Json click{{"op", "fill"}, {"page", 1}, {"layer_id", "fresh"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({100, 100, 100})},
                       {"color_rgb", Json{{"precision", "u16"}, {"values", Json::array({0.3333, 0.5, 0.25})}}}};
            core::Document filled = run(doc, click);
            const core::Layer& bucket = filled.page(0).layers[core::layer_by_id(filled.page(0), "fresh")];
            QVERIFY2(bucket.color_raster != nullptr && bucket.patches.empty(), kind.c_str());
            const Pixels f = whole(filled, "fresh");
            QVERIFY2(std::abs(f.at(at_mm(f, 25, 25))[0] - 0.3333f) < 1.0f / 65535.0f + 1e-6f, kind.c_str());
        }
        // an alpha channel of a page of 16 bits: the gradient in the channel, the layers as they were
        core::Document doc = run(as_kind(paint_book(), "16"), Json{{"op", "add_channel"}, {"page", 1}});
        const std::string channel = doc.page(0).channels.at(0).id;
        Json op = black_white();
        op.erase("layer_id");
        op.erase("dither");
        op["channel"] = channel;
        core::Document drawn = run(doc, op);
        QVERIFY(drawn.page(0).channels.at(0).raster != nullptr);
        QVERIFY(drawn.page(0).channels.at(0).raster != doc.page(0).channels.at(0).raster);
        QCOMPARE(drawn.page(0).layers.size(), doc.page(0).layers.size());
        for (std::size_t i = 0; i < doc.page(0).layers.size(); ++i) {
            QVERIFY(drawn.page(0).layers[i].color_raster == doc.page(0).layers[i].color_raster);
            QVERIFY(drawn.page(0).layers[i].patches.empty());
        }
    }

    // a pen layer: a picture laid on it, as before (the new shapes and keys in it), a blend refused; the calls without the
    // new keys on an 8-bit paint layer: a picture, its pixels as they were
    void penLayersAndOldCalls() {
        core::Document pen = pen_book();
        const std::string ink = pen.page(0).first_layer(core::LayerRole::Ink)->id;
        Json op{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", ink}, {"from", Json::array({5, 25})}, {"to", Json::array({45, 25})},
                {"shape", "diamond"}, {"reverse", true}, {"transparency", Json::parse("[[0, 1], [1, 0.5]]")}, {"dither", true}};
        core::Document drawn = run(pen, op);
        QCOMPARE(drawn.page(0).first_layer(core::LayerRole::Ink)->patches.size(), std::size_t(1));
        QVERIFY(drawn.page(0).first_layer(core::LayerRole::Ink)->kind == core::LayerKind::Strokes);
        op["blend"] = "multiply";
        QVERIFY(refusal(pen, Json::array({op})).find("blend paints a paint layer's pixels") != std::string::npos);
        // the old call: a picture, the layer's pixels as they were
        core::Document paint = paint_book();
        const Pixels before = whole(paint);
        Json old{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", "paint"}, {"from", Json::array({5, 25})}, {"to", Json::array({45, 25})}};
        core::Document laid = run(paint, old);
        QCOMPARE(laid.page(0).layers[0].patches.size(), std::size_t(1));
        const std::string png_before = *paint.page(0).layers[0].raster_png;
        QCOMPARE(*laid.page(0).layers[0].raster_png, png_before);
        (void)before;
    }

    // fill (the bucket, 線で囲まれた所): a blend on an 8-bit paint layer paints its pixels; a precise layer takes the
    // color_rgb as it is (16 bits), an ink layer its inks; without them on an 8-bit layer a picture as before
    void fillIntoTheLayersValues() {
        const Json click{{"op", "fill"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({100, 100, 100})}};
        core::Document laid = run(paint_book(), click);
        QCOMPARE(laid.page(0).layers[0].patches.size(), std::size_t(1));
        Json blended = click;
        blended["blend"] = "multiply";
        core::Document multiplied = run(paint_book(), blended);
        QVERIFY(multiplied.page(0).layers[0].patches.empty());
        const Pixels m = whole(multiplied);
        QVERIFY(std::abs(m.at(at_mm(m, 25, 25))[0] - 200.0f * 100.0f / 255.0f / 255.0f) < 0.01f);
        for (const std::string& kind : kinds()) {
            if (kind == "8") continue;
            Json exact = click;
            exact["color_rgb"] = Json{{"precision", "u16"}, {"values", Json::array({0.3333, 0.5, 0.25})}};
            const auto result = run_ops(as_kind(paint_book(), kind), Json::array({exact}));
            core::Document filled = result.doc;
            QVERIFY2(filled.page(0).layers[0].patches.empty(), kind.c_str());
            const Pixels p = whole(filled);
            const std::vector<Rgba> c = shown(p);
            const Rgba got = c[at_mm(p, 25, 25)];
            if (kind == "16" || kind == "32") {
                QVERIFY2(std::abs(got[0] - 0.3333f) < 2.0f / 65535.0f, qPrintable(QString("%1 %2").arg(kind.c_str()).arg(got[0], 0, 'g', 9)));
                QVERIFY(result.warnings.dump().find("color_rgb") == std::string::npos);
            } else {
                // (an ink page: the rgb by the page's rules; color_rgb said not used)
                QVERIFY2(std::abs(got[1] - (kind == "gray" ? got[0] : 100.0f / 255.0f)) < 0.04f, qPrintable(QString("%1 %2").arg(kind.c_str()).arg(got[1])));
                QVERIFY2(result.warnings.dump().find("color_rgb is for an RGB page of 16 or 32 bits") != std::string::npos, kind.c_str());
            }
        }
        Json pen_blend = click;
        pen_blend["blend"] = "multiply";
        pen_blend.erase("layer_id");
        QVERIFY(refusal(pen_book(), Json::array({pen_blend})).find("blend paints a paint layer's pixels") != std::string::npos);
    }

    // the bucket's 色の近さ: fill_area of PC3 B1's {"color"} area — what the click's colour reaches (contiguous or not)
    void fillByColourNearness() {
        core::Document doc = paint_book();
        // a black bar down the middle splits the grey in two
        doc = run(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", Json{{"poly", Json::parse("[[24, 0], [27, 0], [27, 50.8], [24, 50.8]]")}}},
                            {"rgb", Json::array({0, 0, 0})}, {"blend", "normal"}});
        const auto nearness = [](bool contiguous) {
            return Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint"}, {"rgb", Json::array({250, 0, 0})},
                        {"area", Json{{"color", Json{{"x_mm", 10}, {"y_mm", 10}, {"tolerance", 32}, {"contiguous", contiguous}, {"layer", "paint"}}}}},
                        {"blend", "normal"}};
        };
        core::Document one_side = run(doc, Json(nearness(true)));
        const Pixels a = whole(one_side);
        QVERIFY(a.at(at_mm(a, 10, 10))[0] > 0.95f && a.at(at_mm(a, 10, 10))[1] < 0.05f);
        QVERIFY(a.at(at_mm(a, 40, 10))[1] > 0.7f);  // (past the bar: grey)
        core::Document both = run(doc, Json(nearness(false)));
        const Pixels b = whole(both);
        QVERIFY(b.at(at_mm(b, 40, 10))[0] > 0.95f && b.at(at_mm(b, 40, 10))[1] < 0.05f);
        QVERIFY(b.at(at_mm(b, 25.5, 10))[0] < 0.05f);  // (the bar: black)
    }

    // a layer larger than a band (4 million pixels): painted in bands, the same gradient across their seams
    void largeLayersInBands() {
        core::Document doc = paint_book(2400);  // (5.76 million pixels: two bands)
        Json op = black_white(0, 0.01);
        op["from"] = Json::array({25, 0});
        op["to"] = Json::array({25, 50.8});
        core::Document painted = run(doc, op);
        const Pixels p = whole(painted);
        for (std::size_t y = 1; y < p.height(); ++y) {
            const float up = p.at((y - 1) * p.width() + 7)[0], down = p.at(y * p.width() + 7)[0];
            QVERIFY2(down + 1e-6f >= up && down - up < 3.0f / 255.0f, qPrintable(QString("row %1: %2 %3").arg(y).arg(up).arg(down)));
        }
    }

    // saved, opened again: the same pixels (the book before the op as it was)
    void savedAndOpened() {
        QTemporaryDir scratch;
        const core::Document doc = as_kind(paint_book(), "16");
        const core::Document painted = run(doc, black_white());
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "g.genko";
        write_book(painted, dir);
        core::Document opened = storage::load_document(dir).document;
        const Pixels a = whole(*std::make_unique<core::Document>(painted)), b = whole(opened);
        QCOMPARE(b.size(), a.size());
        for (std::size_t i = 0; i < a.size(); ++i) QCOMPARE(b.at(i)[0], a.at(i)[0]);
        QCOMPARE(*doc.page(0).layers[0].color_raster, *as_kind(paint_book(), "16").page(0).layers[0].color_raster);
        QVERIFY(*painted.page(0).layers[0].color_raster != *doc.page(0).layers[0].color_raster);
    }
};

QTEST_GUILESS_MAIN(TestGradientTool)
#include "test_gradient_tool.moc"
