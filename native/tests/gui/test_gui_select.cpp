// 選択範囲 (M3①-5b: the marquee tool and the selection's commands; PC3-0: the user's decision 5, Photoshop's names)
// on the offscreen platform: the commands with their words, keys, tips and places; a rectangle, a lasso, an ellipse, a
// polyline, auto-select by its two criteria, the selection pen and eraser, with Shift to add, Alt to take away, both for
// the overlap; select all, invert, expand, contract, soften, from the layer, kept and used again; the chosen part moved,
// scaled, turned, flipped, pulled freely and changed by numbers; copied, cut, pasted, deleted, filled. PC3 B1: the tools'
// settings (選択方法, アンチエイリアス on by default, ぼかし, スタイル), 境界線・滑らかに, 選択範囲を拡大・近似色を選択, 再選択,
// 選択範囲を変形, 選択範囲の境界線, 色域指定…, 選択範囲を保存…・読み込む…, the Quick Mask as a channel the tools paint,
// マジック消しゴムツール and the selection's right-click menu.
#include <QtTest>
#include <QAction>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSpinBox>
#include <QToolButton>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/masks.hpp"
#include "core/pyconv.hpp"
#include "render/png.hpp"
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
    const char* place;  // (the menu that holds it, by its path)
};

QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// The words, keys, tips and places (the user's decision 5: Photoshop's names — the tools in ツール, the selection's
// commands in 選択範囲, the clipboard's and the transform's in 編集; Genko's own keep their words)
const std::vector<Expected>& expected() {
    static const std::vector<Expected> list = {
        {"act_marquee", "長方形選択ツール", {s("M")}, "ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み・縦横比を保つ）", true, "ツール"},
        {"act_lasso", "なげなわツール", {s("L")}, "ドラッグで囲んで選びます", true, "ツール"},
        {"act_wand", "自動選択ツール", {s("W")}, "クリックした所を選びます（基準はツールの設定: 線で囲まれた所・色の近さ）", true, "ツール"},
        {"act_sel_ellipse", "楕円形選択ツール", {}, "ドラッグで楕円に選ぶ（Shift で足す、Alt で引く）", true, "ツール"},
        {"act_sel_polyline", "多角形選択ツール", {}, "クリックで角を置き、ダブルクリックか Enter で閉じる", true, "ツール"},
        {"act_sel_pen", "選択ペン", {}, "なぞった所を選択範囲に足す", true, "ツール"},
        {"act_sel_erase", "選択消し", {}, "なぞった所を選択範囲から外す", true, "ツール"},
        {"act_magic_eraser", "マジック消しゴムツール", {}, "クリックした所に近い色の所を透明にします（描く先のペイントのレイヤー）", true, "ツール"},
        {"act_select_all", "すべてを選択", {QKeySequence(Std::SelectAll)}, "", false, "選択範囲"},
        {"act_deselect", "選択を解除", {s("Ctrl+D")}, "", false, "選択範囲"},
        {"act_sel_invert", "選択範囲を反転", {s("Ctrl+Alt+I")}, "", false, "選択範囲"},
        {"act_sel_reselect", "再選択", {s("Ctrl+Shift+D")}, "最後に解除した選択範囲を戻す", false, "選択範囲"},
        {"act_color_range", "色域指定…", {}, "色（スポイトで拾う）・色の系統・明るさ・色域外で選ぶ", false, "選択範囲"},
        {"act_sel_border", "境界線…", {}, "選択範囲の縁を、その幅の帯にする", false, "選択範囲/選択範囲を変更"},
        {"act_sel_smooth", "滑らかに…", {}, "選択範囲の縁のでこぼこをならす", false, "選択範囲/選択範囲を変更"},
        {"act_sel_grow_colour", "選択範囲を拡大", {}, "選択範囲の色に近い、つながった所まで広げる（許容値は自動選択ツールの設定）", false, "選択範囲"},
        {"act_sel_similar", "近似色を選択", {}, "選択範囲の色に近い所を、ページ中から選ぶ（許容値は自動選択ツールの設定）", false, "選択範囲"},
        {"act_sel_transform", "選択範囲を変形", {}, "枠のつまみで選択範囲の形だけを動かす・拡大縮小・回転する（中身は動かない）。Enter で終わる", false,
         "選択範囲"},
        {"act_sel_load", "選択範囲を読み込む…", {}, "チャンネル・色の版・透明部分・マスクから選択範囲を作る（反転・追加・削除・共通範囲も）", false, "選択範囲"},
        {"act_sel_edges", "選択範囲の境界線", {}, "選択範囲の点線を表示する・隠す", true, "表示"},
        {"act_sel_grow", "拡張…", {}, "選択範囲の縁を外へ広げる", false, "選択範囲/選択範囲を変更"},
        {"act_sel_shrink", "縮小…", {}, "選択範囲の縁を内へ狭める", false, "選択範囲/選択範囲を変更"},
        {"act_sel_feather", "境界をぼかす…", {s("Shift+F6")}, "選択範囲の縁をなめらかにぼかす", false, "選択範囲/選択範囲を変更"},
        {"act_sel_layer", "ピクセルを選択", {}, "描く先のレイヤーで描いてある所を選ぶ", false, "選択範囲"},
        {"act_sel_keep", "選択範囲をストック…", {}, "名前を付けてページに残す（後で「ストックから選ぶ」）", false, "選択範囲"},
        {"act_quick_mask", "クイックマスクモードで編集", {}, "選択範囲を赤で見せ、ブラシなどの道具で直す", true, "選択範囲"},
        {"act_copy", "コピー", {QKeySequence(Std::Copy)}, "", false, "編集"},
        {"act_cut", "カット", {QKeySequence(Std::Cut)}, "", false, "編集"},
        {"act_paste", "ペースト", {QKeySequence(Std::Paste)}, "新しいレイヤーにペーストします（そのまま動かせます）", false, "編集"},
        {"act_delete_area", "消去", {}, "", false, "編集"},
        {"act_flip_h", "水平方向に反転", {}, "", false, "編集/変形"},
        {"act_flip_v", "垂直方向に反転", {}, "", false, "編集/変形"},
        {"act_fill_selection", "描画色で塗りつぶし", {s("Alt+Backspace")}, "", false, "編集"},
        {"act_line_width", "選択範囲の線の太さ…", {}, "", false, "選択範囲"},
        {"act_warp_perspective", "自由な形に", {s("Ctrl+Shift+P")}, "選択範囲（無ければ選んだレイヤーの枠）の 4 隅を好きな所へ引っぱる。Enter で確定、Esc でやめる",
         false, "編集/変形"},
        {"act_warp_mesh", "ワープ", {s("Ctrl+Shift+W")}, "選択範囲の 3×3 の点（無ければ選んだレイヤーのワープの枠）を引っぱって曲げる。Enter で確定、Esc でやめる",
         false, "編集/変形"},
        {"act_warp_mesh_grid", "ワープ（格子の数を決める）…", {}, "横と縦の格子の数（1〜8）を決めてから、点を引っぱって曲げる", false, "編集/変形"},
        {"act_move_pivot", "基準位置を動かす", {},
         "次にクリックした所を、選択範囲を回す・数で変形するときの中心にします（置いた＋はドラッグで動かせる）", false, "編集/変形"},
        {"act_transform_numbers", "変形を数で決める…", {},
         "選択範囲を、移動（mm）・拡大率（%）・回転（°）の数で変形します。基準位置（選択範囲の中の＋）を中心に", false, "編集/変形"},
        {"act_warp_apply", "変形を確定", {}, "", false, "編集/変形"},
    };
    return list;
}

// A menu of the menu bar by its path ("編集/変形").
QMenu* menu_at(const app::MainWindow* window, const QString& path) {
    QMenu* menu = nullptr;
    QList<QAction*> actions = window->menuBar()->actions();
    for (const QString& part : path.split(QLatin1Char('/'))) {
        QMenu* found = nullptr;
        for (QAction* a : actions)
            if (a->menu() != nullptr && a->menu()->title() == part) found = a->menu();
        if (found == nullptr) return nullptr;
        menu = found;
        actions = menu->actions();
    }
    return menu;
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
        QVERIFY(menu_at(studio.window.get(), QStringLiteral("選択範囲")) != nullptr);
        for (const Expected& e : expected()) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            if (!e.keys.isEmpty()) QCOMPARE(a->shortcuts().first(), e.keys.first());
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QMenu* menu = menu_at(studio.window.get(), QString::fromUtf8(e.place));
            QVERIFY2(menu != nullptr && menu->actions().contains(a), e.attribute);
        }
        // (すべてを選択 and 選択を解除 in 編集 too)
        for (const char* name : {"act_select_all", "act_deselect"})
            QVERIFY2(menu_at(studio.window.get(), QStringLiteral("編集"))->actions().contains(studio.window->action(QString::fromLatin1(name))), name);
        QVERIFY(studio.window->action("act_delete_area")->shortcuts().contains(s("Backspace")));
        // the tools: the marquee in its ways
        for (const auto& [name, way] : {std::pair{"act_marquee", "rect"}, {"act_lasso", "lasso"}, {"act_wand", "wand"}, {"act_sel_ellipse", "ellipse"},
                                        {"act_sel_polyline", "polyline"}, {"act_sel_pen", "pen"}, {"act_sel_erase", "erase"}}) {
            studio.trigger(name);
            QCOMPARE(studio.canvas()->tool(), QStringLiteral("marquee"));
            QCOMPARE(studio.canvas()->marquee, QString::fromLatin1(way));
            QVERIFY(studio.window->action(QString::fromLatin1(name))->isChecked());
        }
        studio.trigger("act_magic_eraser");  // (its own tool, beside the selection's)
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("magiceraser"));
        QCOMPARE(studio.window->tool_settings()->current_page(), studio.window->tool_settings()->page_for(QStringLiteral("magiceraser")));
        QVERIFY(studio.window->action("act_sel_edges")->isChecked());
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
        QVERIFY(!studio.area()->contains("antialias"));  // (a rectangle is never anti-aliased)
        QVERIFY(std::abs(poly[0][0].get<double>() - 10) < 0.5 && std::abs(poly[2][1].get<double>() - 32) < 0.5);
        QVERIFY2(std::abs(studio.size() - 20 * 20) < 25, qPrintable(QString::number(studio.size())));

        studio.trigger("act_lasso");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(25, 5), QPointF(25, 25), QPointF(5, 25)});
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(4));
        QCOMPARE((*studio.area())["antialias"], Json(true));  // (なげなわ: anti-aliased by default, the user's decision 5)
        QVERIFY2(std::abs(studio.size() - 400) < 25, qPrintable(QString::number(studio.size())));

        studio.trigger("act_deselect");  // (a press inside the selection moves it, with any of the marquee's ways)
        studio.trigger("act_sel_ellipse");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(20, 20), QPointF(30, 30)});
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(72));
        QCOMPARE((*studio.area())["antialias"], Json(true));
        QVERIFY2(std::abs(studio.size() - 3.14159 * 100) < 20, qPrintable(QString::number(studio.size())));

        studio.trigger("act_sel_polyline");
        for (const QPointF& p : {QPointF(10, 10), QPointF(40, 10), QPointF(10, 40)}) inject::click_mm(canvas, p);
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE((*studio.area())["poly"].size(), std::size_t(3));
        QCOMPARE((*studio.area())["antialias"], Json(true));
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

    // すべてを選択, 反転, 拡張・縮小 (rounded by the distance transform)・境界をぼかす (the width asked), ピクセルを選択.
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
        QCOMPARE(studio.window->last_notice(), QStringLiteral("先に選択範囲を作ります（長方形選択ツール M・なげなわツール L・自動選択ツール W）"));
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_sel_layer");
        QVERIFY(studio.area() && studio.size() > 5 && studio.size() < 60);
    }

    // 自動選択ツール inside the panel's border (線で囲まれた所), of the paper's colour (色の近さ); the selection pen adds, the
    // eraser takes away.
    void wandColourAndPen() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_wand");
        inject::click_mm(canvas, QPointF(40, 15));
        QVERIFY(studio.area() && studio.area()->contains("mask"));
        const double panel = studio.size();
        QVERIFY2(panel > 1500 && panel < 2600, qPrintable(QString::number(panel)));  // (the panel, less the line across it)
        studio.trigger("act_deselect");
        auto* criterion = studio.window->findChild<QComboBox*>(QStringLiteral("wand_criterion"));
        criterion->setCurrentIndex(criterion->findData(QStringLiteral("colour")));
        QCOMPARE(studio.window->wand_criterion, QStringLiteral("colour"));
        studio.trigger("act_wand");
        inject::click_mm(canvas, QPointF(40, 15));
        QVERIFY2(studio.size() > panel, qPrintable(QStringLiteral("%1 %2").arg(studio.size()).arg(panel)));  // (white paper all over the page)
        criterion->setCurrentIndex(criterion->findData(QStringLiteral("lines")));
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
        QCOMPARE(studio.window->last_notice(), QStringLiteral("ペーストするものがありません（先にコピー）"));
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

        // クイックマスクモードで編集 (masks@1): the selection becomes the page's Quick Mask channel, which the tools paint (white
        // selects); out of it, the selection it says; each a step 取り消し takes back
        studio.trigger("act_marquee");
        studio.trigger("act_deselect");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        studio.window->action("act_quick_mask")->setChecked(true);
        QVERIFY(canvas->quick_mask);
        const core::Channel* quick = core::quick_mask_of(studio.page());
        QVERIFY(quick != nullptr);
        const std::string quick_id = quick->id;
        QVERIFY(!studio.area());
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));  // (a brush in hand)
        QCOMPARE(studio.window->channel_view().target, quick_id);
        QVERIFY(studio.window->channel_view().shown.contains(quick_id));
        canvas->repaint();  // (the mask laid over the page in red)
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", studio.ink},
                                                           {"area", Json{{"rect", Json::array({40, 40, 10, 10})}}}, {"rgb", Json::array({255, 255, 255})}}})));
        QVERIFY(studio.layer(studio.ink)->patches.empty());  // (painted on the channel, not the layer)
        // (an op that only changes a layer's pixels — 境界線を描く, the retouching brushes, …: refused, the layer as it was)
        QVERIFY(!studio.window->apply_ops(Json::array({Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", studio.ink},
                                                            {"area", Json{{"rect", Json::array({40, 40, 10, 10})}}}, {"width_mm", 1.0}}})));
        QVERIFY(studio.layer(studio.ink)->patches.empty());
        QVERIFY2(studio.window->last_notice().contains(QStringLiteral("クイックマスク")), qPrintable(studio.window->last_notice()));
        studio.window->action("act_quick_mask")->setChecked(false);
        QVERIFY(!canvas->quick_mask);
        QVERIFY(core::quick_mask_of(studio.page()) == nullptr);
        QVERIFY(studio.window->channel_view().target.empty());
        QVERIFY2(std::abs(studio.size() - 500) < 40, qPrintable(QString::number(studio.size())));
        studio.trigger("act_undo");  // (back in Quick Mask)
        QVERIFY(core::quick_mask_of(studio.page()) != nullptr);
        QVERIFY(studio.window->action("act_quick_mask")->isChecked());
        QVERIFY(canvas->quick_mask);
        QCOMPARE(studio.window->channel_view().target, quick_id);
    }

    // channels@1 (SPEC.md COLOR-03): 選択範囲を保存… (its name asked; none: Alpha n) and 選択範囲を読み込む
    // (the page's channels, remade each time): the same area back; with nothing chosen, said.
    void savedToAndLoadedFromAChannel() {
        Studio studio;
        auto* canvas = studio.canvas();
        QMenu* menu = nullptr;
        for (QAction* top : studio.window->menuBar()->actions())
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("選択範囲")) menu = top->menu();
        QVERIFY(menu != nullptr);
        QAction* save = studio.window->action("act_sel_to_channel");
        QVERIFY(save != nullptr && menu->actions().contains(save));
        QCOMPARE(save->text(), QStringLiteral("選択範囲を保存…"));
        QAction* load = studio.window->action("act_sel_load");
        QVERIFY(load != nullptr && menu->actions().contains(load));
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        const double chosen = studio.size();
        QVERIFY2(std::abs(chosen - 400) < 30, qPrintable(QString::number(chosen)));
        // a new channel, named; (none: Alpha n)
        QString name = QStringLiteral(" 空 ");
        QString operation_seen;
        studio.answers.responder->exec = [&name, &operation_seen](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("save_selection_dialog")) return int(QDialog::Rejected);
            operation_seen = dialog->findChild<QComboBox*>(QStringLiteral("save_operation"))->currentData().toString();
            dialog->findChild<QLineEdit*>(QStringLiteral("save_name"))->setText(name);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_sel_to_channel");
        QCOMPARE(operation_seen, QStringLiteral("new"));
        QCOMPARE(studio.page().channels.size(), std::size_t{1});
        QCOMPARE(studio.page().channels[0].name, std::string("空"));
        QVERIFY(studio.page().channels[0].kind == core::ChannelKind::Alpha);
        name.clear();
        studio.trigger("act_sel_to_channel");
        QCOMPARE(studio.page().channels.back().name, std::string("Alpha 1"));
        // into the channel the page has: added to (another square)
        studio.trigger("act_deselect");
        inject::mouse_stroke(canvas, {QPointF(40, 40), QPointF(50, 50)});
        bool named = true;
        studio.answers.responder->exec = [&named](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("save_selection_dialog")) return int(QDialog::Rejected);
            auto* channel = dialog->findChild<QComboBox*>(QStringLiteral("save_channel"));
            channel->setCurrentIndex(channel->findText(QStringLiteral("空")));
            named = dialog->findChild<QLineEdit*>(QStringLiteral("save_name"))->isEnabled();
            auto* operation = dialog->findChild<QComboBox*>(QStringLiteral("save_operation"));
            operation->setCurrentIndex(operation->findData(QStringLiteral("add")));
            return int(QDialog::Accepted);
        };
        studio.trigger("act_sel_to_channel");
        QVERIFY(!named);  // (a channel the page has keeps its name)
        QCOMPARE(studio.page().channels.size(), std::size_t{2});
        // read back, inverted, joined
        studio.trigger("act_deselect");
        QVERIFY(!studio.area());
        bool inverted = false;
        QString how = QStringLiteral("replace");
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("load_selection_dialog")) return int(QDialog::Rejected);
            auto* source = dialog->findChild<QComboBox*>(QStringLiteral("load_source"));
            source->setCurrentIndex(source->findText(QStringLiteral("空")));
            dialog->findChild<QCheckBox*>(QStringLiteral("load_invert"))->setChecked(inverted);
            auto* operation = dialog->findChild<QComboBox*>(QStringLiteral("load_operation"));
            operation->setCurrentIndex(operation->findData(how));
            return int(QDialog::Accepted);
        };
        studio.trigger("act_sel_load");
        QVERIFY2(std::abs(studio.size() - (chosen + 100)) < 30, qPrintable(QString::number(studio.size())));
        inverted = true;
        studio.trigger("act_sel_load");
        QVERIFY2(std::abs(studio.size() - (3600 - chosen - 100)) < 40, qPrintable(QString::number(studio.size())));
        inverted = false;
        how = QStringLiteral("add");  // (the inverse and the channel: all of the page)
        studio.trigger("act_sel_load");
        QVERIFY2(std::abs(studio.size() - 3600) < 40, qPrintable(QString::number(studio.size())));
        // a colour plate (the page's red: the paper and more) and the drawing layer's transparency are there too
        studio.window->set_target_layer(studio.ink);
        QStringList offered;
        studio.answers.responder->exec = [&offered](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("load_selection_dialog")) return int(QDialog::Rejected);
            auto* source = dialog->findChild<QComboBox*>(QStringLiteral("load_source"));
            for (int i = 0; i < source->count(); ++i) offered << source->itemText(i);
            source->setCurrentIndex(source->count() - 1);  // (the drawing layer's transparency: its line)
            return int(QDialog::Accepted);
        };
        studio.trigger("act_deselect");
        studio.trigger("act_sel_load");
        QVERIFY2(offered.contains(QStringLiteral("レッド")) && offered.contains(QStringLiteral("合成（明るさ）")), qPrintable(offered.join(QLatin1Char(','))));
        QVERIFY(offered.last().endsWith(QStringLiteral("の透明部分")));
        QVERIFY(studio.area() && studio.size() > 5 && studio.size() < 60);
        // nothing chosen: nothing kept
        studio.trigger("act_deselect");
        studio.answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
        studio.trigger("act_sel_to_channel");
        QCOMPARE(studio.page().channels.size(), std::size_t{2});
    }

    // ツールの設定: 選択方法 (a new area added without Shift), アンチエイリアス (off: none), ぼかし (the area feathered), スタイル
    // (縦横比を固定, 固定).
    void theToolsSettings() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_marquee");
        auto* add = studio.window->findChild<QToolButton*>(QStringLiteral("select_method_add"));
        QVERIFY(add != nullptr);
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        add->click();
        QCOMPARE(canvas->select_method, QStringLiteral("add"));
        inject::mouse_stroke(canvas, {QPointF(20, 20), QPointF(40, 40)});
        QVERIFY2(std::abs(studio.size() - 700) < 30, qPrintable(QString::number(studio.size())));
        studio.window->findChild<QToolButton*>(QStringLiteral("select_method_replace"))->click();
        studio.trigger("act_deselect");
        // anti-aliasing off, a feather
        auto* antialias = studio.window->findChild<QCheckBox*>(QStringLiteral("select_antialias"));
        QVERIFY(antialias->isChecked());
        antialias->setChecked(false);
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("select_feather"))->setValue(1.5);
        studio.trigger("act_lasso");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(25, 5), QPointF(25, 25), QPointF(5, 25)});
        QVERIFY(!studio.area()->contains("antialias"));
        QCOMPARE((*studio.area())["feather_mm"], Json(1.5));
        QVERIFY2(std::abs(studio.size() - 400) < 30, qPrintable(QString::number(studio.size())));
        antialias->setChecked(true);
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("select_feather"))->setValue(0);
        studio.trigger("act_deselect");
        // the style: a ratio of 2 : 1, a fixed size (a click is enough)
        studio.trigger("act_marquee");
        auto* style = studio.window->findChild<QComboBox*>(QStringLiteral("marquee_style"));
        style->setCurrentIndex(style->findData(QStringLiteral("ratio")));
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("marquee_width"))->setValue(2);
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("marquee_height"))->setValue(1);
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(20, 15), QPointF(30, 12)});
        auto box = canvas->selection_box();
        QVERIFY(std::abs((box[2] - box[0]) - 2 * (box[3] - box[1])) < 0.05);
        studio.trigger("act_deselect");
        style->setCurrentIndex(style->findData(QStringLiteral("fixed")));
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("marquee_width"))->setValue(12);
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("marquee_height"))->setValue(6);
        inject::click_mm(canvas, QPointF(10, 40));
        box = canvas->selection_box();
        QVERIFY(std::abs((box[2] - box[0]) - 12) < 0.05 && std::abs((box[3] - box[1]) - 6) < 0.05);
        style->setCurrentIndex(style->findData(QStringLiteral("normal")));
    }

    // 境界線・滑らかに, 選択範囲を拡大・近似色を選択 (the wand's tolerance), 再選択 (the selection let go, on its page),
    // 選択範囲を変形 (the outline alone, the book as it is; Enter ends it), 選択範囲の境界線 (hidden, shown).
    void modifyGrowReselectTransform() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(10, 10), QPointF(30, 30)});
        studio.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(2.0); };
        studio.trigger("act_sel_border");
        QVERIFY2(std::abs(studio.size() - 160) < 30, qPrintable(QString::number(studio.size())));  // (a band 2 mm wide along the edge)
        studio.trigger("act_deselect");
        studio.trigger("act_sel_reselect");  // (the band back)
        QVERIFY2(std::abs(studio.size() - 160) < 30, qPrintable(QString::number(studio.size())));
        studio.trigger("act_sel_smooth");
        QVERIFY(studio.area());
        // grow: the white joined to a small square inside the panel; similar: the white anywhere
        studio.trigger("act_deselect");
        inject::mouse_stroke(canvas, {QPointF(40, 12), QPointF(42, 14)});
        const double small = studio.size();
        studio.trigger("act_sel_grow_colour");
        const double grown = studio.size();
        QVERIFY2(grown > small * 20, qPrintable(QStringLiteral("%1 %2").arg(small).arg(grown)));
        studio.trigger("act_sel_similar");
        QVERIFY2(studio.size() >= grown - 1, qPrintable(QStringLiteral("%1 %2").arg(grown).arg(studio.size())));
        // 選択範囲を変形: a drag inside moves the outline, not the line
        studio.trigger("act_deselect");
        studio.window->set_target_layer(studio.ink);
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(35, 35)});
        const auto before = studio.line(studio.ink);
        studio.trigger("act_sel_transform");
        QVERIFY(canvas->transform_outline);
        inject::mouse_stroke(canvas, {QPointF(20, 20), QPointF(25, 22), QPointF(30, 25)});
        QCOMPARE(studio.line(studio.ink), before);
        QVERIFY(std::abs(canvas->selection_box()[0] - 15) < 0.05);
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!canvas->transform_outline);
        QVERIFY(studio.area());
        // the edges hidden and shown
        studio.window->action("act_sel_edges")->setChecked(false);
        QVERIFY(!canvas->show_selection_edges);
        canvas->repaint();
        studio.window->action("act_sel_edges")->setChecked(true);
        QVERIFY(canvas->show_selection_edges);
        // another page: nothing to reselect there
        studio.trigger("act_deselect");
        QVERIFY(canvas->previous_selection().has_value());
    }

    // 色域指定…: the highlights (the paper), the shadows (the line), the colour picked on its picture with the eyedropper.
    void colorRangeWindow() {
        Studio studio;
        QString select = QStringLiteral("highlights");
        bool pick = false;
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("color_range_dialog")) return int(QDialog::Rejected);
            auto* which = dialog->findChild<QComboBox*>(QStringLiteral("range_select"));
            which->setCurrentIndex(which->findData(select));
            if (pick) {  // (a click on the picture at the paper's (40, 15) mm)
                auto* picture = dialog->findChild<QLabel*>(QStringLiteral("range_preview"));
                const QPixmap shown = picture->pixmap();
                const QPointF at(40.0 / 60 * shown.width(), 15.0 / 60 * shown.height());
                QMouseEvent press(QEvent::MouseButtonPress, at, picture->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(picture, &press);
            }
            return int(QDialog::Accepted);
        };
        studio.trigger("act_color_range");
        QVERIFY(studio.area());
        QVERIFY2(studio.size() > 3000, qPrintable(QString::number(studio.size())));
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("marquee"));
        select = QStringLiteral("shadows");
        studio.trigger("act_color_range");
        QVERIFY2(studio.size() > 3 && studio.size() < 600, qPrintable(QString::number(studio.size())));
        select = QStringLiteral("sampled");
        pick = true;
        studio.trigger("act_color_range");
        QVERIFY2(studio.size() > 3000, qPrintable(QString::number(studio.size())));
    }

    // マジック消しゴムツール: the paint layer's colour around the click made clear (one step), the settings sent with it.
    void magicEraser() {
        Studio studio;
        const std::string white(std::size_t(40) * 40 * 4, '\xff');
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{40, 40}, white);
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                                                      Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", core::b64encode(render::write_png(picture))}}})));
        studio.window->set_target_layer("paint");
        studio.trigger("act_magic_eraser");
        studio.window->findChild<QSpinBox*>(QStringLiteral("magic_opacity"))->setValue(50);
        QCOMPARE(studio.window->magic_eraser_settings.opacity, 50.0);
        inject::click_mm(studio.canvas(), QPointF(30, 30));
        const render::Image after = render::open_image(*studio.layer("paint")->raster_png).convert("RGBA");
        const auto px = after.getpixel(after.width() / 2, after.height() / 2);
        QVERIFY2(std::abs(px[3] - 128) <= 1, qPrintable(QString::number(px[3])));
        studio.trigger("act_undo");
        QCOMPARE(render::open_image(*studio.layer("paint")->raster_png).convert("RGBA").getpixel(after.width() / 2, after.height() / 2)[3], 255.0);
    }

    // The selection's right-click menu (Photoshop's): over the selection with a selection tool; not elsewhere.
    void rightClickOnTheSelection() {
        Studio studio;
        studio.trigger("act_marquee");
        inject::mouse_stroke(studio.canvas(), {QPointF(10, 10), QPointF(30, 30)});
        QMenu menu;
        QVERIFY(studio.window->fill_selection_menu(menu, QPointF(20, 20)));
        QStringList words;
        for (QAction* a : menu.actions())
            if (!a->isSeparator()) words << a->text();
        for (const char* word : {"選択を解除", "再選択", "選択範囲を反転", "境界をぼかす…", "選択範囲を保存…", "選択範囲を変形", "描画色で塗りつぶし"})
            QVERIFY2(words.contains(QString::fromUtf8(word)), word);
        QMenu outside;
        QVERIFY(!studio.window->fill_selection_menu(outside, QPointF(50, 50)));
        QVERIFY(outside.actions().isEmpty());
        studio.trigger("act_pen");
        QVERIFY(!studio.window->fill_selection_menu(outside, QPointF(20, 20)));
    }

    // Keys while a drag goes on: Esc (or Enter refused, on a locked layer) while a free transform's point is held
    // stops it, and the point let go afterwards is nothing; Ctrl held in the middle of a lasso does not switch the
    // tool under it.
    void keysDuringADrag() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_marquee");
        inject::mouse_stroke(canvas, {QPointF(5, 5), QPointF(35, 35)});
        const auto before = studio.line(studio.ink);
        studio.trigger("act_warp_perspective");
        inject::mouse(canvas, inject::Phase::Press, QPointF(35, 35));
        inject::mouse(canvas, inject::Phase::Move, QPointF(38, 38));
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!canvas->warping());
        inject::mouse(canvas, inject::Phase::Move, QPointF(40, 40));
        inject::mouse(canvas, inject::Phase::Release, QPointF(40, 40));
        QVERIFY(studio.area());
        QCOMPARE(studio.line(studio.ink).size(), before.size());

        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", studio.ink}, {"locked", true}}})));
        studio.trigger("act_warp_perspective");
        inject::mouse(canvas, inject::Phase::Press, QPointF(35, 35));
        inject::mouse(canvas, inject::Phase::Move, QPointF(38, 38));
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!canvas->warping());
        inject::mouse(canvas, inject::Phase::Move, QPointF(40, 40));
        inject::mouse(canvas, inject::Phase::Release, QPointF(40, 40));
        QCOMPARE(studio.line(studio.ink).size(), before.size());

        studio.trigger("act_deselect");
        studio.trigger("act_lasso");
        inject::mouse(canvas, inject::Phase::Press, QPointF(10, 10));
        inject::mouse(canvas, inject::Phase::Move, QPointF(30, 10));
        QTest::keyPress(canvas, Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(canvas->tool(), QStringLiteral("marquee"));
        inject::mouse(canvas, inject::Phase::Move, QPointF(30, 30), Qt::ControlModifier);
        inject::mouse(canvas, inject::Phase::Move, QPointF(10, 30), Qt::ControlModifier);
        inject::mouse(canvas, inject::Phase::Release, QPointF(10, 30), Qt::ControlModifier);
        QTest::keyRelease(canvas, Qt::Key_Control, Qt::NoModifier);
        QCOMPARE(canvas->tool(), QStringLiteral("marquee"));
        QVERIFY2(std::abs(studio.size() - 400) < 30, qPrintable(QString::number(studio.size())));
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
