// PC5 節 G (文字; type@1, SPEC.md TEXT-02, design section G 5.1): the 書式 menu — Photoshop's, after レイヤー. G-0 makes it
// with the type commands this build has (ワープテキスト…, 作業用パスを作成, シェイプに変換: the same commands as ページ → 台詞);
// the later groups add theirs in Photoshop's order: パネル ▸, アンチエイリアス ▸, 方向 ▸, OpenType ▸ (G-a, G-b), the
// conversions and the commands that rasterize (G-a), フォントプレビューのサイズ ▸, 言語オプション ▸, the fonts' commands, Lorem
// ipsum and 初期設定の書式 (G-a, G-d).

#include <QMenu>

#include "app/main_window.hpp"

namespace genko::app {

void MainWindow::build_type_menu(QMenu* menu) {
    type_menu_ = menu;
    menu->setObjectName(QStringLiteral("menu_type"));
    // (Photoshop's 書式: 作業用パスを作成, シェイプに変換 together, then ワープテキスト…)
    for (const char* name : {"act_line_work_path", "act_line_to_shape"}) {
        if (QAction* act = action(name)) menu->addAction(act);
    }
    menu->addSeparator();
    if (QAction* act = action("act_warp_text")) menu->addAction(act);
}

}  // namespace genko::app
