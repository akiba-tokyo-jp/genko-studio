// The production's commands (PC5 節 J, SPEC.md PROD-01「制作の補助」): the frames J-0 puts in Photoshop's places — ページ
// (Photoshop's イメージ) → 解析 ▸, レイヤー → ビデオレイヤー ▸, ウィンドウ's panels アクション・レイヤーカンプ・計測ログ・注釈 —,
// each saying it is not in this build yet until its group writes it (J-a レイヤーカンプ, J-c 注釈, J-d1/J-d2 計測,
// J-e2 ビデオレイヤー, J-g アクション). A group makes a frame real by taking its rows out of the tables below and making
// the command in its own build_* under the same attribute (its menus here keep taking it by action()); a panel's row goes
// for its dock's own toggle in build_production_docks. (make() puts a command made again under an attribute in the old
// one's places and lets the old one go, so a row left behind never answers in its place — whichever is made first.)
// Keys: none here. 計測値を記録 is Photoshop's Shift+Ctrl+M, which is コマを結合（割る前に戻す）'s (act_merge) in both key sets
// (H-0's key_sets): J-d2 gives it only in the Photoshop set and only with act_merge's key there moved (a decision to ask);
// アクション's Alt+F9 is free in both.

#include <QAction>
#include <QMenu>

#include <algorithm>
#include <iterator>
#include <span>
#include <string_view>

#include "app/main_window.hpp"

namespace genko::app {

namespace {

struct Frame {
    const char* attribute;
    const char* title;
    const char* tip;
};

// イメージ → 解析 ▸ (J-d1・J-d2), in Photoshop's order
constexpr Frame kAnalysis[] = {
    {"act_measure_scale_default", "初期設定", "計測スケールを 1 pixel = 1 pixel に戻します"},
    {"act_measure_scale_custom", "カスタム…", "計測スケール（ピクセルの長さ・論理的な長さ・単位）を決めます"},
    {"act_measure_data_points", "カスタム…", "計測値を記録するときに出す項目を選びます"},
    {"act_measure_record", "計測値を記録", "選択範囲・ものさし・カウントの計測値を計測ログに記録します"},
    {"act_measure_ruler_tool", "ものさしツール", "2 点の距離と角度を計ります"},
    {"act_measure_count_tool", "カウントツール", "クリックした所に番号の印を付けて数えます"},
    {"act_measure_scale_marker", "目盛りマーカーを配置…", "計測スケールの目盛りをページに置きます"},
};

// レイヤー → ビデオレイヤー ▸ (J-e2), in Photoshop's order
constexpr Frame kVideo[] = {
    {"act_video_from_file", "ファイルから新規ビデオレイヤー…", "連番の画像・動く画像（GIF・APNG・WebP）をビデオレイヤーにします"},
    {"act_video_blank", "新規空白ビデオレイヤー", "空のビデオレイヤーを足します"},
    {"act_video_insert_blank", "空白フレームを挿入", "今のフレームに空白のフレームを差し込みます"},
    {"act_video_duplicate", "フレームを複製", "今のフレームを複製します"},
    {"act_video_delete", "フレームを削除", "今のフレームを削除して後ろを詰めます"},
    {"act_video_replace", "フッテージを置き換え…", "ビデオレイヤーの素材を別のものに替えます"},
    {"act_video_interpret", "フッテージを解釈…", "素材のフレームレート・アルファチャンネル・ピクセル縦横比を決めます"},
    {"act_video_show_altered", "変更されたビデオを表示", "描き替えたフレームを見せるか、素材のまま見せるかを切り替えます"},
    {"act_video_restore", "フレームを復元", "今のフレームの描き替えを外して素材に戻します"},
    {"act_video_restore_all", "すべてのフレームを復元", "ビデオレイヤーの描き替えをすべて外します"},
    {"act_video_reload", "フレームを再読み込み", "素材のファイルを読み直します"},
    {"act_video_rasterize", "ラスタライズ", "今のフレームの見えをふつうのペイントレイヤーにします"},
};

// ウィンドウ's panels (J-g アクション, J-d2 計測ログ, J-c 注釈; タイムライン is the dock's; J-a's レイヤーカンプ is made by
// main_window_layer_comps.cpp, after アクション)
constexpr Frame kPanels[] = {
    {"act_panel_actions", "アクション", "記録した操作の手順を並べ、再生・記録します"},
    {"act_panel_measurement_log", "計測ログ", "記録した計測値の表"},
    {"act_panel_notes", "注釈", "ページに置いた注釈の一覧"},
};

}  // namespace

void MainWindow::build_production_actions() {
    for (const std::span<const Frame> list : {std::span<const Frame>(kAnalysis), std::span<const Frame>(kVideo), std::span<const Frame>(kPanels)}) {
        for (const Frame& frame : list) {
            const QString title = QString::fromUtf8(frame.title);
            make(QString::fromLatin1(frame.attribute), title, [this, title] { not_yet_command(title); }, {}, QString::fromUtf8(frame.tip));
        }
    }
    build_layer_comp_actions();  // (PC5 J-a: レイヤーカンプ, レイヤーカンプからファイル…)
}

void MainWindow::build_production_menus(QMenu* layers, QMenu* pages) {
    // レイヤー → ビデオレイヤー ▸: before ラスタライズ ▸, as Photoshop's
    auto* video = new QMenu(QStringLiteral("ビデオレイヤー"), layers);
    video->setObjectName(QStringLiteral("menu_video_layers"));
    for (const char* name : {"act_video_from_file", "act_video_blank"}) video->addAction(action(QString::fromLatin1(name)));
    video->addSeparator();
    for (const char* name : {"act_video_insert_blank", "act_video_duplicate", "act_video_delete"}) video->addAction(action(QString::fromLatin1(name)));
    video->addSeparator();
    for (const char* name : {"act_video_replace", "act_video_interpret"}) video->addAction(action(QString::fromLatin1(name)));
    video->addSeparator();
    for (const char* name : {"act_video_show_altered", "act_video_restore", "act_video_restore_all", "act_video_reload"}) video->addAction(action(QString::fromLatin1(name)));
    video->addSeparator();
    video->addAction(action(QStringLiteral("act_video_rasterize")));
    QAction* before = nullptr;
    for (QAction* item : layers->actions()) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_rasterize")) before = item;
    }
    if (before != nullptr) {
        layers->insertMenu(before, video);
    } else {
        layers->addMenu(video);
    }
    // ページ (Photoshop's イメージ) → 解析 ▸: after 複製… (Photoshop's 画像操作… · 演算… · 変数 · トラッピング go between, 解析 the
    // last of them), before 台詞's commands
    auto* analysis = new QMenu(QStringLiteral("解析"), pages);
    analysis->setObjectName(QStringLiteral("menu_analysis"));
    const QList<QAction*> in_pages = pages->actions();
    const auto duplicate = std::find(in_pages.begin(), in_pages.end(), action(QStringLiteral("act_duplicate_image")));
    if (duplicate != in_pages.end() && std::next(duplicate) != in_pages.end()) {
        pages->insertMenu(*std::next(duplicate), analysis);
    } else {
        pages->addMenu(analysis);
    }
    QMenu* scale = analysis->addMenu(QStringLiteral("計測スケールを設定"));
    scale->setObjectName(QStringLiteral("menu_measure_scale"));
    for (const char* name : {"act_measure_scale_default", "act_measure_scale_custom"}) scale->addAction(action(QString::fromLatin1(name)));
    QMenu* points = analysis->addMenu(QStringLiteral("データポイントを選択"));
    points->setObjectName(QStringLiteral("menu_measure_points"));
    points->addAction(action(QStringLiteral("act_measure_data_points")));
    analysis->addAction(action(QStringLiteral("act_measure_record")));
    analysis->addSeparator();
    for (const char* name : {"act_measure_ruler_tool", "act_measure_count_tool"}) analysis->addAction(action(QString::fromLatin1(name)));
    analysis->addSeparator();
    analysis->addAction(action(QStringLiteral("act_measure_scale_marker")));
    // (PC5 J-a) ファイル → 書き出し → レイヤーカンプからファイル…: after レイヤーからファイル…, as Photoshop's
    if (export_menu_ != nullptr) {
        const QList<QAction*> exports = export_menu_->actions();
        const auto layers_files = std::find(exports.begin(), exports.end(), action(QStringLiteral("act_layers_to_files")));
        if (layers_files != exports.end() && std::next(layers_files) != exports.end()) {
            export_menu_->insertAction(*std::next(layers_files), action(QStringLiteral("act_comps_to_files")));
        } else {
            export_menu_->addAction(action(QStringLiteral("act_comps_to_files")));
        }
    }
}

void MainWindow::build_production_docks() {
    // (each group's dock in its frame's place: its toggleViewAction() into view_menu_ instead of the frame)
    for (const Frame& frame : kPanels) {
        view_menu_->addAction(action(QString::fromLatin1(frame.attribute)));
        if (std::string_view(frame.attribute) == "act_panel_actions") build_layer_comps_dock();  // (PC5 J-a: レイヤーカンプ)
    }
}

void MainWindow::not_yet_command(const QString& name) { flash(QStringLiteral("「%1」は、この版ではまだ使えません").arg(name), 4000, true); }

}  // namespace genko::app
