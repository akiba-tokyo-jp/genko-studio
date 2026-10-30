// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」; the user's decision 3: アートボード＝ページ): the artboards as an agent or a
// person uses them — add_artboard (a preset's size, a size given, its name, a page of its own paper, no panels, one empty
// layer, where it goes), artboard_from_layers (from layers and from a group: the page cut to what they show, the layers
// moved — or copied —, the core layers and すべてをロック kept and said, a slice of a layer moved going with it), set_artboard
// (size and anchor, contents kept or moved, the preset dropped, its background and name, the mark taken off), a cover's
// and a spread's refused; the background on the three ways of drawing (8 bits, precise colour, inks: as a fill layer of
// the colour under everything); the exports (to files: names, prefix, a name met again, JPEG's quality, a transparent
// background's alpha, the write guard; to PDF: its pages, their size on paper; one PSD as psd-tools reads it — Photoshop's
// artboards, their boards, backgrounds and presets — of 8-bit and 16-bit pages, refused of RGB and CMYK; `genko export
// --format artboards`); a PSD of artboards opened a page each (import_psd as before: one page, the groups kept); a
// .pcraft's artboard written and read back; the book written and read (artboards@1), one of a mark it cannot read or on a
// cover read-only; a book without artboards as before (no feature, no key, the same bytes, a PSD opened as one page).

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/artboards.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/artboards.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/pcraft_read.hpp"
#include "render/png.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops.is_array() ? ops : Json::array({ops}), core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& ops) { return run_ops(doc, ops).doc; }

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of `pages` pages 60 × 80 mm at 72 dpi, their panels taken away.
core::Document book(int pages = 1) {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(72);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    spec.expression = "color";
    core::Document doc = core::new_episode("ab", core::Num(1), pages, spec, core::Binding::Right);
    for (int p = 0; p < pages; ++p) doc.edit_page(static_cast<std::size_t>(p)).frames.clear();
    return doc;
}

Json op(const char* name, Json more = Json::object(), int page = 1) {
    Json out{{"op", name}, {"page", page}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}
Json paint(const std::string& id, int page = 1) { return op("add_layer", {{"kind", "paint"}, {"id", id}, {"name", id}}, page); }
Json drawn_in(const std::string& id, double x0, double y0, double x1, double y1, const Json& rgb, int page = 1) {
    return op("fill_area",
              {{"layer_id", id},
               {"area", Json{{"poly", Json::array({Json::array({x0, y0}), Json::array({x1, y0}), Json::array({x1, y1}), Json::array({x0, y1})})}}},
               {"rgb", rgb}},
              page);
}

const core::Page* page_of(const core::Document& doc, int number) {
    for (const auto& p : doc.pages)
        if (p->index == core::Num(number)) return p.get();
    return nullptr;
}
bool has_layer(const core::Page& page, const std::string& id) {
    return std::any_of(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == id; });
}
std::array<int, 2> pixels(const core::Page& page) {
    return {render::mm_to_px(page.spec.width_mm.value(), 72), render::mm_to_px(page.spec.height_mm.value(), 72)};
}
std::array<int, 3> rgb_at(const render::Image& image, int x, int y) {
    const std::vector<double> p = image.getpixel(x, y);
    return {static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])};
}
render::Image drawn(const core::Document& doc, const core::Page& page, bool precise = false) {
    render::RenderOptions options;
    options.mode = "print";
    options.precise = precise;
    return render::render_page(page, 72, options, &doc).image;
}
bool near(const std::array<int, 3>& a, const std::array<int, 3>& b, int by = 1) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(a[c] - b[c]) > by) return false;
    return true;
}
QByteArray shown(const std::array<int, 3>& c) { return QByteArray::number(c[0]) + "," + QByteArray::number(c[1]) + "," + QByteArray::number(c[2]); }
int count_of(const std::string& text, const std::string& what) {
    int n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

}  // namespace

class TestContractArtboards : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    int views_ = 0;
    Json psd_view(const QString& psd) {
        const QString out = path(QStringLiteral("artboards-%1.json").arg(++views_));
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/psd_artboards_check.py"), out, psd},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return genko::test::read_json(out)[psd.toStdString()];
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // add_artboard: a page marked an artboard after the page asked, of its own paper (the size in pixels at the book's
    // resolution, its finished size the paper, no bleed, its basic frame the paper), no panels or nombre, one empty layer.
    void makingArtboards() {
        core::Document doc = run(book(2), Json{{"op", "add_artboard"}, {"after", 1}, {"preset", "Web 1280"}});
        QCOMPARE(doc.pages.size(), std::size_t(3));
        const core::Page& made = *page_of(doc, 2);
        QVERIFY(made.artboard);
        QCOMPARE(made.artboard->name, std::string("アートボード 1"));
        QCOMPARE(made.artboard->preset.value_or(""), std::string("Web 1280"));
        QCOMPARE(made.artboard->background, std::string("white"));
        QCOMPARE(pixels(made), (std::array<int, 2>{1280, 800}));
        QVERIFY(made.own_spec);
        QCOMPARE(made.spec.bleed_mm.value(), 0.0);
        QCOMPARE(made.trim_rect_mm().width.value(), made.spec.width_mm.value());
        QCOMPARE(made.inner_rect_mm().width.value(), made.spec.width_mm.value());
        QVERIFY(made.frames.empty());
        QVERIFY(!made.numero);
        QCOMPARE(made.layers.size(), std::size_t(1));
        QCOMPARE(made.layers.front().title, std::string("レイヤー 1"));
        QVERIFY(!page_of(doc, 1)->artboard && !page_of(doc, 3)->artboard);
        // a size given by hand, a name; the preset's size changed by hand is no more the preset's
        doc = run(doc, Json{{"op", "add_artboard"}, {"width_px", 300}, {"height_px", 200}, {"name", "B"}, {"background", "black"}});
        const core::Page& b = *doc.pages.back();
        QCOMPARE(b.artboard->name, std::string("B"));
        QCOMPARE(pixels(b), (std::array<int, 2>{300, 200}));
        QVERIFY(!b.artboard->preset);
        doc = run(doc, Json{{"op", "add_artboard"}, {"preset", "Instagram Post"}, {"width_px", 1000}});
        QVERIFY(!doc.pages.back()->artboard->preset);
        QCOMPARE(doc.pages.back()->artboard->name, std::string("アートボード 2"));
        // Genko's finished sizes: mm at the book's resolution
        doc = run(doc, Json{{"op", "add_artboard"}, {"preset", "B5 仕上がり"}});
        QCOMPARE(pixels(*doc.pages.back()), (std::array<int, 2>{static_cast<int>(std::llround(182 / 25.4 * 72)), static_cast<int>(std::llround(257 / 25.4 * 72))}));
        // refused: an unknown preset, a resolution of its own, a background not known, a custom one without its colour
        QVERIFY(refusal(doc, Json{{"op", "add_artboard"}, {"preset", "nope"}}).find("no artboard preset nope") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_artboard"}, {"dpi", 300}}).find("the resolution is the whole book's") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_artboard"}, {"background", "pink"}}).find("background is white") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_artboard"}, {"background", "custom"}}).find("custom background takes its colour") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_artboard"}, {"width_px", 0}}).find("width_px is 1 to") != std::string::npos);
        // not repeated page by page, not inside a smart object's contents
        QVERIFY(refusal(doc, Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", "add_artboard"}}})}})
                    .find("cannot be repeated page by page") != std::string::npos);
        // undone as one step
        const core::ApplyResult undone = run_ops(doc, Json::array({Json{{"op", "add_artboard"}}, Json{{"op", "undo"}}}));
        QCOMPARE(undone.doc.pages.size(), doc.pages.size());
    }

    // A cover and a page of a spread are never artboards: the ops refuse them, a spread asked of an artboard is refused.
    void coversAndSpreadsRefused() {
        core::Document doc = run(book(2), Json{{"op", "add_artboard"}, {"name", "A"}});
        QVERIFY(refusal(doc, Json{{"op", "set_spread"}, {"page", 3}, {"with", 2}}).find("spread") != std::string::npos);
        QVERIFY(refusal(doc, op("set_artboard", {{"name", "X"}}, 1)).find("is not an artboard") != std::string::npos);
        core::Document spread = run(book(2), Json{{"op", "set_spread"}, {"page", 1}, {"with", 2}});
        spread = run(spread, Json::array({paint("a"), drawn_in("a", 5, 5, 20, 20, Json::array({200, 0, 0}))}));
        // (the new artboard is not in a spread: from layers of a page of a spread is fine)
        QVERIFY(refusal(spread, op("artboard_from_layers", {{"ids", Json::array({"a"})}})).empty());
        core::Document marked = doc;
        marked.edit_page(2).spread_with = core::Num(2);
        QVERIFY(refusal(marked, op("set_artboard", {{"name", "X"}}, 3)).find("spread") != std::string::npos);
        // a cover
        core::Document covered = run(book(1), Json{{"op", "add_cover"}, {"kind", "front"}});
        const std::size_t last = covered.pages.size() - 1;
        covered.edit_page(last).artboard = core::Artboard{"C", std::nullopt, "white", std::nullopt, std::nullopt};
        QVERIFY_THROWS_EXCEPTION(core::Error, core::validate_artboards(covered));
    }

    // artboard_from_layers: from layers and from a group — the new page right after, its paper the box of what they show,
    // the layers on it where they were, taken off the page (copied with remove false); the core layers and those locked
    // with すべてをロック kept and said; a layer-based slice of a layer moved going with it.
    void fromLayersAndGroups() {
        core::Document doc = run(book(), Json::array({paint("a"), drawn_in("a", 10, 10, 30, 20, Json::array({200, 0, 0})), paint("b"),
                                                     drawn_in("b", 40, 50, 50, 70, Json::array({0, 0, 200})), op("add_slice", {{"layer", "a"}})}));
        QVERIFY(page_of(doc, 1)->slices && !page_of(doc, 1)->slices->list.empty());
        const core::ApplyResult made = run_ops(doc, op("artboard_from_layers", {{"ids", Json::array({"a"})}, {"name", "赤"}}));
        doc = made.doc;
        QCOMPARE(doc.pages.size(), std::size_t(2));
        const core::Page& board = *page_of(doc, 2);
        QVERIFY(board.artboard);
        QCOMPARE(board.artboard->name, std::string("赤"));
        QVERIFY(std::abs(board.spec.width_mm.value() - 20) < 0.2 && std::abs(board.spec.height_mm.value() - 10) < 0.2);
        QCOMPARE(board.layers.size(), std::size_t(1));
        QVERIFY(!has_layer(*page_of(doc, 1), "a") && has_layer(*page_of(doc, 1), "b"));
        // (a slice of a layer taken off goes with it: J-b's)
        QVERIFY(!page_of(doc, 1)->slices || page_of(doc, 1)->slices->list.empty());
        const render::Image picture = drawn(doc, board);
        QVERIFY2(near(rgb_at(picture, picture.width() / 2, picture.height() / 2), {200, 0, 0}, 2), shown(rgb_at(picture, picture.width() / 2, picture.height() / 2)));
        // a group: its name, the group kept on the artboard; remove false copies
        core::Document grouped = run(book(), Json::array({paint("c"), drawn_in("c", 5, 5, 15, 15, Json::array({0, 150, 0})), paint("d"),
                                                         op("group_layers", {{"ids", Json::array({"c", "d"})}, {"id", "g"}, {"name", "群"}, {"group", true}})}));
        grouped = run(grouped, op("artboard_from_layers", {{"ids", Json::array({"g"})}, {"remove", false}}));
        const core::Page& copy = *page_of(grouped, 2);
        QCOMPARE(copy.artboard->name, std::string("群"));
        QCOMPARE(copy.layers.size(), std::size_t(3));
        QVERIFY(std::any_of(copy.layers.begin(), copy.layers.end(), [](const core::Layer& l) { return l.kind == core::LayerKind::Folder && l.group; }));
        QVERIFY(has_layer(*page_of(grouped, 1), "g") && has_layer(*page_of(grouped, 1), "c"));
        // the core layers and すべてをロック kept and said
        const std::string ink = page_of(grouped, 1)->first_layer(core::LayerRole::Ink)->id;
        core::Document locked = run(grouped, Json::array({op("set_layer", {{"id", "c"}, {"lock_all", true}})}));
        const core::ApplyResult moved = run_ops(locked, op("artboard_from_layers", {{"ids", Json::array({"c", ink})}}));
        const Json kept = moved.results.at(0).value("kept", Json::array());
        QVERIFY2(kept.dump().find("c") != std::string::npos && kept.dump().find(ink) != std::string::npos, kept.dump().c_str());
        QVERIFY(has_layer(*page_of(moved.doc, 1), "c") && has_layer(*page_of(moved.doc, 1), ink));
        // refused: no ids, an id the page has not
        QVERIFY(!refusal(doc, op("artboard_from_layers", {{"ids", Json::array()}})).empty());
        QVERIFY(!refusal(doc, op("artboard_from_layers", {{"ids", Json::array({"nope"})}})).empty());
    }

    // set_artboard: its size as Canvas Size changes it from the anchor (the contents moved, or kept and the board's place
    // on a PSD's canvas moved), a preset's size, the preset dropped by a size given by hand, its name and background, the
    // mark taken off; a page op for すべてをロック.
    void settingArtboards() {
        core::Document doc = run(book(), Json{{"op", "add_artboard"}, {"width_px", 100}, {"height_px", 100}, {"preset", "Instagram Post"}});
        doc = run(doc, Json::array({op("add_layer", {{"kind", "paint"}, {"id", "p"}}, 2), drawn_in("p", 0, 0, 5, 5, Json::array({0, 0, 0}), 2),
                                    op("set_layer", {{"id", "p"}, {"lock_all", true}}, 2)}));
        QVERIFY(!doc.page(1).artboard->preset);
        doc = run(doc, op("set_artboard", {{"origin_px", Json::array({500, 40})}, {"name", "N"}, {"background", "custom"}, {"rgb", Json::array({10, 20, 30})}}, 2));
        QCOMPARE(doc.page(1).artboard->name, std::string("N"));
        QCOMPARE(doc.page(1).artboard->rgb.value(), (std::array<int, 3>{10, 20, 30}));
        // the anchor at the centre: the contents moved by half the growth, the board's place by minus that
        const render::Image before = drawn(doc, doc.page(1));
        doc = run(doc, op("set_artboard", {{"width_px", 140}, {"height_px", 100}, {"anchor", "center"}}, 2));
        QCOMPARE(pixels(doc.page(1)), (std::array<int, 2>{140, 100}));
        QCOMPARE(doc.page(1).artboard->origin_px.value(), (std::array<std::int64_t, 2>{480, 40}));
        const render::Image wider = drawn(doc, doc.page(1));
        QVERIFY2(near(rgb_at(wider, 20 + 3, 3), rgb_at(before, 3, 3), 2), shown(rgb_at(wider, 23, 3)));
        // move_contents false: the contents at the top left, the board's place moved
        doc = run(doc, op("set_artboard", {{"width_px", 180}, {"anchor", "right"}, {"move_contents", false}}, 2));
        QCOMPARE(doc.page(1).artboard->origin_px.value(), (std::array<std::int64_t, 2>{440, 40}));
        const render::Image kept = drawn(doc, doc.page(1));
        QVERIFY2(near(rgb_at(kept, 23, 3), rgb_at(wider, 23, 3), 2), shown(rgb_at(kept, 23, 3)));
        // a preset's size
        doc = run(doc, op("set_artboard", {{"preset", "Android 1080p"}}, 2));
        QCOMPARE(pixels(doc.page(1)), (std::array<int, 2>{360, 640}));
        QCOMPARE(doc.page(1).artboard->preset.value_or(""), std::string("Android 1080p"));
        doc = run(doc, op("set_artboard", {{"height_px", 600}}, 2));
        QVERIFY(!doc.page(1).artboard->preset);
        // the mark taken off: a page of its own paper
        const core::Document plain = run(doc, op("set_artboard", {{"artboard", nullptr}}, 2));
        QVERIFY(!plain.page(1).artboard && plain.page(1).own_spec);
        QVERIFY(refusal(doc, op("set_artboard", {{"artboard", nullptr}, {"name", "x"}}, 2)).find("cannot go with artboard null") != std::string::npos);
        QVERIFY(refusal(doc, op("set_artboard", {{"anchor", "middle"}}, 2)).find("anchor is") != std::string::npos);
    }

    // The background on the three ways of drawing: black or a colour of its own under the page's layers, the same pixels
    // as a fill layer of that colour under everything — 8 bits, precise colour (16 bits) and inks (CMYK).
    void backgroundOnThreeWays() {
        for (const char* way : {"8", "16", "cmyk"}) {
            core::Document doc = run(book(), Json{{"op", "add_artboard"}, {"width_px", 60}, {"height_px", 40}, {"background", "custom"}, {"rgb", Json::array({200, 120, 40})}});
            doc = run(doc, Json::array({op("add_layer", {{"kind", "paint"}, {"id", "p"}}, 2), drawn_in("p", 2, 2, 8, 8, Json::array({0, 0, 255}), 2)}));
            if (std::string(way) == "16") doc = run(doc, op("set_color_mode", {{"mode", "rgb"}, {"depth", 16}}, 2));
            if (std::string(way) == "cmyk") doc = run(doc, op("set_color_mode", {{"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}}, 2));
            // the same page, white, with a fill layer of the colour at the bottom
            core::Document filled = run(doc, op("set_artboard", {{"background", "white"}}, 2));
            filled = run(filled, op("add_layer", {{"kind", "fill"}, {"id", "f"}, {"rgb", Json::array({200, 120, 40})}}, 2));
            {
                core::Page& page = filled.edit_page(1);
                auto f = std::find_if(page.layers.begin(), page.layers.end(), [](const core::Layer& l) { return l.id == "f"; });
                core::Layer fill = *f;
                page.layers.erase(f);
                page.layers.insert(page.layers.begin(), fill);
            }
            const render::Image ours = drawn(doc, doc.page(1), std::string(way) != "8");
            const render::Image theirs = drawn(filled, filled.page(1), std::string(way) != "8");
            QCOMPARE(ours.width(), theirs.width());
            for (const auto& [x, y] : {std::pair{1, 1}, std::pair{30, 30}, std::pair{10, 10}}) {
                QVERIFY2(near(rgb_at(ours, x, y), rgb_at(theirs, x, y), 1), (std::string(way) + ": " + shown(rgb_at(ours, x, y)).toStdString() + " vs " +
                                                                            shown(rgb_at(theirs, x, y)).toStdString()).c_str());
            }
            if (std::string(way) != "cmyk") QVERIFY2(near(rgb_at(ours, 30, 30), {200, 120, 40}, 1), shown(rgb_at(ours, 30, 30)));
            // black
            const core::Document black = run(doc, op("set_artboard", {{"background", "black"}}, 2));
            QVERIFY(rgb_at(drawn(black, black.page(1), std::string(way) != "8"), 30, 30)[0] < 60);
        }
    }

    // ファイル → 書き出し → アートボードからファイル… and PDF…: the files' names, sizes and formats, JPEG's quality, a transparent
    // background's alpha, the write guard; the PDF's pages and their size on paper; through `genko export`.
    void exportsToFilesAndPdf() {
        core::Document doc = run(book(), Json::array({Json{{"op", "add_artboard"}, {"width_px", 120}, {"height_px", 80}, {"name", "表紙/案"}},
                                                      Json{{"op", "add_artboard"}, {"width_px", 90}, {"height_px", 60}, {"name", "B"}, {"background", "transparent"}},
                                                      Json{{"op", "add_artboard"}, {"width_px", 30}, {"height_px", 30}, {"name", "B"}, {"background", "black"}}}));
        doc = run(doc, Json::array({op("add_layer", {{"kind", "paint"}, {"id", "q"}}, 3), drawn_in("q", 0, 0, 10, 10, Json::array({255, 0, 0}), 3)}));
        const fs::path out = core::path_from_utf8(path(QStringLiteral("files")).toStdString());
        fs::create_directories(out);
        formats::ArtboardFiles files;
        files.prefix = std::string("p");
        const auto written = formats::export_artboards(doc, out, files);
        QCOMPARE(written.size(), std::size_t(3));
        QCOMPARE(core::path_to_utf8(written[0].file.filename()), std::string("p_表紙_案.png"));
        QCOMPARE(core::path_to_utf8(written[1].file.filename()), std::string("p_B.png"));
        QCOMPARE(core::path_to_utf8(written[2].file.filename()), std::string("p_B_2.png"));
        const render::Image first = render::read_png(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(written[0].file))));
        QCOMPARE(first.width(), 120);
        QCOMPARE(first.height(), 80);
        // a transparent background: its alpha (nothing where nothing is drawn, the red opaque)
        const render::Image clear = render::read_png(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(written[1].file))));
        QCOMPARE(clear.mode(), std::string("RGBA"));
        QCOMPARE(clear.getpixel(80, 50)[3], 0.0);
        QCOMPARE(clear.getpixel(5, 5)[3], 255.0);
        QVERIFY(clear.getpixel(5, 5)[0] > 250 && clear.getpixel(5, 5)[1] < 5);
        // the artboards chosen, the book's title as the prefix, JPEG's quality
        formats::ArtboardFiles some;
        some.artboards = {"表紙/案"};
        some.extension = "jpg";
        some.options.quality = 0;
        const auto low = formats::export_artboards(doc, out, some);
        QCOMPARE(low.size(), std::size_t(1));
        QCOMPARE(core::path_to_utf8(low[0].file.filename()), std::string("ab_表紙_案.jpg"));
        some.options.quality = 12;
        some.prefix = std::string();
        const auto high = formats::export_artboards(doc, out, some);
        QCOMPARE(core::path_to_utf8(high[0].file.filename()), std::string("表紙_案.jpg"));
        QVERIFY(fs::file_size(high[0].file) > fs::file_size(low[0].file));
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_artboards(doc, out, formats::ArtboardFiles{"png", std::nullopt, {"nope"}, {}, {}}));
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_artboards(book(), out, {}));
        // the write guard: never into the book
        const fs::path book_dir = core::path_from_utf8(path(QStringLiteral("guarded.genko")).toStdString());
        genko::test::write_project(doc, book_dir);
        const formats::WriteGuard guard = formats::write_guard(doc, book_dir);
        formats::ArtboardFiles guarded;
        guarded.check = [guard](const fs::path& file) { formats::check_target(guard, file); };
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_artboards(doc, book_dir, guarded));
        // the PDF: a page an artboard, its size on paper
        const fs::path pdf = out / "all.pdf";
        const formats::ArtboardPdfResult made = formats::export_artboards_pdf(doc, pdf, {});
        QCOMPARE(made.pages, std::size_t(3));
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(pdf)));
        QVERIFY(bytes.starts_with("%PDF-1.4"));
        QCOMPARE(count_of(bytes, "/Type /Page "), 3);
        QVERIFY(bytes.find("/MediaBox [0 0 120.000 80.000]") != std::string::npos);
        QCOMPARE(count_of(bytes, "/Filter /DCTDecode"), 3);
        formats::ArtboardPdf one;
        one.artboards = {"表紙/案"};
        one.quality = 0;
        QCOMPARE(formats::export_artboards_pdf(doc, out / "one.pdf", one).pages, std::size_t(1));
        // `genko export --format artboards`
        const QString cli_out = path(QStringLiteral("cli"));
        auto ran = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book_dir)), cli_out, QStringLiteral("--format"),
                                           QStringLiteral("artboards"), QStringLiteral("--prefix"), QStringLiteral("x"), QStringLiteral("--json")});
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        Json said = genko::test::one_line(ran.out);
        QCOMPARE(said.value("count", 0), 3);
        ran = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book_dir)), path(QStringLiteral("cli.pdf")),
                                      QStringLiteral("--format"), QStringLiteral("artboards"), QStringLiteral("--artboard-format"), QStringLiteral("pdf"),
                                      QStringLiteral("--artboards"), QStringLiteral("B"), QStringLiteral("--json")});
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        said = genko::test::one_line(ran.out);
        QCOMPARE(said.value("pages", 0), 2);
        // (into the book: refused)
        ran = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book_dir)), QString::fromStdString(core::path_to_utf8(book_dir)),
                                      QStringLiteral("--format"), QStringLiteral("artboards")});
        QVERIFY(ran.exit_code != 0);
        ran = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book_dir)), cli_out, QStringLiteral("--artboards"), QStringLiteral("B")});
        QVERIFY(ran.exit_code != 0);
    }

    // One PSD of the artboards: Photoshop's artboards as psd-tools reads them (their boards where they lie — their places,
    // or in a row 100 pixels apart —, background types and presets), opened again a page each; 8-bit and 16-bit pages made
    // one 16-bit PSD; RGB and CMYK refused; import_psd of it as before (one page, the groups kept as groups).
    void onePsdOfArtboards() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        core::Document doc = run(book(), Json::array({Json{{"op", "add_artboard"}, {"preset", "Instagram Post"}, {"width_px", 120}, {"height_px", 80}, {"name", "A"}},
                                                      Json{{"op", "add_artboard"}, {"preset", "Android 1080p"}, {"name", "B"}, {"background", "black"}},
                                                      Json{{"op", "add_artboard"}, {"width_px", 50}, {"height_px", 50}, {"name", "C"}, {"background", "custom"},
                                                           {"rgb", Json::array({10, 200, 30})}}}));
        doc = run(doc, Json::array({op("add_layer", {{"kind", "paint"}, {"id", "a1"}, {"name", "a1"}}, 2), drawn_in("a1", 0, 0, 10, 10, Json::array({255, 0, 0}), 2),
                                    op("set_artboard", {{"origin_px", Json::array({0, 300})}}, 4)}));
        const QString file = path(QStringLiteral("boards.psd"));
        const fs::path dest = core::path_from_utf8(file.toStdString());
        formats::ArtboardPsd options;
        const formats::CopyResult made = formats::export_artboards_psd(doc, dest, options);
        QVERIFY(fs::exists(dest));
        const Json view = psd_view(file);
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        QCOMPARE(view["depth"].get<int>(), 8);
        const Json& layers = view["layers"];
        QCOMPARE(layers.size(), std::size_t(3));
        // (top first: the last artboard on top)
        QCOMPARE(layers[2]["name"].get<std::string>(), std::string("A"));
        QCOMPARE(layers[2]["kind"].get<std::string>(), std::string("artboard"));
        QCOMPARE(layers[2]["bbox"], Json::array({0, 0, 120, 80}));
        QCOMPARE(layers[2]["background"].get<int>(), 1);
        QCOMPARE(layers[2]["children"], Json::array({"a1", "レイヤー 1"}));
        QCOMPARE(layers[1]["name"].get<std::string>(), std::string("B"));
        QCOMPARE(layers[1]["bbox"], Json::array({220, 0, 580, 640}));
        QCOMPARE(layers[1]["background"].get<int>(), 2);
        QCOMPARE(layers[1]["preset"].get<std::string>(), std::string("Android 1080p"));
        QCOMPARE(layers[0]["bbox"], Json::array({0, 300, 50, 350}));
        QCOMPARE(layers[0]["background"].get<int>(), 4);
        QCOMPARE(layers[0]["colour"], Json::array({10, 200, 30}));
        QCOMPARE(view["width"].get<int>(), 580);
        QCOMPARE(view["height"].get<int>(), 640);
        QCOMPARE(made.width, std::uint32_t(580));
        // opened again: a page each, its name, size, background, preset and place
        const formats::Opened opened = formats::open_file(genko::test::read_bytes(file), "boards.psd");
        QCOMPARE(opened.doc.pages.size(), std::size_t(3));
        const std::vector<std::string> names{"A", "B", "C"};
        for (std::size_t i = 0; i < 3; ++i) {
            const core::Page& page = opened.doc.page(i);
            QVERIFY(page.artboard);
            QCOMPARE(page.artboard->name, names[i]);
        }
        QCOMPARE(pixels(opened.doc.page(0)), (std::array<int, 2>{120, 80}));
        QCOMPARE(pixels(opened.doc.page(1)), (std::array<int, 2>{360, 640}));
        QCOMPARE(opened.doc.page(1).artboard->background, std::string("black"));
        QCOMPARE(opened.doc.page(1).artboard->preset.value_or(""), std::string("Android 1080p"));
        QCOMPARE(opened.doc.page(2).artboard->rgb.value(), (std::array<int, 3>{10, 200, 30}));
        QCOMPARE(opened.doc.page(2).artboard->origin_px.value(), (std::array<std::int64_t, 2>{0, 300}));
        QVERIFY(std::any_of(opened.doc.page(0).layers.begin(), opened.doc.page(0).layers.end(), [](const core::Layer& l) { return l.title == "a1"; }));
        QVERIFY(std::none_of(opened.doc.page(0).layers.begin(), opened.doc.page(0).layers.end(), [](const core::Layer& l) { return l.kind == core::LayerKind::Folder; }));
        const render::Image red = drawn(opened.doc, opened.doc.page(0));
        QVERIFY2(near(rgb_at(red, 5, 5), {255, 0, 0}, 3), shown(rgb_at(red, 5, 5)));
        QVERIFY(std::any_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("アートボード 3 枚") != std::string::npos; }));
        // import_psd as before: one page, the artboards' groups kept as groups
        core::Document imported = book();
        imported.asset_dir = core::path_from_utf8(tmp_.path().toStdString());
        imported = run(imported, Json{{"op", "import_psd"}, {"page", 1}, {"path", file.toStdString()}});
        QCOMPARE(imported.pages.size(), std::size_t(1));
        QCOMPARE(std::count_if(imported.page(0).layers.begin(), imported.page(0).layers.end(), [](const core::Layer& l) { return l.kind == core::LayerKind::Folder; }), 3);
        // 8-bit and 16-bit: one 16-bit PSD
        const core::Document deep = run(doc, op("set_color_mode", {{"mode", "rgb"}, {"depth", 16}}, 3));
        const QString deep_file = path(QStringLiteral("deep.psd"));
        formats::export_artboards_psd(deep, core::path_from_utf8(deep_file.toStdString()), options);
        QCOMPARE(psd_view(deep_file)["depth"].get<int>(), 16);
        // RGB and CMYK: refused
        const core::Document mixed = run(doc, op("set_color_mode", {{"mode", "cmyk"}, {"profile", "coated-cmyk"}}, 3));
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_artboards_psd(mixed, core::path_from_utf8(path(QStringLiteral("mixed.psd")).toStdString()), options));
        // `genko export --one-psd`
        const fs::path book_dir = core::path_from_utf8(path(QStringLiteral("psd.genko")).toStdString());
        genko::test::write_project(doc, book_dir);
        const auto ran = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(book_dir)), path(QStringLiteral("cli.psd")),
                                                 QStringLiteral("--format"), QStringLiteral("artboards"), QStringLiteral("--artboard-format"), QStringLiteral("psd"),
                                                 QStringLiteral("--one-psd"), QStringLiteral("--json")});
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        QCOMPARE(psd_view(path(QStringLiteral("cli.psd")))["layers"].size(), std::size_t(3));
    }

    // A .pcraft of an artboard: PhotoCraft's artboard over the whole document (a top-level group, its board, background
    // and preset), opened again as an artboard page.
    void pcraftArtboards() {
        core::Document doc = run(book(), Json{{"op", "add_artboard"}, {"width_px", 64}, {"height_px", 48}, {"name", "P"}, {"preset", "Web 1280"},
                                             {"background", "custom"}, {"rgb", Json::array({20, 40, 60})}});
        doc = run(doc, Json::array({op("add_layer", {{"kind", "paint"}, {"id", "x"}, {"name", "x"}}, 2), drawn_in("x", 0, 0, 5, 5, Json::array({0, 0, 0}), 2)}));
        const fs::path dest = core::path_from_utf8(path(QStringLiteral("board.pcraft")).toStdString());
        formats::write_copy(doc, 1, dest);
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dest)));
        const Json manifest = render::pcraft::read_manifest(bytes);
        const Json& layers = manifest.at("document").at("layers");
        QCOMPARE(layers.size(), std::size_t(1));
        const Json& board = layers[0].at("content").at("artboard");
        QCOMPARE(layers[0]["name"].get<std::string>(), std::string("P"));
        QCOMPARE(board.at("rect"), (Json{{"x0", 0}, {"y0", 0}, {"x1", 64}, {"y1", 48}}));
        QVERIFY(board.at("background").contains("custom"));
        QCOMPARE(board.at("preset").get<std::string>(), std::string());
        const formats::Opened opened = formats::open_file(bytes, "board.pcraft");
        QCOMPARE(opened.doc.pages.size(), std::size_t(1));
        QVERIFY(opened.doc.page(0).artboard);
        QCOMPARE(opened.doc.page(0).artboard->name, std::string("P"));
        QCOMPARE(opened.doc.page(0).artboard->background, std::string("custom"));
        QCOMPARE(opened.doc.page(0).artboard->rgb.value(), (std::array<int, 3>{20, 40, 60}));
        QCOMPARE(pixels(opened.doc.page(0)), (std::array<int, 2>{64, 48}));
        QVERIFY(std::none_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("PC5 で入るまで") != std::string::npos; }));
    }

    // The book written and read again (artboards@1 named while a page is an artboard); a mark it cannot read, or on a
    // cover, opens read-only; a book without artboards as before — no feature, no key, the same bytes, its PSD opened as one
    // page.
    void theBookAndBooksOfNone() {
        core::Document doc = run(book(), Json{{"op", "add_artboard"}, {"width_px", 64}, {"height_px", 48}, {"background", "custom"}, {"rgb", Json::array({1, 2, 3})}});
        doc = run(doc, op("set_artboard", {{"origin_px", Json::array({-5, 7})}}, 2));
        const fs::path dir = core::path_from_utf8(path(QStringLiteral("saved.genko")).toStdString());
        genko::test::write_project(doc, dir);
        const Json project = genko::test::read_json(QString::fromStdString(core::path_to_utf8(dir / "project.json")));
        QVERIFY(project["features"].dump().find("artboards@1") != std::string::npos);
        QCOMPARE(project["pages"][1]["artboard"], (Json{{"name", "アートボード 1"}, {"background", "custom"}, {"rgb", Json::array({1, 2, 3})}, {"origin_px", Json::array({-5, 7})}}));
        QVERIFY(!project["pages"][0].contains("artboard"));
        const storage::LoadResult read = storage::load_document(dir);
        QVERIFY(read.report.clean());
        QVERIFY(read.document.page(1).artboard == doc.page(1).artboard);
        // a mark it cannot read: read-only, kept
        Json broken = project;
        broken["pages"][1]["artboard"]["background"] = "pink";
        const storage::LoadResult bad = storage::load_document_payload(broken, dir);
        QVERIFY(!bad.report.clean());
        Json unknown = project;
        unknown["pages"][1]["artboard"]["board"] = 1;
        QVERIFY(!storage::load_document_payload(unknown, dir).report.clean());
        // a book without artboards: no feature, no key, the same bytes twice
        const core::Document plain = run(book(2), Json::array({paint("a"), drawn_in("a", 1, 1, 9, 9, Json::array({9, 9, 9}))}));
        const fs::path one = core::path_from_utf8(path(QStringLiteral("plain1.genko")).toStdString());
        const fs::path two = core::path_from_utf8(path(QStringLiteral("plain2.genko")).toStdString());
        genko::test::write_project(plain, one);
        genko::test::write_project(storage::load_document(one).document, two);
        const std::string first = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(one / "project.json")));
        QCOMPARE(first, genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(two / "project.json"))));
        QVERIFY(first.find("artboard") == std::string::npos);
        // its PSD opened as one page, no notes of artboards
        const fs::path psd = core::path_from_utf8(path(QStringLiteral("plain.psd")).toStdString());
        formats::write_copy(plain, 0, psd);
        const formats::Opened opened = formats::open_file(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(psd))), "plain.psd");
        QCOMPARE(opened.doc.pages.size(), std::size_t(1));
        QVERIFY(!opened.doc.page(0).artboard);
        QVERIFY(std::none_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("アートボード") != std::string::npos; }));
    }
};

QTEST_GUILESS_MAIN(TestContractArtboards)
#include "test_contract_artboards.moc"
