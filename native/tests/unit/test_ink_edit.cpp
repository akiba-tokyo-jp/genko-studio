// color-mode@1, the second step (SPEC.md COLOR-02): the pixel ops of paint layers on CMYK and Lab pages
// (render/ink_edit). What an op does not change keeps its inks exactly (16 bits as they were); what it moves, copies or
// lays on new paper keeps them (a rich black whose screen colour is a Genko black among them); a line merged into the
// pixels takes the page's rules (Genko's black: K alone) while what was there keeps its inks; every op of the step
// leaves the page in inks; the eyedropper's inks at a point; a colour for
// chosen inks (a grey the rules give them, else one not the rules' nor the table's for other inks); the gamut mark.

#include <QtTest>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
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

// A multichannel page of `n` plates (Cyan, Magenta, Yellow, Black, then more: up to 32; 16 bits; 60 × 80 mm) with a
// paint layer "pic" at 200 dpi: its top half M (plate k at density (k + 1) / (n + 1)), its bottom left N (plate k at
// 1 − (k + 1) / (n + 1)), its bottom right clear.
core::Document multi_book(unsigned n) {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("multi", core::Num(1), 1, spec, core::Binding::Right);
    doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}}})).doc;
    core::PageColor color;
    color.space = core::InkSpace::Multi;
    color.depth = 16;
    color.from = "rgb";
    const std::array<std::pair<const char*, std::array<int, 3>>, 6> named{
        {{"Cyan", {0, 174, 239}}, {"Magenta", {236, 0, 140}}, {"Yellow", {255, 242, 0}}, {"Black", {35, 31, 32}}, {"Orange", {255, 128, 0}}, {"Green", {0, 160, 80}}}};
    for (unsigned k = 0; k < n; ++k) {
        core::Plate plate;
        plate.name = k < named.size() ? named[k].first : "Plate " + std::to_string(k + 1);
        plate.color = k < named.size() ? named[k].second : std::array<int, 3>{static_cast<int>(k * 7 % 256), 90, static_cast<int>(255 - k * 5)};
        plate.key = k < 3;
        color.plates.push_back(std::move(plate));
    }
    doc.edit_page(0).color = color;
    const std::uint32_t w = static_cast<std::uint32_t>(render::mm_to_px(60, 200)), h = static_cast<std::uint32_t>(render::mm_to_px(80, 200));
    core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Multi, 16, w, h, n);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = std::size_t(y) * w + x;
            if (y >= px(40) && x >= px(30)) continue;
            for (unsigned k = 0; k < n; ++k) p.set(i, k, y < px(40) ? (k + 1.0) / (n + 1) : 1.0 - (k + 1.0) / (n + 1));
            p.set(i, n, 1.0);
        }
    }
    for (auto& layer : doc.edit_page(0).layers) {
        if (layer.id != "pic") continue;
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        layer.panel_clip = layer.panel_each = false;
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

    // color-model@1: a grey page of 32 bits (lightness as floats, HDR kept) and a CMYK page of 32 bits: delete_area clears
    // the area and leaves every other float as it was; transform_area carries what it moves whole (an HDR lightness
    // too); the eraser lowers the alpha alone.
    void greyAndFloatPages() {
        core::Document doc = ink_book();
        doc = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "gray"}, {"depth", 32}}})).doc;
        for (auto& layer : doc.edit_page(0).layers) {  // (an HDR stripe at the top right)
            if (layer.id != "pic") continue;
            core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
            QVERIFY(p.is_float() && p.space == core::InkSpace::Gray);
            for (std::uint32_t y = px(5); y < px(25); ++y)
                for (std::uint32_t x = px(35); x < px(55); ++x) p.set(std::size_t(y) * p.width + x, 0, 2.5);
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        }
        const core::InkPixels before = pixels_of(doc, "pic");
        const core::Document deleted =
            run_ops(doc, Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({0, 0, 20, 20})}}}}})).doc;
        all_in_inks(deleted);
        const core::InkPixels got = pixels_of(deleted, "pic");
        QVERIFY(got.is_float());
        for (std::uint32_t y = 0; y < got.height; y += 3) {
            for (std::uint32_t x = 0; x < got.width; x += 3) {
                const std::size_t i = std::size_t(y) * got.width + x;
                if (x + 2 < px(20) && y + 2 < px(20)) {
                    QCOMPARE(got.value(i, 1), 0.0);
                    QCOMPARE(got.value(i, 0), 1.0);  // (clear: white, as a blank grey raster)
                } else if (x > px(20) + 2 || y > px(20) + 2) {
                    QCOMPARE(got.floats[i * 2], before.floats[i * 2]);
                    QCOMPARE(got.floats[i * 2 + 1], before.floats[i * 2 + 1]);
                }
            }
        }
        // moved by whole pixels: the HDR lightness carried as it is
        const core::Document moved =
            run_ops(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({32, 2, 26, 30})}}},
                                           {"matrix", Json::array({1, 0, 0, 1, -30, 45})}}})).doc;
        const core::InkPixels shifted = pixels_of(moved, "pic");
        QCOMPARE(shifted.value(std::size_t(px(60)) * shifted.width + px(15), 0), 2.5);
        QCOMPARE(shifted.value(std::size_t(px(60)) * shifted.width + px(15), 1), 1.0);
        // the eraser: the alpha lower, the lightness as it was
        const core::Document erased = run_ops(doc, Json::array({Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "pic"},
                                                                    {"points", Json::array({Json::array({35, 15}), Json::array({55, 15})})},
                                                                    {"width_mm", 4}, {"texture", "soft"}}})).doc;
        const core::InkPixels rubbed = pixels_of(erased, "pic");
        int partly = 0;
        for (std::uint32_t y = px(5); y < px(25); ++y) {
            for (std::uint32_t x = px(35); x < px(55); ++x) {
                const std::size_t i = std::size_t(y) * rubbed.width + x;
                if (rubbed.value(i, 1) == 0) continue;
                QCOMPARE(rubbed.value(i, 0), 2.5);
                if (rubbed.value(i, 1) < 1) ++partly;
            }
        }
        QVERIFY(partly > 0);
        // a CMYK page of 32 bits: delete_area leaves the other floats as they were
        const core::Document cmyk = run_ops(ink_book(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 32}}})).doc;
        const core::InkPixels was = pixels_of(cmyk, "pic");
        QVERIFY(was.is_float() && was.space == core::InkSpace::Cmyk);
        const core::InkPixels now = pixels_of(run_ops(cmyk, Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "pic"},
                                                                              {"area", Json{{"rect", Json::array({0, 0, 20, 20})}}}}})).doc, "pic");
        const std::size_t far = std::size_t(px(60)) * now.width + px(10);
        for (unsigned c = 0; c < 5; ++c) QCOMPARE(now.floats[far * 5 + c], was.floats[far * 5 + c]);
        // an op through the picture the layer shows (smudge) on a grey page of 8 bits: what it did not change keeps its
        // samples
        const core::Document grey8 = run_ops(ink_book(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "gray"}}})).doc;
        const core::Document smudged = run_ops(grey8, Json::array({Json{{"op", "smudge"}, {"page", 1}, {"layer_id", "pic"}, {"mode", "blur"},
                                                                       {"points", Json::array({Json::array({33, 40}), Json::array({57, 40})})}, {"width_mm", 6}}})).doc;
        all_in_inks(smudged);
        const core::InkPixels g0 = pixels_of(grey8, "pic"), g1 = pixels_of(smudged, "pic");
        const std::size_t corner = std::size_t(px(75)) * g0.width + px(5);
        QCOMPARE(g1.samples[corner * 2], g0.samples[corner * 2]);
        QCOMPARE(g1.samples[corner * 2 + 1], g0.samples[corner * 2 + 1]);
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

    // PC3 B2: a piece of inks turned, flipped or sheared as it is pasted stays inks — resampled as inks (premultiplied by
    // the alpha), as transform_area moves them —, never its picture: a colour all over the same inks.
    void turnedPiecesStayInks() {
        const core::Document doc = ink_book();
        // (the piece: S, from the top right — 35..45 × 5..15 mm)
        const auto piece = render::ink_edit::lift_piece(doc.page(0), layer_of(doc, "pic"),
                                                        Json{{"poly", Json::array({Json::array({35, 5}), Json::array({45, 5}), Json::array({45, 15}), Json::array({35, 15})})}});
        QVERIFY(piece);
        const core::Patch patch = render::ink_edit::piece_patch(*piece, *doc.page(0).color);
        Json one = patch.attrs;
        one["png"] = core::b64encode(*patch.png);
        const Json items{{"strokes", Json::array()}, {"patches", Json::array({one})}};
        // turned a quarter about (40, 10) and moved down by 50 mm; flipped; sheared
        for (const Json& matrix : {Json::array({0, 1, -1, 0, 50, 10}), Json::array({-1, 0, 0, 1, 80, 50}), Json::array({1, 0, 0.3, 1, -30, 50})}) {
            const core::Document pasted =
                run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pasted"}},
                                          Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pasted"}, {"items", items}, {"matrix", matrix}}}))
                    .doc;
            QVERIFY(layer_of(pasted, "pasted").patches.empty());
            QVERIFY(layer_of(pasted, "pasted").ink_raster);
            const core::InkPixels laid = pixels_of(pasted, "pasted");
            // (where the middle of the piece, (40, 10) mm, went)
            const auto m = matrix;
            const double x = m[0].get<double>() * 40 + m[2].get<double>() * 10 + m[4].get<double>(),
                         y = m[1].get<double>() * 40 + m[3].get<double>() * 10 + m[5].get<double>();
            const std::uint16_t* middle = at(laid, px(x), px(y));
            for (unsigned c = 0; c < 4; ++c) QVERIFY2(std::abs(int(middle[c]) - int(kS[c])) <= 1, qPrintable(QString::number(c)));
            QCOMPARE(middle[4], std::uint16_t{65535});
            QCOMPARE(at(laid, px(5), px(5))[4], std::uint16_t{0});  // (nothing laid far from it)
        }
    }

    // A piece of 600 dpi inks pasted onto a layer with no inks yet: the layer's inks made at the piece's 600 dpi (as a
    // precise piece's are), each of its pixels laid whole — a pattern of single pixels kept.
    void finePiecesKeepTheirResolution() {
        core::Document doc = ink_book();
        const std::uint32_t w = static_cast<std::uint32_t>(render::mm_to_px(60, 600)), h = static_cast<std::uint32_t>(render::mm_to_px(80, 600));
        core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Cmyk, 16, w, h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint16_t* s = p.samples.data() + (std::size_t(y) * w + x) * 5;
                const auto& colour = (x + y) % 2 == 0 ? kS : kT;  // (a checkerboard of single pixels)
                std::copy(colour.begin(), colour.end(), s);
                s[4] = 65535;
            }
        }
        for (auto& layer : doc.edit_page(0).layers)
            if (layer.id == "pic") layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        const auto piece = render::ink_edit::lift_piece(doc.page(0), layer_of(doc, "pic"),
                                                        Json{{"poly", Json::array({Json::array({10, 10}), Json::array({20, 10}), Json::array({20, 20}), Json::array({10, 20})})}});
        QVERIFY(piece);
        const core::Patch patch = render::ink_edit::piece_patch(*piece, *doc.page(0).color);
        Json one = patch.attrs;
        one["png"] = core::b64encode(*patch.png);
        const Json items{{"strokes", Json::array()}, {"patches", Json::array({one})}};
        const core::Document pasted = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pasted"}},
                                                                Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pasted"}, {"items", items}}}))
                                          .doc;
        const core::InkPixels laid = pixels_of(pasted, "pasted");
        QCOMPARE(laid.width, w);
        QCOMPARE(laid.height, h);
        for (std::uint32_t y = px(14) * 3; y < px(16) * 3; ++y) {
            for (std::uint32_t x = px(14) * 3; x < px(16) * 3; ++x) {
                const auto& colour = (x + y) % 2 == 0 ? kS : kT;
                for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(laid, x, y)[c], colour[c]);
            }
        }
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

    // New paper (set_page_spec): inks at the working 200 dpi moved to the very inks they were before; inks of another
    // resolution (2 pixels a mm here: a PSD's, put_raster's, resized without resampling) keep it — not taken for 200 dpi —,
    // each pixel the inks that moved there with the basic frame.
    void newPaperKeepsTheResolution() {
        const core::Document doc = ink_book();
        const core::InkPixels working = pixels_of(run_ops(doc, Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}})).doc, "pic");
        const std::string working_ref =
            core::bytes_ref(std::string_view(reinterpret_cast<const char*>(working.samples.data()), working.samples.size() * sizeof(std::uint16_t)));
        QCOMPARE(working.width, 1638u);
        QCOMPARE(working_ref, std::string("sha256:141f5b0142aacbdefb7977871b2dfa78caaeeb84b4c0099ec22fbd780311ae5e"));
        // (60 × 80 mm at 2 pixels a mm: 120 × 160; S at the top right, T at the bottom left, as ink_book's)
        core::Document coarse = doc;
        core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Cmyk, 16, 120, 160);
        for (std::uint32_t y = 0; y < 160; ++y) {
            for (std::uint32_t x = 0; x < 120; ++x) {
                std::uint16_t* s = p.samples.data() + (std::size_t(y) * 120 + x) * 5;
                if (y < 80 && x >= 60) std::copy(kS.begin(), kS.end(), s);
                if (y >= 80 && x < 60) std::copy(kT.begin(), kT.end(), s);
                s[4] = (y < 80 || x < 60) ? 65535 : 0;
            }
        }
        for (auto& layer : coarse.edit_page(0).layers) {
            if (layer.id == "pic") layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        }
        const core::Document moved = run_ops(coarse, Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}})).doc;
        all_in_inks(moved);
        const core::InkPixels got = pixels_of(moved, "pic");
        const double w = moved.page(0).spec.width_mm.value(), h = moved.page(0).spec.height_mm.value();
        QCOMPARE(got.width, static_cast<std::uint32_t>(std::lround(w * 2)));
        QCOMPARE(got.height, static_cast<std::uint32_t>(std::lround(h * 2)));
        const std::string side = coarse.start_side.value_or("");
        const core::Rect a = coarse.page(0).inner_rect_mm(side), b = moved.page(0).inner_rect_mm(side);
        const auto there = [&](double x, double y) {
            const double nx = b.x.value() + (x - a.x.value()) * (b.width / a.width).value();
            const double ny = b.y.value() + (y - a.y.value()) * (b.height / a.height).value();
            return at(got, static_cast<std::uint32_t>(nx * 2), static_cast<std::uint32_t>(ny * 2));
        };
        for (const auto& [x, y, inks] : {std::tuple{45.0, 20.0, kS}, std::tuple{15.0, 60.0, kT}}) {
            const std::uint16_t* q = there(x, y);
            for (unsigned c = 0; c < 4; ++c) QVERIFY2(std::abs(int(q[c]) - int(inks[c])) <= 1, qPrintable(QStringLiteral("(%1, %2) %3: %4").arg(x).arg(y).arg(c).arg(q[c])));
            QCOMPARE(q[4], std::uint16_t{65535});
        }
        QCOMPARE(there(50, 70)[4], std::uint16_t{0});  // (the clear bottom right)
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
        const core::InkPixels laid = pixels_of(pasted, "pic");
        const std::uint16_t* p = at(laid, px(40), px(55));
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

    // smudge works an ink layer's own inks (PC4 E5, design section E 0.4: no picture lent): what it did not reach keeps its
    // inks, and what it mixed mixes the plates — a rich black whose screen colour is a Genko black's does not become K
    // alone (as it would through the screen colour).
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
        const core::InkPixels before = pixels_of(doc, "pic"), got = pixels_of(after, "pic");
        QCOMPARE(got.width, before.width);
        // (the smudge's box: the line and 1.5 times its width around it, 33..57 × 40 mm with 9 mm: 24..66 × 31..49 mm)
        const auto reached = [](std::uint32_t x, std::uint32_t y) { return x + 1 >= px(24) && x <= px(66) + 1 && y + 1 >= px(31) && y <= px(49) + 1; };
        int unchanged = 0, changed = 0, rich = 0;
        for (std::uint32_t y = 0; y < before.height; ++y) {
            for (std::uint32_t x = 0; x < before.width; ++x) {
                const std::uint16_t* a = at(before, x, y);
                const std::uint16_t* b = at(got, x, y);
                if (std::equal(a, a + 5, b)) {
                    ++unchanged;
                    continue;
                }
                ++changed;
                if (!reached(x, y)) QFAIL(qPrintable(QStringLiteral("pixel (%1, %2) (past the smudge) lost its inks").arg(x).arg(y)));
                // a rich black mixed stays rich: each plate keeps at least what the strength (0.6) leaves of it
                if (std::equal(kRich.begin(), kRich.end(), a) && a[4] == 65535) {
                    ++rich;
                    for (unsigned c = 0; c < 4; ++c) {
                        if (b[c] < kRich[c] * 0.35) QFAIL(qPrintable(QStringLiteral("pixel (%1, %2): plate %3 of the rich black %4 → %5").arg(x).arg(y).arg(c).arg(kRich[c]).arg(b[c])));
                    }
                }
            }
        }
        QVERIFY2(changed > 100 && unchanged > 1000 && rich > 100, qPrintable(QStringLiteral("%1 changed, %2 not, %3 rich").arg(changed).arg(unchanged).arg(rich)));
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

    // Review B, F: a multichannel page of more plates than CMYK's (6, and 32 — the most a page has): what an op moves
    // (by whole pixels, and resampled bicubic), lends (smudge) or lays on new paper (set_page_spec) keeps every plate's
    // density; what it does not reach keeps its samples. (Each plate and the alpha were held in arrays of 5 before.)
    void manyPlatesKeepTheirInks_data() {
        QTest::addColumn<int>("plates");
        QTest::newRow("6") << 6;
        QTest::newRow("32") << 32;
    }
    void manyPlatesKeepTheirInks() {
        QFETCH(int, plates);
        const unsigned n = static_cast<unsigned>(plates);
        const core::Document doc = multi_book(n);
        const core::InkPixels before = pixels_of(doc, "pic");
        QCOMPARE(before.planes, n);
        const auto m_at = [&](unsigned k) { return (k + 1.0) / (n + 1); };
        const auto n_at = [&](unsigned k) { return 1.0 - (k + 1.0) / (n + 1); };
        // the plates of a pixel (x, y in mm) against a colour, within a sample
        const auto is = [&](const core::InkPixels& p, double x_mm, double y_mm, const std::function<double(unsigned)>& want, const char* what) {
            const std::size_t i = std::size_t(px(y_mm)) * p.width + px(x_mm);
            QCOMPARE(p.planes, n);
            QVERIFY2(std::abs(p.value(i, n) - 1.0) <= 1.0 / 65535, what);
            for (unsigned k = 0; k < n; ++k) {
                QVERIFY2(std::abs(p.value(i, k) - want(k)) <= 1.0 / 65535,
                         qPrintable(QStringLiteral("%1: plate %2 is %3, not %4").arg(QString::fromLatin1(what)).arg(k).arg(p.value(i, k)).arg(want(k))));
            }
        };
        // moved resampled (a fraction of a pixel: bicubic): the colour all over stays the colour, where it was clear
        const core::Document resampled =
            run_ops(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({5, 5, 50, 30})}}},
                                           {"matrix", Json::array({1, 0, 0, 1, 0.37, 40.29})}, {"interp", "bicubic"}}})).doc;
        all_in_inks(resampled);
        const core::InkPixels r = pixels_of(resampled, "pic");
        is(r, 45, 60, m_at, "moved resampled");
        QCOMPARE(r.value(std::size_t(px(20)) * r.width + px(30), n), 0.0);  // (where it was: clear)
        is(r, 2, 2, m_at, "not reached");
        // moved by whole pixels (200 px = 25.4 mm) over N: inside, M as it was; at its edges (part of a pixel covered) M
        // mixed over N plate by plate
        const core::Document whole =
            run_ops(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({5.1, 5.1, 20, 30})}}},
                                           {"matrix", Json::array({1, 0, 0, 1, 0, 25.4})}}})).doc;
        const core::InkPixels wp = pixels_of(whole, "pic");
        is(wp, 15, 50, m_at, "moved whole");
        const std::size_t edge = std::size_t(px(50)) * wp.width + std::size_t(std::floor(5.1 * 200 / 25.4));  // (its left edge: part covered)
        for (unsigned k = 0; k < n; ++k) {
            const double v = wp.value(edge, k);
            QVERIFY2(v >= std::min(m_at(k), n_at(k)) - 1e-4 && v <= std::max(m_at(k), n_at(k)) + 1e-4, qPrintable(QStringLiteral("edge plate %1: %2").arg(k).arg(v)));
        }
        // lent (smudge works the picture the layer shows): what it did not reach keeps every plate's sample
        const core::Document smudged = run_ops(doc, Json::array({Json{{"op", "smudge"}, {"page", 1}, {"layer_id", "pic"}, {"mode", "blur"},
                                                                     {"points", Json::array({Json::array({10, 38}), Json::array({25, 42})})}, {"width_mm", 4}}})).doc;
        all_in_inks(smudged);
        const core::InkPixels sm = pixels_of(smudged, "pic");
        QCOMPARE(sm.planes, n);
        for (const auto& [x, y] : {std::pair{50.0, 10.0}, std::pair{5.0, 70.0}}) {
            const std::size_t i = std::size_t(px(y)) * sm.width + px(x);
            for (unsigned k = 0; k <= n; ++k) QCOMPARE(sm.samples[i * (n + 1) + k], before.samples[i * (n + 1) + k]);
        }
        // new paper (set_page_spec: the layer laid on the B5 page): M and N kept on every plate
        const core::Document b5 = run_ops(doc, Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}})).doc;
        all_in_inks(b5);
        const core::InkPixels laid = pixels_of(b5, "pic");
        QCOMPARE(laid.planes, n);
        QVERIFY(laid.width != before.width);
        int m_seen = 0, n_seen = 0;
        const std::size_t probe = std::size_t(px(10)) * before.width + px(10), probe_n = std::size_t(px(70)) * before.width + px(10);
        for (std::size_t i = 0; i < laid.pixel_count(); ++i) {
            const auto same = [&](std::size_t j) {
                for (unsigned k = 0; k <= n; ++k) {
                    if (laid.samples[i * (n + 1) + k] != before.samples[j * (n + 1) + k]) return false;
                }
                return true;
            };
            m_seen += same(probe);
            n_seen += same(probe_n);
        }
        QVERIFY2(m_seen > 1000 && n_seen > 1000, qPrintable(QStringLiteral("M %1, N %2").arg(m_seen).arg(n_seen)));
    }
};

QTEST_GUILESS_MAIN(TestInkEdit)
#include "test_ink_edit.moc"
