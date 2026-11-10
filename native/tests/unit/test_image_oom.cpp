#include <QtTest>
#include "core/error.hpp"
#include "render/image.hpp"
#include <array>
extern "C" void genko_test_fail_scale_table(int);
extern "C" int genko_test_scale_failures(void);
using namespace genko::render;
class TestImageOom : public QObject {
    Q_OBJECT
private slots:
    void transformTableFailure() {
        ImageAllocationBudget budget(4096);
        const Image src = Image::create("L", {8, 8}, Ink(255));
        const std::array<double, 6> matrix{0.5, 0, 0, 0, 0.5, 0};
        genko_test_fail_scale_table(1);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, src.transform({16, 16}, TransformMethod::Affine, matrix, Resample::Nearest));
        QCOMPARE(genko_test_scale_failures(), 1);
        QCOMPARE(budget.live(), std::uint64_t(64));
        QCOMPARE(src.tobytes(), std::string(64, '\xff'));
        genko_test_fail_scale_table(0);
        QCOMPARE(src.transform({16, 16}, TransformMethod::Affine, matrix, Resample::Nearest).tobytes(), std::string(256, '\xff'));
        QCOMPARE(budget.live(), std::uint64_t(64));
    }
    void resizeTableFailure() {
        ImageAllocationBudget budget(4096);
        const Image src = Image::create("L", {8, 8}, Ink(255));
        genko_test_fail_scale_table(1);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, src.resize({16, 16}, Resample::Nearest));
        QCOMPARE(genko_test_scale_failures(), 1);
        QCOMPARE(budget.live(), std::uint64_t(64));
        QCOMPARE(src.tobytes(), std::string(64, '\xff'));
        genko_test_fail_scale_table(0);
        QCOMPARE(src.resize({16, 16}, Resample::Nearest).tobytes(), std::string(256, '\xff'));
        QCOMPARE(budget.live(), std::uint64_t(64));
    }
};
QTEST_GUILESS_MAIN(TestImageOom)
#include "test_image_oom.moc"
