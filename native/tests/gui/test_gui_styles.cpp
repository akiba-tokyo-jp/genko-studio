// PC4 E6 (layer.layerStyle.*, style.presets.*, window.panel.styles; SPEC.md COLOR-09「レイヤースタイル」「スタイル」) on
// the offscreen platform: レイヤー → レイヤースタイル ▸ in Photoshop's order (before レイヤーマスク), the レイヤースタイル window
// (an effect's settings, the blending options, the list's boxes, ＋ and −, its swatch) setting the target layer's style in
// one Undo; コピー・ペースト・消去, 包括光源…, 効果を拡大・縮小…, すべての効果を隠す・表示, レイヤーを作成; the layer panel's fx
// mark; ウィンドウ → スタイル: the built-in groups with their swatches (made in the background), a click applying a style
// (Shift: added), 新規スタイル… from the layer, renamed, deleted, the built-in ones again.

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>

#include <filesystem>

#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/layer_style_dialog.hpp"
#include "app/main_window.hpp"
#include "app/styles_panel.hpp"
#include "core/command_bus.hpp"
#include "core/layer_styles.hpp"
#include "render/preset_store.hpp"
#include "render/style_presets.hpp"

using namespace genko;
using core::Json;

namespace {

core::Document book_doc() {
    auto doc = core::new_episode("スタイル", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    return core::CommandBus()
        .apply(doc,
               Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"points", Json::array({Json::array({10.0, 10.0, 0.7}), Json::array({30.0, 30.0, 0.7})})},
                                 {"stabilize", 0}}}),
               core::Actor("human:tester"))
        .doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::string ink_id = "ink";
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        window->canvas()->fit_page();
        for (const core::Layer& l : book().page(0).layers)
            if (l.role == core::LayerRole::Ink) ink_id = l.id;
        window->set_target_layer(ink_id);
    }
    const core::Document& book() const { return session->document(); }
    const core::Layer& ink() const {
        for (const core::Layer& l : book().page(0).layers)
            if (l.id == ink_id) return l;
        return book().page(0).layers.front();
    }
    void trigger(const char* name) {
        QAction* act = window->action(QString::fromLatin1(name));
        QVERIFY2(act != nullptr, name);
        act->trigger();
    }
    QStringList menus_of(QAction* act) const {
        QStringList out;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& top) {
            for (QAction* a : menu->actions()) {
                if (a == act) out << top;
                if (a->menu() != nullptr) walk(a->menu(), top);
            }
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->text());
        return out;
    }
};

QTreeWidgetItem* item_named(QTreeWidget* tree, const QString& name) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* group = tree->topLevelItem(i);
        for (int j = 0; j < group->childCount(); ++j)
            if (group->child(j)->text(0) == name) return group->child(j);
    }
    return nullptr;
}

void forget_libraries() {
    std::error_code ec;
    std::filesystem::remove(app::config_dir() / "styles.json", ec);
    std::filesystem::remove(app::config_dir() / "patterns.json", ec);
    std::filesystem::remove_all(app::config_dir() / "patterns", ec);
}

QStringList kinds_of(const core::Layer& layer) {
    QStringList out;
    if (!layer.style) return out;
    for (const Json& e : (*layer.style)["effects"]) out << QString::fromStdString(e["kind"].get<std::string>());
    return out;
}

}  // namespace

class TestGuiStyles : public QObject {
    Q_OBJECT
private slots:
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        forget_libraries();
    }

    // レイヤー → レイヤースタイル ▸: its commands in Photoshop's order, before レイヤーマスク
    void theMenu() {
        Studio s;
        QMenu* layers = nullptr;
        for (QAction* top : s.window->menuBar()->actions())
            if (top->text() == QStringLiteral("レイヤー")) layers = top->menu();
        QVERIFY(layers != nullptr);
        QMenu* styles = nullptr;
        int style_at = -1, mask_at = -1;
        const QList<QAction*> items = layers->actions();
        for (int i = 0; i < items.size(); ++i) {
            if (items[i]->text() == QStringLiteral("レイヤースタイル")) {
                styles = items[i]->menu();
                style_at = i;
            }
            if (items[i]->text() == QStringLiteral("レイヤーマスク")) mask_at = i;
        }
        QVERIFY(styles != nullptr);
        QVERIFY(style_at >= 0 && style_at + 1 == mask_at);
        QStringList names;
        for (QAction* a : styles->actions())
            if (!a->isSeparator()) names << a->text();
        QCOMPARE(names, (QStringList{QStringLiteral("混合オプション…"), QStringLiteral("ベベルとエンボス…"), QStringLiteral("境界線…"), QStringLiteral("シャドウ（内側）…"),
                                     QStringLiteral("光彩（内側）…"), QStringLiteral("サテン…"), QStringLiteral("カラーオーバーレイ…"),
                                     QStringLiteral("グラデーションオーバーレイ…"), QStringLiteral("パターンオーバーレイ…"), QStringLiteral("光彩（外側）…"),
                                     QStringLiteral("ドロップシャドウ…"), QStringLiteral("レイヤースタイルをコピー"), QStringLiteral("レイヤースタイルをペースト"),
                                     QStringLiteral("レイヤースタイルを消去"), QStringLiteral("包括光源…"), QStringLiteral("レイヤーを作成"),
                                     QStringLiteral("すべての効果を隠す"), QStringLiteral("すべての効果を表示"), QStringLiteral("効果を拡大・縮小…")}));
    }

    // the レイヤースタイル window: ドロップシャドウ… opens at its settings, switched on; its distance and colour set; the
    // blending options' fill and mode; OK sets the style in one step (Undo takes it back); a cancel changes nothing
    void theWindow() {
        Studio s;
        s.answers.responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(10, 20, 200)); };
        QStringList wrong;
        s.answers.responder->exec = [&wrong](QDialog* dialog) {
            auto* window = qobject_cast<app::LayerStyleDialog*>(dialog);
            if (window == nullptr) return int(QDialog::Rejected);
            QListWidget* list = window->list();
            if (list->count() != 11) wrong << QStringLiteral("rows %1").arg(list->count());
            if (list->item(0)->text() != QStringLiteral("混合オプション")) wrong << list->item(0)->text();
            if (list->item(10)->data(Qt::UserRole).toString() != QStringLiteral("drop_shadow")) wrong << list->item(10)->text();
            if (list->item(10)->checkState() != Qt::Checked) wrong << QStringLiteral("not on");  // (opened at it: on)
            auto* distance = window->findChild<QDoubleSpinBox*>(QStringLiteral("style_distance_mm"));
            if (distance == nullptr) return int(QDialog::Rejected);
            distance->setValue(2.5);
            window->findChild<QPushButton*>(QStringLiteral("style_rgb"))->click();
            // a stroke switched on by its box, then ＋: two strokes
            list->item(2)->setCheckState(Qt::Checked);
            list->setCurrentRow(2);
            window->findChild<QPushButton*>(QStringLiteral("layer_style_more"))->click();
            // the blending options: fill 50 %, the mode
            list->setCurrentRow(0);
            window->findChild<QSpinBox*>(QStringLiteral("style_fill_opacity"))->setValue(50);
            auto* mode = window->findChild<QComboBox*>(QStringLiteral("style_layer_blend"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("multiply")));
            // its swatch, made off the window's thread
            if (!gui_test::wait_for([&] { return !window->findChild<QLabel*>(QStringLiteral("layer_style_swatch"))->pixmap().isNull(); }))
                wrong << QStringLiteral("no swatch");
            return int(QDialog::Accepted);
        };
        QVERIFY(s.window->edit_layer_style("drop_shadow"));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral(", "))));
        const core::Layer& ink = s.ink();
        QCOMPARE(kinds_of(ink), (QStringList{QStringLiteral("stroke"), QStringLiteral("stroke"), QStringLiteral("drop_shadow")}));
        const Json& shadow = (*ink.style)["effects"][2];
        QCOMPARE(shadow["distance_mm"], Json(2.5));
        QCOMPARE(shadow["rgb"], Json::parse("[10, 20, 200]"));
        QCOMPARE((*ink.style)["fill_opacity"], Json(0.5));
        QCOMPARE(ink.blend, std::string("multiply"));
        // the layer panel's mark
        bool marked = false;
        for (int i = 0; i < s.window->layer_panel()->list()->count(); ++i)
            marked = marked || s.window->layer_panel()->list()->item(i)->text().contains(QStringLiteral("fx"));
        QVERIFY(marked);
        // one Undo: as it was
        s.trigger("act_undo");
        QVERIFY(!s.ink().style);
        QCOMPARE(s.ink().blend, std::string("normal"));
        // cancelled: nothing
        s.answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        QVERIFY(!s.window->edit_layer_style("stroke"));
        QVERIFY(!s.ink().style);
    }

    // コピー・ペースト・消去, 包括光源…, 効果を拡大・縮小…, すべての効果を隠す・表示, レイヤーを作成
    void theCommands() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", s.ink_id},
                                                      {"style", Json{{"effects", Json::array({Json{{"kind", "drop_shadow"}, {"size_mm", 1}}, Json{{"kind", "color_overlay"}}})}}}}})));
        s.trigger("act_layer_style_copy");
        s.trigger("act_layer_style_clear");
        QVERIFY(!s.ink().style);
        s.trigger("act_layer_style_paste");
        QCOMPARE(kinds_of(s.ink()), (QStringList{QStringLiteral("drop_shadow"), QStringLiteral("color_overlay")}));
        // 包括光源…
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("global_light_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("global_light_angle"))->setValue(45);
            return int(QDialog::Accepted);
        };
        s.trigger("act_global_light");
        QCOMPARE(core::global_light(s.book())[0], 45.0);
        // 効果を拡大・縮小…
        s.answers.responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(300); };
        s.trigger("act_layer_style_scale");
        QCOMPARE((*s.ink().style)["effects"][0]["size_mm"], Json(3));
        // すべての効果を隠す・表示
        s.trigger("act_layer_style_hide_all");
        QVERIFY(!core::style_shows_effects(*s.ink().style));
        s.trigger("act_layer_style_show_all");
        QVERIFY(core::style_shows_effects(*s.ink().style));
        // レイヤーを作成: two layers made, the layer's style gone
        const std::size_t layers = s.book().page(0).layers.size();
        s.trigger("act_layer_style_layers");
        QCOMPARE(s.book().page(0).layers.size(), layers + 2);
        QVERIFY(!s.ink().style);
    }

    // ウィンドウ → スタイル: the built-in groups and their swatches (in the background); a click applies, Shift+click adds;
    // 新規スタイル… from the layer; renamed and deleted; the built-in ones again
    void thePanel() {
        Studio s;
        QDockWidget* dock = s.window->styles_dock();
        QVERIFY(dock != nullptr && !dock->isVisible());
        QVERIFY(s.menus_of(dock->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        dock->toggleViewAction()->trigger();
        app::StylesPanel* panel = s.window->styles_panel();
        QVERIFY(panel != nullptr);
        panel->refresh();
        QTreeWidget* tree = panel->tree();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("基本"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("テキスト効果"));
        QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("ボタン"));
        QVERIFY(gui_test::wait_for([&] { return panel->pending() == 0; }, 60000));
        QVERIFY(!item_named(tree, QStringLiteral("ゴールド"))->icon(0).isNull());
        // a refresh: the swatches kept (none made again)
        panel->refresh();
        QCOMPARE(panel->pending(), 0);
        QVERIFY(!item_named(tree, QStringLiteral("ゴールド"))->icon(0).isNull());
        // a click: applied to the target layer; Shift: added
        emit tree->itemClicked(item_named(tree, QStringLiteral("ドロップシャドウ")), 0);
        QCOMPARE(kinds_of(s.ink()), QStringList{QStringLiteral("drop_shadow")});
        QVERIFY(panel->apply("黒の境界線", true));
        QCOMPARE(kinds_of(s.ink()), (QStringList{QStringLiteral("drop_shadow"), QStringLiteral("stroke")}));
        QVERIFY(panel->apply("デフォルトスタイル（なし）", false));
        QVERIFY(!s.ink().style);
        QVERIFY(panel->apply("中抜き", false));
        QCOMPARE((*s.ink().style)["fill_opacity"], Json(0));
        // 新規スタイル…: its name and what it takes
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("new_style_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QLineEdit*>(QStringLiteral("new_style_name"))->setText(QStringLiteral("自分の中抜き"));
            return int(QDialog::Accepted);
        };
        QVERIFY(s.window->new_style_preset());
        QTreeWidgetItem* mine = item_named(tree, QStringLiteral("自分の中抜き"));
        QVERIFY(mine != nullptr);
        const auto lib = render::preset_store::load(app::config_dir(), render::style_presets::kind());
        const Json* kept = render::preset_store::find(lib.groups, "自分の中抜き");
        QVERIFY(kept != nullptr);
        QCOMPARE((*kept)["fill_opacity"], Json(0));
        // renamed, deleted
        tree->setCurrentItem(mine);
        panel->rename(QStringLiteral("中抜き 2"));
        QVERIFY(item_named(tree, QStringLiteral("中抜き 2")) != nullptr);
        tree->setCurrentItem(item_named(tree, QStringLiteral("中抜き 2")));
        panel->remove();
        QVERIFY(item_named(tree, QStringLiteral("中抜き 2")) == nullptr);
        // the built-in ones again
        panel->reset(false);
        QCOMPARE(tree->topLevelItemCount(), 3);
    }
};

QTEST_MAIN(TestGuiStyles)
#include "test_gui_styles.moc"
