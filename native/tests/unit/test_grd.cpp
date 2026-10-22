// PC4 E4 (gradient.presets.importGrd, SPEC.md COLOR-09「グラデーションのプリセット」): Photoshop's .grd files (version 5) as
// PhotoCraft v0.2.0's crates/psd/src/grd.rs tests them — written by the writer (upstream's, ported: no Adobe file is used)
// and read back the same (a name past ASCII, the foreground and background, a midpoint, the transparency stops, a noise
// gradient), what is not a .grd refused, every cut and every flipped byte of one read or refused without harm; and the
// gradients made Genko's presets (sRGB colours: RGB, HSB, CMYK, Lab and grey; "foreground" and "background"; the
// transparency 0..1; what is not kept as it is said: a noise gradient left out, midpoints, more than 16 stops), and
// `genko gradients import-grd`.

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "render/gradient_presets.hpp"
#include "render/grd.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace grd = render::grd;

namespace {

grd::Colour rgb(double r, double g, double b) { return grd::Colour{grd::Colour::Kind::Rgb, {r, g, b, 0}}; }

std::vector<grd::Gradient> sample() {
    grd::Gradient sunset;
    sunset.name = "Sunset ✓";
    sunset.stops = {{0.0, 0.5, grd::Colour{grd::Colour::Kind::Foreground, {}}},
                    {0.25, 0.3, rgb(1.0, 0.5, 0.0)},
                    {1.0, 0.5, grd::Colour{grd::Colour::Kind::Background, {}}}};
    sunset.opacity = {{0.0, 1.0, 0.5}, {1.0, 0.25, 0.5}};
    grd::Gradient noise;
    noise.name = "Noise";
    noise.noise = true;
    return {sunset, noise};
}

bool refused(std::string_view data) {
    try {
        (void)grd::read(data);
    } catch (const core::Error&) {
        return true;
    }
    return false;
}

}  // namespace

class TestGrd : public QObject {
    Q_OBJECT

private slots:
    void roundTrip() {
        const std::string bytes = grd::write(sample());
        QCOMPARE(bytes.substr(0, 4), std::string("8BGR"));
        const auto back = grd::read(bytes);
        QCOMPARE(back.size(), std::size_t(2));
        QCOMPARE(back[0].name, std::string("Sunset ✓"));
        QCOMPARE(back[0].stops.size(), std::size_t(3));
        QVERIFY(back[0].stops[0].colour.kind == grd::Colour::Kind::Foreground);
        QVERIFY(back[0].stops[2].colour.kind == grd::Colour::Kind::Background);
        QCOMPARE(back[0].stops[1].location, 0.25);
        QCOMPARE(back[0].stops[1].midpoint, 0.3);
        QCOMPARE(back[0].stops[1].colour.values[1], 0.5);
        QCOMPARE(back[0].opacity.size(), std::size_t(2));
        QCOMPARE(back[0].opacity[1].opacity, 0.25);
        QVERIFY(back[1].noise);
        QVERIFY(back == sample());
    }

    void hostileInput() {
        QVERIFY(refused(""));
        QVERIFY(refused(std::string("8BGR\0\x03", 6)));
        QVERIFY(refused(std::string("8BPS\0\x05", 6)));
        QVERIFY(refused(grd::write({})));  // (no gradients)
        const std::string bytes = grd::write(sample());
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) (void)refused(bytes.substr(0, cut));
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            std::string flipped = bytes;
            flipped[i] = static_cast<char>(flipped[i] ^ 0x5a);
            (void)refused(flipped);
        }
    }

    // (review P1-1) A file within its size whose descriptor would unpack to far more — a list or an object of millions of
    // one-byte values (some fifty times their bytes as values) — refused as too large before the memory is had, in seconds;
    // a file past 32 MB refused unread.
    void unpackedTooLargeIsRefused() {
        const auto u32 = [](std::string& out, std::uint32_t v) {
            for (int s = 24; s >= 0; s -= 8) out += static_cast<char>((v >> s) & 0xFF);
        };
        const auto head = [&](std::string& out) {
            out = "8BGR";
            out += std::string("\0\x05", 2);
            u32(out, 16);                   // (the descriptor's version)
            u32(out, 1);                    // (the class's name: one UTF-16 0)
            out += std::string("\0\0", 2);
            u32(out, 0);
            out += "null";
        };
        constexpr std::uint32_t kMany = 5'000'000;
        std::string list;
        head(list);
        u32(list, 1);
        u32(list, 0);
        list += "GrdL";
        list += "VlLs";
        u32(list, kMany);
        list.reserve(list.size() + std::size_t(kMany) * 5);
        for (std::uint32_t i = 0; i < kMany; ++i) list += std::string("bool\x01", 5);
        QVERIFY(list.size() < grd::kMaxBytes);
        std::string object;
        head(object);
        constexpr std::uint32_t kItems = 2'000'000;
        u32(object, kItems);
        object.reserve(object.size() + std::size_t(kItems) * 13);
        for (std::uint32_t i = 0; i < kItems; ++i) {
            u32(object, 0);
            object += "Item";
            object += std::string("bool\x01", 5);
        }
        QVERIFY(object.size() < grd::kMaxBytes);
        for (const std::string* data : {&list, &object}) {
            QElapsedTimer clock;
            clock.start();
            std::string said;
            try {
                (void)grd::read(*data);
            } catch (const core::Error& error) {
                said = std::string(error.code()) + ": " + error.what();
            }
            QVERIFY2(said.find("format") == 0 && said.find("too large") != std::string::npos, said.c_str());
            QVERIFY2(clock.elapsed() < 20000, qPrintable(QString::number(clock.elapsed())));
        }
        // a list whose count says more than the bytes hold: refused at once
        std::string lying;
        head(lying);
        u32(lying, 1);
        u32(lying, 0);
        lying += "GrdL";
        lying += "VlLs";
        u32(lying, 0xFFFFFFF0u);
        QVERIFY(refused(lying));
        // past 32 MB: refused unread
        std::string big = grd::write(sample());
        big.resize(grd::kMaxBytes + 1, '\0');
        try {
            (void)grd::read(big);
            QFAIL("a file past the size was read");
        } catch (const core::Error& error) {
            QVERIFY(std::string(error.what()).find("larger than 32 MB") != std::string::npos);
        }
    }

    // (review P3-3) a name past the presets' 200 characters cut (and said), one of spaces alone named グラデーション
    void namesAsPresetsKeepThem() {
        grd::Gradient long_one;
        long_one.name = std::string(250, 'a') + "  ";
        long_one.stops = {{0.0, 0.5, rgb(0, 0, 0)}, {1.0, 0.5, rgb(1, 1, 1)}};
        grd::Gradient blank = long_one;
        blank.name = "   ";
        grd::Gradient twin = long_one;  // (the same long name: its number fits)
        std::vector<std::string> warnings;
        const Json items = grd::to_presets(grd::read(grd::write({long_one, blank, twin})), warnings);
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items[0]["name"].get<std::string>(), std::string(grd::kNameChars, 'a'));
        QCOMPARE(items[1]["name"], Json("グラデーション"));
        QCOMPARE(items[2]["name"].get<std::string>(), std::string(grd::kNameChars, 'a') + " 2");
        QVERIFY(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.find("切りました") != std::string::npos; }));
        for (const Json& item : items) render::gradient_presets::kind().check(item);
        QCOMPARE(grd::kept_name("  日本語の名前 ", "x"), std::string("日本語の名前"));
        std::string kana;
        for (int i = 0; i < 300; ++i) kana += "あ";
        bool cut = false;
        const std::string kept = grd::kept_name(kana, "x", &cut);
        QVERIFY(cut);
        QCOMPARE(kept.size(), grd::kNameChars * 3);  // (cut at a character, never inside one)
    }

    // the gradients as Genko's presets: sRGB colours, the live colours by their words, the transparency 0..1, and what is
    // not kept as it is said
    void asPresets() {
        std::vector<grd::Gradient> all = sample();
        grd::Gradient models;
        models.name = "Models";
        models.stops = {{0.0, 0.5, grd::Colour{grd::Colour::Kind::Hsb, {120, 1, 1, 0}}},
                        {0.25, 0.5, grd::Colour{grd::Colour::Kind::Cmyk, {0, 1, 1, 0}}},
                        {0.5, 0.5, grd::Colour{grd::Colour::Kind::Lab, {100, 0, 0, 0}}},
                        {1.0, 0.5, grd::Colour{grd::Colour::Kind::Gray, {0.25, 0, 0, 0}}}};
        all.push_back(models);
        grd::Gradient many;
        many.name = "Sunset ✓";  // (a name taken: a number)
        for (int i = 0; i < 20; ++i) many.stops.push_back({i / 19.0, 0.5, rgb(i / 19.0, 0, 0)});
        all.push_back(many);
        std::vector<std::string> warnings;
        const Json items = grd::to_presets(grd::read(grd::write(all)), warnings);
        QCOMPARE(items.size(), std::size_t(3));  // (the noise left out)
        QCOMPARE(items[0]["name"], Json("Sunset ✓"));
        QCOMPARE(items[0]["stops"][0][1], Json("foreground"));
        QCOMPARE(items[0]["stops"][1][1], Json::parse("[255, 128, 0]"));
        QCOMPARE(items[0]["stops"][2][1], Json("background"));
        QCOMPARE(items[0]["transparency"], Json::parse("[[0.0, 1.0], [1.0, 0.25]]"));
        QVERIFY(!items[1].contains("transparency"));
        // (the writer keeps only RGB: the other models are read back as black; read straight from their descriptors below)
        QCOMPARE(items[2]["name"], Json("Sunset ✓ 2"));
        QCOMPARE(items[2]["stops"].size(), std::size_t(16));
        std::string said;
        for (const std::string& w : warnings) said += w + "\n";
        QVERIFY2(said.find("ノイズ") != std::string::npos, said.c_str());
        QVERIFY2(said.find("中間点") != std::string::npos, said.c_str());
        QVERIFY2(said.find("16 個") != std::string::npos, said.c_str());
        for (const Json& item : items) render::gradient_presets::kind().check(item);
        // the other models made sRGB
        warnings.clear();
        const Json made = grd::to_presets({models}, warnings);
        QCOMPARE(made[0]["stops"][0][1], Json::parse("[0, 255, 0]"));  // (HSB 120°, full: green)
        QCOMPARE(made[0]["stops"][1][1], Json::parse("[255, 0, 0]"));  // (CMYK 0 100 100 0: red by the simple sums)
        QCOMPARE(made[0]["stops"][2][1], Json::parse("[255, 255, 255]"));  // (Lab L 100: white)
        QCOMPARE(made[0]["stops"][3][1], Json::parse("[191, 191, 191]"));  // (grey ink 25 %)
        QVERIFY(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.find("sRGB") != std::string::npos; }));
    }

    // `genko gradients import-grd`: a group of the file's name with its presets, and what was not kept as it is
    void importedByTheCommandLine() {
        QTemporaryDir scratch;
        const QString file = scratch.path() + "/夕焼け.grd";
        {
            std::ofstream out(file.toStdString(), std::ios::binary);
            out << grd::write(sample());
        }
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), scratch.path() + "/config");
        auto ran = genko::test::run_genko({QStringLiteral("gradients"), QStringLiteral("import-grd"), file}, env);
        QVERIFY2(ran.finished && ran.exit_code == 0, qPrintable(ran.described()));
        const Json got = genko::test::one_line(ran.out);
        QCOMPARE(got["group"], Json("夕焼け"));
        QCOMPARE(got["presets"], Json::parse(R"(["Sunset ✓"])"));
        QVERIFY(got["warnings"].dump().find("ノイズ") != std::string::npos);
        const Json listed = genko::test::one_line(genko::test::run_genko({QStringLiteral("gradients"), QStringLiteral("list")}, env).out);
        QCOMPARE(listed["groups"].back()["name"], Json("夕焼け"));
        // again: the group put in its place, not doubled
        (void)genko::test::run_genko({QStringLiteral("gradients"), QStringLiteral("import-grd"), file}, env);
        const Json again = genko::test::one_line(genko::test::run_genko({QStringLiteral("gradients"), QStringLiteral("list")}, env).out);
        QCOMPARE(again["groups"].size(), listed["groups"].size());
        // not a .grd
        {
            std::ofstream out((scratch.path() + "/no.grd").toStdString(), std::ios::binary);
            out << "8BPS";
        }
        ran = genko::test::run_genko({QStringLiteral("gradients"), QStringLiteral("import-grd"), scratch.path() + "/no.grd"}, env);
        QVERIFY(ran.finished && ran.exit_code != 0);
    }
};

QTEST_GUILESS_MAIN(TestGrd)
#include "test_grd.moc"
