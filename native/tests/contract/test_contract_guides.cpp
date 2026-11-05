// PC5 H-b (SPEC.md VIEW-01「ガイド」, FILE-01「PSD・PSB」) as an agent or a person uses it: add_guide_layout (新規ガイドレイアウト
// を作成…) — its columns, rows, gutters, margins, centred columns, what it is laid on (paper, trim, frame), the guides there
// taken away first, those already there not put twice, its refusals; PhotoCraft v0.2.0's own test's values (file_cmds
// guide_layouts, in mm) —; delete_ruler's kind (the guides alone, the drawing rulers left; every page with for_pages; an
// unknown kind refused; none: every ruler as before); through the command line, one undo; the PSD's resource 1032 as
// psd-tools reads it (written from the page's guides, none without guides — the file byte for byte as before), read back by
// this build, and a PSD whose guides psd-tools put in read by this build onto the page where its picture is.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>

#include "core/command_bus.hpp"
#include "core/guide_layout.hpp"
#include "core/json.hpp"
#include "core/units.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "render/psd_guides.hpp"
#include "render/psd_production.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

// A 100 × 50 mm page (finished 90 × 40), 200 dpi, without panels.
core::Document book() {
    auto doc = core::new_episode("ガイド", core::Num(1), 2, core::PageSpec::custom(100, 50, 90, 40, 3, 5, 5, 5, 5, 200, "mono"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        auto& p = doc.edit_page(i);
        p.numero = false;
        p.frames.clear();
    }
    return doc;
}

struct Outcome {
    bool ok = false;
    std::string error;
    core::Document doc;
    Json results = Json::array();
};

Outcome applied(const core::Document& doc, const Json& ops) {
    try {
        core::ApplyResult r = core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
        return Outcome{true, {}, std::move(r.doc), std::move(r.results)};
    } catch (const std::exception& error) {
        return Outcome{false, error.what(), doc, Json::array()};
    }
}

core::Document run(const core::Document& doc, const Json& op) {
    Outcome r = applied(doc, Json::array({op}));
    if (!r.ok) throw std::runtime_error(r.error);
    return std::move(r.doc);
}

// The page's guides along one way ("v": their x, "h": their y), in the order they were put.
std::vector<double> guides(const core::Document& doc, std::size_t page, const char* axis) {
    std::vector<double> out;
    for (const Json& r : doc.page(page).rulers) {
        if (r.value("kind", std::string()) == "guide" && r.value("axis", std::string()) == axis) out.push_back(r["at"].get<double>());
    }
    return out;
}

std::size_t kinds(const core::Document& doc, std::size_t page, const char* kind) {
    std::size_t n = 0;
    for (const Json& r : doc.page(page).rulers) n += r.value("kind", std::string()) == kind;
    return n;
}

Json layout(Json more) {
    Json op{{"op", "add_guide_layout"}, {"page", 1}};
    for (auto& [k, v] : more.items()) op[k] = v;
    return op;
}

}  // namespace

class TestContractGuides : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    int views_ = 0;

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json guides_view(const QString& psd) {
        const QString out = path(QStringLiteral("guides-%1.json").arg(++views_));
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/psd_guides_check.py"), QStringLiteral("read"), out, psd},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.described().toStdString()}};
        return genko::test::read_json(out);
    }
    bool psd_tools_adds(const QString& in, const QString& out, const Json& items) {
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/psd_guides_check.py"), QStringLiteral("add"), in, out,
                                           QString::fromStdString(items.dump())},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) qWarning() << ran.described();
        return ran.exit_code == 0;
    }
    static bool has_resource(const QString& psd, int id) {
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(psd));
        const auto all = file.resources();
        return std::any_of(all.begin(), all.end(), [id](const auto& r) { return static_cast<int>(r.id) == id; });
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // PhotoCraft's own test (file_cmds/tests.rs guide_layouts) on a page of the same size in mm.
    void layoutAsUpstream() {
        core::Document doc = book();
        Outcome r = applied(doc, Json::array({layout({{"columns", {{"count", 2}, {"gutter_mm", 10}}}, {"margin_mm", Json::array({5, 10, 5, 10})}})}));
        QVERIFY2(r.ok, r.error.c_str());
        doc = r.doc;
        QCOMPARE(guides(doc, 0, "v"), (std::vector<double>{10, 90, 45, 55}));
        QCOMPARE(guides(doc, 0, "h"), (std::vector<double>{5, 45}));
        doc = run(doc, layout({{"rows", {{"count", 2}}}, {"clear", true}}));
        QVERIFY(guides(doc, 0, "v").empty());
        QCOMPARE(guides(doc, 0, "h"), (std::vector<double>{0, 25, 50}));
        // (fixed-width columns centred)
        doc = run(doc, layout({{"columns", {{"count", 2}, {"width_mm", 20}, {"gutter_mm", 10}}}, {"center_columns", true}, {"clear", true}}));
        QCOMPARE(guides(doc, 0, "v"), (std::vector<double>{25, 45, 55, 75}));
        // (nothing to lay out: refused, the page as it was)
        const Outcome none = applied(doc, Json::array({layout(Json::object())}));
        QVERIFY(!none.ok);
        QVERIFY2(none.error.find("give columns, rows or a margin_mm") != std::string::npos, none.error.c_str());
    }

    void layoutsWays() {
        core::Document doc = book();
        // (the guides already there are not put twice; the drawing rulers stay with clear)
        doc = run(doc, Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 50}});
        doc = run(doc, Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "line"}, {"points", Json::array({Json::array({1, 1}), Json::array({9, 9})})}});
        Outcome r = applied(doc, Json::array({layout({{"columns", {{"count", 2}}}})}));
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(guides(r.doc, 0, "v"), (std::vector<double>{50, 0, 100}));
        QCOMPARE(r.results.at(0).value("vertical", -1), 2);
        QCOMPARE(r.results.at(0)["rulers"].size(), std::size_t(2));
        r = applied(r.doc, Json::array({layout({{"rows", {{"count", 1}}}, {"clear", true}})}));
        QCOMPARE(kinds(r.doc, 0, "line"), std::size_t(1));
        QCOMPARE(guides(r.doc, 0, "h"), (std::vector<double>{0, 50}));
        // laid on the finished size and on the 基本枠: within their boxes
        for (const char* from : {"trim", "frame"}) {
            const core::Document laid = run(book(), layout({{"columns", {{"count", 1}}}, {"rows", {{"count", 1}}}, {"from", from}}));
            const core::Rect box = std::string(from) == "trim" ? laid.page(0).trim_rect_mm() : laid.page(0).inner_rect_mm();
            const auto near = [](double a, double b) { return std::abs(a - b) < 0.002; };
            const auto v = guides(laid, 0, "v"), h = guides(laid, 0, "h");
            QCOMPARE(v.size(), std::size_t(2));
            QVERIFY2(near(v[0], box.x.value()) && near(v[1], box.x.value() + box.width.value()), from);
            QVERIFY2(near(h[0], box.y.value()) && near(h[1], box.y.value() + box.height.value()), from);
        }
        // the refusals
        for (const Json& bad : {layout({{"columns", {{"count", -1}}}}), layout({{"columns", {{"count", 2}, {"width_mm", 0}}}}),
                                layout({{"margin_mm", Json::array({1, 2})}}), layout({{"rows", {{"count", 1}}}, {"from", "sky"}}),
                                layout({{"rows", {{"count", 1}}}, {"clear", "yes"}}), layout({{"columns", 3}})}) {
            QVERIFY2(!applied(book(), Json::array({bad})).ok, bad.dump().c_str());
        }
        // (the dialog's preview asks the same function)
        const core::GuideLayout preview = core::guide_layout(book().page(0), layout({{"columns", {{"count", 2}, {"gutter_mm", 10}}}}));
        QCOMPARE(preview.down, (std::vector<double>{0, 45, 55, 100}));
    }

    // delete_ruler's kind: the guides alone (ガイドを消去 on every page, カンバスガイドを消去 on one), as before without it.
    void deleteByKind() {
        core::Document doc = book();
        for (std::size_t page : {1, 2}) {
            doc = run(doc, Json{{"op", "add_ruler"}, {"page", page}, {"kind", "guide"}, {"axis", "h"}, {"at", 10}});
            doc = run(doc, Json{{"op", "add_ruler"}, {"page", page}, {"kind", "line"}, {"points", Json::array({Json::array({1, 1}), Json::array({9, 9})})}});
        }
        core::Document one = run(doc, Json{{"op", "delete_ruler"}, {"page", 1}, {"kind", "guide"}});
        QCOMPARE(kinds(one, 0, "guide"), std::size_t(0));
        QCOMPARE(kinds(one, 0, "line"), std::size_t(1));
        QCOMPARE(kinds(one, 1, "guide"), std::size_t(1));
        const core::Document every = run(doc, Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", "delete_ruler"}, {"kind", "guide"}}})}});
        for (std::size_t p : {0, 1}) {
            QCOMPARE(kinds(every, p, "guide"), std::size_t(0));
            QCOMPARE(kinds(every, p, "line"), std::size_t(1));
        }
        QVERIFY(!applied(doc, Json::array({Json{{"op", "delete_ruler"}, {"page", 1}, {"kind", "cloud"}}})).ok);
        const core::Document all = run(doc, Json{{"op", "delete_ruler"}, {"page", 1}});  // (as before: every ruler)
        QVERIFY(all.page(0).rulers.empty());
    }

    // Through the command line: the layout saved, one undo takes it all away.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(), fs::path(book_path.toStdString()));
        genko::test::write_bytes(path(QStringLiteral("layout.json")),
                                 Json::array({layout({{"columns", {{"count", 3}, {"gutter_mm", 4}}}, {"margin_mm", 5}})}).dump());
        int exit = -1;
        const Json done = genko({QStringLiteral("apply"), book_path, path(QStringLiteral("layout.json")), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0 && done.value("ok", false), done.dump().c_str());
        const auto count = [&] {
            const Json project = Json::parse(genko::test::read_bytes(book_path + QStringLiteral("/project.json")));
            std::size_t n = 0;
            for (const Json& r : project["pages"][0].value("rulers", Json::array())) n += r.value("kind", std::string()) == "guide";
            return n;
        };
        QCOMPARE(count(), std::size_t(2 + 2 + 4));  // (margins 2 + 2, the columns' inner edges 4)
        const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(count(), std::size_t(0));
    }

    // The PSD's 1032: the page's guides as psd-tools reads them, read back here; none without guides (the file as before).
    void psdGuides() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 25.4}});
        doc = run(doc, Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "h"}, {"at", 12.7}});
        doc = run(doc, Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "h"}, {"at", -1.27}});
        const QString file = path(QStringLiteral("guides.psd"));
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), 200);
        const Json view = guides_view(file);
        QVERIFY2(!view.contains("error") && view["guides"].is_object(), view.dump().c_str());
        QCOMPARE(view["guides"]["version"], Json(1));
        // (the review's P3-1: at the picture's pixels — mm at 200 dpi —, in 1/32 of them: 25.4 mm 6400, 12.7 mm 3200, −1.27 mm
        // −320; upright first)
        QVERIFY2(view["guides"]["items"] == Json::parse("[[6400, 0], [3200, 1], [-320, 1]]"), view.dump().c_str());
        const auto near = [](const std::vector<double>& got, const std::vector<double>& wanted) {
            if (got.size() != wanted.size()) return false;
            for (std::size_t i = 0; i < got.size(); ++i)
                if (std::abs(got[i] - wanted[i]) > 0.002) return false;
            return true;
        };
        formats::Opened opened = formats::open_file(genko::test::read_bytes(file), "guides.psd");
        QVERIFY2(near(guides(opened.doc, 0, "v"), {25.4}), Json(guides(opened.doc, 0, "v")).dump().c_str());
        QVERIFY2(near(guides(opened.doc, 0, "h"), {12.7, -1.27}), Json(guides(opened.doc, 0, "h")).dump().c_str());
        // (a PSD read onto a page — import_psd — brings none, as Photoshop's 配置: the page's rulers as they were)
        {
            core::Document onto = book();
            const std::size_t before = onto.page(0).rulers.size();
            const Json op{{"op", "import_psd"}, {"page", 1}, {"path", file.toStdString()}};
            const Outcome done = applied(onto, Json::array({op}));
            QVERIFY2(done.ok, done.error.c_str());
            QCOMPARE(done.doc.page(0).rulers.size(), before);
        }
        // (written again from the book read: the same guides)
        const QString again = path(QStringLiteral("again.psd"));
        (void)formats::export_page_psd(opened.doc, opened.doc.page(0), fs::path(again.toStdString()), 200);
        QCOMPARE(guides_view(again)["guides"]["items"], view["guides"]["items"]);
        // a page without guides: no 1032, the file byte for byte as a table without the guides' row writes it
        const core::Document plain = book();
        const QString without = path(QStringLiteral("plain.psd"));
        (void)formats::export_page_psd(plain, plain.page(0), fs::path(without.toStdString()), 200);
        QVERIFY(!has_resource(without, 1032));
        QVERIFY(guides_view(without)["guides"].is_null());
        {
            std::vector<render::psd_production::Row> others;
            for (const auto& row : render::psd_production::rows())
                if (row.resource != 1032) others.push_back(row);
            render::psd_production::TableForTest table(others);
            const QString before = path(QStringLiteral("before.psd"));
            (void)formats::export_page_psd(plain, plain.page(0), fs::path(before.toStdString()), 200);
            QVERIFY(genko::test::read_bytes(before) == genko::test::read_bytes(without));
        }
        // guides psd-tools put in a PSD: read here where the file's picture is placed (a whole-canvas page: its pixels' mm)
        const QString theirs = path(QStringLiteral("theirs.psd"));
        QVERIFY(psd_tools_adds(without, theirs, Json::parse("[[3200, 0], [-640, 1], [1600, 1]]")));
        formats::Opened read = formats::open_file(genko::test::read_bytes(theirs), "theirs.psd");
        const double mm_per_px = read.doc.page(0).spec.width_mm.value() / std::round(read.doc.page(0).spec.width_mm.value() / 25.4 * 200);
        QVERIFY2(near(guides(read.doc, 0, "v"), {100 * mm_per_px}), Json(guides(read.doc, 0, "v")).dump().c_str());
        QVERIFY2(near(guides(read.doc, 0, "h"), {-20 * mm_per_px, 50 * mm_per_px}), Json(guides(read.doc, 0, "h")).dump().c_str());
    }

    // (the review's P3-3) A 1032 broken or hostile: a count past its data, past 4096, a version not 1, places past the int32's
    // ends — read without falling over: what is there and on (or near) the canvas, the rest left out.
    void hostileGuides() {
        const auto u32 = [](std::string& out, std::uint32_t v) {
            for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (8 * k)) & 0xff);
        };
        const auto block = [&](std::uint32_t version, std::uint32_t count, const std::vector<std::pair<std::int32_t, int>>& items) {
            std::string out;
            u32(out, version);
            u32(out, 576);
            u32(out, 576);
            u32(out, count);
            for (const auto& [place, way] : items) {
                u32(out, static_cast<std::uint32_t>(place));
                out += static_cast<char>(way);
            }
            return out;
        };
        const core::vector::Affine onto{100, 0, 0, 50, 0, 0};  // (a 100 × 50 px canvas on a 100 × 50 mm page)
        render::psd_production::ReadContext context;
        context.onto = &onto;
        context.width_px = 100;
        context.height_px = 50;
        context.opened = true;
        const auto read = [&](const std::string& data) {
            core::Page page = book().page(0);
            const bool taken = render::psd_guides::read(page, data, context);
            std::vector<double> v, h;
            for (const Json& r : page.rulers) (r.value("axis", std::string()) == "v" ? v : h).push_back(r["at"].get<double>());
            return std::tuple{taken, v, h};
        };
        // (a count of 1000000 and two guides' data: the two)
        {
            const auto [taken, v, h] = read(block(1, 1000000, {{320, 0}, {640, 1}}));
            QVERIFY(taken);
            QCOMPARE(v, (std::vector<double>{10}));
            QCOMPARE(h, (std::vector<double>{20}));
        }
        // (5000 in the data: 4096 read at most — each its own place)
        {
            std::vector<std::pair<std::int32_t, int>> many;
            for (int i = 0; i < 5000; ++i) many.emplace_back(i, 0);
            const auto [taken, v, h] = read(block(1, 5000, many));
            QVERIFY(taken);
            QVERIFY(v.size() <= 4096 && v.size() > 100);
            QVERIFY(h.empty());
        }
        // (the int32's ends and far off the canvas: left out; just past an edge: kept)
        {
            const auto [taken, v, h] = read(block(1, 4, {{INT32_MAX, 0}, {INT32_MIN, 1}, {-100 * 32, 0}, {120 * 32, 0}}));
            QVERIFY(taken);
            QCOMPARE(v, (std::vector<double>{-100, 120}));
            QVERIFY(h.empty());
        }
        {
            const auto [taken, v, h] = read(block(1, 1, {{201 * 32, 0}}));
            QVERIFY(taken && v.empty());
        }
        // (a version not 1, data shorter than its head: not read)
        QVERIFY(!std::get<0>(read(block(2, 1, {{320, 0}}))));
        QVERIFY(!std::get<0>(read(std::string("\0\0\0\1\0\0", 6))));
        // (onto a page — not opened —: none)
        context.opened = false;
        QVERIFY(!std::get<0>(read(block(1, 1, {{320, 0}}))));
    }

    // 単位・定規 (core/units): a length in mm in each unit and back.
    void units() {
        using core::units::Unit;
        core::units::Basis basis;
        basis.dpi = 300;
        basis.extent_mm = 200;
        for (const Unit u : {Unit::Px, Unit::In, Unit::Cm, Unit::Mm, Unit::Pt, Unit::Pica, Unit::Percent, Unit::Columns}) {
            for (const double mm : {0.5, 25.4, 123.0}) {
                const double back = core::units::to_mm(core::units::from_mm(mm, u, basis), u, basis);
                QVERIFY2(std::abs(back - mm) < 1e-9, core::units::key_of(u).c_str());
            }
        }
        QCOMPARE(core::units::from_mm(25.4, Unit::Px, basis), 300.0);
        QCOMPARE(core::units::from_mm(25.4, Unit::Pt, basis), 72.0);
        QCOMPARE(core::units::from_mm(25.4, Unit::Pica, basis), 6.0);
        QCOMPARE(core::units::from_mm(50, Unit::Percent, basis), 25.0);
        basis.points_per_inch = core::units::points_per_inch("traditional");
        QCOMPARE(core::units::from_mm(25.4, Unit::Pt, basis), 72.27);
        // (two columns of 180 pt and a 12 pt gutter: 372 pt)
        basis.points_per_inch = 72;
        QCOMPARE(core::units::to_mm(2, Unit::Columns, basis), 372.0 / 72.0 * 25.4);
        QCOMPARE(core::units::unit_of("percent"), std::optional<Unit>(Unit::Percent));
        QVERIFY(!core::units::unit_of("furlong"));
    }
};

QTEST_GUILESS_MAIN(TestContractGuides)
#include "test_contract_guides.moc"
