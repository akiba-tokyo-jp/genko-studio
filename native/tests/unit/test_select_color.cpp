// PC3 B1 (SPEC.md COLOR-06「色で選ぶ」「クイックマスク」): choosing by colour as PhotoCraft's engine/selection_cmds.rs tests
// it (lines 353-541) — the magic wand's colour on a layer and on the page (contiguous or not, anti-aliased, joined to the
// selection; a precise page compared by its values), Color Range (one colour, several, the eyedroppers and the minus
// one, inverted, localized clusters, the colour families and the tones, the colours out of the proof's gamut as the
// gamut warning marks them, its refusals), Grow and Similar —, Select › Modify through the area language (expand,
// contract, border, smooth by the distance transform) and the op quick_mask (in and out, the selection it gives back,
// its refusals, masks@1 while it lives).

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/masks.hpp"
#include "render/channels.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/proof.hpp"
#include "render/raster.hpp"
#include "render/select_color.hpp"
#include "render/selection.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

constexpr double kInch = 25.4;  // (200 px at the working 200 dpi)
constexpr int kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A pixel's centre in mm.
double mm(int px) { return (px + 0.5) * kInch / kSide; }

core::Document blank() {
    auto doc = core::new_episode("色で選ぶ", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

using Colour = std::array<int, 4>;

// White, with two red squares (A on the left, B on the right), a blue square, a black and a mid-grey strip.
Colour colours(int x, int y) {
    const auto in = [&](int x0, int x1, int y0, int y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; };
    if (in(20, 60, 20, 60) || in(100, 140, 20, 60)) return {255, 0, 0, 255};
    if (in(20, 60, 100, 140)) return {0, 0, 255, 255};
    if (in(80, 100, 100, 140)) return {0, 0, 0, 255};
    if (in(120, 140, 100, 140)) return {128, 128, 128, 255};
    return {255, 255, 255, 255};
}

render::Image picture(const std::function<Colour(int, int)>& at) {
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            for (const int v : at(x, y)) bytes.push_back(static_cast<char>(v));
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Layer paint_layer(const core::Page& page, const std::string& id, const render::Image& image) {
    core::Layer layer;
    layer.id = id;
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(page, layer, image);
    return layer;
}

core::Document painted(const std::function<Colour(int, int)>& at = colours) {
    auto doc = blank();
    doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "paint", picture(at)));
    return doc;
}

render::Image mask(const core::Document& doc, const Json& area) { return render::selection::to_mask(area, doc.page(0), &doc, 200); }

int at(const render::Image& m, int x, int y) { return static_cast<int>(m.getpixel(x, y)[0]); }

Json point(int x, int y) { return Json::array({mm(x), mm(y)}); }

void refused(const core::Document& doc, const Json& area, const char* why) {
    try {
        (void)mask(doc, area);
    } catch (const core::OpError&) {
        return;
    }
    QFAIL(why);
}

}  // namespace

class TestSelectColor : public QObject {
    Q_OBJECT
private slots:
    // The wand's colour: the drawing layer's own colours or the page's; contiguous or anywhere; anti-aliased; joined.
    void theWandsColour() {
        const core::Document doc = painted();
        const core::Page& page = doc.page(0);
        Json spec{{"x_mm", mm(30)}, {"y_mm", mm(30)}, {"layer", "paint"}, {"contiguous", true}, {"antialias", false}};
        QVERIFY(render::select_color::own_colour(spec, page));
        QVERIFY(!render::select_color::own_colour(Json{{"x_mm", 1}, {"y_mm", 1}}, page));  // (an 8-bit page's own way, as always)
        render::Image m = mask(doc, Json{{"color", spec}});
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 59, 59), 255);
        QCOMPARE(at(m, 120, 40), 0);  // (B, apart)
        QCOMPARE(at(m, 70, 70), 0);   // (white)
        spec["contiguous"] = false;
        m = mask(doc, Json{{"color", spec}});
        QCOMPARE(at(m, 120, 40), 255);
        QCOMPARE(at(m, 40, 120), 0);  // (blue)
        spec.erase("layer");  // (the page as it shows)
        m = mask(doc, Json{{"color", spec}});
        QCOMPARE(at(m, 120, 40), 255);
        QCOMPARE(at(m, 70, 70), 0);
        // anti-aliased: the edge grey
        spec["contiguous"] = true;
        spec["antialias"] = true;
        m = mask(doc, Json{{"color", spec}});
        QCOMPARE(at(m, 40, 40), 255);
        QVERIFY2(at(m, 19, 40) > 0 && at(m, 19, 40) < 255, qPrintable(QString::number(at(m, 19, 40))));
        QVERIFY2(at(m, 20, 40) > 128 && at(m, 20, 40) < 255, qPrintable(QString::number(at(m, 20, 40))));
        QCOMPARE(at(m, 17, 40), 0);
        // joined to a selection: added, taken away
        spec["antialias"] = false;
        const Json a = *render::selection::combine(std::nullopt, Json{{"color", spec}}, "replace", page, &doc);
        Json b_spec = spec;
        b_spec["x_mm"] = mm(120);
        const auto both = render::selection::combine(a, Json{{"color", b_spec}}, "add", page, &doc);
        QVERIFY(both);
        m = mask(doc, *both);
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 120, 40), 255);
        const auto left = render::selection::combine(*both, Json{{"color", spec}}, "subtract", page, &doc);
        m = mask(doc, *left);
        QCOMPARE(at(m, 30, 30), 0);
        QCOMPARE(at(m, 120, 40), 255);
        // a point off the page, a layer the page has not
        refused(doc, Json{{"color", Json{{"x_mm", -1}, {"y_mm", 3}, {"layer", "paint"}}}}, "off the page");
        refused(doc, Json{{"color", Json{{"x_mm", 1}, {"y_mm", 3}, {"layer", "nothing"}}}}, "no such layer");
    }

    // A precise page compares its values (the tolerance in levels of 255): two colours one 8-bit level holds apart.
    void aPrecisePageByItsValues() {
        core::Document doc = run_ops(painted(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).doc;
        Json pixels = Json::array();
        for (int y = 0; y < kSide; ++y)
            for (int x = 0; x < kSide; ++x)
                for (const int v : {x < 100 ? 32768 : 32868, 32768, 32768, 65535}) pixels.push_back(v);
        for (core::Layer& layer : doc.edit_page(0).layers) {
            if (layer.id != "paint") continue;
            QVERIFY(layer.color_raster);
            layer.color_raster = std::make_shared<const std::string>(
                core::encode_color_raster(Json{{"width", kSide}, {"height", kSide}, {"precision", "u16"}, {"pixels", pixels}}));
        }
        const Json spec{{"x_mm", mm(10)}, {"y_mm", mm(10)}, {"tolerance", 0}};
        QVERIFY(render::select_color::own_colour(spec, doc.page(0)));
        render::Image m = mask(doc, Json{{"color", spec}});
        QCOMPARE(at(m, 50, 50), 255);
        QCOMPARE(at(m, 150, 50), 0);  // (0.39 of a level away)
        Json wider = spec;
        wider["tolerance"] = 1;
        m = mask(doc, Json{{"color", wider}});
        QCOMPARE(at(m, 150, 50), 255);
    }

    // Color Range: sampled colours (given, picked, taken away), inverted, localized; families and tones; refusals.
    void colorRange() {
        const core::Document doc = painted();
        render::Image m = mask(doc, Json{{"range", Json{{"colors", Json::array({Json::array({255, 0, 0})})}, {"fuzziness", 10}}}});
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 120, 40), 255);
        QCOMPARE(at(m, 70, 70), 0);
        QCOMPARE(at(m, 40, 120), 0);
        m = mask(doc, Json{{"range", Json{{"colors", Json::array({Json::array({255, 0, 0}), Json::array({0, 0, 255})})}, {"fuzziness", 10}}}});
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 40, 120), 255);
        QCOMPARE(at(m, 70, 70), 0);
        // the eyedropper on the blue square, inverted
        m = mask(doc, Json{{"range", Json{{"points_mm", Json::array({point(30, 110)})}, {"fuzziness", 10}, {"invert", true}}}});
        QCOMPARE(at(m, 40, 120), 0);
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 70, 70), 255);
        // the minus eyedropper takes the blue away from red and blue
        m = mask(doc, Json{{"range", Json{{"colors", Json::array({Json::array({255, 0, 0}), Json::array({0, 0, 255})})},
                                           {"subtract_points_mm", Json::array({point(30, 110)})},
                                           {"fuzziness", 10}}}});
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 40, 120), 0);
        // localized clusters: picked on A, a 15 % range (30 px) leaves B
        m = mask(doc, Json{{"range", Json{{"points_mm", Json::array({point(40, 40)})}, {"fuzziness", 10}, {"localized", true}, {"range", 15}}}});
        QCOMPARE(at(m, 40, 40), 255);
        QVERIFY(at(m, 50, 40) > 128);
        QCOMPARE(at(m, 120, 40), 0);
        // the families
        m = mask(doc, Json{{"range", Json{{"select", "reds"}}}});
        QCOMPARE(at(m, 30, 30), 255);
        QCOMPARE(at(m, 120, 40), 255);
        QCOMPARE(at(m, 40, 120), 0);
        QCOMPARE(at(m, 70, 70), 0);
        QCOMPARE(at(m, 130, 120), 0);
        m = mask(doc, Json{{"range", Json{{"select", "blues"}}}});
        QCOMPARE(at(m, 40, 120), 255);
        QCOMPARE(at(m, 30, 30), 0);
        m = mask(doc, Json{{"range", Json{{"select", "greens"}}}});
        QCOMPARE(at(m, 30, 30), 0);
        // the tones: black a shadow, white a highlight, 128 a midtone
        m = mask(doc, Json{{"range", Json{{"select", "shadows"}}}});
        QCOMPARE(at(m, 90, 120), 255);
        QCOMPARE(at(m, 70, 70), 0);
        QCOMPARE(at(m, 130, 120), 0);
        m = mask(doc, Json{{"range", Json{{"select", "highlights"}}}});
        QCOMPARE(at(m, 90, 120), 0);
        QCOMPARE(at(m, 70, 70), 255);
        m = mask(doc, Json{{"range", Json{{"select", "midtones"}, {"fuzziness", 0}}}});
        QCOMPARE(at(m, 130, 120), 255);
        QCOMPARE(at(m, 70, 70), 0);
        QCOMPARE(at(m, 90, 120), 0);
        m = mask(doc, Json{{"range", Json{{"select", "shadows"}, {"tonal_range", 140}, {"fuzziness", 0}}}});
        QCOMPARE(at(m, 130, 120), 255);
        // a layer's own colours
        m = mask(doc, Json{{"range", Json{{"select", "reds"}, {"layer", "paint"}}}});
        QCOMPARE(at(m, 30, 30), 255);
        // refusals
        for (const Json& bad : {Json{{"select", "skin"}}, Json{{"select", 3}}, Json{{"points_mm", Json::array({Json::array({100, 2})})}},
                                Json{{"points_mm", Json::array({Json::array({1})})}}, Json{{"points_mm", "here"}}, Json{{"colors", "#ff0000"}},
                                Json{{"localized", true}, {"colors", Json::array({Json::array({1, 2, 3})})}},
                                Json{{"select", "midtones"}, {"tonal_range", Json::array({200, 100})}}, Json{{"select", "shadows"}, {"tonal_range", 300}},
                                Json::object(), Json{{"colors", Json::array({Json::array({300, 0, 0})})}}, Json{{"what", 1}}}) {
            refused(doc, Json{{"range", bad}}, bad.dump().c_str());
        }
    }

    // Out of Gamut: the colours the proof profile cannot hold, as 色域外警告 marks them.
    void outOfGamutAsTheWarning() {
        const core::Document doc = painted([](int x, int) { return x < 100 ? Colour{0, 0, 255, 255} : Colour{128, 128, 128, 255}; });
        const Json area{{"range", Json{{"select", "out_of_gamut"}, {"profile", "coated-cmyk"}}}};
        const render::Image m = mask(doc, area);
        QCOMPARE(at(m, 50, 100), 255);
        QCOMPARE(at(m, 150, 100), 0);
        render::proof::Setup setup;
        setup.icc = core::builtin_profile("coated-cmyk")->bytes;
        const render::RenderResult drawn = render::render_page(doc.page(0), 200, render::proof_options(), &doc);
        std::size_t warned = 0;
        (void)render::proof::Gamut::of(setup)->mask(drawn.image, render::proof::kDefaultThreshold, &warned);
        std::size_t chosen = 0;
        for (const char v : m.tobytes()) chosen += static_cast<unsigned char>(v) > 127 ? 1 : 0;
        QCOMPARE(chosen, warned);
        refused(doc, Json{{"range", Json{{"select", "out_of_gamut"}, {"profile", "no-such"}}}}, "an unknown profile");
    }

    // Grow (joined) and Similar (anywhere): the selection's colours widened by the tolerance.
    void growAndSimilar() {
        const core::Document doc = painted();
        const Json inside{{"rect", Json::array({mm(36), mm(36), 8 * kInch / kSide, 8 * kInch / kSide})}};
        render::Image m = mask(doc, Json{{"grow", Json{{"area", inside}, {"tolerance", 5}}}});
        QCOMPARE(at(m, 22, 22), 255);
        QCOMPARE(at(m, 58, 58), 255);
        QCOMPARE(at(m, 120, 40), 0);
        QCOMPARE(at(m, 70, 70), 0);
        m = mask(doc, Json{{"similar", Json{{"area", inside}, {"tolerance", 5}, {"layer", "paint"}}}});
        QCOMPARE(at(m, 120, 40), 255);
        QCOMPARE(at(m, 70, 70), 0);
        QCOMPARE(at(m, 40, 120), 0);
        refused(doc, Json{{"grow", Json{{"area", Json{{"rect", Json::array({-20, -20, 1, 1})}}}}}}, "an empty area");
        refused(doc, Json{{"grow", Json{{"tolerance", 5}}}}, "no area");
        refused(doc, Json{{"similar", Json{{"area", inside}, {"radius", 3}}}}, "a key it does not have");
        // through the bus, as an op's area
        const core::Document filled = run_ops(doc, Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint"},
                                                                     {"area", Json{{"similar", Json{{"area", inside}}}}}, {"rgb", Json::array({0, 255, 0})}}}))
                                          .doc;
        QVERIFY(!filled.page(0).layers.back().patches.empty());
    }

    // Select › Modify in the area language: expand, contract (rounded, the distance transform), border, smooth.
    void modifyByDistance() {
        const core::Document doc = painted();
        const double px = kInch / kSide;
        const Json rect{{"rect", Json::array({50 * px, 50 * px, 50 * px, 50 * px})}};
        Json area = rect;
        area["expand_mm"] = 2 * px;
        render::Image m = mask(doc, area);
        QCOMPARE(at(m, 48, 75), 255);
        QCOMPARE(at(m, 46, 75), 0);
        QVERIFY(at(m, 48, 48) < 255);  // (the corner rounded)
        area = rect;
        area["contract_mm"] = 4 * px;
        m = mask(doc, area);
        QCOMPARE(at(m, 52, 75), 0);
        QCOMPARE(at(m, 75, 75), 255);
        area = rect;
        area["border_mm"] = 4 * px;
        m = mask(doc, area);
        QCOMPARE(at(m, 75, 75), 0);
        QCOMPARE(at(m, 50, 75), 255);
        area = rect;
        area["smooth_mm"] = 2 * px;
        m = mask(doc, area);
        QCOMPARE(at(m, 75, 75), 255);
        QCOMPARE(at(m, 40, 75), 0);
    }

    // quick_mask: in (white where the selection was; none: all white), out (the selection it says; all white: none),
    // its refusals; masks@1 while it lives; a copy of it is an ordinary alpha channel.
    void quickMaskInAndOut() {
        const core::Document doc = painted();
        const Json rect{{"rect", Json::array({mm(50), mm(50), 50 * kInch / kSide, 50 * kInch / kSide})}};
        core::ApplyResult in = run_ops(doc, Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"area", rect}}}));
        const std::string id = in.results.at(0)["channel"].get<std::string>();
        const core::Channel* quick = core::quick_mask_of(in.doc.page(0));
        QVERIFY(quick != nullptr);
        QCOMPARE(quick->id, id);
        QCOMPARE(quick->name, std::string("クイックマスク"));
        QVERIFY(quick->kind == core::ChannelKind::Alpha);
        QCOMPARE(quick->color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(quick->opacity, 0.5);
        QCOMPARE(quick->indicates, std::string("masked"));
        const render::Image density = render::channels::mask_of(*quick, render::Size{kSide, kSide});
        QCOMPARE(at(density, 75, 75), 255);
        QCOMPARE(at(density, 20, 20), 0);
        QVERIFY(core::uses_masks(in.doc));
        core::validate_masks(in.doc);
        QVERIFY(refusal(in.doc, Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}}).find("Quick Mask already") != std::string::npos);
        // a copy of it is not a second Quick Mask
        const core::Document copied = run_ops(in.doc, Json::array({Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", id}}})).doc;
        QCOMPARE(copied.page(0).channels.size(), std::size_t{2});
        QVERIFY(!copied.page(0).channels[1].quick_mask);
        core::validate_masks(copied);
        // out: the selection back, the channel gone
        const core::ApplyResult out = run_ops(in.doc, Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", false}}}));
        QVERIFY(out.doc.page(0).channels.empty());
        QVERIFY(!core::uses_masks(out.doc));
        const Json back = out.results.at(0)["area"];
        QVERIFY(back.is_object() && back.contains("mask"));
        const render::Image was = mask(doc, rect), now = mask(doc, back);
        int worst = 0;
        const std::string a = was.tobytes(), b = now.tobytes();
        for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(static_cast<unsigned char>(a[i]) - static_cast<unsigned char>(b[i])));
        QVERIFY2(worst <= 2, qPrintable(QString::number(worst)));
        QVERIFY(refusal(out.doc, Json{{"op", "quick_mask"}, {"page", 1}, {"on", false}}).find("not in Quick Mask") != std::string::npos);
        // without a selection: all white, and out again no selection
        const core::ApplyResult white = run_ops(doc, Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"color", Json::array({0, 0, 255})},
                                                                       {"opacity", 0.3}, {"indicates", "selected"}}}));
        const core::Channel& all = *core::quick_mask_of(white.doc.page(0));
        QCOMPARE(at(render::channels::mask_of(all, render::Size{kSide, kSide}), 20, 20), 255);
        QCOMPARE(all.color, (std::array<int, 3>{0, 0, 255}));
        QCOMPARE(all.opacity, 0.3);
        QCOMPARE(all.indicates, std::string("selected"));
        const core::ApplyResult none = run_ops(white.doc, Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", false}}}));
        QVERIFY(none.results.at(0)["area"].is_null());
        // refusals: on missing, wrong values, entering's keys on the way out
        for (const Json& bad : {Json{{"op", "quick_mask"}, {"page", 1}}, Json{{"op", "quick_mask"}, {"page", 1}, {"on", "yes"}},
                                Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"opacity", 2}},
                                Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"indicates", "both"}},
                                Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"color", Json::array({1, 2})}},
                                Json{{"op", "quick_mask"}, {"page", 1}, {"on", false}, {"opacity", 0.5}}}) {
            QVERIFY2(refusal(white.doc, bad).starts_with("apply: ") && refusal(doc, bad).starts_with("apply: "), bad.dump().c_str());
        }
    }
};

QTEST_MAIN(TestSelectColor)
#include "test_select_color.moc"
