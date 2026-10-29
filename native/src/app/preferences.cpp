// 環境設定 (Python's genko/app/preferences.py).

#include "app/preferences.hpp"
#include "app/export_as_dialog.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTabletEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <cmath>
#include <set>
#include <stdexcept>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/camera_raw_open.hpp"
#include "app/canvas.hpp"
#include "app/comfort.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/key_sets.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/workspace.hpp"
#include "core/model.hpp"
#include "core/prefs_table.hpp"
#include "core/pynum.hpp"

namespace genko::app::preferences {

namespace {

bool truthy(const QVariant& value) {
    const QString text = value.toString().toLower();
    return text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("yes");
}

QComboBox* choice_box(const std::vector<std::pair<QString, QString>>& choices, const QString& current) {
    auto* box = new QComboBox;
    for (const auto& [label, value] : choices) box->addItem(label, value);
    box->setCurrentIndex(std::max(0, box->findData(current)));
    return box;
}

}  // namespace

const std::vector<std::pair<QString, QString>>& pen_buttons() {
    static const std::vector<std::pair<QString, QString>> all{{QStringLiteral("右クリック（メニュー）"), QStringLiteral("menu")},
                                                              {QStringLiteral("スポイトツール"), QStringLiteral("picker")},
                                                              {QStringLiteral("手のひら（表示を動かす）"), QStringLiteral("hand")}};
    return all;
}

// (PC5 H-0: read through 環境設定の表 — core/prefs_table: its fallbacks, ranges and choices)
namespace {

core::Json pref(const char* path) {
    const auto store = settings();
    return core::prefs::value(*store, path);
}

}  // namespace

int ui_font_pt() { return pref("interface.uiFontSize").get<int>(); }

QString default_paper() { return QString::fromStdString(pref("book.newPaper").get<std::string>()); }

int recent_count() { return pref("fileHandling.recentFileCount").get<int>(); }

bool lowercase_extension() { return pref("fileHandling.lowercaseExtension").get<bool>(); }

bool ignore_exif_profile() { return pref("fileHandling.ignoreExifProfileTag").get<bool>(); }

int save_after_ms() {
    bool ok = false;
    const int value = settings()->value(QStringLiteral("save/after_ms"), kSaveAfterDefault).toInt(&ok);
    return std::clamp(ok && value > 0 ? value : kSaveAfterDefault, kSaveAfterMin, kSaveAfterMax);
}

std::optional<double> tablet_gamma() {
    const core::Json gamma = pref("tablet.pressureGamma");
    if (!gamma.is_number()) return std::nullopt;  // ("": the usual curve)
    return gamma.get<double>();
}

bool brush_preview() { return pref("view.brushPreview").get<bool>(); }

void set_brush_preview(bool on) { settings()->setValue(QStringLiteral("ui/brush_preview"), on ? QStringLiteral("true") : QStringLiteral("false")); }

bool cross_while_drawing() { return pref("cursors.showOnlyCrosshairWhilePainting").get<bool>(); }

QString pen_button() { return QString::fromStdString(pref("tablet.penButton").get<std::string>()); }

double gamma_for(const std::vector<double>& pressures) {
    std::vector<double> usable;
    for (const double p : pressures) {
        if (0.02 < p && p < 0.999) usable.push_back(p);
    }
    std::sort(usable.begin(), usable.end());
    if (usable.size() < 10) throw std::invalid_argument("もう少し長く試し描きします");
    const double median = usable[usable.size() / 2];
    const double gamma = std::max(0.3, std::min(3.0, std::log(0.5) / std::log(median)));
    return core::py_round(gamma, 2);  // (Python's round(x, 2): correctly rounded, ties to even)
}

// --- the commands and their keys ----------------------------------------------------------------------------------------

std::vector<QAction*> commands(QMainWindow* window) {
    // (by its name: its words, and its place after them when another command has the same words — the user's decision 8,
    // workspace's command_names)
    const auto names = command_names(window);
    QSet<QString> seen;
    std::vector<QAction*> out;
    for (QAction* action : window->findChildren<QAction*>()) {
        const QString text = action->text();
        if (text.isEmpty() || action->isSeparator() || action->menu() != nullptr || !action->objectName().startsWith(QStringLiteral("cmd:")) ||
            remade(action)) {
            continue;
        }
        const QString name = command_name(names, action);
        if (seen.contains(name)) continue;
        seen.insert(name);
        out.push_back(action);
    }
    std::stable_sort(out.begin(), out.end(), [](QAction* a, QAction* b) { return a->objectName() < b->objectName(); });
    return out;
}

void name_commands(MainWindow* window) {
    window->default_shortcuts.clear();
    for (QAction* action : window->findChildren<QAction*>()) {
        const QString text = action->text();
        if (text.isEmpty() || action->isSeparator() || action->menu() != nullptr || remade(action)) continue;
        if (action->objectName().isEmpty()) action->setObjectName(QStringLiteral("cmd:%1").arg(text));
    }
    const auto names = command_names(window);
    for (QAction* action : window->findChildren<QAction*>()) {
        const QString text = action->text();
        if (text.isEmpty() || action->isSeparator() || action->menu() != nullptr || remade(action)) continue;
        const QString name = command_name(names, action);
        if (action->objectName().startsWith(QStringLiteral("cmd:")) && !window->default_shortcuts.contains(name)) {
            window->default_shortcuts[name] = action->shortcuts();
        }
    }
}

const std::multimap<QString, QString>& renamed() {
    static const std::multimap<QString, QString> all{{QStringLiteral("レイヤーを消す"), QStringLiteral("レイヤーを削除")},
                                                     {QStringLiteral("このページを消す…"), QStringLiteral("このページを削除…")},
                                                     {QStringLiteral("色の校正"), QStringLiteral("CMYK で見る（色校正）")},  // (PC1b)
                                                     // (PC3, the user's decision 5: the selection's, 編集's, 変形's and the moving's
                                                     // commands by Photoshop's names)
                                                     {QStringLiteral("長方形選択ツール"), QStringLiteral("範囲選択（長方形）")},
                                                     {QStringLiteral("楕円形選択ツール"), QStringLiteral("範囲選択（楕円）")},
                                                     {QStringLiteral("なげなわツール"), QStringLiteral("範囲選択（投げ縄）")},
                                                     {QStringLiteral("多角形選択ツール"), QStringLiteral("範囲選択（折れ線）")},
                                                     {QStringLiteral("自動選択ツール"), QStringLiteral("自動選択")},
                                                     {QStringLiteral("自動選択ツール"), QStringLiteral("色域選択")},  // (PC3 B1: its 色の近さ)
                                                     {QStringLiteral("すべてを選択"), QStringLiteral("すべて選択")},
                                                     {QStringLiteral("拡張…"), QStringLiteral("選択範囲を広げる…")},
                                                     {QStringLiteral("縮小…"), QStringLiteral("選択範囲を狭める…")},
                                                     {QStringLiteral("ピクセルを選択"), QStringLiteral("描画部分から選択")},
                                                     {QStringLiteral("選択範囲を保存…"), QStringLiteral("選択範囲をチャンネルに保存…")},
                                                     {QStringLiteral("クイックマスクモードで編集"), QStringLiteral("クイックマスク")},
                                                     {QStringLiteral("カット"), QStringLiteral("切り取り")},
                                                     {QStringLiteral("ペースト"), QStringLiteral("貼り付け")},
                                                     {QStringLiteral("消去"), QStringLiteral("選択範囲を消す")},
                                                     {QStringLiteral("描画色で塗りつぶし"), QStringLiteral("選択範囲を塗る")},
                                                     {QStringLiteral("水平方向に反転"), QStringLiteral("左右反転")},
                                                     {QStringLiteral("垂直方向に反転"), QStringLiteral("上下反転")},
                                                     {QStringLiteral("自由な形に"), QStringLiteral("自由変形（遠近・4 隅）")},
                                                     {QStringLiteral("ワープ"), QStringLiteral("自由変形（メッシュ・3×3）")},
                                                     {QStringLiteral("ワープ（格子の数を決める）…"), QStringLiteral("自由変形（メッシュ・格子の数を決める）…")},
                                                     {QStringLiteral("変形を確定"), QStringLiteral("自由変形を確定")},
                                                     {QStringLiteral("移動ツール"), QStringLiteral("レイヤー移動")},
                                                     {QStringLiteral("前面へ"), QStringLiteral("レイヤーを前へ")},
                                                     {QStringLiteral("背面へ"), QStringLiteral("レイヤーを後ろへ")},
                                                     // (PC3 B2: レイヤー → レイヤーマスク by Photoshop's names; the words the vector
                                                     // mask's (PC3 C-a) and the page's すべての領域を表示 (PC3 C-e) have too by
                                                     // their places, as the names are when two commands share the words)
                                                     {QStringLiteral("すべての領域を表示（レイヤー → レイヤーマスク）"), QStringLiteral("全部見せるマスクを作る")},
                                                     {QStringLiteral("選択範囲外をマスク"), QStringLiteral("選択範囲からマスクを作る")},
                                                     {QStringLiteral("削除（レイヤー → レイヤーマスク）"), QStringLiteral("マスクを消す")},
                                                     {QStringLiteral("使用しない（レイヤー → レイヤーマスク）"), QStringLiteral("マスクを使わない")},
                                                     // (PC3 C-b: the shape tools' and the direct selection's)
                                                     {QStringLiteral("長方形ツール"), QStringLiteral("図形")},
                                                     {QStringLiteral("部分選択ツール"), QStringLiteral("線の編集（制御点）")},
                                                     {QStringLiteral("前方ワープツール"), QStringLiteral("ゆがみ（指で押す）")},  // (PC3 C-c)
                                                     // (PC4 F-0, 節 F 5.1: ファイル's commands by Photoshop's names)
                                                     {QStringLiteral("新規…"), QStringLiteral("新しい原稿…")},
                                                     {QStringLiteral("別名で保存…"), QStringLiteral("別の場所に保存…")},
                                                     {QStringLiteral("原稿を書き出す…"), QStringLiteral("書き出し…")},
                                                     {QStringLiteral("プリント…"), QStringLiteral("印刷…")},
                                                     {QStringLiteral("終了"), QStringLiteral("Genko を終わる")},
                                                     {QStringLiteral("カラールックアップテーブル…"), QStringLiteral("カラールックアップテーブルを書き出す…")},
                                                     // (PC4 E-0, the user's decision 2: the drawing tools and the colours by
                                                     // Photoshop's names)
                                                     {QStringLiteral("ブラシツール"), QStringLiteral("ペン")},
                                                     {QStringLiteral("消しゴムツール"), QStringLiteral("消しゴム")},
                                                     {QStringLiteral("スポイトツール"), QStringLiteral("スポイト")},
                                                     {QStringLiteral("塗りつぶしツール"), QStringLiteral("塗りつぶし")},
                                                     {QStringLiteral("グラデーションツール"), QStringLiteral("グラデーション")},
                                                     {QStringLiteral("描画色と背景色を入れ替え"), QStringLiteral("メインとサブの色を入れ替える")},
                                                     {QStringLiteral("カラーピッカー（描画色）…"), QStringLiteral("ペンの色…")},
                                                     {QStringLiteral("ブラシサイズを大きく"), QStringLiteral("太く（ペン・消しゴム）")},
                                                     {QStringLiteral("ブラシサイズを小さく"), QStringLiteral("細く（ペン・消しゴム）")},
                                                     {QStringLiteral("選んだ線を描画色にする"), QStringLiteral("選んだ線をペンの色にする")},
                                                     {QStringLiteral("マスクを編集する（ブラシで見せる・消しゴムで隠す）"),
                                                      QStringLiteral("マスクを編集する（ペンで見せる・消しゴムで隠す）")},
                                                     // (PC5 H-0, 節 H 1.2 and the user's decision 12: 表示, ウィンドウ and 編集's
                                                     // commands and the panels by Photoshop's names; 表示 → 水平方向に反転 and 定規
                                                     // share their words with 編集 → 変形's and the 定規 tool: by their places)
                                                     {QStringLiteral("取り消し"), QStringLiteral("元に戻す")},
                                                     {QStringLiteral("やり直し"), QStringLiteral("やり直す")},
                                                     {QStringLiteral("画面サイズに合わせる"), QStringLiteral("全体を表示")},
                                                     {QStringLiteral("ズームイン"), QStringLiteral("拡大")},
                                                     {QStringLiteral("ズームアウト"), QStringLiteral("縮小")},
                                                     {QStringLiteral("プリントサイズ"), QStringLiteral("原寸（紙の大きさ）")},
                                                     {QStringLiteral("ズームツール"), QStringLiteral("虫めがね")},
                                                     {QStringLiteral("水平方向に反転（表示）"), QStringLiteral("左右反転して見る")},
                                                     {QStringLiteral("定規（表示）"), QStringLiteral("目盛りを表示")},
                                                     {QStringLiteral("描画定規を表示"), QStringLiteral("定規を表示")},
                                                     {QStringLiteral("描画定規にスナップ"), QStringLiteral("定規にスナップ")},
                                                     {QStringLiteral("ヒストリー…"), QStringLiteral("履歴…")},
                                                     {QStringLiteral("ヒストリー"), QStringLiteral("履歴")},
                                                     {QStringLiteral("ナビゲーター"), QStringLiteral("全体図")},
                                                     {QStringLiteral("オプション"), QStringLiteral("ツールの設定")},
                                                     {QStringLiteral("検索"), QStringLiteral("コマンドを探す…")},
                                                     {QStringLiteral("新規ウィンドウ"), QStringLiteral("新しいウィンドウ（同じ原稿）")},
                                                     {QStringLiteral("一般…"), QStringLiteral("環境設定…")}};
    return all;
}

const std::vector<QString>& reused_names() {
    // (PC5 H-0: 書き出し… — 原稿を書き出す…'s before PC4 F-0 — is 編集 → 環境設定 → 書き出し…'s own name since; the list is
    // core/prefs_table's, whose set() marks a key it writes)
    static const std::vector<QString> all = [] {
        std::vector<QString> out;
        for (const std::string& name : core::prefs::reused_names()) out.push_back(QString::fromStdString(name));
        return out;
    }();
    return all;
}

const std::map<QString, QString>& words_owner() {
    static const std::map<QString, QString> all{{QStringLiteral("定規"), QStringLiteral("act_ruler")}};
    return all;
}

const std::vector<Pane>& panes() {
    // (Photoshop's 環境設定 ▸ in its order, then Genko's own; the pages the dialog has now — H-e makes the rest and sorts the
    // dialog by these panes)
    static const std::vector<Pane> all{
        {QStringLiteral("general"), QStringLiteral("一般…"), QStringLiteral("表示・作業")},
        {QStringLiteral("interface"), QStringLiteral("インターフェイス…"), QStringLiteral("表示・作業")},
        {QStringLiteral("workspace"), QStringLiteral("ワークスペース…"), QString()},
        {QStringLiteral("tools"), QStringLiteral("ツール…"), QStringLiteral("表示・作業")},
        {QStringLiteral("historyLog"), QStringLiteral("ヒストリーログ…"), QString()},
        {QStringLiteral("fileHandling"), QStringLiteral("ファイル管理…"), QStringLiteral("ファイル管理")},
        {QStringLiteral("export"), QStringLiteral("書き出し…"), QStringLiteral("書き出し")},
        {QStringLiteral("performance"), QStringLiteral("パフォーマンス…"), QStringLiteral("表示・作業")},
        {QStringLiteral("scratchDisks"), QStringLiteral("仮想記憶ディスク…"), QString()},
        {QStringLiteral("cursors"), QStringLiteral("カーソル…"), QStringLiteral("表示・作業")},
        {QStringLiteral("transparencyAndGamut"), QStringLiteral("透明部分・色域…"), QStringLiteral("透明部分・色域")},
        {QStringLiteral("unitsAndRulers"), QStringLiteral("単位・定規…"), QString()},
        {QStringLiteral("guidesGridAndSlices"), QStringLiteral("ガイド・グリッド・スライス…"), QString()},
        {QStringLiteral("plugIns"), QStringLiteral("プラグイン…"), QString()},
        {QStringLiteral("type"), QStringLiteral("テキスト…"), QString()},
        {QStringLiteral("enhancedControls"), QStringLiteral("拡張コントロール…"), QString()},
        {QStringLiteral("rawDefaults"), QStringLiteral("Camera Raw…"), QStringLiteral("Camera Raw")},
        {QStringLiteral("integrations"), QStringLiteral("連携…"), QString()},
        {QStringLiteral("tablet"), QStringLiteral("ペンタブレット…"), QStringLiteral("ペンタブレット")},
        {QStringLiteral("comfort"), QStringLiteral("長い時間の作業…"), QStringLiteral("表示・作業")}};
    return all;
}

bool reused_name(const QString& name) {
    const auto& all = reused_names();
    return std::find(all.begin(), all.end(), name) != all.end();
}

bool names_current(const QString& key) {
    const auto store = settings();
    const QString mark = QStringLiteral("names_current/") + key;
    return store->contains(mark) && store->value(mark).toString() == store->value(key).toString();
}

void mark_names_current(const QString& key) {
    const auto store = settings();
    store->setValue(QStringLiteral("names_current/") + key, store->value(key).toString());
}

namespace {

// The old keys a command's keys are read under when it has none under its name (apply_shortcuts' order): its old names'
// (renamed(), a reused one only while not marked as the new command's) and its words alone when another command came to
// share them.
QStringList old_keys(MainWindow* window, QAction* action, const std::map<const QAction*, QString>& names, const std::map<QString, QAction*>& first) {
    const auto current = [](const QString& name) { return names_current(QStringLiteral("shortcuts/") + name); };
    const QString name = command_name(names, action);
    const auto words = first.find(action->text());
    const bool first_of_words = name != action->text() && words != first.end() && words->second == action;
    QStringList out;
    for (const QString& now : first_of_words ? QStringList{name, action->text()} : QStringList{name}) {
        for (auto [old, end] = renamed().equal_range(now); old != end; ++old) {
            if (!(reused_name(old->second) && current(old->second))) out << QStringLiteral("shortcuts/%1").arg(old->second);
        }
    }
    if (first_of_words) out << QStringLiteral("shortcuts/%1").arg(action->text());
    (void)window;
    return out;
}

}  // namespace

void apply_shortcuts(MainWindow* window) {
    const auto store = settings();
    std::map<QString, QString> chosen;  // (a key the person gave a command: the command)
    std::vector<QAction*> own;          // (the commands with their own keys)
    const auto names = command_names(window);
    const auto first = by_text(window);  // (the words alone: the first command of them in the menus)
    // (PC4 E4: keys kept under a reused name before it was a command's own name are the renamed command's — reused_names())
    const auto current = [](const QString& name) { return names_current(QStringLiteral("shortcuts/") + name); };
    for (QAction* action : preferences::commands(window)) {
        const QString name = command_name(names, action);
        QString key = QStringLiteral("shortcuts/%1").arg(name);
        if (reused_name(name) && !current(name)) key = QStringLiteral("shortcuts_unread/%1").arg(name);  // (none: the old command's)
        // (keys chosen under an old name: renamed() knows the new words, which another command may also have come to
        // have — the user's decision 8 —: then the first of them in the menus had the old name, as below)
        const auto words = first.find(action->text());
        const bool first_of_words = name != action->text() && words != first.end() && words->second == action;
        for (const QString& now : first_of_words ? QStringList{name, action->text()} : QStringList{name}) {
            for (auto [old, end] = renamed().equal_range(now); !store->contains(key) && old != end; ++old) {
                if (reused_name(old->second) && current(old->second)) continue;  // (the new command's own key now)
                if (store->contains(QStringLiteral("shortcuts/%1").arg(old->second))) {
                    key = QStringLiteral("shortcuts/%1").arg(old->second);  // (keys chosen under an old name)
                }
            }
        }
        // (keys chosen under its words alone, before another command had the same words — the user's decision 8: the
        // first of them in the menus had them then)
        if (const auto alone = first.find(action->text());
            !store->contains(key) && name != action->text() && alone != first.end() && alone->second == action &&
            store->contains(QStringLiteral("shortcuts/%1").arg(action->text()))) {
            key = QStringLiteral("shortcuts/%1").arg(action->text());
        }
        if (store->contains(key)) {
            const QString value = store->value(key).toString();
            QList<QKeySequence> keys;
            for (const QString& one : value.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
                keys << QKeySequence(one);
                chosen.emplace(keys.back().toString(QKeySequence::PortableText), name);
            }
            action->setShortcuts(keys);
        } else {
            own.push_back(action);
        }
    }
    // (a command's own key the person has given to another command — a key a later version gave a new command: the
    // person's stays and the command's own is taken off, as the preferences refuse one key for two; said)
    QStringList taken;
    for (QAction* action : own) {
        const QString name = command_name(names, action);
        const auto found = window->default_shortcuts.find(name);
        if (found == window->default_shortcuts.end()) continue;
        QList<QKeySequence> keys;
        for (const QKeySequence& key : found->second) {
            const auto other = chosen.find(key.toString(QKeySequence::PortableText));
            if (other == chosen.end() || other->second == name) {
                keys << key;
                continue;
            }
            taken << QStringLiteral("%1 は「%2」に付けてあるため、「%3」のキーから外しました").arg(key.toString(QKeySequence::NativeText), other->second, name);
        }
        if (keys != action->shortcuts()) action->setShortcuts(keys);
    }
    retip(window);
    // (said once for the window: the same keys taken off again — each time the keys are applied — are not said again)
    const QString said = taken.join(QLatin1Char('\n'));
    if (!taken.isEmpty() && window->property("genko_keys_taken").toString() != said) {
        window->flash(said + QStringLiteral("（編集 → キーボードショートカット… で変えられます）"), 8000);
    }
    window->setProperty("genko_keys_taken", said);
}

QString keys_in_words(const std::vector<const QAction*>& actions) {
    QStringList keys;
    for (const QAction* action : actions) {
        if (action != nullptr && !action->shortcut().isEmpty()) keys << action->shortcut().toString(QKeySequence::NativeText);
    }
    return keys.isEmpty() ? QString() : QStringLiteral("（%1）").arg(keys.join(QStringLiteral("・")));
}

void retip(MainWindow* window) {
    const auto names = command_names(window);
    std::set<const QAction*> tools;
    for (const auto& [name, action] : window->tool_actions()) tools.insert(action);
    for (QAction* action : preferences::commands(window)) {
        const QString keys = action->shortcut().toString(QKeySequence::NativeText);
        const QString what = action->statusTip();
        // (its name: its place too when another command has the same words — the command bar's button tells it so; a
        // tool's is its name alone, the tool palette showing which it is)
        const QString name = tools.contains(action) ? action->text() : command_name(names, action);
        action->setToolTip(QString(name).remove(QLatin1Char('&')) + (keys.isEmpty() ? QString() : QStringLiteral("（%1）").arg(keys)) +
                           (what.isEmpty() ? QString() : QStringLiteral("\n") + what));
    }
}

std::vector<Clash> conflicts(const std::vector<std::pair<QString, QStringList>>& mapping) {
    std::map<QString, QString> owner;
    std::vector<Clash> out;
    for (const auto& [name, keys] : mapping) {
        for (const QString& key : keys) {
            if (key.isEmpty()) continue;
            const auto found = owner.find(key);
            if (found != owner.end() && found->second != name) out.push_back(Clash{key, found->second, name});
            owner.emplace(key, name);
        }
    }
    return out;
}

const std::vector<std::pair<QString, QString>>& checker_sizes() {
    static const std::vector<std::pair<QString, QString>> sizes{{QStringLiteral("なし"), QStringLiteral("none")},
                                                                {QStringLiteral("小"), QStringLiteral("small")},
                                                                {QStringLiteral("中"), QStringLiteral("medium")},
                                                                {QStringLiteral("大"), QStringLiteral("large")}};
    return sizes;
}

const std::vector<std::pair<QString, QString>>& checker_colour_sets() {
    static const std::vector<std::pair<QString, QString>> sets{
        {QStringLiteral("明るい"), QStringLiteral("light")}, {QStringLiteral("中間"), QStringLiteral("medium")}, {QStringLiteral("暗い"), QStringLiteral("dark")},
        {QStringLiteral("赤"), QStringLiteral("red")},       {QStringLiteral("オレンジ"), QStringLiteral("orange")}, {QStringLiteral("緑"), QStringLiteral("green")},
        {QStringLiteral("青"), QStringLiteral("blue")},      {QStringLiteral("紫"), QStringLiteral("purple")},       {QStringLiteral("カスタム"), QStringLiteral("custom")}};
    return sets;
}

namespace {

// (upstream's TransparencyAndGamut::square and ::colors)
int checker_square(const QString& size) {
    return size == QLatin1String("none") ? 0 : size == QLatin1String("small") ? 4 : size == QLatin1String("large") ? 16 : 8;
}

std::pair<QColor, QColor> checker_pair(const QString& set, const QColor& light, const QColor& dark) {
    static const std::map<QString, std::pair<QColor, QColor>> sets{
        {QStringLiteral("light"), {QColor(255, 255, 255), QColor(204, 204, 204)}}, {QStringLiteral("medium"), {QColor(153, 153, 153), QColor(102, 102, 102)}},
        {QStringLiteral("dark"), {QColor(102, 102, 102), QColor(51, 51, 51)}},     {QStringLiteral("red"), {QColor(255, 255, 255), QColor(255, 204, 204)}},
        {QStringLiteral("orange"), {QColor(255, 255, 255), QColor(255, 229, 204)}}, {QStringLiteral("green"), {QColor(255, 255, 255), QColor(204, 255, 204)}},
        {QStringLiteral("blue"), {QColor(255, 255, 255), QColor(204, 229, 255)}},   {QStringLiteral("purple"), {QColor(255, 255, 255), QColor(229, 204, 255)}}};
    if (set == QLatin1String("custom")) return {light.isValid() ? light : QColor(255, 255, 255), dark.isValid() ? dark : QColor(204, 204, 204)};
    const auto it = sets.find(set);
    return it != sets.end() ? it->second : sets.at(QStringLiteral("light"));
}

void show_colour(QPushButton* button, const QColor& colour) {
    button->setProperty("colour", colour);
    QPixmap swatch(28, 14);
    swatch.fill(colour);
    button->setIcon(QIcon(swatch));
    button->setText(colour.name());
}

}  // namespace

Checker checker() {
    const auto store = settings();
    Checker out;
    out.square = checker_square(store->value(QStringLiteral("view/checker_size"), QStringLiteral("medium")).toString());
    const auto [light, dark] = checker_pair(store->value(QStringLiteral("view/checker_colors"), QStringLiteral("light")).toString(),
                                            QColor(store->value(QStringLiteral("view/checker_light"), QStringLiteral("#ffffff")).toString()),
                                            QColor(store->value(QStringLiteral("view/checker_dark"), QStringLiteral("#cccccc")).toString()));
    out.light = light;
    out.dark = dark;
    return out;
}

void paint_checker(QImage& image, const Checker& c) {
    if (c.square <= 0) {
        image.fill(Qt::white);
        return;
    }
    QPainter painter(&image);
    painter.fillRect(image.rect(), c.light);
    for (int y = 0; y < image.height(); y += c.square)
        for (int x = (y / c.square) % 2 == 0 ? c.square : 0; x < image.width(); x += 2 * c.square) painter.fillRect(x, y, c.square, c.square, c.dark);
}

QColor gamut_colour() {
    const QColor colour(settings()->value(QStringLiteral("view/gamut_color"), QStringLiteral("#808080")).toString());
    return colour.isValid() ? colour : QColor(128, 128, 128);
}

int gamut_opacity() { return pref("transparencyAndGamut.gamutWarningOpacity").get<int>(); }

void apply_all(MainWindow* window) {
    // (PC5 H-0: the key set's keys first — each command's own then —, the person's over them)
    key_sets::apply(window, key_sets::current());
    name_commands(window);
    apply_shortcuts(window);
    if (ToolSettings* ts = window->tool_settings(); ts != nullptr && !ts->tool().isEmpty()) ts->show_tool(ts->tool());  // (its keys in its title)
    window->set_save_after(save_after_ms());
    PageCanvas* canvas = window->canvas();
    canvas->pen_button = pen_button();
    if (window->brush_panel() != nullptr) window->brush_panel()->set_personal_pressure(tablet_gamma());
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    theme::apply(app);
    apply_motion(app);
    apply_font(app);
    window->refresh_icons();
    window->refresh_first_steps();  // (the keys its buttons show)
    canvas->cursor_kind = cursor_kind();
    canvas->cross_while_drawing = cross_while_drawing();  // (PC4 E1)
    canvas->brush_preview = brush_preview();
    canvas->modifier_tools = {{QStringLiteral("alt"), modifier_tool(QStringLiteral("alt"))}, {QStringLiteral("ctrl"), modifier_tool(QStringLiteral("ctrl"))}};
    canvas->tool_modifiers = tool_modifiers();
    canvas->refresh_cursor();
    window->apply_proof_view();  // (透明部分・色域: the gamut warning's mark)
    canvas->update();
}

bool tool_keys_dialog(QWidget* parent, MainWindow* window, const std::function<void(QDialog&)>& before_exec) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("道具ごとの修飾キー"));
    auto* grid = new QGridLayout(&dialog);
    int col = 0;
    for (const QString& head : {QString(), QStringLiteral("Alt"), QStringLiteral("Ctrl"), QStringLiteral("Shift")}) grid->addWidget(new QLabel(head), 0, col++);
    const auto now = tool_modifiers();
    std::vector<std::pair<std::pair<QString, QString>, QComboBox*>> boxes;
    int row = 1;
    for (const auto& [label, tool] : keyed_tools()) {
        grid->addWidget(new QLabel(label), row, 0);
        int at = 1;
        for (const QString& key : {QStringLiteral("alt"), QStringLiteral("ctrl"), QStringLiteral("shift")}) {
            QString current;
            if (const auto own = now.find(tool); own != now.end()) {
                if (const auto it = own->second.find(key); it != own->second.end()) current = it->second;
            }
            QComboBox* box = choice_box(held_tools(), current);
            box->setObjectName(tool + QLatin1Char('/') + key);
            grid->addWidget(box, row, at++);
            boxes.push_back({{tool, key}, box});
        }
        ++row;
    }
    auto* note = new QLabel(QStringLiteral("Shift は、決めた道具だけで持ち替えます（それ以外は直線を引くなど、いつもの働き）。"));
    note->setWordWrap(true);
    grid->addWidget(note, row, 0, 1, 4);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    grid->addWidget(buttons, row + 1, 0, 1, 4);
    if (before_exec) before_exec(dialog);
    if (dialog.result() != QDialog::Accepted && ask::exec(&dialog) != QDialog::Accepted) return false;
    for (const auto& [where, box] : boxes) set_tool_modifier(where.first, where.second, box->currentData().toString());
    if (window != nullptr) window->canvas()->tool_modifiers = tool_modifiers();
    return true;
}

// --- the pen tablet's pad ----------------------------------------------------------------------------------------------

PressurePad::PressurePad() {
    setMinimumSize(320, 140);
    setAttribute(Qt::WA_TabletTracking, true);
}

void PressurePad::tabletEvent(QTabletEvent* event) {
    if ((event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove) && event->pressure() > 0) {
        pressures.push_back(event->pressure());
        points.emplace_back(event->position(), event->pressure());
        update();
    }
    event->accept();
}

void PressurePad::clear() {
    pressures.clear();
    points.clear();
    update();
}

void PressurePad::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    painter.setPen(QPen(Qt::lightGray, 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    if (points.empty()) painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("ここにペンでいつものように描きます"));
    for (const auto& [position, pressure] : points) {
        painter.setPen(QPen(Qt::black, 0.5 + 6 * pressure, Qt::SolidLine, Qt::RoundCap));
        painter.drawPoint(position);
    }
}

// --- the dialog --------------------------------------------------------------------------------------------------------

PreferencesDialog::PreferencesDialog(MainWindow* window) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("環境設定"));
    setMinimumSize(560, 480);
    tabs = new QTabWidget;
    // shortcuts
    table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({QStringLiteral("コマンド"), QStringLiteral("ショートカット")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    const auto names = command_names(window);
    for (QAction* action : preferences::commands(window)) {
        const QString name = command_name(names, action);  // (its place too when another has the same words)
        const int row = table->rowCount();
        table->insertRow(row);
        auto* item = new QTableWidgetItem(name);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, item);
        auto* edit = new QKeySequenceEdit(action->shortcut());
        if (auto* field = edit->findChild<QLineEdit*>()) field->setPlaceholderText(QStringLiteral("キーを押す"));
        edit->setAccessibleName(QStringLiteral("%1 のショートカット").arg(name));
        table->setCellWidget(row, 1, edit);
        editors.emplace_back(name, edit);
    }
    clash = new QLabel;
    theme::role(clash, "error");
    clash->setWordWrap(true);
    auto* reset = new QPushButton(QStringLiteral("すべて最初のキーに戻す"));
    connect(reset, &QPushButton::clicked, this, &PreferencesDialog::reset_keys);
    // キーの組 (PC5 H-0, the user's decision 8): Photoshop's keys, or Genko's before PC5
    key_set = new QComboBox;
    key_set->setObjectName(QStringLiteral("prefs_key_set"));
    for (const key_sets::Set& one : key_sets::sets()) key_set->addItem(one.label, one.id);
    key_set->setCurrentIndex(std::max(0, key_set->findData(key_sets::current())));
    key_set->setToolTip(QStringLiteral("「Photoshop の初期設定」は Photoshop と同じキー、「Genko」は PC5 より前の Genko のキーです。自分で変えたキーはどちらでもそのまま"));
    choose_key_set();
    connect(key_set, &QComboBox::currentIndexChanged, this, [this] { choose_key_set(); });
    auto* set_row = new QHBoxLayout;
    set_row->addWidget(new QLabel(QStringLiteral("キーの組")));
    set_row->addWidget(key_set, 1);
    auto* keys = new QWidget;
    auto* kl = new QVBoxLayout(keys);
    kl->addLayout(set_row);
    kl->addWidget(new QLabel(QStringLiteral("キーを変えたいコマンドの右をクリックして、新しいキーを押します。")));
    kl->addWidget(table, 1);
    kl->addWidget(clash);
    kl->addWidget(reset);
    tabs->addTab(keys, QStringLiteral("ショートカット"));
    // the pen tablet
    pad = new PressurePad;
    gamma_note = new QLabel;
    gamma_note->setWordWrap(true);
    gamma = tablet_gamma();
    auto* measure_button = new QPushButton(QStringLiteral("この描き方に合わせる"));
    connect(measure_button, &QPushButton::clicked, this, &PreferencesDialog::measure);
    auto* again = new QPushButton(QStringLiteral("描き直す"));
    connect(again, &QPushButton::clicked, pad, &PressurePad::clear);
    auto* forget = new QPushButton(QStringLiteral("合わせるのをやめる"));
    connect(forget, &QPushButton::clicked, this, &PreferencesDialog::forget_gamma);
    button = choice_box(pen_buttons(), pen_button());
    auto* tablet = new QWidget;
    auto* tl = new QVBoxLayout(tablet);
    tl->addWidget(new QLabel(QStringLiteral("筆圧の試し書き: いつもの強さで線を何本か引き、「この描き方に合わせる」を押します。")));
    tl->addWidget(pad, 1);
    auto* row = new QFormLayout;
    row->addRow(QString(), measure_button);
    row->addRow(QString(), again);
    row->addRow(QString(), forget);
    row->addRow(QString(), gamma_note);
    row->addRow(QStringLiteral("ペンのサイドボタン"), button);
    tl->addLayout(row);
    tabs->addTab(tablet, QStringLiteral("ペンタブレット"));
    // the screen and the work
    ui_font = choice_box(theme::font_choices(), theme::font_choice());
    ui_font->setToolTip(QStringLiteral("Genko の標準は、どのパソコンでも同じ見た目になるよう同梱した書体です（SIL OFL）"));
    font_pt = new QSpinBox;
    font_pt->setRange(0, 24);
    font_pt->setSpecialValueText(QStringLiteral("パソコンの設定のまま"));
    font_pt->setSuffix(QStringLiteral(" pt"));
    font_pt->setValue(ui_font_pt());
    paper = new QComboBox;
    paper->addItem(QStringLiteral("前回と同じ（B4 投稿用）"), QString());
    for (const core::PaperPreset& p : core::paper_presets()) {
        paper->addItem(QString::fromUtf8(p.label.data(), static_cast<qsizetype>(p.label.size())), QString::fromLatin1(p.key.data(), static_cast<qsizetype>(p.key.size())));
    }
    paper->setCurrentIndex(std::max(0, paper->findData(default_paper())));
    save_after = new QSpinBox;
    save_after->setRange(kSaveAfterMin / 1000 + 1, kSaveAfterMax / 1000);
    save_after->setSuffix(QStringLiteral(" 秒"));
    save_after->setValue(std::max(1, static_cast<int>(std::lround(save_after_ms() / 1000.0))));
    save_after->setToolTip(QStringLiteral("手を止めてからこの時間で保存します。描き続けているときも 10 秒までには保存します"));
    theme = choice_box(theme::modes(), theme::mode());
    brightness = new QSlider(Qt::Horizontal);
    brightness->setRange(-2, 2);
    brightness->setPageStep(1);
    brightness->setTickPosition(QSlider::TicksBelow);
    brightness->setValue(theme::brightness());
    brightness->setToolTip(QStringLiteral("パネルのグレーの明るさ（左ほど暗く、右ほど明るい）"));
    surround = choice_box({{QStringLiteral("自動（画面の色に合わせる）"), QStringLiteral("auto")}, {QStringLiteral("暗いグレー"), QStringLiteral("58")},
                           {QStringLiteral("中間のグレー"), QStringLiteral("128")}, {QStringLiteral("明るいグレー"), QStringLiteral("190")},
                           {QStringLiteral("黒"), QStringLiteral("20")}},
                          theme::surround_setting());
    surround->setToolTip(QStringLiteral("原稿のまわりの色。無彩色のグレーにしておくと、原稿のグレーの見え方が狂いません"));
    mono_icons = new QCheckBox(QStringLiteral("アイコンをグレーだけで描く"));
    mono_icons->setChecked(theme::mono_icons());
    hints = new QCheckBox(QStringLiteral("パネルに説明の文を出す"));
    hints->setToolTip(QStringLiteral("切ると、説明はツールチップに入ります"));
    hints->setChecked(theme::show_hints());
    radial = new QCheckBox(QStringLiteral("描く道具の右クリックで円形のメニュー"));
    radial->setToolTip(QStringLiteral("Shift＋右クリックは、ふだんのメニューになります"));
    radial->setChecked(radial_on());
    rest = new QComboBox;
    for (const auto& [label, minutes] : {std::pair{"知らせない", 0}, {"30 分ごと", 30}, {"45 分ごと", 45}, {"60 分ごと", 60}, {"90 分ごと", 90}}) {
        rest->addItem(QString::fromUtf8(label), minutes);
    }
    rest->setCurrentIndex(std::max(0, rest->findData(settings()->value(QStringLiteral("ui/rest_minutes"), 0).toInt())));
    rest->setToolTip(QStringLiteral("作業を続けた時間（手を止めていた時間は数えない）で、目を休める頃を知らせます"));
    requests = new QCheckBox(QStringLiteral("承認の依頼が届いたら、承認箱を前に出す"));
    requests->setToolTip(QStringLiteral("AI から承認の依頼や相談が届いたとき"));
    requests->setChecked(truthy(settings()->value(QStringLiteral("ui/raise_requests"), QStringLiteral("true"))));
    gpu = new QCheckBox(QStringLiteral("原稿の表示にグラフィックボードを使う"));
    gpu->setChecked(truthy(settings()->value(QStringLiteral("ui/gpu_canvas"), QStringLiteral("false"))));
    gpu->setToolTip(QStringLiteral("拡大・回転がなめらかになります（次に開いた窓から）。原稿が白いままになったら外してください。"
                                   "対応していないパソコンでは、入れていても使いません"));
    gpu_memory = new QSpinBox;  // (PC5 H-a, render:GPU-CPU-parity)
    gpu_memory->setObjectName(QStringLiteral("gpu_memory"));
    gpu_memory->setRange(64, 65536);
    gpu_memory->setSingleStep(64);
    gpu_memory->setSuffix(QStringLiteral(" MB"));
    gpu_memory->setValue(static_cast<int>(PageCanvas::gpu_budget_bytes() / (1024 * 1024)));
    gpu_memory->setToolTip(QStringLiteral("グラフィックボードで表示するときに、原稿の絵を置いておく量の上限です。超えると、前に表示した所から外します（次に開いた窓から）"));
    motion = new QCheckBox(QStringLiteral("動きを減らす"));
    motion->setToolTip(QStringLiteral("メニューの開き方・拡大の寄り方・知らせの出方などの動きを止めます（決めていなければパソコンの設定に合わせます）"));
    motion->setChecked(reduce_motion());
    cursor = choice_box(cursors(), cursor_kind());
    cross_drawing = new QCheckBox(QStringLiteral("描いている間は十字だけにする"));
    cross_drawing->setObjectName(QStringLiteral("cross_drawing"));
    cross_drawing->setToolTip(QStringLiteral("線を引いている間は、ブラシの円や形を隠して十字のポインターだけを見せます（Photoshop の「ペイント時は十字のみ表示」）"));
    cross_drawing->setChecked(cross_while_drawing());
    alt_tool = choice_box(modifier_tool_choices(), modifier_tool(QStringLiteral("alt")));
    ctrl_tool = choice_box(modifier_tool_choices(), modifier_tool(QStringLiteral("ctrl")));
    for (QComboBox* box : {alt_tool, ctrl_tool}) box->setToolTip(QStringLiteral("押している間だけ、この道具になります（離すと元の道具に戻る）"));
    backup_folder = new QLineEdit(settings()->value(QStringLiteral("backup/folder"), QString()).toString());
    backup_folder->setPlaceholderText(QStringLiteral("残さない"));
    backup_folder->setToolTip(QStringLiteral("保存したとき、原稿をまるごと zip にしてこのフォルダに残します（原稿の外のフォルダ）"));
    auto* pick_backup_button = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick_backup_button, &QPushButton::clicked, this, &PreferencesDialog::pick_backup);
    auto* backup_row = new QWidget;
    auto* brow = new QHBoxLayout(backup_row);
    brow->setContentsMargins(0, 0, 0, 0);
    brow->addWidget(backup_folder, 1);
    brow->addWidget(pick_backup_button);
    backup_minutes = new QSpinBox;
    backup_minutes->setRange(1, 1440);
    backup_minutes->setSuffix(QStringLiteral(" 分ごと"));
    backup_minutes->setValue(static_cast<int>(settings()->value(QStringLiteral("backup/minutes"), 30).toDouble()));
    backup_keep = new QSpinBox;
    backup_keep->setRange(1, 200);
    backup_keep->setSuffix(QStringLiteral(" 個まで残す"));
    backup_keep->setValue(settings()->value(QStringLiteral("backup/keep"), 10).toInt());
    hold_swap = new QCheckBox(QStringLiteral("道具のキーを押している間だけ持ち替える"));
    hold_swap->setChecked(app::hold_swap());
    hold_swap->setToolTip(QStringLiteral("道具のキー（E の消しゴムツールなど）を長く押していると、離したときに前の道具に戻ります。短く押すと持ち替えたまま"));
    per_tool = new QPushButton(QStringLiteral("道具ごとの修飾キー…"));
    per_tool->setToolTip(QStringLiteral("ブラシツール・消しゴムツールなど道具ごとに、Alt・Ctrl・Shift を押している間の道具を決めます"));
    connect(per_tool, &QPushButton::clicked, this, [this] { tool_keys_dialog(this, window_); });
    auto* work = new QWidget;
    QFormLayout* wl = look::form();
    work->setLayout(wl);
    auto* note = new QLabel(QStringLiteral("文字の大きさは、決めるとすぐに変わります。"));
    theme::role(note, "hint");
    const std::vector<std::pair<QString, std::vector<std::pair<QString, QWidget*>>>> sections{
        {QStringLiteral("見た目"),
         {{QStringLiteral("画面の色"), theme}, {QStringLiteral("パネルの明るさ"), brightness}, {QStringLiteral("原稿のまわり"), surround},
          {QStringLiteral("画面の書体"), ui_font}, {QStringLiteral("画面の文字の大きさ"), font_pt}, {QString(), note}, {QString(), mono_icons},
          {QString(), hints}, {QString(), gpu}, {QStringLiteral("グラフィックボードの記憶の上限"), gpu_memory}}},
        {QStringLiteral("描く・操作"),
         {{QStringLiteral("ペンのカーソル"), cursor}, {QString(), cross_drawing}, {QStringLiteral("Alt を押している間"), alt_tool},
          {QStringLiteral("Ctrl を押している間"), ctrl_tool},
          {QString(), per_tool}, {QString(), hold_swap}, {QString(), radial}}},
        {QStringLiteral("長い時間の作業"), {{QStringLiteral("休憩の案内"), rest}, {QString(), motion}, {QString(), requests}}}};
    for (const auto& [head, rows] : sections) {
        wl->addRow(look::section(head));
        for (const auto& [label, widget] : rows) wl->addRow(label, widget);
    }
    look::quiet_labels(wl);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(work);
    tabs->addTab(scroll, QStringLiteral("表示・作業"));
    // ファイル管理 (PC4 F-b, edit.preferences.fileHandling: 原稿と保存 and the files' own)
    QWidget* files_page = nullptr;
    {
        recent_count = new QSpinBox;
        recent_count->setObjectName(QStringLiteral("prefs_recent_count"));
        recent_count->setRange(0, kRecentMax);
        recent_count->setSuffix(QStringLiteral(" 個"));
        recent_count->setValue(preferences::recent_count());
        recent_count->setToolTip(QStringLiteral("ファイル → 最近使用したファイルを開く に並べる原稿・画像の数"));
        lowercase_extension = new QCheckBox(QStringLiteral("拡張子を小文字にする"));
        lowercase_extension->setObjectName(QStringLiteral("prefs_lowercase_extension"));
        lowercase_extension->setChecked(preferences::lowercase_extension());
        lowercase_extension->setToolTip(QStringLiteral("コピーを保存… で書くファイルの拡張子（.png・.jpg など）を小文字にします"));
        ignore_exif_profile = new QCheckBox(QStringLiteral("EXIF のプロファイルのタグを無視"));
        ignore_exif_profile->setObjectName(QStringLiteral("prefs_ignore_exif_profile"));
        ignore_exif_profile->setChecked(preferences::ignore_exif_profile());
        ignore_exif_profile->setToolTip(QStringLiteral("プロファイルを持たない画像を開くとき、EXIF の「sRGB」の印を見ずに作業用の RGB を割り当てます"));
        auto* compatible = new QLabel(QStringLiteral("PSD と PSB の互換性を優先: 常に（Genko の PSD はいつも統合した画像を持ちます）"));
        compatible->setWordWrap(true);
        theme::role(compatible, "hint");
        auto* page = new QWidget;
        QFormLayout* rows = look::form();
        page->setLayout(rows);
        rows->addRow(look::section(QStringLiteral("原稿と保存")));
        for (const auto& [label, widget] : std::vector<std::pair<QString, QWidget*>>{
                 {QStringLiteral("新しい原稿の用紙"), paper}, {QStringLiteral("変更を保存するまで"), save_after},
                 {QStringLiteral("バックアップの置き場所"), backup_row}, {QStringLiteral("バックアップの間隔"), backup_minutes},
                 {QStringLiteral("バックアップの数"), backup_keep}}) {
            rows->addRow(label, widget);
        }
        rows->addRow(look::section(QStringLiteral("ファイル")));
        rows->addRow(QStringLiteral("最近使用したファイルの数"), recent_count);
        rows->addRow(QString(), lowercase_extension);
        rows->addRow(QString(), ignore_exif_profile);
        rows->addRow(QString(), compatible);
        look::quiet_labels(rows);
        files_page = page;
    }
    // 透明部分・色域 (edit.preferences.transparencyAndGamut)
    {
        const auto store = settings();
        checker_size = choice_box(checker_sizes(), store->value(QStringLiteral("view/checker_size"), QStringLiteral("medium")).toString());
        checker_colours = choice_box(checker_colour_sets(), store->value(QStringLiteral("view/checker_colors"), QStringLiteral("light")).toString());
        checker_light = new QPushButton;
        checker_dark = new QPushButton;
        show_colour(checker_light, QColor(store->value(QStringLiteral("view/checker_light"), QStringLiteral("#ffffff")).toString()));
        show_colour(checker_dark, QColor(store->value(QStringLiteral("view/checker_dark"), QStringLiteral("#cccccc")).toString()));
        checker_preview = new QLabel;
        checker_preview->setFixedSize(96, 48);
        gamut_button = new QPushButton;
        show_colour(gamut_button, gamut_colour());
        gamut_percent = new QSpinBox;
        gamut_percent->setRange(1, 100);
        gamut_percent->setSuffix(QStringLiteral(" %"));
        gamut_percent->setValue(gamut_opacity());
        for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{{"checker_size", checker_size}, {"checker_colors", checker_colours},
                                                                                         {"checker_light", checker_light}, {"checker_dark", checker_dark},
                                                                                         {"gamut_color", gamut_button}, {"gamut_opacity", gamut_percent}})
            widget->setObjectName(QStringLiteral("prefs_%1").arg(QString::fromLatin1(name)));
        for (QPushButton* button : {checker_light, checker_dark, gamut_button}) {
            connect(button, &QPushButton::clicked, this, [this, button] {
                if (const auto chosen = ask::colour(this, button->property("colour").value<QColor>(), QStringLiteral("色を選ぶ"))) {
                    show_colour(button, *chosen);
                    show_checker();
                }
            });
        }
        connect(checker_size, &QComboBox::currentIndexChanged, this, [this] { show_checker(); });
        connect(checker_colours, &QComboBox::currentIndexChanged, this, [this] { show_checker(); });
        auto* page = new QWidget;
        QFormLayout* rows = look::form();
        page->setLayout(rows);
        rows->addRow(look::section(QStringLiteral("透明部分")));
        rows->addRow(QStringLiteral("市松の大きさ"), checker_size);
        rows->addRow(QStringLiteral("市松の色"), checker_colours);
        auto* custom = new QWidget;
        auto* line = new QHBoxLayout(custom);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(checker_light);
        line->addWidget(checker_dark);
        line->addStretch(1);
        rows->addRow(QStringLiteral("カスタムの色"), custom);
        rows->addRow(QString(), checker_preview);
        rows->addRow(look::section(QStringLiteral("色域外警告")));
        rows->addRow(QStringLiteral("印の色"), gamut_button);
        rows->addRow(QStringLiteral("不透明度"), gamut_percent);
        auto* hint = new QLabel(QStringLiteral("市松は、レイヤーの縮小図の透明な所に出ます。色域外警告（表示 → 色域外警告）は、校正のプロファイルで刷れない色にこの印を重ねます。"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        rows->addRow(QString(), hint);
        look::quiet_labels(rows);
        tabs->addTab(page, QStringLiteral("透明部分・色域"));
        show_checker();
    }
    tabs->addTab(files_page, QStringLiteral("ファイル管理"));
    // 書き出し (PC4 F-c, edit.preferences.export: the same values as ファイル → 書き出し → 書き出しの環境設定…)
    export_form = new ExportPreferencesForm(export_preferences());
    tabs->addTab(export_form, QStringLiteral("書き出し"));
    // Camera Raw (PC4 F-e, edit.preferences.rawDefaults; PhotoCraft's prefs.rs RawDefaults: Adobe RGB, 16 bits, 300 ppi, no
    // sharpening, not a smart object, no automatic tone)
    {
        const camera_raw_open::Workflow flow = camera_raw_open::workflow();
        raw_space = choice_box({{QStringLiteral("Adobe RGB (1998)"), QStringLiteral("adobe-rgb-compat")},
                                {QStringLiteral("sRGB IEC61966-2.1"), QStringLiteral("srgb")},
                                {QStringLiteral("ProPhoto RGB"), QStringLiteral("prophoto-compat")},
                                {QStringLiteral("Display P3"), QStringLiteral("display-p3")}},
                               QString::fromStdString(flow.space));
        raw_depth = choice_box({{QStringLiteral("8 bit/チャンネル"), QStringLiteral("8")}, {QStringLiteral("16 bit/チャンネル"), QStringLiteral("16")}},
                               QString::number(flow.depth));
        raw_resolution = new QSpinBox;
        raw_resolution->setRange(1, 100000);
        raw_resolution->setSuffix(QStringLiteral(" ppi"));
        raw_resolution->setValue(static_cast<int>(std::lround(flow.dpi)));
        raw_sharpen = choice_box({{QStringLiteral("なし"), QStringLiteral("none")},
                                  {QStringLiteral("スクリーン"), QStringLiteral("screen")},
                                  {QStringLiteral("光沢紙"), QStringLiteral("glossy")},
                                  {QStringLiteral("マット紙"), QStringLiteral("matte")}},
                                 QString::fromStdString(flow.sharpen));
        raw_smart = new QCheckBox(QStringLiteral("スマートオブジェクトとして開く"));
        raw_smart->setChecked(flow.smart);
        raw_auto = new QCheckBox(QStringLiteral("自動補正を適用"));
        raw_auto->setChecked(flow.auto_tone);
        raw_auto->setToolTip(QStringLiteral("RAW を開くとき、露光量を「自動」で決めます（覚えた設定のあるファイルは、その設定のまま）"));
        for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{{"raw_space", raw_space}, {"raw_depth", raw_depth},
                                                                                         {"raw_resolution", raw_resolution}, {"raw_sharpen", raw_sharpen},
                                                                                         {"raw_smart", raw_smart}, {"raw_auto", raw_auto}})
            widget->setObjectName(QStringLiteral("prefs_%1").arg(QString::fromLatin1(name)));
        auto* page = new QWidget;
        QFormLayout* rows = look::form();
        page->setLayout(rows);
        rows->addRow(look::section(QStringLiteral("ワークフロー")));
        rows->addRow(QStringLiteral("色空間"), raw_space);
        rows->addRow(QStringLiteral("ビット数"), raw_depth);
        rows->addRow(QStringLiteral("解像度"), raw_resolution);
        rows->addRow(QStringLiteral("出力のシャープ"), raw_sharpen);
        rows->addRow(QString(), raw_smart);
        rows->addRow(look::section(QStringLiteral("既定の画像設定")));
        rows->addRow(QString(), raw_auto);
        auto* hint = new QLabel(QStringLiteral("カメラ RAW を開くときの Camera Raw の窓の、はじめの設定です。Shift を押しながら開くと、窓を出さずにこの設定で開きます。"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        rows->addRow(QString(), hint);
        look::quiet_labels(rows);
        tabs->addTab(page, QStringLiteral("Camera Raw"));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // the pages chosen from a list at the side (as the main window's panels are chosen), not tabs across the top
    tabs->tabBar()->hide();
    tabs->setDocumentMode(true);
    pages_list = new QListWidget;
    pages_list->setObjectName(QStringLiteral("prefsNav"));
    pages_list->setFixedWidth(150);
    int page = 0;
    for (const char* picture : {"search", "pen", "settings", "kind_tone", "open", "export", "kind_placed"}) {
        pages_list->addItem(new QListWidgetItem(icons::icon(picture), tabs->tabText(page++)));
    }
    connect(pages_list, &QListWidget::currentRowChanged, tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (pages_list->currentRow() != i) pages_list->setCurrentRow(i);
    });
    pages_list->setCurrentRow(tabs->currentIndex());
    auto* middle = new QHBoxLayout;
    middle->setSpacing(16);
    middle->addWidget(pages_list);
    middle->addWidget(tabs, 1);
    middle->setContentsMargins(0, 0, 0, 0);
    auto* body = new QWidget;
    body->setLayout(middle);
    look::frame(this, look::header(QStringLiteral("環境設定"), QStringLiteral("このパソコンでの Genko の使い心地です。原稿には残りません。")), body,
                nullptr, look::footer(buttons));
    resize(820, 620);
    show_gamma();
}

void PreferencesDialog::show_checker() {
    const bool custom = checker_colours->currentData().toString() == QLatin1String("custom");
    checker_light->setEnabled(custom);
    checker_dark->setEnabled(custom);
    Checker c;
    c.square = checker_square(checker_size->currentData().toString());
    const auto [light, dark] = checker_pair(checker_colours->currentData().toString(), checker_light->property("colour").value<QColor>(),
                                            checker_dark->property("colour").value<QColor>());
    c.light = light;
    c.dark = dark;
    QImage swatch(checker_preview->size(), QImage::Format_RGB32);
    paint_checker(swatch, c);
    checker_preview->setPixmap(QPixmap::fromImage(swatch));
}

QKeySequenceEdit* PreferencesDialog::editor(const QString& command) const {
    for (const auto& [name, edit] : editors) {
        if (name == command) return edit;
    }
    return nullptr;
}

void PreferencesDialog::show_gamma() {
    if (!gamma) {
        gamma_note->setText(QStringLiteral("筆圧は「ふつう」の曲線です。"));
        return;
    }
    const QString feel = *gamma < 0.9 ? QStringLiteral("やわらかめ") : *gamma > 1.1 ? QStringLiteral("かため") : QStringLiteral("ふつう");
    gamma_note->setText(QStringLiteral("あなたに合わせた曲線（%1、γ=%2）。ブラシの「筆圧」に「自分に合わせた」が出ます。").arg(feel, QString::number(*gamma, 'g', 6)));
}

void PreferencesDialog::measure() {
    try {
        gamma = gamma_for(pad->pressures);
    } catch (const std::invalid_argument& error) {
        gamma_note->setText(QString::fromUtf8(error.what()));
        return;
    }
    show_gamma();
}

void PreferencesDialog::forget_gamma() {
    gamma.reset();
    show_gamma();
}

void PreferencesDialog::reset_keys() {
    for (const auto& [name, edit] : editors) {
        const auto found = set_keys.find(name);
        edit->setKeySequence(found != set_keys.end() && !found->second.isEmpty() ? found->second.front() : QKeySequence());
    }
}

void PreferencesDialog::choose_key_set() {
    // (each command's keys in the set now chosen; a command whose keys are the last set's own takes the new set's — the
    // person's own stay)
    const QString set = key_set->currentData().toString();
    std::map<QString, QList<QKeySequence>> next;
    const auto names = command_names(window_);
    for (QAction* action : preferences::commands(window_)) next[command_name(names, action)] = key_sets::keys_of(action, set);
    if (!set_keys.empty()) {
        for (const auto& [name, edit] : editors) {
            const auto was = set_keys.find(name);
            const QKeySequence old = was != set_keys.end() && !was->second.isEmpty() ? was->second.front() : QKeySequence();
            if (edit->keySequence() != old) continue;
            const auto now = next.find(name);
            edit->setKeySequence(now != next.end() && !now->second.isEmpty() ? now->second.front() : QKeySequence());
        }
    }
    set_keys = std::move(next);
}

void PreferencesDialog::show_pane(const QString& id) {
    QString page = id == QLatin1String("keyboardShortcuts") ? QStringLiteral("ショートカット") : QString();
    for (const Pane& pane : panes()) {
        if (pane.id == id) page = pane.page;
    }
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == page) tabs->setCurrentIndex(i);
    }
}

std::vector<std::pair<QString, QStringList>> PreferencesDialog::mapping() const {
    std::vector<std::pair<QString, QStringList>> out;
    for (const auto& [name, edit] : editors) {
        out.emplace_back(name, edit->keySequence().isEmpty() ? QStringList() : QStringList{edit->keySequence().toString()});
    }
    return out;
}

void PreferencesDialog::pick_backup() {
    const QString folder = ask::existing_dir(this, QStringLiteral("バックアップを残すフォルダ"), backup_folder->text());
    if (!folder.isEmpty()) backup_folder->setText(folder);
}

void PreferencesDialog::save() {
    const auto keys = mapping();
    if (const auto clashes = conflicts(keys); !clashes.empty()) {
        const Clash& first = clashes.front();
        clash->setText(QStringLiteral("「%1」が「%2」と「%3」の両方に付いています。どちらかを変えます。").arg(first.keys, first.first, first.second));
        tabs->setCurrentIndex(0);
        return;
    }
    const auto store = settings();
    // (PC5 H-0: the set's keys are each command's own from now — written only when chosen: the default stays the default)
    if (key_set->currentData().toString() != key_sets::current()) key_sets::set_current(key_set->currentData().toString());
    std::map<QString, QStringList> chosen(keys.begin(), keys.end());
    const auto names = command_names(window_);
    const auto first = by_text(window_);
    for (QAction* action : preferences::commands(window_)) {
        const QString name = command_name(names, action);
        const QStringList now = chosen[name];
        const auto found = set_keys.find(name);
        const QList<QKeySequence> defaults = found != set_keys.end() ? found->second : QList<QKeySequence>();
        const QStringList first_default = defaults.isEmpty() ? QStringList() : QStringList{defaults.front().toString()};
        const QString key = QStringLiteral("shortcuts/%1").arg(name);
        if (now == first_default) {
            store->remove(key);
            // (PC5 H-0: and the keys read under its old names — else the next apply_shortcuts reads them back)
            for (const QString& old : old_keys(window_, action, names, first)) store->remove(old);
            action->setShortcuts(defaults);
        } else {
            store->setValue(key, now.join(QLatin1Char('|')));
        }
    }
    // (PC4 E4: the keys under a reused name are kept by this build's names)
    for (const QString& reused : preferences::reused_names()) preferences::mark_names_current(QStringLiteral("shortcuts/") + reused);
    store->setValue(QStringLiteral("ui/font_pt"), font_pt->value());
    store->setValue(QStringLiteral("ui/ui_font"), ui_font->currentData());
    store->setValue(QStringLiteral("new/paper"), paper->currentData());
    store->setValue(QStringLiteral("save/after_ms"), save_after->value() * 1000);
    store->setValue(QStringLiteral("tablet/gamma"), gamma ? QVariant(*gamma) : QVariant(QString()));
    store->setValue(QStringLiteral("tablet/button"), button->currentData());
    store->setValue(QStringLiteral("ui/theme"), theme->currentData());
    store->setValue(QStringLiteral("ui/brightness"), brightness->value());
    store->setValue(QStringLiteral("ui/surround"), surround->currentData());
    store->setValue(QStringLiteral("ui/mono_icons"), mono_icons->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    theme::set_hints(hints->isChecked());
    store->setValue(QStringLiteral("ui/radial"), radial->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("ui/rest_minutes"), rest->currentData());
    store->setValue(QStringLiteral("ui/raise_requests"), requests->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("ui/gpu_canvas"), gpu->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    if (gpu_memory->value() != static_cast<int>(PageCanvas::gpu_budget_bytes() / (1024 * 1024))) store->setValue(QStringLiteral("ui/gpu_memory_mb"), gpu_memory->value());
    if (motion->isChecked() == system_reduces_motion()) {  // (as the computer is set: keep following it)
        store->remove(QStringLiteral("ui/reduce_motion"));
    } else {
        store->setValue(QStringLiteral("ui/reduce_motion"), motion->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    }
    store->setValue(QStringLiteral("ui/cursor"), cursor->currentData());
    store->setValue(QStringLiteral("ui/cross_drawing"), cross_drawing->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("keys/alt_tool"), alt_tool->currentData());
    store->setValue(QStringLiteral("keys/ctrl_tool"), ctrl_tool->currentData());
    store->setValue(QStringLiteral("keys/hold_swap"), hold_swap->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("backup/folder"), backup_folder->text().trimmed());
    store->setValue(QStringLiteral("backup/minutes"), backup_minutes->value());
    store->setValue(QStringLiteral("backup/keep"), backup_keep->value());
    store->setValue(QStringLiteral("files/recent_count"), recent_count->value());
    store->setValue(QStringLiteral("files/lowercase_extension"), lowercase_extension->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("files/ignore_exif_profile"), ignore_exif_profile->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    set_export_preferences(export_form->value());  // (PC4 F-c: 書き出し)
    {  // (PC4 F-e: Camera Raw)
        camera_raw_open::Workflow flow;
        flow.space = raw_space->currentData().toString().toStdString();
        flow.depth = raw_depth->currentData().toInt();
        flow.dpi = raw_resolution->value();
        flow.sharpen = raw_sharpen->currentData().toString().toStdString();
        flow.smart = raw_smart->isChecked();
        flow.auto_tone = raw_auto->isChecked();
        camera_raw_open::set_workflow(flow);
    }
    store->setValue(QStringLiteral("view/checker_size"), checker_size->currentData());
    store->setValue(QStringLiteral("view/checker_colors"), checker_colours->currentData());
    store->setValue(QStringLiteral("view/checker_light"), checker_light->property("colour").value<QColor>().name());
    store->setValue(QStringLiteral("view/checker_dark"), checker_dark->property("colour").value<QColor>().name());
    store->setValue(QStringLiteral("view/gamut_color"), gamut_button->property("colour").value<QColor>().name());
    store->setValue(QStringLiteral("view/gamut_opacity"), gamut_percent->value());
    for (MainWindow* window : documents::windows()) apply_all(window);  // (this computer's: every window)
    accept();
}

}  // namespace genko::app::preferences
