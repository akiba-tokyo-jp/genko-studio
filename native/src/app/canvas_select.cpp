// The canvas's selection (Python's genko/app/canvas.py "selections" and canvas_shapes.py): the marquee tool's ways
// of choosing — a rectangle, an ellipse, a lasso, a polyline, auto-select, the selection pen and eraser — with Shift to
// add, Alt to take away, both for the overlap (PC3 B1: else as the 選択方法 buttons say); an ellipse, a lasso and a polygon
// anti-aliased and feathered as the tool says, a rectangle or an ellipse of a fixed ratio or size; the chosen area moved,
// scaled, turned and slanted by its handles (選択範囲を変形: its outline only), or pulled freely (perspective, mesh); a mask
// selection shown as a tint; the selection let go kept for 再選択. PC3 B3: クイック選択ツール's stroke and オブジェクト選択ツール's
// rectangle (the window finds the area they say on a worker thread).

#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <functional>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"
#include "render/selection.hpp"
#include "render/warp.hpp"

namespace genko::app {

using core::Json;

namespace {

const QColor kSelection(QStringLiteral("#1c7ed6"));
constexpr double kPi = 3.14159265358979323846;

double r3(double v) { return core::py_round(v, 3); }

std::vector<QPointF> through(const std::array<double, 6>& m, const std::vector<QPointF>& points) {
    const auto [a, b, c, d, e, f] = m;
    std::vector<QPointF> out;
    out.reserve(points.size());
    for (const QPointF& p : points) out.emplace_back(a * p.x() + c * p.y() + e, b * p.x() + d * p.y() + f);
    return out;
}

double distance(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

}  // namespace

QString PageCanvas::how_from(Qt::KeyboardModifiers modifiers) {
    const bool shift = (modifiers & Qt::ShiftModifier) != 0;
    const bool alt = (modifiers & Qt::AltModifier) != 0;
    return shift && alt ? QStringLiteral("intersect") : shift ? QStringLiteral("add") : alt ? QStringLiteral("subtract") : QStringLiteral("replace");
}

void PageCanvas::set_selection(std::optional<Json> area, std::optional<std::vector<QPointF>> outline) {
    if (!area && selection_ && page() != nullptr) previous_ = std::make_pair(page()->id, *selection_);  // (再選択)
    if (!area) transform_outline = false;
    warp_.reset();
    sel_pivot_.reset();
    sel_drag_.reset();
    mask_picture_.reset();
    if (!area) {
        selection_.reset();
    } else {
        if (!outline) {
            outline.emplace();
            if (area->contains("poly") && (*area)["poly"].is_array() && !(*area)["poly"].empty()) {
                for (const Json& p : (*area)["poly"]) outline->emplace_back(p[0].get<double>(), p[1].get<double>());
            } else {
                const Json& box = (*area)["mask"]["box"];
                const double x = box[0].get<double>(), y = box[1].get<double>(), w = box[2].get<double>(), h = box[3].get<double>();
                *outline = {QPointF(x, y), QPointF(x + w, y), QPointF(x + w, y + h), QPointF(x, y + h)};
            }
        }
        selection_ = Selection{std::move(*area), std::move(*outline)};
    }
    update();
}

std::optional<PageCanvas::Selection> PageCanvas::previous_selection() const {
    if (!previous_ || page() == nullptr || previous_->first != page()->id) return std::nullopt;
    return previous_->second;
}

std::array<double, 4> PageCanvas::selection_box() const {
    if (!selection_ || selection_->outline.empty()) return {0, 0, 0, 0};
    std::array<double, 4> box{selection_->outline[0].x(), selection_->outline[0].y(), selection_->outline[0].x(), selection_->outline[0].y()};
    for (const QPointF& p : selection_->outline) {
        box[0] = std::min(box[0], p.x());
        box[1] = std::min(box[1], p.y());
        box[2] = std::max(box[2], p.x());
        box[3] = std::max(box[3], p.y());
    }
    return box;
}

QPointF PageCanvas::selection_pivot() const {
    if (sel_pivot_) return *sel_pivot_;
    const auto [x0, y0, x1, y1] = selection_box();
    return QPointF((x0 + x1) / 2, (y0 + y1) / 2);
}

bool PageCanvas::start_warp(const QString& kind, int columns, int rows) {
    if (!selection_) return false;
    const auto [x0, y0, x1, y1] = selection_box();
    columns = std::clamp(columns, 1, 8);
    rows = std::clamp(rows, 1, 8);
    Warp warp;
    warp.kind = kind;
    warp.box = {x0, y0, x1 - x0, y1 - y0};
    if (kind == QLatin1String("perspective")) {
        warp.points = {QPointF(x0, y0), QPointF(x1, y0), QPointF(x1, y1), QPointF(x0, y1)};
    } else {
        for (int j = 0; j <= rows; ++j)
            for (int i = 0; i <= columns; ++i) warp.points.emplace_back(x0 + (x1 - x0) * i / columns, y0 + (y1 - y0) * j / rows);
    }
    warp.columns = columns + 1;
    warp.rows = rows + 1;
    warp_ = std::move(warp);
    update();
    return true;
}

void PageCanvas::finish_warp() {
    if (!warp_) return;
    const Warp warp = *warp_;
    warp_.reset();
    sel_drag_.reset();  // (a point still held goes with it)
    Json points = Json::array();
    for (const QPointF& p : warp.points) points.push_back(Json::array({r3(p.x()), r3(p.y())}));
    Json out{{warp.kind.toStdString(), points}};
    if (warp.kind == QLatin1String("mesh") && (warp.columns != 3 || warp.rows != 3)) out["grid"] = Json::array({warp.columns, warp.rows});
    emit selectionWarped(out);
    update();
}

void PageCanvas::cancel_warp() {
    warp_.reset();
    sel_drag_.reset();
    update();
}

std::vector<PageCanvas::SelHandle> PageCanvas::sel_handles() const {
    std::vector<SelHandle> out;
    if (!selection_ || tool_ != QLatin1String("marquee")) return out;
    if (warp_) {
        for (std::size_t i = 0; i < warp_->points.size(); ++i) out.push_back({QStringLiteral("warp"), {}, static_cast<int>(i), warp_->points[i]});
        return out;
    }
    const auto [x0, y0, x1, y1] = selection_box();
    const std::pair<const char*, std::pair<double, double>> corners[] = {{"nw", {0, 0}}, {"n", {0.5, 0}}, {"ne", {1, 0}}, {"e", {1, 0.5}},
                                                                         {"se", {1, 1}}, {"s", {0.5, 1}}, {"sw", {0, 1}}, {"w", {0, 0.5}}};
    for (const auto& [key, at] : corners)
        out.push_back({QStringLiteral("scale"), QString::fromLatin1(key), 0, QPointF(x0 + (x1 - x0) * at.first, y0 + (y1 - y0) * at.second)});
    out.push_back({QStringLiteral("rotate"), QStringLiteral("r"), 0, QPointF((x0 + x1) / 2, y0 - 18 / view_.scale)});
    if (sel_pivot_) out.push_back({QStringLiteral("pivot"), QStringLiteral("p"), 0, *sel_pivot_});
    // 平行ゆがみ (skew): a diamond a quarter along each side slants the box along that side
    out.push_back({QStringLiteral("skew"), QStringLiteral("n"), 0, QPointF(x0 + (x1 - x0) * 0.25, y0)});
    out.push_back({QStringLiteral("skew"), QStringLiteral("s"), 0, QPointF(x0 + (x1 - x0) * 0.75, y1)});
    out.push_back({QStringLiteral("skew"), QStringLiteral("w"), 0, QPointF(x0, y0 + (y1 - y0) * 0.75)});
    out.push_back({QStringLiteral("skew"), QStringLiteral("e"), 0, QPointF(x1, y0 + (y1 - y0) * 0.25)});
    return out;
}

std::array<double, 6> PageCanvas::sel_matrix(const QPointF& mm) const {
    const SelDrag& drag = *sel_drag_;
    const auto [x0, y0, x1, y1] = drag.box;
    const double px = mm.x(), py = mm.y();
    const double sx0 = drag.start.x(), sy0 = drag.start.y();
    const auto nonzero = [](double v) { return v != 0 ? v : 1e-6; };
    if (drag.kind == QLatin1String("move")) return {1, 0, 0, 1, px - sx0, py - sy0};
    if (drag.kind == QLatin1String("rotate")) {
        const QPointF centre = sel_pivot_.value_or(QPointF((x0 + x1) / 2, (y0 + y1) / 2));
        const double cx = centre.x(), cy = centre.y();
        double angle = std::atan2(py - cy, px - cx) - std::atan2(sy0 - cy, sx0 - cx);
        if (modifiers_ & Qt::ShiftModifier) angle = std::nearbyint(angle / (kPi / 12)) * (kPi / 12);
        const double c = std::cos(angle), s = std::sin(angle);
        return {c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy};
    }
    const QString& key = drag.key;
    if (drag.kind == QLatin1String("skew")) {  // the side dragged slides along itself; the opposite side stays
        if (key == QLatin1String("n") || key == QLatin1String("s")) {
            const double ay = key == QLatin1String("n") ? y1 : y0;
            const double k = (px - sx0) / nonzero(sy0 - ay);
            return {1, 0, k, 1, -k * ay, 0};
        }
        const double ax = key == QLatin1String("w") ? x1 : x0;
        const double k = (py - sy0) / nonzero(sx0 - ax);
        return {1, k, 0, 1, 0, -k * ax};
    }
    const bool west = key.contains(QLatin1Char('w')), east = key.contains(QLatin1Char('e'));
    const bool north = key.startsWith(QLatin1Char('n')), south = key.startsWith(QLatin1Char('s'));
    const double ax = west ? x1 : east ? x0 : (x0 + x1) / 2;
    const double ay = north ? y1 : south ? y0 : (y0 + y1) / 2;
    double sx = west || east ? (px - ax) / nonzero(sx0 - ax) : 1.0;
    double sy = north || south ? (py - ay) / nonzero(sy0 - ay) : 1.0;
    if ((modifiers_ & Qt::ShiftModifier) && key.size() == 2) {
        sx = sy = (std::abs(sx) + std::abs(sy)) / 2 * (sx * sy > 0 ? 1 : -1);
    }
    return {sx, 0, 0, sy, ax - sx * ax, ay - sy * ay};
}

std::vector<QPointF> PageCanvas::marquee_points() const {
    // (a rectangle of a fixed size is there at a click)
    if (marquee == QLatin1String("rect") && (marquee_stroke_.size() >= 2 || (!marquee_stroke_.empty() && marquee_style == QLatin1String("fixed")))) {
        const QPointF a = marquee_stroke_.front(), b = styled_corner(a, marquee_stroke_.back());
        return {a, QPointF(b.x(), a.y()), b, QPointF(a.x(), b.y())};
    }
    return marquee_stroke_;
}

QPointF PageCanvas::styled_corner(const QPointF& a, const QPointF& b) const {
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    if (marquee_style == QLatin1String("fixed")) {
        return QPointF(a.x() + std::copysign(std::max(0.1, style_width), dx), a.y() + std::copysign(std::max(0.1, style_height), dy));
    }
    if (marquee_style == QLatin1String("ratio") && style_width > 0 && style_height > 0) {
        const double k = std::max(std::abs(dx) / style_width, std::abs(dy) / style_height);
        return QPointF(a.x() + std::copysign(k * style_width, dx), a.y() + std::copysign(k * style_height, dy));
    }
    return b;
}

Json PageCanvas::drawn_area(const Json& poly, bool smooth) const {
    Json area{{"poly", poly}};
    if (smooth && select_antialias) area["antialias"] = true;
    if (select_feather_mm > 0) area["feather_mm"] = r3(select_feather_mm);
    return area;
}

void PageCanvas::selection_done(const Json& area) {
    const QString how = sel_how_;
    if (how == QLatin1String("replace") || !selection_) set_selection(area);
    emit selectionDrawn(area, how);
}

bool PageCanvas::finish_points() {
    std::vector<QPointF> points;
    std::swap(points, poly_points_);
    update();
    if (points.size() < 3) return false;
    Json poly = Json::array();
    for (const QPointF& p : points) poly.push_back(Json::array({r3(p.x()), r3(p.y())}));
    selection_done(drawn_area(poly, true));
    return true;
}

bool PageCanvas::cancel_points() {
    if (poly_points_.empty() && !ellipse_drag_) return false;
    poly_points_.clear();
    ellipse_drag_.reset();
    update();
    return true;
}

// --- the mouse and the keys -------------------------------------------------------------------------------------------

bool PageCanvas::marquee_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    modifiers_ = modifiers;
    if (marquee == QLatin1String("quick") || marquee == QLatin1String("object")) return smart_press(mm, modifiers);  // (PC3 B3)
    sel_how_ = how_from(modifiers);
    if ((modifiers & (Qt::ShiftModifier | Qt::AltModifier)) == 0) sel_how_ = select_method;  // (選択方法: the tool's buttons)
    if ((sel_how_ == QLatin1String("replace") || transform_outline) && marquee != QLatin1String("polyline") && selection_) {
        // moving, scaling, turning the selection; a press elsewhere starts a new one
        if (pivot_mode) {  // (基準位置を動かす: this press puts it)
            pivot_mode = false;
            sel_pivot_ = QPointF(r3(mm.x()), r3(mm.y()));
            sel_drag_ = SelDrag{QStringLiteral("pivot"), QStringLiteral("p"), 0, mm, selection_box(), std::nullopt};
            update();
            return true;
        }
        for (const SelHandle& handle : sel_handles()) {
            const QPointF at = pt(handle.at.x(), handle.at.y());
            if (std::abs(at.x() - pos.x()) <= 7 && std::abs(at.y() - pos.y()) <= 7) {
                sel_drag_ = SelDrag{handle.kind, handle.key, handle.index, mm, selection_box(), std::nullopt};
                return true;
            }
        }
        if (warp_) return true;  // (only the points move while a free transform is being set up)
        Json outline = Json::array();
        for (const QPointF& p : selection_->outline) outline.push_back(Json::array({p.x(), p.y()}));
        const Json poly{{"poly", outline}};
        if (render::selection::AreaTest(poly).contains(mm.x(), mm.y())) {
            sel_drag_ = SelDrag{QStringLiteral("move"), {}, 0, mm, selection_box(), std::nullopt};
            return true;
        }
    }
    if (marquee == QLatin1String("wand")) {
        emit wandRequested(mm.x(), mm.y());
        return true;
    }
    if (marquee == QLatin1String("polyline")) {
        poly_points_.push_back(mm);
        update();
        return true;
    }
    if (marquee == QLatin1String("ellipse")) {
        ellipse_drag_ = std::make_pair(mm, mm);
        if (sel_how_ == QLatin1String("replace")) set_selection(std::nullopt);
        return true;
    }
    if (sel_how_ == QLatin1String("replace") && marquee != QLatin1String("pen") && marquee != QLatin1String("erase")) set_selection(std::nullopt);
    marquee_stroke_ = {mm};
    update();
    return true;
}

bool PageCanvas::marquee_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed) {
    if (!smart_drag_.empty()) return smart_move(mm, pressed);  // (PC3 B3)
    if (sel_drag_ && sel_drag_->kind == QLatin1String("warp")) {
        if (!warp_ || static_cast<std::size_t>(sel_drag_->index) >= warp_->points.size()) {
            sel_drag_.reset();
            return true;
        }
        warp_->points[static_cast<std::size_t>(sel_drag_->index)] = QPointF(r3(mm.x()), r3(mm.y()));
        update();
        return true;
    }
    if (sel_drag_ && sel_drag_->kind == QLatin1String("pivot")) {
        sel_pivot_ = QPointF(r3(mm.x()), r3(mm.y()));
        update();
        return true;
    }
    if (sel_drag_) {
        modifiers_ = modifiers;
        sel_drag_->matrix = sel_matrix(mm);
        update();
        return true;
    }
    if (ellipse_drag_) {  // (Shift: a circle)
        const QPointF a = ellipse_drag_->first;
        QPointF b = mm;
        if (modifiers & Qt::ShiftModifier) {
            const double dx = b.x() - a.x(), dy = b.y() - a.y();
            const double side = std::max(std::abs(dx), std::abs(dy));
            b = QPointF(a.x() + std::copysign(side, dx != 0 ? dx : 1), a.y() + std::copysign(side, dy != 0 ? dy : 1));
        }
        ellipse_drag_->second = styled_corner(a, b);
        update();
        return true;
    }
    if (pressed && !marquee_stroke_.empty()) {
        marquee_stroke_.push_back(mm);
        update();
        return true;
    }
    return false;
}

bool PageCanvas::marquee_release() {
    if (!smart_drag_.empty()) return smart_release();  // (PC3 B3)
    if (sel_drag_ && (sel_drag_->kind == QLatin1String("warp") || sel_drag_->kind == QLatin1String("pivot"))) {
        sel_drag_.reset();
        update();
        return true;
    }
    if (sel_drag_) {
        const SelDrag drag = *sel_drag_;
        sel_drag_.reset();
        if (drag.matrix) {
            const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
            bool moved = false;
            for (std::size_t i = 0; i < 6; ++i) moved = moved || std::abs((*drag.matrix)[i] - identity[i]) > 1e-4;
            if (moved) emit selectionTransformed(QVector<double>(drag.matrix->begin(), drag.matrix->end()));
        }
        update();
        return true;
    }
    if (ellipse_drag_) {
        const QPointF a = ellipse_drag_->first, b = styled_corner(a, ellipse_drag_->second);
        ellipse_drag_.reset();
        if (std::abs(b.x() - a.x()) > 0.5 && std::abs(b.y() - a.y()) > 0.5) {
            selection_done(drawn_area(render::selection::ellipse_poly(Json::array({std::min(a.x(), b.x()), std::min(a.y(), b.y()),
                                                                                   std::abs(b.x() - a.x()), std::abs(b.y() - a.y())})),
                                      true));
        }
        update();
        return true;
    }
    if (marquee_stroke_.empty()) return false;
    if (marquee == QLatin1String("pen") || marquee == QLatin1String("erase")) {
        QVector<QPointF> points;
        for (const QPointF& p : marquee_stroke_) points.append(QPointF(r3(p.x()), r3(p.y())));
        marquee_stroke_.clear();
        emit selectionPainted(points, marquee == QLatin1String("pen"));
        update();
        return true;
    }
    const std::vector<QPointF> points = marquee_points();
    marquee_stroke_.clear();
    double reach = 0;
    for (const QPointF& p : points) reach = std::max(reach, distance(points.front(), p));
    if (points.size() >= 3 && reach > 1.0) {
        Json poly = Json::array();
        for (const QPointF& p : points) poly.push_back(Json::array({p.x(), p.y()}));
        selection_done(drawn_area(poly, marquee != QLatin1String("rect")));
    }
    update();
    return true;
}

bool PageCanvas::marquee_key(QKeyEvent* event) {
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (!poly_points_.empty() && enter) {
        finish_points();
        return true;
    }
    if (event->key() == Qt::Key_Escape && cancel_points()) return true;
    if (warp_ && enter) {
        finish_warp();
        return true;
    }
    if (warp_ && event->key() == Qt::Key_Escape) {
        cancel_warp();
        return true;
    }
    if (transform_outline && (enter || event->key() == Qt::Key_Escape)) {  // (選択範囲を変形: done; the selection stays)
        transform_outline = false;
        update();
        return true;
    }
    return false;
}

// --- drawing ---------------------------------------------------------------------------------------------------------

void PageCanvas::draw_selection_mask(QPainter& painter) const {
    // a selection that is not a simple outline (auto-select, colour, pen, joined) shows as a tint of its own shape (the
    // Quick Mask is its channel, laid over by the page's view)
    const core::Page* p = page();
    if (!selection_ || p == nullptr) return;
    const Json& area = selection_->area;
    if (!area.contains("mask")) return;
    const std::string key = area.dump();
    if (!mask_picture_ || mask_picture_->first != key) {
        try {
            render::Image mask = render::selection::to_mask(area, *p, nullptr, 60);
            const std::string bytes = mask.tobytes();
            const QColor colour(28, 126, 214, 70);
            QImage picture(mask.width(), mask.height(), QImage::Format_RGBA8888);
            for (int y = 0; y < mask.height(); ++y) {
                auto* row = picture.scanLine(y);
                for (int x = 0; x < mask.width(); ++x) {
                    const int v = static_cast<unsigned char>(bytes[static_cast<std::size_t>(y) * mask.width() + x]);
                    row[x * 4] = static_cast<uchar>(colour.red());
                    row[x * 4 + 1] = static_cast<uchar>(colour.green());
                    row[x * 4 + 2] = static_cast<uchar>(colour.blue());
                    row[x * 4 + 3] = static_cast<uchar>(v * colour.alpha() / 255);
                }
            }
            mask_picture_ = std::make_pair(key, picture);
        } catch (const std::exception&) {
            return;
        }
    }
    painter.drawImage(QRectF(pt(0, 0), pt(p->spec.width_mm.value(), p->spec.height_mm.value())), mask_picture_->second);
}

void PageCanvas::draw_marquee(QPainter& painter) const {
    if (selection_) {
        if (show_selection_edges) draw_selection_mask(painter);
        std::vector<QPointF> outline = selection_->outline;
        if (sel_drag_ && sel_drag_->matrix) outline = through(*sel_drag_->matrix, outline);
        QPolygonF shown;
        for (const QPointF& q : outline) shown << pt(q.x(), q.y());
        painter.setBrush(Qt::NoBrush);
        if (warp_) {  // the box's grid, bent the way the area will be
            try {
                Json warp{{warp_->kind.toStdString(), Json::array()}};
                for (const QPointF& p : warp_->points) warp[warp_->kind.toStdString()].push_back(Json::array({p.x(), p.y()}));
                warp["grid"] = Json::array({warp_->columns, warp_->rows});
                const auto go = render::warp::mapping({warp_->box[0], warp_->box[1], warp_->box[2], warp_->box[3]}, warp);
                const auto [x0, y0, w, h] = warp_->box;
                painter.setPen(QPen(theme::accent(), 1.2));
                constexpr int kSteps = 16;
                for (int k = 0; k < 5; ++k) {
                    const double t = k / 4.0;
                    QPolygonF across, down;
                    for (int i = 0; i <= kSteps; ++i) {
                        const auto a = go(x0 + w * i / kSteps, y0 + h * t);
                        const auto b = go(x0 + w * t, y0 + h * i / kSteps);
                        across << pt(a.first, a.second);
                        down << pt(b.first, b.second);
                    }
                    painter.drawPolyline(across);
                    painter.drawPolyline(down);
                }
            } catch (const std::exception&) {
                // (points that fold the area: no grid until they are pulled apart)
            }
        }
        if (show_selection_edges) {  // (表示 → 選択範囲の境界線)
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawPolygon(shown);
            painter.setPen(QPen(transform_outline ? theme::accent() : kSelection, 1.5, Qt::DashLine));
            painter.drawPolygon(shown);
        }
        for (const SelHandle& handle : sel_handles()) {
            const QPointF h = pt(handle.at.x(), handle.at.y());
            painter.setPen(QPen(kSelection, 1.5));
            painter.setBrush(Qt::white);
            if (handle.kind == QLatin1String("rotate")) {
                painter.drawEllipse(h, 5, 5);
            } else if (handle.kind == QLatin1String("pivot")) {  // (a target: the point the selection turns about)
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(h, 6, 6);
                painter.drawLine(QPointF(h.x() - 9, h.y()), QPointF(h.x() + 9, h.y()));
                painter.drawLine(QPointF(h.x(), h.y() - 9), QPointF(h.x(), h.y() + 9));
            } else if (handle.kind == QLatin1String("skew")) {
                painter.drawPolygon(QPolygonF({QPointF(h.x(), h.y() - 5), QPointF(h.x() + 5, h.y()), QPointF(h.x(), h.y() + 5), QPointF(h.x() - 5, h.y())}));
            } else if (handle.kind == QLatin1String("warp")) {
                painter.setPen(QPen(theme::accent(), 1.5));
                painter.drawEllipse(h, 5, 5);
            } else {
                painter.drawRect(QRectF(h.x() - 4, h.y() - 4, 8, 8));
            }
        }
        painter.setBrush(Qt::NoBrush);
    }
    // what is being drawn
    if (!marquee_stroke_.empty() && (marquee == QLatin1String("pen") || marquee == QLatin1String("erase"))) {
        const QColor colour = marquee == QLatin1String("pen") ? QColor(28, 126, 214, 110) : QColor(220, 60, 60, 110);
        painter.setPen(QPen(colour, std::max(2.0, selection_pen_mm * view_.scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF line;
        for (const QPointF& p : marquee_stroke_) line << pt(p.x(), p.y());
        painter.drawPolyline(line);
    } else if (!marquee_stroke_.empty()) {
        painter.setPen(QPen(kSelection, 1.5, Qt::DashLine));
        painter.setBrush(QColor(28, 126, 214, 30));
        QPolygonF area;
        for (const QPointF& p : marquee_points()) area << pt(p.x(), p.y());
        painter.drawPolygon(area);
        painter.setBrush(Qt::NoBrush);
    }
    draw_smart_drag(painter);  // (PC3 B3)
    if (ellipse_drag_ || !poly_points_.empty()) {
        painter.setPen(QPen(kSelection, 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        if (ellipse_drag_) {
            const QPointF a = ellipse_drag_->first, b = ellipse_drag_->second;
            const Json poly = render::selection::ellipse_poly(Json::array({std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::abs(b.x() - a.x()),
                                                                          std::abs(b.y() - a.y())}));
            QPolygonF shown;
            for (const Json& p : poly) shown << pt(p[0].get<double>(), p[1].get<double>());
            painter.drawPolygon(shown);
        } else {
            QPolygonF line;
            for (const QPointF& p : poly_points_) line << pt(p.x(), p.y());
            if (hover_) line << pt(hover_->x(), hover_->y());
            painter.drawPolyline(line);
            for (const QPointF& p : poly_points_) painter.drawEllipse(pt(p.x(), p.y()), 3, 3);
        }
    }
}

// --- PC3 B3: クイック選択ツール and オブジェクト選択ツール ------------------------------------------------------------------

bool PageCanvas::smart_press(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    // クイック選択ツール adds to the selection there is (Photoshop's: its brush is 選択範囲に追加 after the first stroke), Alt takes
    // away; オブジェクト選択ツール joins as the other tools (Shift, Alt, both; else 選択方法)
    const bool alt = (modifiers & Qt::AltModifier) != 0;
    if (marquee == QLatin1String("quick")) {
        if (alt) smart_how_ = QStringLiteral("subtract");
        else if (select_method == QLatin1String("subtract") || select_method == QLatin1String("intersect")) smart_how_ = select_method;
        else if (selection_ || (modifiers & Qt::ShiftModifier) != 0) smart_how_ = QStringLiteral("add");
        else smart_how_ = QStringLiteral("replace");
    } else {
        smart_how_ = (modifiers & (Qt::ShiftModifier | Qt::AltModifier)) != 0 ? how_from(modifiers) : select_method;
    }
    smart_drag_ = {mm};
    update();
    return true;
}

bool PageCanvas::smart_move(const QPointF& mm, bool pressed) {
    if (!pressed) return true;
    if (marquee == QLatin1String("object")) {
        if (smart_drag_.size() < 2) smart_drag_.push_back(mm);
        else smart_drag_[1] = mm;
    } else if (distance(smart_drag_.back(), mm) >= std::max(0.05, quick_size_mm / 8)) {  // (the stroke's points a little apart)
        smart_drag_.push_back(mm);
    }
    update();
    return true;
}

bool PageCanvas::smart_release() {
    std::vector<QPointF> drag;
    std::swap(drag, smart_drag_);
    update();
    if (marquee == QLatin1String("object")) {
        if (drag.size() < 2) return true;
        const QRectF box = QRectF(drag.front(), drag.back()).normalized();
        if (box.width() < 0.5 || box.height() < 0.5) return true;
        emit objectSelected(QRectF(r3(box.x()), r3(box.y()), r3(box.width()), r3(box.height())), smart_how_);
        return true;
    }
    QVector<QPointF> points;
    for (const QPointF& p : drag) points.append(QPointF(r3(p.x()), r3(p.y())));
    emit quickSelected(points, smart_how_);
    return true;
}

void PageCanvas::draw_smart_drag(QPainter& painter) const {
    if (smart_drag_.empty()) return;
    if (marquee == QLatin1String("object")) {
        if (smart_drag_.size() < 2) return;
        painter.setPen(QPen(kSelection, 1.5, Qt::DashLine));
        painter.setBrush(QColor(28, 126, 214, 30));
        const QPointF a = smart_drag_.front(), b = smart_drag_.back();
        painter.drawRect(QRectF(pt(a.x(), a.y()), pt(b.x(), b.y())).normalized());
        painter.setBrush(Qt::NoBrush);
        return;
    }
    const QColor colour = smart_how_ == QLatin1String("subtract") ? QColor(220, 60, 60, 90) : QColor(28, 126, 214, 90);
    painter.setPen(QPen(colour, std::max(2.0, quick_size_mm * view_.scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF line;
    for (const QPointF& p : smart_drag_) line << pt(p.x(), p.y());
    if (line.size() == 1) line << line.front();
    painter.drawPolyline(line);
}

void PageCanvas::place_launcher() {
    if (launcher == nullptr) return;
    if (!selection_ || selection_->outline.empty() || sel_drag_ || warp_ || page() == nullptr) {
        launcher->hide();
        return;
    }
    const QTransform turned = view_transform();
    double centre = 0;
    double bottom = -1e300;
    for (const QPointF& p : selection_->outline) {
        const QPointF at = turned.map(pt(p.x(), p.y()));
        centre += at.x();
        bottom = std::max(bottom, at.y());
    }
    const QSize hint = launcher->sizeHint();
    double x = centre / static_cast<double>(selection_->outline.size()) - hint.width() / 2.0;
    double y = bottom + 10;
    x = std::max(4.0, std::min(width() - hint.width() - 4.0, x));
    y = std::max(4.0, std::min(height() - hint.height() - 4.0, y));
    launcher->adjustSize();
    launcher->move(static_cast<int>(x), static_cast<int>(y));
    launcher->show();
    launcher->raise();
}

}  // namespace genko::app
