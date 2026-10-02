// The tone, effect line and ruler ops against Python's apply_ops (src/genko run by the reference Python), M3-B:
//   1. fixed cases (tone_cases.hpp: each op at least 8 times applied and 4 times refused) on the books of
//      tone_harness.py `fixture`: the same reply (applied, snapshot, warnings, results) and the same book after it
//      (snapshot(full=True); project.json as Python's writer writes it, each patch picture compared by its pixels and
//      each stroke blob by its bytes) — or the same error;
//   2. 160 random op sequences (tone_harness.py `sequences`: these ops mixed with the basic ops this build has) over
//      15 random books, each step compared the same way;
//   3. the command line: `genko apply` and `python -m genko apply` given the same ops files print the same JSON with
//      the same exit code, step after step on the same book.
// Ids are counted on both sides (ScopedIdSource / tone_harness ids_from), so the books compare exactly.
//
// Where Python stops with an exception apply_ops lets through (IndexError, AttributeError, OverflowError: a traceback in
// its command line), the C++ build gives the error code python_error, its error ending with the traceback's last line
// ("IndexError: list index out of range"); counted.
// Differences on purpose, each counted and checked, never taken for a match:
//   - Python keeps a number that is not finite (an angle of "inf", written into project.json as Infinity; the lines
//     effect_to_layer makes from a centre of "inf"): refused here ("… must be a finite number", "… not finite
//     numbers"); the case's Python reply must hold such a number. A sequence stops there (the books differ from then
//     on). (The other refusals of docs/cpp-migration/SPEC.md COMP-01a, as delete_tone of a layer that is not a tone,
//     are not asked for here: the unit tests check them.)
//   - The basic ops mixed in are M2's, with M2's refusals of COMP-01a: a lock on a page the book does not have ("no
//     page <n>") and a selected panel that is not on its page ("no frame <id> on page <n>"), which Python takes
//     without a word (an earlier op of the batch may have moved the pages). Counted as refused on purpose where
//     Python's book after the step shows why (no such page; the selection is no panel of the page); a sequence stops
//     there. Where Python refuses a later op of the same batch (or stops with a traceback), both leave the book as it
//     was: counted, and the sequence goes on.
//   - A pen line's point Python keeps as an int in memory (a guide drawn to the edge of a page whose size is an int,
//     ruler_to_layer): a float here; the saved strokes are the same bytes. snapshot(full)'s name_strokes / ink_strokes
//     are compared by value, and the cases where Python had an int are counted.
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "m3b_support.hpp"
#include "render/png.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "testsupport.hpp"
#include "tone_cases.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
namespace m3b = genko::test::m3b;

namespace {

const char* const kBasicOps[] = {"set_note",    "set_meta", "name_ok",   "lock_page",   "unlock_page", "set_autosave", "add_page",
                                 "delete_page", "add_layer", "set_layer", "add_stroke", "split_frame", "select_frame"};

std::string with(std::string text, const std::map<std::string, std::string>& values) {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        std::size_t at = 0;
        while ((at = text.find(it->first, at)) != std::string::npos) {
            text.replace(at, it->first.size(), it->second);
            at += it->second.size();
        }
    }
    return text;
}

genko::test::Run tone_harness(const QStringList& args, const QString& scratch, int timeout_ms = 3600000) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/tone_harness.py")};
    full += args;
    return genko::test::run(genko::test::python_ref(), full, genko::test::python_env(scratch), timeout_ms);
}

// Every number of a JSON value as a float (ints counted).
Json as_floats(const Json& value, int& ints) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        ++ints;
        return Json(value.get<double>());
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(as_floats(item, ints));
        return out;
    }
    return value;
}

// snapshot(full)'s pen points compared by value (see the file comment).
Json strokes_by_value(Json snapshot, int& ints) {
    if (!snapshot.is_object() || !snapshot.contains("pages")) return snapshot;
    for (Json& page : snapshot["pages"]) {
        for (const char* key : {"name_strokes", "ink_strokes"}) {
            if (page.contains(key)) page[key] = as_floats(page[key], ints);
        }
    }
    return snapshot;
}

struct Tally {
    int same = 0;
    int refused_on_purpose = 0;
    int both_refused = 0;  // (Python at a later op of the batch)
    int python_raised = 0;
    int int_points = 0;
    std::vector<std::string> failures;
};

// The C++ reply to a batch on `doc`, as tone_harness.reply_of writes Python's (and the book after it).
Json cpp_reply(const genko::core::Document& doc, const Json& ops, const std::string& agent, bool dry_run, std::int64_t ids_from,
               const fs::path& scratch, genko::core::Document* after) {
    const genko::core::ScopedIdSource ids(genko::core::counting_ids(static_cast<std::uint64_t>(ids_from)));
    Json got = Json::object();
    try {
        const auto result = m3b::bus().apply(doc, ops, genko::core::Actor(agent), dry_run);
        got["ok"] = true;
        got["applied"] = result.applied;
        got["snapshot"] = genko::storage::snapshot(result.doc);
        if (result.has_warnings) got["warnings"] = result.warnings;
        const bool undo = ops.size() == 1 && ops[0].is_object() && ops[0].value("op", Json()) == Json("undo");
        if (!dry_run && !undo) {
            got["snapshot_full"] = genko::storage::snapshot(result.doc, true);
            got["payload"] = m3b::normalized_payload(result.doc, scratch);
            if (after != nullptr) *after = result.doc;
        }
    } catch (const genko::core::ApplyError& error) {
        got = Json::object({{"ok", false}, {"error", error.what()}, {"code", error.code()}});
    }
    return got;
}

// The index N of an error "ops[N] <op>: …".
std::optional<std::size_t> op_index(const std::string& error) {
    const std::size_t close = error.find("] ");
    if (!error.starts_with("ops[") || close == std::string::npos) return std::nullopt;
    const std::string digits = error.substr(4, close - 4);
    if (digits.empty() || digits.size() > 6 || digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return static_cast<std::size_t>(std::stoul(digits));
}

// M2's refusals of docs/cpp-migration/SPEC.md COMP-01a among the basic ops (see the file comment): the batch's op
// `error` names when it is one of them ("no page <n>" of lock_page and unlock_page, "no frame <id> on page <n>" of
// select_frame), else nullptr.
const Json* m2_refused_op(const std::string& error, const Json& ops) {
    const auto i = op_index(error);
    if (!i || *i >= ops.size() || !ops[*i].is_object() || !ops[*i].contains("page")) return nullptr;
    const Json& op = ops[*i];
    const std::string name = op.value("op", std::string());
    const std::string head = "ops[" + std::to_string(*i) + "] " + name + ": ";
    if (!error.starts_with(head)) return nullptr;
    const std::string message = error.substr(head.size());
    if ((name == "lock_page" || name == "unlock_page") && message.starts_with("no page ")) return &op;
    if (name == "select_frame" && message.starts_with("no frame ")) return &op;
    return nullptr;
}

// Python applied the batch: whether its book after it (the reply's snapshot) shows why the op was refused here (no
// such page; a selection that is no panel of its page).
bool shown_after(const Json& op, const Json& want) {
    if (!want.contains("snapshot")) return false;
    const Json* page = nullptr;
    for (const Json& p : want["snapshot"]["pages"]) {
        if (genko::core::py_equals(p["index"], op["page"])) page = &p;
    }
    if (op["op"] != Json("select_frame")) return page == nullptr;
    if (page == nullptr) return false;
    const Json frame = op.contains("frame_id") ? op["frame_id"] : Json();
    if ((*page)["selected_frame_id"] != frame) return false;
    for (const Json& leaf : (*page)["leaves"]) {
        if (leaf["id"] == frame) return false;
    }
    return true;
}

// Python refused the batch too, at a later op (or stopped with a traceback): whether the book before it, the same on
// both sides, shows why the op was refused here.
bool shown_before(const Json& op, const genko::core::Document& before) {
    const genko::core::Page* page = nullptr;
    for (const auto& p : before.pages) {
        if (genko::core::py_equals(p->index.json(), op["page"])) page = p.get();
    }
    if (op["op"] != Json("select_frame")) return page == nullptr;
    if (page == nullptr) return false;
    const Json frame = op.contains("frame_id") ? op["frame_id"] : Json();
    return !frame.is_string() || page->find_frame(frame.get<std::string>()) == nullptr;
}

// "" when C++ did what Python did; "on purpose…" / "python raised" / "both refused (M2)" for the counted differences;
// else what differs. `before`: the book the batch was given (the same on both sides).
std::string compare(const Json& got, const Json& want, const Json& ops, const genko::core::Document& before, bool want_nonfinite,
                    Tally& tally) {
    if (want.value("ok", false)) {
        if (!got.value("ok", false)) {
            const std::string error = got.value("error", std::string());
            if (want_nonfinite && (error.find("must be a finite number") != std::string::npos || error.find("not finite numbers") != std::string::npos)) {
                ++tally.refused_on_purpose;
                return "on purpose";
            }
            if (const Json* op = m2_refused_op(error, ops); op != nullptr && shown_after(*op, want)) {
                ++tally.refused_on_purpose;
                return "on purpose (M2)";
            }
            return "Python applied it, here: " + error;
        }
        for (const char* key : {"applied", "snapshot", "warnings", "results", "payload"}) {
            if (want.contains(key) != got.contains(key)) return std::string("one side has no ") + key;
            if (!want.contains(key)) continue;
            std::string where;
            if (!genko::test::strict_equal(got[key], want[key], &where)) return std::string(key) + where;
        }
        if (want.contains("snapshot_full") != got.contains("snapshot_full")) return "one side has no snapshot_full";
        if (want.contains("snapshot_full")) {
            int ints = 0;
            int none = 0;
            std::string where;
            const Json theirs = strokes_by_value(want["snapshot_full"], ints);
            if (!genko::test::strict_equal(strokes_by_value(got["snapshot_full"], none), theirs, &where)) return "snapshot_full" + where;
            std::string strict_where;
            if (!genko::test::strict_equal(got["snapshot_full"], want["snapshot_full"], &strict_where)) ++tally.int_points;
        }
        ++tally.same;
        return {};
    }
    // (an op of the batch refused here on purpose, which Python passed before refusing a later one, or stopping: the
    // book stays as it was on both sides)
    if (const Json* op = got.value("ok", true) ? nullptr : m2_refused_op(got.value("error", std::string()), ops); op != nullptr) {
        const bool python_later = want.value("raised", std::string("ApplyError")) != "ApplyError" ||
                                  op_index(want.value("error", std::string())) > op_index(got.value("error", std::string()));
        if (python_later && shown_before(*op, before)) {
            ++tally.both_refused;
            return "both refused (M2)";
        }
    }
    if (want.value("raised", std::string("ApplyError")) != "ApplyError") {
        // (python_error, the error ending with the traceback's last line: "<type>: <message>")
        const std::string error = got.value("error", std::string());
        const std::string last = want.value("error", std::string());
        if (got.value("ok", true) || got.value("code", std::string()) != "python_error" || !error.ends_with(last)) {
            return "Python raised " + last + ", here: " + (got.value("ok", true) ? std::string("applied") : got.value("code", std::string()) + " " + error);
        }
        ++tally.python_raised;
        return "python raised";
    }
    if (got.value("ok", true)) return "Python refused it (" + want.value("error", std::string()) + "), here it was applied";
    if (got["error"] != want["error"]) return "errors differ: " + got["error"].get<std::string>() + " ≠ " + want["error"].get<std::string>();
    if (got.value("code", std::string()) == "python_error") return "Python's ApplyError is python_error here";
    ++tally.same;
    return {};
}

bool has_prefix(const std::vector<std::string>& pointers, const std::string& prefix) {
    for (const auto& p : pointers) {
        if (p.starts_with(prefix)) return true;
    }
    return false;
}

}  // namespace

class TestContractToneOps : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString fixture_;
    std::map<std::string, std::string> values_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        fixture_ = path("fixture");
        const auto made = tone_harness({"fixture", fixture_}, scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        // the ink square of panel f-bottom (tone_harness.make_fixture), the pictures of the cases
        const auto doc = genko::storage::load_document(m3b::to_path(fixture_ + "/fixture.genko")).document;
        const genko::core::Frame* bottom = doc.page(0).find_frame("f-bottom");
        QVERIFY(bottom != nullptr);
        const double x = bottom->rect.x.value() + 10;
        const double y = bottom->rect.y.value() + 10;
        values_["$IN_X"] = genko::core::py_float_repr(x + 15);
        values_["$IN_Y"] = genko::core::py_float_repr(y + 10);
        values_["$LINE_X"] = genko::core::py_float_repr(x);
        values_["$LINE_Y"] = genko::core::py_float_repr(y + 10);
        genko::render::Image mask = genko::render::Image::create("L", genko::render::Size{20, 14}, genko::render::Ink(0));
        mask.paste(genko::render::Ink(255), genko::render::Box{3, 2, 17, 12});
        values_["$MASK"] = genko::core::b64encode(genko::render::write_png(mask));
        genko::render::Image tile = genko::render::Image::create("RGBA", genko::render::Size{8, 8}, genko::render::Ink{255, 255, 255, 0});
        tile.paste(genko::render::Ink{20, 20, 20, 255}, genko::render::Box{2, 2, 6, 6});
        values_["$TILE"] = genko::core::b64encode(genko::render::write_png(tile));
    }

    void fixedCases() {
        const std::size_t n = std::size(genko::test::tones::kCases);
        Json jobs = Json::array();
        QDir().mkpath(path("cases"));
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = genko::test::tones::kCases[i];
            Json job = Json::object();
            job["book"] = (fixture_ + (c.book == 'A' ? "/fixture.genko" : "/strict.genko")).toStdString();
            job["ops"] = genko::core::parse_python_json(with(c.ops, values_));
            job["agent"] = c.agent;
            job["dry_run"] = false;
            job["ids_from"] = 1 + 1000 * static_cast<std::int64_t>(i);
            job["out"] = path(QStringLiteral("cases/%1.json").arg(i)).toStdString();
            jobs.push_back(job);
        }
        genko::test::write_bytes(path("cases/jobs.json"), genko::core::dump_python(jobs));
        const auto ran = tone_harness({"apply", path("cases/jobs.json")}, scratch_.path());
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(3000).constData());

        Tally tally;
        std::map<std::string, std::pair<int, int>> counts;  // op → (applied, refused) in Python
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = genko::test::tones::kCases[i];
            const Json& job = jobs[i];
            genko::core::ParseRepairs repairs;
            const Json want = genko::core::parse_python_json(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())), &repairs);
            const std::string label = std::to_string(i) + " " + c.op + " " + c.ops;
            if (want.value("ok", false) != c.works) {
                tally.failures.push_back(label + ": Python " + (want.value("ok", false) ? "applied it" : "refused it: " + want.value("error", std::string())));
                continue;
            }
            auto& count = counts[c.op];
            (want.value("ok", false) ? count.first : count.second) += 1;
            const auto doc = genko::storage::load_document(m3b::to_path(QString::fromStdString(job["book"].get<std::string>()))).document;
            const Json got = cpp_reply(doc, job["ops"], c.agent, false, job["ids_from"].get<std::int64_t>(),
                                       m3b::to_path(path(QStringLiteral("cases/cpp-%1").arg(i))), nullptr);
            const std::string diff = compare(got, want, job["ops"], doc, !repairs.nonfinite.empty(), tally);
            if (!diff.empty() && !diff.starts_with("on purpose") && diff != "python raised" && diff != "both refused (M2)") {
                tally.failures.push_back(label + ": " + diff);
            }
        }
        for (const auto& [op, count] : counts) {
            qInfo("%s: %d applied, %d refused", op.c_str(), count.first, count.second);
            QVERIFY2(count.first >= 8 && count.second >= 4, op.c_str());
        }
        QCOMPARE(counts.size(), std::size_t{12});
        qInfo("cases: %d the same, %d refused here on purpose, %d Python raised, %d with int points", tally.same,
              tally.refused_on_purpose, tally.python_raised, tally.int_points);
        for (const auto& f : tally.failures) qWarning("%s", f.c_str());
        QVERIFY(tally.failures.empty());
    }

    void randomSequences() {
        const QString books = path("books");
        auto made = tone_harness({"make-books", books, "--seed", "20261002", "--count", "15"}, scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        QStringList basic;
        for (const char* name : kBasicOps) {
            if (genko::render::ops_registry().find(name) != nullptr) basic << name;
        }
        qInfo("basic ops mixed in: %s", basic.join(',').toUtf8().constData());
        const QString out = path("sequences.json");
        // (160 sequences: with M2's basic ops some stop at a refusal on purpose; 10 more than M3-B's 150 keep the steps
        // compared at least M3-B's 1,371)
        made = tone_harness({"sequences", out, "--books", books, "--seed", "7", "--count", "160", "--steps", "12", "--ops", basic.join(',')},
                            scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(3000).constData());
        genko::core::ParseRepairs repairs;
        const Json sequences = genko::core::parse_python_json(genko::test::read_bytes(out), &repairs);
        QCOMPARE(sequences.size(), std::size_t{160});

        Tally tally;
        int steps = 0;
        int stopped = 0;
        std::set<std::string> books_used;
        std::map<std::string, int> ops_seen;
        for (std::size_t k = 0; k < sequences.size(); ++k) {
            const Json& sequence = sequences[k];
            books_used.insert(sequence["book"].get<std::string>());
            auto doc = genko::storage::load_document(m3b::to_path(QString::fromStdString(sequence["book"].get<std::string>()))).document;
            const Json& list = sequence["steps"];
            for (std::size_t s = 0; s < list.size(); ++s) {
                const Json& step = list[s];
                for (const Json& op : step["ops"]) ops_seen[op.is_object() ? op.value("op", std::string("?")) : "?"] += 1;
                const std::string at = "/" + std::to_string(k) + "/steps/" + std::to_string(s) + "/";
                genko::core::Document after;
                const Json got = cpp_reply(doc, step["ops"], step["agent"].get<std::string>(), step["dry_run"].get<bool>(),
                                           step["ids_from"].get<std::int64_t>(), m3b::to_path(path(QStringLiteral("seq/%1-%2").arg(k).arg(s))), &after);
                Json want = step["reply"];
                if (step.contains("payload")) want["payload"] = step["payload"];
                ++steps;
                const std::string diff = compare(got, want, step["ops"], doc, has_prefix(repairs.nonfinite, at), tally);
                if (diff.starts_with("on purpose") || diff == "python raised" || diff == "both refused (M2)") {  // (each shown, for the report)
                    const std::string python = diff == "on purpose"          ? std::string("kept a number that is not finite")
                                               : diff == "on purpose (M2)"   ? std::string("applied it (COMP-01a, M2)")
                                               : diff == "both refused (M2)" ? "refused a later op (COMP-01a, M2): " + want.value("error", std::string())
                                                                             : want.value("raised", std::string()) + ": " + want.value("error", std::string());
                    qInfo("sequence %zu step %zu %s: Python %s; here: %s", k, s, genko::core::dump_python(step["ops"]).substr(0, 300).c_str(),
                          python.c_str(), got.value("error", std::string()).c_str());
                }
                if (diff.starts_with("on purpose")) {
                    ++stopped;  // (the books differ from here on)
                    break;
                }
                if (!diff.empty() && diff != "python raised" && diff != "both refused (M2)") {
                    tally.failures.push_back("sequence " + std::to_string(k) + " step " + std::to_string(s) + " " +
                                             genko::core::dump_python(step["ops"]).substr(0, 400) + " as " + step["agent"].get<std::string>() + ": " + diff);
                    break;
                }
                if (got.value("ok", false) && got.contains("payload")) doc = after;
            }
        }
        for (const auto& [op, n] : ops_seen) qInfo("  %s: %d", op.c_str(), n);
        qInfo("sequences: %d steps, %d the same, %d refused here on purpose (sequence stopped), %d refused on both sides (here "
              "on purpose at an earlier op), %d Python raised, %d with int points",
              steps, tally.same, tally.refused_on_purpose, tally.both_refused, tally.python_raised, tally.int_points);
        for (const auto& f : tally.failures) qWarning("%s", f.c_str());
        QCOMPARE(books_used.size(), std::size_t{15});
        QVERIFY(tally.failures.empty());
        QVERIFY(stopped <= 15);  // (rare: most sequences run to their end)
        QVERIFY(steps >= 1371);  // (M3-B's own test compared 1,371)
    }

    void commandLine() {
        const QString py_book = path("cli-py.genko");
        const QString cpp_book = path("cli-cpp.genko");
        genko::test::copy_tree(fixture_ + "/fixture.genko", py_book);
        const auto migrated = genko::test::run_genko({"migrate", fixture_ + "/fixture.genko", cpp_book});
        QVERIFY2(migrated.exit_code == 0, migrated.out.constData());
        const std::vector<std::string> batches{
            R"([{"op": "add_tone", "page": 1, "id": "c1", "area": {"rect": [20, 20, 30, 20]}, "pattern": "dot", "density": 0.4}])",
            R"([{"op": "add_tone", "page": 1, "pattern": "stripes"}])",
            R"([{"op": "set_tone", "page": 1, "id": "c1", "dot_shape": "diamond", "offset_mm": [0.5, 0]}])",
            R"([{"op": "set_tone", "page": 1, "id": "ink", "density": 0.2}])",
            R"([{"op": "add_tone", "page": 1, "id": "c2", "at": {"x_mm": $IN_X, "y_mm": $IN_Y}}])",
            R"([{"op": "delete_tone", "page": 1, "id": "t-grad"}])",
            R"([{"op": "delete_tone", "page": 1, "id": "t-grad"}])",
            R"([{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": "f-top", "id": "c3", "params": {"count": 25, "twist": 15}}])",
            R"([{"op": "add_effect", "page": 1, "kind": "sparkle"}])",
            R"([{"op": "edit_effect", "page": 1, "id": "c3", "params": {"count": null, "bundle": 4}, "visible": false}])",
            R"([{"op": "edit_effect", "page": 1, "id": "c3", "params": {"jitter": 3}}])",
            R"([{"op": "effect_to_layer", "page": 1, "id": "e-beta", "layer": "finish"}])",
            R"([{"op": "effect_to_layer", "page": 1, "id": "e-beta"}])",
            R"([{"op": "delete_effect", "page": 1, "id": "c3"}])",
            R"([{"op": "delete_effect", "page": 1, "id": "c3"}])",
            R"([{"op": "add_ruler", "page": 1, "kind": "curve", "points": [[10, 10], [40, 30], [80, 15]], "id": "c4"}])",
            R"([{"op": "add_ruler", "page": 1, "kind": "spiral"}])",
            R"([{"op": "edit_ruler", "page": 1, "id": "r-persp", "points": [[-30, 60], [150, 30]]}])",
            R"([{"op": "edit_ruler", "page": 1, "id": "r-fixed", "points": [[1, 1]]}])",
            R"([{"op": "ruler_to_layer", "page": 1, "id": "c4", "width_mm": 0.4}])",
            R"([{"op": "ruler_to_layer", "page": 1, "id": "r-par"}])",
            R"([{"op": "set_ruler", "page": 2, "points": [[10, 20]]}])",
            R"([{"op": "delete_ruler", "page": 1, "id": "c4"}])",
            R"([{"op": "delete_ruler", "page": 1, "id": "c4"}])",
            R"([{"op": "add_tone", "page": 1, "id": "c5"}, {"op": "add_effect", "page": 1, "kind": "white", "id": "c6"}, {"op": "delete_ruler", "page": 2}])"};
        int same = 0;
        for (std::size_t i = 0; i < batches.size(); ++i) {
            const QString ops = path(QStringLiteral("cli-%1.json").arg(i));
            genko::test::write_bytes(ops, with(batches[i], values_));
            const auto py = genko::test::run(genko::test::python_ref(), {"-m", "genko", "apply", py_book, ops}, genko::test::python_env(scratch_.path()));
            const auto cpp = genko::test::run_genko({"apply", cpp_book, ops});
            QVERIFY2(py.finished && cpp.finished, batches[i].c_str());
            QVERIFY2(cpp.exit_code == py.exit_code, (batches[i] + ": " + cpp.out.toStdString() + " / " + py.out.toStdString()).c_str());
            Json want = genko::test::one_line(py.out);
            Json got = genko::test::one_line(cpp.out);
            for (const char* key : {"job_id", "revision", "txn", "code"}) {
                want.erase(key);
                got.erase(key);
            }
            std::string where;
            QVERIFY2(genko::test::strict_equal(got, want, &where), (batches[i] + ": " + where).c_str());
            ++same;
        }
        qInfo("command line: %d batches the same", same);
        // and the books they made
        const auto py_snap = genko::test::run(genko::test::python_ref(), {"-m", "genko", "inspect", py_book, "--full"}, genko::test::python_env(scratch_.path()));
        const auto cpp_snap = genko::test::run_genko({"inspect", cpp_book, "--full"});
        std::string where;
        QVERIFY2(genko::test::strict_equal(genko::test::one_line(cpp_snap.out), genko::test::one_line(py_snap.out), &where), where.c_str());
    }
};

QTEST_GUILESS_MAIN(TestContractToneOps)
#include "test_contract_tone_ops.moc"
