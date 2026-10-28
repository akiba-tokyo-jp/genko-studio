#pragma once

#include <QImage>
#include <QPainter>
#include <QWidget>

#include <array>
#include <functional>

#include "app/filter_menu.hpp"

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → レンズ補正… and 広角補正… — Photoshop's windows: on the
// left the layer's picture (what shows of it, the frame the filter works) with the result laid on it a moment after a
// change (worked on a thread at the window's resolution: render/fx_lens, the op's own kernels), on the right the settings.
// レンズ補正: 自動補正 (the camera and lens the picture's EXIF names, its profile — none, generic of a focal length, or auto
// from the EXIF —, what it corrects, 画像を自動で拡大・縮小, エッジ) and カスタム (ゆがみを補正, the fringes, 周辺光量補正,
// 変形), グリッドを表示 and 角度補正ツール (a line drawn on the picture levels it). 広角補正: lines drawn on the picture
// become its constraints (Shift: level or upright; the selected one's orientation, 削除), 補正 (自動・魚眼・遠近法・完全な球面),
// 焦点距離, 切り抜き係数, 拡大・縮小 — each constraint shown as the curve the lens makes of the straight line. OK sends the
// settings (filter_raster's lens_correction, adaptive_wide_angle; on a smart object a smart filter), キャンセル nothing.

namespace genko::app::lens_window {

// The picture the windows show (and 消点's: app/vanishing_window): an image laid over a box of the page (mm), fitted to the
// widget; what is drawn over it (`overlay`, in the widget's pixels) and the mouse (`press`, `drag`, `release` with the
// page's mm and the keys held) are the window's.
class PictureView : public QWidget {
public:
    explicit PictureView(QWidget* parent = nullptr);
    void set_image(const QImage& image, const std::array<double, 4>& box_mm);
    const std::array<double, 4>& box_mm() const { return box_; }
    // A place on the page (mm) in the widget's pixels, and back.
    QPointF to_widget(double x_mm, double y_mm) const;
    std::array<double, 2> to_mm(const QPointF& at) const;
    // How many of the widget's pixels a mm is.
    double scale() const;
    std::function<void(QPainter&)> overlay;
    std::function<void(std::array<double, 2>, Qt::KeyboardModifiers, Qt::MouseButton)> press;
    std::function<void(std::array<double, 2>, Qt::KeyboardModifiers)> drag;
    std::function<void(std::array<double, 2>, Qt::KeyboardModifiers)> release;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF target() const;
    QImage image_;
    std::array<double, 4> box_{0, 0, 1, 1};
    bool down_ = false;
};

// The windows from フィルター's commands or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open_lens(const FilterWindowCall& call);
void open_wide_angle(const FilterWindowCall& call);

}  // namespace genko::app::lens_window
