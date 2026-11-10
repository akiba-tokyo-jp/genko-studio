#include "app/perf.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QInputEvent>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace genko::app::perf {

namespace {

struct Log {
    std::mutex mutex;
    std::mutex file_mutex;
    std::string buffer;
    std::unique_ptr<QFile> file;
    std::atomic<std::uint64_t> seq{0};
};

Log* log() {
    static Log* instance = [] {
        const QString path = qEnvironmentVariable("GENKO_PERF_LOG");
        if (path.isEmpty()) return static_cast<Log*>(nullptr);
        auto* made = new Log;  // (kept to the end of the process: written by a timer and at exit)
        made->file = std::make_unique<QFile>(path);
        if (!made->file->open(QIODevice::Append | QIODevice::WriteOnly)) {
            delete made;
            return static_cast<Log*>(nullptr);
        }
        std::atexit([] { flush(); });
        return made;
    }();
    return instance;
}

void write(core::Json record) {
    Log* l = log();
    if (l == nullptr) return;
    const std::string line = record.dump(-1, ' ', false, core::Json::error_handler_t::replace) + "\n";
    std::lock_guard lock(l->mutex);
    l->buffer += line;
}

}  // namespace

bool enabled() { return log() != nullptr; }

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void event(const char* name, std::initializer_list<std::pair<const char*, core::Json>> fields) {
    if (!enabled()) return;
    core::Json record = core::Json::object();
    record["ev"] = name;
    record["t"] = now_ns();
    for (const auto& [key, value] : fields) record[key] = value;
    write(std::move(record));
}

void event(const char* name, core::Json fields) {
    if (!enabled()) return;
    core::Json record = core::Json::object();
    record["ev"] = name;
    record["t"] = now_ns();
    if (fields.is_object()) {
        for (auto& [key, value] : fields.items()) record[key] = value;
    }
    write(std::move(record));
}

std::uint64_t input(const QInputEvent* e, const char* kind) {
    Log* l = log();
    if (l == nullptr) return 0;
    const std::uint64_t seq = ++l->seq;
    event("input", {{"kind", kind}, {"seq", seq}, {"qt_ms", static_cast<std::int64_t>(e->timestamp())}});
    return seq;
}

void watch_loop(QObject* parent) {
    if (!enabled()) return;
    auto* timer = new QTimer(parent);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(5);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto ticks = std::make_shared<std::int64_t>(0);
    QObject::connect(timer, &QTimer::timeout, parent, [clock, ticks] {
        const qint64 gap = clock->nsecsElapsed();
        clock->restart();
        ++*ticks;
        const double late_ms = (gap - 5'000'000) / 1e6;
        if (late_ms > 4.0) event("loop_stall", {{"ms", late_ms}});
        if (*ticks % 200 == 0) event("loop_ticks", {{"ticks", *ticks}});
    });
    timer->start();
    auto* flusher = new QTimer(parent);
    flusher->setInterval(1000);
    QObject::connect(flusher, &QTimer::timeout, parent, [] { flush(); });
    flusher->start();
}

void flush() {
    Log* l = log();
    if (l == nullptr) return;
    std::string out;
    {
        std::lock_guard lock(l->mutex);
        out.swap(l->buffer);
    }
    if (out.empty()) return;
    std::lock_guard lock(l->file_mutex);
    l->file->write(out.data(), static_cast<qint64>(out.size()));
    l->file->flush();
}

}  // namespace genko::app::perf
