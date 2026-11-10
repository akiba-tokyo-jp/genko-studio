// smart-objects@1, PC3 A1 (SPEC.md COLOR-05「中身とリンク」): the ops of a smart object's contents and links —
// smart_replace (another file, every smart object showing it changed, its place kept), smart_relink and smart_update (a
// linked file's place and its new copy), smart_embed, smart_link (a file as it is, a page as the file it was written to,
// laid as large), smart_to_page (a picture made a page to edit: the same pixels on the page) —, what a link's record
// must be, every page showing the contents asked its lock, and contents inside contents drawn again when the inner ones
// change (コンテンツを編集's tab).

#include <QtTest>

#include <cmath>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
using core::Actor;
using core::ApplyError;
using core::Document;
using core::Json;
using core::Num;
using core::smart::Matrix;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

Document run(const Document& doc, const Json& ops, const Actor& actor = Actor("human:作者")) { return bus().apply(doc, ops, actor).doc; }
Json reported(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")).results.back(); }

std::string png_of(int w, int h, render::Ink ink) { return render::write_png(render::Image::create("RGB", render::Size{w, h}, ink)); }

// A picture of two colours, side by side.
std::string two_colours(int w, int h) {
    render::Image picture = render::Image::create("RGB", render::Size{w, h}, render::Ink{200, 40, 90});
    picture.paste(render::Ink{30, 160, 90}, render::Box{w / 2, 0, w, h});
    return render::write_png(picture);
}

const core::Layer* layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& l : page.layers)
        if (l.id == id) return &l;
    return nullptr;
}

bool refused(const Document& doc, const Json& ops, const QString& words, const Actor& actor = Actor("human:作者")) {
    try {
        (void)bus().apply(doc, ops, actor);
    } catch (const ApplyError& error) {
        if (QString::fromUtf8(error.what()).contains(words)) return true;
        qWarning("%s", error.what());
        return false;
    }
    return false;
}

// A colour book (the pictures' own colours on the page) with a picture placed as a smart object "so" on page 1, in the
// middle at 10 times its size, and a second smart object "twin" showing the same contents.
Document placed(const std::string& file = two_colours(8, 6)) {
    Document doc = core::new_episode("中身", Num(1), 2, core::PageSpec::webtoon());
    doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(file)}, {"name", "a.png"}, {"new_id", "so"}, {"scale", 1000}},
                                Json{{"op", "duplicate_layer"}, {"page", 1}, {"id", "so"}}}));
    return doc;
}

std::string twin_of(const Document& doc) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.kind == core::LayerKind::Smart && l.id != "so") return l.id;
    return {};
}

const core::SmartContents& contents(const Document& doc, const std::string& layer = "so") {
    return *core::smart::contents_of(doc, *layer_of(doc.page(0), layer));
}

render::Image drawn(const Document& doc, std::size_t page = 0, int dpi = 40) {
    render::RenderOptions options;
    options.mode = "proof";
    return render::render_page(doc.page(page), dpi, options, &doc).image.convert("RGB");
}

int most_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.tobytes(), y = b.tobytes();
    if (x.size() != y.size()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i]))));
    return most;
}

}  // namespace

class TestSmartContents : public QObject {
    Q_OBJECT

private slots:
    void replacedForEverySmartObjectShowingIt() {
        const Document doc = placed();
        const std::string twin = twin_of(doc);
        QVERIFY(!twin.empty());
        const std::string id = *core::smart::contents_id(*layer_of(doc.page(0), "so"));
        QCOMPARE(core::smart::contents_id(*layer_of(doc.page(0), twin)), std::optional<std::string>(id));
        const Matrix before = core::smart::matrix_of(*layer_of(doc.page(0), "so"));
        const std::string other = png_of(10, 5, render::Ink{10, 20, 30});
        const Json report = reported(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(other)}, {"name", "b.png"}}}));
        QCOMPARE(report["contents"], Json(id));
        QCOMPARE(report["size_px"], Json::array({10, 5}));
        const Document replaced = run(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(other)}, {"name", "b.png"}}}));
        // (the contents are the new file — the same id, so both smart objects show it —, their places kept)
        QCOMPARE(contents(replaced).asset, core::bytes_ref(other));
        QCOMPARE(contents(replaced).name, std::string("b.png"));
        QCOMPARE(contents(replaced).width_px, std::int64_t{10});
        QVERIFY(!contents(replaced).link);
        QCOMPARE(&contents(replaced, twin), &contents(replaced));
        QCOMPARE(core::smart::matrix_of(*layer_of(replaced.page(0), "so")), before);
        // (the file a page drew is new: the page another object, drawn as the new file)
        QVERIFY(replaced.pages[0] != doc.pages[0]);
        const auto cx = replaced.page(0).spec.width_mm.value() / 2, cy = replaced.page(0).spec.height_mm.value() / 2;
        const render::Image after = drawn(replaced);
        const auto px = after.getpixel(static_cast<int>(cx * 40 / 25.4), static_cast<int>(cy * 40 / 25.4));
        QCOMPARE(px, (std::vector<double>{10, 20, 30}));
        // (a page's contents replaced by a file: a file's contents now)
        Document wrapped = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 2}, {"kind", "paint"}, {"id", "p"}},
                                                 Json{{"op", "put_raster"}, {"page", 2}, {"id", "p"}, {"png_base64", core::b64encode(two_colours(30, 30))}},
                                                 Json{{"op", "smart_convert"}, {"page", 2}, {"id", "p"}, {"new_id", "wrap"}}}));
        wrapped = run(wrapped, Json::array({Json{{"op", "smart_replace"}, {"page", 2}, {"id", "wrap"}, {"data", core::b64encode(other)}}}));
        QCOMPARE(core::smart::contents_of(wrapped, *layer_of(wrapped.page(1), "wrap"))->kind, std::string("file"));
        // (refused: no file, not a smart object, a file that is not a picture)
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}}}), "gives the file"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", doc.page(0).layers[0].id}, {"data", core::b64encode(other)}}}),
                        "is not a smart object"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(std::string("not a picture"))}}}),
                        "cannot be read"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"asset", "/tmp/a.png"}}}), "not a path"));
    }

    void linkedRelinkedUpdatedAndEmbedded() {
        const Document doc = placed();
        const std::string first = png_of(8, 6, render::Ink{1, 2, 3});
        // relinked: a new file and its place, its sha256 and size recorded
        const Document linked = run(doc, Json::array({Json{{"op", "smart_relink"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(first)},
                                                           {"link", Json{{"path", "/files/c.png"}, {"relative", "../files/c.png"}}}}}));
        const core::SmartContents c = contents(linked);
        QVERIFY(c.link.has_value());
        QCOMPARE((*c.link)["path"], Json("/files/c.png"));
        QCOMPARE((*c.link)["relative"], Json("../files/c.png"));
        QCOMPARE((*c.link)["sha256"], Json(core::bytes_ref(first).substr(7)));
        QCOMPARE((*c.link)["bytes"], Json(static_cast<std::int64_t>(first.size())));
        QCOMPARE(c.name, std::string("c.png"));
        QCOMPARE(c.asset, core::bytes_ref(first));
        // (the same bytes in another place: only the record)
        const Document moved = run(linked, Json::array({Json{{"op", "smart_relink"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(first)},
                                                             {"link", Json{{"path", "/elsewhere/c.png"}}}}}));
        QCOMPARE((*contents(moved).link)["path"], Json("/elsewhere/c.png"));
        QVERIFY(!contents(moved).link->contains("relative"));
        QCOMPARE(contents(moved).asset, c.asset);
        // updated: the file's new bytes its copy, the record with them; the same bytes: nothing updated
        Json same = reported(linked, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(first)}}}));
        QCOMPARE(same["updated"], Json(false));
        const std::string second = png_of(12, 6, render::Ink{9, 8, 7});
        Json report = reported(linked, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(second)}}}));
        QCOMPARE(report["updated"], Json(true));
        const Document updated = run(linked, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(second)}}}));
        QCOMPARE(contents(updated).asset, core::bytes_ref(second));
        QCOMPARE(contents(updated).width_px, std::int64_t{12});
        QCOMPARE((*contents(updated).link)["sha256"], Json(core::bytes_ref(second).substr(7)));
        QCOMPARE((*contents(updated).link)["path"], Json("/files/c.png"));
        QCOMPARE(&contents(updated, twin_of(updated)), &contents(updated));
        // embedded: the copy is the contents, the record gone; update and embed refused for an embedded one
        const Document embedded = run(updated, Json::array({Json{{"op", "smart_embed"}, {"page", 1}, {"id", "so"}}}));
        QVERIFY(!contents(embedded).link);
        QCOMPARE(contents(embedded).asset, core::bytes_ref(second));
        QVERIFY(refused(embedded, Json::array({Json{{"op", "smart_embed"}, {"page", 1}, {"id", "so"}}}), "not linked"));
        QVERIFY(refused(embedded, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(second)}}}), "not linked"));
        // linked as it is (the window wrote its bytes to the file): the record of its bytes
        const Document relinked = run(embedded, Json::array({Json{{"op", "smart_link"}, {"page", 1}, {"id", "so"}, {"link", Json{{"path", "/out/b.png"}}}}}));
        QCOMPARE((*contents(relinked).link)["sha256"], Json(core::bytes_ref(second).substr(7)));
        QCOMPARE((*contents(relinked).link)["bytes"], Json(static_cast<std::int64_t>(second.size())));
        QVERIFY(refused(embedded, Json::array({Json{{"op", "smart_link"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(first)}, {"link", Json{{"path", "/x.png"}}}}}),
                        "linked as they are"));
        // a link's record must be one: a path, 64 hex digits, a size, no other keys
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_relink"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(first)}, {"link", Json{{"relative", "a"}}}}}),
                        "path"));
        Document strange = linked;
        Json bad = *contents(strange).link;
        bad["when"] = 1;
        strange.edit_contents(*core::smart::contents_id(*layer_of(strange.page(0), "so"))).link = bad;
        try {
            core::smart::validate_smart(strange);
            QFAIL("a link of a key not known");
        } catch (const core::Error& error) {
            QVERIFY2(QString::fromUtf8(error.what()).contains(QStringLiteral("link's 'when'")), error.what());
        }
        bad.erase("when");
        bad["sha256"] = "abc";
        strange.edit_contents(*core::smart::contents_id(*layer_of(strange.page(0), "so"))).link = bad;
        try {
            core::smart::validate_smart(strange);
            QFAIL("a link of a short sha256");
        } catch (const core::Error& error) {
            QVERIFY2(QString::fromUtf8(error.what()).contains(QStringLiteral("sha256")), error.what());
        }
    }

    void aPageLinkedAsTheFileItWasWrittenTo() {
        Document doc = core::new_episode("中身", Num(1), 1, core::PageSpec::webtoon());
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", core::b64encode(two_colours(30, 30))}},
                                    Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p"}, {"new_id", "so"}}}));
        const auto [w_mm, h_mm] = core::smart::size_mm(contents(doc));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_link"}, {"page", 1}, {"id", "so"}, {"link", Json{{"path", "/out/so.psd"}}}}}), "written to"));
        // (a PNG stands for the file written: 300 pixels wide)
        const std::string file = png_of(300, 200, render::Ink{5, 6, 7});
        const Document linked =
            run(doc, Json::array({Json{{"op", "smart_link"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(file)}, {"link", Json{{"path", "/out/so.png"}}}}}));
        const core::SmartContents now = contents(linked);
        QCOMPARE(now.kind, std::string("file"));
        QCOMPARE(now.name, std::string("so.png"));
        QVERIFY(now.link.has_value());
        // (laid as large as the page was: its width the page's)
        QVERIFY(std::abs(core::smart::size_mm(now).first - w_mm) < 1e-9);
        (void)h_mm;
    }

    void aPictureMadeAPageDrawsAsItDid_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("8 bits") << QString();
        QTest::newRow("16 bits") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 16})");
        QTest::newRow("CMYK") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "cmyk", "profile": "coated-cmyk"})");
    }

    void aPictureMadeAPageDrawsAsItDid() {
        QFETCH(QString, mode);
        Document doc = core::new_episode("中身", Num(1), 1, core::PageSpec::webtoon());
        if (!mode.isEmpty()) doc = run(doc, Json::array({Json::parse(mode.toStdString())}));
        // (laid at whole pixels: 40 by 30 pixels at 200 dpi)
        const std::string file = two_colours(40, 30);
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(file)}, {"name", "pic.png"}, {"new_id", "so"},
                                          {"placement_mm", Json::array({25.4, 50.8, 40 * 25.4 / 200, 30 * 25.4 / 200})}}}));
        const render::Image before = drawn(doc, 0, 200);
        const std::string id = *core::smart::contents_id(*layer_of(doc.page(0), "so"));
        const Json report = reported(doc, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}, {"new_id", "pic"}}}));
        QCOMPARE(report["kind"], Json("page"));
        QCOMPARE(report["layer"], Json("pic"));
        const Document edited = run(doc, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}, {"new_id", "pic"}}}));
        const core::SmartContents now = contents(edited);
        QCOMPARE(now.kind, std::string("page"));
        QCOMPARE(now.name, std::string("pic.png"));
        QVERIFY(now.page != nullptr && now.page->layers.size() == 1);
        const core::Layer& picture = now.page->layers[0];
        QCOMPARE(picture.kind, core::LayerKind::Raster);
        if (mode.isEmpty()) {  // (its pixels the file's, every one of them — the edges too)
            QVERIFY(picture.raster_png != nullptr);
            const render::Image held = render::open_image(*picture.raster_png, render::kPillowOpenLimits).convert("RGBA");
            QCOMPARE(held.size(), (render::Size{40, 30}));
            QCOMPARE(held.tobytes(), render::open_image(file, render::kPillowOpenLimits).convert("RGBA").tobytes());
        } else {
            QVERIFY(mode.contains(QStringLiteral("cmyk")) ? picture.ink_raster != nullptr : picture.color_raster != nullptr);
        }
        QCOMPARE(picture.title, std::string("pic"));
        QVERIFY(std::abs(now.page->spec.width_mm.value() - 40 * 25.4 / 200) < 1e-9);
        QVERIFY(std::abs(now.page->spec.height_mm.value() - 30 * 25.4 / 200) < 1e-9);
        QCOMPARE(core::smart::contents_id(*layer_of(edited.page(0), "so")), std::optional<std::string>(id));
        // (the page drawn as before: a pixel of the file a pixel of the paint layer, laid where it was. On a CMYK page the
        // paint layer holds the page's inks, 8 bits a plate, where the file's RGB was taken into them as it was drawn: the
        // screen's sRGB of them within 1)
        QVERIFY(most_apart(drawn(edited, 0, 200), before) <= (mode.contains(QStringLiteral("cmyk")) ? 1 : 0));
        QVERIFY(refused(edited, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}}}), "a page already"));
        // an op on its page (コンテンツを編集's tab): every page showing it changes
        const Document painted = run(edited, Json::array({Json{{"op", "fill_area"}, {"contents", id}, {"page", 1}, {"layer_id", "pic"},
                                                                {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({3, 0}), Json::array({3, 4}), Json::array({0, 4})})}}}, {"rgb", Json::array({0, 0, 255})}}}));
        QVERIFY(most_apart(drawn(painted, 0, 200), before) > 100);
    }

    void everyPageShowingTheContentsIsAsked() {
        Document doc = placed();
        doc = run(doc, Json::array({Json{{"op", "duplicate_page"}, {"page", 1}}}));
        // (the copy — page 3 — shows the same contents, held by another)
        const core::Page& copy = *doc.pages.back();
        QCOMPARE(copy.index, Num(3));
        bool shared = false;
        for (const core::Layer& l : copy.layers) shared = shared || (l.kind == core::LayerKind::Smart && core::smart::contents_id(l) == core::smart::contents_id(*layer_of(doc.page(0), "so")));
        QVERIFY(shared);
        doc = run(doc, Json::array({Json{{"op", "lock_page"}, {"page", 3}, {"agent", "ai:other"}}}), Actor("ai:other"));
        const std::string file = png_of(4, 4, render::Ink{0, 0, 0});
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(file)}}}), "page 3 locked by ai:other"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}}}), "page 3 locked by ai:other"));
        QVERIFY(refused(doc, Json::array({Json{{"op", "smart_profile"}, {"page", 1}, {"id", "so"}, {"embedded_profile", "keep"}}}), "page 3 locked by ai:other"));
        // (their own changes: allowed)
        (void)run(doc, Json::array({Json{{"op", "smart_replace"}, {"page", 1}, {"id", "so"}, {"data", core::b64encode(file)}}}), Actor("ai:other"));
    }

    void contentsInsideContentsAreDrawnAgain() {
        Document doc = core::new_episode("入れ子", Num(1), 1, core::PageSpec::webtoon());
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", core::b64encode(two_colours(30, 30))}},
                                    Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p"}, {"new_id", "inner"}},
                                    Json{{"op", "smart_transform"}, {"page", 1}, {"id", "inner"}, {"matrix", Json::array({0.5, 0, 0, 0.5, 3, 4})}},
                                    Json{{"op", "smart_convert"}, {"page", 1}, {"id", "inner"}, {"new_id", "outer"}},
                                    Json{{"op", "smart_transform"}, {"page", 1}, {"id", "outer"}, {"matrix", Json::array({1.5, 0, 0, 1.5, 0, 0})}}}));
        const std::string outer = *core::smart::contents_id(*layer_of(doc.page(0), "outer"));
        const std::string inner = *core::smart::contents_id(*layer_of(*doc.smart_contents.at(outer)->page, "inner"));
        const render::Image before = drawn(doc);
        const core::SmartContents* was = doc.smart_contents.at(outer).get();
        // (the inner contents painted in their tab: the outer contents a new value, the page drawn anew)
        const Document painted = run(doc, Json::array({Json{{"op", "fill_area"}, {"contents", inner}, {"page", 1}, {"layer_id", "p"},
                                                            {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({400, 0}), Json::array({400, 400}), Json::array({0, 400})})}}}, {"rgb", Json::array({0, 0, 255})}}}));
        QVERIFY(painted.smart_contents.at(outer).get() != was);
        QVERIFY(most_apart(drawn(painted), before) > 100);
        // (replaced inside: the same)
        const Document replaced = run(doc, Json::array({Json{{"op", "smart_replace"}, {"contents", outer}, {"page", 1}, {"id", "inner"},
                                                             {"data", core::b64encode(png_of(10, 10, render::Ink{0, 255, 0}))}}}));
        QVERIFY(replaced.smart_contents.at(outer).get() != was);
        QVERIFY(most_apart(drawn(replaced), before) > 100);
    }
};

QTEST_MAIN(TestSmartContents)
#include "test_smart_contents.moc"
