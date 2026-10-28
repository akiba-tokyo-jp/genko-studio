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

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page_internal.hpp"
#include "render/paint_fill.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
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

// The book after ops saved over the one in `dir` as the next step of its journal (what Undo takes back).
void save_next(const core::Document& doc, const fs::path& dir, const Json& ops) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = storage::read_disk_state(dir).revision;
    request.ops = ops;
    storage::Saver(lock).save(doc, request);
}

// The page's colours as it shows (render/image_ops page_pixels: an ink page's as its inks mean them), straight 0..1.
std::vector<Rgba> page_colours(const core::Document& doc) { return shown(render::image_ops::page_pixels(doc, doc.page(0))); }

bool has_feature(const core::Document& doc, const std::string& name) {
    return std::find(doc.features.begin(), doc.features.end(), name) != doc.features.end();
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
            // (an 8-bit layer in 通常: a picture laid on it — its lines kept as lines, the coordinator's decision —; the others
            // their values)
            QVERIFY2(painted.page(0).layers[0].patches.size() == (kind == "8" ? 1u : 0u), kind.c_str());
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

    // dither (an 8-bit layer's): a flat gradient between two levels (128.5) one level without it, the two levels around it
    // with it, on its mean (a band broken: PhotoCraft's gradient_dither_breaks_banding)
    void ditherBreaksBanding() {
        Json flat = black_white();
        flat["stops"] = Json::parse("[[0, [128.5, 128.5, 128.5]], [1, [128.5, 128.5, 128.5]]]");
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
            QVERIFY(std::abs(b.at(i)[0] - 128.5f / 255.0f) <= 0.5f / 255.0f + 1e-6f);
        }
        QCOMPARE(plain.size(), std::size_t(1));
        QCOMPARE(varied.size(), std::size_t(2));
        QVERIFY(std::abs(sum / static_cast<double>(b.size()) - 128.5 / 255.0) < 0.1 / 255.0);
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

    // a new (empty) paint layer of a page of 16 or 32 bits that may hold precise pixels (the rule of keep_page_modes and
    // set_color_mode): a gradient and a fill make it precise (its pixels finer than 8 bits, no picture laid on it) — one
    // cut at the panels too, add_layer's default (the user's decision of 2026-10-28; before it the picture was laid) —;
    // an alpha channel of such a page takes a gradient as before (lent as an 8-bit picture)
    void emptyLayersOfPrecisePagesBecomePrecise() {
        for (const std::string& kind : {std::string("16"), std::string("32")}) {
            const core::Document clipped = run(as_kind(paint_book(), kind), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "fresh"}});
            Json op = black_white();
            op["layer_id"] = "fresh";
            op.erase("dither");
            {
                const core::Document laid = run(clipped, op);
                const core::Layer& fresh = laid.page(0).layers[core::layer_by_id(laid.page(0), "fresh")];
                QVERIFY2(fresh.panel_clip && fresh.color_raster != nullptr && fresh.patches.empty(), kind.c_str());
            }
            const core::Document doc = run(clipped, Json{{"op", "set_layer"}, {"page", 1}, {"id", "fresh"}, {"panel_clip", false}});
            QVERIFY(doc.page(0).layers[core::layer_by_id(doc.page(0), "fresh")].color_raster == nullptr);
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

    // PC5-0 (the user's decision of 2026-10-28): a new paint layer — add_layer's default, cut at the panels — of an RGB
    // page of 16 or 32 bits is at the page's depth: a gradient makes it precise, its paint cut at the panels as an 8-bit
    // layer's is (clear outside them), a layer of 8-bit pixels already stays 8-bit; a grey page of 16 or 32 bits gives it
    // its inks at that depth (as before); an 8-bit page lays the picture on it (as before), and a layer let out of the
    // panels (set_layer panel_clip: false) is made precise as before.
    void newLayersAreAtThePagesDepth() {
        core::Document base = core::new_episode("新しい層", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        base.edit_page(0).numero = false;
        QVERIFY(!base.page(0).frames.empty());
        Json across{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", "fresh"}, {"from", Json::array({0, 25})}, {"to", Json::array({50, 25})},
                    {"stops", Json::parse("[[0, [0, 0, 0]], [1, [255, 255, 255]]]")}};
        const Json add{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "fresh"}};
        for (const int depth : {16, 32}) {
            const core::Document deep = run(base, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}});
            core::Document laid = run_ops(deep, Json::array({add, across})).doc;
            const core::Layer& fresh = laid.page(0).layers[core::layer_by_id(laid.page(0), "fresh")];
            QVERIFY(fresh.panel_clip);
            QVERIFY2(fresh.color_raster != nullptr && fresh.patches.empty(), qPrintable(QString::number(depth)));
            QCOMPARE(core::ColorRasterView(*fresh.color_raster).metadata("")["precision"], Json(depth == 16 ? "u16" : "f32"));
            const Pixels p = whole(laid, "fresh");
            QVERIFY(p.at(at_mm(p, 0.5, 25))[3] == 0.0f);   // (outside the panels: clear)
            QVERIFY(p.at(at_mm(p, 25.4, 25.4))[3] == 1.0f);  // (inside: painted)
            std::set<float> levels;
            const std::size_t row = at_mm(p, 0, 25.4) / p.width() * p.width();
            for (std::size_t x = 0; x < p.width(); ++x) levels.insert(p.at(row + x)[0]);
            QVERIFY2(levels.size() > 256, qPrintable(QString("%1: %2 levels").arg(depth).arg(levels.size())));
            // 8-bit pixels an op lays on it (put_raster) once it was made: the page's precise pixels (keep_page_modes)
            {
                std::string grey;
                for (int i = 0; i < 40 * 40; ++i) grey += std::string("\x40\x80\xC0\xFF", 4);
                const core::Document made = run(deep, add);
                const core::Document put = run(made, Json{{"op", "put_raster"}, {"page", 1}, {"id", "fresh"},
                                                          {"png_base64", core::b64encode(render::write_png(render::Image::frombytes("RGBA", Size{40, 40}, grey)))}});
                const core::Layer& taken = put.page(0).layers[core::layer_by_id(put.page(0), "fresh")];
                QVERIFY2(taken.panel_clip && taken.color_raster != nullptr && (taken.raster_png == nullptr || taken.raster_png->empty()),
                         qPrintable(QString::number(depth)));
            }
            // a layer whose 8-bit pixels an op made before: they stay 8-bit
            core::Document old = deep;
            old = run(old, add);
            core::Layer& eight = old.edit_page(0).layers[core::layer_by_id(old.page(0), "fresh")];
            std::string bytes;
            for (int i = 0; i < 40 * 40; ++i) bytes += std::string("\x10\x20\x30\xFF", 4);
            render::raster::save_raster(old.edit_page(0), eight, render::Image::frombytes("RGBA", Size{40, 40}, bytes));
            Json multiplied = across;
            multiplied["blend"] = "multiply";  // (into its pixels)
            const core::Document kept = run(old, multiplied);
            const core::Layer& still = kept.page(0).layers[core::layer_by_id(kept.page(0), "fresh")];
            QVERIFY(still.color_raster == nullptr && still.raster_png != nullptr && still.raster_png != eight.raster_png);
            // let out of the panels: made precise, as before
            const core::Document out = run_ops(deep, Json::array({add, Json{{"op", "set_layer"}, {"page", 1}, {"id", "fresh"}, {"panel_clip", false}}, across})).doc;
            const core::Layer& free = out.page(0).layers[core::layer_by_id(out.page(0), "fresh")];
            QVERIFY(!free.panel_clip && free.color_raster != nullptr);
        }
        // grey of 16 and 32 bits: its inks at the page's depth
        for (const int depth : {16, 32}) {
            const core::Document grey = run(base, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", depth}});
            const core::Document laid = run_ops(grey, Json::array({add, across})).doc;
            const core::Layer& fresh = laid.page(0).layers[core::layer_by_id(laid.page(0), "fresh")];
            QVERIFY(fresh.ink_raster != nullptr && fresh.color_raster == nullptr && fresh.raster_png == nullptr);
            QCOMPARE(core::ink_raster_header(*fresh.ink_raster).depth, depth);
        }
        // an 8-bit page: the picture laid on it, as before
        const core::Document eight = run_ops(base, Json::array({add, across})).doc;
        const core::Layer& fresh = eight.page(0).layers[core::layer_by_id(eight.page(0), "fresh")];
        QVERIFY(fresh.color_raster == nullptr && fresh.patches.size() == 1);
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
        core::Document doc = as_kind(paint_book(2400), "16");  // (5.76 million pixels of a precise layer: two bands)
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
    // (review P2-1) An ink page's paint layer with a line on it: fill and gradient_fill without the new keys — the 通常
    // mode — paint its inks under the line, which stays a line (shown over the paint, as before E4); the paint shows where
    // the line is not. A blend makes the line its pixels (the window asks first: blend_bakes_ok).
    void inkLayersKeepTheirLines() {
        const auto at = [](double x_mm, double y_mm) {  // (on the page at the working 200 dpi: 400 pixels across)
            return std::size_t(std::lround(y_mm * kSide / (2 * kInch) - 0.5)) * kSide + std::size_t(std::lround(x_mm * kSide / (2 * kInch) - 0.5));
        };
        for (const std::string& kind : {std::string("cmyk"), std::string("gray"), std::string("lab")}) {
            const core::Document doc = run(as_kind(paint_book(), kind), Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "paint"},
                                                                              {"points", Json::parse("[[5, 10], [45, 10]]")}, {"width_mm", 2.0},
                                                                              {"rgb", Json::array({0, 0, 0})}});
            QCOMPARE(doc.page(0).layers[0].stroke_count(), std::size_t(1));
            const std::vector<Rgba> before = page_colours(doc);
            const Json gradient{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", "paint"}, {"from", Json::array({5, 25})}, {"to", Json::array({45, 25})},
                                {"rgb_from", Json::array({0, 0, 200})}, {"rgb_to", Json::array({200, 0, 0})}};
            const Json fill{{"op", "fill"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 25}, {"y_mm", 30}, {"rgb", Json::array({0, 150, 0})}};
            for (const Json& op : {gradient, fill}) {
                const core::Document painted = run(doc, op);
                const core::Layer& layer = painted.page(0).layers[0];
                QVERIFY2(layer.stroke_count() == 1 && layer.patches.empty(), qPrintable(QString::fromStdString(kind + " " + op["op"].get<std::string>())));
                QVERIFY(layer.ink_raster != nullptr && layer.ink_raster != doc.page(0).layers[0].ink_raster);
                const std::vector<Rgba> after = page_colours(painted);
                for (unsigned k = 0; k < 3; ++k) {  // (the line as it showed)
                    QVERIFY2(std::abs(after[at(25, 10)][k] - before[at(25, 10)][k]) < 0.03f,
                             qPrintable(QString("%1 %2: %3 %4").arg(kind.c_str()).arg(k).arg(after[at(25, 10)][k]).arg(before[at(25, 10)][k])));
                }
                float moved = 0;  // (the paint where the line is not)
                for (unsigned k = 0; k < 3; ++k) moved = std::max(moved, std::abs(after[at(25, 30)][k] - before[at(25, 30)][k]));
                QVERIFY2(moved > 0.1f, kind.c_str());
            }
            // a blend: the line made the layer's pixels
            Json multiplied = gradient;
            multiplied["blend"] = "multiply";
            const core::Document blended = run(doc, multiplied);
            QCOMPARE(blended.page(0).layers[0].stroke_count(), std::size_t(0));
        }
    }

    // (the coordinator's decision after E4's review) An 8-bit paint layer with a line on it: gradient_fill with this build's
    // keys in the 通常 mode lays a picture on it — under the line, which stays a line; its pixels as they were — at the
    // layer's grid (200 dpi; an opened picture's page its grid), in the box it paints; dither and transparency in the
    // picture's pixels; saved, opened and undone. A blend makes the line its pixels.
    void eightBitLayersKeepTheirLines() {
        const Json line{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "paint"}, {"points", Json::parse("[[5, 10], [45, 10]]")}, {"width_mm", 2.0}};
        const core::Document doc = run(paint_book(), line);
        Json op = black_white();
        op["dither"] = true;
        op["transparency"] = Json::parse("[[0, 1], [1, 0.5]]");
        const core::Document painted = run(doc, op);
        const core::Layer& layer = painted.page(0).layers[0];
        QCOMPARE(layer.stroke_count(), std::size_t(1));
        QCOMPARE(layer.patches.size(), std::size_t(1));
        QVERIFY(layer.raster_png == doc.page(0).layers[0].raster_png);
        const auto dpi_of = [](const core::Patch& patch) {
            const render::Image picture = render::selection::open_picture(*patch.png);
            return picture.width() / (patch.attrs["box"][2].get<double>() / kInch);
        };
        QVERIFY2(std::abs(dpi_of(layer.patches[0]) - 200) < 1, qPrintable(QString::number(dpi_of(layer.patches[0]))));
        {
            const render::Image picture = render::selection::open_picture(*layer.patches[0].png).convert("RGBA");
            const std::string bytes = picture.tobytes();
            // (the end of the drag: half transparent; along a column of one place the dither's levels differ)
            const auto alpha = [&](int x, int y) { return static_cast<unsigned char>(bytes[(std::size_t(y) * picture.width() + x) * 4 + 3]); };
            QVERIFY(std::abs(int(alpha(picture.width() - 3, 100)) - 128) <= 1);
            QCOMPARE(int(alpha(2, 100)), 255);
            std::set<int> levels;
            for (int y = 0; y < picture.height(); ++y) levels.insert(static_cast<unsigned char>(bytes[(std::size_t(y) * picture.width() + 200) * 4]));
            QVERIFY(levels.size() > 1);
        }
        // in a selection: the picture its box only
        Json inside = op;
        inside["area"] = Json{{"poly", Json::parse("[[10, 20], [30, 20], [30, 40], [10, 40]]")}};
        const core::Document part = run(doc, inside);
        const Json box = part.page(0).layers[0].patches.at(0).attrs["box"];
        QVERIFY2(std::abs(box[0].get<double>() - 10) < 0.2 && std::abs(box[2].get<double>() - 20) < 0.3, box.dump().c_str());
        // an opened picture's page (its grid "finest", 300 dpi here): the picture at the grid
        core::Document fine = run(paint_book(600), line);
        fine.edit_page(0).spec.grid = "finest";
        const core::Document fine_painted = run(fine, op);
        QVERIFY2(std::abs(dpi_of(fine_painted.page(0).layers[0].patches.at(0)) - 300) < 1, qPrintable(QString::number(dpi_of(fine_painted.page(0).layers[0].patches.at(0)))));
        // a blend: the line made the layer's pixels
        Json multiplied = op;
        multiplied["blend"] = "multiply";
        const core::Document blended = run(doc, multiplied);
        QCOMPARE(blended.page(0).layers[0].stroke_count(), std::size_t(0));
        QVERIFY(blended.page(0).layers[0].patches.empty());
        // saved, opened, undone, done again
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "e.genko";
        write_book(doc, dir);
        const auto done = run_ops(doc, Json::array({op}));
        save_next(done.doc, dir, done.journal_ops);
        const auto opened = storage::load_document(dir);
        QCOMPARE(opened.document.page(0).layers[0].stroke_count(), std::size_t(1));
        QCOMPARE(opened.document.page(0).layers[0].patches.size(), std::size_t(1));
        QCOMPARE(render::selection::open_picture(*opened.document.page(0).layers[0].patches[0].png).tobytes(),
                 render::selection::open_picture(*done.doc.page(0).layers[0].patches[0].png).tobytes());
        storage::restore(dir, "human:test", false, false);
        QVERIFY(storage::load_document(dir).document.page(0).layers[0].patches.empty());
        storage::restore(dir, "human:test", true, false);
        QCOMPARE(storage::load_document(dir).document.page(0).layers[0].patches.size(), std::size_t(1));
    }

    // op Undo and Redo: a gradient on a precise layer and a fill on an ink layer saved as a step of the journal, undone
    // (the layer's pixels as they were) and done again (as they were after)
    void undoneAndRedone() {
        const auto same = [](const Pixels& a, const Pixels& b) {
            if (a.size() != b.size() || a.all() != b.all()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                for (unsigned k = 0; k < a.all(); ++k)
                    if (a.at(i)[k] != b.at(i)[k]) return false;
            return true;
        };
        for (const std::string& kind : {std::string("16"), std::string("cmyk")}) {
            QTemporaryDir scratch;
            const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "u.genko";
            const core::Document doc = as_kind(paint_book(), kind);
            write_book(doc, dir);
            const Json op = kind == "16" ? black_white()
                                         : Json{{"op", "fill"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({10, 200, 30})}};
            const auto done = run_ops(doc, Json::array({op}));
            save_next(done.doc, dir, done.journal_ops);
            core::Document saved = storage::load_document(dir).document;
            core::Document after = done.doc;
            core::Document original = doc;
            QVERIFY2(same(whole(saved), whole(after)), kind.c_str());
            QVERIFY(!same(whole(saved), whole(original)));
            storage::restore(dir, "human:test", false, false);
            core::Document undone = storage::load_document(dir).document;
            QVERIFY2(same(whole(undone), whole(original)), kind.c_str());
            storage::restore(dir, "human:test", true, false);
            core::Document redone = storage::load_document(dir).document;
            QVERIFY2(same(whole(redone), whole(after)), kind.c_str());
        }
    }

    // (review P3-10) A gradient fill layer's transparency stops: saved (vector@1), opened the same, undone (no layer, no
    // vector@1), done again
    void transparencySavedAndUndone() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "t.genko";
        const core::Document doc = pen_book();
        write_book(doc, dir);
        const Json transparency = Json::parse("[[0.0, 1.0], [1.0, 0.25]]");
        const Json op{{"op", "add_layer"}, {"page", 1}, {"kind", "gradient"}, {"id", "g"},
                      {"gradient", Json{{"from", Json::array({0, 0})}, {"to", Json::array({50, 0})},
                                        {"stops", Json::parse("[[0, [0, 0, 0], 1], [1, [255, 255, 255], 1]]")}, {"transparency", transparency}}}};
        const auto done = run_ops(doc, Json::array({op}));
        save_next(done.doc, dir, done.journal_ops);
        const auto layer_g = [](const core::Document& d) -> const core::Layer* {
            for (const core::Layer& l : d.page(0).layers)
                if (l.id == "g") return &l;
            return nullptr;
        };
        const auto opened = storage::load_document(dir);
        QVERIFY2(opened.document.read_only_reason.empty(), opened.document.read_only_reason.c_str());
        QVERIFY(has_feature(opened.document, "vector@1"));
        QVERIFY(layer_g(opened.document) != nullptr);
        QCOMPARE(layer_g(opened.document)->fill->at("gradient").at("transparency"), transparency);
        storage::restore(dir, "human:test", false, false);
        const auto undone = storage::load_document(dir);
        QVERIFY(layer_g(undone.document) == nullptr);
        QVERIFY(!has_feature(undone.document, "vector@1"));
        storage::restore(dir, "human:test", true, false);
        const auto redone = storage::load_document(dir);
        QVERIFY(layer_g(redone.document) != nullptr && has_feature(redone.document, "vector@1"));
        QCOMPARE(layer_g(redone.document)->fill->at("gradient").at("transparency"), transparency);
    }

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
