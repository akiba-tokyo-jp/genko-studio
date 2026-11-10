// PC4 E2 (tool.presets.new・.list・.edit・.reset・.select; SPEC.md COLOR-09「ツールプリセット」): the tool presets of this machine
// (render/tool_presets in render/preset_store's file) — PhotoCraft v0.2.0's engine/src/presets/tests.rs
// tool_presets_save_filter_select (a preset with the colour, the list of one tool, selected, renamed, deleted, reset), what
// a preset may hold, a file that cannot be read never written over, and `genko tool-presets` on a settings folder of its
// own.

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <tuple>
#include <fstream>
#include <iterator>

#include "core/error.hpp"
#include "render/preset_store.hpp"
#include "render/tool_presets.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace store = render::preset_store;
namespace tools = render::tool_presets;
namespace fs = std::filesystem;

class TestToolPresets : public QObject {
    Q_OBJECT

private slots:
    void savedFilteredSelected() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        store::Library lib = store::load(config, tools::kind());
        QVERIFY(!lib.from_file);
        QVERIFY(!lib.groups.empty());  // (the built-in ones)
        // a pen preset with its brush, its fields and the 描画色 (描画色を含める)
        const Json options{{"brush", "my_genko_soft_round"}, {"fields", Json{{"brush_size", 7.5}, {"brush_blend", "multiply"}}}, {"foreground", Json::array({200, 10, 0})}};
        Json out = store::add(lib.groups, tools::kind(), tools::item_of("pen", std::string("七・五"), options), std::nullopt, "pen");
        QCOMPARE(out["name"].get<std::string>(), std::string("七・五"));
        out = store::add(lib.groups, tools::kind(), tools::item_of("eraser", std::nullopt, Json::object()), std::nullopt, "eraser");
        QCOMPARE(out["name"].get<std::string>(), std::string("eraser"));  // (none given: the tool's name)
        store::save(config, tools::kind(), lib.groups);
        lib = store::load(config, tools::kind());
        QVERIFY(lib.from_file);
        // 現在のツールのみ
        const Json erasers = tools::list(lib.groups, std::string_view("eraser"))["presets"];
        QVERIFY(!erasers.empty());
        for (const Json& p : erasers) QCOMPARE(p["tool"].get<std::string>(), std::string("eraser"));
        QVERIFY(tools::list(lib.groups)["presets"].size() > erasers.size());
        // selected: the whole preset (the window switches to its tool and sets its fields)
        const Json& chosen = tools::select(lib.groups, "七・五");
        QCOMPARE(chosen["tool"].get<std::string>(), std::string("pen"));
        QCOMPARE(chosen["options"]["fields"]["brush_size"].get<double>(), 7.5);
        QCOMPARE(chosen["options"]["foreground"], Json::array({200, 10, 0}));
        // renamed, deleted, gone
        store::edit(lib.groups, tools::kind(), Json{{"action", "rename"}, {"preset", "eraser"}, {"name", "E"}});
        store::edit(lib.groups, tools::kind(), Json{{"action", "delete"}, {"preset", "E"}});
        try {
            (void)tools::select(lib.groups, "E");
            QFAIL("a deleted preset was selected");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("key"));
        }
        // reset: the built-in ones again (append: after the person's)
        const auto back = store::reset(lib.groups, tools::kind(), false);
        QVERIFY(store::find(back, "七・五") == nullptr);
        QVERIFY(store::find(store::reset(lib.groups, tools::kind(), true), "七・五") != nullptr);
    }

    void whatAPresetHolds() {
        const auto refused = [](const std::function<void()>& work) {
            try {
                work();
                return false;
            } catch (const core::Error& e) {
                return e.code() == "value";
            }
        };
        QVERIFY(refused([] { tools::item_of("", std::nullopt, Json::object()); }));
        QVERIFY(refused([] { tools::item_of("Pen Tool", std::nullopt, Json::object()); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json::array()); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json{{"something", 1}}); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json{{"foreground", Json::array({300, 0, 0})}}); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json{{"fields", Json{{"x", Json::array()}}}}); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json{{"brush", ""}}); }));
        QVERIFY(refused([] { tools::item_of("pen", std::nullopt, Json{{"fields", Json{{"big", std::string(70000, 'x')}}}}); }));
        // the built-in ones are presets of the window's tools
        for (const store::Group& g : tools::kind().builtin()) {
            for (const Json& item : g.items) tools::kind().check(item);
        }
    }

    void aFileThatCannotBeReadIsNeverWrittenOver() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        fs::create_directories(config);
        std::ofstream(config / "tool_presets.json", std::ios::binary) << "{\"version\": 1, \"groups\": [{\"name\": \"x\", \"items\": [{\"name\": \"a\", \"tool\": \"Bad Tool\"}]}]}";
        const store::Library lib = store::load(config, tools::kind());
        QVERIFY(!lib.problem.empty());
        try {
            store::save(config, tools::kind(), lib.groups);
            QFAIL("a file that cannot be read was written over");
        } catch (const core::Error&) {
        }
        std::ifstream in(config / "tool_presets.json", std::ios::binary);
        QVERIFY(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()).find("Bad Tool") != std::string::npos);
    }

    // `genko tool-presets`: list, new, select, edit, reset on a settings folder of its own
    void theCommandLine() {
        QTemporaryDir dir;
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), dir.path() + "/config");
        const auto cli = [&env](const QStringList& args) {
            const auto ran = genko::test::run_genko(args, env);
            return std::pair{ran.exit_code, genko::test::one_line(ran.out)};
        };
        auto [code, listed] = cli({QStringLiteral("tool-presets"), QStringLiteral("list")});
        QCOMPARE(code, 0);
        QVERIFY(listed["presets"].size() >= 4);
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("new"), QStringLiteral("--tool"), QStringLiteral("eraser"), QStringLiteral("--name"),
                                      QStringLiteral("細い消しゴム"), QStringLiteral("--options"), QStringLiteral("{\"fields\": {\"eraser_size\": 0.5}}"),
                                      QStringLiteral("--foreground"), QStringLiteral("0,0,255")});
        QCOMPARE(code, 0);
        QCOMPARE(listed["name"].get<std::string>(), std::string("細い消しゴム"));
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("list"), QStringLiteral("--tool"), QStringLiteral("eraser")});
        QVERIFY(std::any_of(listed["presets"].begin(), listed["presets"].end(), [](const Json& p) { return p["name"] == "細い消しゴム"; }));
        for (const Json& p : listed["presets"]) QCOMPARE(p["tool"].get<std::string>(), std::string("eraser"));
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("select"), QStringLiteral("細い消しゴム")});
        QCOMPARE(code, 0);
        QCOMPARE(listed["tool"].get<std::string>(), std::string("eraser"));
        QCOMPARE(listed["options"]["foreground"], Json::array({0, 0, 255}));
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("edit"), QStringLiteral("{\"action\": \"delete\", \"preset\": \"細い消しゴム\"}")});
        QCOMPARE(code, 0);
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("select"), QStringLiteral("細い消しゴム")});
        QVERIFY(code != 0);
        QCOMPARE(listed["ok"], Json(false));
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("reset")});
        QCOMPARE(code, 0);
        std::tie(code, listed) = cli({QStringLiteral("tool-presets"), QStringLiteral("new"), QStringLiteral("--tool"), QStringLiteral("Not A Tool")});
        QVERIFY(code != 0);
    }
};

QTEST_GUILESS_MAIN(TestToolPresets)
#include "test_tool_presets.moc"
