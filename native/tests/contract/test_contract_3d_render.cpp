// render::render_page against Python's genko.render.render_page for pages with 3D guides on them, every pixel (RGB),
// nothing left out (no skip_unported):
//  - 10 random books (tools/migration/geom3d_harness.py make-books --render, seed fixed): figures of random builds,
//    poses and hands, heads, hands, models, boxes, cylinders, stairs, floors, spheres, cones, props, scenes and
//    mannequins, near and far, turned, some bound to panels (cut, slanted, rounded); page cameras and lights;
//  - the book of the fixed op cases with the pictures and lines render_prims and trace_prims made on it;
// in print (where 3D is never drawn), proof and name at 72, 150 and 350 dpi; panels and spreads of them too.
// Regions: five random parts of each page (proof at 150 dpi, name at 350) drawn alone are the same as the whole page
// cut. Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "rendertest.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "test3d.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

struct Book {
    genko::core::Document doc;
    std::string path;
};

const std::vector<std::string> kModes{"print", "proof", "name"};
const std::vector<int> kDpis{72, 150, 350};

}  // namespace

class TestContract3dRender : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    std::vector<Book> books_;
    Json jobs_ = Json::array();

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    const Book& book_of(const std::string& path) const {
        for (const Book& b : books_) {
            if (b.path == path) return b;
        }
        qFatal("no book %s", path.c_str());
        std::abort();  // qFatal cannot return; state that for MSVC's control-flow warning too.
    }

    static const genko::core::Page* page_of(const genko::core::Document& doc, const genko::core::Num& index) {
        for (const auto& p : doc.pages) {
            if (p->index == index) return p.get();
        }
        return nullptr;
    }

    static void use_brushes(const Book& b) {
        render::brushes::clear_custom();
        render::brushes::register_brushes(b.doc.brush_custom);
        render::clear_render_caches();
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        auto r = genko::test::geom3d_harness({"make-books", path("books"), "--seed", "20261003", "--count", "10", "--render"}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        // the fixed book with what render_prims and trace_prims draw (a picture of the surfaces and lines, pen lines)
        r = genko::test::geom3d_harness({"fixtures", path("fixtures")}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        const Json steps = Json::parse(R"([
            [{"op": "add_prim3d", "page": 2, "id": "ball", "kind": "sphere", "pos": [140, 200, 0], "size": 30},
             {"op": "add_prim3d", "page": 2, "id": "cone", "kind": "cone", "pos": [40, 210, 20], "size": [25, 40, 25], "rot": [0.2, 0.5, 0]},
             {"op": "add_prim3d", "page": 3, "id": "ground", "kind": "floor", "pos": [90, 200, 0], "size": [120, 1, 120], "focal_mm": 300},
             {"op": "add_prim3d", "page": 3, "id": "far", "kind": "sphere", "pos": [60, 90, 150], "size": [40, 20, 40]}],
            [{"op": "render_prims", "page": 1, "layer_id": "paint"}],
            [{"op": "trace_prims", "page": 1, "layer_id": "pen", "kind": "gpen", "width_mm": 0.4}],
            [{"op": "render_prims", "page": 2, "layer_id": "paint2", "lines": false, "light": [0.5, -1, -0.3], "ambient": 0.4}],
            [{"op": "trace_prims", "page": 2, "layer": "ink", "rgb": [10, 20, 200]}]])");
        const Json drawn = Json::object({{"book", path("fixtures/a.genko").toStdString()},
                                         {"steps", steps},
                                         {"agent", "human:作者"},
                                         {"out", path("fixtures/drawn.json").toStdString()},
                                         {"dest", path("books/fixture.genko").toStdString()}});
        genko::test::write_bytes(path("fixtures/jobs.json"), genko::core::dump_python(Json::array({drawn})));
        r = genko::test::geom3d_harness({"apply", path("fixtures/jobs.json")}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        const Json made = genko::test::read_json(path("fixtures/drawn.json"));  // (held: the loop below reads into it)
        for (const Json& step : made["steps"]) {
            QVERIFY2(step["reply"]["ok"] == Json(true), genko::core::dump_python(step["reply"]).c_str());
        }

        QStringList paths;
        for (int i = 0; i < 10; ++i) paths << path(QStringLiteral("books/book-%1.genko").arg(i, 2, 10, QLatin1Char('0')));
        paths << path("books/fixture.genko");
        for (const QString& p : paths) {
            Book b;
            b.path = p.toStdString();
            b.doc = genko::storage::load_document(genko::storage::path_from_utf8(b.path)).document;
            books_.push_back(std::move(b));
        }
        QDir().mkpath(path("py"));
        int n = 0;
        const auto add = [&](Json job) {
            job["out"] = path(QStringLiteral("py/%1.png").arg(n++)).toStdString();
            jobs_.push_back(std::move(job));
        };
        for (const Book& b : books_) {
            for (const auto& page : b.doc.pages) {
                for (const std::string& mode : kModes) {
                    for (const int dpi : kDpis) add(Json::object({{"book", b.path}, {"page", page->index.json()}, {"dpi", dpi}, {"mode", mode}}));
                }
            }
            // panels of the first page and a spread
            const auto& first = *b.doc.pages.front();
            int frames = 0;
            for (const genko::core::Frame* f : first.leaf_frames()) {
                if (frames++ == 2) break;
                add(Json::object({{"book", b.path}, {"page", first.index.json()}, {"kind", "frame"}, {"frame", f->id}, {"dpi", 100},
                                  {"mode", frames == 1 ? "proof" : "name"}}));
            }
            if (b.doc.pages.size() >= 2) {
                add(Json::object({{"book", b.path}, {"page", first.index.json()}, {"kind", "spread"}, {"first", b.doc.pages[0]->index.json()},
                                  {"second", b.doc.pages[1]->index.json()}, {"to_trim", false}, {"dpi", 72}, {"mode", "proof"}}));
            }
        }
        genko::test::write_bytes(path("jobs.json"), genko::core::dump_python(jobs_));
        // (four processes: pages at 350 dpi next to an ASan build of this test fit a 4 GB container)
        r = genko::test::render_harness({"render", path("jobs.json"), "--workers", "4"}, scratch_.path(), 3600000);
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
    }

    void pages() {
        int failures = 0;
        int compared = 0;
        int with_3d = 0;
        int regions = 0;
        std::map<std::string, int> kinds;
        unsigned seed = 20261003;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return n <= 1 ? 0 : static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const Json& job = jobs_[i];
            if (job.contains("kind")) continue;
            const Book& b = book_of(job["book"].get<std::string>());
            const genko::core::Page* page = page_of(b.doc, *genko::core::Num::from_json(job["page"]));
            QVERIFY(page != nullptr);
            use_brushes(b);
            render::RenderOptions options;
            options.mode = job["mode"].get<std::string>();
            const int dpi = job["dpi"].get<int>();
            const std::string label = QFileInfo(QString::fromStdString(b.path)).fileName().toStdString() + " p" + page->index.repr() +
                                      " " + options.mode + " @" + std::to_string(dpi);
            render::RenderResult got;
            try {
                got = render::render_page(*page, dpi, options, &b.doc);
            } catch (const render::NotYetPorted& e) {
                qWarning("%s: NotYetPorted(%s)", label.c_str(), e.element().c_str());
                ++failures;
                continue;
            }
            ++compared;
            if (options.mode != "print" && !page->prims.empty()) {
                ++with_3d;
                for (const Json& prim : page->prims) ++kinds[prim.value("kind", std::string("?"))];
            }
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            if (got.image.size() != want.size() || got.image.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(got.image, want);
                genko::test::keep_pictures(QStringLiteral("3d-page-%1").arg(i), got.image, want);
                qWarning("%s: %lld pixels differ (largest %d, mean %.4f), first at (%lld, %lld)", label.c_str(), diff.pixels, diff.largest,
                         diff.mean, diff.first % std::max(1, want.width()), diff.first / std::max(1, want.width()));
                ++failures;
                continue;
            }
            if (!((options.mode == "proof" && dpi == 150) || (options.mode == "name" && dpi == 350))) continue;
            // five parts of the page drawn alone (from nothing, or from what the whole page left remembered)
            for (int k = 0; k < 5; ++k) {
                const int w = 1 + next(got.image.width());
                const int h = 1 + next(got.image.height());
                const render::RenderRegion region{next(got.image.width() - w + 1), next(got.image.height() - h + 1), w, h};
                render::RenderOptions part = options;
                part.region = region;
                if (k % 2 == 0) render::clear_render_caches();
                const render::Image piece = render::render_page(*page, dpi, part, &b.doc).image;
                const render::Image cut = got.image.crop(render::Box{region.x, region.y, region.x + region.w, region.y + region.h});
                ++regions;
                if (piece.size() != cut.size() || piece.tobytes() != cut.tobytes()) {
                    genko::test::keep_pictures(QStringLiteral("3d-region-%1").arg(regions), piece, cut);
                    qWarning("%s region (%d, %d, %d, %d) differs from the page cut", label.c_str(), region.x, region.y, region.w, region.h);
                    ++failures;
                }
            }
        }
        std::string spread;
        for (const auto& [kind, count] : kinds) spread += kind + ":" + std::to_string(count) + " ";
        qInfo("3D pages: %d compared (%d drawn with 3D on them: %s), %d regions", compared, with_3d, spread.c_str(), regions);
        QVERIFY(compared >= 150);
        QVERIFY(with_3d >= 80);
        QVERIFY(kinds.size() >= 12);
        QVERIFY(regions >= 200);
        QCOMPARE(failures, 0);
    }

    void panelsAndSpreads() {
        int failures = 0;
        int frames = 0;
        int spreads = 0;
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const Json& job = jobs_[i];
            if (!job.contains("kind")) continue;
            const Book& b = book_of(job["book"].get<std::string>());
            const genko::core::Page* page = page_of(b.doc, *genko::core::Num::from_json(job["page"]));
            QVERIFY(page != nullptr);
            use_brushes(b);
            render::RenderOptions options;
            options.mode = job["mode"].get<std::string>();
            const int dpi = job["dpi"].get<int>();
            render::Image got;
            if (job["kind"] == "frame") {
                got = render::render_frame(*page, job["frame"].get<std::string>(), dpi, options, &b.doc);
                ++frames;
            } else {
                got = render::render_spread(b.doc, *genko::core::Num::from_json(job["first"]), *genko::core::Num::from_json(job["second"]), dpi,
                                            options, job["to_trim"].get<bool>());
                ++spreads;
            }
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            if (got.mode() != want.mode() || got.size() != want.size() || got.tobytes() != want.tobytes()) {
                genko::test::keep_pictures(QStringLiteral("3d-other-%1").arg(i), got, want);
                qWarning("%s %s p%s: differs", job["kind"].get<std::string>().c_str(), job["book"].get<std::string>().c_str(),
                         page->index.repr().c_str());
                ++failures;
            }
        }
        qInfo("panels: %d, spreads: %d", frames, spreads);
        QVERIFY(frames >= 11);
        QVERIFY(spreads >= 3);
        QCOMPARE(failures, 0);
    }

    void cleanupTestCase() {
        render::brushes::clear_custom();
        render::clear_render_caches();
    }
};

QTEST_GUILESS_MAIN(TestContract3dRender)
#include "test_contract_3d_render.moc"
