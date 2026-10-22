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
#include <QSpinBox>
#include "gui_support.hpp"
#include "app/adjust_forms.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/workspace.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
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
            // (its picture when drawn on, else a quiet mark of its kind: every row has one)
            QVERIFY(!panel.list()->item(row)->icon().isNull());
            QCOMPARE(panel.list()->item(row)->data(Qt::UserRole + 1).toString() == QStringLiteral("picture"), drawn);
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
        book.menu_action(panel.mask_menu(), QStringLiteral("選択範囲外をマスク"))->trigger();
        QVERIFY(!book.layer(ink)->mask);
        book.menu_action(panel.mask_menu(), QStringLiteral("すべての領域を表示"))->trigger();
        QVERIFY(book.layer(ink)->mask && book.layer(ink)->mask->enabled);
        const auto shown = book.layer(ink)->mask->png;
        book.menu_action(panel.mask_menu(), QStringLiteral("マスクを反転"))->trigger();
        QVERIFY(*book.layer(ink)->mask->png != *shown);
        auto* off = book.menu_action(panel.mask_menu(), QStringLiteral("使用しない"));
        off->setChecked(true);
        QVERIFY(!book.layer(ink)->mask->enabled);
        off->setChecked(false);
        QVERIFY(book.layer(ink)->mask->enabled);
        book.menu_action(panel.mask_menu(), QStringLiteral("削除"))->trigger();
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
        // (PC2a, the user's decision 3: levels has its own window, app/adjust_forms_tone — "adjust_dialog" —, and a correction
        // made new starts in PhotoCraft's form: its formula upstream)
        answers.responder->exec = [&black](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
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
        QCOMPARE((*adjust->adjust)["native"]["method"], Json("upstream"));
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

    // PC2 (adjustments@1): a correction layer's native settings (its blend…) are kept when its numbers are changed in
    // Python's window, which does not hold them.
    void correctionKeepsItsNative() {
        auto doc = book_doc();
        doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                              {"adjust", Json{{"kind", "levels"}, {"black", 10}, {"native", Json{{"blend", "multiply"}}}}}}}),
                                       core::Actor("human:tester")).doc;
        Book book(doc);
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose("adj"));
        // (PC2a, the user's decision 3: levels' own window, app/adjust_forms_tone — an existing one in its own form, Genko's)
        answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_black"))->setValue(40);
            return int(QDialog::Accepted);
        };
        panel.edit_special();
        const core::Layer* adjust = book.layer("adj");
        QCOMPARE((*adjust->adjust)["black"], Json(40.0));
        QCOMPARE((*adjust->adjust)["native"], (Json{{"blend", "multiply"}}));
        QVERIFY2(!adjust->adjust->contains("gamma"), adjust->adjust->dump().c_str());  // (what was not changed: not added)
    }

    // (the user's decision 3: an existing correction is set again in its own form) Python's levels and tone curve on one
    // colour only (`channel`: r, g, b) keep it through their windows — the window shows it and changes it —, and what a
    // window does not change stays as it was (nothing changed: the same correction).
    void correctionKeepsItsChannel() {
        auto doc = book_doc();
        doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "red"},
                                                              {"adjust", Json{{"kind", "levels"}, {"black", 20}, {"white", 200}, {"channel", "r"}}}},
                                                         Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "green"},
                                                              {"adjust", Json{{"kind", "curve"}, {"points", Json::array({Json::array({0, 0}), Json::array({128, 160}), Json::array({255, 255})})},
                                                                              {"channel", "g"}}}}}),
                                       core::Actor("human:tester")).doc;
        Book book(doc);
        gui_test::Answers answers;
        auto& panel = book.panel();
        QString shown;
        answers.responder->exec = [&shown](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
            auto* channel = dialog->findChild<QComboBox*>(QStringLiteral("field_channel"));
            shown = channel != nullptr ? channel->currentText() : QStringLiteral("(no field)");
            return int(QDialog::Accepted);
        };
        for (const auto& [id, words] : {std::pair<std::string, QString>{"red", QStringLiteral("赤")}, std::pair<std::string, QString>{"green", QStringLiteral("緑")}}) {
            const Json before = *book.layer(id)->adjust;
            QVERIFY(book.choose(id));
            panel.edit_special();
            QCOMPARE(shown, words);
            QVERIFY2(*book.layer(id)->adjust == before, book.layer(id)->adjust->dump().c_str());
        }
        // the colour changed in the window: blue only
        answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
            auto* channel = dialog->findChild<QComboBox*>(QStringLiteral("field_channel"));
            channel->setCurrentIndex(channel->findText(QStringLiteral("青")));
            return int(QDialog::Accepted);
        };
        QVERIFY(book.choose("red"));
        panel.edit_special();
        QCOMPARE(*book.layer("red")->adjust, (Json{{"kind", "levels"}, {"black", 20}, {"white", 200}, {"channel", "b"}}));
    }

    // (the user's decision 2) A correction layer's 合成 in the layer panel is its native.blend — what it is drawn by —,
    // not the layer's own blend, which a correction does not use: shown, set (set_adjustment) and listed so.
    void correctionBlendIsItsNative() {
        auto doc = book_doc();
        doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"},
                                                              {"adjust", Json{{"kind", "invert"}, {"native", Json{{"blend", "screen"}}}}}}}),
                                       core::Actor("human:tester")).doc;
        Book book(doc);
        auto& panel = book.panel();
        QVERIFY(book.choose("adj"));
        auto* blend = book.child<QComboBox>("layer_blend");
        QCOMPARE(blend->currentData().toString(), QStringLiteral("screen"));
        const int multiply = blend->findData(QStringLiteral("multiply"));
        blend->setCurrentIndex(multiply);
        emit blend->activated(multiply);
        const core::Layer* layer = book.layer("adj");
        QCOMPARE((*layer->adjust)["native"]["blend"], Json("multiply"));
        QCOMPARE(layer->blend, std::string("normal"));
        const auto& ids = panel.ids();
        const auto row = static_cast<int>(std::find(ids.begin(), ids.end(), std::string("adj")) - ids.begin());
        QVERIFY2(panel.list()->item(row)->data(Qt::UserRole + 2).toString().contains(QStringLiteral("乗算")),
                 qPrintable(panel.list()->item(row)->data(Qt::UserRole + 2).toString()));
        // back to normal: the native's blend taken out
        const int normal = blend->findData(QStringLiteral("normal"));
        blend->setCurrentIndex(normal);
        emit blend->activated(normal);
        QVERIFY2(!book.layer("adj")->adjust->contains("native"), book.layer("adj")->adjust->dump().c_str());
    }

    // PC2's form of a correction's settings (app/adjust_forms): its fields from the kinds' table or its own, a native
    // setting by its path, the values set and read back, back to the defaults; a kind without a form has no window.
    void adjustForm() {
        app::adjust_forms::Form form;
        int changes = 0;
        QObject::connect(&form, &app::adjust_forms::Form::changed, [&changes] { ++changes; });
        form.from_table(*core::adjust_kind("vibrance"), {{"vibrance", QStringLiteral("自然な彩度")}, {"saturation", QStringLiteral("彩度")},
                                                         {"native.blend", QStringLiteral("描画モード")}});
        form.check("preserve", QStringLiteral("輝度を保持"), true);
        form.colour("color", QStringLiteral("色"), {236, 138, 0});
        QCOMPARE(form.values(), (Json{{"vibrance", 0}, {"saturation", 0}, {"native", Json{{"blend", "normal"}}}, {"preserve", true},
                                      {"color", Json::array({236, 138, 0})}}));
        form.set_values(Json{{"vibrance", 35}, {"native", Json{{"blend", "screen"}}}, {"color", Json::array({1, 2, 3})}});
        QCOMPARE(changes, 1);
        QCOMPARE(form.values()["vibrance"], Json(35));
        QCOMPARE(form.values()["native"]["blend"], Json("screen"));
        QCOMPARE(form.values()["color"], Json::array({1, 2, 3}));
        form.findChild<QDoubleSpinBox*>(QStringLiteral("field_saturation"))->setValue(-20);
        QCOMPARE(changes, 2);
        QCOMPARE(form.values()["saturation"], Json(-20));
        form.reset();
        QCOMPARE(form.values()["vibrance"], Json(0));
        QCOMPARE(form.values()["native"]["blend"], Json("normal"));
        QVERIFY(!app::adjust_forms::adjust_dialog(nullptr, "blur", Json::object(), {}));  // (a kind without a form: no window)
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

    // PC3 B2 (the user's decision 5): レイヤー → レイヤーマスク by Photoshop's names — すべての領域を隠す (a new mask linked),
    // 選択範囲をマスク (the selection let go), 透明部分から, 適用, リンク (the row says it, and it can be taken off) — the
    // 属性 panel's 濃度 and ぼかし, and ファイル → スクリプト → すべてのマスクを統合 (one Undo).
    void photoshopMaskCommands() {
        Book book;
        const std::string ink = book.ink;
        auto& panel = book.panel();
        QVERIFY(book.choose(ink));
        QMenu* menu = nullptr;
        for (QMenu* m : book.window->findChildren<QMenu*>())
            if (m->title() == QStringLiteral("レイヤーマスク")) menu = m;
        QVERIFY(menu != nullptr);
        QStringList items;
        for (QAction* a : menu->actions()) if (!a->isSeparator()) items << a->text();
        for (const char* name : {"すべての領域を表示", "すべての領域を隠す", "選択範囲外をマスク", "選択範囲をマスク", "透明部分から", "削除", "適用", "使用しない", "リンク"})
            QVERIFY2(items.contains(QString::fromUtf8(name)), name);
        // (the renamed ones: each a command under its new name, none under its old; a key chosen under an old name kept —
        // the words the vector mask's (PC3 C-a) and the page's すべての領域を表示 (PC3 C-e) have too, by their place)
        QSet<QString> now;
        for (const auto& [action, name] : app::command_names(book.window.get())) now.insert(name);
        for (const auto& [name, before] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"すべての領域を表示（レイヤー → レイヤーマスク）", "全部見せるマスクを作る"}, {"選択範囲外をマスク", "選択範囲からマスクを作る"},
                 {"削除（レイヤー → レイヤーマスク）", "マスクを消す"}, {"使用しない（レイヤー → レイヤーマスク）", "マスクを使わない"}}) {
            bool kept = false;
            for (auto [it, end] = app::preferences::renamed().equal_range(QString::fromUtf8(name)); it != end; ++it) kept = kept || it->second == QString::fromUtf8(before);
            QVERIFY2(kept && now.contains(QString::fromUtf8(name)) && !now.contains(QString::fromUtf8(before)), name);
        }
        app::settings()->setValue(QStringLiteral("shortcuts/マスクを消す"), QStringLiteral("Ctrl+Alt+K"));
        app::preferences::apply_shortcuts(book.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/マスクを消す"));
        QCOMPARE(book.menu_action(panel.mask_menu(), QStringLiteral("削除"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
        book.menu_action(panel.mask_menu(), QStringLiteral("すべての領域を隠す"))->trigger();
        QVERIFY(book.layer(ink)->mask && book.layer(ink)->mask->linked);  // (a new mask moves with its layer)
        QVERIFY(panel.mask_link_action()->isChecked());
        QVERIFY(panel.list()->item(panel.list()->currentRow())->text().contains(QStringLiteral("マスク（リンク）")));
        panel.mask_link_action()->setChecked(false);
        QVERIFY(!book.layer(ink)->mask->linked);
        // 選択範囲をマスク: the selection hidden, then let go
        book.window->canvas()->set_selection(
            Json{{"poly", Json::array({Json::array({0, 0}), Json::array({20, 0}), Json::array({20, 40}), Json::array({0, 40})})}});
        book.menu_action(panel.mask_menu(), QStringLiteral("選択範囲をマスク"))->trigger();
        QVERIFY(!book.window->canvas()->selection());
        const render::Image hidden = render::open_image(*book.layer(ink)->mask->png).convert("L");  // (150 dpi: 236 px)
        QCOMPARE(int(hidden.getpixel(10, 50)[0]), 0);    // (inside: hidden)
        QCOMPARE(int(hidden.getpixel(200, 50)[0]), 255);  // (outside: shown)
        // 適用: the mask gone, the line's pixels where it showed
        book.menu_action(panel.mask_menu(), QStringLiteral("適用"))->trigger();
        QVERIFY(!book.layer(ink)->mask);
        QCOMPARE(book.layer(ink)->kind, core::LayerKind::Raster);
        book.undo();
        QVERIFY(book.layer(ink)->mask && book.layer(ink)->stroke_count() == 1);
        // 透明部分から
        book.menu_action(panel.mask_menu(), QStringLiteral("削除"))->trigger();
        book.menu_action(panel.mask_menu(), QStringLiteral("透明部分から"))->trigger();
        QVERIFY(book.layer(ink)->mask && book.layer(ink)->mask->linked);
        // 属性: 濃度 and ぼかし
        auto* density = book.window->findChild<QSpinBox*>(QStringLiteral("properties_mask_density"));
        if (density == nullptr) {
            for (QDockWidget* dock : book.window->findChildren<QDockWidget*>())
                if (dock->objectName() == QStringLiteral("属性")) dock->show();
            QCoreApplication::processEvents();
            QTest::qWait(400);
            density = book.window->findChild<QSpinBox*>(QStringLiteral("properties_mask_density"));
        }
        QVERIFY(density != nullptr);
        density->setValue(40);
        emit density->editingFinished();
        QVERIFY(std::abs(book.layer(ink)->mask->density - 0.4) < 1e-9);
        QTest::qWait(400);  // (the panel shown again from the book)
        auto* feather = book.window->findChild<QDoubleSpinBox*>(QStringLiteral("properties_mask_feather"));
        QVERIFY(feather != nullptr);
        feather->setValue(1.5);
        emit feather->editingFinished();
        QVERIFY(std::abs(book.layer(ink)->mask->feather_mm - 1.5) < 1e-9);
        // すべてのマスクを統合: every pen and paint layer with a mask, one Undo
        panel.add("paint", QStringLiteral("ペイント2"));
        const std::string paint = book.window->target_layer()->id;
        book.menu_action(panel.mask_menu(), QStringLiteral("すべての領域を表示"))->trigger();
        QVERIFY(book.layer(paint)->mask);
        const auto steps = book.window->session().history().done.size();
        book.window->action(QStringLiteral("act_flatten_masks"))->trigger();
        QVERIFY(!book.layer(ink)->mask && !book.layer(paint)->mask);
        QCOMPARE(book.window->session().history().done.size(), steps + 1);
        book.undo();
        QVERIFY(book.layer(ink)->mask && book.layer(paint)->mask);
    }

    // PC3 B2: 選択範囲 → すべてのレイヤー, レイヤーの選択を解除 (the layer drawn on stays chosen), レイヤーを検索 (Enter
    // chooses what it finds), レイヤーを分離 (only the chosen listed, until it is turned off).
    void layersChosen() {
        Book book;
        auto& panel = book.panel();
        panel.add("pen", QStringLiteral("空のペン"));
        panel.add("paint", QStringLiteral("空のペイント"));
        const std::string drawn_on = book.window->target_layer()->id;
        const auto chosen = [&] { return panel.selected_ids().size(); };
        book.window->action(QStringLiteral("act_select_all_layers"))->trigger();
        QCOMPARE(chosen(), panel.ids().size());
        book.window->action(QStringLiteral("act_deselect_layers"))->trigger();
        QCOMPARE(panel.selected_ids(), std::vector<std::string>{drawn_on});
        QCOMPARE(book.window->target_layer()->id, drawn_on);
        book.window->action(QStringLiteral("act_find_layers"))->trigger();
        QVERIFY(panel.search_box()->isVisible());
        panel.search_box()->setText(QStringLiteral("空の"));
        emit panel.search_box()->returnPressed();
        QCOMPARE(chosen(), std::size_t{2});
        panel.search_box()->clear();
        QAction* isolate = book.window->action(QStringLiteral("act_isolate_layers"));
        isolate->setChecked(true);
        int shown = 0;
        for (int row = 0; row < panel.list()->count(); ++row) shown += panel.list()->item(row)->isHidden() ? 0 : 1;
        QCOMPARE(shown, 2);
        QVERIFY(panel.isolated());
        isolate->setChecked(false);
        shown = 0;
        for (int row = 0; row < panel.list()->count(); ++row) shown += panel.list()->item(row)->isHidden() ? 0 : 1;
        QCOMPARE(shown, panel.list()->count());
    }
};

QTEST_MAIN(TestGuiLayers)
#include "test_gui_layers.moc"
