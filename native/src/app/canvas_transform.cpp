// 自由変形 and the warps on the canvas (PC3 C-c, SPEC.md COLOR-07): a box over what the window transforms, taking the
// mouse and the keys until Enter or Esc — Free Transform's handles (move, scale, turn, skew, distort, perspective; the
// commands' own ways), Warp's Bézier mesh, Puppet Warp's pins and Perspective Warp's planes. Each drag let go sends the
// spec for a preview, Enter for the op (transform_layers); the window builds the op (app/main_window_transform.cpp).

#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/page_geometry.hpp"
#include "core/pynum.hpp"
#include "render/layer_transform.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr double kPi = 3.14159265358979323846;
const QColor kBox(QStringLiteral("#1c7ed6"));

double r3(double v) { return core::py_round(v, 3); }

QPointF through(const std::array<double, 6>& m, const QPointF& p) {
    return QPointF(m[0] * p.x() + m[2] * p.y() + m[4], m[1] * p.x() + m[3] * p.y() + m[5]);
}

// m2 after m1.
std::array<double, 6> then(const std::array<double, 6>& m1, const std::array<double, 6>& m2) {
    return {m2[0] * m1[0] + m2[2] * m1[1], m2[1] * m1[0] + m2[3] * m1[1], m2[0] * m1[2] + m2[2] * m1[3], m2[1] * m1[2] + m2[3] * m1[3],
            m2[0] * m1[4] + m2[2] * m1[5] + m2[4], m2[1] * m1[4] + m2[3] * m1[5] + m2[5]};
}

std::optional<std::array<double, 6>> inverse(const std::array<double, 6>& m) {
    const double det = m[0] * m[3] - m[1] * m[2];
    if (std::fabs(det) < 1e-12) return std::nullopt;
    const double a = m[3] / det, b = -m[1] / det, c = -m[2] / det, d = m[0] / det;
    return std::array<double, 6>{a, b, c, d, -(a * m[4] + c * m[5]), -(b * m[4] + d * m[5])};
}

double distance(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

Json point_json(const QPointF& p) { return Json::array({r3(p.x()), r3(p.y())}); }

// The box's corners clockwise from the top left.
std::array<QPointF, 4> corners_of(const std::array<double, 4>& b) {
    return {QPointF(b[0], b[1]), QPointF(b[2], b[1]), QPointF(b[2], b[3]), QPointF(b[0], b[3])};
}

bool inside_quad(const std::array<QPointF, 4>& q, const QPointF& p) {
    QPolygonF poly;
    for (const QPointF& c : q) poly << c;
    return poly.containsPoint(p, Qt::OddEvenFill);
}

}  // namespace

struct PageCanvas::Transforming {
    QString mode;  // free | scale | rotate | skew | distort | perspective | warp | puppet | perspective_warp
    std::array<double, 4> box{0, 0, 0, 0};
    std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
    std::array<QPointF, 4> quad{};
    bool affine = true;
    Json warp;                                      // (warp: its normal form)
    std::vector<std::pair<QPointF, QPointF>> pins;  // (puppet: where each was put and where it is)
    std::vector<std::array<QPointF, 4>> src, dst;   // (perspective_warp: the planes)
    bool warping = false;                           // (perspective_warp: the Warp phase)
    std::optional<QPointF> point;
    struct Drag {
        QString kind;  // move | scale | rotate | corner | edge | skew | perspective | mesh | pin | plane | new_plane
        QString key;
        int index = -1;
        QPointF start;
        std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
        std::array<QPointF, 4> quad{};
        std::vector<QPointF> mesh;
        bool moved = false;
        QPointF at;  // (where the pointer is now)
    };
    std::optional<Drag> drag;

    std::vector<QPointF> mesh_points() const {
        std::vector<QPointF> out;
        if (!warp.contains("mesh")) return out;
        for (const Json& p : warp["mesh"]["points"]) out.emplace_back(p[0].get<double>(), p[1].get<double>());
        return out;
    }
    void set_mesh_points(const std::vector<QPointF>& points) {
        Json list = Json::array();
        for (const QPointF& p : points) list.push_back(point_json(p));
        warp["mesh"]["points"] = std::move(list);
    }
    // Free modes: the handles, by kind (scale: nw n ne e se s sw w at the quad's corners and sides; rotate: none — the
    // outside of the quad turns it).
    std::vector<std::pair<QString, QPointF>> handles() const {
        const auto mid = [](const QPointF& a, const QPointF& b) { return (a + b) / 2.0; };
        return {{QStringLiteral("nw"), quad[0]}, {QStringLiteral("n"), mid(quad[0], quad[1])}, {QStringLiteral("ne"), quad[1]},
                {QStringLiteral("e"), mid(quad[1], quad[2])}, {QStringLiteral("se"), quad[2]}, {QStringLiteral("s"), mid(quad[2], quad[3])},
                {QStringLiteral("sw"), quad[3]}, {QStringLiteral("w"), mid(quad[3], quad[0])}};
    }
};

bool PageCanvas::start_transform(const QString& mode, const std::array<double, 4>& box) {
    if (!(box[2] > box[0] && box[3] > box[1])) return false;
    auto t = std::make_shared<Transforming>();
    t->mode = mode;
    t->box = box;
    t->quad = corners_of(box);
    if (mode == QLatin1String("warp")) {
        t->warp = render::layer_transform::warp_to_json(render::layer_transform::warp_from_json(Json{{"style", "custom"}}, box));
    }
    if (mode == QLatin1String("perspective_warp")) {
        t->src = {corners_of(box)};
        t->dst = t->src;
    }
    xform_ = std::move(t);
    update();
    return true;
}

bool PageCanvas::transforming() const { return xform_ != nullptr; }
QString PageCanvas::transform_mode() const { return xform_ ? xform_->mode : QString(); }
std::optional<QPointF> PageCanvas::transform_point() const { return xform_ ? xform_->point : std::nullopt; }
std::array<double, 4> PageCanvas::transform_box() const { return xform_ ? xform_->box : std::array<double, 4>{0, 0, 0, 0}; }

Json PageCanvas::transform_warp() const { return xform_ ? xform_->warp : Json(); }

void PageCanvas::set_transform_warp(const Json& warp) {
    if (!xform_) return;
    xform_->warp = warp;
    update();
}

void PageCanvas::set_perspective_warping(bool on) {
    if (!xform_) return;
    xform_->warping = on;
    update();
}

bool PageCanvas::perspective_warping() const { return xform_ && xform_->warping; }

std::vector<std::array<QPointF, 4>> PageCanvas::perspective_planes(bool destination) const {
    if (!xform_) return {};
    return destination ? xform_->dst : xform_->src;
}

void PageCanvas::set_perspective_planes(const std::vector<std::array<QPointF, 4>>& src, const std::vector<std::array<QPointF, 4>>& dst) {
    if (!xform_ || src.size() != dst.size()) return;
    xform_->src = src;
    xform_->dst = dst;
    update();
}

std::vector<std::pair<QPointF, QPointF>> PageCanvas::puppet_pins() const { return xform_ ? xform_->pins : std::vector<std::pair<QPointF, QPointF>>{}; }

void PageCanvas::set_puppet_pins(const std::vector<std::pair<QPointF, QPointF>>& pins) {
    if (!xform_) return;
    xform_->pins = pins;
    update();
}

Json PageCanvas::transform_spec() const {
    if (!xform_) return Json();
    const Transforming& t = *xform_;
    if (t.mode == QLatin1String("warp")) return Json{{"warp", t.warp}};
    if (t.mode == QLatin1String("puppet")) {
        Json pins = Json::array();
        for (const auto& [src, dst] : t.pins) pins.push_back(Json{{"src", point_json(src)}, {"dst", point_json(dst)}});
        return Json{{"puppet", Json{{"pins", pins}}}};
    }
    if (t.mode == QLatin1String("perspective_warp")) {
        Json planes = Json::array();
        for (std::size_t i = 0; i < t.src.size(); ++i) {
            Json s = Json::array(), d = Json::array();
            for (const QPointF& p : t.src[i]) s.push_back(point_json(p));
            for (const QPointF& p : t.dst[i]) d.push_back(point_json(p));
            planes.push_back(Json{{"src", s}, {"dst", d}});
        }
        return Json{{"perspective", Json{{"planes", planes}}}};
    }
    if (t.affine) {
        Json m = Json::array();
        for (const double v : t.matrix) m.push_back(core::py_round(v, 6));
        return Json{{"matrix", m}};
    }
    Json q = Json::array();
    for (const QPointF& p : t.quad) q.push_back(point_json(p));
    return Json{{"quad", q}};
}

void PageCanvas::finish_transform() {
    if (!xform_) return;
    const Json spec = transform_spec();
    xform_.reset();
    update();
    emit transformCommitted(spec);
}

void PageCanvas::cancel_transform() {
    if (!xform_) return;
    xform_.reset();
    update();
    emit transformCancelled();
}

// --- the mouse and the keys -------------------------------------------------------------------------------------------

bool PageCanvas::transform_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!xform_) return false;
    Transforming& t = *xform_;
    using Drag = Transforming::Drag;
    const auto near = [&](const QPointF& at) {
        const QPointF p = pt(at.x(), at.y());
        return std::abs(p.x() - pos.x()) <= 8 && std::abs(p.y() - pos.y()) <= 8;
    };
    t.point = QPointF(r3(mm.x()), r3(mm.y()));
    if (t.mode == QLatin1String("warp")) {
        std::vector<QPointF> points = t.mesh_points();
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (!near(points[i])) continue;
            t.drag = Drag{QStringLiteral("mesh"), {}, static_cast<int>(i), mm, t.matrix, t.quad, points, false, mm};
            return true;
        }
        return true;  // (a click marks where a split goes)
    }
    if (t.mode == QLatin1String("puppet")) {
        for (std::size_t i = 0; i < t.pins.size(); ++i) {
            if (!near(t.pins[i].second)) continue;
            if (modifiers & Qt::AltModifier) {  // (Alt: the pin taken out)
                t.pins.erase(t.pins.begin() + static_cast<std::ptrdiff_t>(i));
                update();
                emit transformPreview(transform_spec());
                return true;
            }
            t.drag = Drag{QStringLiteral("pin"), {}, static_cast<int>(i), mm, t.matrix, t.quad, {}, false, mm};
            return true;
        }
        if (mm.x() >= t.box[0] && mm.x() <= t.box[2] && mm.y() >= t.box[1] && mm.y() <= t.box[3]) {
            t.pins.emplace_back(QPointF(r3(mm.x()), r3(mm.y())), QPointF(r3(mm.x()), r3(mm.y())));
            t.drag = Drag{QStringLiteral("pin"), {}, static_cast<int>(t.pins.size() - 1), mm, t.matrix, t.quad, {}, false, mm};
            update();
        }
        return true;
    }
    if (t.mode == QLatin1String("perspective_warp")) {
        auto& planes = t.warping ? t.dst : t.src;
        for (std::size_t i = 0; i < planes.size(); ++i) {
            for (std::size_t k = 0; k < 4; ++k) {
                if (!near(planes[i][k])) continue;
                t.drag = Drag{QStringLiteral("plane"), {}, static_cast<int>(i * 4 + k), mm, t.matrix, t.quad, {}, false, mm};
                return true;
            }
        }
        if (!t.warping) t.drag = Drag{QStringLiteral("new_plane"), {}, -1, mm, t.matrix, t.quad, {}, false, mm};
        return true;
    }
    // Free Transform's ways
    const QString& mode = t.mode;
    const bool ctrl = (modifiers & Qt::ControlModifier) != 0;
    const bool all = ctrl && (modifiers & Qt::AltModifier) && (modifiers & Qt::ShiftModifier);
    const bool free = mode == QLatin1String("free");
    for (const auto& [key, at] : t.handles()) {
        if (!near(at)) continue;
        const bool corner = key.size() == 2;
        QString kind = QStringLiteral("scale");
        if (mode == QLatin1String("rotate")) {
            kind = QStringLiteral("rotate");
        } else if (corner) {
            if (mode == QLatin1String("distort") || (free && ctrl && !all)) kind = QStringLiteral("corner");
            if (mode == QLatin1String("perspective") || (free && all)) kind = QStringLiteral("perspective");
        } else if (mode == QLatin1String("skew") || mode == QLatin1String("distort") || mode == QLatin1String("perspective") || (free && ctrl)) {
            kind = QStringLiteral("skew");
        }
        if (!t.affine && (kind == QLatin1String("scale") || kind == QLatin1String("skew"))) kind = corner ? QStringLiteral("corner") : QStringLiteral("edge");
        static const std::array<QString, 8> names{QStringLiteral("nw"), QStringLiteral("ne"), QStringLiteral("se"), QStringLiteral("sw"),
                                                  QStringLiteral("n"),  QStringLiteral("e"),  QStringLiteral("s"),  QStringLiteral("w")};
        const int index = static_cast<int>(std::find(names.begin(), names.end(), key) - names.begin());
        t.drag = Drag{kind, key, index, mm, t.matrix, t.quad, {}, false, mm};
        return true;
    }
    // (inside: a move; outside: a turn — 自由変形 and 回転 alone; 回転 turns from anywhere)
    const bool in = inside_quad(t.quad, mm);
    const bool turns = mode == QLatin1String("rotate") || (free && !in);
    t.drag = Drag{turns ? QStringLiteral("rotate") : QStringLiteral("move"), {}, -1, mm, t.matrix, t.quad, {}, false, mm};
    return true;
}

bool PageCanvas::transform_move(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!xform_ || !xform_->drag) return xform_ != nullptr;
    Transforming& t = *xform_;
    Transforming::Drag& d = *t.drag;
    d.moved = true;
    d.at = mm;
    const QPointF delta = mm - d.start;
    const auto& b = t.box;
    if (d.kind == QLatin1String("mesh")) {
        std::vector<QPointF> points = d.mesh;
        const std::size_t nx = t.warp["mesh"]["us"].size() * 3 - 2;  // (3·columns + 1)
        const std::size_t i = std::size_t(d.index), col = i % nx, row = i / nx;
        points[i] = d.mesh[i] + delta;
        if (col % 3 == 0 && row % 3 == 0) {  // (an anchor takes its handles along)
            constexpr std::array<std::array<long, 2>, 4> kAround{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (const auto& [dc, dr] : kAround) {
                const long c = long(col) + dc, r = long(row) + dr;
                const std::size_t ny = points.size() / nx;
                if (c < 0 || r < 0 || c >= long(nx) || r >= long(ny)) continue;
                const std::size_t j = std::size_t(r) * nx + std::size_t(c);
                points[j] = d.mesh[j] + delta;
            }
        }
        t.set_mesh_points(points);
    } else if (d.kind == QLatin1String("pin")) {
        t.pins[std::size_t(d.index)].second = QPointF(r3(mm.x()), r3(mm.y()));
    } else if (d.kind == QLatin1String("plane")) {
        auto& planes = t.warping ? t.dst : t.src;
        const std::size_t plane = std::size_t(d.index) / 4, k = std::size_t(d.index) % 4;
        const QPointF was = planes[plane][k];
        const QPointF to(r3(mm.x()), r3(mm.y()));
        // (corners where it was move with it: the planes' links)
        for (auto& q : planes)
            for (QPointF& c : q)
                if (distance(c, was) < 1e-6) c = to;
        if (!t.warping) t.dst = t.src;
    } else if (d.kind == QLatin1String("new_plane")) {
        // (made when let go)
    } else if (d.kind == QLatin1String("edge")) {  // (a free quad's side: its two corners moved)
        const std::size_t e = std::size_t(d.index - 4);
        t.quad = d.quad;
        t.quad[e] = d.quad[e] + delta;
        t.quad[(e + 1) % 4] = d.quad[(e + 1) % 4] + delta;
    } else if (d.kind == QLatin1String("move")) {
        if (t.affine) {
            t.matrix = then(d.matrix, {1, 0, 0, 1, delta.x(), delta.y()});
        } else {
            for (std::size_t k = 0; k < 4; ++k) t.quad[k] = d.quad[k] + delta;
        }
    } else if (d.kind == QLatin1String("rotate")) {
        const QPointF c = through(d.matrix, QPointF((b[0] + b[2]) / 2, (b[1] + b[3]) / 2));
        double angle = std::atan2(mm.y() - c.y(), mm.x() - c.x()) - std::atan2(d.start.y() - c.y(), d.start.x() - c.x());
        if (modifiers & Qt::ShiftModifier) angle = std::nearbyint(angle / (kPi / 12)) * (kPi / 12);
        const double cs = std::cos(angle), sn = std::sin(angle);
        const std::array<double, 6> turn{cs, sn, -sn, cs, c.x() - cs * c.x() + sn * c.y(), c.y() - sn * c.x() - cs * c.y()};
        if (t.affine) {
            t.matrix = then(d.matrix, turn);
        } else {
            for (std::size_t k = 0; k < 4; ++k) t.quad[k] = through(turn, d.quad[k]);
        }
    } else if (d.kind == QLatin1String("scale") || d.kind == QLatin1String("skew")) {
        const auto inv = inverse(d.matrix);
        if (!inv) return true;
        const QPointF p = through(*inv, mm), s = through(*inv, d.start);  // (in the box's own place)
        const QString& key = d.key;
        const auto nonzero = [](double v) { return std::fabs(v) > 1e-9 ? v : 1e-9; };
        std::array<double, 6> local{1, 0, 0, 1, 0, 0};
        if (d.kind == QLatin1String("skew")) {
            if (key == QLatin1String("n") || key == QLatin1String("s")) {
                const double ay = key == QLatin1String("n") ? b[3] : b[1];
                const double k = (p.x() - s.x()) / nonzero(s.y() - ay);
                local = {1, 0, k, 1, -k * ay, 0};
            } else {
                const double ax = key == QLatin1String("w") ? b[2] : b[0];
                const double k = (p.y() - s.y()) / nonzero(s.x() - ax);
                local = {1, k, 0, 1, 0, -k * ax};
            }
        } else {
            const bool west = key.contains(QLatin1Char('w')), east = key.contains(QLatin1Char('e'));
            const bool north = key.startsWith(QLatin1Char('n')), south = key.startsWith(QLatin1Char('s'));
            const double ax = west ? b[2] : east ? b[0] : (b[0] + b[2]) / 2;
            const double ay = north ? b[3] : south ? b[1] : (b[1] + b[3]) / 2;
            double sx = west || east ? (p.x() - ax) / nonzero(s.x() - ax) : 1.0;
            double sy = north || south ? (p.y() - ay) / nonzero(s.y() - ay) : 1.0;
            if ((modifiers & Qt::ShiftModifier) && key.size() == 2) sx = sy = (std::fabs(sx) + std::fabs(sy)) / 2 * (sx * sy > 0 ? 1 : -1);
            local = {sx, 0, 0, sy, ax - sx * ax, ay - sy * ay};
        }
        t.matrix = then(local, d.matrix);
    } else if (d.kind == QLatin1String("corner")) {
        if (t.affine) {
            t.quad = {through(t.matrix, corners_of(b)[0]), through(t.matrix, corners_of(b)[1]), through(t.matrix, corners_of(b)[2]),
                      through(t.matrix, corners_of(b)[3])};
            d.quad = t.quad;
            t.affine = false;
        }
        t.quad[std::size_t(d.index)] = d.quad[std::size_t(d.index)] + delta;
    } else if (d.kind == QLatin1String("perspective")) {
        if (t.affine) {
            t.quad = {through(t.matrix, corners_of(b)[0]), through(t.matrix, corners_of(b)[1]), through(t.matrix, corners_of(b)[2]),
                      through(t.matrix, corners_of(b)[3])};
            d.quad = t.quad;
            t.affine = false;
        }
        // (the corner and its mirror on the same side: across moves the other end of its top or bottom, down the other
        // end of its left or right side)
        const std::size_t i = std::size_t(d.index);
        t.quad = d.quad;
        if (std::fabs(delta.x()) >= std::fabs(delta.y())) {
            const std::size_t other = i == 0 ? 1 : i == 1 ? 0 : i == 2 ? 3 : 2;
            t.quad[i] = d.quad[i] + QPointF(delta.x(), 0);
            t.quad[other] = d.quad[other] - QPointF(delta.x(), 0);
        } else {
            const std::size_t other = i == 0 ? 3 : i == 3 ? 0 : i == 1 ? 2 : 1;
            t.quad[i] = d.quad[i] + QPointF(0, delta.y());
            t.quad[other] = d.quad[other] - QPointF(0, delta.y());
        }
    }
    if (t.affine && t.mode != QLatin1String("warp") && t.mode != QLatin1String("puppet") && t.mode != QLatin1String("perspective_warp")) {
        const auto c = corners_of(b);
        for (std::size_t k = 0; k < 4; ++k) t.quad[k] = through(t.matrix, c[k]);
    }
    update();
    return true;
}

bool PageCanvas::transform_release(const QPointF& mm) {
    if (!xform_) return false;
    Transforming& t = *xform_;
    if (!t.drag) return true;
    const Transforming::Drag d = *t.drag;
    t.drag.reset();
    if (d.kind == QLatin1String("new_plane")) {  // (Perspective Warp's Layout: a plane drawn on nothing)
        const QPointF a = d.start;
        if (std::fabs(mm.x() - a.x()) >= 1.0 && std::fabs(mm.y() - a.y()) >= 1.0) {
            std::array<QPointF, 4> q = corners_of({r3(std::min(a.x(), mm.x())), r3(std::min(a.y(), mm.y())), r3(std::max(a.x(), mm.x())), r3(std::max(a.y(), mm.y()))});
            // (a corner near one already there is that corner: the planes linked)
            for (QPointF& c : q) {
                for (const auto& plane : t.src)
                    for (const QPointF& other : plane)
                        if (distance(pt(c.x(), c.y()), pt(other.x(), other.y())) <= 10) c = other;
            }
            t.src.push_back(q);
            t.dst = t.src;
        }
        update();
        return true;
    }
    update();
    if (d.moved || d.kind == QLatin1String("pin")) emit transformPreview(transform_spec());
    return true;
}

bool PageCanvas::transform_key(QKeyEvent* event) {
    if (!xform_) return false;
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        finish_transform();
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        cancel_transform();
        return true;
    }
    return false;
}

// --- drawing ---------------------------------------------------------------------------------------------------------

void PageCanvas::draw_transform(QPainter& painter) const {
    if (!xform_) return;
    const Transforming& t = *xform_;
    painter.save();
    painter.setBrush(Qt::NoBrush);
    const auto poly = [&](const std::array<QPointF, 4>& q) {
        QPolygonF shown;
        for (const QPointF& p : q) shown << pt(p.x(), p.y());
        return shown;
    };
    const auto dot = [&](const QPointF& at, bool round) {
        const QPointF h = pt(at.x(), at.y());
        painter.setPen(QPen(kBox, 1.5));
        painter.setBrush(Qt::white);
        if (round) {
            painter.drawEllipse(h, 5, 5);
        } else {
            painter.drawRect(QRectF(h.x() - 4, h.y() - 4, 8, 8));
        }
        painter.setBrush(Qt::NoBrush);
    };
    if (t.mode == QLatin1String("warp")) {
        try {
            const auto w = render::layer_transform::warp_from_json(t.warp, t.box);
            painter.setPen(QPen(theme::accent(), 1.2));
            constexpr int kSteps = 24;
            for (int k = 0; k <= 4; ++k) {
                const double f = k / 4.0;
                QPolygonF across, down;
                for (int i = 0; i <= kSteps; ++i) {
                    const auto a = w.map(t.box[0] + (t.box[2] - t.box[0]) * i / kSteps, t.box[1] + (t.box[3] - t.box[1]) * f);
                    const auto c = w.map(t.box[0] + (t.box[2] - t.box[0]) * f, t.box[1] + (t.box[3] - t.box[1]) * i / kSteps);
                    across << pt(a.x, a.y);
                    down << pt(c.x, c.y);
                }
                painter.drawPolyline(across);
                painter.drawPolyline(down);
            }
        } catch (const std::exception&) {
        }
        const std::vector<QPointF> points = t.mesh_points();
        if (!points.empty()) {
            const std::size_t nx = t.warp["mesh"]["us"].size() * 3 - 2;
            painter.setPen(QPen(QColor(120, 120, 120), 1, Qt::DotLine));
            for (std::size_t i = 0; i < points.size(); ++i) {
                const std::size_t col = i % nx, row = i / nx;
                if (col % 3 != 0 && row % 3 != 0) continue;
                if (col % 3 == 0 && row % 3 == 0) continue;
                // (a handle: a line to its anchor)
                const std::size_t ac = col % 3 == 0 ? col : (col % 3 == 1 ? col - 1 : col + 1);
                const std::size_t ar = row % 3 == 0 ? row : (row % 3 == 1 ? row - 1 : row + 1);
                const QPointF anchor = points[ar * nx + ac];
                painter.drawLine(pt(anchor.x(), anchor.y()), pt(points[i].x(), points[i].y()));
            }
            for (std::size_t i = 0; i < points.size(); ++i) dot(points[i], !((i % nx) % 3 == 0 && (i / nx) % 3 == 0));
        }
    } else if (t.mode == QLatin1String("puppet")) {
        painter.setPen(QPen(kBox, 1, Qt::DashLine));
        painter.drawPolygon(poly(corners_of(t.box)));
        for (const auto& [src, dst] : t.pins) {
            painter.setPen(QPen(QColor(120, 120, 120), 1, Qt::DotLine));
            painter.drawLine(pt(src.x(), src.y()), pt(dst.x(), dst.y()));
            const QPointF h = pt(dst.x(), dst.y());
            painter.setPen(QPen(Qt::black, 1.5));
            painter.setBrush(QColor(255, 210, 0));
            painter.drawEllipse(h, 5, 5);
            painter.setBrush(Qt::NoBrush);
        }
    } else if (t.mode == QLatin1String("perspective_warp")) {
        const auto& planes = t.warping ? t.dst : t.src;
        for (const auto& q : planes) {
            painter.setPen(QPen(t.warping ? theme::accent() : kBox, 1.5));
            painter.drawPolygon(poly(q));
            painter.setPen(QPen(t.warping ? theme::accent() : kBox, 0.8, Qt::DotLine));
            for (int k = 1; k < 4; ++k) {
                const double f = k / 4.0;
                const QPointF a = q[0] + (q[1] - q[0]) * f, c = q[3] + (q[2] - q[3]) * f;
                const QPointF e = q[0] + (q[3] - q[0]) * f, g = q[1] + (q[2] - q[1]) * f;
                painter.drawLine(pt(a.x(), a.y()), pt(c.x(), c.y()));
                painter.drawLine(pt(e.x(), e.y()), pt(g.x(), g.y()));
            }
            for (const QPointF& c : q) dot(c, false);
        }
        if (t.drag && t.drag->kind == QLatin1String("new_plane") && t.drag->moved) {
            painter.setPen(QPen(kBox, 1, Qt::DashLine));
            const QPointF a = t.drag->start, e = t.drag->at;
            painter.drawRect(QRectF(pt(std::min(a.x(), e.x()), std::min(a.y(), e.y())), pt(std::max(a.x(), e.x()), std::max(a.y(), e.y()))));
        }
    } else {
        painter.setPen(QPen(Qt::white, 2.5));
        painter.drawPolygon(poly(t.quad));
        painter.setPen(QPen(kBox, 1.2));
        painter.drawPolygon(poly(t.quad));
        for (const auto& [key, at] : t.handles()) dot(at, false);
        const QPointF c = (t.quad[0] + t.quad[1] + t.quad[2] + t.quad[3]) / 4.0;
        const QPointF h = pt(c.x(), c.y());
        painter.setPen(QPen(kBox, 1.2));
        painter.drawEllipse(h, 4, 4);  // (the reference point: what it turns about)
    }
    painter.restore();
}

}  // namespace genko::app
