// 範囲選択 (M3①-5b, Python's marquee tool and selection commands in genko/app/canvas.py, canvas_shapes.py and main.py)
// on the offscreen platform: the commands with Python's words, keys and tips; a rectangle, a lasso, an ellipse, a
// polyline, auto-select, colour, the selection pen and eraser, with Shift to add, Alt to take away, both for the
// overlap; select all, invert, grow, shrink, soften, from the layer, kept and used again, the quick mask; the chosen
// part moved, scaled, turned, flipped, pulled freely and changed by numbers; copied, cut, pasted, deleted, filled.
#include <QtTest>
#include <QAction>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/selection.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

using Std = QKeySequence::StandardKey;

struct Expected {
    const char* attribute;
    const char* text;
    QList<QKeySequence> keys;
    const char* tip;
    bool checkable;
};

QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// Python's words, keys and tips (main.py _build_actions)
const std::vector<Expected>& expected() {
    static const std::vector<Expected> list = {
        {"act_marquee", "範囲選択（長方形）", {s("M")}, "ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み・縦横比を保つ）", true},
        {"act_lasso", "範囲選択（投げ縄）", {s("L")}, "ドラッグで囲んで選びます", true},
        {"act_wand", "自動選択", {s("W")}, "クリックした所の、線で囲まれた範囲を選びます", true},
        {"act_sel_ellipse", "範囲選択（楕円）", {}, "ドラッグで楕円に選ぶ（Shift で足す、Alt で引く）", true},
        {"act_sel_polyline", "範囲選択（折れ線）", {}, "クリックで角を置き、ダブルクリックか Enter で閉じる", true},
        {"act_sel_colour", "色域選択", {}, "クリックした所と同じ色の所をページ中から選ぶ", true},
        {"act_sel_pen", "選択ペン", {}, "なぞった所を選択範囲に足す", true},
        {"act_sel_erase", "選択消し", {}, "なぞった所を選択範囲から外す", true},
        {"act_select_all", "すべて選択", {QKeySequence(Std::SelectAll)}, "", false},
        {"act_deselect", "選択を解除", {s("Ctrl+D")}, "", false},
        {"act_sel_invert", "選択範囲を反転", {s("Ctrl+Alt+I")}, "", false},
        {"act_sel_grow", "選択範囲を広げる…", {}, "選択範囲の縁を外へ広げる", false},
        {"act_sel_shrink", "選択範囲を狭める…", {}, "選択範囲の縁を内へ狭める", false},
        {"act_sel_feather", "境界をぼかす…", {}, "選択範囲の縁をなめらかにぼかす", false},
        {"act_sel_layer", "描画部分から選択", {}, "描く先のレイヤーで描いてある所を選ぶ", false},
        {"act_sel_keep", "選択範囲をストック…", {}, "名前を付けてページに残す（後で「ストックから選ぶ」）", false},
        {"act_quick_mask", "クイックマスク", {}, "選択範囲を赤で見せ、選択ペン・選択消しで直す", true},
        {"act_copy", "コピー", {QKeySequence(Std::Copy)}, "", false},
        {"act_cut", "切り取り", {QKeySequence(Std::Cut)}, "", false},
        {"act_paste", "貼り付け", {QKeySequence(Std::Paste)}, "新しいレイヤーに貼り付けます（そのまま動かせます）", false},
        {"act_delete_area", "選択範囲を消す", {}, "", false},
        {"act_flip_h", "左右反転", {}, "", false},
        {"act_flip_v", "上下反転", {}, "", false},
        {"act_fill_selection", "選択範囲を塗る", {s("Alt+Backspace")}, "", false},
        {"act_line_width", "選択範囲の線の太さ…", {}, "", false},
        {"act_warp_perspective", "自由変形（遠近・4 隅）", {s("Ctrl+Shift+P")}, "選択範囲の 4 隅を好きな所へ引っぱる。Enter で確定、Esc でやめる", false},
        {"act_warp_mesh", "自由変形（メッシュ・3×3）", {s("Ctrl+Shift+W")}, "選択範囲の 3×3 の点を引っぱって曲げる。Enter で確定、Esc でやめる", false},
        {"act_warp_mesh_grid", "自由変形（メッシュ・格子の数を決める）…", {}, "横と縦の格子の数（1〜8）を決めてから、点を引っぱって曲げる", false},
        {"act_move_pivot", "基準位置を動かす", {},
         "次にクリックした所を、選択範囲を回す・数で変形するときの中心にします（置いた＋はドラッグで動かせる）", false},
        {"act_transform_numbers", "変形を数で決める…", {},
         "選択範囲を、移動（mm）・拡大率（%）・回転（°）の数で変形します。基準位置（選択範囲の中の＋）を中心に", false},
        {"act_warp_apply", "自由変形を確定", {}, "", false},
    };
    return list;
}

// A book with one colour page (60 × 60 mm, one panel), a line on the ink layer from (10, 10) to (30, 30).
core::Document book_doc() {
    auto doc = core::new_episode("範囲選択", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    return core::CommandBus().apply(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                           {"points", Json::array({Json::array({10.0, 10.0, 0.7}), Json::array({30.0, 30.0, 0.7})})},
                                                           {"stabilize", 0}}}), core::Actor("human:tester")).doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::string ink;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        ink = gui_test::ink_of(session->document().page(0))->id;
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers) if (l.id == id) return &l;
        return nullptr;
    }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    std::optional<Json> area() const { return canvas()->selection() ? std::optional<Json>(canvas()->selection()->area) : std::nullopt; }
    // the selection's size (mm², counted at 200 dpi)
    double size() const {
        if (!area()) return 0;
        const auto mask = render::selection::to_mask(*area(), page(), nullptr, 200);
        const std::string bytes = mask.tobytes();
        double inside = 0;
        for (const char v : bytes) inside += static_cast<unsigned char>(v) / 255.0;
        return inside * (25.4 / 200) * (25.4 / 200);
    }
    // the line's points on a layer (its first stroke)
    std::vector<core::PointF> line(const std::string& id) const {
        const auto* l = layer(id);
        return l != nullptr && l->stroke_count() > 0 ? l->strokes->items[0]->points : std::vector<core::PointF>{};
    }
};

}  // namespace

class TestGuiSelect : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    void theSelectionCommands() {
        Studio studio;
        QMenu* menu = nullptr;
        for (QAction* top : studio.window->menuBar()->actions())
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("選択")) menu = top->menu();
        QVERIFY(menu != nullptr);
        for (const Expected& e : expected()) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            if (!e.keys.isEmpty()) QCOMPARE(a->shortcuts().first(), e.keys.first());
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QVERIFY2(menu->actions().contains(a), e.attribute);
        }
        QVERIFY(studio.window->action("act_delete_area")->shortcuts().contains(s("Backspace")));
        // the tools: the marquee in its ways
        for (const auto& [name, way] : {std::pair{"act_marquee", "rect"}, {"act_lasso", "lasso"}, {"act_wand", "wand"}, {"act_sel_ellipse", "ellipse"},
                                        {"act_sel_polyline", "polyline"}, {"act_sel_colour", "color"}, {"act_sel_pen", "pen"}, {"act_sel_erase", "erase"}}) {
            studio.trigger(name);
            QCOMPARE(studio.canvas()->tool(), QStringLiteral("marquee"));
            QCOMPARE(studio.canvas()->marquee, QString::fromLatin1(way));
            QVERIFY(studio.window->action(QString::fromLatin1(name))->isChecked());
        }
    }

    // A rectangle, a lasso, an ellipse, a polyline (Enter closes it); Esc lets it go.
    void drawnShapes() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(10, 12), QPointF(20, 20), QPointF(30, 32)});
        QVERIFY(studio.area());
        const Json poly = (*studio.area())["poly"];
        QCOMPARE(poly.size(), std::size_t(4));
        QVERIFY(std::abs(poly[0][0].get<double>() - 10) < 0.5 && std::abs(poly[2][1].get<double>() - 32) < 0.5);
        QVERIFY2(std::abs(studio.size() - 20 * 20) < 25, qPrintable(QString::number(studio.size())));

        studio.trigger("act_lasso");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(25, 5), QPointF(25, 25), QPointF(5, 25)});
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(4));
        QVERIFY2(std::abs(studio.size() - 400) < 25, qPrintable(QString::number(studio.size())));

        studio.trigger("act_deselect");  // (a press inside the selection moves it, with any of the marquee's ways)
        studio.trigger("act_sel_ellipse");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(20, 20), QPointF(30, 30)});
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(72));
        QVERIFY2(std::abs(studio.size() - 3.14159 * 100) < 20, qPrintable(QString::number(studio.size())));

        studio.trigger("act_sel_polyline");
        for (const QPointF& p : {QPointF(10, 10), QPointF(40, 10), QPointF(10, 40)}) inject::click_mm(canvas, p);
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(3));
        QVERIFY2(std::abs(studio.size() - 450) < 25, qPrintable(QString::number(studio.size())));

        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!studio.area());
    }

    // Shift adds, Alt takes away, both keep the overlap; Ctrl+D lets it go.
    void joined() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        inject::mouse_stroke(canvas, {QPointF(20, 20), QPointF(40, 40)}, Qt::ShiftModifier);
        QVERIFY(studio.area()->contains("mask"));
        QVERIFY2(std::abs(studio.size() - 700) < 30, qPrintable(QString::number(studio.size())));
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(20, 40)}, Qt::AltModifier);
        QVERIFY2(std::abs(studio.size() - 500) < 30, qPrintable(QString::number(studio.size())));
        inject::mouse_stroke(canvas, {QPointF(25, 25), QPointF(35, 35)}, Qt::ShiftModifier | Qt::AltModifier);
        QVERIFY2(std::abs(studio.size() - 100) < 15, qPrintable(QString::number(studio.size())));
        // what is not drawn joins nothing; taking it all away leaves none
        inject::mouse_stroke(canvas, {QPointF(0, 0), QPointF(60, 60)}, Qt::AltModifier);
        QVERIFY(!studio.area());
        QCOMPARE(studio.window->last_notice(), QStringLiteral("選択範囲がなくなりました"));
        studio.trigger("act_select_all");
        QVERIFY(studio.area());
        studio.trigger("act_deselect");
        QVERIFY(!studio.area());
    }

    // すべて選択, 反転, 広げる・狭める・ぼかす (the width asked), 描画部分から選択.
    void changed() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_select");
        studio.trigger("act_select_all");
        QCOMPARE(canvas->tool(), QStringLiteral("marquee"));
        QVERIFY2(std::abs(studio.size() - 3600) < 30, qPrintable(QString::number(studio.size())));
        studio.trigger("act_deselect");  // (a drag inside the selection would move what is in it)
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        studio.trigger("act_sel_invert");
        QVERIFY2(std::abs(studio.size() - 3200) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_sel_invert");
        QVERIFY2(std::abs(studio.size() - 400) < 30, qPrintable(QString::number(studio.size())));
        studio.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(2.0); };
        studio.trigger("act_sel_grow");
        QVERIFY2(std::abs(studio.size() - 24 * 24) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_sel_shrink");
        QVERIFY2(std::abs(studio.size() - 400) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_sel_feather");
        QVERIFY2(std::abs(studio.size() - 400) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_deselect");
        studio.trigger("act_sel_invert");  // (nothing chosen: the page as a whole)
        QVERIFY2(std::abs(studio.size() - 3600) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_deselect");
        studio.trigger("act_sel_grow");  // (nothing chosen: a word)
        QCOMPARE(studio.window->last_notice(), QStringLiteral("先に範囲を選びます（範囲選択 M・投げ縄 L・自動選択 W）"));
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_sel_layer");
        QVERIFY(studio.area() && studio.size() > 5 && studio.size() < 60);
    }

    // 自動選択 inside the panel's border; 色域選択 of the paper; the selection pen adds, the eraser takes away.
    void wandColourAndPen() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_wand");
        inject::click_mm(canvas, QPointF(40, 15));
        QVERIFY(studio.area() && studio.area()->contains("mask"));
        const double panel = studio.size();
        QVERIFY2(panel > 1500 && panel < 2600, qPrintable(QString::number(panel)));  // (the panel, less the line across it)
        studio.trigger("act_deselect");
        studio.trigger("act_sel_colour");
        inject::click_mm(canvas, QPointF(40, 15));
        QVERIFY2(studio.size() > panel, qPrintable(QStringLiteral("%1 %2").arg(studio.size()).arg(panel)));  // (white paper all over the page)
        studio.trigger("act_deselect");
        studio.trigger("act_sel_pen");
        inject::mouse_stroke(canvas, {QPointF(10, 30), QPointF(30, 30)});
        const double pen = studio.size();
        QVERIFY2(std::abs(pen - (20 * 4 + 3.14159 * 4)) < 15, qPrintable(QString::number(pen)));
        studio.trigger("act_sel_erase");
        inject::mouse_stroke(canvas, {QPointF(20, 25), QPointF(20, 35)});
        QVERIFY(studio.size() < pen - 10);
    }

    // The chosen part moved by a drag inside it, scaled by a corner, turned by the ○: one transform_area each, the
    // selection going with it.
    void movedByHandles() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(35, 35)});
        const auto before = studio.line(studio.ink);
        inject::mouse_stroke(canvas, {QPointF(20, 20), QPointF(25, 22), QPointF(30, 25)});
        const auto after = studio.line(studio.ink);
        QCOMPARE(after.size(), before.size());
        QVERIFY(std::abs(after.front().x - before.front().x - 10) < 0.05 && std::abs(after.front().y - before.front().y - 5) < 0.05);
        QVERIFY(std::abs(canvas->selection_box()[0] - 15) < 0.05);
        // the south-east corner pulled: twice as large from the north-west corner
        const auto [x0, y0, x1, y1] = canvas->selection_box();
        inject::mouse_stroke(canvas, {QPointF(x1, y1), QPointF(x1 + 15, y1 + 15), QPointF(x1 + 30, y1 + 30)});
        const auto scaled = studio.line(studio.ink);
        QVERIFY(std::abs((scaled.back().x - scaled.front().x) - 2 * (after.back().x - after.front().x)) < 0.1);
        QVERIFY(std::abs(canvas->selection_box()[2] - (x1 + 30)) < 0.05);
        // Escape: the selection goes, the book stays
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!studio.area());
        QCOMPARE(studio.line(studio.ink).size(), scaled.size());
    }

    // 左右反転, 自由変形 (the corners pulled, Enter), 変形を数で決める (about the pivot).
    void flippedWarpedAndNumbers() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(35, 35)});
        const auto before = studio.line(studio.ink);
        studio.trigger("act_flip_h");
        const auto flipped = studio.line(studio.ink);
        QVERIFY(std::abs(flipped.front().x - (40 - before.front().x)) < 0.05 && std::abs(flipped.front().y - before.front().y) < 0.05);

        studio.trigger("act_warp_perspective");
        QVERIFY(canvas->warping());
        inject::mouse_stroke(canvas, {QPointF(35, 35), QPointF(40, 40), QPointF(45, 45)});  // the bottom-right corner
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!canvas->warping());
        QVERIFY(!studio.area());  // (a free transform lets the selection go)
        QVERIFY(studio.line(studio.ink) != flipped);

        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(45, 45)});
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("transform_numbers_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("transform_dx"))->setValue(5);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("transform_dy"))->setValue(-2);
            return int(QDialog::Accepted);
        };
        const auto warped = studio.line(studio.ink);
        studio.trigger("act_transform_numbers");
        const auto moved = studio.line(studio.ink);
        QVERIFY(std::abs(moved.front().x - warped.front().x - 5) < 0.05 && std::abs(moved.front().y - warped.front().y + 2) < 0.05);
        const auto m = app::transform_matrix(QPointF(10, 10), 0, 0, 2, 2, 90);  // (Python's transform_matrix)
        QVERIFY(std::abs(m[0]) < 1e-12 && std::abs(m[1] - 2) < 1e-12 && std::abs(m[2] + 2) < 1e-12 && std::abs(m[4] - 30) < 1e-9 && std::abs(m[5] + 10) < 1e-9);
    }

    // コピー・切り取り・貼り付け (a new layer 貼り付け, the selection where it was), 消す, 塗る, 線の太さ.
    void copyCutPasteAndMore() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_paste");
        QCOMPARE(studio.window->last_notice(), QStringLiteral("貼り付けるものがありません（先にコピー）"));
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(35, 35)});
        studio.trigger("act_copy");
        const auto count = studio.page().layers.size();
        studio.trigger("act_paste");
        QCOMPARE(studio.page().layers.size(), count + 1);
        const core::Layer* pasted = studio.window->target_layer();
        QCOMPARE(pasted->title, std::string("貼り付け"));
        QCOMPARE(pasted->stroke_count(), std::size_t(1));
        QVERIFY(studio.area());
        // cut from the ink: gone there
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_cut");
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(0));
        studio.trigger("act_undo");
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(1));
        // the line's width in the area
        studio.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(1.2); };
        studio.trigger("act_line_width");
        QCOMPARE(studio.layer(studio.ink)->strokes->items[0]->width_mm, 1.2);
        studio.trigger("act_delete_area");
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(0));
        studio.trigger("act_fill_selection");
        QVERIFY(!studio.layer(studio.ink)->patches.empty());
        // nothing in the area: a word
        studio.trigger("act_deselect");
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(45, 45), QPointF(55, 55)});
        studio.window->set_target_layer(pasted->id);
        studio.trigger("act_copy");
        QCOMPARE(studio.window->last_notice(), QStringLiteral("選んだ範囲に、このレイヤーの絵がありません"));
    }

    // ストック: kept with a name, chosen again from the menu, forgotten; クイックマスク.
    void keptAndQuickMask() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        studio.answers.responder->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(QStringLiteral(" 空 ")); };
        studio.trigger("act_sel_keep");
        QVERIFY(studio.page().extra["saved_areas"].contains("空"));
        studio.trigger("act_deselect");
        QMenu* stock = nullptr;
        for (QMenu* m : studio.window->findChildren<QMenu*>()) if (m->title() == QStringLiteral("ストックから選ぶ")) stock = m;
        QVERIFY(stock != nullptr);
        emit stock->aboutToShow();
        QAction* use = nullptr;
        for (QAction* a : stock->actions()) if (a->text() == QStringLiteral("空")) use = a;
        QVERIFY(use != nullptr);
        use->trigger();
        QVERIFY2(std::abs(studio.size() - 400) < 30, qPrintable(QString::number(studio.size())));
        emit stock->aboutToShow();
        QMenu* forget = nullptr;
        for (QAction* a : stock->actions()) if (a->menu() != nullptr) forget = a->menu();
        QVERIFY(forget != nullptr);
        forget->actions().first()->trigger();
        QVERIFY(!studio.page().extra.contains("saved_areas") || !studio.page().extra["saved_areas"].contains("空"));

        studio.window->action("act_quick_mask")->setChecked(true);
        QVERIFY(canvas->quick_mask);
        QCOMPARE(canvas->marquee, QStringLiteral("pen"));
        QVERIFY(studio.window->action("act_sel_pen")->isChecked());
        canvas->repaint();  // (the red over what is not chosen is drawn)
        studio.window->action("act_quick_mask")->setChecked(false);
        QVERIFY(!canvas->quick_mask);
    }

    // Alt held with the marquee takes away: it does not switch the tool.
    void altStaysWithTheMarquee() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_lasso");
        QTest::keyPress(canvas, Qt::Key_Alt, Qt::AltModifier);
        QCOMPARE(canvas->tool(), QStringLiteral("marquee"));
        QTest::keyRelease(canvas, Qt::Key_Alt, Qt::NoModifier);
        QTest::keyPress(canvas, Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(canvas->tool(), QStringLiteral("select"));
        QTest::keyRelease(canvas, Qt::Key_Control, Qt::NoModifier);
        QCOMPARE(canvas->tool(), QStringLiteral("marquee"));
        QVERIFY(studio.window->action("act_lasso")->isChecked());
    }
};

QTEST_MAIN(TestGuiSelect)
#include "test_gui_select.moc"
