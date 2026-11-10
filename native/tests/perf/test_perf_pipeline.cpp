// PERF-F 制作API (ACCEPTANCE.md §4), judged against its pass values (label perf, not in the usual CI; run it in a Release
// build): F3's default offline making (FIXTURES.md: tests/test_m4_pipeline.py's 8 pages, the fixed synthetic pictures,
// no AI) on this build's `genko mcp` (stdio) through the MCP SDK's own client, driven by Python's FakeAgent, with the
// person's `genko studio` processes, as tools/migration/pipeline_harness.py runs it (test_contract_pipeline checks the
// same run against Python's). Each time is the caller's: from sending the call until its whole answer (a tool over
// JSON-RPC; a command, the process run).
//   - every call of the making, the export included and nothing left out: p95 ≤ 2 s;
//   - then, on the same book, each kind at least 100 times: information (status, inspect of a page and a panel,
//     candidates, next, tickets and a page's preview — render, 1024 px — in turn), an edit committed (apply_ops, a
//     page's note), a candidate adopted (adopt: the panel's good picture, which Genko enlarges and adopts), a page
//     finished (finish_page committed, after the person approves the page's art again as they must after an agent
//     adopts): each p95 ≤ 2 s;
//   - the 8-page proof (export_proof as a background job) 10 times, each into a folder of its own: from the call until
//     its PDF is whole (the job's result read, the file there with its 8 pages) ≤ 30 s each; the time the job was
//     taken (the reply) and the time it was done are kept apart.
// Shown: n, p50, p95, p99, mean and max (the ceil(p × n)-th of the sorted n, 1-based) for each, and the machine. It is
// this development machine, not the reference class (ACCEPTANCE.md §6): the pass values are judged here as the stand-in
// the user chose, not as the reference machine's result. Skipped without a Python that has the mcp extra
// ($GENKO_MCP_PYTHON), as test_contract_mcp is.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

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
    QString line(const char* what) const {
        return QStringLiteral("%1: n=%2 p50=%3 p95=%4 p99=%5 mean=%6 max=%7 (ms)")
            .arg(QString::fromUtf8(what))
            .arg(v.size())
            .arg(pct(0.5), 0, 'f', 1)
            .arg(pct(0.95), 0, 'f', 1)
            .arg(pct(0.99), 0, 'f', 1)
            .arg(mean(), 0, 'f', 1)
            .arg(max(), 0, 'f', 1);
    }
};

// The CPU's model and the memory, as /proc says them (Linux; elsewhere what Qt knows).
QString machine() {
    QString cpu = QSysInfo::currentCpuArchitecture();
    QFile info(QStringLiteral("/proc/cpuinfo"));
    if (info.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : info.readAll().split('\n')) {
            if (line.startsWith("model name")) {
                cpu = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).trimmed();
                break;
            }
        }
    }
    QString memory = QStringLiteral("?");
    QFile mem(QStringLiteral("/proc/meminfo"));
    if (mem.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : mem.readAll().split('\n')) {
            if (line.startsWith("MemTotal:")) {
                const double kb = line.mid(9).trimmed().split(' ').value(0).toDouble();
                memory = QString::number(kb / 1024 / 1024, 'f', 1) + QStringLiteral(" GiB");
                break;
            }
        }
    }
    return QStringLiteral("%1, %2 logical CPUs, RAM %3, %4").arg(cpu).arg(QThread::idealThreadCount()).arg(memory, QSysInfo::prettyProductName());
}

}  // namespace

class PerfPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (genko::test::mcp_python().isEmpty()) {
            QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON (a venv with uv.lock's mcp, pydantic, Pillow and numpy)");
        }
        QVERIFY(scratch_.isValid());
    }

    void productionApi() {
        const QString dir = scratch_.path();
        Json job = Json::object();
        job["server"] = Json::object({{"kind", "cpp"}, {"genko", genko::test::genko_cli().toStdString()}});
        job["root"] = (dir + "/root").toStdString();
        job["config"] = (dir + "/config").toStdString();
        job["out"] = (dir + "/out").toStdString();
        job["agent"] = "ai:test";
        job["pages"] = 8;
        job["perf"] = Json::object({{"each", 100}, {"proofs", 10}});
        // (the harness stops itself before its process is given up on, saying where it was: ctest gives the test 3600 s)
        job["time_limit_s"] = 3150;
        genko::test::write_bytes(dir + "/job.json", genko::core::dump_python(job));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("PYTHONPATH"));
        env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        const genko::test::Run run = genko::test::run(
            genko::test::mcp_python(), {genko::test::repo_root() + "/tools/migration/pipeline_harness.py", QStringLiteral("run"), dir + "/job.json", dir + "/run.json"},
            env, (3150 + 120) * 1000);
        QVERIFY2(run.finished && run.exit_code == 0, run.err.right(6000).constData());
        const Json out = genko::test::read_json(dir + "/run.json");

        qInfo("machine: %s — not the reference class (ACCEPTANCE.md §6: this development machine stands in for it)", qPrintable(machine()));
        qInfo("F3: the making in %.1f s, %d work items; the person's PDF and %zu TIFFs", out["making_seconds"].get<double>(), out["agent"]["steps"].get<int>(),
              out["person"]["exports"].size() == 2 ? out["person"]["exports"][1]["files"].size() : std::size_t{0});

        // every call of the making, the export included
        Stats all;
        Stats tools;
        Stats commands;
        Stats exports;
        for (const Json& call : out["calls"]) {
            const double ms = call["ms"].get<double>();
            all.add(ms);
            (call["by"] == "agent" ? tools : commands).add(ms);
            const bool exporting = call["by"] == "agent" ? call["tool"] == "export" || call["tool"] == "export_proof"
                                                         : call["cli"].size() > 1 && call["cli"][1] == "export";
            if (exporting) exports.add(ms);
        }
        qInfo("%s", qPrintable(all.line("PERF-F every call of the making")));
        qInfo("%s", qPrintable(tools.line("  of them the agent's tools (MCP)")));
        qInfo("%s", qPrintable(commands.line("  of them the person's commands (genko studio)")));
        qInfo("%s", qPrintable(exports.line("  of them exports (export_proof, the official export)")));

        // each kind, at least 100 times
        const Json& perf = out["perf"];
        Stats kinds[4];
        const char* names[4] = {"info", "edit", "adopt", "finish"};
        const char* words[4] = {"PERF-F information (status, inspect, candidates, next, tickets, render)", "PERF-F edit committed (apply_ops)",
                                "PERF-F candidate adopted (adopt, enlarged)", "PERF-F page finished (finish_page)"};
        for (int k = 0; k < 4; ++k) {
            for (const Json& call : perf[names[k]]) kinds[k].add(call["ms"].get<double>());
            qInfo("%s", qPrintable(kinds[k].line(words[k])));
        }
        Stats approvals;
        for (const Json& ms : perf["approve_ms"]) approvals.add(ms.get<double>());
        qInfo("%s", qPrintable(approvals.line("  (between them: the person's approval of the art, genko studio approve)")));

        // the 8-page proof, 10 times. MCP's export_proof is always a job, and the tool waits up to 40 s for it before it
        // answers: the reply holds that wait (it comes with the PDF when the job ends within it). The job's own record
        // says when it started (after the call) and how long it ran.
        Stats accepted;
        Stats done;
        Stats job_start;
        Stats job_run;
        int proof = 0;
        for (const Json& p : perf["proofs"]) {
            ++proof;
            accepted.add(p["accepted_ms"].get<double>());
            done.add(p["done_ms"].get<double>());
            if (p.value("job_start_ms", Json()).is_number()) job_start.add(p["job_start_ms"].get<double>());
            if (p.value("job_ms", Json()).is_number()) job_run.add(p["job_ms"].get<double>());
            qInfo("proof %d: replied %.1f ms, its PDF whole %.1f ms; the job started %.1f ms after the call and ran %.1f ms (%s)", proof,
                  p["accepted_ms"].get<double>(), p["done_ms"].get<double>(), p.value("job_start_ms", Json(-1.0)).get<double>(),
                  p.value("job_ms", Json(-1.0)).get<double>(), p["file"].is_string() ? p["file"].get<std::string>().c_str() : "no file");
        }
        qInfo("%s", qPrintable(accepted.line("PERF-F 8-page proof: the reply (the tool waits up to 40 s for the job: within it, the reply is the job's end)")));
        qInfo("%s", qPrintable(done.line("PERF-F 8-page proof: from the call to its PDF whole")));
        qInfo("%s", qPrintable(job_start.line("  the job's record: from the call to the job's start")));
        qInfo("%s", qPrintable(job_run.line("  the job's record: from its start to its end")));

        // the pass values (ACCEPTANCE.md §4 PERF-F): every one judged, all that fail said together
        QStringList failed;
        if (all.v.size() <= 100) failed << QStringLiteral("too few calls in the making");
        if (!(all.pct(0.95) <= 2000.0)) failed << all.line("every call of the making") + QStringLiteral(": p95 over 2 s");
        for (int k = 0; k < 4; ++k) {
            if (kinds[k].v.size() < 100) failed << kinds[k].line(words[k]) + QStringLiteral(": fewer than 100");
            if (!(kinds[k].pct(0.95) <= 2000.0)) failed << kinds[k].line(words[k]) + QStringLiteral(": p95 over 2 s");
        }
        if (perf["proofs"].size() != 10) failed << QStringLiteral("not 10 proofs");
        if (job_run.v.size() != perf["proofs"].size()) failed << QStringLiteral("a proof's job record without its start and end");
        std::vector<std::string> files;
        proof = 0;
        for (const Json& p : perf["proofs"]) {
            ++proof;
            if (!(p["ok"].get<bool>() && p["pdf"].get<bool>() && p["pages"].get<int>() == 8)) {
                failed << QStringLiteral("proof %1 is not an 8-page PDF: %2").arg(proof).arg(QString::fromStdString(p.dump()));
            }
            if (!p["new_folder"].get<bool>()) failed << QStringLiteral("proof %1 did not go to a new folder").arg(proof);
            if (!(p["done_ms"].get<double>() <= 30000.0)) failed << QStringLiteral("proof %1: %2 ms, over 30 s").arg(proof).arg(p["done_ms"].get<double>(), 0, 'f', 1);
            if (p["file"].is_string()) files.push_back(p["file"].get<std::string>());
        }
        std::sort(files.begin(), files.end());
        if (std::adjacent_find(files.begin(), files.end()) != files.end()) failed << QStringLiteral("two proofs went to the same place");
        QVERIFY2(failed.isEmpty(), qPrintable(QStringLiteral("PERF-F not met: ") + failed.join(QStringLiteral("; "))));
    }

private:
    QTemporaryDir scratch_;
};

QTEST_GUILESS_MAIN(PerfPipeline)
#include "test_perf_pipeline.moc"
