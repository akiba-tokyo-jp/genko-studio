// Tone layers, effect lines and layer screens drawn against Python's genko.render (M3-B), every pixel (RGB):
//   - 30 random books (tone_harness.py make-books --render: tones of every pattern with gradients, dot shapes, moved
//     screens, patches, regions and scrapes; effect lines of every kind with bundles, tapers, paths, avoid / within;
//     layer screens of every pattern) and the fixture book: each page in print, proof and name at 72, 150 and 350 dpi;
//   - the same pages on screen with the dots shown (render.SCREEN_DOTS) in proof and name at 150 dpi;
//   - each tone layer alone (layer_image) at 72 and 150 dpi;
//   - pages in black and white with a screen (to_bitonal: dot, line, cross, noise; shapes, angles) at 100 dpi;
//   - five random parts of each page drawn alone, the same as the whole page cut (in print and proof, 150 and 350 dpi).
// No tolerance: the same pixels or a failure (the pictures kept for a look). Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <map>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

struct Book {
    genko::core::Document doc;
    std::string path;
};

const genko::core::Page* page_of(const genko::core::Document& doc, const genko::core::Num& index) {
    for (const auto& p : doc.pages) {
        if (p->index == index) return p.get();
    }
    return nullptr;
}

bool is_tone(const genko::core::Layer& layer) {
    return layer.role == genko::core::LayerRole::Tone || layer.kind == genko::core::LayerKind::Tone;
}

}  // namespace

class TestContractToneRender : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    std::map<std::string, Book> books_;
    Json jobs_ = Json::array();

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    const Book& book(const std::string& path) {
        auto it = books_.find(path);
        if (it == books_.end()) {
            Book b;
            b.path = path;
            b.doc = genko::storage::load_document(genko::storage::path_from_utf8(path)).document;
            it = books_.emplace(path, std::move(b)).first;
        }
        return it->second;
    }

    void add(Json job) {
        job["out"] = path(QStringLiteral("py/%1.png").arg(jobs_.size())).toStdString();
        jobs_.push_back(std::move(job));
    }

    render::Image draw(const Json& job, const Book& b, const genko::core::Page& page) {
        render::brushes::clear_custom();
        render::brushes::register_brushes(b.doc.brush_custom);
        render::clear_render_caches();
        const std::string kind = job.value("kind", std::string("page"));
        const int dpi = job["dpi"].get<int>();
        if (kind == "layer") {
            for (const auto& layer : page.layers) {
                if (layer.id == job["layer"].get<std::string>()) return render::layer_image(page, layer, dpi, &b.doc);
            }
            throw genko::core::Error("key", "no layer");
        }
        render::RenderOptions options;
        options.mode = job["mode"].get<std::string>();
        options.screen_dots = job.value("screen_dots", false);
        render::Image image = render::render_page(page, dpi, options, &b.doc).image;
        if (kind == "bitonal") {
            const Json* screen = job.contains("screen") ? &job["screen"] : nullptr;
            image = render::to_bitonal(image, job["threshold"].get<int>(), screen);
        }
        return image;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString script = genko::test::repo_root() + QStringLiteral("/tools/migration/tone_harness.py");
        const auto harness = [&](const QStringList& args) {
            return genko::test::run(genko::test::python_ref(), QStringList{script} + args, genko::test::python_env(scratch_.path()), 3600000);
        };
        auto made = harness({"make-books", path("books"), "--seed", "20261003", "--count", "30", "--render"});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        made = harness({"fixture", path("fixture")});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        QStringList paths;
        for (int i = 0; i < 30; ++i) paths << path(QStringLiteral("books/book-%1.genko").arg(i, 2, 10, QLatin1Char('0')));
        paths << path("fixture/fixture.genko");
        QDir().mkpath(path("py"));
        const Json screens = genko::core::parse_python_json(R"([{"lpi": 60, "dpi": 600}, {"pattern": "line", "lpi": 45, "angle": 15, "black": 0.2},
            {"pattern": "cross", "lpi": 30, "white": 0.8, "dpi": 300}, {"pattern": "noise", "black": 0.15}, {"shape": "square", "lpi": 50},
            {"shape": "ellipse", "angle": 75, "lpi": 35, "dpi": 150}, {"pattern": "wave", "shape": "diamond", "lpi": 40}])");
        int bitonal = 0;
        for (const QString& p : paths) {
            const Book& b = book(p.toStdString());
            for (const auto& page : b.doc.pages) {
                Json base = Json::object({{"book", b.path}, {"page", page->index.json()}});
                for (const char* mode : {"print", "proof", "name"}) {
                    for (const int dpi : {72, 150, 350}) {
                        Json job = base;
                        job["mode"] = mode;
                        job["dpi"] = dpi;
                        add(job);
                    }
                }
                for (const char* mode : {"proof", "name"}) {
                    Json job = base;
                    job["mode"] = mode;
                    job["dpi"] = 150;
                    job["screen_dots"] = true;
                    add(job);
                }
                for (const auto& layer : page->layers) {
                    if (!is_tone(layer)) continue;
                    for (const int dpi : {72, 150}) {
                        Json job = base;
                        job["kind"] = "layer";
                        job["layer"] = layer.id;
                        job["dpi"] = dpi;
                        job["mode"] = "proof";
                        add(job);
                    }
                }
                Json job = base;
                job["kind"] = "bitonal";
                job["mode"] = "proof";
                job["dpi"] = 100;
                job["threshold"] = 180;
                job["screen"] = screens[static_cast<std::size_t>(bitonal++) % screens.size()];
                add(job);
            }
        }
        const QString jobs_file = path("jobs.json");
        genko::test::write_bytes(jobs_file, genko::core::dump_python(jobs_));
        made = harness({"render", jobs_file});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(3000).constData());
    }

    void pages() {
        int compared = 0;
        int failures = 0;
        int python_failed = 0;
        std::map<std::string, int> counts;
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const Json& job = jobs_[i];
            const Book& b = book(job["book"].get<std::string>());
            const genko::core::Page* page = page_of(b.doc, *genko::core::Num::from_json(job["page"]));
            QVERIFY(page != nullptr);
            const std::string kind = job.value("kind", std::string("page")) + (job.value("screen_dots", false) ? "+dots" : "");
            const std::string label = QFileInfo(QString::fromStdString(b.path)).fileName().toStdString() + " p" + page->index.repr() + " " + kind +
                                      " " + job["mode"].get<std::string>() + " @" + std::to_string(job["dpi"].get<int>());
            const QString out = QString::fromStdString(job["out"].get<std::string>());
            if (QFileInfo::exists(out + ".error")) {  // (Python cannot draw it: neither can this build)
                ++python_failed;
                bool refused = false;
                try {
                    (void)draw(job, b, *page);
                } catch (const std::exception&) {
                    refused = true;
                }
                if (!refused) {
                    qWarning("%s: Python failed (%s), this build drew it", label.c_str(), genko::test::read_bytes(out + ".error").c_str());
                    ++failures;
                }
                continue;
            }
            render::Image got;
            try {
                got = draw(job, b, *page);
            } catch (const std::exception& error) {
                qWarning("%s: %s", label.c_str(), error.what());
                ++failures;
                continue;
            }
            const render::Image want = render::read_png(genko::test::read_bytes(out));
            ++compared;
            ++counts[kind];
            if (got.mode() != want.mode() || got.size() != want.size()) {
                qWarning("%s: %s %dx%d, Python %s %dx%d", label.c_str(), std::string(got.mode()).c_str(), got.width(), got.height(),
                         std::string(want.mode()).c_str(), want.width(), want.height());
                ++failures;
            } else if (got.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(got, want);
                genko::test::keep_pictures(QStringLiteral("tone-%1").arg(i), got, want);
                qWarning("%s: %lld pixels differ (largest %d, mean %.4f), first at (%lld, %lld)", label.c_str(), diff.pixels, diff.largest,
                         diff.mean, diff.first % std::max(1, want.width()), diff.first / std::max(1, want.width()));
                ++failures;
            }
        }
        for (const auto& [kind, n] : counts) qInfo("%s: %d", kind.c_str(), n);
        qInfo("compared %d pictures (Python could not draw %d)", compared, python_failed);
        QVERIFY(compared >= 600);  // (31 books of one or two pages: about 700 pictures)
        QCOMPARE(failures, 0);
    }

    void regions() {
        unsigned seed = 20261003;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return n <= 1 ? 0 : static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        int checked = 0;
        int failures = 0;
        for (const auto& [path, b] : books_) {
            for (const auto& page_ptr : b.doc.pages) {
                for (const auto& [mode, dpi] : {std::pair<std::string, int>{"print", 150}, {"proof", 350}, {"print", 350}}) {
                    render::brushes::clear_custom();
                    render::brushes::register_brushes(b.doc.brush_custom);
                    render::RenderOptions options;
                    options.mode = mode;
                    options.screen_dots = true;
                    render::clear_render_caches();
                    const render::Image whole = render::render_page(*page_ptr, dpi, options, &b.doc).image;
                    for (int k = 0; k < 5; ++k) {
                        const int w = 1 + next(whole.width());
                        const int h = 1 + next(whole.height());
                        const render::RenderRegion r{next(whole.width() - w + 1), next(whole.height() - h + 1), w, h};
                        render::RenderOptions part = options;
                        part.region = r;
                        if (k % 2 == 0) render::clear_render_caches();
                        const render::Image got = render::render_page(*page_ptr, dpi, part, &b.doc).image;
                        ++checked;
                        if (got.tobytes() != whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h}).tobytes()) {
                            genko::test::keep_pictures(QStringLiteral("tone-region-%1").arg(checked), got,
                                                       whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h}));
                            qWarning("%s p%s %s @%d region (%d, %d, %d, %d) differs", path.c_str(), page_ptr->index.repr().c_str(), mode.c_str(), dpi,
                                     r.x, r.y, r.w, r.h);
                            ++failures;
                        }
                    }
                }
            }
        }
        qInfo("regions: %d", checked);
        QVERIFY(checked >= 5 * 3 * 31);
        QCOMPARE(failures, 0);
    }

    void cleanupTestCase() {
        render::brushes::clear_custom();
        render::clear_render_caches();
    }
};

QTEST_GUILESS_MAIN(TestContractToneRender)
#include "test_contract_tone_render.moc"
