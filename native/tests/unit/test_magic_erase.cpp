// PC3 B1 (SPEC.md COLOR-06「マジック消しゴムツール」): the op magic_erase as PhotoCraft's eraser_cmds/tests.rs and
// algo/src/erase.rs test it — the wand's region made clear (contiguous or anywhere, its tolerance, half at half opacity,
// inside the selection only), the page sampled as it shows or the layer alone, a layer whose transparency is protected
// painted with the background colour instead, the colours kept on 8-bit, 16-bit and CMYK layers, a click off the page
// leaving the book as it was, and the refusals.

#include <QtTest>

#include <array>
#include <cmath>
#include <functional>
#include <string>

#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

constexpr double kInch = 25.4;  // (200 px at the working 200 dpi)
constexpr int kSide = 200;

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

double mm(int px) { return (px + 0.5) * kInch / kSide; }

using Colour = std::array<int, 4>;

// White, a black square at 40..80 (and, with `second`, another at 120..160 apart from it), a near-white band below.
Colour squares(int x, int y, bool second) {
    if (x >= 40 && x < 80 && y >= 40 && y < 80) return {0, 0, 0, 255};
    if (second && x >= 120 && x < 160 && y >= 40 && y < 80) return {0, 0, 0, 255};
    if (y >= 180) return {230, 230, 230, 255};
    return {255, 255, 255, 255};
}

render::Image picture(const std::function<Colour(int, int)>& at) {
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            for (const int v : at(x, y)) bytes.push_back(static_cast<char>(v));
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Document book(bool second = false) {
    auto doc = core::new_episode("消す", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, picture([second](int x, int y) { return squares(x, y, second); }));
    p.layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + id);
}

// The layer's pixel (x, y) as straight RGBA 0..1 (any kind: its colours as RGB).
render::image_ops::Rgba pixel(const core::Document& doc, const std::string& id, int x, int y) {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, id));
    const auto rgba = render::image_ops::rgba_of(p);
    const int px = static_cast<int>(std::floor((x + 0.5) * p.width() / kSide)), py = static_cast<int>(std::floor((y + 0.5) * p.height() / kSide));
    return rgba[static_cast<std::size_t>(py) * p.width() + static_cast<std::size_t>(px)];
}

float alpha(const core::Document& doc, int x, int y, const std::string& id = "paint") { return pixel(doc, id, x, y)[3]; }

Json erase_at(int x, int y) { return Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", mm(x)}, {"y_mm", mm(y)}, {"antialias", false}}; }

}  // namespace

class TestMagicErase : public QObject {
    Q_OBJECT
private slots:
    // The white erased (joined to the click), the square kept with its colour; anywhere; half at half opacity.
    void erasesTheRegion() {
        const core::Document doc = book(true);
        const core::ApplyResult done = run_ops(doc, Json::array({erase_at(2, 2)}));
        QCOMPARE(done.results.at(0)["erased"], Json(true));
        QCOMPARE(alpha(done.doc, 2, 2), 0.0f);
        QCOMPARE(alpha(done.doc, 190, 20), 0.0f);  // (joined white)
        QCOMPARE(alpha(done.doc, 60, 60), 1.0f);   // (the square kept)
        QCOMPARE(pixel(done.doc, "paint", 60, 60)[0], 0.0f);
        QCOMPARE(alpha(done.doc, 10, 190), 0.0f);  // (the near-white band: 25 levels away, within the 32 of the default)
        // contiguous: the other square stays; anywhere: it goes too
        core::Document one = run(doc, erase_at(60, 60));
        QCOMPARE(alpha(one, 60, 60), 0.0f);
        QCOMPARE(alpha(one, 140, 60), 1.0f);
        Json anywhere = erase_at(60, 60);
        anywhere["contiguous"] = false;
        one = run(doc, anywhere);
        QCOMPARE(alpha(one, 140, 60), 0.0f);
        QCOMPARE(alpha(one, 2, 2), 1.0f);
        // half opacity: half erased, the colour kept
        Json half = erase_at(2, 2);
        half["opacity"] = 0.5;
        one = run(doc, half);
        QVERIFY(std::abs(alpha(one, 2, 2) - 0.5f) < 0.01f);
        QCOMPARE(pixel(one, "paint", 2, 2)[0], 1.0f);
        // the tolerance: the near-white band (25 levels away) only from 25 up
        Json wide = erase_at(2, 2);
        wide["tolerance"] = 25;
        QCOMPARE(alpha(run(doc, wide), 10, 190), 0.0f);
        Json narrow = erase_at(2, 2);
        narrow["tolerance"] = 24;
        QCOMPARE(alpha(run(doc, narrow), 10, 190), 1.0f);
        Json none = erase_at(2, 2);
        none["tolerance"] = 0;
        QCOMPARE(alpha(run(doc, none), 10, 190), 1.0f);
        // anti-aliased: the edge of the square partly erased
        Json soft = erase_at(2, 2);
        soft["antialias"] = true;
        one = run(doc, soft);
        QCOMPARE(alpha(one, 2, 2), 0.0f);
        QVERIFY(alpha(one, 39, 60) > 0.0f && alpha(one, 39, 60) < 1.0f);
    }

    // Inside the selection only.
    void theSelectionLimitsIt() {
        const core::Document doc = book();
        Json op = erase_at(2, 2);
        op["area"] = Json{{"rect", Json::array({0, 0, kInch / 2, kInch})}};
        const core::Document done = run(doc, op);
        QCOMPARE(alpha(done, 2, 2), 0.0f);
        QCOMPARE(alpha(done, 150, 2), 1.0f);
    }

    // Sample All Layers: the page as it shows splits the white by a band above it; the layer alone does not.
    void thePageOrTheLayer() {
        core::Document doc = book();
        core::Layer band;
        band.id = "band";
        band.role = core::LayerRole::User;
        band.kind = core::LayerKind::Raster;
        band.panel_clip = false;
        render::raster::save_raster(doc.page(0), band, picture([](int, int y) { return y >= 100 && y < 120 ? Colour{255, 0, 0, 255} : Colour{0, 0, 0, 0}; }));
        doc.edit_page(0).layers.push_back(std::move(band));
        Json op = erase_at(2, 2);
        op["sample"] = "page";
        core::Document done = run(doc, op);
        QCOMPARE(alpha(done, 2, 2), 0.0f);
        QCOMPARE(alpha(done, 2, 110), 1.0f);  // (under the band: another colour on the page)
        QCOMPARE(alpha(done, 2, 150), 1.0f);  // (below it: not joined)
        done = run(doc, erase_at(2, 2));
        QCOMPARE(alpha(done, 2, 110), 0.0f);
        QCOMPARE(alpha(done, 2, 150), 0.0f);
    }

    // A layer whose transparency is protected: painted with the background colour, its alpha kept.
    void protectedTransparency() {
        core::Document doc = book();
        doc.edit_page(0).layers.back().lock_alpha = true;
        Json op = erase_at(2, 2);
        op["background"] = Json::array({0, 255, 0});
        const core::Document done = run(doc, op);
        const auto p = pixel(done, "paint", 2, 2);
        QCOMPARE(p, (render::image_ops::Rgba{0.0f, 1.0f, 0.0f, 1.0f}));
        QCOMPARE(pixel(done, "paint", 60, 60), (render::image_ops::Rgba{0.0f, 0.0f, 0.0f, 1.0f}));
    }

    // A 16-bit layer and a CMYK layer: the alpha erased in their own values, the square's colour untouched.
    void preciseAndInkLayers() {
        const core::Document deep = run(book(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        QVERIFY(layer_of(deep, "paint").color_raster);
        const render::image_ops::Rgba square = pixel(deep, "paint", 60, 60);
        core::Document done = run(deep, erase_at(2, 2));
        QVERIFY(layer_of(done, "paint").color_raster);
        QCOMPARE(alpha(done, 2, 2), 0.0f);
        QCOMPARE(pixel(done, "paint", 60, 60), square);
        const core::Document cmyk = run(book(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
        QVERIFY(layer_of(cmyk, "paint").ink_raster);
        const core::InkPixels before = core::decode_ink_raster(*layer_of(cmyk, "paint").ink_raster);
        done = run(cmyk, erase_at(2, 2));
        const core::InkPixels after = core::decode_ink_raster(*layer_of(done, "paint").ink_raster);
        const std::size_t white = std::size_t(2) * after.width + 2, black = std::size_t(60) * after.width + 60;
        QCOMPARE(after.value(white, 4), 0.0);
        QCOMPARE(after.value(black, 4), 1.0);
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(after.value(black, c), before.value(black, c));
    }

    // Off the page: nothing erased, the book as it was; the refusals.
    void offThePageAndRefusals() {
        const core::Document doc = book();
        Json off = erase_at(2, 2);
        off["x_mm"] = -5;
        const core::ApplyResult none = run_ops(doc, Json::array({off}));
        QCOMPARE(none.results.at(0)["erased"], Json(false));
        QCOMPARE(*layer_of(none.doc, "paint").raster_png, *layer_of(doc, "paint").raster_png);
        Json clamped = erase_at(2, 2);
        clamped["tolerance"] = 9999;
        clamped["opacity"] = -3;  // (out of range: as near as it goes — nothing erased)
        QCOMPARE(run_ops(doc, Json::array({clamped})).results.at(0)["erased"], Json(false));
        for (const Json& bad : {Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}}, Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 1}},
                                Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", "a"}, {"y_mm", 1}},
                                Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 1}, {"y_mm", 1}, {"tolerance", "high"}},
                                Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "paint"}, {"x_mm", 1}, {"y_mm", 1}, {"sample", "all"}},
                                Json{{"op", "magic_erase"}, {"page", 1}, {"x_mm", 1}, {"y_mm", 1}},
                                Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "nothing"}, {"x_mm", 1}, {"y_mm", 1}}}) {
            QVERIFY2(refusal(doc, bad).starts_with("apply: "), bad.dump().c_str());
        }
        core::Document locked = doc;
        locked.edit_page(0).layers.back().locked = true;
        QVERIFY(refusal(locked, erase_at(2, 2)).find("locked") != std::string::npos);
        core::Document folder = doc;
        folder.edit_page(0).layers.back().kind = core::LayerKind::Folder;
        QVERIFY(refusal(folder, erase_at(2, 2)).find("not a paint layer") != std::string::npos);
    }
};

QTEST_MAIN(TestMagicErase)
#include "test_magic_erase.moc"
