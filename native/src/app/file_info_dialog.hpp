#pragma once

#include <QDialog>

#include "core/json.hpp"
#include "core/model.hpp"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

// ファイル情報… (PC4 F-b, file.fileInfo; 節 F 5.3; Photoshop's File Info, PhotoCraft's file_cmds.rs): 基本 — ドキュメント
// のタイトル (the book's title), 作成者, 作成者の役職, 説明, キーワード (; between them), 著作権情報 (不明・著作権あり・
// パブリックドメイン), 著作権表示, 著作権情報 URL —; カメラデータ and 元のファイル of the page in front (read only: what it was
// opened from, native_source). OK gives the set_file_info op of what changed (one change, undone by 取り消し).

namespace genko::app {

class FileInfoDialog : public QDialog {
    Q_OBJECT

public:
    FileInfoDialog(QWidget* parent, const core::Document& doc, const core::Page* page);
    // The op of what was changed ({"op": "set_file_info", …}); none when nothing was.
    std::optional<core::Json> op() const;

    QLineEdit* title = nullptr;
    QLineEdit* author = nullptr;
    QLineEdit* author_title = nullptr;
    QPlainTextEdit* description = nullptr;
    QLineEdit* keywords = nullptr;
    QComboBox* copyright_status = nullptr;
    QPlainTextEdit* copyright = nullptr;
    QLineEdit* copyright_url = nullptr;
    QLabel* source = nullptr;  // 元のファイル
    QLabel* camera = nullptr;  // カメラデータ
    QDialogButtonBox* buttons = nullptr;

private:
    core::Json values() const;  // (every item as the op takes it)
    core::Json was_;
};

}  // namespace genko::app
