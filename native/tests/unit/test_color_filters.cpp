#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
using namespace genko;
using core::Json;

// Filters on high-precision colour pixels: on a picture of 8-bit values they give what the 8-bit filter gives (within
// its rounding), and they keep what 8 bits cannot hold — the steps between, and HDR.
namespace {
using Pixel = std::array<double, 4>;
constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
constexpr std::uint32_t kSide = 120;

core::Document blank() {
    auto doc = core::new_episode("高精度フィルター", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0); p.numero = false; p.frames.clear(); p.layers.clear();
    return doc;
}
// A picture of 8-bit values: shapes and gradients in colour, a half-clear band, a few dark specks.
std::array<int, 4> eight_bit(std::uint32_t x, std::uint32_t y, bool clear_band = true) {
    std::array<int, 4> v{static_cast<int>((x * 2 + y) % 256), static_cast<int>((y * 3 + 40) % 256), static_cast<int>((x * y) % 256), 255};
    if (x > 30 && x < 60 && y > 30 && y < 60) v = {240, 235, 230, 255};      // a light square (glow, threshold)
    if ((x == 80 && y == 80) || (x == 81 && y == 80)) v = {10, 10, 10, 255};  // a speck
    if (clear_band && y >= 100 && y < 110) v[3] = 128;                       // half clear
    return v;
}
// The raster is 200 dpi × 25.4 mm only when kSide is 200; this page is 120 px across at about 120 dpi: scaled sizes
core::Document precise(const QString& precision, std::uint32_t side, const std::function<Pixel(std::uint32_t, std::uint32_t)>& at) {
    auto doc = blank();
    Json pixels = Json::array();
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x)
            for (const double v : at(x, y)) {
                if (precision == "u16") pixels.push_back(static_cast<std::int64_t>(std::lround(v * 65535)));
                else pixels.push_back(static_cast<double>(static_cast<float>(v)));
            }
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", side}, {"height", side},
                                                           {"precision", precision.toStdString()}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}
core::Document eight_bit_page(std::uint32_t side, bool clear_band = true) {
    auto doc = blank();
    core::Layer layer; layer.id = "paint"; layer.role = core::LayerRole::User; layer.kind = core::LayerKind::Raster; layer.panel_clip = false;
    std::string bytes;
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x)
            for (const int v : eight_bit(x, y, clear_band)) bytes.push_back(static_cast<char>(v));
    render::raster::save_raster(doc.page(0), layer, render::Image::frombytes("RGBA", render::Size{int(side), int(side)}, bytes));
    doc.edit_page(0).layers.push_back(layer);
    return doc;
}
core::Document edit(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc;
}
const core::Layer& paint(const core::Document& doc) { return doc.page(0).layers[0]; }
core::ColorRasterView view(const core::Document& doc) { return core::ColorRasterView(*paint(doc).color_raster); }
void same_book_after_reopening(const core::Document& doc) {
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdString()) / "book";
    storage::AssetStore store(path);
    const auto loaded = storage::load_document_text(storage::project_json_v4(doc, store), path);
    QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
    QCOMPARE(*paint(loaded.document).color_raster, *paint(doc).color_raster);
}
// Each kind with settings that make it do something here (sizes in 8-bit pixels: the raster is 200 dpi).
const std::vector<std::pair<QString, Json>>& filters() {
    static const std::vector<std::pair<QString, Json>> all{
        {"blur", Json{{"radius", 3}}},
        {"sharpen", Json::object()},
        {"sharpen-amount", Json{{"amount", 1.5}, {"radius", 2}}},
        {"hue", Json{{"shift", 40}, {"saturation", 0.8}, {"value", 0.9}}},
        {"levels", Json{{"black", 20}, {"white", 220}}},
        {"levels-table", Json{{"black", 10}, {"white", 240}, {"gamma", 1.4}, {"out_black", 5}, {"out_white", 250}}},
        {"curve", Json{{"gamma", 1.8}}},
        {"curve-points", Json{{"points", Json::array({Json::array({0, 0}), Json::array({64, 90}), Json::array({192, 170}), Json::array({255, 255})})}}},
        {"mosaic", Json{{"block", 9}}},
        {"bitonal", Json{{"threshold", 150}}},
        {"motion_blur", Json{{"distance", 9}, {"angle", 30}}},
        {"radial_blur", Json{{"amount", 0.1}}},
        {"zoom_blur", Json{{"amount", 0.1}}},
        {"noise", Json{{"amount", 0.3}, {"seed", "s"}}},
        {"noise-colour", Json{{"amount", 0.3}, {"mono", false}}},
        {"wave", Json{{"amplitude", 4}, {"wavelength", 30}}},
        {"twirl", Json{{"angle", 70}}},
        {"lineart", Json::object()},
        {"invert", Json::object()},
        {"posterize", Json{{"levels", 5}}},
        {"threshold", Json{{"threshold", 120}}},
        {"gradient_map", Json{{"colors", Json::array({Json::array({20, 0, 80}), Json::array({250, 200, 40}), Json::array({255, 255, 255})})}}},
        {"gradient_map-stops", Json{{"stops", Json::array({Json::array({0.0, Json::array({0, 0, 0})}), Json::array({0.3, Json::array({200, 20, 20})}), Json::array({1.0, Json::array({255, 255, 200})})})}}},
        {"brightness_contrast", Json{{"brightness", 20}, {"contrast", 30}, {"channel", "g"}}},
        {"despeckle", Json{{"size_px", 3}}},
        {"glow", Json{{"radius", 6}, {"threshold", 200}}},
        {"rain", Json{{"count", 60}, {"length", 30}, {"seed", "r"}}},
    };
    return all;
}
}  // namespace

class TestColorFilters : public QObject {
    Q_OBJECT
private slots:
    void likeTheEightBitFilter_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("name");
        for (const QString precision : {"u16", "f32"})
            for (const auto& [name, params] : filters()) QTest::newRow(qPrintable(precision + "-" + name)) << precision << name;
    }
    // On 8-bit values, within the 8-bit filter's own rounding (a level, a few where it rounds at each step).
    void likeTheEightBitFilter() {
        QFETCH(QString, precision); QFETCH(QString, name);
        Json params;
        for (const auto& [n, p] : filters()) if (n == name) params = p;
        const std::string kind = name.split('-').front().toStdString();
        Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind}};
        op.update(params);
        constexpr std::uint32_t side = 200;
        const auto doc = precise(precision, side, [](std::uint32_t x, std::uint32_t y) {
            const auto v = eight_bit(x, y);
            return Pixel{v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, v[3] / 255.0};
        });
        const auto original = paint(doc).color_raster;
        const auto filtered = edit(doc, op);
        QCOMPARE(paint(doc).color_raster, original);  // (the book it was applied to is as it was)
        QVERIFY(paint(filtered).color_raster); QVERIFY(!paint(filtered).raster_png);
        QCOMPARE(view(filtered).metadata("")["precision"], Json(precision.toStdString()));
        const auto expected_doc = edit(eight_bit_page(side), op);
        const std::string expected = render::selection::open_picture(*paint(expected_doc).raster_png).convert("RGBA").tobytes();
        const auto after = view(filtered);
        // the steps the 8-bit filter rounds at: one level for a table or a remap, more for its passes of blur
        // (the 8-bit hue goes through HSV in whole levels: its hue and saturation cut down to 1/255 of their range)
        // (its motion, radial and zoom blurs average by Image.blend, which cuts each step down to a whole level)
        const double tolerance = name == "hue" ? 8 : name == "blur" || name == "glow" || name.startsWith("gradient_map") ? 2
                                 : name.startsWith("sharpen") ? 6 : name == "motion_blur" ? 4 : name.endsWith("_blur") ? 7 : 1;
        std::size_t off = 0, worst_at = 0; double worst = 0;
        for (std::size_t i = 0; i < std::size_t(side) * side; ++i) {
            const Pixel got = after.pixel(i);
            const auto* want = reinterpret_cast<const unsigned char*>(expected.data() + i * 4);
            for (unsigned c = 0; c < 4; ++c) {
                const double diff = std::abs(got[c] * 255 - want[c]);
                if (diff > worst) { worst = diff; worst_at = i; }
                if (diff > tolerance) ++off;
            }
        }
        // a threshold may fall the other side for a value the 8-bit filter rounded: a few pixels, never a region
        QVERIFY2(off <= std::size_t(side) * side / 500,
                 qPrintable(QString("%1 samples off by more than %2 (worst %3 at pixel %4)").arg(off).arg(tolerance).arg(worst).arg(worst_at)));
        same_book_after_reopening(filtered);
    }

    void stepsBetweenTheEightBitOnesStay_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"invert", "levels", "curve", "brightness_contrast", "hue", "blur"}) QTest::newRow(kind) << QString(kind);
    }
    // A u16 ramp of 4096 steps: after a filter that keeps the order of greys, many more than 256 of them remain.
    void stepsBetweenTheEightBitOnesStay() {
        QFETCH(QString, kind);
        const auto doc = precise("u16", 64, [](std::uint32_t x, std::uint32_t y) {
            const double v = (y * 64 + x) / 4095.0 * 0.5 + 0.25;
            return Pixel{v, v * 0.8, v * 0.6, 1};
        });
        Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind.toStdString()}};
        if (kind == "levels") op.update(Json{{"black", 30}, {"white", 230}});
        if (kind == "blur") op["radius"] = 0.6;
        const auto filtered = edit(doc, op);
        const auto after = view(filtered);
        std::set<std::uint32_t> reds;
        for (std::size_t i = 0; i < 64 * 64; ++i) reds.insert(static_cast<std::uint32_t>(std::lround(after.pixel(i)[0] * 65535)));
        QVERIFY2(reds.size() > 1000, qPrintable(QString("%1 distinct reds").arg(reds.size())));
        if (kind == "invert") {
            const auto before = view(doc);
            for (std::size_t i = 0; i < 64 * 64; ++i)
                QVERIFY2(std::abs(after.pixel(i)[0] - (1 - before.pixel(i)[0])) < 1e-12, qPrintable(QString("%1: %2 → %3").arg(i).arg(before.pixel(i)[0]).arg(after.pixel(i)[0])));
        }
    }

    void hdrStays_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<double>("expected");
        QTest::newRow("blur") << QString("blur") << 1.5;
        QTest::newRow("invert") << QString("invert") << -0.5;
        QTest::newRow("curve") << QString("curve") << std::pow(1.5, 1.6);
        QTest::newRow("motion_blur") << QString("motion_blur") << 1.5;
        QTest::newRow("mosaic") << QString("mosaic") << 1.5 * 0.88;
        QTest::newRow("sharpen") << QString("sharpen") << 1.5 + 12 / 255.0;  // (then contrast and unsharp: checked as above 1)
    }
    // f32 samples above 1 come out of a filter as HDR, not clipped to 1 as 8 bits would.
    void hdrStays() {
        QFETCH(QString, kind); QFETCH(double, expected);
        const auto doc = precise("f32", 40, [](std::uint32_t, std::uint32_t) { return Pixel{1.5, 0.25, 0.5, 1}; });
        const auto filtered = edit(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind.toStdString()}});
        const auto after = view(filtered);
        const double red = after.pixel(20 * 40 + 20)[0];
        if (kind == "sharpen") QVERIFY2(red > 1.2, qPrintable(QString::number(red)));
        else QVERIFY2(std::abs(red - expected) < 1e-5, qPrintable(QString("%1, expected %2").arg(red).arg(expected)));
    }

    // Inside a selection only: what is outside keeps its bytes; the raster's other resolution scales the sizes.
    void onlyInsideTheArea() {
        const auto doc = precise("f32", kSide, [](std::uint32_t x, std::uint32_t y) { return Pixel{x / 119.0, y / 119.0, 0.5, 1}; });
        const Json area{{"poly", Json::array({Json::array({0.0, 0.0}), Json::array({12.7, 0.0}), Json::array({12.7, 25.4}), Json::array({0.0, 25.4})})}};
        const auto after = edit(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "invert"}, {"area", area}});
        const auto& before_bytes = *paint(doc).color_raster;
        const auto& after_bytes = *paint(after).color_raster;
        for (std::uint32_t y = 0; y < kSide; ++y) {
            QVERIFY(std::abs(view(after).pixel(y * kSide + 10)[0] - (1 - view(doc).pixel(y * kSide + 10)[0])) < 1e-6);  // inside
            for (std::uint32_t x = 70; x < kSide; ++x) {  // outside
                const std::size_t at = 16 + (std::size_t(y) * kSide + x) * 16;
                if (before_bytes.compare(at, 16, after_bytes, at, 16) != 0) QFAIL("a pixel outside the area changed");
            }
        }
    }

    void correctionLayersLikeEightBit_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("name"); QTest::addColumn<double>("opacity"); QTest::addColumn<bool>("masked");
        const std::vector<QString> kinds{"levels", "levels-table", "curve", "curve-points", "hue", "invert", "posterize", "threshold",
                                         "gradient_map", "gradient_map-stops", "bitonal", "brightness_contrast"};
        for (const QString precision : {"u16", "f32"}) {
            for (const QString& name : kinds) QTest::newRow(qPrintable(precision + "-" + name)) << precision << name << 1.0 << false;
            QTest::newRow(qPrintable(precision + "-hue-faded-masked")) << precision << QString("hue") << 0.6 << true;
        }
    }
    // A correction layer over precise pixels shows what it shows over their 8-bit picture (within its tables'
    // rounding); merged down, the page looks the same and the pixels stay precise.
    void correctionLayersLikeEightBit() {
        QFETCH(QString, precision); QFETCH(QString, name); QFETCH(double, opacity); QFETCH(bool, masked);
        Json spec;
        for (const auto& [n, p] : filters()) if (n == name) spec = p;
        spec["kind"] = name.split('-').front().toStdString();
        const Json add{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", spec}, {"opacity", opacity}};
        constexpr std::uint32_t side = 200;
        const auto with = [&](core::Document doc) {
            doc = edit(doc, add);
            if (masked) doc = edit(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", doc.page(0).layers.back().id},
                                             {"area", Json{{"poly", Json::array({Json::array({0.0, 0.0}), Json::array({15.0, 0.0}), Json::array({8.0, 25.4})})}}}});
            return doc;
        };
        // (opaque: a precise page composites a half-clear layer in linear light, an 8-bit one in sRGB samples)
        const auto doc = with(precise(precision, side, [](std::uint32_t x, std::uint32_t y) {
            const auto v = eight_bit(x, y, false);
            return Pixel{v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, v[3] / 255.0};
        }));
        const auto eight = with(eight_bit_page(side, false));
        const auto shown = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.tobytes();
        const auto expected = render::render_page(eight.page(0), 200, render::proof_options(), &eight).image.tobytes();
        QCOMPARE(shown.size(), expected.size());
        const int limit = name == "hue" ? 8 : 2;  // (the 8-bit hue's HSV in whole levels, as for the filter)
        std::size_t off = 0; int worst = 0;
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const int diff = std::abs(int(static_cast<unsigned char>(shown[i])) - int(static_cast<unsigned char>(expected[i])));
            worst = std::max(worst, diff);
            if (diff > limit) ++off;
        }
        QVERIFY2(off <= shown.size() / 500, qPrintable(QString("%1 samples off by more than %2 (worst %3)").arg(off).arg(limit).arg(worst)));
        // merged into the pixels: still precise, the page as it was shown
        const auto merged = edit(doc, Json{{"op", "merge_down"}, {"page", 1}, {"id", doc.page(0).layers.back().id}});
        QCOMPARE(merged.page(0).layers.size(), std::size_t(1));
        QVERIFY(merged.page(0).layers[0].color_raster);
        QCOMPARE(core::ColorRasterView(*merged.page(0).layers[0].color_raster).metadata("")["precision"], Json(precision.toStdString()));
        const auto again = render::render_page(merged.page(0), 200, render::proof_options(), &merged).image.tobytes();
        std::size_t moved = 0;
        for (std::size_t i = 0; i < again.size(); ++i)
            if (std::abs(int(static_cast<unsigned char>(again[i])) - int(static_cast<unsigned char>(shown[i]))) > 1) ++moved;
        QCOMPARE(moved, std::size_t(0));
        same_book_after_reopening(doc);
    }

    // A correction merged into a u16 ramp keeps the ramp's steps between the 8-bit ones.
    void mergedCorrectionKeepsSteps() {
        const auto doc = edit(precise("u16", 64, [](std::uint32_t x, std::uint32_t y) {
            const double v = (y * 64 + x) / 4095.0 * 0.5 + 0.25;
            return Pixel{v, v, v, 1};
        }), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", "levels"}, {"black", 40}, {"white", 220}, {"gamma", 1.2}}}});
        core::Document merged;
        try {
            merged = edit(doc, Json{{"op", "merge_down"}, {"page", 1}, {"id", doc.page(0).layers.back().id}});
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        const core::ColorRasterView after(*merged.page(0).layers[0].color_raster);
        std::set<std::uint32_t> greys;
        for (std::size_t i = 0; i < 64 * 64; ++i) greys.insert(static_cast<std::uint32_t>(std::lround(after.pixel(i)[0] * 65535)));
        QVERIFY2(greys.size() > 1000, qPrintable(QString("%1 distinct greys").arg(greys.size())));
    }

    // A plugin runs in the external runner, not here; precise pen lines are converted to paint first: both refused,
    // the book as it was.
    void refusedAsBefore() {
        const auto doc = precise("u16", 4, [](std::uint32_t, std::uint32_t) { return Pixel{.5, .5, .5, 1}; });
        const auto original = paint(doc).color_raster;
        for (const Json& op : {Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "plugin:nothing"}},
                               Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "nothing"}},
                               Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "despeckle"}, {"what", "dust"}},
                               Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "curve"}, {"points", Json::array({Json::array({1, 2})})}}}) {
            QVERIFY_EXCEPTION_THROWN(edit(doc, op), core::Error);
            QCOMPARE(paint(doc).color_raster, original);
        }
    }
};

QTEST_GUILESS_MAIN(TestColorFilters)
#include "test_color_filters.moc"
