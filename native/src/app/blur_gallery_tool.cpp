// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): ぼかしギャラリー's work over the canvas (app/blur_gallery_tool.hpp).
// The settings: core/filter_kinds (blur_field, blur_iris, blur_tilt_shift, blur_path, blur_spin); the formulas:
// render/fx_gallery_blur.cpp (PhotoCraft v0.2.0's gallery.rs). The words are Photoshop's (日本語版).

#include "app/blur_gallery_tool.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "app/canvas.hpp"
#include "app/filter_forms.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/adjust.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyops.hpp"
#include "service/wording.hpp"

namespace genko::app::blur_gallery {

namespace {

using core::Json;

constexpr double kPi = std::numbers::pi;
constexpr double kGrab = 9.0;  // (how near a handle a press takes it: the screen's pixels)

enum class Grip { None, Pin, RadiusX, RadiusY, Feather, Focus, Transition, Turn, Point };

double num(const Json& v, const char* key, double fallback) {
    const Json* x = core::get(v, key);
    return x != nullptr && x->is_number() && !x->is_boolean() ? x->get<double>() : fallback;
}

// The work over the canvas: the settings as they are now, the base box (mm), what is chosen and grabbed, the panel.
struct Work {
    QPointer<MainWindow> window;
    std::string kind;
    Json params;
    std::array<double, 4> box{0, 0, 1, 1};  // (x0, y0, x1, y1 mm)
    std::function<void(const Json&)> send;
    std::function<void(const std::optional<Json>&)> preview;
    std::function<bool()> previewing;
    MainWindow::Asked asked;
    int chosen = 0;  // (a pin, or a path)
    int point = -1;  // (a path's point)
    Grip grip = Grip::None;
    QPointer<QDialog> panel;
    QTimer* timer = nullptr;
    std::function<void()> fields;  // (the panel's fields from what is chosen)
    bool shown = true;             // (プレビュー)
    bool done = false;

    double w() const { return std::max(box[2] - box[0], 1e-6); }
    double h() const { return std::max(box[3] - box[1], 1e-6); }
    double shortest() const { return std::min(w(), h()); }
    QPointF at_share(double x, double y) const { return {box[0] + x * w(), box[1] + y * h()}; }
    std::array<double, 2> share_of(QPointF mm) const { return {(mm.x() - box[0]) / w(), (mm.y() - box[1]) / h()}; }
    Json& pins() { return params["pins"]; }
    Json& paths() { return params["paths"]; }
    bool has_pins() const { return kind == "blur_field" || kind == "blur_iris" || kind == "blur_spin"; }
    // A place in a pin's ellipse (its radii's units) on the page.
    QPointF in_ellipse(const Json& pin, double u, double v) const {
        const double a = num(pin, "angle", 0) * kPi / 180.0, ss = shortest();
        const double lu = u * num(pin, "radius_x", 0.3) * ss, lv = v * num(pin, "radius_y", 0.3) * ss;
        const QPointF c = at_share(num(pin, "x", 0.5), num(pin, "y", 0.5));
        return {c.x() + lu * std::cos(a) - lv * std::sin(a), c.y() + lu * std::sin(a) + lv * std::cos(a)};
    }
    // チルトシフト's centre, its band's normal and its run along it.
    QPointF centre() const { return at_share(num(params, "center_x", 0.5), num(params, "center_y", 0.5)); }
    QPointF normal() const {
        const double a = num(params, "angle", 0) * kPi / 180.0;
        return {std::sin(a), std::cos(a)};
    }
    QPointF along() const {
        const double a = num(params, "angle", 0) * kPi / 180.0;
        return {std::cos(a), -std::sin(a)};
    }
};

std::weak_ptr<Work>& current() {
    static std::weak_ptr<Work> work;
    return work;
}

double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double length(QPointF a) { return std::hypot(a.x(), a.y()); }

// The settings shown on the page a moment after a change (none: as it is).
void schedule(const std::shared_ptr<Work>& work) {
    if (work->timer != nullptr) work->timer->start();
    if (work->window != nullptr && work->window->canvas() != nullptr) work->window->canvas()->update();
}

void show_now(const std::shared_ptr<Work>& work) {
    if (!work->preview || work->done) return;
    if (!work->shown) {
        work->preview(std::nullopt);
        return;
    }
    try {  // (settings the kind cannot take — a path of one point —: no preview)
        work->preview(core::normalize_filter(work->kind, work->params, nullptr));
    } catch (const std::exception&) {
        work->preview(std::nullopt);
    }
}

// The work let go (`sent`: its settings sent first), the canvas and the panel as they were.
void finish(const std::shared_ptr<Work>& work, bool sent) {
    if (work->done) return;
    work->done = true;
    if (work->preview) work->preview(std::nullopt);
    if (work->window != nullptr && work->window->canvas() != nullptr) work->window->canvas()->set_work(nullptr);
    if (work->panel != nullptr) {
        work->panel->hide();
        work->panel->deleteLater();
    }
    if (work->timer != nullptr) work->timer->stop();
    if (!sent || !work->send) return;
    Json params = work->params;
    try {
        params = core::normalize_filter(work->kind, params, nullptr);
    } catch (const std::exception& error) {
        if (work->window != nullptr) work->window->flash(wording::error(QString::fromUtf8(error.what())), 5000, true);
        return;
    }
    work->send(params);
}

// --- drawing over the page -----------------------------------------------------------------------------------------------------

void draw_handle(QPainter& p, QPointF at, bool chosen, bool round = false) {
    p.setPen(QPen(theme::accent(), 1.5));
    p.setBrush(chosen ? theme::accent() : QColor(255, 255, 255, 230));
    if (round) p.drawEllipse(at, 5.5, 5.5);
    else p.drawRect(QRectF(at.x() - 4.5, at.y() - 4.5, 9, 9));
}

// A pin's ellipse (a superellipse of `m` — an iris's roundness —, scaled by `k`) as the painter draws it.
QPainterPath ellipse_path(const Work& work, const Json& pin, double m, double k, const std::function<QPointF(QPointF)>& at) {
    QPainterPath path;
    for (int i = 0; i <= 96; ++i) {
        const double t = 2.0 * kPi * i / 96.0;
        const double c = std::cos(t), s = std::sin(t);
        const double u = (c < 0 ? -1.0 : 1.0) * std::pow(std::fabs(c), 2.0 / m), v = (s < 0 ? -1.0 : 1.0) * std::pow(std::fabs(s), 2.0 / m);
        const QPointF q = at(work.in_ellipse(pin, u * k, v * k));
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    return path;
}

void paint(const Work& work, QPainter& p, const std::function<QPointF(QPointF)>& at, double per_mm) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = theme::accent();
    // the base box the shares are of
    p.setPen(QPen(QColor(128, 128, 128, 140), 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(at({work.box[0], work.box[1]}), at({work.box[2], work.box[3]})));
    if (work.has_pins()) {
        const Json& pins = work.params["pins"];
        for (std::size_t i = 0; i < pins.size(); ++i) {
            const Json& pin = pins[i];
            const bool chosen = static_cast<int>(i) == work.chosen;
            const QPointF c = at(work.at_share(num(pin, "x", 0.5), num(pin, "y", 0.5)));
            if (work.kind != "blur_field") {
                const double m = work.kind == "blur_iris" ? 2.0 + std::clamp(num(pin, "roundness", 0), 0.0, 100.0) / 100.0 * 6.0 : 2.0;
                p.setPen(QPen(ink, chosen ? 2.0 : 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(ellipse_path(work, pin, m, 1.0, at));
                if (work.kind == "blur_iris") {
                    p.setPen(QPen(ink, 1.0, Qt::DashLine));
                    p.drawPath(ellipse_path(work, pin, m, std::clamp(num(pin, "feather", 0.5), 0.0, 0.99), at));
                }
                if (chosen) {
                    draw_handle(p, at(work.in_ellipse(pin, 1, 0)), work.grip == Grip::RadiusX);
                    draw_handle(p, at(work.in_ellipse(pin, 0, 1)), work.grip == Grip::RadiusY);
                    if (work.kind == "blur_iris") {
                        const double f = std::clamp(num(pin, "feather", 0.5), 0.0, 0.99) * std::sqrt(0.5);
                        draw_handle(p, at(work.in_ellipse(pin, f, f)), work.grip == Grip::Feather, true);
                    }
                }
            }
            // the pin: a ring and its centre (the chosen one filled)
            p.setPen(QPen(ink, 2.0));
            p.setBrush(chosen ? QBrush(ink) : QBrush(QColor(255, 255, 255, 200)));
            p.drawEllipse(c, 8, 8);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(chosen ? QColor(255, 255, 255) : ink, 1.5));
            p.drawEllipse(c, 3, 3);
        }
        return;
    }
    if (work.kind == "blur_tilt_shift") {
        const QPointF c = work.centre(), n = work.normal(), t = work.along();
        const double ss = work.shortest(), reach = std::hypot(work.w(), work.h());
        const double focus = num(work.params, "focus", 0.1) * ss, edge = focus + num(work.params, "transition", 0.15) * ss;
        for (const double side : {-1.0, 1.0}) {
            for (const auto& [d, style] : {std::pair{focus, Qt::SolidLine}, std::pair{edge, Qt::DashLine}}) {
                const QPointF o = c + n * (side * d);
                p.setPen(QPen(ink, 1.5, style));
                p.drawLine(at(o - t * reach), at(o + t * reach));
            }
        }
        draw_handle(p, at(c + n * focus), work.grip == Grip::Focus);
        draw_handle(p, at(c + n * edge), work.grip == Grip::Transition);
        const QPointF turn = c + t * (48.0 / std::max(per_mm, 1e-6));
        p.setPen(QPen(ink, 1.2));
        p.drawLine(at(c), at(turn));
        draw_handle(p, at(turn), work.grip == Grip::Turn, true);
        p.setPen(QPen(ink, 2.0));
        p.setBrush(QColor(255, 255, 255, 200));
        p.drawEllipse(at(c), 8, 8);
        return;
    }
    // パスぼかし: each path's points and its direction's arrow at its end
    const Json& paths = work.params["paths"];
    for (std::size_t k = 0; k < paths.size(); ++k) {
        const Json& points = paths[k]["points"];
        const bool chosen = static_cast<int>(k) == work.chosen;
        QPainterPath line;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const QPointF q = at({points[i][0].get<double>(), points[i][1].get<double>()});
            if (i == 0) line.moveTo(q);
            else line.lineTo(q);
        }
        p.setPen(QPen(ink, chosen ? 2.5 : 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        if (points.size() >= 2) {
            const QPointF a = at({points[points.size() - 2][0].get<double>(), points[points.size() - 2][1].get<double>()});
            const QPointF b = at({points.back()[0].get<double>(), points.back()[1].get<double>()});
            const double ang = std::atan2(b.y() - a.y(), b.x() - a.x());
            for (const double side : {-0.5, 0.5})
                p.drawLine(b, b - QPointF(std::cos(ang + side), std::sin(ang + side)) * 12.0);
        }
        for (std::size_t i = 0; i < points.size(); ++i)
            draw_handle(p, at({points[i][0].get<double>(), points[i][1].get<double>()}), chosen && static_cast<int>(i) == work.point);
    }
}

// --- the mouse and the keys ------------------------------------------------------------------------------------------------

bool near(QPointF a, QPointF b, double per_mm) { return length(a - b) * per_mm <= kGrab; }

bool press(const std::shared_ptr<Work>& work, QPointF mm, double per_mm) {
    work->grip = Grip::None;
    if (work->has_pins()) {
        Json& pins = work->pins();
        if (work->chosen >= 0 && work->chosen < static_cast<int>(pins.size()) && work->kind != "blur_field") {
            const Json& pin = pins[static_cast<std::size_t>(work->chosen)];
            if (near(mm, work->in_ellipse(pin, 1, 0), per_mm)) work->grip = Grip::RadiusX;
            else if (near(mm, work->in_ellipse(pin, 0, 1), per_mm)) work->grip = Grip::RadiusY;
            else if (work->kind == "blur_iris") {
                const double f = std::clamp(num(pin, "feather", 0.5), 0.0, 0.99) * std::sqrt(0.5);
                if (near(mm, work->in_ellipse(pin, f, f), per_mm)) work->grip = Grip::Feather;
            }
            if (work->grip != Grip::None) return true;
        }
        for (std::size_t i = pins.size(); i-- > 0;) {
            if (near(mm, work->at_share(num(pins[i], "x", 0.5), num(pins[i], "y", 0.5)), per_mm)) {
                work->chosen = static_cast<int>(i);
                work->grip = Grip::Pin;
                if (work->fields) work->fields();
                return true;
            }
        }
        if (pins.size() >= core::kBlurGalleryMostPins) {
            if (work->window != nullptr)
                work->window->flash(QStringLiteral("ピンは %1 個までです").arg(core::kBlurGalleryMostPins), 3000);
            return true;
        }
        // (a new pin where it was pressed, as the chosen one is)
        Json made = work->chosen >= 0 && work->chosen < static_cast<int>(pins.size()) ? pins[static_cast<std::size_t>(work->chosen)] : Json::object();
        const auto [x, y] = work->share_of(mm);
        made["x"] = x;
        made["y"] = y;
        pins.push_back(made);
        work->chosen = static_cast<int>(pins.size()) - 1;
        work->grip = Grip::Pin;
        if (work->fields) work->fields();
        schedule(work);
        return true;
    }
    if (work->kind == "blur_tilt_shift") {
        const QPointF c = work->centre(), n = work->normal(), t = work->along();
        const double ss = work->shortest();
        const double focus = num(work->params, "focus", 0.1) * ss, edge = focus + num(work->params, "transition", 0.15) * ss;
        if (near(mm, c + t * (48.0 / std::max(per_mm, 1e-6)), per_mm)) work->grip = Grip::Turn;
        else if (near(mm, c + n * focus, per_mm)) work->grip = Grip::Focus;
        else if (near(mm, c + n * edge, per_mm)) work->grip = Grip::Transition;
        else {
            const auto [x, y] = work->share_of(mm);
            work->params["center_x"] = x;
            work->params["center_y"] = y;
            work->grip = Grip::Pin;
            schedule(work);
        }
        return true;
    }
    // パスぼかし: a point taken, else one added at the chosen path's end (a new path when there is none)
    Json& paths = work->paths();
    for (std::size_t k = 0; k < paths.size(); ++k) {
        const Json& points = paths[k]["points"];
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (near(mm, {points[i][0].get<double>(), points[i][1].get<double>()}, per_mm)) {
                work->chosen = static_cast<int>(k);
                work->point = static_cast<int>(i);
                work->grip = Grip::Point;
                if (work->fields) work->fields();
                return true;
            }
        }
    }
    if (work->chosen < 0 || work->chosen >= static_cast<int>(paths.size())) {
        if (paths.size() >= core::kBlurPathMostPaths) return true;
        paths.push_back(Json{{"points", Json::array()}, {"speed", 50}, {"taper", 0}});
        work->chosen = static_cast<int>(paths.size()) - 1;
    }
    Json& points = paths[static_cast<std::size_t>(work->chosen)]["points"];
    if (points.size() >= core::kBlurPathMostPoints) return true;
    points.push_back(Json::array({mm.x(), mm.y()}));
    work->point = static_cast<int>(points.size()) - 1;
    work->grip = Grip::Point;
    if (work->fields) work->fields();
    schedule(work);
    return true;
}

bool drag(const std::shared_ptr<Work>& work, QPointF mm) {
    if (work->grip == Grip::None) return true;
    const double ss = work->shortest();
    if (work->has_pins()) {
        Json& pins = work->pins();
        if (work->chosen < 0 || work->chosen >= static_cast<int>(pins.size())) return true;
        Json& pin = pins[static_cast<std::size_t>(work->chosen)];
        const QPointF c = work->at_share(num(pin, "x", 0.5), num(pin, "y", 0.5));
        const double a = num(pin, "angle", 0) * kPi / 180.0;
        const QPointF u{std::cos(a), std::sin(a)}, v{-std::sin(a), std::cos(a)};
        switch (work->grip) {
            case Grip::Pin: {
                const auto [x, y] = work->share_of(mm);
                pin["x"] = std::clamp(x, -10.0, 10.0);
                pin["y"] = std::clamp(y, -10.0, 10.0);
                break;
            }
            case Grip::RadiusX: pin["radius_x"] = std::clamp(std::fabs(dot(mm - c, u)) / ss, 0.001, 10.0); break;
            case Grip::RadiusY: pin["radius_y"] = std::clamp(std::fabs(dot(mm - c, v)) / ss, 0.001, 10.0); break;
            case Grip::Feather: {
                const double du = dot(mm - c, u) / (num(pin, "radius_x", 0.35) * ss), dv = dot(mm - c, v) / (num(pin, "radius_y", 0.25) * ss);
                pin["feather"] = std::clamp(std::hypot(du, dv), 0.0, 0.99);
                break;
            }
            default: break;
        }
    } else if (work->kind == "blur_tilt_shift") {
        const QPointF c = work->centre(), n = work->normal();
        switch (work->grip) {
            case Grip::Pin: {
                const auto [x, y] = work->share_of(mm);
                work->params["center_x"] = std::clamp(x, -10.0, 10.0);
                work->params["center_y"] = std::clamp(y, -10.0, 10.0);
                break;
            }
            case Grip::Focus: work->params["focus"] = std::clamp(std::fabs(dot(mm - c, n)) / ss, 0.0, 2.0); break;
            case Grip::Transition:
                work->params["transition"] = std::clamp(std::fabs(dot(mm - c, n)) / ss - num(work->params, "focus", 0.1), 0.001, 2.0);
                break;
            case Grip::Turn: {
                const QPointF d = mm - c;
                if (length(d) > 1e-9) work->params["angle"] = std::remainder(std::atan2(-d.y(), d.x()) * 180.0 / kPi, 360.0);
                break;
            }
            default: break;
        }
    } else if (work->grip == Grip::Point) {
        Json& paths = work->paths();
        if (work->chosen >= 0 && work->chosen < static_cast<int>(paths.size())) {
            Json& points = paths[static_cast<std::size_t>(work->chosen)]["points"];
            if (work->point >= 0 && work->point < static_cast<int>(points.size())) points[static_cast<std::size_t>(work->point)] = Json::array({mm.x(), mm.y()});
        }
    }
    if (work->fields) work->fields();
    schedule(work);
    return true;
}

// Delete: the chosen pin (one stays), or the chosen path's point (a path of two goes whole).
void delete_chosen(const std::shared_ptr<Work>& work) {
    if (work->has_pins()) {
        Json& pins = work->pins();
        if (pins.size() <= 1 || work->chosen < 0 || work->chosen >= static_cast<int>(pins.size())) return;
        pins.erase(pins.begin() + work->chosen);
        work->chosen = std::max(0, work->chosen - 1);
    } else if (work->kind == "blur_path") {
        Json& paths = work->paths();
        if (work->chosen < 0 || work->chosen >= static_cast<int>(paths.size())) return;
        Json& points = paths[static_cast<std::size_t>(work->chosen)]["points"];
        if (points.size() > 2 && work->point >= 0 && work->point < static_cast<int>(points.size())) {
            points.erase(points.begin() + work->point);
            work->point = std::min(work->point, static_cast<int>(points.size()) - 1);
        } else {
            paths.erase(paths.begin() + work->chosen);
            work->chosen = static_cast<int>(paths.size()) - 1;
            work->point = -1;
        }
    }
    if (work->fields) work->fields();
    schedule(work);
}

// --- the panel (ぼかしツール) ----------------------------------------------------------------------------------------------------

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, int decimals, double step) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setKeyboardTracking(false);
    return box;
}

// A field of the chosen pin's (or the settings' own, `own`) setting `key`, its value shown ×`k`.
void bind(const std::shared_ptr<Work>& work, QFormLayout* form, const QString& label, QDoubleSpinBox* box, const char* key, double fallback,
          bool own, double k, std::vector<std::function<void()>>& shows) {
    form->addRow(label, box);
    const std::string field = key;
    const auto target = [work, own]() -> Json* {
        if (own) return &work->params;
        if (work->kind == "blur_path") {
            Json& paths = work->paths();
            return work->chosen >= 0 && work->chosen < static_cast<int>(paths.size()) ? &paths[static_cast<std::size_t>(work->chosen)] : nullptr;
        }
        Json& pins = work->pins();
        return work->chosen >= 0 && work->chosen < static_cast<int>(pins.size()) ? &pins[static_cast<std::size_t>(work->chosen)] : nullptr;
    };
    shows.push_back([box, target, field, fallback, k] {
        const Json* at = target();
        const QSignalBlocker quiet(box);
        box->setEnabled(at != nullptr);
        box->setValue(at != nullptr ? num(*at, field.c_str(), fallback) * k : fallback * k);
    });
    QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [work, target, field, k](double value) {
        if (Json* at = target()) {
            (*at)[field] = value / k;
            schedule(work);
        }
    });
}

void make_panel(const std::shared_ptr<Work>& work) {
    auto* panel = new QDialog(work->window, Qt::Tool);
    panel->setObjectName(QStringLiteral("blur_gallery_panel"));
    panel->setProperty("kind", QString::fromStdString(work->kind));
    panel->setWindowTitle(QStringLiteral("ぼかしツール — %1").arg(filter_forms::title(work->kind)));
    panel->setAttribute(Qt::WA_DeleteOnClose);
    work->panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    std::vector<std::function<void()>> shows;
    const std::string& kind = work->kind;
    if (kind == "blur_field" || kind == "blur_iris") {
        bind(work, form, QStringLiteral("ぼかし（px）"), spin(panel, "gallery_blur", 0, 500, 0, 1), "blur", 15, false, 1, shows);
    }
    if (kind == "blur_iris") {
        bind(work, form, QStringLiteral("丸み（%）"), spin(panel, "gallery_roundness", 0, 100, 0, 5), "roundness", 0, false, 1, shows);
        bind(work, form, QStringLiteral("シャープな範囲（%）"), spin(panel, "gallery_feather", 0, 99, 0, 5), "feather", 0.5, false, 100, shows);
    }
    if (kind == "blur_iris" || kind == "blur_spin") {
        bind(work, form, QStringLiteral("角度（°）"), spin(panel, "gallery_angle", -360, 360, 0, 5), "angle", 0, false, 1, shows);
    }
    if (kind == "blur_spin") {
        bind(work, form, QStringLiteral("ぼかしの角度（°）"), spin(panel, "gallery_blur_angle", 0, 360, 0, 1), "blur_angle", 15, false, 1, shows);
    }
    if (kind == "blur_tilt_shift") {
        bind(work, form, QStringLiteral("ぼかし（px）"), spin(panel, "gallery_blur", 0, 500, 0, 1), "blur", 15, true, 1, shows);
        bind(work, form, QStringLiteral("角度（°）"), spin(panel, "gallery_angle", -360, 360, 0, 5), "angle", 0, true, 1, shows);
        bind(work, form, QStringLiteral("シャープな範囲（%）"), spin(panel, "gallery_focus", 0, 200, 1, 1), "focus", 0.1, true, 100, shows);
        bind(work, form, QStringLiteral("移行の範囲（%）"), spin(panel, "gallery_transition", 0.1, 200, 1, 1), "transition", 0.15, true, 100, shows);
    }
    if (kind == "blur_path") {
        bind(work, form, QStringLiteral("速度（px）"), spin(panel, "gallery_speed", 0, 500, 0, 1), "speed", 50, false, 1, shows);
        bind(work, form, QStringLiteral("先細り（%）"), spin(panel, "gallery_taper", 0, 100, 0, 5), "taper", 0, false, 1, shows);
        auto* add = new QPushButton(QStringLiteral("新しいパス"), panel);
        add->setObjectName(QStringLiteral("gallery_new_path"));
        QObject::connect(add, &QPushButton::clicked, panel, [work] {
            Json& paths = work->paths();
            if (paths.size() >= core::kBlurPathMostPaths) return;
            const double y = work->box[1] + work->h() * (0.3 + 0.1 * static_cast<double>(paths.size() % 5));
            paths.push_back(Json{{"points", Json::array({Json::array({work->box[0] + 0.3 * work->w(), y}), Json::array({work->box[0] + 0.7 * work->w(), y})})},
                                 {"speed", 50},
                                 {"taper", 0}});
            work->chosen = static_cast<int>(paths.size()) - 1;
            work->point = 1;
            if (work->fields) work->fields();
            schedule(work);
        });
        layout->addWidget(add);
    }
    auto* remove = new QPushButton(work->kind == "blur_path" ? QStringLiteral("点を削除") : QStringLiteral("ピンを削除"), panel);
    remove->setObjectName(QStringLiteral("gallery_delete"));
    QObject::connect(remove, &QPushButton::clicked, panel, [work] { delete_chosen(work); });
    if (work->kind != "blur_tilt_shift") layout->addWidget(remove);
    auto* hint = new QLabel(work->kind == "blur_path"
                                ? QStringLiteral("ページをクリックすると、選んだパスに点を足します。点はドラッグで動かします。Enter で確定、Esc でやめます。")
                            : work->kind == "blur_tilt_shift"
                                ? QStringLiteral("中心・線の取っ手・回転の取っ手をドラッグします。Enter で確定、Esc でやめます。")
                                : QStringLiteral("ページをクリックするとピンを足し、ピンと取っ手はドラッグで動かします。Delete で選んだピンを消します。Enter で確定、Esc でやめます。"),
                            panel);
    hint->setWordWrap(true);
    hint->setProperty("role", "hint");
    layout->addWidget(hint);
    auto* check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"), panel);
    check->setObjectName(QStringLiteral("filter_preview"));
    check->setChecked(true);
    QObject::connect(check, &QCheckBox::toggled, panel, [work](bool on) {
        work->shown = on;
        schedule(work);
    });
    layout->addWidget(check);
    auto* busy = new QLabel(QStringLiteral("プレビューを作っています…"), panel);
    busy->setObjectName(QStringLiteral("filter_preview_busy"));
    busy->setProperty("role", "hint");
    busy->setVisible(false);
    layout->addWidget(busy);
    auto* poll = new QTimer(panel);
    poll->setInterval(100);
    QObject::connect(poll, &QTimer::timeout, panel, [work, busy] {
        busy->setVisible(work->previewing && work->previewing());
        // (the book changed under the work — another window, an agent's change —, or its page left: the work let go)
        if (work->window == nullptr || !work->window->still(work->asked)) finish(work, false);
    });
    poll->start();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, panel, [work] { finish(work, true); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, panel, [work] { finish(work, false); });
    QObject::connect(panel, &QDialog::finished, panel, [work](int) { finish(work, false); });  // (closed: let go)
    layout->addWidget(buttons);
    work->fields = [shows] {
        for (const auto& show : shows) show();
    };
    work->fields();
    work->timer = new QTimer(panel);
    work->timer->setSingleShot(true);
    work->timer->setInterval(250);
    QObject::connect(work->timer, &QTimer::timeout, panel, [work] { show_now(work); });
    // (beside the window's right edge, over the canvas)
    if (work->window != nullptr) {
        const QRect frame = work->window->geometry();
        panel->adjustSize();
        panel->move(frame.right() - panel->width() - 40, frame.top() + 120);
    }
    panel->show();
}

}  // namespace

void open(const FilterWindowCall& call) {
    MainWindow* window = call.window;
    if (window == nullptr || call.page == nullptr || call.layer == nullptr || window->canvas() == nullptr) return;
    if (auto before = current().lock()) finish(before, false);  // (one work at a time)
    auto work = std::make_shared<Work>();
    work->window = window;
    work->kind = call.item != nullptr ? call.item->kind : std::string();
    if (work->kind.empty()) {  // (the layer panel's smart filter: its kind from its settings' filter)
        const Json* filters = call.layer->smart && call.layer->smart->is_object() ? core::get(*call.layer->smart, "filters") : nullptr;
        if (filters != nullptr && filters->is_array() && call.filter >= 0 && call.filter < static_cast<int>(filters->size()))
            work->kind = core::py_str(core::py_get((*filters)[static_cast<std::size_t>(call.filter)], "kind", Json("")));
    }
    if (core::filter_info(work->kind) == nullptr) return;
    work->send = call.send;
    work->preview = call.preview;
    work->previewing = call.previewing;
    work->asked = window->asking();
    // the base box: a smart object's page; a layer's selection's box, else the page
    const double pw = call.page->spec.width_mm.value(), ph = call.page->spec.height_mm.value();
    work->box = {0, 0, pw, ph};
    if (call.layer->kind != core::LayerKind::Smart && window->selection_area()) {
        const auto b = window->canvas()->selection_box();
        if (b[2] > b[0] && b[3] > b[1]) work->box = b;
    }
    try {
        work->params = core::normalize_filter(work->kind, call.now.is_object() ? call.now : Json::object(), call.page);
    } catch (const std::exception&) {
        work->params = core::normalize_filter(work->kind, Json::object(), call.page);
    }
    if (work->kind == "blur_path" && !work->params.contains("paths")) {  // (upstream's path across the middle, on the page)
        const double y = work->box[1] + 0.5 * work->h();
        work->params["paths"] = Json::array({Json{{"points", Json::array({Json::array({work->box[0] + 0.2 * work->w(), y}), Json::array({work->box[0] + 0.8 * work->w(), y})})},
                                                  {"speed", 50},
                                                  {"taper", 0}}});
    }
    current() = work;
    auto hook = std::make_shared<PageCanvas::CanvasWork>();
    const std::weak_ptr<Work> weak = work;
    hook->paint = [weak](QPainter& painter, const std::function<QPointF(QPointF)>& at, double per_mm) {
        if (auto w = weak.lock()) paint(*w, painter, at, per_mm);
    };
    hook->press = [weak, window](QPointF mm, Qt::KeyboardModifiers) {
        auto w = weak.lock();
        if (!w) return false;
        return press(w, mm, window->canvas() != nullptr ? window->canvas()->scale() : 1.0);
    };
    hook->move = [weak](QPointF mm, Qt::KeyboardModifiers) {
        auto w = weak.lock();
        return w ? drag(w, mm) : false;
    };
    hook->release = [weak](QPointF, Qt::KeyboardModifiers) {
        auto w = weak.lock();
        if (!w) return false;
        w->grip = Grip::None;
        return true;
    };
    hook->key = [weak](QKeyEvent* event) {
        auto w = weak.lock();
        if (!w) return false;
        switch (event->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter: finish(w, true); return true;
            case Qt::Key_Escape: finish(w, false); return true;
            case Qt::Key_Delete:
            case Qt::Key_Backspace: delete_chosen(w); return true;
            default: return false;
        }
    };
    // (the work holds itself through the canvas's hook and its panel until it is let go)
    hook->paint = [work, paint_one = hook->paint](QPainter& painter, const std::function<QPointF(QPointF)>& at, double per_mm) {
        paint_one(painter, at, per_mm);
    };
    window->canvas()->set_work(hook);
    window->canvas()->setFocus();
    make_panel(work);
    window->flash(QStringLiteral("%1: ページの上でピンを置きます。Enter で確定、Esc でやめます").arg(filter_forms::title(work->kind)), 5000);
    schedule(work);
}

bool working() {
    const auto work = current().lock();
    return work != nullptr && !work->done;
}

Json settings() {
    const auto work = current().lock();
    return work != nullptr ? work->params : Json();
}

}  // namespace genko::app::blur_gallery
