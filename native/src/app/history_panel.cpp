// 履歴 (Python's genko/app/history.py): HistoryPanel, describe, timeline (Session::history) and _when.

#include "app/history_panel.hpp"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

using core::Json;

namespace history {

namespace {

// history.NAMES
const std::map<std::string, const char*>& names() {
    static const std::map<std::string, const char*> table = {
        {"add_stroke", "ペンで描いた"}, {"erase", "消しゴムで消した"}, {"fill", "塗りつぶした"}, {"fill_area", "囲って塗った"},
        {"fill_enclosed", "囲った中の閉じた所を塗った"}, {"delete_area", "範囲を消した"}, {"transform_area", "範囲を動かした・変形した"},
        {"paste", "貼り付けた"}, {"set_stroke_width", "線の太さを変えた"}, {"reshape_stroke", "線を修正した"}, {"split_frame", "コマを割った"},
        {"cut_frame", "コマを割った"}, {"merge_frame", "コマを結合した"}, {"add_frame", "コマを描いた"}, {"delete_frame", "コマを消した"},
        {"move_gutter", "コマの間を動かした"}, {"set_frame", "コマの形を変えた"}, {"apply_template", "テンプレートでコマを割った"},
        {"set_border", "枠線を変えた"}, {"add_line", "台詞を入れた"}, {"edit_line", "台詞を直した"}, {"move_line", "フキダシを動かした"},
        {"cut_balloon", "フキダシを削った"}, {"delete_line", "台詞を消した"}, {"reorder_lines", "台詞の順番を変えた"},
        {"set_balloon_path", "フキダシの形を描いた"}, {"add_layer", "レイヤーを足した"}, {"delete_layer", "レイヤーを消した"},
        {"set_layer", "レイヤーの設定を変えた"}, {"reorder_layers", "レイヤーの順番を変えた"}, {"duplicate_layer", "レイヤーを複製した"},
        {"merge_down", "レイヤーを結合した"}, {"set_layer_mask", "マスクを変えた"}, {"paint_mask", "マスクを描いた"},
        {"filter_raster", "フィルターをかけた"}, {"add_page", "ページを足した"}, {"delete_page", "ページを消した"},
        {"duplicate_page", "ページを複製した"}, {"import_pages", "ほかの原稿のページを取り込んだ"}, {"move_page", "ページを並べ替えた"},
        {"set_spread", "見開きを変えた"}, {"set_page_spec", "原稿用紙を変えた"}, {"set_nombre", "ノンブルを変えた"}, {"add_tone", "トーンを貼った"},
        {"set_tone", "トーンを変えた"}, {"add_effect", "効果線を入れた"}, {"edit_effect", "効果線を変えた"}, {"delete_effect", "効果線を消した"},
        {"effect_to_layer", "効果線を線にした"}, {"stamp_material", "素材を置いた"}, {"add_ruler", "定規を置いた"},
        {"ruler_from_3d", "3D からパース定規を作った"}, {"camera_from_ruler", "カメラをパース定規に合わせた"}, {"edit_ruler", "定規を動かした"},
        {"delete_ruler", "定規を消した"}, {"add_prim3d", "3D を置いた"}, {"add_scene", "背景の 3D を置いた"}, {"add_mannequin", "デッサン人形を置いた"},
        {"edit_prim", "3D を動かした"}, {"pose_mannequin", "ポーズを変えた"}, {"delete_prim", "3D を消した"}, {"trace_prims", "3D を線にした"},
        {"import_raster", "画像を読み込んだ"}, {"place_image", "画像を置いた"}, {"merge_layers", "レイヤーを結合した"},
        {"merge_visible", "表示レイヤーを結合した"}, {"group_layers", "レイヤーをフォルダにまとめた"}, {"move_layers", "レイヤーを動かした"},
        {"convert_layer", "レイヤーを変換した"}, {"set_layers", "レイヤーの設定をまとめて変えた"}, {"set_paper", "用紙の色を変えた"},
        {"liquify", "ゆがませた"}, {"smudge", "色を混ぜた"}, {"vector_edit", "線を編集した"}, {"trace_edit", "線をなぞって直した"},
        {"fill_gaps", "塗り残しを埋めた"}, {"add_shape", "図形を描いた"}, {"ruler_to_layer", "定規の線を描いた"}, {"add_figure", "デッサン人形を置いた"},
        {"pose_figure", "ポーズを変えた"}, {"add_head", "頭部を置いた"}, {"add_hand", "手を置いた"}, {"import_model", "3D モデルを読み込んだ"},
        {"set_camera", "カメラを動かした"}, {"set_light", "光の向きを変えた"}, {"render_prims", "3D を線と面にした"}, {"add_cover", "表紙を足した"},
        {"import_psd", "PSD を読み込んだ"}, {"set_timelapse", "タイムラプスを切り替えた"}, {"set_animation", "アニメーションを設定した"},
        {"add_anim_folder", "アニメーションフォルダーを足した"}, {"add_cel", "セルを足した"}, {"set_exposure", "タイムラインを直した"},
        {"set_exposures", "タイムラインを直した"}, {"set_camera_key", "カメラワークを直した"}, {"set_light_table", "ライトテーブルを直した"},
        {"replace_text", "台詞を置き換えた"}, {"set_assignee", "担当を決めた"}, {"define_brush", "ブラシを作った"}, {"approve", "承認した"},
        {"allow_chat_approval", "チャットでの承認を切り替えた"}, {"reject", "差し戻した"}, {"advance", "工程を進めた"},
    };
    return table;
}

}  // namespace

QString describe(const Json& ops) {
    QStringList named;
    if (ops.is_array()) {
        for (const Json& op : ops) {
            // NAMES.get(str(op.get("op")), "原稿を変えた")
            const Json* kind = op.is_object() && op.contains("op") ? &op["op"] : nullptr;
            const std::string key = kind != nullptr ? core::py_str(*kind) : std::string("None");
            const auto it = names().find(key);
            named << (it != names().end() ? QString::fromUtf8(it->second) : QStringLiteral("原稿を変えた"));
        }
    }
    if (named.isEmpty()) return QStringLiteral("原稿を変えた");
    const QString first = named.front();
    const bool same = std::all_of(named.begin(), named.end(), [&](const QString& n) { return n == first; });
    if (named.size() > 1 && same) return QStringLiteral("%1（%2 回）").arg(first).arg(named.size());
    if (named.size() > 1) return QStringLiteral("%1 ほか %2 件").arg(first).arg(named.size() - 1);
    return first;
}

QString when(std::optional<double> at) {
    if (!at || *at == 0.0 || !std::isfinite(*at)) return {};
    const QDateTime moment = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(std::llround(*at * 1000.0)));
    const bool today = QDate::currentDate() == moment.date();
    // (the wide space stays out of the format, as Python's)
    return QStringLiteral("　") + moment.toString(today ? QStringLiteral("HH:mm") : QStringLiteral("MM/dd HH:mm"));
}

}  // namespace history

HistoryPanel::HistoryPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("history_panel"));
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("history_list"));
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { go_to(item->data(Qt::UserRole).toInt()); });
    auto* note = new QLabel(QStringLiteral("クリックで、その変更をした直後まで戻る（進む）。灰色は取り消した変更（やり直せる）。"));
    note->setWordWrap(true);
    theme::hint(note);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addWidget(note);
}

void HistoryPanel::refresh() {
    const Session::History timeline = window_->session().history();
    done_ = static_cast<int>(timeline.done.size());
    list_->clear();
    auto* start = new QListWidgetItem(QStringLiteral("（はじめ）"));
    start->setData(Qt::UserRole, 0);
    list_->addItem(start);
    const std::string& me = window_->session().actor();
    int n = 0;
    for (const Session::HistoryEntry& entry : timeline.done) {
        ++n;
        const std::string actor = entry.actor.is_null() ? std::string() : core::py_str(entry.actor);
        QString who;
        if (!actor.empty() && actor != me) {
            const std::size_t colon = actor.rfind(':');
            who = QStringLiteral("　〔%1〕").arg(QString::fromStdString(colon == std::string::npos ? actor : actor.substr(colon + 1)));
        }
        auto* item = new QListWidgetItem(history::describe(entry.ops) + who + history::when(entry.at));
        item->setData(Qt::UserRole, n);
        list_->addItem(item);
    }
    for (const Session::HistoryEntry& entry : timeline.later) {
        ++n;
        auto* item = new QListWidgetItem(history::describe(entry.ops) + QStringLiteral("（戻した操作）") + history::when(entry.at));
        item->setForeground(QColor(theme::tokens().faint));
        item->setData(Qt::UserRole, n);
        list_->addItem(item);
    }
    QListWidgetItem* now = list_->item(done_);
    now->setText(QStringLiteral("▶ ") + now->text());
    QFont font = now->font();
    font.setBold(true);
    now->setFont(font);
    list_->setCurrentRow(done_);
    list_->scrollToItem(now);
}

void HistoryPanel::go_to(int target) {
    Session& session = window_->session();
    int steps = 0;
    try {
        while (done_ > target) {
            session.undo();
            --done_;
            ++steps;
        }
        while (done_ < target) {
            session.redo();
            ++done_;
            ++steps;
        }
    } catch (const core::Error& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 5000, true);
    }
    if (steps > 0) {
        window_->watch();
        window_->reload_pages();
    }
    refresh();
}

}  // namespace genko::app
