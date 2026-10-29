// The canvas's painting: the page's tiles, the lines being drawn and committed, and the overlays (Python's
// PageCanvas.paintEvent and _paint).

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/perf.hpp"
#include "app/theme.hpp"
#include "core/frames.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"

namespace genko::app {

namespace {

const QColor kChosen(QStringLiteral("#1c7ed6"));

// Does every pixel this paint can change (`region`, the canvas's: none — the whole canvas) lie inside `rect` (in the
// painter's coordinates now), with two pixels to spare? Then what is drawn outside it changes none of them: a large
// fill there is left out (the same pixels on the screen, the pen not held up by it while drawing).
bool painted_within(const QPainter& painter, const QRegion& region, const QRectF& rect) {
    if (region.isEmpty()) return false;
    bool invertible = false;
    const QTransform back = painter.transform().inverted(&invertible);
    if (!invertible) return false;
    const double scale = std::max(1e-6, std::sqrt(std::abs(painter.transform().determinant())));
    const double spare = 2.0 / scale;
    return rect.adjusted(spare, spare, -spare, -spare).contains(back.map(QPolygonF(QRectF(region.boundingRect()))).boundingRect());
}

// The sheet's rectangle filled (the page's white, a spread's partner): with fillRect while the view is not turned —
// moved, scaled or mirrored only (TxScale at most: its edges on the pixels as ever) —; in a turned view (TxRotate and
// beyond) as a path — Qt's fill of a turned rectangle can put an edge pixel otherwise when it is painted under a clip (a
// part painted again: a line being drawn asks for its piece) than when the whole canvas is, which a path's fill never
// does (PC4 E-0: found when the default クイックアクセス's longer names made the canvas narrower).
void fill_sheet(QPainter& painter, const QRectF& rect, const QColor& colour) {
    if (painter.transform().type() <= QTransform::TxScale) {
        painter.fillRect(rect, colour);
        return;
    }
    QPainterPath path;
    path.addRect(rect);
    painter.fillPath(path, colour);
}

}  // namespace

void PageCanvas::paintEvent(QPaintEvent* event) {
    if (gpu_surface_ != nullptr) {  // (the surface over the whole canvas draws it: canvas_gpu.cpp)
        gpu_surface_->update();
        return;
    }
    QPainter painter(this);
    painting_ = event->region();  // (the part asked to be painted again: a line being drawn asks for its piece)
    paint_frame(painter);
    painting_ = QRegion();
}

void PageCanvas::paint_frame(QPainter& painter) {
    if (glide_ && glide_t_ < 1.0) {
        // the scale moves evenly in steps (as a zoom feels), the page point in the middle slides straight
        const ViewState kept = view_;
        const auto& [from, to] = *glide_;
        const double w = width() / 2.0;
        const double h = height() / 2.0;
        const QPointF c0((w - from.pan.x()) / from.scale, (h - from.pan.y()) / from.scale);
        const QPointF c1((w - to.pan.x()) / to.scale, (h - to.pan.y()) / to.scale);
        const double t = glide_t_;
        view_.scale = from.scale * std::pow(to.scale / from.scale, t);
        const QPointF c = c0 + (c1 - c0) * t;
        view_.pan = QPointF(w - c.x() * view_.scale, h - c.y() * view_.scale);
        paint_page(painter);
        view_ = kept;
    } else {
        paint_page(painter);
    }
    static std::uint64_t frames = 0;
    perf::event("paint_done", {{"frame", ++frames}});
    if (launcher != nullptr) QMetaObject::invokeMethod(this, [this] { place_launcher(); }, Qt::QueuedConnection);  // (not moved while painting)
}

void PageCanvas::draw_in_page_px(QPainter& painter, const QImage& image, const QRect& box, int dpi) const {
    if (image.isNull()) return;
    const double mm = 25.4 / dpi;
    QTransform t = QTransform::fromScale(mm, mm) * QTransform(view_.scale, 0, 0, view_.scale, view_.pan.x(), view_.pan.y()) * view_transform();
    // (exactly one page pixel per screen pixel at 100 %, as the tiles are drawn)
    if (std::abs(std::abs(t.m11()) - 1.0) < 1e-9 && std::abs(std::abs(t.m22()) - 1.0) < 1e-9 && std::abs(t.m12()) < 1e-12 && std::abs(t.m21()) < 1e-12) {
        t = QTransform(t.m11() < 0 ? -1 : 1, 0, 0, t.m22() < 0 ? -1 : 1, std::round(t.dx()), std::round(t.dy()));  // (mirrored too: PC5 H-a)
    }
    const QTransform kept = painter.transform();
    painter.setTransform(t);
    // (PC5 H-a: the page's own pixels enlarged — 画素の段 — and ピクセルアートのプレビュー: nearest, as the tiles are laid)
    const bool smooth = painter.testRenderHint(QPainter::SmoothPixmapTransform);
    const int pixel = renderer_->pixel_dpi();
    if (renderer_->pixel_art() || (std::abs(t.determinant()) > 1.0 + 1e-9 && pixel > 0 && dpi >= pixel)) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }
    painter.drawImage(box.topLeft(), image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    painter.setTransform(kept);
}

void PageCanvas::paint_page(QPainter& painter) {
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), theme::surround());  // (a neutral grey: it does not sway how the page's greys look)
    const core::Page* p = page();
    if (p == nullptr) {
        painter.setPen(QColor(theme::tokens().muted));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("ページがありません"));
        return;
    }
    painter.setTransform(view_transform());
    const QPointF origin = pt(0, 0);
    const QRectF page_rect(origin.x(), origin.y(), p->spec.width_mm.value() * view_.scale, p->spec.height_mm.value() * view_.scale);
    if (p->spread_with && p->spread_with->truthy()) {
        // the partner sits on the other physical side; strokes drawn there go to the partner page
        const double step = p->spread_step_mm().value();
        const double offset = p->side() == "right" ? -step : step;
        fill_sheet(painter, page_rect.translated(offset * view_.scale, 0), QColor(QStringLiteral("#e9e4d8")));
    }
    draw_shadow(painter, page_rect);
    fill_sheet(painter, page_rect, Qt::white);
    if (pattern_preview && shown_frame_.isNull() && renderer_->any_shown()) {
        // (PC4 E3: the page's picture laid again around it, as far as the view reaches — its tiles there are —, at most the
        // 8 pages around it: a view far out never draws a page hundreds of times)
        const double w = p->spec.width_mm.value(), h = p->spec.height_mm.value();
        const QRectF seen = seen_mm();
        const QTransform kept = painter.transform();
        const QTransform to_screen = QTransform(view_.scale, 0, 0, view_.scale, view_.pan.x(), view_.pan.y()) * view_transform();
        if (w > 0 && h > 0) {
            const auto around = [](double at) { return std::clamp(static_cast<int>(std::floor(at)), -1, 1); };
            for (int j = around(seen.top() / h); j <= around(seen.bottom() / h); ++j) {
                for (int i = around(seen.left() / w); i <= around(seen.right() / w); ++i) {
                    if (i == 0 && j == 0) continue;
                    const QRectF there = seen.translated(-i * w, -j * h) & QRectF(0, 0, w, h);
                    if (there.isEmpty()) continue;
                    painter.setTransform(view_transform());
                    painter.fillRect(QRectF(pt(i * w, j * h), pt((i + 1) * w, (j + 1) * h)), Qt::white);
                    painter.setTransform(QTransform::fromTranslate(i * w, j * h) * to_screen);
                    renderer_->paint(painter, there);
                }
            }
        }
        painter.setTransform(kept);
    }
    if (!shown_frame_.isNull()) {
        painter.drawImage(page_rect, shown_frame_);
    } else if (renderer_->any_shown()) {
        const QTransform kept = painter.transform();
        painter.setTransform(QTransform(view_.scale, 0, 0, view_.scale, view_.pan.x(), view_.pan.y()) * view_transform());
        renderer_->paint(painter, seen_mm());
        painter.setTransform(kept);
    } else {
        draw_plain(painter);
    }
    // the lines committed a moment ago, exactly as their tiles will be, until the tiles are drawn
    for (const Overlay& overlay : overlays_) draw_in_page_px(painter, overlay.ink->image(), overlay.ink->box(), overlay.ink->dpi());
    if (committing_) draw_in_page_px(painter, committing_->image(), committing_->box(), committing_->dpi());
    draw_pixel_grid(painter);  // (PC5 H-a: ピクセルグリッド, over the page's pixels and under the guides: canvas_view.cpp)
    painter.setRenderHint(QPainter::Antialiasing);
    if (show_guides) draw_guides(painter, page_rect);
    if (phone_view) draw_phone(painter);  // (the phone screens: canvas_scale.cpp)
    draw_grid(painter);    // (canvas_guides.cpp)
    draw_rulers(painter);
    draw_selection(painter);
    draw_lines(painter);  // (the balloons' boxes and the chosen one's handles: canvas_lines.cpp)
    if (tool_drag_ && tool_ == QLatin1String("move") && !move_image_.isNull()) {
        // the layer being moved: its picture following the pen
        const QPointF d = tool_drag_->second - tool_drag_->first;
        const QRectF where(pt(move_where_.x() + d.x(), move_where_.y() + d.y()),
                           pt(move_where_.right() + d.x(), move_where_.bottom() + d.y()));
        painter.setOpacity(0.7);
        painter.drawImage(where, move_image_);
        painter.setOpacity(1.0);
        painter.setPen(QPen(kChosen, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(where);
    }
    draw_marquee(painter);  // (the selection, its handles, what the marquee tool is drawing: canvas_select.cpp)
    draw_crop(painter);       // (the crop tool's box: canvas_crop.cpp)
    draw_transform(painter);  // (自由変形 and the warps' box: canvas_transform.cpp)
    draw_tools(painter);      // (the area being filled, the gradient's drag, the figure being drawn: canvas_tools.cpp)
    draw_move_box(painter);   // (the move tool's バウンディングボックス: canvas_snap.cpp)
    draw_snap(painter);       // (what a drag snapped to: canvas_snap.cpp)
    draw_prims(painter);    // (the 3D tool's handles: canvas_guides.cpp)
    draw_effect_handles(painter);  // (the effect tool's centres: canvas_effects.cpp)
    if (work_ && work_->paint) {   // (PC4 D4: a piece of work over the page — ぼかしギャラリー's pins)
        painter.save();
        work_->paint(painter, [this](QPointF mm) { return pt(mm.x(), mm.y()); }, view_.scale);
        painter.restore();
    }
    draw_highlight(painter);       // (a problem the checks found: canvas_scale.cpp)
    if (zoom_drag_) {  // (the area the magnifier will fill the view with)
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme::accent(), 1.2, Qt::DashLine));
        painter.drawRect(QRectF(zoom_drag_->first, zoom_drag_->second).normalized());
    }
    draw_balloon_stroke(painter);  // (the balloon pen's outline)
    if (!stroke_.empty() && draws_line(tool_) && live_) {
        if (live_snapped_) {
            for (const auto& copy : live_copies_) draw_in_page_px(painter, copy->image(), copy->box(), copy->dpi());
        }
        draw_in_page_px(painter, live_->image(), live_->box(), live_->dpi());
    } else if (!stroke_.empty()) {
        const QColor colour = draws_line(tool_) ? theme::accent() : QColor(200, 60, 60, 160);
        const double width_mm = is_retouch_tool(tool_)  ? retouch_mm      // (PC3 B4: the retouching brush's own)
                                : is_pixel_tool(tool_) ? pixel_tool_mm  // (PC4 E5)
                                                       : brush_width_mm;
        QPen line(colour, std::max(1.5, width_mm * view_.scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(line);
        painter.setBrush(Qt::NoBrush);
        // the line snapped to the rulers (the pen: with its symmetry copies; the eraser: the line alone)
        std::optional<std::vector<core::PenPoints>> snapped;
        if (draws_line(tool_) || tool_ == QLatin1String("eraser")) snapped = snapped_preview(stroke_);
        if (snapped && tool_ == QLatin1String("eraser")) snapped->resize(1);
        std::vector<const core::PenPoints*> shown;
        if (snapped) {
            for (const core::PenPoints& points : *snapped) shown.push_back(&points);
        } else {
            shown.push_back(&stroke_);
        }
        for (const core::PenPoints* points : shown) {
            if (points->size() < 2) continue;
            QPainterPath path(pt((*points)[0].x, (*points)[0].y));
            for (std::size_t i = 1; i < points->size(); ++i) path.lineTo(pt((*points)[i].x, (*points)[i].y));
            painter.drawPath(path);
        }
    }
    if (hover_ && stroke_.empty() && is_stroke_tool(tool_) && brush_preview) {
        // the brush's circle and (circle, dot) its centre, as the pointer chosen in 環境設定 (the cross is the system's)
        const QPointF h = pt(hover_->x(), hover_->y());
        const double size = draws_line(tool_)                  ? brush_width_mm
                            : tool_ == QLatin1String("eraser") ? eraser_mm
                            : is_retouch_tool(tool_)           ? retouch_mm  // (PC3 B4)
                            : is_pixel_tool(tool_)             ? pixel_tool_mm  // (PC4 E5)
                                                               : blend_mm;
        const double radius = std::max(2.0, size / 2 * view_.scale);
        painter.setPen(QPen(theme::accent(), 1));
        painter.setBrush(Qt::NoBrush);
        // (PC4 E1: a dab brush's own tip — one dab at the view's scale, its outline where it is half covered)
        const bool tip = cursor_kind == QLatin1String("tip") && draws_line(tool_) && draw_tip(painter, h, size);
        if (!tip && (cursor_kind == QLatin1String("circle") || cursor_kind == QLatin1String("circle_cross") || cursor_kind == QLatin1String("tip"))) {
            painter.drawEllipse(h, radius, radius);
        }
        if (cursor_kind == QLatin1String("circle") || cursor_kind == QLatin1String("dot") || cursor_kind == QLatin1String("tip")) {
            painter.setBrush(theme::accent());
            painter.drawEllipse(h, 1.5, 1.5);
            painter.setBrush(Qt::NoBrush);
        }
    }
    draw_retouch(painter);     // (PC3 B4: the clone source's cross and overlay, retouch_tools.cpp)
    draw_guide_drag(painter);  // (a guide line pulled from a scale, and the scales: canvas_scale.cpp)
    draw_scale(painter);
}

bool PageCanvas::draw_tip(QPainter& painter, const QPointF& at, double size_mm) const {
    if (pointer_kind.empty()) return false;
    const render::brushes::Brush b = render::brushes::brush(pointer_kind);
    if (!b.dabs) return false;
    try {
        // one dab of the brush's tip (its angle and roundness, no jitter) at the view's scale, its half-covered edge drawn
        const double px_mm = std::max(view_.scale, 0.05);
        const int dpi = std::clamp(static_cast<int>(std::lround(px_mm * 25.4)), 10, 2400);
        render::dabs::Dab d;
        d.radius = std::max(size_mm / 2.0, 0.05);
        d.angle = b.dabs->angle;
        d.roundness = b.dabs->roundness;
        d.flip_x = b.dabs->flip_x;
        d.flip_y = b.dabs->flip_y;
        d.x = d.radius * 2;
        d.y = d.radius * 2;
        const auto ctx = render::dabs::context(*b.dabs, dpi);
        const render::Box rect = ctx->dab_rect(d, false);
        if (rect.width() <= 0 || rect.height() <= 0 || rect.width() > 2048 || rect.height() > 2048) return false;
        std::vector<float> values;
        ctx->rasterize(d, false, rect, values);
        QImage shape(rect.width(), rect.height(), QImage::Format_ARGB32_Premultiplied);
        shape.fill(Qt::transparent);
        const QColor accent = theme::accent();
        for (int y = 0; y < rect.height(); ++y) {
            for (int x = 0; x < rect.width(); ++x) {
                const auto inside = [&](int xx, int yy) {
                    return xx >= 0 && yy >= 0 && xx < rect.width() && yy < rect.height() && values[std::size_t(yy) * std::size_t(rect.width()) + std::size_t(xx)] >= 0.5f;
                };
                if (inside(x, y) && (!inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1))) shape.setPixelColor(x, y, accent);
            }
        }
        const double cx = d.x * dpi / 25.4, cy = d.y * dpi / 25.4;
        painter.save();
        painter.translate(at);
        painter.scale(px_mm * 25.4 / dpi, px_mm * 25.4 / dpi);
        painter.drawImage(QPointF(rect.x0 - cx, rect.y0 - cy), shape);
        painter.restore();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void PageCanvas::draw_shadow(QPainter& painter, const QRectF& page_rect) const {
    // a soft shadow under the sheet, so the page sits on the surround like paper on a desk (none of it shows where
    // this paint is all under the sheet's white)
    if (painted_within(painter, painting_, page_rect)) return;
    const double scale = std::max(0.2, std::sqrt(std::abs(painter.transform().determinant())));
    painter.setPen(Qt::NoPen);
    const bool dark = theme::tokens().dark;
    for (int i = 6; i >= 1; --i) {
        const double grow = i * 1.6 / scale;
        painter.setBrush(QColor(0, 0, 0, (dark ? 14 : 9) + (6 - i) * (dark ? 5 : 3)));
        painter.drawRoundedRect(page_rect.adjusted(-grow, -grow + 1.5 / scale, grow, grow + 3 / scale), grow, grow);
    }
    painter.setBrush(Qt::NoBrush);
}

void PageCanvas::draw_guides(QPainter& painter, const QRectF& page_rect) const {
    // the bleed (cut off, shaded), the trim line (the finished size) and the basic frame
    const core::Page* p = page();
    const auto box = [this](const core::Rect& r) {
        const QPointF a = pt(r.x.value(), r.y.value());
        return QRectF(a.x(), a.y(), r.width.value() * view_.scale, r.height.value() * view_.scale);
    };
    const QRectF bleed = box(p->bleed_rect_mm());
    const QRectF trim = box(p->trim_rect_mm());
    if (!painted_within(painter, painting_, trim)) {  // (the shaded bands lie outside the trim line)
        QPainterPath outside;
        outside.addRect(page_rect);
        QPainterPath inside;
        inside.addRect(bleed);
        painter.fillPath(outside.subtracted(inside), QColor(90, 92, 100, 26));
        QPainterPath band;
        band.addRect(bleed);
        QPainterPath cut;
        cut.addRect(trim);
        painter.fillPath(band.subtracted(cut), QColor(120, 122, 130, 22));
    }
    painter.setBrush(Qt::NoBrush);
    if (p->spec.bleed_mm.value() > 0) {
        painter.setPen(QPen(QColor(120, 122, 130, 110), 1, Qt::DotLine));
        painter.drawRect(bleed);
    }
    painter.setPen(QPen(QColor(200, 60, 120, 130), 1));
    painter.drawRect(trim);
    painter.setPen(QPen(QColor(40, 126, 214, 95), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box(p->inner_rect_mm()));
}

void PageCanvas::draw_polygon_mm(QPainter& painter, const std::vector<QPointF>& points) const {
    QPolygonF polygon;
    for (const QPointF& q : points) polygon << pt(q.x(), q.y());
    painter.drawPolygon(polygon);
}

void PageCanvas::draw_plain(QPainter& painter) const {
    // before the first tiles: the panels, so the page is never blank
    painter.setPen(QPen(QColor(QStringLiteral("#111111")), 2));
    painter.setBrush(Qt::NoBrush);
    for (const core::Frame* frame : page()->leaf_frames()) draw_polygon_mm(painter, outline_of(*frame));
    for (const core::StoryLine* line : lines()) draw_balloon_box(painter, *line, std::nullopt);  // (and the balloons' boxes)
}

void PageCanvas::draw_selection(QPainter& painter) const {
    const core::Page* p = page();
    const core::Frame* hover_frame = nullptr;
    if ((tool_ == QLatin1String("select") || tool_ == QLatin1String("frame")) && hover_ && !frame_drag_) {
        hover_frame = p->frame_at(core::Num(hover_->x()), core::Num(hover_->y()));
    }
    const std::string chosen = p->selected_frame_id.is_string() ? p->selected_frame_id.get<std::string>() : std::string();
    for (const core::Frame* frame : p->leaf_frames()) {
        if (!chosen.empty() && frame->id == chosen) {
            painter.setPen(QPen(kChosen, 3));
            painter.setBrush(QColor(28, 126, 214, 16));
            draw_polygon_mm(painter, outline_of(*frame));
        } else if (hover_frame != nullptr && frame->id == hover_frame->id) {
            painter.setPen(QPen(QColor(28, 126, 214, 150), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            draw_polygon_mm(painter, outline_of(*frame));
        }
    }
    painter.setBrush(Qt::NoBrush);
    if (show_frame_numbers) draw_frame_numbers(painter);
    if (tool_ == QLatin1String("frame")) draw_frame_tool(painter);
}

void PageCanvas::draw_frame_numbers(QPainter& painter) const {
    // コマ番号: each panel's place in the reading order, in a small circle at its top outer corner
    const core::Page* p = page();
    if (p->frames.empty()) return;
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    int n = 0;
    for (const core::Frame* frame : leaves_in_reading_order(p->frames[0], binding)) {
        ++n;
        const std::vector<QPointF> pts = shape_of(*frame);
        if (pts.empty()) continue;
        const auto key = [this](const QPointF& q) { return (binding == core::Binding::Right ? q.x() : -q.x()) - q.y(); };
        QPointF corner = pts[0];
        for (const QPointF& q : pts) {
            if (key(q) > key(corner)) corner = q;  // (max(): the first of equal keys)
        }
        QPointF q = pt(corner.x(), corner.y());
        q = QPointF(q.x() + (binding == core::Binding::Right ? -14 : 14), q.y() + 14);
        painter.setPen(QPen(kChosen, 1.5));
        painter.setBrush(QColor(255, 255, 255, 230));
        painter.drawEllipse(q, 10, 10);
        painter.setPen(kChosen);
        painter.drawText(QRectF(q.x() - 10, q.y() - 10, 20, 20), Qt::AlignCenter, QString::number(n));
    }
    painter.setBrush(Qt::NoBrush);
}

void PageCanvas::draw_frame_tool(QPainter& painter) const {
    const core::Page* p = page();
    const FrameDrag* drag = frame_drag_ ? &*frame_drag_ : nullptr;
    const std::optional<Gutter> hover = hover_ && !drag ? hit_gutter(hover_->x(), hover_->y()) : std::nullopt;
    if (!p->frames.empty()) {
        for (const Gutter& gutter : gutters(p->frames[0])) {
            const bool held = drag != nullptr && drag->kind == QLatin1String("gutter") && drag->gutter.node == gutter.node &&
                              drag->gutter.index == gutter.index;
            const bool strong = (hover && hover->node == gutter.node && hover->index == gutter.index) || held;
            if (!strong) continue;
            QPointF p0 = gutter.p0;
            QPointF p1 = gutter.p1;
            if (held) {
                p0 += drag->offset;
                p1 += drag->offset;
            }
            painter.setPen(QPen(QColor(232, 89, 12, 160), std::max(3.0, gutter.width * view_.scale)));
            painter.drawLine(pt(p0.x(), p0.y()), pt(p1.x(), p1.y()));
        }
    }
    if (drag != nullptr && drag->kind == QLatin1String("cut")) {
        painter.setPen(QPen(QColor(QStringLiteral("#e03131")), 2, Qt::DashLine));
        painter.drawLine(pt(drag->p0.x(), drag->p0.y()), pt(drag->p1.x(), drag->p1.y()));
    }
    if ((drag != nullptr && (drag->kind == QLatin1String("rect") || drag->kind == QLatin1String("free"))) || !frame_poly_.empty()) {
        painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        if (drag != nullptr && drag->kind == QLatin1String("rect")) {
            painter.drawRect(QRectF(pt(drag->p0.x(), drag->p0.y()), pt(drag->p1.x(), drag->p1.y())).normalized());
        } else if (drag != nullptr && drag->kind == QLatin1String("free")) {
            draw_polygon_mm(painter, drag->points);
        } else {
            QPolygonF line;
            for (const QPointF& q : frame_poly_) line << pt(q.x(), q.y());
            if (hover_) line << pt(hover_->x(), hover_->y());
            painter.drawPolyline(line);
            painter.setBrush(Qt::white);
            painter.drawEllipse(line.front(), kHandlePx / 2.0 + 2, kHandlePx / 2.0 + 2);
            painter.setBrush(Qt::NoBrush);
        }
    }
    if (drag != nullptr && drag->kind == QLatin1String("vertex")) {
        painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
        draw_polygon_mm(painter, drag->poly);
    }
    for (const auto& [index, at] : vertex_handles()) {
        painter.setPen(QPen(kChosen, 1.5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(pt(at.x(), at.y()), kHandlePx / 2.0 + 1, kHandlePx / 2.0 + 1);
    }
    for (const auto& [index, at] : bow_handles()) {
        const QPointF q = pt(at.x(), at.y());
        const double r = kHandlePx / 2.0 + 1;
        painter.setPen(QPen(theme::accent(), 1.5));
        painter.setBrush(Qt::white);
        painter.drawPolygon(QPolygonF({QPointF(q.x(), q.y() - r), QPointF(q.x() + r, q.y()), QPointF(q.x(), q.y() + r), QPointF(q.x() - r, q.y())}));
    }
    if (drag != nullptr && drag->kind == QLatin1String("bow")) {
        if (const core::Frame* frame = p->find_frame(drag->frame.toStdString())) {
            core::Frame ghost = *frame;
            std::vector<double> curves = drag->curves;
            curves[static_cast<std::size_t>(drag->edge)] = drag->mm;
            ghost.curves = curves;
            painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            draw_polygon_mm(painter, outline_of(ghost));
        }
    }
    painter.setBrush(Qt::NoBrush);
}

}  // namespace genko::app
