#pragma once

#include <QDialog>
#include <QImage>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/model.hpp"
#include "formats/print_marks.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPrinter;
class QPrintPreviewDialog;
class QPushButton;
class QTabWidget;

// ファイル → 印刷… (Python's genko/app/printing.py): pages on paper, from any printer the computer has. The pages are
// drawn as they print (render_page in print mode: no name lines, tones as dots), cut to the finished size or the bleed or
// left as the whole sheet (with crop marks), and fitted to the printer's paper keeping their shape (or at their real
// size); a spread's two pages on one sheet when asked.
//
// PC5 I-f (SPEC.md AUTO-01「プリント」; PhotoCraft v0.2.0's print_cmds.rs, MIT): Photoshop's プリント… — the colours handled by
// the printer or by Genko (the printer's profile, the rendering intent, black point compensation: formats/print_marks),
// the position and size (centred or at a place, fitted, at its size or scaled), the marks (corner and centre crop marks,
// registration marks, the description and the labels), to a PDF; 1 部プリント (one copy with the last settings, QSettings
// print/last and print/printer); `genko print` (app/print_cli.cpp) draws the same sheets.

namespace genko::app {

class MainWindow;

namespace printing {

inline constexpr int kMaxDpi = 600;

// printing.sheets: the pages grouped by sheet — one page each, or a spread's two pages side by side (left first).
// core::Error("key") for a page number the book does not have (Python's KeyError).
std::vector<std::vector<const core::Page*>> sheets(const core::Document& episode, const std::vector<std::int64_t>& pages, bool spreads);

// The resolution the pages are drawn at for this printer: max(150, min(600, its resolution or 300)).
int print_dpi(const QPrinter& printer);

// What stops a print, found before any paper is used (a quick look at each page): a book whose pages are still being
// read (core::Error("page_not_loaded")), what this build does not draw yet (render::NotYetPorted), a filter plugin's own
// refusal (not chosen to run, its runner's trouble: core::Error as when the page prints).
void check_pages(const core::Document& episode, const std::vector<std::int64_t>& pages, bool spreads = false);

// The printer's paper and the part of it it prints on (mm from the paper's top-left corner).
std::pair<formats::print::RectMm, formats::print::RectMm> paper_of(const QPrinter& printer);

// (PC5 I-f) Each sheet drawn as the setup asks on the printer — or, `draw` false, only made and placed (`genko print
// --dry-run`): its picture (the pages as they print at the setup's dpi or the printer's, their colours handled) and where it
// and its marks go. `progress` after each sheet (done, of); `sheet` gets each sheet's picture and where it is drawn (device
// pixels) before it is drawn. core::Error("value") "プリンターを開けませんでした" when the printer does not open; what check_pages
// and formats::print::check find is said before anything goes to the printer.
std::vector<formats::print::Sheet> print_sheets(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages,
                                                const formats::print::Setup& setup, const std::function<void(int, int)>& progress = {},
                                                const std::function<void(const QImage&, const QRectF&)>& sheet = {}, bool draw = true);

// printing.print_pages: draw these pages on the printer; scale "fit" (as large as the paper allows) or "actual" (the
// page's real size); `progress` after each sheet (done, of); `sheet` gets each sheet's picture and where it is drawn
// (device pixels) before it is drawn. Returns how many sheets were printed. (print_sheets with no marks, the printer
// managing the colours.)
int print_pages(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages, const QString& area = QStringLiteral("trim"),
                const std::function<void(int, int)>& progress = {}, const QString& scale = QStringLiteral("fit"), bool spreads = false,
                const std::function<void(const QImage&, const QRectF&)>& sheet = {});

// (PC5 I-f) The last プリント… settings and printer (1 部プリント's), kept on each print; none kept: the defaults and "".
formats::print::Setup last_setup();
QString last_printer();
void remember(const formats::print::Setup& setup, const QString& printer);

}  // namespace printing

// Which pages and how much of each; then the system's printer dialog (or its preview).
class PrintDialog : public QDialog {
    Q_OBJECT

public:
    explicit PrintDialog(MainWindow* window);

    // The page numbers chosen (core::PyValueError with Python's words when they cannot be read).
    std::vector<std::int64_t> chosen() const;
    // (PC5 I-f) The settings as the window shows them, and shown.
    formats::print::Setup setup() const;
    void show_setup(const formats::print::Setup& setup);
    // プリンターを選んで印刷…: to `printer` (none: the system's printer dialog asks for one).
    void print(QPrinter* printer = nullptr);
    // (PC5 I-f) PDF に出力…: the same sheets to a PDF file (none: asked where).
    void print_pdf(const QString& file = {});
    // プレビュー…: how the sheets will come out (the preview dialog, opened; null when the pages cannot be read or
    // check_pages stops them: said). A sheet that cannot be drawn in it is said after it has tried.
    QPrintPreviewDialog* preview();

    QLineEdit* pages = nullptr;
    QComboBox* area = nullptr;
    QComboBox* scale = nullptr;
    QCheckBox* spreads = nullptr;
    QPushButton* print_button = nullptr;
    QPushButton* preview_button = nullptr;
    QPushButton* this_page = nullptr;
    QPushButton* cancel_button = nullptr;
    // (PC5 I-f) 位置と大きさ
    QDoubleSpinBox* percent = nullptr;
    QCheckBox* center = nullptr;
    QDoubleSpinBox* left = nullptr;
    QDoubleSpinBox* top = nullptr;
    // 印
    QCheckBox* corner_marks = nullptr;
    QCheckBox* center_marks = nullptr;
    QCheckBox* registration_marks = nullptr;
    QCheckBox* description = nullptr;
    QCheckBox* labels = nullptr;
    // カラーマネジメント
    QComboBox* color_handling = nullptr;
    QComboBox* printer_profile = nullptr;
    QPushButton* profile_file = nullptr;
    QComboBox* intent = nullptr;
    QCheckBox* bpc = nullptr;
    QLabel* color_note = nullptr;
    QPushButton* pdf_button = nullptr;
    QTabWidget* tabs = nullptr;

private:
    // The setup checked before anything is drawn (said, false when it cannot print).
    bool usable(const formats::print::Setup& setup, std::vector<std::int64_t>& wanted);
    void follow();  // (the fields that go with a choice on or off)
    MainWindow* window_;
    QString current_;
};

}  // namespace genko::app
