#include "app/key_sets.hpp"

#include <QAction>
#include <QDockWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVariant>

#include <algorithm>

#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
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
        // whose letter Photoshop gives another take a Shift+letter Photoshop leaves free (the user's decision 2026-10-29:
        // photoshop_free_for_genko): 選択 Shift+N, 効果線 Shift+K, 3D 操作 Shift+D, 線の修正 Shift+R, 塗りつぶし Shift+Q;
        // カラーピッカー（描画色）…, a command, none (Photoshop's has none)
        {QStringLiteral("act_move"), k({"V"})},
        {QStringLiteral("act_select"), k({"Shift+N"})},
        {QStringLiteral("act_quick_mask"), k({"Q"})},
        {QStringLiteral("act_crop_tool"), k({"C"})},
        {QStringLiteral("act_color"), {}},
        {QStringLiteral("act_spot_healing"), k({"J"})},
        {QStringLiteral("act_3d"), k({"Shift+D"})},
        {QStringLiteral("act_history_brush"), k({"Y"})},
        {QStringLiteral("act_reshape"), k({"Shift+R"})},
        {QStringLiteral("act_gradient"), k({"G"})},
        {QStringLiteral("act_fill"), k({"Shift+Q"})},
        {QStringLiteral("act_shape"), k({"U"})},
        {QStringLiteral("act_next_shape_tool"), k({"Shift+U"})},
        {QStringLiteral("act_dodge_tool"), k({"O"})},
        {QStringLiteral("act_frame"), k({"K"})},
        {QStringLiteral("act_effect"), k({"Shift+K"})},
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
        // (PC5 H-a) 100% Ctrl+1, 手のひらツール H, スクリーンモード F (going round them)
        {QStringLiteral("act_actual_pixels"), k({"Ctrl+1"})},
        {QStringLiteral("act_hand_tool"), k({"H"})},
        {QStringLiteral("act_screen_next"), k({"F"})},
        // (PC5 H-b) エクストラ Ctrl+H, ガイド Ctrl+;, ガイドをロック Ctrl+Alt+;
        {QStringLiteral("act_extras"), k({"Ctrl+H"})},
        {QStringLiteral("act_show_guides"), k({"Ctrl+;"})},
        {QStringLiteral("act_lock_guides"), k({"Ctrl+Alt+;"})},
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
        // (PC5 H-d) メニュー… Ctrl+Alt+Shift+M
        {QStringLiteral("act_menus"), k({"Ctrl+Alt+Shift+M"})},
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
        // (PC5 I-a) クリッピングマスクを作成 Ctrl+Alt+G, レイヤーのグループ解除 Ctrl+Shift+G (Genko's set the same: free there)
        {QStringLiteral("act_clip_create"), k({"Ctrl+Alt+G"})},
        {QStringLiteral("act_layer_ungroup"), k({"Ctrl+Shift+G"})},
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
        // (the user's decision 13, 2026-10-29 — PC5 H-d: Photoshop's Shift+Ctrl+M is 計測値を記録's, J-d2: コマを結合 takes a
        // modifier more, as the Genko-only tools do — Ctrl+Alt+M, none of Photoshop's —, Ctrl+Shift+M left free)
        {QStringLiteral("act_merge"), k({"Ctrl+Alt+M"})},
        // (PC5 J-d2, the user's decision 13) 計測値を記録 Shift+Ctrl+M (Genko's set: none — Ctrl+Shift+M is コマを結合's there)
        {QStringLiteral("act_measure_record"), k({"Ctrl+Shift+M"})},
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

const QStringList& photoshop_defaults() {
    // (Photoshop 2025 for Windows: the tools' letters, the Shift+letter that goes round a tool's group, F's screen modes,
    // and the menu commands' keys — PhotoCraft v0.2.0's menu_catalog.rs, MIT, and Photoshop's own list)
    static const QStringList all = [] {
        QStringList out;
        for (const char c : QByteArrayLiteral("ABCDEFGHIJKLMOPQRSTUVWXYZ")) out << QString(QLatin1Char(c));  // (N is free)
        for (const char c : QByteArrayLiteral("ABCEFGIJLMOPSTUVWY")) out << QStringLiteral("Shift+%1").arg(QLatin1Char(c));
        out << QStringLiteral("[") << QStringLiteral("]") << QStringLiteral("Shift+[") << QStringLiteral("Shift+]") << QStringLiteral("Tab")
            << QStringLiteral("Shift+Tab") << QStringLiteral("F1") << QStringLiteral("F5") << QStringLiteral("F6") << QStringLiteral("F7")
            << QStringLiteral("F8") << QStringLiteral("Alt+F9") << QStringLiteral("F12") << QStringLiteral("Shift+F5") << QStringLiteral("Shift+F6");
        for (const char* key : {"Ctrl+N", "Ctrl+O", "Ctrl+Alt+Shift+O", "Ctrl+W", "Ctrl+Alt+W", "Ctrl+Alt+P", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+Alt+S",
                                "Ctrl+Alt+Shift+W", "Ctrl+Alt+Shift+S", "Ctrl+Alt+Shift+I", "Ctrl+P", "Ctrl+Alt+Shift+P", "Ctrl+Q", "Ctrl+Z",
                                "Ctrl+Shift+Z", "Ctrl+Alt+Z", "Ctrl+Shift+F", "Ctrl+X", "Ctrl+C", "Ctrl+Shift+C", "Ctrl+V", "Ctrl+Shift+V",
                                "Ctrl+Alt+Shift+V", "Ctrl+F", "Ctrl+Alt+Shift+C", "Ctrl+T", "Ctrl+Shift+T", "Ctrl+Shift+K", "Ctrl+Alt+Shift+K",
                                "Ctrl+Alt+Shift+M", "Ctrl+Shift+M", "Ctrl+K", "Ctrl+L", "Ctrl+M", "Ctrl+U", "Ctrl+B", "Ctrl+Alt+Shift+B", "Ctrl+I", "Ctrl+Shift+U",
                                "Ctrl+Shift+L", "Ctrl+Alt+Shift+L", "Ctrl+Shift+B", "Ctrl+Alt+I", "Ctrl+Alt+C", "Ctrl+Shift+N", "Ctrl+J",
                                "Ctrl+Shift+J", "Ctrl+Shift+'", "Ctrl+Alt+Shift+'", "Ctrl+G", "Ctrl+Shift+G", "Ctrl+Alt+G", "Ctrl+Shift+]", "Ctrl+]",
                                "Ctrl+[", "Ctrl+Shift+[", "Ctrl+E", "Ctrl+Shift+E", "Ctrl+Alt+Shift+E", "Ctrl+A", "Ctrl+D", "Ctrl+Shift+D",
                                "Ctrl+Shift+I", "Ctrl+Alt+A", "Ctrl+Alt+Shift+F", "Ctrl+Alt+R", "Ctrl+Alt+F", "Ctrl+Shift+A", "Ctrl+Alt+Shift+A",
                                "Ctrl+Shift+R", "Ctrl+Shift+X", "Ctrl+Alt+V", "Ctrl+Y", "Ctrl+Shift+Y", "Ctrl++", "Ctrl+=", "Ctrl+-", "Ctrl+0",
                                "Ctrl+1", "Ctrl+Alt+0", "Ctrl+H", "Ctrl+Shift+H", "Ctrl+'", "Ctrl+;", "Ctrl+R", "Ctrl+Shift+;", "Ctrl+Alt+;",
                                "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6", "Ctrl+7", "Ctrl+8", "Ctrl+9", "Ctrl+Alt+2"}) {
            out << QString::fromLatin1(key);
        }
        return out;
    }();
    return all;
}

const std::vector<QString>& genko_tools() {
    static const std::vector<QString> all{QStringLiteral("act_select"), QStringLiteral("act_effect"), QStringLiteral("act_3d"), QStringLiteral("act_reshape"),
                                          QStringLiteral("act_fill")};
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

bool from_before_pc5() {
    const auto store = settings();
    const QString told = QStringLiteral("ui/key_set_told");
    const bool before = !store->allKeys().isEmpty() && !store->contains(QStringLiteral("ui/key_set")) && !store->contains(told);
    if (!store->contains(told)) store->setValue(told, QStringLiteral("1"));  // (once: a new person, too, is never told later)
    return before;
}

void tell_new_default(MainWindow* window) {
    window->flash(QStringLiteral("キーを Photoshop と同じ組にしました。前の Genko のキーは、下の「Genko の組に戻す」か 編集 → キーボードショートカット… の「キーの組」で戻せます"),
                  20000);
    auto* back = new QPushButton(QStringLiteral("Genko の組に戻す"));
    back->setObjectName(QStringLiteral("key_set_back"));
    back->setToolTip(QStringLiteral("PC5 より前の Genko のキー（V で選択、F でコマ割り、H で左右反転 など）に戻します"));
    window->statusBar()->addPermanentWidget(back);
    QObject::connect(back, &QPushButton::clicked, window, [window, back] {
        set_current(QStringLiteral("genko"));
        for (MainWindow* open : documents::windows()) preferences::apply_all(open);
        window->flash(QStringLiteral("前の Genko のキーに戻しました（編集 → キーボードショートカット… で選び直せます）"), 6000);
        back->hide();
        back->deleteLater();
    });
    QTimer::singleShot(5 * 60 * 1000, back, &QObject::deleteLater);  // (not for ever: the menu's way stays)
}

namespace {

// An own set's keys of a command: under its name, or an old name of it (renamed() — the review of PC5 H-d, P3-10).
const QList<QKeySequence>* own_keys_of(const std::map<QString, QList<QKeySequence>>& over, const QString& name) {
    if (const auto found = over.find(name); found != over.end()) return &found->second;
    for (const QString& old : preferences::old_names(name)) {
        if (const auto found = over.find(old); found != over.end()) return &found->second;
    }
    return nullptr;
}

}  // namespace

void apply(QMainWindow* window, const QString& set) {
    const bool own = set.startsWith(QStringLiteral("own:"));
    const QString base = own ? own_base(set.mid(4)) : set;
    std::map<QString, QList<QKeySequence>> over;
    std::map<const QAction*, QString> names;
    if (own) {
        over = own_keys(set.mid(4));
        names = command_names(window);
    }
    for (QAction* action : window->findChildren<QAction*>()) {
        if (action->text().isEmpty() || action->isSeparator() || action->menu() != nullptr || remade(action)) continue;
        if (!action->property(kGenkoKeys).isValid()) action->setProperty(kGenkoKeys, QVariant::fromValue(action->shortcuts()));
        QList<QKeySequence> keys = keys_of(action, base);
        if (own && action->objectName().startsWith(QStringLiteral("cmd:"))) {
            if (const QList<QKeySequence>* found = own_keys_of(over, command_name(names, action))) keys = *found;
        }
        if (keys != action->shortcuts()) action->setShortcuts(keys);
    }
}

// --- the person's own sets (PC5 H-d) --------------------------------------------------------------------------------------

namespace {

const QString kOwn = QStringLiteral("key_sets");

}  // namespace

QStringList own_sets() {
    auto store = settings();
    store->beginGroup(kOwn);
    QStringList names = store->childGroups();
    store->endGroup();
    names.sort();
    return names;
}

bool reserved(const QString& name) {
    const QString trimmed = name.trimmed();
    for (const Set& one : sets()) {
        if (trimmed.compare(one.id, Qt::CaseInsensitive) == 0 || trimmed == one.label) return true;
    }
    // (the words people call them by: Photoshop, Photoshop の初期設定, Genko)
    for (const char* words : {"Photoshop", "Photoshop の初期設定", "Photoshop 初期設定", "Genko"}) {
        if (trimmed.compare(QString::fromUtf8(words), Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

QString own_base(const QString& name) {
    const QString base = settings()->value(QStringLiteral("%1/%2/base").arg(kOwn, name)).toString();
    return std::any_of(sets().begin(), sets().end(), [&base](const Set& s) { return s.id == base; }) ? base : sets().front().id;
}

std::map<QString, QList<QKeySequence>> own_keys(const QString& name) {
    std::map<QString, QList<QKeySequence>> out;
    const QJsonObject keys = QJsonDocument::fromJson(settings()->value(QStringLiteral("%1/%2/keys").arg(kOwn, name)).toString().toUtf8()).object();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QList<QKeySequence> list;
        for (const QString& one : it.value().toString().split(QLatin1Char('|'), Qt::SkipEmptyParts)) list << QKeySequence(one);
        out[it.key()] = list;
    }
    return out;
}

void save_own(const QString& name, const QString& base, const std::map<QString, QList<QKeySequence>>& keys) {
    QJsonObject out;
    for (const auto& [command, list] : keys) {
        QStringList words;
        for (const QKeySequence& key : list) words << key.toString(QKeySequence::PortableText);
        out[command] = words.join(QLatin1Char('|'));
    }
    auto store = settings();
    store->setValue(QStringLiteral("%1/%2/base").arg(kOwn, name), base);
    store->setValue(QStringLiteral("%1/%2/keys").arg(kOwn, name), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

void delete_own(const QString& name) {
    auto store = settings();
    store->remove(QStringLiteral("%1/%2").arg(kOwn, name));
    if (store->value(QStringLiteral("ui/key_set_own")).toString() == name) store->remove(QStringLiteral("ui/key_set_own"));
}

QString current_own() {
    const QString name = settings()->value(QStringLiteral("ui/key_set_own")).toString();
    return !name.isEmpty() && own_sets().contains(name) ? name : QString();
}

QString chosen() {
    const QString own = current_own();
    return own.isEmpty() ? current() : QStringLiteral("own:") + own;
}

std::map<QString, QList<QKeySequence>> keys_by_name(QMainWindow* window, const QString& set) {
    const bool own = set.startsWith(QStringLiteral("own:"));
    const QString base = own ? own_base(set.mid(4)) : set;
    const auto over = own ? own_keys(set.mid(4)) : std::map<QString, QList<QKeySequence>>{};
    const auto names = command_names(window);
    std::map<QString, QList<QKeySequence>> out;
    for (QAction* action : preferences::commands(window)) {
        const QString name = command_name(names, action);
        const QList<QKeySequence>* found = own_keys_of(over, name);
        out[name] = found != nullptr ? *found : keys_of(action, base);
    }
    return out;
}

QString label(const QString& set) {
    if (set.startsWith(QStringLiteral("own:"))) return set.mid(4);
    for (const Set& one : sets()) {
        if (one.id == set) return one.label;
    }
    return set;
}

}  // namespace genko::app::key_sets
