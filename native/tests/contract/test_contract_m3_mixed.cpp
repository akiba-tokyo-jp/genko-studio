// The milestones together (M3-I): `genko apply` against `python -m genko apply` on random books carrying tones, effect
// lines, rulers and 3D besides panels, layers and pen lines (tools/migration/mixed_harness.py make-books: 32 books, the
// C++ build working on a v4 copy made by `genko migrate`), each given a random sequence of 10 steps mixing the ops of
// tones, effect lines, rulers and 3D with M2's (add_stroke, erase, add_layer, split_frame, add_page, …;
// mixed_harness.py sequences: what an op names has the same id on both sides). At each step:
//   - the same exit code and the same JSON (ok, applied, the snapshot and warnings, or the same error); where Python
//     stops with a traceback (an exception apply_ops lets through), python_error, the error ending with the
//     traceback's last line;
// and at the end of each sequence:
//   - the same book (`inspect --full`);
//   - every page of it drawn by `genko render` and `python -m genko render` in print, proof and name at 72 and 150 dpi:
//     the same JSON (each with its own --out) and a PNG of the same pixels.
// Not compared: "revision" and "txn" of a success (a v4 book's; checked to be there), "code" of a failure (checked to be
// there) and "job_id" (a new id on each side, checked to be one); the ids each side makes at random for what the ops
// add (pen lines, panels cut, pages added, layers copied) are replaced by the order in which they first appear, in
// messages too. The books are worked on three at a time, Python's command and the C++ build's at once.
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"

using genko::core::Json;

namespace {

constexpr int kBooks = 32;
constexpr int kSteps = 10;
constexpr int kWorkers = 3;
const char* const kModes[] = {"print", "proof", "name"};
const int kDpis[] = {72, 150};

const char* const kToneOps[] = {"add_tone",      "set_tone",  "delete_tone", "add_effect", "edit_effect",  "delete_effect",
                                "effect_to_layer", "add_ruler", "edit_ruler", "delete_ruler", "set_ruler", "ruler_to_layer"};
const char* const k3dOps[] = {"add_figure",    "pose_figure", "add_head",       "add_hand",   "import_model", "set_camera",
                              "set_light",     "render_prims", "add_mannequin", "pose_mannequin", "add_prim3d", "add_scene",
                              "edit_prim",     "delete_prim", "trace_prims",    "ruler_from_3d", "camera_from_ruler"};

genko::test::Run mixed_harness(const QStringList& args, const QString& scratch) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/mixed_harness.py")};
    full += args;
    return genko::test::run(genko::test::python_ref(), full, genko::test::python_env(scratch), 3600000);
}

// The two commands of a step, Python's and the C++ build's, run at once.
struct Pair {
    genko::test::Run py;
    genko::test::Run cpp;
};

genko::test::Run finish(QProcess& process) {
    genko::test::Run result;
    result.started = process.waitForStarted(60000);
    if (!result.started) return result;
    process.closeWriteChannel();
    result.finished = process.waitForFinished(900000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.exit_code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

struct Envs {
    QProcessEnvironment py;
    QProcessEnvironment cpp;
};

Pair run_pair(const QStringList& py_args, const QStringList& cpp_args, const Envs& envs) {
    QProcess py;
    QProcess cpp;
    py.setProcessEnvironment(envs.py);
    cpp.setProcessEnvironment(envs.cpp);
    py.start(genko::test::python_ref(), QStringList{"-m", "genko"} + py_args);
    cpp.start(genko::test::genko_cli(), cpp_args);
    Pair out;
    out.py = finish(py);
    out.cpp = finish(cpp);
    return out;
}

bool is_hex(const Json& value, std::size_t size) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void collect_strings(const Json& value, std::set<std::string>& out) {
    if (value.is_string()) {
        out.insert(value.get<std::string>());
    } else if (value.is_array()) {
        for (const Json& item : value) collect_strings(item, out);
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            out.insert(it.key());
            collect_strings(it.value(), out);
        }
    }
}

// New ids (12 hex digits, "pg_" before them for a page; alone or in a message) the book did not have (`known`),
// replaced by "<new N>" in the order they first appear.
std::string renamed(const std::string& text, const std::set<std::string>& known, std::map<std::string, std::string>& seen) {
    static const std::regex id(R"(\b(?:pg_)?[0-9a-f]{12}\b)");
    if (text.size() < 12) return text;
    std::string out;
    std::size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), id); it != std::sregex_iterator(); ++it) {
        const std::string token = it->str();
        const auto at = static_cast<std::size_t>(it->position());
        out += text.substr(last, at - last);
        if (known.contains(token)) {
            out += token;
        } else {
            out += seen.emplace(token, "<new " + std::to_string(seen.size() + 1) + ">").first->second;
        }
        last = at + token.size();
    }
    return out + text.substr(last);
}

Json numbered(const Json& value, const std::set<std::string>& known, std::map<std::string, std::string>& seen) {
    if (value.is_string()) return Json(renamed(value.get<std::string>(), known, seen));
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(numbered(item, known, seen));
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) out[renamed(it.key(), known, seen)] = numbered(it.value(), known, seen);
        return out;
    }
    return value;
}

Json numbered(const Json& value, const std::set<std::string>& known) {
    std::map<std::string, std::string> seen;
    return numbered(value, known, seen);
}

// The last line of a traceback ("IndexError: list index out of range").
std::string last_line(const QByteArray& err) {
    const QList<QByteArray> lines = err.trimmed().split('\n');
    return lines.isEmpty() ? std::string() : lines.back().trimmed().toStdString();
}

std::string outputs(const Pair& r) {
    return " | C++ " + r.cpp.out.left(500).toStdString() + r.cpp.err.left(500).toStdString() + " | Python " +
           r.py.out.left(500).toStdString() + r.py.err.right(500).toStdString();
}

// What differs between the replies of a step ("" when nothing does); `kind` gets applied, refused or traceback.
std::string step_difference(const Pair& r, bool dry, const std::set<std::string>& known, std::string& kind) {
    if (!r.py.finished || !r.cpp.finished) return "did not finish";
    if (r.py.exit_code != r.cpp.exit_code) {
        return "exit code " + std::to_string(r.cpp.exit_code) + ", Python's " + std::to_string(r.py.exit_code);
    }
    Json got = genko::test::one_line(r.cpp.out);
    if (!got.is_object()) return "C++ printed " + got.dump();
    if (r.py.out.isEmpty()) {  // (a traceback)
        const std::string line = last_line(r.py.err);
        if (!r.py.err.contains("Traceback") || got.value("code", std::string()) != "python_error" ||
            !got.value("error", std::string()).ends_with(line)) {
            return "Python stopped with " + line;
        }
        kind = "traceback";
        return {};
    }
    Json want = genko::test::one_line(r.py.out);
    if (!want.is_object()) return "Python printed " + want.dump();
    if (got.value("ok", false)) {
        const bool has_revision = got.contains("revision") && got["revision"].is_number_integer();
        const bool has_txn = dry || is_hex(got.value("txn", Json()), 32);
        if (!has_revision || !has_txn || !is_hex(got.value("job_id", Json()), 12) || !is_hex(want.value("job_id", Json()), 12)) {
            return "revision, txn or job_id missing";
        }
        for (const char* key : {"revision", "txn", "job_id"}) got.erase(key);
        want.erase("job_id");
        kind = "applied";
    } else {
        if (!got.contains("code") || !got["code"].is_string() || got["code"].get<std::string>().empty()) return "no code";
        got.erase("code");
        kind = "refused";
    }
    std::string where;
    if (!genko::test::strict_equal(numbered(got, known), numbered(want, known), &where)) return "the replies differ: " + where;
    return {};
}

struct BookRun {
    std::string name;
    int steps = 0;
    int applied = 0;
    int refused = 0;
    int tracebacks = 0;
    int renders = 0;
    int renders_same = 0;
    bool compared_to_the_end = false;
    std::map<std::string, int> ops;
    std::vector<std::string> failures;
};

// One book through its sequence, the book at the end and its pages drawn; in `dir`.
BookRun run_book(const Json& sequence, const QString& dir, const Envs& envs) {
    BookRun run;
    const QString book = QString::fromStdString(sequence["book"].get<std::string>());
    run.name = QFileInfo(book).fileName().toStdString();
    const QString py_book = dir + QStringLiteral("/py.genko");
    const QString cpp_book = dir + QStringLiteral("/cpp.genko");
    QDir().mkpath(dir);
    genko::test::copy_tree(book, py_book);
    const auto migrated = genko::test::run(genko::test::genko_cli(), {"migrate", book, cpp_book}, envs.cpp, 600000);
    if (!migrated.finished || migrated.exit_code != 0) {
        run.failures.push_back(run.name + ": genko migrate failed: " + (migrated.out + migrated.err).toStdString());
        return run;
    }
    std::set<std::string> known;  // (every string of the book's files: its ids among them)
    QDirIterator files(book, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString file = files.next();
        if (file.endsWith(QLatin1String(".json"))) collect_strings(genko::test::read_json(file), known);
    }

    const Json& steps = sequence["steps"];
    for (std::size_t s = 0; s < steps.size(); ++s) {
        const Json& step = steps[s];
        const QString ops = dir + QStringLiteral("/ops-%1.json").arg(s);
        genko::test::write_bytes(ops, genko::core::dump_python(step["ops"]));
        const std::string agent = step["agent"].get<std::string>();
        const bool dry = step["dry_run"].get<bool>();
        QStringList tail{ops, "--agent", QString::fromStdString(agent)};
        if (dry) tail << "--dry-run";
        const Pair r = run_pair(QStringList{"apply", py_book} + tail, QStringList{"apply", cpp_book} + tail, envs);
        ++run.steps;
        for (const Json& op : step["ops"]) ++run.ops[op.is_object() ? op.value("op", std::string("?")) : std::string("?")];
        std::string kind;
        const std::string difference = step_difference(r, dry, known, kind);
        if (!difference.empty()) {
            run.failures.push_back(run.name + " step " + std::to_string(s) + " " + genko::core::dump_python(step["ops"]).substr(0, 400) +
                                   " as " + agent + (dry ? " (dry run)" : "") + ": " + difference + outputs(r));
            return run;  // (the books may differ from here on)
        }
        (kind == "applied" ? run.applied : kind == "refused" ? run.refused : run.tracebacks) += 1;
    }

    const Pair inspected = run_pair({"inspect", py_book, "--full"}, {"inspect", cpp_book, "--full"}, envs);
    if (inspected.py.exit_code != 0 || inspected.cpp.exit_code != 0) {
        run.failures.push_back(run.name + ": inspect failed" + outputs(inspected));
        return run;
    }
    const Json theirs = genko::test::one_line(inspected.py.out);
    const Json mine = genko::test::one_line(inspected.cpp.out);
    std::string where;
    if (!genko::test::strict_equal(numbered(mine, known), numbered(theirs, known), &where)) {
        run.failures.push_back(run.name + ": the books differ at the end (inspect --full): " + where);
        return run;
    }
    run.compared_to_the_end = true;

    for (const Json& page : theirs["pages"]) {
        const QString index = QString::number(page["index"].get<std::int64_t>());
        for (const char* mode : kModes) {
            for (const int dpi : kDpis) {
                const QString name = QStringLiteral("p%1-%2-%3").arg(index, QString::fromLatin1(mode)).arg(dpi);
                const QString py_out = dir + QStringLiteral("/py-") + name + QStringLiteral(".png");
                const QString cpp_out = dir + QStringLiteral("/cpp-") + name + QStringLiteral(".png");
                const QStringList args{"--page", index, "--dpi", QString::number(dpi), "--mode", QString::fromLatin1(mode)};
                const Pair r = run_pair(QStringList{"render", py_book, "--out", py_out} + args,
                                        QStringList{"render", cpp_book, "--out", cpp_out} + args, envs);
                ++run.renders;
                const std::string label = run.name + " page " + index.toStdString() + " " + mode + " " + std::to_string(dpi) + " dpi";
                if (!r.py.finished || !r.cpp.finished || r.py.exit_code != 0 || r.cpp.exit_code != 0) {
                    run.failures.push_back(label + ": exit code " + std::to_string(r.cpp.exit_code) + ", Python's " +
                                           std::to_string(r.py.exit_code) + outputs(r));
                    continue;
                }
                Json want = genko::test::one_line(r.py.out);
                Json got = genko::test::one_line(r.cpp.out);
                if (!want.is_object() || !got.is_object() || want.value("path", std::string()) != py_out.toStdString() ||
                    got.value("path", std::string()) != cpp_out.toStdString()) {
                    run.failures.push_back(label + ": the replies" + outputs(r));
                    continue;
                }
                want["path"] = "<out>";
                got["path"] = "<out>";
                if (!genko::test::strict_equal(got, want, &where)) {
                    run.failures.push_back(label + ": the replies differ: " + where);
                    continue;
                }
                const genko::render::Image a = genko::render::read_png(genko::test::read_bytes(cpp_out));
                const genko::render::Image b = genko::render::read_png(genko::test::read_bytes(py_out));
                const std::string mode_a(a.mode());
                const std::string mode_b(b.mode());
                if (mode_a != mode_b || a.width() != b.width() || a.height() != b.height()) {
                    run.failures.push_back(label + ": " + mode_a + " " + std::to_string(a.width()) + "x" + std::to_string(a.height()) +
                                           ", Python's " + mode_b + " " + std::to_string(b.width()) + "x" + std::to_string(b.height()));
                    continue;
                }
                if (a.tobytes() != b.tobytes()) {
                    const genko::test::PixelDiff d = genko::test::pixel_diff(a, b);
                    genko::test::keep_pictures(QString::fromStdString(run.name) + QLatin1Char('-') + name, a, b);
                    run.failures.push_back(label + ": " + std::to_string(d.pixels) + " pixels differ (by up to " + std::to_string(d.largest) +
                                           ", the first at " + std::to_string(d.first) + ")");
                    continue;
                }
                ++run.renders_same;
            }
        }
    }
    return run;
}

}  // namespace

class TestContractM3Mixed : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json sequences_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        auto r = mixed_harness({"make-books", path("books"), "--seed", "20261002", "--count", QString::number(kBooks)}, path("harness"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        r = mixed_harness({"sequences", path("sequences.json"), "--books", path("books"), "--seed", "20261002", "--steps",
                           QString::number(kSteps)},
                          path("harness"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        sequences_ = genko::test::read_json(path("sequences.json"));
        QCOMPARE(sequences_.size(), std::size_t{kBooks});
    }

    void sequencesMatchPython() {
        // (each worker its own Python home: the commands may write their settings there)
        std::vector<Envs> envs;
        QProcessEnvironment cpp = QProcessEnvironment::systemEnvironment();
        cpp.remove(QStringLiteral("GENKO_FAULT"));
        cpp.remove(QStringLiteral("GENKO_TEST_MIGRATE_HOOK"));
        for (int w = 0; w < kWorkers; ++w) envs.push_back(Envs{genko::test::python_env(path(QStringLiteral("py-home-%1").arg(w))), cpp});
        std::vector<BookRun> runs(sequences_.size());
        std::atomic<std::size_t> next{0};
        std::vector<std::unique_ptr<QThread>> workers;
        for (int w = 0; w < kWorkers; ++w) {
            workers.emplace_back(QThread::create([&, w] {
                for (std::size_t n = next++; n < sequences_.size(); n = next++) {
                    runs[n] = run_book(sequences_[n], path(QStringLiteral("work/%1").arg(n)), envs[static_cast<std::size_t>(w)]);
                }
            }));
            workers.back()->start();
        }
        for (const auto& worker : workers) worker->wait();

        int steps = 0, applied = 0, refused = 0, tracebacks = 0, renders = 0, renders_same = 0, to_the_end = 0;
        std::map<std::string, int> ops;
        std::vector<std::string> failures;
        for (const BookRun& run : runs) {
            steps += run.steps;
            applied += run.applied;
            refused += run.refused;
            tracebacks += run.tracebacks;
            renders += run.renders;
            renders_same += run.renders_same;
            to_the_end += run.compared_to_the_end ? 1 : 0;
            for (const auto& [op, count] : run.ops) ops[op] += count;
            failures.insert(failures.end(), run.failures.begin(), run.failures.end());
        }
        std::string spread;
        for (const auto& [op, count] : ops) spread += op + ":" + std::to_string(count) + " ";
        qInfo("%zu books, %d steps (%d applied, %d refused, %d where Python stops with a traceback), %d books the same at the end; "
              "%d pages drawn, %d the same",
              runs.size(), steps, applied, refused, tracebacks, to_the_end, renders, renders_same);
        qInfo("ops: %s", spread.c_str());
        for (std::size_t i = 0; i < failures.size() && i < 60; ++i) qWarning("%s", failures[i].c_str());
        QCOMPARE(runs.size(), std::size_t{kBooks});
        QCOMPARE(steps, kBooks * kSteps);
        QCOMPARE(to_the_end, kBooks);
        QCOMPARE(failures.size(), std::size_t{0});
        QCOMPARE(renders_same, renders);
        QVERIFY(renders >= kBooks * 6);
        QVERIFY(applied >= steps / 2);
        // every op of tones, effect lines, rulers and 3D in the sequences, and most of M2's
        for (const char* name : kToneOps) QVERIFY2(ops.contains(name), name);
        for (const char* name : k3dOps) QVERIFY2(ops.contains(name), name);
        QVERIFY(ops.size() >= std::size(kToneOps) + std::size(k3dOps) + 25);
    }
};

QTEST_GUILESS_MAIN(TestContractM3Mixed)
#include "test_contract_m3_mixed.moc"
