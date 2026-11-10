// F3 (ACCEPTANCE.md §2, FIXTURES.md): the 8-page offline making — the bible, the script, the names, the fixed
// synthetic candidate pictures (native/tests/support/agents/images.py), adoption, the finishing and the export — on
// this build's products: the FakeAgent (native/tests/support/agents/fake_agent.py) through the MCP SDK's own client
// (mcp.Client over stdio) on `genko mcp`, and a scripted person answering its requests with `genko studio …` (a
// process each time), as tools/migration/pipeline_harness.py runs them. No AI, no charges, no network: only the
// synthetic pictures.
//
// Checked: every page approved and finished, the characters approved with their sheet and face, the official export
// refused for test pictures and done with --allow-fixture, only the good pictures adopted (and enlarged), faces
// reported, the proof made before the export was asked for, only the person approved, few pictures per panel, the
// style taken from page 1; what was written out (the person's PDF and eight TIFFs, the agent's proof). Nothing here
// depends on time (a background job is waited on until it ends, then asked after once): it holds in ASan and -O0
// builds; the times are PERF-F's (test_perf_pipeline). Skipped without a Python that has the MCP SDK
// ($GENKO_MCP_PYTHON). GENKO_TEST_KEEP=1 keeps the run.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "export_files.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kProject = "story.genko";

// The leaf panels of a page of project.json (the frames' tree), in the page's own JSON.
void leaves(const Json& frame, std::vector<const Json*>& out) {
    const auto kids = frame.find("children");
    if (kids != frame.end() && kids->is_array() && !kids->empty()) {
        for (const Json& kid : *kids) leaves(kid, out);
    } else {
        out.push_back(&frame);
    }
}

std::vector<const Json*> leaves_of(const Json& page) {
    std::vector<const Json*> out;
    if (const auto frames = page.find("frames"); frames != page.end() && frames->is_array()) {
        for (const Json& frame : *frames) leaves(frame, out);
    }
    return out;
}

// A panel's candidate by its id, in the panel's own JSON.
const Json* candidate(const Json& panel, const Json& id) {
    const auto all = panel.find("candidates");
    if (all == panel.end() || !all->is_array()) return nullptr;
    for (const Json& c : *all) {
        if (c.value("id", Json()) == id) return &c;
    }
    return nullptr;
}

// The checks of the eight pages made offline on the run (its book's project.json payload, its records): "" when they
// hold, else the first that does not.
std::string pipeline_problem(const Json& book, const Json& run, const QString& root) {
    const Json& pages = book["pages"];
    if (pages.size() != 8) return "the book has " + std::to_string(pages.size()) + " pages";
    for (const Json& page : pages) {
        if (!page.value("name_ok", false) || !page.value("art_ok", false) || page.value("stage", std::string()) != "finish") {
            return "page " + page["index"].dump() + " is not approved and finished";
        }
    }
    for (const Json& c : book["bible"]["characters"]) {
        std::set<std::string> kinds;
        for (const Json& r : c.value("refs", Json::array())) kinds.insert(r.value("kind", std::string()));
        if (!c.value("locked", false) || !kinds.count("sheet") || !kinds.count("face")) return "character " + c["id"].dump() + " is not approved with its sheet";
    }
    // (d) the final export refuses test images, and says why
    const Json& refused = run["person"]["refused"];
    if (refused.empty() || refused[0].value("ok", true)) return "the export with test pictures was not refused";
    bool fixture = false;
    for (const Json& e : refused[0].value("errors", Json::array())) fixture = fixture || e.value("code", std::string()) == "fixture_image";
    if (!fixture) return "the refusal does not say fixture_image";
    // (c) with --allow-fixture the book comes out as PDF and TIFF
    const Json& exports = run["person"]["exports"];
    if (exports.size() != 2) return "not two exports";
    const Json& pdf = exports[0]["files"];
    const Json& tiffs = exports[1]["files"];
    if (pdf.size() != 1 || genko::test::read_bytes(QString::fromStdString(pdf[0].get<std::string>())).substr(0, 4) != "%PDF") return "no PDF";
    if (tiffs.size() != 8) return "not eight TIFFs";
    for (const Json& t : tiffs) {
        if (!QFileInfo(QString::fromStdString(t.get<std::string>())).isFile()) return "a TIFF is missing";
    }
    bool export_approved = false;
    for (const Json& a : book["studio"]["approvals"]) {
        export_approved = export_approved || (a.value("gate", std::string()) == "export" && a.value("by", std::string()) == "human:test");
        // (b) the agent never approved anything
        if (a.value("by", std::string()).rfind("human:", 0) != 0) return "an approval not by a person: " + a.dump();
    }
    if (!export_approved) return "the export was not approved by the person";
    // every panel went through request → import → review → adopt; only the good pictures adopted
    bool agent_region = false;
    const Json empty = Json::object();
    for (const Json& page : pages) {
        for (const Json* frame : leaves_of(page)) {
            const Json& panel = frame->contains("panel") ? (*frame)["panel"] : empty;
            if (panel.value("status", std::string()) != "adopted") return "a panel is not adopted";
            if (panel.value("attempts", Json::object()).value("images", Json()) != Json(2)) return "a panel's images are not 2";
            const Json* adopted = candidate(panel, panel.value("adopted", Json::object()).value("art", Json()));
            while (adopted != nullptr && adopted->value("upscaled", Json()).is_object()) adopted = candidate(panel, adopted->value("parent", Json()));
            if (adopted == nullptr) return "the adopted art is not a candidate";
            if (adopted->value("origin", Json::object()).value("model", std::string()) != "fixture-good") return "a decoy was adopted";
            if (adopted->value("review", Json::object()).value("score", Json()) != Json(0.9)) return "the adopted art's review is not 0.9";
            if (adopted->value("mapping", Json::object()).value("pad_mm", Json()) != Json(3.0)) return "the adopted art's pad is not 3.0";
            for (const Json& r : panel.value("regions", Json::array())) agent_region = agent_region || r.value("source", std::string()) == "agent";
        }
    }
    if (!agent_region) return "no region reported by the agent";
    // the agent exported a watermarked proof before asking for the export
    QDirIterator proofs(root + "/" + kProject + "/studio/proofs", {QStringLiteral("proof.pdf")}, QDir::Files, QDirIterator::Subdirectories);
    if (!proofs.hasNext()) return "no proof.pdf";
    if (run["agent"].value("log", 0) <= 50) return "the agent did too little";
    // only people changed approvals, and few images per panel
    const Json& audit = run["end"]["audit"];
    const Json by_actor = audit.value("changes_by_actor", Json::object());
    if (!audit.value("ok", false) || !by_actor.is_object() || by_actor.size() != 1 || !by_actor.contains("human:test")) {
        return "the audit: " + audit.dump().substr(0, 600);
    }
    const Json& stats = run["end"]["stats"];
    if (!(stats["images_per_adopted_panel"].value("median", 100.0) <= 8)) return "too many images per panel";
    if (!(stats["tool_calls"].value("total", 0) > 100)) return "too few tool calls";
    // the pilot page was approved first and fixed the style for the rest
    const Json locked = book["studio"]["style"].value("locked", Json::object());
    if (locked.value("page", Json()) != Json(1) || locked.value("tool", Json()) != Json("fixture")) return "the style was not fixed from page 1";
    return {};
}

}  // namespace

// The time the test has (CMakeLists: ctest's TIMEOUT 7200 s, QTEST_FUNCTION_TIMEOUT 7100 s): the making has 2300 s (the
// harness stops itself there with what was asked last — run.json.partial.json — and where each thread was; its process
// is given 120 s more).
constexpr int kRunLimitS = 2300;
constexpr int kGraceS = 120;

class ContractPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (genko::test::mcp_python().isEmpty()) {
            QSKIP("no Python with the MCP SDK: set GENKO_MCP_PYTHON (a venv of tools/migration/mcp_reference_requirements.txt)");
        }
        QVERIFY(scratch_.isValid());
        if (qEnvironmentVariableIsSet("GENKO_TEST_KEEP")) {  // (the run kept, to read it)
            scratch_.setAutoRemove(false);
            qInfo("kept in %s", qPrintable(scratch_.path()));
        }
    }

    void eightPagesOffline() {
        const QString dir = scratch_.path() + "/cpp";
        QVERIFY(QDir().mkpath(dir));
        Json job = Json::object();
        job["server"] = Json::object({{"kind", "cpp"}, {"genko", genko::test::genko_cli().toStdString()}});
        job["root"] = (dir + "/root").toStdString();
        job["config"] = (dir + "/config").toStdString();
        job["out"] = (dir + "/out").toStdString();
        job["agent"] = "ai:test";
        job["pages"] = 8;
        job["time_limit_s"] = kRunLimitS;
        genko::test::write_bytes(dir + "/job.json", genko::core::dump_python(job));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("PYTHONPATH"));
        env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        const genko::test::Run run = genko::test::run(
            genko::test::mcp_python(),
            {genko::test::repo_root() + "/tools/migration/pipeline_harness.py", QStringLiteral("run"), dir + "/job.json", dir + "/run.json"}, env,
            (kRunLimitS + kGraceS) * 1000);
        QVERIFY2(run.finished && run.exit_code == 0, QString::fromUtf8(run.err.right(6000)).toUtf8());
        // (the server's standard error comes through the harness's: a sanitizer's report there fails the run)
        for (const char* report : {"ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:"}) {
            QVERIFY2(!run.err.contains(report), QString::fromUtf8(run.err.right(6000)).toUtf8());
        }
        const Json made = genko::test::read_json(dir + "/run.json");
        qInfo("the making in %.1f s, %zu calls, %d work items", made["seconds"].get<double>(), made["calls"].size(), made["agent"]["steps"].get<int>());

        // the book at the end (its project.json payload; the page spec as written: the reader takes margins_mm as floats,
        // a book not read back since keeps the numbers it was made with)
        const fs::path path = genko::storage::path_from_utf8((dir + "/root/" + kProject).toStdString());
        const genko::core::Document doc = genko::storage::load_document(path).document;
        genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + "/store").toStdString()));
        Json book = genko::storage::project_payload_v4(doc, store);
        const Json raw = genko::core::parse_python_json(genko::storage::read_file(path / "project.json"));
        if (raw.is_object() && raw.contains("spec") && raw["spec"].is_object()) book["spec"] = raw["spec"];
        std::string problem;
        try {
            problem = pipeline_problem(book, made, dir + "/root");
        } catch (const std::exception& error) {
            problem = std::string("the book or the records are not as the making leaves them: ") + error.what();
        }
        QVERIFY2(problem.empty(), problem.c_str());

        // what was written out: the person's PDF and eight TIFFs, the agent's proof
        QStringList written;
        for (const QString& folder : {QStringLiteral("/out"), QStringLiteral("/root/") + kProject + QStringLiteral("/studio/proofs")}) {
            QDirIterator it(dir + folder, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) written << it.next();
        }
        QCOMPARE(written.size(), 1 + 8 + 1);
    }

private:
    QTemporaryDir scratch_;
};

QTEST_GUILESS_MAIN(ContractPipeline)
#include "test_contract_pipeline.moc"
