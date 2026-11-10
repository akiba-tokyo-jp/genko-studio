// PSD-01 against psd-tools (AC-PSD as ACCEPTANCE.md §6 has it: the structure checked with psd-tools and with this
// build's own reader; the receiving apps' editing is not run): the PSD book (data/psd_text_book.json) written editable
// — and a book with what PSD text cannot keep, written with those lines as pictures — read by psd-tools 1.10.9
// (tools/migration/psd_text_check.py with the reference Python) and by render::psd::File: the same layers in the same
// groups and order, each text layer's text (the descriptor's and the EngineData's), orientation, transform, point or
// area text, warp, style runs (font, size, bold, italic, colour, spacing, line distance, 縦中横, scale) and paragraphs,
// and the stroke of the effect letters; and what psd-tools reads of the lines themselves (the text, the readings beside
// it in its group, the fonts by name).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "psd_book_hashes.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/psd_text.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/psd.hpp"
#include "render/psd_text.hpp"
#include "render/psd_type.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace psd = genko::render::psd;

namespace {

constexpr int kDpi = 150;

genko::core::Document run_ops(const genko::core::Document& doc, const Json& ops) {
    static const genko::core::CommandBus bus(genko::render::ops_registry());
    return bus.apply(doc, ops, genko::core::Actor("human:作者")).doc;
}

std::string position_key(const std::string& position) {
    return position == "inside" ? "InsF" : position == "center" ? "CtrF" : "OutF";
}

// (PC5 G-e) a type layer's keys of a run's style
Json extra_json(const psd::TextStyle& s) {
    Json opentype = Json::array();
    std::vector<std::string> on;
    for (std::size_t i = 0; i < psd::kOpenTypeKeys.size(); ++i) {
        if ((s.opentype >> i) & 1u) on.emplace_back(psd::kOpenTypeKeys[i].key);
    }
    std::sort(on.begin(), on.end());
    for (const std::string& key : on) opentype.push_back(key);
    return Json{{"baseline_shift", s.baseline_shift}, {"caps", s.caps},       {"underline", s.underline},
                {"strikethrough", s.strikethrough},   {"auto_kerning", s.auto_kerning}, {"ligatures", s.ligatures},
                {"dligatures", s.dligatures},         {"auto_leading", s.auto_leading}, {"opentype", opentype}};
}

Json style_json(const psd::TextStyle& s) {
    return Json{{"font", s.font},
                {"size", s.size},
                {"faux_bold", s.faux_bold},
                {"faux_italic", s.faux_italic},
                {"fill", Json::array({s.fill[0], s.fill[1], s.fill[2]})},
                {"tracking", s.tracking},
                {"leading", s.leading},
                {"baseline_direction", s.baseline_direction},
                {"horizontal_scale", s.horizontal_scale},
                {"vertical_scale", s.vertical_scale},
                {"run_alignment", s.run_alignment},
                {"extra", extra_json(s)}};
}

// What psd_text_check.py prints for a file, made with this build's reader.
Json own_view(const std::string& path) {
    const psd::File file = psd::File::read(genko::test::read_bytes(QString::fromStdString(path)));
    const auto& layers = file.layers();
    Json out{{"width", file.width()}, {"height", file.height()}, {"layers", Json::object()}, {"order", Json::object()}};
    out["order"][""] = Json::array();
    for (const auto& layer : layers) {
        if (layer.folder) out["order"][layer.name] = Json::array();
    }
    for (const auto& layer : layers) {
        const std::string folder = layer.parent ? layers.at(*layer.parent).name : std::string();
        out["order"][folder].push_back(layer.name);
        Json entry{{"kind", layer.kind == "folder" ? "group" : layer.kind == "text" ? "type" : "pixel"}};
        if (const auto text = file.text(layer)) {
            Json runs = Json::array();
            // (PC5 G-e: a sheet by its index in its set — the normal one first, as this build writes it)
            for (const auto& run : text->runs)
                runs.push_back(Json{{"length", run.length}, {"style", style_json(run.style)}, {"sheet", run.style.sheet ? Json(*run.style.sheet + 1) : Json()}});
            Json paragraphs = Json::array();
            for (const auto& p : text->paragraphs) {
                paragraphs.push_back(Json{{"length", p.length},
                                          {"justification", p.justification},
                                          {"extra", Json{{"first_indent", p.first_indent},
                                                         {"start_indent", p.start_indent},
                                                         {"end_indent", p.end_indent},
                                                         {"space_before", p.space_before},
                                                         {"space_after", p.space_after},
                                                         {"auto_leading", p.auto_leading},
                                                         {"hyphenate", p.hyphenate},
                                                         {"sheet", p.sheet ? Json(*p.sheet + 1) : Json()}}}});
            }
            Json char_sheets = Json::array(), para_sheets = Json::array();
            for (const auto& s : text->char_sheets) char_sheets.push_back(psd::utf8_of(s.name));
            for (const auto& s : text->para_sheets) para_sheets.push_back(psd::utf8_of(s.name));
            Json t{{"text", psd::utf8_of(text->text)},
                   {"engine_text", psd::utf8_of(text->engine_text)},
                   {"orientation", text->vertical ? "Vrtc" : "Hrzn"},
                   {"writing_direction", text->vertical ? 2 : 0},
                   {"transform", Json::array()},
                   {"shape", text->box ? "box" : "point"},
                   {"warp", text->warp},
                   {"runs", runs},
                   {"paragraphs", paragraphs},
                   {"antialias", text->antialias},
                   {"engine_antialias", text->engine_antialias},
                   {"char_sheets", char_sheets},
                   {"para_sheets", para_sheets}};
            for (const double v : text->transform) t["transform"].push_back(v);
            if (text->box) t["box"] = Json::array({(*text->box)[0], (*text->box)[1], (*text->box)[2], (*text->box)[3]});
            entry["text"] = t;
        }
        if (const auto stroke = file.stroke(layer)) {
            entry["stroke"] = Json{{"size", stroke->size},
                                   {"rgb", Json::array({stroke->rgb[0], stroke->rgb[1], stroke->rgb[2]})},
                                   {"position", position_key(stroke->position)},
                                   {"opacity", stroke->opacity},
                                   {"enabled", stroke->enabled}};
        }
        out["layers"][folder + "/" + layer.name] = entry;
    }
    return out;
}

}  // namespace

class TestContractPsdText : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    // psd-tools' view of the files (keyed by their paths).
    Json psd_tools(const std::vector<std::string>& files) {
        QStringList args{genko::test::repo_root() + QStringLiteral("/tools/migration/psd_text_check.py"), path(QStringLiteral("view.json"))};
        for (const auto& f : files) args << QString::fromStdString(f);
        const auto ran = genko::test::run(genko::test::python_ref(), args, genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) qWarning("%s", ran.err.constData());
        return ran.exit_code == 0 ? genko::test::read_json(path(QStringLiteral("view.json"))) : Json();
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
    }

    // The PSD book written editable: psd-tools and this build's reader read the same structure, and it is the book's.
    void editableBookAsPsdToolsReadsIt() {
        const genko::core::Document blank = genko::core::new_episode("PSD文字", genko::core::Num(1), 4, genko::core::PageSpec::a4_mono());
        const genko::core::Document doc = run_ops(blank, genko::test::read_json(genko::test::test_data("psd_text_book.json")));
        std::vector<std::string> files;
        for (const auto& f : genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("editable").toStdString()), kDpi,
                                                              genko::formats::PsdText{true, false})) {
            files.push_back(f.string());
        }
        QCOMPARE(files.size(), std::size_t{4});
        Json theirs = psd_tools(files);  // (not const: a key it lacks reads as null)
        QVERIFY(theirs.is_object());
        int text_layers = 0;
        for (const auto& f : files) {
            const Json mine = own_view(f);
            std::string where;
            QVERIFY2(genko::test::same_content(theirs[f], mine, &where), (f + ": " + where).c_str());
            for (const auto& [key, layer] : theirs[f]["layers"].items()) text_layers += layer["kind"] == "type" ? 1 : 0;
        }
        QCOMPARE(text_layers, 19 + 13);
        // what psd-tools reads of the lines: 「東京と東京」 down, its readings beside it in its group, fonts by name
        Json& p1 = theirs[files[0]];
        QCOMPARE(p1["order"]["台詞 東京と東京"], Json::parse(R"(["フキダシ", "本文 #p1a", "ルビ「とうきょう」 #p1a-r0", "ルビ「とうきょう」 #p1a-r1"])"));
        Json& body = p1["layers"]["台詞 東京と東京/本文 #p1a"]["text"];
        QCOMPARE(body["text"], Json("東京と東京"));
        QCOMPARE(body["engine_text"], Json("東京と東京\r"));
        QCOMPARE(body["orientation"], Json("Vrtc"));
        QCOMPARE(body["warp"], Json("warpNone"));
        QCOMPARE(body["runs"][0]["style"]["font"], Json("ZenKakuGothicNew-Bold"));
        QCOMPARE(body["runs"][1]["style"]["font"], Json("ZenOldMincho-SemiBold"));
        QCOMPARE(p1["layers"]["台詞 東京と東京/ルビ「とうきょう」 #p1a-r1"]["text"]["text"], Json("とうきょう"));
        QCOMPARE(p1["layers"]["台詞 AIと12時/本文 #p1c"]["text"]["runs"][0]["style"]["baseline_direction"], Json(3));
        Json& p3 = theirs[files[2]];
        QCOMPARE(p3["layers"]["台詞 ドーン/本文 #p3a"]["stroke"],
                 Json::parse(R"({"size": 6.0, "rgb": [255, 255, 255], "position": "OutF", "opacity": 100.0, "enabled": true})"));
        QCOMPARE(p3["layers"]["台詞 ドーン/本文 #p3a"]["text"]["runs"][0]["style"]["fill"], Json::parse("[200, 30, 30]"));
        Json& p4 = theirs[files[3]];
        QCOMPARE(p4["layers"]["台詞 均等に割り付けた縦の行/本文 #p4c"]["text"]["shape"], Json("box"));
        QVERIFY(p4["layers"].contains("/ノンブル"));
        QCOMPARE(p4["layers"]["/ノンブル"]["kind"], Json("pixel"));
    }

    // The image way (the default PSD) is Python's: the PSD book as Python makes it (its rubies as ruby_runs — Python's
    // book has none by place) written by Python's psd.export_psd_pages — its text laid out by Pillow's BASIC layout, without
    // raqm, as render_harness holds the reference — and by this build, byte for byte. (The unit test holds the PSD book's
    // own image-way bytes to those of the build before PSD-01.)
    void imageWayIsPythons() {
        const QString script = genko::test::repo_root() + QStringLiteral("/tools/migration/psd_text_check.py");
        const QString book = path(QStringLiteral("python.genko"));
        auto ran = genko::test::run(genko::test::python_ref(), {script, QStringLiteral("python-book"), genko::test::test_data(QStringLiteral("psd_text_book.json")), book},
                                    genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, ran.err.constData());
        ran = genko::test::run(genko::test::python_ref(), {script, QStringLiteral("python-psd"), book, path(QStringLiteral("python-psd")), QStringLiteral("150")},
                               genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, ran.err.constData());
        const auto loaded = genko::storage::load_document(genko::core::path_from_utf8(book.toStdString()));
        const auto files = genko::formats::export_psd_pages(loaded.document, genko::core::path_from_utf8(path("cpp-psd").toStdString()), kDpi);
        QCOMPARE(files.size(), std::size_t{4});
        const auto theirs = genko::test::tree_hashes(path(QStringLiteral("python-psd")));
        const auto mine = genko::test::tree_hashes(path(QStringLiteral("cpp-psd")));
        QCOMPARE(mine.size(), std::size_t{4});
        QVERIFY(mine == theirs);
        // page 1 of Python's PSD is the page 1 the unit test holds this build's book to (psd_book_hashes.hpp: since M5① the
        // book's rubies on 「声」 and 「約束」, a page Python's book and this build's lay out alike)
        QCOMPARE(QString::fromStdString(theirs.begin()->first), QStringLiteral("PSD文字_ep01_p001.psd"));
        QCOMPARE(QString::fromStdString(theirs.begin()->second), QString::fromLatin1(genko::test::kPsdBookImageWay[0]));
    }

    // A font whose PostScript name ends in a backslash: written without it, and psd-tools reads the text.
    void fontNameAsPsdToolsReadsIt() {
        std::string font = genko::test::read_bytes(genko::test::repo_root() + QStringLiteral("/src/genko/fonts/Yomogi-Regular.ttf"));
        const auto be16 = [&](std::size_t at) { return (static_cast<unsigned char>(font[at]) << 8) | static_cast<unsigned char>(font[at + 1]); };
        const auto be32 = [&](std::size_t at) { return (static_cast<std::size_t>(be16(at)) << 16) | static_cast<std::size_t>(be16(at + 2)); };
        for (std::size_t i = 0; i < static_cast<std::size_t>(be16(4)); ++i) {  // (each PostScript name record's last character a backslash)
            const std::size_t rec = 12 + i * 16;
            if (font.compare(rec, 4, "name") != 0) continue;
            const std::size_t name = be32(rec + 8);
            const std::size_t strings = name + static_cast<std::size_t>(be16(name + 4));
            for (std::size_t k = 0; k < static_cast<std::size_t>(be16(name + 2)); ++k) {
                const std::size_t r = name + 6 + k * 12;
                if (be16(r + 6) != 6) continue;
                const std::size_t end = strings + static_cast<std::size_t>(be16(r + 10)) + static_cast<std::size_t>(be16(r + 8));
                if (be16(r) != 1) font[end - 2] = '\0';
                font[end - 1] = '\\';
            }
        }
        genko::test::write_bytes(path(QStringLiteral("spoilt.ttf")), font);
        genko::core::Document doc = genko::core::new_episode("書体名", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "f"}, {"text", "ふでの字"}, {"x_mm", 140}, {"y_mm", 20},
                                             {"w_mm", 30}, {"h_mm", 60}, {"wrap", "vertical"}, {"balloon", "none"},
                                             {"style", Json{{"size_mm", 5}, {"font", path(QStringLiteral("spoilt.ttf")).toStdString()}}}}}));
        const std::string file = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("font-name").toStdString()), kDpi,
                                                                  genko::formats::PsdText{true, false})
                                     .front()
                                     .string();
        Json theirs = psd_tools({file});  // (not const: a key it lacks reads as null)
        QVERIFY(theirs.is_object() && theirs.contains(file));
        Json& text = theirs[file]["layers"]["台詞 ふでの字/本文 #f"]["text"];
        QCOMPARE(text["text"], Json("ふでの字"));
        QCOMPARE(text["runs"][0]["style"]["font"], Json("Yomogi-Regula"));
        std::string where;
        QVERIFY2(genko::test::same_content(theirs[file], own_view(file), &where), where.c_str());
    }

    // A line using what PSD text cannot keep, drawn as a picture when asked: psd-tools reads it as a picture layer
    // outside the groups, the line beside it as text.
    void rasterizedLinesAsPsdToolsReadsThem() {
        genko::core::Document doc = genko::core::new_episode("装飾", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "ok", "text": "ふつうの台詞", "x_mm": 140, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical"},
            {"op": "add_line", "page": 1, "id": "grad", "text": "グラデ", "x_mm": 100, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "style": {"gradient": {"rgb_from": [0, 0, 0], "rgb_to": [200, 0, 0]}}}])"));
        const auto written = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("rasterized").toStdString()), kDpi,
                                                              genko::formats::PsdText{true, true});
        const std::string file = written.front().string();
        Json theirs = psd_tools({file});  // (not const: a key it lacks reads as null)
        QVERIFY(theirs.is_object() && theirs.contains(file));
        std::string where;
        QVERIFY2(genko::test::same_content(theirs[file], own_view(file), &where), where.c_str());
        QCOMPARE(theirs[file]["layers"]["/台詞 グラデ"]["kind"], Json("pixel"));
        QCOMPARE(theirs[file]["layers"]["台詞 ふつうの台詞/本文 #ok"]["kind"], Json("type"));
    }

    // PC5 G-e: a book of type layers (a point text across with a style on a range, a box text down, one set in its balloon,
    // one with a gradient — what PSD text cannot keep —) and type styles.
    genko::core::Document type_book() {
        genko::core::Document doc = genko::core::new_episode("文字の層", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        return run_ops(doc, Json::parse(R"([
            {"op": "type_style", "kind": "character", "action": "new", "name": "強調", "attrs": {"underline": true, "rgb": [200, 0, 0]}},
            {"op": "type_style", "kind": "paragraph", "action": "new", "name": "見出し", "attrs": {"align": "center", "space_after_mm": 2}},
            {"op": "add_type_layer", "page": 1, "text": "Hello 1/2\n二行目の文字", "point": [20, 40], "wrap": "horizontal", "new_id": "t1",
             "title": "横", "antialias": "none", "char": {"size_mm": 6, "tracking": 0.05, "features": {"frac": 1}}},
            {"op": "type_style", "kind": "character", "action": "apply", "id": "cs-1", "page": 1, "layer_id": "t1", "range": [0, 5]},
            {"op": "type_style", "kind": "paragraph", "action": "apply", "id": "ps-1", "page": 1, "layer_id": "t1", "range": [10, 12]},
            {"op": "add_type_layer", "page": 1, "text": "縦の段落テキストです", "box": [120, 30, 30, 90], "wrap": "vertical", "new_id": "t2",
             "title": "縦", "char": {"size_mm": 7, "baseline_shift_mm": 0.5}},
            {"op": "add_type_layer", "page": 1, "text": "フキダシの中", "box": [40, 150, 40, 40], "wrap": "vertical", "balloon": "speech",
             "new_id": "t3", "title": "フキダシ付き"},
            {"op": "add_type_layer", "page": 1, "text": "グラデ", "box": [90, 200, 40, 30], "wrap": "horizontal", "new_id": "t4", "title": "グラデ",
             "style": {"gradient": {"rgb_from": [0, 0, 0], "rgb_to": [200, 0, 0]}}}])"));
    }

    // PC5 G-e (type@1): the type layers written as Photoshop's text layers — psd-tools and this build's reader read the
    // same structure (the new keys, the anti-alias, the named sheets and the runs' and paragraphs' sheets among it) —;
    // the one set in its balloon a group of its balloon and text; what PSD text cannot keep its pixels (a strict editable
    // export stops, naming the layer); whatever way the lines are written.
    void typeLayersAsPsdToolsReadsThem() {
        const genko::core::Document doc = type_book();
        try {
            (void)genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("type-strict").toStdString()), kDpi, genko::formats::PsdText{true, false});
            QFAIL("the strict editable PSD wrote a type layer of a gradient");
        } catch (const genko::formats::PsdTextRefused& refused) {
            QCOMPARE(refused.issues().size(), std::size_t{1});
            QCOMPARE(refused.issues().front().layer, std::string("t4"));
            QCOMPARE(refused.issues().front().features, std::vector<std::string>{"gradient"});
        }
        for (const bool editable : {true, false}) {
            genko::formats::PsdTextReport report;
            const std::string file = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path(editable ? "type-editable" : "type-image").toStdString()),
                                                                      kDpi, genko::formats::PsdText{editable, true}, &report)
                                         .front()
                                         .string();
            Json theirs = psd_tools({file});
            QVERIFY(theirs.is_object() && theirs.contains(file));
            std::string where;
            QVERIFY2(genko::test::same_content(theirs[file], own_view(file), &where), where.c_str());
            Json& layers = theirs[file]["layers"];
            if (!editable) {  // (見た目優先で画像化: the type layers their pictures, as before G-e — nothing said)
                for (const char* name : {"/横", "/縦", "/グラデ", "/フキダシ付き"}) QCOMPARE(layers[name]["kind"], Json("pixel"));
                QVERIFY(report.rasterized.empty() && report.text_layers == 0);
                continue;
            }
            QCOMPARE(layers["/横"]["kind"], Json("type"));
            QCOMPARE(layers["/縦"]["kind"], Json("type"));
            QCOMPARE(layers["/グラデ"]["kind"], Json("pixel"));
            QCOMPARE(layers["/フキダシ付き"]["kind"], Json("group"));
            QCOMPARE(layers["フキダシ付き/本文 #t3"]["kind"], Json("type"));
            QCOMPARE(layers["フキダシ付き/フキダシ"]["kind"], Json("pixel"));
            QCOMPARE(report.rasterized.size(), std::size_t{1});
            QCOMPARE(report.rasterized.front().layer, std::string("t4"));
            QVERIFY(report.rasterized.front().line.empty());  // (a type layer's: `line` none — the review's P3-9)
            Json& across = layers["/横"]["text"];
            QCOMPARE(across["text"], Json("Hello 1/2\r二行目の文字"));
            QCOMPARE(across["orientation"], Json("Hrzn"));
            QCOMPARE(across["shape"], Json("point"));
            QCOMPARE(across["antialias"], Json("Anno"));
            QCOMPARE(across["engine_antialias"], Json(0));
            QCOMPARE(across["char_sheets"], Json::array({"強調"}));
            QCOMPARE(across["para_sheets"], Json::array({"見出し"}));
            QCOMPARE(across["runs"][0]["sheet"], Json(1));
            QCOMPARE(across["runs"][0]["length"], Json(5));
            QCOMPARE(across["runs"][0]["style"]["extra"]["underline"], Json(true));
            QCOMPARE(across["runs"][0]["style"]["fill"], Json::array({200, 0, 0}));
            QCOMPARE(across["runs"][0]["style"]["extra"]["opentype"], Json::array({"Fractions"}));
            QCOMPARE(across["runs"][0]["style"]["extra"]["auto_leading"], Json(true));
            QVERIFY(std::abs(across["runs"][0]["style"]["size"].get<double>() - 6 / 25.4 * kDpi) < 1e-6);
            QVERIFY(std::abs(across["runs"][0]["style"]["tracking"].get<double>() - 50) < 1e-6);
            QVERIFY(std::abs(across["transform"][4].get<double>() - 20 / 25.4 * kDpi) < 1e-6);
            QVERIFY(std::abs(across["transform"][5].get<double>() - 40 / 25.4 * kDpi) < 1e-6);
            QCOMPARE(across["paragraphs"][0]["extra"]["sheet"], Json());
            QCOMPARE(across["paragraphs"][1]["extra"]["sheet"], Json(1));
            QCOMPARE(across["paragraphs"][1]["justification"], Json(2));
            Json& down = layers["/縦"]["text"];
            QCOMPARE(down["orientation"], Json("Vrtc"));
            QCOMPARE(down["shape"], Json("box"));
            QCOMPARE(down["antialias"], Json("AnSm"));
            QVERIFY(std::abs(down["box"][2].get<double>() - 30 / 25.4 * kDpi) < 1e-6);
            QVERIFY(std::abs(down["runs"][0]["style"]["extra"]["baseline_shift"].get<double>() - 0.5 / 25.4 * kDpi) < 1e-6);
        }
    }

    // PC5 G-e (the coordinator's decision for G): a PSD's text layers opened as type layers — their text, shape, matrix,
    // anti-alias, attributes and named sheets the book's styles —; written again while they are the same (saved and read
    // back too), their TySh blocks as the file's, byte for byte; a text changed, its block written anew, as the book's.
    void typeLayersComeBack() {
        const genko::core::Document doc = type_book();
        const std::string first = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("type-first").toStdString()), kDpi,
                                                                   genko::formats::PsdText{true, true})
                                      .front()
                                      .string();
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(first));
        const auto blocks = [](const std::string& psd_bytes) {
            const psd::File file = psd::File::read(psd_bytes);
            std::map<std::string, std::string> out;
            for (const auto& layer : file.layers()) {
                if (const auto block = file.text_block(layer)) out[layer.name] = std::string(*block);
            }
            return out;
        };
        const auto original = blocks(bytes);
        QVERIFY(original.contains("横") && original.contains("縦") && original.contains("本文 #t3"));
        genko::formats::Opened opened = genko::formats::open_file(bytes, "type.psd");
        const genko::core::Page& page = opened.doc.page(0);
        const auto layer_named = [&](const genko::core::Page& p, const std::string& title) -> const genko::core::Layer* {
            for (const auto& l : p.layers) {
                if (l.title == title) return &l;
            }
            return nullptr;
        };
        const genko::core::Layer* across = layer_named(page, "横");
        QVERIFY(across != nullptr && across->kind == genko::core::LayerKind::Type && across->type);
        QCOMPARE(across->type->text, std::string("Hello 1/2\n二行目の文字"));
        QCOMPARE(across->type->wrap, std::string("horizontal"));
        const Json& nt = *across->type->native_type;
        QCOMPARE(nt["shape"], Json("point"));
        QCOMPARE(nt["antialias"], Json("none"));
        QVERIFY(std::abs(nt["anchor_mm"][0].get<double>() - 20) < 0.01 && std::abs(nt["anchor_mm"][1].get<double>() - 40) < 0.01);
        QVERIFY(across->native_psd && !across->native_psd->text.empty());
        const genko::core::Layer* down = layer_named(page, "縦");
        QVERIFY(down != nullptr && down->kind == genko::core::LayerKind::Type);
        QCOMPARE((*down->type->native_type)["shape"], Json("box"));
        QCOMPARE(down->type->wrap, std::string("vertical"));
        QVERIFY(opened.doc.type_styles.has_value());
        QCOMPARE((*opened.doc.type_styles)["character"][0]["name"], Json("強調"));
        QCOMPARE((*opened.doc.type_styles)["paragraph"][0]["name"], Json("見出し"));
        QVERIFY(std::find(opened.doc.features.begin(), opened.doc.features.end(), "type@1") != opened.doc.features.end());
        // (the same since: its blocks as the file's — a book saved and read back too)
        const auto again = [&](const genko::core::Document& book, const char* name) {
            const QString at = path(QStringLiteral("type-again/%1.psd").arg(QString::fromLatin1(name)));
            QDir().mkpath(path(QStringLiteral("type-again")));
            (void)genko::formats::export_page_psd(book, book.page(0), genko::core::path_from_utf8(at.toStdString()), kDpi,
                                                  genko::formats::PsdText{true, true});
            return blocks(genko::test::read_bytes(at));
        };
        const auto same = again(opened.doc, "same");
        QVERIFY(same.at("横") == original.at("横"));
        QVERIFY(same.at("縦") == original.at("縦"));
        const QString saved = path(QStringLiteral("type-saved.genko"));
        genko::test::write_project(opened.doc, genko::core::path_from_utf8(saved.toStdString()));
        const genko::core::Document loaded = genko::storage::load_document(genko::core::path_from_utf8(saved.toStdString())).document;
        const auto reloaded = again(loaded, "loaded");
        QVERIFY(reloaded.at("横") == original.at("横"));
        // (its text changed: written anew — the new text read back —; another resolution: anew too)
        const genko::core::Document edited = run_ops(loaded, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", across->id}, {"text", "Changed"}}}));
        const auto changed = again(edited, "changed");
        QVERIFY(changed.at("横") != original.at("横"));
        QVERIFY(psd::read_tysh(changed.at("横")).text == u"Changed");
        QVERIFY(changed.at("縦") == original.at("縦"));
        const QString other = path(QStringLiteral("type-again/dpi.psd"));
        (void)genko::formats::export_page_psd(loaded, loaded.page(0), genko::core::path_from_utf8(other.toStdString()), 200,
                                              genko::formats::PsdText{true, true});
        QVERIFY(blocks(genko::test::read_bytes(other)).at("横") != original.at("横"));
        // (a style it takes changed: anew; one that takes none, as it was)
        const std::string cs = (*loaded.type_styles)["character"][0]["id"].get<std::string>();
        const genko::core::Document restyled = run_ops(loaded, Json::array({Json{{"op", "type_style"}, {"kind", "character"}, {"action", "set"}, {"id", cs},
                                                                                  {"attrs", Json{{"rgb", Json::array({0, 0, 255})}}}}}));
        const auto after_style = again(restyled, "restyled");
        QVERIFY(after_style.at("横") != original.at("横"));
        QVERIFY(after_style.at("縦") == original.at("縦"));
        // (its anti-alias, matrix, anchor or warp changed: anew — the fingerprint's watch, the review's missing test (5))
        const std::string down_id = layer_named(loaded.page(0), "縦")->id;
        for (const Json& change : {Json{{"antialias", "crisp"}}, Json{{"transform", Json::array({0.9, 0.1, -0.1, 0.9, 1, 2})}},
                                   Json{{"box", Json::array({118, 30, 30, 90})}}, Json{{"warp", Json{{"style", "arc"}, {"bend", 20}}}}}) {
            Json op{{"op", "edit_type"}, {"page", 1}, {"layer_id", down_id}};
            for (const auto& [k, v] : change.items()) op[k] = v;
            const auto moved = again(run_ops(loaded, Json::array({op})), "changed-down");
            QVERIFY2(moved.at("縦") != original.at("縦"), change.dump().c_str());
        }
        // (its page made CMYK: anew — the colour mode in the fingerprint, the review's P3-3)
        const genko::core::Document cmyk = run_ops(loaded, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}}));
        QVERIFY(again(cmyk, "cmyk").at("縦") != original.at("縦"));
        // (the image way: no text layer — its pictures, the kept block not written)
        const QString image = path(QStringLiteral("type-again/image.psd"));
        (void)genko::formats::export_page_psd(loaded, loaded.page(0), genko::core::path_from_utf8(image.toStdString()), kDpi);
        QVERIFY(blocks(genko::test::read_bytes(image)).empty());
    }

    // PC6 review B P2-1: a PSD of named character and paragraph styles placed as a smart object and made a page
    // (smart_to_page): the styles its type layers take are the book's — before, the op was refused ("… takes the style …,
    // which the book does not keep") —, a style of the book's of the same name and other attributes kept as it is, the file's
    // under a name of its own; saved and read back, not read-only.
    void placedPsdMadeAPageBringsItsStyles() {
        namespace core = genko::core;
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(
            genko::formats::export_psd_pages(type_book(), core::path_from_utf8(path("type-placed").toStdString()), kDpi, genko::formats::PsdText{true, true})
                .front()
                .string()));
        core::Document book = core::new_episode("置く", core::Num(1), 1, core::PageSpec::a4_mono());
        book = run_ops(book, Json::array({Json{{"op", "type_style"}, {"kind", "character"}, {"action", "new"}, {"name", "強調"},
                                               {"attrs", Json{{"rgb", Json::array({0, 0, 200})}}}}}));
        const Json own = (*book.type_styles)["character"][0];
        book = run_ops(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(bytes)}, {"name", "type.psd"}, {"new_id", "so"}}}));
        const core::Document paged = run_ops(book, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}, {"new_id", "in"}}}));
        QVERIFY(paged.type_styles.has_value());
        const Json& chars = (*paged.type_styles)["character"];
        QCOMPARE(chars.size(), std::size_t(2));
        QCOMPARE(chars[0], own);
        QCOMPARE(chars[1]["name"], Json("強調 2"));
        QCOMPARE((*paged.type_styles)["paragraph"][0]["name"], Json("見出し"));
        const core::Layer* so = nullptr;
        for (const core::Layer& l : paged.page(0).layers)
            if (l.id == "so") so = &l;
        QVERIFY(so != nullptr);
        const core::SmartContents* contents = core::smart::contents_of(paged, *so);
        QVERIFY(contents != nullptr && contents->page);
        const core::Layer* across = nullptr;
        for (const core::Layer& l : contents->page->layers)
            if (l.title == "横") across = &l;
        QVERIFY(across != nullptr && across->type && across->type->native_type);
        const Json& runs = across->type->native_type->at("runs");
        QVERIFY2(std::any_of(runs.begin(), runs.end(), [&](const Json& run) { return run.value("style", Json()) == chars[1]["id"]; }), runs.dump().c_str());
        const QString saved = path(QStringLiteral("placed-type.genko"));
        genko::test::write_project(paged, core::path_from_utf8(saved.toStdString()));
        const core::Document loaded = genko::storage::load_document(core::path_from_utf8(saved.toStdString())).document;
        QVERIFY2(loaded.read_only_reason.empty(), loaded.read_only_reason.c_str());
        QCOMPARE(loaded.type_styles->dump(), paged.type_styles->dump());
    }

    // PC5 G-e (the review's P2-1): opening a PSD (the window's, the CLI's and MCP's 開く) says what of its text layers it
    // could not hold — a font not among the bundled ones by its PostScript name, a text layer opened as its pixels and why.
    void openingSaysTheText() {
        genko::core::Document doc = genko::core::new_episode("書体", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_type_layer", "page": 1, "text": "Arial", "point": [20, 40], "wrap": "horizontal", "new_id": "a", "title": "書体",
             "char": {"postscript": "ArialMT"}},
            {"op": "add_type_layer", "page": 1, "text": "壊す", "point": [20, 80], "wrap": "horizontal", "new_id": "b", "title": "壊す"}])"));
        const std::string file = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("notes").toStdString()), kDpi,
                                                                  genko::formats::PsdText{true, false})
                                     .front()
                                     .string();
        std::string bytes = genko::test::read_bytes(QString::fromStdString(file));
        {  // (the second's TySh made one of another version: not read)
            const psd::File read = psd::File::read(bytes);
            for (const auto& layer : read.layers()) {
                if (layer.name != "壊す") continue;
                const std::size_t at = bytes.find(std::string(*read.text_block(layer)));
                QVERIFY(at != std::string::npos);
                bytes[at + 1] = 2;
            }
        }
        const genko::formats::Opened opened = genko::formats::open_file(bytes, "notes.psd");
        std::string all;
        for (const std::string& n : opened.notes) all += n + "\n";
        QVERIFY2(all.find("ArialMT") != std::string::npos && all.find("既定の書体") != std::string::npos, all.c_str());
        QVERIFY2(all.find("文字レイヤー「壊す」は画素で開きました（文字として読めません") != std::string::npos, all.c_str());
    }

    // PC5 G-e, the review of J-b's P2-4's way: a book opened from a PSD with text layers before G-e (its text layers its
    // pixels: render::psd_type::PixelsForTest) and saved — read and saved again now, the same bytes; its page drawn and
    // written as a PSD the same. Only opening the PSD again changes what the book is.
    void bookBeforeTypeLayersAsBefore() {
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(
            genko::formats::export_psd_pages(type_book(), genko::core::path_from_utf8(path("type-old").toStdString()), kDpi, genko::formats::PsdText{true, true})
                .front()
                .string()));
        const QString old_book = path(QStringLiteral("before-ge/old.genko"));
        QDir().mkpath(path(QStringLiteral("before-ge")));
        {
            const genko::render::psd_type::PixelsForTest pixels;
            const genko::formats::Opened opened = genko::formats::open_file(bytes, "type.psd");
            for (const auto& l : opened.doc.page(0).layers) QVERIFY(l.kind != genko::core::LayerKind::Type);
            QVERIFY(!opened.doc.type_styles);
            genko::test::write_project(opened.doc, genko::core::path_from_utf8(old_book.toStdString()));
        }
        const std::string old_json = genko::test::read_bytes(old_book + QStringLiteral("/project.json"));
        const genko::core::Document doc = genko::storage::load_document(genko::core::path_from_utf8(old_book.toStdString())).document;
        const QString again = path(QStringLiteral("before-ge/again.genko"));
        genko::test::write_project(doc, genko::core::path_from_utf8(again.toStdString()));
        QVERIFY(genko::test::read_bytes(again + QStringLiteral("/project.json")) == old_json);
        QVERIFY(old_json.find("type@1") == std::string::npos);
        // (its PSD: its layers of pixels — no text layer kept, none written —, the same twice; its picture the same as the
        // book's as it was opened)
        const auto psd_of = [&](const genko::core::Document& book, const char* name) {
            const QString at = path(QStringLiteral("before-ge/%1.psd").arg(QString::fromLatin1(name)));
            (void)genko::formats::export_page_psd(book, book.page(0), genko::core::path_from_utf8(at.toStdString()), kDpi);
            return genko::test::read_bytes(at);
        };
        const std::string written = psd_of(doc, "now");
        QVERIFY(written == psd_of(doc, "again"));
        const psd::File file = psd::File::read(written);
        for (const auto& layer : file.layers()) QVERIFY2(!file.text_block(layer), layer.name.c_str());
        genko::render::RenderOptions options;
        const auto drawn = [&](const genko::core::Document& book) { return genko::render::render_page(book.page(0), kDpi, options, &book).image.tobytes(); };
        genko::core::Document opened_then;
        {
            const genko::render::psd_type::PixelsForTest pixels;
            opened_then = genko::formats::open_file(bytes, "type.psd").doc;
        }
        QVERIFY(drawn(doc) == drawn(opened_then));
        // (opened again now: its text layers type layers)
        const genko::formats::Opened now = genko::formats::open_file(bytes, "type.psd");
        QVERIFY(std::any_of(now.doc.page(0).layers.begin(), now.doc.page(0).layers.end(), [](const genko::core::Layer& l) { return l.kind == genko::core::LayerKind::Type; }));
    }
};

QTEST_GUILESS_MAIN(TestContractPsdText)
#include "test_contract_psd_text.moc"
