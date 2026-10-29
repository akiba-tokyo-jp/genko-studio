// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): the notes of a page as PhotoCraft v0.2.0's own tests check them —
// crates/engine/src/notes_cmds.rs (add, edit, move with its box, delete one and all, the refusals) and crates/io/src/
// annotations_map.rs (an Anno block read and written again byte for byte, a sound note kept, the dates) — with Genko's
// actors: an agent (ai:) adds and changes its own notes and never a person's, a person changes any; the JSON as written;
// a .pcraft's notes; the import's ops pulled inside the paper; MCP's apply_ops not listing the ops.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/annotations.hpp"
#include "core/clock.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/notes_import.hpp"
#include "render/ops_annotations.hpp"
#include "render/ops_production.hpp"
#include "render/ops_registry.hpp"
#include "render/pcraft_annotations.hpp"
#include "render/psd_annotations.hpp"
#include "service/studio_service.hpp"

using namespace genko;
using core::Json;
namespace pa = render::psd_annotations;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor(actor));
}
core::Document run(const core::Document& doc, const Json& op, const char* actor = "human:test") { return run_ops(doc, Json::array({op}), actor).doc; }
Json report(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    const auto applied = run_ops(doc, Json::array({op}), actor);
    return applied.results.empty() ? Json() : applied.results[0];
}
std::string refusal(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    try {
        run_ops(doc, Json::array({op}), actor);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

core::Document book() {
    auto doc = core::new_episode("注釈", core::Num(1), 1, core::PageSpec::custom(100, 80, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    return doc;
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

const core::Annotation& note(const core::Document& doc, std::size_t i) { return doc.page(0).annotations.at(i); }

std::string u16(unsigned v) { return std::string{static_cast<char>(v >> 8), static_cast<char>(v & 0xff)}; }
std::string u32(unsigned v) { return u16(v >> 16) + u16(v & 0xffff); }

// keep.psd's note (native/tests/data/psd_keep/make_psd_keep.py), and a sound note after it.
std::string one_text_note() {
    const std::string text = std::string("\xFE\xFF", 2) + u16(0x30E1) + u16(0x30E2) + u16('\r') + u16('b');  // (メモ⏎b)
    std::string one = std::string("txtA") + '\x01' + '\x00' + u16(1);
    for (const unsigned v : {2u, 2u, 6u, 6u, 2u, 8u, 40u, 60u}) one += u32(v);
    one += u16(0) + u16(65535) + u16(52428) + u16(0) + u16(0);
    for (const std::string& p : {std::string("author"), std::string("note"), std::string("D:20210521231029+01'00'")}) {
        std::string body = std::string(1, static_cast<char>(p.size())) + p;
        if (body.size() % 2 != 0) body += '\0';
        one += body;
    }
    one += u32(static_cast<unsigned>(text.size() + 12)) + "txtC" + u32(static_cast<unsigned>(text.size())) + text;
    return u32(static_cast<unsigned>(one.size() + 4)) + one;
}
std::string sound_note() {
    const std::string body = std::string("sndA") + std::string(40, '\x07');
    return u32(static_cast<unsigned>(body.size() + 4)) + body;
}
std::string anno(const std::vector<std::string>& entries) {
    std::string out = u16(2) + u16(1) + u32(static_cast<unsigned>(entries.size()));
    for (const std::string& e : entries) out += e;
    return out;
}

}  // namespace

class TestAnnotations : public QObject {
    Q_OBJECT

private slots:
    // notes_cmds.rs add_edit_move_delete_with_undo: added where asked with its text, author and colour; edited and moved
    // (its box along); deleted one and all.
    void addEditMoveDelete() {
        const core::ScopedClock clock("2026-10-29T08:00:00");
        core::Document doc = book();
        const Json made = report(doc, op("add_annotation", {{"at_mm", Json::array({10, 12})}, {"text", "直して"}, {"author", "QA"}, {"color", "#ff0000"}}));
        QVERIFY2(made.contains("annotation"), made.dump().c_str());
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({10, 12})}, {"text", "直して"}, {"author", "QA"}, {"color", "#ff0000"},
                                             {"popup_mm", Json::array({20, 20, 40, 30})}}));
        QCOMPARE(doc.page(0).annotations.size(), std::size_t(1));
        const core::Annotation& a = note(doc, 0);
        QVERIFY(core::annotation_id(a.id));
        QCOMPARE(a.text, std::string("直して"));
        QCOMPARE(a.author, std::string("QA"));
        QCOMPARE(a.color, (std::array<int, 3>{255, 0, 0}));
        QVERIFY(a.open);  // (a new note's box open)
        QCOMPARE(a.actor, std::string("human:test"));
        QVERIFY(core::annotation_time(a.modified));
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({50, 40})}, {"color", Json::array({1, 2, 3})}, {"open", false}}));
        QCOMPARE(note(doc, 1).color, (std::array<int, 3>{1, 2, 3}));
        QVERIFY(!note(doc, 1).open);
        QCOMPARE(note(doc, 1).author, std::string());
        const std::string id = note(doc, 0).id;
        // edited and moved: its box along
        doc = run(doc, op("set_annotation", {{"id", id}, {"text", "直した"}, {"at_mm", Json::array({20, 12})}}));
        QCOMPARE(note(doc, 0).text, std::string("直した"));
        QCOMPARE(note(doc, 0).at_mm, (std::array<double, 2>{20, 12}));
        QCOMPARE(*note(doc, 0).popup_mm, (std::array<double, 4>{30, 20, 40, 30}));
        // nothing new: not changed
        const Json same = report(doc, op("set_annotation", {{"id", id}, {"text", "直した"}}));
        QCOMPARE(same["changed"], Json(false));
        // opened and closed: its date as it was
        const std::string when = note(doc, 0).modified;
        doc = run(doc, op("set_annotation", {{"id", id}, {"open", false}, {"popup_mm", nullptr}}));
        QVERIFY(!note(doc, 0).open && !note(doc, 0).popup_mm);
        QCOMPARE(note(doc, 0).modified, when);
        // deleted one, then all
        doc = run(doc, op("delete_annotations", {{"ids", Json::array({note(doc, 1).id})}}));
        QCOMPARE(doc.page(0).annotations.size(), std::size_t(1));
        const Json gone = report(doc, op("delete_annotations", {{"all", true}}));
        QCOMPARE(gone["deleted"], Json(1));
        doc = run(doc, op("delete_annotations", {{"all", true}}));
        QVERIFY(doc.page(0).annotations.empty());
        QVERIFY(refusal(doc, op("delete_annotations", {{"all", true}})).find("the page has no notes") != std::string::npos);
    }

    // notes_cmds.rs bad_params and Genko's limits.
    void refusals() {
        core::Document doc = book();
        QVERIFY(refusal(doc, op("add_annotation", {{"at_mm", Json::array({1})}})).find("at_mm is [x, y] (mm)") != std::string::npos);
        QVERIFY(refusal(doc, op("add_annotation", {})).find("at_mm is [x, y] (mm)") != std::string::npos);
        QVERIFY(refusal(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}, {"color", "red"}})).find("color is") != std::string::npos);
        QVERIFY(refusal(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}, {"color", Json::array({0, 0, 300})}})).find("color is") != std::string::npos);
        QVERIFY(refusal(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}, {"text", std::string(65537, 'a')}})).find("at most 65536 bytes") != std::string::npos);
        QVERIFY(refusal(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}, {"modified", "yesterday"}})).find("modified is") != std::string::npos);
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}}));
        QVERIFY(refusal(doc, op("set_annotation", {{"id", "an_000000000000"}, {"text", "x"}})).find("no note an_000000000000 on the page") != std::string::npos);
        QVERIFY(refusal(doc, op("set_annotation", {{"id", note(doc, 0).id}})).find("give text, author, color, open, at_mm or popup_mm") != std::string::npos);
        QVERIFY(refusal(doc, op("delete_annotations", {})).find("give ids") != std::string::npos);
        QVERIFY(refusal(doc, op("delete_annotations", {{"ids", Json::array({"an_ffffffffffff"})}})).find("no note an_ffffffffffff") != std::string::npos);
        // (a page holds at most 1000)
        core::Document full = book();
        for (int i = 0; i < 1000; ++i) {
            core::Annotation a;
            a.id = "an_" + std::string(9, '0') + (i < 10 ? "00" : i < 100 ? "0" : "") + std::to_string(i);
            full.edit_page(0).annotations.push_back(std::move(a));
        }
        QVERIFY(refusal(full, op("add_annotation", {{"at_mm", Json::array({1, 1})}})).find("at most 1000 notes") != std::string::npos);
    }

    // Genko's actors: an agent adds and changes its own notes, never a person's; a person changes any, and an agent's note
    // a person edits is the person's.
    void agentsAndPeople() {
        core::Document doc = book();
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({1, 1})}, {"text", "人"}}), "human:ann");
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({5, 5})}, {"text", "AI"}}), "ai:claude");
        const std::string person = note(doc, 0).id, agent = note(doc, 1).id;
        QCOMPARE(note(doc, 1).actor, std::string("ai:claude"));
        QVERIFY(core::annotation_by_person(note(doc, 0).actor) && !core::annotation_by_person(note(doc, 1).actor));
        // (the agent: its own yes, the person's no — not even opened or moved)
        doc = run(doc, op("set_annotation", {{"id", agent}, {"text", "AI 2"}}), "ai:claude");
        QCOMPARE(note(doc, 1).text, std::string("AI 2"));
        for (const Json& change : {Json{{"text", "x"}}, Json{{"open", false}}, Json{{"at_mm", Json::array({9, 9})}}}) {
            Json o = op("set_annotation", change);
            o["id"] = person;
            const std::string said = refusal(doc, o, "ai:claude");
            QVERIFY2(said.find("a person wrote the note " + person + "; only a person can change it") != std::string::npos, said.c_str());
        }
        QVERIFY(refusal(doc, op("delete_annotations", {{"ids", Json::array({person})}}), "ai:claude").find("only a person can delete it") != std::string::npos);
        QVERIFY(refusal(doc, op("delete_annotations", {{"all", true}}), "ai:claude").find("only a person can delete it") != std::string::npos);
        // (another agent's note: an agent may — the rule is a person's notes)
        doc = run(doc, op("set_annotation", {{"id", agent}, {"open", false}}), "ai:other");
        // (the unnamed caller is a person: its notes "genko")
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({2, 2})}}), "genko");
        QCOMPARE(note(doc, 2).actor, std::string("genko"));
        QVERIFY(!refusal(doc, op("set_annotation", {{"id", note(doc, 2).id}, {"text", "x"}}), "ai:claude").empty());
        // a person edits the agent's note: the person's now
        doc = run(doc, op("set_annotation", {{"id", agent}, {"text", "人が直した"}}), "human:ann");
        QCOMPARE(note(doc, 1).actor, std::string("human:ann"));
        QVERIFY(!refusal(doc, op("set_annotation", {{"id", agent}, {"text", "y"}}), "ai:claude").empty());
        doc = run(doc, op("delete_annotations", {{"ids", Json::array({person, agent})}}), "human:ann");
        QCOMPARE(doc.page(0).annotations.size(), std::size_t(1));
        // (the review's P3-1: a person who moves an agent's note makes it the person's — no agent puts it back)
        doc = run(doc, op("add_annotation", {{"at_mm", Json::array({7, 7})}}), "ai:claude");
        const std::string moved = doc.page(0).annotations.back().id;
        doc = run(doc, op("set_annotation", {{"id", moved}, {"at_mm", Json::array({8, 8})}}), "human:ann");
        QCOMPARE(doc.page(0).annotations.back().actor, std::string("human:ann"));
        QVERIFY(!refusal(doc, op("set_annotation", {{"id", moved}, {"at_mm", Json::array({7, 7})}}), "ai:claude").empty());
        // (the review's P3-9: all: false beside ids is as none)
        doc = run(doc, op("delete_annotations", {{"ids", Json::array({moved})}, {"all", false}}), "human:ann");
        QVERIFY(std::none_of(doc.page(0).annotations.begin(), doc.page(0).annotations.end(), [&](const core::Annotation& a) { return a.id == moved; }));
        // (another name of a caller that cannot approve: an agent's note)
        QCOMPARE(render::annotations::writer_of(core::Actor("legacy:unknown")), std::string("ai:legacy:unknown"));
        QCOMPARE(render::annotations::writer_of(core::Actor("human")), std::string("genko"));
        // MCP's apply_ops does not take them (the C++ build's own ops, as J-a's), nor are they frames any more
        for (const char* name : {"add_annotation", "set_annotation", "delete_annotations"}) {
            QVERIFY(!service::agent_op(name));
            const auto frames = render::production_op_names();
            QVERIFY(std::find(frames.begin(), frames.end(), std::string_view(name)) == frames.end());
        }
    }

    // The JSON as written and read: an empty date kept; a broken one refused.
    void json() {
        core::Annotation a;
        a.id = "an_0123456789ab";
        a.at_mm = {1.5, 2};
        a.text = "a\nb";
        a.modified = "";
        a.actor = "ai:x";
        const Json written = core::to_json(std::vector<core::Annotation>{a});
        QCOMPARE(written[0]["modified"], Json(""));
        QCOMPARE(core::annotations_from_json(written)[0], a);
        Json bad = written;
        bad[0]["modified"] = "2026-10-29";
        bool refused = false;
        try {
            core::annotations_from_json(bad);
        } catch (const core::Error&) {
            refused = true;
        }
        QVERIFY(refused);
    }

    // annotations_map.rs: keep.psd's note read (its colour, author, text with its CR, its PDF date as UTC, the box) and
    // written again byte for byte; a sound note kept as its bytes, after the text notes.
    void psdAnno() {
        const std::string raw = anno({one_text_note(), sound_note()});
        const auto notes = pa::parse(raw);
        QVERIFY(notes);
        QCOMPARE(notes->size(), std::size_t(2));
        const pa::FileNote& n = (*notes)[0];
        QVERIFY(n.is_text && n.open);
        QCOMPARE(n.author, std::string("author"));
        QCOMPARE(n.text, std::string("メモ\nb"));
        QCOMPARE(n.color, (std::array<int, 3>{255, 204, 0}));
        QCOMPARE(n.at_px, (std::array<double, 2>{2, 2}));
        QCOMPARE(n.popup_px, (std::array<double, 4>{8, 2, 60, 40}));
        QCOMPARE(n.modified, std::string("2021-05-21T22:10:29Z"));  // (+01:00)
        QVERIFY(!(*notes)[1].is_text);
        QCOMPARE(raw.substr((*notes)[1].at, (*notes)[1].length), sound_note());  // (where it is: its bytes not copied)
        // written again: the sound note's bytes as they were; the text note as PhotoCraft writes one (its name "", its
        // date with Z, its icon 16 × 20)
        const std::string again = pa::block(*notes, raw);
        const auto back = pa::parse(again);
        QVERIFY(back);
        QCOMPARE((*back)[0].text, n.text);
        QCOMPARE((*back)[0].color, n.color);
        QCOMPARE((*back)[0].modified, n.modified);
        QCOMPARE((*back)[0].popup_px, n.popup_px);
        QVERIFY(again.ends_with(sound_note()));
        QCOMPARE(pa::block(*back, again), again);  // (stable)
        // (a text note as read: its bytes, not written anew)
        std::vector<pa::FileNote> as_read = *notes;
        as_read[0].as_read = true;
        QCOMPARE(pa::block(as_read, raw), raw);
        // dates
        QCOMPARE(pa::utc_of_pdf_date("D:20211231233000-02'30'"), std::string("2022-01-01T02:00:00Z"));
        QCOMPARE(pa::utc_of_pdf_date("D:20200101000000Z"), std::string("2020-01-01T00:00:00Z"));
        QCOMPARE(pa::utc_of_pdf_date("20200101000000"), std::string("2020-01-01T00:00:00Z"));
        QCOMPARE(pa::utc_of_pdf_date(""), std::string());
        QCOMPARE(pa::utc_of_pdf_date("D:2020"), std::string());
        QCOMPARE(pa::pdf_date_of("2020-01-01T00:00:00Z"), std::string("D:20200101000000Z"));
        QCOMPARE(pa::pdf_date_of(""), std::string());
        // a block cut short: none
        QVERIFY(!pa::parse(raw.substr(0, raw.size() - 3)));
        // (the review's P2-2: more than 4096 notes of any kind — the block kept as it is, not read)
        std::vector<std::string> many(4097, sound_note());
        QVERIFY(!pa::parse(anno(many)));
        many.pop_back();
        QVERIFY(pa::parse(anno(many)) && pa::parse(anno(many))->size() == 4096);
        // (the review's P2-1: a surrogate without its pair is U+FFFD, and the notes can be written)
        const std::string text = std::string("\xFE\xFF", 2) + u16('a') + u16(0xD800) + u16('b') + u16(0xDC00);
        std::string odd = std::string("txtA") + '\x00' + '\x00' + u16(1);
        for (int k = 0; k < 8; ++k) odd += u32(0);
        odd += u16(0) + u16(0) + u16(0) + u16(0) + u16(0) + std::string(2, '\0') + std::string(2, '\0') + std::string(2, '\0');
        odd += u32(static_cast<unsigned>(text.size() + 12)) + "txtC" + u32(static_cast<unsigned>(text.size())) + text;
        const auto odd_notes = pa::parse(anno({u32(static_cast<unsigned>(odd.size() + 4)) + odd}));
        QVERIFY(odd_notes);
        QCOMPARE((*odd_notes)[0].text, std::string("a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
        const auto odd_page = pa::notes_of(*odd_notes, {1, 0, 0, 1, 0, 0}, {});
        QVERIFY(!core::to_json(odd_page).dump().empty());
        QVERIFY(!pa::parse(std::string("\x00\x02\x00\x01\x00\x00\x00\x09", 8)));
        // the page's notes from the file's (mm a pixel 0.5)
        const auto made = pa::notes_of(*notes, {0.5, 0, 0, 0.5, 0, 0}, {});
        QCOMPARE(made.size(), std::size_t(1));
        QCOMPARE(made[0].at_mm, (std::array<double, 2>{1, 1}));
        QCOMPARE(*made[0].popup_mm, (std::array<double, 4>{4, 1, 26, 19}));
        QCOMPARE(made[0].actor, std::string("genko"));
    }

    // A .pcraft's notes (PhotoCraft's Note: its pixels, its PDF date) as the page's, and back.
    void pcraftNotes() {
        const Json value = Json::array({Json{{"author", "someone"}, {"color", Json{{"mode", "Rgb"}, {"c", Json::array({1.0, 0.0, 0.0, 0.0})}, {"alpha", 1.0}}},
                                             {"text", "open note"}, {"position", Json::array({72, 144})}, {"popup", Json::array({0, 0, 0, 0})},
                                             {"open", true}, {"modified", "D:20210521231029Z"}},
                                        Json{{"text", "defaults"}}});
        const auto notes = render::pcraft_annotations::notes_of(value, 25.4 / 72);
        QCOMPARE(notes.size(), std::size_t(2));
        QCOMPARE(notes[0].at_mm, (std::array<double, 2>{25.4, 50.8}));
        QVERIFY(!notes[0].popup_mm);
        QCOMPARE(notes[0].color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(notes[0].modified, std::string("2021-05-21T23:10:29Z"));
        QCOMPARE(notes[1].text, std::string("defaults"));
        QVERIFY(!notes[1].open);
        bool refused = false;
        try {
            render::pcraft_annotations::notes_of(Json::array({Json{{"position", "here"}}}), 1.0);
        } catch (const core::Error&) {
            refused = true;
        }
        QVERIFY(refused);
        core::Page page;
        page.annotations = notes;
        render::pcraft_production::WriteContext ctx;
        ctx.px = 72 / 25.4;
        const auto written = render::pcraft_annotations::write(page, ctx);
        QVERIFY(written);
        QCOMPARE((*written)[0]["position"], Json::array({72.0, 144.0}));
        QCOMPARE((*written)[0]["modified"], Json("D:20210521231029Z"));
        QCOMPARE((*written)[0]["color"]["mode"], Json("Rgb"));
        const auto again = render::pcraft_annotations::notes_of(*written, 25.4 / 72);
        QCOMPARE(again[0].at_mm, notes[0].at_mm);
        QCOMPARE(again[0].text, notes[0].text);
        QCOMPARE(again[0].color, notes[0].color);
    }

    // (the review's P1-2) What a file brings is what the book's reader takes: a note far past the page pulled inside the
    // paper (its box past a page's places the default), an author cut to 1024 bytes by whole characters; the checks the
    // writer and the reader share refuse the rest. (P2-3) A note read while an op's actor reads is that actor's.
    void filesNotesAreTheBooks() {
        core::Annotation far;
        far.id = "an_0123456789ab";
        far.at_mm = {1.27e7, -5};
        far.popup_mm = std::array<double, 4>{1.3e7, 0, 3e7, 10};
        far.modified = "";
        std::string author;
        for (int i = 0; i < 400; ++i) author += "あ";  // (1200 bytes)
        far.author = author;
        bool refused = false;
        try {
            core::check_annotations({far});
        } catch (const core::Error&) {
            refused = true;
        }
        QVERIFY(refused);
        render::annotations::pull_inside(far, 100, 80);
        QCOMPARE(far.at_mm, (std::array<double, 2>{96, 0}));
        QVERIFY(!far.popup_mm);
        QCOMPARE(far.author.size(), std::size_t(1023));  // (341 whole characters)
        core::check_annotations({far});
        QCOMPARE(core::annotations_from_json(core::to_json(std::vector<core::Annotation>{far}))[0], far);
        // a .pcraft's: far and a long author, read onto a page — inside it
        const Json value = Json::array({Json{{"text", "遠い"}, {"author", author}, {"position", Json::array({1e9, 5})}, {"popup", Json::array({0, 0, 1e300, 1e300})}}});
        core::Page page;
        page.spec = core::PageSpec::custom(100, 80, 20, 20, 1, 2, 2, 2, 2, 200, "color");
        render::pcraft_production::ReadContext ctx;
        ctx.page = &page;
        ctx.mm = 25.4 / 72;
        std::vector<std::string> said;
        ctx.say = [&said](std::string s) { said.push_back(std::move(s)); };
        render::pcraft_annotations::read(value, ctx);
        QVERIFY2(said.empty(), said.empty() ? "" : said[0].c_str());
        QCOMPARE(page.annotations.size(), std::size_t(1));
        QCOMPARE(page.annotations[0].at_mm[0], 96.0);
        QVERIFY(!page.annotations[0].popup_mm);
        core::check_annotations(page.annotations);
        QCOMPARE(page.annotations[0].actor, std::string("genko"));
        // (read while an agent's op reads: the agent's)
        {
            const render::annotations::ReadingActor reader(core::Actor("ai:claude"));
            QCOMPARE(render::pcraft_annotations::notes_of(value, 1.0)[0].actor, std::string("ai:claude"));
        }
        QCOMPARE(render::pcraft_annotations::notes_of(value, 1.0)[0].actor, std::string("genko"));
    }

    // ファイル → 読み込み → 注釈…'s ops: each mark inside the paper, its box along; the page's room.
    void importOps() {
        const core::Document doc = book();  // (100 × 80 mm)
        core::Annotation far;
        far.at_mm = {150, -10};
        far.popup_mm = std::array<double, 4>{160, 0, 30, 20};
        far.text = "外";
        far.modified = "2021-05-21T23:10:29Z";
        const Json ops = formats::annotation_ops({far}, doc.page(0));
        QCOMPARE(ops.size(), std::size_t(1));
        QCOMPARE(ops[0]["at_mm"], Json::array({96.0, 0.0}));
        QCOMPARE(ops[0]["popup_mm"], Json::array({106.0, 10.0, 30.0, 20.0}));
        const core::Document after = run_ops(doc, ops).doc;
        QCOMPARE(note(after, 0).modified, std::string("2021-05-21T23:10:29Z"));  // (the file's date kept)
        QCOMPARE(note(after, 0).text, std::string("外"));
        bool refused = false;
        try {
            (void)formats::read_annotations("not a file");
        } catch (const core::Error& error) {
            refused = std::string(error.what()).find("notes are read from a PSD, a PSB or a .pcraft") != std::string::npos;
        }
        QVERIFY(refused);
    }
};

QTEST_GUILESS_MAIN(TestAnnotations)
#include "test_annotations.moc"
