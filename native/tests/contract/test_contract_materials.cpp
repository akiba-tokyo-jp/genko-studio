// The person's material library (render/materials, render/zip), never lost or reached outside: a library that cannot be
// read is never written over, a linked library folder is left alone, a picture outside the library is never deleted, a
// pack's links are not followed, and a zip that is broken, encrypted or too large is refused. And the zip64 archives
// render/zip packs (a backup past 4 GiB or 65535 files) read whole by the reference Python's zipfile.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/materials.hpp"
#include "render/png.hpp"
#include "render/zip.hpp"
#include "testsupport.hpp"

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

}  // namespace

class TestContractMaterials : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

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

    // Names inside a zip as Python's zipfile reads them: code page 437 without the UTF-8 flag ('éá.png' for the bytes
    // 82 A0), "./a.png" unpacked as "a.png" and found by pack.json's "./a.png" (Python 3.12 gives the same: the
    // material "éá" in "cp", "点" in "dot").
    void zipNamesAsPythonReadsThem() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        const std::string png = render::write_png(render::Image::create("RGB", render::Size{2, 2}));
        render::zip::write(dir / "cp.zip", {{"AB.png", png}});
        std::string bytes = slurp(dir / "cp.zip");
        for (std::size_t at = bytes.find("AB.png"); at != std::string::npos; at = bytes.find("AB.png", at)) bytes.replace(at, 2, "\x82\xA0");
        spit(dir / "cp.zip", bytes);
        const auto files = render::zip::read(dir / "cp.zip");
        QCOMPARE(files.size(), std::size_t(1));
        QCOMPARE(files.begin()->first, std::string("éá.png"));
        Json added = materials::import_pack(dir, dir / "cp.zip");
        QCOMPARE(added.size(), std::size_t(1));
        QCOMPARE(added[0]["name"], Json("éá"));
        QCOMPARE(added[0]["folder"], Json("cp"));
        QCOMPARE(render::zip::member_path("./a.png"), std::string("a.png"));
        QCOMPARE(render::zip::inside_path("./a.png"), std::optional<std::string>("a.png"));
        render::zip::write(dir / "dot.zip", {{"pack.json", R"([{"name": "点", "kind": "image", "file": "./a.png"}])"}, {"./a.png", png}});
        added = materials::import_pack(dir, dir / "dot.zip");
        QCOMPARE(added.size(), std::size_t(1));
        QCOMPARE(added[0]["name"], Json("点"));
        QCOMPARE(added[0]["folder"], Json("dot"));
    }

    // Beyond Python: pack.json's "file" is read only inside the pack. "../x.png", an absolute path and a path through
    // a linked folder are skipped like a missing picture (Python 3.12 reads all four here: 外, 絶対, リンク越し, 中).
    void packFilesStayInsideThePack() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        const std::string png = render::write_png(render::Image::create("RGB", render::Size{2, 2}));
        spit(dir / "outside.png", png);
        spit(dir / "pack" / "real" / "in.png", png);
        fs::create_directory_symlink(dir, dir / "pack" / "linked");
        const Json manifest = Json::array({Json{{"name", "外"}, {"kind", "image"}, {"file", "../outside.png"}},
                                           Json{{"name", "絶対"}, {"kind", "image"}, {"file", core::path_to_utf8(dir / "outside.png")}},
                                           Json{{"name", "リンク越し"}, {"kind", "image"}, {"file", "linked/outside.png"}},
                                           Json{{"name", "中"}, {"kind", "image"}, {"file", "real/in.png"}}});
        spit(dir / "pack" / "pack.json", core::dump(manifest, core::DumpOptions{}));
        const Json added = materials::import_pack(dir, dir / "pack");
        QCOMPARE(added.size(), std::size_t(1));
        QCOMPARE(added[0]["name"], Json("中"));
        QCOMPARE(render::zip::inside_path("../outside.png"), std::nullopt);
        QCOMPARE(render::zip::inside_path("/x/outside.png"), std::nullopt);
        // the same in a zip
        render::zip::write(dir / "out.zip", {{"pack.json", core::dump(Json::array({manifest[0], manifest[1], Json{{"name", "中"}, {"kind", "image"}, {"file", "in.png"}}}), core::DumpOptions{})},
                                             {"in.png", png}});
        const Json zipped = materials::import_pack(dir, dir / "out.zip");
        QCOMPARE(zipped.size(), std::size_t(1));
        QCOMPARE(zipped[0]["name"], Json("中"));
    }

    // Beyond Python: a picture that cannot be kept leaves nothing behind (no picture in the library without its entry).
    void aRefusedPictureLeavesNoFile() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        spit(dir / "a.png", render::write_png(render::Image::create("RGB", render::Size{4, 4})));
        const auto pictures = [&] {
            std::size_t n = 0;
            for (const auto& entry : fs::directory_iterator(materials::library_dir(dir))) n += entry.path().extension() == ".png" ? 1 : 0;
            return n;
        };
        // a width that is not a number (float("wide"))
        materials::add_material(dir, "x", "lines", "マイ素材");
        QVERIFY_THROWS_EXCEPTION(core::Error, materials::import_image(dir, dir / "a.png", Json(), Json("画像"), Json("wide")));
        QCOMPARE(pictures(), std::size_t(0));
        // a library past the old 1 MB (as the materials panel read it before 2026-10-11) is read and changed: 16 MB now
        const std::string past_old(2 * 1048576, 'x');
        spit(materials::library_dir(dir) / "library.json", "[{\"id\": \"u-big\", \"name\": \"" + past_old + "\", \"kind\": \"lines\", \"folder\": \"f\"}]");
        QCOMPARE(materials::user_materials(dir).size(), std::size_t(1));
        const Json grown = materials::import_image(dir, dir / "a.png");
        QCOMPARE(pictures(), std::size_t(1));
        QCOMPARE(materials::user_materials(dir).size(), std::size_t(2));
        materials::delete_material(dir, grown["id"].get<std::string>());
        QCOMPARE(pictures(), std::size_t(0));
        // a library that would pass 16 MB
        const std::string big(16 * 1048576 - 200, 'x');
        spit(materials::library_dir(dir) / "library.json", "[{\"id\": \"u-big\", \"name\": \"" + big + "\", \"kind\": \"lines\", \"folder\": \"f\"}]");
        QVERIFY(slurp(materials::library_dir(dir) / "library.json").size() <= 16 * 1048576);
        QCOMPARE(materials::user_materials(dir).size(), std::size_t(1));  // (read: it is within 16 MB)
        const std::string kept = slurp(materials::library_dir(dir) / "library.json");
        try {
            materials::import_image(dir, dir / "a.png");
            QFAIL("a library past 16 MB was written");
        } catch (const core::PyValueError& e) {
            QCOMPARE(std::string(e.what()), std::string("the material library would be too large for the materials panel (at most 16 MB)"));
        }
        QCOMPARE(pictures(), std::size_t(0));
        QCOMPARE(slurp(materials::library_dir(dir) / "library.json"), kept);
        // one past 16 MB is not read (for the list) and is not changed
        spit(materials::library_dir(dir) / "library.json", "[{\"id\": \"u-big\", \"name\": \"" + big + std::string(300, 'y') +
                                                               "\", \"kind\": \"lines\", \"folder\": \"f\"}]");
        QVERIFY(slurp(materials::library_dir(dir) / "library.json").size() > 16 * 1048576);
        QCOMPARE(materials::user_materials(dir).size(), std::size_t(0));
        QVERIFY_THROWS_EXCEPTION(core::Error, materials::import_image(dir, dir / "a.png"));
        QCOMPARE(pictures(), std::size_t(0));
    }

    // The empty folders' list (folders.json) is read up to 16 MB, as the library: a folder added that would make it larger
    // is refused before it is written (written, it could never be read again), the list as it was.
    void foldersListBounded() {
        QTemporaryDir config;
        const fs::path dir = core::path_from_utf8(config.path().toStdString());
        materials::add_folder(dir, "最初");
        const std::string big(16 * 1048576 - 200, 'x');
        spit(materials::library_dir(dir) / "folders.json", "[\"" + big + "\"]");
        QCOMPARE(materials::empty_folders(dir).size(), std::size_t(1));  // (read: within 16 MB)
        const std::string kept = slurp(materials::library_dir(dir) / "folders.json");
        try {
            materials::add_folder(dir, std::string(300, 'y'));
            QFAIL("a folders list past 16 MB was written");
        } catch (const core::PyValueError& e) {
            QCOMPARE(std::string(e.what()), std::string("the material folders would be too large for the materials panel (at most 16 MB)"));
        }
        QCOMPARE(slurp(materials::library_dir(dir) / "folders.json"), kept);
        QCOMPARE(materials::empty_folders(dir).size(), std::size_t(1));
        // one that fits is added, and read
        materials::add_folder(dir, "短い");
        QVERIFY(slurp(materials::library_dir(dir) / "folders.json").size() <= 16 * 1048576);
        QCOMPARE(materials::empty_folders(dir), (std::vector<std::string>{big, "短い"}));
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

    // zip64 (a backup past 4 GiB or 65535 files, Python's zipfile writes it so): the archives of test_gui_look's
    // bigBackupsAreZip64 — one with the zip64 records from its first byte, one of 65537 files — read whole by Python's
    // zipfile (their end records are looked at there, without Python).
    void bigZipsAreReadWholeByPython() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8(tmp.path().toStdString());
        const fs::path source = dir / "s.txt";
        spit(source, "abc");
        const fs::path forced = dir / "forced.zip";
        render::zip::pack(forced, {{"a.genko/project.json", source}, {"x/y.txt", source}}, 1);
        std::vector<std::pair<std::string, fs::path>> many;
        for (int i = 0; i < 65537; ++i) many.emplace_back("n/" + std::to_string(i) + ".txt", source);
        const fs::path crowded = dir / "crowded.zip";
        render::zip::pack(crowded, many);
        for (const auto& [archive, count] : {std::pair{forced, 2}, std::pair{crowded, 65537}}) {
            const genko::test::Run check = genko::test::run(
                genko::test::python_ref(),
                {QStringLiteral("-I"), QStringLiteral("-c"),
                 QStringLiteral("import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; print(len(z.namelist()))"),
                 QString::fromStdString(core::path_to_utf8(archive))},
                QProcessEnvironment::systemEnvironment(), 120000);
            QVERIFY2(check.finished && check.exit_code == 0, qPrintable(check.described()));
            QCOMPARE(check.out.trimmed().toInt(), count);
        }
    }
};

QTEST_GUILESS_MAIN(TestContractMaterials)
#include "test_contract_materials.moc"
