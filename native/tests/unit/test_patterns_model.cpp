// PC4 E-0 (patterns@1, schema-v4.md「パターン」, SPEC.md COLOR-09): a pattern as a book keeps it (every key, in order; each
// one that cannot be refused), the suffix its picture is kept with by its mode and depth (PNG, GKCR, GKIR) and the check
// of the picture against it, a book with patterns written with patterns@1 and their pictures and read back the same
// (its pictures kept by gc), a book without patterns written as before, and what makes a book read-only.

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/patterns.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::Document book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("patterns", core::Num(1), 1, spec, core::Binding::Right);
}

std::string png(const char* mode, int w, int h) {
    const int n = std::string_view(mode) == "RGBA" ? 4 : std::string_view(mode) == "LA" ? 2 : std::string_view(mode) == "RGB" ? 3 : 1;
    std::string bytes(std::size_t(w) * h * n, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i * 37 % 251);
    return render::write_png(render::Image::frombytes(mode, render::Size{w, h}, bytes));
}

std::string gkcr(int w, int h, const char* precision) {
    return core::encode_color_pixels(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), precision,
                                     [](std::size_t i) { return std::array<double, 4>{0.25, (i % 7) / 7.0, 0.5, 1.0}; });
}

std::string gkir(core::InkSpace space, int depth, int w, int h) {
    core::InkPixels p = core::blank_ink_pixels(space, depth, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (std::size_t i = 0; i < p.pixel_count(); ++i) {
        for (unsigned c = 0; c < p.colours(); ++c) p.set(i, c, (i + c) % 5 / 5.0);
        p.set(i, p.colours(), 1.0);
    }
    return core::encode_ink_raster(p);
}

core::Pattern pattern(const std::string& id, const std::string& mode, int depth, int w, int h, const std::string& bytes) {
    core::Pattern p;
    p.id = id;
    p.name = "模様 " + id;
    p.asset = storage::AssetStore::ref(bytes);
    p.mode = mode;
    p.depth = depth;
    p.width = w;
    p.height = h;
    p.dpi = 200;
    return p;
}

void keep(core::Document& doc, const core::Pattern& p, const std::string& bytes) {
    doc.patterns.push_back(p);
    doc.pattern_files[p.asset] = std::make_shared<const std::string>(bytes);
}

Json project(const fs::path& dir) { return genko::test::read_json(QString::fromStdString((dir / "project.json").string())); }

std::string error_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

}  // namespace

class TestPatternsModel : public QObject {
    Q_OBJECT

private slots:
    // Every key written, in order; read back the same; each key that cannot be refused with what is wrong.
    void aPatternIsWrittenWhole() {
        const core::Pattern p = pattern("pat-1", "rgb", 8, 4, 3, png("RGBA", 4, 3));
        const Json written = core::pattern_to_json(p);
        std::vector<std::string> keys;
        for (const auto& [k, v] : written.items()) keys.push_back(k);
        QCOMPARE(keys, (std::vector<std::string>{"id", "name", "asset", "mode", "depth", "width", "height", "dpi"}));
        QVERIFY(core::pattern_from_json(written) == p);
        const auto refused = [&](const std::function<void(Json&)>& change) {
            Json j = written;
            change(j);
            return error_of([&] { (void)core::pattern_from_json(j); });
        };
        QVERIFY(refused([](Json& j) { j["id"] = ""; }).find("id must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["id"] = "pat 1"; }).find("id must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["id"] = std::string(65, 'a'); }).find("id must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["name"] = ""; }).find("name must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["name"] = std::string(256, 'a'); }).find("name must be") != std::string::npos);
        QVERIFY(refused([](Json& j) {
                    std::string kana;
                    for (int i = 0; i < 255; ++i) kana += "模";
                    j["name"] = kana;  // (255 characters: taken)
                }).empty());
        QVERIFY(refused([](Json& j) { j["asset"] = "sha256:00"; }).find("asset must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["mode"] = "hsv"; }).find("mode must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["depth"] = 12; }).find("depth must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["depth"] = 8.0; }).find("depth must be") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["width"] = 0; }).find("width") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["width"] = 8193; }).find("width") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["width"] = 8192; j["height"] = 8192; }).find("pixels") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["dpi"] = 49; }).find("dpi") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["dpi"] = true; }).find("dpi") != std::string::npos);
        QVERIFY(refused([](Json& j) { j["scale"] = 1; }).find("unknown key: scale") != std::string::npos);
        QVERIFY(refused([](Json& j) { j.erase("height"); }).find("height") != std::string::npos);
    }

    // The picture's kind by the mode and depth, and its check: PNG RGBA (8-bit RGB) and LA (8-bit grey), GKCR (16- and
    // 32-bit RGB), GKIR (grey of more bits, CMYK and Lab; its body checked whole).
    void thePictureIsOfItsKind() {
        QCOMPARE(core::pattern_suffix(pattern("a", "rgb", 8, 1, 1, "x")), std::string_view(".png"));
        QCOMPARE(core::pattern_suffix(pattern("a", "gray", 8, 1, 1, "x")), std::string_view(".png"));
        QCOMPARE(core::pattern_suffix(pattern("a", "rgb", 16, 1, 1, "x")), core::kColorRasterSuffix);
        QCOMPARE(core::pattern_suffix(pattern("a", "rgb", 32, 1, 1, "x")), core::kColorRasterSuffix);
        QCOMPARE(core::pattern_suffix(pattern("a", "gray", 16, 1, 1, "x")), core::kInkRasterSuffix);
        QCOMPARE(core::pattern_suffix(pattern("a", "cmyk", 8, 1, 1, "x")), core::kInkRasterSuffix);
        QCOMPARE(core::pattern_suffix(pattern("a", "lab", 16, 1, 1, "x")), core::kInkRasterSuffix);
        const auto ok = [](const core::Pattern& p, const std::string& bytes) { return !core::check_pattern_file(p, bytes).has_value(); };
        const std::string rgba = png("RGBA", 5, 4), la = png("LA", 5, 4);
        QVERIFY(ok(pattern("a", "rgb", 8, 5, 4, rgba), rgba));
        QVERIFY(!ok(pattern("a", "rgb", 8, 5, 4, la), la));            // (an LA picture for an RGB pattern)
        QVERIFY(!ok(pattern("a", "rgb", 8, 4, 4, rgba), rgba));        // (another size)
        QVERIFY(!ok(pattern("a", "rgb", 8, 5, 4, "x"), png("RGB", 5, 4)));  // (no alpha)
        QVERIFY(ok(pattern("a", "gray", 8, 5, 4, la), la));
        const std::string u16 = gkcr(6, 2, "u16"), f32 = gkcr(6, 2, "f32");
        QVERIFY(ok(pattern("a", "rgb", 16, 6, 2, u16), u16));
        QVERIFY(ok(pattern("a", "rgb", 32, 6, 2, f32), f32));
        QVERIFY(!ok(pattern("a", "rgb", 32, 6, 2, u16), u16));
        const std::string cmyk = gkir(core::InkSpace::Cmyk, 8, 3, 3), lab = gkir(core::InkSpace::Lab, 16, 3, 3),
                          grey = gkir(core::InkSpace::Gray, 32, 3, 3);
        QVERIFY(ok(pattern("a", "cmyk", 8, 3, 3, cmyk), cmyk));
        QVERIFY(ok(pattern("a", "lab", 16, 3, 3, lab), lab));
        QVERIFY(ok(pattern("a", "gray", 32, 3, 3, grey), grey));
        QVERIFY(!ok(pattern("a", "lab", 8, 3, 3, lab), lab));
        QVERIFY(!ok(pattern("a", "cmyk", 8, 3, 3, lab), lab));
        QVERIFY(!ok(pattern("a", "cmyk", 8, 3, 3, "bytes"), "bytes"));
        // (a GKIR picture cut short: its header whole, its body not — read-only now, not when it is drawn)
        const std::string cut = lab.substr(0, lab.size() - 3);
        QVERIFY(!ok(pattern("a", "lab", 16, 3, 3, cut), cut));
    }

    // A book with patterns of each kind: written with patterns@1 and every picture with its suffix, read back the same
    // (written again: the same keys), gc keeps the pictures; a book without them as before; patterns@1 named while used.
    void aBookWithPatternsIsWrittenAndReadBack() {
        QVERIFY(storage::is_known_feature("patterns@1"));
        core::Document doc = book();
        QVERIFY(!core::uses_patterns(doc));
        const std::string rgba = png("RGBA", 8, 8), u16 = gkcr(4, 4, "u16"), cmyk = gkir(core::InkSpace::Cmyk, 16, 5, 2);
        keep(doc, pattern("pat-1", "rgb", 8, 8, 8, rgba), rgba);
        keep(doc, pattern("pat-2", "rgb", 16, 4, 4, u16), u16);
        keep(doc, pattern("pat-3", "cmyk", 16, 5, 2, cmyk), cmyk);
        QVERIFY(core::uses_patterns(doc));
        QVERIFY(core::find_pattern(doc, "pat-2") != nullptr && core::find_pattern(doc, "pat-9") == nullptr);
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
        genko::test::write_project(doc, dir);
        const Json written = project(dir);
        QCOMPARE(written["features"], Json::array({"patterns@1"}));
        QCOMPARE(written["patterns"].size(), std::size_t(3));
        QCOMPARE(written["patterns"][1], core::pattern_to_json(doc.patterns[1]));
        const storage::AssetStore store(dir);
        QVERIFY(store.has(doc.patterns[0].asset, ".png"));
        QVERIFY(store.has(doc.patterns[1].asset, core::kColorRasterSuffix));
        QVERIFY(store.has(doc.patterns[2].asset, core::kInkRasterSuffix));
        const std::set<std::string> kept = storage::referenced_assets(dir);
        for (const core::Pattern& p : doc.patterns) QVERIFY(kept.contains(p.asset));
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QCOMPARE(loaded.document.patterns.size(), std::size_t(3));
        for (std::size_t i = 0; i < 3; ++i) {
            QVERIFY(loaded.document.patterns[i] == doc.patterns[i]);
            QCOMPARE(*loaded.document.pattern_files.at(doc.patterns[i].asset), *doc.pattern_files.at(doc.patterns[i].asset));
        }
        QTemporaryDir again;
        const fs::path dir2 = fs::path(again.path().toStdString()) / "b";
        genko::test::write_project(loaded.document, dir2);
        std::string where;
        QVERIFY2(genko::test::strict_equal(project(dir2)["patterns"], written["patterns"], &where), where.c_str());
        // without patterns: no key, no feature (the feature named while used)
        core::Document none = book();
        none.features.push_back("patterns@1");
        QTemporaryDir tmp3;
        const fs::path dir3 = fs::path(tmp3.path().toStdString()) / "b";
        genko::test::write_project(none, dir3);
        QVERIFY(!project(dir3).contains("patterns"));
        QCOMPARE(project(dir3)["features"], Json::array());
    }

    // What a book cannot hold opens read-only, and the writer refuses it: a pattern that cannot be read, an id twice, a
    // picture missing or not of its pattern, more than 512 patterns, patterns that are not a list.
    void whatCannotBeOpensReadOnly() {
        const std::string rgba = png("RGBA", 4, 4);
        const auto opened = [&](const std::function<void(Json&, const fs::path&)>& change) {
            core::Document doc = book();
            keep(doc, pattern("pat-1", "rgb", 8, 4, 4, rgba), rgba);
            QTemporaryDir tmp;
            const fs::path dir = fs::path(tmp.path().toStdString()) / "b";
            genko::test::write_project(doc, dir);
            Json p = project(dir);
            change(p, dir);
            storage::write_atomic(dir / "project.json", core::dump_python_indent2(p));
            return storage::load_document(dir).document.read_only_reason;
        };
        QVERIFY(opened([](Json&, const fs::path&) {}).empty());
        QVERIFY(opened([](Json& p, const fs::path&) { p["patterns"][0]["mode"] = "xyz"; }).find("mode") != std::string::npos);
        QVERIFY(opened([](Json& p, const fs::path&) { p["patterns"].push_back(p["patterns"][0]); }).find("twice") != std::string::npos);
        QVERIFY(opened([](Json& p, const fs::path&) { p["patterns"][0]["width"] = 5; }).find("pat-1") != std::string::npos);
        QVERIFY(opened([](Json& p, const fs::path&) { p["patterns"] = Json::object(); }).find("list") != std::string::npos);
        QVERIFY(opened([&](Json&, const fs::path& dir) { fs::remove(storage::AssetStore(dir).path(storage::AssetStore::ref(rgba), ".png")); })
                    .find("missing") != std::string::npos);
        QVERIFY(opened([](Json& p, const fs::path&) {
                    for (int i = 2; i <= 513; ++i) {
                        Json more = p["patterns"][0];
                        more["id"] = "pat-" + std::to_string(i);
                        p["patterns"].push_back(more);
                    }
                }).find("512") != std::string::npos);
        // the writer refuses what it cannot hold
        core::Document doc = book();
        core::Pattern p = pattern("pat-1", "rgb", 8, 4, 4, rgba);
        doc.patterns.push_back(p);  // (its picture not held)
        QTemporaryDir tmp;
        QVERIFY(error_of([&] { genko::test::write_project(doc, fs::path(tmp.path().toStdString()) / "b"); }).find("not held") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestPatternsModel)
#include "test_patterns_model.moc"
