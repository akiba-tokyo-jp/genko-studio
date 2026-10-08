// Python's genko/app/subview.py.

#include "app/subview.hpp"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

const QString kImageFilter = QStringLiteral("画像 (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)");

}  // namespace

QStringList kept_pictures() {
    const QString value = settings()->value(QStringLiteral("subview/images"), QString()).toString();
    QStringList out;
    for (const QString& path : value.split(QLatin1Char('\t'), Qt::SkipEmptyParts)) {
        if (QFileInfo(path).isFile()) out << path;
    }
    return out;
}

void keep_pictures(const QStringList& paths) { settings()->setValue(QStringLiteral("subview/images"), paths.join(QLatin1Char('\t'))); }

SubPicture::SubPicture() {
    setMinimumHeight(140);
    setCursor(Qt::CrossCursor);
    setToolTip(QStringLiteral("クリックした所の色をペンの色にします"));
}

void SubPicture::set_image(const QImage& image) {
    image_ = image;
    update();
}

std::optional<SubPicture::Target> SubPicture::target() const {
    if (image_.isNull()) return std::nullopt;
    const double scale = std::min(static_cast<double>(width()) / image_.width(), static_cast<double>(height()) / image_.height());
    const double w = image_.width() * scale, h = image_.height() * scale;
    return Target{(width() - w) / 2, (height() - h) / 2, scale};
}

void SubPicture::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(theme::tokens().surround));
    const auto at = target();
    if (!at) {
        painter.setPen(QColor(theme::tokens().text));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("「画像を足す」で資料を開きます"));
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(static_cast<int>(at->x), static_cast<int>(at->y),
                       QPixmap::fromImage(image_).scaled(static_cast<int>(image_.width() * at->scale), static_cast<int>(image_.height() * at->scale),
                                                         Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

std::optional<std::array<int, 3>> SubPicture::colour_at(const QPointF& pos) const {
    const auto at = target();
    if (!at) return std::nullopt;
    const int px = static_cast<int>((pos.x() - at->x) / at->scale), py = static_cast<int>((pos.y() - at->y) / at->scale);
    if (px < 0 || py < 0 || px >= image_.width() || py >= image_.height()) return std::nullopt;  // (int(): toward 0, as Python's)
    const QColor c = image_.pixelColor(px, py);
    return std::array<int, 3>{c.red(), c.green(), c.blue()};
}

void SubPicture::mousePressEvent(QMouseEvent* event) {
    if (const auto colour = colour_at(event->position())) emit picked(*colour);
}

SubView::SubView(MainWindow* window) : window_(window) {
    choice = new QComboBox;
    choice->setObjectName(QStringLiteral("subviewChoice"));
    connect(choice, &QComboBox::activated, this, [this](int) { show_current(); });
    auto* add_button = new QPushButton(QStringLiteral("画像を足す…"));
    connect(add_button, &QPushButton::clicked, this, [this] { add_dialog(); });
    auto* remove_button = new QPushButton(QStringLiteral("外す"));
    connect(remove_button, &QPushButton::clicked, this, [this] { remove_current(); });
    picture = new SubPicture;
    connect(picture, &SubPicture::picked, this, [this](const std::array<int, 3>& rgb) {
        window_->brush_panel()->set_colour(rgb);
        window_->flash(QStringLiteral("資料の色 (%1, %2, %3) をペンの色にしました").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]), 2000);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(add_button);
    row->addWidget(remove_button);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(choice);
    layout->addWidget(picture, 1);
    layout->addLayout(row);
    refresh();
}

void SubView::refresh() {
    const QString current = choice->currentData().toString();
    choice->clear();
    for (const QString& path : kept_pictures()) choice->addItem(QFileInfo(path).fileName(), path);
    if (!current.isEmpty()) choice->setCurrentIndex(std::max(0, choice->findData(current)));
    show_current();
}

void SubView::show_current() {
    const QString path = choice->currentData().toString();
    picture->set_image(path.isEmpty() ? QImage() : QImage(path));
}

bool SubView::add(const QString& path) {
    if (QImage(path).isNull()) return false;
    QStringList paths;
    for (const QString& p : kept_pictures()) {
        if (p != path) paths << p;
    }
    paths << path;
    keep_pictures(paths);
    refresh();
    choice->setCurrentIndex(choice->findData(path));
    show_current();
    return true;
}

void SubView::add_dialog() {
    const QString path = ask::open_path(this, QStringLiteral("資料の画像"), kImageFilter);
    if (!path.isEmpty() && !add(path)) window_->flash(QStringLiteral("その画像は開けませんでした"), 3000);
}

void SubView::remove_current() {
    const QString path = choice->currentData().toString();
    if (path.isEmpty()) return;
    QStringList paths;
    for (const QString& p : kept_pictures()) {
        if (p != path) paths << p;
    }
    keep_pictures(paths);
    refresh();
}

}  // namespace genko::app
