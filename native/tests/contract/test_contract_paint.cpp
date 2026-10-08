// The paint ops of M3④ (Python's ops._add_shape, ops._smudge and layerops.liquify) against Python's apply_ops: the
// cases of contract/paint_cases.json on the book `pyref_harness.py make-opsbook` makes (shapes drawn as lines, filled or
// both; a paint layer and the ink lines blurred, blended and pushed along; lines and pixels pushed, pinched, bloated
// and twirled; and their refusals): each step's reply, full snapshot and project.json payload (the pixels of every
// picture), and the book after the case saved and read back. Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "opsupport.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

// The steps of a case (ops_cases.json's format): "steps", or one step of "ops", "agent", "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

}  // namespace

class TestContractPaint : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void opsLikePython() {
        const QString pyenv = path("pyenv");
        const QString opsbook = path("opsbook.genko");
        const auto made = genko::test::harness({"make-opsbook", opsbook}, pyenv, 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/paint_cases.json");
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        const QString dir = path("ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = opsbook.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            if (cases[n]["ok"].get<bool>()) job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, pyenv, 1200000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(opsbook.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, int> same;  // op → cases the same as Python's
        int read_back = 0;
        int refused = 0;
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            Json records = genko::test::read_json(QString::fromStdString(jobs[n]["out"].get<std::string>()));
            Json reread;
            if (!records.empty() && records.back().contains("reread")) {
                reread = records.back();
                records.erase(records.size() - 1);
            }
            genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-store-%1").arg(n)).toStdString()));
            genko::core::Document last;
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &last);
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            if (records.back()["reply"]["ok"].get<bool>() != c["ok"].get<bool>()) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(records.back()["reply"]).substr(0, 400));
                continue;
            }
            bool ok = true;
            for (std::size_t s = 0; s < outcomes.size() && ok; ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    ok = false;
                }
            }
            if (!ok) continue;
            ++same[c["op"].get<std::string>()];
            if (!c["ok"].get<bool>()) {
                ++refused;
                continue;
            }
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [op, n] : same) qInfo("%s: %d cases the same as Python", op.c_str(), n);
        qInfo("refused as Python refuses: %d; saved and read back as they were: %d", refused, read_back);
        QVERIFY(failures.empty());
        QVERIFY(same["add_shape"] >= 18);
        QVERIFY(same["smudge"] >= 7);
        QVERIFY(same["liquify"] >= 9);
    }
};

QTEST_GUILESS_MAIN(TestContractPaint)
#include "test_contract_paint.moc"
