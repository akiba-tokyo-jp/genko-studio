// PC4 E3 (window.panel.patterns, pattern.*, edit.definePattern, edit.fill, layer.newFillLayer.pattern,
// brush.texturePattern, view.patternPreview; SPEC.md COLOR-09「パターン」) on the offscreen platform: the Patterns panel
// (ウィンドウ → パターン: the library's groups and this book's patterns; a click chooses the pattern, a double click makes
// a pattern fill layer; its buttons and menu: rename, delete, .pat in and out, into the library), 編集 → パターンを定義…
// (the page as it shows, into the library), 塗りつぶし… with パターン, レイヤー → パターンのレイヤー… and its settings
// changed, the brush's 紙質 from a pattern, 表示 → パターンプレビュー; their places in the menus.

#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QImage>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTreeWidget>

#include <filesystem>

#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/patterns_panel.hpp"
#include "core/command_bus.hpp"
#include "core/patterns.hpp"
#include "render/paper.hpp"
#include "render/pattern_library.hpp"
#include "render/patterns.hpp"
#include "storage/asset_store.hpp"

using namespace genko;
using core::Json;

namespace {

// A book of one colour page (60 × 60 mm, one panel), a red square on the ink layer.
core::Document book_doc() {
    auto doc = core::new_episode("パターン", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
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
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        window->canvas()->fit_page();
    }
    const core::Document& book() const { return session->document(); }
    const core::Page& page() const { return book().page(0); }
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

void forget_library() {
    std::error_code ec;
    std::filesystem::remove(app::config_dir() / "patterns.json", ec);
    std::filesystem::remove_all(app::config_dir() / "patterns", ec);
}

}  // namespace

class TestGuiPatterns : public QObject {
    Q_OBJECT
private slots:
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        forget_library();
    }

    // ウィンドウ → パターン: the library's groups (幾何学, テクスチャ) and their patterns with pictures; a click chooses the
    // pattern (the window's state, kept in the settings); the commands' places in the menus.
    void thePanelAndItsPlaces() {
        Studio s;
        QDockWidget* dock = s.window->patterns_dock();
        QVERIFY(dock != nullptr && !dock->isVisible());
        QVERIFY(s.menus_of(dock->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        dock->toggleViewAction()->trigger();
        QVERIFY(dock->isVisible());
        app::PatternsPanel* panel = s.window->patterns_panel();
        QTreeWidget* tree = panel->tree();
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("幾何学"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("テクスチャ"));
        QTreeWidgetItem* dots = item_named(tree, QStringLiteral("水玉"));
        QVERIFY(dots != nullptr && !dots->icon(0).isNull());
        QVERIFY(dots->toolTip(0).contains(QStringLiteral("12 × 12")));
        tree->setCurrentItem(dots);
        QCOMPARE(s.window->current_pattern(), QStringLiteral("library:水玉"));
        QCOMPARE(app::settings()->value(QStringLiteral("patterns/current")).toString(), QStringLiteral("library:水玉"));
        QVERIFY(s.menus_of(s.window->action(QStringLiteral("act_define_pattern"))).contains(QStringLiteral("編集")));
        QVERIFY(s.menus_of(s.window->action(QStringLiteral("act_pattern_preview"))).contains(QStringLiteral("表示")));
        QVERIFY(s.menus_of(s.window->action(QStringLiteral("act_layer_pattern"))).contains(QStringLiteral("レイヤー")));
        QCOMPARE(s.window->action(QStringLiteral("act_define_pattern"))->text(), QStringLiteral("パターンを定義…"));
        QCOMPARE(s.window->action(QStringLiteral("act_pattern_preview"))->text(), QStringLiteral("パターンプレビュー"));
    }

    // レイヤー → パターンのレイヤー…: the dialog (パターン, 角度, 比率, レイヤーにリンク) makes a pattern fill layer, the library
    // pattern put into the book with it (one Undo takes both); its settings changed again (its scale), the book's
    // pattern then in the panel; a double click on a pattern makes one too.
    void aPatternFillLayer() {
        Studio s;
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("pattern_fill_dialog")) return int(QDialog::Rejected);
            auto* choice = dialog->findChild<QComboBox*>(QStringLiteral("pattern_choice"));
            choice->setCurrentIndex(choice->findData(QStringLiteral("library:斜線")));
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("pattern_scale"))->setValue(200);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("pattern_angle"))->setValue(15);
            return int(QDialog::Accepted);
        };
        const std::size_t layers = s.page().layers.size();
        s.trigger("act_layer_pattern");
        QCOMPARE(s.page().layers.size(), layers + 1);
        const core::Layer* made = s.window->target_layer();
        QVERIFY(made != nullptr && made->kind == core::LayerKind::Fill);
        const Json* pattern = core::pattern_of_fill(*made->fill);
        QVERIFY(pattern != nullptr);
        QCOMPARE((*pattern)["scale"], Json(200));
        QCOMPARE((*pattern)["angle"], Json(15));
        QCOMPARE(s.book().patterns.size(), std::size_t(1));
        QCOMPARE(s.book().patterns[0].name, std::string("斜線"));
        // changed again (塗り・補正を直す…): its scale
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("pattern_fill_dialog")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("pattern_scale"))->setValue(50);
            return int(QDialog::Accepted);
        };
        s.window->layer_panel()->edit_special();
        const core::Layer* changed = s.window->target_layer();
        QCOMPARE((*core::pattern_of_fill(*changed->fill))["scale"], Json(50));
        QCOMPARE(s.book().patterns.size(), std::size_t(1));  // (the book's copy used again)
        // undone: the scale as it was; the layer and the pattern gone with the first
        s.trigger("act_undo");
        QCOMPARE((*core::pattern_of_fill(*s.window->target_layer()->fill))["scale"], Json(200));
        s.trigger("act_undo");
        QCOMPARE(s.page().layers.size(), layers);
        QVERIFY(s.book().patterns.empty());
        // a double click in the panel: a pattern fill layer of that pattern
        s.window->patterns_dock()->toggleViewAction()->trigger();
        QTreeWidget* tree = s.window->patterns_panel()->tree();
        s.answers.responder->exec = [](QDialog* dialog) { return int(dialog->objectName() == QStringLiteral("pattern_fill_dialog") ? QDialog::Accepted : QDialog::Rejected); };
        emit tree->itemDoubleClicked(item_named(tree, QStringLiteral("グリッド")), 0);
        QCOMPARE(s.book().patterns.size(), std::size_t(1));
        QCOMPARE(s.book().patterns[0].name, std::string("グリッド"));
        s.window->patterns_panel()->refresh();
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("この原稿のパターン"));
        QVERIFY(item_named(tree, QStringLiteral("グリッド")) != nullptr);
    }

    // 編集 → 塗りつぶし… with パターン: the chosen pattern (its 比率, 角度) laid in the selection (none: the layer), the
    // library pattern put into the book.
    void fillWithAPattern() {
        Studio s;
        s.window->set_current_pattern(QStringLiteral("library:チェッカーボード"));
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("fill_dialog")) return int(QDialog::Rejected);
            auto* contents = dialog->findChild<QComboBox*>(QStringLiteral("fill_contents"));
            contents->setCurrentIndex(contents->findData(QStringLiteral("pattern")));
            auto* pattern = dialog->findChild<QComboBox*>(QStringLiteral("fill_pattern"));
            if (pattern->currentData().toString() != QStringLiteral("library:チェッカーボード")) return int(QDialog::Rejected);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("fill_pattern_scale"))->setValue(300);
            return int(QDialog::Accepted);
        };
        const core::Layer* ink = gui_test::ink_of(s.page());
        s.window->set_target_layer(ink->id);
        const std::size_t patches = ink->patches.size();
        s.trigger("act_fill_dialog");
        QCOMPARE(s.book().patterns.size(), std::size_t(1));
        QCOMPARE(gui_test::ink_of(s.page())->patches.size(), patches + 1);
    }

    // 編集 → パターンを定義…: the page as it shows (no selection: the whole page) into the library under the name asked
    // for, chosen; the panel shows it; renamed and deleted from the panel; written to a .pat and read back.
    void defineRenameDeleteAndPat() {
        Studio s;
        s.answers.responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("パターンを定義") ? std::optional<QString>(QStringLiteral("ページ")) : std::optional<QString>(QStringLiteral("頁"));
        };
        s.trigger("act_define_pattern");
        QCOMPARE(s.window->current_pattern(), QStringLiteral("library:ページ"));
        const auto chosen = app::pattern_chosen(s.book(), s.window->current_pattern());
        QVERIFY(chosen.has_value());
        QCOMPARE(chosen->first.mode, std::string("rgb"));
        QVERIFY(chosen->first.width > 300);  // (the 60 mm page at its working grid)
        s.window->patterns_dock()->toggleViewAction()->trigger();
        app::PatternsPanel* panel = s.window->patterns_panel();
        QVERIFY(item_named(panel->tree(), QStringLiteral("ページ")) != nullptr);
        panel->rename();
        QCOMPARE(s.window->current_pattern(), QStringLiteral("library:頁"));
        const QString pat = s.tmp.path() + QStringLiteral("/p.pat");
        QVERIFY(panel->export_pat(pat));
        panel->remove();
        QVERIFY(item_named(panel->tree(), QStringLiteral("頁")) == nullptr);
        QVERIFY(panel->import_pat(pat));
        QVERIFY(item_named(panel->tree(), QStringLiteral("頁")) != nullptr);
        // a book's pattern into the library (ライブラリに追加), and deleted from the book (unused)
        s.window->apply_ops(Json::array({render::pattern_library::add_op(render::pattern_library::builtins()[1].pattern, render::pattern_library::builtins()[1].bytes)}));
        QCOMPARE(s.book().patterns.size(), std::size_t(1));
        s.window->set_current_pattern(QStringLiteral("book:") + QString::fromStdString(s.book().patterns[0].id));
        panel->refresh();
        panel->remove();
        QVERIFY(s.book().patterns.empty());
    }

    // The brush's 紙質 from a pattern (パターンから…: its lightness over white); 表示 → パターンプレビュー (the page laid again
    // around it, the book unchanged).
    void paperFromAPatternAndThePreview() {
        Studio s;
        render::paper::clear();
        s.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("paper_pattern_dialog")) return int(QDialog::Rejected);
            auto* choice = dialog->findChild<QComboBox*>(QStringLiteral("pattern_choice"));
            choice->setCurrentIndex(choice->findData(QStringLiteral("library:紙のノイズ")));
            return int(QDialog::Accepted);
        };
        app::BrushDialog d(s.window.get(), "fill_pen");
        QVERIFY(!d.paper_on->isChecked());
        auto* from = d.findChild<QPushButton*>(QStringLiteral("paper_pattern"));
        QVERIFY(from != nullptr);
        from->click();
        QVERIFY(d.paper_on->isChecked());
        QVERIFY(storage::AssetStore::is_ref(d.paper_asset));
        QVERIFY(render::paper::bytes(d.paper_asset) != nullptr);
        QVERIFY(d.paper_about->text().contains(QStringLiteral("64 × 64")));
        // パターンプレビュー
        const auto before = s.session->snapshot();
        QVERIFY(!s.window->canvas()->pattern_preview);
        s.trigger("act_pattern_preview");
        QVERIFY(s.window->canvas()->pattern_preview);
        s.window->canvas()->repaint();
        const QImage shown = s.window->canvas()->grab().toImage();
        QVERIFY(!shown.isNull());
        s.trigger("act_pattern_preview");
        QVERIFY(!s.window->canvas()->pattern_preview);
        QVERIFY(s.session->snapshot() == before);
    }
};

QTEST_MAIN(TestGuiPatterns)
#include "test_gui_patterns.moc"
