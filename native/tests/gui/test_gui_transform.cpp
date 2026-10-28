// PC3 C-c (SPEC.md COLOR-07「変形」, the user's decision 5: Photoshop's names) on the offscreen platform: 編集's 自由変形
// (Ctrl+T), 変形 ▸, パペットワープ, 遠近法ワープ, コンテンツに応じて拡大・縮小…, ゆがみ… and 台詞's ワープテキスト… with their words,
// keys and places; the free transform box previews each drag (the book untouched) and Enter makes one op (one undo), Esc
// lets it go; 再実行 sends the map again; the right-angle turns; the warp's box split crosswise; puppet pins put and pulled;
// the liquify window's strokes, the content-aware scale's window and the warp text's window as one op each; the liquify
// tool's new name (前方ワープツール) keeping what was chosen under its old one.

#include <QtTest>

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolBar>

#include <cmath>

#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/story_panel.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// A 60 × 60 mm colour page: a line on the ink layer from (10, 10) to (30, 30), and a paint layer with a block from 20 to
// 40 mm (its working pixels: 200 dpi).
core::Document book_doc() {
    auto doc = core::new_episode("変形", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                         {"points", Json::array({Json::array({10.0, 10.0, 0.7}), Json::array({30.0, 30.0, 0.7})})},
                                                         {"stabilize", 0}}}),
                                   core::Actor("human:tester"))
              .doc;
    auto& page = doc.edit_page(0);
    core::Layer paint;
    paint.id = "paint";
    paint.kind = core::LayerKind::Raster;
    paint.panel_clip = false;
    const render::Image blank = render::raster::ensure_raster(page, paint);
    std::string bytes = blank.tobytes();
    const double px = 200 / 25.4;
    for (int y = 0; y < blank.height(); ++y) {
        for (int x = 0; x < blank.width(); ++x) {
            if (x < 20 * px || x >= 40 * px || y < 20 * px || y >= 40 * px) continue;
            const std::size_t i = (std::size_t(y) * std::size_t(blank.width()) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>((x * 7) % 256);
            bytes[i + 1] = static_cast<char>((y * 5) % 256);
            bytes[i + 2] = static_cast<char>(80);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    render::raster::save_raster(page, paint, render::Image::frombytes("RGBA", blank.size(), bytes));
    page.layers.push_back(std::move(paint));
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::string ink;
    gui_test::Answers answers;
    std::vector<Json> ops;  // every op applied, in order
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
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    // the first line's points on the ink layer: of the book, or of the page the canvas shows (a preview)
    static std::vector<core::PointF> line_of(const core::Page& page, const std::string& id) {
        for (const auto& l : page.layers)
            if (l.id == id && l.stroke_count() > 0) return l.strokes->items[0]->points;
        return {};
    }
    std::vector<core::PointF> line() const { return line_of(page(), ink); }
    std::vector<core::PointF> shown() const { return canvas()->page() != nullptr ? line_of(*canvas()->page(), ink) : std::vector<core::PointF>{}; }
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
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
};

bool near(const core::PointF& a, double x, double y, double tolerance = 0.1) { return std::fabs(a.x - x) < tolerance && std::fabs(a.y - y) < tolerance; }

void send_mouse(QWidget* widget, QEvent::Type type, const QPointF& at) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, at, widget->mapToGlobal(at), button, held, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

}  // namespace

class TestGuiTransform : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The commands: their words, keys and places (編集, 編集 → 変形, ページ → 台詞); the liquify tool's new name.
    void theCommands() {
        Studio studio;
        struct Expected {
            const char* attribute;
            const char* text;
            const char* key;
            const char* place;
        };
        const std::vector<Expected> expected{
            {"act_free_transform", "自由変形", "Ctrl+T", "編集"},
            {"act_content_aware_scale", "コンテンツに応じて拡大・縮小…", "Ctrl+Alt+Shift+C", "編集"},
            {"act_puppet_warp", "パペットワープ", "", "編集"},
            {"act_perspective_warp", "遠近法ワープ", "", "編集"},
            {"act_liquify_filter", "ゆがみ…", "Ctrl+Shift+X", "フィルター"},  // (PC4 節 D: moved to フィルター, PC3 節 A 5 章's promise)
            {"act_transform_again", "再実行", "", "編集/変形"},
            {"act_transform_scale", "拡大・縮小", "", "編集/変形"},
            {"act_transform_rotate", "回転", "", "編集/変形"},
            {"act_transform_skew", "ゆがみ", "", "編集/変形"},
            {"act_transform_perspective", "遠近法", "", "編集/変形"},
            {"act_split_warp_h", "水平方向に分割ワープ", "", "編集/変形"},
            {"act_split_warp_v", "垂直方向に分割ワープ", "", "編集/変形"},
            {"act_split_warp_cross", "交差方向に分割ワープ", "", "編集/変形"},
            {"act_remove_warp_split", "ワープ分割を削除", "", "編集/変形"},
            {"act_transform_rotate_180", "180° 回転", "", "編集/変形"},
            {"act_transform_rotate_90cw", "90° 回転（時計回り）", "", "編集/変形"},
            {"act_transform_rotate_90ccw", "90° 回転（反時計回り）", "", "編集/変形"},
            {"act_warp_text", "ワープテキスト…", "", "ページ/台詞"},
        };
        for (const Expected& e : expected) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), s(e.key));
            QMenu* menu = studio.menu_at(QString::fromUtf8(e.place));
            QVERIFY2(menu != nullptr && menu->actions().contains(a), e.attribute);
        }
        // 編集: Photoshop's order (コンテンツに応じて拡大・縮小… パペットワープ 遠近法ワープ 自由変形, then 変形 ▸)
        const QList<QAction*> edit = studio.menu_at(QStringLiteral("編集"))->actions();
        const auto at = [&](const char* name) { return edit.indexOf(studio.window->action(QString::fromLatin1(name))); };
        QVERIFY(at("act_content_aware_scale") < at("act_puppet_warp") && at("act_puppet_warp") < at("act_perspective_warp") &&
                at("act_perspective_warp") < at("act_free_transform"));
        // 編集 → 変形: 再実行 first, the flips last before the selection's own
        const QList<QAction*> transform = studio.menu_at(QStringLiteral("編集/変形"))->actions();
        QCOMPARE(transform.first(), studio.window->action(QStringLiteral("act_transform_again")));
        QVERIFY(transform.indexOf(studio.window->action(QStringLiteral("act_transform_rotate_90ccw"))) < transform.indexOf(studio.window->action(QStringLiteral("act_flip_h"))));
        // the liquify tool by its new name; what was chosen under the old one stays
        QCOMPARE(studio.window->action(QStringLiteral("act_liquify"))->text(), QStringLiteral("前方ワープツール"));
        bool kept = false;
        for (auto [it, end] = app::preferences::renamed().equal_range(QStringLiteral("前方ワープツール")); it != end; ++it)
            kept = kept || it->second == QStringLiteral("ゆがみ（指で押す）");
        QVERIFY(kept);
    }

    // Ctrl+T: the box over the line; a drag inside moves it on the canvas only (a preview); Enter makes one op, undone in
    // one; Esc lets a drag go; 再実行 moves it again.
    void freeTransformPreviewEnterEscAndAgain() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        const auto before = studio.line();
        QCOMPARE(before.size(), std::size_t(2));
        studio.trigger("act_free_transform");
        QVERIFY(canvas->transforming());
        QCOMPARE(canvas->transform_mode(), QStringLiteral("free"));
        const auto box = canvas->transform_box();
        QVERIFY2(box[0] < 10 && box[0] > 9 && box[2] > 30 && box[2] < 31, qPrintable(QStringLiteral("%1 %2").arg(box[0]).arg(box[2])));
        auto* bar = studio.window->findChild<QToolBar*>(QStringLiteral("transform_bar"));
        QVERIFY(bar != nullptr && bar->isVisible());
        inject::mouse_stroke(canvas, {QPointF(20, 20), QPointF(23, 21), QPointF(25, 22)});
        QVERIFY(canvas->transforming());
        QVERIFY(studio.ops.empty());  // (the book as it was)
        QVERIFY(near(studio.line().front(), before.front().x, before.front().y, 1e-9));
        QVERIFY2(!studio.shown().empty() && near(studio.shown().front(), before.front().x + 5, before.front().y + 2),
                 qPrintable(QStringLiteral("%1 %2").arg(studio.shown().empty() ? -1 : studio.shown().front().x).arg(studio.shown().empty() ? -1 : studio.shown().front().y)));
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!canvas->transforming());
        QVERIFY(!bar->isVisible());
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("transform_layers"));
        QCOMPARE(studio.ops[0]["ids"], Json::array({studio.ink}));
        QVERIFY(studio.ops[0].contains("matrix"));
        QVERIFY(near(studio.line().front(), before.front().x + 5, before.front().y + 2));
        studio.trigger("act_undo");
        QVERIFY(near(studio.line().front(), before.front().x, before.front().y, 1e-6));
        studio.trigger("act_redo");
        QVERIFY(near(studio.line().front(), before.front().x + 5, before.front().y + 2));
        // Esc: the drag let go, nothing made
        const auto moved = studio.line();
        studio.trigger("act_free_transform");
        inject::mouse_stroke(canvas, {QPointF(25, 22), QPointF(30, 30)});
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!canvas->transforming());
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QVERIFY(near(studio.shown().front(), moved.front().x, moved.front().y, 1e-6));
        // 再実行: the same map again
        studio.trigger("act_transform_again");
        QCOMPARE(studio.ops.size(), std::size_t(2));
        QCOMPARE(studio.ops[1]["matrix"], studio.ops[0]["matrix"]);
        QVERIFY(near(studio.line().front(), before.front().x + 10, before.front().y + 4));
    }

    // 180° 回転 and 90° 回転: one op each (turn), about the box's middle; with a selection, within it.
    void rightAngleTurns() {
        Studio studio;
        studio.window->set_target_layer(studio.ink);
        const auto before = studio.line();
        studio.trigger("act_transform_rotate_180");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["turn"], Json("180"));
        // (the line from (10, 10) to (30, 30) about its middle: its ends swapped)
        QVERIFY(near(studio.line().front(), before.back().x, before.back().y, 0.1));
        studio.trigger("act_transform_rotate_90cw");
        QCOMPARE(studio.ops.back()["turn"], Json("90cw"));
        const auto turned = studio.line();
        QVERIFY(std::fabs(turned.front().x + turned.front().y - 40) < 0.2);  // (the other diagonal)
    }

    // Warp (no selection: the layer's box): a click marks where a split goes, 交差方向に分割ワープ splits the box there; Enter
    // makes one op with the mesh.
    void warpAndItsSplits() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_warp_mesh");
        QVERIFY(canvas->transforming());
        QCOMPARE(canvas->transform_mode(), QStringLiteral("warp"));
        QCOMPARE(canvas->transform_warp()["mesh"]["us"].size(), std::size_t(2));
        QVERIFY(studio.window->findChild<QComboBox*>(QStringLiteral("warp_style")) != nullptr);
        const auto box = canvas->transform_box();
        inject::click_mm(canvas, QPointF((box[0] + box[2]) / 2, (box[1] + box[3]) / 2));
        studio.trigger("act_split_warp_cross");
        QCOMPARE(canvas->transform_warp()["mesh"]["us"].size(), std::size_t(3));
        QCOMPARE(canvas->transform_warp()["mesh"]["vs"].size(), std::size_t(3));
        studio.trigger("act_remove_warp_split");  // (the split nearest the point: the one down it, first)
        QCOMPARE(canvas->transform_warp()["mesh"]["us"].size(), std::size_t(2));
        QCOMPARE(canvas->transform_warp()["mesh"]["vs"].size(), std::size_t(3));
        studio.trigger("act_remove_warp_split");  // (then the one across)
        QCOMPARE(canvas->transform_warp()["mesh"]["vs"].size(), std::size_t(2));
        studio.trigger("act_split_warp_v");
        QCOMPARE(canvas->transform_warp()["mesh"]["us"].size(), std::size_t(3));
        QCOMPARE(canvas->transform_warp()["mesh"]["vs"].size(), std::size_t(2));
        // a style chosen: the arc
        auto* style = studio.window->findChild<QComboBox*>(QStringLiteral("warp_style"));
        style->setCurrentIndex(style->findData(QStringLiteral("arc")));
        QCOMPARE(canvas->transform_warp()["style"], Json("arc"));
        QVERIFY(studio.ops.empty());
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["warp"]["style"], Json("arc"));
        QVERIFY(!canvas->transforming());
        // (a split asked for with no warp going on: a word)
        studio.trigger("act_split_warp_h");
        QCOMPARE(studio.window->last_notice(), QStringLiteral("ワープの最中に使います（編集 → 変形 → ワープ）"));
    }

    // Puppet Warp on the paint layer's block: clicks put pins, a pin pulled bends it (previewed), Alt+click takes one out;
    // Enter makes one op with the pins.
    void puppetPins() {
        Studio studio;
        auto* canvas = studio.canvas();
        studio.window->set_target_layer("paint");
        studio.trigger("act_puppet_warp");
        QVERIFY(canvas->transforming());
        QCOMPARE(canvas->transform_mode(), QStringLiteral("puppet"));
        inject::click_mm(canvas, QPointF(22, 22));
        inject::click_mm(canvas, QPointF(38, 22));
        inject::click_mm(canvas, QPointF(30, 38));
        QCOMPARE(canvas->puppet_pins().size(), std::size_t(3));
        inject::click_mm(canvas, QPointF(30, 38), Qt::AltModifier);
        QCOMPARE(canvas->puppet_pins().size(), std::size_t(2));
        inject::mouse_stroke(canvas, {QPointF(38, 22), QPointF(41, 21), QPointF(44, 20)});
        QCOMPARE(canvas->puppet_pins().size(), std::size_t(2));
        QVERIFY(std::fabs(canvas->puppet_pins()[1].second.x() - 44) < 0.2);
        QVERIFY(studio.ops.empty());
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(studio.ops.size(), std::size_t(1));
        const Json pins = studio.ops[0]["puppet"]["pins"];
        QCOMPARE(pins.size(), std::size_t(2));
        QVERIFY(std::fabs(pins[1]["dst"][0].get<double>() - 44) < 0.2);
        QCOMPARE(studio.ops[0]["puppet"]["mode"], Json("normal"));
    }

    // ゆがみ…: strokes drawn in its window make one liquify op (the picture pushed); nothing drawn makes none.
    void liquifyWindow() {
        Studio studio;
        studio.window->set_target_layer("paint");
        const std::string before = *studio.layer("paint")->raster_png;
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("liquify_dialog")) return int(QDialog::Rejected);
            QWidget* view = dialog->findChild<QWidget*>(QStringLiteral("liquify_view"));
            if (view == nullptr) return int(QDialog::Rejected);
            const double scale = view->width() / 60.0;  // (the page's mm on the window)
            send_mouse(view, QEvent::MouseButtonPress, QPointF(26 * scale, 30 * scale));
            for (int i = 1; i <= 6; ++i) send_mouse(view, QEvent::MouseMove, QPointF((26 + i) * scale, 30 * scale));
            send_mouse(view, QEvent::MouseButtonRelease, QPointF(33 * scale, 30 * scale));
            return int(QDialog::Accepted);
        };
        studio.trigger("act_liquify_filter");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("liquify"));
        QCOMPARE(studio.ops[0]["strokes"].size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["strokes"][0]["tool"], Json("forward_warp"));
        QVERIFY(*studio.layer("paint")->raster_png != before);
        studio.answers.responder->exec = [](QDialog* dialog) { return dialog->objectName() == QStringLiteral("liquify_dialog") ? int(QDialog::Accepted) : 0; };
        studio.trigger("act_liquify_filter");
        QCOMPARE(studio.ops.size(), std::size_t(1));
    }

    // コンテンツに応じて拡大・縮小…: its window's width as one op.
    void contentAwareScaleWindow() {
        Studio studio;
        studio.window->set_target_layer("paint");
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("content_aware_scale_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("cas_scale_x"))->setValue(50);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_content_aware_scale");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("content_aware_scale"));
        QCOMPARE(studio.ops[0]["scale_x"], Json(50.0));
        QCOMPARE(studio.ops[0]["id"], Json("paint"));
    }

    // ワープテキスト…: the chosen line's native_warp as one edit_line; なし takes it away.
    void warpTextWindow() {
        Studio studio;
        const std::string frame = studio.page().leaf_frames().front()->id;
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"frame_id", frame}, {"text", "ワープ"}, {"x_mm", 30.0},
                                                           {"y_mm", 30.0}, {"w_mm", 20.0}, {"h_mm", 20.0}}})));
        const std::string id = studio.session->document().story.back().id;
        studio.ops.clear();
        studio.window->on_line_selected(id, false);  // (chosen, as a click on it chooses it)
        QCOMPARE(studio.canvas()->selected_line_id, std::optional<std::string>(id));
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("warp_text_dialog")) return int(QDialog::Rejected);
            auto* style = dialog->findChild<QComboBox*>(QStringLiteral("warp_text_style"));
            style->setCurrentIndex(style->findData(QStringLiteral("flag")));
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("warp_text_bend"))->setValue(30);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_warp_text");
        QCOMPARE(studio.ops.size(), std::size_t(1));
        QCOMPARE(studio.ops[0]["op"], Json("edit_line"));
        const Json warp = studio.ops[0]["style"]["native_warp"];
        QCOMPARE(warp["style"], Json("flag"));
        QCOMPARE(warp["bend"], Json(30.0));
        QCOMPARE(warp["vertical"], Json(false));
        QVERIFY(studio.window->line_by_id(id) != nullptr);
        QCOMPARE(studio.window->line_by_id(id)->style["native_warp"]["style"], Json("flag"));  // (the line's own now)
        QCOMPARE(studio.canvas()->selected_line_id, std::optional<std::string>(id));
        QString shown;  // (the window opened on the line's own warp)
        studio.answers.responder->exec = [&shown](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("warp_text_dialog")) return int(QDialog::Rejected);
            auto* style = dialog->findChild<QComboBox*>(QStringLiteral("warp_text_style"));
            shown = style->currentData().toString();
            style->setCurrentIndex(0);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_warp_text");
        QCOMPARE(shown, QStringLiteral("flag"));
        QCOMPARE(studio.ops.size(), std::size_t(2));
        QVERIFY(studio.ops[1]["style"]["native_warp"].is_null());
        // (no line chosen: a word, no window)
        studio.window->story()->select(std::nullopt);
        studio.canvas()->selected_line_id.reset();
        int opened = 0;
        studio.answers.responder->exec = [&opened](QDialog*) {
            ++opened;
            return int(QDialog::Rejected);
        };
        studio.trigger("act_warp_text");
        QCOMPARE(opened, 0);
        QCOMPARE(studio.ops.size(), std::size_t(2));
        QCOMPARE(studio.window->last_notice(), QStringLiteral("先に選択ツール（V）で台詞（フキダシ）をクリックして選びます"));
    }
};

QTEST_MAIN(TestGuiTransform)
#include "test_gui_transform.moc"
