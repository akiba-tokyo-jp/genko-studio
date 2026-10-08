// Photoshop brush files and the person's brush library (render/abr, render/brushes library) against Python's
// genko/abr.py and brushes.save_to_library / load_library (render_harness.py abr-cases, brush-library): .abr files of
// each layout made here (versions 1 and 2 one brush after another, plain and PackBits; version 6 "8BIM samp", the
// tips Photoshop's own fields before them; a tip larger than a brush keeps; tips this does not read; broken files),
// the definitions and each tip's pixels; a picture made a tip; the library file's text, byte for byte.
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/abr.hpp"
#include "render/brushes.hpp"
#include "render/fill.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
using namespace genko;
using core::Json;

namespace {

struct Bytes {
    std::string data;
    Bytes& u8(int v) { data.push_back(static_cast<char>(v & 0xff)); return *this; }
    Bytes& u16(int v) { return u8(v >> 8).u8(v); }
    Bytes& u32(std::int64_t v) { return u16(static_cast<int>((v >> 16) & 0xffff)).u16(static_cast<int>(v & 0xffff)); }
    Bytes& raw(std::string_view s) { data.append(s); return *this; }
    Bytes& zeros(std::size_t n) { data.append(n, '\0'); return *this; }
};

// a tip's pixels: a ring of ink in w × h
std::string ring(int w, int h, int seed) {
    std::string out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = (x + 0.5) / w - 0.5, dy = (y + 0.5) / h - 0.5;
            const double r = std::sqrt(dx * dx + dy * dy);
            out.push_back(static_cast<char>(r < 0.45 && r > 0.2 ? 200 + (x * seed + y) % 56 : (x + y * seed) % 9));
        }
    return out;
}

// PackBits: rows of a run, then literal bytes (a count before each row)
std::string packbits(const std::string& pixels, int w, int h) {
    std::vector<std::string> rows;
    for (int y = 0; y < h; ++y) {
        const std::string row = pixels.substr(static_cast<std::size_t>(y) * w, w);
        std::string packed;
        std::size_t i = 0;
        while (i < row.size()) {
            std::size_t run = 1;
            while (i + run < row.size() && run < 128 && row[i + run] == row[i]) ++run;
            if (run >= 3) {
                packed.push_back(static_cast<char>(1 - static_cast<int>(run)));
                packed.push_back(row[i]);
                i += run;
            } else {
                std::size_t n = std::min<std::size_t>(row.size() - i, 128);
                packed.push_back(static_cast<char>(n - 1));
                packed.append(row.substr(i, n));
                i += n;
            }
        }
        rows.push_back(packed);
    }
    Bytes out;
    for (const auto& r : rows) out.u16(static_cast<int>(r.size()));
    for (const auto& r : rows) out.raw(r);
    return out.data;
}

// one brush of a version 1 / 2 file (kind 2: sampled)
std::string v12_brush(int version, int w, int h, int spacing, bool compressed, const std::u16string& name, int seed) {
    Bytes body;
    body.u32(0).u16(spacing);
    if (version == 2) {
        body.u32(static_cast<std::int64_t>(name.size()));
        for (const char16_t c : name) body.u16(c);
    }
    body.u8(1).u16(0).u16(0).u16(h).u16(w);
    body.u32(0).u32(0).u32(h).u32(w).u16(8).u8(compressed ? 1 : 0);
    const std::string pixels = ring(w, h, seed);
    body.raw(compressed ? packbits(pixels, w, h) : pixels);
    Bytes out;
    out.u16(2).u32(static_cast<std::int64_t>(body.data.size())).raw(body.data);
    return out.data;
}

// one tip of a version 6 "samp" section (subversion 1: 47 bytes before the bounds; 2: 301)
std::string v6_tip(int subversion, int w, int h, int depth, bool compressed, int seed) {
    Bytes body;
    body.zeros(subversion == 1 ? 47 : 301);
    body.u32(0).u32(0).u32(h).u32(w).u16(depth).u8(compressed ? 1 : 0);
    const std::string pixels = ring(w, h, seed);
    body.raw(compressed ? packbits(pixels, w, h) : pixels);
    Bytes out;
    out.u32(static_cast<std::int64_t>(body.data.size())).raw(body.data).zeros((4 - body.data.size() % 4) % 4);
    return out.data;
}

std::string v6_file(int subversion, const std::vector<std::string>& tips, bool with_desc) {
    Bytes out;
    out.u16(6).u16(subversion);
    if (with_desc) out.raw("8BIM").raw("desc").u32(6).raw("abcdef");
    std::string samp;
    for (const auto& t : tips) samp += t;
    out.raw("8BIM").raw("samp").u32(static_cast<std::int64_t>(samp.size())).raw(samp);
    return out.data;
}

// one version 1 sampled brush whose tip is packed as given (`packed`: the row lengths, then the rows)
std::string v1_packed(int w, int h, const std::string& packed) {
    Bytes body;
    body.u32(0).u16(25).u8(1).u16(0).u16(0).u16(h).u16(w);
    body.u32(0).u32(0).u32(h).u32(w).u16(8).u8(1).raw(packed);
    Bytes out;
    out.u16(1).u16(1).u16(2).u32(static_cast<std::int64_t>(body.data.size())).raw(body.data);
    return out.data;
}

std::map<std::string, std::string> files() {
    std::map<std::string, std::string> out;
    {  // version 1: a computed tip skipped, then two sampled ones (plain)
        Bytes b;
        b.u16(1).u16(3);
        b.u16(1).u32(10).zeros(10);
        b.raw(v12_brush(1, 7, 5, 30, false, u"", 3));
        b.raw(v12_brush(1, 12, 9, 250, false, u"", 5));
        out["old-plain.abr"] = b.data;
    }
    {  // version 2: names (UTF-16), PackBits, a spacing past 500 %
        Bytes b;
        b.u16(2).u16(2);
        b.raw(v12_brush(2, 20, 11, 900, true, u"かすれ筆\u0000", 7));
        b.raw(v12_brush(2, 33, 17, 0, true, u"とても長い名前のブラシとても長い名前のブラシとても長い名前のブラシ", 2));
        out["old-named.abr"] = b.data;
    }
    out["new-small.abr"] = v6_file(1, {v6_tip(1, 9, 9, 8, false, 4), v6_tip(1, 5, 5, 16, false, 1), v6_tip(1, 14, 6, 8, true, 6)}, true);
    out["new-large.abr"] = v6_file(2, {v6_tip(2, 300, 40, 8, true, 9), v6_tip(2, 260, 270, 8, false, 8)}, false);
    out["version-3.abr"] = Bytes().u16(3).u16(0).data;
    out["ends-early.abr"] = Bytes().u16(1).u16(1).u16(2).u32(500).u32(0).data;
    out["not-8bim.abr"] = Bytes().u16(6).u16(1).raw("XXXXsamp").u32(0).data;
    out["no-tips.abr"] = Bytes().u16(1).u16(0).data;
    out["only-16-bit.abr"] = v6_file(1, {v6_tip(1, 5, 5, 16, false, 1)}, false);
    {  // packed rows of nothing (filled out with \0), a run that stops short, a literal past the row's end
        Bytes rows;
        rows.u16(0).u16(3).u16(0).u16(2).u16(0).u16(4);
        rows.u8(1).u8(200).u8(201);  // (two literal bytes)
        rows.u8(0xfd).u8(150);       // (a run of four)
        rows.u8(5).u8(1).u8(2).u8(3);  // (six literal bytes promised, three there)
        out["zero-rows.abr"] = v1_packed(40, 6, rows.data);
        Bytes short_run;
        short_run.u16(2).u16(1).u8(0).u8(9).u8(0xfe);  // (the second row's run has no byte)
        out["run-at-end.abr"] = v1_packed(8, 2, short_run.data);
        Bytes body;
        body.zeros(47).u32(0).u32(0).u32(2).u32(8).u16(8).u8(1).raw(short_run.data);
        Bytes tip;
        tip.u32(static_cast<std::int64_t>(body.data.size())).raw(body.data).zeros((4 - body.data.size() % 4) % 4);
        out["new-run-at-end.abr"] = v6_file(1, {v6_tip(1, 6, 6, 8, false, 3), tip.data}, false);
    }
    {  // pictures made tips: dark marks on light paper, and marks on transparency
        std::string rgb, rgba;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 60; ++x) {
                const bool ink = (x - 30) * (x - 30) + (y - 20) * (y - 20) < 150;
                for (int c = 0; c < 3; ++c) rgb.push_back(static_cast<char>(ink ? 20 + x : 240 - y));
                rgba.append({static_cast<char>(x * 4), static_cast<char>(y * 6), 0, static_cast<char>(ink ? 255 - y : 0)});
            }
        out["paper.png"] = render::fills::png_data(render::Image::frombytes("RGB", render::Size{60, 40}, rgb));
        out["clear.png"] = render::fills::png_data(render::Image::frombytes("RGBA", render::Size{60, 40}, rgba));
    }
    return out;
}

Json tip_pixels(const std::string& tip_png) {
    const render::Image image = render::open_image(core::a2b_base64(tip_png));
    return Json{{"mode", image.mode()}, {"size", Json::array({image.width(), image.height()})}, {"bytes", core::b64encode(image.tobytes())}};
}

Json error_of(const core::Error& e) {
    if (const auto* uncaught = dynamic_cast<const core::PyUncaught*>(&e)) return Json{{"error", Json::array({uncaught->type(), e.what()})}};
    return Json{{"error", Json::array({"AbrError", e.what()})}};
}

}  // namespace

class TestContractAbr : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void likePython() {
        QStringList args{"abr-cases", scratch_.path() + "/abr.json"};
        const auto folder = std::filesystem::path(scratch_.path().toStdString());
        for (const auto& [name, bytes] : files()) {
            std::ofstream(folder / name, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            args << QString::fromStdString((folder / name).string());
        }
        const auto py = genko::test::render_harness(args, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const Json want = genko::test::read_json(scratch_.path() + "/abr.json");
        std::size_t brushes = 0;
        for (const auto& [name, bytes] : files()) {
            Json got;
            try {
                if (name.ends_with(".abr")) {
                    Json found = Json::array();
                    const std::string stem = name.substr(0, name.size() - 4);
                    for (const Json& definition : render::abr::brushes_from(bytes, stem + " ")) {
                        Json shown = Json::object();
                        for (const auto& [key, value] : definition.items())
                            if (key != "tip_png") shown[key] = value;
                        shown["tip"] = tip_pixels(definition["tip_png"].get<std::string>());
                        shown["tip_kind"] = definition["tip"];
                        found.push_back(shown);
                        ++brushes;
                    }
                    got = Json{{"brushes", found}};
                } else {
                    got = Json{{"tip", tip_pixels(render::abr::tip_from_picture(folder / name))}};
                }
            } catch (const core::Error& e) {
                got = error_of(e);
            }
            QVERIFY2(want.contains(name), name.c_str());
            Json expected = want[name];
            // (the PNG of a tip is the encoder's own: its pixels are what both keep)
            QCOMPARE(QString::fromStdString(got.dump()), QString::fromStdString(expected.dump()));
        }
        QVERIFY(brushes >= 9);
    }

    // Beyond Python: a broken file cannot unpack without end. A record that points back where it began is refused
    // (Python reads the file again from there), and the tips of one file unpack to at most so many pixels together
    // (checked before each is unpacked: rows packed to nothing cost no input).
    void brokenFilesStopEarly() {
        Bytes back;
        back.u16(1).u16(2).u16(1).u32(-10).zeros(16);
        try {
            (void)render::abr::brushes_from(back.data);
            QFAIL("a record pointing back was read");
        } catch (const core::PyValueError& e) {
            QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("a brush record points back into the file"));
        }
        Bytes three;
        three.u16(1).u16(3);
        for (int n = 0; n < 3; ++n) three.raw(v12_brush(1, 10, 10, 25, false, u"", n + 1));
        QCOMPARE(render::abr::brushes_from(three.data, "", 300).size(), std::size_t(3));
        try {
            (void)render::abr::brushes_from(three.data, "", 299);
            QFAIL("past the budget");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("abr_too_large"));
        }
        Bytes nothing;  // (16384 × 16384 from 32 KB of row lengths of 0, again and again)
        for (int r = 0; r < 16384; ++r) nothing.u16(0);
        const std::string huge = v1_packed(16384, 16384, nothing.data);
        try {
            (void)render::abr::brushes_from(huge, "", 1 << 20);
            QFAIL("past the budget");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("abr_too_large"));
        }
    }

    void libraryLikePython() {
        const QString py_config = scratch_.path() + "/py-config", cpp_config = scratch_.path() + "/cpp-config";
        const auto py = genko::test::render_harness({"brush-library", scratch_.path() + "/library.json", py_config}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const Json want = genko::test::read_json(scratch_.path() + "/library.json");
        const std::filesystem::path config = std::filesystem::path(cpp_config.toStdString());
        QCOMPARE(render::brushes::load_library(config), Json::object());  // (none yet)
        render::brushes::save_to_library(config, "my_a", Json{{"label", "細い線"}, {"base", "gpen"}, {"width_mm", 0.35}, {"opacity", 0.8}, {"taper", true}});
        render::brushes::save_to_library(config, "my_b", Json{{"label", "B"}, {"base", "maru"}, {"width_mm", 1}, {"rgb", Json::array({255, 255, 255})}, {"spacing", 0.25}});
        render::brushes::save_to_library(config, "my_c", Json{{"label", "\"引用\"\n改行"}, {"base", "gpen"}, {"min_pressure", 1e-05}});
        render::brushes::save_to_library(config, "my_b", std::nullopt);
        std::ifstream file(render::brushes::library_path(config), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        QCOMPARE(QString::fromStdString(text), QString::fromStdString(want["text"].get<std::string>()));
        QCOMPARE(render::brushes::load_library(config), want["loaded"]);
        // a file that is not JSON: an empty library, as Python reads it; but never written over (beyond Python, which
        // would keep only the one brush saved)
        std::ofstream(render::brushes::library_path(config), std::ios::binary | std::ios::trunc) << "{not json";
        QCOMPARE(render::brushes::load_library(config), Json::object());
        try {
            render::brushes::save_to_library(config, "my_d", Json{{"label", "D"}});
            QFAIL("a library that cannot be read was written over");
        } catch (const core::PyUncaught& e) {
            QCOMPARE(QString::fromStdString(e.type()), QStringLiteral("OSError"));
        }
        std::ifstream again(render::brushes::library_path(config), std::ios::binary);
        QCOMPARE(std::string((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>()), std::string("{not json"));
    }
};

QTEST_GUILESS_MAIN(TestContractAbr)
#include "test_contract_abr.moc"
