// The ops of M3-A1 through core::CommandBus with render::ops_registry, without Python (their words, books and pictures
// against Python's apply_ops: test_contract_raster_ops). For every op: a batch that succeeds and does what it says,
// with the book it was given left as it was and only the pages it changes copied; the same batch as a dry run; a
// batch that fails and leaves the book as it was; and, for an op on a page, the page locked by a person refusing an
// AI. Then the rules of strict_gates for these ops; Undo and Redo of a saved change through the journal; a book saved
// and read back; and what this build refuses where Python breaks or hangs (numbers that are not finite, sizes beyond
// the limits of render/op_limits.hpp, saved areas that name each other, patches that cannot be drawn, a filter plugin
// that only the plugin runner may run, a fill colour past 0..255, a folder grouped with a layer in it, a locked layer,
// a picture whose pixels cannot be decoded) or lets an op through that it should not (strict_gates looking at another
// layer than the op changes, set_paper on every page past another's page lock).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/selection.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::ApplyError;
using genko::core::ApplyResult;
using genko::core::Actor;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::core::Layer;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Page;

namespace {

const std::string kPerson = "human:作者";
const std::string kAi = "ai:hermes";

// A picture (Pillow: 20 × 16, RGBA) and a grey one (30 × 20).
const std::string kPicture =
    "iVBORw0KGgoAAAANSUhEUgAAABQAAAAQCAYAAAAWGF8bAAAAXklEQVR4nGNgGOyAEZ/kCTm5/9jELR49wqkPqwQug4gxmIlcw3CpZcSmwOfbqkZ0hVu4wuqJcSnchaS4DJ9LMbxMKRj8BmKNFFIB1khBlyDHMAwDSTUUm1qqZ73BDwCofSgUOvhv4gAAAABJRU5ErkJggg==";
const std::string kGrey =
    "iVBORw0KGgoAAAANSUhEUgAAAB4AAAAUCAAAAAC/wNIYAAAAXklEQVR4nH2SQRLAIAgDN07//2V6qa3QABeUJBgYCaYQ8eQRxpMEtb1m+CSJDgdg9RAg0cs1qrWb26EFcA3S15pbB636Y69ayBfjXO4cBvyri8mV63WE5Lz5E/n1M24N1gkwJ2mpfAAAAABJRU5ErkJggg==";

// A batch's "PICTURE" and "GREY" given as the pictures above (each batch stays one raw string literal: moc reads raw
// strings cut by "+" wrong).
Json parse(std::string text) {
    for (const auto& [from, to] : {std::pair<std::string, const std::string*>{"PICTURE", &kPicture}, {"GREY", &kGrey}}) {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to->size())) {
            text.replace(at, from.size(), *to);
        }
    }
    return genko::core::parse_python_json(text);
}

CommandBus bus() { return CommandBus(genko::render::ops_registry()); }

// (not "apply": std::apply would be found for a std::string argument)
ApplyResult run_ops(const Document& doc, const std::string& batch, const std::string& actor = "genko", bool dry_run = false) {
    return bus().apply(doc, parse(batch), Actor(actor), dry_run);
}

std::string error_of(const Document& doc, const std::string& batch, const std::string& actor = "genko") {
    try {
        bus().apply(doc, parse(batch), Actor(actor));
    } catch (const ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return "(applied)";
}

// What `work` gives, worked out on another thread: the test is stopped (qFatal) when it has not ended in time (an op that
// went round for ever would hold the test until ctest's timeout, or until memory ran out).
template <class Work>
auto in_time(Work work, int seconds = 10) -> decltype(work()) {
    using Result = decltype(work());
    auto outcome = std::make_shared<std::promise<Result>>();
    std::future<Result> done = outcome->get_future();
    std::thread([work = std::move(work), outcome] {
        try {
            outcome->set_value(work());
        } catch (...) {
            outcome->set_exception(std::current_exception());
        }
    }).detach();
    if (done.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) qFatal("not ended after %d s", seconds);
    return done.get();
}

std::string base64(const std::string& bytes) { return QByteArray::fromStdString(bytes).toBase64().toStdString(); }

// A picture's base64 PNG: `width` × `height` pixels of one colour.
std::string png_base64(std::string_view mode, int width, int height, const genko::render::Ink& colour) {
    return base64(genko::render::write_png(genko::render::Image::create(mode, {width, height}, colour)));
}

// CRC-32, a PNG chunk's checksum.
std::uint32_t crc32_of(std::string_view data) {
    std::uint32_t crc = 0xffffffffU;
    for (const char ch : data) {
        crc ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k) crc = (crc & 1U) != 0 ? 0xedb88320U ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

// The PNG with the data of its first chunk of `type` changed, its length and checksum written for the new data.
std::string with_chunk(std::string png, std::string_view type, const std::function<void(std::string&)>& change) {
    const auto be32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
    };
    const std::size_t at = png.find(type, 8);  // (the chunk's type: its length before it, its data and checksum after)
    const std::size_t length = (static_cast<std::size_t>(static_cast<unsigned char>(png[at - 4])) << 24) |
                               (static_cast<std::size_t>(static_cast<unsigned char>(png[at - 3])) << 16) |
                               (static_cast<std::size_t>(static_cast<unsigned char>(png[at - 2])) << 8) |
                               static_cast<std::size_t>(static_cast<unsigned char>(png[at - 1]));
    std::string data = png.substr(at + 4, length);
    change(data);
    const std::string chunk = std::string(type) + data;
    png.replace(at - 4, 4 + 4 + length + 4, be32(static_cast<std::uint32_t>(data.size())) + chunk + be32(crc32_of(chunk)));
    return png;
}

const Layer* layer_by_id(const Page& page, const std::string& id) {
    for (const Layer& layer : page.layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

bool has_pixels(const Layer* layer) { return layer != nullptr && layer->raster_png && !layer->raster_png->empty(); }

// The book: three small pages (70 × 95 mm at 150 dpi). Page 1, its name approved: a closed ink line, a paint layer with
// a filled square, a pen layer with a line, a folder holding a pen layer, a locked paint layer and a kept area; pages
// 2 and 3 empty.
Document fixture() {
    Document doc = genko::core::new_episode("塗り", 1, 3, genko::core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6, 150));
    doc.nombre = Json::object({{"show", false}});  // (nombres are drawn from M4 on: the fills look at the page without)
    return run_ops(doc, R"([
        {"op": "name_ok", "page": 1},
        {"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0, "width_mm": 0.8,
         "points": [[15, 20], [30, 20], [30, 40], [15, 40], [15, 20]]},
        {"op": "add_layer", "page": 1, "kind": "paint", "id": "paint-1"},
        {"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [40, 50, 10, 10]}, "rgb": [200, 0, 0]},
        {"op": "add_layer", "page": 1, "kind": "pen", "id": "pen-1"},
        {"op": "add_stroke", "page": 1, "layer_id": "pen-1", "stabilize": 0, "points": [[10, 70], [50, 80]], "width_mm": 1},
        {"op": "add_layer", "page": 1, "kind": "folder", "id": "fold-1"},
        {"op": "add_layer", "page": 1, "kind": "pen", "id": "in-a", "parent": "fold-1", "after": "fold-1"},
        {"op": "add_layer", "page": 1, "kind": "paint", "id": "lock-1"},
        {"op": "set_layer", "page": 1, "id": "lock-1", "locked": true},
        {"op": "store_area", "page": 1, "name": "s", "area": {"rect": [10, 10, 20, 20]}}
    ])",
                   kPerson)
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
    std::vector<int> pages_changed;        // the pages (1-based) the batch changes
    std::function<bool(const Document&)> did;  // what the batch did
    std::string failing;                   // fails…
    std::string words;                     // …with these words in its error
    int page = 0;                          // the page the op is on (0: a book op)
};

}  // namespace

class TestRasterOps : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    // Everything about a book: its full snapshot and its project.json as the writer writes it (its pictures and lines
    // by their bytes; not its revision, which every save counts on).
    Json content(const Document& doc) {
        genko::storage::AssetStore store(genko::storage::path_from_utf8((tmp_.path() + "/store").toStdString()));
        Json payload = genko::storage::project_payload_v4(doc, store);
        payload.erase("revision");
        return Json::array({genko::storage::snapshot(doc, true), std::move(payload)});
    }
    std::string state(const Document& doc) { return genko::core::dump_python(content(doc)); }

    // The same content, the keys of objects in any order (a book Undo or Redo restores is a state of its history,
    // canonical JSON: what it keeps as it was given comes back with its keys sorted, as in M1's undo)
    bool same_book(const Document& a, const Json& b, std::string* where) { return genko::test::same_content(content(a), b, where); }

    // A book saved into a new folder (its first revision).
    std::filesystem::path saved(const Document& doc, const QString& name) {
        const std::filesystem::path dir = genko::storage::path_from_utf8((tmp_.path() + QLatin1Char('/') + name).toStdString());
        std::filesystem::create_directories(dir);
        genko::storage::ProjectLock lock(dir, "genko");
        lock.try_acquire();
        genko::storage::SaveRequest request;
        request.ops = Json::array();
        genko::storage::Saver(lock).save(doc, request);
        return dir;
    }

    static Document read(const std::filesystem::path& dir) {
        const genko::core::ScopedIdSource ids(genko::core::counting_ids());
        const auto loaded = genko::storage::load_document(dir);
        if (!loaded.report.clean()) throw std::runtime_error(genko::core::dump_python(loaded.report.to_json()));
        return loaded.document;
    }

private slots:
    void strokeEditsCannotBypassApprovalWithNumericLayerIds() {
        const Document doc = run_ops(fixture(), R"([{"op":"add_layer","page":2,"kind":"pen","id":"1"},{"op":"add_stroke","page":2,"layer_id":"1","stabilize":0,"points":[[10,10],[20,20]]},{"op":"set_meta","strict_gates":true}])", kPerson).doc;
        const auto line = layer_by_id(doc.page(1), "1")->strokes->items.front();
        const std::string before = state(doc);
        for (const std::string& name : {std::string("set_stroke_width"),std::string("reshape_stroke")}) {
            Json request = Json::object({{"op",name},{"page",2},{"layer_id",1},{"width_mm",0.3}});
            if (name == "set_stroke_width") request["ids"] = Json::array({line->id});
            else request["stroke_id"] = line->id;
            for (const Json& id : {Json(1),Json("1")}) {
                request["layer_id"] = id;
                const std::string refused = error_of(doc, genko::core::dump_python(Json::array({request})), kAi);
                QVERIFY2(refused.find("needs name_ok") != std::string::npos, refused.c_str());
                QCOMPARE(state(doc), before);
            }
            request["layer_id"] = 1;
            for (const Document& allowed : {run_ops(doc, R"([{"op":"name_ok","page":2}])", kPerson).doc,
                                           run_ops(doc, R"([{"op":"set_layer","page":2,"id":"1","exportable":false}])", kPerson).doc}) {
                const auto result = bus().apply(allowed, Json::array({request}), Actor(kAi));
                QCOMPARE(layer_by_id(result.doc.page(1), "1")->strokes->items.front()->width_mm, 0.3);
            }
        }
    }

    void strokeWidthRejectsExcessColourComponents() {
        const Document doc = fixture();
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string before = state(doc);
        for (const Json& rgb : {Json::array({1,2,3,4}), Json::array({1,2,3,-1})}) {
            const Json batch = Json::array({Json::object({{"op","set_note"},{"page",1},{"note","must not survive"}}),
                Json::object({{"op","set_stroke_width"},{"page",1},{"layer_id","pen-1"},
                    {"ids",Json::array({selected->id})},{"rgb",rgb}})});
            const std::string error = error_of(doc, genko::core::dump_python(batch), kPerson);
            QVERIFY2(error != "(applied)" && error.find("rgb") != std::string::npos, error.c_str());
            QCOMPARE(state(doc), before);
        }
        const Json valid = Json::array({Json::object({{"op","set_stroke_width"},{"page",1},{"layer_id","pen-1"},
            {"ids",Json::array({selected->id})},{"rgb",Json::array({1,2,3})}})});
        const auto changed = bus().apply(doc, valid, Actor(kPerson));
        QCOMPARE(layer_by_id(changed.doc.page(0), "pen-1")->strokes->items.front()->rgb, (std::vector<std::int64_t>{1,2,3}));
    }

    void strokeWidthRejectsUnhashableIdsBeforeEditing() {
        const Document doc = fixture();
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string before = state(doc);
        for (const Json& invalid : {Json::array(), Json::object()}) {
            const Json batch = Json::array({Json::object({{"op","set_note"},{"page",1},{"note","must not survive"}}),
                Json::object({{"op","set_stroke_width"},{"page",1},{"layer_id","pen-1"},
                    {"ids",Json::array({selected->id,invalid})},{"area",Json::object({{"rect",Json::array({5,60,50,30})}})},{"width_mm",0.3}})});
            const std::string error = error_of(doc, genko::core::dump_python(batch), kPerson);
            QVERIFY2(error != "(applied)" && error.find("unhashable") != std::string::npos, error.c_str());
            QCOMPARE(state(doc), before);
        }
        const Json valid = Json::array({Json::object({{"op","set_stroke_width"},{"page",1},{"layer_id","pen-1"},
            {"ids",Json::array({selected->id,42,nullptr})},{"width_mm",0.3}})});
        const auto changed = bus().apply(doc, valid, Actor(kPerson));
        QCOMPARE(layer_by_id(changed.doc.page(0), "pen-1")->strokes->items.front()->width_mm, 0.3);
    }

    void reshapeRejectsNonFiniteCoordinatesAndPressure() {
        const Document doc = fixture();
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string before = state(doc);
        for (const Json& point : {Json::array({"nan",0}), Json::array({0,"inf"}), Json::array({0,0,"-inf"}), Json::array({0,0,"1e999"})}) {
            const Json batch = Json::array({Json::object({{"op","set_note"},{"page",1},{"note","must not survive"}}),
                Json::object({{"op","reshape_stroke"},{"page",1},{"layer_id","pen-1"},{"stroke_id",selected->id},
                    {"points",Json::array({point,Json::array({20,30,0.5})})}})});
            const std::string error = error_of(doc, genko::core::dump_python(batch), kPerson);
            QVERIFY2(error != "(applied)" && error.find("finite") != std::string::npos, error.c_str());
            QCOMPARE(state(doc), before);
        }
        const Json valid = Json::array({Json::object({{"op","reshape_stroke"},{"page",1},{"layer_id","pen-1"},{"stroke_id",selected->id},
            {"points",Json::array({Json::array({"10","20","0.2"}),Json::array({20,30,0.5})})}})});
        const auto changed = bus().apply(doc, valid, Actor(kPerson));
        QCOMPARE(layer_by_id(changed.doc.page(0), "pen-1")->strokes->items.front()->pressure, (std::vector<double>{0.2,0.5}));
    }

    void strokeWidthClampsNegativeOverflow() {
        const Document doc = fixture();
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string before = state(doc);
        const Json ops = Json::array({Json::object({
            {"op", "set_stroke_width"}, {"page", 1}, {"layer_id", "pen-1"},
            {"ids", Json::array({selected->id})}, {"width_mm", 1e308}, {"scale", -1e308}})});
        QCOMPARE(error_of(doc, ops.dump(), kPerson), std::string("(applied)"));
        const auto changed = bus().apply(doc, ops, Actor(kPerson));
        QCOMPARE(layer_by_id(changed.doc.page(0), "pen-1")->strokes->items.front()->width_mm, 0.05);
        QCOMPARE(layer_by_id(changed.doc.page(0), "pen-1")->strokes->items.front()->id, selected->id);
        QCOMPARE(state(doc), before);
    }

    void strokeWidthRejectsOverflowAtomically() {
        const Document doc = fixture();
        const std::string before = state(doc);
        const std::string batch = R"([{"op":"set_note","page":1,"note":"must not survive"},{"op":"set_stroke_width","page":1,"layer_id":"pen-1","area":{"rect":[5,60,50,30]},"width_mm":1e308,"scale":1e308}])";
        const std::string error = error_of(doc, batch, kPerson);
        QVERIFY2(error != "(applied)" && error.find("finite") != std::string::npos, error.c_str());
        QCOMPARE(state(doc), before);
        const auto valid = run_ops(doc, R"([{"op":"set_stroke_width","page":1,"layer_id":"pen-1","area":{"rect":[5,60,50,30]},"width_mm":0.2,"scale":2}])", kPerson);
        QCOMPARE(layer_by_id(valid.doc.page(0), "pen-1")->strokes->items.front()->width_mm, 0.4);
    }

    void reshapeWithMorePointsClearsAnIncompletePressure() {
        Document doc = fixture();
        auto& page = doc.edit_page(0);
        for (auto& layer : page.layers) {
            if (layer.id != "pen-1") continue;
            auto stroke = *layer.strokes->items.front();
            stroke.points = {{10,70},{30,75},{50,80}};
            stroke.pressure = {0.2,0.4,0.8};
            layer.strokes = genko::core::make_strokes({std::make_shared<const genko::core::Stroke>(std::move(stroke))});
        }
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string unchanged = state(doc);
        const Json ops = Json::array({Json::object({{"op","reshape_stroke"},{"page",1},{"layer_id","pen-1"},
            {"stroke_id",selected->id},{"points",Json::array({Json::array({11,71}),Json::array({20,72}),Json::array({30,73}),Json::array({40,75})})}})});
        const auto after = bus().apply(doc, ops, Actor(kPerson));
        QCOMPARE(layer_by_id(after.doc.page(0), "pen-1")->strokes->items.front()->pressure,
                (std::vector<double>{}));
        QCOMPARE(state(doc), unchanged);
    }

    void reshapeWithoutPressureKeepsTheExistingPrefix() {
        Document doc = fixture();
        auto& page = doc.edit_page(0);
        for (auto& layer : page.layers) {
            if (layer.id != "pen-1") continue;
            auto stroke = *layer.strokes->items.front();
            stroke.points = {{10,70},{30,75},{50,80}};
            stroke.pressure = {0.2,0.4,0.8};
            layer.strokes = genko::core::make_strokes({std::make_shared<const genko::core::Stroke>(std::move(stroke))});
        }
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string unchanged = state(doc);
        const Json ops = Json::array({Json::object({{"op","reshape_stroke"},{"page",1},{"layer_id","pen-1"},
            {"stroke_id",selected->id},{"points",Json::array({Json::array({11,71}),Json::array({40,75})})}})});
        const auto after = bus().apply(doc, ops, Actor(kPerson));
        QCOMPARE(layer_by_id(after.doc.page(0), "pen-1")->strokes->items.front()->pressure,
                (std::vector<double>{0.2,0.4}));
        QCOMPARE(state(doc), unchanged);
    }

    void reshapeKeepsIdentityAndReplacesPressure() {
        const Document doc = fixture();
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const std::string unchanged = state(doc);
        const Json ops = Json::array({Json::object({{"op","reshape_stroke"},{"page",1},{"layer_id","pen-1"},
            {"stroke_id",selected->id},{"points",Json::array({Json::array({11,71,0.2}),Json::array({40,75,0.7})})},
            {"width_mm",0.4}})});
        std::optional<ApplyResult> outcome;
        try { outcome.emplace(bus().apply(doc, ops, Actor(kPerson))); }
        catch (const ApplyError& error) { QFAIL(error.what()); }
        const auto after = layer_by_id(outcome->doc.page(0), "pen-1")->strokes->items.front();
        QCOMPARE(after->id, selected->id);
        QCOMPARE(after->kind, selected->kind);
        QCOMPARE(after->rgb, selected->rgb);
        QCOMPARE(after->points.size(), std::size_t(2));
        QCOMPARE(after->points[0].x, 11.0);
        QCOMPARE(after->points[0].y, 71.0);
        QCOMPARE(after->points[1].x, 40.0);
        QCOMPARE(after->points[1].y, 75.0);
        QCOMPARE(after->pressure, (std::vector<double>{0.2,0.7}));
        QCOMPARE(after->width_mm, 0.4);
        QVERIFY(after != selected);
        QVERIFY(outcome->doc.pages[1] == doc.pages[1]);
        QCOMPARE(state(doc), unchanged);
    }

    void strokeWidthRefusesInvalidColourAtomically() {
        const Document doc = fixture();
        const std::string unchanged = state(doc);
        const auto selected = layer_by_id(doc.page(0), "pen-1")->strokes->items.front();
        const Json ops = Json::array({Json::object({{"op","set_note"},{"page",1},{"note","rejected prefix"}}),
            Json::object({{"op","set_stroke_width"},{"page",1},{"layer_id","pen-1"},
                {"ids",Json::array({selected->id})},{"width_mm",2},{"rgb",Json::array({-1,2,3})}})});
        bool refused = false;
        try { bus().apply(doc, ops, Actor(kPerson)); }
        catch (const ApplyError& error) {
            refused = true;
            QVERIFY(std::string(error.what()).find("ops[1] set_stroke_width") != std::string::npos);
            QVERIFY(std::string(error.what()).find("rgb") != std::string::npos);
        }
        QVERIFY2(refused, "RGB outside 0..255 must not enter a saved stroke");
        QCOMPARE(state(doc), unchanged);
    }

    void strokeWidthRestylesTheAreaSelection() {
        const Document doc = run_ops(fixture(), R"([{"op":"add_stroke","page":1,"layer_id":"pen-1","stabilize":0,"points":[[25,60],[45,60]],"width_mm":1.5}])", kPerson).doc;
        const auto before = layer_by_id(doc.page(0), "pen-1")->strokes;
        const std::string unchanged = state(doc);
        std::optional<ApplyResult> outcome;
        try {
            outcome.emplace(run_ops(doc, R"([{"op":"set_stroke_width","page":1,"layer_id":"pen-1","area":{"rect":[5,65,20,20]},"width_mm":0.1,"scale":3,"kind":"oil","rgb":[1,2,3]}])", kPerson));
        } catch (const ApplyError& error) { QFAIL(error.what()); }
        const auto after = layer_by_id(outcome->doc.page(0), "pen-1")->strokes;
        QCOMPARE(after->items[0]->width_mm, 0.1 * 3.0);
        QCOMPARE(after->items[0]->kind, std::string("marker"));
        QCOMPARE(after->items[0]->rgb, (std::vector<std::int64_t>{1,2,3}));
        QCOMPARE(after->items[0]->points, before->items[0]->points);
        QCOMPARE(after->items[0]->pressure, before->items[0]->pressure);
        QCOMPARE(after->items[0]->id, before->items[0]->id);
        QVERIFY(after->items[1] == before->items[1]);
        QCOMPARE(state(doc), unchanged);
    }

    void strokeWidthChangesOnlyRequestedIds() {
        const Document doc = run_ops(fixture(), R"([{"op":"add_stroke","page":1,"layer_id":"pen-1","stabilize":0,"points":[[25,60],[45,60]],"width_mm":1.5}])", kPerson).doc;
        const Layer* before = layer_by_id(doc.page(0), "pen-1");
        QVERIFY(before != nullptr && before->strokes->items.size() == 2);
        const auto selected = before->strokes->items[0];
        const auto other = before->strokes->items[1];
        const std::string unchanged = state(doc);
        const Json ops = Json::array({Json::object({{"op", "set_stroke_width"}, {"page", 1},
                {"layer_id", "pen-1"}, {"ids", Json::array({selected->id})}, {"width_mm", 0.25}})});
        std::optional<ApplyResult> outcome;
        try {
            outcome.emplace(bus().apply(doc, ops, Actor(kPerson)));
        } catch (const ApplyError& error) {
            QFAIL(error.what());
        }
        const auto& result = *outcome;
        const Layer* after = layer_by_id(result.doc.page(0), "pen-1");
        QVERIFY(after != nullptr);
        QCOMPARE(after->strokes->items.size(), std::size_t(2));
        QCOMPARE(after->strokes->items[0]->width_mm, 0.25);
        QCOMPARE(after->strokes->items[0]->id, selected->id);
        QCOMPARE(after->strokes->items[0]->points, selected->points);
        QCOMPARE(after->strokes->items[0]->pressure, selected->pressure);
        QVERIFY(after->strokes->items[0] != selected);
        QVERIFY(after->strokes->items[1] == other);
        QVERIFY(result.doc.pages[0] != doc.pages[0]);
        QVERIFY(result.doc.pages[1] == doc.pages[1]);
        QCOMPARE(state(doc), unchanged);
    }
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        QDir().mkpath(tmp_.path() + "/config/plugins");
        qputenv("GENKO_CONFIG_DIR", (tmp_.path() + "/config").toUtf8());
    }

    void everyOp() {
        const Document doc = fixture();
        const Page& p1 = doc.page(0);
        const std::vector<std::pair<std::string, std::string>> names{{"BG1", p1.first_layer(LayerRole::Bg)->id},
            {"SID1", layer_by_id(p1, "pen-1")->strokes->items.front()->id}};
        const std::vector<Case> cases{
            {"set_stroke_width", R"([{"op":"set_stroke_width","page":1,"layer_id":"pen-1","ids":["SID1"],"width_mm":0.25}])", kPerson, {1},
             [](const Document& d) { return layer_by_id(d.page(0), "pen-1")->strokes->items.front()->width_mm == 0.25; },
             R"([{"op":"set_note","page":1,"note":"must not survive"},{"op":"set_stroke_width","page":1,"layer_id":"pen-1","ids":["missing"],"width_mm":0.5}])", "no line there", 1},
            {"reshape_stroke", R"([{"op":"reshape_stroke","page":1,"layer_id":"pen-1","stroke_id":"SID1","points":[[20,71,0.3],[40,79,0.7]],"width_mm":0.4}])", kPerson, {1},
             [](const Document& d) { const auto& s = *layer_by_id(d.page(0), "pen-1")->strokes->items.front();
                 return s.width_mm == 0.4 && s.points.size() == 2 && s.points.front().x == 20 && s.pressure == std::vector<double>{0.3,0.7}; },
             R"([{"op":"set_note","page":1,"note":"must not survive"},{"op":"reshape_stroke","page":1,"layer_id":"pen-1","stroke_id":"SID1","points":[[1,2]]}])", "points needs at least two", 1},
            {"convert_layer", R"([{"op": "convert_layer", "page": 1, "id": "pen-1", "to": "paint"}])", "genko", {1},
             [](const Document& d) {
                 const Layer* l = layer_by_id(d.page(0), "pen-1");
                 return l != nullptr && l->kind == LayerKind::Raster && has_pixels(l) && l->stroke_count() == 0;
             },
             R"([{"op": "convert_layer", "page": 1, "id": "pen-1", "to": "sticker"}])", "to must be paint or pen", 1},
            {"merge_down", R"([{"op": "merge_down", "page": 1, "id": "pen-1"}])", "genko", {1},
             [](const Document& d) { return layer_by_id(d.page(0), "pen-1") == nullptr && has_pixels(layer_by_id(d.page(0), "paint-1")); },
             R"([{"op": "merge_down", "page": 1, "id": "BG1"}])", "there is no layer below to merge into", 1},
            {"merge_layers", R"([{"op": "merge_layers", "page": 1, "ids": ["paint-1", "pen-1"], "name": "まとめ"}])", "genko", {1},
             [&doc](const Document& d) {
                 const auto& layers = d.page(0).layers;
                 return layers.size() + 1 == doc.page(0).layers.size() &&
                        std::any_of(layers.begin(), layers.end(), [](const Layer& l) { return l.title == "まとめ" && has_pixels(&l); });
             },
             R"([{"op": "merge_layers", "page": 1, "ids": ["pen-1"]}])", "choose two or more layers to merge", 1},
            {"merge_visible", R"([{"op": "merge_visible", "page": 1, "id": "vis"}])", "genko", {1},
             [](const Document& d) { return has_pixels(layer_by_id(d.page(0), "vis")); },
             R"([{"op": "merge_visible", "page": 1, "id": "pen-1"}])", "layer pen-1 exists", 1},
            {"move_layers", R"([{"op": "move_layers", "page": 1, "ids": ["pen-1"], "after": "bottom"}])", kAi, {1},
             [](const Document& d) { return d.page(0).layers.front().id == "pen-1"; },
             R"([{"op": "move_layers", "page": 1, "ids": ["pen-1"], "parent": "paint-1"}])", "parent must be a folder", 1},
            {"group_layers", R"([{"op": "group_layers", "page": 1, "ids": ["pen-1", "paint-1"], "id": "g1", "name": "人物"}])", "genko",
             {1},
             [](const Document& d) {
                 const Layer* g = layer_by_id(d.page(0), "g1");
                 const Layer* pen = layer_by_id(d.page(0), "pen-1");
                 return g != nullptr && g->kind == LayerKind::Folder && pen != nullptr && pen->parent_id == Json("g1");
             },
             R"([{"op": "group_layers", "page": 1, "ids": []}])", "ids is the list of layer ids", 1},
            {"set_layer_mask", R"([{"op": "set_layer_mask", "page": 1, "id": "pen-1", "area": {"rect": [5, 60, 30, 30]}}])", "genko", {1},
             [](const Document& d) { return layer_by_id(d.page(0), "pen-1")->mask.has_value(); },
             R"([{"op": "set_layer_mask", "page": 1, "id": "fold-1", "fill": "hide"}])", "a folder cannot take a mask", 1},
            {"paint_mask", R"([{"op": "paint_mask", "page": 1, "id": "paint-1", "points": [[40, 50], [50, 60]], "show": false}])",
             "genko", {1}, [](const Document& d) { return layer_by_id(d.page(0), "paint-1")->mask.has_value(); },
             R"([{"op": "paint_mask", "page": 1, "id": "paint-1", "points": []}])", "points needs at least one [x_mm, y_mm] pair", 1},
            {"put_raster", R"([{"op": "put_raster", "page": 2, "png_base64": "PICTURE"}])", "genko", {2},
             [](const Document& d) { return has_pixels(d.page(1).first_layer(LayerRole::Ink)); },
             R"([{"op": "put_raster", "page": 2, "png_base64": "aGVsbG8sIG5vdCBhIHBpY3R1cmU="}])",
             "not a readable image: cannot identify image file", 2},
            {"filter_raster", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "blur", "radius": 2}])", "genko", {1},
             [](const Document& d) { return has_pixels(layer_by_id(d.page(0), "paint-1")); },
             R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "sparkle"}])", "unknown filter sparkle", 1},
            {"fill", R"([{"op": "fill", "page": 1, "x_mm": 22, "y_mm": 30, "rgb": [240, 120, 40]}])", "genko", {1},
             [](const Document& d) { return d.page(0).first_layer(LayerRole::Ink)->patches.size() == 1; },
             R"([{"op": "fill", "page": 1, "y_mm": 30}])", "not found: 'x_mm'", 1},
            {"fill_area", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"poly": [[10, 10], [30, 12], [20, 30]]}}])",
             "genko", {1}, [](const Document& d) { return layer_by_id(d.page(0), "paint-1")->patches.size() == 2; },
             R"([{"op": "fill_area", "page": 1}])", "area is {poly: [[x, y], …]} or {mask: {box, png}}", 1},
            {"fill_enclosed", R"([{"op": "fill_enclosed", "page": 1, "poly": [[12, 15], [34, 15], [34, 44], [12, 44]]}])", "genko", {1},
             [](const Document& d) { return d.page(0).first_layer(LayerRole::Ink)->patches.size() == 1; },
             R"([{"op": "fill_enclosed", "page": 1, "poly": [[1, 2], [3, 4]]}])", "poly needs three points or more (the lasso)", 1},
            {"fill_gaps", R"([{"op": "fill_gaps", "page": 1, "layer_id": "paint-1", "max_mm": 3}])", "genko", {1},
             [](const Document& d) { return layer_by_id(d.page(0), "paint-1") != nullptr; },
             R"([{"op": "fill_gaps", "page": 1, "layer_id": "fold-1"}])", "this layer cannot be painted on", 1},
            {"flood_fill", R"([{"op": "flood_fill", "page": 1, "layer": "bg", "x_mm": 5, "y_mm": 5, "rgb": [0, 0, 200]}])", "genko", {1},
             [](const Document& d) { return has_pixels(d.page(0).first_layer(LayerRole::Bg)); },
             R"([{"op": "flood_fill", "page": 1, "x_mm": 5, "y_mm": 5, "rgb": [1, 2, 3, 4, 5]}])",
             "color must be int, or tuple of one, three or four elements", 1},
            {"gradient_fill", R"([{"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80]}])", "genko", {2},
             [](const Document& d) { return d.page(1).first_layer(LayerRole::Ink)->patches.size() == 1; },
             R"([{"op": "gradient_fill", "page": 2, "from": [10, 10]}])", "gradient_fill needs from and to: [x_mm, y_mm]", 2},
            {"delete_area", R"([{"op": "delete_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]}}])", "genko", {1},
             [](const Document& d) { return layer_by_id(d.page(0), "pen-1")->stroke_count() == 0; },
             R"([{"op": "delete_area", "page": 1, "layer_id": "lock-1", "area": {"rect": [5, 60, 50, 30]}}])", "the layer is locked", 1},
            {"transform_area",
             R"([{"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]}, "matrix": [1, 0, 0, 1, 3, 0]}])",
             "genko", {1},
             [](const Document& d) {
                 const Layer* l = layer_by_id(d.page(0), "pen-1");
                 return l->stroke_count() == 1 && l->strokes->items[0]->points.front().x == 13.0;
             },
             R"([{"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]}, "matrix": [1, 0, 0, 1, 0]}])",
             "matrix is [a, b, c, d, e, f]", 1},
            {"paste",
             R"([{"op": "paste", "page": 1, "layer_id": "pen-1", "items": {"strokes": [{"points": [[12, 12], [30, 12]]}],
                 "patches": [{"box": [20, 20, 10, 6.667], "mode": "mask", "png": "GREY", "rgb": [200, 10, 10]}]}}])",
             "genko", {1},
             [](const Document& d) {
                 const Layer* l = layer_by_id(d.page(0), "pen-1");
                 return l->stroke_count() == 2 && l->patches.size() == 1;
             },
             R"([{"op": "paste", "page": 1, "items": {}}])", "nothing to paste", 1},
            {"store_area", R"([{"op": "store_area", "page": 1, "name": "t", "area": {"ellipse": [10, 10, 20, 20]}}])", kAi, {1},
             [](const Document& d) { return d.page(0).extra["saved_areas"].contains("t") && d.page(0).extra["saved_areas"].contains("s"); },
             R"([{"op": "store_area", "page": 1, "name": "  ", "area": {"rect": [1, 1, 5, 5]}}])", "name is required", 1},
            {"forget_area", R"([{"op": "forget_area", "page": 1, "name": "s"}])", "genko", {1},
             [](const Document& d) { return !d.page(0).extra.contains("saved_areas") || !d.page(0).extra["saved_areas"].contains("s"); },
             R"([{"op": "forget_area", "page": 1, "name": "nope"}])", "no saved area nope", 1},
            {"set_paper", R"([{"op": "set_paper", "page": 1, "rgb": [250, 240, 220]}])", "genko", {1},
             [](const Document& d) { return d.page(0).extra["paper_rgb"] == Json::array({250, 240, 220}); },
             R"([{"op": "set_paper", "page": 1, "rgb": [300, 0, 0]}])", "rgb is [r, g, b], each 0..255", 1},
            {"set_timelapse", R"([{"op": "set_timelapse"}])", kAi, {},
             [](const Document& d) { return d.extra["timelapse"] == Json::object({{"on", true}}); },
             R"([{"op": "set_timelapse"}, {"op": "set_paper", "page": 9, "rgb": [1, 2, 3]}])", "set_paper: page not found"},
            {"erase", R"([{"op": "erase", "page": 1, "layer_id": "paint-1", "points": [[40, 50], [50, 60]], "width_mm": 4}])", "genko", {1},
             [](const Document& d) { return has_pixels(layer_by_id(d.page(0), "paint-1")); },
             R"([{"op": "erase", "page": 1, "layer_id": "paint-1", "points": [[40, 50], [50, 60]], "texture": "glitter"}])",
             "texture must be hard, soft or rough", 1},
            {"erase_raster",
             R"([{"op": "erase_raster", "page": 1, "layer_id": "paint-1", "points": [[40, 50], [50, 60]], "texture": "soft"}])", "genko",
             {1}, [](const Document& d) { return has_pixels(layer_by_id(d.page(0), "paint-1")); },
             R"([{"op": "erase_raster", "page": 1, "layer_id": "lock-1", "points": [[40, 50], [50, 60]]}])", "the layer is locked", 1},
        };
        QCOMPARE(cases.size(), std::size_t{27});
        std::set<std::string> ops;
        const std::string before = state(doc);
        for (const Case& c : cases) {
            ops.insert(c.op);
            const std::string batch = replaced(c.batch, names);
            const QByteArray what = QByteArray::fromStdString(c.op + ": ");
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
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                const bool changed =
                    std::find(c.pages_changed.begin(), c.pages_changed.end(), static_cast<int>(i + 1)) != c.pages_changed.end();
                QVERIFY2((result.doc.pages[i].get() != doc.pages[i].get()) == changed,
                         what + "page " + QByteArray::number(int(i + 1)) + (changed ? " was not copied" : " was copied"));
            }
            // a dry run shows the same book (and the caller keeps the book it had)
            const ApplyResult dry = counted(true);
            QVERIFY2(state(dry.doc) == state(result.doc), what + "a dry run shows another book");
            QVERIFY2(state(doc) == before, what + "the dry run changed the book given");
            // the failing batch: refused with its words, the book as it was
            const std::string error = error_of(doc, replaced(c.failing, names));
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
        QCOMPARE(ops.size(), std::size_t{27});
    }

    void strictGates() {
        // page 2 not approved, with a paint layer (printed) and a pen layer kept out of print
        const Document doc = run_ops(fixture(), R"([
            {"op": "set_meta", "strict_gates": true},
            {"op": "add_layer", "page": 2, "kind": "paint", "id": "paint-2"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "trial"},
            {"op": "set_layer", "page": 2, "id": "trial", "exportable": false}
        ])",
                                     kPerson)
                                 .doc;
        const std::vector<std::pair<std::string, std::string>> refused{
            {R"([{"op": "fill", "page": 2, "x_mm": 30, "y_mm": 40}])", "fill on ink needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "flood_fill", "page": 2, "x_mm": 30, "y_mm": 40}])", "flood_fill on ink needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "put_raster", "page": 2, "layer": "finish", "png_base64": "PICTURE"}])",
             "put_raster on finish needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "fill_area", "page": 2, "layer_id": "paint-2", "area": {"rect": [1, 1, 5, 5]}}])",
             "fill_area on a printed layer needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "set_layer_mask", "page": 2, "id": "paint-2", "fill": "hide"}])",
             "fill on a printed layer needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "merge_visible", "page": 2}])", "merge_visible on a printed layer needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "merge_layers", "page": 2, "ids": ["paint-2", "trial"]}])",
             "merge_layers on a printed layer needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "gradient_fill", "page": 2, "from": [1, 1], "to": [50, 50]}])",
             "gradient_fill on ink needs name_ok on page 2 (strict_gates)"},
            {R"([{"op": "filter_raster", "page": 2, "id": "paint-2", "kind": "invert"}])",
             "filter_raster on a printed layer needs name_ok on page 2 (strict_gates)"},
        };
        for (const auto& [batch, words] : refused) {
            const std::string error = error_of(doc, batch, kAi);
            QVERIFY2(error.find(words) != std::string::npos, (batch + " → " + error).c_str());
        }
        // what is not printed may be drawn on before the name is approved (a filter too); page 1 is approved
        for (const std::string& batch :
             {std::string(R"([{"op": "fill_area", "page": 2, "layer_id": "trial", "area": {"rect": [1, 1, 5, 5]}}])"),
              std::string(R"([{"op": "fill_area", "page": 2, "layer": "name", "area": {"rect": [1, 1, 5, 5]}}])"),
              std::string(R"([{"op": "filter_raster", "page": 2, "id": "trial", "kind": "invert"}])"),
              std::string(R"([{"op": "fill", "page": 1, "x_mm": 22, "y_mm": 30}])"),
              std::string(R"([{"op": "move_layers", "page": 2, "ids": ["paint-2"], "after": "bottom"}])")}) {
            const std::string error = error_of(doc, batch, kAi);
            QVERIFY2(error == "(applied)", (batch + " → " + error).c_str());
        }
        // a person approves the name, then the AI may fill
        const Document approved = run_ops(doc, R"([{"op": "name_ok", "page": 2}])", kPerson).doc;
        QCOMPARE(error_of(approved, R"([{"op": "fill", "page": 2, "x_mm": 30, "y_mm": 40}])", kAi), std::string("(applied)"));
    }

    // strict_gates looks at the layer the op works on, found as the op finds it: put_raster and filter_raster by "id",
    // flood_fill by its role alone ("layer", ink by default), the others by "layer_id" — a key the op does not read
    // decides nothing. Before the name is approved the printed ink layer P is refused, the draft D may be drawn on.
    void strictGatesTheLayerEdited() {
        const Document doc = run_ops(fixture(), R"([
            {"op": "put_raster", "page": 2, "layer": "draft", "png_base64": "PICTURE"},
            {"op": "set_meta", "strict_gates": true}
        ])",
                                     kPerson)
                                 .doc;
        const Page& p2 = doc.page(1);
        QVERIFY(!p2.name_ok);
        QVERIFY(p2.first_layer(LayerRole::Ink) != nullptr && p2.first_layer(LayerRole::Ink)->exportable);
        QVERIFY(p2.first_layer(LayerRole::Draft) != nullptr);
        const std::vector<std::pair<std::string, std::string>> names{{"P_ID", p2.first_layer(LayerRole::Ink)->id},
                                                                     {"D_ID", p2.first_layer(LayerRole::Draft)->id}};
        const std::string before = state(doc);
        const std::vector<std::pair<std::string, std::string>> refused{
            {R"([{"op": "filter_raster", "page": 2, "id": "P_ID", "kind": "invert"}])", "filter_raster on a printed layer"},
            {R"([{"op": "filter_raster", "page": 2, "id": "P_ID", "layer_id": "D_ID", "kind": "invert"}])", "filter_raster on a printed layer"},
            {R"([{"op": "put_raster", "page": 2, "id": "P_ID", "png_base64": "PICTURE"}])", "put_raster on a printed layer"},
            {R"([{"op": "put_raster", "page": 2, "id": "P_ID", "layer_id": "D_ID", "png_base64": "PICTURE"}])", "put_raster on a printed layer"},
            {R"([{"op": "put_raster", "page": 2, "layer": "finish", "layer_id": "D_ID", "png_base64": "PICTURE"}])", "put_raster on finish"},
            {R"([{"op": "flood_fill", "page": 2, "x_mm": 30, "y_mm": 40, "layer_id": "D_ID"}])", "flood_fill on ink"},
            {R"([{"op": "flood_fill", "page": 2, "layer": "ink", "x_mm": 30, "y_mm": 40, "layer_id": "D_ID"}])", "flood_fill on ink"},
            {R"([{"op": "flood_fill", "page": 2, "layer": "bg", "x_mm": 30, "y_mm": 40, "layer_id": "D_ID"}])", "flood_fill on bg"},
        };
        std::string wrong;  // (every case that goes wrong, said at the end)
        for (const auto& [batch, words] : refused) {
            const std::string error = error_of(doc, replaced(batch, names), kAi);
            if (error.find(words + " needs name_ok on page 2 (strict_gates)") == std::string::npos) wrong += "\n" + batch + " → " + error;
            QVERIFY2(state(doc) == before, (batch + ": the failing batch changed the book").c_str());
        }
        for (const std::string& batch : {
                 std::string(R"([{"op": "filter_raster", "page": 2, "id": "D_ID", "kind": "invert"}])"),
                 std::string(R"([{"op": "filter_raster", "page": 2, "id": "D_ID", "layer_id": "P_ID", "kind": "invert"}])"),
                 std::string(R"([{"op": "put_raster", "page": 2, "id": "D_ID", "png_base64": "PICTURE"}])"),
                 std::string(R"([{"op": "put_raster", "page": 2, "layer": "draft", "layer_id": "P_ID", "png_base64": "PICTURE"}])"),
                 std::string(R"([{"op": "flood_fill", "page": 2, "layer": "draft", "x_mm": 30, "y_mm": 40}])"),
                 std::string(R"([{"op": "flood_fill", "page": 2, "layer": "draft", "x_mm": 30, "y_mm": 40, "layer_id": "P_ID"}])"),
             }) {
            const std::string error = error_of(doc, replaced(batch, names), kAi);
            if (error != "(applied)") wrong += "\n" + batch + " → " + error;
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
        // the name approved, P may be filtered
        const Document approved = run_ops(doc, R"([{"op": "name_ok", "page": 2}])", kPerson).doc;
        QCOMPARE(error_of(approved, replaced(R"([{"op": "filter_raster", "page": 2, "id": "P_ID", "kind": "invert"}])", names), kAi),
                 std::string("(applied)"));
    }

    // A change saved through the journal is undone (the book as it was) and redone (as it was after the change).
    void undoAndRedo() {
        const Document doc = fixture();
        const std::vector<std::pair<std::string, std::string>> names{{"SID1", layer_by_id(doc.page(0), "pen-1")->strokes->items.front()->id}};
        const std::vector<std::string> batches{
            R"([{"op":"set_stroke_width","page":1,"layer_id":"pen-1","ids":["SID1"],"width_mm":0.3,"scale":2,"rgb":[30,40,50]}])",
            R"([{"op":"reshape_stroke","page":1,"layer_id":"pen-1","stroke_id":"SID1","points":[[20,71,0.3],[40,79,0.7]],"width_mm":0.4}])",
            R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"ellipse": [10, 10, 20, 20]}, "rgb": [0, 0, 255]}])",
            R"([{"op": "put_raster", "page": 2, "png_base64": "PICTURE"}])",
            R"([{"op": "merge_down", "page": 1, "id": "pen-1"}])",
            R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "mosaic", "block": 4}])",
            R"([{"op": "set_layer_mask", "page": 1, "id": "pen-1", "area": {"saved": "s"}, "invert": true}])",
            R"([{"op": "store_area", "page": 2, "name": "x", "area": {"rect": [1, 1, 9, 9]}}, {"op": "set_paper", "rgb": [240, 240, 230]}])",
            R"([{"op": "erase", "page": 1, "layer_id": "paint-1", "points": [[40, 50], [50, 60]], "texture": "rough"}])",
            R"([{"op": "transform_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [38, 48, 14, 14]}, "matrix": [0, 1, -1, 0, 100, 10]}])",
            R"([{"op": "set_timelapse"}, {"op": "group_layers", "page": 1, "ids": ["pen-1", "paint-1"], "id": "g"}])",
        };
        int n = 0;
        for (const std::string& batch : batches) {
            ++n;
            const std::filesystem::path dir = saved(doc, QStringLiteral("undo-%1.genko").arg(n));
            const Document original = read(dir);
            const Json before = content(original);
            ApplyResult result;
            {
                genko::storage::ProjectLock lock(dir, kPerson);
                lock.try_acquire();
                result = run_ops(original, replaced(batch, names), kPerson);
                genko::storage::SaveRequest request;
                request.actor = kPerson;
                request.base_revision = original.revision;
                request.ops = result.journal_ops;
                genko::storage::Saver(lock).save(result.doc, request);
            }
            const Json after = content(read(dir));
            QVERIFY2(after != before, batch.c_str());
            std::string where;
            QVERIFY2(same_book(result.doc, after, &where), ("saved: " + batch + " " + where).c_str());
            const auto undone = genko::storage::restore(dir, kPerson, false, false);
            QCOMPARE(undone.kind, std::string("undo"));
            QVERIFY2(same_book(read(dir), before, &where), ("undo: " + batch + " " + where).c_str());
            const auto redone = genko::storage::restore(dir, kPerson, true, false);
            QCOMPARE(redone.kind, std::string("redo"));
            QVERIFY2(same_book(read(dir), after, &where), ("redo: " + batch + " " + where).c_str());
            // another actor's change is not undone by an AI
            try {
                genko::storage::restore(dir, kAi, false, false);
                QFAIL("an AI undid a person's change");
            } catch (const genko::core::Error& error) {
                QCOMPARE(error.code(), std::string("other_actor"));
            }
        }
    }

    // The book after every op, saved and read back, is the book before the save: its pixels, masks, patches, kept
    // areas, paper and switches.
    void savedAndReadBack() {
        const Document doc = run_ops(fixture(), R"([
            {"op": "put_raster", "page": 2, "png_base64": "PICTURE"},
            {"op": "fill", "page": 1, "x_mm": 22, "y_mm": 30, "rgb": [240, 120, 40]},
            {"op": "gradient_fill", "page": 3, "from": [10, 10], "to": [60, 80], "shape": "radial"},
            {"op": "set_layer_mask", "page": 1, "id": "pen-1", "area": {"rect": [5, 60, 30, 30]}},
            {"op": "paint_mask", "page": 1, "id": "paint-1", "points": [[40, 50], [50, 60]], "show": false},
            {"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "noise", "amount": 0.3},
            {"op": "merge_visible", "page": 1, "id": "vis"},
            {"op": "convert_layer", "page": 1, "id": "in-a", "to": "paint"},
            {"op": "flood_fill", "page": 3, "layer": "bg", "x_mm": 3, "y_mm": 3, "rgb": [230, 240, 255]},
            {"op": "store_area", "page": 3, "name": "e", "area": {"ellipse": [10, 10, 30, 20]}},
            {"op": "set_paper", "page": 2, "rgb": [250, 245, 230]},
            {"op": "set_timelapse"},
            {"op": "group_layers", "page": 1, "ids": ["pen-1", "vis"], "id": "g"}
        ])",
                                     kPerson)
                                 .doc;
        const std::filesystem::path dir = saved(doc, "saved.genko");
        const Document back = read(dir);
        Document expected = doc;
        for (std::size_t i = 0; i < expected.pages.size(); ++i) {
            if (!expected.page(i).selected_frame_id.is_null()) expected.edit_page(i).selected_frame_id = nullptr;  // (not saved)
        }
        std::string where;
        QVERIFY2(genko::test::strict_equal(genko::storage::snapshot(back, true), genko::storage::snapshot(expected, true), &where),
                 where.c_str());
        genko::storage::AssetStore store(genko::storage::path_from_utf8((tmp_.path() + "/store").toStdString()));
        Json got = genko::storage::project_payload_v4(back, store);
        Json want = genko::storage::project_payload_v4(expected, store);
        got.erase("revision");
        want.erase("revision");
        QVERIFY2(genko::test::strict_equal(got, want, &where), where.c_str());
        // the pictures the book keeps are all there
        int pictures = 0;
        for (const auto& page : back.pages) {
            for (const Layer& layer : page->layers) {
                pictures += has_pixels(&layer) ? 1 : 0;
                pictures += static_cast<int>(layer.patches.size()) + (layer.mask ? 1 : 0);
            }
        }
        QVERIFY2(pictures >= 9, std::to_string(pictures).c_str());
    }

    // What this build refuses where Python goes on to break the book, hang or crash.
    void refusedOnTheSafeSide() {
        const Document doc = fixture();
        const std::vector<std::pair<std::string, std::string>> refused{
            // numbers that are not finite (float("nan"), float("inf"), float("1e999"): Python runs into pixels it cannot
            // have; the JSON literals NaN and Infinity this build reads as null)
            {R"([{"op": "fill", "page": 1, "x_mm": "nan", "y_mm": 30}])", "fill: x_mm must be a finite number"},
            {R"([{"op": "fill_area", "page": 1, "area": {"poly": [[1, 1], ["inf", 5], [3, 9]]}}])", "must be a finite number"},
            {R"([{"op": "gradient_fill", "page": 2, "from": ["nan", 1], "to": [50, 50]}])", "from must be a finite number"},
            {R"([{"op": "paint_mask", "page": 1, "id": "paint-1", "points": [[1, 1]], "width_mm": "1e999"}])", "must be a finite number"},
            {R"([{"op": "paste", "page": 1, "items": {"strokes": [{"points": [[12, 12], [30, 12]]}]}, "matrix": [1, 0, 0, 1, "-inf", 0]}])",
             "matrix must be a finite number"},
            {R"([{"op": "transform_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [38, 48, 14, 14]},
                  "matrix": [1, 0, 0, 1, "nan", 0]}])",
             "must be a finite number"},
            // sizes beyond the limits (Python would try and run out of memory or time)
            {R"([{"op": "fill", "page": 1, "x_mm": 22, "y_mm": 30, "gap_mm": 1e12}])", "gap_mm is too large"},
            {R"([{"op": "flood_fill", "page": 1, "layer": "bg", "x_mm": 1, "y_mm": 1, "gap_mm": 1000}])", "gap_mm is too large"},
            {R"([{"op": "paint_mask", "page": 1, "id": "paint-1", "points": [[1, 1]], "width_mm": 1e9}])", "width_mm is too large"},
            {R"([{"op": "erase", "page": 1, "layer_id": "paint-1", "points": [[1, 1], [1e9, 1]]}])", "too"},
            {R"([{"op": "fill_gaps", "page": 1, "layer_id": "paint-1", "max_mm": 1e9}])", "max_mm is too large"},
            {R"([{"op": "fill_area", "page": 1, "area": {"rect": [1, 1, 5, 5], "grow_mm": 1e9}}])", "grow_mm is too large"},
            {R"([{"op": "transform_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [38, 48, 14, 14]},
                  "matrix": [1, 0, 0, 1, 1e15, 0]}])",
             "the transform is too far off the page"},
            {R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "blur", "radius": 1e9}])", "radius is too large"},
            // a patch Python keeps without a box or a picture it can open: every drawing of the page would fail
            {R"([{"op": "paste", "page": 1, "items": {"patches": [{"box": [1, 2, 3, 4], "png": "aGVsbG8="}]}}])",
             "items: a patch needs a box [x, y, w, h] (mm) and a png (a picture, base64)"},
            {R"([{"op": "paste", "page": 1, "items": {"patches": [{"png": "GREY"}]}}])",
             "items: a patch needs a box [x, y, w, h] (mm) and a png (a picture, base64)"},
        };
        for (const auto& [batch, words] : refused) {
            const std::string error = error_of(doc, batch);
            QVERIFY2(error.find(words) != std::string::npos, (batch + " → " + error).c_str());
        }
        // saved areas that name each other (a book edited by hand): Python recurses until it fails; this build stops
        Document looped = doc;
        looped.edit_page(0).extra["saved_areas"] =
            Json::object({{"a", Json::object({{"saved", "b"}})}, {"b", Json::object({{"saved", "a"}})}});
        const std::string error = error_of(looped, R"([{"op": "fill_area", "page": 1, "area": {"saved": "a"}}])");
        QVERIFY2(error.find("the area is nested too deeply (saved areas in a loop?)") != std::string::npos, error.c_str());
        // a filter plugin someone installed runs in the plugin runner (COMP-04), not here; one that is not there is
        // refused as Python refuses it
        QVERIFY(error_of(doc, R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "plugin:sepia"}])")
                    .find("no plugin sepia") != std::string::npos);
        genko::test::write_bytes(tmp_.path() + "/config/plugins/sepia.py", "def run(image, params):\n    return image\n");
        const std::string plugin = error_of(doc, R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "plugin:sepia"}])");
        QVERIFY2(plugin.starts_with("not_yet_ported: "), plugin.c_str());
        QVERIFY2(plugin.find("a filter plugin (sepia)") != std::string::npos, plugin.c_str());
    }

    // flood_fill on a picture of three white pixels writes the colour asked for, as Pillow's fill does (all three; none
    // for a colour within its threshold of white or of fewer than three numbers). A value past 0..255, which Pillow
    // writes clipped, is refused and the batch leaves the book as it was: clipped, it can be the white the fill goes
    // over, which this build's fill went round for ever.
    void floodFillColours_data() {
        QTest::addColumn<QByteArray>("rgb");
        QTest::addColumn<QByteArray>("after");  // the pixels after (RGB); empty: refused
        QTest::newRow("black") << QByteArray("[0, 0, 0]") << QByteArray(9, '\0');
        QTest::newRow("four numbers") << QByteArray("[10, 20, 30, 40]") << QByteArray::fromHex("0a141e0a141e0a141e");
        QTest::newRow("within the threshold") << QByteArray("[255, 255, 250]") << QByteArray(9, '\xff');
        QTest::newRow("two numbers") << QByteArray("[1, 2]") << QByteArray(9, '\xff');
        QTest::newRow("below 0") << QByteArray("[-100, 0, 0]") << QByteArray();
        QTest::newRow("past 255") << QByteArray("[0, 0, 256]") << QByteArray();
        QTest::newRow("clipped to white") << QByteArray("[300, 255, 255]") << QByteArray();
        QTest::newRow("clipped near white") << QByteArray("[255, 250, 264]") << QByteArray();
    }

    void floodFillColours() {
        QFETCH(QByteArray, rgb);
        QFETCH(QByteArray, after);
        const Document doc = run_ops(fixture(), replaced(R"([{"op": "put_raster", "page": 2, "layer": "bg", "png_base64": "WHITE"}])",
                                                         {{"WHITE", png_base64("RGB", 3, 1, genko::render::Ink{255, 255, 255})}}))
                                 .doc;
        const std::string before = state(doc);
        const std::string batch = replaced(R"([{"op": "set_paper", "page": 2, "rgb": [250, 240, 220]},
            {"op": "flood_fill", "page": 2, "layer": "bg", "x_mm": 0, "y_mm": 0, "rgb": RGB}])",
                                           {{"RGB", rgb.toStdString()}});
        const std::string error = in_time([doc, batch] { return error_of(doc, batch); });
        if (after.isEmpty()) {
            QVERIFY2(error.find("ops[1] flood_fill: rgb is [r, g, b], each 0..255") != std::string::npos, error.c_str());
            QVERIFY2(state(doc) == before, "the failing batch changed the book");
            return;
        }
        QCOMPARE(error, std::string("(applied)"));
        const ApplyResult result = run_ops(doc, batch);
        const Layer* bg = result.doc.page(1).first_layer(LayerRole::Bg);
        QVERIFY(has_pixels(bg));
        const genko::render::Image image = genko::render::read_png(*bg->raster_png);
        QCOMPARE(std::string(image.mode()), std::string("RGB"));
        QCOMPARE(QByteArray::fromStdString(image.tobytes()), after);
    }

    // group_layers: refuse a destination that would put the new folder inside a selected folder (F in N in F), before
    // anything moves. Selecting an ancestor and child is valid when their actual destination stays outside the ancestor;
    // leaves the book as it was; layers side by side are grouped. A loop the page already has (a book edited by hand)
    // stops the search.
    void groupLayersInThemselves() {
        // page 2, bottom to top: the folder F, C and S in it, the folder G in it and D in G (each above its folder); then
        // the pen layer K in the folder E, under it
        const Document doc = run_ops(fixture(), R"([
            {"op": "add_layer", "page": 2, "kind": "folder", "id": "F"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "C", "parent": "F", "after": "F"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "S", "parent": "F", "after": "C"},
            {"op": "add_layer", "page": 2, "kind": "folder", "id": "G", "parent": "F", "after": "S"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "D", "parent": "G", "after": "G"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "K"},
            {"op": "add_layer", "page": 2, "kind": "folder", "id": "E"},
            {"op": "move_layers", "page": 2, "ids": ["K"], "parent": "E"}
        ])")
                                 .doc;
        const auto parent = [](const Document& d, const std::string& id) { return layer_by_id(d.page(1), id)->parent_id; };
        QCOMPARE(parent(doc, "C"), Json("F"));
        QCOMPARE(parent(doc, "D"), Json("G"));
        QCOMPARE(parent(doc, "K"), Json("E"));
        QCOMPARE(doc.page(1).layers.back().id, std::string("E"));
        const std::string before = state(doc);
        // (the topmost selected layer is inside a selected ancestor: the destination would make a loop)
        for (const std::string ids : {R"(["F", "C"])", R"(["F", "D"])", R"(["G", "D", "S"])"}) {
            const std::string batch = replaced(R"([{"op": "set_paper", "page": 2, "rgb": [250, 240, 220]},
                {"op": "group_layers", "page": 2, "ids": IDS, "id": "N"}])",
                                               {{"IDS", ids}});
            const std::string error = in_time([doc, batch] { return error_of(doc, batch); });
            QVERIFY2(error.find("ops[1] group_layers: a folder cannot be grouped with a layer in it") != std::string::npos,
                     (ids + " → " + error).c_str());
            QVERIFY2(state(doc) == before, (ids + ": the failing batch changed the book").c_str());
        }
        // E is topmost and at the root: grouping it with K cannot form a cycle. Preserve Python's valid behaviour.
        const Document ancestor = run_ops(doc, R"([{"op": "group_layers", "page": 2, "ids": ["E", "K"], "id": "N"}])").doc;
        QCOMPARE(parent(ancestor, "N"), Json(nullptr));
        QCOMPARE(parent(ancestor, "E"), Json("N"));
        QCOMPARE(parent(ancestor, "K"), Json("N"));
        // side by side: in their folder, the new folder where they were
        const Document grouped = run_ops(doc, R"([{"op": "group_layers", "page": 2, "ids": ["C", "S"], "id": "N"}])").doc;
        QCOMPARE(parent(grouped, "N"), Json("F"));
        QCOMPARE(parent(grouped, "C"), Json("N"));
        QCOMPARE(parent(grouped, "S"), Json("N"));
        QCOMPARE(parent(grouped, "F"), Json(nullptr));
        const Document apart = run_ops(doc, R"([{"op": "group_layers", "page": 2, "ids": ["F", "K"], "id": "N"}])").doc;
        QCOMPARE(parent(apart, "F"), Json("N"));
        QCOMPARE(parent(apart, "K"), Json("N"));
        QCOMPARE(parent(apart, "N"), Json("E"));
        QCOMPARE(parent(apart, "E"), Json(nullptr));
        // a loop the page has: F in G in F. G is in itself (refused); D, in G, is grouped; the search ends either way
        Document looped = doc;
        for (Layer& layer : looped.edit_page(1).layers) {
            if (layer.id == "F") layer.parent_id = "G";
        }
        const std::string refused = in_time([looped] { return error_of(looped, R"([{"op": "group_layers", "page": 2, "ids": ["G"]}])"); });
        QVERIFY2(refused.find("a folder cannot be grouped with a layer in it") != std::string::npos, refused.c_str());
        const std::string kept = in_time([looped] { return error_of(looped, R"([{"op": "group_layers", "page": 2, "ids": ["D"]}])"); });
        QCOMPARE(kept, std::string("(applied)"));
    }

    // A locked layer is not changed: put_raster, filter_raster, set_layer_mask, paint_mask and flood_fill refuse it as
    // the other drawing ops do, and the batch leaves the book as it was; unlocked, the same ops change it. (The page's
    // lock is asked first, as before.)
    void lockedLayers() {
        // page 1: lock-1, a locked paint layer; page 2: its paper layer (bg) locked
        Document doc = fixture();
        const std::vector<std::pair<std::string, std::string>> names{{"BG2", doc.page(1).first_layer(LayerRole::Bg)->id}};
        doc = run_ops(doc, replaced(R"([{"op": "set_layer", "page": 2, "id": "BG2", "locked": true}])", names), kPerson).doc;
        QVERIFY(layer_by_id(doc.page(0), "lock-1")->locked);
        QVERIFY(doc.page(1).first_layer(LayerRole::Bg)->locked);
        const std::vector<std::pair<std::string, std::string>> edits{
            {"put_raster", R"({"op": "put_raster", "page": 1, "id": "lock-1", "png_base64": "PICTURE"})"},
            {"filter_raster", R"({"op": "filter_raster", "page": 1, "id": "lock-1", "kind": "invert"})"},
            {"set_layer_mask", R"({"op": "set_layer_mask", "page": 1, "id": "lock-1", "fill": "hide"})"},
            {"set_layer_mask", R"({"op": "set_layer_mask", "page": 1, "id": "lock-1", "delete": true})"},
            {"paint_mask", R"({"op": "paint_mask", "page": 1, "id": "lock-1", "points": [[10, 10], [30, 30]]})"},
            {"flood_fill", R"({"op": "flood_fill", "page": 2, "layer": "bg", "x_mm": 5, "y_mm": 5, "rgb": [0, 0, 200]})"},
        };
        const std::string before = state(doc);
        std::string wrong;  // (every case that goes wrong, said at the end)
        for (const auto& [op, edit] : edits) {
            const std::string batch = replaced(R"([{"op": "set_paper", "page": 1, "rgb": [250, 240, 220]}, EDIT])", {{"EDIT", edit}});
            const std::string error = error_of(doc, batch);
            if (error.find("ops[1] " + op + ": the layer is locked") == std::string::npos) wrong += "\n" + edit + " → " + error;
            QVERIFY2(state(doc) == before, (edit + ": the failing batch changed the book").c_str());
        }
        // unlocked: the same edits go through
        const Document unlocked = run_ops(doc, replaced(R"([{"op": "set_layer", "page": 1, "id": "lock-1", "locked": false},
            {"op": "set_layer", "page": 2, "id": "BG2", "locked": false}])", names), kPerson)
                                      .doc;
        for (const auto& [op, edit] : edits) {
            const std::string error = error_of(unlocked, "[" + edit + "]");
            if (error != "(applied)") wrong += "\n(unlocked) " + edit + " → " + error;
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
        // the page locked by a person: an AI is told of the page first
        Document page_locked = doc;
        page_locked.page_locks[doc.page(0).id] = kPerson;
        const std::string refused = error_of(page_locked, "[" + edits[0].second + "]", kAi);
        QVERIFY2(refused.find("page 1 locked by " + kPerson) != std::string::npos, refused.c_str());
    }

    // set_paper with a page of none, null, "" or 0 changes every page: a page someone else has locked refuses it, and the
    // whole batch, as an op on that page is refused (Python asks no page's lock then). With no lock, or the page locked
    // by the one asking, it goes through.
    void paperOnEveryPageLocks() {
        Document book = genko::core::new_episode("紙", 1, 2, genko::core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6, 150));
        Document locked = book;
        locked.page_locks[book.page(1).id] = kPerson;
        Document mine = book;
        mine.page_locks[book.page(1).id] = kAi;
        const std::string before = state(locked);
        const auto papered = [](const Document& d) {
            return genko::core::get_or(d.page(0).extra, "paper_rgb", Json()) == Json::array({250, 240, 220}) &&
                   genko::core::get_or(d.page(1).extra, "paper_rgb", Json()) == Json::array({250, 240, 220});
        };
        std::string wrong;  // (every case that goes wrong, said at the end)
        for (const std::string page : {"", R"("page": null, )", R"("page": "", )", R"("page": 0, )"}) {
            const std::string paper = replaced(R"({"op": "set_paper", PAGE"rgb": [250, 240, 220]})", {{"PAGE", page}});
            const std::string batch = replaced(R"([{"op": "set_note", "page": 1, "note": "紙の色"}, PAPER])", {{"PAPER", paper}});
            const std::string error = error_of(locked, batch, kAi);
            if (error.find("ops[1] set_paper: page 2 locked by " + kPerson) == std::string::npos) wrong += "\n" + paper + " → " + error;
            QVERIFY2(state(locked) == before, (paper + ": the failing batch changed the book").c_str());
            for (const Document* control : {&book, &mine}) {
                try {
                    if (!papered(run_ops(*control, batch, kAi).doc)) wrong += "\n" + paper + ": not every page papered";
                } catch (const ApplyError& e) {
                    wrong += "\n" + paper + (control == &book ? " (no lock) → " : " (locked by the AI) → ") + e.what();
                }
            }
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
        // one page: as before, the page asked for alone
        QCOMPARE(error_of(locked, R"([{"op": "set_paper", "page": 1, "rgb": [250, 240, 220]}])", kAi), std::string("(applied)"));
        QVERIFY(error_of(locked, R"([{"op": "set_paper", "page": 2, "rgb": [250, 240, 220]}])", kAi).find("page 2 locked by") !=
                std::string::npos);
    }

    // put_raster keeps a picture whose pixels are all read, by the reader the pages are drawn with, as the bytes it was
    // given. A PNG whose checksums are right but whose image data cannot be decoded (not zlib, cut short, a compression
    // or interlace method PNG does not have) is refused and the batch leaves the book as it was — Python reads the
    // chunks only and keeps it, and the page cannot be drawn again; so is one past the size limit, however long its
    // sides.
    void putRasterDecoded() {
        const Document doc = fixture();
        const std::string before = state(doc);
        const std::string dot = genko::render::write_png(genko::render::Image::create("RGBA", {1, 1}, genko::render::Ink{10, 20, 30, 255}));
        const auto ihdr = [&dot](std::size_t at, char value) {  // (IHDR: width, height, depth, colour, compression, filter, interlace)
            return with_chunk(dot, "IHDR", [=](std::string& data) { data[at] = value; });
        };
        const std::string long_sides = with_chunk(dot, "IHDR", [](std::string& data) { data.replace(0, 8, std::string(8, '\xff')); });
        const std::vector<std::pair<std::string, std::string>> refused{
            {"image data not zlib", with_chunk(dot, "IDAT", [](std::string& data) { data = "not zlib at all"; })},
            {"image data cut short", with_chunk(dot, "IDAT", [](std::string& data) { data.resize(data.size() / 2); })},
            {"compression method 1", ihdr(10, 1)},
            {"interlace method 2", ihdr(12, 2)},
            {"sides of 2^32 - 1", long_sides},
        };
        std::string wrong;  // (every case that goes wrong, said at the end)
        for (const auto& [what, png] : refused) {
            const std::string batch = replaced(R"([{"op": "set_paper", "page": 2, "rgb": [250, 240, 220]},
                {"op": "put_raster", "page": 2, "png_base64": "PNG"}])",
                                               {{"PNG", base64(png)}});
            const std::string error = error_of(doc, batch);
            if (error.find("ops[1] put_raster: not a readable image: ") == std::string::npos) wrong += "\n" + what + " → " + error;
            QVERIFY2(state(doc) == before, (what + ": the failing batch changed the book").c_str());
        }
        const auto put = [](const std::string& png) {
            return replaced(R"([{"op": "put_raster", "page": 2, "png_base64": "PNG"}])", {{"PNG", base64(png)}});
        };
        // (the pixels counted as Python counts them: (2^32 - 1)², past 64 bits signed)
        const std::string huge = error_of(doc, put(long_sides));
        if (huge.find("Image size (18446744065119617025 pixels) exceeds limit of 178956970 pixels") == std::string::npos) {
            wrong += "\nsides of 2^32 - 1 → " + huge;
        }
        // kept as given: the 20 × 16 picture, the 1 × 1 one and the same interlaced (Adam7: one pass, the same data)
        const std::vector<std::pair<std::string, std::string>> kept{
            {"20 × 16", QByteArray::fromBase64(QByteArray::fromStdString(kPicture)).toStdString()}, {"1 × 1", dot}, {"1 × 1 interlaced", ihdr(12, 1)}};
        for (const auto& [what, png] : kept) {
            try {
                const ApplyResult result = run_ops(doc, put(png));
                const Layer* ink = result.doc.page(1).first_layer(LayerRole::Ink);
                if (!has_pixels(ink) || *ink->raster_png != png) wrong += "\n" + what + ": not kept as given";
            } catch (const ApplyError& error) {
                wrong += "\n" + what + " → " + error.what();
            }
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // --- numbers and areas within bounds ---------------------------------------------------------------------------

    // Each batch, after a set_note on page 1, fails with these words and leaves the book as it was; what goes wrong is
    // added to wrong.
    void expectRefused(const Document& doc, const std::vector<std::pair<std::string, std::string>>& cases, std::string& wrong,
                       int seconds = 60) {
        const std::string before = state(doc);
        for (const auto& [edit, words] : cases) {
            const std::string batch = replaced(R"([{"op": "set_note", "page": 1, "note": "前"}, EDIT])", {{"EDIT", edit}});
            const std::string error = in_time([doc, batch] { return error_of(doc, batch); }, seconds);
            if (error.find(words) == std::string::npos) wrong += "\n" + edit.substr(0, 300) + "\n    → " + error;
            if (state(doc) != before) wrong += "\n" + edit.substr(0, 300) + "\n    → the failing batch changed the book";
        }
    }

    // What a page holds, as a checksum: each layer and its pixels, mask, patches (box and pixels) and lines.
    static std::string look(const Page& page) {
        const auto pixels = [](const std::string& png) {
            const genko::render::Image image = genko::render::read_png(png);
            return std::to_string(image.width()) + "x" + std::to_string(image.height()) + std::string(image.mode()) + ":" +
                   std::to_string(crc32_of(image.tobytes()));
        };
        std::string out;
        for (const Layer& layer : page.layers) {
            out += "|";
            if (has_pixels(&layer)) out += " raster " + pixels(*layer.raster_png);
            if (layer.mask && layer.mask->png) out += " mask " + pixels(*layer.mask->png);
            for (const genko::core::Patch& patch : layer.patches) {
                const Json* box = genko::core::get(patch.attrs, "box");
                out += " patch " + (box != nullptr ? genko::core::dump_python(*box) : std::string("-")) + " " +
                       (patch.png ? pixels(*patch.png) : std::string("-"));
            }
            for (const auto& stroke : layer.strokes->items) {
                out += " line " + genko::core::dump_python(genko::core::stroke_points_json(*stroke)) + " " +
                       genko::core::py_float_repr(stroke->width_mm);
            }
        }
        return std::to_string(crc32_of(out)) + "/" + std::to_string(out.size());
    }

    // Lines moved by paste or transform_area through a matrix or a warp: a point or a width the move makes NaN or an
    // infinity (from finite numbers: 12 × 1e308) is refused before the book keeps it, and the batch leaves the book as
    // it was. A move that stays finite, however far, is kept as before.
    void pastedInputStaysFinite() {
        const Document doc = fixture();
        std::string wrong;
        std::vector<std::pair<std::string, std::string>> cases;
        const auto add = [&](const Json& stroke) {
            const Json op = Json::object({{"op", "paste"}, {"page", 1}, {"layer_id", "pen-1"},
                {"items", Json::object({{"strokes", Json::array({stroke})}})}});
            cases.emplace_back(genko::core::dump_python(op), "ops[1] paste: stroke must contain only finite numbers");
        };
        for (const char* value : {"inf", "-inf", "nan"}) {
            add(Json::object({{"points", Json::array({Json::array({value, 0}), Json::array({1, 1})})}}));
            for (const char* key : {"width_mm", "opacity", "po", "pressure_opacity"}) {
                Json stroke = Json::object({{"points", Json::array({Json::array({0, 0}), Json::array({1, 1})})}});
                stroke[key] = value;
                add(stroke);
            }
            for (const char* key : {"pressure", "rotation"}) {
                Json stroke = Json::object({{"points", Json::array({Json::array({0, 0}), Json::array({1, 1})})}});
                stroke[key] = Json::array({value, 1});
                add(stroke);
            }
        }
        const std::vector<double> finite{0, 0, 1, 1};
        const std::vector<double> bad{std::numeric_limits<double>::infinity(), 0, 1, 1};
        for (const char* key : {"xy", "p", "r"}) {
            Json stroke = Json::object({{"xy", genko::core::pack_doubles(finite)}});
            stroke[key] = genko::core::pack_doubles(bad);
            add(stroke);
        }
        expectRefused(doc, cases, wrong);
        QVERIFY2(wrong.empty(), wrong.c_str());
        const auto ok = run_ops(doc, R"([{"op":"paste","page":1,"layer_id":"pen-1","items":{"strokes":[{"points":[["0","0"],["1","1"]],"pressure":["0.5",1],"rotation":[0,45],"width_mm":"0.2","opacity":"0.7"}]}}])").doc;
        const auto& line = *layer_by_id(ok.page(0), "pen-1")->strokes->items.back();
        QCOMPARE(line.width_mm, 0.2);
        QCOMPARE(line.opacity, 0.7);
        QCOMPARE(line.points.size(), std::size_t(2));
    }

    void movedLinesStayFinite() {
        const Document doc = fixture();
        const std::string items = R"("items": {"strokes": [{"points": [[12, 12], [30, 12]]}]})";
        const auto paste = [&items](const std::string& matrix) {
            return replaced(R"({"op": "paste", "page": 1, "layer_id": "pen-1", ITEMS, "matrix": MATRIX})", {{"ITEMS", items}, {"MATRIX", matrix}});
        };
        const std::string far = "ops[1] paste: the transform is too far off the page";
        const std::string wide = "ops[1] paste: the transform makes a line too wide";
        std::string wrong;  // (every case that goes wrong, said at the end)
        expectRefused(doc,
                      {
                          {paste("[1e308, 0, 0, 1, 0, 0]"), far},
                          {paste(R"(["1e308", 0, 0, "1e308", 0, 0])"), far},
                          {paste("[1e307, 0, 0, 1, 1e308, 0]"), far},
                          {paste("[1e308, 0, -1e308, 1, 0, 0]"), far},
                          {paste("[1e200, 0, 0, 1e200, 0, 0]"), wide},
                          {paste("[1e200, 1e200, 1e200, 1e200, 0, 0]"), wide},
                          {R"({"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]},
                              "matrix": [1e308, 0, 0, 1, 0, 0]})",
                           "ops[1] transform_area: the transform is too far off the page"},
                          {R"({"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]},
                              "warp": {"perspective": [[0, 0], [1e308, 0], [1e308, 1e308], [0, 1e308]]}})",
                           "ops[1] transform_area: the transform stretches the area too far"},
                          {R"({"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]},
                              "warp": {"perspective": [["0", "0"], ["1e308", "0"], ["1e308", "1e308"], ["0", "1e308"]]}})",
                           "ops[1] transform_area: the transform stretches the area too far"},
                      },
                      wrong);
        // finite: kept, the numbers as Python works them out (a·x + c·y + e; the width × √|det|, rounded to 4 places)
        const auto last_line = [](const Document& d) { return layer_by_id(d.page(0), "pen-1")->strokes->items.back(); };
        try {
            const auto moved = last_line(run_ops(doc, "[" + paste("[2, 0, 0, 2, 1, 1]") + "]").doc);
            if (moved->points.size() != 2 || moved->points[0].x != 25.0 || moved->points[0].y != 25.0 || moved->points[1].x != 61.0 ||
                moved->width_mm != 0.7) {
                wrong += "\n[2, 0, 0, 2, 1, 1]: " + genko::core::dump_python(genko::core::stroke_points_json(*moved));
            }
            const auto kept = last_line(run_ops(doc, "[" + paste("[1e300, 0, 0, 1e-300, 0, 0]") + "]").doc);
            if (kept->points.size() != 2 || kept->points[0].x != 1e300 * 12.0 + 0.0 * 12.0 + 0.0 || kept->points[1].x != 1e300 * 30.0 ||
                kept->points[0].y != 0.0 * 12.0 + 1e-300 * 12.0 + 0.0 || !std::isfinite(kept->width_mm)) {
                wrong += "\n[1e300, 0, 0, 1e-300, 0, 0]: " + genko::core::dump_python(genko::core::stroke_points_json(*kept));
            }
            const Document warped = run_ops(doc, R"([{"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]},
                "warp": {"perspective": [[5, 60], [57, 62], [54, 92], [4, 88]]}}])")
                                        .doc;
            for (const auto& p : last_line(warped)->points) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) wrong += "\nperspective: a point not finite";
            }
        } catch (const ApplyError& error) {
            wrong += std::string("\nfinite moves → ") + error.what();
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // A size or a corner worked out from finite numbers (1e300 mm, or a page 1e300 mm wide) that no picture can have is
    // refused as too large before it is made a whole number, and the batch leaves the book as it was.
    void nearestLongRotation() {
        genko::core::Patch patch;
        patch.attrs = Json::object({{"box", Json::array({0.0, 0.0, 40000.0*25.4/300, 25.4/300})}});
        patch.png = std::make_shared<const std::string>(genko::render::write_png(
            genko::render::Image::create("L", {40000, 1}, genko::render::Ink(255))));
        const auto rotated = genko::render::selection::transform_patch(patch, {0, -1, 1, 0, 0, 0},
                                                                   genko::render::Resample::Nearest);
        QVERIFY(rotated.has_value());
        const auto image = genko::render::read_png(*rotated->png);
        QCOMPARE(image.width(), 1);
        QCOMPARE(image.height(), 40000);
        QCOMPARE(image.tobytes(), std::string(40000, '\xff'));
    }

    void nearestFixedCoordinatesChecked() {
        genko::core::Patch patch;
        patch.attrs = Json::object({{"box", Json::array({0.0, 0.0, 10.0, 6.667})}});
        patch.png = std::make_shared<const std::string>(genko::render::write_png(
            genko::render::Image::create("L", {2, 2}, genko::render::Ink(255))));
        const genko::render::selection::Matrix fixed{1.0/60000, 0, -1.0/60000, 1, 25.4/(2*300), 0};
        QVERIFY(genko::render::selection::transform_patch(patch, fixed, genko::render::Resample::Nearest).has_value());
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError,
            genko::render::selection::transform_patch(patch, {1e-10, 0, 0, 1, 0, 0}, genko::render::Resample::Nearest));
        QVERIFY(genko::render::selection::transform_patch(patch, {0.5, 0, 0, 0.5, 0, 0},
                    genko::render::Resample::Nearest).has_value());
    }

    void inverseCoordinatesChecked() {
        const Document doc = fixture();
        std::string wrong;
        expectRefused(doc, {{R"({"op":"paste","page":1,"layer_id":"pen-1","interp":"nearest",
            "matrix":[1e-12,0,0,1,0,0],"items":{"patches":[{"box":[0,0,10,6.667],"mode":"mask","png":"GREY"}]}})",
            "ops[1] paste: the inverse transform is too far off the page"}}, wrong);
        QVERIFY2(wrong.empty(), wrong.c_str());
        const auto out = run_ops(doc, R"([{"op":"paste","page":1,"layer_id":"pen-1","interp":"nearest",
            "matrix":[0.5,0,0,1,0,0],"items":{"patches":[{"box":[0,0,10,6.667],"mode":"mask","png":"GREY"}]}}])").doc;
        QVERIFY(layer_by_id(out.page(0), "pen-1")->patches.size() > layer_by_id(doc.page(0), "pen-1")->patches.size());
    }

    void sizesCheckedBeforeWhole() {
        const Document doc = fixture();
        const std::string far_poly = R"({"poly": [[1, 1], [1e300, 5], [3, 9]]})";
        std::string wrong;  // (every case that goes wrong, said at the end)
        expectRefused(doc,
                      {
                          {replaced(R"({"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": AREA})", {{"AREA", far_poly}}),
                           "ops[1] fill_area: area is too large"},
                          {replaced(R"({"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80], "area": AREA})", {{"AREA", far_poly}}),
                           "ops[1] gradient_fill: area is too large"},
                          {R"({"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"mask": {"box": [0, 0, 1e300, 5], "png": "GREY"}}})",
                           "ops[1] fill_area: area is too large"},
                          {R"({"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"mask": {"box": [0, 0, "1e300", 5], "png": "GREY"}}})",
                           "ops[1] fill_area: area is too large"},
                          {R"({"op": "paste", "page": 1, "layer_id": "pen-1", "matrix": [1e308, 0, 0, 1, 0, 0],
                              "items": {"patches": [{"box": [0, 0, 10, 6.667], "mode": "mask", "png": "GREY", "rgb": [200, 10, 10]}]}})",
                           "ops[1] paste: the transform is too large"},
                          {R"({"op": "paste", "page": 1, "layer_id": "pen-1", "matrix": [1, 0, 0, 1, 1, 0],
                              "items": {"patches": [{"box": [0, 0, 1e300, 5], "mode": "mask", "png": "GREY", "rgb": [200, 10, 10]}]}})",
                           "ops[1] paste: a patch is too large"},
                      },
                      wrong);
        // page 2 1e300 mm wide (a book edited by hand)
        Document wide = doc;
        wide.edit_page(1).spec.width_mm = genko::core::Num(1e300);
        const std::string ink2 = wide.page(1).first_layer(LayerRole::Ink)->id;
        expectRefused(wide,
                      {
                          {replaced(R"({"op": "set_layer_mask", "page": 2, "id": "INK2", "fill": "hide"})", {{"INK2", ink2}}),
                           "ops[1] set_layer_mask: the page is too large"},
                          {R"({"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80]})", "ops[1] gradient_fill: the page is too large"},
                          {R"({"op": "fill_area", "page": 2, "area": {"all": true}})", "ops[1] fill_area: the page is too large"},
                      },
                      wrong);
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // gradient_fill: where from and to are so far apart or so far off the page that the place of a pixel along the
    // gradient is not a number (or a colour is past what the value of a pixel can be made from), the op is refused and
    // the batch leaves the book as it was, instead of casting NaN to a pixel.
    void gradientNumbersChecked() {
        const Document doc = fixture();
        const std::string apart = "ops[1] gradient_fill: from and to of the gradient are too far apart or off the page";
        std::string wrong;  // (every case that goes wrong, said at the end)
        expectRefused(doc,
                      {
                          {R"({"op": "gradient_fill", "page": 2, "from": [1e308, 0], "to": [-1e308, 0]})", apart},
                          {R"({"op": "gradient_fill", "page": 2, "from": ["1e308", "0"], "to": ["-1e308", "0"], "shape": "linear"})", apart},
                          {R"({"op": "gradient_fill", "page": 2, "from": [1e308, 0], "to": [1e308, 100], "shape": "ellipse", "repeat": "repeat"})",
                           apart},
                          {R"({"op": "gradient_fill", "page": 2, "from": [1e308, 0], "to": [1e308, 100], "shape": "ellipse", "repeat": "mirror"})",
                           apart},
                          {R"({"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80], "rgb_from": [10000000000, 0, 0]})",
                           "ops[1] gradient_fill: the colours of the gradient are too large"},
                      },
                      wrong);
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // An area made of many areas (a union of a thousand, or saved areas that name the next twice over, level after
    // level) is refused before it takes the memory or the time of all its masks, and the batch leaves the book as it
    // was; a union of as many as may be is made.
    void areaPartsBounded() {
        Document doc = fixture();
        doc.edit_page(2).spec.width_mm = genko::core::Num(10);  // (page 3, 10 × 10 mm: 79 × 79 pixels a mask)
        doc.edit_page(2).spec.height_mm = genko::core::Num(10);
        const auto all_of = [](int n) {
            std::string parts;
            for (int i = 0; i < n; ++i) parts += std::string(i > 0 ? ", " : "") + R"({"all": true})";
            return replaced(R"({"op": "fill_area", "page": 3, "area": {"union": [PARTS]}})", {{"PARTS", parts}});
        };
        // saved areas a0 … a29: each the union of the next twice over (2 ** 30 masks in all, 30 deep)
        Json saved = Json::object();
        for (int i = 0; i < 30; ++i) {
            const Json next = Json::object({{"saved", "a" + std::to_string(i + 1)}});
            saved["a" + std::to_string(i)] = Json::object({{"union", Json::array({next, next})}});
        }
        saved["a30"] = Json::object({{"all", true}});
        doc.edit_page(2).extra["saved_areas"] = saved;
        const std::string many = "ops[1] fill_area: the area is made of too many areas (at most 1000)";
        std::string wrong;  // (every case that goes wrong, said at the end)
        expectRefused(doc, {{all_of(1000), many}, {R"({"op": "fill_area", "page": 3, "area": {"saved": "a0"}})", many}}, wrong, 20);
        try {
            const Document filled = in_time([doc, all_of] { return run_ops(doc, "[" + all_of(999) + "]").doc; }, 60);
            if (filled.page(2).first_layer(LayerRole::Ink)->patches.size() != 1) wrong += "\n999 areas: not filled";
        } catch (const ApplyError& error) {
            wrong += std::string("\n999 areas → ") + error.what();
        }
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // The pixels of all the masks an area makes (each part, each step of a union) are counted: a union of hundreds of
    // page-sized masks is refused (and only a few masks are kept at once on the way, not all of them).
    void areaLiveBuffersBounded() {
        Document doc = fixture();
        doc.edit_page(2).spec.width_mm = genko::core::Num(1000);
        doc.edit_page(2).spec.height_mm = genko::core::Num(1000);
        Json area = Json::object({{"all", true}});
        for (int i = 0; i < 6; ++i) area = Json::object({{"union", Json::array({Json::object({{"all", true}}), area})}});
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError,
            genko::render::selection::to_mask(area, doc.page(2), &doc, 200));
    }

    void areaWorkBounded() {
        Document doc = fixture();
        doc.edit_page(2).spec.width_mm = genko::core::Num(100);  // (page 3, 100 × 100 mm: 787 × 787 pixels a mask)
        doc.edit_page(2).spec.height_mm = genko::core::Num(100);
        std::string parts;
        for (int i = 0; i < 990; ++i) parts += std::string(i > 0 ? ", " : "") + R"({"all": true})";
        std::string wrong;  // (every case that goes wrong, said at the end)
        expectRefused(doc,
                      {{replaced(R"({"op": "fill_area", "page": 3, "area": {"union": [PARTS]}})", {{"PARTS", parts}}),
                        "ops[1] fill_area: the area takes too much work"}},
                      wrong, 300);
        QVERIFY2(wrong.empty(), wrong.c_str());
    }

    // Ordinary inputs give what they gave before these limits (the pixels, patches and lines of their page; the wave
    // and the twirl also with numbers that send pixels off the picture, which come out transparent as in Python).
    void ordinaryResultsKept_data() {
        QTest::addColumn<QByteArray>("batch");
        QTest::addColumn<int>("page");
        QTest::addColumn<QByteArray>("expected");
        const auto row = [](const char* name, const char* batch, int page, const char* expected) {
            QTest::newRow(name) << QByteArray(batch) << page << QByteArray(expected);
        };
        row("fill_area poly", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"poly": [[10, 10], [30, 12], [20, 30]]}}])", 1, "557709699/241");
        row("fill_area mask", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"mask": {"box": [20, 20, 10, 6.667], "png": "GREY"}}}])", 1, "2971949079/241");
        row("gradient linear", R"([{"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80]}])", 2, "1015609120/59");
        row("gradient radial repeat", R"([{"op": "gradient_fill", "page": 2, "from": [35, 47], "to": [45, 47], "shape": "radial", "repeat": "repeat",
            "rgb_from": [200, 0, 0], "rgb_to": [0, 0, 200]}])", 2, "3172552318/60");
        row("gradient ellipse mirror", R"([{"op": "gradient_fill", "page": 2, "from": ["20", "30"], "to": [50, 60], "shape": "ellipse", "ratio": 0.3,
            "repeat": "mirror", "rgb_to": [250, 250, 0], "opacity_from": 0.5}])", 2, "481867199/60");
        row("gradient stops in area", R"([{"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [50, 50], "area": {"rect": [10, 10, 40, 40]},
            "stops": [[0, [255, 0, 0], 1], [0.5, [0, 255, 0], 0.5], [1, [0, 0, 255], 0]]}])", 2, "413724862/64");
        row("gradient radial far", R"([{"op": "gradient_fill", "page": 2, "from": [1e308, 0], "to": [-1e308, 0], "shape": "radial", "rgb_from": [9, 99, 199]}])", 2, "1857264733/59");
        row("gradient radial far repeat", R"([{"op": "gradient_fill", "page": 2, "from": [-1e308, 0], "to": [1e308, 0], "shape": "radial", "repeat": "repeat",
            "area": {"rect": [10, 10, 5, 5]}}])", 2, "3358178169/58");
        row("gradient colour past 255", R"([{"op": "gradient_fill", "page": 2, "from": [10, 10], "to": [60, 80], "rgb_from": [300, -20, 0], "rgb_to": [0, 600, 0]}])", 2, "3450780615/60");
        row("paste moved", R"([{"op": "paste", "page": 1, "layer_id": "pen-1", "matrix": [1.5, 0.2, -0.1, 1.2, 3, 4], "items": {"strokes": [{"points": [[12, 12], [30, 12]]}],
            "patches": [{"box": [20, 20, 10, 6.667], "mode": "mask", "png": "GREY", "rgb": [200, 10, 10]}]}}])", 1, "814087220/298");
        row("transform_area", R"([{"op": "transform_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [38, 48, 14, 14]}, "matrix": [1, 0.3, 0, 1, 2, -1]}])", 1, "2009147138/185");
        row("warp perspective", R"([{"op": "transform_area", "page": 1, "layer_id": "pen-1", "area": {"rect": [5, 60, 50, 30]},
            "warp": {"perspective": [[5, 60], [57, 62], [54, 92], [4, 88]]}}])", 1, "1216672128/1287");
        row("warp mesh", R"([{"op": "transform_area", "page": 1, "layer_id": "paint-1", "area": {"rect": [38, 48, 14, 14]},
            "warp": {"mesh": [[38, 48], [45, 47], [52, 48], [37, 55], [46, 56], [53, 55], [38, 62], [45, 63], [52, 62]]}}])", 1, "663644412/185");
        row("set_layer_mask area", R"([{"op": "set_layer_mask", "page": 1, "id": "pen-1", "area": {"rect": [5, 60, 30, 30]}}])", 1, "2173996834/210");
        row("wave", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": 6, "wavelength": 60}])", 1, "2978599020/157");
        row("wave strings", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": "-3.5", "wavelength": "7"}])", 1, "2602572867/156");
        row("twirl", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "twirl", "angle": 90, "radius": 0.45}])", 1, "1418000732/157");
        row("twirl strong", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "twirl", "angle": -720, "radius": 0.8}])", 1, "228589212/157");
        row("wave 1e30", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": 1e30, "wavelength": 60}])", 1, "3724908635/155");
        row("wave string 1e30", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": "1e30", "wavelength": "60"}])", 1, "3724908635/155");
        row("wave 1e39", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": 1e39}])", 1, "3724908635/155");
        row("wave 1e300", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": -1e300, "wavelength": 1e300}])", 1, "3724908635/155");
        row("wave 2^63", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": 9223372036854775808, "wavelength": 4}])", 1, "3724908635/155");
        row("wave -2^63", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": -9223372036854775808, "wavelength": 4}])", 1, "3724908635/155");
        row("twirl 1e30", R"([{"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "twirl", "angle": 1e30, "radius": "1e30"}])", 1, "3273359843/157");
        row("union", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"union": [{"rect": [10, 10, 20, 20]}, {"ellipse": [25, 25, 20, 15]}]}}])", 1, "1176746437/242");
        row("intersect", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"intersect": [{"rect": [10, 10, 30, 30]}, {"ellipse": [20, 20, 30, 30]}]}}])", 1, "1897818824/243");
        row("subtract", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"subtract": [{"rect": [10, 10, 30, 30]}, {"ellipse": [20, 20, 10, 10]},
            {"rect": [12, 12, 3, 3]}]}}])", 1, "3062809130/242");
        row("saved grown feathered", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"union": [{"saved": "s"}, {"rect": [40, 40, 10, 10], "grow_mm": 1}],
            "feather_mm": 0.5}}])", 1, "3962260918/241");
        row("nested", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"union": [{"intersect": [{"all": true}, {"saved": "s", "invert": true}]},
            {"subtract": [{"union": [{"rect": [5, 5, 10, 10]}, {"rect": [12, 12, 10, 10]}]}, {"ellipse": [8, 8, 6, 6]}]}], "grow_mm": -0.5}}])", 1, "2828267568/239");
        row("one part", R"([{"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"subtract": [{"rect": [10, 10, 30, 30]}]}}])", 1, "3473190623/242");
    }

    void ordinaryResultsKept() {
        QFETCH(QByteArray, batch);
        QFETCH(int, page);
        QFETCH(QByteArray, expected);
        const Document doc = fixture();
        std::string got;
        try {
            got = look(in_time([doc, batch] { return run_ops(doc, batch.toStdString()).doc; }, 120).page(static_cast<std::size_t>(page - 1)));
        } catch (const ApplyError& error) {
            got = std::string("error: ") + error.what();
        }
        QCOMPARE(got, expected.toStdString());
    }
};

QTEST_GUILESS_MAIN(TestRasterOps)
#include "test_raster_ops.moc"
