// smart-objects@1 (PC3 A0, SPEC.md「スマートオブジェクト」, schema-v4.md「スマートオブジェクト（smart-objects@1）」)
// through the command line, as a person or an agent uses it: layers converted, saved, opened again, undone and redone,
// gc keeping the contents' files that Redo needs, the journal's Undo stack and its states holding the contents; the
// feature named only while a smart object is there, and a reader that does not know it opening the book read-only; a
// 16-bit PNG placed on a 16-bit page drawn with its 16 bits; import_pages bringing the contents; a picture's smart object
// moved, cut and orphaned with its panel as place_asset's picture is; the checks counting it as a picture; rasterize_layer
// keeping the mask, and a tone layer's tone kept as grey screened as it printed. (This build's own contract.)

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/checks.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace storage = genko::storage;

namespace {

std::filesystem::path path_of(const QString& dir) { return genko::core::path_from_utf8(dir.toStdString()); }

// A picture of two colours: a grey (128) on the left, a green on the right.
std::string two_colours(int w, int h) {
    render::Image picture = render::Image::create("RGBA", render::Size{w, h}, render::Ink{128, 128, 128, 255});
    picture.paste(render::Ink{30, 160, 90, 255}, render::Box{w / 2, 0, w, h});
    return render::write_png(picture);
}

std::string one_colour(int w, int h, int r, int g, int b) {
    return render::write_png(render::Image::create("RGB", render::Size{w, h}, render::Ink{r, g, b}));
}

const Json* layer_of(const Json& page, const std::string& id) {
    for (const Json& layer : page["layers"]) {
        if (layer.value("id", std::string()) == id) return &layer;
    }
    return nullptr;
}

bool has_feature(const Json& saved, const char* feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

// The panels of a page (its frames' leaves), in order.
void leaves(const Json& frame, std::vector<std::string>& out) {
    if (!frame.contains("children") || frame["children"].empty()) {
        out.push_back(frame["id"].get<std::string>());
        return;
    }
    for (const Json& child : frame["children"]) leaves(child, out);
}
std::vector<std::string> panels(const Json& page) {
    std::vector<std::string> out;
    for (const Json& frame : page["frames"]) leaves(frame, out);
    return out;
}

// How many pixels of a box of an RGB picture are dark (under 128).
double dark_share(const render::Image& image, const render::Box& box) {
    const std::string grey = image.crop(box).convert("L").tobytes();
    const auto dark = std::count_if(grey.begin(), grey.end(), [](char v) { return static_cast<unsigned char>(v) < 128; });
    return double(dark) / double(grey.size());
}

int most_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.convert("RGB").tobytes(), y = b.convert("RGB").tobytes();
    if (x.size() != y.size()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i]))));
    return most;
}

}  // namespace

class TestContractSmart : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString make_book(const QString& name, const QStringList& options = {}) {
        const QString book = path(name);
        const auto made = genko::test::run_genko(QStringList{QStringLiteral("new"), book} + options);
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        return book;
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    bool restore(const QString& book, const char* what) {
        int exit = -1;
        const Json reply = genko({QString::fromLatin1(what), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) qWarning("%s", reply.dump().c_str());
        return exit == 0;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    QString asset(const QString& book, const std::string& ref, const std::string& suffix) {
        return book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") + QString::fromStdString(ref.substr(7) + suffix);
    }
    render::Image drawn(const QString& book, int page, int dpi, const char* mode = "proof") {
        const QString out = path(QStringLiteral("drawn-%1.png").arg(drawings_++));
        const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QString::number(page), QStringLiteral("--dpi"),
                                                 QString::number(dpi), QStringLiteral("--mode"), QString::fromLatin1(mode), QStringLiteral("--out"), out});
        if (ran.exit_code != 0) {
            qWarning("%s", qPrintable(ran.described()));
            return render::Image::create("RGB", render::Size{1, 1});
        }
        return render::open_image(genko::test::read_bytes(out), render::kPillowOpenLimits).convert("RGB");
    }
    Json pixel(const QString& book, int page, double x, double y, int dpi) {
        return genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("%1:%2,%3").arg(page).arg(x).arg(y), QStringLiteral("--dpi"),
                      QString::number(dpi)});
    }
    int drawings_ = 0;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void convertedSavedUndoneAndRedone() {
        // (a colour book: the picture's own colours on the page, without a monochrome book's finish)
        const QString book = make_book(QStringLiteral("book.genko"), {QStringLiteral("--webtoon"), QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY(apply(book, QStringLiteral("paint.json"),
                      Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                   Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(two_colours(60, 40))}}}))
                    .value("ok", false));
        const render::Image before = drawn(book, 1, 40);
        QVERIFY(!has_feature(project(book), "smart-objects@1"));

        // converted: one layer's smart object, its contents a page holding the layer; the page drawn as before
        const Json converted = apply(book, QStringLiteral("convert.json"),
                                     Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}}));
        QVERIFY2(converted.value("ok", false), converted.dump().c_str());
        Json saved = project(book);
        QVERIFY(has_feature(saved, "smart-objects@1"));
        const Json* so = layer_of(saved["pages"][0], "so");
        QVERIFY(so != nullptr && layer_of(saved["pages"][0], "pic") == nullptr);
        QCOMPARE((*so)["kind"], Json("smart"));
        const std::string page_contents = (*so)["smart"]["contents"].get<std::string>();
        QVERIFY(page_contents.starts_with("sc_"));
        QCOMPARE(saved["smart_contents"][page_contents]["kind"], Json("page"));
        QVERIFY(layer_of(saved["smart_contents"][page_contents]["page"], "pic") != nullptr);
        QCOMPARE(drawn(book, 1, 40).tobytes(), before.tobytes());
        // (genko inspect: its contents and settings)
        const Json inspected = genko({QStringLiteral("inspect"), book});
        Json brief;
        for (const Json& layer : inspected["pages"][0]["layers"])
            if (layer.value("id", std::string()) == "so") brief = layer["smart"];
        QCOMPARE(brief["contents"], Json(page_contents));
        QCOMPARE(brief["kind"], Json("page"));
        QCOMPARE(brief["layers"], Json(1));
        QCOMPARE(brief["matrix"], Json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0}));
        QCOMPARE(brief["stack"], Json("none"));

        // a file placed: its own bytes are the book's asset, drawn on the page
        const std::string file = one_colour(120, 80, 200, 40, 90);
        const Json placed = apply(book, QStringLiteral("place.json"),
                                  Json::array({Json{{"op", "smart_place"}, {"page", 2}, {"data", genko::core::b64encode(file)}, {"name", "photo.png"},
                                                    {"new_id", "photo"}, {"scale", 1000}}}));
        QVERIFY2(placed.value("ok", false), placed.dump().c_str());
        saved = project(book);
        const Json* photo = layer_of(saved["pages"][1], "photo");
        QVERIFY(photo != nullptr);
        const std::string file_contents = (*photo)["smart"]["contents"].get<std::string>();
        const Json& contents = saved["smart_contents"][file_contents];
        QCOMPARE(contents["kind"], Json("file"));
        QCOMPARE(contents["suffix"], Json(".png"));
        const std::string ref = contents["asset"].get<std::string>();
        QCOMPARE(genko::test::read_bytes(asset(book, ref, ".png")), file);
        const double cx = saved["spec"]["width_mm"].get<double>() / 2, cy = saved["spec"]["height_mm"].get<double>() / 2;
        QCOMPARE(pixel(book, 2, cx, cy, 40)["srgb"], Json::array({200, 40, 90}));
        const Json after_place = saved;

        // undone: the file's contents leave the book, gc keeps the file (Redo needs it), redone: drawn again
        QVERIFY(restore(book, "undo"));
        saved = project(book);
        QVERIFY(layer_of(saved["pages"][1], "photo") == nullptr);
        QVERIFY(!saved["smart_contents"].contains(file_contents));
        QVERIFY(has_feature(saved, "smart-objects@1"));
        int exit = -1;
        (void)genko({QStringLiteral("gc"), book}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY2(QFile::exists(asset(book, ref, ".png")), "the file kept: Redo still needs it");
        QVERIFY(restore(book, "redo"));
        QCOMPARE(pixel(book, 2, cx, cy, 40)["srgb"], Json::array({200, 40, 90}));

        // both undone: no smart object, no feature, no contents; gc; both redone: the same book as before
        QVERIFY(restore(book, "undo"));
        QVERIFY(restore(book, "undo"));
        saved = project(book);
        QVERIFY(!has_feature(saved, "smart-objects@1"));
        QVERIFY(!saved.contains("smart_contents"));
        QVERIFY(layer_of(saved["pages"][0], "pic") != nullptr);
        (void)genko({QStringLiteral("gc"), book}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY(QFile::exists(asset(book, ref, ".png")));
        QVERIFY(restore(book, "redo"));
        QVERIFY(restore(book, "redo"));
        saved = project(book);
        for (const char* key : {"pages", "smart_contents", "features"}) {
            const Json differs = Json::diff(after_place[key], saved[key]);
            QVERIFY2(differs.empty(), (std::string(key) + ": " + differs.dump().substr(0, 2000)).c_str());
        }
        QCOMPARE(drawn(book, 1, 40).tobytes(), before.tobytes());
        QCOMPARE(genko::test::book_problems(book), std::string());

        // the journal: the Undo stack replayed from it, the state of the last change holding both contents
        const storage::journal::Stacks stacks = storage::journal::stacks(path_of(book));
        QVERIFY(stacks.problems.empty());
        QVERIFY(stacks.undo.size() >= 3);
        QVERIFY(stacks.redo.empty());
        QVERIFY(stacks.undo.back().after.has_value());
        const auto state = storage::AssetStore(path_of(book)).get_bytes(*stacks.undo.back().after, storage::kStateSuffix);
        QVERIFY(state.has_value());
        const Json held = Json::parse(*state);
        QVERIFY2(held.dump().find(file_contents) != std::string::npos && held.dump().find(page_contents) != std::string::npos, held.dump().substr(0, 400).c_str());

        // opened again: both contents read, the book writable; doctor finds nothing wrong
        const storage::LoadResult loaded = storage::load_document(path_of(book));
        QCOMPARE(loaded.document.smart_contents.size(), std::size_t(2));
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        const Json doctor = genko({QStringLiteral("doctor"), book}, &exit);
        QVERIFY2(exit == 0, doctor.dump().c_str());

        // a reader that does not know smart-objects@1 opens the book read-only (the rule of features: here a later
        // version of it, which this build does not know, stands for it)
        Json later = project(book);
        for (Json& feature : later["features"])
            if (feature == Json("smart-objects@1")) feature = "smart-objects@2";
        QCOMPARE(later["min_reader"], Json(4));
        const storage::LoadResult older = storage::load_document_text(later.dump(), path_of(book));
        QVERIFY2(older.document.read_only_reason.find("smart-objects@2") != std::string::npos, older.document.read_only_reason.c_str());
        // (a key of a smart object this build does not know: read-only too, never dropped by a save)
        Json unknown = project(book);
        for (Json& layer : unknown["pages"][0]["layers"])
            if (layer.value("id", std::string()) == "so") layer["smart"]["later"] = 1;
        QVERIFY(!storage::load_document_text(unknown.dump(), path_of(book)).document.read_only_reason.empty());
    }

    void aSixteenBitPngKeepsItsBits() {
        const QString book = make_book(QStringLiteral("deep.genko"), {QStringLiteral("--webtoon"), QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY(apply(book, QStringLiteral("mode.json"), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).value("ok", false));
        // (1000 of 65535: between two 8-bit steps, 3/255 = 771 and 4/255 = 1028)
        const std::string png = render::write_png(render::Image::create("I;16", render::Size{4, 4}, render::Ink(1000)));
        const Json spec = project(book)["spec"];
        const double w = spec["width_mm"].get<double>(), h = spec["height_mm"].get<double>();
        const Json placed = apply(book, QStringLiteral("deep.json"),
                                  Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(png)}, {"name", "deep.png"},
                                                    {"placement_mm", Json::array({0, 0, w, h})}, {"clip_to", "none"}}}));
        QVERIFY2(placed.value("ok", false), placed.dump().c_str());
        const Json saved = project(book);
        QCOMPARE(saved["smart_contents"].begin().value()["depth"], Json(16));
        const Json value = pixel(book, 1, w / 2, h / 2, 40);
        QCOMPARE(value["depth"], Json(16));
        for (const Json& stored : value["stored"]) QVERIFY2(std::abs(stored.get<std::int64_t>() - 1000) <= 2, value.dump().c_str());
    }

    void importPagesBringTheContents() {
        const QString from = make_book(QStringLiteral("from.genko"), {QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY(apply(from, QStringLiteral("from.json"),
                      Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(two_colours(60, 40))}},
                                   Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}},
                                   Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"matrix", Json::array({0.5, 0, 0, 0.5, 20, 30})}},
                                   Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(one_colour(90, 60, 10, 120, 220))}, {"name", "a.png"},
                                        {"new_id", "file"}, {"center_mm", Json::array({150, 200})}, {"scale", 1000}}}))
                    .value("ok", false));
        const QString to = make_book(QStringLiteral("to.genko"), {QStringLiteral("--pages"), QStringLiteral("1")});
        // (the other book's assets copied first, as the app and the MCP export do)
        for (const QFileInfo& dir : QDir(from + QStringLiteral("/assets")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir().mkpath(to + QStringLiteral("/assets/") + dir.fileName());
            for (const QFileInfo& file : QDir(dir.filePath()).entryInfoList(QDir::Files)) {
                QFile::copy(file.filePath(), to + QStringLiteral("/assets/") + dir.fileName() + QStringLiteral("/") + file.fileName());
            }
        }
        const Json imported = apply(to, QStringLiteral("import.json"), Json::array({Json{{"op", "import_pages"}, {"from", from.toStdString()}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        const Json source = project(from), saved = project(to);
        QVERIFY(has_feature(saved, "smart-objects@1"));
        QCOMPARE(saved["pages"].size(), std::size_t(2));
        QCOMPARE(saved["smart_contents"].size(), std::size_t(2));
        // (the page's layers get ids of their own, in the same order)
        std::vector<const Json*> mine, theirs;
        for (const Json& layer : saved["pages"][1]["layers"])
            if (layer["kind"] == Json("smart")) mine.push_back(&layer);
        for (const Json& layer : source["pages"][0]["layers"])
            if (layer["kind"] == Json("smart")) theirs.push_back(&layer);
        QCOMPARE(mine.size(), std::size_t(2));
        QCOMPARE(theirs.size(), std::size_t(2));
        for (std::size_t i = 0; i < 2; ++i) {
            QCOMPARE((*mine[i])["smart"]["matrix"], (*theirs[i])["smart"]["matrix"]);
            const Json a = saved["smart_contents"][(*mine[i])["smart"]["contents"].get<std::string>()];
            const Json b = source["smart_contents"][(*theirs[i])["smart"]["contents"].get<std::string>()];
            QCOMPARE(a["kind"], b["kind"]);
            if (a["kind"] == Json("page")) {  // (the contents' page: its layers the same pictures)
                QCOMPARE(a["page"]["layers"].size(), b["page"]["layers"].size());
                QCOMPARE(a["page"]["layers"][0]["asset"], b["page"]["layers"][0]["asset"]);
            } else {
                QCOMPARE(a, b);
            }
        }
        for (const auto& [x, y] : {std::pair{40.0, 45.0}, std::pair{150.0, 200.0}, std::pair{25.0, 32.0}}) {
            QCOMPARE(pixel(to, 2, x, y, 40)["srgb"], pixel(from, 1, x, y, 40)["srgb"]);
        }
        QCOMPARE(genko::test::book_problems(to), std::string());
    }

    // A picture's smart object in a panel and place_asset's picture of the same file in the same panel: drawn the same
    // (print), the panel resized, deleted (refused without force, the art orphaned with it).
    void panelsMoveItAsAPlacedPicture() {
        const std::string file = one_colour(300, 200, 90, 150, 60);
        std::vector<QString> books;
        for (const char* how : {"placed", "smart"}) {
            const QString book = make_book(QString::fromLatin1(how) + QStringLiteral(".genko"), {QStringLiteral("--pages"), QStringLiteral("1")});
            QVERIFY(apply(book, QStringLiteral("split.json"),
                          Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"}, {"ratio", 0.5}, {"gutter_mm", 6}}}))
                        .value("ok", false));
            const std::string ref = storage::AssetStore(path_of(book)).put_bytes(file, ".png");
            const std::string panel = panels(project(book)["pages"][0]).front();
            const Json op = std::string(how) == "placed" ? Json{{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"frame_id", panel}}
                                                         : Json{{"op", "smart_place"}, {"page", 1}, {"asset", ref}, {"frame_id", panel}, {"name", "a.png"}};
            QVERIFY(apply(book, QStringLiteral("put.json"), Json::array({op})).value("ok", false));
            books.push_back(book);
        }
        const QString& placed = books[0];
        const QString& smart = books[1];
        const Json smart_layers = project(smart)["pages"][0]["layers"];
        QCOMPARE(std::count_if(smart_layers.begin(), smart_layers.end(), [](const Json& layer) { return layer["kind"] == Json("smart"); }), 1L);
        QCOMPARE(drawn(smart, 1, 60, "print").tobytes(), drawn(placed, 1, 60, "print").tobytes());

        for (const QString& book : books) {
            const std::string panel = panels(project(book)["pages"][0]).front();
            QVERIFY(apply(book, QStringLiteral("resize.json"),
                          Json::array({Json{{"op", "resize_frame"}, {"page", 1}, {"frame_id", panel}, {"rect", Json{{"x", 30}, {"y", 40}, {"width", 100}, {"height", 70}}}}}))
                        .value("ok", false));
        }
        QCOMPARE(drawn(smart, 1, 60, "print").tobytes(), drawn(placed, 1, 60, "print").tobytes());

        std::vector<Json> refused;
        for (const QString& book : books) {
            const std::string panel = panels(project(book)["pages"][0]).front();
            const QString file_name = path(QStringLiteral("delete.json"));
            genko::test::write_bytes(file_name, Json::array({Json{{"op", "delete_frame"}, {"page", 1}, {"frame_id", panel}}}).dump());
            int exit = -1;
            refused.push_back(genko({QStringLiteral("apply"), book, file_name, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit));
            QVERIFY(exit != 0);
            QVERIFY(apply(book, QStringLiteral("delete.json"), Json::array({Json{{"op", "delete_frame"}, {"page", 1}, {"frame_id", panel}, {"force", true}}}))
                        .value("ok", false));
        }
        QCOMPARE(refused[0].value("code", Json()), refused[1].value("code", Json()));
        QCOMPARE(drawn(smart, 1, 60, "print").tobytes(), drawn(placed, 1, 60, "print").tobytes());
        const Json orphans = project(smart)["studio"]["orphans"];
        QCOMPARE(orphans.size(), std::size_t(1));
        QCOMPARE(orphans[0]["layers"].size(), std::size_t(1));
        QCOMPARE(orphans[0]["layers"][0]["kind"], Json("smart"));
        QCOMPARE(orphans[0]["smart_contents"].begin().value()["kind"], Json("file"));
        QCOMPARE(project(placed)["studio"]["orphans"][0]["layers"].size(), std::size_t(1));
        QVERIFY(!has_feature(project(smart), "smart-objects@1"));  // (no smart object left on a page)
        int exit = -1;
        (void)genko({QStringLiteral("gc"), smart}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY2(QFile::exists(asset(smart, storage::AssetStore::ref(file), ".png")), "the orphaned art keeps its file");
    }

    // The checks: a picture's smart object is art (a page with it and a draft is not "art on layers that do not print"),
    // its resolution where it lands is counted as a placed picture's, and its missing file is said.
    void checksCountItAsAPicture() {
        const QString book = make_book(QStringLiteral("checks.genko"), {QStringLiteral("--pages"), QStringLiteral("3")});
        const std::string draft = genko::core::b64encode(two_colours(60, 40));
        const std::string file = one_colour(120, 80, 60, 60, 60);
        const std::string ref = storage::AssetStore(path_of(book)).put_bytes(file, ".png");
        QVERIFY(apply(book, QStringLiteral("draw.json"),
                      Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"layer", "draft"}, {"png_base64", draft}},
                                   Json{{"op", "put_raster"}, {"page", 2}, {"layer", "draft"}, {"png_base64", draft}},
                                   Json{{"op", "smart_place"}, {"page", 1}, {"asset", ref}, {"name", "a.png"}, {"frame_id", panels(project(book)["pages"][0]).front()}},
                                   Json{{"op", "place_asset"}, {"page", 3}, {"asset", ref}, {"frame_id", panels(project(book)["pages"][2]).front()}}}))
                    .value("ok", false));
        const storage::LoadResult loaded = storage::load_document(path_of(book));
        const auto codes = [&](int page) {
            std::vector<std::string> out;
            for (const Json& issue : genko::formats::checks::page_issues(loaded.document, loaded.document.page(std::size_t(page - 1))))
                out.push_back(issue["code"].get<std::string>());
            return out;
        };
        const auto has = [](const std::vector<std::string>& list, const char* code) { return std::find(list.begin(), list.end(), code) != list.end(); };
        QVERIFY(!has(codes(1), "art_not_printed"));
        QVERIFY(has(codes(2), "art_not_printed"));
        QVERIFY(!has(codes(3), "art_not_printed"));

        genko::formats::checks::StudioCheck options;
        Json checked = genko::formats::checks::studio_check(loaded.document, path_of(book), options);
        QCOMPARE(checked["dpi"].size(), std::size_t(2));
        QCOMPARE(checked["dpi"][0]["dpi"], checked["dpi"][1]["dpi"]);  // (the same file in the same panel: the same resolution)
        int low = 0;
        for (const Json& error : checked["errors"]) low += error["code"] == Json("low_dpi") ? 1 : 0;
        QCOMPARE(low, 2);
        options.force = true;
        checked = genko::formats::checks::studio_check(loaded.document, path_of(book), options);
        int warned = 0;
        for (const Json& warning : checked["warnings"]) warned += warning["code"] == Json("low_dpi") ? 1 : 0;
        QCOMPARE(warned, 2);
        QVERIFY(QFile::remove(asset(book, ref, ".png")));
        checked = genko::formats::checks::studio_check(loaded.document, path_of(book), options);
        int missing = 0;
        for (const Json& error : checked["errors"]) missing += error["code"] == Json("asset_missing") ? 1 : 0;
        QCOMPARE(missing, 2);
    }

    // rasterize_layer: a smart object becomes the paint layer it looks like, its mask kept; a tone layer the grey of its
    // tone screened as it printed; all: fills and pictures (a picture on a monochrome page as its finish shows it, screened
    // as it printed), lines kept.
    void rasterizeKeepsTheMaskAndTheTone() {
        const QString book = make_book(QStringLiteral("raster.genko"), {QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY(apply(book, QStringLiteral("so.json"),
                      Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                   Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(two_colours(60, 40))}},
                                   Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}},
                                   Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "so"},
                                        {"area", Json{{"poly", Json::array({Json::array({10, 10}), Json::array({150, 30}), Json::array({60, 250})})}}}}}))
                    .value("ok", false));
        const Json mask = (*layer_of(project(book)["pages"][0], "so"))["mask"];
        const render::Image before = drawn(book, 1, 50);
        const Json done = apply(book, QStringLiteral("raster.json"), Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "so"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book);
        const Json* layer = layer_of(saved["pages"][0], "so");
        QVERIFY(layer != nullptr);
        QCOMPARE((*layer)["kind"], Json("raster"));
        QVERIFY(!layer->contains("smart"));
        QCOMPARE((*layer)["mask"], mask);
        QVERIFY(!has_feature(saved, "smart-objects@1") && !saved.contains("smart_contents"));
        QVERIFY2(most_apart(drawn(book, 1, 50), before) <= 2, "rasterized as it looked");

        // a tone: grey where it is, screened as it printed
        QVERIFY(apply(book, QStringLiteral("tone.json"),
                      Json::array({Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.3}, {"lpi", 60}, {"angle", 45},
                                        {"area", Json{{"poly", Json::array({Json::array({40, 120}), Json::array({160, 120}), Json::array({160, 240}), Json::array({40, 240})})}}}}}))
                    .value("ok", false));
        const render::Image proof = drawn(book, 1, 50);
        const render::Image print = drawn(book, 1, 300, "print");
        const Json rasterized = apply(book, QStringLiteral("tone2.json"), Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "tn"}}}));
        QVERIFY2(rasterized.value("ok", false), rasterized.dump().c_str());
        saved = project(book);
        layer = layer_of(saved["pages"][0], "tn");
        QVERIFY(layer != nullptr);
        QCOMPARE((*layer)["kind"], Json("raster"));
        QCOMPARE((*layer)["role"], Json("user"));
        QVERIFY(!layer->contains("tone"));
        QCOMPARE((*layer)["screen"]["pattern"], Json("dot"));
        QCOMPARE((*layer)["screen"]["lpi"], Json(60.0));
        QCOMPARE((*layer)["screen"]["angle"], Json(45.0));
        QCOMPARE((*layer)["screen"]["black"], Json(0.0));
        QCOMPARE((*layer)["screen"]["white"], Json(1.0));
        const render::Box at50{static_cast<int>(60 / 25.4 * 50), static_cast<int>(140 / 25.4 * 50), static_cast<int>(140 / 25.4 * 50),
                               static_cast<int>(220 / 25.4 * 50)};
        QVERIFY2(most_apart(drawn(book, 1, 50).crop(at50), proof.crop(at50)) <= 3, "the grey of the tone");
        const render::Box at300{static_cast<int>(60 / 25.4 * 300), static_cast<int>(140 / 25.4 * 300), static_cast<int>(140 / 25.4 * 300),
                                static_cast<int>(220 / 25.4 * 300)};
        const double was = dark_share(print, at300), now = dark_share(drawn(book, 1, 300, "print"), at300);
        QVERIFY2(std::abs(was - 0.3) < 0.05 && std::abs(now - was) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(was).arg(now)));

        // all: a fill layer and a placed picture become paint layers as they looked; a pen layer's lines stay lines
        const std::string ref = storage::AssetStore(path_of(book)).put_bytes(one_colour(40, 30, 20, 90, 200), ".png");
        QVERIFY(apply(book, QStringLiteral("all.json"),
                      Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "fl"}, {"rgb", Json::array({250, 200, 40})}},
                                   Json{{"op", "set_layer"}, {"page", 1}, {"id", "fl"}, {"opacity", 0.4}},
                                   Json{{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"placement_mm", Json::array({120, 30, 60, 45})}},
                                   Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                                   Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"},
                                        {"points", Json::array({Json::array({20, 30}), Json::array({80, 60})})}, {"width_mm", 1.0}}}))
                    .value("ok", false));
        const render::Image all_before = drawn(book, 1, 50);
        const Json all = apply(book, QStringLiteral("all2.json"), Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"all", true}}}));
        QVERIFY2(all.value("ok", false), all.dump().c_str());
        saved = project(book);
        QCOMPARE((*layer_of(saved["pages"][0], "fl"))["kind"], Json("raster"));
        QCOMPARE((*layer_of(saved["pages"][0], "fl"))["opacity"], Json(0.4));
        QCOMPARE((*layer_of(saved["pages"][0], "pen"))["kind"], Json("strokes"));
        int screened = 0;  // (the tone, and the picture on this monochrome page: screened as they printed)
        for (const Json& each : saved["pages"][0]["layers"]) {
            QVERIFY2(each["kind"] != Json("placed"), each.dump().c_str());
            screened += each.contains("screen") && each["screen"]["pattern"] == Json("dot") ? 1 : 0;
        }
        QCOMPARE(screened, 2);
        // (inside the picture and over the fill: as they looked; the picture's edge is resampled once more)
        const render::Image all_after = drawn(book, 1, 50);
        const auto box = [](double x0, double y0, double x1, double y1) {
            return render::Box{static_cast<int>(x0 / 25.4 * 50), static_cast<int>(y0 / 25.4 * 50), static_cast<int>(x1 / 25.4 * 50),
                               static_cast<int>(y1 / 25.4 * 50)};
        };
        for (const render::Box& part : {box(125, 35, 175, 70), box(20, 250, 150, 280)})
            QVERIFY2(most_apart(all_after.crop(part), all_before.crop(part)) <= 2, "rasterized as they looked");
    }
};

QTEST_GUILESS_MAIN(TestContractSmart)
#include "test_contract_smart.moc"
