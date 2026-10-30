#pragma once

#include <QDialog>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "service/spell.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

// PC5 G-d (SPEC.md TEXT-02「スペルチェック」「書体の置き換え」): 編集 → スペルチェック… and 書式 → 見つからないフォントを解決…,
// after PhotoCraft v0.2.0's ui-egui/src/type_panels_ui.rs (the spelling window, MIT).

namespace genko::app {

class MainWindow;

// スペルチェック: one finding at a time — a word not in the dictionary (or, 表記の点検, one the notation table says to write
// otherwise) and the line or type layer it is in (the page shown, the text chosen) —: 無視, すべて無視, 変更 (edit_type at its
// place, the word checked to be still there), すべて変更 (every place of the word, one step), 辞書に追加 (this computer's
// user dictionary), 表記の表… (the notation table). すべてのレイヤーを確認 off: the page shown alone; 効果音も: lines in
// sound-effect balloons too.
class SpellingDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpellingDialog(MainWindow* window);
    // The findings looked for again (the dictionary, the table and the options as they are now); the first one shown.
    void check();
    void ignore();
    void ignore_all();
    void change();
    void change_all();
    void add_to_dictionary();
    // The finding shown (none: done).
    const service::spell::Finding* current() const;

    QLineEdit* word = nullptr;     // (not in the dictionary — as found)
    QLineEdit* change_to = nullptr;
    QListWidget* suggestions = nullptr;
    QLabel* where = nullptr;
    QCheckBox* all_layers = nullptr;
    QCheckBox* sfx = nullptr;
    QPushButton* ignore_button = nullptr;
    QPushButton* ignore_all_button = nullptr;
    QPushButton* change_button = nullptr;
    QPushButton* change_all_button = nullptr;
    QPushButton* add_button = nullptr;
    QPushButton* table_button = nullptr;

private:
    void show_current();
    void step();

    MainWindow* window_;
    std::vector<service::spell::Finding> findings_;
    std::size_t at_ = 0;
    std::set<std::string> ignored_;  // (すべて無視, lowercase, for this check)
    std::map<std::string, std::vector<std::string>> suggested_;  // (kind and word → the suggestions shown, made once)
};

// 表記の表…: the notation table (this computer's settings, type/notations) — 誤 → 正, a row each.
class NotationDialog : public QDialog {
    Q_OBJECT
public:
    // (`rows`: the table to show — 環境設定's own while it is open —; none: the settings')
    explicit NotationDialog(QWidget* parent, const std::optional<std::vector<service::spell::Notation>>& rows = std::nullopt);
    // The rows as the table has them now (checked as the settings take them); saved (OK) to the settings.
    std::vector<service::spell::Notation> rows() const;
    void save();
    QTableWidget* table = nullptr;
};

// 見つからないフォントを解決…: the fonts the book's texts name that this computer does not have (replace_fonts as a
// question), each with the font to put in its place (a bundled one, a font of this computer — the one of the same
// PostScript name or family first —, or いつもの書体: the default) — OK replaces them (one replace_fonts {map}).
class MissingFontsDialog : public QDialog {
    Q_OBJECT
public:
    MissingFontsDialog(MainWindow* window, const core::Json& missing);
    // The map replace_fonts takes ({missing: font | {font, face_index, family, font_style, postscript} | null}).
    core::Json map() const;
    QTableWidget* table = nullptr;
    std::vector<QComboBox*> choices;

private:
    core::Json missing_;
};

}  // namespace genko::app
