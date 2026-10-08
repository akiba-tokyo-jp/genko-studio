#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <functional>
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"
#include "core/json.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "render/ops_registry.hpp"
#include "render/fill.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/warp.hpp"
using namespace genko;
using core::Json;

// The eraser and the selection on high-precision colour pixels (native.color_raster_v1): what they leave is still the
// layer's own u16/f32 samples — never an 8-bit picture — and every pixel they do not reach keeps its bytes.
namespace {
using Pixel = std::array<double, 4>;
constexpr double kInch = 25.4;  // a page of 25.4 mm: 200 px at the working 200 dpi

core::Document page_with(const std::string& precision, std::uint32_t w, std::uint32_t h,
                         const std::function<Pixel(std::uint32_t, std::uint32_t)>& at) {
    auto doc = core::new_episode("高精度編集", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0); p.numero = false; p.frames.clear(); p.layers.clear();
    Json pixels = Json::array();
    for (std::uint32_t y = 0; y < h; ++y) for (std::uint32_t x = 0; x < w; ++x) {
        const Pixel v = at(x, y);
        for (unsigned c = 0; c < 4; ++c) {
            if (precision == "u16") pixels.push_back(static_cast<std::int64_t>(std::lround(v[c] * 65535)));
            else pixels.push_back(static_cast<double>(static_cast<float>(v[c])));
        }
    }
    const Json put{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h}, {"precision", precision}, {"pixels", pixels}};
    doc = core::CommandBus().apply(doc, Json::array({put}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}
// Every pixel its own colour (f32: some beyond 1 and below 0, as HDR keeps them); opaque, except a band at 0.6.
Pixel patterned(std::uint32_t x, std::uint32_t y, bool hdr) {
    const double r = (x * 37 + y * 11) % 251 / 250.0, g = (x * 7 + y * 53) % 241 / 240.0, b = (x * 3 + y * 5) % 239 / 238.0;
    const double a = y >= 150 && y < 160 ? .6 : 1;
    if (hdr) return {r * 1.75 - .125, g, b + .25, a};
    return {r, g, b, a};
}
core::Document patterned_page(const QString& precision, std::uint32_t side = 200) {
    const bool hdr = precision == "f32";
    return page_with(precision.toStdString(), side, side, [&](std::uint32_t x, std::uint32_t y) { return patterned(x, y, hdr); });
}
core::Document edit(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc;
}
const core::Layer& paint(const core::Document& doc) { return doc.page(0).layers[0]; }
core::ColorRasterView pixels(const core::Document& doc) { return core::ColorRasterView(*paint(doc).color_raster); }
std::string_view sample_bytes(const core::Document& doc, std::size_t i) {
    const auto& bytes = *paint(doc).color_raster;
    const std::size_t size = bytes[5];
    return std::string_view(bytes).substr(16 + i * 4 * size, 4 * size);
}
// The same op on an 8-bit paint layer of the same id, all white and opaque: the alpha it leaves (Python's eraser).
std::string eight_bit_alpha(const Json& op, std::uint32_t side) {
    auto doc = core::new_episode("8bit", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0); p.numero = false; p.frames.clear(); p.layers.clear();
    core::Layer layer; layer.id = "paint"; layer.role = core::LayerRole::User; layer.kind = core::LayerKind::Raster; layer.panel_clip = false;
    const int s = static_cast<int>(side);
    render::raster::save_raster(p, layer, render::Image::create("RGBA", render::Size{s, s}, render::Ink{255, 255, 255, 255}));
    p.layers.push_back(layer);
    const auto done = edit(doc, op);
    return render::selection::open_picture(*paint(done).raster_png).getchannel(3).tobytes();
}
void same_book_after_reopening(const core::Document& doc) {
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdString()) / "book";
    storage::AssetStore store(path);
    const auto text = storage::project_json_v4(doc, store);
    const auto loaded = storage::load_document_text(text, path);
    QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
    QCOMPARE(*paint(loaded.document).color_raster, *paint(doc).color_raster);
    QCOMPARE(bool(paint(loaded.document).mask), bool(paint(doc).mask));
    for (const char* mode : {"proof", "print"}) {
        auto options = render::proof_options(); options.mode = mode;
        QCOMPARE(render::render_page(loaded.document.page(0), 100, options, &loaded.document).image.tobytes(),
                 render::render_page(doc.page(0), 100, options, &doc).image.tobytes());
    }
}
// What the op leaves must still be the layer's precise samples, and the book it was applied to is as it was (Undo).
void still_precise(const core::Document& before, const core::Bytes& original, const core::Document& after, const QString& precision) {
    QCOMPARE(paint(before).color_raster, original);
    QVERIFY(paint(after).color_raster);
    QVERIFY(!paint(after).raster_png);
    QVERIFY(paint(after).patches.empty());
    QCOMPARE(paint(after).stroke_count(), std::size_t(0));
    QCOMPARE(pixels(after).metadata("")["precision"], Json(precision.toStdString()));
    QCOMPARE(pixels(after).width(), pixels(before).width());
    QCOMPARE(pixels(after).height(), pixels(before).height());
    QVERIFY(std::find(after.features.begin(), after.features.end(), core::kColorRasterFeature) != after.features.end());
}
double tolerance(const QString& precision) { return precision == "u16" ? .5000001 / 65535 : 1e-7; }
}  // namespace

class TestColorRasterEdit : public QObject {
    Q_OBJECT
private slots:
    void eraserOnPrecisePixels_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("texture");
        for (const QString precision : {"u16", "f32"})
            for (const QString texture : {"hard", "soft", "rough"})
                QTest::newRow((precision + "-" + texture).toUtf8().constData()) << precision << texture;
    }
    // The eraser takes from the alpha just what it takes from an 8-bit paint layer: where it went, the 8-bit layer
    // keeps a8 of 255, and the precise one loses 1 - a8/255 of its alpha (a hard eraser clears the pixel).
    void eraserOnPrecisePixels() {
        QFETCH(QString, precision); QFETCH(QString, texture);
        const auto doc = patterned_page(precision);
        const auto original = paint(doc).color_raster;
        const Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 3.0}, {"texture", texture.toStdString()},
                      {"points", Json::array({Json::array({3.0, 19.3}), Json::array({22.0, 19.8})})}};
        const auto erased = edit(doc, op);
        still_precise(doc, original, erased, precision);
        const auto oracle = eight_bit_alpha(op, 200);
        const auto before = pixels(doc), after = pixels(erased);
        std::size_t reached = 0;
        for (std::size_t i = 0; i < std::size_t(200) * 200; ++i) {
            const int kept = static_cast<unsigned char>(oracle[i]);
            if (kept == 255) {
                if (sample_bytes(erased, i) != sample_bytes(doc, i)) QFAIL(qPrintable(QString("pixel %1 was not erased but changed").arg(i)));
                continue;
            }
            ++reached;
            const Pixel was = before.pixel(i), now = after.pixel(i);
            if (texture == "hard") {
                QCOMPARE(kept, 0);
                QCOMPARE(now, (Pixel{0, 0, 0, 0}));
                continue;
            }
            const double expected = std::max(0.0, was[3] - (1 - kept / 255.0));
            if (std::abs(now[3] - expected) > tolerance(precision))
                QFAIL(qPrintable(QString("pixel %1 alpha %2, expected %3").arg(i).arg(now[3]).arg(expected)));
            for (unsigned c = 0; c < 3; ++c) QCOMPARE(now[c], was[c]);  // (straight alpha: the colour stays)
        }
        QVERIFY(reached > 500);
        same_book_after_reopening(erased);
    }

    void eraserOnRastersOfOtherSizes_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<int>("width"); QTest::addColumn<int>("height");
        for (const QString precision : {"u16", "f32"}) {
            QTest::newRow(qPrintable(precision + "-600dpi")) << precision << 600 << 600;
            QTest::newRow(qPrintable(precision + "-stretched")) << precision << 31 << 7;
        }
    }
    // A raster of its own size (600 dpi, or stretched over the page): erased where the eraser went on the page.
    void eraserOnRastersOfOtherSizes() {
        QFETCH(QString, precision); QFETCH(int, width); QFETCH(int, height);
        const auto w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(height);
        const auto doc = page_with(precision.toStdString(), w, h, [](std::uint32_t x, std::uint32_t y) {
            return Pixel{x % 5 / 4.0, y % 3 / 2.0, .5, 1};
        });
        const auto original = paint(doc).color_raster;
        // a hard eraser 4 mm wide across the middle of the page, from edge to edge
        const auto erased = edit(doc, Json{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0},
                                            {"points", Json::array({Json::array({-2.0, 12.7}), Json::array({27.4, 12.7})})}});
        still_precise(doc, original, erased, precision);
        const auto after = pixels(erased);
        for (std::uint32_t x = 0; x < w; ++x) {
            QCOMPARE(after.pixel(std::size_t(h / 2) * w + x)[3], 0.0);  // (the middle row: under the eraser)
            QCOMPARE(sample_bytes(erased, x), sample_bytes(doc, x));     // (the top row: 12.7 mm away)
            QCOMPARE(sample_bytes(erased, std::size_t(h - 1) * w + x), sample_bytes(doc, std::size_t(h - 1) * w + x));
        }
        same_book_after_reopening(erased);
    }

    void deleteAreaOnPrecisePixels_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QString("u16"); QTest::newRow("f32") << QString("f32");
    }
    void deleteAreaOnPrecisePixels() {
        QFETCH(QString, precision);
        const auto doc = patterned_page(precision);
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({2.54, 2.54}), Json::array({7.62, 2.54}), Json::array({7.62, 20.32}), Json::array({2.54, 20.32})})}};
        const auto deleted = edit(doc, Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}});
        still_precise(doc, original, deleted, precision);
        const auto drawn = render::selection::area_mask(area, 200);
        const auto inside = drawn.mask.tobytes();
        const auto after = pixels(deleted);
        for (std::uint32_t y = 0; y < 200; ++y) for (std::uint32_t x = 0; x < 200; ++x) {
            const std::size_t i = std::size_t(y) * 200 + x;
            const auto lx = std::int64_t(x) - drawn.x0, ly = std::int64_t(y) - drawn.y0;
            const bool in = lx >= 0 && ly >= 0 && lx < drawn.mask.width() && ly < drawn.mask.height() &&
                            static_cast<unsigned char>(inside[std::size_t(ly) * drawn.mask.width() + lx]) == 255;
            if (in) QCOMPARE(after.pixel(i)[3], 0.0);
            else if (sample_bytes(deleted, i) != sample_bytes(doc, i)) QFAIL(qPrintable(QString("pixel %1,%2 outside changed").arg(x).arg(y)));
        }
        same_book_after_reopening(deleted);
    }

    void moveKeepsEverySample_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("interp");
        for (const QString precision : {"u16", "f32"})
            for (const QString interp : {"nearest", "bilinear", "bicubic"})
                QTest::newRow(qPrintable(precision + "-" + interp)) << precision << interp;
    }
    // A move by whole pixels (2.54 mm = 20 px) carries every sample as it was: no 8-bit picture on the way, whatever
    // the resampling; where the area was and nothing came, it is clear.
    void moveKeepsEverySample() {
        QFETCH(QString, precision); QFETCH(QString, interp);
        const auto doc = patterned_page(precision);
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({2.54, 2.54}), Json::array({7.62, 2.54}), Json::array({7.62, 22.86}), Json::array({2.54, 22.86})})}};
        const auto moved = edit(doc, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}, {"interp", interp.toStdString()},
                                           {"matrix", Json::array({1.0, 0.0, 0.0, 1.0, 2.54, 0.0})}});
        still_precise(doc, original, moved, precision);
        const auto before = pixels(doc), after = pixels(moved);
        for (std::uint32_t y = 22; y < 178; ++y) {
            for (std::uint32_t x = 22; x < 58; ++x) {  // the area's inside (its edge pixels aside), now 20 px to the right
                const std::size_t from = std::size_t(y) * 200 + x, to = from + 20;
                const Pixel was = before.pixel(from), now = after.pixel(to);
                if (was[3] == 1) {
                    if (precision == "f32" ? sample_bytes(moved, to) != sample_bytes(doc, from) : now != was)
                        QFAIL(qPrintable(QString("%1,%2 moved to %3: %4 %5 %6 → %7 %8 %9").arg(x).arg(y).arg(x + 20)
                                         .arg(was[0]).arg(was[1]).arg(was[2]).arg(now[0]).arg(now[1]).arg(now[2])));
                } else if (x + 20 > 61) {  // (beyond the area's edge pixel at 60)
                    // half-clear over half-clear (the band): the moved pixel over what stayed there, in linear light
                    const double kept = before.pixel(to)[3];
                    QVERIFY(std::abs(now[3] - (was[3] + kept * (1 - was[3]))) <= tolerance(precision));
                }
            }
            for (std::uint32_t x = 22; x < 40; ++x) QCOMPARE(after.pixel(std::size_t(y) * 200 + x)[3], 0.0);  // left behind: clear
            for (std::uint32_t x = 82; x < 200; ++x)  // beyond: untouched
                if (sample_bytes(moved, std::size_t(y) * 200 + x) != sample_bytes(doc, std::size_t(y) * 200 + x)) QFAIL("an untouched pixel changed");
        }
        same_book_after_reopening(moved);
    }

    void turnAndWarpStayPrecise_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("how");
        for (const QString precision : {"u16", "f32"})
            for (const QString how : {"turn", "scale", "perspective", "mesh"})
                QTest::newRow(qPrintable(precision + "-" + how)) << precision << how;
    }
    // Turned a quarter about a pixel corner, every sample lands on a pixel centre: kept to the sample's precision.
    // Scaled or warped, the colours in between come from the samples (HDR above 1 stays above 1), never via 8 bits.
    void turnAndWarpStayPrecise() {
        QFETCH(QString, precision); QFETCH(QString, how);
        const bool hdr = precision == "f32";
        // one flat colour on a square of 60 px at (70, 70), clear elsewhere
        const Pixel flat = hdr ? Pixel{1.25, .0625, .5009765625, 1} : Pixel{40123 / 65535., 1031 / 65535., 60001 / 65535., 1};
        const auto doc = page_with(precision.toStdString(), 200, 200, [&](std::uint32_t x, std::uint32_t y) {
            return x >= 70 && x < 130 && y >= 70 && y < 130 ? flat : Pixel{0, 0, 0, 0};
        });
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({5.0, 5.0}), Json::array({20.0, 5.0}), Json::array({20.0, 20.0}), Json::array({5.0, 20.0})})}};
        Json op{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}, {"interp", "bilinear"}};
        if (how == "turn") op["matrix"] = Json::array({0.0, 1.0, -1.0, 0.0, 25.4, 0.0});  // a quarter turn about the middle
        else if (how == "scale") op["matrix"] = Json::array({1.5, 0.0, 0.0, 1.5, -6.35, -6.35});
        else if (how == "perspective")
            op["warp"] = Json{{"perspective", Json::array({Json::array({4.0, 6.0}), Json::array({21.0, 5.0}), Json::array({20.0, 20.0}), Json::array({5.0, 21.0})})}};
        else op["warp"] = Json{{"mesh", Json::array({Json::array({5.0, 5.0}), Json::array({12.5, 4.0}), Json::array({20.0, 5.0}),
                                                    Json::array({4.0, 12.5}), Json::array({12.0, 12.0}), Json::array({21.0, 12.5}),
                                                    Json::array({5.0, 20.0}), Json::array({12.5, 21.0}), Json::array({20.0, 20.0})})}};
        const auto changed = edit(doc, op);
        still_precise(doc, original, changed, precision);
        const auto after = pixels(changed);
        const std::size_t middle = std::size_t(100) * 200 + 100;
        for (unsigned c = 0; c < 4; ++c) QVERIFY(std::abs(after.pixel(middle)[c] - flat[c]) <= 1e-6);
        if (how == "turn") {
            const auto before = pixels(doc);
            for (std::size_t i = 0; i < std::size_t(200) * 200; ++i)
                for (unsigned c = 0; c < 4; ++c) QVERIFY(std::abs(after.pixel(i)[c] - before.pixel(i)[c]) <= 1e-6);
        }
        if (how == "scale") QCOMPARE(after.pixel(std::size_t(57) * 200 + 57)[3], 1.0);  // (grown: 70 → 55 px)
        same_book_after_reopening(changed);
    }

    void pasteOntoPrecisePixels_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QString("u16"); QTest::newRow("f32") << QString("f32");
    }
    // Pasted lines and pictures are drawn into the precise pixels (they are 8-bit, which u16 and f32 hold exactly);
    // what they do not cover keeps its samples.
    void pasteOntoPrecisePixels() {
        QFETCH(QString, precision);
        const auto doc = patterned_page(precision);
        const auto original = paint(doc).color_raster;
        const Json line{{"points", Json::array({Json::array({2.0, 5.0}), Json::array({23.0, 5.0})})}, {"width_mm", 1.0}, {"rgb", Json::array({200, 10, 30})}};
        const auto picture = render::Image::create("RGBA", render::Size{40, 40}, render::Ink{10, 220, 40, 255});
        const Json patch{{"box", Json::array({5.08, 12.7, 5.08, 5.08})}, {"png", core::b64encode(*render::fills::png_bytes(picture))}, {"mode", "image"}};
        const auto pasted = edit(doc, Json{{"op", "paste"}, {"page", 1}, {"layer_id", "paint"},
                                            {"items", Json{{"strokes", Json::array({line})}, {"patches", Json::array({patch})}}},
                                            {"matrix", Json::array({1.0, 0.0, 0.0, 1.0, 0.0, 1.27})}});
        still_precise(doc, original, pasted, precision);
        const auto after = pixels(pasted);
        const auto ink = after.pixel(std::size_t(49) * 200 + 100);  // (6.27 mm down: the middle of the line)
        QVERIFY(std::abs(ink[0] - 200 / 255.) < 1e-6 && std::abs(ink[1] - 10 / 255.) < 1e-6 && ink[3] == 1);
        const auto green = after.pixel(std::size_t(130) * 200 + 60);  // (the picture, 12.7 + 1.27 mm down)
        QVERIFY(std::abs(green[1] - 220 / 255.) < 1e-6 && green[3] == 1);
        for (std::uint32_t y = 160; y < 200; ++y)
            for (std::uint32_t x = 0; x < 200; ++x)
                if (sample_bytes(pasted, std::size_t(y) * 200 + x) != sample_bytes(doc, std::size_t(y) * 200 + x)) QFAIL("a pixel nothing covered changed");
        same_book_after_reopening(pasted);
    }

    void masksOnPrecisePixels_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QString("u16"); QTest::newRow("f32") << QString("f32");
    }
    // A mask on a precise layer: what it hides is not shown or merged, the samples themselves stay as they were.
    void masksOnPrecisePixels() {
        QFETCH(QString, precision);
        auto doc = patterned_page(precision);
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({0.0, 0.0}), Json::array({12.7, 0.0}), Json::array({12.7, 25.4}), Json::array({0.0, 25.4})})}};
        const auto masked = edit(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", area}});
        QCOMPARE(paint(masked).color_raster, original);  // (the pixels are not touched)
        QVERIFY(paint(masked).mask);
        const auto shown = render::render_page(masked.page(0), 200, render::proof_options(), &masked).image;
        const auto whole = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image;
        QCOMPARE(shown.getpixel(40, 40), whole.getpixel(40, 40));  // (left half: shown)
        QCOMPARE(shown.getpixel(160, 40), (std::vector<double>{255, 255, 255}));  // (right half: hidden, the paper shows)
        // a brush on the mask hides more
        const auto painted = edit(masked, Json{{"op", "paint_mask"}, {"page", 1}, {"id", "paint"}, {"show", false}, {"width_mm", 3.0},
                                                {"points", Json::array({Json::array({5.0, 2.0}), Json::array({5.0, 23.0})})}});
        QCOMPARE(render::render_page(painted.page(0), 200, render::proof_options(), &painted).image.getpixel(39, 100), (std::vector<double>{255, 255, 255}));
        same_book_after_reopening(painted);
        // merged down onto a precise layer below, the mask is in the merged pixels, and those stay precise
        auto two = painted;
        auto below = paint(doc); below.id = "below"; below.mask.reset();
        below.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(1, 1, precision.toStdString(), [](std::size_t) { return Pixel{0, 0, 1, 1}; }));
        two.edit_page(0).layers.insert(two.edit_page(0).layers.begin(), below);
        const auto merged = core::CommandBus(render::ops_registry()).apply(two, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", "paint"}}}), core::Actor("human:test")).doc;
        QCOMPARE(merged.page(0).layers.size(), std::size_t(1));
        const core::ColorRasterView flat(*merged.page(0).layers[0].color_raster);
        QCOMPARE(flat.metadata("")["precision"], Json(precision.toStdString()));
        QCOMPARE(flat.pixel(std::size_t(40) * 200 + 160), (Pixel{0, 0, 1, 1}));  // (hidden: the layer below)
        const auto kept = pixels(doc).pixel(std::size_t(40) * 200 + 60);
        for (unsigned c = 0; c < 4; ++c) QVERIFY(std::abs(flat.pixel(std::size_t(40) * 200 + 60)[c] - kept[c]) <= 1e-6);
        QCOMPARE(render::render_page(merged.page(0), 200, render::proof_options(), &merged).image.tobytes(),
                 render::render_page(two.page(0), 200, render::proof_options(), &two).image.tobytes());
        // a masked precise layer is not traced into lines (the mask would be lost): refused, the book unchanged
        try {
            core::CommandBus(render::ops_registry()).apply(painted, Json::array({Json{{"op", "convert_layer"}, {"page", 1}, {"id", "paint"}, {"to", "pen"}, {"preserve_precision", true}}}), core::Actor("human:test"));
            QFAIL("a masked precise layer was traced");
        } catch (const core::Error&) {}
        QCOMPARE(paint(painted).color_raster, original);
    }

    void drawingOnPrecisePixels_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("operation");
        for (const QString precision : {"u16", "f32"})
            for (const QString operation : {"add_stroke", "precise_stroke", "fill_area", "gradient_fill", "lock_alpha"})
                QTest::newRow(qPrintable(precision + "-" + operation)) << precision << operation;
    }
    // The pen, a fill and a gradient on a precise paint layer go into its pixels (a precise layer keeps no lines or
    // fills of its own): the book is taken, saved and read back; what they did not cover keeps its samples.
    void drawingOnPrecisePixels() {
        QFETCH(QString, precision); QFETCH(QString, operation);
        auto doc = page_with(precision.toStdString(), 200, 200, [](std::uint32_t x, std::uint32_t y) {
            return y < 100 ? Pixel{.25, .5, .75, 1} : Pixel{0, 0, 0, 0};  // the top half painted, the bottom clear
        });
        if (operation == "lock_alpha") doc.edit_page(0).layers[0].lock_alpha = true;
        const auto original = paint(doc).color_raster;
        const Json across = Json::array({Json::array({1.0, 12.7}), Json::array({24.4, 12.7})});
        const Json area{{"poly", Json::array({Json::array({0.0, 0.0}), Json::array({25.4, 0.0}), Json::array({25.4, 6.35}), Json::array({0.0, 6.35})})}};
        Json op{{"page", 1}, {"layer_id", "paint"}};
        if (operation == "add_stroke" || operation == "lock_alpha")
            op.update(Json{{"op", "add_stroke"}, {"points", across}, {"width_mm", 2.0}, {"rgb", Json::array({250, 20, 10})}, {"kind", "mili"}});
        else if (operation == "precise_stroke")
            op.update(Json{{"op", "paste"}, {"items", Json{{"strokes", Json::array({Json{{"points", across}, {"width_mm", 2.0}, {"kind", "mili"},
                {"color_rgb", Json{{"precision", "f32"}, {"values", Json::array({1.5, .125, .0625})}}}}})}}}});
        else if (operation == "fill_area") op.update(Json{{"op", "fill_area"}, {"area", area}, {"rgb", Json::array({10, 200, 30})}});
        else op.update(Json{{"op", "gradient_fill"}, {"area", area}, {"from", Json::array({0.0, 0.0})}, {"to", Json::array({25.4, 0.0})},
                            {"rgb_from", Json::array({255, 0, 0})}, {"rgb_to", Json::array({0, 0, 255})}, {"opacity_to", 1.0}});
        const auto drawn = edit(doc, op);
        still_precise(doc, original, drawn, precision);
        const auto after = pixels(drawn);
        const Pixel under_line = after.pixel(std::size_t(100) * 200 + 100), top = after.pixel(std::size_t(20) * 200 + 100);
        if (operation == "add_stroke") {
            QVERIFY(std::abs(under_line[0] - 250 / 255.) < 1e-6 && under_line[3] == 1);
        } else if (operation == "lock_alpha") {
            QCOMPARE(after.pixel(std::size_t(110) * 200 + 100)[3], 0.0);  // (the clear half stays clear: 透明保護)
            QVERIFY(after.pixel(std::size_t(99) * 200 + 30)[3] == 1);
            QVERIFY(std::abs(after.pixel(std::size_t(95) * 200 + 100)[0] - 250 / 255.) < 1e-6);
        } else if (operation == "precise_stroke") {
            if (precision == "f32") QVERIFY(std::abs(under_line[0] - 1.5) < 1e-6);  // (HDR: above 1, as it was given)
            else QCOMPARE(under_line[0], 1.0);
            QVERIFY(std::abs(under_line[1] - .125) < 1e-4);
        } else if (operation == "fill_area") {
            QVERIFY(std::abs(top[1] - 200 / 255.) < 1e-6 && top[3] == 1);
        } else {
            const Pixel left = after.pixel(std::size_t(20) * 200 + 2), right = after.pixel(std::size_t(20) * 200 + 197);
            QVERIFY(left[0] > .9 && left[2] < .1 && right[2] > .9 && right[0] < .1);
        }
        for (std::uint32_t y = 140; y < 200; ++y)  // (nothing drawn there)
            for (std::uint32_t x = 0; x < 200; ++x)
                if (sample_bytes(drawn, std::size_t(y) * 200 + x) != sample_bytes(doc, std::size_t(y) * 200 + x)) QFAIL("a pixel nothing covered changed");
        same_book_after_reopening(drawn);
    }

    void stretchedRastersTakeTheTools_data() {
        QTest::addColumn<QString>("operation");
        for (const char* op : {"move", "warp", "paste", "precise_pen", "pen"}) QTest::newRow(op) << QString(op);
    }
    // A raster of no whole dpi (300 × 150 over the page) takes the selection, the pen and precise lines too: drawn
    // at its resolution or finer and boxed down onto it; what they do not reach keeps its bytes.
    void stretchedRastersTakeTheTools() {
        QFETCH(QString, operation);
        const auto doc = page_with("f32", 300, 150, [](std::uint32_t x, std::uint32_t y) {
            return x >= 60 && x < 120 && y >= 30 && y < 60 ? Pixel{1.25, .5, .25, 1} : Pixel{0, 0, 0, 0};
        });
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({4.0, 4.0}), Json::array({11.0, 4.0}), Json::array({11.0, 11.0}), Json::array({4.0, 11.0})})}};
        const Json line = Json::array({Json::array({2.0, 20.0}), Json::array({23.0, 20.0})});
        Json op{{"page", 1}, {"layer_id", "paint"}};
        if (operation == "move") op.update(Json{{"op", "transform_area"}, {"area", area}, {"matrix", Json::array({1.0, 0.0, 0.0, 1.0, 8.0, 0.0})}});
        else if (operation == "warp") op.update(Json{{"op", "transform_area"}, {"area", area}, {"warp", Json{{"perspective", Json::array({Json::array({12.0, 4.0}), Json::array({19.0, 4.0}), Json::array({19.0, 11.0}), Json::array({12.0, 11.0})})}}}});
        else if (operation == "paste") op.update(Json{{"op", "paste"}, {"items", Json{{"strokes", Json::array({Json{{"points", line}, {"width_mm", 1.5}, {"kind", "mili"}, {"rgb", Json::array({0, 0, 255})}}})}}}});
        else if (operation == "precise_pen") op.update(Json{{"op", "paste"}, {"items", Json{{"strokes", Json::array({Json{{"points", line}, {"width_mm", 1.5}, {"kind", "mili"},
                                                            {"color_rgb", Json{{"precision", "f32"}, {"values", Json::array({.125, 2.0, .5})}}}}})}}}});
        else op.update(Json{{"op", "add_stroke"}, {"points", line}, {"width_mm", 1.5}, {"kind", "mili"}, {"rgb", Json::array({0, 0, 255})}});
        const auto changed = edit(doc, op);
        still_precise(doc, original, changed, "f32");
        const auto after = pixels(changed);
        const auto at = [&](double x_mm, double y_mm) { return after.pixel(std::size_t(y_mm / kInch * 150) * 300 + std::size_t(x_mm / kInch * 300)); };
        if (operation == "move" || operation == "warp") {
            QCOMPARE(at(7.5, 7.5)[3], 0.0);                                  // (left behind)
            QVERIFY(std::abs(at(15.5, 7.5)[0] - 1.25) < 1e-5 && at(15.5, 7.5)[3] == 1);  // (arrived, HDR as it was)
        } else {
            const Pixel ink = at(12.7, 20.0);
            QVERIFY2(ink[3] > .99, qPrintable(QString::number(ink[3])));
            if (operation == "precise_pen") QVERIFY2(std::abs(ink[1] - 2.0) < 1e-4, qPrintable(QString::number(ink[1])));
            else QVERIFY(std::abs(ink[2] - 1.0) < 1e-6 && ink[0] < 1e-6);
        }
        for (std::uint32_t y = 0; y < 20; ++y)  // (the top rows: nothing reached them)
            for (std::uint32_t x = 0; x < 300; ++x)
                if (sample_bytes(changed, std::size_t(y) * 300 + x) != sample_bytes(doc, std::size_t(y) * 300 + x)) QFAIL("an untouched pixel changed");
        same_book_after_reopening(changed);
    }

    void warpsAndScalesLandWhereTheyShould_data() {
        QTest::addColumn<QString>("how");
        for (const char* how : {"scale", "turn", "perspective", "mesh"}) QTest::newRow(how) << QString(how);
    }
    // Where a transformed piece lands: its alpha's centre of mass within a tenth of a pixel of the transform's own
    // (the square's points through the matrix or the warp), its area within one part in a hundred. (An 8-bit layer's
    // piece lands about half a pixel off this: Python moves a patch's pixels by their corners, not their centres.)
    void warpsAndScalesLandWhereTheyShould() {
        QFETCH(QString, how);
        const auto inside = [](double x, double y) { return x >= 64 && x < 118 && y >= 70 && y < 130; };
        const auto doc = page_with("u16", 200, 200, [&](std::uint32_t x, std::uint32_t y) { return inside(x, y) ? Pixel{.1, .2, .3, 1} : Pixel{0, 0, 0, 0}; });
        const Json area{{"poly", Json::array({Json::array({5.0, 5.0}), Json::array({20.0, 5.0}), Json::array({20.0, 20.0}), Json::array({5.0, 20.0})})}};
        Json op{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}, {"interp", "bilinear"}};
        std::array<double, 6> m{1, 0, 0, 1, 0, 0};
        if (how == "scale") m = {1.3, 0.0, 0.0, 0.8, -2.5, 1.9};
        else if (how == "turn") m = {0.9659258, 0.2588190, -0.2588190, 0.9659258, 3.7, -2.9};  // (15°)
        if (how == "scale" || how == "turn") op["matrix"] = Json::array({m[0], m[1], m[2], m[3], m[4], m[5]});
        else if (how == "perspective")
            op["warp"] = Json{{"perspective", Json::array({Json::array({4.0, 6.0}), Json::array({21.0, 5.0}), Json::array({20.0, 20.0}), Json::array({5.0, 21.0})})}};
        else op["warp"] = Json{{"mesh", Json::array({Json::array({5.0, 5.0}), Json::array({12.5, 4.0}), Json::array({20.0, 5.0}),
                                                    Json::array({4.0, 12.5}), Json::array({13.0, 12.0}), Json::array({21.0, 12.5}),
                                                    Json::array({5.0, 20.0}), Json::array({12.5, 21.0}), Json::array({20.0, 20.0})})}};
        const render::warp::Go go = op.contains("warp") ? render::warp::mapping({5.0, 5.0, 15.0, 15.0}, op["warp"])
                                                        : render::warp::Go([&](double x, double y) { return std::pair<double, double>{m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]}; });
        // the transform's own: the square sampled finely, each point carried by it (its area by the cells' images)
        constexpr double px = kInch / 200, step = 0.125;
        double weight = 0, cx = 0, cy = 0;
        for (double y = 70; y < 130; y += step) {
            for (double x = 64; x < 118; x += step) {
                const auto a = go((x + step / 2) * px, (y + step / 2) * px);
                const auto b = go((x + step) * px, y * px), c = go(x * px, (y + step) * px), o = go(x * px, y * px);
                const double cell = std::abs((b.first - o.first) * (c.second - o.second) - (b.second - o.second) * (c.first - o.first)) / (px * px);
                weight += cell; cx += cell * a.first / px; cy += cell * a.second / px;
            }
        }
        cx /= weight; cy /= weight;
        const auto changed = edit(doc, op);
        const auto after = pixels(changed);
        double mine = 0, mx = 0, my = 0;
        for (std::uint32_t y = 0; y < 200; ++y)
            for (std::uint32_t x = 0; x < 200; ++x) {
                const double a = after.pixel(std::size_t(y) * 200 + x)[3];
                mine += a; mx += a * (x + .5); my += a * (y + .5);
            }
        mx /= mine; my /= mine;
        QVERIFY2(std::abs(mx - cx) < 0.1 && std::abs(my - cy) < 0.1, qPrintable(QString("centre %1,%2 against %3,%4").arg(mx).arg(my).arg(cx).arg(cy)));
        QVERIFY2(std::abs(mine / weight - 1) < 0.01, qPrintable(QString("area %1 against %2").arg(mine).arg(weight)));
    }

    // Tiles: saved again after a small change, a raster adds the one tile the change touched, and reads back whole.
    void aSmallChangeAddsOneTile() {
        const auto doc = patterned_page("f32", 600);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto path = std::filesystem::path(temporary.path().toStdString()) / "book";
        storage::AssetStore store(path);
        const auto tiles = [&] {
            std::size_t n = 0;
            for (const auto& file : store.all_files()) n += file.extension() == ".colorrgba";
            return n;
        };
        (void)storage::project_json_v4(doc, store);
        QCOMPARE(tiles(), std::size_t(9));  // (600 px: three tiles of 256 each way, the last ones smaller)
        const auto erased = edit(doc, Json{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 1.0},
                                           {"points", Json::array({Json::array({2.0, 2.0}), Json::array({4.0, 4.0})})}});
        const auto text = storage::project_json_v4(erased, store);
        QCOMPARE(tiles(), std::size_t(10));
        const auto loaded = storage::load_document_text(text, path);
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        QCOMPARE(*paint(loaded.document).color_raster, *paint(erased).color_raster);
        const auto features = core::parse_python_json(text)["features"];
        QVERIFY(std::find(features.begin(), features.end(), Json(std::string(core::kColorTilesFeature))) != features.end());
    }

    // An op that changes nothing keeps the very picture (no new copy, nothing new to save).
    void nothingChangedKeepsThePicture() {
        const auto doc = patterned_page("u16", 40);
        const auto original = paint(doc).color_raster;
        const Json far{{"poly", Json::array({Json::array({30.0, 30.0}), Json::array({40.0, 30.0}), Json::array({40.0, 40.0})})}};
        for (const Json& op : {Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", far}},
                               Json{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"points", Json::array({Json::array({40.0, 40.0}), Json::array({50.0, 50.0})})}},
                               Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", far}, {"matrix", Json::array({1, 0, 0, 1, 2, 0})}}})
            QCOMPARE(paint(edit(doc, op)).color_raster, original);
    }

    // A locked layer is not erased, nor its area deleted or moved.
    void lockedPrecisePixelsAreRefused() {
        auto doc = patterned_page("u16", 20);
        doc.edit_page(0).layers[0].locked = true;
        const auto original = paint(doc).color_raster;
        const Json area{{"poly", Json::array({Json::array({0.0, 0.0}), Json::array({10.0, 0.0}), Json::array({10.0, 10.0})})}};
        for (const Json& op : {Json{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"points", Json::array({Json::array({1.0, 1.0}), Json::array({9.0, 9.0})})}},
                               Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}},
                               Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}, {"matrix", Json::array({1, 0, 0, 1, 1, 0})}}}) {
            QVERIFY_EXCEPTION_THROWN(edit(doc, op), core::Error);
            QCOMPARE(paint(doc).color_raster, original);
        }
    }
};

QTEST_GUILESS_MAIN(TestColorRasterEdit)
#include "test_color_raster_edit.moc"
