// The figure's pose library (SPEC FIGURE-02, genko.poses@1, this build's own): the file as embedded and well formed, the
// categories as many as promised; every pose (and its mirror) within the joints' ranges, resting on what it says it
// rests on and balanced, no limb through the body, the mirror exact, drawn within the budget; the nine presets as
// before and passing the same checks; the thumbnails the same bytes on every run and thread; the library's filters;
// the ops' pose_id, mirror, keep, spread, thumb and hold; figures@1 named only while a book uses them (a book without
// them saved as before, byte for byte), and a book with them saved and read back.
// GENKO_POSE_SHEETS=<folder> also writes the contact sheets there (one per category and page) and checks.json.

#include <QtTest>

#include <QCryptographicHash>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/mesh3d.hpp"
#include "core/pose_checks.hpp"
#include "core/pose_library.hpp"
#include "core/model.hpp"
#include "core/ops_schema.hpp"
#include "render/figure_thumbs.hpp"
#include "render/ops_registry.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "pose_sheets.hpp"
#include "testsupport.hpp"

namespace {

using genko::core::Actor;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
namespace library = genko::core::pose_library;
namespace checks = genko::core::pose_checks;
namespace mesh3d = genko::core::mesh3d;

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(genko::render::ops_registry());
    return b;
}

Json ops(const std::string& text) { return genko::core::parse_python_json(text); }

std::string sha256_of(std::string_view bytes) {
    return QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}

// A book whose ids are the same each time, with figures, a head and a hand made by the ops of before (no pose_id,
// hold, spread or thumb).
Document book_of_before() {
    Document doc = genko::core::new_episode("figures", Num(1), 2, genko::core::PageSpec::b5_doujin());
    doc.book_id = "0123456789abcdef0123456789abcdef";
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        genko::core::Page& page = doc.edit_page(i);
        page.id = "pg" + std::to_string(i + 1);
        for (std::size_t k = 0; k < page.layers.size(); ++k) page.layers[k].id = "ly" + std::to_string(i + 1) + "_" + std::to_string(k);
        for (std::size_t k = 0; k < page.frames.size(); ++k) page.frames[k].id = "fr" + std::to_string(i + 1) + "_" + std::to_string(k);
    }
    return bus()
        .apply(doc, ops(R"([
            {"op": "add_figure", "page": 1, "id": "a", "pos": [60, 120, 0], "preset": "walk", "body": {"sex": "female"}},
            {"op": "add_figure", "page": 1, "id": "b", "pos": [120, 120, 0], "height_mm": 110,
             "joints": {"l_arm": {"z": 1.2}}, "hands": {"l": {"pose": "grip", "curls": [0.2, 0.5, 0.5, 0.6, 0.7]}, "r": "fist"}},
            {"op": "pose_figure", "page": 1, "id": "b", "preset": "think", "joints": {"head": {"y": 0.2}}},
            {"op": "add_head", "page": 1, "id": "h", "pos": [140, 60, 0]},
            {"op": "add_hand", "page": 2, "id": "k", "pos": [120, 90, 0], "side": "l", "pose": "peace"},
            {"op": "pose_figure", "page": 2, "id": "k", "curls": [0.1, 0, 0, 1, 1]}])"),
               Actor("genko"))
        .doc;
}

// The book as it is saved (project.json's text), but for the version of the build that wrote it.
std::string saved_text(const Document& doc) {
    QTemporaryDir dir;
    genko::storage::AssetStore store(std::filesystem::path(dir.path().toStdString()));
    Json payload = genko::storage::project_payload_v4(doc, store);
    payload["writer"]["version"] = "pinned";
    return genko::core::dump_python_indent2(payload);
}

// The least each category holds (mirrors do not count): batch A (G3), then batch B (G4: plan §8 asked pinup 85,
// relax_bed 36, lie 25 — the lying poses this body could not rest properly were taken out, docs/POSES.md).
const std::pair<const char*, std::size_t> kAtLeast[] = {{"stand", 40},      {"fashion", 25},   {"walk", 10},        {"run", 10},
                                                         {"sit_chair", 30},  {"sit_floor", 30}, {"kneel_squat", 25}, {"everyday", 25},
                                                         {"pinup", 78},      {"relax_bed", 33}, {"lie", 20},         {"upper_body", 35},
                                                         {"turn", 10},       {"point", 8},      {"emotion", 25},     {"talk", 13},
                                                         {"hands_props", 24},
                                                         // (batch C, G5: the numbers delivered; plan §8 asked sports_action 35, action
                                                         // 30, dance 20, heroic 20, work_life 25, couple 30, two_people 30)
                                                         {"sports_action", 38}, {"action", 30},     {"dance", 20},  {"heroic", 20},
                                                         {"work_life", 26},     {"couple", 30},     {"two_people", 30}};

std::string issues_text(const std::vector<checks::Issue>& issues) {
    std::string out;
    for (const auto& i : issues) out += "\n  " + i.check + ": " + i.what;
    return out;
}

// The sha256 of four thumbnails (60 × 90 at three quarters: stand's first, fashion's last mirrored, sit_chair's first,
// kneel_squat's last mirrored), taken with this build (again after the body's anatomy pass of 2026-11-16).
const char* const kThumbPins =
    "4d463fcbff7c22fad25885d9af7c6cceae54263675fe67cda69926650dee2cb2 3b4f7e7859c5e6be95193c358c0775d09ff055d159229f9a71a2381311346d01 "
    "67f70778a732c736c6eaa1708064caa0332e17cca5efb13c69b81d3887bfec88 c88d1a2cdcd3a095e6b2b0d8267c153b86e15cfa81504558e7b747269c4152cd ";

std::string sheets_dir() { return qEnvironmentVariable("GENKO_POSE_SHEETS").toStdString(); }

// The gallery's pictures (app/pose_gallery: 120 × 180)
constexpr int PoseGalleryWidth = 120;
constexpr int PoseGalleryHeight = 180;

}  // namespace

class TestFigurePoses : public QObject {
    Q_OBJECT

    Json measured_ = Json::array();

private slots:
    // The file is data/figure_poses.json byte for byte, genko.poses@1, well formed (core::pose_checks::check_file), and
    // each category of batches A and B has its poses; the hands of hands_props hold something.
    void theFileIsWellFormed() {
        const std::string on_disk = genko::test::read_bytes(genko::test::repo_root() + QStringLiteral("/native/src/core/data/figure_poses.json"));
        QVERIFY(library::text() == on_disk);
        const Json& file = library::library();
        QCOMPARE(file["format"], Json("genko.poses@1"));
        const auto issues = checks::check_file(file);
        QVERIFY2(issues.empty(), issues_text(issues).c_str());
        std::map<std::string, std::size_t> counts;
        for (const auto& c : library::categories()) counts[c.id] = c.count;
        std::size_t total = 0;
        for (const auto& [category, least] : kAtLeast) {
            QVERIFY2(counts[category] >= least, (std::string(category) + ": " + std::to_string(counts[category])).c_str());
            total += counts[category];
        }
        QCOMPARE(total, file["poses"].size());
        QVERIFY(total >= 650);
        library::Filter held;
        held.category = "hands_props";
        for (const Json* p : library::list(held)) QVERIFY2(p->contains("hold"), (*p)["id"].get<std::string>().c_str());
    }

    // A body lying on the floor names the face it lies on (back, front or side), and that is the face toward the floor;
    // its torso on the floor without one is refused; off the floor, back is a lean on a wall as before.
    void lyingNamesItsFace() {
        // (a pose lying on its back whose ribcage is right on the floor)
        Json back;
        library::Filter lying;
        lying.category = "lie";
        lying.contact = {"back"};
        for (const Json* p : library::list(lying)) {
            if (back.is_null() && checks::check_pose(*p).measured.gap.at("torso") < checks::kUnlistedGap) back = *p;
        }
        QVERIFY(back.is_object());
        QVERIFY(std::find(back["contact"].begin(), back["contact"].end(), Json("back")) != back["contact"].end());
        QVERIFY2(checks::check_pose(back).ok(), issues_text(checks::check_pose(back).issues).c_str());
        QCOMPARE(checks::check_pose(back).measured.down, std::string("back"));
        Json wrong = back;
        for (Json& c : wrong["contact"])
            if (c == Json("back")) c = "front";
        const auto said = checks::check_pose(wrong).issues;
        QVERIFY(std::any_of(said.begin(), said.end(), [](const checks::Issue& i) { return i.what == "the body lies on its back, not its front"; }));
        Json none = back;
        Json kept = Json::array();
        for (const Json& c : back["contact"])
            if (c != Json("back")) kept.push_back(c);
        none["contact"] = kept;
        const auto unnamed = checks::check_pose(none).issues;
        QVERIFY(std::any_of(unnamed.begin(), unnamed.end(), [](const checks::Issue& i) { return i.what.find("the torso touches the floor") == 0; }));
        QCOMPARE(checks::check_pose(*library::find("relax_bed/front_sprawl")).measured.down, std::string("front"));
        // a lean on a wall: back off the floor, no balance asked
        const checks::Report wall = checks::check_pose(*library::find("stand/wall_lean_back"));
        QVERIFY(wall.ok());
        QVERIFY(wall.measured.gap.at("torso") > checks::kContactGap);
    }

    // The hip's twist is its rotation about the femur in the joint's own frame (flexion, abduction, rotation): a thigh
    // flexed and turned reads its turn exactly, the right as the left's mirror; the cross-legged sits keep the knees low.
    void hipTwistIsTheHipsOwn() {
        for (const double x : {0.0, 0.6, 1.2, 1.9}) {
            for (const double y : {-0.7, 0.0, 0.4, 0.9}) {
                const auto l = checks::ball_of("l_leg", Json::object({{"x", x}, {"y", y}}));
                const auto r = checks::ball_of("r_leg", Json::object({{"x", x}, {"y", -y}}));
                QVERIFY2(std::fabs(l.twist - y) < 1e-9, (std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(l.twist)).c_str());
                QVERIFY(std::fabs(r.twist - y) < 1e-9);
            }
        }
        for (const char* id : {"sit_floor/agura", "sit_floor/agura_chin", "sit_floor/meditation", "sit_floor/agura_book"}) {
            const Json pose = *library::find(id);
            const Json prim = checks::figure_of(pose);
            const mesh3d::Skeleton sk = mesh3d::figure_skeleton(prim);
            const checks::Report r = checks::check_pose(pose);
            QVERIFY2(r.ok(), (std::string(id) + issues_text(r.issues)).c_str());
            for (const char* side : {"l", "r"}) {
                const double knee = (r.measured.floor - sk.point(std::string(side) + "_knee")[1]) / sk.unit;  // (the knee's joint above the floor)
                QVERIFY2(knee < 1.1, (std::string(id) + " " + side + " " + std::to_string(knee)).c_str());  // (G3's: about 1.9)
                QVERIFY(checks::ball_of(std::string(side) + "_leg", pose["joints"][std::string(side) + "_leg"]).twist > 0.5);
            }
        }
    }

    // What the figure_poses tool and the gallery use: a pose's summary, the hold filter, words that each must match, a
    // random pick that is the same for the same seed (and every pose once), the seed's text.
    void picksAndSummaries() {
        const Json s = library::summary(*library::find("hands_props/cup_offer"));
        for (const char* key : {"id", "ja", "en", "category", "tags", "hands", "contact", "support", "props", "symmetric"}) QVERIFY2(s.contains(key), key);
        QCOMPARE(s["props"], Json::array({"cup"}));
        QVERIFY(s["hands"]["r"].is_string());
        library::Filter f;
        f.props = true;
        const auto held = library::list(f);
        f.props = false;
        const auto empty = library::list(f);
        QCOMPARE(held.size() + empty.size(), library::list().size());
        const auto holds = [](const Json& p) {  // (a pair: one of its people)
            if (!library::is_pair(p)) return p.contains("hold");
            return p["figures"][0].contains("hold") || p["figures"][1].contains("hold");
        };
        for (const Json* p : held) QVERIFY(holds(*p));
        for (const Json* p : empty) QVERIFY(!holds(*p));
        f = {};
        f.query = "phone";
        const std::size_t phones = library::list(f).size();
        f.query = "phone\xe3\x80\x80lying";  // (a 全角 space between)
        const auto both = library::list(f);
        QVERIFY(!both.empty() && both.size() < phones);
        const auto all = library::list();
        const auto a = library::sample(all, 30, 0xa5);
        QCOMPARE(a.size(), std::size_t{30});
        QVERIFY(library::sample(all, 30, 0xa5) == a);
        QVERIFY(library::sample(all, 30, 0xa6) != a);
        QCOMPARE(std::set<const Json*>(a.begin(), a.end()).size(), a.size());
        QCOMPARE(library::sample(all, 100000, 7).size(), all.size());
        QCOMPARE(library::seed_text(0xa5), std::string("fp-00000000000000a5"));
        QCOMPARE(*library::seed_of("fp-00000000000000A5"), std::uint64_t{0xa5});
        QVERIFY(!library::seed_of("km-00000000000000a5").has_value());
        QVERIFY(!library::seed_of("fp-123").has_value());
    }

    // The nine presets are as before (the JSON text of figure_presets(), taken with the build before FIGURE-02).
    void presetsAreAsBefore() {
        QCOMPARE(sha256_of(genko::core::dump_python(mesh3d::figure_presets())),
                 std::string("7eaa858c1fe794e06cba02c1f44e3f74a3beaa72519b3faedd3c73040271a4e3"));
    }

    // The checks' thresholds hold the nine presets: within the ranges, no limb through the body, the mirror exact, drawn;
    // the standing ones on both feet and balanced.
    void presetsPassTheChecks() {
        for (const auto& [name, data] : mesh3d::figure_presets().items()) {
            Json pose = Json::object({{"id", "preset/" + name}, {"category", "preset"}, {"joints", Json::object()}, {"hands", Json::object()},
                                      {"support", "none"}, {"contact", Json::array()}, {"symmetric", false}});
            for (const auto& [k, v] : data.items()) (k == "hands" ? pose["hands"] : pose["joints"][k]) = v;
            if (name == "stand" || name == "point" || name == "arms_up" || name == "think" || name == "peace") {
                pose["support"] = "floor";
                pose["contact"] = Json::array({"l_foot", "r_foot"});
            }
            const checks::Report r = checks::check_pose(pose);
            QVERIFY2(r.ok(), (name + issues_text(r.issues)).c_str());
            for (const char* sex : {"male", "female"}) {
                const auto in = checks::check_inside(checks::figure_of(pose, 120.0, Json::object({{"sex", sex}})));
                QVERIFY2(in.empty(), (name + " " + sex + issues_text(in)).c_str());
            }
        }
    }

    // Every pose of the library, and its mirror: each check (core/pose_checks.hpp), its surface made once. With
    // GENKO_POSE_SHEETS the sheets and what each pose measured are written.
    void everyPose_data() {
        QTest::addColumn<QString>("category");
        for (const auto& c : library::categories()) QTest::newRow(c.id.c_str()) << QString::fromStdString(c.id);
    }
    void everyPose() {
        QFETCH(QString, category);
        library::Filter filter;
        filter.category = category.toStdString();
        // (every pose is checked whole on its own; the poses share out over a few threads — each figure is meshed once,
        // about 2.5 s under ASan — and their words and entries are put together in the library's order)
        const std::vector<const Json*> poses = library::list(filter);
        std::vector<std::string> said(poses.size());
        std::vector<std::optional<genko::test::pose_sheets::Entry>> made(poses.size());
        const auto check_one = [&](std::size_t i) {
            const Json* pose = poses[i];
            std::string& failed = said[i];
            if (library::is_pair(*pose)) {  // (a pair: both people and the two together, and the mirror of it)
                const checks::PairReport pr = checks::check_pair(*pose);
                if (!pr.ok()) failed += pr.id + issues_text(pr.issues) + "\n";
                const Json mirror = library::mirrored(*pose);
                if (library::mirrored(mirror) != *pose) failed += pr.id + "\n  mirror: the mirror of the mirror is not the pair\n";
                const checks::PairReport pm = checks::check_pair(mirror, Json(), false);  // (its bodies' inside and lines: the pair's own, mirrored)
                if (!pm.ok()) failed += pr.id + " (mirrored)" + issues_text(pm.issues) + "\n";
                // (the mirror's two are the reflection of the pair's two: b's pelvis across a's middle)
                const auto two = checks::pair_figures(*pose);
                const auto twin = checks::pair_figures(mirror);
                if (std::fabs(two[1]["pos"][0].get<double>() + twin[1]["pos"][0].get<double>()) > 2e-4 ||
                    std::fabs(two[1]["pos"][2].get<double>() - twin[1]["pos"][2].get<double>()) > 2e-4) {
                    failed += pr.id + "\n  mirror: b is not where the reflection puts it\n";
                }
                made[i] = genko::test::pose_sheets::Entry{*pose, pr.a, pr};
                return;
            }
            const checks::Report r = checks::check_pose(*pose);
            if (!r.ok()) failed += r.id + issues_text(r.issues) + "\n";
            const Json mirror = library::mirrored(*pose);
            const Json mirror_prim = checks::figure_of(mirror);
            for (const auto& list : {checks::check_ranges(mirror["joints"]), checks::check_inside(mirror_prim), checks::check_mirror(mirror)}) {
                if (!list.empty()) failed += r.id + " (mirrored)" + issues_text(list) + "\n";
            }
            for (const char* sex : {"male", "female"}) {
                const auto in = checks::check_inside(checks::figure_of(*pose, 120.0, Json::object({{"sex", sex}})));
                if (!in.empty()) failed += r.id + " (" + sex + ")" + issues_text(in) + "\n";
            }
            made[i] = genko::test::pose_sheets::Entry{*pose, r, std::nullopt};
        };
        const std::size_t workers = std::max<std::size_t>(1, std::min<std::size_t>({4, std::thread::hardware_concurrency(), poses.size()}));
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back([&] {
                for (std::size_t i = next++; i < poses.size(); i = next++) check_one(i);
            });
        }
        for (std::thread& t : pool) t.join();
        std::vector<genko::test::pose_sheets::Entry> entries;
        std::string failed;
        for (std::size_t i = 0; i < poses.size(); ++i) {
            failed += said[i];
            entries.push_back(std::move(*made[i]));
        }
        if (!sheets_dir().empty()) {
            for (const auto& e : entries) {
                const Json m = genko::test::pose_sheets::measured(e);
                if (m.is_array()) {
                    for (const Json& one : m) measured_.push_back(one);
                } else {
                    measured_.push_back(m);
                }
            }
            genko::test::pose_sheets::write(sheets_dir(), category.toStdString(), entries);
        }
        QVERIFY2(failed.empty(), failed.c_str());
    }

    // The library's filters for the gallery and the tool to come (G4): categories and their counts, a category, tags,
    // words in Japanese or English (any ASCII case), what the body rests on, its contacts; a mirrored id.
    void filtersFindPoses() {
        std::size_t all = 0;
        for (const auto& c : library::categories()) {
            library::Filter f;
            f.category = c.id;
            QCOMPARE(library::list(f).size(), c.count);
            all += c.count;
        }
        QCOMPARE(library::list().size(), all);
        library::Filter f;
        f.query = "スマホ";
        const auto ja = library::list(f);
        f.query = "PHONE";
        const auto en = library::list(f);
        QVERIFY(ja.size() >= 4 && en.size() >= ja.size());
        for (const Json* p : ja) QVERIFY(std::find(en.begin(), en.end(), p) != en.end());
        f = {};
        f.tags = {"正座"};
        QVERIFY(!library::list(f).empty());
        f = {};
        f.support = "chair";
        for (const Json* p : library::list(f)) {
            QVERIFY(library::is_pair(*p) ? ((*p)["figures"][0].contains("seat_h") || (*p)["figures"][1].contains("seat_h")) : p->contains("seat_h"));
        }
        f.contact = {"seat", "l_foot"};
        QVERIFY(!library::list(f).empty());
        f = {};
        f.category = "sit_floor";
        f.query = "あぐら";
        QVERIFY(!library::list(f).empty());
        f.category = "stand";
        QVERIFY(library::list(f).empty());
        // a mirrored id: the mirror of the pose, its id with ~m; twice is the pose
        const std::optional<Json> m = library::resolve("stand/wave~m");
        QVERIFY(m.has_value());
        QCOMPARE((*m)["id"], Json("stand/wave~m"));
        QCOMPARE((*m)["joints"], library::mirrored_joints((*library::find("stand/wave"))["joints"]));
        QCOMPARE((*library::resolve("stand/wave~m", true))["id"], Json("stand/wave"));
        QCOMPARE(*library::resolve("stand/wave", false), *library::find("stand/wave"));
        QVERIFY(!library::resolve("stand/nope").has_value());
        QVERIFY(!library::resolve("~m").has_value());
    }

    // Pair poses (G5): couple and two_people hold only pairs, the other categories none; a pair's summary says pair and
    // both people; "<id>#a" and "#b" are its people as poses of their own (the mirror's too), "#a" of a pose of one person
    // is none; the filters take a pair when one of its people matches (contact partner, support none); b is where the
    // pair says (pair_figures: a's pelvis + offset × u, turned by turn).
    void pairEntries() {
        std::size_t pairs = 0;
        for (const Json* p : library::list()) {
            const std::string cat = (*p)["category"].get<std::string>();
            const bool pair_cat = cat == "couple" || cat == "two_people";
            QCOMPARE(library::is_pair(*p), pair_cat);
            if (!pair_cat) continue;
            ++pairs;
            const Json sum = library::summary(*p);
            QCOMPARE(sum["pair"], Json(true));
            QCOMPARE(sum["figures"].size(), std::size_t{2});
            const std::string id = (*p)["id"].get<std::string>();
            const std::optional<Json> a = library::resolve(id + "#a");
            const std::optional<Json> bm = library::resolve(id + "~m#b");
            QVERIFY(a && bm);
            QCOMPARE((*a)["joints"], (*p)["figures"][0]["joints"]);
            QCOMPARE((*bm)["joints"], library::mirrored_joints((*p)["figures"][1]["joints"]));
            QCOMPARE((*bm)["id"], Json(id + "~m#b"));
            const auto two = checks::pair_figures(*p);
            const double u = mesh3d::figure_skeleton(two[0]).unit;
            const Json& f = (*p)["figures"][1];
            for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(two[1]["pos"][i].get<double>() - f["offset"][i].get<double>() * u) < 2e-4);
            QVERIFY(std::fabs(two[1]["rot"][1].get<double>() - f["turn"].get<double>()) < 2e-4);
        }
        QVERIFY(pairs >= 60);
        QVERIFY(!library::resolve("stand/wave#a").has_value());
        library::Filter f;
        f.contact = {"partner"};
        const auto leaning = library::list(f);
        QVERIFY(!leaning.empty());
        for (const Json* p : leaning) QVERIFY(library::is_pair(*p));
        f = {};
        f.category = "couple";
        f.support = "none";
        QVERIFY(!library::list(f).empty());  // (one of them carried)
        QCOMPARE(library::mirror_id("couple/hug#a"), std::string("couple/hug~m#a"));
        QCOMPARE(library::mirror_id("couple/hug~m#b"), std::string("couple/hug#b"));
        QCOMPARE(library::mirror_id("stand/wave"), std::string("stand/wave~m"));
    }

    // Thumbnails are the same bytes every time and on every thread (a pose's surface made in four threads at once), and
    // a few are pinned (G3's four, unchanged).
    void thumbnailsAreTheSameBytes() {
        std::vector<std::vector<Json>> prims;  // (a pair's two)
        for (const auto& c : library::categories()) {
            library::Filter f;
            f.category = c.id;
            const auto list = library::list(f);
            QVERIFY2(!list.empty(), c.id.c_str());
            for (const Json& pose : {*list.front(), library::mirrored(*list.back())}) {
                prims.push_back(library::is_pair(pose) ? genko::render::figure_thumbs::figures_for(pose) : std::vector<Json>{checks::figure_of(pose)});
            }
        }
        // (batch A's categories with G3's thumbnail, pinned; batch B's with the gallery's: render::figure_thumbs, which
        // draws a body lying down as large as it fits)
        const std::size_t batch_a = 16;
        const auto thumbs = [&](std::size_t first, std::size_t step, std::vector<std::string>& out) {
            for (std::size_t i = first; i < prims.size(); i += step) {
                out[i] = i < batch_a ? checks::thumbnail(prims[i][0], 0.785, 60, 90)
                                     : genko::render::figure_thumbs::grey(prims[i], 0.785, PoseGalleryWidth, PoseGalleryHeight, 2);
            }
        };
        mesh3d::clear_caches();
        std::vector<std::string> one(prims.size());
        thumbs(0, 1, one);
        std::vector<std::string> again(prims.size());
        thumbs(0, 1, again);
        mesh3d::clear_caches();
        std::vector<std::string> four(prims.size());
        {
            std::vector<std::thread> threads;
            for (std::size_t t = 0; t < 4; ++t) threads.emplace_back(thumbs, t, 4, std::ref(four));
            for (auto& t : threads) t.join();
        }
        for (std::size_t i = 0; i < prims.size(); ++i) {
            const std::size_t area = i < batch_a ? std::size_t{60 * 90} : std::size_t{PoseGalleryWidth * PoseGalleryHeight};
            QCOMPARE(one[i].size(), area);
            QVERIFY(one[i] == again[i]);
            QVERIFY(one[i] == four[i]);
            QVERIFY2(static_cast<std::size_t>(std::count(one[i].begin(), one[i].end(), static_cast<char>(255))) < area - 300, std::to_string(i).c_str());  // (a figure drawn)
        }
        std::string got;
        for (std::size_t i : {0, 3, 8, 13}) got += sha256_of(one[i]) + " ";
        QCOMPARE(got, std::string(kThumbPins));
    }

    // figures@1 is named while a prim says pose_id, hold, props, pair, spread or thumb, and not otherwise; such a book is saved and read
    // back with them as they were.
    void figuresFeature() {
        namespace storage = genko::storage;
        Document doc = book_of_before();
        QVERIFY(!library::uses_figures_feature(doc));
        const auto features = [](const Document& d) {
            QTemporaryDir dir;
            storage::AssetStore store(std::filesystem::path(dir.path().toStdString()));
            return storage::project_payload_v4(d, store)["features"];
        };
        const auto has = [](const Json& list) { return std::find(list.begin(), list.end(), Json("figures@1")) != list.end(); };
        QVERIFY(!has(features(doc)));
        for (const char* batch : {R"([{"op": "pose_figure", "page": 1, "id": "a", "pose_id": "stand/neutral"}])",
                                  R"([{"op": "pose_figure", "page": 1, "id": "a", "hold": {"l": "pen"}}])",
                                  R"([{"op": "pose_figure", "page": 1, "id": "b", "hands": {"r": {"pose": "fist", "thumb": 0.1}}}])",
                                  R"([{"op": "pose_figure", "page": 2, "id": "k", "spread": 0.5}])",
                                  R"([{"op": "pose_figure", "page": 1, "id": "a", "props": false}])"}) {
            const Document d = bus().apply(doc, ops(batch), Actor("genko")).doc;
            QVERIFY2(library::uses_figures_feature(d), batch);
            QVERIFY2(has(features(d)), batch);
        }
        // the pose's id taken away by a drag and its hands' shapes by named ones: the book is one of before again
        const Document dragged = bus().apply(doc, ops(R"([{"op": "pose_figure", "page": 1, "id": "a", "pose_id": "stand/neutral"},
                                                          {"op": "pose_figure", "page": 1, "id": "a", "drag": {"handle": "l_wrist", "to": [70, 100]}}])"),
                                             Actor("genko")).doc;
        QVERIFY(has(features(dragged)));  // (the library's hands say spread and thumb)
        const Document plain = bus().apply(dragged, ops(R"([{"op": "pose_figure", "page": 1, "id": "a", "hands": {"l": "relaxed", "r": "relaxed"}}])"), Actor("genko")).doc;
        QVERIFY(!has(features(plain)));
        // saved and read back
        const Document posed = bus().apply(doc, ops(R"([{"op": "pose_figure", "page": 1, "id": "a", "pose_id": "everyday/drink~m", "keep": "ground"},
                                                        {"op": "pose_figure", "page": 2, "id": "k", "spread": 0.5, "thumb": 0.3}])"),
                                           Actor("genko")).doc;
        QTemporaryDir tmp;
        const std::filesystem::path dir = std::filesystem::path(tmp.path().toStdString()) / "posed.genko";
        {
            storage::ProjectLock lock(dir);
            lock.try_acquire();
            storage::SaveRequest request;
            request.ops = Json::array();
            storage::Saver(lock).save(posed, request);
        }
        const auto loaded = storage::load_document(dir);
        QVERIFY(std::find(loaded.document.features.begin(), loaded.document.features.end(), "figures@1") != loaded.document.features.end());
        for (std::size_t p = 0; p < posed.pages.size(); ++p) QCOMPARE(loaded.document.page(p).prims, posed.page(p).prims);
        QCOMPARE(loaded.document.page(0).prims[0]["pose_id"], Json("everyday/drink~m"));
        QCOMPARE(loaded.document.page(0).prims[0]["hold"], Json::object({{"l", "cup"}}));
    }

    // The op catalogue (`genko schema`) says the new keys.
    void theSchemaSaysTheKeys() {
        for (const Json& item : genko::core::ops_schema()) {
            const std::string op = item["op"].get<std::string>();
            if (op == "add_figure" || op == "pose_figure") {
                for (const char* key : {"pose_id", "mirror", "keep", "hold"}) QVERIFY2(item.contains(key), (op + " " + key).c_str());
                QVERIFY(item["hands"].get<std::string>().find("spread") != std::string::npos);
            }
            if (op == "add_hand" || op == "pose_figure") {
                QVERIFY(item.contains("spread") && item.contains("thumb"));
            }
        }
    }

    // A book without the library's keys is saved as the build before FIGURE-02 saved it (the sha256 of its project.json
    // was taken with that build: native/figures e64e001d).
    void booksWithoutTheNewKeysSaveAsBefore() {
        const std::string text = saved_text(book_of_before());
        QCOMPARE(sha256_of(text), std::string("81b68808e67d7ba9ed3643c1ae8b9a6ba6152e4980b92f5deb50bdf9905b615c"));
        QCOMPARE(saved_text(book_of_before()), text);
    }

    void cleanupTestCase() {
        if (sheets_dir().empty()) return;
        std::ofstream out(std::filesystem::path(sheets_dir()) / "checks.json", std::ios::binary);
        out << genko::core::dump_python_indent2(measured_);
    }
};

QTEST_MAIN(TestFigurePoses)
#include "test_figure_poses.moc"
