// BRUSH-01 紙質 (this build only: SPEC.md BRUSH-01, ACCEPTANCE.md AC-BRUSH, schema-v4.md paper-texture@1): a paper
// picture taken in as a grey asset of the book, the brush's paper settings, define_brush with a paper, the arithmetic
// against the independent reference (data/paper/samples.json: from a pixel's mm position to the paper's sample and the
// ink left, every setting), the pictures Pillow makes of the test files laid over white, the refusals (broken, huge,
// unread formats), the full-size reference pages (every setting, crossing lines; the inherited brushes without a
// paper), the same pixels after saving, on another computer (the folder copied, the source picture gone, the process
// knowing nothing), after Undo and Redo, at another resolution (the paper stays the page's size), in print (the PDF
// back to pixels with poppler's pdftocairo at the print's resolution) and through the command line.
//
// The expected pictures (data/paper/expected/) are this build's own (BRUSH-01 is new: no Python output to match),
// approved after a look. GENKO_PAPER_WRITE_EXPECTED=1 writes them again from this build (then MANIFEST.json's hashes).

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "core/strokes.hpp"
#include "formats/export.hpp"
#include "render/brushes.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/paper.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace paper = genko::render::paper;
namespace brushes = genko::render::brushes;

namespace {

constexpr int kDpi = 300;

QString data_path(const QString& name) { return genko::test::test_data(QStringLiteral("paper/") + name); }
std::string data_bytes(const QString& name) { return genko::test::read_bytes(data_path(name)); }

std::uint64_t bits(double x) { return std::bit_cast<std::uint64_t>(x); }

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

// The error code and message an op batch is refused with ("" when it is not).
std::string refusal(const core::Document& doc, const Json& ops, const core::OpRegistry& registry = render::ops_registry()) {
    try {
        core::CommandBus(registry).apply(doc, ops, core::Actor("human:test"));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

std::string take_in_code(std::string_view bytes) {
    try {
        paper::take_in(bytes);
    } catch (const core::Error& error) {
        return error.code();
    }
    return {};
}

core::Paper fixture_paper(const Json& j) {
    core::Paper p;
    p.asset = j["asset"].get<std::string>();
    p.density = j["density"].get<double>();
    p.scale = j["scale"].get<double>();
    p.rotation = j["rotation"].get<double>();
    p.flip_x = j["flip_x"].get<bool>();
    p.flip_y = j["flip_y"].get<bool>();
    p.invert = j["invert"].get<bool>();
    p.blend = j["blend"].get<std::string>();
    p.coords = j["coords"].get<std::string>();
    p.seam = j["seam"].get<std::string>();
    p.seed = j["seed"].get<std::int64_t>();
    return p;
}

core::Stroke line(std::string id, std::string kind, double width, std::vector<std::array<double, 3>> points) {
    core::Stroke s;
    s.id = std::move(id);
    s.kind = std::move(kind);
    s.width_mm = width;
    for (const auto& [x, y, p] : points) {
        s.points.push_back(core::PointF{x, y});
        s.pressure.push_back(p);
    }
    return s;
}

// A wave of n points from (x0, y) to (x1, y), pressed from `p0` to `p1`.
std::vector<std::array<double, 3>> wave(double x0, double x1, double y, double amplitude, double p0, double p1, int n = 24) {
    std::vector<std::array<double, 3>> out;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        out.push_back({x0 + (x1 - x0) * t, y + amplitude * std::sin(t * 6.283185307179586), p0 + (p1 - p0) * t});
    }
    return out;
}

core::Layer& ink_layer(core::Document& doc, std::size_t page) {
    for (core::Layer& layer : doc.edit_page(page).layers) {
        if (layer.role == core::LayerRole::Ink) return layer;
    }
    throw std::runtime_error("no ink layer");
}

const core::Layer& ink_layer(const core::Document& doc, std::size_t page) {
    for (const core::Layer& layer : doc.page(page).layers) {
        if (layer.role == core::LayerRole::Ink) return layer;
    }
    throw std::runtime_error("no ink layer");
}

Json paper_of(Json settings, const std::string& picture_b64) {
    settings["png"] = picture_b64;
    return settings;
}

// The reference book: page 1 every paper setting (crossing lines, pressure, opacity, a plain line beside its paper
// twin), page 2 every inherited brush without a paper. 100 × 80 mm at 300 dpi, no panels (nothing clipped).
core::Document reference_book() {
    core::Document doc = core::new_episode("紙質", core::Num(1), 2, core::PageSpec::custom(100, 80, 90, 70, 2, 4, 4, 4, 4, kDpi, "mono"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).frames.clear();
    const std::string picture = core::b64encode(data_bytes(QStringLiteral("grain.png")));
    Json ops = Json::array();
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_a"}, {"label", "紙 A"}, {"base", "fill_pen"}, {"width_mm", 4.0},
                       {"paper", paper_of(Json{{"density", 1.0}}, picture)}});
    doc = run_ops(doc, ops).doc;
    const std::string asset = doc.brush_custom["my_paper_a"]["paper"]["asset"].get<std::string>();
    ops = Json::array();
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_b"}, {"label", "紙 B"}, {"base", "fill_pen"}, {"width_mm", 5.0},
                       {"paper", Json{{"asset", asset}, {"density", 0.7}, {"scale", 2.5}, {"rotation", 30.0}, {"flip_x", true},
                                      {"seam", "mirror"}, {"seed", 12345}, {"blend", "subtract"}}}});
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_c"}, {"label", "紙 C"}, {"base", "gpen"}, {"width_mm", 2.0},
                       {"paper", Json{{"asset", asset}, {"density", 0.8}, {"scale", 0.5}, {"rotation", -90.0}, {"flip_y", true},
                                      {"invert", true}, {"coords", "stroke"}, {"seed", 7}}}});
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_d"}, {"label", "紙 D"}, {"base", "pencil"}, {"width_mm", 2.5},
                       {"paper", Json{{"asset", asset}, {"density", 0.4}, {"scale", 3.0}, {"rotation", 45.0}, {"flip_x", true},
                                      {"flip_y", true}, {"coords", "stroke"}, {"seam", "mirror"}, {"blend", "subtract"},
                                      {"seed", core::kPaperMaxSeed}}}});
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_e"}, {"label", "紙 E"}, {"base", "airbrush"}, {"width_mm", 6.0},
                       {"paper", Json{{"asset", asset}, {"density", 0.6}, {"scale", 1.5}}}});
    ops.push_back(Json{{"op", "define_brush"}, {"key", "my_paper_f"}, {"label", "紙 F"}, {"base", "stipple"}, {"width_mm", 4.0},
                       {"paper", Json{{"asset", asset}, {"density", 1.0}, {"blend", "subtract"}, {"seed", 99}}}});
    doc = run_ops(doc, ops).doc;
    std::vector<core::StrokePtr> items;
    const auto add = [&](core::Stroke s) { items.push_back(std::make_shared<const core::Stroke>(std::move(s))); };
    add(line("paper-a-1", "my_paper_a", 4.0, wave(8, 92, 12, 3, 1.0, 1.0)));
    add(line("paper-a-2", "my_paper_a", 4.0, {{20, 4, 1.0}, {50, 22, 1.0}, {80, 4, 1.0}}));  // (crosses the first)
    add(line("plain-a", "fill_pen", 4.0, wave(8, 92, 27, 2, 1.0, 1.0)));                  // (the same without a paper)
    add(line("paper-b-1", "my_paper_b", 5.0, wave(8, 92, 38, 3, 0.05, 1.0)));
    core::Stroke c1 = line("paper-c-1", "my_paper_c", 2.0, wave(8, 60, 50, 4, 0.2, 1.0));
    c1.opacity = 0.6;
    c1.pressure_opacity = 0.5;
    add(c1);
    add(line("paper-c-2", "my_paper_c", 2.0, {{30, 44, 0.9}, {40, 56, 0.9}, {52, 44, 0.9}}));  // (each line its own start)
    core::Stroke d1 = line("paper-d-1", "my_paper_d", 2.5, wave(64, 92, 50, 4, 0.3, 1.0, 16));
    d1.rgb = std::vector<std::int64_t>{40, 70, 160};
    add(d1);
    add(line("paper-e-1", "my_paper_e", 6.0, wave(8, 50, 64, 3, 0.6, 1.0)));
    core::Stroke f1 = line("paper-f-1", "my_paper_f", 4.0, wave(56, 92, 66, 3, 1.0, 0.6));
    f1.rgb = std::vector<std::int64_t>{170, 40, 40};
    add(f1);
    ink_layer(doc, 0).strokes = core::make_strokes(std::move(items));
    // page 2: every inherited brush, none with a paper
    std::vector<core::StrokePtr> inherited;
    int n = 0;
    for (const core::Brush& b : core::builtin_brushes()) {
        const double x = 6 + (n % 3) * 30.0;
        const double y = 7 + (n / 3) * 10.0;
        const double width = std::min(b.width_mm, 3.0);
        inherited.push_back(std::make_shared<const core::Stroke>(
            line("inherited-" + b.key, b.key, width, wave(x, x + 24, y, 2, 0.2, 1.0, 16))));
        ++n;
    }
    ink_layer(doc, 1).strokes = core::make_strokes(std::move(inherited));
    return doc;
}

render::Image ink_picture(const core::Document& doc, std::size_t page, int dpi = kDpi) {
    brushes::register_book(doc);
    return render::layer_image(doc.page(page), ink_layer(doc, page), dpi, &doc);
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

void save_next(const core::Document& doc, const fs::path& dir, const Json& ops) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = storage::read_disk_state(dir).revision;
    request.ops = ops;
    storage::Saver(lock).save(doc, request);
}

bool same(const render::Image& a, const render::Image& b, const QString& name) {
    if (a.mode() == b.mode() && a.size() == b.size() && a.tobytes() == b.tobytes()) return true;
    if (a.mode() == b.mode() && a.size() == b.size()) {
        const auto diff = genko::test::pixel_diff(a, b);
        qWarning("%s: %lld pixels differ (largest %d)", qPrintable(name), diff.pixels, diff.largest);
    } else {
        qWarning("%s: %dx%d %s against %dx%d %s", qPrintable(name), a.width(), a.height(), std::string(a.mode()).c_str(), b.width(),
                 b.height(), std::string(b.mode()).c_str());
    }
    genko::test::keep_pictures(name, a, b);
    return false;
}

// The picture against data/paper/expected/<name>.png (GENKO_PAPER_WRITE_EXPECTED=1: written there first).
bool as_expected(const render::Image& got, const QString& name) {
    const QString file = data_path(QStringLiteral("expected/") + name + QStringLiteral(".png"));
    if (qEnvironmentVariableIsSet("GENKO_PAPER_WRITE_EXPECTED")) {
        QDir().mkpath(QFileInfo(file).path());
        render::save_png(got, core::path_from_utf8(file.toStdString()), 9);
    }
    if (!QFile::exists(file)) {
        qWarning("no expected picture %s", qPrintable(file));
        return false;
    }
    return same(got, render::read_png(genko::test::read_bytes(file)), name);
}

// How a picture and the PDF's page drawn again differ, at the best of the nine shifts of at most one pixel: the shift
// and the largest difference of a channel over the pixels both have.
struct Fit {
    int dx = 0, dy = 0, largest = 256;
};

Fit best_fit(const render::Image& a, const render::Image& b) {
    const std::string x = a.tobytes();
    const std::string y = b.tobytes();
    const int bands = a.bands();
    Fit best;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int largest = 0;
            for (int row = std::max(0, -dy); row < a.height() && row + dy < b.height(); ++row) {
                for (int col = std::max(0, -dx); col < a.width() && col + dx < b.width(); ++col) {
                    for (int c = 0; c < bands; ++c) {
                        const int va = static_cast<unsigned char>(x[(static_cast<std::size_t>(row) * a.width() + col) * bands + c]);
                        const int vb = static_cast<unsigned char>(y[(static_cast<std::size_t>(row + dy) * b.width() + col + dx) * bands + c]);
                        largest = std::max(largest, std::abs(va - vb));
                    }
                }
            }
            if (largest < best.largest) best = Fit{dx, dy, largest};
        }
    }
    return best;
}

}  // namespace

class TestPaper : public QObject {
    Q_OBJECT

private slots:
    void init() {
        brushes::clear_custom();
        paper::clear();
    }

    // The fixed test files are the ones listed, with their hashes (FIXTURES.md: MANIFEST.json).
    void manifest() {
        const Json manifest = genko::test::read_json(data_path(QStringLiteral("MANIFEST.json")));
        QStringList listed;
        for (const auto& [name, hash] : manifest["files"].items()) {
            const QByteArray bytes = QByteArray::fromStdString(data_bytes(QString::fromStdString(name)));
            QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString(), hash.get<std::string>());
            listed << QString::fromStdString(name);
        }
        QStringList found;
        QDirIterator it(data_path({}), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = QDir(data_path({})).relativeFilePath(it.next());
            if (file != QLatin1String("MANIFEST.json")) found << file;
        }
        listed.sort();
        found.sort();
        QCOMPARE(found, listed);
    }

    // sin and cos of the rotation: the reference's bits, every quarter turn exact.
    void sinCos() {
        const Json data = genko::test::read_json(data_path(QStringLiteral("samples.json")));
        for (const Json& row : data["sin_cos"]) {
            const auto [s, c] = paper::sin_cos_degrees(row[0].get<double>());
            QVERIFY2(bits(s) == bits(row[1].get<double>()) && bits(c) == bits(row[2].get<double>()),
                     qPrintable(QStringLiteral("%1°: %2, %3").arg(row[0].get<double>()).arg(s, 0, 'g', 17).arg(c, 0, 'g', 17)));
        }
    }

    // Every case of the analysable fixture: the start (seed), each pixel's mm position, its place in the picture, the
    // four pixels mixed, the ink left for seven coverages; a coverage picture laid on the paper.
    void samples() {
        const Json data = genko::test::read_json(data_path(QStringLiteral("samples.json")));
        paper::Grain grain;
        grain.width = data["grain"]["width"].get<int>();
        grain.height = data["grain"]["height"].get<int>();
        for (const Json& v : data["grain"]["values"]) grain.values.push_back(static_cast<std::uint8_t>(v.get<int>()));
        int checked = 0;
        for (const Json& c : data["cases"]) {
            const core::Paper p = fixture_paper(c["paper"]);
            const auto offset = paper::offsets(p, c["line_seed"].get<std::string>(), grain.width, grain.height);
            QCOMPARE(offset.first, c["offset"][0].get<std::int64_t>());
            QCOMPARE(offset.second, c["offset"][1].get<std::int64_t>());
            const double ax = c["anchor"][0].get<double>();
            const double ay = c["anchor"][1].get<double>();
            for (const Json& px : c["pixels"]) {
                const auto s = paper::sample(grain, p, c["dpi"].get<int>(), px["x"].get<std::int64_t>(), px["y"].get<std::int64_t>(), ax, ay, offset);
                const QString where = QStringLiteral("case %1 pixel (%2, %3)").arg(checked).arg(px["x"].get<int>()).arg(px["y"].get<int>());
                QVERIFY2(bits(s.x_mm) == bits(px["x_mm"].get<double>()) && bits(s.y_mm) == bits(px["y_mm"].get<double>()), qPrintable(where));
                QVERIFY2(bits(s.u) == bits(px["u"].get<double>()) && bits(s.v) == bits(px["v"].get<double>()), qPrintable(where));
                QVERIFY2(s.U == px["U"].get<std::int64_t>() && s.V == px["V"].get<std::int64_t>(), qPrintable(where));
                QVERIFY2(s.value == px["value"].get<int>(), qPrintable(where + QStringLiteral(": %1, not %2").arg(s.value).arg(px["value"].get<int>())));
                for (const Json& pair : px["ink"]) QCOMPARE(paper::blend(pair[0].get<int>(), s.value, p), pair[1].get<int>());
            }
            // apply: the same through a coverage picture (the picture known by its ref)
            const Json& laid = c["apply"];
            std::string mask_bytes;
            for (const Json& v : laid["mask"]) mask_bytes.push_back(static_cast<char>(v.get<int>()));
            const render::Size size{laid["size"][0].get<int>(), laid["size"][1].get<int>()};
            render::Image mask = render::Image::frombytes("L", size, mask_bytes);
            const std::string grey = render::write_png(render::Image::frombytes(
                "L", render::Size{grain.width, grain.height}, std::string(grain.values.begin(), grain.values.end())));
            core::Paper known = p;
            known.asset = storage::AssetStore::ref(grey);
            paper::keep(known.asset, std::make_shared<const std::string>(grey));
            paper::apply(mask, render::Point{laid["origin"][0].get<int>(), laid["origin"][1].get<int>()}, c["dpi"].get<int>(), known,
                         c["line_seed"].get<std::string>(), ax, ay);
            std::string want;
            for (const Json& v : laid["result"]) want.push_back(static_cast<char>(v.get<int>()));
            QCOMPARE(mask.tobytes(), want);
            ++checked;
        }
        QCOMPARE(checked, 7);
    }

    // A picture taken in: laid over white and made grey exactly as Pillow does it (the test files' greys are Pillow's),
    // kept as a grey PNG; transparent parts are white paper.
    void takeIn() {
        const Json expected = genko::test::read_json(data_path(QStringLiteral("grain-grey.json")));
        for (const auto& [name, want] : expected.items()) {
            const std::string png = paper::take_in(data_bytes(QString::fromStdString(name)));
            const render::Image kept = render::read_png(png);
            QCOMPARE(std::string(kept.mode()), std::string("L"));
            QCOMPARE(kept.width(), want["size"][0].get<int>());
            QCOMPARE(kept.height(), want["size"][1].get<int>());
            QVERIFY2(kept.tobytes() == core::a2b_base64(want["grey"].get<std::string>()), name.c_str());
            const paper::Grain grain = paper::grain_of(png);
            QCOMPARE(grain.width, kept.width());
            QCOMPARE(std::string(grain.values.begin(), grain.values.end()), kept.tobytes());
        }
        // alpha: transparent is white (255), half transparent black is half grey
        render::Image rgba = render::Image::create("RGBA", render::Size{3, 1}, render::Ink::tuple({0, 0, 0, 0}));
        rgba.paste(render::Ink::tuple({0, 0, 0, 128}), render::Box{1, 0, 2, 1});
        rgba.paste(render::Ink::tuple({0, 0, 0, 255}), render::Box{2, 0, 3, 1});
        const paper::Grain g = paper::grain_of(paper::take_in(render::write_png(rgba)));
        QCOMPARE(int(g.values[0]), 255);
        QCOMPARE(int(g.values[1]), 127);
        QCOMPARE(int(g.values[2]), 0);
    }

    // What is refused, and why (core::Error codes; define_brush words them, the app in Japanese).
    void takeInRefusals() {
        QCOMPARE(take_in_code(""), std::string("paper_unreadable"));
        QCOMPARE(take_in_code("not a picture at all"), std::string("paper_unreadable"));
        const std::string png = data_bytes(QStringLiteral("grain.png"));
        QCOMPARE(take_in_code(png.substr(0, png.size() / 2)), std::string("paper_unreadable"));  // (cut short)
        const std::string jpg = data_bytes(QStringLiteral("grain.jpg"));
        QCOMPARE(take_in_code(jpg.substr(0, jpg.size() / 2)), std::string("paper_unreadable"));
        QCOMPARE(take_in_code(std::string("II*\0", 4) + std::string(64, '\0')), std::string("paper_format"));     // TIFF
        QCOMPARE(take_in_code(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16) + std::string(32, '\0')), std::string("paper_format"));  // WebP
        // a side over 4096 pixels: refused from the header, nothing decoded (4097 × 1, and 1 × 100000)
        for (const auto& size : {render::Size{4097, 1}, render::Size{1, 4097}}) {
            const std::string wide = render::write_png(render::Image::create("L", size, render::Ink(200)));
            QCOMPARE(take_in_code(wide), std::string("paper_too_large"));
        }
        std::string huge_header = render::write_png(render::Image::create("L", render::Size{1, 1}, render::Ink(200)));
        huge_header[16] = 0, huge_header[17] = 1, huge_header[18] = static_cast<char>(0x86), huge_header[19] = static_cast<char>(0xa0);  // (width 100000; its CRC now wrong)
        QVERIFY(take_in_code(huge_header) == "paper_too_large" || take_in_code(huge_header) == "paper_unreadable");
        // the largest that is taken: 4096 on a side
        QCOMPARE(take_in_code(render::write_png(render::Image::create("L", render::Size{4096, 2}, render::Ink(9)))), std::string());
        // a file over 64 MiB is not even read as a picture
        QCOMPARE(take_in_code(std::string(paper::kMaxFileBytes + 1, '\x89')), std::string("paper_file_too_large"));
    }

    // The settings a brush keeps: defaults, their order, what is refused; a brush without a paper is written as before.
    void settings() {
        const std::string ref = "sha256:" + std::string(64, 'a');
        const core::Paper p = core::paper_from_json(Json{{"asset", ref}});
        QCOMPARE(p.density, 0.5);
        QCOMPARE(p.scale, 1.0);
        QCOMPARE(p.blend, std::string("multiply"));
        QCOMPARE(p.coords, std::string("paper"));
        QCOMPARE(p.seam, std::string("repeat"));
        QCOMPARE(p.seed, std::int64_t(0));
        const Json written = core::paper_to_json(p);
        std::vector<std::string> keys;
        for (const auto& [key, value] : written.items()) keys.push_back(key);
        QCOMPARE(keys, (std::vector<std::string>{"asset", "density", "scale", "rotation", "flip_x", "flip_y", "invert", "blend", "coords", "seam", "seed"}));
        QVERIFY(core::paper_from_json(written) == p);
        Json full = written;
        full["density"] = 1;  // (an int is a number)
        full["rotation"] = -360;
        full["seed"] = core::kPaperMaxSeed;
        QCOMPARE(core::paper_from_json(full).density, 1.0);
        for (const Json& bad : {Json(nullptr), Json("paper"), Json::array(), Json{{"density", 0.5}}, Json{{"asset", "sha256:12"}},
                                Json{{"asset", ref}, {"density", 1.5}}, Json{{"asset", ref}, {"density", true}},
                                Json{{"asset", ref}, {"density", "0.5"}}, Json{{"asset", ref}, {"scale", 0.05}},
                                Json{{"asset", ref}, {"scale", 10.5}}, Json{{"asset", ref}, {"rotation", 360.5}},
                                Json{{"asset", ref}, {"rotation", std::numeric_limits<double>::infinity()}},
                                Json{{"asset", ref}, {"flip_x", 1}}, Json{{"asset", ref}, {"invert", "yes"}},
                                Json{{"asset", ref}, {"blend", "screen"}}, Json{{"asset", ref}, {"coords", "view"}},
                                Json{{"asset", ref}, {"seam", "clamp"}}, Json{{"asset", ref}, {"seed", -1}},
                                Json{{"asset", ref}, {"seed", core::kPaperMaxSeed + 1}}, Json{{"asset", ref}, {"seed", 1.5}},
                                Json{{"asset", ref}, {"png", "AAAA"}}, Json{{"asset", ref}, {"colour", 3}}}) {
            QVERIFY_THROWS_EXCEPTION(core::PyValueError, core::paper_from_json(bad));
        }
        // the brushes Python knows: no paper, the same settings as before
        for (const core::Brush& b : core::builtin_brushes()) QVERIFY(!core::brush_to_dict(b).contains("paper"));
        std::vector<core::Brush> known;
        const core::Brush plain = core::brush_from_dict("my_plain", Json{{"label", "x"}, {"texture", "grain"}}, std::nullopt, known);
        QVERIFY(!plain.paper);
        QVERIFY(!core::brush_to_dict(plain).contains("paper"));
        // a paper kept, inherited from the base, taken away with null
        const core::Brush with = core::brush_from_dict("my_with", Json{{"label", "x"}, {"paper", written}}, std::nullopt, known);
        QVERIFY(with.paper == std::optional<core::Paper>(p));
        QCOMPARE(core::brush_to_dict(with)["paper"], written);
        QCOMPARE(core::brush_to_dict(with).items().begin().key(), std::string("label"));
        known.push_back(with);
        QVERIFY(core::brush_from_dict("my_child", Json{{"label", "y"}, {"base", "my_with"}}, std::nullopt, known).paper == with.paper);
        QVERIFY(!core::brush_from_dict("my_child", Json{{"label", "y"}, {"base", "my_with"}, {"paper", nullptr}}, std::nullopt, known).paper);
        // a book's papers, in order, each once
        const Json custom = Json{{"my_a", core::brush_to_dict(with)}, {"my_b", Json{{"label", "z"}}}, {"my_c", core::brush_to_dict(with)}};
        QCOMPARE(core::paper_refs(custom), std::vector<std::string>{ref});
    }

    // define_brush with a paper: the picture taken in (the book keeps it by its ref), the feature declared, the journal
    // without the picture's text, the same picture by its asset; what is refused leaves the book as it was.
    void defineBrush() {
        const core::Document doc = core::new_episode("op", core::Num(1), 1, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 150, "mono"));
        const std::string picture = core::b64encode(data_bytes(QStringLiteral("grain.png")));
        // a brush without a paper: the book as before (no feature, Python's settings)
        const auto plain = run_ops(doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_plain"}, {"label", "普通"}}}));
        QVERIFY(plain.doc.features.empty());
        QVERIFY(plain.doc.papers.empty());
        QVERIFY(!plain.doc.brush_custom["my_plain"].contains("paper"));
        // with one
        const auto made = run_ops(doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_paper"}, {"label", "紙"},
                                                       {"paper", Json{{"png", picture}, {"density", 0.75}, {"blend", "subtract"}}}}}));
        const Json& kept = made.doc.brush_custom["my_paper"]["paper"];
        const std::string ref = kept["asset"].get<std::string>();
        QVERIFY(storage::AssetStore::is_ref(ref));
        QVERIFY(!kept.contains("png"));
        QCOMPARE(kept["density"], Json(0.75));
        QCOMPARE(kept["blend"], Json("subtract"));
        QCOMPARE(made.doc.papers.size(), std::size_t(1));
        QCOMPARE(storage::AssetStore::ref(*made.doc.papers.at(ref)), ref);
        QCOMPARE(*made.doc.papers.at(ref), paper::take_in(data_bytes(QStringLiteral("grain.png"))));
        QCOMPARE(made.doc.features, std::vector<std::string>{std::string(core::kPaperFeature)});
        for (const Json& warning : made.warnings) QVERIFY2(warning.dump().find("paper") == std::string::npos, warning.dump().c_str());  // (this build's key: not ignored)
        const std::string journal = made.journal_ops.dump();
        QVERIFY(journal.find(picture.substr(0, 64)) == std::string::npos);
        QCOMPARE(made.journal_ops[0]["paper"]["png"], Json("<" + std::to_string(picture.size()) + " base64 chars>"));
        // the same picture again by its asset, and the brush drawn with it
        const auto again = run_ops(made.doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_paper2"}, {"label", "紙2"},
                                                             {"paper", Json{{"asset", ref}, {"rotation", 30}}}}}));
        QCOMPARE(again.doc.papers.size(), std::size_t(1));
        QCOMPARE(again.doc.brush_custom["my_paper2"]["paper"]["rotation"], Json(30.0));
        QCOMPARE(again.doc.features, std::vector<std::string>{std::string(core::kPaperFeature)});
        // taken away (null), deleted: the picture stays with the book until it is saved (only the kept are written)
        const auto bare = run_ops(again.doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_paper2"}, {"label", "紙2"}, {"paper", nullptr}}}));
        QVERIFY(!bare.doc.brush_custom["my_paper2"].contains("paper"));
        // refused, the book as it was
        const auto refused = [&](const Json& paper_value, const std::string& words) {
            const std::string why = refusal(made.doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_bad"}, {"label", "x"}, {"paper", paper_value}}}));
            if (why.find(words) == std::string::npos) qWarning("%s", why.c_str());
            return why.find(words) != std::string::npos;
        };
        QVERIFY(refused(Json{{"asset", "sha256:" + std::string(64, 'b')}}, "the book has no paper picture sha256:bbbb"));
        QVERIFY(refused(Json{{"png", core::b64encode("not a picture")}}, "the paper's picture cannot be read"));
        QVERIFY(refused(Json{{"png", "@@@"}}, "the paper's picture cannot be read"));
        QVERIFY(refused(Json{{"png", picture}, {"asset", ref}}, "a paper takes its picture once: png or asset"));
        QVERIFY(refused(Json::object(), "a paper needs its picture: png (base64) or asset"));
        QVERIFY(refused(Json{{"png", picture}, {"density", 2}}, "paper density must be between 0 and 1"));
        QVERIFY(refused(Json{{"png", picture}, {"blend", "screen"}}, "paper blend must be multiply or subtract"));
        QVERIFY(refused(Json{{"png", core::b64encode(std::string("II*\0", 4) + std::string(64, '\0'))}},
                        "this build does not read that kind of picture"));
        QVERIFY(refused(Json{{"png", core::b64encode(render::write_png(render::Image::create("L", render::Size{4097, 1}, render::Ink(1))))}},
                        "the paper's picture is too large (at most 4096 pixels a side)"));
        QVERIFY(refused(Json("paper"), "paper must be an object or null"));
        // core's own registry cannot take a picture in (not_yet_ported), but takes the asset of a paper the book keeps
        QVERIFY(refusal(doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_x"}, {"label", "x"}, {"paper", Json{{"png", picture}}}}}),
                        core::OpRegistry::builtin()).starts_with("not_yet_ported: "));
        const auto by_core = core::CommandBus().apply(made.doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_x"}, {"label", "x"},
                                                                                    {"paper", Json{{"asset", ref}}}}}),
                                                      core::Actor("human:test"));
        QCOMPARE(by_core.doc.brush_custom["my_x"]["paper"]["asset"], Json(ref));
    }

    // A line drawn with a paper: the brush's own coverage, then each pixel through the paper (render/paper.hpp); the
    // same box. Lines crossing on paper fixed to the page meet the same grain; with coords "stroke" each line's own.
    void drawnWithPaper() {
        const core::Document doc = reference_book();
        brushes::register_book(doc);
        const render::Size size{render::mm_to_px(100, kDpi), render::mm_to_px(80, kDpi)};
        const core::PenPoints points = core::stroke_points(*ink_layer(doc, 0).strokes->items[0]);
        const auto plain = brushes::draw(size, points, kDpi, 4.0, "fill_pen", "paper-a-1");
        const auto laid = brushes::draw(size, points, kDpi, 4.0, "my_paper_a", "paper-a-1");
        QVERIFY(plain && laid);
        QVERIFY(laid->origin == plain->origin);
        QVERIFY(laid->mask.size() == plain->mask.size());
        QVERIFY(brushes::extent(size, points, kDpi, 4.0, "my_paper_a") == brushes::extent(size, points, kDpi, 4.0, "fill_pen"));
        QVERIFY(laid->mask.tobytes() != plain->mask.tobytes());
        const core::Paper p = *brushes::brush("my_paper_a").paper;
        const auto grain = paper::grain(p.asset);
        const auto start = paper::offsets(p, "paper-a-1", grain->width, grain->height);
        const std::string a = plain->mask.tobytes();
        const std::string b = laid->mask.tobytes();
        for (int y = 0; y < plain->mask.height(); y += 7) {
            for (int x = 0; x < plain->mask.width(); x += 5) {
                const std::size_t at = static_cast<std::size_t>(y) * plain->mask.width() + x;
                const auto s = paper::sample(*grain, p, kDpi, plain->origin.x + x, plain->origin.y + y, 0, 0, start);
                QCOMPARE(static_cast<int>(static_cast<unsigned char>(b[at])), paper::blend(static_cast<unsigned char>(a[at]), s.value, p));
            }
        }
        // on the page: the same paper whichever line (and wherever it starts); per line with coords "stroke"
        const core::Paper c = *brushes::brush("my_paper_c").paper;
        const auto at_a = paper::offsets(p, "one", grain->width, grain->height);
        QVERIFY(at_a == paper::offsets(p, "another", grain->width, grain->height));
        QCOMPARE(paper::sample(*grain, p, kDpi, 500, 300, 0, 0, at_a).value, paper::sample(*grain, p, kDpi, 500, 300, 12, 34, at_a).value);
        QVERIFY(paper::offsets(c, "one", grain->width, grain->height) != paper::offsets(c, "another", grain->width, grain->height) ||
                paper::offsets(c, "one", grain->width, grain->height) != paper::offsets(c, "third", grain->width, grain->height));
    }

    // The paper is the page's: the same mm under a pixel at 300 dpi and at 900 dpi (whose pixel 3X + 1 has the same
    // centre) samples the same grain (the screen's resolutions draw it the same size). The two centres are computed by
    // different divisions, so where a centre falls exactly on a 1/256 step of the picture (at scale 1 every pixel of
    // 300 dpi does) the last bit can put it one step either side: within one level of grey, never more.
    void fixedToThePage() {
        const core::Document doc = reference_book();
        brushes::register_book(doc);
        for (const char* key : {"my_paper_a", "my_paper_b", "my_paper_d"}) {
            const core::Paper p = *brushes::brush(key).paper;
            const auto grain = paper::grain(p.asset);
            const auto start = paper::offsets(p, "x", grain->width, grain->height);
            int same_value = 0, total = 0, largest = 0;
            for (int y = 0; y < 900; y += 13) {
                for (int x = 0; x < 1100; x += 11) {
                    const int a = paper::sample(*grain, p, 300, x, y, 1, 2, start).value;
                    const int b = paper::sample(*grain, p, 900, 3 * x + 1, 3 * y + 1, 1, 2, start).value;
                    same_value += a == b;
                    largest = std::max(largest, std::abs(a - b));
                    ++total;
                }
            }
            QVERIFY2(same_value * 10 >= total * 9 && largest <= 1, qPrintable(QStringLiteral("%1: %2 of %3 the same, largest %4")
                                                                                        .arg(key).arg(same_value).arg(total).arg(largest)));
        }
    }

    // The full-size reference pages (8-bit RGBA, the ink layer at the book's 300 dpi): every paper setting, and every
    // inherited brush without one.
    void referencePages() {
        const core::Document doc = reference_book();
        QVERIFY(as_expected(ink_picture(doc, 0), QStringLiteral("paper-settings")));
        QVERIFY(as_expected(ink_picture(doc, 1), QStringLiteral("inherited-brushes")));
        // the paper changes the lines it is under, and only those
        core::Document bare = doc;
        Json custom = bare.brush_custom;
        for (auto& [key, value] : custom.items()) value.erase("paper");
        bare.brush_custom = custom;
        brushes::clear_custom();
        const render::Image without = ink_picture(bare, 0);
        brushes::clear_custom();
        QVERIFY(without.tobytes() != ink_picture(doc, 0).tobytes());
    }

    // Saved, the folder taken to another computer (copied; the source picture file deleted; this process knowing
    // neither the brushes nor the pictures): the book reads clean with its feature, and draws the same pixels.
    void savedAndElsewhere() {
        QTemporaryDir tmp;
        const QString source = tmp.path() + QStringLiteral("/紙の画像.png");
        genko::test::write_bytes(source, data_bytes(QStringLiteral("grain.png")));
        core::Document doc = reference_book();
        const core::Document by_file = run_ops(doc, Json::array({Json{{"op", "define_brush"}, {"key", "my_from_file"}, {"label", "ファイル"},
                                                                    {"paper", Json{{"png", core::b64encode(genko::test::read_bytes(source))}}}}})).doc;
        QVERIFY(QFile::remove(source));
        const render::Image page1 = ink_picture(by_file, 0);
        const render::RenderResult whole = render::render_page(by_file.page(0), kDpi, {}, &by_file);
        const fs::path dir = core::path_from_utf8((tmp.path() + QStringLiteral("/紙質.genko")).toStdString());
        write_book(by_file, dir);
        const Json project = genko::test::read_json(QString::fromStdString(core::path_to_utf8(dir / "project.json")));
        QCOMPARE(project["features"], Json::array({std::string(core::kPaperFeature)}));
        const std::string ref = project["brush"]["custom"]["my_paper_a"]["paper"]["asset"].get<std::string>();
        QVERIFY(fs::exists(dir / storage::AssetStore::relpath(ref, ".png")));
        // elsewhere
        const QString other = tmp.path() + QStringLiteral("/別の PC/紙質.genko");
        genko::test::copy_tree(QString::fromStdString(core::path_to_utf8(dir)), other);
        brushes::clear_custom();
        paper::clear();
        const auto loaded = storage::load_document(core::path_from_utf8(other.toStdString()));
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        QVERIFY(loaded.document.read_only_reason.empty());
        QCOMPARE(loaded.document.features, std::vector<std::string>{std::string(core::kPaperFeature)});
        QCOMPARE(loaded.document.papers.size(), std::size_t(1));
        QVERIFY(same(ink_picture(loaded.document, 0), page1, QStringLiteral("reopened")));
        QVERIFY(as_expected(ink_picture(loaded.document, 0), QStringLiteral("paper-settings")));
        QVERIFY(same(render::render_page(loaded.document.page(0), kDpi, {}, &loaded.document).image, whole.image, QStringLiteral("reopened page")));
        // a book without any paper: no feature, no paper anywhere in it
        const fs::path plain = core::path_from_utf8((tmp.path() + QStringLiteral("/普通.genko")).toStdString());
        core::Document none = core::new_episode("普通", core::Num(1), 1, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 150, "mono"));
        none = run_ops(none, Json::array({Json{{"op", "define_brush"}, {"key", "my_plain"}, {"label", "普通"}, {"texture", "grain"}}})).doc;
        write_book(none, plain);
        const std::string text = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(plain / "project.json")));
        QVERIFY(text.find("\"features\": []") != std::string::npos);
        QVERIFY(text.find("paper") == std::string::npos);
    }

    // A missing picture or settings that cannot be read: reported, the book read-only (never drawn without its paper).
    void brokenBooks() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8((tmp.path() + QStringLiteral("/b.genko")).toStdString());
        const core::Document doc = reference_book();
        write_book(doc, dir);
        const std::string ref = doc.brush_custom["my_paper_a"]["paper"]["asset"].get<std::string>();
        // settings that cannot be read
        Json project = genko::test::read_json(QString::fromStdString(core::path_to_utf8(dir / "project.json")));
        const Json saved = project;
        project["brush"]["custom"]["my_paper_b"]["paper"]["blend"] = "screen";
        genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")), project.dump(2));
        auto loaded = storage::load_document(dir);
        QVERIFY(!loaded.document.read_only_reason.empty());
        bool reported = false;
        for (const auto& issue : loaded.report.issues) reported = reported || issue.pointer == "/brush/custom/my_paper_b/paper";
        QVERIFY2(reported, loaded.report.to_json().dump().c_str());
        // the picture gone
        genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json")), saved.dump(2));
        QVERIFY(fs::remove(dir / storage::AssetStore::relpath(ref, ".png")));
        loaded = storage::load_document(dir);
        QVERIFY(!loaded.document.read_only_reason.empty());
        reported = false;
        for (const auto& issue : loaded.report.issues) reported = reported || (issue.kind == "missing_asset" && issue.ref == ref);
        QVERIFY2(reported, loaded.report.to_json().dump().c_str());
        // a feature this build knows is not a reason to open read-only
        QVERIFY(storage::is_known_feature(core::kPaperFeature));
    }

    // Undo and Redo of the journal (on disk): the paper's change undone gives back the first pixels; the first use of a
    // paper undone gives back the book without the feature.
    void undoAndRedo() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8((tmp.path() + QStringLiteral("/u.genko")).toStdString());
        core::Document doc = core::new_episode("戻す", core::Num(1), 1, core::PageSpec::custom(60, 40, 50, 30, 1, 2, 2, 2, 2, kDpi, "mono"));
        doc.edit_page(0).frames.clear();
        write_book(doc, dir);
        const Json first_ops = Json::array({Json{{"op", "define_brush"}, {"key", "my_u"}, {"label", "戻す"}, {"base", "fill_pen"}, {"width_mm", 5},
                                                 {"paper", Json{{"png", core::b64encode(data_bytes(QStringLiteral("grain.png")))}, {"density", 1}}}}});
        const auto first = run_ops(doc, first_ops);
        core::Document drawn = first.doc;
        ink_layer(drawn, 0).strokes = core::make_strokes({std::make_shared<const core::Stroke>(line("u-1", "my_u", 5, wave(5, 55, 20, 6, 1, 1)))});
        save_next(drawn, dir, first.journal_ops);
        const render::Image before = ink_picture(storage::load_document(dir).document, 0);
        const auto changed = run_ops(drawn, Json::array({Json{{"op", "define_brush"}, {"key", "my_u"}, {"label", "戻す"}, {"base", "fill_pen"},
                                                            {"width_mm", 5}, {"paper", Json{{"asset", drawn.brush_custom["my_u"]["paper"]["asset"]},
                                                                                             {"density", 1}, {"rotation", 60}, {"scale", 2}}}}}));
        save_next(changed.doc, dir, changed.journal_ops);
        brushes::clear_custom();
        const render::Image after = ink_picture(storage::load_document(dir).document, 0);
        QVERIFY(after.tobytes() != before.tobytes());
        storage::restore(dir, "human:test", false, false);  // undo
        brushes::clear_custom();
        paper::clear();
        QVERIFY(same(ink_picture(storage::load_document(dir).document, 0), before, QStringLiteral("undone")));
        storage::restore(dir, "human:test", true, false);  // redo
        brushes::clear_custom();
        paper::clear();
        QVERIFY(same(ink_picture(storage::load_document(dir).document, 0), after, QStringLiteral("redone")));
        storage::restore(dir, "human:test", false, false);
        storage::restore(dir, "human:test", false, false);  // the first use undone: no feature again
        const auto bare = storage::load_document(dir);
        QVERIFY(bare.document.features.empty());
        QVERIFY(bare.document.brush_custom.empty());
    }

    // Print: the PDF (each page one lossless picture) drawn back to pixels by a fixed rasterizer, poppler's cairo
    // backend (pdftocairo), at the resolution of the print (the size the page has in pixels at 300 dpi: the PDF's page
    // is its mm, a fraction of a pixel more, so a literal -r 300 resamples the whole page) lies within one pixel of the
    // PNG export, each channel within 2/255; and the print is the page as render_page draws it.
    void printedPdf() {
        const QString pdftocairo = QStandardPaths::findExecutable(QStringLiteral("pdftocairo"));
        if (pdftocairo.isEmpty()) QSKIP("no PDF rasterizer (poppler's pdftocairo) to draw the PDF back");
        QTemporaryDir tmp;
        const core::Document doc = reference_book();
        brushes::register_book(doc);
        const fs::path out = core::path_from_utf8(tmp.path().toStdString());
        const auto pdf = formats::export_print(doc, out / "pdf", "pdf", kDpi, 180, false, "paper", "rgb");
        const auto png = formats::export_print(doc, out / "png", "png", kDpi, 180, false, "paper", "rgb");
        QCOMPARE(pdf.size(), std::size_t(1));
        QCOMPARE(png.size(), std::size_t(2));
        const auto version = genko::test::run(pdftocairo, {QStringLiteral("-v")}, QProcessEnvironment::systemEnvironment());
        qInfo("%s", (version.out + version.err).trimmed().split('\n').value(0).constData());
        for (int n = 0; n < 2; ++n) {
            const render::Image want = render::read_png(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(png[n])))).convert("RGB");
            const QString prefix = tmp.path() + QStringLiteral("/back%1").arg(n + 1);
            const auto drawn = genko::test::run(pdftocairo, {QStringLiteral("-png"), QStringLiteral("-f"), QString::number(n + 1), QStringLiteral("-l"),
                                                             QString::number(n + 1), QStringLiteral("-singlefile"), QStringLiteral("-scale-to-x"),
                                                             QString::number(want.width()), QStringLiteral("-scale-to-y"), QString::number(want.height()),
                                                             QString::fromStdString(core::path_to_utf8(pdf[0])), prefix},
                                                QProcessEnvironment::systemEnvironment());
            QVERIFY2(drawn.finished && drawn.exit_code == 0, drawn.err.constData());
            const render::Image back = render::read_png(genko::test::read_bytes(prefix + QStringLiteral(".png"))).convert("RGB");
            QVERIFY(std::abs(back.width() - want.width()) <= 1 && std::abs(back.height() - want.height()) <= 1);
            const Fit fit = best_fit(want, back);
            qInfo("page %d: %dx%d drawn back as %dx%d, shift (%d, %d), largest difference %d", n + 1, want.width(), want.height(),
                  back.width(), back.height(), fit.dx, fit.dy, fit.largest);
            QVERIFY(fit.largest <= 2);
        }
        const render::Image page = render::render_page(doc.page(0), kDpi, {}, &doc).image;
        const render::Image printed = render::read_png(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(png[0])))).convert("RGB");
        QVERIFY(same(printed, page, QStringLiteral("print png")));
    }

    // Through the command line: `genko apply` with define_brush's paper (the picture in the op) and add_stroke, then
    // `genko render` of another process draws it as this one does.
    void commandLine() {
        QTemporaryDir tmp;
        const QString book = tmp.path() + QStringLiteral("/cli.genko");
        QCOMPARE(genko::test::run_genko({"new", book, "--pages", "1", "--title", "紙"}).exit_code, 0);
        const Json ops = Json::array({Json{{"op", "define_brush"}, {"key", "my_cli"}, {"label", "紙"}, {"base", "fill_pen"}, {"width_mm", 6},
                                           {"paper", Json{{"png", core::b64encode(data_bytes(QStringLiteral("grain.jpg")))}, {"density", 1}, {"scale", 2}}}},
                                      Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"kind", "my_cli"}, {"width_mm", 6},
                                           {"points", Json::array({Json::array({40, 60, 1}), Json::array({150, 200, 1})})}}});
        genko::test::write_bytes(tmp.path() + QStringLiteral("/ops.json"), ops.dump());
        const auto applied = genko::test::run_genko({"apply", book, tmp.path() + QStringLiteral("/ops.json"), "--agent", "human:test"});
        QVERIFY2(applied.exit_code == 0, (applied.out + applied.err).constData());
        const Json reply = genko::test::one_line(applied.out);
        QVERIFY2(reply.value("ok", false), applied.out.constData());
        const auto rendered = genko::test::run_genko({"render", book, "--page", "1", "--dpi", "100", "--out", tmp.path() + QStringLiteral("/p1.png")});
        QVERIFY2(rendered.exit_code == 0, (rendered.out + rendered.err).constData());
        const auto loaded = storage::load_document(core::path_from_utf8(book.toStdString()));
        QVERIFY(loaded.report.clean());
        brushes::register_book(loaded.document);
        const render::Image here = render::render_page(loaded.document.page(0), 100, {}, &loaded.document).image;
        QVERIFY(same(render::read_png(genko::test::read_bytes(tmp.path() + QStringLiteral("/p1.png"))).convert("RGB"), here, QStringLiteral("cli")));
        // and it differs from the same line without its paper
        core::Document bare = loaded.document;
        bare.brush_custom["my_cli"].erase("paper");
        brushes::clear_custom();
        brushes::register_book(bare);
        QVERIFY(render::render_page(bare.page(0), 100, {}, &bare).image.tobytes() != here.tobytes());
    }
};

QTEST_GUILESS_MAIN(TestPaper)
#include "test_paper.moc"
