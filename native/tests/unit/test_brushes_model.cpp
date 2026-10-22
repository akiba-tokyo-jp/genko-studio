// PC4 E-0 (brushes@1, schema-v4.md §3, SPEC.md COLOR-08): the dab brush's settings as a book keeps them (only those that
// differ from the defaults; each refused setting), a brush of one's own with them (brush_to_dict, brush_from_dict,
// define_brush leaving "engine" and "dabs" to E1), a line's inputs (tilt, time_ms, wheel, flow) packed and read, the
// brush state's kind and background_rgb, a book that has them written with brushes@1 and its pictures and read back the
// same (and its pictures kept by gc), a book that has none written as before (no feature, the same keys), and what makes
// a book read-only. A dab brush's line is not drawn yet (not_yet_ported until E1).

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

std::string grey_png(int w, int h, int value = 128) {
    return render::write_png(render::Image::frombytes("L", render::Size{w, h}, std::string(std::size_t(w) * h, static_cast<char>(value))));
}

std::string ref_of(const std::string& bytes) { return storage::AssetStore::ref(bytes); }

core::Document book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("brushes", core::Num(1), 1, spec, core::Binding::Right);
}

core::StrokePtr line(int points) {
    core::Stroke s = core::coerce_stroke(core::PenPoints{});
    for (int i = 0; i < points; ++i) s.points.push_back(core::PointF{10.0 + i, 20.0 + i * 0.5});
    s.width_mm = 0.5;
    return std::make_shared<const core::Stroke>(s);
}

void add_line(core::Document& doc, core::Stroke s) {
    core::Layer& ink = doc.edit_page(0).layer_for(core::LayerRole::Ink);
    std::vector<core::StrokePtr> items = ink.strokes->items;
    items.push_back(std::make_shared<const core::Stroke>(std::move(s)));
    ink.strokes = core::make_strokes(std::move(items));
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

Json project(const fs::path& dir) { return genko::test::read_json(QString::fromStdString((dir / "project.json").string())); }

std::string error_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

// A dab brush's settings other than the defaults, in every section.
core::DabSettings full(const std::string& tip, const std::string& texture) {
    core::DabSettings s;
    s.size_mm = 12.5;
    s.hardness = 0.4;
    s.spacing = 0.3;
    s.angle = -30;
    s.roundness = 0.5;
    s.flip_x = true;
    s.aliased = true;
    s.opacity = 0.8;
    s.flow = 0.25;
    s.pressure_size = false;
    s.pressure_opacity = true;
    s.erase = true;
    s.tip = tip;
    s.shape.enabled = true;
    s.shape.size = core::DabDynamic{0.5, core::DabControl::Pressure, 25, 0.2};
    s.shape.angle.control = core::DabControl::Direction;
    s.scatter.enabled = true;
    s.scatter.scatter.jitter = 4.5;
    s.scatter.count = 3;
    s.texture.enabled = true;
    s.texture.pattern.asset = texture;
    s.texture.mode = core::DabMaskMode::ColorBurn;
    s.texture.depth_jitter.control = core::DabControl::Fade;
    s.texture.depth_jitter.fade_steps = 40;
    s.dual.enabled = true;
    s.dual.size_mm = 3;
    s.dual.mode = core::DabMaskMode::HardMix;
    s.color.enabled = true;
    s.color.per_tip = false;
    s.color.purity = -0.5;
    s.transfer.enabled = true;
    s.transfer.flow.control = core::DabControl::Wheel;
    s.pose.enabled = true;
    s.pose.tilt_x = 45;
    s.pose.override_tilt = true;
    s.noise = true;
    s.wet_edges = true;
    s.build_up = true;
    s.build_up_rate = 40;
    s.protect_texture = true;
    s.seed = 1234;
    return s;
}

}  // namespace

class TestBrushesModel : public QObject {
    Q_OBJECT

private slots:
    // The defaults are an empty object; a setting written only where it differs, a section only where it differs (and in
    // it only what differs); every setting read back as it was.
    void dabSettingsKeepOnlyWhatDiffers() {
        QCOMPARE(core::dabs_to_json(core::DabSettings{}), Json::object());
        QVERIFY(core::dabs_from_json(Json::object()) == core::DabSettings{});
        core::DabSettings one;
        one.hardness = 0.5;
        one.scatter.count = 4;
        QCOMPARE(core::dump_python(core::dabs_to_json(one)), std::string(R"({"hardness": 0.5, "scatter": {"count": 4}})"));
        const std::string tip = ref_of(grey_png(8, 8)), tile = ref_of(grey_png(16, 16, 40));
        const core::DabSettings all = full(tip, tile);
        const Json written = core::dabs_to_json(all);
        QVERIFY(core::dabs_from_json(written) == all);
        QCOMPARE(written["tip"], Json({{"asset", tip}}));
        QCOMPARE(written["texture"]["pattern"], Json({{"asset", tile}}));
        QCOMPARE(written["shape"]["size"], Json({{"jitter", 0.5}, {"control", "pressure"}, {"minimum", 0.2}}));
        QCOMPARE(written["texture"]["mode"], Json("color_burn"));
        QVERIFY(!written["texture"].contains("scale"));  // (as the default)
        // the order of the keys is the order of the settings
        std::vector<std::string> keys;
        for (const auto& [k, v] : written.items()) keys.push_back(k);
        QCOMPARE(keys.front(), std::string("size_mm"));
        QCOMPARE(keys.back(), std::string("seed"));
        // the pictures each once
        QCOMPARE(core::dab_assets(all), (std::vector<std::string>{tip, tile}));
        // a procedural texture (the default style, paper): only what differs
        core::DabSettings dots;
        dots.texture.pattern.procedural = "dots";
        dots.texture.pattern.size = 64;
        QCOMPARE(core::dump_python(core::dabs_to_json(dots)), std::string(R"({"texture": {"pattern": {"procedural": "dots", "size": 64}}})"));
        QVERIFY(core::dabs_from_json(core::dabs_to_json(dots)) == dots);
    }

    // Each setting that cannot be is refused with what is wrong.
    void dabSettingsRefuseWhatCannotBe() {
        const auto refused = [](const char* text) { return error_of([&] { (void)core::dabs_from_json(Json::parse(text)); }); };
        QVERIFY(refused(R"({"hardnes": 1})").find("unknown setting: hardnes") != std::string::npos);
        QVERIFY(refused(R"({"size_mm": 0})").find("size_mm must be a number between 0.01 and 500") != std::string::npos);
        QVERIFY(refused(R"({"size_mm": 501})").find("size_mm") != std::string::npos);
        QVERIFY(refused(R"({"hardness": true})").find("hardness") != std::string::npos);
        QVERIFY(refused(R"({"hardness": "1"})").find("hardness") != std::string::npos);
        QVERIFY(refused(R"({"spacing": 0.001})").find("spacing") != std::string::npos);
        QVERIFY(refused(R"({"aliased": 1})").find("aliased must be true or false") != std::string::npos);
        QVERIFY(refused(R"({"seed": 1.5})").find("seed must be a whole number") != std::string::npos);
        QVERIFY(refused(R"({"seed": -1})").find("seed") != std::string::npos);
        QVERIFY(refused(R"({"tip": "square"})").find("tip must be") != std::string::npos);
        QVERIFY(refused(R"({"tip": {"asset": "sha256:12"}})").find("tip must be") != std::string::npos);
        QVERIFY(refused(R"({"shape": {"size": {"control": "speed"}}})").find("shape.size.control must be one of off, fade") != std::string::npos);
        QVERIFY(refused(R"({"shape": {"size": {"jitter": 2}}})").find("shape.size.jitter") != std::string::npos);
        QVERIFY(refused(R"({"scatter": {"scatter": {"jitter": 10}, "count": 17}})").find("scatter.count") != std::string::npos);
        QVERIFY(refused(R"({"texture": {"mode": "screen"}})").find("texture.mode") != std::string::npos);
        QVERIFY(refused(R"({"texture": {"pattern": {"procedural": "wood"}}})").find("texture.pattern.procedural") != std::string::npos);
        QVERIFY(refused(R"({"texture": {"pattern": {"size": 1025}}})").find("texture.pattern.size") != std::string::npos);
        QVERIFY(refused(R"({"pose": {"tilt_x": 91}})").find("pose.tilt_x") != std::string::npos);
        QVERIFY(refused(R"({"dual": {"count": 0}})").find("dual.count") != std::string::npos);
        QVERIFY(refused(R"({"shape": []})").find("shape must be an object") != std::string::npos);
        QVERIFY(refused(R"([])").find("must be an object") != std::string::npos);
        // (a value that is not finite)
        Json nan = Json::object();
        nan["opacity"] = std::nan("");
        QVERIFY(error_of([&] { (void)core::dabs_from_json(nan); }).find("opacity") != std::string::npos);
    }

    // A brush of one's own with "engine": "dabs": its settings read (brush_from_dict) and written last (brush_to_dict);
    // every other brush's settings as before (no "engine"); an engine other than dabs refused. define_brush leaves
    // "engine" and "dabs" to E1 (not read, as before).
    void aBrushOfOnesOwnKeepsItsDabs() {
        const core::Brush plain = core::brush_from_dict("my_plain", Json{{"label", "ふつう"}}, std::nullopt, {});
        QVERIFY(!plain.dabs);
        const Json plain_dict = core::brush_to_dict(plain);
        QVERIFY(!plain_dict.contains("engine") && !plain_dict.contains("dabs"));
        const core::Brush dab = core::brush_from_dict("my_dab", Json::parse(R"({"label": "ダブ", "engine": "dabs", "dabs": {"hardness": 0.5}})"),
                                                      std::nullopt, {});
        QVERIFY(dab.dabs.has_value());
        QCOMPARE(dab.dabs->hardness, 0.5);
        const Json dict = core::brush_to_dict(dab);
        std::vector<std::string> keys;
        for (const auto& [k, v] : dict.items()) keys.push_back(k);
        QCOMPARE(keys[keys.size() - 2], std::string("engine"));
        QCOMPARE(keys.back(), std::string("dabs"));
        QCOMPARE(dict["dabs"], Json({{"hardness", 0.5}}));
        QVERIFY(core::brush_from_dict("my_dab", dict, std::nullopt, {}) == dab);
        // made on it: its dabs kept
        std::vector<core::Brush> known{dab};
        const core::Brush on = core::brush_from_dict("my_on", Json{{"label", "上"}}, std::string("my_dab"), known);
        QVERIFY(on.dabs == dab.dabs);
        QVERIFY(error_of([] { (void)core::brush_from_dict("my_x", Json::parse(R"({"label": "x", "engine": "ink"})"), std::nullopt, {}); })
                    .find("brush engine must be dabs") != std::string::npos);
        QVERIFY(error_of([] { (void)core::brush_from_dict("my_x", Json::parse(R"({"label": "x", "engine": "dabs", "dabs": {"flow": 2}})"),
                                                           std::nullopt, {}); })
                    .find("flow") != std::string::npos);
        // register_brushes skips one that cannot be (the reader reports it)
        std::vector<core::Brush> custom;
        core::register_brushes(Json::parse(R"({"my_bad": {"label": "x", "engine": "dabs", "dabs": {"nope": 1}}})"), custom);
        QVERIFY(custom.empty());
        // define_brush: "engine" and "dabs" not read yet (E1)
        const core::Document defined =
            core::CommandBus(render::ops_registry())
                .apply(book(), Json::parse(R"([{"op": "define_brush", "key": "my_d", "label": "d", "engine": "dabs", "dabs": {"hardness": 0.1}}])"),
                       core::Actor("human:test"))
                .doc;
        QVERIFY(!defined.brush_custom["my_d"].contains("engine"));
        QVERIFY(!core::uses_brushes(defined));
    }

    // A line's inputs: packed as "r" is (tilt two values a point), read back from the packed form and from the dict
    // form; none for a line without them (its packed form as before); refused when they are not one a point or out of
    // their range; a list an op left not one a point is not written.
    void aLinesInputsArePackedAndRead() {
        core::Stroke s = *line(3);
        const Json before = core::stroke_to_packed(s);
        QVERIFY(!before.contains("tilt") && !before.contains("time_ms") && !before.contains("wheel") && !before.contains("flow"));
        QVERIFY(!core::has_stroke_inputs(s));
        s.tilt = {10, -20, 30, 40, -90, 90};
        s.time_ms = {0, 16, 33};
        s.wheel = {0, 0.5, 1};
        s.flow = 0.4;
        QVERIFY(core::has_stroke_inputs(s));
        const Json packed = core::stroke_to_packed(s);
        QCOMPARE(packed["tilt"], Json(core::pack_doubles(s.tilt)));
        QCOMPARE(packed["flow"], Json(0.4));
        const core::Stroke back = core::coerce_stroke(packed);
        QCOMPARE(back.tilt, s.tilt);
        QCOMPARE(back.time_ms, s.time_ms);
        QCOMPARE(back.wheel, s.wheel);
        QCOMPARE(*back.flow, 0.4);
        const core::Stroke dict = core::coerce_stroke(Json::parse(
            R"({"points": [[1, 2], [3, 4]], "tilt": [[5, 6], [7, 8]], "time_ms": [0, 10], "wheel": [0.25, 0.75], "flow": 1})"));
        QCOMPARE(dict.tilt, (std::vector<double>{5, 6, 7, 8}));
        QCOMPARE(dict.wheel, (std::vector<double>{0.25, 0.75}));
        QCOMPARE(*dict.flow, 1.0);
        const auto refused = [](const char* text) { return error_of([&] { (void)core::coerce_stroke(Json::parse(text)); }); };
        QVERIFY(refused(R"({"points": [[1, 2], [3, 4]], "tilt": [[5, 6]]})").find("tilt is an x and a y") != std::string::npos);
        QVERIFY(refused(R"({"points": [[1, 2]], "tilt": [[91, 0]]})").find("tilt is between") != std::string::npos);
        QVERIFY(refused(R"({"points": [[1, 2]], "time_ms": [-1]})").find("time_ms") != std::string::npos);
        QVERIFY(refused(R"({"points": [[1, 2]], "wheel": [1.5]})").find("wheel") != std::string::npos);
        QVERIFY(refused(R"({"points": [[1, 2]], "flow": true})").find("flow") != std::string::npos);
        QVERIFY(refused(R"({"points": [[1, 2]], "flow": 2})").find("flow") != std::string::npos);
        // (an op that changed its points without carrying them: those lists are not written, the flow is)
        core::Stroke cut = s;
        cut.points.pop_back();
        const Json written = core::stroke_to_packed(cut);
        QVERIFY(!written.contains("tilt") && !written.contains("time_ms") && !written.contains("wheel"));
        QCOMPARE(written["flow"], Json(0.4));
        QVERIFY(core::coerce_stroke(written).tilt.empty());
    }

    // The 描画色's precise values (PC4 E-0): add_stroke's color_rgb kept on the line on an RGB page of 16 or 32 bits — a
    // pen layer's and a paint layer's — (its rgb the 8-bit picture), never named an unknown key; refused there when it is
    // not native.color_stroke_v1's form; on an 8-bit page (and an ink one) not used, the line drawn by its rgb as before,
    // and the warnings say so.
    void aLineTakesThePreciseColourOnAPrecisePage() {
        const core::CommandBus bus(render::ops_registry());
        const core::Actor actor("human:test");
        const Json line_op = Json::parse(R"({"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0, "points": [[10, 10], [20, 20]],
                                              "rgb": [128, 0, 0], "color_rgb": {"precision": "u16", "values": [0.5, 0.25, 0.125]}})");
        for (const int depth : {16, 32}) {
            const core::Document deep =
                bus.apply(book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}}), actor).doc;
            const core::ApplyResult drawn = bus.apply(deep, Json::array({line_op}), actor);
            QCOMPARE(drawn.warnings, Json::array());
            const auto& items = drawn.doc.page(0).first_layer(core::LayerRole::Ink)->strokes->items;
            QVERIFY(!items.empty());
            QCOMPARE((*items.back()->color_rgb)["values"], Json::array({0.5, 0.25, 0.125}));
            QCOMPARE(*items.back()->rgb, (std::vector<std::int64_t>{128, 0, 0}));
            // a paint layer's line (drawn into its precise pixels) keeps it too
            Json on_paint = line_op;
            on_paint.erase("layer");
            on_paint["layer_id"] = "paint";
            const core::ApplyResult painted =
                bus.apply(deep, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}}, on_paint}), actor);
            QCOMPARE(painted.warnings, Json::array());
            const core::Layer* paint = nullptr;
            for (const core::Layer& layer : painted.doc.page(0).layers)
                if (layer.id == "paint") paint = &layer;
            QVERIFY(paint != nullptr && paint->strokes && !paint->strokes->items.empty());
            QCOMPARE((*paint->strokes->items.back()->color_rgb)["values"], Json::array({0.5, 0.25, 0.125}));
        }
        // an 8-bit page and an ink one (Lab, 16 bits): the line by its rgb, color_rgb not used and said so
        const Json lab_mode = Json::parse(R"({"op": "set_color_mode", "page": 1, "mode": "lab", "depth": 16})");
        for (const core::Document& plain : {book(), bus.apply(book(), Json::array({lab_mode}), actor).doc}) {
            const core::ApplyResult drawn = bus.apply(plain, Json::array({line_op}), actor);
            QCOMPARE(drawn.warnings, Json::array({"ops[0] add_stroke: color_rgb is for an RGB page of 16 or 32 bits: the line's rgb is used"}));
            const auto& items = drawn.doc.page(0).first_layer(core::LayerRole::Ink)->strokes->items;
            QVERIFY(!items.empty() && !items.back()->color_rgb);
            QCOMPARE(*items.back()->rgb, (std::vector<std::int64_t>{128, 0, 0}));
        }
        const auto refused = [&](const core::Document& doc, const Json& op) {
            try {
                bus.apply(doc, Json::array({op}), actor);
            } catch (const core::ApplyError& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        const core::Document deep = bus.apply(book(), Json::parse(R"([{"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 16}])"), actor).doc;
        Json bad = line_op;
        bad["color_rgb"] = Json::parse(R"({"precision": "u8", "values": [0, 0, 0]})");
        const std::string why = refused(deep, bad);
        QVERIFY2(why.find("color_rgb") != std::string::npos && why.find("unknown keys") == std::string::npos, why.c_str());
    }

    // A book with a dab brush (its tip and texture pictures), a line with inputs and the brush state's kind and
    // background: written with brushes@1 and the pictures, read back the same (written again: the same bytes), gc keeps
    // the pictures; brushes@1 is a feature this build knows.
    void aBookWithDabsIsWrittenAndReadBack() {
        QVERIFY(storage::is_known_feature("brushes@1"));
        const std::string tip_png = grey_png(32, 24, 200), tile_png = grey_png(64, 64, 30);
        const std::string tip = ref_of(tip_png), tile = ref_of(tile_png);
        core::Document doc = book();
        doc.brush_custom["my_dab"] = core::brush_to_dict(core::brush_from_dict(
            "my_dab", Json{{"label", "ダブ"}, {"engine", "dabs"}, {"dabs", core::dabs_to_json(full(tip, tile))}}, std::nullopt, {}));
        doc.brush_tips[tip] = std::make_shared<const std::string>(tip_png);
        doc.brush_tips[tile] = std::make_shared<const std::string>(tile_png);
        doc.brush_kind = "my_dab";
        doc.brush_background_rgb = std::vector<std::int64_t>{250, 240, 230};
        core::Stroke s = *line(2);
        s.kind = "my_dab";
        s.tilt = {10, 20, 30, 40};
        s.time_ms = {0, 8};
        s.wheel = {0.5, 0.5};
        s.flow = 0.75;
        add_line(doc, s);
        QVERIFY(core::uses_brushes(doc));
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
        write_book(doc, dir);
        const Json written = project(dir);
        const auto& features = written["features"];
        QVERIFY(std::find(features.begin(), features.end(), Json("brushes@1")) != features.end());
        QCOMPARE(written["brush"]["kind"], Json("my_dab"));
        QCOMPARE(written["brush"]["background_rgb"], Json::array({250, 240, 230}));
        const storage::AssetStore store(dir);
        QVERIFY(store.has(tip, ".png") && store.has(tile, ".png"));
        const std::set<std::string> kept = storage::referenced_assets(dir);
        QVERIFY(kept.contains(tip) && kept.contains(tile));
        // read back
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        const core::Document& read = loaded.document;
        QCOMPARE(*read.brush_kind, std::string("my_dab"));
        QCOMPARE(*read.brush_background_rgb, (std::vector<std::int64_t>{250, 240, 230}));
        QCOMPARE(read.brush_tips.size(), std::size_t(2));
        QCOMPARE(*read.brush_tips.at(tip), tip_png);
        QVERIFY(core::brush_for(Json("my_dab"), read).dabs == full(tip, tile));
        const core::Stroke& back = *read.page(0).first_layer(core::LayerRole::Ink)->strokes->items.back();
        QCOMPARE(back.tilt, s.tilt);
        QCOMPARE(back.time_ms, s.time_ms);
        QCOMPARE(back.wheel, s.wheel);
        QCOMPARE(*back.flow, 0.75);
        // written again: the same project.json
        QTemporaryDir again;
        const fs::path dir2 = fs::path(again.path().toStdString()) / "b";
        genko::test::write_project(read, dir2);
        std::string where;
        const Json first = project(dir), second = project(dir2);
        QVERIFY2(genko::test::strict_equal(first["brush"], second["brush"], &where), where.c_str());
        QVERIFY2(genko::test::strict_equal(first["features"], second["features"], &where), where.c_str());
        QCOMPARE(first["pages"][0]["layers"], second["pages"][0]["layers"]);  // (the strokes' blob: the same asset)
        // the dab brush's line is not drawn yet (E1), never as another brush's
        render::brushes::register_book(read);
        QVERIFY(error_of([] {
                    (void)render::brushes::draw(render::Size{100, 100}, core::PenPoints{{1, 1, std::nullopt}, {5, 5, std::nullopt}}, 200, 0.5, "my_dab",
                                                "seed", {}, 0.0);
                }).find("not yet ported: brushes@1") != std::string::npos);
    }

    // A book without a dab brush, without a line's inputs and without the brush state's new keys: no brushes@1, the
    // brush's keys as before, a line packed as before; and once the last dab brush is gone, the feature goes too.
    void aBookWithoutDabsIsWrittenAsBefore() {
        core::Document doc = book();
        add_line(doc, *line(4));
        doc.brush_custom["my_plain"] = core::brush_to_dict(core::brush_from_dict("my_plain", Json{{"label", "ふつう"}}, std::nullopt, {}));
        QVERIFY(!core::uses_brushes(doc));
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
        genko::test::write_project(doc, dir);
        const Json written = project(dir);
        QCOMPARE(written["features"], Json::array());
        std::vector<std::string> keys;
        for (const auto& [k, v] : written["brush"].items()) keys.push_back(k);
        QCOMPARE(keys, (std::vector<std::string>{"rgb", "width_mm", "stabilize", "taper", "curve", "custom"}));
        QVERIFY(!written.contains("patterns"));
        const Json packed = core::stroke_to_packed(*line(4));
        std::vector<std::string> stroke_keys;
        for (const auto& [k, v] : packed.items()) stroke_keys.push_back(k);
        QCOMPARE(stroke_keys, (std::vector<std::string>{"id", "kind", "width_mm", "xy"}));
        // the feature named while it is used
        doc.features.push_back("brushes@1");
        QTemporaryDir tmp2;
        const fs::path dir2 = fs::path(tmp2.path().toStdString()) / "b";
        genko::test::write_project(doc, dir2);
        QCOMPARE(project(dir2)["features"], Json::array());
    }

    // What a book cannot hold opens read-only (and is never written): a dab brush's setting that cannot be, its picture
    // missing or not a grey picture of its size (a picture the tip and the texture share: the texture's bound), "dabs"
    // without the engine, a line's inputs not one a point (in the strokes' blob too), the brush state's kind or
    // background that cannot be.
    void whatCannotBeOpensReadOnly() {
        const std::string tip_png = grey_png(8, 8);
        const std::string tip = ref_of(tip_png);
        const auto opened = [&](const std::function<void(Json&)>& change, bool with_tip = true) {
            core::Document doc = book();
            doc.brush_custom["my_dab"] = Json{{"label", "d"}, {"engine", "dabs"}, {"dabs", Json{{"tip", Json{{"asset", tip}}}}}};
            doc.brush_tips[tip] = std::make_shared<const std::string>(tip_png);
            QTemporaryDir tmp;
            const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
            genko::test::write_project(doc, dir);
            Json p = project(dir);
            change(p);
            storage::write_atomic(dir / "project.json", core::dump_python_indent2(p));
            if (!with_tip) fs::remove(storage::AssetStore(dir).path(tip, ".png"));
            return storage::load_document(dir).document.read_only_reason;
        };
        QVERIFY(opened([](Json&) {}).empty());
        QVERIFY(opened([](Json& p) { p["brush"]["custom"]["my_dab"]["dabs"]["hardness"] = 3; }).find("hardness") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["custom"]["my_dab"]["dabs"]["unknown"] = 1; }).find("unknown setting") != std::string::npos);
        QVERIFY(opened([](Json&) {}, false).find("missing") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["custom"]["my_dab"].erase("engine"); }).find("engine") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["custom"]["my_dab"]["engine"] = "ink"; }).find("engine") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["kind"] = 3; }).find("kind") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["background_rgb"] = Json::array({1, 2}); }).find("background_rgb") != std::string::npos);
        QVERIFY(opened([](Json& p) { p["brush"]["background_rgb"] = Json::array({1, 2, 300}); }).find("background_rgb") != std::string::npos);
        // a tip that is not a grey picture
        const std::string rgb_png = render::write_png(render::Image::frombytes("RGB", render::Size{4, 4}, std::string(48, '\x10')));
        QVERIFY(core::check_tip_png(rgb_png, core::kMaxTipEdge).has_value());
        QVERIFY(core::check_tip_png(grey_png(2501, 1), core::kMaxTipEdge).has_value());
        QVERIFY(core::check_tip_png(grey_png(1025, 4), core::kMaxPatternEdge).has_value());
        QVERIFY(!core::check_tip_png(grey_png(1024, 4), core::kMaxPatternEdge).has_value());
        QVERIFY(core::check_tip_png("not a picture", core::kMaxTipEdge).has_value());
        // a line's inputs not one a point (in the strokes' blob)
        core::Document doc = book();
        core::Stroke s = *line(2);
        s.flow = 0.5;
        add_line(doc, s);
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
        genko::test::write_project(doc, dir);
        QVERIFY(storage::load_document(dir).document.read_only_reason.empty());
        QCOMPARE(*storage::load_document(dir).document.page(0).first_layer(core::LayerRole::Ink)->strokes->items.back()->flow, 0.5);
        // a line in the strokes' blob whose tilt is not an x and a y for each point (the blob rewritten as an asset of its
        // own, its name its bytes'): read-only, the blob a broken asset
        {
            core::Document tilted = book();
            core::Stroke t = *line(2);
            t.tilt = {10, 20, 30, 40};
            add_line(tilted, t);
            QTemporaryDir tmp3;
            const fs::path dir3 = fs::path(tmp3.path().toStdString()) / "b";
            genko::test::write_project(tilted, dir3);
            Json p = project(dir3);
            Json* layer_of_blob = nullptr;
            for (Json& layer : p["pages"][0]["layers"])
                if (layer.contains("strokes_blob")) layer_of_blob = &layer;
            QVERIFY(layer_of_blob != nullptr);
            storage::AssetStore store(dir3);
            Json list = Json::parse(*store.get_bytes((*layer_of_blob)["strokes_blob"].get<std::string>(), ".strokes.json"));
            QVERIFY(list.back().contains("tilt"));
            list.back()["tilt"] = core::pack_doubles(std::vector<double>{10, 20, 30});
            (*layer_of_blob)["strokes_blob"] = store.put_bytes(list.dump(), ".strokes.json");
            storage::write_atomic(dir3 / "project.json", core::dump_python_indent2(p));
            const auto loaded = storage::load_document(dir3);
            QVERIFY(loaded.document.read_only_reason.find("tilt") != std::string::npos);
            bool broken = false;
            for (const auto& issue : loaded.report.issues) broken = broken || issue.kind == "broken_asset";
            QVERIFY(broken);
        }
        // a picture both the tip and the texture use: the texture's bound (1024 a side), the smaller
        {
            const std::string wide_png = grey_png(2000, 4);
            const std::string wide = ref_of(wide_png);
            core::Document doc2 = book();
            doc2.brush_tips[wide] = std::make_shared<const std::string>(wide_png);
            doc2.brush_custom["my_dab"] = Json{{"label", "d"}, {"engine", "dabs"}, {"dabs", Json{{"tip", Json{{"asset", wide}}}}}};
            QVERIFY(error_of([&] { core::validate_brushes(doc2); }).empty());
            doc2.brush_custom["my_dab"]["dabs"]["texture"] = Json{{"enabled", true}, {"pattern", Json{{"asset", wide}}}};
            QVERIFY(error_of([&] { core::validate_brushes(doc2); }).find("1024") != std::string::npos);
        }
    }
};

QTEST_GUILESS_MAIN(TestBrushesModel)
#include "test_brushes_model.moc"
