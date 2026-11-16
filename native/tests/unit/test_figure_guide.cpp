// The people's generation guides (SPEC FIGURE-03): a panel with 3D figures gets guides/figures.png (the figures shaded
// on black, a material colour each, in the panel's order) and guides/regions.png (a solid colour each, black where
// nobody is), named first in the request (files.image_order, the first note, request.control) and exactly as large as
// the image asked for (the request's suggested size, or the tool's size when the request names a tool of tools.json);
// the same bytes and the same request id every time; a panel without people keeps its request as before; a panel with
// two people of the brief and no 3D figure gets the regions from the brief's places; the printed page is unchanged.
// GENKO_FIGURE_GUIDE_OUT=<folder> also writes example pairs there (a café of two, runners) for trying them on an image
// tool.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/json.hpp"
#include "core/mesh3d.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "service/book.hpp"
#include "service/figure_guide.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"
#include "test3d.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::service::HumanService;
using genko::service::StudioService;
using genko::service::ToolResult;
namespace fs = std::filesystem;
namespace render = genko::render;
namespace fg = genko::service::figure_guide;

namespace {

constexpr const char* kBook = "demo.genko";

fs::path utf8_path(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }
fs::path utf8_path(const Json& text) { return genko::core::path_from_utf8(text.get<std::string>()); }

Json fixture(const std::string& name) {
    return genko::core::parse_python_json(
        genko::storage::read_file(utf8_path(genko::test::repo_root()) / "native" / "tests" / "fixtures" / "studio" / "demo4" / name));
}

void as_person(const fs::path& book, const Json& ops) {
    genko::service::Session session(book, "human:leaf");
    session.apply(ops, "human:leaf");
    session.save("human:leaf");
}

// The demo book's first page, named and approved; its third panel (p3) has two people in the brief (hina and sora).
std::string make_book(StudioService& agent) {
    if (const ToolResult r = agent.call("create_project", Json::array({kBook, "demo", 4}), Json::object()); !r.ok) return "create_project: " + r.to_dict().dump();
    for (const auto& [tool, file] : std::vector<std::pair<const char*, std::string>>{{"set_bible", "bible.json"}, {"set_script", "script.json"}}) {
        if (const ToolResult r = agent.call(tool, Json::array({kBook, fixture(file)}), Json{{"commit", true}}); !r.ok) return std::string(tool) + ": " + r.to_dict().dump();
    }
    Json name = fixture("p001.json");
    name["panels"][2]["characters"] = Json::array({Json{{"id", "hina"}, {"pose", "座る"}, {"expression", "笑顔"}, {"facing", "right"}, {"pos", "left"}, {"scale", 0.7}},
                                                   Json{{"id", "sora"}, {"pose", "座る"}, {"expression", "驚き"}, {"facing", "left"}, {"pos", "right"}, {"scale", 0.7}}});
    if (const ToolResult r = agent.call("submit_name", Json::array({kBook, name}), Json{{"commit", true}}); !r.ok) return "submit_name: " + r.to_dict().dump();
    const Json approved = HumanService(agent.root() / kBook, "human:leaf").approve_name(Json::array({1}));
    if (!approved.value("ok", false)) return "approve_name: " + approved.dump();
    return {};
}

genko::core::Frame slot(const fs::path& book, const std::string& name) {
    const genko::core::Document doc = genko::service::load_episode(book);
    for (const genko::core::Frame* f : doc.page(0).leaf_frames()) {
        if (f->panel && genko::core::py_get(*f->panel, "slot") == Json(name)) return *f;
    }
    return {};
}

// A figure op in a panel: its pelvis at (fx, fy) of the panel (0..1), `height` of the panel's height tall.
Json figure_op(const genko::core::Frame& f, const std::string& id, double fx, double fy, double height, const std::string& preset, double turn,
               const std::string& sex) {
    const double x = f.rect.x.value() + f.rect.width.value() * fx;
    const double y = f.rect.y.value() + f.rect.height.value() * fy;
    Json op{{"op", "add_figure"}, {"page", 1}, {"id", id}, {"frame_id", f.id}, {"pos", Json::array({x, y, 0})}, {"height_mm", f.rect.height.value() * height},
            {"preset", preset}, {"rot", Json::array({0, turn, 0})}};
    if (!sex.empty()) op["body"] = Json{{"sex", sex}};
    return op;
}

render::Image png_of(const Json& result, const std::string& name) {
    return render::read_png(genko::storage::read_file(utf8_path(result["dir"]) / "guides" / name));
}

std::array<int, 3> rgb_at(const render::Image& image, int x, int y) {
    const std::vector<double> p = image.getpixel(x, y);
    return {static_cast<int>(p.at(0)), static_cast<int>(p.at(1)), static_cast<int>(p.at(2))};
}

bool black(const std::array<int, 3>& c) { return c[0] == 0 && c[1] == 0 && c[2] == 0; }

// The guide's look up close (the pictures a person judges the body and the material by, not a test): one figure an
// image, as guides/figures.png shades it — seated, standing, the torso, the back, the head; look_<name>.png.
void write_looks(const fs::path& to) {
    const auto figure = [](const char* joints, const char* sex, double turn, double tip, const char* hands = "{}") {
        Json prim{{"id", "f"}, {"kind", "figure"}, {"pos", Json::array({0.0, 0.0, 0.0})}, {"size", Json::array({100.0, 200.0, 50.0})},
                  {"rot", Json::array({tip, turn, 0.0})}, {"body", *sex != '\0' ? Json{{"sex", sex}} : Json::object()},
                  {"joints", genko::core::parse_python_json(joints)}, {"hands", genko::core::parse_python_json(hands)}};
        return prim;
    };
    const char* const seated =
        R"({"spine": {"x": 0.1, "z": -0.06}, "l_leg": {"x": 1.25, "z": 0.75}, "l_knee": {"x": -1.25}, "r_leg": {"x": 0.95, "z": -0.35}, "r_knee": {"x": -1.0},
            "l_ankle": {"x": -0.5}, "r_ankle": {"x": -0.5}, "l_arm": {"z": 0.25, "x": -0.15}, "r_arm": {"z": -0.2, "x": -0.1}, "l_elbow": {"x": 0.25}, "r_elbow": {"x": 0.2},
            "head": {"y": 0.15}})";
    const char* const stand = R"({"l_arm": {"z": 0.25}, "r_arm": {"z": -0.25}, "l_leg": {"z": 0.08}, "r_leg": {"z": -0.08}})";
    const char* const reach = R"({"l_arm": {"z": 1.5}, "r_arm": {"z": -2.8}, "l_leg": {"x": 0.5}, "l_knee": {"x": -0.9}, "spine": {"y": 0.25}})";
    struct Look {
        const char* name;
        Json prim;
        fg::Box box;
        std::array<int, 2> px;
    };
    const std::vector<Look> looks{
        {"seated_female", figure(seated, "female", 0.35, 0.0), {-90.0, -140.0, 220.0, 330.0}, {768, 1152}},
        {"seated_male", figure(seated, "male", -0.5, 0.0), {-110.0, -140.0, 220.0, 330.0}, {768, 1152}},
        {"stand_male", figure(stand, "male", 0.5, 0.0), {-75.0, -115.0, 150.0, 225.0}, {768, 1152}},
        {"stand_female", figure(stand, "female", -0.5, 0.0), {-75.0, -115.0, 150.0, 225.0}, {768, 1152}},
        {"stand_neither", figure(stand, "", 0.0, 0.0), {-75.0, -115.0, 150.0, 225.0}, {768, 1152}},
        {"back_male", figure(stand, "male", 2.7, 0.0), {-75.0, -115.0, 150.0, 225.0}, {768, 1152}},
        {"back_female", figure(stand, "female", 3.5, 0.0), {-75.0, -115.0, 150.0, 225.0}, {768, 1152}},
        {"reach_female", figure(reach, "female", 0.4, 0.0, R"({"l": "open", "r": "open"})"), {-90.0, -150.0, 180.0, 270.0}, {768, 1152}},
        {"torso_male", figure(stand, "male", 0.4, 0.0), {-45.0, -112.0, 90.0, 135.0}, {768, 1152}},
        {"torso_female", figure(stand, "female", 0.4, 0.0), {-45.0, -112.0, 90.0, 135.0}, {768, 1152}},
        {"head_male", figure(stand, "male", 0.5, 0.0), {-24.0, -122.0, 48.0, 48.0}, {768, 768}},
        {"head_female", figure(stand, "female", -0.4, 0.0), {-24.0, -122.0, 48.0, 48.0}, {768, 768}},
    };
    const genko::core::Page page;
    for (const Look& look : looks) {
        const std::vector<Json> prims{look.prim};
        const fg::View view = fg::view_of(look.box, look.px, false);
        const std::string png = render::write_png(fg::figures(page, prims, view));
        std::ofstream(to / (std::string("look_") + look.name + ".png"), std::ios::binary) << png;
    }
}

bool any_note(const Json& request, const std::string& words) {
    for (const Json& n : request.value("notes_for_agent", Json::array())) {
        if (n.is_string() && n.get<std::string>().find(words) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

class TestFigureGuide : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = utf8_path(dir_.path() + QLatin1Char('/') + name);
        fs::create_directories(out);
        return out;
    }

    Json ask(StudioService& agent, const std::string& frame_id, const Json& more = Json::object()) {
        Json args{{"page", 1}, {"frame_id", frame_id}};
        for (const auto& [k, v] : more.items()) args[k] = v;
        const ToolResult r = agent.call("generation_request", Json::array({kBook}), args);
        if (!r.ok) qWarning("%s", r.to_dict().dump().c_str());
        return r.data;
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        qputenv("GENKO_CONFIG_DIR", QFile::encodeName(dir_.path() + QStringLiteral("/config")));
        qputenv("GENKO_USER", "leaf");
    }

    // A panel without 3D figures and with one person keeps its request as before: no new files, keys or notes (its
    // JSON and id are those of the build before FIGURE-03: checked by hand, docs/cpp-migration/HANDOFF.md). Figures put
    // in another panel do not change it.
    void aPanelWithoutPeopleKeepsItsRequest() {
        const fs::path root = folder(QStringLiteral("none"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const Json p1 = ask(agent, slot(book, "p1").id);
        const Json p2 = ask(agent, slot(book, "p2").id);
        qInfo("ids before figures: p1 %s, p2 %s", p1["request"]["id"].dump().c_str(), p2["request"]["id"].dump().c_str());
        for (const Json* r : {&p1, &p2}) {
            const Json& request = (*r)["request"];
            QVERIFY(!request.contains("control"));
            for (const char* key : {"figures", "regions", "image_order"}) QVERIFY2(!request["files"].contains(key), key);
            QVERIFY(!any_note(request, "figures.png") && !any_note(request, "regions.png"));
            QVERIFY(!fs::exists(utf8_path((*r)["dir"]) / "guides" / "figures.png"));
        }
        as_person(book, Json::array({figure_op(slot(book, "p3"), "a", 0.3, 0.6, 0.6, "sit", 0.6, "female")}));
        QCOMPARE(ask(agent, slot(book, "p1").id)["request"]["id"], p1["request"]["id"]);
        QCOMPARE(ask(agent, slot(book, "p2").id)["request"]["id"], p2["request"]["id"]);
    }

    // Two figures in a panel: figures.png and regions.png at the suggested size, named first; black around them; each
    // person in their own colour, the first warm beige and the second lavender; the same bytes and id when asked again.
    void figuresAndRegionsOfAPanel() {
        const fs::path root = folder(QStringLiteral("two"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame f = slot(book, "p2");
        as_person(book, Json::array({figure_op(f, "a", 0.3, 0.55, 0.55, "stand", 0.5, "female"), figure_op(f, "b", 0.72, 0.55, 0.55, "walk", -0.6, "male")}));
        const Json result = ask(agent, f.id);
        const Json& request = result["request"];
        QCOMPARE(request["files"]["figures"], Json("guides/figures.png"));
        QCOMPARE(request["files"]["regions"], Json("guides/regions.png"));
        QCOMPARE(request["files"]["image_order"].at(0), Json("guides/figures.png"));
        if (request["files"]["references_sheet"].is_string()) QCOMPARE(request["files"]["image_order"].at(1), request["files"]["references_sheet"]);
        const std::string first = request["notes_for_agent"].at(0).get<std::string>();
        QVERIFY2(first.find("guides/figures.png") != std::string::npos && first.find("lllite_3dpose") != std::string::npos &&
                     first.find("regions.png") != std::string::npos,
                 first.c_str());
        const Json px = request["size"]["suggested_px"];
        QCOMPARE(request["control"]["px"], px);
        QCOMPARE(request["control"]["people"], Json(2));
        const render::Image figures = png_of(result, "figures.png");
        const render::Image regions = png_of(result, "regions.png");
        for (const render::Image* image : {&figures, &regions}) {
            QCOMPARE(image->width(), static_cast<int>(px[0].get<std::int64_t>()));
            QCOMPARE(image->height(), static_cast<int>(px[1].get<std::int64_t>()));
            QCOMPARE(std::string(image->mode()), std::string("RGB"));
            const int w = image->width() - 1;
            const int h = image->height() - 1;
            for (const auto& [x, y] : std::vector<std::pair<int, int>>{{0, 0}, {w, 0}, {0, h}, {w, h}}) QVERIFY(black(rgb_at(*image, x, y)));
        }
        // the regions: the two colours (and black), each a good part of the picture; under each, figures.png in that
        // person's colour (beige: red over blue; lavender: blue over red)
        long counts[2] = {0, 0};
        long sums[2][3] = {{0, 0, 0}, {0, 0, 0}};
        std::set<std::array<int, 3>> seen;
        for (int y = 0; y < regions.height(); y += 2) {
            for (int x = 0; x < regions.width(); x += 2) {
                const std::array<int, 3> r = rgb_at(regions, x, y);
                seen.insert(r);
                for (int i = 0; i < 2; ++i) {
                    if (r != fg::colour(static_cast<std::size_t>(i))) continue;
                    const std::array<int, 3> c = rgb_at(figures, x, y);
                    if (black(c)) continue;
                    ++counts[i];
                    for (int k = 0; k < 3; ++k) sums[i][k] += c[static_cast<std::size_t>(k)];
                }
            }
        }
        QCOMPARE(seen, (std::set<std::array<int, 3>>{{0, 0, 0}, fg::colour(0), fg::colour(1)}));
        QVERIFY2(counts[0] > 2000 && counts[1] > 2000, (std::to_string(counts[0]) + " " + std::to_string(counts[1])).c_str());
        QVERIFY(sums[0][0] > sums[0][2] * 1.2);  // (beige)
        QVERIFY(sums[1][2] > sums[1][0] * 1.1);  // (lavender)
        QVERIFY(fg::colour(0) == (std::array<int, 3>{236, 204, 150}));
        // shaded: many tones (light, shadow and highlight), not flat colours
        std::set<std::array<int, 3>> tones;
        for (int y = 0; y < figures.height(); y += 3) {
            for (int x = 0; x < figures.width(); x += 3) tones.insert(rgb_at(figures, x, y));
        }
        QVERIFY2(tones.size() > 200, std::to_string(tones.size()).c_str());
        // the same request, the same bytes
        const Json again = ask(agent, f.id);
        QCOMPARE(again["request"]["id"], request["id"]);
        for (const char* name : {"figures.png", "regions.png"}) {
            QCOMPARE(genko::storage::read_file(utf8_path(again["dir"]) / "guides" / name), genko::storage::read_file(utf8_path(result["dir"]) / "guides" / name));
        }
        qInfo("figures.png %s, regions.png %s, request %s",
              genko::test::sha256(genko::storage::read_file(utf8_path(result["dir"]) / "guides" / "figures.png")).c_str(),
              genko::test::sha256(genko::storage::read_file(utf8_path(result["dir"]) / "guides" / "regions.png")).c_str(), request["id"].dump().c_str());
        // the pins (SPEC FIGURE-03; docs/cpp-migration/HANDOFF.md records them)
        QCOMPARE(genko::test::sha256(genko::storage::read_file(utf8_path(result["dir"]) / "guides" / "figures.png")), std::string(kFiguresPng));
        QCOMPARE(genko::test::sha256(genko::storage::read_file(utf8_path(result["dir"]) / "guides" / "regions.png")), std::string(kRegionsPng));
        // the render tool's kinds show the same people
        const ToolResult shown = agent.call("render", Json::array({kBook, 1, "name", 512}), Json{{"frame_id", f.id}, {"kind", "guide:figures"}});
        QVERIFY2(shown.ok && !shown.images.empty(), shown.to_dict().dump().c_str());
        const ToolResult areas = agent.call("render", Json::array({kBook, 1, "name", 512}), Json{{"frame_id", f.id}, {"kind", "guide:regions"}});
        QVERIFY2(areas.ok && !areas.images.empty(), areas.to_dict().dump().c_str());
        QCOMPARE(std::string(render::read_png(shown.images.front()).mode()), std::string("RGB"));
    }

    // What a hand holds (SPEC FIGURE-02, `hold`; G5) is drawn in its holder's colour: a sword in the first person's hand
    // adds pixels of the first colour to regions.png and shaded pixels of its hue (warm: red over blue) to figures.png,
    // and none of the second person's; props: false draws the person as without it.
    void propsInTheHoldersColour() {
        genko::core::Page page;  // (no camera: the prims' own; the usual light)
        Json a = Json::object({{"id", "a"}, {"kind", "figure"}, {"pos", Json::array({60.0, 100.0, 0.0})}, {"size", Json::array({45.0, 90.0, 22.5})},
                               {"rot", Json::array({0.0, 0.6, 0.0})}, {"body", Json::object()},
                               {"joints", genko::core::parse_python_json(R"({"r_arm": {"x": 0.9, "z": -0.2}, "r_elbow": {"x": 1.1}})")},
                               {"hands", Json::object({{"r", "grip"}})}, {"hold", Json::object({{"r", "sword"}})}});
        Json b = a;
        b["id"] = "b";
        b["pos"] = Json::array({140.0, 100.0, 0.0});
        b.erase("hold");
        Json bare = a;
        bare["props"] = false;
        const fg::View view = fg::view_of({10.0, 20.0, 180.0, 140.0}, {450, 350}, true);
        const std::vector<Json> held{a, b};
        const std::vector<Json> plain{bare, b};
        const render::Image with = fg::regions(page, held, view, 0);
        const render::Image without = fg::regions(page, plain, view, 0);
        const render::Image shaded = fg::figures(page, held, view);
        const render::Image shaded_without = fg::figures(page, plain, view);
        std::size_t added = 0;
        std::size_t warm = 0;
        for (int y = 0; y < 350; ++y) {
            for (int x = 0; x < 450; ++x) {
                const std::array<int, 3> w = rgb_at(with, x, y);
                const std::array<int, 3> o = rgb_at(without, x, y);
                if (w == o) continue;
                QVERIFY2(w == fg::colour(0) && black(o), (std::to_string(x) + "," + std::to_string(y)).c_str());
                ++added;
                const std::array<int, 3> s = rgb_at(shaded, x, y);
                QVERIFY(!black(s));
                if (s[0] > s[2]) ++warm;
            }
        }
        QVERIFY2(added > 300, std::to_string(added).c_str());
        QVERIFY2(warm * 10 >= added * 9, (std::to_string(warm) + " of " + std::to_string(added)).c_str());
        QVERIFY(shaded.tobytes() != shaded_without.tobytes());
        // props: false is the person as without hold
        Json none = a;
        none.erase("hold");
        QVERIFY(fg::figures(page, std::vector<Json>{none, b}, view).tobytes() == shaded_without.tobytes());
    }


    // Drawn on one thread or four at once: the same bytes.
    void theSameBytesOnEveryThread() {
        const fs::path root = folder(QStringLiteral("threads"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame f = slot(book, "p1");
        as_person(book, Json::array({figure_op(f, "a", 0.5, 0.6, 0.7, "run", 0.4, "")}));
        const genko::core::Document doc = genko::service::load_episode(book);
        const genko::core::Page& page = doc.page(0);
        const genko::core::Frame* frame = nullptr;
        for (const genko::core::Frame* g : page.leaf_frames()) {
            if (g->id == f.id) frame = g;
        }
        QVERIFY(frame != nullptr);
        const std::vector<Json> people = fg::panel_prims(page, *frame);
        QCOMPARE(people.size(), std::size_t{1});
        const fg::View view = fg::view_of({f.rect.x.value() - 3, f.rect.y.value() - 3, f.rect.width.value() + 6, f.rect.height.value() + 6}, {512, 320}, true);
        genko::core::mesh3d::clear_caches();
        const std::string alone = fg::figures(page, people, view).tobytes() + fg::regions(page, people, view).tobytes();
        genko::core::mesh3d::clear_caches();
        std::vector<std::string> threads(4);
        std::vector<std::thread> pool;
        for (std::size_t t = 0; t < threads.size(); ++t) {
            pool.emplace_back([&, t] { threads[t] = fg::figures(page, people, view).tobytes() + fg::regions(page, people, view).tobytes(); });
        }
        for (std::thread& t : pool) t.join();
        for (const std::string& d : threads) QVERIFY(d == alone);
    }

    // A tool of tools.json (a PS5 AI Anima profile: 1024 at most a side, on a 64 px grid): the guides are the tool's
    // size nearest the panel's shape, the panel in their middle; never more than 1024 a side.
    void theToolsSize() {
        const fs::path root = folder(QStringLiteral("tool"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        genko::studio::set_tool(genko::core::poses::config_dir(), "ps5:anima", ps5_profile());
        const fs::path book = root / kBook;
        for (const char* name : {"p1", "p2"}) {
            const genko::core::Frame f = slot(book, name);
            as_person(book, Json::array({figure_op(f, std::string("f") + name, 0.5, 0.6, 0.6, "point", 0.3, "")}));
            const Json result = ask(agent, f.id, Json{{"tool", "ps5:anima"}});
            const Json& request = result["request"];
            const Json want = request["size"]["tool_sizes"].at(0)["px"];
            QCOMPARE(request["control"]["px"], want);
            const render::Image figures = png_of(result, "figures.png");
            QCOMPARE(figures.width(), static_cast<int>(want[0].get<std::int64_t>()));
            QCOMPARE(figures.height(), static_cast<int>(want[1].get<std::int64_t>()));
            QVERIFY(figures.width() <= 1024 && figures.height() <= 1024 && figures.width() % 64 == 0 && figures.height() % 64 == 0);
            QCOMPARE(png_of(result, "regions.png").width(), figures.width());
            QVERIFY(any_note(request, std::to_string(figures.width()) + "×" + std::to_string(figures.height())));
        }
    }

    // Two people in the brief and no 3D figure: regions.png from the brief's places (no figures.png); one person in the
    // brief: nothing new.
    void theBriefsPlacesWithoutFigures() {
        const fs::path root = folder(QStringLiteral("cast"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const Json result = ask(agent, slot(book, "p3").id);
        const Json& request = result["request"];
        QCOMPARE(request["files"]["regions"], Json("guides/regions.png"));
        QVERIFY(!request["files"].contains("figures") && !request["files"].contains("image_order"));
        QVERIFY(request["control"]["pose"].is_null());
        QCOMPARE(request["control"]["people"], Json(2));
        const render::Image regions = png_of(result, "regions.png");
        std::set<std::array<int, 3>> seen;
        for (int y = 0; y < regions.height(); y += 4) {
            for (int x = 0; x < regions.width(); x += 4) seen.insert(rgb_at(regions, x, y));
        }
        QVERIFY(seen.count(fg::colour(0)) == 1 && seen.count(fg::colour(1)) == 1 && seen.count(std::array<int, 3>{0, 0, 0}) == 1);
        QVERIFY(any_note(request, "regions.png"));
    }

    // Printing never shows the figures, with or without the guides asked for.
    void thePrintIsUnchanged() {
        const fs::path root = folder(QStringLiteral("print"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const auto print = [&] {
            const genko::core::Document doc = genko::service::load_episode(book);
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = "print";
            return render::render_page(doc.page(0), 72, options, &doc).image.tobytes();
        };
        const std::string before = print();
        const genko::core::Frame f = slot(book, "p3");
        as_person(book, Json::array({figure_op(f, "a", 0.3, 0.6, 0.6, "sit", 0.6, "female"), figure_op(f, "b", 0.7, 0.6, 0.6, "sit", -0.6, "male")}));
        (void)ask(agent, f.id);
        QVERIFY(print() == before);
    }

    // (not a test of its own: example pairs for trying on an image tool, written when GENKO_FIGURE_GUIDE_OUT names a
    // folder: a café of two at the PS5 profile's size, runners, a close two-shot)
    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FIGURE_GUIDE_OUT");
        if (out.isEmpty()) return;
        const fs::path to = genko::core::path_from_utf8(out.toStdString());
        fs::create_directories(to);
        write_looks(to);
        if (qEnvironmentVariableIsSet("GENKO_FIGURE_GUIDE_LOOKS_ONLY")) return;
        // (the book of the first test, its panels p1 and p2 without people: to ask with another build)
        fs::remove_all(to / "book_none");
        fs::copy(folder(QStringLiteral("none")), to / "book_none", fs::copy_options::recursive);
        const fs::path root = folder(QStringLiteral("examples"));
        StudioService agent(root, "ai:test");
        const std::string made = make_book(agent);
        QVERIFY2(made.empty(), made.c_str());
        genko::studio::set_tool(genko::core::poses::config_dir(), "ps5:anima", ps5_profile());
        const fs::path book = root / kBook;
        const genko::core::Frame cafe = slot(book, "p3");
        const genko::core::Frame run = slot(book, "p1");
        const genko::core::Frame two = slot(book, "p2");
        as_person(book, Json::array({figure_op(cafe, "c1", 0.2, 0.62, 0.55, "sit", 0.8, "female"), figure_op(cafe, "c2", 0.8, 0.62, 0.55, "sit", -0.8, "male"),
                                     figure_op(run, "r1", 0.33, 0.5, 0.88, "run", 0.9, "male"), figure_op(run, "r2", 0.68, 0.53, 0.82, "run", 1.2, "female"),
                                     figure_op(two, "t1", 0.3, 0.75, 1.1, "point", 0.4, "female"), figure_op(two, "t2", 0.72, 0.75, 1.1, "peace", -0.5, "male")}));
        for (const auto& [frame, name] : std::vector<std::pair<genko::core::Frame, std::string>>{{cafe, "cafe"}, {run, "running"}, {two, "two_shot"}}) {
            for (const bool tool : {true, false}) {
                const Json result = ask(agent, frame.id, tool ? Json{{"tool", "ps5:anima"}} : Json::object());
                const std::string stem = name + (tool ? "_ps5" : "_suggested");
                for (const char* file : {"figures.png", "regions.png"}) {
                    const std::string bytes = genko::storage::read_file(utf8_path(result["dir"]) / "guides" / file);
                    std::ofstream(to / (stem + "_" + file), std::ios::binary) << bytes;
                }
                std::ofstream(to / (stem + "_request.json"), std::ios::binary) << result["request"].dump(2);
            }
        }
    }

private:
    static Json ps5_profile() {
        Json sizes = Json::array();
        for (const auto& [w, h] : std::vector<std::pair<int, int>>{{1024, 1024}, {896, 1024}, {1024, 896}, {832, 1024}, {1024, 832}, {768, 1024}, {1024, 768},
                                                                   {704, 1024}, {1024, 704}, {640, 1024}, {1024, 640}, {576, 1024}, {1024, 576}, {512, 1024}, {1024, 512}}) {
            sizes.push_back(Json::array({w, h}));
        }
        return Json{{"label", "PS5 AI Anima（3D ポーズ・領域の制御。1 辺 1024 まで、64 px 刻み）"}, {"sizes_px", sizes},
                    {"supports", Json{{"references", false}, {"edit", true}, {"mask", true}, {"seed", true}}},
                    {"notes", "制御画像（guides/figures.png・regions.png）は出力と同じ大きさ。CFG 1"}};
    }

    static constexpr const char* kFiguresPng = "fbefdbc9057f358c799de2cafd861e1f29b91d5984062fe3cedc7933b5508c29";
    static constexpr const char* kRegionsPng = "c894a1af3e44688fd78461e039534ff1f25e427cb6a069c296e2c54e78cfb696";
};

QTEST_GUILESS_MAIN(TestFigureGuide)
#include "test_figure_guide.moc"
