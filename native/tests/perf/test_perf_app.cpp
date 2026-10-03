// The app's internal timings (shown, not judged; label perf, not in the usual CI). ACCEPTANCE.md §6: these are the
// times from an input event as Qt receives it to the app's own steps — the live line drawn, the screen update asked
// for, the paint done — not a pen's physical latency, and not on the reference machine.
//
// The F1 book (docs/cpp-migration/FIXTURES.md: B4, 32 pages of 1500 ink lines, as tests/test_h1.py::_thick makes it)
// opened in the main window, on Xvfb (the X server in memory: this program runs itself again under xvfb-run; the
// offscreen platform when there is none), and a recorded five-minute series played into it in real time: lines of the
// standard G pen (200 samples a second, pressure rising and falling), 元に戻す after every tenth line, the next page
// after every fiftieth, the autosave running as it does. Each event carries its due time, so a busy event loop counts in
// the delays. From GENKO_PERF_LOG: input → live line drawn → update asked for → paint done (p50/p95/p99, mean, max, n),
// a line committed and an undo until shown, a page switch until its first picture and its fine one, the saves'
// durations, and the event loop's stalls during saves. Run it in a release build for numbers worth reading
// (GENKO_PERF_MINUTES shortens the series).

#include <QtTest>

#include <QAction>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include "app/canvas.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/perf.hpp"
#include "app/session.hpp"
#include "app/theme.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "storage/lock.hpp"
#include "storage/transaction.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
namespace app = genko::app;

namespace {

// tests/test_h1.py::_thick: B4, `pages` pages of `lines` ink lines of 20 points, pressure 0.7, random.Random(1)
genko::core::Document f1_book(int pages = 32, int lines = 1500) {
    genko::core::PyRandom rng(1);
    genko::core::Document doc = genko::core::new_episode("厚い本", genko::core::Num(1), pages, genko::core::PageSpec::b4_comic());
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        genko::core::Page& page = doc.edit_page(p);
        for (auto& layer : page.layers) {
            if (layer.role != genko::core::LayerRole::Ink) continue;
            std::vector<genko::core::StrokePtr> items;
            for (int n = 0; n < lines; ++n) {
                const double x = genko::core::py_round(rng.uniform(20, 230), 3);
                const double y = genko::core::py_round(rng.uniform(20, 340), 3);
                auto stroke = std::make_shared<genko::core::Stroke>();
                stroke->id = genko::core::new_id();
                for (int i = 0; i < 20; ++i) {
                    stroke->points.push_back({genko::core::py_round(x + i * 1.2, 3), genko::core::py_round(y + rng.uniform(-2, 2), 3)});
                }
                stroke->pressure.assign(20, 0.7);
                items.push_back(stroke);
            }
            layer.strokes = genko::core::make_strokes(std::move(items));
        }
    }
    return doc;
}

struct Event {
    double due_ms;
    std::string kind;  // press | move | release | action
    double x = 0, y = 0, pressure = 0;
    QString action;
};

// The recorded series: G pen lines (1 s each at 200 Hz, 0.3 s apart), an undo after every tenth line, the next page
// after every fiftieth (then 2 s for its fine picture).
std::vector<Event> pen_series(double minutes) {
    genko::core::PyRandom rng(7);
    std::vector<Event> events;
    double t = 500;
    const double end = minutes * 60000;
    int line = 0;
    while (t < end) {
        const double x0 = rng.uniform(40, 180);
        const double y0 = rng.uniform(40, 320);
        const double length = rng.uniform(30, 70);
        const double wave = rng.uniform(2, 8);
        const int samples = 200;
        for (int i = 0; i < samples; ++i) {
            const double s = static_cast<double>(i) / (samples - 1);
            events.push_back(Event{t + i * 5.0, i == 0 ? "press" : "move", x0 + length * s, y0 + wave * std::sin(s * 6.283),
                                   0.2 + 0.75 * std::sin(s * 3.1416), {}});
        }
        t += samples * 5.0;
        events.push_back(Event{t, "release", x0 + length, y0, 0.0, {}});
        t += 300;
        ++line;
        if (line % 10 == 0) {
            events.push_back(Event{t, "action", 0, 0, 0, QStringLiteral("act_undo")});
            t += 300;
        }
        if (line % 50 == 0) {
            events.push_back(Event{t, "action", 0, 0, 0, QStringLiteral("act_next")});
            t += 2000;
        }
    }
    return events;
}

// Plays the series in real time; each pen event stamped with its due time on the perf log's clock.
class Player : public QObject {
public:
    Player(app::MainWindow* window, std::vector<Event> events) : window_(window), events_(std::move(events)) {}
    void start() {
        origin_ = static_cast<double>(app::perf::now_ns()) / 1e6;
        tick();
    }
    bool finished() const { return next_ >= events_.size(); }

private:
    double now() const { return static_cast<double>(app::perf::now_ns()) / 1e6 - origin_; }
    void tick() {
        while (next_ < events_.size() && events_[next_].due_ms <= now()) fire(events_[next_++]);
        if (finished()) return;
        QTimer::singleShot(std::max(0, static_cast<int>(events_[next_].due_ms - now())), Qt::PreciseTimer, this, [this] { tick(); });
    }
    void fire(const Event& e) {
        if (e.kind == "action") {
            window_->action(e.action)->trigger();
            return;
        }
        app::inject::PenInput p;
        p.mm = QPointF(e.x, e.y);
        p.pressure = e.pressure;
        const auto phase = e.kind == "press" ? app::inject::Phase::Press : e.kind == "release" ? app::inject::Phase::Release : app::inject::Phase::Move;
        app::inject::tablet(window_->canvas(), phase, p, false, static_cast<quint64>(origin_ + e.due_ms));
    }

    app::MainWindow* window_;
    std::vector<Event> events_;
    std::size_t next_ = 0;
    double origin_ = 0;
};

struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    // ACCEPTANCE.md §4: the ceil(p × n)-th of the sorted n (1-based)
    double pct(double p) const {
        if (v.empty()) return NAN;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        const auto k = static_cast<std::size_t>(std::ceil(p * static_cast<double>(s.size())));
        return s[std::max<std::size_t>(1, k) - 1];
    }
    double mean() const {
        double sum = 0;
        for (double x : v) sum += x;
        return v.empty() ? NAN : sum / static_cast<double>(v.size());
    }
    double max() const { return v.empty() ? NAN : *std::max_element(v.begin(), v.end()); }
    Json json() const {
        return Json::object({{"n", v.size()}, {"p50", pct(0.5)}, {"p95", pct(0.95)}, {"p99", pct(0.99)}, {"mean", mean()}, {"max", max()}});
    }
    QString line(const char* what) const {
        return QStringLiteral("%1: n=%2 p50=%3 p95=%4 p99=%5 mean=%6 max=%7 (ms)")
            .arg(QString::fromUtf8(what))
            .arg(v.size())
            .arg(pct(0.5), 0, 'f', 2)
            .arg(pct(0.95), 0, 'f', 2)
            .arg(pct(0.99), 0, 'f', 2)
            .arg(mean(), 0, 'f', 2)
            .arg(max(), 0, 'f', 2);
    }
};

double ms(std::int64_t ns) { return static_cast<double>(ns) / 1e6; }

bool wait_until(const std::function<bool()>& done, int ms_limit) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms_limit) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace

class TestPerfApp : public QObject {
    Q_OBJECT

private slots:
    void fiveMinutesOfPenOnF1() {
        const QString log = qEnvironmentVariable("GENKO_PERF_LOG");
        QVERIFY2(!log.isEmpty() && app::perf::enabled(), "GENKO_PERF_LOG is set by main()");
        QTemporaryDir dir;
        const fs::path book = genko::core::path_from_utf8(dir.filePath("f1.genko").toStdString());
        {
            QElapsedTimer made;
            made.start();
            const genko::core::Document doc = f1_book();
            genko::storage::ProjectLock lock(book, "genko");
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = "genko";
            request.ops = Json::array();
            genko::storage::Saver(lock).save(doc, request);
            qInfo("F1 written (32 pages × 1500 lines) in %lld ms", static_cast<long long>(made.elapsed()));
        }
        const double minutes = qEnvironmentVariableIsSet("GENKO_PERF_MINUTES") ? qEnvironmentVariable("GENKO_PERF_MINUTES").toDouble() : 5.0;
        QElapsedTimer opening;
        opening.start();
        auto window = std::make_unique<app::MainWindow>(app::Session::open(book));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QVERIFY(window->canvas()->wait_rendered(300000));
        qInfo("opened and drawn in %lld ms (window %d×%d, canvas %d×%d)", static_cast<long long>(opening.elapsed()), window->width(), window->height(),
              window->canvas()->width(), window->canvas()->height());
        window->choose_tool(QStringLiteral("pen"));
        Player player(window.get(), pen_series(minutes));
        player.start();
        QVERIFY(wait_until([&] { return player.finished(); }, static_cast<int>(minutes * 60000 + 300000)));
        QVERIFY(wait_until([&] { return window->session().status().kind == app::SaveKind::Saved; }, 120000));
        window->close();
        window.reset();
        app::perf::flush();

        // --- the log ---------------------------------------------------------------------------------------------
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::vector<Json> events;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty()) events.push_back(Json::parse(line.toStdString()));
        }
        std::stable_sort(events.begin(), events.end(), [](const Json& a, const Json& b) { return a["t"].get<std::int64_t>() < b["t"].get<std::int64_t>(); });
        std::vector<std::int64_t> paints;
        for (const Json& e : events) {
            if (e["ev"] == "paint_done") paints.push_back(e["t"].get<std::int64_t>());
        }
        const auto next_paint = [&](std::int64_t after) -> std::optional<std::int64_t> {
            const auto it = std::lower_bound(paints.begin(), paints.end(), after);
            if (it == paints.end()) return std::nullopt;
            return *it;
        };
        std::map<std::uint64_t, std::int64_t> due;  // seq → the input's due time (ns)
        Stats to_live, to_update, to_paint, queue, commit, undo, page_first, page_fine, saves, stalls_saving, stalls_all, applied;
        std::optional<std::int64_t> switching, save_from, undo_at;
        std::vector<std::pair<std::int64_t, std::int64_t>> saving;
        std::int64_t ticks = 0;
        for (const Json& e : events) {
            const std::string ev = e["ev"].get<std::string>();
            const std::int64_t t = e["t"].get<std::int64_t>();
            if (ev == "input" && e["kind"] == "tablet") {
                const std::int64_t at = e["qt_ms"].get<std::int64_t>() * 1'000'000;
                due[e["seq"].get<std::uint64_t>()] = at;
                queue.add(ms(t - at));
            } else if (ev == "live_drawn" && due.count(e["seq"].get<std::uint64_t>()) != 0) {
                to_live.add(ms(t - due[e["seq"].get<std::uint64_t>()]));
            } else if (ev == "update_requested" && due.count(e["seq"].get<std::uint64_t>()) != 0) {
                const std::int64_t at = due[e["seq"].get<std::uint64_t>()];
                to_update.add(ms(t - at));
                if (const auto p = next_paint(t)) to_paint.add(ms(*p - at));
            } else if (ev == "applied") {
                applied.add(e["ms"].get<double>());  // (the change applied in memory, on the GUI thread)
            } else if (ev == "stroke_committed") {
                if (const auto p = next_paint(t)) commit.add(ms(*p - t));
            } else if (ev == "undo") {
                undo_at = t;
            } else if (ev == "undo_shown_model" && undo_at) {
                if (const auto p = next_paint(t)) undo.add(ms(*p - *undo_at));
                undo_at.reset();
            } else if (ev == "page_switch") {
                switching = t;
            } else if (ev == "page_first_shown" && switching) {
                page_first.add(ms(t - *switching));
            } else if (ev == "page_settled" && switching) {
                page_fine.add(ms(t - *switching));
                switching.reset();
            } else if (ev == "save_start") {
                save_from = t;
            } else if (ev == "save_end" && save_from) {
                saves.add(ms(t - *save_from));
                saving.emplace_back(*save_from, t);
                save_from.reset();
            } else if (ev == "loop_ticks") {
                ticks = std::max(ticks, e["ticks"].get<std::int64_t>());
            }
        }
        std::int64_t saving_ns = 0;
        for (const auto& [from, to] : saving) saving_ns += to - from;
        for (const Json& e : events) {
            if (e["ev"] != "loop_stall") continue;
            const std::int64_t t = e["t"].get<std::int64_t>();
            const double late = e["ms"].get<double>();
            stalls_all.add(late);
            // (a stall is logged when it ends: one that began during a save ends at most its length after it)
            if (std::any_of(saving.begin(), saving.end(), [t, late](const auto& s) {
                    return t >= s.first && t <= s.second + static_cast<std::int64_t>(late * 1e6) + 5'000'000;
                })) {
                stalls_saving.add(late);
            }
        }
        // the event loop's lateness during saves over all its 5 ms ticks there (a tick not recorded was late by ≤ 4 ms)
        const double ticks_saving = ms(saving_ns) / 5.0;
        const double share_late = ticks_saving > 0 ? static_cast<double>(stalls_saving.v.size()) / ticks_saving : 0.0;
        const QString p99_saving = share_late < 0.01 ? QStringLiteral("≤ 4 ms (fewer than 1 % of its ticks late by more)")
                                                     : QStringLiteral("%1 ms").arg(stalls_saving.pct(1.0 - 0.01 / share_late), 0, 'f', 2);
        const QString platform = QGuiApplication::platformName();
        qInfo("platform: %s; series: %.1f min of G pen input; %zu events in the log", qPrintable(platform), minutes, events.size());
        qInfo("%s", qPrintable(to_live.line("input → live line drawn")));
        qInfo("%s", qPrintable(to_update.line("input → update asked for")));
        qInfo("%s", qPrintable(to_paint.line("input → paint done")));
        qInfo("%s", qPrintable(queue.line("input waiting in the event loop")));
        qInfo("%s", qPrintable(applied.line("change applied in memory (CommandBus, GUI thread)")));
        qInfo("%s", qPrintable(commit.line("line committed → shown (paint)")));
        qInfo("%s", qPrintable(undo.line("undo → shown (paint)")));
        qInfo("%s", qPrintable(page_first.line("page switch → first picture")));
        qInfo("%s", qPrintable(page_fine.line("page switch → fine picture")));
        qInfo("%s", qPrintable(saves.line("save (autosave) duration")));
        qInfo("%s", qPrintable(stalls_saving.line("event loop stalls > 4 ms during saves")));
        qInfo("event loop during saves (%.1f s in all): p99 lateness %s; stalls over 250 ms: %d", ms(saving_ns) / 1000, qPrintable(p99_saving),
              static_cast<int>(std::count_if(stalls_saving.v.begin(), stalls_saving.v.end(), [](double x) { return x > 250; })));
        qInfo("%s; %lld ticks of 5 ms in all", qPrintable(stalls_all.line("event loop stalls > 4 ms, whole run")), static_cast<long long>(ticks));
        const Json summary = Json::object({{"platform", platform.toStdString()},
                                           {"minutes", minutes},
                                           {"input_to_live", to_live.json()},
                                           {"input_to_update", to_update.json()},
                                           {"input_to_paint", to_paint.json()},
                                           {"input_queue", queue.json()},
                                           {"applied", applied.json()},
                                           {"commit_shown", commit.json()},
                                           {"undo_shown", undo.json()},
                                           {"page_first", page_first.json()},
                                           {"page_fine", page_fine.json()},
                                           {"saves", saves.json()},
                                           {"stalls_during_saves", stalls_saving.json()},
                                           {"stalls_all", stalls_all.json()},
                                           {"saving_seconds", ms(saving_ns) / 1000},
                                           {"p99_lateness_during_saves", p99_saving.toStdString()}});
        const QString out = qEnvironmentVariableIsSet("GENKO_PERF_SUMMARY") ? qEnvironmentVariable("GENKO_PERF_SUMMARY")
                                                                            : QDir::current().filePath(QStringLiteral("perf_app_summary.json"));
        QFile summary_file(out);
        if (summary_file.open(QIODevice::WriteOnly)) summary_file.write(QByteArray::fromStdString(summary.dump(2)));
        QVERIFY(!to_live.v.empty());
        QVERIFY(!commit.v.empty());
    }
};

int main(int argc, char** argv) {
    // (the series runs five minutes: longer than QtTest's own limit for one test function)
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "3500000");
    // Under Xvfb: this program again, under xvfb-run, on the X platform (when xvfb-run is there and this is not that run)
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
    QTemporaryDir logs;
    if (!qEnvironmentVariableIsSet("GENKO_PERF_LOG")) qputenv("GENKO_PERF_LOG", logs.filePath(QStringLiteral("perf.jsonl")).toUtf8());
    if (!qEnvironmentVariableIsSet("GENKO_CONFIG_DIR")) qputenv("GENKO_CONFIG_DIR", logs.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc, argv);
    app::icons::init_resources();
    app::theme::apply(&application);
    TestPerfApp test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_perf_app.moc"
