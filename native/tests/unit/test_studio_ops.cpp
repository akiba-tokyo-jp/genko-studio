// The studio ops (M5) where this build goes past Python on purpose (SPEC COMP-01a): what only a person holds — a panel's
// regions set wholesale, a character's approval (in the bible, or one character upserted), an approved character
// deleted, a request for approval, the style a person fixed — refused to an agent (Python lets the agent do it), a
// person doing each; an approved
// character that is gone recorded as an approval gone (core::gate_changes); a count past 64 bits refused; pages an op
// only looks at left shared with the book they came from (no copy for a change that is not made); a set of thousands
// of pinned keys kept in O(n log n). (Each op against Python: test_contract_studio.)

#include <QtTest>

#include <QElapsedTimer>

#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/gates.hpp"
#include "core/model.hpp"
#include "render/ops_registry.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::ApplyError;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;

namespace {

const CommandBus& bus() {
    static const CommandBus b(genko::render::ops_registry());
    return b;
}

const std::string kPerson = "human:作者";
const std::string kAgent = "ai:hermes";

// A book with a studio: c-ken's sheet approved (locked, its sheet and face), c-yuki not, c-mio's approval taken back
// (revoke: unlocked, the sheet a person approved still there); a request for approval and a
// fix for the agent; the style fixed from page 1; page 1's first panel with a person's region.
Document studio_book() {
    Document doc = genko::core::new_episode("試験", Num(1), 2, genko::core::PageSpec::a4_mono());
    doc.bible.characters = Json::parse(R"([
        {"id": "c-yuki", "name": "ユキ"},
        {"id": "c-ken", "name": "ケン", "locked": true,
         "refs": [{"asset": "sha256:aa", "kind": "sheet", "approved_by": "human:作者"},
                  {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]},
        {"id": "c-mio", "name": "ミオ", "locked": false, "refs": [{"asset": "sha256:ee", "kind": "sheet", "approved_by": "human:作者"}]}])");
    doc.studio = Json::parse(R"({"style": {"notes": ["細い線"], "locked": {"from_page": "p1", "rev": 3}}})");
    auto& page = doc.edit_page(0);
    page.frames[0].panel = Json::parse(R"({"status": "briefed", "regions": [{"id": "rg_a", "kind": "face", "rect_mm": [1, 2, 3, 4], "source": "user"}]})");
    doc.tickets = Json::array({Json::parse(R"({"id": "t-gate", "kind": "gate", "item": "name", "status": "open", "assignee": "human"})"),
                               Json::parse(R"({"id": "t-fix", "kind": "fix", "status": "open", "assignee": "agent"})")});
    return doc;
}

std::string leaf_id(const Document& doc) { return doc.page(0).frames[0].id; }

// The batch refused (with this code and these words in its error) and the book left as it was.
std::string refused(const Document& doc, const Json& batch, const std::string& actor) {
    try {
        (void)bus().apply(doc, batch, Actor(actor));
    } catch (const ApplyError& e) {
        return e.code() + ": " + e.what();
    }
    return "(applied)";
}

}  // namespace

class TestStudioOps : public QObject {
    Q_OBJECT

private slots:
    void whatOnlyAPersonHolds_data() {
        QTest::addColumn<QString>("batch");
        QTest::addColumn<QString>("words");
        const char* regions = "only a person can set a panel's regions with set_panel";
        const char* approval = "only a person can change a character's approval";
        QTest::newRow("set_panel regions") << R"([{"op": "set_panel", "page": 1, "frame_id": "$LEAF", "set": {"regions": []}}])" << regions;
        QTest::newRow("set_panel unset regions") << R"([{"op": "set_panel", "page": 1, "frame_id": "$LEAF", "unset": ["regions"]}])" << regions;
        QTest::newRow("set_bible drops c-ken") << R"([{"op": "set_bible", "characters": [{"id": "c-yuki"}]}])" << approval;
        QTest::newRow("set_bible unlocks c-ken")
            << R"([{"op": "set_bible", "characters": [{"id": "c-yuki"}, {"id": "c-ken", "locked": false, "refs": [{"asset": "sha256:aa", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}]}])"
            << approval;
        QTest::newRow("set_bible another sheet")
            << R"([{"op": "set_bible", "characters": [{"id": "c-yuki"}, {"id": "c-ken", "locked": true, "refs": [{"asset": "sha256:cc", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}]}])"
            << approval;
        QTest::newRow("set_bible locks c-yuki")
            << R"([{"op": "set_bible", "characters": [{"id": "c-yuki", "locked": true}, {"id": "c-ken", "locked": true, "refs": [{"asset": "sha256:aa", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}]}])"
            << approval;
        // (upsert_character: an approval given; an approved character an agent cannot touch at all, as in Python —
        // neither unlock it nor change its sheet and face)
        QTest::newRow("upsert_character locks c-yuki") << R"([{"op": "upsert_character", "character": {"id": "c-yuki", "name": "ユキ", "locked": true}}])"
                                                       << approval;
        QTest::newRow("upsert_character a new one locked") << R"([{"op": "upsert_character", "character": {"id": "c-new", "locked": 1}}])" << approval;
        QTest::newRow("upsert_character unlocks c-ken") << R"([{"op": "upsert_character", "character": {"id": "c-ken", "name": "ケン"}, "unlock": true}])"
                                                        << "character c-ken is locked (a person can unlock it)";
        QTest::newRow("upsert_character another sheet for c-ken")
            << R"([{"op": "upsert_character", "character": {"id": "c-ken", "locked": true, "refs": [{"asset": "sha256:cc", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}, "unlock": true}])"
            << "character c-ken is locked (a person can unlock it)";
        QTest::newRow("delete_character c-ken") << R"([{"op": "delete_character", "id": "c-ken", "force": true}])"
                                                << "only a person can delete an approved character (c-ken)";
        QTest::newRow("set_ticket a request for approval") << R"([{"op": "set_ticket", "id": "t-gate", "status": "done"}])"
                                                           << "only a person can change a request for approval (ticket t-gate)";
        QTest::newRow("set_studio style.locked") << R"([{"op": "set_studio", "style": {"locked": null}}])"
                                                 << "only a person can change the style fixed by the pilot page (style.locked)";
        QTest::newRow("set_studio style replaced") << R"([{"op": "set_studio", "style": "plain"}])"
                                                   << "only a person can change the style fixed by the pilot page (style.locked)";
    }

    void whatOnlyAPersonHolds() {
        QFETCH(QString, batch);
        QFETCH(QString, words);
        const Document doc = studio_book();
        const Json ops = genko::core::parse_python_json(batch.replace(QStringLiteral("$LEAF"), QString::fromStdString(leaf_id(doc))).toStdString());
        // an agent: refused, as the other things only a person does are (code "apply"), after a change it made before
        Json with_note = Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "前置き"}})});
        for (const Json& op : ops) with_note.push_back(op);
        const std::string said = refused(doc, with_note, kAgent);
        QVERIFY2(said.starts_with("apply: ops[1] ") && said.find(words.toStdString()) != std::string::npos, said.c_str());
        // a person: done
        const auto done = bus().apply(doc, ops, Actor(kPerson));
        QCOMPARE(done.applied.size(), ops.size());
    }

    void whatAnAgentStillDoes() {
        // (beside them, what is not a person's: the style's notes, a fix for the agent, a character's name and other
        // refs, the bible kept with its approvals, a character with no approval upserted or given in the bible — the
        // sheet and face of one that is not locked references until a person approves one, as attach_reference gives
        // them, also after a person took the approval back)
        const Document doc = studio_book();
        for (const char* batch : {
                 R"([{"op": "set_studio", "style": {"notes": ["太い線"]}}])",
                 R"([{"op": "set_ticket", "id": "t-fix", "assignee": "human"}])",
                 R"([{"op": "set_bible", "plot": "筋", "characters": [{"id": "c-yuki", "name": "雪", "refs": [{"asset": "sha256:dd", "kind": "pose"}]}, {"id": "c-ken", "name": "健", "locked": true, "refs": [{"asset": "sha256:aa", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}]}])",
                 R"([{"op": "delete_character", "id": "c-yuki"}])",
                 R"([{"op": "set_bible", "plot": "筋だけ"}])",
                 R"([{"op": "upsert_character", "character": {"id": "c-yuki", "name": "雪", "locked": false, "refs": [{"asset": "sha256:dd", "kind": "pose"}]}}])",
                 R"([{"op": "upsert_character", "character": {"id": "c-new", "name": "新"}}])",
                 R"([{"op": "upsert_character", "character": {"id": "c-yuki", "refs": [{"asset": "sha256:cc", "kind": "sheet"}]}}])",
                 R"([{"op": "upsert_character", "character": {"id": "c-mio", "name": "ミオ"}}])",
                 R"([{"op": "set_bible", "characters": [{"id": "c-yuki", "refs": [{"asset": "sha256:cc", "kind": "sheet"}, {"asset": "sha256:dd", "kind": "face"}]}, {"id": "c-ken", "name": "ケン", "locked": true, "refs": [{"asset": "sha256:aa", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "sha256:bb", "kind": "face", "approved_by": "human:作者"}]}, {"id": "c-mio", "refs": [{"asset": "sha256:ff", "kind": "sheet"}]}]}])"}) {
            const auto done = bus().apply(doc, genko::core::parse_python_json(batch), Actor(kAgent));
            QCOMPARE(done.applied.size(), std::size_t{1});
        }
        // a person's region kept by an agent's other set_panel changes
        const auto done = bus().apply(doc, Json::array({Json::object({{"op", "set_panel"}, {"page", 1}, {"frame_id", leaf_id(doc)},
                                                                       {"set", Json::object({{"shot", "CU"}})}})}),
                                      Actor(kAgent));
        QCOMPARE(done.doc.page(0).frames[0].panel->at("regions").size(), std::size_t{1});
    }

    void anApprovedCharacterGoneIsAnApprovalGone() {
        // (Python's gate_changes looks at the keys of the new state only: an approved character deleted is not in it)
        const Json before = Json::parse(R"({"pages": [], "bible": {"characters": [{"id": "c-ken", "locked": true}, {"id": "c-yuki", "locked": false},
                                            {"id": "c-mio", "locked": true}]}, "studio": {}})");
        const Json after = Json::parse(R"({"pages": [], "bible": {"characters": [{"id": "c-mio", "locked": true}, {"id": "c-new", "locked": true}]},
                                           "studio": {}})");
        QCOMPARE(genko::core::gate_changes(&before, after),
                 Json::parse(R"([{"what": "character:c-ken:locked", "from": true, "to": null},
                                 {"what": "character:c-new:locked", "from": null, "to": true}])"));
        // and back (Undo): from null to true, as Python records it
        QCOMPARE(genko::core::gate_changes(&after, before),
                 Json::parse(R"([{"what": "character:c-ken:locked", "from": null, "to": true},
                                 {"what": "character:c-new:locked", "from": true, "to": null}])"));
    }

    void aCountPast64BitsIsRefused() {
        Document doc = studio_book();
        const std::string leaf = leaf_id(doc);
        const auto request = [&](const char* id) {
            return Json::array({Json::object({{"op", "open_request"},
                                              {"request", Json::object({{"id", id}, {"purpose", "art"}, {"mode", "new"},
                                                                        {"target", Json::object({{"page", 1}, {"frame_id", leaf}})}})}})});
        };
        (*doc.edit_page(0).frames[0].panel)["attempts"] = Json::parse(R"({"requests": 9223372036854775807, "images": 0, "fix_rounds": 0})");
        const auto past_int64 = bus().apply(doc, request("rq_a"), Actor(kAgent));  // (Python's int: exact)
        QCOMPARE(past_int64.doc.page(0).frames[0].panel->at("attempts")["requests"], Json(std::uint64_t{9223372036854775808ULL}));
        (*doc.edit_page(0).frames[0].panel)["attempts"]["requests"] = std::uint64_t{18446744073709551615ULL};
        const std::string said = refused(doc, request("rq_b"), kAgent);
        QVERIFY2(said.find("a count past 2**64 - 1 is too large for this build") != std::string::npos, said.c_str());
    }

    void pagesAnOpOnlyLooksAtStayShared() {
        Document doc = studio_book();
        (*doc.edit_page(0).frames[0].panel)["status"] = "skip";
        doc.tickets.push_back(Json::parse(R"({"id": "t-done", "kind": "fix", "status": "done", "assignee": "agent", "page_index": 1})"));
        doc.tickets.back()["frame_id"] = leaf_id(doc);
        doc.tickets.push_back(Json::parse(R"({"id": "t-open", "kind": "fix", "status": "open", "assignee": "agent", "page_index": 1})"));
        doc.tickets.back()["frame_id"] = leaf_id(doc);
        const std::string leaf = leaf_id(doc);
        for (const auto& [batch, actor] : std::initializer_list<std::pair<Json, std::string>>{
                 // a question about a panel (the panel is looked at, not changed)
                 {Json::array({Json::object({{"op", "ask_human"}, {"text", "どうしますか"}, {"page", 1}, {"frame_id", leaf}})}), kAgent},
                 // a fix reopened on a panel marked skip (its status is kept)
                 {Json::array({Json::object({{"op", "reopen_ticket"}, {"id", "t-done"}, {"note", "もう一度"}})}), kPerson},
                 // a fix done on a panel that does not wait for it
                 {Json::array({Json::object({{"op", "resolve_ticket"}, {"id", "t-open"}, {"note", "直した"}})}), kAgent},
                 // candidates for a panel that is not there (kept aside as orphans)
                 {Json::array({Json::object({{"op", "import_candidates"}, {"page", 1}, {"frame_id", "no-such-panel"},
                                             {"candidates", Json::parse(R"([{"asset": "sha256:ee", "px": [10, 10], "origin": {"kind": "agent"}}])")}})}),
                  kAgent}}) {
            const auto done = bus().apply(doc, batch, Actor(actor));
            QCOMPARE(done.applied.size(), std::size_t{1});
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                QVERIFY2(done.doc.pages[i].get() == doc.pages[i].get(), genko::core::dump_python(batch).c_str());
            }
        }
    }

    void thousandsOfPinnedKeys() {
        // (sorted(set(...)) of 60 000 keys: a set kept by hash, a sort in O(n log n) — an O(n²) one takes minutes here)
        const Document doc = studio_book();
        Json pin = Json::array();
        for (int i = 60000; i-- > 0;) pin.push_back("key-" + std::to_string(i));
        pin.push_back("key-7");  // (once in the set)
        QElapsedTimer timer;
        timer.start();
        const auto done = bus().apply(doc, Json::array({Json::object({{"op", "set_panel"}, {"page", 1}, {"frame_id", leaf_id(doc)}, {"pin", pin}})}),
                                      Actor(kPerson));
        const Json& pinned = done.doc.page(0).frames[0].panel->at("pinned");
        QCOMPARE(pinned.size(), std::size_t{60000});
        QCOMPARE(pinned[0], Json("key-0"));
        QCOMPARE(pinned[1], Json("key-1"));
        QCOMPARE(pinned[2], Json("key-10"));
        QVERIFY2(timer.elapsed() < 20000, std::to_string(timer.elapsed()).c_str());
        // numbers by value (1, 1.0 and True are one key: the first kept), texts and numbers together refused as Python does
        const auto numbers = bus().apply(doc, genko::core::parse_python_json(
                                                  ("[{\"op\": \"set_panel\", \"page\": 1, \"frame_id\": \"" + leaf_id(doc) + "\", \"pin\": [3, 1, 2.5, 1.0, true, -4]}]").c_str()),
                                         Actor(kPerson));
        QCOMPARE(numbers.doc.page(0).frames[0].panel->at("pinned"), Json::parse("[-4, 1, 2.5, 3]"));
        const std::string mixed = refused(doc, genko::core::parse_python_json(
                                                   ("[{\"op\": \"set_panel\", \"page\": 1, \"frame_id\": \"" + leaf_id(doc) + "\", \"pin\": [\"a\", 1]}]").c_str()),
                                          kPerson);
        QVERIFY2(mixed.find("'<' not supported between instances of") != std::string::npos, mixed.c_str());
    }
};

QTEST_GUILESS_MAIN(TestStudioOps)
#include "test_studio_ops.moc"
