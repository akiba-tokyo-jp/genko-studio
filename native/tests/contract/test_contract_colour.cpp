// render/colour (CMYK through an ICC profile or the plain conversion, the screen proof, the sRGB profile) against
// Python's genko/colour.py (render_harness.py colour-cases), on a fixed picture: the plain conversion byte for byte;
// through a profile within a level (Little CMS here 2.14, Pillow's own build another version).
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "rendertest.hpp"
using namespace genko;
using core::Json;

namespace {
const std::filesystem::path kIcc = std::filesystem::path(GENKO_TEST_DATA) / "colour" / "photocraft-coated-cmyk.icc";

Json shot(const std::function<render::Image()>& make) {
    try {
        const render::Image image = make();
        return Json{{"mode", image.mode()}, {"size", Json::array({image.width(), image.height()})}, {"bytes", core::b64encode(image.tobytes())}};
    } catch (const core::PyValueError& e) {
        return Json{{"error", Json::array({"ValueError", e.what()})}};
    }
}
}  // namespace

class TestContractColour : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    Json cases_;
    render::Image picture_;

    // the same mode and size; every band within `levels` of Python's
    void compare(const char* name, const Json& got, int levels) {
        const Json& want = cases_[name];
        if (want.contains("error") || got.contains("error")) {
            QCOMPARE(QString::fromStdString(got.dump()), QString::fromStdString(want.dump()));
            return;
        }
        QCOMPARE(QString::fromStdString(got["mode"].get<std::string>()), QString::fromStdString(want["mode"].get<std::string>()));
        QCOMPARE(got["size"], want["size"]);
        const std::string a = core::a2b_base64(got["bytes"].get<std::string>()), b = core::a2b_base64(want["bytes"].get<std::string>());
        QCOMPARE(a.size(), b.size());
        int worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i]))));
        QVERIFY2(worst <= levels, qPrintable(QString("%1: off by %2 levels").arg(name).arg(worst)));
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + "/colour.json";
        const auto py = genko::test::render_harness({"colour-cases", out, QString::fromStdString(kIcc.string())}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        cases_ = genko::test::read_json(out);
        picture_ = render::Image::frombytes("RGBA", render::Size{cases_["size"][0].get<int>(), cases_["size"][1].get<int>()},
                                            core::a2b_base64(cases_["picture"].get<std::string>()));
    }

    // The plain conversion (no profile): numpy's float32 steps, byte for byte.
    void plainConversion() {
        compare("to_cmyk", shot([&] { return render::colour::to_cmyk(picture_); }), 0);
        compare("to_cmyk_200", shot([&] { return render::colour::to_cmyk(picture_, std::nullopt, 200); }), 0);
        compare("from_cmyk", shot([&] { return render::colour::from_cmyk(render::colour::to_cmyk(picture_)); }), 0);
        compare("proof", shot([&] { return render::colour::proof(picture_); }), 0);
        QCOMPARE(render::colour::ink_coverage(render::colour::to_cmyk(picture_)), cases_["ink"].get<double>());
    }

    void throughTheProfile_data() {
        QTest::addColumn<QString>("intent");
        for (const char* intent : {"perceptual", "relative", "saturation", "absolute"}) QTest::newRow(intent) << QString(intent);
    }
    void throughTheProfile() {
        QFETCH(QString, intent);
        const std::string name = intent.toStdString();
        compare(("to_cmyk_icc_" + name).c_str(), shot([&] { return render::colour::to_cmyk(picture_, kIcc, render::colour::kInkLimit, name); }), 1);
        compare(("from_cmyk_icc_" + name).c_str(),
                shot([&] { return render::colour::from_cmyk(render::colour::to_cmyk(picture_, kIcc), kIcc, name); }), 2);
    }

    void proofAndProfiles() {
        compare("proof_icc", shot([&] { return render::colour::proof(picture_, kIcc); }), 2);
        QVERIFY(std::abs(render::colour::ink_coverage(render::colour::to_cmyk(picture_, kIcc)) - cases_["ink_icc"].get<double>()) <= 400.0 / 255);
        QCOMPARE(render::colour::is_cmyk_profile(kIcc), cases_["is_cmyk"].get<bool>());
        QCOMPARE(QString::fromStdString(render::colour::profile_name(kIcc)), QString::fromStdString(cases_["name"].get<std::string>()));
        std::string srgb = render::colour::srgb_icc();
        QVERIFY(srgb.size() > 36);
        std::fill(srgb.begin() + 24, srgb.begin() + 36, '\0');  // (the date and time it was made)
        QCOMPARE(core::b64encode(srgb), cases_["srgb_icc"].get<std::string>());
        const auto srgb_file = std::filesystem::path(scratch_.path().toStdString()) / "srgb.icc";
        {
            std::FILE* f = std::fopen(srgb_file.string().c_str(), "wb");
            const std::string bytes = render::colour::srgb_icc();
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
        QCOMPARE(render::colour::is_cmyk_profile(srgb_file), cases_["srgb_is_cmyk"].get<bool>());
        compare("to_cmyk_srgb_profile", shot([&] { return render::colour::to_cmyk(picture_, srgb_file); }), 0);
        compare("to_cmyk_bad_intent", shot([&] { return render::colour::to_cmyk(picture_, kIcc, render::colour::kInkLimit, "vivid"); }), 0);
        compare("missing_profile", shot([&] { return render::colour::to_cmyk(picture_, srgb_file.parent_path() / "none.icc"); }), 0);
    }

    // A profile at a path of any name is read through the path itself (lcms2's own fopen takes a narrow name); a file
    // that changes is read again (the transforms made from it are kept only while it is as it was).
    void anyPathAndChanges() {
        const auto folder = std::filesystem::path(scratch_.path().toStdString()) / core::path_from_utf8("色校正 プロファイル");
        std::filesystem::create_directories(folder);
        const auto named = folder / core::path_from_utf8("ジャパンカラー.icc");
        std::filesystem::copy_file(kIcc, named, std::filesystem::copy_options::overwrite_existing);
        QVERIFY(render::colour::is_cmyk_profile(named));
        QCOMPARE(QString::fromStdString(render::colour::profile_name(named)), QString::fromStdString(render::colour::profile_name(kIcc)));
        const std::string expected = render::colour::to_cmyk(picture_, kIcc).tobytes();
        QCOMPARE(render::colour::to_cmyk(picture_, named).tobytes(), expected);
        QCOMPARE(render::colour::to_cmyk(picture_, named).tobytes(), expected);  // (the transform kept)
        QCOMPARE(render::colour::proof(picture_, named).tobytes(), render::colour::proof(picture_, kIcc).tobytes());
        // the file replaced by an RGB profile: no longer a CMYK one
        {
            std::ofstream file(named, std::ios::binary | std::ios::trunc);
            const std::string bytes = render::colour::srgb_icc();
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            QVERIFY(file.good());
        }
        QVERIFY(!render::colour::is_cmyk_profile(named));
        QVERIFY_EXCEPTION_THROWN(render::colour::to_cmyk(picture_, named), core::PyValueError);
        std::filesystem::remove(named);
        QVERIFY_EXCEPTION_THROWN(render::colour::to_cmyk(picture_, named), core::PyValueError);
    }
};

QTEST_GUILESS_MAIN(TestContractColour)
#include "test_contract_colour.moc"
