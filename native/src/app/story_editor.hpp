#pragma once

#include <QDialog>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/json.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

// ストーリーエディター (Python's genko/app/story_editor.py): every line of the book in one table (page, speaker, text,
// balloon), edited together, and a script poured in (流し込み) onto the pages' panels in reading order. And 台詞の
// 検索・置換 (Python's bookview.ReplaceDialog): every line holding the words, a double click to go there, and one step
// to replace them all. Their changes are ops through the window.
//
// Script lines:
//   # 3 / ＃3 / 3ページ / P3        the lines that follow go on page 3 ("---" alone: the next page)
//   話者「台詞」 / 話者：台詞          a speaker and their line
//   （心の声）                         a thought (thought balloon)
//   ナレ：… / N: …                     narration (a box)
//   anything else                      a line without a speaker

namespace genko::app {

class MainWindow;

// One row of the table, or of a script: its line's id (none: a new line), page, speaker, words (as typed) and balloon.
struct ScriptRow {
    std::optional<std::string> id;
    std::int64_t page = 0;
    QString speaker;
    QString text;
    QString balloon = QStringLiteral("speech");
};

// parse_script(text, start_page): [{page, speaker, text, balloon}] from a script.
std::vector<ScriptRow> parse_script(const QString& text, std::int64_t start_page = 1);

// 台本を流し込む
class PourDialog : public QDialog {
    Q_OBJECT
public:
    PourDialog(QWidget* parent, std::int64_t first_page);
    std::vector<ScriptRow> rows() const;

    QPlainTextEdit* text = nullptr;
    QSpinBox* start = nullptr;
    QCheckBox* replace = nullptr;
};

class StoryEditor : public QDialog {
    Q_OBJECT
public:
    explicit StoryEditor(MainWindow* window);

    // The table from the book again (nothing to delete).
    void load();
    ScriptRow row(int r) const;
    std::vector<ScriptRow> rows() const;
    void add_row();
    void delete_rows();
    void move(int delta);
    void pour();
    void pour_rows(const std::vector<ScriptRow>& rows, bool replace = false);
    // The ops that make the book match the table (core::Error when they cannot be made).
    core::Json plan();
    // 原稿に反映
    void apply();

    QTableWidget* table = nullptr;
    QLabel* status = nullptr;
    QPushButton* apply_button = nullptr;
    std::set<std::string> deleted;

private:
    void append(const ScriptRow& row, std::optional<int> at = std::nullopt);
    void count();

    MainWindow* window_ = nullptr;
};

class ReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit ReplaceDialog(MainWindow* window);

    // 探す: the lines holding the words listed; how many places.
    int search();
    // 全部置き換える: one replace_text op.
    void replace_all();
    void go(QListWidgetItem* item);

    QLineEdit* find = nullptr;
    QLineEdit* replace = nullptr;
    QCheckBox* regex = nullptr;
    QCheckBox* speakers = nullptr;
    QListWidget* results = nullptr;
    QLabel* count = nullptr;

private:
    MainWindow* window_ = nullptr;
};

}  // namespace genko::app
