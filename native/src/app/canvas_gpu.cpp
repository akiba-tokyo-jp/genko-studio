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

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/config.hpp"

namespace genko::app {

namespace {

bool truthy_setting(const QString& key) {
    const QString chosen = settings()->value(key, QStringLiteral("false")).toString().toLower();
    return chosen == QLatin1String("1") || chosen == QLatin1String("true") || chosen == QLatin1String("yes");
}

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
    // Where the card's context cannot be made (no OpenGL on the platform, a broken driver), Qt makes a new one at each
    // resize and show after that and leaks the one before (QOpenGLWidgetPrivate::initialize): one try only.
    bool event(QEvent* event) override {
        const QEvent::Type type = event->type();
        if (gave_up() && (type == QEvent::Show || type == QEvent::WindowChangeInternal || type == QEvent::ScreenChangeInternal)) {
            return QWidget::event(event);
        }
        const bool handled = QOpenGLWidget::event(event);
        // (PC5 H-a: a context that could not be made — no OpenGL here, a broken driver —: the canvas draws by the processor
        // again, and says so; once, after this event)
        if (gave_up() && !told_) {
            told_ = true;
            QMetaObject::invokeMethod(canvas_, [canvas = canvas_] { canvas->gpu_failed(); }, Qt::QueuedConnection);
        }
        return handled;
    }

    void resizeEvent(QResizeEvent* event) override {
        if (!gave_up()) QOpenGLWidget::resizeEvent(event);
    }

    void paintGL() override {
        {
            QPainter painter(this);
            canvas_->paint_frame(painter);
        }
        canvas_->renderer().trim_textures();  // (PC5 H-a: the tiles on the card kept within GPU の記憶)
    }

    void resizeGL(int, int) override {
        // (review P3-2: the widget's own frame buffers — 4 samples of colour and of depth and stencil, and the one they
        // are resolved to — come out of GPU の記憶 too; the tiles keep at least a quarter of it)
        const double ratio = devicePixelRatioF();
        const auto pixels = static_cast<std::size_t>(std::ceil(width() * ratio)) * static_cast<std::size_t>(std::ceil(height() * ratio));
        const std::size_t budget = PageCanvas::gpu_budget_bytes();
        const std::size_t frames = pixels * 4 * (2 * static_cast<std::size_t>(std::max(1, format().samples())) + 1);
        canvas_->renderer().set_texture_budget(std::max(budget / 4, budget > frames ? budget - frames : std::size_t{0}));
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
    bool gave_up() const { return context() != nullptr && !context()->isValid(); }

    PageCanvas* canvas_;
    QTimer settle_;
    bool watching_screen_ = false;
    bool told_ = false;
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
        renderer_->set_texture_budget(std::nullopt);
        QWidget::update();
        return;
    }
    renderer_->set_texture_budget(gpu_budget_bytes());
    auto* surface = new GpuSurface(this);
    new FollowSize(this, surface);
    surface->setGeometry(rect());
    surface->lower();  // (under the launcher, the line editor and the notes)
    surface->setCursor(cursor());
    gpu_surface_ = surface;
    if (isVisible()) surface->show();
}

void PageCanvas::gpu_failed() {
    if (gpu_surface_ == nullptr) return;
    use_gpu(false);
    // (review P3-2: not asked again at every window — the choice turned off, said so)
    if (truthy_setting(QStringLiteral("ui/gpu_canvas"))) settings()->setValue(QStringLiteral("ui/gpu_canvas"), QStringLiteral("false"));
    emit gpuFailed(QStringLiteral("グラフィックボードで原稿を表示できないため、プロセッサーで表示します（環境設定 → パフォーマンス の"
                                  "「原稿の表示にグラフィックボードを使う」を切りました）"));
}

std::size_t PageCanvas::gpu_budget_bytes() {
    // (環境設定 → パフォーマンス: GPU の記憶の上限, MB; PC5 H-a)
    bool ok = false;
    const int mb = settings()->value(QStringLiteral("ui/gpu_memory_mb"), 512).toInt(&ok);
    return static_cast<std::size_t>(ok ? std::clamp(mb, 64, 1024) : 512) * 1024 * 1024;
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
