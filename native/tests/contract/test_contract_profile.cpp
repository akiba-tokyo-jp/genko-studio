// PC2d (SPEC.md COLOR-04「取り込む画像のプロファイル」「プロファイルの情報」) through the command line, as a person or an
// agent uses it: put_raster and import_psd with the C++ build's own embedded_profile — keep (Python's: the numbers as
// they are), convert (from the profile the file carries into the page's space), assign (the numbers taken as the op's
// profile's) — onto an sRGB page, a Display P3 page and a CMYK page: the values the layer holds there (inspect --pixel
// --layer), the op's report, a refusal that changes nothing; and `genko inspect --profile` (a built-in, a page's, one
// the book holds by its sha256; one that is none). (Python has neither: this build's own contract.)

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <zlib.h>

#include <array>
#include <cmath>
#include <string>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

std::string be32(std::uint32_t v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

// A PNG with an ICC profile in it (an iCCP chunk after the header, as a painting app writes one).
std::string with_icc(const std::string& png, const std::string& icc) {
    uLongf size = compressBound(static_cast<uLong>(icc.size()));
    std::string packed(size, '\0');
    compress2(reinterpret_cast<Bytef*>(packed.data()), &size, reinterpret_cast<const Bytef*>(icc.data()), static_cast<uLong>(icc.size()), 9);
    packed.resize(size);
    std::string body = std::string("iCCP") + "ICC Profile" + std::string(2, '\0') + packed;
    const auto crc = crc32(0L, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()));
    const std::string chunk = be32(static_cast<std::uint32_t>(body.size() - 4)) + body + be32(static_cast<std::uint32_t>(crc));
    return png.substr(0, 33) + chunk + png.substr(33);  // (the signature and IHDR, then the profile)
}

// A picture of one colour (opaque), 8 × 8.
std::string picture(const std::array<int, 3>& rgb) {
    return render::write_png(render::Image::create("RGBA", render::Size{8, 8}, render::Ink{rgb[0], rgb[1], rgb[2], 255}));
}

// The colour taken from a profile to sRGB as render::proof::convert_picture takes a picture (relative, black point
// compensation), 0..255.
std::array<int, 3> into_srgb(const std::array<int, 3>& rgb, const std::string& icc) {
    const render::Image one = render::Image::create("RGB", render::Size{1, 1}, render::Ink{rgb[0], rgb[1], rgb[2]});
    const std::string b = render::proof::convert_picture(one, icc, "relative", true).tobytes();
    return {static_cast<unsigned char>(b[0]), static_cast<unsigned char>(b[1]), static_cast<unsigned char>(b[2])};
}

}  // namespace

class TestContractProfile : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const Json& ops, int* exit = nullptr) {
        static int n = 0;
        const QString file = path(QStringLiteral("ops-%1.json").arg(++n));
        genko::test::write_bytes(file, ops.dump());
        return genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, exit);
    }
    // The layer's own pixel at a point of the page, as inspect --pixel --layer gives it.
    Json pixel(const QString& book, int page, const QString& layer) {
        int exit = -1;
        const Json at = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("%1:20,30").arg(page), QStringLiteral("--layer"), layer,
                               QStringLiteral("--dpi"), QStringLiteral("40")},
                              &exit);
        if (exit != 0) qWarning("%s", at.dump().c_str());
        return at;
    }
    std::array<int, 3> levels(const Json& at) {
        std::array<int, 3> out{};
        for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<int>(std::lround(at["values"][c].get<double>() * 255));
        return out;
    }
    static bool near(const std::array<int, 3>& a, const std::array<int, 3>& b, int most) {
        for (unsigned c = 0; c < 3; ++c)
            if (std::abs(a[c] - b[c]) > most) return false;
        return true;
    }

private slots:
    void picturesComeInAsTheOpSays() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("3")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const std::string p3 = *genko::core::builtin_profile("display-p3")->bytes;
        const std::string adobe = *genko::core::builtin_profile("adobe-rgb-compat")->bytes;
        const std::array<int, 3> colour{100, 150, 200};
        const std::string tagged = genko::core::b64encode(with_icc(picture(colour), p3));
        const std::string plain = genko::core::b64encode(picture(colour));
        Json ops = Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"profile", "display-p3"}},
                                Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}});
        for (int page = 1; page <= 3; ++page) {
            for (const char* way : {"keep", "convert", "assign", "none"}) {
                const std::string id = std::string(way) + std::to_string(page);
                ops.push_back(Json{{"op", "add_layer"}, {"page", page}, {"kind", "paint"}, {"id", id}});
                Json put{{"op", "put_raster"}, {"page", page}, {"id", id}, {"png_base64", std::string(way) == "assign" ? plain : tagged}};
                if (std::string(way) != "none") put["embedded_profile"] = way;
                if (std::string(way) == "assign") put["profile"] = "adobe-rgb-compat";
                ops.push_back(put);
            }
        }
        const Json done = apply(book, ops, &exit);
        QVERIFY2(exit == 0 && done.value("ok", false), done.dump().c_str());
        // the op's report: the way, and the profile it took the colours from
        bool reported = false;
        for (const Json& r : done.value("results", Json::array())) {
            if (!r.is_object() || !r.contains("embedded_profile")) continue;
            const Json& e = r["embedded_profile"];
            if (e["way"] == "convert") {
                QVERIFY2(e.value("profile", std::string()).find("P3") != std::string::npos, e.dump().c_str());
                reported = true;
            }
        }
        QVERIFY2(reported, done.dump().c_str());
        // an sRGB page: kept (and with none said: Python's) the numbers; converted, P3's colour as sRGB; assigned, the
        // numbers as Adobe RGB's
        QCOMPARE(levels(pixel(book, 1, QStringLiteral("keep1"))), colour);
        QCOMPARE(levels(pixel(book, 1, QStringLiteral("none1"))), colour);
        const auto converted = levels(pixel(book, 1, QStringLiteral("convert1")));
        QVERIFY2(near(converted, into_srgb(colour, p3), 1), qPrintable(QStringLiteral("%1 %2 %3").arg(converted[0]).arg(converted[1]).arg(converted[2])));
        QVERIFY(!near(converted, colour, 3));
        QVERIFY(near(levels(pixel(book, 1, QStringLiteral("assign1"))), into_srgb(colour, adobe), 1));
        // a Display P3 page: P3's own numbers stay (the same space), kept too; Adobe RGB's into P3 differ
        QVERIFY(near(levels(pixel(book, 2, QStringLiteral("convert2"))), colour, 1));
        QCOMPARE(levels(pixel(book, 2, QStringLiteral("keep2"))), colour);
        QVERIFY(!near(levels(pixel(book, 2, QStringLiteral("assign2"))), colour, 3));
        // a CMYK page: its inks from the colour the profile means, not from the numbers as sRGB
        const Json kept = pixel(book, 3, QStringLiteral("keep3")), inks = pixel(book, 3, QStringLiteral("convert3"));
        QCOMPARE(kept["mode"], Json("cmyk"));
        QVERIFY2(kept["values"] != inks["values"], inks.dump().c_str());
        QCOMPARE(kept["values"], pixel(book, 3, QStringLiteral("none3"))["values"]);
        // refused, nothing changed: a way that is none, an assigned profile of another space, assign with no profile
        const std::string before = genko::test::read_bytes(book + QStringLiteral("/project.json"));
        for (const Json& bad : {Json{{"op", "put_raster"}, {"page", 1}, {"id", "keep1"}, {"png_base64", plain}, {"embedded_profile", "discard"}},
                                Json{{"op", "put_raster"}, {"page", 1}, {"id", "keep1"}, {"png_base64", plain}, {"embedded_profile", "assign"}, {"profile", "coated-cmyk"}},
                                Json{{"op", "put_raster"}, {"page", 1}, {"id", "keep1"}, {"png_base64", plain}, {"embedded_profile", "assign"}}}) {
            const Json refused = apply(book, Json::array({bad}), &exit);
            QVERIFY2(exit != 0 && !refused.value("ok", true), refused.dump().c_str());
            QCOMPARE(genko::test::read_bytes(book + QStringLiteral("/project.json")), before);
        }
        // import_psd: a PSD that carries Display P3 (page 2 written out), taken into the sRGB page — kept, its numbers; converted, its colours
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const std::string file = genko::test::one_line(psd.out)["files"][1].get<std::string>();  // (page 2's)
        const std::string psd_bytes = genko::core::b64encode(genko::test::read_bytes(QString::fromStdString(file)));
        const Json imported = apply(book, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", psd_bytes}, {"id", "kept"}},
                                                       Json{{"op", "import_psd"}, {"page", 1}, {"psd", psd_bytes}, {"id", "conv"}, {"embedded_profile", "convert"}}}),
                                    &exit);
        QVERIFY2(exit == 0 && imported.value("ok", false), imported.dump().c_str());
        QCOMPARE(imported["results"][1]["embedded_profile"]["way"], Json("convert"));
        // (each import's layers: the page's paper, then the layer of the colour put on page 2 — its middle)
        const Json first = pixel(book, 1, QStringLiteral("kept-2")), second = pixel(book, 1, QStringLiteral("conv-2"));
        QVERIFY2(first["alpha"].get<double>() > 0.99 && second["alpha"].get<double>() > 0.99, (first.dump() + second.dump()).c_str());
        QVERIFY2(near(levels(second), into_srgb(levels(first), p3), 1), (first.dump() + second.dump()).c_str());
        QVERIFY(!near(levels(second), levels(first), 3));
    }

    void profileInfo() {
        const QString book = path(QStringLiteral("info.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const Json moded = apply(book, Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}}), &exit);
        QVERIFY2(exit == 0, moded.dump().c_str());
        const Json p3 = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QStringLiteral("display-p3")}, &exit);
        QVERIFY2(exit == 0, p3.dump().c_str());
        QCOMPARE(p3["space"], Json("rgb"));
        QCOMPARE(p3["id"], Json("display-p3"));
        QVERIFY2(p3["description"].get<std::string>().find("P3") != std::string::npos, p3.dump().c_str());
        QVERIFY(p3["version"].is_string() && p3["white"].is_array() && p3["white"].size() == 3 && p3["trc"].is_string());
        QCOMPARE(p3["bytes"], Json(genko::core::builtin_profile("display-p3")->bytes->size()));
        QCOMPARE(p3["builtins"].size(), genko::core::builtin_profiles().size());
        QCOMPARE(p3["builtins"][0]["id"], Json("srgb"));
        // a page's: page 1 has none (sRGB, Little CMS's own), page 2 its CMYK
        const Json one = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QStringLiteral("1")}, &exit);
        QVERIFY2(exit == 0, one.dump().c_str());
        QCOMPARE(one["page"], Json(1));
        QCOMPARE(one["space"], Json("rgb"));
        QCOMPARE(one["bytes"], Json(0));
        const Json two = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QStringLiteral("2")}, &exit);
        QVERIFY2(exit == 0, two.dump().c_str());
        QCOMPARE(two["space"], Json("cmyk"));
        QCOMPARE(two["class"], Json("output"));
        QVERIFY(two["lut"].get<bool>());
        // by its sha256: the same
        const std::string ref = genko::core::builtin_profile("coated-cmyk")->ref();
        const Json by_ref = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QString::fromStdString(ref)}, &exit);
        QVERIFY2(exit == 0, by_ref.dump().c_str());
        QCOMPARE(by_ref["description"], two["description"]);
        // none: refused
        const Json none = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QStringLiteral("nope")}, &exit);
        QVERIFY2(exit != 0, none.dump().c_str());
        const Json far = genko({QStringLiteral("inspect"), book, QStringLiteral("--profile"), QStringLiteral("9")}, &exit);
        QVERIFY2(exit != 0, far.dump().c_str());
    }
};

QTEST_GUILESS_MAIN(TestContractProfile)
#include "test_contract_profile.moc"
