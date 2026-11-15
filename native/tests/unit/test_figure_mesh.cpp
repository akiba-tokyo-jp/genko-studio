// The smooth 3D figure, head and hands (SPEC FIGURE-01, this build's own): every preset's surface is closed and within
// the budget, the same bytes on every run and on every thread, the body numbers change what they say, the hands' poses
// and the soles are as a person sees them, a mirrored pose gives the mirrored body, the lines are the outlines; and the
// other kinds of 3D (boxes, scenes, imported models) are drawn exactly as before (their digests were taken with the
// build before FIGURE-01).
// GENKO_FIGURE_OUT=<folder> also writes the contact sheets there (the pictures a person looks at).

#include <QtTest>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#include "core/figure_shape.hpp"
#include "core/mesh3d.hpp"
#include "core/prim3d.hpp"
#include "figure_sheets.hpp"
#include "test3d.hpp"

namespace {

using genko::core::Json;
namespace mesh3d = genko::core::mesh3d;
namespace shape = genko::core::figure_shape;
using genko::test::figures::figure_prim;
using genko::test::figures::kPresetNames;

Json ops(const char* text) { return genko::core::parse_python_json(text); }

std::string mesh_digest(const mesh3d::Model& m) {
    std::string bytes;
    const auto put = [&](const void* p, std::size_t n) { bytes.append(static_cast<const char*>(p), n); };
    for (const auto& v : m.mesh.v) put(v.data(), sizeof v);
    for (const auto& v : m.vn) put(v.data(), sizeof v);
    for (const auto& t : m.tris) put(t.data(), sizeof t);
    put(m.nocontour.data(), m.nocontour.size());
    for (const auto& line : m.marks) {
        for (const auto& v : line) put(v.data(), sizeof v);
    }
    return genko::test::sha256(bytes);
}

std::string smooth_digest(const shape::Smooth& m) {
    std::string bytes;
    const auto put = [&](const void* p, std::size_t n) { bytes.append(static_cast<const char*>(p), n); };
    for (const auto& v : m.v) put(v.data(), sizeof v);
    for (const auto& v : m.n) put(v.data(), sizeof v);
    for (const auto& t : m.t) put(t.data(), sizeof t);
    put(m.no_contour.data(), m.no_contour.size());
    return genko::test::sha256(bytes);
}

std::string raster_digest(const mesh3d::Raster& r) {
    return genko::test::sha_f32(r.zbuf) + genko::test::sha_f32(r.shade);
}

// Each directed edge of the triangles has its reverse as often (the surface has no hole and faces one way).
bool closed(const mesh3d::Model& m, std::string* why) {
    std::map<std::pair<std::int32_t, std::int32_t>, int> count;
    for (const auto& t : m.tris) {
        for (int i = 0; i < 3; ++i) ++count[{t[static_cast<std::size_t>(i)], t[static_cast<std::size_t>((i + 1) % 3)]}];
    }
    for (const auto& [e, n] : count) {
        const auto back = count.find({e.second, e.first});
        if (back == count.end() || back->second != n) {
            *why = "edge " + std::to_string(e.first) + "-" + std::to_string(e.second) + " has no partner";
            return false;
        }
    }
    return true;
}

// The volume inside (the triangles' signed volumes)
double volume(const mesh3d::Model& m) {
    double v = 0.0;
    for (const auto& t : m.tris) {
        const auto& a = m.mesh.v[static_cast<std::size_t>(t[0])];
        const auto& b = m.mesh.v[static_cast<std::size_t>(t[1])];
        const auto& c = m.mesh.v[static_cast<std::size_t>(t[2])];
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    return v;
}

// The widest |x| of the points with y in [y0, y1]
double width_at(const mesh3d::Model& m, double y0, double y1) {
    double w = 0.0;
    for (const auto& p : m.mesh.v) {
        if (p[1] >= y0 && p[1] <= y1) w = std::max(w, std::fabs(p[0]));
    }
    return w;
}

std::array<double, 6> box_of(const std::vector<mesh3d::Vec3>& pts) {
    std::array<double, 6> b{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    for (const auto& p : pts) {
        for (int a = 0; a < 3; ++a) {
            b[static_cast<std::size_t>(a)] = std::min(b[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(a)]);
            b[static_cast<std::size_t>(a + 3)] = std::max(b[static_cast<std::size_t>(a + 3)], p[static_cast<std::size_t>(a)]);
        }
    }
    return b;
}

Json with(Json prim, const char* key, Json value) {
    prim[key] = std::move(value);
    return prim;
}

Json body(const char* key, double value, const char* sex = "") {
    Json b = Json::object({{key, value}});
    if (*sex != '\0') b["sex"] = sex;
    return b;
}

// a pose mirrored: left and right swapped, each joint's y and z turned the other way
Json mirrored(const Json& prim) {
    Json out = prim;
    Json joints = Json::object();
    for (const auto& [name, axes] : prim["joints"].items()) {
        std::string other = name;
        if (name.rfind("l_", 0) == 0) other = "r_" + name.substr(2);
        if (name.rfind("r_", 0) == 0) other = "l_" + name.substr(2);
        Json a = Json::object();
        for (const auto& [axis, v] : axes.items()) a[axis] = axis == "x" ? v.get<double>() : -v.get<double>();
        joints[other] = a;
    }
    out["joints"] = joints;
    Json hands = Json::object();
    for (const auto& [side, pose] : prim["hands"].items()) hands[side == "l" ? "r" : "l"] = pose;
    out["hands"] = hands;
    return out;
}

// The other kinds of 3D, as the pages have them
std::vector<Json> other_kinds() {
    return {
        ops(R"({"kind": "box", "pos": [80, 90, 0], "size": [40, 30, 20], "rot": [0.3, 0.6, 0.1]})"),
        ops(R"({"kind": "cylinder", "pos": [60, 150, 10], "size": [20, 40, 20], "rot": [0.2, 0, 0.3]})"),
        ops(R"({"kind": "stairs", "pos": [120, 160, 0], "size": [40, 30, 50], "rot": [0.1, 0.8, 0], "steps": 5})"),
        ops(R"({"kind": "scene", "pos": [100, 120, 0], "size": [160, 90, 120], "rot": [0.15, 0.4, 0], "scene": "room"})"),
        ops(R"({"kind": "sphere", "pos": [140, 70, 0], "size": [30, 30, 30]})"),
        ops(R"({"kind": "cone", "pos": [40, 60, 0], "size": [25, 35, 25], "rot": [0.4, 0.2, 0]})"),
        ops(R"({"kind": "prop", "pos": [150, 200, 0], "size": [40, 50, 40], "rot": [0, 0.7, 0], "prop": "chair"})"),
        ops(R"({"kind": "mesh", "pos": [100, 220, 0], "size": [30, 30, 30], "rot": [0.3, 0.5, 0],
                "mesh": {"v": [0.5, 0, 0, -0.5, 0, 0, 0, 0.5, 0, 0, -0.5, 0, 0, 0, 0.5, 0, 0, -0.5],
                         "f": [[0, 2, 4], [2, 1, 4], [1, 3, 4], [3, 0, 4], [2, 0, 5], [1, 2, 5], [3, 1, 5], [0, 3, 5]]}})"),
    };
}

}  // namespace

class TestFigureMesh : public QObject {
    Q_OBJECT

private slots:
    void init() { mesh3d::clear_caches(); }

    // Every preset, of each sex, and the thinnest and the heaviest build: one closed surface each, within the budget,
    // its normals unit vectors; the head and the hands on their own too.
    void everyFigureIsClosedAndWithinTheBudget() {
        std::vector<Json> prims;
        for (const char* preset : kPresetNames) {
            for (const char* sex : {"", "male", "female"}) prims.push_back(figure_prim(preset, sex, 0.0));
        }
        for (const char* preset : {"stand", "sit", "arms_up", "kneel"}) {
            for (const double build : {0.5, 1.8}) prims.push_back(with(figure_prim(preset, "male", 0.0), "body", body("build", build, "male")));
        }
        prims.push_back(with(figure_prim("run", "female", 0.0), "body", ops(R"({"sex": "female", "heads": 4, "legs": 0.6, "hips": 1.6, "shoulders": 0.6})")));
        prims.push_back(with(figure_prim("run", "male", 0.0), "body", ops(R"({"sex": "male", "heads": 10, "legs": 1.5, "hips": 0.6, "shoulders": 1.5, "build": 1.8})")));
        for (const char* pose : {"open", "relaxed", "fist", "point", "peace", "grip"}) {
            for (const char* side : {"l", "r"}) {
                prims.push_back(Json::object({{"kind", "hand"}, {"size", Json::array({25, 25, 25})}, {"side", side}, {"pose", pose}}));
            }
        }
        prims.push_back(ops(R"({"kind": "head", "size": [30, 30, 30]})"));
        std::size_t most = 0;
        for (const Json& prim : prims) {
            const auto m = mesh3d::model_of(prim);
            const std::string what = prim.dump();
            QVERIFY2(m->smooth(), what.c_str());
            QVERIFY2(m->mesh.f.empty(), what.c_str());
            QVERIFY2(m->tris.size() <= shape::kMaxTriangles, (what + ": " + std::to_string(m->tris.size())).c_str());
            QVERIFY2(m->mesh.v.size() > 500, what.c_str());
            QCOMPARE(m->vn.size(), m->mesh.v.size());
            QCOMPARE(m->nocontour.size(), m->mesh.v.size());
            std::string why;
            QVERIFY2(closed(*m, &why), (what + ": " + why).c_str());
            for (std::size_t i = 0; i < m->mesh.v.size(); ++i) {
                const auto& n = m->vn[i];
                QVERIFY2(std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0) < 1e-9, what.c_str());
                for (int a = 0; a < 3; ++a) QVERIFY2(std::isfinite(m->mesh.v[i][static_cast<std::size_t>(a)]), what.c_str());
            }
            QVERIFY2(volume(*m) > 0.0, what.c_str());  // (facing outward)
            if (prim["kind"] == "figure") most = std::max(most, m->tris.size());
        }
        qInfo("the most triangles of a figure: %zu", most);
    }

    // The same bytes every time: the figure, a hand and the head made twice (the cache emptied between), on four
    // threads at once, drawn and traced twice; and the pins (taken when FIGURE-01 was made, again after G2's proportion
    // pass).
    void theSameBytesOnEveryRunAndThread() {
        const Json walk = figure_prim("walk", "", 0.0);
        const Json fist = ops(R"({"kind": "hand", "size": [25, 25, 25], "side": "l", "pose": "fist"})");
        const Json head = ops(R"({"kind": "head", "size": [30, 30, 30]})");
        std::vector<std::string> first;
        for (const Json& p : {walk, fist, head}) first.push_back(mesh_digest(*mesh3d::model_of(p)));
        mesh3d::clear_caches();
        for (std::size_t i = 0; i < 3; ++i) QCOMPARE(mesh_digest(*mesh3d::model_of(std::vector<Json>{walk, fist, head}[i])), first[i]);
        // made on four threads at once (each its own, not the cache's)
        const auto sk = mesh3d::figure_skeleton(walk);
        const auto relaxed = mesh3d::hand_shape(Json("relaxed"));
        const std::string alone = smooth_digest(shape::figure(sk, relaxed, relaxed).mesh);
        std::vector<std::string> threads(4);
        {
            std::vector<std::thread> pool;
            for (std::size_t t = 0; t < 4; ++t) {
                pool.emplace_back([&, t] { threads[t] = smooth_digest(shape::figure(mesh3d::figure_skeleton(walk), relaxed, relaxed).mesh); });
            }
            for (auto& t : pool) t.join();
        }
        for (const std::string& d : threads) QCOMPARE(d, alone);
        // drawn on four threads at once: the same pixels
        const std::vector<Json> prims{with(walk, "pos", Json::array({60, 80, 0})), with(with(fist, "pos", Json::array({30, 40, 0})), "rot", Json::array({0.2, 0.7, 0}))};
        const std::string drawn = raster_digest(mesh3d::raster(prims, 240, 320, 60.0, nullptr, nullptr, 0.35));
        {
            std::vector<std::string> pics(4);
            std::vector<std::thread> pool;
            for (std::size_t t = 0; t < 4; ++t) pool.emplace_back([&, t] { pics[t] = raster_digest(mesh3d::raster(prims, 240, 320, 60.0, nullptr, nullptr, 0.35)); });
            for (auto& t : pool) t.join();
            for (const std::string& d : pics) QCOMPARE(d, drawn);
        }
        const Json traced = genko::test::lines_digest(mesh3d::lines(std::vector<Json>{prims[0]}, nullptr, 40.0));
        mesh3d::clear_caches();
        QCOMPARE(genko::test::lines_digest(mesh3d::lines(std::vector<Json>{prims[0]}, nullptr, 40.0)), traced);
        // the pins (SPEC FIGURE-01; docs/cpp-migration/HANDOFF.md records them)
        const auto m = mesh3d::model_of(walk);
        qInfo("walk: %zu points, %zu triangles, mesh %s; lines %s; drawn %s", m->mesh.v.size(), m->tris.size(), first[0].c_str(), traced.dump().c_str(),
              drawn.c_str());
        QCOMPARE(first[0], std::string(kWalkMesh));
        QCOMPARE(traced.dump(), std::string(kWalkLines));
        QCOMPARE(drawn, std::string(kDrawn));
    }

    // The body numbers do what they say: longer legs a taller figure, fewer heads a larger one, wider shoulders a wider
    // chest, wider hips a wider pelvis, a heavier build more of it; and a woman's hips are wider for her shoulders than a
    // man's.
    void theBodyNumbersChangeTheBody() {
        const Json up = figure_prim("arms_up", "", 0.0);
        const auto extent = [](const Json& prim, int axis) {
            const auto b = box_of(mesh3d::model_of(prim)->mesh.v);
            return b[static_cast<std::size_t>(axis + 3)] - b[static_cast<std::size_t>(axis)];
        };
        const Json stand = figure_prim("stand", "", 0.0);
        QVERIFY(extent(with(stand, "body", body("legs", 0.8)), 1) < extent(stand, 1));
        QVERIFY(extent(stand, 1) < extent(with(stand, "body", body("legs", 1.3)), 1));
        QVERIFY(extent(with(stand, "body", body("heads", 8.5)), 1) < extent(stand, 1));
        QVERIFY(extent(stand, 1) < extent(with(stand, "body", body("heads", 6)), 1));
        const double u = 90.0 / 7.5;
        const auto chest = [&](double v) { return width_at(*mesh3d::model_of(with(up, "body", body("shoulders", v))), -2.1 * u, -1.9 * u); };
        QVERIFY(chest(0.8) < chest(1.0));
        QVERIFY(chest(1.0) < chest(1.3));
        const auto pelvis = [&](double v) { return width_at(*mesh3d::model_of(with(up, "body", body("hips", v))), -0.1 * u, 0.1 * u); };
        QVERIFY(pelvis(0.8) < pelvis(1.0));
        QVERIFY(pelvis(1.0) < pelvis(1.4));
        const auto mass = [&](double v) { return volume(*mesh3d::model_of(with(up, "body", body("build", v)))); };
        QVERIFY(mass(0.6) < mass(1.0));
        QVERIFY(mass(1.0) < mass(1.5));
        const auto ratio = [&](const char* sex) {
            const Json prim = figure_prim("arms_up", sex, 0.0);
            const auto sk = mesh3d::figure_skeleton(prim);
            const double uu = sk.unit;
            const auto m = mesh3d::model_of(prim);
            return width_at(*m, -0.1 * uu, 0.1 * uu) / width_at(*m, -2.1 * uu, -1.9 * uu);
        };
        qInfo("hips / chest: male %.3f, female %.3f, neither %.3f", ratio("male"), ratio("female"), ratio(""));
        QVERIFY(ratio("female") > ratio("") && ratio("") > ratio("male"));
        QVERIFY(ratio("female") > 1.3 * ratio("male"));
    }

    // The hands: a fist is shorter than an open hand, a pointing hand reaches past the fist, fingers spread wide take
    // more room across than fingers together; a hand's thumb turns from beside the palm to across it.
    void theHandsArePosed() {
        const auto hand_box = [](const Json& pose) {
            return box_of(mesh3d::model_of(Json::object({{"kind", "hand"}, {"size", Json::array({40, 40, 40})}, {"side", "r"}, {"pose", pose}}))->mesh.v);
        };
        const auto open = hand_box("open");
        const auto relaxed = hand_box("relaxed");
        const auto fist = hand_box("fist");
        const auto point = hand_box("point");
        QVERIFY(fist[4] - fist[1] < 0.75 * (open[4] - open[1]));
        QVERIFY(relaxed[4] < open[4]);
        QVERIFY(point[4] > fist[4] + 8.0);
        QVERIFY(open[4] > 19.0 && open[4] < 22.0);  // (the wrist at −20: the length asked for, to the middle finger's tip)
        const auto wide = hand_box(ops(R"({"pose": "open", "spread": 1.0, "thumb": 0.0})"));
        const auto together = hand_box(ops(R"({"pose": "open", "spread": 0.0, "thumb": 0.0})"));
        QVERIFY(wide[3] - wide[0] > together[3] - together[0] + 1.0);
        const auto across = hand_box(ops(R"({"pose": "open", "spread": 0.0, "thumb": 1.0})"));
        QVERIFY(across[3] - across[0] < together[3] - together[0] - 2.0);  // (the thumb no longer out to the side)
        QVERIFY(across[5] > together[5] + 2.0);                             // (but in front of the palm)
        // left and right are mirror images
        const auto l = mesh3d::model_of(Json::object({{"kind", "hand"}, {"size", Json::array({40, 40, 40})}, {"side", "l"}, {"pose", "peace"}}));
        const auto r = mesh3d::model_of(Json::object({{"kind", "hand"}, {"size", Json::array({40, 40, 40})}, {"side", "r"}, {"pose", "peace"}}));
        QCOMPARE(l->mesh.v.size(), r->mesh.v.size());
        for (std::size_t i = 0; i < l->mesh.v.size(); ++i) {
            QCOMPARE(l->mesh.v[i][0], -r->mesh.v[i][0]);
            QCOMPARE(l->mesh.v[i][1], r->mesh.v[i][1]);
        }
    }

    // A standing figure's soles are flat: the lowest points of each foot reach from the heel to the ball.
    void theSolesAreFlat() {
        const auto m = mesh3d::model_of(figure_prim("stand", "", 0.0));
        const double u = 90.0 / 7.5;
        for (const double side : {1.0, -1.0}) {
            double low = -1e300;
            for (const auto& p : m->mesh.v) {
                if (p[0] * side > 0.1 * u) low = std::max(low, p[1]);
            }
            double z0 = 1e300;
            double z1 = -1e300;
            for (const auto& p : m->mesh.v) {
                if (p[0] * side > 0.1 * u && p[1] > low - 0.02 * u) {
                    z0 = std::min(z0, p[2]);
                    z1 = std::max(z1, p[2]);
                }
            }
            qInfo("sole %+.0f: lowest y %.3f u, flat over %.3f u", side, low / u, (z1 - z0) / u);
            QVERIFY(z1 - z0 > 0.55 * u);
            QCOMPARE(low, box_of(m->mesh.v)[4]);  // (nothing lower than the soles)
        }
    }

    // A mirrored pose gives the mirrored body (to rounding: the cells are summed in their own order).
    void aMirroredPoseIsTheMirroredBody() {
        Json prim = figure_prim("point", "female", 0.0);
        prim["joints"]["l_leg"] = ops(R"({"x": 0.4, "z": 0.2})");
        prim["joints"]["spine"] = ops(R"({"y": 0.3, "z": 0.1})");
        prim["hands"]["l"] = "fist";
        const auto a = mesh3d::model_of(prim);
        const auto b = mesh3d::model_of(mirrored(prim));
        QCOMPARE(a->mesh.v.size(), b->mesh.v.size());
        QCOMPARE(a->tris.size(), b->tris.size());
        const double u = 90.0 / 7.5;
        for (int axis = 0; axis < 3; ++axis) {
            std::vector<double> x;
            std::vector<double> y;
            for (const auto& p : a->mesh.v) x.push_back(axis == 0 ? -p[0] : p[static_cast<std::size_t>(axis)]);
            for (const auto& p : b->mesh.v) y.push_back(p[static_cast<std::size_t>(axis)]);
            std::sort(x.begin(), x.end());
            std::sort(y.begin(), y.end());
            double most = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::fabs(x[i] - y[i]));
            QVERIFY2(most < 1e-7 * u, std::to_string(most).c_str());
        }
    }

    // The lines are the outlines: few of them, along the edge of the shaded picture all round, never away from it.
    void theLinesAreTheOutlines() {
        for (const double turn : {0.0, 0.785}) {
            const Json prim = with(figure_prim("walk", "female", turn), "pos", Json::array({60, 70, 0}));
            const double dpi = 40.0;
            const double k = dpi / 25.4;
            const std::vector<mesh3d::Line2> lines = mesh3d::lines(std::vector<Json>{prim}, nullptr, dpi);
            const int w = 200;
            const int h = 240;
            const mesh3d::Raster r = mesh3d::raster(std::vector<Json>{prim}, w, h, dpi, nullptr, nullptr, 0.35);
            QVERIFY(!lines.empty());
            QVERIFY2(lines.size() < 120, std::to_string(lines.size()).c_str());
            std::vector<std::uint8_t> near(static_cast<std::size_t>(w * h), 0);
            std::size_t points = 0;
            std::size_t stray = 0;
            for (const auto& line : lines) {
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    const double ax = line[i][0] * k;
                    const double ay = line[i][1] * k;
                    const double bx = line[i + 1][0] * k;
                    const double by = line[i + 1][1] * k;
                    const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(bx - ax, by - ay) * 4)));
                    for (int s = 0; s <= n; ++s) {
                        const int x = static_cast<int>(std::floor(ax + (bx - ax) * s / n));
                        const int y = static_cast<int>(std::floor(ay + (by - ay) * s / n));
                        ++points;
                        bool by_surface = false;
                        for (int dy = -2; dy <= 2; ++dy) {
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int xx = x + dx;
                                const int yy = y + dy;
                                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                                near[static_cast<std::size_t>(yy * w + xx)] = 1;
                                if (r.alpha(static_cast<std::size_t>(yy * w + xx))) by_surface = true;
                            }
                        }
                        if (!by_surface) ++stray;
                    }
                }
            }
            QCOMPARE(stray, std::size_t{0});
            // the edge of the shaded picture: a covered pixel beside an empty one
            std::size_t edge = 0;
            std::size_t drawn = 0;
            for (int y = 1; y + 1 < h; ++y) {
                for (int x = 1; x + 1 < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y * w + x);
                    if (!r.alpha(i)) continue;
                    if (r.alpha(i - 1) && r.alpha(i + 1) && r.alpha(i - static_cast<std::size_t>(w)) && r.alpha(i + static_cast<std::size_t>(w))) continue;
                    ++edge;
                    if (near[i] != 0) ++drawn;
                }
            }
            qInfo("turn %.3f: %zu lines, the edge drawn at %zu of %zu pixels", turn, lines.size(), drawn, edge);
            QVERIFY(edge > 300);
            QVERIFY2(drawn * 100 >= edge * 97, (std::to_string(drawn) + " of " + std::to_string(edge)).c_str());
        }
    }

    // Boxes, scenes and imported models are drawn and traced exactly as before FIGURE-01 (the digests were taken with
    // the build before it).
    void theOtherKindsAreAsBefore() {
        const std::vector<Json> prims = other_kinds();
        Json camera = ops(R"({"turn": 0.3, "tip": 0.2, "roll": 0.05, "focal_mm": 300, "target": [100, 140]})");
        std::string got;
        for (const Json* cam : {static_cast<const Json*>(nullptr), static_cast<const Json*>(&camera)}) {
            got += raster_digest(mesh3d::raster(prims, 300, 420, 50.0, cam, nullptr, 0.35)) + ";";
            for (const Json& p : prims) {
                if (p["kind"] == "mesh") {
                    got += genko::test::lines_digest(mesh3d::lines(std::vector<Json>{p}, cam, 40.0)).dump() + ";";
                } else {
                    got += genko::test::lines_digest(genko::core::prim3d::trace(p, cam)).dump() + ";";
                }
            }
        }
        qInfo("the other kinds: %s", got.c_str());
        QCOMPARE(genko::test::sha256(got), std::string(kOtherKinds));
    }

    // (not a test of its own: the contact sheets, written when GENKO_FIGURE_OUT names a folder)
    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FIGURE_OUT");
        if (out.isEmpty()) return;
        genko::test::figures::write_all(std::filesystem::path(out.toStdString()));
    }

private:
    static constexpr const char* kWalkMesh = "cc8a7dc26f6e8a084e75fc25a835a33ea0a1d87fb47530e71c81be1a3b8d08cd";
    static constexpr const char* kWalkLines = R"({"count":35,"points":779,"sha":"75273a3c6b3787e38d22c5f51877e9ffef7c72b0821243b9c9983308cd329849"})";
    static constexpr const char* kDrawn =
        "08ccdef4df52bcc0ac63a432f59324b4d823493427b57945ff89c3e588122f589255237ced8ae109131c3e7645271f423bc0b82b8d2d860c1b65c8c7a18b5eae";
    static constexpr const char* kOtherKinds = "e409e23fb070dbc71bec9c449236bb21af6a5d096c658aa93538c930a241f54a";
};

QTEST_GUILESS_MAIN(TestFigureMesh)
#include "test_figure_mesh.moc"
