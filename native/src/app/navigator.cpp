#include "app/navigator.hpp"

#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QVBoxLayout>

#include <algorithm>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

Navigator::Navigator(PageCanvas* canvas, QWidget* parent) : QWidget(parent), canvas_(canvas) {
    setMinimumHeight(150);
    setToolTip(QStringLiteral("クリック・ドラッグで、その場所を画面の真ん中に"));
    connect(canvas_, &PageCanvas::changed, this, qOverload<>(&QWidget::update));
    connect(canvas_, &PageCanvas::zoomChanged, this, [this](double) { update(); });
    connect(&canvas_->renderer(), &PageRenderer::updated, this, qOverload<>(&QWidget::update));
}

QRectF Navigator::page_rect() const {
    const core::Page* page = canvas_->page();
    if (page == nullptr) return {};
    const double w = page->spec.width_mm.value();
    const double h = page->spec.height_mm.value();
    const double scale = std::min((width() - 8) / w, (height() - 8) / h);
    return QRectF((width() - w * scale) / 2, (height() - h * scale) / 2, w * scale, h * scale);
}

void Navigator::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(theme::tokens().surround));
    const QRectF r = page_rect();
    const core::Page* page = canvas_->page();
    if (r.isEmpty() || page == nullptr) return;
    painter.fillRect(r, Qt::white);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const double scale = r.width() / page->spec.width_mm.value();
    {
        // the canvas's own tiles, small (nothing is drawn again for the navigator)
        painter.save();
        painter.setTransform(QTransform(scale, 0, 0, scale, r.x(), r.y()));
        painter.setClipRect(QRectF(0, 0, page->spec.width_mm.value(), page->spec.height_mm.value()));
        canvas_->renderer().paint(painter, QRectF(0, 0, page->spec.width_mm.value(), page->spec.height_mm.value()));
        painter.restore();
    }
    const QRectF seen_mm = canvas_->seen_mm();
    const QRectF seen = QRectF(r.x() + seen_mm.x() * scale, r.y() + seen_mm.y() * scale, seen_mm.width() * scale, seen_mm.height() * scale)
                            .intersected(QRectF(rect()));
    if (seen.contains(r)) return;  // (the whole page is in sight: no frame around everything)
    // the part out of sight dimmed, the part in sight framed in the accent
    QPainterPath outside;
    outside.addRect(r);
    QPainterPath inside;
    inside.addRect(seen);
    QColor shade(theme::tokens().surround);
    shade.setAlpha(110);
    painter.fillPath(outside.subtracted(inside), shade);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::accent(), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(seen.adjusted(0.75, 0.75, -0.75, -0.75), 2, 2);
}

void Navigator::go(const QPointF& pos) {
    const QRectF r = page_rect();
    const core::Page* page = canvas_->page();
    if (r.isEmpty() || page == nullptr) return;
    const double scale = r.width() / page->spec.width_mm.value();
    canvas_->center_on((pos.x() - r.x()) / scale, (pos.y() - r.y()) / scale);
}

void Navigator::mousePressEvent(QMouseEvent* event) { go(event->position()); }

void Navigator::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) go(event->position());
}

PageOverview::PageOverview(QWidget* parent, DocPtr doc, int current) : QDialog(parent), maker_(std::make_unique<ThumbMaker>()), doc_(std::move(doc)) {
    setWindowTitle(QStringLiteral("ページを並べて見る"));
    resize(900, 640);
    list_ = new QListWidget;
    list_->setViewMode(QListWidget::IconMode);
    list_->setResizeMode(QListWidget::Adjust);
    list_->setMovement(QListWidget::Static);
    list_->setIconSize(QSize(160, 226));
    list_->setSpacing(6);
    auto* note = new QLabel(QStringLiteral("ダブルクリックでそのページを開きます。見開きのページは「◀▶」で示します。"));
    theme::hint(note);
    note->setVisible(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(list_, 1);
    for (std::size_t i = 0; i < doc_->pages.size(); ++i) {
        const core::Page& page = *doc_->pages[i];
        QString label = QString::fromStdString(page.index.repr());
        if (page.spread_with && page.spread_with->truthy()) label += QStringLiteral(" ◀▶ ") + QString::fromStdString(page.spread_with->repr());
        QPixmap blank(QSize(160, 226));
        blank.fill(QColor(QStringLiteral("#f4f4f4")));
        auto* item = new QListWidgetItem(QIcon(blank), label);
        const int number = static_cast<int>(core::py_int(page.index.json()));
        item->setData(Qt::UserRole, number);
        item->setData(Qt::UserRole + 1, QString::fromStdString(page.id));
        list_->addItem(item);
        if (number == current) item->setSelected(true);
        // (16 dpi, as Python's overview draws them; from the lasting cache when they were drawn before)
        const int height = static_cast<int>(std::nearbyint(page.spec.height_mm.value() / 25.4 * 16));
        maker_->request(doc_, i, height, "proof");
    }
    connect(maker_.get(), &ThumbMaker::done, this, [this](const QString& id, int, const QImage& image, bool) {
        for (int row = 0; row < list_->count(); ++row) {
            if (list_->item(row)->data(Qt::UserRole + 1).toString() == id) list_->item(row)->setIcon(QIcon(QPixmap::fromImage(image)));
        }
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit pageChosen(item->data(Qt::UserRole).toInt());
        accept();
    });
}

}  // namespace genko::app
