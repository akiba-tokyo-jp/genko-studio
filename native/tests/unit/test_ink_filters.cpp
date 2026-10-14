// color-mode@1, the third step (SPEC.md COLOR-02, 群C): the filters and the corrections on CMYK and Lab pages
// (render/ink_filters, render/ink correct, InkCanvas::recolour). Every filter leaves the layer in inks; a line on K
// alone stays on K alone; what a filter does not reach keeps its samples (16 bits); a correction works the colours the
// inks mean and goes back by the rules (a grey on K alone, Genko's black, the table), keeping the inks of what it leaves
// as it was; a correction layer is drawn (the screen and the exports), not left out.

#include <QtTest>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/strokes.hpp"
#include "render/ink.hpp"
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

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const auto& layer : doc.page(0).layers) {
        if (layer.id == id) return layer;
    }
    throw core::Error("value", "no layer " + std::string(id));
}

core::InkPixels pixels_of(const core::Document& doc, const std::string& id) { return core::decode_ink_raster(*layer_of(doc, id).ink_raster); }

const std::uint16_t* at(const core::InkPixels& p, std::uint32_t x, std::uint32_t y) { return p.samples.data() + (std::size_t(y) * p.width + x) * p.channels(); }

// A raster pixel of a point (mm): paint layers are kept at the working 200 dpi.
std::uint32_t px(double mm) { return static_cast<std::uint32_t>(mm * 200 / 25.4); }

constexpr std::array<std::uint16_t, 4> kS{13107, 45875, 6554, 3277};  // (a colour of four inks)

// A CMYK page (16 bits; 60 × 80 mm) with a paint layer "pic" at 200 dpi: at the top left lines on K alone (K 100%, a
// stripe every 2 mm) over nothing, at the top right S, at the bottom left a grey on K alone (K 50%), the bottom right
// clear but for specks of K 100% (2 × 2 pixels).
core::Document filter_book(const std::string& mode = "cmyk") {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("ink", core::Num(1), 1, spec, core::Binding::Right);
    Json to = mode == "cmyk" ? Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", 16}}
                             : Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"depth", 16}};
    doc = run_ops(doc, Json::array({to, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
    const core::PageColor& color = *doc.page(0).color;
    const std::uint32_t w = static_cast<std::uint32_t>(render::mm_to_px(60, 200)), h = static_cast<std::uint32_t>(render::mm_to_px(80, 200));
    core::InkPixels p = core::blank_ink_pixels(color.space, 16, w, h);
    const unsigned n = core::colour_channels(color.space), all = n + 1;
    const auto set = [&](std::uint32_t x, std::uint32_t y, const std::vector<double>& ink) {
        std::uint16_t* s = p.samples.data() + (std::size_t(y) * w + x) * all;
        for (unsigned c = 0; c < n; ++c) s[c] = render::ink::encode_sample(color.space, c, ink[c], 16);
        s[n] = 65535;
    };
    const std::vector<double> black = color.space == core::InkSpace::Cmyk ? std::vector<double>{0, 0, 0, 1} : std::vector<double>{0, 0, 0};
    const std::vector<double> grey = color.space == core::InkSpace::Cmyk ? std::vector<double>{0, 0, 0, 0.5} : std::vector<double>{50, 0, 0};
    const std::vector<double> s_ink = color.space == core::InkSpace::Cmyk ? std::vector<double>{kS[0] / 65535.0, kS[1] / 65535.0, kS[2] / 65535.0, kS[3] / 65535.0}
                                                                         : std::vector<double>{60, 40, -30};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (y < px(40) && x < px(30)) {
                if ((x / px(1)) % 2 == 0) set(x, y, black);  // (stripes 1 mm wide)
            } else if (y < px(40)) {
                set(x, y, s_ink);
            } else if (x < px(30)) {
                set(x, y, grey);
            } else if ((x / 2) % 40 == 0 && (y / 2) % 40 == 0) {
                set(x, y, black);  // (specks of 2 × 2 pixels on what is clear)
            }
        }
    }
    for (auto& layer : doc.edit_page(0).layers) {
        if (layer.id != "pic") continue;
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
        layer.panel_clip = layer.panel_each = false;
    }
    return doc;
}

// The pixels of the lines' corner (away from S and the page's middle): what shows there is on K alone (Lab: a = b =
// 0), however the filter moved or blurred it.
void lines_stay_on_k(const core::InkPixels& p, const char* what) {
    const bool cmyk = p.space == core::InkSpace::Cmyk;
    const std::uint16_t neutral = render::ink::encode_sample(core::InkSpace::Lab, 1, 0.0, 16);
    int shown = 0;
    for (std::uint32_t y = px(2); y < px(28); ++y) {
        for (std::uint32_t x = px(2); x < px(18); ++x) {
            const std::uint16_t* s = at(p, x, y);
            if (s[p.channels() - 1] == 0) continue;
            ++shown;
            const bool alone = cmyk ? s[0] == 0 && s[1] == 0 && s[2] == 0 : s[1] == neutral && s[2] == neutral;
            if (!alone) QFAIL(qPrintable(QStringLiteral("%1: (%2, %3) is not on K alone").arg(QString::fromUtf8(what)).arg(x).arg(y)));
        }
    }
    QVERIFY2(shown > 1000, what);
}

// And for a filter that only moves, blurs or lights from what shows: the lines' pixels that still show (alpha at
// least half) are Genko's black as they were (K 100%; Lab: L 0) — no paper of no ink, no light from nothing.
void lines_stay_black(const core::InkPixels& p, const char* what) {
    const bool cmyk = p.space == core::InkSpace::Cmyk;
    const unsigned n = p.channels() - 1;
    for (std::uint32_t y = px(2); y < px(28); ++y) {
        for (std::uint32_t x = px(2); x < px(18); ++x) {
            const std::uint16_t* s = at(p, x, y);
            if (s[n] < 32768) continue;
            const bool black = cmyk ? s[3] == 65535 : s[0] == 0;
            if (!black) QFAIL(qPrintable(QStringLiteral("%1: (%2, %3) shows, not as Genko's black").arg(QString::fromUtf8(what)).arg(x).arg(y)));
        }
    }
}

}  // namespace

class TestInkFilters : public QObject {
    Q_OBJECT

private slots:
    // Every filter on a CMYK and a Lab page: the layer in inks, its lines on K alone (Lab: no colour).
    void filtersKeepLinesOnK_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("op");
        const QStringList ops{
            R"({"kind": "blur", "radius": 3})",
            R"({"kind": "sharpen"})",
            R"({"kind": "sharpen", "amount": 2})",
            R"({"kind": "mosaic", "block": 6})",
            R"({"kind": "motion_blur", "distance": 10, "angle": 30})",
            R"({"kind": "radial_blur", "amount": 0.05})",
            R"({"kind": "zoom_blur", "amount": 0.05})",
            R"({"kind": "wave", "amplitude": 4})",
            R"({"kind": "twirl", "angle": 60})",
            R"({"kind": "despeckle", "size_mm": 0.5})",
            R"({"kind": "glow", "threshold": 100})",
            R"({"kind": "noise", "amount": 0.3})",
            R"({"kind": "lineart"})",
            R"({"kind": "rain", "amount": 0.4})",
            R"({"kind": "levels", "black": 30, "white": 220, "gamma": 1.4})",
            R"({"kind": "curve", "points": [[0, 0], [128, 90], [255, 255]]})",
            R"({"kind": "invert"})",
            R"({"kind": "brightness_contrast", "brightness": 20, "contrast": 30})",
            R"({"kind": "posterize", "levels": 4})",
            R"({"kind": "threshold", "level": 100})",
            R"({"kind": "hue", "hue": 40, "saturation": 1.5})",
            R"({"kind": "exposure", "exposure": 1})",
        };
        for (const char* mode : {"cmyk", "lab"}) {
            for (const QString& op : ops) QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(QString::fromLatin1(mode), op))) << QString::fromLatin1(mode) << op;
        }
    }
    void filtersKeepLinesOnK() {
        QFETCH(QString, mode);
        QFETCH(QString, op);
        const core::Document doc = filter_book(mode.toStdString());
        Json filter = Json::parse(op.toStdString());
        filter["op"] = "filter_raster";
        filter["page"] = 1;
        filter["id"] = "pic";
        const core::Document after = run_ops(doc, Json::array({filter})).doc;
        const core::Layer& pic = layer_of(after, "pic");
        QVERIFY(!pic.raster_png && !pic.color_raster && pic.ink_raster);
        const core::InkPixels got = pixels_of(after, "pic");
        QCOMPARE(got.space, doc.page(0).color->space);
        QCOMPARE(got.depth, 16);
        QVERIFY(got.samples != pixels_of(doc, "pic").samples);
        lines_stay_on_k(got, qPrintable(op));
        const std::string kind = filter["kind"].get<std::string>();
        for (const char* moving : {"blur", "mosaic", "motion_blur", "radial_blur", "zoom_blur", "wave", "twirl", "despeckle", "glow"}) {
            if (kind == moving) lines_stay_black(got, qPrintable(op));
        }
        QCOMPARE(layer_of(after, "pic").kind, core::LayerKind::Raster);
    }

    // A blur keeps what it does not reach (16 bits) and a line's inks where it fades (weighted by alpha: no paper mixed
    // in); a filter with an area changes nothing outside it.
    void blurKeepsTheInks() {
        const core::Document doc = filter_book();
        const core::Document after =
            run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "blur"}, {"radius", 2}}})).doc;
        const core::InkPixels got = pixels_of(after, "pic");
        for (unsigned c = 0; c < 4; ++c) QVERIFY(std::abs(int(at(got, px(45), px(20))[c]) - int(kS[c])) <= 1);  // (S all round)
        QVERIFY(std::abs(int(at(got, px(15), px(60))[3]) - 32768) <= 1);                                       // (the grey)
        // the stripes' edges: K 100% with less alpha
        int faded = 0;
        for (std::uint32_t x = px(2); x < px(18); ++x) {
            const std::uint16_t* s = at(got, x, px(10));
            if (s[4] == 0 || s[4] == 65535) continue;
            ++faded;
            QCOMPARE(s[3], std::uint16_t(65535));
        }
        QVERIFY(faded > 4);
        // inside an area only
        const core::Document part = run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "invert"},
                                                                  {"area", Json{{"poly", Json::array({Json::array({35, 5}), Json::array({55, 5}),
                                                                                                       Json::array({55, 30}), Json::array({35, 30})})}}}}}))
                                        .doc;
        const core::InkPixels inside = pixels_of(part, "pic"), before = pixels_of(doc, "pic");
        for (unsigned c = 0; c < 5; ++c) QCOMPARE(at(inside, px(15), px(60))[c], at(before, px(15), px(60))[c]);
        QVERIFY(at(inside, px(45), px(20))[1] != kS[1]);
    }

    // The corrections: a grey on K alone stays on K alone (inverted: K 100% → no ink, K 50% → K 50%), Genko's black by
    // the rules, and one that leaves a colour as it was keeps its inks (16 bits).
    void correctionsByTheRules() {
        const core::Document doc = filter_book();
        const core::Document inverted =
            run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "invert"}}})).doc;
        const core::InkPixels got = pixels_of(inverted, "pic");
        const std::uint16_t* line = at(got, px(0.5), px(10));
        QVERIFY(line[0] == 0 && line[1] == 0 && line[2] == 0 && line[3] == 0 && line[4] == 65535);
        const std::uint16_t* grey = at(got, px(15), px(60));
        QVERIFY(grey[0] == 0 && grey[1] == 0 && grey[2] == 0 && std::abs(int(grey[3]) - 32768) <= 1);
        // levels that change nothing: every ink as it was
        const core::Document same = run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "levels"},
                                                                  {"black", 0}, {"white", 255}, {"gamma", 1.0}}}))
                                        .doc;
        QCOMPARE(*layer_of(same, "pic").ink_raster, *layer_of(doc, "pic").ink_raster);
    }

    // A correction layer is drawn on an ink page: nothing left out, an export written; inverting at half opacity takes a
    // line on K alone to K 50% alone; one that changes nothing leaves the page's inks as they were.
    void correctionLayersAreDrawn() {
        core::Document doc = filter_book();
        const int dpi = 100;
        render::RenderOptions o;
        o.inks = true;
        const core::InkPixels plain = *render::render_page(doc.page(0), dpi, o, &doc).inks;
        core::Document inverted = doc;
        {
            core::Layer layer;
            layer.id = "inv";
            layer.kind = core::LayerKind::Adjust;
            layer.adjust = Json{{"kind", "invert"}};
            layer.opacity = 0.5;
            inverted.edit_page(0).layers.push_back(std::move(layer));
        }
        const render::RenderResult made = render::render_page(inverted.page(0), dpi, o, &inverted);
        QVERIFY(made.omitted.empty());
        const auto page_px = [&](double mm) { return static_cast<std::uint32_t>(render::mm_to_px(mm, dpi)); };
        const std::uint16_t* line = at(*made.inks, page_px(0.5), page_px(10));
        QVERIFY(line[0] == 0 && line[1] == 0 && line[2] == 0 && std::abs(int(line[3]) - 32768) <= 2);
        core::Document same = doc;
        {
            core::Layer layer;
            layer.id = "lv";
            layer.kind = core::LayerKind::Adjust;
            layer.adjust = Json{{"kind", "levels"}, {"black", 0}, {"white", 255}, {"gamma", 1.0}};
            same.edit_page(0).layers.push_back(std::move(layer));
        }
        QVERIFY(render::render_page(same.page(0), dpi, o, &same).inks->samples == plain.samples);
        // a correction layer in a blend mode: not yet (left out and said, as on precise pixels)
        core::Document blended = inverted;
        blended.edit_page(0).layers.back().blend = "multiply";
        render::RenderOptions skip = o;
        skip.skip_unported = true;
        const auto omitted = render::render_page(blended.page(0), dpi, skip, &blended).omitted;
        QVERIFY(std::find(omitted.begin(), omitted.end(), "ink_adjust_blend:multiply") != omitted.end());
    }

    // A pen layer filtered on an ink page: its lines drawn into inks first (K alone), the layer a paint layer of inks.
    void penLinesBecomeInks() {
        core::Document doc = filter_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"},
                                             {"points", Json::array({Json::array({5, 50}), Json::array({25, 50})})}, {"width_mm", 3}},
                                        Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pen"}, {"kind", "blur"}, {"radius", 1}}}))
                  .doc;
        const core::Layer& pen = layer_of(doc, "pen");
        QVERIFY(pen.ink_raster && !pen.raster_png && pen.stroke_count() == 0);
        QCOMPARE(pen.kind, core::LayerKind::Raster);
        const core::InkPixels inks = pixels_of(doc, "pen");
        const std::uint16_t* s = at(inks, px(15), px(50));
        QVERIFY(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 65535 && s[4] == 65535);
    }

    // A layer whose alpha is locked (透明保護): its lines drawn into its inks only inside its pixels, as the page shows
    // them — a line over what is clear stays unseen.
    void lockedAlphaKeepsLinesInside() {
        core::Document doc = filter_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pic"},
                                             {"points", Json::array({Json::array({35, 60}), Json::array({55, 60})})}, {"width_mm", 3}}}))
                  .doc;
        for (auto& layer : doc.edit_page(0).layers) {
            if (layer.id == "pic") layer.lock_alpha = true;
        }
        const core::Document after = run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "blur"}, {"radius", 1}}})).doc;
        QCOMPARE(at(pixels_of(after, "pic"), px(45), px(60))[4], std::uint16_t(0));
    }

    // A layer with nothing to bake filtered: a paint layer after it, as the 8-bit filter leaves one.
    void filteredLayerIsPaint() {
        core::Document doc = filter_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "empty"}},
                                        Json{{"op", "filter_raster"}, {"page", 1}, {"id", "empty"}, {"kind", "blur"}}}))
                  .doc;
        QCOMPARE(layer_of(doc, "empty").kind, core::LayerKind::Raster);
        QVERIFY(layer_of(doc, "empty").ink_raster);
    }

    // Precise lines (color_rgb) on a pen layer of an ink page are drawn by the rules (their 8-bit colour): filtered or
    // merged, a line in Genko's black stays on K alone (never precise RGB pixels through the profile).
    void preciseLinesByTheRules() {
        core::Document doc = filter_book();
        const Json across = Json::array({Json::array({5, 50}), Json::array({25, 50})});
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}}})).doc;
        for (auto& layer : doc.edit_page(0).layers) {
            if (layer.id == "pen") layer.panel_clip = layer.panel_each = false;  // (precise lines: on a pen layer of the whole page)
        }
        doc = run_ops(doc, Json::array({Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pen"},
                                             {"items", Json{{"strokes", Json::array({Json{{"points", across}, {"width_mm", 3}, {"rgb", Json::array({20, 20, 20})},
                                                                                         {"color_rgb", Json{{"precision", "u16"}, {"values", Json::array({0.0785, 0.0785, 0.0785})}}}}})}}}}}))
                  .doc;
        QVERIFY(core::has_color_strokes(layer_of(doc, "pen")));
        const auto k_alone = [](const core::InkPixels& p, std::uint32_t x, std::uint32_t y) {
            const std::uint16_t* s = at(p, x, y);
            return s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 65535 && s[4] == 65535;
        };
        const core::Document blurred = run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pen"}, {"kind", "blur"}, {"radius", 1}}})).doc;
        QVERIFY(!layer_of(blurred, "pen").color_raster);
        QVERIFY(k_alone(pixels_of(blurred, "pen"), px(15), px(50)));
        const core::Document merged = run_ops(doc, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", "pen"}}})).doc;
        QVERIFY(!layer_of(merged, "pic").color_raster && !layer_of(merged, "pic").raster_png);
        QVERIFY(k_alone(pixels_of(merged, "pic"), px(15), px(50)));
        QVERIFY(std::find(merged.features.begin(), merged.features.end(), core::kColorRasterFeature) == merged.features.end());
    }
};

QTEST_GUILESS_MAIN(TestInkFilters)
#include "test_ink_filters.moc"
