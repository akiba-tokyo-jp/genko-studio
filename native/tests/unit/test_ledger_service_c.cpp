// Checks the Python tests make that no other C++ test made (M7, the ledger's ports for the studio service): the lint
// codes of a script, a name plan and a bible, the layout of a name plan (gutters, gaps, bleed, slant, spread), the
// lettering (balloons by their speakers, the book's lettering, sound effects, the title page, overflow), the finish
// (marks, rain, effect lines, a balloon nearer someone else, each mark on its own layer), the panel checks, the art
// request's props and panel before, a mask on placed art, and the studio flow around a person's approval and comments.
// Each slot is named after the Python test it ports and checks the same thing with the same inputs.
//
// Ports checks of tests/test_quality_report.py, tests/test_studio_flow.py, tests/test_studio_units.py,
// tests/test_t_report.py and tests/test_tibo_report.py.
//
// DEPS genko_test_support genko_service

#include <QtTest>

#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "formats/pillow_save.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "service/blocking.hpp"
#include "service/book.hpp"
#include "service/finish.hpp"
#include "service/fxwords.hpp"
#include "service/human_service.hpp"
#include "service/layout.hpp"
#include "service/letter.hpp"
#include "service/lint.hpp"
#include "service/result.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "studio/schemas.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
namespace layout = genko::service::layout;
namespace letter = genko::service::letter;
namespace lint = genko::service::lint;
namespace fxwords = genko::service::fxwords;
namespace finish = genko::service::finish;

using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
using genko::core::PageSpec;
using genko::service::HumanService;
using genko::service::StudioService;
using genko::service::ToolResult;
using genko::service::Work;
using genko::studio::Issue;
using MmBox = genko::service::Box;  // x, y, w, h in mm

namespace {

const char* const kTool = "openai:gpt-image-1";  // (test_m5.TOOL)
const char* const kTiboAgent = "ai:test";        // (test_tibo_report.AGENT)
const std::string kAsset = "sha256:" + std::string(64, '1');
const std::string kOther = "sha256:" + std::string(64, '2');

// The bible of test_quality_report (BIBLE): a title, an author and two people.
const char* const kBible = R"({"title": "雨の日", "author": "山田", "characters": [{"id": "hina", "name": "ひな"}, {"id": "sota", "name": "そうた"}]})";

Json parsed(const char* text) { return genko::core::parse_python_json(text); }

// tests/fixtures/studio/demo4/<name> as the Python tests' _load reads it (json.loads).
Json fixture(const std::string& name) {
    return genko::test::read_json(genko::test::repo_root() + QStringLiteral("/tests/fixtures/studio/demo4/") + QString::fromStdString(name));
}

// value[key][key]…, or null when a step is not there.
Json dig(const Json& value, std::initializer_list<const char*> keys) {
    const Json* at = &value;
    for (const char* key : keys) {
        if (!at->is_object() || !at->contains(key)) return Json();
        at = &at->at(key);
    }
    return *at;
}

bool has_text(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string described(const std::vector<Issue>& issues) {
    std::string out;
    for (const Issue& i : issues) out += (out.empty() ? "" : ", ") + i.code + "@" + i.path;
    return out;
}

bool has_issue(const std::vector<Issue>& issues, const std::string& code, const std::string& path) {
    return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) { return i.code == code && i.path == path; });
}

bool has_code(const std::vector<Issue>& issues, const std::string& code) {
    return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) { return i.code == code; });
}

std::vector<Issue> errors_of(const std::vector<Issue>& issues) {
    std::vector<Issue> out;
    for (const Issue& i : issues) {
        if (i.severity == "error") out.push_back(i);
    }
    return out;
}

// The kinds of a next() reply's items, in order.
std::vector<std::string> kinds_of(const ToolResult& reply) {
    std::vector<std::string> out;
    const Json items = dig(reply.data, {"items"});
    if (!items.is_array()) return out;
    for (const Json& item : items) out.push_back(item.value("kind", std::string()));
    return out;
}

// What apply_ops raised: its ApplyError's words ("" when it applied; "<other: …>" for another exception).
std::string apply_error(const Document& doc, const Json& ops, const std::string& actor) {
    try {
        (void)genko::service::apply_ops(doc, ops, actor);
    } catch (const genko::core::ApplyError& e) {
        return e.what();
    } catch (const std::exception& e) {
        return std::string("<other: ") + e.what() + ">";
    }
    return {};
}

// The panel brief of the page's first panel, to change (an empty one made when it has none).
Json& leaf_panel(Document& doc, std::size_t page = 0) {
    genko::core::Page& edited = doc.edit_page(page);
    const std::string id = edited.leaf_frames().at(0)->id;
    genko::core::Frame* frame = edited.find_frame(id);
    if (!frame->panel) frame->panel = Json::object();
    return *frame->panel;
}

MmBox box_of(const layout::CompiledLayout& compiled, const std::string& slot) {
    const layout::RectMm* rect = compiled.rect_of(slot);
    return rect == nullptr ? MmBox{0, 0, 0, 0} : genko::service::as_box(*rect);
}

// test_quality_report._book: a B4 book whose names are approved.
Document quality_book(int pages = 2) {
    Document doc = genko::core::new_episode("t", Num(1), pages, PageSpec::b4_comic());
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).name_ok = true;
    return doc;
}

// test_tibo_report._cand
Json tibo_candidate(const std::string& id, const std::string& asset = kAsset) {
    return Json::object({{"id", id},
                         {"asset", asset},
                         {"px", Json::array({800, 1200})},
                         {"origin", Json::object({{"kind", "agent"}, {"tool_id", "xai:grok-imagine"}})},
                         {"status", "candidate"}});
}

// test_tibo_report._book: a B5 book whose names are approved, each page's first panel with its art adopted (c1) and
// tibo in it.
Document tibo_book(int pages = 2) {
    Document doc = genko::core::new_episode("t", Num(1), pages, PageSpec::b5_doujin());
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        doc.edit_page(i).name_ok = true;
        leaf_panel(doc, i) = Json::object({{"status", "adopted"},
                                           {"candidates", Json::array({tibo_candidate("c1"), tibo_candidate("c2", kOther)})},
                                           {"adopted", Json::object({{"art", "c1"}})},
                                           {"characters", Json::array({Json::object({{"id", "tibo"}})})}});
    }
    return doc;
}

// test_tibo_report._lettered: one page whose first panel has these faces reported ([char, rect_mm] each) and the line
// 「リセット来るぞ！」 in this box, said by tibo. The book and the line's id.
std::pair<Document, std::string> tibo_lettered(const std::array<int, 4>& box, const Json& faces) {
    Document doc = tibo_book(1);
    Json regions = Json::array();
    for (const Json& face : faces) {
        regions.push_back(Json::object({{"kind", "face"}, {"char", face.at(0)}, {"rect_mm", face.at(1)}, {"source", "agent"}}));
    }
    leaf_panel(doc)["regions"] = regions;
    const std::string frame_id = doc.page(0).leaf_frames().at(0)->id;
    genko::core::StoryLine& line =
        doc.add_line(Num(1), "リセット来るぞ！", "", std::nullopt, "", Num(box[0]), Num(box[1]), Num(box[2]), Num(box[3]));
    line.frame_id = frame_id;
    line.style = Json::object({{"speaker_id", "tibo"}});
    const std::string id = line.id;
    return {std::move(doc), id};
}

// A name plan laid out on a page and lettered (test_quality_report._two_person_page's results).
struct Lettered {
    Work work;
    Json plan;
    layout::CompiledLayout compiled;
    std::vector<letter::BalloonPlacement> placements;
    std::vector<Issue> issues;
};

// test_quality_report._two_person_page: hina (left) and sota (right) in the one panel of page 1 of a two-page B4 book,
// laid out as an agent's plan is (agent "ai:agent", apply_layout's default) and its lines lettered with b1 said by
// hina, b2 by sota and b3 by no one.
Lettered two_person_page(const Json& lines, const std::string& tmpl = "splash", const Json& extra = Json::object(),
                         const Json& bible = parsed(kBible)) {
    Lettered out;
    out.work.doc = genko::core::new_episode("t", Num(1), 2, PageSpec::b4_comic());
    Json panel = Json::object({{"slot", "p1"},
                               {"shot", "MS"},
                               {"characters", Json::array({Json::object({{"id", "hina"}, {"pos", "left"}, {"scale", 0.8}}),
                                                           Json::object({{"id", "sota"}, {"pos", "right"}, {"scale", 0.8}})})},
                               {"lines", lines}});
    for (const auto& [key, value] : extra.items()) panel[key] = value;
    out.plan = Json::object({{"page", 1}, {"template", tmpl}, {"panels", Json::array({panel})}});
    out.compiled = layout::apply_layout(out.work, Json(1), out.plan, "ai:agent");
    const Json speakers = Json::object({{"b1", "hina"}, {"b2", "sota"}, {"b3", nullptr}});
    std::tie(out.placements, out.issues) = letter::place_page(out.plan, out.compiled, bible, speakers);
    return out;
}

// A picture of `px` pixels as agents/images.sheet draws it: a light ground, the face close-up at the top centre and a
// standing figure below it.
std::string sheet_png(const Json& px) {
    const int w = static_cast<int>(genko::core::to_float(px.at(0)));
    const int h = static_cast<int>(genko::core::to_float(px.at(1)));
    genko::render::Image image = genko::render::Image::create("L", genko::render::Size{w, h}, genko::render::Ink(245));
    const int side = static_cast<int>(0.4 * w);
    const int left = static_cast<int>(0.3 * w);
    const int top = static_cast<int>(0.03 * h);
    image.paste(genko::render::Ink(255), genko::render::Box{left, top, std::min(w, left + side), std::min(h, top + side)});
    const int body_top = std::min(h, top + side + 20);
    image.paste(genko::render::Ink(120), genko::render::Box{static_cast<int>(0.35 * w), body_top, static_cast<int>(0.65 * w),
                                                            std::max(body_top, static_cast<int>(0.95 * h))});
    return genko::formats::png_bytes(image.convert("RGB"));
}

// A panel's picture of `px` pixels as agents/images.panel draws it: a grey gradient ground, and a grey body and a white
// head where the request's pose guide puts each figure.
std::string panel_png(const Json& px, const Json& figures) {
    const int w = static_cast<int>(genko::core::to_float(px.at(0)));
    const int h = static_cast<int>(genko::core::to_float(px.at(1)));
    genko::render::Image image = genko::render::Image::create("L", genko::render::Size{w, h}, genko::render::Ink(230));
    for (int y = 0; y < h; y += 8) {
        image.paste(genko::render::Ink(150 + static_cast<int>(80.0 * y / h)), genko::render::Box{0, y, w, std::min(h, y + 9)});
    }
    const auto pixels = [w, h](const Json& box01) {
        const double bx = genko::core::to_float(box01.at(0)), by = genko::core::to_float(box01.at(1));
        const double bw = genko::core::to_float(box01.at(2)), bh = genko::core::to_float(box01.at(3));
        const int x0 = std::clamp(static_cast<int>(bx * w), 0, w), y0 = std::clamp(static_cast<int>(by * h), 0, h);
        const int x1 = std::clamp(static_cast<int>((bx + bw) * w), x0, w), y1 = std::clamp(static_cast<int>((by + bh) * h), y0, h);
        return genko::render::Box{x0, y0, x1, y1};
    };
    if (figures.is_array()) {
        for (const Json& figure : figures) {
            image.paste(genko::render::Ink(90), pixels(figure.at("body01")));
            image.paste(genko::render::Ink(250), pixels(figure.at("head01")));
        }
    }
    return genko::formats::png_bytes(image.convert("RGB"));
}

// Ops applied and saved as a person (HumanService._apply).
void as_person(const fs::path& book, const Json& ops) {
    try {
        genko::service::Session session(book, "human:leaf");
        session.apply(ops, "human:leaf");
        session.save("human:leaf");
    } catch (const std::exception& e) {
        QFAIL(e.what());
    }
}

// test_studio_flow._run_agent: a scripted agent makes demo.genko under `root` (4 pages), writes the bible and the
// script, and submits the name of each page, dry first and then committed (next() says what to do at each stage).
void run_agent(const fs::path& root) {
    StudioService agent(root, "ai:test");
    const ToolResult made = agent.call("create_project", Json::array({"demo.genko", "demo", 4}), Json::object());
    QVERIFY2(made.ok, made.to_dict().dump().c_str());
    const std::vector<std::string> first = kinds_of(agent.call("next", Json::array({"demo.genko"}), Json::object()));
    QVERIFY(!first.empty() && first[0] == "write_bible");
    const ToolResult bible = agent.call("set_bible", Json::array({"demo.genko", fixture("bible.json")}), Json::object({{"commit", true}}));
    QVERIFY2(bible.ok, bible.to_dict().dump().c_str());
    const std::vector<std::string> second = kinds_of(agent.call("next", Json::array({"demo.genko"}), Json::object()));
    QVERIFY(!second.empty() && second[0] == "write_script");
    const ToolResult script = agent.call("set_script", Json::array({"demo.genko", fixture("script.json")}), Json::object({{"commit", true}}));
    QVERIFY2(script.ok, script.to_dict().dump().c_str());
    QVERIFY(kinds_of(agent.call("next", Json::array({"demo.genko"}), Json::object({{"limit", 10}}))) == std::vector<std::string>(4, "plan_page"));
    for (int n = 1; n <= 4; ++n) {
        const Json plan = fixture("p00" + std::to_string(n) + ".json");
        const ToolResult dry = agent.call("submit_name", Json::array({"demo.genko", plan}), Json::object());
        QVERIFY2(dry.ok && dig(dry.data, {"committed"}) == Json(false) && !dry.images.empty(), dry.to_dict().dump().c_str());
        const ToolResult result = agent.call("submit_name", Json::array({"demo.genko", plan}), Json::object({{"commit", true}}));
        QVERIFY2(result.ok && dig(result.data, {"committed"}) == Json(true), result.to_dict().dump().c_str());
    }
}

// test_m5._project: demo.genko under `root` with the demo4 bible, script and names, the names approved by a person.
void m5_project(const fs::path& root) {
    StudioService agent(root, "ai:test");
    const ToolResult made = agent.call("create_project", Json::array({"demo.genko", "demo", 4}), Json::object());
    QVERIFY2(made.ok, made.to_dict().dump().c_str());
    const ToolResult bible = agent.call("set_bible", Json::array({"demo.genko", fixture("bible.json")}), Json::object({{"commit", true}}));
    QVERIFY2(bible.ok, bible.to_dict().dump().c_str());
    const ToolResult script = agent.call("set_script", Json::array({"demo.genko", fixture("script.json")}), Json::object({{"commit", true}}));
    QVERIFY2(script.ok, script.to_dict().dump().c_str());
    for (int n = 1; n <= 4; ++n) {
        const ToolResult name =
            agent.call("submit_name", Json::array({"demo.genko", fixture("p00" + std::to_string(n) + ".json")}), Json::object({{"commit", true}}));
        QVERIFY2(name.ok, name.to_dict().dump().c_str());
    }
    const Json approved = HumanService(root / "demo.genko", "human:leaf").approve_name(Json::array({1, 2, 3, 4}));
    QVERIFY2(approved.value("ok", false), approved.dump().c_str());
}

// test_m5._sheets: hina's and sora's character sheets asked for, made, imported and approved by a person.
void m5_sheets(const fs::path& root) {
    StudioService agent(root, "ai:test");
    for (const char* cid : {"hina", "sora"}) {
        const ToolResult req = agent.call("generation_request", Json::array({"demo.genko"}), Json::object({{"character_id", cid}}));
        QVERIFY2(req.ok, req.to_dict().dump().c_str());
        const Json request = dig(req.data, {"request"});
        const std::string id = request.value("id", std::string());
        const fs::path inbox = genko::core::path_from_utf8(req.data.value("inbox", std::string()));
        genko::storage::write_atomic(inbox / "s.png", sheet_png(dig(request, {"size", "suggested_px"})));
        const Json images = Json::array({Json::object({{"file", "studio/inbox/" + id + "/s.png"}, {"origin", Json::object({{"tool_id", kTool}})}})});
        const ToolResult imported = agent.call("import_images", Json::array({"demo.genko", id, images}), Json::object());
        QVERIFY2(imported.ok, imported.to_dict().dump().c_str());
        const Json candidates = dig(imported.data, {"candidates"});
        QVERIFY(candidates.is_array() && !candidates.empty());
        const Json approved = HumanService(root / "demo.genko", "human:leaf").approve_sheet(Json(cid), candidates.at(0));
        QVERIFY2(approved.value("ok", false), approved.dump().c_str());
    }
}

// test_m5._art: a panel's art asked for, made, imported, adopted (not enlarged) and its figures reported as regions;
// `candidate` gets the adopted candidate's id.
void m5_art(const fs::path& root, int page, const std::string& frame_id, std::string& candidate) {
    StudioService agent(root, "ai:test");
    const ToolResult req = agent.call("generation_request", Json::array({"demo.genko"}), Json::object({{"page", page}, {"frame_id", frame_id}}));
    QVERIFY2(req.ok, req.to_dict().dump().c_str());
    const Json request = dig(req.data, {"request"});
    const std::string id = request.value("id", std::string());
    const fs::path inbox = genko::core::path_from_utf8(req.data.value("inbox", std::string()));
    genko::storage::write_atomic(inbox / "a.png", panel_png(dig(request, {"size", "suggested_px"}), dig(request, {"figures"})));
    const Json images = Json::array({Json::object({{"file", "studio/inbox/" + id + "/a.png"},
                                                   {"origin", Json::object({{"tool_id", kTool}, {"model", "gpt-image-1"}, {"prompt", "x"}})}})});
    const ToolResult imported = agent.call("import_images", Json::array({"demo.genko", id, images}), Json::object());
    QVERIFY2(imported.ok, imported.to_dict().dump().c_str());
    const Json candidates = dig(imported.data, {"candidates"});
    QVERIFY(candidates.is_array() && !candidates.empty());
    candidate = candidates.at(0).get<std::string>();
    const ToolResult adopted =
        agent.call("adopt", Json::array({"demo.genko", candidate}), Json::object({{"page", page}, {"frame_id", frame_id}, {"upscale", false}}));
    QVERIFY2(adopted.ok, adopted.to_dict().dump().c_str());
    const ToolResult inspected = agent.call("inspect", Json::array({"demo.genko", "panel", page, frame_id}), Json::object());
    QVERIFY2(inspected.ok, inspected.to_dict().dump().c_str());
    const Json panels = dig(inspected.data, {"panels"});
    QVERIFY(panels.is_array() && !panels.empty());
    Json regions = Json::array();
    for (const Json& figure : panels.at(0).value("figures", Json::array())) {
        regions.push_back(Json::object({{"kind", "face"}, {"char", figure.at("char")}, {"rect_mm", figure.at("head_mm")}}));
        regions.push_back(Json::object({{"kind", "person"}, {"char", figure.at("char")}, {"rect_mm", figure.at("body_mm")}}));
    }
    if (!regions.empty()) {
        const ToolResult reported = agent.call("report_regions", Json::array({"demo.genko", page, frame_id, regions}), Json::object());
        QVERIFY2(reported.ok, reported.to_dict().dump().c_str());
    }
}

// The cut_by_panel issues of a book's first page (checks.page_issues).
std::vector<Json> cut_by_panel(const Document& doc) {
    std::vector<Json> out;
    for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
        if (issue.value("code", std::string()) == "cut_by_panel") out.push_back(issue);
    }
    return out;
}

}  // namespace

class TestLedgerServiceC : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = genko::core::path_from_utf8((dir_.path() + QLatin1Char('/') + name).toStdString());
        fs::create_directories(out);
        return out;
    }

private slots:
    // (the Python tests' autouse fixture: a config folder of their own and the user leaf)
    void initTestCase() {
        QVERIFY(dir_.isValid());
        qputenv("GENKO_CONFIG_DIR", (dir_.path() + QStringLiteral("/config")).toUtf8());
        qputenv("GENKO_USER", "leaf");
    }

    // --- tests/test_quality_report.py ----------------------------------------------------------------------------------

    // A balloon of one of two people in a panel sits on its speaker's side, and its tail ends by the speaker's face
    // (under 4 mm from it, never on it); the add_line op carries the speaker.
    // (Python: test_quality_report.py test_a_balloon_sits_by_its_speaker_and_its_tail_reaches_them)
    void test_a_balloon_sits_by_its_speaker_and_its_tail_reaches_them() {
        const Lettered page = two_person_page(parsed(R"([{"beat_id": "b1", "balloon": "speech", "breaks": ["ちっちゃ!"]}])"));
        const layout::RectMm* rect = page.compiled.rect_of("p1");
        QVERIFY(rect != nullptr);
        const std::vector<genko::service::Figure> figs = genko::service::figures(page.plan["panels"][0], *rect);
        QCOMPARE(figs.size(), std::size_t{2});
        const MmBox hina = genko::service::as_box(figs[0].head);
        const MmBox sota = genko::service::as_box(figs[1].head);
        QVERIFY(!page.placements.empty());
        const letter::BalloonPlacement& p = page.placements[0];
        const double centre = p.x_mm + p.w_mm / 2;
        const double hina_x = hina[0] + hina[2] / 2;
        const double sota_x = sota[0] + sota[2] / 2;
        QVERIFY2(std::abs(centre - hina_x) < std::abs(centre - sota_x), (std::to_string(centre) + " " + std::to_string(hina_x)).c_str());
        QVERIFY(p.tail.has_value());
        const double tx = (*p.tail)[0];
        const double ty = (*p.tail)[1];
        const double hx = hina[0], hy = hina[1], hw = hina[2], hh = hina[3];
        const double gap = std::max({hx - tx, tx - (hx + hw), hy - ty, ty - (hy + hh), 0.0});
        QVERIFY2(gap < 4.0, std::to_string(gap).c_str());  // (the point ends by the face, not 7 mm out of the balloon)
        QVERIFY(!(hx < tx && tx < hx + hw && hy < ty && ty < hy + hh));  // (and never on it)
        const Json ops = letter::placements_to_ops(Json(1), page.placements, page.compiled);
        QVERIFY(!ops.empty());
        QCOMPARE(dig(ops.at(0), {"style", "speaker_id"}), Json("hina"));
    }

    // A shout is lettered big and bold, a whisper small, narration with no font forced; the bible's lettering changes
    // them (a shout at its usual size, narration in maru), and a font that is not there is a lint warning.
    // (Python: test_quality_report.py test_shouts_are_big_and_whispers_small_and_the_book_can_change_them)
    void test_shouts_are_big_and_whispers_small_and_the_book_can_change_them() {
        const Lettered three = two_person_page(parsed(R"([{"beat_id": "b1", "balloon": "shout", "breaks": ["よくない!"]},
                                                          {"beat_id": "b2", "balloon": "whisper", "breaks": ["うん"]},
                                                          {"beat_id": "b3", "balloon": "narration", "breaks": ["雨だった"]}])"));
        QCOMPARE(three.placements.size(), std::size_t{3});
        const Json shout = three.placements[0].style.value_or(Json::object());
        const Json whisper = three.placements[1].style.value_or(Json::object());
        const Json narration = three.placements[2].style.value_or(Json::object());
        QVERIFY2(shout.contains("size_mm") && genko::core::to_float(shout["size_mm"]) > 6 && shout.value("weight", std::string()) == "bold",
                 shout.dump().c_str());
        QVERIFY2(whisper.contains("size_mm") && genko::core::to_float(whisper["size_mm"]) < 4.5, whisper.dump().c_str());
        QVERIFY2(!genko::core::py_truthy(genko::core::py_get(narration, "font")), narration.dump().c_str());  // (no font forced on the book)
        Json bible = parsed(kBible);
        bible["lettering"] = parsed(R"({"narration": {"font": "maru", "scale": null, "weight": null}, "shout": {"scale": 1.0}})");
        const Lettered changed = two_person_page(parsed(R"([{"beat_id": "b1", "balloon": "shout", "breaks": ["よくない!"]},
                                                            {"beat_id": "b3", "balloon": "narration", "breaks": ["雨だった"]}])"),
                                                 "splash", Json::object(), bible);
        QCOMPARE(changed.placements.size(), std::size_t{2});
        QVERIFY(changed.placements[0].style.has_value());
        QVERIFY2(!changed.placements[0].style->contains("size_mm"), changed.placements[0].style->dump().c_str());
        QCOMPARE(genko::core::py_get(changed.placements[1].style.value_or(Json::object()), "font"), Json("maru"));
        Json unknown = bible;
        unknown["lettering"] = parsed(R"({"narration": {"font": "nothing-like-it"}})");
        const std::vector<Issue> linted = lint::lint_bible(unknown);
        QVERIFY(!linted.empty());
        QCOMPARE(linted.back().code, std::string("unknown_font"));
    }

    // The effect words: rain, art, a mark and an effect known by kind; an unknown word in a plan a fx_unknown warning at
    // its place; the words written into the art request (an unknown one as it is, said to be Japanese); a panel's
    // weight in words only for a big moment.
    // (Python: test_quality_report.py test_effect_words_are_known_warned_and_written_into_the_art_request)
    void test_effect_words_are_known_warned_and_written_into_the_art_request() {
        const auto kind_of = [](const char* word) {
            const std::optional<Json> found = fxwords::resolve(Json(word));
            return found ? found->value("kind", std::string()) : std::string("(unknown)");
        };
        QCOMPARE(kind_of("雨"), std::string("rain"));
        QCOMPARE(kind_of("水しぶき"), std::string("art"));
        QCOMPARE(kind_of("汗"), std::string("mark"));
        QCOMPARE(kind_of("集中線"), std::string("effect"));
        Json plan = fixture("p001.json");
        plan["panels"][0]["fx"] = Json::array({"雨", "ぴかぴかビーム"});
        const std::vector<Issue> issues = lint::lint_name_plan(plan, fixture("script.json"), fixture("bible.json"), 4);
        std::vector<std::string> paths;
        for (const Issue& issue : issues) {
            if (issue.code == "fx_unknown") paths.push_back(issue.path);
        }
        QVERIFY2(paths == std::vector<std::string>{"/panels/0/fx/1"}, described(issues).c_str());
        const auto [ja, en] = fxwords::art_words(Json::array({"雨", "ぴかぴかビーム"}));
        QCOMPARE(ja.size(), std::size_t{2});
        QCOMPARE(en.size(), std::size_t{2});
        QVERIFY2(has_text(en[0], "rain"), en[0].c_str());
        QVERIFY2(has_text(en[1], "(in Japanese)"), en[1].c_str());
        QVERIFY(fxwords::emphasis_words(Json(0.9)).has_value());
        QVERIFY(!fxwords::emphasis_words(Json(0.5)).has_value());
    }

    // The finish of a panel with 汗, 雨, 集中線, 水しぶき and an unknown word: focus lines, the 汗 mark stamped beside
    // the reported face, at least 12 rain strokes, notes for each word; applied, 汗 and 雨 are on a finishing layer each
    // (fx-…, titled "…・仕上げ）"), and a second finish draws them no more.
    // (Python: test_quality_report.py test_the_finish_draws_marks_by_the_face_rain_and_lines)
    void test_the_finish_draws_marks_by_the_face_rain_and_lines() {
        Document doc = quality_book(1);
        const genko::core::Rect r = doc.page(0).leaf_frames().at(0)->rect;
        const double x = r.x.value();
        const double y = r.y.value();
        leaf_panel(doc) = Json::object({{"status", "adopted"},
                                        {"fx", Json::array({"汗", "雨", "集中線", "水しぶき", "なぞの音"})},
                                        {"regions", Json::array({Json::object({{"kind", "face"}, {"char", "hina"}, {"rect_mm", Json::array({x + 40, y + 40, 30, 30})}})})}});
        const auto [ops, notes] = finish::plan(doc, doc.page(0));
        std::vector<std::string> kinds;
        for (const Json& op : ops) kinds.push_back(op.value("op", std::string()));
        QVERIFY(std::find(kinds.begin(), kinds.end(), "add_effect") != kinds.end());
        QVERIFY(std::find(kinds.begin(), kinds.end(), "stamp_material") != kinds.end());
        QVERIFY2(std::count(kinds.begin(), kinds.end(), "add_stroke") >= 12, ops.dump().c_str());
        Json stamp;
        for (const Json& op : ops) {
            if (op.value("op", std::string()) == "stamp_material") {
                stamp = op;
                break;
            }
        }
        QCOMPARE(dig(stamp, {"material_id"}), Json("mark-汗"));
        QVERIFY2(genko::core::to_float(stamp.at("x_mm")) > x + 70, stamp.dump().c_str());  // (beside the face)
        std::set<std::string> said;
        for (const Json& note : notes) said.insert(note.value("kind", std::string()));
        for (const char* kind : {"fx_in_art", "fx_unknown", "add_mark", "add_rain"}) QVERIFY2(said.count(kind) == 1, kind);
        const Document done = genko::service::apply_ops(doc, ops, "genko").doc;
        std::vector<const genko::core::Layer*> finishing;
        for (const genko::core::Layer& layer : done.page(0).layers) {
            if (layer.id.starts_with("fx-")) finishing.push_back(&layer);
        }
        QCOMPARE(finishing.size(), std::size_t{2});  // (汗 and 雨, one layer each)
        bool rain = false;
        for (const genko::core::Layer* layer : finishing) {
            QVERIFY2(layer->title.ends_with("・仕上げ）"), layer->title.c_str());
            rain = rain || (layer->title.starts_with("雨") && layer->stroke_count() >= 12);
        }
        QVERIFY(rain);
        const Json again = finish::plan(done, done.page(0)).first;
        for (const Json& op : again) {
            const std::string name = op.value("op", std::string());
            QVERIFY2(name != "stamp_material" && name != "add_stroke", again.dump().c_str());  // (once)
        }
    }

    // A sound effect is lettered by its source (sfx_at: 0.3 across, 0.85 down the panel).
    // (Python: test_quality_report.py test_a_sound_effect_goes_to_its_source)
    void test_a_sound_effect_goes_to_its_source() {
        const Lettered page = two_person_page(parsed(R"([{"beat_id": "b3", "balloon": "sfx", "breaks": ["バシャッ"]}])"), "splash",
                                              parsed(R"({"sfx_at": [0.3, 0.85]})"));
        QVERIFY(page.compiled.rect_of("p1") != nullptr);
        const MmBox r = box_of(page.compiled, "p1");
        QVERIFY(!page.placements.empty());
        const letter::BalloonPlacement& p = page.placements[0];
        QVERIFY2(std::abs(p.x_mm + p.w_mm / 2 - (r[0] + r[2] * 0.3)) < 25, std::to_string(p.x_mm).c_str());
        QVERIFY2(p.y_mm + p.h_mm / 2 > r[1] + r[3] * 0.6, std::to_string(p.y_mm).c_str());
    }

    // A prop of the bible with its picture, set on a panel, goes with that panel's art request (refs/prop_kasa_…, its
    // name in the Japanese prompt), and so does the panel before it once that one has art (refs/previous_panel.png).
    // (Python: test_quality_report.py test_props_and_the_panel_before_go_with_the_art_request)
    void test_props_and_the_panel_before_go_with_the_art_request() {
        const fs::path root = folder(QStringLiteral("props"));
        m5_project(root);
        if (QTest::currentTestFailed()) return;
        m5_sheets(root);
        if (QTest::currentTestFailed()) return;
        const fs::path book = root / "demo.genko";
        const Document doc = genko::service::load_episode(book);
        const std::string umbrella = genko::storage::AssetStore(book).put_bytes(sheet_png(Json::array({256, 256})), ".png");
        as_person(book, Json::array({Json::object({{"op", "upsert_prop"}, {"prop", Json::object({{"id", "kasa"}, {"name", "傘"}, {"desc", "うさぎ柄の透明傘"}})}}),
                                     Json::object({{"op", "attach_reference"}, {"target", Json::object({{"prop_id", "kasa"}})}, {"asset", umbrella}})}));
        if (QTest::currentTestFailed()) return;
        const std::vector<const genko::core::Frame*> frames = doc.page(0).leaf_frames();
        QVERIFY(frames.size() >= 2);
        const std::string first = frames[0]->id;
        const std::string second = frames[1]->id;
        std::string candidate;
        m5_art(root, 1, first, candidate);
        if (QTest::currentTestFailed()) return;
        as_person(book, Json::array({Json::object({{"op", "set_panel"}, {"page", 1}, {"frame_id", second}, {"set", Json::object({{"props", Json::array({"kasa"})}})}})}));
        if (QTest::currentTestFailed()) return;
        const ToolResult req = StudioService(root, "ai:test").call("generation_request", Json::array({"demo.genko"}),
                                                                   Json::object({{"page", 1}, {"frame_id", second}}));
        QVERIFY2(req.ok, req.to_dict().dump().c_str());
        const Json refs = dig(req.data, {"request", "files", "references"});
        QVERIFY2(refs.is_array(), req.data.dump().c_str());
        bool prop = false;
        bool before = false;
        for (const Json& ref : refs) {
            const std::string name = ref.is_string() ? ref.get<std::string>() : std::string();
            prop = prop || name.starts_with("refs/prop_kasa_");
            before = before || name == "refs/previous_panel.png";
        }
        QVERIFY2(prop && before, refs.dump().c_str());
        const Json ja = dig(req.data, {"request", "prompt", "ja"});
        QVERIFY2(ja.is_string() && has_text(ja.get<std::string>(), "傘"), ja.dump().c_str());
    }

    // A plan's own bleed and slant on a panel and a spread: the panel bleeds and is slanted (a tilted split of 12 mm),
    // the page faces the next one; the newer templates are there.
    // (Python: test_quality_report.py test_bleed_slant_and_spread_in_the_plan)
    void test_bleed_slant_and_spread_in_the_plan() {
        Work work{genko::core::new_episode("t", Num(1), 4, PageSpec::b4_comic()), Json::array()};
        const Json plan =
            parsed(R"({"page": 2, "template": "2tier_wide", "spread": true, "panels": [{"slot": "p1", "bleed": true, "slant": 12}, {"slot": "p2"}]})");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(2), plan, "ai:agent");
        const genko::core::Page& page = work.doc.page(1);
        const std::vector<const genko::core::Frame*> leaves = page.leaf_frames();
        QCOMPARE(leaves.size(), std::size_t{2});
        QVERIFY(leaves[0]->bleed);
        QVERIFY(leaves[0]->poly.has_value() && !leaves[0]->poly->empty());
        QVERIFY(page.spread_with.has_value() && page.spread_with->value() == 3);
        bool tilted = false;
        for (const Json& op : compiled.ops) tilted = tilted || (op.contains("tilt_mm") && genko::core::to_float(op["tilt_mm"]) == 12.0);
        QVERIFY2(tilted, compiled.ops.dump().c_str());
        for (const char* name : {"reveal_top", "finale_bleed", "action_slant", "reveal_bleed", "title_top"}) QVERIFY2(layout::templates().contains(name), name);
    }

    // The size hints of a plan: on the last page a small last panel (finale_small) unless it bleeds, a weighty panel
    // that is small (emphasis_small), and a reveal whose first panel is small (reveal_small). (lint._size_hints is
    // internal to lint.cpp here, as it is a private helper in Python: it is reached through lint_name_plan, whose page
    // is the plan's own, so the plan says page 2 for _size_hints(plan, 2, 4).)
    // (Python: test_quality_report.py test_small_big_moments_are_pointed_out)
    void test_small_big_moments_are_pointed_out() {
        const Json script = fixture("script.json");
        const Json bible = fixture("bible.json");
        Json plan = fixture("p001.json");  // (tiers 0.35 / 0.65 split 0.55 and 0.45: the last panel is under 3 tenths)
        plan["page"] = 4;
        plan["panels"][2]["emphasis"] = 0.9;
        const std::vector<Issue> last = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(has_code(last, "finale_small") && has_code(last, "emphasis_small"), described(last).c_str());
        plan["panels"][2]["bleed"] = true;
        const std::vector<Issue> bled = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(!has_code(bled, "finale_small"), described(bled).c_str());
        plan["turn_role"] = "reveal";
        plan["tiers"][0]["h"] = 0.2;
        plan["page"] = 2;
        const std::vector<Issue> reveal = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(has_code(reveal, "reveal_small"), described(reveal).c_str());
    }

    // A title page: the bible's title down the right edge and the author's name smaller, set before the page's line;
    // applied, they are the page's first lines. A title page without an author in the bible is warned about.
    // (Python: test_quality_report.py test_a_title_page_sets_the_title_and_author)
    void test_a_title_page_sets_the_title_and_author() {
        Work work{genko::core::new_episode("t", Num(1), 2, PageSpec::b4_comic()), Json::array()};
        const Json plan = parsed(R"({"page": 1, "template": "reveal_bleed", "title": true, "panels": [{"slot": "p1", "shot": "LS", "characters": [], "lines": [
            {"beat_id": "b1", "balloon": "speech", "breaks": ["……あ。"]}]}]})");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:agent");
        const std::vector<letter::BalloonPlacement> placements = letter::place_page(plan, compiled, parsed(kBible), parsed(R"({"b1": "hina"})")).first;
        QCOMPARE(placements.size(), std::size_t{3});
        const letter::BalloonPlacement& title = placements[0];
        const letter::BalloonPlacement& author = placements[1];
        const letter::BalloonPlacement& line = placements[2];
        QCOMPARE(title.text, std::string("雨の日"));
        QCOMPARE(author.text, std::string("山田"));
        QVERIFY(title.style.has_value() && title.style->contains("size_mm"));
        QVERIFY(author.style.has_value() && author.style->contains("size_mm"));
        QVERIFY(genko::core::to_float(title.style->at("size_mm")) > genko::core::to_float(author.style->at("size_mm")));
        QVERIFY(title.x_mm > line.x_mm);  // (the title down the right edge, the line after it)
        const Json ops = letter::placements_to_ops(Json(1), placements, compiled);
        const Document lettered = genko::service::apply_ops(work.doc, ops, "genko").doc;
        const std::vector<const genko::core::StoryLine*> lines = lettered.story_for_page(Num(1));
        QVERIFY(lines.size() >= 2);
        QCOMPARE(lines[0]->text, std::string("雨の日"));
        QCOMPARE(lines[1]->text, std::string("山田"));
        Json titled = fixture("p001.json");
        titled["title"] = true;
        Json authorless = fixture("bible.json");
        authorless["author"] = nullptr;
        const std::vector<Issue> missing = lint::lint_name_plan(titled, fixture("script.json"), authorless, 4);
        QVERIFY2(has_code(missing, "author_missing"), described(missing).c_str());
    }

    // set_layer_mask on the placed art an agent adopted, committed: the mask is kept through saving.
    // (Python: test_quality_report.py test_placed_art_takes_a_mask)
    void test_placed_art_takes_a_mask() {
        const fs::path root = folder(QStringLiteral("mask"));
        m5_project(root);
        if (QTest::currentTestFailed()) return;
        m5_sheets(root);
        if (QTest::currentTestFailed()) return;
        const fs::path book = root / "demo.genko";
        const std::string frame_id = genko::service::load_episode(book).page(0).leaf_frames().at(0)->id;
        std::string candidate;
        m5_art(root, 1, frame_id, candidate);
        if (QTest::currentTestFailed()) return;
        const Document adopted = genko::service::load_episode(book);
        std::string layer_id;
        for (const genko::core::Layer& layer : adopted.page(0).layers) {
            if (layer.frame_id && *layer.frame_id == frame_id && layer.placement_mm) {
                layer_id = layer.id;
                break;
            }
        }
        QVERIFY(!layer_id.empty());
        const Json ops = Json::array({Json::object({{"op", "set_layer_mask"}, {"page", 1}, {"id", layer_id}, {"area", parsed(R"({"rect": [0, 0, 60, 60]})")}})});
        const ToolResult result = StudioService(root, "ai:test").call("apply_ops", Json::array({"demo.genko", ops}), Json::object({{"commit", true}}));
        QVERIFY2(result.ok, result.to_dict().dump().c_str());
        const Document saved = genko::service::load_episode(book);
        const genko::core::Layer* again = nullptr;
        for (const genko::core::Layer& layer : saved.page(0).layers) {
            if (layer.id == layer_id) again = &layer;
        }
        QVERIFY(again != nullptr);
        QVERIFY(again->mask.has_value() && again->mask->png && !again->mask->png->empty());  // (kept through saving)
    }

    // --- tests/test_studio_flow.py -------------------------------------------------------------------------------------

    // The scripted agent's name waits for a person: reviews recorded, nothing to do but wait for the name's approval,
    // the request for it made; an agent cannot approve as a person; once a person approves, the art starts from the
    // character sheets, every panel is briefed, the plans keep their reviews, the tickets are done — and a name the
    // person approved is not replaced by submit_name (name_approved).
    // (Python: test_studio_flow.py test_scripted_agent_builds_a_name_and_waits_for_a_human)
    void test_scripted_agent_builds_a_name_and_waits_for_a_human() {
        const fs::path root = folder(QStringLiteral("flow-approved"));
        run_agent(root);
        if (QTest::currentTestFailed()) return;
        StudioService agent(root, "ai:test");
        const fs::path book = root / "demo.genko";
        QVERIFY(kinds_of(agent.call("next", Json::array({"demo.genko"}), Json::object({{"limit", 10}}))) == std::vector<std::string>(4, "review_name"));
        for (int n = 1; n <= 4; ++n) {
            const ToolResult review = agent.call("record_review", Json::array({"demo.genko", n, 0.8, "読み順よし"}), Json::object());
            QVERIFY2(review.ok, review.to_dict().dump().c_str());
        }
        const ToolResult waiting = agent.call("next", Json::array({"demo.genko"}), Json::object());
        QCOMPARE(dig(waiting.data, {"items"}), Json::array());
        std::string where;
        QVERIFY2(genko::test::same_content(dig(waiting.data, {"waiting_for"}), parsed(R"([{"gate": "name", "pages": [1, 2, 3, 4]}])"), &where),
                 where.c_str());
        const ToolResult asked =
            agent.call("request_approval", Json::array({"demo.genko", "name", Json::array({1, 2, 3, 4}), "確認お願いします"}), Json::object());
        QVERIFY2(asked.ok, asked.to_dict().dump().c_str());
        QVERIFY_THROWS_EXCEPTION(std::exception, (void)HumanService(book, "ai:test"));
        (void)HumanService(book, "human:leaf").approve_name(Json::array({1, 2, 3, 4}));
        const Document approved = genko::service::load_episode(book);
        for (const genko::core::PagePtr& page : approved.pages) QVERIFY(page->name_ok);
        const ToolResult after = agent.call("next", Json::array({"demo.genko"}), Json::object({{"limit", 50}}));
        // (panels on later pages wait for the pilot page (blocked), nothing waits for a person)
        QCOMPARE(dig(after.data, {"waiting_for"}), Json::array());
        // (with the name approved, the art starts from the character sheets: panels with unapproved characters wait)
        bool sheet = false;
        for (const std::string& kind : kinds_of(after)) {
            QVERIFY2(kind == "make_sheet" || kind == "gen_panel", kind.c_str());
            sheet = sheet || kind == "make_sheet";
        }
        QVERIFY(sheet);
        const Document briefed = genko::service::load_episode(book);
        for (const genko::core::PagePtr& page : briefed.pages) {
            for (const genko::core::Frame* frame : page->leaf_frames()) QVERIFY(frame->panel && dig(*frame->panel, {"status"}) == Json("briefed"));
            QVERIFY(page->plan.has_value() && genko::core::py_truthy(dig(*page->plan, {"input_hash"})));
            QCOMPARE(dig(*page->plan, {"reviews", "name", "score"}), Json(0.8));
        }
        QVERIFY(!fs::exists(book / "studio" / "drafts"));  // (M3: everything lives in project.json)
        const ToolResult tickets = agent.call("tickets", Json::array({"demo.genko", "all"}), Json::object());
        QVERIFY2(tickets.ok, tickets.to_dict().dump().c_str());
        for (const Json& ticket : dig(tickets.data, {"tickets"})) QCOMPARE(dig(ticket, {"status"}), Json("done"));
        const ToolResult again = agent.call("submit_name", Json::array({"demo.genko", fixture("p001.json")}),
                                            Json::object({{"commit", true}, {"replace", true}}));
        QVERIFY(!again.ok);
        QVERIFY(!again.issues.empty());
        QCOMPARE(again.issues[0].code, std::string("name_approved"));
    }

    // A person's comment on a page reopens it (revise_page with the comment) until the agent submits it again; then the
    // item and its ticket are gone.
    // (Python: test_studio_flow.py test_human_comment_reopens_a_page_until_it_is_resubmitted)
    void test_human_comment_reopens_a_page_until_it_is_resubmitted() {
        const fs::path root = folder(QStringLiteral("flow-comment"));
        run_agent(root);
        if (QTest::currentTestFailed()) return;
        StudioService agent(root, "ai:test");
        (void)HumanService(root / "demo.genko", "human:leaf").comment(Json(2), Json("2コマ目の台詞を減らして"));
        const ToolResult reopened = agent.call("next", Json::array({"demo.genko"}), Json::object({{"limit", 10}}));
        Json revise;
        for (const Json& item : dig(reopened.data, {"items"})) {
            if (item.value("kind", std::string()) == "revise_page") {
                revise = item;
                break;
            }
        }
        QVERIFY2(!revise.is_null(), reopened.data.dump().c_str());
        QCOMPARE(dig(revise, {"target"}), Json::object({{"page", 2}}));
        QCOMPARE(dig(revise, {"comments"}), Json::array({"2コマ目の台詞を減らして"}));
        const ToolResult resubmitted = agent.call("submit_name", Json::array({"demo.genko", fixture("p002.json")}),
                                                  Json::object({{"commit", true}, {"replace", true}}));
        QVERIFY2(resubmitted.ok, resubmitted.to_dict().dump().c_str());
        const ToolResult later = agent.call("next", Json::array({"demo.genko"}), Json::object({{"limit", 10}}));
        for (const Json& item : dig(later.data, {"items"})) {
            QVERIFY2(!(item.value("kind", std::string()) == "revise_page" && dig(item, {"target", "page"}) == Json(2)), item.dump().c_str());
        }
        const ToolResult remaining = agent.call("tickets", Json::array({"demo.genko"}), Json::object());
        QCOMPARE(dig(remaining.data, {"tickets"}), Json::array());
    }

    // --- tests/test_studio_units.py ------------------------------------------------------------------------------------

    // check_tiers' errors point into the plan: the columns' widths that do not add up (/tiers/1/cols) and a panel whose
    // slot the tiers do not have (/panels/3/slot).
    // (Python: test_studio_units.py test_check_tiers_errors_point_into_the_plan)
    void test_check_tiers_errors_point_into_the_plan() {
        Json plan = fixture("p001.json");
        plan["tiers"][1]["cols"][0]["w"] = 0.8;
        Json extra = plan["panels"][0];
        extra["slot"] = "zz";
        plan["panels"].push_back(extra);
        const std::vector<Issue> issues = layout::check_tiers(plan);
        QVERIFY2(has_issue(issues, "layout_ratio_sum", "/tiers/1/cols"), described(issues).c_str());
        QVERIFY2(has_issue(issues, "panel_unknown_slot", "/panels/3/slot"), described(issues).c_str());
    }

    // A line too long for a panel 8 % of the page high: balloon_overflow at the line's path.
    // (Python: test_studio_units.py test_lettering_reports_overflow_with_the_line_path)
    void test_lettering_reports_overflow_with_the_line_path() {
        Work work{genko::core::new_episode("t", Num(1), 4, PageSpec::b4_comic()), Json::array()};
        Json plan = fixture("p003.json");
        plan["tiers"] = parsed(R"([{"h": 0.08, "cols": [{"slot": "p1", "w": 1.0, "rows": null}]}, {"h": 0.92, "cols": [{"slot": "p2", "w": 1.0, "rows": null}]}])");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(3), plan, "ai:test");
        const std::vector<Issue> issues = letter::place_page(plan, compiled, fixture("bible.json"), Json::object()).second;
        QVERIFY2(has_issue(issues, "balloon_overflow", "/panels/0/lines/0"), described(issues).c_str());
    }

    // The script's lint (a speaker the bible does not have, a page out of range) and the name plan's (a beat of the
    // page not placed, a column too long), each at its path; the demo's own script and plan have no errors.
    // (Python: test_studio_units.py test_script_and_name_lint)
    void test_script_and_name_lint() {
        const Json bible = fixture("bible.json");
        const Json script = fixture("script.json");
        const std::vector<Issue> good = lint::lint_script(script, bible, 4);
        QVERIFY2(errors_of(good).empty(), described(good).c_str());
        Json bad = script;
        bad["scenes"][0]["beats"][4]["speaker_id"] = "nobody";
        bad["scenes"][0]["beats"][0]["page"] = 9;
        const std::vector<Issue> wrong = lint::lint_script(bad, bible, 4);
        QVERIFY2(has_issue(wrong, "unknown_speaker", "/scenes/0/beats/4/speaker_id"), described(wrong).c_str());
        QVERIFY2(has_issue(wrong, "page_out_of_range", "/scenes/0/beats/0/page"), described(wrong).c_str());
        Json plan = fixture("p001.json");
        const std::vector<Issue> fine = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(errors_of(fine).empty(), described(fine).c_str());
        plan["panels"][2]["lines"] = Json::array();
        std::string fifteen;
        for (int i = 0; i < 15; ++i) fifteen += "あ";
        plan["panels"][0]["lines"][0]["breaks"] = Json::array({fifteen});
        const std::vector<Issue> found = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(has_issue(found, "beat_not_placed", "/panels"), described(found).c_str());
        QVERIFY2(has_issue(found, "column_too_long", "/panels/0/lines/0/breaks/0"), described(found).c_str());
    }

    // A reveal on an odd page is warned about (reveal_on_odd_page); on an even page it is not.
    // (Python: test_studio_units.py test_turn_role_warnings_follow_page_parity)
    void test_turn_role_warnings_follow_page_parity() {
        const Json bible = fixture("bible.json");
        const Json script = fixture("script.json");
        Json plan = fixture("p001.json");
        plan["turn_role"] = "reveal";
        const std::vector<Issue> odd = lint::lint_name_plan(plan, script, bible, 4);
        QVERIFY2(has_code(odd, "reveal_on_odd_page"), described(odd).c_str());
        const std::vector<Issue> even = lint::lint_name_plan(fixture("p002.json"), script, bible, 4);
        QVERIFY2(!has_code(even, "reveal_on_odd_page"), described(even).c_str());
    }

    // The tier DSL laid out: columns right to left, tiers top to bottom, a column's rows stacked; the widths as their
    // ratios and the gutters between them (TIER_GUTTER_MM between tiers and rows, COL_GUTTER_MM between columns).
    // (Python: test_studio_units.py test_layout_cols_run_right_to_left_and_tiers_top_to_bottom)
    void test_layout_cols_run_right_to_left_and_tiers_top_to_bottom() {
        Work work{genko::core::new_episode("t", Num(1), 4, PageSpec::b4_comic()), Json::array()};
        const Json plan = parsed(R"({"page": 1, "tiers": [
            {"h": 0.4, "cols": [{"slot": "a", "w": 0.6, "rows": null}, {"slot": "b", "w": 0.4, "rows": null}]},
            {"h": 0.6, "cols": [{"slot": "c", "w": 0.5, "rows": [{"slot": "c1", "h": 0.5}, {"slot": "c2", "h": 0.5}]},
                                {"slot": "d", "w": 0.5, "rows": null}]}
        ], "panels": [{"slot": "a"}, {"slot": "b"}, {"slot": "c1"}, {"slot": "c2"}, {"slot": "d"}]})");
        const layout::CompiledLayout out = layout::apply_layout(work, Json(1), plan, "ai:test");
        QVERIFY(out.reading_order == (std::vector<std::string>{"a", "b", "c1", "c2", "d"}));
        for (const char* slot : {"a", "b", "c1", "c2"}) QVERIFY2(out.rect_of(slot) != nullptr, slot);
        const MmBox a = box_of(out, "a");
        const MmBox b = box_of(out, "b");
        const MmBox c1 = box_of(out, "c1");
        const MmBox c2 = box_of(out, "c2");
        QVERIFY(a[0] > b[0]);  // (a is the right column)
        QVERIFY(a[1] < c1[1]);
        QVERIFY(c1[1] < c2[1] && c1[0] == c2[0]);
        const genko::core::Rect inner = work.doc.page(0).inner_rect_mm();
        QVERIFY(std::abs(a[2] / (a[2] + b[2]) - 0.6) < 0.01);
        QVERIFY(std::abs(a[3] + c1[3] + c2[3] + 2 * layout::kTierGutterMm - inner.height.value()) < 0.01);
        QVERIFY(std::abs((b[0] + b[2] + layout::kColGutterMm) - a[0]) < 0.01);
    }

    // --- tests/test_t_report.py ----------------------------------------------------------------------------------------

    // A name plan's panel with bleed and slant runs out to the bleed along its outer sides only (its shape reaches the
    // bleed's top; the other panel has none), and ink drawn above it in the bleed is printed.
    // (Python: test_t_report.py test_a_slanted_bleed_panel_runs_out_along_its_outer_sides_only)
    void test_a_slanted_bleed_panel_runs_out_along_its_outer_sides_only() {
        Work work{genko::core::new_episode("t", Num(1), 1, PageSpec::b4_comic()), Json::array()};
        (void)layout::apply_layout(work, Json(1),
                                   parsed(R"({"page": 1, "template": "2tier_wide", "panels": [{"slot": "p1", "bleed": true, "slant": 12}, {"slot": "p2"}]})"),
                                   "ai:agent");
        Document doc = work.doc;
        const genko::core::Rect bleed = doc.page(0).bleed_rect_mm();
        {
            const genko::core::Page& page = doc.page(0);
            const std::vector<const genko::core::Frame*> leaves = page.leaf_frames();
            QCOMPARE(leaves.size(), std::size_t{2});
            const auto shape = genko::core::bleed_poly(page, leaves[0]);
            QVERIFY(shape.has_value() && !shape->empty());
            double top = shape->front().y;
            for (const genko::core::PointF& point : *shape) top = std::min(top, point.y);
            QVERIFY2(std::abs(top - bleed.y.value()) <= 1e-6 * std::max(1.0, std::abs(bleed.y.value())), std::to_string(top).c_str());
            QVERIFY(!genko::core::bleed_poly(page, leaves[1]).has_value());
        }
        doc.edit_page(0).name_ok = true;
        const double bx = bleed.x.value(), by = bleed.y.value(), bw = bleed.width.value();
        const Json stroke = Json::array({Json::object({{"op", "add_stroke"},
                                                       {"page", 1},
                                                       {"layer", "ink"},
                                                       {"width_mm", 4},
                                                       {"points", Json::array({Json::array({bx + 2, by + 2}), Json::array({bx + bw - 2, by + 2})})}})});
        const Document inked = genko::service::apply_ops(doc, stroke, "genko").doc;
        const genko::core::Rect inner = inked.page(0).inner_rect_mm();
        genko::render::RenderOptions options;
        options.mode = "print";
        const genko::render::Image grey = genko::render::render_page(inked.page(0), 100, options, &inked).image.convert("L");
        const std::vector<double> pixel =
            grey.getpixel(genko::render::mm_to_px(inner.x.value() + inner.width.value() / 2, 100), genko::render::mm_to_px(by + 2, 100));
        QVERIFY(!pixel.empty());
        QVERIFY2(pixel[0] < 80, std::to_string(pixel[0]).c_str());  // (the ink reaches into the bleed above the panel)
    }

    // Under strict_gates, before the name is approved, an agent cannot draw on a printed pen layer, but can on one set
    // not to print (a trial); making it printed again waits for the name's approval.
    // (Python: test_t_report.py test_a_trial_layer_can_be_drawn_before_the_name_is_approved)
    void test_a_trial_layer_can_be_drawn_before_the_name_is_approved() {
        Document doc = genko::core::new_episode("t", Num(1), 1, PageSpec::b4_comic());
        doc.strict_gates = true;
        doc = genko::service::apply_ops(doc, parsed(R"([{"op": "add_layer", "page": 1, "kind": "pen", "id": "try", "name": "はみ出し"}])"), "ai:hermes").doc;
        const Json stroke = parsed(R"({"op": "add_stroke", "page": 1, "layer_id": "try", "points": [[20, 20], [60, 60]]})");
        const std::string printed = apply_error(doc, Json::array({stroke}), "ai:hermes");
        QVERIFY2(!printed.starts_with("<other") && has_text(printed, "name_ok"), printed.c_str());
        doc = genko::service::apply_ops(doc, Json::array({parsed(R"({"op": "set_layer", "page": 1, "id": "try", "exportable": false})"), stroke}), "ai:hermes")
                  .doc;
        const genko::core::Layer* layer = nullptr;
        for (const genko::core::Layer& item : doc.page(0).layers) {
            if (item.id == "try") layer = &item;
        }
        QVERIFY(layer != nullptr && layer->stroke_count() > 0);
        const Json exportable = parsed(R"([{"op": "set_layer", "page": 1, "id": "try", "exportable": true}])");
        const std::string early = apply_error(doc, exportable, "ai:hermes");  // (drawn lines do not become printed before the approval)
        QVERIFY2(!early.starts_with("<other") && has_text(early, "name_ok"), early.c_str());
        doc.edit_page(0).name_ok = true;
        const std::string approved = apply_error(doc, exportable, "ai:hermes");
        QVERIFY2(approved.empty(), approved.c_str());
    }

    // The plan sets the gaps between tiers and columns (tier_gap_mm, col_gap_mm), which the schema knows; a negative gap
    // is refused (LayoutError).
    // (Python: test_t_report.py test_the_plan_sets_the_gaps_between_panels)
    void test_the_plan_sets_the_gaps_between_panels() {
        Work work{genko::core::new_episode("t", Num(1), 1, PageSpec::webtoon()), Json::array()};
        const Json plan = parsed(R"({"page": 1, "template": null, "tier_gap_mm": 64, "col_gap_mm": 5,
            "tiers": [{"h": 0.5, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.5, "cols": [{"slot": "p2", "w": 0.5}, {"slot": "p3", "w": 0.5}]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}, {"slot": "p3"}]})");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:agent");
        for (const char* slot : {"p1", "p2", "p3"}) QVERIFY2(compiled.rect_of(slot) != nullptr, slot);
        const MmBox p1 = box_of(compiled, "p1");
        const MmBox p2 = box_of(compiled, "p2");
        const MmBox p3 = box_of(compiled, "p3");
        QVERIFY2(std::abs(p2[1] - (p1[1] + p1[3]) - 64) <= 0.01, std::to_string(p2[1] - (p1[1] + p1[3])).c_str());
        QVERIFY2(std::abs(p2[0] - (p3[0] + p3[2]) - 5) <= 0.01, std::to_string(p2[0] - (p3[0] + p3[2])).c_str());  // (read right to left)
        for (const Issue& issue : genko::studio::validate(plan, genko::studio::schema("name_plan@1"))) {
            QVERIFY2(!has_text(issue.path, "gap"), issue.path.c_str());  // (known to the schema)
        }
        Work fresh{genko::core::new_episode("t", Num(1), 1, PageSpec::webtoon()), Json::array()};
        Json negative = plan;
        negative["tier_gap_mm"] = -1;
        QVERIFY_THROWS_EXCEPTION(layout::LayoutError, (void)layout::apply_layout(fresh, Json(1), negative, "ai:agent"));
    }

    // Three unknown effect words: three fx_unknown warnings, the known words listed in the first one's hint only; and a
    // tool's reply lists its errors before its warnings.
    // (Python: test_t_report.py test_the_known_effect_words_are_listed_once_and_errors_come_first)
    void test_the_known_effect_words_are_listed_once_and_errors_come_first() {
        const Json plan = parsed(R"({"page": 1, "template": "splash", "panels": [{"slot": "p1", "fx": ["ぴかぴかビーム", "ぐにゃぐにゃ", "もやもや光線"]}]})");
        std::vector<Issue> unknown;
        for (const Issue& issue : lint::lint_name_plan(plan, parsed(R"({"scenes": []})"), parsed(R"({"characters": []})"), 1)) {
            if (issue.code == "fx_unknown") unknown.push_back(issue);
        }
        QCOMPARE(unknown.size(), std::size_t{3});
        const auto listed = std::count_if(unknown.begin(), unknown.end(), [](const Issue& item) { return has_text(item.hint, "・"); });
        QCOMPARE(static_cast<int>(listed), 1);
        const Json out = ToolResult(false, Json::object(),
                                    {genko::service::warning("fx_unknown", "/a", "w"), genko::service::error("balloon_overflow", "/b", "e")})
                             .to_dict();
        std::vector<std::string> severities;
        for (const Json& issue : dig(out, {"issues"})) severities.push_back(issue.value("severity", std::string()));
        QVERIFY2(severities == (std::vector<std::string>{"error", "warning"}), out.dump().c_str());
    }

    // A tall (webtoon) strip has no last-panel rule: its small last panel is finale_small only for a page. (Python's
    // _size_hints(plan, 1, 1, tall=…) is reached through lint_name_plan's tall.)
    // (Python: test_t_report.py test_a_tall_strip_has_no_last_panel_rule)
    void test_a_tall_strip_has_no_last_panel_rule() {
        const Json plan = parsed(R"({"page": 1, "tiers": [{"h": 0.8, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.2, "cols": [{"slot": "p2", "w": 1}]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}]})");
        const Json script = parsed(R"({"scenes": []})");
        const Json bible = parsed(R"({"characters": []})");
        const std::vector<Issue> page = lint::lint_name_plan(plan, script, bible, 1);
        QVERIFY2(has_code(page, "finale_small"), described(page).c_str());
        const std::vector<Issue> strip = lint::lint_name_plan(plan, script, bible, 1, true);
        QVERIFY2(!has_code(strip, "finale_small"), described(strip).c_str());
    }

    // A sound effect lettered over a face is the drawing's, not a balloon covering a face (no balloon_covers_face).
    // (Python: test_t_report.py test_a_sound_effect_over_a_face_is_not_a_line_over_a_face)
    void test_a_sound_effect_over_a_face_is_not_a_line_over_a_face() {
        Work work{genko::core::new_episode("t", Num(1), 1, PageSpec::b4_comic()), Json::array()};
        const Json plan = parsed(R"({"page": 1, "template": "splash", "panels": [{
            "slot": "p1", "shot": "CU", "characters": [{"id": "hina", "pos": "center", "scale": 1.6}],
            "lines": [{"beat_id": "b1", "balloon": "sfx", "breaks": ["ぱしっ"]}], "sfx_at": [0.5, 0.3]}]})");
        const layout::CompiledLayout compiled = layout::apply_layout(work, Json(1), plan, "ai:agent");
        const std::vector<Issue> issues =
            letter::place_page(plan, compiled, parsed(R"({"characters": [{"id": "hina", "name": "ひな"}]})"), parsed(R"({"b1": null})")).second;
        QVERIFY2(!has_code(issues, "balloon_covers_face"), described(issues).c_str());
    }

    // --- tests/test_tibo_report.py -------------------------------------------------------------------------------------

    // The finish only suggests moving a balloon nearer another face than its speaker's (suggest_move, no move_line);
    // a balloon touching only the top of the head box (the hair) does not cover the face; over the eyes and mouth it
    // does (moved, or face_covered).
    // (Python: test_tibo_report.py test_a_balloon_nearer_someone_else_is_only_suggested_and_hair_is_not_the_face)
    void test_a_balloon_nearer_someone_else_is_only_suggested_and_hair_is_not_the_face() {
        {
            const auto [crowd, line_id] = tibo_lettered({100, 40, 12, 30}, parsed(R"([["tibo", [40, 150, 24, 28]], ["crowd", [110, 80, 24, 28]]])"));
            const auto [ops, notes] = finish::plan(crowd, crowd.page(0));
            for (const Json& op : ops) QVERIFY2(!(op.value("op", std::string()) == "move_line" && op.contains("x_mm")), op.dump().c_str());
            bool suggested = false;
            for (const Json& note : notes) suggested = suggested || (note.value("kind", std::string()) == "suggest_move" && dig(note, {"line_id"}) == Json(line_id));
            QVERIFY2(suggested, notes.dump().c_str());
        }
        {
            // (touching the top of the head box, the hair, is not covering the face)
            const auto [hair, hair_line] = tibo_lettered({70, 40, 12, 22}, parsed(R"([["tibo", [65, 58, 24, 28]]])"));
            (void)hair_line;
            const Json notes = finish::plan(hair, hair.page(0)).second;
            for (const Json& note : notes) {
                const std::string kind = note.value("kind", std::string());
                QVERIFY2(kind != "move_line" && kind != "face_covered", notes.dump().c_str());
            }
        }
        {
            // (over the eyes and mouth it is)
            const auto [face, face_line] = tibo_lettered({70, 70, 12, 22}, parsed(R"([["tibo", [65, 58, 24, 28]]])"));
            (void)face_line;
            const Json notes = finish::plan(face, face.page(0)).second;
            bool covered = false;
            for (const Json& note : notes) {
                const std::string kind = note.value("kind", std::string());
                covered = covered || kind == "move_line" || kind == "face_covered";
            }
            QVERIFY2(covered, notes.dump().c_str());
        }
    }

    // Each mark of the finish (汗, はてな) is on a layer of its own, which its note names (with delete_layer to take it
    // away); deleting one leaves the other.
    // (Python: test_tibo_report.py test_each_mark_has_its_own_layer)
    void test_each_mark_has_its_own_layer() {
        Document doc = tibo_book(1);
        {
            Json& panel = leaf_panel(doc);
            panel["regions"] = parsed(R"([{"kind": "face", "char": "tibo", "rect_mm": [60, 60, 20, 24], "source": "agent"}])");
            panel["fx"] = Json::array({"汗", "はてな"});
        }
        const auto [ops, notes] = finish::plan(doc, doc.page(0));
        std::vector<Json> layers;
        for (const Json& op : ops) {
            if (op.value("op", std::string()) == "add_layer") layers.push_back(dig(op, {"id"}));
        }
        std::vector<Json> marks;
        for (const Json& note : notes) {
            if (note.value("kind", std::string()) == "add_mark") marks.push_back(note);
        }
        QCOMPARE(layers.size(), std::size_t{2});
        QCOMPARE(marks.size(), std::size_t{2});
        QVERIFY(layers[0] != layers[1]);
        for (const Json& mark : marks) {
            QVERIFY2(std::find(layers.begin(), layers.end(), dig(mark, {"layer_id"})) != layers.end(), mark.dump().c_str());
            QVERIFY2(has_text(mark.value("why", std::string()), "delete_layer"), mark.dump().c_str());
        }
        const Document drawn = genko::service::apply_ops(doc, ops, kTiboAgent).doc;
        const Document left =
            genko::service::apply_ops(drawn, Json::array({Json::object({{"op", "delete_layer"}, {"page", 1}, {"id", dig(marks[0], {"layer_id"})}})}), kTiboAgent).doc;
        bool kept = false;
        for (const genko::core::Layer& layer : left.page(0).layers) kept = kept || Json(layer.id) == dig(marks[1], {"layer_id"});
        QVERIFY(kept);
    }

    // A body cut by the panel's edge with its face inside is the usual framing (no cut_by_panel); a face cut by the edge
    // is said (顔).
    // (Python: test_tibo_report.py test_a_body_cut_by_the_edge_with_its_face_inside_is_framing)
    void test_a_body_cut_by_the_edge_with_its_face_inside_is_framing() {
        Document doc = tibo_book(1);
        const genko::core::Rect r = doc.page(0).leaf_frames().at(0)->rect;
        const double x = r.x.value(), y = r.y.value();
        leaf_panel(doc)["regions"] = Json::array({
            Json::object({{"kind", "face"}, {"char", "tibo"}, {"rect_mm", Json::array({x + 30, y + 45, 18, 20})}}),
            Json::object({{"kind", "person"}, {"char", "tibo"}, {"rect_mm", Json::array({x + 20, y + 40, 40, r.height.value()})}}),  // (legs out of the bottom)
        });
        const std::vector<Json> framing = cut_by_panel(doc);
        QVERIFY2(framing.empty(), framing.empty() ? "" : framing[0].value("message", std::string()).c_str());
        leaf_panel(doc)["regions"][0]["rect_mm"] = Json::array({x - 8, y + 45, 18, 20});  // (the face itself cut: said)
        const std::vector<Json> cut = cut_by_panel(doc);
        QVERIFY(!cut.empty());
        QVERIFY2(has_text(cut[0].value("message", std::string()), "顔"), cut[0].dump().c_str());
    }

    // An op's refusal in the service's words says how the op is written: a value of the wrong type (値の型が違います,
    // move_line の書き方 naming its tails) and a key the op does not know (知らない鍵: layer_id, with delete_layer's id: str).
    // (Python: test_tibo_report.py test_a_wrong_key_or_type_says_how_the_op_is_written)
    void test_a_wrong_key_or_type_says_how_the_op_is_written() {
        Document doc = tibo_book(1);
        const std::string id = doc.add_line(Num(1), "あ", "", std::nullopt, "", Num(10), Num(10), Num(10), Num(20)).id;
        const std::string wrong_type =
            apply_error(doc, Json::array({Json::object({{"op", "move_line"}, {"id", id}, {"tail", parsed(R"([{"to": [1, 2]}])")}})}), "genko");
        QVERIFY2(!wrong_type.empty() && !wrong_type.starts_with("<other"), wrong_type.c_str());
        const std::string type_shown = genko::service::wording_error(wrong_type);
        QVERIFY2(has_text(type_shown, "値の型が違います") && has_text(type_shown, "move_line の書き方") && has_text(type_shown, "tails"), type_shown.c_str());
        const std::string wrong_key = apply_error(doc, parsed(R"([{"op": "delete_layer", "page": 1, "layer_id": "x"}])"), "genko");
        QVERIFY2(!wrong_key.empty() && !wrong_key.starts_with("<other"), wrong_key.c_str());
        const std::string key_shown = genko::service::wording_error(wrong_key);
        QVERIFY2(has_text(key_shown, "知らない鍵: layer_id") && has_text(key_shown, "id: str"), key_shown.c_str());
    }
};

QTEST_MAIN(TestLedgerServiceC)
#include "test_ledger_service_c.moc"
