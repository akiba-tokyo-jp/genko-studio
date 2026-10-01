// How long drawing takes (shown, not judged): a page of the F1 book (docs/cpp-migration/FIXTURES.md: B4, 1500 ink
// lines of 20 points, pressure 0.7, made from random.Random(1) as tests/test_h1.py::_thick makes them) at 350 dpi,
// the whole page from nothing; the same page again with one more line (the remembered lines are drawn on); and a
// 512 × 512 part of the page (a screen tile) from nothing.

#include <QtTest>

#include <QElapsedTimer>

#include <memory>
#include <vector>

#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "render/page.hpp"

namespace render = genko::render;
using genko::core::Document;

namespace {

// tests/test_h1.py::_thick, page 1
Document f1_page() {
    genko::core::PyRandom rng(1);
    Document doc = genko::core::new_episode("厚い本", genko::core::Num(1), 1, genko::core::PageSpec::b4_comic());
    genko::core::Page& page = doc.edit_page(0);
    page.numero = false;  // (nombres are drawn in M4)
    for (auto& layer : page.layers) {
        if (layer.role != genko::core::LayerRole::Ink) continue;
        std::vector<genko::core::StrokePtr> items;
        for (int n = 0; n < 1500; ++n) {
            const double x = genko::core::py_round(rng.uniform(20, 230), 3);
            const double y = genko::core::py_round(rng.uniform(20, 340), 3);
            auto stroke = std::make_shared<genko::core::Stroke>();
            stroke->id = genko::core::new_id();
            for (int i = 0; i < 20; ++i) {
                stroke->points.push_back({genko::core::py_round(x + i * 1.2, 3), genko::core::py_round(y + rng.uniform(-2, 2), 3)});
            }
            stroke->pressure.assign(20, 0.7);
            items.push_back(stroke);
        }
        layer.strokes = genko::core::make_strokes(std::move(items));
    }
    return doc;
}

}  // namespace

class TestPerfRender : public QObject {
    Q_OBJECT

private slots:
    void f1_page_at_350dpi() {
        Document doc = f1_page();
        // the same lines as Python's _thick makes for its first page (its first and last line's ends, exactly)
        const auto at = [](const genko::core::PointF& p, double x, double y) { return p.x == x && p.y == y; };
        for (const auto& layer : doc.pages[0]->layers) {
            if (layer.role != genko::core::LayerRole::Ink) continue;
            const auto& items = layer.strokes->items;
            QCOMPARE(items.size(), std::size_t{1500});
            QVERIFY(at(items.front()->points.front(), 48.216, 292.234) && at(items.front()->points.back(), 71.016, 291.345));
            QVERIFY(at(items.back()->points.front(), 40.759, 253.114) && at(items.back()->points.back(), 63.559, 253.42));
        }
        render::RenderOptions options;
        render::clear_render_caches();
        QElapsedTimer timer;
        timer.start();
        const render::RenderResult whole = render::render_page(*doc.pages[0], 350, options, &doc);
        const qint64 cold = timer.elapsed();
        QVERIFY(whole.omitted.empty());

        // one more line: drawn on the remembered picture of the others
        genko::core::Page& page = doc.edit_page(0);
        for (auto& layer : page.layers) {
            if (layer.role != genko::core::LayerRole::Ink) continue;
            auto stroke = std::make_shared<genko::core::Stroke>();
            stroke->id = genko::core::new_id();
            stroke->points = {{40.0, 60.0}, {90.0, 80.0}, {120.0, 70.5}};
            stroke->pressure = {0.4, 0.9, 0.6};
            std::vector<genko::core::StrokePtr> items = layer.strokes->items;
            items.push_back(stroke);
            layer.strokes = genko::core::make_strokes(std::move(items));
        }
        timer.restart();
        (void)render::render_page(*doc.pages[0], 350, options, &doc);
        const qint64 warm = timer.elapsed();

        render::clear_render_caches();
        render::RenderOptions tile = options;
        tile.region = render::RenderRegion{1200, 1800, 512, 512};
        timer.restart();
        (void)render::render_page(*doc.pages[0], 350, tile, &doc);
        const qint64 part = timer.elapsed();

        qInfo("F1 page (B4, 1500 lines) at 350 dpi (%d x %d px): whole page %lld ms; with one more line (remembered "
              "lines) %lld ms; a 512x512 part from nothing %lld ms",
              whole.image.width(), whole.image.height(), cold, warm, part);
    }
};

QTEST_GUILESS_MAIN(TestPerfRender)
#include "test_perf_render.moc"
