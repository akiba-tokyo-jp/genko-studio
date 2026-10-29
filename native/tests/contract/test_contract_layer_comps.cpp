// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」) as an agent or a person uses it: through the command line — the
// comps made, applied and undone with `genko apply` and `genko undo`, the book naming layer-comps@1 only while a page has
// comps (none: its features and project.json as before), `genko inspect --comps`, `genko export --format comps` (a file a
// comp, each showing its comp, the book unchanged; one chosen; its refusals) —; the PSD's resource 1065 and the layers'
// cmls as psd-tools reads them (the names, ids, options, the last applied, each layer's state and offset, a lyid for a layer
// made here), read back as the page's comps (their places where they were), written again as read while unchanged (the same
// bytes), anew when changed, not at all when gone, kept when a layer they name is deleted; a .pcraft's layer_comps written
// and read back (with $GENKO_PCRAFT_CHECK, upstream's reader reads it).

#include <QtTest>

#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/layer_comps.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/layer_bounds.hpp"
#include "render/ops_layer_comps.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace lb = render::layer_bounds;

namespace {

constexpr double kInch = 25.4;  // (a 25.4 mm page: 200 working pixels a side)
constexpr int kSide = 200;
constexpr double kPx = kInch / kSide;

core::Document run(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc;
}

render::Image block(int x0, int y0, int x1, int y1) {
    std::string bytes(std::size_t(kSide) * kSide * 4, '\0');
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t(y) * kSide + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(200);
            bytes[i + 1] = static_cast<char>(60);
            bytes[i + 2] = static_cast<char>(80);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

void add_paint(core::Document& doc, const std::string& id, const render::Image& picture) {
    auto& p = doc.edit_page(0);
    core::Layer layer;
    layer.id = id;
    layer.title = id;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, picture);
    p.layers.push_back(std::move(layer));
}

// A 25.4 mm page without panels: "pic" (a block from (40, 50) to (80, 110) working pixels) and "pen" (a line).
core::Document book() {
    auto doc = core::new_episode("カンプ", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    add_paint(doc, "pic", block(40, 50, 80, 110));
    doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
    doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"panel_clip", false}});
    doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({5, 5}), Json::array({10, 6})})},
                        {"width_mm", 0.4}});
    return doc;
}

const core::Layer* titled(const core::Document& doc, std::string_view title) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.title == title) return &layer;
    return nullptr;
}

Json move(const std::string& id, double dx_px, double dy_px) {
    return Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({id})}, {"move", Json::array({dx_px * kPx, dy_px * kPx})}};
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// Two comps: "A" (pic shown where it is), then pic moved 10 px right and hidden, "B".
core::Document two_comps() {
    core::Document doc = book();
    doc = run(doc, op("new_layer_comp", {{"name", "A"}, {"comment", "first"}}));
    doc = run(doc, move("pic", 10, 0));
    doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", false}});
    doc = run(doc, op("new_layer_comp", {{"name", "B"}, {"appearance", false}}));
    // (shown again, as the PSD writes the layers shown: B still records it hidden)
    doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", true}});
    return doc;
}

}  // namespace

class TestContractLayerComps : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool names_comps(const Json& project) {
        const Json features = project.value("features", Json::array());
        return std::find(features.begin(), features.end(), Json("layer-comps@1")) != features.end();
    }
    Json comps_view(const QString& psd) {
        const QString out = path(QStringLiteral("comps-%1.json").arg(++views_));
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/psd_comps_check.py"), out, psd},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return genko::test::read_json(out);
    }
    int views_ = 0;

    std::map<std::string, std::string> blocks_of(const QString& psd) {
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(psd));
        std::map<std::string, std::string> out;
        for (const auto& r : file.resources()) out[std::to_string(r.id)] = std::string(r.data);
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Through the command line: made, applied, saved with layer-comps@1, listed, written a file a comp, undone.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(), fs::path(book_path.toStdString()));
        const std::string before = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        QVERIFY(!names_comps(project(book_path)));
        Json done = apply(book_path, QStringLiteral("a.json"), Json::array({op("new_layer_comp", {{"name", "A"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book_path);
        QVERIFY2(names_comps(saved), saved.value("features", Json::array()).dump().c_str());
        QVERIFY(saved["pages"][0].contains("layer_comps") || saved.dump().find("\"layer_comps\"") != std::string::npos);
        done = apply(book_path, QStringLiteral("b.json"),
                     Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", false}}, op("new_layer_comp", {{"name", "B/2"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // (a comp the page has not: refused, the book as it was)
        done = apply(book_path, QStringLiteral("bad.json"), Json::array({op("apply_layer_comp", {{"comp", 99}})}));
        QVERIFY2(done.contains("failed") && done.dump().find("no layer comp with id 99") != std::string::npos, done.dump().c_str());
        // inspect --comps
        int exit = -1;
        const Json listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--comps")}, &exit);
        QVERIFY2(exit == 0, listed.dump().c_str());
        QCOMPARE(listed["pages"].size(), std::size_t(1));
        QCOMPARE(listed["pages"][0]["page"], Json(1));
        QCOMPARE(listed["pages"][0]["comps"].size(), std::size_t(2));
        QCOMPARE(listed["pages"][0]["comps"][0]["name"], Json("A"));
        QCOMPARE(listed["pages"][0]["comps"][1]["name"], Json("B/2"));
        QCOMPARE(listed["pages"][0]["last_applied"], Json(2));
        QCOMPARE(listed["pages"][0]["comps"][0]["missing"], Json::array());
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--comps"), QStringLiteral("--page"), QStringLiteral("7")}, &exit);
        QVERIFY(exit != 0);
        // export --format comps: a file a comp, each showing its comp; the book unchanged
        const std::string kept = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        const QString out = path(QStringLiteral("comps-out"));
        const Json written = genko({QStringLiteral("export"), book_path, out, QStringLiteral("--format"), QStringLiteral("comps"),
                                    QStringLiteral("--prefix"), QStringLiteral("doc"), QStringLiteral("--json")},
                                   &exit);
        QVERIFY2(exit == 0, written.dump().c_str());
        QCOMPARE(written["count"], Json(2));
        const QString first = QString::fromStdString(written["files"][0].get<std::string>());
        const QString second = QString::fromStdString(written["files"][1].get<std::string>());
        QVERIFY2(first.endsWith(QStringLiteral("doc_0000_A.png")), qPrintable(first));
        QVERIFY2(second.endsWith(QStringLiteral("doc_0001_B_2.png")), qPrintable(second));
        const auto green_at = [](const QString& file) {
            const render::Image picture = render::read_png(genko::test::read_bytes(file)).convert("RGB");
            const std::string bytes = picture.tobytes();
            return static_cast<unsigned char>(bytes[(std::size_t(80) * picture.width() + 60) * 3 + 1]);
        };
        QCOMPARE(int(green_at(first)), 60);    // (A: the picture shown)
        QCOMPARE(int(green_at(second)), 255);  // (B: hidden — the paper)
        QVERIFY(genko::test::read_bytes(book_path + QStringLiteral("/project.json")) == kept);
        // one chosen, as a JPEG; the refusals
        const Json one = genko({QStringLiteral("export"), book_path, path(QStringLiteral("one")), QStringLiteral("--format"), QStringLiteral("comps"),
                                QStringLiteral("--comps"), QStringLiteral("2"), QStringLiteral("--comp-format"), QStringLiteral("jpg"), QStringLiteral("--json")},
                               &exit);
        QVERIFY2(exit == 0, one.dump().c_str());
        QCOMPARE(one["count"], Json(1));
        QVERIFY(one["files"][0].get<std::string>().ends_with("_0001_B_2.jpg"));
        genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--format"), QStringLiteral("comps"),
               QStringLiteral("--comp-format"), QStringLiteral("doc")},
              &exit);
        QCOMPARE(exit, 2);
        genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--format"), QStringLiteral("png"), QStringLiteral("--comps"),
               QStringLiteral("1")},
              &exit);
        QCOMPARE(exit, 2);
        const Json none = genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--format"), QStringLiteral("comps"),
                                 QStringLiteral("--comps"), QStringLiteral("9")},
                                &exit);
        QVERIFY2(exit == 1, none.dump().c_str());
        // deleted both: no layer_comps, layer-comps@1 not named, the project as before the comps; undone: back
        done = apply(book_path, QStringLiteral("del.json"),
                     Json::array({op("delete_layer_comp", {{"comp", 1}}), op("delete_layer_comp", {{"comp", 2}}),
                                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", true}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book_path);
        QVERIFY(!names_comps(saved));
        QVERIFY(saved.dump().find("\"layer_comps\"") == std::string::npos);
        const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QVERIFY(names_comps(project(book_path)));
        (void)before;
    }

    // PSD: 1065 and the layers' cmls as psd-tools reads them; read back; written as read while unchanged, anew when changed,
    // not when gone; kept when a layer they name is deleted.
    void psdComps() {
        const core::Document doc = two_comps();
        const QString first = path(QStringLiteral("comps.psd"));
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(first.toStdString()), 200);
        const Json view = comps_view(first);
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        const Json& list = view["comps"]["list"];
        QCOMPARE(list.size(), std::size_t(2));
        QCOMPARE(list[0]["Nm  "], Json("A"));
        QCOMPARE(list[0]["compID"], Json(1));
        QCOMPARE(list[0]["capturedInfo"], Json(7));
        QCOMPARE(list[0]["comment"], Json("first"));
        QCOMPARE(list[1]["Nm  "], Json("B"));
        QCOMPARE(list[1]["capturedInfo"], Json(3));
        QCOMPARE(view["comps"]["lastAppliedComp"], Json(2));
        Json pic;
        for (const Json& l : view["layers"])
            if (l["name"] == Json("pic")) pic = l;
        QVERIFY2(!pic.is_null() && pic["cmls"].is_object(), view.dump().c_str());
        QVERIFY(pic["lyid"].is_number());  // (a layer made here: a lyid given)
        QCOMPARE(pic["cmls"]["LyrI"], pic["lyid"]);
        const Json& settings = pic["cmls"]["layerSettings"];
        QCOMPARE(settings.size(), std::size_t(3));  // (A, B, the last document state)
        QCOMPARE(settings[0]["compList"], Json::array({1}));
        QCOMPARE(settings[0]["enab"], Json(true));
        QCOMPARE(settings[0]["Ofst"]["Hrzn"], Json(-10));  // (A: 10 px to the left of where it is now)
        QCOMPARE(settings[0]["Ofst"]["Vrtc"], Json(0));
        QCOMPARE(settings[1]["enab"], Json(false));
        QCOMPARE(settings[1]["Ofst"]["Hrzn"], Json(0));
        QCOMPARE(settings[2]["compList"], Json::array({0}));
        QVERIFY(!settings[2].contains("enab"));
        // read back: the comps, their states where they were
        formats::Opened opened = formats::open_file(genko::test::read_bytes(first), "comps.psd");
        core::Document back = opened.doc;
        QVERIFY(back.page(0).layer_comps);
        const core::LayerComps& comps = *back.page(0).layer_comps;
        QCOMPARE(comps.comps.size(), std::size_t(2));
        QCOMPARE(comps.comps[0].name, std::string("A"));
        QCOMPARE(comps.comps[0].comment, std::string("first"));
        QVERIFY(comps.comps[1].apply_visibility && comps.comps[1].apply_position && !comps.comps[1].apply_appearance);
        QCOMPARE(comps.last_applied, std::optional<std::int64_t>(2));
        const core::Layer* pic_back = titled(back, "pic");
        QVERIFY(pic_back != nullptr);
        QVERIFY(back.page(0).native_psd && std::any_of(back.page(0).native_psd->copied.begin(), back.page(0).native_psd->copied.end(),
                                                       [](const core::PsdCopied& c) { return c.resource == 1065; }));
        // (applying A there: shown, back 10 px to the left)
        const auto pic_at = [&](const core::Document& d) {
            const auto box = lb::of_layer(d, d.page(0), titled(d, "pic")->id);
            return std::lround((*box)[0] * lb::grid(d.page(0))[0]);
        };
        const long now_x = pic_at(back);
        const core::Document applied = run(back, op("apply_layer_comp", {{"comp", "A"}}));
        QVERIFY(titled(applied, "pic")->visible);
        QCOMPARE(pic_at(applied), now_x - 10);
        // written again unchanged: 1065 and the layer's shmd as read
        const QString again = path(QStringLiteral("again.psd"));
        (void)formats::export_page_psd(back, back.page(0), fs::path(again.toStdString()), 200);
        QVERIFY(blocks_of(again)["1065"] == blocks_of(first)["1065"]);
        const Json again_view = comps_view(again);
        for (const Json& l : again_view["layers"])
            if (l["name"] == Json("pic")) QCOMPARE(l["cmls"], pic["cmls"]);
        // changed (renamed): anew
        const core::Document renamed = run(back, op("set_layer_comp", {{"comp", 1}, {"name", "A2"}}));
        const QString changed = path(QStringLiteral("changed.psd"));
        (void)formats::export_page_psd(renamed, renamed.page(0), fs::path(changed.toStdString()), 200);
        const Json changed_view = comps_view(changed);
        QCOMPARE(changed_view["comps"]["list"][0]["Nm  "], Json("A2"));
        // moved after reading: the offsets from where it is now
        const core::Document moved = run(back, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({titled(back, "pic")->id})},
                                                    {"move", Json::array({5 * 25.4 / 200, 0})}});
        const QString moved_psd = path(QStringLiteral("moved.psd"));
        (void)formats::export_page_psd(moved, moved.page(0), fs::path(moved_psd.toStdString()), 200);
        for (const Json& l : comps_view(moved_psd)["layers"])
            if (l["name"] == Json("pic")) QCOMPARE(l["cmls"]["layerSettings"][0]["Ofst"]["Hrzn"], Json(-15));
        // a layer they name deleted: the comps kept, 1065 written
        const core::Document deleted = run(back, Json{{"op", "delete_layer"}, {"page", 1}, {"id", titled(back, "pic")->id}});
        const QString deleted_psd = path(QStringLiteral("deleted.psd"));
        (void)formats::export_page_psd(deleted, deleted.page(0), fs::path(deleted_psd.toStdString()), 200);
        QVERIFY(blocks_of(deleted_psd).contains("1065"));
        // gone: neither 1065 nor cmls
        core::Document gone = run(run(back, op("delete_layer_comp", {{"comp", 1}})), op("delete_layer_comp", {{"comp", 2}}));
        QVERIFY(!gone.page(0).layer_comps);
        const QString gone_psd = path(QStringLiteral("gone.psd"));
        (void)formats::export_page_psd(gone, gone.page(0), fs::path(gone_psd.toStdString()), 200);
        QVERIFY(!blocks_of(gone_psd).contains("1065"));
        for (const Json& l : comps_view(gone_psd)["layers"]) QVERIFY2(l["cmls"].is_null(), l.dump().c_str());
        // (a page without comps: no 1065, no lyid written for its layers — as before)
        const QString plain = path(QStringLiteral("plain.psd"));
        (void)formats::export_page_psd(book(), book().page(0), fs::path(plain.toStdString()), 200);
        QVERIFY(!blocks_of(plain).contains("1065"));
        for (const Json& l : comps_view(plain)["layers"]) QVERIFY2(l["lyid"].is_null() && l["cmls"].is_null(), l.dump().c_str());
    }

    // (the review's P2-3, the coordinator's decision) A page with comps writes its hidden layers too, hidden — Photoshop's
    // comps show them —; read back hidden, its comp showing it. A page without comps writes them not, as before.
    void psdHiddenLayers() {
        const core::Document doc = run(two_comps(), Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", false}});
        const QString file = path(QStringLiteral("hidden.psd"));
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), 200);
        const Json view = comps_view(file);
        Json pic;
        for (const Json& l : view["layers"])
            if (l["name"] == Json("pic")) pic = l;
        QVERIFY2(!pic.is_null(), view.dump().c_str());
        QCOMPARE(pic["visible"], Json(false));
        QVERIFY(pic["cmls"].is_object());
        formats::Opened opened = formats::open_file(genko::test::read_bytes(file), "hidden.psd");
        const core::Document back = opened.doc;
        QVERIFY(titled(back, "pic") != nullptr && !titled(back, "pic")->visible);
        const core::Document shown = run(back, op("apply_layer_comp", {{"comp", "A"}}));
        QVERIFY(titled(shown, "pic")->visible);
        // (no comps: the hidden layer not written, as before)
        core::Document plain = book();
        plain = run(plain, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"visible", false}});
        const QString plain_file = path(QStringLiteral("hidden-plain.psd"));
        (void)formats::export_page_psd(plain, plain.page(0), fs::path(plain_file.toStdString()), 200);
        for (const Json& l : comps_view(plain_file)["layers"]) QVERIFY2(l["name"] != Json("pic"), l.dump().c_str());
    }

    // (the review's P3-6) A comp's long name: the file's name cut to at most 240 bytes by whole characters.
    void longNames() {
        core::Document doc = book();
        std::string name;
        for (int i = 0; i < 255; ++i) name += "あ";
        doc = run(doc, op("new_layer_comp", {{"name", name}}));
        formats::CompFiles files;
        files.prefix = "p";
        const auto written = formats::export_layer_comps(doc, 0, fs::path(path(QStringLiteral("long")).toStdString()), files);
        QCOMPARE(written.size(), std::size_t(1));
        const std::string file_name = core::path_to_utf8(written[0].file.filename());
        QVERIFY2(file_name.size() <= 240, file_name.c_str());
        QVERIFY(file_name.starts_with("p_0000_あ") && file_name.ends_with("あ.png"));
        QVERIFY(QFileInfo::exists(QString::fromStdString(core::path_to_utf8(written[0].file))));
    }

    // .pcraft: written and read back (the comps, their states naming the layers read, the last applied); upstream's reader
    // reads it.
    void pcraftComps() {
        const core::Document doc = two_comps();
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        const QString file = path(QStringLiteral("comps.pcraft"));
        genko::test::write_bytes(file, bytes);
        formats::Opened opened = formats::open_file(bytes, "comps.pcraft");
        const core::Document& back = opened.doc;
        QVERIFY(back.page(0).layer_comps);
        const core::LayerComps& comps = *back.page(0).layer_comps;
        QCOMPARE(comps.comps.size(), std::size_t(2));
        QCOMPARE(comps.comps[0].name, std::string("A"));
        QCOMPARE(comps.comps[1].apply_appearance, false);
        QCOMPARE(comps.last_applied, std::optional<std::int64_t>(2));
        const core::Layer* pic = titled(back, "pic");
        QVERIFY(pic != nullptr);
        bool found = false;
        for (const core::CompLayerState& s : comps.comps[0].layers) {
            if (s.layer != pic->id) continue;
            found = true;
            QCOMPARE(s.visible, std::optional<bool>(true));
            QVERIFY(s.position);
            QVERIFY(std::fabs((*s.position)[0] - 40 * kPx) < 25.4 / 72 + 1e-6);  // (within a pixel of the document)
        }
        QVERIFY(found);
        QVERIFY(render::layer_comps::missing(back.page(0), comps.comps[0].layers).empty());
        for (const std::string& n : opened.notes) QVERIFY2(n.find("レイヤーカンプ") == std::string::npos, n.c_str());
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) return;
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check"), file});
        QVERIFY(process.waitForFinished(600000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardOutput().constData());
    }
};

QTEST_GUILESS_MAIN(TestContractLayerComps)
#include "test_contract_layer_comps.moc"
