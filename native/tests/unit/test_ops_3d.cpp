// The 3D ops: for each op a batch that succeeds and what it leaves, input it
// refuses with the book left as it was, a page another agent has locked, strict_gates where the op draws on a
// printed layer, dry runs, Undo and Redo through the journal, the book saved and read back; what the C++ build refuses
// where Python would keep a broken book; the pose library; 3D guides drawn in the proof and the name, never printed.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/limits.hpp"
#include "core/linalg3.hpp"
#include "core/model.hpp"
#include "core/pose_checks.hpp"
#include "core/pose_library.hpp"
#include "core/poses.hpp"
#include "core/pynum.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "test3d.hpp"

namespace fs = std::filesystem;
namespace render = genko::render;
using genko::core::Actor;
using genko::core::ApplyError;
using genko::core::Document;
using genko::core::Json;
using genko::core::Layer;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Num;

namespace {

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(render::ops_registry());
    return b;
}

Json ops(const std::string& text) { return genko::core::parse_python_json(text); }

const char* const kCube = R"("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8")";

// Three pages; page 1 cut in two, with a picture layer, a pen layer and a locked one, a figure, a head, a hand, a
// box, a mannequin and a perspective ruler from the box.
Document base() {
    Document doc = genko::core::new_episode("3D", Num(1), 3, genko::core::PageSpec::b5_doujin());
    genko::core::Page& page = doc.edit_page(0);
    page.split_frame(page.frames[0].id, "horizontal", Num(0.5), Num(4));
    for (const auto& [id, kind, locked] : {std::tuple{"paint", LayerKind::Raster, false}, std::tuple{"pen", LayerKind::Strokes, false},
                                          std::tuple{"frozen", LayerKind::Strokes, true}}) {
        Layer layer;
        layer.id = id;
        layer.role = LayerRole::User;
        layer.kind = kind;
        layer.locked = locked;
        page.layers.push_back(layer);
    }
    return bus()
        .apply(doc, ops(R"([
            {"op": "add_figure", "page": 1, "id": "fig", "pos": [60, 120, 0], "preset": "walk"},
            {"op": "add_head", "page": 1, "id": "head", "pos": [140, 60, 0]},
            {"op": "add_hand", "page": 1, "id": "hand", "pos": [120, 90, 0], "side": "l", "pose": "peace"},
            {"op": "add_prim3d", "page": 1, "id": "box", "kind": "box", "pos": [100, 150, 0], "size": [30, 20, 25], "rot": [0.3, 0.6, 0]},
            {"op": "add_mannequin", "page": 1, "id": "man", "pos": [150, 160, 0], "height_mm": 70},
            {"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "pr"}])"),
               Actor("genko"))
        .doc;
}

const Json* prim(const Document& doc, const std::string& id, std::size_t page = 0) {
    for (const Json& p : doc.page(page).prims) {
        if (p.value("id", std::string()) == id) return &p;
    }
    return nullptr;
}

const Layer* layer(const Document& doc, const std::string& id, std::size_t page = 0) {
    for (const Layer& l : doc.page(page).layers) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

// What a book holds as compared here: its full snapshot and its 3D state (prims, rulers, camera and light, every
// layer's strokes by their bytes and pictures by their pixels).
Json held(const Document& doc) {
    return Json::object({{"full", genko::storage::snapshot(doc, true)}, {"state", genko::test::state_of(doc)}});
}

struct OpCase {
    const char* op;
    const char* good;   // a batch on page 1 that succeeds
    const char* bad;    // an op it refuses (put after the good batch: nothing of the batch stays)
    const char* error;  // the reason given
    std::function<void(const Document&)> check;  // what the good batch leaves
};

const OpCase kOps[] = {
    {"add_figure", R"([{"op": "add_figure", "page": 1, "id": "f2", "preset": "run", "height_mm": 120, "pos": [40, 150, 0]}])",
     R"({"op": "add_figure", "page": 1, "height_mm": 5})", "height_mm must be between 10 and 400",
     [](const Document& d) {
         const Json* p = prim(d, "f2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("figure"));
         QCOMPARE((*p)["size"], Json::array({60.0, 120.0, 30.0}));
         QCOMPARE((*p)["pos"], Json::array({40.0, 150.0, 0.0}));
         QVERIFY(!(*p)["joints"].empty());  // (the preset's)
     }},
    {"add_hand", R"([{"op": "add_hand", "page": 1, "id": "h2", "side": "r", "pose": "fist", "size_mm": 30}])",
     R"({"op": "add_hand", "page": 1, "side": "x"})", "side is l or r",
     [](const Document& d) {
         const Json* p = prim(d, "h2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("hand"));
         QCOMPARE((*p)["side"], Json("r"));
         QCOMPARE((*p)["pose"], Json("fist"));
         QCOMPARE((*p)["size"], Json::array({30.0, 30.0, 30.0}));
     }},
    {"add_head", R"([{"op": "add_head", "page": 1, "id": "h3", "rot": [0, 0.5, 0]}])", R"({"op": "add_head", "page": 1, "pos": "x"})",
     "pos is [x, y, z]",
     [](const Document& d) {
         const Json* p = prim(d, "h3");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("head"));
         QCOMPARE((*p)["size"], Json::array({30.0, 30.0, 30.0}));
         QCOMPARE((*p)["rot"], Json::array({0.0, 0.5, 0.0}));
     }},
    {"add_mannequin", R"([{"op": "add_mannequin", "page": 1, "id": "m2", "preset": "walk", "height_mm": 100}])",
     R"({"op": "add_mannequin", "page": 1, "preset": "dance"})", "preset must be one of",
     [](const Document& d) {
         const Json* p = prim(d, "m2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("mannequin"));
         QCOMPARE((*p)["size"], Json::array({50.0, 100.0, 25.0}));
         QVERIFY((*p)["joints"].is_object() && !(*p)["joints"].empty());
     }},
    {"add_prim3d", R"([{"op": "add_prim3d", "page": 1, "id": "c1", "kind": "cylinder", "size": [20, 40, 20]}])",
     R"({"op": "add_prim3d", "page": 1, "kind": "torus"})", "kind must be box, cylinder, stairs, floor, sphere, cone or prop",
     [](const Document& d) {
         const Json* p = prim(d, "c1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("cylinder"));
         QCOMPARE((*p)["size"], Json::array({20.0, 40.0, 20.0}));
     }},
    {"add_scene", R"([{"op": "add_scene", "page": 1, "id": "s1", "kind": "classroom"}])", R"({"op": "add_scene", "page": 1, "kind": "castle"})",
     "scene kind must be one of",
     [](const Document& d) {
         const Json* p = prim(d, "s1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["scene"], Json("classroom"));
     }},
    {"camera_from_ruler", R"([{"op": "camera_from_ruler", "page": 1, "id": "pr", "prim_id": "box"}])",
     R"({"op": "camera_from_ruler", "page": 1, "id": "nope"})", "no ruler nope",
     [](const Document& d) {
         const Json& camera = d.page(0).extra["camera"];
         QVERIFY(camera.is_object());
         for (const char* key : {"turn", "tip", "focal_mm"}) QVERIFY2(camera.contains(key) && camera[key].is_number(), key);
     }},
    {"delete_prim", R"([{"op": "delete_prim", "page": 1, "id": "box"}])", R"({"op": "delete_prim", "page": 1, "id": "nothing"})", "no 3D",
     [](const Document& d) {
         QVERIFY(prim(d, "box") == nullptr);
         QVERIFY(prim(d, "fig") != nullptr);
     }},
    {"edit_prim", R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [90, 140, 10], "rot": [0, 0.3, 0]}])",
     R"({"op": "edit_prim", "page": 1, "id": "nothing"})", "no 3D figure or box nothing",
     [](const Document& d) {
         const Json* p = prim(d, "box");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["pos"], Json::array({90.0, 140.0, 10.0}));
         QCOMPARE((*p)["rot"], Json::array({0.0, 0.3, 0.0}));
     }},
    {"import_model", R"([{"op": "import_model", "page": 1, "id": "m1", "obj": $CUBE, "size_mm": 40}])",
     R"({"op": "import_model", "page": 1, "obj": "v 0 0 0"})", "the OBJ file has no faces",
     [](const Document& d) {
         const Json* p = prim(d, "m1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("mesh"));
         QCOMPARE((*p)["size"], Json::array({40.0, 40.0, 40.0}));
         QCOMPARE((*p)["mesh"]["v"].size(), std::size_t{24});
         QCOMPARE((*p)["mesh"]["f"].size(), std::size_t{6});
     }},
    {"pose_figure", R"([{"op": "pose_figure", "page": 1, "id": "fig", "joints": {"r_arm": {"z": -1.5}}}])",
     R"({"op": "pose_figure", "page": 1, "id": "nope"})", "no figure/hand nope",
     [](const Document& d) { QCOMPARE((*prim(d, "fig"))["joints"]["r_arm"]["z"], Json(-1.5)); }},
    {"pose_mannequin", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": {"l_arm": {"yaw": 0.5}}}])",
     R"({"op": "pose_mannequin", "page": 1, "id": "nope"})", "no mannequin nope",
     [](const Document& d) { QCOMPARE((*prim(d, "man"))["joints"]["l_arm"]["yaw"], Json(0.5)); }},
    {"render_prims", R"([{"op": "render_prims", "page": 1, "layer_id": "paint"}])", R"({"op": "render_prims", "page": 1, "layer_id": "frozen"})",
     "the layer is locked",
     [](const Document& d) {
         const Layer* l = layer(d, "paint");
         QVERIFY(l != nullptr && l->raster_png && !l->raster_png->empty());
         const render::Image picture = render::open_image(*l->raster_png);
         QCOMPARE(picture.width(), static_cast<int>(std::nearbyint(d.page(0).spec.width_mm.value() / 25.4 * 200)));  // (raster.WORKING_DPI)
         QCOMPARE(picture.height(), static_cast<int>(std::nearbyint(d.page(0).spec.height_mm.value() / 25.4 * 200)));
     }},
    {"ruler_from_3d", R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "r2", "grid": 10}])",
     R"({"op": "ruler_from_3d", "page": 1, "prim_id": "nope"})", "no 3D nope",
     [](const Document& d) {
         const Json& rulers = d.page(0).rulers;
         QVERIFY(std::any_of(rulers.begin(), rulers.end(), [](const Json& r) {
             return r.value("id", std::string()) == "r2" && r.value("kind", std::string()) == "perspective" && r.value("grid", 0) == 10;
         }));
     }},
    {"set_camera", R"([{"op": "set_camera", "page": 1, "turn": 0.4, "tip": 0.15, "focal_mm": 300}])",
     R"({"op": "set_camera", "page": 1, "focal_mm": 5})", "focal_mm must be between 20 and 5000",
     [](const Document& d) {
         const Json& camera = d.page(0).extra["camera"];
         QCOMPARE(camera["turn"], Json(0.4));
         QCOMPARE(camera["tip"], Json(0.15));
         QCOMPARE(camera["focal_mm"], Json(300.0));
         QCOMPARE(camera["target"].size(), std::size_t{2});  // (the page's middle)
     }},
    {"set_light", R"([{"op": "set_light", "page": 1, "dir": [0.2, -1, -0.5], "ambient": 0.3}])",
     R"({"op": "set_light", "page": 1, "dir": [0, 0, 0]})", "dir must point somewhere",
     [](const Document& d) {
         const Json& light = d.page(0).extra["light"];
         QCOMPARE(light["dir"], Json::array({0.2, -1.0, -0.5}));
         QCOMPARE(light["ambient"], Json(0.3));
     }},
    {"trace_prims", R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["box"], "kind": "gpen"}])",
     R"({"op": "trace_prims", "page": 1, "layer_id": "frozen"})", "the layer is locked",
     [](const Document& d) {
         const Layer* l = layer(d, "pen");
         QVERIFY(l != nullptr && l->stroke_count() >= 1);  // (the box's edges, joined into lines)
         std::size_t points = 0;
         for (const auto& s : l->strokes->items) {
             QCOMPARE(s->kind, std::string("gpen"));
             points += s->points.size();
         }
         QVERIFY(points >= 7);  // (the corners of the box seen)
     }},
};

std::string good_of(const OpCase& c) { return genko::test::replace_all(c.good, "$CUBE", kCube); }

std::string error_of(const Document& doc, const Json& batch, const std::string& actor = "genko") {
    try {
        (void)bus().apply(doc, batch, Actor(actor));
    } catch (const ApplyError& error) {
        return error.what();
    }
    return "(applied)";
}

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

// A v4 book on disk holding `doc` (its first revision).
fs::path make_book(const fs::path& dir, const Document& doc) {
    genko::storage::ProjectLock lock(dir);
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
    return dir;
}

// `genko apply`: read under the lock, apply, save as the next revision with the ops in the journal.
void edit(const fs::path& dir, const Json& batch, const std::string& actor = "genko") {
    genko::storage::ProjectLock lock(dir, actor);
    lock.try_acquire();
    genko::storage::journal::repair(dir);
    const auto loaded = genko::storage::load_document(dir);
    const auto result = bus().apply(loaded.document, batch, Actor(actor));
    genko::storage::SaveRequest request;
    request.actor = actor;
    request.base_revision = loaded.document.revision;
    request.ops = result.journal_ops;
    genko::storage::Saver(lock).save(result.doc, request);
}

Document read(const fs::path& dir) { return genko::storage::load_document(dir).document; }

}  // namespace

class TestOps3d : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    Document base_;
    int books_ = 0;

    fs::path book_path() { return to_path(tmp_.path()) / ("book" + std::to_string(++books_) + ".genko"); }

    static void rows() {
        QTest::addColumn<int>("n");
        for (int n = 0; n < static_cast<int>(std::size(kOps)); ++n) QTest::newRow(kOps[n].op) << n;
    }

    void same(const Json& got, const Json& want, const char* what) {
        std::string where;
        QVERIFY2(genko::test::strict_equal(got, want, &where), (std::string(what) + ": " + where).c_str());
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        base_ = base();
        QCOMPARE(base_.page(0).prims.size(), std::size_t{5});
        QCOMPARE(base_.page(0).rulers.size(), std::size_t{1});
    }

    void succeeds_data() { rows(); }
    void succeeds() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const auto result = bus().apply(base_, ops(good_of(c)), Actor("genko"));
        QCOMPARE(result.applied, Json::array({c.op}));
        c.check(result.doc);
        if (QTest::currentTestFailed()) return;
        QVERIFY(held(result.doc) != held(base_));
        // pages 2 and 3 are not copied (copy-on-write)
        QVERIFY(result.doc.pages[1].get() == base_.pages[1].get());
        QVERIFY(result.doc.pages[2].get() == base_.pages[2].get());
    }

    void refusalLeavesTheBook_data() { rows(); }
    void refusalLeavesTheBook() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Json before = held(base_);
        Json batch = ops(good_of(c));
        const std::size_t at = batch.size();
        batch.push_back(ops(c.bad));
        const std::string error = error_of(base_, batch);
        const std::string want = "ops[" + std::to_string(at) + "] " + c.op + ": " + c.error;
        QVERIFY2(error.starts_with(want), (error + " (wanted " + want + ")").c_str());
        same(held(base_), before, "the book");
    }

    void lockedPage_data() { rows(); }
    void lockedPage() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        Document locked = base_;
        locked.page_locks[locked.page(0).id] = "ai:other";
        const std::string error = error_of(locked, ops(good_of(c)), "ai:hermes");
        QVERIFY2(error.starts_with("ops[0] " + std::string(c.op) + ": page 1 locked by ai:other ‖ " + c.op + " takes {"), error.c_str());
        QCOMPARE(error_of(locked, ops(good_of(c)), "ai:other"), std::string("(applied)"));  // (the holder edits it)
        locked.page_locks.clear();
        locked.page_locks[locked.page(1).id] = "ai:other";  // (another page locked: no matter)
        QCOMPARE(error_of(locked, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));
    }

    void strictGates_data() { rows(); }
    void strictGates() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        Document strict = base_;
        strict.strict_gates = true;
        const std::string op = c.op;
        if (op != "render_prims" && op != "trace_prims") {
            QCOMPARE(error_of(strict, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));  // (no gate on placing 3D)
            return;
        }
        // drawing on a printed layer waits for the page's name to be approved, whoever draws
        for (const char* actor : {"ai:hermes", "human:作者"}) {
            const std::string printed = error_of(strict, ops(good_of(c)), actor);
            QVERIFY2(printed.starts_with("ops[0] " + op + ": " + op + " on a printed layer needs name_ok on page 1 (strict_gates)"),
                     printed.c_str());
            const std::string ink = error_of(strict, ops(R"([{"op": ")" + op + R"(", "page": 1, "layer": "ink"}])"), actor);
            QVERIFY2(ink.starts_with("ops[0] " + op + ": " + op + " on ink needs name_ok on page 1 (strict_gates)"), ink.c_str());
        }
        // the name is not printed
        QCOMPARE(error_of(strict, ops(R"([{"op": ")" + op + R"(", "page": 1, "layer": "name"}])"), "ai:hermes"), std::string("(applied)"));
        strict.edit_page(0).name_ok = true;
        QCOMPARE(error_of(strict, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));
    }

    // A dry run answers with the book as the ops would leave it (Python's apply_ops snapshots its working copy) and
    // changes nothing: neither the book given nor the book on disk.
    void dryRun_data() { rows(); }
    void dryRun() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Json before = held(base_);
        const auto result = bus().apply(base_, ops(good_of(c)), Actor("genko"), true);
        QCOMPARE(result.applied, Json::array({c.op}));
        c.check(result.doc);
        if (QTest::currentTestFailed()) return;
        same(held(base_), before, "the book after a dry run");
        const fs::path dir = make_book(book_path(), base_);
        const auto files = genko::test::tree_hashes(QString::fromStdString(genko::storage::path_to_utf8(dir)), false);
        {
            genko::storage::ProjectLock lock(dir, "genko");
            lock.try_acquire();
            (void)bus().apply(genko::storage::load_document(dir).document, ops(good_of(c)), Actor("genko"), true);
        }
        QVERIFY(genko::test::tree_hashes(QString::fromStdString(genko::storage::path_to_utf8(dir)), false) == files);
    }

    void undoAndRedo_data() { rows(); }
    void undoAndRedo() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const fs::path dir = make_book(book_path(), base_);
        const Json before = held(read(dir));
        edit(dir, ops(good_of(c)));
        const Document edited = read(dir);
        c.check(edited);
        if (QTest::currentTestFailed()) return;
        const Json after = held(edited);
        QVERIFY(after != before);
        // (Undo and Redo bring back the canonical state the journal keeps: its objects' keys sorted)
        std::string where;
        genko::storage::restore(dir, "genko", false, false);
        QVERIFY2(genko::test::same_content(held(read(dir)), before, &where), ("undone: " + where).c_str());
        genko::storage::restore(dir, "genko", true, false);
        QVERIFY2(genko::test::same_content(held(read(dir)), after, &where), ("redone: " + where).c_str());
        QCOMPARE(genko::test::book_problems(QString::fromStdString(genko::storage::path_to_utf8(dir))), std::string());
    }

    void savedAndReadBack_data() { rows(); }
    void savedAndReadBack() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Document applied = bus().apply(base_, ops(good_of(c)), Actor("genko")).doc;
        const fs::path dir = make_book(book_path(), applied);
        const Document back = read(dir);
        // (the page size is read back as floats, as Python reads it: the pages and the 3D are compared)
        same(genko::storage::snapshot(back, true)["pages"], genko::storage::snapshot(applied, true)["pages"], "pages read back");
        same(genko::test::state_of(back), genko::test::state_of(applied), "3D read back");
        for (std::size_t p = 0; p < applied.pages.size(); ++p) {
            same(back.page(p).prims, applied.page(p).prims, "prims");  // (the numbers as they were: 0.3, 1e-05, …)
            same(back.page(p).rulers, applied.page(p).rulers, "rulers");
            same(back.page(p).extra, applied.page(p).extra, "extra");
        }
    }

    // What this build refuses on purpose (docs/cpp-migration/SPEC.md §2).
    void safeSideRefusals() {
        const double inf = std::numeric_limits<double>::infinity();
        const auto refused = [&](Json op, const std::string& reason) {
            const std::string name = op["op"].get<std::string>();
            const std::string error = error_of(base_, Json::array({std::move(op)}));
            QVERIFY2(error.starts_with("ops[0] " + name + ": " + reason), error.c_str());
        };
        // (a number that is not finite: "<key> must be a finite number", once the op has done everything else, as the
        // other ops have it)
        refused(Json::object({{"op", "add_figure"}, {"page", 1}, {"pos", Json::array({inf, 0, 0})}}), "pos must be a finite number");
        refused(Json::object({{"op", "add_figure"}, {"page", 1}, {"joints", ops(R"({"l_knee": {"x": 1}})")}, {"rot", Json::array({0, inf, 0})}}),
                "rot must be a finite number");
        refused(Json::object({{"op", "pose_figure"}, {"page", 1}, {"id", "fig"}, {"height_mm", inf}}), "height_mm must be a finite number");
        refused(Json::object({{"op", "add_head"}, {"page", 1}, {"size_mm", inf}}), "size_mm must be a finite number");
        refused(Json::object({{"op", "set_camera"}, {"page", 1}, {"turn", inf}}), "turn must be a finite number");
        refused(Json::object({{"op", "set_camera"}, {"page", 1}, {"target", Json::array({inf, 5})}}), "target must be a finite number");
        refused(Json::object({{"op", "set_light"}, {"page", 1}, {"dir", Json::array({inf, 0, 0})}}), "dir must be a finite number");
        refused(Json::object({{"op", "edit_prim"}, {"page", 1}, {"id", "box"}, {"pos", Json::array({inf, 1, 1})}}), "pos must be a finite number");
        refused(Json::object({{"op", "add_prim3d"}, {"page", 1}, {"focal_mm", inf}}), "focal_mm must be a finite number");
        refused(Json::object({{"op", "add_scene"}, {"page", 1}, {"size", inf}}), "size must be a finite number");
        refused(Json::object({{"op", "add_mannequin"}, {"page", 1}, {"height_mm", inf}}), "height_mm must be a finite number");
        refused(Json::object({{"op", "trace_prims"}, {"page", 1}, {"layer_id", "pen"}, {"width_mm", inf}}), "width_mm must be a finite number");
        refused(Json::object({{"op", "render_prims"}, {"page", 1}, {"layer_id", "paint"}, {"tone", Json::object({{"angle", inf}})}}),
                "tone.angle must be a finite number");
        // Python's own errors come first: a hand of an unknown side (ApplyError) with a size of infinity
        refused(Json::object({{"op", "add_hand"}, {"page", 1}, {"size_mm", inf}, {"side", "x"}}), "side is l or r");
        refused(Json::object({{"op", "add_mannequin"}, {"page", 1}, {"height_mm", -50}}), "a mannequin's height must be above 0");
        refused(Json::object({{"op", "add_mannequin"}, {"page", 1}, {"pos", Json::array({"a", "b"})}}), "pos is [x, y, z]");
        refused(Json::object({{"op", "pose_mannequin"}, {"page", 1}, {"id", "man"}, {"joints", ops(R"({"l_arm": {"yaw": "x"}})")}}),
                "each joint is {yaw, pitch} in radians");
        refused(Json::object({{"op", "pose_mannequin"}, {"page", 1}, {"id", "man"},
                              {"drag", Json::object({{"handle", "pelvis"}, {"to", Json::array({inf, 0})}})}}),
                "to is [x, y]");
        refused(ops(R"({"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": 100})"), "grid is 0 (none) to 60 lines");
        refused(ops(R"({"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": -1})"), "grid is 0 (none) to 60 lines");
        const Json loop = ops(R"({"scenes": [{"nodes": [0]}], "nodes": [{"children": [1]}, {"children": [0]}]})");
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"gltf", genko::core::dump_python(loop)}}), "the model's nodes contain themselves");
        std::string huge;
        for (int i = 0; i <= genko::core::limits::kModelVertices; ++i) huge += "v " + std::to_string(i % 1000) + " " + std::to_string(i / 1000) + " 0\n";
        huge += "f 1 2 3\n";
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"obj", huge}}), "the model has too many corners (at most 240000)");
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"obj", "v 1e999 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3"}}),
                "the model has a corner that is not a finite number");
        // infinities the op only works with (not kept) are taken as Python takes them
        QCOMPARE(error_of(base_, Json::array({Json::object({{"op", "render_prims"}, {"page", 1}, {"layer_id", "paint"},
                                                             {"light", Json::array({inf, 0, 0})}})})),
                 std::string("(applied)"));
        QCOMPARE(error_of(base_, Json::array({Json::object({{"op", "pose_figure"}, {"page", 1}, {"id", "fig"},
                                                             {"drag", Json::object({{"handle", "r_hand"}, {"to", Json::array({inf, 0})}})}})})),
                 std::string("(applied)"));
    }

    void renderPrimsKeepsOwnCameraAlive() {
        Document doc = base_;
        doc.edit_page(0).extra.erase("camera");
        const Json camera = ops(R"({"turn":0.25,"tip":0,"focal_mm":400,"target":[100,150]})");
        for (Json& p : doc.edit_page(0).prims) {
            if (p.value("id", std::string()) == "box") p["camera"] = camera;
        }
        const Json batch = ops(R"([{"op":"render_prims","page":1,"ids":["box"],"layer_id":"pen","surfaces":false,"lines":true}])");
        const Document own = bus().apply(doc, batch, Actor("genko")).doc;
        doc.edit_page(0).extra["camera"] = camera;
        const Document shared = bus().apply(doc, batch, Actor("genko")).doc;
        const auto& a = layer(own, "pen")->strokes->items;
        const auto& b = layer(shared, "pen")->strokes->items;
        QVERIFY(!a.empty());
        QCOMPARE(a.size(), b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            QCOMPARE(a[i]->points.size(), b[i]->points.size());
            for (std::size_t k = 0; k < a[i]->points.size(); ++k) {
                QCOMPARE(a[i]->points[k].x, b[i]->points[k].x);
                QCOMPARE(a[i]->points[k].y, b[i]->points[k].y);
            }
        }
    }

    // A mannequin-specific op may not rewrite a different kind of 3D object (SPEC COMP-01a).
    void mannequinKindRefusal() {
        const Json before = held(base_);
        for (const char* id : {"box", "fig", "head", "hand"}) {
            const Json batch = Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "rollback"}}),
                                           Json::object({{"op", "pose_mannequin"}, {"page", 1}, {"id", id}, {"height_mm", 100}})});
            const std::string error = error_of(base_, batch);
            QVERIFY2(error.starts_with("ops[1] pose_mannequin: 3D " + std::string(id) + " is not a mannequin"), error.c_str());
            same(held(base_), before, "book after wrong-kind refusal");
        }
        const auto changed = bus().apply(base_, ops(R"([{"op":"pose_mannequin","page":1,"id":"man","height_mm":100}])"), Actor("genko"));
        QCOMPARE((*prim(changed.doc, "man"))["size"], Json::array({50.0, 100.0, 25.0}));
    }

    // What is too large to draw (the C++ build only: Python would draw a street's floor lines for as long as it takes,
    // or run out of memory for the depth picture of a 3D kilometres wide).
    void sizeLimits() {
        Document doc = base_;
        doc.edit_page(0).numero = false;  // (nombres are not drawn yet in this build)
        const Document tall =
            bus().apply(doc, ops(R"([{"op": "add_scene", "page": 1, "id": "tall", "kind": "street", "size": [150, 1000000, 200]}])"),
                        Actor("genko"))
                .doc;
        const std::string traced = error_of(tall, ops(R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["tall"]}])"));
        QVERIFY2(traced.starts_with("ops[0] trace_prims: a value of the wrong type (the scene is too large to draw)"), traced.c_str());
        render::RenderOptions options;
        options.mode = "proof";
        try {
            (void)render::render_page(tall.page(0), 72, options, &tall);
            QFAIL("a street of 55555 floors was drawn");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("image_too_large"));
        }
        // the same street at a size anyone draws
        const Document street =
            bus().apply(doc, ops(R"([{"op": "add_scene", "page": 1, "id": "s", "kind": "street", "size": [150, 80, 200]}])"), Actor("genko")).doc;
        QCOMPARE(error_of(street, ops(R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["s"]}])")), std::string("(applied)"));
    }

    // The pose library.
    void poseLibrary() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        const QByteArray before = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        namespace poses = genko::core::poses;
        QCOMPARE(poses::user_poses(), Json::array());  // (no file yet)
        const Json figure = ops(R"({"kind": "figure", "joints": {"l_arm": {"z": 0.5}}, "hands": {"l": "fist"}, "size": [45, 90, 22.5]})");
        const Json kept = poses::save_pose("　立ち ", figure);
        QCOMPARE(kept, ops(R"({"name": "立ち", "joints": {"l_arm": {"z": 0.5}}, "hands": {"l": "fist"}})"));
        QVERIFY(poses::find("立ち").has_value());
        (void)poses::save_pose("立ち", ops(R"({"joints": {"head": {"y": 1}}})"));  // (replaced, not added)
        QCOMPARE(poses::user_poses().size(), std::size_t{1});
        QCOMPARE((*poses::find("立ち"))["joints"], ops(R"({"head": {"y": 1}})"));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, poses::save_pose("  ", figure));
        poses::delete_pose("立ち");
        QVERIFY(!poses::find("立ち").has_value());
        // at most limits::kUserPoses poses (Python keeps any number): a new name past them is refused, a pose kept is
        // replaced. (The full library written at once: each save reads and writes the whole file.)
        Json full = Json::array();
        for (std::size_t i = 0; i < genko::core::limits::kUserPoses; ++i) {
            full.push_back(Json::object({{"name", "p" + std::to_string(i)}, {"joints", Json::object()}, {"hands", Json::object()}}));
        }
        genko::test::write_bytes(config.path() + "/poses.json", genko::core::dump_python(full));
        QCOMPARE(poses::user_poses().size(), genko::core::limits::kUserPoses);
        QVERIFY_THROWS_EXCEPTION(genko::core::PyValueError, poses::save_pose("one more", figure));
        (void)poses::save_pose("p7", ops(R"({"joints": {"head": {"y": 0.25}}})"));
        QCOMPARE((*poses::find("p7"))["joints"], ops(R"({"head": {"y": 0.25}})"));
        QCOMPARE(poses::user_poses().size(), genko::core::limits::kUserPoses);
        genko::test::write_bytes(config.path() + "/poses.json", "not json");
        QCOMPARE(poses::user_poses(), Json::array());  // (a file that is not JSON: none)
        QCOMPARE(QString::fromStdString(genko::storage::path_to_utf8(poses::config_dir())), config.path());
        if (before.isNull()) {
            qunsetenv("GENKO_CONFIG_DIR");
        } else {
            qputenv("GENKO_CONFIG_DIR", before);
        }
    }

    // The pose library through the ops (SPEC FIGURE-02, figures@1): pose_id puts the library's joints and hands in place
    // as a preset does and keeps the id and what the hands hold, mirror swaps left and right (the library's mirror, or the
    // figure's own), keep: ground keeps the lowest point where it was, a drag forgets the id, a preset too; the hands'
    // spread and thumb; the refusals; saved and read back as they were.
    void libraryPoses() {
        namespace library = genko::core::pose_library;
        const auto apply = [&](const Document& doc, const std::string& batch) { return bus().apply(doc, ops(batch), Actor("genko")).doc; };
        const Json& wave = *library::find("stand/wave");
        Document d = apply(base_, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "stand/wave"}])");
        Json f = *prim(d, "fig");
        same(f["joints"], wave["joints"], "joints");
        QCOMPARE(f["pose_id"], Json("stand/wave"));
        QVERIFY(!f.contains("preset"));  // (the walk preset is gone)
        QVERIFY(!f.contains("hold"));
        QCOMPARE(f["hands"]["r"], wave["hands"]["r"]);
        // the mirror: the library's, with "~m"; and a figure's own mirror toggles it back
        d = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "stand/wave", "mirror": true}])");
        f = *prim(d, "fig");
        QCOMPARE(f["pose_id"], Json("stand/wave~m"));
        same(f["joints"], library::mirrored_joints(wave["joints"]), "mirrored joints");
        QCOMPARE(f["hands"]["l"], wave["hands"]["r"]);
        d = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "mirror": true}])");
        f = *prim(d, "fig");
        QCOMPARE(f["pose_id"], Json("stand/wave"));
        QCOMPARE(genko::core::dump_python(f["joints"]), genko::core::dump_python(library::mirrored_joints(library::mirrored_joints(wave["joints"]))));
        // what the hands hold comes with the pose (and goes with the next one that holds nothing)
        d = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "everyday/drink"}])");
        QCOMPARE((*prim(d, "fig"))["hold"], Json::object({{"r", "cup"}}));
        d = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "hold": {"r": null, "l": "book"}}])");
        QCOMPARE((*prim(d, "fig"))["hold"], Json::object({{"l", "book"}}));
        d = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "stand/neutral"}])");
        QVERIFY(!(*prim(d, "fig")).contains("hold"));
        // keep: ground — the lowest point stays (a figure sitting down on the floor keeps its feet's floor); pelvis: pos stays
        const double floor_before = (*prim(d, "fig"))["pos"][1].get<double>() + genko::core::pose_checks::lowest(*prim(d, "fig"));
        const Document sat = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "sit_floor/agura", "keep": "ground"}])");
        const Json& s = *prim(sat, "fig");
        QVERIFY(std::abs(s["pos"][1].get<double>() + genko::core::pose_checks::lowest(s) - floor_before) < 1e-3);
        QVERIFY(s["pos"][1].get<double>() > (*prim(d, "fig"))["pos"][1].get<double>() + 10.0);  // (the pelvis came down)
        QCOMPARE(s["pos"][0], (*prim(d, "fig"))["pos"][0]);
        const Document kept = apply(d, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "sit_floor/agura", "keep": "pelvis"}])");
        QCOMPARE((*prim(kept, "fig"))["pos"], (*prim(d, "fig"))["pos"]);
        const Document added = apply(base_, R"([{"op": "add_figure", "page": 1, "id": "f9", "pos": [80, 150, 0], "pose_id": "sit_chair/upright", "keep": "ground"},
                                               {"op": "add_figure", "page": 1, "id": "f8", "pos": [80, 150, 0]}])");
        QVERIFY(std::abs((*prim(added, "f9"))["pos"][1].get<double>() + genko::core::pose_checks::lowest(*prim(added, "f9")) -
                         150.0 - genko::core::pose_checks::lowest(*prim(added, "f8"))) < 1e-3);
        // a drag forgets the pose's id (it is the person's own pose now), as a preset does
        const Document dragged = apply(sat, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "l_wrist", "to": [70, 100]}}])");
        QVERIFY(!(*prim(dragged, "fig")).contains("pose_id"));
        QVERIFY(!(*prim(apply(sat, R"([{"op": "pose_figure", "page": 1, "id": "fig", "preset": "run"}])"), "fig")).contains("pose_id"));
        QCOMPARE((*prim(apply(sat, R"([{"op": "pose_figure", "page": 1, "id": "fig", "joints": {"head": {"y": 0.2}}}])"), "fig"))["pose_id"], Json("sit_floor/agura"));
        // the hands' spread and thumb (figures@1): kept when given, as numbers 0..1 to 3 places
        const Document spread = apply(base_, R"([{"op": "pose_figure", "page": 1, "id": "fig", "hands": {"l": {"pose": "open", "spread": 0.71234, "thumb": 2}, "r": "fist"}},
                                                {"op": "pose_figure", "page": 1, "id": "hand", "spread": 0.4, "thumb": 0.25},
                                                {"op": "add_hand", "page": 1, "id": "h3", "pose": "grip", "thumb": 0.9}])");
        QCOMPARE((*prim(spread, "fig"))["hands"]["l"], ops(R"({"pose": "open", "curls": [0.0, 0.0, 0.0, 0.0, 0.0], "spread": 0.712, "thumb": 1.0})"));
        QCOMPARE((*prim(spread, "fig"))["hands"]["r"], Json("fist"));
        QCOMPARE((*prim(spread, "hand"))["spread"], Json(0.4));
        QCOMPARE((*prim(spread, "h3"))["thumb"], Json(0.9));
        QVERIFY((*prim(base_, "fig"))["hands"].dump().find("spread") == std::string::npos);
        // refusals (the reason first, then the op's keys)
        const auto says = [](const std::string& error, const std::string& reason) { return error.find(": " + reason + " \u2016") != std::string::npos; };
        QVERIFY(says(error_of(base_, ops(R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "stand/nope"}])")), "no pose stand/nope (figure_poses lists them)"));
        QVERIFY(says(error_of(base_, ops(R"([{"op": "add_figure", "page": 1, "pose_id": "stand/wave", "preset": "run"}])")), "give preset or pose_id, not both"));
        QVERIFY(says(error_of(base_, ops(R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "stand/wave", "keep": "feet"}])")), "keep is pelvis or ground"));
        QVERIFY(says(error_of(base_, ops(R"([{"op": "pose_figure", "page": 1, "id": "fig", "hold": {"r": "cake"}}])")),
                     "a hand holds one of cup, phone, paper, bag, umbrella, book, pen, sword, bat, ball"));
        QVERIFY(says(error_of(base_, ops(R"([{"op": "pose_figure", "page": 1, "id": "fig", "hands": {"l": {"pose": "open", "spread": "wide"}}}])")),
                     "spread and thumb are numbers from 0 to 1"));
        // saved and read back as they were
        const Document mirrored = apply(base_, R"([{"op": "pose_figure", "page": 1, "id": "fig", "pose_id": "everyday/drink~m", "hands": {"l": {"pose": "grip", "spread": 0.2}}}])");
        const Document back = read(make_book(book_path(), mirrored));
        same(back.page(0).prims, mirrored.page(0).prims, "prims read back");
        QCOMPARE((*prim(back, "fig"))["hold"], Json::object({{"l", "cup"}}));
    }

    // A box seen through the page's camera is traced as the box (it was a floor's grid): its twelve edges between the
    // corners project puts on the page, the ones at its back not drawn — a face is seen when the camera's eye is on its
    // outer side, worked out here on its own from the corners where the camera has them; with a camera that does not
    // turn the box is seen as without one.
    void boxThroughTheCamera() {
        namespace prim3d = genko::core::prim3d;
        namespace mesh3d = genko::core::mesh3d;
        namespace la = genko::core::la;
        using namespace genko::core::la;  // (the element-wise operators)
        using la::Vec3;
        // corner i: x from bit 2, y from bit 1, z from bit 0
        static constexpr int kBoxFaces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
        const auto expected_seen = [](const Json& box, const Json& camera) {
            const Vec3 size{box["size"][0].get<double>(), box["size"][1].get<double>(), box["size"][2].get<double>()};
            const Vec3 at{box["pos"][0].get<double>(), box["pos"][1].get<double>(), box["pos"][2].get<double>()};
            const la::Mat3 turn = mesh3d::prim_rotation(box["rot"]);
            const la::Mat3 view = mesh3d::prim_rotation(Json::array({camera.value("tip", 0.0), camera.value("turn", 0.0), camera.value("roll", 0.0)}));
            const Vec3 target{camera["target"][0].get<double>(), camera["target"][1].get<double>(), 0.0};
            const double focal = camera.value("focal_mm", 400.0);
            std::array<Vec3, 8> c{};
            for (int i = 0; i < 8; ++i) {
                const Vec3 local{(i & 4 ? 0.5 : -0.5) * size[0], (i & 2 ? 0.5 : -0.5) * size[1], (i & 1 ? 0.5 : -0.5) * size[2]};
                c[static_cast<std::size_t>(i)] = la::matvec(view, la::matvec(turn, local) + at - target);
            }
            Vec3 middle{0, 0, 0};
            for (const Vec3& p : c) middle = middle + p / 8.0;
            const Vec3 eye{0.0, 0.0, -focal};
            std::array<bool, 6> seen{};
            for (int f = 0; f < 6; ++f) {
                Vec3 centre{0, 0, 0};
                for (const int k : kBoxFaces[f]) centre = centre + c[static_cast<std::size_t>(k)] / 4.0;
                seen[static_cast<std::size_t>(f)] = la::dot(centre - middle, eye - centre) > 0;
            }
            return seen;
        };
        const auto near = [](const prim3d::Point2& a, const prim3d::Point2& b) { return std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9; };
        const Json boxes[] = {ops(R"({"kind": "box", "pos": [100, 150, 0], "size": [30, 20, 25], "rot": [0.3, 0.6, 0]})"),
                              ops(R"({"kind": "box", "pos": [70, 90, 40], "size": [50, 12, 18], "rot": [-0.4, 1.9, 0.2]})")};
        const Json cameras[] = {ops(R"({"turn": 0.3, "tip": 0.2, "target": [91, 128.5]})"),
                                ops(R"({"turn": 2.5, "tip": -0.4, "roll": 0.1, "focal_mm": 200, "target": [100, 150]})"),
                                ops(R"({"turn": -1.2, "tip": 0.9, "focal_mm": 900, "target": [40, 200]})")};
        for (const Json& box : boxes) {
            for (const Json& camera : cameras) {
                const std::vector<prim3d::Point2> corners = prim3d::project(box, &camera);
                const std::vector<prim3d::Edge> edges = prim3d::edges(box, &camera);
                QCOMPARE(edges.size(), std::size_t{12});
                const std::array<bool, 6> faces = expected_seen(box, camera);
                int faces_seen = 0;
                for (const bool f : faces) faces_seen += f ? 1 : 0;
                QVERIFY(faces_seen >= 1 && faces_seen <= 3);
                std::size_t seen_edges = 0;
                for (const prim3d::Edge& e : edges) {
                    int a = -1;
                    int b = -1;
                    for (int i = 0; i < 8; ++i) {
                        if (near(e.a, corners[static_cast<std::size_t>(i)])) a = i;
                        if (near(e.b, corners[static_cast<std::size_t>(i)])) b = i;
                    }
                    QVERIFY2(a >= 0 && b >= 0 && std::popcount(static_cast<unsigned>(a ^ b)) == 1, "an edge between two corners next to each other");
                    bool want = false;
                    for (int f = 0; f < 6; ++f) {
                        const auto& face = kBoxFaces[f];
                        const bool has_a = std::find(std::begin(face), std::end(face), a) != std::end(face);
                        const bool has_b = std::find(std::begin(face), std::end(face), b) != std::end(face);
                        want = want || (faces[static_cast<std::size_t>(f)] && has_a && has_b);
                    }
                    QCOMPARE(e.seen, want);
                    seen_edges += e.seen ? 1 : 0;
                }
                const std::vector<prim3d::Line2> traced = prim3d::trace(box, &camera);
                QCOMPARE(traced.size(), seen_edges);
                // its box on the page is its corners'
                const auto [x, y, w, h] = prim3d::bbox(box, &camera);
                double x0 = corners[0][0], x1 = x0, y0 = corners[0][1], y1 = y0;
                for (const auto& p : corners) x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
                QCOMPARE(x, x0);
                QCOMPARE(y, y0);
                QCOMPARE(w, x1 - x0);
                QCOMPARE(h, y1 - y0);
            }
            // a camera that does not turn, aimed at the box's own centre: the box as it is seen without a camera
            const Json still = Json::object({{"turn", 0}, {"tip", 0}, {"target", Json::array({box["pos"][0], box["pos"][1]})}});
            const std::vector<prim3d::Edge> with = prim3d::edges(box, &still);
            const std::vector<prim3d::Edge> without = prim3d::edges(box, nullptr);
            QCOMPARE(with.size(), without.size());
            for (std::size_t i = 0; i < with.size(); ++i) {
                QVERIFY(near(with[i].a, without[i].a) && near(with[i].b, without[i].b));
                QCOMPARE(with[i].seen, without[i].seen);
            }
        }
        // through the op: the box's seen edges, joined, every point a corner of the box
        Document doc = base_;
        doc = bus().apply(doc, ops(R"([{"op": "set_camera", "page": 1, "turn": 0.3, "tip": 0.2},
                                       {"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["box"]}])"),
                          Actor("genko"))
                  .doc;
        const Json camera = doc.page(0).extra["camera"];
        const std::vector<prim3d::Point2> corners = prim3d::project(*prim(doc, "box"), &camera);
        const auto& strokes = layer(doc, "pen")->strokes->items;
        QVERIFY(!strokes.empty() && strokes.size() <= 3);  // (nine edges, three faces: joined into a few lines)
        std::size_t segments = 0;
        for (const auto& s : strokes) {
            segments += s->points.size() - 1;
            for (const auto& p : s->points) {
                bool corner = false;
                for (const auto& c : corners) corner = corner || (std::abs(p.x - c[0]) < 6e-4 && std::abs(p.y - c[1]) < 6e-4);
                QVERIFY2(corner, "a point of the traced box is one of its corners");
            }
        }
        std::size_t seen = 0;
        for (const auto& e : prim3d::edges(*prim(doc, "box"), &camera)) seen += e.seen ? 1 : 0;
        QCOMPARE(segments, seen);
        QVERIFY(seen >= 4 && seen <= 9);
    }

    // A figure's lines are hidden where another prim is nearer to the viewer (SPEC FIGURE-01, 線): a box, a head or another
    // figure in front of it, with the page's camera and without one; a prim behind it, in another panel, or seen with
    // a camera of its own changes nothing; the same bytes each time.
    void figureBehindAnotherPrim() {
        namespace prim3d = genko::core::prim3d;
        namespace mesh3d = genko::core::mesh3d;
        Document doc = genko::core::new_episode("3D", Num(1), 1, genko::core::PageSpec::b5_doujin());
        {
            Layer pen;
            pen.id = "pen";
            pen.role = LayerRole::User;
            pen.kind = LayerKind::Strokes;
            doc.edit_page(0).layers.push_back(pen);
        }
        doc = bus().apply(doc, ops(R"([{"op": "add_figure", "page": 1, "id": "fig", "pos": [80, 130, 0], "preset": "walk"}])"), Actor("genko")).doc;
        const auto with = [&](const Document& from, const std::string& batch) { return bus().apply(from, ops(batch), Actor("genko")).doc; };
        // the figure's traced points (by trace_prims or render_prims' lines)
        const auto traced = [&](const Document& from, const char* op = "trace_prims") {
            mesh3d::clear_caches();
            Json batch = ops(R"([{"page": 1, "layer_id": "pen", "ids": ["fig"]}])");
            batch[0]["op"] = op;
            if (std::string(op) == "render_prims") batch[0]["surfaces"] = false;
            const Document d = bus().apply(from, batch, Actor("genko")).doc;
            std::vector<prim3d::Line2> out;
            for (const auto& s : layer(d, "pen")->strokes->items) {
                prim3d::Line2 line;
                for (const auto& p : s->points) line.push_back(prim3d::Point2{p.x, p.y});
                out.push_back(std::move(line));
            }
            return out;
        };
        // how many of the points are inside (x0, y0)–(x1, y1)
        const auto inside = [](const std::vector<prim3d::Line2>& lines, double x0, double y0, double x1, double y1) {
            int n = 0;
            for (const auto& line : lines) {
                for (const auto& p : line) n += (p[0] > x0 && p[0] < x1 && p[1] > y0 && p[1] < y1) ? 1 : 0;
            }
            return n;
        };
        // how many are more than `margin` mm inside the box's outline on the page (the hull of its corners)
        const auto behind_box = [](const std::vector<prim3d::Line2>& lines, std::vector<prim3d::Point2> pts, double margin) {
            std::sort(pts.begin(), pts.end());
            const auto turn = [](const prim3d::Point2& o, const prim3d::Point2& a, const prim3d::Point2& b) {
                return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
            };
            std::vector<prim3d::Point2> hull;  // (Andrew's monotone chain: counter-clockwise in x right, y up)
            for (int pass = 0; pass < 2; ++pass) {
                const std::size_t start = hull.size();
                for (const auto& p : pts) {
                    while (hull.size() >= start + 2 && turn(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
                    hull.push_back(p);
                }
                hull.pop_back();
                std::reverse(pts.begin(), pts.end());
            }
            int n = 0;
            for (const auto& line : lines) {
                for (const auto& p : line) {
                    bool in = true;
                    for (std::size_t i = 0; i < hull.size() && in; ++i) {
                        const auto& a = hull[i];
                        const auto& b = hull[(i + 1) % hull.size()];
                        in = turn(a, b, p) / std::hypot(b[0] - a[0], b[1] - a[1]) > margin;
                    }
                    n += in ? 1 : 0;
                }
            }
            return n;
        };
        for (const bool camera : {false, true}) {
            Document page = camera ? with(doc, R"([{"op": "set_camera", "page": 1, "turn": 0.3, "tip": 0.2, "target": [80, 130]}])") : doc;
            const std::vector<prim3d::Line2> alone = traced(page);
            QVERIFY(!alone.empty());
            QCOMPARE(traced(page), alone);  // (the same each time)
            // a thin box in front of the figure's middle: nothing of the figure inside it, the rest still there
            const Document front = with(page, R"([{"op": "add_prim3d", "page": 1, "id": "wall", "kind": "box", "pos": [80, 130, -60], "size": [36, 40, 4], "rot": [0, 0, 0]}])");
            const Json& extra = front.page(0).extra;
            const Json* cam = mesh3d::camera_of(*prim(front, "wall"), extra.contains("camera") ? &extra["camera"] : nullptr);
            const std::vector<prim3d::Point2> corners = prim3d::project(*prim(front, "wall"), cam);
            const double by = prim3d::bbox(*prim(front, "wall"), cam)[1];
            const std::vector<prim3d::Line2> hidden = traced(front);
            QVERIFY2(behind_box(alone, corners, 2.0) > 20, "the figure has lines where the box is");
            QCOMPARE(behind_box(hidden, corners, 2.0), 0);
            QVERIFY(inside(hidden, 0, 0, 1000, by - 2) > 20);  // (the head and shoulders above the box)
            QCOMPARE(traced(front), hidden);
            const std::vector<prim3d::Line2> drawn = traced(front, "render_prims");  // (render_prims draws the same lines)
            QVERIFY(!drawn.empty());
            QCOMPARE(behind_box(drawn, corners, 2.0), 0);
            // the same box behind the figure, a box in front in another panel, or one seen with a camera of its own
            QCOMPARE(traced(with(page, R"([{"op": "add_prim3d", "page": 1, "id": "wall", "kind": "box", "pos": [80, 130, 60], "size": [36, 40, 4], "rot": [0, 0, 0]}])")), alone);
            Document elsewhere = front;
            for (Json& p : elsewhere.edit_page(0).prims) {
                if (p.value("id", std::string()) == "wall") p["frame_id"] = elsewhere.page(0).leaf_frames().front()->id;
            }
            QCOMPARE(traced(elsewhere), alone);
            for (Json& p : elsewhere.edit_page(0).prims) {
                if (p.value("id", std::string()) == "fig") p["frame_id"] = elsewhere.page(0).leaf_frames().front()->id;
            }
            QCOMPARE(traced(elsewhere), hidden);  // (both in the one panel)
            if (!camera) {
                Document own = front;
                for (Json& p : own.edit_page(0).prims) {
                    if (p.value("id", std::string()) == "wall") p["camera"] = ops(R"({"turn": 0.3, "tip": 0.2, "target": [80, 130]})");
                }
                QCOMPARE(traced(own), alone);
            }
            // a head and another figure in front: less of the figure's lines; behind, the same lines
            const auto length = [](const std::vector<prim3d::Line2>& lines) {
                double sum = 0.0;
                for (const auto& line : lines) {
                    for (std::size_t i = 0; i + 1 < line.size(); ++i) sum += std::hypot(line[i + 1][0] - line[i][0], line[i + 1][1] - line[i][1]);
                }
                return sum;
            };
            const Document head = with(page, R"([{"op": "add_head", "page": 1, "id": "h", "pos": [80, 110, -60], "size_mm": 40}])");
            qInfo("camera %d: the figure's lines %.1f mm, behind a head %.1f mm", camera, length(alone), length(traced(head)));
            QVERIFY(length(traced(head)) < length(alone) - 20.0);
            // (half in front of it: the camera's turn moves what is nearer to the right)
            const Document other = with(page, camera ? R"([{"op": "add_figure", "page": 1, "id": "f2", "pos": [65, 135, -50], "preset": "stand"}])"
                                                     : R"([{"op": "add_figure", "page": 1, "id": "f2", "pos": [95, 135, -50], "preset": "stand"}])");
            qInfo("camera %d: behind another figure %.1f mm", camera, length(traced(other)));
            QVERIFY(length(traced(other)) < length(alone) - 20.0);
            QCOMPARE(traced(with(page, R"([{"op": "add_figure", "page": 1, "id": "f2", "pos": [95, 135, 50], "preset": "stand"}])")), alone);
        }
    }

    // 3D guides are drawn in the proof and the name, never printed; a region drawn alone is the whole page cut.
    void drawnWhereGuidesAre() {
        Document guides = base_;
        guides.edit_page(0).numero = false;  // (nombres are not drawn yet in this build)
        Document bare = guides;
        bare.edit_page(0).prims = Json::array();
        const auto draw = [](const Document& doc, const std::string& mode, std::optional<render::RenderRegion> region = std::nullopt) {
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = mode;
            options.region = region;
            return render::render_page(doc.page(0), 72, options, &doc).image;
        };
        QVERIFY(draw(guides, "print").tobytes() == draw(bare, "print").tobytes());
        for (const char* mode : {"proof", "name"}) {
            const render::Image whole = draw(guides, mode);
            QVERIFY2(whole.tobytes() != draw(bare, mode).tobytes(), mode);
            for (const render::RenderRegion r : {render::RenderRegion{100, 200, 150, 120}, render::RenderRegion{0, 0, 37, 400},
                                                 render::RenderRegion{whole.width() - 60, whole.height() - 50, 60, 50}}) {
                const render::Image cut = whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h});
                QVERIFY2(draw(guides, mode, r).tobytes() == cut.tobytes(), mode);
            }
        }
    }

    // A few of the numbers the geometry rests on.
    void geometryBasics() {
        QCOMPARE(genko::core::py_remainder(5.5, 2.0), -0.5);  // (math.remainder: to the nearest, halves to even)
        QCOMPARE(genko::core::py_remainder(7.0, 2.0), -1.0);
        QCOMPARE(genko::core::py_remainder(-7.0, 2.0), 1.0);
        QCOMPARE(genko::core::py_hypot(3.0, 4.0, 12.0), 13.0);
        // a page's surfaces drawn in a window are the whole picture's pixels there
        const std::vector<Json> prims(base_.page(0).prims.begin(), base_.page(0).prims.end());
        const auto whole = genko::core::mesh3d::raster(prims, 300, 400, 40.0, nullptr, nullptr, 0.35);
        QVERIFY(whole.any());
        const genko::core::mesh3d::Window window{50, 80, 210, 300};
        const auto part = genko::core::mesh3d::raster(prims, 300, 400, 40.0, nullptr, nullptr, 0.35, 0.0, 0.0, &window);
        QCOMPARE(part.x0, 50);
        QCOMPARE(part.y0, 80);
        QCOMPARE(part.width, 160);
        QCOMPARE(part.height, 220);
        int compared = 0;
        for (int y = 0; y < part.height; ++y) {
            for (int x = 0; x < part.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * part.width + x;
                const std::size_t j = static_cast<std::size_t>(y + 80) * 300 + (x + 50);
                QCOMPARE(part.alpha(i), whole.alpha(j));
                if (!whole.alpha(j)) continue;
                QCOMPARE(std::bit_cast<std::uint32_t>(part.zbuf[i]), std::bit_cast<std::uint32_t>(whole.zbuf[j]));
                QCOMPARE(std::bit_cast<std::uint32_t>(part.shade[i]), std::bit_cast<std::uint32_t>(whole.shade[j]));
                ++compared;
            }
        }
        QVERIFY(compared > 1000);
    }
};

QTEST_GUILESS_MAIN(TestOps3d)
#include "test_ops_3d.moc"
