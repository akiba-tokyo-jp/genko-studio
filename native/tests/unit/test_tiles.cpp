// PC5 H-a (SPEC VIEW-01): the page's tiles on the screen — 画素の段 (tiles of the page's own pixels, laid nearest: each a
// square, its edges where the pixel grid is), ピクセルアートプレビュー (every level nearest), 32 bit プレビューオプション on the
// three ways a page composites (an 8-bit page untouched; a 32-bit RGB page's values, a value past 1 brought back by a
// negative exposure, the highlights compressed; a 32-bit grey, CMYK page's inks), and the tiles counted on the graphics
// card and let go past its budget (render:GPU-CPU-parity's memory).

#include <QtTest>

#include <QPainter>

#include <cmath>
#include <set>

#include "app/tiles.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/exposure.hpp"
#include "core/model.hpp"
#include "render/color_canvas.hpp"
#include "render/hdr_preview.hpp"
#include "render/ink.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;

namespace {

core::Document run_ops(const core::Document& doc, const Json& ops) {
    try {
        return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
    } catch (const core::ApplyError& error) {  // (said, the test failing on it)
        qWarning("%s: %s", error.code().c_str(), error.what());
        throw;
    }
}

core::Document small_book(double w = 60, double h = 80, int dpi = 150, const char* expression = "color") {
    core::Document doc = core::new_episode("tiles", core::Num(1), 1, core::PageSpec::custom(w, h, w - 10, h - 10, 3, 5, 5, 5, 5, dpi, expression));
    doc.edit_page(0).numero = false;
    return doc;
}

// A paint layer of 8 × 8 squares of two colours across the page (its pixels: the page's working grid).
void add_checks(core::Document& doc) {
    auto& page = doc.edit_page(0);
    core::Layer paint;
    paint.id = "checks";
    paint.kind = core::LayerKind::Raster;
    paint.panel_clip = false;
    const render::Image blank = render::raster::ensure_raster(page, paint);
    std::string bytes = blank.tobytes();
    for (int y = 0; y < blank.height(); ++y) {
        for (int x = 0; x < blank.width(); ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(blank.width()) + std::size_t(x)) * 4;
            const bool dark = ((x / 8) + (y / 8)) % 2 == 0;
            bytes[i] = static_cast<char>(dark ? 30 : 230);
            bytes[i + 1] = static_cast<char>(dark ? 60 : 200);
            bytes[i + 2] = static_cast<char>(dark ? 120 : 40);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    render::raster::save_raster(page, paint, render::Image::frombytes("RGBA", blank.size(), bytes));
    page.layers.push_back(std::move(paint));
}

// The renderer's tiles of the whole page at dpi, drawn and composed.
QImage composed(app::PageRenderer& renderer, const std::shared_ptr<const core::Document>& doc, int dpi) {
    renderer.show(doc, 0);
    const core::Page& page = doc->page(0);
    renderer.want(dpi, dpi, QRectF(0, 0, page.spec.width_mm.value(), page.spec.height_mm.value()));
    [&] { QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000); }();
    return renderer.compose(dpi);
}

// The page's tiles painted at `zoom` screen pixels a page pixel of `dpi` over its top left (`px` × `px` screen pixels).
QImage painted(const app::PageRenderer& renderer, int dpi, double zoom, int px) {
    QImage out(px, px, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const double scale = zoom * dpi / 25.4;  // (screen px a mm)
    painter.setTransform(QTransform::fromScale(scale, scale));
    renderer.paint(painter, QRectF(0, 0, px / scale, px / scale));
    painter.end();
    return out;
}

// Each block of n × n screen pixels from the corner is one colour.
bool blocks_whole(const QImage& image, int n) {
    for (int y = 0; y + n <= image.height(); y += n) {
        for (int x = 0; x + n <= image.width(); x += n) {
            const QRgb first = image.pixel(x, y);
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    if (image.pixel(x + i, y + j) != first) return false;
                }
            }
        }
    }
    return true;
}

std::size_t colours(const QImage& image) {
    std::set<QRgb> seen;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) seen.insert(image.pixel(x, y));
    }
    return seen.size();
}

int byte(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

}  // namespace

class TestTiles : public QObject {
    Q_OBJECT

private slots:
    // The preview's arithmetic: 露光量 × 2^stops and ^(1/gamma) in linear light, not cut; ハイライト圧縮 L / (1 + L); the
    // ranges of Photoshop's window; 0 and 1 change nothing.
    void thePreviewsArithmetic() {
        render::HdrPreview p;
        QVERIFY(p.identity());
        double rgb[3] = {2.0, 0.5, -0.1};
        render::hdr_linear(p, rgb);
        QCOMPARE(rgb[0], 2.0);
        p.exposure = -1;
        p.gamma = 2;
        QVERIFY(!p.identity());
        double c[3] = {2.0, 0.5, -0.1};
        render::hdr_linear(p, c);
        QCOMPARE(c[0], 1.0);
        QCOMPARE(c[1], std::sqrt(0.25));
        QCOMPARE(c[2], 0.0);
        render::HdrPreview h;
        h.highlight_compression = true;
        h.exposure = 5;  // (not taken)
        QCOMPARE(render::hdr_grey(h, 3.0), 0.75);
        double colour[3] = {1.0, 0.0, 0.0};
        render::hdr_linear(h, colour);
        QVERIFY(std::abs(colour[0] - 1.0 / 1.2126) < 1e-12 && colour[1] == 0.0);
        render::HdrPreview wide;
        wide.exposure = 99;
        wide.gamma = 0;
        QCOMPARE(wide.clamped().exposure, 20.0);
        QCOMPARE(wide.clamped().gamma, 0.1);
        wide.exposure = std::nan("");
        QCOMPARE(wide.clamped().exposure, 0.0);
    }

    // 画素の段: tiles at the page's own resolution are its pixels (the page as render_page draws it there), laid on the
    // screen enlarged as squares — 4 × 4 screen pixels a page pixel at 400 % — though the painter smooths; under the page's
    // resolution (another level enlarged) they are smoothed as before, unless ピクセルアートプレビュー.
    void thePagesPixelsAreLaidAsSquares() {
        core::Document doc = small_book();
        add_checks(doc);
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        const QImage tiles = composed(renderer, shared, 150);
        QCOMPARE(renderer.pixel_dpi(), 150);
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        const render::Image page = render::render_page(shared->page(0), 150, options, shared.get()).image;
        const std::string bytes = page.tobytes();
        QCOMPARE(tiles, QImage(reinterpret_cast<const uchar*>(bytes.data()), page.width(), page.height(), page.width() * 3, QImage::Format_RGB888)
                            .convertToFormat(QImage::Format_RGB32));
        const QImage big = painted(renderer, 150, 4.0, 256);
        QVERIFY(blocks_whole(big, 4));  // (nearest: each page pixel a 4 × 4 square, its edges on the grid)
        QCOMPARE(big.pixel(1, 1), tiles.pixel(0, 0));
        QCOMPARE(big.pixel(4 * 17 + 2, 4 * 9 + 1), tiles.pixel(17, 9));
        // a level under the page's resolution enlarged: smoothed (more colours than the page has), but for the pixel art preview
        app::PageRenderer low;
        (void)composed(low, shared, 72);
        const QImage smooth = painted(low, 72, 4.0, 256);
        QVERIFY(!blocks_whole(smooth, 4));
        QVERIFY(colours(smooth) > colours(painted(renderer, 150, 4.0, 256)));
        low.set_pixel_art(true);
        QVERIFY(blocks_whole(painted(low, 72, 4.0, 256), 4));
    }

    // 32 bit プレビューオプション, the three ways: an 8-bit page shows as it is; a 32-bit RGB page's values — 1.5 (past white:
    // shown white) back under it with −2 stops, the highlights compressed L / (1 + L) — as render/hdr_preview shows them; a
    // 32-bit CMYK page's inks, its colours as shown through the preview (paper white darkened by a negative exposure).
    void thirtyTwoBitPreviewOnTheThreeWays() {
        render::HdrPreview darker;
        darker.exposure = -2;
        // the 8-bit page
        {
            core::Document doc = small_book();
            add_checks(doc);
            const auto shared = std::make_shared<const core::Document>(doc);
            app::PageRenderer renderer;
            const QImage plain = composed(renderer, shared, 72);
            renderer.set_hdr(darker);
            QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
            QCOMPARE(renderer.compose(72), plain);
        }
        // the 32-bit RGB page: a precise layer of 1.5 (left) and 0.25 (right)
        core::Document doc = run_ops(small_book(), Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
                                                                       {"pixels", Json::array({1.5, 1.5, 1.5, 1.0, 0.25, 0.25, 0.25, 1.0})}},
                                                                 Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}}));
        QVERIFY(render::hdr_page(doc.page(0)));
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        const QImage plain = composed(renderer, shared, 72);
        const QPoint left(45, 100), right(125, 100);
        QCOMPARE(qRed(plain.pixel(left)), 255);  // (cut to white)
        renderer.set_hdr(darker);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        const QImage shown = renderer.compose(72);
        // (the page's values there, as render_page gives them: the 2 pixels stretched over the page)
        render::RenderOptions values_options;
        values_options.mode = "proof";
        values_options.skip_unported = true;
        values_options.precise = true;
        const render::RenderResult values = render::render_page(doc.page(0), 72, values_options, &doc);
        QVERIFY(values.precise.has_value());
        const core::ColorRasterView view(*values.precise);
        const auto value = [&view](const QPoint& p) { return view.pixel(std::size_t(p.y()) * view.width() + std::size_t(p.x()))[0]; };
        QVERIFY(value(left) > 1.4);
        const render::RgbSpace srgb;
        const int back = byte(srgb.encode(0, srgb.decode(0, value(left)) * 0.25));
        QVERIFY2(back < 250, qPrintable(QString::number(back)));
        QCOMPARE(qRed(shown.pixel(left)), back);  // (what was past white, back under it)
        QCOMPARE(qRed(shown.pixel(right)), byte(srgb.encode(0, srgb.decode(0, value(right)) * 0.25)));
        render::HdrPreview compress;
        compress.highlight_compression = true;
        renderer.set_hdr(compress);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        const double l = srgb.decode(0, value(left));
        QCOMPARE(qRed(renderer.compose(72).pixel(left)), byte(srgb.encode(0, l / (1 + l))));
        // back to 0 and 1: as before
        renderer.set_hdr(render::HdrPreview{});
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        QCOMPARE(renderer.compose(72), plain);
        // the 32-bit CMYK page: its inks shown through the preview
        core::Document cmyk = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 32}}}));
        QVERIFY(cmyk.page(0).color && render::hdr_page(cmyk.page(0)));
        const auto inked = std::make_shared<const core::Document>(cmyk);
        app::PageRenderer inks;
        const QImage paper = composed(inks, inked, 72);
        inks.set_hdr(darker);
        QTRY_VERIFY_WITH_TIMEOUT(inks.settled(), 30000);
        const QImage dimmed = inks.compose(72);
        const QRgb was = paper.pixel(100, 100), now = dimmed.pixel(100, 100);
        QVERIFY(qRed(now) < qRed(was));
        QVERIFY(std::abs(qRed(now) - byte(core::linear_to_srgb(core::srgb_to_linear(qRed(was) / 255.0) * 0.25))) <= 1);
    }

    // The grey and the CMYK ways of render/hdr_preview on their inks alone: a grey's lightness past white (any value at
    // 32 bits) back under it through its tone curve; a pixel drawn over the page (not what its inks show) left as drawn.
    void inksThroughThePreview() {
        // (a grey page's colour as set_color_mode makes it: its profile held)
        const core::Document grey_book = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 32}}}));
        QVERIFY(grey_book.page(0).color && grey_book.page(0).color->space == core::InkSpace::Gray);
        const core::PageColor grey = *grey_book.page(0).color;
        core::InkPixels pixels;
        pixels.space = core::InkSpace::Gray;
        pixels.depth = 32;
        pixels.width = 3;
        pixels.height = 1;
        pixels.floats = {1.6f, 1.0f, 1.0f, 1.0f, 0.5f, 1.0f};  // (lightness, alpha: past white, white, a grey drawn over)
        render::HdrPreview p;
        p.exposure = -2;
        // (the page as shown: its lightness cut; the third pixel drawn over in red)
        const std::string plain_bytes = std::string{char(255), char(255), char(255), char(255), char(255), char(255), char(255), char(0), char(0)};
        const render::Image drawn = render::Image::frombytes("RGB", render::Size{3, 1}, plain_bytes);
        const render::Image shown = render::hdr_shown(drawn, pixels, grey, p);
        const std::string out = shown.tobytes();
        // (the lightness through the page's tone curve, shown as the page shows a lightness: its ink 1 − L to the screen)
        const render::GrayCurve curve(grey);
        const auto shown_of = [&grey](double light) {
            const double ink = 1.0 - std::clamp(light, 0.0, 1.0);
            float srgb[3];
            render::ink::to_srgb(grey, &ink, srgb, 1);
            return byte(srgb[0]);
        };
        QCOMPARE(int(static_cast<unsigned char>(out[0])), shown_of(curve.encode(curve.decode(1.6) * 0.25)));
        QVERIFY(static_cast<unsigned char>(out[0]) < 250);
        QCOMPARE(int(static_cast<unsigned char>(out[3])), shown_of(curve.encode(curve.decode(1.0) * 0.25)));
        QVERIFY(static_cast<unsigned char>(out[3]) < static_cast<unsigned char>(out[0]));
        QCOMPARE(int(static_cast<unsigned char>(out[6])), 255);  // (drawn over: as drawn)
        QCOMPARE(int(static_cast<unsigned char>(out[7])), 0);
        QCOMPARE(render::hdr_shown(drawn, pixels, grey, render::HdrPreview{}).tobytes(), plain_bytes);  // (0 and 1: nothing)
    }

    // (review P3-4) 32 bit プレビュー leaves what is drawn over the page as drawn — a white balloon on a white page stays
    // white while the page round it darkens — by render_page's marks (RenderOptions::drawn_over), not by its colours.
    void whatIsDrawnOverStaysAsDrawn() {
        const core::Document doc = run_ops(small_book(), Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1},
                                                                            {"precision", "f32"}, {"pixels", Json::array({1.0, 1.0, 1.0, 1.0})}},
                                                                       Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}},
                                                                       Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}, {"x_mm", 30}, {"y_mm", 40}}}));
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        options.precise = true;
        options.drawn_over = true;
        const render::RenderResult marked = render::render_page(doc.page(0), 72, options, &doc);
        QVERIFY(marked.drawn_over.has_value());
        QCOMPARE(marked.drawn_over->size(), std::size_t(marked.image.width()) * std::size_t(marked.image.height()));
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        const QImage plain = composed(renderer, shared, 72);
        render::HdrPreview darker;
        darker.exposure = -2;
        renderer.set_hdr(darker);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        const QImage shown = renderer.compose(72);
        int kept_white = 0, darkened = 0;
        for (int y = 0; y < shown.height(); ++y) {
            for (int x = 0; x < shown.width(); ++x) {
                const bool over = (*marked.drawn_over)[std::size_t(y) * shown.width() + x] != 0;
                if (over && qRed(plain.pixel(x, y)) == 255 && qRed(shown.pixel(x, y)) == 255) ++kept_white;
                if (!over && qRed(plain.pixel(x, y)) == 255 && qRed(shown.pixel(x, y)) < 200) ++darkened;
                if (over) QCOMPARE(shown.pixel(x, y), plain.pixel(x, y));  // (drawn over: as drawn)
            }
        }
        QVERIFY2(kept_white > 50, qPrintable(QString::number(kept_white)));  // (the balloon's white inside)
        QVERIFY(darkened > 1000);  // (the page's white round it)
    }

    // (review P2-3) The finer levels keep at most their budget: zoomed in and moved across a large page, the tiles out of
    // sight the longest are let go — the ones in sight stay drawn and current —, drawn again when they come back.
    void theFinerTilesKeepToTheirBudget() {
        core::Document doc = small_book(400, 400, 150, "mono");
        add_checks(doc);
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        renderer.show(shared, 0);
        const std::size_t tile = std::size_t(512) * 512 * 4;
        renderer.set_fine_budget(3 * tile);
        for (int step = 0; step < 6; ++step) {  // (a 40 mm view at 150 dpi moved across: about one tile and its neighbours)
            const QRectF seen(step * 60.0, step * 60.0, 40, 40);
            renderer.want(72, 150, seen);
            QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
            QVERIFY(renderer.current(150, seen));
            QVERIFY2(renderer.fine_bytes() <= 3 * tile + 9 * tile, qPrintable(QString::number(renderer.fine_bytes())));  // (the wanted ones — at most 3 × 3 round the view — kept)
        }
        QVERIFY(renderer.fine_released() > 0);
        // back where it began: drawn again, the same pixels as the page
        renderer.want(72, 150, QRectF(0, 0, 40, 40));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        QVERIFY(renderer.current(150, QRectF(0, 0, 40, 40)));
        QCOMPARE(app::PageRenderer::kFineBytes, std::size_t(256) * 1024 * 1024);
    }

    // (review P2-3) A page with a correction that moves shapes is drawn whole for any part of it: the screen's tiles draw it
    // once a level and cut each from it — the same pixels as render_page's parts.
    void aWholePageIsDrawnOnceALevel() {
        core::Document doc = small_book(120, 120, 150, "mono");  // (709 px a side: 4 tiles at 150 dpi)
        add_checks(doc);
        core::Layer blur;
        blur.id = "adj";
        blur.role = core::LayerRole::User;
        blur.kind = core::LayerKind::Adjust;
        blur.adjust = Json{{"kind", "blur"}, {"radius", 2}};
        doc.edit_page(0).layers.push_back(std::move(blur));
        QVERIFY(render::draws_whole_page(doc.page(0)));
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        const QImage tiles = composed(renderer, shared, 150);
        QCOMPARE(renderer.wholes_drawn(), std::size_t{1});  // (4 tiles, one page drawn)
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        const render::Image page = render::render_page(shared->page(0), 150, options, shared.get()).image;
        const std::string bytes = page.tobytes();
        QCOMPARE(tiles, QImage(reinterpret_cast<const uchar*>(bytes.data()), page.width(), page.height(), page.width() * 3, QImage::Format_RGB888)
                            .convertToFormat(QImage::Format_RGB32));
    }

    // GPU の記憶: with a budget the tiles drawn are counted as on the card; past the budget, after a frame, the ones drawn
    // longest ago are let go (their pictures copied — the same pixels — so the card drops their textures), never the frame's
    // own; no budget, nothing counted.
    void theTilesOnTheCardKeepToTheBudget() {
        core::Document doc = small_book(400, 400, 150, "mono");  // (2362 px a side: 5 × 5 tiles of 1 MiB)
        const auto shared = std::make_shared<const core::Document>(doc);
        app::PageRenderer renderer;
        const QImage whole = composed(renderer, shared, 150);
        QCOMPARE(renderer.texture_bytes(), std::size_t{0});
        const std::size_t tile = std::size_t(512) * 512 * 4;
        renderer.set_texture_budget(4 * tile);
        // a frame of the whole page: all 25 drawn — over the budget, but each is the frame's own
        QImage frame(400, 400, QImage::Format_RGB32);
        {
            QPainter painter(&frame);
            painter.setTransform(QTransform::fromScale(1.0, 1.0));
            renderer.paint(painter, QRectF(0, 0, 400, 400));
        }
        QCOMPARE(renderer.texture_bytes(), std::size_t(2362) * 2362 * 4);  // (every tile, the edge ones smaller)
        renderer.trim_textures();
        QCOMPARE(renderer.textures_released(), std::size_t{0});
        // the next frame: one tile in sight — the others go, the oldest first, down to the budget
        {
            QPainter painter(&frame);
            renderer.paint(painter, QRectF(0, 0, 20, 20));
        }
        renderer.trim_textures();
        QVERIFY(renderer.texture_bytes() <= 4 * tile && renderer.texture_bytes() >= tile);  // (the frame's own kept)
        QVERIFY(renderer.textures_released() >= 15);
        QCOMPARE(renderer.compose(150), whole);  // (the same pixels)
        // a tile drawn again after it changed is counted anew; no budget: nothing counted
        renderer.set_texture_budget(std::nullopt);
        QCOMPARE(renderer.texture_bytes(), std::size_t{0});
        {
            QPainter painter(&frame);
            renderer.paint(painter, QRectF(0, 0, 400, 400));
        }
        QCOMPARE(renderer.texture_bytes(), std::size_t{0});
    }
};

QTEST_MAIN(TestTiles)
#include "test_tiles.moc"
