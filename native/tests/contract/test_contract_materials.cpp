// The person's material library (render/materials, render/zip) against Python's genko/materials/__init__.py
// (render_harness.py material-library): the same steps — materials added, pictures imported (PNG with alpha, a
// palette with transparency, grey, JPEG, BMP, GIF), renamed, tagged, deleted, folders made, packs exported and imported
// (zip and folder, with pack.json or only pictures) — give the same results and errors, the same library.json and
// folders.json text byte for byte, and pictures with the same pixels. Then what C++ does beyond Python, so a library
// is never lost or reached outside: a library that cannot be read is never written over, a linked library folder is
// left alone, a picture outside the library is never deleted, a pack's links are not followed, and a zip that is
// broken, encrypted or too large is refused.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/materials.hpp"
#include "render/png.hpp"
#include "render/zip.hpp"
#include "rendertest.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace materials = render::materials;

namespace {

std::string slurp(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void spit(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

Json pixels(const std::string& bytes) {
    const render::Image image = render::open_image(bytes, render::kPillowOpenLimits);
    const std::string raw = image.tobytes();
    const QByteArray sha = QCryptographicHash::hash(QByteArray(raw.data(), static_cast<qsizetype>(raw.size())), QCryptographicHash::Sha256).toHex();
    return Json{{"mode", std::string(image.mode())}, {"size", Json::array({image.width(), image.height()})}, {"sha256", sha.toStdString()}};
}

std::string type_of(const core::Error& e) {
    if (const auto* uncaught = dynamic_cast<const core::PyUncaught*>(&e)) return uncaught->type();
    if (dynamic_cast<const core::PyValueError*>(&e) != nullptr) return "ValueError";
    if (dynamic_cast<const core::PyTypeError*>(&e) != nullptr) return "TypeError";
    return "Error:" + e.code();
}

// The steps, as the harness runs them (paths in the scratch folder).
Json steps(const QString& scratch) {
    const auto at = [&](const char* name) { return (scratch + "/" + QString::fromUtf8(name)).toStdString(); };
    const Json lines = Json{{"strokes", Json::array({Json{{"id", "s1"}, {"points", Json::array({Json::array({1.0, 2.0}), Json::array({5.5, 7.25})})},
                                                         {"width_mm", 0.5}, {"kind", "gpen"}}})},
                            {"patches", Json::array()}};
    Json s = Json::array();
    const auto picture = [&](const char* name, const char* mode, int w, int h, const char* format) {
        s.push_back(Json{{"do", "picture"}, {"path", at(name)}, {"mode", mode}, {"size", Json::array({w, h})}, {"format", format}});
    };
    picture("a.png", "RGBA", 40, 20, "PNG");
    picture("p.png", "P", 17, 9, "PNG");
    picture("l.png", "L", 9, 30, "PNG");
    picture("la.png", "LA", 12, 12, "PNG");
    picture("c.jpg", "RGB", 33, 21, "JPEG");
    picture("c.bmp", "RGB", 13, 7, "BMP");
    picture("g.gif", "P", 10, 6, "GIF");
    picture("pics/top.png", "RGB", 8, 8, "PNG");
    picture("pics/sub/b.PNG", "RGBA", 6, 4, "PNG");
    picture("pics/sub/deeper/c.jpg", "RGB", 5, 5, "JPEG");
    picture("pics/sub.png", "L", 3, 3, "PNG");
    s.push_back(Json{{"do", "file"}, {"path", at("pics/notes.txt")}, {"text", "読まない"}});
    s.push_back(Json{{"do", "file"}, {"path", at("pics/.png")}, {"text", "not a picture"}});
    s.push_back(Json{{"do", "file"}, {"path", at("broken.png")}, {"text", "not a picture at all"}});
    s.push_back(Json{{"do", "file"}, {"path", at("notes.txt")}, {"text", "a text"}});
    s.push_back(Json{{"do", "file"}, {"path", at("empty/readme.md")}, {"text", "no pictures"}});
    const std::string manifest = core::dump(Json::array({
        Json{{"id", "x1"}, {"name", "トーン素材"}, {"kind", "tone"}, {"tone", Json{{"pattern", "dot"}, {"density", 0.25}}}, {"tags", Json::array({"網"})}},
        Json{{"name", "古い効果"}, {"kind", "effect"}, {"effect", "focus"}, {"folder", "効果"}, {"builtin", true}},
        Json{{"name", "絵"}, {"kind", "image"}, {"file", "a.png"}, {"width_mm", 30}, {"tags", Json::array({"青", "  "})}},
        Json{{"name", "ない絵"}, {"kind", "image"}, {"file", "missing.png"}},
        Json{{"name", "フォルダ"}, {"kind", "image"}, {"file", ""}},
        Json{{"kind", "lines"}, {"items", lines}},
        Json{{"name", "未知"}, {"kind", "bogus"}},
        Json{{"name", "線"}, {"kind", "lines"}, {"items", lines}, {"folder", "  "}},
    }), core::DumpOptions{.indent = 1, .item_separator = ","});
    s.push_back(Json{{"do", "file"}, {"path", at("manifest/pack.json")}, {"text", manifest}});
    s.push_back(Json{{"do", "zip"}, {"path", at("manifest.zip")},
                     {"members", Json::array({Json::array({"pack.json", nullptr, manifest}), Json::array({"a.png", at("a.png"), nullptr})})}});
    s.push_back(Json{{"do", "zip"}, {"path", at("pictures.zip")},
                     {"members", Json::array({Json::array({"one.gif", at("g.gif"), nullptr}), Json::array({"in/two.bmp", at("c.bmp"), nullptr}),
                                              Json::array({"in/", nullptr, ""}), Json::array({"skip.txt", nullptr, "x"})})}});
    s.push_back(Json{{"do", "zip"}, {"path", at("evil.zip")},
                     {"members", Json::array({Json::array({"ok.png", at("a.png"), nullptr}), Json::array({"../evil.png", at("a.png"), nullptr})})}});
    // (a library Python wrote long ago: old kinds made tones, an effect given its params, on the first save)
    s.push_back(Json{{"do", "write_library"}, {"name", "library.json"},
                     {"text", "[{\"id\": \"u-old1\", \"name\": \"昔の網\", \"kind\": \"dot\", \"lpi\": 50, \"folder\": \"昔\"},\n"
                              " {\"id\": \"u-old2\", \"name\": \"昔の砂\", \"kind\": \"noise\"},\n"
                              " {\"id\": \"u-old3\", \"name\": \"昔の線\", \"kind\": \"effect\", \"params\": {\"count\": 3}, \"extra\": 1.5}]"}});
    s.push_back(Json{{"do", "user_materials"}});
    s.push_back(Json{{"do", "folders"}});
    s.push_back(Json{{"do", "add_material"}, {"name", " パーツ1 "}, {"kind", "lines"}, {"folder", "マイ素材"}, {"data", Json{{"items", lines}}}});
    s.push_back(Json{{"do", "add_material"}, {"name", "  "}, {"kind", "lines"}});
    s.push_back(Json{{"do", "add_material"}, {"name", "x"}, {"kind", "bogus"}});
    s.push_back(Json{{"do", "add_material"}, {"name", "ブラシ"}, {"kind", "brush"}, {"folder", " "}, {"data", Json{{"brush", Json{{"base", "gpen"}, {"width_mm", 0.7}}}, {"tags", Json::array({"線"})}}}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("a.png")}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("p.png")}, {"name", "パレット"}, {"folder", "  "}, {"width_mm", 80}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("l.png")}, {"folder", "写真"}, {"width_mm", 0}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("la.png")}, {"name", "  半透明  "}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("c.jpg")}, {"width_mm", 12.5}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("c.bmp")}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("g.gif")}});
    s.push_back(Json{{"do", "import_image"}, {"path", at("broken.png")}, {"compare", "type"}});
    s.push_back(Json{{"do", "update_material"}, {"id", "$3"}, {"change", Json{{"name", "改名"}, {"folder", "新"}, {"tags", Json::array({"  青 ", "", "赤", 3})}}}});
    s.push_back(Json{{"do", "update_material"}, {"id", "$4"}, {"change", Json{{"tags", nullptr}, {"other", "ignored"}}}});
    s.push_back(Json{{"do", "update_material"}, {"id", "u-none"}, {"change", Json{{"name", "x"}}}});
    s.push_back(Json{{"do", "get_material"}, {"id", "$3"}});
    s.push_back(Json{{"do", "get_material"}, {"id", "dot-60-30"}});
    s.push_back(Json{{"do", "get_material"}, {"id", "u-none"}});
    s.push_back(Json{{"do", "add_folder"}, {"name", "  空のフォルダ  "}});
    s.push_back(Json{{"do", "add_folder"}, {"name", "空のフォルダ"}});
    s.push_back(Json{{"do", "add_folder"}, {"name", "トーン"}});
    s.push_back(Json{{"do", "add_folder"}, {"name", "もう一つ"}});
    s.push_back(Json{{"do", "add_folder"}, {"name", "　"}});
    s.push_back(Json{{"do", "folders"}});
    s.push_back(Json{{"do", "delete_material"}, {"id", "$5"}});
    s.push_back(Json{{"do", "delete_material"}, {"id", "$0"}});
    s.push_back(Json{{"do", "delete_material"}, {"id", "dot-60-30"}});
    s.push_back(Json{{"do", "delete_material"}, {"id", "u-none"}});
    s.push_back(Json{{"do", "export_pack"}, {"ids", Json::array({"$3", "$6", "$9", "$1", "dot-60-30"})}, {"path", at("out.zip")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("out.zip")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("pics")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("pics")}, {"folder", "別名"}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("manifest")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("manifest.zip")}, {"folder", "zip から"}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("pictures.zip")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("evil.zip")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("notes.txt")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("empty")}});
    s.push_back(Json{{"do", "import_pack"}, {"path", at("nothing-here")}});
    s.push_back(Json{{"do", "folders"}});
    s.push_back(Json{{"do", "user_materials"}});
    return s;
}

// One step in C++, reported as the harness reports it (before the ids are hidden).
Json run_step(const fs::path& config, const Json& step, std::vector<std::string>& created) {
    const std::string what = step["do"].get<std::string>();
    const auto ref = [&](const Json& value) {
        const std::string text = value.get<std::string>();
        return text.starts_with("$") ? created.at(static_cast<std::size_t>(std::stoi(text.substr(1)))) : text;
    };
    const auto get = [&](const char* key, const Json& fallback) { return step.contains(key) ? step[key] : fallback; };
    Json result = Json::object();
    try {
        Json value;
        if (what == "write_library") {
            spit(materials::library_dir(config) / step["name"].get<std::string>(), step["text"].get<std::string>());
        } else if (what == "add_material") {
            value = materials::add_material(config, step["name"], step["kind"].get<std::string>(), get("folder", "マイ素材"), get("data", Json::object()));
        } else if (what == "import_image") {
            value = materials::import_image(config, core::path_from_utf8(step["path"].get<std::string>()), get("name", Json()), get("folder", "画像"),
                                            get("width_mm", Json()));
        } else if (what == "update_material") {
            value = materials::update_material(config, ref(step["id"]), step["change"]);
        } else if (what == "delete_material") {
            materials::delete_material(config, ref(step["id"]));
        } else if (what == "add_folder") {
            materials::add_folder(config, step["name"].get<std::string>());
        } else if (what == "folders") {
            value = Json::array();
            for (const std::string& name : materials::folders(config)) value.push_back(name);
        } else if (what == "get_material") {
            value = materials::get_material(config, ref(step["id"]));
        } else if (what == "user_materials") {
            value = materials::user_materials(config);
        } else if (what == "import_pack") {
            const Json folder = get("folder", Json());
            value = materials::import_pack(config, core::path_from_utf8(step["path"].get<std::string>()),
                                           folder.is_string() ? std::optional<std::string>(folder.get<std::string>()) : std::nullopt);
        } else if (what == "export_pack") {
            std::vector<std::string> ids;
            for (const Json& id : step["ids"]) ids.push_back(ref(id));
            const fs::path target = materials::export_pack(config, ids, core::path_from_utf8(step["path"].get<std::string>()));
            value = Json::array();
            // (in the order written: Python's namelist())
            const auto files = render::zip::read(target);
            std::vector<std::string> order;
            for (const auto& id : ids) {
                const std::string name = id + ".png";
                if (files.contains(name) && std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
            }
            order.push_back("pack.json");
            if (order.size() != files.size()) throw std::runtime_error("the pack holds other files");
            for (const std::string& name : order) {
                const std::string& bytes = files.at(name);
                value.push_back(Json::array({name, name.ends_with(".json") ? Json(bytes) : pixels(bytes)}));
            }
        }
        for (const Json& item : value.is_array() ? value : Json::array({value})) {
            if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                const std::string id = item["id"].get<std::string>();
                if (id.starts_with("u-") && std::find(created.begin(), created.end(), id) == created.end()) created.push_back(id);
            }
        }
        result["ok"] = value;
    } catch (const core::Error& e) {
        result["error"] = Json::array({type_of(e), e.what()});
    }
    // the ids the library holds, then its files
    const fs::path library = materials::library_dir(config);
    try {
        for (const Json& item : core::parse_python_json(slurp(library / "library.json"))) {
            if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                const std::string id = item["id"].get<std::string>();
                if (id.starts_with("u-") && std::find(created.begin(), created.end(), id) == created.end()) created.push_back(id);
            }
        }
    } catch (const core::Error&) {
    }
    Json files = Json::object();
    std::vector<fs::path> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(library, ec)) names.push_back(entry.path());
    std::sort(names.begin(), names.end());
    for (const fs::path& file : names) {
        const std::string name = core::path_to_utf8(file.filename());
        if (file.extension() == ".png") files[name] = pixels(slurp(file));
        else if (name == "library.json" || name == "folders.json") files[name] = slurp(file);
    }
    result["files"] = files;
    return result;
}

// Python's hide(): each id made as $n (the longest first).
Json hidden(const Json& value, const std::vector<std::string>& created) {
    std::string text = core::dump(value, core::DumpOptions{});
    std::vector<std::size_t> order(created.size());
    for (std::size_t n = 0; n < order.size(); ++n) order[n] = n;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return created[a].size() > created[b].size(); });
    for (const std::size_t n : order) {
        // (json.dumps writes the ids as they are: plain ASCII)
        std::size_t at = 0;
        while ((at = text.find(created[n], at)) != std::string::npos) {
            text.replace(at, created[n].size(), "$" + std::to_string(n));
            at += 1;
        }
    }
    return core::parse_python_json(text);
}

}  // namespace

class TestContractMaterials : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

    void libraryLikePython() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        const QString root = scratch_.path() + "/same";
        QVERIFY(QDir().mkpath(root + "/py") && QDir().mkpath(root + "/cpp"));
        const Json all = steps(root);
        spit(core::path_from_utf8((root + "/steps.json").toStdString()), core::dump(all, core::DumpOptions{.indent = 1, .item_separator = ","}));
        const auto py = genko::test::render_harness({"material-library", root + "/steps.json", root + "/want.json", root + "/py"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const Json want = genko::test::read_json(root + "/want.json");
        const fs::path config = core::path_from_utf8((root + "/cpp").toStdString());
        std::vector<std::string> created;
        std::size_t n = 0, compared = 0;
        for (const Json& step : all) {
            const std::string what = step["do"].get<std::string>();
            if (what == "picture" || what == "file" || what == "zip") continue;  // (inputs the harness made)
            QVERIFY(n < want.size());
            const Json expected = want[n++];
            const Json got = hidden(run_step(config, step, created), created);
            const std::string where = "step " + std::to_string(n - 1) + " (" + what + "): " + core::dump(step, core::DumpOptions{});
            // (the library's files by name, whatever order the ids made sort them in)
            const auto same = [](const Json& a, const Json& b) {
                return core::dump(a, core::DumpOptions{.sort_keys = true}) == core::dump(b, core::DumpOptions{.sort_keys = true});
            };
            if (step.value("compare", std::string()) == "type") {
                // (a picture that cannot be opened: refused on both sides, the library left as it was)
                QVERIFY2(expected.contains("error") && got.contains("error"), where.c_str());
                QVERIFY2(same(got["files"], expected["files"]), where.c_str());
                continue;
            }
            if (!same(got, expected)) {
                // (what differs: the result, or one of the library's files)
                std::string diff;
                for (const char* key : {"ok", "error"}) {
                    if (!same(got.value(key, Json()), expected.value(key, Json())))
                        diff += std::string("\n  ") + key + ": C++ " + core::dump(got.value(key, Json()), {}) + "\n      Python " + core::dump(expected.value(key, Json()), {});
                }
                Json names = Json::object();
                for (const auto& [name, _] : got["files"].items()) names[name] = true;
                for (const auto& [name, _] : expected["files"].items()) names[name] = true;
                for (const auto& [name, _] : names.items()) {
                    if (!same(got["files"].value(name, Json()), expected["files"].value(name, Json())))
                        diff += "\n  " + name + ": C++ " + core::dump(got["files"].value(name, Json()), {}) + "\n      Python " +
                                core::dump(expected["files"].value(name, Json()), {});
                }
                qWarning("%s%s", where.c_str(), diff.c_str());
                QFAIL("differs from Python");
            }
            ++compared;
        }
        QCOMPARE(n, want.size());
        QVERIFY(compared > 40);
        QVERIFY(created.size() >= 20);  // (every kind of import made materials)
    }

    // Beyond Python: a library that cannot be read (not JSON, not a list of entries, NaN that would be written back
    // as null, too large) is never written over by a change, though it reads as an empty one as in Python.
    void unreadableLibraryIsLeftAsItIs_data() {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<int>("listed");  // (what reading it lists, as Python lists it: NaN read as null)
        QTest::newRow("not json") << QByteArray("{not json") << 0;
        QTest::newRow("an object") << QByteArray("{\"id\": \"u-1\"}") << 0;
        QTest::newRow("not entries") << QByteArray("[1, 2]") << 0;
        QTest::newRow("nan") << QByteArray("[{\"id\": \"u-1\", \"kind\": \"tone\", \"density\": NaN}]") << 1;
        QTest::newRow("not utf-8") << QByteArray("[{\"id\": \"u-\xff\"}]") << 0;
        QTest::newRow("too large") << QByteArray(17 << 20, ' ').append("[]") << 0;
    }
    void unreadableLibraryIsLeftAsItIs() {
        QFETCH(QByteArray, text);
        QFETCH(int, listed);
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        const fs::path file = materials::library_dir(dir) / "library.json";
        spit(file, text.toStdString());
        QCOMPARE(static_cast<int>(materials::user_materials(dir).size()), listed);
        const auto refused = [&](const std::function<void()>& change) {
            try {
                change();
                return false;
            } catch (const core::PyUncaught& e) {
                return e.type() == "OSError" && std::string(e.what()).find("cannot be read, so it is left as it is") != std::string::npos;
            }
        };
        QVERIFY(refused([&] { materials::add_material(dir, "x", "lines", "マイ素材"); }));
        QVERIFY(refused([&] { materials::update_material(dir, "u-1", Json{{"name", "y"}}); }));
        QVERIFY(refused([&] { materials::delete_material(dir, "u-1"); }));
        spit(dir / "a.png", render::write_png(render::Image::create("RGBA", render::Size{4, 4})));
        QVERIFY(refused([&] { materials::import_image(dir, dir / "a.png"); }));
        QCOMPARE(QByteArray::fromStdString(slurp(file)), text);
        QCOMPARE(static_cast<int>(std::distance(fs::directory_iterator(materials::library_dir(dir)), fs::directory_iterator())), 1);  // (no picture left)
        // folders.json likewise
        spit(materials::library_dir(dir) / "folders.json", "[\"a\", 3]");
        QVERIFY(refused([&] { materials::add_folder(dir, "b"); }));
        QCOMPARE(slurp(materials::library_dir(dir) / "folders.json"), std::string("[\"a\", 3]"));
    }

    // Beyond Python: a library folder that is a link is neither read nor written; a picture named outside the library
    // ("../x.png", a path, a link) is never deleted or exported.
    void outsideTheLibraryIsNeverTouched() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        const fs::path outside = dir / "outside.png";
        spit(outside, render::write_png(render::Image::create("RGBA", render::Size{3, 3})));
        const std::string before = slurp(outside);
        fs::create_directories(materials::library_dir(dir));
        fs::create_symlink(outside, materials::library_dir(dir) / "u-link.png");
        spit(materials::library_dir(dir) / "library.json",
             "[{\"id\": \"u-a\", \"name\": \"a\", \"kind\": \"image\", \"file\": \"../outside.png\"},"
             " {\"id\": \"u-b\", \"name\": \"b\", \"kind\": \"image\", \"file\": \"" + core::path_to_utf8(outside) + "\"},"
             " {\"id\": \"u-c\", \"name\": \"c\", \"kind\": \"image\", \"file\": \"u-link.png\"}]");
        for (const char* id : {"u-a", "u-b", "u-c"}) QVERIFY(!materials::image_bytes(dir, materials::get_material(dir, id)));
        const auto exported = materials::export_pack(dir, {"u-a", "u-b", "u-c"}, dir / "out.zip");
        const auto files = render::zip::read(exported);
        QCOMPARE(files.size(), std::size_t(1));
        QCOMPARE(core::parse_python_json(files.at("pack.json")), Json::array());
        for (const char* id : {"u-a", "u-b", "u-c"}) materials::delete_material(dir, id);
        QCOMPARE(slurp(outside), before);
        QCOMPARE(materials::user_materials(dir), Json::array());
        // a linked library folder
        QTemporaryDir other;
        const fs::path elsewhere = core::path_from_utf8(other.path().toStdString());
        fs::rename(materials::library_dir(dir), elsewhere / "real");
        fs::create_directory_symlink(elsewhere / "real", materials::library_dir(dir));
        const std::string kept = slurp(elsewhere / "real" / "library.json");
        QCOMPARE(materials::user_materials(dir), Json::array());
        try {
            materials::add_material(dir, "x", "lines", "マイ素材");
            QFAIL("a linked library folder was written");
        } catch (const core::PyUncaught& e) {
            QCOMPARE(e.type(), std::string("OSError"));
        }
        QCOMPARE(slurp(elsewhere / "real" / "library.json"), kept);
    }

    // Beyond Python: a folder pack's links are not followed (a picture linked from outside is not read).
    void packLinksAreNotFollowed() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        spit(dir / "secret.png", render::write_png(render::Image::create("RGBA", render::Size{2, 2})));
        spit(dir / "pack" / "own.png", render::write_png(render::Image::create("RGB", render::Size{2, 2})));
        fs::create_symlink(dir / "secret.png", dir / "pack" / "linked.png");
        fs::create_directory_symlink(dir, dir / "pack" / "loop");
        const Json added = materials::import_pack(dir, dir / "pack");
        QCOMPARE(added.size(), std::size_t(1));
        QCOMPARE(added[0]["name"], Json("own"));
        QCOMPARE(added[0]["folder"], Json("pack"));
    }

    // zip files that are broken, encrypted, zip64 or too large are refused before anything is added.
    void badZipsAreRefused() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        const std::string png = render::write_png(render::Image::create("RGB", render::Size{2, 2}));
        render::zip::write(dir / "good.zip", {{"a.png", png}, {"b.png", png}});
        const std::string good = slurp(dir / "good.zip");
        QCOMPARE(render::zip::read(dir / "good.zip").size(), std::size_t(2));
        const auto refused = [&](const std::string& bytes, const render::zip::Limits& limits = {}) {
            spit(dir / "bad.zip", bytes);
            try {
                render::zip::read(dir / "bad.zip", limits);
                return false;
            } catch (const core::PyValueError&) {
                return true;
            }
        };
        QVERIFY(refused("not a zip"));
        QVERIFY(refused(good.substr(0, good.size() - 5)));
        std::string flipped = good;
        flipped[40] = static_cast<char>(flipped[40] ^ 0x5a);  // (inside the first file's data: its CRC no longer holds)
        QVERIFY(refused(flipped));
        std::string encrypted = good;
        encrypted[good.find(std::string("PK\x01\x02", 4)) + 8] |= 1;
        QVERIFY(refused(encrypted));
        QVERIFY(refused(good, render::zip::Limits{.max_files = 1}));
        QVERIFY(refused(good, render::zip::Limits{.max_file_bytes = 10}));
        QVERIFY(refused(good, render::zip::Limits{.max_total_bytes = static_cast<std::int64_t>(png.size() * 2 - 1)}));
        QVERIFY(refused(good, render::zip::Limits{.max_archive_bytes = 10}));
        for (const char* name : {"/abs.png", "..\\x.png", "a/../../x.png", "C:x.png"}) {
            render::zip::write(dir / "escape.zip", {{name, png}});
            try {
                render::zip::read(dir / "escape.zip");
                QFAIL(name);
            } catch (const core::PyValueError& e) {
                QCOMPARE(std::string(e.what()), std::string("the pack has a file outside itself"));
            }
        }
        // nothing was added by a refused pack
        spit(dir / "broken.zip", "PK broken");
        QVERIFY_THROWS_EXCEPTION(core::PyValueError, materials::import_pack(dir, dir / "broken.zip"));
        QCOMPARE(materials::user_materials(dir), Json::array());
    }
};

QTEST_GUILESS_MAIN(TestContractMaterials)
#include "test_contract_materials.moc"
