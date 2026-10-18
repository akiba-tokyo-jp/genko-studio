// vector@1 (PC3 C-a, SPEC.md COLOR-07, design section C 5) in the window: ペンツール (P) — corners clicked, a smooth point
// dragged, the subpath closed on its first anchor, one Undo for it —, 曲線ペンツール (Shift+P) ended by Enter, パス選択ツール (A)
// moving a subpath (Alt: a copy) and taking it out (Delete); the パス panel's rows and buttons (新規パス, 名前, 塗りつぶす,
// 境界線を描く, 選択範囲に, 作業用パスを作成, マスクを追加, 削除); 選択範囲 → 作業用パスを作成…; ページ → 台詞 → 作業用パスを
// 作成; レイヤー → ベクトルマスク (すべての領域を表示・すべての領域を隠す・現在のパス・使用しない・リンク・削除) and ラスタライズ →
// ベクトルマスク; 表示 → ターゲットパス; ファイル → パスを Illustrator へ…; the tools' keys.

#include <QtTest>

#include <QDockWidget>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTemporaryDir>

#include <cmath>
#include <memory>
#include <string>

#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/paths_panel.hpp"
#include "core/vector.hpp"
#include "gui_support.hpp"
#include "storage/fsutil.hpp"

using namespace genko;
using app::PageCanvas;
using core::Json;
namespace vec = core::vector;

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
        answers.responder->question = [](const QString&, const QString&) { return true; };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    PageCanvas* canvas() const { return window->canvas(); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page() const { return *doc().pages[0]; }
    const core::Layer& target() const { return *window->target_layer(); }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }

    void mouse(QEvent::Type type, double x_mm, double y_mm, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPointF at = canvas()->to_widget(QPointF(x_mm, y_mm));
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, at, canvas()->mapToGlobal(at), button, buttons, modifiers);
        QCoreApplication::sendEvent(canvas(), &event);
    }
    void click(double x, double y, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, x, y, modifiers);
        mouse(QEvent::MouseButtonRelease, x, y, modifiers);
    }
    void drag(double x0, double y0, double x1, double y1, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, x0, y0, modifiers);
        for (int i = 1; i <= 4; ++i) mouse(QEvent::MouseMove, x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4, modifiers);
        mouse(QEvent::MouseButtonRelease, x1, y1, modifiers);
    }
    void key(Qt::Key k) {
        canvas()->setFocus();
        QTest::keyClick(canvas(), k);
    }
};

Json square(double x, double y, double w) {
    return Json{{"subpaths", Json::array({Json{{"knots", Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + w}),
                                                                      Json::array({x, y + w})})}}})}};
}

bool in_menu(QMenu* menu, QAction* wanted) {
    for (QAction* a : menu->actions()) {
        if (a == wanted) return true;
        if (a->menu() != nullptr && in_menu(a->menu(), wanted)) return true;
    }
    return false;
}

QMenu* top(app::MainWindow* w, const QString& title) {
    for (QAction* a : w->menuBar()->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

}  // namespace

class TestGuiPaths : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // The tools: P, Shift+P, A, in the tools' group and the ツール menu; the commands in their menus.
    void theToolsAndMenus() {
        Book b;
        QCOMPARE(b.act("act_path_pen")->shortcut(), QKeySequence(QStringLiteral("P")));
        QCOMPARE(b.act("act_curvature_pen")->shortcut(), QKeySequence(QStringLiteral("Shift+P")));
        QCOMPARE(b.act("act_path_select")->shortcut(), QKeySequence(QStringLiteral("A")));
        for (const char* name : {"act_path_pen", "act_curvature_pen", "act_path_select"}) {
            QCOMPARE(b.act(name)->actionGroup(), b.window->tools_group());
            QVERIFY2(in_menu(top(b.window.get(), QStringLiteral("ツール")), b.act(name)), name);
        }
        b.act("act_path_pen")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("pathpen"));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("選択範囲")), b.act("act_work_path")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("ページ")), b.act("act_line_work_path")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("表示")), b.act("act_show_target_path")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("ファイル")), b.act("act_export_paths_ai")));
        QMenu* layers = top(b.window.get(), QStringLiteral("レイヤー"));
        QMenu* masks = layers->findChild<QMenu*>(QStringLiteral("menu_vector_mask"));
        QVERIFY(masks != nullptr && masks->title() == QStringLiteral("ベクトルマスク"));
        for (const char* name : {"act_vmask_reveal_all", "act_vmask_hide_all", "act_vmask_current_path", "act_vmask_delete", "act_vmask_disable", "act_vmask_link"})
            QVERIFY2(in_menu(masks, b.act(name)), name);
        QVERIFY(in_menu(layers->findChild<QMenu*>(QStringLiteral("menu_rasterize")), b.act("act_rasterize_vmask")));
        QVERIFY(b.act("act_show_target_path")->isCheckable() && b.act("act_show_target_path")->isChecked());
        b.act("act_show_target_path")->trigger();
        QVERIFY(!b.canvas()->show_target_path);
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("ウィンドウ")), b.window->paths_dock()->toggleViewAction()));
    }

    // ペンツール: corners clicked, a smooth point dragged, closed on the first anchor — one subpath, one op (one Undo); the
    // next subpath added to the work path.
    void thePenDrawsAPath() {
        Book b;
        b.act("act_path_pen")->trigger();
        b.click(20, 20);
        b.click(80, 20);
        QVERIFY(b.canvas()->pen_drawing());
        QVERIFY(!b.page().work_path.has_value());  // (the anchors are the screen's until the subpath ends)
        b.drag(80, 80, 95, 95);  // (a smooth point: its handles pulled)
        b.click(20, 80);
        b.click(20, 20);  // (the first anchor: closed)
        QVERIFY(!b.canvas()->pen_drawing());
        QVERIFY(b.page().work_path.has_value());
        const vec::Subpath& s = b.page().work_path->subpaths.at(0);
        QVERIFY(s.closed);
        QCOMPARE(s.knots.size(), std::size_t(4));
        QVERIFY(s.knots[2].smooth);
        QVERIFY(std::fabs(s.knots[2].out.x - 95) < 0.5 && std::fabs(s.knots[2].in.x - 65) < 0.5);
        QVERIFY(!s.knots[0].smooth);
        // another subpath: added to the work path, ended open by Enter; Backspace takes an anchor back first
        b.click(100, 100);
        b.click(120, 100);
        b.click(140, 120);
        b.key(Qt::Key_Backspace);
        b.key(Qt::Key_Return);
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t(2));
        QVERIFY(!b.page().work_path->subpaths[1].closed);
        QCOMPARE(b.page().work_path->subpaths[1].knots.size(), std::size_t(2));
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t(1));
        b.act("act_undo")->trigger();
        QVERIFY(!b.page().work_path.has_value());
    }

    // 曲線ペンツール: the curve through the points clicked, smooth; Alt+click a corner; Enter ends it open.
    void theCurvaturePen() {
        Book b;
        b.act("act_curvature_pen")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("curvepen"));
        b.click(20, 50);
        b.click(50, 20);
        b.click(80, 50, Qt::AltModifier);
        b.click(110, 20);
        b.key(Qt::Key_Return);
        const vec::Subpath& s = b.page().work_path->subpaths.at(0);
        QVERIFY(!s.closed);
        QCOMPARE(s.knots.size(), std::size_t(4));
        QVERIFY(s.knots[1].smooth && !(s.knots[1].in == s.knots[1].anchor));
        QVERIFY(!s.knots[2].smooth && s.knots[2].is_corner());
    }

    // パス選択ツール: a subpath moved by a drag, copied by Alt+drag, an anchor moved alone, taken out by Delete.
    void thePathSelectionTool() {
        Book b;
        QVERIFY(b.apply(Json{{"op", "set_path"}, {"page", 1}, {"path", square(20, 20, 30)}}));
        b.act("act_path_select")->trigger();
        b.drag(35, 35, 45, 40);  // (inside: the subpath chosen and moved 10, 5 mm)
        QCOMPARE(b.canvas()->path_subpath, std::optional<std::size_t>(0));
        const vec::Knot moved = b.page().work_path->subpaths.at(0).knots.at(0);
        QVERIFY2(std::fabs(moved.anchor.x - 30) < 0.2 && std::fabs(moved.anchor.y - 25) < 0.2, std::to_string(moved.anchor.x).c_str());
        b.drag(45, 40, 105, 40, Qt::AltModifier);  // (a copy)
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t(2));
        QVERIFY(std::fabs(b.page().work_path->subpaths[1].knots[0].anchor.x - 90) < 0.2);
        QCOMPARE(b.canvas()->path_subpath, std::optional<std::size_t>(1));
        // (an anchor of the chosen subpath dragged alone: as the direct selection tool)
        b.drag(90, 25, 85, 15);
        const vec::Subpath& copy = b.page().work_path->subpaths[1];
        QVERIFY(std::fabs(copy.knots[0].anchor.x - 85) < 0.2 && std::fabs(copy.knots[0].anchor.y - 15) < 0.2);
        QVERIFY(std::fabs(copy.knots[1].anchor.x - 120) < 0.2);
        b.act("act_delete_area")->trigger();  // (Delete)
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t(1));
        b.click(35, 32);
        b.act("act_delete_area")->trigger();
        QVERIFY(!b.page().work_path.has_value());  // (the last subpath: the work path gone)
    }

    // The パス panel: its rows (the work path, the saved paths, the vector mask of the layer drawn on), a new path drawn
    // on, renamed, filled, stroked, made the selection, a work path from the selection, a vector mask added, deleted.
    void thePathsPanel() {
        Book b;
        b.window->paths_dock()->show();
        app::PathsPanel* panel = b.window->paths_panel();
        QVERIFY(b.apply(Json{{"op", "set_path"}, {"page", 1}, {"path", square(20, 20, 30)}}));
        panel->refresh();
        QCOMPARE(panel->keys(), std::vector<QString>{QStringLiteral("work")});
        panel->add_new();
        QCOMPARE(b.page().paths.size(), std::size_t(1));
        QCOMPARE(b.page().paths[0].name, std::string("パス 1"));
        QCOMPARE(b.canvas()->target_path, QStringLiteral("saved:パス 1"));
        b.act("act_path_pen")->trigger();
        b.click(60, 60);
        b.click(100, 60);
        b.click(100, 100);
        b.click(60, 60);
        QCOMPARE(b.page().paths[0].path.subpaths.size(), std::size_t(1));
        panel->rename(QStringLiteral("輪郭"));
        QCOMPARE(b.page().paths[0].name, std::string("輪郭"));
        QCOMPARE(panel->list()->count(), 2);
        // 塗りつぶす, 境界線を描く on the layer drawn on
        const std::size_t patches = b.target().patches.size(), strokes = b.target().stroke_count();
        panel->button(QStringLiteral("path_fill"))->click();
        QCOMPARE(b.target().patches.size(), patches + 1);
        panel->button(QStringLiteral("path_stroke"))->click();
        QCOMPARE(b.target().stroke_count(), strokes + 1);
        // 選択範囲に, then 作業用パスを作成 from it (the work path replaced)
        panel->button(QStringLiteral("path_selection"))->click();
        QVERIFY(b.canvas()->selection().has_value());
        panel->button(QStringLiteral("path_from_selection"))->click();
        QCOMPARE(b.canvas()->target_path, QStringLiteral("work"));
        const auto box = b.page().work_path->control_bounds();
        QVERIFY(box && std::fabs((*box)[0] - 60) < 0.5 && std::fabs((*box)[3] - 100) < 0.5);
        // マスクを追加: the layer drawn on masked by the target path; its row in the panel
        panel->button(QStringLiteral("path_mask"))->click();
        QVERIFY(b.target().vector_mask.has_value());
        QVERIFY(b.canvas()->target_path.startsWith(QStringLiteral("mask:")));
        panel->refresh();
        QCOMPARE(panel->list()->count(), 3);
        // 削除: the mask row's mask, then a saved path
        panel->button(QStringLiteral("path_delete"))->click();
        QVERIFY(!b.target().vector_mask.has_value());
        b.canvas()->target_path = QStringLiteral("saved:輪郭");
        panel->button(QStringLiteral("path_delete"))->click();
        QVERIFY(b.page().paths.empty());
    }

    // レイヤー → ベクトルマスク and ラスタライズ → ベクトルマスク on the layer drawn on.
    void theVectorMaskMenu() {
        Book b;
        b.act("act_vmask_hide_all")->trigger();
        QVERIFY(b.target().vector_mask && b.target().vector_mask->path.empty() && b.target().vector_mask->path.inverted);
        b.act("act_vmask_reveal_all")->trigger();
        QVERIFY(b.target().vector_mask && !b.target().vector_mask->path.inverted);
        QVERIFY(b.apply(Json{{"op", "set_path"}, {"page", 1}, {"path", square(20, 20, 30)}}));
        b.canvas()->target_path = QStringLiteral("work");
        b.act("act_vmask_current_path")->trigger();
        QCOMPARE(b.target().vector_mask->path.subpaths.size(), std::size_t(1));
        b.act("act_vmask_disable")->trigger();
        QVERIFY(!b.target().vector_mask->enabled);
        b.act("act_vmask_disable")->trigger();
        QVERIFY(b.target().vector_mask->enabled);
        b.act("act_vmask_link")->trigger();
        QVERIFY(!b.target().vector_mask->linked);
        // (drawn with the pen onto the mask's own path)
        QCOMPARE(b.canvas()->target_path, QStringLiteral("mask:") + QString::fromStdString(b.target().id));
        b.act("act_path_pen")->trigger();
        b.click(60, 60);
        b.click(90, 60);
        b.click(90, 90);
        b.click(60, 60);
        QCOMPARE(b.target().vector_mask->path.subpaths.size(), std::size_t(2));
        b.act("act_rasterize_vmask")->trigger();
        QVERIFY(!b.target().vector_mask.has_value() && b.target().mask.has_value());
        b.act("act_vmask_reveal_all")->trigger();
        b.act("act_vmask_delete")->trigger();
        QVERIFY(!b.target().vector_mask.has_value());
    }

    // 選択範囲 → 作業用パスを作成… (asked its tolerance); ページ → 台詞 → 作業用パスを作成 (the chosen line); パスを Illustrator へ….
    void workPathsAndExport() {
        Book b;
        b.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(0.5); };
        b.canvas()->set_selection(Json{{"poly", Json::array({Json::array({20, 20}), Json::array({70, 20}), Json::array({70, 60}), Json::array({20, 60})})}});
        b.act("act_work_path")->trigger();
        QVERIFY(b.page().work_path.has_value());
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t(1));
        QVERIFY(b.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "t1"}, {"text", "台詞"}, {"x_mm", 100}, {"y_mm", 100}}));
        b.canvas()->selected_line_id = "t1";
        b.act("act_line_work_path")->trigger();
        QVERIFY(b.page().work_path->subpaths.size() >= 2);
        const auto box = b.page().work_path->control_bounds();
        QVERIFY(box && (*box)[0] > 80);  // (the letters', not the selection's)
        const QString file = b.tmp.path() + QStringLiteral("/paths.ai");
        QVERIFY(b.window->export_paths_ai(file));
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(written.readAll().startsWith("%!PS-Adobe-2.0 EPSF-1.2"));
    }
};

QTEST_MAIN(TestGuiPaths)
#include "test_gui_paths.moc"
