#pragma once

#include <QDialog>

#include <map>
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

// ページの色モード… (color-mode@1, color-model@1, SPEC.md COLOR-02, COLOR-03): the page in front, or every page and the
// pages to come, in RGB, grey, CMYK or Lab, 8, 16 or 32 bits; a profile of the mode's space (a built-in one, the book's,
// or one chosen as a file: .icc), its intent, black point compensation, its greys, 8 bits dithered and HDR values cut.
// op() is the set_color_mode to send (its ICC in base64 when chosen here; the defaults left out).
class ColorModeDialog : public QDialog {
    Q_OBJECT

public:
    // `preset`: a mode to begin with ("" the page's own).
    ColorModeDialog(QWidget* parent, const core::Document& book, const core::Page* page, const QString& preset = {});
    core::Json op() const;
    // Why the choice cannot be sent (a CMYK mode without a profile), or "" when it can.
    QString problem() const;

    QComboBox* scope = nullptr;
    QComboBox* mode = nullptr;
    QComboBox* depth = nullptr;
    QComboBox* profile = nullptr;
    QPushButton* pick = nullptr;
    QComboBox* intent = nullptr;
    QCheckBox* bpc = nullptr;
    QComboBox* gray = nullptr;
    QCheckBox* dither = nullptr;
    QCheckBox* clip_hdr = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void enable();
    void fill_profiles();
    void choose_file();
    const core::Document& book_;
    const core::Page* page_ = nullptr;
    std::map<std::string, std::string> chosen_;  // (ICC files chosen here: their ref → their bytes)
};

// プロファイルの指定… (edit.assignProfile) and プロファイルに変換… (edit.convertToProfile), color-model@1: the page in front
// or every page; a profile (built-in, the book's, or a file) — to name the one its pixels are in (assign_profile: of its
// own space, or none: sRGB, sGray) or to convert them into (convert_profile: any space, its intent, black point
// compensation, dither, HDR cut).
class ProfileDialog : public QDialog {
    Q_OBJECT

public:
    ProfileDialog(QWidget* parent, const core::Document& book, const core::Page* page, bool convert);
    core::Json op() const;
    QString problem() const;

    QComboBox* scope = nullptr;
    QComboBox* profile = nullptr;
    QPushButton* pick = nullptr;
    QComboBox* intent = nullptr;
    QCheckBox* bpc = nullptr;
    QCheckBox* dither = nullptr;
    QCheckBox* clip_hdr = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void choose_file();
    const core::Page* page_ = nullptr;
    bool convert_ = false;
    std::optional<core::ProfileSpace> space_;  // (assign: the page's space)
    std::map<std::string, std::string> chosen_;
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
