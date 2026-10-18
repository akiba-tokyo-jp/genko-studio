// The canvas's path tools (PC3 C-a, SPEC.md COLOR-07「パス」; Photoshop's names, the user's decision 5): ペンツール —
// a click puts a corner, a drag pulls a smooth point's handles (Alt while dragging breaks them: only the one after
// moves), a click on the first anchor closes the subpath, Enter, Esc or a double click ends it open, Backspace takes
// the last anchor back —; 曲線ペンツール — each click an anchor the curve passes through smoothly (Alt+click: a corner),
// the same ends —; パス選択ツール — a click on a subpath of the target path chooses it, a drag moves it (Alt: a copy),
// an anchor or handle of the chosen subpath dragged moves alone (as the direct selection tool), Delete takes the chosen
// subpath out. The target path (the Paths panel's choice) is drawn over the page with its anchors while a path tool is
// in hand or 表示 → ターゲットパス is on. What a tool makes goes to the window as a subpath drawn (pathDrawn: a set_path)
// or the target path changed (pathEdited); the points being drawn are the screen's only, until the subpath ends.

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"
#include "render/path_raster.hpp"

namespace genko::app {

namespace vec = core::vector;

namespace {

constexpr double kPickPx = 7.0;

double r4(double v) { return core::py_round(v, 4); }

vec::XY xy(const QPointF& p) { return vec::XY{r4(p.x()), r4(p.y())}; }
QPointF qp(const vec::XY& p) { return QPointF(p.x, p.y); }

bool is_path_tool(const QString& tool) {
    return tool == QLatin1String("pathpen") || tool == QLatin1String("curvepen") || tool == QLatin1String("pathselect");
}

// The curvature pen's knots: each anchor's handles along the line through its neighbours (a sixth of the way between
// them: Catmull-Rom as Bézier), a corner's at its anchor; an open subpath's ends take their one neighbour.
std::vector<vec::Knot> curved(const std::vector<vec::Knot>& anchors, bool closed) {
    std::vector<vec::Knot> out = anchors;
    const std::size_t n = anchors.size();
    if (n < 2) return out;
    for (std::size_t i = 0; i < n; ++i) {
        if (!anchors[i].smooth) {
            out[i].in = out[i].out = anchors[i].anchor;
            continue;
        }
        const vec::XY p = anchors[i].anchor;
        const vec::XY prev = i > 0 ? anchors[i - 1].anchor : closed ? anchors[n - 1].anchor : p;
        const vec::XY next = i + 1 < n ? anchors[i + 1].anchor : closed ? anchors[0].anchor : p;
        const double tx = (next.x - prev.x) / 6.0, ty = (next.y - prev.y) / 6.0;
        out[i].in = vec::XY{r4(p.x - tx), r4(p.y - ty)};
        out[i].out = vec::XY{r4(p.x + tx), r4(p.y + ty)};
    }
    return out;
}

double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

// The subpath's outline as points (mm), flattened finely enough for the screen.
std::vector<QPointF> outline(const vec::Subpath& s) {
    std::vector<QPointF> out;
    const render::path_raster::Polyline line = render::path_raster::flatten_subpath(s, 0.02);
    for (const auto& [x, y] : line.pts) out.emplace_back(x, y);
    if (s.closed && !out.empty()) out.push_back(out.front());
    return out;
}

double distance_to(const std::vector<QPointF>& line, const QPointF& p) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const QPointF a = line[i], b = line[i + 1];
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        const double seg = dx * dx + dy * dy;
        const double t = seg == 0 ? 0.0 : std::clamp(((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / seg, 0.0, 1.0);
        best = std::min(best, std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy)));
    }
    return best;
}

// Is the point inside the closed subpath (even-odd over its outline)?
bool inside(const std::vector<QPointF>& line, const QPointF& p) {
    bool in = false;
    for (std::size_t i = 0, j = line.size() ? line.size() - 1 : 0; i < line.size(); j = i++) {
        const QPointF a = line[i], b = line[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()) in = !in;
    }
    return in;
}

vec::Subpath moved(vec::Subpath s, double dx, double dy) {
    for (vec::Knot& k : s.knots) {
        for (vec::XY* p : {&k.anchor, &k.in, &k.out}) *p = vec::XY{r4(p->x + dx), r4(p->y + dy)};
    }
    return s;
}

}  // namespace

std::optional<vec::Path> PageCanvas::target_path_now() const {
    const core::Page* p = page();
    if (p == nullptr) return std::nullopt;
    if (target_path == QLatin1String("work")) return p->work_path;
    if (target_path.startsWith(QLatin1String("saved:"))) {
        const std::string name = target_path.mid(6).toStdString();
        for (const vec::NamedPath& named : p->paths)
            if (named.name == name) return named.path;
        return std::nullopt;
    }
    if (target_path.startsWith(QLatin1String("mask:"))) {
        const std::string id = target_path.mid(5).toStdString();
        for (const core::Layer& layer : p->layers)
            if (layer.id == id && layer.vector_mask) return layer.vector_mask->path;
    }
    return std::nullopt;
}

bool PageCanvas::finish_pen(bool closed) {
    if (pen_knots_.empty()) return false;
    std::vector<vec::Knot> knots;
    std::swap(knots, pen_knots_);
    const bool curve = pen_curve_;
    pen_handle_.reset();
    update();
    if (knots.size() < 2) return true;  // (one anchor: nothing to keep)
    if (curve) knots = curved(knots, closed);
    vec::Path one;
    one.subpaths.push_back(vec::Subpath{closed, vec::PathOp::Combine, std::move(knots)});
    emit pathDrawn(vec::to_json(one)["subpaths"][0]);
    return true;
}

bool PageCanvas::pen_back() {
    if (pen_knots_.empty()) return false;
    pen_knots_.pop_back();
    pen_handle_.reset();
    update();
    return true;
}

bool PageCanvas::path_delete() {
    if (tool_ != QLatin1String("pathselect") || !path_subpath) return false;
    std::optional<vec::Path> path = target_path_now();
    if (!path || *path_subpath >= path->subpaths.size()) return false;
    path->subpaths.erase(path->subpaths.begin() + static_cast<std::ptrdiff_t>(*path_subpath));
    if (!path->subpaths.empty()) path->subpaths.front().op = vec::PathOp::Combine;
    path_subpath.reset();
    emit pathEdited(vec::to_json(*path));
    update();
    return true;
}

bool PageCanvas::path_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!is_path_tool(tool_)) return false;
    const double reach = kPickPx / std::max(0.01, view_.scale);
    if (tool_ == QLatin1String("pathpen") || tool_ == QLatin1String("curvepen")) {
        const bool curve = tool_ == QLatin1String("curvepen");
        if (!pen_knots_.empty() && pen_curve_ != curve) finish_pen(false);  // (the other pen's subpath ended first)
        pen_curve_ = curve;
        if (pen_knots_.size() >= 2 && dist(qp(pen_knots_.front().anchor), mm) <= reach) {
            finish_pen(true);  // (on the first anchor: closed)
            return true;
        }
        vec::Knot knot = vec::Knot::corner(r4(mm.x()), r4(mm.y()));
        knot.smooth = curve && !(modifiers & Qt::AltModifier);
        pen_knots_.push_back(knot);
        if (!curve) pen_handle_ = pen_knots_.size() - 1;
        modifiers_ = modifiers;
        update();
        return true;
    }
    // パス選択ツール: the chosen subpath's anchors and handles first, then any subpath
    (void)pos;
    const std::optional<vec::Path> path = target_path_now();
    if (!path) {
        path_subpath.reset();
        update();
        return true;
    }
    if (path_subpath && *path_subpath < path->subpaths.size()) {
        const vec::Subpath& s = path->subpaths[*path_subpath];
        for (std::size_t k = 0; k < s.knots.size(); ++k) {
            for (const int part : {1, 2, 0}) {  // (the handles before the anchor they sit on)
                const vec::XY& p = part == 0 ? s.knots[k].anchor : part == 1 ? s.knots[k].in : s.knots[k].out;
                if (part != 0 && p == s.knots[k].anchor) continue;
                if (dist(qp(p), mm) <= reach) {
                    path_drag_ = PathDrag{*path, *path_subpath, static_cast<int>(k), part, mm, mm, false};
                    return true;
                }
            }
        }
    }
    std::optional<std::size_t> hit;
    for (std::size_t i = path->subpaths.size(); i-- > 0;) {
        const std::vector<QPointF> line = outline(path->subpaths[i]);
        if (distance_to(line, mm) <= reach || (path->subpaths[i].closed && inside(line, mm))) {
            hit = i;
            break;
        }
    }
    path_subpath = hit;
    if (hit) path_drag_ = PathDrag{*path, *hit, -1, 0, mm, mm, (modifiers & Qt::AltModifier) != 0};
    update();
    return true;
}

bool PageCanvas::path_move(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (tool_ == QLatin1String("pathpen") && pen_handle_ && *pen_handle_ < pen_knots_.size()) {
        vec::Knot& knot = pen_knots_[*pen_handle_];
        if (dist(qp(knot.anchor), mm) * view_.scale < 2.0) return true;  // (a click, not a drag yet)
        knot.out = xy(mm);
        if (modifiers & Qt::AltModifier) {
            knot.smooth = false;  // (broken: only the handle after follows)
        } else {
            knot.in = vec::XY{r4(2 * knot.anchor.x - mm.x()), r4(2 * knot.anchor.y - mm.y())};
            knot.smooth = true;
        }
        update();
        return true;
    }
    if (path_drag_) {
        path_drag_->now = mm;
        update();
        return true;
    }
    return false;
}

std::optional<vec::Path> PageCanvas::path_dragged() const {
    if (!path_drag_ || path_drag_->subpath >= path_drag_->path.subpaths.size()) return std::nullopt;
    vec::Path path = path_drag_->path;
    const double dx = path_drag_->now.x() - path_drag_->start.x(), dy = path_drag_->now.y() - path_drag_->start.y();
    vec::Subpath& s = path.subpaths[path_drag_->subpath];
    if (path_drag_->knot < 0) {
        if (path_drag_->copy) {
            vec::Subpath copy = moved(s, dx, dy);
            copy.op = vec::PathOp::Combine;
            path.subpaths.push_back(std::move(copy));
        } else {
            s = moved(s, dx, dy);
        }
        return path;
    }
    vec::Knot& k = s.knots[static_cast<std::size_t>(path_drag_->knot)];
    const auto shift = [&](vec::XY& p) { p = vec::XY{r4(p.x + dx), r4(p.y + dy)}; };
    if (path_drag_->part == 0) {  // (the anchor with its handles)
        shift(k.anchor);
        shift(k.in);
        shift(k.out);
    } else {
        vec::XY& moved_handle = path_drag_->part == 1 ? k.in : k.out;
        shift(moved_handle);
        if (k.smooth) {  // (the other handle stays opposite, its length kept)
            vec::XY& other = path_drag_->part == 1 ? k.out : k.in;
            const double len = std::hypot(other.x - k.anchor.x, other.y - k.anchor.y);
            const double hx = moved_handle.x - k.anchor.x, hy = moved_handle.y - k.anchor.y;
            const double hl = std::hypot(hx, hy);
            if (hl > 1e-9) other = vec::XY{r4(k.anchor.x - hx / hl * len), r4(k.anchor.y - hy / hl * len)};
        }
    }
    return path;
}

bool PageCanvas::path_release() {
    if (tool_ == QLatin1String("pathpen") && pen_handle_) {
        pen_handle_.reset();
        update();
        return true;
    }
    if (!path_drag_) return is_path_tool(tool_);
    const std::optional<vec::Path> path = path_dragged();
    const bool copy = path_drag_->copy;
    const double moved_mm = dist(path_drag_->start, path_drag_->now);
    path_drag_.reset();
    if (path && moved_mm * view_.scale >= 1.0) {
        if (copy) path_subpath = path->subpaths.size() - 1;
        emit pathEdited(vec::to_json(*path));
    }
    update();
    return true;
}

void PageCanvas::draw_paths(QPainter& painter) const {
    const bool tool = is_path_tool(tool_);
    if (!tool && !show_target_path && pen_knots_.empty()) return;
    const auto draw_subpath = [&](const vec::Subpath& s, const QColor& colour, double width) {
        QPolygonF line;
        for (const QPointF& p : outline(s)) line << pt(p.x(), p.y());
        painter.setPen(QPen(colour, width));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(line);
    };
    const auto draw_anchors = [&](const vec::Subpath& s, bool handles) {
        for (const vec::Knot& k : s.knots) {
            const QPointF a = pt(k.anchor.x, k.anchor.y);
            if (handles) {
                painter.setPen(QPen(theme::accent(), 1));
                for (const vec::XY& h : {k.in, k.out}) {
                    if (h == k.anchor) continue;
                    const QPointF hp = pt(h.x, h.y);
                    painter.drawLine(a, hp);
                    painter.setBrush(QColor(Qt::white));
                    painter.drawEllipse(hp, 3, 3);
                }
            }
            painter.setPen(QPen(theme::accent(), 1));
            painter.setBrush(handles ? QBrush(theme::accent()) : QBrush(Qt::white));
            painter.drawRect(QRectF(a.x() - 3, a.y() - 3, 6, 6));
        }
        painter.setBrush(Qt::NoBrush);
    };
    if (tool || show_target_path) {
        std::optional<vec::Path> path = path_drag_ ? path_dragged() : target_path_now();
        if (path) {
            for (std::size_t i = 0; i < path->subpaths.size(); ++i) {
                const bool chosen = tool_ == QLatin1String("pathselect") && path_subpath == i;
                draw_subpath(path->subpaths[i], chosen ? theme::accent() : QColor(40, 90, 200), chosen ? 1.6 : 1.0);
                if (tool) draw_anchors(path->subpaths[i], chosen);
            }
        }
    }
    if (!pen_knots_.empty()) {
        const vec::Subpath drawing{false, vec::PathOp::Combine, pen_curve_ ? curved(pen_knots_, false) : pen_knots_};
        draw_subpath(drawing, theme::accent(), 1.5);
        draw_anchors(drawing, true);
    }
}

}  // namespace genko::app
