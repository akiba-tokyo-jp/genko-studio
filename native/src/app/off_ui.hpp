#pragma once

// Heavy work kept off the window's thread (PC4 F-b: コピーを保存…'s picture and bytes, 複製…'s merged picture, 新規… →
// 画像's pixels): the work runs on a worker thread while the window goes on drawing itself — the person's input held
// until it is done (the wait cursor shown), so nothing is chosen in the middle of it — and its result, or the exception
// it threw, comes back as if it had run here. The work must only read what it is given (copies, snapshots).

#include <QApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace genko::app {

template <class Work>
auto off_ui(Work work) -> std::invoke_result_t<Work&> {
    using Result = std::invoke_result_t<Work&>;
    std::optional<Result> result;
    std::exception_ptr error;
    QEventLoop loop;
    QFutureWatcher<void> watcher;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([&] {
        try {
            result.emplace(work());
        } catch (...) {
            error = std::current_exception();
        }
    }));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    if (!watcher.isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    watcher.waitForFinished();
    QApplication::restoreOverrideCursor();
    if (error) std::rethrow_exception(error);
    return std::move(*result);
}

}  // namespace genko::app
