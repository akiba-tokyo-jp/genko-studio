#include "app/key_sets.hpp"

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QVariant>

#include <algorithm>

#include "app/config.hpp"
#include "app/workspace.hpp"

namespace genko::app::key_sets {

namespace {

const char* const kGenkoKeys = "genko_keys";  // (the property a command's Genko keys are kept in)

QList<QKeySequence> k(std::initializer_list<const char*> keys) {
    QList<QKeySequence> out;
    for (const char* key : keys) out << QKeySequence(QString::fromLatin1(key));
    return out;
}

}  // namespace

const std::vector<Set>& sets() {
    static const std::vector<Set> all{{QStringLiteral("photoshop"), QStringLiteral("Photoshop の初期設定")},
                                      {QStringLiteral("genko"), QStringLiteral("Genko（PC5 より前のキー）")}};
    return all;
}

QString current() {
    const QString value = settings()->value(QStringLiteral("ui/key_set"), sets().front().id).toString();
    return std::any_of(sets().begin(), sets().end(), [&value](const Set& s) { return s.id == value; }) ? value : sets().front().id;
}

void set_current(const QString& id) { settings()->setValue(QStringLiteral("ui/key_set"), id); }

const std::vector<std::pair<QString, QList<QKeySequence>>>& photoshop_keys() {
    // (Photoshop 2025's keys for Windows, as PhotoCraft v0.2.0's menu_catalog.rs has them (MIT) and Photoshop's own list;
    // a Genko command whose key Photoshop gives another command has none here, or a free key near it)
    static const std::vector<std::pair<QString, QList<QKeySequence>>> all{
        // ツール: Photoshop's letters — 移動 V, クイックマスク Q, 切り抜き C, スポット修復ブラシ J, ヒストリーブラシ Y, グラデーション G,
        // 長方形 U (Shift+U the next shape), 覆い焼き O; Genko's コマ割り takes K (Photoshop's フレームツール). The Genko tools
        // whose letter Photoshop gives another have none (選択 V, 効果線 K, 3D 操作 J, 線の修正 Y, 塗りつぶし G, カラーピッカー C)
        {QStringLiteral("act_move"), k({"V"})},
        {QStringLiteral("act_select"), {}},
        {QStringLiteral("act_quick_mask"), k({"Q"})},
        {QStringLiteral("act_crop_tool"), k({"C"})},
        {QStringLiteral("act_color"), {}},
        {QStringLiteral("act_spot_healing"), k({"J"})},
        {QStringLiteral("act_3d"), {}},
        {QStringLiteral("act_history_brush"), k({"Y"})},
        {QStringLiteral("act_reshape"), {}},
        {QStringLiteral("act_gradient"), k({"G"})},
        {QStringLiteral("act_fill"), {}},
        {QStringLiteral("act_shape"), k({"U"})},
        {QStringLiteral("act_next_shape_tool"), k({"Shift+U"})},
        {QStringLiteral("act_dodge_tool"), k({"O"})},
        {QStringLiteral("act_frame"), k({"K"})},
        {QStringLiteral("act_effect"), {}},
        // 表示: 水平方向に反転 has none (H is 手のひらツール's — H-a); プリントサイズ none (Ctrl+1 is 100% — H-a); 仕上がり線・基本枠を表示
        // none (Ctrl+; is ガイド — H-b); 色の校正 Ctrl+Y; ターゲットパス Ctrl+Shift+H; the drawing rulers' switches none (Ctrl+Shift+R
        // is レンズ補正…, Ctrl+2 the composite channel)
        {QStringLiteral("act_mirror"), {}},
        {QStringLiteral("act_actual"), {}},
        {QStringLiteral("act_guides"), {}},
        {QStringLiteral("act_cmyk_proof"), k({"Ctrl+Y"})},
        {QStringLiteral("act_show_target_path"), k({"Ctrl+Shift+H"})},
        {QStringLiteral("act_show_rulers"), {}},
        {QStringLiteral("act_snap"), {}},
        // 編集: やり直し Ctrl+Shift+Z (Ctrl+Y is 色の校正), ヒストリー… none (Ctrl+H is エクストラ — H-b), 検索 Ctrl+F, フェード…
        // Ctrl+Shift+F, 同じ位置にペースト Ctrl+Shift+V, 再実行 Ctrl+Shift+T, 一般… Ctrl+K (Ctrl+, kept), キーボードショートカット…
        // Ctrl+Alt+Shift+K
        {QStringLiteral("act_redo"), k({"Ctrl+Shift+Z"})},
        {QStringLiteral("act_history"), {}},
        {QStringLiteral("act_find_command"), k({"Ctrl+F"})},
        {QStringLiteral("act_fade"), k({"Ctrl+Shift+F"})},
        {QStringLiteral("act_paste_in_place"), k({"Ctrl+Shift+V"})},
        {QStringLiteral("act_transform_again"), k({"Ctrl+Shift+T"})},
        {QStringLiteral("act_prefs"), k({"Ctrl+K", "Ctrl+,"})},
        {QStringLiteral("act_keyboard_shortcuts"), k({"Ctrl+Alt+Shift+K"})},
        // ファイル: 原稿を書き出す… none (Ctrl+E is レイヤーを結合), 画像を読み込む… none (Ctrl+Shift+I is 選択範囲を反転)
        {QStringLiteral("act_export"), {}},
        {QStringLiteral("act_import"), {}},
        // イメージ (Genko's ページ): 画像解像度… Ctrl+Alt+I, 自動トーン補正 Ctrl+Shift+L, 自動コントラスト Ctrl+Alt+Shift+L, 自動カラー補正
        // Ctrl+Shift+B, 白黒… Ctrl+Alt+Shift+B; ストーリーエディター… and 本の形でプレビュー… none
        {QStringLiteral("act_image_size"), k({"Ctrl+Alt+I"})},
        {QStringLiteral("act_auto_tone"), k({"Ctrl+Shift+L"})},
        {QStringLiteral("act_auto_contrast"), k({"Ctrl+Alt+Shift+L"})},
        {QStringLiteral("act_auto_color"), k({"Ctrl+Shift+B"})},
        {QStringLiteral("act_adjust_black_white"), k({"Ctrl+Alt+Shift+B"})},
        {QStringLiteral("act_story_editor"), {}},
        {QStringLiteral("act_book_preview"), {}},
        // レイヤー: 下のレイヤーと結合 Ctrl+E, PNG としてクイック書き出し Ctrl+Shift+', 書き出し形式… Ctrl+Alt+Shift+'
        {QStringLiteral("act_layer_merge"), k({"Ctrl+E"})},
        {QStringLiteral("act_layer_quick_export_png"), k({"Ctrl+Shift+'"})},
        {QStringLiteral("act_layer_export_as"), k({"Ctrl+Alt+Shift+'"})},
        // 選択範囲: 選択範囲を反転 Ctrl+Shift+I
        {QStringLiteral("act_sel_invert"), k({"Ctrl+Shift+I"})},
        // フィルター: フィルターの再実行 Ctrl+Alt+F, レンズ補正… Ctrl+Shift+R; 台詞の検索・置換… none
        {QStringLiteral("act_filter_last"), k({"Ctrl+Alt+F"})},
        {QStringLiteral("act_filter_lens_correction"), k({"Ctrl+Shift+R"})},
        {QStringLiteral("act_replace"), {}},
        // ページ → コマ: コマを横に割る・縦に割る none (Ctrl+Shift+H, Ctrl+Shift+V are ターゲットパス, 同じ位置にペースト);
        // ツール → トーン: トーンを貼る none (Ctrl+Shift+T is 再実行)
        {QStringLiteral("act_split_h"), {}},
        {QStringLiteral("act_split_v"), {}},
        {QStringLiteral("act_tone_here"), {}},
        // チャンネル: Ctrl+2 the composite, Ctrl+3… each channel
        {QStringLiteral("act_channel_composite"), k({"Ctrl+2"})},
        {QStringLiteral("act_channel_3"), k({"Ctrl+3"})},
        {QStringLiteral("act_channel_4"), k({"Ctrl+4"})},
        {QStringLiteral("act_channel_5"), k({"Ctrl+5"})},
        {QStringLiteral("act_channel_6"), k({"Ctrl+6"})},
        {QStringLiteral("act_channel_7"), k({"Ctrl+7"})},
        {QStringLiteral("act_channel_8"), k({"Ctrl+8"})},
        {QStringLiteral("act_channel_9"), k({"Ctrl+9"})},
        // ウィンドウ: レイヤー F7 (a panel's switch by its panel's object name)
        {QStringLiteral("dock:レイヤー"), k({"F7"})},
    };
    return all;
}

QList<QKeySequence> keys_of(const QAction* action, const QString& set) {
    const QVariant kept = action->property(kGenkoKeys);
    const QList<QKeySequence> genko = kept.isValid() ? kept.value<QList<QKeySequence>>() : action->shortcuts();
    if (set != QLatin1String("photoshop")) return genko;
    QString name = action->property("genko_attribute").toString();
    if (const auto* dock = qobject_cast<const QDockWidget*>(action->parent()); name.isEmpty() && dock != nullptr) {
        name = QStringLiteral("dock:") + dock->objectName();
    }
    if (name.isEmpty()) return genko;
    for (const auto& [row, keys] : photoshop_keys()) {
        if (row == name) return keys;
    }
    return genko;
}

void apply(QMainWindow* window, const QString& set) {
    for (QAction* action : window->findChildren<QAction*>()) {
        if (action->text().isEmpty() || action->isSeparator() || action->menu() != nullptr || remade(action)) continue;
        if (!action->property(kGenkoKeys).isValid()) action->setProperty(kGenkoKeys, QVariant::fromValue(action->shortcuts()));
        const QList<QKeySequence> keys = keys_of(action, set);
        if (keys != action->shortcuts()) action->setShortcuts(keys);
    }
}

}  // namespace genko::app::key_sets
