// The studio ops of M5 (Python's genko/studio/studio_ops.py, which ops.apply_ops dispatches its STUDIO_OPS to: set_studio,
// the bible's characters, locations and props, the script and page plans, panel briefs, regions and references, reviews,
// approvals and their revocation, chat approvals, tickets, requests, candidates and their adoption as placed art,
// placement, set_finish, set_layout, proposals; and set_bible, add_ticket and set_ticket of ops._apply_one) against
// Python's apply_ops: the cases of contract/studio_cases.json on the book `studio_harness.py make-studiobook` makes (its
// pictures by name: $A1 … from the book's assets.json), each step's reply, full snapshot and project.json payload, and the
// book after the case saved and read back; the human-only rules (approve, revoke, reopen, reject, resolve a proposal,
// allow chat approvals, a policy, an unlock, a pinned field, a person's region, skip, a prompt override) refused for an
// agent as Python refuses them. A case marked "cpp" is one this build refuses on purpose (a number Python makes from
// text, "inf" or "nan", that it would keep): C++ must refuse it with that code, its error holding "cpp_says".
//
// savedChangesLikePython: the same ops saved one batch at a time as `genko apply --agent` saves them, and Undo / Redo by
// actor as `genko undo` / `genko redo` do them — this build's command line on the book converted to v4, Python's on the v3
// book (studio_harness.py's saved job) —: each step's success or error and the approval changes it recorded in
// studio/audit.jsonl ({actor, changes, via}: an agent cannot undo a person's approval, nor another's change; a person
// can, and the audit says so).
//
// studioFilesLikePython: the studio's own files and checks (genko/studio: presence, toollog, claims, genreq, jsonutil,
// schemas, jsonschema_lite, state, tools_registry; this build's studio/) called one after another on each side's
// folders: each call's result or error, and the files after it.
//
// placedPicturesLikePython: placed pictures drawn (render._placed_raster, _finish_placed; psd's placed layers) on the book
// `studio_harness.py make-placedbook` makes, page by page, layer by layer and in the exports, pixel for pixel and file
// for file. Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <string>

#include "core/base64.hpp"
#include "core/clock.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "export_files.hpp"
#include "formats/export.hpp"
#include "opsupport.hpp"
#include "render/brushes.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "studio/files.hpp"
#include "studio/schemas.hpp"
#include "studio/state.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

// tools/migration/studio_harness.py <args>, run by the Python reference.
genko::test::Run studio_harness(const QStringList& args, const QString& scratch, int timeout_ms = 1800000) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/studio_harness.py")};
    full += args;
    return genko::test::run(genko::test::python_ref(), full, genko::test::python_env(scratch), timeout_ms);
}

// The steps of a case: "steps", or one step of "ops", "agent", "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

// Every text of `value` (and every key) with the book's pictures put in for their names ($A1 → sha256:…), the longest
// names first.
Json placed(const Json& value, const std::map<std::string, std::string, std::greater<>>& refs) {
    const auto put = [&refs](std::string text) {
        for (const auto& [name, ref] : refs) {
            const std::string key = "$" + name;
            for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + ref.size())) {
                text.replace(at, key.size(), ref);
            }
        }
        return text;
    };
    if (value.is_string()) return put(value.get<std::string>());
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(placed(item, refs));
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) out[put(key)] = placed(item, refs);
        return out;
    }
    return value;
}

// The studio's files under `project`/studio ("studio/…") and `config` ("config/…") as the harness's files job reads them:
// each file's text ("<bytes base64>" when it is not UTF-8).
Json studio_files(const QString& project, const QString& config) {
    Json files = Json::object();
    for (const auto& [prefix, folder] : {std::pair{QStringLiteral("studio"), project + "/studio"}, std::pair{QStringLiteral("config"), config}}) {
        QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        std::vector<QString> paths;
        while (it.hasNext()) paths.push_back(it.next());
        std::sort(paths.begin(), paths.end());
        for (const QString& file : paths) {
            const std::string bytes = genko::test::read_bytes(file);
            const std::string rel = (prefix + "/" + QDir(folder).relativeFilePath(file)).toStdString();
            files[rel] = genko::core::utf8_error(bytes) ? "<bytes " + genko::core::b64encode(bytes) + ">" : bytes;
        }
    }
    return files;
}

// Python's exception type for an error of this build's studio files (as the harness names them).
std::string python_type(const genko::core::Error& error) {
    if (error.code() == "type") return "TypeError";
    if (error.code() == "json") return "JSONDecodeError";
    if (error.code() == "key") return "KeyError";
    return "ValueError";
}

Json others_json(const std::vector<genko::studio::Other>& others) {
    Json out = Json::array();
    for (const auto& other : others) out.push_back(Json::object({{"actor", other.actor}, {"minutes_ago", other.minutes_ago}}));
    return out;
}

Json issues_json(const std::vector<genko::studio::Issue>& issues) {
    Json out = Json::array();
    for (const auto& issue : issues) out.push_back(issue.to_json());
    return out;
}

Json optional_json(const std::optional<Json>& value) { return value ? *value : Json(); }

// One call of the harness's files job made with this build's studio (studio/files.hpp, schemas.hpp, state.hpp).
Json studio_call(const Json& call, const QString& project, const QString& config, const genko::core::Document& book) {
    namespace st = genko::studio;
    const std::string fn = call["fn"].get<std::string>();
    const Json args = call.value("args", Json::object());
    const auto dir = genko::storage::path_from_utf8(project.toStdString());
    const auto conf = genko::storage::path_from_utf8(config.toStdString());
    const auto text = [&args](const char* key) { return args[key].get<std::string>(); };
    const auto optional_text = [&args](const char* key) {
        return args.contains(key) && args[key].is_string() ? std::optional<std::string>(args[key].get<std::string>()) : std::nullopt;
    };
    Json result;
    if (fn == "write") {
        genko::test::write_bytes(project + "/" + QString::fromStdString(text("path")), text("text"));
    } else if (fn == "remove") {
        const QString target = project + "/" + QString::fromStdString(text("path"));
        if (QFileInfo(target).isDir()) {
            QDir(target).removeRecursively();
        } else {
            QFile::remove(target);
        }
    } else if (fn == "presence.actor_for") {
        result = st::actor_for(text("base"), optional_text("session"));
    } else if (fn == "presence.others") {
        result = others_json(st::others(dir, text("actor"), args["now"].get<double>()));
    } else if (fn == "presence.check") {
        result = others_json(st::check(dir, text("actor"), args["now"].get<double>()));
    } else if (fn == "presence.touch") {
        st::touch(dir, text("actor"), args["now"].get<double>());
    } else if (fn == "toollog.record") {
        const std::optional<Json> given = args.contains("result") && !args["result"].is_null() ? std::optional<Json>(args["result"]) : std::nullopt;
        st::record_tool(dir, text("actor"), text("tool"), args.value("kwargs", Json::object()), given, optional_text("error"),
                        args.value("ms", 0.0), args["at"].get<double>());
    } else if (fn == "toollog.read") {
        result = Json::array();
        for (Json& line : st::read_tools(dir)) result.push_back(std::move(line));
    } else if (fn == "claims.holder") {
        const auto who = st::holder(dir, text("item"), args["now"].get<double>());
        result = who ? Json(*who) : Json();
    } else if (fn == "claims.claim") {
        result = st::claim(dir, text("item"), text("actor"), args["now"].get<double>());
    } else if (fn == "claims.release") {
        st::release(dir, text("item"), text("actor"), args["now"].get<double>());
    } else if (fn == "genreq.read") {
        result = optional_json(st::read_request(dir, text("request_id")));
    } else if (fn == "genreq.write") {
        std::map<std::string, std::string> files;
        const Json given = args.value("files", Json::object());
        for (const auto& [name, data] : given.items()) files[name] = genko::core::a2b_base64(data.get<std::string>());
        const auto folder = st::write_request(dir, args["request"], files);
        result = genko::storage::path_to_utf8(std::filesystem::relative(folder, dir));
    } else if (fn == "jsonutil.content_hash") {
        result = st::content_hash(args["value"]);
    } else if (fn == "jsonutil.canonical_json") {
        result = st::canonical_json(args["value"]);
    } else if (fn == "jsonutil.write_json") {
        st::write_json(dir / genko::storage::path_from_utf8(text("path")), args["value"]);
    } else if (fn == "jsonutil.read_json") {
        result = st::read_json(dir / genko::storage::path_from_utf8(text("path")), args.value("default", Json()));
    } else if (fn == "schemas.validate") {
        const Json& schema = st::schema(text("schema"));
        result = issues_json(st::validate(st::fill_nulls(args["data"], schema), schema));
    } else if (fn == "schemas.fill_nulls") {
        result = st::fill_nulls(args["data"], st::schema(text("schema")));
    } else if (fn == "schemas.schema") {
        result = st::schema(text("schema"));
    } else if (fn == "jsonschema_lite.validate") {
        result = issues_json(st::validate(args["data"], args["schema"]));
    } else if (fn == "jsonschema_lite.conservative_problems") {
        result = Json::array();
        for (const std::string& problem : st::conservative_problems(args["schema"])) result.push_back(problem);
    } else if (fn == "state.bible") {
        result = optional_json(st::bible(book));
    } else if (fn == "state.script") {
        result = optional_json(st::script(book));
    } else if (fn == "state.name") {
        result = optional_json(st::name(book, genko::core::Num(args["page"].get<std::int64_t>())));
    } else if (fn == "state.name_review") {
        result = optional_json(st::name_review(book, genko::core::Num(args["page"].get<std::int64_t>())));
    } else if (fn == "state.names") {
        result = Json::object();
        for (const auto& [index, plan] : st::names(book)) result[index.repr()] = plan;
    } else if (fn == "state.open_tickets") {
        result = Json::array();
        for (Json& ticket : st::open_tickets(book, optional_text("kind"))) result.push_back(std::move(ticket));
    } else if (fn == "state.page_fixes") {
        result = Json::array();
        for (Json& ticket : st::page_fixes(book, genko::core::Num(args["page"].get<std::int64_t>()))) result.push_back(std::move(ticket));
    } else if (fn == "state.written_look") {
        result = st::written_look(args["character"]);
    } else if (fn == "tools_registry.load") {
        result = st::load_tools(conf);
    } else if (fn == "tools_registry.get") {
        const auto [id, spec] = st::get_tool(conf, optional_text("tool_id"));
        result = Json::array({id ? Json(*id) : Json(), spec});
    } else if (fn == "tools_registry.set_tool") {
        result = st::set_tool(conf, text("tool_id"), args["spec"]);
    } else if (fn == "tools_registry.remove") {
        result = st::remove_tool(conf, text("tool_id"));
    } else {
        throw std::runtime_error("unknown call " + fn);
    }
    return result;
}

}  // namespace

// Where two pictures differ ("" when they are the same: the same mode, size and pixels).
std::string picture_difference(const genko::render::Image& got, const genko::render::Image& want) {
    if (got.mode() != want.mode()) return "mode " + std::string(got.mode()) + ", Python's " + std::string(want.mode());
    if (got.size() != want.size()) {
        return "size " + std::to_string(got.width()) + "×" + std::to_string(got.height()) + ", Python's " + std::to_string(want.width()) +
               "×" + std::to_string(want.height());
    }
    const std::string a = got.tobytes();
    const std::string b = want.tobytes();
    if (a == b) return {};
    std::size_t first = 0;
    while (a[first] == b[first]) ++first;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) count += a[i] != b[i] ? 1 : 0;
    const std::size_t pixel = first / static_cast<std::size_t>(got.bands());
    return std::to_string(count) + " bytes differ, the first at (" + std::to_string(pixel % static_cast<std::size_t>(got.width())) + ", " +
           std::to_string(pixel / static_cast<std::size_t>(got.width())) + "): " + std::to_string(static_cast<unsigned char>(a[first])) +
           ", Python's " + std::to_string(static_cast<unsigned char>(b[first]));
}

class TestContractStudio : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString book_;
    std::map<std::string, std::string, std::greater<>> refs_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        book_ = path("studio.genko");
        const auto made = studio_harness({"make-studiobook", book_}, path("pyenv"), 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json assets = genko::test::read_json(book_ + "/assets.json");
        for (const auto& [name, ref] : assets.items()) refs_[name] = ref.get<std::string>();
        QVERIFY(refs_.size() >= 10);
        QVERIFY(QFile::remove(book_ + "/assets.json"));  // (not part of the book)
    }

    void studioOpsLikePython() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/studio_cases.json");
        const Json cases = placed(file["cases"], refs_);
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        const std::string clock = file["clock"].get<std::string>();
        const QString dir = path("ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = book_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["clock"] = clock;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            if (cases[n]["ok"].get<bool>() && !cases[n].contains("cpp")) {
                job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            }
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = studio_harness({"batch", dir + "/jobs.json"}, path("pyenv"), 3600000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(book_.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        const genko::core::ScopedClock fixed(clock);
        std::map<std::string, std::pair<int, int>> same;  // op → (successes, refusals) the same as Python's
        int read_back = 0;
        int refused_here = 0;
        int uncaught = 0;
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const std::string op = c["op"].get<std::string>();
            const bool expect_ok = c["ok"].get<bool>();
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
            // the case does what it says: its last step succeeds or fails in Python as marked, and a failure is the op's
            const Json& last_reply = records.back()["reply"];
            if (last_reply["ok"].get<bool>() != expect_ok) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(last_reply).substr(0, 400));
                continue;
            }
            if (!expect_ok && !last_reply.contains("uncaught")) {
                const std::string error = last_reply["error"].get<std::string>();
                if (error.find("] " + op + ": ") == std::string::npos && error.find("] for_pages: ") == std::string::npos) {
                    failures.push_back(name + ": fails in another op: " + error.substr(0, 300));
                    continue;
                }
            }
            if (c.contains("cpp")) {  // refused here on purpose
                const auto& final = outcomes.back();
                const std::string error = final.reply.value("error", std::string());
                if (final.code != c["cpp"].get<std::string>() || error.find(c["cpp_says"].get<std::string>()) == std::string::npos) {
                    failures.push_back(name + ": C++ should refuse it with " + c["cpp"].get<std::string>() + " (" +
                                       c["cpp_says"].get<std::string>() + "), gave " +
                                       genko::core::dump_python(final.reply).substr(0, 400) + " (" + final.code + ")");
                    continue;
                }
                // (and the steps before it as Python's)
                bool before_ok = true;
                for (std::size_t s = 0; s + 1 < outcomes.size() && before_ok; ++s) {
                    const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                    if (!diff.empty()) {
                        failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                        before_ok = false;
                    }
                }
                if (before_ok) ++refused_here;
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
            if (last_reply.contains("uncaught")) ++uncaught;
            (expect_ok ? same[op].first : same[op].second) += 1;
            if (!expect_ok || c.contains("dry")) continue;
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [op, n] : same) qInfo("%s: %d successes and %d refusals the same as Python", op.c_str(), n.first, n.second);
        qInfo("saved and read back as they were: %d; refused here on purpose: %d; Python's own exceptions (python_error): %d",
              read_back, refused_here, uncaught);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        // every op of the family, its successes and its refusals
        for (const char* op : {"set_studio", "set_style_catalog", "upsert_character", "delete_character", "upsert_location",
                               "delete_location", "upsert_prop", "delete_prop", "set_script", "set_page_plan", "set_panel",
                               "record_review", "approve", "allow_chat_approval", "revoke", "request_approval", "request_fix",
                               "add_region", "edit_region", "delete_region", "replace_regions", "bind_ref", "unbind_ref",
                               "register_assets", "attach_reference", "open_request", "close_request", "import_candidates",
                               "review_candidates", "set_candidate", "adopt_candidate", "unadopt", "set_placement",
                               "place_asset", "set_finish", "ask_human", "reject_sheet", "withdraw_candidates", "set_layout",
                               "propose", "resolve_proposal", "resolve_ticket", "reopen_ticket", "set_bible", "add_ticket",
                               "set_ticket"}) {
            const auto got = same[op];
            QVERIFY2(got.first >= 1 && got.second >= 1, (std::string(op) + ": " + std::to_string(got.first) + " successes, " +
                                                          std::to_string(got.second) + " refusals matched").c_str());
        }
        QVERIFY2(read_back >= 200, std::to_string(read_back).c_str());
        QVERIFY(refused_here >= 6);
        QVERIFY(uncaught >= 8);
    }

    // The studio's approvals saved batch by batch and undone or redone by actor, as `genko apply --agent`, `genko undo
    // --as` and `genko redo --as` do them: this build's command line on the studio book converted to v4, Python's
    // (studio_harness.py's saved job, which saves as Python's `genko apply` does) on the v3 book. Each step's success or
    // error (an Undo or Redo: its kind), and the approval changes it added to studio/audit.jsonl ({actor, changes, via}).
    void savedChangesLikePython() {
        const std::string pg2 = "pg_000000000007";
        const Json steps = Json::parse(R"([
            {"ops": [{"op": "set_panel", "page": 4, "frame_id": "000000000018", "set": {"shot": "CU"}}], "agent": "ai:hermes"},
            {"ops": [{"op": "approve", "gate": "name", "page": 2}], "agent": "human:作者"},
            {"restore": "ai:hermes"},
            {"restore": "ai:hermes", "force": true},
            {"restore": "human:作者"},
            {"restore": "ai:hermes", "redo": true},
            {"restore": "human:作者", "redo": true},
            {"ops": [{"op": "approve", "gate": "sheet", "character_id": "c-yuki", "candidate_id": "cd_y1"},
                     {"op": "approve", "gate": "export"}], "agent": "human:作者"},
            {"ops": [{"op": "revoke", "gate": "sheet", "character_id": "c-ken", "reason": "描き直す"}], "agent": "human:作者"},
            {"ops": [{"op": "upsert_character", "character": {"id": "c-ken", "name": "健"}}], "agent": "ai:hermes"},
            {"ops": [{"op": "approve", "gate": "bible"}], "agent": "ai:hermes"},
            {"ops": [{"op": "approve", "gate": "art", "page": 1}], "agent": "human:作者"},
            {"restore": "ai:hermes"},
            {"restore": "ai:hermes"},
            {"restore": "human:作者", "force": true},
            {"ops": [{"op": "revoke", "gate": "art", "page": 6}, {"op": "revoke", "gate": "name", "page": 6}], "agent": "genko"},
            {"restore": "legacy:unknown", "force": true},
            {"restore": "human", "force": true},
            {"ops": [{"op": "approve", "gate": "export", "via": "ai:hermes", "message": "出していいよ"}], "agent": "human:作者"},
            {"ops": [{"op": "resolve_ticket", "id": "000000000a10", "note": "直した"}], "agent": "ai:hermes"},
            {"restore": "ai:other"},
            {"restore": "ai:hermes"}
        ])");
        (void)pg2;
        // Python
        const QString dir = path("saved");
        QDir().mkpath(dir);
        Json job = Json::object();
        job["op"] = "saved";
        job["book"] = book_.toStdString();
        job["dest"] = (dir + "/py.genko").toStdString();
        job["steps"] = steps;
        job["out"] = (dir + "/py.json").toStdString();
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(Json::array({job})));
        const auto ran = studio_harness({"batch", dir + "/jobs.json"}, path("pyenv"), 1800000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(dir + "/py.json");
        QCOMPARE(records.size(), steps.size());
        // this build's command line, on the book converted to v4
        const QString v4 = dir + "/cpp.genko";
        const auto migrated = genko::test::run_genko({"migrate", book_, v4});
        QVERIFY2(migrated.finished && migrated.exit_code == 0, (migrated.out + migrated.err).constData());
        const auto audit_lines = [&v4] {
            std::vector<Json> out;
            QFile file(v4 + "/studio/audit.jsonl");
            if (!file.open(QIODevice::ReadOnly)) return out;
            for (const QByteArray& line : file.readAll().split('\n')) {
                if (!line.trimmed().isEmpty()) out.push_back(genko::core::parse_python_json(line.toStdString()));
            }
            return out;
        };
        int saved = 0, refused = 0, restored = 0, audited = 0;
        for (std::size_t n = 0; n < steps.size(); ++n) {
            const Json& step = steps[n];
            const std::size_t before = audit_lines().size();
            genko::test::Run run;
            if (step.contains("restore")) {
                QStringList args{step.value("redo", false) ? "redo" : "undo", v4, "--as", QString::fromStdString(step["restore"].get<std::string>())};
                if (step.value("force", false)) args << "--force";
                run = genko::test::run_genko(args);
            } else {
                const QString ops = dir + QStringLiteral("/ops-%1.json").arg(n);
                genko::test::write_bytes(ops, genko::core::dump_python(step["ops"]));
                run = genko::test::run_genko({"apply", v4, ops, "--agent", QString::fromStdString(step["agent"].get<std::string>())});
            }
            QVERIFY2(run.finished, std::to_string(n).c_str());
            const Json reply = genko::test::one_line(run.out);
            const Json& want = records[n]["reply"];
            const std::string where = "step " + std::to_string(n) + ": C++ " + genko::core::dump_python(reply).substr(0, 500) +
                                      ", Python " + genko::core::dump_python(want).substr(0, 500);
            QVERIFY2(reply.is_object() && reply.value("ok", false) == want.value("ok", false), where.c_str());
            QCOMPARE(run.exit_code == 0, want.value("ok", false));
            if (!want.value("ok", false)) {
                QVERIFY2(reply["error"] == want["error"], where.c_str());
                ++refused;
            } else if (step.contains("restore")) {
                QVERIFY2(reply["kind"] == want["kind"], where.c_str());
                ++restored;
            } else {
                QVERIFY2(reply["applied"] == want["applied"], where.c_str());
                ++saved;
            }
            // the approval changes the step recorded
            const std::vector<Json> lines = audit_lines();
            Json added = Json::array();
            for (std::size_t k = before; k < lines.size(); ++k) {
                Json line = Json::object();
                for (const char* key : {"actor", "changes", "via"}) {
                    if (lines[k].contains(key)) line[key] = lines[k][key];
                }
                added.push_back(std::move(line));
            }
            std::string difference;
            QVERIFY2(genko::test::strict_equal(added, records[n]["audit"], &difference),
                     ("step " + std::to_string(n) + " audit: " + difference + " — C++ " + genko::core::dump_python(added) + ", Python " +
                      genko::core::dump_python(records[n]["audit"])).c_str());
            audited += static_cast<int>(added.size());
        }
        const std::string problems = genko::test::book_problems(v4);
        QVERIFY2(problems.empty(), problems.c_str());
        qInfo("saved: %d, undone or redone: %d, refused: %d, audit lines: %d — each as Python's", saved, restored, refused, audited);
        QVERIFY(saved >= 8 && restored >= 5 && refused >= 6 && audited >= 8);
    }

    // The studio's own files and checks (studio/presence.json, studio/logs/tools.jsonl, studio/claims/, studio/requests/,
    // <config>/tools.json; jsonutil, the input schemas and jsonschema_lite, the state read from a book) against Python's
    // genko.studio modules: the same calls one after another on each side's folders, each call's result or error and the
    // files on disk after it. (A brace inside a text is written as the JSON escape for U+007B: moc counts the braces
    // in a raw string.)
    void studioFilesLikePython() {
        const Json calls = Json::parse(R"__([
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": "9204"}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": " telegram-9204 "}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": null}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": ""}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": "a b"}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": "12345678901234567890123456789012345678901"}},
            {"fn": "presence.actor_for", "args": {"base": "ai:hermes", "session": "テスト"}},
            {"fn": "presence.others", "args": {"actor": "ai:a", "now": 1000000000.0}},
            {"fn": "presence.touch", "args": {"actor": "ai:a", "now": 1000000000.5}},
            {"fn": "presence.touch", "args": {"actor": "ai:b/会話", "now": 1000000100.25}},
            {"fn": "presence.others", "args": {"actor": "ai:a", "now": 1000000200.0}},
            {"fn": "presence.check", "args": {"actor": "ai:a", "now": 1000000200.0}},
            {"fn": "presence.check", "args": {"actor": "ai:a", "now": 1000000300.0}},
            {"fn": "presence.touch", "args": {"actor": "ai:c", "now": 1000000400.0}},
            {"fn": "presence.check", "args": {"actor": "ai:a", "now": 1000000500.0}},
            {"fn": "presence.check", "args": {"actor": "ai:a", "now": 1000003000.0}},
            {"fn": "presence.touch", "args": {"actor": "ai:c", "now": 1000090000.0}},
            {"fn": "write", "args": {"path": "studio/presence.json", "text": "\u007bbroken"}},
            {"fn": "presence.others", "args": {"actor": "ai:a", "now": 1000090000.0}},
            {"fn": "presence.touch", "args": {"actor": "ai:a", "now": 1000090001.0}},
            {"fn": "write", "args": {"path": "studio/presence.json", "text": "[1, 2]"}},
            {"fn": "presence.check", "args": {"actor": "ai:a", "now": 1000090002.0}},
            {"fn": "presence.touch", "args": {"actor": "ai:b", "now": 1000090003.0}},
            {"fn": "toollog.read", "args": {}},
            {"fn": "toollog.record", "args": {"actor": "ai:a", "tool": "apply", "at": 1000.123456, "ms": 12.345,
                                               "kwargs": {"page": 1, "text": "短い", "long": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                                                          "flag": true, "ratio": 0.5, "obj": {}, "none": null}, "result": {"ok": true}}},
            {"fn": "toollog.record", "args": {"actor": "ai:a", "tool": "apply", "at": 1001.0, "ms": 0.05, "error": "失敗しました"}},
            {"fn": "toollog.record", "args": {"actor": "ai:b", "tool": "commit", "at": 1002.5, "ms": 3.0,
                                               "result": {"ok": false, "error": "bad line", "issues": [{"severity": "error", "code": "b"},
                                                          {"severity": "error", "code": "a"}, {"severity": "warning", "code": "w"},
                                                          {"severity": "error", "code": "a"}]}}},
            {"fn": "toollog.record", "args": {"actor": "ai:b", "tool": "next", "at": 1003.0, "ms": 1.25, "result": null}},
            {"fn": "toollog.record", "args": {"actor": "ai:b", "tool": "next", "at": 1004.0, "ms": 1.25, "result": {}}},
            {"fn": "toollog.read", "args": {}},
            {"fn": "claims.holder", "args": {"item": "page-3", "now": 1000.0}},
            {"fn": "claims.claim", "args": {"item": "page-3", "actor": "ai:a", "now": 1000.0}},
            {"fn": "claims.claim", "args": {"item": "page-3", "actor": "ai:a", "now": 1100.5}},
            {"fn": "claims.claim", "args": {"item": "page-3", "actor": "ai:b", "now": 1200.0}},
            {"fn": "claims.holder", "args": {"item": "page-3", "now": 1200.0}},
            {"fn": "claims.release", "args": {"item": "page-3", "actor": "ai:b", "now": 1300.0}},
            {"fn": "claims.claim", "args": {"item": "page-3", "actor": "human:作者", "now": 2000.0}},
            {"fn": "claims.holder", "args": {"item": "page-3", "now": 2000.0}},
            {"fn": "claims.release", "args": {"item": "page-3", "actor": "human:作者", "now": 2100.0}},
            {"fn": "claims.holder", "args": {"item": "page-3", "now": 2100.0}},
            {"fn": "write", "args": {"path": "studio/claims/art-1.json", "text": "not json"}},
            {"fn": "claims.holder", "args": {"item": "art-1", "now": 1.0}},
            {"fn": "claims.claim", "args": {"item": "art-1", "actor": "ai:a", "now": 5.0}},
            {"fn": "genreq.read", "args": {"request_id": "rq_a1"}},
            {"fn": "genreq.write", "args": {"request": {"id": "rq_a1", "purpose": "art", "target": {"page": 1, "frame_id": "f"},
                                                       "brief_hash": "sha256:00", "size": {"pad_mm": 2.5}},
                                            "files": {"guide.png": "iVBORw0KGgo=", "refs/one.png": "AAEC"}}},
            {"fn": "genreq.read", "args": {"request_id": "rq_a1"}},
            {"fn": "genreq.write", "args": {"request": {"id": "rq_a1", "purpose": "art", "note": "二度目"},
                                            "files": {"guide.png": "iVBORw0KGgo=", "refs/one.png": "AAED"}}},
            {"fn": "genreq.read", "args": {"request_id": "rq_a1"}},
            {"fn": "genreq.read", "args": {"request_id": "x_a1"}},
            {"fn": "genreq.read", "args": {"request_id": "rq_../rq_a1"}},
            {"fn": "genreq.read", "args": {"request_id": "rq_none"}},
            {"fn": "write", "args": {"path": "studio/requests/rq_bad/request.json", "text": "\u007b\u0022id\u0022: 1,"}},
            {"fn": "genreq.read", "args": {"request_id": "rq_bad"}},
            {"fn": "jsonutil.content_hash", "args": {"value": {"b": [1, 2.5, "あ"], "a": {"z": null, "y": true}}}},
            {"fn": "jsonutil.canonical_json", "args": {"value": {"b": [1, 2.5, "あ\n"], "a": {"z": null, "y": true}}}},
            {"fn": "jsonutil.write_json", "args": {"path": "studio/x/y.json", "value": {"b": 1, "a": [1, 2.5, "あ"], "c": {}}}},
            {"fn": "jsonutil.read_json", "args": {"path": "studio/x/y.json"}},
            {"fn": "jsonutil.read_json", "args": {"path": "studio/x/none.json", "default": {"d": 1}}},
            {"fn": "schemas.schema", "args": {"schema": "bible@1"}},
            {"fn": "schemas.schema", "args": {"schema": "script@1"}},
            {"fn": "schemas.schema", "args": {"schema": "name_plan@1"}},
            {"fn": "schemas.validate", "args": {"schema": "script@1", "data": {"scenes": [{"id": "s1", "summary": "x", "location_id": null,
                "time": null, "beats": [{"id": "b1", "kind": "dialogue", "speaker_id": "c", "text": "t", "emotion": "e", "page": 1, "reveal": false}]}]}}},
            {"fn": "schemas.validate", "args": {"schema": "script@1", "data": {"scenes": [{"id": 1, "summary": "x", "extra": 2,
                "beats": [{"id": "b1", "kind": "song", "speaker_id": 5, "text": "t", "page": 1.5, "reveal": 1}]}], "x/y~z": 1}}},
            {"fn": "schemas.validate", "args": {"schema": "bible@1", "data": {"title": "t", "logline": "l", "plot": "p", "themes": [],
                "characters": [{"id": "c", "name": "n", "role": "r", "age": "12", "look": {"hair": "h", "hair_value": "beta", "eyes": "e",
                "build": "b", "height_cm": null, "outfits": [], "marks": [], "silhouette": "s"}, "speech": {"first_person": "私", "tone": "t"}}],
                "locations": [], "style": {"notes": []}, "constraints": [], "lettering": {"speech": {"font": 1, "scale": null, "weight": "light"}}}}},
            {"fn": "schemas.validate", "args": {"schema": "name_plan@1", "data": []}},
            {"fn": "schemas.fill_nulls", "args": {"schema": "bible@1", "data": {"title": "t", "characters": [{"id": "c", "look": {}}],
                "lettering": {"speech": {}}}}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"const": 1}, "data": true}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"const": "a"}, "data": "b"}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"enum": [1, "x", null]}, "data": [1]}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"anyOf": [{"type": "integer"}, {"const": 3.5}, {}]}, "data": "s"}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"anyOf": [{"type": "integer"}, {"const": 3.5}]}, "data": "s"}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"type": "object", "additionalProperties": false}, "data": {"a~b": 1}}},
            {"fn": "jsonschema_lite.validate", "args": {"schema": {"type": "number"}, "data": false}},
            {"fn": "jsonschema_lite.conservative_problems", "args": {"schema": {"type": "object", "properties": {"a": {"type": "string",
                "minLength": 1}, "b": {"anyOf": [{"type": "array", "items": {"type": "object", "properties": {}}}, {"type": "null"}]}},
                "required": ["a"], "default": 1}}},
            {"fn": "jsonschema_lite.conservative_problems", "args": {"schema": {"type": "object", "properties": {"a": {"type": "string"}},
                "required": ["a"], "additionalProperties": false}}},
            {"fn": "state.bible", "args": {}},
            {"fn": "state.script", "args": {}},
            {"fn": "state.name", "args": {"page": 2}},
            {"fn": "state.name", "args": {"page": 3}},
            {"fn": "state.names", "args": {}},
            {"fn": "state.name_review", "args": {"page": 2}},
            {"fn": "state.name_review", "args": {"page": 4}},
            {"fn": "state.open_tickets", "args": {}},
            {"fn": "state.open_tickets", "args": {"kind": "fix"}},
            {"fn": "state.page_fixes", "args": {"page": 1}},
            {"fn": "state.page_fixes", "args": {"page": 3}},
            {"fn": "state.written_look", "args": {"character": {"look": {"hair": "黒髪", "eyes": "", "build": "小柄", "silhouette": "丸",
                "height_cm": 152.5, "outfits": [{"desc": "制服"}, "x", {"desc": ""}, {"desc": 7}], "marks": ["ヘアピン", 3]}}}},
            {"fn": "state.written_look", "args": {"character": {"look": null}}},
            {"fn": "tools_registry.load", "args": {}},
            {"fn": "tools_registry.get", "args": {"tool_id": null}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "openai:gpt-image-1", "spec": {"label": "画像", "sizes_px": [[1024, "1536"], [512, 512.9]],
                "supports": {"seed": true}, "notes": "メモ"}}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "local:sd", "spec": {"label": "SD"}}},
            {"fn": "tools_registry.get", "args": {"tool_id": "openai:gpt-image-1"}},
            {"fn": "tools_registry.get", "args": {"tool_id": "none"}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "bad", "spec": {"sizes_px": [[0, 1]]}}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "bad", "spec": {"sizes_px": [[1, "x"]]}}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "bad", "spec": {"sizes_px": []}}},
            {"fn": "tools_registry.set_tool", "args": {"tool_id": "", "spec": {}}},
            {"fn": "tools_registry.remove", "args": {"tool_id": "local:sd"}},
            {"fn": "tools_registry.remove", "args": {"tool_id": "local:sd"}},
            {"fn": "tools_registry.load", "args": {}}
        ])__");
        const QString dir = path("files");
        QDir().mkpath(dir + "/py/studio");
        QDir().mkpath(dir + "/py-config");
        QDir().mkpath(dir + "/cpp/studio");
        QDir().mkpath(dir + "/cpp-config");
        Json py_calls = Json::array();
        for (const Json& call : calls) {
            Json one = call;
            if (one["fn"].get<std::string>().starts_with("state.")) one["args"]["book"] = book_.toStdString();
            py_calls.push_back(std::move(one));
        }
        Json job = Json::object({{"op", "files"}, {"project", (dir + "/py").toStdString()}, {"config", (dir + "/py-config").toStdString()},
                                 {"calls", py_calls}, {"out", (dir + "/py.json").toStdString()}});
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(Json::array({job})));
        const auto ran = studio_harness({"batch", dir + "/jobs.json"}, path("pyenv"), 600000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(dir + "/py.json");
        QCOMPARE(records.size(), calls.size());
        const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(book_.toStdString()));
        std::vector<std::string> failures;
        int errors = 0;
        int split_entries = 0;
        for (std::size_t n = 0; n < calls.size(); ++n) {
            Json got = Json::object();
            try {
                got["result"] = studio_call(calls[n], dir + "/cpp", dir + "/cpp-config", loaded.document);
            } catch (const genko::core::Error& error) {
                got["error"] = python_type(error);
                got["message"] = error.what();
            }
            Json want = records[n];
            const Json py_files = want["files"];
            want.erase("files");
            if (calls[n]["fn"] == "toollog.read" && want.contains("result")) {
                // A documented deviation (SPEC COMP-01a): Python's toollog.read splits the log with str.splitlines, so
                // an entry whose text holds U+2028, U+2029 or U+0085 (json.dumps writes them as they are) is lost; this
                // build ends a line at "\n" only. Python's result must be the log's lines without exactly those.
                Json by_newline = Json::array();
                Json kept = Json::array();
                for (const char* name : {"studio/logs/tools.1.jsonl", "studio/logs/tools.jsonl"}) {
                    if (!py_files.contains(name)) continue;
                    const std::string text = py_files[name].get<std::string>();
                    for (std::size_t start = 0; start < text.size();) {
                        std::size_t end = text.find('\n', start);
                        if (end == std::string::npos) end = text.size();
                        const std::string line = text.substr(start, end - start);
                        start = end + 1;
                        Json entry;
                        try {
                            entry = genko::core::parse_python_json(line);
                        } catch (const genko::core::Error&) {
                            continue;
                        }
                        by_newline.push_back(entry);
                        const bool broken = line.find("\xE2\x80\xA8") != std::string::npos ||
                                            line.find("\xE2\x80\xA9") != std::string::npos ||
                                            line.find("\xC2\x85") != std::string::npos;
                        if (!broken) kept.push_back(entry);
                    }
                }
                std::string where;
                QVERIFY2(genko::test::strict_equal(want["result"], kept, &where), (std::to_string(n) + " toollog.read: " + where).c_str());
                if (kept.size() != by_newline.size()) ++split_entries;
                want["result"] = by_newline;
            }
            Json mine = studio_files(dir + "/cpp", dir + "/cpp-config");
            std::string where;
            const std::string name = std::to_string(n) + " " + calls[n]["fn"].get<std::string>();
            if (!genko::test::strict_equal(got, want, &where)) {
                failures.push_back(name + ": " + where + " — C++ " + genko::core::dump_python(got).substr(0, 600) + ", Python " +
                                   genko::core::dump_python(want).substr(0, 600));
                continue;
            }
            if (!genko::test::strict_equal(mine, py_files, &where)) {
                failures.push_back(name + " files: " + where);
                continue;
            }
            if (want.contains("error")) ++errors;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " calls differ from Python").c_str());
        QVERIFY2(split_entries >= 1, "no toollog.read met an entry Python's splitlines loses");
        qInfo("%zu calls the same as Python (%d of them errors; %d reads keeping an entry Python loses), with the same files "
              "after each", calls.size(), errors, split_entries);
    }

    // Placed pictures drawn (render._placed_raster and _finish_placed; psd.page_layers' placed art before the finish)
    // against Python's on the book `studio_harness.py make-placedbook` makes (pictures placed in panels cut square,
    // rounded, bowed and slanted, out to the bleed, past the paper; faces and clothes; the style's and panels' finishes;
    // line art, background, draft and name pictures; every picture mode and a JPEG; a painting app's colour picture on a
    // mono page; a colour page; hidden and missing art): every page in print, proof and name at two resolutions, each
    // placed and painted layer alone, and the exports that draw them (print PNGs, the PSD's layers, the layer PNGs, the
    // page sequence), file for file. A part of a page drawn alone is that part of the whole page (C++ only: tiles).
    void placedPicturesLikePython() {
        const QString dir = path("placed");
        QDir().mkpath(dir + "/py");
        const QString book = dir + "/book.genko";
        const auto made = studio_harness({"make-placedbook", book}, path("pyenv"), 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        QVERIFY(QFile::remove(book + "/assets.json"));
        genko::render::clear_render_caches();
        const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(book.toStdString()));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);

        Json jobs = Json::array();
        const auto out = [&](const QString& name) { return (dir + "/py/" + name + ".png").toStdString(); };
        int placed_layers = 0;
        for (const auto& page : doc.pages) {
            for (const char* mode : {"print", "proof", "name"}) {
                for (const int dpi : {60, 150}) {
                    jobs.push_back(Json{{"book", book.toStdString()}, {"page", page->index.json()}, {"dpi", dpi}, {"mode", mode},
                                        {"out", out(QStringLiteral("%1-%2-%3").arg(page->index.repr().c_str()).arg(mode).arg(dpi))}});
                }
            }
            for (const auto& layer : page->layers) {
                const bool painted = layer.source && layer.source->is_object() && layer.source->value("kind", Json()) == Json("psd");
                if (layer.kind != genko::core::LayerKind::Placed && !painted) continue;
                placed_layers += layer.kind == genko::core::LayerKind::Placed ? 1 : 0;
                jobs.push_back(Json{{"book", book.toStdString()}, {"page", page->index.json()}, {"kind", "layer"}, {"layer", layer.id},
                                    {"dpi", 100}, {"out", out(QStringLiteral("layer-%1").arg(layer.id.c_str()))}});
            }
        }
        QVERIFY2(placed_layers >= 18, std::to_string(placed_layers).c_str());
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto drawn = genko::test::render_harness({"render", dir + "/jobs.json"}, path("pyenv"));
        QVERIFY2(drawn.finished && drawn.exit_code == 0, drawn.err.right(4000).constData());

        std::vector<std::string> failures;
        int pages = 0;
        int layers = 0;
        int tiles = 0;
        for (const Json& job : jobs) {
            const genko::core::Page* page = nullptr;
            for (const auto& p : doc.pages) {
                if (p->index.json() == job["page"]) page = p.get();
            }
            QVERIFY(page != nullptr);
            const genko::render::Image want =
                genko::render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            const int dpi = job["dpi"].get<int>();
            std::string name = "page " + page->index.repr() + " " + job.value("mode", std::string("layer")) + " " + std::to_string(dpi);
            try {
                if (job.value("kind", std::string()) == "layer") {
                    const std::string id = job["layer"].get<std::string>();
                    name += " " + id;
                    const auto layer = std::find_if(page->layers.begin(), page->layers.end(), [&](const auto& l) { return l.id == id; });
                    const auto got = genko::render::layer_image(*page, *layer, dpi, &doc);
                    if (const auto d = picture_difference(got, want.convert("RGBA")); !d.empty()) failures.push_back(name + ": " + d);
                    ++layers;
                    continue;
                }
                genko::render::RenderOptions options;
                options.mode = job["mode"].get<std::string>();
                const auto whole = genko::render::render_page(*page, dpi, options, &doc);
                if (!whole.omitted.empty()) failures.push_back(name + ": left out " + whole.omitted.front());
                if (const auto d = picture_difference(whole.image, want.convert("RGB")); !d.empty()) {
                    failures.push_back(name + ": " + d);
                    continue;
                }
                ++pages;
                if (dpi != 150 || options.mode == "name") continue;
                // parts of the page drawn alone (as the screen draws them in tiles): that part of the whole page
                const int w = whole.image.width();
                const int h = whole.image.height();
                for (const auto& part : {genko::render::RenderRegion{0, 0, w / 2, h / 3}, genko::render::RenderRegion{w / 3, h / 4, w / 2, h / 2},
                                         genko::render::RenderRegion{w / 2, (h * 2) / 3, w - w / 2, h - (h * 2) / 3}}) {
                    genko::render::RenderOptions alone = options;
                    alone.region = part;
                    const auto got = genko::render::render_page(*page, dpi, alone, &doc);
                    const auto cut = whole.image.crop(genko::render::Box{part.x, part.y, part.x + part.w, part.y + part.h});
                    if (const auto d = picture_difference(got.image, cut); !d.empty()) failures.push_back(name + " tile: " + d);
                    ++tiles;
                }
            } catch (const std::exception& e) {
                failures.push_back(name + ": " + e.what());
            }
        }

        // the exports that draw them: Python's into out/ (moved to out_py/), then this build's into out/
        Json exports = Json::array();
        const auto add = [&](const std::string& id, const std::string& call, const std::string& dest, const Json& kwargs) {
            exports.push_back(Json{{"id", id}, {"book", book.toStdString()}, {"call", call}, {"dest", (dir + "/out/").toStdString() + dest},
                                   {"kwargs", kwargs}});
        };
        add("print", "export_print", "print", Json{{"fmt", "png"}, {"dpi", 50}, {"area", "bleed"}});
        add("print-gray", "export_print", "print-gray", Json{{"fmt", "png"}, {"dpi", 40}, {"color", "gray"}});
        add("psd", "export_psd_pages", "psd", Json{{"dpi", 40}});
        add("layers", "export_layers", "layers", Json{{"dpi", 40}});
        add("sequence", "export_png_sequence", "sequence", Json{{"working_dpi", 40}, {"mode", "proof"}});
        genko::test::write_bytes(dir + "/exports.json", genko::core::dump_python(exports));
        const auto exported = genko::test::render_harness({"export-cases", dir + "/exports.json", dir + "/exported.json"}, path("pyenv"));
        QVERIFY2(exported.finished && exported.exit_code == 0, exported.err.right(4000).constData());
        const Json results = genko::test::read_json(dir + "/exported.json");
        QVERIFY(QDir().rename(dir + "/out", dir + "/out_py"));
        const auto u8paths = [](const std::vector<std::filesystem::path>& files) {
            Json list = Json::array();
            for (const auto& f : files) list.push_back(genko::core::path_to_utf8(f));
            return list;
        };
        int exported_files = 0;
        for (const Json& job : exports) {
            const std::string id = job["id"].get<std::string>();
            const Json& kw = job["kwargs"];
            const std::filesystem::path dest = genko::core::path_from_utf8(job["dest"].get<std::string>());
            Json got;
            genko::render::clear_render_caches();
            try {
                const std::string call = job["call"].get<std::string>();
                if (call == "export_print") {
                    got = u8paths(genko::formats::export_print(doc, dest, "png", kw["dpi"].get<std::int64_t>(), 180, true,
                                                               kw.value("area", std::string("paper")), kw.value("color", std::string("auto"))));
                } else if (call == "export_psd_pages") {
                    got = u8paths(genko::formats::export_psd_pages(doc, dest, kw["dpi"].get<std::int64_t>()));
                } else if (call == "export_layers") {
                    got = u8paths(genko::formats::export_layers(doc, dest, kw["dpi"].get<int>()));
                } else {
                    got = u8paths(genko::formats::export_png_sequence(doc, dest, kw["working_dpi"].get<int>(), kw["mode"].get<std::string>()));
                }
            } catch (const std::exception& e) {
                failures.push_back("export " + id + ": " + e.what());
                continue;
            }
            if (!results.contains(id) || !results[id].contains("result") || results[id]["result"] != got) {
                failures.push_back("export " + id + ": " + genko::core::dump_python(got).substr(0, 300) + ", Python " +
                                   (results.contains(id) ? genko::core::dump_python(results[id]).substr(0, 300) : std::string("nothing")));
                continue;
            }
            const QString mine = dir + "/out/" + QString::fromStdString(id);
            const QString theirs = dir + "/out_py/" + QString::fromStdString(id);
            if (const QString d = genko::test::exports::tree_difference(mine, theirs); !d.isEmpty()) {
                failures.push_back("export " + id + ": " + d.toStdString());
                continue;
            }
            exported_files += static_cast<int>(genko::test::exports::tree(theirs).size());
        }

        for (const auto& f : failures) qWarning("%s", f.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " drawings differ from Python's").c_str());
        // C++ only: a finish whose value is not a number where the finish uses it is refused (Python fails with its
        // own TypeError inside Pillow's point())
        {
            genko::core::Document bad = doc;
            bool found = false;
            for (std::size_t i = 0; i < bad.pages.size() && !found; ++i) {
                if (!(bad.pages[i]->index == genko::core::Num(1))) continue;
                for (auto& layer : bad.edit_page(i).layers) {
                    if (layer.kind == genko::core::LayerKind::Placed && layer.finish) {
                        layer.finish = Json{{"line_threshold", "dark"}};
                        found = true;
                    }
                }
            }
            QVERIFY(found);
            genko::render::clear_render_caches();
            QString said;
            try {
                (void)genko::render::render_page(*bad.pages.front(), 60, genko::render::RenderOptions{}, &bad);
            } catch (const genko::core::Error& e) {
                said = QString::fromStdString(e.code() + ": " + e.what());
            }
            QCOMPARE(said, QStringLiteral("value: the finish's line_threshold is not a number"));
        }

        QVERIFY(pages >= 42 && layers >= 20 && tiles >= 42 && exported_files >= 30);
        qInfo("%d pages, %d layers alone and %d exported files the same as Python's; %d tiles the same as their pages", pages, layers,
              exported_files, tiles);
    }
};

QTEST_GUILESS_MAIN(TestContractStudio)
#include "test_contract_studio.moc"
