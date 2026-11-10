#pragma once

// Long work kept off the window's thread with a progress window and 中止 (PC5 I-h, its review's P2-4: the way send_filter
// waits, and I-g's made_book). The work runs on a worker thread, handed `stopped` (true once 中止 is pressed, or the wait
// is left early — the app quitting —); the window's own thread goes on drawing while an application-modal progress window
// holds the person's input. 中止 does not take the progress window down (its own cancel is taken off): it asks the work
// to stop and says so, and the window stays until the work is back. The progress window lives on the heap, watched (one
// owner: `parent`). The work must only read what it is given (copies, snapshots); render::segment's stop (StopScope) is
// set for it too, so the render work that asks it stops as well.
//
// The work's result; none when it was stopped (render::Cancelled, or 中止 pressed even if it had finished); the exception it
// threw, as if it had run here. The caller checks that its window is still there (QPointer) before it touches it.
//
// (PC6, the review's P2-4) While it runs, the outermost window over `parent` is busy: its close (its own, or Genko を終わる
// from another window — QApplication::closeAllWindows, inside this wait —) asks the work to stop and is left for when the
// work is back (close_after_stoppable: the window's closeEvent ignores it then), and the window is closed again then. A
// work that says how far it is in its own terms (files, frames) is given `told` (StoppableTold): its count and what it is
// on shown in place of the render work's own.

#include <QCoreApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPointer>
#include <QProgressDialog>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include "render/fx.hpp"
#include "render/page.hpp"
#include "render/segment/segment.hpp"

namespace genko::app {

// What a work says of itself, written on its thread and read on the window's: how far it is (0..1) and what it is on.
class StoppableTold {
public:
    void put(double done, const QString& now = QString()) {
        const std::lock_guard<std::mutex> hold(mutex_);
        done_ = done;
        if (!now.isEmpty()) now_ = now;
    }
    // (how far, below 0 when nothing was said; what it is on, emptied once read)
    std::pair<double, QString> take() {
        const std::lock_guard<std::mutex> hold(mutex_);
        return {done_, std::exchange(now_, QString())};
    }

private:
    std::mutex mutex_;
    double done_ = -1.0;
    QString now_;
};

namespace stoppable_detail {

struct Busy {
    std::vector<std::shared_ptr<std::stop_source>> stops;
    bool close_after = false;
};
// (the windows with work running, by the window: the window's own thread only)
inline std::map<const QWidget*, Busy>& busy() {
    static std::map<const QWidget*, Busy> windows;
    return windows;
}
inline QWidget* outermost(QWidget* widget) {
    while (widget != nullptr && widget->parentWidget() != nullptr) widget = widget->parentWidget();
    return widget;
}

// A work's place among its window's while it runs (whatever way it leaves): a window asked to close already stops it at
// once; the last one gone closes the window again when it was asked to close meanwhile.
class Registered {
public:
    Registered(QWidget* window, std::shared_ptr<std::stop_source> stop) : window_(window), alive_(window), stop_(std::move(stop)) {
        if (window_ == nullptr) return;
        Busy& here = busy()[window_];
        here.stops.push_back(stop_);
        if (here.close_after) stop_->request_stop();
    }
    ~Registered() {
        if (window_ == nullptr) return;
        auto& windows = busy();
        const auto found = windows.find(window_);
        if (found == windows.end()) return;
        auto& stops = found->second.stops;
        stops.erase(std::remove(stops.begin(), stops.end(), stop_), stops.end());
        if (!stops.empty()) return;
        const bool close_again = found->second.close_after;
        windows.erase(found);
        if (close_again && alive_) {  // (asked to close while it worked: closed now that it is back)
            QTimer::singleShot(0, alive_.data(), [w = alive_] {
                if (w) w->close();
            });
        }
    }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;

private:
    QWidget* window_;
    QPointer<QWidget> alive_;
    std::shared_ptr<std::stop_source> stop_;
};

}  // namespace stoppable_detail

// `window` has long work running (run_stoppable for it, or for a widget or a window over it): each is asked to stop and
// true is said — the caller leaves its close (closeEvent ignores it): the window is closed again once the work is back.
inline bool close_after_stoppable(QWidget* window) {
    auto& busy = stoppable_detail::busy();
    const auto found = busy.find(window);
    if (found == busy.end() || found->second.stops.empty()) return false;
    for (const auto& stop : found->second.stops) stop->request_stop();
    found->second.close_after = true;
    return true;
}

template <class Work>
auto run_stoppable(QWidget* parent, const QString& title, const QString& label, Work work, std::shared_ptr<StoppableTold> told = nullptr)
    -> std::optional<std::invoke_result_t<Work&, const std::function<bool()>&>> {
    using Result = std::invoke_result_t<Work&, const std::function<bool()>&>;
    auto stop = std::make_shared<std::stop_source>();
    const stoppable_detail::Registered registered(stoppable_detail::outermost(parent), stop);
    const QPointer<QProgressDialog> progress = new QProgressDialog(label, QStringLiteral("中止"), 0, 0, parent);
    progress->setObjectName(QStringLiteral("stoppable_progress"));
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(300);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    QObject::disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));  // (kept up while it stops)
    QObject::connect(progress, &QProgressDialog::canceled, progress, [stop, p = progress.data(), title] {
        stop->request_stop();
        p->setLabelText(QStringLiteral("%1を中止しています…").arg(title));
    });
    struct Done {
        std::optional<Result> result;
        std::exception_ptr error;
        bool cancelled = false;
    };
    // (PC5 I-g: how far the render work is, as render::fx::ProgressScope says it — the window busy until it says anything)
    auto fraction = std::make_shared<std::atomic<double>>(0.0);
    QTimer tick;
    tick.setInterval(100);
    QObject::connect(&tick, &QTimer::timeout, progress, [p = progress.data(), fraction, told, stop] {
        double f = fraction->load();
        if (told) {
            const auto [done, now] = told->take();
            if (!now.isEmpty() && !stop->stop_requested()) p->setLabelText(now);
            if (done >= 0.0) f = done;
        }
        if (f <= 0.0) return;
        if (p->maximum() != 1000) p->setRange(0, 1000);
        p->setValue(static_cast<int>(f * 1000));
    });
    tick.start();
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    auto done = std::make_shared<Done>();
    watcher.setFuture(QtConcurrent::run([work = std::move(work), token = stop->get_token(), done, fraction]() mutable {
        const render::segment::StopScope scope(token);
        const render::fx::ProgressScope progress_scope(*fraction);
        const std::function<bool()> stopped = [token] { return token.stop_requested(); };
        try {
            done->result.emplace(work(stopped));
        } catch (const render::Cancelled&) {
            done->cancelled = true;
        } catch (...) {
            done->error = std::current_exception();
        }
    }));
    if (!watcher.isFinished()) loop.exec();
    if (!watcher.isFinished()) stop->request_stop();  // (the loop left early — the app quitting —: stopped, then waited for)
    watcher.waitForFinished();
    tick.stop();
    if (progress) {
        progress->hide();
        progress->deleteLater();
    }
    if (done->error) std::rethrow_exception(done->error);
    if (done->cancelled || stop->stop_requested()) return std::nullopt;
    return std::move(done->result);
}

}  // namespace genko::app
