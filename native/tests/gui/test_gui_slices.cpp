// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」) in the window: the スライスツール and the スライス選択ツール (in the ツール menu
// beside 切り抜きツール and in its group, in Photoshop's order), 表示 → 表示・非表示 → スライス, スナップ先 → スライス, スライスを
// ロック・スライスを消去, レイヤー → 新規レイヤーベースのスライス, 環境設定 → ガイド・グリッド・スライス's スライス; the スライス
// ツール — a drag draws a user slice, Shift square, a click chooses the slice there, 固定サイズ, ガイドからスライスを作成 —; the
// スライス選択ツール — a click chooses, a drag moves, a handle sizes, an auto slice moved made a user one, ユーザー定義スライスに
// 変換, 分割…, 削除, スライスオプション… (and its double click) —; スライスをロック refusing every gesture; Web 用に保存 by
// slices — each change one Undo.

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/save_for_web_dialog.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

// A 60 × 45 mm page at 254 dpi (10 pixels a mm: 600 × 450) with a paint layer "a" (a block at 10..20 × 30..40 mm).
core::Document made_book() {
    auto doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::custom(60, 45, 5, 5, 1, 2, 2, 2, 2, 254, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    const int w = 472, h = 354;  // (60 × 45 mm at the working 200 dpi)
    std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
    for (int y = 236; y < 315; ++y)
        for (int x = 79; x < 157; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            bytes[i + 2] = static_cast<char>(200);
            bytes[i + 3] = static_cast<char>(255);
        }
    core::Layer layer;
    layer.id = "a";
    layer.title = "四角";
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

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, made_book());
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        window->canvas()->fit_page();
        QCoreApplication::processEvents();
        // (no snapping: the slices where the pointer puts them)
        if (act("act_snap_on")->isChecked()) act("act_snap_on")->trigger();
        if (act("act_lock_slices")->isChecked()) act("act_lock_slices")->trigger();
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Page& page() const { return *window->book().pages[0]; }
    const core::Slices& slices() const { return *page().slices; }
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
    void click(const QPointF& at) {
        inject::mouse(canvas(), inject::Phase::Press, at);
        inject::mouse(canvas(), inject::Phase::Release, at);
        QCoreApplication::processEvents();
    }
};

QMenu* submenu(QMenu* menu, const QString& name) {
    for (QAction* a : menu->actions())
        if (a->menu() != nullptr && a->menu()->objectName() == name) return a->menu();
    return nullptr;
}

QMenu* titled(QMenu* menu, const QString& title) {
    for (QAction* a : menu->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

bool near(double a, double b, double by = 0.35) { return std::fabs(a - b) <= by; }

}  // namespace

class TestGuiSlices : public QObject {
    Q_OBJECT
private slots:
    // The commands in their places: the two tools in ツール after 切り抜きツール and in its group; 表示・非表示 ▸ スライス (on),
    // スナップ先 ▸ スライス (between レイヤー and ドキュメントの境界), スライスをロック・スライスを消去 after the guides' commands,
    // レイヤー → 新規レイヤーベースのスライス after ラスタライズ ▸; 環境設定 → ガイド・グリッド・スライス's スライス.
    void theCommandsInTheirPlaces() {
        Book b;
        for (const auto& [name, words] : {std::pair{"act_slice_tool", "スライスツール"}, std::pair{"act_slice_select_tool", "スライス選択ツール"}}) {
            QAction* tool = b.act(name);
            QVERIFY2(tool != nullptr && tool->isCheckable(), name);
            QCOMPARE(tool->text(), QString::fromUtf8(words));
            QVERIFY(tool->shortcuts().isEmpty());  // (C is the 切り抜き group's: the crop tool keeps it)
        }
        const QList<QAction*> tools = b.top(QStringLiteral("ツール"))->actions();
        const auto crop = tools.indexOf(b.act("act_crop_tool"));
        QVERIFY(crop >= 0);
        QCOMPARE(tools.indexOf(b.act("act_slice_tool")), crop + 1);
        QCOMPARE(tools.indexOf(b.act("act_slice_select_tool")), crop + 2);
        QCOMPARE(b.window->tool_group_of(b.act("act_crop_tool")),
                 (std::vector<QAction*>{b.act("act_crop_tool"), b.act("act_slice_tool"), b.act("act_slice_select_tool")}));
        QMenu* view = b.top(QStringLiteral("表示"));
        QMenu* shows = submenu(view, QStringLiteral("menu_show"));
        QVERIFY(shows != nullptr && shows->actions().contains(b.act("act_show_slices")));
        QVERIFY(b.act("act_show_slices")->isChecked());
        QMenu* snap = titled(view, QStringLiteral("スナップ先"));
        QVERIFY(snap != nullptr);
        const QList<QAction*> to = snap->actions();
        QCOMPARE(to.indexOf(b.act("act_snap_slices")), to.indexOf(b.act("act_snap_layers")) + 1);
        QCOMPARE(to.indexOf(b.act("act_snap_bounds")), to.indexOf(b.act("act_snap_slices")) + 1);
        const QList<QAction*> items = view->actions();
        QVERIFY(items.indexOf(b.act("act_lock_slices")) > items.indexOf(b.act("act_guides_from_shape")));
        QCOMPARE(items.indexOf(b.act("act_clear_slices")), items.indexOf(b.act("act_lock_slices")) + 1);
        QCOMPARE(b.act("act_lock_slices")->text(), QStringLiteral("スライスをロック"));
        QCOMPARE(b.act("act_clear_slices")->text(), QStringLiteral("スライスを消去"));
        QMenu* layers = b.top(QStringLiteral("レイヤー"));
        const QList<QAction*> in_layers = layers->actions();
        int rasterize = -1;
        for (int i = 0; i < in_layers.size(); ++i)
            if (in_layers[i]->menu() != nullptr && in_layers[i]->menu()->objectName() == QLatin1String("menu_rasterize")) rasterize = i;
        QVERIFY(rasterize >= 0);
        QCOMPARE(in_layers.indexOf(b.act("act_layer_slice")), rasterize + 1);
        QCOMPARE(b.act("act_layer_slice")->text(), QStringLiteral("新規レイヤーベースのスライス"));
        // 環境設定 → ガイド・グリッド・スライス: ラインのカラー and スライス番号を表示, kept by 決める and on the canvas
        const auto pages = app::preferences::unit_pages(b.window.get());
        auto* colour = pages.second->findChild<QPushButton*>(QStringLiteral("prefs_slice_color"));
        auto* numbers = pages.second->findChild<QCheckBox*>(QStringLiteral("prefs_slice_numbers"));
        QVERIFY(colour != nullptr && numbers != nullptr);
        QCOMPARE(colour->property("colour").value<QColor>(), QColor(QStringLiteral("#38b5ff")));
        QVERIFY(numbers->isChecked());
        colour->setProperty("colour", QColor(QStringLiteral("#ff00ff")));
        numbers->setChecked(false);
        app::preferences::save_unit_pages(pages.first, pages.second);
        b.window->apply_guide_prefs();
        QCOMPARE(b.canvas()->slice_colour, QColor(QStringLiteral("#ff00ff")));
        QVERIFY(!b.canvas()->show_slice_numbers);
        app::settings()->remove(QStringLiteral("guides/slice_color"));
        app::settings()->remove(QStringLiteral("guides/slice_numbers"));
        b.window->apply_guide_prefs();
        QVERIFY(b.canvas()->show_slice_numbers);
        delete pages.first;
        delete pages.second;
    }

    // The スライスツール: a drag draws a user slice (one Undo, chosen), Shift square, a click chooses the slice there, 固定サイズ,
    // ガイドからスライスを作成.
    void theSliceTool() {
        Book b;
        b.act("act_slice_tool")->trigger();
        QCOMPARE(b.canvas()->tool(), QStringLiteral("slice"));
        b.drag(QPointF(10, 5), QPointF(30, 15));
        QVERIFY(b.page().slices && b.slices().list.size() == 1);
        const auto r = b.slices().list[0].rect_mm;
        QVERIFY2(near(r[0], 10) && near(r[1], 5) && near(r[2], 20) && near(r[3], 10), Json(r).dump().c_str());
        QVERIFY(b.canvas()->chosen_slice_now() && b.canvas()->chosen_slice_now()->id == b.slices().list[0].id);
        // Shift: a square
        b.drag(QPointF(40, 20), QPointF(50, 24), Qt::ShiftModifier);
        QCOMPARE(b.slices().list.size(), std::size_t(2));
        const auto sq = b.slices().list[1].rect_mm;
        QVERIFY2(near(sq[2], sq[3], 0.15) && near(sq[2], 10), Json(sq).dump().c_str());
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        // a click: the slice there chosen, none made
        b.canvas()->chosen_slice.reset();
        b.click(QPointF(20, 10));
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        QVERIFY(b.canvas()->chosen_slice_now() && b.canvas()->chosen_slice_now()->id == b.slices().list[0].id);
        // 固定サイズ: 100 × 50 pixels of the page where it is pressed
        auto* style = b.find<QComboBox>("slice_style");
        style->setCurrentIndex(2);
        b.find<QDoubleSpinBox>("slice_style_width")->setValue(100);
        b.find<QDoubleSpinBox>("slice_style_height")->setValue(50);
        b.click(QPointF(35, 30));
        QCOMPARE(b.slices().list.size(), std::size_t(2));
        const auto fixed = b.slices().list[1].rect_mm;
        QVERIFY2(near(fixed[0], 35) && near(fixed[2], 10, 0.01) && near(fixed[3], 5, 0.01), Json(fixed).dump().c_str());
        style->setCurrentIndex(0);
        // ガイドからスライスを作成: every slice replaced by the guides' cells (one Undo)
        b.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 30}},
                                         Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "h"}, {"at", 20}}}));
        b.find<QPushButton>("slices_from_guides")->click();
        QCOMPARE(b.slices().list.size(), std::size_t(4));
        QCOMPARE(b.slices().list[0].rect_mm, (std::array<double, 4>{0, 0, 30, 20}));
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(2));
    }

    // The スライス選択ツール: a click chooses (its words in the options), a drag moves a user slice, a handle sizes it, an auto
    // slice dragged made a user one there; ユーザー定義スライスに変換, 分割…, スライスオプション… (and a double click), 削除 — each
    // one Undo.
    void theSliceSelectTool() {
        Book b;
        b.window->apply_ops(Json::array({Json{{"op", "add_slice"}, {"page", 1}, {"rect_mm", {10, 10, 20, 10}}, {"name", "hero"}}}));
        const std::int64_t id = b.slices().list[0].id;
        b.act("act_slice_select_tool")->trigger();
        b.click(QPointF(20, 15));
        QVERIFY(b.canvas()->chosen_slice_now() && b.canvas()->chosen_slice_now()->id == id);
        QVERIFY2(b.window->slice_chosen_label()->text().contains(QStringLiteral("hero")), qPrintable(b.window->slice_chosen_label()->text()));
        // moved by 5 × 5 mm
        b.drag(QPointF(20, 15), QPointF(25, 20));
        auto r = b.slices().list[0].rect_mm;
        QVERIFY2(near(r[0], 15) && near(r[1], 15) && near(r[2], 20, 0.11) && near(r[3], 10, 0.11), Json(r).dump().c_str());
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list[0].rect_mm, (std::array<double, 4>{10, 10, 20, 10}));
        // its bottom right handle to (40, 30)
        b.click(QPointF(20, 15));
        b.drag(QPointF(30, 20), QPointF(40, 30));
        r = b.slices().list[0].rect_mm;
        QVERIFY2(near(r[0], 10) && near(r[1], 10) && near(r[2], 30) && near(r[3], 20), Json(r).dump().c_str());
        b.act("act_undo")->trigger();
        // an auto slice (below it) dragged: a user slice where it is put
        b.drag(QPointF(5, 40), QPointF(6, 41));
        QCOMPARE(b.slices().list.size(), std::size_t(2));
        QCOMPARE(b.slices().list[1].origin, std::string("user"));
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        // ユーザー定義スライスに変換 of an auto slice chosen
        b.click(QPointF(5, 40));
        QVERIFY(b.canvas()->chosen_slice_now() && !b.canvas()->chosen_slice_now()->id);
        b.find<QPushButton>("slice_promote")->click();
        QCOMPARE(b.slices().list.size(), std::size_t(2));
        QVERIFY(b.canvas()->chosen_slice_now() && b.canvas()->chosen_slice_now()->id);
        b.act("act_undo")->trigger();
        // 分割…: 2 down × 3 across
        b.click(QPointF(20, 15));
        b.answers.responder->exec = [](QDialog* dialog) {
            dialog->findChild<QCheckBox*>(QStringLiteral("divide_horizontal"))->setChecked(true);
            dialog->findChild<QSpinBox*>(QStringLiteral("divide_rows"))->setValue(2);
            dialog->findChild<QCheckBox*>(QStringLiteral("divide_vertical"))->setChecked(true);
            dialog->findChild<QSpinBox*>(QStringLiteral("divide_columns"))->setValue(3);
            return int(QDialog::Accepted);
        };
        b.find<QPushButton>("slice_divide")->click();
        QCOMPARE(b.slices().list.size(), std::size_t(6));
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        // スライスオプション… by a double click: its name, URL, kind
        b.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QLatin1String("slice_options")) return int(QDialog::Rejected);
            dialog->findChild<QLineEdit*>(QStringLiteral("slice_name"))->setText(QStringLiteral("logo"));
            dialog->findChild<QLineEdit*>(QStringLiteral("slice_url"))->setText(QStringLiteral("https://example.org/"));
            auto* kind = dialog->findChild<QComboBox*>(QStringLiteral("slice_kind"));
            kind->setCurrentIndex(kind->findData(QStringLiteral("noImage")));
            return int(QDialog::Accepted);
        };
        QTest::mouseDClick(b.canvas(), Qt::LeftButton, Qt::NoModifier, b.canvas()->to_widget(QPointF(20, 15)).toPoint());
        QCoreApplication::processEvents();
        QCOMPARE(b.slices().list[0].name, std::string("logo"));
        QCOMPARE(b.slices().list[0].url, std::string("https://example.org/"));
        QCOMPARE(b.slices().list[0].kind, std::string("noImage"));
        QCOMPARE(b.slices().list[0].rect_mm, (std::array<double, 4>{10, 10, 20, 10}));  // (寸法 untouched: the rect as it was)
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list[0].name, std::string("hero"));
        // 削除
        b.click(QPointF(20, 15));
        b.find<QPushButton>("slice_delete")->click();
        QVERIFY(b.page().slices && b.slices().list.empty());
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
    }

    // 新規レイヤーベースのスライス (the layer drawn on), 表示 → スライス, スライスをロック refusing the gestures, スライスを消去.
    void layerBasedLockAndClear() {
        Book b;
        b.window->set_target_layer("a");
        b.act("act_layer_slice")->trigger();
        QVERIFY2(b.page().slices && b.slices().list.size() == 1, qPrintable(b.window->last_notice()));
        QCOMPARE(b.slices().list[0].origin, std::string("layer"));
        QCOMPARE(*b.slices().list[0].layer, std::string("a"));
        const auto r = b.slices().list[0].rect_mm;
        QVERIFY2(near(r[0], 10, 0.2) && near(r[1], 30, 0.2) && near(r[2], 10, 0.3) && near(r[3], 10, 0.3), Json(r).dump().c_str());
        // the extra shown and hidden
        QVERIFY(b.canvas()->extra_shown(QStringLiteral("slices")));
        b.act("act_show_slices")->trigger();
        QVERIFY(!b.canvas()->extra_shown(QStringLiteral("slices")));
        b.act("act_show_slices")->trigger();
        QVERIFY(b.canvas()->extra_shown(QStringLiteral("slices")));
        // スライスをロック: a drag draws nothing, said
        b.act("act_lock_slices")->trigger();
        QVERIFY(b.canvas()->lock_slices);
        b.act("act_slice_tool")->trigger();
        b.drag(QPointF(40, 5), QPointF(55, 15));
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("ロック")), qPrintable(b.window->last_notice()));
        b.act("act_clear_slices")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        b.act("act_lock_slices")->trigger();
        // スライスを消去 (one Undo)
        b.act("act_clear_slices")->trigger();
        QVERIFY(b.slices().list.empty());
        b.act("act_undo")->trigger();
        QCOMPARE(b.slices().list.size(), std::size_t(1));
        // スナップ先 → スライス: the canvas's switch
        QVERIFY(b.act("act_snap_slices")->isChecked());
        b.act("act_snap_slices")->trigger();
        QVERIFY(!b.canvas()->snap_slices);
        b.act("act_snap_slices")->trigger();
        QVERIFY(b.canvas()->snap_slices);
    }

    // Web 用に保存（従来）… by the page's slices: スライスごとに書き出す (on when the page has slices), a folder asked, a picture
    // a slice in its images/ and the HTML.
    void saveForWebBySlices() {
        Book b;
        b.window->apply_ops(Json::array({Json{{"op", "add_slice"}, {"page", 1}, {"rect_mm", {10, 10, 20, 10}}, {"name", "hero"}}}));
        const QString out = b.tmp.path() + QStringLiteral("/web");
        QDir().mkpath(out);
        b.answers.responder->existing_dir = [out](const QString&, const QString&) { return out; };
        bool asked = false;
        b.answers.responder->exec = [&asked](QDialog* dialog) {
            auto* web = qobject_cast<app::SaveForWebDialog*>(dialog);
            if (web == nullptr) return int(QDialog::Rejected);
            asked = true;
            if (!web->by_slices->isEnabled() || !web->by_slices->isChecked()) return int(QDialog::Rejected);
            web->with_html->setChecked(true);
            return int(QDialog::Accepted);
        };
        b.act("act_save_for_web")->trigger();
        QVERIFY(asked);
        QVERIFY2(QFileInfo::exists(out + QStringLiteral("/images/hero.png")), qPrintable(b.window->last_notice()));
        QVERIFY(QFileInfo::exists(out + QStringLiteral("/p001.html")));
        QVERIFY(QFileInfo::exists(out + QStringLiteral("/images/spacer.gif")));
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("スライスを")), qPrintable(b.window->last_notice()));
        // (PC5 J-b review P3-3: files of those names there — asked once before anything is written; no: as they were)
        const QString hero = out + QStringLiteral("/images/hero.png");
        genko::test::write_bytes(hero, "old");
        int questions = 0;
        b.answers.responder->question = [&questions](const QString&, const QString& text) {
            ++questions;
            return !text.contains(QStringLiteral("置き換えますか")) ? true : false;
        };
        b.act("act_save_for_web")->trigger();
        QCOMPARE(questions, 1);
        QCOMPARE(genko::test::read_bytes(hero), std::string("old"));
        b.answers.responder->question = [&questions](const QString&, const QString&) {
            ++questions;
            return true;
        };
        b.act("act_save_for_web")->trigger();
        QCOMPARE(questions, 2);
        QVERIFY(genko::test::read_bytes(hero) != std::string("old"));
    }
};

QTEST_MAIN(TestGuiSlices)
#include "test_gui_slices.moc"
