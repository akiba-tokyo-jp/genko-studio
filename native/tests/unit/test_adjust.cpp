// PC2-0 (補正の土台, SPEC.md COLOR-04): the corrections' common ground — Python's form kept as it is (its JSON, its tables,
// its pixels, no feature named), a `native` read, normalized and stored under adjustments@1 (what is not well formed
// opens read-only), the formula's choice (native.method), the kinds' table and the ops and filter kinds not in this
// build yet (refused, named), the three ways a correction is drawn through one AdjustFunction, native.blend on each
// (the 8-bit page: blend_over's bytes; 16 bits: the 8-bit page's within a level; CMYK: the plates multiplied), a
// correction layer's own blend not looked at (the user's decision 2), and the PSD's frame.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/base64.hpp"
#include "core/color_adjust.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/ink.hpp"
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

    // The user's decision 4: native.method — genko (Python's formula, the default: the same bytes) or upstream (PC2a's
    // formula: refused, named, until it is in this build); a stored upstream layer is well formed and left out of a
    // preview, never drawn as if it were not there.
    void methodChoice() {
        const auto doc = eight_bit_page();
        for (const std::string kind : {"levels", "curve"}) {
            const Json genko{{"kind", kind}, {"native", Json{{"method", "genko"}}}};
            QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", genko}}).empty());
            const std::string why = refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                                      {"adjust", Json{{"kind", kind}, {"native", Json{{"method", "upstream"}}}}}});
            QVERIFY2(why.starts_with("not_yet_ported: ") && why.find(kind) != std::string::npos && why.find("PC2a") != std::string::npos, why.c_str());
        }
        for (const Json& later : {Json{{"kind", "brightness_contrast"}, {"native", Json{{"method", "modern"}}}},
                                  Json{{"kind", "posterize"}, {"native", Json{{"method", "floor"}}}},
                                  Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", true}}}}}) {
            QVERIFY2(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"adjust", later}}).starts_with("not_yet_ported: "), later.dump().c_str());
        }
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"},
                                  {"adjust", Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", false}, {"dither", false}}}}}}).empty());
        const auto upstream = with_correction(doc, Json{{"kind", "levels"}, {"black", 20}, {"native", Json{{"method", "upstream"}}}});
        core::validate_adjustments(upstream);  // (well formed)
        QVERIFY_EXCEPTION_THROWN(render::render_page(upstream.page(0), 200, render::proof_options(), &upstream), render::NotYetPorted);
        auto preview = render::proof_options();
        preview.skip_unported = true;
        const auto left_out = render::render_page(upstream.page(0), 200, preview, &upstream);
        QCOMPARE(left_out.omitted, std::vector<std::string>{"adjust:levels"});
        QVERIFY(left_out.image.tobytes() == shown(doc));
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
            QVERIFY(kind.group == "Python" || kind.group == "PC2a" || kind.group == "PC2b" || kind.group == "PC2c");
            QVERIFY(kind.filter);
            QVERIFY(!kind.native.empty() && kind.native.front().key == "blend");
            if (kind.group == "Python") python.emplace_back(kind.name);
        }
        QCOMPARE(python, std::vector<std::string>(core::kAdjustments.begin(), core::kAdjustments.end()));
        QCOMPARE(kinds.size(), std::size_t(10 + 8 + 2 + 3));  // (Python's, PC2b's eight, PC2a's two, PC2c's three)
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
        const std::vector<const char*> not_yet{
            "set_adjustment",  // PC2a
            "auto_adjust",     // PC2a
            "hdr_toning",      // PC2c
            "apply_image",     // PC2c
            "calculations",    // PC2c
            "trap",            // PC2c
            "fade_layer",      // PC2c
        };
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
        QCOMPARE(codecs.size(), std::size_t(16));
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
};

QTEST_GUILESS_MAIN(TestAdjust)
#include "test_adjust.moc"
