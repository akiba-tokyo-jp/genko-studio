// PC3 A3 (the user's decision 3: a PSD's smart objects are always read and written; SPEC.md COLOR-05「PSD のスマート
// オブジェクト」) through the command line: Genko's smart objects written as Photoshop's placed layers — read by psd-tools
// (the reference environment's, an independent reader: GENKO_PYREF) and by this build: placed layers, their contents' ids,
// the files embedded (a file's own bytes; a page's, a PSD of its layers), where their boxes land —; a PSD's placed layer
// imported as a smart object (Photoshop's picture shown, its file drawn once it changes, as upstream's
// smart_cmds/tests.rs:359-397), saved, undone and redone, and written back with the block and item it was read with.
// And a layer's style (layer-styles@1) in the page's PSD read by psd-tools as Photoshop's effects.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/psd_smart.hpp"
#include "render/raster.hpp"
#include "psd_build.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace psd = genko::render::psd;
namespace build = genko::test::psd_build;

namespace {

constexpr const char* kPsdTools = R"PY(
import hashlib, io, json, sys
from psd_tools import PSDImage
psd = PSDImage.open(sys.argv[2])
out = {"size": [psd.width, psd.height], "layers": []}
for l in psd.descendants():
    item = {"name": l.name, "kind": l.kind}
    if l.kind == "smartobject":
        so = l.smart_object
        item["uuid"] = so.unique_id
        item["link"] = so.kind
        item["filename"] = so.filename
        item["filetype"] = so.filetype
        item["sha256"] = hashlib.sha256(so.data).hexdigest()
        item["box"] = list(so.transform_box) if so.transform_box else None
        if so.is_psd():
            inner = PSDImage.open(io.BytesIO(so.data))
            item["inner"] = {"size": [inner.width, inner.height], "layers": [x.name for x in inner.descendants()]}
    out["layers"].append(item)
json.dump(out, open(sys.argv[1], "w"))
)PY";

std::string halves_png(int w, int h) {
    render::Image picture = render::Image::create("RGB", render::Size{w, h}, render::Ink{0, 255, 0});
    picture.paste(render::Ink{255, 0, 0}, render::Box{0, 0, w / 2, h});
    return render::write_png(picture);
}

const Json* layer_of(const Json& page, const std::string& id) {
    for (const Json& layer : page["layers"]) {
        if (layer.value("id", std::string()) == id) return &layer;
    }
    return nullptr;
}

}  // namespace

class TestContractPsdSmart : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString make_book(const QString& name, const QStringList& options) {
        const QString book = path(name);
        const auto made = genko::test::run_genko(QStringList{QStringLiteral("new"), book} + options);
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
    bool exported(const QString& book, const QString& out, int dpi) {
        const auto ran = genko::test::run_genko({QStringLiteral("export"), book, out, QStringLiteral("--format"), QStringLiteral("psd"), QStringLiteral("--dpi"),
                                                 QString::number(dpi), QStringLiteral("--json")});
        if (ran.exit_code != 0) qWarning("%s", qPrintable(ran.described()));
        return ran.exit_code == 0;
    }
    render::Image drawn(const QString& book, int dpi) {
        const QString out = path(QStringLiteral("drawn-%1.png").arg(drawings_++));
        const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                 QString::number(dpi), QStringLiteral("--mode"), QStringLiteral("proof"), QStringLiteral("--out"), out});
        if (ran.exit_code != 0) {
            qWarning("%s", qPrintable(ran.described()));
            return render::Image::create("RGB", render::Size{1, 1});
        }
        return render::open_image(genko::test::read_bytes(out), render::kPillowOpenLimits).convert("RGB");
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
    int drawings_ = 0;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Genko's smart objects as Photoshop's placed layers: psd-tools and this build read the same placed layers, ids, files
    // and boxes
    void smartObjectsWrittenAsPlacedLayers() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        const QString book = make_book(QStringLiteral("write.genko"), {QStringLiteral("--webtoon"), QStringLiteral("--pages"), QStringLiteral("1")});
        const std::string file = halves_png(60, 40);
        render::Image picture = render::Image::create("RGBA", render::Size{80, 60}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{30, 60, 200, 255}, render::Box{10, 10, 50, 40});
        const Json made = apply(book, QStringLiteral("make.json"),
                                Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(file)}, {"name", "leaf.png"}, {"new_id", "pic"},
                                                  {"scale", 300}},
                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}, {"name", "青い絵"}},
                                             Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                                             Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", genko::core::b64encode(render::write_png(picture))}},
                                             Json{{"op", "smart_convert"}, {"page", 1}, {"id", "paint"}, {"new_id", "wrap"}},
                                             Json{{"op", "smart_transform"}, {"page", 1}, {"id", "pic"}, {"matrix", Json::array({0, 1, -1, 0, 240, 160})}}}));
        QVERIFY2(made.value("ok", false), made.dump().c_str());
        const Json saved = project(book);
        const int dpi = 40;
        QVERIFY(exported(book, path(QStringLiteral("so.psd")), dpi));
        const Json view = psd_tools(path(QStringLiteral("so.psd")));
        QVERIFY2(view.is_object(), "psd-tools reads the PSD");
        Json pic, wrap;
        for (const Json& layer : view["layers"]) {
            if (layer["kind"] != Json("smartobject")) continue;
            if (layer["filename"] == Json("leaf.png")) pic = layer;
            else wrap = layer;
        }
        QVERIFY2(pic.is_object() && wrap.is_object(), view.dump().c_str());
        // the file's own bytes, embedded; its box turned a quarter where its matrix lays it
        QCOMPARE(pic["link"], Json("data"));
        QCOMPARE(pic["sha256"], Json(genko::core::bytes_ref(file).substr(7)));
        QCOMPARE(pic["filetype"], Json("png"));
        const Json& so = *layer_of(saved["pages"][0], "pic");
        const Json& contents = saved["smart_contents"][so["smart"]["contents"].get<std::string>()];
        const Json& m = so["smart"]["matrix"];
        const double k = 25.4 / contents["dpi"].get<double>(), px = dpi / 25.4;
        const std::array<std::pair<double, double>, 4> box{{{0, 0}, {60, 0}, {60, 40}, {0, 40}}};
        for (std::size_t i = 0; i < 4; ++i) {
            const double x = box[i].first * k, y = box[i].second * k;
            const double ex = (m[0].get<double>() * x + m[2].get<double>() * y + m[4].get<double>()) * px;
            const double ey = (m[1].get<double>() * x + m[3].get<double>() * y + m[5].get<double>()) * px;
            QVERIFY2(std::abs(pic["box"][2 * i].get<double>() - ex) < 1e-6 && std::abs(pic["box"][2 * i + 1].get<double>() - ey) < 1e-6, pic.dump().c_str());
        }
        // a page's contents: a PSD of its layers (no paper, no borders), at its own resolution
        QVERIFY(wrap["inner"].is_object());
        const Json& inner = wrap["inner"]["layers"];
        QVERIFY2(std::find(inner.begin(), inner.end(), Json("青い絵")) != inner.end(), wrap.dump().c_str());
        QVERIFY2(std::find(inner.begin(), inner.end(), Json("紙")) == inner.end() && std::find(inner.begin(), inner.end(), Json("ノンブル")) == inner.end(), wrap.dump().c_str());
        const Json& spec = saved["spec"];
        const double page_dpi = spec["dpi"].get<double>();
        QCOMPARE(wrap["inner"]["size"][0].get<int>(), static_cast<int>(std::lround(spec["width_mm"].get<double>() * page_dpi / 25.4)));
        QVERIFY(pic["uuid"] != wrap["uuid"]);
        // read by this build: the same placed layers and files
        const psd::File read = psd::File::read(genko::test::read_bytes(path(QStringLiteral("so.psd"))));
        int placed = 0;
        for (const psd::Layer& layer : read.layers()) {
            if (layer.kind != "smart") continue;
            ++placed;
            const auto linked = read.linked(layer.placed->uuid);
            QVERIFY(linked && linked->kind == "liFD");
            if (linked->name == "leaf.png") QCOMPARE(std::string(linked->data), file);
            else QCOMPARE(std::string(linked->data.substr(0, 4)), std::string("8BPS"));
        }
        QCOMPARE(placed, 2);
        // (written again: the same bytes)
        QVERIFY(exported(book, path(QStringLiteral("again.psd")), dpi));
        QCOMPARE(genko::test::read_bytes(path(QStringLiteral("again.psd"))), genko::test::read_bytes(path(QStringLiteral("so.psd"))));
    }

    // A PSD's placed layer imported: a smart object of the file the PSD holds, Photoshop's picture shown, saved and opened,
    // undone and redone; drawn from its file once it changes (upstream's psd_placed_layer_renders_from_lnk2_data); written
    // back with its block and item as they were while it is where it was
    void placedLayerImported() {
        // (the webtoon page, 80 × 400 mm, at 127 dpi: 400 × 2000 pixels exactly; the PSD's canvas the page's pixels)
        const QString book = make_book(QStringLiteral("read.genko"), {QStringLiteral("--webtoon"), QStringLiteral("--pages"), QStringLiteral("1")});
        Json saved = project(book);
        const double width_mm = saved["spec"]["width_mm"].get<double>(), height_mm = saved["spec"]["height_mm"].get<double>();
        const int dpi = 127;
        const int w = static_cast<int>(std::lround(width_mm * dpi / 25.4)), h = static_cast<int>(std::lround(height_mm * dpi / 25.4));
        QVERIFY(std::abs(w * 25.4 - width_mm * dpi) < 1e-6);  // (pixels exactly: the PSD placed on the page pixel for pixel)
        QVERIFY(std::abs(h * 25.4 - height_mm * dpi) < 1e-6);
        const std::string png = halves_png(10, 8);
        psd::PlacedOut out;
        out.uuid = "5f0c3e1a-7d2b-4c19-9e8f-2a6b0d4c1e77";
        out.placed = "placed";
        out.corners = {20, 30, 60, 30, 60, 62, 20, 62};
        out.width = 10;
        out.height = 8;
        out.resolution = 72;
        const std::string sold = psd::sold_block(out);
        const std::string framed = psd::linked_item(out.uuid, "in.png", "png ", png);
        build::File f;
        f.width = w;
        f.height = h;
        build::Layer layer;
        layer.name = "placed";
        layer.left = 20;
        layer.top = 30;
        layer.right = 60;
        layer.bottom = 62;
        layer.channels = {{-1, build::samples(40 * 32, 255, 8)}, {0, build::samples(40 * 32, 0, 8)}, {1, build::samples(40 * 32, 0, 8)}, {2, build::samples(40 * 32, 255, 8)}};
        layer.blocks = build::block("SoLd", sold);
        f.layers = {layer};
        f.global = build::block("lnk2", framed);
        f.merged = {build::samples(std::size_t(w) * h, 255, 8), build::samples(std::size_t(w) * h, 255, 8), build::samples(std::size_t(w) * h, 255, 8)};
        genko::test::write_bytes(path(QStringLiteral("in.psd")), build::bytes(f));
        const Json imported = apply(book, QStringLiteral("import.json"),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", path(QStringLiteral("in.psd")).toStdString()}, {"fit", "paper"},
                                                      {"id", "ps"}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        saved = project(book);
        const Json* so = layer_of(saved["pages"][0], "ps-1");
        QVERIFY2(so != nullptr && (*so)["kind"] == Json("smart"), saved["pages"][0].dump().c_str());
        QCOMPARE((*so)["smart"]["psd"]["block"], Json("SoLd"));
        const Json& contents = saved["smart_contents"][(*so)["smart"]["contents"].get<std::string>()];
        QCOMPARE(contents["format"], Json("png"));
        QCOMPARE(contents["psd"]["uuid"], Json(out.uuid));
        QVERIFY(std::find(saved["features"].begin(), saved["features"].end(), Json("smart-objects@1")) != saved["features"].end());
        // drawn: Photoshop's picture (blue)
        render::Image page = drawn(book, dpi);
        QCOMPARE(page.getpixel(30, 40), (std::vector<double>{0, 0, 255}));
        // written back: the block and the item as read
        QVERIFY(exported(book, path(QStringLiteral("back.psd")), dpi));
        const psd::File back = psd::File::read(genko::test::read_bytes(path(QStringLiteral("back.psd"))));
        const psd::Layer* placed = nullptr;
        for (const psd::Layer& l : back.layers())
            if (l.kind == "smart") placed = &l;
        QVERIFY(placed != nullptr);
        QCOMPARE(std::string(back.placed_block(*placed)), sold);
        QCOMPARE(psd::framed_item(back.linked(out.uuid)->item), framed);
        const Json view = psd_tools(path(QStringLiteral("back.psd")));
        bool seen = false;
        for (const Json& l : view["layers"]) {
            if (l["kind"] != Json("smartobject")) continue;
            seen = true;
            QCOMPARE(l["uuid"], Json(out.uuid));
            QCOMPARE(l["sha256"], Json(genko::core::bytes_ref(png).substr(7)));
        }
        QVERIFY2(seen, view.dump().c_str());
        // undone, gc (Redo still needs the block, the picture and the item: kept), redone
        const std::string raw = (*so)["smart"]["psd"]["raw"].get<std::string>(), shown = (*so)["smart"]["psd"]["shown"]["asset"].get<std::string>();
        const std::string item = contents["psd"]["item"].get<std::string>();
        QVERIFY(restore(book, "undo"));
        QVERIFY(layer_of(project(book)["pages"][0], "ps-1") == nullptr);
        int exit = -1;
        (void)genko({QStringLiteral("gc"), book}, &exit);
        QCOMPARE(exit, 0);
        const auto kept = [&](const std::string& ref, const char* suffix) {
            return QFile::exists(book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") +
                                 QString::fromStdString(ref.substr(7)) + QString::fromLatin1(suffix));
        };
        QVERIFY(kept(raw, ".bin") && kept(shown, ".png") && kept(item, ".bin"));
        QVERIFY(restore(book, "redo"));
        QCOMPARE(drawn(book, dpi).getpixel(30, 40), (std::vector<double>{0, 0, 255}));
        // changed (a smart filter: Photoshop's picture no longer is what it draws): drawn from the file the PSD held —
        // red on the left, green on the right (upstream's psd_placed_layer_renders_from_lnk2_data)
        const Json filtered = apply(book, QStringLiteral("filter.json"),
                                    Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "ps-1"}, {"kind", "blur"}, {"radius", 1}}}));
        QVERIFY2(filtered.value("ok", false), filtered.dump().c_str());
        saved = project(book);
        QVERIFY(!layer_of(saved["pages"][0], "ps-1")->at("smart").at("psd").contains("shown"));
        page = drawn(book, dpi);
        const std::vector<double> left = page.getpixel(28, 46), right = page.getpixel(55, 46);
        QVERIFY2(left[0] > 200 && left[1] < 60 && left[2] < 60, Json(left).dump().c_str());
        QVERIFY2(right[1] > 200 && right[0] < 60 && right[2] < 60, Json(right).dump().c_str());
        // (written: the same block, while it is where it was; a note that its filters are baked)
        const auto ran = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("filtered.psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QString::number(dpi), QStringLiteral("--json")});
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        const Json reply = genko::test::one_line(ran.out);
        QVERIFY2(reply.contains("notes") && reply["notes"].is_array(), ran.out.constData());
        bool said = false;
        for (const Json& note : reply["notes"]) said = said || note.get<std::string>().find("スマートフィルター") != std::string::npos;
        QVERIFY2(said, ran.out.constData());
        const psd::File filtered_psd = psd::File::read(genko::test::read_bytes(path(QStringLiteral("filtered.psd"))));
        for (const psd::Layer& l : filtered_psd.layers())
            if (l.kind == "smart") QCOMPARE(std::string(filtered_psd.placed_block(l)), sold);
    }

    // PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」; moved here from test_layer_styles's psdWrittenAndRead,
    // which Windows runs without the reference Python): a layer's style written into the page's PSD (lfx2, iOpa) read by
    // psd-tools (tools/migration/psd_effects_check.py) as Photoshop's effects — the layer's effects on, a drop shadow
    // (1.27 mm: 10 px at 200 dpi; opacity 50 %), a pattern overlay and a stroke (0.762 mm: 6 px), the fill opacity 128 of
    // 255. The same book as there: a grey square on a page of 2 inches at 200 dpi, its style.
    void layerStylesReadByPsdTools() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        namespace core = genko::core;
        constexpr double kInch = 25.4;
        constexpr int kSide = 400;
        core::Document doc = core::new_episode("スタイル", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        doc.edit_page(0).frames.clear();
        doc.edit_page(0).numero = false;
        {
            auto& p = doc.edit_page(0);
            p.layers.clear();
            core::Layer layer;
            layer.id = "paint";
            layer.title = "四角";
            layer.role = core::LayerRole::User;
            layer.kind = core::LayerKind::Raster;
            layer.panel_clip = false;
            std::string bytes;
            for (int y = 0; y < kSide; ++y)
                for (int x = 0; x < kSide; ++x)
                    bytes += (x >= 100 && x < 300 && y >= 100 && y < 300) ? std::string("\xC8\xC8\xC8\xFF", 4) : std::string(4, '\0');
            render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes));
            p.layers.push_back(layer);
        }
        std::string checker;  // (a 2 × 2 picture of red and blue)
        for (int i = 0; i < 4; ++i) checker += (i == 0 || i == 3) ? std::string("\xFF\x00\x00\xFF", 4) : std::string("\x00\x00\xFF\xFF", 4);
        const Json pattern{{"name", "チェック"}, {"png", core::b64encode(render::write_png(render::Image::frombytes("RGBA", render::Size{2, 2}, checker)))},
                           {"mode", "rgb"}, {"depth", 8}, {"dpi", 300}};
        const Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 0.762}, {"rgb", Json::array({255, 0, 0})}},
                                                  Json{{"kind", "drop_shadow"}, {"distance_mm", 1.27}, {"size_mm", 0.508}, {"opacity", 0.5}, {"rgb", Json::array({10, 20, 30})}},
                                                  Json{{"kind", "pattern_overlay"}, {"pattern", pattern}}})},
                         {"fill_opacity", 0.5},
                         {"blend_if", Json::array({Json{{"channel", "gray"}, {"this", Json::array({0, 10, 240, 250})}, {"under", Json::array({0, 0, 255, 255})}}})}};
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", style}}}), core::Actor("human:test"))
                  .doc;
        const QString file = path(QStringLiteral("styles.psd"));
        genko::formats::export_page_psd(doc, doc.page(0), core::path_from_utf8(file.toStdString()), 200);
        const QString answer = path(QStringLiteral("styles.json"));
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {QStringLiteral("-I"), genko::test::repo_root() + QStringLiteral("/tools/migration/psd_effects_check.py"), answer, file},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        const Json seen = genko::test::read_json(answer);
        Json layer;
        for (const Json& l : seen["layers"])
            if (!l["effects"].empty()) layer = l;
        QVERIFY2(layer.is_object(), seen.dump().c_str());
        QCOMPARE(layer["enabled"], Json(true));
        std::vector<std::string> kinds;
        for (const Json& e : layer["effects"]) kinds.push_back(e["kind"].get<std::string>());
        std::sort(kinds.begin(), kinds.end());
        QCOMPARE(kinds, (std::vector<std::string>{"DropShadow", "PatternOverlay", "Stroke"}));
        for (const Json& e : layer["effects"]) {
            if (e["kind"] == Json("DropShadow")) {
                QVERIFY2(std::abs(e["distance"].get<double>() - 10) < 0.6, e.dump().c_str());  // (1.27 mm at 200 dpi)
                QVERIFY2(std::abs(e["opacity"].get<double>() - 50) < 0.6, e.dump().c_str());
            }
            if (e["kind"] == Json("Stroke")) QVERIFY2(std::abs(e["size"].get<double>() - 6) < 0.6, e.dump().c_str());
        }
        QVERIFY2(std::abs(layer["fill_opacity"].get<double>() - 128) <= 1, layer.dump().c_str());
    }
};

QTEST_MAIN(TestContractPsdSmart)
#include "test_contract_psd_smart.moc"
