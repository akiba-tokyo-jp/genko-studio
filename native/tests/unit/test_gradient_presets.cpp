// PC4 E4 (gradient.presets.*, SPEC.md COLOR-09「グラデーションのプリセット」): the gradients of this machine as PhotoCraft
// v0.2.0's engine/src/presets/tests.rs (24-151) tries them — the built-in groups (基本 first: 描画色から背景色へ, 描画色から透明に
// with its transparency, 黒、白; the hues' groups), a preset made of stops (a list of colours spaced evenly, "#rrggbb",
// "foreground", "background", the transparency in upstream's 0..100 too), the bad ones refused, new presets named apart, renamed,
// moved, deleted, groups made and deleted, the built-in ones again (and added after the person's), the library written to the
// settings folder and read again (one that cannot be read never written over); a preset's stops for gradient_fill with the
// 描画色 and 背景色 (8-bit or precise) and its swatch's colours; `genko gradients`.

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/gradient_presets.hpp"
#include "render/preset_store.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

// (a colour "#rrggbb" is written in a plain string literal, not a raw one: moc of Qt 6.11 finds no class in a file whose
// raw string holds `"#`)

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace store = render::preset_store;
namespace presets = render::gradient_presets;

namespace {

std::vector<std::string> group_names(const std::vector<store::Group>& groups) {
    std::vector<std::string> out;
    for (const store::Group& g : groups) out.push_back(g.name);
    return out;
}

std::string refused(const std::function<void()>& work) {
    try {
        work();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

}  // namespace

class TestGradientPresets : public QObject {
    Q_OBJECT

private slots:
    void builtinsAndListing() {
        const auto groups = presets::kind().builtin();
        const auto names = group_names(groups);
        QCOMPARE(names.front(), std::string("基本"));
        QCOMPARE(names.size(), std::size_t(9));
        for (const char* hue : {"ブルー", "パープル", "ピンク", "レッド", "オレンジ", "グリーン", "ブラウン", "グレー"})
            QVERIFY2(std::find(names.begin(), names.end(), std::string(hue)) != names.end(), hue);
        QCOMPARE(groups[0].items[0]["name"], Json("描画色から背景色へ"));
        QCOMPARE(groups[0].items[0]["stops"], Json::parse(R"([[0.0, "foreground"], [1.0, "background"]])"));
        QCOMPARE(groups[0].items[1]["name"], Json("描画色から透明に"));
        QCOMPARE(groups[0].items[1]["transparency"][1], Json::parse("[1.0, 0.0]"));
        QCOMPARE(groups[0].items[2]["stops"], Json::parse("[[0.0, [0, 0, 0]], [1.0, [255, 255, 255]]]"));
        // レッド 01 begins at #7f0000; three colours spaced evenly
        const Json* red = store::find(groups, "レッド 01");
        QVERIFY(red != nullptr);
        QCOMPARE((*red)["stops"][0][1], Json::parse("[127, 0, 0]"));
        const Json* three = store::find(groups, "ブルー 02");
        QCOMPARE((*three)["stops"][1][0], Json(0.5));
        // the listing of names (preset_store's) and every built-in preset as the kind checks it
        const Json list = store::list(groups);
        QCOMPARE(list["groups"][0]["items"][1], Json("描画色から透明に"));
        std::size_t n = 0;
        for (const store::Group& g : groups) n += g.items.size();
        QCOMPARE(n, std::size_t(3 + 8 * 6));
    }

    void presetsMadeOfStops() {
        const Json made = presets::item_of(Json::parse("{\"stops\": [[0, \"#112233\"], [0.5, \"foreground\"], [1, \"#ffffff\"]], \"transparency\": [[0, 100], [1, 50]]}"),
                                           "Mine", true);
        QCOMPARE(made["stops"][0][1], Json::parse("[17, 34, 51]"));
        QCOMPARE(made["stops"][1], Json::parse(R"([0.5, "foreground"])"));
        QCOMPARE(made["transparency"][1], Json::parse("[1.0, 0.5]"));
        // a list of colours: spaced evenly; stops out of order: sorted
        QCOMPARE(presets::item_of(Json::parse("{\"stops\": [\"#000000\", \"background\", [9, 9, 9]]}"), "x")["stops"][1][0], Json(0.5));
        QCOMPARE(presets::item_of(Json::parse("{\"stops\": [[1, \"#000000\"], [0, \"#ffffff\"]]}"), "x")["stops"][0][1], Json::parse("[255, 255, 255]"));
        // what cannot be
        QVERIFY(refused([] { (void)presets::item_of(Json::parse("{\"stops\": [[0, \"#zzzzzz\"], [1, \"#000000\"]]}"), "x"); }).find("colour") != std::string::npos);
        QVERIFY(refused([] { (void)presets::item_of(Json::parse("{\"stops\": [[0, \"#000000\"]]}"), "x"); }).find("2 to 16") != std::string::npos);
        QVERIFY(refused([] { (void)presets::item_of(Json::parse("{\"stops\": [[0, \"#000000\"], [2, \"#ffffff\"]]}"), "x"); }).find("0..1") != std::string::npos);
        QVERIFY(refused([] { (void)presets::item_of(Json::parse("{\"stops\": [[0, [0, 0, 256]], [1, \"#ffffff\"]]}"), "x"); }).find("0..255") != std::string::npos);
        QVERIFY(!refused([] { (void)presets::item_of(Json::parse("{\"stops\": [\"#000000\", \"#ffffff\"], \"transparency\": [[0, 1.5], [1, 0]]}"), "x"); }).empty());
        // the kind's check: a key it does not know, stops out of order
        QVERIFY(!refused([] { presets::kind().check(Json::parse(R"({"name": "a", "stops": [[0, [0, 0, 0]], [1, [1, 1, 1]]], "glow": 1})")); }).empty());
        QVERIFY(!refused([] { presets::kind().check(Json::parse(R"({"name": "a", "stops": [[1, [0, 0, 0]], [0, [1, 1, 1]]]})")); }).empty());
        presets::kind().check(made);
    }

    void newRenameMoveDeleteGroups() {
        std::vector<store::Group> groups = presets::kind().builtin();
        const auto& kind = presets::kind();
        const Json mine = presets::item_of(Json::parse("{\"stops\": [[0, \"#112233\"], [0.5, \"foreground\"], [1, \"#ffffff\"]]}"), "Mine");
        QCOMPARE(store::add(groups, kind, mine, std::string_view("My Gradients"), "カスタム")["group"], Json("My Gradients"));
        QCOMPARE(store::add(groups, kind, mine, std::nullopt, "カスタム")["name"], Json("Mine 2"));
        (void)store::edit(groups, kind, Json::parse(R"({"action": "rename", "preset": "Mine", "name": "Ours"})"));
        QVERIFY(!refused([&] { (void)store::edit(groups, kind, Json::parse(R"({"action": "rename", "preset": "Ours", "name": "Mine 2"})")); }).empty());
        (void)store::edit(groups, kind, Json::parse(R"({"action": "move", "preset": "Mine 2", "to": "My Gradients", "index": 0})"));
        const auto mine_group = std::find_if(groups.begin(), groups.end(), [](const store::Group& g) { return g.name == "My Gradients"; });
        QCOMPARE(mine_group->items[0]["name"], Json("Mine 2"));
        QCOMPARE(mine_group->items[1]["stops"][1], Json::parse(R"([0.5, "foreground"])"));
        (void)store::edit(groups, kind, Json::parse(R"({"action": "delete", "preset": ["Ours", "Mine 2"]})"));
        (void)store::edit(groups, kind, Json::parse(R"({"action": "delete_group", "group": "My Gradients"})"));
        (void)store::edit(groups, kind, Json::parse(R"({"action": "delete_group", "group": "ブルー"})"));
        const auto without = group_names(groups);
        QVERIFY(std::find(without.begin(), without.end(), std::string("ブルー")) == without.end());
        groups = store::reset(groups, kind, false);
        const auto names = group_names(groups);
        QVERIFY(std::find(names.begin(), names.end(), std::string("ブルー")) != names.end());
        // added after the person's: a name taken gets a number
        const auto both = store::reset(groups, kind, true);
        QCOMPARE(both.size(), groups.size() * 2);
        QCOMPARE(both[groups.size()].name, std::string("基本 2"));
        QVERIFY(!refused([&] { (void)store::edit(groups, kind, Json::parse(R"({"action": "explode"})")); }).empty());
    }

    // the settings folder's gradients.json: written whole, read again; one that cannot be read shows the built-in groups and
    // is never written over
    void keptInTheSettingsFolder() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString());
        store::Library first = store::load(dir, presets::kind());
        QVERIFY(!first.from_file && first.problem.empty());
        (void)store::add(first.groups, presets::kind(), presets::item_of(Json::parse("{\"stops\": [\"#ff0000\", \"#0000ff\"]}"), "Saved"), std::nullopt, "x");
        store::save(dir, presets::kind(), first.groups);
        store::Library again = store::load(dir, presets::kind());
        QVERIFY(again.from_file);
        QVERIFY(store::find(again.groups, "Saved") != nullptr);
        {
            std::ofstream broken(dir / "gradients.json", std::ios::binary);
            broken << R"({"version": 1, "groups": [{"name": "a", "items": [{"name": "b", "stops": "no"}]}]})";
        }
        store::Library unread = store::load(dir, presets::kind());
        QVERIFY(!unread.problem.empty());
        QCOMPARE(group_names(unread.groups).front(), std::string("基本"));
        QVERIFY(!refused([&] { store::save(dir, presets::kind(), unread.groups); }).empty());
        QCOMPARE(storage::read_file_bounded(dir / "gradients.json", 1 << 20).substr(0, 12), std::string(R"({"version": )"));
    }

    // a preset's stops for gradient_fill: the 描画色 and 背景色 put in (8-bit, or precise colours), its transparency (or none,
    // 透明部分 off); a swatch's colour at a place
    void stopsForTheTool() {
        const Json item = presets::kind().builtin()[0].items[1];  // (描画色から透明に)
        const Json stops = presets::stops_for(item, Json::array({10, 20, 30}), Json::array({200, 200, 200}));
        QCOMPARE(stops["stops"], Json::parse("[[0.0, [10, 20, 30], 1.0], [1.0, [10, 20, 30], 1.0]]"));
        QCOMPARE(stops["transparency"], item["transparency"]);
        QVERIFY(!presets::stops_for(item, Json::array({10, 20, 30}), Json::array({200, 200, 200}), false).contains("transparency"));
        const Json precise = Json{{"precision", "u16"}, {"values", Json::array({0.5, 0.25, 0.125})}};
        QCOMPARE(presets::stops_for(presets::kind().builtin()[0].items[0], precise, Json::array({1, 2, 3}))["stops"][0][1], precise);
        const auto mid = presets::sample(item, 0.5, {10, 20, 30}, {200, 200, 200});
        QCOMPARE(mid[0], 10);
        QCOMPARE(mid[3], 128);
        const auto grey = presets::sample(presets::kind().builtin()[0].items[2], 0.5, {0, 0, 0}, {0, 0, 0});
        QCOMPARE(grey[0], 128);
        QCOMPARE(grey[3], 255);
    }

    // `genko gradients`: list, new, edit, reset, import-grd (the CLI on a settings folder of its own)
    void theCommandLine() {
        QTemporaryDir scratch;
        const QString config = scratch.path() + "/config";
        QDir().mkpath(config);
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        const auto genko = [&](const QStringList& args) { return genko::test::run_genko(args, env); };
        auto out = genko({"gradients", "list"});
        QVERIFY2(out.finished && out.exit_code == 0, qPrintable(out.described()));
        Json got = genko::test::one_line(out.out);
        QCOMPARE(got["groups"][0]["name"], Json("基本"));
        QCOMPARE(got["groups"][0]["presets"][1]["name"], Json("描画色から透明に"));
        out = genko({"gradients", "new", "--stops", "[[0, \"#ff0000\"], [1, \"background\"]]", "--transparency", "[[0, 1], [1, 0.25]]", "--name", "赤から",
                     "--group", "自分の"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["group"], Json("自分の"));
        out = genko({"gradients", "edit", R"({"action": "rename", "preset": "赤から", "name": "赤"})"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        got = genko::test::one_line(genko({"gradients", "list"}).out);
        QCOMPARE(got["groups"].back()["presets"][0]["name"], Json("赤"));
        QCOMPARE(got["groups"].back()["presets"][0]["transparency"][1], Json::parse("[1.0, 0.25]"));
        out = genko({"gradients", "new", "--stops", "[[0, \"#zz0000\"], [1, \"background\"]]"});
        QVERIFY(out.exit_code != 0);
        out = genko({"gradients", "reset"});
        QVERIFY2(out.exit_code == 0, qPrintable(out.described()));
        QCOMPARE(genko::test::one_line(out.out)["groups"], Json(9));
    }
};

QTEST_GUILESS_MAIN(TestGradientPresets)
#include "test_gradient_presets.moc"
