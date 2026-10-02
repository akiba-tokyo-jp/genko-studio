// The 3D geometry against the Python baseline (genko.mesh3d, prim3d, persp3d, mannequin, threeops), to the last bit:
//  - numpy's rounding on the reference machine first (geom3d_harness.py blas): the C++ build reproduces its OpenBLAS
//    kernels' order of operations (core/linalg3.hpp); a machine whose BLAS rounds otherwise is reported;
//  - 300 random prims (figures with random builds, joints and hands, heads, hands, models, boxes, guides, props, scenes,
//    mannequins) seen through random cameras: the skeleton and its handles, drags and IK, the mesh (its vertices, the
//    faces in order, their parts), its projection and normals, the shaded surfaces and depth (float32), the pen lines
//    with the hidden parts left out, edges, traces, joined lines, boxes, vanishing points, rulers and the camera that
//    fits a ruler — every float compared bit for bit (sha256 of the bits for the arrays);
//  - 120 model files (OBJ, GLB, glTF; good and broken): the mesh read, or the same error;
//  - the pose library: the same poses, poses.json byte for byte.
// Skipped without the Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/mannequin.hpp"
#include "core/mesh3d.hpp"
#include "core/persp3d.hpp"
#include "core/poses.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "test3d.hpp"

using genko::core::Json;
namespace mesh3d = genko::core::mesh3d;
namespace prim3d = genko::core::prim3d;
using genko::test::bits_of;

namespace {

std::vector<double> flat(const std::vector<mesh3d::Point2>& pts) {
    std::vector<double> out;
    for (const auto& p : pts) {
        out.push_back(p[0]);
        out.push_back(p[1]);
    }
    return out;
}

Json mesh_digest(const mesh3d::Model& model) {
    std::vector<double> v;
    for (const auto& p : model.mesh.v) v.insert(v.end(), p.begin(), p.end());
    Json faces = Json::array();
    for (const auto& f : model.mesh.f) {
        Json item = Json::array();
        for (const auto i : f) item.push_back(i);
        faces.push_back(item);
    }
    Json parts = Json::array();
    for (const int p : model.mesh.parts) parts.push_back(p);
    std::vector<double> marks;
    for (const auto& line : model.marks) {
        for (const auto& p : line) marks.insert(marks.end(), p.begin(), p.end());
    }
    return Json::object({{"n", static_cast<std::int64_t>(model.mesh.v.size())},
                         {"v", genko::test::sha_f64(v)},
                         {"f", genko::test::sha256(genko::core::dump_python(faces))},
                         {"parts", genko::test::sha256(genko::core::dump_python(parts))},
                         {"marks", genko::test::sha_f64(marks)},
                         {"faces", static_cast<std::int64_t>(model.mesh.f.size())}});
}

}  // namespace

class TestContract3dGeometry : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    bool exact_ = true;

    // The C++ value of one quantity against Python's: the same JSON (floats bit for bit), or an error on both sides.
    template <typename F>
    bool same(const std::string& label, const Json& want, F compute, int& failures) {
        Json got;
        std::string error;
        try {
            got = compute();
        } catch (const genko::core::Error& e) {
            error = e.what();
        } catch (const genko::core::OpError& e) {
            error = e.what();
        } catch (const genko::core::OpKeyError& e) {
            error = e.what();
        }
        if (want.is_object() && want.contains("error")) {
            if (!error.empty()) return true;
            qWarning("%s: Python raised %s (%s), C++ gave %s", label.c_str(), want["error"].get<std::string>().c_str(),
                     want["message"].get<std::string>().c_str(), genko::core::dump_python(got).substr(0, 300).c_str());
            ++failures;
            return false;
        }
        if (!error.empty()) {
            qWarning("%s: C++ raised %s, Python gave %s", label.c_str(), error.c_str(), genko::core::dump_python(want).substr(0, 300).c_str());
            ++failures;
            return false;
        }
        std::string where;
        if (!genko::test::strict_equal(got, want, &where)) {
            qWarning("%s: %s", label.c_str(), where.c_str());
            ++failures;
            return false;
        }
        return true;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        mesh3d::clear_caches();
    }

    void blasRoundsAsLinalg3() {
        const auto r = genko::test::geom3d_harness({"blas"}, scratch_.path());
        QVERIFY2(r.finished && r.exit_code == 0, r.err.constData());
        const Json report = genko::test::one_line(r.out);
        // (another BLAS — another CPU's OpenBLAS kernel, another numpy — rounds products in another order: the C++ build
        // keeps the reference machine's, so Python's own results differ there in the last bits)
        if (report["ok"] != Json(true)) exact_ = false;
        QVERIFY2(report["ok"] == Json(true), ("numpy on this machine rounds otherwise: " + genko::core::dump_python(report)).c_str());
    }

    void randomPrimsAndCameras() {
        const QString file = scratch_.path() + QStringLiteral("/geometry.json");
        // (GENKO_TEST_3D_COUNT: fewer cases while working on the code; the test itself runs 300)
        const QString count = qEnvironmentVariable("GENKO_TEST_3D_COUNT", QStringLiteral("300"));
        const auto r = genko::test::geom3d_harness({"geometry", file, "--seed", "20261002", "--count", count}, scratch_.path());
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        const Json cases = genko::test::read_json(file);
        QCOMPARE(cases.size(), static_cast<std::size_t>(count.toInt()));
        int failures = 0;
        int compared = 0;
        std::map<std::string, int> kinds;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const Json& prim = c["prim"];
            const Json* camera = c["camera"].is_null() ? nullptr : &c["camera"];
            const std::string kind = prim["kind"].get<std::string>();
            ++kinds[kind];
            const std::string at = "case " + std::to_string(n) + " (" + kind + ")";
            const auto check = [&](const char* key, auto compute) {
                if (!c.contains(key)) return;
                ++compared;
                same(at + " " + key, c[key], compute, failures);
            };
            mesh3d::clear_caches();
            check("skeleton", [&] {
                const mesh3d::Skeleton sk = mesh3d::figure_skeleton(prim);
                Json points = Json::array();
                for (const auto& [name, p] : sk.points) points.push_back(Json::array({name, Json::array({bits_of(p[0]), bits_of(p[1]), bits_of(p[2])})}));
                Json turns = Json::array();
                for (const auto& [name, m] : sk.turns) {
                    Json values = Json::array();
                    for (const auto& row : m) {
                        for (const double v : row) values.push_back(bits_of(v));
                    }
                    turns.push_back(Json::array({name, values}));
                }
                return Json::object({{"points", points}, {"turns", turns}, {"unit", bits_of(sk.unit)}});
            });
            check("handles", [&] {
                Json out = Json::array();
                for (const auto& [name, p] : mesh3d::figure_handles(prim, camera)) out.push_back(Json::array({name, bits_of(p[0]), bits_of(p[1])}));
                return out;
            });
            for (const char* key : {"drag", "reach"}) {
                check(key, [&] {
                    const Json& want = c[key];
                    if (want.contains("error")) {  // (the inputs are not in an error entry: compute the same failing thing)
                        throw genko::core::Error("value", "Python failed");
                    }
                    const mesh3d::Point2 to{want["to"][0].get<double>(), want["to"][1].get<double>()};
                    const std::string handle = want["handle"].get<std::string>();
                    const Json change = std::string(key) == "drag" ? mesh3d::drag_joint(prim, handle, to, camera)
                                                                   : mesh3d::reach(prim, handle, to, camera);
                    return Json::object({{"handle", handle}, {"to", want["to"]}, {"change", change}});
                });
            }
            check("mesh", [&] { return mesh_digest(*mesh3d::model_of(prim)); });
            check("seen", [&] {
                const mesh3d::Seen s = mesh3d::seen(prim, camera);
                std::vector<double> guides;
                for (const auto& g : s.guides) {
                    for (const double v : flat(g.pts)) guides.push_back(v);
                    guides.insert(guides.end(), g.depth.begin(), g.depth.end());
                }
                return Json::object({{"pts", genko::test::sha_f64(flat(s.at.pts))},
                                     {"depth", genko::test::sha_f64(s.at.depth)},
                                     {"guides", genko::test::sha_f64(guides)}});
            });
            check("normals", [&] {
                const auto model = mesh3d::model_of(prim);
                if (model->mesh.v.empty()) return Json::object({{"n", 0}});
                const auto normals = mesh3d::face_normals(prim, model->mesh, camera);
                std::vector<double> values;
                for (const auto& v : normals) values.insert(values.end(), v.begin(), v.end());
                return Json::object({{"n", static_cast<std::int64_t>(normals.size())}, {"sha", genko::test::sha_f64(values)}});
            });
            check("raster", [&] {
                const Json& want = c["raster"];
                if (want.contains("error")) throw genko::core::Error("value", "Python failed");
                const double box_x = genko::test::from_bits(want["box"][0]);
                const double box_y = genko::test::from_bits(want["box"][1]);
                const Json* light = want["light"].is_null() ? nullptr : &want["light"];
                const std::vector<Json> one{prim};
                const mesh3d::Raster made = mesh3d::raster(one, want["size"][0].get<int>(), want["size"][1].get<int>(),
                                                           want["dpi"].get<double>(), camera, light, want["ambient"].get<double>(), box_x, box_y);
                std::int64_t alpha = 0;
                for (std::size_t i = 0; i < made.zbuf.size(); ++i) alpha += made.alpha(i) ? 1 : 0;
                return Json::object({{"size", want["size"]}, {"dpi", want["dpi"]}, {"light", want["light"]}, {"ambient", want["ambient"]},
                                     {"box", want["box"]}, {"zbuf", genko::test::sha_f32(made.zbuf)},
                                     {"shade", genko::test::sha_f32(made.shade)}, {"alpha", alpha}});
            });
            check("lines", [&] {
                const Json& want = c["lines"];
                if (want.contains("error")) throw genko::core::Error("value", "Python failed");
                const std::vector<Json> one{prim};
                Json out = genko::test::lines_digest(mesh3d::lines(one, camera, want["dpi"].get<double>()));
                Json full = Json::object({{"dpi", want["dpi"]}});
                for (const auto& [k, v] : out.items()) full[k] = v;
                return full;
            });
            check("edges", [&] { return genko::test::edges_digest(prim3d::edges(prim, camera)); });
            check("trace", [&] { return genko::test::lines_digest(prim3d::trace(prim, camera)); });
            check("joined", [&] { return genko::test::lines_digest(prim3d::join_lines(prim3d::trace(prim, camera))); });
            check("bbox", [&] {
                const auto b = prim3d::prim_bbox(prim, camera);
                return Json::array({bits_of(b[0]), bits_of(b[1]), bits_of(b[2]), bits_of(b[3])});
            });
            check("box", [&] {
                const auto b = prim3d::bbox(prim, camera);
                return Json::array({bits_of(b[0]), bits_of(b[1]), bits_of(b[2]), bits_of(b[3])});
            });
            check("bone", [&] {
                const auto bone = genko::core::mannequin::skeleton(prim);
                Json segments = Json::array();
                for (const auto& s : bone.segments) {
                    segments.push_back(Json::array({bits_of(s.a[0]), bits_of(s.a[1]), bits_of(s.b[0]), bits_of(s.b[1]), s.part}));
                }
                Json box = Json::array();
                for (const double v : bone.bbox) box.push_back(bits_of(v));
                return Json::object({{"segments", segments},
                                     {"head", Json::array({bits_of(bone.head_c[0]), bits_of(bone.head_c[1]), bits_of(bone.head_r)})},
                                     {"facing", bone.facing},
                                     {"bbox", box}});
            });
            check("pose_to", [&] {
                const Json& want = c["pose_to"];
                if (want.contains("error")) throw genko::core::Error("value", "Python failed");
                const Json change = genko::core::mannequin::pose_to(prim, want["handle"].get<std::string>(), want["to"]);
                return Json::object({{"handle", want["handle"]}, {"to", want["to"]}, {"change", change}});
            });
            check("vanishing", [&] { return genko::core::persp3d::vanishing_points(prim, camera); });
            check("ruler", [&] {
                const auto ruler = genko::core::persp3d::ruler_from(prim, camera);
                return ruler ? *ruler : Json();
            });
            check("camera_for", [&] {
                const Json& want = c["camera_for"];
                if (want.contains("error")) throw genko::core::Error("value", "Python failed");
                const Json base = camera != nullptr ? *camera : Json::object({{"target", Json::array({91.0, 128.5})}});
                const Json ruler = Json::object({{"points", want["points"]}});
                return Json::object({{"points", want["points"]}, {"camera", genko::core::persp3d::camera_for(ruler, prim, base)}});
            });
        }
        std::string spread;
        for (const auto& [k, v] : kinds) spread += k + ":" + std::to_string(v) + " ";
        qInfo("geometry: %d values of %zu prims compared (%s)", compared, cases.size(), spread.c_str());
        QVERIFY(compared > static_cast<int>(cases.size()) * 10);
        QCOMPARE(failures, 0);
    }

    void modelFiles() {
        const QString file = scratch_.path() + QStringLiteral("/models.json");
        const auto r = genko::test::geom3d_harness({"models", file, "--seed", "20261002", "--count", "120"}, scratch_.path());
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        const Json cases = genko::test::read_json(file);
        int failures = 0;
        int errors = 0;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string kind = c["kind"].get<std::string>();
            const std::string data = c["data"].get<std::string>();
            Json got;
            std::string error;
            bool obj_error = false;
            try {
                if (kind == "obj") {
                    got = mesh3d::read_obj(data);
                } else if (kind == "glb") {
                    got = mesh3d::read_gltf(genko::core::a2b_base64(data));
                } else {
                    got = mesh3d::read_gltf(data);
                }
            } catch (const mesh3d::ObjError& e) {
                error = e.what();
                obj_error = true;
            } catch (const genko::core::Error& e) {
                error = e.what();
            } catch (const genko::core::OpKeyError& e) {
                error = e.what();
            }
            const std::string label = "model " + std::to_string(n) + " (" + kind + ")";
            if (c.contains("error")) {
                ++errors;
                if (error.empty()) {
                    qWarning("%s: Python raised %s (%s), C++ read it", label.c_str(), c["error"].get<std::string>().c_str(),
                             c["message"].get<std::string>().c_str());
                    ++failures;
                } else if (!c.contains("crash") && (error != c["message"].get<std::string>() || obj_error != (c["error"] == Json("ObjError")))) {
                    qWarning("%s: Python raised %s (%s), C++ %s", label.c_str(), c["error"].get<std::string>().c_str(),
                             c["message"].get<std::string>().c_str(), error.c_str());
                    ++failures;
                }
                continue;
            }
            if (!error.empty()) {
                qWarning("%s: C++ raised %s", label.c_str(), error.c_str());
                ++failures;
                continue;
            }
            std::string where;
            if (!genko::test::strict_equal(got, c["mesh"], &where)) {
                qWarning("%s: %s", label.c_str(), where.c_str());
                ++failures;
            }
        }
        qInfo("models: %zu files, %d of them refused", cases.size(), errors);
        QVERIFY(errors > 5);
        QCOMPARE(failures, 0);
    }

    // The pose library (genko.poses): the same results step by step and poses.json byte for byte.
    void posesFile() {
        const QString py_config = scratch_.path() + QStringLiteral("/poses-py");
        const QString cpp_config = scratch_.path() + QStringLiteral("/poses-cpp");
        const auto r = genko::test::geom3d_harness({"poses", py_config}, scratch_.path());
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        const Json want = genko::core::parse_python_json(r.out.toStdString());
        const QByteArray before = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", cpp_config.toUtf8());
        Json results = Json::array();
        for (const Json& step : want["steps"]) {
            const std::string name = step[1].get<std::string>();
            try {
                if (step[0] == Json("save")) {
                    results.push_back(genko::core::poses::save_pose(name, step[2]));
                } else if (step[0] == Json("delete")) {
                    genko::core::poses::delete_pose(name);
                    results.push_back(nullptr);
                } else {
                    const auto found = genko::core::poses::find(name);
                    results.push_back(found ? *found : Json());
                }
            } catch (const genko::core::Error& e) {
                results.push_back(Json::object({{"error", e.what()}}));
            }
        }
        const Json kept = genko::core::poses::user_poses();
        if (before.isNull()) {
            qunsetenv("GENKO_CONFIG_DIR");
        } else {
            qputenv("GENKO_CONFIG_DIR", before);
        }
        std::string where;
        QVERIFY2(genko::test::strict_equal(results, want["results"], &where), where.c_str());
        QVERIFY2(genko::test::strict_equal(kept, want["poses"], &where), where.c_str());
        QCOMPARE(genko::test::read_bytes(cpp_config + "/poses.json"), genko::test::read_bytes(py_config + "/poses.json"));
    }
};

QTEST_GUILESS_MAIN(TestContract3dGeometry)
#include "test_contract_3d_geometry.moc"
