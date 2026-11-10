// The manuscript model against Python's models.py: paper presets (Python's own numbers from
// tests/data/pyref/unit_tables.json), page sides and binding, reading order, frame edits, new books and
// copy-on-write pages.

#include <QtTest>

#include <set>
#include <string>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "testsupport.hpp"

using genko::core::Binding;
using genko::core::Document;
using genko::core::Frame;
using genko::core::Json;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Num;
using genko::core::Page;
using genko::core::PageSpec;
using genko::core::Rect;

namespace {

Json rect_json(const Rect& r) { return Json::array({r.x.json(), r.y.json(), r.width.json(), r.height.json()}); }

void expect_json(const Json& got, const Json& want, const char* what) {
    std::string where;
    QVERIFY2(genko::test::strict_equal(got, want, &where), (std::string(what) + ": " + where).c_str());
}

Frame frame(std::string id, Num x, Num y, Num w, Num h) {
    Frame f;
    f.id = std::move(id);
    f.rect = Rect{x, y, w, h};
    return f;
}

std::vector<std::string> leaf_ids(const Page& page) {
    std::vector<std::string> out;
    for (const Frame* f : page.leaf_frames()) out.push_back(f->id);
    return out;
}

Page page_with(Frame root, Binding binding = Binding::Right) {
    Page page;
    page.index = 1;
    page.spec = PageSpec::a4_mono();
    page.binding = binding;
    page.frames.push_back(std::move(root));
    return page;
}

PageSpec spec_named(const std::string& name) {
    if (name == "b4") return PageSpec::b4_comic();
    if (name == "b5") return PageSpec::b5_doujin();
    if (name == "a5") return PageSpec::a5_doujin();
    if (name == "a4") return PageSpec::a4_mono();
    if (name == "webtoon") return PageSpec::webtoon();
    if (name == "custom") return PageSpec::custom(200.5, 280, 180, 260.25, 3, 12, 12.5, 10, 8);
    if (name == "shueisha") return PageSpec::publisher(" Shueisha ");
    return PageSpec::publisher("x");
}

}  // namespace

class TestModel : public QObject {
    Q_OBJECT

private slots:
    void presetsMatchPython() {
        const Json table = genko::test::read_json(genko::test::test_data("pyref/unit_tables.json"));
        QCOMPARE(table["specs"].size(), std::size_t{8});
        for (const auto& [name, want] : table["specs"].items()) {
            const PageSpec spec = spec_named(name);
            const std::string label = "spec " + name;
            expect_json(spec.width_mm.json(), want["width_mm"], label.c_str());
            expect_json(spec.height_mm.json(), want["height_mm"], label.c_str());
            expect_json(spec.dpi.json(), want["dpi"], label.c_str());
            QCOMPARE(spec.preset.value_or(""), want["preset"].get<std::string>());
            const auto [tw, th] = spec.trim_size();
            expect_json(Json::array({tw.json(), th.json()}), want["trim_size"], "trim_size");
            const auto [ox, oy] = spec.trim_origin();
            expect_json(Json::array({ox, oy}), want["trim_origin"], "trim_origin");
            const auto m = spec.margins();
            expect_json(Json::object({{"top", m.top}, {"bottom", m.bottom}, {"inner", m.inner}, {"outer", m.outer}}),
                        want["margins"], "margins");
            const auto [fw, fh] = spec.frame_size();
            expect_json(Json::array({fw, fh}), want["frame_size"], "frame_size");
            QCOMPARE(spec.describe(), want["describe"].get<std::string>());
            for (const auto& [key, page_want] : want["pages"].items()) {
                Page page;
                page.spec = spec;
                page.binding = key.starts_with("right") ? Binding::Right : Binding::Left;
                page.index = key.back() == '1' ? 1 : 2;
                const std::string where = label + " " + key;
                QCOMPARE(page.side(), page_want["side"].get<std::string>());
                QCOMPARE(page.binding_edge(), page_want["edge"].get<std::string>());
                QCOMPARE(page.side("left"), page_want["side_left"].get<std::string>());
                expect_json(rect_json(page.inner_rect_mm()), page_want["inner"], (where + " inner").c_str());
                expect_json(rect_json(page.inner_rect_mm("left")), page_want["inner_start_left"], (where + " inner(left)").c_str());
                expect_json(rect_json(page.trim_rect_mm()), page_want["trim"], (where + " trim").c_str());
                expect_json(rect_json(page.bleed_rect_mm()), page_want["bleed"], (where + " bleed").c_str());
                expect_json(page.spread_step_mm().json(), page_want["step"], (where + " step").c_str());
            }
        }
    }

    void explicitPresetValues() {
        // (the numbers the presets promise, written out)
        const auto b4 = PageSpec::b4_comic();
        QVERIFY(b4.trim_size().first.same(Num(220.0)));
        QCOMPARE(b4.frame_size(), std::make_pair(180.0, 270.0));
        const auto a4 = PageSpec::a4_mono();
        QVERIFY(a4.trim_size().first.same(Num(204)));  // no trim: the paper less the bleed, still an int
        QVERIFY(a4.trim_size().second.same(Num(291)));
        QCOMPARE(PageSpec::b5_doujin().frame_size(), std::make_pair(150.0, 220.0));
        QCOMPARE(PageSpec::a5_doujin().frame_size(), std::make_pair(120.0, 180.0));
        QCOMPARE(PageSpec::webtoon().expression, std::string("color"));
        Page right;
        right.spec = PageSpec::custom(200.5, 280, 180, 260.25, 3, 12, 12.5, 10, 8);
        right.index = 1;
        // page 1 of a right-bound book is on the left: its binding (のど, 10 mm) is on its right
        QCOMPARE(right.inner_rect_mm().x.value(), 10.25 + 8);
        right.index = 2;
        QCOMPARE(right.inner_rect_mm().x.value(), 10.25 + 10);
    }

    void customAndPublisherPresets() {
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, PageSpec::custom(100, 100, 98, 90, 3, 10, 10, 10, 10));
        try {
            PageSpec::custom(200, 300, 180, 260, 3, 130, 130, 10, 10);
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("the basic frame must fit inside the finished size"));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, PageSpec::custom(200, 300, 180, 260, -1, 10, 10, 10, 10));
        const auto spec = PageSpec::custom(200, 300, 180, 260, 3, 10, 11, 12, 9.5, 350);
        QVERIFY(spec.width_mm.same(Num(200.0)));
        QVERIFY(spec.dpi.same(Num(350)));
        QVERIFY(spec.inner_margin_mm.same(Num(9.5)));
        QCOMPARE(PageSpec::publisher("KADOKAWA").preset.value(), std::string("kadokawa"));
        QCOMPARE(PageSpec::publisher("acme").preset.value(), std::string("none"));
        const auto presets = genko::core::paper_presets();
        QCOMPARE(presets.size(), std::size_t{5});
        QCOMPARE(presets[0].key, std::string_view("b4"));
        QCOMPARE(genko::core::find_paper_preset("webtoon")->make().preset.value(), std::string("webtoon"));
        QVERIFY(genko::core::find_paper_preset("b3") == nullptr);
    }

    void coverSpecsMatchPython() {
        const Json table = genko::test::read_json(genko::test::test_data("pyref/unit_tables.json"));
        QVERIFY(table["covers"].size() >= 12);
        for (const Json& row : table["covers"]) {
            const std::string book = row["book"].get<std::string>();
            const PageSpec base = book == "commercial-b4" ? PageSpec::b4_comic()
                                  : book == "a4-mono"     ? PageSpec::a4_mono()
                                                          : PageSpec::b5_doujin();
            const PageSpec spec = genko::core::spec_for(base, row["cover"]);
            const std::string label = book + " " + genko::core::dump_python(row["cover"]);
            expect_json(spec.width_mm.json(), row["width_mm"], label.c_str());
            expect_json(spec.height_mm.json(), row["height_mm"], label.c_str());
            expect_json(spec.trim_w_mm ? spec.trim_w_mm->json() : Json(nullptr), row["trim_w_mm"], label.c_str());
            expect_json(spec.trim_h_mm ? spec.trim_h_mm->json() : Json(nullptr), row["trim_h_mm"], label.c_str());
            QCOMPARE(spec.preset.value_or(""), row["preset"].get<std::string>());
            Json margins = Json(nullptr);
            if (spec.margins_mm) {
                margins = Json::array();
                for (const auto& v : *spec.margins_mm) margins.push_back(v.json());
            }
            expect_json(margins, row["margins_mm"], label.c_str());
        }
    }

    void pageSidesAndBinding() {
        Page page;
        page.spec = PageSpec::a4_mono();
        page.index = 1;
        QCOMPARE(page.side(), std::string("left"));  // right-bound: page 1 on the left
        QCOMPARE(page.binding_edge(), std::string("right"));
        QVERIFY(page.is_recto());
        page.index = 2;
        QCOMPARE(page.side(), std::string("right"));
        QCOMPARE(page.side("right"), std::string("left"));
        page.binding = Binding::Left;
        QCOMPARE(page.side(), std::string("left"));
        page.index = 3;
        QCOMPARE(page.side(), std::string("right"));
        page.index = 1.0;  // (a float page number from an old file: Python's 1.0 % 2 == 1)
        QCOMPARE(page.side(), std::string("right"));
        page.index = -1;  // (Python: -1 % 2 == 1)
        QVERIFY(page.is_recto());
    }

    void readingOrderVerticalAndHorizontal() {
        Page page = page_with(frame("root", 0, 0, 100, 100));
        page.split_frame("root", "vertical", 0.5, 4);
        QCOMPARE(leaf_ids(page), std::vector<std::string>({page.frames[0].children[1].id, page.frames[0].children[0].id}));
        Page tall = page_with(frame("root", 0, 0, 100, 100));
        tall.split_frame("root", "horizontal", 0.5, 4);
        QCOMPARE(leaf_ids(tall), std::vector<std::string>({tall.frames[0].children[0].id, tall.frames[0].children[1].id}));
        // Python reads the columns of a cut panel right to left whatever the binding (only drawn panels follow it)
        Page left = page_with(frame("root", 0, 0, 100, 100), Binding::Left);
        left.split_frame("root", "vertical", 0.5, 4);
        QCOMPARE(leaf_ids(left), std::vector<std::string>({left.frames[0].children[1].id, left.frames[0].children[0].id}));
    }

    void readingOrderNested() {
        Frame root = frame("root", 0, 0, 100, 100);
        root.split_axis = "vertical";
        Frame left = frame("L", 0, 0, 48, 100);
        Frame right = frame("R", 52, 0, 48, 100);
        left.split_axis = "horizontal";
        left.children = {frame("L2", 0, 52, 48, 48), frame("L1", 0, 0, 48, 48)};
        right.split_axis = "horizontal";
        right.children = {frame("R1", 52, 0, 48, 30), frame("R2", 52, 34, 48, 66)};
        root.children = {left, right};
        const Page page = page_with(root);
        QCOMPARE(leaf_ids(page), std::vector<std::string>({"R1", "R2", "L1", "L2"}));
    }

    void readingOrderOfDrawnPanels() {
        Frame root = frame("root", 0, 0, 200, 200);
        root.split_axis = "free";
        root.children = {frame("C", 10, 50, 30, 20), frame("A", 10, 10, 30, 20), frame("B", 50, 12, 30, 20)};
        QCOMPARE(leaf_ids(page_with(root, Binding::Right)), std::vector<std::string>({"B", "A", "C"}));
        QCOMPARE(leaf_ids(page_with(root, Binding::Left)), std::vector<std::string>({"A", "B", "C"}));
        // a panel without height is never in its own row (Python loops for ever): it is read alone
        Frame flat = root;
        flat.children.push_back(frame("F", 100, 0, 10, 0));
        const auto ids = leaf_ids(page_with(flat));
        QCOMPARE(ids.size(), std::size_t{4});
        QCOMPARE(ids.front(), std::string("F"));
    }

    void readingOrderOfSlantedPanels() {
        // two panels cut by a slanted line: their boxes overlap, their middles do not
        Frame root = frame("root", 0, 0, 100, 100);
        root.split_axis = "horizontal";
        Frame top = frame("top", 0, 0, 100, 60);
        top.poly = std::vector<genko::core::Point>{{0, 0}, {100, 0}, {100, 20}, {0, 60}};
        Frame bottom = frame("bottom", 0, 20, 100, 80);
        bottom.poly = std::vector<genko::core::Point>{{0, 64}, {100, 24}, {100, 100}, {0, 100}};
        root.children = {bottom, top};
        QCOMPARE(leaf_ids(page_with(root)), std::vector<std::string>({"top", "bottom"}));
    }

    void frameAt() {
        Frame root = frame("root", 0, 0, 100, 100);
        root.split_axis = "free";
        Frame plain = frame("plain", 0, 0, 40, 40);
        Frame tri = frame("tri", 50, 0, 50, 50);
        tri.poly = std::vector<genko::core::Point>{{50, 0}, {100, 0}, {100, 50}};
        Frame round = frame("round", 0, 50, 40, 40);
        round.corner_mm = 10;
        Frame bowed = frame("bowed", 50, 60, 40, 30);
        bowed.curves = std::vector<double>{5, 0, 0, 0};
        root.children = {plain, tri, round, bowed};
        const Page page = page_with(root);
        const auto at = [&](Num x, Num y) {
            const Frame* f = page.frame_at(x, y);
            return f ? f->id : std::string("-");
        };
        QCOMPARE(at(40, 40), std::string("plain"));       // the edge of a box is inside
        QCOMPARE(at(Num(0), Num(0)), std::string("plain"));
        QCOMPARE(at(90, 10), std::string("tri"));
        QCOMPARE(at(60, 30), std::string("-"));            // in the triangle's box, outside the triangle
        QCOMPARE(at(20, 70), std::string("round"));
        QCOMPARE(at(0.5, 50.5), std::string("-"));         // cut off by the rounded corner
        QCOMPARE(at(70, 58), std::string("bowed"));        // above the box, inside the bowed top edge
        QCOMPARE(at(70, 50), std::string("-"));
        QVERIFY(genko::core::rounded(round) && genko::core::rounded(bowed) && !genko::core::rounded(plain));
    }

    void splitMergeResizeKeepPythonNumbers() {
        Page page = page_with(frame("root", 13, 13, 184, 271));  // a v1 book's ints
        const auto [a, b] = page.split_frame("root", "horizontal", 0.5, 4);
        QVERIFY(a->rect.x.same(Num(13)) && a->rect.width.same(Num(184)) && a->rect.height.same(Num(133.5)));
        QVERIFY(b->rect.y.same(Num(150.5)) && b->rect.height.same(Num(133.5)));
        QCOMPARE(page.frames[0].split_axis.value(), std::string("horizontal"));
        const std::string first = a->id;
        page.split_frame(first, "vertical", 1, 0);  // all ints: the spans stay ints
        const Frame* split = page.find_frame(first);
        QVERIFY(split->children[0].rect.width.same(Num(184)));
        QVERIFY(split->children[1].rect.x.same(Num(197)));
        QVERIFY(split->children[1].rect.width.same(Num(0)));
        try {
            page.split_frame("nope", "vertical", 0.5, 4);
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("key"));
        }
        try {
            page.split_frame(first, "vertical", 0.5, 4);
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("can only split a leaf frame"));
        }
        try {
            page.split_frame(split->children[0].id, "diagonal", 0.5, 4);
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("diagonal"));
        }
        QCOMPARE(page.parent_of(split->children[0].id)->id, first);
        QVERIFY(page.parent_of("root") == nullptr);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, page.merge_frame("root"));
        Frame& merged = page.merge_frame(split->children[1].id);
        QCOMPARE(merged.id, first);
        QVERIFY(merged.children.empty() && !merged.split_axis);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, page.resize_frame("root", Rect{0, 0, 1, 1}));
        page.resize_frame(first, Rect{1, 2.5, 3, 4});
        QVERIFY(page.find_frame(first)->rect.y.same(Num(2.5)));
    }

    void newEpisodeAsPython() {
        genko::core::ScopedIdSource ids(genko::core::counting_ids());
        const Document doc = genko::core::new_episode("題", 3, 2, PageSpec::a4_mono());
        QCOMPARE(doc.pages.size(), std::size_t{2});
        QVERIFY(genko::core::is_book_id(doc.book_id));
        // the same ids in the same order as Python: the page, its four layers, its root panel
        const Page& p1 = doc.page(0);
        QCOMPARE(p1.id, std::string("pg_000000000001"));
        QCOMPARE(p1.layers[0].id, std::string("000000000002"));
        QCOMPARE(p1.layers[3].id, std::string("000000000005"));
        QCOMPARE(p1.frames[0].id, std::string("000000000006"));
        QCOMPARE(doc.page(1).id, std::string("pg_000000000007"));
        QVERIFY(p1.index.same(Num(1)));
        QCOMPARE(p1.layers.size(), std::size_t{4});
        QVERIFY(p1.layers[0].role == LayerRole::Bg && p1.layers[0].kind == LayerKind::Fill && p1.layers[0].exportable);
        QVERIFY(p1.layers[1].role == LayerRole::Name && !p1.layers[1].exportable);
        QVERIFY(p1.layers[2].role == LayerRole::Ink && p1.layers[2].panel_each);
        QVERIFY(p1.layers[3].role == LayerRole::Finish && p1.layers[3].exportable);
        expect_json(rect_json(p1.frames[0].rect), Json::array({13.0, 13.0, 184.0, 271.0}), "root panel");
        QVERIFY(doc.spec.width_mm.same(Num(210)));
    }

    void storyLines() {
        genko::core::ScopedIdSource ids(genko::core::counting_ids(100));
        Document doc = genko::core::new_episode("t", 1, 2, PageSpec::a4_mono());  // ids 100…111
        auto& line = doc.add_line(1, "こんにちは", "A");
        QCOMPARE(line.id, std::string("000000000070"));  // 112
        QVERIFY(line.x_mm.same(Num(0)) && line.w_mm.same(Num(40)) && line.h_mm.same(Num(20)));
        doc.add_line(2, "二");
        doc.add_line(1, "また");
        QCOMPARE(doc.story_for_page(Num(1.0)).size(), std::size_t{2});  // Python: 1 == 1.0
        QCOMPARE(doc.story_for_page(2).front()->text, std::string("二"));
        QVERIFY(doc.story_for_page(3).empty());
    }

    void copyOnWritePages() {
        Document a = genko::core::new_episode("t", 1, 3, PageSpec::b5_doujin());
        auto strokes = std::vector<genko::core::StrokePtr>{std::make_shared<const genko::core::Stroke>()};
        a.edit_page(0).layers[2].strokes = genko::core::make_strokes(strokes);
        const Page* before = a.pages[0].get();
        a.edit_page(0).note = "only a has it";  // not shared: changed in place
        QCOMPARE(a.pages[0].get(), before);

        Document b = a;  // a copy shares every page
        QCOMPARE(b.pages[1].get(), a.pages[1].get());
        b.edit_page(0).note = "b's note";
        QCOMPARE(a.page(0).note, std::string("only a has it"));
        QCOMPARE(b.page(0).note, std::string("b's note"));
        QVERIFY(b.pages[0].get() != a.pages[0].get());
        QCOMPARE(b.pages[1].get(), a.pages[1].get());  // the other pages are still shared
        // the copied page shares its (immutable) strokes
        QCOMPARE(b.page(0).layers[2].strokes.get(), a.page(0).layers[2].strokes.get());
        QCOMPARE(b.page(0).layers[2].strokes->items[0].get(), strokes[0].get());
        b.edit_page(0).split_frame(b.page(0).frames[0].id, "vertical", 0.5, 4);
        QVERIFY(a.page(0).frames[0].children.empty());
    }

    void ids() {
        std::set<std::string> seen;
        for (int i = 0; i < 1000; ++i) {
            const std::string id = genko::core::new_id();
            QCOMPARE(id.size(), std::size_t{12});
            QVERIFY(id.find_first_not_of("0123456789abcdef") == std::string::npos);
            seen.insert(id);
        }
        QCOMPARE(seen.size(), std::size_t{1000});
        QVERIFY(genko::core::new_page_id().starts_with("pg_"));
        const std::string book = genko::core::new_book_id();
        QVERIFY(genko::core::is_book_id(book));
        QCOMPARE(book[12], '4');
        QVERIFY(!genko::core::is_book_id("ABCDEF0123456789abcdef0123456789"));
        {
            genko::core::ScopedIdSource scoped(genko::core::counting_ids(255));
            QCOMPARE(genko::core::new_id(), std::string("0000000000ff"));
            QVERIFY(genko::core::is_book_id(genko::core::new_book_id()));  // (book ids stay random)
        }
        QVERIFY(genko::core::new_id() != std::string("000000000100"));
    }

    void paintAsPython() {
        Page page = genko::core::make_page(1, PageSpec::a4_mono(), Binding::Right);
        page.paint(LayerRole::Bg, {255, 255, 255});
        page.paint(LayerRole::Draft, {1, 2, 3});
        page.paint(LayerRole::Tone, {4, 5, 6});
        page.paint(LayerRole::Bg, {0, 0, 0});
        QCOMPARE(page.fills.size(), std::size_t{3});
        QVERIFY(page.fills[0].first == LayerRole::Bg && page.fills[0].second[0].same(Num(0)));
        QVERIFY(page.first_layer(LayerRole::Bg)->kind == LayerKind::Fill);
        const auto* draft = page.first_layer(LayerRole::Draft);
        QVERIFY(draft != nullptr && draft->kind == LayerKind::Fill && !draft->exportable);
        QCOMPARE(page.layers.size(), std::size_t{5});
        QVERIFY(page.first_layer(LayerRole::Tone) == nullptr);
    }
};

QTEST_GUILESS_MAIN(TestModel)
#include "test_model.moc"
