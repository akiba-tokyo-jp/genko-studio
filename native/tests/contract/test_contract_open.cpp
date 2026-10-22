// PC4 F-0 (SPEC.md FILE-01, the user's decision 4 of 2026-10-20): a picture opened as a book of one page held in memory
// (formats/open_file) — a PNG, a 16-bit TIFF, a grey picture, a CMYK JPEG — holds the file's pixels as they are, at its
// depth, on a paper of its pixels at its resolution, and gives them back as its picture at that depth
// (formats/page_picture); saved, it names what it uses and is read back the same. A book of none of PC4 節 F's
// extensions is written as before (no new key, no new feature); the five extensions' frames (file-info@1, image-grid@1,
// psd-keep@1, formats@1, camera-raw@1) are written while used and read back, one not well formed opens read-only; the
// files' ops are registered and refused as not in this build yet. (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cstring>
#include <string>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/file_info.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/psd_keep.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "formats/pillow_save.hpp"
#include "render/codecs/codecs.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
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
namespace fs = std::filesystem;
using core::Json;

namespace {

std::string data_file(const char* relative) { return genko::test::read_bytes(genko::test::test_data(QString::fromUtf8(relative))); }

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

core::Document read_book(const fs::path& dir) { return storage::load_document(dir).document; }

Json project(const fs::path& dir) { return Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")))); }

bool has_feature(const Json& saved, std::string_view feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

// Does the JSON hold this key anywhere?
bool holds_key(const Json& value, std::string_view key) {
    if (value.is_object()) {
        for (const auto& [k, v] : value.items()) {
            if (k == key || holds_key(v, key)) return true;
        }
    } else if (value.is_array()) {
        for (const Json& v : value) {
            if (holds_key(v, key)) return true;
        }
    }
    return false;
}

std::string code_of(const std::function<void()>& run) {
    try {
        run();
    } catch (const render::NotYetPorted& error) {
        return "not_yet_ported:" + error.element();
    } catch (const core::Error& error) {
        return error.code();
    }
    return {};
}

std::uint16_t u16_at(const std::string& data, std::size_t i) {
    std::uint16_t v = 0;
    std::memcpy(&v, data.data() + i * 2, 2);
    return v;
}

// A 16-bit RGB TIFF of 300 dpi (this build's value writer) and its values.
std::pair<std::string, codecs::CodecImage> tiff16(std::uint32_t w, std::uint32_t h) {
    std::vector<float> values(std::size_t(w) * h * 3);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<float>((i * 40503u + 17u) % 65536u) / 65535.0f;
    const codecs::CodecImage picture = codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U16, values);
    const std::string gkcr = core::encode_color_pixels(w, h, "u16", [&](std::size_t i) {
        return std::array<double, 4>{picture.value(i * 3), picture.value(i * 3 + 1), picture.value(i * 3 + 2), 1.0};
    });
    return {formats::values_tiff_bytes(core::ColorRasterView(gkcr), 16, formats::TiffSave{"tiff_lzw", std::array<double, 2>{300, 300}, {}}), picture};
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("files", core::Num(1), 1, spec, core::Binding::Right);
}

core::Document run(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
}

}  // namespace

class TestContractOpen : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const char* name) const { return fs::path(tmp_.path().toStdString()) / name; }

private slots:
    void aPngOpensAsItsPixels() {
        for (const char* name : {"rgb8.png", "rgba8.png"}) {
            const std::string bytes = data_file((std::string("pyref/png/") + name).c_str());
            const render::Image file = render::open_image(bytes);
            const formats::Opened opened = formats::open_file(bytes, std::string("some/folder/") + name);
            const core::Document& doc = opened.doc;
            QCOMPARE(doc.title, fs::path(name).stem().string());
            QVERIFY(!doc.asset_dir);  // (no folder: saving asks where its .genko goes)
            QCOMPARE(doc.pages.size(), std::size_t(1));
            const core::Page& page = doc.page(0);
            QVERIFY(page.frames.empty());
            QCOMPARE(page.layers.size(), std::size_t(1));
            const core::Layer& layer = page.layers[0];
            const bool alpha = std::string_view(name) == "rgba8.png";
            QCOMPARE(layer.title, std::string(alpha ? "レイヤー 0" : "背景"));
            QVERIFY(layer.raster_png && !layer.color_raster && !layer.ink_raster);
            QVERIFY(render::open_image(*layer.raster_png).convert("RGBA").tobytes() == file.convert("RGBA").tobytes());
            // (its paper: its pixels at its resolution — 72 ppi for a file that says none —, the trim the paper)
            const int dpi = static_cast<int>(page.spec.dpi.value());
            QVERIFY(dpi >= 72);
            QCOMPARE(render::mm_to_px(page.spec.width_mm.value(), dpi), file.width());
            QCOMPARE(render::mm_to_px(page.spec.height_mm.value(), dpi), file.height());
            QCOMPARE(page.spec.bleed_mm.value(), 0.0);
            QCOMPARE(page.spec.trim_size().first.value(), page.spec.width_mm.value());
            QCOMPARE(page.spec.grid, std::optional<std::string>("finest"));  // (an 8-bit page at its pixels' grid: design 8-2 (b))
            QVERIFY(!page.color && !page.rgb);
            QVERIFY(page.native_source);
            QCOMPARE(page.native_source->format, std::string("png"));
            QCOMPARE(page.native_source->name, std::string(name));
            QCOMPARE(page.native_source->width_px, std::int64_t(file.width()));
            QCOMPARE(page.native_source->depth, 8);
            QCOMPARE(page.native_source->mode, std::string(alpha ? "rgba" : "rgb"));
            // (its picture: the file's pixels, the page's paper under a transparent one)
            const formats::PagePicture picture = formats::page_picture(page, doc, dpi);
            QCOMPARE(picture.image.layout(), codecs::Layout::Rgb);
            QCOMPARE(picture.image.sample(), codecs::Sample::U8);
            if (!alpha) QVERIFY(picture.image.data() == file.convert("RGB").tobytes());
            const formats::PagePicture own = formats::layer_picture(page, layer, doc, dpi);
            QCOMPARE(own.image.layout(), codecs::Layout::Rgba);
            QVERIFY(own.image.data() == file.convert("RGBA").tobytes());
        }
    }

    void aSixteenBitTiffKeepsItsBits() {
        const auto [bytes, values] = tiff16(13, 9);
        const formats::Opened opened = formats::open_file(bytes, "deep.tif");
        const core::Document& doc = opened.doc;
        const core::Page& page = doc.page(0);
        QVERIFY(page.rgb && !page.color);
        QCOMPARE(page.rgb->depth, 16);
        QVERIFY(page.rgb->profile.empty());  // (it carries no profile: sRGB)
        QVERIFY(doc.rgb && *doc.rgb == *page.rgb);
        QCOMPARE(page.spec.dpi.value(), 300.0);
        QVERIFY(!page.spec.grid);  // (a page in precise colour works at its finest grid already)
        const core::Layer& layer = page.layers[0];
        QVERIFY(layer.color_raster && !layer.raster_png);
        const core::ColorRasterView view(*layer.color_raster);
        QCOMPARE(view.width(), 13u);
        QCOMPARE(view.height(), 9u);
        for (std::size_t i = 0; i < 13 * 9; ++i) {
            const auto p = view.pixel(i);
            for (unsigned c = 0; c < 3; ++c) QCOMPARE(std::lround(p[c] * 65535.0), long(u16_at(values.data(), i * 3 + c)));
            QCOMPARE(p[3], 1.0);
        }
        QCOMPARE(page.native_source->depth, 16);
        QCOMPARE(page.native_source->dpi, (std::optional<std::pair<double, double>>(std::make_pair(300.0, 300.0))));
        const formats::PagePicture picture = formats::page_picture(page, doc, 300);
        QCOMPARE(picture.image.sample(), codecs::Sample::U16);
        QCOMPARE(picture.image.layout(), codecs::Layout::Rgb);
        QVERIFY(picture.image.data() == values.data());
        QVERIFY(picture.notes.empty());
        const formats::PagePicture own = formats::layer_picture(page, layer, doc, 300);
        QCOMPARE(own.image.layout(), codecs::Layout::Rgba);
        QVERIFY(own.image.convert(codecs::Layout::Rgb, codecs::Sample::U16).data() == values.data());
    }

    void aGreyPictureOpensAsInks() {
        const std::string bytes = data_file("pyref/png/l8.png");
        const render::Image file = render::open_image(bytes);
        const formats::Opened opened = formats::open_file(bytes, "grey.png");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.color && !page.rgb);
        QCOMPARE(page.color->space, core::InkSpace::Gray);
        QCOMPARE(page.color->depth, 8);
        QCOMPARE(page.color->profile, core::builtin_profile("sgray")->ref());  // (none carried: the working grey's, built in)
        QCOMPARE(page.spec.expression, std::string("mono"));
        const core::InkPixels inks = core::decode_ink_raster(*page.layers[0].ink_raster);
        const std::string grey = file.convert("L").tobytes();
        QCOMPARE(inks.pixel_count(), grey.size());
        for (std::size_t i = 0; i < grey.size(); ++i) {
            QCOMPARE(inks.samples[i * 2], std::uint16_t(static_cast<unsigned char>(grey[i])));
            QCOMPARE(inks.samples[i * 2 + 1], std::uint16_t(255));
        }
        const formats::PagePicture picture = formats::page_picture(page, opened.doc, static_cast<int>(page.spec.dpi.value()));
        QCOMPARE(picture.image.layout(), codecs::Layout::Gray);
        QVERIFY(picture.image.data() == grey);
        QVERIFY(picture.image.icc && *picture.image.icc == *core::builtin_profile("sgray")->bytes);
        // (a grey TIFF of 16 bits: a grey page of 16 bits)
        core::InkPixels deep = core::blank_ink_pixels(core::InkSpace::Gray, 16, 7, 5);
        for (std::size_t i = 0; i < deep.pixel_count(); ++i) {
            deep.set(i, 0, static_cast<double>(i * 9001 % 65536) / 65535.0);
            deep.set(i, 1, 1.0);
        }
        const formats::Opened tiff = formats::open_file(formats::ink_tiff_bytes(deep, formats::TiffSave{}), "grey16.tif");
        const core::Page& deep_page = tiff.doc.page(0);
        QCOMPARE(deep_page.color->depth, 16);
        QVERIFY(core::decode_ink_raster(*deep_page.layers[0].ink_raster).samples == deep.samples);
        const formats::PagePicture deep_picture = formats::page_picture(deep_page, tiff.doc, static_cast<int>(deep_page.spec.dpi.value()));
        QCOMPARE(deep_picture.image.sample(), codecs::Sample::U16);
        for (std::size_t i = 0; i < deep.pixel_count(); ++i) QCOMPARE(u16_at(deep_picture.image.data(), i), deep.samples[i * 2]);
    }

    void aCmykJpegOpensAsInks() {
        const std::string bytes = data_file("pyref/jpeg/cmyk.jpg");
        const render::Image file = render::open_image(bytes);
        QCOMPARE(file.mode(), std::string_view("CMYK"));
        const formats::Opened opened = formats::open_file(bytes, "print.jpg");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.color);
        QCOMPARE(page.color->space, core::InkSpace::Cmyk);
        QCOMPARE(page.color->depth, 8);
        QVERIFY(page.color->icc && !page.color->icc->empty());
        QCOMPARE(page.native_source->mode, std::string("cmyk"));
        QCOMPARE(page.native_source->format, std::string("jpeg"));
        const core::InkPixels inks = core::decode_ink_raster(*page.layers[0].ink_raster);
        const std::string cmyk = file.tobytes();
        QCOMPARE(inks.pixel_count() * 4, cmyk.size());
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            for (unsigned c = 0; c < 4; ++c) QCOMPARE(inks.samples[i * 5 + c], std::uint16_t(static_cast<unsigned char>(cmyk[i * 4 + c])));
        }
        const formats::PagePicture picture = formats::page_picture(page, opened.doc, static_cast<int>(page.spec.dpi.value()));
        QCOMPARE(picture.image.layout(), codecs::Layout::Cmyk);
        QVERIFY(picture.image.data() == cmyk);
    }

    // The page's profile: the file's own (an sRGB one is the page's without a profile, whatever the working RGB); a file that
    // carries none takes the working RGB given (an sRGB one: none). The numbers stay the file's (keep / assign, not converted).
    void anRgbPictureTakesItsOwnProfileOrTheWorkingOne() {
        const std::string srgb = formats::srgb_icc_once();
        const std::string adobe = *core::builtin_profile("adobe-rgb-compat")->bytes;
        QVERIFY(core::icc_description(srgb).starts_with("sRGB"));
        QVERIFY(!core::icc_description(adobe).starts_with("sRGB"));
        const render::Image picture = render::Image::create("RGB", {6, 4}, render::Ink{30, 140, 220});
        const auto png = [&](const std::string& icc) { return formats::png_bytes(picture, formats::PngSave{std::nullopt, icc, false}); };
        const auto open = [](const std::string& bytes, const char* name, const std::string& working) {
            formats::OpenOptions options;
            options.working_rgb = working;
            return formats::open_file(bytes, name, options);
        };
        // (an sRGB file, the working RGB Adobe RGB: sRGB, the 8-bit page of before)
        const formats::Opened tagged_srgb = open(png(srgb), "srgb.png", adobe);
        QVERIFY(!tagged_srgb.doc.page(0).rgb && !tagged_srgb.doc.rgb);
        QVERIFY(render::open_image(*tagged_srgb.doc.page(0).layers[0].raster_png).convert("RGB").tobytes() == picture.tobytes());
        // (an Adobe RGB file: its own profile, its numbers)
        const formats::Opened tagged_adobe = open(png(adobe), "adobe.png", {});
        QVERIFY(tagged_adobe.doc.page(0).rgb);
        QCOMPARE(tagged_adobe.doc.page(0).rgb->profile, core::builtin_profile("adobe-rgb-compat")->ref());
        QCOMPARE(tagged_adobe.doc.page(0).rgb->depth, 8);
        QVERIFY(render::open_image(*tagged_adobe.doc.page(0).layers[0].raster_png).convert("RGB").tobytes() == picture.tobytes());
        // (no profile, the working RGB Adobe RGB: that one assigned; the working RGB sRGB: none)
        const formats::Opened untagged = open(png({}), "plain.png", adobe);
        QVERIFY(untagged.doc.page(0).rgb);
        QCOMPARE(untagged.doc.page(0).rgb->profile, core::builtin_profile("adobe-rgb-compat")->ref());
        QVERIFY(!open(png({}), "plain.png", srgb).doc.page(0).rgb);
        // (a 16-bit sRGB TIFF, the working RGB Adobe RGB: 16 bits of sRGB, no profile)
        const std::string gkcr = core::encode_color_pixels(5, 3, "u16", [](std::size_t i) {
            return std::array<double, 4>{static_cast<double>(i) / 15.0, 0.25, 0.75, 1.0};
        });
        const formats::Opened deep =
            open(formats::values_tiff_bytes(core::ColorRasterView(gkcr), 16, formats::TiffSave{"tiff_lzw", std::nullopt, srgb}), "deep.tif", adobe);
        QVERIFY(deep.doc.page(0).rgb);
        QCOMPARE(deep.doc.page(0).rgb->depth, 16);
        QVERIFY(deep.doc.page(0).rgb->profile.empty());
    }

    void anOpenedBookIsSavedAndReadBack() {
        const auto [deep_bytes, deep_values] = tiff16(10, 6);
        struct Case {
            const char* name;
            std::string bytes;
            std::vector<std::string_view> features;
        };
        const std::vector<Case> cases{
            {"rgba8.png", data_file("pyref/png/rgba8.png"), {core::kFileInfoFeature, core::kImageGridFeature}},
            {"deep.tif", deep_bytes, {core::kFileInfoFeature, core::kColorModelFeature}},
            {"l8.png", data_file("pyref/png/l8.png"), {core::kFileInfoFeature, core::kColorModelFeature}},
            {"cmyk.jpg", data_file("pyref/jpeg/cmyk.jpg"), {core::kFileInfoFeature, core::kColorModeFeature}},
        };
        int n = 0;
        for (const Case& c : cases) {
            const formats::Opened opened = formats::open_file(c.bytes, c.name);
            const fs::path dir = at(("opened-" + std::to_string(n++) + ".genko").c_str());
            write_book(opened.doc, dir);
            const Json saved = project(dir);
            for (const std::string_view f : c.features) QVERIFY2(has_feature(saved, f), (std::string(c.name) + " " + std::string(f)).c_str());
            if (std::string_view(c.name) != "rgba8.png") QVERIFY(!has_feature(saved, core::kImageGridFeature));
            for (const std::string_view f : {core::kPsdKeepFeature, core::kFormatsFeature, core::kCameraRawFeature}) QVERIFY(!has_feature(saved, f));
            const storage::LoadResult read = storage::load_document(dir);
            QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
            const core::Page& before = opened.doc.page(0);
            const core::Page& after = read.document.page(0);
            QVERIFY(after.native_source && *after.native_source == *before.native_source);
            QCOMPARE(after.spec.grid, before.spec.grid);
            const core::Layer& a = after.layers[0];
            const core::Layer& b = before.layers[0];
            QVERIFY((a.raster_png && b.raster_png && *a.raster_png == *b.raster_png) || (a.color_raster && b.color_raster && *a.color_raster == *b.color_raster) ||
                    (a.ink_raster && b.ink_raster && *a.ink_raster == *b.ink_raster));
            // (written again, the same)
            storage::AssetStore store(dir);
            Json again = storage::project_payload_v4(read.document, store);
            again["revision"] = saved["revision"];
            again["writer"] = saved["writer"];
            std::string where;
            QVERIFY2(genko::test::strict_equal(again, saved, &where), (std::string(c.name) + " " + where).c_str());
        }
    }

    void aBookWithoutTheNewExtensionsIsAsBefore() {
        const QString dir = QString::fromStdString(core::path_to_utf8(at("plain.genko")));
        const auto made = genko::test::run_genko({QStringLiteral("new"), dir, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const std::string text = genko::test::read_bytes(dir + QStringLiteral("/project.json"));
        const Json saved = Json::parse(text);
        for (const char* key : {"file_info", "native_source", "native_psd", "grid", "raw"}) QVERIFY2(!holds_key(saved, key), key);
        for (const std::string_view f : {core::kFileInfoFeature, core::kImageGridFeature, core::kPsdKeepFeature, core::kFormatsFeature, core::kCameraRawFeature})
            QVERIFY(!has_feature(saved, f));
        // (read and written again: the same bytes)
        const core::Document doc = read_book(fs::path(dir.toStdString()));
        QVERIFY(!doc.file_info && !doc.spec.grid && !doc.page(0).native_source && !doc.page(0).native_psd);
        storage::AssetStore store(fs::path(dir.toStdString()));
        QVERIFY(storage::project_json_v4(doc, store) == text);
        // (a book made by ops with pictures, a precise page and a smart object: none of them either)
        core::Document worked = run(small_book(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}},
                                                               Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"},
                                                                    {"png_base64", core::b64encode(render::write_png(render::Image::create("RGBA", {8, 8}, render::Ink{1, 2, 3, 255})))}},
                                                               Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so1"}}}));
        const fs::path worked_dir = at("worked.genko");
        write_book(worked, worked_dir);
        const Json worked_saved = project(worked_dir);
        for (const char* key : {"file_info", "native_source", "native_psd", "grid", "raw"}) QVERIFY2(!holds_key(worked_saved, key), key);
        QVERIFY(has_feature(worked_saved, core::kSmartObjectsFeature));
        for (const std::string_view f : {core::kFileInfoFeature, core::kImageGridFeature, core::kPsdKeepFeature, core::kFormatsFeature, core::kCameraRawFeature})
            QVERIFY(!has_feature(worked_saved, f));
    }

    // The five extensions' frames: written while used (with the assets they name, from the book in memory), read back the
    // same; one not well formed opens the book read-only.
    void theNewExtensionsAreWrittenAndRead() {
        const std::string png = render::write_png(render::Image::create("RGB", {6, 4}, render::Ink{200, 100, 50}));
        const std::string other = render::write_png(render::Image::create("RGB", {6, 4}, render::Ink{20, 100, 250}));
        core::Document doc = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png)}, {"name", "a.png"}},
                                                            Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(other)}, {"name", "b.png"}}}));
        QCOMPARE(doc.smart_contents.size(), std::size_t(2));
        // (their files taken as an OpenEXR file and a DNG: formats@1, camera-raw@1)
        int k = 0;
        for (auto& [id, contents] : doc.smart_contents) {
            auto changed = std::make_shared<core::SmartContents>(*contents);
            if (k++ == 0) {
                changed->format = "exr";
                changed->suffix = ".exr";
                changed->depth = 32;
            } else {
                changed->format = "raw";
                changed->suffix = ".dng";
                changed->depth = 16;
                changed->raw = Json{{"develop", Json{{"white_balance", "as_shot"}}}, {"camera", Json{{"make", "Test"}}}};
            }
            contents = changed;
        }
        const auto blob = [&](const std::string& bytes) {
            const std::string ref = core::bytes_ref(bytes);
            doc.native_files[ref] = std::make_shared<const std::string>(bytes);
            return ref;
        };
        core::FileInfo info;
        info.author = "作者";
        info.keywords = {"漫画", "テスト"};
        info.copyright_status = "copyrighted";
        info.xmp = blob("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>");
        doc.file_info = info;
        doc.spec.grid = "finest";
        core::Page& page = doc.edit_page(0);
        page.spec.grid = "finest";
        core::NativeSource source;
        source.format = "psd";
        source.name = "元.psd";
        source.width_px = 354;
        source.height_px = 472;
        source.dpi = std::make_pair(150.0, 150.0);
        source.depth = 16;
        source.mode = "cmyk";
        source.exif = blob(std::string("II*\0\x08\0\0\0\0\0", 10));
        source.camera = core::CameraInfo{"Maker", "Model", 200.0, 0.01, 2.8, 35.0};
        page.native_source = source;
        core::PsdPageKeep page_keep;
        page_keep.resources = {core::PsdResource{1050, "slices", blob("slices bytes")}};
        page_keep.globals = {core::PsdGlobal{"8BIM", "Patt", blob("pattern bytes")}};
        page_keep.from = core::PsdFrom{2, 4, 16};
        page.native_psd = page_keep;
        core::PsdLayerKeep layer_keep;
        layer_keep.blocks = {{"lfx2", blob("effects bytes")}, {"lclr", blob("label bytes")}};
        layer_keep.ranges = blob("ranges bytes");
        layer_keep.id = 7;
        page.layers.back().native_psd = layer_keep;

        const fs::path dir = at("extensions.genko");
        write_book(doc, dir);
        const Json saved = project(dir);
        for (const std::string_view f : {core::kFileInfoFeature, core::kImageGridFeature, core::kPsdKeepFeature, core::kFormatsFeature, core::kCameraRawFeature})
            QVERIFY2(has_feature(saved, f), std::string(f).c_str());
        QVERIFY2(saved["file_info"] == core::to_json(info), saved["file_info"].dump().c_str());
        QVERIFY(saved["spec"]["grid"] == Json("finest"));
        storage::AssetStore store(dir);
        QVERIFY(store.has(info.xmp, core::kXmpSuffix));
        QVERIFY(store.has(source.exif, core::kExifSuffix));
        QVERIFY(store.has(layer_keep.ranges, core::kPsdBlockSuffix));
        QVERIFY(store.has(page_keep.globals[0].ref, core::kPsdBlockSuffix));

        const storage::LoadResult read = storage::load_document(dir);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(read.document.file_info && *read.document.file_info == info);
        QCOMPARE(read.document.spec.grid, std::optional<std::string>("finest"));
        const core::Page& back = read.document.page(0);
        QVERIFY(back.native_source && *back.native_source == source);
        QVERIFY(back.native_psd && *back.native_psd == page_keep);
        QVERIFY(back.layers.back().native_psd && *back.layers.back().native_psd == layer_keep);
        QVERIFY(core::smart::uses_formats(read.document) && core::smart::uses_camera_raw(read.document));
        Json again = storage::project_payload_v4(read.document, store);
        again["revision"] = saved["revision"];
        again["writer"] = saved["writer"];
        std::string where;
        QVERIFY2(genko::test::strict_equal(again, saved, &where), where.c_str());

        // (not well formed: read-only, and kept as it was read)
        const auto broken = [&](const char* name, const std::function<void(Json&)>& change) {
            const fs::path copy = at(name);
            genko::test::copy_tree(QString::fromStdString(core::path_to_utf8(dir)), QString::fromStdString(core::path_to_utf8(copy)));
            Json p = saved;
            change(p);
            genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(copy / "project.json")), core::dump_python_indent2(p));
            const storage::LoadResult r = storage::load_document(copy);
            return !r.document.read_only_reason.empty();
        };
        QVERIFY(broken("b1.genko", [](Json& p) { p["pages"][0]["native_source"]["format"] = "xyz"; }));
        QVERIFY(broken("b2.genko", [](Json& p) { p["file_info"]["copyright_status"] = "maybe"; }));
        QVERIFY(broken("b3.genko", [](Json& p) { p["file_info"]["colour"] = "red"; }));
        QVERIFY(broken("b4.genko", [](Json& p) { p["spec"]["grid"] = "coarse"; }));
        QVERIFY(broken("b5.genko", [](Json& p) { p["pages"][0]["native_psd"]["globals"][0][0] = "XXXX"; }));
        QVERIFY(broken("b6.genko", [](Json& p) { p["pages"][0]["layers"].back()["native_psd"]["blocks"][0][1] = "not an asset"; }));
        QVERIFY(broken("b7.genko", [](Json& p) {
            for (auto& [id, item] : p["smart_contents"].items()) item["raw"] = Json::array();  // (a list, and on the EXR's too)
        }));
        QVERIFY(broken("b8.genko", [](Json& p) { p["features"].push_back("file-info@2"); }));
    }

    void whatIsNotOpenedYet() {
        const std::string png = data_file("pyref/png/rgb8.png");
        QCOMPARE(code_of([] { (void)formats::open_file(std::string("8BPS\0\1", 6) + std::string(40, '\0'), "layers.psd"); }), std::string("not_yet_ported:image_format"));
        const auto [tiff, values] = tiff16(4, 4);
        QCOMPARE(code_of([&] { (void)formats::open_file(tiff, "photo.DNG"); }), std::string("not_yet_ported:image_format"));
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "doc.pcraft"); }), std::string("not_yet_ported:image_format"));
        QCOMPARE(code_of([] { (void)formats::open_file("not a picture at all", "x.png"); }), std::string("unidentified_image"));
        formats::OpenOptions as_jpeg;
        as_jpeg.as = codecs::Format::Jpeg;
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "x.png", as_jpeg); }), std::string("format"));
        formats::OpenOptions small;
        small.limits.max_pixels = 10;
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "x.png", small); }), std::string("image_too_large"));
    }

    void theFilesOpsAreFrames() {
        for (const auto& [op, group] : {std::pair{"set_file_info", "PC4 F-b"}, std::pair{"set_camera_raw", "PC4 F-e"}}) {
            QVERIFY(render::ops_registry().find(op) != nullptr);
            try {
                (void)run(small_book(), Json::array({Json{{"op", op}}}));
                QFAIL("not refused");
            } catch (const core::ApplyError& error) {
                QCOMPARE(error.code(), std::string("not_yet_ported"));
                QVERIFY2(std::string(error.what()).find(group) != std::string::npos, error.what());
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestContractOpen)
#include "test_contract_open.moc"
