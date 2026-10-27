#include <QtTest>

#include "api/buildinfo.hpp"
#include "core/version.hpp"

class TestBuildInfo : public QObject {
    Q_OBJECT
private slots:
    void reportsEveryLibrary() {
        const auto info = genko::api::build_info();
        for (const char* key : {"genko", "format", "qt", "zlib", "libpng", "libjpeg", "libjpeg_turbo", "libtiff", "freetype", "harfbuzz",
                                "webp", "lcms2", "openexr", "libavif", "aom", "zstd", "blake3", "libraw", "nlohmann_json"}) {
            QVERIFY2(info.contains(key), key);
        }
        QCOMPARE(info["format"].get<int>(), genko::kWriteFormatVersion);
        QVERIFY(info["freetype"].get<std::string>() != "unavailable");
        QVERIFY(QString::fromStdString(info["qt"].get<std::string>()).startsWith("6.11"));
        // (libjpeg-turbo's own version, for the SBOM: "libjpeg" is the API's, 80 for 8.0)
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$")).match(QString::fromStdString(info["libjpeg_turbo"].get<std::string>())).hasMatch());
    }
};

QTEST_GUILESS_MAIN(TestBuildInfo)
#include "test_buildinfo.moc"
