// smart-objects@1 (PC3 A0, SPEC.md「スマートオブジェクト」): the model's rules of smart objects — the matrix and its
// snapping, a book with smart objects written and read back (the contents a layer shows only, the feature named only then,
// a book without them the bytes it had), what validate_smart refuses (contents that show themselves, too deep, a matrix
// that squashes them flat, a key not known, pixels drawn on a smart object), an op sent into contents ("contents": the
// contents' page changes, the page showing it is asked its lock, what cannot be done there refused), and the ops of A0:
// smart_convert, smart_place, smart_copy, smart_transform, smart_profile, rasterize_layer, the later steps refused by name,
// a picture's smart object going with its panel as placed art does.

#include <QtTest>

#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace storage = genko::storage;
namespace fs = std::filesystem;
using core::Actor;
using core::ApplyError;
using core::Document;
using core::Json;
using core::Num;
using core::smart::Matrix;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

Document run(const Document& doc, const Json& ops, const Actor& actor = Actor("human:作者")) { return bus().apply(doc, ops, actor).doc; }
core::ApplyResult applied(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")); }

fs::path path_of(const QString& dir) { return core::path_from_utf8(dir.toStdString()); }

std::string png_of(int w, int h, render::Ink ink) { return render::write_png(render::Image::create("RGB", render::Size{w, h}, ink)); }

// A two-page book with a paint layer "p1" of a red square on page 1.
Document painted() {
    Document doc = core::new_episode("スマート", Num(1), 2, core::PageSpec::a4_mono());
    render::Image picture = render::Image::create("RGBA", render::Size{100, 140}, render::Ink{0, 0, 0, 0});
    picture.paste(render::Image::create("RGBA", render::Size{40, 30}, render::Ink{200, 20, 20, 255}), render::Point{20, 30});
    return run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}, {"name", "赤"}},
                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(render::write_png(picture))}}}));
}

const core::Layer* layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& l : page.layers)
        if (l.id == id) return &l;
    return nullptr;
}

bool refused(const Document& doc, const Json& ops, const QString& words) {
    try {
        (void)applied(doc, ops);
    } catch (const ApplyError& error) {
        if (QString::fromUtf8(error.what()).contains(words)) return true;
        qWarning("%s", error.what());
        return false;
    }
    return false;
}

bool invalid(const Document& doc, const QString& words) {
    try {
        core::smart::validate_smart(doc);
    } catch (const core::Error& error) {
        if (QString::fromUtf8(error.what()).contains(words)) return true;
        qWarning("%s", error.what());
        return false;
    }
    return false;
}

}  // namespace

class TestSmart : public QObject {
    Q_OBJECT

private slots:
    void theMatrixComposesAndSnaps() {
        const Matrix half{0.5, 0, 0, 0.5, 10, 20};
        const Matrix twice = *core::smart::inverse(half);
        QCOMPARE(core::smart::snapped(core::smart::multiply(twice, half)), Matrix{});
        QVERIFY(core::smart::is_identity(core::smart::snapped(core::smart::multiply(twice, half))));
        const double turn = 30 * 3.14159265358979323846 / 180;
        const Matrix turned{std::cos(turn) * 2, std::sin(turn) * 2, -std::sin(turn) * 2, std::cos(turn) * 2, 0, 0};
        QVERIFY(std::abs(core::smart::largest_scale(turned) - 2) < 1e-12);
        QVERIFY(!core::smart::axis_aligned(turned));
        QVERIFY(core::smart::axis_aligned(half));
        QCOMPARE(core::smart::snapped(Matrix{1.0000000000004, 0, 0, 1, 3.0000000000001, 0}), (Matrix{1, 0, 0, 1, 3, 0}));
        QVERIFY_THROWS_EXCEPTION(core::Error, core::smart::matrix_from(Json::array({1, 0, 0, 1, 0})));
        QVERIFY_THROWS_EXCEPTION(core::Error, core::smart::matrix_from(Json::array({1, 0, 0, 1, 0, "x"})));
        QVERIFY(core::smart::is_contents_id(core::smart::new_contents_id()));
        QVERIFY(!core::smart::is_contents_id("sc_XYZ"));
        QCOMPARE(core::smart::suffix_of_name("Photo.JPEG").value_or(""), std::string(".jpg"));
        QCOMPARE(core::smart::suffix_of_name("a.tiff").value_or(""), std::string(".tif"));
        QVERIFY(!core::smart::suffix_of_name("a.svg"));
    }

    void aBookWithSmartObjectsIsWrittenAndReadBack() {
        QTemporaryDir tmp;
        // (a book without them: no key, no feature, the same bytes as before)
        const Document plain = painted();
        genko::test::write_project(plain, path_of(tmp.path() + "/plain"));
        const Json plain_payload = genko::test::read_json(tmp.path() + "/plain/project.json");
        QVERIFY(!plain_payload.contains("smart_contents"));
        QVERIFY(std::find(plain_payload["features"].begin(), plain_payload["features"].end(), Json("smart-objects@1")) == plain_payload["features"].end());
        Document doc = run(plain, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}},
                                                 Json{{"op", "smart_place"}, {"page", 2}, {"data", core::b64encode(png_of(8, 6, render::Ink{10, 200, 30}))},
                                                      {"name", "緑.png"}}}));
        const core::Layer* so = layer_of(doc.page(0), "so1");
        QVERIFY(so != nullptr && so->kind == core::LayerKind::Smart);
        QCOMPARE(doc.smart_contents.size(), std::size_t{2});
        // (contents no layer shows are not written)
        auto orphan = std::make_shared<core::SmartContents>();
        orphan->kind = "page";
        orphan->page = std::make_shared<core::Page>(doc.page(0));
        orphan->page->layers.clear();
        doc.smart_contents["sc_000000000abc"] = orphan;
        genko::test::write_project(doc, path_of(tmp.path() + "/smart"));
        const Json payload = genko::test::read_json(tmp.path() + "/smart/project.json");
        QVERIFY(payload.contains("smart_contents"));
        QCOMPARE(payload["smart_contents"].size(), std::size_t{2});
        QVERIFY(!payload["smart_contents"].contains("sc_000000000abc"));
        QVERIFY(std::find(payload["features"].begin(), payload["features"].end(), Json("smart-objects@1")) != payload["features"].end());
        const auto loaded = storage::load_document(path_of(tmp.path() + "/smart"));
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        QCOMPARE(loaded.document.read_only_reason, std::string());
        const core::Layer* back = layer_of(loaded.document.page(0), "so1");
        QVERIFY(back != nullptr && back->kind == core::LayerKind::Smart && back->smart == so->smart);
        const core::SmartContents* contents = core::smart::contents_of(loaded.document, *back);
        QVERIFY(contents != nullptr && contents->kind == "page" && contents->page->layers.size() == 1);
        QCOMPARE(contents->page->layers[0].id, std::string("p1"));
        QVERIFY(contents->page->layers[0].raster_png && *contents->page->layers[0].raster_png == *layer_of(plain.page(0), "p1")->raster_png);
        // (the file kept as its bytes, under its suffix)
        const core::Layer* picture = nullptr;
        for (const core::Layer& l : loaded.document.page(1).layers)
            if (l.kind == core::LayerKind::Smart) picture = &l;
        QVERIFY(picture != nullptr);
        const core::SmartContents* file = core::smart::contents_of(loaded.document, *picture);
        QVERIFY(file != nullptr && file->kind == "file" && file->suffix == ".png" && file->format == "png" && file->width_px == 8 && file->height_px == 6);
        QCOMPARE(storage::AssetStore(path_of(tmp.path() + "/smart")).get_bytes(file->asset, ".png").value_or(""), png_of(8, 6, render::Ink{10, 200, 30}));
        // (written again: the same bytes)
        genko::test::write_project(loaded.document, path_of(tmp.path() + "/again"));
        QCOMPARE(genko::test::read_bytes(tmp.path() + "/again/project.json"), genko::test::read_bytes(tmp.path() + "/smart/project.json"));
        // (a missing file: reported, read-only)
        fs::remove(storage::AssetStore(path_of(tmp.path() + "/smart")).path(file->asset, ".png"));
        const auto missing = storage::load_document(path_of(tmp.path() + "/smart"));
        QVERIFY(!missing.document.read_only_reason.empty());
    }

    void validateRefusesWhatCannotBe() {
        const Document doc = run(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        core::smart::validate_smart(doc);
        const std::string id = *core::smart::contents_id(*layer_of(doc.page(0), "so1"));
        {  // (contents showing themselves)
            Document bad = doc;
            core::Layer self = *layer_of(doc.page(0), "so1");
            self.id = "self";
            bad.edit_contents_page(id).layers.push_back(self);
            QVERIFY(invalid(bad, "show themselves"));
        }
        {  // (a ring through two)
            Document bad = doc;
            auto other = std::make_shared<core::SmartContents>(*bad.smart_contents.at(id));
            other->page = std::make_shared<core::Page>(*other->page);
            core::Layer to_first = *layer_of(doc.page(0), "so1");
            to_first.id = "back";
            other->page->layers = {to_first};
            bad.smart_contents["sc_0000000000ff"] = other;
            core::Layer to_other = to_first;
            to_other.id = "there";
            (*to_other.smart)["contents"] = "sc_0000000000ff";
            bad.edit_contents_page(id).layers.push_back(to_other);
            QVERIFY(invalid(bad, "show themselves"));
        }
        {  // (deeper than 8)
            Document bad = doc;
            std::string inner = id;
            for (int i = 0; i < 9; ++i) {
                const std::string outer = "sc_00000000001" + std::to_string(i);
                auto c = std::make_shared<core::SmartContents>(*bad.smart_contents.at(id));
                c->page = std::make_shared<core::Page>(*c->page);
                core::Layer shows = *layer_of(doc.page(0), "so1");
                (*shows.smart)["contents"] = inner;
                c->page->layers = {shows};
                bad.smart_contents[outer] = c;
                inner = outer;
            }
            QVERIFY(invalid(bad, "deeper than 8"));
        }
        {  // (squashed flat, a key not known, an interpolation not known)
            Document bad = doc;
            for (core::Layer& l : bad.edit_page(0).layers) {
                if (l.id == "so1") (*l.smart)["matrix"] = Json::array({1, 0, 1, 0, 0, 0});
            }
            QVERIFY(invalid(bad, "squashes it flat"));
            Document unknown = doc;
            for (core::Layer& l : unknown.edit_page(0).layers) {
                if (l.id == "so1") (*l.smart)["shine"] = true;
            }
            QVERIFY(invalid(unknown, "not known to this build"));
            Document interp = doc;
            for (core::Layer& l : interp.edit_page(0).layers) {
                if (l.id == "so1") (*l.smart)["interp"] = "cubic";
            }
            QVERIFY(invalid(interp, "interp is"));
        }
        // (pixels drawn on a smart object: the op is refused — rasterize it first)
        QVERIFY(refused(doc, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", "so1"}, {"png_base64", core::b64encode(png_of(4, 4, render::Ink{1, 2, 3}))}}}),
                        "is a smart object"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", "so1"}}}), "cannot be merged"));
    }

    void anOpGoesIntoTheContents() {
        const Document doc = run(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        const std::string id = *core::smart::contents_id(*layer_of(doc.page(0), "so1"));
        const auto page_before = doc.pages[0];
        const auto result = applied(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "inside"}, {"name", "中"}, {"contents", id}}}));
        const Document& after = result.doc;
        QVERIFY(layer_of(*after.smart_contents.at(id)->page, "inside") != nullptr);
        QVERIFY(layer_of(after.page(0), "inside") == nullptr);
        QVERIFY(after.pages[0] != page_before);  // (the page showing them is another page now: drawn again)
        QCOMPARE(result.journal_ops[0]["contents"], Json(id));
        for (const Json& warning : result.warnings) QVERIFY2(warning.get<std::string>().find("unknown keys") == std::string::npos, warning.dump().c_str());
        // (what cannot be done inside: lines, panels, pages; contents that are not there; a file's)
        QVERIFY(refused(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "x"}, {"contents", id}}}), "inside smart object contents"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"}, {"contents", id}}}),
                        "inside smart object contents"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "add_page"}, {"contents", id}}}), "inside smart object contents"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"contents", "sc_00000000dead"}}}), "no smart object contents"));
        // (the page showing them locked by someone else: refused)
        Document locked = doc;
        locked.page_locks[locked.page(0).id] = "ai:helper";
        QVERIFY(refused(locked, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"contents", id}}}), "locked by ai:helper"));
        // (an error inside said of the op as sent)
        QVERIFY(refused(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "nope"}, {"title", "x"}, {"contents", id}}}), "ops[0] set_layer:"));
    }

    void convertingKeepsTheLayersAndTheirLook() {
        Document doc = painted();
        doc = run(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "p1"}, {"opacity", 0.5}, {"blend", "multiply"}},
                                      Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "p2"}, {"name", "線"}},
                                      Json{{"op", "add_line"}, {"page", 1}, {"text", "下"}, {"id", "line1"}, {"x_mm", 30}, {"y_mm", 30},
                                           {"style", Json{{"below_layer", "p1"}}}}}));
        const std::size_t before = doc.page(0).layers.size();
        const std::size_t at = [&] {
            for (std::size_t i = 0; i < doc.page(0).layers.size(); ++i)
                if (doc.page(0).layers[i].id == "p1") return i;
            return std::size_t{0};
        }();
        const auto result = applied(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        const Document& after = result.doc;
        QCOMPARE(after.page(0).layers.size(), before);
        QCOMPARE(after.page(0).layers[at].id, std::string("so1"));
        const core::Layer& so = after.page(0).layers[at];
        QCOMPARE(so.opacity, 0.5);
        QCOMPARE(so.blend, std::string("multiply"));
        const core::SmartContents& contents = *core::smart::contents_of(after, so);
        const core::Layer& inner = contents.page->layers.front();
        QCOMPARE(inner.opacity, 1.0);
        QCOMPARE(inner.blend, std::string("normal"));
        QCOMPARE(result.results[0]["id"], Json("so1"));
        // (the line set under the layer is set under the smart object)
        for (const core::StoryLine& line : after.story)
            if (line.id == "line1") QCOMPARE(line.style["below_layer"], Json("so1"));
        // (one layer: the smart object draws as the layer did — the stand-in)
        const auto stand_in = core::smart::with_plain_layers(after.page(0), &after);
        QVERIFY(stand_in.has_value());
        const core::Layer* as_was = layer_of(*stand_in, "so1");
        QVERIFY(as_was != nullptr && as_was->kind == core::LayerKind::Raster && as_was->opacity == 0.5 && as_was->blend == "multiply");
        // (several layers: one smart object of them; nothing drawn: refused)
        const Document two = run(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1", "p2"})}, {"new_id", "so2"}}}));
        QCOMPARE(core::smart::contents_of(two, *layer_of(two.page(0), "so2"))->page->layers.size(), std::size_t{2});
        QVERIFY(!core::smart::with_plain_layers(two.page(0), &two));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p2"}}}), "nothing to draw"));
    }

    void copyTransformProfileAndRasterize() {
        Document doc = run(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        // (a copy: its own contents)
        Document copied = run(doc, Json::array({Json{{"op", "smart_copy"}, {"page", 1}, {"id", "so1"}, {"new_id", "so2"}}}));
        QVERIFY(core::smart::contents_id(*layer_of(copied.page(0), "so2")) != core::smart::contents_id(*layer_of(copied.page(0), "so1")));
        QCOMPARE(layer_of(copied.page(0), "so2")->title, layer_of(copied.page(0), "so1")->title + " のコピー");
        // (a transform: made smaller and back, the matrix it had)
        Document moved = run(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so1"}, {"matrix", Json::array({0.5, 0, 0, 0.5, 7, 9})}}}));
        QCOMPARE(core::smart::matrix_of(*layer_of(moved.page(0), "so1")), (Matrix{0.5, 0, 0, 0.5, 7, 9}));
        moved = run(moved, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so1"}, {"matrix", Json::array({2, 0, 0, 2, -14, -18})}}}));
        QVERIFY(core::smart::is_identity(core::smart::matrix_of(*layer_of(moved.page(0), "so1"))));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so1"}, {"set_matrix", Json::array({1, 0, 1, 0, 0, 0})}}}),
                        "squashes"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", doc.page(0).layers[0].id}, {"matrix", Json::array({1, 0, 0, 1, 0, 0})}}}),
                        "is not a smart object"));
        // (rasterize: the layer's pixels, its mask kept)
        Document masked = run(doc, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "so1"},
                                                       {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({100, 0}), Json::array({100, 100})})}}}}}));
        QVERIFY(layer_of(masked.page(0), "so1")->mask.has_value());
        const Document flat = run(masked, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "so1"}}}));
        const core::Layer* raster = layer_of(flat.page(0), "so1");
        QVERIFY(raster != nullptr && raster->kind == core::LayerKind::Raster && raster->raster_png && raster->mask.has_value() && !raster->smart);
        QVERIFY(refused(flat, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "so1"}}}), "nothing to rasterize"));
        // (smart_profile: a page's contents have none)
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_profile"}, {"page", 1}, {"id", "so1"}, {"embedded_profile", "keep"}}}), "a page, not a file"));
    }

    void laterStepsAreRefusedByName() {
        const Document doc = run(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        // (A2's smart_filter_* are written: test_smart_filters)
        for (const char* op : {"smart_replace", "smart_relink", "smart_update", "smart_embed", "smart_link", "smart_to_page", "smart_stack", "rasterize_lines"}) {
            try {
                (void)applied(doc, Json::array({Json{{"op", op}, {"page", 1}, {"id", "so1"}}}));
                QFAIL(op);
            } catch (const ApplyError& error) {
                QCOMPARE(error.code(), std::string("not_yet_ported"));
                QVERIFY2(QString::fromUtf8(error.what()).contains(QString::fromLatin1(op)), error.what());
            }
        }
    }

    void aPicturesSmartObjectGoesWithItsPanel() {
        Document doc = core::new_episode("コマ", Num(1), 1, core::PageSpec::a4_mono());
        doc = run(doc, Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"}}}));
        const std::string panel = doc.page(0).leaf_frames()[0]->id;
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png_of(8, 6, render::Ink{1, 2, 3}))}, {"name", "a.png"},
                                           {"frame_id", panel}, {"id", "unused"}}}));
        const core::Layer* placed = nullptr;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.kind == core::LayerKind::Smart) placed = &l;
        QVERIFY(placed != nullptr && placed->frame_id == panel && placed->clip_to == "frame");
        // (covering the panel as place_asset would: the matrix maps the picture onto the panel's box widened to its shape)
        const core::Frame& frame = *doc.page(0).find_frame(panel);
        const core::Rect fitted = core::fit_rect(frame.rect, 8, 6, "cover", 0, 0, 1.0);
        const Matrix m = core::smart::matrix_of(*placed);
        const auto [w_mm, h_mm] = core::smart::size_mm(*core::smart::contents_of(doc, *placed));
        QVERIFY(std::abs(m.e - fitted.x.value()) < 1e-9 && std::abs(m.f - fitted.y.value()) < 1e-9);
        QVERIFY(std::abs(m.a * w_mm - fitted.width.value()) < 1e-6 && std::abs(m.d * h_mm - fitted.height.value()) < 1e-6);
        // (the panel deleted: refused without force, then kept with its file in studio.orphans)
        QVERIFY(refused(doc, Json::array({Json{{"op", "delete_frame"}, {"page", 1}, {"frame_id", panel}}}), "has placed art"));
        const Document gone = run(doc, Json::array({Json{{"op", "delete_frame"}, {"page", 1}, {"frame_id", panel}, {"force", true}}}));
        for (const core::Layer& l : gone.page(0).layers) QVERIFY(l.kind != core::LayerKind::Smart);
        const Json& orphan = gone.studio["orphans"].back();
        QCOMPARE(orphan["layers"][0]["kind"], Json("smart"));
        QVERIFY(orphan.contains("smart_contents"));
        // (its finish set with its panel's)
        const Document finished = run(doc, Json::array({Json{{"op", "set_finish"}, {"page", 1}, {"frame_id", panel}, {"finish", Json{{"tone", "flat"}}}}}));
        for (const core::Layer& l : finished.page(0).layers)
            if (l.kind == core::LayerKind::Smart) QCOMPARE(*l.finish, (Json{{"tone", "flat"}}));
    }
};

QTEST_MAIN(TestSmart)
#include "test_smart.moc"
