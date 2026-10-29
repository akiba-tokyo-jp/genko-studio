// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」) in the window: ウィンドウ → レイヤーカンプ (its dock shown and hidden,
// the command checked with it); the panel — 最後のドキュメントの状態 and the comps, ✓ on the one applied last, ⚠ on one whose
// layer is gone —; its buttons (新規, 前へ・次へ, 更新, 削除) and menu (複製, オプション…, 警告をクリア, 最後のドキュメントの状態を
// 復元), a double click applying a comp, each one Undo; 新規レイヤーカンプ… and レイヤーカンプオプション… asked in their window;
// ファイル → 書き出し → レイヤーカンプからファイル… writing a file a comp, the book unchanged; another page's comps shown.

#include <QtTest>

#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>
#include <string>

#include "app/layer_comps_panel.hpp"
#include "app/main_window.hpp"
#include "gui_support.hpp"

using namespace genko;
using core::Json;

namespace {

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::b4_comic()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->question = [](const QString&, const QString&) { return true; };
        apply(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
        apply(Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"panel_clip", false}});
        apply(Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({20, 20}), Json::array({60, 30})})},
                   {"width_mm", 1.0}});
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page() const { return *doc().pages[0]; }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }
    const core::Layer& layer(const std::string& id) const {
        for (const core::Layer& l : page().layers)
            if (l.id == id) return l;
        throw std::runtime_error("no layer " + id);
    }
    app::LayerCompsPanel* panel() const { return window->layer_comps_panel(); }
    QStringList rows() const {
        QStringList out;
        for (int i = 0; i < panel()->list()->count(); ++i) out << panel()->list()->item(i)->text();
        return out;
    }
    std::size_t comps() const { return page().layer_comps ? page().layer_comps->comps.size() : 0; }
    std::optional<std::int64_t> applied() const { return page().layer_comps ? page().layer_comps->last_applied : std::nullopt; }
    QAction* menu_item(const QString& text) const {
        for (QAction* a : panel()->menu()->actions())
            if (a->text() == text) return a;
        return nullptr;
    }
};

}  // namespace

class TestGuiLayerComps : public QObject {
    Q_OBJECT
private slots:
    // ウィンドウ → レイヤーカンプ: the dock shown and hidden, the command checked with it; in the window menu after アクション.
    void theWindowMenuShowsThePanel() {
        Book b;
        QAction* toggle = b.act("act_panel_layer_comps");
        QVERIFY(toggle != nullptr && toggle->isCheckable());
        QCOMPARE(toggle->text(), QStringLiteral("レイヤーカンプ"));
        QVERIFY(!b.window->layer_comps_dock()->isVisible());
        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY(b.window->layer_comps_dock()->isVisible());
        QVERIFY(toggle->isChecked());
        QCOMPARE(b.rows(), QStringList{QStringLiteral("✓ 最後のドキュメントの状態")});
        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!b.window->layer_comps_dock()->isVisible());
        QVERIFY(!toggle->isChecked());
        // (the window menu: アクション, then レイヤーカンプ — no "まだ使えません")
        QMenu* window_menu = nullptr;
        for (QAction* top : b.window->menuBar()->actions())
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("ウィンドウ")) window_menu = top->menu();
        QVERIFY(window_menu != nullptr);
        const QList<QAction*> items = window_menu->actions();
        const qsizetype at = items.indexOf(toggle);
        QVERIFY(at > 0);
        QCOMPARE(items[at - 1], b.act("act_panel_actions"));
    }

    // The panel's buttons and menu: new, next and previous round, a double click, update, duplicate, options, warnings
    // cleared, the last document state restored, delete — each one Undo.
    void thePanelsButtons() {
        Book b;
        b.act("act_panel_layer_comps")->trigger();
        QCoreApplication::processEvents();
        app::LayerCompsPanel* p = b.panel();
        QVERIFY(!p->button(QStringLiteral("comp_next"))->isEnabled());
        p->button(QStringLiteral("comp_new"))->click();
        QCOMPARE(b.comps(), std::size_t(1));
        QCOMPARE(b.rows(), (QStringList{QStringLiteral("　 最後のドキュメントの状態"), QStringLiteral("✓ レイヤーカンプ 1")}));
        QVERIFY(p->button(QStringLiteral("comp_next"))->isEnabled());
        QVERIFY(b.apply(Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"visible", false}}));
        p->button(QStringLiteral("comp_new"))->click();
        QCOMPARE(b.comps(), std::size_t(2));
        QCOMPARE(b.applied(), std::optional<std::int64_t>(2));
        // next: round to the first — the pen shown again
        p->button(QStringLiteral("comp_next"))->click();
        QCOMPARE(b.applied(), std::optional<std::int64_t>(1));
        QVERIFY(b.layer("pen").visible);
        QVERIFY(b.rows()[1].startsWith(QStringLiteral("✓ ")));
        b.act("act_undo")->trigger();  // (one Undo)
        QCOMPARE(b.applied(), std::optional<std::int64_t>(2));
        QVERIFY(!b.layer("pen").visible);
        p->button(QStringLiteral("comp_previous"))->click();
        QCOMPARE(b.applied(), std::optional<std::int64_t>(1));
        // a double click on レイヤーカンプ 2
        p->list()->setCurrentRow(2);
        emit p->list()->itemDoubleClicked(p->list()->item(2));
        QCOMPARE(b.applied(), std::optional<std::int64_t>(2));
        QVERIFY(!b.layer("pen").visible);
        // update the chosen (2) after showing the pen: its visibility recorded again
        QVERIFY(b.apply(Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"visible", true}}));
        p->list()->setCurrentRow(2);
        p->button(QStringLiteral("comp_update"))->click();
        for (const auto& s : b.page().layer_comps->comps[1].layers)
            if (s.layer == "pen") QCOMPARE(s.visible, std::optional<bool>(true));
        // duplicate: after it, 「… のコピー」
        p->list()->setCurrentRow(1);
        b.menu_item(QStringLiteral("レイヤーカンプを複製"))->trigger();
        QCOMPARE(b.comps(), std::size_t(3));
        QCOMPARE(b.page().layer_comps->comps[1].name, std::string("レイヤーカンプ 1 のコピー"));
        // options…: the window asked (its name, options, comment), then set_layer_comp
        b.answers.responder->exec = [](QDialog* dialog) {
            auto* options = qobject_cast<app::LayerCompOptionsDialog*>(dialog);
            if (options == nullptr) return 0;
            options->name->setText(QStringLiteral("表紙"));
            options->position->setChecked(false);
            options->comment->setPlainText(QStringLiteral("台詞なし"));
            return int(QDialog::Accepted);
        };
        p->list()->setCurrentRow(1);
        b.menu_item(QStringLiteral("レイヤーカンプオプション…"))->trigger();
        const core::LayerComp& named = b.page().layer_comps->comps[0];
        QCOMPARE(named.name, std::string("表紙"));
        QCOMPARE(named.comment, std::string("台詞なし"));
        QVERIFY(named.apply_visibility && !named.apply_position && named.apply_appearance);
        QVERIFY(p->list()->item(1)->toolTip().contains(QStringLiteral("台詞なし")));
        // 新規レイヤーカンプ…: its window, the first free name offered
        QString offered;
        b.answers.responder->exec = [&offered](QDialog* dialog) {
            auto* options = qobject_cast<app::LayerCompOptionsDialog*>(dialog);
            if (options == nullptr) return 0;
            offered = options->name->text();
            return int(QDialog::Accepted);
        };
        b.menu_item(QStringLiteral("新規レイヤーカンプ…"))->trigger();
        QCOMPARE(offered, QStringLiteral("レイヤーカンプ 2"));
        QCOMPARE(b.comps(), std::size_t(4));
        // a layer gone: ⚠; the warnings cleared
        QVERIFY(b.apply(Json{{"op", "delete_layer"}, {"page", 1}, {"id", "pen"}}));
        QVERIFY(b.rows()[1].endsWith(QStringLiteral("⚠")));
        b.menu_item(QStringLiteral("レイヤーカンプの警告をクリア"))->trigger();
        for (const QString& row : b.rows()) QVERIFY2(!row.endsWith(QStringLiteral("⚠")), qPrintable(row));
        // the last document state: refused while there is none (said), restored when there is
        b.menu_item(QStringLiteral("最後のドキュメントの状態を復元"))->trigger();
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("最後のドキュメントの状態はまだありません")), qPrintable(b.window->last_notice()));
        // delete the chosen
        p->list()->setCurrentRow(1);
        p->button(QStringLiteral("comp_delete"))->click();
        QCOMPARE(b.comps(), std::size_t(3));
        // another page: its own (none)
        b.window->select_page(1);
        QCoreApplication::processEvents();
        QCOMPARE(b.rows(), QStringList{QStringLiteral("✓ 最後のドキュメントの状態")});
    }

    // ファイル → 書き出し → レイヤーカンプからファイル…: a file a comp in the folder asked, the book unchanged; none without comps.
    void compsToFiles() {
        Book b;
        b.act("act_comps_to_files")->trigger();
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("レイヤーカンプがありません")), qPrintable(b.window->last_notice()));
        QVERIFY(b.apply(Json{{"op", "new_layer_comp"}, {"page", 1}, {"name", "A"}}));
        QVERIFY(b.apply(Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"visible", false}}));
        QVERIFY(b.apply(Json{{"op", "new_layer_comp"}, {"page", 1}, {"name", "B"}}));
        const std::int64_t revision = b.doc().revision;
        const QString folder = b.tmp.path() + QStringLiteral("/out");
        b.answers.responder->exec = [&folder](QDialog* dialog) {
            auto* files = qobject_cast<app::LayerCompsToFilesDialog*>(dialog);
            if (files == nullptr) return 0;
            files->folder->setText(folder);
            files->prefix->setText(QStringLiteral("p"));
            return int(QDialog::Accepted);
        };
        b.act("act_comps_to_files")->trigger();
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("2 枚書き出しました")), qPrintable(b.window->last_notice()));
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/p_0000_A.png")));
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/p_0001_B.png")));
        QCOMPARE(b.doc().revision, revision);
        QVERIFY(!b.layer("pen").visible);
    }
};

QTEST_MAIN(TestGuiLayerComps)
#include "test_gui_layer_comps.moc"
