#include "app/session_file.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QTimer>

#include <cstdio>
#include <string>

#include "app/colours.hpp"
#include "app/doc_area.hpp"
#include "app/documents.hpp"
#include "app/main_window.hpp"
#include "core/paths.hpp"
#include "core/session_file.hpp"
#include "core/version.hpp"

namespace genko::app::session_file {

namespace {

using core::Json;

std::string hex(const std::array<int, 3>& rgb) {
    char out[8];
    std::snprintf(out, sizeof out, "#%02x%02x%02x", rgb[0] & 0xff, rgb[1] & 0xff, rgb[2] & 0xff);
    return out;
}

QString file() { return QString::fromStdString(core::path_to_utf8(core::session_file::path())); }

Json last;          // (what was written last, without its time)
bool started = false;

}  // namespace

Json now() {
    Json windows = Json::array();
    MainWindow* front = qobject_cast<MainWindow*>(QApplication::activeWindow());
    const auto all = documents::windows();
    if (front == nullptr && !all.empty()) front = all.front();
    for (MainWindow* window : all) {
        Json tabs = Json::array();
        const auto& docs = window->documents();
        for (std::size_t i = 0; i < docs.size(); ++i) {
            const Document& d = docs[i];
            if (!d.session) continue;
            const bool shown = static_cast<int>(i) == window->current_document();
            Json tab{{"index", static_cast<std::int64_t>(i)},
                     {"title", d.title().toStdString()},
                     {"saved", !d.session->unsaved()},
                     {"revision", d.session->snapshot() ? d.session->snapshot()->revision : 0},
                     {"page", (shown ? window->page_index() : d.page_index) + 1}};
            if (d.session->path() && !d.picture) tab["path"] = core::path_to_utf8(*d.session->path());
            if (d.contents) tab["contents"] = d.contents_title.toStdString();
            tabs.push_back(std::move(tab));
        }
        windows.push_back(Json{{"active", window == front},
                               {"arrange", (window->doc_area() != nullptr ? window->doc_area()->layout() : QStringLiteral("tabs")).toStdString()},
                               {"front", window->current_document()},
                               {"tabs", std::move(tabs)}});
    }
    Json out{{"v", 1}, {"pid", static_cast<std::int64_t>(QCoreApplication::applicationPid())}, {"app_version", std::string(genko::version())},
             {"windows", std::move(windows)}};
    if (front != nullptr && front->colours() != nullptr) {
        out["foreground"] = hex(front->colours()->rgb());
        out["background"] = hex(front->colours()->sub_rgb());
    }
    return out;
}

bool write_now() {
    Json made = now();
    if (made == last) return false;
    Json written = made;
    written["updated_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    QSaveFile out(file());
    if (!out.open(QIODevice::WriteOnly)) return false;
    const std::string text = written.dump(1) + "\n";
    out.write(text.data(), static_cast<qint64>(text.size()));
    if (!out.commit()) return false;  // (the folder cannot be written: looked at again in a second)
    last = std::move(made);
    return true;
}

void start() {
    if (started || QCoreApplication::instance() == nullptr) return;
    started = true;
    auto* timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, [] { (void)write_now(); });
    timer->start();
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
        // (the app going: its file taken away — unless another app wrote it since)
        const Json kept = core::session_file::read();
        if (kept.value("pid", Json(0)) == Json(static_cast<std::int64_t>(QCoreApplication::applicationPid()))) QFile::remove(file());
        last = Json();
    });
}

}  // namespace genko::app::session_file
