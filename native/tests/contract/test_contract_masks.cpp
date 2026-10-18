// PC3 B2 (masks@1, SPEC.md COLOR-06「マスク」「範囲の中身の編集」; the user's decision 3) through the command line, as a
// person or an agent uses it: a layer mask's link, density and feather set, saved (the feature named only while used),
// drawn, undone and redone; apply_layer_mask and mask_from_transparency saved and undone; outline_area; the window's copy
// as ops (add_layer + paste, at center_mm). A PSD's masks: one of masks@1 written with Photoshop's flags and parameters —
// read by psd-tools (the reference environment's, an independent reader: GENKO_PYREF) and by this build —, imported back
// with them; a mask without them written as before (no parameters, its record of 20 bytes) and imported fixed to the page.
// (This build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace psd = genko::render::psd;

namespace {

constexpr const char* kPsdTools = R"PY(
import json, sys
from psd_tools import PSDImage
psd = PSDImage.open(sys.argv[2])
out = {"size": [psd.width, psd.height], "layers": {}}
for l in psd.descendants():
    data = l._record.mask_data
    if data is None:
        out["layers"][l.name] = None
        continue
    item = {"relative": bool(data.flags.pos_relative_to_layer), "disabled": bool(data.flags.mask_disabled),
            "parameters": bool(data.flags.parameters_applied)}
    if data.parameters is not None:
        item["density"] = data.parameters.user_mask_density
        item["feather"] = data.parameters.user_mask_feather
    out["layers"][l.name] = item
json.dump(out, open(sys.argv[1], "w"))
)PY";

const Json* layer_of(const Json& page, const std::string& id) {
    for (const Json& layer : page["layers"]) {
        if (layer.value("id", std::string()) == id) return &layer;
    }
    return nullptr;
}

bool has_feature(const Json& saved, const char* feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

// A picture of one colour, its left quarter clear.
std::string picture_png(int w, int h) {
    render::Image picture = render::Image::create("RGBA", render::Size{w, h}, render::Ink{200, 150, 100, 255});
    picture.paste(render::Ink{0, 0, 0, 0}, render::Box{0, 0, w / 4, h});
    return render::write_png(picture);
}

double share_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.tobytes(), y = b.tobytes();
    if (x.size() != y.size() || x.empty()) return 1.0;
    std::size_t apart = 0;
    for (std::size_t i = 0; i < x.size(); i += 3) {
        int most = 0;
        for (std::size_t c = 0; c < 3; ++c) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i + c])) - int(static_cast<unsigned char>(y[i + c]))));
        apart += most > 3 ? 1 : 0;
    }
    return static_cast<double>(apart) / static_cast<double>(x.size() / 3);
}

}  // namespace

class TestContractMasks : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString make_book(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko(QStringList{QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        return book;
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    bool restore(const QString& book, const char* what) {
        int exit = -1;
        const Json reply = genko({QString::fromLatin1(what), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) qWarning("%s", reply.dump().c_str());
        return exit == 0;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    render::Image drawn(const QString& book) {
        const QString out = path(QStringLiteral("drawn-%1.png").arg(drawings_++));
        const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                 QStringLiteral("30"), QStringLiteral("--mode"), QStringLiteral("proof"), QStringLiteral("--out"), out});
        if (ran.exit_code != 0) {
            qWarning("%s", qPrintable(ran.described()));
            return render::Image::create("RGB", render::Size{1, 1});
        }
        return render::open_image(genko::test::read_bytes(out), render::kPillowOpenLimits).convert("RGB");
    }
    bool exported(const QString& book, const QString& out) {
        const auto ran = genko::test::run_genko({QStringLiteral("export"), book, out, QStringLiteral("--format"), QStringLiteral("psd"), QStringLiteral("--dpi"),
                                                 QStringLiteral("72"), QStringLiteral("--json")});
        if (ran.exit_code != 0) qWarning("%s", qPrintable(ran.described()));
        return ran.exit_code == 0;
    }
    Json psd_tools(const QString& file) {
        const QString json = file + QStringLiteral(".json");
        const auto ran = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), QString::fromUtf8(kPsdTools), json, file},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) {
            qWarning("%s", ran.err.constData());
            return Json();
        }
        return genko::test::read_json(json);
    }
    // A book of one page with a paint layer "pic" over it (its left quarter clear).
    QString painted(const QString& name) {
        const QString book = make_book(name);
        const Json done = apply(book, name + QStringLiteral(".paint.json"),
                                Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                             Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                             Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(picture_png(80, 60))}}}));
        if (!done.value("ok", false)) qWarning("%s", done.dump().c_str());
        return book;
    }
    int drawings_ = 0;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // A mask's link, density and feather saved (masks@1 named only while a mask holds them), drawn, undone and redone;
    // apply_layer_mask and mask_from_transparency saved and undone; outline_area; the window's copy as ops.
    void masksThroughTheCommandLine() {
        const QString book = painted(QStringLiteral("masks.genko"));
        const render::Image plain = drawn(book);
        QVERIFY(!has_feature(project(book), "masks@1"));
        // (the right half hidden at a density of 0.6, feathered 2 mm)
        Json done = apply(book, QStringLiteral("mask.json"),
                          Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "pic"}, {"area", Json{{"rect", Json::array({0, 0, 105, 297})}}},
                                            {"linked", true}, {"density", 0.6}, {"feather_mm", 2.0}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book);
        QVERIFY(has_feature(saved, "masks@1"));
        const Json* pic = layer_of(saved["pages"][0], "pic");
        QVERIFY(pic != nullptr);
        QCOMPARE((*pic)["mask"]["linked"], Json(true));
        QCOMPARE((*pic)["mask"]["density"], Json(0.6));
        QCOMPARE((*pic)["mask"]["feather_mm"], Json(2.0));
        const render::Image masked = drawn(book);
        const auto right = masked.getpixel(masked.width() * 3 / 4, masked.height() / 2), left = masked.getpixel(masked.width() * 3 / 8, masked.height() / 2);
        QCOMPARE(left, plain.getpixel(masked.width() * 3 / 8, masked.height() / 2));  // (shown)
        QVERIFY2(right[0] > 210 && right[0] < 250, qPrintable(QString::number(right[0])));  // (hidden at 0.6: 40 % of the colour over the paper)
        QVERIFY(restore(book, "undo"));
        QVERIFY(!has_feature(project(book), "masks@1"));
        QVERIFY(layer_of(project(book)["pages"][0], "pic")->contains("mask") == false);
        QVERIFY(restore(book, "redo"));
        QCOMPARE((*layer_of(project(book)["pages"][0], "pic"))["mask"]["density"], Json(0.6));
        // applied: the mask gone, the page drawn as it was
        done = apply(book, QStringLiteral("apply.json"), Json::array({Json{{"op", "apply_layer_mask"}, {"page", 1}, {"id", "pic"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book);
        QVERIFY(!layer_of(saved["pages"][0], "pic")->contains("mask"));
        QVERIFY(!has_feature(saved, "masks@1"));
        QVERIFY2(share_apart(drawn(book), masked) < 0.05, qPrintable(QString::number(share_apart(drawn(book), masked))));
        QVERIFY(restore(book, "undo"));
        QVERIFY(layer_of(project(book)["pages"][0], "pic")->contains("mask"));
        // from the transparency (replacing the mask, its settings kept)
        done = apply(book, QStringLiteral("transparency.json"), Json::array({Json{{"op", "mask_from_transparency"}, {"page", 1}, {"id", "pic"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE((*layer_of(project(book)["pages"][0], "pic"))["mask"]["density"], Json(0.6));
        QVERIFY(restore(book, "undo"));
        // a band along an area's edge
        done = apply(book, QStringLiteral("outline.json"),
                     Json::array({Json{{"op", "outline_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({60, 60, 80, 80})}}},
                                       {"width_mm", 3.0}, {"rgb", Json::array({0, 0, 255})}, {"location", "outside"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE((*layer_of(project(book)["pages"][0], "pic"))["patches"].size(), std::size_t{1});
        // the window's copy as an agent sends it: a new layer and the items pasted at a point
        const Json stroke{{"points", Json::array({Json::array({10.0, 10.0}), Json::array({30.0, 20.0})})}, {"width_mm", 0.6}};
        done = apply(book, QStringLiteral("paste.json"),
                     Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "copy"}, {"name", "貼り付け"}},
                                  Json{{"op", "paste"}, {"page", 1}, {"layer_id", "copy"}, {"items", Json{{"strokes", Json::array({stroke})}}},
                                       {"center_mm", Json::array({100, 150})}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QVERIFY2(!done.contains("warnings") || done["warnings"].empty(), done.dump().c_str());  // (center_mm: the op's own key)
        QVERIFY(layer_of(project(book)["pages"][0], "copy") != nullptr);
    }

    // The user's decision 3: a PSD's masks of masks@1 written with Photoshop's flags and parameters (psd-tools reads them),
    // imported back with them; a mask without them as before.
    void psdMasks() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        const QString book = painted(QStringLiteral("psd.genko"));
        const Json done = apply(book, QStringLiteral("psd-masks.json"),
                                Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "plain"}, {"name", "そのまま"}},
                                             Json{{"op", "put_raster"}, {"page", 1}, {"id", "plain"}, {"png_base64", genko::core::b64encode(picture_png(40, 30))}},
                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "linked"}, {"name", "リンク"}},
                                             Json{{"op", "put_raster"}, {"page", 1}, {"id", "linked"}, {"png_base64", genko::core::b64encode(picture_png(40, 30))}},
                                             Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "pic"}, {"area", Json{{"rect", Json::array({0, 0, 105, 297})}}},
                                                  {"density", 0.5}, {"feather_mm", 3.0}},
                                             Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "plain"}, {"area", Json{{"rect", Json::array({0, 0, 105, 297})}}}},
                                             Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "linked"}, {"fill", "show"}, {"linked", true}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const QString file = path(QStringLiteral("masks.psd"));
        QVERIFY(exported(book, file));
        // psd-tools
        const Json view = psd_tools(file);
        QVERIFY2(view.is_object(), view.dump().c_str());
        const Json& layers = view["layers"];
        QVERIFY2(layers.contains("絵") && layers.contains("そのまま") && layers.contains("リンク"), layers.dump().c_str());
        const Json& pic = layers["絵"];
        QCOMPARE(pic["relative"], Json(true));  // (fixed to the page: not linked)
        QCOMPARE(pic["parameters"], Json(true));
        QCOMPARE(pic["density"], Json(128));
        const double feather = pic["feather"].get<double>();
        const double px_per_mm = view["size"][0].get<double>() / 210.0;
        QVERIFY2(std::abs(feather - 3.0 * px_per_mm) < 1e-6, qPrintable(QString::number(feather)));
        QCOMPARE(layers["そのまま"], (Json{{"relative", false}, {"disabled", false}, {"parameters", false}}));
        QCOMPARE(layers["リンク"]["relative"], Json(false));
        QCOMPARE(layers["リンク"]["parameters"], Json(true));
        QVERIFY(layers["リンク"]["density"].is_null() && layers["リンク"]["feather"].is_null());
        // this build's reader
        const psd::File read = psd::File::read(genko::test::read_bytes(file));
        int seen = 0;
        for (const psd::Layer& l : read.layers()) {
            if (l.name == "絵") {
                ++seen;
                QVERIFY(l.has_mask && l.mask_parameters && l.mask_unlinked && l.mask_density == 128);
                QVERIFY(l.mask_feather && std::abs(*l.mask_feather - 3.0 * px_per_mm) < 1e-6);
            } else if (l.name == "そのまま") {
                ++seen;
                QVERIFY(l.has_mask && !l.mask_parameters);
            }
        }
        QCOMPARE(seen, 2);
        // imported back: the masks of masks@1 with their settings, the other fixed to the page
        const QString back = make_book(QStringLiteral("back.genko"));
        const Json imported = apply(back, QStringLiteral("import.json"),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", file.toStdString()}, {"fit", "paper"}, {"id", "ps"}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        const Json saved = project(back);
        QVERIFY(has_feature(saved, "masks@1"));
        int found = 0;
        for (const Json& layer : saved["pages"][0]["layers"]) {
            const std::string title = layer.value("title", std::string());
            if (!layer.contains("mask")) continue;
            const Json& mask = layer["mask"];
            if (title == "絵") {
                ++found;
                QVERIFY(!mask.contains("linked"));
                QVERIFY2(std::abs(mask["density"].get<double>() - 128 / 255.0) < 1e-3, mask.dump().c_str());
                QVERIFY2(std::abs(mask["feather_mm"].get<double>() - 3.0) < 0.05, mask.dump().c_str());
            } else if (title == "そのまま") {
                ++found;
                QVERIFY2(!mask.contains("linked") && !mask.contains("density") && !mask.contains("feather_mm"), mask.dump().c_str());
            } else if (title == "リンク") {
                ++found;
                QCOMPARE(mask["linked"], Json(true));
            }
        }
        QCOMPARE(found, 3);
    }
};

QTEST_GUILESS_MAIN(TestContractMasks)
#include "test_contract_masks.moc"
