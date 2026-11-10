// render::read_png against Pillow: PNG files of every mode (saved by Pillow; one interlaced) read to the mode,
// pixels and transparency Pillow reads, and converted to RGBA and L the same; broken files and huge sizes refused;
// write_png keeps the pixels; the exports' row writer (formats::PngWriter) refuses rows wider than an int counts.

#include <QtTest>

#include <string>
#include <vector>
#include <thread>
#include <barrier>
#include <array>

#include "render/page.hpp"

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/command_bus.hpp"
#include "formats/pillow_save.hpp"
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
    void gifFirstFramePixelsMatchPillow() {
        const Json c = genko::test::read_json(genko::test::test_data("pyref/gif/tables.json")).at(0);
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/palette.gif"));
        QVERIFY(!bytes.empty());
        try {
            const render::Image im = render::open_image(bytes);
            QCOMPARE(std::string(im.mode()), c.at("mode").get<std::string>());
            QCOMPARE(im.width(), c.at("size").at(0).get<int>());
            QCOMPARE(im.height(), c.at("size").at(1).get<int>());
            QVERIFY(im.tobytes() == genko::core::a2b_base64(c.at("data").get<std::string>()));
            QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(c.at("rgba").get<std::string>()));
            QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(c.at("l").get<std::string>()));
        } catch (const genko::core::Error& error) {
            QFAIL(error.what());
        }
    }
    void gifGrayFirstFrameMatchesPillow() {
        const Json c = genko::test::read_json(genko::test::test_data("pyref/gif/tables.json")).at(2);
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/gray.gif"));
        QVERIFY(!bytes.empty());
        try {
            const render::Image im = render::open_image(bytes);
            QCOMPARE(std::string(im.mode()), c.at("mode").get<std::string>());
            QCOMPARE(im.width(), c.at("size").at(0).get<int>());
            QCOMPARE(im.height(), c.at("size").at(1).get<int>());
            QVERIFY(im.tobytes() == genko::core::a2b_base64(c.at("data").get<std::string>()));
            QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(c.at("rgba").get<std::string>()));
            QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(c.at("l").get<std::string>()));
        } catch (const genko::core::Error& error) {
            QFAIL(error.what());
        }
    }
    void gifTransparentFirstFrameMatchesPillow() {
        const Json c = genko::test::read_json(genko::test::test_data("pyref/gif/tables.json")).at(1);
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/transparent.gif"));
        QVERIFY(!bytes.empty());
        try {
            const render::Image im = render::open_image(bytes);
            QCOMPARE(std::string(im.mode()), c.at("mode").get<std::string>());
            QCOMPARE(im.width(), c.at("size").at(0).get<int>());
            QCOMPARE(im.height(), c.at("size").at(1).get<int>());
            QVERIFY(im.tobytes() == genko::core::a2b_base64(c.at("data").get<std::string>()));
            QCOMPARE(im.transparency().kind, render::Transparency::Kind::Index);
            QCOMPARE(im.transparency().value, c.at("transparency_index").get<int>());
            QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(c.at("rgba").get<std::string>()));
            QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(c.at("l").get<std::string>()));
        } catch (const genko::core::Error& error) {
            QFAIL(error.what());
        }
    }
    void gifAdditionalPixelsMatchPillow_data() {
        QTest::addColumn<Json>("item");
        for (const auto& c : genko::test::read_json(genko::test::test_data("pyref/gif/tables.json"))) {
            QTest::newRow(c.at("name").get<std::string>().c_str()) << c;
        }
    }
    void gifAdditionalPixelsMatchPillow() {
        QFETCH(Json, item);
        const auto bytes = genko::test::read_bytes(genko::test::test_data(QString::fromStdString("pyref/" + item.at("file").get<std::string>())));
        try {
            const auto im = render::open_image(bytes);
            QCOMPARE(std::string(im.mode()), item.at("mode").get<std::string>());
            QCOMPARE(im.width(), item.at("size").at(0).get<int>());
            QCOMPARE(im.height(), item.at("size").at(1).get<int>());
            QVERIFY(im.tobytes() == genko::core::a2b_base64(item.at("data").get<std::string>()));
            QVERIFY(im.convert("RGBA").tobytes() == genko::core::a2b_base64(item.at("rgba").get<std::string>()));
            QVERIFY(im.convert("L").tobytes() == genko::core::a2b_base64(item.at("l").get<std::string>()));
            if (item.contains("transparency_index")) {
                QCOMPARE(im.transparency().kind, render::Transparency::Kind::Index);
                QCOMPARE(im.transparency().value, item.at("transparency_index").get<int>());
            } else QCOMPARE(im.transparency().kind, render::Transparency::Kind::None);
        } catch (const genko::core::Error& error) { QFAIL(error.what()); }
    }
    void gifClassifiedErrorsMatchPillow_data() {
        QTest::addColumn<Json>("item");
        for (const auto& c : genko::test::read_json(genko::test::test_data("pyref/gif/errors.json"))) {
            QVERIFY(!c.at("python").at("ok").get<bool>());
            QTest::newRow(c.at("name").get<std::string>().c_str()) << c;
        }
    }
    void gifClassifiedErrorsMatchPillow() {
        QFETCH(Json, item);
        try {
            (void)render::open_image(genko::core::a2b_base64(item.at("b64").get<std::string>()));
            QFAIL("broken GIF must be refused before it can be registered");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), item.at("python").at("code").get<std::string>());
            if (item.at("python").at("stage") == "load")
                QCOMPARE(std::string(error.what()), item.at("python").at("message").get<std::string>());
        }
    }
    void gifMixedLogicalModeFollowsPillowCopyAndMove() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/colored-global-gray-local.gif"));
        auto image = render::open_image(bytes);
        QCOMPARE(std::string(image.mode()), std::string("L"));
        QCOMPARE(image.getbands(), std::vector<std::string>{"L"});
        auto moved = std::move(image);
        QVERIFY(image.empty());
        QCOMPARE(std::string(moved.mode()), std::string("L"));
        render::Image assigned;
        assigned = std::move(moved);
        QVERIFY(moved.empty());
        QCOMPARE(std::string(assigned.mode()), std::string("L"));
        QCOMPARE(std::string(assigned.copy().mode()), std::string("P"));
        QCOMPARE(std::string(assigned.convert("L").mode()), std::string("P"));
        QCOMPARE(assigned.copy().tobytes(), assigned.tobytes());
        QCOMPARE(assigned.convert("L").tobytes(), assigned.tobytes());
    }
    void gifValueCopyPreservesOpenedWrapper() {
        const Json oracle = genko::test::read_json(genko::test::test_data("pyref/gif/mixed-state.json"));
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/colored-global-gray-local.gif"));
        render::ImageAllocationBudget budget(1 << 20);
        {
            const auto opened = render::open_image(bytes);
            render::Image constructed(opened);
            render::Image assigned = render::Image::create("RGBA", {2, 3});
            assigned = opened;
            for (const auto* value : std::array<const render::Image*, 3>{&opened, &constructed, &assigned}) {
                QCOMPARE(std::string(value->mode()), oracle.at("opened").at("mode").get<std::string>());
                QCOMPARE(value->getbands(), oracle.at("opened").at("bands").get<std::vector<std::string>>());
                QCOMPARE(value->tobytes(), genko::core::a2b_base64(oracle.at("opened").at("data").get<std::string>()));
                QCOMPARE(value->convert("RGBA").tobytes(), genko::core::a2b_base64(oracle.at("opened").at("rgba").get<std::string>()));
                QCOMPARE(std::string(value->copy().mode()), std::string("P"));
                QCOMPARE(std::string(value->convert("L").mode()), std::string("P"));
            }
            constructed.putalpha(113);
            QCOMPARE(std::string(opened.mode()), std::string("L"));
            QCOMPARE(std::string(assigned.mode()), std::string("L"));
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void gifMixedCacheConsumersMatchPillow_data() {
        QTest::addColumn<QString>("order");
        for (const char* order : {"cold", "RGBA-first", "L-first", "patch-cold", "patch-RGBA-first", "patch-L-first"})
            QTest::newRow(order) << QString(order);
    }
    void gifMixedCacheConsumersMatchPillow() {
        QFETCH(QString, order);
        const Json oracle = genko::test::read_json(genko::test::test_data("pyref/gif/mixed-state.json"));
        const auto bytes = std::make_shared<const std::string>(genko::test::read_bytes(
            genko::test::test_data("pyref/gif/colored-global-gray-local.gif")));
        render::clear_render_caches();
        render::ImageAllocationBudget budget(1 << 20);
        {
            genko::core::Page page;
            const double w = 17 * 25.4 / 100, h = 19 * 25.4 / 100;
            page.spec = genko::core::PageSpec::custom(w, h, w, h, 0, 0, 0, 0, 0, 100, "color");
            genko::core::Layer raster;
            raster.role = genko::core::LayerRole::User;
            raster.kind = genko::core::LayerKind::Raster;
            raster.panel_clip = false;
            raster.raster_png = bytes;
            genko::core::Layer masked;
            masked.role = genko::core::LayerRole::User;
            masked.kind = genko::core::LayerKind::Fill;
            masked.panel_clip = false;
            masked.fill = Json::object({{"rgb", Json::array({31, 61, 91})}});
            masked.mask = genko::core::Mask{};
            masked.mask->png = bytes;
            const auto show_rgba = [&] { return render::layer_image(page, raster, 100); };
            const auto show_mask = [&] { return render::layer_image(page, masked, 100); };
            if (order.contains("RGBA-first")) {
                QCOMPARE(show_rgba().tobytes(), genko::core::a2b_base64(oracle.at("opened").at("rgba").get<std::string>()));
            }
            if (order.contains("L-first")) {
                QCOMPARE(show_mask().tobytes(), genko::core::a2b_base64(oracle.at("layer_mask").at("data").get<std::string>()));
            }
            if (order.startsWith("patch")) {
                genko::core::Layer patched;
                patched.role = genko::core::LayerRole::User;
                patched.panel_clip = false;
                genko::core::Patch patch;
                patch.png = bytes;
                patch.attrs = Json::object({{"box", Json::array({0, 0, w, h})}, {"mode", "mask"}, {"rgb", Json::array({31, 61, 91})}});
                patched.patches.push_back(patch);
                QVERIFY(!oracle.at("mask_patch").at("ok").get<bool>());
                for (int attempt = 0; attempt < 2; ++attempt) {
                    bool refused = false;
                    try { (void)render::layer_image(page, patched, 100); }
                    catch (const genko::core::Error& error) {
                        QCOMPARE(error.code(), std::string("value"));
                        QCOMPARE(std::string(error.what()), oracle.at("mask_patch").at("message").get<std::string>());
                        refused = true;
                    }
                    QVERIFY2(refused, "Pillow refuses a P core as an alpha mask even when the opened wrapper is L");
                }
            } else {
                for (int repeat = 0; repeat < 2; ++repeat) {
                    QCOMPARE(show_mask().tobytes(), genko::core::a2b_base64(oracle.at("layer_mask").at("data").get<std::string>()));
                    QCOMPARE(show_rgba().tobytes(), genko::core::a2b_base64(oracle.at("opened").at("rgba").get<std::string>()));
                }
            }
            render::clear_render_caches();
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void gifMixedCacheParallelColdConsumersMatchPillow() {
        const Json oracle = genko::test::read_json(genko::test::test_data("pyref/gif/mixed-state.json"));
        const auto bytes = std::make_shared<const std::string>(genko::test::read_bytes(
            genko::test::test_data("pyref/gif/colored-global-gray-local.gif")));
        genko::core::Page page;
        const double w = 17 * 25.4 / 100, h = 19 * 25.4 / 100;
        page.spec = genko::core::PageSpec::custom(w, h, w, h, 0, 0, 0, 0, 0, 100, "color");
        genko::core::Layer raster;
        raster.role = genko::core::LayerRole::User;
        raster.kind = genko::core::LayerKind::Raster;
        raster.panel_clip = false;
        raster.raster_png = bytes;
        genko::core::Layer masked;
        masked.role = genko::core::LayerRole::User;
        masked.kind = genko::core::LayerKind::Fill;
        masked.panel_clip = false;
        masked.fill = Json::object({{"rgb", Json::array({31, 61, 91})}});
        masked.mask = genko::core::Mask{};
        masked.mask->png = bytes;
        const std::string expected_rgba = genko::core::a2b_base64(oracle.at("opened").at("rgba").get<std::string>());
        const std::string expected_mask = genko::core::a2b_base64(oracle.at("layer_mask").at("data").get<std::string>());
        for (int batch = 0; batch < 8; ++batch) {
            render::clear_render_caches();
            std::barrier start(4);
            std::array<std::string, 4> errors;
            std::vector<std::thread> threads;
            for (std::size_t i = 0; i < errors.size(); ++i) {
                threads.emplace_back([&, i] {
                    start.arrive_and_wait();
                    try {
                        render::ImageAllocationBudget budget(1 << 20);
                        for (int repeat = 0; repeat < 4; ++repeat) {
                            const bool mask = (i + static_cast<std::size_t>(repeat)) % 2 == 0;
                            const auto image = render::layer_image(page, mask ? masked : raster, 100);
                            if (image.tobytes() != (mask ? expected_mask : expected_rgba)) {
                                errors[i] = "parallel cold/warm consumer differs from fixed Pillow pixels";
                                break;
                            }
                        }
                    } catch (const std::exception& error) { errors[i] = error.what(); }
                });
            }
            for (auto& thread : threads) thread.join();
            render::clear_render_caches();
            for (const auto& error : errors) QVERIFY2(error.empty(), error.c_str());
        }
    }
    void gifMixedCopyAndAlphaBudgetFailurePreserveSource() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/colored-global-gray-local.gif"));
        render::ImageAllocationBudget budget(24000);
        {
            auto opened = render::open_image(bytes);
            auto target = render::Image::create("RGBA", {2, 3}, render::Ink{31, 61, 91, 113});
            const auto original = opened.tobytes();
            const auto target_pixels = target.tobytes();
            auto held = render::Image::create_blank("L", {static_cast<int>(24000 - budget.live() - 100), 1});
            const auto live = budget.live();
            for (int operation = 0; operation < 3; ++operation) {
                bool refused = false;
                try {
                    if (operation == 0) { render::Image copy(opened); }
                    else if (operation == 1) target = opened;
                    else opened.putalpha(113);
                } catch (const genko::core::OpError&) { refused = true; }
                QVERIFY2(refused, "copy or alpha output must respect the active allocation budget");
                QCOMPARE(budget.live(), live);
                QCOMPARE(std::string(opened.mode()), std::string("L"));
                QCOMPARE(opened.tobytes(), original);
                QCOMPARE(std::string(target.mode()), std::string("RGBA"));
                QCOMPARE(target.tobytes(), target_pixels);
            }
            held = render::Image{};
            target = opened;
            QCOMPARE(std::string(target.mode()), std::string("L"));
            opened.putalpha(113);
            QCOMPARE(std::string(opened.mode()), std::string("LA"));
            QCOMPARE(std::string(target.mode()), std::string("L"));
        }
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void gifMixedPutalphaMatchesPillow_data() {
        QTest::addColumn<Json>("item");
        const Json cases = genko::test::read_json(genko::test::test_data("pyref/gif/mixed-state.json")).at("cases");
        QCOMPARE(cases.size(), std::size_t{2});
        for (const auto& item : cases)
            QTest::newRow(item.at("kind").get<std::string>().c_str()) << item;
    }
    void gifMixedPutalphaMatchesPillow() {
        QFETCH(Json, item);
        auto image = render::open_image(genko::test::read_bytes(genko::test::test_data("pyref/gif/colored-global-gray-local.gif")));
        QCOMPARE(std::string(image.mode()), std::string("L"));
        try {
            if (item.at("kind") == "constant") image.putalpha(113);
            else {
                std::string pixels(static_cast<std::size_t>(image.width()) * image.height(), '\0');
                for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<char>((i * 37) % 256);
                image.putalpha(render::Image::frombytes("L", image.size(), pixels));
            }
        } catch (const genko::core::Error& error) { QFAIL(error.what()); }
        QCOMPARE(std::string(image.mode()), item.at("mode").get<std::string>());
        QCOMPARE(image.getbands(), item.at("bands").get<std::vector<std::string>>());
        QCOMPARE(image.tobytes(), genko::core::a2b_base64(item.at("data").get<std::string>()));
        QCOMPARE(image.convert("RGBA").tobytes(), genko::core::a2b_base64(item.at("rgba").get<std::string>()));
        QCOMPARE(image.getchannel("A").tobytes(), image.getchannel(1).tobytes());
    }
    void gifCapsLogicalAndExpandedSizes() {
        auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/palette.gif"));
        render::ImageAllocationBudget budget(1 << 20);
        for (int layout = 0; layout < 2; ++layout) {
            if (layout == 1) { bytes[6] = 1; bytes[7] = 0; bytes[8] = 1; bytes[9] = 0; }
            bool refused = false;
            try { (void)render::open_image(bytes, render::PngLimits{20}); }
            catch (const genko::core::Error& error) { refused = error.code() == "image_too_large"; }
            QVERIFY(refused);
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
    }
    void gifRowsStayReservedThroughImageLifetime() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/palette.gif"));
        render::ImageAllocationBudget budget(1 << 20);
        auto im = render::open_image(bytes);
        QCOMPARE(budget.live(), static_cast<std::uint64_t>(im.width()) * im.height() +
                                static_cast<std::uint64_t>(im.height()) * sizeof(void*));
        im = render::Image{};
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void gifActiveBudgetAndDecodeFailuresRecover() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/gif/palette.gif"));
        {
            render::ImageAllocationBudget budget(12000);
            auto held = render::Image::create_blank("RGBA", {10, 10});
            const auto before = budget.live();
            bool refused = false;
            try { (void)render::open_image(bytes); }
            catch (const genko::core::OpError&) { refused = true; }
            QVERIFY(refused);
            QCOMPARE(budget.live(), before);
        }
        render::ImageAllocationBudget budget(1 << 20);
        auto held = render::Image::create_blank("RGBA", {10, 10});
        const auto before = budget.live();
        for (const auto& c : genko::test::read_json(genko::test::test_data("pyref/gif/errors.json"))) {
            bool refused = false;
            try { (void)render::open_image(genko::core::a2b_base64(c.at("b64").get<std::string>())); }
            catch (const genko::core::Error&) { refused = true; }
            QVERIFY(refused);
            QCOMPARE(budget.live(), before);
        }
        QVERIFY(!render::open_image(bytes).empty());
        QCOMPARE(budget.live(), before);
        held = render::Image{};
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void bmpPixelsMatchPillow_data() {
        QTest::addColumn<int>("index");
        const Json data = genko::test::read_json(genko::test::test_data("pyref/bmp/tables.json"));
        for (std::size_t i = 0; i < data.size(); ++i)
            QTest::newRow(data[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
    }
    void bmpPixelsMatchPillow() {
        QFETCH(int, index);
        const Json data = genko::test::read_json(genko::test::test_data("pyref/bmp/tables.json"));
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

    void bmpGuardsDimensionsTruncationAndBudget() {
        const auto source = genko::test::read_bytes(genko::test::test_data("pyref/bmp/rgb24.bmp"));
        const auto put32 = [](std::string& data, std::size_t at, std::uint32_t value) {
            for (std::size_t i = 0; i < 4; ++i) data[at + i] = static_cast<char>((value >> (i * 8)) & 0xff);
        };
        auto invalid = source;
        invalid.resize(53);
        const auto rejected = [](const std::string& bytes) {
            try { (void)render::open_image(bytes); }
            catch (const genko::core::Error& error) { return error.code(); }
            return std::string("accepted");
        };
        QCOMPARE(rejected(invalid), std::string("format"));  // Same truncated DIB header as Pillow.
        invalid = source.substr(0, source.size() - 4);  // The last 3 bytes are padding, not pixels.
        QCOMPARE(rejected(invalid), std::string("format"));
        for (const std::size_t field : {std::size_t{18}, std::size_t{22}}) {
            invalid = source; put32(invalid, field, 0);
            QCOMPARE(rejected(invalid), std::string("unidentified_image"));
        }
        invalid = source; put32(invalid, 10, 20);
        QCOMPARE(rejected(invalid), std::string("unidentified_image"));
        invalid = source; invalid[26] = 2;
        QCOMPARE(rejected(invalid), std::string("unidentified_image"));
        bool capped = false;
        try { (void)render::open_image(source, render::PngLimits{34}); }
        catch (const genko::core::Error& error) { capped = error.code() == "image_too_large" && std::string(error.what()) == "image has too many pixels"; }
        QVERIFY(capped);
        invalid = source; put32(invalid, 22, 300000000);
        capped = false;
        try { (void)render::open_image(invalid); }
        catch (const genko::core::Error& error) { capped = error.code() == "image_too_large" && std::string(error.what()) == "image has too many pixels"; }
        QVERIFY(capped);
        {
            render::ImageAllocationBudget budget(32);
            bool exhausted = false;
            try { (void)render::open_image(source); }
            catch (const genko::core::OpError& error) { exhausted = std::string(error.what()).find("too many masks") != std::string::npos; }
            QVERIFY(exhausted);
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
        {
            render::ImageAllocationBudget budget(source.size() + 1);
            bool exhausted = false;
            try { (void)render::open_image(source); }
            catch (const genko::core::OpError& error) { exhausted = std::string(error.what()).find("too many masks") != std::string::npos; }
            QVERIFY(exhausted);
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
        QCOMPARE(render::open_image(source).width(), 7);
    }

    void bmpEveryShortPixelPrefixIsRefused() {
        const auto source = genko::test::read_bytes(genko::test::test_data("pyref/bmp/rgb24.bmp"));
        // Omit actual pixels, not the three legal last-row padding bytes.
        for (std::size_t n = 0; n < source.size() - 3; ++n) {
            bool refused = false;
            try { (void)render::open_image(source.substr(0, n)); }
            catch (const genko::core::Error& error) { refused = error.code() != "not_yet_ported"; }
            QVERIFY2(refused, ("accepted truncated BMP prefix " + std::to_string(n)).c_str());
        }
        QCOMPARE(render::open_image(source).width(), 7);
    }

    void bmpClassifiedErrorsMatchPillow_data() {
        QTest::addColumn<int>("index");
        const auto data = genko::test::read_json(genko::test::test_data("pyref/bmp/errors.json"));
        for (std::size_t i = 0; i < data.size(); ++i) QTest::newRow(data.at(i).at("name").get<std::string>().c_str()) << static_cast<int>(i);
    }
    void bmpClassifiedErrorsMatchPillow() {
        QFETCH(int, index);
        const auto data = genko::test::read_json(genko::test::test_data("pyref/bmp/errors.json")).at(static_cast<std::size_t>(index));
        const auto bytes = genko::core::a2b_base64(data.at("b64").get<std::string>());
        bool refused = false;
        try { (void)render::open_image(bytes); }
        catch (const genko::core::Error& error) {
            refused = true;
            QCOMPARE(error.code(), data.at("code").get<std::string>());
            QCOMPARE(std::string(error.what()), data.at("message").get<std::string>());
        }
        QVERIFY(refused);
    }
    void bmpAccountsForRowPointersBeforeAllocation() {
        auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/bmp/gray8.bmp"));
        const auto put32 = [](std::string& b, std::size_t at, std::uint32_t v) { for (std::size_t i=0;i<4;++i) b.at(at+i)=static_cast<char>((v>>(8*i))&255U); };
        // Keep only the small header and palette; no enormous payload is created.
        bytes.resize(1078); put32(bytes,2,1078); put32(bytes,18,1); put32(bytes,30,1); put32(bytes,34,0);
        {
            render::ImageAllocationBudget budget(4096);
            put32(bytes,22,400000000);
            bool beforeAllocation = false;
            try { (void)render::open_image(bytes); }
            catch (const genko::core::Error& error) { beforeAllocation = error.code()=="memory"; }
            catch (const genko::core::OpError&) { /* Safe RED: old pixel reservation fails before calloc. */ }
            QVERIFY(beforeAllocation);
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
        {
            render::ImageAllocationBudget budget(128 << 10);
            put32(bytes,22,20000);
            bool withActiveBudget = false;
            try { (void)render::open_image(bytes); }
            catch (const genko::core::OpError& error) { withActiveBudget = std::string(error.what()).find("too many masks")!=std::string::npos; }
            catch (const genko::core::Error&) {}
            QVERIFY(withActiveBudget);
            QCOMPARE(budget.live(), std::uint64_t{0});
        }
    }
    void bmpKeepsRowReservationsUntilImageDestruction() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/bmp/rgb24.bmp"));
        render::ImageAllocationBudget budget(4096);
        auto a = render::open_image(bytes);
        const std::uint64_t owned = 7 * 5 * 4 + 5 * sizeof(void*);
        QCOMPARE(budget.live(), owned);
        auto b = render::open_image(bytes);
        QCOMPARE(budget.live(), owned * 2);
        a = render::Image{};
        QCOMPARE(budget.live(), owned);
        b = render::Image{};
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
    void bmpActiveBudgetAndFailedRleReleaseEverything() {
        const auto bytes = genko::test::read_bytes(genko::test::test_data("pyref/bmp/rgb24.bmp"));
        {
            const std::uint64_t limit = 400 + bytes.size() + 5 * sizeof(void*) + 7 * 5 * 4 - 1;
            render::ImageAllocationBudget budget(limit);
            auto held = render::Image::create_blank("RGBA", {10, 10});
            const auto before = budget.live();
            bool refused = false;
            try { (void)render::open_image(bytes); }
            catch (const genko::core::OpError& error) { refused = std::string(error.what()).find("too many masks") != std::string::npos; }
            QVERIFY(refused);
            QCOMPARE(budget.live(), before);
            held = render::Image{};
            QCOMPARE(budget.live(), std::uint64_t{0});
            QVERIFY(!render::open_image(bytes).empty());
        }
        const auto cases = genko::test::read_json(genko::test::test_data("pyref/bmp/errors.json"));
        render::ImageAllocationBudget budget(1 << 20);
        auto held = render::Image::create_blank("RGBA", {10, 10});
        const auto before = budget.live();
        for (const auto& c : cases) {
            if (c.at("stage") != "load") continue;
            bool refused = false;
            try { (void)render::open_image(genko::core::a2b_base64(c.at("b64").get<std::string>())); }
            catch (const genko::core::Error&) { refused = true; }
            QVERIFY(refused);
            QCOMPARE(budget.live(), before);
        }
        held = render::Image{};
        QCOMPARE(budget.live(), std::uint64_t{0});
    }
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
        QCOMPARE(code_of("GIF89a...."), std::string("unidentified_image"));  // native reader rejects incomplete logical descriptor
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

    // The exports' PNG writer (formats::PngWriter, a strip written row by row): a picture whose rows hold more bytes
    // than an int counts is refused before anything is made for it (not cut to a smaller count).
    void theRowWriterRefusesRowsPastAnInt() {
        for (const auto& [width, mode] : {std::pair{1'000'000'000, "RGBA"}, std::pair{715'827'883, "RGB"}}) {
            QString said;
            try {
                genko::formats::PngWriter writer(render::Size{width, 1}, mode);
            } catch (const genko::core::Error& error) {
                said = QString::fromStdString(error.code() + ": " + error.what());
            } catch (const std::exception& error) {
                said = QStringLiteral("std::exception: %1").arg(QString::fromUtf8(error.what()));
            }
            QCOMPARE(said, QStringLiteral("image_too_large: image too large to write as PNG"));
        }
        // (the widest that fits: made, its rows taken)
        genko::formats::PngWriter fits(render::Size{3, 2}, "RGB");
        fits.add(render::Image::create("RGB", render::Size{3, 2}, render::Ink{1, 2, 3}));
        QVERIFY(fits.finish().starts_with("\x89PNG"));
    }
};

QTEST_GUILESS_MAIN(TestPng)
#include "test_png.moc"
