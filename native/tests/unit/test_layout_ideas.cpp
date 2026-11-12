// komawari@1 (コマ割り K1, SPEC LAYOUT-01): the name plan's new layout keys (差し込み・重ね, はみ出し, 枠なし, each border's
// gap and slant), add_frame into / over, the batch of layout ideas (the same batch from the same seed, the exact
// number of panels, the sieve, variations, applying an idea), and the warnings of a dull layout.
//
// DEPS genko_service

#include <QtTest>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pyops.hpp"
#include "render/page.hpp"
#include "service/book.hpp"
#include "service/komawari_shape.hpp"
#include "service/layout.hpp"
#include "service/layout_ideas.hpp"
#include "service/lint.hpp"
#include "service/studio_service.hpp"
#include "service/wording.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "studio/schemas.hpp"

namespace layout = genko::service::layout;
namespace ideas = genko::service::layout_ideas;
namespace komawari = genko::service::komawari;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
using genko::core::PageSpec;
using genko::service::Work;

namespace {

Json parsed(const char* text) { return Json::parse(text); }

std::string sha256(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

Work blank_book(int pages = 1, const PageSpec& spec = PageSpec::b5_doujin()) {
    return Work{genko::core::new_episode("t", Num(1), pages, spec), Json::array()};
}

ideas::Request request(ideas::Preset preset, int panels, const std::string& seed) {
    ideas::Request r;
    const auto [w, h] = komawari::frame_size(PageSpec::b5_doujin());
    r.frame_w_mm = w;
    r.frame_h_mm = h;
    r.preset = preset;
    r.panels = panels;
    r.seed = seed;
    return r;
}

std::string skeletons(const ideas::Batch& batch) {
    Json all = Json::array();
    for (const ideas::Idea& idea : batch.ideas) all.push_back(idea.skeleton);
    return all.dump();
}

std::vector<std::string> order_of(const ideas::Idea& idea) {
    std::vector<std::string> out;
    for (const komawari::PanelBox& box : idea.shape.panels) out.push_back(box.slot);
    return out;
}

std::string project_text(const Document& doc, const QTemporaryDir& dir) {
    genko::storage::AssetStore store(dir.path().toStdString());
    return genko::storage::project_json_v4(doc, store);
}

// The page's panels as saved, then as saved again after reading the book back (the same, "over" kept), and whether the
// book read back is saved again byte for byte.
bool saved_and_read_back(const Document& doc, std::string* why) {
    QTemporaryDir dir;
    const std::string text = project_text(doc, dir);
    const auto read = genko::storage::load_document_text(text, dir.path().toStdString());
    const std::string again = project_text(read.document, dir);
    const auto twice = genko::storage::load_document_text(again, dir.path().toStdString());
    const Json a = Json::parse(text)["pages"];
    const Json b = Json::parse(again)["pages"];
    bool same = read.report.clean() && a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i]["frames"] == b[i]["frames"];
    same = same && project_text(twice.document, dir) == again;
    if (!same && why != nullptr) *why = a.dump().substr(0, 600) + "\n---\n" + b.dump().substr(0, 600);
    return same;
}

// Where two texts first differ (for a failure's message).
std::string first_difference(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const std::size_t from = i > 200 ? i - 200 : 0;
    return "at " + std::to_string(i) + ":\n" + a.substr(from, 400) + "\n---\n" + b.substr(from, 400);
}

bool has_code(const std::vector<genko::studio::Issue>& issues, const std::string& code) {
    for (const auto& i : issues) {
        if (i.code == code) return true;
    }
    return false;
}

const genko::core::Frame* parent_of(const genko::core::Page& page, const std::string& id) { return page.parent_of(id); }

const ideas::Preset kPresets[] = {ideas::Preset::Talk, ideas::Preset::Standard, ideas::Preset::Action, ideas::Preset::Showcase};

}  // namespace

class TestLayoutIdeas : public QObject {
    Q_OBJECT

private slots:
    // --- A: the name plan's new keys ------------------------------------------------------------------------------------

    // A plan without the new keys gives the panels, the ops and the saved bytes it gave before them: the ops are the
    // split_frame and set_frame ops as before (written out here), applying them by hand saves the same bytes, no frame
    // is written with "over", and the bytes are pinned.
    void aPlanWithoutTheNewKeysIsAsBefore() {
        const Json plan = parsed(R"({"page": 1, "template": null, "tier_gap_mm": 6,
            "tiers": [{"h": 0.4, "cols": [{"slot": "p1", "w": 1}]},
                      {"h": 0.6, "cols": [{"slot": "p2", "w": 0.45}, {"slot": "p3", "w": 0.55}]}],
            "panels": [{"slot": "p1", "slant": 10, "bleed": true}, {"slot": "p2"}, {"slot": "p3"}]})");
        std::string via_layout, via_ops;
        Json ops;
        {
            genko::core::ScopedIdSource ids(genko::core::counting_ids());
            Work work = blank_book();
            work.doc.book_id = "0123456789abcdef0123456789abcdef";  // (a new book's id is always random)
            const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:test");
            ops = compiled.ops;
            QTemporaryDir dir;
            via_layout = project_text(work.doc, dir);
        }
        QCOMPARE(ops.size(), std::size_t{3});
        for (const Json& op : ops) {
            for (const char* key : {"into", "over", "border_mm", "clip"}) QVERIFY2(!op.contains(key), op.dump().c_str());
        }
        QCOMPARE(ops[0]["op"], Json("split_frame"));
        QCOMPARE(ops[0]["gutter_mm"], Json(6.0));
        QCOMPARE(ops[0]["tilt_mm"], Json(10.0));
        QCOMPARE(ops[1]["gutter_mm"], Json(3.0));
        QVERIFY(!ops[1].contains("tilt_mm"));
        Json bleed = ops[2];
        bleed.erase("frame_id");
        QCOMPARE(bleed, parsed(R"({"op": "set_frame", "page": 1, "bleed": true})"));
        {
            genko::core::ScopedIdSource ids(genko::core::counting_ids());
            Work work = blank_book();
            work.doc.book_id = "0123456789abcdef0123456789abcdef";
            for (const Json& op : ops) work.apply(Json::array({op}), "ai:test");
            QTemporaryDir dir;
            via_ops = project_text(work.doc, dir);
        }
        QVERIFY2(via_layout == via_ops, first_difference(via_layout, via_ops).c_str());
        QVERIFY(via_layout.find("\"over\"") == std::string::npos);
        QCOMPARE(Json::parse(via_layout)["features"], Json::array());
        QCOMPARE(QString::fromStdString(sha256(via_layout)), QStringLiteral("ed22ec856315cc7038727380d82225db3dc4e9d2411ae2e3dbaee5dd181cc922"));
        // the plan as submit_name keeps it: no null for the new keys it did not write
        const Json& schema = genko::studio::schema("name_plan@1");
        Json given = plan;
        given["turn_role"] = "normal";
        const Json kept = genko::studio::unfill_extensions(genko::studio::fill_nulls(given, schema), given);
        const std::string text = kept.dump();
        for (const char* key : {"\"insets\"", "\"gap_mm\"", "\"tilt_mm\"", "\"border_mm\"", "\"overflow\""}) QVERIFY2(text.find(key) == std::string::npos, key);
        QVERIFY(kept.contains("spread"));  // (the keys filled before K1 stay filled)
        QVERIFY(kept["tiers"][0]["cols"][0].contains("rows"));
    }

    // Each border's gap and slant, 枠なし and はみ出し, and a panel set over another: the panels as the plan says, the
    // inset read beside its host, over the others, in a node of drawn panels with it.
    void theNewKeysShapeThePanels() {
        Work work = blank_book();
        const Json plan = parsed(R"({"page": 1,
            "tiers": [{"h": 0.5, "gap_mm": 12, "tilt_mm": -8, "cols": [{"slot": "p1", "w": 1}]},
                      {"h": 0.5, "cols": [{"slot": "p2", "w": 0.5, "gap_mm": 6, "tilt_mm": 5}, {"slot": "p3", "w": 0.5}]}],
            "insets": [{"slot": "p4", "on": "p1", "rect": [0.05, 0.3, 0.3, 0.17]}],
            "panels": [{"slot": "p1", "overflow": true}, {"slot": "p2", "border_mm": 0}, {"slot": "p3"}, {"slot": "p4"}]})");
        QVERIFY(layout::check_tiers(plan).empty());
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:test");
        const std::vector<std::string> order(compiled.reading_order.begin(), compiled.reading_order.end());
        QCOMPARE(order, (std::vector<std::string>{"p1", "p4", "p2", "p3"}));
        QCOMPARE(order, komawari::reading_slots(plan));
        const genko::core::Page& page = work.doc.page(0);
        const auto* p1 = page.find_frame(*compiled.frame_of("p1"));
        const auto* p2 = page.find_frame(*compiled.frame_of("p2"));
        const auto* p3 = page.find_frame(*compiled.frame_of("p3"));
        const auto* p4 = page.find_frame(*compiled.frame_of("p4"));
        QVERIFY(!p1->clip && p1->border_mm > 0);
        QVERIFY(p2->border_mm == 0 && p2->clip);
        QVERIFY(p4->over && !p1->over);
        QVERIFY(p1->poly && !p1->poly->empty());  // (its lower border slants)
        const auto* holder = parent_of(page, p4->id);
        QVERIFY(holder != nullptr && genko::core::is_free(*holder));
        QCOMPARE(holder, parent_of(page, p1->id));
        // the tiers' gap: 12 mm at the middle of the slanted border
        const auto line = genko::core::cut_line(*parent_of(page, parent_of(page, p1->id)->id));
        QVERIFY(line.has_value());
        QCOMPARE(line->gutter, 12.0);
        // the columns' gap 6 mm (p2 is read first: on the right)
        QVERIFY(std::abs((p2->rect.x - (p3->rect.x + p3->rect.width)).value() - 6) < 5.1);  // (slanted 5 mm: the boxes overlap by up to that)
        // saved and read back: the same panels, over kept
        QTemporaryDir dir;
        const std::string text = project_text(work.doc, dir);
        const auto read = genko::storage::load_document_text(text, dir.path().toStdString());
        QVERIFY(read.report.clean());
        const genko::core::Frame* back = read.document.page(0).find_frame(p4->id);
        QVERIFY(back != nullptr && back->over);
        QCOMPARE(Json::parse(text)["features"], parsed(R"(["komawari@1"])"));
        QVERIFY(genko::storage::is_known_feature("komawari@1"));
        std::string why;
        QVERIFY2(saved_and_read_back(work.doc, &why), why.c_str());
    }

    void theNewKeysAreChecked() {
        const auto codes = [](const char* text) {
            std::set<std::string> out;
            for (const auto& issue : layout::check_tiers(Json::parse(text))) out.insert(issue.code);
            return out;
        };
        QVERIFY(codes(R"({"tiers": [{"h": 1, "gap_mm": -1, "cols": [{"slot": "p1", "w": 1}]}], "panels": [{"slot": "p1"}]})").count("layout_gap_range"));
        QVERIFY(codes(R"({"tiers": [{"h": 0.5, "tilt_mm": 200, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.5, "cols": [{"slot": "p2", "w": 1}]}],
                         "panels": [{"slot": "p1"}, {"slot": "p2"}]})").count("layout_tilt_range"));
        QVERIFY(codes(R"({"tiers": [{"h": 1, "tilt_mm": 5, "cols": [{"slot": "p1", "w": 1}]}], "panels": [{"slot": "p1"}]})").count("layout_border_unused"));
        QVERIFY(codes(R"({"tiers": [{"h": 1, "cols": [{"slot": "p1", "w": 1}]}], "insets": [{"slot": "p2", "on": "p9", "rect": [0, 0, 0.3, 0.3]}],
                         "panels": [{"slot": "p1"}, {"slot": "p2"}]})").count("layout_inset_host"));
        QVERIFY(codes(R"({"tiers": [{"h": 1, "cols": [{"slot": "p1", "w": 1}]}], "insets": [{"slot": "p2", "on": "p1", "rect": [0.8, 0, 0.3, 0.3]}],
                         "panels": [{"slot": "p1"}, {"slot": "p2"}]})").count("layout_inset_rect"));
        QVERIFY(codes(R"({"tiers": [{"h": 1, "cols": [{"slot": "p1", "w": 1}]}], "insets": [{"slot": "p2", "on": "p1", "rect": [0, 0, 0.3, 0.3]}],
                         "panels": [{"slot": "p1"}]})").count("panel_missing"));
        QVERIFY(codes(R"({"tiers": [{"h": 1, "cols": [{"slot": "p1", "w": 1}]}], "panels": [{"slot": "p1", "border_mm": 9}]})").count("panel_border_range"));
        // a slant the panel cannot take: the plan's error, in Japanese
        Work work = blank_book();
        const Json plan = parsed(R"({"page": 1, "tiers": [{"h": 0.5, "tilt_mm": 79, "cols": [{"slot": "p1", "w": 0.5, "tilt_mm": 79}, {"slot": "p2", "w": 0.5}]},
            {"h": 0.5, "cols": [{"slot": "p3", "w": 0.34, "gap_mm": 80}, {"slot": "p4", "w": 0.33, "gap_mm": 80}, {"slot": "p5", "w": 0.33}]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}, {"slot": "p4"}, {"slot": "p5"}]})");
        bool refused = false;
        try {
            (void)layout::apply_layout(work, Json(1), plan, "ai:test");
        } catch (const layout::LayoutError& e) {
            refused = true;
            QVERIFY2(QString::fromStdString(e.message()).contains(QRegularExpression(QStringLiteral("[ぁ-んァ-ヶ一-龥]"))), e.what());
        }
        QVERIFY(refused);
    }

    // add_frame "into": the panel's place becomes a node of drawn panels holding it (its id kept) and the new one over
    // it; refused for a split and for a panel it does not touch. set_frame "over" sets and clears it.
    void addFrameIntoAPanel() {
        Work work = blank_book();
        work.apply(parsed(R"([{"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5}])"), "human:t");
        const genko::core::Page& page = work.doc.page(0);
        const std::string top = page.frames[0].children[0].id;
        const genko::core::Rect r = page.frames[0].children[0].rect;
        Json op = Json::object({{"op", "add_frame"}, {"page", 1}, {"into", top},
                                {"rect", Json::array({(r.x + Num(5)).value(), (r.y + Num(5)).value(), 50, 40})}});
        work.apply(Json::array({op}), "human:t");
        const genko::core::Page& now = work.doc.page(0);
        QVERIFY(now.find_frame(top) != nullptr && now.find_frame(top)->children.empty());
        const auto* node = now.parent_of(top);
        QVERIFY(node != nullptr && genko::core::is_free(*node) && node->children.size() == 2);
        QVERIFY(node->children[1].over);
        QCOMPARE(now.leaf_frames().size(), std::size_t{3});
        QVERIFY_THROWS_EXCEPTION(genko::core::ApplyError,
                                 work.apply(Json::array({Json::object({{"op", "add_frame"}, {"page", 1}, {"into", node->id}, {"rect", Json::array({10, 10, 30, 30})}})}), "human:t"));
        QVERIFY_THROWS_EXCEPTION(genko::core::ApplyError,
                                 work.apply(Json::array({Json::object({{"op", "add_frame"}, {"page", 1}, {"into", top}, {"rect", Json::array({10, 250, 30, 30})}})}), "human:t"));
        const std::string inset = node->children[1].id;
        work.apply(Json::array({Json::object({{"op", "set_frame"}, {"page", 1}, {"frame_id", inset}, {"over", false}})}), "human:t");
        QVERIFY(!work.doc.page(0).find_frame(inset)->over);
        // the refusals read in Japanese
        for (const char* message : {"into is the id of a panel", "into takes one panel (not a split)", "the drawn panel does not touch the panel it goes into",
                                    "only a panel (not a split) lies over the others"}) {
            QVERIFY2(!genko::wording::error(QString::fromLatin1(message)).contains(QRegularExpression(QStringLiteral("[A-Za-z]{4,}"))), message);
        }
    }

    // A panel over the others hides the border of the panel below it: the border's pixels under it are paper.
    void aPanelOverTheOthersHidesTheBorderBelow() {
        Work work = blank_book();
        const Json plan = parsed(R"({"page": 1, "tiers": [{"h": 0.5, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.5, "cols": [{"slot": "p2", "w": 1}]}],
            "insets": [{"slot": "p3", "on": "p1", "rect": [0.3, 0.4, 0.4, 0.2]}], "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}]})");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:test");
        const genko::core::Page& page = work.doc.page(0);
        const genko::core::Frame* p1 = page.find_frame(*compiled.frame_of("p1"));
        const int dpi = 100;
        // a point on p1's bottom border, under the middle of the inset
        const double x_mm = (p1->rect.x + p1->rect.width / Num(2)).value();
        const double y_mm = (p1->rect.y + p1->rect.height).value();
        const int x = genko::render::mm_to_px(x_mm, dpi);
        const int y = genko::render::mm_to_px(y_mm, dpi);
        const auto shown = genko::render::render_page(page, dpi, genko::render::proof_options(), &work.doc).image;
        QVERIFY2(shown.getpixel(x, y)[0] > 200, std::to_string(shown.getpixel(x, y)[0]).c_str());
        work.apply(Json::array({Json::object({{"op", "set_frame"}, {"page", 1}, {"frame_id", *compiled.frame_of("p3")}, {"over", false}})}), "ai:test");
        const auto plain = genko::render::render_page(work.doc.page(0), dpi, genko::render::proof_options(), &work.doc).image;
        QVERIFY2(plain.getpixel(x, y)[0] < 100, std::to_string(plain.getpixel(x, y)[0]).c_str());
    }

    // --- the batch of ideas ------------------------------------------------------------------------------------------

    void theSameSeedGivesTheSameBatch() {
        const ideas::Request r = request(ideas::Preset::Standard, 5, "km-0123456789abcdef");
        const ideas::Batch a = ideas::generate(r);
        const ideas::Batch b = ideas::generate(r);
        QCOMPARE(a.ideas.size(), std::size_t{ideas::kDefaultCount});
        QCOMPARE(a.seed, std::string("km-0123456789abcdef"));
        for (const ideas::Idea& idea : a.ideas) qInfo("%s", idea.summary.c_str());
        QCOMPARE(skeletons(a), skeletons(b));
        QCOMPARE(QString::fromStdString(sha256(skeletons(a))), QStringLiteral("cd748f42b15318b1c7dee9f3b0afe9d89ec27a597cb1f647f584cc1d6e1fe9d8"));
        ideas::Request action = request(ideas::Preset::Action, 6, "ネームの種");
        QCOMPARE(QString::fromStdString(sha256(skeletons(ideas::generate(action)))), QStringLiteral("574b6021e94f89c4e9ba4f0122695df3bc392537b9a538dfc1d3c6c85e78791d"));
        ideas::Request other = r;
        other.seed = "km-0123456789abcdee";
        QVERIFY(skeletons(ideas::generate(other)) != skeletons(a));
        // no seed: a new one, which gives the batch again
        ideas::Request fresh = r;
        fresh.seed.clear();
        const ideas::Batch c = ideas::generate(fresh);
        QVERIFY(c.seed.rfind("km-", 0) == 0 && c.seed.size() == 19);
        fresh.seed = c.seed;
        QCOMPARE(skeletons(ideas::generate(fresh)), skeletons(c));
        QCOMPARE(ideas::seed_value("km-0123456789abcdef"), std::uint64_t{0x0123456789abcdefULL});
    }

    void eachIdeaHasExactlyTheNumberOfPanels() {
        for (const ideas::Preset preset : kPresets) {
            for (int n = 1; n <= static_cast<int>(layout::kMaxPanels); ++n) {
                const ideas::Batch batch = ideas::generate(request(preset, n, "count-" + std::to_string(n)));
                QVERIFY2(!batch.ideas.empty(), (std::string(ideas::preset_key(preset)) + " " + std::to_string(n)).c_str());
                if (n >= 3) QVERIFY2(batch.ideas.size() >= 6, (std::string(ideas::preset_key(preset)) + " " + std::to_string(n) + ": " + std::to_string(batch.ideas.size())).c_str());
                for (const ideas::Idea& idea : batch.ideas) {
                    QCOMPARE(static_cast<int>(idea.shape.count()), n);
                    QCOMPARE(static_cast<int>(core_panels(idea.skeleton)), n);
                    QVERIFY(layout::check_tiers(idea.skeleton).empty());
                }
            }
        }
    }

    // Plain grids, a broken reading order, panels under the smallest size and gaps under the narrowest never come out;
    // the sieve drops them (and alike ideas).
    void theSieveKeepsTheDullAndTheBrokenOut() {
        std::set<std::string> reasons;
        for (const ideas::Preset preset : kPresets) {
            for (int k = 0; k < 6; ++k) {
                ideas::Request r = request(preset, 0, "sieve-" + std::to_string(k));
                const ideas::Batch batch = ideas::generate(r);
                for (const auto& [why, n] : batch.dropped) reasons.insert(why);
                for (const ideas::Idea& idea : batch.ideas) {
                    QVERIFY(!komawari::plain_grid(idea.shape));
                    QCOMPARE(komawari::reading_break(idea.shape), std::size_t{0});
                    QVERIFY(komawari::smallest_side(idea.shape) >= genko::service::lint::kMinPanelSideMm);
                    QVERIFY(komawari::narrowest_gap(idea.shape) >= komawari::kMinGapMm);
                }
                for (std::size_t i = 0; i < batch.ideas.size(); ++i) {
                    for (std::size_t j = 0; j < i; ++j) QVERIFY(komawari::difference(batch.ideas[i].shape, batch.ideas[j].shape) > 0);
                }
            }
        }
        QVERIFY(reasons.count("plain") && reasons.count("similar"));
        // the measures themselves
        const Json grid = parsed(R"({"tiers": [{"h": 0.5, "cols": [{"slot": "p1", "w": 0.5}, {"slot": "p2", "w": 0.5}]},
                                               {"h": 0.5, "cols": [{"slot": "p3", "w": 0.5}, {"slot": "p4", "w": 0.5}]}],
                                    "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}, {"slot": "p4"}]})");
        QVERIFY(komawari::plain_grid(komawari::shape_of_plan(grid, 180, 270)));
        Json slanted = grid;
        slanted["tiers"][0]["tilt_mm"] = 10;
        QVERIFY(!komawari::plain_grid(komawari::shape_of_plan(slanted, 180, 270)));
        const Json backwards = parsed(R"({"tiers": [{"h": 1, "cols": [{"slot": "p1", "w": 1}]}],
            "insets": [{"slot": "p2", "on": "p1", "rect": [0.6, 0.0, 0.4, 0.2]}, {"slot": "p3", "on": "p1", "rect": [0.0, 0.8, 0.4, 0.2]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}]})");
        QCOMPARE(komawari::reading_break(komawari::shape_of_plan(backwards, 180, 270)), std::size_t{0});
        const Json upward = parsed(R"({"tiers": [{"h": 0.5, "cols": [{"slot": "p1", "w": 0.5}, {"slot": "c", "w": 0.5, "rows": [{"slot": "p2", "h": 0.5}, {"slot": "p3", "h": 0.5}]}]},
                                                {"h": 0.5, "cols": [{"slot": "p4", "w": 1}]}],
            "insets": [{"slot": "p5", "on": "p1", "rect": [0.6, 0.3, 0.3, 0.15]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}, {"slot": "p4"}, {"slot": "p5"}]})");
        QVERIFY(komawari::reading_break(komawari::shape_of_plan(upward, 180, 270)) > 0);  // (the inset low in p1, then up to p2)
    }

    // The 見せ場 asked for is the largest panel of every idea.
    void theFocusIsTheLargestPanel() {
        for (const int focus : {1, 3, 5}) {
            ideas::Request r = request(ideas::Preset::Standard, 5, "focus");
            r.focus = focus;
            const ideas::Batch batch = ideas::generate(r);
            QVERIFY(batch.ideas.size() >= 6);
            for (const ideas::Idea& idea : batch.ideas) QCOMPARE(komawari::largest(idea.shape), std::size_t(focus));
        }
    }

    // Each idea applied to a blank page is real panels, read in the order the idea gives, saved and read back the same.
    void anIdeaAppliedIsRealPanels() {
        int insets = 0, slanted = 0, bleeds = 0;
        for (const ideas::Preset preset : kPresets) {
            ideas::Request r = request(preset, 0, "apply");
            r.knobs.overlap = 0.9;
            const ideas::Batch batch = ideas::generate(r);
            for (const ideas::Idea& idea : batch.ideas) {
                Work work = blank_book();
                const layout::CompiledLayout compiled = ideas::apply_idea(work, Json(1), idea.skeleton, "ai:test", false);
                const std::vector<std::string> order(compiled.reading_order.begin(), compiled.reading_order.end());
                QCOMPARE(order, order_of(idea));
                const genko::core::Page& page = work.doc.page(0);
                QCOMPARE(page.leaf_frames().size(), idea.shape.count());
                for (const genko::core::Frame* f : page.leaf_frames()) {
                    insets += f->over ? 1 : 0;
                    slanted += f->poly && !f->poly->empty() ? 1 : 0;
                    bleeds += f->bleed ? 1 : 0;
                }
                std::string why;
                QVERIFY2(saved_and_read_back(work.doc, &why), why.c_str());
                // the page as a skeleton again: the same layout
                const auto again = komawari::skeleton_of_page(work.doc, page);
                QVERIFY(again.has_value());
                const auto shape = komawari::shape_of_page(work.doc, page);
                QVERIFY(shape.has_value());
                QVERIFY2(komawari::difference(*shape, idea.shape) == 0, (komawari::describe(*shape) + " / " + idea.summary + " " + again->dump() + " // " + idea.skeleton.dump()).c_str());
                // replace: the page cleared and laid again
                (void)ideas::apply_idea(work, Json(1), batch.ideas.front().skeleton, "ai:test", true);
                QCOMPARE(work.doc.page(0).leaf_frames().size(), batch.ideas.front().shape.count());
            }
        }
        QVERIFY(insets > 0 && slanted > 0 && bleeds > 0);
        // a page whose name a person approved: an agent does not lay it again
        Work work = blank_book();
        work.doc.edit_page(0).name_ok = true;
        const ideas::Batch batch = ideas::generate(request(ideas::Preset::Talk, 4, "approved"));
        QVERIFY_THROWS_EXCEPTION(layout::LayoutError, (void)ideas::apply_idea(work, Json(1), batch.ideas.front().skeleton, "ai:test", true));
    }

    // Variations of one skeleton: a small degree keeps its tiers and panels, a large one changes them; the panels'
    // number stays; the same seed gives the same variations; the page as it is can be the base.
    void variationsOfOneSkeleton() {
        const ideas::Batch first = ideas::generate(request(ideas::Preset::Standard, 6, "base"));
        const ideas::Idea& base = first.ideas.front();
        ideas::Request small = request(ideas::Preset::Standard, 0, "vary");
        small.base = base.skeleton;
        small.variation = 0.1;
        const ideas::Batch near = ideas::generate(small);
        QVERIFY(near.ideas.size() >= 6);
        for (const ideas::Idea& idea : near.ideas) {
            QCOMPARE(idea.shape.count(), base.shape.count());
            QCOMPARE(idea.shape.pattern, base.shape.pattern);
        }
        QCOMPARE(skeletons(ideas::generate(small)), skeletons(near));
        ideas::Request large = small;
        large.variation = 0.95;
        const ideas::Batch far = ideas::generate(large);
        QVERIFY(far.ideas.size() >= 6);
        bool changed = false;
        for (const ideas::Idea& idea : far.ideas) {
            QCOMPARE(idea.shape.count(), base.shape.count());
            changed = changed || idea.shape.pattern != base.shape.pattern;
        }
        QVERIFY(changed);
        // the page as it is
        Work work = blank_book();
        (void)ideas::apply_idea(work, Json(1), base.skeleton, "ai:test", false);
        const auto skeleton = komawari::skeleton_of_page(work.doc, work.doc.page(0));
        QVERIFY(skeleton.has_value());
        ideas::Request from_page = small;
        from_page.base = *skeleton;
        QVERIFY(!ideas::generate(from_page).ideas.empty());
    }

    // 12 ideas well within the time a window waits for (the drawing aside).
    void aBatchIsQuick() {
        QElapsedTimer timer;
        timer.start();
        int made = 0;
        for (const ideas::Preset preset : kPresets) made += static_cast<int>(ideas::generate(request(preset, 0, "time")).ideas.size());
        const qint64 ms = timer.elapsed();
        qInfo("4 batches (%d ideas): %lld ms", made, static_cast<long long>(ms));
        QVERIFY2(ms < 4 * 50, std::to_string(ms).c_str());
    }

    void theRequestIsChecked() {
        const auto refused = [](const char* text, const std::string& path) {
            try {
                (void)ideas::request_from_json(Json::parse(text), PageSpec::b5_doujin());
            } catch (const layout::LayoutError& e) {
                return e.where() == path;
            }
            return false;
        };
        QVERIFY(refused(R"({"panels": 9})", "/panels"));
        QVERIFY(refused(R"({"preset": "horror"})", "/preset"));
        QVERIFY(refused(R"({"knobs": {"slant": 1.5}})", "/knobs/slant"));
        QVERIFY(refused(R"({"knobs": {"wobble": 0.5}})", "/knobs/wobble"));
        QVERIFY(refused(R"({"count": 0})", "/count"));
        QVERIFY(refused(R"({"colour": 1})", "/colour"));
        QVERIFY(refused(R"({"panels": 3, "focus": 4})", "/focus"));
        const ideas::Request r = ideas::request_from_json(parsed(R"({"panels": 4, "preset": "会話", "knobs": {"big": 0.7}, "seed": 42})"), PageSpec::b5_doujin());
        QCOMPARE(r.panels, 4);
        QVERIFY(r.preset == ideas::Preset::Talk);
        QCOMPARE(*r.knobs.big, 0.7);
        QCOMPARE(r.seed, std::string("42"));
        QCOMPARE(r.count, ideas::kDefaultCount);
        const Json back = ideas::request_to_json(r);
        QCOMPARE(back["preset"], Json("talk"));
        const Json shown = ideas::batch_to_json(ideas::generate(r));
        QCOMPARE(shown["ideas"].size(), std::size_t{12});
        QVERIFY(shown["ideas"][0].contains("skeleton") && shown["ideas"][0].contains("summary"));
        // into a name plan as it is
        const Json plan = parsed(R"({"page": 1, "turn_role": "normal", "template": "splash", "tiers": null,
            "panels": [{"slot": "p1", "shot": "MS", "beat_ids": ["b1"]}]})");
        const Json merged = ideas::merge_into_plan(shown["ideas"][0]["skeleton"], plan);
        QVERIFY(merged["template"].is_null());
        QCOMPARE(merged["panels"].size(), std::size_t{4});
        QCOMPARE(merged["panels"][0]["shot"], Json("MS"));
        QVERIFY(layout::check_tiers(merged).empty());
    }

    // --- C: 単調さの警告 -------------------------------------------------------------------------------------------------

    void aDullLayoutIsWarnedOf() {
        Work work = blank_book(5);
        const Json grid = parsed(R"({"tiers": [{"h": 0.34, "cols": [{"slot": "p1", "w": 0.5}, {"slot": "p2", "w": 0.5}]},
                                               {"h": 0.33, "cols": [{"slot": "p3", "w": 0.5}, {"slot": "p4", "w": 0.5}]},
                                               {"h": 0.33, "cols": [{"slot": "p5", "w": 0.5}, {"slot": "p6", "w": 0.5}]}],
                                    "panels": [{"slot": "p1", "beat_ids": ["b2"]}, {"slot": "p2"}, {"slot": "p3"}, {"slot": "p4"}, {"slot": "p5"}, {"slot": "p6"}]})");
        for (int page = 1; page <= 2; ++page) {
            Json plan = grid;
            plan["page"] = page;
            (void)layout::apply_layout(work, Json(page), plan, "ai:test");
        }
        const Json script = parsed(R"({"scenes": [{"id": "s1", "beats": [{"id": "b1", "page": 3, "reveal": false}, {"id": "b2", "page": 3, "reveal": true}]}]})");
        Json plan = grid;
        plan["page"] = 3;
        const auto issues = komawari::monotony(work.doc, Json(3), plan, script);
        QVERIFY(has_code(issues, "layout_repeats"));
        QVERIFY(has_code(issues, "layout_flat"));
        QVERIFY(has_code(issues, "reveal_beat_small"));
        QVERIFY(!has_code(issues, "book_without_accent"));  // (3 pages with panels: the book-wide check waits for 4)
        for (const auto& issue : issues) {
            QCOMPARE(issue.severity, std::string("warning"));
            QVERIFY(QString::fromStdString(issue.message).contains(QRegularExpression(QStringLiteral("[ぁ-んァ-ヶ一-龥]"))));
            QCOMPARE(genko::wording::error(issue.message), QString::fromStdString(issue.message));
        }
        // a fourth page alike, none slanted nor bled: the book-wide warning
        Json third = grid;
        third["page"] = 3;
        (void)layout::apply_layout(work, Json(3), third, "ai:test");
        Json fourth = grid;
        fourth["page"] = 4;
        QVERIFY(has_code(komawari::monotony(work.doc, Json(4), fourth, script), "book_without_accent"));
        // a page of its own: no warning of the three
        const Json lively = parsed(R"({"page": 4, "tiers": [{"h": 0.62, "tilt_mm": 14, "cols": [{"slot": "p1", "w": 1}]},
                                                             {"h": 0.38, "cols": [{"slot": "p2", "w": 0.4}, {"slot": "p3", "w": 0.6}]}],
                                      "panels": [{"slot": "p1", "bleed": true, "beat_ids": ["b2"]}, {"slot": "p2"}, {"slot": "p3"}]})");
        const auto calm = komawari::monotony(work.doc, Json(4), lively, script);
        for (const char* code : {"layout_repeats", "layout_flat", "reveal_beat_small", "book_without_accent"}) QVERIFY2(!has_code(calm, code), code);
        // the book as it is (review.html): the run of three, once
        int runs = 0;
        for (const auto& [page, issue] : komawari::book_monotony(work.doc, script)) runs += issue.code == "layout_repeats" ? 1 : 0;
        QCOMPARE(runs, 1);
    }

    // Through the agent's tool (contract): a plan without the new keys is kept as before them (no null for them, the
    // same input hash); an idea merged into a plan is submitted, laid as the idea reads, and its warnings never stop it.
    void submitNameTakesAnIdea() {
        QTemporaryDir root;
        const auto fixture = [](const char* name) {
            QFile file(QStringLiteral(GENKO_REPO_ROOT "/native/tests/fixtures/studio/demo4/") + QString::fromLatin1(name));
            if (!file.open(QIODevice::ReadOnly)) return Json();
            return Json::parse(file.readAll().toStdString());
        };
        genko::service::StudioService agent(root.path().toStdString(), "ai:test");
        QVERIFY(agent.call("create_project", Json::array({"demo.genko", "demo", 4}), Json::object()).ok);
        QVERIFY(agent.call("set_bible", Json::array({"demo.genko", fixture("bible.json")}), Json::object({{"commit", true}})).ok);
        QVERIFY(agent.call("set_script", Json::array({"demo.genko", fixture("script.json")}), Json::object({{"commit", true}})).ok);
        const Json p1 = fixture("p001.json");
        const genko::service::ToolResult first = agent.call("submit_name", Json::array({"demo.genko", p1}), Json::object({{"commit", true}}));
        QVERIFY2(first.ok, first.to_dict().dump().c_str());
        const std::string book = root.path().toStdString() + "/demo.genko";
        {
            const auto read = genko::storage::load_document(book);
            const Json stored = (*read.document.page(0).plan)["name"];
            const std::string text = stored.dump();
            for (const char* key : {"\"insets\"", "\"gap_mm\"", "\"tilt_mm\"", "\"border_mm\"", "\"overflow\""}) QVERIFY2(text.find(key) == std::string::npos, key);
            QCOMPARE((*read.document.page(0).plan)["input_hash"], Json(genko::service::worklist::plan_hash(stored)));
            QVERIFY(stored.contains("spread") && stored["panels"][0].contains("sfx_at"));  // (filled as before)
        }
        ideas::Request r = request(ideas::Preset::Action, 3, "km-00000000000000a2");
        const ideas::Batch batch = ideas::generate(r);
        QVERIFY(!batch.ideas.empty());
        const ideas::Idea& idea = batch.ideas.front();
        const Json plan = ideas::merge_into_plan(idea.skeleton, fixture("p002.json"));
        const genko::service::ToolResult second = agent.call("submit_name", Json::array({"demo.genko", plan}), Json::object({{"commit", true}}));
        QVERIFY2(second.ok, second.to_dict().dump().c_str());
        Json expected = Json::array();
        for (const std::string& slot : order_of(idea)) expected.push_back(slot);
        QCOMPARE(second.data["reading_order"], expected);
        for (const auto& issue : second.issues) QVERIFY2(issue.severity != "error", issue.code.c_str());
        const auto read = genko::storage::load_document(book);
        QCOMPARE(read.document.page(1).leaf_frames().size(), std::size_t{3});
    }

private:
    static std::size_t core_panels(const Json& skeleton) {
        std::size_t n = layout::slots_in_order(skeleton["tiers"]).size();
        if (skeleton.contains("insets")) n += skeleton["insets"].size();
        return n;
    }
};

QTEST_GUILESS_MAIN(TestLayoutIdeas)
#include "test_layout_ideas.moc"
