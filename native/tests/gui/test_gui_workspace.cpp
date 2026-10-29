// The screen made one's own and the window's remaining commands (M6, Python's genko/app/workspace.py and main.py) on the
// offscreen platform: コマンドを探す (every menu command once, found by words in any order, done with Enter), the
// quick-access panel and the command bar chosen (kept in the settings), workspaces (the panels kept under a name, put
// back, はじめの配置に戻す), 道具の名前を表示 and パネルの説明を表示, Python's menus (ファイル → AI と作る…, ヘルプ, the
// layer panel's menus in the menu bar with their keys, 3D's scenes and poses, このコマの詳細を見る), the first steps over
// an empty book and what comes next after the first panels (I11), the selection launcher, the tool palette on a short
// window, the layers in front at the first show, and every menu item's place as where() finds it.

#include <QtTest>

#include <QAction>
#include <QDockWidget>
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
#include "gui_support.hpp"

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
        for (const char* key : {"ui/quick", "ui/commandbar", "ui/tool_names", "ui/next_steps", "workspaces", "ui/workspace"}) store->remove(QString::fromLatin1(key));
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
        QVERIFY(seen.contains(QStringLiteral("レイヤーをグループ化")));  // (in レイヤー → まとめて)
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

    // Workspaces: the panels kept under a name (asked for), listed in ウィンドウ → ワークスペース, put back, deleted;
    // はじめの配置に戻す: the panels as the window first showed them.
    void workspacesKeepThePanels() {
        Book b;
        QDockWidget* layers = b.dock(QStringLiteral("レイヤー"));
        QVERIFY(layers != nullptr && layers->isVisible());
        b.answers.responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("ワークスペース") ? std::optional<QString>(QStringLiteral(" ペン入れ ")) : std::nullopt;
        };
        b.act("act_save_workspace")->trigger();
        QCOMPARE(app::workspaces(), QStringList{QStringLiteral("ペン入れ")});
        QVERIFY(b.window->last_notice().contains(QStringLiteral("ワークスペース「ペン入れ」を残しました")));
        layers->hide();
        QVERIFY(!layers->isVisible());
        QVERIFY(app::load_workspace(b.window.get(), QStringLiteral("ペン入れ")));
        QVERIFY(layers->isVisible());
        QCOMPARE(app::settings()->value(QStringLiteral("ui/workspace")).toString(), QStringLiteral("ペン入れ"));
        // the menu: 残す…, はじめの配置に戻す, the names, 消す
        QMenu* window_menu = b.menu(QStringLiteral("ウィンドウ"));
        QMenu* spaces = submenu(window_menu, QStringLiteral("ワークスペース"));
        QVERIFY(spaces != nullptr);
        emit spaces->aboutToShow();
        const QStringList shown = texts(spaces);
        QCOMPARE(shown.mid(0, 4), (QStringList{QStringLiteral("今の配置をワークスペースとして残す…"), QStringLiteral("はじめの配置に戻す"), QStringLiteral("|"),
                                               QStringLiteral("ペン入れ")}));
        QMenu* remove = submenu(spaces, QStringLiteral("消す"));
        QVERIFY(remove != nullptr);
        remove->actions().front()->trigger();
        QVERIFY(app::workspaces().isEmpty());
        // はじめの配置に戻す
        layers->hide();
        b.window->default_layout();
        QVERIFY(layers->isVisible());
        // no name: nothing kept
        b.answers.responder->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(QStringLiteral("  ")); };
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
        QVERIFY(texts(submenu(layers, QStringLiteral("まとめて（複数のレイヤー・表示レイヤー・変換）"))).contains(QStringLiteral("レイヤーをグループ化")));
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
            app::preferences::PreferencesDialog dialog(b.window.get());
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
