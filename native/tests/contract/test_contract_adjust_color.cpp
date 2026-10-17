// PC2b (色の補正, SPEC.md COLOR-04) through the command line, as a person or an agent uses it: a book's pages of RGB 8
// bits, RGB 16 bits and CMYK given correction layers of the colour kinds and filter_raster's colour kinds (apply), saved
// in their normal form under adjustments@1 and read again, undone and done again, drawn (render, inspect --pixel),
// written as a PSD whose adjustment layers psd-tools (GENKO_PYREF) reads as Photoshop's — hue2, vibA, blnc, blwh, phfl,
// mixr, selc — and this build reads back (import_psd) as the same corrections. (Python has no such kinds: this build's
// own contract; a book without correction layers keeps Python's PSD bytes: test_contract_psd.)

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

// psd-tools' view of PSD files: each layer's kind, blend and its adjustment's values.
constexpr const char* kPsdTools = R"PY(
import json, sys
from psd_tools import PSDImage
out = {}
for f in sys.argv[2:]:
    layers = []
    for layer in PSDImage.open(f).descendants():
        e = {"name": layer.name, "kind": layer.kind, "blend": str(layer.blend_mode), "opacity": layer.opacity}
        k = layer.kind
        if k == "huesaturation":
            e.update(colorize=int(layer.enable_colorization), colorization=list(layer.colorization), master=list(layer.master),
                     ranges=[[list(r), list(s)] for r, s in layer.data])
        elif k == "vibrance":
            e.update(vibrance=layer.vibrance, saturation=layer.saturation)
        elif k == "colorbalance":
            e.update(shadows=list(layer.shadows), midtones=list(layer.midtones), highlights=list(layer.highlights), luminosity=int(layer.luminosity))
        elif k == "blackandwhite":
            e.update(weights=[int(w) for w in (layer.red, layer.yellow, layer.green, layer.cyan, layer.blue, layer.magenta)], tint=bool(layer.use_tint))
        elif k == "photofilter":
            e.update(color_space=int(layer.color_space), components=[int(c) for c in layer.color_components], density=int(layer.density),
                     luminosity=int(layer.luminosity))
        elif k == "channelmixer":
            e.update(monochrome=int(layer.monochrome), data=[int(x) for x in layer.data])
        elif k == "selectivecolor":
            e.update(method=int(layer.method), data=[[int(v) for v in x] for x in layer.data])
        layers.append(e)
    out[f] = layers
json.dump(out, open(sys.argv[1], "w"))
)PY";

const Json* layer_named(const Json& layers, const std::string& name) {
    for (const Json& l : layers)
        if (l.value("name", std::string()) == name) return &l;
    return nullptr;
}

}  // namespace

class TestContractAdjustColor : public QObject {
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
        return exit == 0 ? out : Json{{"failed", out}};
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    Json psd_tools(const std::vector<std::string>& files) {
        QStringList args{QStringLiteral("-c"), QString::fromUtf8(kPsdTools), path(QStringLiteral("view.json"))};
        for (const auto& f : files) args << QString::fromStdString(f);
        const auto ran = genko::test::run(genko::test::python_ref(), args, genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) qWarning("%s", ran.err.constData());
        return ran.exit_code == 0 ? genko::test::read_json(path(QStringLiteral("view.json"))) : Json();
    }
    // The page's correction layers' adjust, by id.
    static Json corrections(const Json& saved, std::size_t page) {
        Json out = Json::object();
        for (const Json& l : saved["pages"][page]["layers"])
            if (l.value("kind", std::string()) == "adjust") out[l["id"].get<std::string>()] = l["adjust"];
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("3")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a picture on each page (a red, a green, a blue and a grey band); page 2 RGB 16 bits, page 3 CMYK
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{80, 40}, genko::render::Ink{200, 40, 40, 255});
        picture.paste(genko::render::Ink{40, 180, 60, 255}, genko::render::Box{20, 0, 40, 40});
        picture.paste(genko::render::Ink{50, 70, 210, 255}, genko::render::Box{40, 0, 60, 40});
        picture.paste(genko::render::Ink{128, 128, 128, 255}, genko::render::Box{60, 0, 80, 40});
        Json paint = Json::array();
        for (int page = 1; page <= 3; ++page) {
            paint.push_back(Json{{"op", "add_layer"}, {"page", page}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}});
            paint.push_back(Json{{"op", "set_layer"}, {"page", page}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}});
            paint.push_back(Json{{"op", "put_raster"}, {"page", page}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}});
        }
        paint.push_back(Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}});
        paint.push_back(Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
        Json out = apply(book, QStringLiteral("paint.json"), paint);
        QVERIFY2(out.value("ok", false), out.dump().c_str());
        // the corrections: layers of each colour kind, and the filters
        const Json hs{{"kind", "hue_saturation"}, {"hue", 25}, {"saturation", -20}, {"lightness", 10},
                      {"reds", Json{{"hue", 10}, {"saturation", -30}, {"lightness", 5}, {"range", Json::array({300, 340, 20, 50})}}},
                      {"native", Json{{"blend", "multiply"}}}};
        const Json sc{{"kind", "selective_color"}, {"method", "absolute"}, {"reds", Json::array({10, 0, -20, 5})}, {"blacks", Json::array({0, 0, 0, 25})}};
        const Json bw{{"kind", "black_white"}, {"reds", 120}, {"blues", -40}, {"tint", true}, {"tint_color", Json::array({208, 160, 112})}};
        const Json pf{{"kind", "photo_filter"}, {"filter", "cooling80"}, {"density", 60}};
        const Json cm{{"kind", "channel_mixer"}, {"red", Json::array({80, 30, -10, 5})}, {"blue", Json::array({0, 10, 90, 0})}};
        const Json vb{{"kind", "vibrance"}, {"vibrance", 40}, {"saturation", -10}};
        const Json cb{{"kind", "color_balance"}, {"shadows", Json::array({10, -5, 0})}, {"midtones", Json::array({0, 20, -30})}, {"preserve_luminosity", false}};
        const auto layer = [](int page, const char* id, const Json& adjust) {
            return Json{{"op", "add_layer"}, {"page", page}, {"kind", "adjust"}, {"id", id}, {"name", id}, {"adjust", adjust}};
        };
        out = apply(book, QStringLiteral("corrections.json"),
                    Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "vibrance"}, {"vibrance", 40}},
                                 layer(1, "hs", hs), layer(1, "sc", sc), layer(1, "bw", bw), layer(2, "pf", pf), layer(2, "cm", cm), layer(2, "vb", vb),
                                 Json{{"op", "filter_raster"}, {"page", 3}, {"id", "pic"}, {"kind", "desaturate"}}, layer(3, "cb", cb)}));
        QVERIFY2(out.value("ok", false), out.dump().c_str());
        // saved: adjustments@1, each in its normal form
        const auto check = [&](const Json& saved) {
            bool named = false;
            for (const Json& f : saved.value("features", Json::array())) named = named || f == Json("adjustments@1");
            QVERIFY2(named, saved["features"].dump().c_str());
            const Json one = corrections(saved, 0), two = corrections(saved, 1), three = corrections(saved, 2);
            QCOMPARE(one["hs"]["reds"]["range"], Json::array({-60, -20, 20, 50}));
            QCOMPARE(one["hs"]["native"], (Json{{"blend", "multiply"}}));
            const Json magentas{{"hue", 0}, {"saturation", 0}, {"lightness", 0}, {"range", Json::array({255, 285, 315, 345})}};
            QVERIFY2(genko::test::same_content(one["hs"]["magentas"], magentas), one["hs"].dump().c_str());  // (saved: keys sorted)
            QCOMPARE(one["sc"]["greens"], Json::array({0, 0, 0, 0}));
            QCOMPARE(one["bw"]["yellows"], Json(60));
            QVERIFY2(genko::test::same_content(two["pf"], Json{{"kind", "photo_filter"}, {"color", Json::array({0, 109, 255})}, {"density", 60}, {"preserve_luminosity", true}}),
                     two["pf"].dump().c_str());
            QCOMPARE(two["cm"]["gray"], Json::array({40, 40, 20, 0}));
            QCOMPARE(three["cb"]["highlights"], Json::array({0, 0, 0}));
        };
        const Json saved = project(book);
        check(saved);
        // undone (the corrections, all at once) and done again
        Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        {
            const Json back = project(book);
            for (std::size_t page = 0; page < 3; ++page) QVERIFY(corrections(back, page).empty());
            for (const Json& f : back.value("features", Json::array())) QVERIFY(f != Json("adjustments@1"));
        }
        Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        check(project(book));
        // drawn: every page (nothing left out); the CMYK page's desaturated picture on K alone (the grey band's inks)
        for (int page = 1; page <= 3; ++page) {
            const QString png = path(QStringLiteral("p%1.png").arg(page));
            const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QString::number(page), QStringLiteral("--dpi"),
                                                       QStringLiteral("40"), QStringLiteral("--out"), png});
            QVERIFY2(drawn.exit_code == 0 && QFile::exists(png), qPrintable(drawn.described()));
        }
        const Json ink = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("3:1,1"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0 && ink["mode"] == Json("cmyk"), ink.dump().c_str());
        // written as a PSD: the corrections as Photoshop's adjustment layers
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const Json files = genko::test::one_line(psd.out)["files"];
        QCOMPARE(files.size(), std::size_t(3));
        if (genko::test::python_ref().isEmpty()) {
            qWarning("no reference Python (GENKO_PYREF): psd-tools' reading is not checked");
        } else {
            std::vector<std::string> list;
            for (const Json& f : files) list.push_back(f.get<std::string>());
            const Json view = psd_tools(list);
            QVERIFY2(view.is_object(), "psd-tools could not read the PSDs");
            const Json& p1 = view[list[0]];
            const Json* h = layer_named(p1, "hs");
            QVERIFY2(h != nullptr && (*h)["kind"] == Json("huesaturation"), p1.dump().c_str());
            QCOMPARE((*h)["master"], Json::array({25, -20, 10}));
            QCOMPARE((*h)["colorize"], Json(0));
            QCOMPARE((*h)["ranges"][0], Json::array({Json::array({300, 340, 20, 50}), Json::array({10, -30, 5})}));
            QVERIFY2((*h)["blend"].get<std::string>().find("MULTIPLY") != std::string::npos, (*h)["blend"].dump().c_str());
            const Json* s = layer_named(p1, "sc");
            QVERIFY(s != nullptr && (*s)["kind"] == Json("selectivecolor"));
            QCOMPARE((*s)["method"], Json(1));
            QCOMPARE((*s)["data"][1], Json::array({10, 0, -20, 5}));
            QCOMPARE((*s)["data"][9], Json::array({0, 0, 0, 25}));
            const Json* b = layer_named(p1, "bw");
            QVERIFY(b != nullptr && (*b)["kind"] == Json("blackandwhite"));
            QCOMPARE((*b)["weights"], Json::array({120, 60, 40, 60, -40, 80}));
            QCOMPARE((*b)["tint"], Json(true));
            const Json& p2 = view[list[1]];
            const Json* f = layer_named(p2, "pf");
            QVERIFY(f != nullptr && (*f)["kind"] == Json("photofilter"));
            QCOMPARE((*f)["color_space"], Json(0));
            QCOMPARE((*f)["components"], Json::array({0, 109 * 257, 65535, 0}));
            QCOMPARE((*f)["density"], Json(60));
            const Json* m = layer_named(p2, "cm");
            QVERIFY(m != nullptr && (*m)["kind"] == Json("channelmixer"));
            QCOMPARE((*m)["data"], Json::array({80, 30, -10, 0, 5}));
            const Json* v = layer_named(p2, "vb");
            QVERIFY(v != nullptr && (*v)["kind"] == Json("vibrance"));
            QCOMPARE((*v)["vibrance"], Json(40));
            QCOMPARE((*v)["saturation"], Json(-10));
            const Json* c = layer_named(view[list[2]], "cb");
            QVERIFY(c != nullptr && (*c)["kind"] == Json("colorbalance"));
            QCOMPARE((*c)["midtones"], Json::array({0, 20, -30}));
            QCOMPARE((*c)["luminosity"], Json(0));
        }
        // read back by this build: each PSD onto a page of another book, its adjustment layers the same corrections
        const QString other = path(QStringLiteral("other.genko"));
        QVERIFY(genko::test::run_genko({QStringLiteral("new"), other, QStringLiteral("--pages"), QStringLiteral("3")}).exit_code == 0);
        out = apply(other, QStringLiteral("modes.json"),
                    Json::array({Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}}));
        QVERIFY2(out.value("ok", false), out.dump().c_str());
        Json imports = Json::array();
        for (std::size_t page = 0; page < 3; ++page) {
            imports.push_back(Json{{"op", "import_psd"}, {"page", page + 1}, {"psd", genko::core::b64encode(genko::test::read_bytes(QString::fromStdString(files[page].get<std::string>())))}});
        }
        out = apply(other, QStringLiteral("imports.json"), imports);
        QVERIFY2(out.value("ok", false), out.dump().c_str());
        const Json read = project(other);
        const auto by_title = [&read](std::size_t page, const std::string& title) {
            for (const Json& l : read["pages"][page]["layers"])
                if (l.value("title", std::string()) == title && l.value("kind", std::string()) == "adjust") return l["adjust"];
            return Json();
        };
        const Json mine = project(book);
        for (const auto& [page, id] : std::vector<std::pair<std::size_t, std::string>>{{0, "hs"}, {0, "sc"}, {0, "bw"}, {1, "pf"}, {1, "cm"}, {1, "vb"}, {2, "cb"}}) {
            const Json theirs = by_title(page, id), ours = corrections(mine, page)[id];
            std::string where;
            QVERIFY2(genko::test::same_content(theirs, ours, &where), (id + ": " + where + " " + theirs.dump() + " / " + ours.dump()).c_str());
        }
    }
};

QTEST_GUILESS_MAIN(TestContractAdjustColor)
#include "test_contract_adjust_color.moc"
