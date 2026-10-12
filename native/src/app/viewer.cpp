#include "app/viewer.hpp"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "app/theme.hpp"

namespace genko::app {

ImageView::ImageView() {
    setScene(new QGraphicsScene(this));
    setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setBackgroundBrush(QColor(theme::tokens().surround));
}

void ImageView::show_items(const std::vector<ViewerItem>& items) {
    QGraphicsScene* s = scene();
    s->clear();
    double x = 0;
    const double gap = 24;
    QFont font;
    font.setPointSize(14);
    int tallest = 0;
    for (const auto& [caption, image] : items) tallest = std::max(tallest, image.height());
    for (const auto& [caption, image] : items) {
        QGraphicsPixmapItem* item = s->addPixmap(QPixmap::fromImage(image));
        item->setPos(x, 0);
        item->setTransformationMode(Qt::SmoothTransformation);
        if (!caption.isEmpty()) {
            QGraphicsSimpleTextItem* text = s->addSimpleText(caption, font);
            text->setBrush(QColor(theme::tokens().text));
            text->setPos(x, tallest + 8);
        }
        x += image.width() + gap;
    }
    s->setSceneRect(s->itemsBoundingRect().adjusted(-gap, -gap, gap, gap));
    fitted_ = true;
    fit();
}

void ImageView::fit() {
    fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    fitted_ = true;
}

void ImageView::actual() {
    resetTransform();
    fitted_ = false;
}

void ImageView::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta != 0) {
        const double factor = 1.0 + std::clamp(delta / 600.0, -0.5, 0.5);
        scale(factor, factor);
        fitted_ = false;
    }
}

void ImageView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (fitted_) fit();
}

ViewerDialog::ViewerDialog(QWidget* parent, const QString& title, const std::vector<ViewerItem>& items, const QString& note, std::vector<int> pages,
                           std::function<std::vector<ViewerItem>(int)> load_page, std::optional<int> current)
    : QDialog(parent), pages_(std::move(pages)), load_page_(std::move(load_page)) {
    setWindowTitle(title);
    if (current) {
        const auto it = std::find(pages_.begin(), pages_.end(), *current);
        if (it != pages_.end()) index_ = static_cast<int>(it - pages_.begin());
    }
    view_ = new ImageView;
    auto* caption = new QLabel(note);
    caption->setWordWrap(true);
    auto* prev_button = new QPushButton(QStringLiteral("← 前のページ"));
    auto* next_button = new QPushButton(QStringLiteral("次のページ →"));
    connect(prev_button, &QPushButton::clicked, this, [this] { step(-1); });
    connect(next_button, &QPushButton::clicked, this, [this] { step(1); });
    auto* fit = new QPushButton(QStringLiteral("全体（0）"));
    connect(fit, &QPushButton::clicked, view_, &ImageView::fit);
    auto* actual = new QPushButton(QStringLiteral("100%（1）"));
    connect(actual, &QPushButton::clicked, view_, &ImageView::actual);
    auto* close = new QPushButton(QStringLiteral("閉じる"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    page_label_ = new QLabel;
    auto* bar = new QHBoxLayout;
    if (!pages_.empty() && load_page_) {
        bar->addWidget(prev_button);
        bar->addWidget(page_label_);
        bar->addWidget(next_button);
    } else {
        prev_button->hide();
        next_button->hide();
    }
    bar->addStretch(1);
    bar->addWidget(fit);
    bar->addWidget(actual);
    bar->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(caption);
    layout->addWidget(view_, 1);
    layout->addLayout(bar);
    const std::pair<const char*, std::function<void()>> keys[] = {
        {"0", [this] { view_->fit(); }}, {"1", [this] { view_->actual(); }}, {"Left", [this] { step(-1); }}, {"Right", [this] { step(1); }}};
    for (const auto& [key, slot] : keys) {
        auto* shortcut = new QShortcut(QKeySequence(QString::fromLatin1(key)), this);
        connect(shortcut, &QShortcut::activated, this, slot);
    }
    QRect room(0, 0, 1280, 800);
    if (parent != nullptr && parent->screen() != nullptr) room = parent->screen()->availableGeometry();
    resize(static_cast<int>(room.width() * 0.85), static_cast<int>(room.height() * 0.9));
    view_->show_items(items);
    label();
}

void ViewerDialog::label() {
    if (!pages_.empty()) {
        page_label_->setText(QStringLiteral("%1 ページ（%2 / %3）").arg(pages_[static_cast<std::size_t>(index_)]).arg(index_ + 1).arg(pages_.size()));
    }
}

void ViewerDialog::step(int delta) {
    if (pages_.empty() || !load_page_) return;
    const int next = index_ + delta;
    if (next >= 0 && next < static_cast<int>(pages_.size())) {
        index_ = next;
        view_->show_items(load_page_(pages_[static_cast<std::size_t>(next)]));
        label();
    }
}

}  // namespace genko::app
