// studio/journal.jsonl and studio/audit.jsonl (storage::journal; schema-v4 §4, §5) on hand-made files: the line
// format, reading complete lines and a cut last line, transactions and their outcome, the Undo/Redo stacks replayed
// from commits (with legacy/map.json first), the repair of §4.3 (a cut line kept in journal.partial, pending
// transactions committed or aborted by project.json's sha256, approvals audited once), and the state's canonical
// form.

#include <QtTest>

#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/state.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
namespace journal = genko::storage::journal;
using genko::core::Json;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

std::string txn_id(char c) { return std::string(32, c); }
std::string state(char c) { return "sha256:" + std::string(64, c); }

Json prepare(const std::string& txn, const std::string& action, std::int64_t rev, const Json& before, const Json& after,
             const std::string& project_sha = std::string(64, '0'), const std::string& actor = "human:a") {
    Json line = Json::object();
    line["v"] = 4;
    line["kind"] = "prepare";
    line["txn"] = txn;
    line["action"] = action;
    line["rev"] = rev;
    line["base_rev"] = rev - 1;
    line["actor"] = actor;
    line["at"] = 1790000000.5;
    line["before"] = before;
    line["after"] = after;
    line["project_sha256"] = project_sha;
    return line;
}

std::string lines(std::initializer_list<Json> values) {
    std::string out;
    for (const Json& value : values) out += journal::line_text(value);
    return out;
}

std::vector<std::string> ids(const std::vector<journal::HistoryItem>& items) {
    std::vector<std::string> out;
    for (const auto& item : items) out.push_back(item.id);
    return out;
}

}  // namespace

class TestJournal : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int books_ = 0;

    QString new_book() { return tmp_.path() + "/book" + QString::number(++books_) + ".genko"; }

private slots:
    void lineFormat() {
        Json line = Json::object();
        line["v"] = 4;
        line["kind"] = "commit";
        line["txn"] = "東京";
        line["rev"] = 3;
        line["at"] = 0.1 + 0.2;
        QCOMPARE(journal::line_text(line), std::string("{\"v\":4,\"kind\":\"commit\",\"txn\":\"東京\",\"rev\":3,\"at\":0.30000000000000004}\n"));
        QCOMPARE(journal::commit_line(txn_id('a'), 7, true).dump(), std::string("{\"v\":4,\"kind\":\"commit\",\"txn\":\"" + txn_id('a') + "\",\"rev\":7,\"recovered\":true}"));
        QCOMPARE(journal::abort_line(txn_id('b')).dump(), std::string("{\"v\":4,\"kind\":\"abort\",\"txn\":\"" + txn_id('b') + "\"}"));
    }

    void readLines() {
        const QString dir = new_book();
        const std::string fragment = "{\"v\":4,\"kind\":\"comm";
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 "{\"v\":4,\"kind\":\"prepare\",\"txn\":\"x\"}\n\ngarbage\n[1]\r\n{\"kind\": \"undo\"}\r\n" + fragment);
        const auto read = journal::read_lines(to_path(dir) / "studio" / "journal.jsonl");
        QVERIFY(read.exists);
        QCOMPARE(read.values.size(), std::size_t{2});
        QCOMPARE(read.unreadable, std::vector<std::size_t>({3, 4}));
        QCOMPARE(read.tail, fragment);
        QCOMPARE(read.complete_size,
                 genko::test::read_bytes(dir + "/studio/journal.jsonl").size() - fragment.size());
        const auto missing = journal::read_lines(to_path(dir) / "nothing.jsonl");
        QVERIFY(!missing.exists && missing.values.empty() && missing.tail.empty());
    }

    void transactionsAndOutcomes() {
        const QString dir = new_book();
        const auto a = txn_id('a'), b = txn_id('b'), c = txn_id('c');
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 lines({prepare(a, "edit", 1, nullptr, state('1')), journal::commit_line(a, 1),
                                        prepare(b, "edit", 2, state('1'), state('2')), journal::abort_line(b),
                                        prepare(b, "edit", 3, state('1'), state('3')), journal::commit_line(b, 3),
                                        prepare(c, "edit", 4, state('3'), state('4')),
                                        Json::object({{"kind", "commit"}, {"rev", 99}})}));  // (an older line: not v4)
        const auto read = journal::read_lines(to_path(dir) / "studio" / "journal.jsonl");
        const auto txns = journal::transactions(read);
        QCOMPARE(txns.size(), std::size_t{4});
        QCOMPARE(int(txns[0].status), int(journal::Transaction::Status::committed));
        QCOMPARE(int(txns[1].status), int(journal::Transaction::Status::aborted));
        QCOMPARE(int(txns[2].status), int(journal::Transaction::Status::committed));
        QCOMPARE(int(txns[3].status), int(journal::Transaction::Status::pending));
        QCOMPARE(journal::find_committed(txns, b)->rev, std::int64_t{3});
        QVERIFY(journal::find_committed(txns, c) == nullptr);
        QCOMPARE(journal::max_revision(read), std::int64_t{4});  // (the v3 line's 99 is not a v4 revision)
    }

    void replayStacks() {
        const QString dir = new_book();
        const auto a = txn_id('a'), b = txn_id('b'), c = txn_id('c'), u1 = txn_id('d'), r1 = txn_id('e'), u2 = txn_id('f'),
                   x = txn_id('1'), m = txn_id('2'), p = txn_id('3');
        Json undo1 = prepare(u1, "undo", 3, state('2'), state('1'));
        undo1["target"] = b;
        Json redo1 = prepare(r1, "redo", 4, state('1'), state('2'));
        redo1["target"] = b;
        Json undo2 = prepare(u2, "undo", 5, state('2'), state('1'));
        undo2["target"] = b;
        genko::test::write_bytes(
            dir + "/studio/journal.jsonl",
            lines({prepare(a, "edit", 1, nullptr, state('1')), journal::commit_line(a, 1),
                   prepare(b, "edit", 2, state('1'), state('2')), journal::commit_line(b, 2), undo1, journal::commit_line(u1, 3),
                   redo1, journal::commit_line(r1, 4), undo2, journal::commit_line(u2, 5),
                   prepare(m, "recover", 6, state('1'), state('9')), journal::commit_line(m, 6),
                   prepare(x, "edit", 7, state('9'), state('8')), journal::abort_line(x),     // (aborted: not on a stack)
                   prepare(p, "edit", 7, state('9'), state('7'))}));                         // (pending: neither)
        auto stacks = journal::stacks(to_path(dir));
        QCOMPARE(ids(stacks.undo), std::vector<std::string>({a}));
        QCOMPARE(ids(stacks.redo), std::vector<std::string>({b}));
        QVERIFY(stacks.problems.empty());
        QCOMPARE(stacks.redo[0].before.value(), state('1'));
        QCOMPARE(stacks.redo[0].after.value(), state('2'));
        QCOMPARE(stacks.redo[0].actor, Json("human:a"));
        QVERIFY(!stacks.undo[0].before.has_value());  // (the book did not exist before it)

        // an edit empties Redo
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 genko::test::read_bytes(dir + "/studio/journal.jsonl") + journal::line_text(journal::abort_line(p)) +
                                     lines({prepare(c, "edit", 8, state('9'), state('6')), journal::commit_line(c, 8)}));
        stacks = journal::stacks(to_path(dir));
        QCOMPARE(ids(stacks.undo), std::vector<std::string>({a, c}));
        QVERIFY(stacks.redo.empty());

        // a transaction naming another target than the stack's is reported
        Json wrong = prepare(txn_id('9'), "undo", 9, state('6'), state('9'));
        wrong["target"] = a;
        genko::test::write_bytes(dir + "/studio/journal.jsonl", genko::test::read_bytes(dir + "/studio/journal.jsonl") +
                                                                     lines({wrong, journal::commit_line(txn_id('9'), 9)}));
        stacks = journal::stacks(to_path(dir));
        QCOMPARE(stacks.problems.size(), std::size_t{1});
    }

    void legacyHistoryComesFirst() {
        const QString dir = new_book();
        const Json map = genko::core::parse_python_json(R"({"v": 4, "source_version": 3, "boundary_rev": 1, "entries": [
            {"old_rev": 1, "actor": "human:a", "at": 1.0, "kind": "commit", "before_old": null, "after_old": "sha256:)" +
                                                        std::string(64, '1') + R"(", "before": null, "after": ")" + state('a') + R"("},
            {"old_rev": 2, "actor": "ai:b", "at": 2.0, "kind": "commit", "before_old": "sha256:)" + std::string(64, '1') +
                                                        R"(", "after_old": "sha256:)" + std::string(64, '2') + R"(", "before": ")" + state('a') +
                                                        R"(", "after": null},
            {"old_rev": 2, "actor": "ai:b", "at": 3.0, "kind": "undo", "before_old": null, "after_old": null, "before": null, "after": null},
            {"old_rev": 2, "actor": "ai:b", "at": 4.0, "kind": "redo", "before_old": null, "after_old": null, "before": null, "after": null}]})");
        genko::test::write_bytes(dir + "/legacy/map.json", map.dump());
        const auto m = txn_id('m');
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 lines({prepare(m, "migrate", 1, nullptr, state('b')), journal::commit_line(m, 1)}));
        auto stacks = journal::stacks(to_path(dir));
        QCOMPARE(ids(stacks.undo), std::vector<std::string>({"legacy:0", "legacy:1"}));
        QCOMPARE(stacks.undo[1].rev, Json(2));
        QCOMPARE(stacks.undo[1].actor, Json("ai:b"));
        QVERIFY(!stacks.undo[1].after.has_value());  // (its snapshot could not be converted…)
        QCOMPARE(stacks.undo[1].after_old, "sha256:" + std::string(64, '2'));  // (…which the old ref tells)
        // undoing it in v4 moves it to Redo; a new edit then empties Redo
        const auto u = txn_id('u');
        Json undo = prepare(u, "undo", 2, state('b'), state('a'));
        undo["target"] = "legacy:1";
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 genko::test::read_bytes(dir + "/studio/journal.jsonl") + lines({undo, journal::commit_line(u, 2)}));
        stacks = journal::stacks(to_path(dir));
        QCOMPARE(ids(stacks.undo), std::vector<std::string>({"legacy:0"}));
        QCOMPARE(ids(stacks.redo), std::vector<std::string>({"legacy:1"}));
        QVERIFY(stacks.problems.empty());
    }

    void repairCommitsWhatProjectHolds() {
        const QString dir = new_book();
        const std::string old_project = "{\"old\": true}";
        const std::string new_project = "{\"new\": true}";
        const auto a = txn_id('a'), b = txn_id('b');
        Json pending = prepare(b, "edit", 2, state('1'), state('2'), genko::storage::sha256_hex(new_project));
        pending["audit"] = Json::array({Json::object({{"what", "page:p:name_ok"}, {"from", false}, {"to", true}})});
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 lines({prepare(a, "edit", 1, nullptr, state('1'), genko::storage::sha256_hex(old_project)),
                                        journal::commit_line(a, 1), pending}));
        genko::test::write_bytes(dir + "/project.json", new_project);  // (replaced before the crash)
        const auto files_before = genko::test::tree_hashes(dir);
        const auto plan = journal::plan_repair(to_path(dir));
        QCOMPARE(plan.committed, std::vector<std::string>({b}));
        QCOMPARE(plan.audited, std::vector<std::string>({b}));
        QVERIFY(plan.aborted.empty());
        QVERIFY(genko::test::tree_hashes(dir) == files_before);  // (a plan writes nothing)

        const auto done = journal::repair(to_path(dir));
        QCOMPARE(done.committed, std::vector<std::string>({b}));
        const auto read = journal::read_lines(to_path(dir) / "studio" / "journal.jsonl");
        QCOMPARE(read.values.back(), journal::commit_line(b, 2, true));
        const auto audit = journal::audit_entries(to_path(dir));
        QCOMPARE(audit.size(), std::size_t{1});
        QCOMPARE(audit[0], journal::audit_line(pending));
        QCOMPARE(audit[0]["at"], pending["at"]);  // (the same line the save would have written)
        QVERIFY(!journal::repair(to_path(dir)).needed());  // (once)
        QCOMPARE(journal::audit_entries(to_path(dir)).size(), std::size_t{1});
    }

    void repairAbortsWhatProjectDoesNotHold() {
        const QString dir = new_book();
        const auto a = txn_id('a'), b = txn_id('b');
        genko::test::write_bytes(dir + "/studio/journal.jsonl",
                                 lines({prepare(a, "edit", 1, nullptr, state('1'), genko::storage::sha256_hex("old")),
                                        journal::commit_line(a, 1),
                                        prepare(b, "edit", 2, state('1'), state('2'), genko::storage::sha256_hex("new"))}));
        genko::test::write_bytes(dir + "/project.json", "old");
        const auto done = journal::repair(to_path(dir));
        QCOMPARE(done.aborted, std::vector<std::string>({b}));
        QCOMPARE(journal::read_lines(to_path(dir) / "studio" / "journal.jsonl").values.back(), journal::abort_line(b));
        QVERIFY(!QFile::exists(dir + "/studio/audit.jsonl"));
        // and without project.json at all (a book that was being made)
        const QString empty = new_book();
        genko::test::write_bytes(empty + "/studio/journal.jsonl", lines({prepare(a, "edit", 1, nullptr, state('1'))}));
        QCOMPARE(journal::repair(to_path(empty)).aborted, std::vector<std::string>({a}));
    }

    void repairKeepsACutLine() {
        const QString dir = new_book();
        const auto a = txn_id('a');
        const std::string complete = lines({prepare(a, "edit", 1, nullptr, state('1'), genko::storage::sha256_hex("p")),
                                            journal::commit_line(a, 1)});
        const std::string fragment = "{\"v\":4,\"kind\":\"prepare\",\"txn\":\"bbbb";
        genko::test::write_bytes(dir + "/project.json", "p");
        genko::test::write_bytes(dir + "/studio/journal.jsonl", complete + fragment);
        const auto done = journal::repair(to_path(dir));
        QCOMPARE(done.journal_tail, fragment);
        QCOMPARE(genko::test::read_bytes(dir + "/studio/journal.jsonl"), complete);  // (the lines before stay)
        QCOMPARE(genko::test::read_bytes(dir + "/studio/journal.partial"), fragment + "\n");
        // a repair stopped after keeping the line and before cutting it: the line is not kept twice
        genko::test::write_bytes(dir + "/studio/journal.jsonl", complete + fragment);
        journal::repair(to_path(dir));
        QCOMPARE(genko::test::read_bytes(dir + "/studio/journal.partial"), fragment + "\n");
        QCOMPARE(genko::test::read_bytes(dir + "/studio/journal.jsonl"), complete);
        // a second cut line is added after the first
        genko::test::write_bytes(dir + "/studio/journal.jsonl", complete + "xyz");
        journal::repair(to_path(dir));
        QCOMPARE(genko::test::read_bytes(dir + "/studio/journal.partial"), fragment + "\nxyz\n");
        // the audit's cut line is kept apart in the same way
        genko::test::write_bytes(dir + "/studio/audit.jsonl", "{\"rev\": 1}\n{\"v\":4,\"tx");
        QCOMPARE(journal::repair(to_path(dir)).audit_tail, std::string("{\"v\":4,\"tx"));
        QCOMPARE(genko::test::read_bytes(dir + "/studio/audit.jsonl"), std::string("{\"rev\": 1}\n"));
        QCOMPARE(genko::test::read_bytes(dir + "/studio/audit.partial"), std::string("{\"v\":4,\"tx\n"));
    }

    void auditOncePerTransaction() {
        const QString dir = new_book();
        const auto a = txn_id('a');
        genko::test::write_bytes(dir + "/studio/audit.jsonl",
                                 "{\"rev\": 3, \"actor\": \"human:a\", \"at\": 1.5, \"changes\": []}\n"
                                 "not json\n" +
                                     lines({Json::object({{"v", 4}, {"txn", a}, {"rev", 2}}), Json::object({{"v", 4}, {"txn", a}, {"rev", 2}}),
                                            Json::object({{"v", 4}, {"kind", "migrated"}, {"txn", txn_id('m')}})}));
        const auto entries = journal::audit_entries(to_path(dir));
        QCOMPARE(entries.size(), std::size_t{3});  // (v3 line, a once, migrated)
        QCOMPARE(entries[0]["rev"], Json(3));
    }

    void stateIsCanonical() {
        const Json one = genko::core::parse_python_json(R"({"version": 4, "writer": {"app": "x"}, "revision": 5, "title": "東京", "b": [1, 2.5, {"z": 1, "a": null}]})");
        const Json two = genko::core::parse_python_json(R"({"b": [1, 2.5, {"a": null, "z": 1}], "revision": 9, "title": "東京", "version": 4})");
        QCOMPARE(genko::storage::state_text(one), std::string("{\"b\":[1,2.5,{\"a\":null,\"z\":1}],\"title\":\"東京\",\"version\":4}"));
        QCOMPARE(genko::storage::state_ref(one), genko::storage::state_ref(two));  // (revision and writer aside, key order aside)
        QCOMPARE(genko::storage::state_ref(one), genko::storage::AssetStore::ref(genko::storage::state_text(two)));
        Json three = two;
        three["b"][1] = 2;  // (an int is not the float)
        QVERIFY(genko::storage::state_ref(three) != genko::storage::state_ref(two));
    }
};

QTEST_GUILESS_MAIN(TestJournal)
#include "test_journal.moc"
