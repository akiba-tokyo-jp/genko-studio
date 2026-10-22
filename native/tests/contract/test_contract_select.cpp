// PC3 B1 (SPEC.md COLOR-06「区域の種類」「クイックマスク」「マジック消しゴムツール」) through the command line, as a person or an
// agent uses it: `genko inspect --page N --area` (the area resolved, how much of the page it covers, its box: what an op
// given the same area works on), the area kinds this group writes given to ops (Color Range, Grow, Similar, the wand's
// colour on one layer), the op quick_mask in and out (saved with masks@1, undone and done again, the selection given back),
// the Quick Mask written into a PSD as its last extra channel and named by image resource 1022 (read back by psd-tools and
// by this build's reader), and magic_erase saved. PC3 B3: the smart kinds (the subject, the sky) through inspect --area
// and given to set_layer_mask (すべてのオブジェクトをマスク: saved, undone), the matting saved, and the same area on an 8-bit, a
// 16-bit and a CMYK page of one picture. (Python has none of these: this build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

unsigned be16(const std::string& b, std::size_t at) { return unsigned(static_cast<unsigned char>(b[at])) << 8 | static_cast<unsigned char>(b[at + 1]); }

// A PSD's image resources: id → data.
std::map<unsigned, std::string> psd_resources(const std::string& b) {
    std::map<unsigned, std::string> out;
    std::size_t at = 26;
    const auto be32 = [&](std::size_t p) { return std::uint32_t(be16(b, p)) << 16 | be16(b, p + 2); };
    at += 4 + be32(at);
    const std::size_t end = at + 4 + be32(at);
    for (std::size_t r = at + 4; r + 12 <= end;) {
        const unsigned id = be16(b, r + 4);
        const std::size_t data = r + 6 + ((static_cast<unsigned char>(b[r + 6]) + 2) & ~std::size_t(1));
        const std::size_t size = be32(data);
        out[id] = b.substr(data + 4, size);
        r = data + 4 + ((size + 1) & ~std::size_t(1));
    }
    return out;
}

// psd-tools' view of a PSD: its channel count and resource 1022's bytes.
constexpr const char* kPsdTools = R"PY(
import json, sys
from psd_tools import PSDImage
psd = PSDImage.open(sys.argv[2])
data = psd.image_resources.get_data(1022)
json.dump({"channels": psd.channels, "quick": list(data) if isinstance(data, (bytes, bytearray)) else None,
           "size": [psd.width, psd.height]}, open(sys.argv[1], "w"))
)PY";

}  // namespace

class TestContractSelect : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString write_ops(const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        return file;
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json inspect_area(const QString& book, const Json& area, int* exit = nullptr) {
        return genko({QStringLiteral("inspect"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--area"), QString::fromStdString(area.dump())},
                     exit);
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    genko::render::Image drawn(const QString& book, const QString& name) {
        const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                 QStringLiteral("20"), QStringLiteral("--out"), path(name)});
        if (ran.exit_code != 0) qWarning("%s", qPrintable(ran.described()));
        return genko::render::open_image(genko::test::read_bytes(path(name))).convert("RGB");
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a paint layer over page 1: red on the left half, white on the right
        std::string bytes;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x)
                for (const int v : x < 20 ? std::array<int, 4>{255, 0, 0, 255} : std::array<int, 4>{255, 255, 255, 255}) bytes.push_back(static_cast<char>(v));
        const genko::render::Image picture = genko::render::Image::frombytes("RGBA", genko::render::Size{40, 40}, bytes);
        Json done = apply(book, QStringLiteral("paint.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());

        // inspect --area: the reds (the left half), as an op given the same area works on it
        const Json reds{{"range", Json{{"select", "reds"}, {"layer", "pic"}}}};
        Json seen = inspect_area(book, reds, &exit);
        QVERIFY2(exit == 0, seen.dump().c_str());
        QVERIFY(seen["selected"].get<bool>());
        QVERIFY(seen["area"].contains("mask"));
        QVERIFY2(seen["coverage"].get<double>() > 0.4 && seen["coverage"].get<double>() < 0.6, seen.dump().c_str());
        const Json box = seen["box_mm"];
        QCOMPARE(box.size(), std::size_t{4});
        QVERIFY(box[0].get<double>() < 1 && box[1].get<double>() < 1 && std::abs(box[2].get<double>() - 105) < 8 && box[3].get<double>() > 290);
        // nothing there: not selected
        seen = inspect_area(book, Json{{"rect", Json::array({-20, -20, 5, 5})}}, &exit);
        QVERIFY2(exit == 0, seen.dump().c_str());
        QCOMPARE(seen, (Json{{"area", nullptr}, {"coverage", 0.0}, {"box_mm", nullptr}, {"selected", false}}));
        // an area that cannot be read: refused, said
        seen = inspect_area(book, Json{{"range", Json{{"select", "skin"}}}}, &exit);
        QVERIFY2(exit != 0, seen.dump().c_str());
        seen = inspect_area(book, reds, &exit);
        QVERIFY(exit == 0);
        int bad = -1;
        genko({QStringLiteral("inspect"), book, QStringLiteral("--area"), QString::fromStdString(reds.dump())}, &bad);
        QVERIFY(bad != 0);  // (--area needs --page)

        // the kinds given to ops: the reds filled green, Similar of a point of them, the wand's colour on the layer
        done = apply(book, QStringLiteral("fill.json"),
                     Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", reds}, {"rgb", Json::array({0, 200, 0})}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        genko::render::Image shown = drawn(book, QStringLiteral("filled.png"));
        QCOMPARE(shown.getpixel(40, 120), (std::vector<double>{0, 200, 0}));
        QCOMPARE(shown.getpixel(130, 120), (std::vector<double>{255, 255, 255}));
        const Json spot{{"rect", Json::array({40, 140, 4, 4})}};
        seen = inspect_area(book, Json{{"similar", Json{{"area", spot}, {"layer", "pic"}}}}, &exit);
        QVERIFY2(exit == 0 && seen["coverage"].get<double>() > 0.4 && seen["coverage"].get<double>() < 0.6, seen.dump().c_str());
        const double layer_wide = seen["coverage"].get<double>();
        // (on the page as it shows the green joined to the spot stops at the panel's border)
        seen = inspect_area(book, Json{{"grow", Json{{"area", spot}}}}, &exit);
        QVERIFY2(exit == 0 && seen["coverage"].get<double>() > 0.25 && seen["coverage"].get<double>() < layer_wide, seen.dump().c_str());
        QVERIFY2(seen["box_mm"][0].get<double>() > 5 && seen["box_mm"][1].get<double>() > 5, seen.dump().c_str());
        seen = inspect_area(book, Json{{"color", Json{{"x_mm", 150}, {"y_mm", 150}, {"layer", "pic"}, {"contiguous", true}}}}, &exit);
        QVERIFY2(exit == 0 && seen["coverage"].get<double>() > 0.4 && seen["coverage"].get<double>() < 0.6, seen.dump().c_str());

        // quick_mask: in (the selection white), saved with masks@1, undone and done again; out (the selection back)
        done = apply(book, QStringLiteral("quick.json"),
                     Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}, {"area", Json{{"rect", Json::array({20, 20, 100, 100})}}}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const std::string quick_id = done["results"][0]["channel"].get<std::string>();
        Json saved = project(book);
        const Json features = saved["features"];
        QVERIFY2(std::find(features.begin(), features.end(), Json("masks@1")) != features.end(), features.dump().c_str());
        QVERIFY(std::find(features.begin(), features.end(), Json("channels@1")) != features.end());
        QCOMPARE(saved["pages"][0]["channels"].size(), std::size_t{1});
        QCOMPARE(saved["pages"][0]["channels"][0]["quick_mask"], Json(true));
        QCOMPARE(saved["pages"][0]["channels"][0]["id"], Json(quick_id));
        Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        saved = project(book);
        QVERIFY(!saved["pages"][0].contains("channels"));
        QVERIFY(!saved.contains("features") || std::find(saved["features"].begin(), saved["features"].end(), Json("masks@1")) == saved["features"].end());
        Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        QCOMPARE(project(book)["pages"][0]["channels"][0]["quick_mask"], Json(true));
        // painted on as a channel (the pixel ops' channel): a white fill selects more
        done = apply(book, QStringLiteral("paint-quick.json"),
                     Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pic"}, {"channel", quick_id},
                                       {"area", Json{{"rect", Json::array({150, 200, 30, 30})}}}, {"rgb", Json::array({255, 255, 255})}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());

        // written into a PSD while in Quick Mask: its last extra channel, named by resource 1022 (its number, not empty)
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("20"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const Json psd_reply = genko::test::one_line(psd.out);
        const QString psd_file = QString::fromStdString(psd_reply["files"][0].get<std::string>());
        const std::string psd_bytes = genko::test::read_bytes(psd_file);
        QCOMPARE(be16(psd_bytes, 12), 4u);  // (RGB and the Quick Mask)
        const auto resources = psd_resources(psd_bytes);
        QVERIFY(resources.contains(1022));
        QCOMPARE(resources.at(1022), std::string("\x00\x03\x00", 3));
        const genko::render::psd::File file = genko::render::psd::File::read(psd_bytes);
        const auto extra = file.extra_channels();
        QCOMPARE(extra.size(), std::size_t{1});
        QVERIFY(extra[0].quick_mask);
        QCOMPARE(extra[0].name, std::string("クイックマスク"));
        if (genko::test::python_ref().isEmpty()) {
            qWarning("no reference Python (GENKO_PYREF): psd-tools' reading is not checked");
        } else {
            const auto ran = genko::test::run(genko::test::python_ref(),
                                              {QStringLiteral("-c"), QString::fromUtf8(kPsdTools), path(QStringLiteral("view.json")), psd_file},
                                              genko::test::python_env(path(QStringLiteral("pyenv"))));
            QVERIFY2(ran.exit_code == 0, ran.err.constData());
            const Json view = genko::test::read_json(path(QStringLiteral("view.json")));
            QCOMPARE(view["channels"], Json(4));
            QCOMPARE(view["quick"], Json::array({0, 3, 0}));
        }

        // out: the selection it says given back, the channel gone; undone: in Quick Mask again
        done = apply(book, QStringLiteral("out.json"), Json::array({Json{{"op", "quick_mask"}, {"page", 1}, {"on", false}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const Json back = done["results"][0]["area"];
        QVERIFY2(back.is_object() && back.contains("mask"), done.dump().c_str());
        QVERIFY(!project(book)["pages"][0].contains("channels"));
        seen = inspect_area(book, back, &exit);
        QVERIFY2(exit == 0, seen.dump().c_str());
        const double page_mm2 = 210.0 * 297.0;
        QVERIFY2(std::abs(seen["coverage"].get<double>() * page_mm2 - (100.0 * 100 + 30 * 30)) < 300, seen.dump().c_str());
        undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(project(book)["pages"][0]["channels"][0]["quick_mask"], Json(true));

        // magic_erase: the white of the layer made clear, saved
        done = apply(book, QStringLiteral("erase.json"),
                     Json::array({Json{{"op", "magic_erase"}, {"page", 1}, {"layer_id", "pic"}, {"x_mm", 180}, {"y_mm", 250}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(done["results"][0]["erased"], Json(true));
        seen = inspect_area(book, Json{{"transparency", "pic"}}, &exit);
        QVERIFY2(exit == 0 && seen["coverage"].get<double>() > 0.4 && seen["coverage"].get<double>() < 0.6, seen.dump().c_str());
    }

    // PC3 B3 (SPEC.md COLOR-06「賢い選択」): the smart kinds through the command line — inspect --area of the subject and the
    // sky, すべてのオブジェクトをマスク (set_layer_mask with the subject: saved with masks@1, undone), the matting saved — and
    // the same area on an 8-bit, a 16-bit and a CMYK page of one picture (the analysis is RGB).
    void smartKindsThroughTheCommandLine() {
        // a textured red disc (centre (105, 148) mm, radius 50) on a noisy blue ground, 2 mm a pixel over the A4 page
        const auto disc = [](int x, int y) {
            const double mx = (x + 0.5) * 2, my = (y + 0.5) * 2;
            const double n = 0.05 * std::sin(x * 12.9898 + y * 78.233) * std::cos(x * 4.1 - y * 2.7);
            std::array<double, 3> c{0.25 + n, 0.45 + n, 0.7 + n};
            if (std::hypot(mx - 105, my - 148) <= 50) {
                const double t = std::sin((x + y) * 0.35) * 0.5 + 0.5;
                c = {0.85 + 0.05 * t + n, 0.2 + 0.3 * t + n, 0.15 + n};
            }
            return c;
        };
        const auto sky = [](int, int y) {
            return y < 70 ? std::array<double, 3>{0.4 + y * 0.002, 0.6, 0.95} : std::array<double, 3>{0.35, 0.3, 0.12};
        };
        const auto made = [&](const QString& name, const std::function<std::array<double, 3>(int, int)>& at, const Json& mode) {
            const QString book = path(name);
            const auto ran = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
            if (ran.exit_code != 0) qWarning("%s", qPrintable(ran.described()));
            std::string bytes;
            for (int y = 0; y < 148; ++y)
                for (int x = 0; x < 105; ++x) {
                    for (const double v : at(x, y)) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0, 1.0) * 255)));
                    bytes.push_back(static_cast<char>(255));
                }
            const genko::render::Image picture = genko::render::Image::frombytes("RGBA", genko::render::Size{105, 148}, bytes);
            Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "写真"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}});
            if (!mode.is_null()) ops.push_back(mode);
            const Json done = apply(book, name + QStringLiteral(".json"), ops);
            if (!done.value("ok", false)) qWarning("%s", done.dump().c_str());
            return book;
        };
        const QString eight = made(QStringLiteral("eight.genko"), disc, Json());
        const QString deep = made(QStringLiteral("deep.genko"), disc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        const QString cmyk =
            made(QStringLiteral("cmyk.genko"), disc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
        int exit = -1;
        const Json subject{{"subject", Json{{"layer", "pic"}}}};
        const double disc_share = 3.14159265 * 50 * 50 / (210.0 * 297.0);
        Json seen[3];
        const QString books[3] = {eight, deep, cmyk};
        for (int i = 0; i < 3; ++i) {
            seen[i] = inspect_area(books[i], subject, &exit);
            QVERIFY2(exit == 0 && seen[i]["selected"].get<bool>(), seen[i].dump().c_str());
            QVERIFY2(std::abs(seen[i]["coverage"].get<double>() - disc_share) < 0.03, seen[i].dump().c_str());
        }
        for (int i = 1; i < 3; ++i) {
            QVERIFY2(std::abs(seen[i]["coverage"].get<double>() - seen[0]["coverage"].get<double>()) < 0.01, (seen[0].dump() + seen[i].dump()).c_str());
            for (std::size_t k = 0; k < 4; ++k)
                QVERIFY2(std::abs(seen[i]["box_mm"][k].get<double>() - seen[0]["box_mm"][k].get<double>()) < 3, (seen[0].dump() + seen[i].dump()).c_str());
        }
        // no sky on ground alone; a sky over ground found
        const QString ground = made(QStringLiteral("ground.genko"), [](int x, int y) {
            const double n = 0.15 * std::sin(x * 12.9898 + y * 78.233) * std::cos(x * 4.1 - y * 2.7);
            return std::array<double, 3>{0.35 + n, 0.3 + n, 0.12};
        }, Json());
        Json none = inspect_area(ground, Json{{"sky", Json{{"layer", "pic"}}}}, &exit);
        QVERIFY2(exit == 0 && !none["selected"].get<bool>(), none.dump().c_str());
        const QString outside = made(QStringLiteral("sky.genko"), sky, Json());
        Json found = inspect_area(outside, Json{{"sky", Json{{"layer", "pic"}}}}, &exit);
        QVERIFY2(exit == 0 && found["selected"].get<bool>() && found["box_mm"][1].get<double>() < 2 && found["box_mm"][3].get<double>() < 150,
                 found.dump().c_str());
        // すべてのオブジェクトをマスク: the subject as the layer's mask (linked: masks@1), saved; undone
        Json done = apply(eight, QStringLiteral("mask-all.json"),
                          Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "pic"}, {"area", subject}, {"linked", true}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const auto layer_in = [&](const Json& saved) {
            for (const Json& layer : saved["pages"][0]["layers"])
                if (layer.value("id", std::string()) == "pic") return layer;
            return Json();
        };
        Json saved = project(eight);
        QVERIFY2(layer_in(saved).contains("mask") && layer_in(saved)["mask"].value("linked", false), layer_in(saved).dump().c_str());
        QVERIFY(std::find(saved["features"].begin(), saved["features"].end(), Json("masks@1")) != saved["features"].end());
        Json undone = genko({QStringLiteral("undo"), eight, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QVERIFY(!layer_in(project(eight)).contains("mask"));
        // the matting, saved: an opaque layer has no fringe (nothing changed); the CMYK layer's too
        for (const QString& book : {eight, cmyk}) {
            done = apply(book, QStringLiteral("matte.json"), Json::array({Json{{"op", "matting"}, {"page", 1}, {"id", "pic"}, {"kind", "remove_black"}}}));
            QVERIFY2(done.value("ok", false) && done["results"][0]["changed"] == Json(false), done.dump().c_str());
        }
        done = apply(eight, QStringLiteral("matte-bad.json"), Json::array({Json{{"op", "matting"}, {"page", 1}, {"id", "pic"}, {"kind", "blur"}}}));
        QVERIFY2(done.contains("failed"), done.dump().c_str());
    }
};

QTEST_MAIN(TestContractSelect)
#include "test_contract_select.moc"
