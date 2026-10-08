// The drawing tools (M3④-2, Python's main.py and canvas.py) on the offscreen platform: their commands with Python's
// words, keys and tips in the ツール menu; スポイト (as seen, the layer's own, Alt held for a moment), 塗りつぶし,
// 囲って塗る (the shape, the closed areas in it, the gaps along it), グラデーション, 図形 (dragged, Shift for 45° and
// squares; a polyline clicked and ended by Enter, closed by Shift+Enter, forgotten by Esc), 色混ぜ, ゆがみ, 透明色,
// 塗り残し, メインとサブの入れ替え, 太く・細く — each becoming the op Python's window makes — and the カラー panel
// remembering the colour a line was drawn with.
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

core::Document book_doc() {
    core::Document doc = core::new_episode("道具", core::Num(1), 1, core::PageSpec::custom(120, 160, 100, 140, 1, 2, 2, 2, 2, 72, "color"));
    doc.edit_page(0).name_ok = true;
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;  // every op applied, in order
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
        // a paint layer to draw on
        QVERIFY(window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}}})));
        window->set_target_layer("p");
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    app::BrushPanel& brush() const { return *window->brush_panel(); }
    app::ColourPanel& colours() const { return *window->colours(); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
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
};

// a number as an op carries it, near the point asked (the click went to the screen and back)
bool close_to(const Json& v, double want, double within = 0.02) { return v.is_number() && std::abs(v.get<double>() - want) <= within; }
bool close2(const Json& p, double x, double y, double within = 0.02) { return p.is_array() && p.size() == 2 && close_to(p[0], x, within) && close_to(p[1], y, within); }

struct Expected {
    const char* attribute;
    const char* text;
    const char* key;
    const char* tip;
    bool checkable;
};

}  // namespace

class TestGuiPaint : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<genko::app::BookChange>();
    }

    void commandsAsPythonsWindowHasThem() {
        Studio s;
        const std::vector<Expected> expected = {
            {"act_picker", "スポイト", "I", "クリックした所の色をペンの色にします", true},
            {"act_fill", "塗りつぶし", "G", "線で囲まれた所をクリックで塗ります（隙間閉じ・見る範囲はブラシ パネルで）", true},
            {"act_lassofill", "囲って塗る", "Shift+G", "ドラッグで囲んだ所を塗ります", true},
            {"act_gradient", "グラデーション", "U", "ドラッグの向きに色をなめらかに変えて塗る（選択範囲があればその中だけ）", true},
            {"act_shape", "図形", "O",
             "直線・折れ線・曲線・長方形・楕円・多角形を描く（Shift で 45° と正方形。折れ線と曲線はクリックで点、ダブルクリックか Enter で終わり）", true},
            {"act_blend", "色混ぜ", "Shift+B", "ペイントのレイヤーの色をぼかす・指先でのばす・なじませる", true},
            {"act_liquify", "ゆがみ（指で押す）", "Shift+L", "なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く", true},
            {"act_point_thinner", "選んだ点を細く", "Ctrl+Alt+[", "線の編集で選んだ制御点のところだけ、線を細くします", false},
            {"act_fill_gaps", "塗り残しを塗る", "",
             "塗った色の間に残った小さなすき間を、同じ色で塗ります。選択範囲があればその中を、無ければなぞった所を", false},
            {"act_swap_colour", "メインとサブの色を入れ替える", "X", "", false},
            {"act_transparent", "透明色で描く", "", "ペンで描いた所が消える（もう一度で戻る）", true},
            {"act_thicker", "太く（ペン・消しゴム）", "]", "", false},
            {"act_thinner", "細く（ペン・消しゴム）", "[", "", false},
        };
        for (const Expected& e : expected) {
            QAction* a = s.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), QKeySequence(QString::fromLatin1(e.key)));
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QVERIFY2(s.menus_of(a).contains(QStringLiteral("ツール")), e.attribute);
        }
        // the tools are one at a time, and the palette holds Python's
        for (const char* tool : {"picker", "fill", "lassofill", "gradient", "shape", "blend", "liquify"}) {
            s.window->choose_tool(QString::fromLatin1(tool));
            QCOMPARE(s.canvas()->tool(), QString::fromLatin1(tool));
            QVERIFY(s.window->tool_actions().at(QString::fromLatin1(tool))->isChecked());
        }
        // the カラー panel is a tab on the right
        QDockWidget* dock = nullptr;
        for (QDockWidget* d : s.window->findChildren<QDockWidget*>())
            if (d->windowTitle() == QStringLiteral("カラー")) dock = d;
        QVERIFY(dock != nullptr);
        QVERIFY(dock->isAncestorOf(s.window->colours()));
    }

    void eyedropperAsSeenOrFromTheLayer() {
        Studio s;
        s.brush().set_colour({200, 30, 30});
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"}, {"rgb", Json::array({200, 30, 30})},
                                                      {"area", Json{{"poly", Json::parse("[[20, 20], [60, 20], [60, 60], [20, 60]]")}}}}})));
        QVERIFY(s.canvas()->wait_rendered(20000));
        s.brush().set_colour({1, 2, 3});
        s.window->choose_tool(QStringLiteral("picker"));
        inject::click_mm(s.canvas(), QPointF(40, 40));
        QCOMPARE(s.brush().rgb(), (app::Rgb{200, 30, 30}));
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("色を拾いました")));
        inject::click_mm(s.canvas(), QPointF(90, 120));  // (the white paper)
        QCOMPARE(s.brush().rgb(), (app::Rgb{255, 255, 255}));
        // from the layer drawn on: its own colour, and nothing where it is clear
        s.colours().pick_source->setCurrentIndex(1);
        emit s.colours().pick_source->activated(1);
        QCOMPARE(s.canvas()->pick_source, QStringLiteral("layer"));
        inject::click_mm(s.canvas(), QPointF(40, 40));
        QCOMPARE(s.brush().rgb(), (app::Rgb{200, 30, 30}));
        s.brush().set_colour({1, 2, 3});
        inject::click_mm(s.canvas(), QPointF(90, 120));
        QCOMPARE(s.brush().rgb(), (app::Rgb{1, 2, 3}));
        // Alt held: the eyedropper for a moment
        s.window->choose_tool(QStringLiteral("pen"));
        QTest::keyPress(s.canvas(), Qt::Key_Alt);
        QCOMPARE(s.canvas()->tool(), QStringLiteral("picker"));
        QTest::keyRelease(s.canvas(), Qt::Key_Alt);
        QCOMPARE(s.canvas()->tool(), QStringLiteral("pen"));
    }

    void fillsAsTheBrushPanelSays() {
        Studio s;
        s.brush().set_colour({10, 20, 30});
        s.window->choose_tool(QStringLiteral("fill"));
        inject::click_mm(s.canvas(), QPointF(40.123, 50.456));
        Json op = s.last();
        QCOMPARE(op["op"], Json("fill"));
        QCOMPARE(op["layer_id"], Json("p"));
        QVERIFY(close_to(op["x_mm"], 40.12) && close_to(op["y_mm"], 50.46));
        QCOMPARE(op["x_mm"].get<double>(), core::py_round(op["x_mm"].get<double>(), 2));  // (rounded to 0.01 mm)
        QCOMPARE(op["rgb"], Json::array({10, 20, 30}));
    }

    void lassoFillsAsTheBrushPanelSays() {
        // 囲って塗る: the closed areas in it (a square drawn in ink; with nothing closed in it — the paper already
        // filled, say — the lasso is refused, as in Python), the shape, the gaps along it (once there is colour)
        Studio s;
        const std::string ink = gui_test::ink_of(s.session->document().page(0))->id;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"width_mm", 0.8},
                                                      {"points", Json::parse("[[35, 35], [55, 35], [55, 55], [35, 55], [35, 35]]")}}})));
        Json op;
        const std::vector<QPointF> around{{20, 20}, {70, 22}, {72, 70}, {22, 72}, {20, 21}};
        s.window->choose_tool(QStringLiteral("lassofill"));
        for (const auto& [mode, want] : {std::pair{"enclosed", "fill_enclosed"}, {"shape", "fill_area"}, {"gaps", "fill_gaps"}}) {
            s.brush().lasso_mode->setCurrentIndex(s.brush().lasso_mode->findData(QString::fromLatin1(mode)));
            const std::size_t before = s.ops.size();
            inject::mouse_stroke(s.canvas(), around);
            QCOMPARE(s.ops.size(), before + 1);
            op = s.last();
            QCOMPARE(op["op"], Json(want));
            if (std::string(mode) == "shape") QCOMPARE(op["area"]["poly"].size(), around.size());
            if (std::string(mode) == "enclosed") QCOMPARE(op["poly"].size(), around.size());
            if (std::string(mode) == "gaps") QCOMPARE(op["max_mm"], Json(s.brush().gap_size->value()));
        }
        // a drag that is only a click fills nothing
        const std::size_t before = s.ops.size();
        inject::mouse_stroke(s.canvas(), {QPointF(30, 30), QPointF(30.2, 30.2)});
        QCOMPARE(s.ops.size(), before);
        // 塗り残しを塗る: without a selection, the tool that traces over the gaps; with one, in it
        s.brush().lasso_mode->setCurrentIndex(0);
        s.trigger("act_fill_gaps");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("lassofill"));
        QCOMPARE(s.brush().lasso_mode->currentData().toString(), QStringLiteral("gaps"));
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("塗り残しの所をなぞると")));
        s.trigger("act_select_all");
        s.trigger("act_fill_gaps");
        op = s.last();
        QCOMPARE(op["op"], Json("fill_gaps"));
        QVERIFY(op.contains("area"));
    }

    void gradientFromTheDrag() {
        Studio s;
        s.brush().set_colour({200, 100, 0});
        s.window->choose_tool(QStringLiteral("gradient"));
        auto* mode = s.window->findChild<QComboBox*>(QStringLiteral("gradient_mode"));
        QVERIFY(mode != nullptr);
        inject::mouse_stroke(s.canvas(), {QPointF(20, 30), QPointF(50, 31), QPointF(80, 30)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("gradient_fill"));
        QVERIFY(close2(op["from"], 20, 30) && close2(op["to"], 80, 30));
        QCOMPARE(op["rgb_from"], Json::array({200, 100, 0}));
        QCOMPARE(op["opacity_to"], Json(0.0));
        QCOMPARE(op["shape"], Json("linear"));
        mode->setCurrentIndex(mode->findData(QStringLiteral("bw")));
        inject::mouse_stroke(s.canvas(), {QPointF(20, 30), QPointF(80, 90)});
        op = s.last();
        QCOMPARE(op["rgb_from"], Json::array({20, 20, 20}));
        QCOMPARE(op["rgb_to"], Json::array({255, 255, 255}));
        mode->setCurrentIndex(mode->findData(QStringLiteral("radial")));
        s.trigger("act_select_all");
        s.window->choose_tool(QStringLiteral("gradient"));
        inject::mouse_stroke(s.canvas(), {QPointF(50, 50), QPointF(70, 50)});
        op = s.last();
        QCOMPARE(op["shape"], Json("radial"));
        QVERIFY(op.contains("area"));  // (only in the selection)
        // too short a drag paints nothing
        const std::size_t before = s.ops.size();
        inject::mouse_stroke(s.canvas(), {QPointF(50, 50), QPointF(50.2, 50.1)});
        QCOMPARE(s.ops.size(), before);
    }

    void figuresDrawnAsPythonsCanvasDrawsThem() {
        Studio s;
        s.brush().set_colour({5, 6, 7});
        s.window->choose_tool(QStringLiteral("shape"));
        auto* kind = s.window->findChild<QComboBox*>(QStringLiteral("shape_kind"));
        auto* style = s.window->findChild<QComboBox*>(QStringLiteral("shape_style"));
        auto* sides = s.window->findChild<QSpinBox*>(QStringLiteral("shape_sides"));
        auto* radius = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("shape_radius"));
        QVERIFY(kind && style && sides && radius);
        const auto choose = [&](const char* key) {
            kind->setCurrentIndex(kind->findData(QString::fromLatin1(key)));
            emit kind->activated(kind->currentIndex());
        };
        // a line; Shift: at 45°
        choose("line");
        inject::mouse_stroke(s.canvas(), {QPointF(20, 20), QPointF(40, 21)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_shape"));
        QCOMPARE(op["shape"], Json("line"));
        QVERIFY(close2(op["points"][0], 20, 20));
        QCOMPARE(op["line"], Json(true));
        QCOMPARE(op["fill"], Json(false));
        QCOMPARE(op["rgb"], Json::array({5, 6, 7}));
        inject::mouse_stroke(s.canvas(), {QPointF(20, 20), QPointF(40, 22)}, Qt::ShiftModifier);
        op = s.last();
        QVERIFY(close_to(op["points"][1][1], op["points"][0][1].get<double>(), 1e-6));  // (straight across)
        // a rectangle, filled, its corners rounded; Shift: a square
        choose("rect");
        style->setCurrentIndex(style->findData(QStringLiteral("both")));
        radius->setValue(2.5);
        inject::mouse_stroke(s.canvas(), {QPointF(60, 20), QPointF(80, 30)});
        op = s.last();
        QCOMPARE(op["shape"], Json("rect"));
        QVERIFY(close_to(op["box"][0], 60) && close_to(op["box"][1], 20) && close_to(op["box"][2], 20) && close_to(op["box"][3], 10));
        QCOMPARE(op["radius_mm"], Json(2.5));
        QCOMPARE(op["fill"], Json(true));
        inject::mouse_stroke(s.canvas(), {QPointF(60, 20), QPointF(80, 30)}, Qt::ShiftModifier);
        QVERIFY(close_to(s.last()["box"][2], 20) && close_to(s.last()["box"][3], s.last()["box"][2].get<double>(), 1e-9));  // (a square)
        // a polygon of 7 corners
        choose("polygon");
        sides->setValue(7);
        inject::mouse_stroke(s.canvas(), {QPointF(20, 60), QPointF(50, 90)});
        QCOMPARE(s.last()["sides"], Json(7));
        // a polyline clicked point by point: Enter ends it, Shift+Enter closes it, Esc forgets it
        choose("polyline");
        const std::size_t before = s.ops.size();
        for (const QPointF& p : {QPointF(20, 100), QPointF(40, 110), QPointF(60, 100)}) inject::click_mm(s.canvas(), p);
        QCOMPARE(s.ops.size(), before);  // (still being drawn)
        QTest::keyClick(s.canvas(), Qt::Key_Return);
        op = s.last();
        QCOMPARE(op["shape"], Json("polyline"));
        QCOMPARE(op["points"].size(), std::size_t{3});
        QVERIFY(!op.contains("closed"));
        for (const QPointF& p : {QPointF(20, 100), QPointF(40, 110), QPointF(60, 100)}) inject::click_mm(s.canvas(), p);
        QTest::keyClick(s.canvas(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(s.last()["closed"], Json(true));
        const std::size_t now = s.ops.size();
        inject::click_mm(s.canvas(), QPointF(20, 100));
        inject::click_mm(s.canvas(), QPointF(40, 100));
        QTest::keyClick(s.canvas(), Qt::Key_Escape);
        QTest::keyClick(s.canvas(), Qt::Key_Return);
        QCOMPARE(s.ops.size(), now);  // (forgotten)
        // the line is the pen's width
        QCOMPARE(s.last()["width_mm"], Json(s.canvas()->brush_width_mm));
    }

    void blendLiquifyAndTheTransparentPen() {
        Studio s;
        auto* blend_mode = s.window->findChild<QComboBox*>(QStringLiteral("blend_mode"));
        auto* blend_strength = s.window->findChild<QSpinBox*>(QStringLiteral("blend_strength"));
        auto* blend_size = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("blend_size"));
        auto* liquify_mode = s.window->findChild<QComboBox*>(QStringLiteral("liquify_mode"));
        auto* liquify_strength = s.window->findChild<QSpinBox*>(QStringLiteral("liquify_strength"));
        auto* liquify_size = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("liquify_size"));
        QVERIFY(blend_mode && blend_strength && blend_size && liquify_mode && liquify_strength && liquify_size);
        // (something on the paint layer to blend: an empty one is refused, as in Python)
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"}, {"rgb", Json::array({200, 30, 30})},
                                                      {"area", Json{{"poly", Json::parse("[[10, 10], [60, 10], [60, 60], [10, 60]]")}}}}})));
        s.window->choose_tool(QStringLiteral("blend"));
        blend_mode->setCurrentIndex(blend_mode->findData(QStringLiteral("smudge")));
        blend_strength->setValue(40);
        blend_size->setValue(8);
        inject::mouse_stroke(s.canvas(), {QPointF(20, 20), QPointF(30, 25), QPointF(40, 30)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("smudge"));
        QCOMPARE(op["mode"], Json("smudge"));
        QCOMPARE(op["strength"], Json(0.4));
        QCOMPARE(op["width_mm"], Json(8.0));
        QCOMPARE(op["points"][0].size(), std::size_t{3});  // (with the pen's pressure)
        s.window->choose_tool(QStringLiteral("liquify"));
        liquify_mode->setCurrentIndex(liquify_mode->findData(QStringLiteral("twirl_cw")));
        liquify_strength->setValue(25);
        liquify_size->setValue(12);
        inject::mouse_stroke(s.canvas(), {QPointF(20, 20), QPointF(30, 25)});
        op = s.last();
        QCOMPARE(op["op"], Json("liquify"));
        QCOMPARE(op["mode"], Json("twirl_cw"));
        QCOMPARE(op["strength"], Json(0.25));
        QCOMPARE(op["width_mm"], Json(12.0));
        QCOMPARE(op["points"][0].size(), std::size_t{2});
        // 透明色: the pen takes away where it passes (the menu and the panel's box are one)
        s.window->choose_tool(QStringLiteral("pen"));
        s.trigger("act_transparent");
        QVERIFY(s.colours().transparent->isChecked());
        inject::mouse_stroke(s.canvas(), {QPointF(20, 40), QPointF(50, 40)});
        op = s.last();
        QCOMPARE(op["op"], Json("erase"));
        QCOMPARE(op["width_mm"], Json(std::max(0.3, s.brush().size->value())));
        s.colours().transparent->setChecked(false);
        QVERIFY(!s.window->action("act_transparent")->isChecked());
        // a line drawn: its colour goes to the front of 履歴
        s.brush().set_colour({9, 99, 199});
        inject::mouse_stroke(s.canvas(), {QPointF(20, 50), QPointF(50, 50)});
        QCOMPARE(s.last()["op"], Json("add_stroke"));
        QCOMPARE(s.colours().history().front(), (app::Rgb{9, 99, 199}));
        // a colour chosen in the panel turns 透明色 off
        s.trigger("act_transparent");
        s.colours().choose({1, 1, 1});
        QVERIFY(!s.window->action("act_transparent")->isChecked());
    }

    void linesEditedAsPythonsVectorTool() {
        Studio s;
        const std::vector<Expected> expected = {
            {"act_reshape", "線の修正（つまむ）", "Y", "描いた線をつまんでドラッグすると、その辺りが滑らかに動きます", true},
            {"act_vector", "線の編集（制御点）", "Shift+Y", "線を選んで制御点を動かす（Alt+クリックで点を足す、Delete で消す、Shift+クリックで 2 本目）", true},
            {"act_vector_join", "選んだ 2 本の線をつなぐ", "", "", false},
            {"act_vector_cut", "クリックした所で線を切る", "", "オンの間、線をクリックするとそこで 2 本に分かれます", true},
            {"act_vector_colour", "選んだ線をペンの色にする", "", "", false},
            {"act_vector_delete", "選んだ線を消す", "", "", false},
            {"act_vector_simplify", "選んだ線の点を減らす", "", "形を保ったまま、制御点を減らします（単純化）", false},
        };
        for (const Expected& e : expected) {
            QAction* a = s.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), QKeySequence(QString::fromLatin1(e.key)));
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
        }
        // two pen lines on the ink layer
        const std::string ink = gui_test::ink_of(s.session->document().page(0))->id;
        s.window->set_target_layer(ink);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink},
                                                      {"points", Json::parse("[[20, 20], [40, 20], [60, 20]]")}},
                                                 Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink},
                                                      {"points", Json::parse("[[62, 20], [80, 20]]")}}})));
        // (add_stroke gives each line its own new id, as in Python)
        const core::Layer* inked = gui_test::ink_of(s.session->document().page(0));
        QCOMPARE(inked->strokes->items.size(), std::size_t{2});
        const std::string a = inked->strokes->items[0]->id, b = inked->strokes->items[1]->id;
        s.trigger("act_vector");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("vector"));
        // choose a line; drag its control point
        inject::click_mm(s.canvas(), QPointF(30, 20));
        QCOMPARE(s.canvas()->vector_ids, (std::vector<std::string>{a}));
        inject::mouse_stroke(s.canvas(), {QPointF(40, 20), QPointF(40, 25), QPointF(40, 30)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("vector_edit"));
        QCOMPARE(op["action"], Json("move_point"));
        QCOMPARE(op["stroke_id"], Json(a));
        QCOMPARE(op["index"], Json(1));
        QVERIFY(close2(op["to"], 40, 30));
        QCOMPARE(s.canvas()->vector_point, std::optional<int>(1));
        // Delete takes the chosen point away; Alt+click on the line adds one (the canvas's own key and click, as
        // Python's canvas keyPressEvent and mousePressEvent take them: no command of the window has Delete in either,
        // so the window behaves the same; a desktop that keeps Alt+drag for moving windows does so for both)
        QTest::keyClick(s.canvas(), Qt::Key_Delete);
        QCOMPARE(s.last()["action"], Json("delete_point"));
        inject::click_mm(s.canvas(), QPointF(30, 20));
        inject::click_mm(s.canvas(), QPointF(50, 20), Qt::AltModifier);
        QCOMPARE(s.last()["action"], Json("add_point"));
        // Shift+click: a second line; the two joined
        inject::click_mm(s.canvas(), QPointF(70, 20), Qt::ShiftModifier);
        QCOMPARE(s.canvas()->vector_ids, (std::vector<std::string>{a, b}));
        s.trigger("act_vector_join");
        op = s.last();
        QCOMPARE(op["action"], Json("connect"));
        QCOMPARE(op["ids"], Json::array({a, b}));
        QCOMPARE(s.canvas()->vector_ids, (std::vector<std::string>{a}));
        // the pen's colour, fewer points
        s.brush().set_colour({90, 10, 10});
        s.trigger("act_vector_colour");
        QCOMPARE(s.last()["action"], Json("recolor"));
        QCOMPARE(s.last()["rgb"], Json::array({90, 10, 10}));
        s.trigger("act_vector_simplify");
        QCOMPARE(s.last()["action"], Json("simplify"));
        QCOMPARE(s.last()["stroke_id"], Json(a));
        // cut where clicked (while it is on)
        s.trigger("act_vector_cut");
        QVERIFY(s.canvas()->vector_cut);
        inject::click_mm(s.canvas(), QPointF(30, 20));
        op = s.last();
        QCOMPARE(op["action"], Json("cut"));
        QVERIFY(close2(op["at"], 30, 20, 0.5));
        s.trigger("act_vector_cut");
        QVERIFY(!s.canvas()->vector_cut);
        // a click on empty paper chooses nothing; the commands then say how
        inject::click_mm(s.canvas(), QPointF(100, 140));
        QVERIFY(s.canvas()->vector_ids.empty());
        s.trigger("act_vector_delete");
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("先に「線の編集」")));
        // なぞって直す: a trace becomes a trace_edit op with the tool's settings
        auto* mode = s.window->findChild<QComboBox*>(QStringLiteral("vector_mode"));
        QVERIFY(mode != nullptr);
        mode->setCurrentIndex(mode->findData(QStringLiteral("widen")));
        QCOMPARE(s.canvas()->vector_mode, QStringLiteral("widen"));
        inject::mouse_stroke(s.canvas(), {QPointF(20, 21), QPointF(30, 21), QPointF(40, 21)});
        op = s.last();
        QCOMPARE(op["op"], Json("trace_edit"));
        QCOMPARE(op["action"], Json("widen"));
        QCOMPARE(op["amount"], Json(0.3));
        QCOMPARE(op["radius_mm"], Json(2.0));
        QCOMPARE(op["points"][0].size(), std::size_t{3});
        mode->setCurrentIndex(0);
        // 線の修正（つまむ）: pinched into new points, its ends kept when asked
        s.trigger("act_reshape");
        auto* pin = s.window->findChild<QCheckBox*>(QStringLiteral("reshape_pin"));
        QVERIFY(pin != nullptr);
        pin->setChecked(true);
        const std::size_t before = s.ops.size();
        inject::mouse_stroke(s.canvas(), {QPointF(60, 20), QPointF(60, 23), QPointF(60, 26)});
        QCOMPARE(s.ops.size(), before + 1);
        op = s.last();
        QCOMPARE(op["op"], Json("reshape_stroke"));
        const Json& points = op["points"];
        QVERIFY(points.size() > 3);
        QVERIFY(close2(Json::array({points.front()[0], points.front()[1]}), points.front()[0].get<double>(), 20, 1e-9));  // (an end kept)
    }

    void swapAndSizes() {
        Studio s;
        s.brush().set_colour({10, 20, 30});
        const app::Rgb sub = s.colours().sub_rgb();
        s.trigger("act_swap_colour");
        QCOMPARE(s.brush().rgb(), sub);
        QCOMPARE(s.colours().sub_rgb(), (app::Rgb{10, 20, 30}));
        s.trigger("act_swap_colour");
        QCOMPARE(s.brush().rgb(), (app::Rgb{10, 20, 30}));
        // ] and [: the pen's size one step up and down, or the eraser's
        s.window->choose_tool(QStringLiteral("pen"));
        const double width = s.canvas()->brush_width_mm;
        s.trigger("act_thicker");
        QVERIFY(s.canvas()->brush_width_mm > width);
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("ペンの太さ")));
        const double wider = s.canvas()->brush_width_mm;
        s.trigger("act_thinner");
        QVERIFY(s.canvas()->brush_width_mm < wider);
        s.window->choose_tool(QStringLiteral("eraser"));
        const double eraser = s.canvas()->eraser_mm;
        s.trigger("act_thicker");
        QVERIFY(s.canvas()->eraser_mm > eraser);
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("消しゴムの太さ")));
        auto* box = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("eraser_size"));
        QCOMPARE(box->value(), s.canvas()->eraser_mm);
        s.trigger("act_thinner");
        QCOMPARE(s.canvas()->eraser_mm, 2.0);
        // 選んだ点を細く without a point chosen: said how
        s.trigger("act_point_thinner");
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("先に")));
    }
};

QTEST_MAIN(TestGuiPaint)
#include "test_gui_paint.moc"
