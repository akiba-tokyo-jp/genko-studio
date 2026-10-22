// PC4 F-b (SPEC.md FILE-01, file.fileInfo): ファイル情報… — the book's File Info (file-info@1) written by the op
// set_file_info, kept in the book and read back; its eight XMP items written into the packet a picture was opened with,
// the packet's other properties kept as they were (PhotoCraft's file_cmds.rs read_file_info / write_file_info); the
// packet in the files the exports and コピーを保存… write (PNG, JPEG, TIFF, PSD) read back by this build's reader and by
// the reference's Pillow and psd-tools; a picture's EXIF and XMP (PNG, JPEG, WebP) kept when it is opened, its EXIF
// orientation applied to its pixels and set upright; `genko inspect --file-info`. (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <string>

#include "core/command_bus.hpp"
#include "core/file_info.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/file_xmp.hpp"
#include "formats/open_file.hpp"
#include "formats/pillow_save.hpp"
#include "render/codecs/metadata.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
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
using core::Json;

namespace {

void put32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xFF);
}
void le16(std::string& out, std::uint32_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}
void le32(std::string& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((v >> shift) & 0xFF);
}

// An EXIF block (little-endian TIFF structure): its orientation, and an Exif IFD saying sRGB when `srgb`.
std::string exif_block(int orientation, bool srgb = false) {
    std::string out("II*\0", 4);
    le32(out, 8);
    le16(out, srgb ? 2 : 1);
    le16(out, 0x0112);
    le16(out, 3);
    le32(out, 1);
    le16(out, static_cast<std::uint32_t>(orientation));
    le16(out, 0);
    if (srgb) {
        le16(out, 0x8769);
        le16(out, 4);
        le32(out, 1);
        le32(out, 8 + 2 + 2 * 12 + 4);  // (the Exif IFD right after this one)
    }
    le32(out, 0);
    if (srgb) {
        le16(out, 1);
        le16(out, 0xA001);
        le16(out, 3);
        le32(out, 1);
        le16(out, 1);
        le16(out, 0);
        le32(out, 0);
    }
    return out;
}

// A PNG chunk inserted before the first IDAT.
std::string with_chunk(std::string png, const char* type, const std::string& data) {
    const std::size_t at = png.find("IDAT") - 4;
    std::string chunk;
    put32(chunk, static_cast<std::uint32_t>(data.size()));
    chunk += type;
    chunk += data;
    put32(chunk, 0);  // (the readers here do not check the CRC)
    png.insert(at, chunk);
    return png;
}

// A JPEG APP1 segment after SOI and the JFIF header.
std::string with_app1(std::string jpeg, const std::string& data) {
    std::size_t at = 2;
    if (static_cast<unsigned char>(jpeg[3]) == 0xE0) at += 2 + ((std::size_t(static_cast<unsigned char>(jpeg[4])) << 8) | static_cast<unsigned char>(jpeg[5]));
    std::string segment = "\xFF\xE1";
    segment += static_cast<char>(((data.size() + 2) >> 8) & 0xFF);
    segment += static_cast<char>((data.size() + 2) & 0xFF);
    jpeg.insert(at, segment + data);
    return jpeg;
}

// A picture of distinct pixels (each pixel its own colour, so a turn shows).
render::Image distinct(int w, int h) {
    std::string rgb;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            rgb += static_cast<char>(x * 40 + 5);
            rgb += static_cast<char>(y * 40 + 5);
            rgb += static_cast<char>((x + y) * 10);
        }
    return render::Image::frombytes("RGB", render::Size{w, h}, rgb);
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("情報の本", core::Num(1), 1, spec, core::Binding::Right);
}

core::Document run(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
}

core::ApplyResult run_result(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    request.assets_from = doc.asset_dir;
    storage::Saver(lock).save(doc, request);
}

Json project(const fs::path& dir) { return Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")))); }

bool has_feature(const Json& saved, std::string_view feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

// A PSD's image resource 1060 (its XMP), read straight from its bytes.
std::optional<std::string> psd_xmp(const std::string& psd) {
    const auto be = [&](std::size_t at) {
        return (std::uint32_t(static_cast<unsigned char>(psd[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(psd[at + 1])) << 16) |
               (std::uint32_t(static_cast<unsigned char>(psd[at + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(psd[at + 3]));
    };
    std::size_t at = 26;
    at += 4 + be(at);
    const std::size_t end = at + 4 + be(at);
    at += 4;
    while (at + 12 <= end) {
        const unsigned id = (static_cast<unsigned char>(psd[at + 4]) << 8) | static_cast<unsigned char>(psd[at + 5]);
        const std::size_t name = static_cast<unsigned char>(psd[at + 6]);
        std::size_t data = at + 6 + ((1 + name + 1) & ~std::size_t(1));
        const std::uint32_t size = be(data);
        data += 4;
        if (id == 1060) return psd.substr(data, size);
        at = data + size + (size & 1);
    }
    return std::nullopt;
}

core::FileInfo some_info() {
    core::FileInfo info;
    info.author = "山田 花子";
    info.author_title = "漫画家";
    info.description = "説明 <&> \"引用\"";
    info.keywords = {"漫画", "テスト"};
    info.copyright = "© 2026 山田";
    info.copyright_status = "copyrighted";
    info.copyright_url = "https://example.com/rights";
    return info;
}

Json info_ops(const std::string& title, const core::FileInfo& info) {
    return Json::array({Json{{"op", "set_file_info"},
                             {"title", title},
                             {"author", info.author},
                             {"author_title", info.author_title},
                             {"description", info.description},
                             {"keywords", info.keywords},
                             {"copyright", info.copyright},
                             {"copyright_status", info.copyright_status},
                             {"copyright_url", info.copyright_url}}});
}

// The eight items of two File Infos the same (the packet's ref aside).
bool same_items(core::FileInfo a, core::FileInfo b) {
    a.xmp.clear();
    b.xmp.clear();
    return a == b;
}

}  // namespace

class TestContractFileInfo : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const char* name) const { return fs::path(tmp_.path().toStdString()) / name; }
    QString qat(const char* name) const { return QString::fromStdString(core::path_to_utf8(at(name))); }

private slots:
    // The XMP packet: the eight items written and read back; the packet's other properties kept; the old items replaced
    // (elements and attributes alike); text escaped.
    void theXmpItemsAreWrittenAndReadBack() {
        const core::FileInfo info = some_info();
        const std::string made = core::write_xmp(std::nullopt, info, "題名");
        QVERIFY(made.find("<x:xmpmeta") != std::string::npos && made.find("</rdf:RDF>") != std::string::npos);
        QVERIFY(same_items(core::file_info_from_xmp(made), info));
        QCOMPARE(core::xmp_title(made), std::string("題名"));
        QVERIFY(made.find("説明 &lt;&amp;&gt; &quot;引用&quot;") != std::string::npos);
        // (another packet: its other properties kept, its old items gone)
        const std::string other =
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
            "<rdf:Description rdf:about=\"\" xmlns:exif=\"http://ns.adobe.com/exif/1.0/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
            " dc:rights=\"old rights\" exif:ISOSpeed=\"100\">"
            "<dc:creator><rdf:Seq><rdf:li>Old Name</rdf:li><rdf:li>Second</rdf:li></rdf:Seq></dc:creator>"
            "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Old title</rdf:li></rdf:Alt></dc:title>"
            "<exif:Flash>16</exif:Flash>"
            "</rdf:Description></rdf:RDF></x:xmpmeta>";
        const core::FileInfo read = core::file_info_from_xmp(other);
        QCOMPARE(read.author, std::string("Old Name; Second"));
        QCOMPARE(read.copyright, std::string("old rights"));
        QCOMPARE(core::xmp_title(other), std::string("Old title"));
        const std::string written = core::write_xmp(other, info, "新しい題名");
        QVERIFY(written.find("exif:ISOSpeed=\"100\"") != std::string::npos);
        QVERIFY(written.find("<exif:Flash>16</exif:Flash>") != std::string::npos);
        QVERIFY(written.find("Old Name") == std::string::npos && written.find("old rights") == std::string::npos &&
                written.find("Old title") == std::string::npos);
        QVERIFY(same_items(core::file_info_from_xmp(written), info));
        QCOMPARE(core::xmp_title(written), std::string("新しい題名"));
        // (an item emptied: gone from the packet; the copyright's status not said: no Marked)
        core::FileInfo fewer = info;
        fewer.description.clear();
        fewer.copyright_status.clear();
        const std::string less = core::write_xmp(written, fewer, "");
        QVERIFY(less.find("dc:description") == std::string::npos && less.find("xmpRights:Marked") == std::string::npos &&
                less.find("dc:title") == std::string::npos);
        QVERIFY(same_items(core::file_info_from_xmp(less), fewer));
        // (public domain: Marked False)
        fewer.copyright_status = "public_domain";
        QCOMPARE(core::file_info_from_xmp(core::write_xmp(std::nullopt, fewer, "")).copyright_status, std::string("public_domain"));
    }

    // set_file_info: the items given replace the book's (an empty one removes it), the title is the book's; keywords as a
    // list or "a; b"; unknown copyright status, keys and long text refused; saved and read back (file-info@1), emptied:
    // nothing written.
    void setFileInfoKeepsTheItemsInTheBook() {
        const core::FileInfo info = some_info();
        const core::ApplyResult applied = run_result(small_book(), info_ops("新しい題", info));
        const core::Document& doc = applied.doc;
        QCOMPARE(doc.title, std::string("新しい題"));
        QVERIFY(doc.file_info && same_items(*doc.file_info, info));
        QCOMPARE(applied.results.size(), std::size_t(1));
        QVERIFY2(applied.results[0]["file_info"] == core::to_json(info), applied.results[0].dump().c_str());
        // (only what is given changes; keywords as text)
        const core::Document some = run(doc, Json::array({Json{{"op", "set_file_info"}, {"keywords", "a; b, c ;; a"}, {"description", "  "}}}));
        QCOMPARE(some.file_info->keywords, (std::vector<std::string>{"a", "b", "c"}));
        QVERIFY(some.file_info->description.empty());
        QCOMPARE(some.file_info->author, info.author);
        QCOMPARE(some.title, std::string("新しい題"));
        // (unknown status: not said)
        QVERIFY(run(doc, Json::array({Json{{"op", "set_file_info"}, {"copyright_status", "unknown"}}})).file_info->copyright_status.empty());
        for (const Json& bad : {Json{{"op", "set_file_info"}, {"copyright_status", "maybe"}}, Json{{"op", "set_file_info"}, {"colour", "red"}},
                                Json{{"op", "set_file_info"}, {"author", std::string(2001, 'x')}}, Json{{"op", "set_file_info"}, {"keywords", 3}},
                                Json{{"op", "set_file_info"}, {"author", 7}}}) {
            try {
                (void)run(doc, Json::array({bad}));
                QFAIL(("not refused: " + bad.dump()).c_str());
            } catch (const core::ApplyError& error) {
                QVERIFY2(std::string(error.what()).find("set_file_info") != std::string::npos, error.what());
            }
        }
        // (saved: file-info@1, read back the same)
        const fs::path dir = at("info.genko");
        write_book(doc, dir);
        const Json saved = project(dir);
        QVERIFY(has_feature(saved, core::kFileInfoFeature));
        QVERIFY(saved["file_info"] == core::to_json(*doc.file_info));
        const core::Document back = storage::load_document(dir).document;
        QVERIFY(back.read_only_reason.empty());
        QVERIFY(back.file_info && *back.file_info == *doc.file_info);
        // (every item emptied: no file_info, no file-info@1)
        const core::Document cleared = run(back, Json::array({Json{{"op", "set_file_info"}, {"author", ""}, {"author_title", ""}, {"description", ""},
                                                                    {"keywords", Json::array()}, {"copyright", ""}, {"copyright_status", ""},
                                                                    {"copyright_url", ""}}}));
        QVERIFY(!cleared.file_info);
        const fs::path empty = at("cleared.genko");
        write_book(cleared, empty);
        QVERIFY(!project(empty).contains("file_info"));
        QVERIFY(!has_feature(project(empty), core::kFileInfoFeature));
    }

    // A picture's EXIF and XMP kept when it is opened: the XMP's items the book's File Info (its packet kept with it), the
    // EXIF's orientation applied to the pixels as Pillow's exif_transpose turns them and the kept EXIF upright; saved, the
    // packet and the block are the book's assets; `genko inspect --file-info`.
    void anOpenedPictureKeepsItsExifAndXmp() {
        const render::Image picture = distinct(5, 3);
        const core::FileInfo info = some_info();
        const std::string packet = core::write_xmp(std::nullopt, info, "元の題");
        static constexpr render::Transpose kTurn[] = {render::Transpose::FlipLeftRight, render::Transpose::Rotate180, render::Transpose::FlipTopBottom,
                                                      render::Transpose::Transpose,     render::Transpose::Rotate270, render::Transpose::Transverse,
                                                      render::Transpose::Rotate90};
        for (int orientation = 1; orientation <= 8; ++orientation) {
            std::string png = render::write_png(picture);
            png = with_chunk(png, "eXIf", exif_block(orientation));
            png = with_chunk(png, "iTXt", std::string("XML:com.adobe.xmp", 17) + std::string(5, '\0') + packet);
            const formats::Opened opened = formats::open_file(png, "turned.png");
            const core::Page& page = opened.doc.page(0);
            const render::Image shown = render::open_image(*page.layers[0].raster_png).convert("RGB");
            const render::Image wanted = orientation == 1 ? picture : picture.transpose(kTurn[orientation - 2]);
            QVERIFY2(shown.size() == wanted.size() && shown.tobytes() == wanted.tobytes(), std::to_string(orientation).c_str());
            QCOMPARE(page.native_source->width_px, std::int64_t(wanted.width()));
            const bool noted = std::any_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("EXIF の向き") != std::string::npos; });
            QCOMPARE(noted, orientation != 1);
            // (the EXIF kept upright, the XMP as the book's File Info)
            QVERIFY(!page.native_source->exif.empty() && !page.native_source->xmp.empty());
            const auto& exif = opened.doc.native_files.at(page.native_source->exif);
            QCOMPARE(codecs::exif_orientation(*exif), 1);
            QVERIFY(opened.doc.file_info && same_items(*opened.doc.file_info, info));
            QCOMPARE(opened.doc.file_info->xmp, page.native_source->xmp);
            QCOMPARE(*opened.doc.native_files.at(page.native_source->xmp), packet);
        }
        // (a JPEG: APP1's EXIF and XMP; turned 180°)
        const std::string jpeg = with_app1(with_app1(formats::jpeg_bytes(picture, formats::JpegSave{95, false, {}}), std::string("Exif\0\0", 6) + exif_block(3)),
                                           std::string("http://ns.adobe.com/xap/1.0/\0", 29) + packet);
        const formats::Opened from_jpeg = formats::open_file(jpeg, "photo.jpg");
        const render::Image decoded = render::open_image(jpeg).convert("RGB");
        QVERIFY(render::open_image(*from_jpeg.doc.page(0).layers[0].raster_png).convert("RGB").tobytes() == decoded.transpose(render::Transpose::Rotate180).tobytes());
        QVERIFY(from_jpeg.doc.file_info && same_items(*from_jpeg.doc.file_info, info));
        // (saved: the assets, file-info@1; read back the same; inspect --file-info)
        const fs::path dir = at("opened-info.genko");
        write_book(from_jpeg.doc, dir);
        storage::AssetStore store(dir);
        const core::NativeSource& source = *from_jpeg.doc.page(0).native_source;
        QVERIFY(store.has(source.exif, core::kExifSuffix) && store.has(source.xmp, core::kXmpSuffix));
        QVERIFY(has_feature(project(dir), core::kFileInfoFeature));
        const auto inspected = genko::test::run_genko({QStringLiteral("inspect"), qat("opened-info.genko"), QStringLiteral("--file-info")});
        QVERIFY2(inspected.exit_code == 0, qPrintable(inspected.described()));
        const Json said = genko::test::one_line(inspected.out);
        QCOMPARE(said["title"].get<std::string>(), std::string("photo"));
        QVERIFY2(said["file_info"] == core::to_json(*from_jpeg.doc.file_info), said.dump().c_str());
        QCOMPARE(said["pages"][0]["native_source"]["format"].get<std::string>(), std::string("jpeg"));
        QVERIFY(said["pages"][0]["native_source"]["exif"] == Json(source.exif));
        // (the book's own inspect says its File Info too; a book with none says nothing new)
        const auto plain = genko::test::run_genko({QStringLiteral("inspect"), qat("opened-info.genko")});
        QVERIFY(genko::test::one_line(plain.out).contains("file_info"));
        // (written again with File Info changed: the packet's other properties kept, the items the book's)
        const core::Document edited = run(from_jpeg.doc, Json::array({Json{{"op", "set_file_info"}, {"author", "別の人"}}}));
        const std::string out = *formats::book_xmp(edited);
        QCOMPARE(core::file_info_from_xmp(out).author, std::string("別の人"));
        QCOMPARE(core::file_info_from_xmp(out).copyright, info.copyright);
        QCOMPARE(core::xmp_title(out), std::string("photo"));
    }

    // An RGB picture with no profile whose EXIF says sRGB is sRGB, not the working RGB (unless the EXIF's profile tag is
    // ignored); a WebP's EXIF and XMP chunks read.
    void exifColourSpaceAndWebP() {
        const std::string adobe = *core::builtin_profile("adobe-rgb-compat")->bytes;
        const std::string png = with_chunk(render::write_png(distinct(4, 4)), "eXIf", exif_block(1, true));
        formats::OpenOptions options;
        options.working_rgb = adobe;
        QVERIFY(!formats::open_file(png, "s.png", options).doc.page(0).rgb);
        options.ignore_exif_profile = true;
        QVERIFY(formats::open_file(png, "s.png", options).doc.page(0).rgb);
        // (a WebP made extended by hand around this build's own lossy picture: VP8X, the picture, EXIF and XMP chunks)
        const QString book = qat("webp.genko");
        QVERIFY(genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")}).exit_code == 0);
        const QString file = qat("page.webp");
        const auto rendered = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                      QStringLiteral("20"), QStringLiteral("--out"), file});
        QVERIFY2(rendered.exit_code == 0, qPrintable(rendered.described()));
        const std::string simple = genko::test::read_bytes(file);
        QCOMPARE(simple.substr(12, 4), std::string("VP8 "));
        const std::string packet = core::write_xmp(std::nullopt, some_info(), "WebP");
        const render::Size size = render::picture_size(simple);
        std::string vp8x("VP8X", 4);
        le32(vp8x, 10);
        vp8x += static_cast<char>(0x08 | 0x04);  // (EXIF and XMP)
        vp8x += std::string(3, '\0');
        for (const int side : {size.width - 1, size.height - 1}) {
            vp8x += static_cast<char>(side & 0xFF);
            vp8x += static_cast<char>((side >> 8) & 0xFF);
            vp8x += static_cast<char>((side >> 16) & 0xFF);
        }
        const auto riff_chunk = [](const char* type, const std::string& data) {
            std::string out(type, 4);
            le32(out, static_cast<std::uint32_t>(data.size()));
            out += data;
            if (data.size() % 2) out += '\0';
            return out;
        };
        const std::string body = std::string("WEBP", 4) + vp8x + simple.substr(12) + riff_chunk("EXIF", exif_block(1)) + riff_chunk("XMP ", packet);
        std::string webp("RIFF", 4);
        le32(webp, static_cast<std::uint32_t>(body.size()));
        webp += body;
        const codecs::FileMetadata meta = codecs::read_metadata(codecs::Format::WebP, webp);
        QVERIFY(meta.xmp && *meta.xmp == packet);
        QVERIFY(meta.exif && *meta.exif == exif_block(1));
        const formats::Opened opened = formats::open_file(webp, "page.webp");
        QVERIFY(opened.doc.file_info && same_items(*opened.doc.file_info, some_info()));
        QCOMPARE(opened.doc.page(0).native_source->format, std::string("webp"));
    }

    // The exports (`genko export`: png, tiff, psd, sns's JPEG) and コピーを保存… (PNG, JPEG, TIFF, PSD) put the book's File
    // Info in each file; read back by this build's reader. A book with none: no XMP (its files as before).
    void theFilesWrittenCarryTheXmp() {
        const core::FileInfo info = some_info();
        const core::Document doc = run(small_book(), info_ops("書き出す本", info));
        const fs::path dir = at("export.genko");
        write_book(doc, dir);
        const fs::path plain_dir = at("plain-export.genko");
        write_book(small_book(), plain_dir);
        // ("png", "jpg", "tif" by this build's reader; "psd" by its resource 1060, read straight)
        const auto check = [&](const std::string& bytes, std::string_view kind, bool carries, const std::string& what) {
            const std::optional<std::string> xmp = kind == "psd"   ? psd_xmp(bytes)
                                                   : kind == "png" ? codecs::read_metadata(codecs::Format::Png, bytes).xmp
                                                   : kind == "tif" ? codecs::read_metadata(codecs::Format::Tiff, bytes).xmp
                                                                    : codecs::read_metadata(codecs::Format::Jpeg, bytes).xmp;
            QVERIFY2(xmp.has_value() == carries, what.c_str());
            if (!carries) return;
            QVERIFY2(same_items(core::file_info_from_xmp(*xmp), info), what.c_str());
            QCOMPARE(core::xmp_title(*xmp), std::string("書き出す本"));
        };
        for (const bool with_info : {true, false}) {
            const fs::path book = with_info ? dir : plain_dir;
            for (const char* format : {"png", "tiff", "psd", "sns", "sns-jpeg"}) {
                const QString out = QString::fromStdString(core::path_to_utf8(book.parent_path() / (book.stem().string() + "-" + format)));
                const bool jpeg = std::string_view(format) == "sns-jpeg";
                QStringList args{QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book)), out, QStringLiteral("--format"),
                                 jpeg ? QStringLiteral("sns") : QString::fromLatin1(format), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--json")};
                if (jpeg) args << QStringLiteral("--jpeg");
                const auto made = genko::test::run_genko(args);
                QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
                const Json files = genko::test::one_line(made.out)["files"];
                QVERIFY(!files.empty());
                const std::string path = files[0].get<std::string>();
                const std::string_view f(format);
                // (sns: a PNG by default)
                const std::string_view kind = f == "tiff" ? "tif" : f.starts_with("sns") ? (path.ends_with(".jpg") ? "jpg" : "png") : f;
                if (jpeg) QVERIFY2(path.ends_with(".jpg"), path.c_str());
                check(genko::test::read_bytes(QString::fromStdString(path)), kind, with_info, std::string(format) + (with_info ? " with" : " without"));
            }
        }
        // (コピーを保存…)
        for (const char* name : {"copy.png", "copy.jpg", "copy.tif", "copy.psd"}) {
            const fs::path file = at(name);
            const formats::CopyResult copied = formats::write_copy(doc, 0, file);
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(file)));
            const std::string_view ext = fs::path(name).extension().string() == ".png"   ? "png"
                                         : fs::path(name).extension().string() == ".jpg" ? "jpg"
                                         : fs::path(name).extension().string() == ".tif" ? "tif"
                                                                                          : "psd";
            check(bytes, ext, true, name);
            if (ext != "psd") QVERIFY(codecs::detect(bytes) == (ext == "png" ? codecs::Format::Png : ext == "jpg" ? codecs::Format::Jpeg : codecs::Format::Tiff));
        }
        // (a merged PSD copy too)
        formats::CopyOptions flat;
        flat.layers = false;
        (void)formats::write_copy(doc, 0, at("flat.psd"), flat);
        check(genko::test::read_bytes(qat("flat.psd")), "psd", true, "flat.psd");
    }

    // The reference's readers (Pillow, psd-tools) find the same packet in the files written.
    void theReferencesReadersReadTheXmp() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        const core::Document doc = run(small_book(), info_ops("参照で読む", some_info()));
        for (const char* name : {"ref.png", "ref.jpg", "ref.tif", "ref.psd"}) (void)formats::write_copy(doc, 0, at(name));
        const QString script = QStringLiteral(
            "import json, sys\n"
            "from PIL import Image\n"
            "from psd_tools import PSDImage\n"
            "out = {}\n"
            "d = sys.argv[1]\n"
            "im = Image.open(d + '/ref.png'); out['png'] = im.info.get('XML:com.adobe.xmp')\n"
            "im = Image.open(d + '/ref.jpg'); x = im.info.get('xmp'); out['jpg'] = x.decode('utf-8') if isinstance(x, bytes) else x\n"
            "im = Image.open(d + '/ref.tif'); x = im.tag_v2.get(700); out['tif'] = x.decode('utf-8') if isinstance(x, bytes) else x\n"
            "p = PSDImage.open(d + '/ref.psd'); x = p.image_resources.get_data(1060); out['psd'] = x.decode('utf-8') if isinstance(x, bytes) else x\n"
            "print(json.dumps(out))\n");
        const auto ran = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), script, tmp_.path()}, genko::test::python_env(tmp_.path() + QStringLiteral("/py")));
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        const Json read = Json::parse(ran.out.toStdString());
        for (const char* kind : {"png", "jpg", "tif", "psd"}) {
            QVERIFY2(read[kind].is_string(), kind);
            QVERIFY2(same_items(core::file_info_from_xmp(read[kind].get<std::string>()), some_info()), kind);
            QCOMPARE(core::xmp_title(read[kind].get<std::string>()), std::string("参照で読む"));
        }
    }
};

QTEST_GUILESS_MAIN(TestContractFileInfo)
#include "test_contract_file_info.moc"
