#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QSpinBox;

// The book's dialogs of M4 (Python's genko/app/pages_panel.py nombre_dialog and main.py MainWindow._add_cover_dialog),
// and what 作品の結合 (MainWindow._merge_book) reads and sends (main._page_list, merge.import_op). Their fields, ranges and
// words are Python's, and each gives the op Python's window sends. Python lays each out as one QFormLayout with its
// buttons as the last row; here the rows scroll when the screen is small (1024 × 640 at 200 %) and the buttons stay in
// sight under them (SPEC UX-02).

namespace genko::app {

// ノンブルの設定: where, the face, the size, the first number and the hidden nombre, as the book has them
// (nombre.settings: Python's defaults under the book's own).
class NombreDialog : public QDialog {
    Q_OBJECT

public:
    // core::Error when the book's settings are not what Python's float() and int() take (its dialog fails there and
    // never opens).
    NombreDialog(QWidget* parent, const core::Document& book);
    // {"op": "set_nombre", "show", "position", "font", "size_mm", "start", "hidden", "hidden_size_mm"}: every setting.
    core::Json op() const;

    QCheckBox* shown = nullptr;  // (ノンブルを入れる)
    QComboBox* position = nullptr;
    QComboBox* face = nullptr;
    QDoubleSpinBox* size_mm = nullptr;
    QSpinBox* start = nullptr;
    QCheckBox* hidden = nullptr;
    QDoubleSpinBox* hidden_mm = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

// 表紙・カバーを足す: the kinds the book has not got yet (covers.LABELS), the spine and flaps of a jacket or a band, the
// band's height (each on only for the kinds that take it).
class CoverDialog : public QDialog {
    Q_OBJECT

public:
    CoverDialog(QWidget* parent, const core::Document& book);
    // Every kind there already (Python says so instead of opening the dialog).
    bool all_there() const;
    // {"op": "add_cover", "kind"} with spine_mm and flap_mm for a jacket or a band, height_mm for a band.
    core::Json op() const;

    QComboBox* kind = nullptr;
    QDoubleSpinBox* spine = nullptr;
    QDoubleSpinBox* flap = nullptr;
    QDoubleSpinBox* band = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void enable();
};

// main._page_list(text, count): "1-4, 7" → [1, 2, 3, 4, 7] (、 a comma, 〜 and ～ a dash; a range either way round;
// each page once, in the order written); nothing written: none (every page). core::Error("value") where Python raises
// ValueError: a part Python's int() does not read, or a page outside 1..count — and, where Python's range() fails with
// OverflowError or MemoryError first (a range past any book), the same.
std::optional<std::vector<std::int64_t>> page_list(const QString& text, std::int64_t count);

// str(Path(folder)): the folder as pathlib writes it (no closing separator, no empty or "." parts; \ on Windows).
std::string python_path_text(const QString& folder);

// merge.import_op(src, pages, after): {"op": "import_pages", "from"}, with "pages" when some are named and "after".
core::Json import_op(const std::string& from, const std::optional<std::vector<std::int64_t>>& pages,
                     std::optional<std::int64_t> after = std::nullopt);

}  // namespace genko::app
