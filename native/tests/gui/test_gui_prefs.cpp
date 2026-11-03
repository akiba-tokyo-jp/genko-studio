// PC5 H-e (SPEC.md VIEW-01「環境設定と API」) in the app's window: 環境設定's pages in Photoshop's order of its panes, the
// fields of the new preferences written by 決める (all or none), この面を初期設定に戻す and すべての環境設定をリセット… (the app's
// memories kept), what the panes do in the window (the wheel, the zoom tool's click, the overscroll, the trackpad, the page's
// border, ヒストリー数, the channels in colour, the tooltips, the interpolation a new window starts with, the cache folder);
// ドキュメントをタブで開く off and a floating window docked; `genko prefs set` applied to the running app (its watch), the app and
// the command line writing settings.ini at the same time (QSettings' lock: both kept); commands.json made from the menus
// and checked (GENKO_WRITE_COMMANDS=1 writes it); session.json following the tabs; the history log's text file (never in a
// book) and its metadata in the files written.

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolTip>
#include <QWheelEvent>

#include <fstream>

#include "app/canvas.hpp"
#include "app/channel_panel.hpp"
#include "app/commands_file.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/history_log.hpp"
#include "app/history_panel.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/session_file.hpp"
#include "app/settings_watch.hpp"
#include "core/commands_list.hpp"
#include "core/json.hpp"
#include "core/prefs_table.hpp"
#include "core/session_file.hpp"
#include "formats/file_xmp.hpp"
#include "gui_support.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::Document book_doc(const std::string& title = "環境") {
    core::Document doc = core::new_episode(title, core::Num(1), 2, core::PageSpec::custom(160, 200, 140, 180, 1, 2, 2, 2, 2, 72, "mono"));
    doc.edit_page(0).name_ok = true;
    return doc;
}

// What H-e's panes keep, back as a new person has them (each test begins so).
void fresh() {
    const auto store = app::settings();
    for (const core::prefs::Pref& p : core::prefs::table()) {
        const std::string section = p.path.substr(0, p.path.find('.'));
        if (section == "general" || section == "historyLog" || section == "performance" || section == "scratchDisks" || section == "enhancedControls" ||
            p.path.rfind("interface.", 0) == 0 || p.path.rfind("tools.", 0) == 0 || p.path.rfind("workspace.open", 0) == 0 ||
            p.path.rfind("workspace.enable", 0) == 0) {
            if (p.key.find('*') == std::string::npos) store->remove(QString::fromStdString(p.key));
        }
    }
}

bool set_prefs(const Json& values) {
    const auto store = app::settings();
    return core::prefs::set(*store, values).ok;
}

struct Desk {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    fs::path book;
    Desk() {
        (void)gui_test::config_folder();
        book = gui_test::path_of(tmp.path() + "/book.genko");
        gui_test::write_book(book, book_doc());
        session = app::Session::open(book, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 900);
        window->show();
        QApplication::processEvents();
    }
    ~Desk() {
        for (app::MainWindow* other : app::documents::windows()) {
            if (other != window.get()) delete other;
        }
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    fs::path another(const std::string& name) {
        const fs::path path = gui_test::path_of(tmp.path() + QString::fromStdString("/" + name));
        gui_test::write_book(path, book_doc(name.substr(0, name.find('.'))));
        return path;
    }
};

QString read_text(const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

}  // namespace

class TestGuiPrefs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<genko::app::BookChange>();
    }
    void init() { fresh(); }

    // commands.json is the menus' commands (made from a window as it is now; GENKO_WRITE_COMMANDS=1 writes it).
    void commandsJsonIsTheMenus() {
        const auto store = app::settings();
        store->remove(QStringLiteral("shortcuts"));
        store->remove(QStringLiteral("ui/key_set"));
        store->remove(QStringLiteral("ui/key_set_own"));
        Desk d;
        const Json made = app::commands_catalogue(d.window.get());
        QVERIFY(made["commands"].size() == d.window->actions_by_name().size() || made["commands"].size() > 500);
        const QString file = genko::test::repo_root() + QStringLiteral("/native/src/core/data/commands.json");
        if (qEnvironmentVariableIsSet("GENKO_WRITE_COMMANDS")) {
            std::ofstream out(gui_test::path_of(file), std::ios::binary);  // (a command a line: a change reads as its lines)
            out << "{\"v\": 1, \"commands\": [\n";
            for (std::size_t i = 0; i < made["commands"].size(); ++i) out << (i == 0 ? "" : ",\n") << made["commands"][i].dump();
            out << "\n]}\n";
        }
        const Json kept = Json::parse(read_text(file).toStdString());
        if (kept != made) {
            std::string where;
            (void)genko::test::strict_equal(kept, made, &where);
            QFAIL(qPrintable(QStringLiteral("native/src/core/data/commands.json is not the menus (%1): GENKO_WRITE_COMMANDS=1 test_gui_prefs "
                                            "commandsJsonIsTheMenus writes it")
                                 .arg(QString::fromStdString(where))));
        }
        QCOMPARE(core::commands::catalogue(), made);  // (the one built in: genko commands reads it)
        // a command's place, keys of both sets, a tool, a switch
        for (const Json& c : made["commands"]) {
            if (c["id"] == "act_undo") {
                QCOMPARE(c["menu"], Json::array({"編集"}));
                QCOMPARE(c["keys"]["photoshop"], Json::array({"Ctrl+Z"}));
            }
            if (c["id"] == "act_move") QCOMPARE(c["tool"], Json(true));
            if (c["id"] == "act_extras") QCOMPARE(c["checkable"], Json(true));
        }
    }

    // The pages in Photoshop's order of its panes, each pane's page opened by its id, この面を初期設定に戻す on each.
    void thePagesInPhotoshopsOrder() {
        Desk d;
        app::preferences::PreferencesDialog dialog(d.window.get());
        QStringList titles;
        for (int i = 0; i < dialog.tabs->count(); ++i) titles << dialog.tabs->tabText(i);
        QCOMPARE(titles, (QStringList{"一般", "インターフェイス", "ワークスペース", "ツール", "ヒストリーログ", "ファイル管理", "書き出し", "パフォーマンス",
                                      "仮想記憶ディスク", "カーソル", "透明部分・色域", "単位・定規", "ガイド・グリッド・スライス", "拡張コントロール",
                                      "Camera Raw", "ペンタブレット", "長い時間の作業"}));
        QCOMPARE(dialog.pages_list->count(), dialog.tabs->count());
        for (const app::preferences::Pane& pane : app::preferences::panes()) {
            if (pane.page.isEmpty()) continue;
            dialog.show_pane(pane.id);
            QCOMPARE(dialog.tabs->tabText(dialog.tabs->currentIndex()), pane.page);
            QVERIFY2(dialog.findChild<QPushButton*>(QStringLiteral("prefs_reset_") + pane.id) != nullptr, qPrintable(pane.id));
        }
        // (仮想記憶ディスク says where the caches are and that no pixel goes there)
        auto* note = dialog.findChild<QLabel*>(QStringLiteral("prefs_scratch_note"));
        QVERIFY(note != nullptr && note->text().contains(QStringLiteral("画素")));
    }

    // 決める writes the fields (the window follows); この面を初期設定に戻す puts one pane back; すべての環境設定をリセット… every
    // preference — the app's memories kept.
    void fieldsAndTheirResets() {
        Desk d;
        {
            app::preferences::PreferencesDialog dialog(d.window.get());
            dialog.findChild<QCheckBox*>(QStringLiteral("prefs_general.zoomWithScrollWheel"))->setChecked(true);
            dialog.findChild<QSpinBox*>(QStringLiteral("prefs_performance.historyStates"))->setValue(20);
            auto* border = dialog.findChild<QComboBox*>(QStringLiteral("prefs_interface.canvasBorder"));
            border->setCurrentIndex(border->findData(QStringLiteral("none")));
            dialog.save();
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        }
        QVERIFY(d.canvas()->wheel_zooms);
        QCOMPARE(d.canvas()->page_border, QStringLiteral("none"));
        QCOMPARE(d.window->history()->states, 20);
        QCOMPARE(app::settings()->value(QStringLiteral("ui/history_states")).toInt(), 20);
        // a refusal: nothing written, the dialog kept open
        {
            app::preferences::PreferencesDialog dialog(d.window.get());
            dialog.findChild<QLineEdit*>(QStringLiteral("prefs_historyLog.filePath"))->setText(QStringLiteral("x"));
            QVERIFY(app::preferences::save_pane_fields(&dialog));
        }
        // この面を初期設定に戻す: asked; the other panes' choices kept first, this pane's put back, opened again there
        app::settings()->setValue(QStringLiteral("brush/kind"), QStringLiteral("gpen"));
        d.answers.responder->question = [](const QString&, const QString&) { return true; };
        {
            app::preferences::PreferencesDialog dialog(d.window.get());
            dialog.findChild<QCheckBox*>(QStringLiteral("prefs_enhancedControls.rotateViewWithTrackpad"))->setChecked(true);
            dialog.reset_pane(QStringLiteral("general"));
            QCOMPARE(dialog.reopen, QStringLiteral("general"));
        }
        QVERIFY(!app::settings()->contains(QStringLiteral("ui/wheel_zooms")));
        QVERIFY(!d.canvas()->wheel_zooms);
        QVERIFY(d.canvas()->trackpad_rotate);  // (another pane's, kept by 決める first)
        QCOMPARE(app::settings()->value(QStringLiteral("ui/history_states")).toInt(), 20);
        {
            app::preferences::PreferencesDialog dialog(d.window.get());
            dialog.reset_everything();
            QCOMPARE(dialog.reopen, QStringLiteral("general"));
        }
        QVERIFY(!app::settings()->contains(QStringLiteral("ui/history_states")));
        QVERIFY(!app::settings()->contains(QStringLiteral("ui/trackpad_rotate")));
        QCOMPARE(d.window->history()->states, 1000);
        QCOMPARE(d.canvas()->page_border, QStringLiteral("dropShadow"));
        QCOMPARE(app::settings()->value(QStringLiteral("brush/kind")).toString(), QStringLiteral("gpen"));  // (the app's memory kept)
        app::settings()->remove(QStringLiteral("brush/kind"));
    }

    // What the panes do on the canvas: the wheel zooms, the zoom tool's click centred, the page kept in the view, the
    // trackpad's pinch and turn.
    void theCanvasFollowsThePanes() {
        Desk d;
        app::PageCanvas* canvas = d.canvas();
        canvas->fit_page();
        const auto wheel = [canvas](Qt::KeyboardModifiers modifiers) {
            QWheelEvent event(QPointF(200, 200), canvas->mapToGlobal(QPointF(200, 200)), QPoint(), QPoint(0, 120), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(canvas, &event);
        };
        double before = canvas->view_state().scale;
        wheel(Qt::NoModifier);
        QCOMPARE(canvas->view_state().scale, before);  // (scrolls, as before)
        QVERIFY(set_prefs(Json{{"general.zoomWithScrollWheel", true}, {"tools.overscroll", false}, {"enhancedControls.zoomWithTrackpadPinch", false},
                               {"enhancedControls.rotateViewWithTrackpad", true}}));
        app::preferences::apply_all(d.window.get());
        wheel(Qt::NoModifier);
        QVERIFY(canvas->view_state().scale > before);
        // オーバースクロール off: the page never leaves the view's edges (larger than the view: covering it)
        canvas->set_zoom_percent(400);
        const QPointF pan = canvas->view_state().pan;
        wheel(Qt::ShiftModifier);  // (across: a scroll, never a zoom)
        for (int i = 0; i < 200; ++i) wheel(Qt::ShiftModifier);
        QVERIFY(canvas->view_state().pan.x() <= 0.0 && canvas->view_state().pan.x() >= canvas->width() - 160 * canvas->view_state().scale);
        (void)pan;
        // the trackpad: the pinch does nothing, the turn turns the view
        const double scale = canvas->view_state().scale;
        QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(100, 100), QPointF(100, 100),
                                  canvas->mapToGlobal(QPointF(100, 100)), 0.5, QPointF(), 0);
        QApplication::sendEvent(canvas, &pinch);
        QCOMPARE(canvas->view_state().scale, scale);
        QNativeGestureEvent turn(Qt::RotateNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(100, 100), QPointF(100, 100),
                                 canvas->mapToGlobal(QPointF(100, 100)), 15, QPointF(), 0);
        QApplication::sendEvent(canvas, &turn);
        QVERIFY(std::abs(canvas->view_state().rotation - 15) < 0.01);
        canvas->set_rotation(0);
        // クリックした点を中央にズーム: the zoom tool's click brings the point to the middle
        QVERIFY(set_prefs(Json{{"tools.zoomClickedPointToCenter", true}, {"general.animatedZoom", false}, {"tools.overscroll", true}}));
        app::preferences::apply_all(d.window.get());
        canvas->fit_page();
        d.window->choose_tool(QStringLiteral("zoom"));
        const QPoint at(canvas->width() / 2 + 80, canvas->height() / 2 + 60);
        const QPointF mm = canvas->to_mm(QPointF(at));
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, at);
        const QPointF middle = canvas->to_mm(QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        QVERIFY2(std::abs(middle.x() - mm.x()) < 1.0 && std::abs(middle.y() - mm.y()) < 1.0,
                 qPrintable(QStringLiteral("%1,%2 / %3,%4").arg(middle.x()).arg(middle.y()).arg(mm.x()).arg(mm.y())));
    }

    // The window's other followers: the channels' pictures in colour, the tooltips (all, or the tools' alone), the
    // interpolation a new window starts with, the cache folder.
    void theWindowFollowsThePanes() {
        Desk d;
        QVERIFY(!d.window->channel_panel()->in_colour);
        QVERIFY(set_prefs(Json{{"interface.showChannelsInColor", true}, {"general.imageInterpolation", "bicubic"}}));
        app::preferences::apply_all(d.window.get());
        QVERIFY(d.window->channel_panel()->in_colour);
        QCOMPARE(QString::fromStdString(d.window->interpolation()), QStringLiteral("bilinear"));  // (this window began so)
        {
            app::MainWindow other;
            QCOMPARE(QString::fromStdString(other.interpolation()), QStringLiteral("bicubic"));
        }
        // the tooltips
        // (the application's filter sees a tooltip's event before the button: what reaches the button would be shown)
        struct Seen : QObject {
            bool got = false;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::ToolTip) got = true;
                return event->type() == QEvent::ToolTip;
            }
        } seen;
        QPushButton button(QStringLiteral("x"));
        button.setToolTip(QStringLiteral("説明"));
        button.installEventFilter(&seen);
        const auto tip = [&button, &seen] {
            seen.got = false;
            QHelpEvent help(QEvent::ToolTip, QPoint(2, 2), button.mapToGlobal(QPoint(2, 2)));
            QApplication::sendEvent(&button, &help);
            return seen.got;
        };
        QVERIFY(tip());
        QVERIFY(set_prefs(Json{{"interface.showTooltips", false}}));
        app::preferences::apply_all(d.window.get());
        QVERIFY(!tip());
        QVERIFY(set_prefs(Json{{"interface.showTooltips", true}, {"tools.showTooltips", false}}));
        app::preferences::apply_all(d.window.get());
        QVERIFY(tip());  // (not a tool's: shown)
        QVERIFY(set_prefs(Json{{"tools.showTooltips", true}}));
        app::preferences::apply_all(d.window.get());
        // 仮想記憶ディスク: the caches' folder
        QTemporaryDir scratch;
        QVERIFY(set_prefs(Json{{"scratchDisks.folder", scratch.path().toStdString()}}));
        QVERIFY(gui_test::qpath(app::cache_root()).startsWith(scratch.path()));
        QVERIFY(set_prefs(Json{{"scratchDisks.folder", ""}}));
        QVERIFY(gui_test::qpath(app::cache_root()).startsWith(QString::fromUtf8(qgetenv("GENKO_CONFIG_DIR"))));
    }

    // genko prefs set, while the app runs: applied there (its watch of settings.ini); the app and the command line writing
    // at once — both kept.
    void theCommandLineReachesTheApp() {
        Desk d;
        QVERIFY(!d.canvas()->wheel_zooms);
        (void)app::settings_changed_now();
        const genko::test::Run r = genko::test::run(genko::test::genko_cli(), {"prefs", "set", "general.zoomWithScrollWheel", "true"},
                                                    QProcessEnvironment::systemEnvironment(), 60000);
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        QVERIFY(gui_test::wait_for([&d] {
            (void)app::settings_changed_now();
            return d.canvas()->wheel_zooms;
        }));
        // at once: the command line sets one, the app another, over and over
        QProcess writer;
        writer.start(genko::test::genko_cli(), {"prefs", "set", "--values", R"({"interface.theme": "dark", "tools.overscroll": false})"});
        for (int i = 0; i < 40; ++i) app::settings()->setValue(QStringLiteral("ui/brush_preview"), i % 2 == 0 ? QStringLiteral("false") : QStringLiteral("true"));
        QVERIFY(writer.waitForFinished(60000));
        QCOMPARE(writer.exitCode(), 0);
        const auto store = core::prefs::store();
        QCOMPARE(store->value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        QCOMPARE(store->value(QStringLiteral("ui/overscroll")).toString(), QStringLiteral("false"));
        QCOMPARE(store->value(QStringLiteral("ui/brush_preview")).toString(), QStringLiteral("true"));
        QVERIFY(gui_test::wait_for([&d] {
            (void)app::settings_changed_now();
            return !d.canvas()->overscroll;
        }));
        app::settings()->remove(QStringLiteral("ui/theme"));
        app::settings()->remove(QStringLiteral("ui/brush_preview"));
    }

    // session.json: the windows, their tabs, the book in front, the colours — written when they change.
    void sessionJsonFollowsTheTabs() {
        Desk d;
        (void)app::session_file::write_now();
        Json read = core::session_file::read();
        QCOMPARE(read["running"], Json(true));
        QCOMPARE(read["pid"], Json(static_cast<std::int64_t>(QCoreApplication::applicationPid())));
        const auto tabs_of_desk = [&d](const Json& session) {
            for (const Json& w : session["windows"]) {
                for (const Json& t : w["tabs"]) {
                    if (t.value("path", std::string()) == core::path_to_utf8(d.book)) return w["tabs"];
                }
            }
            return Json();
        };
        Json tabs = tabs_of_desk(read);
        QCOMPARE(tabs.size(), std::size_t{1});
        QCOMPARE(tabs[0]["title"], Json("環境 第1話"));
        QCOMPARE(tabs[0]["saved"], Json(true));
        QCOMPARE(tabs[0]["page"], Json(1));
        QVERIFY(read["foreground"].is_string() && read["foreground"].get<std::string>().size() == 7);
        QVERIFY(!app::session_file::write_now());  // (nothing changed: not written)
        d.window->open_project(d.another("二冊目.genko"));
        QVERIFY(gui_test::wait_for([&d] { return d.window->documents().size() == 2; }));
        QVERIFY(app::session_file::write_now());
        tabs = tabs_of_desk(core::session_file::read());
        QCOMPARE(tabs.size(), std::size_t{2});
        QCOMPARE(tabs[1]["title"], Json("二冊目 第1話"));
    }

    // ドキュメントをタブで開く off: a book opened in a window of its own; that window let go over the first one's tabs docks there.
    void ownWindowsAndDocking() {
        Desk d;
        QVERIFY(set_prefs(Json{{"workspace.openDocumentsAsTabs", false}}));
        const std::size_t windows = app::documents::windows().size();
        d.window->open_project(d.another("別窓.genko"));
        QVERIFY(gui_test::wait_for([&] { return app::documents::windows().size() == windows + 1; }));
        app::MainWindow* own = nullptr;
        QVERIFY(gui_test::wait_for([&] {  // (the book read on a worker: its window shows it once read)
            for (app::MainWindow* w : app::documents::windows()) {
                if (w != d.window.get() && w->documents().size() == 1 && w->documents().front().title().startsWith(QStringLiteral("別窓"))) own = w;
            }
            return own != nullptr;
        }));
        QCOMPARE(d.window->documents().size(), std::size_t{1});
        // not over the tabs: stays; over them: docked, its window closed
        QVERIFY(!own->dock_over(d.window->mapToGlobal(QPoint(-500, -500))));
        QVERIFY(set_prefs(Json{{"workspace.enableFloatingDocumentWindowDocking", false}}));
        const QPoint tabs_at = d.window->centralWidget()->mapToGlobal(QPoint(30, 6));
        QVERIFY(!own->dock_over(tabs_at));
        QVERIFY(set_prefs(Json{{"workspace.enableFloatingDocumentWindowDocking", true}}));
        QVERIFY(own->dock_over(tabs_at));
        QCOMPARE(d.window->documents().size(), std::size_t{2});
        QVERIFY(gui_test::wait_for([&] { return app::documents::windows().size() == windows; }));
        QVERIFY(set_prefs(Json{{"workspace.openDocumentsAsTabs", true}}));
    }

    // ヒストリーログ → テキストファイル: a line for each change (詳細: the ops' names), and never a file in a book.
    void historyLogInATextFile() {
        QTemporaryDir logs;
        const QString file = logs.path() + QStringLiteral("/log.txt");
        QVERIFY(set_prefs(Json{{"historyLog", Json{{"enabled", true}, {"destination", "textFile"}, {"filePath", file.toStdString()}, {"detail", "detailed"}}}}));
        Desk d;
        QVERIFY(d.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "h"}, {"at", 10}}})));
        const QString text = read_text(file);
        QVERIFY2(text.contains(QStringLiteral("環境\t開く")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("add_ruler")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("環境\t")), qPrintable(text));
        const int lines = static_cast<int>(text.count(QLatin1Char('\n')));
        // 簡潔: the words alone
        QVERIFY(set_prefs(Json{{"historyLog.detail", "concise"}}));
        QVERIFY(d.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 12}}})));
        const QString more = read_text(file);
        QCOMPARE(static_cast<int>(more.count(QLatin1Char('\n'))), lines + 1);
        QVERIFY(!more.section(QLatin1Char('\n'), -2, -2).contains(QStringLiteral("add_ruler")));
        // a file in the book: refused (said once), nothing written there
        const QString inside = gui_test::qpath(d.book / "log.txt");
        QVERIFY(set_prefs(Json{{"historyLog.filePath", inside.toStdString()}}));
        QVERIFY(d.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 20}}})));
        QVERIFY(!QFile::exists(inside));
        QVERIFY(set_prefs(Json{{"historyLog.enabled", false}}));
        QVERIFY(d.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 30}}})));
        QCOMPARE(read_text(file), more);  // (off: nothing more)
    }

    // ヒストリーログ → メタデータ: the files written from the book carry its saved edits as photoshop:History; off, as before.
    void historyLogInTheMetadata() {
        Desk d;
        QVERIFY(d.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "h"}, {"at", 10}}})));
        d.session->save_now();
        QVERIFY(gui_test::wait_for([&d] { return !d.session->unsaved(); }));
        const core::Document doc = *d.session->snapshot();
        QVERIFY(!formats::book_xmp(doc).has_value());  // (no File Info, no log: none, as before)
        QVERIFY(set_prefs(Json{{"historyLog", Json{{"enabled", true}, {"destination", "metadata"}, {"detail", "detailed"}}}}));
        const auto xmp = formats::book_xmp(doc);
        QVERIFY(xmp.has_value());
        QVERIFY2(xmp->find("<photoshop:History>") != std::string::npos && xmp->find("add_ruler") != std::string::npos, xmp->c_str());
        QVERIFY(set_prefs(Json{{"historyLog.destination", "textFile"}}));
        QVERIFY(!formats::book_xmp(doc).has_value());
        QVERIFY(set_prefs(Json{{"historyLog.enabled", false}}));
    }
};

QTEST_MAIN(TestGuiPrefs)
#include "test_gui_prefs.moc"
