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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>
#include <QApplication>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "render/movie.hpp"
#include "render/plugins.hpp"
#include "render/timelapse.hpp"
#include "storage/asset_store.hpp"
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
        // (one step: 取り消し takes the timeline, its folder and its first cel back at once)
        const std::size_t plain_layers = s.page().layers.size();
        w->action("act_undo")->trigger();
        QVERIFY(!core::anim::is_animation(s.page()));
        QCOMPARE(s.page().layers.size() + 2, plain_layers);
        w->action("act_timeline")->trigger();
        QVERIFY(core::anim::is_animation(s.page()));
        app::TimelinePanel* panel = w->timeline();
        QVERIFY(panel->isVisible());
        QCOMPARE(core::anim::frames_of(s.page()), std::int64_t{24});
        // its menu
        QStringList items;
        for (QAction* a : panel->panel_menu->actions()) items << (a->isSeparator() ? QStringLiteral("|") : a->menu() ? a->menu()->title() + QStringLiteral(" ▸") : a->text());
        // (PC5 J-e2: ビデオレイヤー ▸ before ビデオをレンダリング…)
        QCOMPARE(items, (QStringList{QStringLiteral("ビデオタイムラインを作成"), QStringLiteral("タイムラインを削除"), QStringLiteral("|"), QStringLiteral("移動 ▸"),
                                     QStringLiteral("作業範囲 ▸"), QStringLiteral("|"), QStringLiteral("ビデオレイヤー ▸"), QStringLiteral("|"),
                                     QStringLiteral("ビデオをレンダリング…")}));
        // (made again — J-e2's items go in it —: the same menu, nothing left over, its items followed as it opens)
        panel->build_menu();
        panel->build_menu();
        QStringList again;
        for (QAction* a : panel->panel_menu->actions()) again << (a->isSeparator() ? QStringLiteral("|") : a->menu() ? a->menu()->title() + QStringLiteral(" ▸") : a->text());
        QCOMPARE(again, items);
        QCOMPARE(panel->panel_menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly).size(), qsizetype(3));
        emit panel->panel_menu->aboutToShow();
        QVERIFY(panel->findChild<QAction*>(QStringLiteral("timeline_delete"))->isEnabled());
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
        // the work area, from its boxes: the book names timeline@1 — one step, taken back to the anim as it was
        const Json anim_before = *core::anim::spec(s.page());
        panel->work_first->setValue(5);
        panel->work_last->setValue(10);
        panel->work->setChecked(true);
        QVERIFY(core::work_area(s.page()).has_value());
        w->action("act_undo")->trigger();
        QVERIFY(!core::work_area(s.page()).has_value());
        QCOMPARE(core::anim::spec(s.page())->dump(), anim_before.dump());
        QVERIFY(!panel->work->isChecked());
        panel->work_first->setValue(5);
        panel->work_last->setValue(10);
        panel->work->setChecked(true);
        QVERIFY(core::work_area(s.page()) == (std::optional<std::array<std::int64_t, 2>>(std::array<std::int64_t, 2>{5, 10})));
        QVERIFY(core::page_uses_timeline(s.page()));
        QVERIFY(panel->work_first->isEnabled());
        panel->table->setFocus();
        QVERIFY(gui_test::wait_for([&] { return panel->table->hasFocus(); }, 5000));
        QTest::keyClick(panel->table, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(w->current_frame(), std::int64_t{10});
        QTest::keyClick(panel->table, Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(w->current_frame(), std::int64_t{5});
        // played within it: from its first frame when the frame shown is at its end or outside it (Photoshop's), round again
        // after its last
        panel->set_frame(10);
        panel->play_button->setChecked(true);
        QCOMPARE(panel->frame, std::int64_t{5});
        panel->tick();
        QCOMPARE(panel->frame, std::int64_t{6});
        panel->play_button->setChecked(false);
        panel->set_frame(9);
        panel->play_button->setChecked(true);
        QCOMPARE(panel->frame, std::int64_t{9});
        panel->tick();
        panel->tick();
        QCOMPARE(panel->frame, std::int64_t{5});
        panel->play_button->setChecked(false);
        panel->set_frame(20);  // (outside it, the loop off: still played from its first frame)
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_animation"}, {"page", 1}, {"loop", false}}})));
        panel->play_button->setChecked(true);
        QCOMPARE(panel->frame, std::int64_t{5});
        panel->tick();
        QCOMPARE(panel->frame, std::int64_t{6});
        QVERIFY(panel->play_button->isChecked());
        panel->play_button->setChecked(false);
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_animation"}, {"page", 1}, {"loop", true}}})));
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
            QCOMPARE(d.width_px(), std::optional<int>(320));
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

    // (PC5 J-e1, the review's P3-4) A page someone else holds: ビデオタイムラインを作成 changes nothing — the refusal said, the book
    // as it was —, and the page stays an ordinary one.
    // PC5 J-e2 (video@1): レイヤー → ビデオレイヤー ▸ in the window — a video layer from a GIF (its frames the book's pictures,
    // the page made an animation, the timeline's row of its frames), a blank one; the frame shown altered by a stroke begun
    // on it (paint_layer: its cel made and drawn on) and taken back with one Undo; 変更されたビデオを表示 off and on; a frame
    // inserted, duplicated, deleted, restored; フッテージを解釈… (its window), フレームを再読み込み (the file again; gone: said);
    // ラスタライズ → ビデオ; ビデオフレームからレイヤー… (its window); a locked video layer refused; the panel's menu and a cell's.
    void videoLayersInTheWindow() {
        Studio s;
        app::MainWindow* w = s.window.get();
        const QString gif = s.tmp.path() + QStringLiteral("/four.gif");
        {
            std::vector<render::Image> pictures;
            for (const auto& rgb : {std::array<int, 3>{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}}) {
                pictures.push_back(render::Image::create("RGBA", render::Size{32, 24}, render::Ink{rgb[0], rgb[1], rgb[2], 255}).convert("RGB"));
            }
            (void)render::movie::write_movie(pictures, gui_test::path_of(gif), 10, "gif");
        }
        // the commands made real: none says it is not in this build
        for (const char* name : {"act_video_from_file", "act_video_blank", "act_video_insert_blank", "act_video_duplicate", "act_video_delete",
                                 "act_video_replace", "act_video_interpret", "act_video_show_altered", "act_video_restore", "act_video_restore_all",
                                 "act_video_reload", "act_video_rasterize", "act_rasterize_video", "act_video_frames_to_layers"}) {
            QVERIFY2(w->action(name) != nullptr, name);
        }
        QCOMPARE(w->action("act_rasterize_video")->text(), QStringLiteral("ビデオ"));
        QMenu* rasterize = w->findChild<QMenu*>(QStringLiteral("menu_rasterize"));
        const QList<QAction*> in_rasterize = rasterize->actions();
        QCOMPARE(in_rasterize.indexOf(w->action("act_rasterize_video")), in_rasterize.indexOf(w->action("act_rasterize_smart")) + 1);
        // nothing chosen: said
        w->action("act_video_insert_blank")->trigger();
        QVERIFY(w->last_error().contains(QStringLiteral("ビデオレイヤーを選んでください")));
        // from a file
        s.answers.responder->open_path = [gif](const QString&, const QString&) { return gif; };
        w->action("act_video_from_file")->trigger();
        QVERIFY2(core::anim::is_animation(s.page()), qPrintable(w->last_error()));
        const auto folder = w->video_folder();
        QVERIFY(folder.has_value());
        const Json* video = core::video_row(s.page(), Json(*folder));
        QVERIFY(video != nullptr);
        QCOMPARE(video->at("frames").size(), std::size_t(4));
        QCOMPARE(core::anim::frames_of(s.page()), std::int64_t(4));
        for (const Json& ref : video->at("footage").at("frames")) QVERIFY(storage::AssetStore(s.book).has(ref.get<std::string>(), ".png"));
        app::TimelinePanel* panel = w->timeline();
        panel->refresh();
        QCOMPARE(panel->table->item(0, 0)->text(), QStringLiteral("▪"));
        QVERIFY(panel->table->verticalHeaderItem(0)->text().endsWith(QStringLiteral("（ビデオ）")));
        // a stroke drawn on it: the frame altered — its cel the layer drawn on —, one Undo takes the stroke and the cel back
        panel->set_frame(2, false);
        w->set_target_layer(*folder);
        const std::size_t layers = s.page().layers.size();
        app::StrokeInput stroke;
        stroke.tool = QStringLiteral("pen");
        for (int i = 0; i <= 10; ++i) stroke.points.push_back(core::PenPoint{40.0 + i * 3, 60.0, 0.5});
        emit w->canvas()->strokeCommitted(stroke);
        QVERIFY2(gui_test::wait_for([&] { return s.page().layers.size() == layers + 1; }, 10000), qPrintable(w->last_error()));
        const core::Layer* cel = w->target_layer();
        QVERIFY2(cel != nullptr && cel->kind == core::LayerKind::Raster && cel->parent_id == Json(*folder),
                 cel == nullptr ? "none" : (cel->id + " " + cel->parent_id.dump() + " " + std::to_string(static_cast<int>(cel->kind))).c_str());
        QVERIFY(core::video_row(s.page(), Json(*folder))->at("frames")[1][1] == Json(cel->id));
        panel->refresh();
        QCOMPARE(panel->table->item(0, 1)->text(), QStringLiteral("✎"));
        w->action("act_undo")->trigger();
        QCOMPARE(s.page().layers.size(), layers);
        QVERIFY(core::video_row(s.page(), Json(*folder))->at("frames")[1][1].is_null());
        w->action("act_redo")->trigger();
        QCOMPARE(s.page().layers.size(), layers + 1);
        // 変更されたビデオを表示 off and on (the menu's check follows the layer)
        w->set_target_layer(*folder);
        w->action("act_video_show_altered")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("show_altered"), Json(false));
        emit w->findChild<QMenu*>(QStringLiteral("menu_video_layers"))->aboutToShow();
        QVERIFY(!w->action("act_video_show_altered")->isChecked());
        w->action("act_video_show_altered")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("show_altered"), Json(true));
        // insert, duplicate, delete, restore at the frame shown (2)
        w->action("act_video_insert_blank")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames").size(), std::size_t(5));
        QVERIFY(core::video_row(s.page(), Json(*folder))->at("frames")[1][0].is_null());
        w->action("act_video_delete")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames").size(), std::size_t(4));
        panel->set_frame(2, false);
        w->action("act_video_duplicate")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames").size(), std::size_t(5));
        QCOMPARE(core::anim::cels_of(s.page(), Json(*folder)).size(), std::size_t(2));
        w->action("act_video_restore_all")->trigger();
        QVERIFY(core::anim::cels_of(s.page(), Json(*folder)).empty());
        // フッテージを解釈…: its window
        s.answers.responder->exec = [](QDialog* dialog) {
            if (auto* fps = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("interpret_fps"))) fps->setValue(5);
            return 1;
        };
        w->action("act_video_interpret")->trigger();
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("footage").at("fps"), Json(5.0));
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames").size(), std::size_t(8));
        // フレームを再読み込み: the file named shown and asked first (no: nothing read), then read again; gone: said
        const Json frames_before = core::video_row(s.page(), Json(*folder))->at("frames");
        QStringList asked;
        s.answers.responder->question = [&asked](const QString& title, const QString& text) {
            asked << title + QStringLiteral(": ") + text;
            return false;
        };
        w->action("act_video_reload")->trigger();
        QVERIFY(!asked.isEmpty() && asked.back().contains(gif) && asked.back().contains(QStringLiteral("同じ中身")));
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames"), frames_before);
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        w->action("act_video_reload")->trigger();
        QVERIFY2(w->last_error().isEmpty() || !w->last_error().contains(QStringLiteral("見つかりません")), qPrintable(w->last_error()));
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("footage").at("fps"), Json(5.0));
        QCOMPARE(core::video_row(s.page(), Json(*folder))->at("frames"), frames_before);
        QFile::rename(gif, gif + QStringLiteral(".moved"));
        w->action("act_video_reload")->trigger();
        QVERIFY(w->last_error().contains(QStringLiteral("元のファイルが見つかりません")));
        QFile::rename(gif + QStringLiteral(".moved"), gif);
        // locked: refused
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", *folder}, {"locked", true}}})));
        const std::int64_t revision = w->book().revision;
        w->set_target_layer(*folder);
        w->action("act_video_insert_blank")->trigger();
        QCOMPARE(w->book().revision, revision);
        QVERIFY(w->last_error().contains(QStringLiteral("ロック")) || !w->last_error().isEmpty());
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", *folder}, {"locked", false}}})));
        // the panel's menu: ビデオレイヤー ▸ before ビデオをレンダリング…; a cell's menu of a video row
        QMenu* in_panel = panel->findChild<QMenu*>(QStringLiteral("timeline_video_menu"));
        QVERIFY(in_panel != nullptr && in_panel->actions().contains(w->action("act_video_insert_blank")));
        QMenu* cell = panel->cell_menu(0, 0);
        QVERIFY(cell != nullptr);
        QStringList cell_items;
        for (QAction* a : cell->actions()) if (!a->isSeparator()) cell_items << a->text();
        QVERIFY(cell_items.contains(QStringLiteral("空白フレームを挿入")) && cell_items.contains(QStringLiteral("フレームを復元")));
        delete cell;
        // ラスタライズ → ビデオ: a paint layer in its place
        w->set_target_layer(*folder);
        panel->set_frame(3, false);
        w->action("act_rasterize_video")->trigger();
        QVERIFY(core::video_row(s.page(), Json(*folder)) == nullptr);
        QVERIFY(w->target_layer() != nullptr && w->target_layer()->kind == core::LayerKind::Raster);
        // a blank video layer, then ビデオフレームからレイヤー… (its window: every 2nd frame, as layers)
        w->action("act_video_blank")->trigger();
        QVERIFY(w->video_folder().has_value());
        QVERIFY(core::video_row(s.page(), Json(*w->video_folder()))->at("footage").is_null());
        s.answers.responder->exec = [](QDialog* dialog) {
            if (auto* every = dialog->findChild<QSpinBox*>(QStringLiteral("frames_every"))) every->setValue(2);
            return 1;
        };
        const std::size_t before = s.page().layers.size();
        w->action("act_video_frames_to_layers")->trigger();
        QCOMPARE(s.page().layers.size(), before + 2);
    }

    // (the review's P2-3) フレームを再読み込み of a GIF of frames of other lengths (1/10 s, 3/10 s, 1/10 s: shown 1, 3 and 1
    // times at 10 fps): the same frames after it, its interpreted speed kept.
    void videoReloadKeepsLongFrames() {
        Studio s;
        app::MainWindow* w = s.window.get();
        const QString gif = s.tmp.path() + QStringLiteral("/long.gif");
        {
            std::vector<render::Image> pictures;  // (the same picture in a row: one frame shown as long — movie::Writer's)
            for (const auto& rgb : {std::array<int, 3>{255, 0, 0}, {0, 255, 0}, {0, 255, 0}, {0, 255, 0}, {0, 0, 255}}) {
                pictures.push_back(render::Image::create("RGBA", render::Size{16, 12}, render::Ink{rgb[0], rgb[1], rgb[2], 255}).convert("RGB"));
            }
            (void)render::movie::write_movie(pictures, gui_test::path_of(gif), 10, "gif");
        }
        w->new_video_layer(gif);
        const auto folder = w->video_folder();
        QVERIFY2(folder.has_value(), qPrintable(w->last_error()));
        const Json before = core::video_row(s.page(), Json(*folder))->at("footage").at("frames");
        QCOMPARE(before.size(), std::size_t(5));
        QVERIFY(before[1] == before[2] && before[2] == before[3] && before[0] != before[1]);
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_video_layer"}, {"page", 1}, {"folder", *folder}, {"fps", 20}}})));
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        w->action("act_video_reload")->trigger();
        const Json* after = core::video_row(s.page(), Json(*folder));
        QCOMPARE(after->at("footage").at("frames"), before);
        QCOMPARE(after->at("footage").at("fps"), Json(20.0));
        for (const Json& f : after->at("frames")) QVERIFY(f[0].is_number_integer() && f[0].get<std::int64_t>() < 5);
    }

    // (the review's P2-7; 節 J 4.3) The frames drawn for playing kept within their bytes, the oldest let go first.
    void playedFramesKeptWithinBounds() {
        Studio s;
        app::MainWindow* w = s.window.get();
        w->action("act_timeline")->trigger();
        app::TimelinePanel* panel = w->timeline();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_animation"}, {"page", 1}, {"frames", 12}}})));
        panel->played_most = 1;  // (one frame at most kept: the latest)
        panel->play(true);
        std::int64_t most = 0;
        for (int i = 0; i < 12; ++i) {
            panel->tick();
            most = std::max(most, panel->played_bytes());
        }
        panel->play(false);
        QVERIFY(most > 0);
        QVERIFY2(most <= 5 * 1024 * 1024, std::to_string(most).c_str());  // (one A4 frame at 100 dpi, never twelve)
        panel->played_most = 256ll << 20;
    }

    void aHeldPageIsNotMadeAnAnimation() {
        core::Document doc = book_doc();
        doc.page_locks[doc.page(0).id] = "human:other";
        Studio s(doc);
        app::MainWindow* w = s.window.get();
        const std::int64_t revision = w->book().revision;
        const std::size_t layers = s.page().layers.size();
        w->action("act_timeline")->trigger();
        QVERIFY(!core::anim::is_animation(s.page()));
        QCOMPARE(w->book().revision, revision);
        QCOMPARE(s.page().layers.size(), layers);
        QVERIFY(!w->last_error().isEmpty());
        QVERIFY(w->timeline()->start->isVisible());
    }

    // PC6 (review C's P2-4, P3-4): ビデオをレンダリング… on a worker with its progress window over every window; MP4's ffmpeg
    // (here one of the test's own that never ends) waited for there — 中止 stops it, nothing written, said; the window's
    // close while it works is kept until the work is back, then the window closes. (POSIX: the test's ffmpeg a shell script)
    void renderingStopsWhileFfmpegWorks() {
#ifdef Q_OS_UNIX
        Studio s;
        app::MainWindow* w = s.window.get();
        w->show_dock(QStringLiteral("タイムライン"));
        app::TimelinePanel* panel = w->timeline();
        panel->start_animation();
        QVERIFY(w->apply_ops(s.ball(30, w->target_layer()->id)));
        w->session().save_now();
        QVERIFY(s.saved());
        const QString bin = s.tmp.path() + "/bin";
        QVERIFY(QDir().mkpath(bin));
        const QString started = bin + "/started";
        {
            QFile script(bin + "/ffmpeg");
            QVERIFY(script.open(QIODevice::WriteOnly));
            script.write(("#!/bin/sh\nsleep 120 &\necho $! > '" + started + "'\nwait\n").toUtf8());
            script.close();
            QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        const QByteArray path_was = qgetenv("PATH");
        qputenv("PATH", bin.toUtf8() + ":" + path_was);
        s.answers.responder->exec = [](QDialog* dialog) {
            return qobject_cast<app::RenderVideoDialog*>(dialog) != nullptr ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        const QString mp4 = s.tmp.path() + "/w.mp4";
        s.answers.responder->save_path_filtered = [mp4](const QString&, const QString&, const QString&) {
            return std::pair<QString, QString>(mp4, QStringLiteral("MP4 (*.mp4)"));
        };
        // (`then` given the work's progress window once ffmpeg is working)
        const auto when_encoding = [started](std::function<void(QProgressDialog*)> then) {
            auto* timer = new QTimer;
            timer->setInterval(20);
            QObject::connect(timer, &QTimer::timeout, timer, [timer, then, started] {
                if (!QFile::exists(started)) return;
                for (QWidget* top : QApplication::topLevelWidgets()) {
                    auto* progress = qobject_cast<QProgressDialog*>(top);
                    if (progress == nullptr || progress->objectName() != QStringLiteral("stoppable_progress")) continue;
                    timer->stop();
                    timer->deleteLater();
                    then(progress);
                    return;
                }
            });
            timer->start();
        };
        bool pressed = false;
        when_encoding([&](QProgressDialog* progress) {
            pressed = true;
            emit progress->canceled();
        });
        panel->export_button->click();
        QVERIFY(pressed);
        QVERIFY(!QFile::exists(mp4));
        QVERIFY2(w->last_notice().contains(QStringLiteral("やめました")), qPrintable(w->last_notice()));
        QVERIFY(QDir(s.tmp.path()).entryList({QStringLiteral("*.mp4*"), QStringLiteral(".*mp4*")}, QDir::Files | QDir::Hidden).isEmpty());
        // the window's close while ffmpeg works
        QVERIFY(QFile::remove(started));
        bool closed = false;
        when_encoding([&](QProgressDialog*) {
            closed = true;
            w->close();
        });
        panel->export_button->click();
        qputenv("PATH", path_was);
        QVERIFY(closed);
        QVERIFY(w->isVisible());  // (its close left for when the work is back)
        QVERIFY(!QFile::exists(mp4));
        QTRY_VERIFY(!w->isVisible());
#else
        QSKIP("the test's own ffmpeg is a shell script (POSIX)");
#endif
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
        std::ofstream(folder / "broken" / "plugin.json") << R"({"protocol": 3, "name": "x", "executable": "x"})";  // (PC5 J-f: 2 is protocol 2 now)
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
        QCOMPARE(broken_tip, QStringLiteral("プラグイン「broken」の plugin.json が使えません（protocol は 1 か 2 にします）"));
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

    // PC5 J-f (edit.preferences.plugIns, plugin.install, plugin.remove, plugin.reload): 編集 → 環境設定 ▸ プラグイン… — a
    // WebAssembly module listed by its file and size (nothing run), chosen with its own question (a sandbox), then named by
    // its manifest and offered among the filters; インストール… copies one in unchosen, 取り除く takes it out, the additional
    // folder is kept and read; フィルター → プラグイン ▸ プラグインをインストール… does the same as インストール….
    void webAssemblyPluginsInTheirPane() {
        Studio s;
        app::MainWindow* w = s.window.get();
        if (QFileInfo::exists(QStringLiteral(GENKO_TEST_PLUGIN_HOST))) render::plugins::set_host_for_testing(std::filesystem::path(GENKO_TEST_PLUGIN_HOST));
        const bool host = QFileInfo::exists(QStringLiteral(GENKO_TEST_PLUGIN_HOST));
        const std::filesystem::path folder = render::plugins::folder();
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(GENKO_TEST_DATA "/plugins/invert.wasm", folder / "invert.wasm", std::filesystem::copy_options::overwrite_existing);
        QCOMPARE(w->action("act_plugin_settings")->text(), QStringLiteral("プラグイン…"));
        QVERIFY(w->findChild<QMenu*>(QStringLiteral("menu_preferences"))->actions().contains(w->action("act_plugin_settings")));
        QString question, row_text, row_after;
        const QString extra = QDir::tempPath() + QStringLiteral("/genko-gui-extra-%1").arg(QCoreApplication::applicationPid());
        QDir().mkpath(extra);
        s.answers.responder->question = [&](const QString& title, const QString& text) {
            if (title == QStringLiteral("プラグインを動かす")) question = text;
            return true;
        };
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::PluginsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            if (d->windowTitle() != QStringLiteral("プラグイン")) return int(QDialog::Rejected);
            d->enabled->setChecked(true);
            d->use_extra->setChecked(true);
            d->extra->setText(extra);
            for (int i = 0; i < d->list->count(); ++i) {
                QListWidgetItem* row = d->list->item(i);
                if (row->data(Qt::UserRole).toString() == QStringLiteral("invert")) {
                    row_text = row->text();
                    row->setCheckState(Qt::Checked);
                }
            }
            return d->keep() ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        w->action("act_plugin_settings")->trigger();
        QCOMPARE(row_text, QStringLiteral("invert.wasm  1 KB"));  // (its file and size: nothing ran to list it)
        QVERIFY2(question.startsWith(QStringLiteral("「invert.wasm」（WebAssembly）を動かしてよいですか。")), qPrintable(question));
        QVERIFY(question.contains(QStringLiteral("ファイルもネットも使えません")));
        QVERIFY(render::plugins::allowed("invert"));
        QVERIFY(render::plugins::settings().use_extra_folder);
        QCOMPARE(QString::fromStdString(render::plugins::settings().extra_folder), extra);
        if (host) {  // (named by its manifest once chosen, among the filters)
            QCOMPARE(plugin_filters(w).at(QStringLiteral("plugin:invert")), QStringLiteral("Invert (WebAssembly)"));
            s.answers.responder->exec = [&](QDialog* dialog) {
                auto* d = qobject_cast<app::PluginsDialog*>(dialog);
                if (d == nullptr) return int(QDialog::Rejected);
                for (int i = 0; i < d->list->count(); ++i) {
                    if (d->list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("invert")) row_after = d->list->item(i)->text();
                }
                return int(QDialog::Rejected);
            };
            w->action("act_plugin_settings")->trigger();
            QCOMPARE(row_after, QStringLiteral("invert.wasm — Invert (WebAssembly) 1.0.0（PhotoCraft contributors）  1 KB"));
        }
        // インストール… / 取り除く / 読み直す; the menu's プラグインをインストール…
        QFile::copy(QStringLiteral(GENKO_TEST_DATA "/plugins/invert.wasm"), extra + QStringLiteral("/../genko-gui-copy.wasm"));
        const QString copy = QDir(extra + QStringLiteral("/..")).absoluteFilePath(QStringLiteral("genko-gui-copy.wasm"));
        bool installed = false, removed = false;
        int rows_extra = -1;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::PluginsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            installed = d->install_file(copy);
            int at = -1;
            for (int i = 0; i < d->list->count(); ++i) {
                if (d->list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("genko-gui-copy")) at = i;
            }
            QFile::copy(QStringLiteral(GENKO_TEST_DATA "/plugins/invert.wasm"), extra + QStringLiteral("/fromextra.wasm"));
            d->reload();
            rows_extra = 0;
            for (int i = 0; i < d->list->count(); ++i) rows_extra += d->list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("fromextra") ? 1 : 0;
            for (int i = 0; i < d->list->count(); ++i) {
                if (d->list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("genko-gui-copy")) at = i;
            }
            removed = at >= 0 && d->remove_row(at);
            return int(QDialog::Rejected);
        };
        w->action("act_plugin_settings")->trigger();
        QVERIFY(installed);
        QVERIFY(removed);
        QCOMPARE(rows_extra, 1);
        QVERIFY(!std::filesystem::exists(folder / "genko-gui-copy.wasm"));
        s.answers.responder->open_path = [&](const QString&, const QString&) { return copy; };
        w->action("act_plugin_install")->trigger();
        QVERIFY(std::filesystem::exists(folder / "genko-gui-copy.wasm"));
        QVERIFY(!render::plugins::allowed("genko-gui-copy"));  // (installed: not chosen)
        QVERIFY(w->last_notice().startsWith(QStringLiteral("プラグイン「genko-gui-copy」を入れました")));
        // the WebAssembly plugins' words
        QCOMPARE(genko::wording::error(QStringLiteral("ops[0] filter_raster: plugin loop failed (plug-in exceeded its instruction budget)")),
                 QStringLiteral("プラグイン「loop」が命令の数の上限を超えたため止めました（終わらない繰り返しの可能性があります）"));
        QCOMPARE(genko::wording::error(QStringLiteral("the module of plugin imports cannot be used (plug-in ABI: the module imports `wasi_snapshot_preview1.fd_write`, "
                                                    "but plug-ins get no host imports (no WASI, files or network))")),
                 QStringLiteral("プラグイン「imports」の WebAssembly のモジュールが使えません（外の機能（wasi_snapshot_preview1.fd_write）を読み込もうとしています。"
                                "プラグインはファイル・ネット・WASI を使えません）"));
        QCOMPARE(genko::wording::error(QStringLiteral("the WebAssembly plugins need genko-plugin-host beside Genko (it was not found), so plugin x cannot run")),
                 QStringLiteral("WebAssembly のプラグインを動かす genko-plugin-host が Genko と同じフォルダーに無いため、プラグイン「x」を動かせません"));
        std::filesystem::remove(folder / "genko-gui-copy.wasm");
        std::filesystem::remove(folder / "invert.wasm");
        QFile::remove(copy);
        QDir(extra).removeRecursively();
        render::plugins::Settings off = render::plugins::settings();
        off.enabled = false;
        off.use_extra_folder = false;
        off.extra_folder.clear();
        off.chosen = Json::object();
        render::plugins::save_settings(off);
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
