// render_page without the Python reference: what is not drawn yet stops the render (NotYetPorted) unless
// skip_unported, a stop request cancels it, renders on several threads at once give the same pixels, parts of the
// page are the same as the whole page cut, and the remembered lines are used (rough_needed).

#include <QtTest>

#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

namespace render = genko::render;
using genko::core::Document;
using genko::core::Json;
using genko::core::Layer;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Num;

namespace {

genko::core::StrokePtr line(double x, double y, double dx, double dy, std::string kind = "gpen") {
    auto s = std::make_shared<genko::core::Stroke>();
    s->id = genko::core::new_id();
    for (int i = 0; i < 12; ++i) s->points.push_back({x + dx * i, y + dy * i * (i % 3 == 0 ? 1.0 : 0.5)});
    for (int i = 0; i < 12; ++i) s->pressure.push_back(0.2 + 0.06 * i);
    s->kind = std::move(kind);
    s->width_mm = 0.8;
    return s;
}

// A small book: split panels, ink lines, an effect on a layer, a paper colour; nothing that is not drawn yet.
Document book() {
    Document doc = genko::core::new_episode("試験", Num(1), 2, genko::core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6));
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        genko::core::Page& page = doc.edit_page(p);
        page.numero = false;
        page.split_frame(page.frames[0].id, "horizontal", Num(0.4), Num(3));
        page.extra["paper_rgb"] = Json::array({250, 248, 240});
        for (auto& layer : page.layers) {
            if (layer.role != LayerRole::Ink) continue;
            std::vector<genko::core::StrokePtr> items;
            const char* kinds[] = {"gpen", "pencil", "fude", "spray", "water", "calligraphy"};
            for (int i = 0; i < 24; ++i) items.push_back(line(5.0 + 2.1 * i, 8.0 + 3.0 * (i % 7), 1.7, 2.3, kinds[i % 6]));
            layer.strokes = genko::core::make_strokes(std::move(items));
            layer.effect = Json::object({{"border", Json::object({{"width_mm", 0.6}, {"rgb", Json::array({250, 20, 20})}})},
                                         {"water_edge", Json::object({{"width_mm", 0.8}})}});
        }
    }
    return doc;
}

std::string unported_element(const genko::core::Page& page, const Document& doc, render::RenderOptions options) {
    try {
        (void)render::render_page(page, 72, options, &doc);
    } catch (const render::NotYetPorted& e) {
        return e.element();
    }
    return {};
}

}  // namespace

class TestRenderPage : public QObject {
    Q_OBJECT

private slots:
    void init() { render::clear_render_caches(); }

    void not_yet_ported_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("mode");
        for (const char* what : {"balloons", "nombre", "tones", "effects", "prims", "covers", "anim", "screen", "placed"}) {
            const char* mode = std::string(what) == "prims" || std::string(what) == "covers" ? "proof" : "print";
            QTest::newRow(what) << QString(what) << QString(mode);
        }
    }

    void not_yet_ported() {
        QFETCH(QString, what);
        QFETCH(QString, mode);
        Document doc = book();
        genko::core::Page& page = doc.edit_page(0);
        const std::string w = what.toStdString();
        if (w == "balloons") doc.add_line(page.index, "台詞", "A", std::nullopt, "", Num(10), Num(12));
        if (w == "nombre") page.numero = true;
        if (w == "tones") {
            Layer tone;
            tone.id = genko::core::new_id();
            tone.role = LayerRole::Tone;
            tone.kind = LayerKind::Tone;
            page.layers.push_back(tone);
        }
        if (w == "effects") page.effects = Json::array({Json::object({{"kind", "speed"}})});
        if (w == "prims") page.prims = Json::array({Json::object({{"kind", "cube"}})});
        if (w == "covers") page.extra["cover"] = Json::object({{"kind", "jacket"}, {"spine_mm", 5}, {"flap_mm", 10}});
        if (w == "anim") page.extra["anim"] = Json::object({{"fps", 12}, {"tracks", Json::array()}});
        if (w == "screen") {  // (on a layer with lines: an empty layer is never screened)
            for (auto& layer : page.layers) {
                if (layer.role == LayerRole::Ink) layer.screen = Json::object({{"pattern", "dot"}, {"lpi", 60}});
            }
        }
        if (w == "placed") {
            Layer placed;
            placed.id = genko::core::new_id();
            placed.kind = LayerKind::Placed;
            page.layers.push_back(placed);
        }
        render::RenderOptions options;
        options.mode = mode.toStdString();
        QCOMPARE(unported_element(*doc.pages[0], doc, options), w);
        options.skip_unported = true;
        const render::RenderResult r = render::render_page(*doc.pages[0], 72, options, &doc);
        QVERIFY(std::find(r.omitted.begin(), r.omitted.end(), w) != r.omitted.end());
        // the other page has none of it
        options.skip_unported = false;
        QVERIFY(unported_element(*doc.pages[1], doc, options).empty());
    }

    void unported_only_where_drawn() {
        Document doc = book();
        genko::core::Page& page = doc.edit_page(0);
        page.numero = true;  // nombres are printed and proofed, not shown in the name
        page.prims = Json::array({Json::object({{"kind", "cube"}})});  // 3D guides are never printed
        render::RenderOptions name;
        name.mode = "name";
        QCOMPARE(unported_element(*doc.pages[0], doc, name), std::string("prims"));
        page.prims = Json::array();
        QVERIFY(unported_element(*doc.pages[0], doc, name).empty());
        // a hidden tone layer is not drawn
        Layer tone;
        tone.id = genko::core::new_id();
        tone.role = LayerRole::Tone;
        tone.visible = false;
        page.layers.push_back(tone);
        QVERIFY(unported_element(*doc.pages[0], doc, name).empty());
        // nor nombres turned off for the book
        doc.nombre = Json::object({{"show", false}, {"hidden", false}});
        render::RenderOptions print;
        QVERIFY(unported_element(*doc.pages[0], doc, print).empty());
    }

    void cancelled() {
        const Document doc = book();
        std::stop_source stop;
        stop.request_stop();
        render::RenderOptions options;
        options.stop = stop.get_token();
        QVERIFY_THROWS_EXCEPTION(render::Cancelled, render::render_page(*doc.pages[0], 150, options, &doc));
    }

    void threads() {
        const Document doc = book();
        render::RenderOptions options;
        options.mode = "proof";
        const std::string want = render::render_page(*doc.pages[0], 150, options, &doc).image.tobytes();
        std::vector<std::string> got(6);
        std::vector<std::thread> workers;
        for (std::size_t i = 0; i < got.size(); ++i) {
            workers.emplace_back([&, i] {
                if (i % 2 == 0) render::clear_render_caches();
                render::RenderOptions o = options;
                if (i % 3 == 0) {
                    o.region = render::RenderRegion{0, 0, 200, 300};
                    got[i] = render::render_page(*doc.pages[0], 150, o, &doc).image.tobytes();
                } else {
                    got[i] = render::render_page(*doc.pages[0], 150, o, &doc).image.tobytes();
                }
            });
        }
        for (auto& w : workers) w.join();
        const render::Image whole = render::Image::frombytes("RGB", render::Size{render::mm_to_px(70, 150), render::mm_to_px(95, 150)}, want);
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (i % 3 == 0) {
                QVERIFY(got[i] == whole.crop(render::Box{0, 0, 200, 300}).tobytes());
            } else {
                QVERIFY(got[i] == want);
            }
        }
    }

    void regions() {
        const Document doc = book();
        for (const char* mode : {"print", "proof", "name"}) {
            render::RenderOptions options;
            options.mode = mode;
            const render::Image whole = render::render_page(*doc.pages[0], 150, options, &doc).image;
            for (const render::RenderRegion r : {render::RenderRegion{0, 0, 1, 1}, render::RenderRegion{17, 33, 120, 77},
                                                 render::RenderRegion{0, 200, whole.width(), 60},
                                                 render::RenderRegion{whole.width() - 40, whole.height() - 25, 40, 25}}) {
                for (const bool fresh : {true, false}) {
                    if (fresh) render::clear_render_caches();
                    render::RenderOptions part = options;
                    part.region = r;
                    const render::Image got = render::render_page(*doc.pages[0], 150, part, &doc).image;
                    QCOMPARE(got.size(), (render::Size{r.w, r.h}));
                    QVERIFY(got.tobytes() == whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h}).tobytes());
                }
            }
        }
        render::RenderOptions outside;
        outside.region = render::RenderRegion{-1, 0, 10, 10};
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::render_page(*doc.pages[0], 150, outside, &doc));
        outside.region = render::RenderRegion{0, 0, 10000, 10};
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::render_page(*doc.pages[0], 150, outside, &doc));
    }

    void remembered_lines() {
        Document doc = book();
        genko::core::Page& page = doc.edit_page(0);
        for (auto& layer : page.layers) {
            if (layer.role != LayerRole::Ink) continue;
            std::vector<genko::core::StrokePtr> items = layer.strokes->items;
            for (int i = 0; i < 320; ++i) items.push_back(line(3.0 + 0.15 * i, 40.0 + 0.1 * i, 0.4, 0.3));
            layer.strokes = genko::core::make_strokes(std::move(items));
            layer.panel_each = false;
            layer.effect.reset();
        }
        QVERIFY(render::rough_needed(*doc.pages[0], 72));
        render::RenderOptions options;
        (void)render::render_page(*doc.pages[0], 72, options, &doc);
        QVERIFY(!render::rough_needed(*doc.pages[0], 72));
        QVERIFY(render::rough_needed(*doc.pages[0], 100));  // (another resolution has nothing remembered)
    }

    void frames_and_errors() {
        const Document doc = book();
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::render_frame(*doc.pages[0], "no-such-frame", 72));
        const auto leaves = doc.pages[0]->leaf_frames();
        const render::Image panel = render::render_frame(*doc.pages[0], leaves.front()->id, 72);
        QVERIFY(panel.width() > 0 && panel.height() > 0);
        QCOMPARE(std::string(render::to_bitonal(panel).mode()), std::string("1"));
        const Json dots = Json::object({{"pattern", "dot"}});
        QVERIFY_THROWS_EXCEPTION(render::NotYetPorted, render::to_bitonal(panel, 180, &dots));
        QCOMPARE(render::export_plan(*doc.pages[0]).size(), std::size_t{7});
        QCOMPARE(render::mm_to_px(0.0, 600), 1);
        QCOMPARE(render::mm_to_px(25.4, 72), 72);
        QCOMPARE(render::mm_to_px(0.1, 254), 1);  // round(1.0)
    }
};

QTEST_GUILESS_MAIN(TestRenderPage)
#include "test_render_page.moc"
