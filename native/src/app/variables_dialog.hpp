#pragma once

#include <QDialog>

#include <memory>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/variables.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;

// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」): the windows of イメージ → 変数 ▸ 定義…・データセット…, イメージ →
// データセットを適用…, ファイル → 書き出し → データセットからファイル… and ファイル → 読み込み → 変数データセット… (Photoshop's names;
// app/main_window_variables.cpp runs them and applies their ops). Each dialog reads the book given and gives back what its op
// takes; it changes nothing itself.

namespace genko::app {

// 変数の定義…: the book's variables, a row each (名前・種類・対象・方法・配置・クリップ), made from a layer or a line of the
// page shown (追加) or taken away (削除). defs() is set_variables's defs.
class VariablesDialog : public QDialog {
    Q_OBJECT
public:
    VariablesDialog(QWidget* parent, const core::Document& doc, const core::Page& page);
    core::Json defs() const;

    QTableWidget* table = nullptr;
    QComboBox* target = nullptr;   // (the page's layers, then its lines)
    QComboBox* type = nullptr;     // visibility | text | pixels
    QLineEdit* name = nullptr;
    QComboBox* method = nullptr;
    QComboBox* align = nullptr;
    QCheckBox* clip = nullptr;
    QPushButton* add = nullptr;
    QPushButton* remove = nullptr;
    QDialogButtonBox* buttons = nullptr;

    // 追加: a def of the target and type chosen, named as the name says (a free name when it is empty); one of that name
    // replaced. False (said) when the target does not take the type.
    bool add_chosen();

private:
    void fill_table();
    const core::Document& doc_;
    const core::Page& page_;        // (the page the window was opened on: its layers the targets — the review's P3-4)
    std::vector<core::Json> defs_;  // (set_variables's form, the page's number for a layer)
};

// データセット…: the book's data sets — one shown (前へ・次へ, its name), its values a row a variable (表示: a check box;
// テキストの置き換え: its words; ピクセルの置き換え: a picture's file chosen, or the one it has) —, 新規 and 削除. data_sets() is
// set_data_sets's data_sets (the pictures chosen given as their bytes, those kept as the book's assets).
class DataSetsDialog : public QDialog {
    Q_OBJECT
public:
    DataSetsDialog(QWidget* parent, const core::Document& doc);
    core::Json data_sets() const;

    QComboBox* sets = nullptr;
    QLineEdit* name = nullptr;
    QPushButton* previous = nullptr;
    QPushButton* next = nullptr;
    QPushButton* add = nullptr;
    QPushButton* remove = nullptr;
    QTableWidget* values = nullptr;
    QDialogButtonBox* buttons = nullptr;

    // A picture for a pixels variable of the data set shown (its bytes read from the file; false: it cannot be read).
    bool set_picture(const QString& variable, const QString& file);
    // 新規: a data set of no values, named データセット N; shown.
    void add_set();
    void remove_set();

private:
    void show_set(int index);
    void keep_shown();
    const core::Variables variables_;
    struct Set {
        std::string name;
        std::vector<std::pair<std::string, core::Json>> values;  // (variable, set_data_sets's value)
    };
    std::vector<Set> sets_;
    int shown_ = -1;
};

// データセットを適用…: the data sets listed, the page drawn with the one chosen (a copy of the book, small) — chosen() its index.
class ApplyDataSetDialog : public QDialog {
    Q_OBJECT
public:
    ApplyDataSetDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page, const std::string& actor);
    int chosen() const;

    QListWidget* list = nullptr;
    QLabel* preview = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void draw();
    std::shared_ptr<const core::Document> doc_;
    std::size_t page_ = 0;
    std::string actor_;
};

// データセットからファイル…: where, the file's format and quality, how the files are named, which data sets.
class DataSetsToFilesDialog : public QDialog {
    Q_OBJECT
public:
    DataSetsToFilesDialog(QWidget* parent, const QString& folder, const core::Document& doc);
    QLineEdit* folder = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QLineEdit* naming = nullptr;
    QComboBox* which = nullptr;  // (すべてのデータセット, then each)
    QDialogButtonBox* buttons = nullptr;
};

// 変数データセットを読み込み: the text file, its delimiter, whether its data sets replace the book's.
class ImportDataSetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImportDataSetsDialog(QWidget* parent);
    QLineEdit* file = nullptr;
    QComboBox* delimiter = nullptr;  // (自動・コンマ・タブ: 0, ',', '\t')
    QCheckBox* replace = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

// The words a variable's type is shown with (Photoshop's 表示・テキストの置き換え・ピクセルの置き換え), and a method's and
// an alignment's.
QString variable_type_name(const std::string& type);
QString variable_method_name(const std::string& method);
QString variable_align_name(const std::string& align);

}  // namespace genko::app
