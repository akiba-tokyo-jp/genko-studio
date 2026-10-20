// PC3 B2 (SPEC.md COLOR-06「マスク」「範囲の中身の編集」): the ops of a layer's mask and of an area's edge, as PhotoCraft's
// engine/src/layer_menu_cmds/tests.rs and extra_cmds.rs test them, on Genko's three ways (8 bits, precise, inks) —
// apply_layer_mask (the alpha × the mask as it shows, its density too; the mask taken off; a pen layer's lines made
// pixels; what is refused), mask_from_transparency (the alpha, grey as it is, the mask; the pixels opaque), a linked mask
// moved with its layer's pixels (transform_area's "mask": "follow", a warp too; "stay" its bytes), outline_area's band
// inside, straddling and outside the edge, of the area or of what the layer holds, with the alpha kept when asked,
// fill_area's preserve_transparency, paste's center_mm, and the masks of a precise or ink page at its finest grid.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/masks.hpp"
#include "render/fill.hpp"
#include "render/image.hpp"
#include "render/layer_masks.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

constexpr const char* kPaint = "paint0000001";
constexpr int kW = 472, kH = 630;  // (60 × 80 mm at 200 dpi)

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

// A book of one page 60 × 80 mm, its panels taken away.
core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("b2", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    return doc;
}

// The paint layer's alpha at a pixel of the working 200 dpi: x < 50 clear, the square 100..200 half, else opaque.
double alpha_at(int x, int y) { return x >= 100 && x < 200 && y >= 100 && y < 200 ? 128 / 255.0 : (x < 50 ? 0.0 : 1.0); }

core::Layer paint_layer() {
    core::Layer layer;
    layer.id = kPaint;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    return layer;
}

// An 8-bit page with a grey (100) paint layer of alpha_at.
core::Document eight() {
    core::Document doc = small_book();
    std::string bytes(std::size_t(kW) * kH * 4, '\0');
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (std::size_t(y) * kW + x) * 4;
            bytes[at] = bytes[at + 1] = bytes[at + 2] = static_cast<char>(100);
            bytes[at + 3] = static_cast<char>(std::lround(alpha_at(x, y) * 255));
        }
    }
    core::Layer layer = paint_layer();
    layer.raster_png = std::make_shared<const std::string>(render::write_png(render::Image::frombytes("RGBA", render::Size{kW, kH}, bytes)));
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

// A page of 16 or 32 bits with a precise paint layer of colour (0.3, 0.45, 0.6) — f32: red 1.5, past the screen's white —
// and alpha_at.
core::Document precise(const std::string& precision) {
    core::Document doc = run(small_book(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", precision == "f32" ? 32 : 16}});
    const double red = precision == "f32" ? 1.5 : 0.3;
    core::Layer layer = paint_layer();
    layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(kW, kH, precision, [&](std::size_t i) {
        const int x = static_cast<int>(i % kW), y = static_cast<int>(i / kW);
        return std::array<double, 4>{red, 0.45, 0.6, alpha_at(x, y)};
    }));
    doc.edit_page(0).layers.push_back(std::move(layer));
    if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end())
        doc.features.emplace_back(core::kColorRasterFeature);
    return doc;
}

// A CMYK page (16 bits, the built-in coated profile) with an ink paint layer of C 20 % M 40 % and alpha_at.
core::Document inks() {
    core::Document doc = run(small_book(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Cmyk, 16, kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const double a = alpha_at(x, y);
            if (!(a > 0)) continue;
            const std::size_t i = std::size_t(y) * kW + x;
            p.set(i, 0, 0.2);
            p.set(i, 1, 0.4);
            p.set(i, 2, 0.0);
            p.set(i, 3, 0.0);
            p.set(i, 4, a);
        }
    }
    core::Layer layer = paint_layer();
    layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + std::string(id));
}

// The layer's alpha at a pixel of its own grid (any of the three ways), 0..1.
double alpha_of(const core::Layer& layer, int x, int y) {
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        return view.pixel(std::size_t(y) * view.width() + std::size_t(x))[3];
    }
    if (layer.ink_raster) {
        const core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
        return p.value(std::size_t(y) * p.width + std::size_t(x), p.colours());
    }
    return render::open_image(*layer.raster_png).convert("RGBA").getpixel(x, y)[3] / 255.0;
}

render::Image mask_pixels(const core::Layer& layer) { return render::open_image(*layer.mask->png).convert("L"); }

Json square(double x0, double y0, double x1, double y1) {
    return Json{{"poly", Json::array({Json::array({x0, y0}), Json::array({x1, y0}), Json::array({x1, y1}), Json::array({x0, y1})})}};
}

// The page drawn for print at 100 dpi (RGB).
render::Image drawn(const core::Document& doc) {
    render::RenderOptions options;
    options.mode = "print";
    return render::render_page(doc.page(0), 100, options, &doc).image.convert("RGB");
}

// The share of the two pictures' pixels more than 3 levels apart (resampled at a different grid: an edge's pixels).
double share_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.tobytes(), y = b.tobytes();
    if (x.size() != y.size() || x.empty()) return 1.0;
    std::size_t apart = 0;
    for (std::size_t i = 0; i < x.size(); i += 3) {
        int most = 0;
        for (std::size_t c = 0; c < 3; ++c) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i + c])) - int(static_cast<unsigned char>(y[i + c]))));
        apart += most > 3 ? 1 : 0;
    }
    return static_cast<double>(apart) / static_cast<double>(x.size() / 3);
}

// A patch's mask over its box: the columns (mm) along the row at `y_mm` its coverage is at least half on.
std::vector<double> covered_columns(const core::Patch& patch, double y_mm) {
    const Json box = patch.attrs["box"];
    const render::Image mask = render::open_image(*patch.png).convert("L");
    const double k = render::fills::kFillDpi / 25.4;
    const int row = static_cast<int>(std::floor((y_mm - box[1].get<double>()) * k));
    std::vector<double> out;
    if (row < 0 || row >= mask.height()) return out;
    for (int x = 0; x < mask.width(); ++x)
        if (mask.getpixel(x, row)[0] >= 128) out.push_back(box[0].get<double>() + (x + 0.5) / k);
    return out;
}

}  // namespace

class TestMasks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { render::ops_registry(); }

    // layer_menu_cmds/tests.rs apply_layer_mask: the alpha × the mask, the mask gone; its density applied with it; the
    // page drawn the same before and after; on the three ways (colours kept: only the alpha changes).
    void applyLayerMask_data() {
        QTest::addColumn<QString>("way");
        for (const char* way : {"eight", "u16", "f32", "cmyk"}) QTest::newRow(way) << QString::fromLatin1(way);
    }
    void applyLayerMask() {
        QFETCH(QString, way);
        const core::Document doc = way == "eight" ? eight() : way == "cmyk" ? inks() : precise(way.toStdString());
        // (the mask hides the left of x = 30 mm — 236 px at 200 dpi —, half of it at density 0.5 above y = 40 mm)
        core::Document masked = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(30, -5, 65, 85)}});
        const core::Document dense = run(masked, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"density", 0.5}});
        const core::Document applied = run(masked, Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", kPaint}});
        const core::Layer& layer = layer_of(applied, kPaint);
        QVERIFY(!layer.mask);
        QCOMPARE(alpha_of(layer, 150, 150), 0.0);                    // (hidden: the half square)
        QCOMPARE(alpha_of(layer, 100, 400), 0.0);                    // (hidden: opaque)
        QVERIFY(std::abs(alpha_of(layer, 300, 400) - 1.0) < 1e-4);   // (shown)
        QVERIFY(std::abs(alpha_of(layer, 40, 400)) < 1e-4);          // (clear stays clear)
        QVERIFY2(share_apart(drawn(masked), drawn(applied)) < 0.02, qPrintable(QString::number(share_apart(drawn(masked), drawn(applied)))));
        // (density 0.5: the hidden part kept at half)
        const core::Document dense_applied = run(dense, Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", kPaint}});
        const double half = alpha_of(layer_of(dense_applied, kPaint), 100, 400);
        QVERIFY2(std::abs(half - 0.5) < 0.01, qPrintable(QString::number(half)));
        QVERIFY(share_apart(drawn(dense), drawn(dense_applied)) < 0.02);
        // (the colours kept)
        if (way == "u16" || way == "f32") {
            const core::ColorRasterView view(*layer.color_raster);
            const auto p = view.pixel(std::size_t(400) * view.width() + 300);
            QVERIFY(std::abs(p[0] - (way == "f32" ? 1.5 : 0.3)) < 1e-4 && std::abs(p[1] - 0.45) < 1e-4);
        } else if (way == "cmyk") {
            const core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
            QVERIFY(std::abs(p.value(std::size_t(400) * p.width + 300, 1) - 0.4) < 1e-3);
        }
    }

    // A pen layer's lines are made pixels first (it is a paint layer after); the refusals.
    void applyLayerMaskRefusals() {
        core::Document doc = eight();
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"name", "ペン"}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"width_mm", 1.0},
                                             {"points", Json::array({Json::array({5.0, 20.0}), Json::array({55.0, 20.0})})}, {"stabilize", 0}},
                                        Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "pen"}, {"area", square(30, -5, 65, 85)}}}))
                  .doc;
        const core::Document applied = run(doc, Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", "pen"}});
        const core::Layer& pen = layer_of(applied, "pen");
        QCOMPARE(pen.kind, core::LayerKind::Raster);
        QVERIFY(!pen.mask && pen.raster_png && pen.stroke_count() == 0);
        QVERIFY(alpha_of(pen, render::mm_to_px(45, 200), render::mm_to_px(20, 200)) > 0.9);
        QCOMPARE(alpha_of(pen, render::mm_to_px(15, 200), render::mm_to_px(20, 200)), 0.0);
        QVERIFY(refusal(eight(), Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", kPaint}}).find("has no layer mask") != std::string::npos);
        core::Document locked = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"locked", true}});
        QVERIFY(refusal(locked, Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", "pen"}}).find("locked") != std::string::npos);
        core::Document fill = run_ops(eight(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "fill"}},
                                                            Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "fill"}, {"fill", "hide"}}}))
                                  .doc;
        QVERIFY(refusal(fill, Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", "fill"}}).find("rasterize it first") != std::string::npos);
        QVERIFY(refusal(fill, Json{{"op", "mask_from_transparency"}, {"page", 1}, {"id", "fill"}}).find("rasterize it first") != std::string::npos);
    }

    // layer_menu_cmds/tests.rs mask_from_transparency: the alpha, grey as it is, the mask; the pixels opaque over the box of
    // what the layer held; drawn the same; a new mask linked when asked.
    void maskFromTransparency_data() { applyLayerMask_data(); }
    void maskFromTransparency() {
        QFETCH(QString, way);
        const core::Document doc = way == "eight" ? eight() : way == "cmyk" ? inks() : precise(way.toStdString());
        const core::Document made = run(doc, Json{{"op", "mask_from_transparency"}, {"page", 1}, {"id", kPaint}, {"linked", true}});
        const core::Layer& layer = layer_of(made, kPaint);
        QVERIFY(layer.mask && layer.mask->linked && layer.mask->enabled);
        const render::Image mask = mask_pixels(layer);
        const render::Size grid = render::layer_masks::grid_size(made.page(0));
        QCOMPARE(mask.size(), grid);
        const double k = grid.width / 60.0;  // (mask pixels per mm)
        const auto at = [&](double x_mm, double y_mm) { return int(mask.getpixel(int(x_mm * k), int(y_mm * k))[0]); };
        QCOMPARE(at(3, 40), 0);              // (clear)
        QCOMPARE(at(45, 60), 255);           // (opaque)
        QVERIFY(std::abs(at(19, 19) - 128) <= 2);  // (half: grey as it is)
        QVERIFY(std::abs(alpha_of(layer, 150, 150) - 1.0) < 1e-4);  // (opaque now)
        QVERIFY(std::abs(alpha_of(layer, 300, 300) - 1.0) < 1e-4);
        QVERIFY2(share_apart(drawn(doc), drawn(made)) < 0.03, qPrintable(QString::number(share_apart(drawn(doc), drawn(made)))));
    }

    // commands.rs translate_layer: a linked mask moves with the layer's pixels (follow), the part it left its background;
    // "stay" leaves its bytes; a warp moves it as it moves the pixels.
    void linkedMaskMoves() {
        core::Document doc = run(eight(), Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(20, 20, 30, 30)}, {"linked", true}});
        const Json whole = square(-30, -30, 90, 110);
        const auto moved_by = [&](const std::string& how) {
            return run(doc, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", whole}, {"matrix", Json::array({1, 0, 0, 1, 10, 5})},
                                 {"mask", how}});
        };
        const core::Document stays = moved_by("stay");
        QCOMPARE(*layer_of(stays, kPaint).mask->png, *layer_of(doc, kPaint).mask->png);
        const core::Document follows = moved_by("follow");
        const render::Image mask = mask_pixels(layer_of(follows, kPaint));
        QCOMPARE(mask.size(), mask_pixels(layer_of(doc, kPaint)).size());
        const double k = mask.width() / 60.0;
        const auto at = [&](double x_mm, double y_mm) { return int(mask.getpixel(int(x_mm * k), int(y_mm * k))[0]); };
        QCOMPARE(at(35, 30), 255);  // (the square, now at 30..40 × 25..35)
        QCOMPARE(at(25, 22), 0);    // (where it was: the background, hidden)
        QCOMPARE(at(5, 2), 0);      // (the strip it left at the top: the background)
        QVERIFY(layer_of(follows, kPaint).mask->linked);
        // (the pixels moved with it: what was at 0..10 mm left clear)
        QCOMPARE(alpha_of(layer_of(doc, kPaint), render::mm_to_px(8, 200), render::mm_to_px(60, 200)), 1.0);
        QCOMPARE(alpha_of(layer_of(follows, kPaint), render::mm_to_px(8, 200), render::mm_to_px(60, 200)), 0.0);
        // a mask that reveals around: the part it left shows
        core::Document reveal = run(eight(), Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(20, 20, 30, 30)}, {"invert", true}});
        reveal = run(reveal, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", whole}, {"matrix", Json::array({1, 0, 0, 1, 10, 5})},
                                  {"mask", "follow"}});
        const render::Image shown = mask_pixels(layer_of(reveal, kPaint));
        QCOMPARE(int(shown.getpixel(int(35 * k), int(30 * k))[0]), 0);
        QCOMPARE(int(shown.getpixel(int(5 * k), int(2 * k))[0]), 255);
        // a warp: the square's part of the mask goes where the warp takes the area's box
        const Json warp{{"perspective", Json::array({Json::array({30, 30}), Json::array({50, 30}), Json::array({50, 50}), Json::array({30, 50})})}};
        const core::Document warped = run(doc, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", square(20, 20, 30, 30)},
                                                    {"warp", warp}, {"mask", "follow"}});
        const render::Image bent = mask_pixels(layer_of(warped, kPaint));
        QCOMPARE(int(bent.getpixel(int(40 * k), int(40 * k))[0]), 255);
        QCOMPARE(int(bent.getpixel(int(25 * k), int(25 * k))[0]), 0);
        QVERIFY(refusal(doc, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", whole}, {"matrix", Json::array({1, 0, 0, 1, 1, 1})},
                                  {"mask", "drag"}})
                    .find("mask must be follow or stay") != std::string::npos);
    }

    // extra_cmds.rs stroke: a band of the width inside, straddling or outside the area's edge.
    void outlineBands_data() {
        QTest::addColumn<QString>("location");
        QTest::addColumn<double>("from");
        QTest::addColumn<double>("to");
        QTest::newRow("inside") << QString("inside") << 20.0 << 22.0;
        QTest::newRow("center") << QString("center") << 19.0 << 21.0;
        QTest::newRow("outside") << QString("outside") << 18.0 << 20.0;
    }
    void outlineBands() {
        QFETCH(QString, location);
        QFETCH(double, from);
        QFETCH(double, to);
        core::Document doc = run(small_book(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"name", "ペン"}});
        const core::Document stroked = run(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", "pen"}, {"area", square(20, 20, 40, 40)},
                                                     {"width_mm", 2.0}, {"rgb", Json::array({200, 0, 0})}, {"location", location.toStdString()}});
        const core::Layer& pen = layer_of(stroked, "pen");
        QCOMPARE(pen.patches.size(), std::size_t(1));
        QCOMPARE(pen.patches[0].attrs["rgb"], Json::array({200, 0, 0}));
        const std::vector<double> columns = covered_columns(pen.patches[0], 30);
        QVERIFY(!columns.empty());
        std::vector<double> left;
        for (const double x : columns) if (x < 30) left.push_back(x);
        QVERIFY(!left.empty());
        const double tolerance = 0.2;  // (a pixel and a half at 300 dpi)
        QVERIFY2(std::abs(left.front() - from) < tolerance && std::abs(left.back() - to) < tolerance,
                 qPrintable(QString("%1..%2").arg(left.front()).arg(left.back())));
        QVERIFY(std::none_of(columns.begin(), columns.end(), [&](double x) { return x > to + 0.3 && x < 40 - (to - 20) - 0.3; }));  // (hollow)
    }

    // The edge of what the layer holds when no area is given; the alpha kept when asked; on the three ways.
    void outlineLayerEdge_data() { applyLayerMask_data(); }
    void outlineLayerEdge() {
        QFETCH(QString, way);
        const core::Document doc = way == "eight" ? eight() : way == "cmyk" ? inks() : precise(way.toStdString());
        // (the layer's edge: x = 50 px at 200 dpi, 6.35 mm; a band outside it would lie on the clear part: kept clear)
        const core::Document kept = run(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", kPaint}, {"width_mm", 2.0}, {"location", "outside"},
                                                  {"rgb", Json::array({255, 0, 0})}, {"preserve_transparency", true}});
        QCOMPARE(alpha_of(layer_of(kept, kPaint), 30, 400), 0.0);
        const core::Document inside = run(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", kPaint}, {"width_mm", 2.0}, {"location", "inside"},
                                                    {"rgb", Json::array({255, 0, 0})}, {"preserve_transparency", true}});
        const render::Image page = drawn(inside), before = drawn(doc);
        // (inside the edge, the colour laid: red where it was grey; past the band, as it was)
        const auto red = page.getpixel(render::mm_to_px(7, 100), render::mm_to_px(60, 100));
        QVERIFY2(red[0] > red[1] + 60, qPrintable(QString("%1 %2 %3").arg(red[0]).arg(red[1]).arg(red[2])));
        QCOMPARE(page.getpixel(render::mm_to_px(40, 100), render::mm_to_px(60, 100)), before.getpixel(render::mm_to_px(40, 100), render::mm_to_px(60, 100)));
        QVERIFY(std::abs(alpha_of(layer_of(inside, kPaint), render::mm_to_px(7, 200), render::mm_to_px(60, 200)) - 1.0) < 1e-3);
        // (without the alpha kept, a patch on the layer — drawn into its pixels on a precise or ink page)
        const core::Document plain = run(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", kPaint}, {"width_mm", 2.0}, {"location", "outside"},
                                                   {"rgb", Json::array({255, 0, 0})}});
        QVERIFY(alpha_of(layer_of(plain, kPaint), render::mm_to_px(5.4, 200), render::mm_to_px(60, 200)) > 0.9 || !layer_of(plain, kPaint).patches.empty());
        QVERIFY(refusal(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", kPaint}, {"width_mm", 0}}).find("more than 0") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", kPaint}, {"width_mm", 1}, {"location", "around"}}).find("location") !=
                std::string::npos);
    }

    // fill_area's preserve_transparency: the colour laid where the layer shows, its alpha as it was.
    void fillKeepingAlpha_data() { applyLayerMask_data(); }
    void fillKeepingAlpha() {
        QFETCH(QString, way);
        const core::Document doc = way == "eight" ? eight() : way == "cmyk" ? inks() : precise(way.toStdString());
        const core::Document filled = run(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", square(0, 0, 40, 40)},
                                                    {"rgb", Json::array({255, 0, 0})}, {"preserve_transparency", true}});
        const core::Layer& layer = layer_of(filled, kPaint);
        QVERIFY(layer.patches.empty());
        for (const auto& [x, y] : {std::pair{30, 300}, std::pair{150, 150}, std::pair{300, 300}, std::pair{300, 600}})
            QVERIFY2(std::abs(alpha_of(layer, x, y) - alpha_of(layer_of(doc, kPaint), x, y)) < 2e-3, qPrintable(QString("%1,%2").arg(x).arg(y)));
        const render::Image page = drawn(filled);
        const auto red = page.getpixel(render::mm_to_px(30, 100), render::mm_to_px(30, 100));
        QVERIFY2(red[0] > 200 && red[1] < 80, qPrintable(QString("%1 %2 %3").arg(red[0]).arg(red[1]).arg(red[2])));
        QCOMPARE(page.getpixel(render::mm_to_px(50, 100), render::mm_to_px(60, 100)), drawn(doc).getpixel(render::mm_to_px(50, 100), render::mm_to_px(60, 100)));
    }

    // paste's center_mm: the items' box centred there, through the matrix.
    void pasteAtCentre() {
        core::Document doc = run(small_book(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"name", "ペン"}});
        const Json stroke{{"points", Json::array({Json::array({5.0, 5.0}), Json::array({15.0, 9.0})})}, {"width_mm", 0.5}};
        const Json items{{"strokes", Json::array({stroke})}, {"patches", Json::array()}};
        const core::Document pasted = run(doc, Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pen"}, {"items", items}, {"center_mm", Json::array({30, 40})}});
        const auto& points = layer_of(pasted, "pen").strokes->items.at(0)->points;
        QVERIFY(std::abs((points.front().x + points.back().x) / 2 - 30) < 1e-6 && std::abs((points.front().y + points.back().y) / 2 - 40) < 1e-6);
        const core::ApplyResult warned = run_ops(doc, Json::array({Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pen"}, {"items", items}, {"center_mm", Json::array({30, 40})}}}));
        QVERIFY(warned.warnings.empty());  // (center_mm is the op's own key: not an unknown one)
        QVERIFY(refusal(doc, Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pen"}, {"items", items}, {"center_mm", 3}}).find("center_mm") != std::string::npos);
    }

    // The masks of a precise or ink page at its finest grid (8-bit pages: 150 dpi, as before); its settings alone
    // changed keep its picture.
    void maskGrid() {
        const auto side = [](const core::Document& d) { return mask_pixels(layer_of(d, kPaint)).width(); };
        const core::Document plain = run(eight(), Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(10, 10, 30, 30)}});
        QCOMPARE(side(plain), render::mm_to_px(60, 150));
        QCOMPARE(render::layer_masks::grid_dpi(plain.page(0)), 150);
        for (const core::Document& deep : {precise("u16"), inks()}) {
            const core::Document masked = run(deep, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(10, 10, 30, 30)}});
            QCOMPARE(render::layer_masks::grid_dpi(masked.page(0)), 200);
            QCOMPARE(side(masked), render::mm_to_px(60, 200));
            const core::Document linked = run(masked, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"linked", true}});
            QCOMPARE(*layer_of(linked, kPaint).mask->png, *layer_of(masked, kPaint).mask->png);
            // (a pixel op's "layer_mask": true on a layer without one: the mask it makes at the grid too — the
            // coordinator's decision after the PC3 review: it was made at 150 dpi)
            const core::Document lent = run(deep, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", kPaint}, {"layer_mask", true},
                                                       {"area", square(10, 10, 30, 30)}, {"rgb", Json::array({0, 0, 0})}});
            QVERIFY(layer_of(lent, kPaint).mask.has_value());
            QCOMPARE(mask_pixels(layer_of(lent, kPaint)).size(), render::layer_masks::grid_size(lent.page(0)));
        }
        {  // (an 8-bit page's: 150 dpi, as before)
            const core::Document lent = run(eight(), Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", kPaint}, {"layer_mask", true},
                                                          {"area", square(10, 10, 30, 30)}, {"rgb", Json::array({0, 0, 0})}});
            QCOMPARE(side(lent), render::mm_to_px(60, 150));
        }
        // (a 600 dpi raster on the page: its grid, 600 at most)
        core::Document fine = precise("u16");
        const int w = render::mm_to_px(60, 600), h = render::mm_to_px(80, 600);
        fine.edit_page(0).layers.back().color_raster = std::make_shared<const std::string>(
            core::encode_color_pixels(w, h, "u16", [](std::size_t) { return std::array<double, 4>{0.5, 0.5, 0.5, 1}; }));
        QCOMPARE(render::layer_masks::grid_dpi(fine.page(0)), 600);
    }

    // Layer › Rasterize › Vector Mask (rasterize_layer vector_mask, C-a) with the layer mask's settings (B2): the layer drawn
    // as it was — the layer mask's density and feather drawn in (not laid again on the product) —, a new mask linked as
    // the vector mask was, a vector mask that is off alone made a mask that is off; at the page's mask grid. Where a mask
    // that is off would be lost (a layer mask that is off, or a vector mask that is off over a layer mask: the
    // coordinator's decision after the PC3 review), refused, the book as it was.
    void rasterizeVectorMask_data() { applyLayerMask_data(); }
    void rasterizeVectorMask() {
        QFETCH(QString, way);
        const core::Document doc = way == "eight" ? eight() : way == "cmyk" ? inks() : precise(way.toStdString());
        // (the vector mask shows x ≥ 30 mm)
        const Json path{{"subpaths", Json::array({Json{{"closed", true},
                                                      {"knots", Json::array({Json::array({30, -5}), Json::array({65, -5}), Json::array({65, 85}), Json::array({30, 85})})}}})}};
        const core::Document vectored = run(doc, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", kPaint}, {"path", path}});
        const auto rasterized = [](const core::Document& d) {
            return run(d, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", kPaint}, {"vector_mask", true}});
        };
        const auto same_look = [](const core::Document& a, const core::Document& b) { return share_apart(drawn(a), drawn(b)) < 0.02; };
        // no layer mask: one made, linked as the vector mask is, at the grid
        const core::Document made = rasterized(vectored);
        QVERIFY(!layer_of(made, kPaint).vector_mask && layer_of(made, kPaint).mask && layer_of(made, kPaint).mask->linked);
        QCOMPARE(mask_pixels(layer_of(made, kPaint)).size(), render::layer_masks::grid_size(made.page(0)));
        QVERIFY2(same_look(vectored, made), qPrintable(QString::number(share_apart(drawn(vectored), drawn(made)))));
        // a layer mask that is off (it hides x ≥ 45 mm): refused (the vector mask's picture would take its place), the
        // vector mask off too as well; used again, drawn in
        core::Document off = run(vectored, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(-5, -5, 45, 85)}});
        off = run(off, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"enabled", false}});
        const Json rasterize_op{{"op", "rasterize_layer"}, {"page", 1}, {"id", kPaint}, {"vector_mask", true}};
        QVERIFY2(refusal(off, rasterize_op).find("the layer mask of layer " + std::string(kPaint) + " is off") != std::string::npos,
                 refusal(off, rasterize_op).c_str());
        const core::Document both_off = run(off, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", kPaint}, {"enabled", false}});
        QVERIFY(refusal(both_off, rasterize_op).find("is off") != std::string::npos);
        const core::Document on_again = run(off, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"enabled", true}});
        const core::Document on_done = rasterized(on_again);
        QVERIFY2(same_look(on_again, on_done), qPrintable(QString::number(share_apart(drawn(on_again), drawn(on_done)))));
        QVERIFY(!layer_of(on_done, kPaint).vector_mask && layer_of(on_done, kPaint).mask->enabled);
        // a layer mask at density 0.5 (and feathered): drawn in once
        core::Document dense = run(vectored, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", kPaint}, {"area", square(-5, -5, 45, 85)},
                                                  {"density", 0.5}, {"feather_mm", 1.0}, {"linked", true}});
        const core::Document dense_done = rasterized(dense);
        QVERIFY2(same_look(dense, dense_done), qPrintable(QString::number(share_apart(drawn(dense), drawn(dense_done)))));
        const core::Mask& baked = *layer_of(dense_done, kPaint).mask;
        QVERIFY(baked.density == 1.0 && baked.feather_mm == 0.0 && baked.linked && baked.enabled);
        // a vector mask that is off over a layer mask: refused (it would be taken off unseen); without a layer mask, made a
        // mask that is off
        const core::Document vector_off = run(dense, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", kPaint}, {"enabled", false}});
        QVERIFY2(refusal(vector_off, rasterize_op).find("the vector mask of layer " + std::string(kPaint) + " is off") != std::string::npos,
                 refusal(vector_off, rasterize_op).c_str());
        const core::Document alone_off = run(vectored, Json{{"op", "set_vector_mask"}, {"page", 1}, {"id", kPaint}, {"enabled", false}});
        const core::Document alone_off_done = rasterized(alone_off);
        QVERIFY(layer_of(alone_off_done, kPaint).mask && !layer_of(alone_off_done, kPaint).mask->enabled);
        QVERIFY(same_look(alone_off, alone_off_done));
        // (a 600 dpi layer on a precise or ink page: the mask at 600 dpi)
        if (way != "eight") {
            core::Document fine = vectored;
            const int w = render::mm_to_px(60, 600), h = render::mm_to_px(80, 600);
            core::Layer& layer = fine.edit_page(0).layers.back();
            if (way == "cmyk") layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(core::blank_ink_pixels(core::InkSpace::Cmyk, 16, w, h)));
            else layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(w, h, "u16", [](std::size_t) { return std::array<double, 4>{0.5, 0.5, 0.5, 1}; }));
            QCOMPARE(mask_pixels(layer_of(rasterized(fine), kPaint)).width(), w);
        }
    }

    // The selection of a precise or ink page at its finest grid too (the coordinator's 8-2 (b)): an anti-aliased area
    // resolved there, its grey edge one pixel of a 600 dpi layer wide, not a 200 dpi pixel stretched over three; an 8-bit
    // page's at 200 dpi, as before.
    void selectionGrid_data() {
        QTest::addColumn<QString>("way");
        for (const char* way : {"u16", "cmyk"}) QTest::newRow(way) << QString::fromLatin1(way);
    }
    void selectionGrid() {
        QFETCH(QString, way);
        const Json area{{"poly", Json::array({Json::array({10.0, 10.0}), Json::array({30.05, 10.0}), Json::array({30.05, 50.0}), Json::array({10.0, 50.0})})},
                        {"antialias", true}};
        const auto mask_width = [](const Json& resolved) { return render::open_image(render::selection::b64decode(resolved["mask"]["png"])).width(); };
        // (8 bits: 200 dpi, as before)
        const core::Document plain = eight();
        QCOMPARE(render::selection::grid_dpi(plain.page(0)), 200);
        QVERIFY(std::abs(mask_width(render::selection::resolve(area, plain.page(0), &plain)) - 20.05 * 200 / 25.4) < 3);
        // (a 600 dpi layer on the page: its grid)
        core::Document fine = way == "cmyk" ? inks() : precise("u16");
        const int w = render::mm_to_px(60, 600), h = render::mm_to_px(80, 600);
        core::Layer& layer = fine.edit_page(0).layers.back();
        if (way == "cmyk") {
            core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Cmyk, 16, w, h);
            for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
                p.set(i, 0, 0.2);
                p.set(i, 4, 1.0);
            }
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        } else {
            layer.color_raster = std::make_shared<const std::string>(
                core::encode_color_pixels(w, h, "u16", [](std::size_t) { return std::array<double, 4>{0.5, 0.5, 0.5, 1}; }));
        }
        QCOMPARE(render::selection::grid_dpi(fine.page(0)), 600);
        QVERIFY(std::abs(mask_width(render::selection::resolve(area, fine.page(0), &fine)) - 20.05 * 600 / 25.4) < 3);
        // (the bus resolves the op's area there: the layer's pixels partly cleared across one pixel of its own)
        const core::Document cleared = run(fine, Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", kPaint}, {"area", area}});
        const core::Layer& done = layer_of(cleared, kPaint);
        const int row = render::mm_to_px(30, 600);
        int partial = 0;
        for (int x = render::mm_to_px(27, 600); x < render::mm_to_px(33, 600); ++x) {
            const double a = alpha_of(done, x, row);
            if (a > 0.02 && a < 0.98) ++partial;
        }
        QVERIFY2(partial >= 1 && partial <= 2, qPrintable(QString::number(partial)));
        QVERIFY(alpha_of(done, render::mm_to_px(29, 600), row) < 0.02 && alpha_of(done, render::mm_to_px(31, 600), row) > 0.98);
    }
};

QTEST_GUILESS_MAIN(TestMasks)
#include "test_masks.moc"
