// PC4 E6 (style.presets.*, SPEC.md COLOR-09「スタイル」): the layer styles of this machine as PhotoCraft v0.2.0's
// engine/src/presets/tests.rs tries its styles — the built-in groups 基本・テキスト効果・ボタン (upstream's designs, its
// pixels as 150 dpi's), a preset checked (a book's pattern refused, a library's named), a preset of a layer (its patterns
// into the pattern library, once), the op that applies one (a library pattern as its picture; Shift: added), the listing,
// the swatch; .asl files written and read (their patterns into the library, a broken or huge one refused before anything is
// made); `genko styles`.

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/layer_styles.hpp"
#include "core/paths.hpp"
#include "core/patterns.hpp"
#include "render/asl.hpp"
#include "render/image.hpp"
#include "render/ops_patterns.hpp"
#include "render/ops_registry.hpp"
#include "render/pat.hpp"
#include "render/pattern_library.hpp"
#include "render/png.hpp"
#include "render/preset_store.hpp"
#include "render/style_presets.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace store = render::preset_store;
namespace styles = render::style_presets;

namespace {

std::string refused(const std::function<void()>& work) {
    try {
        work();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

// A 2 × 2 RGBA picture of red and blue as add_pattern takes it.
Json checker(const std::string& name = "チェック") {
    std::string bytes;
    for (int i = 0; i < 4; ++i) bytes += (i == 0 || i == 3) ? std::string("\xFF\x00\x00\xFF", 4) : std::string("\x00\x00\xFF\xFF", 4);
    const std::string png = render::write_png(render::Image::frombytes("RGBA", render::Size{2, 2}, bytes));
    return Json{{"name", name}, {"png", core::b64encode(png)}, {"mode", "rgb"}, {"depth", 8}, {"dpi", 300}};
}

core::Document book_with_layer(const Json& style) {
    core::Document doc = core::new_episode("スタイル", core::Num(1), 1, core::PageSpec::custom(50.8, 50.8, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    const std::string id = render::add_pattern_to(doc, checker()).first;
    core::Layer layer;
    layer.id = "paint";
    layer.title = "見出し";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.blend = "multiply";
    Json s = style;
    core::change_style_patterns(s, [&](Json& fill) { fill["id"] = id; });
    layer.style = core::layer_style_spec(s);
    doc.edit_page(0).layers.push_back(layer);
    return doc;
}

}  // namespace

class TestStylePresets : public QObject {
    Q_OBJECT

private slots:
    // the built-in groups: upstream's three, their styles' effects in this build's form (its pixels as 150 dpi's mm)
    void builtins() {
        const auto groups = styles::kind().builtin();
        QCOMPARE(groups.size(), std::size_t(3));
        QCOMPARE(groups[0].name, std::string("基本"));
        QCOMPARE(groups[1].name, std::string("テキスト効果"));
        QCOMPARE(groups[2].name, std::string("ボタン"));
        QCOMPARE(groups[0].items.size(), std::size_t(8));
        QCOMPARE(groups[1].items.size(), std::size_t(7));
        QCOMPARE(groups[2].items.size(), std::size_t(6));
        // デフォルトスタイル（なし）: no effect (applying it takes the layer's style off)
        QCOMPARE(groups[0].items[0]["name"], Json("デフォルトスタイル（なし）"));
        QCOMPARE(groups[0].items[0]["effects"], Json::array());
        // ドロップシャドウ: distance 6 px, size 8 px, 60 % (upstream) → 1.02 mm, 1.35 mm, 0.6
        const Json* shadow = store::find(groups, "ドロップシャドウ");
        QVERIFY(shadow != nullptr);
        const Json& e = (*shadow)["effects"][0];
        QCOMPARE(e["kind"], Json("drop_shadow"));
        QCOMPARE(e["distance_mm"], Json(1.02));
        QCOMPARE(e["size_mm"], Json(1.35));
        QCOMPARE(e["opacity"], Json(0.6));
        QCOMPARE(e["global_light"], Json(true));
        // コミックの縁取り: its own angle (135°, not the global light); 中抜き: fill 0
        const Json* comic = store::find(groups, "コミックの縁取り");
        QCOMPARE((*comic)["effects"].size(), std::size_t(3));
        const Json& drop = (*comic)["effects"][2];
        QCOMPARE(drop["angle"], Json(135));
        QCOMPARE(drop["global_light"], Json(false));
        QCOMPARE((*store::find(groups, "中抜き"))["fill_opacity"], Json(0));
        // ゴールド: its gradient's two colours, the bevel's depth 180 % → 1.8
        const Json* gold = store::find(groups, "ゴールド");
        QCOMPARE((*gold)["effects"][1]["kind"], Json("bevel"));
        QCOMPARE((*gold)["effects"][1]["depth"], Json(1.8));
        QCOMPARE((*gold)["effects"][0]["gradient"]["stops"][0][1], Json::parse("[138, 90, 18]"));
        // every built-in style as the kind checks it, each effect in its normal form
        for (const store::Group& g : groups) {
            for (const Json& item : g.items) {
                QVERIFY2(refused([&] { styles::kind().check(item); }).empty(), item.dump().c_str());
                QCOMPARE(styles::item_of(item, item["name"].get<std::string>()), item);
            }
        }
        // the listing
        const Json list = styles::list(groups);
        QCOMPARE(list["groups"][1]["styles"][0]["name"], Json("ネオン（青）"));
        QCOMPARE(list["groups"][1]["styles"][0]["effects"], Json::parse(R"(["color_overlay", "inner_glow", "outer_glow"])"));
        QCOMPARE(list["groups"][1]["styles"][6]["fill_opacity"], Json(0));
    }

    // a preset checked: its keys, its patterns the library's (a book's id refused), its blend mode
    void checked() {
        QVERIFY(refused([] { styles::item_of(Json{{"effects", Json::array()}, {"what", 1}}, "x"); }).find("unknown key") != std::string::npos);
        QVERIFY(!refused([] { styles::item_of(Json{{"effects", Json::array({Json{{"kind", "nothing"}}})}}, "x"); }).empty());
        const Json overlay{{"kind", "pattern_overlay"}, {"pattern", Json{{"id", "pat-1"}}}};
        QVERIFY(refused([&] { styles::item_of(Json{{"effects", Json::array({overlay})}}, "x"); }).find("library") != std::string::npos);
        const Json item = styles::item_of(
            Json{{"effects", Json::array({Json{{"kind", "pattern_overlay"}, {"pattern", Json{{"library", "木目"}, {"scale", 50}}}}})}, {"blend", "screen"}}, "木");
        QCOMPARE(item["effects"][0]["pattern"]["library"], Json("木目"));
        QCOMPARE(item["effects"][0]["pattern"]["scale"], Json(50));
        QCOMPARE(item["effects"][0]["pattern"]["link"], Json(true));
        QCOMPARE(item["blend"], Json("screen"));
        QCOMPARE(styles::patterns_of(item), (std::vector<Json>{Json{{"library", "木目"}}}));
        QVERIFY(!refused([] { styles::item_of(Json{{"effects", Json::array()}, {"blend", "sparkle"}}, "x"); }).empty());
        QVERIFY(!refused([] { styles::item_of(Json{{"effects", Json::array()}, {"fill_opacity", 2}}, "x"); }).empty());
    }

    // 新規スタイル…: a layer's effects and blending options, its pattern into the library once; applied again: the op that
    // gives the library's pattern as its picture (set_layer_style puts it into the book); Shift: added
    void ofALayerAndApplied() {
        QTemporaryDir scratch;
        const fs::path config = core::path_from_utf8(scratch.path().toStdString());
        const Json style{{"effects", Json::array({Json{{"kind", "pattern_overlay"}, {"pattern", Json{{"id", "x"}, {"scale", 200}}}},
                                                  Json{{"kind", "stroke"}, {"size_mm", 0.5}}})},
                         {"fill_opacity", 0.5}};
        const core::Document doc = book_with_layer(style);
        const core::Layer& layer = doc.page(0).layers.back();
        store::Library patterns = render::pattern_library::load(config);
        const std::size_t before = patterns.groups.front().items.size();
        bool added = false;
        const Json item = styles::item_of_layer(doc, layer, true, true, "見出し", config, patterns.groups, added);
        QVERIFY(added);
        QCOMPARE(item["name"], Json("見出し"));
        QCOMPARE(item["blend"], Json("multiply"));
        QCOMPARE(item["fill_opacity"], Json(0.5));
        QCOMPARE(item["effects"].size(), std::size_t(2));
        QCOMPARE(item["effects"][0]["pattern"]["library"], Json("チェック"));
        QCOMPARE(item["effects"][0]["pattern"]["asset"], Json(doc.patterns[0].asset));
        QCOMPARE(item["effects"][0]["pattern"]["scale"], Json(200));
        std::size_t total = 0;
        for (const store::Group& g : patterns.groups) total += g.items.size();
        QVERIFY(total > before);
        // the same again: the library's own pattern, none added
        bool again = false;
        (void)styles::item_of_layer(doc, layer, true, false, "見出し", config, patterns.groups, again);
        QVERIFY(!again);
        // effects only / blending only / neither
        const Json effects_only = styles::item_of_layer(doc, layer, true, false, "a", config, patterns.groups, again);
        QVERIFY(!effects_only.contains("blend") && !effects_only.contains("fill_opacity"));
        const Json blending_only = styles::item_of_layer(doc, layer, false, true, "b", config, patterns.groups, again);
        QCOMPARE(blending_only["effects"], Json::array());
        QCOMPARE(blending_only["fill_opacity"], Json(0.5));
        QVERIFY(!refused([&] { styles::item_of_layer(doc, layer, false, false, "c", config, patterns.groups, again); }).empty());
        render::pattern_library::save(config, patterns.groups);

        // applied: the pattern found by its picture (renamed in the library still found), given as its picture
        patterns = render::pattern_library::load(config);
        store::edit(patterns.groups, render::pattern_library::kind(), Json{{"action", "rename"}, {"preset", "チェック"}, {"name", "市松"}});
        const Json op = styles::apply_op(item, Json(1), {"paint"}, false, config, patterns.groups);
        QCOMPARE(op["op"], Json("set_layer_style"));
        QCOMPARE(op["blend"], Json("multiply"));
        QVERIFY(!op.contains("add"));
        QVERIFY(op["style"]["effects"][0]["pattern"].contains("png"));
        QCOMPARE(op["style"]["effects"][0]["pattern"]["scale"], Json(200));
        // on a book without the pattern: put in by the op, the overlay naming it
        core::Document plain = core::new_episode("p", core::Num(1), 1, core::PageSpec::custom(50.8, 50.8, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        core::Layer paint;
        paint.id = "paint";
        paint.role = core::LayerRole::User;
        paint.kind = core::LayerKind::Raster;
        plain.edit_page(0).layers.push_back(paint);
        const core::Document styled = core::CommandBus(render::ops_registry()).apply(plain, Json::array({op}), core::Actor("human:test")).doc;
        QCOMPARE(styled.patterns.size(), std::size_t(1));
        const core::Layer& got = styled.page(0).layers.back();
        QVERIFY(got.style.has_value());
        QCOMPARE((*got.style)["effects"][0]["pattern"]["id"], Json(styled.patterns[0].id));
        QCOMPARE(got.blend, std::string("multiply"));
        // Shift: added after the layer's own
        const Json more = styles::apply_op(*store::find(styles::kind().builtin(), "ドロップシャドウ"), Json(1), {"paint"}, true, config, patterns.groups);
        QCOMPARE(more["add"], Json(true));
        const core::Document both = core::CommandBus(render::ops_registry()).apply(styled, Json::array({more}), core::Actor("human:test")).doc;
        QCOMPARE((*both.page(0).layers.back().style)["effects"].size(), std::size_t(3));
        // a preset whose pattern the library has not: refused
        patterns.groups.clear();
        QVERIFY(refused([&] { styles::apply_op(item, Json(1), {"paint"}, false, config, patterns.groups); }).find("no pattern") != std::string::npos);
    }

    // the panel's swatch: the size asked, the rounded square with its effects (a drop shadow darkens below it), its
    // patterns from the library (one not there left out)
    void swatch() {
        QTemporaryDir scratch;
        const fs::path config = core::path_from_utf8(scratch.path().toStdString());
        const store::Library patterns = render::pattern_library::load(config);
        const Json plain = *store::find(styles::kind().builtin(), "デフォルトスタイル（なし）");
        const render::Image none = styles::thumbnail(plain, 48, config, patterns.groups);
        QCOMPARE(none.width(), 48);
        QCOMPARE(none.mode(), std::string("RGBA"));
        const Json red = *store::find(styles::kind().builtin(), "黒の境界線");
        const render::Image stroked = styles::thumbnail(red, 48, config, patterns.groups);
        QVERIFY(stroked.tobytes() != none.tobytes());
        // (just outside the square's left edge, half way down: the black stroke there, nothing on the plain one)
        const auto alpha_at = [](const render::Image& image, int x, int y) { return static_cast<unsigned char>(image.tobytes()[std::size_t(y * image.width() + x) * 4 + 3]); };
        QCOMPARE(int(alpha_at(none, 9, 24)), 0);
        QVERIFY(alpha_at(stroked, 9, 24) > 128);
        // a pattern the library has not: its effect left out, the swatch made still
        const Json missing = styles::item_of(Json{{"effects", Json::array({Json{{"kind", "pattern_overlay"}, {"pattern", Json{{"library", "ない"}}}}})}}, "x");
        QCOMPARE(styles::thumbnail(missing, 48, config, patterns.groups).tobytes(), none.tobytes());
        QCOMPARE(styles::thumbnail(plain, 4000, config, patterns.groups).width(), 256);
    }

    // .asl: the styles written (their patterns with them) and read back as presets — lengths at the dpi read, the patterns
    // into the library once, the blend mode of the blending options; one broken or huge refused before it is read
    void aslFiles() {
        QTemporaryDir scratch;
        const fs::path config = core::path_from_utf8(scratch.path().toStdString());
        const Json style{{"effects", Json::array({Json{{"kind", "pattern_overlay"}, {"pattern", Json{{"id", "x"}, {"scale", 50}}}},
                                                  Json{{"kind", "drop_shadow"}, {"distance_mm", 1.016}, {"size_mm", 0.508}, {"opacity", 0.5}}})},
                         {"fill_opacity", 0.25}};
        const core::Document doc = book_with_layer(style);
        const Json& own = *doc.page(0).layers.back().style;
        std::vector<render::asl::Style> written{{"模様", own, std::string("screen")}, {"影だけ", Json{{"effects", Json::array({own["effects"][1]})}}, std::nullopt}};
        const std::string file = render::asl::write(written, doc, 150);
        QCOMPARE(file.substr(2, 4), std::string("8BSL"));
        const render::asl::File read = render::asl::read(file, 150);
        QCOMPARE(read.styles.size(), std::size_t(2));
        QCOMPARE(read.patterns.size(), std::size_t(1));
        QCOMPARE(read.styles[0].name, std::string("模様"));
        QCOMPARE(read.styles[0].blend, std::optional<std::string>("screen"));
        QCOMPARE(read.styles[0].style["fill_opacity"], Json(0.25));
        QCOMPARE(read.styles[0].style["effects"][0]["pattern"]["id"], Json("asl-0"));
        QVERIFY(std::abs(read.styles[0].style["effects"][1]["distance_mm"].get<double>() - 1.016) < 0.01);
        // (its scale at the file's dpi: the book's 300 dpi pattern at 50 % is the file's 150 dpi one at 25 %, the same size)
        QVERIFY(std::abs(read.styles[0].style["effects"][0]["pattern"]["scale"].get<double>() - 25) < 0.5);
        QVERIFY(!read.styles[1].blend);

        store::Library patterns = render::pattern_library::load(config);
        bool added = false;
        const Json got = styles::import_asl(file, 150, config, patterns.groups, "読んだ", added);
        QVERIFY(added);
        QCOMPARE(got["items"].size(), std::size_t(2));
        QCOMPARE(got["patterns"].size(), std::size_t(1));
        QCOMPARE(got["items"][0]["blend"], Json("screen"));
        const std::string library_name = got["items"][0]["effects"][0]["pattern"]["library"].get<std::string>();
        QVERIFY(store::find(patterns.groups, library_name, std::string_view("読んだ")) != nullptr);
        // read again: the library's own pattern
        bool again = false;
        const Json twice = styles::import_asl(file, 150, config, patterns.groups, "読んだ", again);
        QVERIFY(!again);
        QCOMPARE(twice["items"][0]["effects"][0]["pattern"]["library"], Json(library_name));

        // not an .asl; one that ends early; more styles than kMaxStyles; past kMaxFileBytes (refused before it is read)
        const auto code_of = [](const std::string& data) {
            try {
                render::asl::read(data, 150);
            } catch (const core::Error& error) {
                return std::string(error.code());
            }
            return std::string("read");
        };
        QCOMPARE(code_of(std::string("\x00\x02" "8BPS", 6) + std::string(20, '\0')), std::string("format"));
        QCOMPARE(code_of(file.substr(0, 12)), std::string("format"));
        std::string many = file.substr(0, 12);  // (version, 8BSL, patterns' version and a length of 0)
        many[8] = many[9] = many[10] = many[11] = 0;
        many += std::string("\x7F\xFF\xFF\xFF", 4);
        QCOMPARE(code_of(many), std::string("too_large"));
        std::string lying = many.substr(0, 12) + std::string("\x00\x00\x00\x05", 4) + std::string(8, '\0');
        QCOMPARE(code_of(lying), std::string("format"));
        QCOMPARE(code_of(std::string(render::asl::kMaxFileBytes + 1, '\0')), std::string("too_large"));
        // a style whose descriptor is broken: left out, said
        std::string broken = file;
        broken[broken.size() - 9] = '\x7F';
        const render::asl::File partly = render::asl::read(broken, 150);
        QVERIFY(partly.styles.size() + partly.notes.size() >= 2);
    }

    // `genko styles`: list, new, define, edit, reset, op, thumbnail, import-asl (the CLI on a settings folder of its own)
    void theCommandLine() {
        QTemporaryDir scratch;
        const QString config = scratch.path() + "/config";
        QDir().mkpath(config);
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        const auto genko = [&](const QStringList& args) { return genko::test::run_genko(args, env); };
        auto out = genko({"styles", "list"});
        QVERIFY2(out.finished && out.exit_code == 0, qPrintable(out.described()));
        Json got = genko::test::one_line(out.out);
        QCOMPARE(got["groups"][0]["name"], Json("基本"));
        QCOMPARE(got["groups"][0]["styles"][1]["name"], Json("ドロップシャドウ"));
        out = genko({"styles", "new", "--style", R"({"effects": [{"kind": "color_overlay", "rgb": [0, 128, 0]}]})", "--name", "緑", "--group", "自分の"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["group"], Json("自分の"));
        out = genko({"styles", "new", "--style", R"({"effects": [{"kind": "glitter"}]})"});
        QVERIFY(out.exit_code != 0);
        out = genko({"styles", "op", "緑", "--page", "1", "--ids", "a,b", "--add"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        got = genko::test::one_line(out.out);
        QCOMPARE(got["op"], Json("set_layer_style"));
        QCOMPARE(got["ids"], Json::parse(R"(["a", "b"])"));
        QCOMPARE(got["add"], Json(true));
        QCOMPARE(got["style"]["effects"][0]["rgb"], Json::parse("[0, 128, 0]"));
        out = genko({"styles", "thumbnail", "緑", "--size", "32"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["size"], Json(32));
        out = genko({"styles", "edit", R"({"action": "rename", "preset": "緑", "name": "みどり"})"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        got = genko::test::one_line(genko({"styles", "list"}).out);
        QCOMPARE(got["groups"].back()["styles"][0]["name"], Json("みどり"));
        // a book's layer: define
        const QString book = scratch.path() + "/b.genko";
        out = genko({"new", book, "--pages", "1", "--title", "t", "--plain"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        const QString ops = scratch.path() + "/ops.json";
        storage::write_atomic(core::path_from_utf8(ops.toStdString()),
                              R"([{"op": "add_layer", "page": 1, "kind": "paint", "id": "fxl", "name": "効果"}, {"op": "set_layer_style", "page": 1, "id": "fxl", "style": {"effects": [{"kind": "stroke", "size_mm": 0.4}]}}])");
        out = genko({"apply", book, ops, "--agent", "human:test"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        out = genko({"styles", "define", book, "--page", "1", "--layer", "fxl", "--name", "線"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["effects"], Json(1));
        // import-asl: a group of the file's name
        const std::string asl = render::asl::write({{"影", Json{{"effects", Json::array({core::default_effect("drop_shadow")})}}, std::nullopt}}, core::Document(), 150);
        const QString asl_path = scratch.path() + "/影の.asl";
        storage::write_atomic(core::path_from_utf8(asl_path.toStdString()), asl);
        out = genko({"styles", "import-asl", asl_path});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        got = genko::test::one_line(out.out);
        QCOMPARE(got["group"], Json("影の"));
        QCOMPARE(got["styles"], Json::parse(R"(["影"])"));
        out = genko({"styles", "reset"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["groups"], Json(3));
    }
};

QTEST_GUILESS_MAIN(TestStylePresets)
#include "test_style_presets.moc"
