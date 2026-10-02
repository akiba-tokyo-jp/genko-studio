// The ops of M2-O1 through core::CommandBus, without Python (their words and results against Python's apply_ops:
// test_contract_ops). For every op: a batch that succeeds and does what it says, with the book it was given left as
// it was and only the pages it changes copied (the others stay shared); the same batch as a dry run; a batch that
// fails and leaves the book as it was; and, for an op on a page, the page locked by a person refusing an AI. Then
// the rules of strict_gates, one by one, and a layer listed twice on a page (Python's one object in two places).

#include <QtTest>

#include <QTemporaryDir>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/snapshot.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::ApplyError;
using genko::core::ApplyResult;
using genko::core::Actor;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::core::Layer;
using genko::core::LayerRole;
using genko::core::Page;

namespace {

const std::string kPerson = "human:作者";
const std::string kAi = "ai:hermes";

Json parse(const std::string& text) { return genko::core::parse_python_json(text); }

// (not "apply": std::apply would be found for a std::string argument)
ApplyResult run_ops(const Document& doc, const std::string& batch, const std::string& actor = "genko", bool dry_run = false) {
    return CommandBus().apply(doc, parse(batch), Actor(actor), dry_run);
}

std::string error_of(const Document& doc, const std::string& batch, const std::string& actor = "genko") {
    try {
        CommandBus().apply(doc, parse(batch), Actor(actor));
    } catch (const ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return "(applied)";
}

std::size_t count(const Page& page, LayerRole role) {
    const Layer* layer = page.first_layer(role);
    return layer != nullptr ? layer->stroke_count() : 0;
}

const Layer* layer_by_id(const Page& page, const std::string& id) {
    for (const Layer& layer : page.layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

// The book: four pages; on page 1 a name line, two ink lines, a pen layer with a line and a folder; page 2 split in
// two (top and bottom).
Document fixture() {
    const Document doc = genko::core::new_episode("t", 1, 4, genko::core::PageSpec::a4_mono());
    return run_ops(doc, R"([
        {"op": "add_stroke", "page": 1, "layer": "name", "points": [[10, 10], [40, 41], [70, 20], [90, 62], [120, 30]]},
        {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 100], [180, 100]]},
        {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 150], [180, 150]]},
        {"op": "add_layer", "page": 1, "kind": "pen", "id": "pen-1"},
        {"op": "add_stroke", "page": 1, "layer_id": "pen-1", "points": [[30, 30], [60, 60]]},
        {"op": "add_layer", "page": 1, "kind": "folder", "id": "folder-1"},
        {"op": "split_frame", "page": 2, "axis": "horizontal"}
    ])")
        .doc;
}

std::string replaced(std::string text, const std::vector<std::pair<std::string, std::string>>& names) {
    for (const auto& [from, to] : names) {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    return text;
}

struct Case {
    std::string op;
    std::string batch;                     // succeeds
    std::string actor;
    std::vector<int> pages_changed;        // the pages (1-based) the batch changes; -1: the pages are rearranged
    std::function<bool(const Document&)> did;  // what the batch did
    std::string failing;                   // fails…
    std::string words;                     // …with these words in its error
    int page = 0;                          // the page the op is on (0: a book op)
    std::string failing_actor = "genko";   // who tries the failing batch
};

}  // namespace

class TestOps : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    // Everything about a book: its full snapshot and its project.json as the writer writes it.
    std::string state(const Document& doc) {
        genko::storage::AssetStore store(genko::storage::path_from_utf8((tmp_.path() + "/store").toStdString()));
        return genko::core::dump_python(genko::storage::snapshot(doc, true)) +
               genko::core::dump_python(genko::storage::project_payload_v4(doc, store));
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void everyOp() {
        const Document doc = fixture();
        const Page& p1 = doc.page(0);
        const Page& p2 = doc.page(1);
        const std::vector<std::pair<std::string, std::string>> names{
            {"LEAF1", p1.frames[0].id},
            {"ROOT2", p2.frames[0].id},
            {"TOP2", p2.frames[0].children.at(0).id},
            {"INK1", p1.first_layer(LayerRole::Ink)->id},
        };
        const std::string leaf1 = p1.frames[0].id;
        const std::vector<Case> cases{
            {"split_frame", R"([{"op": "split_frame", "page": 1, "axis": "vertical", "ratio": 0.3, "gutter_mm": 4}])", "genko", {1},
             [](const Document& d) { return d.page(0).frames[0].children.size() == 2; },
             R"([{"op": "split_frame", "page": 1, "axis": "diagonal"}])", "axis must be horizontal or vertical", 1},
            {"cut_frame", R"([{"op": "cut_frame", "page": 1, "p0": [0, 150], "p1": [210, 140]}])", "genko", {1},
             [](const Document& d) { return d.page(0).frames[0].children.size() == 2 && d.page(0).frames[0].children[0].poly.has_value(); },
             R"([{"op": "cut_frame", "page": 1, "p1": [1, 2]}])", "p0 and p1 are [x, y] in mm", 1},
            {"move_gutter", R"([{"op": "move_gutter", "page": 2, "frame_id": "ROOT2", "delta_mm": 5}])", kAi, {2},
             [&p2](const Document& d) {
                 return d.page(1).frames[0].children[0].rect.height.value() > p2.frames[0].children[0].rect.height.value();
             },
             R"([{"op": "move_gutter", "page": 1, "frame_id": "LEAF1", "delta_mm": 5}])", "frame_id must be a split", 2},
            {"merge_frame", R"([{"op": "merge_frame", "page": 2, "frame_id": "TOP2"}])", "genko", {2},
             [](const Document& d) { return d.page(1).frames[0].children.empty(); },
             R"([{"op": "merge_frame", "page": 1, "frame_id": "LEAF1"}])", "cannot merge the root frame", 2},
            {"resize_frame", R"([{"op": "resize_frame", "page": 2, "frame_id": "TOP2", "rect": {"x": 20, "y": 20, "width": 100, "height": 50}}])",
             "genko", {2}, [](const Document& d) { return d.page(1).frames[0].children[0].rect.x.value() == 20.0; },
             R"([{"op": "resize_frame", "page": 2, "rect": {"x": 1, "y": 1, "width": 9, "height": 9}}])", "frame_id is required", 2},
            {"set_frame", R"([{"op": "set_frame", "page": 1, "frame_id": "LEAF1", "border_mm": 0, "bleed": true}])", "genko", {1},
             [](const Document& d) { return d.page(0).frames[0].bleed && d.page(0).frames[0].border_mm == 0.0; },
             R"([{"op": "set_frame", "page": 1, "frame_id": "LEAF1", "poly": [[0, 0], [1, 1]]}])", "a shape needs at least three corners", 1},
            {"add_frame", R"([{"op": "add_frame", "page": 3, "rect": [20, 20, 80, 60], "id": "f-a"}])", "genko", {3},
             [](const Document& d) { return d.page(2).leaf_frames().size() == 1 && d.page(2).leaf_frames()[0]->id == "f-a"; },
             R"([{"op": "add_frame", "page": 3, "rect": [1, 1, 2, 2]}])", "a panel", 3},
            {"delete_frame",
             R"([{"op": "add_frame", "page": 3, "rect": [20, 20, 80, 60], "id": "f-a"}, {"op": "add_frame", "page": 3, "rect": [110, 20, 80, 60], "id": "f-b"},
                 {"op": "delete_frame", "page": 3, "frame_id": "f-a"}])",
             "genko", {3}, [](const Document& d) { return d.page(2).leaf_frames().size() == 1 && d.page(2).leaf_frames()[0]->id == "f-b"; },
             R"([{"op": "delete_frame", "page": 1, "frame_id": "LEAF1"}])", "the page's last panel cannot be deleted", 3},
            {"select_frame", R"([{"op": "select_frame", "page": 1, "frame_id": "LEAF1"}])", kAi, {1},
             [leaf1](const Document& d) { return d.page(0).selected_frame_id == Json(leaf1); },
             R"([{"op": "select_frame", "page": 9, "frame_id": "LEAF1"}])", "no page 9", 1},
            {"add_page", R"([{"op": "add_page", "count": 2}])", "genko", {-1}, [](const Document& d) { return d.pages.size() == 6; },
             R"([{"op": "add_page", "count": 0}])", "count is 1 to 200"},
            {"delete_page", R"([{"op": "delete_page", "page": 4}])", "genko", {-1}, [](const Document& d) { return d.pages.size() == 3; },
             R"([{"op": "delete_page", "page": 9}])", "no page 9", 4},
            {"duplicate_page", R"([{"op": "duplicate_page", "page": 1}])", "genko", {-1},
             [&p1](const Document& d) {
                 const Page& copy = d.page(4);
                 return d.pages.size() == 5 && copy.layers.size() == p1.layers.size() && copy.layers[0].id != p1.layers[0].id &&
                        count(copy, LayerRole::Ink) == 2 && copy.frames[0].id != p1.frames[0].id;
             },
             R"([{"op": "duplicate_page", "page": "x"}])", "page (int) is required", 1},
            {"reorder", R"([{"op": "reorder", "order": [2, 1, 3, 4]}])", "genko", {-1},
             [&p1](const Document& d) { return d.page(1).id == p1.id && d.page(1).index.value() == 2.0; },
             R"([{"op": "reorder", "order": [1, 1, 2, 3]}])", "order must list every page exactly once"},
            {"advance", R"([{"op": "advance", "page": 1, "to": "finish"}])", kAi, {1},
             [](const Document& d) { return d.page(0).stage == "finish"; },
             R"([{"op": "advance", "page": 1, "to": "ink"}])", "name is not OK", 1},
            // (an AI is refused name_ok before any page lock is looked at, and a person is not stopped by a lock)
            {"name_ok", R"([{"op": "name_ok", "page": 2}])", kPerson, {2}, [](const Document& d) { return d.page(1).name_ok; },
             R"([{"op": "name_ok", "page": 2}])", "name_ok needs a person (actor ai:hermes cannot approve)", 0, kAi},
            {"lock_page", R"([{"op": "lock_page", "page": 1}])", kAi, {},
             [&p1](const Document& d) { return d.page_locks.value(p1.id, Json()) == Json(kAi); },
             R"([{"op": "lock_page", "page": 1, "agent": "ai:other"}])", "cannot lock page 1 as ai:other (actor is ai:hermes)", 1, kAi},
            {"unlock_page", R"([{"op": "lock_page", "page": 1}, {"op": "unlock_page", "page": 1}])", kAi, {},
             [](const Document& d) { return d.page_locks.empty(); },
             R"([{"op": "unlock_page", "page": 9}, {"op": "set_note", "page": 9}])", "no page 9", 1},
            {"set_note", R"([{"op": "set_note", "page": 3, "note": "メモ"}])", kAi, {3}, [](const Document& d) { return d.page(2).note == "メモ"; },
             R"([{"op": "set_note", "page": 9}])", "no page 9", 3},
            {"set_meta", R"([{"op": "set_meta", "title": "題", "binding": "left"}])", "genko", {1, 2, 3, 4},
             [](const Document& d) { return d.title == "題" && d.page(3).binding == genko::core::Binding::Left; },
             R"([{"op": "set_meta", "start_side": "top"}])", "start_side must be left, right or null"},
            {"set_autosave", R"([{"op": "set_autosave", "enabled": true}])", kAi, {}, [](const Document& d) { return d.autosave; },
             R"([{"op": "set_autosave", "enabled": true}, {"op": "set_note", "page": 9}])", "no page 9"},
            {"add_stroke", R"([{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[10, 10], [50, 50]], "width_mm": 0.7, "rgb": [1, 2, 3]}])",
             kAi, {2}, [](const Document& d) { return count(d.page(1), LayerRole::Ink) == 1; },
             R"([{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[10, 10]]}])", "points needs at least two", 2},
            {"delete_stroke", R"([{"op": "delete_stroke", "page": 1, "layer": "ink", "index": 0}])", "genko", {1},
             [](const Document& d) { return count(d.page(0), LayerRole::Ink) == 1; },
             R"([{"op": "delete_stroke", "page": 1, "layer": "ink", "index": 9}])", "stroke index out of range", 1},
            {"edit_stroke", R"([{"op": "edit_stroke", "page": 1, "layer": "name", "index": 0, "points": [[1, 1], [2, 2], [3, 3]]}])", "genko", {1},
             [](const Document& d) { return d.page(0).first_layer(LayerRole::Name)->strokes->items[0]->points.size() == 3; },
             R"([{"op": "edit_stroke", "page": 1, "layer": "name", "index": 0, "points": [[1, 1]]}])", "points needs at least two", 1},
            {"simplify_stroke", R"([{"op": "simplify_stroke", "page": 1, "layer": "name", "index": 0, "epsilon_mm": 50}])", "genko", {1},
             [](const Document& d) { return d.page(0).first_layer(LayerRole::Name)->strokes->items[0]->points.size() < 5; },
             R"([{"op": "simplify_stroke", "page": 1, "layer": "name"}])", "not found: 'index'", 1},
            {"erase", R"([{"op": "erase", "page": 1, "layer": "ink", "points": [[100, 90], [100, 160]], "width_mm": 2}])", "genko", {1},
             [](const Document& d) { return count(d.page(0), LayerRole::Ink) == 4; },
             R"([{"op": "erase", "page": 1, "layer": "ink", "points": [[1, 1], [2, 2]], "mode": "smudge"}])", "mode must be cut, to_crossing or whole", 1},
            {"erase_raster", R"([{"op": "erase_raster", "page": 1, "points": [[100, 90], [100, 160]], "mode": "whole"}])", "genko", {1},
             [](const Document& d) { return count(d.page(0), LayerRole::Ink) == 0; },
             R"([{"op": "erase_raster", "page": 1, "points": [[1, 1], [2, 2]], "texture": "wet"}])", "texture must be hard, soft or rough", 1},
            {"add_layer", R"([{"op": "add_layer", "page": 2, "kind": "fill", "rgb": [200, 200, 200], "id": "fill-1"}])", kAi, {2},
             [](const Document& d) { return layer_by_id(d.page(1), "fill-1") != nullptr; },
             R"([{"op": "add_layer", "page": 2, "kind": "sticker"}])", "kind must be pen, paint, folder, fill, gradient or adjust", 2},
            {"delete_layer", R"([{"op": "delete_layer", "page": 1, "id": "pen-1"}])", "genko", {1},
             [](const Document& d) { return layer_by_id(d.page(0), "pen-1") == nullptr; },
             R"([{"op": "delete_layer", "page": 1, "id": "INK1"}])", "cannot delete core layer", 1},
            {"duplicate_layer", R"([{"op": "duplicate_layer", "page": 1, "id": "pen-1", "new_id": "pen-2"}])", "genko", {1},
             [](const Document& d) {
                 const Layer* copy = layer_by_id(d.page(0), "pen-2");
                 const Layer* source = layer_by_id(d.page(0), "pen-1");
                 return copy != nullptr && copy->stroke_count() == 1 && copy->strokes->items[0]->id != source->strokes->items[0]->id &&
                        copy->identity != source->identity && (copy - source) == 1;
             },
             R"([{"op": "duplicate_layer", "page": 1, "id": "folder-1"}])", "a folder cannot be duplicated", 1},
            {"set_layer", R"([{"op": "set_layer", "page": 1, "layer": "ink", "opacity": 0.5, "blend": "multiply", "visible": false}])", "genko", {1},
             [](const Document& d) {
                 const Layer* ink = d.page(0).first_layer(LayerRole::Ink);
                 return ink->opacity == 0.5 && ink->blend == "multiply" && !ink->visible;
             },
             R"([{"op": "set_layer", "page": 1}])", "layer id or role required", 1},
            {"set_layers", R"([{"op": "set_layers", "page": 1, "all": true, "locked": true}])", "genko", {1},
             [](const Document& d) {  // (all: a folder only for visible)
                 return std::all_of(d.page(0).layers.begin(), d.page(0).layers.end(),
                                    [](const Layer& l) { return l.locked == (l.kind != genko::core::LayerKind::Folder); });
             },
             R"([{"op": "set_layers", "page": 1, "all": true}])", "set_layers needs something to set", 1},
            {"reorder_layers", R"([{"op": "reorder_layers", "page": 1, "order": ["folder-1", "pen-1", "INK1"]}])", "genko", {1},
             [](const Document& d) { return d.page(0).layers.size() == 3 && d.page(0).layers[0].id == "folder-1"; },
             R"([{"op": "reorder_layers", "page": 9, "order": []}])", "no page 9", 1},
            {"set_brush", R"([{"op": "set_brush", "rgb": [10, 20, 30], "width_mm": 0.9}])", kAi, {},
             [](const Document& d) { return d.brush_rgb == std::vector<std::int64_t>{10, 20, 30} && d.brush_width_mm == 0.9; },
             R"([{"op": "set_brush", "rgb": ["x"]}])", "a value of the wrong type"},
        };
        QCOMPARE(cases.size(), std::size_t{33});
        std::set<std::string> ops;
        const std::string before = state(doc);
        for (const Case& c : cases) {
            ops.insert(c.op);
            const std::string batch = replaced(c.batch, names);
            const QByteArray what = QByteArray::fromStdString(c.op + ": ");
            // it succeeds and does what it says; the book it was given is as it was
            const auto counted = [&](bool dry_run) {  // (the same new ids each time)
                const genko::core::ScopedIdSource ids(genko::core::counting_ids(0x1000));
                return run_ops(doc, batch, c.actor, dry_run);
            };
            ApplyResult result;
            try {
                result = counted(false);
            } catch (const std::exception& error) {
                QFAIL(what + "failed: " + error.what());
            }
            QVERIFY2(c.did(result.doc), what + "did not do it");
            QVERIFY2(state(doc) == before, what + "the book given changed");
            QVERIFY2(!result.applied.empty() && result.applied.back() == Json(c.op), what + "not applied");
            // only the pages it changes are copied
            if (c.pages_changed != std::vector<int>{-1}) {
                for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                    const bool changed = std::find(c.pages_changed.begin(), c.pages_changed.end(), static_cast<int>(i + 1)) !=
                                         c.pages_changed.end();
                    QVERIFY2((result.doc.pages[i].get() != doc.pages[i].get()) == changed,
                             what + "page " + QByteArray::number(int(i + 1)) + (changed ? " was not copied" : " was copied"));
                }
            }
            // a dry run shows the same book (and the caller keeps the book it had)
            const ApplyResult dry = counted(true);
            QVERIFY2(state(dry.doc) == state(result.doc), what + "a dry run shows another book");
            QVERIFY2(state(doc) == before, what + "the dry run changed the book given");
            // the failing batch: refused with its words, the book as it was
            const std::string error = error_of(doc, replaced(c.failing, names), c.failing_actor);
            QVERIFY2(error.find(c.words) != std::string::npos, what + "failing batch: " + QByteArray::fromStdString(error));
            QVERIFY2(state(doc) == before, what + "the failing batch changed the book");
            // a page locked by a person: an AI is refused there
            if (c.page != 0) {
                Document locked = doc;
                locked.page_locks[doc.page(static_cast<std::size_t>(c.page - 1)).id] = kPerson;
                const std::string refused = error_of(locked, batch, kAi);
                QVERIFY2(refused.find("page " + std::to_string(c.page) + " locked by " + kPerson) != std::string::npos,
                         what + "on a locked page: " + QByteArray::fromStdString(refused));
            }
        }
        QCOMPARE(ops.size(), std::size_t{33});
    }

    void strictGates() {
        // page 1's name approved, page 2's not; page 2 has a pen layer (printed) and a layer kept out of print with a line
        const Document doc = run_ops(fixture(), R"([
            {"op": "set_meta", "strict_gates": true}, {"op": "name_ok", "page": 1},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "pen-2"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "trial"},
            {"op": "set_layer", "page": 2, "layer": "user", "id": "trial", "exportable": false},
            {"op": "add_stroke", "page": 2, "layer_id": "trial", "points": [[1, 1], [5, 5]]}
        ])",
                                   kPerson)
                                 .doc;
        QVERIFY(doc.strict_gates && doc.page(0).name_ok && !doc.page(1).name_ok);
        const std::string leaf1 = doc.page(0).frames[0].id;
        const std::string approved = "page 1: the name is approved; a person must revoke it before the layout changes (strict_gates)";
        // the layout of an approved page: a person only
        for (const std::string& batch : {
                 std::string(R"([{"op": "split_frame", "page": 1, "axis": "vertical"}])"),
                 std::string(R"([{"op": "cut_frame", "page": 1, "p0": [0, 150], "p1": [210, 140]}])"),
                 R"([{"op": "move_gutter", "page": 1, "frame_id": ")" + leaf1 + R"(", "delta_mm": 5}])",
                 R"([{"op": "merge_frame", "page": 1, "frame_id": ")" + leaf1 + R"("}])",
                 R"([{"op": "resize_frame", "page": 1, "frame_id": ")" + leaf1 + R"(", "rect": {"x": 1, "y": 1, "width": 50, "height": 50}}])",
                 std::string(R"([{"op": "add_frame", "page": 1, "rect": [20, 20, 80, 60]}])"),
                 R"([{"op": "delete_frame", "page": 1, "frame_id": ")" + leaf1 + R"("}])",
                 R"([{"op": "set_frame", "page": 1, "frame_id": ")" + leaf1 + R"(", "poly": [[0, 0], [100, 0], [50, 80]]}])"}) {
            const std::string error = error_of(doc, batch, kAi);
            QVERIFY2(error.find(approved) != std::string::npos, (batch + ": " + error).c_str());
            QVERIFY2(error_of(doc, batch, kPerson).find("strict_gates") == std::string::npos, batch.c_str());
        }
        QCOMPARE(error_of(doc, R"([{"op": "set_frame", "page": 1, "frame_id": ")" + leaf1 + R"(", "border_mm": 2}])", kAi),
                 std::string("(applied)"));  // (not the layout)
        // finish needs the art approved
        QVERIFY(error_of(doc, R"([{"op": "advance", "page": 1, "to": "finish"}])", kPerson)
                    .find("page 1: finish needs the art approved (strict_gates)") != std::string::npos);
        // drawing on, deleting or copying a printed layer needs the page's name approved
        QVERIFY(error_of(doc, R"([{"op": "add_stroke", "page": 2, "layer_id": "pen-2", "points": [[1, 1], [2, 2]]}])", kPerson)
                    .find("add_stroke on a printed layer needs name_ok on page 2 (strict_gates)") != std::string::npos);
        QVERIFY(error_of(doc, R"([{"op": "delete_layer", "page": 2, "id": "pen-2"}])", kPerson)
                    .find("fill on a printed layer needs name_ok on page 2 (strict_gates)") != std::string::npos);
        QVERIFY(error_of(doc, R"([{"op": "duplicate_layer", "page": 2, "id": "pen-2"}])", kPerson)
                    .find("fill on a printed layer needs name_ok on page 2 (strict_gates)") != std::string::npos);
        QCOMPARE(error_of(doc, R"([{"op": "add_stroke", "page": 2, "layer_id": "trial", "points": [[1, 1], [2, 2]]}])", kAi),
                 std::string("(applied)"));  // (not printed: trying a line out)
        QCOMPARE(error_of(doc, R"([{"op": "add_stroke", "page": 1, "layer_id": "pen-1", "points": [[1, 1], [2, 2]]}])", kAi),
                 std::string("(applied)"));  // (page 1 is approved)
        // a drawn layer put into print
        QVERIFY(error_of(doc, R"([{"op": "set_layer", "page": 2, "layer": "user", "id": "trial", "exportable": true}])", kPerson)
                    .find("set_layer exportable on a drawn layer needs name_ok on page 2 (strict_gates)") != std::string::npos);
        // erasing on a printed layer
        QVERIFY(error_of(doc, R"([{"op": "erase", "page": 2, "points": [[1, 1], [2, 2]]}])", kPerson)
                    .find("erase on ink needs name_ok on page 2 (strict_gates)") != std::string::npos);
        QVERIFY(error_of(doc, R"([{"op": "erase_raster", "page": 2, "layer": "finish", "points": [[1, 1], [2, 2]]}])", kPerson)
                    .find("erase_raster on finish needs name_ok on page 2 (strict_gates)") != std::string::npos);
        QCOMPARE(error_of(doc, R"([{"op": "erase", "page": 2, "layer": "name", "points": [[1, 1], [2, 2]]}])", kAi),
                 std::string("(applied)"));
        // set_layers is not refused (only the merges are)
        QCOMPARE(error_of(doc, R"([{"op": "set_layers", "page": 2, "ids": ["pen-2"], "visible": false}])", kAi), std::string("(applied)"));
        // without strict_gates none of these is refused
        const Document plain = run_ops(doc, R"([{"op": "set_meta", "strict_gates": false}])", kPerson).doc;
        QCOMPARE(error_of(plain, R"([{"op": "split_frame", "page": 1, "axis": "vertical"}])", kAi), std::string("(applied)"));
        QCOMPARE(error_of(plain, R"([{"op": "delete_layer", "page": 2, "id": "pen-2"}])", kAi), std::string("(applied)"));
    }

    void aLayerListedTwice() {
        const Document doc = fixture();
        const std::string ink = doc.page(0).first_layer(LayerRole::Ink)->id;
        const std::string name = doc.page(0).first_layer(LayerRole::Name)->id;
        // Python's reorder_layers lists the one layer object twice: an edit shows in both places, in this batch and
        // in the next
        const auto twice = run_ops(doc, R"([{"op": "reorder_layers", "page": 1, "order": [")" + ink + R"(", ")" + name + R"(", ")" + ink +
                                          R"("]}, {"op": "set_layer", "page": 1, "layer": "ink", "opacity": 0.25}])");
        const auto& layers = twice.doc.page(0).layers;
        QCOMPARE(layers.size(), std::size_t{3});
        QCOMPARE(layers[0].identity, layers[2].identity);
        QVERIFY(layers[0].opacity == 0.25 && layers[2].opacity == 0.25);
        const auto drawn = run_ops(twice.doc, R"([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[1, 1], [2, 2]]}])");
        QCOMPARE(drawn.doc.page(0).layers[0].stroke_count(), std::size_t{3});
        QCOMPARE(drawn.doc.page(0).layers[2].stroke_count(), std::size_t{3});
        QVERIFY(drawn.doc.page(0).layers[0].strokes == drawn.doc.page(0).layers[2].strokes);
        QCOMPARE(twice.doc.page(0).layers[2].stroke_count(), std::size_t{2});  // (the book before is as it was)
        // a page copied: the copy lists its own layer twice, under the id Python's loop gave it last
        const auto copied = run_ops(twice.doc, R"([{"op": "duplicate_page", "page": 1},
                                                 {"op": "set_layer", "page": 5, "layer": "ink", "visible": false}])");
        const auto& copy = copied.doc.page(4).layers;
        QCOMPARE(copy[0].identity, copy[2].identity);
        QVERIFY(copy[0].identity != layers[0].identity);
        QCOMPARE(copy[0].id, copy[2].id);
        QVERIFY(copy[0].id != copy[1].id && copy[0].id != ink);
        QVERIFY(!copy[0].visible && !copy[2].visible);
        QVERIFY(copied.doc.page(0).layers[0].visible && copied.doc.page(0).layers[2].visible);
        // deleted: both go
        const auto deleted = run_ops(twice.doc, R"([{"op": "add_layer", "page": 1, "kind": "pen", "id": "p"},
            {"op": "reorder_layers", "page": 1, "order": ["p", ")" + ink + R"(", "p"]}, {"op": "delete_layer", "page": 1, "id": "p"}])");
        QCOMPARE(deleted.doc.page(0).layers.size(), std::size_t{1});
        // ordinary copies of a book keep their layers apart from the book's
        const auto separate = run_ops(doc, R"([{"op": "set_layer", "page": 1, "layer": "ink", "opacity": 0.5}])");
        QVERIFY(doc.page(0).first_layer(LayerRole::Ink)->opacity == 1.0);
        QVERIFY(separate.doc.page(0).first_layer(LayerRole::Ink)->identity == doc.page(0).first_layer(LayerRole::Ink)->identity);
    }
};

QTEST_GUILESS_MAIN(TestOps)
#include "test_ops.moc"
