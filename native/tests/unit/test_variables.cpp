// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」, core/variables.hpp): the book's variables and data sets as
// PhotoCraft v0.2.0's engine/src/variables_cmds/tests.rs tests them — a def's errors (an unknown layer, a type its target
// does not take, two of one name), a data set's (a variable the book has not, a value of another type), a text file of data
// sets (its quotes, delimiters, the column of names, a value of a type it does not fit), visibility and text applied in one
// step and undone — and Genko's own: the normal form read and written (and refused when it is not), the ops refused page by
// page, a picture given as bytes kept out of the journal, a picture's file read beside the text file.

#include <QtTest>
#include <QDir>
#include <QTemporaryDir>

#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/variables.hpp"
#include "formats/datasets.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_variables.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& ops) { return run_ops(doc, ops.is_array() ? ops : Json::array({ops})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.what();
    }
    return {};
}

std::string png_of(int w, int h, std::array<int, 4> rgba) {
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        for (int c = 0; c < 4; ++c) bytes[i + std::size_t(c)] = static_cast<char>(rgba[std::size_t(c)]);
    }
    return render::write_png(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
}

// A page 40 × 40 mm: a paint layer "photo", a paint layer "badge" (both holding a block), a type layer "title" ("Old") and a
// line ("台詞").
core::Document book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(40);
    spec.height_mm = core::Num(40);
    spec.dpi = core::Num(100);
    spec.expression = "color";
    core::Document doc = core::new_episode("vars", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    const auto block = [](const char* id, double x0, double w) {
        return Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", id}, {"name", id}},
                            Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", id},
                                 {"area", Json{{"poly", Json::array({Json::array({x0, x0}), Json::array({x0 + w, x0}), Json::array({x0 + w, x0 + w}),
                                                                     Json::array({x0, x0 + w})})}}},
                                 {"rgb", Json::array({128, 128, 128})}}});
    };
    doc = run(doc, block("photo", 5, 20));
    doc = run(doc, block("badge", 30, 5));
    doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "Old"}, {"point", Json::array({5, 35})}, {"new_id", "title"},
                        {"style", Json{{"size_mm", 3}}}});
    doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}, {"x_mm", 20}, {"y_mm", 20}});
    return doc;
}

std::string line_id(const core::Document& doc) { return doc.story.front().id; }

const core::Layer& layer(const core::Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return l;
    throw std::runtime_error("no layer " + id);
}

Json defs(const core::Document& doc) {
    return Json::array({Json{{"name", "showBadge"}, {"type", "visibility"}, {"page", 1}, {"layer", "badge"}},
                        Json{{"name", "headline"}, {"type", "text"}, {"page", 1}, {"layer", "title"}},
                        Json{{"name", "words"}, {"type", "text"}, {"line", line_id(doc)}}});
}

}  // namespace

class TestVariables : public QObject {
    Q_OBJECT

private slots:
    // The normal form: read and written the same; what is not as it must be refused (the book read-only).
    void normalForm() {
        const Json stored{{"defs", Json::array({Json{{"name", "a"}, {"type", "visibility"}, {"page", "p1"}, {"layer", "l1"}},
                                                Json{{"name", "b"}, {"type", "text"}, {"line", "x"}},
                                                Json{{"name", "c"}, {"type", "pixels"}, {"page", "p1"}, {"layer", "l2"}, {"method", "fill"},
                                                     {"align", "top_left"}, {"clip", true}, {"box_mm", Json::array({1, 2, 30.5, 40})}}})},
                          {"data_sets", Json::array({Json{{"name", "A"},
                                                          {"values", Json::array({Json{{"variable", "a"}, {"value", false}},
                                                                                  Json{{"variable", "b"}, {"value", "Hello"}},
                                                                                  Json{{"variable", "c"},
                                                                                       {"value", Json{{"asset", "sha256:" + std::string(64, 'a')}, {"suffix", ".png"}, {"name", "a.png"}}}}})}}})},
                          {"active", 0}};
        const core::Variables v = core::variables_from_json(stored);
        QCOMPARE(v.defs.size(), std::size_t(3));
        QCOMPARE(core::variables_to_json(v), stored);
        QVERIFY(core::value_fits("pixels", v.data_sets[0].values[2]));
        QVERIFY(!core::value_fits("text", v.data_sets[0].values[0]));
        const auto refused = [&](const std::function<void(Json&)>& change, const char* why) {
            Json bad = stored;
            change(bad);
            try {
                (void)core::variables_from_json(bad);
            } catch (const core::Error& error) {
                QVERIFY2(std::string(error.what()).find(why) != std::string::npos, error.what());
                return;
            }
            QFAIL(why);
        };
        refused([](Json& j) { j["defs"][1]["name"] = "a"; }, "two variables are named a");
        refused([](Json& j) { j["defs"][0]["type"] = "colour"; }, "type must be visibility, text or pixels");
        refused([](Json& j) { j["defs"][1]["type"] = "pixels"; }, "only a text variable does");
        refused([](Json& j) { j["defs"][2].erase("box_mm"); }, "must say its box_mm");
        refused([](Json& j) { j["defs"][0]["clip"] = true; }, "takes no clip");
        refused([](Json& j) { j["defs"][2]["box_mm"] = Json::array({5, 5, 5, 9}); }, "must be a box");
        refused([](Json& j) { j["defs"][2]["align"] = "middle"; }, "nine places");
        refused([](Json& j) { j["data_sets"][0]["values"][2]["value"]["suffix"] = ".exe"; }, "is not a picture's");
        refused([](Json& j) { j["data_sets"][0]["values"][2]["value"]["asset"] = "/tmp/a.png"; }, "must be an asset");
        refused([](Json& j) { j["data_sets"][0]["values"][1]["variable"] = "a"; }, "two values");
        refused([](Json& j) { j["active"] = 1; }, "active must be the index");
        refused([](Json& j) { j["extra"] = 1; }, "unknown key");
        // a book of none: no `variables`, no feature
        core::Document doc = book();
        QVERIFY(!core::uses_variables(doc));
        // (a state restored from its canonical JSON: its keys sorted — the same variables)
        doc.variables = Json::parse(core::dump_canonical(stored));
        core::validate_variables(doc);
        doc.variables->erase("active");
        doc.variables->operator[]("active") = 0;
        core::validate_variables(doc);
        QVERIFY(core::uses_variables(doc));
        core::set_variables(doc, core::Variables{});
        QVERIFY(!doc.variables.has_value() && !core::uses_variables(doc));
    }

    // set_variables: each def's target as its type takes it; the listing.
    void defineAndList() {
        core::Document doc = book();
        QVERIFY(refusal(doc, Json{{"op", "set_variables"}, {"defs", Json::array({Json{{"name", "x"}, {"type", "visibility"}, {"page", 1}, {"layer", "nope"}}})}})
                    .find("nope") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_variables"}, {"defs", Json::array({Json{{"name", "x"}, {"type", "text"}, {"page", 1}, {"layer", "photo"}}})}})
                    .find("is not a type layer") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_variables"}, {"defs", Json::array({Json{{"name", "x"}, {"type", "pixels"}, {"page", 1}, {"layer", "title"}}})}})
                    .find("not a paint layer or a smart object") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_variables"}, {"defs", Json::array({Json{{"name", "x"}, {"type", "visibility"}, {"line", line_id(doc)}}})}})
                    .find("a line takes a text variable only") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_variables"},
                                  {"defs", Json::array({Json{{"name", "x"}, {"type", "visibility"}, {"page", 1}, {"layer", "badge"}},
                                                        Json{{"name", "x"}, {"type", "visibility"}, {"page", 1}, {"layer", "photo"}}})}})
                    .find("two variables are named x") != std::string::npos);
        Json with_pixels = defs(doc);
        with_pixels.push_back(Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}, {"method", "conform"}});
        const core::ApplyResult done = run_ops(doc, Json::array({Json{{"op", "set_variables"}, {"defs", with_pixels}}}));
        doc = done.doc;
        QVERIFY(core::uses_variables(doc));
        const core::Variables v = core::variables_of(doc);
        QCOMPARE(v.defs.size(), std::size_t(4));
        QCOMPARE(v.defs[0].page, std::optional<std::string>(doc.page(0).id));
        // (a pixels def's box: the layer's content when it was defined — its block 5..25 mm —, kept when it is defined again)
        QVERIFY2(std::abs(v.defs[3].box_mm[0] - 5) < 0.2 && std::abs(v.defs[3].box_mm[2] - 25) < 0.2, core::variables_to_json(v).dump().c_str());
        const Json listed = render::variables_listing(doc);
        QCOMPARE(listed["defs"][0]["page"], Json(1));
        QCOMPARE(listed["defs"][0]["target"], Json("badge"));
        QCOMPARE(listed["defs"][2]["target"], Json("台詞"));
        QCOMPARE(listed["defs"][2]["missing"], Json(false));
        QCOMPARE(listed["active"], Json());
        // (the layer gone: the def kept, said missing)
        core::Document gone = run(doc, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "badge"}});
        QCOMPARE(render::variables_listing(gone)["defs"][0]["missing"], Json(true));
        QVERIFY(core::uses_variables(gone));
    }

    // set_data_sets and apply_data_set: visibility and text in one step, undone by the history (upstream's
    // define_apply_visibility_and_text); its refusals (bad_params_are_errors).
    void applyVisibilityAndText() {
        const core::Document base = book();
        core::Document doc = run(base, Json{{"op", "set_variables"}, {"defs", defs(base)}});
        QVERIFY(refusal(doc, Json{{"op", "set_data_sets"}, {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "nope"}, {"value", true}}})}}})}})
                    .find("no variable nope") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_data_sets"}, {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "showBadge"}, {"value", "yes"}}})}}})}})
                    .find("(visibility) takes true or false") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_data_sets"},
                                  {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array()}}, Json{{"name", "A"}, {"values", Json::array()}}})}})
                    .find("two data sets are named A") != std::string::npos);
        QVERIFY(refusal(book(), Json{{"op", "set_data_sets"}, {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array()}}})}})
                    .find("define them first") != std::string::npos);
        doc = run(doc, Json{{"op", "set_data_sets"},
                            {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "showBadge"}, {"value", false}},
                                                                                                    Json{{"variable", "headline"}, {"value", "Hello"}},
                                                                                                    Json{{"variable", "words"}, {"value", "こんにちは"}}})}},
                                                       Json{{"name", "B"}, {"values", Json::array({Json{{"variable", "headline"}, {"value", "Bye"}}})}}})}});
        QVERIFY(layer(doc, "badge").visible);
        QVERIFY(refusal(doc, Json{{"op", "apply_data_set"}, {"name", "missing"}}).find("no data set missing") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "apply_data_set"}, {"index", 5}}).find("no data set at index 5") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "apply_data_set"}}).find("name or index") != std::string::npos);
        const core::ApplyResult applied = run_ops(doc, Json::array({Json{{"op", "apply_data_set"}, {"name", "A"}}}));
        QCOMPARE(applied.results.size(), std::size_t(1));
        QCOMPARE(applied.results[0]["applied"], Json("A"));
        QCOMPARE(applied.results[0]["data_set"], Json(0));
        QCOMPARE(applied.results[0]["skipped"], Json::array());
        const core::Document& after = applied.doc;
        QVERIFY(!layer(after, "badge").visible);
        QCOMPARE(layer(after, "title").type->text, std::string("Hello"));
        QCOMPARE(after.story.front().text, std::string("こんにちは"));
        QCOMPARE(core::variables_of(after).active, std::optional<std::int64_t>(0));
        // (B gives headline only: the badge's visibility as A left it)
        const core::Document b = run(after, Json{{"op", "apply_data_set"}, {"index", 1}});
        QVERIFY(!layer(b, "badge").visible);
        QCOMPARE(layer(b, "title").type->text, std::string("Bye"));
        // (one op: the book before it is what an undo gives back — the journal records one op)
        QCOMPARE(applied.journal_ops.size(), std::size_t(1));
        QVERIFY(layer(doc, "badge").visible);
        // a value whose target is gone: skipped and said
        const core::Document gone = run(doc, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "badge"}});
        const core::ApplyResult partly = run_ops(gone, Json::array({Json{{"op", "apply_data_set"}, {"name", "A"}}}));
        QCOMPARE(partly.results[0]["skipped"], Json::array({Json{{"variable", "showBadge"}, {"reason", "missing"}}}));
        QCOMPARE(layer(partly.doc, "title").type->text, std::string("Hello"));
        // the ops are the book's: never page by page, nor inside a smart object's contents
        QVERIFY(refusal(doc, Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", "apply_data_set"}, {"index", 0}}})}})
                    .find("cannot be repeated page by page") != std::string::npos);
    }

    // A picture given as its bytes: kept by the book (smart_files until it is written), out of the journal.
    void pictureBytes() {
        core::Document doc = run(book(), Json{{"op", "set_variables"},
                                              {"defs", Json::array({Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}}})}});
        const std::string png = png_of(8, 4, {0, 0, 255, 255});
        QVERIFY(refusal(doc, Json{{"op", "set_data_sets"},
                                  {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "img"}, {"value", Json{{"data", core::b64encode("not a picture")}}}}})}}})}})
                    .find("cannot be read") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "set_data_sets"},
                                  {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "img"}, {"value", Json{{"path", "/tmp/a.png"}}}}})}}})}})
                    .find("a picture is {data: base64, name?} or {asset: sha256:…, name?}") != std::string::npos);  // (a path is no picture)
        const core::ApplyResult done = run_ops(doc, Json::array({Json{{"op", "set_data_sets"},
                                                                      {"data_sets", Json::array({Json{{"name", "A"},
                                                                                                      {"values", Json::array({Json{{"variable", "img"},
                                                                                                                                   {"value", Json{{"data", core::b64encode(png)}, {"name", "blue.png"}}}}})}}})}}}));
        const core::Variables v = core::variables_of(done.doc);
        const auto& picture = std::get<core::PictureValue>(v.data_sets[0].values[0].value);
        QCOMPARE(picture.suffix, std::string(".png"));
        QCOMPARE(picture.name, std::string("blue.png"));
        QVERIFY(done.doc.smart_files.contains(picture.asset));
        QCOMPARE(core::variable_pictures(done.doc).size(), std::size_t(1));
        const std::string journal = done.journal_ops.dump();
        QVERIFY2(journal.find("base64 chars") != std::string::npos && journal.find(core::b64encode(png)) == std::string::npos, journal.c_str());
        // (given again by its asset: the same picture)
        const core::Document again = run(done.doc, Json{{"op", "set_data_sets"}, {"append", true},
                                                        {"data_sets", Json::array({Json{{"name", "B"}, {"values", Json::array({Json{{"variable", "img"}, {"value", Json{{"asset", picture.asset}}}}})}}})}});
        QCOMPARE(core::variables_of(again).data_sets.size(), std::size_t(2));
        QCOMPARE(core::variable_pictures(again).size(), std::size_t(1));
    }

    // The text file's fields (upstream's split_csv, with quoted line breaks).
    void csvRows() {
        const auto rows = formats::csv_rows("a,\"b,c\",\"d \"\"e\"\"\"\r\n\r\n1,\"two\nlines\",3\n,,\n", ',');
        QCOMPARE(rows.size(), std::size_t(2));
        QCOMPARE(rows[0], (std::vector<std::string>{"a", "b,c", "d \"e\""}));
        QCOMPARE(rows[1], (std::vector<std::string>{"1", "two\nlines", "3"}));
        QCOMPARE(formats::csv_rows("x\ty\n1\t2", '\t').size(), std::size_t(2));
        try {
            formats::csv_rows("a,\"b\n", ',');
            QFAIL("an open quote");
        } catch (const core::Error& error) {
            QVERIFY(std::string(error.what()).find("not closed") != std::string::npos);
        }
    }

    // A text file of data sets (upstream's csv_import_and_apply): its names' column, values of each type, a column of no
    // variable left out (said), a picture read beside it.
    void csvImport() {
        QTemporaryDir dir;
        const core::Document base = book();
        Json all = defs(base);
        all.push_back(Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}});
        core::Document doc = run(base, Json{{"op", "set_variables"}, {"defs", all}});
        const core::Variables v = core::variables_of(doc);
        genko::test::write_bytes(dir.path() + QStringLiteral("/blue.png"), png_of(4, 4, {0, 0, 255, 255}));
        formats::CsvOptions options;
        options.folder = dir.path().toStdString();
        const formats::CsvRead read = formats::read_data_sets_csv(
            "\xEF\xBB\xBF" "DataSet,showBadge,headline,img,unknown\nrow-on,true,On Sale,blue.png,x\nrow-off,false,\"Sold, Out\",,y\n,yes,,,\n", v, options);
        QCOMPARE(read.data_sets.size(), std::size_t(3));
        QCOMPARE(read.data_sets[0]["name"], Json("row-on"));
        QCOMPARE(read.data_sets[2]["name"], Json("データセット 3"));
        QCOMPARE(read.data_sets[0]["values"][0], (Json{{"variable", "showBadge"}, {"value", true}}));
        QCOMPARE(read.data_sets[1]["values"][1], (Json{{"variable", "headline"}, {"value", "Sold, Out"}}));
        QCOMPARE(read.data_sets[1]["values"].size(), std::size_t(2));  // (an empty field: no value)
        QCOMPARE(read.data_sets[0]["values"][2]["value"]["name"], Json("blue.png"));
        QCOMPARE(read.notes.size(), std::size_t(1));
        QVERIFY(read.notes[0].find("unknown") != std::string::npos);
        // applied: the data sets the book's
        doc = run(doc, Json{{"op", "set_data_sets"}, {"data_sets", read.data_sets}});
        const core::Document off = run(doc, Json{{"op", "apply_data_set"}, {"name", "row-off"}});
        QVERIFY(!layer(off, "badge").visible);
        QCOMPARE(layer(off, "title").type->text, std::string("Sold, Out"));
        // no column of names: each line a data set named データセット N; a tab file found by its first line
        const formats::CsvRead tabs = formats::read_data_sets_csv("headline\tshowBadge\nA\t0\n", v, options);
        QCOMPARE(tabs.data_sets[0]["name"], Json("データセット 1"));
        QCOMPARE(tabs.data_sets[0]["values"][1]["value"], Json(false));
        // refused: a picture that is not there, two data sets of one name, a file that is not UTF-8
        const auto refused = [&](std::string_view text, const char* why) {
            try {
                formats::read_data_sets_csv(text, v, options);
            } catch (const core::Error& error) {
                QVERIFY2(std::string(error.what()).find(why) != std::string::npos, error.what());
                return;
            }
            QFAIL(why);
        };
        refused("DataSet,img\nA,gone.png\n", "cannot be read");
        refused("DataSet,headline\nA,1\nA,2\n", "two data sets are named A");
        refused("headline\n\x93\xFA\x96\x7B\n", "not UTF-8");
        refused("", "no line of names");
    }

    // The review's P1-2: a picture many rows name read once and given once (the others its asset ref); another file of the
    // same bytes given by its ref; the pictures together past the most refused before the next file is read.
    void csvPicturesOnce() {
        QTemporaryDir dir;
        const core::Document base = book();
        core::Document doc = run(base, Json{{"op", "set_variables"},
                                            {"defs", Json::array({Json{{"name", "img"}, {"type", "pixels"}, {"page", 1}, {"layer", "photo"}}})}});
        const core::Variables v = core::variables_of(doc);
        const std::string blue = png_of(4, 4, {0, 0, 255, 255});
        genko::test::write_bytes(dir.path() + QStringLiteral("/blue.png"), blue);
        genko::test::write_bytes(dir.path() + QStringLiteral("/same.png"), blue);
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
        std::string text = "DataSet,img\n";
        for (int i = 0; i < 1000; ++i) text += "r" + std::to_string(i) + "," + (i % 2 == 0 ? "blue.png" : "sub/../blue.png") + "\n";
        text += "last,same.png\n";
        formats::CsvOptions options;
        options.folder = dir.path().toStdString();
        const formats::CsvRead read = formats::read_data_sets_csv(text, v, options);
        QCOMPARE(read.data_sets.size(), std::size_t(1001));
        QCOMPARE(read.files_read, std::size_t(2));  // (blue.png once, whichever way it is named; same.png once)
        std::size_t given = 0;
        const std::string ref = core::bytes_ref(blue);
        for (const Json& set : read.data_sets) {
            const Json& value = set["values"][0]["value"];
            if (value.contains("data")) ++given;
            else QCOMPARE(value["asset"], Json(ref));
        }
        QCOMPARE(given, std::size_t(1));
        QCOMPARE(read.data_sets[1000]["values"][0]["value"]["name"], Json("same.png"));
        // the op takes them: the bytes given once, the refs naming them
        doc = run(doc, Json{{"op", "set_data_sets"}, {"data_sets", read.data_sets}});
        QCOMPARE(core::variables_of(doc).data_sets.size(), std::size_t(1001));
        QCOMPARE(std::get<core::PictureValue>(core::variables_of(doc).data_sets[999].values[0].value).asset, ref);
        // past the most: refused before the next file is read
        genko::test::write_bytes(dir.path() + QStringLiteral("/red.png"), png_of(4, 4, {255, 0, 0, 255}));
        options.pictures_most = blue.size() + 10;
        try {
            formats::read_data_sets_csv("DataSet,img\nA,blue.png\nB,blue.png\nC,red.png\n", v, options);
            QFAIL("not refused");
        } catch (const core::Error& error) {
            QVERIFY2(std::string(error.what()).find("larger than 4 GiB together") != std::string::npos, error.what());
        }
    }
};

QTEST_GUILESS_MAIN(TestVariables)
#include "test_variables.moc"
