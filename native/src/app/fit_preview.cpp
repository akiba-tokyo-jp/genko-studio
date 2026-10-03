#include "app/fit_preview.hpp"

#include <QPainter>

#include <algorithm>

#include "app/theme.hpp"

namespace genko::app {

FitPreview::FitPreview(QWidget* parent) : QWidget(parent) {
    setMinimumSize(80, 80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void FitPreview::set_image(const QImage& image) {
    image_ = image;
    update();
}

QRectF FitPreview::fit(const QSizeF& picture, const QSizeF& room, double margin) {
    if (picture.width() <= 0 || picture.height() <= 0) return {};
    const double w = std::max(1.0, room.width() - 2 * margin);
    const double h = std::max(1.0, room.height() - 2 * margin);
    const double scale = std::min(w / picture.width(), h / picture.height());
    const QSizeF shown(picture.width() * scale, picture.height() * scale);
    return QRectF((room.width() - shown.width()) / 2, (room.height() - shown.height()) / 2, shown.width(), shown.height());
}

QRectF FitPreview::shown_rect() const {
    if (image_.isNull()) return {};
    return fit(QSizeF(image_.size()), QSizeF(size()));
}

void FitPreview::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), theme::surround());
    if (image_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(shown_rect(), image_);
}

}  // namespace genko::app
