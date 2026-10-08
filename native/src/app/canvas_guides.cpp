// The canvas's guides (M3④-4, Python's canvas_guides.GuideMixin): rulers — placed by dragging or click by click, their
// points dragged, drawn as they guide (lines, curves, circles, rays, perspective with its eye level and ground grid,
// shapes, symmetry axes) — the grid, and the 3D figures and boxes: their handles (joints, the pelvis, a box's centre,
// the ○ that turns it) dragged, the part being dragged drawn live. Everything changes the book only through the
// canvas's signals (the window turns them into ops).

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/mannequin.hpp"
#include "core/mesh3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pynum.hpp"
#include "core/rulers.hpp"

namespace genko::app {

namespace {

constexpr double kPi = 3.14159265358979323846;
const QColor kActive(QStringLiteral("#2b8a3e"));
const QColor kIdle(130, 130, 130, 170);
const QColor kPrim(70, 90, 170);

double r2(double v) { return core::py_round(v, 2); }

std::string id_of(const core::Json& j) {
    const auto found = j.is_object() ? j.find("id") : j.end();
    return j.is_object() && found != j.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string kind_of(const core::Json& j) {
    const auto found = j.is_object() ? j.find("kind") : j.end();
    return j.is_object() && found != j.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool truthy(const core::Json& j, const char* key, bool fallback) {
    if (!j.is_object()) return fallback;
    const auto found = j.find(key);
    return found == j.end() ? fallback : core::py_truthy(*found);
}

double number(const core::Json& j, const char* key, double fallback) {
    if (!j.is_object()) return fallback;
    const auto found = j.find(key);
    if (found == j.end() || !core::py_truthy(*found)) return fallback;
    try {
        return core::to_float(*found);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::vector<QPointF> points_of(const core::Json& ruler) {
    std::vector<QPointF> out;
    const auto found = ruler.is_object() ? ruler.find("points") : ruler.end();
    if (!ruler.is_object() || found == ruler.end() || !found->is_array()) return out;
    for (const core::Json& p : *found) {
        try {
            out.emplace_back(core::to_float(p.at(0)), core::to_float(p.at(1)));
        } catch (const std::exception&) {
        }
    }
    return out;
}

core::Json json_point(double x, double y) { return core::Json::array({x, y}); }

const core::Json* page_camera(const core::Page* page) {
    if (page == nullptr || !page->extra.is_object()) return nullptr;
    const auto found = page->extra.find("camera");
    return found != page->extra.end() && found->is_object() && !found->empty() ? &*found : nullptr;
}

bool dragged_kind(const QString& kind) {
    return kind == QLatin1String("line") || kind == QLatin1String("parallel") || kind == QLatin1String("concentric") ||
           kind == QLatin1String("symmetry") || kind == QLatin1String("rect") || kind == QLatin1String("ellipse");
}

bool clicked_kind(const QString& kind) {
    return kind == QLatin1String("curve") || kind == QLatin1String("perspective") || kind == QLatin1String("parallel_curve") ||
           kind == QLatin1String("radial_curve") || kind == QLatin1String("multi_curve") || kind == QLatin1String("polygon");
}

}  // namespace

// --- helpers --------------------------------------------------------------------------------------------------------

core::Json PageCanvas::page_rulers() const {
    core::Json out = core::Json::array();
    const core::Page* p = page();
    if (p == nullptr || !p->rulers.is_array()) return out;
    const core::Layer* layer = drawing_layer ? drawing_layer() : nullptr;
    for (const core::Json& r : p->rulers) {
        // (a ruler kept with a layer shows and snaps only for it)
        const auto kept = r.is_object() ? r.find("layer_id") : r.end();
        if (r.is_object() && kept != r.end() && core::py_truthy(*kept) && layer != nullptr && !(kept->is_string() && kept->get<std::string>() == layer->id))
            continue;
        out.push_back(r);
    }
    return out;
}

QPointF PageCanvas::grid_point(double x, double y) const {
    if (grid_snap && grid_mm > 0) {
        const auto snapped = core::rulers::snap_to_grid({x, y}, grid_mm);
        return QPointF(snapped.x, snapped.y);
    }
    return QPointF(x, y);
}

bool PageCanvas::near_point(const QPointF& pos, const QPointF& mm, double px) const {
    const QPointF q = pt(mm.x(), mm.y());
    return std::abs(q.x() - pos.x()) <= px && std::abs(q.y() - pos.y()) <= px;
}

// --- painting -------------------------------------------------------------------------------------------------------

void PageCanvas::draw_grid(QPainter& painter) const {
    const core::Page* p = page();
    if (!grid_visible || p == nullptr || grid_mm <= 0) return;
    const double width = p->spec.width_mm.value(), height = p->spec.height_mm.value();
    double step = grid_mm;
    if (step * view_.scale < 4) step *= 5;  // (too dense to see: every fifth line only)
    const auto major = [this](double v) {  // (Python's float %: the sign of the divisor)
        double m = std::fmod(v / grid_mm, 5.0);
        if (m != 0 && m < 0) m += 5.0;
        return std::abs(m) < 1e-6;
    };
    for (int k = 0; k <= static_cast<int>(width / step); ++k) {
        const double x = k * step;
        painter.setPen(QPen(QColor(80, 140, 220, major(x) ? 90 : 45), 1));
        painter.drawLine(pt(x, 0), pt(x, height));
    }
    for (int k = 0; k <= static_cast<int>(height / step); ++k) {
        const double y = k * step;
        painter.setPen(QPen(QColor(80, 140, 220, major(y) ? 90 : 45), 1));
        painter.drawLine(pt(0, y), pt(width, y));
    }
}

void PageCanvas::line_across(QPainter& painter, const QPointF& a, const QPointF& d, double length) const {
    painter.drawLine(pt(a.x() - d.x() * length, a.y() - d.y() * length), pt(a.x() + d.x() * length, a.y() + d.y() * length));
}

void PageCanvas::draw_one_ruler(QPainter& painter, const core::Json& ruler, bool selected) const {
    const bool active = truthy(ruler, "active", true);
    const QColor colour = active ? kActive : kIdle;
    painter.setPen(QPen(colour, selected ? 2.2 : 1.3, active ? Qt::SolidLine : Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    const std::vector<QPointF> pts = points_of(ruler);
    const std::string kind = kind_of(ruler);
    const core::Page* p = page();
    const core::rulers::PageSize page_size{p->spec.width_mm, p->spec.height_mm};
    const auto draw_poly = [&](const auto& line, auto x_of, auto y_of) {
        if (line.empty()) return;
        QPainterPath path(pt(x_of(line.front()), y_of(line.front())));
        for (std::size_t i = 1; i < line.size(); ++i) path.lineTo(pt(x_of(line[i]), y_of(line[i])));
        painter.drawPath(path);
    };
    const auto xy_x = [](const core::rulers::XY& q) { return q.x; };
    const auto xy_y = [](const core::rulers::XY& q) { return q.y; };
    const auto out_x = [](const core::rulers::OutlinePoint& q) { return q.x.value(); };
    const auto out_y = [](const core::rulers::OutlinePoint& q) { return q.y.value(); };
    try {
        if (kind == "line" && pts.size() >= 2) {
            const double dx = pts[1].x() - pts[0].x(), dy = pts[1].y() - pts[0].y();
            const double n = std::hypot(dx, dy);
            if (n > 1e-9) line_across(painter, pts[0], QPointF(dx / n, dy / n));
        } else if (kind == "curve" && pts.size() >= 2) {
            draw_poly(core::rulers::smooth_curve(ruler["points"]), xy_x, xy_y);
        } else if (kind == "parallel") {
            const double a = number(ruler, "angle", 0) * kPi / 180.0;
            const QPointF d(std::cos(a), std::sin(a));
            std::vector<QPointF> anchors(pts.begin(), pts.begin() + std::min<std::size_t>(1, pts.size()));
            if (hover_) anchors.push_back(*hover_);
            if (anchors.empty()) anchors.emplace_back(p->spec.width_mm.value() / 2, p->spec.height_mm.value() / 2);
            for (const QPointF& at : anchors) line_across(painter, at, d);
        } else if (kind == "concentric" && !pts.empty()) {
            const QPointF c = pts[0];
            const double ratio = number(ruler, "ratio", 1), angle = number(ruler, "angle", 0);
            std::vector<double> radii{15, 30, 45, 60, 75};
            if (hover_) {
                const double dx = hover_->x() - c.x(), dy = hover_->y() - c.y(), rot = angle * kPi / 180.0;
                const double lx = dx * std::cos(rot) + dy * std::sin(rot), ly = (-dx * std::sin(rot) + dy * std::cos(rot)) / ratio;
                radii.push_back(std::hypot(lx, ly));
            }
            painter.save();
            painter.translate(pt(c.x(), c.y()));
            painter.rotate(angle);
            for (const double r : radii) painter.drawEllipse(QPointF(0, 0), r * view_.scale, r * ratio * view_.scale);
            painter.restore();
        } else if (kind == "radial" && !pts.empty()) {
            const QPointF c = pts[0];
            for (int k = 0; k < 24; ++k) {
                const double a = k * kPi / 12;
                painter.drawLine(pt(c.x(), c.y()), pt(c.x() + std::cos(a) * 2000, c.y() + std::sin(a) * 2000));
            }
        } else if (kind == "perspective" && !pts.empty()) {
            if (const auto h = core::rulers::horizon(ruler)) {
                painter.setPen(QPen(theme::accent(), 1.3));
                line_across(painter, QPointF(h->first.x, h->first.y), QPointF(h->second.x, h->second.y));
                painter.setPen(QPen(colour, 1));
            }
            for (const QPointF& v : pts) {
                for (int k = 0; k < 16; ++k) {
                    const double a = k * kPi / 8;
                    painter.drawLine(pt(v.x(), v.y()), pt(v.x() + std::cos(a) * 2000, v.y() + std::sin(a) * 2000));
                }
            }
            if (truthy(ruler, "grid", false)) {  // パースのグリッド: the ground
                painter.setPen(QPen(colour, 1, Qt::DashLine));
                for (const auto& [a, b] : core::rulers::perspective_grid(ruler, page_size)) painter.drawLine(pt(a.x, a.y), pt(b.x, b.y));
                painter.setPen(QPen(colour, 1));
            }
            if (hover_) {
                painter.setPen(QPen(colour, 1.6, Qt::DotLine));
                for (const auto& d : core::rulers::directions(ruler, {hover_->x(), hover_->y()})) line_across(painter, *hover_, QPointF(d.x, d.y));
            }
        } else if ((kind == "rect" || kind == "ellipse" || kind == "polygon") && pts.size() >= 2) {
            if (kind != "polygon" || pts.size() >= 3) {
                const auto lines = core::rulers::outline(ruler, page_size);
                if (!lines.empty()) draw_poly(lines.front(), out_x, out_y);
            } else {
                QPolygonF line;
                for (const QPointF& q : pts) line << pt(q.x(), q.y());
                painter.drawPolyline(line);
            }
        } else if ((kind == "parallel_curve" || kind == "radial_curve" || kind == "multi_curve") && pts.size() >= 2) {
            for (const auto& line : core::rulers::outline(ruler, page_size)) draw_poly(line, out_x, out_y);
            if (kind == "radial_curve" && ruler.contains("center") && core::py_truthy(ruler["center"])) {
                const auto c = core::rulers::xy_of(ruler["center"]);
                painter.drawEllipse(pt(c.x, c.y), 5, 5);
            }
        } else if (kind == "symmetry" && pts.size() >= 2) {
            painter.setPen(QPen(QColor(QStringLiteral("#ae3ec9")), 1.5, Qt::DashDotLine));
            const QPointF a = pts[0];
            const double base = std::atan2(pts[1].y() - a.y(), pts[1].x() - a.x());
            const int copies = static_cast<int>(number(ruler, "copies", 2));
            const int count = copies == 2 ? 1 : copies;
            for (int k = 0; k < count; ++k) {
                const double angle = copies > 2 ? base + k * 2 * kPi / copies : base;
                line_across(painter, a, QPointF(std::cos(angle), std::sin(angle)));
            }
        } else if (kind == "guide") {
            const auto lines = core::rulers::outline(ruler, page_size);
            painter.setPen(QPen(QColor(0, 170, 200), 1, Qt::DashLine));
            for (const auto& line : lines) draw_poly(line, out_x, out_y);
        }
    } catch (const std::exception&) {
        // (a ruler that cannot be drawn as it is: its points only)
    }
    // control points
    painter.setPen(QPen(colour, 1.3));
    painter.setBrush(QColor(Qt::white));
    for (const QPointF& q : pts) {
        const QPointF s = pt(q.x(), q.y());
        painter.drawRect(QRectF(s.x() - 4, s.y() - 4, 8, 8));
    }
    painter.setBrush(Qt::NoBrush);
}

void PageCanvas::draw_rulers(QPainter& painter) const {
    if (page() == nullptr) return;
    painter.save();
    if (rulers_visible || tool_ == QLatin1String("ruler")) {
        for (const core::Json& r : page_rulers()) {
            if (!truthy(r, "visible", true) && tool_ != QLatin1String("ruler")) continue;
            const core::Json& shown = ruler_drag_ && ruler_drag_->id == id_of(r) ? ruler_drag_->ruler : r;
            draw_one_ruler(painter, shown, selected_ruler_id && id_of(r) == *selected_ruler_id);
        }
    }
    if (ruler_draft_ && !ruler_draft_->empty()) {
        std::vector<QPointF> points = *ruler_draft_;
        if (hover_ && clicked_kind(ruler_kind) && ruler_kind != QLatin1String("polygon")) points.push_back(*hover_);
        if (const auto draft = draft_ruler(points)) draw_one_ruler(painter, *draft, true);
    }
    painter.restore();
}

// --- placing rulers -------------------------------------------------------------------------------------------------

std::optional<core::Json> PageCanvas::draft_ruler(const std::vector<QPointF>& given) const {
    // the ruler the points placed so far make (none while it is not one yet)
    std::vector<QPointF> pts = given;
    if (pts.empty()) return std::nullopt;
    const QString kind = ruler_kind;
    if (clicked_kind(kind) && kind != QLatin1String("perspective")) {  // (a double click adds the same point twice)
        std::vector<QPointF> kept{pts.front()};
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (std::hypot(pts[i].x() - kept.back().x(), pts[i].y() - kept.back().y()) > 0.5) kept.push_back(pts[i]);
        pts = kept;
        if (pts.size() < 2) return std::nullopt;
    }
    const auto list = [](const std::vector<QPointF>& ps) {
        core::Json out = core::Json::array();
        for (const QPointF& q : ps) out.push_back(json_point(q.x(), q.y()));
        return out;
    };
    core::Json ruler{{"id", "_draft"}, {"kind", kind.toStdString()}, {"points", list(pts)}, {"active", true}};
    if (kind == QLatin1String("radial_curve")) {  // (the first click is the centre, then the curve)
        if (pts.size() < 3) {
            if (pts.size() < 2) return std::nullopt;
            return core::Json{{"id", "_draft"}, {"kind", "curve"}, {"points", list(std::vector<QPointF>(pts.begin() + 1, pts.end()))}, {"active", true}};
        }
        ruler["center"] = json_point(pts[0].x(), pts[0].y());
        ruler["points"] = list(std::vector<QPointF>(pts.begin() + 1, pts.end()));
    } else if (kind == QLatin1String("multi_curve")) {
        if (!ruler_first_) return core::Json{{"id", "_draft"}, {"kind", "curve"}, {"points", list(pts)}, {"active", true}};
        ruler["points"] = *ruler_first_;
        ruler["points2"] = list(pts);
    }
    if (kind == QLatin1String("line") || kind == QLatin1String("symmetry") || kind == QLatin1String("rect") || kind == QLatin1String("ellipse")) {
        if (pts.size() < 2) return std::nullopt;
        ruler["points"] = list({pts.front(), pts.back()});
        if (kind == QLatin1String("symmetry")) ruler["copies"] = ruler_copies;
    } else if (kind == QLatin1String("parallel")) {
        if (pts.size() < 2) return std::nullopt;
        ruler["points"] = list({pts.front(), pts.back()});
        ruler["angle"] = r2(std::atan2(pts.back().y() - pts.front().y(), pts.back().x() - pts.front().x()) * 180.0 / kPi);
    } else if (kind == QLatin1String("concentric")) {
        ruler["points"] = list({pts.front()});
        if (pts.size() >= 2) {
            const double dx = std::abs(pts.back().x() - pts.front().x()), dy = std::abs(pts.back().y() - pts.front().y());
            if ((modifiers_ & Qt::AltModifier) && dx > 0.5) ruler["ratio"] = core::py_round(std::max(0.05, dy / dx), 3);
        }
    } else if (kind == QLatin1String("radial")) {
        ruler["points"] = list({pts.front()});
    } else if (kind == QLatin1String("perspective")) {
        ruler["points"] = list(std::vector<QPointF>(pts.begin(), pts.begin() + std::min<std::size_t>(pts.size(), std::max(1, ruler_vps))));
    }
    return ruler;
}

void PageCanvas::ruler_press(const QPointF& pos) {
    const QPointF mm = mm_of(pos);
    const QPointF at = grid_point(mm.x(), mm.y());
    if (!ruler_draft_) {
        const core::Json rulers = page_rulers();
        for (const core::Json& r : rulers) {
            if (!truthy(r, "visible", true)) continue;
            const auto pts = points_of(r);
            for (std::size_t i = 0; i < pts.size(); ++i) {
                if (near_point(pos, pts[i])) {
                    selected_ruler_id = id_of(r);
                    ruler_drag_ = RulerDrag{id_of(r), static_cast<int>(i), r, false};
                    update();
                    return;
                }
            }
        }
    }
    const QPointF placed(r2(at.x()), r2(at.y()));
    if (clicked_kind(ruler_kind)) {  // click by click
        if (!ruler_draft_) ruler_draft_.emplace();
        ruler_draft_->push_back(placed);
        if (ruler_kind == QLatin1String("perspective") && static_cast<int>(ruler_draft_->size()) >= ruler_vps) finish_ruler();
        update();
        return;
    }
    ruler_draft_ = std::vector<QPointF>{placed};
    if (ruler_kind == QLatin1String("radial")) {
        finish_ruler();
        return;
    }
    update();  // (dragged kinds: line, parallel, concentric, symmetry, rect, ellipse)
}

bool PageCanvas::ruler_move(const QPointF& pos) {
    const QPointF mm = mm_of(pos);
    const QPointF at = grid_point(mm.x(), mm.y());
    if (ruler_drag_) {
        core::Json& r = ruler_drag_->ruler;
        if (r.contains("points") && r["points"].is_array() && ruler_drag_->index < static_cast<int>(r["points"].size()))
            r["points"][static_cast<std::size_t>(ruler_drag_->index)] = json_point(r2(at.x()), r2(at.y()));
        if (kind_of(r) == "parallel" && points_of(r).size() >= 2) {
            const auto pts = points_of(r);
            r["angle"] = r2(std::atan2(pts[1].y() - pts[0].y(), pts[1].x() - pts[0].x()) * 180.0 / kPi);
        }
        ruler_drag_->moved = true;
        update();
        return true;
    }
    if (ruler_draft_ && !ruler_draft_->empty() && dragged_kind(ruler_kind)) {
        ruler_draft_ = std::vector<QPointF>{ruler_draft_->front(), QPointF(r2(at.x()), r2(at.y()))};
        update();
        return true;
    }
    return false;
}

void PageCanvas::ruler_release() {
    if (ruler_drag_) {
        const RulerDrag drag = *ruler_drag_;
        ruler_drag_.reset();
        if (drag.moved) {
            core::Json change{{"points", drag.ruler["points"]}};
            if (drag.ruler.contains("angle")) change["angle"] = drag.ruler["angle"];
            emit rulerEdited(QString::fromStdString(drag.id), change);
        }
        update();
        return;
    }
    if (ruler_draft_ && !ruler_draft_->empty() && dragged_kind(ruler_kind)) {
        const QPointF a = ruler_draft_->front(), b = ruler_draft_->back();
        const bool box = ruler_kind == QLatin1String("rect") || ruler_kind == QLatin1String("ellipse");
        if (ruler_draft_->size() >= 2 && std::hypot(a.x() - b.x(), a.y() - b.y()) > 1.0 &&
            (!box || std::min(std::abs(a.x() - b.x()), std::abs(a.y() - b.y())) >= 0.5)) {
            finish_ruler();
        } else if (ruler_kind == QLatin1String("concentric")) {
            finish_ruler();  // (a click: circles around that point)
        } else {
            ruler_draft_.reset();
        }
        update();
    }
}

bool PageCanvas::finish_curve() {
    // Enter or a double click ends a curve ruler (a multi-curve's first curve, then its second)
    if (!ruler_draft_ || ruler_draft_->size() < 2) return false;
    if (ruler_kind == QLatin1String("multi_curve") && !ruler_first_) {
        const auto draft = draft_ruler(*ruler_draft_);
        if (draft) ruler_first_ = (*draft)["points"];
        ruler_draft_ = std::vector<QPointF>{};
        update();
        return true;
    }
    if (ruler_kind == QLatin1String("curve") || ruler_kind == QLatin1String("parallel_curve") || ruler_kind == QLatin1String("multi_curve") ||
        (ruler_kind == QLatin1String("radial_curve") && ruler_draft_->size() >= 3)) {
        finish_ruler();
        return true;
    }
    if (ruler_kind == QLatin1String("polygon")) {
        const auto draft = draft_ruler(*ruler_draft_);
        if (draft && points_of(*draft).size() >= 3) {
            finish_ruler();
            return true;
        }
    }
    return false;
}

bool PageCanvas::cancel_ruler() {
    if (!ruler_draft_ && !ruler_first_ && !ruler_drag_) return false;
    ruler_draft_.reset();
    ruler_first_.reset();
    ruler_drag_.reset();
    update();
    return true;
}

void PageCanvas::finish_ruler() {
    const auto ruler = draft_ruler(ruler_draft_.value_or(std::vector<QPointF>{}));
    ruler_draft_.reset();
    ruler_first_.reset();
    if (ruler) {
        core::Json placed = *ruler;
        placed.erase("id");
        placed.erase("active");
        emit rulerPlaced(placed);
    }
    update();
}

// --- 3D -------------------------------------------------------------------------------------------------------------

std::vector<std::pair<std::string, QPointF>> PageCanvas::prim_handles(const core::Json& prim) const {
    std::vector<std::pair<std::string, QPointF>> out;
    const std::string kind = kind_of(prim);
    const core::Json* camera = page_camera(page());
    try {
        if (kind == "mannequin") {
            const auto bone = core::mannequin::skeleton(prim);
            const auto add = [&](std::string_view name) {
                const auto& q = bone.point(name);
                out.emplace_back(std::string(name), QPointF(q[0], q[1]));
            };
            add("pelvis");
            for (const char* name : {"chest", "head", "l_elbow", "r_elbow", "l_hand", "r_hand", "l_fingers", "r_fingers", "l_knee", "r_knee",
                                     "l_ankle", "r_ankle", "l_toe", "r_toe"})
                add(name);
            return out;
        }
        if (kind == "figure") {
            for (const auto& [name, q] : core::mesh3d::figure_handles(prim, camera)) out.emplace_back(name, QPointF(q[0], q[1]));
            const auto box = core::prim3d::prim_bbox(prim, camera);
            out.emplace_back("turn", QPointF(box[0] + box[2] / 2, box[1] - 8));
            return out;
        }
        const auto box = core::prim3d::bbox(prim, camera);
        double px = 0, py = 0;
        if (prim.contains("pos") && prim["pos"].is_array() && prim["pos"].size() >= 2) {
            px = core::to_float(prim["pos"][0]);
            py = core::to_float(prim["pos"][1]);
        }
        out.emplace_back("move", QPointF(px, py));
        out.emplace_back("turn", QPointF(box[0] + box[2] / 2, box[1] - 8));
    } catch (const std::exception&) {
    }
    return out;
}

std::vector<std::pair<QPointF, QPointF>> PageCanvas::prim_lines(const core::Json& prim) const {
    std::vector<std::pair<QPointF, QPointF>> out;
    const core::Json* camera = page_camera(page());
    try {
        if (kind_of(prim) == "mannequin") {
            const auto bone = core::mannequin::skeleton(prim);
            for (const auto& s : bone.segments) out.emplace_back(QPointF(s.a[0], s.a[1]), QPointF(s.b[0], s.b[1]));
            std::vector<QPointF> circle;
            for (int k = 0; k < 17; ++k)
                circle.emplace_back(bone.head_c[0] + bone.head_r * std::cos(k * kPi / 8), bone.head_c[1] + bone.head_r * std::sin(k * kPi / 8));
            for (std::size_t k = 0; k + 1 < circle.size(); ++k) out.emplace_back(circle[k], circle[k + 1]);
            return out;
        }
        if (kind_of(prim) == "figure") {  // (while dragging: the bones, which follow the pen at once)
            const auto sk = core::mesh3d::figure_skeleton(prim);
            std::vector<std::pair<std::string, std::string>> bones{{"pelvis", "neck"}, {"neck", "head"}};
            for (const char side : {'l', 'r'}) {
                const std::string s(1, side);
                for (const auto& [a, b] : {std::pair{"neck", "_shoulder"}, {"_shoulder", "_elbow"}, {"_elbow", "_wrist"}, {"_wrist", "_hand"},
                                           {"pelvis", "_hip"}, {"_hip", "_knee"}, {"_knee", "_ankle"}, {"_ankle", "_toe"}}) {
                    const auto name = [&](const char* n) { return n[0] == '_' ? s + n : std::string(n); };
                    bones.emplace_back(name(a), name(b));
                }
            }
            std::vector<std::string> names;
            for (const auto& [a, b] : bones) {
                for (const std::string& n : {a, b})
                    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
            }
            std::vector<core::mesh3d::Vec3> local;
            for (const std::string& n : names) local.push_back(sk.point(n));
            const auto seen = core::mesh3d::to_page(prim, local, camera);
            const auto where = [&](const std::string& n) {
                const auto i = static_cast<std::size_t>(std::find(names.begin(), names.end(), n) - names.begin());
                return QPointF(seen.pts[i][0], seen.pts[i][1]);
            };
            for (const auto& [a, b] : bones) out.emplace_back(where(a), where(b));
            return out;
        }
        for (const auto& e : core::prim3d::edges(prim, camera)) out.emplace_back(QPointF(e.a[0], e.a[1]), QPointF(e.b[0], e.b[1]));
    } catch (const std::exception&) {
    }
    return out;
}

void PageCanvas::draw_prims(QPainter& painter) const {
    const core::Page* p = page();
    if (p == nullptr || tool_ != QLatin1String("3d") || !p->prims.is_array()) return;
    const core::Json* camera = page_camera(p);
    painter.save();
    for (const core::Json& prim : p->prims) {
        const bool dragged = prim_drag_ && prim_drag_->id == id_of(prim);
        const core::Json& shown = dragged ? prim_drag_->prim : prim;
        const bool selected = selected_prim_id && *selected_prim_id == id_of(prim);
        if (dragged) {  // the part being dragged, drawn live
            painter.setPen(QPen(theme::accent(), 2));
            for (const auto& [a, b] : prim_lines(shown)) painter.drawLine(pt(a.x(), a.y()), pt(b.x(), b.y()));
        }
        for (const auto& [name, at] : prim_handles(shown)) {
            const QPointF q = pt(at.x(), at.y());
            painter.setPen(QPen(kPrim, 1.2));
            painter.setBrush(selected && (name == "pelvis" || name == "move") ? theme::accent() : QColor(Qt::white));
            if (name == "turn") {
                painter.drawEllipse(q, 6, 6);
            } else {
                painter.drawEllipse(q, 4.5, 4.5);
            }
        }
        if (selected) {
            try {
                const auto box = core::prim3d::prim_bbox(shown, camera);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(QStringLiteral("#1c7ed6")), 1, Qt::DashLine));
                painter.drawRect(QRectF(pt(box[0], box[1]), pt(box[0] + box[2], box[1] + box[3])));
            } catch (const std::exception&) {
            }
        }
    }
    painter.restore();
}

void PageCanvas::select_prim(std::optional<std::string> prim_id) {
    if (prim_id != selected_prim_id) {
        selected_prim_id = prim_id;
        emit primSelected(QString::fromStdString(prim_id.value_or(std::string())));
    }
    update();
}

void PageCanvas::prim_press(const QPointF& pos) {
    const core::Page* p = page();
    if (p == nullptr) return;
    const QPointF mm = mm_of(pos);
    std::vector<core::Json> prims;
    if (p->prims.is_array())
        for (const core::Json& prim : p->prims) prims.push_back(prim);
    // the selected one first, so its handles win where figures overlap
    std::stable_sort(prims.begin(), prims.end(), [this](const core::Json& a, const core::Json& b) {
        const bool sa = selected_prim_id && id_of(a) == *selected_prim_id, sb = selected_prim_id && id_of(b) == *selected_prim_id;
        return sa && !sb;
    });
    for (const core::Json& prim : prims) {
        for (const auto& [name, at] : prim_handles(prim)) {
            if (near_point(pos, at)) {
                select_prim(id_of(prim));
                prim_drag_ = PrimDrag{id_of(prim), name, prim, prim, mm, false, false, {}};
                return;
            }
        }
    }
    const core::Json* camera = page_camera(p);
    for (auto it = prims.rbegin(); it != prims.rend(); ++it) {
        try {
            const auto box = core::prim3d::prim_bbox(*it, camera);
            if (box[0] <= mm.x() && mm.x() <= box[0] + box[2] && box[1] <= mm.y() && mm.y() <= box[1] + box[3]) {
                select_prim(id_of(*it));
                const std::string kind = kind_of(*it);
                const std::string handle = kind == "mannequin" || kind == "figure" ? "pelvis" : "move";
                prim_drag_ = PrimDrag{id_of(*it), handle, *it, *it, mm, false, true, {}};
                return;
            }
        } catch (const std::exception&) {
        }
    }
    select_prim(std::nullopt);
}

bool PageCanvas::prim_move(const QPointF& pos) {
    if (!prim_drag_) return false;
    PrimDrag& drag = *prim_drag_;
    const QPointF mm = mm_of(pos);
    const core::Json& orig = drag.orig;
    core::Json& prim = drag.prim;
    const double sx = drag.start.x(), sy = drag.start.y();
    try {
        if (drag.handle == "pelvis" || drag.handle == "move") {
            std::array<double, 3> at{0, 0, 0};
            if (orig.contains("pos") && orig["pos"].is_array())
                for (std::size_t i = 0; i < 3 && i < orig["pos"].size(); ++i) at[i] = core::to_float(orig["pos"][i]);
            prim["pos"] = core::Json::array({core::py_round(at[0] + mm.x() - sx, 3), core::py_round(at[1] + mm.y() - sy, 3), at[2]});
        } else if (drag.handle == "turn") {
            std::array<double, 3> r{0, 0, 0};
            if (orig.contains("rot") && orig["rot"].is_array())
                for (std::size_t i = 0; i < 3 && i < orig["rot"].size(); ++i) r[i] = core::to_float(orig["rot"][i]);
            prim["rot"] = core::Json::array({core::py_round(r[0] - (mm.y() - sy) / 40, 4), core::py_round(r[1] + (mm.x() - sx) / 40, 4), r[2]});
        } else {
            core::Json change;
            if (kind_of(prim) == "figure") {
                change = core::mesh3d::drag_joint(prim, drag.handle, {mm.x(), mm.y()}, page_camera(page()));
            } else {
                change = core::mannequin::pose_to(prim, drag.handle, json_point(mm.x(), mm.y()));
            }
            if (change.is_object() && change.contains("joints") && change["joints"].is_object()) {
                if (!prim.contains("joints") || !prim["joints"].is_object()) prim["joints"] = core::Json::object();
                for (const auto& [joint, values] : change["joints"].items()) {
                    if (!prim["joints"].contains(joint) || !prim["joints"][joint].is_object()) prim["joints"][joint] = core::Json::object();
                    for (const auto& [axis, v] : values.items()) prim["joints"][joint][axis] = v;
                }
            }
        }
    } catch (const std::exception&) {
        // (a drag this figure cannot follow: it stays as it was)
    }
    drag.to = json_point(r2(mm.x()), r2(mm.y()));
    drag.moved = true;
    update();
    return true;
}

void PageCanvas::prim_release() {
    if (!prim_drag_) return;
    const PrimDrag drag = *prim_drag_;
    prim_drag_.reset();
    if (drag.moved) {
        if (drag.handle == "move" || drag.handle == "turn") {
            const char* key = drag.handle == "move" ? "pos" : "rot";
            emit primEdited(QString::fromStdString(drag.id), core::Json{{key, drag.prim[key]}});
        } else if (drag.handle == "pelvis") {
            emit primEdited(QString::fromStdString(drag.id), core::Json{{"pos", drag.prim["pos"]}});
        } else {
            emit primPosed(QString::fromStdString(drag.id), QString::fromStdString(drag.handle), drag.to);
        }
    }
    update();
}

}  // namespace genko::app
