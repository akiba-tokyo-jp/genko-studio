// PC2a (SPEC.md COLOR-04, schema-v4.md「補正（adjustments@1）」: color_lookup's 3D table): the LUT files a person gives
// read as PhotoCraft reads them (cms/lutfile.rs's tests: a .cube written and read back, red varying fastest, a 1D .cube
// spread to 33³, a .3dl's blue-fastest rows and output depth, a .look's hexadecimal floats, broken ones refused), the
// eight generated looks in 0..1, the lookup's identity both ways (compose/adjust.rs 605-616) and its dither's balance
// (637-642), and the table as a book holds it (GKLU: the same table the same bytes; what is not one refused); the
// numbers read and written whatever the locale.

#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "../support/comma_locale.hpp"

using namespace genko;
namespace lut3d = core::lut3d;

class TestLut3d : public QObject {
    Q_OBJECT

private slots:
    void cubeRoundTripAndRedFastest() {
        lut3d::Table id = lut3d::identity(5);
        id.title = "Id";
        const lut3d::Table back = lut3d::parse_cube(lut3d::write_cube(id));
        QCOMPARE(back.size, std::size_t(5));
        QCOMPARE(QString::fromStdString(back.title), QStringLiteral("Id"));
        for (std::size_t i = 0; i < back.data.size(); ++i) QVERIFY(std::abs(back.data[i] - id.data[i]) < 1e-5);
        // (red varies fastest: entry 1 is (0.25, 0, 0))
        QCOMPARE(back.data[3], 0.25f);
        QCOMPARE(back.data[4], 0.0f);
        QCOMPARE(back.data[5], 0.0f);
        // comments, keywords it does not know and a domain are passed over
        const lut3d::Table more = lut3d::parse_cube("# a comment\nTITLE \"x\"\nLUT_IN_VIDEO_RANGE\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
                                                    "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1 # end\n");
        QCOMPARE(more.size, std::size_t(2));
        QCOMPARE(more.data, lut3d::identity(2).data);
    }

    void cube1dExpands() {
        const lut3d::Table t = lut3d::parse_cube("LUT_1D_SIZE 2\n0 0 0\n0.5 1 1\n");
        QCOMPARE(t.size, std::size_t(33));
        const float* last = t.data.data() + t.data.size() - 3;
        QVERIFY(std::abs(last[0] - 0.5f) < 1e-6 && std::abs(last[1] - 1.0f) < 1e-6);
    }

    void threeDlBlueFastestAndBitDepth() {
        std::string text = "0 1023\n";
        for (int r = 0; r < 2; ++r)
            for (int g = 0; g < 2; ++g)
                for (int b = 0; b < 2; ++b) text += std::to_string(r * 4095) + " " + std::to_string(g * 4095) + " " + std::to_string(b * 4095) + "\n";
        const lut3d::Table t = lut3d::parse_3dl(text);
        QCOMPARE(t.size, std::size_t(2));
        QCOMPARE(t.data, lut3d::identity(2).data);
        QCOMPARE(lut3d::parse("A.3DL", text).data, t.data);  // (the format by the name's extension, any case)
    }

    void lookHexFloats() {
        const lut3d::Table id = lut3d::identity(2);
        std::string hex;
        for (const float v : id.data) {
            unsigned char b[4];
            std::memcpy(b, &v, 4);
            for (const unsigned char c : b) hex += QString::asprintf("%02X", c).toStdString();
        }
        const std::string xml = "<?xml version=\"1.0\"?><look><LUT><size>\"2\"</size><data>\"" + hex + "\"</data></LUT></look>";
        QCOMPARE(lut3d::parse("a.look", xml).data, id.data);
        QCOMPARE(lut3d::parse("any.cube", xml).data, id.data);  // (a text starting with "<" is a .look)
    }

    void malformedRefused() {
        for (const auto& [name, text] : std::vector<std::pair<std::string, std::string>>{
                 {"a.cube", "LUT_3D_SIZE 2\n0 0 0\n"},
                 {"a.cube", "garbage"},
                 {"a.cube", "LUT_3D_SIZE 99999\n"},
                 {"a.cube", "LUT_3D_SIZE 2\n0 0\n"},
                 {"a.cube", "LUT_3D_SIZE x\n"},
                 {"a.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 nan\n"},
                 {"a.3dl", "1 2 3\n4 5 6\n"},
                 {"a.look", "<look></look>"},
             }) {
            QVERIFY_EXCEPTION_THROWN(lut3d::parse(name, text), core::Error);
        }
        try {
            (void)lut3d::parse_cube("LUT_3D_SIZE 2\n0 0 0\n");
            QFAIL("refused");
        } catch (const core::Error& error) {
            QCOMPARE(QString::fromUtf8(error.what()), QStringLiteral("expected 8 entries, found 1"));
        }
    }

    // A file's numbers are read as Rust reads them (a sign, ".5", "5.", an exponent; past a float's range an infinity —
    // refused as not finite — or 0; no hexadecimal), and a table is written with a point, whatever the locale: the app and
    // the command line run in the environment's (a German or French one has a comma for the decimal point).
    void numbersWhateverTheLocale() {
        const auto check = [] {
            const lut3d::Table t = lut3d::parse_cube("LUT_3D_SIZE 2\n0.0000 0.0000 0.0000\n+.5 0 0\n0 1. 0\n1 1 0\n0 0 5e-1\n1 0 1e-60\n0 1 1\n1 1 1.0E0\n");
            QCOMPARE(t.data[3], 0.5f);
            QCOMPARE(t.data[7], 1.0f);
            QCOMPARE(t.data[14], 0.5f);
            QCOMPARE(t.data[17], 0.0f);
            QCOMPARE(t.data[23], 1.0f);
            for (const char* bad : {"1e40", "-inf", "nan", "0x1p-1", "0,5", "+-1", "1e", ""}) {
                const std::string text = std::string("LUT_3D_SIZE 2\n") + bad + " 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
                QVERIFY_EXCEPTION_THROWN(lut3d::parse_cube(text), core::Error);
            }
            // a .3dl's numbers too
            std::string dl = "0 1023\n";
            for (int r = 0; r < 2; ++r)
                for (int g = 0; g < 2; ++g)
                    for (int b = 0; b < 2; ++b) dl += std::to_string(r * 1023) + ".0 " + std::to_string(g * 1023) + " " + std::to_string(b * 1023) + "\n";
            QCOMPARE(lut3d::parse_3dl(dl).data, lut3d::identity(2).data);
            lut3d::Table half = lut3d::identity(2);
            half.data[3] = 0.5f;
            const std::string written = lut3d::write_cube(half);
            QVERIFY2(written.find(',') == std::string::npos, written.c_str());
            QVERIFY2(written.find("\n0.500000 0.000000 0.000000\n") != std::string::npos, written.c_str());
            QCOMPARE(lut3d::parse_cube(written).data, half.data);
        };
        check();  // (the C locale)
        const genko::test::CommaLocale comma;
        QVERIFY2(comma.ok(), "no locale with a comma for the decimal point could be set (de_DE, fr_FR, or made by localedef)");
        check();
    }

    void builtinsInRange() {
        QCOMPARE(lut3d::looks().size(), std::size_t(8));
        for (const auto& look : lut3d::looks()) {
            const lut3d::Table* t = lut3d::builtin(look.id);
            QVERIFY(t != nullptr);
            QCOMPARE(t->data.size(), std::size_t(33 * 33 * 33 * 3));
            for (const float v : t->data) QVERIFY(v >= 0.0f && v <= 1.0f);
            QVERIFY(lut3d::builtin(look.id) == t);  // (made once)
        }
        QVERIFY(lut3d::builtin("nope") == nullptr);
        // (warm: red lifted, blue lowered — the look's own formula at the grid's corners)
        const lut3d::Table* warm = lut3d::builtin("warm");
        QVERIFY(std::abs(warm->data[0] - 0.08f) < 1e-6 && warm->data[2] == 0.0f);
    }

    void identityBothInterpolations() {
        const lut3d::Table t = lut3d::identity(5);
        for (const std::array<double, 3> c : {std::array<double, 3>{0.1, 0.5, 0.9}, {0.33, 0.77, 0.0}, {1.0, 1.0, 1.0}}) {
            for (const bool tet : {false, true}) {
                const auto o = lut3d::sample(t, c, tet);
                for (int k = 0; k < 3; ++k) QVERIFY2(std::abs(o[k] - c[k]) < 1e-5, qPrintable(QString("%1 %2").arg(k).arg(tet)));
            }
        }
        // (a colour beyond 0..1 is held to it first)
        const auto held = lut3d::sample(t, {1.5, -0.2, 0.5}, false);
        QVERIFY(std::abs(held[0] - 1.0) < 1e-6 && std::abs(held[1]) < 1e-6);
    }

    void ditherBalanced() {
        double sum = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                sum += lut3d::bayer4(x, y);
                QVERIFY(std::abs(lut3d::bayer4(x, y)) < 0.5);
            }
        QVERIFY(std::abs(sum) < 1e-9);
        QCOMPARE(lut3d::bayer4(-4, -4), lut3d::bayer4(0, 0));
    }

    void assetBytes() {
        const lut3d::Table* look = lut3d::builtin("tealOrange");
        const std::string a = lut3d::encode(*look), b = lut3d::encode(*look);
        QVERIFY(a == b);
        QVERIFY(a.starts_with("GKLU"));
        const lut3d::Table back = lut3d::decode(a);
        QCOMPARE(back.size, std::size_t(33));
        QCOMPARE(back.data, look->data);
        QVERIFY(core::bytes_ref(a) != core::bytes_ref(lut3d::encode(lut3d::identity(33))));
        std::string bad = a;
        bad[4] = 2;  // (another version)
        QVERIFY_EXCEPTION_THROWN(lut3d::decode(bad), core::Error);
        bad = a;
        bad[5] = static_cast<char>(130);  // (a size past 129)
        QVERIFY_EXCEPTION_THROWN(lut3d::decode(bad), core::Error);
        QVERIFY_EXCEPTION_THROWN(lut3d::decode(a.substr(0, a.size() - 3)), core::Error);
        QVERIFY_EXCEPTION_THROWN(lut3d::decode(a + "x"), core::Error);
        QVERIFY_EXCEPTION_THROWN(lut3d::decode("PNG"), core::Error);
        // the tables an op made, kept a few at a time until its book takes them
        const auto bytes = std::make_shared<const std::string>(a);
        lut3d::remember(core::bytes_ref(a), bytes);
        QVERIFY(lut3d::remembered(core::bytes_ref(a)) == bytes);
        QVERIFY(lut3d::decoded(core::bytes_ref(a), bytes)->data == look->data);
    }
};

QTEST_GUILESS_MAIN(TestLut3d)
#include "test_lut3d.moc"
