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

#include <QCoreApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPointer>
#include <QProgressDialog>
#include <QString>
#include <QWidget>
#include <QtConcurrent>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "render/page.hpp"
#include "render/segment/segment.hpp"

namespace genko::app {

template <class Work>
auto run_stoppable(QWidget* parent, const QString& title, const QString& label, Work work)
    -> std::optional<std::invoke_result_t<Work&, const std::function<bool()>&>> {
    using Result = std::invoke_result_t<Work&, const std::function<bool()>&>;
    auto stop = std::make_shared<std::stop_source>();
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
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    auto done = std::make_shared<Done>();
    watcher.setFuture(QtConcurrent::run([work = std::move(work), token = stop->get_token(), done]() mutable {
        const render::segment::StopScope scope(token);
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
    if (progress) {
        progress->hide();
        progress->deleteLater();
    }
    if (done->error) std::rethrow_exception(done->error);
    if (done->cancelled || stop->stop_requested()) return std::nullopt;
    return std::move(done->result);
}

}  // namespace genko::app
