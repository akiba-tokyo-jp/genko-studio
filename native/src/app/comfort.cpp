// Comfort over long sessions (Python's genko/app/comfort.py).

#include "app/comfort.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRegion>
#include <QToolBar>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <numbers>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/studio_widgets.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

bool truthy(const QVariant& value) {
    const QString text = value.toString().toLower();
    return text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("yes");
}

}  // namespace

// --- the page alone (Tab) ------------------------------------------------------------------------------------------

CanvasOnly::CanvasOnly(MainWindow* window) : QObject(window), window_(window) {
    timer_.setInterval(120);
    connect(&timer_, &QTimer::timeout, this, &CanvasOnly::watch);
}

std::vector<QWidget*> CanvasOnly::pieces() const {
    std::vector<QWidget*> out;
    for (QDockWidget* dock : window_->findChildren<QDockWidget*>()) {
        if (dock->parent() == window_) out.push_back(dock);
    }
    for (QToolBar* bar : window_->findChildren<QToolBar*>()) {
        if (bar->parent() == window_) out.push_back(bar);
    }
    return out;
}

bool CanvasOnly::toggle() {
    if (on_) {
        leave();
    } else {
        enter();
    }
    return on_;
}

void CanvasOnly::enter() {
    saved_.clear();
    for (QWidget* widget : pieces()) saved_.emplace_back(widget, widget->isVisible());
    for (const auto& [widget, shown] : saved_) {
        if (widget) widget->hide();
    }
    on_ = true;
    peek_.clear();
    timer_.start();
    window_->flash(QStringLiteral("原稿だけの表示（Tab で戻す）。ウィンドウの左右の端にカーソルを寄せると、その側のパネルが出ます"), 3500);
}

void CanvasOnly::leave() {
    timer_.stop();
    for (const auto& [widget, shown] : saved_) {
        if (widget) widget->setVisible(shown);
    }
    saved_.clear();
    on_ = false;
    peek_.clear();
}

QString CanvasOnly::side_of(QWidget* widget) const {
    Qt::DockWidgetArea dock_area = Qt::NoDockWidgetArea;
    Qt::ToolBarArea bar_area = Qt::NoToolBarArea;
    if (auto* dock = qobject_cast<QDockWidget*>(widget)) dock_area = window_->dockWidgetArea(dock);
    if (auto* bar = qobject_cast<QToolBar*>(widget)) bar_area = window_->toolBarArea(bar);
    if (dock_area == Qt::LeftDockWidgetArea || bar_area == Qt::LeftToolBarArea) return QStringLiteral("left");
    if (dock_area == Qt::RightDockWidgetArea || bar_area == Qt::RightToolBarArea) return QStringLiteral("right");
    return QStringLiteral("top");
}

void CanvasOnly::watch() {
    if (!on_ || !window_->isVisible()) return;
    watch_at(window_->mapFromGlobal(QCursor::pos()));
}

void CanvasOnly::watch_at(const QPoint& pos) {
    if (!on_) return;
    const int width = window_->width();
    const bool inside = 0 <= pos.y() && pos.y() <= window_->height();
    if (inside && 0 <= pos.x() && pos.x() <= kEdgePx) {
        show_side(QStringLiteral("left"));
    } else if (inside && width - kEdgePx <= pos.x() && pos.x() <= width) {
        show_side(QStringLiteral("right"));
    } else if (!peek_.isEmpty()) {
        std::vector<QWidget*> shown;
        for (const auto& [widget, was] : saved_) {
            if (widget && was && side_of(widget) == peek_ && widget->isVisible()) shown.push_back(widget);
        }
        bool away = true;
        if (!shown.empty()) {
            if (peek_ == QLatin1String("left")) {
                int edge = shown.front()->geometry().right();
                for (QWidget* w : shown) edge = std::max(edge, w->geometry().right());
                away = pos.x() > edge + 40;
            } else {
                int edge = shown.front()->geometry().left();
                for (QWidget* w : shown) edge = std::min(edge, w->geometry().left());
                away = pos.x() < edge - 40;
            }
        }
        if (away) {
            for (const auto& [widget, was] : saved_) {
                if (widget && side_of(widget) == peek_) widget->hide();
            }
            peek_.clear();
        }
    }
}

void CanvasOnly::show_side(const QString& side) {
    if (peek_ == side) return;
    peek_ = side;
    for (const auto& [widget, was] : saved_) {
        if (widget && was && side_of(widget) == side) widget->show();
    }
}

// --- the round menu at the pen ---------------------------------------------------------------------------------------

bool radial_tool(const QString& tool) {
    static const QStringList tools{QStringLiteral("pen"),   QStringLiteral("eraser"),   QStringLiteral("fill"),  QStringLiteral("lassofill"),
                                   QStringLiteral("blend"), QStringLiteral("gradient"), QStringLiteral("shape"), QStringLiteral("picker"),
                                   QStringLiteral("effect"), QStringLiteral("stamp")};
    return tools.contains(tool);
}

bool radial_on() { return truthy(settings()->value(QStringLiteral("ui/radial"), QStringLiteral("true"))); }

std::vector<QAction*> radial_actions(MainWindow* window) {
    std::vector<QAction*> out;
    for (const char* name : {"act_pen", "act_eraser", "act_fill", "act_picker", "act_select", "act_undo", "act_redo", "act_fit"}) {
        if (QAction* act = window->action(QString::fromLatin1(name))) out.push_back(act);
    }
    return out;
}

RadialMenu::RadialMenu(QWidget* window, const std::vector<QAction*>& actions, const QPoint& centre)
    : QWidget(window, Qt::Popup | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    for (QAction* act : actions) {
        if (act != nullptr && actions_.size() < 8) actions_.push_back(act);
    }
    const int size = 2 * (kRadius + kButton + 6);
    resize(size, size);
    move(centre.x() - size / 2, centre.y() - size / 2);
    opened_s_ = monotonic_seconds();
    setProperty("glass_wanted", true);
    setMask(QRegion(0, 0, size, size, QRegion::Ellipse));  // (a round window: the glass stays round)
    if (!reduce_motion()) {  // it opens from the pen outward
        grow_ = 0.0;
        opening_ = new QVariantAnimation(this);
        opening_->setDuration(140);
        opening_->setStartValue(0.0);
        opening_->setEndValue(1.0);
        opening_->setEasingCurve(QEasingCurve::OutCubic);
        connect(opening_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            grow_ = value.toDouble();
            update();
        });
        opening_->start();
    }
}

QPointF RadialMenu::centre() const { return QPointF(width() / 2.0, height() / 2.0); }

QPointF RadialMenu::spot(int i) const {
    const double angle = -std::numbers::pi / 2 + 2 * std::numbers::pi * i / std::max<std::size_t>(1, actions_.size());
    const QPointF c = centre();
    return QPointF(c.x() + kRadius * std::cos(angle), c.y() + kRadius * std::sin(angle));
}

int RadialMenu::at(const QPointF& pos) const {
    const QPointF c = centre();
    const double dx = pos.x() - c.x();
    const double dy = pos.y() - c.y();
    if (std::hypot(dx, dy) < 22 || actions_.empty()) return -1;
    const double angle = std::fmod(std::atan2(dy, dx) + std::numbers::pi / 2 + 2 * std::numbers::pi, 2 * std::numbers::pi);
    const double step = 2 * std::numbers::pi / static_cast<double>(actions_.size());
    return static_cast<int>(std::floor((angle + step / 2) / step)) % static_cast<int>(actions_.size());
}

void RadialMenu::paintEvent(QPaintEvent*) {
    const theme::Tokens t = theme::tokens();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c = centre();
    if (grow_ < 1.0) {
        p.setOpacity(grow_);
        p.translate(c);
        p.scale(0.82 + 0.18 * grow_, 0.82 + 0.18 * grow_);
        p.translate(-c);
    }
    QPainterPath ring;
    ring.addEllipse(c, kRadius + kButton + 2, kRadius + kButton + 2);
    QColor back(t.panel);
    back.setAlpha(property("glass").toBool() ? 150 : 240);  // (over the system's glass, a thinner face)
    p.setPen(QPen(QColor(t.border), 1));
    p.setBrush(back);
    p.drawPath(ring);
    // the centre: where the pen was; letting go here closes the menu
    p.setBrush(QColor(t.window));
    p.drawEllipse(c, 14, 14);
    p.setPen(QPen(QColor(t.muted), 1.4));
    for (const auto& [dx, dy] : {std::pair{-4.0, -4.0}, {-4.0, 4.0}}) p.drawLine(QPointF(c.x() + dx, c.y() + dy), QPointF(c.x() - dx, c.y() - dy));
    QFont small = font();
    small.setPointSizeF(std::max(9.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
        QAction* action = actions_[static_cast<std::size_t>(i)];
        const QPointF s = spot(i);
        const bool chosen = i == hover_;
        p.setPen(QPen(QColor(chosen ? t.accent : t.border), chosen ? 1.5 : 1));
        p.setBrush(QColor(chosen ? t.selected : t.raised));
        p.drawEllipse(s, kButton, kButton);
        const QIcon icon = action->icon();
        if (!icon.isNull()) icon.paint(&p, static_cast<int>(s.x() - 11), static_cast<int>(s.y() - 17), 22, 22);
        p.setPen(QColor(t.text));
        QString label = action->iconText().isEmpty() ? action->text() : action->iconText();
        label = label.remove(QStringLiteral("…")).split(QStringLiteral("（")).front();
        p.drawText(QRectF(s.x() - 40, s.y() + 5, 80, 16), Qt::AlignCenter, label.left(6));
    }
}

void RadialMenu::mouseMoveEvent(QMouseEvent* event) {
    const int found = at(event->position());
    if (found != hover_) {
        hover_ = found;
        update();
    }
}

void RadialMenu::mouseReleaseEvent(QMouseEvent* event) {
    // (a quick right-click opens it and leaves it open; letting go over a command after holding chooses it)
    if (event->button() == Qt::RightButton && monotonic_seconds() - opened_s_ < 0.3) return;
    choose(at(event->position()));
}

void RadialMenu::mousePressEvent(QMouseEvent* event) {
    if (!rect().contains(event->position().toPoint())) {
        close();
        return;
    }
    if (event->button() == Qt::LeftButton) choose(at(event->position()));
}

void RadialMenu::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) close();
}

void RadialMenu::choose(int index) {
    const std::vector<QAction*> actions = actions_;
    close();
    if (0 <= index && index < static_cast<int>(actions.size())) actions[static_cast<std::size_t>(index)]->trigger();
}

// --- resting the eyes ------------------------------------------------------------------------------------------------

namespace {

// One watcher for the whole application: when a person last pressed, drew or typed.
class InputWatch : public QObject {
public:
    double last = monotonic_seconds();

    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::KeyPress:
            case QEvent::TabletPress:
            case QEvent::Wheel:
            case QEvent::TabletMove:
                last = monotonic_seconds();
                break;
            default:
                break;
        }
        return false;
    }
};

InputWatch* input_watch = nullptr;

}  // namespace

double monotonic_seconds() {
    static QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return static_cast<double>(clock.nsecsElapsed()) / 1e9;
}

double last_input() {
    auto* app = QCoreApplication::instance();
    if (input_watch == nullptr && app != nullptr) {
        input_watch = new InputWatch;
        input_watch->setParent(app);
        app->installEventFilter(input_watch);
        QObject::connect(input_watch, &QObject::destroyed, [watch = input_watch] {
            if (input_watch == watch) input_watch = nullptr;  // (a newer watch made meanwhile is kept)
        });
    }
    return input_watch != nullptr ? input_watch->last : monotonic_seconds();
}

void stop_input() {
    auto* app = QCoreApplication::instance();
    if (input_watch != nullptr && app != nullptr) {
        app->removeEventFilter(input_watch);
        InputWatch* gone = input_watch;
        input_watch = nullptr;
        gone->deleteLater();
    }
}

bool input_watched() { return input_watch != nullptr; }

void touch_input() {
    last_input();
    if (input_watch != nullptr) input_watch->last = monotonic_seconds();
}

RestReminder::RestReminder(MainWindow* window) : QObject(window), window_(window) {
    if (minutes() > 0) last_input();
    tick_.setInterval(15000);
    connect(&tick_, &QTimer::timeout, this, &RestReminder::count);
    tick_.start();
}

int RestReminder::minutes() const {
    bool ok = false;
    const int value = settings()->value(QStringLiteral("ui/rest_minutes"), 0).toInt(&ok);
    return ok ? std::max(0, value) : 0;
}

void RestReminder::count() {
    if (window_->closed()) {
        tick_.stop();
        return;
    }
    if (minutes() == 0) {
        stop_input();  // (off: nothing watches the input)
        return;
    }
    const double idle = monotonic_seconds() - last_input();
    if (idle > kResetIdleS) {
        worked = 0.0;  // (a real pause already happened)
    } else if (idle < kIdleS) {
        worked += tick_.interval() / 1000.0;
    }
    if (worked >= minutes() * 60.0) {
        worked = 0.0;
        remind();
    }
}

void RestReminder::remind() {
    if (on_remind) {
        on_remind();
        return;
    }
    toast(window_, QStringLiteral("%1 分続けて作業しました。20 秒ほど遠くを見て、目を休めましょう").arg(minutes()), 12000);
}

// --- fewer moving things; the letters' size at once -------------------------------------------------------------------

bool system_reduces_motion() {
#ifdef _WIN32
    BOOL on = TRUE;
    if (SystemParametersInfoW(0x1042 /* SPI_GETCLIENTAREAANIMATION */, 0, &on, 0)) return !on;
#endif
    return false;  // (no answer from the system: move as usual)
}

void apply_motion(QApplication* app) {
    if (app == nullptr) app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr) return;
    const bool still = reduce_motion();
    for (const auto effect : {Qt::UI_AnimateMenu, Qt::UI_FadeMenu, Qt::UI_AnimateCombo, Qt::UI_AnimateTooltip, Qt::UI_FadeTooltip,
                              Qt::UI_AnimateToolBox}) {
        QApplication::setEffectEnabled(effect, !still);
    }
}

void apply_font(QApplication* app) {
    if (app == nullptr) app = qobject_cast<QApplication*>(QCoreApplication::instance());
    const int size = settings()->value(QStringLiteral("ui/font_pt"), 0).toInt();
    if (app == nullptr || size <= 0) return;
    QFont font = QApplication::font();
    if (font.pointSize() == size) return;
    font.setPointSize(size);
    QApplication::setFont(font);  // (every widget without a font of its own follows)
    theme::apply(app);           // (the style sheet is made again for the new size)
}

void fade_in(QWidget* widget, int ms, int rise) {
    if (widget == nullptr || reduce_motion() || widget->graphicsEffect() != nullptr) return;
    auto* fade = new QGraphicsOpacityEffect(widget);
    fade->setOpacity(0.0);
    widget->setGraphicsEffect(fade);
    auto* move = new QPropertyAnimation(fade, "opacity", fade);  // (owned by the effect: they go together)
    move->setDuration(ms);
    move->setStartValue(0.0);
    move->setEndValue(1.0);
    move->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QWidget> alive(widget);
    // (queued: the effect owns the animation, and is let go only after its signal has returned)
    QObject::connect(
        move, &QPropertyAnimation::finished, widget,
        [alive, fade] {
            if (alive && alive->graphicsEffect() == fade) alive->setGraphicsEffect(nullptr);
        },
        Qt::QueuedConnection);
    move->start();
    if (rise != 0) {
        const QPoint end = widget->pos();
        auto* slide = new QPropertyAnimation(widget, "pos", widget);
        slide->setDuration(ms + 20);
        slide->setStartValue(end + QPoint(0, rise));
        slide->setEndValue(end);
        slide->setEasingCurve(QEasingCurve::OutCubic);
        slide->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

QSize icon_size() { return QSize(24, 24); }

}  // namespace genko::app
