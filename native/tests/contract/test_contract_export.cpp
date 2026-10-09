// The exports (formats/: genko/export.py, profiles.py, pack.py, psd.py's writer, app/exporting.py) against Python's on
// the same books (render_harness.py make-export-books: monochrome and colour books, spreads, covers front and back, a
// jacket, a band, nombres, tones, effect lines, balloons, raster layers with masks): each call made by Python
// (render_harness.py export-cases) and by this build into the same folders, what it returned or raised the same, and
// every file it wrote the same bytes (export_files.hpp: only the times Python's files carry are left out). The
// encoders on their own first (render_harness.py save-cases): PNG, JPEG and TIFF as Pillow writes them, byte for byte.
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <filesystem>
#include <functional>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/base64.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "export_files.hpp"
#include "formats/checks.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "render/brushes.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "rendertest.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"

using genko::core::Json;
namespace formats = genko::formats;
namespace fs = std::filesystem;

namespace {

const fs::path kCmykIcc = fs::path(GENKO_TEST_DATA) / "colour" / "photocraft-coated-cmyk.icc";

std::string u8(const QString& s) { return s.toStdString(); }
fs::path path_of(const QString& s) { return genko::core::path_from_utf8(s.toStdString()); }

// Python's (type, message) of an exception this build raised in its place.
Json error_of(const std::exception& e) {
    if (const auto* n = dynamic_cast<const genko::render::NotYetPorted*>(&e)) return Json::array({"NotYetPorted", n->what()});
    if (const auto* c = dynamic_cast<const genko::core::Error*>(&e)) {
        if (c->code() == "value") return Json::array({"ValueError", c->what()});  // (PyValueError, and Pillow's ValueError)
        return Json::array({"Error:" + c->code(), c->what()});
    }
    return Json::array({"exception", e.what()});
}

std::optional<std::int64_t> opt_int(const Json& kw, const char* key) {
    if (!kw.contains(key) || kw[key].is_null()) return std::nullopt;
    return kw[key].get<std::int64_t>();
}

// The most memory this process has held (kB) since the last reset (Linux: VmHWM; reset: from what it holds now through
// /proc/self/clear_refs); -1 where that cannot be read.
long long peak_kb(bool reset) {
#ifdef __linux__
    if (reset) {
        const int fd = ::open("/proc/self/clear_refs", O_WRONLY);
        if (fd < 0) return -1;
        const bool written = ::write(fd, "5", 1) == 1;
        ::close(fd);
        if (!written) return -1;
    }
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith("VmHWM:")) return line.mid(6).trimmed().split(' ').front().toLongLong();
    }
#else
    (void)reset;
#endif
    return -1;
}

Json paths(const std::vector<fs::path>& files) {
    Json out = Json::array();
    for (const auto& f : files) out.push_back(genko::core::path_to_utf8(f));
    return out;
}

}  // namespace

class TestContractExport : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;
    QString out_;  // where both write (Python's moved to out_py_ before this build writes)
    QString out_py_;
    Json jobs_ = Json::array();
    Json results_;

    void add(const std::string& id, const std::string& book, const std::string& call, const std::string& dest, Json kwargs = Json::object()) {
        jobs_.push_back(Json{{"id", id}, {"book", u8(books_) + "/" + book}, {"call", call}, {"dest", u8(out_) + "/" + dest}, {"kwargs", kwargs}});
    }

    // The job called on this build: what it returned (as Python's result: paths as str), or {"error": …}.
    Json call(const Json& job) {
        genko::render::clear_render_caches();
        const auto loaded = genko::storage::load_document(path_of(QString::fromStdString(job["book"].get<std::string>())));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const Json kw = job["kwargs"];
        const std::string fn = job["call"].get<std::string>();
        const fs::path dest = genko::core::path_from_utf8(job["dest"].get<std::string>());
        try {
            if (fn == "export_png_sequence") {
                return Json{{"result", paths(formats::export_png_sequence(doc, dest, kw.value("working_dpi", 150), kw.value("mode", std::string("print"))))}};
            }
            if (fn == "export_print") {
                std::optional<Json> screen;
                if (kw.contains("screen")) screen = kw["screen"];
                return Json{{"result", paths(formats::export_print(doc, dest, kw.value("fmt", std::string("png")), opt_int(kw, "dpi"),
                                                                   kw.value("threshold", 180), kw.value("crop_marks", true),
                                                                   kw.value("area", std::string("paper")), kw.value("color", std::string("auto")),
                                                                   kw.value("icc", std::string()), screen))}};
            }
            if (fn == "export_layers") {
                return Json{{"result", paths(formats::export_layers(doc, dest, kw.value("dpi", 350), kw.value("area", std::string("paper"))))}};
            }
            if (fn == "export_strip") return Json{{"result", genko::core::path_to_utf8(formats::export_strip(doc, dest, kw.value("dpi", 150)))}};
            if (fn == "export_epub") {
                std::optional<int> long_edge;
                if (kw.contains("long_edge")) long_edge = kw["long_edge"].get<int>();
                return Json{{"result", genko::core::path_to_utf8(formats::export_epub(doc, dest, kw.value("dpi", 150), kw.value("kindle", false), long_edge,
                                                                                      kw.value("gray", false), kw.value("jpeg", false), kw.value("dots", false)))}};
            }
            if (fn == "export_kindle") {
                std::optional<bool> gray;
                if (kw.contains("gray")) gray = kw["gray"].get<bool>();
                return Json{{"result", genko::core::path_to_utf8(formats::export_kindle(doc, dest, kw.value("long_edge", formats::kKindleLongEdge), gray,
                                                                                        kw.value("dots", false)))}};
            }
            if (fn == "export_webtoon") {
                return Json{{"result", paths(formats::export_webtoon(doc, dest, kw.value("width_px", 800), kw.value("max_height", 1280), kw.value("gap_px", 0),
                                                                     kw.value("fmt", std::string("png")), kw.value("quality", 92)))}};
            }
            if (fn == "export_sns") {
                return Json{{"result", paths(formats::export_sns(doc, dest, kw.value("long_edge", 2048), kw.value("fmt", std::string("jpeg")),
                                                                 kw.value("quality", 92), kw.value("spreads", false)))}};
            }
            if (fn == "export_pack") return Json{{"result", paths(formats::export_pack(doc, dest, kw.value("preset", std::string("shueisha")), opt_int(kw, "dpi")))}};
            if (fn == "export_psd_pages") return Json{{"result", paths(formats::export_psd_pages(doc, dest, opt_int(kw, "dpi")))}};
            if (fn == "export_psd") {
                return Json{{"result", genko::core::path_to_utf8(formats::export_psd(doc, dest, kw.value("dpi", std::int64_t{150}), kw.value("page", std::int64_t{1})))}};
            }
            if (fn == "run") {
                Json k = kw;
                const std::string key = k["key"].get<std::string>();
                formats::RunOptions o;
                o.official = k.value("official", false);
                o.dpi = opt_int(k, "dpi");
                o.width = k.value("width", 800);
                o.max_height = k.value("max_height", 1280);
                o.long_edge = k.value("long_edge", 2048);
                o.jpeg = k.value("jpeg", false);
                o.spreads = k.value("spreads", false);
                o.area = k.value("area", std::string("bleed"));
                if (k.contains("pages") && !k["pages"].is_null()) o.pages = k["pages"].get<std::vector<std::int64_t>>();
                o.color = k.value("color", std::string("auto"));
                if (k.contains("icc") && !k["icc"].is_null()) o.icc = k["icc"].get<std::string>();
                o.dots = k.value("dots", false);
                if (k.contains("screen")) o.screen = k["screen"];
                std::optional<fs::path> project;
                if (job.value("project", true)) project = genko::core::path_from_utf8(job["book"].get<std::string>());
                return Json{{"result", formats::run(doc, project, key, dest, o)}};
            }
            if (fn == "parse_pages") {
                return Json{{"result", formats::parse_pages(kw["text"].get<std::string>(), kw["count"].get<std::int64_t>())}};
            }
            if (fn == "default_dpi") return Json{{"result", formats::default_dpi(doc, kw["key"].get<std::string>())}};
            if (fn == "subset") {
                return Json{{"result", genko::storage::snapshot(formats::subset(doc, kw["pages"].get<std::vector<std::int64_t>>()), true)}};
            }
        } catch (const std::exception& e) {
            return Json{{"error", error_of(e)}};
        }
        return Json{{"error", Json::array({"unknown call", fn})}};
    }

    // Python's error and this build's mean the same: the same ValueError message; Python's StopIteration (no such page)
    // is this build's KeyError.
    static bool same_error(const Json& got, const Json& want) {
        if (want[0] == "StopIteration") return got[0] == "Error:key";
        return got == want;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        books_ = scratch_.path() + "/books";
        out_ = scratch_.path() + "/out";
        out_py_ = scratch_.path() + "/out_py";
        const auto made = genko::test::render_harness({"make-export-books", books_}, scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        // a profile that is not a CMYK one (RGB)
        genko::test::write_bytes(scratch_.path() + "/srgb.icc", genko::render::colour::srgb_icc());
        const std::string cmyk = genko::core::path_to_utf8(kCmykIcc);
        const std::string srgb = u8(scratch_.path()) + "/srgb.icc";

        for (const std::string b : {"mono", "colour", "jacket", "obi"}) {
            const std::string book = b + ".genko";
            add(b + "-seq", book, "export_png_sequence", b + "-seq", {{"working_dpi", 40}});
            add(b + "-seq-name", book, "export_png_sequence", b + "-seq-name", {{"working_dpi", 30}, {"mode", b == "mono" ? "name" : "proof"}});
            for (const std::string fmt : {"png", "tiff", "pdf"}) {
                for (const std::string color : {"auto", "rgb", "gray", "bitonal"}) {
                    add(b + "-" + fmt + "-" + color, book, "export_print", b + "-" + fmt + "-" + color,
                        {{"fmt", fmt}, {"dpi", 40}, {"color", color}, {"area", b == "obi" ? "paper" : b == "colour" ? "trim" : "bleed"}});
                }
            }
            add(b + "-png1", book, "export_print", b + "-png1", {{"fmt", "png1"}, {"dpi", 40}, {"threshold", 120}});
            add(b + "-tiff-cmyk", book, "export_print", b + "-tiff-cmyk", {{"fmt", "tiff"}, {"dpi", 40}, {"color", "cmyk"}, {"area", "trim"}});
            add(b + "-cmyk", book, "export_print", b + "-cmyk", {{"fmt", "cmyk"}, {"dpi", 40}, {"color", "cmyk"}, {"icc", cmyk}, {"area", "bleed"}});
            add(b + "-pdf-cmyk", book, "export_print", b + "-pdf-cmyk", {{"fmt", "pdf"}, {"dpi", 40}, {"color", "cmyk"}});
            add(b + "-pdf-cmyk-icc", book, "export_print", b + "-pdf-cmyk-icc", {{"fmt", "pdf"}, {"dpi", 30}, {"color", "cmyk"}, {"icc", cmyk}, {"area", "trim"}});
            add(b + "-screen", book, "export_print", b + "-screen",
                {{"fmt", "tiff"}, {"dpi", 60}, {"screen", {{"lpi", 30}, {"shape", "diamond"}}}});
            add(b + "-screen-pdf", book, "export_print", b + "-screen-pdf",
                {{"fmt", "pdf"}, {"dpi", 50}, {"color", "bitonal"}, {"screen", {{"lpi", 25}, {"angle", 15}}}});
            add(b + "-layers", book, "export_layers", b + "-layers", {{"dpi", 40}, {"area", b == "mono" ? "trim" : "bleed"}});
            add(b + "-layers-paper", book, "export_layers", b + "-layers-paper", {{"dpi", 30}});
            add(b + "-strip", book, "export_strip", b + "-strip/s.png", {{"dpi", 30}});
            add(b + "-webtoon", book, "export_webtoon", b + "-webtoon", {{"width_px", 200}, {"max_height", 300}});
            add(b + "-webtoon-jpg", book, "export_webtoon", b + "-webtoon-jpg", {{"width_px", 150}, {"max_height", 500}, {"gap_px", 10}, {"fmt", "jpeg"}});
            add(b + "-sns", book, "export_sns", b + "-sns", {{"long_edge", 300}, {"spreads", true}});
            add(b + "-sns-png", book, "export_sns", b + "-sns-png", {{"long_edge", 200}, {"fmt", "png"}, {"spreads", true}, {"quality", 80}});
            add(b + "-epub", book, "export_epub", b + "-epub/b.epub", {{"dpi", 40}});
            add(b + "-epub-jpeg", book, "export_epub", b + "-epub-jpeg/b.epub", {{"dpi", 30}, {"jpeg", true}, {"gray", true}, {"dots", true}});
            add(b + "-kindle", book, "export_kindle", b + "-kindle/k.epub", {{"long_edge", 300}});
            add(b + "-kindle-colour", book, "export_kindle", b + "-kindle-colour/k.epub", {{"long_edge", 250}, {"gray", false}});
            add(b + "-pack", book, "export_pack", b + "-pack", {{"dpi", 40}});
            add(b + "-psd", book, "export_psd_pages", b + "-psd", {{"dpi", 40}});
            add(b + "-psd-one", book, "export_psd", b + "-psd-one/one.psd", {{"dpi", 30}, {"page", 2}});
        }
        // Python's refusals
        add("bad-area", "mono.genko", "export_print", "bad-area", {{"area", "margin"}, {"dpi", 40}});
        add("bad-color", "mono.genko", "export_print", "bad-color", {{"color", "purple"}, {"dpi", 40}});
        add("cmyk-png", "mono.genko", "export_print", "cmyk-png", {{"color", "cmyk"}, {"dpi", 40}});
        add("rgb-icc", "colour.genko", "export_print", "rgb-icc", {{"fmt", "pdf"}, {"color", "cmyk"}, {"icc", srgb}, {"dpi", 40}});
        add("screen-lpi", "mono.genko", "export_print", "screen-lpi", {{"fmt", "tiff"}, {"dpi", 40}, {"screen", {{"lpi", 5}}}});
        add("screen-shape", "mono.genko", "export_print", "screen-shape", {{"fmt", "png1"}, {"dpi", 40}, {"screen", {{"shape", "star"}}}});
        add("psd-no-page", "mono.genko", "export_psd", "psd-no-page/x.psd", {{"page", 9}, {"dpi", 30}});
        add("webtoon-no-rows", "obi.genko", "export_webtoon", "webtoon-no-rows", {{"width_px", 100}, {"max_height", 0}});
        add("webtoon-minus", "obi.genko", "export_webtoon", "webtoon-minus", {{"width_px", 100}, {"max_height", -5}});
        add("webtoon-no-width", "obi.genko", "export_webtoon", "webtoon-no-width", {{"width_px", 0}, {"max_height", 100}});
        // the dispatcher (app/exporting.run) and its helpers
        for (const auto& [key, extra] : std::vector<std::pair<std::string, Json>>{
                 {"pdf", {{"dpi", 30}}}, {"tiff", {{"dpi", 30}, {"area", "paper"}}}, {"png", {{"dpi", 30}, {"color", "gray"}}},
                 {"cmyk", {{"dpi", 30}, {"icc", genko::core::path_to_utf8(kCmykIcc)}}}, {"layers", {{"dpi", 30}}}, {"psd", {{"dpi", 30}}},
                 {"pack", {{"dpi", 30}}}, {"webtoon", {{"width", 120}, {"max_height", 200}, {"jpeg", true}}},
                 {"sns", {{"long_edge", 160}, {"spreads", true}}}, {"epub", Json::object()}, {"kindle", {{"long_edge", 200}}},
                 {"strip", Json::object()}, {"png", {{"dpi", 30}, {"pages", {2}}}}, {"pdf", {{"dpi", 30}, {"pages", {1, 3}}, {"color", "rgb"}}},
                 {"sns", {{"long_edge", 160}, {"spreads", true}, {"pages", {3}}}}, {"png", {{"dpi", 30}, {"color", "cmyk"}}},
                 {"gif", Json::object()}, {"layers", {{"official", true}}}, {"pdf", {{"official", true}, {"pages", {1}}}},
                 {"png", {{"dpi", 30}, {"pages", {9}}}}, {"tiff", {{"dpi", 30}, {"pages", {3, 1, 2}}}}, {"png", {{"dpi", 30}, {"pages", {3, 1}}}}}) {
            Json kw = extra;
            kw["key"] = key;
            const std::string id = "run-" + std::to_string(jobs_.size());
            add(id, "mono.genko", "run", id, kw);
        }
        add("run-colour-sns", "colour.genko", "run", "run-colour-sns", {{"key", "sns"}, {"long_edge", 150}, {"spreads", true}, {"pages", {2, 3, 4}}});
        add("run-colour-epub", "colour.genko", "run", "run-colour-epub", {{"key", "epub"}, {"dpi", 30}, {"pages", {1, 3, 4}}, {"dots", true}});
        jobs_.push_back(Json{{"id", "run-no-project"}, {"book", u8(books_) + "/mono.genko"}, {"call", "run"}, {"project", false},
                             {"dest", u8(out_) + "/run-no-project"}, {"kwargs", {{"key", "pdf"}, {"official", true}}}});
        for (const auto& [text, count] : std::vector<std::pair<std::string, int>>{
                 {"3-5, 8", 10}, {"1〜2、3", 3}, {"5-3", 6}, {"2-2,2，1", 4}, {"x", 3}, {"0", 3}, {"", 3}, {" , ", 2}, {"1-x", 4}, {"9", 8}, {"２", 3},
                 // (numbers past 64 bits: out of the book, said with Python's digits)
                 {"99999999999999999999", 3}, {"1-99999999999999999999", 3}, {"99999999999999999998-99999999999999999999", 3},
                 {"1--99999999999999999999", 4}, {"+0_0099999999999999999999", 3}, {"٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩٩", 2}}) {
            const std::string id = "pages-" + std::to_string(jobs_.size());
            add(id, "mono.genko", "parse_pages", id, {{"text", text}, {"count", count}});
        }
        for (const std::string key : {"epub", "strip", "pdf", "kindle", "sns"}) add("dpi-" + key, "mono.genko", "default_dpi", "x", {{"key", key}});
        add("subset-23", "mono.genko", "subset", "x", {{"pages", {2}}});
        add("subset-c", "colour.genko", "subset", "x", {{"pages", {1, 3}}});

        genko::test::write_bytes(scratch_.path() + "/jobs.json", genko::core::dump_python(jobs_));
        const auto py = genko::test::render_harness({"export-cases", scratch_.path() + "/jobs.json", scratch_.path() + "/results.json"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        results_ = genko::test::read_json(scratch_.path() + "/results.json");
        QVERIFY(QDir().rename(out_, out_py_));
    }

    // The encoders alone: pictures Pillow saved as the exports save them (PNG with dpi, a profile, optimize; JPEG at a
    // quality, optimized, with a profile; TIFF in LZW and Group 4 with dpi and a profile), the same bytes.
    void encoders() {
        const QString file = scratch_.path() + "/save.json";
        const auto py = genko::test::render_harness({"save-cases", file, "--seed", "5", "--count", "70"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const Json cases = genko::test::read_json(file);
        const std::string icc = genko::core::a2b_base64(cases["icc"].get<std::string>());
        int compared = 0;
        for (const Json& c : cases["cases"]) {
            const genko::render::Image image = genko::render::Image::frombytes(
                c["mode"].get<std::string>(), genko::render::Size{c["size"][0].get<int>(), c["size"][1].get<int>()},
                genko::core::a2b_base64(c["pixels"].get<std::string>()));
            const Json& p = c["params"];
            const std::string fmt = c["format"].get<std::string>();
            std::string got;
            if (fmt == "PNG") {
                formats::PngSave s;
                if (p.contains("dpi")) s.dpi = std::array<double, 2>{p["dpi"][0].get<double>(), p["dpi"][1].get<double>()};
                if (p.contains("icc_profile")) s.icc = icc;
                s.optimize = p.value("optimize", false);
                got = formats::png_bytes(image, s);
            } else if (fmt == "JPEG") {
                formats::JpegSave s;
                s.quality = p.value("quality", -1);
                s.optimize = p.value("optimize", false);
                if (p.contains("icc_profile")) s.icc = icc;
                got = formats::jpeg_bytes(image, s);
            } else {
                formats::TiffSave s;
                s.compression = p["compression"].get<std::string>();
                s.dpi = std::array<double, 2>{p["dpi"][0].get<double>(), p["dpi"][1].get<double>()};
                if (p.contains("icc_profile")) s.icc = icc;
                got = formats::tiff_bytes(image, s);
            }
            const std::string want = genko::core::a2b_base64(c["bytes"].get<std::string>());
            QVERIFY2(got == want, qPrintable(QStringLiteral("%1 %2 %3x%4 %5: %6 bytes, Pillow's %7")
                                                 .arg(QString::fromStdString(fmt), QString::fromStdString(c["mode"].get<std::string>()))
                                                 .arg(image.width()).arg(image.height())
                                                 .arg(QString::fromStdString(p.dump())).arg(got.size()).arg(want.size())));
            ++compared;
        }
        QCOMPARE(compared, 70);
    }

    // What this build does not draw yet stops an export: a placed picture on the last page of a book (Python draws it;
    // this build says so, render::NotYetPorted) — and none of the files of the pages before it is left behind.
    void notYetPorted() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        genko::core::Document doc = loaded.document;
        genko::core::Layer placed;
        placed.id = "placed000001";
        placed.kind = genko::core::LayerKind::Placed;
        doc.edit_page(doc.pages.size() - 1).layers.push_back(placed);
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/unported";
        const auto refused = [&](const char* what, const std::function<void(const fs::path&)>& make) {
            const QString dir = root + "/" + what;
            bool stopped = false;
            try {
                make(path_of(dir));
            } catch (const genko::render::NotYetPorted& e) {
                stopped = e.element() == "placed";
            }
            QVERIFY2(stopped, what);
            QVERIFY2(genko::test::exports::tree(dir).empty(), what);  // (nothing written, not even the first pages)
        };
        refused("png", [&](const fs::path& d) { formats::export_print(doc, d, "png", 30); });
        refused("pdf", [&](const fs::path& d) { formats::export_print(doc, d, "pdf", 30); });
        refused("tiff", [&](const fs::path& d) { formats::export_print(doc, d, "tiff", 30); });
        refused("layers", [&](const fs::path& d) { formats::export_layers(doc, d, 30); });
        refused("psd", [&](const fs::path& d) { formats::export_page_psd(doc, *doc.pages.back(), d / "x.psd", 30); });
        refused("psd-pages", [&](const fs::path& d) { formats::export_psd_pages(doc, d, 30); });
        refused("pack", [&](const fs::path& d) { formats::export_pack(doc, d, "shueisha", 30); });
        refused("strip", [&](const fs::path& d) { formats::export_strip(doc, d / "s.png", 30); });
        refused("epub", [&](const fs::path& d) { formats::export_epub(doc, d / "b.epub", 30); });
        refused("webtoon", [&](const fs::path& d) { formats::export_webtoon(doc, d, 120, 100); });
        refused("sns", [&](const fs::path& d) { formats::export_sns(doc, d, 120); });
        // the dispatcher says so in its reply
        formats::RunOptions at30;
        at30.dpi = 30;
        const Json reply = formats::run(doc, std::nullopt, "png", path_of(root + "/run"), at30);
        QCOMPARE(reply.value("ok", true), false);
        QCOMPARE(QString::fromStdString(reply.value("code", std::string())), QStringLiteral("not_yet_ported"));
        QVERIFY(genko::test::exports::tree(root + "/run").empty());
        // and the official export (the studio's) is refused, after Python's own refusals
        formats::RunOptions official;
        official.official = true;
        const Json studio = formats::run(doc, path_of(books_ + "/mono.genko"), "pdf", path_of(root + "/official"), official);
        QCOMPARE(QString::fromStdString(studio.value("code", std::string())), QStringLiteral("not_yet_ported"));
        QVERIFY(!QFileInfo::exists(root + "/official"));
    }

    // Something in the way of one of an export's files (a folder of its name): nothing is moved into place, the files
    // there before stay as they were.
    void somethingInTheWayStopsTheWholeExport() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        QVERIFY(doc.pages.size() >= 3);
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/in-the-way";
        const auto name = [&](std::size_t page, const char* suffix) {
            return QString::fromStdString(formats::stem(doc) + "_" + genko::core::file_stem(*doc.pages[page]) + suffix);
        };
        const QString png = root + "/png";
        QVERIFY(QDir().mkpath(png + "/" + name(1, ".png")));
        genko::test::write_bytes(png + "/" + name(0, ".png"), "an export before");
        QString said;
        try {
            formats::export_print(doc, path_of(png), "png", 30);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 21] Is a directory: '%1'").arg(png + "/" + name(1, ".png")));
        QCOMPARE(QString::fromStdString(genko::test::read_bytes(png + "/" + name(0, ".png"))), QStringLiteral("an export before"));
        QStringList there{name(0, ".png"), name(1, ".png")};
        there.sort();
        QCOMPARE(QDir(png).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name), there);
    }

    // A place whose state cannot be read (its folder cannot be looked into) stops the export before any file is moved
    // — not even the files elsewhere, set aside and put back.
    void anUnreadablePlaceStopsBeforeAnyFileMoves() {
        const QString open = scratch_.path() + "/unreadable/open";
        const QString shut = scratch_.path() + "/unreadable/shut";
        QVERIFY(QDir().mkpath(open) && QDir().mkpath(shut));
        genko::test::write_bytes(open + "/a.png", "an export before");
        QString said;
        bool untouched = false;
        {
            formats::detail::Output output;
            output.put(path_of(open + "/a.png"), "new a");
            output.put(path_of(shut + "/b.png"), "new b");
            QThread::msleep(30);  // (the folder's time from the files written into it, before the commit)
            const fs::file_time_type stamp = fs::last_write_time(path_of(open));
            fs::permissions(path_of(shut), fs::perms::owner_read | fs::perms::owner_write);  // (its names cannot be looked up)
            try {
                output.commit();
            } catch (const genko::core::Error& e) {
                said = QString::fromStdString(e.code() + ": " + e.what());
            }
            untouched = fs::last_write_time(path_of(open)) == stamp;  // (before the files not moved are removed)
            fs::permissions(path_of(shut), fs::perms::owner_all);
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 13] Permission denied: '%1'").arg(shut + "/b.png"));
        QCOMPARE(QString::fromStdString(genko::test::read_bytes(open + "/a.png")), QStringLiteral("an export before"));
        QVERIFY2(untouched, "a file was moved there and back");
    }

    // The submission pack is one export: its PNGs, which come after its TIFFs, stopped (a folder of a PNG's name) — no
    // TIFF left either.
    void aPackStoppedPartWayLeavesNothing() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString name = QString::fromStdString(formats::stem(doc) + "_" + genko::core::file_stem(*doc.pages[0]) + ".png");
        const QString pack = scratch_.path() + "/in-the-way-pack";
        QVERIFY(QDir().mkpath(pack + "/" + name));
        QString said;
        try {
            formats::export_pack(doc, path_of(pack), "shueisha", 30);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 21] Is a directory: '%1'").arg(pack + "/" + name));
        QCOMPARE(QDir(pack).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{name});
    }

    // The resolutions an export takes: 1 to 100000 dpi, refused past them before anything is drawn or written (Python
    // takes any int, and fails somewhere — or writes pictures of one pixel), also by the dispatcher, whose dpi is a
    // 64-bit number (never cut to an int).
    void resolutions() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/resolutions";
        for (const std::string key : {"layers", "epub", "strip", "psd", "pack", "png"}) {
            for (const std::int64_t dpi : {std::int64_t{-5}, std::int64_t{100001}, std::int64_t{4294967446}}) {
                const QString dir = root + "/" + QString::fromStdString(key) + QString::number(dpi);
                formats::RunOptions o;
                o.dpi = dpi;
                Json reply;
                try {
                    reply = formats::run(doc, std::nullopt, key, path_of(dir), o);
                } catch (const std::exception& e) {
                    reply = Json{{"thrown", e.what()}};
                }
                QVERIFY2(reply == (Json{{"ok", false}, {"error", "the resolution must be between 1 and 100000 dpi"}}),
                         (key + " " + std::to_string(dpi) + ": " + reply.dump()).c_str());
                QVERIFY2(!QFileInfo::exists(dir), qPrintable(dir));
            }
        }
    }

    // A strip too large at its resolution (export_strip called with it: past what the dispatcher and `genko export`
    // take) is refused before its rows are made — not after holding memory for rows of 70 MB each.
    void aStripTooLargeIsRefusedBeforeItsRowsAreMade() {
        const QString root = scratch_.path() + "/strip-too-large";
        // (a book of three small pages: at ten million dpi each row of the strip would take 70 MB)
        const genko::core::Document small =
            genko::core::new_episode("s", genko::core::Num(1), 3, genko::core::PageSpec::custom(60, 60, 54, 54, 2, 5, 5, 4, 4, 600));
        const long long peak_before = peak_kb(true);
        QString said;
        try {
            formats::export_strip(small, path_of(root + "/s.png"), 10000000);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        const long long peak_after = peak_kb(false);
        if (peak_before >= 0 && peak_after >= 0) {
            QVERIFY2(peak_after - peak_before < 200 * 1024, qPrintable(QStringLiteral("%1 kB more held while refused").arg(peak_after - peak_before)));
        }
        QCOMPARE(said, QStringLiteral("image_too_large: the strip is too large at this resolution"));
        QVERIFY(!QFileInfo::exists(root + "/s.png"));
    }

    // A book whose pages are still being read (the app's first page first, SPEC PERF-01): nothing is written from it or
    // checked in it — the pages not read yet would come out empty.
    void aBookStillBeingReadIsNeitherWrittenNorChecked() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        genko::core::Document doc = loaded.document;
        doc.deferred.push_back(doc.pages.back());
        const QString dir = scratch_.path() + "/partial";
        const auto code_of = [](const std::function<void()>& make) {
            try {
                make();
            } catch (const genko::core::Error& e) {
                return std::string(e.code());
            }
            return std::string("done");
        };
        formats::RunOptions at30;
        at30.dpi = 30;
        QCOMPARE(code_of([&] { (void)formats::run(doc, std::nullopt, "png", path_of(dir), at30); }), std::string("page_not_loaded"));
        QVERIFY(!QFileInfo::exists(dir));
        QCOMPARE(code_of([&] { (void)formats::checks::book(doc); }), std::string("page_not_loaded"));
    }

    // Every job: the same result (or error), and the same files.
    void exports() {
        int compared = 0;
        int files = 0;
        for (const Json& job : jobs_) {
            const std::string id = job["id"].get<std::string>();
            const Json got = call(job);
            const Json& want = results_[id];
            if (want.contains("error")) {
                QVERIFY2(got.contains("error") && same_error(got["error"], want["error"]),
                         (id + ": " + got.dump() + " / Python: " + want.dump()).c_str());
            } else {
                std::string where;
                QVERIFY2(!got.contains("error"), (id + ": " + got.dump()).c_str());
                QVERIFY2(genko::test::strict_equal(got["result"], want["result"], &where),
                         (id + ": " + where + "\n" + got["result"].dump().substr(0, 2000) + "\n/ Python: " + want["result"].dump().substr(0, 2000)).c_str());
            }
            ++compared;
        }
        // the files: those Python wrote, each the same
        const QString difference = genko::test::exports::tree_difference(out_, out_py_);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        files = static_cast<int>(genko::test::exports::tree(out_py_).size());
        const int identical = genko::test::exports::same_bytes(out_, out_py_);
        qInfo("exports: %d calls, %d files the same (%d byte for byte; the others only their times differ)", compared, files, identical);
        QVERIFY(compared >= 150);
        QVERIFY(files >= 400);
    }
};

QTEST_GUILESS_MAIN(TestContractExport)
#include "test_contract_export.moc"
