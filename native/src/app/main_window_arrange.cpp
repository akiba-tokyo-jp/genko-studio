// ウィンドウ (PC5 H-0, 節 H 5.2; SPEC VIEW-01): アレンジ ▸ and ワークスペース ▸ at the top as Photoshop's, then the books
// open, the bars made one's own and the panels. H-0 makes the places; H-c (窓の区画・浮動・同じ表示にする) makes its
// commands in build_arrange_actions and puts them in アレンジ ▸ in build_arrange_menus — after 新規ウィンドウ, Photoshop's
// last, which H-0 moves there (今の「新しいウィンドウ（同じ原稿）」). H-d fills ワークスペース ▸ (fill_workspaces).

#include <QDockWidget>
#include <QMenu>

#include "app/main_window.hpp"

namespace genko::app {

void MainWindow::build_arrange_actions() {
    // 「<名前>」の新規ウィンドウ (window.arrange.newWindowForDocument: this book in one more window — its id kept, the old name
    // in preferences::renamed()). Its words stay the same for every book: a command is kept — its keys, its place in the
    // quick access — by its words.
    make("act_new_window", QStringLiteral("新規ウィンドウ"), [this] { new_window(); }, {},
         QStringLiteral("この原稿をもう 1 つのウィンドウで開きます。拡大して描きながら、別の窓で全体を見る"));
    // (H-c: すべてを垂直方向に並べる … 6 アップ, すべてをタブに統合, 重ねて表示, 並べて表示, ウィンドウで浮動, すべてをウィンドウで浮動,
    // 同じズーム倍率で表示, 同じ位置に表示, 同じ回転で表示, すべてを同じ表示にする)
}

void MainWindow::build_arrange_menus(QMenu* arrange) {
    arrange->setObjectName(QStringLiteral("menu_arrange"));
    // (H-c: its commands before 新規ウィンドウ, in Photoshop's order — 節 H 5.2)
    arrange->addAction(action("act_new_window"));
}

void MainWindow::name_panels() {
    // (節 H 1.2: Photoshop's names — the panels' switches in ウィンドウ follow; renamed() reads the old ones)
    for (const auto& [object, title] : {std::pair{"全体図", "ナビゲーター"}, {"ツールの設定", "オプション"}, {"履歴", "ヒストリー"}}) {
        for (QDockWidget* dock : findChildren<QDockWidget*>(QString::fromUtf8(object))) dock->setWindowTitle(QString::fromUtf8(title));
    }
}

}  // namespace genko::app
