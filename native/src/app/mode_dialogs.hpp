#pragma once

#include <QDialog>
#include <QString>

#include <array>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

// ページ → 色モード's own modes (color-model@1, PC1d; PhotoCraft's Image › Mode › Indexed Color…, Color Table…,
// Bitmap…, Duotone…): each dialog gives the op the window sends (to_indexed, set_color_table, to_bitmap, set_duotone),
// every setting written out (an op never reads the machine's settings). Their fields and defaults are PhotoCraft's
// (mode_cmds.rs), their words Japanese.

namespace genko::app {

// インデックスカラー: the page in front or every page; the palette, its colours, the forced colours, transparency, the
// dither and its amount.
class IndexedDialog : public QDialog {
    Q_OBJECT

public:
    IndexedDialog(QWidget* parent, const core::Page* page);
    core::Json op() const;

    QComboBox* scope = nullptr;
    QComboBox* palette = nullptr;
    QSpinBox* colors = nullptr;
    QComboBox* forced = nullptr;
    QCheckBox* transparency = nullptr;
    QComboBox* dither = nullptr;
    QSpinBox* amount = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    const core::Page* page_ = nullptr;
};

// カラーテーブル: an indexed page's table as a 16 × 16 grid (a click chooses an entry's colour), a preset, the
// transparent entry.
class ColorTableDialog : public QDialog {
    Q_OBJECT

public:
    ColorTableDialog(QWidget* parent, const core::Page& page);
    core::Json op() const;
    // The table as the dialog has it.
    const std::vector<std::array<int, 3>>& colors() const { return colors_; }
    // An entry set (as a click on its cell and a colour chosen does), the grid repainted.
    void set_entry(int index, const std::array<int, 3>& colour);

    QComboBox* preset = nullptr;
    QComboBox* transparent = nullptr;
    std::vector<QToolButton*> cells;
    QDialogButtonBox* buttons = nullptr;

private:
    void paint();
    const core::Page& page_;
    std::vector<std::array<int, 3>> colors_;
};

// 2 階調: the method, a halftone's frequency, angle and shape.
class BitmapDialog : public QDialog {
    Q_OBJECT

public:
    BitmapDialog(QWidget* parent, const core::Page* page);
    core::Json op() const;

    QComboBox* scope = nullptr;
    QComboBox* method = nullptr;
    QDoubleSpinBox* frequency = nullptr;
    QDoubleSpinBox* angle = nullptr;
    QComboBox* shape = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    const core::Page* page_ = nullptr;
};

// ダブルトーン: monotone to quadtone, each ink's name, colour and curve (points "in:out" 0..100).
class DuotoneDialog : public QDialog {
    Q_OBJECT

public:
    DuotoneDialog(QWidget* parent, const core::Page* page);
    core::Json op() const;
    // Why the inks cannot be sent (a curve that cannot be read), or "".
    QString problem() const;

    QComboBox* scope = nullptr;
    QComboBox* type = nullptr;
    std::array<QLineEdit*, 4> names{};
    std::array<QPushButton*, 4> colours{};
    std::array<QLineEdit*, 4> curves{};
    QDialogButtonBox* buttons = nullptr;

private:
    void enable();
    const core::Page* page_ = nullptr;
    std::array<std::array<int, 3>, 4> chosen_{};
};

}  // namespace genko::app
