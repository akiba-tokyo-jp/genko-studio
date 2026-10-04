// render::read_png against Pillow: PNG files of every mode (saved by Pillow; one interlaced) read to the mode,
// pixels and transparency Pillow reads, and converted to RGBA and L the same; broken files and huge sizes refused;
// write_png keeps the pixels.

#include <QtTest>

#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/command_bus.hpp"
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
    void jpegPixelsMatchPillow_data() {
        QTest::addColumn<int>("index");
        const Json data = genko::test::read_json(genko::test::test_data("pyref/jpeg/tables.json"));
        for (std::size_t i = 0; i < data.size(); ++i)
            QTest::newRow(data[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
    }

    void jpegPixelsMatchPillow() {
        QFETCH(int, index);
        const Json data = genko::test::read_json(genko::test::test_data("pyref/jpeg/tables.json"));
        const Json& c = data[static_cast<std::size_t>(index)];
        const std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/" + QString::fromStdString(c["file"].get<std::string>())));
        QVERIFY(!bytes.empty());
        try {
            const render::Image im = render::open_image(bytes);
            QCOMPARE(std::string(im.mode()), c["mode"].get<std::string>());
            QCOMPARE(im.width(), c["size"][0].get<int>());
            QCOMPARE(im.height(), c["size"][1].get<int>());
            QVERIFY(im.tobytes() == genko::core::a2b_base64(c["data"].get<std::string>()));
            QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(c["rgba"].get<std::string>()));
            QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(c["l"].get<std::string>()));
        } catch (const genko::core::Error& error) {
            QFAIL(error.what());
        }
    }

    void jpegPixelLimit() {
        const std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg"));
        render::PngLimits limits;
        limits.max_pixels = 1;
        try {
            (void)render::open_image(bytes, limits);
            QFAIL("JPEG must honor the configured pixel limit");
        } catch (const genko::core::Error& error) {
            QVERIFY2(error.code() == "image_too_large", error.what());
        }
    }

    void jpegOversizedHeader() {
        std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg"));
        const std::size_t sof = bytes.find(std::string("\xff\xc0", 2));
        QVERIFY(sof != std::string::npos);
        QVERIFY(sof + 9 < bytes.size());
        constexpr unsigned dimension = 40000;
        for (std::size_t offset : {std::size_t{5}, std::size_t{7}}) {
            bytes[sof + offset] = static_cast<char>(dimension >> 8);
            bytes[sof + offset + 1] = static_cast<char>(dimension & 255);
        }
        try {
            (void)render::open_image(bytes);
            QFAIL("Oversized JPEG must be rejected before decompression/allocation");
        } catch (const genko::core::Error& error) {
            QVERIFY2(error.code() == "image_too_large", error.what());
        }
    }

    void jpegMalformed_data() {
        QTest::addColumn<int>("index");
        const Json rows = genko::test::read_json(genko::test::test_data("pyref/jpeg/broken.json"));
        for (std::size_t i = 0; i < rows.size(); ++i)
            QTest::newRow(rows[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
    }

    void jpegMalformed() {
        QFETCH(int, index);
        const Json rows = genko::test::read_json(genko::test::test_data("pyref/jpeg/broken.json"));
        const Json good = genko::test::read_json(genko::test::test_data("pyref/jpeg/tables.json"));
        const std::string broken = genko::core::a2b_base64(rows[static_cast<std::size_t>(index)]["data"].get<std::string>());
        const std::string valid = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg"));
        const std::string expected = rows[static_cast<std::size_t>(index)]["python_error"] == "UnidentifiedImageError"
                                         ? "unidentified_image" : "format";
        for (int repeat = 0; repeat < 3; ++repeat) {
            try {
                (void)render::open_image(broken);
                QFAIL("Pillow-rejected JPEG must not become a successful partial image");
            } catch (const genko::core::Error& error) {
                QVERIFY2(error.code() == expected, error.what());
            }
            const render::Image decoded = render::open_image(valid);
            QVERIFY(decoded.tobytes() == genko::core::a2b_base64(good[0]["data"].get<std::string>()));
        }
    }

    void jpegUnsupportedPrecision() {
        std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg"));
        const std::size_t sof = bytes.find("\xff\xc0");
        QVERIFY(sof != std::string::npos && sof + 9 < bytes.size());
        bytes[sof + 4] = 12;
        try {
            (void)render::open_image(bytes);
            QFAIL("an unsupported precision was accepted");
        } catch (const genko::core::Error& error) {
            QVERIFY2(error.code() == "unidentified_image", error.what());
        }
    }
    void jpegCoefficientBudget() {
        std::string bytes = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/progressive.jpg"));
        const std::size_t sof = bytes.find("\xff\xc2");
        QVERIFY(sof != std::string::npos && sof + 18 < bytes.size());
        bytes[sof + 5] = bytes[sof + 7] = static_cast<char>(20000 >> 8);
        bytes[sof + 6] = bytes[sof + 8] = static_cast<char>(20000 & 255);
        for (std::size_t i = 0; i < 3; ++i) bytes[sof + 11 + 3 * i] = '\x11';
        render::ImageAllocationBudget budget(128ull << 20);
        try {
            (void)render::open_image(bytes);
            QFAIL("a progressive coefficient allocation exceeding the budget was accepted");
        } catch (const genko::core::Error& error) {
            QVERIFY2(error.code() == "image_too_large", error.what());
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
        const render::Image good = render::open_image(genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg")));
        QVERIFY(!good.empty());
        QVERIFY(budget.live() > 0);
    }
    void jpegActiveBudget() {
        render::ImageAllocationBudget budget(4096);
        try {
            (void)render::open_image(genko::test::read_bytes(genko::test::test_data("pyref/jpeg/progressive.jpg")));
            QFAIL("the existing image allocation budget did not account for JPEG working memory");
        } catch (const genko::core::OpError& error) {
            QVERIFY2(std::string(error.what()).find("too many masks") != std::string::npos, error.what());
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void jpegAllocationAndDecodeFailuresReturnBudget() {
        const std::string valid = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/gray.jpg"));
        {
            // Working reservation succeeds; the output image allocation must then fail without leaking it.
            render::ImageAllocationBudget budget((64ull << 20) + valid.size() + 1);
            try {
                (void)render::open_image(valid);
                QFAIL("an output image exceeding the remaining active budget was accepted");
            } catch (const genko::core::OpError& error) {
                QVERIFY2(std::string(error.what()).find("too many masks") != std::string::npos, error.what());
            }
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
        render::ImageAllocationBudget budget(128ull << 20);
        const Json rows = genko::test::read_json(genko::test::test_data("pyref/jpeg/broken.json"));
        for (const auto& row : rows) {
            try {
                (void)render::open_image(genko::core::a2b_base64(row["data"].get<std::string>()));
                QFAIL("a truncated JPEG was accepted");
            } catch (const genko::core::Error&) {}
            QCOMPARE(budget.live(), std::uint64_t{0});
            {
                const render::Image recovered = render::open_image(valid);
                QVERIFY(!recovered.empty());
                QVERIFY(budget.live() > 0);
            }
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
    }
    void jpegBudgetIncludesAlreadyLivePixels() {
        const std::string valid = genko::test::read_bytes(genko::test::test_data("pyref/jpeg/progressive.jpg"));
        render::ImageAllocationBudget budget(65ull << 20);
        {
            const auto existing = render::Image::create_blank("RGBA", {1024, 1024});
            const auto live_before = budget.live();
            QVERIFY(live_before >= (4ull << 20));
            try {
                (void)render::open_image(valid);
                QFAIL("JPEG working memory ignored existing live image allocations");
            } catch (const genko::core::OpError& error) {
                QVERIFY2(std::string(error.what()).find("too many masks") != std::string::npos, error.what());
            }
            QCOMPARE(budget.live(), live_before);
            QVERIFY(!existing.empty());
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
        {
            const auto recovered = render::open_image(valid);
            QVERIFY(!recovered.empty());
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
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
