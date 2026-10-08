// The layer panel (M3①-5, Python's LayerPanel in genko/app/main.py) on the offscreen platform: the page's layers front
// first, adding, moving, copying, taking away, each layer's settings, several at once, masks and effects, fill and
// correction layers, a filter with its result on the page while its numbers change; every change an op (Undo, saved).
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSlider>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
using namespace genko;
using core::Json;

namespace {

core::Document book_doc() {
    auto doc = core::new_episode("レイヤー", core::Num(1), 2, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    doc = core::CommandBus().apply(doc, Json::array({
        Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"points", Json::array({Json::array({8.0, 8.0, 0.7}), Json::array({30.0, 28.0, 0.7})})},
             {"stabilize", 0}}}), core::Actor("human:tester")).doc;
    return doc;
}

// A window on a book of its own; the panel's work through the window's ops.
struct Book {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::string ink;  // the page's ink layer (ペン入れ)
    explicit Book(const core::Document& doc = book_doc()) {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, doc);
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        if (const auto* layer = gui_test::ink_of(session->document().page(0))) ink = layer->id;
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 800);
        window->show();
        for (QDockWidget* dock : window->findChildren<QDockWidget*>())  // (the layers' tab in front of the pages')
            if (dock->objectName() == QStringLiteral("レイヤー")) dock->raise();
        QCoreApplication::processEvents();
    }
    app::LayerPanel& panel() const { return *window->layer_panel(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers) if (l.id == id) return &l;
        return nullptr;
    }
    std::vector<std::string> order() const {
        std::vector<std::string> out;
        for (const auto& l : page().layers) out.push_back(l.id);
        return out;
    }
    // choose a layer in the list as a person clicks it
    bool choose(const std::string& id) const {
        const auto& ids = panel().ids();
        const auto at = std::find(ids.begin(), ids.end(), id);
        if (at == ids.end()) return false;
        panel().list()->setCurrentRow(static_cast<int>(at - ids.begin()));
        return true;
    }
    void undo() const { window->action(QStringLiteral("act_undo"))->trigger(); }
    template <class W> W* child(const char* name) const { return window->layer_panel()->findChild<W*>(QString::fromLatin1(name)); }
    QAction* menu_action(QMenu* menu, const QString& text) const {
        for (QAction* a : menu->actions()) if (a->text() == text) return a;
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr)
                for (QAction* b : a->menu()->actions()) if (b->text() == text) return b;
        return nullptr;
    }
};

}  // namespace

class TestGuiLayers : public QObject {
    Q_OBJECT
private slots:
    // The list: the page's layers front first, each shown or hidden, the one drawn on chosen; a drawn layer has a small
    // picture, an empty one none.
    void listsThePageFrontFirst() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        auto order = book.order();
        std::reverse(order.begin(), order.end());
        QCOMPARE(panel.ids(), order);
        QCOMPARE(panel.list()->count(), static_cast<int>(order.size()));
        for (int row = 0; row < panel.list()->count(); ++row) {
            const auto* layer = book.layer(order[static_cast<std::size_t>(row)]);
            QCOMPARE(panel.list()->item(row)->checkState(), layer->visible ? Qt::Checked : Qt::Unchecked);
            const bool drawn = layer->stroke_count() > 0;
            QCOMPARE(!panel.list()->item(row)->icon().isNull(), drawn);
        }
        QVERIFY(book.window->target_layer() != nullptr);
        QCOMPARE(panel.ids()[static_cast<std::size_t>(panel.list()->currentRow())], book.window->target_layer()->id);
        // another page: its layers
        book.window->go_to_page(2);
        QTRY_COMPARE(book.window->current_page()->index.json(), Json(2));
        QCOMPARE(panel.list()->count(), static_cast<int>(book.session->document().page(1).layers.size()));
    }

    // ＋ペン・＋ペイント・＋フォルダ just in front of the chosen layer (the new pen or paint layer drawn on next); ↑↓;
    // 複製; 消す after the question; each one step of Undo.
    void addMoveCopyTakeAway() {
        Book book;
        const std::string ink = book.ink;
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        const auto before = book.order();
        panel.add("paint", QStringLiteral("ペイント"));
        auto order = book.order();
        QCOMPARE(order.size(), before.size() + 1);
        const auto ink_at = std::find(order.begin(), order.end(), ink) - order.begin();
        const std::string paint = order[static_cast<std::size_t>(ink_at + 1)];
        QCOMPARE(book.layer(paint)->kind, core::LayerKind::Raster);
        QCOMPARE(book.layer(paint)->title, std::string("ペイント"));
        QCOMPARE(book.window->target_layer()->id, paint);
        QCOMPARE(panel.ids()[static_cast<std::size_t>(panel.list()->currentRow())], paint);

        panel.move(-1);  // ↓: behind the ink
        order = book.order();
        QCOMPARE(order[static_cast<std::size_t>(ink_at)], paint);
        QCOMPARE(order[static_cast<std::size_t>(ink_at + 1)], ink);
        book.undo();
        QCOMPARE(book.order()[static_cast<std::size_t>(ink_at + 1)], paint);

        QVERIFY(book.choose(ink));
        panel.duplicate();
        order = book.order();
        const std::string copy = order[static_cast<std::size_t>(ink_at + 1)];
        QVERIFY(copy != ink && copy != paint);
        QCOMPARE(book.layer(copy)->stroke_count(), book.layer(ink)->stroke_count());
        QCOMPARE(book.window->target_layer()->id, copy);

        const auto count = book.order().size();
        panel.remove();  // (the question answered no)
        QCOMPARE(book.order().size(), count);
        QVERIFY(answers.asked.last().contains(QStringLiteral("消しますか")));
        answers.responder->question = [](const QString&, const QString&) { return true; };
        panel.remove();
        QCOMPARE(book.order().size(), count - 1);
        QVERIFY(book.layer(copy) == nullptr);
        book.undo();
        QVERIFY(book.layer(copy) != nullptr);

        panel.add("folder", QStringLiteral("フォルダ"));
        bool folder = false;
        for (const auto& l : book.page().layers) folder = folder || l.kind == core::LayerKind::Folder;
        QVERIFY(folder);
    }

    // The settings of the chosen layer: each control one set_layer; the controls show the layer chosen next.
    void settingsOfTheChosenLayer() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        auto* name = book.child<QLineEdit>("layer_name");
        name->setText(QStringLiteral("主線"));
        emit name->editingFinished();
        QCOMPARE(book.layer(ink)->title, std::string("主線"));
        auto* opacity = book.child<QSlider>("layer_opacity");
        opacity->setValue(40);
        emit opacity->sliderReleased();
        QCOMPARE(book.layer(ink)->opacity, 0.4);
        auto* blend = book.child<QComboBox>("layer_blend");
        const int multiply = blend->findData(QStringLiteral("multiply"));
        QVERIFY(multiply > 0);
        blend->setCurrentIndex(multiply);
        emit blend->activated(multiply);
        QCOMPARE(book.layer(ink)->blend, std::string("multiply"));
        const std::vector<std::pair<const char*, std::function<bool(const core::Layer&)>>> boxes{
            {"layer_clip", [](const core::Layer& l) { return l.clip; }},
            {"layer_lock_alpha", [](const core::Layer& l) { return l.lock_alpha; }},
            {"layer_locked", [](const core::Layer& l) { return l.locked; }},
            {"layer_overhang", [](const core::Layer& l) { return !l.panel_clip; }},
            {"layer_draft", [](const core::Layer& l) { return !l.exportable; }},
            {"layer_reference", [](const core::Layer& l) { return l.reference; }}};
        for (const auto& [box_name, on] : boxes) {
            auto* box = book.child<QCheckBox>(box_name);
            QVERIFY2(box != nullptr && box->isEnabled(), box_name);
            const bool was = on(*book.layer(ink));
            box->click();
            QVERIFY2(on(*book.layer(ink)) != was, box_name);
            QCOMPARE(box->isChecked(), !was);
        }
        auto* tint = book.child<QComboBox>("layer_tint");
        tint->setCurrentIndex(1);
        emit tint->activated(1);
        QVERIFY(book.layer(ink)->color && Json(*book.layer(ink)->color) == Json::array({40, 110, 230}));
        // the list shows the marks
        const QString shown = panel.list()->item(static_cast<int>(std::find(panel.ids().begin(), panel.ids().end(), ink) - panel.ids().begin()))->text();
        QVERIFY2(shown.contains(QStringLiteral("主線")) && shown.contains(QStringLiteral("下描き")) && shown.contains(QStringLiteral("ロック")), qPrintable(shown));
        // another layer chosen: its settings
        std::string other;
        for (const auto& l : book.page().layers) if (l.id != ink && l.kind != core::LayerKind::Folder) other = l.id;
        QVERIFY(book.choose(other));
        QCOMPARE(opacity->value(), static_cast<int>(std::lround(book.layer(other)->opacity * 100)));
        QCOMPARE(book.child<QCheckBox>("layer_locked")->isChecked(), book.layer(other)->locked);
        QCOMPARE(book.window->target_layer()->id, other);
    }

    // The tick: shown or hidden; Alt+クリック (solo): this one alone, again: the others back.
    void showHideAndAlone() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        const auto row = static_cast<int>(std::find(panel.ids().begin(), panel.ids().end(), ink) - panel.ids().begin());
        panel.list()->item(row)->setCheckState(Qt::Unchecked);
        QVERIFY(!book.layer(ink)->visible);
        book.undo();
        QVERIFY(book.layer(ink)->visible);
        std::vector<std::string> shown;
        for (const auto& l : book.page().layers) if (l.visible) shown.push_back(l.id);
        QVERIFY(shown.size() >= 2);
        panel.solo(ink);
        for (const auto& l : book.page().layers) QCOMPARE(l.visible, l.id == ink);
        panel.solo(ink);
        std::vector<std::string> again;
        for (const auto& l : book.page().layers) if (l.visible) again.push_back(l.id);
        QCOMPARE(again, shown);
    }

    // まとめて: the layers chosen with Ctrl+click merged, put in a folder, hidden, locked; the drafts all hidden.
    void severalAtOnce() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        panel.add("pen", QStringLiteral("ペン2"));
        const std::string pen = book.window->target_layer()->id;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", pen},
                                                         {"points", Json::array({Json::array({20.0, 8.0, 0.7}), Json::array({8.0, 30.0, 0.7})})}, {"stabilize", 0}}})));
        const auto row_of = [&](const std::string& id) {
            return static_cast<int>(std::find(panel.ids().begin(), panel.ids().end(), id) - panel.ids().begin());
        };
        // a click on a layer's name, as a person does it (Ctrl: one more; Shift: all from the last one)
        const auto click = [&](const std::string& id, Qt::KeyboardModifiers modifiers) {
            QListWidget* list = panel.list();
            QListWidgetItem* item = list->item(row_of(id));
            list->scrollToItem(item);
            const QRect r = list->visualItemRect(item);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, modifiers, QPoint(r.left() + r.width() * 3 / 4, r.center().y()));
        };
        const auto pick = [&](std::initializer_list<std::string> ids) {
            bool first = true;
            for (const auto& id : ids) {
                click(id, first ? Qt::NoModifier : Qt::ControlModifier);
                first = false;
            }
        };
        // one only: a word, nothing done
        pick({ink});
        const auto count = book.order().size();
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーを結合"))->trigger();
        QCOMPARE(book.order().size(), count);

        pick({ink, pen});
        QCOMPARE(panel.selected_ids().size(), std::size_t(2));
        QCOMPARE(book.window->target_layer()->id, pen);  // (the last one clicked is drawn on)
        // Shift+click: the layers from the one clicked before to this one
        std::string third;
        for (const auto& l : book.page().layers) if (l.id != ink && l.id != pen && l.kind != core::LayerKind::Folder) third = l.id;
        click(third, Qt::NoModifier);
        click(pen, Qt::ShiftModifier);
        const int a = row_of(third), b = row_of(pen);
        QCOMPARE(panel.selected_ids().size(), static_cast<std::size_t>(std::abs(a - b) + 1));
        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーを隠す"))->trigger();
        QVERIFY(!book.layer(ink)->visible && !book.layer(pen)->visible);
        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーを見せる"))->trigger();
        QVERIFY(book.layer(ink)->visible && book.layer(pen)->visible);
        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーをロック"))->trigger();
        QVERIFY(book.layer(ink)->locked && book.layer(pen)->locked);
        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーのロックを外す"))->trigger();
        QVERIFY(!book.layer(ink)->locked && !book.layer(pen)->locked);

        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーをフォルダにまとめる"))->trigger();
        QVERIFY(core::py_truthy(book.layer(ink)->parent_id) && book.layer(ink)->parent_id == book.layer(pen)->parent_id);
        book.undo();
        QVERIFY(!core::py_truthy(book.layer(ink)->parent_id));

        pick({ink, pen});
        book.menu_action(panel.many_menu(), QStringLiteral("選んだレイヤーを結合"))->trigger();
        QCOMPARE(book.order().size(), count - 1);
        QVERIFY(book.layer(pen) == nullptr);
        QCOMPARE(book.layer(ink)->stroke_count(), std::size_t(0));  // (drawn as they show, into the lowest: pixels)
        QVERIFY(book.layer(ink)->raster_png);
        QCOMPARE(book.window->target_layer()->id, ink);

        // the drafts: none on this page yet, then one
        QVERIFY(book.choose(ink));
        book.child<QCheckBox>("layer_draft")->click();
        book.menu_action(panel.many_menu(), QStringLiteral("下描きを全部隠す"))->trigger();
        QVERIFY(!book.layer(ink)->visible);
        book.menu_action(panel.many_menu(), QStringLiteral("下描きを全部見せる"))->trigger();
        QVERIFY(book.layer(ink)->visible);
    }

    // 下と結合: into the layer under it, which is then the one drawn on.
    void mergeDown() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        panel.add("pen", QStringLiteral("ペン2"));
        const std::string pen = book.window->target_layer()->id;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", pen},
                                                         {"points", Json::array({Json::array({20.0, 8.0, 0.7}), Json::array({8.0, 30.0, 0.7})})}, {"stabilize", 0}}})));
        const auto strokes = book.layer(ink)->stroke_count() + 1;
        panel.merge_down();
        QVERIFY(book.layer(pen) == nullptr);
        QCOMPARE(book.layer(ink)->stroke_count(), strokes);
        QCOMPARE(book.window->target_layer()->id, ink);
    }

    // マスク: from the selection (none: a word), all shown, inverted, not used, taken away; 効果: a border, a halftone,
    // the tint printed.
    void masksAndEffects() {
        Book book;
        const std::string ink = book.ink;
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        book.menu_action(panel.mask_menu(), QStringLiteral("選択範囲からマスクを作る"))->trigger();
        QVERIFY(!book.layer(ink)->mask);
        book.menu_action(panel.mask_menu(), QStringLiteral("全部見せるマスクを作る"))->trigger();
        QVERIFY(book.layer(ink)->mask && book.layer(ink)->mask->enabled);
        const auto shown = book.layer(ink)->mask->png;
        book.menu_action(panel.mask_menu(), QStringLiteral("マスクを反転"))->trigger();
        QVERIFY(*book.layer(ink)->mask->png != *shown);
        auto* off = book.menu_action(panel.mask_menu(), QStringLiteral("マスクを使わない"));
        off->setChecked(true);
        QVERIFY(!book.layer(ink)->mask->enabled);
        off->setChecked(false);
        QVERIFY(book.layer(ink)->mask->enabled);
        book.menu_action(panel.mask_menu(), QStringLiteral("マスクを消す"))->trigger();
        QVERIFY(!book.layer(ink)->mask);
        QVERIFY(!off->isChecked());

        answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(0.8); };
        answers.responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(250, 20, 30)); };
        book.menu_action(panel.effects_menu(), QStringLiteral("フチをつける…"))->trigger();
        QVERIFY(book.layer(ink)->effect);
        QCOMPARE((*book.layer(ink)->effect)["border"], (Json{{"width_mm", 0.8}, {"rgb", Json::array({250, 20, 30})}}));
        book.menu_action(panel.effects_menu(), QStringLiteral("水彩境界…"))->trigger();
        QCOMPARE((*book.layer(ink)->effect)["water_edge"], (Json{{"width_mm", 0.8}, {"strength", 0.7}}));
        book.menu_action(panel.effects_menu(), QStringLiteral("境界効果を外す"))->trigger();
        QVERIFY(!book.layer(ink)->effect || !core::py_truthy(*book.layer(ink)->effect));

        answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("screen_dialog")) return int(QDialog::Rejected);
            return int(QDialog::Accepted);
        };
        book.menu_action(panel.effects_menu(), QStringLiteral("トーン化（グレーを網点で印刷）…"))->trigger();
        QVERIFY(book.layer(ink)->screen && core::py_truthy(*book.layer(ink)->screen));
        book.menu_action(panel.effects_menu(), QStringLiteral("トーン化を外す"))->trigger();
        QVERIFY(!book.layer(ink)->screen || !core::py_truthy(*book.layer(ink)->screen));

        auto* prints = book.menu_action(panel.effects_menu(), QStringLiteral("表示色で印刷する"));
        prints->setChecked(true);
        QVERIFY(book.layer(ink)->color_prints);
    }

    // ＋塗り・補正: a fill of the colour asked, a gradient, a correction with its numbers (the dialog's fields); a
    // double click opens its numbers again.
    void fillAndCorrectionLayers() {
        Book book;
        const std::string ink = book.ink;
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        answers.responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(10, 120, 200)); };
        panel.add_fill();
        const core::Layer* fill = book.window->target_layer();
        QCOMPARE(fill->kind, core::LayerKind::Fill);
        QCOMPARE(*fill->fill, (Json{{"rgb", Json::array({10, 120, 200})}}));
        answers.responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(1, 2, 3)); };
        panel.edit_special();
        QCOMPARE(*book.window->target_layer()->fill, (Json{{"rgb", Json::array({1, 2, 3})}}));

        answers.responder->exec = [](QDialog* dialog) { return int(dialog->objectName() == QStringLiteral("gradient_dialog") ? QDialog::Accepted : QDialog::Rejected); };
        panel.add_gradient();
        QVERIFY(book.window->target_layer()->fill && book.window->target_layer()->fill->contains("gradient"));

        double black = 33;
        answers.responder->exec = [&black](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("filter_dialog")) return int(QDialog::Rejected);
            auto* field = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_black"));
            if (field == nullptr) return int(QDialog::Rejected);
            field->setValue(black);
            return int(QDialog::Accepted);
        };
        book.menu_action(panel.special_menu(), QStringLiteral("レベル補正…"))->trigger();
        const core::Layer* adjust = book.window->target_layer();
        QCOMPARE(adjust->kind, core::LayerKind::Adjust);
        QCOMPARE((*adjust->adjust)["kind"], Json("levels"));
        QCOMPARE((*adjust->adjust)["black"], Json(33.0));
        black = 50;
        const auto row = static_cast<int>(std::find(panel.ids().begin(), panel.ids().end(), adjust->id) - panel.ids().begin());
        emit panel.list()->itemDoubleClicked(panel.list()->item(row));
        QCOMPARE((*book.window->target_layer()->adjust)["black"], Json(50.0));
        // stopped: nothing added
        const auto count = book.order().size();
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        panel.add_adjust("hue");
        QCOMPARE(book.order().size(), count);
    }

    // かける: after the question, the filter's numbers in a dialog while the page shows the result; かける applies it
    // (pen lines become pixels), やめる leaves the page as it was.
    void filterWithItsResultShown() {
        Book book;
        const std::string ink = book.ink;
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        auto* filter = panel.filter_box();
        filter->setCurrentIndex(filter->findData(QStringLiteral("blur")));
        answers.responder->question = [](const QString&, const QString&) { return true; };
        bool previewed = false;
        auto* canvas = book.window->canvas();
        const auto with_preview = [&](int answer) {
            return [&, answer](QDialog* dialog) {
                if (dialog->objectName() != QStringLiteral("filter_dialog")) return int(QDialog::Rejected);
                dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"))->setValue(4);
                // the page while the dialog is open: the layer blurred, the book itself as it was
                previewed = gui_test::wait_for([&] {
                    return canvas->doc() != book.session->snapshot() && canvas->doc()->page(0).layers.size() == book.page().layers.size() &&
                           [&] { for (const auto& l : canvas->doc()->page(0).layers) if (l.id == ink) return l.stroke_count() == 0 && bool(l.raster_png); return false; }();
                }, 10000);
                return answer;
            };
        };
        answers.responder->exec = with_preview(QDialog::Rejected);
        panel.apply_filter();
        QVERIFY(previewed);
        QVERIFY(book.layer(ink)->stroke_count() > 0);
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());

        previewed = false;
        answers.responder->exec = with_preview(QDialog::Accepted);
        panel.apply_filter();
        QVERIFY(previewed);
        QCOMPARE(book.layer(ink)->stroke_count(), std::size_t(0));
        QCOMPARE(book.layer(ink)->kind, core::LayerKind::Raster);
        QVERIFY(book.layer(ink)->raster_png);
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());
        book.undo();
        QVERIFY(book.layer(ink)->stroke_count() > 0);

        // a filter without numbers: straight on, after the question
        filter->setCurrentIndex(filter->findData(QStringLiteral("invert")));
        answers.asked.clear();
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        panel.apply_filter();
        QCOMPARE(book.layer(ink)->stroke_count(), std::size_t(0));
        QVERIFY(book.layer(ink)->raster_png);
    }

    // On high-precision pixels the panel's filter keeps them precise.
    void filterOnPrecisePixels() {
        auto doc = core::new_episode("高精度", core::Num(1), 1, core::PageSpec::custom(20, 20, 16, 16, 1, 2, 2, 2, 2, 72, "color"));
        doc.edit_page(0).layers.clear();
        Json pixels = Json::array();
        for (int i = 0; i < 8 * 8; ++i) for (const int v : {i * 1000, 30000, 50000, 65535}) pixels.push_back(v);
        doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 8}, {"height", 8},
                                                             {"precision", "u16"}, {"pixels", pixels}}}), core::Actor("human:tester")).doc;
        doc.edit_page(0).layers[0].id = "paint";
        Book book(doc);
        gui_test::Answers answers;
        answers.responder->question = [](const QString&, const QString&) { return true; };
        auto& panel = book.panel();
        QVERIFY(book.choose("paint"));
        panel.filter_box()->setCurrentIndex(panel.filter_box()->findData(QStringLiteral("invert")));
        const auto before = book.layer("paint")->color_raster;
        panel.apply_filter();
        QVERIFY(book.layer("paint")->color_raster && book.layer("paint")->color_raster != before);
        const core::ColorRasterView after(*book.layer("paint")->color_raster);
        QCOMPARE(after.metadata("")["precision"], Json("u16"));
        QVERIFY(std::abs(after.pixel(3)[0] - (1 - 3000 / 65535.0)) < 1e-9);
    }
};

QTEST_MAIN(TestGuiLayers)
#include "test_gui_layers.moc"
