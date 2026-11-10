// render/colour: a CMYK profile read through a path of any name (lcms2's own fopen takes a narrow name), and read again
// when the file changes.
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "testsupport.hpp"
using namespace genko;

namespace {
const std::filesystem::path kIcc = std::filesystem::path(GENKO_TEST_DATA) / "colour" / "photocraft-coated-cmyk.icc";
}  // namespace

class TestContractColour : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    render::Image picture_;

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        // (an RGBA picture of every grey, saturated and dark colours, half-clear pixels)
        const int w = 64, h = 40;
        std::string bytes;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                bytes.append({static_cast<char>((x * 4) % 256), static_cast<char>((y * 6 + x) % 256), static_cast<char>((x * y) % 256),
                              static_cast<char>(y < 30 ? 255 : 100 + x)});
        picture_ = render::Image::frombytes("RGBA", render::Size{w, h}, bytes);
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
