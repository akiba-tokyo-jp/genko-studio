// The studio service where this build goes past Python (SPEC COMP-01a, M5②) and what its contract test cannot show:
// background jobs (a job's record from running to done or failed, waited for before the process ends; a job lost with
// its process; ids that are not a job's), a palette picture (a GIF) refused before anything is written where Python
// stores a palette PNG this build does not write yet, and the official export of formats::run handed to the studio
// (HumanService.export: its preflight) after Python's own refusals; adopt-drafts refused before the book is saved when
// its folder cannot be renamed.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "formats/exporting.hpp"
#include "render/not_yet_ported.hpp"
#include "service/book.hpp"
#include "service/evaluate.hpp"
#include "service/human_service.hpp"
#include "service/jobs.hpp"
#include "service/studio_service.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;
namespace jobs = genko::service::jobs;

namespace {

double now_seconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// The smallest GIF with a coloured palette (red and blue): Pillow opens it as a "P" picture.
std::string palette_gif() {
    const unsigned char bytes[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
                                   0xff, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof bytes);
}

std::size_t files_under(const fs::path& folder) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) n += it->is_regular_file() ? 1 : 0;
    return n;
}

}  // namespace

class TestService : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = genko::core::path_from_utf8((dir_.path() + QLatin1Char('/') + name).toStdString());
        fs::create_directories(out);
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    // A job that is not done within the wait is "running" (its id for export_status); when its work ends, the record
    // says done with the reply. wait_all() returns only when it has ended.
    void jobsRecordTheirWork() {
        const fs::path project = folder(QStringLiteral("jobs-a"));
        std::promise<void> go;
        std::shared_future<void> started = go.get_future().share();
        const Json reply = jobs::start(
            project, "export",
            [started] {
                started.wait();
                return Json{{"ok", true}, {"files", Json::array({"x.png"})}};
            },
            "ai:test", 0.05);
        QCOMPARE(reply["ok"], Json(true));
        QCOMPARE(reply["status"], Json("running"));
        QCOMPARE(reply["kind"], Json("export"));
        const std::string id = reply["job"].get<std::string>();
        QVERIFY(id.starts_with("job_"));
        Json status = jobs::status(project, Json(id));
        QCOMPARE(status["status"], Json("running"));
        QVERIFY(status["seconds"].is_number_float());
        QVERIFY(!status.contains("result"));
        go.set_value();
        jobs::wait_all();
        status = jobs::status(project, Json(id));
        QCOMPARE(status["status"], Json("done"));
        QCOMPARE(status["result"], (Json{{"ok", true}, {"files", Json::array({"x.png"})}}));
        const Json record = genko::core::parse_python_json(genko::storage::read_file(project / "studio" / "jobs" / (id + ".json")));
        QCOMPARE(record["actor"], Json("ai:test"));
        QVERIFY(record["finished"].get<double>() >= record["started"].get<double>());
    }

    // What the work threw is the job's error, as Python's f"{type(exc).__name__}: {exc}" says it; a reply that is not
    // ok is a failed job.
    void jobsSayWhatBroke() {
        const fs::path project = folder(QStringLiteral("jobs-b"));
        const Json thrown = jobs::start(project, "upscale", []() -> Json { throw genko::core::PyValueError("scale は 1〜4"); }, "ai:test", 30);
        QCOMPARE(thrown["ok"], Json(false));
        QCOMPARE(thrown["error"], Json("ValueError: scale は 1〜4"));
        const Json failed = jobs::start(project, "export", [] { return Json{{"ok", false}, {"error", "no"}}; }, "ai:test", 30);
        jobs::wait_all();
        QCOMPARE(jobs::status(project, thrown["job"])["status"], Json("failed"));
        QCOMPARE(jobs::status(project, failed["job"])["status"], Json("failed"));
        QCOMPARE(jobs::status(project, failed["job"])["result"]["error"], Json("no"));
        QCOMPARE(jobs::exception_text(std::make_exception_ptr(genko::core::PyUncaught("KeyError", "'x'"))), std::string("KeyError: 'x'"));
        QCOMPARE(jobs::exception_text(std::make_exception_ptr(genko::core::ApplyError("project locked: x"))), std::string("ApplyError: project locked: x"));
    }

    // A "running" job whose beat stopped (its process died) is lost, with what to do.
    void lostJobs() {
        const fs::path project = folder(QStringLiteral("jobs-c"));
        const double then = now_seconds() - 200;
        genko::storage::write_atomic(project / "studio" / "jobs" / "job_old.json",
                                     genko::core::dump_python(Json{{"id", "job_old"}, {"kind", "export"}, {"actor", "ai:x"}, {"status", "running"},
                                                                  {"started", then}, {"beat", then}, {"pid", 1}}));
        const Json status = jobs::status(project, Json("job_old"));
        QCOMPARE(status["status"], Json("lost"));
        QVERIFY(status.contains("hint"));
    }

    // Ids that are not a job's: Python's answers and its TypeError and AttributeError.
    void jobIdsAreChecked() {
        const fs::path project = folder(QStringLiteral("jobs-d"));
        for (const Json& id : {Json(""), Json("../x"), Json("a/b"), Json("a\\b"), Json(".hidden"), Json("job_none"), Json(nullptr), Json(0),
                               Json::array({"/"})}) {
            const Json status = jobs::status(project, id);
            QCOMPARE(status["ok"], Json(false));
            QCOMPARE(status["error"], Json("job " + genko::core::py_str(id) + " はない"));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::PyTypeError, jobs::status(project, Json(5)));
        QVERIFY_THROWS_EXCEPTION(genko::core::PyUncaught, jobs::status(project, Json::array({"a"})));
    }

    // adopt-drafts when studio/drafts.adopted is already there and cannot be replaced: refused before the book is saved
    // (Python saves the drafts into the book, then fails to rename the folder, and adopts them again next time).
    void adoptedDraftsAreCheckedFirst() {
        const fs::path root = folder(QStringLiteral("drafts"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"d.genko", "d", 1}), Json::object()).ok);
        const fs::path book = root / "d.genko";
        genko::storage::write_atomic(book / "studio" / "drafts" / "bible.json",
                                     genko::storage::read_file(genko::core::path_from_utf8(
                                         (genko::test::repo_root() + "/tests/fixtures/studio/demo4/bible.json").toStdString())));
        genko::storage::write_atomic(book / "studio" / "drafts.adopted" / "keep.txt", "already here");
        const std::int64_t before = genko::service::load_episode(book).revision;
        QVERIFY_THROWS_EXCEPTION(genko::core::PyUncaught, genko::service::adopt::adopt_drafts(book));
        QCOMPARE(genko::service::load_episode(book).revision, before);
        QVERIFY(fs::is_regular_file(book / "studio" / "drafts" / "bible.json"));
    }

    // import_image of a palette picture (Python stores a palette PNG): not_yet_ported, nothing written.
    void palettePicturesAreNotWritten() {
        const fs::path root = folder(QStringLiteral("palette"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"p.genko", "p", 1}), Json::object()).ok);
        const std::size_t before = files_under(root / "p.genko" / "assets");
        try {
            service.call("import_image", Json::array({"p.genko"}), Json{{"png_base64", genko::core::b64encode(palette_gif())}});
            QFAIL("a palette picture was stored");
        } catch (const genko::render::NotYetPorted& e) {
            QCOMPARE(e.element(), std::string("palette_png"));
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        QCOMPARE(files_under(root / "p.genko" / "assets"), before);
    }

    // formats::run's official export, handed to the studio by service::run_export after Python's own refusals: the
    // studio's preflight answers (not "not yet ported").
    void officialExportGoesThroughTheStudio() {
        const fs::path root = folder(QStringLiteral("official"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"o.genko", "o", 1}), Json{{"spec_preset", "doujin-a5"}}).ok);
        const fs::path book = root / "o.genko";
        const genko::core::Document doc = genko::service::load_episode(book);
        genko::formats::RunOptions options;
        options.official = true;
        options.actor = "human:leaf";
        const Json studio = genko::service::run_export(doc, book, "pdf", root / "out", options);
        QCOMPARE(studio["ok"], Json(false));
        QCOMPARE(studio["error"], Json("preflight で止まった"));
        QVERIFY(studio["errors"].is_array() && !studio["errors"].empty());
        QVERIFY(!fs::exists(root / "out"));
        // (Python's own refusals come first)
        const Json layers = genko::service::run_export(doc, book, "layers", root / "out", options);
        QVERIFY(layers["error"].get<std::string>().ends_with("は正式な書き出しに使えない"));
        // an agent cannot make it
        options.actor = "ai:agent";
        QVERIFY_THROWS_EXCEPTION(genko::core::ApplyError, genko::service::run_export(doc, book, "pdf", root / "out", options));
        // without the studio, formats::run alone still refuses it
        options.actor = "human:leaf";
        const Json alone = genko::formats::run(doc, book, "pdf", root / "out", options);
        QCOMPARE(alone["code"], Json("not_yet_ported"));
    }
};

QTEST_GUILESS_MAIN(TestService)
#include "test_service.moc"
