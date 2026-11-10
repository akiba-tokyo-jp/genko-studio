// Photoshop brush files and the person's brush library (render/abr, render/brushes library): broken .abr files stop
// early; a library that cannot be read is never written over; `genko brushes import-abr` reads a whole file into the
// library; the brushes' and presets' command line.
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/abr.hpp"
#include "render/brushes.hpp"
#include "render/fill.hpp"
#include "render/png.hpp"
#include "render/psd_descriptor.hpp"
#include "testsupport.hpp"
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

// one version 1 sampled brush whose tip is packed as given (`packed`: the row lengths, then the rows)
std::string v1_packed(int w, int h, const std::string& packed) {
    Bytes body;
    body.u32(0).u16(25).u8(1).u16(0).u16(0).u16(h).u16(w);
    body.u32(0).u32(0).u32(h).u32(w).u16(8).u8(1).raw(packed);
    Bytes out;
    out.u16(1).u16(1).u16(2).u32(static_cast<std::int64_t>(body.data.size())).raw(body.data);
    return out.data;
}

// (PC4 E2) `genko …` on a settings folder of its own: the exit code and its one JSON line.
std::pair<int, Json> cli_in(const QString& config, const QStringList& args) {
    QProcessEnvironment env;
    env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
    const auto ran = genko::test::run_genko(args, env);
    return {ran.exit_code, genko::test::one_line(ran.out)};
}

// (PC4 E2) a version 6 file of every part: a 16-bit sampled tip and an 8-bit one, a pattern, a brush of each section
// whose tip is in the file, one whose tip is not, one whose texture's pattern is not, a key nothing here takes.
std::string whole_v6() {
    namespace desc = render::psd::descriptor;
    render::abr::Sample deep;
    deep.id = "$deep";
    deep.width = 40;
    deep.height = 30;
    deep.depth = 16;
    for (std::uint32_t y = 0; y < deep.height; ++y)
        for (std::uint32_t x = 0; x < deep.width; ++x) {
            const int v = static_cast<int>((x * 1601 + y * 977) % 65536);  // (values no 8 bits keep)
            deep.data.push_back(static_cast<char>(v >> 8));
            deep.data.push_back(static_cast<char>(v & 0xff));
        }
    render::abr::Sample flat;
    flat.id = "$flat";
    flat.width = 12;
    flat.height = 12;
    for (int i = 0; i < 144; ++i) flat.data.push_back(static_cast<char>(i % 3 == 0 ? 255 : 0));
    render::pat::PsdPattern grit;
    grit.mode = 1;
    grit.width = 4;
    grit.height = 2;
    grit.name = "Grit";
    grit.id = "pat-uuid";
    grit.channels = {std::string("\x00\x40\x80\xff\xff\x80\x40\x00", 8)};
    const auto var = [](int control, double jitter) {
        desc::Writer w;
        w.integer("bVTy", control);
        w.integer("fStp", 25);
        w.unit("jitter", "#Prc", jitter);
        w.unit("Mnm ", "#Prc", 0.0);
        return w;
    };
    const auto tip = [](const std::u16string& id, double px) {
        desc::Writer t;
        t.unit("Dmtr", "#Pxl", px);
        t.unit("Angl", "#Ang", 30.0);
        t.unit("Rndn", "#Prc", 80.0);
        t.unit("Spcn", "#Prc", 40.0);
        t.boolean("Intr", true);
        t.text("sampledData", id);
        return t;
    };
    desc::Writer a;  // (16 bits, shape, scatter, texture of the file's pattern, transfer, wet edges, an unknown key)
    a.text("Nm  ", u"深いブラシ");
    a.object("Brsh", tip(u"$deep", 64.0), "sampledBrush");
    a.boolean("useTipDynamics", true);
    a.object("szVr", var(2, 20.0), "brVr");
    a.boolean("useScatter", true);
    a.integer("Cnt ", 2);
    a.object("scatterDynamics", var(0, 150.0), "brVr");
    a.boolean("useTexture", true);
    a.integer("textureBrightness", -20);
    a.integer("textureContrast", 10);
    {
        desc::Writer t;
        t.text("Nm  ", u"Grit");
        t.text("Idnt", u"pat-uuid");
        a.object("Txtr", t, "Ptrn");
    }
    a.boolean("usePaintDynamics", true);
    a.object("opVr", var(2, 0.0), "brVr");
    a.boolean("Wtdg", true);
    a.boolean("someFutureKey", true);
    desc::Writer b;  // (the 8-bit tip, a texture whose pattern is not in the file)
    b.text("Nm  ", u"点々");
    b.object("Brsh", tip(u"$flat", 12.0), "sampledBrush");
    b.boolean("useTexture", true);
    {
        desc::Writer t;
        t.text("Nm  ", u"Elsewhere");
        t.text("Idnt", u"other-uuid");
        b.object("Txtr", t, "Ptrn");
    }
    desc::Writer c;  // (a tip the file does not hold)
    c.text("Nm  ", u"Lost");
    c.object("Brsh", tip(u"$gone", 20.0), "sampledBrush");
    return render::abr::write_v6(2, {deep, flat}, {grit}, {{"brushPreset", a}, {"brushPreset", b}, {"brushPreset", c}}, true);
}

// (PC4 E2) a version 2 file: a computed (round) tip and a sampled one of 16 bits.
std::string computed_v2() {
    render::abr::LegacyBrush round;
    round.computed = true;
    round.spacing = 20;
    round.diameter = 30;
    round.hardness = 50;
    round.roundness = 60;
    round.angle = 45;
    render::abr::LegacyBrush deep;
    deep.name = "細かい点";
    deep.sample.width = 8;
    deep.sample.height = 8;
    deep.sample.depth = 16;
    for (int i = 0; i < 64; ++i) {
        deep.sample.data.push_back(static_cast<char>(i % 2 == 0 ? 0xff : 0x01));
        deep.sample.data.push_back(static_cast<char>(i % 2 == 0 ? 0xff : 0x23));
    }
    return render::abr::write_v12(2, {round, deep}, false);
}

const Json* brush_named(const Json& listed, const std::string& group, const std::string& name) {
    for (const Json& g : listed["groups"])
        if (g["name"] == group)
            for (const Json& b : g["brushes"])
                if (b["name"] == name) return &b;
    return nullptr;
}

bool has_warning(const Json& out, const std::string& part) {
    for (const Json& w : out.value("warnings", Json::array()))
        if (w.get<std::string>().find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

class TestContractAbr : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

    // A broken file cannot unpack without end. A record that points back where it began is refused, and the tips of one file unpack to at most so many pixels together
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

    // The library: none at first; brushes kept, one forgotten; a file that is not JSON an empty library, and never
    // written over.
    void libraryNeverWrittenOver() {
        const QString cpp_config = scratch_.path() + "/cpp-config";
        const std::filesystem::path config = std::filesystem::path(cpp_config.toStdString());
        QCOMPARE(render::brushes::load_library(config), Json::object());  // (none yet)
        render::brushes::save_to_library(config, "my_a", Json{{"label", "細い線"}, {"base", "gpen"}, {"width_mm", 0.35}, {"opacity", 0.8}, {"taper", true}});
        render::brushes::save_to_library(config, "my_b", Json{{"label", "B"}, {"base", "maru"}, {"width_mm", 1}, {"rgb", Json::array({255, 255, 255})}, {"spacing", 0.25}});
        render::brushes::save_to_library(config, "my_c", Json{{"label", "\"引用\"\n改行"}, {"base", "gpen"}, {"min_pressure", 1e-05}});
        render::brushes::save_to_library(config, "my_b", std::nullopt);
        // a file that is not JSON: an empty library; but never written over
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

    // PC4 E2 (brush.presets.importAbr, assets:adobe-presets; SPEC.md COLOR-09「ブラシのプリセット」): `genko brushes
    // import-abr` reads the whole file into the library — version 6's descriptors (each section), its 16-bit tips (kept
    // in 16 bits), its patterns as the brushes' textures, version 2's computed tips — and names in "warnings" what it did
    // not keep as it was; a file that is not one changes nothing.
    void importAbrWhole() {
        const QString config = scratch_.path() + "/e2-import";
        const QString v6 = scratch_.path() + "/画材.abr", v2 = scratch_.path() + "/丸.abr", broken = scratch_.path() + "/壊れ.abr";
        genko::test::write_bytes(v6, whole_v6());
        genko::test::write_bytes(v2, computed_v2());
        genko::test::write_bytes(broken, whole_v6().substr(0, 300));
        auto [code, out] = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("import-abr"), v6});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["group"].get<std::string>(), std::string("画材"));
        QCOMPARE(out["version"].get<int>(), 6);
        QCOMPARE(out["brushes"].size(), std::size_t(2));
        QVERIFY(has_warning(out, "「Lost」はサンプルの先端がファイルに入っていないので読み込みませんでした"));
        QVERIFY(has_warning(out, "テクスチャのパターン「Elsewhere」はファイルに入っていないので、紙の模様で代えました"));
        QVERIFY(has_warning(out, "someFutureKey"));
        auto [listed_code, listed] = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list"), QStringLiteral("--full")});
        QCOMPARE(listed_code, 0);
        const Json* deep = brush_named(listed, "画材", "深いブラシ");
        QVERIFY(deep != nullptr);
        const core::DabSettings d = core::dabs_from_json((*deep)["settings"]["dabs"]);
        QCOMPARE((*deep)["engine"].get<std::string>(), std::string("dabs"));
        QVERIFY(std::abs(d.size_mm - 64 * 25.4 / 200) < 1e-9);  // (Photoshop's pixels at 200 dpi)
        QVERIFY(std::abs(d.spacing - 0.4) < 1e-9);
        QVERIFY(std::abs(d.roundness - 0.8) < 1e-9);
        QVERIFY(std::abs(d.angle - 30) < 1e-9);
        QVERIFY(d.scatter.enabled);
        QVERIFY(d.texture.enabled && !d.texture.pattern.asset.empty());  // (the file's pattern)
        QVERIFY(d.wet_edges);
        // the 16-bit tip kept in 16 bits (its PNG's bit depth), by its hash in brush_tips/
        const std::string ref = d.tip;
        QVERIFY(!ref.empty());
        const auto tip_file = render::brushes::library_tip_path(std::filesystem::path(config.toStdString()), ref);
        const std::string png = genko::test::read_bytes(QString::fromStdString(tip_file.string()));
        QVERIFY(png.size() > 25);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(png[24])), 16);
        // `genko brushes get`: the library's, its every dab setting
        auto [got_code, got] = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("get"), QString::fromStdString((*deep)["key"].get<std::string>())});
        QCOMPARE(got_code, 0);
        QCOMPARE(got["from"].get<std::string>(), std::string("library"));
        QCOMPARE(core::dabs_from_json(got["dabs"]).tip, ref);
        // version 2: a computed tip as a round one of its hardness, roundness, angle and spacing; a 16-bit sampled one
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("import-abr"), v2, QStringLiteral("--group"), QStringLiteral("丸")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["version"].get<int>(), 2);
        std::tie(listed_code, listed) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list"), QStringLiteral("--full")});
        const Json* round = brush_named(listed, "丸", "円ブラシ 30 px");
        QVERIFY(round != nullptr);
        const core::DabSettings r = core::dabs_from_json((*round)["settings"]["dabs"]);
        QCOMPARE(r.tip, std::string());  // (round)
        QVERIFY(std::abs(r.hardness - 0.5) < 1e-9);
        QVERIFY(std::abs(r.roundness - 0.6) < 1e-9);
        QVERIFY(std::abs(r.angle - 45) < 1e-9);
        QVERIFY(std::abs(r.spacing - 0.2) < 1e-9);
        QVERIFY(std::abs(r.size_mm - 30 * 25.4 / 200) < 1e-9);
        QVERIFY(brush_named(listed, "丸", "細かい点") != nullptr);
        // read again with --replace: the group's brushes in their place, not twice
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("import-abr"), v2, QStringLiteral("--group"), QStringLiteral("丸"),
                                              QStringLiteral("--replace")});
        QCOMPARE(code, 0);
        std::tie(listed_code, listed) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list")});
        for (const Json& g : listed["groups"])
            if (g["name"] == "丸") QCOMPARE(g["brushes"].size(), std::size_t(2));
        // a broken file: refused, the library as it was
        const std::string before = genko::test::read_bytes(config + "/brushes.json");
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("import-abr"), broken});
        QVERIFY(code != 0);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(genko::test::read_bytes(config + "/brushes.json"), before);
    }

    // PC4 E2 (brush.presets.list・.save・.delete, brush.defineFromSelection): `genko brushes list | save | delete | edit |
    // define` on a settings folder of its own; the built-in brushes (Genko's and the 36 dab brushes) listed, never changed.
    void brushesCommandLine() {
        const QString config = scratch_.path() + "/e2-brushes";
        auto [code, out] = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list")});
        QCOMPARE(code, 0);
        std::size_t dab_builtins = 0;
        QStringList groups;
        for (const Json& g : out["groups"]) {
            groups << QString::fromStdString(g["name"].get<std::string>());
            if (g["builtin"].get<bool>() && g["name"] != "Genko") dab_builtins += g["brushes"].size();
        }
        QCOMPARE(groups.mid(0, 5), (QStringList{QStringLiteral("Genko"), QStringLiteral("一般"), QStringLiteral("ドライメディア"), QStringLiteral("ウェットメディア"),
                                               QStringLiteral("特殊効果")}));
        QCOMPARE(dab_builtins, std::size_t(36));
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("my_genko_chalk")});
        QCOMPARE(code, 0);
        QCOMPARE(out["from"].get<std::string>(), std::string("preset"));
        // save: a new key, its name and group; again under its key in its place
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("save"), QStringLiteral("--brush"),
                                              QStringLiteral("{\"label\": \"太い線\", \"base\": \"gpen\", \"width_mm\": 1.2}"), QStringLiteral("--group"), QStringLiteral("線")});
        QVERIFY2(code == 0, out.dump().c_str());
        const QString key = QString::fromStdString(out["key"].get<std::string>());
        QVERIFY(key.startsWith(QStringLiteral("my_")));
        QCOMPARE(out["group"].get<std::string>(), std::string("線"));
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("save"), key, QStringLiteral("--brush"),
                                              QStringLiteral("{\"label\": \"太い線\", \"base\": \"gpen\", \"width_mm\": 2}")});
        QCOMPARE(code, 0);
        QCOMPARE(out["key"].get<std::string>(), key.toStdString());
        QCOMPARE(out["group"].get<std::string>(), std::string("線"));
        // the file stays the one the window reads (a brush, its group)
        const Json kept = Json::parse(genko::test::read_bytes(config + "/brushes.json"));
        QCOMPARE(kept["brushes"][key.toStdString()]["width_mm"].get<double>(), 2.0);
        QCOMPARE(render::brushes::load_library(std::filesystem::path(config.toStdString()))[key.toStdString()]["label"].get<std::string>(), std::string("太い線"));
        // edit: renamed, moved to a new group
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("edit"),
                                              QStringLiteral("{\"action\": \"rename\", \"preset\": \"太い線\", \"name\": \"極太\"}")});
        QVERIFY2(code == 0, out.dump().c_str());
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("edit"), QStringLiteral("{\"action\": \"new_group\", \"name\": \"仕上げ\"}")});
        QCOMPARE(code, 0);
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("edit"),
                                              QStringLiteral("{\"action\": \"move\", \"preset\": \"極太\", \"to\": \"仕上げ\"}")});
        QCOMPARE(code, 0);
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list")});
        QVERIFY(brush_named(out, "仕上げ", "極太") != nullptr);
        // a built-in brush is not changed: refused, the file as it was
        const std::string before = genko::test::read_bytes(config + "/brushes.json");
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("delete"), QStringLiteral("my_genko_chalk")});
        QVERIFY(code != 0);
        QCOMPARE(out["ok"], Json(false));
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("edit"),
                                              QStringLiteral("{\"action\": \"rename\", \"preset\": \"チョーク\", \"name\": \"x\"}")});
        QVERIFY(code != 0);
        QCOMPARE(genko::test::read_bytes(config + "/brushes.json"), before);
        // define: a sampled tip from a book's paint within an area (8 × 6 mm: its larger side the brush's size)
        const QString book = scratch_.path() + "/e2-book.genko";
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const std::string black = core::b64encode(render::write_png(render::Image::create("RGBA", render::Size{1000, 1400}, render::Ink{0, 0, 0, 255})));
        const Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "ink2"}},
                                      Json{{"op", "set_layer"}, {"page", 1}, {"id", "ink2"}, {"panel_each", false}, {"panel_clip", false}},
                                      Json{{"op", "put_raster"}, {"page", 1}, {"id", "ink2"}, {"png_base64", black}}});
        genko::test::write_bytes(scratch_.path() + "/e2-ops.json", ops.dump());
        const auto applied = genko::test::run_genko({QStringLiteral("apply"), book, scratch_.path() + "/e2-ops.json", QStringLiteral("--agent"), QStringLiteral("human:test")});
        QVERIFY2(applied.exit_code == 0, qPrintable(applied.described()));
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("define"), book, QStringLiteral("--page"), QStringLiteral("1"),
                                              QStringLiteral("--rect"), QStringLiteral("10,10,8,6"), QStringLiteral("--layer"), QStringLiteral("ink2"),
                                              QStringLiteral("--name"), QStringLiteral("四角")});
        QVERIFY2(code == 0, out.dump().c_str());
        QVERIFY(std::abs(out["size_mm"].get<double>() - 8.0) < 0.3);
        QVERIFY(out["width"].get<int>() > out["height"].get<int>());
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("list")});
        QVERIFY(brush_named(out, "自分のブラシ", "四角") != nullptr);
        // delete by name
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("delete"), QStringLiteral("極太"), QStringLiteral("四角")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["deleted"].size(), std::size_t(2));
        QVERIFY(!Json::parse(genko::test::read_bytes(config + "/brushes.json"))["brushes"].contains(key.toStdString()));
    }

    // PC4 E2 (tool.presets.*, edit.presets.*): `genko tool-presets` and `genko presets` (list, edit, export, import,
    // migrate) on settings folders of their own.
    void presetsCommandLine() {
        const QString config = scratch_.path() + "/e2-presets", other = scratch_.path() + "/e2-other";
        auto [code, out] = cli_in(config, {QStringLiteral("tool-presets"), QStringLiteral("new"), QStringLiteral("--tool"), QStringLiteral("pen"), QStringLiteral("--name"),
                                           QStringLiteral("細ペン"), QStringLiteral("--options"), QStringLiteral("{\"brush\": \"gpen\", \"fields\": {\"brush_size\": 0.3}}")});
        QVERIFY2(code == 0, out.dump().c_str());
        std::tie(code, out) = cli_in(config, {QStringLiteral("tool-presets"), QStringLiteral("list"), QStringLiteral("--tool"), QStringLiteral("pen")});
        QCOMPARE(code, 0);
        bool found = false;
        for (const Json& p : out["presets"]) found = found || p["name"] == "細ペン";
        QVERIFY(found);
        std::tie(code, out) = cli_in(config, {QStringLiteral("brushes"), QStringLiteral("save"), QStringLiteral("--brush"),
                                              QStringLiteral("{\"label\": \"持ち出す線\", \"base\": \"maru\"}")});
        QCOMPARE(code, 0);
        // the manager's list: every kind, by its groups
        std::tie(code, out) = cli_in(config, {QStringLiteral("presets"), QStringLiteral("list")});
        QCOMPARE(code, 0);
        for (const char* kind : {"brushes", "gradients", "patterns", "shapes", "tool_presets"}) QVERIFY2(out.contains(kind) && out[kind].contains("groups"), kind);
        std::tie(code, out) = cli_in(config, {QStringLiteral("presets"), QStringLiteral("edit"), QStringLiteral("tool_presets"),
                                              QStringLiteral("{\"action\": \"rename\", \"preset\": \"細ペン\", \"name\": \"極細ペン\"}")});
        QVERIFY2(code == 0, out.dump().c_str());
        // export, then import into another folder; import again adds nothing (the same items)
        const QString file = scratch_.path() + "/e2.genkopresets";
        std::tie(code, out) = cli_in(config, {QStringLiteral("presets"), QStringLiteral("export"), file, QStringLiteral("--kinds"), QStringLiteral("brushes,tool_presets")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["presets"]["brushes"].get<int>(), 1);
        std::tie(code, out) = cli_in(other, {QStringLiteral("presets"), QStringLiteral("import"), file});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["added"]["brushes"].get<int>(), 1);
        QVERIFY(out["added"]["tool_presets"].get<int>() >= 1);
        std::tie(code, out) = cli_in(other, {QStringLiteral("presets"), QStringLiteral("import"), file});
        QCOMPARE(code, 0);
        QCOMPARE(out["added"]["brushes"].get<int>(), 0);
        std::tie(code, out) = cli_in(other, {QStringLiteral("tool-presets"), QStringLiteral("select"), QStringLiteral("極細ペン")});
        QCOMPARE(code, 0);
        QCOMPARE(out["options"]["fields"]["brush_size"].get<double>(), 0.3);
        // migrate from a settings folder: its groups not here yet
        const QString third = scratch_.path() + "/e2-third";
        std::tie(code, out) = cli_in(third, {QStringLiteral("presets"), QStringLiteral("migrate"), config});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["added"]["brushes"].get<int>(), 1);
        std::tie(code, out) = cli_in(third, {QStringLiteral("presets"), QStringLiteral("migrate"), config});
        QCOMPARE(out["added"]["brushes"].get<int>(), 0);
        // a file that is not one: refused
        genko::test::write_bytes(scratch_.path() + "/e2-not.json", "{\"x\": 1}");
        std::tie(code, out) = cli_in(third, {QStringLiteral("presets"), QStringLiteral("import"), scratch_.path() + "/e2-not.json"});
        QVERIFY(code != 0);
        QCOMPARE(out["ok"], Json(false));
    }
};

QTEST_GUILESS_MAIN(TestContractAbr)
#include "test_contract_abr.moc"
