// PC3 C-d (SPEC.md COLOR-07「移動・整列」, the user's decision 5: Photoshop's names) on the offscreen platform: レイヤー →
// 重ね順 ▸ (最前面へ Ctrl+Shift+], 最背面へ Ctrl+Shift+[, 逆順; 前面へ・背面へ among siblings now), 整列 ▸ and 分布 ▸ with
// Genko's bases, レイヤーをリンク, リンクしたレイヤーを選択, レイヤーをロック…, 表示 → スナップ (Ctrl+Shift+;) and スナップ先 ▸ with
// their words, keys and places; each command's one op on the layers chosen (and the line chosen); the move tool's drag
// (transform_layers' move by whole working pixels, the linked layer along), its Ctrl+click (自動選択: the layer showing
// there drawn on) and バウンディングボックス (its □ starts 自由変形); snapping to a guide while dragging (the line drawn,
// Ctrl lets it go), スナップ先 ▸ すべて・なし and the grid's one switch.

#include <QtTest>

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>

#include <cmath>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/layer_bounds.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

const double kPx = 60.0 / render::mm_to_px(60.0, 200);  // (a working pixel of the 60 mm page, mm: the page drawn at 200 dpi)

QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// A paint layer with a block from (x0, y0) to (x1, y1) mm (its working pixels: 200 dpi).
void add_block(core::Document& doc, const std::string& id, double x0, double y0, double x1, double y1) {
    auto& page = doc.edit_page(0);
    core::Layer paint;
    paint.id = id;
    paint.kind = core::LayerKind::Raster;
    paint.panel_clip = false;
    const render::Image blank = render::raster::ensure_raster(page, paint);
    std::string bytes = blank.tobytes();
    const double px = 200 / 25.4;
    for (int y = 0; y < blank.height(); ++y) {
        for (int x = 0; x < blank.width(); ++x) {
            if (x < x0 * px || x >= x1 * px || y < y0 * px || y >= y1 * px) continue;
            const std::size_t i = (std::size_t(y) * std::size_t(blank.width()) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(40);
            bytes[i + 1] = static_cast<char>(90);
            bytes[i + 2] = static_cast<char>(160);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    render::raster::save_raster(page, paint, render::Image::frombytes("RGBA", blank.size(), bytes));
    page.layers.push_back(std::move(paint));
}

// A 60 × 60 mm colour page without panels: blocks "a" (20..30 mm square at 20, 20) and "b" (40..46 × 10..16), and a line of
// dialogue "l1".
core::Document book_doc() {
    auto doc = core::new_episode("整列", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    doc.edit_page(0).frames.clear();
    add_block(doc, "a", 20, 20, 30, 30);
    add_block(doc, "b", 40, 10, 46, 16);
    return core::CommandBus().apply(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "ね"}, {"x_mm", 5}, {"y_mm", 40}, {"w_mm", 8}, {"h_mm", 6}, {"id", "l1"}}}),
                                    core::Actor("human:tester"))
        .doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;  // every op applied, in order
    Studio() {
        (void)gui_test::config_folder();
        {  // (the switches kept on this computer as they are at first: each test its own)
            const auto store = app::settings();
            for (const char* key : {"snap", "move", "align", "guides/grid_snap"}) store->remove(QString::fromLatin1(key));
        }
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
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    QMenu* menu_at(const QString& path) const {
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
    std::vector<std::string> order() const {
        std::vector<std::string> out;
        for (const core::Layer& l : page().layers) out.push_back(l.id);
        return out;
    }
    std::array<long, 4> box(const std::string& id) const {
        const auto b = *render::layer_bounds::of_layer(session->document(), page(), id);
        return {std::lround(b[0] / kPx), std::lround(b[1] / kPx), std::lround(b[2] / kPx), std::lround(b[3] / kPx)};
    }
    // The layers chosen in the panel (the first drawn on).
    void choose(const std::vector<std::string>& ids) const {
        window->set_target_layer(ids.front());
        window->layer_panel()->select_ids(ids);
    }
    // The page's layers' boxes drawn for snapping (on a worker).
    bool boxes_ready() const {
        return gui_test::wait_for([this] { return !window->layer_snap_boxes().empty(); }, 20000);
    }
};

void send(QWidget* widget, QEvent::Type type, const QPointF& at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, at, widget->mapToGlobal(at), button, held, modifiers);
    QApplication::sendEvent(widget, &event);
}

bool whole(double mm) { return std::fabs(mm / kPx - std::round(mm / kPx)) < 1e-6; }

}  // namespace

class TestGuiAlign : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The commands: their words, keys and places.
    void theCommands() {
        Studio studio;
        struct Expected {
            const char* attribute;
            const char* text;
            const char* key;
            const char* place;
        };
        const std::vector<Expected> expected{
            {"act_layer_front", "最前面へ", "Ctrl+Shift+]", "レイヤー/重ね順"},
            {"act_layer_up", "前面へ", "Ctrl+]", "レイヤー/重ね順"},
            {"act_layer_down", "背面へ", "Ctrl+[", "レイヤー/重ね順"},
            {"act_layer_back", "最背面へ", "Ctrl+Shift+[", "レイヤー/重ね順"},
            {"act_layer_reverse", "逆順", "", "レイヤー/重ね順"},
            {"act_align_top", "上端", "", "レイヤー/整列"},
            {"act_align_vcenter", "垂直方向中央", "", "レイヤー/整列"},
            {"act_align_bottom", "下端", "", "レイヤー/整列"},
            {"act_align_left", "左端", "", "レイヤー/整列"},
            {"act_align_hcenter", "水平方向中央", "", "レイヤー/整列"},
            {"act_align_right", "右端", "", "レイヤー/整列"},
            {"act_align_to_auto", "選んだもの・選択範囲・用紙に", "", "レイヤー/整列"},
            {"act_align_to_trim", "仕上がりに", "", "レイヤー/整列"},
            {"act_align_to_frame", "基本枠に", "", "レイヤー/整列"},
            {"act_align_to_panel", "選んだコマに", "", "レイヤー/整列"},
            {"act_distribute_top", "上端", "", "レイヤー/分布"},
            {"act_distribute_vcenter", "垂直方向中央", "", "レイヤー/分布"},
            {"act_distribute_bottom", "下端", "", "レイヤー/分布"},
            {"act_distribute_left", "左端", "", "レイヤー/分布"},
            {"act_distribute_hcenter", "水平方向中央", "", "レイヤー/分布"},
            {"act_distribute_right", "右端", "", "レイヤー/分布"},
            {"act_distribute_horizontal", "水平方向", "", "レイヤー/分布"},
            {"act_distribute_vertical", "垂直方向", "", "レイヤー/分布"},
            {"act_link_layers", "レイヤーをリンク", "", "レイヤー"},
            {"act_select_linked", "リンクしたレイヤーを選択", "", "レイヤー"},
            {"act_lock_layers", "レイヤーをロック…", "", "レイヤー"},
            {"act_snap_on", "スナップ", "Ctrl+Shift+;", "表示"},
            {"act_snap_guides", "ガイド", "", "表示/スナップ先"},
            {"act_snap_grid", "グリッド", "", "表示/スナップ先"},
            {"act_snap_layers", "レイヤー", "", "表示/スナップ先"},
            {"act_snap_bounds", "ドキュメントの境界", "", "表示/スナップ先"},
            {"act_snap_all", "すべて", "", "表示/スナップ先"},
            {"act_snap_none", "なし", "", "表示/スナップ先"},
        };
        for (const Expected& e : expected) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), s(e.key));
            QMenu* menu = studio.menu_at(QString::fromUtf8(e.place));
            QVERIFY2(menu != nullptr && menu->actions().contains(a), e.attribute);
        }
        // 重ね順: Photoshop's order
        const QList<QAction*> arrange = studio.menu_at(QStringLiteral("レイヤー/重ね順"))->actions();
        const auto at = [&](const char* name) { return arrange.indexOf(studio.window->action(QString::fromLatin1(name))); };
        QVERIFY(at("act_layer_front") < at("act_layer_up") && at("act_layer_up") < at("act_layer_down") && at("act_layer_down") < at("act_layer_back") &&
                at("act_layer_back") < at("act_layer_reverse"));
        // the switches: snapping on, to the guides, the layers and the paper; the move tool's own off
        for (const char* name : {"act_snap_on", "act_snap_guides", "act_snap_layers", "act_snap_bounds"}) QVERIFY2(studio.window->action(name)->isChecked(), name);
        QVERIFY(studio.canvas()->snap_on && studio.canvas()->snap_guides && studio.canvas()->snap_layers && studio.canvas()->snap_bounds);
        QVERIFY(!studio.window->action("act_move_auto_select")->isChecked() && !studio.window->action("act_move_show_box")->isChecked());
        QVERIFY(studio.window->action("act_align_to_auto")->isChecked());
    }

    // 重ね順: one arrange_layers each, among siblings; nothing moving sends nothing.
    void arrangeByTheMenu() {
        Studio studio;
        const auto before = studio.order();
        studio.choose({"a"});
        studio.trigger("act_layer_front");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("arrange_layers"));
        QCOMPARE(studio.ops[0]["to"], Json("front"));
        QCOMPARE(studio.order().back(), std::string("a"));
        studio.trigger("act_layer_front");  // (the front already: nothing)
        QCOMPARE(studio.ops.size(), std::size_t(1));
        studio.trigger("act_layer_down");  // (背面へ: arrange_layers now)
        QCOMPARE(studio.ops.size(), std::size_t(2));
        QCOMPARE(studio.ops[1]["to"], Json("backward"));
        studio.trigger("act_layer_back");
        QCOMPARE(studio.order().front(), std::string("a"));
        studio.trigger("act_undo");
        studio.trigger("act_undo");
        studio.trigger("act_undo");
        QCOMPARE(studio.order(), before);
        studio.choose({"a", "b"});
        studio.trigger("act_layer_reverse");
        QCOMPARE(studio.ops.back()["op"], Json("arrange_layers"));
        QCOMPARE(studio.ops.back()["to"], Json("reverse"));
        const auto now = studio.order();
        QVERIFY(std::find(now.begin(), now.end(), "b") < std::find(now.begin(), now.end(), "a"));
    }

    // 整列 and 分布: one op on the layers chosen and the line chosen; the base chosen in the menu.
    void alignAndDistributeByTheMenu() {
        Studio studio;
        studio.choose({"a", "b"});
        studio.trigger("act_align_top");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("align_layers"));
        QCOMPARE(studio.ops[0]["edge"], Json("top"));
        QCOMPARE(studio.ops[0]["to"], Json("auto"));
        QCOMPARE(studio.box("a")[1], studio.box("b")[1]);
        // with the line chosen: it too
        studio.canvas()->selected_line_id = "l1";
        studio.trigger("act_align_left");
        QCOMPARE(studio.ops.back()["lines"], Json::array({"l1"}));
        QCOMPARE(studio.box("a")[0], std::lround(5 / kPx));
        QVERIFY(std::fabs(studio.session->document().story.front().x_mm.value() - 5) < 1e-9);
        // a base of Genko's own: the trim
        studio.trigger("act_align_to_trim");
        QVERIFY(studio.window->action("act_align_to_trim")->isChecked() && !studio.window->action("act_align_to_auto")->isChecked());
        studio.trigger("act_align_right");
        QCOMPARE(studio.ops.back()["to"], Json("trim"));
        // 選んだコマに without a panel chosen: said, nothing sent
        const std::size_t sent = studio.ops.size();
        studio.trigger("act_align_to_panel");
        studio.trigger("act_align_top");
        QCOMPARE(studio.ops.size(), sent);
        QVERIFY(studio.window->last_notice().contains(QStringLiteral("コマ")));
        studio.trigger("act_align_to_auto");
        // 分布: three needed (two layers and the line here)
        studio.trigger("act_distribute_horizontal");
        QCOMPARE(studio.ops.back()["op"], Json("distribute_layers"));
        QCOMPARE(studio.ops.back()["by"], Json("horizontal"));
        studio.canvas()->selected_line_id.reset();
        const std::size_t three = studio.ops.size();
        studio.trigger("act_distribute_left");
        QCOMPARE(studio.ops.size(), three);  // (two alone: said, nothing sent)
    }

    // リンク: link_layers on the two chosen; リンクしたレイヤーを選択 chooses them again; the panel marks them.
    void linksAndLocks() {
        Studio studio;
        studio.choose({"a", "b"});
        studio.trigger("act_link_layers");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("link_layers"));
        for (const core::Layer& l : studio.page().layers)
            if (l.id == "a" || l.id == "b") QCOMPARE(l.link, std::optional<std::string>("link-1"));
        studio.choose({"a"});
        studio.trigger("act_select_linked");
        std::vector<std::string> chosen = studio.window->layer_panel()->selected_ids();
        std::sort(chosen.begin(), chosen.end());
        QCOMPARE(chosen, (std::vector<std::string>{"a", "b"}));
        bool marked = false;
        for (int row = 0; row < studio.window->layer_panel()->list()->count(); ++row)
            marked = marked || studio.window->layer_panel()->list()->item(row)->text().contains(QStringLiteral("リンク 1"));
        QVERIFY(marked);
        // レイヤーをロック…: 位置 on both
        studio.answers.responder->exec = [](QDialog* dialog) {
            dialog->findChild<QCheckBox*>(QStringLiteral("lock_position"))->setChecked(true);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_lock_layers");
        QCOMPARE(studio.ops.back()["op"], Json("set_layers"));
        QCOMPARE(studio.ops.back()["lock_position"], Json(true));
        for (const core::Layer& l : studio.page().layers)
            if (l.id == "a" || l.id == "b") QVERIFY(l.lock_position && !l.locked && !l.lock_alpha);
        // (a position locked: the move tool's drag refused, the book as it was)
        const auto a = studio.box("a");
        studio.trigger("act_move");
        studio.choose({"a"});
        studio.canvas()->snap_on = false;
        inject::mouse_stroke(studio.canvas(), {QPointF(25, 25), QPointF(27, 25), QPointF(30, 26)});
        QCOMPARE(studio.box("a"), a);
        QVERIFY(studio.window->last_error().contains(QStringLiteral("位置をロック")));
    }

    // The move tool: the layers chosen and the linked one by whole working pixels (one transform_layers); Ctrl+click picks
    // the layer showing there; the bounding box's □ starts 自由変形.
    void theMoveTool() {
        Studio studio;
        studio.trigger("act_snap_on");  // (off: the moves as dragged; on again at the end, as it is kept)
        QVERIFY(!studio.canvas()->snap_on);
        studio.choose({"a", "b"});
        studio.trigger("act_link_layers");
        studio.choose({"a"});
        studio.trigger("act_move");
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("move"));
        const auto a = studio.box("a"), b = studio.box("b");
        inject::mouse_stroke(studio.canvas(), {QPointF(25, 25), QPointF(27, 25), QPointF(30.3, 27.1)});
        QCOMPARE(studio.ops.back()["op"], Json("transform_layers"));
        const Json move = studio.ops.back()["move"];
        QVERIFY(whole(move[0].get<double>()) && whole(move[1].get<double>()));
        const long dx = std::lround(move[0].get<double>() / kPx), dy = std::lround(move[1].get<double>() / kPx);
        QCOMPARE(dx, std::lround(5.3 / kPx));
        QCOMPARE(studio.box("a"), (std::array<long, 4>{a[0] + dx, a[1] + dy, a[2] + dx, a[3] + dy}));
        QCOMPARE(studio.box("b"), (std::array<long, 4>{b[0] + dx, b[1] + dy, b[2] + dx, b[3] + dy}));  // (linked: along)
        // Ctrl+click: the layer showing there drawn on (自動選択)
        const auto bb = studio.box("b");
        const QPointF on_b((bb[0] + bb[2]) / 2.0 * kPx, (bb[1] + bb[3]) / 2.0 * kPx);
        studio.window->set_target_layer(studio.page().layers.front().id);
        const std::size_t sent = studio.ops.size();
        inject::click_mm(studio.canvas(), on_b, Qt::ControlModifier);
        QCOMPARE(studio.window->target_layer()->id, std::string("b"));
        QCOMPARE(studio.ops.size(), sent);  // (a click moves nothing)
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("move"));  // (Ctrl did not take the select tool)
        // 自動選択 on: a plain click picks
        studio.window->action(QStringLiteral("act_move_auto_select"))->trigger();
        QVERIFY(studio.canvas()->auto_select);
        const auto aa = studio.box("a");
        inject::click_mm(studio.canvas(), QPointF((aa[0] + aa[2]) / 2.0 * kPx, (aa[1] + aa[3]) / 2.0 * kPx));
        QCOMPARE(studio.window->target_layer()->id, std::string("a"));
        studio.window->action(QStringLiteral("act_move_auto_select"))->trigger();
        // バウンディングボックスを表示: its □ grabbed starts 自由変形 over what the move tool moves
        studio.window->action(QStringLiteral("act_move_show_box"))->trigger();
        QVERIFY(studio.canvas()->show_move_box);
        QVERIFY(studio.boxes_ready());
        QVERIFY(gui_test::wait_for([&] { return studio.canvas()->moving_box && studio.canvas()->moving_box().has_value(); }));
        const auto mb = *studio.canvas()->moving_box();  // (a's and the linked b's)
        QCOMPARE(std::lround(mb[2] / kPx), std::max(studio.box("a")[2], studio.box("b")[2]));
        const QPointF corner(mb[2], mb[3]);
        send(studio.canvas(), QEvent::MouseButtonPress, studio.canvas()->to_widget(corner));
        QVERIFY(studio.canvas()->transforming());
        send(studio.canvas(), QEvent::MouseButtonRelease, studio.canvas()->to_widget(corner));
        QTest::keyClick(studio.canvas(), Qt::Key_Escape);
        QVERIFY(!studio.canvas()->transforming());
        studio.trigger("act_move_show_box");
        studio.trigger("act_snap_on");
        QVERIFY(studio.canvas()->snap_on && !studio.canvas()->show_move_box && !studio.canvas()->auto_select);
    }

    // Snapping: a drag near a guide snaps to it (its line drawn while dragging); Ctrl held: not; スナップ先's switches.
    void snapToAGuide() {
        Studio studio;
        studio.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 45.0}}}));
        studio.window->action(QStringLiteral("act_snap_layers"))->trigger();  // (the guide alone, the paper's edges)
        studio.window->action(QStringLiteral("act_snap_bounds"))->trigger();
        QVERIFY(!studio.canvas()->snap_layers && !studio.canvas()->snap_bounds);
        studio.choose({"a"});
        studio.trigger("act_move");
        QVERIFY(studio.boxes_ready());
        const auto a_box = studio.box("a");
        auto* canvas = studio.canvas();
        const double reach = 8.0 / canvas->scale();
        QVERIFY(reach < 1.0);
        // (a's right edge, 30 mm, dragged 14.6 mm: 44.6, within the reach of the guide at 45)
        send(canvas, QEvent::MouseButtonPress, canvas->to_widget(QPointF(25, 25)));
        send(canvas, QEvent::MouseMove, canvas->to_widget(QPointF(32, 25)));
        send(canvas, QEvent::MouseMove, canvas->to_widget(QPointF(39.6, 25)));
        QVERIFY(!canvas->snap_lines().empty());
        QVERIFY(canvas->snap_lines().front().vertical && canvas->snap_lines().front().pos == 45.0);
        send(canvas, QEvent::MouseButtonRelease, canvas->to_widget(QPointF(39.6, 25)));
        QVERIFY(canvas->snap_lines().empty());  // (shown while the drag lasts)
        QCOMPARE(studio.ops.back()["op"], Json("transform_layers"));
        QCOMPARE(std::lround(studio.ops.back()["move"][0].get<double>() / kPx), std::lround(45.0 / kPx) - a_box[2]);  // (its right edge on the pixel nearest the guide)
        studio.trigger("act_undo");
        // Ctrl held: as dragged
        send(canvas, QEvent::MouseButtonPress, canvas->to_widget(QPointF(25, 25)));
        send(canvas, QEvent::MouseMove, canvas->to_widget(QPointF(32, 25)), Qt::ControlModifier);
        send(canvas, QEvent::MouseMove, canvas->to_widget(QPointF(39.6, 25)), Qt::ControlModifier);
        QVERIFY(canvas->snap_lines().empty());
        send(canvas, QEvent::MouseButtonRelease, canvas->to_widget(QPointF(39.6, 25)), Qt::ControlModifier);
        QCOMPARE(std::lround(studio.ops.back()["move"][0].get<double>() / kPx), std::lround(14.6 / kPx));
        // スナップ先 ▸ なし, すべて; the grid's one switch in two places
        studio.trigger("act_snap_none");
        QVERIFY(!canvas->snap_guides && !canvas->snap_layers && !canvas->snap_bounds && !canvas->grid_snap);
        QVERIFY(!studio.window->action("act_grid_snap")->isChecked());
        studio.trigger("act_snap_all");
        QVERIFY(canvas->snap_guides && canvas->snap_layers && canvas->snap_bounds && canvas->grid_snap);
        QVERIFY(studio.window->action("act_grid_snap")->isChecked() && studio.window->action("act_snap_grid")->isChecked());
        QCOMPARE(canvas->grid_point(1.2, 1.2), QPointF(0, 0));  // (the grid of 5 mm)
        studio.trigger("act_snap_on");  // (スナップ off: nothing snaps, the grid neither)
        QVERIFY(!canvas->snap_on);
        QCOMPARE(canvas->grid_point(1.2, 1.2), QPointF(1.2, 1.2));
        studio.trigger("act_snap_on");
        studio.trigger("act_grid_snap");
        QVERIFY(!studio.window->action("act_snap_grid")->isChecked() && !canvas->grid_snap);
    }
};

QTEST_MAIN(TestGuiAlign)
#include "test_gui_align.moc"
