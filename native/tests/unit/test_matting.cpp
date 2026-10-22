// PC3 B3 (SPEC.md COLOR-06「選択とマスク」「マッティング」「賢い選択」): the edge refinement and the matting as PhotoCraft
// v0.2.0's algo/src/matting.rs (lines 577-746) and engine/src/layer_menu_cmds/tests.rs test them (MIT,
// native/third_party/photocraft/NOTICE.md) — the running max and the box mean against the naive ones, the guided filter's
// soft edge following a blurred edge (Smart Radius keeping a sharp one sharp), feather, contrast, shift and smooth, the
// tiles giving what one buffer gives, decontaminate and defringe —; the op matting on 8-bit, 16-bit and CMYK layers
// (the fringe's colours, the alpha kept, the area's weight, the refusals); and the smart selection's area kinds through
// render/selection on a page: subject, sky, object, quick, focus and refine as the ops take them, the same area on an
// 8-bit, a 16-bit and a CMYK page of one picture (the analysis is RGB), at the grid the area is asked at.

#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "render/image_ops.hpp"
#include "render/matting.hpp"
#include "render/ops_registry.hpp"
#include "render/raster.hpp"
#include "render/segment/segment.hpp"
#include "render/selection.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::segment::Rect;
using render::segment::Region;
using render::segment::RgbImage;
namespace matting = render::matting;

namespace {

constexpr double kInch = 25.4;  // (200 px at the working 200 dpi)
constexpr int kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

double mm(double px) { return px * kInch / kSide; }

render::Image picture(const std::function<std::array<double, 4>(int, int)>& at) {
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            for (const double v : at(x, y)) bytes.push_back(static_cast<char>(static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255))));
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Document book(const std::function<std::array<double, 4>(int, int)>& at) {
    auto doc = core::new_episode("マット", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, picture(at));
    p.layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + id);
}

render::image_ops::Rgba pixel(const core::Document& doc, int x, int y) {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, "paint"));
    const auto rgba = render::image_ops::rgba_of(p);
    const int px = static_cast<int>(std::floor((x + 0.5) * p.width() / kSide)), py = static_cast<int>(std::floor((y + 0.5) * p.height() / kSide));
    return rgba[static_cast<std::size_t>(py) * p.width() + static_cast<std::size_t>(px)];
}

// A red disc with a soft edge matted on black (its colour × its alpha) on transparency (upstream's disc, 5× larger).
std::array<double, 4> dark_disc(int x, int y) {
    const double d = std::sqrt(double((x - 100) * (x - 100) + (y - 75) * (y - 75)));
    const double a = std::clamp((50.0 - d) / 3.0, 0.0, 1.0);
    return {0.9 * a, 0.1 * a, 0.1 * a, a};
}

// A textured disc on a noisy background (upstream's disc_scene), opaque.
std::array<double, 4> disc_scene(int x, int y) {
    const double dx = x + 0.5 - 100, dy = y + 0.5 - 100;
    const double n = 0.05 * std::sin(x * 12.9898 + y * 78.233) * std::cos(x * 4.1 - y * 2.7);
    if (dx * dx + dy * dy <= 55.0 * 55.0) {
        const double t = std::sin((x + y) * 0.35) * 0.5 + 0.5;
        return {0.85 + 0.05 * t + n, 0.2 + 0.3 * t + n, 0.15 + n, 1.0};
    }
    return {0.25 + n, 0.45 + n, 0.7 + n, 1.0};
}

bool in_disc(int x, int y) {
    const double dx = x + 0.5 - 100, dy = y + 0.5 - 100;
    return dx * dx + dy * dy <= 55.0 * 55.0;
}

render::Image mask(const core::Document& doc, const Json& area, int dpi = 200) { return render::selection::to_mask(area, doc.page(0), &doc, dpi); }

double iou(const render::Image& a, const render::Image& b) {
    const std::string x = a.tobytes(), y = b.tobytes();
    std::size_t inter = 0, uni = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const bool p = static_cast<unsigned char>(x[i]) >= 128, q = static_cast<unsigned char>(y[i]) >= 128;
        inter += p && q ? 1 : 0;
        uni += p || q ? 1 : 0;
    }
    return uni == 0 ? 1.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

double iou_truth(const render::Image& m, const std::function<bool(int, int)>& truth) {
    std::size_t inter = 0, uni = 0;
    for (int y = 0; y < m.height(); ++y) {
        for (int x = 0; x < m.width(); ++x) {
            const bool p = m.getpixel(x, y)[0] >= 128, q = truth(x * kSide / m.width(), y * kSide / m.height());
            inter += p && q ? 1 : 0;
            uni += p || q ? 1 : 0;
        }
    }
    return static_cast<double>(inter) / static_cast<double>(std::max<std::size_t>(uni, 1));
}

int at(const render::Image& m, int x, int y) { return static_cast<int>(m.getpixel(x, y)[0]); }

// A horizontal blend from a (left) to b (right) across a Gaussian-blurred edge at x = 40, and the true fraction.
std::pair<RgbImage, std::vector<float>> blurred_edge(std::size_t w, std::size_t h, float sigma) {
    const render::segment::Rgb a{0.9f, 0.8f, 0.2f}, b{0.1f, 0.2f, 0.6f};
    std::vector<float> alpha(w);
    for (std::size_t x = 0; x < w; ++x) alpha[x] = 0.5f * (1.0f - std::erf((static_cast<float>(x) + 0.5f - 40.0f) / (sigma * std::sqrt(2.0f))));
    RgbImage img(w, h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            for (std::size_t c = 0; c < 3; ++c) img.px[y * w + x][c] = a[c] * alpha[x] + b[c] * (1.0f - alpha[x]);
    return {std::move(img), std::move(alpha)};
}

}  // namespace

class TestMatting : public QObject {
    Q_OBJECT
private slots:
    // --- matting.rs ---

    void maxSquareAndBoxMeanAsTheNaive() {
        const std::size_t w = 17, h = 11;
        std::vector<float> src(w * h);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<float>((i * 37) % 23);
        for (const std::size_t r : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
            for (const bool max : {true, false}) {
                const std::vector<float> fast = matting::max_square(src, w, h, r, max);
                for (std::size_t y = 0; y < h; ++y) {
                    for (std::size_t x = 0; x < w; ++x) {
                        float v = max ? std::numeric_limits<float>::lowest() : std::numeric_limits<float>::max();
                        for (std::size_t yy = y > r ? y - r : 0; yy < std::min(y + r + 1, h); ++yy)
                            for (std::size_t xx = x > r ? x - r : 0; xx < std::min(x + r + 1, w); ++xx) v = max ? std::max(v, src[yy * w + xx]) : std::min(v, src[yy * w + xx]);
                        QCOMPARE(fast[y * w + x], v);
                    }
                }
            }
        }
        const std::size_t bw = 13, bh = 9;
        std::vector<float> b(bw * bh);
        for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<float>((i * 7) % 11);
        const std::vector<float> mean = matting::box_mean(b, bw, bh, 2);
        for (std::size_t y = 0; y < bh; ++y) {
            for (std::size_t x = 0; x < bw; ++x) {
                float s = 0, n = 0;
                for (std::size_t yy = y > 2 ? y - 2 : 0; yy < std::min(y + 3, bh); ++yy)
                    for (std::size_t xx = x > 2 ? x - 2 : 0; xx < std::min(x + 3, bw); ++xx) {
                        s += b[yy * bw + xx];
                        n += 1;
                    }
                QVERIFY(std::abs(mean[y * bw + x] - s / n) < 1e-4f);
            }
        }
    }

    void theGuidedFilterSoftensABlurredEdge() {
        const std::size_t w = 80, h = 20;
        const auto [img, truth] = blurred_edge(w, h, 3.0f);
        std::vector<float> m(w * h);
        for (std::size_t i = 0; i < m.size(); ++i) m[i] = i % w < 40 ? 1.0f : 0.0f;
        matting::RefineParams p;
        p.radius = 10;
        const std::vector<float> a = matting::refine_buffer(&img, m, w, h, p);
        const float* row = a.data() + 10 * w;
        int soft = 0;
        for (std::size_t x = 0; x < w; ++x) soft += row[x] > 0.05f && row[x] < 0.95f ? 1 : 0;
        QVERIFY2(soft >= 6, qPrintable(QString::number(soft)));
        for (std::size_t x = 0; x < w; ++x) {
            QVERIFY2(std::abs(row[x] - truth[x]) < 0.12f, qPrintable(QStringLiteral("x=%1: %2 vs %3").arg(x).arg(row[x]).arg(truth[x])));
            if (x > 0) QVERIFY(row[x] <= row[x - 1] + 0.02f);
        }
        QCOMPARE(row[5], 1.0f);  // (outside the band, untouched)
        QCOMPARE(row[75], 0.0f);
        // Smart Radius keeps a sharp edge sharp
        RgbImage sharp(w, h);
        for (std::size_t i = 0; i < sharp.px.size(); ++i) sharp.px[i] = i % w < 40 ? render::segment::Rgb{0.9f, 0.8f, 0.2f} : render::segment::Rgb{0.1f, 0.2f, 0.6f};
        p.smart_radius = true;
        const std::vector<float> s = matting::refine_buffer(&sharp, m, w, h, p);
        soft = 0;
        for (std::size_t x = 0; x < w; ++x) soft += s[10 * w + x] > 0.05f && s[10 * w + x] < 0.95f ? 1 : 0;
        QVERIFY2(soft <= 2, qPrintable(QString::number(soft)));
    }

    void featherContrastShiftAndSmooth() {
        const std::size_t w = 60, h = 10;
        std::vector<float> m(w * h);
        for (std::size_t i = 0; i < m.size(); ++i) m[i] = i % w < 30 ? 1.0f : 0.0f;
        matting::RefineParams p;
        p.feather = 6;
        const std::vector<float> f = matting::refine_buffer(nullptr, m, w, h, p);
        QVERIFY(f[5 * w + 29] > 0.5f && f[5 * w + 29] < 0.8f && f[5 * w + 31] > 0.1f);
        p = {};
        p.contrast = 100;
        const std::vector<float> c = matting::refine_buffer(nullptr, f, w, h, p);
        QVERIFY(c[5 * w + 29] == 1.0f && c[5 * w + 31] == 0.0f);
        p = {};
        p.shift_edge = 100;
        const std::vector<float> grown = matting::refine_buffer(nullptr, m, w, h, p);
        QCOMPARE(grown[5 * w + 33], 1.0f);
        QCOMPARE(grown[5 * w + 34], 0.0f);
        p.shift_edge = -50;
        const std::vector<float> shrunk = matting::refine_buffer(nullptr, m, w, h, p);
        QCOMPARE(shrunk[5 * w + 27], 1.0f);
        QCOMPARE(shrunk[5 * w + 28], 0.0f);
        p = {};
        p.smooth = 50;
        const std::vector<float> sm = matting::refine_buffer(nullptr, m, w, h, p);
        QVERIFY(sm[5 * w + 10] == 1.0f && sm[5 * w + 50] == 0.0f);
    }

    void theTilesGiveWhatOneBufferGives() {
        const std::size_t w = 600, h = 300;
        const auto [img, truth] = blurred_edge(w, h, 2.0f);
        const render::segment::ImageSampler s(img);
        std::vector<float> m(w * h);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const float dx = static_cast<float>(x) - 40, dy = static_cast<float>(y) - 150;
                m[y * w + x] = dx * dx * 0.05f + dy * dy < 120.0f * 120.0f || x < 40 ? 1.0f : 0.0f;
            }
        }
        matting::RefineParams p;
        p.radius = 6;
        p.feather = 2;
        p.contrast = 10;
        const Rect canvas{0, 0, static_cast<int>(w), static_cast<int>(h)};
        const auto out = matting::refine_mask(s, matting::buffer_reader(m, w, h), canvas, canvas, p);
        QVERIFY(out);
        const std::vector<float> full = matting::refine_buffer(&img, m, w, h, p);
        float maxd = 0;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) maxd = std::max(maxd, std::abs(out->at(static_cast<int>(x), static_cast<int>(y)) - full[y * w + x]));
        QVERIFY2(maxd <= 1.5f / 255.0f, qPrintable(QString::number(maxd)));
    }

    void decontaminateAndDefringe() {
        // red on the left, blue on the right, a mixed fringe pixel at x = 10
        const std::size_t w = 20, h = 8;
        std::vector<render::segment::Rgba> px(w * h);
        std::vector<float> matte(w * h);
        for (std::size_t i = 0; i < px.size(); ++i) {
            const std::size_t x = i % w;
            px[i] = x < 10 ? render::segment::Rgba{1, 0, 0, 1} : x == 10 ? render::segment::Rgba{0.5f, 0, 0.5f, 1} : render::segment::Rgba{0, 0, 1, 1};
            matte[i] = x < 10 ? 1.0f : x == 10 ? 128.0f / 255.0f : 0.0f;
        }
        const auto before = px;
        matting::decontaminate(px, w, h, matte, 3, 100);
        QVERIFY(px[4 * w + 10][0] > 0.9f && px[4 * w + 10][2] < 0.1f);
        QVERIFY(px[4 * w + 15] == before[4 * w + 15]);
        QCOMPARE(px[4 * w + 10][3], 1.0f);
        // defringe: transparent, a blue fringe, the red core
        std::vector<render::segment::Rgba> row{{0, 0, 0, 0}, {0, 0, 1, 0.5f}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
        matting::defringe(row, 5, 1, 1);
        QVERIFY(row[1] == (render::segment::Rgba{1, 0, 0, 0.5f}));
        QCOMPARE(row[0][3], 0.0f);
        // remove the black matte: a colour over black unpremultiplied
        std::vector<render::segment::Rgba> over{{0.45f, 0.05f, 0.05f, 0.5f}, {0.9f, 0.1f, 0.1f, 1}};
        matting::remove_matte(over, false);
        QVERIFY(std::abs(over[0][0] - 0.9f) < 1e-5f && over[1][0] == 0.9f);
    }

    // --- the op matting ---

    void theOpOnEachKindOfLayer() {
        for (const char* mode : {"8", "16", "cmyk"}) {
            core::Document doc = book(dark_disc);
            if (std::string(mode) == "16") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
            if (std::string(mode) == "cmyk") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
            int ex = -1;
            for (int x = 40; x < 100 && ex < 0; ++x) {
                const float a = pixel(doc, x, 75)[3];
                if (a > 0.2f && a < 0.8f) ex = x;
            }
            QVERIFY2(ex >= 0, mode);
            const auto before = pixel(doc, ex, 75);
            const Json op{{"op", "matting"}, {"page", 1}, {"id", "paint"}};
            Json black = op;
            black["kind"] = "remove_black";
            const core::Document unblacked = run(doc, black);
            auto after = pixel(unblacked, ex, 75);
            QVERIFY2(after[0] > before[0] + 0.05f, qPrintable(QStringLiteral("%1: %2 → %3").arg(mode).arg(before[0]).arg(after[0])));
            QVERIFY2(std::abs(after[3] - before[3]) < 1e-2f, mode);
            QCOMPARE(pixel(unblacked, 100, 75), pixel(doc, 100, 75));  // (the opaque core as it was)
            Json fringe = op;
            fringe["kind"] = "defringe";
            fringe["width_mm"] = mm(4);
            after = pixel(run(doc, fringe), ex, 75);
            QVERIFY2(after[0] > 0.8f, qPrintable(QStringLiteral("%1: %2").arg(mode).arg(after[0])));
            Json clean = op;
            clean["kind"] = "decontaminate";
            clean["amount"] = 100;
            after = pixel(run(doc, clean), ex, 75);
            QVERIFY2(after[0] > before[0], qPrintable(QStringLiteral("%1: %2 → %3").arg(mode).arg(before[0]).arg(after[0])));
            Json white = op;
            white["kind"] = "remove_white";
            (void)run(doc, white);
            // weighed by an area: the right half alone
            black["area"] = Json{{"rect", Json::array({mm(100), 0, mm(100), kInch})}};
            const core::Document half = run(doc, black);
            QCOMPARE(pixel(half, ex, 75), before);
            const int other = 200 - ex;
            QVERIFY(pixel(half, other, 75)[0] > pixel(doc, other, 75)[0] + 0.05f);
        }
    }

    void theOpsRefusals() {
        const core::Document doc = book(dark_disc);
        QVERIFY(refusal(doc, Json{{"op", "matting"}, {"page", 1}, {"id", "paint"}, {"kind", "blur"}}).find("kind is defringe") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "matting"}, {"page", 1}, {"kind", "defringe"}}).find("id (the layer) is required") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "matting"}, {"page", 1}, {"id", "paint"}, {"kind", "defringe"}, {"width_mm", 0}}).find("width_mm") != std::string::npos);
        QVERIFY(!refusal(doc, Json{{"op", "matting"}, {"page", 1}, {"id", "nothing"}, {"kind", "defringe"}}).empty());
        // nothing to change: done, the layer's bytes as they were
        const core::Document opaque = book([](int, int) { return std::array<double, 4>{0.2, 0.4, 0.6, 1.0}; });
        const core::ApplyResult none = run_ops(opaque, Json::array({Json{{"op", "matting"}, {"page", 1}, {"id", "paint"}, {"kind", "remove_black"}}}));
        QVERIFY(*layer_of(none.doc, "paint").raster_png == *layer_of(opaque, "paint").raster_png);
    }

    // --- the area kinds through render/selection ---

    void theKindsOnAPage() {
        const core::Document doc = book(disc_scene);
        // subject: the disc
        const render::Image subject = mask(doc, Json{{"subject", Json::object()}});
        QVERIFY2(iou_truth(subject, in_disc) > 0.9, qPrintable(QString::number(iou_truth(subject, in_disc))));
        QCOMPARE(iou(mask(doc, Json{{"subject", true}}), subject), 1.0);  // (true: no settings)
        // object: a rectangle around it
        const render::Image object = mask(doc, Json{{"object", Json{{"rect_mm", Json::array({mm(35), mm(35), mm(130), mm(130)})}}}});
        QVERIFY2(iou_truth(object, in_disc) > 0.9, qPrintable(QString::number(iou_truth(object, in_disc))));
        // quick: a stroke inside grows over the disc, not past it; the same at the selection's grid and a finer one
        const Json stroke{{"quick", Json{{"points_mm", Json::array({Json::array({mm(80), mm(100)}), Json::array({mm(120), mm(100)})})}, {"size_mm", mm(10)}}}};
        const render::Image quick = mask(doc, stroke);
        QVERIFY2(iou_truth(quick, in_disc) > 0.85, qPrintable(QString::number(iou_truth(quick, in_disc))));
        QCOMPARE(at(quick, 10, 10), 0);
        const render::Image fine = mask(doc, stroke, 400);
        QCOMPARE(fine.width(), 400);
        QVERIFY2(iou(fine.resize(render::Size{200, 200}, render::Resample::Bilinear), quick) > 0.9, "the finer grid");
        Json enhanced = stroke;
        enhanced["quick"]["enhance_edge"] = true;
        QVERIFY(iou_truth(mask(doc, enhanced), in_disc) > 0.85);
        // refine: the disc's hard selection softened along the picture's edge, then shifted out
        const Json hard{{"ellipse", Json::array({mm(45), mm(45), mm(110), mm(110)})}};
        const render::Image refined = mask(doc, Json{{"refine", Json{{"area", hard}, {"radius_mm", mm(4)}, {"feather_mm", mm(1)}}}});
        QCOMPARE(at(refined, 100, 100), 255);
        QCOMPARE(at(refined, 5, 5), 0);
        int soft = 0;
        for (int x = 30; x < 60; ++x) soft += at(refined, x, 100) > 10 && at(refined, x, 100) < 245 ? 1 : 0;
        QVERIFY2(soft >= 2, qPrintable(QString::number(soft)));
        const render::Image shifted = mask(doc, Json{{"refine", Json{{"area", hard}, {"shift_edge", 100}}}});
        QCOMPARE(at(shifted, 44, 100), 255);
        // focus: no edges left of the noise → found or not, a mask of the page's size
        QCOMPARE(mask(doc, Json{{"focus", Json{{"range", 0.5}}}}).size(), (render::Size{200, 200}));
        // sky: none on ground alone (the area is empty), and on a page of sky over ground
        const core::Document ground = book([](int x, int y) {
            const double n = 0.15 * std::sin(x * 12.9898 + y * 78.233) * std::cos(x * 4.1 - y * 2.7);
            return std::array<double, 4>{0.35 + n, 0.3 + n, 0.12, 1.0};
        });
        QVERIFY(!render::selection::from_mask(mask(ground, Json{{"sky", Json::object()}})));
        const core::Document outside = book([](int, int y) {
            return y < 90 ? std::array<double, 4>{0.4 + y * 0.002, 0.6, 0.95, 1.0} : std::array<double, 4>{0.35, 0.3, 0.12, 1.0};
        });
        const render::Image sky = mask(outside, Json{{"sky", Json{{"threshold", 50}, {"softness", 20}}}});
        QVERIFY(at(sky, 100, 20) > 230 && at(sky, 100, 150) < 25);
        // a layer of its own; refusals
        QVERIFY(iou(mask(doc, Json{{"subject", Json{{"layer", "paint"}}}}), subject) > 0.95);
        for (const Json& bad : {Json{{"subject", Json{{"layer", "nothing"}}}}, Json{{"subject", Json{{"blur", 1}}}}, Json{{"object", Json{{"rect_mm", Json::array({1, 2})}}}},
                                Json{{"quick", Json{{"points_mm", Json::array()}}}}, Json{{"refine", Json{{"radius_mm", 1}}}},
                                Json{{"quick", Json{{"points_mm", Json::array({Json::array({1, 1})})}, {"size_mm", 0}}}}}) {
            bool refused = false;
            try {
                (void)mask(doc, bad);
            } catch (const core::OpError&) {
                refused = true;
            }
            QVERIFY2(refused, bad.dump().c_str());
        }
        // given to an op: すべてのオブジェクトをマスク (set_layer_mask with the subject)
        const core::Document masked = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", Json{{"subject", Json{{"layer", "paint"}}}}}});
        QVERIFY(layer_of(masked, "paint").mask);
    }

    // The analysis is RGB: an 8-bit, a 16-bit and a CMYK page of one picture give the same areas.
    void theSameAreaOnEachKindOfPage() {
        const core::Document eight = book(disc_scene);
        const core::Document deep = run(eight, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        const core::Document cmyk = run(eight, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
        QVERIFY(layer_of(deep, "paint").color_raster);
        QVERIFY(layer_of(cmyk, "paint").ink_raster);
        for (const Json& area : {Json{{"subject", Json::object()}}, Json{{"object", Json{{"rect_mm", Json::array({mm(35), mm(35), mm(130), mm(130)})}}}},
                                 Json{{"quick", Json{{"points_mm", Json::array({Json::array({mm(80), mm(100)})})}, {"size_mm", mm(10)}}}},
                                 Json{{"refine", Json{{"area", Json{{"ellipse", Json::array({mm(45), mm(45), mm(110), mm(110)})}}}, {"radius_mm", mm(4)}}}}}) {
            const render::Image a = mask(eight, area), b = mask(deep, area), c = mask(cmyk, area);
            QVERIFY2(iou(a, b) > 0.98, qPrintable(QStringLiteral("16 bit %1: %2").arg(QString::fromStdString(area.dump())).arg(iou(a, b))));
            QVERIFY2(iou(a, c) > 0.95, qPrintable(QStringLiteral("CMYK %1: %2").arg(QString::fromStdString(area.dump())).arg(iou(a, c))));
        }
    }
};

QTEST_GUILESS_MAIN(TestMatting)
#include "test_matting.moc"
