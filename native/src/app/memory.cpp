#include "app/memory.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QTimer>

#if defined(__GLIBC__)
#include <malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

namespace genko::app::memory {

namespace {

constexpr int kIdleMs = 10000;

// (an application-wide filter: any input starts the wait again; after it, the room goes back once)
class IdleWatch : public QObject {
public:
    IdleWatch() : QObject(QCoreApplication::instance()) {
        timer_.setSingleShot(true);
        timer_.setInterval(kIdleMs);
        connect(&timer_, &QTimer::timeout, this, [] { give_back(); });
        QCoreApplication::instance()->installEventFilter(this);
        timer_.start();
    }

    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseMove:
            case QEvent::TabletPress:
            case QEvent::TabletMove:
            case QEvent::TabletRelease:
            case QEvent::KeyPress:
            case QEvent::Wheel:
            case QEvent::TouchBegin:
            case QEvent::InputMethod:
                timer_.start();
                break;
            default:
                break;
        }
        return false;
    }

private:
    QTimer timer_;
};

}  // namespace

void tune() {
    static bool tuned = false;
    if (tuned) return;
    tuned = true;
#if defined(__GLIBC__)
    // (PERF-G: without these, the heap's free room grew some 2 MiB a minute in hours of line work, the memory in use
    // staying the same — what the drawing threads freed kept in their own arenas, in pieces the system cannot take
    // back. Set once threads have made their arenas, the limit came too late: at the start)
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);
#endif
}

void watch_idle() {
    static bool watching = false;
    if (watching || QCoreApplication::instance() == nullptr) return;
    watching = true;
    new IdleWatch;  // (owned by the application)
}

void give_back() {
#if defined(__GLIBC__)
    malloc_trim(0);
#elif defined(_WIN32)
    _heapmin();
#endif
}

}  // namespace genko::app::memory
