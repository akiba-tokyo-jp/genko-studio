// core::CommandBus and the M1 ops (set_note, set_meta, name_ok, lock_page, unlock_page, set_autosave, add_page,
// delete_page): Python's behaviour and messages ("ops[i] <op>: <reason> ‖ <op> takes {…}"), the page locks and the
// person-only gate, the warnings after a batch, the batch that fails leaving the book as it was, and the pages that
// stay shared with the book they came from. (The same batches against Python's apply_ops: test_contract_save.)

#include <QtTest>

#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "render/ops_registry.hpp"
#include "testsupport.hpp"

using genko::core::ApplyError;
using genko::core::Actor;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;

namespace {

Json ops(const char* text) { return genko::core::parse_python_json(text); }

Document book(int pages = 3) {
    Document doc = genko::core::new_episode("t", 1, pages, genko::core::PageSpec::a4_mono());
    doc.revision = 4;
    return doc;
}

// The bus with every op of this build, as the command line has it.
const CommandBus& bus() {
    static const CommandBus b(genko::render::ops_registry());
    return b;
}

// The error of a batch: "code: message".
std::string error_of(const Document& doc, const Json& batch, const std::string& actor = "genko", bool dry_run = false) {
    try {
        bus().apply(doc, batch, Actor(actor), dry_run);
    } catch (const ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return "(applied)";
}

genko::core::ApplyResult apply(const Document& doc, const char* batch, const std::string& actor = "genko") {
    return bus().apply(doc, ops(batch), Actor(actor));
}

std::vector<std::string> strings(const Json& list) {
    std::vector<std::string> out;
    for (const Json& item : list) out.push_back(item.get<std::string>());
    return out;
}

}  // namespace

class TestCommandBus : public QObject {
    Q_OBJECT

private slots:
    // One registry for the whole build (render::ops_registry: core's ops and the ops that draw), each module's ops
    // registered by its own register_*_ops; core::OpRegistry::builtin() has core's alone.
    void registries() {
        const auto& core_ops = genko::core::OpRegistry::builtin();
        const auto& all = genko::render::ops_registry();
        for (const char* name : {"set_note", "add_page", "split_frame", "add_stroke", "erase", "add_layer", "add_ruler",
                                 "ruler_to_layer"}) {
            QVERIFY2(core_ops.find(name) != nullptr, name);
            QVERIFY2(all.find(name) != nullptr, name);
        }
        for (const char* name : {"add_tone", "set_tone", "delete_tone", "add_effect", "effect_to_layer", "add_figure",
                                 "render_prims", "trace_prims", "camera_from_ruler"}) {
            QVERIFY2(core_ops.find(name) == nullptr, name);
            QVERIFY2(all.find(name) != nullptr, name);
        }
        const std::vector<std::string> core_names = core_ops.names();
        const std::vector<std::string> all_names = all.names();
        for (const std::string& name : core_names) QVERIFY2(all.find(name) != nullptr, name.c_str());
        QCOMPARE(all_names.size(), core_names.size() + 3 + 4 + 17);  // (tones, effect lines, 3D)
        // the bus without a registry has core's
        QCOMPARE(error_of(book(), ops(R"([{"op": "add_tone", "page": 1}])")).substr(0, 7), std::string("(applie"));
        try {
            CommandBus().apply(book(), ops(R"([{"op": "add_tone", "page": 1}])"), Actor());
            QFAIL("core's bus has no tone ops");
        } catch (const ApplyError& error) {
            QCOMPARE(error.code(), std::string("not_yet_ported"));
        }
    }

    // An op's area, resolved before the op as Python's apply_ops does (selops.resolve): a rect or an ellipse alone is its
    // polygon for every op; one that needs the selection tools is not ported; the page comes first.
    void areas() {
        const Document doc = book();
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1, "note": "x", "area": {"rect": [0, 0, 10, 5]}}])")),
                 std::string("(applied)"));
        const auto rect = genko::core::resolve_plain_area(ops(R"({"rect": [1, 2, 10, "5"]})"));
        QVERIFY(rect.has_value());
        QCOMPARE(genko::core::dump_python(*rect), std::string("{\"poly\": [[1.0, 2.0], [11.0, 2.0], [11.0, 7.0], [1.0, 7.0]]}"));
        const auto ellipse = genko::core::resolve_plain_area(ops(R"({"ellipse": [0, 0, 20, 10]})"));
        QVERIFY(ellipse.has_value());
        QCOMPARE((*ellipse)["poly"].size(), std::size_t{72});
        QCOMPARE(genko::core::dump_python((*ellipse)["poly"][0]), std::string("[20.0, 5.0]"));
        QVERIFY(!genko::core::resolve_plain_area(ops(R"({"rect": [0, 0, 1, 1], "invert": true})")).has_value());
        QVERIFY(!genko::core::resolve_plain_area(ops(R"({"poly": [[0, 0], [1, 0], [1, 1]]})")).has_value());
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1, "area": {"rect": [0, 0, 10]}}])")),
                 std::string("apply: ops[0] set_note: a value of the wrong type (not enough values to unpack (expected 4, got 3)) ‖ "
                             "set_note takes {page: int}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "note": "x", "area": {"rect": [0, 0, 10, 5]}}])")),
                 std::string("apply: ops[0] set_note: page (int) is required ‖ set_note takes {page: int}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1, "area": {"all": true}}])")).substr(0, 46),
                 std::string("not_yet_ported: ops[0] set_note: an area of th"));
    }

    void canApprove() {
        QVERIFY(genko::core::can_approve("genko"));
        QVERIFY(genko::core::can_approve("human"));
        QVERIFY(genko::core::can_approve("human:作者"));
        QVERIFY(!genko::core::can_approve("ai:hermes"));
        QVERIFY(!genko::core::can_approve("legacy:unknown"));
        QVERIFY(!genko::core::can_approve("humanoid"));
        QVERIFY(!genko::core::can_approve(""));
    }

    void setNoteSharesTheOtherPages() {
        const Document doc = book();
        const auto result = apply(doc, R"([{"op": "set_note", "page": 2, "note": "メモ"}])");
        QCOMPARE(result.doc.page(1).note, std::string("メモ"));
        QCOMPARE(doc.page(1).note, std::string(""));  // (the book read is not changed)
        QVERIFY(result.doc.pages[0].get() == doc.pages[0].get());  // (copy-on-write: only page 2 was copied)
        QVERIFY(result.doc.pages[2].get() == doc.pages[2].get());
        QVERIFY(result.doc.pages[1].get() != doc.pages[1].get());
        QCOMPARE(strings(result.applied), std::vector<std::string>({"set_note"}));
        QVERIFY(result.warnings.empty());
        QCOMPARE(apply(doc, R"([{"op": "set_note", "page": "3", "note": 5}])").doc.page(2).note, std::string("5"));
        QCOMPARE(apply(doc, R"([{"op": "set_note", "page": 1.9, "note": null}])").doc.page(0).note, std::string("None"));
    }

    void pageErrors() {
        const Document doc = book();
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "note": "x"}])")),
                 std::string("apply: ops[0] set_note: page (int) is required ‖ set_note takes {page: int}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 9}])")),
                 std::string("apply: ops[0] set_note: no page 9 ‖ set_note takes {page: int}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": [1], "colour": 1}])")),
                 std::string("apply: ops[0] set_note: page (int) is required ‖ unknown keys: colour ‖ set_note takes {page: int}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "delete_page", "page": "x"}])")),
                 std::string("apply: ops[0] delete_page: page (int) is required ‖ delete_page takes {page: int}"));
    }

    void batchErrors() {
        const Document doc = book();
        QCOMPARE(error_of(doc, ops(R"({"op": "set_note"})")), std::string("apply: ops must be a JSON array"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_autosave"}, 5])")), std::string("apply: ops[1] must be an object"));
        QCOMPARE(error_of(doc, ops(R"([{"page": 1}])")), std::string("apply: ops[0] None: op is required"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "frobnicate"}])")), std::string("apply: ops[0] frobnicate: unknown op: frobnicate"));
        QCOMPARE(error_of(doc, ops(R"([{"op": 5}])")), std::string("apply: ops[0] 5: unknown op: 5"));
        // an op of the public list that this build does not have: refused, never skipped
        QCOMPARE(error_of(doc, ops(R"([{"op": "fill", "page": 1, "x_mm": 1, "y_mm": 2}])")),
                 std::string("not_yet_ported: ops[0] fill: fill is not in the C++ build yet"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1}, {"op": "approve", "page": 1}])")),
                 std::string("not_yet_ported: ops[1] approve: approve is not in the C++ build yet"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "for_pages", "pages": "all", "ops": []}])")),
                 std::string("apply: ops[0] for_pages: ops is the list of ops to run on each page"));
        // undo is an op only on its own (Python's _apply_one does not know it)
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1}, {"op": "undo"}])")),
                 std::string("apply: ops[1] undo: unknown op: undo ‖ undo takes {}"));
        // an op name Python cannot look up in its sets of ops: before the ops run (where Python stops with a
        // traceback), or, after an op that may change any page, when the op is checked
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_note", "page": 1}, {"op": ["set_note"], "page": 1}])")),
                 std::string("python_error: ops[1] ['set_note']: TypeError: unhashable type: 'list'"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page"}, {"op": {"a": 1}, "page": 1}])")),
                 std::string("apply: ops[1] {'a': 1}: a value of the wrong type (unhashable type: 'dict')"));
        // the page lock is checked before the op is looked up (Python's order)
        Document locked = book();
        locked.page_locks[locked.page(0).id] = "ai:x";
        QCOMPARE(error_of(locked, ops(R"([{"op": "frobnicate", "page": 1}])"), "ai:y"),
                 std::string("apply: ops[0] frobnicate: page 1 locked by ai:x"));
    }

    void undoOfTheSession() {
        const Document doc = book();
        QCOMPARE(error_of(doc, ops(R"([{"op": "undo"}])")), std::string("nothing_to_undo: nothing to undo"));
        const auto dry = bus().apply(doc, ops(R"([{"op": "undo"}])"), Actor(), true);
        QCOMPARE(strings(dry.applied), std::vector<std::string>({"undo"}));
        QVERIFY(!dry.has_warnings);
        QVERIFY(dry.doc.pages[0].get() == doc.pages[0].get());
    }

    void failedBatchChangesNothing() {
        const Document doc = book();
        const auto pages = doc.pages;
        QVERIFY(error_of(doc, ops(R"([{"op": "set_note", "page": 1, "note": "x"}, {"op": "set_meta", "title": "T"},
                                      {"op": "delete_page", "page": 1}, {"op": "set_note", "page": 9}])"))
                    .starts_with("apply: ops[3] set_note: no page 9"));
        QCOMPARE(doc.page(0).note, std::string(""));
        QCOMPARE(doc.title, std::string("t"));
        QCOMPARE(doc.pages.size(), std::size_t{3});
        for (std::size_t i = 0; i < pages.size(); ++i) QVERIFY(doc.pages[i].get() == pages[i].get());
    }

    void setMeta() {
        Document doc = book();
        auto result = apply(doc, R"([{"op": "set_meta", "title": "新題", "episode": "7", "binding": "left", "start_side": "right",
                                      "strict_gates": 1, "font_path": "/fonts/明朝.otf"}])");
        QCOMPARE(result.doc.title, std::string("新題"));
        QVERIFY(result.doc.episode.same(Num(7)));
        QCOMPARE(int(result.doc.binding), int(genko::core::Binding::Left));
        for (const auto& page : result.doc.pages) QCOMPARE(int(page->binding), int(genko::core::Binding::Left));
        QCOMPARE(result.doc.start_side.value(), std::string("right"));
        QVERIFY(result.doc.strict_gates);
        QCOMPARE(result.doc.font_path, std::string("/fonts/明朝.otf"));
        QVERIFY(!apply(result.doc, R"([{"op": "set_meta", "start_side": null}])").doc.start_side.has_value());
        const auto preset = apply(doc, R"([{"op": "set_meta", "preset": " Shueisha "}])");
        QCOMPARE(preset.doc.spec.preset.value(), std::string("shueisha"));
        QVERIFY(preset.doc.page(2).spec.width_mm.same(Num(257)));
        const auto webtoon = apply(doc, R"([{"op": "set_meta", "webtoon": true}])");
        QCOMPARE(webtoon.doc.spec.preset.value(), std::string("webtoon"));
        QCOMPARE(strings(webtoon.warnings), std::vector<std::string>({"ops[0] set_meta: unknown keys ignored: webtoon"}));
        const std::string usage = " ‖ set_meta takes {title: str?, episode: int?, preset: str?, binding: right|left?, "
                                  "start_side: left|right|null?, strict_gates: bool?, font_path: str?}";
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_meta", "episode": "x"}])")),
                 "apply: ops[0] set_meta: a value of the wrong type (invalid literal for int() with base 10: 'x')" + usage);
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_meta", "binding": "up"}])")),
                 "apply: ops[0] set_meta: a value of the wrong type ('up' is not a valid Binding)" + usage);
        QCOMPARE(error_of(doc, ops(R"([{"op": "set_meta", "start_side": "top"}])")),
                 "apply: ops[0] set_meta: start_side must be left, right or null" + usage);
    }

    void covers() {
        Document doc = book(2);
        doc.edit_page(1).extra["cover"] = Json::object({{"kind", "jacket"}, {"spine_mm", 12.5}, {"flap_mm", 80}});
        const auto result = apply(doc, R"([{"op": "set_meta", "preset": "b4"}])");
        QVERIFY(result.doc.page(1).spec.width_mm.value() > result.doc.page(0).spec.width_mm.value());  // (a jacket's own paper)
        QCOMPARE(result.doc.page(1).spec.preset.value(), std::string("cover"));
        // new pages go before the covers at the end
        const auto added = apply(doc, R"([{"op": "add_page", "count": 2}])");
        QCOMPARE(added.doc.pages.size(), std::size_t{4});
        QVERIFY(added.doc.page(3).extra.contains("cover"));
        QVERIFY(added.doc.page(3).id == doc.page(1).id);
        QVERIFY(added.doc.page(3).index.same(Num(4)));
    }

    void nameOkNeedsAPerson() {
        const Document doc = book();
        QCOMPARE(error_of(doc, ops(R"([{"op": "name_ok", "page": 1}])"), "ai:hermes"),
                 std::string("apply: ops[0] name_ok: name_ok needs a person (actor ai:hermes cannot approve) ‖ name_ok takes "
                             "{page: int, optional (all pages if omitted)}"));
        const auto one = apply(doc, R"([{"op": "name_ok", "page": 2}])", "human:作者");
        QVERIFY(one.doc.page(1).name_ok && one.doc.page(1).stage == "ink");
        QVERIFY(!one.doc.page(0).name_ok);
        const auto all = apply(doc, R"([{"op": "name_ok"}])");
        for (const auto& page : all.doc.pages) QVERIFY(page->name_ok && page->stage == "ink");
    }

    void pageLocks() {
        const Document doc = book();
        const std::string id1 = doc.page(0).id;
        QCOMPARE(error_of(doc, ops(R"([{"op": "lock_page", "page": 1, "agent": "ai:y"}])"), "ai:x"),
                 std::string("apply: ops[0] lock_page: cannot lock page 1 as ai:y (actor is ai:x) ‖ lock_page takes {page: int, agent: str}"));
        const auto locked = apply(doc, R"([{"op": "lock_page", "page": 1}])", "ai:x");
        QCOMPARE(locked.doc.page_locks[id1], Json("ai:x"));
        QCOMPARE(error_of(locked.doc, ops(R"([{"op": "set_note", "page": 1, "note": "x"}])"), "ai:y"),
                 std::string("apply: ops[0] set_note: page 1 locked by ai:x ‖ set_note takes {page: int}"));
        QCOMPARE(error_of(locked.doc, ops(R"([{"op": "delete_page", "page": 1}])"), "ai:y"),
                 std::string("apply: ops[0] delete_page: page 1 locked by ai:x ‖ delete_page takes {page: int}"));
        apply(locked.doc, R"([{"op": "set_note", "page": 1, "note": "x"}, {"op": "set_meta", "title": "y"}])", "ai:x");
        apply(locked.doc, R"([{"op": "set_meta", "title": "y"}, {"op": "set_autosave", "enabled": false}])", "ai:y");  // (book ops)
        // a person takes over an AI's lock; an AI cannot take or open a person's
        const auto human = apply(locked.doc, R"([{"op": "lock_page", "page": 1}])", "human:作者");
        QCOMPARE(human.doc.page_locks[id1], Json("human:作者"));
        QCOMPARE(error_of(human.doc, ops(R"([{"op": "lock_page", "page": 1}])"), "ai:x"),
                 std::string("apply: ops[0] lock_page: page 1 locked by human:作者 ‖ lock_page takes {page: int, agent: str}"));
        QCOMPARE(error_of(human.doc, ops(R"([{"op": "unlock_page", "page": 1}])"), "ai:x"),
                 std::string("apply: ops[0] unlock_page: page 1 locked by human:作者; ai:x cannot unlock it ‖ unlock_page takes {page: int}"));
        const auto unlocked = apply(human.doc, R"([{"op": "unlock_page", "page": 1}])", "human:other");
        QVERIFY(!unlocked.doc.page_locks.contains(id1));
        // the unnamed caller may lock in anyone's name; a page that is not there is refused (Python locks nothing and
        // says nothing)
        QCOMPARE(apply(doc, R"([{"op": "lock_page", "page": 2, "agent": "ai:z"}])").doc.page_locks[doc.page(1).id], Json("ai:z"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "lock_page", "page": 9}])"), "ai:x"),
                 std::string("apply: ops[0] lock_page: no page 9 ‖ lock_page takes {page: int, agent: str}"));
        QCOMPARE(error_of(doc, ops(R"([{"op": "unlock_page", "page": 9}])"), "ai:x"),
                 std::string("apply: ops[0] unlock_page: no page 9 ‖ unlock_page takes {page: int}"));
        // within one batch, later ops see the lock
        QVERIFY(error_of(doc, ops(R"([{"op": "lock_page", "page": 3}, {"op": "set_note", "page": 3}])"), "ai:x") == "(applied)");
    }

    void autosave() {
        const Document doc = book();
        QVERIFY(apply(doc, R"([{"op": "set_autosave"}])").doc.autosave);
        QVERIFY(!apply(doc, R"([{"op": "set_autosave", "enabled": 0}])").doc.autosave);
    }

    void addPage() {
        Document doc = book(3);
        doc.add_line(Num(2), "二");
        doc.add_line(Num(3), "三");
        doc.edit_page(1).spread_with = Num(3);
        doc.edit_page(2).onion_from = Num(2);
        doc.tickets = ops(R"([{"page_index": 2, "role": "ink"}, {"page_index": null}, {"page_index": "2"}, {"role": "x"}])");
        const std::string id2 = doc.page(1).id;
        const auto result = apply(doc, R"([{"op": "add_page", "count": 2, "after": 1}])");
        QCOMPARE(result.doc.pages.size(), std::size_t{5});
        for (std::size_t i = 0; i < 5; ++i) QVERIFY(result.doc.page(i).index.same(Num(static_cast<std::int64_t>(i + 1))));
        QCOMPARE(result.doc.page(3).id, id2);  // (old page 2 is page 4 now)
        QVERIFY(result.doc.page(1).id.starts_with("pg_") && result.doc.page(1).layers.size() == 4);
        QCOMPARE(result.doc.page(1).frames.size(), std::size_t{1});
        QVERIFY(result.doc.story[0].page_index.same(Num(4)));
        QVERIFY(result.doc.story[1].page_index.same(Num(5)));
        QVERIFY(result.doc.page(3).spread_with->same(Num(5)));
        QVERIFY(result.doc.page(4).onion_from->same(Num(4)));
        QCOMPARE(result.doc.tickets[0]["page_index"], Json(4));
        QVERIFY(result.doc.tickets[1]["page_index"].is_null());
        QCOMPARE(result.doc.tickets[2]["status"], Json("orphaned"));  // (Python: "2" is no page number)
        QVERIFY(!result.doc.tickets[3].contains("status"));
        QVERIFY(result.warnings.empty());  // (pages 4 and 5 face each other, as 2 and 3 did)
        // at the end by default; after 0 first; the page numbers' checks
        QCOMPARE(apply(doc, R"([{"op": "add_page"}])").doc.pages.size(), std::size_t{4});
        QVERIFY(apply(doc, R"([{"op": "add_page", "after": 0}])").doc.page(1).id == doc.page(0).id);
        const std::string usage = " ‖ add_page takes {count: int, after: int? (insert after this page; default after the last story "
                                  "page, before any covers)}";
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page", "count": 0}])")), "apply: ops[0] add_page: count is 1 to 200" + usage);
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page", "count": 201}])")), "apply: ops[0] add_page: count is 1 to 200" + usage);
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page", "after": 9}])")), "apply: ops[0] add_page: no page 9" + usage);
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page", "after": 9.5}])")), "apply: ops[0] add_page: no page 9.5" + usage);
        QVERIFY(apply(doc, R"([{"op": "add_page", "after": 2.5}])").doc.page(2).id != doc.page(2).id);  // (int(2.5): after page 2)
        QCOMPARE(error_of(doc, ops(R"([{"op": "add_page", "count": "many"}])")),
                 "apply: ops[0] add_page: a value of the wrong type (invalid literal for int() with base 10: 'many')" + usage);
    }

    void deletePage() {
        Document doc = book(3);
        doc.add_line(Num(1), "一");
        doc.add_line(Num(2), "二");
        doc.add_line(Num(3), "三");
        doc.edit_page(0).spread_with = Num(2);
        doc.page_locks[doc.page(1).id] = "ai:x";
        doc.tickets = ops(R"([{"page_index": 2}, {"page_index": 3}])");
        const std::string id3 = doc.page(2).id;
        const auto result = apply(doc, R"([{"op": "delete_page", "page": 2}])", "ai:x");
        QCOMPARE(result.doc.pages.size(), std::size_t{2});
        QCOMPARE(result.doc.page(1).id, id3);
        QVERIFY(result.doc.page(1).index.same(Num(2)));
        QCOMPARE(result.doc.story.size(), std::size_t{2});
        QCOMPARE(result.doc.story[1].text, std::string("三"));
        QVERIFY(result.doc.story[1].page_index.same(Num(2)));
        QVERIFY(!result.doc.page(0).spread_with.has_value());  // (its partner went away)
        QVERIFY(result.doc.page_locks.empty());
        QCOMPARE(result.doc.tickets[0]["status"], Json("orphaned"));
        QCOMPARE(result.doc.tickets[0]["page_index"], Json(2));
        QCOMPARE(result.doc.tickets[1]["page_index"], Json(2));
        QCOMPARE(error_of(book(1), ops(R"([{"op": "delete_page", "page": 1}])")),
                 std::string("apply: ops[0] delete_page: cannot delete the last page ‖ delete_page takes {page: int}"));
    }

    void warnings() {
        Document doc = book(2);
        doc.add_line(Num(7), "nowhere");
        doc.add_line(Num(1), "framed", "", std::string("no-such-frame"));
        doc.page_locks["pg_gone"] = "ai:x";
        doc.edit_page(1).spread_with = Num(9);
        doc.edit_page(0).spread_with = Num(2);  // (a right-bound book: 1 and 2 are the two sides of one leaf)
        const auto result = apply(doc, R"([{"op": "set_autosave", "enabled": true, "speed": 2}])");
        std::vector<std::string> want{"line " + doc.story[0].id + ": page 7 does not exist",
                                      "line " + doc.story[1].id + ": frame no-such-frame is not on page 1",
                                      "page lock on unknown page pg_gone",
                                      "spread 1-2: pages 1 and 2 are two sides of one leaf, not a spread",
                                      "spread 2-9: spread partner 9 missing", "ops[0] set_autosave: unknown keys ignored: speed"};
        QCOMPARE(strings(result.warnings), want);
    }

    void journalRecord() {
        const Json op = ops(R"({"op": "put_raster", "page": 1, "png_base64": "iVBORw0KGgo=", "layer": "bg"})");
        QCOMPARE(genko::core::journal_op(op).dump(), std::string(R"({"op":"put_raster","page":1,"png_base64":"<12 base64 chars>","layer":"bg"})"));
        const auto result = apply(book(), R"([{"op": "set_note", "page": 1, "note": "n", "png_base64": "AAAA"}])");
        QCOMPARE(result.journal_ops[0]["png_base64"], Json("<4 base64 chars>"));
    }
};

QTEST_GUILESS_MAIN(TestCommandBus)
#include "test_command_bus.moc"
