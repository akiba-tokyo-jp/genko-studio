// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」「Photomerge」「ファイルをスタックとして読み込み・統計」), this build's own contract
// with PhotoCraft's align_cmds and photo_cmds tests' ideas (MIT): auto_align_layers puts two layers back on each other on 8-bit,
// 16-bit and CMYK pages (reposition within a pixel; each of the six projections brings them together), keeps the reference,
// refuses a locked layer, pictures with nothing to match (対応が見つかりません) and an agent; auto_blend_layers' masks (the
// panorama's seam, focus stacking's sharp layer) and the seamless layer of their kind; Photomerge's new book (its size, a layer
// and a mask a picture, the edges filled), the same again, a panorama past the limits refused (パノラマが大きすぎます); ファイルを
// スタックとして読み込み (the largest size, a layer a file — the first at the bottom —, one smart object); 統計's median taking a
// passer-by out; and `genko automate stack|statistics|photomerge`.

#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "render/features.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_align.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "service/automate.hpp"
#include "service/studio_service.hpp"
#include "service/wording.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

core::Document run(const core::Document& doc, const Json& ops) { return bus().apply(doc, ops, core::Actor("human:作者")).doc; }
core::ApplyResult applied(const core::Document& doc, const Json& ops) { return bus().apply(doc, ops, core::Actor("human:作者")); }

std::string refused(const core::Document& doc, const Json& op, const core::Actor& actor = core::Actor("human:作者")) {
    try {
        (void)bus().apply(doc, Json::array({op}), actor);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }
QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

// A deterministic texture (upstream's texture_layer): a grey box of 240 × 180 at (dx, dy) with 120 coloured rectangles.
render::Image texture(int w, int h, int dx, int dy) {
    render::Image img = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
    img.paste(render::Ink{102, 102, 102, 255}, render::Box{dx, dy, dx + 240, dy + 180});
    std::uint64_t seed = 99;
    const auto rnd = [&seed] {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(seed >> 33) / static_cast<double>(1ULL << 31);
    };
    for (int k = 0; k < 120; ++k) {
        const int x = static_cast<int>(rnd() * 220.0), y = static_cast<int>(rnd() * 160.0);
        const int sw = 4 + static_cast<int>(rnd() * 14.0), sh = 4 + static_cast<int>(rnd() * 10.0);
        const double v = rnd();
        img.paste(render::Ink{static_cast<int>(v * 255), static_cast<int>((1 - v) * 255), static_cast<int>(v * 127), 255},
                  render::Box{dx + x, dy + y, dx + x + sw, dy + y + sh});
    }
    return img;
}

// A book of one transparent page of w × h px (新規 → 画像) with paint layers A and B holding these pictures.
core::Document two_layers(int w, int h, const render::Image& a, const render::Image& b, const Json& mode = Json()) {
    formats::NewImage image;
    image.title = "整列";
    image.width = w;
    image.height = h;
    image.background = "transparent";
    core::Document doc = formats::new_image(image).doc;
    Json ops = Json::array();
    for (const auto& [id, picture] : {std::pair<const char*, const render::Image*>{"A", &a}, {"B", &b}}) {
        ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", id}, {"name", id}});
        ops.push_back(Json{{"op", "put_raster"}, {"page", 1}, {"id", id}, {"png_base64", core::b64encode(render::write_png(*picture))}});
    }
    doc = run(doc, ops);
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    return doc;
}

const Json kSixteen{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}};
const Json kCmyk{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}};

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers) {
        if (l.id == id) return l;
    }
    throw std::runtime_error("no layer " + id);
}

// The box (x0, y0, x1, y1 px of its grid) of where the layer shows (alpha over a half).
// The page's own pixels across and down (its paper at its resolution: what new_image made it).
std::pair<double, double> page_px(const core::Document& doc) {
    const core::Page& page = doc.page(0);
    const double dpi = page.spec.dpi.value();
    return {std::round(page.spec.width_mm.value() / 25.4 * dpi), std::round(page.spec.height_mm.value() / 25.4 * dpi)};
}

// The box (x0, y0, x1, y1, in the page's own pixels whatever grid the layer holds them on) of where the layer shows.
std::array<int, 4> bounds(const core::Document& doc, const std::string& id) {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, id));
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p.at(i)[p.colours()] <= 0.5f) continue;
        const int x = static_cast<int>(i % p.width()), y = static_cast<int>(i / p.width());
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
    }
    const auto [w, h] = page_px(doc);
    const double sx = w / p.width(), sy = h / p.height();
    return {static_cast<int>(std::lround(x0 * sx)), static_cast<int>(std::lround(y0 * sy)), static_cast<int>(std::lround(x1 * sx)),
            static_cast<int>(std::lround(y1 * sy))};
}

// The RGB the layer shows at a pixel of the page (an ink layer's: what its inks mean).
std::array<float, 4> rgba_at(const core::Document& doc, const std::string& id, int x, int y) {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, id));
    const std::vector<render::image_ops::Rgba> all = render::image_ops::rgba_of(p);
    const auto [w, h] = page_px(doc);
    const std::size_t gx = static_cast<std::size_t>((x + 0.5) * p.width() / w), gy = static_cast<std::size_t>((y + 0.5) * p.height() / h);
    return all[gy * p.width() + gx];
}

// A layer's mask's value (0..255) at a pixel of the page (the mask's picture at its own grid).
int mask_at(const core::Document& doc, const std::string& id, int x, int y) {
    const core::Layer& l = layer_of(doc, id);
    if (!l.mask || !l.mask->png) throw std::runtime_error("no mask on " + id);
    const render::Image m = render::read_png(*l.mask->png).convert("L");
    const auto [w, h] = page_px(doc);
    return static_cast<int>(m.getpixel(static_cast<int>((x + 0.5) * m.width() / w), static_cast<int>((y + 0.5) * m.height() / h))[0]);
}

// The mean difference of the two layers' luminance where both show.
double disagreement(const core::Document& doc) {
    const auto pa = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, "A"));
    const auto pb = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, "B"));
    const auto a = render::image_ops::rgba_of(pa), b = render::image_ops::rgba_of(pb);
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i][3] < 0.99f || b[i][3] < 0.99f) continue;
        const auto l = [](const render::image_ops::Rgba& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; };
        sum += std::fabs(l(a[i]) - l(b[i]));
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 1.0;
}

// A scene (blobs over a gradient) and crops of it at offsets, as pictures (upstream's panorama test).
render::Image crop(const render::Image& scene, int x, int y, int w, int h) { return scene.crop(render::Box{x, y, x + w, y + h}); }

render::Image scene(int w, int h, std::uint64_t seed) {
    render::Image img = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 255});
    for (int x = 0; x < w; ++x) {
        const int v = static_cast<int>((0.25 + 0.2 * x / w) * 255);
        img.paste(render::Ink{v, v, v, 255}, render::Box{x, 0, x + 1, h});
    }
    render::features::Rng rng{seed};
    const int count = std::max(w * h / 900, 40);
    for (int k = 0; k < count; ++k) {
        const int cx = static_cast<int>(rng.next() % std::uint64_t(w)), cy = static_cast<int>(rng.next() % std::uint64_t(h));
        const int r = 3 + static_cast<int>(rng.next() % 9);
        const int v = static_cast<int>(rng.next() % 1000) * 255 / 1000;
        const int g = static_cast<int>(rng.next() % 1000) * 255 / 1000;
        img.paste(render::Ink{v, g, 255 - v, 255}, render::Box{std::max(cx - r, 0), std::max(cy - r * 6 / 10, 0), std::min(cx + r, w), std::min(cy + r * 6 / 10, h)});
    }
    return img;
}

render::RenderResult drawn(const core::Document& doc, int dpi) {
    render::RenderOptions options;
    options.mode = "proof";
    options.inks = doc.page(0).color.has_value();
    options.precise = !doc.page(0).color && doc.page(0).rgb && doc.page(0).rgb->depth > 8;
    return render::render_page(doc.page(0), dpi, options, &doc);
}

std::pair<int, Json> cli(const QStringList& args) {
    const test::Run r = test::run_genko(args);
    return {r.exit_code, test::one_line(r.out)};
}

}  // namespace

class TestContractAutoAlign : public QObject {
    Q_OBJECT

private slots:
    // Reposition on 8-bit, 16-bit and CMYK pages: B (moved by 11, −7) back on A within a pixel; the reference stays.
    void alignsLayersOnEachWay() {
        for (const Json& mode : {Json(), kSixteen, kCmyk}) {
            const core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 31, 13), mode);
            const auto before_a = bounds(doc, "A");
            QVERIFY(std::abs(bounds(doc, "B")[0] - 31) <= 1);
            const core::ApplyResult r = applied(doc, Json::array({Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})},
                                                                       {"projection", "reposition"}, {"reference", "A"}}}));
            const Json& report = r.results.at(0);
            QCOMPARE(report["reference"], Json("A"));
            QCOMPARE(report["layout"], Json("reposition"));
            QCOMPARE(report["aligned"].size(), std::size_t(1));
            QCOMPARE(report["aligned"][0]["id"], Json("B"));
            QCOMPARE(report["failed"].size(), std::size_t(0));
            const auto b = bounds(r.doc, "B");
            QVERIFY2(std::abs(b[0] - 20) <= 1 && std::abs(b[1] - 20) <= 1, qPrintable(QString::fromStdString(report.dump()) + QStringLiteral(" %1,%2").arg(b[0]).arg(b[1])));
            QCOMPARE(bounds(r.doc, "A"), before_a);
            QVERIFY(disagreement(r.doc) < 0.02);
        }
    }

    // Each projection brings the two layers together (the layers drawn again on the plane, cylinder or sphere); auto picks
    // the perspective; the same op again gives the same pixels.
    void everyProjectionAligns() {
        const core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 26, 25));
        const double before = disagreement(doc);
        for (const char* projection : {"auto", "perspective", "cylindrical", "spherical", "collage", "reposition"}) {
            const Json op{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}, {"projection", projection}};
            const core::ApplyResult r = applied(doc, Json::array({op}));
            const Json& report = r.results.at(0);
            QCOMPARE(report["layout"], Json(std::string(projection) == "auto" ? "perspective" : projection));
            const double after = disagreement(r.doc);
            QVERIFY2(after < before * 0.5, qPrintable(QStringLiteral("%1: %2 → %3").arg(QString::fromLatin1(projection)).arg(before).arg(after)));
            if (std::string(projection) == "reposition") {
                const Json& m = report["aligned"][0]["matrix"];
                QVERIFY(m.is_array() && m.size() == 9);
            }
            const core::Document again = run(doc, Json::array({op}));
            QVERIFY2(*layer_of(again, "B").raster_png == *layer_of(r.doc, "B").raster_png, projection);
        }
    }

    // (the review of I-g, P2-4 and P3-6) No reference given: the best joined layer — a flat layer at the bottom that joins none
    // is left out (failed), the others aligned —; that flat layer given as the reference: 対応が見つかりません. A layer linked to
    // one that moves moves with it.
    void referenceIsChosenAndLinksMoveAlong() {
        core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 31, 13));
        const std::string first = doc.page(0).layers.front().id;
        render::Image flat = render::Image::create("RGBA", render::Size{300, 240}, render::Ink{120, 120, 120, 255});
        render::Image dot = render::Image::create("RGBA", render::Size{300, 240}, render::Ink{0, 0, 0, 0});
        dot.paste(render::Ink{200, 30, 30, 255}, render::Box{250, 200, 260, 210});
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "Z"}, {"name", "Z"}, {"after", first}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "Z"}, {"png_base64", core::b64encode(render::write_png(flat))}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "L"}, {"name", "L"}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "L"}, {"png_base64", core::b64encode(render::write_png(dot))}},
                                    Json{{"op", "link_layers"}, {"page", 1}, {"ids", Json::array({"B", "L"})}, {"link", true}}}));
        QCOMPARE(doc.page(0).layers[1].id, std::string("Z"));
        const core::ApplyResult r = applied(doc, Json::array({Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"Z", "A", "B"})},
                                                                   {"projection", "reposition"}}}));
        const Json& report = r.results.at(0);
        QCOMPARE(report["failed"], Json::array({"Z"}));
        QVERIFY(report["reference"] == Json("A") || report["reference"] == Json("B"));
        QVERIFY(disagreement(r.doc) < 0.02);
        // (L, linked to B, moved as B did — or stayed with B the reference)
        const auto b0 = bounds(doc, "B"), b1 = bounds(r.doc, "B"), l0 = bounds(doc, "L"), l1 = bounds(r.doc, "L");
        QVERIFY2(std::abs((l1[0] - l0[0]) - (b1[0] - b0[0])) <= 1 && std::abs((l1[1] - l0[1]) - (b1[1] - b0[1])) <= 1,
                 qPrintable(QStringLiteral("B %1,%2 L %3,%4").arg(b1[0] - b0[0]).arg(b1[1] - b0[1]).arg(l1[0] - l0[0]).arg(l1[1] - l0[1])));
        const std::string said = refused(doc, Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"Z", "A", "B"})}, {"reference", "Z"}});
        QVERIFY2(said.find("対応が見つかりません") != std::string::npos, said.c_str());
    }

    // (P2-3) strict_gates: before the name is approved neither op touches the printed layers; after it, both do.
    void strictGatesHoldBoth() {
        core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 31, 13));
        doc.strict_gates = true;
        for (const char* name : {"auto_align_layers", "auto_blend_layers"}) {
            const std::string said = refused(doc, Json{{"op", name}, {"page", 1}, {"ids", Json::array({"A", "B"})}});
            QVERIFY2(said.find("needs name_ok") != std::string::npos, (std::string(name) + ": " + said).c_str());
        }
        doc.edit_page(0).name_ok = true;
        for (const char* name : {"auto_align_layers", "auto_blend_layers"}) QVERIFY2(refused(doc, Json{{"op", name}, {"page", 1}, {"ids", Json::array({"A", "B"})}}).empty(), name);
    }

    // (P3-5) The merged layer goes above the layers clipped onto the top one (its clipping group kept whole).
    void mergedLayerKeepsTheClippingGroup() {
        core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 31, 13));
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "K"}, {"name", "K"}, {"after", "B"}, {"clip", true}}}));
        const core::Document r = run(doc, Json::array({Json{{"op", "auto_blend_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}, {"new_id", "M"}}}));
        const auto& layers = r.page(0).layers;
        const auto at = [&](const char* id) { return std::find_if(layers.begin(), layers.end(), [id](const core::Layer& l) { return l.id == id; }) - layers.begin(); };
        QCOMPARE(at("K"), at("B") + 1);
        QCOMPARE(at("M"), at("K") + 1);
    }

    // (P2-5) Photomerge lays each picture in the first one's profile: a picture of another profile converted first — the
    // pictures agree where they overlap, the panorama in sRGB.
    void photomergeConvertsProfiles() {
        const render::Image sc = scene(640, 300, 3);
        std::vector<service::automate::Picture> pictures;
        for (const auto& [x, y] : {std::pair{20, 30}, std::pair{190, 40}}) {
            const std::string name = "p" + std::to_string(x) + ".png";
            formats::Opened o = formats::open_file(render::write_png(crop(sc, x, y, 260, 220)), name);
            core::Document d = std::move(o.doc);
            if (x == 190) d = run(d, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "prophoto-compat"}}}));
            pictures.push_back({name, std::make_shared<const core::Document>(std::move(d)), 0});
        }
        QVERIFY(pictures[1].doc->page(0).rgb && !pictures[1].doc->page(0).rgb->profile.empty());
        service::automate::PhotomergeOptions options;
        options.layout = "reposition";
        options.blend = false;
        const service::automate::Panorama p = service::automate::photomerge(pictures, options);
        const core::Document& book = p.opened.doc;
        QVERIFY(!book.page(0).rgb || book.page(0).rgb->profile.empty());
        // (where both show — scene 200..240 × 90..230, the panorama's 180..220 × 60..200 —, their colours on the average the same)
        std::array<double, 3> sum_a{}, sum_b{};
        int count = 0;
        for (int y = 60; y < 200; y += 4) {
            for (int x = 180; x < 220; x += 4) {
                const auto a = rgba_at(book, book.page(0).layers[0].id, x, y), b = rgba_at(book, book.page(0).layers[1].id, x, y);
                QVERIFY(a[3] > 0.99f && b[3] > 0.99f);
                for (int k = 0; k < 3; ++k) {
                    sum_a[k] += a[k];
                    sum_b[k] += b[k];
                }
                ++count;
            }
        }
        for (int k = 0; k < 3; ++k) {
            QVERIFY2(std::fabs(sum_a[k] - sum_b[k]) / count < 3.0 / 255.0, qPrintable(QStringLiteral("%1: %2 vs %3").arg(k).arg(sum_a[k] / count).arg(sum_b[k] / count)));
        }
    }

    // (P1-1, P2-7, P3-1) The pictures weighed by their headers before any is decoded: too large refused in words, more than
    // it takes refused before any file is read, the scale counted.
    void refusesLargePicturesBeforeDecoding() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        // (QOI headers of large pictures with no pixels after them: decoding them would fail — the words say they were weighed)
        const auto qoi = [](std::uint32_t w, std::uint32_t h) {
            std::string b = "qoif";
            for (const std::uint32_t v : {w, h}) {
                for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<char>((v >> s) & 0xFF));
            }
            b += std::string("\x04\x00", 2);
            b += std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8);
            return b;
        };
        write(root / "a.qoi", qoi(6000, 6000));
        write(root / "b.qoi", qoi(6000, 6000));
        write(root / "c.qoi", qoi(10000, 10000));
        write(root / "d.qoi", qoi(10000, 10000));
        const auto pictures = service::automate::picture_files({root / "a.qoi", root / "b.qoi"}, {});
        QCOMPARE(service::automate::picture_size(pictures[0]), (std::pair<std::int64_t, std::int64_t>{6000, 6000}));
        const auto said = [](const std::function<void()>& f) -> std::string {
            try {
                f();
            } catch (const std::exception& e) {
                return e.what();
            }
            return {};
        };
        service::automate::PhotomergeOptions options;
        std::string what = said([&] { (void)service::automate::photomerge(pictures, options); });
        QVERIFY2(what.find("too large to stitch at once") != std::string::npos, what.c_str());
        QVERIFY(genko::wording::error(QString::fromStdString(what)).contains(QStringLiteral("画像が大きすぎて")));
        // (read at half their size they fit: weighed, then opened — and these, having no pixels, cannot be)
        options.scale = 50;
        what = said([&] { (void)service::automate::photomerge(pictures, options); });
        QVERIFY2(!what.empty() && what.find("too large to stitch") == std::string::npos, what.c_str());
        service::automate::StackOptions stack;
        stack.actor = core::Actor("human:作者");
        const auto big = service::automate::picture_files({root / "c.qoi", root / "d.qoi"}, {});
        what = said([&] { (void)service::automate::load_files_into_stack(big, stack); });
        QVERIFY2(what.find("the stack would be too large") != std::string::npos, what.c_str());
        what = said([&] { (void)service::automate::statistics(big, "median", stack); });
        QVERIFY2(what.find("the stack would be too large") != std::string::npos, what.c_str());
        // (33 pictures — that are not there —: refused before any is opened; an aligned stack likewise; a wrong mode first)
        std::vector<service::automate::Picture> many;
        for (int i = 0; i < 33; ++i) {
            service::automate::Picture p;
            p.name = "x" + std::to_string(i);
            p.file = root / ("missing" + std::to_string(i) + ".png");
            p.width = p.height = 10;
            many.push_back(p);
        }
        what = said([&] { (void)service::automate::photomerge(many, service::automate::PhotomergeOptions{}); });
        QVERIFY2(what.find("more than 32 pictures") != std::string::npos, what.c_str());
        stack.align = true;
        what = said([&] { (void)service::automate::load_files_into_stack(many, stack); });
        QVERIFY2(what.find("more than 32 pictures to align") != std::string::npos, what.c_str());
        what = said([&] { (void)service::automate::statistics(many, "average", stack); });
        QVERIFY2(what.find("the stack mode is") != std::string::npos, what.c_str());
        QVERIFY(render::align::photomerge_bytes(20'000'000, 24'000'000, true, false) > 2'000'000'000ULL);
    }

    void refusesWhatItShould() {
        const core::Document doc = two_layers(300, 240, texture(300, 240, 20, 20), texture(300, 240, 31, 13));
        const Json op{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}};
        // (a locked layer)
        const core::Document locked = run(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "B"}, {"locked", true}}}));
        QVERIFY(!refused(locked, op).empty());
        // (one layer, an unknown projection, a reference not among them)
        QVERIFY(!refused(doc, Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A"})}}).empty());
        QVERIFY(!refused(doc, Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}, {"projection", "sideways"}}).empty());
        QVERIFY(!refused(doc, Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}, {"reference", "Z"}}).empty());
        // (flat layers: nothing to match — 対応が見つかりません)
        render::Image flat = render::Image::create("RGBA", render::Size{300, 240}, render::Ink{120, 120, 120, 255});
        const core::Document plain = two_layers(300, 240, flat, flat);
        const std::string said = refused(plain, op);
        QVERIFY2(said.find("対応が見つかりません") != std::string::npos, said.c_str());
        QVERIFY(genko::wording::error(QString::fromStdString(said)).contains(QStringLiteral("対応が見つかりません")));
        // (not an agent's: neither op is in apply_ops' list, and steps of them are refused for an agent)
        QVERIFY(!service::agent_op("auto_align_layers"));
        QVERIFY(!service::agent_op("auto_blend_layers"));
        QVERIFY_THROWS_EXCEPTION(service::automate::StepsError, service::automate::check_steps(Json::array({op}), core::Actor("ai:helper")));
    }

    // Panorama: the left layer shows on the left, the right one on the right (their masks), the seamless layer keeps each side's
    // colour; on 8-bit, 16-bit and CMYK pages.
    void blendsPanoramaMasksAndSeamlessLayer() {
        render::Image left = render::Image::create("RGBA", render::Size{200, 60}, render::Ink{0, 0, 0, 0});
        left.paste(render::Ink{51, 77, 204, 255}, render::Box{0, 0, 130, 60});
        render::Image right = render::Image::create("RGBA", render::Size{200, 60}, render::Ink{0, 0, 0, 0});
        right.paste(render::Ink{230, 153, 51, 255}, render::Box{70, 0, 200, 60});
        for (const Json& mode : {Json(), kSixteen, kCmyk}) {
            const core::Document doc = two_layers(200, 60, left, right, mode);
            const core::ApplyResult r = applied(doc, Json::array({Json{{"op", "auto_blend_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})},
                                                                       {"method", "panorama"}, {"new_id", "M"}}}));
            const Json& report = r.results.at(0);
            QCOMPARE(report["blended"], Json("M"));
            // (the masks at the page's mask grid — 200 dpi here, finer than the layers' —, whole: no gap between the layers' pixels)
            for (const char* id : {"A", "B"}) {
                const render::Image m = render::read_png(*layer_of(r.doc, id).mask->png).convert("L");
                int flips = 0;
                for (int x = 1; x < m.width(); ++x) flips += (m.getpixel(x, m.height() / 2)[0] > 127) != (m.getpixel(x - 1, m.height() / 2)[0] > 127) ? 1 : 0;
                QVERIFY2(flips <= 2, qPrintable(QStringLiteral("%1: %2 flips").arg(QString::fromLatin1(id)).arg(flips)));
            }
            const std::array<int, 4> masks{mask_at(r.doc, "A", 10, 30), mask_at(r.doc, "B", 10, 30), mask_at(r.doc, "A", 190, 30), mask_at(r.doc, "B", 190, 30)};
            QVERIFY2(masks == (std::array<int, 4>{255, 0, 0, 255}),
                     qPrintable(QStringLiteral("%1: %2 %3 %4 %5").arg(QString::fromStdString(mode.dump())).arg(masks[0]).arg(masks[1]).arg(masks[2]).arg(masks[3])));
            // (the merged layer above B, of their kind; the left's blue on the left)
            QCOMPARE(r.doc.page(0).layers.back().id, std::string("M"));
            const auto px = rgba_at(r.doc, "M", 5, 30);
            const auto was = rgba_at(doc, "A", 5, 30);
            QVERIFY2(std::fabs(px[2] - was[2]) < 0.03f && px[3] > 0.99f, qPrintable(QStringLiteral("%1 vs %2").arg(px[2]).arg(was[2])));
            QCOMPARE(static_cast<bool>(layer_of(r.doc, "M").color_raster), static_cast<bool>(layer_of(r.doc, "A").color_raster));
            QCOMPARE(static_cast<bool>(layer_of(r.doc, "M").ink_raster), static_cast<bool>(layer_of(r.doc, "A").ink_raster));
        }
    }

    // Focus stacking: the sharp layer shows everywhere, the flat one nowhere; without seamless, no merged layer.
    void blendsStackPicksTheSharp() {
        render::Image sharp = render::Image::create("RGBA", render::Size{64, 64}, render::Ink{0, 0, 0, 255});
        for (int y = 0; y < 64; y += 2)
            for (int x = 0; x < 64; x += 2) {
                const int v = (x / 2 + y / 2) % 2 == 0 ? 25 : 230;
                sharp.paste(render::Ink{v, v, v, 255}, render::Box{x, y, x + 2, y + 2});
            }
        const render::Image flat = render::Image::create("RGBA", render::Size{64, 64}, render::Ink{128, 128, 128, 255});
        const core::Document doc = two_layers(64, 64, sharp, flat);
        const core::ApplyResult r = applied(doc, Json::array({Json{{"op", "auto_blend_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})},
                                                                   {"method", "stack"}, {"seamless", false}}}));
        QVERIFY(r.results.at(0)["blended"].is_null());
        QCOMPARE(r.doc.page(0).layers.size(), doc.page(0).layers.size());
        for (int x = 0; x < 64; ++x) QVERIFY(mask_at(r.doc, "A", x, 32) > 127);
        QCOMPARE(mask_at(r.doc, "B", 10, 10), 0);
        QVERIFY(!refused(doc, Json{{"op", "auto_blend_layers"}, {"page", 1}, {"ids", Json::array({"A", "B"})}, {"method", "sideways"}}).empty());
    }

    // Photomerge: three crops of a scene at known offsets become a book of the panorama's size, a layer and a mask a picture;
    // the edges filled on a layer of their own; the same again.
    void photomergeMakesTheBook() {
        const render::Image sc = scene(640, 300, 3);
        const std::array<std::array<int, 2>, 3> offs{{{20, 30}, {190, 40}, {360, 25}}};
        std::vector<service::automate::Picture> pictures;
        for (std::size_t i = 0; i < offs.size(); ++i) {
            const std::string name = "p" + std::to_string(i) + ".png";
            formats::Opened o = formats::open_file(render::write_png(crop(sc, offs[i][0], offs[i][1], 260, 220)), name);
            pictures.push_back({name, std::make_shared<const core::Document>(std::move(o.doc)), 0});
        }
        service::automate::PhotomergeOptions options;
        options.layout = "reposition";
        const service::automate::Panorama p = service::automate::photomerge(pictures, options);
        QVERIFY2(std::abs(p.info["width"].get<int>() - 600) <= 2 && std::abs(p.info["height"].get<int>() - 235) <= 2, qPrintable(QString::fromStdString(p.info.dump())));
        const core::Page& page = p.opened.doc.page(0);
        QCOMPARE(page.layers.size(), std::size_t(3));
        for (const core::Layer& l : page.layers) QVERIFY(l.mask.has_value());
        QCOMPARE(page.layers.front().title, std::string("p0.png"));
        QCOMPARE(p.info["failed"].size(), std::size_t(0));
        // (the same pictures, the same book's pixels)
        const service::automate::Panorama again = service::automate::photomerge(pictures, options);
        for (std::size_t i = 0; i < page.layers.size(); ++i) QVERIFY(*again.opened.doc.page(0).layers[i].raster_png == *page.layers[i].raster_png);
        // (perspective with the edges filled: one more layer on top, opaque)
        options.layout = "perspective";
        options.content_aware_fill = true;
        const service::automate::Panorama filled = service::automate::photomerge(pictures, options);
        QCOMPARE(filled.opened.doc.page(0).layers.size(), std::size_t(4));
        QCOMPARE(filled.opened.doc.page(0).layers.back().title, std::string("コンテンツに応じた塗りつぶし"));
        // (two flat pictures: nothing to match)
        std::vector<service::automate::Picture> flat;
        for (int i = 0; i < 2; ++i) {
            formats::Opened o = formats::open_file(render::write_png(render::Image::create("RGB", render::Size{200, 150}, render::Ink{90, 90, 90})), "f.png");
            flat.push_back({"f.png", std::make_shared<const core::Document>(std::move(o.doc)), 0});
        }
        try {
            (void)service::automate::photomerge(flat, service::automate::PhotomergeOptions{});
            QFAIL("flat pictures merged");
        } catch (const core::Error& error) {
            QVERIFY2(std::string(error.what()).find("対応が見つかりません") != std::string::npos, error.what());
        }
    }

    // A panorama past the limits is refused before it is made, in words that say how big and what to do.
    void refusesTooLargeAPanorama() {
        const render::Image sc = scene(640, 300, 3);
        std::vector<render::align::Source> sources;
        for (const auto& [x, y] : {std::pair{20, 30}, std::pair{190, 40}}) {
            const render::Image c = crop(sc, x, y, 260, 220);
            render::align::Source s{"c", render::resample::Plane(260, 220, 4, true)};
            const std::string bytes = c.tobytes();
            for (std::size_t i = 0; i < s.plane.v.size(); ++i) s.plane.v[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
            sources.push_back(std::move(s));
        }
        render::align::MergeOptions o;
        o.layout = render::panorama::Layout::Reposition;
        o.max_pixels = 10000;
        try {
            (void)render::align::photomerge(sources, o);
            QFAIL("too large a panorama made");
        } catch (const core::OpError& error) {
            const std::string said = error.what();
            QVERIFY2(said.find("the panorama would be") != std::string::npos, said.c_str());
            QVERIFY(genko::wording::error(QString::fromStdString(said)).contains(QStringLiteral("パノラマが大きすぎます")));
        }
        o.max_pixels = 0;
        o.budget = 1000;
        QVERIFY_THROWS_EXCEPTION(core::OpError, (void)render::align::photomerge(sources, o));
        QVERIFY(render::align::photomerge_bytes(100'000'000, 100'000'000, true, false) > render::align::kBudget);
    }

    // ファイルをスタックとして読み込み: the largest size, one layer a file at the top left (the first at the bottom, each its
    // name); --smart one smart object.
    void loadsFilesIntoAStack() {
        std::vector<service::automate::Picture> pictures;
        for (const auto& [name, w, h, v] : {std::tuple{"a.png", 40, 30, 60}, std::tuple{"b.png", 50, 20, 120}, std::tuple{"c.png", 30, 45, 200}}) {
            formats::Opened o = formats::open_file(render::write_png(render::Image::create("RGB", render::Size{w, h}, render::Ink{v, v, v})), name);
            pictures.push_back({name, std::make_shared<const core::Document>(std::move(o.doc)), 0});
        }
        service::automate::StackOptions options;
        options.actor = core::Actor("human:作者");
        const formats::Opened stack = service::automate::load_files_into_stack(pictures, options);
        const core::Page& page = stack.doc.page(0);
        QCOMPARE(std::llround(page.spec.width_mm.value() / 25.4 * page.spec.dpi.value()), 50LL);
        QCOMPARE(std::llround(page.spec.height_mm.value() / 25.4 * page.spec.dpi.value()), 45LL);
        QCOMPARE(page.layers.size(), std::size_t(3));
        QCOMPARE(page.layers[0].title, std::string("a.png"));
        QCOMPARE(page.layers[2].title, std::string("c.png"));
        QCOMPARE(bounds(stack.doc, page.layers[1].id), (std::array<int, 4>{0, 0, 50, 20}));
        QCOMPARE(stack.doc.title, std::string("a"));
        options.smart = true;
        const formats::Opened smart = service::automate::load_files_into_stack(pictures, options);
        QCOMPARE(smart.doc.page(0).layers.size(), std::size_t(1));
        QCOMPARE(smart.doc.page(0).layers[0].kind, core::LayerKind::Smart);
    }

    // 統計 (median): three shots of a place, a passer-by in one — gone; an unknown mode refused.
    void statisticsMedianTakesThePasserByOut() {
        std::vector<service::automate::Picture> pictures;
        for (int k = 0; k < 3; ++k) {
            render::Image shot = render::Image::create("RGB", render::Size{40, 30}, render::Ink{102, 102, 102});
            if (k == 2) shot.paste(render::Ink{255, 255, 255}, render::Box{10, 10, 20, 20});
            formats::Opened o = formats::open_file(render::write_png(shot), "s" + std::to_string(k) + ".png");
            pictures.push_back({"s" + std::to_string(k) + ".png", std::make_shared<const core::Document>(std::move(o.doc)), 0});
        }
        service::automate::StackOptions options;
        options.actor = core::Actor("human:作者");
        const formats::Opened stats = service::automate::statistics(pictures, "median", options);
        QCOMPARE(stats.doc.page(0).layers.size(), std::size_t(1));
        const core::Layer& so = stats.doc.page(0).layers[0];
        QCOMPARE(so.kind, core::LayerKind::Smart);
        QCOMPARE((*so.smart)["stack"], Json("median"));
        const render::RenderResult r = drawn(stats.doc, 72);
        const render::Image rgb = r.image.convert("RGB");
        const int x = 15 * rgb.width() / 40, y = 15 * rgb.height() / 30;
        QVERIFY2(std::abs(rgb.getpixel(x, y)[0] - 102) <= 2, qPrintable(QString::number(rgb.getpixel(x, y)[0])));
        QVERIFY_THROWS_EXCEPTION(core::Error, (void)service::automate::statistics(pictures, "average", options));
    }

    // The command line: a stack (one smart object), statistics and a panorama written as new books; never over a book.
    void commandLineMakesBooks() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        const render::Image sc = scene(640, 300, 3);
        for (const auto& [name, x, y] : {std::tuple{"p0.png", 20, 30}, std::tuple{"p1.png", 190, 40}, std::tuple{"p2.png", 360, 25}}) {
            write(root / name, render::write_png(crop(sc, x, y, 260, 220)));
        }
        const QStringList pics{q(root / "p0.png"), q(root / "p1.png"), q(root / "p2.png")};
        auto [code, out] = cli(QStringList{QStringLiteral("automate"), QStringLiteral("photomerge")} + pics +
                               QStringList{QStringLiteral("--layout"), QStringLiteral("reposition"), QStringLiteral("--book"), q(root / "pano.genko")});
        QCOMPARE(code, 0);
        QCOMPARE(out["ok"], Json(true));
        QCOMPARE(out["layers"], Json(3));
        QVERIFY(std::abs(out["width"].get<int>() - 600) <= 2);
        const core::Document pano = storage::load_document(root / "pano.genko").document;
        QCOMPARE(pano.page(0).layers.size(), std::size_t(3));
        std::tie(code, out) = cli(QStringList{QStringLiteral("automate"), QStringLiteral("stack")} + pics +
                                  QStringList{QStringLiteral("--smart"), QStringLiteral("--book"), q(root / "stack.genko")});
        QCOMPARE(code, 0);
        QCOMPARE(out["layers"], Json(1));
        std::tie(code, out) = cli(QStringList{QStringLiteral("automate"), QStringLiteral("statistics")} + pics +
                                  QStringList{QStringLiteral("--mode"), QStringLiteral("mean"), QStringLiteral("--book"), q(root / "stats.genko")});
        QCOMPARE(code, 0);
        QCOMPARE(out["mode"], Json("mean"));
        // (never over a book; an option of another kind is a usage error)
        std::tie(code, out) = cli(QStringList{QStringLiteral("automate"), QStringLiteral("stack")} + pics + QStringList{QStringLiteral("--book"), q(root / "stack.genko")});
        QCOMPARE(code, 1);
        const test::Run wrong = test::run_genko(QStringList{QStringLiteral("automate"), QStringLiteral("stack")} + pics +
                                               QStringList{QStringLiteral("--layout"), QStringLiteral("auto"), QStringLiteral("--book"), q(root / "x.genko")});
        QCOMPARE(wrong.exit_code, 2);
    }
};

QTEST_GUILESS_MAIN(TestContractAutoAlign)
#include "test_contract_auto_align.moc"
