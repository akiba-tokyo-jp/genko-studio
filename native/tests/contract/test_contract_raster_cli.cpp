// The ops of M3-A1 through the command line: `genko apply` against `python -m genko apply` (the baseline's own command
// line), on the book of the raster op contract tests (`pyref_harness.py make-rasterbook`, a v3 book: Python gets a copy
// of it, C++ a copy of its `genko migrate` conversion). The fixed cases of contract/raster_cases.json that are one batch
// (for each op its first 4 that succeed and first 3 that fail, and every batch-level case): the same exit code and the
// same JSON on stdout, and then the same book: `inspect --full` of each, and the books' payloads with the pictures they
// refer to as the pixels those hold (both books read by this build). Not compared, as in test_contract_ops_cli: "revision"
// and "txn" of a success and "code" of a failure (keys only a v4 book has; checked to be there), "job_id" (a new id
// on both sides), and the ids each side makes at random for what the ops add (numbered in the order they first appear,
// on each side). Where Python stops with a traceback, C++ prints {"ok": false, "error", "code": "python_error"} and
// exits 1 as Python does: the error ends with the traceback's last line. Left out: the parts this build refuses on
// purpose ("cpp": "not_yet_ported") and the perspective warps ("near": the same within a tolerance only, which
// test_contract_raster_ops checks). Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/ids.hpp"
#include "opsupport.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

bool is_hex(const std::string& text, std::size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// An id as Python's new_id makes them (12 hex digits; a page's id is "pg_" and 12).
bool is_id(const std::string& text) { return is_hex(text, 12) || (text.starts_with("pg_") && is_hex(text.substr(3), 12)); }

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

// The ids that are not in the book (`known`) replaced by "new-1", "new-2", … in the order they first appear.
Json with_new_ids_numbered(const Json& value, const std::set<std::string>& known, std::map<std::string, std::string>& seen) {
    const auto renamed = [&](const std::string& text) {
        if (!is_id(text) || known.contains(text)) return text;
        const auto [it, added] = seen.emplace(text, "new-" + std::to_string(seen.size() + 1));
        return it->second;
    };
    if (value.is_string()) return Json(renamed(value.get<std::string>()));
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(with_new_ids_numbered(item, known, seen));
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) out[renamed(it.key())] = with_new_ids_numbered(it.value(), known, seen);
        return out;
    }
    return value;
}

Json numbered(const Json& value, const std::set<std::string>& known) {
    std::map<std::string, std::string> seen;
    return with_new_ids_numbered(value, known, seen);
}

// The last line of a traceback ("KeyError: 'page'").
std::string last_line(const QByteArray& err) {
    const QList<QByteArray> lines = err.trimmed().split('\n');
    return lines.isEmpty() ? std::string() : lines.back().trimmed().toStdString();
}

const char* const kOps[] = {"convert_layer", "merge_down",    "merge_layers", "merge_visible", "move_layers",
                            "group_layers",  "set_layer_mask", "paint_mask",   "put_raster",    "filter_raster",
                            "fill",          "fill_area",     "fill_enclosed", "fill_gaps",     "flood_fill",
                            "gradient_fill", "delete_area",   "transform_area", "paste",        "store_area",
                            "forget_area",   "set_paper",     "set_timelapse", "erase",         "erase_raster",
                            "set_stroke_width", "reshape_stroke"};

}  // namespace

class TestContractRasterCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString v3_;  // the book as Python made it
    QString v4_;  // its conversion
    std::set<std::string> known_;  // every string in the book's files (its ids among them)

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run python(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args, genko::test::python_env(path("pyenv")));
    }

    // `inspect --full` of a book on each side, the new ids numbered.
    std::string same_books(const QString& cpp_book, const QString& py_book, const Json& ops, bool near) {
        const auto cpp = genko::test::run_genko({"inspect", cpp_book, "--full"});
        const auto py = python({"inspect", py_book, "--full"});
        if (cpp.exit_code != 0 || py.exit_code != 0) return "inspect failed: " + (cpp.out + cpp.err + py.out + py.err).toStdString();
        const Json a = numbered(genko::test::one_line(cpp.out), known_), b = numbered(genko::test::one_line(py.out), known_);
        std::string where;
        if (near) {
            genko::test::NearSides sides;
            genko::test::affect_perspective(sides, ops, genko::test::read_json(py_book + "/project.json"));
            genko::test::StepOutcome got;
            got.reply = Json::object({{"ok", true}});
            got.full = a;
            got.payload = Json::object();
            const Json expected = Json::object({{"reply", got.reply}, {"full", b}, {"payload", got.payload}});
            return genko::test::compare_step_near(got, expected, sides);
        }
        if (!genko::test::strict_equal(a, b, &where)) return "the books differ: " + where;
        return {};
    }

    // The books' payloads (both read by this build), their pictures as the pixels they hold and the lines of each layer
    // as they are (their ids are new on each side), the new ids numbered.
    std::string same_pictures(const QString& cpp_book, const QString& py_book, int n, const Json& ops, bool near) {
        genko::storage::AssetStore cpp_store(genko::storage::path_from_utf8(path(QStringLiteral("store-cpp-%1").arg(n)).toStdString()));
        genko::storage::AssetStore py_store(genko::storage::path_from_utf8(path(QStringLiteral("store-py-%1").arg(n)).toStdString()));
        genko::test::NearSides sides;
        sides.cpp_store = &cpp_store;
        sides.py_store = path(QStringLiteral("store-py-%1").arg(n));
        sides.cpp_dump = path(QStringLiteral("dump-cpp-%1").arg(n));
        sides.py_dump = path(QStringLiteral("dump-py-%1").arg(n));
        const auto payload = [&](const QString& book, genko::storage::AssetStore& store, const QString& dump) {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(book.toStdString()));
            if (near) genko::test::dump_pictures(loaded.document, store, dump, "");
            Json out = genko::test::state_of(loaded.document, store).payload;
            out.erase("revision");
            for (Json& page : out["pages"]) {
                for (Json& layer : page["layers"]) {
                    if (!layer.contains("strokes_blob") || !layer["strokes_blob"].is_string()) continue;
                    const auto bytes = store.get_bytes(layer["strokes_blob"].get<std::string>(), ".strokes.json");
                    layer["strokes_blob"] = bytes ? Json::parse(*bytes) : Json("missing");
                }
            }
            return numbered(out, known_);
        };
        const Json a = payload(cpp_book, cpp_store, sides.cpp_dump);
        const Json b = payload(py_book, py_store, sides.py_dump);
        std::string where;
        if (near) {
            genko::test::affect_perspective(sides, ops, a);
            genko::test::StepOutcome got;
            got.reply = Json::object({{"ok", true}});
            got.full = Json::object();
            got.payload = a;
            return genko::test::compare_step_near(got, Json::object({{"reply", got.reply}, {"full", got.full}, {"payload", b}}), sides);
        }
        if (!genko::test::strict_equal(a, b, &where)) return "the books' payloads differ: " + where;
        return {};
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        // (the plugin filters are looked for in the same empty folder on both sides)
        QDir().mkpath(path("pyenv/config"));
        qputenv("GENKO_CONFIG_DIR", path("pyenv/config").toUtf8());
        v3_ = path("rasterbook.genko");
        v4_ = path("rasterbook-v4.genko");
        const auto made = genko::test::harness({"make-rasterbook", v3_}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const auto converted = genko::test::run_genko({"migrate", v3_, v4_});
        QVERIFY2(converted.exit_code == 0, (converted.out + converted.err).constData());
        QDirIterator it(v3_, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            if (file.endsWith(QLatin1String(".json"))) collect_strings(genko::test::read_json(file), known_);
        }
        QVERIFY(known_.size() > 50);
    }

    void applyAgainstPython() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/raster_cases.json");
        std::map<std::string, std::pair<int, int>> taken;  // op → (successes, failures) chosen
        std::map<std::string, std::pair<int, int>> matched;
        std::vector<std::string> failures;
        int n = 0, tracebacks = 0;
        for (const Json& c : file["cases"]) {
            ++n;
            if (!c.contains("ops") || c.contains("cpp")) continue;
            const std::string op = c["op"].get<std::string>();
            const bool ok = c["ok"].get<bool>();
            auto& [oks, fails] = taken[op];
            if (!op.starts_with("_") && !c.contains("near") && (ok ? oks >= 4 : fails >= 3)) continue;
            (ok ? oks : fails) += 1;

            const std::string name = c["n"].get<std::string>();
            const QString cpp_book = path(QStringLiteral("cpp-%1.genko").arg(n));
            const QString py_book = path(QStringLiteral("py-%1.genko").arg(n));
            genko::test::copy_tree(v4_, cpp_book);
            genko::test::copy_tree(v3_, py_book);
            const QString ops_file = path(QStringLiteral("ops-%1.json").arg(n));
            genko::test::write_bytes(ops_file, genko::core::dump_python(c["ops"]));
            const bool dry = c.value("dry", false);
            const QString agent = QString::fromStdString(c.value("agent", std::string("genko")));
            QStringList tail{ops_file, "--agent", agent};
            if (dry) tail << "--dry-run";
            const auto cpp = genko::test::run_genko(QStringList{"apply", cpp_book} + tail);
            const auto py = python(QStringList{"apply", py_book} + tail);

            const auto fail = [&](const std::string& why) {
                failures.push_back(name + ": " + why.substr(0, 1500) + " | C++ " + cpp.out.left(300).toStdString() +
                                   cpp.err.left(300).toStdString() + " | Python " + py.out.left(300).toStdString() +
                                   py.err.right(300).toStdString());
            };
            if (!cpp.finished || !py.finished) {
                fail("did not finish");
                continue;
            }
            if (cpp.exit_code != py.exit_code) {
                fail("exit codes " + std::to_string(cpp.exit_code) + " and " + std::to_string(py.exit_code));
                continue;
            }
            Json got = genko::test::one_line(cpp.out);
            if (!got.is_object()) {
                fail("C++ printed " + got.dump());
                continue;
            }
            if (py.out.isEmpty()) {  // a traceback
                const std::string line = genko::test::without_addresses(last_line(py.err));
                if (!py.err.contains("Traceback") || got.value("code", std::string()) != "python_error" ||
                    !got.value("error", std::string()).ends_with(line)) {
                    fail("Python stopped with " + line);
                    continue;
                }
                ++tracebacks;
            } else {
                Json want = genko::test::one_line(py.out);
                if (want.contains("error") && want["error"].is_string()) {
                    want["error"] = genko::test::without_addresses(want["error"].get<std::string>());
                }
                if (got.value("ok", false)) {
                    // the keys of a v4 book, and the job's new id
                    const bool has_revision = got.contains("revision") && got["revision"].is_number_integer();
                    const bool has_txn = dry || (got.contains("txn") && is_hex(got["txn"].get<std::string>(), 32));
                    if (!has_revision || !has_txn || !is_hex(got.value("job_id", std::string()), 12) ||
                        !is_hex(want.value("job_id", std::string()), 12)) {
                        fail("revision, txn or job_id missing");
                        continue;
                    }
                    for (const char* key : {"revision", "txn", "job_id"}) got.erase(key);
                    want.erase("job_id");
                } else {
                    if (!got.contains("code") || got["code"].get<std::string>().empty()) {
                        fail("no code");
                        continue;
                    }
                    got.erase("code");
                }
                std::string where;
                if (!genko::test::strict_equal(numbered(got, known_), numbered(want, known_), &where)) {
                    fail("stdout: " + where);
                    continue;
                }
            }
            if (const std::string differ = same_books(cpp_book, py_book, c["ops"], c.contains("near")); !differ.empty()) {
                fail(differ);
                continue;
            }
            if (const std::string differ = same_pictures(cpp_book, py_book, n, c["ops"], c.contains("near")); !differ.empty()) {
                fail(differ);
                continue;
            }
            auto& [ok_matched, fail_matched] = matched[op];
            (ok ? ok_matched : fail_matched) += 1;
            QDir(cpp_book).removeRecursively();
            QDir(py_book).removeRecursively();
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " batches differ from Python's command line").c_str());
        std::string summary;
        int total = 0;
        for (const auto& [op, counts] : matched) {
            summary += op + " " + std::to_string(counts.first) + "/" + std::to_string(counts.second) + ", ";
            total += counts.first + counts.second;
        }
        for (const char* op : kOps) {
            const auto [ok, failing] = matched[op];
            QVERIFY2(ok >= 3 && failing >= 3, (std::string(op) + ": too few cases").c_str());
        }
        qInfo("%d batches the same as `python -m genko apply` (ok/failing): %s%d of them a traceback in Python", total,
              summary.c_str(), tracebacks);
    }
};

int main(int argc, char** argv) {
    // (applyAgainstPython runs some 800 programs: with the sanitizers that takes longer than QtTest's 5 minutes for a
    // test function; CTest's TIMEOUT still bounds the whole)
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "5000000");
    QCoreApplication app(argc, argv);
    TestContractRasterCli test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_raster_cli.moc"
