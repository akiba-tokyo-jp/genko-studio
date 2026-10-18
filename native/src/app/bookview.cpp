// 本の形でプレビュー (Python's genko/app/bookview.py).

#include "app/bookview.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "formats/export.hpp"
#include "render/page.hpp"
#include "service/wording.hpp"

namespace genko::app::bookview {

namespace {

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

}  // namespace

std::vector<std::pair<std::string, const core::Page*>> book_pages(const core::Document& doc) {
    std::vector<std::pair<std::string, const core::Page*>> out;
    for (const auto& [page, part] : core::reading_order(doc)) out.emplace_back(part, page);
    return out;
}

std::vector<std::vector<int>> spreads(int count) {
    std::vector<std::vector<int>> out;
    if (count <= 0) return out;
    out.push_back({0});
    for (int k = 1; k < count; k += 2) out.push_back(k + 1 < count ? std::vector<int>{k, k + 1} : std::vector<int>{k});
    return out;
}

std::vector<std::vector<int>> book_spreads(const core::Document& doc, const std::vector<std::pair<std::string, const core::Page*>>& pages) {
    std::vector<std::vector<int>> out;
    const int count = static_cast<int>(pages.size());
    for (int i = 0; i < count;) {
        const auto& [kind, page] = pages[static_cast<std::size_t>(i)];
        if (kind == "page" && i + 1 < count && pages[static_cast<std::size_t>(i + 1)].first == "page" &&
            !core::facing_problem(doc, *page, *pages[static_cast<std::size_t>(i + 1)].second)) {
            out.push_back({i, i + 1});
            i += 2;
            continue;
        }
        out.push_back({i});
        ++i;
    }
    return out;
}

// --- the view ----------------------------------------------------------------------------------------------------------

BookView::BookView(BookPreview* dialog) : dialog_(dialog) { setMinimumSize(640, 440); }

std::pair<QRectF, QRectF> BookView::page_rects() {
    BookPreview* d = dialog_;
    const auto [left, right] = d->sides(d->spread());
    // (each page as big as its own paper, the pictures all at one scale: drawn at the same dpi, cut to their finished
    // sizes; one alone as big as one of a pair)
    const auto size_of = [&](const std::optional<int>& index) {
        if (!index) return QSizeF();
        const QPixmap& pix = d->pixmap(*index);
        return QSizeF(std::max(1, pix.width()), std::max(1, pix.height()));
    };
    const QSizeF l = size_of(left), r = size_of(right);
    const QSizeF one = l.isEmpty() ? r : l;
    const double across = std::max(l.width(), r.width()) * 2, down = std::max(l.height(), r.height());
    if (one.isEmpty() || across <= 0 || down <= 0) return {};
    const double scale = std::min((width() - 40) / across, (height() - 40) / down);
    const double middle = height() / 2.0;
    const auto at = [&](const QSizeF& size, bool on_left) {
        if (size.isEmpty()) return QRectF();
        const double w = size.width() * scale, h = size.height() * scale;
        return QRectF(on_left ? width() / 2.0 - w : width() / 2.0, middle - h / 2, w, h);
    };
    return {at(l, true), at(r, false)};
}

void BookView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor(theme::tokens().surround));
    BookPreview* d = dialog_;
    const auto [left, right] = d->sides(d->spread());
    const auto [left_rect, right_rect] = page_rects();
    for (const auto& [index, where] : {std::pair{left, left_rect}, {right, right_rect}}) {
        if (!index || where.isEmpty()) continue;
        const QPixmap& pix = d->pixmap(*index);
        painter.drawPixmap(where, pix, QRectF(pix.rect()));
    }
    const QRectF both = left_rect.united(right_rect);
    const double page_w = std::max(left_rect.width(), right_rect.width()), page_h = both.height();
    const QPointF spine(width() / 2.0, both.top());
    // the fold's shadow
    QLinearGradient shade(spine.x() - 18, 0, spine.x() + 18, 0);
    shade.setColorAt(0, QColor(0, 0, 0, 0));
    shade.setColorAt(0.5, QColor(0, 0, 0, 70));
    shade.setColorAt(1, QColor(0, 0, 0, 0));
    painter.fillRect(QRectF(spine.x() - 18, spine.y(), 36, page_h), shade);
    if (turn > 0) {  // the page turning over: squeezed toward the spine, then opening on the other side
        const double t = turn;
        if (const auto leaving = d->turning_page()) {
            const QPixmap& pix = d->pixmap(*leaving);
            const double w = std::abs(1 - 2 * t) * page_w;
            // forward, a left-to-right book turns its right page over to the left; a right-bound one the other way
            const bool starts_right = (direction > 0) != d->rtl();
            const bool on_right = t < 0.5 ? starts_right : !starts_right;
            const double x = on_right ? spine.x() : spine.x() - w;
            painter.save();
            painter.setTransform(QTransform());
            painter.drawPixmap(QRectF(x, spine.y() - 6 * (1 - std::abs(1 - 2 * t)), w, page_h), pix, QRectF(pix.rect()));
            painter.fillRect(QRectF(x, spine.y(), w, page_h), QColor(0, 0, 0, static_cast<int>(90 * (1 - std::abs(1 - 2 * t)))));
            painter.restore();
        }
    }
    painter.setPen(QColor(theme::tokens().text));
    painter.drawText(QRectF(0, height() - 28, width(), 24), Qt::AlignCenter, d->caption());
}

// --- the dialog --------------------------------------------------------------------------------------------------------

BookPreview::BookPreview(MainWindow* window) : QDialog(window), doc_(window->session().snapshot()) {
    setWindowTitle(QStringLiteral("本の形でプレビュー"));
    resize(1000, 700);
    pages_ = book_pages(*doc_);
    rtl_ = doc_->binding == core::Binding::Right;
    groups_ = book_spreads(*doc_, pages_);
    view = new BookView(this);
    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, std::max(0, static_cast<int>(groups_.size()) - 1));
    slider->setInvertedAppearance(rtl_);
    connect(slider, &QSlider::valueChanged, this, &BookPreview::go);
    // (a right-bound book reads to the left: ◀ is the next page there, and the buttons say so)
    auto* prev_b = new QPushButton(rtl_ ? QStringLiteral("◀ 次へ") : QStringLiteral("◀ 前へ"));
    auto* next_b = new QPushButton(rtl_ ? QStringLiteral("前へ ▶") : QStringLiteral("次へ ▶"));
    prev_b->setToolTip(QStringLiteral("左へめくる"));
    next_b->setToolTip(QStringLiteral("右へめくる"));
    connect(prev_b, &QPushButton::clicked, this, [this] { flip(rtl_ ? 1 : -1); });
    connect(next_b, &QPushButton::clicked, this, [this] { flip(rtl_ ? -1 : 1); });
    auto* row = new QHBoxLayout;
    row->addWidget(prev_b);
    row->addWidget(slider, 1);
    row->addWidget(next_b);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(view, 1);
    layout->addLayout(row);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, &BookPreview::step);
}

const QPixmap& BookPreview::pixmap(int index) {
    if (const auto kept = cache_.find(index); kept != cache_.end()) return kept->second;
    const auto& [kind, page] = pages_[static_cast<std::size_t>(index)];
    QPixmap shown;
    try {
        render::RenderOptions options;
        options.mode = "print";
        options.finish = false;
        options.skip_unported = true;  // (a preview on screen: what this build cannot draw yet is left out)
        render::Image image = render::render_page(*page, kPreviewDpi, options, doc_.get()).image;
        const core::Json* cover = core::cover_of(*page);
        if (cover != nullptr && core::is_wrap_kind(cover->value("kind", std::string()))) {
            const std::string part = kind == "front" ? "表紙" : "裏表紙";
            for (const core::Fold& fold : core::folds(*page, core::to_string(doc_->binding))) {
                if (fold.name != part) continue;
                const core::Rect t = page->trim_rect_mm();
                image = image.crop(render::Box{render::mm_to_px(fold.x0, kPreviewDpi), render::mm_to_px(t.y.value(), kPreviewDpi),
                                               render::mm_to_px(fold.x1, kPreviewDpi), render::mm_to_px(t.y.value() + t.height.value(), kPreviewDpi)});
                break;
            }
        } else if (cover == nullptr || !core::is_wrap_kind(cover->value("kind", std::string()))) {
            image = formats::crop_to(image, *page, "trim", kPreviewDpi);
        }
        shown = QPixmap::fromImage(to_qimage(image));
    } catch (const std::exception& error) {
        // (a page that cannot be drawn — a broken asset, a cover that cannot be folded: said on a blank page of its size,
        // never ending the app)
        const core::Rect t = page->trim_rect_mm();
        QImage blank(std::max(40, render::mm_to_px(t.width.value(), kPreviewDpi)), std::max(40, render::mm_to_px(t.height.value(), kPreviewDpi)),
                     QImage::Format_RGB32);
        blank.fill(Qt::white);
        QPainter painter(&blank);
        painter.setPen(QColor(110, 110, 110));
        painter.drawText(blank.rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                         QStringLiteral("このページは表示できません\n%1").arg(wording::error(std::string(error.what()))));
        painter.end();
        shown = QPixmap::fromImage(blank);
    }
    return cache_[index] = shown;
}

std::pair<std::optional<int>, std::optional<int>> BookPreview::sides(int spread) const {
    if (groups_.empty()) return {std::nullopt, std::nullopt};
    const std::vector<int>& group = groups_[static_cast<std::size_t>(std::clamp(spread, 0, static_cast<int>(groups_.size()) - 1))];
    if (group.size() == 1) {
        const auto& [kind, page] = pages_[static_cast<std::size_t>(group[0])];
        if (kind == "page") {  // (a page alone lies on its own side of the open book)
            if (page->side(doc_->start_side.value_or(std::string())) == "left") return {group[0], std::nullopt};
            return {std::nullopt, group[0]};
        }
        // the cover (or the last page) lies on the side the book opens from
        const bool first = group[0] == 0;
        const bool alone_on_left = (first && rtl_) || (!first && !rtl_);
        if (alone_on_left) return {group[0], std::nullopt};
        return {std::nullopt, group[0]};
    }
    if (rtl_) return {group[1], group[0]};
    return {group[0], group[1]};
}

QString BookPreview::caption() const {
    if (groups_.empty()) return QStringLiteral("ページがありません");
    QStringList names;
    for (const int i : groups_[static_cast<std::size_t>(spread_)]) {
        const auto& [kind, page] = pages_[static_cast<std::size_t>(i)];
        names << (kind == "front" ? QStringLiteral("表紙") : kind == "back" ? QStringLiteral("裏表紙") : QStringLiteral("%1 ページ").arg(QString::fromStdString(page->index.repr())));
    }
    return names.join(QStringLiteral("・")) + QStringLiteral("　（%1 / %2）").arg(spread_ + 1).arg(groups_.size());
}

void BookPreview::go(int spread) {
    spread_ = std::clamp(spread, 0, std::max(0, static_cast<int>(groups_.size()) - 1));
    if (slider->value() != spread_) {
        const QSignalBlocker quiet(slider);
        slider->setValue(spread_);
    }
    view->update();
}

void BookPreview::flip(int step_by) {
    const int target = spread_ + step_by;
    if (!(0 <= target && target < static_cast<int>(groups_.size()))) return;
    const std::vector<int>& group = groups_[static_cast<std::size_t>(spread_)];
    turning_ = step_by > 0 ? group.back() : group.front();
    view->direction = step_by;
    view->turn = 0.01;
    target_ = target;
    timer_.start();
}

void BookPreview::step() {
    view->turn += 0.08;
    if (view->turn >= 0.5 && spread_ != target_) go(target_);
    if (view->turn >= 1) {
        view->turn = 0.0;
        turning_.reset();
        timer_.stop();
    }
    view->update();
}

void BookPreview::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left) {
        flip(rtl_ ? 1 : -1);
    } else if (event->key() == Qt::Key_Right) {
        flip(rtl_ ? -1 : 1);
    } else {
        QDialog::keyPressEvent(event);
    }
}

}  // namespace genko::app::bookview
