// F3 (ACCEPTANCE.md §2, FIXTURES.md): the 8-page offline making of tests/test_m4_pipeline.py — the bible, the script,
// the names, the fixed synthetic candidate pictures (tests/agents/images.py), adoption, the finishing and the export —
// on this build's products: Python's FakeAgent (tests/agents/fake_agent.py) through the MCP SDK's own client
// (mcp.Client over stdio) on `genko mcp`, and the person of test_m4_pipeline.py's ScriptedHuman answering its requests
// with `genko studio …` (a process each time), as tools/migration/pipeline_harness.py runs them; then the same run on
// Python's server and command line. No AI, no charges, no network: only the synthetic pictures.
//
// Compared, once normalised as mcp_harness.py normalises (paths, ids and the studio's ids made from them in the order
// met, times, a job's id; the lists ordered by random ids put in an order of their own): every call in order — each
// tool's reply and pictures (by their pixels, within ARCHITECTURE.md §9's drawing tolerance), each command and its
// reply and exit code —; `genko studio audit` and `genko studio stats` at the end (the tool log's times left out); the
// book at the end (project.json as Python's v3 writer writes it, its revision too, with its approvals) and its approval
// audit ({rev, actor, changes, via}); the files written out (the same names). Left out, and why:
//   - a candidates contact sheet's labels (the top 16 px of each tile): each is the candidate's id, random on both
//     sides; the thumbnails are compared;
//   - what is drawn from random ids (a balloon outline's wobble from its line's id, effect lines from the effect's id)
//     differs between any two runs, Python's own too (two Python runs write TIFFs of other pixels): so the person's PDF
//     and eight TIFFs and the agent's watermarked proof are compared byte for byte (but the times export_files.hpp
//     leaves out and the proof's creation time) with what Python writes from this build's book at the end
//     (pipeline_harness.py redraw), and finish_page's previews with Python's drawing of the finished pages;
//   - in those previews, the panels the finishing gave new effect lines: the preview draws them with ids made for it
//     alone (the change is made again, with new ids, when it is saved; Python's the same), so nothing can draw them
//     again; the finished page, effect lines and all, is compared in the files written out;
//   - export_proof is always a background job on MCP (its reply comes within 40 s, or the job does): the harness waits
//     for the job's record to end, asks export_status once, and keeps the tool's reply in the form it has when the job
//     ends in time — so a slow build (ASan, -O0) makes the same calls and tool log; that is one export_status call more
//     than test_m4_pipeline.py's run, on both sides.
// test_m4_pipeline.py's own checks hold on both: every page approved and finished, the characters approved with their
// sheet and face, the official export refused for test pictures and done with --allow-fixture, only the good pictures
// adopted (and enlarged), faces reported, the proof made before the export was asked for, only the person approved, few
// pictures per panel, the style taken from page 1. Nothing here depends on time (a background job is waited on until
// it ends, then asked after once): it holds in ASan and -O0 builds; the times are PERF-F's (test_perf_pipeline).
// Skipped without a Python that has the mcp extra ($GENKO_MCP_PYTHON), as test_contract_mcp is. GENKO_TEST_KEEP=1
// keeps both runs.

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

// Whether two pictures are the same drawing (ARCHITECTURE.md §9), and how far apart they are; the pixels `left_out`
// names are not compared (a contact sheet's labels, the candidates' random ids).
bool close(const QString& a_file, const QString& b_file, std::string& why, const std::function<bool(int x, int y)>& left_out = {}) {
    const auto a = genko::render::read_png(genko::test::read_bytes(a_file)).convert("RGBA");
    const auto b = genko::render::read_png(genko::test::read_bytes(b_file)).convert("RGBA");
    if (a.width() != b.width() || a.height() != b.height()) {
        why = "sizes differ";
        return false;
    }
    const std::string pa = a.tobytes();
    const std::string pb = b.tobytes();
    double total = 0;
    std::size_t far = 0;
    std::size_t counted = 0;
    for (std::size_t i = 0; i < pa.size(); i += 4) {
        const std::size_t pixel = i / 4;
        if (left_out && left_out(static_cast<int>(pixel % static_cast<std::size_t>(a.width())), static_cast<int>(pixel / static_cast<std::size_t>(a.width())))) {
            continue;
        }
        ++counted;
        int worst = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int d = std::abs(static_cast<int>(static_cast<unsigned char>(pa[i + c])) - static_cast<int>(static_cast<unsigned char>(pb[i + c])));
            total += d;
            worst = std::max(worst, d);
        }
        if (worst > 32) ++far;
    }
    const double mean = counted == 0 ? 0 : total / static_cast<double>(counted * 4);
    const double far_share = counted == 0 ? 0 : static_cast<double>(far) / static_cast<double>(counted);
    why = "mean " + std::to_string(mean) + ", pixels past 32: " + std::to_string(far_share * 100) + "% of " + std::to_string(counted);
    return mean <= 2.0 && far_share <= 0.01;
}

// A PDF without the time it was made (a proof is written with its /CreationDate and /ModDate: the same length, so the
// offsets stay).
std::string undated_pdf(std::string bytes) {
    for (const std::string key : {"/CreationDate (D:", "/ModDate (D:"}) {
        for (std::size_t at = bytes.find(key); at != std::string::npos; at = bytes.find(key, at + 1)) {
            for (std::size_t i = at + key.size(); i < bytes.size() && bytes[i] != ')'; ++i) bytes[i] = '0';
        }
    }
    return bytes;
}

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

// test_m4_pipeline.py's checks of test_eight_pages_from_bible_to_export_offline on one side's run (its book as Python's
// v3 writer writes it, its records): "" when they hold, else the first that does not.
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

class ContractPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (genko::test::mcp_python().isEmpty()) {
            QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON (a venv with uv.lock's mcp, pydantic, Pillow and numpy)");
        }
        QVERIFY(scratch_.isValid());
        if (qEnvironmentVariableIsSet("GENKO_TEST_KEEP")) {  // (both runs kept, to read what differs)
            scratch_.setAutoRemove(false);
            qInfo("kept in %s", qPrintable(scratch_.path()));
        }
    }

    void eightPagesLikePython() {
        Json runs[2];
        Json sides[2];
        QString dirs[2];
        genko::core::Document cpp_book;
        for (int side = 0; side < 2; ++side) {
            const QString dir = scratch_.path() + (side == 0 ? "/cpp" : "/py");
            dirs[side] = dir;
            QVERIFY(QDir().mkpath(dir));
            Json job = Json::object();
            job["server"] = side == 0 ? Json::object({{"kind", "cpp"}, {"genko", genko::test::genko_cli().toStdString()}})
                                      : Json::object({{"kind", "py"},
                                                      {"python", genko::test::mcp_python().toStdString()},
                                                      {"repo", genko::test::repo_root().toStdString()}});
            job["root"] = (dir + "/root").toStdString();
            job["config"] = (dir + "/config").toStdString();
            job["out"] = (dir + "/out").toStdString();
            job["agent"] = "ai:test";
            job["pages"] = 8;
            genko::test::write_bytes(dir + "/job.json", genko::core::dump_python(job));
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.remove(QStringLiteral("PYTHONPATH"));
            env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            const genko::test::Run run = genko::test::run(
                genko::test::mcp_python(),
                {genko::test::repo_root() + "/tools/migration/pipeline_harness.py", QStringLiteral("run"), dir + "/job.json", dir + "/run.json"}, env,
                6600000);
            QVERIFY2(run.finished && run.exit_code == 0,
                     (QString(side == 0 ? "this build: " : "Python: ") + QString::fromUtf8(run.err.right(6000))).toUtf8());
            // (the server's standard error comes through the harness's: a sanitizer's report there fails the run)
            for (const char* report : {"ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:"}) {
                QVERIFY2(!run.err.contains(report), (QString(side == 0 ? "this build: " : "Python: ") + QString::fromUtf8(run.err.right(6000))).toUtf8());
            }
            runs[side] = genko::test::read_json(dir + "/run.json");
            qInfo("%s: the making in %.1f s, %zu calls, %d work items", side == 0 ? "this build" : "Python", runs[side]["seconds"].get<double>(),
                  runs[side]["calls"].size(), runs[side]["agent"]["steps"].get<int>());

            // the book at the end, as Python's v3 writer writes it, and its approval audit
            Json book;
            Json audit = Json::array();
            if (side == 0) {
                const fs::path path = genko::storage::path_from_utf8((dir + "/root/" + kProject).toStdString());
                cpp_book = genko::storage::load_document(path).document;
                const genko::core::Document& doc = cpp_book;
                genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + "/store").toStdString()));
                book = genko::storage::project_payload_v4(doc, store);
                for (const char* key : {"min_reader", "writer", "book_id", "features"}) book.erase(key);
                book["version"] = 3;
                // (the page spec as written: the reader takes margins_mm as floats, as Python's from_dict does on its next
                // load, but a book not read back since keeps the numbers it was made with, as Python's does)
                const Json raw = genko::core::parse_python_json(genko::storage::read_file(path / "project.json"));
                if (raw.is_object() && raw.contains("spec") && raw["spec"].is_object()) book["spec"] = raw["spec"];
                for (const Json& e : genko::storage::journal::audit_entries(path)) {
                    if (genko::core::py_get(e, "kind") == "migrated") continue;
                    audit.push_back(Json::object({{"rev", genko::core::py_get(e, "rev")},
                                                  {"actor", genko::core::py_get(e, "actor")},
                                                  {"changes", genko::core::py_get(e, "changes")},
                                                  {"via", genko::core::py_get(e, "via")}}));
                }
            } else {
                book = runs[side]["python"]["book"];
                audit = runs[side]["python"]["audit"];
            }
            std::string problem;
            try {
                problem = pipeline_problem(book, runs[side], dir + "/root");
            } catch (const std::exception& error) {
                problem = std::string("the book or the records are not as test_m4_pipeline.py reads them: ") + error.what();
            }
            QVERIFY2(problem.empty(), (QString(side == 0 ? "this build: " : "Python: ") + QString::fromStdString(problem)).toUtf8());
            Json whole = Json::object({{"calls", runs[side]["calls"]}, {"end", runs[side]["end"]}, {"book", book}, {"audit", audit},
                                       {"root", runs[side]["root"]}, {"config", runs[side]["config"]}, {"out", runs[side]["out"]}});
            genko::test::write_bytes(dir + "/whole.json", genko::core::dump_python(whole));
            const genko::test::Run normal = genko::test::run(
                genko::test::mcp_python(),
                {genko::test::repo_root() + "/tools/migration/pipeline_harness.py", QStringLiteral("normal"), dir + "/whole.json", dir + "/normal.json"}, env,
                600000);
            QVERIFY2(normal.finished && normal.exit_code == 0, normal.err.right(4000).constData());
            sides[side] = genko::test::read_json(dir + "/normal.json");
        }
        // the revision the book ended at (a save each, the same on both)
        QCOMPARE(sides[0]["book"].value("revision", Json()), sides[1]["book"].value("revision", Json()));

        // the pictures, call by call: compared by their pixels, then left out of the records. A candidates' contact sheet
        // without its labels (the candidates' ids: random on both sides). finish_page's preview holds what is drawn from
        // random ids (effect lines from the effect's id, a balloon's outline from its line's), as do the files written
        // out: those are compared with Python's drawing of this build's book below.
        const Json& calls_cpp = sides[0]["calls"];
        QCOMPARE(calls_cpp.size(), sides[1]["calls"].size());
        std::size_t compared_pictures = 0;
        std::map<int, QString> finish_previews;  // page → this build's preview
        std::map<int, std::vector<std::string>> effect_panels;  // page → the panels its finishing gave effect lines
        for (std::size_t i = 0; i < calls_cpp.size(); ++i) {
            Json& mine = sides[0]["calls"][i];
            Json& theirs = sides[1]["calls"][i];
            if (!mine.contains("images") || !theirs.contains("images")) continue;
            QCOMPARE(mine["images"].size(), theirs["images"].size());
            const std::string tool = mine.value("tool", std::string());
            for (std::size_t j = 0; j < mine["images"].size(); ++j) {
                Json& a_png = mine["images"][j]["png"];
                Json& b_png = theirs["images"][j]["png"];
                const QString a = dirs[0] + "/run.json.images/" + QString::fromStdString(a_png["file"].get<std::string>());
                const QString b = dirs[1] + "/run.json.images/" + QString::fromStdString(b_png["file"].get<std::string>());
                if (tool == "finish_page") {
                    const int page = mine["args"]["page"].get<int>();
                    finish_previews[page] = a;
                    for (const Json& change : runs[0]["calls"][i]["reply"].value("changes", Json::array())) {
                        if (change.value("kind", std::string()) == "add_effect") effect_panels[page].push_back(change["frame_id"].get<std::string>());
                    }
                    a_png["file"] = b_png["file"] = "<redrawn>";
                    continue;
                }
                std::string why;
                const auto label = [](int, int y) { return y % 212 < 16; };  // (_contact_sheet: 192 × 208 tiles, the id written at the top)
                QVERIFY2(close(a, b, why, tool == "candidates" ? std::function<bool(int, int)>(label) : std::function<bool(int, int)>()),
                         ("call " + std::to_string(i) + " " + tool + ": " + a.toStdString() + " vs " + b.toStdString() + ": " + why).c_str());
                a_png["file"] = b_png["file"] = "<compared>";
                ++compared_pictures;
            }
        }
        QCOMPARE(static_cast<int>(finish_previews.size()), 8);
        // every call and its reply, in order
        for (std::size_t i = 0; i < calls_cpp.size(); ++i) {
            std::string where;
            QVERIFY2(genko::test::strict_equal(sides[0]["calls"][i], sides[1]["calls"][i], &where),
                     ("call " + std::to_string(i) + " at " + where + "\n  C++:    " + sides[0]["calls"][i].dump().substr(0, 3000) +
                      "\n  Python: " + sides[1]["calls"][i].dump().substr(0, 3000))
                         .c_str());
        }
        for (const char* part : {"end", "book", "audit"}) {
            std::string where;
            QVERIFY2(genko::test::strict_equal(sides[0][part], sides[1][part], &where), (std::string(part) + " at " + where).c_str());
        }
        // what was written out: the same files on both sides (the person's PDF and TIFFs, the agent's proof)
        QStringList written;
        for (const QString& folder : {QStringLiteral("/out"), QStringLiteral("/root/") + kProject + QStringLiteral("/studio/proofs")}) {
            QStringList mine;
            QStringList python;
            for (int side = 0; side < 2; ++side) {
                QDirIterator it(dirs[side] + folder, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) (side == 0 ? mine : python) << QDir(dirs[side] + folder).relativeFilePath(it.next());
            }
            mine.sort();
            python.sort();
            QCOMPARE(mine, python);
            for (const QString& rel : mine) written << folder + "/" + rel;
        }
        QCOMPARE(written.size(), 1 + 8 + 1);  // (the PDF, the eight TIFFs and the proof)

        // Python's drawing of this build's book (the ids, and what is drawn from them, the same): the finished pages
        // as finish_page showed them, the proof and the official exports, byte for byte but the times Python's files
        // carry
        const QString redraw = scratch_.path() + "/redraw";
        const fs::path copy = genko::storage::path_from_utf8((redraw + "/root/" + kProject).toStdString());
        std::error_code ec;
        fs::create_directories(copy.parent_path(), ec);
        fs::copy(genko::storage::path_from_utf8((dirs[0] + "/root/" + kProject).toStdString()), copy, fs::copy_options::recursive, ec);
        QVERIFY2(!ec, ec.message().c_str());
        for (const char* v4 : {"project.lock", "studio/journal.jsonl", "studio/audit.jsonl"}) fs::remove(copy / v4, ec);
        {
            genko::storage::AssetStore store(copy);
            Json payload = genko::storage::project_payload_v4(cpp_book, store);
            for (const char* key : {"min_reader", "writer", "book_id", "features"}) payload.erase(key);
            payload["version"] = 3;
            genko::test::write_bytes(QString::fromStdString(genko::storage::path_to_utf8(copy / "project.json")), genko::core::dump_python_indent2(payload));
        }
        QString proof_folder;
        {
            QDirIterator it(dirs[0] + "/root/" + kProject + "/studio/proofs", QDir::Dirs | QDir::NoDotAndDotDot);
            QVERIFY(it.hasNext());
            proof_folder = QFileInfo(it.next()).fileName();  // (r<revision>)
            QVERIFY(!it.hasNext());
        }
        Json redraw_job = Json::object({{"root", (redraw + "/root").toStdString()},
                                        {"config", (redraw + "/config").toStdString()},
                                        {"out", (redraw + "/out").toStdString()},
                                        {"pages", 8},
                                        {"proof_revision", proof_folder.mid(1).toInt()}});
        genko::test::write_bytes(redraw + "/job.json", genko::core::dump_python(redraw_job));
        {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.remove(QStringLiteral("PYTHONPATH"));
            env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            const genko::test::Run run = genko::test::run(
                genko::test::mcp_python(), {genko::test::repo_root() + "/tools/migration/pipeline_harness.py", QStringLiteral("redraw"), redraw + "/job.json"},
                env, 3600000);
            QVERIFY2(run.finished && run.exit_code == 0, run.err.right(6000).constData());
        }
        // (finish_page's preview draws the effect lines it adds with ids made for the preview alone — the change is made
        // again, with new ids, when it is saved; Python's the same —: those panels are left out of the preview, and the
        // finished page, effect lines and all, is compared in the files written out below)
        int panels_left_out = 0;
        for (const auto& [page, mine] : finish_previews) {
            std::string why;
            const QString theirs = redraw + QStringLiteral("/out/preview-p%1.png").arg(page, 3, 10, QLatin1Char('0'));
            const genko::core::Page& drawn = cpp_book.page(static_cast<std::size_t>(page - 1));
            const double px_per_mm = genko::render::read_png(genko::test::read_bytes(mine)).width() / drawn.spec.width_mm.value();
            std::vector<std::array<int, 4>> boxes;
            for (const std::string& id : effect_panels[page]) {
                const genko::core::Frame* frame = drawn.find_frame(id);
                QVERIFY2(frame != nullptr, id.c_str());
                const genko::core::Rect& r = frame->rect;
                boxes.push_back({static_cast<int>(std::floor(r.x.value() * px_per_mm)) - 3, static_cast<int>(std::floor(r.y.value() * px_per_mm)) - 3,
                                 static_cast<int>(std::ceil((r.x.value() + r.width.value()) * px_per_mm)) + 3,
                                 static_cast<int>(std::ceil((r.y.value() + r.height.value()) * px_per_mm)) + 3});
                ++panels_left_out;
            }
            const auto in_box = [&boxes](int x, int y) {
                return std::any_of(boxes.begin(), boxes.end(), [x, y](const std::array<int, 4>& b) { return x >= b[0] && y >= b[1] && x < b[2] && y < b[3]; });
            };
            QVERIFY2(close(mine, theirs, why, in_box), ("finish_page " + std::to_string(page) + ": " + mine.toStdString() + " vs " + theirs.toStdString() + ": " + why).c_str());
        }
        int same_files = 0;
        for (const QString& file : written) {
            const QString mine = dirs[0] + file;
            const QString name = QFileInfo(file).fileName();
            const QString theirs = file.startsWith(QStringLiteral("/out/")) ? redraw + file : redraw + QStringLiteral("/out/proof.pdf");
            QVERIFY2(QFileInfo::exists(theirs), qPrintable(theirs));
            QVERIFY2(undated_pdf(genko::test::exports::normalized(name, genko::test::read_bytes(mine))) ==
                         undated_pdf(genko::test::exports::normalized(name, genko::test::read_bytes(theirs))),
                     qPrintable(file + ": differs from Python's drawing of the same book (" + theirs + ")"));
            ++same_files;
        }
        qInfo("the same as Python's: %zu calls (%zu pictures), the book, its audit and approvals; Python's drawing of this build's book: "
              "the %zu finished pages (%d panels with new effect lines left out) and %d files written out, byte for byte",
              calls_cpp.size(), compared_pictures, finish_previews.size(), panels_left_out, same_files);
    }

private:
    QTemporaryDir scratch_;
};

QTEST_GUILESS_MAIN(ContractPipeline)
#include "test_contract_pipeline.moc"
