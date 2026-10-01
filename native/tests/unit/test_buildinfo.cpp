#include <QtTest>

#include "api/buildinfo.hpp"
#include "core/version.hpp"

class TestBuildInfo : public QObject {
    Q_OBJECT
private slots:
    void reportsEveryLibrary() {
        const auto info = genko::api::build_info();
        for (const char* key : {"genko", "format", "qt", "zlib", "libpng", "libjpeg", "libtiff", "freetype", "harfbuzz",
                                "webp", "lcms2", "nlohmann_json"}) {
            QVERIFY2(info.contains(key), key);
        }
        QCOMPARE(info["format"].get<int>(), genko::kWriteFormatVersion);
        QVERIFY(info["freetype"].get<std::string>() != "unavailable");
        QVERIFY(QString::fromStdString(info["qt"].get<std::string>()).startsWith("6.11"));
    }
};

QTEST_GUILESS_MAIN(TestBuildInfo)
#include "test_buildinfo.moc"
