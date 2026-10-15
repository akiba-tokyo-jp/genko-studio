// The page drawn by the graphics card (Python's GpuPageCanvas, stage 2 of the look): the same drawing, but scaling,
// turning and compositing the page's picture happen on the card (smooth zoom and rotation on a large page). Python made
// the canvas class itself a QOpenGLWidget; here a QOpenGLWidget lies under the canvas's other children (the launcher,
// the line editor, the notes) and paints the canvas through paint_frame, while the canvas keeps the events, the
// pointer and the focus (the surface lets the mouse and the pen through).

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>

#include "app/canvas.hpp"
#include "app/config.hpp"

namespace genko::app {

namespace {

class GpuSurface : public QOpenGLWidget {
public:
    explicit GpuSurface(PageCanvas* canvas) : QOpenGLWidget(canvas), canvas_(canvas) {
        setObjectName(QStringLiteral("gpuCanvas"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);  // (the mouse, the pen and the wheel go to the canvas)
        setFocusPolicy(Qt::NoFocus);
        QSurfaceFormat form = format();
        form.setSamples(4);  // (smooth edges on the handles and guides)
        setFormat(form);
        // After a resize (maximizing, restoring, dragging the window's edge) or the screen turning, some drivers hand
        // the widget a new context, and what was sent to the card may come back black (Intel HD 5500, Python's
        // _gl_resized): drawn again now and once more when the resizing has stopped.
        settle_.setSingleShot(true);
        settle_.setInterval(250);
        connect(&settle_, &QTimer::timeout, this, [this] { update(); });
    }

protected:
    void paintGL() override {
        QPainter painter(this);
        canvas_->paint_frame(painter);
    }

    void resizeGL(int, int) override {
        update();
        if (!watching_screen_) {
            if (QWindow* window = canvas_->window()->windowHandle()) {
                watching_screen_ = true;
                connect(window, &QWindow::screenChanged, this, [this](QScreen*) { settle_.start(); });
                if (QScreen* screen = window->screen()) {
                    connect(screen, &QScreen::orientationChanged, this, [this] { settle_.start(); });
                    connect(screen, &QScreen::geometryChanged, this, [this] { settle_.start(); });
                }
            }
        }
        settle_.start();
    }

private:
    PageCanvas* canvas_;
    QTimer settle_;
    bool watching_screen_ = false;
};

// (the canvas's size followed by the surface)
class FollowSize : public QObject {
public:
    FollowSize(PageCanvas* canvas, QWidget* surface) : QObject(surface), surface_(surface) { canvas->installEventFilter(this); }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) surface_->setGeometry(QRect(QPoint(0, 0), static_cast<QResizeEvent*>(event)->size()));
        (void)watched;
        return false;
    }

private:
    QWidget* surface_;
};

}  // namespace

void PageCanvas::use_gpu(bool on) {
    if (on == (gpu_surface_ != nullptr)) return;
    if (!on) {
        delete gpu_surface_;
        gpu_surface_ = nullptr;
        QWidget::update();
        return;
    }
    auto* surface = new GpuSurface(this);
    new FollowSize(this, surface);
    surface->setGeometry(rect());
    surface->lower();  // (under the launcher, the line editor and the notes)
    surface->setCursor(cursor());
    gpu_surface_ = surface;
    if (isVisible()) surface->show();
}

void PageCanvas::update() {
    QWidget::update();
    if (gpu_surface_ != nullptr) gpu_surface_->update();
}

void PageCanvas::update(const QRect& area) {
    QWidget::update(area);
    if (gpu_surface_ != nullptr) gpu_surface_->update();  // (the card draws the whole canvas again: cheap there)
}

void PageCanvas::update(const QRegion& area) {
    QWidget::update(area);
    if (gpu_surface_ != nullptr) gpu_surface_->update();
}

bool software_renderer(const QString& name) {
    const QString lower = name.toLower();
    for (const char* word : {"llvmpipe", "softpipe", "software", "swrast", "basic render", "gdi generic"}) {
        if (lower.contains(QLatin1String(word))) return true;
    }
    return false;
}

bool gpu_available() {
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (app == nullptr) return false;
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal") || platform == QLatin1String("vnc")) return false;
    const QString chosen = settings()->value(QStringLiteral("ui/gpu_canvas"), QStringLiteral("false")).toString().toLower();
    if (chosen != QLatin1String("1") && chosen != QLatin1String("true") && chosen != QLatin1String("yes")) {
        return false;  // (off unless chosen: some drivers give a blank white page, e.g. older Intel graphics on Windows)
    }
    QOpenGLContext context;
    if (!context.create()) return false;
    QOffscreenSurface surface;
    surface.create();
    if (!context.makeCurrent(&surface)) return false;
    const auto* renderer = reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER));
    const QString name = renderer != nullptr ? QString::fromLatin1(renderer) : QString();
    context.doneCurrent();
    return !software_renderer(name);
}

}  // namespace genko::app
