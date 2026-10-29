// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」) in the window: the 注釈ツール (in the ツール menu and the スポイト group, its
// options — 作成者・カラー・すべてを消去・注釈パネルの表示切り替え), a click putting a note (its author and colour the options',
// the 注釈 panel shown to type its text), its mark dragged and double clicked (its box opened and closed); ウィンドウ → 注釈
// (the dock, after 計測ログ; the panel's list, text, 前・次・削除); 表示 → 表示・非表示 → 注釈 hiding the marks; ファイル →
// 読み込み → 注釈… reading a PSD's notes onto the page — each change one Undo.

#include <QtTest>

#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>
#include <string>

#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/notes_panel.hpp"
#include "core/command_bus.hpp"
#include "formats/export.hpp"
#include "gui_support.hpp"
#include "render/ops_registry.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::b4_comic()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        window->canvas()->fit_page();
        QCoreApplication::processEvents();
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Page& page() const { return *window->book().pages[0]; }
    app::PageCanvas* canvas() const { return window->canvas(); }
    app::NotesPanel* panel() const { return window->notes_panel(); }
    QMenu* top(const QString& title) const {
        for (QAction* a : window->menuBar()->actions())
            if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
        return nullptr;
    }
    // (a point on a note's mark, mm)
    QPointF on_mark(const core::Annotation& note) const {
        return canvas()->to_mm(canvas()->to_widget(QPointF(note.at_mm[0], note.at_mm[1])) + QPointF(6, 6));
    }
};

QMenu* submenu(QMenu* menu, const QString& name) {
    for (QAction* a : menu->actions())
        if (a->menu() != nullptr && a->menu()->objectName() == name) return a->menu();
    return nullptr;
}

}  // namespace

class TestGuiNotes : public QObject {
    Q_OBJECT
private slots:
    // The commands in their places: the 注釈ツール in ツール and the スポイト group; 表示 → 表示・非表示 → 注釈 (on); ファイル →
    // 読み込み → 注釈…; ウィンドウ → 注釈 after 計測ログ, showing and hiding its dock.
    void theCommandsInTheirPlaces() {
        Book b;
        QAction* tool = b.act("act_note");
        QVERIFY(tool != nullptr && tool->isCheckable());
        QCOMPARE(tool->text(), QStringLiteral("注釈ツール"));
        QVERIFY(b.top(QStringLiteral("ツール"))->actions().contains(tool));
        const std::vector<QAction*> group = b.window->tool_group_of(b.act("act_picker"));
        QVERIFY(std::find(group.begin(), group.end(), tool) != group.end());
        QMenu* shows = submenu(b.top(QStringLiteral("表示")), QStringLiteral("menu_show"));
        QVERIFY(shows != nullptr);
        QCOMPARE(shows->title(), QStringLiteral("表示・非表示"));
        QVERIFY(shows->actions().contains(b.act("act_show_notes")));
        QVERIFY(b.act("act_show_notes")->isChecked());
        QMenu* imports = submenu(b.top(QStringLiteral("ファイル")), QStringLiteral("menu_import"));
        QVERIFY(imports != nullptr);
        QCOMPARE(imports->title(), QStringLiteral("読み込み"));
        // (PC5 I-d: 変数データセット… first, as Photoshop's: MainWindow::add_to_import_menu)
        QCOMPARE(imports->actions(), (QList<QAction*>{b.act("act_import_data_sets"), b.act("act_import_notes")}));
        QCOMPARE(b.act("act_import_notes")->text(), QStringLiteral("注釈…"));
        QAction* toggle = b.act("act_panel_notes");
        const QList<QAction*> window_items = b.top(QStringLiteral("ウィンドウ"))->actions();
        const qsizetype at = window_items.indexOf(toggle);
        QVERIFY(at > 0);
        QCOMPARE(window_items[at - 1], b.act("act_panel_measurement_log"));
        QVERIFY(!b.window->notes_dock()->isVisible());
        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY(b.window->notes_dock()->isVisible() && toggle->isChecked());
        QCOMPARE(b.panel()->head()->text(), QStringLiteral("このページには注釈がありません"));
        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!b.window->notes_dock()->isVisible());
    }

    // The 注釈ツール: a click puts a note (the options' author and colour) and shows it in the panel to type; its text put in
    // the book when the text is left (one Undo); its mark dragged; a double click closes and opens its box.
    void theToolPutsANote() {
        Book b;
        b.act("act_note")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("note"));
        auto* author = b.window->findChild<QLineEdit*>(QStringLiteral("note_author"));
        QVERIFY(author != nullptr);
        author->setText(QStringLiteral("編集"));
        inject::click_mm(b.canvas(), QPointF(40, 60));
        QCoreApplication::processEvents();
        QCOMPARE(b.page().annotations.size(), std::size_t(1));
        const core::Annotation& made = b.page().annotations[0];
        QCOMPARE(made.author, std::string("編集"));
        QVERIFY(std::fabs(made.at_mm[0] - 40) < 0.5 && std::fabs(made.at_mm[1] - 60) < 0.5);
        QVERIFY(made.open);
        QCOMPARE(made.color, (std::array<int, 3>{255, 255, 181}));
        QCOMPARE(made.actor.substr(0, 6), std::string("human:"));
        QVERIFY(b.window->notes_dock()->isVisible());
        QCOMPARE(b.panel()->chosen(), QString::fromStdString(made.id));
        QCOMPARE(b.canvas()->chosen_note, QString::fromStdString(made.id));
        // its text typed in the panel, put in when it is left
        b.panel()->text()->setPlainText(QStringLiteral("ここを直す"));
        b.panel()->commit_text();
        QCOMPARE(b.page().annotations[0].text, std::string("ここを直す"));
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().annotations[0].text, std::string());
        b.act("act_redo")->trigger();
        QCOMPARE(b.page().annotations[0].text, std::string("ここを直す"));
        // its mark dragged 10 mm to the right
        const QPointF from = b.on_mark(b.page().annotations[0]);
        inject::mouse(b.canvas(), inject::Phase::Press, from);
        inject::mouse(b.canvas(), inject::Phase::Move, from + QPointF(5, 0));
        inject::mouse(b.canvas(), inject::Phase::Move, from + QPointF(10, 0));
        inject::mouse(b.canvas(), inject::Phase::Release, from + QPointF(10, 0));
        QCoreApplication::processEvents();
        QCOMPARE(b.page().annotations.size(), std::size_t(1));
        QVERIFY2(std::fabs(b.page().annotations[0].at_mm[0] - 50) < 0.5, std::to_string(b.page().annotations[0].at_mm[0]).c_str());
        // a double click on its mark: its box closed, then open
        const QPoint mark = b.canvas()->to_widget(QPointF(b.page().annotations[0].at_mm[0], b.page().annotations[0].at_mm[1])).toPoint() + QPoint(6, 6);
        QTest::mouseDClick(b.canvas(), Qt::LeftButton, Qt::NoModifier, mark);
        QCoreApplication::processEvents();
        QVERIFY(!b.page().annotations[0].open);
        QTest::mouseDClick(b.canvas(), Qt::LeftButton, Qt::NoModifier, mark);
        QCoreApplication::processEvents();
        QVERIFY(b.page().annotations[0].open);
        QCOMPARE(b.page().annotations.size(), std::size_t(1));
        // (off the paper: no note)
        inject::click_mm(b.canvas(), QPointF(-20, -20));
        QCOMPARE(b.page().annotations.size(), std::size_t(1));
    }

    // The panel: its list, 前・次 round, 削除; すべてを消去 (one Undo); 表示 → 表示・非表示 → 注釈 off: the marks hidden and not
    // clicked.
    void thePanelAndTheOptions() {
        Book b;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(b.window->apply_ops(Json::array({Json{{"op", "add_annotation"}, {"page", 1}, {"at_mm", Json::array({20 + 30 * i, 40})},
                                                          {"text", "注釈 " + std::to_string(i + 1)}}})));
        }
        b.act("act_panel_notes")->trigger();
        QCoreApplication::processEvents();
        app::NotesPanel* p = b.panel();
        QCOMPARE(p->list()->count(), 3);
        QVERIFY(p->list()->item(0)->text().startsWith(QStringLiteral("注釈 1")));
        p->button(QStringLiteral("note_next"))->click();
        QCOMPARE(p->chosen(), QString::fromStdString(b.page().annotations[0].id));
        QCOMPARE(p->text()->toPlainText(), QStringLiteral("注釈 1"));
        p->button(QStringLiteral("note_previous"))->click();
        QCOMPARE(p->chosen(), QString::fromStdString(b.page().annotations[2].id));
        p->button(QStringLiteral("note_delete"))->click();
        QCOMPARE(b.page().annotations.size(), std::size_t(2));
        QCOMPARE(p->list()->count(), 2);
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().annotations.size(), std::size_t(3));
        // 表示・非表示 → 注釈 off: not drawn, not clicked (a click puts a new note there instead)
        b.act("act_note")->trigger();
        b.act("act_show_notes")->trigger();
        QVERIFY(!b.canvas()->extra_shown(QStringLiteral("notes")));
        QVERIFY(!b.canvas()->note_at(b.canvas()->to_widget(QPointF(20, 40)) + QPointF(6, 6)));
        b.act("act_show_notes")->trigger();
        QVERIFY(b.canvas()->extra_shown(QStringLiteral("notes")));
        QVERIFY(b.canvas()->note_at(b.canvas()->to_widget(QPointF(20, 40)) + QPointF(6, 6)));
        // すべてを消去: one Undo
        auto* clear = b.window->findChild<QPushButton*>(QStringLiteral("note_clear"));
        QVERIFY(clear != nullptr);
        clear->click();
        QVERIFY(b.page().annotations.empty());
        QCOMPARE(p->list()->count(), 0);
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().annotations.size(), std::size_t(3));
    }

    // ファイル → 読み込み → 注釈…: a PSD's notes onto the page (one Undo); a file without notes said.
    void importingNotes() {
        Book b;
        QTemporaryDir dir;
        auto doc = core::new_episode("n", core::Num(1), 1, core::PageSpec::custom(100, 80, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "add_annotation"}, {"page", 1}, {"at_mm", Json::array({10, 10})}, {"text", "PSD の注釈"}, {"author", "PS"}}}),
                         core::Actor("human:x"))
                  .doc;
        const QString psd = dir.path() + QStringLiteral("/n.psd");
        (void)formats::export_page_psd(doc, doc.page(0), gui_test::path_of(psd), 200);
        b.answers.responder->open_path = [psd](const QString&, const QString&) { return psd; };
        b.act("act_import_notes")->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(b.page().annotations.size(), std::size_t(1));
        QCOMPARE(b.page().annotations[0].text, std::string("PSD の注釈"));
        QCOMPARE(b.page().annotations[0].author, std::string("PS"));
        b.act("act_undo")->trigger();
        QVERIFY(b.page().annotations.empty());
        // (a PSD without notes: nothing put, said)
        const QString plain = dir.path() + QStringLiteral("/plain.psd");
        auto empty = core::new_episode("p", core::Num(1), 1, core::PageSpec::custom(100, 80, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        (void)formats::export_page_psd(empty, empty.page(0), gui_test::path_of(plain), 200);
        b.answers.responder->open_path = [plain](const QString&, const QString&) { return plain; };
        b.act("act_import_notes")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(b.page().annotations.empty());
    }
};

QTEST_MAIN(TestGuiNotes)
#include "test_gui_notes.moc"
