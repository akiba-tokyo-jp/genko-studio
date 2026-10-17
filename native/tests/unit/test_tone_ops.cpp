// PC2c (局所の補正, SPEC.md COLOR-04): the local corrections and HDR toning without Python — PhotoCraft's own tests of
// algo/tone.rs (366-445: the shadows lifted, dark more than light, nothing at zero amounts; the highlights darkened where
// light only; Replace Color on the matching pixels only; Match Color moving the statistics, neutralize to grey; HDR
// toning within 0..1, an inverted curve turning the order round) and the box Gaussian, Color Range and Lab; then the
// ops: filter_raster's shadows_highlights, replace_color and match_color on an 8-bit layer (within the area), a
// precise one (the 8-bit result within a level), a CMYK one (a grey kept on K) and an alpha channel, match_color's sources (a layer, the
// page merged, statistics given, the target's in the area) and its refusals, hdr_toning flattening the page (the count
// merged and discarded; the panels kept).

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "render/channels.hpp"
#include "render/image_ops.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::image_ops::Rgba;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
constexpr std::uint32_t kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::ApplyResult run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})); }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(bool frames = false) {
    auto doc = core::new_episode("局所", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    if (!frames) p.frames.clear();
    p.layers.clear();
    return doc;
}

// An opaque picture: a dark ramp on the left, light on the right, a red block and a blue block.
std::array<int, 4> colours(std::uint32_t x, std::uint32_t y) {
    if (x >= 120 && x < 150 && y >= 20 && y < 50) return {220, 30, 30, 255};
    if (x >= 160 && x < 190 && y >= 20 && y < 50) return {30, 40, 210, 255};
    const int v = static_cast<int>(x * 255 / (kSide - 1));
    return {v, static_cast<int>(v * 0.8), std::min(255, static_cast<int>(v * 0.6) + 25 + static_cast<int>(y % 7)), 255};
}

render::Image picture(const std::function<std::array<int, 4>(std::uint32_t, std::uint32_t)>& at = colours) {
    std::string bytes;
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x)
            for (const int v : at(x, y)) bytes.push_back(static_cast<char>(v));
    return render::Image::frombytes("RGBA", render::Size{int(kSide), int(kSide)}, bytes);
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

core::Document eight_bit_page() {
    auto doc = blank();
    doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "paint", picture()));
    return doc;
}

core::Document precise_page() {
    auto doc = blank();
    Json pixels = Json::array();
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x)
            for (const int v : colours(x, y)) pixels.push_back(static_cast<std::int64_t>(v) * 257);
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", kSide}, {"height", kSide},
                                                          {"precision", "u16"}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

// A CMYK page (16 bits) with a paint layer: a grey on K alone (lighter downwards) on the left, a colour of four inks on
// the right.
core::Document cmyk_page() {
    auto doc = run_ops(blank(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}},
                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, 16, kSide, kSide);
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const std::vector<double> ink = x < kSide / 2 ? std::vector<double>{0, 0, 0, 0.95 - 0.6 * y / kSide} : std::vector<double>{0.2, 0.7, 0.1, 0.05};
            for (unsigned c = 0; c < 4; ++c) p.set(std::size_t(y) * kSide + x, c, ink[c]);
            p.set(std::size_t(y) * kSide + x, 4, 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const char* id) {
    for (const auto& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", std::string("no layer ") + id);
}

std::string bytes_of(const core::Layer& layer) { return render::open_image(*layer.raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(); }
int at(const std::string& b, std::uint32_t x, std::uint32_t y, unsigned c) { return static_cast<unsigned char>(b[(std::size_t(y) * kSide + x) * 4 + c]); }

std::vector<Rgba> rgba_of_bytes(const std::string& b) {
    std::vector<Rgba> out(b.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) out[i][c] = static_cast<unsigned char>(b[i * 4 + c]) / 255.0f;
    return out;
}

float luma(const Rgba& p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

// PhotoCraft's test picture: a ramp of w × h.
std::vector<Rgba> ramp(std::size_t w, std::size_t h) {
    std::vector<Rgba> out;
    for (std::size_t i = 0; i < w * h; ++i) {
        const float x = static_cast<float>(i % w) / static_cast<float>(w - 1);
        out.push_back({x, x * 0.8f, x * 0.6f + 0.1f, 1.0f});
    }
    return out;
}

}  // namespace

class TestToneOps : public QObject {
    Q_OBJECT

private slots:
    // --- the algorithms (PhotoCraft's tests of algo/tone.rs) ------------------------------------------------------------

    void shadowsLiftDarkPixelsMoreThanBright() {
        using render::image_ops::ShadowsHighlights;
        auto px = ramp(32, 8);
        const auto orig = px;
        ShadowsHighlights p;
        p.black_clip = p.white_clip = 0;
        render::image_ops::shadows_highlights(px, 32, 8, p);
        const auto gain = [&](std::size_t i) { return luma(px[i]) - luma(orig[i]); };
        QVERIFY2(gain(3) > 0.02f, qPrintable(QString::number(gain(3))));
        QVERIFY(gain(3) > gain(30));
        // zero amounts and no clipping: as it was
        auto same = orig;
        ShadowsHighlights id;
        id.shadow_amount = id.highlight_amount = id.color = 0;
        id.black_clip = id.white_clip = 0;
        render::image_ops::shadows_highlights(same, 32, 8, id);
        QVERIFY(same == orig);
    }

    void highlightsDarkenBrightPixels() {
        auto px = ramp(32, 8);
        const auto orig = px;
        render::image_ops::ShadowsHighlights p;
        p.shadow_amount = 0;
        p.highlight_amount = 80;
        p.black_clip = p.white_clip = 0;
        render::image_ops::shadows_highlights(px, 32, 8, p);
        QVERIFY(px[30][0] < orig[30][0]);
        QVERIFY(std::fabs(px[1][0] - orig[1][0]) < 1e-6f);
    }

    void replaceColorOnlyTouchesMatchingPixels() {
        std::vector<Rgba> px{{1, 0, 0, 1}, {0, 0, 1, 1}};
        const auto mask = render::image_ops::replace_color(px, {1, 0, 0}, 40, 120, 0, 0);
        QCOMPARE(mask[1], 0.0f);
        QVERIFY(px[0][1] > 0.9f && px[0][0] < 0.1f);
        QVERIFY((px[1] == Rgba{0, 0, 1, 1}));
        // Color Range: the coverage falls off with the largest channel difference (levels) to fuzziness + 1
        const auto range = render::image_ops::color_range({{0.5f, 0.5f, 0.5f, 1}, {0.5f + 20 / 255.0f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 0}}, {0.5f, 0.5f, 0.5f}, 39);
        QCOMPARE(range[0], 1.0f);
        QVERIFY(std::fabs(range[1] - 0.5f) < 1e-4f);
        QCOMPARE(range[2], 0.0f);
    }

    void matchColorMovesStatisticsTowardTheSource() {
        using render::image_ops::lab_stats;
        std::vector<Rgba> src, tgt;
        for (int i = 0; i < 64; ++i) {
            src.push_back({0.8f, 0.4f + static_cast<float>(i % 8) * 0.02f, 0.2f, 1});
            tgt.push_back({0.2f, 0.3f, 0.6f + static_cast<float>(i % 8) * 0.03f, 1});
        }
        const auto s = *lab_stats(src);
        const auto t = *lab_stats(tgt);
        render::image_ops::match_color(tgt, t, &s, {});
        const auto after = *lab_stats(tgt);
        for (unsigned k = 0; k < 3; ++k) QVERIFY(std::fabs(after.mean[k] - s.mean[k]) < std::fabs(t.mean[k] - s.mean[k]) * 0.2f + 1.0f);
        // neutralize with no source: the mean colour goes grey
        std::vector<Rgba> tint;
        for (int i = 0; i < 16; ++i) tint.push_back({0.7f, 0.5f, 0.3f + static_cast<float>(i) * 0.01f, 1});
        const auto own = *lab_stats(tint);
        render::image_ops::MatchColor o;
        o.neutralize = true;
        render::image_ops::match_color(tint, own, nullptr, o);
        const auto n = *lab_stats(tint);
        QVERIFY(std::fabs(n.mean[1]) < 2.0f && std::fabs(n.mean[2]) < 2.0f);
        // nothing opaque: no statistics
        QVERIFY(!lab_stats({{0.5f, 0.5f, 0.5f, 0}}));
    }

    void hdrToningCompressesTheRangeWithinBounds() {
        using render::image_ops::HdrToning;
        auto px = ramp(40, 10);
        HdrToning p;
        p.strength = 2;
        p.detail = 0;
        p.saturation = 0;
        render::image_ops::hdr_toning(px, 40, 10, p);
        QVERIFY(luma(px[38]) > luma(px[2]));
        for (const Rgba& q : px)
            for (unsigned c = 0; c < 3; ++c) QVERIFY(q[c] >= 0 && q[c] <= 1);
        // a toning curve turning the luminance round turns the order round
        auto turned = ramp(40, 10);
        HdrToning q;
        q.strength = 0;
        q.detail = 0;
        q.saturation = 0;
        q.curve = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        render::image_ops::hdr_toning(turned, 40, 10, q);
        QVERIFY(luma(turned[2]) > luma(turned[38]));
    }

    // gauss_blur_n: Kovesi's box radii, a flat field kept, the sum kept away from the edges, nothing below sigma 0.3.
    void gaussianOfBoxes() {
        QCOMPARE(render::image_ops::gauss_box_radii(1.0f), (std::array<std::size_t, 3>{0, 0, 1}));
        QCOMPARE(render::image_ops::gauss_box_radii(15.0f), (std::array<std::size_t, 3>{14, 14, 15}));
        std::vector<float> flat(30 * 20, 0.25f);
        render::image_ops::gauss_blur(flat, 30, 20, 1, 4.0f);
        for (const float v : flat) QVERIFY(std::fabs(v - 0.25f) < 1e-6f);
        std::vector<float> dot(41 * 41, 0.0f);
        dot[20 * 41 + 20] = 1.0f;
        render::image_ops::gauss_blur(dot, 41, 41, 1, 3.0f);
        double sum = 0;
        for (const float v : dot) sum += v;
        QVERIFY(std::fabs(sum - 1.0) < 1e-4);
        QVERIFY(dot[20 * 41 + 20] < 0.1f && dot[20 * 41 + 20] > dot[20 * 41 + 25] && dot[20 * 41 + 25] > 0);
        std::vector<float> kept{0.1f, 0.9f, 0.3f};
        render::image_ops::gauss_blur(kept, 3, 1, 1, 0.2f);
        QCOMPARE(kept, (std::vector<float>{0.1f, 0.9f, 0.3f}));
    }

    // Lab (color/convert.rs): white is L 100, a grey has no a or b, there and back within a step.
    void labOfSrgb() {
        const auto white = render::image_ops::srgb_to_lab({1, 1, 1});
        QVERIFY(std::fabs(white[0] - 100) < 0.05f && std::fabs(white[1]) < 0.05f && std::fabs(white[2]) < 0.05f);
        const auto grey = render::image_ops::srgb_to_lab({0.5f, 0.5f, 0.5f});
        QVERIFY(std::fabs(grey[1]) < 0.05f && std::fabs(grey[2]) < 0.05f);
        for (const std::array<float, 3> c : {std::array<float, 3>{0.2f, 0.6f, 0.9f}, std::array<float, 3>{0.9f, 0.1f, 0.3f}}) {
            const auto back = render::image_ops::lab_to_srgb(render::image_ops::srgb_to_lab(c));
            for (unsigned k = 0; k < 3; ++k) QVERIFY(std::fabs(back[k] - c[k]) < 1e-3f);
        }
        const auto hsl = render::image_ops::rgb_to_hsl({1, 0, 0});
        QVERIFY(std::fabs(hsl[0]) < 1e-6f && std::fabs(hsl[1] - 1) < 1e-6f && std::fabs(hsl[2] - 0.5f) < 1e-6f);
        const auto green = render::image_ops::hsl_to_rgb({1.0f / 3.0f, 1, 0.5f});
        QVERIFY(std::fabs(green[1] - 1) < 1e-5f && green[0] < 1e-5f && green[2] < 1e-5f);
    }

    // --- filter_raster's local corrections --------------------------------------------------------------------------

    // shadows_highlights on an 8-bit layer: the dark ramp lifted, the light end kept near; only inside the area when
    // there is one; the same on a precise layer within a level, its precision kept.
    void shadowsHighlightsOnEachKindOfPixels() {
        const auto doc = eight_bit_page();
        const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "shadows_highlights"}, {"shadow_amount", 60}, {"black_clip", 0}, {"white_clip", 0}};
        const auto made = run(doc, op).doc;
        const std::string before = bytes_of(layer_of(doc, "paint")), after = bytes_of(layer_of(made, "paint"));
        QVERIFY(at(after, 20, 100, 0) > at(before, 20, 100, 0) + 5);  // (a dark neighbourhood lifted)
        QVERIFY(std::abs(at(after, 190, 100, 0) - at(before, 190, 100, 0)) < 12);
        // within the area only
        Json inside = op;
        inside["area"] = Json{{"poly", Json::array({Json::array({0, 0}), Json::array({8, 0}), Json::array({8, 25.4}), Json::array({0, 25.4})})}};
        const std::string some = bytes_of(layer_of(run(doc, inside).doc, "paint"));
        QVERIFY(at(some, 20, 100, 0) > at(before, 20, 100, 0));
        QCOMPARE(at(some, 100, 100, 0), at(before, 100, 100, 0));
        // a precise layer: within a level of the 8-bit result, still precise
        const auto precise = precise_page();
        const auto worked = run(precise, op).doc;
        const core::Layer& layer = layer_of(worked, "paint");
        QVERIFY(layer.color_raster);
        const core::ColorRasterView view(*layer.color_raster);
        QCOMPARE(view.metadata("")["precision"], Json("u16"));
        double worst = 0;
        for (std::uint32_t y = 0; y < kSide; y += 7)
            for (std::uint32_t x = 0; x < kSide; x += 3)
                for (unsigned c = 0; c < 4; ++c)
                    worst = std::max(worst, std::fabs(view.pixel(std::size_t(y) * kSide + x)[c] - at(after, x, y, c) / 255.0));
        QVERIFY2(worst <= 1.0 / 255 + 1e-9, qPrintable(QString::number(worst * 255)));
    }

    // On a CMYK page the colours the inks mean are worked and go back by the page's rules: a grey stays on K alone.
    void shadowsHighlightsOnInks() {
        const auto doc = cmyk_page();
        const auto made = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "shadows_highlights"}, {"shadow_amount", 80}}).doc;
        const core::InkPixels was = core::decode_ink_raster(*layer_of(doc, "pic").ink_raster);
        const core::InkPixels now = core::decode_ink_raster(*layer_of(made, "pic").ink_raster);
        bool lighter = false;
        for (std::uint32_t y = 0; y < kSide; y += 5) {
            const std::size_t i = std::size_t(y) * kSide + 10;
            for (unsigned c = 0; c < 3; ++c) QCOMPARE(now.value(i, c), 0.0);  // (K alone)
            lighter = lighter || now.value(i, 3) < was.value(i, 3) - 0.02;
        }
        QVERIFY(lighter);
    }

    // replace_color: the colour's pixels shifted in hue, the others as they were (their very bytes).
    void replaceColorOnALayer() {
        const auto doc = eight_bit_page();
        const auto made = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "replace_color"}, {"color", Json::array({220, 30, 30})},
                                        {"fuzziness", 10}, {"hue", 120}}).doc;
        const std::string before = bytes_of(layer_of(doc, "paint")), after = bytes_of(layer_of(made, "paint"));
        QVERIFY(at(after, 130, 30, 1) > 150 && at(after, 130, 30, 0) < 80);  // (red to green)
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(at(after, 170, 30, c), at(before, 170, 30, c));  // (the blue block)
        QCOMPARE(at(after, 10, 150, 0), at(before, 10, 150, 0));
        // the colour is required
        QVERIFY(refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "replace_color"}}).starts_with("apply: "));
        // on an alpha channel (PC1c's "channel": lent as a grey paint layer): its white made grey
        const auto chan = run(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "α"}, {"fill", "white"}}).doc;
        const auto worked = run(chan, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "replace_color"}, {"channel", "α"},
                                           {"color", Json::array({255, 255, 255})}, {"lightness", -50}}).doc;
        const render::channels::Plane plane = render::channels::plane_of(worked.page(0), worked.page(0).channels[0]);
        QVERIFY(std::fabs(plane.values[plane.values.size() / 2] - 0.5f) < 0.02f);
        QCOMPARE(*layer_of(worked, "paint").raster_png, *layer_of(doc, "paint").raster_png);
    }

    // match_color: a layer's colours to another layer's statistics (a flat colour: every pixel to it), to the page
    // merged, to statistics given (another book's, the window worked them out), neutralized; the target's statistics
    // inside the area (stats_in_area) on 8 bits as on precise pixels; what it refuses.
    void matchColorSources() {
        auto doc = eight_bit_page();
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "src", picture([](std::uint32_t, std::uint32_t) { return std::array<int, 4>{200, 120, 60, 255}; })));
        doc.edit_page(0).layers.back().visible = false;
        const auto to = [&](const Json& extra) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "match_color"}};
            for (const auto& [key, value] : extra.items()) op[key] = value;
            return bytes_of(layer_of(run(doc, op).doc, "paint"));
        };
        const std::string flat = to(Json{{"source", Json{{"layer", "src"}}}});
        QVERIFY(std::abs(at(flat, 30, 100, 0) - 200) <= 2 && std::abs(at(flat, 30, 100, 1) - 120) <= 2 && std::abs(at(flat, 30, 100, 2) - 60) <= 2);
        // statistics given: the same as the layer's
        const auto s = *render::image_ops::lab_stats(rgba_of_bytes(bytes_of(layer_of(doc, "src"))));
        const std::string given = to(Json{{"source_stats", Json{{"mean", Json::array({s.mean[0], s.mean[1], s.mean[2]})}, {"std", Json::array({0, 0, 0})}}}});
        QVERIFY(std::abs(at(given, 30, 100, 0) - at(flat, 30, 100, 0)) <= 1);
        // the page merged (the hidden layer not in it): toward the picture's own statistics — little moves
        const std::string merged = to(Json{{"source", Json{{"layer", "merged"}}}});
        QVERIFY(std::abs(at(merged, 100, 100, 1) - at(bytes_of(layer_of(doc, "paint")), 100, 100, 1)) < 25);
        // neutralized: the mean colour grey
        const auto neutral = *render::image_ops::lab_stats(rgba_of_bytes(to(Json{{"neutralize", true}})));
        QVERIFY(std::fabs(neutral.mean[1]) < 2.5f && std::fabs(neutral.mean[2]) < 2.5f);
        // the target's statistics in the area: another result than the whole layer's
        const Json area{{"poly", Json::array({Json::array({15, 0}), Json::array({25.4, 0}), Json::array({25.4, 25.4}), Json::array({15, 25.4})})}};
        const std::string whole = to(Json{{"source", Json{{"layer", "merged"}}}, {"luminance", 150}, {"area", area}});
        const std::string inside = to(Json{{"source", Json{{"layer", "merged"}}}, {"luminance", 150}, {"area", area}, {"stats_in_area", true}});
        QVERIFY(whole != inside);
        QCOMPARE(at(inside, 10, 100, 0), at(bytes_of(layer_of(doc, "paint")), 10, 100, 0));  // (outside the area: as it was)
        // the same on precise pixels: the statistics in the area there too
        const auto precise = precise_page();
        const auto worked = run(precise, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "match_color"}, {"neutralize", true},
                                              {"area", area}, {"stats_in_area", true}}).doc;
        QVERIFY(layer_of(worked, "paint").color_raster != layer_of(precise, "paint").color_raster);
        // refused: both sources, statistics not of their form, a source layer that is not there
        const auto refuses = [&](const Json& extra) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "match_color"}};
            for (const auto& [key, value] : extra.items()) op[key] = value;
            const std::string why = refusal(doc, op);
            QVERIFY2(why.starts_with("apply: "), why.c_str());
        };
        refuses(Json{{"source", Json{{"layer", "src"}}}, {"source_stats", Json{{"mean", Json::array({50, 0, 0})}, {"std", Json::array({1, 1, 1})}}}});
        refuses(Json{{"source_stats", Json{{"mean", Json::array({50, 0})}, {"std", Json::array({1, 1, 1})}}}});
        refuses(Json{{"source_stats", Json{{"mean", Json::array({50, 0, 0})}}}});
        refuses(Json{{"source", Json{{"layer", "nothing"}}}});
        refuses(Json{{"source", Json{{"layer", "src"}, {"what", 1}}}});
        refuses(Json{{"luminance", "a"}});
    }

    // --- hdr_toning ---------------------------------------------------------------------------------------------------

    // The page's pixel layers flattened into one (over the paper), its hidden layers discarded, its panels kept; the
    // result toned; the counts reported. Nothing showing: refused.
    void hdrToningFlattensThePage() {
        auto doc = blank(true);
        const std::size_t frames = doc.page(0).frames.size();
        QVERIFY(frames > 0);
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "a", picture()));
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "b", picture([](std::uint32_t x, std::uint32_t y) {
            return x > 100 && y > 100 ? std::array<int, 4>{30, 200, 90, 255} : std::array<int, 4>{0, 0, 0, 0};
        })));
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "c", picture()));
        doc.edit_page(0).layers.back().visible = false;
        const auto made = run(doc, Json{{"op", "hdr_toning"}, {"page", 1}, {"strength", 2}, {"detail", 0}});
        const Json& report = made.results.at(0);
        QCOMPARE(report["flattened"], Json(2));
        QCOMPARE(report["discarded"], Json(1));
        const std::string flattened = report["layer"].get<std::string>();
        const core::Page& page = made.doc.page(0);
        QCOMPARE(page.frames.size(), frames);
        int paint = 0;
        for (const auto& layer : page.layers) paint += layer.raster_png ? 1 : 0;
        QCOMPARE(paint, 1);
        const std::string toned = bytes_of(layer_of(made.doc, flattened.c_str()));
        QVERIFY(at(toned, 5, 100, 0) > 5);  // (the dark end lifted by the compression)
        QVERIFY(at(toned, 150, 150, 1) > at(toned, 150, 150, 0));  // (the green square from the layer on top)
        // the same on a CMYK page: the inks of the flattened layer
        const auto inks = run(cmyk_page(), Json{{"op", "hdr_toning"}, {"page", 1}});
        QVERIFY(inks.doc.page(0).layers.size() <= cmyk_page().page(0).layers.size());
        QVERIFY(refusal(blank(), Json{{"op", "hdr_toning"}, {"page", 1}}).starts_with("apply: "));
        QVERIFY(refusal(doc, Json{{"op", "hdr_toning"}, {"page", 1}, {"curve", Json::array({Json::array({0, 255})})}}).starts_with("apply: "));
        QVERIFY(refusal(doc, Json{{"op", "hdr_toning"}, {"page", 1}, {"radius", "big"}}).starts_with("apply: "));
    }
};

QTEST_GUILESS_MAIN(TestToneOps)
#include "test_tone_ops.moc"
