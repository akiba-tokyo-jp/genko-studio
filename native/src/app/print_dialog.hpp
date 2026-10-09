#pragma once

#include <QDialog>
#include <QImage>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPrinter;
class QPrintPreviewDialog;
class QPushButton;

// ファイル → 印刷… (Python's genko/app/printing.py): pages on paper, from any printer the computer has. The pages are
// drawn as they print (render_page in print mode: no name lines, tones as dots), cut to the finished size or the bleed or
// left as the whole sheet (with crop marks), and fitted to the printer's paper keeping their shape (or at their real
// size); a spread's two pages on one sheet when asked.

namespace genko::app {

class MainWindow;

namespace printing {

inline constexpr int kMaxDpi = 600;

// printing.sheets: the pages grouped by sheet — one page each, or a spread's two pages side by side (left first).
// core::Error("key") for a page number the book does not have (Python's KeyError).
std::vector<std::vector<const core::Page*>> sheets(const core::Document& episode, const std::vector<std::int64_t>& pages, bool spreads);

// The resolution the pages are drawn at for this printer: max(150, min(600, its resolution or 300)).
int print_dpi(const QPrinter& printer);

// printing.print_pages: draw these pages on the printer; scale "fit" (as large as the paper allows) or "actual" (the
// page's real size); `progress` after each sheet (done, of); `sheet` gets each sheet's picture and where it is drawn
// (device pixels) before it is drawn. Returns how many sheets were printed. core::Error("value") "プリンターを開けません
// でした" when the printer does not open; what this build does not draw yet (render::NotYetPorted) is said before
// anything goes to the printer.
int print_pages(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages, const QString& area = QStringLiteral("trim"),
                const std::function<void(int, int)>& progress = {}, const QString& scale = QStringLiteral("fit"), bool spreads = false,
                const std::function<void(const QImage&, const QRectF&)>& sheet = {});

}  // namespace printing

// Which pages and how much of each; then the system's printer dialog (or its preview).
class PrintDialog : public QDialog {
    Q_OBJECT

public:
    explicit PrintDialog(MainWindow* window);

    // The page numbers chosen (core::PyValueError with Python's words when they cannot be read).
    std::vector<std::int64_t> chosen() const;
    // プリンターを選んで印刷…: to `printer` (none: the system's printer dialog asks for one).
    void print(QPrinter* printer = nullptr);
    // プレビュー…: how the sheets will come out (the preview dialog, opened; null when the pages cannot be read).
    QPrintPreviewDialog* preview();

    QLineEdit* pages = nullptr;
    QComboBox* area = nullptr;
    QComboBox* scale = nullptr;
    QCheckBox* spreads = nullptr;
    QPushButton* print_button = nullptr;
    QPushButton* preview_button = nullptr;
    QPushButton* this_page = nullptr;
    QPushButton* cancel_button = nullptr;

private:
    MainWindow* window_;
    QString current_;
};

}  // namespace genko::app
