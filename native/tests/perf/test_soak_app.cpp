// PERF-G and M7's two hours of continuous work (ACCEPTANCE.md §4 PERF-G, PLAN.md M7; label perf, not in the usual
// runs). The F1 book (docs/cpp-migration/FIXTURES.md: B4, 32 pages of 1500 ink lines) opened in the main window on Xvfb
// (this program runs itself again under xvfb-run, as test_perf_app; the offscreen platform when there is none), and
// two hours of mixed work injected as a person does it, in real time: lines of the standard G pen (200 samples a second,
// pressure rising and falling), 元に戻す and やり直し of the line just drawn, the next page, zooming in and
// out and back to the whole page, a line of 台詞 now and then, the autosave running as it does.
//
// The first 20 minutes warm the caches up to their limits (every page visited). Then 100 minutes of the same fixed
// work in two-minute rounds: 90 s of work, the caches let go (the remembered layer pictures, masks and placed pictures,
// as a new process starts without them), 30 s at rest, then the process's private memory is read (Linux:
// Private_Clean + Private_Dirty of /proc/self/smaps_rollup).
//
// Judged: the most memory resident (VmHWM) within 8 GB; the private memory of the last 10 minutes on average within
// 128 MiB of the first 10 minutes'; its least-squares trend over the 100 minutes at most 1 MiB a minute; no line lost
// and none made twice (each page has the lines drawn on it, less those undone, plus those redone, and the 台詞 put on
// it); the book read back from the disk after the window has closed is whole and has the same lines and 台詞 as the
// window had. These times are of this machine (ACCEPTANCE.md §6), not the reference class.
//
// GENKO_SOAK_MINUTES (120), GENKO_SOAK_WARM (20), GENKO_SOAK_ROUND (120 s) and GENKO_SOAK_REST (30 s) shorten it for a
// trial (a shortened run is not the acceptance); GENKO_SOAK_SUMMARY: where the summary JSON goes.

#include <QtTest>

#include <QAction>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "app/canvas.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/memory.hpp"
#include "app/perf.hpp"
#include "app/session.hpp"
#include "app/theme.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "render/page.hpp"
#include "render/placed.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
namespace app = genko::app;
namespace core = genko::core;

namespace {

// tests/test_h1.py::_thick (as test_perf_app's): B4, 32 pages of 1500 ink lines of 20 points, pressure 0.7, random.Random(1)
core::Document f1_book(int pages = 32, int lines = 1500) {
    core::PyRandom rng(1);
    core::Document doc = core::new_episode("厚い本", core::Num(1), pages, core::PageSpec::b4_comic());
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        core::Page& page = doc.edit_page(p);
        for (auto& layer : page.layers) {
            if (layer.role != core::LayerRole::Ink) continue;
            std::vector<core::StrokePtr> items;
            for (int n = 0; n < lines; ++n) {
                const double x = core::py_round(rng.uniform(20, 230), 3);
                const double y = core::py_round(rng.uniform(20, 340), 3);
                auto stroke = std::make_shared<core::Stroke>();
                stroke->id = core::new_id();
                for (int i = 0; i < 20; ++i) stroke->points.push_back({core::py_round(x + i * 1.2, 3), core::py_round(y + rng.uniform(-2, 2), 3)});
                stroke->pressure.assign(20, 0.7);
                items.push_back(stroke);
            }
            layer.strokes = core::make_strokes(std::move(items));
        }
    }
    return doc;
}

// The process's memory now (kB): VmHWM (the most resident so far) from /proc/self/status, and the private memory
// (Private_Clean + Private_Dirty) from /proc/self/smaps_rollup. -1 where the system does not say.
long long status_kb(const char* key) {
    std::ifstream in("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) return std::stoll(line.substr(prefix.size()));
    }
    return -1;
}

long long private_kb() {
    std::ifstream in("/proc/self/smaps_rollup");
    std::string line;
    long long sum = -1;
    while (std::getline(in, line)) {
        for (const char* key : {"Private_Clean:", "Private_Dirty:"}) {
            if (line.rfind(key, 0) == 0) sum = std::max(0LL, sum) + std::stoll(line.substr(std::string(key).size()));
        }
    }
    return sum;
}

// The private memory (kB) by the kind of mapping it is in (/proc/self/smaps): the main heap, anonymous mappings (the
// other arenas, the large blocks, thread stacks without a name), stacks, files, and the rest. (Shown with each round,
// to tell where it grows.)
std::string private_by_kind() {
    std::ifstream in("/proc/self/smaps");
    std::string line, kind;
    std::map<std::string, long long> kb;
    long long anon_maps = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && std::isxdigit(static_cast<unsigned char>(line[0])) && line.find('-') != std::string::npos &&
            line.find(':') > line.find(' ')) {
            // (a mapping's first line: range perms offset dev inode [path])
            std::istringstream fields(line);
            std::string range, perms, offset, dev, inode, path;
            fields >> range >> perms >> offset >> dev >> inode;
            std::getline(fields, path);
            path.erase(0, path.find_first_not_of(' '));
            kind = path.empty() ? "anon" : path == "[heap]" ? "heap" : path.rfind("[stack", 0) == 0 ? "stack" : path[0] == '/' ? "files" : "other";
            if (kind == "anon") ++anon_maps;
            continue;
        }
        for (const char* key : {"Private_Clean:", "Private_Dirty:"}) {
            if (line.rfind(key, 0) == 0) kb[kind] += std::stoll(line.substr(std::string(key).size()));
        }
    }
    std::ostringstream out;
    out << "heap " << kb["heap"] / 1024 << ", anon " << kb["anon"] / 1024 << " (" << anon_maps << " maps), stack " << kb["stack"] / 1024
        << ", files " << kb["files"] / 1024 << ", other " << kb["other"] / 1024 << " MiB";
    return out.str();
}

// The C library's heap (glibc): what is in use, and what is free but still held (kB); -1 elsewhere. (Shown with each
// round, to tell the memory the program holds from the heap's own room.)
std::pair<long long, long long> heap_kb() {
#if defined(__GLIBC__)
    const struct mallinfo2 m = mallinfo2();
    return {static_cast<long long>((m.uordblks + m.hblkhd) / 1024), static_cast<long long>(m.fordblks / 1024)};
#else
    return {-1, -1};
#endif
}

struct Event {
    double due_ms;
    std::string kind;  // press | move | release | action | page | text
    double x = 0, y = 0, pressure = 0;
    QString action;
    int page = 0;
};

// The fixed work, a block at a time (its state goes on from block to block): G pen lines (1 s each at 200 Hz, 0.3 s
// apart); after every 20th line from the 10th 元に戻す, after every 20th 元に戻す and やり直し; after every 15th zoom in, after every 16th
// zoom out, after every 40th the whole page; after every 25th the next page (from the last, the first: every page
// visited in about 20 minutes); after every 60th a line of 台詞 on the page.
class Work {
public:
    explicit Work(int pages) : pages_(pages) {}
    std::vector<Event> block(double seconds) {
        std::vector<Event> events;
        double t = 300;
        const double end = seconds * 1000;
        while (t < end) {
            const double x0 = rng_.uniform(40, 180);
            const double y0 = rng_.uniform(40, 320);
            const double length = rng_.uniform(30, 70);
            const double wave = rng_.uniform(2, 8);
            const int samples = 200;
            for (int i = 0; i < samples; ++i) {
                const double s = static_cast<double>(i) / (samples - 1);
                events.push_back(Event{t + i * 5.0, i == 0 ? "press" : "move", x0 + length * s, y0 + wave * std::sin(s * 6.283),
                                       0.2 + 0.75 * std::sin(s * 3.1416), {}, 0});
            }
            t += samples * 5.0;
            events.push_back(Event{t, "release", x0 + length, y0, 0.0, {}, page_});
            t += 300;
            ++line_;
            const auto act = [&](const char* name, double after) {
                events.push_back(Event{t, "action", 0, 0, 0, QString::fromLatin1(name), page_});
                t += after;
            };
            if (line_ % 20 == 10) act("act_undo", 300);  // (the line just drawn taken back)
            if (line_ % 20 == 0) {                        // (taken back and put again)
                act("act_undo", 300);
                act("act_redo", 300);
            }
            if (line_ % 15 == 0) act("act_zoom_in", 500);
            if (line_ % 16 == 0) act("act_zoom_out", 500);
            if (line_ % 40 == 0) act("act_fit", 500);
            if (line_ % 60 == 0) {
                events.push_back(Event{t, "text", 0, 0, 0, {}, page_});
                t += 500;
            }
            if (line_ % 25 == 0) {
                page_ = (page_ + 1) % pages_;
                events.push_back(Event{t, "page", 0, 0, 0, {}, page_});
                t += 2000;  // (its fine picture)
            }
        }
        return events;
    }

private:
    core::PyRandom rng_{11};
    int pages_;
    int page_ = 0;
    long long line_ = 0;
};

// Plays a block in real time; each pen event stamped with its due time on the perf clock. Counts what it did per page.
class Player : public QObject {
public:
    explicit Player(app::MainWindow* window) : window_(window) {}
    void play(std::vector<Event> events) {
        events_ = std::move(events);
        next_ = 0;
        origin_ = static_cast<double>(app::perf::now_ns()) / 1e6;
        tick();
    }
    bool finished() const { return next_ >= events_.size(); }
    std::map<int, long long> lines, undone, redone, texts;  // per page row
    long long problems = 0;

private:
    double now() const { return static_cast<double>(app::perf::now_ns()) / 1e6 - origin_; }
    void tick() {
        while (next_ < events_.size() && events_[next_].due_ms <= now()) fire(events_[next_++]);
        if (finished()) return;
        QTimer::singleShot(std::max(0, static_cast<int>(events_[next_].due_ms - now())), Qt::PreciseTimer, this, [this] { tick(); });
    }
    void fire(const Event& e) {
        if (e.kind == "action") {
            QAction* act = window_->action(e.action);
            if (act == nullptr || !act->isEnabled()) {
                ++problems;
                qWarning("not there or not enabled: %s", qPrintable(e.action));
                return;
            }
            act->trigger();
            if (e.action == QLatin1String("act_undo")) ++undone[e.page];
            if (e.action == QLatin1String("act_redo")) ++redone[e.page];
            return;
        }
        if (e.kind == "page") {
            window_->select_page(e.page);
            return;
        }
        if (e.kind == "text") {
            const core::Page* page = window_->current_page();
            if (page == nullptr || !window_->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", page->index.json()}, {"text", "続けて描く"},
                                                                         {"x_mm", 40}, {"y_mm", 50}, {"w_mm", 20}, {"h_mm", 40}}}))) {
                ++problems;
                qWarning("a line of 台詞 not put on page row %d", e.page);
                return;
            }
            ++texts[e.page];
            return;
        }
        app::inject::PenInput p;
        p.mm = QPointF(e.x, e.y);
        p.pressure = e.pressure;
        const auto phase = e.kind == "press" ? app::inject::Phase::Press : e.kind == "release" ? app::inject::Phase::Release : app::inject::Phase::Move;
        app::inject::tablet(window_->canvas(), phase, p, false, static_cast<quint64>(origin_ + e.due_ms));
        if (e.kind == "release") ++lines[e.page];
    }

    app::MainWindow* window_;
    std::vector<Event> events_;
    std::size_t next_ = 0;
    double origin_ = 0;
};

bool wait_until(const std::function<bool()>& done, int ms_limit) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms_limit) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

double env_number(const char* name, double fallback) {
    bool ok = false;
    const double value = qEnvironmentVariable(name).toDouble(&ok);
    return ok && value > 0 ? value : fallback;
}

long long ink_lines(const core::Page& page) {
    long long n = 0;
    for (const core::Layer& layer : page.layers) {
        if (layer.role == core::LayerRole::Ink) n += static_cast<long long>(layer.stroke_count());
    }
    return n;
}

}  // namespace

class TestSoakApp : public QObject {
    Q_OBJECT

private slots:
    void twoHoursOfWorkOnF1() {
        const double minutes = env_number("GENKO_SOAK_MINUTES", 120);
        const double warm = std::min(minutes, env_number("GENKO_SOAK_WARM", 20));
        const double round_s = env_number("GENKO_SOAK_ROUND", 120);
        const double rest_s = std::min(round_s / 2, env_number("GENKO_SOAK_REST", 30));
        QTemporaryDir dir;
        const fs::path book = core::path_from_utf8(dir.filePath("f1.genko").toStdString());
        const core::Document start = f1_book();
        {
            genko::storage::ProjectLock lock(book, "genko");
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = "genko";
            request.ops = Json::array();
            genko::storage::Saver(lock).save(start, request);
        }
        app::Session::Options first_page_first;
        first_page_first.defer_pages = true;
        auto window = std::make_unique<app::MainWindow>(app::Session::from(app::Session::read(book, first_page_first), book, first_page_first));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QVERIFY(window->canvas()->wait_rendered(300000));
        QVERIFY(wait_until([&] { return !window->session().loading(); }, 300000));
        window->choose_tool(QStringLiteral("pen"));
        const int pages = static_cast<int>(window->book().pages.size());
        Work work(pages);
        Player player(window.get());
        QElapsedTimer clock;
        clock.start();
        const auto play = [&](double seconds) {
            player.play(work.block(seconds));
            return wait_until([&] { return player.finished(); }, static_cast<int>(seconds * 1000 + 600000));
        };
        // the warm-up: every page visited, the caches filled to their limits
        QVERIFY(play(warm * 60));
        qInfo("warmed up in %.1f min: VmHWM %lld MiB, private %lld MiB", clock.elapsed() / 60000.0, status_kb("VmHWM") / 1024, private_kb() / 1024);
        // the rounds: work, the caches let go, rest, the private memory read
        struct Sample {
            double minute;  // from the start of the rounds
            long long kb;
        };
        std::vector<Sample> samples;
        QElapsedTimer rounds;
        rounds.start();
        const double rounds_ms = (minutes - warm) * 60000;
        while (rounds.elapsed() + round_s * 1000 <= rounds_ms + 1000) {
            QVERIFY(play(round_s - rest_s));
            genko::render::clear_render_caches();
            genko::render::clear_placed_caches();
            QElapsedTimer resting;
            resting.start();
            while (resting.elapsed() < rest_s * 1000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            samples.push_back(Sample{rounds.elapsed() / 60000.0, private_kb()});
            const auto [in_use, free_kb] = heap_kb();
            qInfo("round %zu at %.1f min: private %lld MiB, VmHWM %lld MiB; heap in use %lld MiB, free in it %lld MiB; changes in memory %zu; "
                  "private by kind: %s",
                  samples.size(), samples.back().minute, samples.back().kb / 1024, status_kb("VmHWM") / 1024, in_use / 1024, free_kb / 1024,
                  window->session().changes_in_memory(), private_by_kind().c_str());
        }
        QVERIFY2(samples.size() >= 2, "too short for two rounds");
        QVERIFY2(samples.front().kb >= 0, "no /proc/self/smaps_rollup here");
        // saved, the window's book kept, the window closed and the book read back
        QVERIFY(wait_until([&] { return window->session().status().kind == app::SaveKind::Saved; }, 300000));
        const core::Document in_window = window->book();
        window->close();
        window.reset();
        const long long hwm_kb = status_kb("VmHWM");
        const core::Document read_back = genko::storage::load_document(book).document;

        // --- the judgement ---------------------------------------------------------------------------------------
        const double span = samples.back().minute - samples.front().minute;
        const double window_min = std::min(10.0, span / 2);
        double first = 0, last = 0;
        int nf = 0, nl = 0;
        for (const Sample& s : samples) {
            if (s.minute <= samples.front().minute + window_min) first += static_cast<double>(s.kb), ++nf;
            if (s.minute >= samples.back().minute - window_min) last += static_cast<double>(s.kb), ++nl;
        }
        first /= nf;
        last /= nl;
        double mx = 0, my = 0;
        for (const Sample& s : samples) mx += s.minute, my += static_cast<double>(s.kb);
        mx /= static_cast<double>(samples.size());
        my /= static_cast<double>(samples.size());
        double sxy = 0, sxx = 0;
        for (const Sample& s : samples) sxy += (s.minute - mx) * (static_cast<double>(s.kb) - my), sxx += (s.minute - mx) * (s.minute - mx);
        const double slope_mib_min = sxx > 0 ? sxy / sxx / 1024 : 0;
        const double drift_mib = (last - first) / 1024;
        long long drawn = 0, undone = 0, redone = 0, texts = 0;
        for (const auto& [row, n] : player.lines) drawn += n;
        for (const auto& [row, n] : player.undone) undone += n;
        for (const auto& [row, n] : player.redone) redone += n;
        for (const auto& [row, n] : player.texts) texts += n;
        int pages_wrong = 0;
        for (int row = 0; row < pages; ++row) {
            const long long expected = ink_lines(start.page(static_cast<std::size_t>(row))) + player.lines[row] - player.undone[row] + player.redone[row];
            const long long shown = ink_lines(in_window.page(static_cast<std::size_t>(row)));
            const long long kept = ink_lines(read_back.page(static_cast<std::size_t>(row)));
            const long long story = static_cast<long long>(read_back.story_for_page(read_back.page(static_cast<std::size_t>(row)).index).size());
            if (shown != expected || kept != shown || story != player.texts[row]) {
                ++pages_wrong;
                qWarning("page row %d: lines expected %lld, in the window %lld, read back %lld; 台詞 put %lld, read back %lld", row, expected, shown, kept,
                         player.texts[row], story);
            }
        }
        qInfo("platform %s; %.1f min in all (%.1f warming up, %zu rounds of %.0f s with %.0f s at rest)", qPrintable(QGuiApplication::platformName()),
              clock.elapsed() / 60000.0, warm, samples.size(), round_s, rest_s);
        qInfo("work: %lld lines, %lld undone, %lld redone, %lld lines of 台詞; problems %lld; pages with a difference %d", drawn, undone, redone, texts,
              player.problems, pages_wrong);
        qInfo("memory: VmHWM %lld MiB (≤ 8192); private first %.0f MiB, last %.0f MiB, drift %.1f MiB (≤ 128); trend %.3f MiB/min (≤ 1)",
              hwm_kb / 1024, first / 1024, last / 1024, drift_mib, slope_mib_min);
        Json series = Json::array();
        for (const Sample& s : samples) series.push_back(Json::array({s.minute, s.kb}));
        const Json summary = Json::object({{"platform", QGuiApplication::platformName().toStdString()},
                                           {"minutes", minutes},
                                           {"warm_minutes", warm},
                                           {"round_seconds", round_s},
                                           {"rest_seconds", rest_s},
                                           {"vm_hwm_kb", hwm_kb},
                                           {"private_kb", series},
                                           {"private_first_mib", first / 1024},
                                           {"private_last_mib", last / 1024},
                                           {"drift_mib", drift_mib},
                                           {"trend_mib_per_min", slope_mib_min},
                                           {"lines", drawn},
                                           {"undone", undone},
                                           {"redone", redone},
                                           {"texts", texts},
                                           {"problems", player.problems},
                                           {"pages_with_a_difference", pages_wrong}});
        const QString out = qEnvironmentVariableIsSet("GENKO_SOAK_SUMMARY") ? qEnvironmentVariable("GENKO_SOAK_SUMMARY")
                                                                           : QDir::current().filePath(QStringLiteral("soak_summary.json"));
        QFile summary_file(out);
        if (summary_file.open(QIODevice::WriteOnly)) summary_file.write(QByteArray::fromStdString(summary.dump(2)));
        QCOMPARE(player.problems, 0LL);
        QCOMPARE(pages_wrong, 0);
        QVERIFY2(hwm_kb <= 8LL * 1024 * 1024, "PERF-G: the most memory resident is over 8 GB");
        QVERIFY2(drift_mib <= 128.0, "PERF-G: the last 10 minutes' private memory is over 128 MiB above the first 10 minutes'");
        QVERIFY2(slope_mib_min <= 1.0, "PERF-G: the private memory grows more than 1 MiB a minute");
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "9000000");  // (two hours and more)
    // Under Xvfb: this program again, under xvfb-run, on the X platform (as test_perf_app)
    if (!qEnvironmentVariableIsSet("GENKO_PERF_CHILD")) {
        const QString xvfb = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
        if (!xvfb.isEmpty()) {
            QCoreApplication core(argc, argv);
            QProcess child;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("GENKO_PERF_CHILD"), QStringLiteral("1"));
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb"));
            child.setProcessEnvironment(env);
            child.setProcessChannelMode(QProcess::ForwardedChannels);
            QStringList args = {QStringLiteral("-a"), QStringLiteral("-s"), QStringLiteral("-screen 0 1920x1080x24"), QCoreApplication::applicationFilePath()};
            for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
            child.start(xvfb, args);
            if (!child.waitForFinished(-1)) return 3;
            return child.exitCode();
        }
    }
    app::memory::tune();  // (as `genko app` does at its start, before any thread)
    QTemporaryDir logs;
    // (the perf log stays off: two hours of it would be large; the timings are test_perf_app's)
    if (!qEnvironmentVariableIsSet("GENKO_CONFIG_DIR")) qputenv("GENKO_CONFIG_DIR", logs.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc, argv);
    app::icons::init_resources();
    app::theme::apply(&application);
    TestSoakApp test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_soak_app.moc"
