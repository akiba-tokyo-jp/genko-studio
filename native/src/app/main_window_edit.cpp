// 編集's PC5 part (PC5 H-0, 節 H 5.4; SPEC VIEW-01): after the editing commands, 検索 (Genko's コマンドを探す, moved from
// ヘルプ), キーボードショートカット… and 環境設定 ▸ — one command for each of 環境設定's panes, Photoshop's (一般… Ctrl+K in
// the Photoshop key set; Ctrl+, kept), moved from ファイル. H-f puts 最後の状態を切り替え after やり直し and メモリをクリア ▸
// before 検索; H-d puts メニュー… and ツールバー… after キーボードショートカット… (PC5 H-d: キーボードショートカット… and メニュー… are one
// window's two tabs — app/shortcuts_dialog —, ツールバー… app/toolbar_dialog); H-e makes the panes' pages (preferences::panes():
// a pane without its page is not in the menu yet).

#include <QMenu>

#include "app/main_window.hpp"
#include "app/preferences.hpp"

namespace genko::app {

void MainWindow::build_edit_actions() {
    // (Photoshop's 一般…; Genko's 環境設定… — its id kept, the old name in preferences::renamed())
    make("act_prefs", QStringLiteral("一般…"), [this] { open_preferences(QStringLiteral("general")); }, {QKeySequence(QStringLiteral("Ctrl+,"))},
         QStringLiteral("画面・道具・ファイル・書き出しなど、このパソコンでの Genko の使い心地を決めます"));
    for (const preferences::Pane& pane : preferences::panes()) {
        if (pane.id == QLatin1String("general") || pane.page.isEmpty()) continue;
        const QString id = pane.id;
        make(QStringLiteral("act_prefs_%1").arg(id), pane.label, [this, id] { open_preferences(id); });
    }
    make("act_keyboard_shortcuts", QStringLiteral("キーボードショートカット…"), [this] { open_keyboard_shortcuts(0); }, {},
         QStringLiteral("コマンドのキーを変えます。キーの組（Photoshop・Genko・自分の組）もここで選びます"));
    // (PC5 H-d: Photoshop's メニュー… Ctrl+Alt+Shift+M in its key set — key_sets::photoshop_keys —, ツールバー…)
    make("act_menus", QStringLiteral("メニュー…"), [this] { open_keyboard_shortcuts(1); }, {},
         QStringLiteral("メニューの項目を隠したり、色を付けたりします（隠した項目もキーと検索で使えます）"));
    make("act_toolbar", QStringLiteral("ツールバー…"), [this] { open_toolbar_dialog(); }, {},
         QStringLiteral("左のツールの並びを変え、使わないツールを「余分なツール」へ移します"));
    // (H-f: 最後の状態を切り替え, メモリをクリア ▸)
}

void MainWindow::build_edit_menu_tail(QMenu* edit) {
    edit->addSeparator();
    edit->addAction(action("act_find_command"));  // (検索)
    // (PC5 G-d: Photoshop's スペルチェック… and 検索と置換… after 検索)
    edit->addAction(action("act_spelling"));
    edit->addAction(action("act_replace"));
    edit->addSeparator();
    edit->addAction(action("act_keyboard_shortcuts"));
    edit->addAction(action("act_menus"));    // (PC5 H-d)
    edit->addAction(action("act_toolbar"));  // (PC5 H-d)
    QMenu* panes = edit->addMenu(QStringLiteral("環境設定"));
    panes->setObjectName(QStringLiteral("menu_preferences"));
    for (const preferences::Pane& pane : preferences::panes()) {
        if (pane.page.isEmpty()) continue;
        panes->addAction(action(pane.id == QLatin1String("general") ? QStringLiteral("act_prefs") : QStringLiteral("act_prefs_%1").arg(pane.id)));
    }
}

}  // namespace genko::app
