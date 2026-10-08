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
#include "gui_support.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/plugins_dialog.hpp"
#include "app/tiles.hpp"
#include "app/timeline.hpp"
#include "core/anim.hpp"
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
        QVERIFY(gui_test::wait_for([&] { return w->canvas()->renderer().settled(); }, 30000));
        const QImage with_onion = w->canvas()->renderer().compose(w->canvas()->base_dpi());
        panel->onion->setChecked(false);
        QVERIFY(gui_test::wait_for([&] { return w->canvas()->renderer().settled(); }, 30000));
        const QImage without = w->canvas()->renderer().compose(w->canvas()->base_dpi());
        QVERIFY(!with_onion.isNull() && with_onion.size() == without.size());
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
        // written out (the format from the file's name)
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
        auto* filters = w->layer_panel()->findChild<QComboBox*>(QStringLiteral("layer_filter"));
        QVERIFY(filters != nullptr);
        QCOMPARE(filters->findData(QStringLiteral("plugin:sepia")), -1);  // (off at first)
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
            QVERIFY(filters->findData(QStringLiteral("plugin:sepia")) >= 0);
            QCOMPARE(filters->itemText(filters->findData(QStringLiteral("plugin:sepia"))), QStringLiteral("セピア（プラグイン）"));
        }
        render::plugins::Settings off = render::plugins::settings();
        off.enabled = false;
        render::plugins::save_settings(off);
        std::filesystem::remove(folder / "sepia.py");
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
