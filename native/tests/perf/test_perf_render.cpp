// How long drawing takes (shown, not judged — but for the small picture's bound of tests/test_h1.py): a page of the F1 book (docs/cpp-migration/FIXTURES.md: B4, 1500 ink
// lines of 20 points, pressure 0.7, made from random.Random(1) as tests/test_h1.py::_thick makes them) at 350 dpi,
// the whole page from nothing; the same page again with one more line (the remembered lines are drawn on); and a
// 512 × 512 part of the page (a screen tile) from nothing. And test_h1.test_small_pictures_draw_lines_plainly: a page
// of 200 lines drawn at 12 dpi for the proof in under 0.5 s, its lines there.
// And (PC5 I-b review P2-4) a B4 RGB PSD of groups at 350 dpi (GENKO_PERF_GROUPS_PSD, made by perf/make_groups_psd.py)
// read by import_psd onto a B4 colour page: the whole page and a 512 x 512 part, from nothing. Where this build draws
// groups (groups@1), also the same page with its groups made plain folders again, as builds before groups read them.

#include <QtTest>

#include <QElapsedTimer>
#include <QFile>

#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/color_model.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/import_psd.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/smart.hpp"

#if __has_include("core/groups.hpp")
#include "core/groups.hpp"
#define GENKO_PERF_GROUPS 1
#endif

namespace render = genko::render;
using genko::core::Document;

namespace {

// tests/test_h1.py::_thick, page 1
Document f1_page(int lines = 1500) {
    genko::core::PyRandom rng(1);
    Document doc = genko::core::new_episode("厚い本", genko::core::Num(1), 1, genko::core::PageSpec::b4_comic());
    genko::core::Page& page = doc.edit_page(0);
    page.numero = false;  // (nombres are drawn in M4)
    for (auto& layer : page.layers) {
        if (layer.role != genko::core::LayerRole::Ink) continue;
        std::vector<genko::core::StrokePtr> items;
        for (int n = 0; n < lines; ++n) {
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

// The most memory this process has held (kB: VmHWM), and what it holds now (VmRSS).
long proc_kb(const char* key) {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith(key)) return line.mid(static_cast<int>(std::strlen(key))).trimmed().split(' ').first().toLong();
    }
    return -1;
}

// A B4 colour page of 16 bits showing a 16-bit grey file of w × h pixels laid over it as a smart object.
Document deep_file_page(int w, int h) {
    genko::core::PageSpec spec = genko::core::PageSpec::b4_comic();
    spec.expression = "color";
    Document doc = genko::core::new_episode("深い", genko::core::Num(1), 1, spec);
    genko::core::Page& page = doc.edit_page(0);
    page.numero = false;
    genko::core::RgbMode rgb;
    rgb.depth = 16;
    page.rgb = rgb;
    std::string png = render::write_png(render::Image::create("I;16", render::Size{w, h}, render::Ink(1000)));
    auto contents = std::make_shared<genko::core::SmartContents>();
    contents->kind = "file";
    contents->name = "scan.png";
    contents->asset = genko::core::bytes_ref(png);
    contents->suffix = ".png";
    contents->format = "png";
    contents->width_px = w;
    contents->height_px = h;
    contents->dpi = w / (page.spec.width_mm.value() / 25.4);
    contents->depth = 16;
    contents->mode = "gray";
    doc.smart_files[contents->asset] = std::make_shared<const std::string>(std::move(png));
    doc.smart_contents["sc_000000000001"] = contents;
    genko::core::Layer layer;
    layer.id = "so";
    layer.kind = genko::core::LayerKind::Smart;
    layer.clip_to = "none";
    layer.smart = genko::core::smart::settings("sc_000000000001", genko::core::smart::Matrix{}, "lanczos");
    page.layers.push_back(std::move(layer));
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

    // (PC5 I-b review P2-4: shown, not judged) the PSD of groups: a page's timings before and after groups@1
    void psd_of_groups_at_350dpi() {
        const QString path = qEnvironmentVariable("GENKO_PERF_GROUPS_PSD");
        if (path.isEmpty()) QSKIP("GENKO_PERF_GROUPS_PSD: a PSD of groups (python tests/perf/make_groups_psd.py OUT.psd)");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        genko::core::PageSpec spec = genko::core::PageSpec::b4_comic();
        spec.expression = "color";
        Document doc = genko::core::new_episode("グループ", genko::core::Num(1), 1, spec);
        doc.edit_page(0).numero = false;
        QElapsedTimer timer;
        timer.start();
        (void)render::import_psd_data(doc, genko::core::Json{{"op", "import_psd"}, {"page", 1}, {"fit", "paper"}},
                                      std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        const qint64 read = timer.elapsed();
        std::size_t groups = 0;
#ifdef GENKO_PERF_GROUPS
        for (const auto& layer : doc.page(0).layers)
            if (genko::core::is_group(layer)) ++groups;
#endif
        const auto times = [](const Document& d, const char* what) {
            render::RenderOptions options;
            render::clear_render_caches();
            QElapsedTimer clock;
            clock.start();
            const render::RenderResult whole = render::render_page(d.page(0), 350, options, &d);
            const qint64 cold = clock.elapsed();
            render::clear_render_caches();
            render::RenderOptions tile = options;
            tile.region = render::RenderRegion{1200, 1800, 512, 512};
            clock.restart();
            (void)render::render_page(d.page(0), 350, tile, &d);
            const qint64 part = clock.elapsed();
            qInfo("%s at 350 dpi (%d x %d px): whole page %lld ms; a 512x512 part from nothing %lld ms", what, whole.image.width(),
                  whole.image.height(), static_cast<long long>(cold), static_cast<long long>(part));
        };
        qInfo("the PSD of groups read in %lld ms: %zu layers, %zu of them drawn as groups", static_cast<long long>(read),
              doc.page(0).layers.size(), groups);
        times(doc, "the page of the PSD");
#ifdef GENKO_PERF_GROUPS
        Document plain = doc;
        for (auto& layer : plain.edit_page(0).layers) {
            if (!layer.group) continue;
            layer.group = false;
            layer.blend = "normal";
        }
        times(plain, "the same page, its groups plain folders");
#endif
    }

    // test_h1.test_small_pictures_draw_lines_plainly (_thick(1, 200)): judged, as Python's
    // (independent review A P1-1) A 16-bit grey file of 20 million pixels on a 16-bit B4 page drawn as the screen draws it
    // at 100 dpi, in tiles of 512 pixels: one after another, then four at once. Shown: the time, the files decoded and the
    // memory the process held at most (run alone, so that its high-water mark is this work's).
    void smart_deep_file_tiles() {
        const long before = proc_kb("VmHWM:");
        const Document doc = deep_file_page(4000, 5000);
        render::RenderOptions options;
        options.mode = "proof";
        options.precise = true;
        const int dpi = 100;
        const int w = render::mm_to_px(doc.page(0).spec.width_mm.value(), dpi), h = render::mm_to_px(doc.page(0).spec.height_mm.value(), dpi);
        std::vector<render::RenderRegion> tiles;
        for (int y = 0; y < h; y += 512)
            for (int x = 0; x < w; x += 512) tiles.push_back(render::RenderRegion{x, y, std::min(512, w - x), std::min(512, h - y)});
        render::clear_render_caches();
        auto decoded = render::smart::counts().files_decoded;
        QElapsedTimer clock;
        clock.start();
        for (const render::RenderRegion& tile : tiles) {
            options.region = tile;
            QVERIFY(render::render_page(doc.page(0), dpi, options, &doc).precise.has_value());
        }
        const qint64 one_by_one = clock.elapsed();
        const auto first = render::smart::counts().files_decoded - decoded;
        render::clear_render_caches();
        decoded = render::smart::counts().files_decoded;
        clock.restart();
        std::vector<std::thread> threads;
        for (std::size_t k = 0; k < 4; ++k) {
            threads.emplace_back([&doc, &tiles, k, dpi] {
                render::RenderOptions o;
                o.mode = "proof";
                o.precise = true;
                for (std::size_t i = k; i < tiles.size(); i += 4) {
                    o.region = tiles[i];
                    (void)render::render_page(doc.page(0), dpi, o, &doc);
                }
            });
        }
        for (std::thread& t : threads) t.join();
        const qint64 together = clock.elapsed();
        const auto second = render::smart::counts().files_decoded - decoded;
        qInfo("smart_deep_file_tiles: %zu tiles: one by one %lld ms (%llu decoded), four at once %lld ms (%llu decoded); the process held at most %ld MB "
              "(%ld MB before)",
              tiles.size(), static_cast<long long>(one_by_one), static_cast<unsigned long long>(first), static_cast<long long>(together),
              static_cast<unsigned long long>(second), proc_kb("VmHWM:") / 1024, before / 1024);
    }

    void small_picture_of_200_lines() {
        Document doc = f1_page(200);
        render::RenderOptions proof;
        proof.mode = "proof";
        render::clear_render_caches();
        QElapsedTimer timer;
        timer.start();
        const render::RenderResult small = render::render_page(*doc.pages[0], 12, proof, &doc);
        const qint64 took = timer.elapsed();
        qInfo("200 lines at 12 dpi (proof): %lld ms", took);
        QVERIFY2(took < 500, qPrintable(QString::number(took)));
        QVERIFY(small.image.convert("L").getextrema()[0].first < 128);  // (the lines are there)
    }
};

QTEST_GUILESS_MAIN(TestPerfRender)
#include "test_perf_render.moc"
