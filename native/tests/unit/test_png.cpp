// render::read_png against Pillow: PNG files of every mode (saved by Pillow; one interlaced) read to the mode,
// pixels and transparency Pillow reads, and converted to RGBA and L the same; broken files and huge sizes refused;
// write_png keeps the pixels.

#include <QtTest>

#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

const Json& tables() {
    static const Json data = genko::test::read_json(genko::test::test_data("pyref/render_unit_tables.json"));
    return data;
}

std::string crc_chunk(const std::string& type, const std::string& data) {
    // CRC-32 of type + data (PNG's chunk CRC)
    std::uint32_t table[256];
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xedb88320U ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    std::uint32_t crc = 0xffffffffU;
    for (const char ch : type + data) crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xffU] ^ (crc >> 8);
    crc ^= 0xffffffffU;
    const auto be32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
    };
    return be32(static_cast<std::uint32_t>(data.size())) + type + data + be32(crc);
}

}  // namespace

class TestPng : public QObject {
    Q_OBJECT

private slots:
    void read_data() {
        QTest::addColumn<int>("index");
        const Json& cases = tables()["png"];
        for (std::size_t i = 0; i < cases.size(); ++i) {
            QTest::newRow(cases[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
        }
    }

    void read() {
        QFETCH(int, index);
        const Json& c = tables()["png"][static_cast<std::size_t>(index)];
        const std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/" + QString::fromStdString(c["file"].get<std::string>())));
        QVERIFY(!bytes.empty());
        const render::Image im = render::read_png(bytes);
        QCOMPARE(std::string(im.mode()), c["mode"].get<std::string>());
        QCOMPARE(im.width(), c["size"][0].get<int>());
        QCOMPARE(im.height(), c["size"][1].get<int>());
        QVERIFY(im.tobytes() == genko::core::a2b_base64(c["data"].get<std::string>()));
        const Json& t = c["transparency"];
        const render::Transparency& got = im.transparency();
        if (t.is_null()) {
            QVERIFY(!got.present());
        } else if (t.contains("index")) {
            QCOMPARE(got.kind, render::Transparency::Kind::Index);
            QCOMPARE(got.value, t["index"].get<int>());
        } else if (t.contains("rgb")) {
            QCOMPARE(got.kind, render::Transparency::Kind::Rgb);
            QCOMPARE(got.rgb[0], t["rgb"][0].get<int>());
            QCOMPARE(got.rgb[1], t["rgb"][1].get<int>());
            QCOMPARE(got.rgb[2], t["rgb"][2].get<int>());
        } else {
            QCOMPARE(got.kind, render::Transparency::Kind::Bytes);
            QVERIFY(got.bytes == genko::core::a2b_base64(t["bytes"].get<std::string>()));
        }
        QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(c["rgba"].get<std::string>()));
        QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(c["l"].get<std::string>()));

        // written and read back: the same pixels
        if (im.mode() != "P") {
            const render::Image back = render::read_png(render::write_png(im));
            QCOMPARE(back.mode(), im.mode());
            QVERIFY(back.tobytes() == im.tobytes());
        }
    }

    void broken() {
        // where Pillow fails: Image.open (UnidentifiedImageError) or load() (OSError)
        const auto code_of = [](const std::string& bytes) -> std::string {
            try {
                (void)render::open_image(bytes);
            } catch (const genko::core::Error& e) {
                return e.code();
            }
            return "(read)";
        };
        const std::string good = genko::test::read_bytes(genko::test::test_data("pyref/png/rgb8.png"));
        QCOMPARE(code_of("not a png"), std::string("unidentified_image"));
        QCOMPARE(code_of(good.substr(0, good.size() / 2)), std::string("format"));  // cut in the image data
        std::string bad_crc = good;
        bad_crc[20] = static_cast<char>(bad_crc[20] ^ 0x40);  // inside IHDR
        QCOMPARE(code_of(bad_crc), std::string("unidentified_image"));
        std::string bad_data = good;
        for (std::size_t i = 45; i < bad_data.size() - 16; ++i) bad_data[i] = static_cast<char>(0x5a);  // the IDAT stream
        QCOMPARE(code_of(bad_data), std::string("format"));
        QCOMPARE(code_of("GIF89a...."), std::string("not_yet_ported"));  // (Pillow opens GIF: not read here yet)
    }

    void huge() {
        // a 30000 × 30000 RGBA header (900 million pixels): refused before anything is decoded
        std::string ihdr;
        for (const std::uint32_t v : {30000U, 30000U}) {
            ihdr += std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
        }
        ihdr += std::string{8, 6, 0, 0, 0};
        const std::string file = std::string("\x89PNG\r\n\x1a\n", 8) + crc_chunk("IHDR", ihdr) + crc_chunk("IDAT", std::string(16, '\0')) +
                                 crc_chunk("IEND", "");
        try {
            (void)render::read_png(file);
            QFAIL("a huge PNG was read");
        } catch (const genko::core::Error& e) {
            QCOMPARE(e.code(), std::string("image_too_large"));
        }
        // a lower limit refuses smaller ones too
        const std::string small = genko::test::read_bytes(genko::test::test_data("pyref/png/rgba8.png"));
        render::PngLimits limits;
        limits.max_pixels = 10;
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::read_png(small, limits));
        // Pillow's Image.open refuses more than 178956970 pixels (13400 × 13355 = 178957000): so does the drawing
        std::string grey;
        for (const std::uint32_t v : {13400U, 13355U}) {
            grey += std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
        }
        grey += std::string{8, 0, 0, 0, 0};
        const std::string bomb = std::string("\x89PNG\r\n\x1a\n", 8) + crc_chunk("IHDR", grey) + crc_chunk("IDAT", std::string(16, '\0')) +
                                 crc_chunk("IEND", "");
        try {
            (void)render::open_image(bomb, render::kPillowOpenLimits);
            QFAIL("a PNG Pillow refuses was read");
        } catch (const genko::core::Error& e) {
            QCOMPARE(e.code(), std::string("image_too_large"));
            QCOMPARE(std::string(e.what()), std::string("Image size (178957000 pixels) exceeds limit of 178956970 pixels, could be "
                                                         "decompression bomb DOS attack."));
        }
    }
};

QTEST_GUILESS_MAIN(TestPng)
#include "test_png.moc"
