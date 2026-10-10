// PSD-01 against psd-tools (AC-PSD as ACCEPTANCE.md §6 has it: the structure checked with psd-tools and with this
// build's own reader; the receiving apps' editing is not run): the PSD book (data/psd_text_book.json) written editable
// — and a book with what PSD text cannot keep, written with those lines as pictures — read by psd-tools 1.10.9
// (tools/migration/psd_text_check.py with the reference Python) and by render::psd::File: the same layers in the same
// groups and order, each text layer's text (the descriptor's and the EngineData's), orientation, transform, point or
// area text, warp, style runs (font, size, bold, italic, colour, spacing, line distance, 縦中横, scale) and paragraphs,
// and the stroke of the effect letters; and what psd-tools reads of the lines themselves (the text, the readings beside
// it in its group, the fonts by name).

#include <QtTest>

#include <QTemporaryDir>

#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/psd_text.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "render/psd_text.hpp"
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
                {"run_alignment", s.run_alignment}};
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
            for (const auto& run : text->runs) runs.push_back(Json{{"length", run.length}, {"style", style_json(run.style)}});
            Json paragraphs = Json::array();
            for (const auto& p : text->paragraphs) paragraphs.push_back(Json{{"length", p.length}, {"justification", p.justification}});
            Json t{{"text", psd::utf8_of(text->text)},
                   {"engine_text", psd::utf8_of(text->engine_text)},
                   {"orientation", text->vertical ? "Vrtc" : "Hrzn"},
                   {"writing_direction", text->vertical ? 2 : 0},
                   {"transform", Json::array()},
                   {"shape", text->box ? "box" : "point"},
                   {"warp", text->warp},
                   {"runs", runs},
                   {"paragraphs", paragraphs}};
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
};

QTEST_GUILESS_MAIN(TestContractPsdText)
#include "test_contract_psd_text.moc"
