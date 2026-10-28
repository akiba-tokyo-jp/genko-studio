// 表示 (PC5 H-0, 節 H 5.1; SPEC VIEW-01): the view menu in Photoshop's order, Genko's own commands after a line. H-0
// lays it out and names the places; H-a (画素の表示・スクリーンモード・32 bit プレビュー) and H-b (ガイド・エクストラ・表示・非表示 ▸・
// 単位) make their commands in build_view_actions and put them in their places in kViewRows (a row naming a command not
// made yet is passed over, so a place may wait for its part). Commands the rows do not name stay after them (a later
// part's own, until it joins the rows).

#include <QMenu>

#include "app/main_window.hpp"
#include "app/workspace.hpp"

namespace genko::app {

namespace {

// (rows: a command's attribute, "▸<title>" a menu inside, "|" a line — lay_out_menu)
const QStringList& view_rows() {
    static const QStringList rows{
        // 校正設定 ▸・色の校正・色域外警告 (PC1b)
        QStringLiteral("▸校正の設定"), QStringLiteral("act_cmyk_proof"), QStringLiteral("act_gamut_warning"),
        QStringLiteral("|"),
        // ピクセル縦横比 ▸・ピクセル縦横比を補正 (H-f, the user's decision 10), 32 bit プレビューオプション… (H-a)
        QStringLiteral("▸ピクセル縦横比"), QStringLiteral("act_pixel_aspect_correction"), QStringLiteral("act_32bit_preview"),
        QStringLiteral("|"),
        // ズームイン・ズームアウト・画面サイズに合わせる・アートボードを画面サイズに合わせる (I)・レイヤーを画面サイズに合わせる (H-a)・
        // 100%・200% (H-a)・プリントサイズ
        QStringLiteral("act_zoom_in"), QStringLiteral("act_zoom_out"), QStringLiteral("act_fit"), QStringLiteral("act_fit_artboard"),
        QStringLiteral("act_fit_layers"), QStringLiteral("act_actual_pixels"), QStringLiteral("act_200_percent"), QStringLiteral("act_actual"),
        QStringLiteral("|"),
        // 水平方向に反転・パターンプレビュー (PC4 E3)
        QStringLiteral("act_mirror"), QStringLiteral("act_pattern_preview"),
        QStringLiteral("|"),
        // スクリーンモード ▸ (H-a)
        QStringLiteral("▸スクリーンモード"),
        QStringLiteral("|"),
        // エクストラ・表示・非表示 ▸ (H-b: ターゲットパス, ブラシプレビュー and the rest go in it there)
        QStringLiteral("act_extras"), QStringLiteral("▸表示・非表示"), QStringLiteral("act_show_target_path"), QStringLiteral("act_brush_preview"),
        QStringLiteral("|"),
        // 定規 (the user's decision 12: Genko's 目盛り)
        QStringLiteral("act_scale"),
        QStringLiteral("|"),
        // スナップ・スナップ先 ▸ (PC3 C-d)
        QStringLiteral("act_snap_on"), QStringLiteral("▸スナップ先"),
        QStringLiteral("|"),
        // ガイドをロック・ガイドを消去・選択したアートボードのガイドを消去 (I)・カンバスガイドを消去・新規ガイド…・新規ガイドレイアウトを作成…
        // (H-b)・シェイプから新規ガイドを作成 (PC3 C-b)
        QStringLiteral("act_lock_guides"), QStringLiteral("act_clear_guides"), QStringLiteral("act_clear_artboard_guides"),
        QStringLiteral("act_clear_canvas_guides"), QStringLiteral("act_new_guide"), QStringLiteral("act_new_guide_layout"),
        QStringLiteral("act_guides_from_shape"),
        QStringLiteral("|"),
        // Genko's own
        QStringLiteral("act_view_flip_v"), QStringLiteral("act_turn_left"), QStringLiteral("act_turn_right"), QStringLiteral("act_turn_reset"),
        QStringLiteral("act_zoom_value"), QStringLiteral("act_zoom_tool"),
        QStringLiteral("|"),
        QStringLiteral("act_overview"), QStringLiteral("act_prev"), QStringLiteral("act_next"),
        QStringLiteral("|"),
        QStringLiteral("act_guides"), QStringLiteral("act_phone"), QStringLiteral("act_sel_edges"), QStringLiteral("act_onion"),
        QStringLiteral("act_screen_dots"),
        QStringLiteral("|"),
        QStringLiteral("act_tool_names")};
    return rows;
}

}  // namespace

void MainWindow::build_view_actions() {
    // (H-a: 100%, 200%, レイヤーを画面サイズに合わせる, スクリーンモード ▸, 32 bit プレビューオプション…, ピクセルグリッド; H-b:
    // エクストラ, 表示・非表示 ▸, ガイドをロック, ガイドを消去, カンバスガイドを消去, 新規ガイド…, 新規ガイドレイアウトを作成…; H-f:
    // ピクセル縦横比 ▸ — each part's commands made here, their Photoshop keys in key_sets::photoshop_keys)
}

void MainWindow::build_view_menus(QMenu* view) {
    // (after every part has put its commands in 表示: laid out once)
    lay_out_menu(view, view_rows());
}

}  // namespace genko::app
