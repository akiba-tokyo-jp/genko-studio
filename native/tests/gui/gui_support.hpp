#pragma once

// Helpers for the app's window tests (label gui, on the offscreen platform): books on disk, a config folder of the
// test's own (settings, recent books, recovery points and caches never touch the person's), answers for the questions
// the app asks, and waiting while the event loop runs.

#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/ask.hpp"
#include "app/session.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "storage/fault.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace gui_test {

namespace fs = std::filesystem;
using genko::core::Json;
using namespace std::chrono_literals;

inline fs::path path_of(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }
inline QString qpath(const fs::path& path) { return QString::fromStdString(genko::core::path_to_utf8(path)); }

// A config folder for this test process (GENKO_CONFIG_DIR): set before the app reads any setting.
inline const QTemporaryDir& config_folder() {
    static QTemporaryDir dir;
    static const bool set = [] {
        qputenv("GENKO_CONFIG_DIR", dir.path().toUtf8());
        qputenv("GENKO_USER", "tester");
        return true;
    }();
    (void)set;
    return dir;
}

// Write a book (as `genko new` and the app's 新しい原稿 do): revision 1.
inline void write_book(const fs::path& dir, const genko::core::Document& doc) {
    genko::storage::ProjectLock lock(dir, "genko");
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = "genko";
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
}

inline genko::core::Document new_doc(int pages = 3, const std::string& title = "試し") {
    return genko::core::new_episode(title, genko::core::Num(1), pages, genko::core::PageSpec::a4_mono());
}

inline void make_book(const fs::path& dir, int pages = 3) { write_book(dir, new_doc(pages)); }

inline const genko::core::Layer* ink_of(const genko::core::Page& page) {
    for (const auto& layer : page.layers) {
        if (layer.role == genko::core::LayerRole::Ink) return &layer;
    }
    return nullptr;
}

inline std::size_t ink_strokes(const genko::core::Document& doc, std::size_t page = 0) {
    const genko::core::Layer* ink = ink_of(doc.page(page));
    return ink == nullptr ? 0 : ink->stroke_count();
}

inline genko::core::Document read_book(const fs::path& dir) { return genko::storage::load_document(dir).document; }
inline std::int64_t disk_revision(const fs::path& dir) { return genko::storage::read_disk_state(dir).revision; }

// A session that saves soon (50 ms after a change, at most 400 ms later) and keeps its recovery points in `recovery`.
inline genko::app::Session::Options quick(const fs::path& recovery) {
    genko::app::Session::Options options;
    options.actor = "human:tester";
    options.idle = 50ms;
    options.longest = 400ms;
    options.lock_wait = 3000ms;
    options.recovery_root = recovery;
    return options;
}

inline bool wait_for(const std::function<bool()>& done, int ms = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

// Fault injection in this process while it lives (the build must have it: GENKO_FAULT_INJECTION).
struct Fault {
    explicit Fault(const char* spec) { genko::storage::fault::set_for_testing(spec); }
    ~Fault() { genko::storage::fault::set_for_testing(""); }
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;
};

// Answers to the app's questions while it lives (each unset one: "no" / cancelled), with a record of what was asked.
struct Answers {
    std::shared_ptr<genko::app::ask::Responder> responder = std::make_shared<genko::app::ask::Responder>();
    QStringList asked;

    Answers() {
        auto* self = this;
        responder->question = [self](const QString& title, const QString& text) {
            self->asked << title + QStringLiteral(": ") + text;
            return false;
        };
        responder->warning = [self](const QString& title, const QString& text) { self->asked << title + QStringLiteral(": ") + text; };
        responder->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(); };
        responder->get_int = [](const QString&, const QString&, int, int, int) { return std::optional<int>(); };
        responder->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(); };
        responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(); };
        responder->existing_dir = [](const QString&, const QString&) { return QString(); };
        responder->save_path = [](const QString&, const QString&) { return QString(); };
        responder->exec = [self](QDialog* dialog) {
            self->asked << QString::fromLatin1(dialog->metaObject()->className());
            return 0;
        };
        genko::app::ask::set_responder(responder);
    }
    ~Answers() { genko::app::ask::set_responder(nullptr); }
    Answers(const Answers&) = delete;
    Answers& operator=(const Answers&) = delete;
};

}  // namespace gui_test
