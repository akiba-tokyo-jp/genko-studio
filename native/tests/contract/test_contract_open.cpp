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
#include "render/codecs/metadata.hpp"
#include "render/image.hpp"
#include "render/layer_masks.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
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
        QCOMPARE(code_of([] { (void)formats::open_file(std::string("8BPS\0\1", 6) + std::string(40, '\0'), "layers.psd"); }), std::string("not_yet_ported:psd_file"));
        const auto [tiff, values] = tiff16(4, 4);
        QCOMPARE(code_of([&] { (void)formats::open_file(tiff, "photo.DNG"); }), std::string("not_yet_ported:raw_file"));
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "doc.pcraft"); }), std::string("not_yet_ported:pcraft_file"));
        QCOMPARE(code_of([] { (void)formats::open_file("not a picture at all", "x.png"); }), std::string("unidentified_image"));
        formats::OpenOptions as_jpeg;
        as_jpeg.as = codecs::Format::Jpeg;
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "x.png", as_jpeg); }), std::string("format"));
        formats::OpenOptions small;
        small.limits.max_pixels = 10;
        QCOMPARE(code_of([&] { (void)formats::open_file(png, "x.png", small); }), std::string("image_too_large"));
    }

    // PC4 F-b: 新規… → 画像 (formats::new_image): each mode at each of its depths with each background — the page's colour
    // model, its one layer's name and pixels, the paper of its pixels at its resolution, the grid mark only on an 8-bit RGB
    // page —, saved and read back.
    void aNewImageIsItsModeDepthAndBackground() {
        struct Mode {
            const char* mode;
            std::vector<int> depths;
        };
        const std::vector<Mode> modes{{"rgb", {8, 16, 32}}, {"gray", {8, 16, 32}}, {"cmyk", {8, 16}}, {"lab", {8, 16}}, {"bitmap", {8}}};
        int n = 0;
        for (const Mode& m : modes) {
            for (const int depth : m.depths) {
                for (const char* background : {"white", "black", "transparent", "#336699"}) {
                    if (std::string_view(m.mode) == "bitmap" && std::string_view(background) == "transparent") continue;
                    formats::NewImage image;
                    image.title = "新しい画像";
                    image.width = 30;
                    image.height = 20;
                    image.dpi = 150;
                    image.mode = m.mode;
                    image.depth = depth;
                    image.background = background;
                    const std::string what = std::string(m.mode) + " " + std::to_string(depth) + " " + background;
                    const formats::Opened made = formats::new_image(image);
                    const core::Document& doc = made.doc;
                    QCOMPARE(doc.title, std::string("新しい画像"));
                    QCOMPARE(doc.pages.size(), std::size_t(1));
                    const core::Page& page = doc.page(0);
                    QCOMPARE(page.spec.dpi.value(), 150.0);
                    QCOMPARE(render::mm_to_px(page.spec.width_mm.value(), 150), 30);
                    QCOMPARE(render::mm_to_px(page.spec.height_mm.value(), 150), 20);
                    QCOMPARE(page.layers.size(), std::size_t(1));
                    const bool clear = std::string_view(background) == "transparent";
                    QCOMPARE(page.layers[0].title, std::string(clear ? "レイヤー 1" : "背景"));
                    QVERIFY2(!page.native_source, what.c_str());  // (made, not opened)
                    const std::string_view mode = m.mode;
                    if (mode == "rgb" && depth == 8) {
                        QVERIFY2(!page.color && !page.rgb, what.c_str());
                        QCOMPARE(page.spec.grid, std::optional<std::string>("finest"));
                        const render::Image pixels = render::open_image(*page.layers[0].raster_png).convert("RGBA");
                        const auto p = pixels.getpixel(3, 4);
                        const std::vector<double> want = clear ? std::vector<double>{0, 0, 0, 0}
                                                         : std::string_view(background) == "white" ? std::vector<double>{255, 255, 255, 255}
                                                         : std::string_view(background) == "black" ? std::vector<double>{0, 0, 0, 255}
                                                                                                    : std::vector<double>{0x33, 0x66, 0x99, 255};
                        if (clear) QCOMPARE(p[3], 0.0);
                        else QVERIFY2(p == want, what.c_str());
                    } else {
                        QVERIFY2(!page.spec.grid && !doc.spec.grid, what.c_str());  // (a page of another model works at its finest grid already)
                        if (mode == "rgb") {
                            QVERIFY2(page.rgb && page.rgb->depth == depth && !page.color, what.c_str());
                        } else {
                            QVERIFY2(page.color, what.c_str());
                            QCOMPARE(core::mode_name(*page.color), std::string_view(mode == "bitmap" ? "bitmap" : m.mode));
                            QCOMPARE(page.color->depth, depth);
                        }
                        QVERIFY2(page.layers[0].color_raster || page.layers[0].ink_raster, what.c_str());
                    }
                    if (mode == "gray" && !clear) {  // (white: the full light; black: none)
                        const core::InkPixels inks = core::decode_ink_raster(*page.layers[0].ink_raster);
                        const double v = inks.value(0, 0);
                        if (std::string_view(background) == "white") QVERIFY2(v > 0.99, what.c_str());
                        if (std::string_view(background) == "black") QVERIFY2(v < 0.01, what.c_str());
                    }
                    if (mode == "cmyk" && std::string_view(background) == "white") {  // (white paper: no ink)
                        const core::InkPixels inks = core::decode_ink_raster(*page.layers[0].ink_raster);
                        for (unsigned c = 0; c < 4; ++c) QVERIFY2(inks.value(0, c) < 0.01, what.c_str());
                    }
                    const fs::path dir = at(("new-image-" + std::to_string(n++) + ".genko").c_str());
                    write_book(doc, dir);
                    const storage::LoadResult read = storage::load_document(dir);
                    QVERIFY2(read.document.read_only_reason.empty(), (what + ": " + read.document.read_only_reason).c_str());
                    QCOMPARE(read.document.page(0).spec.grid, page.spec.grid);
                }
            }
        }
        // (what it does not make)
        formats::NewImage bad;
        bad.width = 10;
        bad.height = 10;
        bad.mode = "cmyk";
        bad.depth = 32;
        QCOMPARE(code_of([&] { (void)formats::new_image(bad); }), std::string("value"));
        bad.mode = "rgb";
        bad.depth = 8;
        bad.background = "plaid";
        QCOMPARE(code_of([&] { (void)formats::new_image(bad); }), std::string("value"));
        bad.background = "white";
        bad.width = 0;
        QCOMPARE(code_of([&] { (void)formats::new_image(bad); }), std::string("value"));
    }

    // PC4 F-b: a picture of a colour table opens as an indexed page of its colours, one of one bit of grey as a bitmap
    // page; an animation or a TIFF of pages says it opened its first frame or page.
    void aPictureOpensInItsColourModel() {
        // (a palette PNG: Pillow's mode P, written by this build's PNG writer)
        std::string data;
        for (int i = 0; i < 6 * 4; ++i) data += static_cast<char>(i % 3);
        render::Image paletted = render::Image::frombytes("P", {6, 4}, data);
        paletted.putpalette("RGB", std::string("\xC8\x0A\x0A\x0A\xC8\x0A\x0A\x0A\xC8", 9));
        const std::string png = formats::png_bytes(paletted);
        const formats::Opened indexed = formats::open_file(png, "palette.png");
        const core::Page& page = indexed.doc.page(0);
        QVERIFY2(page.rgb && page.rgb->table, "an indexed page");
        QCOMPARE(page.rgb->table->colors.size(), std::size_t(3));
        QCOMPARE(page.native_source->mode, std::string("indexed"));
        QVERIFY(render::open_image(*page.layers[0].raster_png).convert("RGB").tobytes() == paletted.convert("RGB").tobytes());
        QCOMPARE(page.spec.grid, std::optional<std::string>("finest"));
        // (one bit of grey)
        const render::Image bits = render::Image::create("1", {8, 3}, render::Ink(0));
        const formats::Opened bitmap = formats::open_file(formats::png_bytes(bits), "bits.png");
        QVERIFY(bitmap.doc.page(0).color && bitmap.doc.page(0).color->bitmap);
        QCOMPARE(bitmap.doc.page(0).native_source->depth, 1);
        QCOMPARE(bitmap.doc.page(0).native_source->mode, std::string("bitmap"));
        // (their pixels as they are, in the layer the file is: not flattened into another)
        QCOMPARE(page.layers.size(), std::size_t(1));
        QCOMPARE(page.layers[0].title, std::string("背景"));
        QCOMPARE(bitmap.doc.page(0).layers.size(), std::size_t(1));
        QCOMPARE(bitmap.doc.page(0).layers[0].title, std::string("背景"));
        const core::InkPixels ones = core::decode_ink_raster(*bitmap.doc.page(0).layers[0].ink_raster);
        QCOMPARE(ones.width, 8u);
        QCOMPARE(ones.value(0, 0), 0.0);
        for (const auto& opened : {std::cref(indexed), std::cref(bitmap)}) {
            const fs::path dir = at(opened.get().doc.page(0).native_source->mode == "indexed" ? "indexed.genko" : "bitmap.genko");
            write_book(opened.get().doc, dir);
            QVERIFY(storage::load_document(dir).document.read_only_reason.empty());
        }
        // (a GIF of two frames: the first, said)
        const std::string gif = data_file("pyref/gif/animated-first-frame.gif");
        QVERIFY(codecs::read_metadata(codecs::Format::Gif, gif).frames > 1);
        const formats::Opened first = formats::open_file(gif, "anim.gif");
        QVERIFY(std::any_of(first.notes.begin(), first.notes.end(), [](const std::string& note) { return note.find("最初のコマ") != std::string::npos; }));
        const codecs::FileMetadata single = codecs::read_metadata(codecs::Format::Png, png);
        QCOMPARE(single.frames, 1);
    }

    // PC4 F-b: `genko new <dest> --from <file> [--as <ext>]` and `genko new <dest> --image …`: the book written at once,
    // {"ok", "path", "notes", "snapshot"}; what they do not take refused as usage.
    void genkoNewFromAFileAndAsAnImage() {
        const QString picture = QString::fromStdString(core::path_to_utf8(at("cli.png")));
        genko::test::write_bytes(picture, data_file("pyref/png/rgba8.png"));
        const QString book = QString::fromStdString(core::path_to_utf8(at("cli-from.genko")));
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--from"), picture});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const Json out = genko::test::one_line(made.out);
        QCOMPARE(out["ok"], Json(true));
        QVERIFY(out["notes"].is_array());
        QCOMPARE(out["snapshot"]["title"].get<std::string>(), std::string("cli"));
        const core::Document doc = read_book(fs::path(book.toStdString()));
        QVERIFY(doc.read_only_reason.empty());
        QCOMPARE(doc.page(0).native_source->format, std::string("png"));
        // (never over a book)
        QCOMPARE(genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--from"), picture}).exit_code, 1);
        // (--as: the bytes read as that format — a PNG read as a JPEG is refused)
        const auto as_jpeg = genko::test::run_genko({QStringLiteral("new"), QString::fromStdString(core::path_to_utf8(at("cli-as.genko"))), QStringLiteral("--from"),
                                                     picture, QStringLiteral("--as"), QStringLiteral("jpg")});
        QCOMPARE(as_jpeg.exit_code, 1);
        QCOMPARE(genko::test::one_line(as_jpeg.out)["code"].get<std::string>(), std::string("format"));
        QVERIFY(genko::test::run_genko({QStringLiteral("new"), QString::fromStdString(core::path_to_utf8(at("cli-as2.genko"))), QStringLiteral("--from"),
                                        picture, QStringLiteral("--as"), QStringLiteral("png")})
                    .exit_code == 0);
        // (a new image: 50 × 30 mm at 200 ppi, grey 16 bits, black)
        const QString image_book = QString::fromStdString(core::path_to_utf8(at("cli-image.genko")));
        const auto image = genko::test::run_genko({QStringLiteral("new"), image_book, QStringLiteral("--image"), QStringLiteral("--width"), QStringLiteral("50"),
                                                   QStringLiteral("--height"), QStringLiteral("30"), QStringLiteral("--unit"), QStringLiteral("mm"),
                                                   QStringLiteral("--dpi"), QStringLiteral("200"), QStringLiteral("--mode"), QStringLiteral("gray"),
                                                   QStringLiteral("--depth"), QStringLiteral("16"), QStringLiteral("--background"), QStringLiteral("black"),
                                                   QStringLiteral("--title"), QStringLiteral("灰色")});
        QVERIFY2(image.exit_code == 0, qPrintable(image.described()));
        const core::Document gray = read_book(fs::path(image_book.toStdString()));
        QCOMPARE(gray.title, std::string("灰色"));
        QVERIFY(gray.page(0).color && gray.page(0).color->space == core::InkSpace::Gray && gray.page(0).color->depth == 16);
        QCOMPARE(render::mm_to_px(gray.page(0).spec.width_mm.value(), 200), static_cast<int>(std::lround(50 / 25.4 * 200)));
        // (usage)
        for (const QStringList& wrong : {QStringList{QStringLiteral("--image")}, QStringList{QStringLiteral("--from"), picture, QStringLiteral("--pages"), QStringLiteral("2")},
                                         QStringList{QStringLiteral("--width"), QStringLiteral("10")},
                                         QStringList{QStringLiteral("--image"), QStringLiteral("--width"), QStringLiteral("10"), QStringLiteral("--height"),
                                                     QStringLiteral("10"), QStringLiteral("--unit"), QStringLiteral("ft")}}) {
            const auto refused = genko::test::run_genko(QStringList{QStringLiteral("new"), QString::fromStdString(core::path_to_utf8(at("cli-wrong.genko")))} + wrong);
            QVERIFY2(refused.exit_code == 2, qPrintable(wrong.join(QLatin1Char(' ')) + QStringLiteral(": ") + refused.described()));
        }
    }

    // PC4 F-b: 画像 → 複製… (formats::duplicate_page): the page as a book of its own — its layers as they are, the book's
    // pictures read from where they were —, or its merged picture alone; saved and read back.
    void aPageIsDuplicatedAsABook() {
        core::Document doc = run(small_book(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}},
                                                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"},
                                                                 {"png_base64", core::b64encode(render::write_png(render::Image::create("RGBA", {8, 8}, render::Ink{10, 200, 30, 255})))}},
                                                            Json{{"op", "add_page"}}}));
        const fs::path source = at("dup-source.genko");
        write_book(doc, source);
        const core::Document saved = read_book(source);
        const formats::Opened copy = formats::duplicate_page(saved, 0, "複製", false);
        QCOMPARE(copy.doc.title, std::string("複製"));
        QCOMPARE(copy.doc.pages.size(), std::size_t(1));
        QCOMPARE(copy.doc.page(0).index.value(), 1.0);
        QCOMPARE(copy.doc.page(0).layers.size(), saved.page(0).layers.size());
        QVERIFY(copy.doc.book_id != saved.book_id);
        QVERIFY(copy.doc.asset_dir && *copy.doc.asset_dir == *saved.asset_dir);
        const fs::path dir = at("dup.genko");
        write_book(copy.doc, dir);
        const core::Document back = read_book(dir);
        QVERIFY2(back.read_only_reason.empty(), back.read_only_reason.c_str());
        const int dpi = static_cast<int>(saved.page(0).spec.dpi.value());
        QVERIFY(formats::page_picture(back.page(0), back, dpi).image.data() == formats::page_picture(saved.page(0), saved, dpi).image.data());
        // (its merged picture alone: one layer, the same picture)
        const formats::Opened merged = formats::duplicate_page(saved, 0, "統合", true);
        QCOMPARE(merged.doc.page(0).layers.size(), std::size_t(1));
        QCOMPARE(merged.doc.page(0).spec.grid, std::optional<std::string>("finest"));
        QVERIFY(formats::page_picture(merged.doc.page(0), merged.doc, dpi).image.data() == formats::page_picture(saved.page(0), saved, dpi).image.data());
    }

    // PC4 F-b: the paper made again (set_page_spec, set_meta's preset) keeps image-grid@1's grid.
    void thePaperMadeAgainKeepsTheGrid() {
        const formats::Opened opened = formats::open_file(data_file("pyref/png/rgb8.png"), "grid.png");
        QCOMPARE(opened.doc.spec.grid, std::optional<std::string>("finest"));
        const core::Document respec = run(opened.doc, Json::array({Json{{"op", "set_page_spec"}, {"paper", Json::array({100, 80})}, {"trim", Json::array({90, 70})}}}));
        QCOMPARE(respec.spec.grid, std::optional<std::string>("finest"));
        QCOMPARE(respec.page(0).spec.grid, std::optional<std::string>("finest"));
        const core::Document preset = run(opened.doc, Json::array({Json{{"op", "set_meta"}, {"preset", "shueisha"}}}));
        QCOMPARE(preset.page(0).spec.grid, std::optional<std::string>("finest"));
        // (a book without it: none, as before)
        QVERIFY(!run(small_book(), Json::array({Json{{"op", "set_page_spec"}, {"paper", Json::array({100, 80})}, {"trim", Json::array({90, 70})}}})).spec.grid);
    }

    // PC4 F-b review P3-3: a 16-bit TIFF of an orientation (tag 274) opened turned as it says (its reader does not turn
    // it; the 8-bit one does), and said.
    void aDeepTiffIsTurnedByItsOrientation() {
        // (grey, 16 bits, 3 × 2, uncompressed, one strip; orientation 6: turned 90° clockwise to be upright)
        const auto le16 = [](std::string& out, unsigned v) {
            out += static_cast<char>(v & 0xFF);
            out += static_cast<char>((v >> 8) & 0xFF);
        };
        const auto le32 = [](std::string& out, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        std::string pixels;
        for (const unsigned v : {0u, 10000u, 20000u, 30000u, 40000u, 50000u}) le16(pixels, v);
        struct Entry {
            unsigned tag, type;
            std::uint32_t value;
        };
        const std::vector<Entry> entries{{256, 3, 3}, {257, 3, 2}, {258, 3, 16}, {259, 3, 1}, {262, 3, 1}, {273, 4, 8},
                                         {274, 3, 6}, {277, 3, 1}, {278, 3, 2},  {279, 4, static_cast<std::uint32_t>(pixels.size())}};
        std::string tiff = "II";
        le16(tiff, 42);
        le32(tiff, static_cast<std::uint32_t>(8 + pixels.size()));
        tiff += pixels;
        le16(tiff, static_cast<unsigned>(entries.size()));
        for (const Entry& e : entries) {
            le16(tiff, e.tag);
            le16(tiff, e.type);
            le32(tiff, 1);
            if (e.type == 3) {
                le16(tiff, e.value);
                le16(tiff, 0);
            } else {
                le32(tiff, e.value);
            }
        }
        le32(tiff, 0);
        const formats::Opened opened = formats::open_file(tiff, "turned.tif");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.color && page.color->depth == 16);
        const core::InkPixels inks = core::decode_ink_raster(*page.layers[0].ink_raster);
        QCOMPARE(inks.width, 2u);
        QCOMPARE(inks.height, 3u);
        // (turned clockwise: the first row is the first column read from the bottom up)
        QVERIFY(std::abs(inks.value(0, 0) - 30000.0 / 65535.0) < 1e-4);
        QVERIFY(std::abs(inks.value(1, 0) - 0.0) < 1e-4);
        QVERIFY(std::any_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("TIFF の向き") != std::string::npos; }));
    }

    // PC4 F-b (image-grid@1, design 8-2 (b); review P2-1・P2-2・P2-3): on the page of a picture opened at 300 ppi the work is
    // done at its pixels' grid — a new paint layer, a pen layer's lines baked by a filter, an area deleted or moved on the
    // picture's layer, the layers merged, the masks' and selections' grid — and at a 72-ppi picture's the area is its own;
    // a book without the mark as before.
    void theGridOfAnOpenedPicture() {
        // (300 × 200 px at 300 ppi: 25.4 × 16.93 mm; white with a black square at its top left, 60 × 60 px)
        std::string rgb(300 * 200 * 3, static_cast<char>(255));
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x)
                for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>((y * 300 + x) * 3 + c)] = 0;
        const render::Image picture = render::Image::frombytes("RGB", {300, 200}, rgb);
        const std::string png = formats::png_bytes(picture, formats::PngSave{std::array<double, 2>{300, 300}, {}, false});
        const core::Document doc = formats::open_file(png, "grid.png").doc;
        const core::Page& page = doc.page(0);
        const std::string back = page.layers[0].id;
        QCOMPARE(render::layer_masks::grid_dpi(page), 300);
        QCOMPARE(render::selection::grid_dpi(page), 300);
        QCOMPARE(render::raster::working_dpi(page), 300);
        const auto layer = [](const core::Document& d, const std::string& id) -> const core::Layer& {
            for (const core::Layer& l : d.page(0).layers) {
                if (l.id == id) return l;
            }
            throw std::runtime_error("no layer " + id);
        };
        const auto alpha = [](const render::Image& image, int x, int y) { return image.convert("RGBA").getpixel(x, y)[3]; };
        const auto grey = [](const render::Image& image, int x, int y) { return image.convert("RGBA").getpixel(x, y)[0]; };
        // (a new paint layer: the page's grid)
        core::Layer fresh;
        QCOMPARE(render::raster::ensure_raster(page, fresh).size(), (render::Size{300, 200}));
        // (a pen layer's line, 5 to 20 mm across at 8 mm down, baked by a filter: where it was drawn, at 300 dpi)
        const core::Document lined = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "ink"}, {"title", "線"}},
                                                           Json{{"op", "set_layer"}, {"page", 1}, {"id", "ink"}, {"panel_each", false}, {"panel_clip", false}},
                                                           Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "ink"},
                                                                {"points", Json::array({Json::array({5, 8}), Json::array({20, 8})})}, {"width_mm", 1},
                                                                {"rgb", Json::array({0, 0, 0})}}}));
        const core::Document filtered =
            run(lined, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "ink"}, {"kind", "blur_gaussian"}, {"radius", 0.5}}}));
        const core::Layer& baked = layer(filtered, "ink");
        QVERIFY(baked.raster_png && !baked.raster_png->empty());
        const render::Image line = render::open_image(*baked.raster_png);
        QCOMPARE(line.size(), (render::Size{300, 200}));
        QVERIFY2(alpha(line, 150, 94) > 128, "the line where it was drawn");
        QCOMPARE(alpha(line, 100, 63), 0.0);  // (not drawn at 200 dpi into the 300-dpi picture: 2/3 of the way)
        // (an area deleted on the picture's layer: its left half, at the picture's pixels)
        const core::Document deleted =
            run(doc, Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", back}, {"area", Json{{"rect", Json::array({0, 0, 12.7, 16.9333})}}}}}));
        const render::Image left = render::raster::layer_pixels(deleted.page(0), layer(deleted, back));
        QCOMPARE(left.size(), (render::Size{300, 200}));
        QCOMPARE(alpha(left, 140, 100), 0.0);
        QCOMPARE(alpha(left, 160, 100), 255.0);
        // (the square moved 10 mm right: the same size, where it was moved)
        const core::Document moved = run(doc, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", back},
                                                                 {"area", Json{{"rect", Json::array({0, 0, 5.08, 5.08})}}},
                                                                 {"matrix", Json::array({1, 0, 0, 1, 10, 0})}}}));
        const render::Image shifted = render::raster::layer_pixels(moved.page(0), layer(moved, back));
        QCOMPARE(shifted.size(), (render::Size{300, 200}));
        QVERIFY2(grey(shifted, 150, 30) < 60 && alpha(shifted, 150, 30) > 200, "the square where it was moved");
        QVERIFY2(grey(shifted, 190, 30) > 200, "the square not grown");
        QCOMPARE(alpha(shifted, 30, 30), 0.0);
        // (merged: the picture's grid kept)
        const core::Document painted = run(lined, Json::array({Json{{"op", "merge_visible"}, {"page", 1}}}));
        for (const core::Layer& l : painted.page(0).layers) {
            if (l.raster_png && !l.raster_png->empty()) QCOMPARE(render::open_image(*l.raster_png).size(), (render::Size{300, 200}));
        }
        const core::Document down = run(lined, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", "ink"}}}));
        QCOMPARE(render::open_image(*layer(down, back).raster_png).size(), (render::Size{300, 200}));
        QVERIFY2(alpha(render::open_image(*layer(down, back).raster_png), 150, 94) > 128 &&
                     grey(render::open_image(*layer(down, back).raster_png), 150, 94) < 128,
                 "the line merged where it was drawn");
        // (a 72-ppi picture: its new layers at 200 dpi — never coarser —, its own layer's area at its 72 ppi)
        const render::Image small = render::Image::create("RGB", {72, 72}, render::Ink{255, 255, 255});
        const core::Document coarse = formats::open_file(formats::png_bytes(small, formats::PngSave{std::array<double, 2>{72, 72}, {}, false}), "coarse.png").doc;
        QCOMPARE(render::raster::working_dpi(coarse.page(0)), 200);
        QCOMPARE(render::raster::ensure_raster(coarse.page(0), fresh).size(), (render::Size{200, 200}));
        const std::string coarse_back = coarse.page(0).layers[0].id;
        const core::Document half = run(coarse, Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", coarse_back},
                                                                  {"area", Json{{"rect", Json::array({0, 0, 12.7, 25.4})}}}}}));
        const render::Image kept = render::raster::layer_pixels(half.page(0), layer(half, coarse_back));
        QCOMPARE(kept.size(), (render::Size{72, 72}));
        QCOMPARE(alpha(kept, 30, 36), 0.0);
        QCOMPARE(alpha(kept, 42, 36), 255.0);
        // (a book without the mark: 200 dpi, as before)
        const core::Document plain = small_book();
        QCOMPARE(render::raster::working_dpi(plain.page(0)), 200);
        QCOMPARE(render::raster::ensure_raster(plain.page(0), fresh).width(), render::mm_to_px(plain.page(0).spec.width_mm.value(), 200));
    }

    void theFilesOpsAreFrames() {
        // (PC4 F-b wrote set_file_info: test_contract_file_info)
        for (const auto& [op, group] : {std::pair{"set_camera_raw", "PC4 F-e"}}) {
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
