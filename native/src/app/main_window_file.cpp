// PC4 F-0 (SPEC.md FILE-01; 節 F 5.1・5.2, the coordinator's reading of PC3's decision 5): ファイル in Photoshop's order and
// names, and the frames of 節 F's new commands — 指定形式で開く…, すべてを閉じる, その他を閉じる, コピーを保存…, 復帰 (F-b),
// 書き出し ▸ PNG としてクイック書き出し・書き出し形式…・書き出しの環境設定…・Web 用に保存（従来）… (F-c) and ファイル情報… (F-b).
// A frame is a command with its name, key and place that says it is not in this build yet; its group connects it.

#include <QDir>
#include <QMenu>
#include <QMenuBar>

#include "app/dialogs.hpp"
#include "app/main_window.hpp"
#include "core/paths.hpp"

namespace genko::app {

void MainWindow::build_file_actions() {
    const auto later = [this](const char* attribute, const QString& title, const QList<QKeySequence>& keys, const QString& tip) {
        make(QString::fromLatin1(attribute), title, [this, title] { flash(QStringLiteral("「%1」は、この版の Genko ではまだ使えません").arg(title)); }, keys, tip);
    };
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    later("act_open_as", QStringLiteral("指定形式で開く…"), key("Ctrl+Alt+Shift+O"), QStringLiteral("ファイルを、選んだ形式のものとして開きます"));
    later("act_close_all", QStringLiteral("すべてを閉じる"), key("Ctrl+Alt+W"), QStringLiteral("開いている原稿をすべて閉じます"));
    later("act_close_others", QStringLiteral("その他を閉じる"), key("Ctrl+Alt+P"), QStringLiteral("この原稿のほかの原稿を閉じます"));
    later("act_save_copy", QStringLiteral("コピーを保存…"), key("Ctrl+Alt+S"),
          QStringLiteral("原稿はそのままに、別のファイル（.genko・PSD・TIFF・PNG・JPEG など）に書きます"));
    later("act_revert", QStringLiteral("復帰"), key("F12"), QStringLiteral("最後に保存したとき（無ければ開いたとき）の状態に戻します（取り消せます）"));
    later("act_quick_export_png", QStringLiteral("PNG としてクイック書き出し"), {}, QStringLiteral("このページを、書き出しの環境設定のとおり PNG に書き出します"));
    later("act_export_as", QStringLiteral("書き出し形式…"), key("Ctrl+Alt+Shift+W"),
          QStringLiteral("このページを、形式・大きさ・画質を選んで 1 枚の画像に書き出します"));
    later("act_export_prefs", QStringLiteral("書き出しの環境設定…"), {}, QStringLiteral("クイック書き出しの形式・保存先・画質・メタデータ"));
    later("act_save_for_web", QStringLiteral("Web 用に保存（従来）…"), key("Ctrl+Alt+Shift+S"),
          QStringLiteral("色数や画質を見比べて、Web 用に小さく書き出します"));
    later("act_file_info", QStringLiteral("ファイル情報…"), key("Ctrl+Alt+Shift+I"), QStringLiteral("作成者・説明・キーワード・著作権情報"));
}

void MainWindow::build_file_menu(QMenu* file) {
    for (const char* name : {"act_new", "act_open", "act_open_as"}) file->addAction(action(name));
    recent_menu_ = file->addMenu(QStringLiteral("最近使用したファイルを開く"));
    recent_menu_->setProperty("remade", true);
    connect(recent_menu_, &QMenu::aboutToShow, this, [this] {
        recent_menu_->clear();
        const auto paths = recent_projects();
        if (paths.empty()) {
            recent_menu_->addAction(QStringLiteral("（まだありません）"))->setEnabled(false);
            return;
        }
        for (std::size_t i = 0; i < paths.size() && i < 12; ++i) {
            const auto path = paths[i];
            QAction* item = recent_menu_->addAction(QString::fromStdString(core::path_to_utf8(path.stem())), this, [this, path] { open_project(path); });
            item->setToolTip(QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path))));  // (the whole path)
        }
    });
    file->addSeparator();
    file->addAction(action("act_ai"));
    file->addSeparator();
    for (const char* name : {"act_close", "act_close_all", "act_close_others"}) file->addAction(action(name));
    file->addSeparator();
    for (const char* name : {"act_save", "act_save_as", "act_save_copy", "act_revert"}) file->addAction(action(name));
    file->addSeparator();
    // 書き出し ▸ (Photoshop's; 原稿を書き出す… is Genko's own book export, Ctrl+E; カラールックアップテーブル… is put last by
    // レイヤー → 色調補正's group, app/adjust_forms_tone.cpp)
    export_menu_ = file->addMenu(QStringLiteral("書き出し"));
    export_menu_->setObjectName(QStringLiteral("menu_export"));
    for (const char* name : {"act_quick_export_png", "act_export_as"}) export_menu_->addAction(action(name));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_export_prefs"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_save_for_web"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_export"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_export_paths_ai"));
    file->addSeparator();
    for (const char* name : {"act_place_embedded", "act_place_linked", "act_package"}) file->addAction(action(name));
    file->addSeparator();
    for (const char* name : {"act_import", "act_import_scan", "act_scanner", "act_merge_book", "act_import_psd"}) file->addAction(action(name));
    file->addSeparator();
    file->addAction(action("act_file_info"));
    file->addSeparator();
    file->addAction(action("act_print"));
    file->addSeparator();
    for (const char* name : {"act_timelapse", "act_timelapse_export"}) file->addAction(action(name));
    file->addSeparator();
    file->addMenu(QStringLiteral("スクリプト"))->addAction(action("act_flatten_masks"));  // (PC3 B2: file.scripts.*)
    actions_menu_ = file->addMenu(QStringLiteral("オートアクション"));
    actions_menu_->setProperty("remade", true);
    connect(actions_menu_, &QMenu::aboutToShow, this, &MainWindow::fill_actions_menu);
    fill_actions_menu();
    file->addAction(action("act_prefs"));
    file->addSeparator();
    file->addAction(action("act_quit"));
}

}  // namespace genko::app
