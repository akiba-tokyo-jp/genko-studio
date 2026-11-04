#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

#include "service/automate.hpp"

class QCheckBox;
class QComboBox;
class QListWidget;
class QSpinBox;

// The windows of ファイル → 自動処理 → Photomerge… and スクリプト → ファイルをスタックとして読み込み…・統計… (PC5 I-g, SPEC.md
// AUTO-01; Photoshop's). Each only asks: what it chose is read back by app/main_window_photomerge.cpp, which makes the new book
// through service/automate on a worker. Their widgets are public for the GUI tests.

namespace genko::app {

// ソースファイル: 使用 ファイル | フォルダー, the list (参照…, 削除, 開いているファイルを追加).
class SourceFiles {
public:
    QComboBox* use = nullptr;      // data "files" | "folder"
    QListWidget* list = nullptr;   // (the files, a folder, or "open:<title>" for an open book's page: its item's data)
    QStringList open_books;        // (the titles 開いているファイルを追加 adds)
    // The files (a folder's pictures, sorted) and the open books chosen (their indices in open_books).
    std::vector<std::filesystem::path> files() const;
    std::vector<int> open_chosen() const;
    int count() const;
};

// Photomerge… (file.automate.photomerge)
class PhotomergeDialog : public QDialog {
    Q_OBJECT
public:
    PhotomergeDialog(QWidget* parent, const QStringList& open_books);
    QComboBox* layout = nullptr;           // data: auto | perspective | cylindrical | spherical | collage | reposition
    SourceFiles source;
    QCheckBox* blend = nullptr;            // 画像を合成
    QCheckBox* vignette = nullptr;         // 周辺光量補正
    QCheckBox* geometric = nullptr;        // 幾何学的歪曲の補正
    QCheckBox* fill = nullptr;             // コンテンツに応じた塗りつぶしを透明な領域に適用
    QSpinBox* scale = nullptr;             // 読み込むときに縮小 (%: Genko's own)
    service::automate::PhotomergeOptions options() const;
    std::optional<QString> problem() const;
};

// ファイルをスタックとして読み込み… (file.scripts.loadFilesIntoStack) and 統計… (file.scripts.statistics: `statistics`)
class StackDialog : public QDialog {
    Q_OBJECT
public:
    StackDialog(QWidget* parent, const QStringList& open_books, bool statistics);
    QComboBox* mode = nullptr;             // (統計…) スタックモードを選択: data a stack mode
    SourceFiles source;
    QCheckBox* align = nullptr;            // ソース画像を自動的に位置合わせ
    QCheckBox* smart = nullptr;            // (読み込み…) 読み込み後にスマートオブジェクトを作成
    std::optional<QString> problem() const;
};

}  // namespace genko::app
