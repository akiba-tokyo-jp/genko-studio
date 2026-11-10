#include "app/fit_preview.hpp"

#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>

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

QRect FitPreview::in_sight() const {
    for (QWidget* up = parentWidget(); up != nullptr; up = up->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(up)) {
            const QWidget* port = scroll->viewport();
            const QRect seen = QRect(mapFrom(port, QPoint(0, 0)), port->size()).intersected(rect());
            return seen.isEmpty() ? rect() : seen;
        }
    }
    return rect();
}

QRectF FitPreview::shown_rect() const {
    if (image_.isNull()) return {};
    const QRect room = in_sight();
    return fit(QSizeF(image_.size()), QSizeF(room.size())).translated(room.topLeft());
}

void FitPreview::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (watching_) return;
    for (QWidget* up = parentWidget(); up != nullptr; up = up->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(up)) {
            // (the body scrolled: the part in sight moved, the page is fitted to it again)
            for (QScrollBar* bar : {scroll->verticalScrollBar(), scroll->horizontalScrollBar()}) {
                connect(bar, &QScrollBar::valueChanged, this, [this] { update(); });
            }
            watching_ = true;
            break;
        }
    }
}

void FitPreview::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::surround());
    painter.drawRoundedRect(QRectF(rect()), 6, 6);  // (as Python's preview: border-radius 6px)
    if (image_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(shown_rect(), image_);
}

}  // namespace genko::app
