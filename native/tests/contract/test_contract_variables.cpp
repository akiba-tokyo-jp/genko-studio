// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」) as a person or an agent uses it: a picture put in a paint layer by
// apply_data_set on the 8-bit, 16-bit, 32-bit and CMYK pages (its kind and depth kept; PhotoCraft's replace_pixels: fit,
// fill, conform and as_is, the nine places, clip), a smart object's contents replaced (its place kept); through the command
// line — the variables defined and given with `genko apply`, the book naming variables@1 and keeping the pictures in assets/,
// `genko inspect --variables`, apply_data_set undone by `genko undo`, `genko export --format datasets` (a file a data set,
// their names, the book unchanged, never into the book) —; a book without variables read and written as before (the same
// bytes); a .pcraft's variables written and read back, a picture named by a path beside it.

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/smart.hpp"
#include "core/variables.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/image.hpp"
#include "render/inspect.hpp"
#include "render/layer_bounds.hpp"
#include "render/ops_registry.hpp"
#include "render/pcraft_variables.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& ops) { return run_ops(doc, ops.is_array() ? ops : Json::array({ops})).doc; }

std::string png_of(int w, int h, std::array<int, 4> rgba) {
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        for (int c = 0; c < 4; ++c) bytes[i + std::size_t(c)] = static_cast<char>(rgba[std::size_t(c)]);
    }
    return render::write_png(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
}

// A page 40 × 40 mm at 100 dpi without panels — "8" (8 bits), "16", "32" (precise RGB) or "cmyk" (inks) — holding a paint
// layer "photo" of a grey block over 10..30 mm (its own values: precise on a precise page, inks on a CMYK one).
core::Document book(const std::string& way = "8") {
    core::PageSpec spec;
    spec.width_mm = core::Num(40);
    spec.height_mm = core::Num(40);
    spec.dpi = core::Num(100);
    spec.expression = "color";
    core::Document doc = core::new_episode("データ", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    if (way == "16" || way == "32") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", way == "16" ? 16 : 32}});
    if (way == "cmyk") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "photo"}, {"name", "photo"}},
                                Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "photo"}, {"blend", way == "8" ? "normal" : "multiply"},
                                     {"area", Json{{"poly", Json::array({Json::array({10, 10}), Json::array({30, 10}), Json::array({30, 30}), Json::array({10, 30})})}}},
                                     {"rgb", Json::array({128, 128, 128})}}}));
    return doc;
}

const core::Layer& layer(const core::Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return l;
    throw std::runtime_error("no layer " + id);
}

// The picture variable "img" on `id` (method, align, clip) and data set "blue" of an 8 × 4 blue picture, applied.
core::Document replaced(const core::Document& doc, const std::string& method, const std::string& align = "center", bool clip = false,
                        const std::string& id = "photo") {
    core::Document out = run(doc, Json{{"op", "set_variables"},
                                       {"defs", Json::array({Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", id}, {"method", method},
                                                                  {"align", align}, {"clip", clip}}})}});
    out = run(out, Json{{"op", "set_data_sets"},
                        {"data_sets", Json::array({Json{{"name", "blue"}, {"values", Json::array({Json{{"variable", "img"}, {"value", Json{{"data", core::b64encode(png_of(8, 4, {0, 0, 255, 255}))}, {"name", "blue.png"}}}}})}}})}});
    return run(out, Json{{"op", "apply_data_set"}, {"name", "blue"}});
}

// The layer's own pixel at a point (mm): its alpha, and its colour as the screen shows it.
double alpha_at(const core::Document& doc, double x, double y, const std::string& id = "photo") {
    const Json p = render::pixel_at(doc.page(0), &doc, x, y, id, 100);
    return p["alpha"].get<double>();
}
Json srgb_at(const core::Document& doc, double x, double y, const std::string& id = "photo") {
    return render::pixel_at(doc.page(0), &doc, x, y, id, 100)["srgb"];
}

}  // namespace

class TestContractVariables : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool names_variables(const Json& project) {
        const Json features = project.value("features", Json::array());
        return std::find(features.begin(), features.end(), Json("variables@1")) != features.end();
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // A picture put in a paint layer on every kind of page: its kind and depth kept, the picture where the def's box says, the
    // layer's old pixels gone.
    void pixelsOnEveryKind() {
        for (const std::string way : {"8", "16", "32", "cmyk"}) {
            const core::Document doc = replaced(book(way), "conform");
            const core::Layer& l = layer(doc, "photo");
            if (way == "8") QVERIFY(l.raster_png && !l.color_raster && !l.ink_raster);
            if (way == "16" || way == "32") QVERIFY2(l.color_raster != nullptr, way.c_str());
            if (way == "cmyk") QVERIFY2(l.ink_raster != nullptr, way.c_str());
            QVERIFY2(alpha_at(doc, 20, 20) > 0.99, way.c_str());
            const Json c = srgb_at(doc, 20, 20);
            QVERIFY2(c[2].get<double>() > c[0].get<double>() + 60 && c[2].get<double>() > c[1].get<double>() + 40, (way + ": " + c.dump()).c_str());
            QVERIFY2(alpha_at(doc, 5, 5) < 0.01, way.c_str());     // (outside the box: nothing)
            QVERIFY2(alpha_at(doc, 29, 29) > 0.99, way.c_str());   // (conform: the whole box)
            QCOMPARE(core::variables_of(doc).active, std::optional<std::int64_t>(0));
        }
    }

    // PhotoCraft's methods and places: an 8 × 4 picture (2.032 × 1.016 mm at 100 dpi) in the 20 mm box 10..30.
    void methodsAndPlaces() {
        const core::Document base = book();
        // fit, center: 20 × 10 mm, from y 15 to 25
        core::Document d = replaced(base, "fit");
        QVERIFY(alpha_at(d, 20, 13) < 0.01);
        QVERIFY(alpha_at(d, 20, 20) > 0.99);
        QVERIFY(alpha_at(d, 11, 16) > 0.99);
        QVERIFY(alpha_at(d, 20, 27) < 0.01);
        // fit, top_center: from y 10 to 20
        d = replaced(base, "fit", "top_center");
        QVERIFY(alpha_at(d, 20, 11) > 0.99);
        QVERIFY(alpha_at(d, 20, 25) < 0.01);
        // fit, bottom_right: from y 20 to 30
        d = replaced(base, "fit", "bottom_right");
        QVERIFY(alpha_at(d, 20, 15) < 0.01);
        QVERIFY(alpha_at(d, 29, 29) > 0.99);
        // fill, center: 40 × 20 mm — past the box unless clipped
        d = replaced(base, "fill");
        QVERIFY(alpha_at(d, 2, 20) > 0.99);
        QVERIFY(alpha_at(d, 20, 5) < 0.01);
        d = replaced(base, "fill", "center", true);
        QVERIFY(alpha_at(d, 2, 20) < 0.01);
        QVERIFY(alpha_at(d, 11, 20) > 0.99);
        // as_is, top_left: its own size at the page's resolution, at the box's corner
        d = replaced(base, "as_is", "top_left");
        QVERIFY(alpha_at(d, 10.5, 10.5) > 0.99);
        QVERIFY(alpha_at(d, 11.8, 10.7) > 0.99);
        QVERIFY(alpha_at(d, 13, 13) < 0.01);
        // the def's box kept when the picture is put in again (defined again with the same layer: not the picture's box)
        const core::Document again = run(d, Json{{"op", "set_variables"},
                                                 {"defs", Json::array({Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}, {"method", "conform"}}})}});
        const auto box = core::variables_of(again).defs[0].box_mm;
        QVERIFY2(std::abs(box[0] - 10) < 0.2 && std::abs(box[2] - 30) < 0.2, core::variables_to_json(core::variables_of(again)).dump().c_str());
        const core::Document conformed = run(again, Json{{"op", "apply_data_set"}, {"index", 0}});
        QVERIFY(alpha_at(conformed, 29, 29) > 0.99);
        // an empty paint layer: the paper is the box
        core::Document empty = run(base, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "blank"}});
        empty = replaced(empty, "conform", "center", false, "blank");
        QVERIFY(alpha_at(empty, 1, 1, "blank") > 0.99);
        QVERIFY(alpha_at(empty, 39, 39, "blank") > 0.99);
    }

    // A smart object's contents replaced (smart_replace: its place kept).
    void smartObject() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png_of(8, 4, {255, 0, 0, 255}))}, {"name", "red.png"}, {"new_id", "so"}});
        const auto before = render::layer_bounds::of_layer(doc, doc.page(0), "so");
        doc = replaced(doc, "fit", "center", false, "so");
        const core::SmartContents* contents = core::smart::contents_of(doc, layer(doc, "so"));
        QVERIFY(contents != nullptr);
        QCOMPARE(contents->asset, core::bytes_ref(png_of(8, 4, {0, 0, 255, 255})));
        QCOMPARE(render::layer_bounds::of_layer(doc, doc.page(0), "so"), before);
    }

    // Through the command line: defined and given (a picture's bytes), saved with variables@1 and the picture in assets/,
    // listed, applied and undone; the data sets as files; a book without variables as before.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        core::Document plain = book();
        plain = run(plain, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "badge"}, {"name", "badge"}});
        genko::test::write_project(plain, fs::path(book_path.toStdString()));
        const std::string before = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        QVERIFY(!names_variables(project(book_path)));
        QVERIFY(!project(book_path).contains("variables"));
        // (a book without variables read and written again: the same bytes)
        {
            const QString again = path(QStringLiteral("again.genko"));
            genko::test::write_project(storage::load_document(fs::path(book_path.toStdString())).document, fs::path(again.toStdString()));
            QCOMPARE(genko::test::read_bytes(again + QStringLiteral("/project.json")), before);
        }
        const std::string blue = png_of(8, 4, {0, 0, 255, 255});
        Json done = apply(book_path, QStringLiteral("a.json"),
                          Json::array({Json{{"op", "set_variables"},
                                            {"defs", Json::array({Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}, {"method", "conform"}},
                                                                  Json{{"name", "show"}, {"type", "visibility"}, {"page", 1}, {"layer", "badge"}}})}},
                                       Json{{"op", "set_data_sets"},
                                            {"data_sets", Json::array({Json{{"name", "Blue/1"}, {"values", Json::array({Json{{"variable", "img"}, {"value", Json{{"data", core::b64encode(blue)}}}},
                                                                                                                        Json{{"variable", "show"}, {"value", false}}})}},
                                                                       Json{{"name", "Shown"}, {"values", Json::array({Json{{"variable", "show"}, {"value", true}}})}}})}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book_path);
        QVERIFY2(names_variables(saved), saved.value("features", Json::array()).dump().c_str());
        QCOMPARE(saved["variables"]["data_sets"].size(), std::size_t(2));
        const std::string ref = core::bytes_ref(blue);
        QVERIFY(storage::AssetStore(fs::path(book_path.toStdString())).has(ref, ".png"));
        QVERIFY(genko::test::book_problems(book_path).empty());
        // inspect --variables
        int exit = -1;
        const Json listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--variables")}, &exit);
        QVERIFY2(exit == 0, listed.dump().c_str());
        QCOMPARE(listed["defs"].size(), std::size_t(2));
        QCOMPARE(listed["defs"][0]["target"], Json("photo"));
        QCOMPARE(listed["data_sets"][0]["name"], Json("Blue/1"));
        QCOMPARE(listed["active"], Json());
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--variables"), QStringLiteral("--comps")}, &exit);
        QCOMPARE(exit, 2);
        // export --format datasets: a file a data set; their names; the book unchanged
        const std::string kept = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        const QString out = path(QStringLiteral("sets-out"));
        Json written = genko({QStringLiteral("export"), book_path, out, QStringLiteral("--format"), QStringLiteral("datasets"), QStringLiteral("--json")}, &exit);
        QVERIFY2(exit == 0, written.dump().c_str());
        QCOMPARE(written["count"], Json(2));
        QVERIFY2(written["files"][0].get<std::string>().ends_with("/Blue_1.png"), written.dump().c_str());
        QVERIFY2(written["files"][1].get<std::string>().ends_with("/Shown.png"), written.dump().c_str());
        QCOMPARE(genko::test::read_bytes(book_path + QStringLiteral("/project.json")), kept);
        {
            const render::Image first = render::read_png(genko::test::read_bytes(QString::fromStdString(written["files"][0].get<std::string>()))).convert("RGB");
            const render::Image second = render::read_png(genko::test::read_bytes(QString::fromStdString(written["files"][1].get<std::string>()))).convert("RGB");
            const int cx = first.width() / 2, cy = first.height() / 2;
            QVERIFY2(first.getpixel(cx, cy)[2] > 200 && first.getpixel(cx, cy)[0] < 60, "the blue picture");
            QVERIFY2(second.getpixel(cx, cy)[0] == second.getpixel(cx, cy)[2], "the grey block as it was");
        }
        written = genko({QStringLiteral("export"), book_path, path(QStringLiteral("named")), QStringLiteral("--format"), QStringLiteral("datasets"),
                         QStringLiteral("--naming"), QStringLiteral("{document}-{index}"), QStringLiteral("--datasets"), QStringLiteral("Shown"),
                         QStringLiteral("--dataset-format"), QStringLiteral("jpg"), QStringLiteral("--json")},
                        &exit);
        QVERIFY2(exit == 0, written.dump().c_str());
        QCOMPARE(written["count"], Json(1));
        QVERIFY2(written["files"][0].get<std::string>().ends_with("/データ-002.jpg"), written.dump().c_str());
        genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--format"), QStringLiteral("datasets"), QStringLiteral("--datasets"),
               QStringLiteral("nope")},
              &exit);
        QCOMPARE(exit, 1);
        genko({QStringLiteral("export"), book_path, book_path + QStringLiteral("/inside"), QStringLiteral("--format"), QStringLiteral("datasets")}, &exit);
        QCOMPARE(exit, 2);  // (never into the book)
        genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--format"), QStringLiteral("png"), QStringLiteral("--naming"),
               QStringLiteral("a")},
              &exit);
        QCOMPARE(exit, 2);
        // applied, then undone
        done = apply(book_path, QStringLiteral("b.json"), Json::array({Json{{"op", "apply_data_set"}, {"name", "Blue/1"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        core::Document now = storage::load_document(fs::path(book_path.toStdString())).document;
        QVERIFY(!layer(now, "badge").visible);
        QVERIFY(alpha_at(now, 29, 29) > 0.99);
        QCOMPARE(core::variables_of(now).active, std::optional<std::int64_t>(0));
        const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        now = storage::load_document(fs::path(book_path.toStdString())).document;
        QVERIFY(layer(now, "badge").visible);
        QVERIFY(alpha_at(now, 5, 5) < 0.01 && alpha_at(now, 20, 20) > 0.99);
        QCOMPARE(core::variables_of(now).active, std::optional<std::int64_t>());
        // a book naming variables@1 whose picture is gone: read-only
        const QString broken = path(QStringLiteral("broken.genko"));
        genko::test::copy_tree(book_path, broken);
        fs::remove(storage::AssetStore(fs::path(broken.toStdString())).path(ref, ".png"));
        QVERIFY(!storage::load_document(fs::path(broken.toStdString())).document.read_only_reason.empty());
    }

    // A .pcraft: the page's variables written (a picture value left out, said) and read back; a picture named by a path beside
    // it read as the book's, one outside the folder or not there left out (said).
    void pcraft() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "badge"}, {"name", "badge"}});
        doc = run(doc, Json{{"op", "set_variables"},
                            {"defs", Json::array({Json{{"name", "show"}, {"type", "visibility"}, {"page", 1}, {"layer", "badge"}},
                                                  Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}, {"method", "fill"}, {"align", "top_left"}}})}});
        doc = run(doc, Json{{"op", "set_data_sets"},
                            {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "show"}, {"value", false}},
                                                                                                    Json{{"variable", "img"}, {"value", Json{{"data", core::b64encode(png_of(2, 2, {0, 0, 255, 255}))}}}}})}}})}});
        const QString file = path(QStringLiteral("vars.pcraft"));
        const formats::CopyResult written = formats::write_copy(doc, 0, fs::path(file.toStdString()));
        QVERIFY2(std::any_of(written.notes.begin(), written.notes.end(), [](const std::string& n) { return n.find("データセットの画像") != std::string::npos; }),
                 Json(written.notes).dump().c_str());
        formats::OpenOptions options;
        options.folder = fs::path(tmp_.path().toStdString());
        const formats::Opened opened = formats::open_file(genko::test::read_bytes(file), "vars.pcraft", options);
        const core::Variables v = core::variables_of(opened.doc);
        QCOMPARE(v.defs.size(), std::size_t(2));
        QCOMPARE(v.defs[1].method, std::string("fill"));
        QCOMPARE(v.defs[1].align, std::string("top_left"));
        QCOMPARE(v.data_sets.size(), std::size_t(1));
        QCOMPARE(v.data_sets[0].values.size(), std::size_t(1));
        const core::Document shown = run(opened.doc, Json{{"op", "apply_data_set"}, {"name", "A"}});
        for (const core::Layer& l : shown.page(0).layers) {
            if (l.title == "badge") QVERIFY(!l.visible);
        }
        // read: upstream's paths beside the .pcraft
        genko::test::write_bytes(path(QStringLiteral("red.png")), png_of(2, 2, {255, 0, 0, 255}));
        core::Document target = book();
        render::pcraft_production::ReadContext context;
        context.page = &target.edit_page(0);
        context.book = &target;
        context.beside = fs::path(tmp_.path().toStdString());
        context.layers = {{"7", "photo"}};
        std::vector<std::string> said;
        context.say = [&said](std::string note) { said.push_back(std::move(note)); };
        const Json upstream{{"defs", Json::array({Json{{"name", "img"}, {"layer", 7}, {"kind", Json{{"type", "pixelReplacement"}, {"method", "asIs"}, {"align", "bottomRight"}, {"clip", true}}}}})},
                            {"data_sets", Json::array({Json{{"name", "near"}, {"values", Json::array({Json{{"variable", "img"}, {"kind", "pixels"}, {"value", "red.png"}}})}},
                                                       Json{{"name", "far"}, {"values", Json::array({Json{{"variable", "img"}, {"kind", "pixels"}, {"value", "../outside.png"}}})}},
                                                       Json{{"name", "gone"}, {"values", Json::array({Json{{"variable", "img"}, {"kind", "pixels"}, {"value", "/tmp/none.png"}}})}}})},
                            {"active", nullptr}};
        render::pcraft_variables::read(upstream, context);
        const core::Variables read = core::variables_of(target);
        QCOMPARE(read.defs.size(), std::size_t(1));
        QCOMPARE(read.defs[0].method, std::string("as_is"));
        QCOMPARE(read.defs[0].align, std::string("bottom_right"));
        QVERIFY(read.defs[0].clip);
        QCOMPARE(read.data_sets.size(), std::size_t(3));
        QCOMPARE(read.data_sets[0].values.size(), std::size_t(1));
        QCOMPARE(std::get<core::PictureValue>(read.data_sets[0].values[0].value).asset, core::bytes_ref(png_of(2, 2, {255, 0, 0, 255})));
        QVERIFY(read.data_sets[1].values.empty() && read.data_sets[2].values.empty());
        QVERIFY2(std::any_of(said.begin(), said.end(), [](const std::string& n) { return n.find("2 個") != std::string::npos; }), Json(said).dump().c_str());
        const core::Document applied = run(target, Json{{"op", "apply_data_set"}, {"name", "near"}});
        QVERIFY(alpha_at(applied, 29.8, 29.8) > 0.99);
        QVERIFY(alpha_at(applied, 20, 20) < 0.01);
    }
};

QTEST_GUILESS_MAIN(TestContractVariables)
#include "test_contract_variables.moc"
