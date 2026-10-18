// 修復 (PC3 B4, SPEC.md COLOR-06「修復」) on the offscreen platform: スポット修復ブラシツール, 修復ブラシツール, コピースタンプツール
// (S) and ヒストリーブラシツール with Photoshop's names, in ツール and the tools' group, their settings page (調整あり and サンプル
// with the clone tools, 種類 with the spot healing brush); Alt+click sets the clone source and a stroke copies it (the
// canvas showing the source's cross), the source paired by the first stroke (調整あり); コピーソース panel's five sources,
// transform and overlay; 履歴's ◆ row as the History Brush's source and a stroke painting it back; the Spot Healing
// Brush's stroke; コンテンツに応じた塗りつぶし…'s window (the result shown on the page while it is open, then a new layer).

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include <cmath>

#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/clone_source_panel.hpp"
#include "app/history_panel.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

constexpr int kSide = 472;  // (the 60 mm page at the working 200 dpi)

double px_of(double mm) { return mm * kSide / 60.0; }

// A book of one 60 × 60 mm colour page and a paint layer "paint": red on the left half, white on the right, a dark dot at
// (45, 15) mm.
core::Document book_doc() {
    auto doc = core::new_episode("修復", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const bool dot = std::hypot(x - px_of(45), y - px_of(15)) < 6;
            const std::array<int, 4> c = dot ? std::array<int, 4>{20, 20, 20, 255} : x < kSide / 2 ? std::array<int, 4>{230, 30, 30, 255}
                                                                                                    : std::array<int, 4>{255, 255, 255, 255};
            for (const int v : c) bytes.push_back(static_cast<char>(v));
        }
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
    return core::CommandBus(render::ops_registry())
        .apply(doc,
               Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", core::b64encode(render::write_png(picture))}}}),
               core::Actor("human:tester"))
        .doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
        window->set_target_layer("paint");
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    // The layer's colour at a point (mm), 0..255.
    std::vector<double> at(double x_mm, double y_mm, const std::string& id = "paint") const {
        const render::Image image = render::open_image(*layer(id)->raster_png).convert("RGBA");
        return image.getpixel(static_cast<int>(px_of(x_mm)), static_cast<int>(px_of(y_mm)));
    }
};

QMenu* menu_at(const app::MainWindow* window, const QString& title) {
    for (QAction* a : window->menuBar()->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

bool reddish(const std::vector<double>& c) { return c[0] > 200 && c[1] < 80 && c[2] < 80; }
bool white(const std::vector<double>& c) { return c[0] > 245 && c[1] > 245 && c[2] > 245; }

}  // namespace

class TestGuiRetouch : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The four tools with Photoshop's names, their key (S), their place, one settings page with each tool's rows.
    void theTools() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        const std::vector<std::pair<const char*, const char*>> tools{{"act_spot_healing", "スポット修復ブラシツール"},
                                                                     {"act_healing", "修復ブラシツール"},
                                                                     {"act_clone_stamp", "コピースタンプツール"},
                                                                     {"act_history_brush", "ヒストリーブラシツール"}};
        QMenu* menu = menu_at(w, QStringLiteral("ツール"));
        QVERIFY(menu != nullptr);
        for (const auto& [name, text] : tools) {
            QAction* a = w->action(QString::fromLatin1(name));
            QVERIFY2(a != nullptr, name);
            QCOMPARE(a->text(), QString::fromUtf8(text));
            QVERIFY(a->isCheckable());
            QVERIFY2(menu->actions().contains(a), name);
            QVERIFY(w->tools_group()->actions().contains(a));
        }
        QCOMPARE(w->action("act_clone_stamp")->shortcut(), QKeySequence(QStringLiteral("S")));
        for (QAction* other : w->findChildren<QAction*>()) {  // (S is its own: no other command has it)
            if (other != w->action("act_clone_stamp")) QVERIFY2(!other->shortcuts().contains(QKeySequence(QStringLiteral("S"))), qPrintable(other->text()));
        }
        QAction* fill = w->action("act_content_aware_fill");
        QVERIFY(fill != nullptr);
        QCOMPARE(fill->text(), QStringLiteral("コンテンツに応じた塗りつぶし…"));
        QVERIFY(menu_at(w, QStringLiteral("編集"))->actions().contains(fill));
        // each tool its canvas tool and the settings page; the rows that apply to it
        const std::map<const char*, QString> kinds{{"act_spot_healing", QStringLiteral("spotheal")},
                                                   {"act_healing", QStringLiteral("heal")},
                                                   {"act_clone_stamp", QStringLiteral("clone")},
                                                   {"act_history_brush", QStringLiteral("historybrush")}};
        for (const auto& [name, tool] : kinds) {
            studio.trigger(name);
            QCOMPARE(studio.canvas()->tool(), tool);
            QCOMPARE(w->tool_settings()->current_page(), w->tool_settings()->page_for(tool));
        }
        auto* aligned = w->findChild<QCheckBox*>(QStringLiteral("retouch_aligned"));
        auto* type = w->findChild<QComboBox*>(QStringLiteral("retouch_spot_type"));
        QVERIFY(aligned != nullptr && type != nullptr);
        studio.trigger("act_clone_stamp");
        QVERIFY(!aligned->isHidden());
        QVERIFY(type->isHidden());
        studio.trigger("act_spot_healing");
        QVERIFY(aligned->isHidden());
        QVERIFY(!type->isHidden());
        // the brush's diameter is the canvas's
        w->findChild<QDoubleSpinBox*>(QStringLiteral("retouch_size"))->setValue(4);
        QCOMPARE(studio.canvas()->retouch_mm, 4.0);
        QCOMPARE(w->retouch_settings.size_mm, 4.0);
    }

    // Alt+click sets the clone source; a stroke copies it there; 調整あり keeps the pairing; a stroke with no source is
    // refused in words.
    void cloneStamp() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        studio.trigger("act_clone_stamp");
        inject::mouse_stroke(studio.canvas(), {QPointF(50, 40), QPointF(52, 40)});  // (no source yet)
        QVERIFY(white(studio.at(51, 40)));
        inject::click_mm(studio.canvas(), QPointF(15, 40), Qt::AltModifier);
        QVERIFY(w->clone_sources.now().source.has_value());
        QVERIFY(std::abs(w->clone_sources.now().source->x() - 15) < 0.2);
        QVERIFY(studio.canvas()->clone_shown.source.has_value());
        QVERIFY(!w->clone_sources.now().anchor);
        inject::mouse_stroke(studio.canvas(), {QPointF(45, 40), QPointF(50, 40)});
        QVERIFY2(reddish(studio.at(47, 40)), qPrintable(QString::number(studio.at(47, 40)[1])));
        QVERIFY(white(studio.at(47, 50)));  // (outside the brush)
        QVERIFY(w->clone_sources.now().anchor.has_value());  // (調整あり: paired by the first stroke)
        // aligned: a second stroke further down reads further down too (still red, the pairing kept)
        inject::mouse_stroke(studio.canvas(), {QPointF(45, 52), QPointF(48, 52)});
        QVERIFY(reddish(studio.at(46, 52)));
        // the source's cross on the canvas (drawn without trouble), its overlay picture made on a worker
        QVERIFY(gui_test::wait_for([&] { return !studio.canvas()->clone_shown.picture.isNull(); }));
        studio.canvas()->repaint();
    }

    // コピーソース: five sources, the one in use; its transform and the overlay; 変形をリセット.
    void cloneSourcePanel() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        auto* dock = w->findChild<QDockWidget*>(QStringLiteral("コピーソース"));
        QVERIFY(dock != nullptr);
        dock->show();
        app::CloneSourcePanel* panel = w->clone_source_panel();
        QVERIFY(panel != nullptr);
        studio.trigger("act_clone_stamp");
        inject::click_mm(studio.canvas(), QPointF(10, 10), Qt::AltModifier);
        panel->findChild<QToolButton*>(QStringLiteral("clone_slot_2"))->click();
        QCOMPARE(w->clone_sources.active, 2);
        QVERIFY(!w->clone_sources.now().source);  // (a source of its own)
        inject::click_mm(studio.canvas(), QPointF(20, 20), Qt::AltModifier);
        QVERIFY(w->clone_sources.items[2].source && w->clone_sources.items[0].source);
        QVERIFY(std::abs(w->clone_sources.items[0].source->x() - 10) < 0.2);
        panel->findChild<QDoubleSpinBox*>(QStringLiteral("clone_width"))->setValue(50);
        panel->findChild<QDoubleSpinBox*>(QStringLiteral("clone_rotation"))->setValue(30);
        panel->findChild<QToolButton*>(QStringLiteral("clone_flip_h"))->setChecked(true);
        QCOMPARE(w->clone_sources.now().width, 50.0);
        QCOMPARE(w->clone_sources.now().rotation, 30.0);
        QVERIFY(w->clone_sources.now().flip_h);
        QVERIFY(std::abs(studio.canvas()->clone_shown.m[0]) > 1.0);  // (the destination's offsets read twice as far)
        panel->findChild<QPushButton*>(QStringLiteral("clone_reset"))->click();
        QCOMPARE(w->clone_sources.now().width, 100.0);
        QCOMPARE(w->clone_sources.now().rotation, 0.0);
        QVERIFY(!w->clone_sources.now().flip_h);
        // the overlay's settings are the canvas's
        panel->findChild<QSpinBox*>(QStringLiteral("clone_opacity"))->setValue(40);
        panel->findChild<QCheckBox*>(QStringLiteral("clone_clipped"))->setChecked(false);
        auto* blend = panel->findChild<QComboBox*>(QStringLiteral("clone_blend"));
        blend->setCurrentIndex(blend->findData(QStringLiteral("difference")));
        QCOMPARE(studio.canvas()->clone_shown.opacity, 0.4);
        QVERIFY(!studio.canvas()->clone_shown.clipped);
        QCOMPARE(studio.canvas()->clone_shown.blend, QStringLiteral("difference"));
        panel->findChild<QCheckBox*>(QStringLiteral("clone_show"))->setChecked(false);
        QVERIFY(!studio.canvas()->clone_shown.overlay);
        // a stroke with a transformed source (mirrored) works too
        panel->findChild<QToolButton*>(QStringLiteral("clone_slot_0"))->click();
        panel->findChild<QToolButton*>(QStringLiteral("clone_flip_h"))->setChecked(true);
        const auto before = *studio.layer("paint")->raster_png;
        inject::mouse_stroke(studio.canvas(), {QPointF(40, 30), QPointF(44, 30)});
        QVERIFY(*studio.layer("paint")->raster_png != before);
    }

    // 履歴's ◆: （はじめ） unless chosen; the History Brush paints its state back.
    void historyBrush() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        studio.trigger("act_clone_stamp");
        inject::click_mm(studio.canvas(), QPointF(15, 40), Qt::AltModifier);
        inject::mouse_stroke(studio.canvas(), {QPointF(42, 40), QPointF(50, 40)});
        QVERIFY(reddish(studio.at(46, 40)));
        studio.trigger("act_history");
        app::HistoryPanel* panel = w->history();
        panel->refresh();
        QCOMPARE(panel->brush_source(), 0);
        QVERIFY(panel->list()->item(0)->data(Qt::UserRole + 1).toBool());  // (marked)
        // painted back to （はじめ）: white again where the brush goes
        studio.trigger("act_history_brush");
        inject::mouse_stroke(studio.canvas(), {QPointF(44, 40), QPointF(48, 40)});
        QVERIFY2(white(studio.at(46, 40)), qPrintable(QString::number(studio.at(46, 40)[1])));
        // the clone's state chosen (row 1): red again
        panel->refresh();
        panel->set_brush_source(1);
        QVERIFY(w->history_brush_row == std::optional<int>(1));
        QVERIFY(panel->list()->item(1)->data(Qt::UserRole + 1).toBool());
        inject::mouse_stroke(studio.canvas(), {QPointF(44, 40), QPointF(48, 40)});
        QVERIFY(reddish(studio.at(46, 40)));
        // a row past the present cannot be one
        panel->set_brush_source(99);
        QVERIFY(w->history_brush_row == std::optional<int>(1));
    }

    // The Spot Healing Brush takes the dark dot away.
    void spotHealing() {
        Studio studio;
        QVERIFY(studio.at(45, 15)[0] < 50);
        studio.trigger("act_spot_healing");
        inject::mouse_stroke(studio.canvas(), {QPointF(44.5, 15), QPointF(45.5, 15)});
        QVERIFY2(studio.at(45, 15)[0] > 200, qPrintable(QString::number(studio.at(45, 15)[0])));
    }

    // コンテンツに応じた塗りつぶし…: asks for a selection; its window shows the result on the page while open; the fill on a
    // new layer.
    void contentAwareFill() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        studio.trigger("act_content_aware_fill");  // (no selection: said, nothing asked)
        QVERIFY(studio.answers.asked.isEmpty());
        studio.trigger("act_marquee");
        inject::mouse_stroke(studio.canvas(), {QPointF(42, 12), QPointF(45, 15), QPointF(48, 18)});
        QVERIFY(studio.canvas()->selection().has_value());
        const std::size_t layers = studio.page().layers.size();
        bool previewed = false;
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("content_aware_dialog")) return int(QDialog::Rejected);
            auto* output = dialog->findChild<QComboBox*>(QStringLiteral("field_output"));
            if (output == nullptr) return int(QDialog::Rejected);
            output->setCurrentIndex(output->findText(QStringLiteral("新規レイヤー")));
            // (the result shown on the page a moment after the change)
            previewed = gui_test::wait_for([&] { return studio.canvas()->page() != nullptr && studio.canvas()->page()->layers.size() == layers + 1; }, 20000);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_content_aware_fill");
        QVERIFY(previewed);
        QCOMPARE(studio.page().layers.size(), layers + 1);
        const core::Layer& made = studio.page().layers.back();
        QCOMPARE(made.title, std::string("コンテンツに応じた塗りつぶし"));
        QVERIFY(studio.at(45, 15, made.id)[3] > 200);  // (the fill alone, where the selection was)
        QCOMPARE(studio.at(10, 50, made.id)[3], 0.0);
        QVERIFY(studio.at(45, 15)[0] < 50);  // (the layer as it was)
        (void)w;
    }
};

QTEST_MAIN(TestGuiRetouch)
#include "test_gui_retouch.moc"
