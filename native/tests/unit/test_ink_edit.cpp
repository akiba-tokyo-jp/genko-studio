// color-mode@1, the second step (SPEC.md COLOR-02): the pixel ops of paint layers on CMYK and Lab pages
// (render/ink_edit). What an op does not change keeps its inks exactly (16 bits as they were), what it moves keeps
// them, a line merged into the pixels takes the page's rules (Genko's black: K alone), every op of the step leaves the
// page in inks; the filters still wait (not_yet_ported); the eyedropper's inks at a point; a colour for chosen inks
// (Genko's blacks and white, and a colour the table holds for other inks, passed over); the gamut mark.

#include <QtTest>

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "render/ink.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

std::string icc_bytes() { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))); }

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const auto& layer : doc.page(0).layers) {
        if (layer.id == id) return layer;
    }
    throw core::Error("value", "no layer " + id);
}

core::InkPixels pixels_of(const core::Document& doc, const std::string& id) { return core::decode_ink_raster(*layer_of(doc, id).ink_raster); }

const std::uint16_t* at(const core::InkPixels& p, std::uint32_t x, std::uint32_t y) { return p.samples.data() + (std::size_t(y) * p.width + x) * p.channels(); }

// The two colours of the picture (CMYK, 16 bits): S at the top right, T at the bottom left.
constexpr std::array<std::uint16_t, 4> kS{13107, 45875, 6554, 3277};
constexpr std::array<std::uint16_t, 4> kT{52428, 3277, 32768, 0};

// A raster pixel of a point (mm): paint layers are kept at the working 200 dpi.
std::uint32_t px(double mm) { return static_cast<std::uint32_t>(mm * 200 / 25.4); }

// A CMYK page (16 bits; 60 × 80 mm) with a paint layer "pic" at 200 dpi (472 × 630): its top left random inks, its top
// right S, its bottom left T, its bottom right clear (the halves at 30 and 40 mm).
core::Document ink_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("ink", core::Num(1), 1, spec, core::Binding::Right);
    doc = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", 16}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}}}))
              .doc;
    const std::uint32_t w = static_cast<std::uint32_t>(render::mm_to_px(60, 200)), h = static_cast<std::uint32_t>(render::mm_to_px(80, 200));
    core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Cmyk, 16, w, h);
    std::mt19937 random(11);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint16_t* s = p.samples.data() + (std::size_t(y) * w + x) * 5;
            if (y < px(40) && x < px(30)) {
                for (unsigned c = 0; c < 4; ++c) s[c] = static_cast<std::uint16_t>(random() % 65536);
                s[4] = 65535;
            } else if (y < px(40)) {
                std::copy(kS.begin(), kS.end(), s);
                s[4] = 65535;
            } else if (x < px(30)) {
                std::copy(kT.begin(), kT.end(), s);
                s[4] = 65535;
            }
        }
    }
    for (auto& layer : doc.edit_page(0).layers) {
        if (layer.id != "pic") continue;
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        layer.panel_clip = layer.panel_each = false;  // (the whole page, not cut to its panels)
    }
    return doc;
}

// The page is in inks after an op: no RGB pixels left on it, its rasters in its space and depth.
void all_in_inks(const core::Document& doc) {
    for (const auto& page : doc.pages) {
        if (!page->color) continue;
        for (const auto& layer : page->layers) {
            QVERIFY2(!layer.raster_png || layer.raster_png->empty(), layer.id.c_str());
            QVERIFY2(!layer.color_raster, layer.id.c_str());
            if (layer.ink_raster) {
                const auto header = core::ink_raster_header(*layer.ink_raster);
                QVERIFY(header.space == page->color->space && header.depth == page->color->depth);
            }
        }
    }
}

}  // namespace

class TestInkEdit : public QObject {
    Q_OBJECT

private slots:
    // delete_area: the area's pixels cleared, every other pixel's 16-bit inks and alpha as they were.
    void untouchedPixelsKeepTheirInks() {
        const core::Document doc = ink_book();
        const core::InkPixels before = pixels_of(doc, "pic");
        const core::Document after =
            run_ops(doc, Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({0, 0, 20, 20})}}}}})).doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, "pic");
        QCOMPARE(got.width, before.width);
        QCOMPARE(got.height, before.height);
        for (std::uint32_t y = 0; y < got.height; ++y) {
            for (std::uint32_t x = 0; x < got.width; ++x) {
                if (x + 2 < px(20) && y + 2 < px(20)) {
                    QCOMPARE(at(got, x, y)[4], std::uint16_t(0));
                } else if (x > px(20) + 2 || y > px(20) + 2) {
                    for (unsigned c = 0; c < 5; ++c) QCOMPARE(at(got, x, y)[c], at(before, x, y)[c]);
                }
            }
        }
        QVERIFY(pixels_of(doc, "pic").samples == before.samples);  // (the book it was applied to: as it was)
    }

    // A soft eraser: the alpha goes down, the colour of what stays is the colour it was.
    void eraserKeepsTheColours() {
        const core::Document doc = ink_book();
        const core::Document after = run_ops(doc, Json::array({Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "pic"},
                                                                   {"points", Json::array({Json::array({5, 60}), Json::array({25, 60})})},
                                                                   {"width_mm", 4}, {"texture", "soft"}}}))
                                        .doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, "pic");
        int partly = 0;
        for (std::uint32_t y = px(40); y < got.height; ++y) {
            for (std::uint32_t x = 0; x < px(30); ++x) {
                const std::uint16_t* p = at(got, x, y);
                if (p[4] == 0) continue;
                for (unsigned c = 0; c < 4; ++c) QCOMPARE(p[c], kT[c]);
                if (p[4] < 65535) ++partly;
            }
        }
        QVERIFY(partly > 0);
    }

    // transform_area: what moved keeps its inks (resampled, a colour all over stays that colour).
    void movedPixelsKeepTheirInks() {
        const core::Document doc = ink_book();
        const core::Document after =
            run_ops(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({32, 2, 26, 30})}}},
                                           {"matrix", Json::array({1, 0, 0, 1, -30, 45})}}}))
                .doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, "pic");
        for (std::uint32_t y = px(47) + 3; y < px(77) - 3; ++y) {
            for (std::uint32_t x = px(2) + 3; x < px(28) - 3; ++x) {
                for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(got, x, y)[c], kS[c]);
                QCOMPARE(at(got, x, y)[4], std::uint16_t(65535));
            }
        }
        QCOMPARE(at(got, px(45), px(15))[4], std::uint16_t(0));  // (where it was: clear)
        QVERIFY(layer_of(after, "pic").patches.empty());           // (drawn into the pixels, not left as a picture)
    }

    // merge_down: the line merged into the pixels takes the page's rules (Genko's black: K 100% alone); the rest as it
    // was.
    void mergedLinesTakeThePageRules() {
        core::Document doc = ink_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen-x"}, {"after", "pic"}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen-x"},
                                             {"points", Json::array({Json::array({5, 50}), Json::array({25, 50})})}, {"width_mm", 3}}}))
                  .doc;
        const core::InkPixels before = pixels_of(doc, "pic");
        const core::Document after = run_ops(doc, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", "pen-x"}}})).doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, "pic");
        const std::uint16_t* line = at(got, px(15), px(50));
        QCOMPARE(line[0], std::uint16_t(0));
        QCOMPARE(line[1], std::uint16_t(0));
        QCOMPARE(line[2], std::uint16_t(0));
        QCOMPARE(line[3], std::uint16_t(65535));
        for (unsigned c = 0; c < 5; ++c) QCOMPARE(at(got, 10, 10)[c], at(before, 10, 10)[c]);
    }

    // Every op of the step on an ink page: done, and the page in inks after it.
    void everyOpLeavesInks_data() {
        QTest::addColumn<QString>("ops");
        QTest::newRow("smudge") << R"([{"op": "smudge", "page": 1, "layer_id": "pic", "points": [[20, 15], [45, 30]]}])";
        QTest::newRow("liquify") << R"([{"op": "liquify", "page": 1, "layer_id": "pic", "points": [[30, 22], [50, 30]], "width_mm": 12}])";
        QTest::newRow("flood_fill") << R"([{"op": "flood_fill", "page": 1, "layer_id": "pic", "x_mm": 45, "y_mm": 70, "rgb": [0, 0, 200]}])";
        QTest::newRow("gradient_fill") << R"([{"op": "gradient_fill", "page": 1, "layer_id": "pic", "from": [10, 10], "to": [50, 70]}])";
        QTest::newRow("add_shape") << R"([{"op": "add_shape", "page": 1, "layer_id": "pic", "shape": "line", "points": [[20, 30], [50, 40]]}])";
        QTest::newRow("fill_gaps") << R"([{"op": "fill_gaps", "page": 1, "layer_id": "pic", "max_mm": 3}])";
        QTest::newRow("put_color_raster")
            << R"([{"op": "put_color_raster", "page": 1, "width": 1, "height": 1, "precision": "u16", "pixels": [1000, 30000, 60000, 65535]}])";
        QTest::newRow("merge_visible") << R"([{"op": "merge_visible", "page": 1, "id": "vis"}])";
        QTest::newRow("convert_layer") << R"([{"op": "add_layer", "page": 1, "kind": "pen", "id": "pen-y"},
            {"op": "add_stroke", "page": 1, "layer_id": "pen-y", "points": [[5, 5], [50, 70]], "width_mm": 2, "rgb": [200, 30, 30]},
            {"op": "convert_layer", "page": 1, "id": "pen-y", "to": "paint"}])";
        QTest::newRow("set_page_spec") << R"([{"op": "set_page_spec", "preset": "b5"}])";
        QTest::newRow("erase") << R"([{"op": "erase", "page": 1, "layer_id": "pic", "points": [[40, 50], [50, 60]], "texture": "rough"}])";
        QTest::newRow("paste") << R"([{"op": "paste", "page": 1, "layer_id": "pic", "items": {"strokes": [{"points": [[12, 12], [30, 12]]}]}}])";
        QTest::newRow("ruler_to_layer") << R"([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[5, 10], [55, 70]], "id": "r1"},
            {"op": "ruler_to_layer", "page": 1, "id": "r1", "width_mm": 0.3, "rgb": [200, 10, 10]}])";
        QTest::newRow("render_prims") << R"([{"op": "add_prim3d", "page": 1, "id": "box", "kind": "box", "pos": [30, 40, 0], "size": [20, 15, 18], "rot": [0.3, 0.6, 0]},
            {"op": "render_prims", "page": 1, "layer_id": "pic"}])";
    }
    void everyOpLeavesInks() {
        QFETCH(QString, ops);
        const core::Document doc = ink_book();
        core::Document after;
        try {
            after = run_ops(doc, Json::parse(ops.toStdString())).doc;
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
        all_in_inks(after);
    }

    // The filters wait for their own step: not_yet_ported, nothing done.
    void filtersWaitTheirTurn() {
        const std::string refused =
            refusal(ink_book(), Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "mosaic"}, {"block", 4}}}));
        QVERIFY2(refused.rfind("not_yet_ported", 0) == 0, refused.c_str());
    }

    // スポイト: the inks at a point (the page as drawn, or the layer's own pixel); a colour for inks.
    void inksAtAPoint() {
        const core::Document doc = ink_book();
        const core::Page& page = doc.page(0);
        const auto seen = render::ink_edit::inks_at(page, &doc, 45, 20);
        QVERIFY(seen);
        for (unsigned c = 0; c < 4; ++c) QVERIFY(std::abs((*seen)[c] - kS[c] / 65535.0) <= 1.0 / 65535);
        const auto own = render::ink_edit::layer_inks_at(page, layer_of(doc, "pic"), 15, 60);  // (T)
        QVERIFY(own);
        for (unsigned c = 0; c < 4; ++c) QCOMPARE((*own)[c], kT[c] / 65535.0);
        QVERIFY(!render::ink_edit::layer_inks_at(page, layer_of(doc, "pic"), 45, 60));  // (clear there)
        QVERIFY(!render::ink_edit::inks_at(page, &doc, -1, 20));
        // a colour for inks: the one shown, not Genko's black nor one the table holds for other inks
        const core::PageColor& color = *page.color;
        const std::vector<double> k100{0, 0, 0, 1};
        const std::array<int, 3> black = render::ink::rgb_for_inks(color, k100);
        QVERIFY(!(black[0] == black[1] && black[1] == black[2] && (black[0] == 0 || black[0] == 10 || black[0] == 15 || black[0] == 20)));
        core::PageColor held = color;
        held.inks.push_back(core::InkEntry{black, {0.5, 0, 0, 0}});
        const std::array<int, 3> moved = render::ink::rgb_for_inks(held, k100);
        QVERIFY(moved != black);
        QVERIFY(render::ink::same_inks(color, k100, k100));
        QVERIFY(!render::ink::same_inks(color, k100, {0, 0, 0, 0.99}));
        // 色域外: a pure sRGB green is out of the CMYK profile, a muted brown is in it; a Lab page holds every colour
        QVERIFY(render::ink::gamut_distance(color, {0, 255, 0}) > 3.0);
        QVERIFY(render::ink::gamut_distance(color, {150, 120, 100}) < 3.0);
        core::PageColor lab;
        lab.space = core::InkSpace::Lab;
        QCOMPARE(render::ink::gamut_distance(lab, {0, 255, 0}), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestInkEdit)
#include "test_ink_edit.moc"
