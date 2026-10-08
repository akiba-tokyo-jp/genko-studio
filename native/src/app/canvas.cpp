#include "app/canvas.hpp"

#include <QCoreApplication>
#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include "app/config.hpp"
#include "app/perf.hpp"
#include "app/theme.hpp"
#include "core/frames.hpp"

namespace genko::app {

bool reduce_motion() {
    const QString value = settings()->value(QStringLiteral("ui/reduce_motion"), QString()).toString().toLower();
    if (value.isEmpty()) return false;  // (the system's own setting: Qt does not tell it on every platform)
    return value == QLatin1String("1") || value == QLatin1String("true") || value == QLatin1String("yes");
}

PageCanvas::PageCanvas(QWidget* parent) : QWidget(parent), renderer_(std::make_unique<PageRenderer>()) {
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    grabGesture(Qt::PinchGesture);
    setMouseTracking(true);
    setAttribute(Qt::WA_TabletTracking, true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(300, 280);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    pan_clock_.start();
    coast_timer_.setInterval(16);
    connect(&coast_timer_, &QTimer::timeout, this, [this] {
        // a page let go while moving slides on a little and slows to a stop (as on a tablet)
        pan_speed_ *= 0.88;
        if (std::abs(pan_speed_.x()) + std::abs(pan_speed_.y()) < 0.3) {
            stop_coast();
            return;
        }
        view_.pan += pan_speed_;
        view_.fitted = false;
        rerender_.start();
        update();
    });
    rerender_.setSingleShot(true);
    rerender_.setInterval(60);
    connect(&rerender_, &QTimer::timeout, this, &PageCanvas::request_tiles);
    connect(renderer_.get(), &PageRenderer::updated, this, [this](const QRectF& area_mm) {
        // committed lines whose tiles are drawn now: their exact pictures are no longer needed
        QRectF gone;
        std::erase_if(overlays_, [this, &gone](const Overlay& o) {
            if (!renderer_->current(o.ink->dpi(), o.ink->box_mm())) return false;
            gone = gone.united(o.ink->box_mm());
            return true;
        });
        perf::event("tiles_updated", {{"settled", renderer_->settled()}});
        if (area_mm.isEmpty()) {
            update();
            return;
        }
        // (only the part of the screen with new pixels is painted again)
        update(mm_transform().mapRect(area_mm.united(gone)).toAlignedRect().adjusted(-2, -2, 2, 2));
    });
    connect(renderer_.get(), &PageRenderer::omittedChanged, this, &PageCanvas::omittedChanged);
    connect(renderer_.get(), &PageRenderer::failed, this, &PageCanvas::renderFailed);
    // (timings: a page's first picture, rough or not, and its fine one)
    connect(renderer_.get(), &PageRenderer::firstShown, this, [] { perf::event("page_first_shown"); });
    connect(renderer_.get(), &PageRenderer::settledChanged, this, [this] {
        if (renderer_->settled()) perf::event("page_settled", {{"dpi", renderer_->shown_dpi()}});
    });
}

PageCanvas::~PageCanvas() = default;

// --- the page --------------------------------------------------------------------------------------------------

const core::Page* PageCanvas::page() const { return doc_ && index_ < doc_->pages.size() ? doc_->pages[index_].get() : nullptr; }

void PageCanvas::set_page(DocPtr doc, std::size_t index) {
    const core::Page* before = page();
    if (!doc || index >= doc->pages.size()) {
        clear_page();
        return;
    }
    const core::Page& now = *doc->pages[index];
    const bool other_page = before == nullptr || before->id != now.id;
    const bool size_changed = before == nullptr || !(before->spec.width_mm == now.spec.width_mm && before->spec.height_mm == now.spec.height_mm);
    doc_ = std::move(doc);
    index_ = index;
    shown_frame_ = QImage();  // (a frame played: until the page changes)
    if (other_page) {
        stroke_.clear();
        live_.reset();
        overlays_.clear();
        frame_drag_.reset();
        frame_poly_.clear();
        line_drag_.reset();
        handle_drag_.reset();
    }
    balloon_stroke_.clear();  // (Python's set_page: _stroke = [])
    if (size_changed || view_.fitted) fit_page();
    renderer_->show(doc_, index_);
    request_tiles();
    update();
}

void PageCanvas::clear_page() {
    doc_.reset();
    index_ = 0;
    live_.reset();
    overlays_.clear();
    stroke_.clear();
    renderer_->clear();
    update();
}

void PageCanvas::set_render_mode(const std::string& mode) { renderer_->set_mode(mode); }

void PageCanvas::show_frame(const QImage& picture) {
    shown_frame_ = picture;
    update();
}

void PageCanvas::request_tiles() {
    if (page() == nullptr || width() <= 0 || height() <= 0) return;
    renderer_->want(base_dpi(), wanted_dpi(), seen_mm());
}

bool PageCanvas::wait_rendered(int ms) {
    QElapsedTimer clock;
    clock.start();
    request_tiles();
    while (!renderer_->settled()) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCoreApplication::processEvents();
    return true;
}

// --- tools -------------------------------------------------------------------------------------------------------

void PageCanvas::set_tool(const QString& tool) {
    tool_ = tool;
    held_tool_.reset();  // (a tool chosen while a key holds another is the tool now: letting go keeps it)
    stroke_.clear();
    marquee_stroke_.clear();
    lasso_fill_.clear();  // (Python's _stroke: the area being drawn round to fill, too)
    balloon_stroke_.clear();  // (and the balloon pen's outline)
    ruler_draft_.reset();
    live_.reset();
    update_cursor();
    update();
}

void PageCanvas::set_live_pen(const core::Json& fields, std::optional<std::string> layer_id) {
    pen_fields_ = fields;
    pen_layer_ = std::move(layer_id);
}

void PageCanvas::set_move_image(const QImage& image, const QRectF& where_mm) {
    move_image_ = image;
    move_where_ = where_mm;
    update();
}

void PageCanvas::stroke_applied(const std::string& id) {
    std::unique_ptr<LiveInk> ink = std::move(committing_);
    const core::Page* p = page();
    if (!ink || p == nullptr) return;
    for (const core::Layer& layer : p->layers) {
        for (const core::StrokePtr& stroke : layer.strokes->items) {
            if (stroke->id != id) continue;
            // the committed line (smoothed, tapered, with its id): exactly as its tiles will be, until they are
            auto kept = std::make_unique<LiveInk>(doc_, *p, layer, ink->dpi(), pen_fields_, id);
            kept->finish(*stroke);
            if (!renderer_->current(kept->dpi(), kept->box_mm())) overlays_.push_back(Overlay{std::move(kept), renderer_->generation()});
            update();
            return;
        }
    }
    update();
}

void PageCanvas::stroke_dropped() {
    committing_.reset();
    update();
}

// --- the view ------------------------------------------------------------------------------------------------------

QTransform PageCanvas::view_transform() const {
    QTransform view;
    if (view_.rotation == 0.0 && !view_.flipped && !view_.flipped_v) return view;
    const double cx = width() / 2.0;
    const double cy = height() / 2.0;
    view.translate(cx, cy);
    view.rotate(view_.rotation);
    if (view_.flipped || view_.flipped_v) view.scale(view_.flipped ? -1 : 1, view_.flipped_v ? -1 : 1);
    view.translate(-cx, -cy);
    return view;
}

QTransform PageCanvas::mm_transform() const {
    return QTransform(view_.scale, 0, 0, view_.scale, view_.pan.x(), view_.pan.y()) * view_transform();
}

QPointF PageCanvas::unturned(const QPointF& widget) const {
    if (view_.rotation == 0.0 && !view_.flipped && !view_.flipped_v) return widget;
    return view_transform().inverted().map(widget);
}

QPointF PageCanvas::mm_of(const QPointF& p) const { return QPointF((p.x() - view_.pan.x()) / view_.scale, (p.y() - view_.pan.y()) / view_.scale); }

QPointF PageCanvas::pt(double x_mm, double y_mm) const {
    return QPointF(x_mm * view_.scale + view_.pan.x(), y_mm * view_.scale + view_.pan.y());
}

QPointF PageCanvas::to_mm(const QPointF& widget) const { return mm_of(unturned(widget)); }

QPointF PageCanvas::to_widget(const QPointF& mm) const { return mm_transform().map(mm); }

QRectF PageCanvas::seen_mm() const {
    const QPointF corners[] = {to_mm(QPointF(0, 0)), to_mm(QPointF(width(), 0)), to_mm(QPointF(0, height())),
                               to_mm(QPointF(width(), height()))};
    double x0 = corners[0].x(), x1 = x0, y0 = corners[0].y(), y1 = y0;
    for (const QPointF& p : corners) {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    return QRectF(x0, y0, x1 - x0, y1 - y0);
}

QRectF PageCanvas::view_rect_mm() const {
    const core::Page* p = page();
    double x0 = 0.0;
    double w = p->spec.width_mm.value();
    if (p->spread_with && p->spread_with->truthy()) {
        const double step = p->spread_step_mm().value();  // the partner's finished size meets this one's at the gutter
        if (p->side() == "right") {
            x0 = -step;
            w += step;
        } else {
            w += step;
        }
    }
    return QRectF(x0, 0.0, w, p->spec.height_mm.value());
}

int PageCanvas::wanted_dpi() const {
    const double ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    const double dpi = view_.scale * 25.4 * ratio;
    return static_cast<int>(std::max(48.0, std::min<double>(PageRenderer::kDetailDpi, std::nearbyint(dpi / 24) * 24)));
}

void PageCanvas::fit_page() {
    if (page() == nullptr || width() <= 0 || height() <= 0) return;
    const QRectF r = view_rect_mm();
    const double margin = 16;
    view_.scale = std::max(kMinScale, std::min(kMaxScale, std::min((width() - 2 * margin) / r.width(), (height() - 2 * margin) / r.height())));
    view_.pan = QPointF((width() - r.width() * view_.scale) / 2 - r.x() * view_.scale, (height() - r.height() * view_.scale) / 2 - r.y() * view_.scale);
    view_.fitted = true;
    after_zoom();
}

void PageCanvas::zoom_by(double factor, std::optional<QPointF> anchor) {
    const QPointF at = anchor.value_or(QPointF(width() / 2.0, height() / 2.0));
    const QPointF mm = mm_of(at);
    view_.scale = std::max(kMinScale, std::min(kMaxScale, view_.scale * factor));
    view_.pan = QPointF(at.x() - mm.x() * view_.scale, at.y() - mm.y() * view_.scale);
    view_.fitted = false;
    after_zoom();
}

void PageCanvas::center_on(double x_mm, double y_mm) {
    view_.pan = QPointF(width() / 2.0 - x_mm * view_.scale, height() / 2.0 - y_mm * view_.scale);
    view_.fitted = false;
    live_reset();
    rerender_.start();
    emit changed();
    update();
}

void PageCanvas::actual_size() { zoom_by((96 / 25.4) / view_.scale); }

void PageCanvas::after_zoom() {
    live_reset();
    rerender_.start(160);
    emit zoomChanged(view_.scale);
    update();
}

int PageCanvas::zoom_percent() const { return static_cast<int>(std::nearbyint(view_.scale / (96 / 25.4) * 100)); }

void PageCanvas::set_zoom_percent(double percent) {
    percent = std::max(1.0, percent);
    zoom_by((percent / 100 * 96 / 25.4) / view_.scale);
}

void PageCanvas::zoom_to_rect(double x0, double y0, double x1, double y1) {
    const double w = std::abs(x1 - x0);
    const double h = std::abs(y1 - y0);
    if (w < 0.5 || h < 0.5) return;
    view_.scale = std::max(kMinScale, std::min(kMaxScale, std::min(width() / w, height() / h) * 0.95));
    const double cx = (x0 + x1) / 2;
    const double cy = (y0 + y1) / 2;
    view_.pan = QPointF(width() / 2.0 - cx * view_.scale, height() / 2.0 - cy * view_.scale);
    view_.fitted = false;
    after_zoom();
}

void PageCanvas::rotate_view(double degrees) { set_rotation(view_.rotation + degrees); }

void PageCanvas::set_rotation(double degrees) {
    const double turned = std::fmod(std::fmod(degrees + 180.0, 360.0) + 360.0, 360.0) - 180.0;  // (Python's % keeps the sign of 360)
    view_.rotation = std::abs(turned) < 0.01 ? 0.0 : turned;
    live_reset();
    rerender_.start();
    emit changed();
    update();
}

void PageCanvas::flip_view(std::optional<bool> on) {
    view_.flipped = on.has_value() ? *on : !view_.flipped;
    live_reset();
    emit changed();
    update();
}

void PageCanvas::flip_view_vertical(std::optional<bool> on) {
    view_.flipped_v = on.has_value() ? *on : !view_.flipped_v;
    live_reset();
    emit changed();
    update();
}

void PageCanvas::reset_view() {
    view_.rotation = 0.0;
    view_.flipped = false;
    view_.flipped_v = false;
    fit_page();
    emit changed();
}

void PageCanvas::set_view_state(const ViewState& state) {
    view_.rotation = state.rotation;
    view_.flipped = state.flipped;
    view_.flipped_v = state.flipped_v;
    if (state.fitted) {
        fit_page();
        return;
    }
    view_.scale = std::max(kMinScale, std::min(kMaxScale, state.scale));
    view_.pan = state.pan;
    view_.fitted = false;
    after_zoom();
}

void PageCanvas::glide(const std::function<void()>& change) {
    const ViewState before = view_;
    change();
    const ViewState after = view_;
    if (reduce_motion() || !isVisible() || before.scale <= 0 ||
        (before.scale == after.scale && before.pan == after.pan)) {
        return;
    }
    glide_ = std::make_pair(before, after);
    glide_t_ = 0.0;
    if (glide_motion_ == nullptr) {
        glide_motion_ = new QVariantAnimation(this);
        glide_motion_->setDuration(170);
        glide_motion_->setStartValue(0.0);
        glide_motion_->setEndValue(1.0);
        glide_motion_->setEasingCurve(QEasingCurve::OutCubic);
        connect(glide_motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            glide_t_ = value.toDouble();
            update();
        });
        connect(glide_motion_, &QVariantAnimation::finished, this, [this] {
            glide_.reset();
            update();
        });
    }
    glide_motion_->stop();
    glide_motion_->start();
}

void PageCanvas::pinch(double factor, double turn_deg, const QPointF& centre, const QPointF& moved) {
    if (moved.x() != 0.0 || moved.y() != 0.0) {
        view_.pan += moved;
        view_.fitted = false;
    }
    if (std::abs(turn_deg) > 0.01) rotate_view(turn_deg);
    if (factor != 0.0 && std::abs(factor - 1.0) > 1e-3) zoom_by(factor, unturned(centre));
    rerender_.start();
    update();
}

void PageCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (view_.fitted) fit_page();
    rerender_.start();
}

// --- the line being drawn ------------------------------------------------------------------------------------------

void PageCanvas::live_reset() {
    // (the view moved: a live line is drawn again at the new resolution with its next point)
    if (live_ && live_->dpi() != wanted_dpi()) live_.reset();
    std::erase_if(overlays_, [this](const Overlay& o) { return o.ink->dpi() != wanted_dpi(); });
}

void PageCanvas::live_sync() {
    const core::Page* p = page();
    if (p == nullptr || tool_ != QLatin1String("pen") || !pen_layer_ || stroke_.empty()) return;
    const core::Layer* layer = nullptr;
    for (const core::Layer& item : p->layers) {
        if (item.id == *pen_layer_) layer = &item;
    }
    if (layer == nullptr) return;
    const auto shown = snapped_preview(stroke_);
    if (!shown) {
        if (live_snapped_) {  // (no longer snapped: drawn plainly again, from the start)
            live_.reset();
            live_copies_.clear();
            live_snapped_ = false;
        }
        if (!live_) live_ = std::make_unique<LiveInk>(doc_, *p, *layer, wanted_dpi(), pen_fields_, stroke_id_);
        live_->follow(stroke_, turns_);
        return;
    }
    // snapped to a ruler, with its symmetry copies: drawn whole again each time (Python's LiveInk.redraw)
    const core::PenPoints& main = shown->front();
    live_ = std::make_unique<LiveInk>(doc_, *p, *layer, wanted_dpi(), pen_fields_, stroke_id_);
    live_->follow(main, main.size() == turns_.size() ? std::span<const double>(turns_) : std::span<const double>());
    live_copies_.clear();
    for (std::size_t i = 1; i < shown->size(); ++i) {
        auto copy = std::make_unique<LiveInk>(doc_, *p, *layer, wanted_dpi(), pen_fields_, stroke_id_);
        copy->follow((*shown)[i]);
        live_copies_.push_back(std::move(copy));
    }
    live_snapped_ = true;
}

}  // namespace genko::app
