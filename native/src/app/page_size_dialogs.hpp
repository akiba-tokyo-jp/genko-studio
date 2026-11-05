#pragma once

#include <QDialog>
#include <QString>

#include <array>
#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/units.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QToolButton;

// 判型 (PC3 C-e, SPEC.md COLOR-07; PhotoCraft's Image › Image Size…, Canvas Size…, Image Rotation › Arbitrary…, Trim…):
// each dialog gives the op the window sends (resize_pages, set_canvas_size, rotate_pages, trim_page), every setting written
// out. Each asks which pages — the page in front, or every page (the defaults Photoshop's: Image Size and Canvas Size the
// whole book, the others the page) — and, where the paper's size changes, where its finished size goes: kept on what the
// page holds, or fitted to the new paper ("trim": "fit").

namespace genko::app {

// 画像解像度: the size (mm or pixels at the resolution), its shape kept or not, the resolution, 再サンプル and its way.
class ImageSizeDialog : public QDialog {
    Q_OBJECT

public:
    ImageSizeDialog(QWidget* parent, const core::Page& page);
    core::Json op() const;

    QComboBox* scope = nullptr;       // page | all
    QComboBox* unit = nullptr;        // mm | px — PC5 H-b: and cm, in, pt, pica, percent, columns (環境設定 → 単位・定規's first)
    QDoubleSpinBox* width = nullptr;
    QDoubleSpinBox* height = nullptr;
    QCheckBox* keep_ratio = nullptr;  // 縦横比を固定
    QSpinBox* dpi = nullptr;
    QCheckBox* resample = nullptr;    // 再サンプル
    QComboBox* method = nullptr;      // bicubic | bilinear | nearest | lanczos | preserve_details
    QLabel* summary = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void follow(bool from_width);
    void show_unit();
    // (a length along the width — `across` — or the height in the unit chosen: px at the resolution in the window, a percent
    // of the page's size now)
    core::units::Basis basis(bool across) const;
    double mm_of(double value, bool across) const;
    double value_of(double mm, bool across) const;

    const core::Page& page_;
    double ratio_ = 1;
    QString unit_now_ = QStringLiteral("mm");
    bool following_ = false;
};

// カンバスサイズ: the new size (mm, or added to the paper: 相対), the base point (9 places), the extension colour, the
// finished size.
class CanvasSizeDialog : public QDialog {
    Q_OBJECT

public:
    CanvasSizeDialog(QWidget* parent, const core::Page& page);
    core::Json op() const;
    // The base point chosen (top_left … bottom_right), and chosen (as a click on its button).
    QString anchor() const { return anchor_; }
    void choose_anchor(const QString& anchor);

    QComboBox* scope = nullptr;
    QComboBox* unit = nullptr;  // (PC5 H-b: mm, px, cm, in, pt, pica, percent, columns — 環境設定 → 単位・定規's first)
    QDoubleSpinBox* width = nullptr;
    QDoubleSpinBox* height = nullptr;
    QCheckBox* relative = nullptr;
    std::array<QToolButton*, 9> anchors{};
    QComboBox* extension = nullptr;  // transparent | white | black | colour (the one chosen with its button)
    QToolButton* colour = nullptr;
    QComboBox* trim = nullptr;       // keep | fit
    QDialogButtonBox* buttons = nullptr;

private:
    core::units::Basis basis(bool across) const;
    void show_unit();
    const core::Page& page_;
    core::units::Unit unit_now_ = core::units::Unit::Mm;
    QString anchor_ = QStringLiteral("center");
    std::array<int, 3> chosen_{255, 255, 255};
};

// 角度入力: the angle, its way (時計回り・反時計回り), the interpolation.
class RotateAngleDialog : public QDialog {
    Q_OBJECT

public:
    RotateAngleDialog(QWidget* parent, const core::Page& page);
    core::Json op() const;

    QComboBox* scope = nullptr;
    QDoubleSpinBox* angle = nullptr;
    QComboBox* direction = nullptr;  // cw | ccw
    QComboBox* interp = nullptr;     // bicubic | bilinear | nearest
    QDialogButtonBox* buttons = nullptr;

private:
    const core::Page& page_;
};

// トリミング: what is cut away (transparent pixels, the top left's colour, the bottom right's), the sides, the finished
// size.
class TrimDialog : public QDialog {
    Q_OBJECT

public:
    TrimDialog(QWidget* parent, const core::Page& page);
    core::Json op() const;

    QComboBox* scope = nullptr;
    QComboBox* based_on = nullptr;  // transparent | top_left | bottom_right
    QCheckBox* top = nullptr;
    QCheckBox* bottom = nullptr;
    QCheckBox* left = nullptr;
    QCheckBox* right = nullptr;
    QComboBox* trim = nullptr;      // keep | fit
    QDialogButtonBox* buttons = nullptr;

private:
    const core::Page& page_;
};

}  // namespace genko::app
