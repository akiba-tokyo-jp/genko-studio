// color-mode@1, the second step (SPEC.md COLOR-02): the pixel ops of paint layers on CMYK and Lab pages
// (render/ink_edit). What an op does not change keeps its inks exactly (16 bits as they were); what it moves, copies or
// lays on new paper keeps them (a rich black whose screen colour is a Genko black among them); a line merged into the
// pixels takes the page's rules (Genko's black: K alone) while what was there keeps its inks; every op of the step
// leaves the page in inks; the filters still wait (not_yet_ported); the eyedropper's inks at a point; a colour for
// chosen inks (a grey the rules give them, else one not the rules' nor the table's for other inks); the gamut mark.

#include <QtTest>

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
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
// A rich black (C60 M40 Y40 K100) whose colour on screen is (0, 0, 0): a Genko black's.
constexpr std::array<std::uint16_t, 4> kRich{39321, 26214, 26214, 65535};

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
        QCOMPARE(at(got, px(45), px(60))[4], std::uint16_t(0));  // (what was clear stays clear)
    }

    // The merges composite the layers alone: where none of them has anything, the layer they make stays clear (no
    // paper of no ink over what is under it).
    void mergesKeepWhatWasClear_data() {
        QTest::addColumn<QString>("op");
        QTest::addColumn<QString>("layer");
        QTest::newRow("merge_down") << R"({"op": "merge_down", "page": 1, "id": "pen-x"})" << "pic";
        QTest::newRow("merge_layers") << R"({"op": "merge_layers", "page": 1, "ids": ["pic", "pen-x"]})" << "pic";
        QTest::newRow("merge_visible") << R"({"op": "merge_visible", "page": 1, "id": "vis"})" << "vis";
        QTest::newRow("convert_layer") << R"({"op": "convert_layer", "page": 1, "id": "pen-x", "to": "paint"})" << "pen-x";
    }
    void mergesKeepWhatWasClear() {
        QFETCH(QString, op);
        QFETCH(QString, layer);
        core::Document doc = ink_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen-x"}, {"after", "pic"}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen-x"},
                                             {"points", Json::array({Json::array({5, 50}), Json::array({25, 50})})}, {"width_mm", 3}}}))
                  .doc;
        const core::Document after = run_ops(doc, Json::array({Json::parse(op.toStdString())})).doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, layer.toStdString());
        QCOMPARE(at(got, px(45), px(60))[4], std::uint16_t(0));
        QCOMPARE(at(got, px(15), px(60))[4], layer == "pen-x" ? std::uint16_t(0) : std::uint16_t(65535));
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
        QTest::newRow("stamp_material") << R"([{"op": "stamp_material", "page": 1, "material_id": "speed-h"}])";
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

    // A rich black (its screen colour a Genko black's) moved, merged under a line and copied: its inks as they were.
    void richBlackKeepsItsInks() {
        core::Document doc = ink_book();
        for (auto& layer : doc.edit_page(0).layers) {
            if (layer.id != "pic") continue;
            core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
            for (std::uint32_t y = 0; y < px(40); ++y)
                for (std::uint32_t x = px(30); x < p.width; ++x) std::copy(kRich.begin(), kRich.end(), p.samples.begin() + std::ptrdiff_t((std::size_t(y) * p.width + x) * 5));
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        }
        const std::array<int, 3> shown = render::ink::srgb_of(*doc.page(0).color, {0.6, 0.4, 0.4, 1.0}) == std::array<double, 3>{0, 0, 0}
                                             ? std::array<int, 3>{0, 0, 0} : std::array<int, 3>{-1, -1, -1};
        QVERIFY2(shown[0] == 0, "the test profile shows this rich black as (0, 0, 0)");
        // moved
        const core::Document moved =
            run_ops(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({32, 2, 26, 30})}}},
                                           {"matrix", Json::array({1, 0, 0, 1, -30, 45})}}}))
                .doc;
        const core::InkPixels got = pixels_of(moved, "pic");
        for (std::uint32_t y = px(47) + 3; y < px(77) - 3; ++y)
            for (std::uint32_t x = px(2) + 3; x < px(28) - 3; ++x)
                for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(got, x, y)[c], kRich[c]);
        // merged under a line in Genko's black: the line K alone, the rest as it was
        core::Document lined = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen-x"}, {"after", "pic"}},
                                                         Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen-x"},
                                                              {"points", Json::array({Json::array({35, 20}), Json::array({55, 20})})}, {"width_mm", 2}},
                                                         Json{{"op", "merge_down"}, {"page", 1}, {"id", "pen-x"}}}))
                                   .doc;
        const core::InkPixels merged = pixels_of(lined, "pic");
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(merged, px(45), px(35))[c], kRich[c]);
        const std::uint16_t* line = at(merged, px(45), px(20));
        QVERIFY(line[0] == 0 && line[1] == 0 && line[2] == 0 && line[3] == 65535);
        // copied (the GUI's lift) and pasted into a paint layer: its inks as they were
        const auto piece = render::ink_edit::lift_piece(doc.page(0), layer_of(doc, "pic"),
                                                           Json{{"poly", Json::array({Json::array({35, 5}), Json::array({45, 5}), Json::array({45, 15}), Json::array({35, 15})})}});
        QVERIFY(piece);
        const core::Patch patch = render::ink_edit::piece_patch(*piece, *doc.page(0).color);
        Json items = Json{{"strokes", Json::array()}, {"patches", Json::array()}};
        Json one = patch.attrs;
        one["png"] = core::b64encode(*patch.png);
        items["patches"].push_back(one);
        const core::Document pasted =
            run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pasted"}},
                                      Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pasted"}, {"items", items}, {"matrix", Json::array({1, 0, 0, 1, -30, 50})}}}))
                .doc;
        all_in_inks(pasted);
        QVERIFY(layer_of(pasted, "pasted").patches.empty());
        const core::InkPixels laid = pixels_of(pasted, "pasted");
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(laid, px(10), px(60))[c], kRich[c]);
        // pasted onto an RGB page: its picture, the key dropped
        core::Document rgb = core::new_episode("rgb", core::Num(1), 1, doc.page(0).spec, core::Binding::Right);
        rgb = run_ops(rgb, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "p"}},
                                        Json{{"op", "paste"}, {"page", 1}, {"layer_id", "p"}, {"items", items}}}))
                  .doc;
        QCOMPARE(layer_of(rgb, "p").patches.size(), std::size_t{1});
        QVERIFY(!layer_of(rgb, "p").patches[0].attrs.contains("gkir"));
        QVERIFY(!layer_of(rgb, "p").patches[0].attrs.contains("gkir_profile"));
    }

    // New paper (set_page_spec): the inks moved onto it, a colour all over the same inks.
    void newPaperKeepsTheInks() {
        const core::Document doc = ink_book();
        const core::Document after = run_ops(doc, Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}})).doc;
        all_in_inks(after);
        const core::InkPixels got = pixels_of(after, "pic");
        QVERIFY(got.width != pixels_of(doc, "pic").width);
        int s_seen = 0;
        for (std::uint32_t y = 0; y < got.height; ++y)
            for (std::uint32_t x = 0; x < got.width; ++x) {
                const std::uint16_t* p = at(got, x, y);
                if (p[4] == 65535 && p[0] == kS[0] && p[1] == kS[1] && p[2] == kS[2] && p[3] == kS[3]) ++s_seen;
            }
        QVERIFY(s_seen > 1000);
    }

    // A picture pasted onto a paint layer (no inks of its own: an RGB picture) is drawn into its inks there and then —
    // Genko's black K alone —, and the eraser takes it like the rest.
    void picturesBecomeInks() {
        const core::Document doc = ink_book();
        const render::Image black = render::Image::create("RGBA", render::Size{40, 40}, render::Ink{20, 20, 20, 255});
        Json items = Json{{"strokes", Json::array()},
                          {"patches", Json::array({Json{{"mode", "image"}, {"opacity", 1.0}, {"box", Json::array({35, 50, 10, 10})},
                                                        {"png", core::b64encode(render::write_png(black))}}})}};
        const core::Document pasted =
            run_ops(doc, Json::array({Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pic"}, {"items", items}}})).doc;
        all_in_inks(pasted);
        QVERIFY(layer_of(pasted, "pic").patches.empty());
        const std::uint16_t* p = at(pixels_of(pasted, "pic"), px(40), px(55));
        QVERIFY2(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 65535 && p[4] == 65535, "Genko's black: K alone");
        for (unsigned c = 0; c < 5; ++c) QCOMPARE(at(pixels_of(pasted, "pic"), px(45), px(20))[c], at(pixels_of(doc, "pic"), px(45), px(20))[c]);
        QCOMPARE(at(pixels_of(pasted, "pic"), px(55), px(75))[4], std::uint16_t(0));  // (the rest of the layer as it was)
        const core::Document erased = run_ops(pasted, Json::array({Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "pic"},
                                                                       {"points", Json::array({Json::array({33, 55}), Json::array({47, 55})})},
                                                                       {"width_mm", 4}}}))
                                          .doc;
        QCOMPARE(at(pixels_of(erased, "pic"), px(40), px(55))[4], std::uint16_t{0});
    }

    // flood_fill fills the layer of its role whatever layer_id says: that layer is the one lent, and what the fill did
    // not reach keeps its inks.
    void floodFillLendsTheLayerItFills() {
        const int w = render::mm_to_px(60, 72), h = render::mm_to_px(80, 72);
        render::Image picture = render::Image::create("RGB", render::Size{w, h}, render::Ink{255, 255, 255});
        picture.paste(render::Ink{0, 0, 0}, render::Box{0, 0, w, h / 2});
        const auto draft_inks = [](const core::Document& doc) {
            for (const auto& layer : doc.page(0).layers) {
                if (layer.role == core::LayerRole::Draft && layer.ink_raster) return core::decode_ink_raster(*layer.ink_raster);
            }
            throw core::Error("value", "no draft inks");
        };
        const core::Document placed = run_ops(ink_book(), Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"layer", "draft"},
                                                                            {"png_base64", core::b64encode(render::write_png(picture))}}}))
                                          .doc;
        const core::Document doc = run_ops(placed, Json::array({Json{{"op", "flood_fill"}, {"page", 1}, {"layer", "draft"}, {"layer_id", "pic"},
                                                                     {"x_mm", 30}, {"y_mm", 70}, {"rgb", Json::array({255, 0, 0})}}}))
                                       .doc;
        all_in_inks(doc);
        const core::InkPixels before = draft_inks(placed), got = draft_inks(doc);
        QCOMPARE(got.width, before.width);
        for (unsigned c = 0; c < 5; ++c) QCOMPARE(at(got, 10, 10)[c], at(before, 10, 10)[c]);  // (the black the fill did not reach)
        const std::uint16_t* filled = at(got, got.width / 2, got.height * 7 / 8);
        const std::uint16_t* white = at(before, got.width / 2, got.height * 7 / 8);
        QVERIFY2(filled[1] > white[1] + 20000, "the fill: red through the profile");
    }

    // smudge lays what it mixed as a picture of the box it changed: drawn into the picture the layer lent, so what it
    // did not change there keeps its inks (a rich black whose screen colour is a Genko black's does not become K alone).
    void smudgeKeepsTheInksItDidNotMix() {
        core::Document doc = ink_book();
        for (auto& layer : doc.edit_page(0).layers) {
            if (layer.id != "pic") continue;
            core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
            for (std::uint32_t y = 0; y < px(40); ++y)
                for (std::uint32_t x = px(30); x < p.width; ++x) std::copy(kRich.begin(), kRich.end(), p.samples.begin() + std::ptrdiff_t((std::size_t(y) * p.width + x) * 5));
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        }
        const Json smudge = Json{{"op", "smudge"}, {"page", 1}, {"layer_id", "pic"}, {"mode", "blur"},
                                 {"points", Json::array({Json::array({33, 40}), Json::array({57, 40})})}, {"width_mm", 6}};
        const core::Document after = run_ops(doc, Json::array({smudge})).doc;
        all_in_inks(after);
        // the same smudge on an RGB page holding the picture the layer shows: the pixels it did not change
        const render::Image shown = render::ink::to_rgba8(pixels_of(doc, "pic"), *doc.page(0).color);
        core::Document rgb = core::new_episode("rgb", core::Num(1), 1, doc.page(0).spec, core::Binding::Right);
        rgb = run_ops(rgb, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
        for (auto& layer : rgb.edit_page(0).layers) {
            if (layer.id != "pic") continue;
            layer.raster_png = std::make_shared<const std::string>(render::write_png(shown));
            layer.panel_clip = layer.panel_each = false;
        }
        rgb = run_ops(rgb, Json::array({smudge})).doc;
        const std::string was = shown.tobytes();
        const std::string now = render::raster::layer_pixels(rgb.page(0), layer_of(rgb, "pic")).convert("RGBA").tobytes();
        QCOMPARE(now.size(), was.size());
        const core::InkPixels before = pixels_of(doc, "pic"), got = pixels_of(after, "pic");
        int unchanged = 0, changed = 0;
        for (std::size_t i = 0; i < before.pixel_count(); ++i) {
            if (now.compare(i * 4, 4, was, i * 4, 4) != 0) {
                ++changed;
                continue;
            }
            ++unchanged;
            const std::uint16_t* a = before.samples.data() + i * 5;
            const std::uint16_t* b = got.samples.data() + i * 5;
            if (!std::equal(a, a + 5, b)) QFAIL(qPrintable(QStringLiteral("pixel %1 (not changed by the smudge) lost its inks").arg(i)));
        }
        QVERIFY(changed > 100 && unchanged > 1000);
    }

    // The eraser taking lines whole or to their crossings takes no pixels (as on an RGB page).
    void wholeLinesEraserLeavesThePixels() {
        const core::Document doc = ink_book();
        for (const char* mode : {"whole", "to_crossing"}) {
            const core::Document after = run_ops(doc, Json::array({Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "pic"}, {"mode", mode},
                                                                       {"points", Json::array({Json::array({5, 20}), Json::array({55, 20})})},
                                                                       {"width_mm", 4}}}))
                                            .doc;
            QCOMPARE(*layer_of(after, "pic").ink_raster, *layer_of(doc, "pic").ink_raster);
        }
    }

    // put_color_raster on an ink page: the precise pixels become inks, and the book does not keep their feature.
    void preciseRgbBecomesInks() {
        const core::Document doc = ink_book();
        const core::Document after = run_ops(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1}, {"precision", "u16"},
                                                                   {"pixels", Json::array({1000, 30000, 60000, 65535})}}}))
                                        .doc;
        all_in_inks(after);
        QVERIFY(std::find(after.features.begin(), after.features.end(), core::kColorRasterFeature) == after.features.end());
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
        QVERIFY((render::ink::rgb_for_inks(color, k100) == std::array<int, 3>{20, 20, 20}));       // (Genko's black)
        QVERIFY((render::ink::rgb_for_inks(color, {0, 0, 0, 0}) == std::array<int, 3>{255, 255, 255}));  // (no ink: white)
        const std::vector<double> red{0.1, 0.9, 0.8, 0};
        const std::array<int, 3> shown = render::ink::rgb_for_inks(color, red);
        QVERIFY(!(shown[0] == shown[1] && shown[1] == shown[2]));
        core::PageColor held = color;
        held.inks.push_back(core::InkEntry{shown, {0.5, 0, 0, 0}});
        const std::array<int, 3> moved = render::ink::rgb_for_inks(held, red);
        QVERIFY(moved != shown);
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
