// Random op sequences against the Python baseline: 150 sequences of 3 to 8 ops (the 3D ops mixed with the book ops this
// build has: set_note, name_ok, set_meta, lock_page, add_page) over 15 random books with 3D on their pages
// (geom3d_harness.py make-books and random-ops, seeds fixed), by three actors. After every op both sides must give the
// same reply or error, the same full snapshot and the same 3D state (prims, rulers, camera and light, strokes by their
// bytes, pictures by their pixels); at the end of each sequence the same book as saved.
// Where Python fails harder than an error (an exception apply_ops lets through) the C++ build must refuse the op and keep
// the book as it was. Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/writer.hpp"
#include "test3d.hpp"

using genko::core::Json;

class TestContract3dRandom : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json sequences_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        auto r = genko::test::geom3d_harness({"make-books", path("books"), "--seed", "20261002", "--count", "15"}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        r = genko::test::geom3d_harness({"random-ops", path("sequences.json"), "--seed", "20261002", "--count", "150", "--books", path("books")},
                                        path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        sequences_ = genko::test::read_json(path("sequences.json"));
        QCOMPARE(sequences_.size(), std::size_t{150});
        Json jobs = Json::array();
        for (std::size_t n = 0; n < sequences_.size(); ++n) {
            Json job = sequences_[n];
            job["ids"] = true;
            job["out"] = path(QStringLiteral("out/%1.json").arg(n)).toStdString();
            job["dest"] = path(QStringLiteral("out/%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        QDir().mkpath(path("out"));
        genko::test::write_bytes(path("out/jobs.json"), genko::core::dump_python(jobs));
        r = genko::test::geom3d_harness({"apply", path("out/jobs.json")}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
    }

    void sequencesMatchPython() {
        int failures = 0;
        int steps = 0;
        int refused = 0;
        int crashes = 0;
        std::map<std::string, int> ops;
        std::set<std::string> books;
        for (std::size_t n = 0; n < sequences_.size(); ++n) {
            const Json& seq = sequences_[n];
            const QString out = path(QStringLiteral("out/%1").arg(n));
            const Json want = genko::test::read_json(out + ".json")["steps"];
            const std::string book = seq["book"].get<std::string>();
            books.insert(book);
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            genko::core::Document doc = genko::storage::load_document(genko::storage::path_from_utf8(book)).document;
            bool diverged = false;
            for (std::size_t s = 0; s < seq["steps"].size(); ++s) {
                const Json& batch = seq["steps"][s];
                const Json& w = want[s];
                const std::string label = "sequence " + std::to_string(n) + " step " + std::to_string(s) + " " +
                                          genko::core::dump_python(batch).substr(0, 140);
                ++steps;
                ++ops[batch[0]["op"].get<std::string>()];
                Json got;
                try {
                    const auto result = genko::core::CommandBus(genko::render::ops_registry())
                                            .apply(doc, batch, genko::core::Actor(seq["agent"].get<std::string>()));
                    got = Json::object();
                    got["ok"] = true;
                    got["applied"] = result.applied;
                    got["snapshot"] = genko::storage::snapshot(result.doc);
                    got["job_id"] = genko::core::new_id();
                    if (result.has_warnings) got["warnings"] = result.warnings;
                    doc = result.doc;
                } catch (const genko::core::ApplyError& error) {
                    got = Json::object({{"ok", false}, {"error", error.what()}});
                    ++refused;
                }
                if (w["reply"].contains("crash")) {
                    // (Python fails harder there: the C++ build refuses the op, the book stays as it was on both sides)
                    ++crashes;
                    if (got["ok"] == Json(true)) {
                        qWarning("%s: Python crashed (%s), C++ applied it", label.c_str(), w["reply"]["crash"].get<std::string>().c_str());
                        ++failures;
                    }
                } else if (w["nonfinite"].get<bool>()) {
                    if (got["ok"] == Json(true)) {
                        qWarning("%s: Python kept a number that is not finite, C++ did too", label.c_str());
                        ++failures;
                    }
                    diverged = true;  // (Python's book now holds what C++ refused: the rest cannot be compared)
                    break;
                } else {
                    std::string where;
                    if (!genko::test::strict_equal(got, w["reply"], &where)) {
                        qWarning("%s reply: %s", label.c_str(), where.c_str());
                        ++failures;
                        diverged = true;
                        break;
                    }
                }
                std::string where;
                if (!genko::test::strict_equal(genko::storage::snapshot(doc, true), w["full"], &where)) {
                    qWarning("%s full snapshot: %s", label.c_str(), where.c_str());
                    ++failures;
                    diverged = true;
                    break;
                }
                if (!genko::test::strict_equal(genko::test::state_of(doc), w["state"], &where)) {
                    qWarning("%s 3D state: %s", label.c_str(), where.c_str());
                    ++failures;
                    diverged = true;
                    break;
                }
            }
            if (diverged || !QFileInfo::exists(out + ".genko/project.json")) continue;
            const QString assets = out + ".cpp-assets";
            genko::storage::AssetStore store(genko::storage::path_from_utf8(assets.toStdString()));
            const std::string difference = genko::test::project_difference(genko::storage::project_payload_v4(doc, store),
                                                                           genko::test::read_json(out + ".genko/project.json"),
                                                                           out + ".genko", assets);
            if (!difference.empty()) {
                qWarning("sequence %zu saved: %s", n, difference.c_str());
                ++failures;
            }
        }
        std::string spread;
        for (const auto& [op, count] : ops) spread += op + ":" + std::to_string(count) + " ";
        qInfo("random: %d steps over %zu books, %d refused (%d where Python fails harder); %s", steps, books.size(), refused, crashes,
              spread.c_str());
        QCOMPARE(books.size(), std::size_t{15});
        QVERIFY(steps > 600);
        QCOMPARE(failures, 0);
    }
};

QTEST_GUILESS_MAIN(TestContract3dRandom)
#include "test_contract_3d_random.moc"
