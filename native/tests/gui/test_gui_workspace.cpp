// The screen made one's own and the window's remaining commands (M6, Python's genko/app/workspace.py and main.py) on the
// offscreen platform: コマンドを探す (every menu command once, found by words in any order, done with Enter), the
// quick-access panel and the command bar chosen (kept in the settings), workspaces (the panels kept under a name, put
// back, はじめの配置に戻す), 道具の名前を表示 and パネルの説明を表示, Python's menus (ファイル → AI と作る…, ヘルプ, the
// layer panel's menus in the menu bar with their keys, 3D's scenes and poses, このコマの詳細を見る), the first steps over
// an empty book and what comes next after the first panels (I11), the selection launcher, the tool palette on a short
// window, the layers in front at the first show, and every menu item's place as where() finds it.

#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QTreeWidget>
#include <QTabWidget>
#include <QLabel>
#include <QKeySequenceEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>

#include <functional>
#include <map>
#include <memory>
#include <tuple>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/workspace.hpp"
#include "app/comfort.hpp"
#include "app/key_sets.hpp"
#include "app/menu_look.hpp"
#include "app/toolbar_dialog.hpp"
#include "app/shortcuts_dialog.hpp"
#include "core/paths.hpp"
#include "gui_support.hpp"
#include "service/studio_service.hpp"

using namespace genko;
using core::Json;

namespace {

// A book in a window, its questions answered by the test.
struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::function<int(QDialog*)> on_dialog;

    explicit Book(core::Document doc = gui_test::new_doc(2)) {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        gui_test::write_book(path, doc);
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 860);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->exec = [this](QDialog* dialog) {
            answers.asked << QString::fromLatin1(dialog->metaObject()->className());
            return on_dialog ? on_dialog(dialog) : 0;
        };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    QMenu* menu(const QString& title) const {
        for (QAction* top : window->menuBar()->actions()) {
            if (top->text() == title) return top->menu();
        }
        return nullptr;
    }
    QDockWidget* dock(const QString& title) const {
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->windowTitle() == title) return d;
        }
        return nullptr;
    }
};

QStringList texts(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
    return out;
}

QMenu* submenu(QMenu* menu, const QString& title) {
    for (QAction* a : menu->actions()) {
        if (a->menu() != nullptr && a->text() == title) return a->menu();
    }
    return nullptr;
}

}  // namespace

class TestGuiWorkspace : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }
    void init() {
        auto store = app::settings();
        for (const char* key : {"ui/quick", "ui/commandbar", "ui/tool_names", "ui/next_steps", "workspaces", "ui/workspace",
                                // (PC5 H-d)
                                "workspace_changes", "ui/workspace_locked", "ui/workspace_remember", "menus", "toolbar", "key_sets", "ui/key_set_own"}) store->remove(QString::fromLatin1(key));
    }

    // コマンドを探す: every command in the menus once (the menus inside menus too, not the empty "（…）" rows), by its
    // words, its menu or its explanation, every word in any order; Enter does the one chosen; the disabled are not offered.
    // (Two with the same words — the user's decision 8: レイヤー → 色調補正 and the layer panel's 色調補正のレイヤー both
    // have レベル補正… — are each there, told apart by their place.)
    void commandSearchFindsAndRuns() {
        Book b;
        const std::vector<QAction*> all = app::commands(b.window.get());
        QSet<QString> seen;
        QSet<QString> rows;
        for (QAction* a : all) {
            const QString row = a->text() + QStringLiteral("　（") + app::where(b.window.get(), a) + QStringLiteral("）");
            QVERIFY2(!rows.contains(row), qPrintable(row));
            rows.insert(row);
            seen.insert(a->text());
            QVERIFY(!a->text().startsWith(QStringLiteral("（")));
        }
        QVERIFY(seen.contains(QStringLiteral("検索")));  // (PC5 H-0: Photoshop's name for コマンドを探す…)
        QVERIFY(seen.contains(QStringLiteral("選んだレイヤーをフォルダにまとめる")));  // (in レイヤー → まとめて)
        QCOMPARE(app::where(b.window.get(), b.act("act_mirror")), QStringLiteral("表示"));
        QCOMPARE(app::where(b.window.get(), b.act("act_find_command")), QStringLiteral("編集"));
        app::CommandSearch* search = b.window->find_command();
        QCOMPARE(search->windowTitle(), QStringLiteral("検索"));
        QVERIFY(search->query->hasFocus() || search->isVisible());
        // (PC5 H-0: by its old name too — 左右反転して見る is 表示 → 水平方向に反転; its key H in the Genko key set only)
        search->query->setText(QStringLiteral("左右反転して見る"));
        QVERIFY(!search->found().empty());
        QCOMPARE(search->found().front(), b.act("act_mirror"));
        QCOMPARE(search->results->item(0)->text(), QStringLiteral("水平方向に反転　（表示）"));
        search->query->setText(QStringLiteral("履歴"));
        QVERIFY(std::find(search->found().begin(), search->found().end(), b.act("act_history")) != search->found().end());
        search->query->setText(QStringLiteral("ルックアップ 書き出す"));
        QVERIFY(std::find(search->found().begin(), search->found().end(), b.act("act_export_lookup")) != search->found().end());
        // the words in any order, the explanation too
        search->query->setText(QStringLiteral("歪み 表示"));
        QVERIFY(std::find(search->found().begin(), search->found().end(), b.act("act_mirror")) != search->found().end());
        // a disabled command is not offered
        b.act("act_mirror")->setEnabled(false);
        search->refresh();
        QVERIFY(std::find(search->found().begin(), search->found().end(), b.act("act_mirror")) == search->found().end());
        b.act("act_mirror")->setEnabled(true);
        // Enter does it
        search->query->setText(QStringLiteral("左右反転して見る"));
        const bool was = b.act("act_mirror")->isChecked();
        QTest::keyClick(search->query, Qt::Key_Return);
        QCOMPARE(b.act("act_mirror")->isChecked(), !was);
        QCOMPARE(b.act("act_find_command")->shortcut(), QKeySequence(QStringLiteral("Ctrl+F")));  // (PC5 H-0: 検索, Photoshop's)
        // a command its menu has deleted while the finder was open (a menu that makes its items again as it opens): not
        // run, the list found again
        QMenu* view = b.menu(QStringLiteral("表示"));
        QVERIFY(view != nullptr);
        auto* gone = new QAction(QStringLiteral("消える試しのコマンド"), view);
        view->addAction(gone);
        bool ran = false;
        connect(gone, &QAction::triggered, [&ran] { ran = true; });
        search->query->setText(QStringLiteral("消える試しのコマンド"));
        QCOMPARE(search->found().size(), std::size_t{1});
        delete gone;
        QTest::keyClick(search->query, Qt::Key_Return);
        QVERIFY(!ran);
        QVERIFY(search->found().empty());
    }

    // クイックアクセス: Python's eight commands as buttons (two to a row, a long name on its own row), each doing its
    // command; the ones chosen kept in the settings; はじめの並びに戻す. The command bar likewise, with its lines.
    void quickAccessAndTheCommandBar() {
        Book b;
        QDockWidget* quick = b.dock(QStringLiteral("クイックアクセス"));
        QVERIFY(quick != nullptr);
        app::QuickAccess* panel = b.window->quick_access();
        const auto names = app::by_text(b.window.get());
        int wanted = 0;
        for (const QString& name : app::kDefaultQuick) wanted += names.count(name) ? 1 : 0;
        QCOMPARE(static_cast<int>(panel->buttons.size()), wanted);
        QVERIFY(wanted >= 6);
        QPushButton* pen = nullptr;
        for (QPushButton* button : panel->buttons) {
            if (button->accessibleName() == QStringLiteral("ブラシツール")) pen = button;  // (PC4 E-0, decision 2)
        }
        QVERIFY(pen != nullptr);
        b.window->choose_tool(QStringLiteral("eraser"));
        pen->click();
        QCOMPARE(b.window->canvas()->tool(), QStringLiteral("pen"));
        // chosen in its dialog (the test answers it): 履歴… first, then 消しゴムツール
        b.on_dialog = [](QDialog* dialog) {
            auto* choose = qobject_cast<app::ChooseCommands*>(dialog);
            if (choose == nullptr) return 0;
            choose->mine->clear();
            choose->filter->setText(QStringLiteral("履歴"));
            choose->all->setCurrentRow(0);
            choose->add->click();
            choose->filter->setText(QStringLiteral("消しゴムツール"));
            for (int i = 0; i < choose->all->count(); ++i) {
                if (choose->all->item(i)->text() == QStringLiteral("消しゴムツール")) choose->all->setCurrentRow(i);
            }
            choose->add->click();
            choose->add->click();  // (twice: once in the list)
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_edit_quick")->trigger();  // (表示 → クイックアクセスを変える…)
        QCOMPARE(panel->names(), (QStringList{QStringLiteral("ヒストリー…"), QStringLiteral("消しゴムツール")}));  // (PC5 H-0: found by its old name)
        QCOMPARE(app::settings()->value(QStringLiteral("ui/quick")).toString(), QStringLiteral("ヒストリー…\t消しゴムツール"));
        QCOMPARE(panel->buttons.size(), std::size_t{2});
        // はじめの並びに戻す
        b.on_dialog = [](QDialog* dialog) {
            if (auto* choose = qobject_cast<app::ChooseCommands*>(dialog)) choose->restore->click();
            return static_cast<int>(QDialog::Rejected);
        };
        panel->edit();
        QCOMPARE(panel->names(), app::kDefaultQuick);
        // the command bar: the books' tabs kept, the commands chosen with a line between
        QToolBar* bar = b.window->command_bar();
        const auto commands_on = [bar] {
            QStringList out;
            for (QAction* a : bar->actions()) {
                if (a->property("keep").toBool()) continue;
                out << (a->isSeparator() ? QStringLiteral("|") : a->text());
            }
            return out;
        };
        QCOMPARE(commands_on(), app::kDefaultCommandBar);
        b.on_dialog = [](QDialog* dialog) {
            auto* choose = qobject_cast<app::ChooseCommands*>(dialog);
            if (choose == nullptr || choose->line == nullptr) return 0;
            choose->mine->clear();
            choose->filter->setText(QStringLiteral("原稿を書き出す…"));  // (PC4 F-0: 書き出し…'s new name)
            choose->all->setCurrentRow(0);
            choose->add->click();
            choose->line->click();
            choose->filter->setText(QStringLiteral("取り消し"));
            choose->all->setCurrentRow(0);
            choose->add->click();
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_edit_commandbar")->trigger();
        QCOMPARE(commands_on(), (QStringList{QStringLiteral("原稿を書き出す…"), QStringLiteral("|"), QStringLiteral("取り消し")}));
        QCOMPARE(b.window->doc_tabs()->isVisibleTo(bar), true);
        // 編集 → 環境設定 → 一般… (PC5 H-0; ファイル → 環境設定… before) opens the preferences (asked before another window takes the answers)
        b.answers.asked.clear();
        b.act("act_prefs")->trigger();
        QVERIFY2(b.answers.asked == QStringList{QStringLiteral("genko::app::preferences::PreferencesDialog")}, qPrintable(b.answers.asked.join(QStringLiteral(" | "))));
        // a new window reads the same
        Book other;
        QCOMPARE(other.window->quick_access()->names(), app::kDefaultQuick);
        QStringList on_other;
        for (QAction* a : other.window->command_bar()->actions()) {
            if (!a->property("keep").toBool()) on_other << (a->isSeparator() ? QStringLiteral("|") : a->text());
        }
        QCOMPARE(on_other, (QStringList{QStringLiteral("原稿を書き出す…"), QStringLiteral("|"), QStringLiteral("取り消し")}));
    }

    // Workspaces: the panels kept under a name (新規ワークスペース… — PC5 H-d, 今の配置をワークスペースとして残す… before), listed in
    // ウィンドウ → ワークスペース after Photoshop's, chosen, deleted (ワークスペースを削除…, never the one in use); 初期設定: the panels
    // as the window first showed them.
    void workspacesKeepThePanels() {
        Book b;
        QDockWidget* layers = b.dock(QStringLiteral("レイヤー"));
        QVERIFY(layers != nullptr && layers->isVisible());
        b.on_dialog = [](QDialog* dialog) {
            auto* named = qobject_cast<app::NewWorkspaceDialog*>(dialog);
            if (named == nullptr) return 0;
            named->name->setText(QStringLiteral(" ペン入れ "));
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_save_workspace")->trigger();
        QCOMPARE(b.act("act_save_workspace")->text(), QStringLiteral("新規ワークスペース…"));
        QCOMPARE(app::workspaces(), QStringList{QStringLiteral("ペン入れ")});
        QVERIFY(b.window->last_notice().contains(QStringLiteral("ワークスペース「ペン入れ」を残しました")));
        QCOMPARE(b.window->current_workspace(), QStringLiteral("ペン入れ"));
        layers->hide();
        QVERIFY(!layers->isVisible());
        QVERIFY(app::load_workspace(b.window.get(), QStringLiteral("ペン入れ")));
        QVERIFY(layers->isVisible());
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace")).toString(), QStringLiteral("ペン入れ"));
        // the menu: Photoshop's, the person's (the one in use checked), then リセット・新規…・削除…・ロック, the keys, 作業の段階
        QMenu* window_menu = b.menu(QStringLiteral("ウィンドウ"));
        QMenu* spaces = submenu(window_menu, QStringLiteral("ワークスペース"));
        QVERIFY(spaces != nullptr);
        emit spaces->aboutToShow();
        QCOMPARE(texts(spaces),
                 (QStringList{QStringLiteral("初期設定"), QStringLiteral("グラフィックと Web"), QStringLiteral("モーション"), QStringLiteral("ペイント"),
                              QStringLiteral("写真"), QStringLiteral("ピクセルアート"), QStringLiteral("|"), QStringLiteral("ペン入れ"), QStringLiteral("|"),
                              QStringLiteral("ワークスペースをリセット"), QStringLiteral("新規ワークスペース…"), QStringLiteral("ワークスペースを削除…"),
                              QStringLiteral("ワークスペースをロック"), QStringLiteral("|"), QStringLiteral("キーボードショートカット…"), QStringLiteral("|"),
                              QStringLiteral("作業の段階")}));
        QVERIFY(spaces->actions().at(7)->isChecked());
        QVERIFY(!b.act("act_workspace_essentials")->isChecked());
        // ワークスペースを削除…: the one in use refused, another deleted (asked which)
        b.answers.responder->get_item = [](const QString&, const QString&, const QStringList& items, int, bool) {
            return items.isEmpty() ? std::optional<QString>() : std::optional<QString>(items.front());
        };
        b.answers.responder->question = [](const QString&, const QString&) { return true; };
        QVERIFY(!b.window->delete_workspace_dialog());
        QVERIFY(b.window->last_notice().contains(QStringLiteral("今のワークスペースなので削除できません")));
        b.act("act_workspace_essentials")->trigger();
        QCOMPARE(b.window->current_workspace(), QStringLiteral("初期設定"));
        b.act("act_delete_workspace")->trigger();
        QVERIFY(app::workspaces().isEmpty());
        // 初期設定
        layers->hide();
        b.window->default_layout();
        QVERIFY(layers->isVisible());
        // no name: nothing kept
        b.on_dialog = [](QDialog* dialog) {
            if (auto* named = qobject_cast<app::NewWorkspaceDialog*>(dialog)) named->name->setText(QStringLiteral("  "));
            return static_cast<int>(QDialog::Accepted);
        };
        QVERIFY(!b.window->save_workspace());
        QVERIFY(app::workspaces().isEmpty());
    }

    // 道具の名前を表示: the palette with the tools' names beside their pictures, kept for the next window;
    // パネルの説明を表示: the panels' hints.
    void toolNamesAndHints() {
        {
            Book b;
            QToolBar* palette = b.window->tool_palette();
            QCOMPARE(palette->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QVERIFY(b.act("act_tool_names")->isCheckable());
            b.act("act_tool_names")->trigger();
            QCOMPARE(palette->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            QCOMPARE(app::settings()->value(QStringLiteral("ui/tool_names")).toString(), QStringLiteral("1"));
            QVERIFY(texts(b.menu(QStringLiteral("表示"))).contains(QStringLiteral("道具の名前を表示")));
        }
        {
            Book b;  // (the next window as it was left)
            QVERIFY(b.act("act_tool_names")->isChecked());
            QCOMPARE(b.window->tool_palette()->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            const bool hints = app::theme::show_hints();
            QCOMPARE(b.act("act_hints")->isChecked(), hints);
            b.act("act_hints")->trigger();
            QCOMPARE(app::theme::show_hints(), !hints);
            b.act("act_hints")->trigger();
            QCOMPARE(app::theme::show_hints(), hints);
            QVERIFY(texts(b.menu(QStringLiteral("ウィンドウ"))).contains(QStringLiteral("パネルの説明を表示")));
        }
    }

    // Python's menus: ファイル → AI と作る…; ヘルプ; レイヤー's submenus with the layer panel's own commands (their keys
    // work from the window); 3D's scenes and poses in menus of their own; a panel's menu with このコマの詳細を見る.
    void pythonsMenus() {
        Book b;
        const QStringList file = texts(b.menu(QStringLiteral("ファイル")));
        QVERIFY(file.contains(QStringLiteral("AI と作る…")));
        QVERIFY(file.indexOf(QStringLiteral("AI と作る…")) < file.indexOf(QStringLiteral("保存")));
        b.act("act_ai")->trigger();
        QCOMPARE(b.answers.asked.back(), QStringLiteral("genko::app::AiDialog"));
        QMenu* help = b.menu(QStringLiteral("ヘルプ"));
        QVERIFY(help != nullptr);
        QVERIFY(texts(b.menu(QStringLiteral("編集"))).contains(QStringLiteral("検索")));  // (PC5 H-0: コマンドを探す… moved from ヘルプ)
        QMenu* layers = b.menu(QStringLiteral("レイヤー"));
        for (const QString& title : {QStringLiteral("塗り・グラデーション・色調補正のレイヤー"), QStringLiteral("まとめて（複数のレイヤー・表示レイヤー・変換）"),
                                     QStringLiteral("境界効果・表示色の印刷"), QStringLiteral("レイヤーマスク")}) {
            QMenu* sub = submenu(layers, title);
            QVERIFY2(sub != nullptr && !sub->actions().isEmpty(), qPrintable(title));
        }
        QVERIFY(texts(submenu(layers, QStringLiteral("まとめて（複数のレイヤー・表示レイヤー・変換）"))).contains(QStringLiteral("選んだレイヤーをフォルダにまとめる")));
        // Ctrl+G from the window: the layer chosen goes into a folder of its own
        const auto folders = [&b] {
            int n = 0;
            for (const core::Layer& l : b.window->book().page(0).layers) n += l.kind == core::LayerKind::Folder ? 1 : 0;
            return n;
        };
        const int before = folders();
        b.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(b.window.get()));  // (on xcb the window becomes active later; a key before goes nowhere)
        b.window->canvas()->setFocus();
        QTest::keyClick(b.window->canvas(), Qt::Key_G, Qt::ControlModifier);
        QCOMPARE(folders(), before + 1);
        // 3D
        QMenu* tools = b.menu(QStringLiteral("ツール"));
        QMenu* three = submenu(tools, QStringLiteral("3D"));
        QVERIFY(three != nullptr);
        QVERIFY(submenu(three, QStringLiteral("背景の 3D を置く")) != nullptr && !submenu(three, QStringLiteral("背景の 3D を置く"))->actions().isEmpty());
        QVERIFY(submenu(three, QStringLiteral("ポーズ")) != nullptr && !submenu(three, QStringLiteral("ポーズ"))->actions().isEmpty());
        // a panel's menu: このコマの詳細を見る brings the panel
        const QString frame = QString::fromStdString(b.window->book().page(0).leaf_frames().front()->id);
        QMenu menu;
        b.window->fill_context_menu(menu, frame);
        QAction* show = nullptr;
        for (QAction* a : menu.actions()) {
            if (a->text() == QStringLiteral("このコマの詳細を見る")) show = a;
        }
        QVERIFY(show != nullptr);
        QDockWidget* details = b.dock(QStringLiteral("コマの詳細"));
        QVERIFY(details != nullptr);
        show->trigger();
        QCoreApplication::processEvents();
        QVERIFY(details->isVisible() && !details->visibleRegion().isEmpty());
    }

    // The first steps over a book with nothing in it (コマを割る・ブラシで描く・台詞を入れる・AI に頼む…, ×); after the first
    // panels, what comes next (I11): 描く・台詞・保存・書き出し, each marked when done, × hides it for good and ヘルプ →
    // 次の作業の案内を表示 brings it back; neither takes the page's room from the side panels.
    void firstStepsThenWhatComesNext() {
        Book b(gui_test::new_doc(1));
        QWidget* first = b.window->first_steps();
        QVERIFY(first != nullptr && first->isVisible());
        QStringList words;
        for (QPushButton* button : first->findChildren<QPushButton*>()) words << button->text();
        QVERIFY(words.contains(QStringLiteral("コマを割る（K）"))  // (PC5 H-0: the Photoshop key set)
                && words.contains(QStringLiteral("ブラシで描く（B）")) &&
                words.contains(QStringLiteral("台詞を入れる（T）")) && words.contains(QStringLiteral("AI に頼む…")));
        for (QPushButton* button : first->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("コマを割る（K）")) button->click();
        }
        QCOMPARE(b.window->canvas()->tool(), QStringLiteral("frame"));
        QVERIFY(first->height() < 60);
        // the first panels
        const core::Page& page = b.window->book().page(0);
        QVERIFY(b.window->apply_ops(Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"},
                                                      {"frame_id", page.leaf_frames().front()->id}, {"gutter_mm", 3}}})));
        QCoreApplication::processEvents();
        QVERIFY(!first->isVisible());
        QWidget* next = b.window->next_steps();
        QVERIFY(next != nullptr && next->isVisible());
        QVERIFY(next->height() < 60);
        const auto button = [next](const char* key) { return next->findChild<QPushButton*>(QStringLiteral("next_") + QString::fromLatin1(key)); };
        QCOMPARE(button("draw")->text(), QStringLiteral("ブラシで描く（B）"));
        QCOMPARE(button("line")->text(), QStringLiteral("台詞を入れる（T）"));
        button("draw")->click();
        QCOMPARE(b.window->canvas()->tool(), QStringLiteral("pen"));
        button("line")->click();
        QCOMPARE(b.window->canvas()->tool(), QStringLiteral("text"));
        // done: marked
        QVERIFY(b.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                      {"points", Json::array({Json::array({20, 20}), Json::array({80, 90})})}}})));
        QVERIFY(b.window->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "テスト"}, {"x_mm", 40}, {"y_mm", 50}, {"w_mm", 30}, {"h_mm", 40}}})));
        QVERIFY(b.window->session().wait_saved(std::chrono::milliseconds(10000)));
        b.window->refresh_status();
        QCOMPARE(button("draw")->text(), QStringLiteral("✓ ブラシで描く（B）"));
        QCOMPARE(button("line")->text(), QStringLiteral("✓ 台詞を入れる（T）"));
        QCOMPARE(button("save")->text(), QStringLiteral("✓ 保存"));
        // × hides it (kept for the next window), ヘルプ → 次の作業の案内を表示 brings it back
        QPushButton* close = nullptr;
        for (QPushButton* p : next->findChildren<QPushButton*>()) {
            if (p->accessibleName() == QStringLiteral("この案内を閉じる")) close = p;
        }
        QVERIFY(close != nullptr);
        close->click();
        QVERIFY(!next->isVisible());
        QCOMPARE(app::settings()->value(QStringLiteral("ui/next_steps")).toString(), QStringLiteral("off"));
        QVERIFY(texts(b.menu(QStringLiteral("ヘルプ"))).contains(QStringLiteral("次の作業の案内を表示")));
        b.act("act_next_steps")->trigger();
        QVERIFY(next->isVisible());
        // a book with panels already: no first steps, what comes next
        core::Document panelled = gui_test::new_doc(2);
        {
            core::Page& first_page = panelled.edit_page(0);
            const std::string root = first_page.leaf_frames().front()->id;  // (a copy: the split changes the frames)
            first_page.split_frame(root, "horizontal", core::Num(0.5), core::Num(3));
        }
        Book other(panelled);
        QVERIFY(other.window->first_steps() == nullptr || !other.window->first_steps()->isVisible());
    }

    // 選択範囲ランチャー: under the selection while there is one (塗りつぶし・消去・トーン・反転・拡張・変形・コピー・選択を解除), each its
    // command; gone with the selection.
    void theLauncherUnderTheSelection() {
        Book b;
        QWidget* launcher = b.window->canvas()->launcher;
        QVERIFY(launcher != nullptr);
        QVERIFY(!launcher->isVisible());
        QStringList words;
        for (const auto& [word, act] : b.window->launcher_actions()) {
            QVERIFY(act != nullptr);
            words << word;
        }
        // (PC3 B3: 選択とマスク… joins it, as the PC3 design's launcher has it)
        QCOMPARE(words, (QStringList{QStringLiteral("塗りつぶし"), QStringLiteral("消去"), QStringLiteral("トーン"), QStringLiteral("反転"), QStringLiteral("拡張"),
                                     QStringLiteral("変形"), QStringLiteral("コピー"), QStringLiteral("選択とマスク"), QStringLiteral("選択を解除")}));
        // (an area as the selection tools give it: its outline in mm)
        b.window->canvas()->set_selection(
            Json{{"poly", Json::array({Json::array({30, 40}), Json::array({90, 40}), Json::array({90, 90}), Json::array({30, 90})})}});
        b.window->canvas()->update();
        QVERIFY(gui_test::wait_for([&] { return launcher->isVisible(); }, 5000));
        // within the canvas, under the selection's middle
        QVERIFY(QRect(QPoint(0, 0), b.window->canvas()->size()).contains(launcher->geometry()));
        QToolButton* clear = nullptr;
        for (QToolButton* button : launcher->findChildren<QToolButton*>()) {
            if (button->text() == QStringLiteral("選択を解除")) clear = button;
        }
        QVERIFY(clear != nullptr);
        clear->click();
        QVERIFY(!b.window->canvas()->selection().has_value());
        b.window->canvas()->update();
        QVERIFY(gui_test::wait_for([&] { return !launcher->isVisible(); }, 5000));
    }

    // The menu paths walked once (menu_places: the commands named as the window opens, and コマンドを探す) are where()'s for
    // every item of every menu — one in two menus at its first place — and so every command's name is its words, with its
    // place when another command has the same words (the user's decision 8).
    void everyItemsPlaceIsWhereFindsIt() {
        Book b;
        QMainWindow* window = b.window.get();
        const auto known = app::menu_places(window);
        QSet<const QAction*> items;
        QStringList wrong;
        int deep = 0;
        std::function<void(QMenu*)> walk = [&](QMenu* menu) {
            for (QAction* item : menu->actions()) {
                items.insert(item);
                const auto found = known.find(item);
                const QString place = found != known.end() ? found->second : QStringLiteral("(none)");
                if (place != app::where(window, item)) wrong << item->text() + QStringLiteral(": ") + place + QStringLiteral(" / ") + app::where(window, item);
                if (place.count(QStringLiteral(" → ")) >= 2) ++deep;
                if (item->menu() != nullptr) walk(item->menu());
            }
        };
        for (QAction* top : window->menuBar()->actions()) {
            if (top->menu() != nullptr) walk(top->menu());
        }
        QCOMPARE(wrong, QStringList());
        QCOMPARE(static_cast<qsizetype>(known.size()), items.size());
        QVERIFY(items.size() > 300);
        QVERIFY(deep > 0);  // (menus inside menus inside menus too)
        const auto names = app::command_names(window);
        std::map<QString, int> count;
        for (const auto& [action, name] : names) ++count[action->text()];
        int placed = 0;
        for (const auto& [action, name] : names) {
            const QString place = count[action->text()] > 1 ? app::where(window, action) : QString();
            const QString expected = place.isEmpty() ? action->text() : action->text() + QStringLiteral("（") + place + QStringLiteral("）");
            if (name != expected) wrong << name + QStringLiteral(" / ") + expected;
            if (!place.isEmpty()) ++placed;
        }
        QCOMPARE(wrong, QStringList());
        QVERIFY(placed > 0);  // (レベル補正… among them: decision 8)
    }

    // A command renamed since its name was kept in the quick-access panel or the command bar keeps its place under its new
    // name — PC1b's 色の校正 (Python's 「CMYK で見る（色校正）」) among them, as the keys (preferences' RENAMED).
    void aRenamedCommandKeepsItsPlace() {
        Book b;
        app::settings()->setValue(QStringLiteral("ui/quick"), QStringLiteral("CMYK で見る（色校正）\tレイヤーを削除\t切り取り\t履歴…"));
        QCOMPARE(app::chosen(QStringLiteral("ui/quick"), app::kDefaultQuick),
                 (QStringList{QStringLiteral("色の校正"), QStringLiteral("レイヤーを消す"), QStringLiteral("カット"), QStringLiteral("ヒストリー…")}));
        app::settings()->remove(QStringLiteral("ui/quick"));
        QVERIFY(app::by_text(b.window.get()).count(QStringLiteral("色の校正")) == 1);
        QVERIFY(app::by_text(b.window.get()).count(QStringLiteral("カット")) == 1);
    }

    // PC3-0 (the user's decision 5: the selection's, 編集's, 変形's and the moving's commands by Photoshop's names): every
    // name the table reads an old one as is a command in the menus now and no old name is, so nothing chosen under an old
    // name is lost; the keys chosen under an old name stay with the command (the command bar's names likewise).
    void theRenamedCommandsAreAllFound() {
        Book b;
        QSet<QString> now;
        for (const auto& [action, name] : app::command_names(b.window.get())) {
            now.insert(name);
            now.insert(action->text());  // (words two commands share are told apart by their places there: decision 8)
        }
        // (a new name may also be a command that holds keys though no menu has it — a tool's settings' button, as PC4
        // E-0's 選んだ線を描画色にする —: renamed() reads the keys chosen under its old name)
        QSet<QString> alive = now;
        const auto names = app::command_names(b.window.get());
        for (QAction* action : app::preferences::commands(b.window.get())) alive.insert(app::command_name(names, action));
        int pc3 = 0;
        for (const auto& [name, before] : app::preferences::renamed()) {
            QVERIFY2(alive.contains(name), qPrintable(name));
            // (PC4 E4: an old name a later command took for its own — the グラデーション panel's — is read by the mark of
            // the list or the key it is in: preferences::reused_names(), theGradientPanelsNameIsApart)
            QVERIFY2(!now.contains(before) || app::preferences::reused_name(before), qPrintable(before));
            if (name == QStringLiteral("長方形選択ツール") || name == QStringLiteral("カット") || name == QStringLiteral("移動ツール") ||
                name == QStringLiteral("自由な形に") || name == QStringLiteral("前面へ"))
                ++pc3;
        }
        QCOMPARE(pc3, 5);
        // (each command renamed by its id, under the old name: its new one)
        const std::vector<std::pair<const char*, const char*>> renamed{
            {"act_marquee", "範囲選択（長方形）"}, {"act_sel_ellipse", "範囲選択（楕円）"}, {"act_lasso", "範囲選択（投げ縄）"},
            {"act_sel_polyline", "範囲選択（折れ線）"}, {"act_wand", "自動選択"}, {"act_select_all", "すべて選択"},
            {"act_sel_grow", "選択範囲を広げる…"}, {"act_sel_shrink", "選択範囲を狭める…"}, {"act_sel_layer", "描画部分から選択"},
            {"act_sel_to_channel", "選択範囲をチャンネルに保存…"}, {"act_quick_mask", "クイックマスク"}, {"act_cut", "切り取り"},
            {"act_paste", "貼り付け"}, {"act_delete_area", "選択範囲を消す"}, {"act_fill_selection", "選択範囲を塗る"},
            {"act_flip_h", "左右反転"}, {"act_flip_v", "上下反転"}, {"act_warp_perspective", "自由変形（遠近・4 隅）"},
            {"act_warp_mesh", "自由変形（メッシュ・3×3）"}, {"act_warp_mesh_grid", "自由変形（メッシュ・格子の数を決める）…"},
            {"act_warp_apply", "自由変形を確定"}, {"act_move", "レイヤー移動"}, {"act_layer_up", "レイヤーを前へ"},
            {"act_layer_down", "レイヤーを後ろへ"}, {"act_liquify", "ゆがみ（指で押す）"}};
        QStringList bar;
        for (const auto& [attribute, before] : renamed) {
            const QString old = QString::fromUtf8(before);
            const QString name = b.window->action(QString::fromLatin1(attribute))->text();
            bool kept = false;
            for (auto [it, end] = app::preferences::renamed().equal_range(name); it != end; ++it) kept = kept || it->second == old;
            QVERIFY2(kept, attribute);
            bar << old;
        }
        app::settings()->setValue(QStringLiteral("ui/commandbar"), bar.join(QLatin1Char('\t')));
        const QStringList read = app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar);
        app::settings()->remove(QStringLiteral("ui/commandbar"));
        QCOMPARE(read.size(), static_cast<qsizetype>(renamed.size()));
        for (std::size_t i = 0; i < renamed.size(); ++i) QCOMPARE(read[static_cast<qsizetype>(i)], b.window->action(QString::fromLatin1(renamed[i].first))->text());
        // a key chosen under the old name
        app::settings()->setValue(QStringLiteral("shortcuts/切り取り"), QStringLiteral("Ctrl+Alt+K"));
        app::preferences::apply_shortcuts(b.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/切り取り"));
        QCOMPARE(b.window->action("act_cut")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
        // a key chosen under an old name whose new words another command also has (ワープ: 編集 → 変形, and レイヤー →
        // スマートオブジェクト): the first of them in the menus, the renamed one, has it — not the other
        app::settings()->setValue(QStringLiteral("shortcuts/自由変形（メッシュ・3×3）"), QStringLiteral("Ctrl+Alt+J"));
        app::preferences::apply_shortcuts(b.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/自由変形（メッシュ・3×3）"));
        QCOMPARE(b.window->action("act_warp_mesh")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+J")));
        QVERIFY(b.window->action("act_smart_warp")->shortcut() != QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    }

    // PC4 F-0 (節 F 5.1: ファイル's commands by Photoshop's names): each renamed command is read under its old name — the
    // command bar's and the quick-access panel's places, a key chosen under it — and the old names are in no menu.
    void theRenamedFileCommandsAreFound() {
        Book b;
        const std::vector<std::pair<const char*, const char*>> renamed{
            {"act_new", "新しい原稿…"},  {"act_save_as", "別の場所に保存…"}, {"act_export", "書き出し…"},
            {"act_print", "印刷…"},      {"act_quit", "Genko を終わる"},     {"act_export_lookup", "カラールックアップテーブルを書き出す…"}};
        QSet<QString> now;
        for (const auto& [action, name] : app::command_names(b.window.get())) now.insert(name);
        QStringList bar;
        for (const auto& [attribute, before] : renamed) {
            const QString old = QString::fromUtf8(before);
            const QString name = b.window->action(QString::fromLatin1(attribute))->text();
            QVERIFY2(name != old, attribute);
            // (PC5 H-0: 書き出し… is 編集 → 環境設定 → 書き出し…'s own name since — a reused name, read by its mark)
            QVERIFY2(now.contains(name) && (!now.contains(old) || app::preferences::reused_name(old)), attribute);
            bool kept = false;
            for (auto [it, end] = app::preferences::renamed().equal_range(name); it != end; ++it) kept = kept || it->second == old;
            QVERIFY2(kept, attribute);
            bar << old;
        }
        app::settings()->setValue(QStringLiteral("ui/commandbar"), bar.join(QLatin1Char('\t')));
        const QStringList read = app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar);
        app::settings()->remove(QStringLiteral("ui/commandbar"));
        QCOMPARE(read.size(), static_cast<qsizetype>(renamed.size()));
        for (std::size_t i = 0; i < renamed.size(); ++i) QCOMPARE(read[static_cast<qsizetype>(i)], b.window->action(QString::fromLatin1(renamed[i].first))->text());
        // (the default command bar ends with the book's export by its new name)
        QCOMPARE(app::kDefaultCommandBar.back(), QStringLiteral("原稿を書き出す…"));
        // a key chosen under the old name
        app::settings()->setValue(QStringLiteral("shortcuts/Genko を終わる"), QStringLiteral("Ctrl+Alt+Shift+Q"));
        app::preferences::apply_shortcuts(b.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/Genko を終わる"));
        QCOMPARE(b.window->action("act_quit")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Q")));
    }

    // PC4 E-0 (the user's decision 2: the drawing tools and the colours by Photoshop's names): each command renamed is
    // read under its old name (renamed()), its id and key kept; the quick access, the command bar and the keys chosen
    // under the old names stay with the commands; the layers, lines and brushes keep their names.
    void theDrawingToolsAreRenamedAndFound() {
        Book b;
        const std::vector<std::tuple<const char*, const char*, const char*, const char*>> renamed{
            {"act_pen", "ペン", "ブラシツール", "B"},
            {"act_eraser", "消しゴム", "消しゴムツール", "E"},
            {"act_picker", "スポイト", "スポイトツール", "I"},
            {"act_fill", "塗りつぶし", "塗りつぶしツール", "Shift+Q"},  // (PC5 H-0: the Photoshop key set)
            
            {"act_gradient", "グラデーション", "グラデーションツール", "G"},
            {"act_swap_colour", "メインとサブの色を入れ替える", "描画色と背景色を入れ替え", "X"},
            {"act_color", "ペンの色…", "カラーピッカー（描画色）…", ""},
            {"act_thicker", "太く（ペン・消しゴム）", "ブラシサイズを大きく", "]"},
            {"act_thinner", "細く（ペン・消しゴム）", "ブラシサイズを小さく", "["},
            {"act_vector_colour", "選んだ線をペンの色にする", "選んだ線を描画色にする", ""}};
        QStringList bar;
        for (const auto& [attribute, before, now, key] : renamed) {
            QAction* act = b.window->action(QString::fromLatin1(attribute));
            QCOMPARE(act->text(), QString::fromUtf8(now));
            QCOMPARE(act->shortcut(), QKeySequence(QString::fromLatin1(key)));
            bool kept = false;
            for (auto [it, end] = app::preferences::renamed().equal_range(act->text()); it != end; ++it) kept = kept || it->second == QString::fromUtf8(before);
            QVERIFY2(kept, attribute);
            bar << QString::fromUtf8(before);
        }
        // the layer mask menu's マスクを編集する (named by its attribute)
        QAction* mask_edit = nullptr;
        for (QAction* act : b.window->findChildren<QAction*>())
            if (act->property("genko_attribute").toString() == QStringLiteral("act_mask_edit")) mask_edit = act;
        QVERIFY(mask_edit != nullptr);
        QCOMPARE(mask_edit->text(), QStringLiteral("マスクを編集する（ブラシで見せる・消しゴムで隠す）"));
        bool mask_kept = false;
        for (auto [it, end] = app::preferences::renamed().equal_range(mask_edit->text()); it != end; ++it)
            mask_kept = mask_kept || it->second == QStringLiteral("マスクを編集する（ペンで見せる・消しゴムで隠す）");
        QVERIFY(mask_kept);
        // the new: 描画色と背景色を初期設定に戻す (D), F5 ブラシ設定, F6 カラー
        QCOMPARE(b.window->action(QStringLiteral("act_default_colours"))->shortcut(), QKeySequence(QStringLiteral("D")));
        const auto toggle = [&](const QString& title) {
            QDockWidget* dock = b.dock(title);
            return dock != nullptr ? dock->toggleViewAction() : nullptr;
        };
        QVERIFY(toggle(QStringLiteral("ブラシ設定")) != nullptr && toggle(QStringLiteral("カラー")) != nullptr);
        QCOMPARE(toggle(QStringLiteral("ブラシ設定"))->shortcut(), QKeySequence(QStringLiteral("F5")));
        QCOMPARE(toggle(QStringLiteral("カラー"))->shortcut(), QKeySequence(QStringLiteral("F6")));
        // the command bar and the quick access chosen under the old names
        app::settings()->setValue(QStringLiteral("ui/commandbar"), bar.join(QLatin1Char('\t')));
        const QStringList read = app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar);
        app::settings()->remove(QStringLiteral("ui/commandbar"));
        QCOMPARE(read.size(), static_cast<qsizetype>(renamed.size()));
        for (std::size_t i = 0; i < renamed.size(); ++i) QCOMPARE(read[static_cast<qsizetype>(i)], QString::fromUtf8(std::get<2>(renamed[i])));
        const auto names = app::by_text(b.window.get());
        for (const QString& name : app::kDefaultQuick) QVERIFY2(names.count(name), qPrintable(name));
        // a key chosen under the old name
        app::settings()->setValue(QStringLiteral("shortcuts/ペン"), QStringLiteral("Ctrl+Alt+K"));
        app::settings()->setValue(QStringLiteral("shortcuts/メインとサブの色を入れ替える"), QStringLiteral("Ctrl+Alt+J"));
        app::preferences::apply_shortcuts(b.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/ペン"));
        app::settings()->remove(QStringLiteral("shortcuts/メインとサブの色を入れ替える"));
        QCOMPARE(b.window->action("act_pen")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
        QCOMPARE(b.window->action("act_swap_colour")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+J")));
        app::preferences::apply_shortcuts(b.window.get());
        QCOMPARE(b.window->action("act_pen")->shortcut(), QKeySequence(QStringLiteral("B")));
        // the layers and the brushes keep theirs
        QCOMPARE(b.window->action("act_layer_pen")->text(), QStringLiteral("新しいペンのレイヤー"));
    }

    // PC5 H-0 (節 H 1.2, the user's decision 12: 表示, ウィンドウ and 編集 by Photoshop's names): each renamed command is read
    // under its old name — the command bar's places, a key chosen under it, 検索 —, its id kept, the old names in no menu.
    // 表示 → 定規 shares its words with the 定規 tool: what was kept under 「定規」 alone is still the tool's
    // (preferences::words_owner); 書き出し… — 原稿を書き出す…'s old name — is 編集 → 環境設定 → 書き出し…'s own since: kept
    // before (no mark) it is the book's export (reused_names).
    void theRenamedViewCommandsAreFound() {
        Book b;
        const std::vector<std::pair<const char*, const char*>> renamed{
            {"act_undo", "元に戻す"},        {"act_redo", "やり直す"},          {"act_fit", "全体を表示"},
            {"act_zoom_in", "拡大"},         {"act_zoom_out", "縮小"},          {"act_actual", "原寸（紙の大きさ）"},
            {"act_zoom_tool", "虫めがね"},   {"act_mirror", "左右反転して見る"}, {"act_scale", "目盛りを表示"},
            {"act_show_rulers", "定規を表示"}, {"act_snap", "定規にスナップ"},   {"act_history", "履歴…"},
            {"act_find_command", "コマンドを探す…"}, {"act_new_window", "新しいウィンドウ（同じ原稿）"}, {"act_prefs", "環境設定…"}};
        const auto names = app::command_names(b.window.get());
        QSet<QString> now;
        for (const auto& [action, name] : names) now.insert(name);
        QStringList bar;
        for (const auto& [attribute, before] : renamed) {
            QAction* act = b.act(attribute);
            QVERIFY2(act != nullptr, attribute);
            const QString name = app::command_name(names, act);
            QVERIFY2(!now.contains(QString::fromUtf8(before)), before);
            bool kept = false;
            for (auto [it, end] = app::preferences::renamed().equal_range(name); it != end; ++it) kept = kept || it->second == QString::fromUtf8(before);
            QVERIFY2(kept, qPrintable(name));
            bar << QString::fromUtf8(before);
        }
        QCOMPARE(app::command_name(names, b.act("act_mirror")), QStringLiteral("水平方向に反転（表示）"));
        QCOMPARE(app::command_name(names, b.act("act_scale")), QStringLiteral("定規（表示）"));
        QCOMPARE(app::command_name(names, b.act("act_ruler")), QStringLiteral("定規（ツール）"));
        // the command bar chosen under the old names
        app::settings()->setValue(QStringLiteral("ui/commandbar"), bar.join(QLatin1Char('\t')));
        const QStringList read = app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar);
        app::settings()->remove(QStringLiteral("ui/commandbar"));
        const auto by = app::by_text(b.window.get());
        QCOMPARE(read.size(), static_cast<qsizetype>(renamed.size()));
        for (std::size_t i = 0; i < renamed.size(); ++i) {
            const auto found = by.find(read[static_cast<qsizetype>(i)]);
            QVERIFY2(found != by.end() && found->second == b.act(renamed[i].first), renamed[i].first);
        }
        for (const QString& name : app::kDefaultCommandBar + app::kDefaultQuick) QVERIFY2(name == QLatin1String("|") || by.count(name), qPrintable(name));
        // the panels' switches by their old titles
        app::settings()->setValue(QStringLiteral("ui/quick"), QStringLiteral("全体図\tツールの設定\t履歴"));
        QCOMPARE(app::chosen(QStringLiteral("ui/quick"), app::kDefaultQuick),
                 (QStringList{QStringLiteral("ナビゲーター"), QStringLiteral("オプション"), QStringLiteral("ヒストリー")}));
        app::settings()->remove(QStringLiteral("ui/quick"));
        // keys chosen under the old names
        app::settings()->setValue(QStringLiteral("shortcuts/左右反転して見る"), QStringLiteral("Ctrl+Alt+K"));
        app::settings()->setValue(QStringLiteral("shortcuts/目盛りを表示"), QStringLiteral("Ctrl+Alt+J"));
        app::settings()->setValue(QStringLiteral("shortcuts/全体図"), QStringLiteral("Ctrl+Alt+N"));
        app::preferences::apply_shortcuts(b.window.get());
        QCOMPARE(b.act("act_mirror")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
        QCOMPARE(b.act("act_scale")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+J")));
        QCOMPARE(b.dock(QStringLiteral("ナビゲーター"))->toggleViewAction()->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+N")));
        QVERIFY(b.act("act_flip_h")->shortcut() != QKeySequence(QStringLiteral("Ctrl+Alt+K")));  // (編集 → 変形's, not the view's)
        for (const char* key : {"shortcuts/左右反転して見る", "shortcuts/目盛りを表示", "shortcuts/全体図"}) app::settings()->remove(QString::fromUtf8(key));
        // 「定規」 alone: the tool's (its key and its place in a list), not 表示 → 定規's
        app::settings()->setValue(QStringLiteral("shortcuts/定規"), QStringLiteral("Ctrl+Alt+Shift+J"));
        app::preferences::apply_shortcuts(b.window.get());
        app::settings()->remove(QStringLiteral("shortcuts/定規"));
        QCOMPARE(b.act("act_ruler")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+J")));
        QCOMPARE(b.act("act_scale")->shortcut(), QKeySequence(QStringLiteral("Ctrl+R")));
        QCOMPARE(app::by_text(b.window.get()).at(QStringLiteral("定規")), b.act("act_ruler"));
        app::preferences::apply_shortcuts(b.window.get());
        // 書き出し…: kept before (no mark) the book's export; kept since, 編集 → 環境設定 → 書き出し…
        QVERIFY(app::preferences::reused_name(QStringLiteral("書き出し…")));
        app::settings()->remove(QStringLiteral("names_current/ui/quick"));
        app::settings()->setValue(QStringLiteral("ui/quick"), QStringLiteral("書き出し…"));
        QCOMPARE(app::chosen(QStringLiteral("ui/quick"), app::kDefaultQuick), QStringList{QStringLiteral("原稿を書き出す…")});
        app::keep(QStringLiteral("ui/quick"), {QStringLiteral("書き出し…")});
        QCOMPARE(app::chosen(QStringLiteral("ui/quick"), app::kDefaultQuick), QStringList{QStringLiteral("書き出し…")});
        QCOMPARE(app::by_text(b.window.get()).at(QStringLiteral("書き出し…")), b.act("act_prefs_export"));
        app::settings()->remove(QStringLiteral("ui/quick"));
        app::settings()->remove(QStringLiteral("names_current/ui/quick"));
        // a key chosen under an old name, put back to the command's first (すべて最初のキーに戻す → 決める): the old name's
        // key goes too — the next apply does not read it back (the review of PC5 H-0, P2-5)
        app::settings()->setValue(QStringLiteral("shortcuts/元に戻す"), QStringLiteral("Ctrl+Alt+Shift+U"));
        app::preferences::apply_all(b.window.get());
        QCOMPARE(b.act("act_undo")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+U")));
        {
            app::ShortcutsDialog dialog(b.window.get());  // (PC5 H-d: キーボードショートカット…'s own window)
            dialog.reset_keys();
            dialog.save();
        }
        QVERIFY(!app::settings()->contains(QStringLiteral("shortcuts/元に戻す")));
        app::preferences::apply_all(b.window.get());
        QCOMPARE(b.act("act_undo")->shortcut(), QKeySequence(QKeySequence::Undo));
        // 検索 finds a command by its old name
        app::CommandSearch* search = b.window->find_command();
        search->query->setText(QStringLiteral("全体を表示"));
        QVERIFY(!search->found().empty());
        QCOMPARE(search->found().front(), b.act("act_fit"));
        search->close();
    }

    // PC4 E4 (E-0's promise): グラデーション — the グラデーションツール's old name — is the グラデーション panel's own name since; a
    // list and the keys kept before the panel (no mark) read it as the tool, those kept since (marked when written) as the
    // panel: the two never taken for each other, nor one key given to both.
    void theGradientPanelsNameIsApart() {
        Book b;
        const auto store = app::settings();
        QAction* panel = b.dock(QStringLiteral("グラデーション"))->toggleViewAction();
        QAction* tool = b.window->action("act_gradient");
        QCOMPARE(panel->text(), QStringLiteral("グラデーション"));
        QVERIFY(app::preferences::reused_name(QStringLiteral("グラデーション")));
        // a list kept before: the tool; kept since (keep() marks it): the panel
        store->remove(QStringLiteral("names_current/ui/commandbar"));
        store->setValue(QStringLiteral("ui/commandbar"), QStringLiteral("グラデーション\tペン"));
        QCOMPARE(app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar),
                 (QStringList{QStringLiteral("グラデーションツール"), QStringLiteral("ブラシツール")}));
        app::keep(QStringLiteral("ui/commandbar"), {QStringLiteral("グラデーション"), QStringLiteral("ブラシツール")});
        QVERIFY(app::preferences::names_current(QStringLiteral("ui/commandbar")));
        QCOMPARE(app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar),
                 (QStringList{QStringLiteral("グラデーション"), QStringLiteral("ブラシツール")}));
        const auto names = app::by_text(b.window.get());
        QCOMPARE(names.find(QStringLiteral("グラデーション"))->second, panel);
        // the list written again by a build before E4 (the mark is of another list): the tool again
        store->setValue(QStringLiteral("ui/commandbar"), QStringLiteral("ペン\tグラデーション"));
        QVERIFY(!app::preferences::names_current(QStringLiteral("ui/commandbar")));
        QCOMPARE(app::chosen(QStringLiteral("ui/commandbar"), app::kDefaultCommandBar),
                 (QStringList{QStringLiteral("ブラシツール"), QStringLiteral("グラデーションツール")}));
        store->remove(QStringLiteral("ui/commandbar"));
        store->remove(QStringLiteral("names_current/ui/commandbar"));
        // keys kept before: the tool's (the panel takes none of them)
        store->remove(QStringLiteral("names_current/shortcuts/グラデーション"));
        store->setValue(QStringLiteral("shortcuts/グラデーション"), QStringLiteral("Ctrl+Alt+G"));
        app::preferences::apply_shortcuts(b.window.get());
        QCOMPARE(tool->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+G")));
        QVERIFY(panel->shortcut().isEmpty());
        // kept since (the preferences mark them as they write them): the panel's, the tool its own key
        app::preferences::mark_names_current(QStringLiteral("shortcuts/グラデーション"));
        app::preferences::apply_shortcuts(b.window.get());
        QCOMPARE(panel->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+G")));
        QCOMPARE(tool->shortcut(), QKeySequence(QStringLiteral("G")));
        // a key written there again by a build before E4: the tool's again
        store->setValue(QStringLiteral("shortcuts/グラデーション"), QStringLiteral("Ctrl+Alt+H"));
        app::preferences::apply_shortcuts(b.window.get());
        QCOMPARE(tool->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+H")));
        QVERIFY(panel->shortcut().isEmpty());
        store->remove(QStringLiteral("shortcuts/グラデーション"));
        store->remove(QStringLiteral("names_current/shortcuts/グラデーション"));
        app::preferences::apply_shortcuts(b.window.get());
        QVERIFY(panel->shortcut().isEmpty());
    }

    // PC5 H-d (window.workspace.*, SPEC VIEW-01「ワークスペース」): Photoshop's workspaces built in — each its panels (the ones
    // Genko has), the one chosen checked and the next window opened in it; ピクセルアート with the pixels' grid; a workspace
    // left chosen again as it was left (環境設定 → ワークスペース → ワークスペースの変更を記憶), ワークスペースをリセット as it is built;
    // 初期設定 the window's first panels; their names never a kept one's; the approval's panels never touched.
    void theWorkspacesOfPhotoshop() {
        Book b;
        app::MainWindow* w = b.window.get();
        const auto shown = [&b](const char* title) {
            QDockWidget* d = b.dock(QString::fromUtf8(title));
            return d != nullptr && d->isVisible();
        };
        std::map<QString, bool> first;
        for (QDockWidget* d : w->findChildren<QDockWidget*>()) first[d->objectName()] = d->isVisible();
        QCOMPARE(w->current_workspace(), QStringLiteral("初期設定"));
        QCOMPARE(app::presets().size(), std::size_t{6});
        QDockWidget* box = b.dock(QStringLiteral("承認箱"));
        QVERIFY(box != nullptr);
        const bool box_shown = box->isVisible();
        b.act("act_workspace_painting")->trigger();
        QCOMPARE(w->current_workspace(), QStringLiteral("ペイント"));
        QVERIFY(b.act("act_workspace_painting")->isChecked() && !b.act("act_workspace_essentials")->isChecked());
        for (const char* title : {"ブラシ", "ブラシ設定", "カラー", "レイヤー", "ナビゲーター", "オプション"}) QVERIFY2(shown(title), title);
        for (const char* title : {"ページ", "台詞", "ヒストグラム"}) QVERIFY2(!shown(title), title);
        QCOMPARE(box->isVisible(), box_shown);
        b.act("act_workspace_photography")->trigger();
        for (const char* title : {"ヒストグラム", "色調補正", "チャンネル", "レイヤー"}) QVERIFY2(shown(title), title);
        QVERIFY(!shown("ブラシ"));
        b.act("act_workspace_motion")->trigger();
        QVERIFY(shown("タイムライン"));
        b.act("act_workspace_graphic_and_web")->trigger();
        for (const char* title : {"台詞", "シェイプ", "パス", "スタイル"}) QVERIFY2(shown(title), title);
        QVERIFY(!b.act("act_pixel_grid")->isChecked());
        b.act("act_workspace_pixel_art")->trigger();
        QVERIFY(b.act("act_pixel_grid")->isChecked() && b.act("act_pixel_art")->isChecked());
        QVERIFY(w->canvas()->show_pixel_grid);
        b.act("act_pixel_grid")->setChecked(false);
        b.act("act_pixel_art")->setChecked(false);
        // a workspace left: chosen again as it was left; reset: as it is built
        b.act("act_workspace_painting")->trigger();
        b.dock(QStringLiteral("ブラシ"))->hide();
        b.act("act_workspace_photography")->trigger();
        b.act("act_workspace_painting")->trigger();
        QVERIFY(!shown("ブラシ"));
        b.act("act_workspace_reset")->trigger();
        QVERIFY(shown("ブラシ"));
        QVERIFY2(w->last_notice().contains(QStringLiteral("「ペイント」")), qPrintable(w->last_notice()));
        // ... unless ワークスペースの変更を記憶 is off: as it is built each time
        app::settings()->setValue(QStringLiteral("ui/workspace_remember"), QStringLiteral("false"));
        b.dock(QStringLiteral("ブラシ"))->hide();
        b.act("act_workspace_photography")->trigger();
        b.act("act_workspace_painting")->trigger();
        QVERIFY(shown("ブラシ"));
        app::settings()->remove(QStringLiteral("ui/workspace_remember"));
        // the next window opens in it
        {
            Book other;
            QCOMPARE(other.window->current_workspace(), QStringLiteral("ペイント"));
            QVERIFY(other.dock(QStringLiteral("ブラシ"))->isVisible());
            QVERIFY(!other.dock(QStringLiteral("ページ"))->isVisible());
        }
        app::ask::set_responder(b.answers.responder);  // (the other window's answers gone with it: this one's again)
        // 初期設定: the window's first panels
        b.act("act_workspace_essentials")->trigger();
        for (QDockWidget* d : w->findChildren<QDockWidget*>()) {
            if (first.contains(d->objectName())) QVERIFY2(d->isVisible() == first[d->objectName()], qPrintable(d->objectName()));
        }
        // Photoshop's names are never a kept workspace's
        for (const char* name : {"写真", "初期設定", "Essentials", "pixelArt", "グラフィックと Web"}) QVERIFY2(!w->save_workspace(QString::fromUtf8(name)), name);
        QVERIFY(w->last_notice().contains(QStringLiteral("Genko にあるワークスペースの名前")));
        QVERIFY(app::workspaces().isEmpty());
    }

    // PC5 H-d (window.workspace.newWorkspace): 新規ワークスペース… keeps the keys, the menus and the toolbar with the panels when
    // chosen (Photoshop's), refuses a built-in workspace's name, and puts them back when the workspace is chosen again.
    void aNewWorkspaceKeepsTheKeysMenusAndToolbar() {
        Book b;
        app::MainWindow* w = b.window.get();
        app::settings()->setValue(QStringLiteral("shortcuts/移動ツール"), QStringLiteral("Ctrl+Alt+Shift+F2"));
        app::menu_look::set_hidden({QStringLiteral("100%")});
        app::toolbar::keep({QStringLiteral("act_move"), QStringLiteral("act_pen")}, {QStringLiteral("act_3d")});
        app::preferences::apply_all(w);
        QCOMPARE(b.act("act_move")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F2")));
        QCOMPARE(w->tool_palette()->actions().front(), b.act("act_move"));
        const auto named = [](const QString& name, bool all) {
            return [name, all](QDialog* dialog) {
                auto* asked = qobject_cast<app::NewWorkspaceDialog*>(dialog);
                if (asked == nullptr) return 0;
                asked->name->setText(name);
                for (QCheckBox* box : {asked->keys, asked->menus, asked->toolbar}) box->setChecked(all);
                return static_cast<int>(QDialog::Accepted);
            };
        };
        b.on_dialog = named(QStringLiteral("ペイント"), true);
        QVERIFY(!w->new_workspace());
        QVERIFY(w->last_notice().contains(QStringLiteral("Genko にあるワークスペースの名前")));
        QVERIFY(app::workspaces().isEmpty());
        b.on_dialog = named(QStringLiteral("仕上げ用"), true);
        QVERIFY(w->new_workspace());
        b.on_dialog = named(QStringLiteral("パネルだけ"), false);
        QVERIFY(w->new_workspace());
        QCOMPARE(app::workspaces(), (QStringList{QStringLiteral("パネルだけ"), QStringLiteral("仕上げ用")}));
        // changed since
        app::settings()->remove(QStringLiteral("shortcuts/移動ツール"));
        app::menu_look::set_hidden({});
        app::toolbar::keep(app::toolbar::default_order(), {});
        app::preferences::apply_all(w);
        QCOMPARE(b.act("act_move")->shortcut(), QKeySequence(QStringLiteral("V")));
        QCOMPARE(w->tool_palette()->actions().front(), b.act("act_select"));
        // a workspace kept without them leaves them as they are; one kept with them puts them back
        w->choose_workspace(QStringLiteral("パネルだけ"));
        QCOMPARE(b.act("act_move")->shortcut(), QKeySequence(QStringLiteral("V")));
        w->choose_workspace(QStringLiteral("仕上げ用"));
        QCOMPARE(w->current_workspace(), QStringLiteral("仕上げ用"));
        QCOMPARE(b.act("act_move")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F2")));
        QCOMPARE(app::menu_look::hidden(), QStringList{QStringLiteral("100%")});
        QCOMPARE(w->tool_palette()->actions().front(), b.act("act_move"));
        QVERIFY(w->extra_tools() != nullptr && w->extra_tools()->menu()->actions().contains(b.act("act_3d")));
        // (as they were)
        b.act("act_workspace_essentials")->trigger();
        for (const char* key : {"shortcuts/移動ツール", "menus/hidden", "toolbar/order", "toolbar/hidden"}) app::settings()->remove(QString::fromUtf8(key));
        app::preferences::apply_all(w);
        QVERIFY(w->extra_tools() == nullptr);
    }

    // PC5 H-d (window.workspace.lockWorkspace): ワークスペースをロック — the panels cannot be moved nor taken out of the window
    // (every window, the next one too), closed still; unlocked, as they were.
    void theWorkspaceLocks() {
        Book b;
        QDockWidget* layers = b.dock(QStringLiteral("レイヤー"));
        const QDockWidget::DockWidgetFeatures own = layers->features();
        QVERIFY(own.testFlag(QDockWidget::DockWidgetMovable));
        b.act("act_lock_workspace")->trigger();
        QVERIFY(b.act("act_lock_workspace")->isChecked());
        for (QDockWidget* d : b.window->findChildren<QDockWidget*>()) {
            QVERIFY2(!d->features().testFlag(QDockWidget::DockWidgetMovable) && !d->features().testFlag(QDockWidget::DockWidgetFloatable),
                     qPrintable(d->objectName()));
        }
        QCOMPARE(layers->features().testFlag(QDockWidget::DockWidgetClosable), own.testFlag(QDockWidget::DockWidgetClosable));
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace_locked")).toString(), QStringLiteral("true"));
        {
            Book other;
            QVERIFY(!other.dock(QStringLiteral("レイヤー"))->features().testFlag(QDockWidget::DockWidgetMovable));
            QVERIFY(other.act("act_lock_workspace")->isChecked());
        }
        app::ask::set_responder(b.answers.responder);  // (the other window's answers gone with it: this one's again)
        b.act("act_workspace_painting")->trigger();  // (a workspace chosen: its panels, still locked)
        QVERIFY(!layers->features().testFlag(QDockWidget::DockWidgetMovable));
        b.act("act_lock_workspace")->trigger();
        QCOMPARE(layers->features(), own);
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace_locked")).toString(), QStringLiteral("false"));
        b.act("act_workspace_essentials")->trigger();
    }

    // PC5 H-d (edit.keyboardShortcuts, SPEC VIEW-01「キーボードショートカットとメニュー」): キーボードショートカット…'s own window —
    // every command by its menu; a key another command has said, taken from it or given up; one back to the set's; found by
    // a part of its name or its key; 概要… as a page; the person's own set saved (never under a built-in set's name), chosen,
    // kept and deleted (never a built-in one).
    void theKeyboardShortcutsWindow() {
        Book b;
        app::MainWindow* w = b.window.get();
        const auto key = [](const char* words) { return QKeySequence(QString::fromLatin1(words)); };
        {
            app::ShortcutsDialog dialog(w);
            QCOMPARE(dialog.windowTitle(), QStringLiteral("キーボードショートカットとメニュー"));
            QCOMPARE(dialog.tabs->count(), 2);
            QCOMPARE(dialog.tabs->tabText(0), QStringLiteral("キーボードショートカット"));
            QCOMPARE(dialog.tabs->tabText(1), QStringLiteral("メニュー"));
            QTreeWidgetItem* move = dialog.row(QStringLiteral("移動ツール"));
            QVERIFY(move != nullptr && move->parent() != nullptr && move->toolTip(0).contains(QStringLiteral(" → ")));
            QCOMPARE(dialog.tree->topLevelItem(0)->text(0), QStringLiteral("ファイル"));
            QCOMPARE(dialog.editor(QStringLiteral("移動ツール"))->keySequence(), key("V"));
            // a key another command has: said, given up (やめる) or taken from it (前のコマンドから外して付ける)
            dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(key("V"));
            QVERIFY2(dialog.clash->text().contains(QStringLiteral("「V」")) && dialog.clash->text().contains(QStringLiteral("移動ツール")),
                     qPrintable(dialog.clash->text()));
            QVERIFY(dialog.take_button->isVisibleTo(&dialog) && dialog.keep_button->isVisibleTo(&dialog));
            dialog.keep_previous();
            QCOMPARE(dialog.editor(QStringLiteral("ブラシツール"))->keySequence(), key("B"));
            QCOMPARE(dialog.editor(QStringLiteral("移動ツール"))->keySequence(), key("V"));
            dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(key("V"));
            dialog.take_from_previous();
            QVERIFY(dialog.editor(QStringLiteral("移動ツール"))->keySequence().isEmpty());
            QCOMPARE(dialog.editor(QStringLiteral("ブラシツール"))->keySequence(), key("V"));
            // one back to the set's (its key another's again: said, taken)
            dialog.reset_one(QStringLiteral("移動ツール"));
            QVERIFY(dialog.clash->text().contains(QStringLiteral("ブラシツール")));
            dialog.take_from_previous();
            QVERIFY(dialog.editor(QStringLiteral("ブラシツール"))->keySequence().isEmpty());
            dialog.reset_one(QStringLiteral("ブラシツール"));
            QCOMPARE(dialog.editor(QStringLiteral("ブラシツール"))->keySequence(), key("B"));
            QCOMPARE(dialog.editor(QStringLiteral("移動ツール"))->keySequence(), key("V"));
            // (the row chosen)
            dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(key("F11"));
            dialog.tree->setCurrentItem(dialog.row(QStringLiteral("ブラシツール")));
            dialog.reset_one_button->click();
            QCOMPARE(dialog.editor(QStringLiteral("ブラシツール"))->keySequence(), key("B"));
            // found by a part of its name or its key
            dialog.filter->setText(QStringLiteral("ブラシツール"));
            QVERIFY(!dialog.row(QStringLiteral("ブラシツール"))->isHidden() && dialog.row(QStringLiteral("移動ツール"))->isHidden());
            dialog.filter->setText(QStringLiteral("Ctrl+Alt+Shift+K"));
            QVERIFY(!dialog.row(QStringLiteral("キーボードショートカット…"))->isHidden());
            QVERIFY(dialog.row(QStringLiteral("ブラシツール"))->isHidden());
            dialog.filter->clear();
            QVERIFY(!dialog.row(QStringLiteral("移動ツール"))->isHidden());
            // 概要…: every command, its menu and its keys as a page
            const QString html = dialog.summary_html();
            QVERIFY(html.contains(QStringLiteral("<h2>ファイル</h2>")) && html.contains(QStringLiteral("Photoshop の初期設定")));
            QVERIFY(html.contains(QStringLiteral("移動ツール</td><td>V</td>")));
            QTemporaryDir out;
            const QString page = out.filePath(QStringLiteral("keys.html"));
            b.answers.responder->save_path = [page](const QString&, const QString&) { return page; };
            dialog.summary_button->click();
            QFile written(page);
            QVERIFY(written.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(written.readAll()), html);
            // the person's own set: never a built-in set's name; saved, chosen, kept by 決める
            for (const char* name : {"Photoshop", "genko", "Photoshop の初期設定", "Genko（PC5 より前のキー）"}) {
                QVERIFY2(!dialog.save_set(QString::fromUtf8(name)), name);
            }
            QVERIFY(dialog.clash->text().contains(QStringLiteral("Genko にある組の名前")));
            QVERIFY(app::key_sets::own_sets().isEmpty());
            QVERIFY(!dialog.delete_set_button->isEnabled());
            dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(key("F11"));
            QVERIFY(dialog.save_set(QStringLiteral("私の組")));
            QCOMPARE(dialog.key_set->currentData().toString(), QStringLiteral("own:私の組"));
            QVERIFY(dialog.delete_set_button->isEnabled());
            dialog.save();
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        }
        QCOMPARE(app::key_sets::chosen(), QStringLiteral("own:私の組"));
        QCOMPARE(b.act("act_pen")->shortcut(), key("F11"));
        QVERIFY(!app::settings()->contains(QStringLiteral("shortcuts/ブラシツール")));  // (the set's own key, not the person's over it)
        {
            Book other;  // (the next window: the same set)
            QCOMPARE(other.act("act_pen")->shortcut(), key("F11"));
        }
        app::ask::set_responder(b.answers.responder);  // (the other window's answers gone with it: this one's again)
        {
            app::ShortcutsDialog again(w);
            QCOMPARE(again.key_set->currentData().toString(), QStringLiteral("own:私の組"));
            QCOMPARE(again.editor(QStringLiteral("ブラシツール"))->keySequence(), key("F11"));
            again.key_set->setCurrentIndex(again.key_set->findData(QStringLiteral("photoshop")));
            QCOMPARE(again.editor(QStringLiteral("ブラシツール"))->keySequence(), key("B"));
            QVERIFY(!again.delete_set());  // (a built-in set: never deleted)
            QVERIFY(again.clash->text().contains(QStringLiteral("削除できません")));
            again.key_set->setCurrentIndex(again.key_set->findData(QStringLiteral("own:私の組")));
            QCOMPARE(again.editor(QStringLiteral("ブラシツール"))->keySequence(), key("F11"));
            b.answers.responder->question = [](const QString&, const QString&) { return true; };
            QVERIFY(again.delete_set());
            QCOMPARE(again.key_set->currentData().toString(), QStringLiteral("photoshop"));
            QVERIFY(again.key_set->findData(QStringLiteral("own:私の組")) < 0);
            QCOMPARE(app::key_sets::own_sets(), QStringList{QStringLiteral("私の組")});  // (deleted by 決める: the review's P3-9)
            again.save();
            QVERIFY(app::key_sets::own_sets().isEmpty());
        }
        QCOMPARE(app::key_sets::chosen(), QStringLiteral("photoshop"));
        QCOMPARE(b.act("act_pen")->shortcut(), key("B"));
    }

    // PC5 H-d (edit.menus): メニュー… hides items and colours them (Photoshop's); a hidden item is out of its menu only while
    // the menu is open — its key works, 検索 finds it, the help names its place —, the menu ends with すべてのメニュー項目を表示;
    // the approval and the AI's connection hidden still ask (the approval's way unchanged).
    void theMenusHideAndColourTheirItems() {
        Book b;
        app::MainWindow* w = b.window.get();
        const QStringList hide{QStringLiteral("色の校正"), QStringLiteral("チャットでの承認…"), QStringLiteral("AI と作る…"), QStringLiteral("レイヤーをロック…")};
        const auto names = app::by_text(w);
        for (const QString& name : hide) QVERIFY2(names.contains(name), qPrintable(name));
        {
            app::ShortcutsDialog dialog(w, 1);
            QCOMPARE(dialog.tabs->currentIndex(), 1);
            QVERIFY(dialog.menu_tree->topLevelItemCount() > 5);
            QVERIFY(!dialog.is_hidden(QStringLiteral("色の校正")));
            for (const QString& name : hide) dialog.set_hidden(name, true);
            dialog.set_colour(QStringLiteral("検索"), QStringLiteral("red"));
            QCOMPARE(dialog.colour_of(QStringLiteral("検索")), QStringLiteral("red"));
            dialog.save();
        }
        for (const QString& name : hide) QVERIFY2(app::menu_look::hidden().contains(name), qPrintable(name));
        QCOMPARE(app::menu_look::coloured().at(QStringLiteral("検索")), QStringLiteral("red"));
        QAction* proof = b.act("act_cmyk_proof");
        QMenu* view = b.menu(QStringLiteral("表示"));
        QVERIFY(view->actions().contains(proof));  // (closed: every item there, for the help and the keys' names)
        view->popup(QPoint(20, 20));
        QVERIFY(QTest::qWaitForWindowExposed(view));
        QVERIFY(!view->actions().contains(proof));
        QCOMPARE(view->actions().back()->text(), QString::fromUtf8(app::menu_look::kShowAll));
        view->hide();
        QVERIFY(view->actions().contains(proof));
        QVERIFY(view->actions().back()->text() != QString::fromUtf8(app::menu_look::kShowAll));
        // すべてのメニュー項目を表示: the menu again with every item
        view->popup(QPoint(20, 20));
        QVERIFY(QTest::qWaitForWindowExposed(view));
        QAction* all = view->actions().back();
        view->hide();
        all->trigger();
        QVERIFY(gui_test::wait_for([view] { return view->isVisible(); }, 3000));
        QVERIFY(view->actions().contains(proof));
        view->hide();
        // its key works, 検索 finds it, its place is still its menu
        QVERIFY(proof->isVisible() && proof->associatedObjects().contains(w));
        const std::vector<QAction*> found = app::search(w, QStringLiteral("色の校正"));
        QVERIFY(std::find(found.begin(), found.end(), proof) != found.end());
        QCOMPARE(app::where(w, proof), QStringLiteral("表示"));
        const bool before = proof->isChecked();
        w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w));
        QTest::keyClick(w->canvas(), Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(proof->isChecked(), !before);
        if (proof->isChecked() != before) proof->trigger();
        // the colour: a band and a tint over 検索 in 編集
        QMenu* edit = b.menu(QStringLiteral("編集"));
        edit->popup(QPoint(20, 20));
        QVERIFY(QTest::qWaitForWindowExposed(edit));
        const auto painted = app::menu_look::painted(edit);
        QCOMPARE(painted.size(), std::size_t{1});
        QCOMPARE(painted.front().first, edit->actionGeometry(b.act("act_find_command")));
        QCOMPARE(painted.front().second, app::menu_look::colour(QStringLiteral("red")));
        const QImage picture = edit->grab().toImage();
        const QRect band = painted.front().first;
        const QColor at = picture.pixelColor(QPoint(band.left() + 1, band.center().y()) * picture.devicePixelRatio());
        QVERIFY2(std::abs(at.red() - 0xe5) <= 8 && std::abs(at.green() - 0x53) <= 8 && std::abs(at.blue() - 0x4b) <= 8, qPrintable(at.name()));
        edit->hide();
        // hidden, the approval and the AI's connection still ask
        b.answers.asked.clear();
        b.act("act_chat_approval")->trigger();
        QVERIFY2(b.answers.asked.join(QStringLiteral("|")).contains(QStringLiteral("チャットでの承認")), qPrintable(b.answers.asked.join(QStringLiteral("|"))));
        b.answers.asked.clear();
        b.act("act_ai")->trigger();
        QVERIFY2(b.answers.asked.join(QStringLiteral("|")).contains(QStringLiteral("AiDialog")), qPrintable(b.answers.asked.join(QStringLiteral("|"))));
        // 初期設定に戻す: every item shown, no colour
        {
            app::ShortcutsDialog dialog(w, 1);
            QVERIFY(dialog.is_hidden(QStringLiteral("色の校正")));
            dialog.reset_menus();
            QVERIFY(!dialog.is_hidden(QStringLiteral("色の校正")));
            dialog.save();
        }
        QVERIFY(app::menu_look::hidden().isEmpty() && app::menu_look::coloured().empty());
        view->popup(QPoint(20, 20));
        QVERIFY(QTest::qWaitForWindowExposed(view));
        QVERIFY(view->actions().contains(proof));
        view->hide();
    }

    // PC5 H-d (edit.toolbar, SPEC VIEW-01「ツールバー」): ツールバー… — the palette in the person's order, the tools away under
    // 余分なツール, kept for the next window; each button with its group (スポイト's ものさし・注釈・カウント — J-d1 —, 手のひらツール's
    // ズームツール); ウィンドウ → ツール still shows and hides it; 初期設定に戻す.
    void theToolbarKeepsItsOrder() {
        Book b;
        app::MainWindow* w = b.window.get();
        const auto tools = [](app::MainWindow* window) {
            QStringList out;
            for (QAction* a : window->tool_palette()->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->property("genko_attribute").toString());
            return out;
        };
        QCOMPARE(tools(w), app::toolbar::default_order());
        QVERIFY(w->extra_tools() == nullptr);
        {
            app::ToolbarDialog dialog(w);
            QCOMPARE(dialog.order(), app::toolbar::default_order());
            dialog.shown->setCurrentRow(1);  // (移動ツール)
            dialog.move(-1);
            dialog.hide_tool(QStringLiteral("act_3d"));
            dialog.hide_tool(QStringLiteral("act_effect"));
            // (J-b's slices among them as Genko makes it)
            QCOMPARE(dialog.hidden(), (QStringList{QStringLiteral("act_slice_tool"), QStringLiteral("act_slice_select_tool"), QStringLiteral("act_3d"), QStringLiteral("act_effect")}));
            dialog.save();
        }
        QCOMPARE(tools(w).mid(0, 2), (QStringList{QStringLiteral("act_move"), QStringLiteral("act_select")}));
        QVERIFY(!tools(w).contains(QStringLiteral("act_3d")) && !tools(w).contains(QStringLiteral("act_effect")));
        QAction* extra = w->extra_tools();
        QVERIFY(extra != nullptr && extra->menu() != nullptr && w->tool_palette()->actions().back() == extra);
        QCOMPARE(extra->menu()->actions().size(), qsizetype{4});  // (the slices' 切り抜き group each, then the two)
        QCOMPARE(extra->menu()->actions().mid(2), (QList<QAction*>{b.act("act_3d"), b.act("act_effect")}));
        QVERIFY(extra->menu()->actions().front()->menu() != nullptr && extra->menu()->actions().front()->menu()->actions().contains(b.act("act_slice_tool")));
        // each button keeps its group
        auto* picker = qobject_cast<QToolButton*>(w->tool_palette()->widgetForAction(b.act("act_picker")));
        QVERIFY(picker != nullptr && picker->menu() != nullptr);
        QStringList group;
        for (QAction* a : picker->menu()->actions()) group << a->property("genko_attribute").toString();
        QCOMPARE(group, (QStringList{QStringLiteral("act_picker"), QStringLiteral("act_measure_ruler_tool"), QStringLiteral("act_note"),
                                     QStringLiteral("act_measure_count_tool")}));
        auto* hand = qobject_cast<QToolButton*>(w->tool_palette()->widgetForAction(b.act("act_hand_tool")));
        QVERIFY(hand != nullptr && hand->menu() != nullptr && hand->menu()->actions().contains(b.act("act_zoom_tool")));
        // ウィンドウ → ツール
        QAction* panel = w->tool_palette()->toggleViewAction();
        panel->trigger();
        QVERIFY(!w->tool_palette()->isVisible());
        panel->trigger();
        QVERIFY(w->tool_palette()->isVisible());
        // the next window: the same order
        {
            Book other;
            QCOMPARE(tools(other.window.get()), tools(w));
        }
        app::ask::set_responder(b.answers.responder);  // (the other window's answers gone with it: this one's again)
        // 初期設定に戻す
        {
            app::ToolbarDialog dialog(w);
            QCOMPARE(dialog.hidden(), (QStringList{QStringLiteral("act_slice_tool"), QStringLiteral("act_slice_select_tool"), QStringLiteral("act_3d"), QStringLiteral("act_effect")}));
            dialog.restore_defaults();
            dialog.save();
        }
        QCOMPARE(tools(w), app::toolbar::default_order());
        QVERIFY(w->extra_tools() == nullptr);
        QVERIFY(!app::settings()->contains(QStringLiteral("toolbar/order")));
        // (編集 → ツールバー… opens it)
        b.answers.asked.clear();
        b.act("act_toolbar")->trigger();
        QCOMPARE(b.answers.asked, QStringList{QStringLiteral("genko::app::ToolbarDialog")});
    }

    // PC5 H-d (edit.preferences.workspace): 環境設定 → ワークスペース — 隠れたパネルを自動的に表示, 大きいタブ, ワークスペースの変更を記憶,
    // ワークスペースをロック; the keys' page gone to キーボードショートカット… (a button there opens it).
    void theWorkspacePreferences() {
        Book b;
        app::MainWindow* w = b.window.get();
        {
            app::preferences::PreferencesDialog dialog(w);
            dialog.show_pane(QStringLiteral("workspace"));
            QCOMPARE(dialog.tabs->tabText(dialog.tabs->currentIndex()), QStringLiteral("ワークスペース"));
            for (int i = 0; i < dialog.tabs->count(); ++i) QVERIFY(dialog.tabs->tabText(i) != QStringLiteral("ショートカット"));
            QCOMPARE(dialog.pages_list->count(), dialog.tabs->count());
            QVERIFY(dialog.workspace_remember->isChecked() && dialog.auto_show_panels->isChecked());
            QVERIFY(!dialog.large_tabs->isChecked() && !dialog.workspace_locked->isChecked());
            b.answers.asked.clear();
            dialog.keys_button->click();
            QCOMPARE(b.answers.asked, QStringList{QStringLiteral("genko::app::ShortcutsDialog")});
            dialog.large_tabs->setChecked(true);
            dialog.auto_show_panels->setChecked(false);
            dialog.workspace_locked->setChecked(true);
            dialog.save();
        }
        QVERIFY(w->doc_tabs()->property("large").toBool());
        QVERIFY(w->doc_tabs()->font().pointSizeF() > QApplication::font().pointSizeF());
        QVERIFY(!w->canvas_only()->auto_show);
        QVERIFY(!b.dock(QStringLiteral("レイヤー"))->features().testFlag(QDockWidget::DockWidgetMovable));
        QVERIFY(b.act("act_lock_workspace")->isChecked());
        // 原稿だけを表示: the window's edge brings no panel now
        w->canvas_only()->toggle();
        w->canvas_only()->watch_at(QPoint(2, w->height() / 2));
        QVERIFY(w->canvas_only()->peek().isEmpty());
        w->canvas_only()->toggle();
        for (const char* key : {"ui/large_tabs", "ui/auto_show_panels", "ui/workspace_locked"}) app::settings()->remove(QString::fromLatin1(key));
        app::preferences::apply_all(w);
        QVERIFY(!w->doc_tabs()->property("large").toBool());
        QVERIFY(w->canvas_only()->auto_show);
        QVERIFY(b.dock(QStringLiteral("レイヤー"))->features().testFlag(QDockWidget::DockWidgetMovable));
        w->canvas_only()->toggle();
        w->canvas_only()->watch_at(QPoint(2, w->height() / 2));
        QCOMPARE(w->canvas_only()->peek(), QStringLiteral("left"));
        w->canvas_only()->toggle();
    }


    // The review of PC5 H-d, P2-1: in an agent's book with a request waiting, choosing a workspace — built in, the person's own,
    // reset, 初期設定 — never hides the approval's panels (承認箱 shown while something waits), nor a window opening in one.
    void theApprovalPanelsStayAsTheyAre() {
        Book b;
        app::MainWindow* w = b.window.get();
        QDockWidget* box = b.dock(QStringLiteral("承認箱"));
        QVERIFY(box != nullptr && !box->isVisible());
        QVERIFY(w->save_workspace(QStringLiteral("ペン入れ")));  // (kept while nothing waited: the box hidden in it)
        service::StudioService ai(gui_test::path_of(b.tmp.path()), "ai:hermes");
        QVERIFY(ai.call("apply_ops", Json::array({"book.genko", Json::array({Json{{"op", "set_studio"}, {"premise", "屋上の話"}}})}), Json{{"commit", true}}).ok);
        QVERIFY(ai.call("request_approval", Json::array(), Json{{"project", "book.genko"}, {"gate", "name"}, {"pages", {1}}}).ok);
        w->session().check_outside();
        QVERIFY(gui_test::wait_for([&] { return box->isVisible(); }, 60000));
        QDockWidget* details = b.dock(QStringLiteral("コマの詳細"));
        const bool details_shown = details->isVisible();
        for (const char* name : {"act_workspace_painting", "act_workspace_photography", "act_workspace_reset", "act_workspace_essentials"}) {
            b.act(name)->trigger();
            QVERIFY2(box->isVisible(), name);
            QVERIFY2(details->isVisible() == details_shown, name);
        }
        app::settings()->remove(QStringLiteral("workspace_changes/ペン入れ"));  // (ペン入れ as kept: the box hidden in it)
        w->choose_workspace(QStringLiteral("ペン入れ"));
        QVERIFY(box->isVisible());
        b.act("act_workspace_reset")->trigger();
        QVERIFY(box->isVisible());
        w->default_layout();
        QVERIFY(box->isVisible());
        w->choose_workspace(QStringLiteral("ペン入れ"));
        app::settings()->remove(QStringLiteral("workspace_changes/ペン入れ"));
        // another window on the book, opening in ペン入れ: the box there too
        {
            app::MainWindow other(app::Session::open(b.path, gui_test::quick(gui_test::path_of(b.tmp.path() + QStringLiteral("/recovery2")))));
            other.resize(1280, 860);
            other.show();
            for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
            QDockWidget* there = nullptr;
            for (QDockWidget* d : other.findChildren<QDockWidget*>()) {
                if (d->objectName() == QStringLiteral("承認箱")) there = d;
            }
            QVERIFY(there != nullptr && there->isVisible());
            other.close();
        }
        b.act("act_workspace_essentials")->trigger();
    }

    // The review's P2-2: a workspace kept before PC5 H-d under a built-in one's name — 写真 — is moved, once, to 「写真（自分）」
    // with its layouts, said; chosen, its own panels come (写真 is Photoshop's).
    void aWorkspaceKeptUnderABuiltInNameIsMoved() {
        {
            Book before;
            before.dock(QStringLiteral("レイヤー"))->hide();
            app::save_workspace(before.window.get(), QStringLiteral("写真"));  // (as a build before H-d kept it: no name refused)
            app::settings()->setValue(QStringLiteral("workspace_changes/写真"), QStringLiteral("x"));
        }
        app::settings()->setValue(QStringLiteral("ui/workspace"), QStringLiteral("写真"));
        Book b;
        QCOMPARE(app::workspaces(), QStringList{QStringLiteral("写真（自分）")});
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace")).toString(), QStringLiteral("写真（自分）"));
        QVERIFY(!app::settings()->contains(QStringLiteral("workspace_changes/写真")));
        QCOMPARE(app::settings()->value(QStringLiteral("workspace_changes/写真（自分）")).toString(), QStringLiteral("x"));
        QVERIFY2(b.window->last_notice().contains(QStringLiteral("「写真（自分）」")), qPrintable(b.window->last_notice()));
        app::settings()->remove(QStringLiteral("workspace_changes/写真（自分）"));  // (its layout as kept, then)
        b.window->choose_workspace(QStringLiteral("写真（自分）"));
        QVERIFY(!b.dock(QStringLiteral("レイヤー"))->isVisible());  // (the person's own panels)
        b.act("act_workspace_photography")->trigger();
        QVERIFY(b.dock(QStringLiteral("ヒストグラム"))->isVisible() && b.dock(QStringLiteral("レイヤー"))->isVisible());  // (Photoshop's)
        QCOMPARE(app::move_reserved_workspaces().size(), std::size_t{0});  // (once)
        b.act("act_workspace_essentials")->trigger();
    }

    // The review's P2-3: a window opening in the person's own workspace takes its panels, never the window's size kept with it
    // (H-c's floating windows and 新規ウィンドウ keep theirs); chosen, the size comes too.
    void aWindowOpeningKeepsItsSize() {
        {
            Book small;
            small.window->resize(900, 600);
            QCoreApplication::processEvents();
            small.dock(QStringLiteral("レイヤー"))->hide();
            QVERIFY(small.window->save_workspace(QStringLiteral("小さい窓")));
        }
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace")).toString(), QStringLiteral("小さい窓"));
        Book b;
        QCOMPARE(b.window->current_workspace(), QStringLiteral("小さい窓"));
        QCOMPARE(b.window->size(), QSize(1280, 860));
        QVERIFY(!b.dock(QStringLiteral("レイヤー"))->isVisible());  // (its panels)
        b.act("act_workspace_essentials")->trigger();
        QVERIFY(b.dock(QStringLiteral("レイヤー"))->isVisible());
    }

    // The review's P3: 組を保存… refuses one key on two commands; 前のコマンドから外して付ける takes that key only (一般…'s Ctrl+, stays);
    // メニュー…'s 決める keeps the items its tree does not show; 概要… never over the book; a workspace's broken keys never empty
    // the person's; a tool moved to 余分なツール goes with its group; the lock reaches a panel made later; 組を削除 given up by
    // やめる; an own set's keys and a hidden item kept under a command's old name.
    void theReviewsSmallerOnes() {
        Book b;
        app::MainWindow* w = b.window.get();
        const auto key = [](const char* words) { return QKeySequence(QString::fromLatin1(words)); };
        {
            app::ShortcutsDialog dialog(w);
            dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(key("Ctrl+K"));  // (一般…'s first: said)
            QVERIFY(!dialog.save_set(QStringLiteral("重なる組")));
            QVERIFY(app::key_sets::own_sets().isEmpty());
            dialog.take_from_previous();
            b.answers.responder->save_path = [this_book = b.path](const QString&, const QString&) {
                return QString::fromStdString(genko::core::path_to_utf8(this_book));
            };
            b.answers.asked.clear();
            QVERIFY(!dialog.write_summary());  // (the book itself: refused, said)
            QVERIFY2(!b.answers.asked.isEmpty(), "said");
            QVERIFY(!dialog.clash->text().contains(QStringLiteral("書き出しました")));
            QVERIFY(gui_test::read_book(b.path).pages.size() == 2);  // (the book as it was)
            dialog.save();
        }
        QCOMPARE(b.act("act_pen")->shortcut(), key("Ctrl+K"));
        QCOMPARE(b.act("act_prefs")->shortcuts(), QList<QKeySequence>{key("Ctrl+,")});  // (its other key kept)
        app::settings()->remove(QStringLiteral("shortcuts"));
        app::settings()->remove(QStringLiteral("key_sets"));
        app::preferences::apply_all(w);
        // メニュー…: a command the tree does not show keeps its setting
        app::menu_look::set_hidden({QStringLiteral("どこにも無い命令")});
        app::menu_look::set_coloured({{QStringLiteral("どこにも無い命令"), QStringLiteral("blue")}});
        {
            app::ShortcutsDialog dialog(w, 1);
            dialog.set_hidden(QStringLiteral("色の校正"), true);
            dialog.save();
        }
        QVERIFY(app::menu_look::hidden().contains(QStringLiteral("どこにも無い命令")) && app::menu_look::hidden().contains(QStringLiteral("色の校正")));
        QCOMPARE(app::menu_look::coloured().at(QStringLiteral("どこにも無い命令")), QStringLiteral("blue"));
        app::menu_look::set_hidden({});
        app::menu_look::set_coloured({});
        // a workspace's keys broken: the person's kept
        app::settings()->setValue(QStringLiteral("shortcuts/移動ツール"), QStringLiteral("Ctrl+Alt+Shift+F2"));
        app::settings()->setValue(QStringLiteral("ui/key_set"), QStringLiteral("genko"));
        app::save_workspace(w, QStringLiteral("壊れた"));
        app::settings()->setValue(QStringLiteral("workspaces/壊れた/keys"), QStringLiteral("{\"keys\": oops"));
        QVERIFY(!app::load_workspace_extras(QStringLiteral("壊れた")));
        QCOMPARE(app::settings()->value(QStringLiteral("shortcuts/移動ツール")).toString(), QStringLiteral("Ctrl+Alt+Shift+F2"));
        QCOMPARE(app::settings()->value(QStringLiteral("ui/key_set")).toString(), QStringLiteral("genko"));
        app::settings()->remove(QStringLiteral("shortcuts"));
        app::settings()->remove(QStringLiteral("ui/key_set"));
        app::preferences::apply_all(w);
        // 余分なツール: スポイト with its group
        app::toolbar::keep(app::toolbar::default_order(), {QStringLiteral("act_picker"), QStringLiteral("act_3d")});
        w->fill_palette();
        QMenu* extra = w->extra_tools()->menu();
        QCOMPARE(extra->actions().size(), qsizetype{4});
        QMenu* group = extra->actions().front()->menu();
        QVERIFY(group != nullptr);
        QStringList tools;
        for (QAction* a : group->actions()) tools << a->property("genko_attribute").toString();
        QCOMPARE(tools, (QStringList{QStringLiteral("act_picker"), QStringLiteral("act_measure_ruler_tool"), QStringLiteral("act_note"),
                                     QStringLiteral("act_measure_count_tool")}));
        QCOMPARE(extra->actions().at(1), b.act("act_3d"));  // (then J-b's slices, away as Genko makes it)
        app::toolbar::keep(app::toolbar::default_order(), {});
        w->fill_palette();
        // the lock over a panel made later
        b.act("act_lock_workspace")->trigger();
        auto* later = new QDockWidget(QStringLiteral("あとから"), w);
        later->setObjectName(QStringLiteral("あとから"));
        later->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        w->addDockWidget(Qt::RightDockWidgetArea, later);
        QVERIFY(gui_test::wait_for([later] { return !later->features().testFlag(QDockWidget::DockWidgetMovable); }, 3000));
        b.act("act_lock_workspace")->trigger();
        QVERIFY(later->features().testFlag(QDockWidget::DockWidgetMovable));
        delete later;
        // 組を削除, then やめる: the set kept
        app::key_sets::save_own(QStringLiteral("残る組"), QStringLiteral("photoshop"), {});
        b.answers.responder->question = [](const QString&, const QString&) { return true; };
        {
            app::ShortcutsDialog dialog(w);
            dialog.key_set->setCurrentIndex(dialog.key_set->findData(QStringLiteral("own:残る組")));
            QVERIFY(dialog.delete_set());
            dialog.reject();
        }
        QCOMPARE(app::key_sets::own_sets(), QStringList{QStringLiteral("残る組")});
        app::key_sets::delete_own(QStringLiteral("残る組"));
        // under a command's old name (renamed()): an own set's key, a hidden item
        const QString old = QStringLiteral("今の配置をワークスペースとして残す…");
        app::key_sets::save_own(QStringLiteral("前の名前の組"), QStringLiteral("photoshop"), {{old, {key("Ctrl+Alt+Shift+F3")}}});
        app::settings()->setValue(QStringLiteral("ui/key_set_own"), QStringLiteral("前の名前の組"));
        app::menu_look::set_hidden({old});
        app::preferences::apply_all(w);
        QCOMPARE(b.act("act_save_workspace")->shortcut(), key("Ctrl+Alt+Shift+F3"));
        {
            app::ShortcutsDialog dialog(w, 1);
            QVERIFY(dialog.is_hidden(QStringLiteral("新規ワークスペース…")));
        }
        app::settings()->remove(QStringLiteral("ui/key_set_own"));
        app::key_sets::delete_own(QStringLiteral("前の名前の組"));
        app::menu_look::set_hidden({});
        app::preferences::apply_all(w);
        QVERIFY(b.act("act_save_workspace")->shortcut().isEmpty());
    }

    // A short window: the palette's pictures shrink (down to 16 px) so the last tool stays within reach; the layers'
    // tab in front at the first show.
    void thePaletteFitsAShortWindowAndTheLayersAreInFront() {
        Book b;
        QDockWidget* layers = b.dock(QStringLiteral("レイヤー"));
        // (in front: in the window — Qt keeps a panel behind another's tab out of it)
        QVERIFY(layers != nullptr && b.window->rect().intersects(QRect(layers->mapTo(b.window.get(), QPoint(0, 0)), layers->size())));
        b.window->resize(1366, 700);  // (Python's test_every_tool_is_within_reach_on_a_short_screen: 1366 × 768, its window 700 high)
        QVERIFY(gui_test::wait_for([&] {
            QToolBar* palette = b.window->tool_palette();
            QAction* last = nullptr;
            for (QAction* a : palette->actions()) {
                if (!a->isSeparator() && a->isVisible()) last = a;
            }
            QWidget* shown = palette->widgetForAction(last);
            return shown != nullptr && shown->isVisibleTo(palette);
        }, 5000));
        QVERIFY(b.window->tool_palette()->iconSize().width() >= 16 && b.window->tool_palette()->iconSize().width() <= 24);
    }
};

QTEST_MAIN(TestGuiWorkspace)
#include "test_gui_workspace.moc"
