// PC4 F-a (SPEC.md FILE-01, the codecs) as a person or an agent uses them: `genko render --out` writes the extensions
// Pillow's rarer writers wrote (GIF, PPM, PGM, PBM, TGA, ICO, QOI, AVIF) as one 8-bit picture of the page — read back by
// the codecs, an icon in its sizes up to 256 —, an extension it does not know refused as before; smart objects of the new
// formats (formats@1: an OpenEXR of floats and of halves, a Radiance HDR, an AVIF, a TGA, an ICO, a QOI, a PAM of 16 bits)
// placed and drawn on an 8-bit page, pages of 16 and 32 bits, a grey and a CMYK page — the lossless ones as the PNG of their
// pixels, OpenEXR's linear light with sRGB's curve on it (past 1 kept on the 32-bit page), a 16-bit PAM's inks as a 16-bit
// TIFF's —, formats@1 named while such a smart object is in the book and not before or after; a file of each opened as a
// book; コピーを保存… in each format with the book's XMP where the format holds it; `genko --version` naming the
// libraries. (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/file_info.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/codecs/codecs.hpp"
#include "render/image.hpp"
#include "render/inspect.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace formats = genko::formats;
namespace codecs = genko::render::codecs;
namespace storage = genko::storage;
namespace fs = std::filesystem;
using codecs::CodecImage;
using codecs::Format;
using codecs::Layout;
using codecs::Sample;
using core::Json;

namespace {

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    spec.expression = "color";  // (a colour page: its smart objects' colours as they are, not a mono page's finish)
    return core::new_episode("codecs", core::Num(1), 1, spec, core::Binding::Right);
}

core::Document run(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = doc.revision;  // (0 for a new book; the revision a book read back was read at)
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

Json project(const fs::path& dir) { return Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")))); }

bool has_feature(const Json& saved, std::string_view feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

std::string code_of(const std::function<void()>& run_it) {
    try {
        run_it();
    } catch (const render::NotYetPorted& error) {
        return "not_yet_ported:" + error.element();
    } catch (const core::Error& error) {
        return error.code();
    }
    return {};
}

// One colour, `w` × `h`, in a layout and sample (values 0..1, past 1 for floats).
CodecImage filled(std::uint32_t w, std::uint32_t h, Layout layout, Sample sample, std::vector<float> colour) {
    std::vector<float> values;
    values.reserve(std::size_t(w) * h * colour.size());
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) values.insert(values.end(), colour.begin(), colour.end());
    return CodecImage::from_normalized(w, h, layout, sample, values);
}

// A picture of a few colours in blocks (lossless formats are compared with the PNG of the same pixels).
CodecImage blocks(std::uint32_t w, std::uint32_t h) {
    std::vector<float> values;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool left = x < w / 2, top = y < h / 2;
            values.push_back(left ? 0.9f : 0.1f);
            values.push_back(top ? 0.2f : 0.7f);
            values.push_back(left == top ? 0.5f : 1.0f);
            values.push_back(1.0f);
        }
    }
    return CodecImage::from_normalized(w, h, Layout::Rgba, Sample::U8, values);
}

// The page with a smart object of this file over it all.
core::Document with_contents(const core::Document& doc, const std::string& bytes, const std::string& name) {
    const core::Page& page = doc.page(0);
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    return run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(bytes)}, {"name", name},
                                      {"placement_mm", Json::array({0, 0, w, h})}, {"clip_to", "none"}}}));
}

Json middle(const core::Document& doc) {
    const core::Page& page = doc.page(0);
    return render::pixel_at(page, &doc, page.spec.width_mm.value() / 2, page.spec.height_mm.value() / 2, std::nullopt, 40);
}

// sRGB's 8-bit value of a linear light.
int srgb8(double linear) { return static_cast<int>(std::lround((linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055) * 255)); }

bool near(const Json& srgb, std::array<int, 3> want, int tolerance) {
    for (std::size_t c = 0; c < 3; ++c) {
        if (std::abs(srgb[c].get<int>() - want[c]) > tolerance) return false;
    }
    return true;
}

}  // namespace

class TestContractCodecs : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const char* name) const { return fs::path(tmp_.path().toStdString()) / name; }
    QString qat(const QString& name) const { return tmp_.path() + QStringLiteral("/") + name; }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // `genko render --out x.<ext>`: the extensions Pillow's rarer writers wrote, through the codecs — the page's picture
    // (the lossless ones its pixels, GIF and AVIF close to them), an icon its sizes up to 256, its JSON as for the others.
    void renderWritesTheNewExtensions() {
        const QString book = qat(QStringLiteral("render.genko"));
        QCOMPARE(genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")}).exit_code, 0);
        const auto drawn = [&](const QString& ext) {
            const QString out = qat(QStringLiteral("page.") + ext);
            const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                     QStringLiteral("20"), QStringLiteral("--out"), out});
            return std::make_pair(ran, out);
        };
        const auto [png_ran, png_out] = drawn(QStringLiteral("png"));
        QCOMPARE(png_ran.exit_code, 0);
        const render::Image page = render::open_image(genko::test::read_bytes(png_out)).convert("RGB");
        for (const char* ext : {"gif", "ppm", "pgm", "pbm", "tga", "ico", "qoi", "avif"}) {
            const auto [ran, out] = drawn(QString::fromLatin1(ext));
            QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
            const Json reply = genko::test::one_line(ran.out);
            QVERIFY2(reply.value("ok", false) && reply.contains("path") && reply.contains("mode"), reply.dump().c_str());
            const std::string bytes = genko::test::read_bytes(out);
            const auto format = codecs::detect(bytes);
            QVERIFY2(format && *format == *codecs::from_extension(ext), ext);
            const CodecImage back = codecs::decode(bytes);
            if (std::string_view(ext) == "ico") {
                // (Pillow's sizes the page is at least as large as, each fitted in its square: the largest read)
                QVERIFY2(back.width() <= 256 && back.height() <= 256 && std::max(back.width(), back.height()) >= 128, ext);
                continue;
            }
            QVERIFY2(int(back.width()) == page.width() && int(back.height()) == page.height(), ext);
            const render::Image read = codecs::eight_bit(back, false).convert("RGB");
            if (std::string_view(ext) == "gif" || std::string_view(ext) == "avif") {
                const std::string a = read.tobytes(), b = page.tobytes();
                double sum = 0;
                for (std::size_t i = 0; i < a.size(); ++i) sum += std::pow((static_cast<unsigned char>(a[i]) - static_cast<unsigned char>(b[i])) / 255.0, 2);
                QVERIFY2(10 * std::log10(a.size() / std::max(sum, 1e-12)) > 28, ext);
            } else {
                QVERIFY2(read.tobytes() == page.tobytes(), ext);  // (PPM, PGM and PBM alike: P6 of the RGB page, as Pillow writes it)
            }
        }
        // (an extension no writer has: refused as before; one of Pillow's writers this build has not: said so)
        QCOMPARE(drawn(QStringLiteral("exr")).first.exit_code, 1);
        QCOMPARE(genko::test::one_line(drawn(QStringLiteral("exr")).first.out).value("code", std::string()), std::string("value"));
        const auto [jp2, jp2_out] = drawn(QStringLiteral("jp2"));
        QVERIFY(jp2.exit_code != 0);
        QVERIFY(!fs::exists(core::path_from_utf8(jp2_out.toStdString())));
    }

    // Smart objects of the new formats on an 8-bit page: the lossless ones drawn as the PNG of their pixels; an OpenEXR's
    // and a Radiance HDR's linear light with sRGB's curve on it; an AVIF close to its colour. Their contents record their
    // format, suffix, depth and colours.
    void smartObjectsDrawnOnAnEightBitPage() {
        const CodecImage pixels = blocks(40, 30);
        const core::Document base = small_book();
        const core::Document as_png = with_contents(base, codecs::encode(pixels, Format::Png), "a.png");
        const render::Image want = render::render_page(as_png.page(0), 40, render::RenderOptions{}, &as_png).image;
        for (const auto& [f, name] : {std::make_pair(Format::Tga, "a.tga"), std::make_pair(Format::Ico, "a.ico"), std::make_pair(Format::Qoi, "a.qoi"),
                                      std::make_pair(Format::Pnm, "a.pam"), std::make_pair(Format::Bmp, "a.bmp")}) {
            const core::Document doc = with_contents(base, codecs::encode(pixels, f), name);
            QCOMPARE(doc.smart_contents.size(), std::size_t(1));
            const core::SmartContents& c = *doc.smart_contents.begin()->second;
            QCOMPARE(c.suffix, "." + std::string(codecs::extensions(f).front()));  // (its format's first suffix, as a .jpeg's .jpg)
            QCOMPARE(c.width_px, std::int64_t(40));
            render::clear_render_caches();
            QVERIFY2(render::render_page(doc.page(0), 40, render::RenderOptions{}, &doc).image.tobytes() == want.tobytes(), name);
        }
        // (an OpenEXR of floats and of halves, a Radiance HDR: linear (0.2, 0.5, 0.8) shown as sRGB's (124, 188, 231))
        const std::array<int, 3> shown{srgb8(0.2), srgb8(0.5), srgb8(0.8)};
        QCOMPARE(shown, (std::array<int, 3>{124, 188, 231}));
        const CodecImage light = filled(16, 16, Layout::Rgb, Sample::F32, {0.2f, 0.5f, 0.8f});
        for (const auto& [bytes, name, depth] : {std::make_tuple(codecs::encode(light, Format::OpenExr), "f.exr", 32),
                                                 std::make_tuple(codecs::encode(light.convert(Layout::Rgb, Sample::F16), Format::OpenExr), "h.exr", 16),
                                                 std::make_tuple(codecs::encode(light, Format::Hdr), "r.hdr", 32)}) {
            const core::Document doc = with_contents(base, bytes, name);
            const core::SmartContents& c = *doc.smart_contents.begin()->second;
            QCOMPARE(c.depth, depth);
            QCOMPARE(c.mode, std::string("rgb"));
            render::clear_render_caches();
            const Json value = middle(doc);
            QVERIFY2(near(value["srgb"], shown, 2), (std::string(name) + " " + value.dump()).c_str());
        }
        // (an AVIF: its colour within the codec's loss)
        const core::Document avif = with_contents(base, codecs::encode(filled(32, 32, Layout::Rgb, Sample::U8, {30 / 255.f, 160 / 255.f, 90 / 255.f}), Format::Avif), "c.avif");
        render::clear_render_caches();
        QVERIFY2(near(middle(avif)["srgb"], {30, 160, 90}, 4), middle(avif).dump().c_str());
    }

    // On pages of 16 and 32 bits an OpenEXR keeps its bits (past 1 on the 32-bit page, cut at 1 on the 16-bit one); on a
    // grey page of 16 bits a PAM of 16-bit grey gives the inks a 16-bit TIFF of the same samples gives; on a CMYK page an
    // OpenEXR and an AVIF are drawn (their colours through the page's profile).
    void smartObjectsDrawnOnDeepAndInkPages() {
        const CodecImage light = filled(16, 16, Layout::Rgb, Sample::F32, {0.2f, 0.5f, 0.8f});
        const CodecImage bright = filled(16, 16, Layout::Rgb, Sample::F32, {2.0f, 0.5f, 0.0f});
        for (const int depth : {16, 32}) {
            const core::Document page = run(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}}));
            render::clear_render_caches();
            const Json value = middle(with_contents(page, codecs::encode(light, Format::OpenExr), "l.exr"));
            QCOMPARE(value["depth"], Json(depth));
            const std::array<double, 3> encoded{0.4845, 0.7354, 0.9063};
            for (std::size_t c = 0; c < 3; ++c) QVERIFY2(std::abs(value["values"][c].get<double>() - encoded[c]) < 0.003, value.dump().c_str());
            render::clear_render_caches();
            const Json hdr = middle(with_contents(page, codecs::encode(bright, Format::OpenExr), "b.exr"));
            const double red = hdr["values"][0].get<double>();
            // (2.0 with sRGB's curve, 1.3532: mixed as it is on both, stored past 1 on the 32-bit page, at 1 on the 16-bit one)
            QVERIFY2(std::abs(red - 1.3532) < 0.01, hdr.dump().c_str());
            if (depth == 16) QCOMPARE(hdr["stored"][0], Json(65535));
        }
        // (a grey page of 16 bits: a 16-bit PAM's samples as a 16-bit TIFF's)
        std::vector<float> ramp(24 * 24);
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>((i * 977) % 65536) / 65535.0f;
        const CodecImage grey = CodecImage::from_normalized(24, 24, Layout::Gray, Sample::U16, ramp);
        const core::Document gray_page = run(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 16}}}));
        render::clear_render_caches();
        const core::Document from_tiff = with_contents(gray_page, codecs::encode(grey, Format::Tiff), "g.tif");
        const core::Document from_pam = with_contents(gray_page, codecs::encode(grey, Format::Pnm), "g.pam");
        QCOMPARE(from_pam.smart_contents.begin()->second->depth, 16);
        for (const double x : {5.0, 22.0, 41.0}) {
            render::clear_render_caches();
            const Json t = render::pixel_at(from_tiff.page(0), &from_tiff, x, 40, std::nullopt, 40);
            render::clear_render_caches();
            const Json p = render::pixel_at(from_pam.page(0), &from_pam, x, 40, std::nullopt, 40);
            QVERIFY2(t["stored"] == p["stored"], (t.dump() + " " + p.dump()).c_str());
        }
        // (a CMYK page: drawn, the colours near the 8-bit page's)
        const core::Document cmyk_page = run(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"depth", 8}, {"profile", "coated-cmyk"}}}));
        render::clear_render_caches();
        const Json exr = middle(with_contents(cmyk_page, codecs::encode(light, Format::OpenExr), "l.exr"));
        QVERIFY2(exr["values"].size() == 4 && near(exr["srgb"], {124, 188, 231}, 30), exr.dump().c_str());
        render::clear_render_caches();
        const Json avif = middle(with_contents(cmyk_page, codecs::encode(filled(32, 32, Layout::Rgb, Sample::U8, {30 / 255.f, 160 / 255.f, 90 / 255.f}), Format::Avif), "c.avif"));
        QVERIFY2(avif["values"].size() == 4 && near(avif["srgb"], {30, 160, 90}, 30), avif.dump().c_str());
    }

    // formats@1 is named while the book's smart objects hold a file of a new format, and not before or after; a book of
    // PC3's formats only names none of PC4's.
    void formatsNamedOnlyWhileUsed() {
        const core::Document plain = with_contents(small_book(), codecs::encode(blocks(8, 8), Format::Png), "a.png");
        write_book(plain, at("plain.genko"));
        Json saved = project(at("plain.genko"));
        QVERIFY(has_feature(saved, core::kSmartObjectsFeature) && !has_feature(saved, core::kFormatsFeature));
        const core::Document exr = with_contents(small_book(), codecs::encode(filled(8, 8, Layout::Rgb, Sample::F32, {0.5f, 0.5f, 0.5f}), Format::OpenExr), "a.exr");
        write_book(exr, at("exr.genko"));
        saved = project(at("exr.genko"));
        QVERIFY(has_feature(saved, core::kFormatsFeature));
        const Json contents = saved["smart_contents"].begin().value();
        QCOMPARE(contents["format"], Json("exr"));
        QCOMPARE(contents["suffix"], Json(".exr"));
        // (read back: drawn the same)
        const core::Document back = storage::load_document(at("exr.genko")).document;
        QVERIFY(back.read_only_reason.empty());
        render::clear_render_caches();
        QCOMPARE(middle(back)["srgb"], middle(exr)["srgb"]);
        // (the smart object gone: so is the name)
        const auto so = std::find_if(back.page(0).layers.begin(), back.page(0).layers.end(),
                                     [&](const core::Layer& l) { return core::smart::contents_of(back, l) != nullptr; });
        QVERIFY(so != back.page(0).layers.end());
        write_book(run(back, Json::array({Json{{"op", "delete_layer"}, {"page", 1}, {"id", so->id}}})), at("exr.genko"));
        QVERIFY(!has_feature(project(at("exr.genko")), core::kFormatsFeature));
    }

    // A file of each new format opened as a book (開く…): its pixels at its depth — an OpenEXR's RGB light on a page of
    // 32 bits in the built-in linear-srgb, its grey light with sRGB's curve in sgray; a PAM of 16 bits on a 16-bit page —,
    // an AVIF's EXIF and XMP kept as the page's and its File Info.
    void openedAsABook() {
        const std::string& linear = *core::builtin_profile("linear-srgb")->bytes;
        const formats::Opened exr = formats::open_file(codecs::encode(filled(10, 8, Layout::Rgb, Sample::F32, {0.2f, 2.0f, 0.0f}), Format::OpenExr), "light.exr");
        const core::Page& page = exr.doc.page(0);
        QVERIFY(page.rgb && page.rgb->depth == 32 && page.rgb->icc && *page.rgb->icc == linear);
        QCOMPARE(page.native_source->format, std::string("exr"));
        const Json pixel = render::pixel_at(page, &exr.doc, 1, 1, page.layers[0].id, 72);
        QVERIFY2(std::abs(pixel["values"][1].get<double>() - 2.0) < 1e-6, pixel.dump().c_str());  // (its light as it is, past 1)
        const formats::Opened grey = formats::open_file(codecs::encode(filled(10, 8, Layout::Gray, Sample::F32, {0.5f}), Format::OpenExr), "grey.exr");
        QVERIFY(grey.doc.page(0).color && grey.doc.page(0).color->space == core::InkSpace::Gray && grey.doc.page(0).color->depth == 32);
        QVERIFY(*grey.doc.page(0).color->icc == *core::builtin_profile("sgray")->bytes);
        const formats::Opened pam = formats::open_file(codecs::encode(filled(10, 8, Layout::Rgb, Sample::U16, {0.1f, 0.2f, 0.3f}), Format::Pnm), "deep.ppm");
        QVERIFY(pam.doc.page(0).rgb && pam.doc.page(0).rgb->depth == 16);
        QCOMPARE(pam.doc.page(0).native_source->format, std::string("pnm"));
        CodecImage tagged = filled(12, 10, Layout::Rgb, Sample::U8, {0.5f, 0.5f, 0.5f});
        core::FileInfo info;
        info.author = "作者";
        info.copyright = "© 2026";
        tagged.meta.xmp = core::write_xmp(std::nullopt, info, "AVIF");
        const formats::Opened avif = formats::open_file(codecs::encode(tagged, Format::Avif), "tagged.avif");
        QVERIFY(avif.doc.file_info && avif.doc.file_info->author == info.author && avif.doc.file_info->copyright == info.copyright);
        QVERIFY(!avif.doc.page(0).native_source->xmp.empty());
        for (const auto& [f, name] : {std::make_pair(Format::Tga, "a.tga"), std::make_pair(Format::Ico, "a.ico"), std::make_pair(Format::Qoi, "a.qoi"),
                                      std::make_pair(Format::Hdr, "a.hdr")}) {
            const formats::Opened opened = formats::open_file(codecs::encode(blocks(20, 10), f), name);
            QCOMPARE(opened.doc.page(0).native_source->format, std::string(codecs::id_of(f)));
            QCOMPARE(opened.doc.page(0).native_source->width_px, std::int64_t(20));
        }
    }

    // コピーを保存… in each format the codecs write: the page's picture at its resolution, the book's XMP where the format
    // holds it (and said lost where not), an OpenEXR's light linear (the white paper 1.0), an icon of a page past 256 refused.
    void aCopyInEachFormat() {
        core::FileInfo info;
        info.author = "書いた人";
        const core::Document doc = run(small_book(), Json::array({Json{{"op", "set_file_info"}, {"author", "書いた人"}}}));
        const int w = render::mm_to_px(60, 150), h = render::mm_to_px(80, 150);
        for (const char* ext : {"webp", "gif", "bmp", "tga", "pnm", "pbm", "qoi", "exr", "hdr", "avif", "png", "jpg"}) {
            const fs::path dest = at((std::string("copy.") + ext).c_str());
            formats::CopyResult result;
            QVERIFY2(code_of([&] { result = formats::write_copy(doc, 0, dest); }).empty(), ext);
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dest)));
            const CodecImage back = codecs::decode(bytes);
            QVERIFY2(int(back.width()) == w && int(back.height()) == h, ext);
            const bool keeps_xmp = codecs::caps(*codecs::from_extension(ext)).xmp;
            QVERIFY2(back.meta.xmp.has_value() == keeps_xmp, ext);
            if (keeps_xmp) QCOMPARE(core::file_info_from_xmp(*back.meta.xmp).author, info.author);
            const bool said = std::any_of(result.notes.begin(), result.notes.end(), [](const std::string& n) { return n.find("XMP") != std::string::npos; });
            QVERIFY2(said == !keeps_xmp, ext);
            if (std::string_view(ext) == "exr") {
                QCOMPARE(back.sample(), Sample::F32);
                QVERIFY2(std::abs(back.at(0, 0, 0) - 1.0f) < 1e-4, std::to_string(back.at(0, 0, 0)).c_str());  // (the white paper)
            }
        }
        QCOMPARE(code_of([&] { (void)formats::write_copy(doc, 0, at("copy.ico")); }), std::string("image_too_large"));
        QVERIFY(!fs::exists(at("copy.ico")));
    }

    void versionNamesTheLibraries() {
        const auto ran = genko::test::run_genko({QStringLiteral("--version")});
        QCOMPARE(ran.exit_code, 0);
        const Json build = genko::test::one_line(ran.out)["build"];
        QCOMPARE(build["openexr"], Json(codecs::openexr_version()));
        QCOMPARE(build["libavif"], Json(codecs::libavif_version()));
        QCOMPARE(build["aom"], Json(codecs::aom_version()));
    }
};

QTEST_GUILESS_MAIN(TestContractCodecs)
#include "test_contract_codecs.moc"
