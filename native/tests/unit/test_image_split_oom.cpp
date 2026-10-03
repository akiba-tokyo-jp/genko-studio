#include <QtTest>
#include "render/image.hpp"
#include <array>
#include <cstdlib>
#include <new>

using namespace genko::render;
namespace {
struct Injection {
    bool requested = false;
    bool armed = false;
    bool metadata = false;
    int remaining = 0;
    int failures = 0;
    int splits = 0;
    ImageAllocationBudget* budget = nullptr;
    std::uint64_t live_after_split = 0;
};
thread_local Injection injection;
struct Disarm {
    ~Disarm() { injection.requested = false; injection.armed = false; injection.budget = nullptr; }
};
}
extern "C" void genko_test_after_split() {
    if (!injection.requested) return;
    ++injection.splits;
    injection.live_after_split = injection.budget->live();
    injection.armed = true;
}
// Test-executable-only replacement. C pixel allocations are never intercepted.
void* operator new(std::size_t size) {
    const bool vector = size >= sizeof(Image) && size <= 4 * sizeof(Image) && size % sizeof(Image) == 0;
    if (injection.armed && vector != injection.metadata && --injection.remaining == 0) {
        injection.armed = false;
        ++injection.failures;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

class TestImageSplitOom : public QObject {
    Q_OBJECT
private slots:
    void ownershipFailure_data() {
        QTest::addColumn<bool>("metadata");
        QTest::addColumn<int>("allocation");
        QTest::newRow("vector") << false << 1;
        QTest::newRow("metadata-first") << true << 1;
        QTest::newRow("metadata-second") << true << 2;
        QTest::newRow("metadata-third") << true << 3;
        QTest::newRow("metadata-fourth") << true << 4;
    }
    void ownershipFailure() {
        QFETCH(bool, metadata);
        QFETCH(int, allocation);
        ImageAllocationBudget budget(4096);
        {
            const std::array<int, 4> values{11, 23, 35, 47};
            Image src = Image::create("RGBA", {8, 8}, Ink{11, 23, 35, 47});
            if (metadata) {
                Transparency info;
                info.kind = Transparency::Kind::Bytes;
                info.bytes = std::string(97, '\x7f');
                src.set_transparency(std::move(info));
            }
            const std::string before = src.tobytes();
            const auto live = budget.live();
            injection = {};
            injection.requested = true;
            injection.metadata = metadata;
            injection.remaining = allocation;
            injection.budget = &budget;
            bool threw = false;
            {
                Disarm disarm;
                try { const auto result = src.split(); Q_UNUSED(result); }
                catch (const std::bad_alloc&) { threw = true; }
            }
            QVERIFY(threw);
            QCOMPARE(injection.failures, 1);
            QCOMPARE(injection.splits, 1);
            QVERIFY(injection.live_after_split > live);
            QCOMPARE(budget.live(), live);
            QCOMPARE(src.tobytes(), before);
            {
                const auto result = src.split();
                QCOMPARE(result.size(), std::size_t(4));
                QCOMPARE(budget.live(), injection.live_after_split);
                for (std::size_t i = 0; i < result.size(); ++i) {
                    QCOMPARE(result[i].mode(), std::string_view("L"));
                    QCOMPARE(result[i].size(), src.size());
                    QCOMPARE(result[i].tobytes(), std::string(std::size_t(src.width()) * std::size_t(src.height()), static_cast<char>(values[i])));
                    QVERIFY(result[i].transparency() == src.transparency());
                }
            }
            QCOMPARE(budget.live(), live);
        }
        QCOMPARE(budget.live(), std::uint64_t(0));
    }
};
QTEST_GUILESS_MAIN(TestImageSplitOom)
#include "test_image_split_oom.moc"
