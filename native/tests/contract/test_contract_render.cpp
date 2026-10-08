// render::render_page against Python's genko.render.render_page, every pixel (RGB):
//  - 40 random books (tools/migration/render_harness.py make-books, seed fixed): 1–3 pages, up to 8 layers with every
//    brush and custom brushes, rasters of every PNG mode and size, patches, masks, every blend mode, opacity, clipping,
//    lock_alpha, fills and gradients, corrections, layer effects, panels cut, slanted, bowed, rounded, bleeding, with
//    every border style, paper colours, rulers and onion skins, lines of dialogue in balloons of every shape with
//    tails of every kind (joined, turned, cut, drawn by hand, set under a layer) and lines not placed (labels); in
//    print, proof and name at 72, 150 and 350 dpi;
//  - the three legacy books (data/legacy);
//  - a line added to a page drawn before (the remembered layer picture is drawn on, as in Python).
// Tone layers, effect lines and layer screens (M3-B), the 3D guides (M3-C) and lines with their balloons (M4) are drawn
// on both sides. Pages with what this step does not draw yet (placed pictures and their finish, cover folds) must say
// so (NotYetPorted) and are then drawn with skip_unported, against Python with the same things left out.
// Regions: parts of a page drawn alone are the same as the whole page cut (with and without remembered lines).
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
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

const std::vector<std::string> kModes{"print", "proof", "name"};
const std::vector<int> kDpis{72, 150, 350};

}  // namespace

class TestContractRender : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    std::map<std::string, Book> books_;
    Json manifest_;
    Json jobs_ = Json::array();

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

    static const genko::core::Page* page_of(const genko::core::Document& doc, const genko::core::Num& index) {
        for (const auto& p : doc.pages) {
            if (p->index == index) return p.get();
        }
        return nullptr;
    }

    // The names a page may give for NotYetPorted (and its onion page's).
    std::set<std::string> allowed(const std::string& name, const genko::core::Document& doc, const genko::core::Page& page) {
        std::set<std::string> out;
        if (manifest_.contains(name)) {
            const Json& pages = manifest_[name];
            const auto add = [&](const genko::core::Num& index) {
                const std::string key = index.repr();
                if (pages.contains(key)) {
                    for (const Json& n : pages[key]) out.insert(n.get<std::string>());
                }
            };
            add(page.index);
            if (page.onion_from) add(*page.onion_from);
        } else {
            out = {"nombre", "placed", "covers", "anim", "finish"};  // (lines and balloons are drawn since M4)
        }
        out.erase("onion");
        return out;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString books = scratch_.path() + QStringLiteral("/books");
        auto py = genko::test::render_harness({"make-books", books, "--seed", "20261002", "--count", "40"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        manifest_ = genko::test::read_json(books + QStringLiteral("/MANIFEST.json"));

        // the jobs: every page of every book in every mode at every resolution; the legacy books too
        QStringList paths;
        for (int i = 0; i < 40; ++i) paths << books + QStringLiteral("/book-%1.genko").arg(i, 2, 10, QLatin1Char('0'));
        for (const char* v : {"v1", "v2", "v3"}) paths << genko::test::test_data(QStringLiteral("legacy/book-%1.genko").arg(v));
        QDir().mkpath(scratch_.path() + QStringLiteral("/py"));
        int n = 0;
        for (const QString& path : paths) {
            const Book& b = book(path.toStdString());
            for (const auto& page : b.doc.pages) {
                for (const std::string& mode : kModes) {
                    for (const int dpi : kDpis) {
                        Json job = Json::object();
                        job["book"] = b.path;
                        job["page"] = page->index.json();
                        job["dpi"] = dpi;
                        job["mode"] = mode;
                        job["skip_unported"] = true;
                        job["out"] = (scratch_.path() + QStringLiteral("/py/%1.png").arg(n++)).toStdString();
                        jobs_.push_back(job);
                    }
                }
            }
        }
        // the other ways to draw: a panel, a spread, one layer alone, black and white, plain lines, crop marks
        const auto add = [&](Json job) {
            job["skip_unported"] = true;
            job["out"] = (scratch_.path() + QStringLiteral("/py/%1.png").arg(n++)).toStdString();
            jobs_.push_back(std::move(job));
        };
        for (int i = 0; i < 40; ++i) {
            const Book& b = book(paths[i].toStdString());
            const auto& first = *b.doc.pages.front();
            Json base = Json::object({{"book", b.path}, {"page", first.index.json()}});
            int frames = 0;
            for (const genko::core::Frame* f : first.leaf_frames()) {
                if (frames++ == 2) break;
                Json job = base;
                job["kind"] = "frame";
                job["frame"] = f->id;
                job["dpi"] = 100;
                job["mode"] = i % 2 == 0 ? "proof" : "print";
                add(job);
            }
            if (b.doc.pages.size() >= 2) {
                for (const bool to_trim : {false, true}) {
                    Json job = base;
                    job["kind"] = "spread";
                    job["first"] = b.doc.pages[0]->index.json();
                    job["second"] = b.doc.pages[1]->index.json();
                    job["to_trim"] = to_trim;
                    job["dpi"] = 72;
                    job["mode"] = to_trim ? "print" : "name";
                    add(job);
                }
            }
            for (const auto& layer : first.layers) {
                Json job = base;
                job["kind"] = "layer";
                job["layer"] = layer.id;
                job["dpi"] = 72;
                job["mode"] = "proof";
                add(job);
            }
            for (const int threshold : {180, 100}) {
                Json job = base;
                job["kind"] = "bitonal";
                job["threshold"] = threshold;
                job["dpi"] = 100;
                job["mode"] = "print";
                add(job);
            }
            {
                Json job = base;
                job["kind"] = "bitonal";
                job["threshold"] = 180;
                job["screen"] = Json::object({{"pattern", "noise"}, {"black", 0.2}, {"white", 0.9}});
                job["dpi"] = 100;
                job["mode"] = "print";
                add(job);
            }
            for (const int dpi : {150, 32, 24}) {
                Json job = base;
                job["rough"] = dpi == 150;
                job["dpi"] = dpi;
                job["mode"] = "proof";
                add(job);
            }
            {
                Json job = base;
                job["crop_marks"] = true;
                job["dpi"] = 72;
                job["mode"] = "print";
                add(job);
            }
        }
        // a line added to a page drawn just before: page 1 of a few books, the first strokes layer
        for (int i : {0, 3, 7, 12}) {
            const Book& b = book(paths[i].toStdString());
            const auto& page = *b.doc.pages.front();
            for (const auto& layer : page.layers) {
                if (layer.kind != genko::core::LayerKind::Strokes || layer.stroke_count() == 0 || !layer.visible) continue;
                Json job = Json::object();
                job["book"] = b.path;
                job["page"] = page.index.json();
                job["dpi"] = 150;
                job["mode"] = "proof";
                job["skip_unported"] = true;
                job["then"] = Json::object({{"layer", layer.id},
                                             {"id", "000000abcdef"},
                                             {"points", Json::array({Json::array({5.5, 6.25}), Json::array({30.0, 40.5}),
                                                                     Json::array({12.0, 50.0})})},
                                             {"pressure", Json::array({0.2, 0.9, 0.5})},
                                             {"width_mm", 1.2},
                                             {"kind", "gpen"}});
                job["out"] = (scratch_.path() + QStringLiteral("/py/%1.png").arg(n++)).toStdString();
                jobs_.push_back(job);
                break;
            }
        }
        const QString jobs_file = scratch_.path() + QStringLiteral("/jobs.json");
        QFile f(jobs_file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const std::string text = genko::core::dump_python(jobs_);
        f.write(text.data(), static_cast<qint64>(text.size()));
        f.close();
        // (four processes: the reference draws pages at 350 dpi, and eight of them next to an ASan build of this test
        // can pass a 4 GB container's memory)
        py = genko::test::render_harness({"render", jobs_file, "--workers", "4"}, scratch_.path(), 3600000);
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
    }

    void pages() {
        int failures = 0;
        int strict = 0;
        int skipped = 0;
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const Json& job = jobs_[i];
            if (job.contains("then") || job.contains("kind") || job.contains("rough") || job.contains("crop_marks")) continue;
            const Book& b = book(job["book"].get<std::string>());
            const genko::core::Page* page = page_of(b.doc, *genko::core::Num::from_json(job["page"]));
            QVERIFY(page != nullptr);
            render::brushes::clear_custom();
            render::brushes::register_brushes(b.doc.brush_custom);
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = job["mode"].get<std::string>();
            const int dpi = job["dpi"].get<int>();
            const std::string name = QFileInfo(QString::fromStdString(b.path)).fileName().toStdString();
            const std::string label = name + " p" + page->index.repr() + " " + options.mode + " @" + std::to_string(dpi);
            render::RenderResult got;
            try {
                got = render::render_page(*page, dpi, options, &b.doc);
                ++strict;
            } catch (const render::NotYetPorted& e) {
                const std::set<std::string> ok = allowed(name, b.doc, *page);
                if (!ok.contains(e.element())) {
                    qWarning("%s: NotYetPorted(%s), not one of the page's", label.c_str(), e.element().c_str());
                    ++failures;
                    continue;
                }
                options.skip_unported = true;
                render::clear_render_caches();
                got = render::render_page(*page, dpi, options, &b.doc);
                ++skipped;
                if (got.omitted.empty()) {
                    qWarning("%s: left nothing out", label.c_str());
                    ++failures;
                }
            }
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            if (got.image.size() != want.size() || got.image.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(got.image, want);
                genko::test::keep_pictures(QStringLiteral("page-%1").arg(i), got.image, want);
                qWarning("%s: %lld pixels differ (largest %d, mean %.4f), first at (%lld, %lld)", label.c_str(), diff.pixels,
                         diff.largest, diff.mean, diff.first % std::max(1, want.width()), diff.first / std::max(1, want.width()));
                ++failures;
            }
        }
        qInfo("pages: %d drawn strictly, %d with skip_unported", strict, skipped);
        QVERIFY(strict > 400);  // (most pages carry nothing this step leaves out)
        QCOMPARE(failures, 0);
    }

    void others() {
        int failures = 0;
        std::map<std::string, int> counts;
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const Json& job = jobs_[i];
            if (job.contains("then") || !(job.contains("kind") || job.contains("rough") || job.contains("crop_marks"))) continue;
            const Book& b = book(job["book"].get<std::string>());
            const genko::core::Page* page = page_of(b.doc, *genko::core::Num::from_json(job["page"]));
            QVERIFY(page != nullptr);
            render::brushes::clear_custom();
            render::brushes::register_brushes(b.doc.brush_custom);
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = job["mode"].get<std::string>();
            options.skip_unported = true;
            options.rough = job.value("rough", false);
            options.crop_marks = job.value("crop_marks", false);
            const int dpi = job["dpi"].get<int>();
            const std::string kind = job.value("kind", std::string("page"));
            render::Image got;
            if (kind == "frame") {
                got = render::render_frame(*page, job["frame"].get<std::string>(), dpi, options, &b.doc);
            } else if (kind == "spread") {
                got = render::render_spread(b.doc, *genko::core::Num::from_json(job["first"]), *genko::core::Num::from_json(job["second"]),
                                            dpi, options, job["to_trim"].get<bool>());
            } else if (kind == "layer") {
                const genko::core::Layer* layer = nullptr;
                for (const auto& l : page->layers) {
                    if (l.id == job["layer"].get<std::string>()) layer = &l;
                }
                got = render::layer_image(*page, *layer, dpi, &b.doc, true);
            } else {
                got = render::render_page(*page, dpi, options, &b.doc).image;
                if (kind == "bitonal") {
                    const Json* screen = job.contains("screen") ? &job["screen"] : nullptr;
                    got = render::to_bitonal(got, job["threshold"].get<int>(), screen);
                }
            }
            const std::string label = kind + (options.rough ? "+rough" : "") + (options.crop_marks ? "+marks" : "");
            ++counts[label];
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            if (got.mode() != want.mode() || got.size() != want.size() || got.tobytes() != want.tobytes()) {
                genko::test::keep_pictures(QStringLiteral("other-%1").arg(i), got, want);
                qWarning("%s %s p%s @%d: differs (%s %dx%d vs %s %dx%d)", label.c_str(), job["book"].get<std::string>().c_str(),
                         page->index.repr().c_str(), dpi, std::string(got.mode()).c_str(), got.width(), got.height(),
                         std::string(want.mode()).c_str(), want.width(), want.height());
                ++failures;
            }
        }
        for (const auto& [label, count] : counts) qInfo("%s: %d", label.c_str(), count);
        QVERIFY(counts.size() >= 7);
        QCOMPARE(failures, 0);
    }

    void added_line() {
        int compared = 0;
        int failures = 0;
        for (const Json& job : jobs_) {
            if (!job.contains("then")) continue;
            const Book& b = book(job["book"].get<std::string>());
            genko::core::Document doc = b.doc;  // (a copy: the line is added to it)
            const auto index = *genko::core::Num::from_json(job["page"]);
            std::size_t at = 0;
            while (!(doc.pages[at]->index == index)) ++at;
            render::brushes::clear_custom();
            render::brushes::register_brushes(doc.brush_custom);
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = "proof";
            options.skip_unported = true;
            (void)render::render_page(*doc.pages[at], 150, options, &doc);  // (the layers' lines are remembered)
            genko::core::Page& page = doc.edit_page(at);
            for (auto& layer : page.layers) {
                if (layer.id != job["then"]["layer"].get<std::string>()) continue;
                auto stroke = std::make_shared<genko::core::Stroke>();
                stroke->id = job["then"]["id"].get<std::string>();
                for (const Json& p : job["then"]["points"]) stroke->points.push_back({p[0].get<double>(), p[1].get<double>()});
                stroke->pressure = job["then"]["pressure"].get<std::vector<double>>();
                stroke->width_mm = job["then"]["width_mm"].get<double>();
                stroke->kind = job["then"]["kind"].get<std::string>();
                std::vector<genko::core::StrokePtr> items = layer.strokes->items;
                items.push_back(stroke);
                layer.strokes = genko::core::make_strokes(std::move(items));
            }
            const render::Image got = render::render_page(*doc.pages[at], 150, options, &doc).image;
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(job["out"].get<std::string>())));
            ++compared;
            if (got.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(got, want);
                genko::test::keep_pictures(QStringLiteral("added-%1").arg(compared), got, want);
                qWarning("added line %d: %lld pixels differ", compared, diff.pixels);
                ++failures;
            }
        }
        qInfo("added lines: %d pages", compared);
        QVERIFY(compared >= 3);
        QCOMPARE(failures, 0);
    }

    void regions() {
        // five random parts of pages, drawn alone, against the whole page cut
        unsigned seed = 20261002;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return n <= 1 ? 0 : static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        int checked = 0;
        int failures = 0;
        for (const auto& [path, b] : books_) {
            for (const auto& page_ptr : b.doc.pages) {
                const genko::core::Page& page = *page_ptr;
                for (const auto& [mode, dpi] : {std::pair<std::string, int>{"proof", 150}, {"print", 350}}) {
                    render::brushes::clear_custom();
                    render::brushes::register_brushes(b.doc.brush_custom);
                    render::RenderOptions options;
                    options.mode = mode;
                    options.skip_unported = true;
                    render::clear_render_caches();
                    const render::Image whole = render::render_page(page, dpi, options, &b.doc).image;
                    for (int k = 0; k < 5; ++k) {
                        const int w = 1 + next(whole.width());
                        const int h = 1 + next(whole.height());
                        const render::RenderRegion r{next(whole.width() - w + 1), next(whole.height() - h + 1), w, h};
                        render::RenderOptions part = options;
                        part.region = r;
                        if (k % 2 == 0) render::clear_render_caches();  // (drawn from nothing, or from remembered lines)
                        const render::Image got = render::render_page(page, dpi, part, &b.doc).image;
                        const render::Image want = whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h});
                        ++checked;
                        if (got.tobytes() != want.tobytes()) {
                            genko::test::keep_pictures(QStringLiteral("region-%1").arg(checked), got, want);
                            qWarning("%s p%s %s @%d region (%d, %d, %d, %d) differs", path.c_str(), page.index.repr().c_str(),
                                     mode.c_str(), dpi, r.x, r.y, r.w, r.h);
                            ++failures;
                        }
                    }
                }
            }
        }
        qInfo("regions: %d", checked);
        QVERIFY(checked > 300);
        QCOMPARE(failures, 0);
    }

    void cleanupTestCase() {
        render::brushes::clear_custom();
        render::clear_render_caches();
    }
};

QTEST_GUILESS_MAIN(TestContractRender)
#include "test_contract_render.moc"
