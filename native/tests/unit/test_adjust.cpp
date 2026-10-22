// PC2-0 (補正の土台, SPEC.md COLOR-04): the corrections' common ground — Python's form kept as it is (its JSON, its tables,
// its pixels, no feature named), a `native` read, normalized and stored under adjustments@1 (what is not well formed
// opens read-only), the formula's choice (native.method), the kinds' table and the ops and filter kinds not in this
// build yet (refused, named), the three ways a correction is drawn through one AdjustFunction, native.blend on each
// (the 8-bit page: blend_over's bytes; 16 bits: the 8-bit page's within a level; CMYK: the plates multiplied), a
// correction layer's own blend not looked at (the user's decision 2), and the PSD's frame. PC2a (階調の補正・3D LUT):
// the tone kinds' upstream formulas (the soft toe, the natural spline, the composite before the channels; brightness and
// contrast modern and legacy, posterize's floor, a gradient map's reverse and dither), the channel records in their own
// space on every kind of page (through Little CMS where the page's differs), auto_adjust, equalize, set_adjustment,
// color_lookup (its table made the book's asset, saved and read back), the PSD's tone codecs and the .cube export.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/filter_kinds.hpp"
#include "core/adjust_tone.hpp"
#include "core/base64.hpp"
#include "core/color_adjust.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "formats/exporting.hpp"
#include "formats/psd_adjust.hpp"
#include "render/adjust.hpp"
#include "render/color_canvas.hpp"
#include "render/filters.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_adjust.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/psd_adjust.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "comma_locale.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
constexpr std::uint32_t kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

// "<code>: <message>" of an op refused, or "" when it is not.
std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank() {
    auto doc = core::new_episode("補正", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// An opaque picture of 8-bit colours: gradients, a light square, a dark speck.
std::array<int, 4> colours(std::uint32_t x, std::uint32_t y) {
    std::array<int, 4> v{static_cast<int>((x * 2 + y) % 256), static_cast<int>((y * 3 + 40) % 256), static_cast<int>((x * y) % 256), 255};
    if (x > 30 && x < 60 && y > 30 && y < 60) v = {240, 235, 230, 255};
    if (x == 80 && y == 80) v = {10, 10, 10, 255};
    return v;
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

// A correction layer laid on the page (as a book holds it: not through an op).
core::Document with_correction(core::Document doc, const Json& adjust, double opacity = 1.0, const std::string& blend = "normal") {
    core::Layer layer;
    layer.id = "adj";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Adjust;
    layer.adjust = adjust;
    layer.opacity = opacity;
    layer.blend = blend;
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

std::string shown(const core::Document& doc) { return render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.tobytes(); }

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

std::string icc_bytes() { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))); }

// A CMYK page (16 bits) with a paint layer of inks: a grey on K alone and a colour of four inks, in halves.
core::Document cmyk_page() {
    auto doc = blank();
    doc = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", 16}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, 16, kSide, kSide);
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const std::vector<double> ink = x < kSide / 2 ? std::vector<double>{0, 0, 0, 0.3 + 0.4 * y / kSide} : std::vector<double>{0.2, 0.7, 0.1, 0.05};
            for (unsigned c = 0; c < 4; ++c) p.set(std::size_t(y) * kSide + x, c, ink[c]);
            p.set(std::size_t(y) * kSide + x, 4, 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

core::InkPixels inks_shown(const core::Document& doc) {
    render::RenderOptions o = render::proof_options();
    o.inks = true;
    return *render::render_page(doc.page(0), 200, o, &doc).inks;
}

// AdjustFunctions of the test's own (the kinds' formulas are their groups'): red, green and blue turned round; the same
// as tables; one that adds where the pixel is on the page.
struct Turn : core::AdjustFunction {
    std::array<double, 3> apply(const std::array<double, 3>& rgb, int, int) const override { return {rgb[1], rgb[2], rgb[0]}; }
};
struct TurnTables : Turn {
    std::optional<core::ChannelTables> tables() const override {
        core::ChannelTables t;  // (tables of its own — each channel inverted —: what the 8-bit page lays when a function has them)
        for (int i = 0; i < 256; ++i) t.rgb[0][i] = t.rgb[1][i] = t.rgb[2][i] = static_cast<std::uint8_t>(255 - i);
        return t;
    }
};
struct Where : core::AdjustFunction {
    std::array<double, 3> apply(const std::array<double, 3>&, int x, int y) const override { return {x / 255.0, y / 255.0, 0.0}; }
    bool positional() const override { return true; }
};

}  // namespace

class TestAdjust : public QObject {
    Q_OBJECT

private slots:
    // native の無い層: Python's form — the JSON as given (its order, its types), Python's tables, no feature named.
    void pythonFormKeepsItsJson() {
        QTemporaryDir tmp;
        const auto doc = eight_bit_page();
        for (const Json& raw : {Json{{"kind", "levels"}, {"black", 20}, {"white", 220}},
                                Json{{"black", 20}, {"kind", "levels"}, {"gamma", 1.5}, {"out_white", "200"}, {"channel", "r"}},
                                Json{{"kind", "curve"}, {"points", Json::array({Json::array({0, 0}), Json::array({128, 100}), Json::array({255, 255})})}},
                                Json{{"kind", "hue"}, {"shift", "45"}, {"saturation", 1.2}}, Json{{"kind", "invert"}},
                                Json{{"kind", "brightness_contrast"}, {"brightness", 20}, {"contrast", "-10"}},
                                Json{{"kind", "exposure"}, {"exposure", 1.0}}}) {
            const auto made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", raw}});
            const core::Layer& layer = correction(made);
            QCOMPARE(QString::fromStdString(layer.adjust->dump()), QString::fromStdString(raw.dump()));
            QVERIFY(!core::uses_adjustments(*layer.adjust));
            QVERIFY(core::python_way(*layer.adjust));
            QVERIFY(!names(payload_of(made, tmp), core::kAdjustmentsFeature));
            // a native that says nothing (empty, null): the layer is Python's, as given
            for (const Json& none : {Json::object(), Json()}) {
                Json given = raw;
                given["native"] = none;
                const auto same = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", given}});
                QCOMPARE(QString::fromStdString(correction(same).adjust->dump()), QString::fromStdString(raw.dump()));
            }
        }
    }

    // Python's form drawn as Python draws it (its tables on the page), and a native that changes nothing of the colours
    // (blend normal, method genko) drawn the same bytes.
    void pythonFormDrawnAsBefore() {
        const auto doc = eight_bit_page();
        const render::Image base = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image;
        for (const Json& raw : {Json{{"kind", "levels"}, {"black", 30}, {"white", 210}}, Json{{"kind", "invert"}},
                                Json{{"kind", "curve"}, {"gamma", 1.7}}}) {
            const std::string plain = shown(with_correction(doc, raw));
            Json params = raw;
            params.erase("kind");
            const std::string tables = render::filters::apply_filter(base, raw["kind"].get<std::string>(), params).convert("RGB").tobytes();
            QVERIFY(plain == tables);
            for (const Json& native : {Json{{"blend", "normal"}}, Json{{"method", "genko"}}}) {
                if (native.contains("method") && raw["kind"] != Json("levels") && raw["kind"] != Json("curve")) continue;
                Json with = raw;
                with["native"] = native;
                QVERIFY(core::python_way(with));
                QVERIFY2(shown(with_correction(doc, with)) == plain, native.dump().c_str());
            }
        }
    }

    // native の有る層: adjustments@1, read in the table's order, kept on reopening; gone with the last of them.
    void nativeNamesTheFeature() {
        QTemporaryDir tmp;
        const auto doc = eight_bit_page();
        const auto made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                        {"adjust", Json{{"kind", "levels"}, {"black", 10}, {"white", 240}, {"native", Json{{"method", "genko"}, {"blend", "multiply"}}}}}});
        const Json expected{{"kind", "levels"}, {"black", 10}, {"white", 240}, {"native", Json{{"blend", "multiply"}, {"method", "genko"}}}};
        QCOMPARE(QString::fromStdString(correction(made).adjust->dump()), QString::fromStdString(expected.dump()));
        QVERIFY(core::uses_adjustments(made));
        const Json payload = payload_of(made, tmp);
        QVERIFY(names(payload, core::kAdjustmentsFeature));
        const auto loaded = storage::load_document_payload(payload, std::filesystem::path(tmp.path().toStdString()));
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QCOMPARE(QString::fromStdString(correction(loaded.document).adjust->dump()), QString::fromStdString(expected.dump()));
        const auto gone = run(made, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "adj"}});
        QVERIFY(!names(payload_of(gone, tmp), core::kAdjustmentsFeature));
        // set_layer adjust reads it too; an exposure's native beside its own settings
        const auto set = run(made, Json{{"op", "set_layer"}, {"page", 1}, {"id", "adj"},
                                        {"adjust", Json{{"kind", "exposure"}, {"exposure", 0.5}, {"native", Json{{"blend", "screen"}}}}}});
        QCOMPARE(correction(set).adjust->at("native"), (Json{{"blend", "screen"}}));
        QVERIFY(names(payload_of(set, tmp), core::kAdjustmentsFeature));
    }

    // What a native may not say: refused by the op, and opens a book read-only (知らないキーは読み取り専用).
    void nativeRefusals() {
        QTemporaryDir tmp;
        const auto doc = eight_bit_page();
        const auto add = [&](const Json& adjust) { return refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", adjust}}); };
        for (const Json& bad : {Json{{"kind", "invert"}, {"native", 5}}, Json{{"kind", "invert"}, {"native", Json{{"unknown", 1}}}},
                                Json{{"kind", "invert"}, {"native", Json{{"blend", "nope"}}}},
                                Json{{"kind", "levels"}, {"native", Json{{"method", "other"}}}},
                                Json{{"kind", "hue"}, {"native", Json{{"method", "genko"}}}},
                                Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", "yes"}}}},
                                Json{{"kind", "brightness_contrast"}, {"native", Json{{"method", "genko"}}}}}) {
            const std::string why = add(bad);
            QVERIFY2(why.starts_with("apply: "), (bad.dump() + " → " + why).c_str());
        }
        // a book holding one: read-only
        const auto made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", "invert"}, {"native", Json{{"blend", "screen"}}}}}});
        const Json payload = payload_of(made, tmp);
        for (const Json& stored : {Json{{"blend", "screen"}, {"unknown", 1}}, Json{{"blend", "nope"}}, Json{{"method", "genko"}}, Json(3),
                                   Json{{"blend", "multiply"}, {"channels", Json{{"red", Json{{"black", 0}}}}}}}) {
            Json bad = payload;
            bad["pages"][0]["layers"].back()["adjust"]["native"] = stored;
            const auto loaded = storage::load_document_payload(bad, std::filesystem::path(tmp.path().toStdString()));
            QVERIFY2(!loaded.document.read_only_reason.empty(), stored.dump().c_str());
        }
        // a kind of the C++ build's own with a setting out of its range, or one it does not have: read-only
        for (const Json& stored : {Json{{"kind", "vibrance"}, {"vibrance", 300}}, Json{{"kind", "vibrance"}, {"tint", true}}}) {
            Json bad = payload;
            bad["pages"][0]["layers"].back()["adjust"] = stored;
            const auto loaded = storage::load_document_payload(bad, std::filesystem::path(tmp.path().toStdString()));
            QVERIFY2(!loaded.document.read_only_reason.empty(), stored.dump().c_str());
        }
        // a Lab page composites only Lab's blends (as its layers')
        const auto lab = run(blank(), Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"depth", 16}});
        const std::string why = refusal(lab, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                                  {"adjust", Json{{"kind", "invert"}, {"native", Json{{"blend", "difference"}}}}}});
        QVERIFY2(why.find("Lab page cannot composite the blend") != std::string::npos, why.c_str());
        QVERIFY(refusal(lab, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", "invert"}, {"native", Json{{"blend", "multiply"}}}}}}).empty());
        // a correction layer's own blend is not looked at there either (the user's decision 2: only its native.blend)
        const auto own = run(blank(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"blend", "difference"}, {"adjust", Json{{"kind", "invert"}}}});
        QVERIFY(refusal(own, Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"depth", 16}}).empty());
    }

    // The user's decision 4: native.method — genko (Python's formula, the default: the same bytes) or upstream
    // (PhotoCraft's, PC2a: drawn by it). (PC2-0 refused upstream as not yet ported; PC2a's formulas take it over.)
    void methodChoice() {
        const auto doc = eight_bit_page();
        for (const std::string kind : {"levels", "curve"}) {
            const Json genko{{"kind", kind}, {"native", Json{{"method", "genko"}}}};
            QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", genko}}).empty());
            QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                      {"adjust", Json{{"kind", kind}, {"native", Json{{"method", "upstream"}}}}}}).empty());
        }
        for (const Json& now : {Json{{"kind", "brightness_contrast"}, {"native", Json{{"method", "modern"}}}},
                                Json{{"kind", "posterize"}, {"native", Json{{"method", "floor"}}}},
                                Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", true}}}}}) {
            QVERIFY2(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", now}}).empty(), now.dump().c_str());
        }
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                  {"adjust", Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", false}, {"dither", false}}}}}}).empty());
        // a straight stretch is the same either way (within a level); a midtone gamma above 1 has Photoshop's soft toe
        const auto genko = with_correction(doc, Json{{"kind", "levels"}, {"black", 20}, {"white", 230}});
        const auto upstream = with_correction(doc, Json{{"kind", "levels"}, {"black", 20}, {"white", 230}, {"native", Json{{"method", "upstream"}}}});
        core::validate_adjustments(upstream);
        QVERIFY(worst_difference(shown(genko), shown(upstream)) <= 1);
        const core::AdjustPtr soft = core::adjust_function(Json{{"kind", "levels"}, {"gamma", 3.0}, {"native", Json{{"method", "upstream"}}}}, doc.page(0));
        const core::AdjustPtr hard = core::adjust_function(Json{{"kind", "levels"}, {"gamma", 3.0}, {"native", Json{{"method", "genko"}}}}, doc.page(0));
        QVERIFY(soft && hard);
        QVERIFY(soft->apply({0.002, 0.002, 0.002}, 0, 0)[0] < hard->apply({0.002, 0.002, 0.002}, 0, 0)[0] - 0.05);  // (the toe: bounded slope)
        QVERIFY(std::abs(soft->apply({0.6, 0.6, 0.6}, 0, 0)[0] - std::pow(0.6, 1 / 3.0)) < 0.01);  // (the body: the power curve)
    }

    // levels' and a tone curve's channel records, read as PhotoCraft reads them (their formula is PC2a's).
    void channelRecords() {
        using core::ReadMode;
        const Json levels = core::read_tone_channels("levels", Json{{"red", Json{{"black", 300}, {"gamma", 0}}}, {"blue", Json{{"black", 100}, {"white", 50}}}},
                                                     "rgb", ReadMode::Op, "levels");
        QCOMPARE(levels, (Json{{"red", Json{{"black", 253}, {"white", 255}, {"gamma", 0.01}, {"out_black", 0}, {"out_white", 255}}},
                               {"blue", Json{{"black", 100}, {"white", 102}, {"gamma", 1}, {"out_black", 0}, {"out_white", 255}}}}));
        QVERIFY_EXCEPTION_THROWN(core::read_tone_channels("levels", Json{{"cyan", Json::object()}}, "rgb", ReadMode::Op, "levels"), core::OpError);
        QVERIFY_EXCEPTION_THROWN(core::read_tone_channels("levels", Json{{"red", Json{{"black", 300}}}}, "rgb", ReadMode::Stored, "levels"), core::OpError);
        QCOMPARE(core::read_tone_channels("levels", Json{{"black", Json{{"black", 5}}}}, "cmyk", ReadMode::Stored, "levels"),
                 (Json{{"black", Json{{"black", 5}}}}));
        const Json curve = core::read_tone_channels("curve", Json{{"lightness", Json::array({Json::array({255, 300}), Json::array({0, 0}), Json::array({0.2, 5})})}},
                                                    "lab", ReadMode::Op, "curve");
        QCOMPARE(curve, (Json{{"lightness", Json::array({Json::array({0, 0}), Json::array({255, 255})})}}));
        QVERIFY_EXCEPTION_THROWN(core::read_tone_channels("curve", Json{{"gray", Json::array({Json::array({9, 9}), Json::array({0, 0})})}}, "gray",
                                                          ReadMode::Stored, "curve"),
                                 core::OpError);
        QVERIFY_EXCEPTION_THROWN(core::read_tone_channels("curve", Json{{"red", Json::array({Json::array({9, 9})})}}, "rgb", ReadMode::Op, "curve"),
                                 core::OpError);
        // stored: records need their space; well formed ones are (their formula not being here yet does not make the
        // book read-only)
        auto doc = with_correction(eight_bit_page(), Json{{"kind", "curve"}, {"native", Json{{"space", "lab"}, {"channels", Json{{"a", Json::array({Json::array({0, 10}), Json::array({255, 245})})}}}}}});
        core::validate_adjustments(doc);
        doc.edit_page(0).layers.back().adjust->at("native").erase("space");
        QVERIFY_EXCEPTION_THROWN(core::validate_adjustments(doc), core::Error);
        QCOMPARE(core::default_tone_space(nullptr), std::string_view("rgb"));
    }

    // The table: every kind, its group, its settings' defaults in their order and ranges; each kind of the C++ build's
    // own and each of its ops refused, named, until its group writes it.
    void kindsTable() {
        const auto& kinds = core::adjust_kinds();
        std::vector<std::string> python;
        for (const auto& kind : kinds) {
            // (PC4 節 D: フィルター's kinds, core/filter_kinds — their own tests: test_filter_kinds, test_fx)
            QVERIFY(kind.group == "Python" || kind.group == "PC2a" || kind.group == "PC2b" || kind.group == "PC2c" || kind.group == "PC4");
            QVERIFY(kind.filter);
            QVERIFY(!kind.native.empty() && kind.native.front().key == "blend");
            if (kind.group == "Python") python.emplace_back(kind.name);
        }
        QCOMPARE(python, std::vector<std::string>(core::kAdjustments.begin(), core::kAdjustments.end()));
        // (Python's, PC2b's eight, PC2a's two, PC2c's three; then PC4's, as many as its groups have added)
        QCOMPARE(kinds.size(), std::size_t(10 + 8 + 2 + 3) + core::filter_infos().size());
        const auto doc = eight_bit_page();
        for (const auto& kind : kinds) {
            if (kind.group == "Python") continue;
            const std::string name(kind.name);
            Json given = Json::object();
            if (name == "replace_color") given["color"] = Json::array({200, 30, 30});
            const Json read = core::read_params(kind.params, given, core::ReadMode::Op, name);
            std::vector<std::string> keys, expected;
            for (const auto& [key, value] : read.items()) keys.push_back(key);
            for (const auto& p : kind.params)
                if (!p.optional) expected.emplace_back(p.key);
            QCOMPARE(keys, expected);
            (void)core::read_params(kind.params, read, core::ReadMode::Stored, name);  // (its normal form is well formed)
            // (a kind whose group has not written its formula yet: refused, named — its group's own tests take it over)
            if (kind.layer && !kind.hooks.function) {
                const std::string why = refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", name}}}});
                QVERIFY2(why.starts_with("not_yet_ported: ") && why.find(name) != std::string::npos, why.c_str());
            }
            bool missing = false;
            try {
                (void)render::adjust_ops::filter_work(doc, doc.page(0), name, given);
            } catch (const core::Error& error) {
                missing = error.code() == "not_yet_ported";
            }
            if (!missing) continue;
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", name}};
            for (const auto& [key, value] : given.items()) op[key] = value;
            const std::string why = refusal(doc, op);
            QVERIFY2(why.starts_with("not_yet_ported: ") && why.find(name) != std::string::npos, why.c_str());
        }
        // a setting out of range clamped, of the wrong type refused; a blend is a correction layer's, not a filter's
        QCOMPARE(core::read_params(core::adjust_kind("vibrance")->params, Json{{"vibrance", 500}}, core::ReadMode::Op, "v")["vibrance"], Json(100));
        QVERIFY_EXCEPTION_THROWN(core::read_params(core::adjust_kind("vibrance")->params, Json{{"vibrance", "a"}}, core::ReadMode::Op, "v"), core::OpError);
        QVERIFY(refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "invert"}, {"native", Json{{"blend", "multiply"}}}})
                    .starts_with("apply: "));
        // (the ops whose bodies are not written yet: each group takes its own out of this list as it writes it)
        const std::vector<const char*> not_yet{};  // (PC2a and PC2c wrote theirs: none is left)
        for (const char* name : not_yet) {
            const std::string why = refusal(doc, Json{{"op", name}, {"page", 1}});
            QVERIFY2(why.starts_with("not_yet_ported: ") && why.find(name) != std::string::npos, why.c_str());
        }
        // a filter_raster of Python's kind with a native that changes nothing: Python's way, the same pixels
        const auto plain = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "levels"}, {"black", 20}});
        const auto genko = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "levels"}, {"black", 20},
                                         {"native", Json{{"method", "genko"}}}});
        QVERIFY(*plain.page(0).layers[0].raster_png == *genko.page(0).layers[0].raster_png);
    }

    // native.blend on the 8-bit page: the corrected picture over the picture under it by blend_over's formula — the same
    // bytes as a layer of that picture in that blend.
    void blendEightBitIsBlendOver() {
        const auto doc = eight_bit_page();
        const render::Image inverted = render::filters::apply_filter(picture(), "invert", Json::object());
        for (const std::string mode : {"multiply", "screen", "difference", "color"}) {
            const auto corrected = with_correction(doc, Json{{"kind", "invert"}, {"native", Json{{"blend", mode}}}});
            auto layered = doc;
            core::Layer over = paint_layer(layered.page(0), "over", inverted);
            over.blend = mode;
            layered.edit_page(0).layers.push_back(over);
            QVERIFY2(shown(corrected) == shown(layered), mode.c_str());
        }
        // its own blend is not looked at (Python's), at any opacity
        const auto own = with_correction(doc, Json{{"kind", "invert"}}, 0.6, "multiply");
        QVERIFY(shown(own) == shown(with_correction(doc, Json{{"kind", "invert"}}, 0.6)));
    }

    // 16 bits: within a level of the 8-bit page; a correction layer's own blend not looked at (it was refused before:
    // the user's decision 2).
    void blendSixteenBitWithinALevel() {
        const auto eight = eight_bit_page();
        const auto precise = precise_page();
        for (const std::string mode : {"multiply", "screen", "darken"}) {
            const Json adjust{{"kind", "invert"}, {"native", Json{{"blend", mode}}}};
            const int worst = worst_difference(shown(with_correction(precise, adjust, 0.75)), shown(with_correction(eight, adjust, 0.75)));
            QVERIFY2(worst <= 1, qPrintable(QString::fromStdString(mode) + QString(" off by %1").arg(worst)));
        }
        const std::string normal = shown(with_correction(precise, Json{{"kind", "invert"}}));
        QVERIFY(shown(with_correction(precise, Json{{"kind", "invert"}}, 1.0, "difference")) == normal);
        // an exposure in a blend: the exposed colour over the one below in their encoded samples
        render::ColorCanvas canvas(render::Image::create("RGBA", render::Size{1, 1}, render::Ink{100, 150, 200, 255}));
        const core::Exposure e = core::Exposure::parse(Json{{"exposure", 1.0}});
        canvas.expose(e, 1.0, false, nullptr, "screen");
        const auto p = canvas.linear_pixel(0);
        for (unsigned c = 0; c < 3; ++c) {
            const double was = std::array<double, 3>{100, 150, 200}[c] / 255.0;
            const double exposed = core::linear_to_srgb(e.apply(core::srgb_to_linear(was)));
            QVERIFY(std::abs(core::linear_to_srgb(p[c]) - (was + exposed - was * exposed)) < 1e-9);
        }
    }

    // CMYK: the plates multiplied (the ink canvas's blend: 1 − (1 − below)(1 − corrected) on each plate).
    void blendCmykMultiplies() {
        const auto doc = cmyk_page();
        const core::InkPixels below = inks_shown(doc);
        const core::InkPixels inverted = inks_shown(with_correction(doc, Json{{"kind", "invert"}}));
        const core::InkPixels multiplied = inks_shown(with_correction(doc, Json{{"kind", "invert"}, {"native", Json{{"blend", "multiply"}}}}));
        QCOMPARE(multiplied.samples.size(), below.samples.size());
        int worst = 0;
        for (std::size_t i = 0; i < below.pixel_count(); ++i)
            for (unsigned c = 0; c < 4; ++c) {
                const double b = below.value(i, c), a = inverted.value(i, c);
                const double expected = 1 - (1 - b) * (1 - a);
                worst = std::max(worst, static_cast<int>(std::lround(std::abs(multiplied.value(i, c) - expected) * 65535)));
            }
        QVERIFY2(worst <= 2, qPrintable(QString("off by %1 / 65535").arg(worst)));
        // a blend Lab does not composite: left out of a preview, as a layer's
        const auto lab = with_correction(run(blank(), Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"depth", 16}}),
                                         Json{{"kind", "invert"}, {"native", Json{{"blend", "difference"}}}});
        auto preview = render::proof_options();
        preview.skip_unported = true;
        QCOMPARE(render::render_page(lab.page(0), 200, preview, &lab).omitted, std::vector<std::string>{"ink_blend:difference"});
    }

    // The three ways take one AdjustFunction: the 8-bit page (tables or each colour, rounded; where the pixel is on the
    // page), the precise canvas (in its encoded samples, the blend by its formula), the ink canvas (the colours inks mean,
    // or records on the plates).
    void oneFunctionThreeWays() {
        const render::Image image = picture();
        const std::string turned = render::adjust::apply_8bit(image, Turn()).tobytes();
        const std::string tabled = render::adjust::apply_8bit(image, TurnTables()).tobytes();
        const std::string source = image.tobytes();
        for (std::size_t i = 0; i < source.size(); i += 4) {
            QCOMPARE(turned[i], source[i + 1]);
            QCOMPARE(turned[i + 2], source[i]);
            QCOMPARE(turned[i + 3], source[i + 3]);
            QCOMPARE(static_cast<unsigned char>(tabled[i]), static_cast<unsigned char>(255 - static_cast<unsigned char>(source[i])));
        }
        const std::string placed = render::adjust::apply_8bit(image, Where(), render::Point{5, 7}).tobytes();
        QCOMPARE(int(static_cast<unsigned char>(placed[0])), 5);
        QCOMPARE(int(static_cast<unsigned char>(placed[1])), 7);
        QCOMPARE(int(static_cast<unsigned char>(placed[(kSide + 1) * 4])), 6);
        // the precise canvas: in its encoded samples, multiply by its formula
        render::ColorCanvas canvas(render::Image::create("RGBA", render::Size{1, 1}, render::Ink{100, 150, 200, 255}));
        canvas.adjust(Turn(), 1.0, false, nullptr, "multiply");
        const auto p = canvas.linear_pixel(0);
        const std::array<double, 3> was{100 / 255.0, 150 / 255.0, 200 / 255.0};
        for (unsigned c = 0; c < 3; ++c) QVERIFY(std::abs(core::linear_to_srgb(p[c]) - was[c] * was[(c + 1) % 3]) < 1e-9);
        QVERIFY_EXCEPTION_THROWN(canvas.adjust(Turn(), 1.0, false, nullptr, "nope"), core::Error);
        // the ink canvas: K's record on the plate itself (its brightness halved: no ink → K 50%)
        const auto doc = cmyk_page();
        render::InkCanvas inks(*doc.page(0).color, render::Image::create("RGB", render::Size{2, 1}, render::Ink{255, 255, 255}));
        core::InkRecords records;
        records.space = core::InkSpace::Cmyk;
        records.plates = {{}, {}, {}, [](double t) { return t * 0.5; }};
        inks.adjust_planes(records, 1.0, false);
        const core::InkPixels out = inks.ink_pixels(16);
        QVERIFY(std::abs(out.value(0, 3) - 0.5) < 1e-4 && out.value(0, 0) == 0);
        records.space = core::InkSpace::Lab;
        QVERIFY_EXCEPTION_THROWN(inks.adjust_planes(records, 1.0, false), core::Error);
        // Python's form is one (PreciseAdjustment): its formula through the interface as through its own call
        const core::PreciseAdjustment levels("levels", Json{{"black", 20}, {"white", 220}});
        const core::AdjustFunction& f = levels;
        QVERIFY(f.apply({0.5, 0.25, 0.75}, 3, 4) == levels({0.5, 0.25, 0.75}));
        QVERIFY(core::adjust_function(Json{{"kind", "levels"}, {"black", 20}}, doc.page(0)) != nullptr);
        QVERIFY(core::adjust_function(Json{{"kind", "blur"}}, doc.page(0)) == nullptr);
    }

    // The PSD's frame (the user's decision 5): the table of adjustment blocks, each with its kind and group; until a codec
    // is written, read as before (left out, said) and not written (Python's PSD: the change in the merged picture only);
    // the correction layers handed to the writer in their places.
    void psdFrame() {
        const auto& codecs = render::psd::adjust_codecs();
        QCOMPARE(codecs.size(), std::size_t(17));  // (hue2 twice: hue_saturation's, and Python's hue written as the nearest)
        for (const auto& codec : codecs) {
            QVERIFY(core::adjust_kind(codec.kind) != nullptr);
            QVERIFY(codec.group == "PC2a" || codec.group == "PC2b");
        }
        const auto codec_of = [&codecs](std::string_view key) {
            return *std::find_if(codecs.begin(), codecs.end(), [key](const auto& c) { return c.key == key; });
        };
        std::vector<std::string> notes;
        if (!codec_of("levl").read) {  // (until PC2a writes it)
            QVERIFY(!render::psd::read_adjustment("levl", "", notes));
            QVERIFY(!notes.empty() && notes.back().find("levels") != std::string::npos);
        }
        QVERIFY(!render::psd::read_adjustment("SoCo", "", notes));
        const auto doc = with_correction(eight_bit_page(), Json{{"kind", "invert"}});
        if (!codec_of("nvrt").write) {  // (until PC2a writes it: not written, as Python's PSD)
            QVERIFY(!render::psd::write_adjustment(Json{{"kind", "invert"}}, doc.page(0), notes));
            QVERIFY(!formats::psd_adjust_layer(correction(doc), doc.page(0), render::Size{200, 200}, notes));
        }
        std::vector<std::string> order;
        const std::function<void(render::PageLayer&&)> each = [&](render::PageLayer&& layer) { order.push_back(layer.name); };
        const std::function<void(const core::Layer&)> adjust_layers = [&](const core::Layer& layer) { order.push_back("adjust " + layer.id); };
        render::page_layers(doc.page(0), doc, 100, each, nullptr, &adjust_layers);
        QVERIFY(std::find(order.begin(), order.end(), "adjust adj") != order.end());
        QVERIFY(std::find(order.begin(), order.end(), "adjust adj") > std::find_if(order.begin(), order.end(), [](const std::string& n) { return n.find("paint") != std::string::npos; }));
    }

    // --- PC2a (階調の補正・3D LUT) ----------------------------------------------------------------------------------

    // PhotoCraft's tone records in their space (compose/adjust.rs 656-697): identity curves leave every space as it is;
    // a CMYK black curve darkens (more K everywhere), a Lab lightness lift keeps a grey grey — on an RGB page through the
    // drawing's conversions (built-in coated-cmyk, D50 Lab).
    void toneRecordsInTheirSpace() {
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        const Json straight = Json::array({Json::array({0, 0}), Json::array({255, 255})});
        for (const std::string space : {"rgb", "cmyk", "lab", "gray"}) {
            Json channels = Json::object();
            for (const std::string_view name : core::tone_channels(space)) channels[std::string(name)] = straight;
            const core::AdjustPtr f = core::adjust_function(
                Json{{"kind", "curve"}, {"points", straight}, {"native", Json{{"method", "upstream"}, {"space", space}, {"channels", channels}}}}, page);
            QVERIFY(f);
            for (int i = 0; i < 64; ++i) {
                const std::array<double, 3> c{(i % 8) / 7.0, (i / 8) / 7.0, 0.3};
                const auto o = f->apply(c, 0, 0);
                for (int k = 0; k < 3; ++k) QVERIFY2(std::abs(o[k] - c[k]) < 0.02, space.c_str());
            }
        }
        const core::AdjustPtr black = core::adjust_function(
            Json{{"kind", "curve"}, {"points", straight},
                 {"native", Json{{"method", "upstream"}, {"space", "cmyk"}, {"channels", Json{{"black", Json::array({Json::array({0, 0}), Json::array({255, 128})})}}}}}},
            page);
        QVERIFY(black->apply({0.8, 0.8, 0.8}, 0, 0)[0] < 0.7);
        QVERIFY(!black->tables());  // (each colour through the conversions, not a table)
        const core::AdjustPtr lift = core::adjust_function(
            Json{{"kind", "curve"}, {"points", Json::array({Json::array({0, 255}), Json::array({255, 0})})},  // (the composite on L only)
                 {"native", Json{{"method", "upstream"}, {"space", "lab"}, {"channels", Json{{"lightness", Json::array({Json::array({0, 51}), Json::array({255, 255})})},
                                                                                             {"a", straight}}}}}},
            page);
        const auto p = lift->apply({0.2, 0.2, 0.2}, 0, 0);
        QVERIFY2(std::abs(p[0] - p[2]) < 0.02, "grey kept");
        // on a CMYK page the records lie on its plates themselves
        const auto cmyk = cmyk_page();
        const core::AdjustPtr on_plates = core::adjust_function(
            Json{{"kind", "levels"}, {"native", Json{{"space", "cmyk"}, {"channels", Json{{"black", Json{{"out_white", 128}}}}}}}}, cmyk.page(0));
        const auto records = on_plates->ink_records();
        QVERIFY(records && records->space == core::InkSpace::Cmyk && records->plates.size() == 4);
        QVERIFY(std::abs(records->plates[3](1.0) - 128 / 255.0) < 0.003);  // (no ink, its brightness 1: K at half)
        QVERIFY(std::abs(records->plates[0](0.4) - 0.4) < 1e-9);
        const core::InkPixels before = inks_shown(cmyk);
        const core::InkPixels after = inks_shown(with_correction(cmyk, Json{{"kind", "levels"}, {"native", Json{{"space", "cmyk"}, {"channels", Json{{"black", Json{{"out_white", 128}}}}}}}}));
        QVERIFY(after.value(0, 3) > before.value(0, 3) + 0.2);  // (K added everywhere)
        QVERIFY(std::abs(after.value(0, 0) - before.value(0, 0)) < 1e-4);
    }

    // levels: the composite, then the channel's record (adjust.rs:211); a tone curve: the channel's, then the composite
    // (216) — in both formulas.
    void compositeAndChannelOrder() {
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        for (const std::string method : {"genko", "upstream"}) {
            const core::AdjustPtr levels = core::adjust_function(
                Json{{"kind", "levels"}, {"black", 0}, {"white", 255}, {"gamma", 1.0}, {"out_black", 100}, {"out_white", 255},
                     {"native", Json{{"method", method}, {"space", "rgb"}, {"channels", Json{{"red", Json{{"black", 50}, {"white", 255}}}}}}}},
                page);
            // composite first: 0 → 100 → (100 − 50)/205; the record first would give 100
            QVERIFY2(std::abs(levels->apply({0, 0, 0}, 0, 0)[0] * 255 - 50.0 * 255 / 205) < 1.0, method.c_str());
            QVERIFY(std::abs(levels->apply({0, 0, 0}, 0, 0)[1] * 255 - 100) < 1.0);
            const core::AdjustPtr curve = core::adjust_function(
                Json{{"kind", "curve"}, {"points", Json::array({Json::array({0, 100}), Json::array({255, 255})})},
                     {"native", Json{{"method", method}, {"space", "rgb"}, {"channels", Json{{"red", Json::array({Json::array({0, 0}), Json::array({255, 128})})}}}}}},
                page);
            // the record first: 0 → 0 → 100; the composite first would give about 50
            QVERIFY2(std::abs(curve->apply({0, 0, 0}, 0, 0)[0] * 255 - 100) < 1.0, method.c_str());
            // (the 8-bit page: the same through its tables)
            const auto t = levels->tables();
            QVERIFY(t && std::abs(int(t->rgb[0][0]) - 62) <= 1);
        }
        // Genko's records are Python's levels_table: a record alone on red is Python's levels with channel r, byte for byte
        const core::AdjustPtr record = core::adjust_function(
            Json{{"kind", "levels"}, {"native", Json{{"method", "genko"}, {"space", "rgb"}, {"channels", Json{{"red", Json{{"black", 30}, {"white", 200}, {"gamma", 1.4}}}}}}}},
            page);
        const core::Adjustment python = core::adjustment("levels", Json{{"black", 30}, {"white", 200}, {"gamma", 1.4}, {"channel", "r"}});
        for (int v = 0; v < 256; ++v) {
            QCOMPARE(int(record->tables()->rgb[0][v]), python.tables[0][v]);
            QCOMPARE(int(record->tables()->rgb[1][v]), v);
        }
    }

    // brightness_contrast (adjust.rs 65-86, 427-451): modern pins black and white and is the identity at 0; legacy is
    // the linear scale about mid-grey; posterize's floor bins (453-458); a gradient map reversed, or dithered within half
    // a level by where the pixel is (723-731).
    void upstreamFormulas() {
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        const auto at = [&page](const Json& adjust, double v, int x = 0, int y = 0) { return core::adjust_function(adjust, page)->apply({v, v, v}, x, y)[0]; };
        const Json zero{{"kind", "brightness_contrast"}, {"native", Json{{"method", "modern"}}}};
        for (const double v : {0.0, 0.1, 0.5, 0.9, 1.0}) QVERIFY(std::abs(at(zero, v) - v) < 1e-12);
        const Json modern{{"kind", "brightness_contrast"}, {"brightness", 80}, {"contrast", 60}, {"native", Json{{"method", "modern"}}}};
        QCOMPARE(at(modern, 0.0), 0.0);
        QCOMPARE(at(modern, 1.0), 1.0);
        QVERIFY(at(modern, 0.4) > 0.45);
        const Json legacy{{"kind", "brightness_contrast"}, {"brightness", 20}, {"contrast", -10}, {"native", Json{{"method", "legacy"}}}};
        QVERIFY(std::abs(at(legacy, 0.5) - (0.5 + 20 / 255.0)) < 1e-12);
        QVERIFY(std::abs(at(legacy, 0.2) - ((0.2 - 0.5) * 0.9 + 0.5 + 20 / 255.0)) < 1e-12);
        const Json floor{{"kind", "posterize"}, {"levels", 3}, {"native", Json{{"method", "floor"}}}};
        QCOMPARE(at(floor, 0.0), 0.0);
        QCOMPARE(at(floor, 100 / 255.0), 127 / 255.0);  // (bin ⌊100·3/256⌋ = 1 → ⌊127.5⌋)
        QCOMPARE(at(floor, 1.0), 1.0);
        QCOMPARE(at(Json{{"kind", "posterize"}, {"levels", 200}, {"native", Json{{"method", "floor"}}}}, 200 / 255.0), 199 / 255.0);  // (past Python's 64)
        const Json reversed{{"kind", "gradient_map"}, {"stops", Json::array({Json::array({0, Json::array({0, 0, 0})}), Json::array({1, Json::array({255, 0, 0})})})},
                            {"native", Json{{"reverse", true}}}};
        QVERIFY(std::abs(at(reversed, 0.0) - 1.0) < 1e-9);
        const Json plain{{"kind", "gradient_map"}};
        const Json dither{{"kind", "gradient_map"}, {"native", Json{{"dither", true}}}};
        const core::AdjustPtr dithered = core::adjust_function(dither, page);
        QVERIFY(dithered->positional());
        bool moved = false;
        for (int i = 0; i < 64; ++i) {
            const double v = i / 63.0;
            const double a = at(plain, v), b = at(dither, v, i % 8, i / 8);
            QVERIFY(std::abs(a - b) <= 0.5 / 255 + 1e-6);
            moved = moved || a != b;
        }
        QVERIFY(moved);
        // the 16-bit page within a level of the 8-bit page, for each (as the three ways take one function)
        for (const Json& adjust : {modern, legacy, floor, reversed, Json{{"kind", "levels"}, {"gamma", 2.0}, {"native", Json{{"method", "upstream"}}}}}) {
            const int worst = worst_difference(shown(with_correction(precise_page(), adjust)), shown(with_correction(eight_bit_page(), adjust)));
            QVERIFY2(worst <= 1, qPrintable(QString::fromStdString(adjust.dump()) + QString(" off by %1").arg(worst)));
        }
    }

    // auto_adjust (edit_cmds.rs 223-301, 563-590): a layer of low contrast stretched to black and white — each channel
    // (tone), all by the same points (contrast: the colours kept), each with its median moved (color); the levels' records
    // said; one step of the history.
    void autoAdjust() {
        const auto ramp = [](std::uint32_t x, std::uint32_t) {
            const double v = 0.3 + 0.4 * x / (kSide - 1.0);
            return std::array<int, 4>{int(std::lround(v * 255)), int(std::lround(v * 0.9 * 255)), int(std::lround(v * 0.8 * 255)), 255};
        };
        auto doc = blank();
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "paint", picture(ramp)));
        for (const std::string mode : {"tone", "contrast", "color"}) {
            const auto applied = run_ops(doc, Json::array({Json{{"op", "auto_adjust"}, {"page", 1}, {"id", "paint"}, {"mode", mode}}}));
            const std::string bytes = render::raster::layer_pixels(applied.doc.page(0), applied.doc.page(0).layers[0]).convert("RGBA").tobytes();
            const auto px = [&bytes](std::uint32_t x, int c) { return int(static_cast<unsigned char>(bytes[(std::size_t(40) * kSide + x) * 4 + c])); };
            // (contrast keeps the colours: only the extreme channels reach 0 and 255)
            const int lo = mode == "contrast" ? px(0, 2) : px(0, 0), hi = px(kSide - 1, 0);
            QVERIFY2(lo < 13 && hi > 242, qPrintable(QString::fromStdString(mode) + QString(" %1 %2").arg(lo).arg(hi)));
            QCOMPARE(applied.results.size(), std::size_t(1));
            const Json levels = applied.results[0]["levels"];
            QVERIFY(levels.contains("red") && levels.contains("green") && levels.contains("blue"));
            if (mode == "contrast") QCOMPARE(levels["red"]["black"], levels["blue"]["black"]);
            if (mode == "color") QVERIFY(levels["red"]["gamma"] != Json(1));
        }
        QVERIFY(refusal(doc, Json{{"op", "auto_adjust"}, {"page", 1}, {"id", "paint"}, {"mode", "nope"}}).starts_with("apply: "));
        // inside the selection only
        const Json area{{"poly", Json::array({Json::array({0, 0}), Json::array({5, 0}), Json::array({5, 25.4}), Json::array({0, 25.4})})}};
        const auto inside = run_ops(doc, Json::array({Json{{"op", "auto_adjust"}, {"page", 1}, {"id", "paint"}, {"mode", "tone"}, {"area", area}}})).doc;
        const std::string a = render::raster::layer_pixels(inside.page(0), inside.page(0).layers[0]).convert("RGBA").tobytes();
        const std::string b = render::raster::layer_pixels(doc.page(0), doc.page(0).layers[0]).convert("RGBA").tobytes();
        QVERIFY(a.substr((40 * kSide + 190) * 4, 4) == b.substr((40 * kSide + 190) * 4, 4));  // (outside: as it was)
        QVERIFY(a.substr((40 * kSide + 10) * 4, 4) != b.substr((40 * kSide + 10) * 4, 4));
        // a precise layer and a page of inks are stretched too
        const auto precise = run_ops(precise_page(), Json::array({Json{{"op", "auto_adjust"}, {"page", 1}, {"id", "paint"}, {"mode", "contrast"}}}));
        QVERIFY(precise.doc.page(0).layers[0].color_raster);
        const auto inks = run_ops(cmyk_page(), Json::array({Json{{"op", "auto_adjust"}, {"page", 1}, {"id", "pic"}, {"mode", "tone"}}}));
        QVERIFY(*inks.doc.page(0).layers.back().ink_raster != *cmyk_page().page(0).layers.back().ink_raster);
    }

    // equalize (extra_cmds.rs 156-187): one CDF over the red, green and blue values, the picture's values spread over the
    // whole range; the same on precise pixels (within a level) and on a page of inks.
    void equalize() {
        const auto narrow = [](std::uint32_t x, std::uint32_t y) {
            const int v = 100 + int((x + y) % 50);
            return std::array<int, 4>{v, v, v, 255};
        };
        auto doc = blank();
        doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "paint", picture(narrow)));
        const auto done = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "equalize"}});
        const std::string bytes = render::raster::layer_pixels(done.page(0), done.page(0).layers[0]).convert("RGBA").tobytes();
        int lo = 255, hi = 0;
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            lo = std::min(lo, int(static_cast<unsigned char>(bytes[i])));
            hi = std::max(hi, int(static_cast<unsigned char>(bytes[i])));
        }
        QVERIFY2(lo == 0 && hi == 255, qPrintable(QString("%1..%2").arg(lo).arg(hi)));
        QVERIFY(refusal(precise_page(), Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "equalize"}}).empty());
        QVERIFY(refusal(cmyk_page(), Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "equalize"}}).empty());
        // with a selection: what is inside counted and changed, what is outside as it was
        const Json area{{"poly", Json::array({Json::array({0, 0}), Json::array({10, 0}), Json::array({10, 10}), Json::array({0, 10})})}};
        const auto part = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "equalize"}, {"area", area}});
        const std::string inside = render::raster::layer_pixels(part.page(0), part.page(0).layers[0]).convert("RGBA").tobytes();
        const std::string before = render::raster::layer_pixels(doc.page(0), doc.page(0).layers[0]).convert("RGBA").tobytes();
        QVERIFY(inside.substr((150 * kSide + 150) * 4, 4) == before.substr((150 * kSide + 150) * 4, 4));
    }

    // set_adjustment (PhotoCraft's layer.setAdjustment, adjust_params.rs 753-772): the settings given laid over the
    // correction's (its native's records too), none given: back to the defaults (the blend, the formula and the
    // records' space kept, the records not); bad settings refused and the layer left; one step of the history; not on
    // another layer.
    void setAdjustment() {
        const auto doc = run(eight_bit_page(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                    {"adjust", Json{{"kind", "levels"}, {"black", 20}, {"native", Json{{"method", "upstream"}, {"blend", "screen"},
                                                                                                                         {"channels", Json{{"red", Json{{"gamma", 1.5}}}}}}}}}});
        const auto merged = run_ops(doc, Json::array({Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}, {"white", 200},
                                                           {"native", Json{{"channels", Json{{"blue", Json{{"black", 10}}}}}}}}}));
        const Json& now = *correction(merged.doc).adjust;
        QCOMPARE(now["black"], Json(20));
        QCOMPARE(now["white"], Json(200));
        QCOMPARE(now["native"]["blend"], Json("screen"));
        QCOMPARE(now["native"]["channels"]["red"]["gamma"], Json(1.5));
        QCOMPARE(now["native"]["channels"]["blue"]["black"], Json(10));
        QCOMPARE(merged.results[0]["adjust"], now);
        const auto reset = run(merged.doc, Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}});
        const Json& back = *correction(reset).adjust;
        QVERIFY2(back == (Json{{"kind", "levels"}, {"black", 0}, {"white", 255}, {"native", Json{{"blend", "screen"}, {"method", "upstream"}, {"space", "rgb"}}}}),
                 back.dump().c_str());
        for (const Json& bad : {Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}, {"native", Json{{"method", "nope"}}}},
                                Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}, {"black", "dark"}},
                                Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}, {"kind", "invert"}},
                                Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "paint"}, {"black", 3}},
                                Json{{"op", "set_adjustment"}, {"page", 1}}}) {
            QVERIFY2(refusal(merged.doc, bad).starts_with("apply: "), bad.dump().c_str());
        }
        // a kind of the C++ build's own: its normal form (a look chosen)
        const auto lookup = run(eight_bit_page(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "lut"}, {"adjust", Json{{"kind", "color_lookup"}}}});
        const auto looked = run(lookup, Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "lut"}, {"look", "warm"}, {"dither", true}});
        QCOMPARE(*correction(looked).adjust, (Json{{"kind", "color_lookup"}, {"look", "warm"}, {"interpolation", "trilinear"}, {"dither", true}}));
    }

    // color_lookup (design 2.4): a file's text made into the book's table (its asset, the same table the same ref), drawn
    // trilinear or tetrahedral, kept on saving and reopening (a missing or broken table: read-only), a table of another
    // book refused; the eight looks; a filter of it.
    void colorLookup() {
        QTemporaryDir tmp;
        // an inverting 2³ table
        std::string cube = "TITLE \"inv\"\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
        const auto doc = eight_bit_page();
        const auto made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                        {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}, {"file_name", "/x/inv.cube"}}}});
        const Json& adjust = *correction(made).adjust;
        QVERIFY(adjust["lut"].get<std::string>().starts_with("sha256:"));
        QCOMPARE(adjust["size"], Json(2));
        QCOMPARE(adjust["name"], Json("inv.cube"));
        QVERIFY(!adjust.contains("data") && !adjust.contains("file_name"));
        QVERIFY(made.luts.contains(adjust["lut"].get<std::string>()));
        QVERIFY(worst_difference(shown(made), shown(with_correction(doc, Json{{"kind", "invert"}}))) <= 1);
        // saved as its asset and read back; the book names adjustments@1
        const Json payload = payload_of(made, tmp);
        QVERIFY(names(payload, core::kAdjustmentsFeature));
        const std::string ref = adjust["lut"].get<std::string>();
        QVERIFY(std::filesystem::exists(std::filesystem::path(tmp.path().toStdString()) / "assets" / ref.substr(7, 2) / (ref.substr(7) + ".gklut")));
        const auto loaded = storage::load_document_payload(payload, std::filesystem::path(tmp.path().toStdString()));
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(loaded.document.luts.contains(ref));
        QVERIFY(shown(loaded.document) == shown(made));
        // a table missing: read-only
        {
            QTemporaryDir other;
            Json bad = payload;
            bad["pages"][0]["layers"].back()["adjust"]["lut"] = "sha256:" + std::string(64, 'a');
            const auto broken = storage::load_document_payload(bad, std::filesystem::path(tmp.path().toStdString()));
            QVERIFY(!broken.document.read_only_reason.empty());
        }
        // a table of another book: refused (its text must be given)
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                  {"adjust", Json{{"kind", "color_lookup"}, {"lut", "sha256:" + std::string(64, 'b')}, {"size", 2}}}}).starts_with("apply: "));
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                  {"adjust", Json{{"kind", "color_lookup"}, {"data", "garbage"}}}}).starts_with("apply: "));
        // the looks, both interpolations; tetrahedral and trilinear agree on a grid's points
        for (const auto& look : core::lut3d::looks()) {
            for (const std::string way : {"trilinear", "tetrahedral"}) {
                const auto with = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                                {"adjust", Json{{"kind", "color_lookup"}, {"look", std::string(look.id)}, {"interpolation", way}}}});
                QVERIFY(shown(with) != shown(doc));
            }
        }
        // a filter: the same colours as the layer over it
        const auto filtered = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "color_lookup"}, {"data", cube}});
        QVERIFY(worst_difference(shown(filtered), shown(made)) <= 1);
        QVERIFY(filtered.luts.empty());  // (a filter's table is not kept)
    }

    // The PSD's adjustment layers of the tone kinds (the user's decision 5): each written and read back as the same
    // correction — levels' and a curve's records in the PSD's mode, brightness_contrast with its CgEd, posterize's
    // floor, threshold, invert, exposure, a gradient map's reverse and dither, a lookup's table —; Python's forms by the
    // nearest, said in the notes.
    void psdToneCodecs() {
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        std::map<std::string, core::Bytes> tables;  // (the lookup tables read: import_psd takes them into the book)
        const auto round_trip = [&page, &tables](const Json& adjust, std::string_view mode = "rgb", const core::Document* book = nullptr) {
            std::vector<std::string> notes;
            const auto block = render::psd::write_adjustment(adjust, page, notes, book);
            if (!block) return Json();
            render::psd::AdjustContext context;
            context.mode = mode;
            context.companion = block->companion;
            context.tables = &tables;
            const auto read = render::psd::read_adjustment(block->key, block->payload, notes, context);
            return read ? render::psd::imported_adjust(*read, "normal", page) : Json();
        };
        const Json levels{{"kind", "levels"}, {"black", 10}, {"white", 240}, {"gamma", 1.2}, {"out_black", 5}, {"out_white", 250},
                          {"native", Json{{"method", "upstream"}, {"space", "rgb"}, {"channels", Json{{"red", Json{{"black", 3}, {"white", 255}, {"gamma", 0.8}, {"out_black", 0}, {"out_white", 255}}}}}}}};
        QCOMPARE(round_trip(levels), levels);
        const Json curve{{"kind", "curve"}, {"points", Json::array({Json::array({0, 0}), Json::array({128, 150}), Json::array({255, 255})})},
                         {"native", Json{{"method", "upstream"}, {"space", "rgb"}, {"channels", Json{{"blue", Json::array({Json::array({0, 20}), Json::array({255, 235})})}}}}}};
        QCOMPARE(round_trip(curve), curve);
        for (const Json& same : {Json{{"kind", "brightness_contrast"}, {"brightness", 20}, {"contrast", -10}, {"native", Json{{"method", "legacy"}}}},
                                 Json{{"kind", "brightness_contrast"}, {"brightness", -40}, {"contrast", 60}, {"native", Json{{"method", "modern"}}}},
                                 Json{{"kind", "posterize"}, {"levels", 7}, {"native", Json{{"method", "floor"}}}}, Json{{"kind", "threshold"}, {"threshold", 100}},
                                 Json{{"kind", "invert"}}, Json{{"kind", "exposure"}, {"exposure", 1.5}, {"offset", 0.125}, {"gamma", 1.25}},
                                 Json{{"kind", "gradient_map"},
                                      {"stops", Json::array({Json::array({0, Json::array({0, 0, 0})}), Json::array({0.5, Json::array({255, 0, 0})}), Json::array({1, Json::array({255, 255, 255})})})},
                                      {"native", Json{{"reverse", true}, {"dither", true}}}}}) {
            QCOMPARE(round_trip(same), same);
        }
        // a CMYK document's ink records
        const auto cmyk = cmyk_page();
        std::vector<std::string> notes;
        const Json ink_curve{{"kind", "curve"}, {"points", Json::array({Json::array({0, 0}), Json::array({255, 255})})},
                             {"native", Json{{"method", "upstream"}, {"space", "cmyk"}, {"channels", Json{{"black", Json::array({Json::array({0, 0}), Json::array({255, 180})})}}}}}};
        const auto block = render::psd::write_adjustment(ink_curve, cmyk.page(0), notes);
        QVERIFY(block);
        render::psd::AdjustContext context;
        context.mode = "cmyk";
        QCOMPARE(render::psd::imported_adjust(*render::psd::read_adjustment(block->key, block->payload, notes, context), "normal", cmyk.page(0)), ink_curve);
        // a lookup's table (the book's), and a look written as its table
        const auto lut = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", Json{{"kind", "color_lookup"}, {"look", "cool"}, {"dither", true}}}});
        const Json back = round_trip(*correction(lut).adjust, "rgb", &lut);
        QCOMPARE(back["name"], Json("Cool Filter"));
        QCOMPARE(back["dither"], Json(true));
        QVERIFY(tables.contains(back["lut"].get<std::string>()));
        const auto table = core::lut3d::decoded(back["lut"].get<std::string>(), tables[back["lut"].get<std::string>()]);
        QCOMPARE(table->data.size(), core::lut3d::builtin("cool")->data.size());
        for (std::size_t i = 0; i < table->data.size(); ++i) QVERIFY(std::abs(table->data[i] - core::lut3d::builtin("cool")->data[i]) < 1e-6);  // (6 decimals)
        // Python's forms: the nearest, said
        notes.clear();
        QVERIFY(render::psd::write_adjustment(Json{{"kind", "brightness_contrast"}, {"brightness", 30}}, page, notes));
        QVERIFY(!notes.empty());
        notes.clear();
        QVERIFY(render::psd::write_adjustment(Json{{"kind", "bitonal"}}, page, notes));
        QVERIFY(!notes.empty());
        // the companion block written after brit
        notes.clear();
        const auto layer = formats::psd_adjust_layer(correction(with_correction(doc, Json{{"kind", "brightness_contrast"}, {"native", Json{{"method", "modern"}}}})), page,
                                                     render::Size{200, 200}, notes);
        QVERIFY(layer && layer->blocks.find("8BIMbrit") == 0 && layer->blocks.find("8BIMCgEd") != std::string::npos);
    }

    // Python's 色相・彩度・明度 (hue: HSV, its hue turned, its saturation and value multiplied) as the nearest PSD
    // adjustment, hue2 (the user's decision 5: always written; design 4.4): its turn as the hue (−180..180), its factors
    // as the saturation and the lightness ((f − 1)·100), said in the notes; read back as hue_saturation (a PSD's hue2 is
    // the upstream form). Python's defaults (a turn of 30°); one Python cannot read is not written (said); a layer of it
    // is an adjustment layer of the PSD.
    void psdPythonHue() {
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        std::vector<std::string> notes;
        const auto block = render::psd::write_adjustment(Json{{"kind", "hue"}, {"shift", 200}, {"saturation", 1.5}, {"value", 0.6}}, page, notes);
        QVERIFY(block);
        QCOMPARE(block->key, std::string("hue2"));
        QVERIFY(!notes.empty());
        const auto read = render::psd::read_adjustment(block->key, block->payload, notes);
        QVERIFY(read);
        QCOMPARE((*read)["kind"], Json("hue_saturation"));
        QCOMPARE((*read)["hue"], Json(-160));
        QCOMPARE((*read)["saturation"], Json(50));
        QCOMPARE((*read)["lightness"], Json(-40));
        QCOMPARE((*read)["colorize"], Json(false));
        const auto plain = render::psd::write_adjustment(Json{{"kind", "hue"}}, page, notes);
        QVERIFY(plain);
        const auto plain_read = render::psd::read_adjustment(plain->key, plain->payload, notes);
        QVERIFY(plain_read);
        QCOMPARE((*plain_read)["hue"], Json(30));
        QCOMPARE((*plain_read)["saturation"], Json(0));
        QCOMPARE((*plain_read)["lightness"], Json(0));
        // (a turn of −90: 270 as Python takes it, −90 as Photoshop's)
        const auto turned = render::psd::write_adjustment(Json{{"kind", "hue"}, {"shift", -90}, {"saturation", 3}, {"value", 2.5}}, page, notes);
        QVERIFY(turned);
        const auto turned_read = render::psd::read_adjustment(turned->key, turned->payload, notes);
        QVERIFY(turned_read);
        QCOMPARE((*turned_read)["hue"], Json(-90));
        QCOMPARE((*turned_read)["saturation"], Json(100));
        QCOMPARE((*turned_read)["lightness"], Json(100));
        notes.clear();
        QVERIFY(!render::psd::write_adjustment(Json{{"kind", "hue"}, {"shift", "x"}}, page, notes));
        QVERIFY(!notes.empty());
        const auto layered = with_correction(doc, Json{{"kind", "hue"}, {"shift", 90}});
        const auto made = formats::psd_adjust_layer(correction(layered), layered.page(0), render::Size{200, 200}, notes);
        QVERIFY(made);
        QVERIFY(made->blocks.starts_with("8BIMhue2"));
    }

    // The notes of the PSD's adjustment layers are the person's words (Japanese, plain): what is not read — a block cut
    // short, of a version or a kind this build does not read, Python's view — and what is written otherwise or not at all
    // (its settings cannot be read), the corrections named by their names (レベル補正, 自然な彩度 …), not their keys.
    void psdNotesInJapanese() {
        std::vector<std::string> notes;
        for (const char* key : {"levl", "curv", "brit", "post", "thrs", "expA", "grdm", "clrL", "hue2", "vibA", "blnc", "blwh", "phfl", "mixr",
                                "selc", "SoCo", "abcd"}) {
            const std::size_t before = notes.size();
            QVERIFY2(!render::psd::read_adjustment(key, "", notes), key);
            QVERIFY2(notes.size() > before, key);
        }
        {
            const render::psd::PythonView python;
            QVERIFY(!render::psd::read_adjustment("levl", "", notes));
        }
        const auto doc = eight_bit_page();
        const core::Page& page = doc.page(0);
        for (const Json& adjust : {Json{{"kind", "levels"}, {"gamma", 2.0}}, Json{{"kind", "brightness_contrast"}, {"brightness", 30}},
                                   Json{{"kind", "bitonal"}}, Json{{"kind", "posterize"}, {"levels", 5}}, Json{{"kind", "hue"}, {"shift", 40}},
                                   Json{{"kind", "levels"}, {"black", "x"}}, Json{{"kind", "curve"}, {"points", "x"}},
                                   Json{{"kind", "gradient_map"}, {"stops", "x"}}, Json{{"kind", "color_lookup"}, {"lut", "sha256:" + std::string(64, '0')}},
                                   Json{{"kind", "vibrance"}, {"vibrance", "x"}}, Json{{"kind", "hue_saturation"}, {"hue", "x"}}}) {
            const std::size_t before = notes.size();
            (void)render::psd::write_adjustment(adjust, page, notes);
            QVERIFY2(notes.size() > before, adjust.dump().c_str());
        }
        const QRegularExpression japanese(QStringLiteral("[\\x{3041}-\\x{30ff}\\x{4e00}-\\x{9fff}]"));
        const QRegularExpression english(QStringLiteral("\\b(the|is|are|not|cannot|this|build|adjustment|read|view)\\b"));
        const QRegularExpression key_word(QStringLiteral("\\b[a-z]+_[a-z_]+\\b"));  // (a kind's key: hue_saturation)
        for (const std::string& note : notes) {
            const QString text = QString::fromStdString(note);
            QVERIFY2(text.contains(japanese), note.c_str());
            QVERIFY2(!text.contains(english), note.c_str());
            QVERIFY2(!text.contains(key_word), note.c_str());
        }
        const auto said = [&notes](const char* words) {
            return std::any_of(notes.begin(), notes.end(), [words](const std::string& n) { return n.find(words) != std::string::npos; });
        };
        QVERIFY(said("レベル補正"));
        QVERIFY(said("自然な彩度"));
        QVERIFY(said("色相・彩度"));
    }

    // The .cube of a page's corrections (file.export.colorLookupTables): an identity lattice through each visible
    // correction layer in order, by its opacity and blend.
    void cubeExport() {
        const auto doc = with_correction(eight_bit_page(), Json{{"kind", "invert"}}, 0.5);
        const std::string cube = formats::bake_cube(doc, doc.page(0), 3, "t");
        const core::lut3d::Table t = core::lut3d::parse_cube(cube);
        QCOMPARE(t.size, std::size_t(3));
        QCOMPARE(QString::fromStdString(t.title), QStringLiteral("t"));
        for (const float v : t.data) QVERIFY(std::abs(v - 0.5f) < 1e-5);  // (half inverted: every colour at mid-grey)
        const auto inverted = with_correction(eight_bit_page(), Json{{"kind", "invert"}});
        const core::lut3d::Table full = core::lut3d::parse_cube(formats::bake_cube(inverted, inverted.page(0), 2, "x"));
        QCOMPARE(full.data[0], 1.0f);  // (black → white)
        QCOMPARE(full.data[3], 0.0f);  // (red → cyan)
        const core::lut3d::Table nine = core::lut3d::parse_cube(formats::bake_cube(inverted, inverted.page(0), 9, "x"));
        for (std::size_t i = 0; i < 9 * 9 * 9; ++i) {  // (every colour of a larger lattice: red fastest, then green, blue)
            const std::size_t at[3] = {i % 9, (i / 9) % 9, i / 81};
            for (std::size_t c = 0; c < 3; ++c) QVERIFY(std::abs(nine.data[i * 3 + c] - (1.0f - static_cast<float>(at[c]) / 8.0f)) < 1e-5f);
        }
    }

    // A correction's lookup table is the book's own: drawn only from the book (not from a table another book's op made
    // in this process), and no op leaves a correction whose table the book does not hold (it could not be written);
    // taking that correction away is allowed.
    void lookupTableOfTheBook() {
        std::string cube = "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
        const auto doc = eight_bit_page();
        const auto made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                        {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}, {"file_name", "inv.cube"}}}});
        QVERIFY(shown(made) != shown(doc));
        // the same correction in a book without its table: it is not drawn with the other book's
        const auto other = with_correction(doc, *correction(made).adjust);
        QVERIFY(other.luts.empty());
        QVERIFY(shown(other) == shown(doc));
        QVERIFY(refusal(other, Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"name", "x"}}).starts_with("apply: "));
        QVERIFY(refusal(other, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "adj"}}).empty());
    }

    // A PSD whose only layer records are adjustment layers, its picture in the merged image only: the merged picture as
    // one layer, as Python reads it (psd.read_psd: no layers, the adjustment layer skipped) — the adjustment layer is
    // not laid over it again (the merged picture shows it already).
    void psdOfAdjustmentLayersOnly() {
        const auto be16 = [](std::string& out, unsigned v) {
            out += static_cast<char>((v >> 8) & 0xff);
            out += static_cast<char>(v & 0xff);
        };
        const auto be32 = [](std::string& out, std::uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xff);
        };
        constexpr std::uint32_t w = 40, h = 30;
        std::string psd = "8BPS";
        be16(psd, 1);
        psd += std::string(6, '\0');
        be16(psd, 3);
        be32(psd, h);
        be32(psd, w);
        be16(psd, 8);
        be16(psd, 3);
        be32(psd, 0);  // (colour mode data)
        be32(psd, 0);  // (image resources)
        std::string rec;
        for (int i = 0; i < 4; ++i) be32(rec, 0);  // (top, left, bottom, right)
        be16(rec, 4);
        for (const int c : {-1, 0, 1, 2}) {
            be16(rec, static_cast<unsigned>(c) & 0xffff);
            be32(rec, 2);
        }
        rec += "8BIMnorm";
        rec += std::string{static_cast<char>(255), 0, 0, 0};
        std::string extra;
        be32(extra, 0);  // (no mask)
        be32(extra, 0);  // (no blending ranges)
        extra += "\x03inv";
        extra += "8BIMnvrt";
        be32(extra, 0);
        be32(rec, static_cast<std::uint32_t>(extra.size()));
        rec += extra;
        std::string info;
        be16(info, 1);
        info += rec;
        for (int i = 0; i < 4; ++i) be16(info, 0);  // (each channel: raw, nothing)
        if (info.size() % 2 != 0) info += '\0';
        std::string layer_info;
        be32(layer_info, static_cast<std::uint32_t>(info.size()));
        layer_info += info;
        be32(layer_info, 0);  // (global mask)
        be32(psd, static_cast<std::uint32_t>(layer_info.size()));
        psd += layer_info;
        be16(psd, 0);
        psd += std::string(w * h, static_cast<char>(200)) + std::string(w * h, static_cast<char>(120)) + std::string(w * h, static_cast<char>(60));
        QTemporaryDir tmp;
        const std::string file = tmp.path().toStdString() + "/only.psd";
        genko::test::write_bytes(QString::fromStdString(file), psd);
        const auto done = run_ops(blank(), Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", file}, {"id", "psd"}}}));
        const auto& layers = done.doc.page(0).layers;
        QCOMPARE(layers.size(), std::size_t(1));
        QCOMPARE(layers[0].kind, core::LayerKind::Raster);
        QCOMPARE(layers[0].title, std::string("PSD"));
        QCOMPARE(done.results[0]["skipped"], Json::array({"inv"}));
        const std::string pixels = render::raster::layer_pixels(done.doc.page(0), layers[0]).convert("RGBA").tobytes();
        bool found = false;
        for (std::size_t i = 0; i + 3 < pixels.size() && !found; i += 4) {
            if (static_cast<unsigned char>(pixels[i + 3]) != 255) continue;
            found = true;
            QCOMPARE(static_cast<int>(static_cast<unsigned char>(pixels[i])), 200);
            QCOMPARE(static_cast<int>(static_cast<unsigned char>(pixels[i + 1])), 120);
            QCOMPARE(static_cast<int>(static_cast<unsigned char>(pixels[i + 2])), 60);
        }
        QVERIFY(found);
    }

    // A LUT file's numbers and a .cube's are C's whatever the locale (the app and the command line run in the
    // environment's: a German or French one has a comma for the decimal point).
    void lookupNumbersWhateverTheLocale() {
        const genko::test::CommaLocale comma;
        QVERIFY2(comma.ok(), "no locale with a comma for the decimal point could be set (de_DE, fr_FR, or made by localedef)");
        std::string cube = "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(1 - r) + ".0000 " + std::to_string(1 - g) + ".0000 " + std::to_string(1 - b) + ".0000\n";
        const auto doc = eight_bit_page();
        const auto made = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                         {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}}}}}));
        QCOMPARE((*correction(made.doc).adjust)["size"], Json(2));
        const auto half = with_correction(eight_bit_page(), Json{{"kind", "invert"}}, 0.5);
        const std::string baked = formats::bake_cube(half, half.page(0), 2, "t");
        QVERIFY2(baked.find(',') == std::string::npos, baked.c_str());
        QVERIFY2(baked.find("\n0.500000 0.500000 0.500000\n") != std::string::npos, baked.c_str());
    }

    // The journal keeps a LUT file's text an op gave as its length (the table is the book's asset; the op is not done
    // again from the journal), as a picture's base64.
    void journalLeavesTheFileOut() {
        std::string cube = "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + "\n";
        const std::string said = "<" + std::to_string(cube.size()) + " chars>";
        const auto done = run_ops(eight_bit_page(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                                      {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}}}},
                                                                 Json{{"op", "set_adjustment"}, {"page", 1}, {"id", "adj"}, {"data", cube}},
                                                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "adj"}, {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}}}},
                                                                 Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "color_lookup"}, {"data", cube}}}));
        QCOMPARE(done.journal_ops[0]["adjust"]["data"], Json(said));
        QCOMPARE(done.journal_ops[1]["data"], Json(said));
        QCOMPARE(done.journal_ops[2]["adjust"]["data"], Json(said));
        QCOMPARE(done.journal_ops[3]["data"], Json(said));
        QCOMPARE(done.journal_ops[0]["adjust"]["kind"], Json("color_lookup"));
    }

    // The largest table: a .cube of 129³ with six decimals (what a table holds at most) read from an op's text.
    void largestTableRead() {
        std::string cube = "LUT_3D_SIZE 129\n";
        cube.reserve(129 * 129 * 129 * 27 + 32);
        char line[64];
        for (int b = 0; b < 129; ++b)
            for (int g = 0; g < 129; ++g)
                for (int r = 0; r < 129; ++r) {
                    const int n = std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", r / 128.0, g / 128.0, b / 128.0);
                    cube.append(line, static_cast<std::size_t>(n));
                }
        QVERIFY(cube.size() > std::size_t(32) * 1024 * 1024);
        const auto made = run_ops(eight_bit_page(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                                      {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}}}}}));
        QCOMPARE((*correction(made.doc).adjust)["size"], Json(129));
    }

    // The largest .cube written: 256 (each place of the lattice where it is, a dither's too) — inverted, then a dither of
    // PhotoCraft's (a lookup of no table): each entry 1 − v, then ± ½ level by its place.
    void largestCubeWritten() {
        auto doc = with_correction(eight_bit_page(), Json{{"kind", "invert"}});
        core::Layer dither;
        dither.id = "dither";
        dither.role = core::LayerRole::User;
        dither.kind = core::LayerKind::Adjust;
        dither.adjust = Json{{"kind", "color_lookup"}, {"interpolation", "trilinear"}, {"dither", true}};
        doc.edit_page(0).layers.push_back(std::move(dither));
        const std::string cube = formats::bake_cube(doc, doc.page(0), 256, "big");
        // (read here line by line: the reader takes tables of at most 129, as PhotoCraft's)
        const std::size_t header = cube.find("DOMAIN_MAX 1.0 1.0 1.0\n");
        QVERIFY(cube.find("\nLUT_3D_SIZE 256\n") != std::string::npos && header != std::string::npos);
        std::vector<std::array<std::size_t, 3>> wanted{{0, 0, 0}, {3, 77, 130}, {128, 1, 254}, {250, 200, 255}, {255, 255, 255}};
        std::size_t at_line = 0, next = 0, start = header + std::strlen("DOMAIN_MAX 1.0 1.0 1.0\n");
        while (start < cube.size()) {
            const std::size_t end = cube.find('\n', start);
            QVERIFY(end != std::string::npos);
            if (next < wanted.size()) {
                const auto& at = wanted[next];
                if (at_line == (at[2] * 256 + at[1]) * 256 + at[0]) {
                    const std::string line = cube.substr(start, end - start);
                    std::array<double, 3> got{};
                    QVERIFY2(std::sscanf(line.c_str(), "%lf %lf %lf", &got[0], &got[1], &got[2]) == 3, line.c_str());
                    const double d = core::lut3d::bayer4(static_cast<int>(at[2] * 256 + at[0]), static_cast<int>(at[1])) / 255.0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const double want = std::clamp(1.0 - static_cast<double>(at[c]) / 255.0 + d, 0.0, 1.0);
                        QVERIFY2(std::abs(got[c] - want) < 2e-6, qPrintable(QStringLiteral("%1,%2,%3.%4: %5 / %6").arg(at[0]).arg(at[1]).arg(at[2]).arg(c).arg(got[c]).arg(want)));
                    }
                    ++next;
                }
            }
            ++at_line;
            start = end + 1;
        }
        QCOMPARE(at_line, std::size_t(256) * 256 * 256);
        QCOMPARE(next, wanted.size());
    }

    // Channel records in another space than the page's: each colour through the conversions once, however many pixels
    // have it (a page of few colours is not converted pixel by pixel).
    void recordsInAnotherSpaceOncePerColour() {
        int into = 0, back = 0;
        core::set_tone_bridge([&into, &back](const core::Page&, std::string_view) {
            core::ToneBridge bridge;
            bridge.into = [&into](const std::array<double, 3>& rgb) {
                ++into;
                return std::vector<double>{rgb[0], rgb[1], rgb[2], 1.0};
            };
            bridge.back = [&back](const std::vector<double>& tones) {
                ++back;
                return std::array<double, 3>{tones[0], tones[1], tones[2]};
            };
            return bridge;
        });
        const auto doc = eight_bit_page();
        const core::AdjustPtr f = core::adjust_function(
            Json{{"kind", "levels"}, {"native", Json{{"method", "upstream"}, {"space", "cmyk"}, {"channels", Json{{"cyan", Json{{"out_white", 128}}}}}}}}, doc.page(0));
        render::adjust::install_tone_bridge();  // (the drawing's own again)
        QVERIFY(f);
        const std::array<std::array<double, 3>, 3> few{{{0.25, 0.5, 0.75}, {0.1, 0.2, 0.3}, {1.0, 1.0, 1.0}}};
        std::array<std::array<double, 3>, 3> first{};
        for (int i = 0; i < 3000; ++i) {
            const auto out = f->apply(few[std::size_t(i % 3)], i % 50, i / 50);
            if (i < 3) first[std::size_t(i)] = out;
            QVERIFY(out == first[std::size_t(i % 3)]);
        }
        QVERIFY(std::abs(first[0][0] - 0.25 * 128 / 255) < 1e-9);  // (cyan's brightness: its record)
        QVERIFY2(into <= 3 && back <= 6, qPrintable(QStringLiteral("%1 into, %2 back").arg(into).arg(back)));
    }
};

QTEST_GUILESS_MAIN(TestAdjust)
#include "test_adjust.moc"
