// render_page without the Python reference: what is not drawn yet stops the render (NotYetPorted) unless
// skip_unported, a stop request cancels it, renders on several threads at once give the same pixels, parts of the
// page are the same as the whole page cut, and the remembered lines are used (rough_needed).

#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include "render/draw.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

#include <memory>
#include <fstream>
#include <filesystem>
#include "render/selection.hpp"
#include <stop_token>
#include <thread>
#include <vector>

#include "core/error.hpp"
#include "core/command_bus.hpp"
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

    void nombreCases_data() {
        QTest::addColumn<QString>("data");
        std::ifstream file(std::filesystem::path(GENKO_SOURCE_DIR) / "tests/fixtures/render/nombre-cases.json");
        QVERIFY(file.good());
        const Json reference = Json::parse(file);
        for (const auto& c : reference.at("cases")) QTest::newRow(c.at("id").get<std::string>().c_str()) << QString::fromStdString(c.dump());
    }
    void nombreCases() {
        QFETCH(QString, data);
        const Json c = Json::parse(data.toStdString());
        auto doc = genko::core::new_episode("nombre fixture", Num(1), 2, genko::core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6));
        doc.nombre = c.at("config");
        if (c.at("start_side").is_string()) doc.start_side = c.at("start_side").get<std::string>();
        if (c.at("cover_before").get<bool>()) doc.edit_page(0).extra["cover"] = {{"kind", "front"}};
        auto& page = doc.edit_page(1); page.index = Num(c.at("index").get<int>()); page.numero = c.at("numero").get<bool>();
        page.layers.clear(); page.frames.clear(); page.extra["paper_rgb"] = c.at("paper");
        const int dpi = c.at("dpi").get<int>();
        std::ifstream file(std::filesystem::path(GENKO_SOURCE_DIR) / "tests/fixtures/render" / c.at("png").get<std::string>(), std::ios::binary);
        QVERIFY(file.good()); const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto expected = render::selection::open_picture(bytes).convert("RGB");
        for (const std::string mode : {"proof", "print"}) {
            render::RenderOptions options; options.mode = mode; options.finish = false;
            const auto whole = render::render_page(page, dpi, options, &doc);
            QVERIFY(whole.omitted.empty()); QCOMPARE(whole.image.size(), expected.size());
            if (whole.image.tobytes() != expected.tobytes()) {
                const auto out = std::filesystem::path(GENKO_SOURCE_DIR) / "../build/text-diagnostics";
                std::filesystem::create_directories(out);
                render::save_png(whole.image, out / (c.at("id").get<std::string>() + "-native.png"));
                for (int size : {9,13}) {
                    const auto mask = render::text_mask("12", c.at("config").value("font",std::string("gothic")),size,{0.5,0.5});
                    qWarning() << "native mask" << size << mask.second[0] << mask.second[1] << mask.second[2] << mask.second[3];
                    render::save_png(mask.first, out / (c.at("config").value("font",std::string("gothic")) + "-" + std::to_string(size) + "-native.png"));
                }
                QFAIL("nombre full pixels must match unmodified Python");
            }
            // Cut through every label, including fractional placement and white-edge neighbourhood.
            for (const auto& p : c.at("placements")) {
                const int x = render::mm_to_px(p.at("x_mm").get<double>(), dpi);
                const int y = render::mm_to_px(p.at("y_mm").get<double>(), dpi);
                render::RenderOptions part = options;
                part.region = render::RenderRegion{std::max(0,x-2), std::max(0,y-2), 4, 4};
                const auto roi = render::render_page(page, dpi, part, &doc);
                const auto r = *part.region;
                QCOMPARE(roi.image.tobytes(), expected.crop(render::Box{r.x,r.y,r.x+r.w,r.y+r.h}).tobytes());
            }
        }
        render::RenderOptions name; name.mode = "name"; name.finish = false;
        const auto named = render::render_page(page, dpi, name, &doc);
        const auto blank = render::Image::create("RGB", expected.size(), render::Ink::tuple(genko::core::int_tuple(c.at("paper"))));
        QCOMPARE(named.image.tobytes(), blank.tobytes());
    }
    void fontBrotliBudget() {
        const std::string path=std::string(GENKO_SOURCE_DIR)+"/tests/fixtures/render/font-wide-glyph.woff2";
        {
            render::ImageAllocationBudget budget(4*1024*1024);
            QVERIFY(!render::text_mask("A",path,6).first.empty());
            QCOMPARE(budget.live(),std::uint64_t(0));
        }
        render::ImageAllocationBudget budget(48*1024);
        QVERIFY2(([&](){try{(void)render::text_mask("A",path,6);return false;}catch(const genko::core::OpError&){return true;}}()),
                 "WOFF2 Brotli temporary memory must be budgeted before decoding");
        QCOMPARE(budget.live(),std::uint64_t(0));
    }
    void fontCodecBudget() {
        const std::string path=std::string(GENKO_SOURCE_DIR)+"/tests/fixtures/render/font-png-profile.ttf";
        {
            render::ImageAllocationBudget budget(32*1024*1024);
            QVERIFY(!render::text_mask("A",path,128).first.empty());
            QCOMPARE(budget.live(),std::uint64_t(0));
        }
        render::ImageAllocationBudget budget(128*1024);
        QVERIFY2(([&](){try{(void)render::text_mask("A",path,128);return false;}catch(const genko::core::OpError&){return true;}}()),
                 "embedded PNG metadata must be budgeted before decoding");
        QVERIFY(budget.peak()<=128*1024);QCOMPARE(budget.live(),std::uint64_t(0));
    }
    void fontRasterBudget() {
        const std::string path=std::string(GENKO_SOURCE_DIR)+"/tests/fixtures/render/font-wide-glyph.ttf";
        { // Positive control proves the font is valid and reaches the native renderer, rather than fallback.
            render::ImageAllocationBudget budget(4*1024*1024);
            const auto [mask,box]=render::text_mask("A",path,128);
            QCOMPARE(mask.width(),512);QCOMPARE(mask.height(),480);
            QVERIFY(mask.getbbox().has_value());QCOMPARE(box[2]-box[0],512);
        }
        { // Large glyph: the library cap refuses before a roughly 225 MiB bitmap can be allocated.
            render::ImageAllocationBudget budget(4*1024*1024);
            try { (void)render::text_mask("A",path,4096);QFAIL("large glyph must refuse before bitmap allocation"); }
            catch(const genko::core::Error& e){QCOMPARE(e.code(),std::string("memory"));}
            QVERIFY(budget.peak()<=4*1024*1024);QCOMPARE(budget.live(),std::uint64_t(0));
        }
        { // Stroke creates its own FreeType bitmap while the output mask is alive.
            render::ImageAllocationBudget budget(32*1024*1024);
            QVERIFY(!render::text_mask("A",path,128,{}, {},512).first.empty());
            QCOMPARE(budget.live(),std::uint64_t(0));
        }
        { // The same valid stroke must refuse under the shared temporary/output limit and recover.
            render::ImageAllocationBudget budget(4*1024*1024);
            QVERIFY_THROWS_EXCEPTION(genko::core::OpError,render::text_mask("A",path,128,{}, {},512));
            QVERIFY(budget.peak()<=4*1024*1024);QCOMPARE(budget.live(),std::uint64_t(0));
        }
        render::ImageAllocationBudget budget(128*1024);
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError,render::text_mask("A",path,128));
        QVERIFY2(budget.peak()>0,"font work must participate in the image allocation budget before rasterization");
        QVERIFY(budget.peak()<=128*1024);QCOMPARE(budget.live(),std::uint64_t(0));
        const auto image=render::Image::create("L",{8,8},0);QVERIFY(!image.empty());
    }
    void fontGlyphs_data() {
        QFile f(QString::fromUtf8(GENKO_SOURCE_DIR)+"/tests/fixtures/render/font-glyphs.json"); QVERIFY(f.open(QIODevice::ReadOnly));
        const Json data=Json::parse(f.readAll().toStdString());QTest::addColumn<int>("index");
        for(std::size_t n=0;n<data.at("cases").size();++n)QTest::newRow(data.at("cases")[n].at("id").get<std::string>().c_str())<<static_cast<int>(n);
    }
    void fontGlyphs() {
        QFETCH(int,index);QFile f(QString::fromUtf8(GENKO_SOURCE_DIR)+"/tests/fixtures/render/font-glyphs.json");QVERIFY(f.open(QIODevice::ReadOnly));
        const Json data=Json::parse(f.readAll().toStdString());const Json& c=data.at("cases")[static_cast<std::size_t>(index)];
        std::string font=c.at("font").get<std::string>();if(font.starts_with("file:"))font=std::string(GENKO_REPO_ROOT)+"/"+font.substr(5);
        const auto [image,box]=render::text_mask(c.at("text").get<std::string>(),font,c.at("size").get<int>());
        const auto expected=c.at("bbox").get<std::array<int,4>>();
        QCOMPARE(box[0],static_cast<int>(expected[0]));QCOMPARE(box[1],static_cast<int>(expected[1]));QCOMPARE(box[2],static_cast<int>(expected[2]));QCOMPARE(box[3],static_cast<int>(expected[3]));
        QCOMPARE(image.width(),c.at("mask_size")[0].get<int>());QCOMPARE(image.height(),c.at("mask_size")[1].get<int>());
        QCOMPARE(image.tobytes(),QByteArray::fromBase64(QByteArray::fromStdString(c.at("data").get<std::string>())).toStdString());
        QCOMPARE(render::text_length(c.at("text").get<std::string>(),font,c.at("size").get<int>()),c.at("advance").get<double>());
    }
    void nombreSafety() {
        const QString root = QString::fromUtf8(GENKO_SOURCE_DIR) + "/tests/fixtures/render/";
        QFile file(root + "MANIFEST.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        const Json manifest = Json::parse(file.readAll().toStdString());
        QStringList listed;
        for (const auto& [name, hash] : manifest.items()) {
            QFile fixture(root + QString::fromStdString(name)); QVERIFY(fixture.open(QIODevice::ReadOnly));
            QCOMPARE(QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex().toStdString(), hash.get<std::string>());
            listed.push_back(QString::fromStdString(name));
        }
        auto actual = QDir(root).entryList(QDir::Files); actual.removeAll("MANIFEST.json"); listed.sort(); QCOMPARE(actual, listed);
        for (const double size : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), -1.0, 21.0}) {
            auto doc = genko::core::new_episode("invalid", Num(1), 1, genko::core::PageSpec::custom(70,95,60,85,3,8,8,7,6));
            doc.edit_page(0).frames.clear(); doc.edit_page(0).layers.clear(); doc.nombre = {{"size_mm",size}};
            QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::render_page(*doc.pages[0], 110, {}, &doc));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::text_mask("12", "gothic", 8193));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::text_mask(std::string(65537,'a'), "gothic", 12));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::text_mask("12", "gothic", 12, {1,0}));
        std::stop_source stop; stop.request_stop();
        QVERIFY_THROWS_EXCEPTION(render::Cancelled, render::text_mask("12", "gothic", 12, {}, stop.get_token()));
        QTemporaryDir font_dir;QVERIFY(font_dir.isValid());
        QFile huge(font_dir.path()+"/huge.ttf");QVERIFY(huge.open(QIODevice::WriteOnly));QVERIFY(huge.resize(32LL*1024*1024+1));huge.close();
        QFile broken(font_dir.path()+"/broken.ttf");QVERIFY(broken.open(QIODevice::WriteOnly));QCOMPARE(broken.write("not a font"),qint64(10));broken.close();
        for(const auto& font_file:{huge.fileName(),broken.fileName()}) {
            QVERIFY_THROWS_EXCEPTION(genko::core::Error,render::text_mask("漢",font_file.toStdString(),12));
            QVERIFY_THROWS_EXCEPTION(genko::core::Error,render::text_length("漢",font_file.toStdString(),12));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error,render::text_mask(std::string("\xff",1),"gothic",12));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error,render::text_font(std::string("\xff",1),"あ"));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error,render::text_length("a","gothic",8193));
        {
            render::ImageAllocationBudget budget(1024);
            const auto live = budget.live();
            QVERIFY_THROWS_EXCEPTION(genko::core::OpError, render::text_mask("0123456789", "gothic", 512));
            QCOMPARE(budget.live(), live);
            QVERIFY(!render::Image::create("L",{8,8},0).empty());
        }
        // A glyph also owns font bytes/library scratch now; a 1 KiB budget cannot hold those.
        // Verify normal glyph recovery after restoring the enclosing budget, not by ignoring font work.
        QVERIFY(!render::text_mask("1", "gothic", 6).first.empty());
    }
    void nombreVisible_data(){QTest::addColumn<QString>("mode");QTest::newRow("proof")<<QString("proof");QTest::newRow("print")<<QString("print");}
    void nombreVisible(){
        QFETCH(QString,mode);
        auto doc=genko::core::new_episode("nombre fixture",Num(1),1,genko::core::PageSpec::custom(70,95,60,85,3,8,8,7,6));
        auto& page=doc.edit_page(0);page.frames.clear();page.layers.clear();doc.nombre={{"font","gothic"},{"start",12}};
        render::RenderOptions options;options.mode=mode.toStdString();options.finish=false;
        try{
            const auto actual=render::render_page(page,110,options,&doc);
            std::ifstream file(std::filesystem::path(GENKO_SOURCE_DIR)/"tests/fixtures/render/nombre-gothic-110.png",std::ios::binary);QVERIFY(file.good());
            const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());const auto expected=render::selection::open_picture(bytes).convert("RGB");
            QCOMPARE(actual.image.size(),expected.size());QCOMPARE(actual.image.tobytes(),expected.tobytes());
        }catch(const render::NotYetPorted& e){QVERIFY(std::string(e.what()).find("nombre")!=std::string::npos);QFAIL("nombre component must be ported");}catch(const std::exception& e){QFAIL((std::string("unexpected nombre failure: ")+e.what()).c_str());}
    }
    void not_yet_ported_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("mode");
        // (tones, effect lines and screens are drawn since M3-B, the 3D guides since M3-C, a page's animation since
        // M3-③: anim_is_drawn)
        for (const char* what : {"balloons", "covers", "placed"}) {
            const char* mode = std::string(what) == "covers" ? "proof" : "print";
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
        if (w == "covers") page.extra["cover"] = Json::object({{"kind", "jacket"}, {"spine_mm", 5}, {"flap_mm", 10}});
        if (w == "anim") page.extra["anim"] = Json::object({{"fps", 12}, {"tracks", Json::array()}});
        if (w == "placed") {
            Layer placed;
            placed.id = genko::core::new_id();
            placed.kind = LayerKind::Placed;
            page.layers.push_back(placed);
        }
        render::RenderOptions options;
        options.mode = mode.toStdString();
        if (w == "nombre") {
            QVERIFY(unported_element(*doc.pages[0], doc, options).empty());
            const auto result = render::render_page(*doc.pages[0], 72, options, &doc);
            QVERIFY(result.omitted.empty());
            QVERIFY(result.image.getextrema().front().first < 255);
            return;
        }
        QCOMPARE(unported_element(*doc.pages[0], doc, options), w);
        options.skip_unported = true;
        const render::RenderResult r = render::render_page(*doc.pages[0], 72, options, &doc);
        QVERIFY(std::find(r.omitted.begin(), r.omitted.end(), w) != r.omitted.end());
        // the other page has none of it
        options.skip_unported = false;
        QVERIFY(unported_element(*doc.pages[1], doc, options).empty());
    }

    void anim_is_drawn() {
        Document doc = book();
        doc.edit_page(0).extra["anim"] = Json::object({{"fps", 12}, {"tracks", Json::array()}});
        for (const char* mode : {"print", "proof", "name"}) {
            render::RenderOptions options;
            options.mode = mode;
            QVERIFY(unported_element(*doc.pages[0], doc, options).empty());
            const render::RenderResult r = render::render_page(*doc.pages[0], 72, options, &doc);
            QVERIFY(r.omitted.empty());
        }
    }

    void unported_only_where_drawn() {
        Document doc = book();
        genko::core::Page& page = doc.edit_page(0);
        page.numero = true;  // nombres are printed and proofed, not shown in the name
        render::RenderOptions name;
        name.mode = "name";
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
        QCOMPARE(std::string(render::to_bitonal(panel, 180, &dots).mode()), std::string("1"));
        QCOMPARE(render::export_plan(*doc.pages[0]).size(), std::size_t{7});
        QCOMPARE(render::mm_to_px(0.0, 600), 1);
        QCOMPARE(render::mm_to_px(25.4, 72), 72);
        QCOMPARE(render::mm_to_px(0.1, 254), 1);  // round(1.0)
    }
};

QTEST_GUILESS_MAIN(TestRenderPage)
#include "test_render_page.moc"
