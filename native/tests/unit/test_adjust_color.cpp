// PC2b (色の補正, SPEC.md COLOR-04): the colour corrections — hue_saturation, vibrance, color_balance, black_white,
// photo_filter, channel_mixer, selective_color (correction layers and filter_raster) and desaturate (filter_raster) —:
// their formulas (PhotoCraft v0.2.0's own tests: compose/src/adjust.rs 618-635, 699-721; doc/src/adjust.rs 268-285),
// their settings read as PhotoCraft reads them (engine/src/adjust_params.rs 565-620: clamped, refused, the normal form
// round trips), stored under adjustments@1, drawn the same on the three ways (the 8-bit page: a layer as the filter;
// 16 bits within a level of it; a CMYK, Lab or grey page through the colours its inks mean), and the PSD's adjustment
// layers hue2, vibA, blnc, blwh, phfl, mixr, selc (io/src/adjust_map.rs 745-1013: round trips, the hand-built blocks,
// what is not read).

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/adjust_color.hpp"
#include "core/adjust_kinds.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/psd_adjust.hpp"
#include "render/adjust.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/psd_adjust.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace color = core::color_adjust;
using Rgb = color::Rgb;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
constexpr std::uint32_t kSide = 200;

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

core::Document blank() {
    auto doc = core::new_episode("色の補正", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// An opaque picture of colours: hues round the circle across, lighter down; a grey square and a white one.
std::array<int, 4> colours(std::uint32_t x, std::uint32_t y) {
    const double h = x / double(kSide), l = 0.15 + 0.7 * y / double(kSide);
    const Rgb c = color::hsl_to_rgb(h, 0.2 + 0.8 * ((x * 7 + y) % 13) / 12.0, l);
    std::array<int, 4> v{int(std::lround(c[0] * 255)), int(std::lround(c[1] * 255)), int(std::lround(c[2] * 255)), 255};
    if (x > 20 && x < 50 && y > 20 && y < 50) v = {128, 128, 128, 255};
    if (x > 150 && x < 170 && y > 150 && y < 170) v = {255, 255, 255, 255};
    return v;
}

render::Image picture() {
    std::string bytes;
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x)
            for (const int v : colours(x, y)) bytes.push_back(static_cast<char>(v));
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

std::string icc_bytes() { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))); }

// A CMYK page (16 bits) with a paint layer of inks: a grey on K alone and a colour of four inks, in halves.
core::Document cmyk_page() {
    auto doc = blank();
    doc = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", 16}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}}})).doc;
    const core::PageColor& c = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(c.space, 16, kSide, kSide);
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const std::vector<double> ink = x < kSide / 2 ? std::vector<double>{0, 0, 0, 0.3 + 0.4 * y / kSide} : std::vector<double>{0.2, 0.7, 0.1, 0.05};
            for (unsigned k = 0; k < 4; ++k) p.set(std::size_t(y) * kSide + x, k, ink[k]);
            p.set(std::size_t(y) * kSide + x, 4, 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

// A Lab page (16 bits) with a paint layer of a red (L 50, a 60, b 40) — normalized as stored: L/100, (a + 128)/255.
core::Document lab_page() {
    auto doc = blank();
    doc = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"depth", 16}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}}})).doc;
    core::InkPixels p = core::blank_ink_pixels(core::InkSpace::Lab, 16, kSide, kSide);
    for (std::size_t i = 0; i < p.pixel_count(); ++i) {
        p.set(i, 0, 0.5);
        p.set(i, 1, (60 + 128) / 255.0);
        p.set(i, 2, (40 + 128) / 255.0);
        p.set(i, 3, 1.0);
    }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

core::Document with_correction(core::Document doc, const Json& adjust, double opacity = 1.0) {
    core::Layer layer;
    layer.id = "adj";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Adjust;
    layer.adjust = adjust;
    layer.opacity = opacity;
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

std::string shown(const core::Document& doc) { return render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.tobytes(); }

core::InkPixels inks_shown(const core::Document& doc) {
    render::RenderOptions o = render::proof_options();
    o.inks = true;
    return *render::render_page(doc.page(0), 200, o, &doc).inks;
}

int worst_difference(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return 256;
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i]))));
    return worst;
}

const core::Layer& correction(const core::Document& doc) {
    for (const auto& layer : doc.page(0).layers)
        if (layer.kind == core::LayerKind::Adjust) return layer;
    throw core::Error("value", "no correction layer");
}

Json payload_of(const core::Document& doc, const QTemporaryDir& tmp) {
    storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
    return storage::project_payload_v4(doc, assets);
}

bool names(const Json& payload, std::string_view feature) {
    for (const Json& f : payload["features"])
        if (f == Json(std::string(feature))) return true;
    return false;
}

// A correction of `adjust` (its kind among them) on one colour.
Rgb through(const Json& adjust, const Rgb& c) {
    const core::Document doc = blank();
    const core::AdjustPtr f = adjust.value("kind", std::string()) == "desaturate"
                                  ? core::filter_function("desaturate", Json::object(), doc.page(0))
                                  : core::adjust_function(adjust, doc.page(0));
    return f->apply(c, 0, 0);
}

bool near(const Rgb& a, const Rgb& b, double within) {
    return std::abs(a[0] - b[0]) <= within && std::abs(a[1] - b[1]) <= within && std::abs(a[2] - b[2]) <= within;
}

double saturation_of(const Rgb& c) { return color::rgb_to_hsl(c)[1]; }

// The settings of each kind that change something (whole numbers: a PSD holds them as they are).
std::vector<Json> samples() {
    return {
        Json{{"kind", "hue_saturation"}, {"hue", 25}, {"saturation", -20}, {"lightness", 10},
             {"reds", Json{{"hue", 10}, {"saturation", -30}, {"lightness", 5}, {"range", Json::array({300, 340, 20, 50})}}}},
        Json{{"kind", "hue_saturation"}, {"hue", 200}, {"saturation", 50}, {"colorize", true}},
        Json{{"kind", "vibrance"}, {"vibrance", 30}, {"saturation", -20}},
        Json{{"kind", "color_balance"}, {"shadows", Json::array({10, -5, 0})}, {"midtones", Json::array({0, 20, -30})}, {"highlights", Json::array({5, 5, 5})},
             {"preserve_luminosity", false}},
        Json{{"kind", "black_white"}, {"reds", 120}, {"blues", -40}, {"tint", true}, {"tint_color", Json::array({208, 160, 112})}},
        Json{{"kind", "photo_filter"}, {"color", Json::array({51, 102, 204})}, {"density", 40}, {"preserve_luminosity", false}},
        Json{{"kind", "channel_mixer"}, {"red", Json::array({80, 30, -10, 5})}, {"blue", Json::array({0, 10, 90, 0})}},
        Json{{"kind", "channel_mixer"}, {"gray", Json::array({30, 59, 11, 0})}, {"monochrome", true}},
        Json{{"kind", "selective_color"}, {"method", "absolute"}, {"reds", Json::array({10, 0, -20, 5})}, {"blacks", Json::array({0, 0, 0, 25})}},
    };
}

std::string be(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) {
        out += static_cast<char>((static_cast<unsigned>(v) >> 8) & 0xff);
        out += static_cast<char>(static_cast<unsigned>(v) & 0xff);
    }
    return out;
}
std::string be32(std::uint32_t v) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xff);
    return out;
}

// A versioned descriptor as PhotoCraft writes one (an empty name of no characters, the class "null"): `items` already
// made (key, type, value bytes).
std::string descriptor(const std::vector<std::string>& items, std::string_view class_id = "null") {
    std::string out = be32(16) + be32(0) + be32(0) + std::string(class_id) + be32(static_cast<std::uint32_t>(items.size()));
    for (const std::string& item : items) out += item;
    return out;
}
std::string key_of(std::string_view key) { return (key.size() == 4 ? be32(0) : be32(static_cast<std::uint32_t>(key.size()))) + std::string(key); }
std::string long_item(std::string_view key, std::int32_t v) { return key_of(key) + "long" + be32(static_cast<std::uint32_t>(v)); }
std::string bool_item(std::string_view key, bool v) { return key_of(key) + "bool" + std::string(1, v ? '\1' : '\0'); }
std::string double_bytes(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return be32(static_cast<std::uint32_t>(bits >> 32)) + be32(static_cast<std::uint32_t>(bits & 0xffffffffULL));
}
std::string rgbc_item(std::string_view key, double r, double g, double b) {
    std::string body = be32(0) + be32(0) + "RGBC" + be32(3);
    body += key_of("Rd  ") + "doub" + double_bytes(r);
    body += key_of("Grn ") + "doub" + double_bytes(g);
    body += key_of("Bl  ") + "doub" + double_bytes(b);
    return key_of(key) + "Objc" + body;
}

}  // namespace

class TestAdjustColor : public QObject {
    Q_OBJECT

private slots:
    // The hue ranges' weights (doc/src/adjust.rs: hue_range_weights) and their sliders in their canonical form.
    void hueRanges() {
        const color::HueRange reds = color::neutral_range(0);
        QCOMPARE(reds.weight(0), 1.0);
        QCOMPARE(reds.weight(350), 1.0);
        QVERIFY(std::abs(reds.weight(30) - 0.5) < 1e-9);
        QVERIFY(std::abs(reds.weight(330) - 0.5) < 1e-9);
        QCOMPARE(reds.weight(60), 0.0);
        QCOMPARE(reds.weight(180), 0.0);
        const color::HueRange blues = color::neutral_range(4);
        QCOMPARE(blues.weight(240), 1.0);
        QCOMPARE(blues.weight(120), 0.0);
        color::HueRange odd = color::neutral_range(0);
        odd.bounds = {10, 10, 10, 10};
        QVERIFY(std::isfinite(odd.weight(10)) && std::isfinite(odd.weight(200)));
        odd.bounds = {NAN, NAN, NAN, NAN};
        QVERIFY(std::isfinite(odd.weight(10)));
        // canonical: the reds whichever way they were written; sliders kept in order within a turn (hue_range_sliders_stay_ordered)
        QCOMPARE(color::canonical_bounds({315, 345, 15, 45}), (std::array<double, 4>{-45, -15, 15, 45}));
        QCOMPARE(color::canonical_bounds({-45, -15, 15, 45}), (std::array<double, 4>{-45, -15, 15, 45}));
        const auto b = color::canonical_bounds({100, 90, 200, 150});
        QVERIFY(b[0] <= b[1] && b[1] <= b[2] && b[2] <= b[3] && b[3] - b[0] < 360);
        for (int i = 0; i < 6; ++i) QCOMPARE(color::canonical_bounds(color::neutral_range(i).bounds), color::neutral_range(i).bounds);
    }

    // A range touches only its colours (adjust.rs: hue_ranges_only_touch_their_colours); the master's hue, saturation,
    // lightness and colorize; settings that change nothing leave every value — an HDR one too.
    void hueSaturation() {
        const Json reds{{"kind", "hue_saturation"}, {"reds", Json{{"saturation", -100}}}};
        const Rgb red = through(reds, {0.9, 0.1, 0.1});
        QVERIFY(std::abs(red[0] - red[1]) < 0.05);
        const Rgb blue = through(reds, {0.1, 0.1, 0.9});
        QVERIFY(std::abs(blue[2] - 0.9) < 1e-4 && std::abs(blue[0] - 0.1) < 1e-4);
        QCOMPARE(through(reds, {0.5, 0.5, 0.5}), (Rgb{0.5, 0.5, 0.5}));
        // a hue shift of 120°: red to green
        QVERIFY(near(through(Json{{"kind", "hue_saturation"}, {"hue", 120}}, {1, 0, 0}), {0, 1, 0}, 1e-9));
        // saturation −100: the HSL lightness (desaturate's); lightness ±100: white, black
        QVERIFY(near(through(Json{{"kind", "hue_saturation"}, {"saturation", -100}}, {0.8, 0.2, 0.4}), {0.5, 0.5, 0.5}, 1e-9));
        QVERIFY(near(through(Json{{"kind", "desaturate"}}, {0.8, 0.2, 0.4}), {0.5, 0.5, 0.5}, 1e-9));
        QVERIFY(near(through(Json{{"kind", "hue_saturation"}, {"lightness", 100}}, {0.3, 0.6, 0.2}), {1, 1, 1}, 1e-9));
        QVERIFY(near(through(Json{{"kind", "hue_saturation"}, {"lightness", -100}}, {0.3, 0.6, 0.2}), {0, 0, 0}, 1e-9));
        // colorize: the hue set (240° blue), the saturation at least ¼
        const Rgb tinted = through(Json{{"kind", "hue_saturation"}, {"colorize", true}, {"hue", 240}, {"saturation", 50}}, {0.5, 0.5, 0.5});
        QVERIFY(tinted[2] > tinted[0] && std::abs(tinted[0] - tinted[1]) < 1e-9);
        QVERIFY(std::abs(saturation_of(tinted) - 0.5) < 1e-9);
        const Rgb faint = through(Json{{"kind", "hue_saturation"}, {"colorize", true}, {"hue", 0}, {"saturation", 0}}, {0.5, 0.5, 0.5});
        QVERIFY(std::abs(saturation_of(faint) - 0.25) < 1e-9);
        // nothing changed: every value as it was (HDR too)
        for (const char* kind : {"hue_saturation", "vibrance", "color_balance", "channel_mixer", "selective_color"}) {
            QCOMPARE(through(Json{{"kind", kind}}, {2.0, -0.1, 0.5}), (Rgb{2.0, -0.1, 0.5}));
        }
    }

    // Black & White's weights (adjust.rs: black_white_weights) and its tint.
    void blackWhite() {
        const std::array<double, 6> d{40, 60, 40, 60, 20, 80};
        QVERIFY(std::abs(color::black_white_grey({0.5, 0.5, 0.5}, d) - 0.5) < 1e-9);
        QVERIFY(std::abs(color::black_white_grey({1, 0, 0}, d) - 0.4) < 1e-9);
        QVERIFY(std::abs(color::black_white_grey({1, 1, 0}, d) - 0.6) < 1e-9);
        std::array<double, 6> w = d;
        w[0] = 100;
        QVERIFY(color::black_white_grey({1, 0, 0}, w) > color::black_white_grey({1, 0, 0}, d));
        QVERIFY(near(through(Json{{"kind", "black_white"}}, {1, 0, 0}), {0.4, 0.4, 0.4}, 1e-9));
        // the tint: (g·t·2 held to 0..1)·½ + g·½
        const Rgb tinted = through(Json{{"kind", "black_white"}, {"tint", true}, {"tint_color", Json::array({255, 0, 0})}}, {0.5, 0.5, 0.5});
        QVERIFY(near(tinted, {0.75, 0.25, 0.25}, 1e-9));
    }

    // Selective Color (adjust.rs: selective_color_zero_is_identity_and_ranges_are_local): 0 changes nothing, a range
    // touches only its colours, relative never tints white (absolute does).
    void selectiveColor() {
        const std::array<std::array<double, 4>, 9> zero{};
        for (const Rgb& c : {Rgb{0.9, 0.1, 0.1}, Rgb{0.5, 0.5, 0.5}, Rgb{0.2, 0.4, 0.8}}) QCOMPARE(color::selective_color(c, true, zero), c);
        auto a = zero;
        a[0][0] = 100;
        QVERIFY(color::selective_color({0.9, 0.1, 0.1}, false, a)[0] < 0.2);
        QCOMPARE(color::selective_color({0.1, 0.1, 0.9}, false, a), (Rgb{0.1, 0.1, 0.9}));
        auto w = zero;
        w[6] = {0, 0, 50, 0};
        QCOMPARE(color::selective_color({1, 1, 1}, true, w), (Rgb{1, 1, 1}));
        QVERIFY(color::selective_color({1, 1, 1}, false, w)[2] < 0.6);
        // through the kind: reds' cyan +100 absolute
        QVERIFY(through(Json{{"kind", "selective_color"}, {"method", "absolute"}, {"reds", Json::array({100, 0, 0, 0})}}, {0.9, 0.1, 0.1})[0] < 0.2);
    }

    // Vibrance moves the duller colour more and leaves a grey; Color Balance and Photo Filter keep the luminosity when
    // asked; Channel Mixer's monochrome starts from 40/40/20.
    void theOtherFormulas() {
        const Json vibrance{{"kind", "vibrance"}, {"vibrance", 60}};
        const Rgb dull{0.65, 0.35, 0.35}, vivid{0.95, 0.05, 0.05};  // (saturation 0.3 and 0.9)
        QVERIFY(saturation_of(through(vibrance, dull)) - saturation_of(dull) > saturation_of(through(vibrance, vivid)) - saturation_of(vivid));
        QCOMPARE(through(vibrance, {0.4, 0.4, 0.4}), (Rgb{0.4, 0.4, 0.4}));  // (a grey has no hue: left)
        QVERIFY(saturation_of(through(Json{{"kind", "vibrance"}, {"saturation", -100}}, dull)) < 1e-9);
        // color balance: the midtones' red at a mid grey moves red by half; with the luminosity kept its grey stays
        const Rgb balanced = through(Json{{"kind", "color_balance"}, {"midtones", Json::array({100, 0, 0})}, {"preserve_luminosity", false}}, {0.5, 0.5, 0.5});
        QVERIFY(near(balanced, {1.0, 0.5, 0.5}, 1e-9));
        const Rgb kept = through(Json{{"kind", "color_balance"}, {"midtones", Json::array({40, -20, 10})}}, {0.4, 0.5, 0.3});
        QVERIFY(std::abs(color::grey_of(kept) - color::grey_of({0.4, 0.5, 0.3})) < 1e-9);
        // shadows touch only the dark
        QCOMPARE(through(Json{{"kind", "color_balance"}, {"shadows", Json::array({100, 0, 0})}, {"preserve_luminosity", false}}, {0.9, 0.9, 0.9}),
                 (Rgb{0.9, 0.9, 0.9}));
        // photo filter: a cool preset makes blue the most; the luminosity kept
        const Rgb cooled = through(Json{{"kind", "photo_filter"}, {"filter", "cooling80"}, {"density", 60}}, {0.6, 0.6, 0.6});
        QVERIFY(cooled[2] > cooled[1] && cooled[1] > cooled[0]);
        QVERIFY(std::abs(color::grey_of(cooled) - 0.6) < 1e-9);
        // channel mixer: monochrome's grey row (40/40/20 by default) on all three; a constant adds
        QVERIFY(near(through(Json{{"kind", "channel_mixer"}, {"monochrome", true}}, {1, 0, 0}), {0.4, 0.4, 0.4}, 1e-9));
        QVERIFY(near(through(Json{{"kind", "channel_mixer"}, {"red", Json::array({0, 100, 0, 10})}}, {0.2, 0.5, 0.7}), {0.6, 0.5, 0.7}, 1e-9));
    }

    // Settings read as PhotoCraft reads them (adjust_params.rs: params_reach_the_model, bad_params_are_errors_not_panics,
    // every_kind_round_trips_through_params): each a correction layer's normal form, clamped, refused, round tripped.
    void settingsAsPhotoCraftReadsThem() {
        const auto doc = eight_bit_page();
        const auto made = [&](const Json& adjust) {
            return *correction(run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", adjust}})).adjust;
        };
        Json a = made(Json{{"kind", "color_balance"}, {"midtones", Json::array({50, 0, -200})}});
        QCOMPARE(a, (Json{{"kind", "color_balance"}, {"shadows", Json::array({0, 0, 0})}, {"midtones", Json::array({50, 0, -100})},
                          {"highlights", Json::array({0, 0, 0})}, {"preserve_luminosity", true}}));
        a = made(Json{{"kind", "channel_mixer"}, {"monochrome", true}});
        QCOMPARE(a["gray"], Json::array({40, 40, 20, 0}));
        a = made(Json{{"kind", "photo_filter"}, {"filter", "cooling80"}, {"density", 60}});
        QCOMPARE(a, (Json{{"kind", "photo_filter"}, {"color", Json::array({0, 109, 255})}, {"density", 60}, {"preserve_luminosity", true}}));
        a = made(Json{{"kind", "black_white"}, {"reds", 120}, {"tint", true}});
        QCOMPARE(a["reds"], Json(120));
        QCOMPARE(a["yellows"], Json(60));
        QCOMPARE(a["tint_color"], Json::array({225, 211, 179}));
        a = made(Json{{"kind", "hue_saturation"}, {"blues", Json{{"hue", 40}}}});
        QCOMPARE(a["blues"], (Json{{"hue", 40}, {"saturation", 0}, {"lightness", 0}, {"range", Json::array({195, 225, 255, 285})}}));
        QCOMPARE(a["reds"]["range"], Json::array({-45, -15, 15, 45}));
        // out of range: clamped (a shift −180..180; colorize: 0..360 — an angle below 0 turned once — and saturation 0..100)
        a = made(Json{{"kind", "hue_saturation"}, {"hue", 1e9}, {"saturation", -1e9}});
        QCOMPARE(a["hue"], Json(180));
        QCOMPARE(a["saturation"], Json(-100));
        a = made(Json{{"kind", "hue_saturation"}, {"hue", -30}, {"saturation", -50}, {"colorize", true}});
        QCOMPARE(a["hue"], Json(330));
        QCOMPARE(a["saturation"], Json(0));
        a = made(Json{{"kind", "hue_saturation"}, {"greens", Json{{"range", Json::array({100, 90, 200, 150})}}}});
        const Json& g = a["greens"]["range"];
        QVERIFY(g[0] <= g[1] && g[1] <= g[2] && g[2] <= g[3] && g[3].get<double>() - g[0].get<double>() < 360);
        // refused: the op's error (apply), nothing changed
        for (const Json& bad : {Json{{"kind", "hue_saturation"}, {"reds", Json{{"range", Json::array({1, 2})}}}}, Json{{"kind", "hue_saturation"}, {"colorize", 1}},
                                Json{{"kind", "hue_saturation"}, {"reds", 4}}, Json{{"kind", "color_balance"}, {"shadows", Json::array({1, 2})}},
                                Json{{"kind", "black_white"}, {"tint_color", "#zz0000"}}, Json{{"kind", "black_white"}, {"tint", "yes"}},
                                Json{{"kind", "photo_filter"}, {"filter", "nope"}}, Json{{"kind", "photo_filter"}, {"filter", 3}},
                                Json{{"kind", "photo_filter"}, {"color", Json::array({1})}}, Json{{"kind", "channel_mixer"}, {"red", Json::array({1, 2, 3})}},
                                Json{{"kind", "channel_mixer"}, {"gray", Json::array({1, 2})}}, Json{{"kind", "selective_color"}, {"method", "both"}},
                                Json{{"kind", "vibrance"}, {"vibrance", "x"}}, Json{{"kind", "vibrance"}, {"amount", 3}}}) {
            const std::string why = refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", bad}});
            QVERIFY2(why.starts_with("apply: "), (bad.dump() + " → " + why).c_str());
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
            for (const auto& [key, value] : bad.items()) op[key] = value;
            QVERIFY2(refusal(doc, op).starts_with("apply: "), op.dump().c_str());
        }
        QVERIFY(refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "desaturate"}, {"amount", 1}}).starts_with("apply: "));
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", "desaturate"}}}}).starts_with("apply: "));
        // the normal form round trips: given again, the same; every kind's sample changes something
        for (const Json& sample : samples()) {
            const Json once = made(sample);
            QCOMPARE(made(once), once);
            Json neutral{{"kind", sample["kind"]}};
            QVERIFY2(made(neutral) != once, sample.dump().c_str());
        }
    }

    // Stored in their normal form under adjustments@1, read again as they were; out of their ranges a book opens read-only.
    void storedUnderAdjustments() {
        QTemporaryDir tmp;
        auto doc = eight_bit_page();
        for (const Json& sample : samples()) doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", sample}});
        const Json payload = payload_of(doc, tmp);
        QVERIFY(names(payload, core::kAdjustmentsFeature));
        const auto loaded = storage::load_document_payload(payload, std::filesystem::path(tmp.path().toStdString()));
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QCOMPARE(loaded.document.page(0).layers.size(), doc.page(0).layers.size());
        for (std::size_t i = 0; i < doc.page(0).layers.size(); ++i) {
            if (doc.page(0).layers[i].adjust) QCOMPARE(*loaded.document.page(0).layers[i].adjust, *doc.page(0).layers[i].adjust);
        }
        QVERIFY(shown(loaded.document) == shown(doc));
        for (const Json& stored : {Json{{"kind", "hue_saturation"}, {"saturation", 300}}, Json{{"kind", "selective_color"}, {"reds", Json::array({0, 0, 0, 200})}},
                                   Json{{"kind", "photo_filter"}, {"color", Json::array({0, 0, 300})}}, Json{{"kind", "black_white"}, {"tint", 1}}}) {
            Json bad = payload;
            bad["pages"][0]["layers"].back()["adjust"] = stored;
            QVERIFY2(!storage::load_document_payload(bad, std::filesystem::path(tmp.path().toStdString())).document.read_only_reason.empty(), stored.dump().c_str());
        }
    }

    // The 8-bit page: a correction layer over an opaque picture is the filter on the picture (the same function, rounded
    // to 8 bits), at any opacity the layer's mix of the two; 16 bits within a level of the 8-bit page.
    void eightAndSixteenBits() {
        const auto eight = eight_bit_page();
        const auto precise = precise_page();
        const render::Image base = render::render_page(eight.page(0), 200, render::proof_options(), &eight).image;
        for (Json sample : samples()) {
            const core::AdjustPtr f = core::adjust_function(sample, eight.page(0));
            QVERIFY(f);
            // the layer: the function on what is under it, rounded to 8 bits
            const std::string layered = shown(with_correction(eight, sample));
            QVERIFY2(layered == render::adjust::apply_8bit(base, *f).convert("RGB").tobytes(), sample.dump().c_str());
            QVERIFY2(layered != base.tobytes(), sample.dump().c_str());
            // the filter: the same function on the layer's pixels
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
            for (const auto& [key, value] : sample.items()) op[key] = value;
            const auto filtered_doc = run(eight, op);
            QVERIFY2(render::raster::layer_pixels(filtered_doc.page(0), filtered_doc.page(0).layers[0]).tobytes() ==
                         render::adjust::apply_8bit(picture(), *f).tobytes(),
                     sample.dump().c_str());
            const std::string filtered = shown(filtered_doc);
            // 16 bits: within a level of the 8-bit page, as a layer and as a filter
            const int worst = worst_difference(shown(with_correction(precise, sample)), layered);
            QVERIFY2(worst <= 1, qPrintable(QString::fromStdString(sample.dump()) + QString(" off by %1").arg(worst)));
            const int filter_worst = worst_difference(shown(run(precise, op)), filtered);
            QVERIFY2(filter_worst <= 1, qPrintable(QString::fromStdString(sample.dump()) + QString(" (filter) off by %1").arg(filter_worst)));
            // at half its opacity: between the picture and the corrected one
            const std::string half = shown(with_correction(eight, sample, 0.5));
            const std::string under = base.tobytes();
            for (std::size_t i = 0; i < half.size(); i += 997) {
                const int lo = std::min<int>(static_cast<unsigned char>(under[i]), static_cast<unsigned char>(layered[i]));
                const int hi = std::max<int>(static_cast<unsigned char>(under[i]), static_cast<unsigned char>(layered[i]));
                QVERIFY(static_cast<unsigned char>(half[i]) >= lo - 1 && static_cast<unsigned char>(half[i]) <= hi + 1);
            }
        }
        // desaturate (filter_raster only): every pixel a grey
        const std::string grey = shown(run(eight, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "desaturate"}}));
        for (std::size_t i = 0; i < grey.size(); i += 3) QVERIFY(grey[i] == grey[i + 1] && grey[i] == grey[i + 2]);
        QVERIFY(worst_difference(shown(run(precise, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "desaturate"}})), grey) <= 1);
        // inside the selection only
        const auto inside = run(eight, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "desaturate"},
                                            {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({10, 0}), Json::array({10, 10}), Json::array({0, 10})})}}}});
        const std::string part = shown(inside);
        QVERIFY(part != grey && part != shown(eight));
    }

    // A CMYK page works the colours its inks mean and takes them back by its rules: Black & White's greys go on K alone;
    // a Lab page's greys have a = b = 0; a grey page stays grey.
    void inkPages() {
        const auto cmyk = cmyk_page();
        for (const core::InkPixels& out : {inks_shown(with_correction(cmyk, Json{{"kind", "black_white"}})),
                                           inks_shown(run(cmyk, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "desaturate"}}))}) {
            for (std::size_t i = 0; i < out.pixel_count(); i += 7)
                QVERIFY2(out.value(i, 0) == 0 && out.value(i, 1) == 0 && out.value(i, 2) == 0, qPrintable(QString("pixel %1").arg(i)));
        }
        // a colour correction moves the colour half and leaves the greys of K alone
        const core::InkPixels before = inks_shown(cmyk);
        const core::InkPixels hued = inks_shown(with_correction(cmyk, Json{{"kind", "hue_saturation"}, {"hue", 120}}));
        const std::size_t grey_px = 50 * kSide + 20, colour_px = 50 * kSide + 150;
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(hued.value(grey_px, c), before.value(grey_px, c));
        QVERIFY(std::abs(hued.value(colour_px, 1) - before.value(colour_px, 1)) > 0.1);
        // Lab
        const core::InkPixels lab = inks_shown(with_correction(lab_page(), Json{{"kind", "black_white"}}));
        for (std::size_t i = 0; i < lab.pixel_count(); i += 101) {
            QVERIFY(std::abs(lab.value(i, 1) - 128 / 255.0) < 0.002 && std::abs(lab.value(i, 2) - 128 / 255.0) < 0.002);
        }
        QVERIFY(refusal(lab_page(), Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "selective_color"}, {"reds", Json::array({0, 50, 0, 0})}})
                    .empty());
        // a grey page: a colorize is a grey's lightness
        const auto grey = run(eight_bit_page(), Json{{"op", "set_color_mode"}, {"mode", "gray"}, {"depth", 16}});
        const core::InkPixels toned = inks_shown(with_correction(grey, Json{{"kind", "hue_saturation"}, {"colorize", true}, {"hue", 30}, {"saturation", 80}}));
        QCOMPARE(toned.colours(), 1u);
    }

    // The PSD's adjustment layers: each kind written and read back the same (adjust_map.rs: roundtrips); a correction
    // layer is laid in the PSD as its block.
    void psdRoundTrips() {
        const auto doc = eight_bit_page();
        std::vector<std::string> notes;
        std::vector<Json> all = samples();
        all.push_back(Json{{"kind", "hue_saturation"}, {"hue", -10},
                           {"reds", Json{{"hue", 15}, {"saturation", -40}, {"lightness", 10}, {"range", Json::array({-60, -20, 10, 30})}}},
                           {"blues", Json{{"saturation", 25}}}});
        all.push_back(Json{{"kind", "selective_color"}, {"reds", Json::array({-40, 5, -100, 100})}, {"whites", Json::array({0, 0, 50, 0})}});
        all.push_back(Json{{"kind", "black_white"}, {"reds", -200}, {"yellows", 300}, {"greens", 0}, {"cyans", 10}, {"blues", 90}, {"magentas", 30}});
        all.push_back(Json{{"kind", "photo_filter"}, {"color", Json::array({0, 156, 255})}, {"density", 100}, {"preserve_luminosity", false}});
        all.push_back(Json{{"kind", "channel_mixer"}, {"red", Json::array({50, 30, 20, 0})}, {"green", Json::array({10, 80, 10, 5})},
                           {"blue", Json::array({0, 20, 90, -5})}});
        for (const Json& sample : all) {
            const Json normal = core::normalize_adjust(sample, &doc.page(0));
            const auto block = render::psd::write_adjustment(normal, doc.page(0), notes);
            QVERIFY2(block, sample.dump().c_str());
            const auto back = render::psd::read_adjustment(block->key, block->payload, notes);
            QVERIFY2(back, sample.dump().c_str());
            QCOMPARE(*back, normal);
        }
        QVERIFY2(notes.empty(), notes.empty() ? "" : notes.front().c_str());
        // the keys
        const auto key_of_kind = [&](const char* kind) {
            return render::psd::write_adjustment(core::normalize_adjust(Json{{"kind", kind}}, &doc.page(0)), doc.page(0), notes)->key;
        };
        QCOMPARE(key_of_kind("hue_saturation"), std::string("hue2"));
        QCOMPARE(key_of_kind("vibrance"), std::string("vibA"));
        QCOMPARE(key_of_kind("color_balance"), std::string("blnc"));
        QCOMPARE(key_of_kind("black_white"), std::string("blwh"));
        QCOMPARE(key_of_kind("photo_filter"), std::string("phfl"));
        QCOMPARE(key_of_kind("channel_mixer"), std::string("mixr"));
        QCOMPARE(key_of_kind("selective_color"), std::string("selc"));
        // a correction layer as an adjustment layer of the PSD: its block after its name, its blend its native's
        const auto layered = with_correction(doc, core::normalize_adjust(Json{{"kind", "vibrance"}, {"vibrance", 20}, {"native", Json{{"blend", "multiply"}}}},
                                                                         &doc.page(0)));
        const auto made = formats::psd_adjust_layer(correction(layered), layered.page(0), render::Size{200, 200}, notes);
        QVERIFY(made);
        QVERIFY(made->blocks.starts_with("8BIMvibA"));
        QCOMPARE(made->blend, std::string("multiply"));
    }

    // The descriptors written (vibA, blwh, brightness_contrast's CgEd) byte for byte as before the PSD's descriptors had one
    // writer (render/psd_descriptor, shared with PSD-01's text layers): a versioned descriptor of an empty name of one
    // closing 0, the class "null", its items in order (the tint colour an RGBC object of three doubles, the preset's file
    // name an empty text).
    void psdDescriptorBytes() {
        const core::Page page = blank().page(0);
        std::vector<std::string> notes;
        const auto written = [&](const std::string& items, std::uint32_t count, std::string_view class_id = "null") {
            return be32(16) + be32(1) + be({0}) + be32(0) + std::string(class_id) + be32(count) + items;
        };
        auto block = render::psd::write_adjustment(core::normalize_adjust(Json{{"kind", "vibrance"}, {"vibrance", 20}, {"saturation", -5}}, &page), page, notes);
        QVERIFY(block && block->key == "vibA");
        QVERIFY(block->payload == written(long_item("vibrance", 20) + long_item("Strt", -5), 2));
        block = render::psd::write_adjustment(core::normalize_adjust(Json{{"kind", "black_white"}, {"tint", true}}, &page), page, notes);
        QVERIFY(block && block->key == "blwh");
        std::string items;
        const char* keys[] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
        const int weights[] = {40, 60, 40, 60, 20, 80};
        for (int k = 0; k < 6; ++k) items += long_item(keys[k], weights[k]);
        items += bool_item("useTint", true);
        items += key_of("tintColor") + "Objc" + be32(1) + be({0}) + be32(0) + "RGBC" + be32(3) + key_of("Rd  ") + "doub" + double_bytes(225) +
                 key_of("Grn ") + "doub" + double_bytes(211) + key_of("Bl  ") + "doub" + double_bytes(179);
        items += long_item("bwPresetKind", 1);
        items += key_of("blackAndWhitePresetFileName") + "TEXT" + be32(1) + be({0});
        QVERIFY(block->payload == written(items, 10));
        block = render::psd::write_adjustment(Json{{"kind", "brightness_contrast"}, {"brightness", 20}, {"contrast", 30}, {"native", Json{{"method", "modern"}}}}, page,
                                              notes);
        QVERIFY(block && block->key == "brit" && block->companion_key == "CgEd");
        QVERIFY(block->companion == written(long_item("Vrsn", 1) + long_item("Brgh", 20) + long_item("Cntr", 30) + long_item("means", 127) +
                                                bool_item("Lab ", false) + bool_item("useLegacy", false) + bool_item("Auto", false),
                                            7));
        // read back by the one reader too
        QCOMPARE((*render::psd::read_adjustment("vibA", written(long_item("vibrance", 20) + long_item("Strt", -5), 2), notes))["saturation"], Json(-5));
    }

    // Blocks laid out per the Adobe spec by hand (adjust_map.rs: the golden blocks), and what is not read (said, left
    // out); arbitrary bytes never throw.
    void psdBlocks() {
        std::vector<std::string> notes;
        const auto read = [&notes](const char* key, const std::string& d) { return render::psd::read_adjustment(key, d, notes); };
        const core::Page page = blank().page(0);
        const auto write = [&notes, &page](const Json& adjust) { return render::psd::write_adjustment(adjust, page, notes)->payload; };
        // blnc: written back byte for byte; the 19-byte form (no pad) the same
        std::string d = be({-30, 20, 0, 10, -5, 100, 0, 0, -100}) + std::string("\1\0", 2);
        auto a = read("blnc", d);
        QCOMPARE(*a, (Json{{"kind", "color_balance"}, {"shadows", Json::array({-30, 20, 0})}, {"midtones", Json::array({10, -5, 100})},
                           {"highlights", Json::array({0, 0, -100})}, {"preserve_luminosity", true}}));
        QVERIFY(write(*a) == d);
        QCOMPARE(*read("blnc", d.substr(0, 19)), *a);
        // phfl: version 2 RGB (65535, 32768, 0), density 40 %; Lab mid grey; version 3 white and a green
        std::string v2 = be({2, 0}) + std::string("\xff\xff\x80\x00\x00\x00\x00\x00", 8) + be32(40) + std::string("\1\0", 2);
        a = read("phfl", v2);
        QCOMPARE(*a, (Json{{"kind", "photo_filter"}, {"color", Json::array({255, 128, 0})}, {"density", 40}, {"preserve_luminosity", true}}));
        QVERIFY(write(*a).starts_with(be({2, 0, 65535, 32896, 0, 0}) + be32(40) + std::string("\1", 1)));  // (the colour is 8-bit in a book)
        a = read("phfl", be({2, 7, 5000, 0, 0, 0}) + be32(25) + std::string("\0", 1));
        for (const Json& c : (*a)["color"]) QVERIFY(std::abs(c.get<double>() - 0.466 * 255) < 3);
        QCOMPARE((*a)["density"], Json(25));
        QCOMPARE((*a)["preserve_luminosity"], Json(false));
        a = read("phfl", std::string("\0\3", 2) + be32(10000) + be32(0) + be32(0) + be32(100) + std::string("\1", 1));
        QCOMPARE((*a)["color"], Json::array({255, 255, 255}));
        QCOMPARE((*a)["density"], Json(100));
        a = read("phfl", std::string("\0\3", 2) + be32(5000) + be32(static_cast<std::uint32_t>(-6000)) + be32(0) + be32(30) + std::string("\0", 1));
        QVERIFY((*a)["color"][1] > (*a)["color"][0] && (*a)["color"][1] > (*a)["color"][2]);
        // mixr: RGB records; the grey record alone of a monochrome one
        d = be({1, 0, 100, 0, 0, 0, 20, 50, 50, 0, 0, 0, 0, -50, 150, 0, -10});
        a = read("mixr", d);
        QCOMPARE(*a, (Json{{"kind", "channel_mixer"}, {"red", Json::array({100, 0, 0, 20})}, {"green", Json::array({50, 50, 0, 0})},
                           {"blue", Json::array({0, -50, 150, -10})}, {"gray", Json::array({40, 40, 20, 0})}, {"monochrome", false}}));
        QVERIFY(write(*a) == d + be({100, 0, 0, 0, 20}));  // (the first record again: PhotoCraft's)
        a = read("mixr", be({1, 1, 30, 59, 11, 0, 0}));
        QCOMPARE((*a)["gray"], Json::array({30, 59, 11, 0}));
        QCOMPARE((*a)["monochrome"], Json(true));
        // vibA: version 16; Photoshop leaves out a 0; another version not read
        d = descriptor({long_item("vibrance", -40), long_item("Strt", 25)});
        QCOMPARE(*read("vibA", d), (Json{{"kind", "vibrance"}, {"vibrance", -40}, {"saturation", 25}}));
        QCOMPARE(*read("vibA", descriptor({long_item("Strt", 10)})), (Json{{"kind", "vibrance"}, {"vibrance", 0}, {"saturation", 10}}));
        std::string v17 = d;
        v17[3] = 17;
        QVERIFY(!read("vibA", v17));
        // blwh: the weights, the tint (127.5 rounds to 128), off with a colour stored, the defaults when empty
        std::vector<std::string> weights;
        const char* keys[] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
        for (int k = 0; k < 6; ++k) weights.push_back(long_item(keys[k], 10 * (k + 1)));
        std::vector<std::string> on = weights;
        on.push_back(bool_item("useTint", true));
        on.push_back(rgbc_item("tintColor", 255, 127.5, 0));
        on.push_back(long_item("bwPresetKind", 1));
        QCOMPARE(*read("blwh", descriptor(on)), (Json{{"kind", "black_white"}, {"reds", 10}, {"yellows", 20}, {"greens", 30}, {"cyans", 40}, {"blues", 50},
                                                      {"magentas", 60}, {"tint", true}, {"tint_color", Json::array({255, 128, 0})}}));
        std::vector<std::string> off = weights;
        off.push_back(bool_item("useTint", false));
        QCOMPARE((*read("blwh", descriptor(off)))["tint"], Json(false));
        QCOMPARE(*read("blwh", descriptor({})), core::normalize_adjust(Json{{"kind", "black_white"}}, nullptr));
        // selc: absolute, reds and blacks; written back byte for byte
        d = be({1, 1}) + std::string(8, '\0');
        for (int r = 0; r < 9; ++r) d += r == 0 ? be({10, -20, 30, -40}) : r == 8 ? be({0, 0, 0, 25}) : be({0, 0, 0, 0});
        a = read("selc", d);
        QCOMPARE((*a)["method"], Json("absolute"));
        QCOMPARE((*a)["reds"], Json::array({10, -20, 30, -40}));
        QCOMPARE((*a)["blacks"], Json::array({0, 0, 0, 25}));
        QVERIFY(write(*a) == d);
        // hue2: colorize's numbers from its own place
        d = be({2}) + std::string("\1\0", 2) + be({200, 50, 0, 0, 0, 0});
        a = read("hue2", d);
        QCOMPARE((*a)["colorize"], Json(true));
        QCOMPARE((*a)["hue"], Json(200));
        QCOMPARE((*a)["saturation"], Json(50));
        QCOMPARE((*a)["reds"]["range"], Json::array({-45, -15, 15, 45}));  // (a short block: the ranges' defaults)
        // not read: said, left out
        notes.clear();
        QVERIFY(!read("blnc", std::string(18, '\0')));
        QVERIFY(!read("vibA", std::string("\0\0\0\x0f\0", 5)));
        QVERIFY(!read("blwh", std::string()));
        QVERIFY(!read("mixr", be({2, 0})));
        QVERIFY(!read("mixr", std::string("\0\1\0\0\0\x64", 6)));
        std::string cmyk = be({1, 0});
        for (int r = 0; r < 4; ++r) cmyk += be({r == 0 ? 100 : 0, r == 1 ? 100 : 0, r == 2 ? 100 : 0, r == 3 ? 100 : 0, 0});
        QVERIFY(!read("mixr", cmyk));  // (a CMYK mixer: no RGB meaning)
        QVERIFY(!read("phfl", be({4, 0})));
        QVERIFY(!read("phfl", be({2, 1, 0, 0, 0, 0, 0, 0}) + be32(25) + std::string("\1", 1)));  // (HSB)
        QVERIFY(!read("phfl", std::string("\0\3", 2) + be32(20000) + be32(0) + be32(0) + be32(25) + std::string("\1", 1)));
        QVERIFY(!read("phfl", be({2, 0, 0})));
        QVERIFY(!read("selc", be({1, 0})));
        QVERIFY(!read("hue2", be({1, 0})));
        QCOMPARE(notes.size(), std::size_t(12));
        QVERIFY(notes[0].find("カラーバランス") != std::string::npos);
        for (const char* key : {"hue2", "vibA", "blnc", "blwh", "phfl", "mixr", "selc"}) {
            for (std::size_t len = 0; len < 96; ++len) {
                std::string junk;
                for (std::size_t i = 0; i < len; ++i) junk += static_cast<char>((i * 37 + len) & 0xff);
                try {
                    (void)read(key, junk);
                } catch (const std::exception& error) {
                    QFAIL(qPrintable(QString("%1 %2: %3").arg(key).arg(len).arg(error.what())));
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestAdjustColor)
#include "test_adjust_color.moc"
