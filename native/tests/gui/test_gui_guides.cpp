// Rulers, the grid and 3D (M3④-4, Python's main.py, canvas_guides.py and guide_panel.py) on the offscreen platform:
// their commands with Python's words, keys and tips (ツール → 定規 / 3D); a ruler placed by a drag or click by click,
// its point dragged, made a layer's own, drawn with the pen, made a selection or a panel, fixed, its perspective grid;
// the snapping and the grid; figures, boxes, props, a background and a model placed, dragged, turned, posed (the hand
// pulled with IK) and traced; the 定規・3D panel's edits — each becoming the op Python's window makes.
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSlider>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <tuple>
#include <fstream>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/guide_dialogs.hpp"
#include "app/guide_panel.hpp"
#include "app/info_panels.hpp"
#include "app/page_size_dialogs.hpp"
#include "app/preferences.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/json.hpp"
#include "core/mannequin.hpp"
#include "core/mesh3d.hpp"
#include "core/poses.hpp"
#include "core/pyops.hpp"
#include "core/prim3d.hpp"
using namespace genko;
using core::Json;
using app::PageCanvas;
namespace inject = genko::app::inject;

namespace {

core::Document book_doc() {
    core::Document doc = core::new_episode("定規", core::Num(1), 1, core::PageSpec::custom(160, 200, 140, 180, 1, 2, 2, 2, 2, 72, "mono"));
    doc.edit_page(0).name_ok = true;
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 900);
        window->show();
        canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return *window->current_page(); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    QAction* titled(const QString& text) const {
        for (QAction* a : window->findChildren<QAction*>())
            if (a->text() == text) return a;
        return nullptr;
    }
    QStringList menus_of(QAction* wanted) const {
        QStringList found;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
            for (QAction* a : menu->actions()) {
                if (a == wanted) found << path;
                if (a->menu() != nullptr) walk(a->menu(), path + QStringLiteral("/") + a->menu()->title());
            }
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
        return found;
    }
    // the prims of the page by kind
    const Json* prim(const std::string& kind) const {
        for (const Json& p : page().prims)
            if (p.value("kind", std::string()) == kind) return &p;
        return nullptr;
    }
};

bool close_to(const Json& v, double want, double within = 0.05) { return v.is_number() && std::abs(v.get<double>() - want) <= within; }

// (PC5 H-b: the view's switches, the guides' lock and the units this computer keeps, as a new person has them)
void fresh_view_settings() {
    const auto store = genko::app::settings();
    for (const char* key : {"view/extras", "view/show_guides", "view/show_canvas_guides", "view/show_smart_guides", "view/show_layer_edges",
                            "view/show_mesh", "view/show_edit_pins", "guides/lock", "guides/grid", "guides/grid_mm", "units", "guide_layouts"})
        store->remove(QString::fromLatin1(key));
}

}  // namespace

class TestGuiGuides : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<genko::app::BookChange>();
    }

    void commandsAsPythonsWindowHasThem() {
        Studio s;
        struct E {
            const char* attribute;
            const char* text;
            const char* key;
            const char* tip;
            bool checkable;
            const char* menu;
        };
        const std::vector<E> expected = {
            {"act_ruler", "定規", "R", "「定規」メニューで選んだ定規を置く（ドラッグ・クリック）。置いた定規の□をドラッグで動かす", true, "ツール"},
            {"act_3d", "3D 操作", "Shift+D",  // (PC5 H-0, the Photoshop key set: J is スポット修復ブラシツール's)
             "デッサン人形の関節（○）や箱をドラッグして動かす。箱の上の○で回す", true, "ツール"},
            {"act_snap", "描画定規にスナップ", "",  // (PC5 H-0: the user's decision 12; Ctrl+2 the composite channel)
             "ペンの線を定規に沿わせる（切ると自由に描ける）", true, "ツール/定規"},
            {"act_show_rulers", "描画定規を表示", "",  // (Ctrl+Shift+R: レンズ補正…)
             "", true, "ツール/定規"},
            // (PC5 H-b: Photoshop's 表示 → 表示・非表示 ▸ グリッド — and in ツール → 定規 as before)
            {"act_grid", "グリッド", "Ctrl+'", "", true, "ツール/定規"},
            {"act_grid_snap", "グリッドにスナップ", "", "Shift で引く直線と定規の点がグリッドに吸い付く", true, "ツール/定規"},
            {"act_grid_mm", "グリッドの間隔…", "", "", false, "ツール/定規"},
            {"act_del_ruler", "選んだ定規を消す", "", "", false, "ツール/定規"},
            {"act_ruler_layer", "選んだ定規をこのレイヤー専用にする／戻す", "", "描く先のレイヤーを描いている時だけ、その定規が見えて効きます", false, "ツール/定規"},
            {"act_ruler_pen", "選んだ定規の線を描く（定規ペン）", "", "定規そのものを、描く先のレイヤーにペンの線で描きます", false, "ツール/定規"},
            {"act_ruler_selection", "選んだ定規から選択範囲を作る", "", "閉じた形の定規（図形定規・円・閉じた曲線）の中を選択範囲にします", false, "ツール/定規"},
            {"act_persp_grid", "パース定規のグリッド…", "", "選んだパース定規に、地面のグリッドを出す（線の数。0 で消す）", false, "ツール/定規"},
            {"act_ruler_from_3d", "3D に合わせてパース定規を作る", "", "ページの 3D（選んだもの、なければ最初のもの）の消失点にパース定規を置きます", false,
             "ツール/定規"},
            {"act_camera_from_ruler", "カメラを選んだパース定規に合わせる", "", "3D のカメラを回して、3D の消失点が選んだパース定規の消失点に来るようにします", false,
             "ツール/定規"},
            {"act_ruler_frame", "選んだ定規でコマを割る・作る", "", "直線の定規: その線でコマを割ります。円・閉じた曲線の定規: その形のコマを作ります", false,
             "ツール/定規"},
            {"act_ruler_fix", "選んだ定規を固定する／外す", "", "点を動かせないようにします", false, "ツール/定規"},
            {"act_ruler_horizon", "パースの目の高さを固定する／外す", "", "消失点を動かしても、アイレベル（目の高さ）の上を滑るだけにします", false, "ツール/定規"},
            {"act_clear_rulers", "このページの定規をすべて消す", "", "", false, "ツール/定規"},
            {"act_add_figure", "デッサン人形を置く", "",
             "体型を変えられ、関節をドラッグでポーズを付けられる人形を、選んだコマ（なければページ）の真ん中に置きます", false, "ツール/3D"},
            {"act_add_stick", "棒人形（手早いポーズ用）を置く", "", "", false, "ツール/3D"},
            {"act_add_head", "頭部（顔の向きの目安）を置く", "", "", false, "ツール/3D"},
            {"act_add_hand", "手（指のポーズ）を置く", "", "", false, "ツール/3D"},
            {"act_import_obj", "3D モデルを読み込む（OBJ・glTF・VRM）…", "", "", false, "ツール/3D"},
            {"act_add_box", "3D の箱を置く", "", "", false, "ツール/3D"},
            {"act_add_cylinder", "3D の円柱を置く", "", "", false, "ツール/3D"},
            {"act_add_stairs", "3D の階段を置く", "", "", false, "ツール/3D"},
            {"act_add_floor", "床（パースの格子）を置く", "", "地面の格子で、背景のパースの目安にします", false, "ツール/3D"},
            {"act_add_sphere", "3D の球を置く", "", "", false, "ツール/3D"},
            {"act_add_cone", "3D の円錐を置く", "", "", false, "ツール/3D"},
            {"act_trace", "3D を線にする（描く先のレイヤーへ）", "", "このページの 3D を鉛筆の線にして下描きにします", false, "ツール/3D"},
            {"act_del_prim", "選んだ 3D を消す", "", "", false, "ツール/3D"},
        };
        for (const E& e : expected) {
            QAction* a = s.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), QKeySequence(QString::fromLatin1(e.key)));
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QVERIFY2(s.menus_of(a).contains(QString::fromUtf8(e.menu)), e.attribute);
        }
        // the 16 ruler kinds, the 8 props, the 4 backgrounds and the poses, in their menus
        for (const char* title : {"直線定規", "パース定規（3 点）", "対称定規（回転）…", "図形定規（多角形）"}) QVERIFY2(s.titled(QString::fromUtf8(title)), title);
        QVERIFY(s.menus_of(s.titled(QStringLiteral("小物: 車"))).contains(QStringLiteral("ツール/3D/小物")));
        // (the scenes and the poses in menus of their own under 3D, as Python's "scenes" and "poses" entries)
        QVERIFY(s.menus_of(s.titled(QStringLiteral("背景: 教室"))).contains(QStringLiteral("ツール/3D/背景の 3D を置く")));
        QVERIFY(s.menus_of(s.titled(QStringLiteral("ポーズ: ピース"))).contains(QStringLiteral("ツール/3D/ポーズ")));
    }

    void rulersPlacedEditedAndUsed() {
        Studio s;
        // a straight ruler dragged
        s.titled(QStringLiteral("直線定規"))->trigger();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("ruler"));
        QCOMPARE(s.canvas()->ruler_kind, QStringLiteral("line"));
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("直線定規: ")));
        s.trigger("act_snap");  // (off: placing one turns it on again)
        QVERIFY(!s.canvas()->snap_rulers);
        inject::mouse_stroke(s.canvas(), {QPointF(30, 60), QPointF(80, 61), QPointF(130, 60)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_ruler"));
        QCOMPARE(op["kind"], Json("line"));
        QCOMPARE(op["points"].size(), std::size_t{2});
        const std::string line_id = op["id"].get<std::string>();
        QCOMPARE(s.canvas()->selected_ruler_id, std::optional<std::string>(line_id));
        QVERIFY(s.canvas()->snap_rulers && s.window->action("act_snap")->isChecked());
        // its end dragged
        const Json end = s.page().rulers[0]["points"][1];
        inject::mouse_stroke(s.canvas(), {QPointF(end[0].get<double>(), end[1].get<double>()), QPointF(130, 80), QPointF(130, 90)});
        op = s.last();
        QCOMPARE(op["op"], Json("edit_ruler"));
        QCOMPARE(op["id"], Json(line_id));
        QVERIFY(close_to(op["points"][1][1], 90, 0.5));
        // a layer's own, fixed, drawn with the pen, a panel cut along it
        s.trigger("act_ruler_layer");
        QCOMPARE(s.last()["layer_id"], Json(s.window->target_layer()->id));
        s.trigger("act_ruler_layer");
        QVERIFY(s.last()["layer_id"].is_null());
        s.trigger("act_ruler_fix");
        QCOMPARE(s.last()["fixed"], Json(true));
        s.trigger("act_ruler_pen");
        QCOMPARE(s.last()["op"], Json("ruler_to_layer"));
        s.trigger("act_ruler_frame");
        QCOMPARE(s.last()["op"], Json("cut_frame"));
        // a shape ruler: the selection and a panel of its shape
        s.titled(QStringLiteral("図形定規（長方形）"))->trigger();
        inject::mouse_stroke(s.canvas(), {QPointF(40, 120), QPointF(80, 150)});
        QCOMPARE(s.last()["kind"], Json("rect"));
        s.trigger("act_ruler_selection");
        QVERIFY(s.canvas()->selection().has_value());
        QVERIFY(s.canvas()->selection()->area.contains("poly"));
        s.window->choose_tool(QStringLiteral("ruler"));
        s.trigger("act_ruler_frame");
        QCOMPARE(s.last()["op"], Json("add_frame"));
        // a curve clicked point by point, ended by Enter; Esc forgets one
        s.titled(QStringLiteral("曲線定規"))->trigger();
        for (const QPointF& p : {QPointF(20, 20), QPointF(60, 35), QPointF(100, 20)}) inject::click_mm(s.canvas(), p);
        QTest::keyClick(s.canvas(), Qt::Key_Return);
        QCOMPARE(s.last()["kind"], Json("curve"));
        QCOMPARE(s.last()["points"].size(), std::size_t{3});
        const std::size_t before = s.ops.size();
        inject::click_mm(s.canvas(), QPointF(20, 30));
        inject::click_mm(s.canvas(), QPointF(50, 40));
        QTest::keyClick(s.canvas(), Qt::Key_Escape);
        QTest::keyClick(s.canvas(), Qt::Key_Return);
        QCOMPARE(s.ops.size(), before);
        // two-point perspective: placed at its second click; its ground grid; its eye level fixed
        s.titled(QStringLiteral("パース定規（2 点）"))->trigger();
        inject::click_mm(s.canvas(), QPointF(10, 50));
        inject::click_mm(s.canvas(), QPointF(150, 50));
        op = s.last();
        QCOMPARE(op["kind"], Json("perspective"));
        QCOMPARE(op["points"].size(), std::size_t{2});
        s.answers.responder->get_int = [](const QString&, const QString&, int, int, int) { return std::optional<int>(8); };
        s.trigger("act_persp_grid");
        QCOMPARE(s.last()["grid"], Json(8));
        s.trigger("act_ruler_horizon");
        QCOMPARE(s.last()["lock_horizon"], Json(true));
        // symmetry about a centre: its copies asked
        s.titled(QStringLiteral("対称定規（回転）…"))->trigger();
        QCOMPARE(s.canvas()->ruler_copies, 8);
        inject::mouse_stroke(s.canvas(), {QPointF(80, 100), QPointF(80, 140)});
        QCOMPARE(s.last()["copies"], Json(8));
        // the toggles and the grid
        s.trigger("act_grid");
        QVERIFY(s.canvas()->grid_visible);
        QCOMPARE(app::settings()->value(QStringLiteral("guides/grid")).toString(), QStringLiteral("true"));
        s.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(10); };
        s.trigger("act_grid_mm");
        QCOMPARE(s.canvas()->grid_mm, 10.0);
        s.trigger("act_grid_snap");
        QCOMPARE(s.canvas()->grid_point(14, 26), QPointF(10, 30));
        s.trigger("act_grid_snap");
        s.trigger("act_show_rulers");
        QVERIFY(!s.canvas()->rulers_visible);
        s.trigger("act_show_rulers");
        // the pen snaps only while 定規にスナップ is on
        s.window->choose_tool(QStringLiteral("pen"));
        inject::mouse_stroke(s.canvas(), {QPointF(30, 62), QPointF(60, 63)});
        QCOMPARE(s.last()["snap_ruler"], Json(true));
        s.trigger("act_snap");
        inject::mouse_stroke(s.canvas(), {QPointF(30, 64), QPointF(60, 65)});
        QVERIFY(!s.last().contains("snap_ruler"));
        // one ruler, then all of them, taken away
        s.window->choose_tool(QStringLiteral("ruler"));
        s.trigger("act_del_ruler");
        QCOMPARE(s.last()["op"], Json("delete_ruler"));
        QVERIFY(s.last().contains("id"));
        s.trigger("act_clear_rulers");
        QVERIFY(!s.last().contains("id"));
        QVERIFY(s.page().rulers.empty());
    }

    void theLineShowsWhereTheRulerPutsIt() {
        // Python's snapped_preview: while the pen is down its line is shown snapped to the ruler, with the copies the
        // symmetry ruler will make; the eraser's line is shown snapped, alone
        Studio s;
        s.titled(QStringLiteral("直線定規"))->trigger();
        inject::mouse_stroke(s.canvas(), {QPointF(20, 60), QPointF(80, 60), QPointF(140, 60)});
        s.titled(QStringLiteral("対称定規（左右）"))->trigger();
        inject::mouse_stroke(s.canvas(), {QPointF(80, 20), QPointF(80, 100), QPointF(80, 180)});
        QCOMPARE(s.page().rulers.size(), std::size_t{2});
        s.window->choose_tool(QStringLiteral("pen"));
        inject::mouse(s.canvas(), inject::Phase::Press, QPointF(30, 62));
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(45, 63.5));
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(60, 62.5));
        QVERIFY(s.canvas()->live() != nullptr && s.canvas()->live_copy(0) != nullptr);
        QVERIFY(s.canvas()->live_copy(1) == nullptr);
        // (where the drawn pixels lie, in mm: the ink's box has room to grow round them)
        const auto drawn = [](const app::LiveInk* ink) {
            const QImage& image = ink->image();
            double sx = 0, sy = 0, n = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 0) sx += x, sy += y, n += 1;
            const double px = 25.4 / ink->dpi();
            return n == 0 ? QPointF(-1, -1) : QPointF((ink->box().x() + sx / n + 0.5) * px, (ink->box().y() + sy / n + 0.5) * px);
        };
        const QPointF main = drawn(s.canvas()->live()), copy = drawn(s.canvas()->live_copy(0));
        QVERIFY2(std::abs(main.y() - 60) < 0.5, qPrintable(QString::number(main.y())));  // (not 62.75)
        QVERIFY2(std::abs(main.x() - 45) < 1, qPrintable(QString::number(main.x())));
        QVERIFY2(std::abs(copy.x() - 115) < 1, qPrintable(QString::number(copy.x())));  // (mirrored about x = 80)
        QVERIFY2(std::abs(copy.y() - 60) < 0.5, qPrintable(QString::number(copy.y())));
        inject::mouse(s.canvas(), inject::Phase::Release, QPointF(60, 62.5));
        QCOMPARE(s.last()["snap_ruler"], Json(true));
        QVERIFY(s.canvas()->live() == nullptr && s.canvas()->live_copy(0) == nullptr);
        // the eraser: its line on the ruler, no copy
        s.window->choose_tool(QStringLiteral("eraser"));
        QApplication::processEvents();
        const QImage before = s.canvas()->grab().toImage();
        inject::mouse(s.canvas(), inject::Phase::Press, QPointF(135, 66));  // (within the ruler's reach, 10 mm)
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(142, 66));
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(150, 66));
        const QImage during = s.canvas()->grab().toImage();
        const auto at = [&](const QImage& image, double x, double y) { return image.pixelColor(s.canvas()->to_widget(QPointF(x, y)).toPoint()); };
        QVERIFY(at(during, 142, 66) == at(before, 142, 66));  // (the line is not shown where it was drawn …)
        QVERIFY(at(during, 142, 60) != at(before, 142, 60));  // (… but on the ruler)
        QVERIFY(at(during, 18, 60) == at(before, 18, 60));    // (and not mirrored)
        inject::mouse(s.canvas(), inject::Phase::Release, QPointF(150, 66));
    }

    void figuresAndBoxesPlacedDraggedAndPosed() {
        Studio s;
        s.trigger("act_add_box");
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_prim3d"));
        QCOMPARE(op["kind"], Json("box"));
        QCOMPARE(s.canvas()->tool(), QStringLiteral("3d"));
        QCOMPARE(s.canvas()->selected_prim_id, std::optional<std::string>(op["id"].get<std::string>()));
        QDockWidget* dock = nullptr;
        for (QDockWidget* d : s.window->findChildren<QDockWidget*>())
            if (d->windowTitle() == QStringLiteral("定規・3D")) dock = d;
        QVERIFY(dock != nullptr && dock->isVisible());
        QCOMPARE(s.window->guides()->prims->count(), 1);
        // the box moved by its centre, turned by its ○
        const Json& box = *s.prim("box");
        const auto bb = core::prim3d::bbox(box, nullptr);
        const QPointF centre(box["pos"][0].get<double>(), box["pos"][1].get<double>());
        inject::mouse_stroke(s.canvas(), {centre, centre + QPointF(5, 0), centre + QPointF(10, 5)});
        op = s.last();
        QCOMPARE(op["op"], Json("edit_prim"));
        QVERIFY(close_to(op["pos"][0], centre.x() + 10, 0.5));
        const QPointF turn(bb[0] + bb[2] / 2, bb[1] - 8);
        const Json& moved = *s.prim("box");
        const auto mb = core::prim3d::bbox(moved, nullptr);
        const QPointF knob(mb[0] + mb[2] / 2, mb[1] - 8);
        inject::mouse_stroke(s.canvas(), {knob, knob + QPointF(20, 0)});
        QVERIFY(s.last().contains("rot"));
        (void)turn;
        // the others, each in the chosen panel's middle (or the page's), the next beside the last
        for (const auto& [name, want] : {std::pair{"act_add_figure", "add_figure"}, {"act_add_stick", "add_mannequin"}, {"act_add_head", "add_head"},
                                         {"act_add_hand", "add_hand"}, {"act_add_cylinder", "add_prim3d"}, {"act_add_stairs", "add_prim3d"},
                                         {"act_add_floor", "add_prim3d"}, {"act_add_sphere", "add_prim3d"}, {"act_add_cone", "add_prim3d"}}) {
            s.trigger(name);
            QCOMPARE(s.last()["op"], Json(want));
        }
        s.titled(QStringLiteral("小物: 車"))->trigger();
        QCOMPARE(s.last()["prop"], Json("car"));
        s.titled(QStringLiteral("背景: 部屋"))->trigger();
        QCOMPARE(s.last()["op"], Json("add_scene"));
        QCOMPARE(s.last()["kind"], Json("room"));
        // a model from a file
        const QString obj = s.tmp.path() + "/cube.obj";
        {
            std::ofstream file(obj.toStdString());
            file << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
                    "f 1 2 3 4\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
        }
        s.answers.responder->open_path = [obj](const QString&, const QString&) { return obj; };
        s.trigger("act_import_obj");
        op = s.last();
        QCOMPARE(op["op"], Json("import_model"));
        QVERIFY(op.contains("obj"));
        QCOMPARE(op["name"], Json("cube"));
        // poses: the stick figure's and the 3D figure's
        s.canvas()->selected_prim_id = s.prim("mannequin")->value("id", std::string());
        s.titled(QStringLiteral("ポーズ: 走る"))->trigger();
        QCOMPARE(s.last()["op"], Json("pose_mannequin"));
        QCOMPARE(s.last()["preset"], Json("run"));
        s.canvas()->selected_prim_id = s.prim("figure")->value("id", std::string());
        s.titled(QStringLiteral("ポーズ: ピース"))->trigger();
        QCOMPARE(s.last()["op"], Json("pose_figure"));
        s.titled(QStringLiteral("ポーズ: 振り返る"))->trigger();  // (the stick figure's own)
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("このポーズは棒人形だけのもの")));
        // the 3D figure's hand pulled: with IK, the arm follows
        s.window->choose_tool(QStringLiteral("3d"));
        const Json& figure = *s.prim("figure");
        const core::Json* camera = nullptr;
        QPointF hand;
        for (const auto& [name, at] : core::mesh3d::figure_handles(figure, camera))
            if (name == "l_hand") hand = QPointF(at[0], at[1]);
        QVERIFY(s.window->guides()->ik->isChecked());
        inject::mouse_stroke(s.canvas(), {hand, hand + QPointF(3, -3), hand + QPointF(6, -6)});
        op = s.last();
        QCOMPARE(op["op"], Json("pose_figure"));
        QCOMPARE(op["drag"]["handle"], Json("l_hand"));
        QCOMPARE(op["drag"]["ik"], Json(true));
        // traced as lines; the perspective ruler from the 3D, the camera from it
        s.trigger("act_trace");
        QCOMPARE(s.last()["op"], Json("trace_prims"));
        s.trigger("act_ruler_from_3d");
        QCOMPARE(s.last()["op"], Json("ruler_from_3d"));
        // the chosen 3D taken away
        const std::size_t count = s.page().prims.size();
        s.trigger("act_del_prim");
        QCOMPARE(s.page().prims.size(), count - 1);
    }

    void thePanelsEdits() {
        Studio s;
        app::GuidePanel* panel = s.window->guides();
        s.trigger("act_add_figure");
        s.window->show_dock(QStringLiteral("定規・3D"));
        panel->refresh();
        QCOMPARE(panel->prims->count(), 1);
        QCOMPARE(panel->prims->item(0)->text(), QStringLiteral("デッサン人形（3D） 1"));
        panel->prims->setCurrentRow(0);
        // its size, its pose
        panel->size->setValue(120);
        emit panel->size->editingFinished();
        QCOMPARE(s.last()["op"], Json("edit_prim"));
        QCOMPARE(s.last()["size"], Json::array({60.0, 120.0, 30.0}));
        panel->preset->setCurrentIndex(panel->preset->findData(QStringLiteral("think")));
        emit panel->preset->activated(panel->preset->currentIndex());
        QCOMPARE(s.last()["preset"], Json("think"));
        // its body and hands
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QLatin1String("body_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("heads"))->setValue(6);
            auto* hand = dialog->findChild<QComboBox*>(QStringLiteral("hand_l"));
            hand->setCurrentIndex(hand->findData(QStringLiteral("fist")));
            return int(QDialog::Accepted);
        };
        panel->body_dialog();
        Json op = s.last();
        QCOMPARE(op["op"], Json("pose_figure"));
        QCOMPARE(op["body"]["heads"], Json(6.0));
        QCOMPARE(op["hands"]["l"], Json("fist"));
        // the camera and the light
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QLatin1String("camera_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QCheckBox*>(QStringLiteral("use"))->setChecked(true);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("turn"))->setValue(30);
            return int(QDialog::Accepted);
        };
        const std::size_t before = s.ops.size();
        panel->camera_dialog();
        QCOMPARE(s.ops.size(), before + 2);
        QCOMPARE(s.ops[before]["op"], Json("set_light"));
        QCOMPARE(s.ops[before + 1]["op"], Json("set_camera"));
        QVERIFY(close_to(s.ops[before + 1]["turn"], 30 * 3.14159265358979 / 180, 1e-6));
        // lines only (no surfaces)
        s.answers.responder->exec = [](QDialog* dialog) { return qobject_cast<QMessageBox*>(dialog) != nullptr ? int(QMessageBox::No) : 0; };
        panel->render_dialog();
        QCOMPARE(s.last()["op"], Json("render_prims"));
        QCOMPARE(s.last()["surfaces"], Json(false));
        // a pose kept with the app's settings, offered again
        panel->keep_pose(QStringLiteral("考える人"));
        bool kept = false;
        for (const Json& pose : core::iterate(core::poses::user_poses())) kept = kept || pose.value("name", std::string()) == "考える人";
        QVERIFY(kept);
        QVERIFY(panel->preset->findData(QStringLiteral("own:考える人")) >= 0);
        // a ruler listed, turned off from the list, its angle set
        s.titled(QStringLiteral("平行線定規"))->trigger();
        inject::mouse_stroke(s.canvas(), {QPointF(30, 30), QPointF(60, 60)});
        panel->refresh();
        QCOMPARE(panel->rulers->count(), 1);
        QCOMPARE(panel->rulers->item(0)->text(), QStringLiteral("平行線定規"));
        panel->rulers->setCurrentRow(0);
        QVERIFY(panel->angle->isEnabled());
        panel->rulers->item(0)->setCheckState(Qt::Unchecked);
        QCOMPARE(s.last()["active"], Json(false));
        panel->refresh();
        panel->rulers->setCurrentRow(0);
        panel->angle->setValue(30);
        emit panel->angle->editingFinished();
        QCOMPARE(s.last()["angle"], Json(30.0));
        panel->delete_ruler();
        QCOMPARE(s.last()["op"], Json("delete_ruler"));
        panel->refresh();
        QVERIFY(!panel->ruler_hint->isHidden());
    }

    // --- PC5 H-b: ガイド・グリッド・エクストラ・単位 (SPEC.md VIEW-01) ------------------------------------------------------

    // A guide placed is drawn across the view (Photoshop's cyan); エクストラ and 表示・非表示 ▸ ガイド hide it.
    void guidesDrawnAndHidden() {
        fresh_view_settings();
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 50}}})));
        const int x = static_cast<int>(std::lround(s.canvas()->to_widget(QPointF(50, 100)).x()));
        const auto column = [&] {
            const QImage image = s.canvas()->grab().toImage();
            QVector<QRgb> out;
            for (int y = image.height() / 4; y < image.height() * 3 / 4; y += 7)
                for (int dx = -1; dx <= 1; ++dx) out << image.pixel(x + dx, y);
            return out;
        };
        const auto cyan = [](const QVector<QRgb>& pixels) {
            return std::count_if(pixels.begin(), pixels.end(), [](QRgb p) { return qRed(p) < 200 && qGreen(p) > 200 && qBlue(p) > 200; });
        };
        const QVector<QRgb> shown = column();
        QVERIFY2(cyan(shown) > 20, QByteArray::number(static_cast<int>(cyan(shown))));
        s.trigger("act_extras");  // (off)
        QVERIFY(!s.canvas()->extras_shown);
        const QVector<QRgb> hidden = column();
        QCOMPARE(cyan(hidden), qsizetype(0));
        s.trigger("act_extras");
        QVERIFY(cyan(column()) > 20);
        s.trigger("act_show_guides");  // (ガイド off: hidden as by エクストラ)
        QCOMPARE(cyan(column()), qsizetype(0));
        s.trigger("act_show_guides");
        s.trigger("act_show_canvas_guides");
        QCOMPARE(cyan(column()), qsizetype(0));
        s.trigger("act_show_canvas_guides");
        QVERIFY(cyan(column()) > 20);
        // (Photoshop's keys: エクストラ Ctrl+H, ガイド Ctrl+;, ガイドをロック Ctrl+Alt+;)
        QCOMPARE(s.window->action("act_extras")->shortcut(), QKeySequence(QStringLiteral("Ctrl+H")));
        QCOMPARE(s.window->action("act_show_guides")->shortcut(), QKeySequence(QStringLiteral("Ctrl+;")));
        QCOMPARE(s.window->action("act_lock_guides")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+;")));
    }

    // The move tool drags a guide (other tools with Ctrl; the ruler tool never); Alt+click turns it; let go off the view it
    // goes; double-clicked, 新規ガイド… moves it; locked, none moves.
    void guidesMovedTurnedTakenAwayAndLocked() {
        fresh_view_settings();
        Studio s;
        PageCanvas* c = s.canvas();
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 50}}})));
        const std::string id = s.page().rulers[0]["id"].get<std::string>();
        const auto at = [&] { return s.page().rulers[0]["at"].get<double>(); };
        s.window->choose_tool(QStringLiteral("move"));
        inject::mouse_stroke(c, {QPointF(50, 100), QPointF(60, 100), QPointF(70, 100)});
        QCOMPARE(s.last()["op"], Json("edit_ruler"));
        QCOMPARE(s.last()["id"], Json(id));
        QVERIFY(close_to(s.last()["at"], 70, 0.3));
        // (the brush draws without Ctrl; with it, it drags the guide)
        s.window->choose_tool(QStringLiteral("pen"));
        inject::mouse_stroke(c, {QPointF(at(), 100), QPointF(at() + 10, 100)}, Qt::ControlModifier);
        QCOMPARE(s.last()["op"], Json("edit_ruler"));
        QVERIFY(close_to(s.page().rulers[0]["at"], 80, 0.3));
        // (Alt+click with the move tool: a guide across through the point clicked)
        s.window->choose_tool(QStringLiteral("move"));
        inject::click_mm(c, QPointF(at(), 120), Qt::AltModifier);
        QCOMPARE(s.page().rulers[0]["axis"], Json("h"));
        QVERIFY(close_to(s.page().rulers[0]["at"], 120, 0.3));
        // (the ruler tool places a ruler there, never grabs the guide)
        s.titled(QStringLiteral("直線定規"))->trigger();
        inject::mouse_stroke(c, {QPointF(30, 120), QPointF(90, 120)});
        QCOMPARE(s.last()["op"], Json("add_ruler"));
        QVERIFY(close_to(s.page().rulers[0]["at"], 120, 0.3));
        // locked: not grabbed
        s.window->choose_tool(QStringLiteral("move"));
        s.trigger("act_lock_guides");
        QVERIFY(c->lock_guides);
        QVERIFY(!c->guide_at(c->to_widget(QPointF(70, 120))));
        inject::mouse_stroke(c, {QPointF(70, 120), QPointF(70, 140)});
        QVERIFY(close_to(s.page().rulers[0]["at"], 120, 0.3));
        s.trigger("act_lock_guides");
        // double-clicked: 新規ガイド… with its way and place, moved
        s.answers.responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<app::NewGuideDialog*>(dialog);
            if (d == nullptr) return 0;
            if (!d->across->isChecked() || std::abs(d->at_mm() - 120) > 0.3) return 0;
            d->position->setValue(30);
            return int(QDialog::Accepted);
        };
        const QPointF on = c->to_widget(QPointF(70, 120));
        QMouseEvent twice(QEvent::MouseButtonDblClick, on, c->mapToGlobal(on), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(c, &twice);
        QCOMPARE(s.last()["op"], Json("edit_ruler"));
        QVERIFY(close_to(s.page().rulers[0]["at"], 30, 0.01));
        // dragged out of the view: taken away
        const QPointF from = c->to_widget(QPointF(70, 30));
        const QPointF off(-20, from.y());
        for (const auto& [type, where, button, buttons] :
             {std::tuple{QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::MouseButtons(Qt::LeftButton)},
              std::tuple{QEvent::MouseMove, from + QPointF(0, 30), Qt::NoButton, Qt::MouseButtons(Qt::LeftButton)},
              std::tuple{QEvent::MouseMove, off, Qt::NoButton, Qt::MouseButtons(Qt::LeftButton)},
              std::tuple{QEvent::MouseButtonRelease, off, Qt::LeftButton, Qt::MouseButtons(Qt::NoButton)}}) {
            QMouseEvent event(type, where, c->mapToGlobal(where), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(c, &event);
        }
        QVERIFY2(s.last()["op"] == Json("delete_ruler"), (s.last().dump() + " " + s.page().rulers.dump()).c_str());
        QCOMPARE(s.last()["id"], Json(id));
        for (const Json& r : s.page().rulers) QVERIFY(r.value("kind", std::string()) != "guide");
        genko::app::settings()->remove(QStringLiteral("guides/lock"));
    }

    // 表示・非表示 ▸ in Photoshop's order; すべて・なし; an item turned on turns エクストラ on; エクストラオプションを表示…; kept for
    // the next window.
    void theShowMenuAndExtras() {
        fresh_view_settings();
        Studio s;
        QMenu* shown = s.window->findChild<QMenu*>(QStringLiteral("menu_show"));
        QVERIFY(shown != nullptr);
        QStringList items;
        for (QAction* a : shown->actions()) items << (a->isSeparator() ? QStringLiteral("|") : a->text());
        QCOMPARE(items, (QStringList{QStringLiteral("レイヤーエッジ"), QStringLiteral("選択範囲のエッジ"), QStringLiteral("ターゲットパス"), QStringLiteral("グリッド"),
                                     QStringLiteral("ガイド"), QStringLiteral("カンバスガイド"), QStringLiteral("スマートガイド"), QStringLiteral("ピクセルグリッド"),
                                     QStringLiteral("ブラシプレビュー"), QStringLiteral("メッシュ"), QStringLiteral("編集ピン"), QStringLiteral("|"),
                                     QStringLiteral("すべて"), QStringLiteral("なし"), QStringLiteral("|"), QStringLiteral("エクストラオプションを表示…")}));
        PageCanvas* c = s.canvas();
        const bool brush = c->brush_preview;
        s.trigger("act_show_none");
        for (bool on : {c->show_layer_edges, c->show_selection_edges, c->show_target_path, c->grid_visible, c->show_guide_lines, c->show_canvas_guides,
                        c->show_smart_guides, c->show_pixel_grid, c->show_mesh, c->show_edit_pins})
            QVERIFY(!on);
        QCOMPARE(c->brush_preview, brush);  // (not one of them)
        s.trigger("act_extras");
        QVERIFY(!c->extras_shown);
        s.trigger("act_show_guides");  // (an item turned on: エクストラ too, as Photoshop's)
        QVERIFY(c->show_guide_lines && c->extras_shown);
        s.trigger("act_show_all");
        for (bool on : {c->show_layer_edges, c->show_selection_edges, c->show_target_path, c->grid_visible, c->show_guide_lines, c->show_canvas_guides,
                        c->show_smart_guides, c->show_pixel_grid, c->show_mesh, c->show_edit_pins, c->extras_shown})
            QVERIFY(on);
        // エクストラオプションを表示…: エクストラ and the items chosen together
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QLatin1String("extras_options")) return 0;
            dialog->findChild<QCheckBox*>(QStringLiteral("extras_on"))->setChecked(false);
            dialog->findChild<QCheckBox*>(QStringLiteral("extras_act_show_layer_edges"))->setChecked(false);
            dialog->findChild<QCheckBox*>(QStringLiteral("extras_act_grid"))->setChecked(false);
            return int(QDialog::Accepted);
        };
        s.trigger("act_extras_options");
        QVERIFY(!c->extras_shown && !c->show_layer_edges && !c->grid_visible && c->show_guide_lines);
        // kept: a new window of the book shows the same
        {
            app::MainWindow other(s.session);
            QVERIFY(!other.action("act_extras")->isChecked() && !other.canvas()->extras_shown);
            QVERIFY(!other.action("act_show_layer_edges")->isChecked());
            QVERIFY(other.action("act_show_guides")->isChecked());
        }
        s.trigger("act_show_all");
        for (const char* key : {"view/extras", "view/show_guides", "view/show_canvas_guides", "view/show_smart_guides", "view/show_layer_edges",
                                "view/show_mesh", "view/show_edit_pins", "guides/grid"})
            genko::app::settings()->remove(QString::fromLatin1(key));
        // (メッシュ and 編集ピン: the warps' grid and pins hidden — or shown — with エクストラ)
        s.trigger("act_show_mesh");
        QVERIFY(!c->shows(c->show_mesh) && c->shows(c->show_edit_pins));
        s.trigger("act_show_mesh");
        genko::app::settings()->remove(QStringLiteral("view/show_mesh"));
    }

    // 新規ガイド… and 新規ガイドレイアウトを作成… (its preview on the page while it is open, its presets), in the ruler's unit;
    // ガイドを消去 and カンバスガイドを消去 the guides alone.
    void guideWindowsAndClearing() {
        fresh_view_settings();
        Studio s;
        s.answers.responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<app::NewGuideDialog*>(dialog);
            if (d == nullptr) return 0;
            d->upright->setChecked(true);
            d->position->setValue(42);
            return int(QDialog::Accepted);
        };
        s.trigger("act_new_guide");
        QCOMPARE(s.last()["op"], Json("add_ruler"));
        QCOMPARE(s.last()["kind"], Json("guide"));
        QCOMPARE(s.last()["axis"], Json("v"));
        QVERIFY(close_to(s.last()["at"], 42, 0.001));
        // in pixels (the page's 72 dpi): 72 px is an inch
        genko::app::settings()->setValue(QStringLiteral("units/ruler"), QStringLiteral("px"));
        app::preferences::apply_all(s.window.get());
        QCOMPARE(s.canvas()->ruler_unit, core::units::Unit::Px);
        QString suffix;
        s.answers.responder->exec = [&suffix](QDialog* dialog) {
            auto* d = qobject_cast<app::NewGuideDialog*>(dialog);
            if (d == nullptr) return 0;
            suffix = d->position->suffix();
            d->across->setChecked(true);
            d->position->setValue(72);
            return int(QDialog::Accepted);
        };
        s.trigger("act_new_guide");
        QCOMPARE(suffix, QStringLiteral(" px"));
        QCOMPARE(s.last()["axis"], Json("h"));
        QVERIFY(close_to(s.last()["at"], 25.4, 0.01));
        genko::app::settings()->remove(QStringLiteral("units/ruler"));
        app::preferences::apply_all(s.window.get());
        // the layout: its preview shown while it is open, gone after; the op; a preset kept
        bool previewed = false;
        s.answers.responder->exec = [&s, &previewed](QDialog* dialog) {
            auto* d = qobject_cast<app::GuideLayoutDialog*>(dialog);
            if (d == nullptr) return 0;
            d->columns->setValue(2);
            d->gutter->setValue(0);
            d->margin_on->setChecked(true);
            d->top->setValue(10);
            d->left->setValue(10);
            d->bottom->setValue(10);
            d->right->setValue(10);
            d->clear->setChecked(true);
            previewed = s.canvas()->guide_preview.has_value() && !s.canvas()->guide_preview->down.empty();
            d->save_preset(QStringLiteral("二段"));
            return int(QDialog::Accepted);
        };
        s.trigger("act_new_guide_layout");
        QVERIFY(previewed);
        QVERIFY(!s.canvas()->guide_preview);
        QCOMPARE(s.last()["op"], Json("add_guide_layout"));
        QCOMPARE(s.last()["margin_mm"], Json::array({10.0, 10.0, 10.0, 10.0}));
        QCOMPARE(s.last()["columns"]["count"], Json(2));
        std::vector<double> down;
        for (const Json& r : s.page().rulers)
            if (r.value("axis", std::string()) == "v") down.push_back(r["at"].get<double>());
        QCOMPARE(down, (std::vector<double>{10, 150, 80}));  // (the 160 mm paper: margins 10, the columns' middle 80)
        QVERIFY(genko::app::settings()->contains(QStringLiteral("guide_layouts/二段")));
        genko::app::settings()->remove(QStringLiteral("guide_layouts"));
        // clearing: the guides alone, the drawing rulers left
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "line"},
                                                      {"points", Json::array({Json::array({1, 1}), Json::array({9, 9})})}}})));
        s.trigger("act_clear_canvas_guides");
        QCOMPARE(s.last()["op"], Json("delete_ruler"));
        QCOMPARE(s.last()["kind"], Json("guide"));
        QCOMPARE(s.page().rulers.size(), std::size_t(1));
        const std::size_t before = s.ops.size();
        s.trigger("act_clear_guides");  // (none left: said, nothing done)
        QCOMPARE(s.ops.size(), before);
        QCOMPARE(s.window->last_notice(), QStringLiteral("この原稿にガイドはありません"));
    }

    // 単位・定規: the scales, 情報, 画像解像度… and カンバスサイズ… in the unit; プリントサイズ at the screen's resolution.
    void unitsInTheScaleInfoAndSizes() {
        fresh_view_settings();
        Studio s;
        PageCanvas* c = s.canvas();
        c->show_scale = true;
        const auto strip = [c] { return c->grab().toImage().copy(0, 0, c->width(), PageCanvas::kScalePx); };
        const QImage mm = strip();
        genko::app::settings()->setValue(QStringLiteral("units/ruler"), QStringLiteral("in"));
        genko::app::settings()->setValue(QStringLiteral("units/screen_resolution"), 72);
        app::preferences::apply_all(s.window.get());
        QCOMPARE(c->ruler_unit, core::units::Unit::In);
        QVERIFY(strip() != mm);
        QCOMPARE(c->length_words(25.4, true), QStringLiteral("1.00 in"));
        s.window->info_panel()->show_at(QPointF(25.4, 50.8));
        QCOMPARE(s.window->findChild<QLabel*>(QStringLiteral("infoPlace"))->text(), QStringLiteral("X 1.00 in（72 px）  Y 2.00 in（144 px）"));
        app::ImageSizeDialog image(s.window.get(), s.page());
        QCOMPARE(image.unit->currentData().toString(), QStringLiteral("in"));
        image.width->setValue(2);
        QCOMPARE(image.op()["width_mm"], Json(50.8));
        app::CanvasSizeDialog canvas_size(s.window.get(), s.page());
        QCOMPARE(canvas_size.unit->currentData().toString(), QStringLiteral("in"));
        canvas_size.width->setValue(3);
        QCOMPARE(canvas_size.op()["width_mm"], Json(76.2));
        s.trigger("act_actual");  // (プリントサイズ: 72 dpi now)
        QVERIFY(std::abs(c->scale() - 72 / 25.4) < 1e-6);
        // the grid's spacing asked in the unit, kept in mm
        s.answers.responder->get_double = [](const QString&, const QString& label, double, double, double, int) {
            return label.contains(QStringLiteral("in")) ? std::optional<double>(0.5) : std::optional<double>();
        };
        s.trigger("act_grid_mm");
        QVERIFY(std::abs(c->grid_mm - 12.7) < 1e-9);
        for (const char* key : {"units/ruler", "units/screen_resolution", "guides/grid_mm", "guides/grid"})
            genko::app::settings()->remove(QString::fromLatin1(key));
        app::preferences::apply_all(s.window.get());
        QCOMPARE(c->ruler_unit, core::units::Unit::Mm);
        s.window->info_panel()->show_at(QPointF(25.4, 50.8));
        QCOMPARE(s.window->findChild<QLabel*>(QStringLiteral("infoPlace"))->text(), QStringLiteral("X 25.4 mm（72 px）  Y 50.8 mm（144 px）"));
    }

    // 環境設定 → 単位・定規 and ガイド・グリッド・スライス: in 編集 → 環境設定 ▸, kept, on the canvas.
    void preferencePages() {
        fresh_view_settings();
        Studio s;
        QMenu* panes = s.window->findChild<QMenu*>(QStringLiteral("menu_preferences"));
        QStringList names;
        for (QAction* a : panes->actions()) names << a->text();
        QVERIFY(names.contains(QStringLiteral("単位・定規…")) && names.contains(QStringLiteral("ガイド・グリッド・スライス…")));
        app::preferences::PreferencesDialog dialog(s.window.get());
        dialog.show_pane(QStringLiteral("guidesGridAndSlices"));
        QCOMPARE(dialog.tabs->tabText(dialog.tabs->currentIndex()), QStringLiteral("ガイド・グリッド・スライス"));
        auto* ruler = dialog.units_page->findChild<QComboBox*>(QStringLiteral("prefs_ruler_unit"));
        ruler->setCurrentIndex(ruler->findData(QStringLiteral("cm")));
        auto* style = dialog.guides_page->findChild<QComboBox*>(QStringLiteral("prefs_guide_style"));
        style->setCurrentIndex(style->findData(QStringLiteral("dots")));
        auto* unit = dialog.guides_page->findChild<QComboBox*>(QStringLiteral("prefs_grid_unit"));
        unit->setCurrentIndex(unit->findData(QStringLiteral("cm")));
        dialog.guides_page->findChild<QDoubleSpinBox*>(QStringLiteral("prefs_grid_every"))->setValue(3);
        dialog.guides_page->findChild<QSpinBox*>(QStringLiteral("prefs_grid_subdivisions"))->setValue(6);
        dialog.save();
        const auto store = genko::app::settings();
        QCOMPARE(store->value(QStringLiteral("units/ruler")).toString(), QStringLiteral("cm"));
        QCOMPARE(store->value(QStringLiteral("guides/guide_style")).toString(), QStringLiteral("dots"));
        QCOMPARE(store->value(QStringLiteral("guides/grid_subdivisions")).toInt(), 6);
        QVERIFY(std::abs(store->value(QStringLiteral("guides/grid_mm")).toDouble() - 5.0) < 1e-9);  // (30 mm in 6)
        QCOMPARE(s.canvas()->ruler_unit, core::units::Unit::Cm);
        QCOMPARE(s.canvas()->guide_style, Qt::DotLine);
        QCOMPARE(s.canvas()->grid_subdivisions, 6);
        QVERIFY(std::abs(s.canvas()->grid_mm - 5.0) < 1e-9);
        for (const char* key : {"units", "guides/guide_color", "guides/guide_style", "guides/smart_color", "guides/grid_color", "guides/grid_style",
                                "guides/grid_unit", "guides/grid_subdivisions", "guides/grid_mm"})
            store->remove(QString::fromLatin1(key));
        app::preferences::apply_all(s.window.get());
    }

    // スマートガイド: a moved box's gaps equal to both neighbours', while the drag lasts (not with it off); レイヤーエッジ: the
    // chosen layer's box.
    void smartGuidesAndLayerEdges() {
        fresh_view_settings();
        Studio s;
        PageCanvas* c = s.canvas();
        s.window->choose_tool(QStringLiteral("move"));
        c->snap_boxes = [] {
            return std::vector<std::pair<std::string, std::array<double, 4>>>{{"a", {10, 100, 20, 110}}, {"b", {40, 100, 50, 110}}};
        };
        c->moving_box = [] { return std::optional<std::array<double, 4>>(std::array<double, 4>{24, 100, 34, 110}); };
        const auto drag = [c](bool check) {
            const QPointF a = c->to_widget(QPointF(30, 105)), b = c->to_widget(QPointF(31, 105));
            QMouseEvent press(QEvent::MouseButtonPress, a, c->mapToGlobal(a), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(c, &press);
            QMouseEvent move(QEvent::MouseMove, b, c->mapToGlobal(b), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(c, &move);
            const std::vector<PageCanvas::Spacing> seen = c->spacing();
            if (check) (void)c->grab();  // (drawn: its words in the unit)
            QMouseEvent release(QEvent::MouseButtonRelease, b, c->mapToGlobal(b), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(c, &release);
            return seen;
        };
        const auto seen = drag(true);
        QCOMPARE(seen.size(), std::size_t(2));
        QVERIFY(std::abs(seen[0].from - 20) < 0.3 && std::abs(seen[0].to - 25) < 0.3);
        QVERIFY(std::abs(seen[1].from - 35) < 0.3 && std::abs(seen[1].to - 40) < 0.3);
        QVERIFY(c->spacing().empty());  // (let go: gone)
        s.trigger("act_show_smart_guides");
        QVERIFY(drag(false).empty());
        s.trigger("act_show_smart_guides");
        genko::app::settings()->remove(QStringLiteral("view/show_smart_guides"));
        // レイヤーエッジ: the layer drawn on's box once it is drawn
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", s.window->target_layer()->id},
                                                      {"points", Json::array({Json::array({30, 40}), Json::array({60, 70})})}, {"width_mm", 1}}})));
        s.trigger("act_show_layer_edges");
        QVERIFY(c->show_layer_edges && c->layer_edges);
        QVERIFY(gui_test::wait_for([c] { return c->layer_edges().size() == 1; }));
        const auto box = c->layer_edges().front();
        QVERIFY(box[0] < 30.5 && box[0] > 28 && box[2] > 59.5 && box[2] < 62);
        s.trigger("act_show_layer_edges");
        genko::app::settings()->remove(QStringLiteral("view/show_layer_edges"));
    }
};

QTEST_MAIN(TestGuiGuides)
#include "test_gui_guides.moc"
