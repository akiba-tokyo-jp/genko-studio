// PC4 E2 (window.panel.toolPresets, tool.presets.new・.list・.edit・.reset・.select; SPEC.md COLOR-09「ツールプリセット」) on
// the offscreen platform: ウィンドウ → ツールプリセット — the machine's tool presets, 現在のツールのみ following the tool in
// hand; 新規ツールプリセット… keeps the tool in hand with its page's settings, its brush and (asked) the 描画色; a click
// takes one (its tool, brush, settings and colour); renamed, deleted, 初期設定に戻す; what another window or
// `genko tool-presets` wrote in between is kept.

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTreeWidget>

#include <filesystem>
#include <fstream>

#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/tool_presets_panel.hpp"
#include "render/preset_store.hpp"
#include "render/tool_presets.hpp"

using namespace genko;
using core::Json;
namespace store = render::preset_store;
namespace tools = render::tool_presets;

namespace {

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, core::new_episode("道具", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color")));
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
    }
    app::ToolPresetsPanel* panel() const { return window->tool_presets_panel(); }
    QStringList shown() const {
        QStringList out;
        for (int i = 0; i < panel()->tree()->topLevelItemCount(); ++i) out << panel()->tree()->topLevelItem(i)->text(0);
        return out;
    }
    void choose(const QString& name) const {
        QTreeWidget* tree = panel()->tree();
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name) {
                tree->setCurrentItem(tree->topLevelItem(i));
                return;
            }
        QFAIL(qPrintable(QStringLiteral("no preset ") + name));
    }
    void click(const QString& name) const {
        choose(name);
        emit panel()->tree()->itemClicked(panel()->tree()->currentItem(), 0);
    }
};

const Json* kept(const std::string& name) {
    static store::Library lib;
    lib = store::load(app::config_dir(), tools::kind());
    return store::find(lib.groups, name);
}

}  // namespace

class TestGuiToolPresets : public QObject {
    Q_OBJECT
private slots:
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        std::error_code ec;
        std::filesystem::remove(app::config_dir() / "tool_presets.json", ec);
        std::filesystem::remove(app::config_dir() / "brushes.json", ec);
    }

    // The panel in its dock (ウィンドウ → ツールプリセット); 現在のツールのみ follows the tool in hand, or all are shown.
    void theListFollowsTheTool() {
        Studio s;
        QDockWidget* dock = s.window->tool_presets_dock();
        QVERIFY(dock != nullptr && dock->widget() == s.panel());
        dock->toggleViewAction()->trigger();
        QVERIFY(s.panel()->current_only->isChecked());
        s.window->action(QStringLiteral("act_pen"))->trigger();
        QVERIFY(s.shown().contains(QStringLiteral("G ペン 0.6 mm")));
        QVERIFY(!s.shown().contains(QStringLiteral("やわらかい消しゴム 6 mm")));
        s.window->action(QStringLiteral("act_eraser"))->trigger();
        QCOMPARE(s.shown(), QStringList{QStringLiteral("やわらかい消しゴム 6 mm")});
        s.panel()->current_only->setChecked(false);
        QVERIFY(s.shown().size() >= 5);
        QCOMPARE(app::settings()->value(QStringLiteral("tool_presets/current_only")).toString(), QStringLiteral("false"));
    }

    // 新規ツールプリセット…: the eraser with its size and the 描画色 (asked: yes); a click on it later sets them back.
    void aPresetKeptAndTaken() {
        Studio s;
        s.window->tool_presets_dock()->toggleViewAction()->trigger();
        s.window->action(QStringLiteral("act_eraser"))->trigger();
        auto* size = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("eraser_size"));
        QVERIFY(size != nullptr);
        size->setValue(12);
        s.window->brush_panel()->set_colour({0, 0, 255});
        s.panel()->new_preset(QStringLiteral("太い消しゴム"), true);
        const Json* item = kept("太い消しゴム");
        QVERIFY(item != nullptr);
        QCOMPARE((*item)["tool"].get<std::string>(), std::string("eraser"));
        QCOMPARE((*item)["options"]["fields"]["eraser_size"].get<double>(), 12.0);
        QCOMPARE((*item)["options"]["foreground"], Json::array({0, 0, 255}));
        QVERIFY(s.shown().contains(QStringLiteral("太い消しゴム")));
        // the 描画色 not asked for: none kept
        s.panel()->new_preset(QStringLiteral("色なし"), false);
        QVERIFY(!(*kept("色なし"))["options"].contains("foreground"));
        // taken back: the pen first, other settings
        s.window->action(QStringLiteral("act_pen"))->trigger();
        size->setValue(3);
        s.window->brush_panel()->set_colour({10, 10, 10});
        s.panel()->current_only->setChecked(false);
        s.click(QStringLiteral("太い消しゴム"));
        QCOMPARE(s.window->tool_in_hand(), QStringLiteral("eraser"));
        QCOMPARE(size->value(), 12.0);
        QCOMPARE(s.window->brush_panel()->rgb(), (std::array<int, 3>{0, 0, 255}));
    }

    // A built-in pen preset: the pen, the dab brush (under the key the book takes for it) and its size.
    void aPenPresetChoosesItsBrush() {
        Studio s;
        s.window->tool_presets_dock()->toggleViewAction()->trigger();
        s.panel()->current_only->setChecked(false);
        QVERIFY(s.panel()->select(QStringLiteral("ソフト円ブラシ 10 mm")));
        QCOMPARE(s.window->tool_in_hand(), QStringLiteral("pen"));
        QCOMPARE(s.window->brush_panel()->kind(), std::string("my_genko_soft_round"));
        auto* size = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("brush_size"));
        QVERIFY(size != nullptr);
        QCOMPARE(size->value(), 10.0);
        // kept as a new one: the brush in hand kept under the library's key
        s.panel()->new_preset(QStringLiteral("自分の円"), false);
        QCOMPARE((*kept("自分の円"))["options"]["brush"].get<std::string>(), std::string("my_genko_soft_round"));
        // a preset whose brush is gone: said, the rest taken
        {
            store::Library lib = store::load(app::config_dir(), tools::kind());
            store::add(lib.groups, tools::kind(), tools::item_of("pen", std::string("消えたブラシ"), Json{{"brush", "my_gone"}, {"fields", Json{{"brush_size", 2.5}}}}),
                       std::nullopt, "pen");
            store::save(app::config_dir(), tools::kind(), lib.groups);
        }
        QVERIFY(s.panel()->select(QStringLiteral("消えたブラシ")));
        QCOMPARE(size->value(), 2.5);
        QCOMPARE(s.window->brush_panel()->kind(), std::string("my_genko_soft_round"));
    }

    // 名前を変更…, 削除, 初期設定に戻す; a preset another window (or `genko tool-presets`) added in between is kept.
    void renamedDeletedReset() {
        Studio s;
        s.window->tool_presets_dock()->toggleViewAction()->trigger();
        s.panel()->current_only->setChecked(false);
        s.window->action(QStringLiteral("act_pen"))->trigger();
        s.panel()->new_preset(QStringLiteral("ペン A"), false);
        {  // (another window's, written while this panel shows its list)
            store::Library lib = store::load(app::config_dir(), tools::kind());
            store::add(lib.groups, tools::kind(), tools::item_of("eraser", std::string("よその消しゴム"), Json::object()), std::nullopt, "eraser");
            store::save(app::config_dir(), tools::kind(), lib.groups);
        }
        s.choose(QStringLiteral("ペン A"));
        s.panel()->rename(QStringLiteral("ペン B"));
        QVERIFY(kept("ペン A") == nullptr && kept("ペン B") != nullptr);
        QVERIFY(kept("よその消しゴム") != nullptr);
        QVERIFY(s.shown().contains(QStringLiteral("よその消しゴム")));
        s.choose(QStringLiteral("ペン B"));
        s.panel()->remove();
        QVERIFY(kept("ペン B") == nullptr);
        QVERIFY(!s.shown().contains(QStringLiteral("ペン B")));
        s.panel()->reset();
        QVERIFY(kept("よその消しゴム") == nullptr);
        QVERIFY(kept("G ペン 0.6 mm") != nullptr);
        // a file that cannot be read: shown as the built-in ones, never written over (said)
        {
            std::ofstream out(app::config_dir() / "tool_presets.json", std::ios::binary);
            out << "{\"version\": 1, \"groups\": [";
        }
        s.panel()->refresh();
        QVERIFY(s.shown().last().contains(QStringLiteral("読めない")));
        s.panel()->new_preset(QStringLiteral("書けない"), false);
        QVERIFY(!s.answers.asked.isEmpty() && s.answers.asked.back().contains(QStringLiteral("作れませんでした")));
    }
};

QTEST_MAIN(TestGuiToolPresets)
#include "test_gui_tool_presets.moc"
