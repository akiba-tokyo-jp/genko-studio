// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」) in the window: イメージ → 解析 ▸'s 計測スケールを設定 ▸ (初期設定・カスタム…:
// its window), ものさしツール and カウントツール (in the ツール menu and the スポイト group, in Photoshop's order), 表示 →
// 表示・非表示 → カウント; the ものさしツール — a drag draws the line (its readout in the options and the 情報 panel), an end
// dragged, Alt+drag from an end the protractor, 消去, レイヤーの角度補正 turning the layer in hand the same as the op through
// `apply` (the line cleared) and, with Alt, the page turned and cut —; the カウントツール — a click puts a mark, a drag moves
// it, Alt+click takes it away, the options' groups (＋・－, the active one), sizes, 表示 and クリア, 表示 → カウント off hiding
// the marks — each change one Undo.
// PC5 J-d2: データポイントを選択 ▸ カスタム… (its window), 計測値を記録 (Shift+Ctrl+M in the Photoshop set) into the 計測ログ panel
// (ウィンドウ → 計測ログ: the table, 削除, 書き出し… — the CSV), 目盛りマーカーを配置… (its window, one step).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <memory>
#include <string>

#include "app/canvas.hpp"
#include "app/info_panels.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/measurement_log.hpp"
#include "core/command_bus.hpp"
#include "core/measure.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/measure_features.hpp"
#include "render/ops_registry.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;

namespace {

std::string file_text(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll().toStdString();
}

}  // namespace
namespace inject = genko::app::inject;

namespace {

// A 120 × 90 mm page at 300 dpi with a paint layer "a" (a bar across its middle at the working 200 dpi).
core::Document made_book() {
    auto doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::custom(120, 90, 40, 40, 1, 2, 2, 2, 2, 300, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    const int w = 945, h = 709;
    std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
    for (int y = h / 2 - 8; y < h / 2 + 8; ++y)
        for (int x = 150; x < w - 150; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            bytes[i + 1] = static_cast<char>(90);
            bytes[i + 3] = static_cast<char>(255);
        }
    core::Layer layer;
    layer.id = "a";
    layer.title = "バー";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
    p.layers.push_back(std::move(layer));
    return doc;
}

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    // (`held`: the page held by an agent — the person's ops on it refused)
    explicit Book(bool held = false) {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        core::Document doc = made_book();
        if (held) doc.page_locks[doc.pages[0]->id] = "ai:claude";
        gui_test::write_book(path, doc);
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        window->canvas()->fit_page();
        QCoreApplication::processEvents();
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Page& page() const { return *window->book().pages[0]; }
    const core::Measure& measure() const { return *page().measure; }
    app::PageCanvas* canvas() const { return window->canvas(); }
    QMenu* top(const QString& title) const {
        for (QAction* a : window->menuBar()->actions())
            if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
        return nullptr;
    }
    template <typename T>
    T* find(const char* name) const {
        return window->findChild<T*>(QString::fromLatin1(name));
    }
    void drag(const QPointF& from, const QPointF& to, Qt::KeyboardModifiers keys = Qt::NoModifier) {
        inject::mouse(canvas(), inject::Phase::Press, from, keys);
        inject::mouse(canvas(), inject::Phase::Move, from + (to - from) * 0.5, keys);
        inject::mouse(canvas(), inject::Phase::Move, to, keys);
        inject::mouse(canvas(), inject::Phase::Release, to, keys);
        QCoreApplication::processEvents();
    }
};

QMenu* submenu(QMenu* menu, const QString& name) {
    for (QAction* a : menu->actions())
        if (a->menu() != nullptr && a->menu()->objectName() == name) return a->menu();
    return nullptr;
}

core::Document applied(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc;
}

}  // namespace

class TestGuiMeasure : public QObject {
    Q_OBJECT
private slots:
    // The commands in their places: イメージ → 解析 ▸ (計測スケールを設定 ▸ 初期設定・カスタム…, ものさしツール, カウントツール — made,
    // not frames: they do not say "まだ使えません"); the two tools in ツール and in the スポイト group in Photoshop's order; 表示 →
    // 表示・非表示 → カウント (on).
    void theCommandsInTheirPlaces() {
        Book b;
        QMenu* analysis = b.find<QMenu>("menu_analysis");
        QVERIFY(analysis != nullptr);
        QVERIFY(analysis->actions().contains(b.act("act_measure_ruler_tool")));
        QVERIFY(analysis->actions().contains(b.act("act_measure_count_tool")));
        QMenu* scale = b.find<QMenu>("menu_measure_scale");
        QCOMPARE(scale->actions(), (QList<QAction*>{b.act("act_measure_scale_default"), b.act("act_measure_scale_custom")}));
        for (const char* name : {"act_measure_ruler_tool", "act_measure_count_tool"}) {
            QAction* tool = b.act(name);
            QVERIFY2(tool->isCheckable(), name);
            QVERIFY2(b.top(QStringLiteral("ツール"))->actions().contains(tool), name);
        }
        QCOMPARE(b.act("act_measure_ruler_tool")->text(), QStringLiteral("ものさしツール"));
        QCOMPARE(b.act("act_measure_count_tool")->text(), QStringLiteral("カウントツール"));
        const std::vector<QAction*> group = b.window->tool_group_of(b.act("act_picker"));
        QCOMPARE(group, (std::vector<QAction*>{b.act("act_picker"), b.act("act_measure_ruler_tool"), b.act("act_note"), b.act("act_measure_count_tool")}));
        QMenu* shows = submenu(b.top(QStringLiteral("表示")), QStringLiteral("menu_show"));
        QVERIFY(shows != nullptr && shows->actions().contains(b.act("act_show_count")));
        QVERIFY(b.act("act_show_count")->isChecked());
        QCOMPARE(b.act("act_show_count")->text(), QStringLiteral("カウント"));
        b.act("act_measure_scale_default")->trigger();
        QVERIFY(!b.window->last_notice().contains(QStringLiteral("まだ使えません")));
        b.act("act_measure_ruler_tool")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("measure"));
        QVERIFY(!b.window->last_notice().contains(QStringLiteral("まだ使えません")));
        b.act("act_measure_count_tool")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("count"));
    }

    // 計測スケールを設定: カスタム… (its window: the lengths and units; 用紙の mm), 初期設定 — each one Undo.
    void theScale() {
        Book b;
        b.answers.responder->exec = [](QDialog* dialog) {
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("scale_pixel_length"))->setValue(100);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("scale_logical_length"))->setValue(2.5);
            dialog->findChild<QLineEdit*>(QStringLiteral("scale_units"))->setText(QStringLiteral("cm"));
            return int(QDialog::Accepted);
        };
        b.act("act_measure_scale_custom")->trigger();
        QVERIFY(b.page().measure && b.page().measure->scale);
        QCOMPARE(*b.measure().scale, (core::MeasurementScale{100, 2.5, "cm"}));
        b.answers.responder->exec = [](QDialog* dialog) {
            dialog->findChild<QPushButton*>(QStringLiteral("scale_paper_mm"))->click();
            return int(QDialog::Accepted);
        };
        b.act("act_measure_scale_custom")->trigger();
        QCOMPARE(b.measure().scale->units, std::string("mm"));
        // (the review's P3-6: sent as the preset — the page's pixels a mm as the op has them)
        QCOMPARE(b.measure().scale->pixel_length, std::round(300 / 25.4 * 1e6) / 1e6);
        b.act("act_measure_scale_default")->trigger();
        QVERIFY(!b.page().measure);
        b.act("act_undo")->trigger();
        QCOMPARE(b.measure().scale->units, std::string("mm"));
    }

    // The ものさしツール: a drag draws the line (one Undo; the readout in the options and the 情報 panel), an end dragged, Alt
    // from an end the protractor, 消去.
    void theRulerTool() {
        Book b;
        b.act("act_measure_ruler_tool")->trigger();
        b.drag(QPointF(20, 60), QPointF(80, 60));
        QVERIFY(b.page().measure && b.measure().ruler);
        QVERIFY(std::fabs(b.measure().ruler->start[0] - 20) < 0.3 && std::fabs(b.measure().ruler->end[0] - 80) < 0.3);
        QVERIFY2(b.window->ruler_readout()->text().contains(QStringLiteral("L1 60.")), qPrintable(b.window->ruler_readout()->text()));
        QVERIFY2(b.window->info_panel()->ruler()->text().contains(QStringLiteral("A 0.0°")), qPrintable(b.window->info_panel()->ruler()->text()));
        // its end dragged up: the line no longer level
        const auto end = b.measure().ruler->end;
        b.drag(QPointF(end[0], end[1]), QPointF(end[0], end[1] - 5));
        QVERIFY(std::fabs(b.measure().ruler->start[0] - 20) < 0.3);
        QVERIFY(std::fabs(b.measure().ruler->end[1] - 55) < 0.3);
        const core::RulerReadout r = core::ruler_readout(*b.measure().ruler, std::nullopt, 300 / 25.4);
        QVERIFY(r.angle > 4 && r.angle < 5.5);
        // Alt from the start: the protractor's arm
        const auto start = b.measure().ruler->start;
        b.drag(QPointF(start[0], start[1]), QPointF(start[0], start[1] + 20), Qt::AltModifier);
        QVERIFY(b.measure().ruler->protractor);
        QVERIFY(std::fabs((*b.measure().ruler->protractor)[1] - (start[1] + 20)) < 0.3);
        QVERIFY(b.window->ruler_readout()->text().contains(QStringLiteral("L2 20.")));
        b.act("act_undo")->trigger();
        QVERIFY(!b.measure().ruler->protractor);
        // 消去
        b.find<QPushButton>("ruler_clear")->click();
        QVERIFY(!b.page().measure);
        QCOMPARE(b.window->ruler_readout()->text(), QStringLiteral("—"));
        b.act("act_undo")->trigger();
        QVERIFY(b.measure().ruler);
    }

    // レイヤーの角度補正 (Photoshop's: it crops by default, Alt not — the coordinator's decision): a click turns the page and
    // cuts it to the rectangle inside, the same as the op through `apply` on the book as it was (the line cleared, one Undo);
    // without a line said; with Alt, the layer in hand turned alone.
    void straightening() {
        Book b;
        b.window->set_target_layer("a");
        b.find<QPushButton>("ruler_straighten")->click();  // (no line yet: said, nothing done)
        QVERIFY(!b.page().measure);
        b.act("act_measure_ruler_tool")->trigger();
        b.drag(QPointF(20, 50), QPointF(90, 44));
        const core::Document before = b.window->book();
        // the click: the page turned and cut
        b.find<QPushButton>("ruler_straighten")->click();
        QVERIFY2(!b.page().measure, qPrintable(b.window->last_notice()));
        QVERIFY(b.page().spec.width_mm.value() < 120 && b.page().spec.height_mm.value() < 90);
        const core::Document cut = applied(before, Json{{"op", "straighten_layer"}, {"page", 1}, {"crop", true}});
        QCOMPARE(b.page().spec.width_mm.value(), cut.page(0).spec.width_mm.value());
        QVERIFY(*b.page().layers[0].raster_png == *cut.page(0).layers[0].raster_png);
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().spec.width_mm.value(), 120.0);
        QVERIFY(b.measure().ruler);
        // Alt: the layer in hand alone (the paper as it was)
        b.window->straighten_by_ruler(false);
        QVERIFY2(!b.page().measure, qPrintable(b.window->last_notice()));
        const core::Document by_op = applied(before, Json{{"op", "straighten_layer"}, {"page", 1}, {"ids", Json::array({"a"})}});
        QVERIFY(b.page().layers[0].raster_png && by_op.page(0).layers[0].raster_png);
        QVERIFY(*b.page().layers[0].raster_png == *by_op.page(0).layers[0].raster_png);
        QVERIFY(*b.page().layers[0].raster_png != *before.page(0).layers[0].raster_png);
        QCOMPARE(b.page().spec.width_mm.value(), 120.0);
        b.act("act_undo")->trigger();
        QVERIFY(b.measure().ruler);
        QVERIFY(*b.page().layers[0].raster_png == *before.page(0).layers[0].raster_png);
    }

    // (the coordinator's decision after the review's P3-1) A page the op cannot change (an agent holds it): the line drawn
    // stays in this window alone, its readout shown, the book untouched; 消去 lets it go; another page lets it go.
    void aLineOnAPageThatCannotChange() {
        Book b(true);
        b.act("act_measure_ruler_tool")->trigger();
        b.drag(QPointF(20, 60), QPointF(80, 60));
        QVERIFY(!b.page().measure);
        QVERIFY(b.canvas()->ruler_here);
        QVERIFY2(b.window->ruler_readout()->text().contains(QStringLiteral("L1 60.")), qPrintable(b.window->ruler_readout()->text()));
        QVERIFY(b.canvas()->ruler_in_hand());
        b.find<QPushButton>("ruler_clear")->click();
        QVERIFY(!b.canvas()->ruler_here);
        QCOMPARE(b.window->ruler_readout()->text(), QStringLiteral("—"));
    }

    // The カウントツール: clicks put marks (one Undo each), a drag moves one, Alt+click takes one away; the options: a new group
    // made active, the group chosen, its sizes and 表示, クリア; 表示 → カウント off: the marks hidden and not found.
    void theCountTool() {
        Book b;
        b.act("act_measure_count_tool")->trigger();
        inject::click_mm(b.canvas(), QPointF(20, 20));
        inject::click_mm(b.canvas(), QPointF(40, 20));
        inject::click_mm(b.canvas(), QPointF(60, 20));
        QCoreApplication::processEvents();
        QCOMPARE(b.measure().count_groups.size(), std::size_t(1));
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(3));
        QCOMPARE(b.window->count_groups()->count(), 1);
        QCOMPARE(b.window->count_groups()->itemText(0), QStringLiteral("カウントグループ 1（3）"));
        b.act("act_undo")->trigger();
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(2));
        // a drag moves the second
        b.drag(QPointF(40, 20), QPointF(40, 30));
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(2));
        QVERIFY(std::fabs(b.measure().count_groups[0].points[1][1] - 30) < 0.3);
        // Alt+click takes the first away
        inject::click_mm(b.canvas(), QPointF(20, 20), Qt::AltModifier);
        QCoreApplication::processEvents();
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(1));
        // a new group, made active: the next mark is its
        b.find<QToolButton>("count_new_group")->click();
        QCOMPARE(b.measure().count_groups.size(), std::size_t(2));
        QCOMPARE(b.measure().active_group, std::size_t(1));
        QCOMPARE(b.window->count_groups()->currentIndex(), 1);
        inject::click_mm(b.canvas(), QPointF(80, 40));
        QCOMPARE(b.measure().count_groups[1].points.size(), std::size_t(1));
        // the first group chosen again, its sizes and 表示
        b.window->count_groups()->setCurrentIndex(0);
        emit b.window->count_groups()->activated(0);
        QCOMPARE(b.measure().active_group, std::size_t(0));
        b.find<QSpinBox>("count_marker_size")->setValue(6);
        QCOMPARE(b.measure().count_groups[0].marker_size, 6);
        b.find<QSpinBox>("count_label_size")->setValue(20);
        QCOMPARE(b.measure().count_groups[0].label_size, 20);
        b.find<QCheckBox>("count_visible")->setChecked(false);
        QVERIFY(!b.measure().count_groups[0].visible);
        QVERIFY(!b.canvas()->count_at(b.canvas()->to_widget(QPointF(40, 30))));
        b.find<QCheckBox>("count_visible")->setChecked(true);
        QVERIFY(b.canvas()->count_at(b.canvas()->to_widget(QPointF(40, 30))));
        // 表示 → カウント off: hidden, not found (a click puts a new mark instead)
        b.act("act_show_count")->trigger();
        QVERIFY(!b.canvas()->extra_shown(QStringLiteral("count")));
        QVERIFY(!b.canvas()->count_at(b.canvas()->to_widget(QPointF(40, 30))));
        b.act("act_show_count")->trigger();
        QVERIFY(b.canvas()->extra_shown(QStringLiteral("count")));
        // クリア: the active group's marks (one Undo)
        b.find<QPushButton>("count_clear")->click();
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(0));
        QCOMPARE(b.measure().count_groups[1].points.size(), std::size_t(1));
        b.act("act_undo")->trigger();
        QCOMPARE(b.measure().count_groups[0].points.size(), std::size_t(1));
        // －: the active group deleted
        b.find<QToolButton>("count_delete_group")->click();
        QCOMPARE(b.measure().count_groups.size(), std::size_t(1));
        // (off the paper: no mark)
        inject::click_mm(b.canvas(), QPointF(-20, -20));
        QCOMPARE(core::count_total(b.measure()), std::size_t(1));
    }

    // PC5 J-d2: イメージ → 解析 ▸ データポイントを選択 ▸ カスタム…, 計測値を記録 (Shift+Ctrl+M in the Photoshop set), 目盛りマーカーを
    // 配置… made (not frames), ウィンドウ → 計測ログ after レイヤーカンプ; 計測値を記録 — the whole page, a selection, the ruler line
    // (this window's own too), the count — into the 計測ログ panel (shown), the book unchanged; 削除 of the rows chosen;
    // 書き出し… (the CSV of the chosen rows, else all: UTF-8 with its BOM, Photoshop's columns).
    void theMeasurementLog() {
        Book b;
        app::MeasurementLog::instance().clear();
        QMenu* points = b.find<QMenu>("menu_measure_points");
        QCOMPARE(points->actions(), (QList<QAction*>{b.act("act_measure_data_points")}));
        QMenu* analysis = b.find<QMenu>("menu_analysis");
        for (const char* name : {"act_measure_record", "act_measure_scale_marker"}) QVERIFY2(analysis->actions().contains(b.act(name)), name);
        QCOMPARE(b.act("act_measure_record")->shortcuts(), (QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+M"))}));
        QMenu* window_menu = b.top(QStringLiteral("ウィンドウ"));
        const QList<QAction*> in_window = window_menu->actions();
        QCOMPARE(in_window.indexOf(b.act("act_panel_layer_comps")) + 1, in_window.indexOf(b.act("act_panel_measurement_log")));
        QCOMPARE(in_window.indexOf(b.act("act_panel_measurement_log")) + 1, in_window.indexOf(b.act("act_panel_notes")));
        app::MeasurementLogPanel* panel = b.window->measurement_log_panel();
        QVERIFY(panel != nullptr);
        QDockWidget* dock = b.window->findChild<QDockWidget*>(QStringLiteral("計測ログ"));
        QVERIFY(dock != nullptr && !dock->isVisible());
        b.act("act_panel_measurement_log")->trigger();
        QVERIFY(dock->isVisible() && b.act("act_panel_measurement_log")->isChecked());
        b.act("act_panel_measurement_log")->trigger();
        QVERIFY(!dock->isVisible());
        const std::int64_t revision = b.window->book().revision;
        // the whole page: one row (counted on the 200 dpi grid, in the page's 300 dpi pixels), the panel shown
        b.act("act_measure_record")->trigger();
        QVERIFY2(!b.window->last_notice().contains(QStringLiteral("まだ使えません")), qPrintable(b.window->last_notice()));
        QVERIFY(dock->isVisible());
        const app::MeasurementLog& log = app::MeasurementLog::instance();
        QCOMPARE(log.rows().size(), std::size_t(1));
        QCOMPARE(log.rows()[0].values["label"], Json("計測 1"));
        QCOMPARE(log.rows()[0].values["source"], Json("選択範囲"));
        QCOMPARE(log.rows()[0].values["area"].get<double>(), 945.0 * 709.0 * 2.25);
        QCOMPARE(panel->table()->rowCount(), 1);
        QCOMPARE(panel->table()->horizontalHeaderItem(0)->text(), QStringLiteral("ラベル"));
        QVERIFY(panel->status()->text().contains(QStringLiteral("200 dpi")));
        // a selection over the bar: its gray darker than the paper's
        b.canvas()->set_selection(Json{{"rect", Json::array({20, 40, 80, 10})}});
        b.act("act_measure_record")->trigger();
        QCOMPARE(log.rows().size(), std::size_t(2));
        QCOMPARE(log.rows()[1].values["label"], Json("計測 2"));
        QVERIFY(log.rows()[1].values["grayMin"].get<double>() < 100.0);
        QVERIFY(log.rows()[1].values["grayMax"].get<double>() > 250.0);
        b.canvas()->set_selection(std::nullopt);
        // the ruler line, then the count (auto: the ruler first)
        b.window->measure_op(Json{{"op", "set_ruler_line"}, {"start", {10, 10}}, {"end", {40, 50}}});
        b.act("act_measure_record")->trigger();
        QCOMPARE(log.rows().back().values["source"], Json("ものさしツール"));
        QVERIFY(log.rows().back().values.contains("angle"));
        b.window->measure_op(Json{{"op", "set_ruler_line"}, {"clear", true}});
        b.window->measure_op(Json{{"op", "add_count"}, {"at_mm", {10, 10}}});
        b.act("act_measure_record")->trigger();
        QCOMPARE(log.rows().back().values["source"], Json("カウントツール"));
        QCOMPARE(log.rows().back().values["count"], Json(1));
        QCOMPARE(panel->table()->rowCount(), 4);
        const std::int64_t marked = b.window->book().revision;
        QVERIFY(marked > revision);  // (the ruler line and the count are the book's; the records are not)
        // 削除: the rows chosen (none: nothing)
        panel->table()->clearSelection();
        panel->button(QStringLiteral("log_delete"))->click();
        QCOMPARE(log.rows().size(), std::size_t(4));
        panel->table()->selectRow(1);
        panel->button(QStringLiteral("log_delete"))->click();
        QCOMPARE(log.rows().size(), std::size_t(3));
        QCOMPARE(log.rows()[1].values["source"], Json("ものさしツール"));
        // 書き出し…: the rows chosen, else all
        const QString csv = b.tmp.path() + QStringLiteral("/log.csv");
        b.answers.responder->save_path = [csv](const QString&, const QString&) { return csv; };
        panel->table()->clearSelection();
        panel->button(QStringLiteral("log_export"))->click();
        std::string text = file_text(csv);
        QVERIFY(text.starts_with("\xEF\xBB\xBF" "ラベル,日時,ドキュメント,ソース,スケール"));
        QCOMPARE(std::count(text.begin(), text.end(), '\n'), std::ptrdiff_t(4));
        QVERIFY(text.find("計測 1,") != std::string::npos && text.find("カウントツール") != std::string::npos);
        panel->table()->selectRow(2);
        panel->button(QStringLiteral("log_export"))->click();
        text = file_text(csv);
        QCOMPARE(std::count(text.begin(), text.end(), '\n'), std::ptrdiff_t(2));
        QVERIFY(text.find("カウントツール") != std::string::npos && text.find("ものさしツール") == std::string::npos);
        QCOMPARE(b.window->book().revision, marked);  // (recording, deleting, exporting: the book untouched)
        // a page the person cannot change: its line this window's own, measured all the same
        app::MeasurementLog::instance().clear();
        Book held(true);
        held.act("act_measure_ruler_tool")->trigger();
        held.drag(QPointF(20, 60), QPointF(80, 60));
        QVERIFY(held.canvas()->ruler_here);
        held.act("act_measure_record")->trigger();
        QCOMPARE(log.rows().size(), std::size_t(1));
        QCOMPARE(log.rows()[0].values["source"], Json("ものさしツール"));
        QVERIFY(std::fabs(log.rows()[0].values["length"].get<double>() - 60.0 * 300 / 25.4) < 5);
    }

    // データポイントを選択 → カスタム…: the points of each source (共通 for all three), this machine's; 計測値を記録 keeps a row to
    // them; 初期設定に戻す.
    void theDataPoints() {
        Book b;
        app::MeasurementLog::instance().clear();
        b.answers.responder->exec = [](QDialog* dialog) {
            for (QCheckBox* box : dialog->findChildren<QCheckBox*>()) box->setChecked(false);
            dialog->findChild<QCheckBox*>(QStringLiteral("point_common_label"))->setChecked(true);
            dialog->findChild<QCheckBox*>(QStringLiteral("point_selection_area"))->setChecked(true);
            dialog->findChild<QCheckBox*>(QStringLiteral("point_ruler_length"))->setChecked(true);
            return int(QDialog::Accepted);
        };
        b.act("act_measure_data_points")->trigger();
        const render::measure::DataPoints points = app::MeasurementLog::data_points();
        QCOMPARE(points.selection, (std::vector<std::string>{"label", "area"}));
        QCOMPARE(points.ruler, (std::vector<std::string>{"label", "length"}));
        QCOMPARE(points.count, (std::vector<std::string>{"label"}));
        b.act("act_measure_record")->trigger();
        const app::MeasurementLog& log = app::MeasurementLog::instance();
        QCOMPARE(log.rows().size(), std::size_t(1));
        QCOMPARE(log.rows()[0].values, (Json{{"label", "計測 1"}, {"area", 945.0 * 709.0 * 2.25}}));
        QCOMPARE(b.window->measurement_log_panel()->table()->columnCount(), 2);
        // the window shows them as chosen; 初期設定に戻す: every one
        b.answers.responder->exec = [](QDialog* dialog) {
            if (!dialog->findChild<QCheckBox*>(QStringLiteral("point_selection_area"))->isChecked()) return int(QDialog::Rejected);
            if (dialog->findChild<QCheckBox*>(QStringLiteral("point_selection_perimeter"))->isChecked()) return int(QDialog::Rejected);
            dialog->findChild<QPushButton*>(QStringLiteral("points_defaults"))->click();
            return int(QDialog::Accepted);
        };
        b.act("act_measure_data_points")->trigger();
        QCOMPARE(app::MeasurementLog::data_points(), render::measure::DataPoints{});
        app::MeasurementLog::instance().clear();
    }

    // 目盛りマーカーを配置…: its window (the nice length in the scale's units; the size, the text, its place, the colour), then
    // one step — a group 目盛りマーカー of the bar and its label —, undone in one.
    void theScaleMarker() {
        Book b;
        b.window->measure_op(Json{{"op", "set_measurement_scale"}, {"preset", "paper_mm"}});
        const std::size_t before = b.page().layers.size();
        double asked_length = 0;
        QString suffix;
        b.answers.responder->exec = [&asked_length, &suffix](QDialog* dialog) {
            auto* length = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("marker_length"));
            asked_length = length->value();
            suffix = length->suffix();
            dialog->findChild<QComboBox*>(QStringLiteral("marker_color"))->setCurrentIndex(1);
            return int(QDialog::Accepted);
        };
        b.act("act_measure_scale_marker")->trigger();
        QCOMPARE(asked_length, 5.0);  // (a fifth of the trim's 40 mm wide, 8 mm → 5 mm)
        QCOMPARE(suffix, QStringLiteral(" mm"));
        QCOMPARE(b.page().layers.size(), before + 3);
        const core::Layer& group = b.page().layers.back();
        QCOMPARE(group.title, std::string("目盛りマーカー"));
        QVERIFY(group.group);
        QCOMPARE(b.page().layers[before + 1].kind, core::LayerKind::Type);
        QCOMPARE(b.page().layers[before + 1].type->text, std::string("5 mm"));
        b.act("act_undo")->trigger();
        QCOMPARE(b.page().layers.size(), before);
        // refused (too long for the trim): said, nothing placed
        b.answers.responder->exec = [](QDialog* dialog) {
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("marker_length"))->setValue(5000);
            return int(QDialog::Accepted);
        };
        b.act("act_measure_scale_marker")->trigger();
        QCOMPARE(b.page().layers.size(), before);
        QVERIFY2(b.window->last_error().contains(QStringLiteral("仕上がりの幅")), qPrintable(b.window->last_error()));
    }
};

QTEST_MAIN(TestGuiMeasure)
#include "test_gui_measure.moc"
