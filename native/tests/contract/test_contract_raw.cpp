// PC4 F-e (SPEC.md FILE-01「カメラ RAW」): a camera RAW file opened as a book (formats/open_file: LibRaw's development
// into a page of its output space and depth, its profile the page's, its camera in native_source; or a smart object of
// the RAW file), a RAW placed as a smart object (smart_place) and developed again (set_camera_raw: its size, depth and
// picture; Undo from the command line), camera-raw@1 written and read back (one not well formed read-only), a RAW of a
// camera LibRaw does not know opened as its embedded preview, `genko new --from x.dng --raw`, and the limits. The RAW files
// are PhotoCraft's synthetic ones (tests/support/raw_testgen), never a camera's. (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/raw_develop.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "raw_testgen.hpp"
#include "render/codecs/codecs.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raw.hpp"
#include "render/smart.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace formats = genko::formats;
namespace codecs = genko::render::codecs;
namespace storage = genko::storage;
namespace rawgen = genko::test::rawgen;
namespace fs = std::filesystem;
using core::Json;

namespace {

constexpr std::array<double, 9> kSrgbCamera{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};

double srgb_encode(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }

// A flat neutral grey of `level` (linear) under the camera's white (0.5, 1, 0.7): a 96 × 64 DNG of 12 bits.
rawgen::DngSpec grey_dng(double level, std::uint16_t orientation = 1) {
    const std::size_t w = 96, h = 64;
    std::vector<std::array<float, 3>> rgb(w * h);
    const std::array<double, 3> neutral{0.5, 1.0, 0.7};
    for (auto& p : rgb)
        for (int c = 0; c < 3; ++c) p[std::size_t(c)] = static_cast<float>(level * neutral[std::size_t(c)]);
    rawgen::DngSpec d = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(rgb, w, {0, 1, 1, 2}, 64, 4000));
    d.bits = 12;
    d.white = 4000;
    d.black = {64};
    d.as_shot_neutral = neutral;
    d.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
    d.orientation = orientation;
    d.make = "Photocraft";
    d.model = "Synthetic";
    return d;
}

std::string jpeg_of(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 3> rgb) {
    std::string data(std::size_t(w) * h * 3, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(rgb[i % 3]);
    return codecs::encode(codecs::CodecImage::from_bytes(w, h, codecs::Layout::Rgb, codecs::Sample::U8, data), codecs::Format::Jpeg);
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

Json project(const fs::path& dir) { return Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")))); }

bool has_feature(const Json& saved, std::string_view feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    spec.expression = "color";  // (a colour page: a picture drawn as it is, not with a monochrome finish)
    return core::new_episode("raw", core::Num(1), 1, spec, core::Binding::Right);
}

core::Document run(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc; }

// The page drawn (proof) and its pixel at (x, y) of the page's own grid.
std::array<int, 3> drawn_at(const core::Document& doc, int x, int y) {
    const core::Page& page = doc.page(0);
    const int dpi = static_cast<int>(page.spec.dpi.value());
    const render::Image image = render::render_page(page, dpi, render::proof_options(), &doc).image.convert("RGB");
    const std::string bytes = image.tobytes();
    const std::size_t at = (std::size_t(y) * std::size_t(image.width()) + std::size_t(x)) * 3;
    return {static_cast<unsigned char>(bytes[at]), static_cast<unsigned char>(bytes[at + 1]), static_cast<unsigned char>(bytes[at + 2])};
}

// The first page's layers, said (a failure's words).
std::string layers_said(const core::Document& doc) {
    std::string out = "expression " + doc.page(0).spec.expression + ";";
    for (const core::Layer& l : doc.page(0).layers) {
        out += " [" + l.title + " kind " + std::to_string(static_cast<int>(l.kind)) + " visible " + std::to_string(l.visible) + " opacity " +
               std::to_string(l.opacity) + (l.smart ? " smart " + l.smart->dump() : std::string()) + "]";
    }
    return out;
}

std::string contents_id_of(const core::Document& doc) {
    for (const auto& [id, contents] : doc.smart_contents)
        if (contents && contents->format == "raw") return id;
    return {};
}

}  // namespace

class TestContractRaw : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const char* name) const { return fs::path(tmp_.path().toStdString()) / name; }
    QString qat(const char* name) const { return QString::fromStdString(core::path_to_utf8(at(name))); }

private slots:
    // Developed with camera-raw@1's defaults (ProPhoto, 16 bits): a page in precise colour of 16 bits in prophoto-compat, its
    // pixels the development's, its paper the picture at 300 ppi, the camera in native_source, said in its notes.
    void aRawOpensDeveloped() {
        const std::string bytes = grey_dng(0.18).build();
        const formats::Opened opened = formats::open_file(bytes, "folder/IMG_0001.dng");
        const core::Document& doc = opened.doc;
        QCOMPARE(doc.title, std::string("IMG_0001"));
        QVERIFY(!doc.asset_dir);
        const core::Page& page = doc.page(0);
        QVERIFY(page.rgb && !page.color);
        QCOMPARE(page.rgb->depth, 16);
        QCOMPARE(page.rgb->profile, core::builtin_profile("prophoto-compat")->ref());
        QCOMPARE(page.spec.dpi.value(), 300.0);
        QCOMPARE(render::mm_to_px(page.spec.width_mm.value(), 300), 96);
        QCOMPARE(render::mm_to_px(page.spec.height_mm.value(), 300), 64);
        QCOMPARE(page.layers.size(), std::size_t(1));
        const core::Layer& layer = page.layers[0];
        QCOMPARE(layer.title, std::string("背景"));
        QVERIFY(layer.color_raster);
        const core::ColorRasterView view(*layer.color_raster);
        QCOMPARE(view.width(), 96u);
        const auto p = view.pixel(32 * 96 + 48);
        const double want = std::pow(0.18, 1 / 1.8);  // (prophoto-compat's curve)
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(p[std::size_t(c)] - want) < 0.01, std::to_string(p[std::size_t(c)]).c_str());
        QVERIFY(page.native_source);
        QCOMPARE(page.native_source->format, std::string("raw"));
        QCOMPARE(page.native_source->name, std::string("IMG_0001.dng"));
        QCOMPARE(page.native_source->width_px, std::int64_t(96));
        QCOMPARE(page.native_source->depth, 16);
        QVERIFY(page.native_source->camera);
        QCOMPARE(page.native_source->camera->make, std::string("Photocraft"));
        QVERIFY(!opened.notes.empty());
        QVERIFY2(opened.notes[0].find("カメラ RAW（DNG、Photocraft Synthetic）を現像して開きました（ProPhoto RGB・16 bit）") != std::string::npos, opened.notes[0].c_str());
        // (drawn: the grey of 0.18 shown as sRGB shows it)
        const auto shown = drawn_at(doc, 48, 32);
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(shown[std::size_t(c)] - srgb_encode(0.18) * 255) < 3, std::to_string(shown[std::size_t(c)]).c_str());
        // (saved: a book of a picture — no new extension for the RAW itself —, read back the same)
        write_book(doc, at("developed.genko"));
        const Json saved = project(at("developed.genko"));
        QVERIFY(!has_feature(saved, core::kCameraRawFeature));
        const storage::LoadResult read = storage::load_document(at("developed.genko"));
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
    }

    // The output space and depth asked: sRGB 8 bits an 8-bit page of no profile (at its pixels' grid), Adobe RGB 8 bits an
    // 8-bit page of that profile, sRGB 16 bits a precise page of no profile; the resolution asked.
    void spacesDepthsAndResolution() {
        const std::string bytes = grey_dng(0.18).build();
        formats::OpenOptions options;
        options.raw = Json{{"develop", Json{{"space", "srgb"}, {"depth", 8}}}};
        options.raw_dpi = 240;
        formats::Opened opened = formats::open_file(bytes, "a.dng", options);
        const core::Page& srgb8 = opened.doc.page(0);
        QVERIFY(!srgb8.rgb && !srgb8.color);
        QVERIFY(srgb8.layers[0].raster_png);
        QCOMPARE(srgb8.spec.grid, std::optional<std::string>("finest"));
        QCOMPARE(srgb8.spec.dpi.value(), 240.0);
        const render::Image picture = render::open_image(*srgb8.layers[0].raster_png).convert("RGB");
        const std::string px = picture.tobytes();
        QVERIFY(std::abs(static_cast<unsigned char>(px[(32 * 96 + 48) * 3]) - srgb_encode(0.18) * 255) < 2.5);
        QVERIFY(opened.notes[0].find("sRGB・8 bit") != std::string::npos);
        options.raw = Json{{"develop", Json{{"space", "adobe-rgb-compat"}, {"depth", 8}}}};
        opened = formats::open_file(bytes, "a.dng", options);
        QVERIFY(opened.doc.page(0).rgb);
        QCOMPARE(opened.doc.page(0).rgb->depth, 8);
        QCOMPARE(opened.doc.page(0).rgb->profile, core::builtin_profile("adobe-rgb-compat")->ref());
        options.raw = Json{{"develop", Json{{"space", "srgb"}, {"depth", 16}}}};
        opened = formats::open_file(bytes, "a.dng", options);
        QVERIFY(opened.doc.page(0).rgb);
        QCOMPARE(opened.doc.page(0).rgb->depth, 16);
        QVERIFY(opened.doc.page(0).rgb->profile.empty());
        // (a development refused: said as a value)
        options.raw = Json{{"develop", Json{{"space", "lab"}}}};
        try {
            (void)formats::open_file(bytes, "a.dng", options);
            QFAIL("a development not of this build");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("value"));
        }
        // (its orientation: turned as the file says)
        const formats::Opened turned = formats::open_file(grey_dng(0.18, 6).build(), "t.dng");
        QCOMPARE(turned.doc.page(0).native_source->width_px, std::int64_t(64));
        QCOMPARE(turned.doc.page(0).native_source->height_px, std::int64_t(96));
    }

    // オブジェクトを開く: a page of one smart object of the RAW file (camera-raw@1: its development kept), drawn as the
    // development; saved with the feature, read back clean.
    void theRawAsASmartObject() {
        const std::string bytes = grey_dng(0.18).build();
        formats::OpenOptions options;
        options.raw_smart = true;
        options.raw = Json{{"develop", Json{{"space", "srgb"}, {"depth", 16}}}};
        const formats::Opened opened = formats::open_file(bytes, "IMG_0002.DNG", options);
        const core::Document& doc = opened.doc;
        const core::Page& page = doc.page(0);
        QCOMPARE(page.layers.size(), std::size_t(1));
        QCOMPARE(page.layers[0].kind, core::LayerKind::Smart);
        const std::string id = contents_id_of(doc);
        QVERIFY(!id.empty());
        const core::SmartContents& contents = *doc.smart_contents.at(id);
        QCOMPARE(contents.format, std::string("raw"));
        QCOMPARE(contents.suffix, std::string(".dng"));
        QCOMPARE(contents.width_px, std::int64_t(96));
        QCOMPARE(contents.depth, 16);
        QCOMPARE(contents.dpi, 300.0);
        QVERIFY(contents.raw);
        QCOMPARE((*contents.raw)["develop"]["space"], Json("srgb"));
        QCOMPARE((*contents.raw)["camera"]["libraw"], Json(render::raw::libraw_version()));
        QVERIFY(doc.smart_files.contains(contents.asset));
        QVERIFY(opened.notes[0].find("スマートオブジェクトとして開きました") != std::string::npos);
        const auto shown = drawn_at(doc, 48, 32);
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(shown[std::size_t(c)] - srgb_encode(0.18) * 255) < 3, std::to_string(shown[std::size_t(c)]).c_str());
        write_book(doc, at("object.genko"));
        const Json saved = project(at("object.genko"));
        QVERIFY(has_feature(saved, core::kCameraRawFeature));
        QVERIFY(has_feature(saved, core::kSmartObjectsFeature));
        const storage::LoadResult read = storage::load_document(at("object.genko"));
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(*read.document.smart_contents.at(id)->raw == *contents.raw);
        // (drawn from the book's folder: the same)
        const auto again = drawn_at(read.document, 48, 32);
        QCOMPARE(again, shown);
    }

    // A RAW placed as a smart object (camera-raw@1's defaults), developed again by set_camera_raw: brighter, its depth, its
    // size turned off, the report; the RAW's bytes never changed; refusals change nothing.
    void placedAndDevelopedAgain() {
        const std::string bytes = grey_dng(0.18, 6).build();
        core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(bytes)}, {"name", "x.dng"},
                                                                 {"placement_mm", Json::array({0, 0, 60, 80})}, {"clip_to", "none"}}}));
        const std::string id = contents_id_of(doc);
        QVERIFY(!id.empty());
        const core::SmartContents placed = *doc.smart_contents.at(id);
        QCOMPARE(placed.width_px, std::int64_t(64));  // (turned as the file says)
        QCOMPARE(placed.height_px, std::int64_t(96));
        QCOMPARE((*placed.raw)["develop"], core::raw::to_json(core::raw::Develop{}));
        QCOMPARE(placed.profile["use"], Json("embedded"));
        const int cx = render::mm_to_px(30, 150), cy = render::mm_to_px(40, 150);
        const auto before = drawn_at(doc, cx, cy);
        const auto picture = render::smart::file_picture(doc, placed);
        QVERIFY2(picture, "the contents' picture");
        const std::string middle = picture->convert("RGB").tobytes();
        const std::size_t mid = (std::size_t(picture->height() / 2) * std::size_t(picture->width()) + std::size_t(picture->width() / 2)) * 3;
        QVERIFY2(std::abs(static_cast<unsigned char>(middle[mid + 1]) - srgb_encode(0.18) * 255) < 4, std::to_string(static_cast<unsigned char>(middle[mid + 1])).c_str());
        QVERIFY2(std::abs(before[1] - srgb_encode(0.18) * 255) < 4,
                 (std::to_string(before[0]) + " " + std::to_string(before[1]) + " " + std::to_string(before[2]) + " page " + layers_said(doc)).c_str());
        core::CommandBus bus(render::ops_registry());
        auto applied = bus.apply(doc, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"exposure_ev", 1}}}}}), core::Actor("human:test"));
        const Json report = applied.results.at(0);
        QCOMPARE(report["contents"], Json(id));
        QCOMPARE(report["raw"]["develop"]["exposure_ev"], Json(1.0));
        QCOMPARE(report["raw"]["develop"]["space"], Json("prophoto-compat"));  // (the others kept)
        QCOMPARE(report["size_px"], (Json::array({64, 96})));
        const core::Document brighter = applied.doc;
        QCOMPARE(brighter.smart_contents.at(id)->asset, placed.asset);
        const auto after = drawn_at(brighter, cx, cy);
        QVERIFY2(std::abs(after[1] - srgb_encode(0.36) * 255) < 4, std::to_string(after[1]).c_str());
        // (orientation none: its sides as the sensor's; 8 bits)
        const core::Document plain = run(brighter, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"orientation", "none"}, {"depth", 8}}}}}));
        QCOMPARE(plain.smart_contents.at(id)->width_px, std::int64_t(96));
        QCOMPARE(plain.smart_contents.at(id)->height_px, std::int64_t(64));
        QCOMPARE(plain.smart_contents.at(id)->depth, 8);
        QCOMPARE((*plain.smart_contents.at(id)->raw)["develop"]["exposure_ev"], Json(1.0));
        // (adjustments: kept, and null takes them away)
        const core::Document adjusted = run(plain, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"adjust", Json{{"saturation", -100}}}}}));
        QCOMPARE((*adjusted.smart_contents.at(id)->raw)["adjust"]["saturation"], Json(-100.0));
        const core::Document cleared = run(adjusted, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"adjust", nullptr}}}));
        QVERIFY(!cleared.smart_contents.at(id)->raw->contains("adjust"));
        // (refused: nothing changed)
        const core::Document png = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(render::write_png(render::Image::create("RGB", {8, 8}, render::Ink{1, 2, 3})))}, {"name", "p.png"}}}));
        const std::string png_id = png.smart_contents.begin()->first;
        for (const auto& [book, op] : std::vector<std::pair<core::Document, Json>>{
                 {png, Json{{"op", "set_camera_raw"}, {"contents", png_id}}},
                 {doc, Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"space", "lab"}}}}},
                 {doc, Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"what", 1}}}}},
                 {doc, Json{{"op", "set_camera_raw"}, {"contents", id}, {"other", 1}}},
                 {doc, Json{{"op", "set_camera_raw"}, {"contents", id}, {"adjust", Json{{"exposure", "x"}}}}},
                 {doc, Json{{"op", "set_camera_raw"}}}}) {
            try {
                (void)run(book, Json::array({op}));
                QFAIL(op.dump().c_str());
            } catch (const core::ApplyError& error) {
                QVERIFY2(std::string(error.code()) == "apply", error.what());
            }
        }
        try {
            (void)run(doc, Json::array({Json{{"op", "set_camera_raw"}, {"contents", "sc_000000000000"}}}));
            QFAIL("no such contents");
        } catch (const core::ApplyError&) {
        }
    }

    // camera-raw@1 as a book holds it: a development of this build's read back; one not well formed opens read-only.
    void readOnlyWhenNotWellFormed() {
        const std::string bytes = grey_dng(0.18).build();
        core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(bytes)}, {"name", "x.dng"}}}));
        write_book(doc, at("raw-ok.genko"));
        Json saved = project(at("raw-ok.genko"));
        QVERIFY(has_feature(saved, core::kCameraRawFeature));
        QVERIFY(storage::load_document(at("raw-ok.genko")).document.read_only_reason.empty());
        for (const auto& [name, change] : std::vector<std::pair<const char*, std::function<void(Json&)>>>{
                 {"raw-space.genko", [](Json& raw) { raw["develop"]["space"] = "lab"; }},
                 {"raw-key.genko", [](Json& raw) { raw["develop"]["film"] = 1; }},
                 {"raw-camera.genko", [](Json& raw) { raw["camera"]["lens"] = "x"; }}}) {
            write_book(doc, at(name));
            Json p = project(at(name));
            bool changed = false;
            for (auto& [key, contents] : p["smart_contents"].items()) {
                if (contents.contains("raw")) {
                    change(contents["raw"]);
                    changed = true;
                }
            }
            QVERIFY(changed);
            genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(at(name) / "project.json")), p.dump(2));
            const storage::LoadResult read = storage::load_document(at(name));
            QVERIFY2(!read.document.read_only_reason.empty(), name);
        }
    }

    // A RAW LibRaw cannot develop (a compression it does not know) opens the camera's embedded preview at 8 bits, said; one
    // with no preview is refused.
    void anUnknownCameraOpensItsPreview() {
        rawgen::DngSpec spec = grey_dng(0.18);
        spec.compression = 65000;
        spec.preview_jpeg = jpeg_of(48, 32, {200, 100, 50});
        const formats::Opened opened = formats::open_file(spec.build(), "new.dng");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.layers[0].raster_png);
        const render::Image picture = render::open_image(*page.layers[0].raster_png);
        QCOMPARE(picture.width(), 48);
        QVERIFY(!opened.notes.empty());
        QVERIFY2(opened.notes[0].find("埋め込んだプレビュー（48×32 px・8 bit）") != std::string::npos, opened.notes[0].c_str());
        QCOMPARE(page.native_source->format, std::string("raw"));
        QCOMPARE(page.native_source->depth, 8);
        spec.preview_jpeg.reset();
        try {
            (void)formats::open_file(spec.build(), "new.dng");
            QFAIL("opened");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("unidentified_image"));
        }
        // (cut short: broken, said so)
        const std::string whole = grey_dng(0.18).build();
        try {
            (void)formats::open_file(whole.substr(0, whole.size() / 2), "cut.dng");
            QFAIL("opened");
        } catch (const core::Error& error) {
            QVERIFY2(std::string(error.code()) == "format" || std::string(error.code()) == "unidentified_image", error.what());
        }
    }

    // (review F-e P2-3) A TIFF-shaped RAW without a RAW's marks (NEF) is known by the name given with its data — MCP's
    // {data, name} —; a TIFF named so is the picture it is.
    void aRawByItsName() {
        const std::string nef = rawgen::tiff_ep("NIKON", "D3X", 64, 48, rawgen::mosaic(rawgen::scene(64, 48), 64, {0, 1, 1, 2}, 0, 4095), {0, 1, 1, 2}, 12);
        QVERIFY(!render::raw::looks_raw(nef));
        const core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(nef)}, {"name", "x.nef"}}}));
        const std::string id = contents_id_of(doc);
        QVERIFY(!id.empty());
        QCOMPARE(doc.smart_contents.at(id)->suffix, std::string(".nef"));
        QCOMPARE(doc.smart_contents.at(id)->width_px, std::int64_t(64));
        QVERIFY(doc.smart_contents.at(id)->raw);
        const std::string tiff = codecs::encode(codecs::CodecImage::blank(30, 20, codecs::Layout::Rgb, codecs::Sample::U8), codecs::Format::Tiff);
        const core::Document named = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(tiff)}, {"name", "fake.nef"}}}));
        QCOMPARE(named.smart_contents.begin()->second->format, std::string("tiff"));
        QVERIFY(!named.smart_contents.begin()->second->raw);
    }

    // (review F-e P2-4) A linked RAW developed with its orientation none and 8 bits, updated from a changed file: its size
    // and depth still its development's, its camera read again; updated to a file no longer a RAW: no development.
    void aLinkedRawUpdated() {
        const std::string first = grey_dng(0.18, 6).build();
        core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(first)}, {"name", "a.dng"},
                                                                 {"new_id", "so"}, {"link", Json{{"path", "/nowhere/a.dng"}}}}}));
        const std::string id = contents_id_of(doc);
        doc = run(doc, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"orientation", "none"}, {"depth", 8}}}}}));
        QCOMPARE(doc.smart_contents.at(id)->width_px, std::int64_t(96));
        rawgen::DngSpec other = grey_dng(0.3, 6);
        other.model = "Other";
        doc = run(doc, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(other.build())}, {"name", "a.dng"}}}));
        const core::SmartContents& updated = *doc.smart_contents.at(id);
        QCOMPARE(updated.width_px, std::int64_t(96));  // (orientation none: the sensor's sides)
        QCOMPARE(updated.height_px, std::int64_t(64));
        QCOMPARE(updated.depth, 8);
        QCOMPARE((*updated.raw)["develop"]["orientation"], Json("none"));
        QCOMPARE((*updated.raw)["camera"]["model"], Json("Other"));
        QCOMPARE(updated.profile["use"], Json("embedded"));
        const std::string png = render::write_png(render::Image::create("RGB", {8, 6}, render::Ink{1, 2, 3}));
        doc = run(doc, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(png)}, {"name", "a.png"}}}));
        QCOMPARE(doc.smart_contents.at(id)->format, std::string("png"));
        QVERIFY(!doc.smart_contents.at(id)->raw);
        QCOMPARE(doc.smart_contents.at(id)->width_px, std::int64_t(8));
    }

    // (review F-e P3-1) set_camera_raw asks the lock of every page showing the contents; smart_profile refuses a RAW's.
    // (P2-2) A thumbnail's drawing (QuickScope) never develops a RAW: only what a drawing developed.
    void locksAndColours() {
        core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(grey_dng(0.21).build())},
                                                                 {"name", "x.dng"}, {"new_id", "so"}}}));
        const std::string id = contents_id_of(doc);
        const core::Document placed = doc;
        const core::SmartContents& contents = *placed.smart_contents.at(id);
        QVERIFY(!render::smart::developed_ready(placed, contents));
        {
            const render::smart::QuickScope quick;
            QVERIFY(!render::smart::file_picture(placed, contents));
        }
        QVERIFY(render::smart::file_picture(placed, contents));
        QVERIFY(render::smart::developed_ready(placed, contents));
        {
            const render::smart::QuickScope quick;
            QVERIFY(render::smart::file_picture(placed, contents));
        }
        core::CommandBus bus(render::ops_registry());
        doc = bus.apply(doc, Json::array({Json{{"op", "lock_page"}, {"page", 1}, {"agent", "ai:y"}}}), core::Actor("ai:y")).doc;
        const Json op{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"exposure_ev", 1}}}};
        try {
            (void)bus.apply(doc, Json::array({op}), core::Actor("ai:x"));
            QFAIL("a page another holds");
        } catch (const core::ApplyError& error) {
            QVERIFY2(std::string(error.what()).find("lock") != std::string::npos, error.what());
        }
        QCOMPARE((*bus.apply(doc, Json::array({op}), core::Actor("ai:y")).doc.smart_contents.at(id)->raw)["develop"]["exposure_ev"], Json(1.0));
        try {
            (void)run(placed, Json::array({Json{{"op", "smart_profile"}, {"page", 1}, {"id", "so"}, {"use", "keep"}}}));
            QFAIL("a RAW's colour use");
        } catch (const core::ApplyError& error) {
            QVERIFY2(std::string(error.what()).find("camera RAW") != std::string::npos, error.what());
        }
    }

    // The limits: refused before LibRaw unpacks the sensor data.
    void theLimits() {
        formats::OpenOptions options;
        options.limits.max_pixels = 1000;
        try {
            (void)formats::open_file(grey_dng(0.18).build(), "big.dng", options);
            QFAIL("opened");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("image_too_large"));
        }
    }

    // genko new --from x.dng --raw '<JSON>' (the development, the resolution, a smart object), Undo of set_camera_raw from
    // the command line, and the usage errors.
    void theCommandLine() {
        const QString file = qat("cli.dng");
        genko::test::write_bytes(file, grey_dng(0.18).build());
        const QString book = qat("cli.genko");
        auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--from"), file, QStringLiteral("--raw"),
                                            QStringLiteral(R"({"develop": {"space": "srgb", "depth": 8}, "resolution": 240})")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const Json reply = genko::test::one_line(made.out);
        QCOMPARE(reply["ok"], Json(true));
        QVERIFY(reply["notes"][0].get<std::string>().find("sRGB・8 bit") != std::string::npos);
        const Json cli_saved = project(at("cli.genko"));  // (the page's spec, or the book's when the page has none of its own)
        const Json& cli_page = cli_saved["pages"][0];
        QCOMPARE(cli_page.is_object() && cli_page.contains("spec") ? cli_page["spec"]["dpi"] : cli_saved["spec"]["dpi"], Json(240));
        // (a smart object, developed again and undone)
        const QString object = qat("cli-object.genko");
        made = genko::test::run_genko({QStringLiteral("new"), object, QStringLiteral("--from"), file, QStringLiteral("--raw"), QStringLiteral(R"({"smart_object": true})")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        Json saved = project(at("cli-object.genko"));
        QVERIFY(has_feature(saved, core::kCameraRawFeature));
        std::string id;
        for (const auto& [key, contents] : saved["smart_contents"].items())
            if (contents.contains("raw")) id = key;
        QVERIFY(!id.empty());
        const QString ops = qat("cli-ops.json");
        genko::test::write_bytes(ops, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"exposure_ev", 1.5}}}}}).dump());
        auto applied = genko::test::run_genko({QStringLiteral("apply"), object, ops, QStringLiteral("--agent"), QStringLiteral("human:test")});
        QVERIFY2(applied.exit_code == 0, qPrintable(applied.described()));
        QCOMPARE(project(at("cli-object.genko"))["smart_contents"][id]["raw"]["develop"]["exposure_ev"], Json(1.5));
        auto undone = genko::test::run_genko({QStringLiteral("undo"), object, QStringLiteral("--as"), QStringLiteral("human:test")});
        QVERIFY2(undone.exit_code == 0, qPrintable(undone.described()));
        QCOMPARE(project(at("cli-object.genko"))["smart_contents"][id]["raw"]["develop"]["exposure_ev"], Json(0.0));
        // (usage)
        for (const QStringList& args : {QStringList{QStringLiteral("new"), qat("u1.genko"), QStringLiteral("--from"), file, QStringLiteral("--raw"), QStringLiteral("{not json")},
                                        QStringList{QStringLiteral("new"), qat("u2.genko"), QStringLiteral("--from"), file, QStringLiteral("--raw"), QStringLiteral("[1]")},
                                        QStringList{QStringLiteral("new"), qat("u3.genko"), QStringLiteral("--from"), file, QStringLiteral("--raw"), QStringLiteral(R"({"resolution": "x"})")},
                                        QStringList{QStringLiteral("new"), qat("u4.genko"), QStringLiteral("--image"), QStringLiteral("--width"), QStringLiteral("8"),
                                                    QStringLiteral("--height"), QStringLiteral("8"), QStringLiteral("--raw"), QStringLiteral("{}")}}) {
            const auto refused = genko::test::run_genko(args);
            QVERIFY2(refused.exit_code == 2, qPrintable(refused.described()));
        }
        // (a development refused: the command failed, no book written)
        const auto bad = genko::test::run_genko({QStringLiteral("new"), qat("bad.genko"), QStringLiteral("--from"), file, QStringLiteral("--raw"),
                                                 QStringLiteral(R"({"develop": {"space": "lab"}})")});
        QCOMPARE(bad.exit_code, 1);
        QVERIFY(!fs::exists(at("bad.genko") / "project.json"));
        // (genko --version: LibRaw's)
        const auto version = genko::test::run_genko({QStringLiteral("--version")});
        QVERIFY(version.out.contains("libraw"));
    }
};

QTEST_GUILESS_MAIN(TestContractRaw)
#include "test_contract_raw.moc"
