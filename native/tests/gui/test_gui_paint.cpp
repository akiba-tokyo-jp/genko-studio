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
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolBar>
#include <QTreeWidget>
#include <filesystem>
#include <fstream>
#include <QToolButton>
#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/gradients_panel.hpp"
#include "app/inject.hpp"
#include "app/layer_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/json.hpp"
#include "core/ops_util.hpp"
#include "core/poses.hpp"
#include "core/pynum.hpp"
#include "render/gradient_presets.hpp"
#include "render/grd.hpp"
#include "render/preset_store.hpp"
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
            // (PC4 E-0, the user's decision 2: the drawing tools and the colours by Photoshop's names)
            // (PC4 E4: the スポイトツール's Alt+click, the 塗りつぶしツール's 基準 — their settings on their own pages)
            {"act_picker", "スポイトツール", "I", "クリックした所の色を描画色にします（Alt＋クリックで背景色）", true},
            {"act_fill", "塗りつぶしツール", "G", "クリックした所を塗ります（基準は 線で囲まれた所・色の近さ。ツールの設定で）", true},
            {"act_lassofill", "囲って塗る", "Shift+G", "ドラッグで囲んだ所を塗ります", true},
            {"act_gradient", "グラデーションツール", "U", "ドラッグの向きに色をなめらかに変えて塗る（選択範囲があればその中だけ）", true},
            // (PC3 C-b, the user's decisions 5 and 7: the 図形 is the shape tools' 長方形ツール — test_gui_shapes)
            {"act_shape", "長方形ツール", "O",
             "ドラッグで長方形のシェイプを描きます（Shift で正方形、Alt で中心から。モードがピクセルなら直線・折れ線・曲線なども）", true},
            {"act_blend", "色混ぜ", "Shift+B", "ペイントのレイヤーの色をぼかす・指先でのばす・なじませる（Photoshop のぼかしツール・指先ツールは別の道具）", true},
            {"act_liquify", "前方ワープツール", "Shift+L", "なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く", true},
            {"act_point_thinner", "選んだ点を細く", "Ctrl+Alt+[", "部分選択ツールで選んだ制御点のところだけ、線を細くします", false},  // (PC3 C-b)
            {"act_fill_gaps", "塗り残しを塗る", "",
             "塗った色の間に残った小さなすき間を、同じ色で塗ります。選択範囲があればその中を、無ければなぞった所を", false},
            {"act_swap_colour", "描画色と背景色を入れ替え", "X", "", false},
            {"act_default_colours", "描画色と背景色を初期設定に戻す", "D", "描画色を黒に、背景色を白にします", false},
            {"act_transparent", "透明色で描く", "", "ブラシで描いた所が消える（もう一度で戻る）", true},
            {"act_thicker", "ブラシサイズを大きく", "]", "", false},
            {"act_thinner", "ブラシサイズを小さく", "[", "", false},
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

    // PC4 E4 (tool:Gradient, Photoshop's options bar): the current gradient (描画色から背景色へ at first; a preset of the list) with
    // the 描画色 and 背景色 now, 形, 逆方向, 不透明度, モード, ディザ, 透明部分; a paint layer's pixels take it (no picture laid on
    // it), undone as one step; in a selection only; too short a drag nothing
    void gradientFromTheDrag() {
        Studio s;
        s.brush().set_colour({200, 100, 0});
        s.colours().set_background({0, 0, 255});
        s.window->choose_tool(QStringLiteral("gradient"));
        auto* preset = s.window->findChild<QComboBox*>(QStringLiteral("gradient_preset"));
        auto* shape = s.window->findChild<QComboBox*>(QStringLiteral("gradient_shape"));
        auto* blend = s.window->findChild<QComboBox*>(QStringLiteral("gradient_blend"));
        auto* opacity = s.window->findChild<QSpinBox*>(QStringLiteral("gradient_opacity"));
        auto* reverse = s.window->findChild<QCheckBox*>(QStringLiteral("gradient_reverse"));
        auto* dither = s.window->findChild<QCheckBox*>(QStringLiteral("gradient_dither"));
        auto* transparency = s.window->findChild<QCheckBox*>(QStringLiteral("gradient_transparency"));
        QVERIFY(preset && shape && blend && opacity && reverse && dither && transparency);
        QCOMPARE(s.window->tool_settings()->current_page(), s.window->tool_settings()->page_for(QStringLiteral("gradient")));
        QCOMPARE(preset->itemText(0), QStringLiteral("描画色から背景色へ"));
        QCOMPARE(shape->currentData().toString(), QStringLiteral("linear"));
        QVERIFY(dither->isChecked() && transparency->isChecked() && !reverse->isChecked());
        QStringList shapes;
        for (int i = 0; i < shape->count(); ++i) shapes << shape->itemText(i);
        QCOMPARE(shapes, (QStringList{QStringLiteral("線形"), QStringLiteral("円形"), QStringLiteral("円錐形"), QStringLiteral("反射形"),
                                      QStringLiteral("菱形"), QStringLiteral("楕円")}));
        const auto layer_p = [&s]() -> const core::Layer& {
            const core::Page& page = s.session->document().page(0);
            return *std::find_if(page.layers.begin(), page.layers.end(), [](const core::Layer& l) { return l.id == "p"; });
        };
        const auto png_before = layer_p().raster_png;
        inject::mouse_stroke(s.canvas(), {QPointF(20, 30), QPointF(50, 31), QPointF(80, 30)});
        Json op = s.last();
        QCOMPARE(op["op"], Json("gradient_fill"));
        QVERIFY(close2(op["from"], 20, 30) && close2(op["to"], 80, 30));
        QCOMPARE(op["stops"], Json::parse("[[0.0, [200, 100, 0], 1.0], [1.0, [0, 0, 255], 1.0]]"));
        QCOMPARE(op["shape"], Json("linear"));
        QCOMPARE(op["dither"], Json(true));
        QVERIFY(!op.contains("transparency") && !op.contains("reverse") && !op.contains("blend") && !op.contains("opacity"));
        QVERIFY(layer_p().patches.empty());  // (the paint layer's pixels: no picture laid on it)
        QVERIFY(layer_p().raster_png && !layer_p().raster_png->empty() && layer_p().raster_png != png_before);
        s.trigger("act_undo");
        QVERIFY(layer_p().raster_png == png_before || (layer_p().raster_png && png_before && *layer_p().raster_png == *png_before));
        // 描画色から透明に: its transparency; 逆方向, 不透明度, モード and 円形, in a selection
        const int fading = preset->findText(QStringLiteral("基本 / 描画色から透明に"));
        QVERIFY(fading > 0);
        preset->setCurrentIndex(fading);
        emit preset->activated(fading);
        QCOMPARE(preset->itemText(0), QStringLiteral("描画色から透明に"));
        reverse->setChecked(true);
        opacity->setValue(50);
        blend->setCurrentIndex(blend->findData(QStringLiteral("multiply")));
        emit blend->activated(blend->currentIndex());
        shape->setCurrentIndex(shape->findData(QStringLiteral("radial")));
        emit shape->activated(shape->currentIndex());
        s.trigger("act_select_all");
        s.window->choose_tool(QStringLiteral("gradient"));
        inject::mouse_stroke(s.canvas(), {QPointF(50, 50), QPointF(70, 50)});
        op = s.last();
        QCOMPARE(op["shape"], Json("radial"));
        QCOMPARE(op["stops"], Json::parse("[[0.0, [200, 100, 0], 1.0], [1.0, [200, 100, 0], 1.0]]"));
        QCOMPARE(op["transparency"], Json::parse("[[0.0, 1.0], [1.0, 0.0]]"));
        QCOMPARE(op["reverse"], Json(true));
        QCOMPARE(op["opacity"], Json(0.5));
        QCOMPARE(op["blend"], Json("multiply"));
        QVERIFY(op.contains("area"));  // (only in the selection)
        // 透明部分 off: opaque; ディザ off
        transparency->setChecked(false);
        dither->setChecked(false);
        inject::mouse_stroke(s.canvas(), {QPointF(50, 50), QPointF(70, 50)});
        op = s.last();
        QVERIFY(!op.contains("transparency"));
        QCOMPARE(op["dither"], Json(false));
        // too short a drag paints nothing
        const std::size_t before = s.ops.size();
        inject::mouse_stroke(s.canvas(), {QPointF(50, 50), QPointF(50.2, 50.1)});
        QCOMPARE(s.ops.size(), before);
        // (the settings as they were, for the tests after)
        for (QCheckBox* box : {reverse, transparency, dither}) box->setChecked(box != reverse);
        opacity->setValue(100);
        for (QComboBox* box : {blend, shape}) {
            box->setCurrentIndex(0);
            emit box->activated(0);
        }
        const int first = preset->findText(QStringLiteral("基本 / 描画色から背景色へ"));
        preset->setCurrentIndex(first);
        emit preset->activated(first);
        s.colours().set_background({255, 255, 255});
    }

    // PC4 E4 (paint.bucket, tool:PaintBucket): the 塗りつぶしツール's own page — 基準, 塗りつぶし (パターン grey until the patterns
    // are in), モード, 不透明度, 色の近さ's 許容値・アンチエイリアス・隣接・全レイヤー, and the lines' fields of before — also
    // 囲って塗る's; 色の近さ a fill_area of the colour's area; a モード and 不透明度 sent; a 描画色 of more than 8 bits on a page of
    // 16 bits its floats.
    void fillTheToolsWays() {
        Studio s;
        s.window->choose_tool(QStringLiteral("fill"));
        auto* basis = s.window->findChild<QComboBox*>(QStringLiteral("fill_basis"));
        auto* source = s.window->findChild<QComboBox*>(QStringLiteral("fill_source"));
        auto* blend = s.window->findChild<QComboBox*>(QStringLiteral("fill_blend"));
        auto* opacity = s.window->findChild<QSpinBox*>(QStringLiteral("fill_opacity"));
        auto* tolerance = s.window->findChild<QSpinBox*>(QStringLiteral("fill_tolerance"));
        auto* contiguous = s.window->findChild<QCheckBox*>(QStringLiteral("fill_contiguous"));
        auto* all_layers = s.window->findChild<QCheckBox*>(QStringLiteral("fill_all_layers"));
        auto* antialias = s.window->findChild<QCheckBox*>(QStringLiteral("fill_antialias"));
        QVERIFY(basis && source && blend && opacity && tolerance && contiguous && all_layers && antialias);
        QWidget* page = s.window->tool_settings()->current_page();
        QCOMPARE(page, s.window->tool_settings()->page_for(QStringLiteral("fill")));
        QCOMPARE(s.window->tool_settings()->page_for(QStringLiteral("lassofill")), page);
        QVERIFY(page->isAncestorOf(s.brush().gap) && page->isAncestorOf(s.brush().lasso_mode));
        QVERIFY(!s.brush().isAncestorOf(s.brush().gap));  // (no longer under the brush's settings)
        QCOMPARE(s.window->tool_settings()->page_for(QStringLiteral("pen")), static_cast<QWidget*>(&s.brush()));
        auto* model = qobject_cast<QStandardItemModel*>(source->model());
        QVERIFY(model != nullptr && !model->item(1)->isEnabled());
        QCOMPARE(source->currentData().toString(), QStringLiteral("foreground"));
        QCOMPARE(basis->currentData().toString(), QStringLiteral("lines"));
        QCOMPARE(tolerance->value(), 32);
        QVERIFY(contiguous->isChecked() && antialias->isChecked() && !all_layers->isChecked());
        // (review P3-2) on a pen layer the モード is grey and the fill goes in 通常 (the setting kept for paint layers)
        blend->setCurrentIndex(blend->findData(QStringLiteral("multiply")));
        emit blend->activated(blend->currentIndex());
        const std::string pen = s.session->document().page(0).first_layer(core::LayerRole::Ink)->id;
        s.window->set_target_layer(pen);
        QVERIFY(!blend->isEnabled());
        inject::click_mm(s.canvas(), QPointF(40, 50));
        QCOMPARE(s.last()["op"], Json("fill"));
        QCOMPARE(s.last()["layer_id"], Json(pen));
        QVERIFY(!s.last().contains("blend"));
        s.trigger("act_undo");
        s.window->set_target_layer("p");
        QVERIFY(blend->isEnabled());
        // モード and 不透明度, 線で囲まれた所: fill with them (the paint layer's pixels; the layer holds no lines or pictures yet: not
        // asked)
        blend->setCurrentIndex(blend->findData(QStringLiteral("multiply")));
        emit blend->activated(blend->currentIndex());
        opacity->setValue(60);
        s.brush().set_colour({10, 200, 30});
        inject::click_mm(s.canvas(), QPointF(40, 50));
        Json op = s.last();
        QCOMPARE(op["op"], Json("fill"));
        QCOMPARE(op["blend"], Json("multiply"));
        QCOMPARE(op["opacity"], Json(0.6));
        QCOMPARE(op["rgb"], Json::array({10, 200, 30}));
        QVERIFY(!op.contains("color_rgb"));
        QVERIFY(s.session->document().page(0).layers.back().patches.empty());
        blend->setCurrentIndex(0);
        emit blend->activated(0);
        opacity->setValue(100);
        // 色の近さ: a fill_area of the colour's area, the colour the 描画色
        basis->setCurrentIndex(basis->findData(QStringLiteral("color")));
        emit basis->activated(basis->currentIndex());
        tolerance->setValue(40);
        const std::size_t patches = s.session->document().page(0).layers.back().patches.size();
        inject::click_mm(s.canvas(), QPointF(40, 50));
        op = s.last();
        QCOMPARE(op["op"], Json("fill_area"));
        QCOMPARE(op["rgb"], Json::array({10, 200, 30}));
        QCOMPARE(s.session->document().page(0).layers.back().patches.size(), patches + 1);
        basis->setCurrentIndex(basis->findData(QStringLiteral("lines")));
        emit basis->activated(basis->currentIndex());
        // a precise 描画色 on a page of 16 bits: its floats (the two fills undone: the green there would be a wall)
        s.trigger("act_undo");
        s.trigger("act_undo");
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})));
        s.colours().choose_precise({0.3, 0.6, 0.15});
        inject::click_mm(s.canvas(), QPointF(40, 50));
        op = s.last();
        QCOMPARE(op["op"], Json("fill"));
        QCOMPARE(op["color_rgb"]["precision"], Json("u16"));
        QCOMPARE(op["color_rgb"]["values"][0].get<double>(), std::round(0.3 * 65535) / 65535);
        // (the settings as they were)
        tolerance->setValue(32);
    }

    // PC4 E4 (tool:Eyedropper): サンプル範囲 (an average of the pixels around), サンプル 現在のレイヤー以下 (the layers above left
    // out), Alt+click the 背景色 (the 描画色 as it was), a page of 16 bits' floats
    void eyedropperSamplesAndBackground() {
        Studio s;
        s.window->choose_tool(QStringLiteral("picker"));
        QWidget* page = s.window->tool_settings()->current_page();
        QCOMPARE(page, s.window->tool_settings()->page_for(QStringLiteral("picker")));
        auto* size = s.window->findChild<QComboBox*>(QStringLiteral("picker_size"));
        QVERIFY(size != nullptr && page->isAncestorOf(size) && page->isAncestorOf(s.colours().pick_source));
        QCOMPARE(size->count(), 7);
        // red on the left of x 50, blue on its right, a green layer above at the bottom
        QVERIFY(s.window->apply_ops(Json::array({
            Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"}, {"rgb", Json::array({255, 0, 0})}, {"area", Json{{"poly", Json::parse("[[0, 0], [50, 0], [50, 160], [0, 160]]")}}}},
            Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"}, {"rgb", Json::array({0, 0, 255})}, {"area", Json{{"poly", Json::parse("[[50, 0], [120, 0], [120, 160], [50, 160]]")}}}},
            Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "q"}},
            Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "q"}, {"rgb", Json::array({0, 255, 0})}, {"area", Json{{"poly", Json::parse("[[0, 100], [120, 100], [120, 160], [0, 160]]")}}}}})));
        QVERIFY(s.canvas()->wait_rendered(20000));
        s.window->set_target_layer("p");
        s.window->choose_tool(QStringLiteral("picker"));
        inject::click_mm(s.canvas(), QPointF(40, 40));
        QCOMPARE(s.brush().rgb(), (app::Rgb{255, 0, 0}));
        // Alt+click: the 背景色 (the green layer on top there)
        inject::click_mm(s.canvas(), QPointF(90, 120), Qt::AltModifier);
        QCOMPARE(s.colours().sub_rgb(), (app::Rgb{0, 255, 0}));
        QCOMPARE(s.brush().rgb(), (app::Rgb{255, 0, 0}));
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("背景色を拾いました")));
        // 11×11 of the layer on its edge: both colours in it
        s.colours().pick_source->setCurrentIndex(1);
        emit s.colours().pick_source->activated(1);
        size->setCurrentIndex(size->findData(11));
        emit size->activated(size->currentIndex());
        inject::click_mm(s.canvas(), QPointF(50, 40));
        const app::Rgb mixed = s.brush().rgb();
        QVERIFY2(mixed[0] > 60 && mixed[2] > 60 && mixed[1] < 10, qPrintable(QStringLiteral("%1 %2 %3").arg(mixed[0]).arg(mixed[1]).arg(mixed[2])));
        size->setCurrentIndex(0);
        emit size->activated(0);
        // 現在のレイヤー以下: the green layer above left out; すべてのレイヤー: green
        s.colours().pick_source->setCurrentIndex(2);
        emit s.colours().pick_source->activated(2);
        inject::click_mm(s.canvas(), QPointF(90, 120));
        QCOMPARE(s.brush().rgb(), (app::Rgb{0, 0, 255}));
        s.colours().pick_source->setCurrentIndex(0);
        emit s.colours().pick_source->activated(0);
        inject::click_mm(s.canvas(), QPointF(90, 120));
        QCOMPARE(s.brush().rgb(), (app::Rgb{0, 255, 0}));
        // a page of 16 bits: the floats there (a precise layer on top, a precise colour laid in it by gradient_fill)
        const Json exact{{"precision", "u16"}, {"values", Json::array({0.3, 0.6, 0.15})}};
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                  Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "r"}},
                                                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "r"}, {"panel_clip", false}},
                                                  Json{{"op", "gradient_fill"}, {"page", 1}, {"layer_id", "r"}, {"from", Json::array({60, 130})}, {"to", Json::array({120, 130})},
                                                       {"stops", Json::array({Json::array({0, exact}), Json::array({1, exact})})},
                                                       {"area", Json{{"poly", Json::parse("[[60, 100], [120, 100], [120, 160], [60, 160]]")}}}}})));
        QVERIFY(s.session->document().page(0).layers[core::layer_by_id(s.session->document().page(0), "r")].color_raster != nullptr);
        QVERIFY(s.canvas()->wait_rendered(20000));
        inject::click_mm(s.canvas(), QPointF(90, 120));
        QVERIFY(s.colours().is_precise());
        QVERIFY2(std::abs(s.colours().foreground()[0] - 0.3) < 2e-4 && std::abs(s.colours().foreground()[2] - 0.15) < 2e-4,
                 qPrintable(QString::number(s.colours().foreground()[0], 'g', 9)));
        inject::click_mm(s.canvas(), QPointF(90, 120), Qt::AltModifier);
        QVERIFY(std::abs(s.colours().background()[1] - 0.6) < 2e-4);
        s.colours().set_background({255, 255, 255});
    }

    // PC4 E4 (window.panel.gradients, gradient.presets.*): the グラデーション panel in ウィンドウ — a preset clicked is the current
    // gradient (and recolours a gradient fill layer chosen), a new one from it, renamed, deleted, a group made, a .grd read,
    // 新規グラデーション塗りつぶしレイヤー; kept in the settings folder's gradients.json; its name apart from the tool's old one.
    void theGradientsPanel() {
        const auto file = core::poses::config_dir() / "gradients.json";
        std::filesystem::remove(file);
        Studio s;
        QDockWidget* dock = nullptr;
        for (QDockWidget* d : s.window->findChildren<QDockWidget*>())
            if (d->windowTitle() == QStringLiteral("グラデーション")) dock = d;
        QVERIFY(dock != nullptr && !dock->isVisible());
        QVERIFY(s.menus_of(dock->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        auto* panel = s.window->findChild<app::GradientsPanel*>();
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->tree->topLevelItemCount(), 9);
        QCOMPARE(panel->tree->topLevelItem(0)->text(0), QStringLiteral("基本"));
        // a preset clicked: the current gradient
        QTreeWidgetItem* black_white = panel->tree->topLevelItem(0)->child(2);
        QCOMPARE(black_white->text(0), QStringLiteral("黒、白"));
        panel->tree->setCurrentItem(black_white);
        emit panel->tree->itemClicked(black_white, 0);
        auto* preset = s.window->findChild<QComboBox*>(QStringLiteral("gradient_preset"));
        QCOMPARE(preset->itemText(0), QStringLiteral("黒、白"));
        // a new preset of it, renamed, a group, deleted — written to gradients.json
        panel->new_group(QStringLiteral("自分の"));
        panel->tree->setCurrentItem(panel->tree->topLevelItem(9));
        panel->new_preset(QStringLiteral("白黒"));
        QVERIFY(std::filesystem::exists(file));
        QCOMPARE(panel->tree->topLevelItem(9)->childCount(), 1);
        panel->tree->setCurrentItem(panel->tree->topLevelItem(9)->child(0));
        panel->rename_chosen(QStringLiteral("モノクロ"));
        QCOMPARE(panel->tree->topLevelItem(9)->child(0)->text(0), QStringLiteral("モノクロ"));
        const render::preset_store::Library kept = render::preset_store::load(core::poses::config_dir(), render::gradient_presets::kind());
        QVERIFY(render::preset_store::find(kept.groups, "モノクロ") != nullptr);
        panel->tree->setCurrentItem(panel->tree->topLevelItem(9)->child(0));
        panel->delete_chosen();
        QCOMPARE(panel->tree->topLevelItem(9)->childCount(), 0);
        // a .grd read: a group of the file's name
        QTemporaryDir tmp;
        render::grd::Gradient g;
        g.name = "夕焼け";
        g.stops = {{0.0, 0.5, render::grd::Colour{render::grd::Colour::Kind::Rgb, {1, 0.5, 0, 0}}}, {1.0, 0.5, render::grd::Colour{render::grd::Colour::Kind::Background, {}}}};
        const QString grd_file = tmp.path() + QStringLiteral("/空.grd");
        {
            std::ofstream out(grd_file.toStdString(), std::ios::binary);
            out << render::grd::write({g});
        }
        // (read in the background: the panel's menu grey until it is in — review P1-1)
        QSignalSpy imported(panel, &app::GradientsPanel::import_done);
        panel->import_grd(grd_file);
        QVERIFY(panel->importing() && !panel->more->isEnabled());
        QVERIFY(imported.wait(20000));
        QVERIFY(!panel->importing() && panel->more->isEnabled());
        QCOMPARE(panel->tree->topLevelItem(panel->tree->topLevelItemCount() - 1)->text(0), QStringLiteral("空"));
        QVERIFY(s.window->last_notice().contains(QStringLiteral("1 個")));
        // the same file again: asked before the group is replaced — no (the test's answer): its presets added to it
        // (review P3-3)
        s.answers.asked.clear();
        panel->import_grd(grd_file);
        QVERIFY(imported.wait(20000));
        QVERIFY(s.answers.asked.join(QLatin1Char('\n')).contains(QStringLiteral("グループ「空」はもうあります")));
        QTreeWidgetItem* sky = panel->tree->topLevelItem(panel->tree->topLevelItemCount() - 1);
        QCOMPARE(sky->childCount(), 2);
        QCOMPARE(sky->child(1)->text(0), QStringLiteral("夕焼け 2"));
        // what `genko gradients` (or another window) wrote since is kept when the panel writes (review P3-4)
        {
            auto library = render::preset_store::load(core::poses::config_dir(), render::gradient_presets::kind());
            (void)render::preset_store::add(library.groups, render::gradient_presets::kind(),
                                            render::gradient_presets::item_of(Json::parse("{\"stops\": [\"#ff0000\", \"#00ff00\"]}"), "外から"), std::string("自分の"), "x");
            render::preset_store::save(core::poses::config_dir(), render::gradient_presets::kind(), library.groups);
        }
        panel->new_group(QStringLiteral("あとの"));
        const render::preset_store::Library both = render::preset_store::load(core::poses::config_dir(), render::gradient_presets::kind());
        QVERIFY(render::preset_store::find(both.groups, "外から") != nullptr);
        QVERIFY(std::any_of(both.groups.begin(), both.groups.end(), [](const auto& group) { return group.name == "あとの"; }));
        // 新規グラデーション塗りつぶしレイヤー: a gradient fill layer of the preset; a preset clicked with it chosen recolours it
        const std::size_t layers = s.session->document().page(0).layers.size();
        panel->tree->setCurrentItem(black_white = panel->tree->topLevelItem(0)->child(2));
        emit panel->apply->clicked();
        QCOMPARE(s.session->document().page(0).layers.size(), layers + 1);
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_layer"));
        QCOMPARE(op["kind"], Json("gradient"));
        QCOMPARE(op["gradient"]["stops"], Json::parse("[[0.0, [0, 0, 0], 1.0], [1.0, [255, 255, 255], 1.0]]"));
        const auto fill_layer = [&s]() -> const core::Layer& {
            const core::Page& page = s.session->document().page(0);
            // (the gradient's: the page's paper is a fill layer too)
            return *std::find_if(page.layers.begin(), page.layers.end(),
                                 [](const core::Layer& l) { return l.kind == core::LayerKind::Fill && l.fill && l.fill->contains("gradient"); });
        };
        const std::string made = fill_layer().id;
        s.window->set_target_layer(made);
        QTreeWidgetItem* fading = panel->tree->topLevelItem(0)->child(1);
        panel->tree->setCurrentItem(fading);
        emit panel->tree->itemClicked(fading, 0);
        op = s.last();
        QCOMPARE(op["op"], Json("set_layer"));
        QCOMPARE(op["fill"]["gradient"]["transparency"], Json::parse("[[0.0, 1.0], [1.0, 0.0]]"));
        QCOMPARE(fill_layer().fill->at("gradient").at("transparency"), Json::parse("[[0.0, 1.0], [1.0, 0.0]]"));
        // the layer panel's グラデーション… on it, OK with nothing changed: its transparency kept (review P2-2)
        s.answers.responder->exec = [](QDialog* dialog) {
            return int(dialog->objectName() == QStringLiteral("gradient_dialog") ? QDialog::Accepted : QDialog::Rejected);
        };
        const Json now = fill_layer().fill->at("gradient");
        const std::optional<Json> again = app::gradient_dialog(nullptr, &s.session->document().page(0), now);
        QVERIFY(again.has_value());
        QCOMPARE(again->at("transparency"), now.at("transparency"));
        std::filesystem::remove(file);
    }

    void figuresDrawnAsPythonsCanvasDrawsThem() {
        Studio s;
        s.brush().set_colour({5, 6, 7});
        s.window->choose_tool(QStringLiteral("shape"));
        // (PC3 C-b, the user's decision 7: the shape tools draw shape parts by default — シェイプ —; ピクセル draws as the
        // 図形 of before)
        auto* shape_mode = s.window->findChild<QComboBox*>(QStringLiteral("shape_mode"));
        QVERIFY(shape_mode != nullptr);
        shape_mode->setCurrentIndex(shape_mode->findData(QStringLiteral("pixel")));
        emit shape_mode->activated(shape_mode->currentIndex());
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
            // (PC3 C-b, the user's decision 5: 線の編集（制御点） by Photoshop's name)
            {"act_vector", "部分選択ツール", "Shift+Y", "ペンの線を選んで制御点を動かす（Alt+クリックで点を足す、Delete で消す、Shift+クリックで 2 本目）", true},
            {"act_vector_join", "選んだ 2 本の線をつなぐ", "", "", false},
            {"act_vector_cut", "クリックした所で線を切る", "", "オンの間、線をクリックするとそこで 2 本に分かれます", true},
            {"act_vector_colour", "選んだ線を描画色にする", "", "", false},  // (PC4 E-0)
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
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("先に「部分選択ツール」")));  // (PC3 C-b: its Photoshop name)
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
        // D (PC4 E-0): Genko's black and white
        s.trigger("act_default_colours");
        QCOMPARE(s.brush().rgb(), (app::Rgb{20, 20, 20}));
        QCOMPARE(s.colours().sub_rgb(), (app::Rgb{255, 255, 255}));
        // the 描画色 as floats (PC4 E-0): its 8-bit picture the nearest; swapped with the 背景色; an 8-bit choice leaves them
        s.colours().choose_precise({0.5, 0.25, 1.0 / 3.0});
        QCOMPARE(s.brush().rgb(), (app::Rgb{128, 64, 85}));
        QVERIFY(s.colours().is_precise());
        QCOMPARE(s.colours().foreground()[0], 0.5);
        QCOMPARE(s.colours().foreground_json("u16")["values"][1].get<double>(), std::round(0.25 * 65535) / 65535);
        QCOMPARE(s.colours().foreground_json("f32")["values"][2].get<double>(), static_cast<double>(static_cast<float>(1.0 / 3.0)));
        s.trigger("act_swap_colour");
        QVERIFY(!s.colours().is_precise());
        QCOMPARE(s.colours().background()[2], 1.0 / 3.0);
        s.trigger("act_swap_colour");
        QVERIFY(s.colours().is_precise());
        s.colours().choose(app::Rgb{10, 20, 30});
        QVERIFY(!s.colours().is_precise());
        QCOMPARE(s.colours().foreground()[0], 10.0 / 255.0);
        // (the same 8-bit colour chosen again — in the panel, or by the brush panel as a swatch or the picker sets it —
        // lets the floats go too)
        s.colours().choose_precise({0.5, 0.25, 1.0 / 3.0});
        s.colours().choose(app::Rgb{128, 64, 85});
        QVERIFY(!s.colours().is_precise());
        QCOMPARE(s.colours().foreground()[1], 64.0 / 255.0);
        s.colours().choose_precise({0.5, 0.25, 1.0 / 3.0});
        s.brush().set_colour({128, 64, 85});
        QVERIFY(!s.colours().is_precise());
        // ] and [: the pen's size one step up and down, or the eraser's
        s.window->choose_tool(QStringLiteral("pen"));
        const double width = s.canvas()->brush_width_mm;
        s.trigger("act_thicker");
        QVERIFY(s.canvas()->brush_width_mm > width);
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("ブラシサイズ ")));
        const double wider = s.canvas()->brush_width_mm;
        s.trigger("act_thinner");
        QVERIFY(s.canvas()->brush_width_mm < wider);
        s.window->choose_tool(QStringLiteral("eraser"));
        const double eraser = s.canvas()->eraser_mm;
        s.trigger("act_thicker");
        QVERIFY(s.canvas()->eraser_mm > eraser);
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("ブラシサイズ（消しゴムツール）")));
        auto* box = s.window->findChild<QDoubleSpinBox*>(QStringLiteral("eraser_size"));
        QCOMPARE(box->value(), s.canvas()->eraser_mm);
        s.trigger("act_thinner");
        QCOMPARE(s.canvas()->eraser_mm, 2.0);
        // 選んだ点を細く without a point chosen: said how
        s.trigger("act_point_thinner");
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("先に")));
    }

    // The drawing tools' groups: a palette button whose group has other tools offers them on a long press and a right
    // click (the eraser's: マジック消しゴムツール), a tool alone none; the ツール menu has each group's tools once; the new
    // panels' frames are in ウィンドウ, hidden; スウォッチ opens the カラー panel's tab.
    void theToolGroupsAndThePanels() {
        Studio s;
        QToolBar* palette = s.window->tool_palette();
        auto* eraser = qobject_cast<QToolButton*>(palette->widgetForAction(s.window->action(QStringLiteral("act_eraser"))));
        QVERIFY(eraser != nullptr && eraser->menu() != nullptr);
        QCOMPARE(eraser->popupMode(), QToolButton::DelayedPopup);
        QCOMPARE(eraser->menu()->actions(), (QList<QAction*>{s.window->action(QStringLiteral("act_eraser")), s.window->action(QStringLiteral("act_magic_eraser"))}));
        QCOMPARE(eraser->contextMenuPolicy(), Qt::CustomContextMenu);
        auto* picker = qobject_cast<QToolButton*>(palette->widgetForAction(s.window->action(QStringLiteral("act_picker"))));
        QVERIFY(picker != nullptr && picker->menu() == nullptr);
        auto* fill = qobject_cast<QToolButton*>(palette->widgetForAction(s.window->action(QStringLiteral("act_fill"))));
        QVERIFY(fill != nullptr && fill->menu() != nullptr);
        QCOMPARE(fill->menu()->actions().size(), 3);  // (塗りつぶしツール, 囲って塗る, グラデーションツール)
        QCOMPARE(s.window->tool_group_of(s.window->action(QStringLiteral("act_blend"))),
                 (std::vector<QAction*>{s.window->action(QStringLiteral("act_blend"))}));  // (ぼかし: 色混ぜ until E5's)
        // each tool once in the ツール menu
        QMenu* tools = nullptr;
        for (QAction* top : s.window->menuBar()->actions())
            if (top->text() == QStringLiteral("ツール")) tools = top->menu();
        QVERIFY(tools != nullptr);
        for (const auto& group : app::MainWindow::paint_tool_groups()) {
            for (const char* name : group.tools) {
                QAction* act = s.window->action(QString::fromLatin1(name));
                if (act == nullptr) continue;
                QVERIFY2(s.menus_of(act).contains(QStringLiteral("ツール")), name);
            }
        }
        QCOMPARE(tools->actions().count(s.window->action(QStringLiteral("act_magic_eraser"))), 1);
        // the panels' frames
        for (const char* title : {"ブラシ設定", "ブラシ", "パターン", "スタイル", "ツールプリセット", "グラデーション"}) {
            QDockWidget* dock = nullptr;
            for (QDockWidget* d : s.window->findChildren<QDockWidget*>())
                if (d->windowTitle() == QString::fromUtf8(title)) dock = d;
            QVERIFY2(dock != nullptr && !dock->isVisible(), title);
            QVERIFY2(s.menus_of(dock->toggleViewAction()).contains(QStringLiteral("ウィンドウ")), title);
        }
        // スウォッチ
        s.window->colours()->tabs->setCurrentIndex(2);
        s.trigger("act_swatches");
        QCOMPARE(s.window->colours()->tabs->currentIndex(), 0);
        QCOMPARE(s.window->colours()->tabs->tabText(0), QStringLiteral("スウォッチ"));
        QVERIFY(s.menus_of(s.window->action(QStringLiteral("act_swatches"))).contains(QStringLiteral("ウィンドウ")));
    }
};

QTEST_MAIN(TestGuiPaint)
#include "test_gui_paint.moc"
