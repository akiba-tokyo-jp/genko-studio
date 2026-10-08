// The canvas's lines (Python's PageCanvas, everything under the text tool and the balloon handles): テキスト — a click
// asks for a line typed there (the window opens the editor, InlineEditor in ime.hpp, where the click was), or with
// フキダシを手で描く a drag draws the balloon's outline —; and with 選択 a balloon is chosen and dragged to a new place,
// its box resized by the eight □, turned by the ○ above it, its tails' tips (●) and bends (◇) dragged; a double click
// types over it, a right click asks for its menu. The page's lines are the book's story for the page shown.

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/ime.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::app {

using core::Json;
using core::Num;

namespace {

constexpr int kHandle = PageCanvas::kHandlePx;

double r2(double v) { return core::py_round(v, 2); }

QPointF point_of(const Json& p) {
    const std::vector<Json> items = core::iterate(p);
    return items.size() >= 2 ? QPointF(core::to_float(items[0]), core::to_float(items[1])) : QPointF();
}

// Python's min(a, b) and max(a, b) of numbers
Num num_min(const Num& a, const Num& b) { return b < a ? b : a; }
Num num_max(const Num& a, const Num& b) { return b > a ? b : a; }

}  // namespace

// --- the page's lines ----------------------------------------------------------------------------------------------

std::vector<const core::StoryLine*> PageCanvas::lines() const {
    const core::Page* p = page();
    if (p == nullptr || !doc_) return {};
    return doc_->story_for_page(p->index);
}

const core::StoryLine* PageCanvas::selected_line() const {
    if (!selected_line_id) return nullptr;
    for (const core::StoryLine* line : lines()) {
        if (line->id == *selected_line_id) return line;
    }
    return nullptr;
}

const core::StoryLine* PageCanvas::hit_line(double x_mm, double y_mm) const {
    const auto all = lines();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const core::StoryLine& line = **it;
        if (line.x_mm <= Num(x_mm) && Num(x_mm) <= line.x_mm + line.w_mm && line.y_mm <= Num(y_mm) && Num(y_mm) <= line.y_mm + line.h_mm) return &line;
    }
    return nullptr;
}

Json PageCanvas::tails_of(const core::StoryLine& line) {
    Json tails = Json::array();
    for (const Json& t : line.tails) {
        if (t.is_object() && core::truthy_at(t, "to")) tails.push_back(t);
    }
    if (tails.empty() && line.tail) tails.push_back(Json{{"to", Json::array({line.tail->x.json(), line.tail->y.json()})}});
    return tails;
}

// --- the handles of the chosen balloon -----------------------------------------------------------------------------

std::vector<PageCanvas::LineHandle> PageCanvas::line_handles() const {
    // 8 resize handles, the turn above, and per tail its tip and its bend (or each corner of a bent one)
    const core::StoryLine* line = selected_line();
    if (line == nullptr || tool_ != QLatin1String("select")) return {};
    const bool resizing = handle_drag_ && handle_drag_->handle.kind == QLatin1String("resize");
    const std::array<Num, 4> box = resizing ? handle_drag_->cur : std::array<Num, 4>{line->x_mm, line->y_mm, line->w_mm, line->h_mm};
    const double x = box[0].value(), y = box[1].value(), w = box[2].value(), h = box[3].value();
    std::vector<LineHandle> out;
    for (const auto& [key, fx, fy] : {std::tuple{"nw", 0.0, 0.0}, {"n", 0.5, 0.0}, {"ne", 1.0, 0.0}, {"e", 1.0, 0.5}, {"se", 1.0, 1.0},
                                      {"s", 0.5, 1.0}, {"sw", 0.0, 1.0}, {"w", 0.0, 0.5}}) {
        LineHandle handle;
        handle.kind = QStringLiteral("resize");
        handle.key = QString::fromLatin1(key);
        handle.at = QPointF(x + w * fx, y + h * fy);
        out.push_back(handle);
    }
    const Json tails = handle_drag_ && handle_drag_->handle.kind == QLatin1String("tail") ? handle_drag_->tails : tails_of(*line);
    const double cx = x + w / 2;
    const double cy = y + h / 2;
    LineHandle turn;
    turn.kind = turn.key = QStringLiteral("turn");
    turn.at = QPointF(cx, y - 7);  // (drag around the middle to turn the balloon)
    out.push_back(turn);
    for (std::size_t i = 0; i < tails.size(); ++i) {
        const Json& tail = tails[i];
        const QPointF tip = point_of(tail["to"]);
        LineHandle to;
        to.kind = QStringLiteral("tail");
        to.tail = static_cast<int>(i);
        to.part = QStringLiteral("to");
        to.at = tip;
        out.push_back(to);
        if (core::truthy_at(tail, "vias")) {  // (a bent tail: a handle at each corner)
            const std::vector<Json> bends = core::iterate(tail["vias"]);
            for (std::size_t k = 0; k < bends.size(); ++k) {
                LineHandle bend = to;
                bend.part = QStringLiteral("vias");
                bend.via = static_cast<int>(k);
                bend.at = point_of(bends[k]);
                out.push_back(bend);
            }
            continue;
        }
        LineHandle via = to;
        via.part = QStringLiteral("via");
        via.at = core::truthy_at(tail, "via") ? point_of(tail["via"]) : QPointF((cx + tip.x()) / 2, (cy + tip.y()) / 2);
        out.push_back(via);
    }
    return out;
}

std::optional<PageCanvas::LineHandle> PageCanvas::hit_line_handle(const QPointF& pos) const {
    const auto handles = line_handles();
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const QPointF p = pt(it->at.x(), it->at.y());
        if (std::abs(p.x() - pos.x()) <= kHandle + 2 && std::abs(p.y() - pos.y()) <= kHandle + 2) return *it;
    }
    return std::nullopt;
}

// --- painting ------------------------------------------------------------------------------------------------------

void PageCanvas::draw_balloon_box(QPainter& painter, const core::StoryLine& line, std::optional<QPointF> at, bool strong, bool fill) const {
    const QPointF p = at ? pt(at->x(), at->y()) : pt(line.x_mm.value(), line.y_mm.value());
    const QRectF rect(p.x(), p.y(), std::max(10.0, line.w_mm.value() * view_.scale), std::max(10.0, line.h_mm.value() * view_.scale));
    painter.setPen(QPen(theme::accent(), strong ? 2 : 1.5, Qt::DashLine));
    painter.setBrush(strong && fill ? QBrush(QColor(255, 255, 255, 170)) : QBrush(Qt::NoBrush));
    painter.drawRect(rect);
    painter.setBrush(Qt::NoBrush);
}

void PageCanvas::draw_lines(QPainter& painter) const {
    // the balloon dragged (where it goes), the chosen one, and with the select tool the one under the pointer
    const core::StoryLine* hovered = tool_ == QLatin1String("select") && hover_ ? hit_line(hover_->x(), hover_->y()) : nullptr;
    for (const core::StoryLine* line : lines()) {
        if (line_drag_ && line_drag_->id == line->id) {
            draw_balloon_box(painter, *line, line_drag_->pos, true);
        } else if (selected_line_id && *selected_line_id == line->id) {
            draw_balloon_box(painter, *line, std::nullopt, true, false);
        } else if (hovered == line) {
            draw_balloon_box(painter, *line, std::nullopt);
        }
    }
    draw_line_handles(painter);
}

void PageCanvas::draw_line_handles(QPainter& painter) const {
    const core::StoryLine* line = selected_line();
    if (line == nullptr || tool_ != QLatin1String("select")) return;
    painter.save();
    if (handle_drag_ && handle_drag_->handle.kind == QLatin1String("resize")) {
        const auto& c = handle_drag_->cur;
        const QPointF p = pt(c[0].value(), c[1].value());
        painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(p.x(), p.y(), c[2].value() * view_.scale, c[3].value() * view_.scale));
    }
    const double cx = line->x_mm.value() + line->w_mm.value() / 2;
    const double cy = line->y_mm.value() + line->h_mm.value() / 2;
    if (handle_drag_ && handle_drag_->handle.kind == QLatin1String("turn") && handle_drag_->angle) {
        const double angle = handle_drag_->angle->value();
        const double a = angle * core::kPi / 180;
        const double r = std::max(line->w_mm.value(), line->h_mm.value()) / 2 + 7;
        painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
        painter.drawLine(pt(cx, cy), pt(cx + r * std::sin(a), cy - r * std::cos(a)));
        painter.drawText(pt(cx, cy) + QPointF(6, -6), QString::asprintf("%+.0f°", angle));
    }
    if (handle_drag_ && handle_drag_->handle.kind == QLatin1String("tail")) {
        painter.setPen(QPen(theme::accent(), 2, Qt::DashLine));
        for (const Json& tail : handle_drag_->tails) {
            QPainterPath path(pt(cx, cy));
            const QPointF to = point_of(tail["to"]);
            if (core::truthy_at(tail, "vias")) {
                for (const Json& bend : core::iterate(tail["vias"])) path.lineTo(pt(point_of(bend).x(), point_of(bend).y()));
                path.lineTo(pt(to.x(), to.y()));
            } else {
                const QPointF via = core::truthy_at(tail, "via") ? point_of(tail["via"]) : QPointF((cx + to.x()) / 2, (cy + to.y()) / 2);
                path.quadTo(pt(via.x(), via.y()), pt(to.x(), to.y()));
            }
            painter.drawPath(path);
        }
    }
    for (const LineHandle& handle : line_handles()) {
        const QPointF p = pt(handle.at.x(), handle.at.y());
        painter.setPen(QPen(theme::accent(), 1.5));
        painter.setBrush(Qt::white);
        if (handle.kind == QLatin1String("resize")) {
            painter.drawRect(QRectF(p.x() - kHandle / 2.0, p.y() - kHandle / 2.0, kHandle, kHandle));
        } else if (handle.kind == QLatin1String("turn")) {
            painter.drawLine(p + QPointF(0, kHandle / 2.0 + 1), pt(handle.at.x(), handle.at.y() + 7));
            painter.drawEllipse(p, kHandle / 2.0 + 2, kHandle / 2.0 + 2);
        } else if (handle.part == QLatin1String("to")) {
            painter.setBrush(theme::accent());
            painter.drawEllipse(p, kHandle / 2.0 + 1, kHandle / 2.0 + 1);
        } else {
            painter.drawPolygon(QPolygonF({p + QPointF(0, -5), p + QPointF(5, 0), p + QPointF(0, 5), p + QPointF(-5, 0)}));
        }
    }
    painter.restore();
}

void PageCanvas::draw_balloon_stroke(QPainter& painter) const {
    // the balloon pen's outline, drawn as a balloon while it is being drawn
    if (balloon_stroke_.empty() || tool_ != QLatin1String("text")) return;
    painter.save();
    painter.setPen(QPen(QColor(20, 20, 20), std::max(1.0, 0.35 * view_.scale)));
    painter.setBrush(QColor(255, 255, 255, 220));
    QPolygonF polygon;
    for (const QPointF& p : balloon_stroke_) polygon << pt(p.x(), p.y());
    painter.drawPolygon(polygon);
    painter.restore();
}

// --- the select tool: choosing, moving, resizing, turning, the tails ---------------------------------------------------

bool PageCanvas::line_press(const QPointF& pos, const QPointF& mm) {
    if (const auto handle = hit_line_handle(pos)) {
        const core::StoryLine* line = selected_line();
        const std::array<Num, 4> box{line->x_mm, line->y_mm, line->w_mm, line->h_mm};
        handle_drag_ = HandleDrag{*handle, line->id, box, box, tails_of(*line), std::nullopt};
        return true;
    }
    if (const core::StoryLine* hit = hit_line(mm.x(), mm.y())) {
        const QPointF corner(hit->x_mm.value(), hit->y_mm.value());
        line_drag_ = LineDrag{hit->id, mm - corner, corner, corner};
        return true;
    }
    return false;
}

bool PageCanvas::line_move(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (handle_drag_) {
        modifiers_ = modifiers;
        HandleDrag& drag = *handle_drag_;
        const double x_mm = mm.x();
        const double y_mm = mm.y();
        if (drag.handle.kind == QLatin1String("resize")) {
            const auto& [x, y, w, h] = drag.orig;
            const QString key = drag.handle.key;
            Num left = x, top = y, right = x + w, bottom = y + h;
            if (key.contains(QLatin1Char('w'))) left = num_min(Num(x_mm), right - Num(4));
            if (key.contains(QLatin1Char('e'))) right = num_max(Num(x_mm), left + Num(4));
            if (key.startsWith(QLatin1Char('n'))) top = num_min(Num(y_mm), bottom - Num(4));
            if (key.startsWith(QLatin1Char('s'))) bottom = num_max(Num(y_mm), top + Num(4));
            drag.cur = {core::py_round(left, 2), core::py_round(top, 2), core::py_round(right - left, 2), core::py_round(bottom - top, 2)};
        } else if (drag.handle.kind == QLatin1String("turn")) {
            const auto& [x, y, w, h] = drag.orig;
            // (0 straight up, clockwise)
            const double angle = core::py_atan2(x_mm - (x + w / Num(2)).value(), (y + h / Num(2)).value() - y_mm) * (180.0 / core::kPi);
            if (modifiers & Qt::ShiftModifier) {
                drag.angle = core::py_round(Num(core::py_round_int(angle / 15) * 15), 1);
            } else {
                drag.angle = Num(core::py_round(angle, 1));
            }
        } else {
            const Json at = Json::array({r2(x_mm), r2(y_mm)});
            Json& tail = drag.tails[static_cast<std::size_t>(drag.handle.tail)];
            if (drag.handle.part == QLatin1String("vias")) {
                tail["vias"][static_cast<std::size_t>(drag.handle.via)] = at;
            } else {
                tail[drag.handle.part.toStdString()] = at;
            }
        }
        update();
        return true;
    }
    if (line_drag_) {
        line_drag_->pos = mm - line_drag_->grab;
        update();
        return true;
    }
    return false;
}

bool PageCanvas::line_release() {
    if (handle_drag_) {
        const HandleDrag drag = std::move(*handle_drag_);
        handle_drag_.reset();
        press_pos_.reset();
        const QString id = QString::fromStdString(drag.line);
        if (drag.handle.kind == QLatin1String("resize") &&
            !(drag.cur[0] == drag.orig[0] && drag.cur[1] == drag.orig[1] && drag.cur[2] == drag.orig[2] && drag.cur[3] == drag.orig[3])) {
            emit lineGeometry(id, Json{{"x_mm", drag.cur[0].json()}, {"y_mm", drag.cur[1].json()}, {"w_mm", drag.cur[2].json()}, {"h_mm", drag.cur[3].json()}});
        } else if (drag.handle.kind == QLatin1String("tail")) {
            emit lineGeometry(id, Json{{"tails", drag.tails}});
        } else if (drag.handle.kind == QLatin1String("turn") && drag.angle) {
            emit lineGeometry(id, Json{{"style", Json{{"rotate_deg", drag.angle->truthy() ? drag.angle->json() : Json()}}}});
        }
        update();
        return true;
    }
    if (line_drag_) {
        const LineDrag drag = *line_drag_;
        line_drag_.reset();
        press_pos_.reset();
        selected_line_id = drag.id;
        const QString id = QString::fromStdString(drag.id);
        if (std::abs(drag.pos.x() - drag.orig.x()) > 0.05 || std::abs(drag.pos.y() - drag.orig.y()) > 0.05) {
            emit textMoved(id, r2(drag.pos.x()), r2(drag.pos.y()));  // (becomes a move_line op)
        }
        emit lineSelected(id, false);
        update();
        return true;
    }
    return false;
}

bool PageCanvas::line_double_click(const QPointF& mm) {
    const core::StoryLine* hit = hit_line(mm.x(), mm.y());
    if (hit == nullptr) return false;
    const QString id = QString::fromStdString(hit->id);
    selected_line_id = hit->id;
    emit lineSelected(id, false);
    emit lineEditRequested(id);
    return true;
}

bool PageCanvas::line_context_menu(const QPointF& mm, const QPoint& global) {
    const core::StoryLine* hit = hit_line(mm.x(), mm.y());
    if (hit == nullptr) return false;
    selected_line_id = hit->id;
    update();
    emit lineContextMenu(QString::fromStdString(hit->id), global);
    return true;
}

// --- the text tool -------------------------------------------------------------------------------------------------

bool PageCanvas::text_press(const QPointF& mm) {
    if (balloon_pen) {
        balloon_stroke_ = {mm};  // the outline of a balloon, drawn by hand
        update();
        return true;
    }
    emit textRequested(mm.x(), mm.y());
    return true;
}

bool PageCanvas::text_move(const QPointF& mm, bool pressed) {
    if (balloon_stroke_.empty() || !pressed) return false;
    balloon_stroke_.push_back(mm);
    update();
    return true;
}

bool PageCanvas::text_release() {
    if (balloon_stroke_.empty() || tool_ != QLatin1String("text")) return false;
    std::vector<QPointF> stroke;
    std::swap(stroke, balloon_stroke_);
    const std::size_t step = std::max<std::size_t>(1, stroke.size() / 80);
    std::vector<QPointF> pts;
    for (std::size_t i = 0; i < stroke.size(); i += step) pts.emplace_back(r2(stroke[i].x()), r2(stroke[i].y()));
    double lo_x = pts[0].x(), hi_x = lo_x, lo_y = pts[0].y(), hi_y = lo_y, far = 0;
    for (const QPointF& p : pts) {
        lo_x = core::py_min(lo_x, p.x());
        hi_x = core::py_max(hi_x, p.x());
        lo_y = core::py_min(lo_y, p.y());
        hi_y = core::py_max(hi_y, p.y());
        far = core::py_max(far, core::py_dist(pts[0].x(), pts[0].y(), p.x(), p.y()));
    }
    if (pts.size() >= 3 && hi_x - lo_x > 4 && hi_y - lo_y > 4 && far > 4) {
        Json outline = Json::array();
        for (const QPointF& p : pts) outline.push_back(Json::array({p.x(), p.y()}));
        emit balloonDrawn(outline);
    } else if (far <= 2) {  // (a click, not a drag: a line is placed as without the pen)
        emit textRequested(pts[0].x(), pts[0].y());
    }
    update();
    return true;
}

// --- the editor ------------------------------------------------------------------------------------------------------

InlineEditor* PageCanvas::open_editor(double x_mm, double y_mm, const QString& text, std::function<void(std::optional<QString>)> on_done) {
    if (!editor_.isNull() && !editor_->finished()) editor_->finish(true);
    const QPointF p = to_widget(QPointF(x_mm, y_mm));
    auto* editor = new InlineEditor(this, text, std::move(on_done));
    editor_ = editor;
    editor->place(std::max(0.0, std::min(width() - 260.0, p.x())), std::max(20.0, std::min(height() - 110.0, p.y())));
    return editor;
}

InlineEditor* PageCanvas::editor() const { return editor_.isNull() || editor_->finished() ? nullptr : editor_.data(); }

}  // namespace genko::app
