// TEXT-01 (改善 I04, AC-TEXT, schema-v4 ruby-layout@1): a line's ruby held by its place in the text, without Python.
// The table of expected results (data/ruby_cases.json, its sha256 in data/MANIFEST.json): each case puts a line, edits
// it (edit_line, replace_text) and wants its text, its ruby_spans ([start, end) in grapheme clusters), its ruby_runs,
// the ruby issues of 入稿前の点検 and what the op reports; the line before the edit is kept whole (Undo). Then what the
// ops refuse (spans outside the text, overlapping, both forms at once, the layout keys' bounds), the Unicode version
// the ranges are counted in, the feature written only when the book uses it (a book of Python's saved as before), and
// a book read back with its ruby where it was.

#include <QtTest>

#include <QChar>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "formats/checks.hpp"
#include "formats/exporting.hpp"
#include "render/ops_registry.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::ApplyError;
using genko::core::ApplyResult;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;

namespace {

const CommandBus& bus() {
    static const CommandBus b(genko::render::ops_registry());
    return b;
}

ApplyResult run_ops(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")); }

std::string refusal(const Document& doc, const Json& ops) {
    try {
        bus().apply(doc, ops, Actor("human:作者"));
    } catch (const ApplyError& error) {
        return error.what();
    }
    return "(applied)";
}

Document blank_book() { return genko::core::new_episode("ルビ", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono()); }

// The book as it would be written (project.json v4).
Json payload_of(const Document& doc, const QTemporaryDir& dir) {
    genko::storage::AssetStore store(genko::storage::path_from_utf8(dir.path().toStdString() + "/assets-of-payload"));
    return genko::storage::project_payload_v4(doc, store);
}

const Json& line_of(const Json& payload, const std::string& id) {
    for (const Json& line : payload["story"]) {
        if (line["id"] == id) return line;
    }
    static const Json none;
    return none;
}

// A case's line as add_line puts it (its text, wrap, box, style, ruby_spans or ruby_runs).
Json add_op(const Json& c) {
    Json op{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", c["text"]}};
    op["wrap"] = c.contains("wrap") ? c["wrap"] : Json("vertical");
    const Json box = c.contains("box") ? c["box"] : Json::array({20, 20, 40, 40});
    op["x_mm"] = box[0];
    op["y_mm"] = box[1];
    op["w_mm"] = box[2];
    op["h_mm"] = box[3];
    if (c.contains("spans")) op["ruby_spans"] = c["spans"];
    if (c.contains("runs")) op["ruby_runs"] = c["runs"];
    if (c.contains("style")) op["style"] = c["style"];
    return op;
}

// The ruby issues 入稿前の点検 gives the line, in order.
std::vector<std::string> ruby_issues(const Document& doc, const std::string& id) {
    std::vector<std::string> out;
    const Json issues = genko::formats::checks::page_issues(doc, doc.page(0));
    for (const Json& issue : issues) {
        const std::string code = issue["code"].get<std::string>();
        if (code.rfind("ruby_", 0) != 0) continue;
        if (issue["target"].is_object() && issue["target"]["id"] == id) out.push_back(code);
    }
    return out;
}

// The ruby_* keys an op's result holds.
Json ruby_report(const ApplyResult& result) {
    Json out = Json::object();
    for (const Json& item : result.results) {
        for (const auto& [key, value] : item.items()) {
            if (key.rfind("ruby_", 0) == 0) out[key] = value;
        }
    }
    return out;
}

std::string shown(const Json& value) { return value.dump(-1, ' ', false); }

}  // namespace

class TestRuby : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // The table is the one MANIFEST.json fixes, its Unicode is Qt's (the grapheme rules of that version).
    void tableIsFixed() {
        QFile file(genko::test::test_data("ruby_cases.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        const Json manifest = genko::test::read_json(genko::test::test_data("MANIFEST.json"));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString(),
                 manifest["ruby_cases.json"].get<std::string>());
        const Json table = Json::parse(bytes.toStdString());
        QCOMPARE(table["unicode"].get<std::string>(), std::string("17.0"));
        QCOMPARE(QChar::currentUnicodeVersion(), QChar::Unicode_17_0);
        QVERIFY(table["cases"].size() >= 30);
    }

    // Every case of the table that is not typed in the canvas's editor (those are test_gui_ruby's).
    void casesOfTheTable() {
        const Json table = genko::test::read_json(genko::test::test_data("ruby_cases.json"));
        std::vector<std::string> failures;
        int ran = 0;
        for (const Json& c : table["cases"]) {
            if (c.value("gui", false)) continue;
            const std::string name = c["n"].get<std::string>();
            try {
                const Document before = run_ops(blank_book(), Json::array({add_op(c)})).doc;
                const Json put = line_of(payload_of(before, tmp_), "L1");
                Document after = before;
                Json report = Json::object();
                if (!c["edit"].is_null()) {
                    Json edit = c["edit"];
                    if (edit["op"] == "edit_line") edit["id"] = "L1";
                    const ApplyResult result = run_ops(before, Json::array({edit}));
                    after = result.doc;
                    report = ruby_report(result);
                }
                const Json& want = c["want"];
                const Json line = line_of(payload_of(after, tmp_), "L1");
                const Json spans = line.contains("ruby_spans") ? line["ruby_spans"] : Json();
                std::string where;
                if (line["text"] != want["text"]) failures.push_back(name + ": text " + shown(line["text"]));
                if (!genko::test::same_content(spans, want["spans"], &where)) {
                    failures.push_back(name + ": ruby_spans " + shown(spans) + " (" + where + ")");
                }
                if (!genko::test::strict_equal(line["ruby_runs"], want["runs"], &where)) {
                    failures.push_back(name + ": ruby_runs " + shown(line["ruby_runs"]) + " (" + where + ")");
                }
                std::vector<std::string> wanted;
                for (const Json& code : want["warnings"]) wanted.push_back(code.get<std::string>());
                const std::vector<std::string> issues = ruby_issues(after, "L1");
                if (issues != wanted) {
                    std::string got;
                    for (const auto& code : issues) got += code + " ";
                    failures.push_back(name + ": issues " + got);
                }
                const Json want_report = want.contains("report") ? want["report"] : Json::object();
                if (!genko::test::same_content(report, want_report, &where)) failures.push_back(name + ": report " + shown(report));
                // the line before the edit is whole in the book before it (Undo gives it back: text, range and reading)
                if (c.value("undo", false)) {
                    const Json again = line_of(payload_of(before, tmp_), "L1");
                    if (!genko::test::strict_equal(again, put, &where)) failures.push_back(name + ": the line before the edit changed");
                    if (!again.contains("ruby_spans") || again["ruby_spans"].empty()) failures.push_back(name + ": no ruby before the edit");
                }
            } catch (const std::exception& error) {
                failures.push_back(name + ": " + error.what());
            }
            ++ran;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(ran) + " cases differ").c_str());
        QVERIFY(ran >= 30);
    }

    // What the ops refuse: a range outside the text or not of whole numbers, rubies over the same characters, no
    // reading, both forms at once, the layout keys out of their bounds. The book is left as it was.
    void refusals() {
        const Document doc = run_ops(blank_book(), Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", "東京へ"}}})).doc;
        const auto spans = [&](const Json& value) {
            return refusal(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"ruby_spans", value}}}));
        };
        const auto has = [](const std::string& error, const char* words) { return error.find(words) != std::string::npos; };
        QVERIFY2(has(spans(Json::parse(R"([{"start": 0, "end": 4, "ruby": "x"}])")), "ruby span"), spans(Json::parse(R"([{"start": 0, "end": 4, "ruby": "x"}])")).c_str());
        QVERIFY(has(spans(Json::parse(R"([{"start": 2, "end": 2, "ruby": "x"}])")), "ruby span"));
        QVERIFY(has(spans(Json::parse(R"([{"start": -1, "end": 1, "ruby": "x"}])")), "ruby span"));
        QVERIFY(has(spans(Json::parse(R"([{"start": 0.5, "end": 1, "ruby": "x"}])")), "ruby span"));
        QVERIFY(has(spans(Json::parse(R"([{"start": 0, "end": 1}])")), "reading"));
        QVERIFY(has(spans(Json::parse(R"([{"start": 0, "end": 1, "ruby": ""}])")), "reading"));
        QVERIFY(has(spans(Json::parse(R"([{"start": 0, "end": 2, "ruby": "a"}, {"start": 1, "end": 3, "ruby": "b"}])")), "overlap"));
        QVERIFY(has(spans(Json::parse(R"([[0, 1, "x"]])")), "ruby_spans"));
        QVERIFY(has(spans(Json("x")), "ruby_spans"));
        QVERIFY(has(refusal(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "ruby_spans": [{"start": 0, "end": 1, "ruby": "x"}],
            "ruby_runs": [["東", "x"]]}])")), "not both"));
        QVERIFY(has(refusal(doc, Json::parse(R"([{"op": "add_line", "page": 1, "text": "東京", "ruby_spans": [{"start": 0, "end": 3, "ruby": "x"}]}])")),
                    "ruby span"));
        const auto style = [&](const Json& value) {
            return refusal(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", value}}}));
        };
        QVERIFY(has(style(Json{{"ruby_align", "left"}}), "ruby_align must be head, center or tail"));
        QVERIFY(has(style(Json{{"ruby_gap_mm", 11}}), "ruby_gap_mm must be between -1 and 10"));
        QVERIFY(has(style(Json{{"ruby_gap_mm", "nan"}}), "ruby_gap_mm"));
        QVERIFY(has(style(Json{{"ruby_tracking", -0.6}}), "ruby_tracking must be between -0.5 and 2"));
        QVERIFY(has(style(Json{{"ruby_font", 3}}), "ruby_font"));
        // (a key that is not one is refused with Python's own words: its list is Python's)
        QVERIFY(has(style(Json{{"colour", 1}}), "unknown style key colour (one of font, size_mm,"));
        // the review state is the book's to set: a reading given back as needing review keeps its base
        const ApplyResult kept = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "ruby_spans": [{"start": 0, "end": 2,
            "ruby": "とうきょう", "base": "西京", "review": "edited"}]}])"));
        const Json line = line_of(payload_of(kept.doc, tmp_), "L1");
        QCOMPARE(shown(line["ruby_spans"]), shown(Json::parse(R"([{"start": 0, "end": 2, "base": "西京", "ruby": "とうきょう", "review": "edited"}])")));
        QCOMPARE(line["ruby_runs"], Json::array());
        QVERIFY(has(refusal(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "ruby_spans": [{"start": 0, "end": 2, "ruby": "x",
            "review": "maybe"}]}])")), "review"));
    }

    // The layout keys: set, read back, cleared with null; the feature only while the book uses it.
    void layoutKeysAndTheFeature() {
        Document doc = run_ops(blank_book(), Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", "東京へ"}}})).doc;
        QCOMPARE(payload_of(doc, tmp_)["features"], Json::array());
        doc = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "style": {"ruby_font": "mincho", "ruby_align": "tail",
            "ruby_gap_mm": 0.5, "ruby_tracking": 0.1, "ruby_scale": 0.6}, "ruby_spans": [{"start": 0, "end": 2, "ruby": "とうきょう"}]}])")).doc;
        Json payload = payload_of(doc, tmp_);
        QCOMPARE(payload["features"], Json::array({"ruby-layout@1"}));
        const Json& style = line_of(payload, "L1")["style"];
        QCOMPARE(style["ruby_font"], Json("mincho"));
        QCOMPARE(style["ruby_align"], Json("tail"));
        QCOMPARE(style["ruby_gap_mm"], Json(0.5));
        QCOMPARE(style["ruby_tracking"], Json(0.1));
        QCOMPARE(doc.features, std::vector<std::string>({"ruby-layout@1"}));
        // nothing of the feature left on the line: the line is Python's again (the book keeps the feature it declared)
        doc = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "ruby_spans": null, "style": {"ruby_font": null,
            "ruby_align": null, "ruby_gap_mm": null, "ruby_tracking": null}}])")).doc;
        payload = payload_of(doc, tmp_);
        QVERIFY(!line_of(payload, "L1").contains("ruby_spans"));
        QCOMPARE(line_of(payload, "L1")["ruby_runs"], Json::array());
    }

    // A book of Python's (book-v3: ruby_runs) is written as before: no ruby_spans, no feature; its ruby drawn as before
    // (test_contract_render). The feature is a known one: a book with it opens for writing.
    void pythonBooksStayAsTheyWere() {
        const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(genko::test::test_data("legacy/book-v3.genko").toStdString()));
        const Json payload = payload_of(loaded.document, tmp_);
        QCOMPARE(payload["features"], Json::array());
        bool runs = false;
        for (const Json& line : payload["story"]) {
            QVERIFY(!line.contains("ruby_spans"));
            runs = runs || !line["ruby_runs"].empty();
        }
        QVERIFY(runs);
        QVERIFY(genko::storage::is_known_feature("ruby-layout@1"));
    }

    // A ruby that needs review stops the official export (正式な書き出し) and is an error of the check and the studio's
    // preflight, which the official export runs; a book without one goes on to what this build does next.
    void reviewStopsTheOfficialExport() {
        const Document review = run_ops(blank_book(), Json::parse(R"([{"op": "add_line", "page": 1, "id": "L1", "text": "東新京へ",
            "x_mm": 20, "y_mm": 20, "ruby_spans": [{"start": 0, "end": 3, "ruby": "とうきょう", "base": "東京", "review": "edited"}]}])")).doc;
        const auto project = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/official.genko");
        const auto out = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/official-out");
        genko::formats::RunOptions official;
        official.official = true;
        const Json stopped = genko::formats::run(review, project, "pdf", out, official);
        QCOMPARE(stopped["ok"], Json(false));
        QCOMPARE(stopped["code"], Json("ruby_review"));
        QVERIFY2(stopped["error"].get<std::string>().find("1 ruby") != std::string::npos, stopped.dump().c_str());
        // (an ordinary export is not stopped: the reading needing review is not drawn)
        QCOMPARE(genko::formats::run(review, project, "png", out, {})["ok"], Json(true));
        const Document fine = run_ops(blank_book(), Json::parse(R"([{"op": "add_line", "page": 1, "id": "L1", "text": "東京へ",
            "x_mm": 20, "y_mm": 20, "ruby_spans": [{"start": 0, "end": 2, "ruby": "とうきょう"}]}])")).doc;
        QCOMPARE(genko::formats::run(fine, project, "pdf", out, official)["code"], Json("not_yet_ported"));
        // the check: an error with the line as its target; the preflight's error (a studio book)
        bool error = false;
        for (const Json& issue : genko::formats::checks::page_issues(review, review.page(0))) {
            if (issue["code"] == "ruby_review") error = issue["level"] == "error" && issue["target"]["id"] == "L1";
        }
        QVERIFY(error);
        Document studio = run_ops(review, Json::parse(R"([{"op": "set_meta", "strict_gates": true}])")).doc;
        const Json pre = genko::formats::checks::preflight(studio, project);
        bool stops = false;
        for (const Json& issue : pre["errors"]) stops = stops || issue["code"] == "ruby_review";
        QVERIFY(stops);
    }

    // Saved and read back: the spans where they were (review and all), the feature; a book whose spans do not fit its
    // text is reported and opens read-only (nothing is guessed or dropped).
    void savedAndReadBack() {
        Document doc = run_ops(blank_book(), Json::parse(R"([{"op": "add_line", "page": 1, "id": "L1", "text": "東京と東京",
            "ruby_spans": [{"start": 3, "end": 5, "ruby": "とうきょう"}, {"start": 0, "end": 2, "ruby": "とうけい", "base": "東京",
            "review": "ambiguous"}], "style": {"ruby_align": "head"}}])")).doc;
        const auto dir = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/book.genko");
        {
            genko::storage::ProjectLock lock(dir, "genko");
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = "genko";
            request.ops = Json::array();
            genko::storage::Saver(lock).save(doc, request);
        }
        const auto loaded = genko::storage::load_document(dir);
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        QVERIFY(loaded.document.read_only_reason.empty());
        const Json payload = payload_of(loaded.document, tmp_);
        QCOMPARE(payload["features"], Json::array({"ruby-layout@1"}));
        QCOMPARE(shown(line_of(payload, "L1")["ruby_spans"]),
                 shown(Json::parse(R"([{"start": 0, "end": 2, "base": "東京", "ruby": "とうけい", "review": "ambiguous"},
                                       {"start": 3, "end": 5, "base": "東京", "ruby": "とうきょう"}])")));
        QCOMPARE(line_of(payload, "L1")["ruby_runs"], Json::parse(R"([["東京", "とうきょう"]])"));
        // spans that do not fit the text (written by hand): reported, read-only
        Json raw = genko::test::read_json(QString::fromStdString(dir.string() + "/project.json"));
        raw["story"][0]["ruby_spans"][1]["end"] = 9;
        genko::test::write_bytes(QString::fromStdString(dir.string() + "/project.json"), genko::core::dump_python(raw));
        const auto broken = genko::storage::load_document(dir);
        QVERIFY(!broken.report.clean());
        QVERIFY(!broken.document.read_only_reason.empty());
    }
};

QTEST_GUILESS_MAIN(TestRuby)
#include "test_ruby.moc"
