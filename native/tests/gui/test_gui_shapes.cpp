// pen-shapes@1 (PC3 C-b, SPEC.md COLOR-07「シェイプ」, design section C 5.2) in the window: the shape tools (長方形ツール O —
// the 図形 of before —, 楕円形・三角形・多角形・ライン・カスタムシェイプツール, Shift+O going round them) in their モード
// (シェイプ: a shape part on the pen layer drawn on or a new one, Alt from the centre, a star; パス: the work path; ピクセル: the
// 図形 of before), the pen tools' モード, パス選択ツール on a part (chosen on the page, moved keeping its live shape, Delete), the
// パス panel's rows of a pen layer's parts, the シェイプ panel (a preset placed), 編集 → カスタムシェイプを定義…, レイヤー →
// シェイプを結合 and ラスタライズ → シェイプ, 選択範囲 → シェイプに変換, ページ → 台詞 → シェイプに変換, 表示 → シェイプから新規
// ガイドを作成, 部分選択ツール (the 線の編集 of before).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cmath>
#include <memory>
#include <string>

#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/paths_panel.hpp"
#include "app/shapes_panel.hpp"
#include "core/vector.hpp"
#include "gui_support.hpp"
#include "render/shape_presets.hpp"

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
        choose("shape_mode", "shape");
        choose("pen_mode", "path");
        choose("shape_combine", "new");
        if (auto* fresh = window->findChild<QCheckBox*>(QStringLiteral("shape_new_layer"))) fresh->setChecked(false);
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    PageCanvas* canvas() const { return window->canvas(); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page() const { return *doc().pages[0]; }
    const core::Layer& target() const { return *window->target_layer(); }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }
    void choose(const char* combo, const char* key) {
        auto* box = window->findChild<QComboBox*>(QString::fromLatin1(combo));
        QVERIFY2(box != nullptr, combo);
        box->setCurrentIndex(box->findData(QString::fromLatin1(key)));
        emit box->activated(box->currentIndex());
    }
    const std::vector<vec::ShapePart>& parts() const { return target().shapes->items; }

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

bool near(double a, double b, double tolerance = 0.3) { return std::fabs(a - b) <= tolerance; }

}  // namespace

class TestGuiShapes : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // The tools: 長方形ツール (O) and the others in the tools' group and the ツール menu, Shift+O going round them; the
    // commands in their menus (Photoshop's names and places); 部分選択ツール the 線の編集 of before.
    void theToolsAndMenus() {
        Book b;
        QCOMPARE(b.act("act_shape")->text(), QStringLiteral("長方形ツール"));
        QCOMPARE(b.act("act_shape")->shortcut(), QKeySequence(QStringLiteral("U")));  // (PC5 H-0: the Photoshop key set)
        QCOMPARE(b.act("act_next_shape_tool")->shortcut(), QKeySequence(QStringLiteral("Shift+U")));
        QCOMPARE(b.act("act_vector")->text(), QStringLiteral("部分選択ツール"));
        QMenu* tools = top(b.window.get(), QStringLiteral("ツール"));
        const std::vector<std::pair<const char*, const char*>> kinds{{"act_shape", "rect"},          {"act_shape_ellipse", "ellipse"}, {"act_shape_triangle", "triangle"},
                                                                     {"act_shape_polygon", "polygon"}, {"act_shape_line", "line"},       {"act_shape_custom", "custom"}};
        for (const auto& [name, kind] : kinds) {
            QCOMPARE(b.act(name)->actionGroup(), b.window->tools_group());
            QVERIFY2(in_menu(tools, b.act(name)), name);
            b.act(name)->trigger();
            QCOMPARE(b.canvas()->tool(), QStringLiteral("shape"));
            QCOMPARE(b.canvas()->shape_kind, QString::fromLatin1(kind));
        }
        b.act("act_shape")->trigger();
        b.act("act_next_shape_tool")->trigger();
        QVERIFY(b.act("act_shape_ellipse")->isChecked());
        QCOMPARE(b.canvas()->shape_kind, QStringLiteral("ellipse"));
        for (int i = 0; i < 5; ++i) b.act("act_next_shape_tool")->trigger();
        QVERIFY(b.act("act_shape")->isChecked());  // (round again)
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("編集")), b.act("act_define_custom_shape")));
        QMenu* layers = top(b.window.get(), QStringLiteral("レイヤー"));
        QMenu* combine = layers->findChild<QMenu*>(QStringLiteral("menu_combine_shapes"));
        QVERIFY(combine != nullptr && combine->title() == QStringLiteral("シェイプを結合"));
        for (const char* name : {"act_combine_unite", "act_combine_subtract", "act_combine_intersect", "act_combine_exclude", "act_merge_components"})
            QVERIFY2(in_menu(combine, b.act(name)), name);
        QVERIFY(in_menu(layers->findChild<QMenu*>(QStringLiteral("menu_rasterize")), b.act("act_rasterize_shape")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("選択範囲")), b.act("act_sel_to_shape")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("ページ")), b.act("act_line_to_shape")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("表示")), b.act("act_guides_from_shape")));
        QVERIFY(in_menu(top(b.window.get(), QStringLiteral("ウィンドウ")), b.window->shapes_dock()->toggleViewAction()));
    }

    // シェイプ: a drag makes a shape part (its live shape) on the pen layer drawn on; Alt from the centre; Shift a square;
    // a star by its ratio; 新しいレイヤー a pen layer of its own; on a layer that is not a pen layer, a new one too.
    void theShapeMode() {
        Book b;
        QVERIFY(b.target().kind == core::LayerKind::Strokes);
        b.act("act_shape")->trigger();
        b.drag(20, 20, 60, 40);
        QVERIFY2(b.parts().size() == 1, qPrintable(b.window->last_notice()));
        const vec::ShapePart& r = b.parts()[0];
        QVERIFY(r.live && r.live->kind == vec::LiveShape::Kind::Rect);
        QVERIFY(near(r.live->rect[0], 20) && near(r.live->rect[2], 40) && near(r.live->rect[3], 20));
        QVERIFY(r.fill.is_object() && r.fill.contains("rgb"));
        QVERIFY(b.canvas()->target_path.startsWith(QStringLiteral("shape:")));
        b.act("act_shape_ellipse")->trigger();
        b.drag(100, 100, 110, 105, Qt::AltModifier);  // (from the centre: 20 × 10 about the press)
        const vec::ShapePart& e = b.parts().back();
        QVERIFY(e.live && e.live->kind == vec::LiveShape::Kind::Ellipse);
        QVERIFY2(near(e.live->rect[0], 90) && near(e.live->rect[1], 95) && near(e.live->rect[2], 20) && near(e.live->rect[3], 10),
                 vec::to_json(e).dump().c_str());
        b.drag(150, 150, 170, 160, Qt::ShiftModifier);  // (a circle)
        QVERIFY(near(b.parts().back().live->rect[2], b.parts().back().live->rect[3], 1e-6));
        b.act("act_shape_polygon")->trigger();
        b.window->findChild<QSpinBox*>(QStringLiteral("shape_star"))->setValue(50);
        b.drag(20, 150, 60, 190);
        QVERIFY(b.parts().back().live && b.parts().back().live->star_ratio == 0.5);
        b.window->findChild<QSpinBox*>(QStringLiteral("shape_star"))->setValue(100);
        b.act("act_shape_line")->trigger();
        b.drag(20, 220, 80, 220);
        QVERIFY(b.parts().back().live && b.parts().back().live->kind == vec::LiveShape::Kind::Line);
        const std::size_t layers = b.page().layers.size();
        b.window->findChild<QCheckBox*>(QStringLiteral("shape_new_layer"))->setChecked(true);
        b.act("act_shape_triangle")->trigger();
        b.drag(100, 200, 140, 240);
        QCOMPARE(b.page().layers.size(), layers + 1);
        const std::string drawn_on = b.target().id;
        const core::Layer* fresh = nullptr;  // (over the layer drawn on)
        for (std::size_t i = 0; i + 1 < b.page().layers.size(); ++i)
            if (b.page().layers[i].id == drawn_on) fresh = &b.page().layers[i + 1];
        QVERIFY(fresh != nullptr && fresh->kind == core::LayerKind::Strokes && fresh->shapes->items.size() == 1);
        QCOMPARE(fresh->title, std::string("三角形"));
        QCOMPARE(fresh->shapes->items[0].live->sides, 3);
        b.window->findChild<QCheckBox*>(QStringLiteral("shape_new_layer"))->setChecked(false);
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().layers.size(), layers);  // (one Undo)
    }

    // パス: the figure's subpaths on the work path; ピクセル: the 図形 of before (the pen's line on the pen layer).
    void thePathAndPixelModes() {
        Book b;
        b.act("act_shape")->trigger();
        b.choose("shape_mode", "path");
        b.drag(20, 20, 60, 40);
        QVERIFY(b.page().work_path.has_value());
        QCOMPARE(b.page().work_path->subpaths.size(), std::size_t{1});
        QVERIFY(b.parts().empty());
        b.choose("shape_mode", "pixel");
        const std::size_t lines = b.target().stroke_count();
        b.drag(80, 80, 120, 120);
        QVERIFY(b.target().stroke_count() > lines);
        QVERIFY(b.parts().empty());
        b.choose("shape_mode", "shape");
    }

    // The pen tools' モード: シェイプ makes a shape part of the subpath drawn; パス (the default) the work path.
    void thePenModes() {
        Book b;
        b.choose("pen_mode", "shape");
        b.act("act_path_pen")->trigger();
        b.click(20, 20);
        b.click(80, 20);
        b.click(50, 70);
        b.click(20, 20);  // (closed)
        QVERIFY2(b.parts().size() == 1, qPrintable(b.window->last_notice()));
        QVERIFY(!b.parts()[0].live.has_value());
        QVERIFY(b.parts()[0].fill.is_object());
        QVERIFY(!b.page().work_path.has_value());
        b.click(100, 100);
        b.click(150, 120);
        b.key(Qt::Key_Return);  // (open: a stroke, no fill)
        QCOMPARE(b.parts().size(), std::size_t{2});
        QVERIFY(b.parts()[1].fill.is_null() && b.parts()[1].stroke.has_value());
        b.choose("pen_mode", "path");
        b.click(20, 120);
        b.click(60, 120);
        b.key(Qt::Key_Return);
        QVERIFY(b.page().work_path.has_value());
    }

    // パス選択ツール on a part: a click on the page chooses it (the パス panel lists the layer's parts), a drag moves it
    // keeping its live shape, Delete takes it out.
    void thePathSelectionOnParts() {
        Book b;
        b.act("act_shape")->trigger();
        b.drag(20, 20, 60, 40);
        const std::string layer = b.target().id, id = b.parts()[0].id;
        b.canvas()->target_path = QStringLiteral("work");
        b.act("act_path_select")->trigger();
        b.drag(40, 30, 50, 35);
        QCOMPARE(b.canvas()->target_path, QStringLiteral("shape:%1/%2").arg(QString::fromStdString(layer), QString::fromStdString(id)));
        QVERIFY(b.parts()[0].live.has_value());
        QVERIFY2(near(b.parts()[0].live->rect[0], 30) && near(b.parts()[0].live->rect[1], 25), vec::to_json(b.parts()[0]).dump().c_str());
        b.window->paths_dock()->show();
        b.window->paths_panel()->refresh();
        const auto& keys = b.window->paths_panel()->keys();
        QVERIFY(std::find(keys.begin(), keys.end(), b.canvas()->target_path) != keys.end());
        b.key(Qt::Key_Delete);
        QVERIFY(b.parts().empty());
    }

    // The シェイプ panel: its groups and shapes; one placed (a part of its name); 編集 → カスタムシェイプを定義… keeping the
    // chosen part's path among the presets (the settings folder's shapes.json); the カスタムシェイプツール drawing it.
    void theShapesPanel() {
        Book b;
        app::ShapesPanel* panel = b.window->shapes_panel();
        QVERIFY(panel != nullptr);
        panel->refresh();
        QVERIFY(panel->groups()->count() >= 5);
        QVERIFY(panel->list()->count() > 0);
        panel->list()->setCurrentRow(0);
        const auto [group, name] = panel->chosen();
        QVERIFY(panel->place());
        QCOMPARE(b.parts().size(), std::size_t{1});
        QCOMPARE(*b.parts()[0].name, name.toStdString());
        QVERIFY(b.window->define_custom_shape(QStringLiteral("自分の形")));
        panel->refresh();
        QVERIFY(render::shape_presets::find(panel->presets(), "自分の形") != nullptr);
        b.window->choose_custom_shape(QString::fromUtf8(std::string(render::shape_presets::kCustomGroup).c_str()), QStringLiteral("自分の形"));
        QCOMPARE(b.canvas()->tool(), QStringLiteral("shape"));
        QCOMPARE(b.canvas()->shape_kind, QStringLiteral("custom"));
        b.drag(100, 100, 140, 140);
        QCOMPARE(b.parts().size(), std::size_t{2});
        QCOMPARE(*b.parts()[1].name, std::string("自分の形"));
        (void)group;
    }

    // レイヤー → シェイプを結合 (the parts of the layer drawn on as one), ラスタライズ → シェイプ, 選択範囲 → シェイプに変換,
    // ページ → 台詞 → シェイプに変換, 表示 → シェイプから新規ガイドを作成.
    void theCommands() {
        Book b;
        b.act("act_shape")->trigger();
        b.drag(20, 20, 60, 60);
        b.drag(40, 40, 80, 80);
        QCOMPARE(b.parts().size(), std::size_t{2});
        const std::size_t rulers = b.page().rulers.size();
        b.act("act_guides_from_shape")->trigger();
        QCOMPARE(b.page().rulers.size(), rulers + 6);
        b.act("act_combine_subtract")->trigger();
        QCOMPARE(b.parts().size(), std::size_t{1});
        QVERIFY(b.parts()[0].path.subpaths[1].op == vec::PathOp::Subtract);
        b.act("act_merge_components")->trigger();
        for (std::size_t i = 1; i < b.parts()[0].path.subpaths.size(); ++i) QVERIFY(b.parts()[0].path.subpaths[i].op == vec::PathOp::Exclude);
        const std::size_t patches = b.target().patches.size();
        b.act("act_rasterize_shape")->trigger();
        QVERIFY(b.parts().empty());
        QCOMPARE(b.target().patches.size(), patches + 1);
        b.canvas()->set_selection(Json{{"poly", Json::array({Json::array({20, 100}), Json::array({70, 100}), Json::array({70, 140}), Json::array({20, 140})})}});
        b.act("act_sel_to_shape")->trigger();
        QCOMPARE(b.parts().size(), std::size_t{1});
        QVERIFY(b.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "t1"}, {"text", "台詞"}, {"x_mm", 100}, {"y_mm", 100}}));
        b.canvas()->selected_line_id = "t1";
        b.act("act_line_to_shape")->trigger();
        QCOMPARE(b.parts().size(), std::size_t{2});
    }
};

QTEST_MAIN(TestGuiShapes)
#include "test_gui_shapes.moc"
