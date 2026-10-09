#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/session.hpp"
#include "core/json.hpp"
#include "formats/exporting.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSpinBox;

// 書き出し… (Python's genko/app/dialogs.py ExportDialog): a format (formats/exporting: the table of formats and their
// options), the pages (all, the page in front, its spread, or a range), only the options the format takes, a folder;
// the first page to be written shown small, cut as it will be. 書き出す runs 入稿前の点検 first (formats/checks: what
// stops a print is asked about — write anyway, fix it, or stop), then formats::run, then says how many files it wrote
// (with a button that opens the folder) or why it could not, in Japanese. "正式な書き出し" (official: the studio's
// checked export) is refused by this build (formats::run says so).

namespace genko::app {

// The CMYK profile chosen last (dialogs.icc_setting: settings color/icc, while its file is there), and keep one.
QString icc_setting();
void set_icc_setting(const QString& path);

class ExportDialog : public QDialog {
    Q_OBJECT

public:
    // The book as it is now (the window's), its folder (none: never saved) and who exports it.
    ExportDialog(QWidget* parent, DocPtr episode, std::optional<std::filesystem::path> project, std::string actor, bool official = false,
                 std::int64_t current_page = 1);

    // The page numbers to write: core::PyValueError (Python's words) when the range cannot be read.
    std::vector<std::int64_t> pages() const;
    // The options of the format (ExportDialog.options) for formats::run, with the pages and whether it is official.
    formats::RunOptions options() const;
    // 書き出す: the check, the export, what came of it (the dialog accepted when files were written).
    void run();
    // The folder the files go to (Path(folder).expanduser()).
    std::filesystem::path out() const;

    // The person chose to fix what the check found first (the window opens 点検 then).
    bool fix_requested = false;
    // What formats::run returned (none: not run).
    std::optional<core::Json> reply;

    QComboBox* which = nullptr;
    QLineEdit* range = nullptr;
    QLabel* preview = nullptr;
    QLabel* preview_note = nullptr;
    QComboBox* format = nullptr;
    QLabel* note = nullptr;
    QSpinBox* dpi = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* max_height = nullptr;
    QSpinBox* long_edge = nullptr;
    QComboBox* area = nullptr;
    QComboBox* color = nullptr;
    QLineEdit* icc = nullptr;
    QCheckBox* screen_on = nullptr;
    QDoubleSpinBox* screen_lpi = nullptr;
    QComboBox* screen_shape = nullptr;
    QWidget* screen_row = nullptr;
    QWidget* icc_row = nullptr;
    QLabel* colour_head = nullptr;
    QCheckBox* jpeg = nullptr;
    QCheckBox* spreads = nullptr;
    QCheckBox* official = nullptr;
    QLineEdit* folder = nullptr;
    QDialogButtonBox* buttons = nullptr;
    std::map<std::string, QWidget*> rows;  // option key → its field (shown only for the formats that take it)

private:
    void format_changed();
    void show_preview();
    bool bitonal() const;
    // ask_preflight: "go" (write anyway), "fix" or "stop".
    QString ask_preflight(const std::vector<core::Json>& errors);
    void pick_icc();
    void pick_folder();

    DocPtr episode_;
    std::optional<std::filesystem::path> project_;
    std::string actor_;
    std::int64_t current_page_ = 1;
    QFormLayout* form_ = nullptr;
};

}  // namespace genko::app
