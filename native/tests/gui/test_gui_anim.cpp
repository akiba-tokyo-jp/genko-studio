// Animation and the timelapse in the window (M3③, Python's tests/test_j12.py test_the_timeline_in_the_app and the
// timelapse commands of main.py) on the offscreen platform: a page made an animation from the timeline (a folder and its
// first cel, the drawing target), a cel added from a later frame, the frame's cel followed as the target, the exposure
// sheet in the table, a cell's menu, the camera, playing, the onion skin on the canvas, writing it out; the timelapse
// turned on (a saved book only), a picture of each changed page at each save, written out from its dialog; the filter
// plugins chosen in their settings (asked once more) and then offered among the layer's filters.
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <fstream>
#include <map>
#include "gui_support.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/plugins_dialog.hpp"
#include "app/preferences.hpp"
#include "app/tiles.hpp"
#include "app/timeline.hpp"
#include "service/wording.hpp"
#include "core/anim.hpp"
#include "core/video.hpp"
#include "render/plugins.hpp"
#include "render/timelapse.hpp"
#include "testsupport.hpp"
using namespace genko;
using core::Json;

namespace {

core::Document book_doc() {
    core::Document doc = core::new_episode("t", core::Num(1), 2, core::PageSpec::b5_doujin());
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).name_ok = true;
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::filesystem::path book;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    explicit Studio(const core::Document& doc = book_doc()) {
        (void)gui_test::config_folder();
        book = gui_test::path_of(tmp.path() + "/w.genko");
        gui_test::write_book(book, doc);
        session = app::Session::open(book, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1280, 800);
        window->show();
    }
    const core::Page& page() const { return *window->current_page(); }
    Json ball(double x, const std::string& layer) const {
        return Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", layer},
                                 {"points", Json::array({Json::array({x, 100}), Json::array({x + 20, 100}), Json::array({x + 20, 120}),
                                                         Json::array({x, 120}), Json::array({x, 100})})},
                                 {"width_mm", 3}}});
    }
    bool saved() const {
        return gui_test::wait_for([&] { return !session->unsaved(); }, 20000);
    }
};

}  // namespace

namespace {

// フィルター → プラグイン's filters (PC4 節 D: the layer panel's list is the フィルター menu now), made again as it opens:
// each plugin's kind and its words.
std::map<QString, QString> plugin_filters(app::MainWindow* w) {
    std::map<QString, QString> out;
    auto* menu = w->findChild<QMenu*>(QStringLiteral("menu_filter_plugins"));
    if (menu == nullptr) return out;
    emit w->filter_menu()->aboutToShow();
    for (QAction* a : menu->actions())
        if (a->data().toString().startsWith(QStringLiteral("plugin:"))) out[a->data().toString()] = a->text();
    return out;
}

}  // namespace

class TestGuiAnim : public QObject {
    Q_OBJECT
private slots:
    void timelineInTheApp() {
        Studio s;
        app::MainWindow* w = s.window.get();
        w->show_dock(QStringLiteral("タイムライン"));
        app::TimelinePanel* panel = w->timeline();
        QVERIFY(panel->start->isVisible());
        panel->start_animation();  // (this page becomes an animation with a folder and its first cel, the drawing target)
        QVERIFY(core::anim::is_animation(s.page()));
        QCOMPARE(panel->table->rowCount(), 1);
        QCOMPARE(panel->table->columnCount(), 24);
        const std::string first = w->target_layer()->id;
        const Json folder = core::anim::folders(s.page()).front();
        QCOMPARE(core::anim::cel_at(s.page(), folder, 1), Json(first));
        QVERIFY(w->apply_ops(s.ball(30, first)));
        panel->set_frame(5);
        panel->add_cel();  // (a new cel from frame 5, drawn on next)
        const std::string second = w->target_layer()->id;
        QVERIFY(second != first);
        QCOMPARE(core::anim::cel_at(s.page(), Json(*panel->current_folder()), 5), Json(second));
        QVERIFY(w->apply_ops(s.ball(90, second)));
        panel->set_frame(2);
        QCOMPARE(w->target_layer()->id, first);  // (the frame's cel becomes the drawing target)
        QCOMPARE(w->current_frame(), std::int64_t{2});
        QCOMPARE(panel->table->item(0, 4)->text(), QStringLiteral("2"));
        QCOMPARE(panel->table->item(0, 0)->text(), QStringLiteral("1"));
        // the canvas: frame 2 (cel 1 only), and cel 2 faint after it (the onion skin)
        QCOMPARE(w->canvas()->renderer().anim_frame(), std::int64_t{2});
        // (drawn as the canvas is now: its view moves when the first steps over the empty book go with the first
        // stroke, and the tiles for it are asked a moment after — asked again while the view still moves)
        const auto drawn = [w] {
            QImage out;
            for (int i = 0; i < 20 && out.isNull(); ++i) {
                if (!w->canvas()->wait_rendered(30000)) break;
                out = w->canvas()->renderer().compose(w->canvas()->base_dpi());
            }
            return out;
        };
        const QImage with_onion = drawn();
        panel->onion->setChecked(false);
        const QImage without = drawn();
        QVERIFY2(!with_onion.isNull() && with_onion.size() == without.size(),
                 qPrintable(QStringLiteral("%1×%2 and %3×%4 at %5 dpi")
                                .arg(with_onion.width()).arg(with_onion.height()).arg(without.width()).arg(without.height()).arg(w->canvas()->base_dpi())));
        QVERIFY(with_onion != without);
        panel->onion->setChecked(true);
        // playing: one frame at a time, the frames drawn once each
        panel->play_button->setChecked(true);
        for (int i = 0; i < 3; ++i) panel->tick();
        QCOMPARE(panel->frame, std::int64_t{5});
        QVERIFY(w->canvas()->showing_frame());
        panel->play_button->setChecked(false);
        QVERIFY(!w->canvas()->showing_frame());
        // a cell's menu: nothing shown at frame 4
        QMenu* menu = panel->cell_menu(0, 3);
        QVERIFY(menu != nullptr);
        QAction* empty = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->text() == QStringLiteral("何も出さない（空セル）")) empty = a;
        }
        QVERIFY(empty != nullptr);
        empty->trigger();
        delete menu;
        QCOMPARE(core::anim::cel_at(s.page(), folder, 4), Json());
        // the length and speed
        panel->frames->setValue(12);
        emit panel->frames->editingFinished();
        QCOMPARE(core::anim::frames_of(s.page()), std::int64_t{12});
        QCOMPARE(panel->table->columnCount(), 12);
        // the camera: the part of the page in sight, at this frame
        panel->set_frame(3);
        panel->camera_here();
        QVERIFY(core::anim::camera_at(s.page(), 3).has_value());
        // written out (PC5 J-e1: ビデオをレンダリング's window — all the frames, as it opens — then where; the format from the
        // file's name)
        s.answers.responder->exec = [](QDialog* dialog) {
            return qobject_cast<app::RenderVideoDialog*>(dialog) != nullptr ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        const QString gif = s.tmp.path() + "/w.gif";
        s.answers.responder->save_path_filtered = [gif](const QString&, const QString&, const QString&) {
            return std::pair<QString, QString>(gif, QStringLiteral("GIF (*.gif)"));
        };
        panel->export_button->click();
        QVERIFY2(QFile::exists(gif), qPrintable(w->last_error() + s.answers.asked.join(QStringLiteral(" | "))));
        QVERIFY(w->last_notice().startsWith(QStringLiteral("アニメーションを書き出しました")));
        // numbered PNGs in a folder
        const QString folder_out = s.tmp.path() + "/frames";
        s.answers.responder->save_path_filtered = [folder_out](const QString&, const QString&, const QString&) {
            return std::pair<QString, QString>(folder_out, QStringLiteral("連番 PNG（フォルダー） (*)"));
        };
        panel->export_button->click();
        QCOMPARE(QDir(folder_out).entryList({QStringLiteral("frame_*.png")}, QDir::Files).size(), 12);
        // the command is in the ページ menu
        QMenu* pages = nullptr;
        for (QAction* a : w->menuBar()->actions()) {
            if (a->text() == QStringLiteral("ページ")) pages = a->menu();
        }
        QVERIFY(pages != nullptr && pages->actions().contains(w->action("act_timeline")));
        // an ordinary page again
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_animation"}, {"page", 1}, {"off", true}}})));
        panel->refresh();
        QVERIFY(panel->start->isVisible());
        QVERIFY(!panel->table->isEnabled());
    }

    // PC5 J-e1 (SPEC.md PROD-01: timeline.*, window.panel.timeline, file.export.renderVideo): Photoshop's names (ページ →
    // ビデオタイムラインを作成 — its old name read —, ウィンドウ → タイムライン, ファイル → 書き出し ▸ ビデオをレンダリング…), the
    // panel's menu, the frame moved by the keys while the panel has the focus (← → Home End, Shift ← → to the work area's
    // ends; the canvas keeps its arrows) and by フレームへ移動…, the work area (its boxes; played within it; the page names
    // timeline@1), an fps past 60, ビデオをレンダリング's range and speed, タイムラインを削除, the words of the refusals.
    void theTimelineMovesAndRenders() {
        Studio s;
        app::MainWindow* w = s.window.get();
        // the names
        QCOMPARE(w->action("act_timeline")->text(), QStringLiteral("ビデオタイムラインを作成"));
        QVERIFY(app::preferences::old_names(QStringLiteral("ビデオタイムラインを作成")).contains(QStringLiteral("アニメーション（タイムライン）")));
        QCOMPARE(w->findChild<QDockWidget*>(QStringLiteral("タイムライン"))->windowTitle(), QStringLiteral("タイムライン"));
        QCOMPARE(w->action("act_render_video")->text(), QStringLiteral("ビデオをレンダリング…"));
        QVERIFY(w->findChild<QMenu*>(QStringLiteral("menu_export"))->actions().contains(w->action("act_render_video")));
        const std::vector<std::pair<const char*, QString>> moves{{"act_next_frame", QStringLiteral("次のフレーム")},
                                                                  {"act_previous_frame", QStringLiteral("前のフレーム")},
                                                                  {"act_first_frame", QStringLiteral("最初のフレーム")},
                                                                  {"act_last_frame", QStringLiteral("最後のフレーム")},
                                                                  {"act_work_start", QStringLiteral("作業範囲の開始点へ移動")},
                                                                  {"act_work_end", QStringLiteral("作業範囲の終了点へ移動")},
                                                                  {"act_go_to_frame", QStringLiteral("フレームへ移動…")}};
        for (const auto& [name, text] : moves) QCOMPARE(w->action(name)->text(), text);
        // a page that is not an animation: said so
        w->action("act_next_frame")->trigger();
        QVERIFY(w->last_notice().startsWith(QStringLiteral("このページはアニメーションではありません")));
        // ビデオタイムラインを作成 from ページ: the panel shown and the page an animation
        w->action("act_timeline")->trigger();
        QVERIFY(core::anim::is_animation(s.page()));
        app::TimelinePanel* panel = w->timeline();
        QVERIFY(panel->isVisible());
        QCOMPARE(core::anim::frames_of(s.page()), std::int64_t{24});
        // its menu
        QStringList items;
        for (QAction* a : panel->panel_menu->actions()) items << (a->isSeparator() ? QStringLiteral("|") : a->menu() ? a->menu()->title() + QStringLiteral(" ▸") : a->text());
        QCOMPARE(items, (QStringList{QStringLiteral("ビデオタイムラインを作成"), QStringLiteral("タイムラインを削除"), QStringLiteral("|"), QStringLiteral("移動 ▸"),
                                     QStringLiteral("作業範囲 ▸"), QStringLiteral("|"), QStringLiteral("ビデオをレンダリング…")}));
        // the keys: while the panel has the focus only, the same in both key sets
        QCOMPARE(w->action("act_next_frame")->shortcuts(), QList<QKeySequence>{QKeySequence(Qt::Key_Right)});
        QCOMPARE(w->action("act_work_end")->shortcuts(), QList<QKeySequence>{QKeySequence(Qt::SHIFT | Qt::Key_Right)});
        QCOMPARE(w->action("act_next_frame")->shortcutContext(), Qt::WidgetWithChildrenShortcut);
        w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w));
        panel->table->setFocus();
        QVERIFY(gui_test::wait_for([&] { return panel->table->hasFocus(); }, 5000));
        QTest::keyClick(panel->table, Qt::Key_Right);
        QCOMPARE(w->current_frame(), std::int64_t{2});
        QTest::keyClick(panel->table, Qt::Key_End);
        QCOMPARE(w->current_frame(), std::int64_t{24});
        QTest::keyClick(panel->table, Qt::Key_Right);  // (not past the last)
        QCOMPARE(w->current_frame(), std::int64_t{24});
        QTest::keyClick(panel->table, Qt::Key_Left);
        QCOMPARE(w->current_frame(), std::int64_t{23});
        QTest::keyClick(panel->table, Qt::Key_Home);
        QCOMPARE(w->current_frame(), std::int64_t{1});
        QCOMPARE(panel->where->text(), QStringLiteral("1 / 24（0.00 秒）"));
        w->canvas()->setFocus();
        QVERIFY(gui_test::wait_for([&] { return w->canvas()->hasFocus(); }, 5000));
        QTest::keyClick(w->canvas(), Qt::Key_Right);  // (the canvas's arrow: not the timeline's)
        QCOMPARE(w->current_frame(), std::int64_t{1});
        // the work area, from its boxes: the book names timeline@1
        panel->work_first->setValue(5);
        panel->work_last->setValue(10);
        panel->work->setChecked(true);
        QCOMPARE(core::work_area(s.page()), (std::optional<std::array<std::int64_t, 2>>{{5, 10}}));
        QVERIFY(core::page_uses_timeline(s.page()));
        QVERIFY(panel->work_first->isEnabled());
        panel->table->setFocus();
        QVERIFY(gui_test::wait_for([&] { return panel->table->hasFocus(); }, 5000));
        QTest::keyClick(panel->table, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(w->current_frame(), std::int64_t{10});
        QTest::keyClick(panel->table, Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(w->current_frame(), std::int64_t{5});
        // played within it
        panel->set_frame(10);
        panel->play_button->setChecked(true);
        panel->tick();
        QCOMPARE(panel->frame, std::int64_t{5});
        panel->play_button->setChecked(false);
        // フレームへ移動…
        s.answers.responder->get_int = [](const QString& title, const QString&, int, int lo, int hi) {
            return title == QStringLiteral("フレームへ移動") && lo == 1 && hi == 24 ? std::optional<int>(7) : std::optional<int>();
        };
        w->action("act_go_to_frame")->trigger();
        QCOMPARE(w->current_frame(), std::int64_t{7});
        // an fps past 60
        panel->fps->setValue(90);
        emit panel->fps->editingFinished();
        QCOMPARE(core::anim::fps_of(s.page()), 90.0);
        // ビデオをレンダリング: the work area at 24 fps, as numbered PNGs (frames 5..10)
        QString range_offered;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::RenderVideoDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            range_offered = d->range->currentText();
            d->fps->setValue(24);
            return int(QDialog::Accepted);
        };
        const QString folder_out = s.tmp.path() + "/work";
        s.answers.responder->save_path_filtered = [folder_out](const QString&, const QString&, const QString&) {
            return std::pair<QString, QString>(folder_out, QStringLiteral("連番 PNG（フォルダー） (*)"));
        };
        w->action("act_render_video")->trigger();
        QCOMPARE(range_offered, QStringLiteral("作業範囲（5〜10）"));
        QCOMPARE(QDir(folder_out).entryList({QStringLiteral("frame_*.png")}, QDir::Files).size(), 6);
        {
            app::RenderVideoDialog d(w, s.page());
            QCOMPARE(d.frames(), (std::array<std::int64_t, 2>{5, 10}));
            QVERIFY(!d.speed().has_value());  // (the page's own)
            d.range->setCurrentIndex(d.range->findData(QStringLiteral("all")));
            QCOMPARE(d.frames(), (std::array<std::int64_t, 2>{1, 24}));
            d.range->setCurrentIndex(d.range->findData(QStringLiteral("custom")));
            d.first->setValue(12);
            d.last->setValue(3);
            QCOMPARE(d.frames(), (std::array<std::int64_t, 2>{3, 12}));
            d.fps->setValue(0.5);
            QCOMPARE(d.speed(), std::optional<double>(0.5));
            d.size->setValue(320);
            QCOMPARE(d.width(), std::optional<int>(320));
        }
        // the work area off, the fps back: as before (no timeline@1)
        panel->work->setChecked(false);
        QVERIFY(!core::work_area(s.page()).has_value());
        panel->fps->setValue(12);
        emit panel->fps->editingFinished();
        QVERIFY(!core::page_uses_timeline(s.page()));
        // the words
        QCOMPARE(genko::wording::error(QStringLiteral("ops[0] set_animation: fps is 0.5 to 120")), QStringLiteral("フレームレートは 0.5〜120 fps です"));
        QCOMPARE(genko::wording::error(QStringLiteral("ops[0] set_animation: work is [first, last]: frames of 1 to 24, or null")),
                 QStringLiteral("作業範囲は 1〜24 のフレームの［開始点, 終了点］で指定します"));
        // タイムラインを削除: an ordinary page again (the cels stay as layers)
        const std::size_t layers = s.page().layers.size();
        QAction* gone = panel->findChild<QAction*>(QStringLiteral("timeline_delete"));
        QVERIFY(gone != nullptr);
        gone->trigger();
        QVERIFY(!core::anim::is_animation(s.page()));
        QCOMPARE(s.page().layers.size(), layers);
    }

    void timelapseRecordsAndExports() {
        // (page 2 has a line of dialogue: recorded too, in its balloon)
        core::Document lined = book_doc();
        lined.add_line(core::Num(2), "台詞です", "", std::nullopt, "", core::Num(50), core::Num(50));
        Studio s(lined);
        app::MainWindow* w = s.window.get();
        QAction* lapse = w->action("act_timelapse");
        QVERIFY(lapse != nullptr && lapse->isCheckable());
        lapse->trigger();  // (on)
        QVERIFY(render::timelapse::is_on(w->book()));
        QVERIFY(s.saved());
        QVERIFY(gui_test::wait_for([&] { return render::timelapse::frames(s.book).size() == 2; }, 20000));  // (every page)
        const auto first = render::timelapse::frames(s.book);
        QCOMPARE(first[0]["n"], Json(1));
        QCOMPARE(first[0]["page"], Json(1));
        QCOMPARE(first[1]["page"], Json(2));
        QVERIFY(std::filesystem::exists(render::timelapse::folder(s.book) / first[0]["file"].get<std::string>()));
        // a change to page 1: one more picture, of page 1
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                {"points", Json::array({Json::array({20, 20}), Json::array({80, 90})})}}})));
        w->session().save_now();
        QVERIFY(s.saved());
        QVERIFY(gui_test::wait_for([&] { return render::timelapse::frames(s.book).size() == 3; }, 20000));
        QCOMPARE(render::timelapse::frames(s.book).back()["page"], Json(1));
        QCOMPARE(render::timelapse::frames(s.book, Json(1)).size(), std::size_t{2});
        // a change to page 2, written out at once: the save is waited for, so its picture is in
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_note"}, {"page", 2}, {"note", "あとで直す"}}})));
        // written out from its dialog: all pages as a GIF
        const QString out = s.tmp.path() + "/lapse.gif";
        s.answers.responder->save_path = [out](const QString&, const QString&) { return out; };
        bool ran = false;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::TimelapseDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            if (d->count() != 4) return int(QDialog::Rejected);
            d->movie->setCurrentIndex(d->movie->findData(QStringLiteral("gif")));
            d->run();
            ran = d->written.has_value();
            return int(QDialog::Accepted);
        };
        w->action("act_timelapse_export")->trigger();
        QVERIFY(ran);
        QVERIFY(QFile::exists(out));
        QCOMPARE(render::timelapse::frames(s.book).back()["page"], Json(2));
        // off
        lapse->trigger();
        QVERIFY(!render::timelapse::is_on(w->book()));
        QVERIFY(!lapse->isChecked());
    }

    void pluginsChosenInTheirSettings() {
        Studio s;
        app::MainWindow* w = s.window.get();
        QVERIFY(w->action("act_plugins") != nullptr);
        QVERIFY(w->action("act_plugin_settings") != nullptr);
        const std::filesystem::path folder = render::plugins::folder();
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "sepia.py") << "NAME = 'セピア'\ndef run(image, amount=1.0):\n    return image\n";
        QVERIFY(!plugin_filters(w).contains(QStringLiteral("plugin:sepia")));  // (off at first)
        const QString python = genko::test::python_ref();
        int asked = 0;
        s.answers.responder->question = [&](const QString& title, const QString&) {
            ++asked;
            return title == QStringLiteral("プラグインを動かす");
        };
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::PluginsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->enabled->setChecked(true);
            if (!python.isEmpty()) d->python->setText(python);
            for (int i = 0; i < d->list->count(); ++i) {
                if (d->list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("sepia")) d->list->item(i)->setCheckState(Qt::Checked);
            }
            return d->keep() ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        w->action("act_plugin_settings")->trigger();
        QCOMPARE(asked, 1);  // (the plugin newly chosen is asked about)
        QVERIFY(render::plugins::settings().enabled);
        QVERIFY(render::plugins::allowed("sepia"));
        if (!python.isEmpty()) {  // (its name read by the runner: the reference Python has Pillow)
            QVERIFY(plugin_filters(w).contains(QStringLiteral("plugin:sepia")));
            QCOMPARE(plugin_filters(w).at(QStringLiteral("plugin:sepia")), QStringLiteral("セピア"));
        }
        render::plugins::Settings off = render::plugins::settings();
        off.enabled = false;
        render::plugins::save_settings(off);
        std::filesystem::remove(folder / "sepia.py");
    }

    void nativePluginsListedAndChosen() {
        // (a native plugin, docs/PLUGIN_NATIVE.md: listed by its plugin.json's name, one whose plugin.json cannot be used
        // shown as such and not offered, the consent asked for a program; nothing runs — the program here is not one)
        Studio s;
        app::MainWindow* w = s.window.get();
        const std::filesystem::path folder = render::plugins::folder();
        std::filesystem::create_directories(folder / "tint");
        std::filesystem::create_directories(folder / "broken");
        std::ofstream(folder / "tint" / "plugin.json")
            << R"({"protocol": 1, "name": "色づけ", "executable": "tint", "args": ["--soft", "a b"], "params": {"amount": {"label": "強さ", "min": 0, "max": 1, "default": 0.5}}})";
        std::ofstream(folder / "tint" / "tint") << "not a program: never started here";
        std::ofstream(folder / "broken" / "plugin.json") << R"({"protocol": 2, "name": "x", "executable": "x"})";
        QString question;
        s.answers.responder->question = [&](const QString& title, const QString& text) {
            question = text;
            return title == QStringLiteral("プラグインを動かす");
        };
        QString tint_row, broken_row, broken_tip;
        bool broken_checkable = true;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::PluginsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->enabled->setChecked(true);
            for (int i = 0; i < d->list->count(); ++i) {
                QListWidgetItem* row = d->list->item(i);
                const QString key = row->data(Qt::UserRole).toString();
                if (key == QStringLiteral("tint")) {
                    tint_row = row->text();
                    row->setCheckState(Qt::Checked);
                }
                if (key == QStringLiteral("broken")) {
                    broken_row = row->text();
                    broken_tip = row->toolTip();
                    broken_checkable = (row->flags() & Qt::ItemIsUserCheckable) != 0;
                }
            }
            return d->keep() ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        w->action("act_plugin_settings")->trigger();
        QCOMPARE(tint_row, QStringLiteral("tint（プログラム） — 色づけ"));
        QCOMPARE(broken_row, QStringLiteral("broken（プログラム）（使えません）"));
        QCOMPARE(broken_tip, QStringLiteral("プラグイン「broken」の plugin.json が使えません（protocol は 1 にします）"));
        QVERIFY(!broken_checkable);
        QVERIFY(question.startsWith(QStringLiteral("「tint」（プログラム）を動かしてよいですか。")));
        QVERIFY2(question.contains(QStringLiteral("動かすプログラム: tint\n引数: --soft \"a b\"")), qPrintable(question));  // (what will run, as plugin.json says)
        QVERIFY(render::plugins::allowed("tint"));
        QVERIFY(!render::plugins::allowed("broken"));
        // offered among the filters by plugin.json's name (its settings too), nothing started to know them
        const auto filters = plugin_filters(w);
        QVERIFY(filters.contains(QStringLiteral("plugin:tint")));
        QCOMPARE(filters.at(QStringLiteral("plugin:tint")), QStringLiteral("色づけ"));
        QVERIFY(!filters.contains(QStringLiteral("plugin:broken")));
        // the native plugins' words
        QCOMPARE(genko::wording::error(QStringLiteral("ops[0] filter_raster: the program of plugin tint could not be started (execve: Permission denied)")),
                 QStringLiteral("プラグイン「tint」のプログラムを起動できませんでした（このコンピューターで動くプログラムか確かめてください）"));
        QCOMPARE(genko::wording::error(QStringLiteral("the plugin.json of plugin x cannot be used (its program tint.exe is not in its folder)")),
                 QStringLiteral("プラグイン「x」の plugin.json が使えません（プログラム「tint.exe」がプラグインのフォルダーにありません）"));
        QCOMPARE(genko::wording::error(QStringLiteral("the plugin.json of plugin twin cannot be used (a Python plugin twin.py has the same name)")),
                 QStringLiteral("プラグイン「twin」の plugin.json が使えません（同じ名前の Python のプラグイン（twin.py）があります）"));
        QCOMPARE(genko::wording::error(QStringLiteral("plugin fails failed (no paper today)")), QStringLiteral("プラグイン「fails」の処理が失敗しました"));
        QCOMPARE(genko::wording::error(QStringLiteral("the plugin.json of plugin x cannot be used (its folder is a link)")),
                 QStringLiteral("プラグイン「x」の plugin.json が使えません（プラグインのフォルダーがリンクです（設定のフォルダーの plugins の中に置きます））"));
        QCOMPARE(genko::wording::error(QStringLiteral("the plugin.json of plugin x cannot be used (its program run is a link)")),
                 QStringLiteral("プラグイン「x」の plugin.json が使えません（プログラム「run」がリンクです（プラグインのフォルダーの中のファイルにします））"));
        QCOMPARE(genko::wording::error(QStringLiteral("the plugin.json of plugin x cannot be used (its program tint cannot be read)")),
                 QStringLiteral("プラグイン「x」の plugin.json が使えません（プログラム「tint」を読めません（読める権限が要ります））"));
        QCOMPARE(genko::wording::error(QStringLiteral("the program of plugin tint could not be copied to run it (No space left on device)")),
                 QStringLiteral("プラグイン「tint」のプログラムを、動かすために設定のフォルダーへ写せませんでした（空きや権限を確かめてください）"));
        QCOMPARE(genko::wording::error(QStringLiteral("programs cannot run from Genko's settings folder on this computer (/home/a/.config/genko/plugin-runs: "
                                                    "its file system is mounted noexec), so plugin tint cannot run")),
                 QStringLiteral("このコンピューターでは Genko の設定のフォルダー（/home/a/.config/genko/plugin-runs）の中でプログラムを動かせないため、プラグイン「tint」を動かせませんでした"));
        render::plugins::Settings off = render::plugins::settings();
        off.enabled = false;
        off.chosen = Json::object();
        render::plugins::save_settings(off);
        std::filesystem::remove_all(folder / "tint");
        std::filesystem::remove_all(folder / "broken");
    }

    void pluginOfTwoKindsTakenBack() {
        // (twin.py and twin/plugin.json: the Python file is the plugin, the folder's row is shown unusable beside it;
        // the Python one, chosen, is taken back by unchecking its own row)
        Studio s;
        app::MainWindow* w = s.window.get();
        const std::filesystem::path folder = render::plugins::folder();
        std::filesystem::create_directories(folder / "twin");
        std::ofstream(folder / "twin.py") << "def run(image):\n    return image\n";
        std::ofstream(folder / "twin" / "plugin.json") << R"({"protocol": 1, "name": "双子", "executable": "twin"})";
        std::ofstream(folder / "twin" / "twin") << "not a program: never started here";
        render::plugins::Settings on = render::plugins::settings();
        on.enabled = true;
        render::plugins::save_settings(on);
        render::plugins::choose("twin", true);
        QVERIFY(render::plugins::allowed("twin"));
        int rows = 0;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::PluginsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            for (int i = 0; i < d->list->count(); ++i) {
                QListWidgetItem* row = d->list->item(i);
                if (row->data(Qt::UserRole).toString() != QStringLiteral("twin")) continue;
                ++rows;
                if (!row->data(Qt::UserRole + 2).toBool()) row->setCheckState(Qt::Unchecked);
            }
            return d->keep() ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        w->action("act_plugin_settings")->trigger();
        QCOMPARE(rows, 2);
        QVERIFY(!render::plugins::allowed("twin"));
        render::plugins::Settings off = render::plugins::settings();
        off.enabled = false;
        off.chosen = Json::object();
        render::plugins::save_settings(off);
        std::filesystem::remove(folder / "twin.py");
        std::filesystem::remove_all(folder / "twin");
    }

    void timelapseNeedsASavedBook() {
        (void)gui_test::config_folder();
        gui_test::Answers answers;
        app::MainWindow window;  // (a new untitled book in memory)
        window.show();
        window.action("act_timelapse")->trigger();
        QVERIFY(!window.action("act_timelapse")->isChecked());
        QVERIFY(!render::timelapse::is_on(window.book()));
        QVERIFY(window.last_notice().startsWith(QStringLiteral("タイムラプスの前に")));
        window.action("act_timelapse_export")->trigger();
        QCOMPARE(window.last_notice(), QStringLiteral("原稿を保存してから使えます"));
    }
};

QTEST_MAIN(TestGuiAnim)
#include "test_gui_anim.moc"
